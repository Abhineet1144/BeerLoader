#define _GNU_SOURCE
/* BD11 part 2: device creation, command-list rotation and submission. */

#include "bd11.h"
#include <time.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void device_destroy(B11Obj *o)
{
    (void)o;   /* the device lives for the whole process */
}

B11Device *b11_device_create(void)
{
    b11_init_vtables();
    B11Device *dev = calloc(1, sizeof(*dev));
    if (!dev) return NULL;
    dev->bd = bd12_device_create();
    if (!dev->bd) { free(dev); return NULL; }
    dev->queue = bd12_queue_create(dev->bd);
    dev->retire = bd12_fence_create(dev->bd, 0);
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&dev->lock, &attr);
    pthread_mutex_init(&dev->grave_lock, NULL);
    for (int i = 0; i < B11_ALLOCATORS; ++i) {
        dev->allocators[i] = bd12_cmd_allocator_create(dev->bd);
        if (dev->allocators[i]) dev->lists[i] = bd12_cmd_list_create(dev->allocators[i]);
    }
    if (!dev->queue || !dev->retire || !dev->allocators[0] || !dev->lists[0]) {
        fprintf(stderr, "[BD11] device creation failed\n");
        free(dev);
        return NULL;
    }
    dev->obj.vtable = g_b11_device_vtable;
    atomic_init(&dev->obj.refs, 1);
    dev->obj.kind = B11_DEVICE;
    dev->obj.dev = dev;
    dev->obj.destroy = device_destroy;
    dev->feature_level = 0xb000;
    dev->null_buffer = bd12_buffer_create_committed(dev->bd, BD12_HEAP_UPLOAD, 65536);
    if (dev->null_buffer && dev->null_buffer->cpu) memset(dev->null_buffer->cpu, 0, 65536);
    dev->immediate = b11_context_create(dev, 0, 0);
    fprintf(stderr, "[BD11] device ready (%s)\n", dev->bd->device_name);
    return dev;
}

uint64_t g_b11_wait_ns;              /* time spent blocked on the GPU, reported by BEER_BD11_FPS */

int b11_wait_serial(B11Device *dev, uint64_t serial)
{
    if (!serial || serial <= bd12_fence_completed(dev->retire)) return 1;
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    const int ok = bd12_fence_wait(dev->retire, serial, 10ull * 1000000000ull);
    clock_gettime(CLOCK_MONOTONIC, &b);
    __atomic_fetch_add(&g_b11_wait_ns, (uint64_t)((b.tv_sec - a.tv_sec) * 1000000000ll + (b.tv_nsec - a.tv_nsec)),
                       __ATOMIC_RELAXED);
    return ok;
}

void b11_wait_gpu_use(B11Device *dev, uint64_t serial)
{
    if (!serial || serial <= bd12_fence_completed(dev->retire)) return;
    pthread_mutex_lock(&dev->lock);
    const int open = serial > dev->serial;
    pthread_mutex_unlock(&dev->lock);
    if (open) b11_flush(dev, 0);
    b11_wait_serial(dev, serial);
}

BD12CmdList *b11_cmd(B11Device *dev)
{
    pthread_mutex_lock(&dev->lock);
    if (!dev->recording) {
        const uint32_t idx = dev->allocator_current;
        if (dev->allocator_serial[idx]) b11_wait_serial(dev, dev->allocator_serial[idx]);
        bd12_cmd_allocator_reset(dev->allocators[idx]);
        dev->cmd = dev->lists[idx];
        bd12_cmd_list_begin(dev->cmd);
        dev->cmd->resolve_hook = b11_resolve_hook;
        dev->cmd->resolve_user = dev->immediate;
        dev->recording = 1;
        dev->cmd_generation++;
        b11_reap(dev);
    }
    BD12CmdList *cmd = dev->cmd;
    pthread_mutex_unlock(&dev->lock);
    return cmd;
}

void b11_flush(B11Device *dev, int wait)
{
    pthread_mutex_lock(&dev->lock);
    if (dev->recording) {
        BD12CmdList *cmd = dev->cmd;
        bd12_cmd_end_rendering(cmd);
        bd12_cmd_list_end(cmd);
        const uint64_t serial = dev->serial + 1;
        const BD12FenceOp signal = { dev->retire, serial };
        const VkResult r = bd12_queue_submit(dev->queue, &cmd, 1, NULL, 0, &signal, 1);
        if (r != VK_SUCCESS) fprintf(stderr, "[BD11] queue submit failed: %d\n", (int)r);
        dev->serial = serial;
        dev->allocator_serial[dev->allocator_current] = serial;
        dev->allocator_current = (dev->allocator_current + 1) % B11_ALLOCATORS;
        dev->recording = 0;
        dev->cmd = NULL;
    }
    const uint64_t wait_for = dev->serial;
    pthread_mutex_unlock(&dev->lock);
    if (wait) b11_wait_serial(dev, wait_for);
    b11_reap(dev);
}

