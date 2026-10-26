/* BD12 acceleration structure test: builds a BLAS and TLAS through the same
 * entry points DXR commands use, then traces two rays with a ray query
 * compute shader. Usage: bd12_rt_test <rt_trace.spv> */
#include "bd12.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__);          \
            return 1;                                                        \
        }                                                                    \
    } while (0)

uint64_t g_beer_vm_live_bytes; /* normally defined by the loader */
typedef struct { uint64_t va, stride; } VaStride;
typedef struct {
    uint32_t type, flags;
    struct {
        uint64_t transform;
        uint32_t index_format, vertex_format, index_count, vertex_count;
        uint64_t index_buffer;
        VaStride vertex;
    } tri;
} Geometry;
typedef struct { uint32_t type, flags, num_descs, layout; uint64_t descs; } Inputs;
typedef struct { uint64_t dest; Inputs inputs; uint64_t source, scratch; } BuildDesc;
typedef struct { uint64_t tlas; uint64_t out; } Push;

static uint32_t *read_file(const char *path, size_t *bytes)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *data = malloc((size_t)n);
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n) { free(data); data = NULL; }
    fclose(f);
    *bytes = (size_t)n;
    return data;
}

int main(int argc, char **argv)
{
    CHECK(argc >= 2, "usage: bd12_rt_test <spv>");
    size_t spv_bytes;
    uint32_t *spv = read_file(argv[1], &spv_bytes);
    CHECK(spv, "read spv");
    BD12Device *device = bd12_device_create();
    CHECK(device, "device create");
    if (!device->rt_enabled) { printf("SKIP: ray tracing unavailable\n"); return 0; }
    BD12Queue *queue = bd12_queue_create(device);
    BD12Fence *fence = bd12_fence_create(device, 0);
    BD12CmdAllocator *allocator = bd12_cmd_allocator_create(device);
    BD12CmdList *list = bd12_cmd_list_create(allocator);
    BD12ComputePipeline *pipeline =
        bd12_compute_pipeline_create(device, spv, spv_bytes, sizeof(Push));
    CHECK(queue && fence && allocator && list && pipeline, "object creation");

    const float verts[9] = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    BD12Buffer *upload = bd12_buffer_create_committed(device, BD12_HEAP_UPLOAD, 4096);
    BD12Buffer *vertex = bd12_buffer_create_committed(device, BD12_HEAP_DEFAULT, 4096);
    BD12Buffer *instances = bd12_buffer_create_committed(device, BD12_HEAP_DEFAULT, 4096);
    BD12Buffer *out = bd12_buffer_create_committed(device, BD12_HEAP_DEFAULT, 4096);
    BD12Buffer *readback = bd12_buffer_create_committed(device, BD12_HEAP_READBACK, 4096);
    CHECK(upload && vertex && instances && out && readback, "buffers");
    memcpy(upload->cpu, verts, sizeof(verts));
    uint8_t *inst = (uint8_t *)upload->cpu + 256;
    memset(inst, 0, 64);
    const float identity[12] = { 1,0,0,0, 0,1,0,0, 0,0,1,0 };
    memcpy(inst, identity, sizeof(identity));
    memset(readback->cpu, 0xcd, 4096);

    Geometry geometry = { .type = 0, .flags = 1 };
    geometry.tri.vertex_format = 6;           /* R32G32B32_FLOAT */
    geometry.tri.vertex_count = 3;
    geometry.tri.vertex.va = vertex->va;
    geometry.tri.vertex.stride = 12;
    Inputs blas_in = { .type = 1, .flags = 4, .num_descs = 1, .descs = (uintptr_t)&geometry };
    uint64_t blas_sizes[3], tlas_sizes[3];
    CHECK(bd12_rt_prebuild(device, &blas_in, blas_sizes), "blas prebuild");
    Inputs tlas_in = { .type = 0, .flags = 4, .num_descs = 1, .descs = instances->va };
    CHECK(bd12_rt_prebuild(device, &tlas_in, tlas_sizes), "tlas prebuild");
    printf("blas: result=%llu scratch=%llu  tlas: result=%llu scratch=%llu\n",
           (unsigned long long)blas_sizes[0], (unsigned long long)blas_sizes[1],
           (unsigned long long)tlas_sizes[0], (unsigned long long)tlas_sizes[1]);
    CHECK(blas_sizes[0] && tlas_sizes[0], "nonzero sizes");

    BD12Buffer *blas = bd12_buffer_create_committed(device, BD12_HEAP_DEFAULT, blas_sizes[0]);
    BD12Buffer *tlas = bd12_buffer_create_committed(device, BD12_HEAP_DEFAULT, tlas_sizes[0]);
    BD12Buffer *scratch = bd12_buffer_create_committed(device, BD12_HEAP_DEFAULT,
        (blas_sizes[1] > tlas_sizes[1] ? blas_sizes[1] : tlas_sizes[1]) + 256);
    CHECK(blas && tlas && scratch, "as buffers");
    CHECK((blas->va & 255) == 0 && (tlas->va & 255) == 0 && (scratch->va & 255) == 0,
          "256-byte aligned VAs");
    memcpy(inst + 56, &blas->va, 8);          /* instance -> BLAS by VA */
    inst[51] = 0xff;                          /* InstanceMask */

    BuildDesc blas_build = { .dest = blas->va, .inputs = blas_in, .scratch = scratch->va };
    BuildDesc tlas_build = { .dest = tlas->va, .inputs = tlas_in, .scratch = scratch->va };
    Push push = { tlas->va, out->va };

    CHECK(bd12_cmd_list_begin(list), "begin");
    bd12_cmd_copy_buffer(list, vertex, 0, upload, 0, 36);
    bd12_cmd_copy_buffer(list, instances, 0, upload, 256, 64);
    bd12_cmd_transition_buffer(list, out, BD12_STATE_COMMON, BD12_STATE_UNORDERED_ACCESS);
    bd12_cmd_build_as(list, &blas_build);
    bd12_cmd_build_as(list, &tlas_build);
    bd12_cmd_dispatch(list, pipeline, &push, 1, 1, 1);
    bd12_cmd_transition_buffer(list, out, BD12_STATE_UNORDERED_ACCESS, BD12_STATE_COPY_SOURCE);
    bd12_cmd_copy_buffer(list, readback, 0, out, 0, 16);
    bd12_cmd_host_visibility(list, readback);
    CHECK(bd12_cmd_list_end(list), "end");
    BD12CmdList *lists[] = { list };
    BD12FenceOp signal = { fence, 1 };
    CHECK(bd12_queue_submit(queue, lists, 1, NULL, 0, &signal, 1) == VK_SUCCESS, "submit");
    CHECK(bd12_fence_wait(fence, 1, 10000000000ull), "fence wait");

    const uint32_t *r = readback->cpu;
    float t;
    memcpy(&t, &r[1], 4);
    printf("ray0 hit=%u t=%f   ray1 hit=%u\n", r[0], (double)t, r[2]);
    CHECK(r[0] == 1, "ray through the triangle hits");
    CHECK(t > 0.99f && t < 1.01f, "hit distance is 1");
    CHECK(r[2] == 0, "ray beside the triangle misses");
    printf("PASS: BLAS+TLAS build and ray query\n");
    return 0;
}
