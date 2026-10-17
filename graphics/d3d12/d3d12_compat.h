#ifndef BEER_D3D12_COMPAT_H
#define BEER_D3D12_COMPAT_H

#include <stddef.h>
#include <stdint.h>

/* Incremental D3D12 compatibility boundary. Beer exposes typed COM objects
 * only for contracts validated against focused tests and observed guests. */
uint64_t __attribute__((ms_abi))
beer_d3d12_create_device(uint64_t adapter, uint32_t minimum_feature_level,
                         const uint8_t *riid, void **device);

uint64_t __attribute__((ms_abi))
beer_d3d12_get_debug_interface(const uint8_t *riid, void **debug);

uint64_t __attribute__((ms_abi))
beer_d3d12_serialize_versioned_root_signature(const void *description,
                                              void **blob,
                                              void **error_blob);

typedef void (*BeerD3D12SignalEventFn)(uint64_t event_handle);
void beer_d3d12_set_event_signaler(BeerD3D12SignalEventFn signaler);

typedef int (*BeerD3D12PresentFn)(const void *resource, uint64_t serial,
                                  const uint8_t *pixels, int width, int height,
                                  int row_pitch);
void beer_d3d12_set_presenter(BeerD3D12PresentFn presenter);

typedef int (*BeerD3D12ClearTargetFn)(const void *resource,
                                      uint64_t input_serial,
                                      uint64_t output_serial,
                                      uint8_t *pixels, uint64_t bytes,
                                      uint32_t width, uint32_t height,
                                      uint32_t format,
                                      const float color[4]);
void beer_d3d12_set_clear_target(BeerD3D12ClearTargetFn clear_target);

typedef void (*BeerD3D12ForgetResourceFn)(const void *resource);
void beer_d3d12_set_resource_forgetter(BeerD3D12ForgetResourceFn forgetter);
/* Optional narrower hook for resources only read by the CPU (copy sources).
 * Their serial is unchanged, so serial-validated mirrors remain correct.
 * Falls back to the resource forgetter when unset. */
void beer_d3d12_set_resource_source_forgetter(
    BeerD3D12ForgetResourceFn forgetter);

/* Host bytes retained by immutable command-list snapshots. Exposed for memory
 * regression tests and bounded diagnostics. */
size_t beer_d3d12_snapshot_bytes(void);
size_t beer_d3d12_snapshot_peak_bytes(void);
/* CPU backing owned by live committed resources and heaps. These counters
 * exclude placed resources because they alias their owning heap. */
size_t beer_d3d12_backing_bytes(void);
size_t beer_d3d12_backing_peak_bytes(void);

#define BEER_D3D12_MAX_ROOT_PARAMETERS 64
#define BEER_D3D12_MAX_ROOT_CONSTANTS 64

/* Keep root-table snapshots capable of representing every descriptor accepted
 * by the shader translator. A lower limit silently truncated large material
 * tables (for example, 34-entry pixel-shader tables) and made valid scene SRVs
 * impossible to resolve at draw replay. */
#define BEER_D3D12_MAX_RESOLVED_DESCRIPTORS 64

