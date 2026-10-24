#ifndef BEER_VULKAN_GRAPHICS_H
#define BEER_VULKAN_GRAPHICS_H

#include <stddef.h>
#include <stdint.h>

#define BEER_VULKAN_GRAPHICS_MAX_BINDINGS 64
#define BEER_VULKAN_GRAPHICS_MAX_TARGETS 8

enum {
    BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_IMAGE = 1,
    BEER_VULKAN_GRAPHICS_BINDING_SAMPLER = 2,
    BEER_VULKAN_GRAPHICS_BINDING_UNIFORM_BUFFER = 3,
    BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_DEPTH = 4,
    BEER_VULKAN_GRAPHICS_BINDING_STORAGE_BUFFER = 5,
    BEER_VULKAN_GRAPHICS_BINDING_UNIFORM_TEXEL_BUFFER = 6
};

enum {
    BEER_VULKAN_GRAPHICS_STAGE_VERTEX = 1,
    BEER_VULKAN_GRAPHICS_STAGE_FRAGMENT = 2
};

typedef struct BeerVulkanGraphicsBinding {
    uint32_t binding;
    uint32_t kind;
    uint32_t stage_flags;
    /* Stable D3D12 resource identity and its live content serial. Sampled
     * images use this pair to preserve an unchanged Vulkan mirror across
     * synchronous draws. NULL identity disables mirror reuse. Serial is stored
     * as a POINTER to the resource's content_serial to ensure we always read
     * the current/live value at execution time, not a stale snapshot. */
    const void *resource;
    const uint64_t *serial;
    const uint8_t *data;
    size_t size;
    uint32_t format;
    uint32_t bytes_per_pixel;
    /* Typed/raw D3D buffer SRVs translate to Vulkan uniform texel buffers.
     * `texel_format` is a VkFormat and `texel_bytes` its element width. */
    uint32_t texel_format;
    uint32_t texel_bytes;
    /* Source storage can have a wider texel than the sampled Vulkan aspect.
     * DXGI R32G8X24, for example, stores 8 bytes while its depth SRV exposes
     * the first 4-byte float. Zero means bytes_per_pixel. */
    uint32_t source_bytes_per_pixel;
    /* Block-compressed images use bytes_per_block instead of bytes_per_pixel.
     * Width/height remain texel dimensions, matching VkImageCreateInfo. */
    uint32_t block_width;
    uint32_t block_height;
    uint32_t bytes_per_block;
    uint32_t width;
    uint32_t height;
    uint32_t row_pitch;
    /* Sampled array views upload one tightly packed image per layer.
     * Texture3D views instead set depth > 1 and keep layer_count at one.
     * image_dimension is 1, 2, or 3 and defaults to 2 when zero. */
    uint32_t layer_count;
    uint32_t depth;
    uint32_t image_dimension;
    /* Cube and cube-array SRVs are backed by 2D array images but require a
     * cube-compatible allocation and cube-shaped image view. */
    uint32_t image_cube;
    /* Multisampled image views retain the D3D resource sample count. */
    uint32_t sample_count;
    /* Consecutive mip levels begin at data. Array resources may retain a
     * layer-major host layout, in which case layer_stride locates each layer's
     * complete resource mip chain. */
    uint32_t mip_levels;
    size_t layer_stride;
    uint32_t aspect;
    uint32_t min_filter;
    uint32_t mag_filter;
    uint32_t address_u;
    uint32_t address_v;
    uint32_t address_w;
    uint32_t mipmap_mode;
    uint32_t anisotropy_enable;
    float max_anisotropy;
    /* Vulkan reduction mode: 0 weighted average, 1 minimum, 2 maximum. */
    uint32_t reduction_mode;
    uint32_t compare_enable;
    uint32_t compare_op;
    uint32_t border_color;
} BeerVulkanGraphicsBinding;

