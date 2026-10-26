#define _GNU_SOURCE
#include "bd12.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Frame capture of compute-shader buffers (diagnostic).
 *
 * BEER_BD12_CAPTURE_DIR=<dir> and BEER_BD12_CAPTURE_CS=<hash,hash,...> select
 * shaders (hex prefixes of the DXBC hash). While a frame is traced (see
 * bd12_trace_active) every dispatch of a selected shader copies each bound
 * buffer range to host memory before and after it runs; at that frame's present
 * the copies are written to <dir> as cap_<n>_<hash>_<class><reg>_<pre|post>.bin
 * together with an index line in cap_index.txt. Ranges are clipped to 64 MiB. */

#define MAX_NOTES 64
#define MAX_CAPTURES 4096
#define CLIP (16ull << 20)

typedef struct {
    uint32_t cls;                 /* 0 CBV, 1 SRV, 2 UAV */
    uint32_t reg, space;
    VkBuffer buffer;
    VkDeviceSize offset, range;
} Note;

typedef struct {
    uint32_t reg, space, mip;
    BD12Image *image;
} ImageNote;
static _Thread_local ImageNote g_image_notes[8];
static _Thread_local uint32_t g_image_note_count;

typedef struct {
    BD12Buffer *readback;
    VkDeviceSize size;
    char name[160];
    int is_rt, is_depth;
    uint32_t rt_w, rt_h, rt_bpp;
    VkFormat rt_fmt;
} Capture;

/* Command lists are recorded on several threads: per-dispatch state is
 * thread-local, the shared capture list is locked. */
static _Thread_local int g_enabled;
static _Thread_local Note g_notes[MAX_NOTES];
static _Thread_local uint32_t g_note_count;
static _Thread_local uint32_t t_sequence;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static Capture g_caps[MAX_CAPTURES];
static uint32_t g_cap_count;
static uint32_t g_sequence;

static void record_images(BD12CmdList *list, uint64_t hash, uint32_t seq, int second);

int bd12_capture_enabled(void) { return g_enabled; }

int bd12_capture_wants(uint64_t hash)
{
    if (!bd12_trace_active()) return 0;
    const char *dir = getenv("BEER_BD12_CAPTURE_DIR");
    const char *list = getenv("BEER_BD12_CAPTURE_CS");
    if (!dir || !list) return 0;
    char text[17];
    snprintf(text, sizeof(text), "%016llx", (unsigned long long)hash);
    for (const char *p = list; *p; ) {
        size_t n = strcspn(p, ",");
        if (n && n <= 16 && strncmp(text, p, n) == 0) return 1;
        p += n + (p[n] == ',');
    }
    return 0;
}

void bd12_capture_begin(void)
{
    g_enabled = 1;
    g_note_count = 0;
    g_image_note_count = 0;
}

void bd12_capture_note_image(uint32_t reg, uint32_t space, BD12Image *image,
                             uint32_t mip)
{
    if (!g_enabled || !image || g_image_note_count == 8) return;
    g_image_notes[g_image_note_count++] = (ImageNote){ reg, space, mip, image };
}

void bd12_capture_note(uint32_t cls, uint32_t space, uint32_t reg, VkBuffer buffer,
                       VkDeviceSize offset, VkDeviceSize range)
{
    if (!g_enabled || !buffer || g_note_count == MAX_NOTES) return;
    g_notes[g_note_count++] = (Note){ cls, reg, space, buffer, offset, range };
}