typedef struct BeerD3D12ComputeBinding {
    uint64_t descriptor_table;
    uint64_t buffer_location;
    uint8_t *resource_storage;
    size_t resource_storage_size;
    const void *resource;
    const uint64_t *resource_serial_location;
    uint32_t resource_heap_type;
    const void *resolved_root_resource;
    uint32_t constants[BEER_D3D12_MAX_ROOT_CONSTANTS];
    uint32_t constant_count;
    uint32_t kind;
    /* D3D12_SHADER_VISIBILITY retained from the owning root parameter. */
    uint32_t shader_visibility;
    uint32_t descriptor_count;
    const void *descriptor_resources[BEER_D3D12_MAX_RESOLVED_DESCRIPTORS];
    /* Retained resources keep these locations alive through deferred queue
     * execution. Backends use the live serial to avoid re-uploading unchanged
     * content while still observing earlier commands in the same submission. */
    const uint64_t *descriptor_resource_serial_locations[
        BEER_D3D12_MAX_RESOLVED_DESCRIPTORS];
    uint8_t *descriptor_resource_storage[BEER_D3D12_MAX_RESOLVED_DESCRIPTORS];
    size_t descriptor_resource_storage_size[BEER_D3D12_MAX_RESOLVED_DESCRIPTORS];
    uint32_t descriptor_types[BEER_D3D12_MAX_RESOLVED_DESCRIPTORS];
    uint32_t descriptor_registers[BEER_D3D12_MAX_RESOLVED_DESCRIPTORS];
    uint32_t descriptor_spaces[BEER_D3D12_MAX_RESOLVED_DESCRIPTORS];
    /* Exact descriptor provenance. Handles identify the selected heap slot;
     * buffer locations retain a CBV's D3D12 GPU virtual address. */
    uint64_t descriptor_handles[BEER_D3D12_MAX_RESOLVED_DESCRIPTORS];
    uint64_t descriptor_buffer_locations[BEER_D3D12_MAX_RESOLVED_DESCRIPTORS];
    uint64_t descriptor_resource_gpu_bases[BEER_D3D12_MAX_RESOLVED_DESCRIPTORS];
    size_t descriptor_resource_total_sizes[BEER_D3D12_MAX_RESOLVED_DESCRIPTORS];
    uint32_t descriptor_resource_heap_types[BEER_D3D12_MAX_RESOLVED_DESCRIPTORS];
    uint8_t descriptor_raw_bytes[BEER_D3D12_MAX_RESOLVED_DESCRIPTORS][32];
    uint32_t descriptor_raw_sizes[BEER_D3D12_MAX_RESOLVED_DESCRIPTORS];
    uint32_t descriptor_widths[BEER_D3D12_MAX_RESOLVED_DESCRIPTORS];
    uint32_t descriptor_heights[BEER_D3D12_MAX_RESOLVED_DESCRIPTORS];
    uint32_t descriptor_formats[BEER_D3D12_MAX_RESOLVED_DESCRIPTORS];
    uint32_t descriptor_subresources[BEER_D3D12_MAX_RESOLVED_DESCRIPTORS];
    size_t descriptor_subresource_offsets[BEER_D3D12_MAX_RESOLVED_DESCRIPTORS];
    size_t descriptor_row_pitches[BEER_D3D12_MAX_RESOLVED_DESCRIPTORS];
    /* DXGI format and buffer FirstElement recorded in the SRV/UAV desc. */
    uint32_t descriptor_view_formats[BEER_D3D12_MAX_RESOLVED_DESCRIPTORS];
    uint64_t descriptor_view_first_elements[BEER_D3D12_MAX_RESOLVED_DESCRIPTORS];
    uint32_t descriptor_view_element_counts[BEER_D3D12_MAX_RESOLVED_DESCRIPTORS];
    uint32_t descriptor_view_structure_strides[BEER_D3D12_MAX_RESOLVED_DESCRIPTORS];
    /* Texture view and resource shape used to decide whether a descriptor can
     * be mirrored as a single-mip 2D device image. */
    uint32_t descriptor_view_dimensions[BEER_D3D12_MAX_RESOLVED_DESCRIPTORS];
    uint32_t descriptor_view_mip_levels[BEER_D3D12_MAX_RESOLVED_DESCRIPTORS];
    uint32_t descriptor_view_first_layers[BEER_D3D12_MAX_RESOLVED_DESCRIPTORS];
    uint32_t descriptor_view_layer_counts[BEER_D3D12_MAX_RESOLVED_DESCRIPTORS];
    uint32_t descriptor_resource_dimensions[BEER_D3D12_MAX_RESOLVED_DESCRIPTORS];
    uint32_t descriptor_resource_mips[BEER_D3D12_MAX_RESOLVED_DESCRIPTORS];
    uint32_t descriptor_resource_layers[BEER_D3D12_MAX_RESOLVED_DESCRIPTORS];
    uint32_t descriptor_depths[BEER_D3D12_MAX_RESOLVED_DESCRIPTORS];
    uint32_t descriptor_resource_samples[BEER_D3D12_MAX_RESOLVED_DESCRIPTORS];
    /* Compact D3D12_SAMPLER_DESC: Filter, AddressU/V/W, float LOD bias,
     * MaxAnisotropy, ComparisonFunc, and classified border color. */
    uint32_t descriptor_samplers[BEER_D3D12_MAX_RESOLVED_DESCRIPTORS][8];
} BeerD3D12ComputeBinding;

