/* Drives the BD12 COM shim through its guest-facing vtables (ms_abi), the way
 * the game does: upload -> default copy -> readback, and an RTV clear copied
 * back through a placed footprint. */

#include "bd12_com.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef void (*SignalEventFn)(uint64_t);
SignalEventFn beer_d3d12_get_event_signaler(void) { return NULL; }

#define V(obj, slot) (((void **)(*(void ***)(obj)))[slot])

static int failures;
#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) { fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
                       ++failures; }                                          \
    } while (0)

static void *make_buffer(void *dev, uint32_t heap_type, uint64_t size,
                         uint32_t state)
{
    uint8_t props[20] = { 0 };
    memcpy(props, &heap_type, 4);
    uint8_t desc[56] = { 0 };
    uint32_t dim = 1, layout = 1, samples = 1;
    uint16_t mips = 1, depth = 1;
    uint32_t height = 1;
    memcpy(desc, &dim, 4);
    memcpy(desc + 16, &size, 8);
    memcpy(desc + 24, &height, 4);
    memcpy(desc + 28, &depth, 2);
    memcpy(desc + 30, &mips, 2);
    memcpy(desc + 36, &samples, 4);
    memcpy(desc + 44, &layout, 4);
    void *res = NULL;
    HR hr = ((HR(MS *)(void *, const void *, uint32_t, const void *, uint32_t,
                       const void *, const uint8_t *, void **))V(dev, 27))(
        dev, props, 0, desc, state, NULL, com_iid_iunknown, &res);
    CHECK(hr == 0 && res, "CreateCommittedResource buffer");
    return res;
}

static void *make_rt(void *dev, uint32_t w, uint32_t h)
{
    uint8_t props[20] = { 0 };
    uint32_t type = 1;
    memcpy(props, &type, 4);
    uint8_t desc[56] = { 0 };
    uint32_t dim = 3, fmt = 28, samples = 1, flags = 1;
    uint16_t mips = 1, depth = 1;
    uint64_t width = w;
    memcpy(desc, &dim, 4);
    memcpy(desc + 16, &width, 8);
    memcpy(desc + 24, &h, 4);
    memcpy(desc + 28, &depth, 2);
    memcpy(desc + 30, &mips, 2);
    memcpy(desc + 32, &fmt, 4);
    memcpy(desc + 36, &samples, 4);
    memcpy(desc + 48, &flags, 4);
    void *res = NULL;
    HR hr = ((HR(MS *)(void *, const void *, uint32_t, const void *, uint32_t,
                       const void *, const uint8_t *, void **))V(dev, 27))(
        dev, props, 0, desc, 0x4, NULL, com_iid_iunknown, &res);
    CHECK(hr == 0 && res, "CreateCommittedResource render target");
    return res;
}

static void barrier(void *list, void *res, uint32_t before, uint32_t after)
{
    uint8_t b[32] = { 0 };
    uint32_t sub = 0xffffffffu;
    memcpy(b + 8, &res, sizeof(res));
    memcpy(b + 16, &sub, 4);
    memcpy(b + 20, &before, 4);
    memcpy(b + 24, &after, 4);
    ((void(MS *)(void *, uint32_t, const void *))V(list, 26))(list, 1, b);
}

static int wait_fence(void *fence, uint64_t value)
{
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        uint64_t done = ((uint64_t(MS *)(void *))V(fence, 8))(fence);
        if (done >= value) return 1;
        clock_gettime(CLOCK_MONOTONIC, &t);
        if (t.tv_sec - t0.tv_sec > 10) return 0;
    }
}