void bd12_capture_record(BD12CmdList *list, int post, uint64_t hash,
                         uint32_t x, uint32_t y, uint32_t z)
{
    BD12Device *device = list->device;
    static const char cls_name[3] = { 'b', 't', 'u' };
    if (!post) t_sequence = __atomic_add_fetch(&g_sequence, 1, __ATOMIC_RELAXED);
    const int second = post == 2;
    post = post != 0;
    if (post) record_images(list, hash, t_sequence, second);
    const uint32_t seq = t_sequence;
    for (uint32_t i = 0; i < g_note_count; ++i) {
        const Note *n = &g_notes[i];
        if (post && n->cls != 2) continue;
        if (!post && n->cls == 2) continue;      /* outputs: post only */
        if (__atomic_load_n(&g_cap_count, __ATOMIC_RELAXED) == MAX_CAPTURES) break;
        VkDeviceSize size = n->range < CLIP ? n->range : CLIP;
        size &= ~(VkDeviceSize)3;
        if (!size) continue;
        BD12Buffer *rb = bd12_buffer_create_committed(device, BD12_HEAP_READBACK, size);
        if (!rb) continue;
        bd12_cmd_end_rendering(list);
        bd12_cmd_global_barrier(list);
        const VkBufferCopy region = { n->offset, 0, size };
        device->vk.CmdCopyBuffer(list->command_buffer, n->buffer, rb->buffer, 1, &region);
        bd12_cmd_global_barrier(list);
        pthread_mutex_lock(&g_lock);
        if (g_cap_count == MAX_CAPTURES) { pthread_mutex_unlock(&g_lock); bd12_buffer_destroy(rb); break; }
        Capture *c = &g_caps[g_cap_count++];
        pthread_mutex_unlock(&g_lock);
        c->readback = rb;
        c->size = size;
        snprintf(c->name, sizeof(c->name), "cap_%u_%016llx_%c%u_s%u_%s", seq,
                 (unsigned long long)hash, cls_name[n->cls], n->reg, n->space,
                 second ? "post2" : post ? "post" : "pre");
        fprintf(stderr, "[CAPTURE] %s groups=%u,%u,%u bytes=%llu (range %llu)\n",
                c->name, x, y, z, (unsigned long long)size,
                (unsigned long long)n->range);
    }
}

/* Storage-image outputs: copy the written mip level to host memory. */
static void record_images(BD12CmdList *list, uint64_t hash, uint32_t seq, int second)
{
    BD12Device *device = list->device;
    for (uint32_t i = 0; i < g_image_note_count; ++i) {
        const ImageNote *n = &g_image_notes[i];
        const BD12Image *im = n->image;
        uint32_t block = 0;
        bd12_format_block(im->desc.format, VK_IMAGE_ASPECT_COLOR_BIT, NULL, NULL, &block);
        const uint32_t w = im->desc.width >> n->mip ? im->desc.width >> n->mip : 1;
        const uint32_t h = im->desc.height >> n->mip ? im->desc.height >> n->mip : 1;
        const uint32_t d = im->desc.type == VK_IMAGE_TYPE_3D ?
            (im->desc.depth_or_layers >> n->mip ? im->desc.depth_or_layers >> n->mip : 1) :
            im->desc.depth_or_layers;
        const VkDeviceSize size = (VkDeviceSize)w * h * d * block;
        if (!block || !size || size > (64ull << 20)) continue;
        BD12Buffer *rb = bd12_buffer_create_committed(device, BD12_HEAP_READBACK, size);
        if (!rb) continue;
        bd12_cmd_end_rendering(list);
        bd12_cmd_global_barrier(list);
        VkBufferImageCopy region = {
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, n->mip, 0,
                                  im->desc.type == VK_IMAGE_TYPE_3D ? 1 : d },
            .imageExtent = { w, h, im->desc.type == VK_IMAGE_TYPE_3D ? d : 1 }
        };
        device->vk.CmdCopyImageToBuffer(list->command_buffer, im->image,
                                        VK_IMAGE_LAYOUT_GENERAL, rb->buffer, 1, &region);
        bd12_cmd_global_barrier(list);
        pthread_mutex_lock(&g_lock);
        if (g_cap_count < MAX_CAPTURES) {
            Capture *c = &g_caps[g_cap_count++];
            c->readback = rb;
            c->size = size;
            snprintf(c->name, sizeof(c->name), "cap_%u_%016llx_img%u_s%u_%ux%ux%u_b%u_%s", seq,
                     (unsigned long long)hash, n->reg, n->space, w, h, d, block,
                     second ? "post2" : "post");
            fprintf(stderr, "[CAPTURE] %s bytes=%llu\n", c->name, (unsigned long long)size);
            rb = NULL;
        }
        pthread_mutex_unlock(&g_lock);
        if (rb) bd12_buffer_destroy(rb);
    }
}