typedef struct BeerD3D12SubmissionStats {
    uint32_t submitted_list_count;
    uint32_t valid_list_count;
    uint32_t open_list_count;
    uint64_t copy_buffer_count;
    uint64_t copy_texture_count;
    uint64_t copy_resource_count;
    uint64_t barrier_count;
    uint64_t clear_depth_stencil_count;
    uint64_t clear_render_target_count;
    uint64_t clear_unordered_access_uint_count;
    uint64_t discard_resource_count;
    uint64_t dispatch_count;
    uint64_t draw_instanced_count;
    uint64_t draw_indexed_instanced_count;
    uint64_t render_target_binding_count;
    uint32_t executed_dispatch_count;
    uint32_t rejected_dispatch_count;
    uint32_t executed_draw_count;
    uint32_t rejected_draw_count;
} BeerD3D12SubmissionStats;

typedef void (*BeerD3D12SubmissionObserverFn)(
    const BeerD3D12SubmissionStats *stats);
void beer_d3d12_set_submission_observer(
    BeerD3D12SubmissionObserverFn observer);

typedef struct BeerD3D12ComputeDispatch {
    const void *pipeline_state;
    const void *root_signature;
    const uint8_t *root_signature_blob;
    size_t root_signature_blob_size;
    const uint8_t *shader;
    size_t shader_size;
    uint64_t shader_hash;
    uint32_t group_count_x;
    uint32_t group_count_y;
    uint32_t group_count_z;
    /* Diagnostic identity assigned when the recorded command is submitted. */
    uint64_t debug_submission;
    uint64_t debug_sequence;
    uint32_t debug_list_index;
    uint32_t descriptor_heap_count;
    const void *descriptor_heaps[2];
    uint64_t descriptor_heap_gpu_start[2];
    const uint8_t *descriptor_heap_storage[2];
    size_t descriptor_heap_size[2];
    uint32_t descriptor_heap_type[2];
    uint32_t descriptor_heap_increment[2];
    BeerD3D12ComputeBinding bindings[BEER_D3D12_MAX_ROOT_PARAMETERS];
} BeerD3D12ComputeDispatch;

/* The compatibility layer snapshots all compute state at Dispatch time and
 * invokes this backend while ExecuteCommandLists is processed. Returning zero
 * explicitly rejects a workload; returning nonzero marks it executed. */
typedef int (*BeerD3D12ExecuteComputeFn)(
    const BeerD3D12ComputeDispatch *dispatch);
enum {
    BEER_D3D12_COMPUTE_ACCESS_READ = 1u,
    BEER_D3D12_COMPUTE_ACCESS_WRITE = 2u
};

enum {
    BEER_D3D12_COMPUTE_HAZARD_RAW = 1u,
    BEER_D3D12_COMPUTE_HAZARD_WAR = 2u,
    BEER_D3D12_COMPUTE_HAZARD_WAW = 4u
};

typedef struct BeerD3D12ComputeResourceAccess {
    const void *resource;
    uint8_t *storage;
    size_t storage_size;
    uint64_t *serial_location;
    uint32_t access;
} BeerD3D12ComputeResourceAccess;

