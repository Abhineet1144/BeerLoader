#define _GNU_SOURCE
/* BD11 part 1: COM plumbing, resources, views, shaders and state objects, and the ID3D11Device
 * creation methods. The immediate/deferred context lives in bd11_ctx.c. */

#include "bd11.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_b11_trace;
long g_b11_trace_frame = -1;

#define IIDB(...) { __VA_ARGS__ }
typedef struct { uint8_t b[16]; } Guid;
#define GUID(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) \
    { { (a) & 0xff, ((a) >> 8) & 0xff, ((a) >> 16) & 0xff, ((a) >> 24) & 0xff, \
        (b) & 0xff, ((b) >> 8) & 0xff, (c) & 0xff, ((c) >> 8) & 0xff, \
        d0, d1, d2, d3, d4, d5, d6, d7 } }
static const Guid k_unknown = GUID(0u, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46);

/* Every D3D11 interface BD11 objects answer to (IID -> allowed kinds, kind = ~0 for any). The
 * lookup is deliberately lenient: asking a texture for ID3D11Resource or ID3D11Texture2D works,
 * asking for something that is not in this table is refused. */
typedef struct { Guid guid; uint32_t kinds; } IidRule;
#define K(k) (1u << (k))
#define ANY_CHILD (K(B11_BUFFER) | K(B11_TEXTURE) | K(B11_SRV) | K(B11_UAV) | K(B11_RTV) | K(B11_DSV) | \
                   K(B11_SHADER) | K(B11_LAYOUT) | K(B11_BLEND) | K(B11_DEPTH) | K(B11_RASTER) | \
                   K(B11_SAMPLER) | K(B11_QUERY) | K(B11_CMDLIST) | K(B11_LINKAGE))
static const IidRule g_iids[] = {
    { GUID(0x1841e5c8u, 0x16b0, 0x489b, 0xbc, 0xc8, 0x44, 0xcf, 0xb0, 0xd5, 0xde, 0xae), ANY_CHILD },
    { GUID(0xdc8e63f3u, 0xd12b, 0x4952, 0xb4, 0x7b, 0x5e, 0x45, 0x02, 0x6a, 0x86, 0x2d), K(B11_BUFFER) | K(B11_TEXTURE) },
    { GUID(0x48570b85u, 0xd1ee, 0x4fcd, 0xa2, 0x50, 0xeb, 0x35, 0x07, 0x22, 0xb0, 0x37), K(B11_BUFFER) },
    { GUID(0xf8fb5c27u, 0xc6b3, 0x4f75, 0xa4, 0xc8, 0x43, 0x9a, 0xf2, 0xef, 0x56, 0x4c), K(B11_TEXTURE) },
    { GUID(0x6f15aaf2u, 0xd208, 0x4e89, 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c), K(B11_TEXTURE) },
    { GUID(0x037e866eu, 0xf56d, 0x4357, 0xa8, 0xaf, 0x9d, 0xab, 0xbe, 0x6e, 0x25, 0x0e), K(B11_TEXTURE) },
    { GUID(0x036af924u, 0xa695, 0x4d29, 0xb9, 0x44, 0xa5, 0x05, 0x9b, 0xc4, 0x17, 0x9d), K(B11_TEXTURE) },
    { GUID(0x839d1216u, 0xbb2e, 0x412b, 0xb7, 0xf4, 0xa9, 0xdb, 0xeb, 0xe0, 0x8e, 0xd1), K(B11_SRV) | K(B11_UAV) | K(B11_RTV) | K(B11_DSV) },
    { GUID(0xb0e06fe0u, 0x8192, 0x4e1a, 0xb1, 0xca, 0x36, 0xd7, 0x41, 0x47, 0x10, 0xb2), K(B11_SRV) },
    { GUID(0x28acf509u, 0x7f5c, 0x48f6, 0x86, 0x11, 0xf3, 0x16, 0x01, 0x0a, 0x63, 0x80), K(B11_UAV) },
    { GUID(0xdfdba067u, 0x0b8d, 0x4865, 0x87, 0x5b, 0xd7, 0xb4, 0x51, 0x6c, 0xc1, 0x64), K(B11_RTV) },
    { GUID(0x9fdac92au, 0x1876, 0x48c3, 0xaf, 0xad, 0x25, 0xb9, 0x4f, 0x84, 0xa9, 0xb6), K(B11_DSV) },
    { GUID(0x3b301d64u, 0xd678, 0x4289, 0x88, 0x97, 0x22, 0xf8, 0x92, 0x8b, 0x72, 0xf3), K(B11_SHADER) },
    { GUID(0xea82e40du, 0x51dc, 0x4f33, 0x93, 0xd4, 0xdb, 0x7c, 0x91, 0x25, 0xae, 0x8c), K(B11_SHADER) },
    { GUID(0x38325b96u, 0xeffb, 0x4022, 0xba, 0x02, 0x2e, 0x79, 0x5b, 0x70, 0x27, 0x5c), K(B11_SHADER) },
    { GUID(0x8e5c6061u, 0x628a, 0x4c8e, 0x82, 0x64, 0xbb, 0xe4, 0x5c, 0xb3, 0xd5, 0xdd), K(B11_SHADER) },
    { GUID(0xf582c508u, 0x0f36, 0x490c, 0x99, 0x77, 0x31, 0xee, 0xce, 0x26, 0x8c, 0xfa), K(B11_SHADER) },
    { GUID(0x4f5b196eu, 0xc2bd, 0x495e, 0xbd, 0x01, 0x1f, 0xde, 0xd3, 0x8e, 0x49, 0x69), K(B11_SHADER) },
    { GUID(0xe4819ddcu, 0x4cf0, 0x4025, 0xbd, 0x26, 0x5d, 0xe8, 0x2a, 0x3e, 0x07, 0xb7), K(B11_LAYOUT) },
    { GUID(0x75b68faau, 0x347d, 0x4159, 0x8f, 0x45, 0xa0, 0x64, 0x0f, 0x01, 0xcd, 0x9a), K(B11_BLEND) },
    { GUID(0x03823efbu, 0x8d8f, 0x4e1c, 0x9a, 0xa2, 0xf6, 0x4b, 0xb2, 0xcb, 0xfd, 0xf1), K(B11_DEPTH) },
    { GUID(0x03823efbu, 0x8d8f, 0x4e1c, 0x9a, 0xa2, 0xf6, 0x4b, 0xb2, 0xcb, 0xfd, 0xdf), K(B11_DEPTH) },
    { GUID(0x9bb4ab81u, 0xab1a, 0x4d8f, 0xb5, 0x06, 0xfc, 0x04, 0x20, 0x0b, 0x6e, 0xe7), K(B11_RASTER) },
    { GUID(0xda6fea51u, 0x564c, 0x4487, 0x98, 0x10, 0xf0, 0xd0, 0xf9, 0xb4, 0xe3, 0xa5), K(B11_SAMPLER) },
    { GUID(0xda9b6e77u, 0xe045, 0x499a, 0xa4, 0xea, 0xcd, 0xe3, 0x8f, 0xe5, 0x9d, 0x9f), K(B11_SAMPLER) },
    { GUID(0x4b35d0cdu, 0x1e15, 0x4258, 0x9c, 0x98, 0x1b, 0x13, 0x33, 0xf6, 0xdd, 0x3b), K(B11_QUERY) },
    { GUID(0xd6c00747u, 0x87b7, 0x425e, 0xb8, 0x4d, 0x44, 0xd1, 0x08, 0x56, 0x0a, 0xfd), K(B11_QUERY) },
    { GUID(0xd60b6f7bu, 0x3488, 0x4923, 0x85, 0x10, 0xea, 0x5a, 0xd0, 0x00, 0x58, 0x9f), K(B11_QUERY) },
    { GUID(0x9eb576ddu, 0x9f77, 0x4d86, 0x81, 0xaa, 0x8b, 0xab, 0x5f, 0xe4, 0x90, 0xe2), K(B11_QUERY) },
    { GUID(0x9e33f628u, 0xd1d7, 0x42b2, 0xab, 0xb8, 0xd1, 0xb2, 0xf3, 0x10, 0x7b, 0x44), K(B11_QUERY) },
    { GUID(0xa24bc4d1u, 0x769e, 0x43f7, 0x80, 0x13, 0x98, 0xff, 0x56, 0x6c, 0x18, 0xe2), K(B11_CMDLIST) },
    { GUID(0xdb6f6ddbu, 0xac77, 0x4e88, 0x82, 0x53, 0x81, 0x9d, 0xf9, 0xbb, 0xf1, 0x40), K(B11_DEVICE) },
    { GUID(0xc0bfa96cu, 0xe089, 0x44fb, 0x8e, 0xaf, 0x26, 0xf8, 0x79, 0x61, 0x90, 0xda), K(B11_CONTEXT) },
    { GUID(0xaec22fb8u, 0x76f3, 0x4639, 0x9b, 0xe0, 0x28, 0xeb, 0x43, 0xa6, 0x7a, 0x2e), K(B11_DEVICE) | K(B11_SWAPCHAIN) },
    { GUID(0x54ec77fau, 0x1377, 0x44e6, 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c), K(B11_DEVICE) },
    { GUID(0x77db970fu, 0x6276, 0x48ba, 0xba, 0x28, 0x07, 0x01, 0x43, 0xb4, 0x39, 0x2c), K(B11_DEVICE) },
    { GUID(0x310d36a0u, 0xd2e4, 0x4a88, 0x8d, 0xf4, 0x1f, 0x64, 0x28, 0x61, 0x5b, 0x08), K(B11_SWAPCHAIN) },
    { GUID(0x31bf3a85u, 0xd2a4, 0x4b30, 0xa5, 0x5f, 0x4d, 0x30, 0x3a, 0xc0, 0x3a, 0x2f), K(B11_SWAPCHAIN) },
};