void bd12_capture_end(void) { g_enabled = 0; }

/* Indirect draws of the traced frame (BEER_BD12_CAPTURE_DRAWS=1): the GPU count
 * word and up to 128 argument records, taken right before the draw. */
void bd12_capture_indirect(BD12CmdList *list, uint64_t pso, uint32_t type,
                           BD12Buffer *args, VkDeviceSize arg_offset,
                           BD12Buffer *count, VkDeviceSize count_offset,
                           uint32_t max_count, uint32_t stride)
{
    if (!getenv("BEER_BD12_CAPTURE_DIR") || !getenv("BEER_BD12_CAPTURE_DRAWS") ||
        !bd12_trace_active() || !args || type > 1) return;
    BD12Device *device = list->device;
    static uint32_t ordinal;
    const uint32_t n = __atomic_add_fetch(&ordinal, 1, __ATOMIC_RELAXED);
    const uint32_t records = max_count < 128 ? max_count : 128;
    const VkDeviceSize arg_bytes = (VkDeviceSize)records * (stride ? stride : 20);
    BD12Buffer *rb_args = arg_bytes && arg_offset + arg_bytes <= args->size ?
        bd12_buffer_create_committed(device, BD12_HEAP_READBACK, arg_bytes) : NULL;
    BD12Buffer *rb_count = count && count_offset + 4 <= count->size ?
        bd12_buffer_create_committed(device, BD12_HEAP_READBACK, 4) : NULL;
    bd12_cmd_end_rendering(list);
    bd12_cmd_global_barrier(list);
    if (rb_args) {
        const VkBufferCopy r = { arg_offset, 0, arg_bytes };
        device->vk.CmdCopyBuffer(list->command_buffer, args->buffer, rb_args->buffer, 1, &r);
    }
    if (rb_count) {
        const VkBufferCopy r = { count_offset, 0, 4 };
        device->vk.CmdCopyBuffer(list->command_buffer, count->buffer, rb_count->buffer, 1, &r);
    }
    bd12_cmd_global_barrier(list);
    pthread_mutex_lock(&g_lock);
    for (int k = 0; k < 2; ++k) {
        BD12Buffer *rb = k ? rb_count : rb_args;
        if (!rb || g_cap_count == MAX_CAPTURES) { if (rb) bd12_buffer_destroy(rb); continue; }
        Capture *c = &g_caps[g_cap_count++];
        c->readback = rb;
        c->size = k ? 4 : arg_bytes;
        snprintf(c->name, sizeof(c->name), "ei_%u_pso%016llx_max%u_args%llx_%s", n,
                 (unsigned long long)pso, max_count,
                 (unsigned long long)(args->va + arg_offset), k ? "count" : "args");
    }
    pthread_mutex_unlock(&g_lock);
}

/* Readback destinations written during the traced frame (BEER_BD12_RB_DUMP=1):
 * their host contents are written to disk at the frame's present. */
