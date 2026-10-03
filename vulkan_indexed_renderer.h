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
    const uint8_t *texture;
    size_t texture_bytes;
    uint64_t texture_serial;
    uint32_t texture_width;
    uint32_t texture_height;
    uint32_t texture_linear;
    uint32_t texture_address_u;
    uint32_t texture_address_v;
} BeerVulkanIndexedDraw;

int vulkan_indexed_renderer_draw(const BeerVulkanIndexedDraw *draw);
int vulkan_indexed_renderer_sync_target(const void *resource, uint64_t serial,
                                        uint8_t *pixels, size_t bytes);
/* Copies a completed GPU target into another Vulkan stage's mapped upload
 * memory without making the guest's CPU resource authoritative. */
int vulkan_indexed_renderer_copy_target(const void *resource, uint64_t serial,
                                        uint8_t *destination, size_t bytes);
void vulkan_indexed_renderer_forget_resource(const void *resource);
void vulkan_indexed_renderer_destroy(void);

#endif
