#define _GNU_SOURCE
/* BD11 part 2: device creation, command-list rotation and submission. */

#include "bd11.h"

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
    for (int i = 0; i < 3; ++i) {
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

int b11_wait_serial(B11Device *dev, uint64_t serial)
{
    if (!serial) return 1;
    return bd12_fence_wait(dev->retire, serial, 10ull * 1000000000ull);
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
        dev->allocator_current = (dev->allocator_current + 1) % 3;
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
