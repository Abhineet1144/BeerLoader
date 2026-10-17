#include "vulkan_compute.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Usage: vulkan_compute_test buffer.spv image.spv
 * buffer.spv: local_size 64; set0 binding0 readonly SSBO src, binding1 SSBO
 * dst, binding2 UBO uvec4(count, multiplier, addend, 0): for i < count,
 * dst[i] = src[i] * multiplier + addend.
 * image.spv: local_size 8x8; binding0 rgba8 storage image dst, binding1 sampled
 * texture src, binding2 sampler: dst(p) = textureLod(src, p).bgra. */

static uint32_t *load(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    uint32_t *data = malloc((size_t)length);
    if (data && fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        data = NULL;
    }
    fclose(file);
    *size = (size_t)length;
    return data;
}

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
    return 1; } } while (0)

int main(int argc, char **argv)
{
    if (argc != 10) return 2;
    size_t buffer_size = 0, image_size = 0, array_size = 0, volume_size = 0;
    size_t mip_size = 0, cube_size = 0, image_1d_size = 0;
    size_t watchdog_volume_size = 0, noop_size = 0;
    uint32_t *buffer_spirv = load(argv[1], &buffer_size);
    uint32_t *image_spirv = load(argv[2], &image_size);
    uint32_t *array_spirv = load(argv[3], &array_size);
    uint32_t *volume_spirv = load(argv[4], &volume_size);
    uint32_t *mip_spirv = load(argv[5], &mip_size);
    uint32_t *cube_spirv = load(argv[6], &cube_size);
    uint32_t *image_1d_spirv = load(argv[7], &image_1d_size);
    uint32_t *watchdog_volume_spirv = load(argv[8], &watchdog_volume_size);
    uint32_t *noop_spirv = load(argv[9], &noop_size);
    CHECK(buffer_spirv && image_spirv && array_spirv && volume_spirv &&
          mip_spirv && cube_spirv && image_1d_spirv &&
          watchdog_volume_spirv && noop_spirv);

    enum { COUNT = 200 };
    uint32_t source[COUNT], destination[COUNT], params[4] = {COUNT, 3, 7, 0};
    uint32_t guard[4] = {0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef};
    for (uint32_t i = 0; i < COUNT; ++i) {
        source[i] = i * 11u;
        destination[i] = 0xffffffffu;
    }
    BeerVulkanComputeBinding bindings[3] = {
        {0, 0, (uint8_t *)source, sizeof(source), 0},
        {1, 0, (uint8_t *)destination, sizeof(destination), 1},
        {2, 1, (uint8_t *)params, sizeof(params), 0}
    };
    const char *reason = NULL;
    int ok = beer_vulkan_compute_dispatch(buffer_spirv, buffer_spirv,
                                          buffer_size, bindings, 3, 4, 1, 1,
                                          &reason);
    if (!ok && reason && !strcmp(reason, "Vulkan unavailable")) {
        printf("vulkan compute test skipped: no Vulkan device\n");
        return 0;
    }
    CHECK(ok);
    for (uint32_t i = 0; i < COUNT; ++i)
        CHECK(destination[i] == source[i] * 3u + 7u);
    CHECK(guard[0] == 0xdeadbeef);

    /* Read-only inputs are never written back. */
    for (uint32_t i = 0; i < COUNT; ++i) CHECK(source[i] == i * 11u);

    /* Dispatch covering fewer groups leaves the tail untouched. */
    memset(destination, 0xff, sizeof(destination));
    CHECK(beer_vulkan_compute_dispatch(buffer_spirv, buffer_spirv, buffer_size,
                                       bindings, 3, 1, 1, 1, &reason));
    CHECK(destination[63] == source[63] * 3u + 7u);
    CHECK(destination[64] == 0xffffffffu);

    /* The same resource bound as SRV and UAV shares one device range. */
    uint32_t inplace[COUNT];
    for (uint32_t i = 0; i < COUNT; ++i) inplace[i] = i;
    BeerVulkanComputeBinding aliased[3] = {
        {0, 0, (uint8_t *)inplace, sizeof(inplace), 0},
        {1, 0, (uint8_t *)inplace, sizeof(inplace), 1},
        {2, 1, (uint8_t *)params, sizeof(params), 0}
    };
    CHECK(beer_vulkan_compute_dispatch(buffer_spirv, buffer_spirv, buffer_size,
                                       aliased, 3, 4, 1, 1, &reason));
    for (uint32_t i = 0; i < COUNT; ++i) CHECK(inplace[i] == i * 3u + 7u);

    /* Partially overlapping ranges are rejected. */
    BeerVulkanComputeBinding overlap[3] = {
        {0, 0, (uint8_t *)inplace, 256, 0},
        {1, 0, (uint8_t *)inplace + 128, 256, 1},
        {2, 1, (uint8_t *)params, sizeof(params), 0}
    };
    CHECK(!beer_vulkan_compute_dispatch(buffer_spirv, buffer_spirv,
                                        buffer_size, overlap, 3, 1, 1, 1,
                                        &reason));

    /* Missing binding and wrong buffer kind are rejected. */
    CHECK(!beer_vulkan_compute_dispatch(buffer_spirv, buffer_spirv,
                                        buffer_size, bindings, 2, 1, 1, 1,
                                        &reason));
    BeerVulkanComputeBinding wrong[3] = {
        {0, 0, (uint8_t *)source, sizeof(source), 0},
        {1, 0, (uint8_t *)destination, sizeof(destination), 1},
        {2, 0, (uint8_t *)params, sizeof(params), 0}
    };
    CHECK(!beer_vulkan_compute_dispatch(buffer_spirv, buffer_spirv,
                                        buffer_size, wrong, 3, 1, 1, 1,
                                        &reason));

    /* Sampled texture + sampler -> storage image, with padded row pitches. */
    enum { SOURCE_PITCH = 48, TARGET_PITCH = 32 };
    uint8_t texture[SOURCE_PITCH * 8], target[TARGET_PITCH * 8];
    memset(texture, 0xcd, sizeof(texture));
    memset(target, 0, sizeof(target));
    for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t x = 0; x < 8; ++x) {
            uint8_t *texel = texture + y * SOURCE_PITCH + x * 4;
            texel[0] = (uint8_t)(x * 16);
            texel[1] = (uint8_t)(y * 16);
            texel[2] = (uint8_t)(x + y);
            texel[3] = 255;
        }
    size_t texture_size = SOURCE_PITCH * 7 + 8 * 4;
    size_t target_size = TARGET_PITCH * 7 + 8 * 4;
    int target_resource;
    uint64_t target_serial = 0;
    BeerVulkanComputeBinding image_bindings[3] = {
        { .binding = 0, .data = target, .size = target_size, .writable = 1,
          .resource = &target_resource, .serial = target_serial,
          .serial_location = &target_serial,
          .kind = BEER_VULKAN_COMPUTE_BINDING_IMAGE, .image_format = 37,
          .image_bytes = 4, .image_width = 8, .image_height = 8,
          .image_row_pitch = TARGET_PITCH },
        { .binding = 1, .data = texture, .size = texture_size,
          .kind = BEER_VULKAN_COMPUTE_BINDING_IMAGE, .image_format = 37,
          .image_bytes = 4, .image_width = 8, .image_height = 8,
          .image_row_pitch = SOURCE_PITCH },
        { .binding = 2, .kind = BEER_VULKAN_COMPUTE_BINDING_SAMPLER,
          .address_u = 2, .address_v = 2, .address_w = 2,
          .anisotropy_enable = 1, .max_anisotropy = 16.0f }
    };
    CHECK(beer_vulkan_compute_dispatch(image_spirv, image_spirv, image_size,
                                       image_bindings, 3, 1, 1, 1, &reason));
    CHECK(target_serial == 1);
    for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t x = 0; x < 8; ++x) {
            const uint8_t *texel = target + y * TARGET_PITCH + x * 4;
            CHECK(texel[0] == (uint8_t)(x + y));
            CHECK(texel[1] == (uint8_t)(y * 16));
            CHECK(texel[2] == (uint8_t)(x * 16));
            CHECK(texel[3] == 255);
        }
    /* The writable mirror is now the next dispatch's sampled input. This
     * exercises the producer->consumer fast path: the second dispatch skips
     * its upload and must therefore see the image restored to GENERAL after
     * the first dispatch's readback. */
    image_bindings[0].serial = target_serial;
    image_bindings[1].data = target;
    image_bindings[1].size = target_size;
    image_bindings[1].resource = &target_resource;
    image_bindings[1].serial = target_serial;
    image_bindings[1].serial_location = &target_serial;
    image_bindings[1].image_row_pitch = TARGET_PITCH;
    CHECK(beer_vulkan_compute_dispatch(image_spirv, image_spirv, image_size,
                                       image_bindings, 3, 1, 1, 1, &reason));
    CHECK(target_serial == 2);
    for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t x = 0; x < 8; ++x) {
            const uint8_t *texel = target + y * TARGET_PITCH + x * 4;
            CHECK(texel[0] == (uint8_t)(x * 16));
            CHECK(texel[1] == (uint8_t)(y * 16));
            CHECK(texel[2] == (uint8_t)(x + y));
            CHECK(texel[3] == 255);
        }
    /* A writable image must not steal another identified resource's current,
     * shape-compatible mirror while preparing a later sampled binding. */
    uint8_t reuse_output[TARGET_PITCH * 8];
    int reuse_output_resource;
    uint64_t reuse_output_serial = 0;
    memset(reuse_output, 0, sizeof(reuse_output));
    image_bindings[0].data = reuse_output;
    image_bindings[0].size = target_size;
    image_bindings[0].resource = &reuse_output_resource;
    image_bindings[0].serial = reuse_output_serial;
    image_bindings[0].serial_location = &reuse_output_serial;
    image_bindings[1].data = target;
    image_bindings[1].size = target_size;
    image_bindings[1].resource = &target_resource;
    image_bindings[1].serial = target_serial;
    image_bindings[1].serial_location = &target_serial;
    CHECK(beer_vulkan_compute_dispatch(image_spirv, image_spirv, image_size,
                                       image_bindings, 3, 1, 1, 1, &reason));
    CHECK(reuse_output_serial == 1);
    for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t x = 0; x < 8; ++x) {
            const uint8_t *texel = reuse_output + y * TARGET_PITCH + x * 4;
            CHECK(texel[0] == (uint8_t)(x + y));
            CHECK(texel[1] == (uint8_t)(y * 16));
            CHECK(texel[2] == (uint8_t)(x * 16));
            CHECK(texel[3] == 255);
        }

    /* A compute batch must preserve producer-to-consumer data, but host
     * storage remains authoritative at each dispatch boundary until the
     * executor models complete D3D12 resource-state and aliasing semantics.
     * Use a separate consumer target so the test does not introduce an
     * unrelated sampled/storage race. */
    uint8_t batch_target[TARGET_PITCH * 8];
    int batch_target_resource;
    uint64_t batch_target_serial = 0;
    memset(target, 0, target_size);
    memset(batch_target, 0, sizeof(batch_target));
    target_serial = 0;
    image_bindings[0].data = target;
    image_bindings[0].size = target_size;
    image_bindings[0].resource = &target_resource;
    image_bindings[0].serial = target_serial;
    image_bindings[0].serial_location = &target_serial;
    image_bindings[0].image_row_pitch = TARGET_PITCH;
    image_bindings[1].data = texture;
    image_bindings[1].size = texture_size;
    image_bindings[1].resource = NULL;
    image_bindings[1].serial_location = NULL;
    image_bindings[1].serial = 0;
    image_bindings[1].image_row_pitch = SOURCE_PITCH;
    beer_vulkan_compute_begin_batch();
    CHECK(beer_vulkan_compute_dispatch(image_spirv, image_spirv, image_size,
                                       image_bindings, 3, 1, 1, 1, &reason));
    CHECK(target_serial == 1);
    image_bindings[0].data = batch_target;
    image_bindings[0].size = target_size;
    image_bindings[0].resource = &batch_target_resource;
    image_bindings[0].serial = batch_target_serial;
    image_bindings[0].serial_location = &batch_target_serial;
    image_bindings[1].data = target;
    image_bindings[1].size = target_size;
    image_bindings[1].resource = &target_resource;
    image_bindings[1].serial = target_serial;
    image_bindings[1].serial_location = &target_serial;
    image_bindings[1].image_row_pitch = TARGET_PITCH;
    CHECK(beer_vulkan_compute_dispatch_with_hazards(
        image_spirv, image_spirv, image_size, image_bindings, 3, 1, 1, 1,
        BEER_VULKAN_COMPUTE_HAZARD_RAW, &reason));
    CHECK(batch_target_serial == 1);
    beer_vulkan_compute_end_batch();
    /* Ending an epoch preserves the produced image as GPU-authoritative. This
     * avoids faulting a large sparse DEFAULT-heap CPU shadow merely because the
     * next consumer is graphics. */
    for (size_t byte = 0; byte < sizeof(batch_target); ++byte)
        CHECK(batch_target[byte] == 0);
    BeerVulkanComputeImageMirror graphics_mirror = {0};
    CHECK(beer_vulkan_compute_has_image_mirror(
        &batch_target_resource, batch_target_serial, 37, 8, 8, 1, 1, 2, 1));
    CHECK(beer_vulkan_compute_acquire_image_mirror(
        &batch_target_resource, batch_target_serial, 37, 8, 8, 1, 1, 2, 1,
        &graphics_mirror));
    CHECK(graphics_mirror.image && graphics_mirror.token);
    beer_vulkan_compute_release_image_mirror(&graphics_mirror);
    /* A true CPU consumer publishes exactly this resource and leaves its GPU
     * mirror reusable at the same serial. */
    CHECK(beer_vulkan_compute_sync_resource(
        &batch_target_resource, batch_target_serial));
    for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t x = 0; x < 8; ++x) {
            const uint8_t *texel = batch_target + y * TARGET_PITCH + x * 4;
            CHECK(texel[0] == (uint8_t)(x * 16));
            CHECK(texel[1] == (uint8_t)(y * 16));
            CHECK(texel[2] == (uint8_t)(x + y));
            CHECK(texel[3] == 255);
        }
    CHECK(beer_vulkan_compute_has_image_mirror(
        &batch_target_resource, batch_target_serial, 37, 8, 8, 1, 1, 2, 1));

    /* The renderer-owned epoch API must preserve operation order, propagate
     * individual results, and publish producer output before the consumer is
     * prepared by the conservative adapter. This API is the replacement seam
     * for future one-command-buffer recording. */
    memset(target, 0, target_size);
    memset(batch_target, 0, sizeof(batch_target));
    target_serial = 0;
    batch_target_serial = 0;
    image_bindings[0].data = target;
    image_bindings[0].resource = &target_resource;
    image_bindings[0].serial = target_serial;
    image_bindings[0].serial_location = &target_serial;
    image_bindings[1].data = texture;
    image_bindings[1].size = texture_size;
    image_bindings[1].resource = NULL;
    image_bindings[1].serial = 0;
    image_bindings[1].serial_location = NULL;
    image_bindings[1].image_row_pitch = SOURCE_PITCH;
    BeerVulkanComputeBinding epoch_consumer[3];
    memcpy(epoch_consumer, image_bindings, sizeof(epoch_consumer));
    epoch_consumer[0].data = batch_target;
    epoch_consumer[0].resource = &batch_target_resource;
    epoch_consumer[0].serial = batch_target_serial;
    epoch_consumer[0].serial_location = &batch_target_serial;
    epoch_consumer[1].data = texture;
    epoch_consumer[1].size = texture_size;
    epoch_consumer[1].resource = NULL;
    epoch_consumer[1].serial = 0;
    epoch_consumer[1].serial_location = NULL;
    uint8_t epoch_results[2] = {0, 0};
    const char *epoch_reasons[2] = {NULL, NULL};
    BeerVulkanComputeOperation epoch_operations[2] = {
        { .shader_key = image_spirv, .spirv = image_spirv,
          .spirv_size = image_size, .bindings = image_bindings,
          .binding_count = 3, .group_count_x = 1, .group_count_y = 1,
          .group_count_z = 1, .result = &epoch_results[0],
          .reason = &epoch_reasons[0] },
        { .shader_key = image_spirv, .spirv = image_spirv,
          .spirv_size = image_size, .bindings = epoch_consumer,
          .binding_count = 3, .group_count_x = 1, .group_count_y = 1,
          .group_count_z = 1,
          .hazards_before = BEER_VULKAN_COMPUTE_HAZARD_RAW,
          .result = &epoch_results[1], .reason = &epoch_reasons[1] }
    };
    beer_vulkan_compute_begin_batch();
    CHECK(beer_vulkan_compute_execute_epoch(epoch_operations, 2));
    beer_vulkan_compute_end_batch();
    CHECK(epoch_results[0] && epoch_results[1]);
    CHECK(target_serial == 1 && batch_target_serial == 1);

    /* Preparing several distinct shaders may grow/reallocate the pipeline
     * cache. Prepared epoch references must survive that growth. */
    BeerVulkanComputeOperation pipeline_growth_epoch[8];
    uint8_t pipeline_growth_results[8] = {0};
    const char *pipeline_growth_reasons[8] = {0};
    for (uint32_t i = 0; i < 8; ++i) {
        pipeline_growth_epoch[i] = epoch_operations[0];
        pipeline_growth_epoch[i].shader_key = (const uint8_t *)image_spirv + i;
        pipeline_growth_epoch[i].result = &pipeline_growth_results[i];
        pipeline_growth_epoch[i].reason = &pipeline_growth_reasons[i];
    }
    BeerVulkanComputeStats shareable_before = {0}, shareable_after = {0};
    beer_vulkan_compute_get_stats(&shareable_before);
    beer_vulkan_compute_begin_batch();
    CHECK(beer_vulkan_compute_execute_epoch(pipeline_growth_epoch, 8));
    beer_vulkan_compute_end_batch();
    beer_vulkan_compute_get_stats(&shareable_after);
    CHECK(shareable_after.shareable_epoch_calls ==
          shareable_before.shareable_epoch_calls + 1);
    CHECK(shareable_after.shareable_epoch_operations ==
          shareable_before.shareable_epoch_operations + 8);
    for (uint32_t i = 0; i < 8; ++i) {
        CHECK(pipeline_growth_results[i]);
        CHECK(!pipeline_growth_reasons[i]);
    }

    /* A binding-free, hazard-free epoch is the first true shared-recording
     * class: every dispatch is recorded into one command buffer and covered by
     * one queue submission/fence wait. */
    uint8_t noop_results[8] = {0};
    const char *noop_reasons[8] = {0};
    BeerVulkanComputeOperation noop_epoch[8];
    for (uint32_t i = 0; i < 8; ++i) {
        memset(&noop_epoch[i], 0, sizeof(noop_epoch[i]));
        noop_epoch[i].shader_key = noop_spirv;
        noop_epoch[i].spirv = noop_spirv;
        noop_epoch[i].spirv_size = noop_size;
        noop_epoch[i].group_count_x = 1;
        noop_epoch[i].group_count_y = 1;
        noop_epoch[i].group_count_z = 1;
        noop_epoch[i].result = &noop_results[i];
        noop_epoch[i].reason = &noop_reasons[i];
    }
    BeerVulkanComputeStats noop_before = {0}, noop_after = {0};
    beer_vulkan_compute_get_stats(&noop_before);
    beer_vulkan_compute_begin_batch();
    CHECK(beer_vulkan_compute_execute_epoch(noop_epoch, 8));
    beer_vulkan_compute_end_batch();
    beer_vulkan_compute_get_stats(&noop_after);
    CHECK(noop_after.queue_submissions == noop_before.queue_submissions + 1);
    CHECK(noop_after.completed_submissions ==
          noop_before.completed_submissions + 1);
    CHECK(noop_after.host_fence_waits == noop_before.host_fence_waits + 1);
    CHECK(noop_after.shared_epoch_calls == noop_before.shared_epoch_calls + 1);
    CHECK(noop_after.shared_epoch_operations ==
          noop_before.shared_epoch_operations + 8);
    for (uint32_t i = 0; i < 8; ++i) {
        CHECK(noop_results[i]);
        CHECK(!noop_reasons[i]);
    }

    /* The growth epoch also leaves its target GPU-authoritative. Publish only
     * because this assertion is an explicit CPU consumer. */
    CHECK(beer_vulkan_compute_sync_resource(
        &target_resource, target_serial));
    for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t x = 0; x < 8; ++x) {
            const uint8_t *texel = target + y * TARGET_PITCH + x * 4;
            CHECK(texel[0] == (uint8_t)(x + y));
            CHECK(texel[1] == (uint8_t)(y * 16));
            CHECK(texel[2] == (uint8_t)(x * 16));
            CHECK(texel[3] == 255);
        }

    /* Epoch preparation rejects malformed immutable shader state before
     * execution starts, while later valid operations still run and publish
     * their individual result. */
    memset(batch_target, 0, sizeof(batch_target));
    batch_target_serial = 0;
    epoch_consumer[0].serial = 0;
    BeerVulkanComputeOperation mixed_epoch[2] = {
        { .shader_key = image_spirv, .spirv = image_spirv,
          .spirv_size = 4, .bindings = image_bindings, .binding_count = 3,
          .group_count_x = 1, .group_count_y = 1, .group_count_z = 1,
          .result = &epoch_results[0], .reason = &epoch_reasons[0] },
        { .shader_key = image_spirv, .spirv = image_spirv,
          .spirv_size = image_size, .bindings = epoch_consumer,
          .binding_count = 3, .group_count_x = 1, .group_count_y = 1,
          .group_count_z = 1, .result = &epoch_results[1],
          .reason = &epoch_reasons[1] }
    };
    epoch_results[0] = epoch_results[1] = 1;
    epoch_reasons[0] = epoch_reasons[1] = NULL;
    beer_vulkan_compute_begin_batch();
    CHECK(!beer_vulkan_compute_execute_epoch(mixed_epoch, 2));
    beer_vulkan_compute_end_batch();
    CHECK(!epoch_results[0]);
    CHECK(epoch_reasons[0] && !strcmp(epoch_reasons[0], "invalid arguments"));
    CHECK(epoch_results[1] && !epoch_reasons[1]);
    CHECK(batch_target_serial == 1);

    image_bindings[0].data = target;
    image_bindings[0].resource = &target_resource;
    image_bindings[0].serial_location = &target_serial;
    image_bindings[0].serial = target_serial;
    image_bindings[1].data = texture;
    image_bindings[1].size = texture_size;
    image_bindings[1].resource = NULL;
    image_bindings[1].serial = 0;
    image_bindings[1].image_row_pitch = SOURCE_PITCH;
    for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t x = 0; x < 8; ++x)
            CHECK(texture[y * SOURCE_PITCH + x * 4 + 0] == (uint8_t)(x * 16));

    /* A BC1 sampled image uses block-row staging while the shader still sees
     * normal texel coordinates. Four solid-red blocks cover the 8x8 image. */
    uint8_t bc1[32];
    memset(bc1, 0, sizeof(bc1));
    for (uint32_t block = 0; block < 4; ++block) {
        bc1[block * 8 + 0] = 0x00;
        bc1[block * 8 + 1] = 0xf8; /* RGB565 endpoint zero: red. */
    }
    memset(target, 0, sizeof(target));
    image_bindings[1].data = bc1;
    image_bindings[1].size = sizeof(bc1);
    image_bindings[1].image_format = 131; /* VK_FORMAT_BC1_RGB_UNORM_BLOCK */
    image_bindings[1].image_bytes = 0;
    image_bindings[1].image_block_width = 4;
    image_bindings[1].image_block_height = 4;
    image_bindings[1].image_block_bytes = 8;
    image_bindings[1].image_row_pitch = 16;
    CHECK(beer_vulkan_compute_dispatch(image_spirv, image_spirv, image_size,
                                       image_bindings, 3, 1, 1, 1, &reason));
    for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t x = 0; x < 8; ++x) {
            const uint8_t *texel = target + y * TARGET_PITCH + x * 4;
            CHECK(texel[0] == 0 && texel[1] == 0 && texel[2] >= 248 &&
                  texel[3] == 255);
        }
    image_bindings[1].data = texture;
    image_bindings[1].size = texture_size;
    image_bindings[1].image_format = 37;
    image_bindings[1].image_bytes = 4;
    image_bindings[1].image_block_width = 0;
    image_bindings[1].image_block_height = 0;
    image_bindings[1].image_block_bytes = 0;
    image_bindings[1].image_row_pitch = SOURCE_PITCH;
    /* Restore the normal sampled-image result expected by the alias test. */
    CHECK(beer_vulkan_compute_dispatch(image_spirv, image_spirv, image_size,
                                       image_bindings, 3, 1, 1, 1, &reason));

    /* Missing sampler, writable sampled image, unsupported sampler state and
     * inconsistent image layout are rejected. */
    CHECK(!beer_vulkan_compute_dispatch(image_spirv, image_spirv, image_size,
                                        image_bindings, 2, 1, 1, 1, &reason));
    image_bindings[1].writable = 1;
    CHECK(!beer_vulkan_compute_dispatch(image_spirv, image_spirv, image_size,
                                        image_bindings, 3, 1, 1, 1, &reason));
    image_bindings[1].writable = 0;
    image_bindings[2].address_u = 3;
    CHECK(!beer_vulkan_compute_dispatch(image_spirv, image_spirv, image_size,
                                        image_bindings, 3, 1, 1, 1, &reason));
    image_bindings[2].address_u = 2;
    image_bindings[1].image_row_pitch = 30;
    CHECK(!beer_vulkan_compute_dispatch(image_spirv, image_spirv, image_size,
                                        image_bindings, 3, 1, 1, 1, &reason));
    image_bindings[1].image_row_pitch = SOURCE_PITCH;

    /* A texture aliased as sampled image and storage image shares one device
     * image; swapping channels a second time restores the original texels. */
    image_bindings[1].data = target;
    image_bindings[1].size = target_size;
    image_bindings[1].image_row_pitch = TARGET_PITCH;
    CHECK(beer_vulkan_compute_dispatch(image_spirv, image_spirv, image_size,
                                       image_bindings, 3, 1, 1, 1, &reason));
    for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t x = 0; x < 8; ++x) {
            const uint8_t *texel = target + y * TARGET_PITCH + x * 4;
            CHECK(texel[0] == (uint8_t)(x * 16));
            CHECK(texel[1] == (uint8_t)(y * 16));
            CHECK(texel[2] == (uint8_t)(x + y));
        }

    /* Array and volume images preserve every selected layer/depth slice and
     * copy writable results back into the original host layout. */
    uint8_t layered_source[4 * 4 * 4 * 2];
    uint8_t layered_target[sizeof(layered_source)];
    for (uint32_t layer = 0; layer < 2; ++layer)
        for (uint32_t y = 0; y < 4; ++y)
            for (uint32_t x = 0; x < 4; ++x) {
                uint8_t *texel = layered_source +
                    ((layer * 4 * 4) + y * 4 + x) * 4;
                texel[0] = (uint8_t)(20 + layer * 80);
                texel[1] = (uint8_t)(30 + y);
                texel[2] = (uint8_t)(40 + x);
                texel[3] = 255;
            }
    memset(layered_target, 0, sizeof(layered_target));
    BeerVulkanComputeBinding layered[3] = {
        { .binding = 0, .data = layered_target, .size = sizeof(layered_target),
          .writable = 1, .kind = BEER_VULKAN_COMPUTE_BINDING_IMAGE,
          .image_format = 37, .image_bytes = 4, .image_width = 4,
          .image_height = 4, .image_row_pitch = 16, .image_dimension = 2,
          .image_arrayed = 1, .image_layers = 2, .image_depth = 1 },
        { .binding = 1, .data = layered_source, .size = sizeof(layered_source),
          .kind = BEER_VULKAN_COMPUTE_BINDING_IMAGE, .image_format = 37,
          .image_bytes = 4, .image_width = 4, .image_height = 4,
          .image_row_pitch = 16, .image_dimension = 2, .image_arrayed = 1,
          .image_layers = 2, .image_depth = 1 },
        { .binding = 2, .kind = BEER_VULKAN_COMPUTE_BINDING_SAMPLER,
          .address_u = 2, .address_v = 2, .address_w = 2 }
    };
    if (!beer_vulkan_compute_dispatch(array_spirv, array_spirv, array_size,
                                      layered, 3, 1, 1, 1, &reason)) {
        fprintf(stderr, "array compute failed: %s\n", reason ? reason : "?");
        CHECK(0);
    }
    CHECK(layered_target[0] == 40 && layered_target[2] == 20);
    CHECK(layered_target[4 * 4 * 4] == 40 &&
          layered_target[4 * 4 * 4 + 2] == 100);

    memset(layered_target, 0, sizeof(layered_target));
    layered[0].image_dimension = layered[1].image_dimension = 3;
    layered[0].image_arrayed = layered[1].image_arrayed = 0;
    layered[0].image_layers = layered[1].image_layers = 1;
    layered[0].image_depth = layered[1].image_depth = 2;
    CHECK(beer_vulkan_compute_dispatch(volume_spirv, volume_spirv, volume_size,
                                       layered, 3, 1, 1, 1, &reason));
    CHECK(layered_target[0] == 40 && layered_target[2] == 20);
    CHECK(layered_target[4 * 4 * 4] == 40 &&
          layered_target[4 * 4 * 4 + 2] == 100);

    /* Shape mismatch is rejected before creating a Vulkan view. */
    layered[1].image_dimension = 2;
    CHECK(!beer_vulkan_compute_dispatch(volume_spirv, volume_spirv,
                                        volume_size, layered, 3, 1, 1, 1,
                                        &reason));

    /* Consecutive mip levels are uploaded into one sampled image and remain
     * addressable through textureLod. Level zero is red; level one is green. */
    uint8_t mip_source[4 * 4 * 4 + 2 * 2 * 4];
    uint8_t mip_target[4] = {0, 0, 0, 0};
    memset(mip_source, 0, sizeof(mip_source));
    for (uint32_t i = 0; i < 4 * 4; ++i) {
        mip_source[i * 4] = 255;
        mip_source[i * 4 + 3] = 255;
    }
    for (uint32_t i = 0; i < 2 * 2; ++i) {
        uint8_t *texel = mip_source + 4 * 4 * 4 + i * 4;
        texel[1] = 255;
        texel[3] = 255;
    }
    BeerVulkanComputeBinding mip_bindings[3] = {
        { .binding = 0, .data = mip_target, .size = sizeof(mip_target),
          .writable = 1, .kind = BEER_VULKAN_COMPUTE_BINDING_IMAGE,
          .image_format = 37, .image_bytes = 4, .image_width = 1,
          .image_height = 1, .image_row_pitch = 4, .image_dimension = 2,
          .image_layers = 1, .image_depth = 1, .image_mip_levels = 1 },
        { .binding = 1, .data = mip_source, .size = sizeof(mip_source),
          .kind = BEER_VULKAN_COMPUTE_BINDING_IMAGE, .image_format = 37,
          .image_bytes = 4, .image_width = 4, .image_height = 4,
          .image_row_pitch = 16, .image_dimension = 2, .image_layers = 1,
          .image_depth = 1, .image_mip_levels = 2 },
        { .binding = 2, .kind = BEER_VULKAN_COMPUTE_BINDING_SAMPLER,
          .address_u = 2, .address_v = 2, .address_w = 2 }
    };
    CHECK(beer_vulkan_compute_dispatch(mip_spirv, mip_spirv, mip_size,
                                       mip_bindings, 3, 1, 1, 1, &reason));
    CHECK(mip_target[0] == 0 && mip_target[1] == 255 &&
          mip_target[2] == 0 && mip_target[3] == 255);

    /* Cube images retain six face layers and create a cube-compatible Vulkan
     * image/view. +X samples face zero, which is solid red here. */
    uint8_t cube_source[2 * 2 * 4 * 6];
    uint8_t cube_target[4] = {0, 0, 0, 0};
    memset(cube_source, 0, sizeof(cube_source));
    for (uint32_t face = 0; face < 6; ++face)
        for (uint32_t texel = 0; texel < 4; ++texel) {
            uint8_t *pixel = cube_source + (face * 4 + texel) * 4;
            pixel[face % 3] = 255;
            pixel[3] = 255;
        }
    BeerVulkanComputeBinding cube_bindings[3] = {
        { .binding = 0, .data = cube_target, .size = sizeof(cube_target),
          .writable = 1, .kind = BEER_VULKAN_COMPUTE_BINDING_IMAGE,
          .image_format = 37, .image_bytes = 4, .image_width = 1,
          .image_height = 1, .image_row_pitch = 4, .image_dimension = 2,
          .image_layers = 1, .image_depth = 1 },
        { .binding = 1, .data = cube_source, .size = sizeof(cube_source),
          .kind = BEER_VULKAN_COMPUTE_BINDING_IMAGE, .image_format = 37,
          .image_bytes = 4, .image_width = 2, .image_height = 2,
          .image_row_pitch = 8, .image_dimension = 2, .image_cube = 1,
          .image_layers = 6, .image_depth = 1 },
        { .binding = 2, .kind = BEER_VULKAN_COMPUTE_BINDING_SAMPLER,
          .address_u = 2, .address_v = 2, .address_w = 2 }
    };
    CHECK(beer_vulkan_compute_dispatch(cube_spirv, cube_spirv, cube_size,
                                       cube_bindings, 3, 1, 1, 1, &reason));
    CHECK(cube_target[0] == 255 && cube_target[1] == 0 &&
          cube_target[2] == 0 && cube_target[3] == 255);

    /* One-dimensional sampled/storage images require the Sampled1D/Image1D
     * SPIR-V capabilities. These are core Vulkan capabilities, not optional
     * physical-device features. */
    uint32_t source_1d[8], target_1d[8];
    for (uint32_t x = 0; x < 8; ++x) {
        source_1d[x] = 0x100u + x;
        target_1d[x] = 0;
    }
    BeerVulkanComputeBinding image_1d_bindings[3] = {
        { .binding = 0, .data = (uint8_t *)target_1d,
          .size = sizeof(target_1d), .writable = 1,
          .kind = BEER_VULKAN_COMPUTE_BINDING_IMAGE,
          .image_format = 98, /* VK_FORMAT_R32_UINT */
          .image_bytes = 4, .image_width = 8, .image_height = 1,
          .image_row_pitch = 32, .image_dimension = 1,
          .image_layers = 1, .image_depth = 1 },
        { .binding = 1, .data = (uint8_t *)source_1d,
          .size = sizeof(source_1d),
          .kind = BEER_VULKAN_COMPUTE_BINDING_IMAGE,
          .image_format = 98, .image_bytes = 4, .image_width = 8,
          .image_height = 1, .image_row_pitch = 32,
          .image_dimension = 1, .image_layers = 1, .image_depth = 1 },
        { .binding = 2, .kind = BEER_VULKAN_COMPUTE_BINDING_SAMPLER,
          .address_u = 2, .address_v = 2, .address_w = 2 }
    };
    if (!beer_vulkan_compute_dispatch(image_1d_spirv, image_1d_spirv,
                                       image_1d_size, image_1d_bindings, 3,
                                       1, 1, 1, &reason)) {
        fprintf(stderr, "1D compute failed: %s\n", reason ? reason : "?");
        CHECK(0);
    }
    for (uint32_t x = 0; x < 8; ++x) CHECK(target_1d[x] == source_1d[x]);

    /* Trailing host allocation padding is legal; only the described image
     * extent is uploaded and sampled. */
    uint8_t padded_texture[SOURCE_PITCH * 8 + 16];
    memcpy(padded_texture, texture, sizeof(texture));
    memset(padded_texture + sizeof(texture), 0xee, 16);
    memset(target, 0, sizeof(target));
    image_bindings[1].data = padded_texture;
    image_bindings[1].size = texture_size + 16;
    image_bindings[1].image_row_pitch = SOURCE_PITCH;
    CHECK(beer_vulkan_compute_dispatch(image_spirv, image_spirv, image_size,
                                       image_bindings, 3, 1, 1, 1, &reason));
    CHECK(target[2] == 0 && target[3] == 255);

    /* Immutable sampler objects are shared across compatible dispatches.
     * Repeating the same sampled-image operation must hit the cache rather than
     * create and destroy another Vulkan sampler. */
    BeerVulkanComputeStats sampler_stats_before = {0};
    BeerVulkanComputeStats sampler_stats_after = {0};
    beer_vulkan_compute_get_stats(&sampler_stats_before);
    memset(target, 0, sizeof(target));
    CHECK(beer_vulkan_compute_dispatch(image_spirv, image_spirv, image_size,
                                       image_bindings, 3, 1, 1, 1, &reason));
    beer_vulkan_compute_get_stats(&sampler_stats_after);
    CHECK(sampler_stats_after.sampler_creations ==
          sampler_stats_before.sampler_creations);
    CHECK(sampler_stats_after.sampler_cache_hits >
          sampler_stats_before.sampler_cache_hits);
    CHECK(sampler_stats_after.descriptor_set_allocations ==
          sampler_stats_before.descriptor_set_allocations);
    CHECK(sampler_stats_after.descriptor_set_cache_hits >
          sampler_stats_before.descriptor_set_cache_hits);
    CHECK(sampler_stats_after.image_cache_hits >
          sampler_stats_before.image_cache_hits);

    /* Dispatches larger than the conservative scheduling tile preserve global
     * invocation IDs through vkCmdDispatchBase. Exercise 17 groups so the last
     * group is emitted as a separate Vulkan submission and must still address
     * element 1024 rather than starting again at zero. */
    enum { TILED_GROUPS = 17, TILED_COUNT = TILED_GROUPS * 64 };
    uint32_t *tiled_source = malloc(TILED_COUNT * sizeof(*tiled_source));
    uint32_t *tiled_target = malloc(TILED_COUNT * sizeof(*tiled_target));
    CHECK(tiled_source && tiled_target);
    for (uint32_t i = 0; i < TILED_COUNT; ++i) {
        tiled_source[i] = i * 5u;
        tiled_target[i] = 0xffffffffu;
    }
    uint32_t tiled_params[4] = { TILED_COUNT, 3, 7, 0 };
    BeerVulkanComputeBinding tiled_bindings[3] = {
        {0, 0, (uint8_t *)tiled_source,
         TILED_COUNT * sizeof(*tiled_source), 0},
        {1, 0, (uint8_t *)tiled_target,
         TILED_COUNT * sizeof(*tiled_target), 1},
        {2, 1, (uint8_t *)tiled_params, sizeof(tiled_params), 0}
    };
    BeerVulkanComputeStats tiled_stats_before = {0};
    BeerVulkanComputeStats tiled_stats_after = {0};
    beer_vulkan_compute_get_stats(&tiled_stats_before);
    CHECK(!setenv("BEER_VULKAN_COMPUTE_TILE_GROUPS", "16", 1));
    CHECK(beer_vulkan_compute_dispatch(buffer_spirv, buffer_spirv, buffer_size,
                                       tiled_bindings, 3, TILED_GROUPS, 1, 1,
                                       &reason));
    CHECK(!unsetenv("BEER_VULKAN_COMPUTE_TILE_GROUPS"));
    beer_vulkan_compute_get_stats(&tiled_stats_after);
    CHECK(tiled_stats_after.queue_submissions >=
          tiled_stats_before.queue_submissions + 2);
    CHECK(tiled_stats_after.completed_submissions >=
          tiled_stats_before.completed_submissions + 2);
    CHECK(tiled_stats_after.host_fence_waits ==
          tiled_stats_before.host_fence_waits + 1);
    CHECK(tiled_target[0] == 7u);
    CHECK(tiled_target[16 * 64] == tiled_source[16 * 64] * 3u + 7u);
    CHECK(tiled_target[TILED_COUNT - 1] ==
          tiled_source[TILED_COUNT - 1] * 3u + 7u);
    free(tiled_target);
    free(tiled_source);

    /* Loop+barrier storage-image writers use the previous completed
     * 16-workgroup submission boundary without serializing every workgroup. */
    uint32_t watchdog_volume[4] = {0, 0, 0, 0};
    BeerVulkanComputeBinding watchdog_binding = {
        .binding = 0, .data = (uint8_t *)watchdog_volume,
        .size = sizeof(watchdog_volume), .writable = 1,
        .kind = BEER_VULKAN_COMPUTE_BINDING_IMAGE, .image_format = 37,
        .image_bytes = 4, .image_width = 4, .image_height = 1,
        .image_row_pitch = 16, .image_dimension = 3, .image_depth = 1
    };
    beer_vulkan_compute_get_stats(&tiled_stats_before);
    CHECK(beer_vulkan_compute_dispatch(
        watchdog_volume_spirv, watchdog_volume_spirv, watchdog_volume_size,
        &watchdog_binding, 1, 4, 1, 1, &reason));
    beer_vulkan_compute_get_stats(&tiled_stats_after);
    CHECK(tiled_stats_after.queue_submissions ==
          tiled_stats_before.queue_submissions + 1);
    CHECK(tiled_stats_after.completed_submissions ==
          tiled_stats_before.completed_submissions + 1);
    CHECK(tiled_stats_after.host_fence_waits ==
          tiled_stats_before.host_fence_waits + 1);
    CHECK(watchdog_volume[0] != 0);
    CHECK(watchdog_volume[1] != 0);
    CHECK(watchdog_volume[2] != 0);
    CHECK(watchdog_volume[3] != 0);

    /* Oversized dispatches are rejected. */
    CHECK(!beer_vulkan_compute_dispatch(buffer_spirv, buffer_spirv,
                                        buffer_size, bindings, 3, 0xffffffffu,
                                        1, 1, &reason));

    beer_vulkan_compute_destroy();
    free(buffer_spirv);
    free(image_spirv);
    free(array_spirv);
    free(volume_spirv);
    free(mip_spirv);
    free(cube_spirv);
    free(image_1d_spirv);
    free(watchdog_volume_spirv);
    printf("vulkan compute tests passed\n");
    return 0;
}