typedef struct { BD12Buffer *dst; VkDeviceSize offset, size; uint64_t src_va; } RbRef;
static RbRef g_rb[64];
static uint32_t g_rb_count;
void bd12_capture_readback(BD12Buffer *dst, VkDeviceSize offset, VkDeviceSize size, uint64_t src_va)
{
    const char *mode = getenv("BEER_BD12_RB_DUMP");
    if (!mode || (strcmp(mode, "all") && !bd12_trace_active()) || !dst || !dst->cpu) return;
    pthread_mutex_lock(&g_lock);
    if (g_rb_count < 64) g_rb[g_rb_count++] = (RbRef){ dst, offset, size, src_va };
    pthread_mutex_unlock(&g_lock);
}
int bd12_capture_rb_pending(void) { return g_rb_count != 0; }
void bd12_capture_rb_flush(void)
{
    const char *dir = getenv("BEER_BD12_CAPTURE_DIR");
    const char *mode = getenv("BEER_BD12_RB_DUMP");
    if (mode && !strcmp(mode, "all")) {
        static unsigned frame;
        ++frame;
        for (uint32_t i = 0; i < g_rb_count; ++i) {
            const uint32_t *w = (const uint32_t *)((char *)g_rb[i].dst->cpu + g_rb[i].offset);
            uint32_t nz = 0, mx = 0;
            for (VkDeviceSize k = 0; k < g_rb[i].size / 4; ++k) { nz += w[k] != 0; if (w[k] > mx) mx = w[k]; }
            if (frame % 20 == 0)
                fprintf(stderr, "[RBDATA] frame=%u src=%llx size=%llu nonzero=%u max=%u\n", frame,
                        (unsigned long long)g_rb[i].src_va, (unsigned long long)g_rb[i].size, nz, mx);
        }
        g_rb_count = 0;
        return;
    }
    for (uint32_t i = 0; dir && i < g_rb_count; ++i) {
        char path[512];
        snprintf(path, sizeof(path), "%s/rb_%u_src%llx_size%llu.bin", dir, i,
                 (unsigned long long)g_rb[i].src_va, (unsigned long long)g_rb[i].size);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite((char *)g_rb[i].dst->cpu + g_rb[i].offset, 1, g_rb[i].size, f); fclose(f); }
    }
    g_rb_count = 0;
}

/* ---- Render-target dump (BEER_BD12_CAPTURE_RT=1, needs CAPTURE_DIR + a traced frame) ---- */
static uint64_t g_rt_bytes;
int bd12_capture_rt_enabled(void)
{
    static int cached = -1;
    if (cached < 0) cached = getenv("BEER_BD12_CAPTURE_RT") && getenv("BEER_BD12_CAPTURE_DIR");
    return cached && bd12_trace_active();
}

static void bd12_capture_depth_pass(BD12CmdList *list);
void bd12_capture_rt_pass(BD12CmdList *list)
{
    if (!bd12_capture_rt_enabled()) return;
    if (getenv("BEER_BD12_CAPTURE_DEBUG")) fprintf(stderr, "[CAPTURE] rt pass rtv=%u dsv=%d\n", list->gfx.cap_rtv_count, list->gfx.cap_has_dsv);
    BD12Device *device = list->device;
    static uint32_t pass_seq;
    const uint32_t pass = __atomic_add_fetch(&pass_seq, 1, __ATOMIC_RELAXED);
    if (list->gfx.cap_has_dsv) bd12_capture_depth_pass(list);
    if (getenv("BEER_BD12_CAPTURE_RT_DEPTH_ONLY")) return;
    for (uint32_t i = 0; i < list->gfx.cap_rtv_count; ++i) {
        const BD12Descriptor *d = &list->gfx.cap_rtv[i];
        const BD12Image *im = d->image;
        if (d->kind != BD12_DESC_RTV || !im || im->desc.samples > 1) continue;
        uint32_t block = 0;
        bd12_format_block(im->desc.format, VK_IMAGE_ASPECT_COLOR_BIT, NULL, NULL, &block);
        const uint32_t w = im->desc.width >> d->view_mip ? im->desc.width >> d->view_mip : 1;
        const uint32_t h = im->desc.height >> d->view_mip ? im->desc.height >> d->view_mip : 1;
        const VkDeviceSize size = (VkDeviceSize)w * h * block;
        if (!block || !size || size > (64ull << 20)) continue;
        if (g_rt_bytes + size > (3ull << 30) || g_cap_count >= MAX_CAPTURES) continue;
        BD12Buffer *rb = bd12_buffer_create_committed(device, BD12_HEAP_READBACK, size);
        if (!rb) continue;
        bd12_cmd_global_barrier(list);
        VkBufferImageCopy region = {
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, d->view_mip, d->view_base_layer, 1 },
            .imageExtent = { w, h, 1 }
        };
        device->vk.CmdCopyImageToBuffer(list->command_buffer, im->image,
                                        VK_IMAGE_LAYOUT_GENERAL, rb->buffer, 1, &region);
        bd12_cmd_global_barrier(list);
        pthread_mutex_lock(&g_lock);
        if (g_cap_count < MAX_CAPTURES) {
            Capture *c = &g_caps[g_cap_count++];
            memset(c, 0, sizeof(*c));
            c->readback = rb; c->size = size; c->is_rt = 1;
            c->rt_w = w; c->rt_h = h; c->rt_bpp = block; c->rt_fmt = im->desc.format;
            snprintf(c->name, sizeof(c->name), "rt%03u_a%u_%ux%u_vk%u_h%llx", pass, i, w, h,
                     (unsigned)im->desc.format, (unsigned long long)list->gfx.cap_hash);
            g_rt_bytes += size;
            rb = NULL;
        }
        pthread_mutex_unlock(&g_lock);
        if (rb) bd12_buffer_destroy(rb);
    }
}