typedef struct BeerD3D12ComputeCommand {
    const BeerD3D12ComputeDispatch *dispatch;
    uint32_t first_access;
    uint32_t access_count;
    /* Hazards against earlier operations in this batch. A Vulkan compiler
     * uses these flags to emit barriers without rediscovering D3D bindings. */
    uint32_t hazards_before;
} BeerD3D12ComputeCommand;

typedef struct BeerD3D12ComputeCommandBatch {
    const BeerD3D12ComputeCommand *commands;
    uint32_t command_count;
    const BeerD3D12ComputeResourceAccess *accesses;
    uint32_t access_count;
} BeerD3D12ComputeCommandBatch;

/* Shared hazard classifier used by the recorder and focused IR tests. */
uint32_t beer_d3d12_compute_hazards_before(
    const BeerD3D12ComputeResourceAccess *accesses, uint32_t first,
    uint32_t count);

/* Builds the stable host-range layout used by a deferred compute batch.
 * Exact duplicate ranges share one aligned slice; distinct views remain
 * separate even when they refer to the same D3D12 resource. Returns zero on
 * invalid input or overflow. */
int beer_d3d12_compute_plan_staging(
    const BeerD3D12ComputeResourceAccess *accesses, uint32_t count,
    size_t alignment, size_t *offsets, size_t *total_size);

/* Partitions an ordered compute run into host-preparation epochs. Commands
 * without dependencies may be prepared together. Any RAW/WAR/WAW dependency
 * starts a new epoch so host-backed inputs are never snapshotted before an
 * earlier GPU writer has reached a synchronization boundary. `epochs` receives
 * one monotonically increasing epoch index per command. */
int beer_d3d12_compute_plan_epochs(
    const BeerD3D12ComputeCommand *commands, uint32_t count,
    uint32_t *epochs, uint32_t *epoch_count);

/* Produces the complete staging layout for dependency epochs. Access offsets
 * are relative to their epoch arena; entries without host storage receive
 * SIZE_MAX. `per_command_bytes` reserves backend-private space (for example,
 * snapshotted root constants) without exposing it as a resource access. */
int beer_d3d12_compute_plan_epoch_staging(
    const BeerD3D12ComputeCommandBatch *batch, const uint32_t *epochs,
    uint32_t epoch_count, size_t alignment, size_t per_command_bytes,
    size_t *access_offsets, size_t *epoch_sizes);

/* Ordered compute-command IR handed to a backend at a real command boundary.
 * Dispatch and resource pointers remain valid for the duration of the call.
 * The backend writes one nonzero result per executed operation. */
typedef void (*BeerD3D12ExecuteComputeBatchFn)(
    const BeerD3D12ComputeCommandBatch *batch, uint8_t *results);
typedef void (*BeerD3D12ComputeBatchFn)(void);
void beer_d3d12_set_compute_executor(BeerD3D12ExecuteComputeFn executor);
void beer_d3d12_set_compute_batch_executor(
    BeerD3D12ExecuteComputeBatchFn executor);
/* Consecutive dispatches are bracketed as one batch. The end callback is
 * invoked before any draw/clear and at submission end, allowing a backend to
 * flush deferred GPU writes without violating D3D12 command order. */
void beer_d3d12_set_compute_batch_callbacks(
    BeerD3D12ComputeBatchFn begin, BeerD3D12ComputeBatchFn end);

#define BEER_D3D12_MAX_VERTEX_BUFFERS 16
#define BEER_D3D12_MAX_RENDER_TARGETS 8
#define BEER_D3D12_MAX_INPUT_ELEMENTS 16

typedef struct BeerD3D12InputElement {
    uint32_t semantic_index;
    uint32_t format;
    uint32_t input_slot;
    uint32_t aligned_byte_offset;
    uint32_t input_class;
    uint32_t instance_step_rate;
} BeerD3D12InputElement;