B11HR b11_query_interface(B11Obj *obj, const uint8_t *riid, void **out)
{
    if (!out) return B11_E_INVALIDARG;
    *out = NULL;
    if (!riid || !obj) return B11_E_NOINTERFACE;
    int ok = memcmp(riid, k_unknown.b, 16) == 0;
    for (size_t i = 0; !ok && i < sizeof(g_iids) / sizeof(*g_iids); ++i)
        if (memcmp(riid, g_iids[i].guid.b, 16) == 0 && (g_iids[i].kinds & K(obj->kind)))
            ok = 1;
    if (!ok) {
        if (getenv("BEER_COM_DIAGNOSTICS"))
            fprintf(stderr, "[BD11 QI] kind=%u refused %02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
                    obj->kind, riid[3], riid[2], riid[1], riid[0], riid[5], riid[4], riid[7], riid[6],
                    riid[8], riid[9], riid[10], riid[11], riid[12], riid[13], riid[14], riid[15]);
        return B11_E_NOINTERFACE;
    }
    b11_add_ref(obj);
    *out = obj;
    return B11_S_OK;
}

uint64_t b11_hash(const void *data, size_t size)
{
    const uint8_t *p = data;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

void *b11_obj_new(B11Device *dev, uint32_t kind, size_t size, void **vtable,
                  void (*destroy)(B11Obj *))
{
    B11Obj *obj = calloc(1, size);
    if (!obj) return NULL;
    obj->vtable = vtable;
    atomic_init(&obj->refs, 1);
    obj->kind = kind;
    obj->dev = dev;
    obj->destroy = destroy;
    return obj;
}

uint32_t B11_MS b11_add_ref(B11Obj *obj)
{
    return obj ? atomic_fetch_add(&obj->refs, 1) + 1 : 0;
}

uint32_t B11_MS b11_release(B11Obj *obj)
{
    if (!obj) return 0;
    uint32_t refs = atomic_fetch_sub(&obj->refs, 1) - 1;
    if (!refs && obj->destroy) obj->destroy(obj);
    return refs;
}

void b11_retain(B11Obj *obj)
{
    if (obj) atomic_fetch_add(&obj->refs, 1);
}

/* Deferred destruction ------------------------------------------------------------------ */

void b11_defer(B11Device *dev, void (*fn)(B11Device *, void *), void *ptr)
{
    B11Grave *g = calloc(1, sizeof(*g));
    if (!g) { fn(dev, ptr); return; }
    pthread_mutex_lock(&dev->grave_lock);
    g->serial = dev->serial + 1;   /* work recorded into the open list retires with the next submit */
    g->fn = fn;
    g->ptr = ptr;
    if (dev->grave_tail) dev->grave_tail->next = g; else dev->grave_head = g;
    dev->grave_tail = g;
    pthread_mutex_unlock(&dev->grave_lock);
}

void b11_reap(B11Device *dev)
{
    const uint64_t done = bd12_fence_completed(dev->retire);
    for (;;) {
        pthread_mutex_lock(&dev->grave_lock);
        B11Grave *g = dev->grave_head;
        if (!g || g->serial > done) { pthread_mutex_unlock(&dev->grave_lock); return; }
        dev->grave_head = g->next;
        if (!dev->grave_head) dev->grave_tail = NULL;
        pthread_mutex_unlock(&dev->grave_lock);
        g->fn(dev, g->ptr);
        free(g);
    }
}

void b11_free_buffer_fn(B11Device *dev, void *p) { (void)dev; bd12_buffer_destroy(p); }
#define free_buffer_fn b11_free_buffer_fn
static void free_image_fn(B11Device *dev, void *p) { (void)dev; bd12_image_destroy(p); }
static void free_heap_fn(B11Device *dev, void *p) { (void)dev; bd12_descriptor_heap_destroy(p); }

/* Generic ID3D11DeviceChild slots ---------------------------------------------------------- */

static B11HR B11_MS child_qi(B11Obj *self, const uint8_t *riid, void **out) { return b11_query_interface(self, riid, out); }
static uint32_t B11_MS child_addref(B11Obj *self) { return b11_add_ref(self); }
static uint32_t B11_MS child_release(B11Obj *self) { return b11_release(self); }
static void B11_MS child_get_device(B11Obj *self, void **device)
{
    if (!device) return;
    *device = self->dev;
    b11_add_ref(&self->dev->obj);
}
static B11HR B11_MS child_get_private(B11Obj *self, const uint8_t *guid, uint32_t *size, void *data)
{ (void)self; (void)guid; (void)data; if (size) *size = 0; return B11_S_OK; }
static B11HR B11_MS child_set_private(B11Obj *self, const uint8_t *guid, uint32_t size, const void *data)
{ (void)self; (void)guid; (void)size; (void)data; return B11_S_OK; }
static B11HR B11_MS child_set_private_iface(B11Obj *self, const uint8_t *guid, void *iface)
{ (void)self; (void)guid; (void)iface; return B11_S_OK; }

#define CHILD_SLOTS(vt) do { \
    (vt)[0] = (void *)child_qi; (vt)[1] = (void *)child_addref; (vt)[2] = (void *)child_release; \
    (vt)[3] = (void *)child_get_device; (vt)[4] = (void *)child_get_private; \
    (vt)[5] = (void *)child_set_private; (vt)[6] = (void *)child_set_private_iface; } while (0)

/* Formats --------------------------------------------------------------------------------- */

VkFormat b11_vk_format(uint32_t dxgi) { return bd12_dxgi_to_vk(dxgi); }

static uint32_t full_mip_count(uint32_t w, uint32_t h, uint32_t d)
{
    uint32_t m = w > h ? w : h;
    if (d > m) m = d;
    uint32_t n = 1;
    while (m > 1) { m >>= 1; ++n; }
    return n;
}

typedef struct { const void *mem; uint32_t pitch, slice; } B11SubData;

/* Buffers -------------------------------------------------------------------------------- */

static void buffer_destroy(B11Obj *o)
{
    B11Buffer *b = (B11Buffer *)o;
    for (uint32_t i = 0; i < b->backing_count; ++i) {
        if (!b->backing[i]) continue;
        if (b->renamed) b11_slice_retire(o->dev, b->backing[i]);
        else b11_defer(o->dev, free_buffer_fn, b->backing[i]);
    }
    free(b->backing);
    free(b->backing_serial);
    free(b->shadow);
    free(b);
}

static void *vt_buffer[11];
static void B11_MS buffer_get_type(B11Obj *self, uint32_t *type) { (void)self; if (type) *type = 1; }
static void B11_MS resource_set_priority(B11Obj *self, uint32_t p) { (void)self; (void)p; }
static uint32_t B11_MS resource_get_priority(B11Obj *self) { (void)self; return 0; }
static void B11_MS buffer_get_desc(B11Obj *self, B11BufferDesc *desc)
{
    if (desc) *desc = ((B11Buffer *)self)->desc;
}

