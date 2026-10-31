#define _GNU_SOURCE
/* BD11 part 4: IDXGISwapChain and IDXGIDevice for BD11 devices. */

#include "bd11.h"
#include "../d3d12v/bd12_com.h"

#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Provided by the loader (weak so standalone builds link). */
extern void *__attribute__((weak)) beer_dxgi_factory(void);
extern void *__attribute__((weak)) beer_dxgi_adapter(void);
extern int __attribute__((weak)) xwayland_window_set_fullscreen(int enabled);

typedef struct {
    B11Obj obj;
    uint8_t desc[72];
    B11Texture *back;
    uint32_t width, height, format;
    int fullscreen;
    uint32_t present_count;
    uint64_t frame_serial[8];     /* retire serial of recent presents, for the frame-latency limit */
} B11Swap;

/* Frames the CPU may run ahead of the GPU. DXGI defaults to 3; 2 keeps input latency lower while CPU recording
 * and GPU rendering still overlap. BEER_BD11_FRAME_LATENCY=1..8 overrides (1 = the old fully serialised present). */
static uint32_t frame_latency(void)
{
    static uint32_t latency;
    if (!latency) {
        const char *e = getenv("BEER_BD11_FRAME_LATENCY");
        const int v = e ? atoi(e) : 2;
        latency = v < 1 ? 1 : v > 8 ? 8 : (uint32_t)v;
    }
    return latency;
}

typedef struct { void **vtable; _Atomic uint32_t refs; B11Device *dev; } B11DxgiDevice;

#define GUIDB(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) \
    { (a) & 0xff, ((a) >> 8) & 0xff, ((a) >> 16) & 0xff, ((a) >> 24) & 0xff, (b) & 0xff, ((b) >> 8) & 0xff, \
      (c) & 0xff, ((c) >> 8) & 0xff, d0, d1, d2, d3, d4, d5, d6, d7 }

static int iid_in(const uint8_t *riid, const uint8_t (*list)[16], size_t n)
{
    for (size_t i = 0; i < n; ++i) if (!memcmp(riid, list[i], 16)) return 1;
    return 0;
}

/* ---- IDXGIDevice ------------------------------------------------------------------------- */

static void *vt_dxgi_device[14];
static B11HR B11_MS dd_qi(B11DxgiDevice *self, const uint8_t *riid, void **out)
{
    if (!out) return B11_E_INVALIDARG;
    static const uint8_t dxgi_dev[16] = GUIDB(0x54ec77fau, 0x1377, 0x44e6, 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c);
    if (riid && !memcmp(riid, dxgi_dev, 16)) {
        atomic_fetch_add(&self->refs, 1);
        *out = self;
        return B11_S_OK;
    }
    return b11_query_interface(&self->dev->obj, riid, out);   /* ID3D11Device, IUnknown... */
}
static uint32_t B11_MS dd_addref(B11DxgiDevice *self) { return atomic_fetch_add(&self->refs, 1) + 1; }
static uint32_t B11_MS dd_release(B11DxgiDevice *self) { return atomic_fetch_sub(&self->refs, 1) - 1; }
static B11HR B11_MS dd_private(void) { return B11_S_OK; }
static B11HR B11_MS dd_get_parent(B11DxgiDevice *self, const uint8_t *riid, void **out)
{
    (void)self; (void)riid;
    if (!out) return B11_E_INVALIDARG;
    void *adapter = beer_dxgi_adapter ? beer_dxgi_adapter() : NULL;
    *out = adapter;
    return adapter ? B11_S_OK : B11_E_FAIL;
}
static B11HR B11_MS dd_get_adapter(B11DxgiDevice *self, void **out)
{ return dd_get_parent(self, NULL, out); }
static B11HR B11_MS dd_noop(void) { return B11_S_OK; }
static B11HR B11_MS dd_get_gpu_priority(B11DxgiDevice *self, int *p) { (void)self; if (p) *p = 0; return B11_S_OK; }
static B11HR B11_MS dd_get_latency(B11DxgiDevice *self, uint32_t *n) { (void)self; if (n) *n = 3; return B11_S_OK; }