typedef struct BeerD3D12BufferView {
    uint64_t address;
    uint32_t size;
    uint32_t stride_or_format;
} BeerD3D12BufferView;

typedef struct BeerD3D12RenderTargetBlend {
    uint32_t blend_enable;
    uint32_t logic_op_enable;
    uint32_t src_blend;
    uint32_t dest_blend;
    uint32_t blend_op;
    uint32_t src_blend_alpha;
    uint32_t dest_blend_alpha;
    uint32_t blend_op_alpha;
    uint32_t logic_op;
    uint32_t write_mask;
} BeerD3D12RenderTargetBlend;

typedef struct BeerD3D12StencilOp {
    uint32_t fail;
    uint32_t depth_fail;
    uint32_t pass;
    uint32_t func;
} BeerD3D12StencilOp;

/* Fixed-function state parsed from legacy and stream PSOs. `valid` is zero
 * when the descriptor could not be decoded completely; consumers must then
 * reject the draw instead of assuming defaults. Raw D3D12 enum values. */
typedef struct BeerD3D12PipelineStateInfo {
    uint32_t valid;
    uint32_t alpha_to_coverage;
    uint32_t independent_blend;
    BeerD3D12RenderTargetBlend render_target[BEER_D3D12_MAX_RENDER_TARGETS];
    uint32_t sample_mask;
    uint32_t index_buffer_strip_cut_value;
    uint32_t fill_mode;
    uint32_t cull_mode;
    uint32_t front_counter_clockwise;
    int32_t depth_bias;
    float depth_bias_clamp;
    float slope_scaled_depth_bias;
    uint32_t depth_clip;
    uint32_t multisample;
    uint32_t antialiased_line;
    uint32_t forced_sample_count;
    uint32_t conservative_raster;
    uint32_t depth_enable;
    uint32_t depth_write_mask;
    uint32_t depth_func;
    uint32_t stencil_enable;
    uint32_t stencil_read_mask;
    uint32_t stencil_write_mask;
    BeerD3D12StencilOp stencil_front;
    BeerD3D12StencilOp stencil_back;
    uint32_t render_target_format_count;
    uint32_t render_target_formats[BEER_D3D12_MAX_RENDER_TARGETS];
    uint32_t depth_stencil_format;
    uint32_t sample_count;
} BeerD3D12PipelineStateInfo;