static int hostbuf_debug(void)
{
    static int v = -1;
    if (v < 0) v = getenv("BEER_BD11_HOSTBUF") != NULL;
    return v;
}

static BD12Buffer *buffer_new_backing(B11Buffer *b)
{
    BD12HeapType type = hostbuf_debug() ? BD12_HEAP_UPLOAD : BD12_HEAP_DEFAULT;
    if (b->desc.usage == B11_USAGE_DYNAMIC) type = BD12_HEAP_UPLOAD;
    else if (b->desc.usage == B11_USAGE_STAGING)
        type = (b->desc.cpu & B11_CPU_READ) ? BD12_HEAP_READBACK : BD12_HEAP_UPLOAD;
    VkDeviceSize size = (b->desc.byte_width + 255u) & ~255u;
    if (!size) size = 256;
    return bd12_buffer_create_committed(b->obj.dev->bd, type, size);
}

BD12Buffer *b11_buffer_current(B11Buffer *b)
{
    return b->backing_count ? b->backing[b->current] : NULL;
}

/* Returns the backing the next write goes to. discard: take a fresh (or idle) backing so work
 * already in flight keeps reading the old contents. */
void b11_buffer_rename(B11Buffer *b, BD12Buffer *slice)
{
    B11Device *dev = b->obj.dev;
    if (b->renamed) {
        if (b->backing[0] != slice) b11_slice_retire(dev, b->backing[0]);
    } else {
        for (uint32_t i = 0; i < b->backing_count; ++i)
            if (b->backing[i]) b11_defer(dev, free_buffer_fn, b->backing[i]);
        b->renamed = 1;
    }
    b->backing[0] = slice;
    b->backing_serial[0] = 0;
    b->backing_count = 1;
    b->current = 0;
}

BD12Buffer *b11_buffer_write_backing(B11Buffer *b, int discard)
{
    B11Device *dev = b->obj.dev;
    if (!discard || b->desc.usage != B11_USAGE_DYNAMIC) return b11_buffer_current(b);
    if (b->renamed) {                               /* versions are upload slices: take a fresh one */
        BD12Buffer *slice = b11_slice_get(dev, b->desc.byte_width);
        if (slice) { b11_buffer_rename(b, slice); return slice; }
    }
    const uint64_t done = bd12_fence_completed(dev->retire);
    for (uint32_t i = 1; i <= b->backing_count; ++i) {
        const uint32_t idx = (b->current + i) % b->backing_count;
        if (b->backing_serial[idx] <= done) {
            b->current = idx;
            b->backing_serial[idx] = 0;
            return b->backing[idx];
        }
    }
    if (b->backing_count < 64) {
        if (b->backing_count == b->backing_capacity) {
            const uint32_t cap = b->backing_capacity ? b->backing_capacity * 2 : 4;
            BD12Buffer **nb = realloc(b->backing, cap * sizeof(*nb));
            uint64_t *ns = realloc(b->backing_serial, cap * sizeof(*ns));
            if (!nb || !ns) return b11_buffer_current(b);
            b->backing = nb; b->backing_serial = ns; b->backing_capacity = cap;
        }
        BD12Buffer *nbk = buffer_new_backing(b);
        if (nbk) {
            b->backing[b->backing_count] = nbk;
            b->backing_serial[b->backing_count] = 0;
            b->current = b->backing_count++;
            return nbk;
        }
    }
    /* Everything is in flight and the cap is reached: wait for the oldest. */
    b11_flush(dev, 0);
    uint32_t oldest = b->current;
    for (uint32_t i = 0; i < b->backing_count; ++i)
        if (b->backing_serial[i] < b->backing_serial[oldest]) oldest = i;
    b11_wait_serial(dev, b->backing_serial[oldest]);
    b->current = oldest;
    b->backing_serial[oldest] = 0;
    return b->backing[oldest];
}

static B11HR B11_MS dev_create_buffer(B11Device *dev, const B11BufferDesc *desc, const B11SubData *init_data, void **out)
{
    const uint8_t *init = init_data ? init_data->mem : NULL;
    if (!desc || !out) return B11_E_INVALIDARG;
    *out = NULL;
    if (!desc->byte_width) return B11_E_INVALIDARG;
    B11Buffer *b = b11_obj_new(dev, B11_BUFFER, sizeof(*b), vt_buffer, buffer_destroy);
    if (!b) return B11_E_OUTOFMEMORY;
    b->desc = *desc;
    if (getenv("BEER_BD11_MAPLOG") && desc->byte_width > 100000) {
        fprintf(stderr, "[BD11 NEWBUF] %p bytes=%u usage=%u bind=%u init=%p:", (void *)b, desc->byte_width, desc->usage, desc->bind, (const void *)init);
        for (int k = 0; init && k < 16; ++k) { uint32_t w; memcpy(&w, init + 1104 + k * 4, 4); fprintf(stderr, " %08x", w); }
        fprintf(stderr, "\n");
    }
    b->host_visible = desc->usage == B11_USAGE_DYNAMIC || desc->usage == B11_USAGE_STAGING || hostbuf_debug();
    b->backing = calloc(4, sizeof(*b->backing));
    b->backing_serial = calloc(4, sizeof(*b->backing_serial));
    b->backing_capacity = 4;
    if (!b->backing || !b->backing_serial) { buffer_destroy(&b->obj); return B11_E_OUTOFMEMORY; }
    BD12Buffer *main = buffer_new_backing(b);
    if (!main) { buffer_destroy(&b->obj); return B11_E_OUTOFMEMORY; }
    b->backing[0] = main;
    b->backing_count = 1;
    const VkDeviceSize alloc = (desc->byte_width + 255u) & ~255u;
    if (init) {
        if (b->host_visible && main->cpu) {
            memcpy(main->cpu, init, desc->byte_width);
        } else {
            BD12Buffer *stage = bd12_buffer_create_committed(dev->bd, BD12_HEAP_UPLOAD, alloc);
            if (!stage || !stage->cpu) { buffer_destroy(&b->obj); return B11_E_OUTOFMEMORY; }
            memcpy(stage->cpu, init, desc->byte_width);
            pthread_mutex_lock(&dev->lock);
            BD12CmdList *cmd = b11_cmd(dev);
            bd12_cmd_global_barrier(cmd);
            bd12_cmd_copy_buffer(cmd, main, 0, stage, 0, desc->byte_width);
            bd12_cmd_global_barrier(cmd);
            pthread_mutex_unlock(&dev->lock);
            b11_defer(dev, free_buffer_fn, stage);
        }
    }
    *out = b;
    return B11_S_OK;
}

/* Textures ------------------------------------------------------------------------------- */

static void texture_destroy(B11Obj *o)
{
    B11Texture *t = (B11Texture *)o;
    if (t->image) b11_defer(o->dev, free_image_fn, t->image);
    if (t->staging) b11_defer(o->dev, free_buffer_fn, t->staging);
    free(t->sub_offset); free(t->sub_pitch); free(t->sub_slice);
    free(t);
}

static void *vt_texture[11];
static void B11_MS texture_get_type(B11Obj *self, uint32_t *type)
{
    if (type) *type = ((B11Texture *)self)->desc.dimension + 1;   /* 1D=2, 2D=3, 3D=4 */
}
static void B11_MS texture_get_desc(B11Obj *self, uint32_t *desc)
{
    if (!desc) return;
    const B11TextureDesc *d = &((B11Texture *)self)->desc;
    switch (d->dimension) {
    case 1: desc[0] = d->width; desc[1] = d->mips; desc[2] = d->array_size; desc[3] = d->format;
            desc[4] = d->usage; desc[5] = d->bind; desc[6] = d->cpu; desc[7] = d->misc; break;
    case 2: desc[0] = d->width; desc[1] = d->height; desc[2] = d->mips; desc[3] = d->array_size;
            desc[4] = d->format; desc[5] = d->samples; desc[6] = d->quality; desc[7] = d->usage;
            desc[8] = d->bind; desc[9] = d->cpu; desc[10] = d->misc; break;
    default: desc[0] = d->width; desc[1] = d->height; desc[2] = d->depth; desc[3] = d->mips;
             desc[4] = d->format; desc[5] = d->usage; desc[6] = d->bind; desc[7] = d->cpu;
             desc[8] = d->misc; break;
    }
}

