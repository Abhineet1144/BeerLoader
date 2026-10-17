#ifndef BEER_VULKAN_COMPUTE_H
#define BEER_VULKAN_COMPUTE_H

#include <stddef.h>
#include <stdint.h>

#define BEER_VULKAN_COMPUTE_MAX_BINDINGS 64

enum {
    BEER_VULKAN_COMPUTE_BINDING_BUFFER = 0,
    BEER_VULKAN_COMPUTE_BINDING_IMAGE = 1,
    BEER_VULKAN_COMPUTE_BINDING_SAMPLER = 2
};

/* One host-memory resource bound to a descriptor binding in set zero.
 *
 * Buffers (`kind` == BUFFER): `uniform` selects a uniform buffer (constant
 * buffer); otherwise the range is bound as a storage buffer. A nonzero
 * `texel_format` (VkFormat) with `texel_bytes` per element lets the same range
 * back a typed texel-buffer view.
 *
 * Images (`kind` == IMAGE): `data`/`size` is the host storage of one 2D
 * subresource laid out in rows of `image_row_pitch` bytes; `image_format` is a
 * VkFormat whose texel is `image_bytes` wide. Block-compressed images instead
 * provide `image_block_width`, `image_block_height`, and `image_block_bytes`.
 * The executor uploads `image_mip_levels` consecutive tightly packed levels
 * (one when zero) and, for `writable` bindings, copies them back.
 *
 * Samplers (`kind` == SAMPLER): filters are 0 nearest / 1 linear and addresses
 * are VkSamplerAddressMode values.
 *
 * Ranges with identical `data`/`size` share one device allocation, so a
 * resource bound as both SRV and UAV stays coherent. Ranges flagged
 * `writable` are copied back to host memory after execution. */
typedef struct BeerVulkanComputeBinding {
    uint32_t binding;
    uint32_t uniform;
    uint8_t *data;
    size_t size;
    uint32_t writable;
    /* Stable D3D12 resource identity and its current content serial. Read-only
     * images use these to retain a Vulkan mirror and skip unchanged uploads.
     * NULL identity keeps the conservative upload-every-dispatch behavior. */
    const void *resource;
    uint64_t serial;
    /* D3D12 heap type (1 default, 2 upload, 3 readback). Persistent Vulkan
     * mirrors are restricted to default-heap buffers whose serial is tracked. */
    uint32_t resource_heap_type;
    /* Optional live D3D12 content serial. Writable image execution increments
     * it after synchronized host writeback, allowing the retained Vulkan image
     * to become the authoritative mirror for the next producer/consumer step. */
    uint64_t *serial_location;
    uint32_t texel_format;
    uint32_t texel_bytes;
    uint32_t kind;
    uint32_t image_format;
    uint32_t image_bytes;
    /* Packed/block-compressed images use block dimensions and bytes instead
     * of image_bytes. Source storage remains in native compressed rows. */
    uint32_t image_block_width;
    uint32_t image_block_height;
    uint32_t image_block_bytes;
    uint32_t image_width;
    uint32_t image_height;
    uint32_t image_row_pitch;
    /* Image shape follows Vulkan: dimension is 1, 2, or 3. Array images use
     * image_layers > 1; 3D images use image_depth > 1 and one layer. */
    uint32_t image_dimension;
    uint32_t image_arrayed;
    uint32_t image_cube;
    uint32_t image_layers;
    uint32_t image_depth;
    /* Number of consecutive mip levels beginning at image_width/height/depth.
     * Host data stores each tightly packed mip consecutively. */
    uint32_t image_mip_levels;
    /* Nonzero for D3D array storage, where each layer owns a complete mip
     * chain. This is the byte distance between the selected mip in adjacent
     * layers; zero means levels are packed consecutively. */
    size_t image_layer_stride;
    uint32_t min_filter;
    uint32_t mag_filter;
    uint32_t address_u;
    uint32_t address_v;
    uint32_t address_w;
    /* D3D reduction mode mapped to Vulkan: 0 weighted average, 1 minimum,
     * 2 maximum. Comparison sampling is orthogonal and uses compare_enable. */
    uint32_t reduction_mode;
    uint32_t compare_enable;
    uint32_t compare_op;
    uint32_t mipmap_mode;
    uint32_t anisotropy_enable;
    float max_anisotropy;
    /* Diagnostic provenance retained by the D3D12 bridge. These fields do not
     * affect descriptor selection or execution; they identify the root/table
     * entry that supplied a binding when a first-failure capture is written. */
    uint32_t source_root_parameter;
    uint32_t source_descriptor_index;
    uint32_t source_descriptor_type;
    uint32_t source_register;
    uint32_t source_register_space;
    uint64_t source_descriptor_table;
    uint64_t source_descriptor_handle;
    uint64_t source_buffer_location;
    uint64_t source_resource_gpu_base;
    size_t source_resource_total_size;
    uint32_t source_resource_heap_type;
    uint8_t source_descriptor_bytes[32];
    uint32_t source_descriptor_size;
} BeerVulkanComputeBinding;