B11HR b11_dxgi_device_query(B11Device *dev, void **out)
{
    static B11DxgiDevice device;
    if (!vt_dxgi_device[0]) {
        vt_dxgi_device[0] = (void *)dd_qi; vt_dxgi_device[1] = (void *)dd_addref; vt_dxgi_device[2] = (void *)dd_release;
        vt_dxgi_device[3] = (void *)dd_private; vt_dxgi_device[4] = (void *)dd_private; vt_dxgi_device[5] = (void *)dd_private;
        vt_dxgi_device[6] = (void *)dd_get_parent; vt_dxgi_device[7] = (void *)dd_get_adapter;
        vt_dxgi_device[8] = (void *)dd_noop; vt_dxgi_device[9] = (void *)dd_noop; vt_dxgi_device[10] = (void *)dd_noop;
        vt_dxgi_device[11] = (void *)dd_get_gpu_priority; vt_dxgi_device[12] = (void *)dd_noop;
        vt_dxgi_device[13] = (void *)dd_get_latency;
        device.vtable = vt_dxgi_device;
        atomic_init(&device.refs, 1);
    }
    device.dev = dev;
    atomic_fetch_add(&device.refs, 1);
    *out = &device;
    return B11_S_OK;
}

/* ---- IDXGISwapChain ------------------------------------------------------------------------ */

static void *vt_swap[18];

static B11HR B11_MS sc_qi(B11Obj *self, const uint8_t *riid, void **out)
{
    if (!out) return B11_E_INVALIDARG;
    *out = NULL;
    static const uint8_t refuse[][16] = {
        GUIDB(0x790a45f7u, 0x0d42, 0x4876, 0x98, 0x3a, 0x0a, 0x55, 0xcf, 0xe6, 0xf4, 0xaa),   /* IDXGISwapChain1 */
        GUIDB(0xa8be2ac4u, 0x199f, 0x4946, 0xb3, 0x31, 0x79, 0x59, 0x9f, 0xb9, 0x8d, 0xe7),   /* IDXGISwapChain2 */
        GUIDB(0x94d99bdbu, 0xf1f8, 0x4ab0, 0xb2, 0x36, 0x7d, 0xa0, 0x17, 0x0e, 0xda, 0xb1),   /* IDXGISwapChain3 */
        GUIDB(0x3d585d5au, 0xbd4a, 0x489e, 0xb1, 0xf4, 0x3d, 0xbc, 0xb6, 0x45, 0x2f, 0xfb),   /* IDXGISwapChain4 */
    };
    if (!riid || iid_in(riid, refuse, 4)) return B11_E_NOINTERFACE;
    b11_add_ref(self);
    *out = self;
    return B11_S_OK;
}
static uint32_t B11_MS sc_addref(B11Obj *self) { return b11_add_ref(self); }
static uint32_t B11_MS sc_release(B11Obj *self) { return b11_release(self); }
static B11HR B11_MS sc_private(void) { return B11_S_OK; }
static B11HR B11_MS sc_get_parent(B11Obj *self, const uint8_t *riid, void **out)
{
    (void)self; (void)riid;
    if (!out) return B11_E_INVALIDARG;
    void *factory = beer_dxgi_factory ? beer_dxgi_factory() : NULL;
    *out = factory;
    return factory ? B11_S_OK : B11_E_FAIL;
}
static B11HR B11_MS sc_get_device(B11Obj *self, const uint8_t *riid, void **out)
{
    if (!out) return B11_E_INVALIDARG;
    return b11_query_interface(&self->dev->obj, riid, out);
}

static void swap_destroy(B11Obj *o)
{
    B11Swap *s = (B11Swap *)o;
    if (s->back) b11_release(&s->back->obj);
    free(s);
}