BD12Image *b11_texture_image(B11Texture *tex) { return tex->image; }

static void texture_layout_staging(B11Texture *t)
{
    const VkFormat f = b11_vk_format(t->desc.format);
    uint32_t bw = 1, bh = 1, bytes = bd12_dxgi_bytes(t->desc.format);
    if (f != VK_FORMAT_UNDEFINED) bd12_format_block(f, VK_IMAGE_ASPECT_COLOR_BIT, &bw, &bh, &bytes);
    if (!bytes) bytes = 4;
    const uint32_t layers = t->desc.dimension == 3 ? 1 : t->desc.array_size;
    t->subresources = t->desc.mips * layers;
    t->sub_offset = calloc(t->subresources, sizeof(uint64_t));
    t->sub_pitch = calloc(t->subresources, sizeof(uint32_t));
    t->sub_slice = calloc(t->subresources, sizeof(uint32_t));
    uint64_t at = 0;
    for (uint32_t layer = 0; layer < layers; ++layer)
        for (uint32_t mip = 0; mip < t->desc.mips; ++mip) {
            uint32_t w = t->desc.width >> mip, h = t->desc.height >> mip, d = t->desc.depth >> mip;
            if (!w) w = 1; if (!h) h = 1; if (!d) d = 1;
            const uint32_t blocks_w = (w + bw - 1) / bw, blocks_h = (h + bh - 1) / bh;
            const uint32_t idx = mip + layer * t->desc.mips;
            t->sub_offset[idx] = at;
            t->sub_pitch[idx] = blocks_w * bytes;
            t->sub_slice[idx] = blocks_w * bytes * blocks_h;
            at += (uint64_t)t->sub_slice[idx] * d;
            at = (at + 255u) & ~255ull;
        }
    t->last_use_serial = at;     /* reused as the total staging size until first use */
}


/* Geometry of one subresource: tight (block-row) pitch and slice size, texel extent. */
static void sub_geometry(const B11Texture *t, uint32_t mip, uint32_t *w, uint32_t *h, uint32_t *d,
                         uint32_t *row_bytes, uint32_t *rows)
{
    const VkFormat f = b11_vk_format(t->desc.format);
    uint32_t bw = 1, bh = 1, bytes = bd12_dxgi_bytes(t->desc.format);
    if (f != VK_FORMAT_UNDEFINED) bd12_format_block(f, VK_IMAGE_ASPECT_COLOR_BIT, &bw, &bh, &bytes);
    if (!bytes) bytes = 4;
    uint32_t ww = t->desc.width >> mip, hh = t->desc.height >> mip, dd = t->desc.depth >> mip;
    if (!ww) ww = 1; if (!hh) hh = 1; if (!dd) dd = 1;
    if (t->desc.dimension == 1) hh = 1;
    if (t->desc.dimension != 3) dd = 1;
    *w = ww; *h = hh; *d = dd;
    *row_bytes = ((ww + bw - 1) / bw) * bytes;
    *rows = (hh + bh - 1) / bh;
}

static void texture_upload_init(B11Device *dev, B11Texture *t, const B11SubData *init)
{
    const uint32_t layers = t->desc.dimension == 3 ? 1 : t->desc.array_size;
    uint64_t total = 0;
    for (uint32_t layer = 0; layer < layers; ++layer)
        for (uint32_t mip = 0; mip < t->desc.mips; ++mip) {
            uint32_t w, h, dp, rb, rows;
            sub_geometry(t, mip, &w, &h, &dp, &rb, &rows);
            total += (((uint64_t)rb * rows * dp) + 255u) & ~255ull;
        }
    BD12Buffer *stage = bd12_buffer_create_committed(dev->bd, BD12_HEAP_UPLOAD, total ? total : 256);
    if (!stage || !stage->cpu) return;
    pthread_mutex_lock(&dev->lock);
    BD12CmdList *cmd = b11_cmd(dev);
    bd12_cmd_transition_image(cmd, t->image, BD12_ALL_SUBRESOURCES, t->state, BD12_STATE_COPY_DEST);
    uint64_t at = 0;
    for (uint32_t layer = 0; layer < layers; ++layer)
        for (uint32_t mip = 0; mip < t->desc.mips; ++mip) {
            uint32_t w, h, dp, rb, rows;
            sub_geometry(t, mip, &w, &h, &dp, &rb, &rows);
            const uint32_t idx = mip + layer * t->desc.mips;
            const uint64_t size = (uint64_t)rb * rows * dp;
            if (getenv("BEER_BD11_TEXLOG") && mip == 0) {
                size_t nz = 0;
                const uint8_t *m = init[idx].mem;
                for (size_t b = 0; m && b < 4096 && b < size; ++b) nz += m[b] != 0;
                fprintf(stderr, "[BD11 TEX] init %ux%u fmt=%u mips=%u layer=%u data=%s nonzero-in-first-4K=%zu\n", w, h, t->desc.format, t->desc.mips, layer, m ? "yes" : "NO", nz);
            }
            if (init[idx].mem) {
                const uint8_t *src = init[idx].mem;
                for (uint32_t z = 0; z < dp; ++z)
                    for (uint32_t r = 0; r < rows; ++r)
                        memcpy((uint8_t *)stage->cpu + at + ((uint64_t)z * rows + r) * rb,
                               src + (uint64_t)z * init[idx].slice + (uint64_t)r * init[idx].pitch, rb);
                bd12_cmd_copy_buffer_to_image(cmd, t->image, (BD12ImagePoint){ idx, 0, 0, 0 },
                                              stage, at, rb, w, h, dp);
            }
            at += (size + 255u) & ~255ull;
        }
    bd12_cmd_transition_image(cmd, t->image, BD12_ALL_SUBRESOURCES, BD12_STATE_COPY_DEST, BD12_STATE_COMMON);
    pthread_mutex_unlock(&dev->lock);
    if (getenv("BEER_BD11_TEXVERIFY")) {       /* read mip 0 back from the GPU image and compare with what the game gave us */
        static unsigned checked;
        if (checked++ < 60 && init[0].mem) {
            uint32_t w, h, dp, rb, rows;
            sub_geometry(t, 0, &w, &h, &dp, &rb, &rows);
            const uint64_t size = (uint64_t)rb * rows * dp;
            BD12Buffer *back = bd12_buffer_create_committed(dev->bd, BD12_HEAP_READBACK, size ? size : 256);
            if (back && back->cpu) {
                pthread_mutex_lock(&dev->lock);
                BD12CmdList *cmd2 = b11_cmd(dev);
                bd12_cmd_transition_image(cmd2, t->image, BD12_ALL_SUBRESOURCES, BD12_STATE_COMMON, BD12_STATE_COPY_SOURCE);
                bd12_cmd_copy_image_to_buffer(cmd2, back, 0, rb, t->image, (BD12ImagePoint){ 0, 0, 0, 0 }, w, h, dp);
                bd12_cmd_transition_image(cmd2, t->image, BD12_ALL_SUBRESOURCES, BD12_STATE_COPY_SOURCE, BD12_STATE_COMMON);
                pthread_mutex_unlock(&dev->lock);
                b11_flush(dev, 1);
                uint64_t mismatch = 0, nonzero_back = 0;
                for (uint32_t r = 0; r < rows * dp; ++r)
                    for (uint32_t x = 0; x < rb; ++x) {
                        const uint8_t a = ((const uint8_t *)init[0].mem)[(uint64_t)r * init[0].pitch + x], b = ((const uint8_t *)back->cpu)[(uint64_t)r * rb + x];
                        mismatch += a != b; nonzero_back += b != 0;
                    }
                fprintf(stderr, "[BD11 TEXVERIFY] %ux%u fmt=%u: %llu of %llu bytes differ after upload (GPU copy has %llu non-zero bytes)\n", w, h, t->desc.format,
                        (unsigned long long)mismatch, (unsigned long long)size, (unsigned long long)nonzero_back);
            }
            if (back) bd12_buffer_destroy(back);
        }
    }
    b11_defer(dev, free_buffer_fn, stage);
}