typedef struct BeerVulkanGraphicsTarget {
    /* Stable D3D12 resource identity. When non-NULL, the renderer may retain
     * the GPU image across draws and use serial to determine whether a host
     * upload is required. */
    const void *resource;
    uint8_t *data;
    size_t size;
    uint64_t *serial;
    uint32_t format;
    uint32_t bytes_per_pixel;
    uint32_t width;
    uint32_t height;
    uint32_t row_pitch;
    uint32_t sample_count;
    /* Keep the produced image GPU-authoritative. The resource serial still
     * advances, but host bytes are published at a later explicit boundary. */
    uint32_t defer_publication;
} BeerVulkanGraphicsTarget;

typedef struct BeerVulkanGraphicsDepthTarget {
    const void *resource;
    uint8_t *data;
    size_t size;
    uint64_t *serial;
    uint32_t format;
    uint32_t bytes_per_pixel;
    uint32_t source_bytes_per_pixel;
    /* Original DXGI depth format. Packed D24 and normalized D16 host texels
     * are expanded to the Vulkan D32 attachment during upload and converted
     * back after a depth-writing draw. */
    uint32_t source_format;
    uint32_t width;
    uint32_t height;
    uint32_t row_pitch;
    uint32_t sample_count;
    uint32_t has_stencil;
    uint32_t stencil_byte_offset;
} BeerVulkanGraphicsDepthTarget;

#define BEER_VULKAN_GRAPHICS_MAX_VERTEX_BINDINGS 16
#define BEER_VULKAN_GRAPHICS_MAX_VERTEX_ATTRIBUTES 16

typedef struct BeerVulkanGraphicsVertexBinding {
    const void *resource;
    const uint8_t *data;
    size_t size;
    uint32_t stride;
    uint32_t input_rate;
} BeerVulkanGraphicsVertexBinding;

typedef struct BeerVulkanGraphicsVertexAttribute {
    uint32_t location;
    uint32_t binding;
    uint32_t format;
    uint32_t offset;
} BeerVulkanGraphicsVertexAttribute;

typedef struct BeerVulkanGraphicsDrawArguments {
    uint32_t element_count;
    uint32_t instance_count;
    uint32_t start_element;
    int32_t base_vertex;
    uint32_t start_instance;
} BeerVulkanGraphicsDrawArguments;

typedef struct BeerVulkanGraphicsDrawInfo {
    BeerVulkanGraphicsVertexBinding
        vertex_bindings[BEER_VULKAN_GRAPHICS_MAX_VERTEX_BINDINGS];
    BeerVulkanGraphicsVertexAttribute
        vertex_attributes[BEER_VULKAN_GRAPHICS_MAX_VERTEX_ATTRIBUTES];
    uint32_t vertex_binding_count;
    uint32_t vertex_attribute_count;
    const void *index_resource;
    const uint8_t *index_data;
    size_t index_size;
    uint32_t index_type;
    uint32_t topology;
    uint32_t indexed;
    uint32_t robust_index_access;
    uint32_t element_count;
    uint32_t instance_count;
    uint32_t start_element;
    int32_t base_vertex;
    uint32_t start_instance;
    /* Optional compact command array. Geometry, descriptors, fixed state and
     * attachments are shared; each entry becomes one ordered Vulkan draw in
     * the same render pass and command-buffer submission. NULL retains the
     * scalar fields above. */
    const BeerVulkanGraphicsDrawArguments *batch_arguments;
    uint32_t batch_argument_count;
    /* Diagnostic identity only; excluded from pipeline identity. */
    uint64_t debug_submission;
    uint64_t debug_sequence;
    uint64_t debug_vertex_hash;
    uint64_t debug_pixel_hash;
    uint32_t debug_list_index;
} BeerVulkanGraphicsDrawInfo;

/* Fixed-function state. Values are Vulkan enum values. A NULL state selects
 * opaque writes, no culling and an unflipped viewport. The fields before
 * blend_constants form the pipeline identity; blend constants are dynamic. */