static B11HR make_back_buffer(B11Swap *s)
{
    B11Device *dev = s->obj.dev;
    B11TextureDesc d = { .dimension = 2, .width = s->width, .height = s->height, .depth = 1, .mips = 1,
                         .array_size = 1, .format = s->format, .samples = 1,
                         .usage = B11_USAGE_DEFAULT, .bind = B11_BIND_RTV | B11_BIND_SRV };
    void *tex = NULL;
    const B11HR hr = b11_texture_create(dev, &d, NULL, &tex);
    if (hr != B11_S_OK) return hr;
    if (s->back) b11_release(&s->back->obj);
    s->back = tex;
    s->back->is_backbuffer = 1;
    if (getenv("BEER_BD11_TRACE")) fprintf(stderr, "[BD11] backbuffer res=%p\n", (void *)s->back);
    return B11_S_OK;
}

static B11HR B11_MS sc_present(B11Swap *s, uint32_t sync, uint32_t flags)
{
    (void)sync; (void)flags;
    B11Device *dev = s->obj.dev;
    if (flags & 1) return B11_S_OK;                       /* DXGI_PRESENT_TEST */
    pthread_mutex_lock(&dev->lock);
    b11_cmd(dev);
    b11_texture_use(dev->immediate, s->back, BD12_STATE_PRESENT);
    pthread_mutex_unlock(&dev->lock);
    /* Submit without waiting: the presenter's blit is queued behind this frame on the same queue. Only block
     * once the GPU is `latency` frames behind, so the CPU records the next frame while this one renders. */
    b11_flush(dev, 0);
    {
        const uint32_t latency = frame_latency();
        s->frame_serial[s->present_count % 8] = dev->serial;
        if (s->present_count + 1 >= latency)
            b11_wait_serial(dev, s->frame_serial[(s->present_count + 1 - latency) % 8]);
    }
    BD12Buffer *dump = NULL;
    const char *dump_list = getenv("BEER_BD11_DUMP_PRESENT");   /* comma list of present numbers, files go to BEER_BD11_DUMP_DIR */
    if (dump_list && s->back->image && s->format == 28) {
        char key[24], hay[512];
        snprintf(key, sizeof(key), ",%u,", s->present_count);
        snprintf(hay, sizeof(hay), ",%s,", dump_list);
        if (strstr(hay, key)) {
            dump = bd12_buffer_create_committed(dev->bd, BD12_HEAP_READBACK, (size_t)s->width * s->height * 4);
            if (dump) {
                BD12ImagePoint origin = {0};
                pthread_mutex_lock(&dev->lock);
                b11_texture_use(dev->immediate, s->back, BD12_STATE_COPY_SOURCE);
                bd12_cmd_global_barrier(b11_cmd(dev));
                bd12_cmd_copy_image_to_buffer(b11_cmd(dev), dump, 0, s->width * 4, s->back->image, origin, s->width, s->height, 1);
                b11_texture_use(dev->immediate, s->back, BD12_STATE_PRESENT);
                pthread_mutex_unlock(&dev->lock);
                b11_flush(dev, 1);
            }
        }
    }
    if (dump) {
        const char *dir = getenv("BEER_BD11_DUMP_DIR");
        char path[512];
        snprintf(path, sizeof(path), "%s/present%05u.ppm", dir ? dir : ".", s->present_count);
        FILE *f = fopen(path, "wb");
        if (f && dump->cpu) {
            fprintf(f, "P6\n%u %u\n255\n", s->width, s->height);
            const uint8_t *px = dump->cpu;
            for (size_t i = 0; i < (size_t)s->width * s->height; ++i) fwrite(px + i * 4, 1, 3, f);
        }
        if (f) fclose(f);
        bd12_buffer_destroy(dump);
    }
    bd12_device_advance_epoch(dev->bd);
    if (bd12_capture_pending()) { b11_flush(dev, 1); bd12_capture_flush(); }   /* traced frame: files */
    bd12_trace_frame_advance();
    dev->frame++;
    s->present_count++;
    {   /* BEER_BD11_TRACE_FILE=<path>: creating the file traces the next frame (and the file is removed). */
        const char *trace_file = getenv("BEER_BD11_TRACE_FILE");
        if (trace_file && unlink(trace_file) == 0) g_b11_trace_frame = (long)dev->frame;
    }
    {
        extern uint64_t g_b11_stats[16];
        static int stats = -1;
        if (stats < 0) stats = getenv("BEER_BD11_STATS") != NULL;
        static int flog = -1;
        static uint64_t last_draws;
        if (flog < 0) flog = getenv("BEER_BD11_FRAMELOG") != NULL;
        if (flog) {
            if (g_b11_stats[0] != last_draws || s->present_count % 60 == 0)
                fprintf(stderr, "[BD11 FRAME] %u draws=%llu\n", s->present_count, (unsigned long long)(g_b11_stats[0] - last_draws));
            last_draws = g_b11_stats[0];
        }
        if (s->present_count % 300 == 0) {       /* BD12 counts every draw it could not record */
            uint64_t total = 0;
            for (int i = 0; i < BD12_DROP_COUNT; ++i) total += g_bd12_drops[i];
            if (total)
                fprintf(stderr, "[BD11 DROPS] draw(no-state)=%llu draw(no-descriptors)=%llu dispatch=%llu/%llu exec-indirect=%llu/%llu pso-failed=%llu\n",
                        (unsigned long long)g_bd12_drops[BD12_DROP_DRAW_NO_STATE], (unsigned long long)g_bd12_drops[BD12_DROP_DRAW_NO_DESCRIPTORS],
                        (unsigned long long)g_bd12_drops[BD12_DROP_DISPATCH_NO_STATE], (unsigned long long)g_bd12_drops[BD12_DROP_DISPATCH_NO_DESCRIPTORS],
                        (unsigned long long)g_bd12_drops[4], (unsigned long long)g_bd12_drops[5], (unsigned long long)g_bd12_drops[BD12_DROP_PSO_FAILED]);
        }
        if (stats && s->present_count % 120 == 0)
            fprintf(stderr, "[BD11 STATS] frame=%u draws=%llu skipped=%llu clearRTV=%llu clearDSV=%llu update=%llu bufwrite=%llu lists=%llu ops=%llu indirect=%llu\n",
                    s->present_count, (unsigned long long)g_b11_stats[0], (unsigned long long)g_b11_stats[1],
                    (unsigned long long)g_b11_stats[2], (unsigned long long)g_b11_stats[3], (unsigned long long)g_b11_stats[5],
                    (unsigned long long)g_b11_stats[6], (unsigned long long)g_b11_stats[8], (unsigned long long)g_b11_stats[7],
                    (unsigned long long)g_b11_stats[9]);
        if (stats && s->present_count % 120 == 0) {
            fprintf(stderr, "[BD11 STATS] per frame: barriers=%llu render-passes=%llu\n",
                    (unsigned long long)(g_bd12_cmd_counts[BD12_COUNT_BARRIER] / 120),
                    (unsigned long long)(g_bd12_cmd_counts[BD12_COUNT_RENDER_PASS] / 120));
            g_bd12_cmd_counts[BD12_COUNT_BARRIER] = g_bd12_cmd_counts[BD12_COUNT_RENDER_PASS] = 0;
            extern uint64_t g_b11_map_stats[8][3];
            fprintf(stderr, "[BD11 STATS] deferred maps per frame (<=64K/<=1M/larger): write=%llu/%llu/%llu discard=%llu/%llu/%llu no-overwrite=%llu/%llu/%llu\n",
                    (unsigned long long)(g_b11_map_stats[2][0] / 120), (unsigned long long)(g_b11_map_stats[2][1] / 120), (unsigned long long)(g_b11_map_stats[2][2] / 120),
                    (unsigned long long)(g_b11_map_stats[4][0] / 120), (unsigned long long)(g_b11_map_stats[4][1] / 120), (unsigned long long)(g_b11_map_stats[4][2] / 120),
                    (unsigned long long)(g_b11_map_stats[5][0] / 120), (unsigned long long)(g_b11_map_stats[5][1] / 120), (unsigned long long)(g_b11_map_stats[5][2] / 120));
            memset(g_b11_map_stats, 0, sizeof(g_b11_map_stats));
        }
        if (stats && s->present_count % 120 == 0) {
            extern uint64_t g_b11_query_counts[8];
            fprintf(stderr, "[BD11 QUERIES] getdata event=%llu occlusion=%llu timestamp=%llu disjoint=%llu stats=%llu predicate=%llu\n",
                    (unsigned long long)g_b11_query_counts[0], (unsigned long long)g_b11_query_counts[1],
                    (unsigned long long)g_b11_query_counts[2], (unsigned long long)g_b11_query_counts[3],
                    (unsigned long long)g_b11_query_counts[4], (unsigned long long)g_b11_query_counts[5]);
        }
    }
    Bd12ImagePresenter presenter = bd12_com_get_image_presenter();
    static uint64_t present_ns;
    if (presenter && s->back->image) {
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        presenter(s->back->image->image, bd12_layout_for_state(BD12_STATE_PRESENT, 0), s->width, s->height, s->format);
        clock_gettime(CLOCK_MONOTONIC, &b);
        present_ns += (uint64_t)((b.tv_sec - a.tv_sec) * 1000000000ll + (b.tv_nsec - a.tv_nsec));
    }
    {   /* BEER_BD11_FPS=1: presents per second and the sync interval the game asks for. */
        static int fps_log = -1;
        if (fps_log < 0) fps_log = getenv("BEER_BD11_FPS") != NULL;
        if (fps_log) {
            static uint64_t window_start_ns, worst_ns, last_ns;
            static uint32_t frames;
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            const uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
            if (last_ns && now - last_ns > worst_ns) worst_ns = now - last_ns;
            last_ns = now;
            if (!window_start_ns) window_start_ns = now;
            ++frames;
            if (now - window_start_ns >= 1000000000ull) {
                extern uint64_t g_b11_wait_ns;
                fprintf(stderr, "[BD11 FPS] %.1f fps sync=%u worst=%.1f ms gpu-wait=%.1f ms/s present=%.1f ms/s\n",
                        frames * 1e9 / (double)(now - window_start_ns), sync, worst_ns / 1e6,
                        __atomic_exchange_n(&g_b11_wait_ns, 0, __ATOMIC_RELAXED) / 1e6, present_ns / 1e6);
                present_ns = 0;
                window_start_ns = now;
                frames = 0;
                worst_ns = 0;
            }
        }
    }
    return B11_S_OK;
}