/* Depth attachments (shadow maps): up to 6 array layers of maps up to ~2048x2048. */
static void bd12_capture_depth_pass(BD12CmdList *list)
{
    BD12Device *device = list->device;
    const BD12Descriptor *d = &list->gfx.cap_dsv;
    const BD12Image *im = d->image;
    if (!im || im->desc.samples > 1 || !(d->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) return;
    const int fmt = (int)im->desc.format;
    if (fmt < 124 || fmt > 130 || fmt == 127) return;
    const uint32_t bpp = (fmt == 124 || fmt == 128) ? 2 : 4;
    const uint32_t w = im->desc.width >> d->view_mip ? im->desc.width >> d->view_mip : 1;
    const uint32_t h = im->desc.height >> d->view_mip ? im->desc.height >> d->view_mip : 1;
    uint64_t max_pix = 4300000;
    if (getenv("BEER_BD12_CAPTURE_DEPTH_MAXPIX")) max_pix = strtoull(getenv("BEER_BD12_CAPTURE_DEPTH_MAXPIX"), NULL, 10);
    if ((uint64_t)w * h > max_pix) return;
    static uint32_t dpass;
    const uint32_t pass = __atomic_add_fetch(&dpass, 1, __ATOMIC_RELAXED);
    const uint32_t layers = d->view_layers ? d->view_layers : 1;
    fprintf(stderr, "[SHADOWPASS] dp%03u dsv %ux%u vk%d layers=%u..+%u mip=%u clear=%d draws=%u pso=%llx\n",
            pass, w, h, fmt, d->view_base_layer, layers, d->view_mip,
            list->gfx.cap_clear_depth, list->gfx.cap_draws, (unsigned long long)list->gfx.cap_hash);
    for (uint32_t l = 0; l < layers && l < 6; ++l) {
        const VkDeviceSize size = (VkDeviceSize)w * h * bpp;
        if (g_rt_bytes + size > (3ull << 30) || g_cap_count >= MAX_CAPTURES) return;
        BD12Buffer *rb = bd12_buffer_create_committed(device, BD12_HEAP_READBACK, size);
        if (!rb) return;
        bd12_cmd_global_barrier(list);
        VkBufferImageCopy region = {
            .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, d->view_mip, d->view_base_layer + l, 1 },
            .imageExtent = { w, h, 1 }
        };
        device->vk.CmdCopyImageToBuffer(list->command_buffer, im->image,
                                        VK_IMAGE_LAYOUT_GENERAL, rb->buffer, 1, &region);
        bd12_cmd_global_barrier(list);
        pthread_mutex_lock(&g_lock);
        if (g_cap_count < MAX_CAPTURES) {
            Capture *c = &g_caps[g_cap_count++];
            memset(c, 0, sizeof(*c));
            c->readback = rb; c->size = size; c->is_rt = 1; c->is_depth = 1;
            c->rt_w = w; c->rt_h = h; c->rt_bpp = bpp; c->rt_fmt = im->desc.format;
            snprintf(c->name, sizeof(c->name), "dp%03u_l%u_%ux%u_vk%u_h%llx", pass, d->view_base_layer + l, w, h,
                     (unsigned)fmt, (unsigned long long)list->gfx.cap_hash);
            g_rt_bytes += size;
            rb = NULL;
        }
        pthread_mutex_unlock(&g_lock);
        if (rb) bd12_buffer_destroy(rb);
    }
}