/* Executes a translated compute shader synchronously on the shared Vulkan
 * queue. `shader_key` must remain stable for the lifetime of `spirv` (the
 * translator cache pointer); pipelines are cached by it.
 *
 * Every descriptor the module declares in set zero must be a uniform/storage
 * buffer, a typed texel buffer, a 2D sampled or storage image, or a sampler,
 * and the caller must provide every one of them. Anything else is rejected
 * rather than approximated.
 *
 * Returns nonzero when the dispatch ran and writable ranges were copied back.
 * `*reason` receives a static string describing a rejection. */
enum {
    BEER_VULKAN_COMPUTE_HAZARD_RAW = 1u,
    BEER_VULKAN_COMPUTE_HAZARD_WAR = 2u,
    BEER_VULKAN_COMPUTE_HAZARD_WAW = 4u
};

int beer_vulkan_compute_dispatch_with_hazards(
                                 const void *shader_key,
                                 const uint32_t *spirv, size_t spirv_size,
                                 const BeerVulkanComputeBinding *bindings,
                                 uint32_t binding_count,
                                 uint32_t group_count_x,
                                 uint32_t group_count_y,
                                 uint32_t group_count_z,
                                 uint32_t hazards_before,
                                 const char **reason);

/* Compatibility entry point for scalar callers that have no preceding IR
 * operation and therefore no cross-dispatch hazards to declare. */
int beer_vulkan_compute_dispatch(const void *shader_key,
                                 const uint32_t *spirv, size_t spirv_size,
                                 const BeerVulkanComputeBinding *bindings,
                                 uint32_t binding_count,
                                 uint32_t group_count_x,
                                 uint32_t group_count_y,
                                 uint32_t group_count_z,
                                 const char **reason);

/* One fully resolved operation in a dependency-isolated compute epoch.
 * Resource payload pointers remain live for the duration of the call. Results
 * and rejection reasons are written directly to backend-owned storage, keeping
 * the D3D12 frontend independent of the renderer's recording strategy. */
typedef struct BeerVulkanComputeOperation {
    const void *shader_key;
    const uint32_t *spirv;
    size_t spirv_size;
    const BeerVulkanComputeBinding *bindings;
    uint32_t binding_count;
    uint32_t group_count_x;
    uint32_t group_count_y;
    uint32_t group_count_z;
    uint32_t hazards_before;
    uint8_t *result;
    const char **reason;
    /* Optional diagnostic provenance. Execution must never branch on these
     * identities; they only preserve the causal D3D12 operation when Vulkan
     * reports device loss from the renderer-owned epoch path. */
    uint64_t debug_submission;
    uint64_t debug_sequence;
    uint64_t shader_hash;
    uint32_t debug_list_index;
    const uint8_t *dxbc;
    size_t dxbc_size;
    const uint8_t *root_signature;
    size_t root_signature_size;
} BeerVulkanComputeOperation;