static B11HR B11_MS sc_get_buffer(B11Swap *s, uint32_t index, const uint8_t *riid, void **out)
{
    if (!out) return B11_E_INVALIDARG;
    *out = NULL;
    if (index != 0 || !s->back) return B11_E_INVALIDARG;
    return b11_query_interface(&s->back->obj, riid, out);
}

static B11HR B11_MS sc_set_fullscreen(B11Swap *s, int enabled, void *output)
{
    (void)output;
    s->fullscreen = enabled != 0;
    if (xwayland_window_set_fullscreen) xwayland_window_set_fullscreen(enabled != 0);
    return B11_S_OK;
}
static B11HR B11_MS sc_get_fullscreen(B11Swap *s, int *state, void **output)
{
    if (state) *state = s->fullscreen;
    if (output) *output = NULL;
    return B11_S_OK;
}
static B11HR B11_MS sc_get_desc(B11Swap *s, uint8_t *desc)
{
    if (!desc) return B11_E_INVALIDARG;
    memcpy(desc, s->desc, 72);
    return B11_S_OK;
}
static B11HR B11_MS sc_resize_buffers(B11Swap *s, uint32_t count, uint32_t w, uint32_t h, uint32_t format, uint32_t flags)
{
    (void)count; (void)flags;
    B11Device *dev = s->obj.dev;
    if (w) s->width = w;
    if (h) s->height = h;
    if (format) s->format = format;
    b11_flush(dev, 1);
    uint32_t *d = (uint32_t *)s->desc;
    d[0] = s->width; d[1] = s->height; d[4] = s->format;
    return make_back_buffer(s);
}
static B11HR B11_MS sc_resize_target(B11Swap *s, const uint32_t *mode)
{
    (void)s; (void)mode;
    return B11_S_OK;
}
static B11HR B11_MS sc_get_output(B11Swap *s, void **out)
{
    (void)s;
    if (out) *out = NULL;
    return B11_DXGI_ERROR_NOT_FOUND;
}
static B11HR B11_MS sc_get_stats(B11Swap *s, void *stats)
{
    (void)s;
    if (stats) memset(stats, 0, 24);
    return B11_S_OK;
}
static B11HR B11_MS sc_get_last_present_count(B11Swap *s, uint32_t *count)
{
    if (count) *count = s->present_count;
    return B11_S_OK;
}