/* Depth textures sampled by a traced compute dispatch (BEER_BD12_CAPTURE_SRV_DEPTH). */
static struct { const BD12Image *im; uint32_t layer, mip; } seen[256];
static uint32_t seen_count;
void bd12_capture_srv_depth(BD12CmdList *list, BD12Image *im, uint32_t mip, uint32_t base_layer,
                            uint32_t layers, uint64_t shader, uint32_t reg)
{
    if (!getenv("BEER_BD12_CAPTURE_DIR") || im->desc.samples > 1) return;
    const int fmt = (int)im->desc.format;
    if (fmt < 124 || fmt > 130 || fmt == 127) return;
    static uint32_t seq;
    BD12Device *device = list->device;
    const uint32_t bpp = (fmt == 124 || fmt == 128) ? 2 : 4;
    const uint32_t w = im->desc.width >> mip ? im->desc.width >> mip : 1;
    const uint32_t h = im->desc.height >> mip ? im->desc.height >> mip : 1;
    if ((uint64_t)w * h > 17000000ull) return;
    if (!layers) layers = 1;
    for (uint32_t l = 0; l < layers && l < 6; ++l) {
        const uint32_t layer = base_layer + l;
        int dup = 0;
        pthread_mutex_lock(&g_lock);
        for (uint32_t i = 0; i < seen_count; ++i)
            if (seen[i].im == im && seen[i].layer == layer && seen[i].mip == mip) dup = 1;
        if (!dup && seen_count < 256) { seen[seen_count].im = im; seen[seen_count].layer = layer; seen[seen_count].mip = mip; ++seen_count; }
        pthread_mutex_unlock(&g_lock);
        if (dup) continue;
        const VkDeviceSize size = (VkDeviceSize)w * h * bpp;
        if (g_rt_bytes + size > (3ull << 30) || g_cap_count >= MAX_CAPTURES) return;
        BD12Buffer *rb = bd12_buffer_create_committed(device, BD12_HEAP_READBACK, size);
        if (!rb) return;
        bd12_cmd_end_rendering(list);
        bd12_cmd_global_barrier(list);
        VkBufferImageCopy region = {
            .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, mip, layer, 1 },
            .imageExtent = { w, h, 1 }
        };
        device->vk.CmdCopyImageToBuffer(list->command_buffer, im->image,
                                        VK_IMAGE_LAYOUT_GENERAL, rb->buffer, 1, &region);
        bd12_cmd_global_barrier(list);
        pthread_mutex_lock(&g_lock);
        if (g_cap_count < MAX_CAPTURES) {
            Capture *c = &g_caps[g_cap_count++];
            memset(c, 0, sizeof(*c));
            c->readback = rb; c->size = size; c->is_rt = 1; c->is_depth = 1;
            c->rt_w = w; c->rt_h = h; c->rt_bpp = bpp; c->rt_fmt = im->desc.format;
            snprintf(c->name, sizeof(c->name), "sd%03u_cs%llx_t%u_l%u_%ux%u_vk%d", ++seq,
                     (unsigned long long)shader, reg, layer, w, h, fmt);
            g_rt_bytes += size;
            rb = NULL;
        }
        pthread_mutex_unlock(&g_lock);
        if (rb) bd12_buffer_destroy(rb);
    }
}

void bd12_capture_srv_depth_reset(void) {}

static float rt_half(uint16_t h)
{
    const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 31, man = h & 1023;
    float v;
    if (!exp) v = (float)man * (1.0f / 16777216.0f);
    else if (exp == 31) v = man ? 0.0f : 65504.0f;
    else v = (1.0f + man / 1024.0f) * __builtin_powif(2.0f, (int)exp - 15);
    return sign ? -v : v;
}