typedef struct BeerD3D12GraphicsDraw {
    const void *pipeline_state;
    const uint8_t *vertex_shader;
    size_t vertex_shader_size;
    uint64_t vertex_shader_hash;
    const uint8_t *pixel_shader;
    size_t pixel_shader_size;
    uint64_t pixel_shader_hash;
    const void *root_signature;
    const uint8_t *root_signature_blob;
    size_t root_signature_blob_size;
    BeerD3D12ComputeBinding bindings[BEER_D3D12_MAX_ROOT_PARAMETERS];
    uint32_t descriptor_heap_count;
    const void *descriptor_heaps[2];
    uint64_t descriptor_heap_gpu_start[2];
    const uint8_t *descriptor_heap_storage[2];
    size_t descriptor_heap_size[2];
    uint32_t descriptor_heap_type[2];
    uint32_t descriptor_heap_increment[2];
    const void *render_targets[BEER_D3D12_MAX_RENDER_TARGETS];
    uint64_t *render_target_serial_location[BEER_D3D12_MAX_RENDER_TARGETS];
    uint8_t *render_target_storage[BEER_D3D12_MAX_RENDER_TARGETS];
    size_t render_target_storage_size[BEER_D3D12_MAX_RENDER_TARGETS];
    uint64_t render_target_serial[BEER_D3D12_MAX_RENDER_TARGETS];
    uint32_t render_target_width[BEER_D3D12_MAX_RENDER_TARGETS];
    uint32_t render_target_height[BEER_D3D12_MAX_RENDER_TARGETS];
    uint32_t render_target_format[BEER_D3D12_MAX_RENDER_TARGETS];
    uint32_t render_target_count;
    const void *depth_stencil;
    uint64_t *depth_stencil_serial_location;
    uint8_t *depth_stencil_storage;
    size_t depth_stencil_storage_size;
    uint32_t depth_stencil_source_bytes_per_pixel;
    uint32_t depth_stencil_width;
    uint32_t depth_stencil_height;
    uint32_t depth_stencil_format;
    BeerD3D12InputElement input_elements[BEER_D3D12_MAX_INPUT_ELEMENTS];
    uint32_t input_element_count;
    BeerD3D12PipelineStateInfo pso;
    BeerD3D12BufferView vertex_buffers[BEER_D3D12_MAX_VERTEX_BUFFERS];
    const uint8_t *vertex_buffer_storage[BEER_D3D12_MAX_VERTEX_BUFFERS];
    size_t vertex_buffer_storage_size[BEER_D3D12_MAX_VERTEX_BUFFERS];
    const void *vertex_buffer_resources[BEER_D3D12_MAX_VERTEX_BUFFERS];
    uint32_t vertex_buffer_mask;
    BeerD3D12BufferView index_buffer;
    const uint8_t *index_buffer_storage;
    size_t index_buffer_storage_size;
    const void *index_buffer_resource;
    float viewport[6];
    int32_t scissor[4];
    float blend_factor[4];
    uint32_t primitive_topology;
    uint32_t stencil_reference;
    uint32_t indexed;
    /* ExecuteIndirect arguments are GPU-generated. D3D robust access returns
     * zero for index fetches beyond the declared view instead of exposing
     * adjacent host staging data. */
    uint32_t robust_index_access;
    /* GPU-generated draw runs may keep color targets authoritative in Vulkan
     * until their final command. The final command publishes host bytes. */
    uint32_t defer_render_target_publication;
    uint32_t element_count;
    uint32_t instance_count;
    uint32_t start_element;
    int32_t base_vertex;
    uint32_t start_instance;
    /* Diagnostic identity assigned when the recorded command is submitted. */
    uint64_t debug_submission;
    uint64_t debug_sequence;
    uint32_t debug_list_index;
} BeerD3D12GraphicsDraw;

/* Called in submission order for each immutable draw snapshot. Returning
 * nonzero means the backend executed the draw; zero keeps it explicitly
 * rejected while preserving accurate command telemetry. */
typedef int (*BeerD3D12ExecuteGraphicsFn)(
    const BeerD3D12GraphicsDraw *draw);
void beer_d3d12_set_graphics_executor(BeerD3D12ExecuteGraphicsFn executor);

/* Compact arguments for several draws sharing one immutable state snapshot.
 * This avoids duplicating the large descriptor/root snapshot merely to batch
 * an ExecuteIndirect expansion. The backend returns the number executed. */
typedef struct BeerD3D12GraphicsArguments {
    uint32_t element_count;
    uint32_t instance_count;
    uint32_t start_element;
    int32_t base_vertex;
    uint32_t start_instance;
    uint32_t defer_render_target_publication;
} BeerD3D12GraphicsArguments;
typedef uint32_t (*BeerD3D12ExecuteGraphicsBatchFn)(
    const BeerD3D12GraphicsDraw *draw,
    const BeerD3D12GraphicsArguments *arguments, uint32_t argument_count);
void beer_d3d12_set_graphics_batch_executor(
    BeerD3D12ExecuteGraphicsBatchFn executor);

/* Returns nonzero when consecutive draws can retain identical color/depth
 * attachments in GPU-authoritative mirrors without an intervening publication. */
int beer_d3d12_graphics_attachments_match(
    const BeerD3D12GraphicsDraw *current,
    const BeerD3D12GraphicsDraw *next);
/* Diagnostic rollback for the guarded GPU-authoritative publication path. */
int beer_d3d12_graphics_publication_deferral_enabled(void);

#endif