/* Executes one dependency epoch. Immutable shader/pipeline state is prepared
 * for the complete epoch before the proven scalar dispatcher executes ready
 * entries. Resource payloads remain live and are inspected only in operation
 * order. This API is the stable replacement point for shared descriptor/image
 * preparation and one-command-buffer recording. Returns nonzero when every
 * supplied operation executed. */
int beer_vulkan_compute_execute_epoch(
    const BeerVulkanComputeOperation *operations, uint32_t operation_count);

/* Brackets consecutive D3D12 compute dispatches. Descriptor-pool storage and
 * descriptor sets remain valid for the complete batch, which is required when
 * several operations are later recorded into one command buffer. Writable
 * image mirrors may remain GPU-authoritative inside the batch; end flushes
 * them before a host or graphics consumer. Buffer UAVs remain synchronous. */
/* Reserves one stable host-visible staging arena before the outer batch starts.
 * The arena may not grow while descriptor sets from that batch are alive.
 * `staging_bytes` is a conservative upper bound over all operations. */
int beer_vulkan_compute_begin_batch_with_staging(size_t staging_bytes);
void beer_vulkan_compute_begin_batch(void);
void beer_vulkan_compute_end_batch(void);

/* A borrowed compute image mirror lets graphics sample a compute-produced
 * resource directly on the shared Vulkan device. The token pins the cache
 * entry against eviction until release. This is an internal backend bridge;
 * host storage and D3D12 serials remain the public synchronization contract. */
typedef struct BeerVulkanComputeImageMirror {
    uint64_t image;
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t layers;
    uint32_t dimension;
    uint32_t mip_levels;
    uint32_t layout;
    void *token;
} BeerVulkanComputeImageMirror;
int beer_vulkan_compute_has_image_mirror(
    const void *resource, uint64_t serial, uint32_t format, uint32_t width,
    uint32_t height, uint32_t depth, uint32_t layers, uint32_t dimension,
    uint32_t mip_levels);
int beer_vulkan_compute_acquire_image_mirror(
    const void *resource, uint64_t serial, uint32_t format, uint32_t width,
    uint32_t height, uint32_t depth, uint32_t layers, uint32_t dimension,
    uint32_t mip_levels, BeerVulkanComputeImageMirror *mirror);
void beer_vulkan_compute_release_image_mirror(
    BeerVulkanComputeImageMirror *mirror);

/* Publishes one current GPU-authoritative image into its CPU shadow without
 * discarding the reusable Vulkan mirror. True host/present consumers call this
 * boundary; ordinary compute/graphics chains remain GPU-resident. */
int beer_vulkan_compute_sync_resource(const void *resource, uint64_t serial);

/* Flushes and removes persistent mirrors for a resource before its host
 * storage is released. */
void beer_vulkan_compute_forget_resource(const void *resource);

/* Monotonic renderer counters used by diagnostics and regression tests. They
 * expose scheduling boundaries without changing dispatch behavior. */
typedef struct BeerVulkanComputeStats {
    uint64_t queue_submissions;
    uint64_t completed_submissions;
    uint64_t host_fence_waits;
    uint64_t host_execution_ns;
    uint64_t sampler_cache_hits;
    uint64_t sampler_creations;
    uint64_t descriptor_set_cache_hits;
    uint64_t descriptor_set_allocations;
    uint64_t image_cache_hits;
    uint64_t image_cache_evictions;
    uint64_t epoch_calls;
    uint64_t epoch_operations;
    uint64_t multi_operation_epochs;
    uint64_t shareable_epoch_calls;
    uint64_t shareable_epoch_operations;
    /* Epochs that actually used one shared command buffer/submission. This is
     * intentionally distinct from eligibility: resource-bearing epochs may be
     * safe in principle but still require the scalar staging fallback. */
    uint64_t shared_epoch_calls;
    uint64_t shared_epoch_operations;
    uint64_t watchdog_epoch_fallbacks;
    uint32_t max_epoch_operations;
} BeerVulkanComputeStats;
void beer_vulkan_compute_get_stats(BeerVulkanComputeStats *stats);

void beer_vulkan_compute_destroy(void);

#endif