B11HR b11_swapchain_create(B11Device *dev, const void *desc, void **out)
{
    if (!desc || !out) return B11_E_INVALIDARG;
    *out = NULL;
    if (!vt_swap[0]) {
        vt_swap[0] = (void *)sc_qi; vt_swap[1] = (void *)sc_addref; vt_swap[2] = (void *)sc_release;
        vt_swap[3] = (void *)sc_private; vt_swap[4] = (void *)sc_private; vt_swap[5] = (void *)sc_private;
        vt_swap[6] = (void *)sc_get_parent; vt_swap[7] = (void *)sc_get_device; vt_swap[8] = (void *)sc_present;
        vt_swap[9] = (void *)sc_get_buffer; vt_swap[10] = (void *)sc_set_fullscreen;
        vt_swap[11] = (void *)sc_get_fullscreen; vt_swap[12] = (void *)sc_get_desc;
        vt_swap[13] = (void *)sc_resize_buffers; vt_swap[14] = (void *)sc_resize_target;
        vt_swap[15] = (void *)sc_get_output; vt_swap[16] = (void *)sc_get_stats;
        vt_swap[17] = (void *)sc_get_last_present_count;
    }
    B11Swap *s = b11_obj_new(dev, B11_SWAPCHAIN, sizeof(*s), vt_swap, swap_destroy);
    if (!s) return B11_E_OUTOFMEMORY;
    memcpy(s->desc, desc, 72);
    const uint32_t *d = (const uint32_t *)s->desc;
    s->width = d[0]; s->height = d[1]; s->format = d[4];
    const uint32_t windowed = d[14];
    if (!s->width || !s->height || !s->format) { swap_destroy(&s->obj); return B11_E_INVALIDARG; }
    fprintf(stderr, "[BD11] swap chain %ux%u format=%u buffers=%u mode=%s\n", s->width, s->height, s->format,
            d[10], windowed ? "windowed" : "fullscreen");
    B11HR hr = make_back_buffer(s);
    if (hr != B11_S_OK) { swap_destroy(&s->obj); return hr; }
    if (!windowed) {
        s->fullscreen = 1;
        if (xwayland_window_set_fullscreen) xwayland_window_set_fullscreen(1);
    }
    *out = s;
    return B11_S_OK;
}

/* Either interface of a BD11 device (ID3D11Device or its IDXGIDevice) -> the B11Device, else NULL. */
static B11Device *device_from_object(const void *object)
{
    if (!object) return NULL;
    void *const *vtable = *(void *const *const *)object;
    if (vtable == g_b11_device_vtable) return (B11Device *)object;
    if (vtable == vt_dxgi_device) return ((const B11DxgiDevice *)object)->dev;
    return NULL;
}

int b11_is_device(const void *object) { return device_from_object(object) != NULL; }

B11HR b11_swapchain_create_for_loader(void *device, const void *desc, void **out)
{
    B11Device *dev = device_from_object(device);
    if (!dev) return B11_E_INVALIDARG;
    return b11_swapchain_create(dev, desc, out);
}
