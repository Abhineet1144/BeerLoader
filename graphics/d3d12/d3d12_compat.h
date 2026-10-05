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

#define BEER_D3D12_MAX_ROOT_PARAMETERS 64
#define BEER_D3D12_MAX_ROOT_CONSTANTS 64

typedef struct BeerD3D12ComputeBinding {
    uint64_t descriptor_table;
    uint64_t buffer_location;
    uint8_t *resource_storage;
    size_t resource_storage_size;
    const void *resource;
    uint32_t constants[BEER_D3D12_MAX_ROOT_CONSTANTS];
    uint32_t constant_count;
    uint32_t kind;
} BeerD3D12ComputeBinding;

typedef struct BeerD3D12SubmissionStats {
    uint32_t submitted_list_count;
    uint32_t valid_list_count;
    uint32_t open_list_count;
    uint64_t copy_buffer_count;
    uint64_t copy_texture_count;
    uint64_t copy_resource_count;
    uint64_t barrier_count;
    uint64_t clear_render_target_count;
    uint64_t dispatch_count;
    uint64_t draw_instanced_count;
    uint64_t draw_indexed_instanced_count;
    uint64_t render_target_binding_count;
    uint32_t executed_dispatch_count;
    uint32_t rejected_dispatch_count;
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
void beer_d3d12_set_compute_executor(BeerD3D12ExecuteComputeFn executor);

#endif