typedef struct BeerVulkanGraphicsState {
    uint32_t blend_enable;
    uint32_t src_color_factor;
    uint32_t dst_color_factor;
    uint32_t color_op;
    uint32_t src_alpha_factor;
    uint32_t dst_alpha_factor;
    uint32_t alpha_op;
    uint32_t write_mask;
    uint32_t primitive_restart_enable;
    uint32_t cull_mode;
    uint32_t front_counter_clockwise;
    uint32_t depth_test_enable;
    uint32_t depth_write_enable;
    uint32_t depth_compare_op;
    uint32_t stencil_test_enable;
    uint32_t stencil_read_mask;
    uint32_t stencil_write_mask;
    uint32_t stencil_reference;
    uint32_t stencil_front_fail;
    uint32_t stencil_front_pass;
    uint32_t stencil_front_depth_fail;
    uint32_t stencil_front_compare;
    uint32_t stencil_back_fail;
    uint32_t stencil_back_pass;
    uint32_t stencil_back_depth_fail;
    uint32_t stencil_back_compare;
    /* Attachment sample count. Values are D3D/Vulkan sample counts (1, 2, 4,
     * 8, 16, 32, or 64); zero is normalized to one by the bridge. */
    uint32_t sample_count;
    /* Nonzero applies a negative-height viewport so D3D clip space (Y up)
     * lands on the same framebuffer rows as in D3D. */
    uint32_t flip_y;
    float blend_constants[4];
} BeerVulkanGraphicsState;

/* Executes a translated graphics draw synchronously. A NULL draw_info selects
 * the original fullscreen triangle contract. Unsupported state is rejected,
 * never approximated. Pipelines are cached by shader, target, vertex and
 * fixed-function state. */
int beer_vulkan_graphics_draw_state(
    const void *vertex_key, const uint32_t *vertex_spirv,
    size_t vertex_spirv_size, const void *pixel_key,
    const uint32_t *pixel_spirv, size_t pixel_spirv_size,
    const BeerVulkanGraphicsBinding *bindings, uint32_t binding_count,
    const BeerVulkanGraphicsDrawInfo *draw_info,
    const BeerVulkanGraphicsState *state,
    BeerVulkanGraphicsTarget *targets, uint32_t target_count,
    BeerVulkanGraphicsDepthTarget *depth, const float viewport[6],
    const int32_t scissor[4], const char **reason);

/* Same as beer_vulkan_graphics_draw_state with a NULL state. */
int beer_vulkan_graphics_draw_fullscreen(
    const void *vertex_key, const uint32_t *vertex_spirv,
    size_t vertex_spirv_size, const void *pixel_key,
    const uint32_t *pixel_spirv, size_t pixel_spirv_size,
    const BeerVulkanGraphicsBinding *bindings, uint32_t binding_count,
    const BeerVulkanGraphicsDrawInfo *draw_info,
    BeerVulkanGraphicsTarget *target, uint32_t target_count,
    BeerVulkanGraphicsDepthTarget *depth, const float viewport[6],
    const int32_t scissor[4], const char **reason);

/* Removes persistent sampled-image mirrors before D3D12 frees a resource. */
void beer_vulkan_graphics_forget_resource(const void *resource);

/* Synchronizes one current graphics image mirror into the D3D12 resource's CPU
 * backing. Returns nonzero only when an exact (resource, serial) mirror was
 * found and published. CPU copy/map/present boundaries use this before reading
 * host bytes; ordinary GPU producer/consumer chains remain GPU-resident. */
int beer_vulkan_graphics_sync_resource(const void *resource, uint64_t serial,
                                       uint8_t *data, size_t size,
                                       uint32_t width, uint32_t height,
                                       uint32_t format,
                                       uint32_t bytes_per_pixel);

/* Invalidate cached images; called after compute batch flush to force re-upload
 * of any sampled images that may have been modified by compute. */
void beer_vulkan_graphics_invalidate_image_cache(void);

typedef struct BeerVulkanGraphicsStats {
    uint64_t framebuffer_cache_hits;
    uint64_t framebuffer_creations;
    uint64_t buffer_view_cache_hits;
    uint64_t buffer_view_creations;
    uint64_t queue_submissions;
    uint64_t fence_waits;
    uint64_t draw_commands;
    uint64_t host_execution_ns;
} BeerVulkanGraphicsStats;

/* Returns cumulative, process-local renderer counters for regression and
 * aggregate performance diagnostics. This performs no GPU synchronization. */
void beer_vulkan_graphics_get_stats(BeerVulkanGraphicsStats *stats);

void beer_vulkan_graphics_destroy(void);

#endif
