/* BD12 milestone 4 test: graphics pipeline, vertex/index buffers, root
 * constants, dynamic rendering, depth test, clears (including in-pass), indexed
 * and indirect draws, D3D y-up convention and clockwise-front culling.
 * Usage: bd12_m4_test <quad.vert.spv> <quad.frag.spv> */
#include "bd12.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__);          \
            return 1;                                                        \
        }                                                                    \
    } while (0)

#define YELLOW 0xff00ffffu
#define BLUE 0xffff0000u
#define RED 0xff0000ffu
#define GREEN 0xff00ff00u

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

typedef struct { float x, y, z, r, g, b, a; } Vertex;

static void quad(Vertex *v, float x0, float y0, float x1, float y1, float z)
{
    v[0] = (Vertex){ x0, y0, z, 1, 0, 1, 1 };
    v[1] = (Vertex){ x0, y1, z, 1, 0, 1, 1 };
    v[2] = (Vertex){ x1, y1, z, 1, 0, 1, 1 };
    v[3] = (Vertex){ x1, y0, z, 1, 0, 1, 1 };
}

static uint32_t expected(uint32_t x, uint32_t y)
{
    if (x < 8 && y < 8) return GREEN;
    if (x >= 16 && x < 48 && y < 32) return BLUE;
    if (x >= 48 && y >= 48) return RED;
    return YELLOW;
}