/* Entry used by the loader's D3D11CreateDevice: one process-wide device and immediate context. */
B11HR b11_device_create_for_loader(void **device, void **context)
{
    static B11Device *dev;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    if (!dev) dev = b11_device_create();
    pthread_mutex_unlock(&lock);
    if (!dev || !dev->immediate) return B11_E_FAIL;
    if (device) { b11_add_ref(&dev->obj); *device = dev; }
    if (context) { b11_add_ref(&dev->immediate->obj); *context = dev->immediate; }
    return B11_S_OK;
}

/* Upload slices ------------------------------------------------------------------------------- */
#define SLICE_CLASSES 18                   /* 256 B .. 32 MiB */
#define SLICE_CHUNK (4u << 20)

typedef struct { BD12Buffer *slice; uint64_t serial; } SliceWait;

struct B11SlicePool {
    pthread_mutex_t lock;
    BD12Buffer **free_list[SLICE_CLASSES];
    uint32_t free_count[SLICE_CLASSES], free_capacity[SLICE_CLASSES];
    SliceWait *wait[SLICE_CLASSES];        /* FIFO in retire order */
    uint32_t wait_head[SLICE_CLASSES], wait_tail[SLICE_CLASSES], wait_capacity[SLICE_CLASSES];
    BD12Heap *chunk;
    VkDeviceSize chunk_used, chunk_size;
};

static int slice_class(size_t size)
{
    int c = 0;
    while (c < SLICE_CLASSES - 1 && ((size_t)256 << c) < size) ++c;
    return ((size_t)256 << c) >= size ? c : -1;
}

static int push_free(struct B11SlicePool *p, int c, BD12Buffer *slice)
{
    if (p->free_count[c] == p->free_capacity[c]) {
        const uint32_t cap = p->free_capacity[c] ? p->free_capacity[c] * 2 : 64;
        BD12Buffer **grown = realloc(p->free_list[c], cap * sizeof(*grown));
        if (!grown) return 0;
        p->free_list[c] = grown;
        p->free_capacity[c] = cap;
    }
    p->free_list[c][p->free_count[c]++] = slice;
    return 1;
}

BD12Buffer *b11_slice_get(B11Device *dev, size_t size)
{
    const int c = slice_class(size);
    if (c < 0) return NULL;
    struct B11SlicePool *p = dev->slices;
    if (!p) {
        pthread_mutex_lock(&dev->lock);
        if (!dev->slices && (p = calloc(1, sizeof(*p)))) {
            pthread_mutex_init(&p->lock, NULL);
            dev->slices = p;
        }
        p = dev->slices;
        pthread_mutex_unlock(&dev->lock);
        if (!p) return NULL;
    }
    const uint64_t done = bd12_fence_completed(dev->retire);
    pthread_mutex_lock(&p->lock);
    while (p->wait_head[c] != p->wait_tail[c]) {
        SliceWait *w = &p->wait[c][p->wait_head[c] % p->wait_capacity[c]];
        if (w->serial > done || !push_free(p, c, w->slice)) break;
        p->wait_head[c]++;
    }
    BD12Buffer *slice = p->free_count[c] ? p->free_list[c][--p->free_count[c]] : NULL;
    if (!slice) {
        const VkDeviceSize bytes = (VkDeviceSize)256 << c;
        if (!p->chunk || p->chunk_used + bytes > p->chunk_size) {
            p->chunk_size = bytes > SLICE_CHUNK ? bytes : SLICE_CHUNK;
            p->chunk = bd12_heap_create_unzeroed(dev->bd, BD12_HEAP_UPLOAD, p->chunk_size);
            p->chunk_used = 0;
        }
        if (p->chunk) {
            slice = bd12_buffer_create_placed(p->chunk, p->chunk_used, bytes);
            if (slice) p->chunk_used += bytes;       /* power-of-two sizes keep every slice aligned */
        }
    }
    pthread_mutex_unlock(&p->lock);
    return slice;
}

void b11_slice_retire(B11Device *dev, BD12Buffer *slice)
{
    struct B11SlicePool *p = dev->slices;
    const int c = slice ? slice_class(slice->size) : -1;
    if (!p || c < 0) return;
    pthread_mutex_lock(&dev->lock);
    const uint64_t serial = dev->serial + 1;           /* the open list may still use it */
    pthread_mutex_unlock(&dev->lock);
    pthread_mutex_lock(&p->lock);
    if (p->wait_tail[c] - p->wait_head[c] == p->wait_capacity[c]) {
        const uint32_t cap = p->wait_capacity[c] ? p->wait_capacity[c] * 2 : 256;
        SliceWait *grown = malloc(cap * sizeof(*grown));
        if (!grown) { pthread_mutex_unlock(&p->lock); return; }     /* leaks one slice; never reused */
        for (uint32_t i = 0; i < p->wait_tail[c] - p->wait_head[c]; ++i)
            grown[i] = p->wait[c][(p->wait_head[c] + i) % p->wait_capacity[c]];
        p->wait_tail[c] -= p->wait_head[c];
        p->wait_head[c] = 0;
        free(p->wait[c]);
        p->wait[c] = grown;
        p->wait_capacity[c] = cap;
    }
    p->wait[c][p->wait_tail[c]++ % p->wait_capacity[c]] = (SliceWait){ slice, serial };
    pthread_mutex_unlock(&p->lock);
}