B11HR b11_texture_create(B11Device *dev, const B11TextureDesc *in, const uint8_t *init, void **out);
static B11HR create_texture(B11Device *dev, const B11TextureDesc *in, const uint8_t *init, void **out)
{ return b11_texture_create(dev, in, init, out); }
B11HR b11_texture_create(B11Device *dev, const B11TextureDesc *in, const uint8_t *init, void **out)
{
    if (!in || !out) return B11_E_INVALIDARG;
    *out = NULL;
    B11Texture *t = b11_obj_new(dev, B11_TEXTURE, sizeof(*t), vt_texture, texture_destroy);
    if (!t) return B11_E_OUTOFMEMORY;
    t->desc = *in;
    B11TextureDesc *d = &t->desc;
    if (!d->height) d->height = 1;
    if (!d->depth) d->depth = 1;
    if (!d->array_size) d->array_size = 1;
    if (!d->samples) d->samples = 1;
    if (!d->mips) d->mips = full_mip_count(d->width, d->dimension == 1 ? 1 : d->height, d->dimension == 3 ? d->depth : 1);
    if (d->usage == B11_USAGE_STAGING) {
        texture_layout_staging(t);
        const uint64_t total = t->last_use_serial;
        t->last_use_serial = 0;
        t->staging = bd12_buffer_create_committed(dev->bd, (d->cpu & B11_CPU_READ) ? BD12_HEAP_READBACK : BD12_HEAP_UPLOAD,
                                                  total ? total : 256);
        if (!t->staging) { texture_destroy(&t->obj); return B11_E_OUTOFMEMORY; }
        *out = t;
        return B11_S_OK;
    }
    BD12ImageDesc id = {
        .type = d->dimension == 1 ? VK_IMAGE_TYPE_1D : d->dimension == 3 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
        .format = b11_vk_format(d->format),
        .width = d->width, .height = d->dimension == 1 ? 1 : d->height,
        .depth_or_layers = d->dimension == 3 ? d->depth : d->array_size,
        .mip_levels = d->mips, .samples = d->samples,
        .allow_render_target = (d->bind & B11_BIND_RTV) != 0,
        .allow_depth_stencil = (d->bind & B11_BIND_DSV) != 0,
        .allow_uav = (d->bind & B11_BIND_UAV) != 0,
        .cube_compatible = (d->misc & B11_MISC_CUBE) != 0
    };
    if (d->bind & B11_BIND_DSV) id.format = bd12_dxgi_resource_to_vk(d->format, 1);
    else id.format = bd12_dxgi_resource_to_vk(d->format, 0);
    /* A depth copy that is only ever sampled (typeless depth-stencil without a DSV bind) holds just the depth plane. */
    if (!(d->bind & B11_BIND_DSV) && d->format == 19 && !getenv("BEER_BD11_KEEP_DS_COPY")) id.format = VK_FORMAT_D32_SFLOAT;
    if (id.format == VK_FORMAT_UNDEFINED) {
        fprintf(stderr, "[BD11] unsupported texture format %u\n", d->format);
        texture_destroy(&t->obj);
        return B11_E_INVALIDARG;
    }
    t->image = bd12_image_create_committed(dev->bd, &id, BD12_STATE_COMMON);
    if (!t->image) { texture_destroy(&t->obj); return B11_E_OUTOFMEMORY; }
    t->state = BD12_STATE_COMMON;
    if (init) texture_upload_init(dev, t, (const B11SubData *)init);
    *out = t;
    return B11_S_OK;
}

static B11HR B11_MS dev_create_texture1d(B11Device *dev, const uint32_t *d, const uint8_t *init, void **out)
{
    if (!d) return B11_E_INVALIDARG;
    B11TextureDesc t = { .dimension = 1, .width = d[0], .mips = d[1], .array_size = d[2], .format = d[3],
                         .usage = d[4], .bind = d[5], .cpu = d[6], .misc = d[7] };
    return create_texture(dev, &t, init, out);
}
static B11HR B11_MS dev_create_texture2d(B11Device *dev, const uint32_t *d, const uint8_t *init, void **out)
{
    if (!d) return B11_E_INVALIDARG;
    B11TextureDesc t = { .dimension = 2, .width = d[0], .height = d[1], .mips = d[2], .array_size = d[3],
                         .format = d[4], .samples = d[5], .quality = d[6], .usage = d[7], .bind = d[8],
                         .cpu = d[9], .misc = d[10] };
    return create_texture(dev, &t, init, out);
}
static B11HR B11_MS dev_create_texture3d(B11Device *dev, const uint32_t *d, const uint8_t *init, void **out)
{
    if (!d) return B11_E_INVALIDARG;
    B11TextureDesc t = { .dimension = 3, .width = d[0], .height = d[1], .depth = d[2], .mips = d[3],
                         .format = d[4], .usage = d[5], .bind = d[6], .cpu = d[7], .misc = d[8] };
    return create_texture(dev, &t, init, out);
}

/* Views ---------------------------------------------------------------------------------- */

static void *vt_view[9];

static void view_destroy(B11Obj *o)
{
    B11View *v = (B11View *)o;
    if (v->heap) b11_defer(o->dev, free_heap_fn, v->heap);
    if (v->resource) b11_release(v->resource);
    free(v);
}
static void B11_MS view_get_resource(B11Obj *self, void **res)
{
    B11View *v = (B11View *)self;
    if (!res) return;
    *res = v->resource;
    b11_retain(v->resource);
}
static void B11_MS view_get_desc(B11Obj *self, uint32_t *desc)
{
    B11View *v = (B11View *)self;
    if (desc) memcpy(desc, v->desc_bytes, sizeof(v->desc_bytes));
}

enum { VIEW_DESC_NONE = 0 };

static VkImageViewType srv_view_type(uint32_t dim, uint32_t *layers_scale)
{
    *layers_scale = 1;
    switch (dim) {
    case 2: return VK_IMAGE_VIEW_TYPE_1D;
    case 3: return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    case 4: case 6: return VK_IMAGE_VIEW_TYPE_2D;
    case 5: case 7: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case 8: return VK_IMAGE_VIEW_TYPE_3D;
    case 9: *layers_scale = 6; return VK_IMAGE_VIEW_TYPE_CUBE;
    case 10: *layers_scale = 6; return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    default: return VK_IMAGE_VIEW_TYPE_2D;
    }
}

static uint32_t default_dim(const B11Texture *t, int srv)
{
    if (t->desc.dimension == 1) return t->desc.array_size > 1 ? 3 : 2;
    if (t->desc.dimension == 3) return 8;
    if (srv && (t->desc.misc & B11_MISC_CUBE)) return t->desc.array_size > 6 ? 10 : 9;
    if (t->desc.samples > 1) return t->desc.array_size > 1 ? 7 : 6;
    return t->desc.array_size > 1 ? 5 : 4;
}

