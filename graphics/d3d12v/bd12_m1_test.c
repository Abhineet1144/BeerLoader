/* BD12 milestone 1 test: upload -> one VA-addressed dispatch -> readback,
 * with exactly one vkQueueSubmit2 for the whole sequence. Runs without any
 * game. Usage: bd12_m1_test <m1_va_double.spv> */
#include "bd12.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define COUNT 65536u
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

typedef struct {
    uint64_t src;
    uint64_t dst;
    uint32_t count;
    uint32_t pad;
} Push;

int main(int argc, char **argv)
{
    CHECK(argc >= 2, "usage: bd12_m1_test <spv>");
    size_t spv_bytes;
    uint32_t *spv = read_file(argv[1], &spv_bytes);
    CHECK(spv, "read spv");

    BD12Device *device = bd12_device_create();
    CHECK(device, "device create");
    BD12Queue *queue = bd12_queue_create(device);
    BD12Fence *fence = bd12_fence_create(device, 0);
    BD12CmdAllocator *allocator = bd12_cmd_allocator_create(device);
    BD12CmdList *list = bd12_cmd_list_create(allocator);
    BD12ComputePipeline *pipeline =
        bd12_compute_pipeline_create(device, spv, spv_bytes, sizeof(Push));
    CHECK(queue && fence && allocator && list && pipeline, "object creation");

    const VkDeviceSize bytes = COUNT * sizeof(uint32_t);
    BD12Buffer *upload = bd12_buffer_create_committed(device, BD12_HEAP_UPLOAD, bytes);
    BD12Buffer *input = bd12_buffer_create_committed(device, BD12_HEAP_DEFAULT, bytes);
    BD12Buffer *output = bd12_buffer_create_committed(device, BD12_HEAP_DEFAULT, bytes);
    BD12Buffer *readback = bd12_buffer_create_committed(device, BD12_HEAP_READBACK, bytes);
    CHECK(upload && input && output && readback, "buffer creation");
    CHECK(upload->cpu && readback->cpu && !input->cpu, "mapping policy");

    /* GPU VA table: every VA, interior and boundary, resolves correctly. */
    VkDeviceSize off = 0;
    CHECK(bd12_va_resolve(device, input->va, &off) == input && off == 0,
          "va resolve start");
    CHECK(bd12_va_resolve(device, input->va + 1234, &off) == input && off == 1234,
          "va resolve interior");
    CHECK(bd12_va_resolve(device, input->va + bytes - 1, &off) == input,
          "va resolve last byte");
    CHECK(bd12_va_resolve(device, output->va, &off) == output, "va resolve output");
    CHECK(bd12_va_resolve(device, 0x10, &off) == NULL, "va resolve miss");

    uint32_t *src = upload->cpu;
    for (uint32_t i = 0; i < COUNT; ++i) src[i] = i;
    memset(readback->cpu, 0xcd, bytes);

    Push push = { input->va, output->va, COUNT, 0 };
    CHECK(bd12_cmd_list_begin(list), "begin");
    bd12_cmd_copy_buffer(list, input, 0, upload, 0, bytes);
    bd12_cmd_transition_buffer(list, input, BD12_STATE_COPY_DEST,
                               BD12_STATE_NON_PIXEL_SHADER_RESOURCE);
    bd12_cmd_transition_buffer(list, output, BD12_STATE_COMMON,
                               BD12_STATE_UNORDERED_ACCESS);
    bd12_cmd_dispatch(list, pipeline, &push, (COUNT + 63) / 64, 1, 1);
    bd12_cmd_transition_buffer(list, output, BD12_STATE_UNORDERED_ACCESS,
                               BD12_STATE_COPY_SOURCE);
    bd12_cmd_copy_buffer(list, readback, 0, output, 0, bytes);
    bd12_cmd_host_visibility(list, readback);
    CHECK(bd12_cmd_list_end(list), "end");

    BD12CmdList *lists[] = { list };
    BD12FenceOp signal = { fence, 1 };
    CHECK(bd12_queue_submit(queue, lists, 1, NULL, 0, &signal, 1) == VK_SUCCESS,
          "submit");
    CHECK(bd12_fence_wait(fence, 1, 5000000000ull), "fence wait");
    CHECK(bd12_fence_completed(fence) == 1, "fence value");
    CHECK(queue->submit_count == 1, "exactly one submit for the sequence");

    /* A split/watchdog submit must carry the previous queue timeline as a
     * device-side wait. Host completion checks do not establish Vulkan memory
     * visibility between separate submissions. Re-run the compute command via
     * the watchdog helper after an ordinary copy submission and verify that it
     * observes the copied input. */
    for (uint32_t i = 0; i < COUNT; ++i) src[i] = i + 1000u;
    CHECK(bd12_cmd_allocator_reset(allocator), "split allocator reset");
    CHECK(bd12_cmd_list_begin(list), "split copy begin");
    bd12_cmd_copy_buffer(list, input, 0, upload, 0, bytes);
    bd12_cmd_transition_buffer(list, input, BD12_STATE_COPY_DEST,
                               BD12_STATE_NON_PIXEL_SHADER_RESOURCE);
    CHECK(bd12_cmd_list_end(list), "split copy end");
    BD12FenceOp predecessor = { fence, 2 };
    CHECK(bd12_queue_submit(queue, lists, 1, NULL, 0, &predecessor, 1) ==
              VK_SUCCESS, "split copy submit");

    BD12CmdAllocator *split_allocator = bd12_cmd_allocator_create(device);
    BD12CmdList *split_list = bd12_cmd_list_create(split_allocator);
    CHECK(split_allocator && split_list, "split command objects");
    CHECK(bd12_cmd_list_begin(split_list), "split compute begin");
    bd12_cmd_transition_buffer(split_list, output, BD12_STATE_COPY_SOURCE,
                               BD12_STATE_UNORDERED_ACCESS);
    bd12_cmd_dispatch(split_list, pipeline, &push, (COUNT + 63) / 64, 1, 1);
    bd12_cmd_transition_buffer(split_list, output, BD12_STATE_UNORDERED_ACCESS,
                               BD12_STATE_COPY_SOURCE);
    bd12_cmd_copy_buffer(split_list, readback, 0, output, 0, bytes);
    bd12_cmd_host_visibility(split_list, readback);
    CHECK(bd12_cmd_list_end(split_list), "split compute end");
    BD12CmdList *split_lists[] = { split_list };
    BD12Fence *split_fence = bd12_fence_create(device, 0);
    CHECK(split_fence, "split fence");
    BD12FenceOp split_signal = { split_fence, 1 };
    CHECK(bd12_queue_submit(queue, split_lists, 1, &predecessor, 1,
                            &split_signal, 1) == VK_SUCCESS,
          "split compute submit with predecessor dependency");
    CHECK(bd12_fence_wait(split_fence, 1, 5000000000ull), "split compute wait");
    bd12_fence_destroy(split_fence);
    const uint32_t *result = readback->cpu;
    for (uint32_t i = 0; i < COUNT; ++i)
        CHECK(result[i] == (i + 1000u) * 2u + 1u,
              "split submission memory dependency");
    bd12_cmd_list_destroy(split_list, split_allocator);
    bd12_cmd_allocator_destroy(split_allocator);
    printf("PASS split submit: predecessor device-memory dependency\n");


    /* A D3D12 list recorded into several primary command buffers must submit
     * every segment through the ordinary path. Submitting only the last one
     * silently dropped earlier copies/barriers/dispatches, so later work read
     * uninitialised device memory. Record the copy in segment 0 and the
     * dispatch plus readback in segment 1, then submit the list once. */
    {
        BD12CmdAllocator *sa = bd12_cmd_allocator_create(device);
        BD12CmdList *segmented = bd12_cmd_list_create(sa);
        BD12Fence *segmented_fence = bd12_fence_create(device, 0);
        CHECK(sa && segmented && segmented_fence, "segmented command objects");

        for (uint32_t i = 0; i < COUNT; ++i) src[i] = i + 7u;
        memset(readback->cpu, 0xcd, bytes);

        CHECK(bd12_cmd_list_begin(segmented), "segmented begin");
        bd12_cmd_transition_buffer(segmented, input, BD12_STATE_NON_PIXEL_SHADER_RESOURCE,
                                   BD12_STATE_COPY_DEST);
        bd12_cmd_copy_buffer(segmented, input, 0, upload, 0, bytes);
        bd12_cmd_transition_buffer(segmented, input, BD12_STATE_COPY_DEST,
                                   BD12_STATE_NON_PIXEL_SHADER_RESOURCE);
        CHECK(device->vk.EndCommandBuffer(segmented->command_buffer) == VK_SUCCESS,
              "segmented prefix end");
        CHECK(bd12_cmd_append_segment(segmented), "append segment");
        VkCommandBufferBeginInfo begin = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
        };
        CHECK(device->vk.BeginCommandBuffer(segmented->command_buffer, &begin) ==
                  VK_SUCCESS, "segmented second begin");
        bd12_cmd_transition_buffer(segmented, output, BD12_STATE_COPY_SOURCE,
                                   BD12_STATE_UNORDERED_ACCESS);
        bd12_cmd_dispatch(segmented, pipeline, &push, (COUNT + 63) / 64, 1, 1);
        bd12_cmd_transition_buffer(segmented, output, BD12_STATE_UNORDERED_ACCESS,
                                   BD12_STATE_COPY_SOURCE);
        bd12_cmd_copy_buffer(segmented, readback, 0, output, 0, bytes);
        bd12_cmd_host_visibility(segmented, readback);
        CHECK(bd12_cmd_list_end(segmented), "segmented end");
        CHECK(segmented->command_buffer_count == 2, "two recorded segments");

        BD12CmdList *segmented_lists[] = { segmented };
        const uint64_t before = queue->submit_count;
        BD12FenceOp segmented_signal = { segmented_fence, 1 };
        CHECK(bd12_queue_submit(queue, segmented_lists, 1, NULL, 0,
                                &segmented_signal, 1) == VK_SUCCESS,
              "segmented submit");
        CHECK(queue->submit_count == before + 1,
              "all segments ride in one submission");
        CHECK(bd12_fence_wait(segmented_fence, 1, 5000000000ull),
              "segmented wait");

        const uint32_t *segmented_result = readback->cpu;
        for (uint32_t i = 0; i < COUNT; ++i)
            CHECK(segmented_result[i] == (i + 7u) * 2u + 1u,
                  "every recorded segment executed");
        bd12_cmd_list_destroy(segmented, sa);
        bd12_cmd_allocator_destroy(sa);
        bd12_fence_destroy(segmented_fence);
        printf("PASS multi-segment list: every segment is submitted\n");

    }

    /* Queue-only signal (D3D12 Queue::Signal with no lists) and CPU signal. */

    BD12FenceOp signal3 = { fence, 3 };
    CHECK(bd12_queue_submit(queue, NULL, 0, NULL, 0, &signal3, 1) == VK_SUCCESS,
          "empty signal submit");
    CHECK(bd12_fence_wait(fence, 3, 5000000000ull), "empty signal wait");
    CHECK(bd12_fence_signal_cpu(fence, 4) && bd12_fence_completed(fence) == 4,
          "cpu signal");
    BD12FenceOp wait4 = { fence, 4 };
    BD12FenceOp signal5 = { fence, 5 };
    CHECK(bd12_queue_submit(queue, NULL, 0, &wait4, 1, &signal5, 1) == VK_SUCCESS,
          "queue wait on completed value");
    CHECK(bd12_fence_wait(fence, 5, 5000000000ull), "queue wait signal");
    printf("PASS fences: queue signal, cpu signal, queue wait\n");

    /* Throughput: N record+submit iterations, one CPU wait at the end. */
    /* A ring of allocators: an allocator is reset only after the fence value
     * of its previous submission completed (the D3D12 contract). */
    enum { RING = 4 };
    BD12CmdAllocator *ring_allocator[RING];
    BD12CmdList *ring_list[RING];
    uint64_t ring_value[RING] = { 0 };
    for (int i = 0; i < RING; ++i) {
        ring_allocator[i] = bd12_cmd_allocator_create(device);
        ring_list[i] = bd12_cmd_list_create(ring_allocator[i]);
        CHECK(ring_allocator[i] && ring_list[i], "ring creation");
    }
    const int iterations = 2000;
    uint64_t value = 5;
    uint64_t t0 = now_ns();
    for (int n = 0; n < iterations; ++n) {
        const int slot = n % RING;
        if (ring_value[slot])
            CHECK(bd12_fence_wait(fence, ring_value[slot], 30000000000ull),
                  "ring wait");
        BD12CmdList *l = ring_list[slot];
        CHECK(bd12_cmd_allocator_reset(ring_allocator[slot]), "allocator reset");
        CHECK(bd12_cmd_list_begin(l), "begin loop");
        bd12_cmd_transition_buffer(l, output, BD12_STATE_COPY_SOURCE,
                                   BD12_STATE_UNORDERED_ACCESS);
        bd12_cmd_dispatch(l, pipeline, &push, (COUNT + 63) / 64, 1, 1);
        bd12_cmd_transition_buffer(l, output, BD12_STATE_UNORDERED_ACCESS,
                                   BD12_STATE_COPY_SOURCE);
        CHECK(bd12_cmd_list_end(l), "end loop");
        BD12FenceOp s = { fence, ++value };
        BD12CmdList *submit_lists[] = { l };
        CHECK(bd12_queue_submit(queue, submit_lists, 1, NULL, 0, &s, 1) ==
                  VK_SUCCESS, "submit loop");
        ring_value[slot] = value;
    }
    CHECK(bd12_fence_wait(fence, value, 30000000000ull), "loop wait");
    for (int i = 0; i < RING; ++i) {
        bd12_cmd_list_destroy(ring_list[i], ring_allocator[i]);
        bd12_cmd_allocator_destroy(ring_allocator[i]);
    }
    uint64_t t1 = now_ns();
    printf("PASS throughput: %d dispatch submits in %.1f ms (%.1f us each, "
           "one CPU wait)\n",
           iterations, (double)(t1 - t0) / 1e6,
           (double)(t1 - t0) / 1e3 / iterations);

    bd12_buffer_destroy(upload);
    bd12_buffer_destroy(input);
    bd12_buffer_destroy(output);
    bd12_buffer_destroy(readback);
    bd12_compute_pipeline_destroy(pipeline);
    bd12_cmd_list_destroy(list, allocator);
    bd12_cmd_allocator_destroy(allocator);
    bd12_fence_destroy(fence);
    bd12_queue_destroy(queue);
    bd12_device_destroy(device);
    free(spv);
    printf("M1 PASS\n");
    return 0;
}
