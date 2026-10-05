#ifndef BEER_VULKAN_INDEXED_RENDERER_H
#define BEER_VULKAN_INDEXED_RENDERER_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *vertices;
    size_t vertex_bytes;
    const uint8_t *indices;
    size_t index_bytes;
    const uint8_t *constants;
    size_t constant_bytes;
    uint8_t *target;
    size_t target_bytes;
    const void *target_resource;
    uint64_t target_input_serial;
    uint64_t target_output_serial;
    uint32_t width;
    uint32_t height;
    uint32_t vertex_offset;
    uint32_t vertex_stride;
    uint32_t index_offset;
    uint32_t index_count;
    int32_t base_vertex;
    uint32_t mode;
    float viewport[4];
    int32_t scissor[4];
    uint32_t blend_enable;
    uint32_t source_blend;
    uint32_t destination_blend;
    uint32_t color_operation;
    uint32_t source_alpha;
    uint32_t destination_alpha;
    uint32_t alpha_operation;
    uint32_t write_mask;
    float blend_factor[4];
    float constant_color[4];
    const uint8_t *texture;
    const void *texture_resource;
    size_t texture_bytes;
    uint64_t texture_serial;
    uint32_t texture_width;
    uint32_t texture_height;
    uint32_t texture_linear;
    uint32_t texture_address_u;
    uint32_t texture_address_v;
    const uint8_t *texture2;
    const void *texture2_resource;
    size_t texture2_bytes;
    uint64_t texture2_serial;
    uint32_t texture2_width;
    uint32_t texture2_height;
} BeerVulkanIndexedDraw;

int vulkan_indexed_renderer_draw(const BeerVulkanIndexedDraw *draw);
/* Clears a complete RGBA/BGRA render target through Vulkan and retains the
 * result in the shared target mirror for zero-copy presentation. */
int vulkan_indexed_renderer_clear_target(const void *resource,
                                         uint64_t input_serial,
                                         uint64_t output_serial,
                                         uint8_t *pixels, size_t bytes,
                                         uint32_t width, uint32_t height,
                                         uint32_t format,
                                         const float color[4]);
int vulkan_indexed_renderer_sync_target(const void *resource, uint64_t serial,
                                        uint8_t *pixels, size_t bytes);
/* Copies a completed GPU target into another Vulkan stage's mapped upload
 * memory without making the guest's CPU resource authoritative. */
int vulkan_indexed_renderer_copy_target(const void *resource, uint64_t serial,
                                        uint8_t *destination, size_t bytes);
/* Returns the shared-device Vulkan buffer backing a completed target mirror.
 * The handle remains owned by the indexed renderer and is valid until the
 * resource is forgotten or its cache slot is reused. */
int vulkan_indexed_renderer_get_target_buffer(const void *resource,
                                              uint64_t serial,
                                              size_t bytes,
                                              uint64_t *buffer_handle);
/* Consumes the indexed UI draw signature accumulated since the previous
 * Present. Enabled only when BEER_UI_STATE_TRACE is set. mode_counts must
 * provide 17 entries (mode zero is unused). */
int vulkan_indexed_renderer_consume_frame_trace(
    uint64_t *draw_signature, uint64_t *resource_signature,
    uint32_t *draw_count, uint32_t mode_counts[17],
    uint64_t mode_draw_signatures[17],
    uint64_t mode_resource_signatures[17]);
void vulkan_indexed_renderer_forget_resource(const void *resource);
void vulkan_indexed_renderer_destroy(void);

#endif