static B11HR create_view(B11Device *dev, uint32_t kind, B11Obj *res, const uint32_t *d, void **out)
{
    if (!out || !res) return B11_E_INVALIDARG;
    *out = NULL;
    if (res->vtable != vt_buffer && res->vtable != vt_texture) {
        fprintf(stderr, "[BD11] CreateView(kind %u) on a foreign object %p (vtable %p)\n", kind, (void *)res, (void *)res->vtable);
        return B11_E_INVALIDARG;
    }
    B11View *v = b11_obj_new(dev, kind, sizeof(*v), vt_view, view_destroy);
    if (!v) return B11_E_OUTOFMEMORY;
    b11_retain(res);
    v->resource = res;
    if (d) memcpy(v->desc_bytes, d, 24);
    v->heap = bd12_descriptor_heap_create(dev->bd, 0, 1);
    if (!v->heap) { view_destroy(&v->obj); return B11_E_OUTOFMEMORY; }
    v->desc = &v->heap->descriptors[0];
    int ok = 0;
    if (res->kind == B11_BUFFER) {
        B11Buffer *b = (B11Buffer *)res;
        uint32_t flags = 0;
        uint32_t first = 0, num = 0xffffffffu;
        uint32_t format = 0;
        if (d) {
            format = d[0];
            first = d[2];
            num = d[3];
            if (kind == B11_SRV) flags = d[1] == 11 ? d[4] : 0;
            else if (kind == B11_UAV) flags = d[4];
        }
        uint32_t elem = 4;
        VkFormat vf = VK_FORMAT_R32_UINT;
        if (format && !(flags & 1)) {
            vf = b11_vk_format(format);
            elem = bd12_dxgi_bytes(format);
            if (vf == VK_FORMAT_UNDEFINED) { vf = VK_FORMAT_R32_UINT; elem = 4; }
        } else if (!(b->desc.misc & B11_MISC_RAW) && (b->desc.misc & B11_MISC_STRUCTURED) && b->desc.stride) {
            elem = b->desc.stride;          /* structured: addressed in bytes by the shader */
        }
        if (!elem) elem = 4;
        const VkDeviceSize total = (b->desc.byte_width + 255u) & ~255u;
        if (num == 0xffffffffu || (VkDeviceSize)(first + num) * elem > total)
            num = (uint32_t)(total / elem) - first;
        v->first = first; v->count = num; v->flags = flags; v->format = format;
        v->dimension = 1;
        BD12Buffer *bk = b11_buffer_current(b);
        if (!bk) { view_destroy(&v->obj); return B11_E_FAIL; }
        if (kind == B11_UAV)
            ok = bd12_descriptor_write_buffer_uav(v->heap, 0, bk, (VkDeviceSize)first * elem, (VkDeviceSize)num * elem, vf);
        else
            ok = bd12_descriptor_write_buffer_srv(v->heap, 0, bk, (VkDeviceSize)first * elem, (VkDeviceSize)num * elem, vf);
    } else {
        B11Texture *t = (B11Texture *)res;
        if (!t->image) { view_destroy(&v->obj); return B11_E_INVALIDARG; }
        uint32_t dim = d ? d[1] : default_dim(t, kind == B11_SRV);
        uint32_t format = d ? d[0] : 0;
        if (!format) format = t->desc.format;
        const VkFormat vf = bd12_dxgi_to_vk(format);
        uint32_t mip = 0, mips = 1, layer = 0, layers = 1;
        const uint32_t res_layers = t->desc.dimension == 3 ? 1 : t->desc.array_size;
        if (kind == B11_SRV) {
            uint32_t scale;
            const VkImageViewType vt = srv_view_type(dim, &scale);
            if (d) {
                mip = d[2]; mips = d[3];
                if (dim == 3 || dim == 5) { layer = d[4]; layers = d[5]; }
                else if (dim == 7) { layer = d[2]; layers = d[3]; mip = 0; mips = 1; }
                else if (dim == 10) { layer = d[4] * 1; layers = d[5] * 6; }
                else if (dim == 9) { layers = 6; }
                else if (dim == 6) { mips = 1; }
            } else {
                mips = 0xffffffffu;
                layers = dim == 9 ? 6 : (dim == 10 ? res_layers : res_layers);
            }
            if (mips == 0xffffffffu || mips == 0 || mip + mips > t->desc.mips) mips = t->desc.mips - mip;
            if (layers == 0xffffffffu || layer + layers > res_layers) layers = res_layers - layer;
            if (dim == 8) { layer = 0; layers = 1; }
            v->base_mip = mip; v->mip_count = mips; v->base_layer = layer; v->layer_count = layers;
            /* X32_TYPELESS_G8X24_UINT (22) and X24_TYPELESS_G8_UINT (47) read the stencil plane of a depth-stencil image. */
            if (format == 22 || format == 47)
                ok = bd12_descriptor_write_image_srv_aspect(v->heap, 0, t->image, vt, vf, mip, mips, layer, layers, VK_IMAGE_ASPECT_STENCIL_BIT);
            else
                ok = bd12_descriptor_write_image_srv(v->heap, 0, t->image, vt, vf, mip, mips, layer, layers);
        } else if (kind == B11_UAV) {
            VkImageViewType vt = dim == 2 ? VK_IMAGE_VIEW_TYPE_1D : dim == 3 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY :
                                 dim == 4 ? VK_IMAGE_VIEW_TYPE_2D : dim == 5 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY :
                                 VK_IMAGE_VIEW_TYPE_3D;
            if (d) {
                mip = d[2];
                if (dim == 3 || dim == 5) { layer = d[3]; layers = d[4]; }
                else if (dim == 8) { layer = d[3]; layers = d[4]; }
            } else layers = vt == VK_IMAGE_VIEW_TYPE_3D ? t->desc.depth : res_layers;
            if (layers == 0xffffffffu || layer + layers > (dim == 8 ? t->desc.depth >> mip : res_layers))
                layers = (dim == 8 ? (t->desc.depth >> mip) : res_layers) - layer;
            v->base_mip = mip; v->mip_count = 1; v->base_layer = layer; v->layer_count = layers;
            ok = bd12_descriptor_write_image_uav(v->heap, 0, t->image, vt, vf, mip, layer, layers);
        } else if (kind == B11_RTV) {
            if (d) {
                if (dim == 4 || dim == 2) mip = d[2];
                else if (dim == 3 || dim == 5 || dim == 8) { mip = d[2]; layer = d[3]; layers = d[4]; }
                else if (dim == 7) { layer = d[2]; layers = d[3]; }
            } else layers = res_layers;
            if (dim == 8 && d) { /* 3D render target: slices are layers of the 3D image */ }
            if (layers == 0xffffffffu || layer + layers > (t->desc.dimension == 3 ? (t->desc.depth >> mip ? t->desc.depth >> mip : 1) : res_layers))
                layers = (t->desc.dimension == 3 ? (t->desc.depth >> mip ? t->desc.depth >> mip : 1) : res_layers) - layer;
            v->base_mip = mip; v->mip_count = 1; v->base_layer = layer; v->layer_count = layers;
            ok = bd12_descriptor_write_rtv(v->heap, 0, t->image, vf, mip, layer, layers);
        } else {
            uint32_t flags = d ? d[2] : 0;
            if (d) {
                if (dim == 3 || dim == 1) mip = d[3];
                else if (dim == 2 || dim == 4) { mip = d[3]; layer = d[4]; layers = d[5]; }
                else if (dim == 6) { layer = d[3]; layers = d[4]; }
            } else layers = res_layers;
            if (layers == 0xffffffffu || layer + layers > res_layers) layers = res_layers - layer;
            v->base_mip = mip; v->mip_count = 1; v->base_layer = layer; v->layer_count = layers;
            v->flags = flags;
            ok = bd12_descriptor_write_dsv(v->heap, 0, t->image, vf, mip, layer, layers, (flags & 1) != 0, (flags & 2) != 0);
        }
        v->dimension = dim; v->format = format;
    }
    if (!ok) { view_destroy(&v->obj); fprintf(stderr, "[BD11] view creation failed (kind %u)\n", kind); return B11_E_FAIL; }
    *out = v;
    return B11_S_OK;
}

static B11HR B11_MS dev_create_srv(B11Device *dev, B11Obj *res, const uint32_t *d, void **out)
{ return create_view(dev, B11_SRV, res, d, out); }
static B11HR B11_MS dev_create_uav(B11Device *dev, B11Obj *res, const uint32_t *d, void **out)
{ return create_view(dev, B11_UAV, res, d, out); }
static B11HR B11_MS dev_create_rtv(B11Device *dev, B11Obj *res, const uint32_t *d, void **out)
{ return create_view(dev, B11_RTV, res, d, out); }
static B11HR B11_MS dev_create_dsv(B11Device *dev, B11Obj *res, const uint32_t *d, void **out)
{ return create_view(dev, B11_DSV, res, d, out); }

/* Shaders, input layouts, states ---------------------------------------------------------- */

static void *vt_child[7];

static void shader_destroy(B11Obj *o) { B11Shader *s = (B11Shader *)o; free(s->bytecode); free(s); }