int main(void)
{
    void *dev = NULL;
    HR hr = beer_bd12_create_device(com_iid_iunknown, &dev);
    CHECK(hr == 0 && dev, "create device");
    if (!dev) return 1;

    void *queue = NULL, *alloc = NULL, *list = NULL, *fence = NULL;
    uint32_t qdesc[4] = { 0 };
    CHECK(((HR(MS *)(void *, const void *, const uint8_t *, void **))V(dev, 8))(
              dev, qdesc, com_iid_iunknown, &queue) == 0, "queue");
    CHECK(((HR(MS *)(void *, uint32_t, const uint8_t *, void **))V(dev, 9))(
              dev, 0, com_iid_iunknown, &alloc) == 0, "allocator");
    CHECK(((HR(MS *)(void *, uint32_t, uint32_t, void *, void *, const uint8_t *,
                     void **))V(dev, 12))(dev, 0, 0, alloc, NULL,
                                          com_iid_iunknown, &list) == 0, "list");
    CHECK(((HR(MS *)(void *, uint64_t, uint32_t, const uint8_t *, void **))V(dev, 36))(
              dev, 0, 0, com_iid_iunknown, &fence) == 0, "fence");

    enum { N = 4096 };
    void *upload = make_buffer(dev, 2, N, 0xac3);
    void *def = make_buffer(dev, 1, N, 0);
    void *readback = make_buffer(dev, 3, 64 * 64 * 4 + N, 0x400);
    void *rt = make_rt(dev, 64, 64);

    uint8_t *mapped = NULL;
    CHECK(((HR(MS *)(void *, uint32_t, const void *, void **))V(upload, 8))(
              upload, 0, NULL, (void **)&mapped) == 0 && mapped, "map upload");
    for (int i = 0; i < N; ++i) mapped[i] = (uint8_t)(i * 7 + 3);

    /* RTV descriptor. */
    void *rtv_heap = NULL;
    uint32_t hdesc[4] = { 2, 1, 0, 0 };
    CHECK(((HR(MS *)(void *, const void *, const uint8_t *, void **))V(dev, 14))(
              dev, hdesc, com_iid_iunknown, &rtv_heap) == 0, "rtv heap");
    uint64_t cpu = 0;
    ((void *(MS *)(void *, uint64_t *))V(rtv_heap, 9))(rtv_heap, &cpu);
    CHECK(cpu != 0, "cpu handle");
    ((void(MS *)(void *, void *, const void *, uint64_t))V(dev, 20))(
        dev, rt, NULL, cpu);

    /* upload -> default */
    barrier(list, def, 0, 0x400);
    ((void(MS *)(void *, void *, uint64_t, void *, uint64_t, uint64_t))V(list, 15))(
        list, def, 0, upload, 0, N);
    barrier(list, def, 0x400, 0x800);
    ((void(MS *)(void *, void *, uint64_t, void *, uint64_t, uint64_t))V(list, 15))(
        list, readback, 0, def, 0, N);

    /* clear RTV, copy to readback after the first N bytes (aligned to 512) */
    ((void(MS *)(void *, uint32_t, const uint64_t *, int32_t, const uint64_t *))V(list, 46))(
        list, 1, &cpu, 0, NULL);
    float color[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
    ((void(MS *)(void *, uint64_t, const float *, uint32_t, const void *))V(list, 48))(
        list, cpu, color, 0, NULL);
    barrier(list, rt, 0x4, 0x800);
    uint8_t dst[48] = { 0 }, src[48] = { 0 };
    uint32_t type_placed = 1, type_sub = 0, fmt = 28, w = 64, h = 64, d = 1,
             pitch = 256;
    uint64_t off = N;
    memcpy(dst, &readback, sizeof(void *));
    memcpy(dst + 8, &type_placed, 4);
    memcpy(dst + 16, &off, 8);
    memcpy(dst + 24, &fmt, 4);
    memcpy(dst + 28, &w, 4);
    memcpy(dst + 32, &h, 4);
    memcpy(dst + 36, &d, 4);
    memcpy(dst + 40, &pitch, 4);
    memcpy(src, &rt, sizeof(void *));
    memcpy(src + 8, &type_sub, 4);
    ((void(MS *)(void *, const void *, uint32_t, uint32_t, uint32_t, const void *,
                 const void *))V(list, 16))(list, dst, 0, 0, 0, src, NULL);

    CHECK(((HR(MS *)(void *))V(list, 9))(list) == 0, "close");

    void *lists[1] = { list };
    ((void(MS *)(void *, uint32_t, void **))V(queue, 10))(queue, 1, lists);
    CHECK(((HR(MS *)(void *, void *, uint64_t))V(queue, 14))(queue, fence, 1) == 0,
          "signal");
    CHECK(wait_fence(fence, 1), "fence wait");

    uint8_t *rb = NULL;
    CHECK(((HR(MS *)(void *, uint32_t, const void *, void **))V(readback, 8))(
              readback, 0, NULL, (void **)&rb) == 0 && rb, "map readback");
    if (rb) {
        int bad = 0;
        for (int i = 0; i < N; ++i)
            if (rb[i] != (uint8_t)(i * 7 + 3)) ++bad;
        CHECK(bad == 0, "buffer copy contents");
        const uint8_t *px = rb + N;
        int ok = 1;
        for (int y = 0; y < 64 && ok; ++y)
            for (int x = 0; x < 64; ++x) {
                const uint8_t *p = px + y * 256 + x * 4;
                if (abs(p[0] - 64) > 1 || abs(p[1] - 128) > 1 ||
                    abs(p[2] - 191) > 1 || p[3] != 255) { ok = 0; break; }
            }
        CHECK(ok, "render target clear contents");
        if (!ok) fprintf(stderr, "pixel0 = %u %u %u %u\n", px[0], px[1], px[2], px[3]);
    }

    /* Reuse: reset allocator and list, run a second submission. */
    CHECK(((HR(MS *)(void *))V(alloc, 8))(alloc) == 0, "allocator reset");
    CHECK(((HR(MS *)(void *, void *, void *))V(list, 10))(list, alloc, NULL) == 0,
          "list reset");
    ((void(MS *)(void *, void *, uint64_t, void *, uint64_t, uint64_t))V(list, 15))(
        list, readback, 0, upload, 0, 256);
    CHECK(((HR(MS *)(void *))V(list, 9))(list) == 0, "close 2");
    ((void(MS *)(void *, uint32_t, void **))V(queue, 10))(queue, 1, lists);
    ((HR(MS *)(void *, void *, uint64_t))V(queue, 14))(queue, fence, 2);
    CHECK(wait_fence(fence, 2), "fence wait 2");

    /* The recorder must retain earlier pending submissions when a later
     * signal-only submit overwrites the alternating output file. */
    setenv("BEER_BD12_FLIGHT_RECORDER", "/tmp", 1);
    g_com_device->flight[0] = (ComFlightSubmission){
        .serial = 16,
        .total_dispatch_count = 73,
        .dispatch_count = 1,
        .dispatches = {{ .shader_hash = UINT64_C(0x1234), .x = 7, .y = 8,
                         .z = 9, .loop_count = 1,
                         .writes_3d_storage_image = 1 }}
    };
    g_com_device->serial = 16;
    CHECK(com_submit(g_com_device, NULL, 0, NULL, 0, NULL, 0) == VK_SUCCESS,
          "flight recorder signal-only submit");
    FILE *flight = fopen("/tmp/beer-bd12-flight-1.log", "r");
    char flight_text[8192] = { 0 };
    CHECK(flight != NULL, "flight recorder output");
    if (flight) {
        const size_t flight_size =
            fread(flight_text, 1, sizeof(flight_text) - 1, flight);
        flight_text[flight_size] = '\0';
        fclose(flight);
    }
    CHECK(strstr(flight_text,
                 "submit=16 state=pending dispatches=1 total-dispatches=73") != NULL &&
          strstr(flight_text, "shader=0000000000001234") != NULL &&
          strstr(flight_text,
                 "submit=17 state=pending dispatches=0 total-dispatches=0") != NULL,
          "flight recorder preserves totals and dispatch history across empty submits");

    BD12CmdList flight_list = { 0 };
    bd12_cmd_record_flight_op(&flight_list, BD12_FLIGHT_DRAW_INDEXED,
                              UINT64_C(0x5678), 36, 12, 3, 0);
    g_com_device->flight[18 % COM_FLIGHT_SUBMISSIONS] = (ComFlightSubmission){
        .serial = 18, .total_op_count = flight_list.op_count, .op_count = 1,
        .ops = { flight_list.op_flight[0] }
    };
    g_com_device->serial = 18;
    CHECK(com_submit(g_com_device, NULL, 0, NULL, 0, NULL, 0) == VK_SUCCESS,
          "flight recorder graphics submit");
    flight = fopen("/tmp/beer-bd12-flight-1.log", "r");
    memset(flight_text, 0, sizeof(flight_text));
    CHECK(flight != NULL, "graphics flight recorder output");
    if (flight) {
        const size_t flight_size =
            fread(flight_text, 1, sizeof(flight_text) - 1, flight);
        flight_text[flight_size] = '\0';
        fclose(flight);
    }
    CHECK(strstr(flight_text, "type=draw-indexed pipeline=0000000000005678") != NULL &&
          strstr(flight_text, "amount=36 dims=12,3,0") != NULL,
          "flight recorder captures graphics metadata");
    remove("/tmp/beer-bd12-flight-0.log");
    remove("/tmp/beer-bd12-flight-1.log");
    unsetenv("BEER_BD12_FLIGHT_RECORDER");

    void *objs[] = { list, alloc, fence, rtv_heap, rt, readback, def, upload,
                     queue };
    for (size_t i = 0; i < sizeof(objs) / sizeof(objs[0]); ++i)
        ((uint32_t(MS *)(void *))V(objs[i], 2))(objs[i]);

    printf(failures ? "bd12 com test: FAIL (%d)\n" : "bd12 com test: PASS\n",
           failures);
    return failures ? 1 : 0;
}