static float rt_f11(uint32_t bits, int mant_bits)
{
    const uint32_t man = bits & ((1u << mant_bits) - 1), exp = bits >> mant_bits;
    if (!exp) return (float)man / (float)(1u << mant_bits) * __builtin_powif(2.0f, -14);
    if (exp == 31) return 65504.0f;
    return (1.0f + (float)man / (float)(1u << mant_bits)) * __builtin_powif(2.0f, (int)exp - 15);
}

static uint8_t rt_tone(float x, int hdr)
{
    if (x != x || x < 0) x = 0;
    if (hdr) {
        x = x / (1.0f + x);
        float y = x > 1.0f ? x : 1.0f;
        for (int k = 0; k < 24; ++k) y = 0.5f * (y + x / y);
        x = x > 0 ? y : 0;
    }
    if (x > 1) x = 1;
    return (uint8_t)(x * 255.0f + 0.5f);
}

/* Converts one render target to a downscaled binary PPM. Returns 0 if the format is not handled. */
static int rt_write_ppm(const Capture *c, const char *dir)
{
    const uint8_t *src = c->readback->cpu;
    if (!src) return 0;
    int scale = 4;
    if (getenv("BEER_BD12_CAPTURE_RT_SCALE")) scale = atoi(getenv("BEER_BD12_CAPTURE_RT_SCALE"));
    if (scale < 1) scale = 1;
    const uint32_t ow = (c->rt_w + scale - 1) / scale, oh = (c->rt_h + scale - 1) / scale;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ppm", dir, c->name);
    uint8_t *out = malloc((size_t)ow * oh * 3);
    if (!out) return 0;
    int ok = 1;
    if (c->is_depth) {
        float lo = 1e30f, hi = -1e30f;
        float *vals = malloc((size_t)ow * oh * sizeof(float));
        if (!vals) { free(out); return 0; }
        for (uint32_t oy = 0; oy < oh; ++oy)
            for (uint32_t ox = 0; ox < ow; ++ox) {
                const uint32_t x = ox * scale < c->rt_w ? ox * scale : c->rt_w - 1;
                const uint32_t y = oy * scale < c->rt_h ? oy * scale : c->rt_h - 1;
                const uint8_t *px = src + ((size_t)y * c->rt_w + x) * c->rt_bpp;
                float v;
                uint32_t u;
                if (c->rt_bpp == 2) { uint16_t h16; memcpy(&h16, px, 2); v = h16 / 65535.f; }
                else if ((int)c->rt_fmt == 125 || (int)c->rt_fmt == 129) { memcpy(&u, px, 4); v = (u & 0xffffff) / 16777215.f; }
                else memcpy(&v, px, 4);
                if (v != v) v = 0;
                vals[(size_t)oy * ow + ox] = v;
                if (v < lo) lo = v;
                if (v > hi) hi = v;
            }
        const float span = hi > lo ? hi - lo : 1.0f;
        for (size_t k = 0; k < (size_t)ow * oh; ++k) {
            uint8_t g = (uint8_t)(((vals[k] - lo) / span) * 255.0f + 0.5f);
            out[k * 3] = out[k * 3 + 1] = out[k * 3 + 2] = g;
        }
        fprintf(stderr, "[CAPTURE] depth %s range %.6f..%.6f\n", c->name, lo, hi);
        free(vals);
        FILE *f = fopen(path, "wb");
        if (f) { fprintf(f, "P6\n%u %u\n255\n", ow, oh); fwrite(out, 1, (size_t)ow * oh * 3, f); fclose(f); }
        free(out);
        return 1;
    }
    for (uint32_t oy = 0; oy < oh && ok; ++oy) {
        for (uint32_t ox = 0; ox < ow; ++ox) {
            const uint32_t x = ox * scale < c->rt_w ? ox * scale : c->rt_w - 1;
            const uint32_t y = oy * scale < c->rt_h ? oy * scale : c->rt_h - 1;
            const uint8_t *px = src + ((size_t)y * c->rt_w + x) * c->rt_bpp;
            float r = 0, g = 0, b = 0; int hdr = 0;
            uint32_t u32;
            switch ((int)c->rt_fmt) {
            case 37: case 43: r = px[0] / 255.f; g = px[1] / 255.f; b = px[2] / 255.f; break;
            case 44: case 50: b = px[0] / 255.f; g = px[1] / 255.f; r = px[2] / 255.f; break;
            case 9: r = g = b = px[0] / 255.f; break;
            case 16: r = px[0] / 255.f; g = px[1] / 255.f; break;
            case 64: memcpy(&u32, px, 4);
                r = (u32 & 1023) / 1023.f; g = ((u32 >> 10) & 1023) / 1023.f; b = ((u32 >> 20) & 1023) / 1023.f; break;
            case 122: memcpy(&u32, px, 4); hdr = 1;
                r = rt_f11(u32 & 2047, 6); g = rt_f11((u32 >> 11) & 2047, 6); b = rt_f11((u32 >> 22) & 1023, 5); break;
            case 97: hdr = 1; { const uint16_t *h = (const uint16_t *)px;
                r = rt_half(h[0]); g = rt_half(h[1]); b = rt_half(h[2]); } break;
            case 91: { const uint16_t *h = (const uint16_t *)px; r = h[0] / 65535.f; g = h[1] / 65535.f; b = h[2] / 65535.f; } break;
            case 83: hdr = 1; { const uint16_t *h = (const uint16_t *)px; r = rt_half(h[0]); g = rt_half(h[1]); } break;
            case 76: hdr = 1; { const uint16_t *h = (const uint16_t *)px; r = g = b = rt_half(h[0]); } break;
            case 70: { const uint16_t *h = (const uint16_t *)px; r = g = b = h[0] / 65535.f; } break;
            case 100: hdr = 1; { float f; memcpy(&f, px, 4); r = g = b = f; } break;
            case 103: hdr = 1; { float f[2]; memcpy(f, px, 8); r = f[0]; g = f[1]; } break;
            case 109: hdr = 1; { float f[4]; memcpy(f, px, 16); r = f[0]; g = f[1]; b = f[2]; } break;
            default: ok = 0; break;
            }
            if (!ok) break;
            uint8_t *o = out + ((size_t)oy * ow + ox) * 3;
            o[0] = rt_tone(r, hdr); o[1] = rt_tone(g, hdr); o[2] = rt_tone(b, hdr);
        }
    }
    if (ok) {
        FILE *f = fopen(path, "wb");
        if (f) { fprintf(f, "P6\n%u %u\n255\n", ow, oh); fwrite(out, 1, (size_t)ow * oh * 3, f); fclose(f); }
    }
    free(out);
    return ok;
}

/* Called at the traced frame's present after the GPU has drained. */
void bd12_capture_flush(void)
{
    const char *dir = getenv("BEER_BD12_CAPTURE_DIR");
    if (!dir) return;
    g_rt_bytes = 0;
    seen_count = 0;
    for (uint32_t i = 0; i < g_cap_count; ++i) {
        Capture *c = &g_caps[i];
        if (c->is_rt) {
            if (!rt_write_ppm(c, dir))
                fprintf(stderr, "[CAPTURE] unhandled render-target format %u (%s)\n", (unsigned)c->rt_fmt, c->name);
            bd12_buffer_destroy(c->readback);
            continue;
        }
        char path[512];
        snprintf(path, sizeof(path), "%s/%s.bin", dir, c->name);
        FILE *f = fopen(path, "wb");
        if (f && c->readback->cpu) fwrite(c->readback->cpu, 1, c->size, f);
        if (f) fclose(f);
        bd12_buffer_destroy(c->readback);
    }
    if (g_cap_count)
        fprintf(stderr, "[CAPTURE] wrote %u buffers to %s\n", g_cap_count, dir);
    g_cap_count = 0;
    g_sequence = 0;
}

int bd12_capture_pending(void) { return g_cap_count != 0; }