static B11HR create_shader(B11Device *dev, const void *bytecode, size_t length, uint32_t stage, void **out)
{
    if (!out || !bytecode || length < 32) return B11_E_INVALIDARG;
    *out = NULL;
    B11Shader *s = b11_obj_new(dev, B11_SHADER, sizeof(*s), vt_child, shader_destroy);
    if (!s) return B11_E_OUTOFMEMORY;
    s->bytecode = malloc(length);
    if (!s->bytecode) { free(s); return B11_E_OUTOFMEMORY; }
    memcpy(s->bytecode, bytecode, length);
    s->size = length;
    s->stage = stage;
    s->hash = b11_hash(bytecode, length);
    const char *dump = getenv("BEER_BD11_DUMP_SHADERS");
    if (dump && *dump) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%016llx.s%u.dxbc", dump, (unsigned long long)s->hash, stage);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(bytecode, 1, length, f); fclose(f); }
    }
    *out = s;
    return B11_S_OK;
}
static B11HR B11_MS dev_create_vs(B11Device *d, const void *b, size_t n, void *l, void **o) { (void)l; return create_shader(d, b, n, B11_STAGE_VS, o); }
static B11HR B11_MS dev_create_gs(B11Device *d, const void *b, size_t n, void *l, void **o) { (void)l; return create_shader(d, b, n, B11_STAGE_GS, o); }
static B11HR B11_MS dev_create_ps(B11Device *d, const void *b, size_t n, void *l, void **o) { (void)l; return create_shader(d, b, n, B11_STAGE_PS, o); }
static B11HR B11_MS dev_create_hs(B11Device *d, const void *b, size_t n, void *l, void **o) { (void)l; return create_shader(d, b, n, B11_STAGE_HS, o); }
static B11HR B11_MS dev_create_ds(B11Device *d, const void *b, size_t n, void *l, void **o) { (void)l; return create_shader(d, b, n, B11_STAGE_DS, o); }
static B11HR B11_MS dev_create_cs(B11Device *d, const void *b, size_t n, void *l, void **o) { (void)l; return create_shader(d, b, n, B11_STAGE_CS, o); }
static B11HR B11_MS dev_create_gs_so(B11Device *d, const void *b, size_t n, const void *e, uint32_t ne,
                                     const uint32_t *st, uint32_t ns, uint32_t rast, void *l, void **o)
{
    (void)e; (void)ne; (void)st; (void)ns; (void)rast; (void)l;
    return create_shader(d, b, n, B11_STAGE_GS, o);        /* stream-out ignored: plain geometry shader */
}
static B11HR B11_MS dev_create_linkage(B11Device *dev, void **out)
{
    if (!out) return B11_E_INVALIDARG;
    B11Obj *o = b11_obj_new(dev, B11_LINKAGE, sizeof(*o), vt_child, (void (*)(B11Obj *))free);
    *out = o;
    return o ? B11_S_OK : B11_E_OUTOFMEMORY;
}

static void layout_destroy(B11Obj *o)
{
    B11Layout *l = (B11Layout *)o;
    free(l->elements); free(l->names); free(l);
}

static B11HR B11_MS dev_create_input_layout(B11Device *dev, const uint8_t *elements, uint32_t count,
                                            const void *bytecode, size_t length, void **out)
{
    (void)bytecode; (void)length;
    if (!out || !elements || !count || count > 32) return B11_E_INVALIDARG;
    *out = NULL;
    B11Layout *l = b11_obj_new(dev, B11_LAYOUT, sizeof(*l), vt_child, layout_destroy);
    if (!l) return B11_E_OUTOFMEMORY;
    l->count = count;
    l->elements = malloc((size_t)count * 32);
    l->names = calloc(count, sizeof(*l->names));
    if (!l->elements || !l->names) { layout_destroy(&l->obj); return B11_E_OUTOFMEMORY; }
    memcpy(l->elements, elements, (size_t)count * 32);
    for (uint32_t i = 0; i < count; ++i) {
        const char *name = NULL;
        memcpy(&name, elements + (size_t)i * 32, sizeof(name));
        if (name) snprintf(l->names[i], sizeof(l->names[i]), "%s", name);
        const char *copy = l->names[i];
        memcpy(l->elements + (size_t)i * 32, &copy, sizeof(copy));
    }
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < count; ++i) {
        h = (h ^ b11_hash(l->names[i], strlen(l->names[i]))) * 1099511628211ull;
        h = (h ^ b11_hash(elements + (size_t)i * 32 + 8, 24)) * 1099511628211ull;
    }
    l->hash = h;
    *out = l;
    return B11_S_OK;
}

static void state_destroy(B11Obj *o) { free(o); }
static void *vt_state[8];
static void B11_MS state_get_desc(B11Obj *self, void *desc)
{
    if (!desc) return;
    static const size_t sizes[] = { [B11_BLEND] = 264, [B11_DEPTH] = 52, [B11_RASTER] = 40, [B11_SAMPLER] = 52 };
    if (self->kind < sizeof(sizes) / sizeof(*sizes) && sizes[self->kind])
        memcpy(desc, ((B11State *)self)->desc, sizes[self->kind]);
}

static B11HR create_state(B11Device *dev, uint32_t kind, const void *desc, size_t size, void **out)
{
    if (!out || !desc) return B11_E_INVALIDARG;
    *out = NULL;
    B11State *s = b11_obj_new(dev, kind, sizeof(*s), vt_state, state_destroy);
    if (!s) return B11_E_OUTOFMEMORY;
    memcpy(s->desc, desc, size);
    s->hash = b11_hash(desc, size);
    if (kind == B11_SAMPLER) s->sampler = b11_sampler_info(s->desc);
    *out = s;
    return B11_S_OK;
}
static B11HR B11_MS dev_create_blend(B11Device *d, const void *desc, void **o) { return create_state(d, B11_BLEND, desc, 264, o); }
static B11HR B11_MS dev_create_depth(B11Device *d, const void *desc, void **o) { return create_state(d, B11_DEPTH, desc, 52, o); }
static B11HR B11_MS dev_create_raster(B11Device *d, const void *desc, void **o) { return create_state(d, B11_RASTER, desc, 40, o); }
static B11HR B11_MS dev_create_sampler(B11Device *d, const void *desc, void **o) { return create_state(d, B11_SAMPLER, desc, 52, o); }

