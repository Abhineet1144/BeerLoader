/* BD12 milestone 2 test: root signature (descriptor table + root CBV + root
 * constants), CPU descriptor heap, record-time descriptor resolution, null
 * descriptors, and per-allocator descriptor/constant recycling.
 * Usage: bd12_m2_test <m2_bound.spv> */
#include "bd12.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define N 4096u
#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__);          \
            return 1;                                                        \
        }                                                                    \
    } while (0)

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static uint32_t *read_file(const char *path, size_t *bytes)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *data = malloc((size_t)n);
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(f);
    *bytes = (size_t)n;
    return data;
}

int main(int argc, char **argv)
{
    CHECK(argc >= 2, "usage: bd12_m2_test <spv>");
    size_t spv_bytes;
    uint32_t *spv = read_file(argv[1], &spv_bytes);
    CHECK(spv, "read spv");

    BD12Device *device = bd12_device_create();
    CHECK(device, "device create");
    BD12Queue *queue = bd12_queue_create(device);
    BD12Fence *fence = bd12_fence_create(device, 0);
    BD12CmdAllocator *allocator = bd12_cmd_allocator_create(device);
    BD12CmdList *list = bd12_cmd_list_create(allocator);
    CHECK(queue && fence && allocator && list, "object creation");

    /* Root signature: [0] table {SRV t0..t1, UAV u0}, [1] root CBV b0,
     * [2] four root constants b1. */
    BD12DescriptorRange ranges[2] = {
        { BD12_RANGE_SRV, 2, 0, 0, 0 },
        { BD12_RANGE_UAV, 1, 0, 0, BD12_RANGE_APPEND }
    };
    BD12RootParameter params[3] = {
        { .type = BD12_ROOT_TABLE, .ranges = ranges, .range_count = 2 },
        { .type = BD12_ROOT_CBV, .shader_register = 0 },
        { .type = BD12_ROOT_CONSTANTS, .shader_register = 1, .dword_count = 4 }
    };
    BD12RootSignature *rs = bd12_root_signature_create(device, params, 3);
    CHECK(rs, "root signature");

    BD12ShaderBinding bindings[] = {
        { BD12_RANGE_SRV, 0, 0, 1, 1, 0 },
        { BD12_RANGE_SRV, 0, 1, 1, 1, 1 },
        { BD12_RANGE_SRV, 0, 5, 1, 1, 5 },
        { BD12_RANGE_UAV, 0, 0, 1, 1, 16384 },
        { BD12_RANGE_CBV, 0, 0, 1, 1, 32768 },
        { BD12_RANGE_CBV, 0, 1, 1, 1, 32769 }
    };
    BD12ComputePipeline *pso = bd12_compute_pipeline_create_bound(
        device, spv, spv_bytes, bindings, 6);
    CHECK(pso, "bound pipeline");
    /* RE8's full-screen setup hash is completed as one whole dispatch. This is
     * a scheduling boundary, not partial-dispatch tiling. */
    pso->shader_hash = UINT64_C(0x9aaf1cbbe609609a);

    const VkDeviceSize bytes = N * 4u;
    BD12Buffer *upload = bd12_buffer_create_committed(device, BD12_HEAP_UPLOAD, 3 * bytes + 4096);
    BD12Buffer *a = bd12_buffer_create_committed(device, BD12_HEAP_DEFAULT, bytes);
    BD12Buffer *b = bd12_buffer_create_committed(device, BD12_HEAP_DEFAULT, bytes);
    BD12Buffer *out1 = bd12_buffer_create_committed(device, BD12_HEAP_DEFAULT, bytes);
    BD12Buffer *out2 = bd12_buffer_create_committed(device, BD12_HEAP_DEFAULT, bytes);
    BD12Buffer *cb = bd12_buffer_create_committed(device, BD12_HEAP_DEFAULT, 4096);
    BD12Buffer *rb = bd12_buffer_create_committed(device, BD12_HEAP_READBACK, 2 * bytes);
    CHECK(upload && a && b && out1 && out2 && cb && rb, "buffers");

    uint32_t *u = upload->cpu;
    for (uint32_t i = 0; i < N; ++i) { u[i] = i; u[N + i] = 1000 + 2 * i; }
    u[2 * N] = 0;                                  /* unused */
    uint32_t *ucb = (uint32_t *)((char *)upload->cpu + 3 * bytes);
    ucb[64] = 3;                                   /* mul at cb offset 256 */

    BD12DescriptorHeap *heap = bd12_descriptor_heap_create(device, 0, 16);
    CHECK(heap, "descriptor heap");
    CHECK(bd12_descriptor_write_buffer_srv(heap, 5, a, 0, bytes, VK_FORMAT_R32_UINT), "srv a");
    CHECK(bd12_descriptor_write_buffer_srv(heap, 6, b, 0, bytes, VK_FORMAT_R32_UINT), "srv b");
    CHECK(bd12_descriptor_write_buffer_uav(heap, 7, out1, 0, bytes, VK_FORMAT_R32_UINT), "uav out1");
    CHECK(bd12_descriptor_write_buffer_srv(heap, 8, b, 0, bytes, VK_FORMAT_R32_UINT), "srv b2");
    CHECK(bd12_descriptor_write_buffer_srv(heap, 9, a, 0, bytes, VK_FORMAT_R32_UINT), "srv a2");
    CHECK(bd12_descriptor_write_buffer_uav(heap, 10, out2, 0, bytes, VK_FORMAT_R32_UINT), "uav out2");
    /* Heap-to-heap copy path (D3D12 CopyDescriptors). */
    BD12DescriptorHeap *staging = bd12_descriptor_heap_create(device, 0, 4);
    CHECK(bd12_descriptor_write_buffer_srv(staging, 0, a, 0, bytes, VK_FORMAT_R32_UINT), "staging");
    bd12_descriptor_copy(heap, 11, staging, 0, 1);
    CHECK(heap->descriptors[11].kind == BD12_DESC_SRV_BUFFER, "descriptor copy");
    /* D3D typed views need only texel alignment. NVIDIA's core limit is
     * commonly 16 bytes, while VK_EXT_texel_buffer_alignment permits this
     * valid R32 view at an offset aligned to one four-byte texel. */
    CHECK(bd12_descriptor_write_buffer_srv(heap, 12, upload, 8, 16,
                                            VK_FORMAT_R32_UINT),
          "single-texel-aligned SRV offset");

    memset(rb->cpu, 0xcd, 2 * bytes);
    CHECK(bd12_cmd_list_begin(list), "begin");
    bd12_cmd_copy_buffer(list, a, 0, upload, 0, bytes);
    bd12_cmd_copy_buffer(list, b, 0, upload, bytes, bytes);
    bd12_cmd_copy_buffer(list, cb, 0, upload, 3 * bytes, 4096);
    bd12_cmd_transition_buffer(list, a, BD12_STATE_COPY_DEST, BD12_STATE_NON_PIXEL_SHADER_RESOURCE);
    bd12_cmd_transition_buffer(list, b, BD12_STATE_COPY_DEST, BD12_STATE_NON_PIXEL_SHADER_RESOURCE);
    bd12_cmd_transition_buffer(list, cb, BD12_STATE_COPY_DEST, BD12_STATE_VERTEX_AND_CONSTANT_BUFFER);
    bd12_cmd_transition_buffer(list, out1, BD12_STATE_COMMON, BD12_STATE_UNORDERED_ACCESS);
    bd12_cmd_transition_buffer(list, out2, BD12_STATE_COMMON, BD12_STATE_UNORDERED_ACCESS);

    bd12_cmd_set_descriptor_heaps(list, heap, NULL);
    bd12_cmd_set_compute_root_signature(list, rs);
    bd12_cmd_set_pipeline(list, pso);
    bd12_cmd_set_compute_root_va(list, 1, cb->va + 256);
    uint32_t add1[4] = { 7, 0, 0, 0 };
    bd12_cmd_set_compute_root_constants(list, 2, add1, 4, 0);
    bd12_cmd_set_compute_root_table(list, 0, bd12_descriptor_gpu_handle(heap, 5));
    CHECK(bd12_cmd_dispatch_bound(list, N / 64, 1, 1), "dispatch 1");
    uint32_t add2[4] = { 100, 0, 0, 0 };
    bd12_cmd_set_compute_root_constants(list, 2, add2, 4, 0);
    bd12_cmd_set_compute_root_table(list, 0, bd12_descriptor_gpu_handle(heap, 8));
    CHECK(bd12_cmd_dispatch_bound(list, N / 64, 1, 1), "dispatch 2");
    CHECK(list->dispatch_count == 2, "flight recorder dispatch count");
    CHECK(list->dispatch_flight[1].x == N / 64 &&
          list->dispatch_flight[1].y == 1 &&
          list->dispatch_flight[1].z == 1 &&
          list->dispatch_flight[1].segment == 0 &&
          !list->dispatch_flight[1].indirect,
          "flight recorder direct dimensions and segment");

    bd12_cmd_transition_buffer(list, out1, BD12_STATE_UNORDERED_ACCESS, BD12_STATE_COPY_SOURCE);
    bd12_cmd_transition_buffer(list, out2, BD12_STATE_UNORDERED_ACCESS, BD12_STATE_COPY_SOURCE);
    bd12_cmd_copy_buffer(list, rb, 0, out1, 0, bytes);
    bd12_cmd_copy_buffer(list, rb, bytes, out2, 0, bytes);
    bd12_cmd_host_visibility(list, rb);
    CHECK(bd12_cmd_list_end(list), "end");

    CHECK(list->command_buffer_count == 1,
          "bound dispatches stay in one command buffer");
    {
        BD12CmdList *ls[] = { list };
        BD12FenceOp sig = { fence, 1 };
        CHECK(bd12_queue_submit(queue, ls, 1, NULL, 0, &sig, 1) == VK_SUCCESS,
              "submit");
        CHECK(bd12_fence_wait(fence, 1, 30000000000ull), "wait");
        CHECK(queue->submit_count == 1, "one submission for both dispatches");
    }
    const uint32_t *r1 = rb->cpu, *r2 = (const uint32_t *)((char *)rb->cpu + bytes);
    for (uint32_t i = 0; i < N; ++i) {
        uint32_t av = i, bv = 1000 + 2 * i;
        if (r1[i] != av * 3 + 7 + bv || r2[i] != bv * 3 + 100 + av) {
            fprintf(stderr, "FAIL: i=%u out1=%u (want %u) out2=%u (want %u)\n", i,
                    r1[i], av * 3 + 7 + bv, r2[i], bv * 3 + 100 + av);
            return 1;
        }
    }
    printf("PASS correctness: table+root CBV+root constants, 2 dispatches, null descriptor\n");

    /* Record-time cost: 100 bound dispatches per list with changing root
     * constants, ring of 4 allocators recycled by fence value. */
    enum { RING = 4, PER_LIST = 100, LISTS = 50 };
    BD12CmdAllocator *ring_a[RING];
    BD12CmdList *ring_l[RING];
    uint64_t ring_v[RING] = { 0 };
    for (int i = 0; i < RING; ++i) {
        ring_a[i] = bd12_cmd_allocator_create(device);
        ring_l[i] = bd12_cmd_list_create(ring_a[i]);
        CHECK(ring_a[i] && ring_l[i], "ring");
    }
    uint64_t value = 1;
    uint64_t t0 = now_ns();
    for (int n = 0; n < LISTS; ++n) {
        int slot = n % RING;
        if (ring_v[slot]) CHECK(bd12_fence_wait(fence, ring_v[slot], 30000000000ull), "ring wait");
        CHECK(bd12_cmd_allocator_reset(ring_a[slot]), "reset");
        BD12CmdList *l = ring_l[slot];
        CHECK(bd12_cmd_list_begin(l), "begin loop");
        bd12_cmd_set_descriptor_heaps(l, heap, NULL);
        bd12_cmd_set_compute_root_signature(l, rs);
        bd12_cmd_set_pipeline(l, pso);
        bd12_cmd_set_compute_root_va(l, 1, cb->va + 256);
        bd12_cmd_set_compute_root_table(l, 0, bd12_descriptor_gpu_handle(heap, 5));
        for (int k = 0; k < PER_LIST; ++k) {
            uint32_t add[4] = { (uint32_t)k, 0, 0, 0 };
            bd12_cmd_set_compute_root_constants(l, 2, add, 4, 0);
            CHECK(bd12_cmd_dispatch_bound(l, N / 64, 1, 1), "loop dispatch");
        }
        CHECK(bd12_cmd_list_end(l), "end loop");
        CHECK(l->command_buffer_count == 1,
              "ordinary compute remains on fast path");
        BD12FenceOp s = { fence, ++value };
        BD12CmdList *sl[] = { l };
        CHECK(bd12_queue_submit(queue, sl, 1, NULL, 0, &s, 1) == VK_SUCCESS, "submit loop");
        ring_v[slot] = value;
    }
    CHECK(bd12_fence_wait(fence, value, 60000000000ull), "loop wait");
    uint64_t t1 = now_ns();
    printf("PASS throughput: %d bound dispatches in %.1f ms (%.2f us each incl. descriptor resolve)\n",
           LISTS * PER_LIST, (double)(t1 - t0) / 1e6,
           (double)(t1 - t0) / 1e3 / (LISTS * PER_LIST));

    for (int i = 0; i < RING; ++i) {
        bd12_cmd_list_destroy(ring_l[i], ring_a[i]);
        bd12_cmd_allocator_destroy(ring_a[i]);
    }
    bd12_descriptor_heap_destroy(staging);
    bd12_descriptor_heap_destroy(heap);
    bd12_buffer_destroy(upload); bd12_buffer_destroy(a); bd12_buffer_destroy(b);
    bd12_buffer_destroy(out1); bd12_buffer_destroy(out2); bd12_buffer_destroy(cb);
    bd12_buffer_destroy(rb);
    bd12_compute_pipeline_destroy(pso);
    bd12_root_signature_destroy(rs);
    bd12_cmd_list_destroy(list, allocator);
    bd12_cmd_allocator_destroy(allocator);
    bd12_fence_destroy(fence);
    bd12_queue_destroy(queue);
    bd12_device_destroy(device);
    free(spv);
    printf("M2 PASS\n");
    return 0;
}
