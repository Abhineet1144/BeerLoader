/* BD12 milestone 3 test: textures, state->layout barriers, buffer<->image
 * copies, image/sampler descriptors in a bound compute dispatch.
 * A 16x16 texture is upscaled 4x with a nearest sampler into a 64x64 storage
 * image and read back. Usage: bd12_m3_test <m3_image.spv> */
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

static uint32_t texel(uint32_t x, uint32_t y)
{
    return 0xff000000u | ((y * 16u + 3u) << 16) | ((x * 16u + 1u) << 8) |
           (x ^ y) * 7u;
}

int main(int argc, char **argv)
{
    CHECK(argc >= 2, "usage: bd12_m3_test <spv>");
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

    BD12DescriptorRange ranges[2] = {
        { BD12_RANGE_SRV, 1, 0, 0, 0 },
        { BD12_RANGE_UAV, 1, 0, 0, BD12_RANGE_APPEND }
    };
    BD12DescriptorRange sampler_range = { BD12_RANGE_SAMPLER, 1, 0, 0, 0 };
    BD12RootParameter params[2] = {
        { .type = BD12_ROOT_TABLE, .ranges = ranges, .range_count = 2 },
        { .type = BD12_ROOT_TABLE, .ranges = &sampler_range, .range_count = 1 }
    };
    BD12RootSignature *rs = bd12_root_signature_create(device, params, 2);
    CHECK(rs, "root signature");
    BD12ShaderBinding bindings[] = {
        { BD12_RANGE_SRV, 0, 0, 0, 1, 8192 },
        { BD12_RANGE_UAV, 0, 0, 0, 1, 24576 },
        { BD12_RANGE_SAMPLER, 0, 0, 0, 1, 40960 }
    };
    BD12ComputePipeline *pso = bd12_compute_pipeline_create_bound(
        device, spv, spv_bytes, bindings, 3);
    CHECK(pso, "bound pipeline");

    BD12ImageDesc src_desc = { .type = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .width = 16, .height = 16, .depth_or_layers = 1, .mip_levels = 1, .samples = 1 };
    BD12ImageDesc dst_desc = { .type = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .width = 64, .height = 64, .depth_or_layers = 1, .mip_levels = 1, .samples = 1 };
    dst_desc.allow_uav = 1;
    BD12Image *src = bd12_image_create_committed(device, &src_desc, BD12_STATE_COMMON);
    BD12Image *dst = bd12_image_create_committed(device, &dst_desc, BD12_STATE_COMMON);
    CHECK(src && dst, "images");

    const uint32_t up_pitch = 256, rb_pitch = 256;
    BD12Buffer *upload = bd12_buffer_create_committed(device, BD12_HEAP_UPLOAD, up_pitch * 16);
    BD12Buffer *rb = bd12_buffer_create_committed(device, BD12_HEAP_READBACK, rb_pitch * 64);
    CHECK(upload && rb, "buffers");
    for (uint32_t y = 0; y < 16; ++y)
        for (uint32_t x = 0; x < 16; ++x)
            ((uint32_t *)((char *)upload->cpu + y * up_pitch))[x] = texel(x, y);
    memset(rb->cpu, 0xcd, rb_pitch * 64);

    BD12DescriptorHeap *heap = bd12_descriptor_heap_create(device, 0, 8);
    BD12DescriptorHeap *samplers = bd12_descriptor_heap_create(device, 1, 4);
    CHECK(heap && samplers, "heaps");
    CHECK(bd12_descriptor_write_image_srv(heap, 2, src, VK_IMAGE_VIEW_TYPE_2D,
                                          VK_FORMAT_UNDEFINED, 0, 1, 0, 1), "srv");
    CHECK(bd12_descriptor_write_image_uav(heap, 3, dst, VK_IMAGE_VIEW_TYPE_2D,
                                          VK_FORMAT_UNDEFINED, 0, 0, 1), "uav");
    VkSamplerCreateInfo sampler = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = VK_LOD_CLAMP_NONE
    };
    CHECK(bd12_descriptor_write_sampler(samplers, 1, &sampler), "sampler");
    CHECK(bd12_descriptor_write_sampler(samplers, 2, &sampler), "sampler dedupe");
    CHECK(heap->descriptors[2].kind == BD12_DESC_SRV_IMAGE &&
          samplers->descriptors[1].sampler == samplers->descriptors[2].sampler,
          "sampler cache");

    CHECK(bd12_cmd_list_begin(list), "begin");
    bd12_cmd_transition_image(list, src, BD12_ALL_SUBRESOURCES, BD12_STATE_COMMON,
                              BD12_STATE_COPY_DEST);
    bd12_cmd_copy_buffer_to_image(list, src, (BD12ImagePoint){ 0 }, upload, 0,
                                  up_pitch, 16, 16, 1);
    bd12_cmd_transition_image(list, src, BD12_ALL_SUBRESOURCES,
                              BD12_STATE_COPY_DEST,
                              BD12_STATE_NON_PIXEL_SHADER_RESOURCE);
    bd12_cmd_transition_image(list, dst, BD12_ALL_SUBRESOURCES, BD12_STATE_COMMON,
                              BD12_STATE_UNORDERED_ACCESS);
    bd12_cmd_set_descriptor_heaps(list, heap, samplers);
    bd12_cmd_set_compute_root_signature(list, rs);
    bd12_cmd_set_pipeline(list, pso);
    bd12_cmd_set_compute_root_table(list, 0, bd12_descriptor_gpu_handle(heap, 2));
    bd12_cmd_set_compute_root_table(list, 1, bd12_descriptor_gpu_handle(samplers, 1));
    CHECK(bd12_cmd_dispatch_bound(list, 8, 8, 1), "dispatch");
    bd12_cmd_transition_image(list, dst, BD12_ALL_SUBRESOURCES,
                              BD12_STATE_UNORDERED_ACCESS, BD12_STATE_COPY_SOURCE);
    bd12_cmd_copy_image_to_buffer(list, rb, 0, rb_pitch, dst, (BD12ImagePoint){ 0 },
                                  64, 64, 1);
    bd12_cmd_host_visibility(list, rb);
    CHECK(bd12_cmd_list_end(list), "end");

    BD12CmdList *lists[] = { list };
    BD12FenceOp signal = { fence, 1 };
    CHECK(bd12_queue_submit(queue, lists, 1, NULL, 0, &signal, 1) == VK_SUCCESS, "submit");
    CHECK(bd12_fence_wait(fence, 1, 5000000000ull), "fence wait");
    CHECK(queue->submit_count == 1, "one submit");

    for (uint32_t y = 0; y < 64; ++y) {
        const uint32_t *row = (const uint32_t *)((char *)rb->cpu + y * rb_pitch);
        for (uint32_t x = 0; x < 64; ++x) {
            if (row[x] != texel(x / 4, y / 4)) {
                fprintf(stderr, "FAIL: (%u,%u) got %08x want %08x\n", x, y,
                        row[x], texel(x / 4, y / 4));
                return 1;
            }
        }
    }
    printf("PASS correctness: copy buffer->image, layout transitions, sampled+storage image, sampler, readback, 1 submit\n");

    /* Image-to-image copy plus a second transition round trip. */
    BD12Image *copy = bd12_image_create_committed(device, &dst_desc, BD12_STATE_COPY_DEST);
    CHECK(copy, "copy image");
    memset(rb->cpu, 0xcd, rb_pitch * 64);
    CHECK(bd12_cmd_allocator_reset(allocator), "reset");
    CHECK(bd12_cmd_list_begin(list), "begin 2");
    bd12_cmd_copy_image(list, copy, (BD12ImagePoint){ 0 }, dst, (BD12ImagePoint){ 0 }, 64, 64, 1);
    bd12_cmd_transition_image(list, copy, BD12_ALL_SUBRESOURCES, BD12_STATE_COPY_DEST,
                              BD12_STATE_COPY_SOURCE);
    bd12_cmd_copy_image_to_buffer(list, rb, 0, rb_pitch, copy, (BD12ImagePoint){ 0 },
                                  64, 64, 1);
    bd12_cmd_host_visibility(list, rb);
    CHECK(bd12_cmd_list_end(list), "end 2");
    signal.value = 2;
    CHECK(bd12_queue_submit(queue, lists, 1, NULL, 0, &signal, 1) == VK_SUCCESS, "submit 2");
    CHECK(bd12_fence_wait(fence, 2, 5000000000ull), "fence wait 2");
    for (uint32_t y = 0; y < 64; ++y) {
        const uint32_t *row = (const uint32_t *)((char *)rb->cpu + y * rb_pitch);
        for (uint32_t x = 0; x < 64; ++x)
            CHECK(row[x] == texel(x / 4, y / 4), "image copy content");
    }
    printf("PASS image-to-image copy (dst was left in COPY_SOURCE)\n");

    bd12_image_destroy(copy);
    bd12_descriptor_heap_destroy(heap);
    bd12_descriptor_heap_destroy(samplers);
    bd12_image_destroy(src); bd12_image_destroy(dst);
    bd12_buffer_destroy(upload); bd12_buffer_destroy(rb);
    bd12_compute_pipeline_destroy(pso);
    bd12_root_signature_destroy(rs);
    bd12_cmd_list_destroy(list, allocator);
    bd12_cmd_allocator_destroy(allocator);
    bd12_fence_destroy(fence);
    bd12_queue_destroy(queue);
    bd12_device_destroy(device);
    free(spv);
    printf("M3 PASS\n");
    return 0;
}