static VkSamplerAddressMode address_mode(uint32_t m)
{
    switch (m) {
    case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 3: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 5: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

VkSamplerCreateInfo b11_sampler_info(const uint8_t *desc)
{
    uint32_t w[13];
    memcpy(w, desc, sizeof(w));
    float f[13];
    memcpy(f, desc, sizeof(f));
    const uint32_t filter = w[0];
    const int aniso = (filter & 0x40) != 0, compare = (filter & 0x180) == 0x80;
    VkSamplerCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = (filter & 0x4) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = (filter & 0x10) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = (filter & 0x1) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = address_mode(w[1]), .addressModeV = address_mode(w[2]), .addressModeW = address_mode(w[3]),
        .mipLodBias = f[4],
        .anisotropyEnable = aniso, .maxAnisotropy = aniso ? (float)(w[5] < 1 ? 1 : w[5] > 16 ? 16 : w[5]) : 1.0f,
        .compareEnable = compare,
        .compareOp = compare ? (VkCompareOp)((w[6] ? w[6] : 1) - 1) : VK_COMPARE_OP_ALWAYS,
        .minLod = f[11], .maxLod = f[12] > 3.0e38f ? VK_LOD_CLAMP_NONE : f[12]
    };
    const float r = f[7], g = f[8], b = f[9], a = f[10];
    info.borderColor = (r == 1.0f && g == 1.0f && b == 1.0f && a == 1.0f) ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE :
                       (r == 0.0f && g == 0.0f && b == 0.0f && a == 1.0f) ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK :
                       VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    return info;
}

/* Queries ---------------------------------------------------------------------------------- */

static void query_destroy(B11Obj *o) { free(o); }
static void *vt_query[9];
static uint32_t B11_MS query_get_data_size(B11Obj *self)
{
    switch (((B11Query *)self)->type) {
    case 0: case 1: return 8;          /* EVENT, OCCLUSION */
    case 2: return 8;                  /* TIMESTAMP */
    case 3: return 16;                 /* TIMESTAMP_DISJOINT */
    case 4: return 40;                 /* PIPELINE_STATISTICS */
    case 5: return 4;                  /* OCCLUSION_PREDICATE */
    default: return 8;
    }
}
static void B11_MS query_get_desc(B11Obj *self, uint32_t *desc)
{
    if (!desc) return;
    desc[0] = ((B11Query *)self)->type;
    desc[1] = ((B11Query *)self)->misc;
}
static B11HR create_query(B11Device *dev, const uint32_t *desc, void **out)
{
    if (!out || !desc) return B11_E_INVALIDARG;
    B11Query *q = b11_obj_new(dev, B11_QUERY, sizeof(*q), vt_query, query_destroy);
    if (!q) return B11_E_OUTOFMEMORY;
    q->type = desc[0];
    q->misc = desc[1];
    q->pool_index = ~0u;
    *out = q;
    return B11_S_OK;
}
static B11HR B11_MS dev_create_query(B11Device *d, const uint32_t *desc, void **o) { return create_query(d, desc, o); }

/* Device ----------------------------------------------------------------------------------- */

void *g_b11_device_vtable[43];

B11HR b11_dxgi_device_query(B11Device *dev, void **out);
static B11HR B11_MS dev_qi(B11Obj *self, const uint8_t *riid, void **out)
{
    static const Guid k_dxgi_device = GUID(0x54ec77fau, 0x1377, 0x44e6, 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c);
    static const Guid k_dxgi_device1 = GUID(0x77db970fu, 0x6276, 0x48ba, 0xba, 0x28, 0x07, 0x01, 0x43, 0xb4, 0x39, 0x2c);
    if (out && riid && (!memcmp(riid, k_dxgi_device.b, 16) || !memcmp(riid, k_dxgi_device1.b, 16)))
        return b11_dxgi_device_query((B11Device *)self, out);
    return b11_query_interface(self, riid, out);
}
static uint32_t B11_MS dev_addref(B11Obj *self) { return b11_add_ref(self); }
static uint32_t B11_MS dev_release(B11Obj *self) { return b11_release(self); }
static B11HR B11_MS dev_notimpl(void) { return B11_E_NOTIMPL; }
static B11HR B11_MS dev_create_deferred(B11Device *dev, uint32_t flags, void **out)
{
    if (!out) return B11_E_INVALIDARG;
    B11Context *c = b11_context_create(dev, 1, flags);
    *out = c;
    return c ? B11_S_OK : B11_E_OUTOFMEMORY;
}

static B11HR B11_MS dev_check_format_support(B11Device *dev, uint32_t format, uint32_t *support)
{
    (void)dev;
    if (!support) return B11_E_INVALIDARG;
    *support = 0;
    if (bd12_dxgi_to_vk(format) == VK_FORMAT_UNDEFINED && bd12_dxgi_resource_to_vk(format, 1) == VK_FORMAT_UNDEFINED)
        return B11_E_FAIL;
    uint32_t s = 0x1 | 0x2 | 0x10 | 0x20 | 0x40 | 0x80 | 0x100 | 0x200 | 0x400 | 0x1000 | 0x2000 |
                 0x4000 | 0x8000 | 0x40000 | 0x80000 | 0x100000 | 0x200000 | 0x400000 | 0x800000 | 0x2000000;
    if (format == 40 || format == 45 || format == 20 || format == 55 || format == 19 || format == 44 || format == 46)
        s |= 0x10000;                                   /* depth-stencil capable formats */
    *support = s;
    return B11_S_OK;
}
static B11HR B11_MS dev_check_ms_quality(B11Device *dev, uint32_t format, uint32_t samples, uint32_t *levels)
{
    (void)dev; (void)format;
    if (!levels) return B11_E_INVALIDARG;
    *levels = (samples == 1 || samples == 2 || samples == 4 || samples == 8) ? 1 : 0;
    return B11_S_OK;
}
static void B11_MS dev_check_counter_info(B11Device *dev, uint32_t *info) { (void)dev; if (info) memset(info, 0, 24); }
static B11HR B11_MS dev_check_feature_support(B11Device *dev, uint32_t feature, void *data, uint32_t size)
{
    (void)dev;
    if (!data) return B11_E_INVALIDARG;
    switch (feature) {
    case 0: if (size >= 8) { ((uint32_t *)data)[0] = 1; ((uint32_t *)data)[1] = 0; } return B11_S_OK;
    case 4: if (size >= 4) ((uint32_t *)data)[0] = 1; return B11_S_OK;
    case 2: {   /* FORMAT_SUPPORT: {format, support} */
        uint32_t *w = data;
        if (size < 8) return B11_E_INVALIDARG;
        return dev_check_format_support(dev, w[0], &w[1]);
    }
    default: memset(data, 0, size); return B11_S_OK;
    }
}
static uint32_t B11_MS dev_get_feature_level(B11Device *dev) { return dev->feature_level; }
static uint32_t B11_MS dev_get_creation_flags(B11Device *dev) { return dev->flags; }
static B11HR B11_MS dev_get_removed_reason(B11Device *dev) { (void)dev; return B11_S_OK; }
static void B11_MS dev_get_immediate(B11Device *dev, void **ctx)
{
    if (!ctx) return;
    *ctx = dev->immediate;
    b11_add_ref(&dev->immediate->obj);
}
static B11HR B11_MS dev_set_exception_mode(B11Device *dev, uint32_t flags) { (void)dev; (void)flags; return B11_S_OK; }
static uint32_t B11_MS dev_get_exception_mode(B11Device *dev) { (void)dev; return 0; }

void b11_init_vtables(void)
{
    static int done;
    if (done) return;
    done = 1;
    g_b11_trace = getenv("BEER_BD11_TRACE") != NULL || getenv("BEER_BD11_TRACE_FILE") != NULL;
    if (getenv("BEER_BD11_TRACE")) g_b11_trace_frame = strtol(getenv("BEER_BD11_TRACE"), NULL, 10);
    CHILD_SLOTS(vt_child);
    CHILD_SLOTS(vt_state);
    vt_state[7] = (void *)state_get_desc;
    CHILD_SLOTS(vt_buffer);
    vt_buffer[7] = (void *)buffer_get_type; vt_buffer[8] = (void *)resource_set_priority;
    vt_buffer[9] = (void *)resource_get_priority; vt_buffer[10] = (void *)buffer_get_desc;
    CHILD_SLOTS(vt_texture);
    vt_texture[7] = (void *)texture_get_type; vt_texture[8] = (void *)resource_set_priority;
    vt_texture[9] = (void *)resource_get_priority; vt_texture[10] = (void *)texture_get_desc;
    CHILD_SLOTS(vt_view);
    vt_view[7] = (void *)view_get_resource; vt_view[8] = (void *)view_get_desc;
    CHILD_SLOTS(vt_query);
    vt_query[7] = (void *)query_get_data_size; vt_query[8] = (void *)query_get_desc;

    void **d = g_b11_device_vtable;
    for (int i = 0; i < 43; ++i) d[i] = (void *)dev_notimpl;
    d[0] = (void *)dev_qi; d[1] = (void *)dev_addref; d[2] = (void *)dev_release;
    d[3] = (void *)dev_create_buffer; d[4] = (void *)dev_create_texture1d;
    d[5] = (void *)dev_create_texture2d; d[6] = (void *)dev_create_texture3d;
    d[7] = (void *)dev_create_srv; d[8] = (void *)dev_create_uav; d[9] = (void *)dev_create_rtv;
    d[10] = (void *)dev_create_dsv; d[11] = (void *)dev_create_input_layout;
    d[12] = (void *)dev_create_vs; d[13] = (void *)dev_create_gs; d[14] = (void *)dev_create_gs_so;
    d[15] = (void *)dev_create_ps; d[16] = (void *)dev_create_hs; d[17] = (void *)dev_create_ds;
    d[18] = (void *)dev_create_cs; d[19] = (void *)dev_create_linkage;
    d[20] = (void *)dev_create_blend; d[21] = (void *)dev_create_depth; d[22] = (void *)dev_create_raster;
    d[23] = (void *)dev_create_sampler; d[24] = (void *)dev_create_query; d[25] = (void *)dev_create_query;
    d[27] = (void *)dev_create_deferred;
    d[29] = (void *)dev_check_format_support; d[30] = (void *)dev_check_ms_quality;
    d[31] = (void *)dev_check_counter_info; d[33] = (void *)dev_check_feature_support;
    d[34] = (void *)child_get_private; d[35] = (void *)child_set_private; d[36] = (void *)child_set_private_iface;
    d[37] = (void *)dev_get_feature_level; d[38] = (void *)dev_get_creation_flags;
    d[39] = (void *)dev_get_removed_reason; d[40] = (void *)dev_get_immediate;
    d[41] = (void *)dev_set_exception_mode; d[42] = (void *)dev_get_exception_mode;
    b11_context_init_vtable();
}

