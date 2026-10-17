#include "vulkan_graphics.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
    long length = ftell(file);
    if (length <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file); return NULL;
    }
    uint8_t *data = malloc((size_t)length);
    if (!data || fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data); fclose(file); return NULL;
    }
    fclose(file);
    *size = (size_t)length;
    return data;
}

int main(int argc, char **argv)
{
    if (argc < 3 || argc > 17) return 1;
    size_t vertex_size = 0, pixel_size = 0;
    uint8_t *vertex = read_file(argv[1], &vertex_size);
    uint8_t *pixel = read_file(argv[2], &pixel_size);
    if (!vertex || !pixel) return 2;
    enum { WIDTH = 32, HEIGHT = 24 };
    uint8_t target_bytes[WIDTH * HEIGHT * 4];
    memset(target_bytes, 0, sizeof(target_bytes));
    uint64_t serial = 4;
    BeerVulkanGraphicsTarget target = {
        .resource = &target,
        .data = target_bytes,
        .size = sizeof(target_bytes),
        .serial = &serial,
        .format = 37, /* VK_FORMAT_R8G8B8A8_UNORM */
        .bytes_per_pixel = 4,
        .width = WIDTH,
        .height = HEIGHT,
        .row_pitch = WIDTH * 4
    };
    const float viewport[6] = {0, 0, WIDTH, HEIGHT, 0, 1};
    const int32_t scissor[4] = {0, 0, WIDTH, HEIGHT};
    const char *reason = NULL;
    BeerVulkanGraphicsStats framebuffer_stats_before = {0};
    BeerVulkanGraphicsStats framebuffer_stats_after = {0};
    BeerVulkanGraphicsStats texel_stats_before = {0};
    BeerVulkanGraphicsStats texel_stats_after = {0};
    beer_vulkan_graphics_get_stats(&framebuffer_stats_before);
    int executed = beer_vulkan_graphics_draw_fullscreen(
        vertex, (const uint32_t *)vertex, vertex_size,
        pixel, (const uint32_t *)pixel, pixel_size,
        NULL, 0, NULL, &target, 1, NULL, viewport, scissor, &reason);
    if (!executed) {
        fprintf(stderr, "graphics execution failed: %s\n",
                reason ? reason : "unknown");
        return 3;
    }
    size_t center = ((HEIGHT / 2) * WIDTH + WIDTH / 2) * 4;
    if (serial != 5 || target_bytes[center] < 60 ||
        target_bytes[center + 1] < 120 || target_bytes[center + 2] < 185 ||
        target_bytes[center + 3] < 250)
        return 4;

    /* A second compatible draw must reuse the framebuffer retained for the
     * same render pass and image view. This immutable-object lifetime is a
     * prerequisite for recording several draws into one graphics batch. */
    target.resource = &target;
    if (!beer_vulkan_graphics_draw_fullscreen(
            vertex, (const uint32_t *)vertex, vertex_size,
            pixel, (const uint32_t *)pixel, pixel_size,
            NULL, 0, NULL, &target, 1, NULL, viewport, scissor, &reason))
        return 126;
    beer_vulkan_graphics_get_stats(&framebuffer_stats_after);
    if (framebuffer_stats_after.framebuffer_creations !=
            framebuffer_stats_before.framebuffer_creations + 1 ||
        framebuffer_stats_after.framebuffer_cache_hits <
            framebuffer_stats_before.framebuffer_cache_hits + 1) {
        fprintf(stderr,
                "framebuffer cache mismatch creations=%llu->%llu hits=%llu->%llu\n",
                (unsigned long long)framebuffer_stats_before.framebuffer_creations,
                (unsigned long long)framebuffer_stats_after.framebuffer_creations,
                (unsigned long long)framebuffer_stats_before.framebuffer_cache_hits,
                (unsigned long long)framebuffer_stats_after.framebuffer_cache_hits);
        return 127;
    }

    /* Intermediate GPU-generated draws may defer host publication. The
     * authoritative mirror and serial must advance without touching CPU bytes;
     * an explicit publication boundary then retrieves the exact result. */
    target.resource = &target;
    target.defer_publication = 1;
    uint8_t deferred_before[4];
    memcpy(deferred_before, target_bytes + center, sizeof(deferred_before));
    uint64_t deferred_input_serial = serial;
    if (!beer_vulkan_graphics_draw_fullscreen(
            vertex, (const uint32_t *)vertex, vertex_size,
            pixel, (const uint32_t *)pixel, pixel_size,
            NULL, 0, NULL, &target, 1, NULL, viewport, scissor, &reason) ||
        serial != deferred_input_serial + 1 ||
        memcmp(target_bytes + center, deferred_before,
               sizeof(deferred_before)) != 0)
        return 124;
    if (!beer_vulkan_graphics_sync_resource(
            target.resource, serial, target_bytes, sizeof(target_bytes), WIDTH,
            HEIGHT, target.format, target.bytes_per_pixel) ||
        target_bytes[center] < 60 || target_bytes[center + 1] < 120 ||
        target_bytes[center + 2] < 185 || target_bytes[center + 3] < 250) {
        fprintf(stderr,
                "deferred graphics publication failed serial=%llu pixel=%u,%u,%u,%u\n",
                (unsigned long long)serial, target_bytes[center],
                target_bytes[center + 1], target_bytes[center + 2],
                target_bytes[center + 3]);
        return 125;
    }
    target.defer_publication = 0;

    if (argc >= 4) {
        size_t cb_pixel_size = 0;
        uint8_t *cb_pixel = read_file(argv[3], &cb_pixel_size);
        float color[4] = {0.75f, 0.25f, 0.5f, 1.0f};
        BeerVulkanGraphicsBinding binding = {
            .binding = 32768,
            .kind = BEER_VULKAN_GRAPHICS_BINDING_UNIFORM_BUFFER,
            .stage_flags = BEER_VULKAN_GRAPHICS_STAGE_FRAGMENT,
            .data = (const uint8_t *)color,
            .size = sizeof(color)
        };
        memset(target_bytes, 0, sizeof(target_bytes));
        if (!cb_pixel || !beer_vulkan_graphics_draw_fullscreen(
                vertex, (const uint32_t *)vertex, vertex_size,
                cb_pixel, (const uint32_t *)cb_pixel, cb_pixel_size,
                &binding, 1, NULL, &target, 1, NULL, viewport, scissor, &reason)) {
            fprintf(stderr, "uniform graphics execution failed: %s\n",
                    reason ? reason : "unknown");
            free(cb_pixel);
            return 5;
        }
        free(cb_pixel);
        if (target_bytes[center] < 180 || target_bytes[center] > 200 ||
            target_bytes[center + 1] < 55 || target_bytes[center + 1] > 75 ||
            target_bytes[center + 2] < 120 || target_bytes[center + 2] > 140)
            return 6;
    }
    if (argc >= 5) {
        size_t depth_pixel_size = 0;
        uint8_t *depth_pixel = read_file(argv[4], &depth_pixel_size);
        uint8_t packed_depth[4 * 4 * 8];
        for (size_t texel = 0; texel < 16; ++texel) {
            float depth = 0.5f;
            memcpy(packed_depth + texel * 8, &depth, sizeof(depth));
            memset(packed_depth + texel * 8 + 4, 0xa5, 4);
        }
        BeerVulkanGraphicsBinding depth_bindings[2] = {
            {
                .binding = 0,
                .kind = BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_DEPTH,
                .stage_flags = BEER_VULKAN_GRAPHICS_STAGE_FRAGMENT,
                .data = packed_depth,
                .size = sizeof(packed_depth),
                .format = 126, /* VK_FORMAT_D32_SFLOAT */
                .bytes_per_pixel = 4,
                .source_bytes_per_pixel = 8,
                .width = 4,
                .height = 4,
                .row_pitch = 4 * 8,
                .aspect = 1
            },
            {
                .binding = 1,
                .kind = BEER_VULKAN_GRAPHICS_BINDING_SAMPLER,
                .stage_flags = BEER_VULKAN_GRAPHICS_STAGE_FRAGMENT,
                .min_filter = 0,
                .mag_filter = 0,
                .address_u = 2,
                .address_v = 2,
                .address_w = 2
            }
        };
        memset(target_bytes, 0, sizeof(target_bytes));
        if (!depth_pixel || !beer_vulkan_graphics_draw_fullscreen(
                vertex, (const uint32_t *)vertex, vertex_size,
                depth_pixel, (const uint32_t *)depth_pixel, depth_pixel_size,
                depth_bindings, 2, NULL, &target, 1, NULL, viewport, scissor, &reason)) {
            fprintf(stderr, "depth graphics execution failed: %s\n",
                    reason ? reason : "unknown");
            free(depth_pixel);
            return 7;
        }
        free(depth_pixel);
        if (target_bytes[center] < 120 || target_bytes[center] > 136 ||
            target_bytes[center + 1] < 120 || target_bytes[center + 1] > 136 ||
            target_bytes[center + 2] < 120 || target_bytes[center + 2] > 136) {
            fprintf(stderr, "depth sample pixel=%u,%u,%u,%u\n",
                    target_bytes[center], target_bytes[center + 1],
                    target_bytes[center + 2], target_bytes[center + 3]);
            return 8;
        }
    }
    if (argc >= 6) {
        size_t buffer_pixel_size = 0;
        uint8_t *buffer_pixel = read_file(argv[5], &buffer_pixel_size);
        float color[4] = {0.25f, 0.75f, 0.125f, 1.0f};
        BeerVulkanGraphicsBinding binding = {
            .binding = 0,
            .kind = BEER_VULKAN_GRAPHICS_BINDING_STORAGE_BUFFER,
            .stage_flags = BEER_VULKAN_GRAPHICS_STAGE_FRAGMENT,
            .data = (const uint8_t *)color,
            .size = sizeof(color)
        };
        memset(target_bytes, 0, sizeof(target_bytes));
        if (!buffer_pixel || !beer_vulkan_graphics_draw_fullscreen(
                vertex, (const uint32_t *)vertex, vertex_size,
                buffer_pixel, (const uint32_t *)buffer_pixel,
                buffer_pixel_size, &binding, 1, NULL, &target, 1, NULL, viewport,
                scissor, &reason)) {
            fprintf(stderr, "buffer graphics execution failed: %s\n",
                    reason ? reason : "unknown");
            free(buffer_pixel);
            return 9;
        }
        free(buffer_pixel);
        if (target_bytes[center] < 55 || target_bytes[center] > 75 ||
            target_bytes[center + 1] < 180 || target_bytes[center + 1] > 200 ||
            target_bytes[center + 2] < 25 || target_bytes[center + 2] > 40)
            return 10;
    }
    if (argc >= 8) {
        size_t indexed_vertex_size = 0, indexed_pixel_size = 0;
        uint8_t *indexed_vertex = read_file(argv[6], &indexed_vertex_size);
        uint8_t *indexed_pixel = read_file(argv[7], &indexed_pixel_size);
        struct IndexedVertex {
            float position[3];
            uint8_t color[4];
            uint16_t texcoord[2];
        } vertices[4] = {
            {{-0.75f, -0.75f, 0.0f}, {255, 32, 16, 255}, {0, 0}},
            {{-0.75f,  0.75f, 0.0f}, {255, 32, 16, 255}, {0, 0}},
            {{ 0.75f, -0.75f, 0.0f}, {255, 32, 16, 255}, {0, 0}},
            {{ 0.75f,  0.75f, 0.0f}, {255, 32, 16, 255}, {0, 0}}
        };
        const uint16_t indices[4] = {0, 1, 2, 3};
        BeerVulkanGraphicsDrawInfo indexed = {0};
        indexed.vertex_bindings[0] = (BeerVulkanGraphicsVertexBinding){
            .data = (const uint8_t *)vertices,
            .size = sizeof(vertices),
            .stride = sizeof(vertices[0])
        };
        indexed.vertex_binding_count = 1;
        indexed.vertex_attributes[0] = (BeerVulkanGraphicsVertexAttribute){
            .location = 0, .binding = 0, .format = 106, .offset = 0
        };
        indexed.vertex_attributes[1] = (BeerVulkanGraphicsVertexAttribute){
            .location = 1, .binding = 0, .format = 37, .offset = 12
        };
        indexed.vertex_attributes[2] = (BeerVulkanGraphicsVertexAttribute){
            .location = 2, .binding = 0, .format = 83, .offset = 16
        };
        indexed.vertex_attribute_count = 3;
        indexed.index_data = (const uint8_t *)indices;
        indexed.index_size = sizeof(indices);
        indexed.index_type = 0;
        indexed.topology = 4; /* VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP */
        /* Direct malformed ranges remain rejected. GPU-generated indirect
         * ranges use robust zero padding so they cannot read adjacent staging
         * allocations or hang the device. */
        BeerVulkanGraphicsDrawInfo invalid_indexed = indexed;
        invalid_indexed.indexed = 1;
        invalid_indexed.instance_count = 1;
        invalid_indexed.start_element = 3;
        invalid_indexed.element_count = 2;
        BeerVulkanGraphicsState default_state = {.write_mask = 0xf};
        memset(target_bytes, 0, sizeof(target_bytes));
        if (beer_vulkan_graphics_draw_state(
                indexed_vertex, (const uint32_t *)indexed_vertex,
                indexed_vertex_size, indexed_pixel,
                (const uint32_t *)indexed_pixel, indexed_pixel_size,
                NULL, 0, &invalid_indexed, &default_state, &target, 1, NULL,
                viewport, scissor, &reason)) {
            free(indexed_vertex);
            free(indexed_pixel);
            return 11;
        }
        invalid_indexed.robust_index_access = 1;
        if (!beer_vulkan_graphics_draw_state(
                indexed_vertex, (const uint32_t *)indexed_vertex,
                indexed_vertex_size, indexed_pixel,
                (const uint32_t *)indexed_pixel, indexed_pixel_size,
                NULL, 0, &invalid_indexed, &default_state, &target, 1, NULL,
                viewport, scissor, &reason)) {
            free(indexed_vertex);
            free(indexed_pixel);
            return 11;
        }
        indexed.indexed = 1;
        indexed.element_count = 4;
        indexed.instance_count = 1;
        memset(target_bytes, 0, sizeof(target_bytes));
        if (!indexed_vertex || !indexed_pixel ||
            !beer_vulkan_graphics_draw_fullscreen(
                indexed_vertex, (const uint32_t *)indexed_vertex,
                indexed_vertex_size, indexed_pixel,
                (const uint32_t *)indexed_pixel, indexed_pixel_size,
                NULL, 0, &indexed, &target, 1, NULL, viewport, scissor, &reason)) {
            fprintf(stderr, "indexed graphics execution failed: %s\n",
                    reason ? reason : "unknown");
            free(indexed_vertex);
            free(indexed_pixel);
            return 11;
        }
        if (target_bytes[center] < 220 || target_bytes[center + 1] > 50 ||
            target_bytes[center + 2] > 35 || target_bytes[center + 3] < 250)
            return 12;

        /* A generated ExecuteIndirect run with shared immutable state must be
         * recorded into one render pass and one queue submission. The two
         * commands render the same quad; additive blending proves ordering and
         * the renderer counters prove batching rather than scalar replay. */
        BeerVulkanGraphicsDrawArguments batch_arguments[2] = {
            {4, 1, 0, 0, 0},
            {4, 1, 0, 0, 0}
        };
        BeerVulkanGraphicsDrawInfo batched = indexed;
        BeerVulkanGraphicsState batch_additive = {
            .blend_enable = 1,
            .src_color_factor = 1,
            .dst_color_factor = 1,
            .color_op = 0,
            .src_alpha_factor = 1,
            .dst_alpha_factor = 1,
            .alpha_op = 0,
            .write_mask = 0xf
        };
        batched.batch_arguments = batch_arguments;
        batched.batch_argument_count = 2;
        memset(target_bytes, 0, sizeof(target_bytes));
        ++serial;
        BeerVulkanGraphicsStats batch_stats_before = {0};
        BeerVulkanGraphicsStats batch_stats_after = {0};
        beer_vulkan_graphics_get_stats(&batch_stats_before);
        if (!beer_vulkan_graphics_draw_state(
                indexed_vertex, (const uint32_t *)indexed_vertex,
                indexed_vertex_size, indexed_pixel,
                (const uint32_t *)indexed_pixel, indexed_pixel_size,
                NULL, 0, &batched, &batch_additive, &target, 1, NULL,
                viewport, scissor, &reason))
            return 124;
        beer_vulkan_graphics_get_stats(&batch_stats_after);
        if (batch_stats_after.queue_submissions !=
                batch_stats_before.queue_submissions + 1 ||
            batch_stats_after.fence_waits != batch_stats_before.fence_waits + 1 ||
            batch_stats_after.draw_commands !=
                batch_stats_before.draw_commands + 2 ||
            target_bytes[center] < 240)
            return 125;

        /* D3D12 triangle strips commonly terminate with the configured 16-bit
         * strip-cut value. Vulkan must enable primitive restart or the sentinel
         * is fetched as vertex 65535 and can create a giant screen triangle. */
        struct {
            float position[3];
            uint8_t color[4];
            uint16_t texcoord[2];
        } restart_vertices[8];
        for (uint32_t i = 0; i < 8; ++i) {
            memcpy(&restart_vertices[i], &vertices[i & 3u],
                   sizeof(restart_vertices[i]));
            restart_vertices[i].position[0] += i >= 4 ? 0.1f : -0.1f;
        }
        const uint16_t restart_indices[10] = {
            0, 1, 2, 3, UINT16_MAX, 4, 5, 6, 7, UINT16_MAX
        };
        indexed.vertex_bindings[0].data =
            (const uint8_t *)restart_vertices;
        indexed.vertex_bindings[0].size = sizeof(restart_vertices);
        indexed.index_data = (const uint8_t *)restart_indices;
        indexed.index_size = sizeof(restart_indices);
        indexed.element_count = 10;
        BeerVulkanGraphicsState restart_state = {
            .write_mask = 0xf, .primitive_restart_enable = 1
        };
        memset(target_bytes, 0, sizeof(target_bytes));
        if (!beer_vulkan_graphics_draw_state(
                indexed_vertex, (const uint32_t *)indexed_vertex,
                indexed_vertex_size, indexed_pixel,
                (const uint32_t *)indexed_pixel, indexed_pixel_size,
                NULL, 0, &indexed, &restart_state, &target, 1, NULL,
                viewport, scissor, &reason) || target_bytes[center] < 220)
            return 13;
        indexed.vertex_bindings[0].data = (const uint8_t *)vertices;
        indexed.vertex_bindings[0].size = sizeof(vertices);
        indexed.index_data = (const uint8_t *)indices;
        indexed.index_size = sizeof(indices);
        indexed.element_count = 4;

        /* Fixed-function state. The quad occupies NDC y in [0.5, 1.0], which
         * is the top of the image under D3D conventions. */
        for (size_t v = 0; v < 4; ++v)
            vertices[v].position[1] = vertices[v].position[1] > 0.0f ?
                1.0f : 0.5f;
        const size_t top_pixel = (3 * WIDTH + WIDTH / 2) * 4;
        const size_t bottom_pixel = (20 * WIDTH + WIDTH / 2) * 4;
        BeerVulkanGraphicsState flip = {.write_mask = 0xf, .flip_y = 1};
        memset(target_bytes, 0, sizeof(target_bytes));
        ++serial;
        if (!beer_vulkan_graphics_draw_state(
                indexed_vertex, (const uint32_t *)indexed_vertex,
                indexed_vertex_size, indexed_pixel,
                (const uint32_t *)indexed_pixel, indexed_pixel_size,
                NULL, 0, &indexed, &flip, &target, 1, NULL, viewport, scissor,
                &reason)) {
            fprintf(stderr, "flipped graphics failed: %s\n",
                    reason ? reason : "unknown");
            return 13;
        }
        if (target_bytes[top_pixel] < 220 || target_bytes[bottom_pixel] != 0)
            return 14;

        /* Additive blending on top of existing target contents. */
        BeerVulkanGraphicsState additive = {
            .blend_enable = 1, .src_color_factor = 1, .dst_color_factor = 1,
            .color_op = 0, .src_alpha_factor = 1, .dst_alpha_factor = 0,
            .alpha_op = 0, .write_mask = 0xf, .flip_y = 1};
        for (size_t texel = 0; texel < WIDTH * HEIGHT; ++texel) {
            target_bytes[texel * 4] = 20;
            target_bytes[texel * 4 + 1] = 200;
            target_bytes[texel * 4 + 2] = 100;
            target_bytes[texel * 4 + 3] = 255;
        }
        ++serial;
        if (!beer_vulkan_graphics_draw_state(
                indexed_vertex, (const uint32_t *)indexed_vertex,
                indexed_vertex_size, indexed_pixel,
                (const uint32_t *)indexed_pixel, indexed_pixel_size,
                NULL, 0, &indexed, &additive, &target, 1, NULL, viewport, scissor,
                &reason))
            return 15;
        if (target_bytes[top_pixel] < 240 || target_bytes[top_pixel + 1] < 200 ||
            target_bytes[top_pixel + 1] > 240 ||
            target_bytes[top_pixel + 2] < 100 ||
            target_bytes[top_pixel + 2] > 135 ||
            target_bytes[bottom_pixel] != 20 ||
            target_bytes[bottom_pixel + 1] != 200)
            return 16;

        /* Color write mask: green only keeps red/blue/alpha of the target. */
        BeerVulkanGraphicsState green_only = {.write_mask = 0x2, .flip_y = 1};
        memset(target_bytes, 7, sizeof(target_bytes));
        ++serial;
        if (!beer_vulkan_graphics_draw_state(
                indexed_vertex, (const uint32_t *)indexed_vertex,
                indexed_vertex_size, indexed_pixel,
                (const uint32_t *)indexed_pixel, indexed_pixel_size,
                NULL, 0, &indexed, &green_only, &target, 1, NULL, viewport,
                scissor, &reason))
            return 17;
        if (target_bytes[top_pixel] != 7 || target_bytes[top_pixel + 2] != 7 ||
            target_bytes[top_pixel + 3] != 7)
            return 18;

        /* Depth attachment: a 0.25 fragment passes LESS against 0.5, while
         * the same geometry at 0.75 is rejected. Packed 8-byte host texels
         * exercise D32 extraction from R32G8X24-like storage. */
        uint8_t packed_attachment[WIDTH * HEIGHT * 8];
        for (size_t texel = 0; texel < WIDTH * HEIGHT; ++texel) {
            float value = 0.5f;
            memcpy(packed_attachment + texel * 8, &value, sizeof(value));
            memset(packed_attachment + texel * 8 + 4, 0xa5, 4);
        }
        uint64_t depth_serial = 9;
        BeerVulkanGraphicsDepthTarget depth_target = {
            .data = packed_attachment,
            .size = sizeof(packed_attachment),
            .serial = &depth_serial,
            .format = 126,
            .bytes_per_pixel = 4,
            .source_bytes_per_pixel = 8,
            .width = WIDTH,
            .height = HEIGHT,
            .row_pitch = WIDTH * 8
        };
        BeerVulkanGraphicsState depth_state = {
            .write_mask = 0xf, .depth_test_enable = 1,
            .depth_write_enable = 0, .depth_compare_op = 1, /* LESS */
            .flip_y = 1
        };
        for (size_t v = 0; v < 4; ++v) vertices[v].position[2] = 0.25f;
        memset(target_bytes, 0, sizeof(target_bytes));
        if (!beer_vulkan_graphics_draw_state(
                indexed_vertex, (const uint32_t *)indexed_vertex,
                indexed_vertex_size, indexed_pixel,
                (const uint32_t *)indexed_pixel, indexed_pixel_size,
                NULL, 0, &indexed, &depth_state, &target, 1, &depth_target,
                viewport, scissor, &reason))
            return 19;
        if (target_bytes[top_pixel] < 220 || depth_serial != 9)
            return 20;

        /* A depth-only D3D12 pass is valid with no bound color render target.
         * This is common for shadow-map and visibility prepasses. */
        BeerVulkanGraphicsState depth_only_state = depth_state;
        depth_only_state.depth_write_enable = 1;
        const uint64_t depth_only_serial = depth_serial;
        if (!beer_vulkan_graphics_draw_state(
                indexed_vertex, (const uint32_t *)indexed_vertex,
                indexed_vertex_size, indexed_pixel,
                (const uint32_t *)indexed_pixel, indexed_pixel_size,
                NULL, 0, &indexed, &depth_only_state, NULL, 0, &depth_target,
                viewport, scissor, &reason)) {
            fprintf(stderr, "depth-only graphics failed: %s\n",
                    reason ? reason : "unknown");
            return 21;
        }
        if (depth_serial != depth_only_serial + 1) return 22;

        /* D3D12 depth-only PSOs may omit the pixel shader entirely. Vulkan
         * likewise permits a vertex-only pipeline when no color attachments
         * are present. */
        const uint64_t vertex_only_depth_serial = depth_serial;
        if (!beer_vulkan_graphics_draw_state(
                indexed_vertex, (const uint32_t *)indexed_vertex,
                indexed_vertex_size, NULL, NULL, 0,
                NULL, 0, &indexed, &depth_only_state, NULL, 0, &depth_target,
                viewport, scissor, &reason)) {
            fprintf(stderr, "vertex-only depth graphics failed: %s\n",
                    reason ? reason : "unknown");
            return 120;
        }
        if (depth_serial != vertex_only_depth_serial + 1) return 121;

        /* Genuine multisampled depth remains GPU-resident: Vulkan forbids
         * buffer copies to or from multisampled images. Two depth-only draws
         * must therefore reuse the same retained 4x image without publishing a
         * fabricated single-sample host plane. */
        BeerVulkanGraphicsDepthTarget multisampled_depth = depth_target;
        BeerVulkanGraphicsState multisampled_depth_state = depth_only_state;
        static const uint8_t multisampled_depth_identity;
        multisampled_depth.resource = &multisampled_depth_identity;
        multisampled_depth.sample_count = 4;
        multisampled_depth_state.sample_count = 4;
        const uint64_t multisampled_serial = depth_serial;
        for (unsigned pass = 0; pass < 2; ++pass) {
            if (!beer_vulkan_graphics_draw_state(
                    indexed_vertex, (const uint32_t *)indexed_vertex,
                    indexed_vertex_size, NULL, NULL, 0,
                    NULL, 0, &indexed, &multisampled_depth_state, NULL, 0,
                    &multisampled_depth, viewport, scissor, &reason)) {
                fprintf(stderr, "4x depth-only graphics failed: %s\n",
                        reason ? reason : "unknown");
                return 122;
            }
        }
        if (depth_serial != multisampled_serial) return 123;

        /* Depth-writing draws must publish their result to the retained host
         * backing so a later synchronous draw observes the updated plane. */
        for (size_t texel = 0; texel < WIDTH * HEIGHT; ++texel) {
            float value = 0.5f;
            memcpy(packed_attachment + texel * 8, &value, sizeof(value));
        }
        depth_state.depth_write_enable = 1;
        const uint64_t serial_before_write = depth_serial;
        if (!beer_vulkan_graphics_draw_state(
                indexed_vertex, (const uint32_t *)indexed_vertex,
                indexed_vertex_size, indexed_pixel,
                (const uint32_t *)indexed_pixel, indexed_pixel_size,
                NULL, 0, &indexed, &depth_state, &target, 1, &depth_target,
                viewport, scissor, &reason))
            return 21;
        float retained_depth = 0.0f;
        memcpy(&retained_depth, packed_attachment + top_pixel / 4 * 8,
               sizeof(retained_depth));
        if (depth_serial != serial_before_write + 1 ||
            retained_depth < 0.24f || retained_depth > 0.26f)
            return 22;
        depth_state.depth_write_enable = 0;
        for (size_t v = 0; v < 4; ++v) vertices[v].position[2] = 0.75f;
        memset(target_bytes, 0, sizeof(target_bytes));
        ++serial;
        if (!beer_vulkan_graphics_draw_state(
                indexed_vertex, (const uint32_t *)indexed_vertex,
                indexed_vertex_size, indexed_pixel,
                (const uint32_t *)indexed_pixel, indexed_pixel_size,
                NULL, 0, &indexed, &depth_state, &target, 1, &depth_target,
                viewport, scissor, &reason))
            return 21;
        if (target_bytes[top_pixel] != 0) return 22;
        for (size_t v = 0; v < 4; ++v) vertices[v].position[2] = 0.0f;

        /* Culling: the same strip is front facing in exactly one winding. */
        unsigned drawn = 0;
        for (uint32_t cull = 1; cull <= 2; ++cull) {
            BeerVulkanGraphicsState culled = {
                .write_mask = 0xf, .cull_mode = cull, .flip_y = 1};
            memset(target_bytes, 0, sizeof(target_bytes));
            if (!beer_vulkan_graphics_draw_state(
                    indexed_vertex, (const uint32_t *)indexed_vertex,
                    indexed_vertex_size, indexed_pixel,
                    (const uint32_t *)indexed_pixel, indexed_pixel_size,
                    NULL, 0, &indexed, &culled, &target, 1, NULL, viewport,
                    scissor, &reason))
                return 23;
            drawn += target_bytes[top_pixel] != 0;
        }
        if (drawn != 1) return 24;

        /* Packed D32/S8 stencil attachment. The low byte after each 32-bit
         * depth value is the stencil aspect in Beer's retained host layout. */
        for (size_t texel = 0; texel < WIDTH * HEIGHT; ++texel) {
            float value = 0.5f;
            memcpy(packed_attachment + texel * 8, &value, sizeof(value));
            packed_attachment[texel * 8 + 4] = 0x3c;
            memset(packed_attachment + texel * 8 + 5, 0, 3);
        }
        depth_target.format = 130; /* VK_FORMAT_D32_SFLOAT_S8_UINT */
        depth_target.has_stencil = 1;
        depth_target.stencil_byte_offset = 4;
        BeerVulkanGraphicsState stencil_state = {
            .write_mask = 0xf,
            .stencil_test_enable = 1,
            .stencil_read_mask = 0xff,
            .stencil_write_mask = 0xff,
            .stencil_reference = 0x3c,
            .stencil_front_fail = 0, /* KEEP */
            .stencil_front_pass = 0,
            .stencil_front_depth_fail = 0,
            .stencil_front_compare = 2, /* EQUAL */
            .stencil_back_fail = 0,
            .stencil_back_pass = 0,
            .stencil_back_depth_fail = 0,
            .stencil_back_compare = 2,
            .flip_y = 1
        };
        memset(target_bytes, 0, sizeof(target_bytes));
        ++serial;
        if (!beer_vulkan_graphics_draw_state(
                indexed_vertex, (const uint32_t *)indexed_vertex,
                indexed_vertex_size, indexed_pixel,
                (const uint32_t *)indexed_pixel, indexed_pixel_size,
                NULL, 0, &indexed, &stencil_state, &target, 1, &depth_target,
                viewport, scissor, &reason))
            return 25;
        if (target_bytes[top_pixel] < 220) return 26;
        stencil_state.stencil_reference = 0x7e;
        memset(target_bytes, 0, sizeof(target_bytes));
        ++serial;
        if (!beer_vulkan_graphics_draw_state(
                indexed_vertex, (const uint32_t *)indexed_vertex,
                indexed_vertex_size, indexed_pixel,
                (const uint32_t *)indexed_pixel, indexed_pixel_size,
                NULL, 0, &indexed, &stencil_state, &target, 1, &depth_target,
                viewport, scissor, &reason))
            return 27;
        if (target_bytes[top_pixel] != 0) return 28;
        free(indexed_vertex);
        free(indexed_pixel);
    }
    if (argc >= 9) {
        size_t array_pixel_size = 0;
        uint8_t *array_pixel = read_file(argv[8], &array_pixel_size);
        uint8_t layers[2 * 4 * 4 * 4];
        memset(layers, 0, sizeof(layers));
        for (size_t texel = 0; texel < 16; ++texel) {
            layers[16 * 4 + texel * 4] = 32;
            layers[16 * 4 + texel * 4 + 1] = 160;
            layers[16 * 4 + texel * 4 + 2] = 224;
            layers[16 * 4 + texel * 4 + 3] = 255;
        }
        BeerVulkanGraphicsBinding array_bindings[2] = {
            {
                .binding = 8192,
                .kind = BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_IMAGE,
                .stage_flags = BEER_VULKAN_GRAPHICS_STAGE_FRAGMENT,
                .data = layers,
                .size = sizeof(layers),
                .format = 37,
                .bytes_per_pixel = 4,
                .width = 4,
                .height = 4,
                .row_pitch = 16,
                .layer_count = 2
            },
            {
                .binding = 40960,
                .kind = BEER_VULKAN_GRAPHICS_BINDING_SAMPLER,
                .stage_flags = BEER_VULKAN_GRAPHICS_STAGE_FRAGMENT,
                .min_filter = 0,
                .mag_filter = 0,
                .address_u = 2,
                .address_v = 2,
                .address_w = 2
            }
        };
        memset(target_bytes, 0, sizeof(target_bytes));
        if (!array_pixel || !beer_vulkan_graphics_draw_fullscreen(
                vertex, (const uint32_t *)vertex, vertex_size,
                array_pixel, (const uint32_t *)array_pixel, array_pixel_size,
                array_bindings, 2, NULL, &target, 1, NULL, viewport, scissor,
                &reason)) {
            fprintf(stderr, "array graphics execution failed: %s\n",
                    reason ? reason : "unknown");
            free(array_pixel);
            return 29;
        }
        free(array_pixel);
        if (target_bytes[center] < 25 || target_bytes[center] > 40 ||
            target_bytes[center + 1] < 150 || target_bytes[center + 1] > 170 ||
            target_bytes[center + 2] < 215 || target_bytes[center + 2] > 235)
            return 30;

        /* Large array uploads must be emitted in bounded copy batches rather
         * than rejected when mip/layer regions exceed the local command array. */
        enum { LARGE_LAYERS = 1025, LARGE_LAYER_BYTES = 4 * 4 * 4 };
        uint8_t *large_layers = calloc(LARGE_LAYERS, LARGE_LAYER_BYTES);
        if (!large_layers) return 44;
        for (size_t texel = 0; texel < 16; ++texel) {
            large_layers[LARGE_LAYER_BYTES + texel * 4] = 32;
            large_layers[LARGE_LAYER_BYTES + texel * 4 + 1] = 160;
            large_layers[LARGE_LAYER_BYTES + texel * 4 + 2] = 224;
            large_layers[LARGE_LAYER_BYTES + texel * 4 + 3] = 255;
        }
        array_bindings[0].data = large_layers;
        array_bindings[0].size = (size_t)LARGE_LAYERS * LARGE_LAYER_BYTES;
        array_bindings[0].layer_count = LARGE_LAYERS;
        array_bindings[0].layer_stride = LARGE_LAYER_BYTES;
        size_t large_pixel_size = 0;
        uint8_t *large_pixel = read_file(argv[8], &large_pixel_size);
        memset(target_bytes, 0, sizeof(target_bytes));
        if (!large_pixel || !beer_vulkan_graphics_draw_fullscreen(
                vertex, (const uint32_t *)vertex, vertex_size,
                large_pixel, (const uint32_t *)large_pixel, large_pixel_size,
                array_bindings, 2, NULL, &target, 1, NULL, viewport, scissor,
                &reason)) {
            fprintf(stderr, "large-array graphics execution failed: %s\n",
                    reason ? reason : "unknown");
            free(large_pixel);
            free(large_layers);
            return 45;
        }
        free(large_pixel);
        free(large_layers);
        if (target_bytes[center] < 25 || target_bytes[center] > 40 ||
            target_bytes[center + 1] < 150 || target_bytes[center + 1] > 170 ||
            target_bytes[center + 2] < 215 || target_bytes[center + 2] > 235)
            return 46;
    }
    if (argc >= 10) {
        size_t volume_pixel_size = 0;
        uint8_t *volume_pixel = read_file(argv[9], &volume_pixel_size);
        uint8_t volume[4 * 4 * 4 * 4];
        memset(volume, 0, sizeof(volume));
        for (size_t texel = 0; texel < 4 * 16; ++texel) {
            volume[texel * 4] = 208;
            volume[texel * 4 + 1] = 80;
            volume[texel * 4 + 2] = 24;
            volume[texel * 4 + 3] = 255;
        }
        BeerVulkanGraphicsBinding volume_bindings[2] = {
            {
                .binding = 8192,
                .kind = BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_IMAGE,
                .stage_flags = BEER_VULKAN_GRAPHICS_STAGE_FRAGMENT,
                .data = volume,
                .size = sizeof(volume),
                .format = 37,
                .bytes_per_pixel = 4,
                .width = 4,
                .height = 4,
                .row_pitch = 16,
                .layer_count = 1,
                .depth = 4,
                .image_dimension = 3
            },
            {
                .binding = 40960,
                .kind = BEER_VULKAN_GRAPHICS_BINDING_SAMPLER,
                .stage_flags = BEER_VULKAN_GRAPHICS_STAGE_FRAGMENT,
                .min_filter = 0,
                .mag_filter = 0,
                .address_u = 2,
                .address_v = 2,
                .address_w = 2
            }
        };
        memset(target_bytes, 0, sizeof(target_bytes));
        if (!volume_pixel || !beer_vulkan_graphics_draw_fullscreen(
                vertex, (const uint32_t *)vertex, vertex_size,
                volume_pixel, (const uint32_t *)volume_pixel,
                volume_pixel_size, volume_bindings, 2, NULL, &target, 1, NULL,
                viewport, scissor, &reason)) {
            fprintf(stderr, "volume graphics execution failed: %s\n",
                    reason ? reason : "unknown");
            free(volume_pixel);
            return 31;
        }
        free(volume_pixel);
        if (target_bytes[center] < 195 || target_bytes[center] > 220 ||
            target_bytes[center + 1] < 70 || target_bytes[center + 1] > 90 ||
            target_bytes[center + 2] < 15 || target_bytes[center + 2] > 35) {
            fprintf(stderr, "volume pixel mismatch: %u,%u,%u,%u\n",
                    target_bytes[center], target_bytes[center + 1],
                    target_bytes[center + 2], target_bytes[center + 3]);
            return 32;
        }
    }
    if (argc >= 11) {
        size_t line_pixel_size = 0;
        uint8_t *line_pixel = read_file(argv[10], &line_pixel_size);
        uint8_t lines[2 * 8 * 4];
        memset(lines, 0, sizeof(lines));
        for (size_t texel = 0; texel < 8; ++texel) {
            lines[8 * 4 + texel * 4] = 48;
            lines[8 * 4 + texel * 4 + 1] = 192;
            lines[8 * 4 + texel * 4 + 2] = 96;
            lines[8 * 4 + texel * 4 + 3] = 255;
        }
        BeerVulkanGraphicsBinding line_bindings[2] = {
            {
                .binding = 8192,
                .kind = BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_IMAGE,
                .stage_flags = BEER_VULKAN_GRAPHICS_STAGE_FRAGMENT,
                .data = lines,
                .size = sizeof(lines),
                .format = 37,
                .bytes_per_pixel = 4,
                .width = 8,
                .height = 1,
                .row_pitch = 32,
                .layer_count = 2,
                .depth = 1,
                .image_dimension = 1
            },
            {
                .binding = 40960,
                .kind = BEER_VULKAN_GRAPHICS_BINDING_SAMPLER,
                .stage_flags = BEER_VULKAN_GRAPHICS_STAGE_FRAGMENT,
                .min_filter = 0,
                .mag_filter = 0,
                .address_u = 2,
                .address_v = 2,
                .address_w = 2
            }
        };
        memset(target_bytes, 0, sizeof(target_bytes));
        if (!line_pixel || !beer_vulkan_graphics_draw_fullscreen(
                vertex, (const uint32_t *)vertex, vertex_size,
                argv[10], (const uint32_t *)line_pixel, line_pixel_size,
                line_bindings, 2, NULL, &target, 1, NULL, viewport, scissor,
                &reason)) {
            fprintf(stderr, "1D-array graphics execution failed: %s\n",
                    reason ? reason : "unknown");
            free(line_pixel);
            return 33;
        }
        free(line_pixel);
        if (target_bytes[center] < 40 || target_bytes[center] > 60 ||
            target_bytes[center + 1] < 180 || target_bytes[center + 1] > 205 ||
            target_bytes[center + 2] < 85 || target_bytes[center + 2] > 110) {
            fprintf(stderr, "1D-array pixel mismatch: %u,%u,%u,%u\n",
                    target_bytes[center], target_bytes[center + 1],
                    target_bytes[center + 2], target_bytes[center + 3]);
            return 34;
        }
    }
    if (argc >= 16) {
        size_t sample_pixel_size = 0;
        uint8_t *sample_pixel = read_file(argv[15], &sample_pixel_size);
        const uint64_t producer_serial = serial;
        if (!sample_pixel || !beer_vulkan_graphics_draw_fullscreen(
                vertex, (const uint32_t *)vertex, vertex_size,
                pixel, (const uint32_t *)pixel, pixel_size,
                NULL, 0, NULL, &target, 1, NULL, viewport, scissor, &reason)) {
            fprintf(stderr, "target producer graphics execution failed: %s\n",
                    reason ? reason : "unknown");
            free(sample_pixel);
            return 39;
        }
        BeerVulkanGraphicsBinding sample_bindings[2] = {
            {
                .binding = 8192,
                .kind = BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_IMAGE,
                .stage_flags = BEER_VULKAN_GRAPHICS_STAGE_FRAGMENT,
                .resource = &target,
                .serial = &serial,
                .data = target_bytes,
                .size = sizeof(target_bytes),
                .format = 37,
                .bytes_per_pixel = 4,
                .width = WIDTH,
                .height = HEIGHT,
                .row_pitch = WIDTH * 4,
                .layer_count = 1,
                .depth = 1,
                .image_dimension = 2,
                .mip_levels = 1
            },
            {
                .binding = 40960,
                .kind = BEER_VULKAN_GRAPHICS_BINDING_SAMPLER,
                .stage_flags = BEER_VULKAN_GRAPHICS_STAGE_FRAGMENT,
                .min_filter = 0,
                .mag_filter = 0,
                .address_u = 2,
                .address_v = 2,
                .address_w = 2
            }
        };
        uint8_t sampled_bytes[WIDTH * HEIGHT * 4];
        memset(sampled_bytes, 0, sizeof(sampled_bytes));
        uint64_t sampled_serial = 1;
        BeerVulkanGraphicsTarget sampled_target = target;
        sampled_target.resource = &sampled_target;
        sampled_target.data = sampled_bytes;
        sampled_target.serial = &sampled_serial;
        if (!beer_vulkan_graphics_draw_fullscreen(
                vertex, (const uint32_t *)vertex, vertex_size,
                sample_pixel, (const uint32_t *)sample_pixel,
                sample_pixel_size, sample_bindings, 2, NULL, &sampled_target,
                1, NULL, viewport, scissor, &reason)) {
            fprintf(stderr, "target-as-SRV graphics execution failed: %s\n",
                    reason ? reason : "unknown");
            free(sample_pixel);
            return 40;
        }
        free(sample_pixel);
        if (serial != producer_serial + 1 ||
            sampled_bytes[center] < 60 || sampled_bytes[center] > 70 ||
            sampled_bytes[center + 1] < 125 ||
            sampled_bytes[center + 1] > 135 ||
            sampled_bytes[center + 2] < 185 ||
            sampled_bytes[center + 2] > 200)
            return 41;
    }
    if (argc >= 15) {
        size_t mip_pixel_size = 0;
        uint8_t *mip_pixel = read_file(argv[14], &mip_pixel_size);
        uint8_t mip_source[4 * 4 * 4 + 2 * 2 * 4];
        memset(mip_source, 0, sizeof(mip_source));
        for (size_t texel = 0; texel < 4 * 4; ++texel) {
            mip_source[texel * 4] = 255;
            mip_source[texel * 4 + 3] = 255;
        }
        for (size_t texel = 0; texel < 2 * 2; ++texel) {
            uint8_t *sample = mip_source + 4 * 4 * 4 + texel * 4;
            sample[1] = 255;
            sample[3] = 255;
        }
        BeerVulkanGraphicsBinding mip_bindings[2] = {
            {
                .binding = 8192,
                .kind = BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_IMAGE,
                .stage_flags = BEER_VULKAN_GRAPHICS_STAGE_FRAGMENT,
                .data = mip_source,
                .size = sizeof(mip_source),
                .format = 37,
                .bytes_per_pixel = 4,
                .width = 4,
                .height = 4,
                .row_pitch = 16,
                .layer_count = 1,
                .depth = 1,
                .image_dimension = 2,
                .mip_levels = 2
            },
            {
                .binding = 40960,
                .kind = BEER_VULKAN_GRAPHICS_BINDING_SAMPLER,
                .stage_flags = BEER_VULKAN_GRAPHICS_STAGE_FRAGMENT,
                .min_filter = 0,
                .mag_filter = 0,
                .mipmap_mode = 0,
                .address_u = 2,
                .address_v = 2,
                .address_w = 2
            }
        };
        memset(target_bytes, 0, sizeof(target_bytes));
        if (!mip_pixel || !beer_vulkan_graphics_draw_fullscreen(
                vertex, (const uint32_t *)vertex, vertex_size,
                argv[14], (const uint32_t *)mip_pixel, mip_pixel_size,
                mip_bindings, 2, NULL, &target, 1, NULL, viewport, scissor,
                &reason)) {
            fprintf(stderr, "mip graphics execution failed: %s\n",
                    reason ? reason : "unknown");
            free(mip_pixel);
            return 39;
        }
        free(mip_pixel);
        if (target_bytes[center] > 10 || target_bytes[center + 1] < 245 ||
            target_bytes[center + 2] > 10 || target_bytes[center + 3] < 245) {
            fprintf(stderr, "mip pixel mismatch: %u,%u,%u,%u\n",
                    target_bytes[center], target_bytes[center + 1],
                    target_bytes[center + 2], target_bytes[center + 3]);
            return 40;
        }
    }
    if (argc >= 17) {
        size_t cube_pixel_size = 0;
        uint8_t *cube_pixel = read_file(argv[16], &cube_pixel_size);
        uint8_t cube_faces[6 * 2 * 2 * 4];
        memset(cube_faces, 0, sizeof(cube_faces));
        for (size_t texel = 0; texel < 6 * 4; ++texel) {
            cube_faces[texel * 4] = 224;
            cube_faces[texel * 4 + 1] = 96;
            cube_faces[texel * 4 + 2] = 32;
            cube_faces[texel * 4 + 3] = 255;
        }
        BeerVulkanGraphicsBinding cube_bindings[2] = {
            {
                .binding = 8192,
                .kind = BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_IMAGE,
                .stage_flags = BEER_VULKAN_GRAPHICS_STAGE_FRAGMENT,
                .data = cube_faces,
                .size = sizeof(cube_faces),
                .format = 37,
                .bytes_per_pixel = 4,
                .width = 2,
                .height = 2,
                .row_pitch = 8,
                .layer_count = 6,
                .depth = 1,
                .image_dimension = 2,
                .image_cube = 1
            },
            {
                .binding = 40960,
                .kind = BEER_VULKAN_GRAPHICS_BINDING_SAMPLER,
                .stage_flags = BEER_VULKAN_GRAPHICS_STAGE_FRAGMENT,
                .address_u = 2, .address_v = 2, .address_w = 2,
                .anisotropy_enable = 1, .max_anisotropy = 16.0f
            }
        };
        memset(target_bytes, 0, sizeof(target_bytes));
        if (!cube_pixel || !beer_vulkan_graphics_draw_fullscreen(
                vertex, (const uint32_t *)vertex, vertex_size,
                cube_pixel, (const uint32_t *)cube_pixel, cube_pixel_size,
                cube_bindings, 2, NULL, &target, 1, NULL, viewport, scissor,
                &reason)) {
            fprintf(stderr, "cube graphics execution failed: %s\n",
                    reason ? reason : "unknown");
            free(cube_pixel);
            return 42;
        }
        free(cube_pixel);
        if (target_bytes[center] < 210 || target_bytes[center] > 235 ||
            target_bytes[center + 1] < 85 || target_bytes[center + 1] > 110 ||
            target_bytes[center + 2] < 20 || target_bytes[center + 2] > 45) {
            fprintf(stderr, "cube pixel mismatch: %u,%u,%u,%u\n",
                    target_bytes[center], target_bytes[center + 1],
                    target_bytes[center + 2], target_bytes[center + 3]);
            return 43;
        }
    }
    /* Narrow R8/R16 attachments are valid Vulkan color targets and are used
     * for masks, visibility and material intermediates. */
    {
        uint8_t r8_bytes[WIDTH * HEIGHT];
        uint16_t r16_bytes[WIDTH * HEIGHT];
        uint64_t r8_serial = 1, r16_serial = 1;
        BeerVulkanGraphicsTarget narrow_targets[2] = {
            { .data = r8_bytes, .size = sizeof(r8_bytes),
              .serial = &r8_serial, .format = 9, .bytes_per_pixel = 1,
              .width = WIDTH, .height = HEIGHT, .row_pitch = WIDTH },
            { .data = (uint8_t *)r16_bytes, .size = sizeof(r16_bytes),
              .serial = &r16_serial, .format = 70, .bytes_per_pixel = 2,
              .width = WIDTH, .height = HEIGHT, .row_pitch = WIDTH * 2 }
        };
        for (size_t i = 0; i < 2; ++i) {
            memset(narrow_targets[i].data, 0, narrow_targets[i].size);
            if (!beer_vulkan_graphics_draw_fullscreen(
                    vertex, (const uint32_t *)vertex, vertex_size,
                    pixel, (const uint32_t *)pixel, pixel_size,
                    NULL, 0, NULL, &narrow_targets[i], 1, NULL,
                    viewport, scissor, &reason)) {
                fprintf(stderr, "narrow target graphics failed: %s\n",
                        reason ? reason : "unknown");
                return 44;
            }
            if (*narrow_targets[i].serial != 2) return 45;
        }
    }
    if (argc >= 14) {
        size_t texel_vertex_size = 0, texel_pixel_size = 0;
        uint8_t *texel_vertex = read_file(argv[12], &texel_vertex_size);
        uint8_t *texel_pixel = read_file(argv[13], &texel_pixel_size);
        float exposure = 0.75f;
        BeerVulkanGraphicsBinding texel_binding = {
            .binding = 0,
            .kind = BEER_VULKAN_GRAPHICS_BINDING_UNIFORM_TEXEL_BUFFER,
            .stage_flags = BEER_VULKAN_GRAPHICS_STAGE_VERTEX,
            .data = (const uint8_t *)&exposure,
            .size = sizeof(exposure),
            .texel_format = 98, /* VK_FORMAT_R32_UINT */
            .texel_bytes = 4
        };
        memset(target_bytes, 0, sizeof(target_bytes));
        beer_vulkan_graphics_get_stats(&texel_stats_before);
        if (!texel_vertex || !texel_pixel ||
            !beer_vulkan_graphics_draw_fullscreen(
                texel_vertex, (const uint32_t *)texel_vertex,
                texel_vertex_size, texel_pixel,
                (const uint32_t *)texel_pixel, texel_pixel_size,
                &texel_binding, 1, NULL, &target, 1, NULL, viewport, scissor,
                &reason)) {
            fprintf(stderr, "texel-buffer graphics execution failed: %s\n",
                    reason ? reason : "unknown");
            free(texel_vertex);
            free(texel_pixel);
            return 37;
        }
        memset(target_bytes, 0, sizeof(target_bytes));
        if (!beer_vulkan_graphics_draw_fullscreen(
                texel_vertex, (const uint32_t *)texel_vertex,
                texel_vertex_size, texel_pixel,
                (const uint32_t *)texel_pixel, texel_pixel_size,
                &texel_binding, 1, NULL, &target, 1, NULL, viewport, scissor,
                &reason)) {
            free(texel_vertex);
            free(texel_pixel);
            return 128;
        }
        beer_vulkan_graphics_get_stats(&texel_stats_after);
        free(texel_vertex);
        free(texel_pixel);
        if (target_bytes[center] < 180 || target_bytes[center] > 200 ||
            target_bytes[center + 1] < 85 || target_bytes[center + 1] > 105 ||
            target_bytes[center + 2] < 40 || target_bytes[center + 2] > 55)
            return 38;
        if (texel_stats_after.buffer_view_creations !=
                texel_stats_before.buffer_view_creations + 1 ||
            texel_stats_after.buffer_view_cache_hits <
                texel_stats_before.buffer_view_cache_hits + 1) {
            fprintf(stderr,
                    "buffer-view cache mismatch creations=%llu->%llu "
                    "hits=%llu->%llu\n",
                    (unsigned long long)texel_stats_before.buffer_view_creations,
                    (unsigned long long)texel_stats_after.buffer_view_creations,
                    (unsigned long long)texel_stats_before.buffer_view_cache_hits,
                    (unsigned long long)texel_stats_after.buffer_view_cache_hits);
            return 129;
        }
    }
    if (argc >= 12) {
        size_t mrt_pixel_size = 0;
        uint8_t *mrt_pixel = read_file(argv[11], &mrt_pixel_size);
        uint8_t second_bytes[WIDTH * HEIGHT * 4];
        memset(target_bytes, 0, sizeof(target_bytes));
        memset(second_bytes, 0, sizeof(second_bytes));
        uint64_t second_serial = 9;
        BeerVulkanGraphicsTarget targets[2] = {
            target,
            {
                .data = second_bytes,
                .size = sizeof(second_bytes),
                .serial = &second_serial,
                .format = 37,
                .bytes_per_pixel = 4,
                .width = WIDTH,
                .height = HEIGHT,
                .row_pitch = WIDTH * 4
            }
        };
        const uint64_t first_serial = serial;
        if (!mrt_pixel || !beer_vulkan_graphics_draw_state(
                vertex, (const uint32_t *)vertex, vertex_size,
                mrt_pixel, (const uint32_t *)mrt_pixel, mrt_pixel_size,
                NULL, 0, NULL, NULL, targets, 2, NULL, viewport, scissor,
                &reason)) {
            fprintf(stderr, "MRT graphics execution failed: %s\n",
                    reason ? reason : "unknown");
            free(mrt_pixel);
            return 35;
        }
        free(mrt_pixel);
        if (serial != first_serial + 1 || second_serial != 10 ||
            target_bytes[center] < 25 || target_bytes[center] > 40 ||
            target_bytes[center + 1] < 120 ||
            target_bytes[center + 1] > 136 ||
            target_bytes[center + 2] < 215 ||
            target_bytes[center + 2] > 235 ||
            second_bytes[center] < 180 || second_bytes[center] > 200 ||
            second_bytes[center + 1] < 55 ||
            second_bytes[center + 1] > 75 ||
            second_bytes[center + 2] < 10 ||
            second_bytes[center + 2] > 25)
            return 36;
    }
    free(vertex);
    free(pixel);
    beer_vulkan_graphics_destroy();
    puts("Vulkan fullscreen graphics test passed");
    return 0;
}