int main(int argc, char **argv)
{
    CHECK(argc >= 3, "usage: bd12_m4_test <vert.spv> <frag.spv>");
    size_t vs_bytes, ps_bytes;
    uint32_t *vs = read_file(argv[1], &vs_bytes);
    uint32_t *ps = read_file(argv[2], &ps_bytes);
    CHECK(vs && ps, "read spv");

    BD12Device *device = bd12_device_create();
    CHECK(device, "device create");
    BD12Queue *queue = bd12_queue_create(device);
    BD12Fence *fence = bd12_fence_create(device, 0);
    BD12CmdAllocator *allocator = bd12_cmd_allocator_create(device);
    BD12CmdList *list = bd12_cmd_list_create(allocator);
    CHECK(queue && fence && allocator && list, "object creation");

    BD12RootParameter param = { .type = BD12_ROOT_CONSTANTS,
                                .shader_register = 0, .dword_count = 4 };
    BD12RootSignature *rs = bd12_root_signature_create(device, &param, 1);
    CHECK(rs, "root signature");

    const BD12ShaderBinding ps_bindings[] = {
        { BD12_RANGE_CBV, 0, 0, 1, 1, 4 }
    };
    const BD12VertexAttribute attributes[] = {
        { 0, 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },
        { 1, 0, 12, VK_FORMAT_R32G32B32A32_SFLOAT, 0 }
    };
    BD12GraphicsPipelineDesc desc = {
        .vs = { vs, vs_bytes, NULL, 0 },
        .ps = { ps, ps_bytes, ps_bindings, 1 },
        .attributes = attributes,
        .attribute_count = 2,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .fill_mode = VK_POLYGON_MODE_FILL,
        .cull_mode = VK_CULL_MODE_BACK_BIT,
        .front_ccw = 0,
        .depth_clip_enable = 1,
        .depth_test = 1, .depth_write = 1, .depth_func = VK_COMPARE_OP_LESS,
        .rt_count = 1,
        .rt_formats = { VK_FORMAT_R8G8B8A8_UNORM },
        .ds_format = VK_FORMAT_D32_SFLOAT,
        .samples = 1
    };
    desc.blend[0].write_mask = 0xf;
    BD12GraphicsPipeline *pso = bd12_graphics_pipeline_create(device, &desc);
    CHECK(pso, "graphics pipeline");

    BD12ImageDesc rt_desc = { .type = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .width = 64, .height = 64,
        .depth_or_layers = 1, .mip_levels = 1, .samples = 1,
        .allow_render_target = 1 };
    BD12ImageDesc ds_desc = { .type = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_D32_SFLOAT, .width = 64, .height = 64,
        .depth_or_layers = 1, .mip_levels = 1, .samples = 1,
        .allow_depth_stencil = 1 };
    BD12Image *rt = bd12_image_create_committed(device, &rt_desc, BD12_STATE_COMMON);
    BD12Image *ds = bd12_image_create_committed(device, &ds_desc, BD12_STATE_COMMON);
    CHECK(rt && ds, "images");

    BD12DescriptorHeap *rtv_heap = bd12_descriptor_heap_create(device, 0, 4);
    BD12DescriptorHeap *dsv_heap = bd12_descriptor_heap_create(device, 0, 4);
    CHECK(rtv_heap && dsv_heap, "heaps");
    CHECK(bd12_descriptor_write_rtv(rtv_heap, 0, rt, VK_FORMAT_UNDEFINED, 0, 0, 1), "rtv");
    CHECK(bd12_descriptor_write_dsv(dsv_heap, 0, ds, VK_FORMAT_UNDEFINED, 0, 0, 1, 0, 0), "dsv");

    BD12Buffer *upload = bd12_buffer_create_committed(device, BD12_HEAP_UPLOAD, 4096);
    const uint32_t rb_pitch = 256;
    BD12Buffer *rb = bd12_buffer_create_committed(device, BD12_HEAP_READBACK, rb_pitch * 64);
    CHECK(upload && rb, "buffers");
    Vertex *vertices = (Vertex *)upload->cpu;
    quad(vertices + 0, -0.5f, 0.0f, 0.5f, 1.0f, 0.5f);   /* A: top middle */
    quad(vertices + 4, 0.5f, -1.0f, 1.0f, -0.5f, 0.25f); /* B: bottom right */
    quad(vertices + 8, -0.5f, 0.0f, 0.5f, 1.0f, 0.75f);  /* C: behind A */
    uint16_t *indices = (uint16_t *)((char *)upload->cpu + 512);
    const uint16_t index_data[6] = { 0, 1, 2, 0, 2, 3 };
    memcpy(indices, index_data, sizeof(index_data));
    uint32_t *args = (uint32_t *)((char *)upload->cpu + 1024);
    args[0] = 6; args[1] = 1; args[2] = 0; args[3] = 8; args[4] = 0;
    memset(rb->cpu, 0xcd, rb_pitch * 64);

    const float tint_a[4] = { 0.0f, 1.0f, 1.0f, 1.0f };
    const float tint_b[4] = { 1.0f, 1.0f, 0.0f, 1.0f };
    const float yellow[4] = { 1.0f, 1.0f, 0.0f, 1.0f };
    const float green[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
    uint32_t tint_bits[4];

    CHECK(bd12_cmd_list_begin(list), "begin");
    bd12_cmd_transition_image(list, rt, BD12_ALL_SUBRESOURCES, BD12_STATE_COMMON,
                              BD12_STATE_RENDER_TARGET);
    bd12_cmd_transition_image(list, ds, BD12_ALL_SUBRESOURCES, BD12_STATE_COMMON,
                              BD12_STATE_DEPTH_WRITE);
    bd12_cmd_set_graphics_root_signature(list, rs);
    bd12_cmd_set_graphics_pipeline(list, pso);
    const BD12Descriptor *rtvs[1] = { &rtv_heap->descriptors[0] };
    bd12_cmd_set_render_targets(list, rtvs, 1, &dsv_heap->descriptors[0]);
    VkViewport viewport = { 0, 0, 64, 64, 0.0f, 1.0f };
    VkRect2D scissor = { { 0, 0 }, { 64, 64 } };
    bd12_cmd_set_viewports(list, &viewport, 1);
    bd12_cmd_set_scissors(list, &scissor, 1);
    bd12_cmd_set_topology(list, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    const uint64_t vb_va = upload->va;
    const uint32_t vb_size = 12 * sizeof(Vertex), vb_stride = sizeof(Vertex);
    bd12_cmd_set_vertex_buffers(list, 0, &vb_va, &vb_size, &vb_stride, 1);
    bd12_cmd_set_index_buffer(list, upload->va + 512, 12, VK_INDEX_TYPE_UINT16);

    bd12_cmd_clear_rtv(list, rtvs[0], yellow, 0, NULL);
    bd12_cmd_clear_dsv(list, &dsv_heap->descriptors[0],
                       VK_IMAGE_ASPECT_DEPTH_BIT, 1.0f, 0, 0, NULL);

    memcpy(tint_bits, tint_a, sizeof(tint_bits));
    bd12_cmd_set_graphics_root_constants(list, 0, tint_bits, 4, 0);
    CHECK(bd12_cmd_draw_indexed(list, 6, 1, 0, 0, 0), "draw A");
    memcpy(tint_bits, tint_b, sizeof(tint_bits));
    bd12_cmd_set_graphics_root_constants(list, 0, tint_bits, 4, 0);
    CHECK(bd12_cmd_draw_indexed(list, 6, 1, 0, 4, 0), "draw B");
    CHECK(bd12_cmd_draw_indexed_indirect(list, upload, 1024, 1, 20), "draw C indirect");
    const VkRect2D corner = { { 0, 0 }, { 8, 8 } };
    bd12_cmd_clear_rtv(list, rtvs[0], green, 1, &corner);   /* in-pass clear */

    bd12_cmd_transition_image(list, rt, BD12_ALL_SUBRESOURCES,
                              BD12_STATE_RENDER_TARGET, BD12_STATE_COPY_SOURCE);
    bd12_cmd_copy_image_to_buffer(list, rb, 0, rb_pitch, rt, (BD12ImagePoint){ 0 },
                                  64, 64, 1);
    bd12_cmd_host_visibility(list, rb);
    CHECK(bd12_cmd_list_end(list), "end");

    BD12CmdList *lists[] = { list };
    BD12FenceOp signal = { fence, 1 };
    CHECK(bd12_queue_submit(queue, lists, 1, NULL, 0, &signal, 1) == VK_SUCCESS, "submit");
    CHECK(bd12_fence_wait(fence, 1, 5000000000ull), "fence wait");
    CHECK(queue->submit_count == 1, "one submit");

    uint32_t bad = 0;
    for (uint32_t y = 0; y < 64; ++y) {
        const uint32_t *row = (const uint32_t *)((char *)rb->cpu + y * rb_pitch);
        for (uint32_t x = 0; x < 64; ++x) {
            if (row[x] != expected(x, y)) {
                if (bad++ < 8)
                    fprintf(stderr, "mismatch (%u,%u) got %08x want %08x\n", x, y,
                            row[x], expected(x, y));
            }
        }
    }
    CHECK(!bad, "pixel mismatches");
    printf("PASS correctness: clear, 3 draws (indexed, base vertex, indirect), depth reject, in-pass clear, y-up + CW-front, 1 submit\n");

    /* Record-time cost: 100 draws per list with changing root constants. */
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
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int n = 0; n < LISTS; ++n) {
        int slot = n % RING;
        if (ring_v[slot]) CHECK(bd12_fence_wait(fence, ring_v[slot], 30000000000ull), "ring wait");
        CHECK(bd12_cmd_allocator_reset(ring_a[slot]), "reset");
        BD12CmdList *l = ring_l[slot];
        CHECK(bd12_cmd_list_begin(l), "begin loop");
        bd12_cmd_transition_image(l, rt, BD12_ALL_SUBRESOURCES,
                                  BD12_STATE_COPY_SOURCE,
                                  BD12_STATE_RENDER_TARGET);
        bd12_cmd_set_graphics_root_signature(l, rs);
        bd12_cmd_set_graphics_pipeline(l, pso);
        bd12_cmd_set_render_targets(l, rtvs, 1, &dsv_heap->descriptors[0]);
        bd12_cmd_set_viewports(l, &viewport, 1);
        bd12_cmd_set_scissors(l, &scissor, 1);
        bd12_cmd_set_topology(l, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        bd12_cmd_set_vertex_buffers(l, 0, &vb_va, &vb_size, &vb_stride, 1);
        bd12_cmd_set_index_buffer(l, upload->va + 512, 12, VK_INDEX_TYPE_UINT16);
        for (int k = 0; k < PER_LIST; ++k) {
            const float tint[4] = { (float)k / PER_LIST, 1.0f, 1.0f, 1.0f };
            memcpy(tint_bits, tint, sizeof(tint_bits));
            bd12_cmd_set_graphics_root_constants(l, 0, tint_bits, 4, 0);
            CHECK(bd12_cmd_draw_indexed(l, 6, 1, 0, 4, 0), "loop draw");
        }
        bd12_cmd_transition_image(l, rt, BD12_ALL_SUBRESOURCES,
                                  BD12_STATE_RENDER_TARGET, BD12_STATE_COPY_SOURCE);
        CHECK(bd12_cmd_list_end(l), "end loop");
        BD12FenceOp s = { fence, ++value };
        BD12CmdList *sl[] = { l };
        CHECK(bd12_queue_submit(queue, sl, 1, NULL, 0, &s, 1) == VK_SUCCESS, "submit loop");
        ring_v[slot] = value;
    }
    CHECK(bd12_fence_wait(fence, value, 60000000000ull), "loop wait");
    clock_gettime(CLOCK_MONOTONIC, &t1);
    const double ms = (double)(t1.tv_sec - t0.tv_sec) * 1e3 +
                      (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
    printf("PASS throughput: %d draws in %.1f ms (%.2f us each incl. descriptor resolve)\n",
           LISTS * PER_LIST, ms, ms * 1e3 / (LISTS * PER_LIST));

    for (int i = 0; i < RING; ++i) {
        bd12_cmd_list_destroy(ring_l[i], ring_a[i]);
        bd12_cmd_allocator_destroy(ring_a[i]);
    }
    bd12_descriptor_heap_destroy(rtv_heap);
    bd12_descriptor_heap_destroy(dsv_heap);
    bd12_image_destroy(rt); bd12_image_destroy(ds);
    bd12_buffer_destroy(upload); bd12_buffer_destroy(rb);
    bd12_graphics_pipeline_destroy(pso);
    bd12_root_signature_destroy(rs);
    bd12_cmd_list_destroy(list, allocator);
    bd12_cmd_allocator_destroy(allocator);
    bd12_fence_destroy(fence);
    bd12_queue_destroy(queue);
    bd12_device_destroy(device);
    free(vs); free(ps);
    printf("M4 PASS\n");
    return 0;
}
