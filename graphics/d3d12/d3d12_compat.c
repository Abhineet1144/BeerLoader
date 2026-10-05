#include "d3d12_compat.h"

#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BEER_S_OK          UINT64_C(0)
#define BEER_E_NOINTERFACE UINT64_C(0x80004002)
#define BEER_E_NOTIMPL     UINT64_C(0x80004001)
#define BEER_E_INVALIDARG  UINT64_C(0x80070057)
#define BEER_E_OUTOFMEMORY UINT64_C(0x8007000e)
#define BEER_DXGI_ERROR_NOT_FOUND UINT64_C(0x887a0002)
#define BEER_DXGI_ERROR_MORE_DATA UINT64_C(0x887a0003)

static int d3d12_diagnostics_enabled(void)
{
    static int initialized;
    static int enabled;
    if (!initialized) {
        const char *value = getenv("BEER_D3D12_DIAGNOSTICS");
        enabled = value && value[0] && strcmp(value, "0") != 0;
        initialized = 1;
    }
    return enabled;
}

#define D3D12_TRACE(...) do { \
    if (d3d12_diagnostics_enabled()) fprintf(stderr, __VA_ARGS__); \
} while (0)

/* IID_IUnknown and IID_ID3D12Device. */
static const uint8_t k_iid_iunknown[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46
};
static const uint8_t k_iid_d3d12_device[16] = {
    0xf1, 0x19, 0x98, 0x18, 0xb6, 0x1d, 0x57, 0x4b,
    0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7
};
static const uint8_t k_iid_d3d12_graphics_command_list[16] = {
    0x0f, 0x0d, 0x16, 0x5b, 0x1b, 0xac, 0x85, 0x41,
    0x8b, 0xa8, 0xb3, 0xae, 0x42, 0xa5, 0xa4, 0x55
};
static const uint8_t k_iid_d3d12_fence[16] = {
    0xcf, 0x3d, 0x75, 0x0a, 0xd8, 0xc4, 0x91, 0x4b,
    0xad, 0xf6, 0xbe, 0x5a, 0x60, 0xd9, 0x5a, 0x76
};
static const uint8_t k_iid_d3d12_descriptor_heap[16] = {
    0x1d, 0x47, 0xfb, 0x8e, 0x6c, 0x61, 0x49, 0x4f,
    0x90, 0xf7, 0x12, 0x7b, 0xb7, 0x63, 0xfa, 0x51
};
static const uint8_t k_iid_d3d12_query_heap[16] = {
    0xae, 0x58, 0x96, 0x0d, 0x45, 0xed, 0x9e, 0x46,
    0xa6, 0x1d, 0x97, 0x0e, 0xc5, 0x83, 0xca, 0xb4
};
static const uint8_t k_iid_d3d12_resource[16] = {
    0xbe, 0x42, 0x64, 0x69, 0x2e, 0xa7, 0x59, 0x40,
    0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad
};
static const uint8_t k_iid_d3d12_heap[16] = {
    0x02, 0x25, 0x3b, 0x6b, 0x51, 0x6e, 0xb3, 0x45,
    0x90, 0xee, 0x98, 0x84, 0x26, 0x5e, 0x8d, 0xf3
};
static const uint8_t k_iid_d3d12_command_signature[16] = {
    0x7c, 0x79, 0x6a, 0xc3, 0x80, 0xec, 0x0a, 0x4f,
    0x89, 0x85, 0xa7, 0xb2, 0x47, 0x50, 0x82, 0xd1
};
static const uint8_t k_iid_d3d12_root_signature[16] = {
    0x66, 0x6b, 0x4a, 0xc5, 0xdf, 0x72, 0xe8, 0x4e,
    0x8b, 0xe5, 0xa9, 0x46, 0xa1, 0x42, 0x92, 0x14
};
static const uint8_t k_iid_d3d12_pipeline_state[16] = {
    0xf3, 0x30, 0x5a, 0x76, 0x24, 0xf6, 0x6f, 0x4c,
    0xa8, 0x28, 0xac, 0xe9, 0x48, 0x62, 0x24, 0x45
};
static const uint8_t k_iid_d3d12_device1[16] = {
    0x80, 0xce, 0xac, 0x77, 0x8e, 0x63, 0x65, 0x4e,
    0x88, 0x95, 0xc1, 0xf2, 0x33, 0x86, 0x86, 0x3e
};
static const uint8_t k_iid_d3d12_device2[16] = {
    0x1e, 0xa4, 0xba, 0x30, 0x5b, 0xb1, 0x5c, 0x47,
    0xa0, 0xbb, 0x1a, 0xf5, 0xc5, 0xb6, 0x43, 0x28
};
static const uint8_t k_iid_d3d_blob[16] = {
    0x08, 0xfb, 0xa5, 0x8b, 0x95, 0x51, 0xe2, 0x40,
    0xac, 0x58, 0x0d, 0x98, 0x9c, 0x3a, 0x01, 0x02
};
/* ID3D12DeviceDownlevel gates the Windows 7 application-owned presentation
 * path. RE8 queries this interface before requesting CommandQueueDownlevel. */
static const uint8_t k_iid_d3d12_device_downlevel[16] = {
    0x3f, 0xee, 0xea, 0x74, 0x4b, 0x2f, 0x6d, 0x47,
    0x82, 0xba, 0x2b, 0x85, 0xcb, 0x49, 0xe3, 0x10
};
/* ID3D12CommandQueueDownlevel, used by applications targeting the Windows 7
 * D3D12 compatibility runtime to present an application-owned texture. */
static const uint8_t k_iid_d3d12_command_queue_downlevel[16] = {
    0xef, 0xc5, 0xa8, 0x38, 0xcb, 0x7c, 0x81, 0x4e,
    0x91, 0x4f, 0xa6, 0xe9, 0xd0, 0x72, 0xc4, 0x94
};

typedef struct BeerD3D12Device {
    void **vtable;
    uint32_t references;
} BeerD3D12Device;

typedef struct BeerD3D12DescriptorHeap BeerD3D12DescriptorHeap;
typedef struct BeerD3D12RootSignature BeerD3D12RootSignature;
typedef struct BeerD3D12PipelineState BeerD3D12PipelineState;
static void *g_resource_vtable[15];
static uint32_t __attribute__((ms_abi))
descriptor_heap_add_ref(BeerD3D12DescriptorHeap *self);
static uint32_t __attribute__((ms_abi))
descriptor_heap_release(BeerD3D12DescriptorHeap *self);
static uint32_t __attribute__((ms_abi))
root_signature_add_ref(BeerD3D12RootSignature *self);
static uint32_t __attribute__((ms_abi))
root_signature_release(BeerD3D12RootSignature *self);
static uint32_t __attribute__((ms_abi))
pipeline_state_add_ref(BeerD3D12PipelineState *self);
static uint32_t __attribute__((ms_abi))
pipeline_state_release(BeerD3D12PipelineState *self);

typedef struct BeerD3D12CommandAllocator {
    void **vtable;
    uint32_t references;
    BeerD3D12Device *device;
    uint32_t type;
} BeerD3D12CommandAllocator;

typedef struct BeerD3D12CommandQueue {
    void **vtable;
    uint32_t references;
    BeerD3D12Device *device;
    uint32_t type;
    uint32_t priority;
    uint32_t flags;
    uint32_t node_mask;
    /* ID3D12CommandQueueDownlevel is a separate COM interface whose `this`
     * points at this embedded vtable field. */
    void **downlevel_vtable;
} BeerD3D12CommandQueue;

typedef enum BeerD3D12RecordedOperation {
    BEER_D3D12_OP_COPY_BUFFER,
    BEER_D3D12_OP_COPY_TEXTURE,
    BEER_D3D12_OP_COPY_RESOURCE,
    BEER_D3D12_OP_BARRIER,
    BEER_D3D12_OP_CLEAR_RTV,
    BEER_D3D12_OP_DISPATCH,
    BEER_D3D12_OP_DRAW_INSTANCED,
    BEER_D3D12_OP_DRAW_INDEXED_INSTANCED,
    BEER_D3D12_OP_SET_RENDER_TARGETS,
    BEER_D3D12_OP_END_QUERY,
    BEER_D3D12_OP_RESOLVE_QUERY,
    BEER_D3D12_OP_COUNT
} BeerD3D12RecordedOperation;

typedef struct BeerD3D12RecordedDispatch {
    BeerD3D12ComputeDispatch state;
    struct BeerD3D12RecordedDispatch *next;
} BeerD3D12RecordedDispatch;

typedef struct BeerD3D12GraphicsCommandList {
    void **vtable;
    uint32_t references;
    BeerD3D12Device *device;
    BeerD3D12CommandAllocator *allocator;
    uint32_t type;
    uint32_t closed;
    BeerD3D12DescriptorHeap *descriptor_heaps[2];
    uint32_t descriptor_heap_count;
    BeerD3D12RootSignature *compute_root_signature;
    BeerD3D12RootSignature *graphics_root_signature;
    BeerD3D12PipelineState *pipeline_state;
    BeerD3D12ComputeBinding compute_bindings[BEER_D3D12_MAX_ROOT_PARAMETERS];
    BeerD3D12RecordedDispatch *dispatches;
    BeerD3D12RecordedDispatch **dispatch_tail;
    float blend_factor[4];
    uint32_t operation_counts[BEER_D3D12_OP_COUNT];
    uint64_t recording_generation;
} BeerD3D12GraphicsCommandList;

typedef struct BeerD3D12FenceWaiter {
    uint64_t value;
    uint64_t event_handle;
    struct BeerD3D12FenceWaiter *next;
} BeerD3D12FenceWaiter;

typedef struct BeerD3D12Fence {
    void **vtable;
    uint32_t references;
    BeerD3D12Device *device;
    uint64_t completed_value;
    atomic_flag waiters_lock;
    BeerD3D12FenceWaiter *waiters;
} BeerD3D12Fence;

static BeerD3D12SignalEventFn g_signal_event;
static BeerD3D12PresentFn g_present;
static BeerD3D12ClearTargetFn g_clear_target;
static BeerD3D12ExecuteComputeFn g_execute_compute;
static BeerD3D12SubmissionObserverFn g_submission_observer;

void beer_d3d12_set_event_signaler(BeerD3D12SignalEventFn signaler)
{
    g_signal_event = signaler;
}

void beer_d3d12_set_presenter(BeerD3D12PresentFn presenter)
{
    g_present = presenter;
}

void beer_d3d12_set_clear_target(BeerD3D12ClearTargetFn clear_target)
{
    g_clear_target = clear_target;
}

void beer_d3d12_set_compute_executor(BeerD3D12ExecuteComputeFn executor)
{
    g_execute_compute = executor;
}

void beer_d3d12_set_submission_observer(
    BeerD3D12SubmissionObserverFn observer)
{
    g_submission_observer = observer;
}

static void fence_lock(BeerD3D12Fence *fence)
{
    while (atomic_flag_test_and_set_explicit(&fence->waiters_lock,
                                             memory_order_acquire)) { }
}

static void fence_unlock(BeerD3D12Fence *fence)
{
    atomic_flag_clear_explicit(&fence->waiters_lock, memory_order_release);
}

typedef struct BeerD3D12DescriptorHeap {
    void **vtable;
    uint32_t references;
    BeerD3D12Device *device;
    uint32_t description[4];
    uint64_t cpu_start;
    uint64_t gpu_start;
    void *storage;
    size_t storage_size;
} BeerD3D12DescriptorHeap;

typedef struct BeerD3D12QueryHeap {
    void **vtable;
    uint32_t references;
    BeerD3D12Device *device;
    uint32_t description[3];
    uint64_t *results;
} BeerD3D12QueryHeap;

typedef struct BeerD3D12CommandSignature {
    void **vtable;
    uint32_t references;
    BeerD3D12Device *device;
    uint32_t byte_stride;
    uint32_t argument_count;
    uint32_t node_mask;
    uint32_t _padding;
    uint8_t *arguments;
    size_t arguments_size;
} BeerD3D12CommandSignature;

typedef struct BeerD3D12PrivateData {
    uint8_t guid[16];
    uint8_t *bytes;
    uint32_t size;
    void *interface_value;
    struct BeerD3D12PrivateData *next;
} BeerD3D12PrivateData;

struct BeerD3D12RootSignature {
    void **vtable;
    uint32_t references;
    BeerD3D12Device *device;
    uint8_t *blob;
    size_t blob_size;
    uint32_t node_mask;
    BeerD3D12PrivateData *private_data;
    uint16_t *name;
};

struct BeerD3D12PipelineState {
    void **vtable;
    uint32_t references;
    BeerD3D12Device *device;
    uint8_t *stream;
    size_t stream_size;
    const uint8_t *compute_shader;
    size_t compute_shader_size;
    uint64_t compute_shader_hash;
};

typedef struct BeerD3DBlob {
    void **vtable;
    uint32_t references;
    size_t size;
    uint8_t data[];
} BeerD3DBlob;

typedef struct BeerD3D12ResourceAllocationInfo {
    uint64_t size_in_bytes;
    uint64_t alignment;
} BeerD3D12ResourceAllocationInfo;

typedef struct BeerD3D12ResourceDesc {
    uint32_t dimension;
    uint32_t _padding0;
    uint64_t alignment;
    uint64_t width;
    uint32_t height;
    uint16_t depth_or_array_size;
    uint16_t mip_levels;
    uint32_t format;
    uint32_t sample_count;
    uint32_t sample_quality;
    uint32_t layout;
    uint32_t flags;
    uint32_t _padding1;
} BeerD3D12ResourceDesc;

typedef struct BeerD3D12Heap BeerD3D12Heap;

typedef struct BeerD3D12Resource {
    void **vtable;
    uint32_t references;
    BeerD3D12Device *device;
    BeerD3D12Heap *heap;
    BeerD3D12ResourceDesc description;
    uint8_t *storage;
    size_t storage_size;
    uint64_t gpu_virtual_address;
    uint64_t content_serial;
    uint32_t heap_properties[5];
    uint32_t heap_flags;
    uint32_t owns_storage;
    uint32_t state;
} BeerD3D12Resource;

typedef struct BeerD3D12HeapDesc {
    uint64_t size_in_bytes;
    uint32_t properties[5];
    uint32_t _padding;
    uint64_t alignment;
    uint32_t flags;
    uint32_t _padding2;
} BeerD3D12HeapDesc;

struct BeerD3D12Heap {
    void **vtable;
    uint32_t references;
    BeerD3D12Device *device;
    BeerD3D12HeapDesc description;
    uint8_t *storage;
};

_Static_assert(sizeof(BeerD3D12HeapDesc) == 48,
               "D3D12_HEAP_DESC must be 48 bytes on Win64");

_Static_assert(sizeof(BeerD3D12ResourceDesc) == 56,
               "D3D12_RESOURCE_DESC must be 56 bytes on Win64");

static uint64_t __attribute__((ms_abi))
device_query_interface(BeerD3D12Device *self, const uint8_t *riid, void **output);
static uint32_t __attribute__((ms_abi)) device_add_ref(BeerD3D12Device *self);
static uint32_t __attribute__((ms_abi)) device_release(BeerD3D12Device *self);
static uint64_t __attribute__((ms_abi))
device_get_private_data(BeerD3D12Device *self, const uint8_t *guid,
                        uint32_t *size, void *data);
static uint32_t __attribute__((ms_abi)) device_get_node_count(BeerD3D12Device *self);
static uint64_t __attribute__((ms_abi))
device_check_feature_support(BeerD3D12Device *self, uint32_t feature,
                             void *data, uint32_t data_size);
static uint64_t __attribute__((ms_abi))
device_create_command_queue(BeerD3D12Device *self, const uint32_t *description,
                            const uint8_t *riid, void **queue);
static uint64_t __attribute__((ms_abi))
device_create_command_allocator(BeerD3D12Device *self, uint32_t type,
                                const uint8_t *riid, void **allocator);
static uint64_t __attribute__((ms_abi))
device_create_descriptor_heap(BeerD3D12Device *self,
                              const uint32_t *description,
                              const uint8_t *riid, void **heap);
static uint64_t __attribute__((ms_abi))
device_create_command_list(BeerD3D12Device *self, uint32_t node_mask,
                           uint32_t type, BeerD3D12CommandAllocator *allocator,
                           void *initial_state, const uint8_t *riid,
                           void **command_list);
static uint64_t __attribute__((ms_abi))
device_create_fence(BeerD3D12Device *self, uint64_t initial_value,
                    uint32_t flags, const uint8_t *riid, void **fence);
static uint64_t __attribute__((ms_abi))
device_create_query_heap(BeerD3D12Device *self, const uint32_t *description,
                         const uint8_t *riid, void **query_heap);
static uint64_t __attribute__((ms_abi))
device_create_root_signature(BeerD3D12Device *self, uint32_t node_mask,
                             const void *blob, size_t blob_size,
                             const uint8_t *riid, void **root_signature);
static uint64_t __attribute__((ms_abi))
device_create_command_signature(BeerD3D12Device *self,
                                const void *description,
                                void *root_signature,
                                const uint8_t *riid,
                                void **command_signature);
static BeerD3D12ResourceAllocationInfo *__attribute__((ms_abi))
device_get_resource_allocation_info(BeerD3D12Device *self,
                                    BeerD3D12ResourceAllocationInfo *result,
                                    uint32_t visible_mask, uint32_t count,
                                    const BeerD3D12ResourceDesc *descriptions);
static uint64_t align_up_u64(uint64_t value, uint64_t alignment);

static uint64_t __attribute__((ms_abi))
device_create_committed_resource(BeerD3D12Device *self,
                                 const uint32_t *heap_properties,
                                 uint32_t heap_flags,
                                 const BeerD3D12ResourceDesc *description,
                                 uint32_t initial_state,
                                 const void *optimized_clear_value,
                                 const uint8_t *riid, void **resource);
static uint64_t __attribute__((ms_abi))
device_create_heap(BeerD3D12Device *self, const uint64_t *description,
                   const uint8_t *riid, void **heap);
static uint64_t __attribute__((ms_abi))
device_create_placed_resource(BeerD3D12Device *self, BeerD3D12Heap *heap,
                              uint64_t heap_offset,
                              const BeerD3D12ResourceDesc *description,
                              uint32_t initial_state,
                              const void *optimized_clear_value,
                              const uint8_t *riid, void **resource);
static uint64_t __attribute__((ms_abi))
resource_query_interface(BeerD3D12Resource *self,
                         const uint8_t *riid, void **output);
static uint32_t __attribute__((ms_abi)) resource_add_ref(BeerD3D12Resource *self);
static uint32_t __attribute__((ms_abi)) resource_release(BeerD3D12Resource *self);
static uint64_t __attribute__((ms_abi)) resource_notimpl(BeerD3D12Resource *self);
static uint64_t __attribute__((ms_abi))
resource_get_device(BeerD3D12Resource *self, const uint8_t *riid, void **device);
static uint64_t __attribute__((ms_abi))
resource_map(BeerD3D12Resource *self, uint32_t subresource,
             const void *read_range, void **data);
static void __attribute__((ms_abi))
resource_unmap(BeerD3D12Resource *self, uint32_t subresource,
               const void *written_range);
static BeerD3D12ResourceDesc *__attribute__((ms_abi))
resource_get_desc(BeerD3D12Resource *self, BeerD3D12ResourceDesc *result);
static uint64_t __attribute__((ms_abi))
resource_get_gpu_virtual_address(BeerD3D12Resource *self);
static uint64_t __attribute__((ms_abi))
heap_query_interface(BeerD3D12Heap *self, const uint8_t *riid, void **output);
static uint32_t __attribute__((ms_abi)) heap_add_ref(BeerD3D12Heap *self);
static uint32_t __attribute__((ms_abi)) heap_release(BeerD3D12Heap *self);
static uint64_t __attribute__((ms_abi)) heap_notimpl(BeerD3D12Heap *self);
static uint64_t __attribute__((ms_abi))
heap_get_device(BeerD3D12Heap *self, const uint8_t *riid, void **device);
static BeerD3D12HeapDesc *__attribute__((ms_abi))
heap_get_desc(BeerD3D12Heap *self, BeerD3D12HeapDesc *description);
static uint64_t __attribute__((ms_abi))
allocator_query_interface(BeerD3D12CommandAllocator *self,
                          const uint8_t *riid, void **output);
static uint32_t __attribute__((ms_abi))
allocator_add_ref(BeerD3D12CommandAllocator *self);
static uint32_t __attribute__((ms_abi))
allocator_release(BeerD3D12CommandAllocator *self);
static uint64_t __attribute__((ms_abi))
allocator_get_private_data(BeerD3D12CommandAllocator *self,
                           const uint8_t *guid, uint32_t *size, void *data);
static uint64_t __attribute__((ms_abi))
allocator_set_private_data(BeerD3D12CommandAllocator *self,
                           const uint8_t *guid, uint32_t size, const void *data);
static uint64_t __attribute__((ms_abi))
allocator_set_private_data_interface(BeerD3D12CommandAllocator *self,
                                     const uint8_t *guid, void *data);
static uint64_t __attribute__((ms_abi))
allocator_set_name(BeerD3D12CommandAllocator *self, const uint16_t *name);
static uint64_t __attribute__((ms_abi))
allocator_get_device(BeerD3D12CommandAllocator *self,
                     const uint8_t *riid, void **device);
static uint64_t __attribute__((ms_abi))
allocator_reset(BeerD3D12CommandAllocator *self);
static uint64_t __attribute__((ms_abi))
queue_query_interface(BeerD3D12CommandQueue *self,
                      const uint8_t *riid, void **output);
static uint64_t __attribute__((ms_abi))
downlevel_query_interface(void *interface, const uint8_t *riid, void **output);
static uint32_t __attribute__((ms_abi)) downlevel_add_ref(void *interface);
static uint32_t __attribute__((ms_abi)) downlevel_release(void *interface);
static uint64_t __attribute__((ms_abi))
downlevel_present(void *interface, BeerD3D12GraphicsCommandList *command_list,
                  BeerD3D12Resource *source_texture, uint64_t window,
                  uint32_t flags);
static uint32_t __attribute__((ms_abi)) queue_add_ref(BeerD3D12CommandQueue *self);
static uint32_t __attribute__((ms_abi)) queue_release(BeerD3D12CommandQueue *self);
static uint64_t __attribute__((ms_abi)) queue_notimpl(BeerD3D12CommandQueue *self);
static uint64_t __attribute__((ms_abi))
queue_get_device(BeerD3D12CommandQueue *self, const uint8_t *riid, void **device);
static void __attribute__((ms_abi))
queue_update_tile_mappings(BeerD3D12CommandQueue *self);
static void __attribute__((ms_abi))
queue_copy_tile_mappings(BeerD3D12CommandQueue *self);
static void __attribute__((ms_abi))
queue_execute_command_lists(BeerD3D12CommandQueue *self,
                            uint32_t count, void *const *lists);
static void __attribute__((ms_abi))
queue_set_marker(BeerD3D12CommandQueue *self, uint32_t metadata,
                 const void *data, uint32_t size);
static void __attribute__((ms_abi))
queue_begin_event(BeerD3D12CommandQueue *self, uint32_t metadata,
                  const void *data, uint32_t size);
static void __attribute__((ms_abi)) queue_end_event(BeerD3D12CommandQueue *self);
static uint64_t __attribute__((ms_abi))
queue_signal(BeerD3D12CommandQueue *self, void *fence, uint64_t value);
static uint64_t __attribute__((ms_abi))
queue_wait(BeerD3D12CommandQueue *self, void *fence, uint64_t value);
static uint64_t __attribute__((ms_abi))
queue_get_timestamp_frequency(BeerD3D12CommandQueue *self, uint64_t *frequency);
static uint32_t __attribute__((ms_abi))
device_get_descriptor_handle_increment_size(BeerD3D12Device *self,
                                            uint32_t heap_type);
static void __attribute__((ms_abi))
device_create_constant_buffer_view(BeerD3D12Device *self,
                                   const void *description,
                                   uint64_t destination);
static void __attribute__((ms_abi))
device_create_shader_resource_view(BeerD3D12Device *self, void *resource,
                                   const void *description,
                                   uint64_t destination);
static void __attribute__((ms_abi))
device_create_unordered_access_view(BeerD3D12Device *self, void *resource,
                                    void *counter_resource,
                                    const void *description,
                                    uint64_t destination);
static void __attribute__((ms_abi))
device_create_render_target_view(BeerD3D12Device *self, void *resource,
                                 const void *description,
                                 uint64_t destination);
static void __attribute__((ms_abi))
device_create_depth_stencil_view(BeerD3D12Device *self, void *resource,
                                 const void *description,
                                 uint64_t destination);
static void __attribute__((ms_abi))
device_create_sampler(BeerD3D12Device *self, const void *description,
                      uint64_t destination);
static void __attribute__((ms_abi))
device_copy_descriptors(BeerD3D12Device *self,
                        uint32_t destination_range_count,
                        const uint64_t *destination_starts,
                        const uint32_t *destination_sizes,
                        uint32_t source_range_count,
                        const uint64_t *source_starts,
                        const uint32_t *source_sizes,
                        uint32_t heap_type);
static void __attribute__((ms_abi))
device_copy_descriptors_simple(BeerD3D12Device *self, uint32_t count,
                               uint64_t destination, uint64_t source,
                               uint32_t heap_type);
static void __attribute__((ms_abi))
device_get_copyable_footprints(BeerD3D12Device *self,
                               const BeerD3D12ResourceDesc *description,
                               uint32_t first_subresource,
                               uint32_t subresource_count,
                               uint64_t base_offset, void *layouts,
                               uint32_t *row_counts, uint64_t *row_sizes,
                               uint64_t *total_bytes);
static uint64_t __attribute__((ms_abi))
queue_get_clock_calibration(BeerD3D12CommandQueue *self,
                            uint64_t *gpu, uint64_t *cpu);
static void __attribute__((ms_abi))
queue_get_desc(BeerD3D12CommandQueue *self, uint32_t *description);
static uint64_t __attribute__((ms_abi))
command_list_query_interface(BeerD3D12GraphicsCommandList *self,
                             const uint8_t *riid, void **output);
static uint32_t __attribute__((ms_abi))
command_list_add_ref(BeerD3D12GraphicsCommandList *self);
static uint32_t __attribute__((ms_abi))
command_list_release(BeerD3D12GraphicsCommandList *self);
static uint64_t __attribute__((ms_abi))
command_list_notimpl(BeerD3D12GraphicsCommandList *self);
#define DECLARE_COMMAND_LIST_SLOT_PROTO(slot) \
    static uint64_t __attribute__((ms_abi)) command_list_unsupported_##slot( \
        BeerD3D12GraphicsCommandList *self, uint64_t a, uint64_t b, \
        uint64_t c, uint64_t d, uint64_t e, uint64_t f);
DECLARE_COMMAND_LIST_SLOT_PROTO(11)
DECLARE_COMMAND_LIST_SLOT_PROTO(12)
DECLARE_COMMAND_LIST_SLOT_PROTO(13)
DECLARE_COMMAND_LIST_SLOT_PROTO(14)
DECLARE_COMMAND_LIST_SLOT_PROTO(15)
DECLARE_COMMAND_LIST_SLOT_PROTO(16)
DECLARE_COMMAND_LIST_SLOT_PROTO(17)
DECLARE_COMMAND_LIST_SLOT_PROTO(18)
DECLARE_COMMAND_LIST_SLOT_PROTO(19)
DECLARE_COMMAND_LIST_SLOT_PROTO(20)
DECLARE_COMMAND_LIST_SLOT_PROTO(21)
DECLARE_COMMAND_LIST_SLOT_PROTO(22)
DECLARE_COMMAND_LIST_SLOT_PROTO(23)
DECLARE_COMMAND_LIST_SLOT_PROTO(24)
DECLARE_COMMAND_LIST_SLOT_PROTO(25)
DECLARE_COMMAND_LIST_SLOT_PROTO(26)
DECLARE_COMMAND_LIST_SLOT_PROTO(27)
DECLARE_COMMAND_LIST_SLOT_PROTO(28)
DECLARE_COMMAND_LIST_SLOT_PROTO(29)
DECLARE_COMMAND_LIST_SLOT_PROTO(30)
DECLARE_COMMAND_LIST_SLOT_PROTO(31)
DECLARE_COMMAND_LIST_SLOT_PROTO(32)
DECLARE_COMMAND_LIST_SLOT_PROTO(33)
DECLARE_COMMAND_LIST_SLOT_PROTO(34)
DECLARE_COMMAND_LIST_SLOT_PROTO(35)
DECLARE_COMMAND_LIST_SLOT_PROTO(36)
DECLARE_COMMAND_LIST_SLOT_PROTO(37)
DECLARE_COMMAND_LIST_SLOT_PROTO(38)
DECLARE_COMMAND_LIST_SLOT_PROTO(39)
DECLARE_COMMAND_LIST_SLOT_PROTO(40)
DECLARE_COMMAND_LIST_SLOT_PROTO(41)
DECLARE_COMMAND_LIST_SLOT_PROTO(42)
DECLARE_COMMAND_LIST_SLOT_PROTO(43)
DECLARE_COMMAND_LIST_SLOT_PROTO(44)
DECLARE_COMMAND_LIST_SLOT_PROTO(45)
DECLARE_COMMAND_LIST_SLOT_PROTO(46)
DECLARE_COMMAND_LIST_SLOT_PROTO(47)
DECLARE_COMMAND_LIST_SLOT_PROTO(48)
DECLARE_COMMAND_LIST_SLOT_PROTO(49)
DECLARE_COMMAND_LIST_SLOT_PROTO(50)
DECLARE_COMMAND_LIST_SLOT_PROTO(51)
DECLARE_COMMAND_LIST_SLOT_PROTO(52)
DECLARE_COMMAND_LIST_SLOT_PROTO(53)
DECLARE_COMMAND_LIST_SLOT_PROTO(54)
DECLARE_COMMAND_LIST_SLOT_PROTO(55)
DECLARE_COMMAND_LIST_SLOT_PROTO(56)
DECLARE_COMMAND_LIST_SLOT_PROTO(57)
DECLARE_COMMAND_LIST_SLOT_PROTO(58)
DECLARE_COMMAND_LIST_SLOT_PROTO(59)
#undef DECLARE_COMMAND_LIST_SLOT_PROTO
static uint64_t __attribute__((ms_abi))
command_list_get_device(BeerD3D12GraphicsCommandList *self,
                        const uint8_t *riid, void **device);
static uint32_t __attribute__((ms_abi))
command_list_get_type(BeerD3D12GraphicsCommandList *self);
static uint64_t __attribute__((ms_abi))
command_list_close(BeerD3D12GraphicsCommandList *self);
static uint64_t __attribute__((ms_abi))
command_list_reset(BeerD3D12GraphicsCommandList *self,
                   BeerD3D12CommandAllocator *allocator, void *initial_state);
static void __attribute__((ms_abi))
command_list_dispatch(BeerD3D12GraphicsCommandList *self,
                      uint32_t group_count_x, uint32_t group_count_y,
                      uint32_t group_count_z)
{
    if (!self || self->closed || !group_count_x || !group_count_y ||
        !group_count_z)
        return;
    BeerD3D12RecordedDispatch *dispatch = calloc(1, sizeof(*dispatch));
    if (!dispatch) return;
    dispatch->state.pipeline_state = self->pipeline_state;
    dispatch->state.root_signature = self->compute_root_signature;
    dispatch->state.group_count_x = group_count_x;
    dispatch->state.group_count_y = group_count_y;
    dispatch->state.group_count_z = group_count_z;
    dispatch->state.descriptor_heap_count = self->descriptor_heap_count;
    for (uint32_t index = 0; index < self->descriptor_heap_count; ++index) {
        BeerD3D12DescriptorHeap *heap = self->descriptor_heaps[index];
        dispatch->state.descriptor_heaps[index] = heap;
        if (heap) {
            dispatch->state.descriptor_heap_gpu_start[index] = heap->gpu_start;
            dispatch->state.descriptor_heap_storage[index] = heap->storage;
            dispatch->state.descriptor_heap_size[index] = heap->storage_size;
            dispatch->state.descriptor_heap_type[index] = heap->description[0];
            dispatch->state.descriptor_heap_increment[index] =
                device_get_descriptor_handle_increment_size(
                    self->device, heap->description[0]);
        }
    }
    memcpy(dispatch->state.bindings, self->compute_bindings,
           sizeof(dispatch->state.bindings));
    for (uint32_t parameter = 0;
         parameter < BEER_D3D12_MAX_ROOT_PARAMETERS; ++parameter) {
        BeerD3D12ComputeBinding *binding = &dispatch->state.bindings[parameter];
        if (binding->kind != 1 || !binding->descriptor_table) continue;
        for (uint32_t heap_index = 0;
             heap_index < self->descriptor_heap_count; ++heap_index) {
            BeerD3D12DescriptorHeap *heap = self->descriptor_heaps[heap_index];
            if (!heap || binding->descriptor_table < heap->gpu_start ||
                binding->descriptor_table - heap->gpu_start >= heap->storage_size)
                continue;
            size_t offset = (size_t)(binding->descriptor_table - heap->gpu_start);
            if (heap->storage_size - offset < sizeof(BeerD3D12Resource *)) break;
            BeerD3D12Resource *resource = NULL;
            memcpy(&resource, (const uint8_t *)heap->storage + offset,
                   sizeof(resource));
            if (resource && resource->vtable == g_resource_vtable &&
                resource->device == self->device) {
                binding->resource = resource;
                binding->resource_storage = resource->storage;
                binding->resource_storage_size = resource->storage_size;
            }
            break;
        }
    }
    if (self->pipeline_state) {
        pipeline_state_add_ref(self->pipeline_state);
        dispatch->state.shader = self->pipeline_state->compute_shader;
        dispatch->state.shader_size = self->pipeline_state->compute_shader_size;
        dispatch->state.shader_hash = self->pipeline_state->compute_shader_hash;
    }
    if (self->compute_root_signature) {
        root_signature_add_ref(self->compute_root_signature);
        dispatch->state.root_signature_blob = self->compute_root_signature->blob;
        dispatch->state.root_signature_blob_size =
            self->compute_root_signature->blob_size;
    }
    for (uint32_t index = 0; index < self->descriptor_heap_count; ++index) {
        if (self->descriptor_heaps[index])
            descriptor_heap_add_ref(self->descriptor_heaps[index]);
    }
    if (!self->dispatch_tail) self->dispatch_tail = &self->dispatches;
    *self->dispatch_tail = dispatch;
    self->dispatch_tail = &dispatch->next;
    ++self->operation_counts[BEER_D3D12_OP_DISPATCH];
}

static void __attribute__((ms_abi))
command_list_copy_buffer_region(BeerD3D12GraphicsCommandList *self,
                                BeerD3D12Resource *destination,
                                uint64_t destination_offset,
                                BeerD3D12Resource *source,
                                uint64_t source_offset, uint64_t byte_count);
static void __attribute__((ms_abi))
command_list_copy_texture_region(BeerD3D12GraphicsCommandList *self,
                                 const void *destination_location,
                                 uint32_t destination_x, uint32_t destination_y,
                                 uint32_t destination_z,
                                 const void *source_location,
                                 const void *source_box);
static void __attribute__((ms_abi))
command_list_copy_resource(BeerD3D12GraphicsCommandList *self,
                           BeerD3D12Resource *destination,
                           BeerD3D12Resource *source);
static void __attribute__((ms_abi))
command_list_set_pipeline_state(BeerD3D12GraphicsCommandList *self,
                                BeerD3D12PipelineState *pipeline_state)
{
    if (!self || self->closed ||
        (pipeline_state && pipeline_state->device != self->device))
        return;
    if (pipeline_state == self->pipeline_state) return;
    if (pipeline_state) pipeline_state_add_ref(pipeline_state);
    if (self->pipeline_state) pipeline_state_release(self->pipeline_state);
    self->pipeline_state = pipeline_state;
}

static void __attribute__((ms_abi))
command_list_draw_instanced(BeerD3D12GraphicsCommandList *self,
                            uint32_t vertex_count, uint32_t instance_count,
                            uint32_t start_vertex, uint32_t start_instance);
static void __attribute__((ms_abi))
command_list_draw_indexed_instanced(BeerD3D12GraphicsCommandList *self,
                                    uint32_t index_count,
                                    uint32_t instance_count,
                                    uint32_t start_index,
                                    int32_t base_vertex,
                                    uint32_t start_instance);
static void __attribute__((ms_abi))
command_list_set_render_targets(BeerD3D12GraphicsCommandList *self,
                                uint32_t render_target_count,
                                const uint64_t *render_targets,
                                uint32_t single_handle_range,
                                const uint64_t *depth_stencil);
static void __attribute__((ms_abi))
command_list_set_blend_factor(BeerD3D12GraphicsCommandList *self,
                              const float *blend_factor);
static void __attribute__((ms_abi))
command_list_resource_barrier(BeerD3D12GraphicsCommandList *self,
                              uint32_t count, const void *barriers);
static void __attribute__((ms_abi))
command_list_set_descriptor_heaps(BeerD3D12GraphicsCommandList *self,
                                  uint32_t count,
                                  BeerD3D12DescriptorHeap *const *heaps);
static void __attribute__((ms_abi))
command_list_set_compute_root_signature(BeerD3D12GraphicsCommandList *self,
                                        BeerD3D12RootSignature *root_signature);
static void __attribute__((ms_abi))
command_list_set_graphics_root_signature(BeerD3D12GraphicsCommandList *self,
                                         BeerD3D12RootSignature *root_signature);
static void __attribute__((ms_abi))
command_list_set_compute_root_descriptor_table(
    BeerD3D12GraphicsCommandList *self, uint32_t root_parameter,
    uint64_t descriptor);
static void __attribute__((ms_abi))
command_list_set_graphics_root_descriptor_table(
    BeerD3D12GraphicsCommandList *self, uint32_t root_parameter,
    uint64_t descriptor);
static void __attribute__((ms_abi))
command_list_set_compute_root_constants(
    BeerD3D12GraphicsCommandList *self, uint32_t root_parameter,
    uint32_t count, const void *data, uint32_t destination_offset);
static void __attribute__((ms_abi))
command_list_set_compute_root_cbv(BeerD3D12GraphicsCommandList *self,
                                  uint32_t root_parameter, uint64_t address);
static void __attribute__((ms_abi))
command_list_set_compute_root_srv(BeerD3D12GraphicsCommandList *self,
                                  uint32_t root_parameter, uint64_t address);
static void __attribute__((ms_abi))
command_list_set_compute_root_uav(BeerD3D12GraphicsCommandList *self,
                                  uint32_t root_parameter, uint64_t address);
static void __attribute__((ms_abi))
command_list_set_index_buffer(BeerD3D12GraphicsCommandList *self,
                              const void *view);
static void __attribute__((ms_abi))
command_list_set_vertex_buffers(BeerD3D12GraphicsCommandList *self,
                                uint32_t start_slot, uint32_t count,
                                const void *views);
static void __attribute__((ms_abi))
command_list_clear_render_target_view(BeerD3D12GraphicsCommandList *self,
                                      uint64_t descriptor,
                                      const float *color, uint32_t rect_count,
                                      const void *rects);
static void __attribute__((ms_abi))
command_list_end_query(BeerD3D12GraphicsCommandList *self,
                       BeerD3D12QueryHeap *heap, uint32_t type,
                       uint32_t index);
static void __attribute__((ms_abi))
command_list_resolve_query_data(BeerD3D12GraphicsCommandList *self,
                                BeerD3D12QueryHeap *heap, uint32_t type,
                                uint32_t start, uint32_t count,
                                BeerD3D12Resource *destination,
                                uint64_t destination_offset);
static uint64_t __attribute__((ms_abi))
fence_query_interface(BeerD3D12Fence *self, const uint8_t *riid, void **output);
static uint32_t __attribute__((ms_abi)) fence_add_ref(BeerD3D12Fence *self);
static uint32_t __attribute__((ms_abi)) fence_release(BeerD3D12Fence *self);
static uint64_t __attribute__((ms_abi)) fence_notimpl(BeerD3D12Fence *self);
static uint64_t __attribute__((ms_abi))
fence_get_device(BeerD3D12Fence *self, const uint8_t *riid, void **device);
static uint64_t __attribute__((ms_abi))
fence_get_completed_value(BeerD3D12Fence *self);
static uint64_t __attribute__((ms_abi))
fence_set_event_on_completion(BeerD3D12Fence *self, uint64_t value, void *event);
static uint64_t __attribute__((ms_abi))
fence_signal(BeerD3D12Fence *self, uint64_t value);
static uint64_t __attribute__((ms_abi))
descriptor_heap_query_interface(BeerD3D12DescriptorHeap *self,
                                const uint8_t *riid, void **output);
static uint32_t __attribute__((ms_abi))
descriptor_heap_add_ref(BeerD3D12DescriptorHeap *self);
static uint32_t __attribute__((ms_abi))
descriptor_heap_release(BeerD3D12DescriptorHeap *self);
static uint64_t __attribute__((ms_abi))
descriptor_heap_notimpl(BeerD3D12DescriptorHeap *self);
static uint64_t __attribute__((ms_abi))
descriptor_heap_get_device(BeerD3D12DescriptorHeap *self,
                           const uint8_t *riid, void **device);
static void __attribute__((ms_abi))
descriptor_heap_get_desc(BeerD3D12DescriptorHeap *self, uint32_t *description);
static uint64_t *__attribute__((ms_abi))
descriptor_heap_get_cpu_start(BeerD3D12DescriptorHeap *self, uint64_t *result);
static uint64_t *__attribute__((ms_abi))
descriptor_heap_get_gpu_start(BeerD3D12DescriptorHeap *self, uint64_t *result);
static uint64_t __attribute__((ms_abi))
query_heap_query_interface(BeerD3D12QueryHeap *self,
                           const uint8_t *riid, void **output);
static uint32_t __attribute__((ms_abi))
query_heap_add_ref(BeerD3D12QueryHeap *self);
static uint32_t __attribute__((ms_abi))
query_heap_release(BeerD3D12QueryHeap *self);
static uint64_t __attribute__((ms_abi))
query_heap_notimpl(BeerD3D12QueryHeap *self);
static uint64_t __attribute__((ms_abi))
query_heap_get_device(BeerD3D12QueryHeap *self,
                      const uint8_t *riid, void **device);
static uint64_t __attribute__((ms_abi))
command_signature_query_interface(BeerD3D12CommandSignature *self,
                                  const uint8_t *riid, void **output);
static uint32_t __attribute__((ms_abi))
command_signature_add_ref(BeerD3D12CommandSignature *self);
static uint32_t __attribute__((ms_abi))
command_signature_release(BeerD3D12CommandSignature *self);
static uint64_t __attribute__((ms_abi))
command_signature_notimpl(BeerD3D12CommandSignature *self);
static uint64_t __attribute__((ms_abi))
command_signature_get_device(BeerD3D12CommandSignature *self,
                             const uint8_t *riid, void **device);
static uint64_t __attribute__((ms_abi))
root_signature_query_interface(BeerD3D12RootSignature *self,
                               const uint8_t *riid, void **output);
static uint32_t __attribute__((ms_abi))
root_signature_add_ref(BeerD3D12RootSignature *self);
static uint32_t __attribute__((ms_abi))
root_signature_release(BeerD3D12RootSignature *self);
static uint64_t __attribute__((ms_abi))
root_signature_get_private_data(BeerD3D12RootSignature *self,
                                const uint8_t *guid, uint32_t *size, void *data);
static uint64_t __attribute__((ms_abi))
root_signature_set_private_data(BeerD3D12RootSignature *self,
                                const uint8_t *guid, uint32_t size,
                                const void *data);
static uint64_t __attribute__((ms_abi))
root_signature_set_private_data_interface(BeerD3D12RootSignature *self,
                                          const uint8_t *guid, void *data);
static uint64_t __attribute__((ms_abi))
root_signature_set_name(BeerD3D12RootSignature *self, const uint16_t *name);
static uint64_t __attribute__((ms_abi))
root_signature_get_device(BeerD3D12RootSignature *self,
                          const uint8_t *riid, void **device);
static uint64_t __attribute__((ms_abi))
device_create_graphics_pipeline_state(BeerD3D12Device *self,
                                      const void *description,
                                      const uint8_t *riid,
                                      void **pipeline_state);
static uint64_t __attribute__((ms_abi))
device_create_compute_pipeline_state(BeerD3D12Device *self,
                                     const void *description,
                                     const uint8_t *riid,
                                     void **pipeline_state);
static uint64_t __attribute__((ms_abi))
device2_create_pipeline_state(BeerD3D12Device *self, const void *description,
                              const uint8_t *riid, void **pipeline_state);
static uint64_t __attribute__((ms_abi))
pipeline_state_query_interface(BeerD3D12PipelineState *self,
                               const uint8_t *riid, void **output);
static uint32_t __attribute__((ms_abi))
pipeline_state_add_ref(BeerD3D12PipelineState *self);
static uint32_t __attribute__((ms_abi))
pipeline_state_release(BeerD3D12PipelineState *self);
static uint64_t __attribute__((ms_abi))
pipeline_state_notimpl(BeerD3D12PipelineState *self);
static uint64_t __attribute__((ms_abi))
pipeline_state_get_device(BeerD3D12PipelineState *self,
                          const uint8_t *riid, void **device);
static uint64_t __attribute__((ms_abi))
pipeline_state_get_cached_blob(BeerD3D12PipelineState *self, void **blob);
static uint64_t __attribute__((ms_abi))
blob_query_interface(BeerD3DBlob *self, const uint8_t *riid, void **output);
static uint32_t __attribute__((ms_abi)) blob_add_ref(BeerD3DBlob *self);
static uint32_t __attribute__((ms_abi)) blob_release(BeerD3DBlob *self);
static void *__attribute__((ms_abi)) blob_get_buffer_pointer(BeerD3DBlob *self);
static size_t __attribute__((ms_abi)) blob_get_buffer_size(BeerD3DBlob *self);
#define DECLARE_UNSUPPORTED_SLOT(slot) \
    static uint64_t __attribute__((ms_abi)) device_unsupported_##slot( \
        BeerD3D12Device *self, uint64_t a, uint64_t b, uint64_t c, \
        uint64_t d, uint64_t e, uint64_t f) \
    { \
        (void)self; (void)a; (void)b; (void)c; \
        (void)d; (void)e; (void)f; \
        fprintf(stderr, "[D3D12] unsupported ID3D12Device slot %u -> E_NOTIMPL\n", \
                (unsigned)(slot)); \
        return BEER_E_NOTIMPL; \
    }

DECLARE_UNSUPPORTED_SLOT(3)
DECLARE_UNSUPPORTED_SLOT(4)
DECLARE_UNSUPPORTED_SLOT(5)
DECLARE_UNSUPPORTED_SLOT(6)
DECLARE_UNSUPPORTED_SLOT(8)
DECLARE_UNSUPPORTED_SLOT(9)
DECLARE_UNSUPPORTED_SLOT(10)
DECLARE_UNSUPPORTED_SLOT(11)
DECLARE_UNSUPPORTED_SLOT(12)
DECLARE_UNSUPPORTED_SLOT(14)
DECLARE_UNSUPPORTED_SLOT(15)
DECLARE_UNSUPPORTED_SLOT(16)
DECLARE_UNSUPPORTED_SLOT(17)
DECLARE_UNSUPPORTED_SLOT(18)
DECLARE_UNSUPPORTED_SLOT(19)
DECLARE_UNSUPPORTED_SLOT(20)
DECLARE_UNSUPPORTED_SLOT(21)
DECLARE_UNSUPPORTED_SLOT(22)
DECLARE_UNSUPPORTED_SLOT(23)
DECLARE_UNSUPPORTED_SLOT(24)
DECLARE_UNSUPPORTED_SLOT(25)
DECLARE_UNSUPPORTED_SLOT(26)
DECLARE_UNSUPPORTED_SLOT(27)
DECLARE_UNSUPPORTED_SLOT(28)
DECLARE_UNSUPPORTED_SLOT(29)
DECLARE_UNSUPPORTED_SLOT(30)
DECLARE_UNSUPPORTED_SLOT(31)
DECLARE_UNSUPPORTED_SLOT(32)
DECLARE_UNSUPPORTED_SLOT(33)
DECLARE_UNSUPPORTED_SLOT(34)
DECLARE_UNSUPPORTED_SLOT(35)
DECLARE_UNSUPPORTED_SLOT(36)
DECLARE_UNSUPPORTED_SLOT(37)
DECLARE_UNSUPPORTED_SLOT(38)
DECLARE_UNSUPPORTED_SLOT(39)
DECLARE_UNSUPPORTED_SLOT(40)
DECLARE_UNSUPPORTED_SLOT(41)
DECLARE_UNSUPPORTED_SLOT(42)
DECLARE_UNSUPPORTED_SLOT(43)
DECLARE_UNSUPPORTED_SLOT(44)
DECLARE_UNSUPPORTED_SLOT(45)
DECLARE_UNSUPPORTED_SLOT(46)

/* ID3D12Device2 extends the 44-slot base device with three Device1 methods and
 * CreatePipelineState at slot 47. RE8 probes newer device versions and uses
 * this stream-based pipeline path when Device2 is available. */
static void *g_device_vtable[48] = {
    [0] = device_query_interface,
    [1] = device_add_ref,
    [2] = device_release,
    [3] = device_get_private_data,
    [4] = device_unsupported_4,
    [5] = device_unsupported_5,
    [6] = device_unsupported_6,
    [7] = device_get_node_count,
    [8] = device_create_command_queue,
    [9] = device_create_command_allocator,
    [10] = device_create_graphics_pipeline_state,
    [11] = device_create_compute_pipeline_state,
    [12] = device_create_command_list,
    [13] = device_check_feature_support,
    [14] = device_create_descriptor_heap,
    [15] = device_get_descriptor_handle_increment_size,
    [16] = device_create_root_signature,
    [17] = device_create_constant_buffer_view,
    [18] = device_create_shader_resource_view,
    [19] = device_create_unordered_access_view,
    [20] = device_create_render_target_view,
    [21] = device_create_depth_stencil_view,
    [22] = device_create_sampler,
    [23] = device_copy_descriptors,
    [24] = device_copy_descriptors_simple,
    [25] = device_get_resource_allocation_info,
    [26] = device_unsupported_26,
    [27] = device_create_committed_resource,
    [28] = device_create_heap,
    [29] = device_create_placed_resource,
    [30] = device_unsupported_30,
    [31] = device_unsupported_31,
    [32] = device_unsupported_32,
    [33] = device_unsupported_33,
    [34] = device_unsupported_34,
    [35] = device_unsupported_35,
    [36] = device_create_fence,
    [37] = device_unsupported_37,
    [38] = device_get_copyable_footprints,
    [39] = device_create_query_heap,
    [40] = device_unsupported_40,
    [41] = device_create_command_signature,
    [42] = device_unsupported_42,
    [43] = device_unsupported_43,
    [44] = device_unsupported_44,
    [45] = device_unsupported_45,
    [46] = device_unsupported_46,
    [47] = device2_create_pipeline_state,
};

static BeerD3D12Device g_device = {
    .vtable = g_device_vtable,
    .references = 1,
};

static void *g_allocator_vtable[9] = {
    [0] = allocator_query_interface,
    [1] = allocator_add_ref,
    [2] = allocator_release,
    [3] = allocator_get_private_data,
    [4] = allocator_set_private_data,
    [5] = allocator_set_private_data_interface,
    [6] = allocator_set_name,
    [7] = allocator_get_device,
    [8] = allocator_reset,
};

static void *g_downlevel_queue_vtable[4] = {
    [0] = downlevel_query_interface,
    [1] = downlevel_add_ref,
    [2] = downlevel_release,
    [3] = downlevel_present,
};

static void *g_queue_vtable[19] = {
    [0] = queue_query_interface,
    [1] = queue_add_ref,
    [2] = queue_release,
    [3] = queue_notimpl,
    [4] = queue_notimpl,
    [5] = queue_notimpl,
    [6] = queue_notimpl,
    [7] = queue_get_device,
    [8] = queue_update_tile_mappings,
    [9] = queue_copy_tile_mappings,
    [10] = queue_execute_command_lists,
    [11] = queue_set_marker,
    [12] = queue_begin_event,
    [13] = queue_end_event,
    [14] = queue_signal,
    [15] = queue_wait,
    [16] = queue_get_timestamp_frequency,
    [17] = queue_get_clock_calibration,
    [18] = queue_get_desc,
};

/* ID3D12GraphicsCommandList v1 has 60 slots. The compatibility object keeps
 * all methods callable; only lifecycle methods are implemented until command
 * recording has a real Vulkan backend. */
static void *g_command_list_vtable[60] = {
    [0] = command_list_query_interface,
    [1] = command_list_add_ref,
    [2] = command_list_release,
    [3] = command_list_notimpl,
    [4] = command_list_notimpl,
    [5] = command_list_notimpl,
    [6] = command_list_notimpl,
    [7] = command_list_get_device,
    [8] = command_list_get_type,
    [9] = command_list_close,
    [10] = command_list_reset,
    [11] = command_list_draw_instanced,
    [12] = command_list_draw_indexed_instanced,
    [13] = command_list_unsupported_13,
    [14] = command_list_dispatch,
    [15] = command_list_copy_buffer_region,
    [16] = command_list_copy_texture_region,
    [17] = command_list_copy_resource,
    [18] = command_list_unsupported_18,
    [19] = command_list_unsupported_19,
    [20] = command_list_unsupported_20,
    [21] = command_list_unsupported_21,
    [22] = command_list_unsupported_22,
    [23] = command_list_set_blend_factor,
    [24] = command_list_unsupported_24,
    [25] = command_list_set_pipeline_state,
    [26] = command_list_resource_barrier,
    [27] = command_list_unsupported_27,
    [28] = command_list_set_descriptor_heaps,
    [29] = command_list_set_compute_root_signature,
    [30] = command_list_set_graphics_root_signature,
    [31] = command_list_set_compute_root_descriptor_table,
    [32] = command_list_set_graphics_root_descriptor_table,
    [33] = command_list_unsupported_33,
    [34] = command_list_unsupported_34,
    [35] = command_list_set_compute_root_constants,
    [36] = command_list_unsupported_36,
    [37] = command_list_set_compute_root_cbv,
    [38] = command_list_set_graphics_root_descriptor_table,
    [39] = command_list_set_compute_root_srv,
    [40] = command_list_set_graphics_root_descriptor_table,
    [41] = command_list_set_compute_root_uav,
    [42] = command_list_set_graphics_root_descriptor_table,
    [43] = command_list_set_index_buffer,
    [44] = command_list_set_vertex_buffers,
    [45] = command_list_unsupported_45,
    [46] = command_list_unsupported_46,
    [47] = command_list_set_render_targets,
    [48] = command_list_clear_render_target_view,
    [49] = command_list_unsupported_49,
    [50] = command_list_unsupported_50,
    [51] = command_list_unsupported_51,
    [52] = command_list_unsupported_52,
    [53] = command_list_end_query,
    [54] = command_list_resolve_query_data,
    [55] = command_list_unsupported_55,
    [56] = command_list_unsupported_56,
    [57] = command_list_unsupported_57,
    [58] = command_list_unsupported_58,
    [59] = command_list_unsupported_59,
};

static void *g_fence_vtable[11] = {
    [0] = fence_query_interface,
    [1] = fence_add_ref,
    [2] = fence_release,
    [3] = fence_notimpl,
    [4] = fence_notimpl,
    [5] = fence_notimpl,
    [6] = fence_notimpl,
    [7] = fence_get_device,
    [8] = fence_get_completed_value,
    [9] = fence_set_event_on_completion,
    [10] = fence_signal,
};

static void *g_descriptor_heap_vtable[12] = {
    [0] = descriptor_heap_query_interface,
    [1] = descriptor_heap_add_ref,
    [2] = descriptor_heap_release,
    [3] = descriptor_heap_notimpl,
    [4] = descriptor_heap_notimpl,
    [5] = descriptor_heap_notimpl,
    [6] = descriptor_heap_notimpl,
    [7] = descriptor_heap_get_device,
    [8] = descriptor_heap_get_desc,
    [9] = descriptor_heap_get_cpu_start,
    [10] = descriptor_heap_get_gpu_start,
    [11] = descriptor_heap_notimpl,
};

/* ID3D12QueryHeap adds no methods beyond ID3D12Pageable. */
static void *g_query_heap_vtable[8] = {
    [0] = query_heap_query_interface,
    [1] = query_heap_add_ref,
    [2] = query_heap_release,
    [3] = query_heap_notimpl,
    [4] = query_heap_notimpl,
    [5] = query_heap_notimpl,
    [6] = query_heap_notimpl,
    [7] = query_heap_get_device,
};

/* ID3D12CommandSignature adds no methods beyond ID3D12Pageable. */
static void *g_command_signature_vtable[8] = {
    [0] = command_signature_query_interface,
    [1] = command_signature_add_ref,
    [2] = command_signature_release,
    [3] = command_signature_notimpl,
    [4] = command_signature_notimpl,
    [5] = command_signature_notimpl,
    [6] = command_signature_notimpl,
    [7] = command_signature_get_device,
};

/* ID3D12RootSignature adds no methods beyond ID3D12DeviceChild. */
static void *g_root_signature_vtable[8] = {
    [0] = root_signature_query_interface,
    [1] = root_signature_add_ref,
    [2] = root_signature_release,
    [3] = root_signature_get_private_data,
    [4] = root_signature_set_private_data,
    [5] = root_signature_set_private_data_interface,
    [6] = root_signature_set_name,
    [7] = root_signature_get_device,
};

static void *g_pipeline_state_vtable[9] = {
    [0] = pipeline_state_query_interface,
    [1] = pipeline_state_add_ref,
    [2] = pipeline_state_release,
    [3] = pipeline_state_notimpl,
    [4] = pipeline_state_notimpl,
    [5] = pipeline_state_notimpl,
    [6] = pipeline_state_notimpl,
    [7] = pipeline_state_get_device,
    [8] = pipeline_state_get_cached_blob,
};

static void *g_blob_vtable[5] = {
    [0] = blob_query_interface,
    [1] = blob_add_ref,
    [2] = blob_release,
    [3] = blob_get_buffer_pointer,
    [4] = blob_get_buffer_size,
};

/* ID3D12Resource v1 has 15 slots through WriteToSubresource. */
static void *g_resource_vtable[15] = {
    [0] = resource_query_interface,
    [1] = resource_add_ref,
    [2] = resource_release,
    [3] = resource_notimpl,
    [4] = resource_notimpl,
    [5] = resource_notimpl,
    [6] = resource_notimpl,
    [7] = resource_get_device,
    [8] = resource_map,
    [9] = resource_unmap,
    [10] = resource_get_desc,
    [11] = resource_get_gpu_virtual_address,
    [12] = resource_notimpl,
    [13] = resource_notimpl,
    [14] = resource_notimpl,
};

/* ID3D12Heap adds GetDesc to the ID3D12Pageable surface. */
static void *g_heap_vtable[9] = {
    [0] = heap_query_interface,
    [1] = heap_add_ref,
    [2] = heap_release,
    [3] = heap_notimpl,
    [4] = heap_notimpl,
    [5] = heap_notimpl,
    [6] = heap_notimpl,
    [7] = heap_get_device,
    [8] = heap_get_desc,
};

static int iid_equal(const uint8_t *left, const uint8_t *right)
{
    return left && memcmp(left, right, 16) == 0;
}

static uint64_t __attribute__((ms_abi))
device_query_interface(BeerD3D12Device *self, const uint8_t *riid, void **output)
{
    if (!output) return BEER_E_INVALIDARG;
    *output = NULL;
    if (d3d12_diagnostics_enabled() && riid) {
        D3D12_TRACE("[D3D12] Device::QueryInterface "
                    "%02x%02x%02x%02x-%02x%02x-%02x%02x-"
                    "%02x%02x-%02x%02x%02x%02x%02x%02x\n",
                    riid[3], riid[2], riid[1], riid[0], riid[5], riid[4],
                    riid[7], riid[6], riid[8], riid[9], riid[10], riid[11],
                    riid[12], riid[13], riid[14], riid[15]);
    }
    if (!self || (!iid_equal(riid, k_iid_iunknown) &&
                  !iid_equal(riid, k_iid_d3d12_device) &&
                  !iid_equal(riid, k_iid_d3d12_device1) &&
                  !iid_equal(riid, k_iid_d3d12_device2) &&
                  !iid_equal(riid, k_iid_d3d12_device_downlevel))) {
        D3D12_TRACE("[D3D12] Device::QueryInterface -> E_NOINTERFACE\n");
        return BEER_E_NOINTERFACE;
    }
    /* Device1/Device2 and DeviceDownlevel inherit the complete base-device
     * prefix. The process-wide object exposes the 48-slot Device2 vtable. */
    device_add_ref(self);
    *output = self;
    D3D12_TRACE("[D3D12] Device::QueryInterface -> %p\n", (void *)self);
    return BEER_S_OK;
}

static uint32_t __attribute__((ms_abi)) device_add_ref(BeerD3D12Device *self)
{
    return self ? __atomic_add_fetch(&self->references, 1, __ATOMIC_RELAXED) : 0;
}

static uint32_t __attribute__((ms_abi)) device_release(BeerD3D12Device *self)
{
    if (!self) return 0;
    uint32_t current = __atomic_load_n(&self->references, __ATOMIC_RELAXED);
    while (current > 1 &&
           !__atomic_compare_exchange_n(&self->references, &current,
                                        current - 1, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    return current > 1 ? current - 1 : 1;
}

static uint64_t __attribute__((ms_abi))
device_get_private_data(BeerD3D12Device *self, const uint8_t *guid,
                        uint32_t *size, void *data)
{
    (void)data;
    if (!self || !guid || !size) return BEER_E_INVALIDARG;
    /* No private-data entries are stored on the process-wide compatibility
     * device yet. Report an absent key truthfully and deterministically. */
    *size = 0;
    return BEER_DXGI_ERROR_NOT_FOUND;
}

static uint32_t __attribute__((ms_abi)) device_get_node_count(BeerD3D12Device *self)
{
    (void)self;
    return 1;
}

static uint64_t __attribute__((ms_abi))
device_check_feature_support(BeerD3D12Device *self, uint32_t feature,
                             void *data, uint32_t data_size)
{
    (void)self;
    if (!data || data_size == 0) return BEER_E_INVALIDARG;
    D3D12_TRACE("[D3D12] CheckFeatureSupport feature=%u size=%u\n",
                feature, data_size);

    /* D3D12_FEATURE_D3D12_OPTIONS: advertise the conservative baseline only.
     * Returning a zeroed successful structure is truthful for unsupported
     * optional tiers and lets the guest select fallback code paths. */
    if (feature == 0 && data_size >= 60) {
        memset(data, 0, data_size);
        return BEER_S_OK;
    }

    /* D3D12_FEATURE_FEATURE_LEVELS: preserve the requested list and report
     * the baseline 11_0 level that this compatibility device accepts. */
    if (feature == 1 && data_size >= 16) {
        uint32_t *fields = data;
        fields[3] = 0xb000;
        return BEER_S_OK;
    }

    /* Common optional capability structures are safe to report as all-zero
     * support rather than leaving caller output untouched. */
    switch (feature) {
    case 7: case 8: case 12: case 18: case 19:
    case 21: case 23: case 27: case 30: case 32:
        memset(data, 0, data_size);
        return BEER_S_OK;
    default:
        fprintf(stderr, "[D3D12] CheckFeatureSupport feature=%u size=%u -> E_NOTIMPL\n",
                feature, data_size);
        return BEER_E_NOTIMPL;
    }
}

static uint32_t __attribute__((ms_abi))
device_get_descriptor_handle_increment_size(BeerD3D12Device *self,
                                            uint32_t heap_type)
{
    (void)self;
    /* Stable, non-overlapping descriptor strides. No descriptor heap is
     * exposed yet, so these values are consumed only for capability sizing. */
    return heap_type == 1 ? 32 : heap_type == 0 ? 32 : 16;
}

static void __attribute__((ms_abi))
device_create_constant_buffer_view(BeerD3D12Device *self,
                                   const void *description,
                                   uint64_t destination)
{
    (void)self;
    if (!destination) return;
    /* Beer descriptor handles are addresses in descriptor-heap storage. Keep
     * a bounded opaque copy of the guest descriptor so later command recording
     * can resolve it without claiming a GPU binding implementation yet. */
    memset((void *)(uintptr_t)destination, 0, 32);
    if (description) memcpy((void *)(uintptr_t)destination, description, 16);
}

static void __attribute__((ms_abi))
device_create_shader_resource_view(BeerD3D12Device *self, void *resource,
                                   const void *description,
                                   uint64_t destination)
{
    (void)self;
    if (!destination) return;
    memset((void *)(uintptr_t)destination, 0, 32);
    memcpy((void *)(uintptr_t)destination, &resource, sizeof(resource));
    if (description)
        memcpy((uint8_t *)(uintptr_t)destination + 8, description, 24);
}

static void __attribute__((ms_abi))
device_create_unordered_access_view(BeerD3D12Device *self, void *resource,
                                    void *counter_resource,
                                    const void *description,
                                    uint64_t destination)
{
    (void)self;
    if (!destination) return;
    memset((void *)(uintptr_t)destination, 0, 32);
    memcpy((void *)(uintptr_t)destination, &resource, sizeof(resource));
    memcpy((uint8_t *)(uintptr_t)destination + 8, &counter_resource,
           sizeof(counter_resource));
    if (description)
        memcpy((uint8_t *)(uintptr_t)destination + 16, description, 16);
}

static void write_resource_descriptor(void *resource, const void *description,
                                      uint64_t destination)
{
    if (!destination) return;
    memset((void *)(uintptr_t)destination, 0, 32);
    memcpy((void *)(uintptr_t)destination, &resource, sizeof(resource));
    if (description)
        memcpy((uint8_t *)(uintptr_t)destination + 8, description, 24);
}

static void __attribute__((ms_abi))
device_create_render_target_view(BeerD3D12Device *self, void *resource,
                                 const void *description,
                                 uint64_t destination)
{
    (void)self;
    write_resource_descriptor(resource, description, destination);
}

static void __attribute__((ms_abi))
device_create_depth_stencil_view(BeerD3D12Device *self, void *resource,
                                 const void *description,
                                 uint64_t destination)
{
    (void)self;
    write_resource_descriptor(resource, description, destination);
}

static void __attribute__((ms_abi))
device_create_sampler(BeerD3D12Device *self, const void *description,
                      uint64_t destination)
{
    (void)self;
    if (!destination) return;
    memset((void *)(uintptr_t)destination, 0, 16);
    if (description)
        memcpy((void *)(uintptr_t)destination, description, 16);
}

static size_t descriptor_stride(uint32_t heap_type)
{
    return heap_type == 0 || heap_type == 1 ? 32u : 16u;
}

static void __attribute__((ms_abi))
device_copy_descriptors_simple(BeerD3D12Device *self, uint32_t count,
                               uint64_t destination, uint64_t source,
                               uint32_t heap_type)
{
    (void)self;
    if (!count || !destination || !source) return;
    size_t stride = descriptor_stride(heap_type);
    if ((size_t)count > SIZE_MAX / stride) return;
    memmove((void *)(uintptr_t)destination, (const void *)(uintptr_t)source,
            (size_t)count * stride);
}

static void __attribute__((ms_abi))
device_copy_descriptors(BeerD3D12Device *self,
                        uint32_t destination_range_count,
                        const uint64_t *destination_starts,
                        const uint32_t *destination_sizes,
                        uint32_t source_range_count,
                        const uint64_t *source_starts,
                        const uint32_t *source_sizes,
                        uint32_t heap_type)
{
    (void)self;
    if (!destination_range_count || !source_range_count ||
        !destination_starts || !source_starts)
        return;

    size_t stride = descriptor_stride(heap_type);
    uint32_t destination_range = 0, source_range = 0;
    uint32_t destination_offset = 0, source_offset = 0;
    while (destination_range < destination_range_count &&
           source_range < source_range_count) {
        uint32_t destination_count = destination_sizes
            ? destination_sizes[destination_range] : 1;
        uint32_t source_count = source_sizes ? source_sizes[source_range] : 1;
        if (destination_offset >= destination_count) {
            ++destination_range; destination_offset = 0; continue;
        }
        if (source_offset >= source_count) {
            ++source_range; source_offset = 0; continue;
        }
        uint32_t copy_count = destination_count - destination_offset;
        if (copy_count > source_count - source_offset)
            copy_count = source_count - source_offset;
        if ((size_t)copy_count > SIZE_MAX / stride) return;
        uint64_t destination = destination_starts[destination_range] +
            (uint64_t)destination_offset * stride;
        uint64_t source = source_starts[source_range] +
            (uint64_t)source_offset * stride;
        if (!destination || !source) return;
        memmove((void *)(uintptr_t)destination, (const void *)(uintptr_t)source,
                (size_t)copy_count * stride);
        destination_offset += copy_count;
        source_offset += copy_count;
    }
}

static uint32_t d3d12_format_bytes_per_pixel(uint32_t format)
{
    switch (format) {
    case 2: case 3: case 4: case 5: case 6: case 7: case 8:
        return 16;
    case 10: case 11: case 12: case 13: case 14: case 15: case 16:
        return 8;
    case 41: case 42: case 43: case 44: case 45: case 46: case 47:
    case 48: case 49: case 50: case 51: case 52: case 53: case 54:
    case 55: case 56: case 57: case 58: case 59: case 60: case 61:
        return 4;
    case 62: case 63: case 64: case 65: case 66: case 67: case 68:
    case 69: case 70:
        return 2;
    default:
        return 4;
    }
}

static void __attribute__((ms_abi))
device_get_copyable_footprints(BeerD3D12Device *self,
                               const BeerD3D12ResourceDesc *description,
                               uint32_t first_subresource,
                               uint32_t subresource_count,
                               uint64_t base_offset, void *layouts,
                               uint32_t *row_counts, uint64_t *row_sizes,
                               uint64_t *total_bytes)
{
    (void)self;
    if (total_bytes) *total_bytes = 0;
    if (!description || subresource_count == 0) return;
    uint64_t cursor = base_offset;
    uint32_t bytes_per_pixel = d3d12_format_bytes_per_pixel(description->format);
    for (uint32_t index = 0; index < subresource_count; ++index) {
        uint32_t subresource = first_subresource + index;
        uint32_t mip = description->mip_levels
                           ? subresource % description->mip_levels : 0;
        uint64_t width = description->width >> mip;
        uint32_t height = description->height >> mip;
        if (!width) width = 1;
        if (!height) height = 1;
        uint64_t row_size = width * bytes_per_pixel;
        uint32_t row_pitch = (uint32_t)align_up_u64(row_size, 256);
        cursor = align_up_u64(cursor, 512);
        if (layouts) {
            /* D3D12_PLACED_SUBRESOURCE_FOOTPRINT is 32 bytes on Win64:
             * Offset followed by {Format, Width, Height, Depth, RowPitch}. */
            uint8_t *layout = (uint8_t *)layouts + index * 32;
            uint32_t format = description->format;
            uint32_t footprint_width = width > UINT32_MAX
                                           ? UINT32_MAX : (uint32_t)width;
            uint32_t depth = description->dimension == 4
                                 ? description->depth_or_array_size >> mip : 1;
            if (!depth) depth = 1;
            memset(layout, 0, 32);
            memcpy(layout, &cursor, sizeof(cursor));
            memcpy(layout + 8, &format, sizeof(format));
            memcpy(layout + 12, &footprint_width, sizeof(footprint_width));
            memcpy(layout + 16, &height, sizeof(height));
            memcpy(layout + 20, &depth, sizeof(depth));
            memcpy(layout + 24, &row_pitch, sizeof(row_pitch));
        }
        if (row_counts) row_counts[index] = height;
        if (row_sizes) row_sizes[index] = row_size;
        cursor += (uint64_t)row_pitch * height;
    }
    if (total_bytes) *total_bytes = cursor - base_offset;
}

static uint64_t __attribute__((ms_abi))
device_create_command_queue(BeerD3D12Device *self, const uint32_t *description,
                            const uint8_t *riid, void **queue)
{
    if (!self || !description || !riid || !queue) return BEER_E_INVALIDARG;
    *queue = NULL;

    BeerD3D12CommandQueue *object = calloc(1, sizeof(*object));
    if (!object) return BEER_E_OUTOFMEMORY;
    object->vtable = g_queue_vtable;
    object->references = 1;
    object->device = self;
    object->type = description[0];
    object->priority = description[1];
    object->flags = description[2];
    object->node_mask = description[3];
    object->downlevel_vtable = g_downlevel_queue_vtable;
    device_add_ref(self);
    *queue = object;
    fprintf(stderr, "[D3D12] CreateCommandQueue type=%u priority=%u flags=0x%x -> %p\n",
            object->type, object->priority, object->flags, (void *)object);
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
device_create_descriptor_heap(BeerD3D12Device *self,
                              const uint32_t *description,
                              const uint8_t *riid, void **heap)
{
    if (!self || !description || !riid || !heap) return BEER_E_INVALIDARG;
    *heap = NULL;
    if (!iid_equal(riid, k_iid_d3d12_descriptor_heap) &&
        !iid_equal(riid, k_iid_iunknown))
        return BEER_E_NOINTERFACE;

    uint32_t increment = device_get_descriptor_handle_increment_size(
        self, description[0]);
    size_t bytes = (size_t)description[1] * increment;
    if (bytes == 0) bytes = increment;
    BeerD3D12DescriptorHeap *object = calloc(1, sizeof(*object));
    if (!object) return BEER_E_OUTOFMEMORY;
    void *storage = calloc(1, bytes);
    if (!storage) {
        free(object);
        return BEER_E_OUTOFMEMORY;
    }

    object->vtable = g_descriptor_heap_vtable;
    object->references = 1;
    object->device = self;
    object->storage = storage;
    object->storage_size = bytes;
    memcpy(object->description, description, sizeof(object->description));
    object->cpu_start = (uint64_t)(uintptr_t)storage;
    /* GPU handles are opaque tokens to the guest until descriptor binding is
     * backed by a D3D12 command renderer. Keep them stable and non-null. */
    object->gpu_start = object->cpu_start;
    device_add_ref(self);
    *heap = object;
    fprintf(stderr, "[D3D12] CreateDescriptorHeap type=%u count=%u flags=0x%x -> %p\n",
            description[0], description[1], description[2], (void *)object);
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
device_create_command_allocator(BeerD3D12Device *self, uint32_t type,
                                const uint8_t *riid, void **allocator)
{
    if (!self || !allocator || !riid) return BEER_E_INVALIDARG;
    *allocator = NULL;
    BeerD3D12CommandAllocator *object = calloc(1, sizeof(*object));
    if (!object) return BEER_E_OUTOFMEMORY;
    object->vtable = g_allocator_vtable;
    object->references = 1;
    object->device = self;
    object->type = type;
    device_add_ref(self);
    *allocator = object;
    fprintf(stderr, "[D3D12] CreateCommandAllocator type=%u -> %p\n",
            type, (void *)object);
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
device_create_command_list(BeerD3D12Device *self, uint32_t node_mask,
                           uint32_t type, BeerD3D12CommandAllocator *allocator,
                           void *initial_state, const uint8_t *riid,
                           void **command_list)
{
    (void)node_mask;
    if (!self || !allocator || !riid || !command_list)
        return BEER_E_INVALIDARG;
    *command_list = NULL;
    if (!iid_equal(riid, k_iid_d3d12_graphics_command_list) &&
        !iid_equal(riid, k_iid_iunknown))
        return BEER_E_NOINTERFACE;
    if (allocator->device != self || allocator->type != type)
        return BEER_E_INVALIDARG;

    BeerD3D12GraphicsCommandList *object = calloc(1, sizeof(*object));
    if (!object) return BEER_E_OUTOFMEMORY;
    object->vtable = g_command_list_vtable;
    object->references = 1;
    object->device = self;
    object->allocator = allocator;
    object->type = type;
    object->closed = 0;
    object->dispatch_tail = &object->dispatches;
    if (initial_state) {
        BeerD3D12PipelineState *pipeline_state = initial_state;
        if (pipeline_state->device != self) {
            free(object);
            return BEER_E_INVALIDARG;
        }
        object->pipeline_state = pipeline_state;
        pipeline_state_add_ref(pipeline_state);
    }
    device_add_ref(self);
    allocator_add_ref(allocator);
    *command_list = object;
    fprintf(stderr, "[D3D12] CreateCommandList type=%u -> %p\n",
            type, (void *)object);
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
device_create_fence(BeerD3D12Device *self, uint64_t initial_value,
                    uint32_t flags, const uint8_t *riid, void **fence)
{
    (void)flags;
    if (!self || !riid || !fence) return BEER_E_INVALIDARG;
    *fence = NULL;
    if (!iid_equal(riid, k_iid_d3d12_fence) &&
        !iid_equal(riid, k_iid_iunknown))
        return BEER_E_NOINTERFACE;

    BeerD3D12Fence *object = calloc(1, sizeof(*object));
    if (!object) return BEER_E_OUTOFMEMORY;
    object->vtable = g_fence_vtable;
    object->references = 1;
    object->device = self;
    __atomic_store_n(&object->completed_value, initial_value, __ATOMIC_RELEASE);
    atomic_flag_clear(&object->waiters_lock);
    device_add_ref(self);
    *fence = object;
    fprintf(stderr, "[D3D12] CreateFence initial=%llu -> %p\n",
            (unsigned long long)initial_value, (void *)object);
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
device_create_query_heap(BeerD3D12Device *self, const uint32_t *description,
                         const uint8_t *riid, void **query_heap)
{
    if (!self || !description || !riid || !query_heap)
        return BEER_E_INVALIDARG;
    *query_heap = NULL;
    if (!iid_equal(riid, k_iid_d3d12_query_heap) &&
        !iid_equal(riid, k_iid_iunknown))
        return BEER_E_NOINTERFACE;
    if (description[1] == 0) return BEER_E_INVALIDARG;

    BeerD3D12QueryHeap *object = calloc(1, sizeof(*object));
    if (!object) return BEER_E_OUTOFMEMORY;
    object->vtable = g_query_heap_vtable;
    object->references = 1;
    object->device = self;
    memcpy(object->description, description, sizeof(object->description));
    object->results = calloc(description[1], sizeof(*object->results));
    if (!object->results) {
        free(object);
        return BEER_E_OUTOFMEMORY;
    }
    device_add_ref(self);
    *query_heap = object;
    fprintf(stderr, "[D3D12] CreateQueryHeap type=%u count=%u node=%u -> %p\n",
            description[0], description[1], description[2], (void *)object);
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
device_create_root_signature(BeerD3D12Device *self, uint32_t node_mask,
                             const void *blob, size_t blob_size,
                             const uint8_t *riid, void **root_signature)
{
    if (!self || !blob || blob_size == 0 || !riid || !root_signature)
        return BEER_E_INVALIDARG;
    *root_signature = NULL;
    if (!iid_equal(riid, k_iid_d3d12_root_signature) &&
        !iid_equal(riid, k_iid_iunknown))
        return BEER_E_NOINTERFACE;

    BeerD3D12RootSignature *object = calloc(1, sizeof(*object));
    if (!object) return BEER_E_OUTOFMEMORY;
    object->blob = malloc(blob_size);
    if (!object->blob) {
        free(object);
        return BEER_E_OUTOFMEMORY;
    }
    memcpy(object->blob, blob, blob_size);
    object->blob_size = blob_size;
    object->vtable = g_root_signature_vtable;
    object->references = 1;
    object->device = self;
    object->node_mask = node_mask;
    device_add_ref(self);
    *root_signature = object;
    D3D12_TRACE("[D3D12] CreateRootSignature node=%u bytes=%zu -> %p\n",
                node_mask, blob_size, (void *)object);
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
device_create_command_signature(BeerD3D12Device *self,
                                const void *description_data,
                                void *root_signature,
                                const uint8_t *riid,
                                void **command_signature)
{
    (void)root_signature;
    if (!self || !description_data || !riid || !command_signature)
        return BEER_E_INVALIDARG;
    *command_signature = NULL;
    if (!iid_equal(riid, k_iid_d3d12_command_signature) &&
        !iid_equal(riid, k_iid_iunknown))
        return BEER_E_NOINTERFACE;

    /* D3D12_COMMAND_SIGNATURE_DESC is 24 bytes on Win64. */
    const uint8_t *description = description_data;
    uint32_t byte_stride;
    uint32_t argument_count;
    const void *arguments;
    uint32_t node_mask;
    memcpy(&byte_stride, description, sizeof(byte_stride));
    memcpy(&argument_count, description + 4, sizeof(argument_count));
    memcpy(&arguments, description + 8, sizeof(arguments));
    memcpy(&node_mask, description + 16, sizeof(node_mask));
    if (byte_stride == 0 || argument_count == 0 || !arguments ||
        argument_count > 1024)
        return BEER_E_INVALIDARG;

    /* D3D12_INDIRECT_ARGUMENT_DESC is eight bytes for every current variant. */
    if ((size_t)argument_count > SIZE_MAX / 8)
        return BEER_E_OUTOFMEMORY;
    size_t arguments_size = (size_t)argument_count * 8;
    BeerD3D12CommandSignature *object = calloc(1, sizeof(*object));
    if (!object) return BEER_E_OUTOFMEMORY;
    object->arguments = malloc(arguments_size);
    if (!object->arguments) {
        free(object);
        return BEER_E_OUTOFMEMORY;
    }
    memcpy(object->arguments, arguments, arguments_size);
    object->arguments_size = arguments_size;
    object->vtable = g_command_signature_vtable;
    object->references = 1;
    object->device = self;
    object->byte_stride = byte_stride;
    object->argument_count = argument_count;
    object->node_mask = node_mask;
    device_add_ref(self);
    *command_signature = object;
    fprintf(stderr,
            "[D3D12] CreateCommandSignature stride=%u arguments=%u node=%u -> %p\n",
            byte_stride, argument_count, node_mask, (void *)object);
    return BEER_S_OK;
}

static uint64_t align_up_u64(uint64_t value, uint64_t alignment)
{
    return alignment ? (value + alignment - 1) & ~(alignment - 1) : value;
}

static int resource_allocation_info(const BeerD3D12ResourceDesc *descriptions,
                                    uint32_t count,
                                    BeerD3D12ResourceAllocationInfo *result)
{
    if (!result) return 0;
    result->size_in_bytes = 0;
    result->alignment = UINT64_C(65536);
    if (!descriptions || count == 0) return 1;

    for (uint32_t index = 0; index < count; ++index) {
        const BeerD3D12ResourceDesc *description = &descriptions[index];
        uint64_t alignment = description->alignment ? description->alignment
                                                    : UINT64_C(65536);
        if ((alignment & (alignment - 1)) != 0) alignment = UINT64_C(65536);
        if (alignment > result->alignment) result->alignment = alignment;
        result->size_in_bytes = align_up_u64(result->size_in_bytes, alignment);

        uint64_t resource_size;
        if (description->dimension == 1) {
            resource_size = description->width;
        } else {
            uint64_t width = description->width ? description->width : 1;
            uint64_t height = description->height ? description->height : 1;
            uint64_t layers = description->depth_or_array_size
                                  ? description->depth_or_array_size : 1;
            uint64_t samples = description->sample_count
                                   ? description->sample_count : 1;
            if (width > UINT64_MAX / height ||
                width * height > UINT64_MAX / layers ||
                width * height * layers > UINT64_MAX / samples ||
                width * height * layers * samples > UINT64_MAX / 4) {
                result->size_in_bytes = UINT64_MAX;
                return 0;
            }
            resource_size = width * height * layers * samples * 4;
        }
        result->size_in_bytes += align_up_u64(resource_size, alignment);
    }
    result->size_in_bytes = align_up_u64(result->size_in_bytes,
                                         result->alignment);
    return result->size_in_bytes != UINT64_MAX;
}

static BeerD3D12ResourceAllocationInfo *__attribute__((ms_abi))
device_get_resource_allocation_info(BeerD3D12Device *self,
                                    BeerD3D12ResourceAllocationInfo *result,
                                    uint32_t visible_mask, uint32_t count,
                                    const BeerD3D12ResourceDesc *descriptions)
{
    (void)self;
    (void)visible_mask;
    if (!result) return NULL;
    resource_allocation_info(descriptions, count, result);
    fprintf(stderr,
            "[D3D12] GetResourceAllocationInfo count=%u -> size=%llu align=%llu\n",
            count, (unsigned long long)result->size_in_bytes,
            (unsigned long long)result->alignment);
    return result;
}

static uint64_t __attribute__((ms_abi))
device_create_committed_resource(BeerD3D12Device *self,
                                 const uint32_t *heap_properties,
                                 uint32_t heap_flags,
                                 const BeerD3D12ResourceDesc *description,
                                 uint32_t initial_state,
                                 const void *optimized_clear_value,
                                 const uint8_t *riid, void **resource)
{
    (void)initial_state;
    (void)optimized_clear_value;
    if (!self || !heap_properties || !description || !riid || !resource)
        return BEER_E_INVALIDARG;
    *resource = NULL;
    if (!iid_equal(riid, k_iid_d3d12_resource) &&
        !iid_equal(riid, k_iid_iunknown))
        return BEER_E_NOINTERFACE;
    if (description->dimension == 0 || description->width == 0)
        return BEER_E_INVALIDARG;

    BeerD3D12ResourceAllocationInfo allocation;
    if (!resource_allocation_info(description, 1, &allocation) ||
        allocation.size_in_bytes == 0 || allocation.size_in_bytes > SIZE_MAX)
        return BEER_E_OUTOFMEMORY;

    BeerD3D12Resource *object = calloc(1, sizeof(*object));
    if (!object) return BEER_E_OUTOFMEMORY;
    object->storage = calloc(1, (size_t)allocation.size_in_bytes);
    if (!object->storage) {
        free(object);
        return BEER_E_OUTOFMEMORY;
    }
    object->vtable = g_resource_vtable;
    object->references = 1;
    object->device = self;
    object->description = *description;
    object->state = initial_state;
    object->storage_size = (size_t)allocation.size_in_bytes;
    object->owns_storage = 1;
    object->gpu_virtual_address = description->dimension == 1
        ? (uint64_t)(uintptr_t)object->storage : 0;
    memcpy(object->heap_properties, heap_properties,
           sizeof(object->heap_properties));
    object->heap_flags = heap_flags;
    device_add_ref(self);
    *resource = object;
    fprintf(stderr,
            "[D3D12] CreateCommittedResource dimension=%u size=%zu -> %p\n",
            description->dimension, object->storage_size, (void *)object);
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
device_create_heap(BeerD3D12Device *self, const uint64_t *description_words,
                   const uint8_t *riid, void **heap)
{
    if (!self || !description_words || !riid || !heap)
        return BEER_E_INVALIDARG;
    *heap = NULL;
    if (!iid_equal(riid, k_iid_d3d12_heap) &&
        !iid_equal(riid, k_iid_iunknown))
        return BEER_E_NOINTERFACE;

    BeerD3D12HeapDesc description;
    memcpy(&description, description_words, sizeof(description));
    if (description.size_in_bytes == 0 ||
        description.size_in_bytes > SIZE_MAX)
        return BEER_E_INVALIDARG;
    uint64_t alignment = description.alignment
                             ? description.alignment : UINT64_C(65536);
    if ((alignment & (alignment - 1)) != 0)
        return BEER_E_INVALIDARG;
    uint64_t storage_size = align_up_u64(description.size_in_bytes, alignment);
    if (storage_size > SIZE_MAX) return BEER_E_OUTOFMEMORY;

    BeerD3D12Heap *object = calloc(1, sizeof(*object));
    if (!object) return BEER_E_OUTOFMEMORY;
    object->storage = aligned_alloc((size_t)alignment, (size_t)storage_size);
    if (!object->storage) {
        free(object);
        return BEER_E_OUTOFMEMORY;
    }
    memset(object->storage, 0, (size_t)storage_size);
    object->vtable = g_heap_vtable;
    object->references = 1;
    object->device = self;
    object->description = description;
    object->description.alignment = alignment;
    device_add_ref(self);
    *heap = object;
    fprintf(stderr, "[D3D12] CreateHeap size=%llu align=%llu flags=0x%x -> %p\n",
            (unsigned long long)description.size_in_bytes,
            (unsigned long long)alignment, description.flags, (void *)object);
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
device_create_placed_resource(BeerD3D12Device *self, BeerD3D12Heap *heap,
                              uint64_t heap_offset,
                              const BeerD3D12ResourceDesc *description,
                              uint32_t initial_state,
                              const void *optimized_clear_value,
                              const uint8_t *riid, void **resource)
{
    (void)initial_state;
    (void)optimized_clear_value;
    if (!self || !heap || heap->device != self || !description ||
        !riid || !resource)
        return BEER_E_INVALIDARG;
    *resource = NULL;
    if (!iid_equal(riid, k_iid_d3d12_resource) &&
        !iid_equal(riid, k_iid_iunknown))
        return BEER_E_NOINTERFACE;

    BeerD3D12ResourceAllocationInfo allocation;
    if (!resource_allocation_info(description, 1, &allocation) ||
        allocation.size_in_bytes == 0)
        return BEER_E_INVALIDARG;
    uint64_t required_alignment = description->alignment
                                      ? description->alignment
                                      : allocation.alignment;
    if ((heap_offset & (required_alignment - 1)) != 0 ||
        heap_offset > heap->description.size_in_bytes ||
        allocation.size_in_bytes > heap->description.size_in_bytes - heap_offset)
        return BEER_E_INVALIDARG;

    BeerD3D12Resource *object = calloc(1, sizeof(*object));
    if (!object) return BEER_E_OUTOFMEMORY;
    object->vtable = g_resource_vtable;
    object->references = 1;
    object->device = self;
    object->heap = heap;
    object->description = *description;
    object->state = initial_state;
    object->storage = heap->storage + heap_offset;
    object->storage_size = (size_t)allocation.size_in_bytes;
    object->owns_storage = 0;
    object->gpu_virtual_address = description->dimension == 1
        ? (uint64_t)(uintptr_t)object->storage : 0;
    memcpy(object->heap_properties, heap->description.properties,
           sizeof(object->heap_properties));
    object->heap_flags = heap->description.flags;
    device_add_ref(self);
    heap_add_ref(heap);
    *resource = object;
    fprintf(stderr,
            "[D3D12] CreatePlacedResource offset=%llu dimension=%u size=%zu -> %p\n",
            (unsigned long long)heap_offset, description->dimension,
            object->storage_size, (void *)object);
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
resource_query_interface(BeerD3D12Resource *self,
                         const uint8_t *riid, void **output)
{
    if (!output) return BEER_E_INVALIDARG;
    *output = NULL;
    if (!self || (!iid_equal(riid, k_iid_iunknown) &&
                  !iid_equal(riid, k_iid_d3d12_resource)))
        return BEER_E_NOINTERFACE;
    resource_add_ref(self);
    *output = self;
    return BEER_S_OK;
}

static uint32_t __attribute__((ms_abi)) resource_add_ref(BeerD3D12Resource *self)
{
    return self ? __atomic_add_fetch(&self->references, 1, __ATOMIC_RELAXED) : 0;
}

static uint32_t __attribute__((ms_abi)) resource_release(BeerD3D12Resource *self)
{
    if (!self) return 0;
    uint32_t remaining = __atomic_sub_fetch(&self->references, 1,
                                             __ATOMIC_ACQ_REL);
    if (remaining == 0) {
        if (self->owns_storage) free(self->storage);
        if (self->heap) heap_release(self->heap);
        device_release(self->device);
        free(self);
    }
    return remaining;
}

static uint64_t __attribute__((ms_abi)) resource_notimpl(BeerD3D12Resource *self)
{
    (void)self;
    return BEER_E_NOTIMPL;
}

static uint64_t __attribute__((ms_abi))
resource_get_device(BeerD3D12Resource *self, const uint8_t *riid, void **device)
{
    if (!self || !self->device) {
        if (device) *device = NULL;
        return BEER_E_NOINTERFACE;
    }
    return device_query_interface(self->device, riid, device);
}

static uint64_t __attribute__((ms_abi))
resource_map(BeerD3D12Resource *self, uint32_t subresource,
             const void *read_range, void **data)
{
    (void)read_range;
    if (!data) return BEER_E_INVALIDARG;
    *data = NULL;
    if (!self || subresource != 0 || self->description.dimension != 1)
        return BEER_E_INVALIDARG;
    *data = self->storage;
    return BEER_S_OK;
}

static void __attribute__((ms_abi))
resource_unmap(BeerD3D12Resource *self, uint32_t subresource,
               const void *written_range)
{
    (void)self;
    (void)subresource;
    (void)written_range;
}

static BeerD3D12ResourceDesc *__attribute__((ms_abi))
resource_get_desc(BeerD3D12Resource *self, BeerD3D12ResourceDesc *result)
{
    if (!result) return NULL;
    if (!self) {
        memset(result, 0, sizeof(*result));
        return result;
    }
    *result = self->description;
    return result;
}

static uint64_t __attribute__((ms_abi))
resource_get_gpu_virtual_address(BeerD3D12Resource *self)
{
    return self ? self->gpu_virtual_address : 0;
}

static uint64_t __attribute__((ms_abi))
heap_query_interface(BeerD3D12Heap *self, const uint8_t *riid, void **output)
{
    if (!output) return BEER_E_INVALIDARG;
    *output = NULL;
    if (!self || (!iid_equal(riid, k_iid_iunknown) &&
                  !iid_equal(riid, k_iid_d3d12_heap)))
        return BEER_E_NOINTERFACE;
    heap_add_ref(self);
    *output = self;
    return BEER_S_OK;
}

static uint32_t __attribute__((ms_abi)) heap_add_ref(BeerD3D12Heap *self)
{
    return self ? __atomic_add_fetch(&self->references, 1, __ATOMIC_RELAXED) : 0;
}

static uint32_t __attribute__((ms_abi)) heap_release(BeerD3D12Heap *self)
{
    if (!self) return 0;
    uint32_t remaining = __atomic_sub_fetch(&self->references, 1,
                                             __ATOMIC_ACQ_REL);
    if (remaining == 0) {
        free(self->storage);
        device_release(self->device);
        free(self);
    }
    return remaining;
}

static uint64_t __attribute__((ms_abi)) heap_notimpl(BeerD3D12Heap *self)
{
    (void)self;
    return BEER_E_NOTIMPL;
}

static uint64_t __attribute__((ms_abi))
heap_get_device(BeerD3D12Heap *self, const uint8_t *riid, void **device)
{
    if (!self || !self->device) {
        if (device) *device = NULL;
        return BEER_E_NOINTERFACE;
    }
    return device_query_interface(self->device, riid, device);
}

static BeerD3D12HeapDesc *__attribute__((ms_abi))
heap_get_desc(BeerD3D12Heap *self, BeerD3D12HeapDesc *description)
{
    if (!description) return NULL;
    if (!self) {
        memset(description, 0, sizeof(*description));
        return description;
    }
    *description = self->description;
    return description;
}

static uint64_t __attribute__((ms_abi))
allocator_query_interface(BeerD3D12CommandAllocator *self,
                          const uint8_t *riid, void **output)
{
    if (!output) return BEER_E_INVALIDARG;
    *output = NULL;
    if (!self || !iid_equal(riid, k_iid_iunknown)) return BEER_E_NOINTERFACE;
    allocator_add_ref(self);
    *output = self;
    return BEER_S_OK;
}

static uint32_t __attribute__((ms_abi))
allocator_add_ref(BeerD3D12CommandAllocator *self)
{
    return self ? __atomic_add_fetch(&self->references, 1, __ATOMIC_RELAXED) : 0;
}

static uint32_t __attribute__((ms_abi))
allocator_release(BeerD3D12CommandAllocator *self)
{
    if (!self) return 0;
    uint32_t remaining = __atomic_sub_fetch(&self->references, 1,
                                             __ATOMIC_ACQ_REL);
    if (remaining == 0) {
        device_release(self->device);
        free(self);
    }
    return remaining;
}

static uint64_t __attribute__((ms_abi))
allocator_get_private_data(BeerD3D12CommandAllocator *self,
                           const uint8_t *guid, uint32_t *size, void *data)
{
    (void)self; (void)guid; (void)size; (void)data;
    return BEER_E_NOTIMPL;
}

static uint64_t __attribute__((ms_abi))
allocator_set_private_data(BeerD3D12CommandAllocator *self,
                           const uint8_t *guid, uint32_t size, const void *data)
{
    (void)self; (void)guid; (void)size; (void)data;
    return BEER_E_NOTIMPL;
}

static uint64_t __attribute__((ms_abi))
allocator_set_private_data_interface(BeerD3D12CommandAllocator *self,
                                     const uint8_t *guid, void *data)
{
    (void)self; (void)guid; (void)data;
    return BEER_E_NOTIMPL;
}

static uint64_t __attribute__((ms_abi))
allocator_set_name(BeerD3D12CommandAllocator *self, const uint16_t *name)
{
    (void)self; (void)name;
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
allocator_get_device(BeerD3D12CommandAllocator *self,
                     const uint8_t *riid, void **device)
{
    if (!self || !self->device) {
        if (device) *device = NULL;
        return BEER_E_NOINTERFACE;
    }
    return device_query_interface(self->device, riid, device);
}

static uint64_t __attribute__((ms_abi))
allocator_reset(BeerD3D12CommandAllocator *self)
{
    return self ? BEER_S_OK : BEER_E_INVALIDARG;
}

static BeerD3D12CommandQueue *queue_from_downlevel(void *interface)
{
    if (!interface) return NULL;
    return (BeerD3D12CommandQueue *)((uint8_t *)interface -
        offsetof(BeerD3D12CommandQueue, downlevel_vtable));
}

static uint64_t __attribute__((ms_abi))
queue_query_interface(BeerD3D12CommandQueue *self,
                      const uint8_t *riid, void **output)
{
    if (!output) return BEER_E_INVALIDARG;
    *output = NULL;
    if (!self || !riid) return BEER_E_NOINTERFACE;
    D3D12_TRACE("[D3D12] CommandQueue::QueryInterface "
                "%02x%02x%02x%02x-%02x%02x-%02x%02x-"
                "%02x%02x-%02x%02x%02x%02x%02x%02x\n",
                riid[3], riid[2], riid[1], riid[0], riid[5], riid[4],
                riid[7], riid[6], riid[8], riid[9], riid[10], riid[11],
                riid[12], riid[13], riid[14], riid[15]);
    if (iid_equal(riid, k_iid_iunknown)) {
        queue_add_ref(self);
        *output = self;
        return BEER_S_OK;
    }
    if (iid_equal(riid, k_iid_d3d12_command_queue_downlevel)) {
        queue_add_ref(self);
        *output = &self->downlevel_vtable;
        return BEER_S_OK;
    }
    return BEER_E_NOINTERFACE;
}

static uint64_t __attribute__((ms_abi))
downlevel_query_interface(void *interface, const uint8_t *riid, void **output)
{
    return queue_query_interface(queue_from_downlevel(interface), riid, output);
}

static uint32_t __attribute__((ms_abi)) downlevel_add_ref(void *interface)
{
    return queue_add_ref(queue_from_downlevel(interface));
}

static uint32_t __attribute__((ms_abi)) downlevel_release(void *interface)
{
    return queue_release(queue_from_downlevel(interface));
}

static uint64_t __attribute__((ms_abi))
downlevel_present(void *interface, BeerD3D12GraphicsCommandList *command_list,
                  BeerD3D12Resource *source_texture, uint64_t window,
                  uint32_t flags)
{
    (void)window;
    (void)flags;
    BeerD3D12CommandQueue *queue = queue_from_downlevel(interface);
    if (!queue || !command_list || !source_texture || !g_present ||
        command_list->device != queue->device ||
        source_texture->device != queue->device ||
        !source_texture->storage)
        return BEER_E_INVALIDARG;

    const BeerD3D12ResourceDesc *description = &source_texture->description;
    if (description->dimension != 3 || description->width == 0 ||
        description->height == 0 || description->width > INT32_MAX ||
        description->height > INT32_MAX ||
        (description->format != 28 && description->format != 29 &&
         description->format != 87))
        return BEER_E_NOTIMPL;
    if (description->width > SIZE_MAX / 4 ||
        description->height > SIZE_MAX / ((size_t)description->width * 4))
        return BEER_E_INVALIDARG;
    size_t row_pitch = (size_t)description->width * 4;
    if (row_pitch * description->height > source_texture->storage_size)
        return BEER_E_INVALIDARG;

    ++source_texture->content_serial;
    if (!g_present(source_texture, source_texture->content_serial,
                   source_texture->storage, (int)description->width,
                   (int)description->height, (int)row_pitch))
        return BEER_E_NOTIMPL;
    D3D12_TRACE("[D3D12] Downlevel Present %llux%u format=%u serial=%llu\n",
                (unsigned long long)description->width, description->height,
                description->format,
                (unsigned long long)source_texture->content_serial);
    return BEER_S_OK;
}

static uint32_t __attribute__((ms_abi)) queue_add_ref(BeerD3D12CommandQueue *self)
{
    return self ? __atomic_add_fetch(&self->references, 1, __ATOMIC_RELAXED) : 0;
}

static uint32_t __attribute__((ms_abi)) queue_release(BeerD3D12CommandQueue *self)
{
    if (!self) return 0;
    uint32_t remaining = __atomic_sub_fetch(&self->references, 1,
                                             __ATOMIC_ACQ_REL);
    if (remaining == 0) {
        device_release(self->device);
        free(self);
    }
    return remaining;
}

static uint64_t __attribute__((ms_abi)) queue_notimpl(BeerD3D12CommandQueue *self)
{
    (void)self;
    return BEER_E_NOTIMPL;
}

static uint64_t __attribute__((ms_abi))
queue_get_device(BeerD3D12CommandQueue *self, const uint8_t *riid, void **device)
{
    if (!self || !self->device) {
        if (device) *device = NULL;
        return BEER_E_NOINTERFACE;
    }
    return device_query_interface(self->device, riid, device);
}

static void __attribute__((ms_abi))
queue_update_tile_mappings(BeerD3D12CommandQueue *self) { (void)self; }
static void __attribute__((ms_abi))
queue_copy_tile_mappings(BeerD3D12CommandQueue *self) { (void)self; }
static void __attribute__((ms_abi))
queue_execute_command_lists(BeerD3D12CommandQueue *self,
                            uint32_t count, void *const *lists)
{
    if (!self || (!lists && count)) return;
    uint64_t totals[BEER_D3D12_OP_COUNT] = {0};
    uint32_t valid_lists = 0;
    uint32_t open_lists = 0;
    uint32_t executed_dispatches = 0;
    uint32_t rejected_dispatches = 0;
    for (uint32_t index = 0; index < count; ++index) {
        BeerD3D12GraphicsCommandList *list = lists[index];
        if (!list || list->device != self->device || list->type != self->type)
            continue;
        ++valid_lists;
        if (!list->closed) ++open_lists;
        for (uint32_t operation = 0; operation < BEER_D3D12_OP_COUNT; ++operation)
            totals[operation] += list->operation_counts[operation];
        for (BeerD3D12RecordedDispatch *dispatch = list->dispatches;
             dispatch; dispatch = dispatch->next) {
            int executed = g_execute_compute &&
                g_execute_compute(&dispatch->state);
            if (executed) ++executed_dispatches;
            else ++rejected_dispatches;
            D3D12_TRACE("[D3D12] Dispatch groups=(%u,%u,%u) pso=%p "
                        "cs-bytes=%zu cs-hash=%016llx root=%p heaps=%u "
                        "executed=%u\n",
                        dispatch->state.group_count_x,
                        dispatch->state.group_count_y,
                        dispatch->state.group_count_z,
                        dispatch->state.pipeline_state,
                        dispatch->state.shader_size,
                        (unsigned long long)dispatch->state.shader_hash,
                        dispatch->state.root_signature,
                        dispatch->state.descriptor_heap_count, executed != 0);
        }
    }
    BeerD3D12SubmissionStats stats = {
        .submitted_list_count = count,
        .valid_list_count = valid_lists,
        .open_list_count = open_lists,
        .copy_buffer_count = totals[BEER_D3D12_OP_COPY_BUFFER],
        .copy_texture_count = totals[BEER_D3D12_OP_COPY_TEXTURE],
        .copy_resource_count = totals[BEER_D3D12_OP_COPY_RESOURCE],
        .barrier_count = totals[BEER_D3D12_OP_BARRIER],
        .clear_render_target_count = totals[BEER_D3D12_OP_CLEAR_RTV],
        .dispatch_count = totals[BEER_D3D12_OP_DISPATCH],
        .draw_instanced_count = totals[BEER_D3D12_OP_DRAW_INSTANCED],
        .draw_indexed_instanced_count =
            totals[BEER_D3D12_OP_DRAW_INDEXED_INSTANCED],
        .render_target_binding_count =
            totals[BEER_D3D12_OP_SET_RENDER_TARGETS],
        .executed_dispatch_count = executed_dispatches,
        .rejected_dispatch_count = rejected_dispatches,
    };
    if (g_submission_observer) g_submission_observer(&stats);
    D3D12_TRACE("[D3D12] ExecuteCommandLists count=%u valid=%u open=%u "
                "copy-buffer=%llu copy-texture=%llu copy-resource=%llu "
                "barrier=%llu clear=%llu dispatch=%llu draw=%llu "
                "draw-indexed=%llu set-rt=%llu executed=%u rejected=%u "
                "end-query=%llu resolve-query=%llu\n",
                count, valid_lists, open_lists,
                (unsigned long long)totals[BEER_D3D12_OP_COPY_BUFFER],
                (unsigned long long)totals[BEER_D3D12_OP_COPY_TEXTURE],
                (unsigned long long)totals[BEER_D3D12_OP_COPY_RESOURCE],
                (unsigned long long)totals[BEER_D3D12_OP_BARRIER],
                (unsigned long long)totals[BEER_D3D12_OP_CLEAR_RTV],
                (unsigned long long)totals[BEER_D3D12_OP_DISPATCH],
                (unsigned long long)totals[BEER_D3D12_OP_DRAW_INSTANCED],
                (unsigned long long)totals[BEER_D3D12_OP_DRAW_INDEXED_INSTANCED],
                (unsigned long long)totals[BEER_D3D12_OP_SET_RENDER_TARGETS],
                executed_dispatches, rejected_dispatches,
                (unsigned long long)totals[BEER_D3D12_OP_END_QUERY],
                (unsigned long long)totals[BEER_D3D12_OP_RESOLVE_QUERY]);
}
static void __attribute__((ms_abi))
queue_set_marker(BeerD3D12CommandQueue *self, uint32_t metadata,
                 const void *data, uint32_t size)
{ (void)self; (void)metadata; (void)data; (void)size; }
static void __attribute__((ms_abi))
queue_begin_event(BeerD3D12CommandQueue *self, uint32_t metadata,
                  const void *data, uint32_t size)
{ (void)self; (void)metadata; (void)data; (void)size; }
static void __attribute__((ms_abi)) queue_end_event(BeerD3D12CommandQueue *self)
{ (void)self; }
static uint64_t __attribute__((ms_abi))
queue_signal(BeerD3D12CommandQueue *self, void *fence, uint64_t value)
{
    if (!self || !fence) return BEER_E_INVALIDARG;
    uint64_t result = fence_signal((BeerD3D12Fence *)fence, value);
    D3D12_TRACE("[D3D12] Queue type=%u Signal fence=%p value=%llu -> 0x%llx\n",
                self->type, fence, (unsigned long long)value,
                (unsigned long long)result);
    return result;
}
static uint64_t __attribute__((ms_abi))
queue_wait(BeerD3D12CommandQueue *self, void *fence, uint64_t value)
{
    if (!self || !fence) return BEER_E_INVALIDARG;
    uint64_t completed = fence_get_completed_value((BeerD3D12Fence *)fence);
    /* ID3D12CommandQueue::Wait enqueues a GPU-side dependency; it does not
     * report failure merely because the fence has not reached the requested
     * value yet. Beer's command work is currently completed synchronously, so
     * there is no host queue to block, but accepting the dependency is still
     * essential: RE8 treats E_NOTIMPL here as queue initialization failure and
     * never schedules its first graphics pipeline job. Do not advance the
     * fence here--only Signal owns fence completion. */
    D3D12_TRACE("[D3D12] Queue type=%u Wait fence=%p value=%llu completed=%llu pending=%u -> S_OK\n",
                self->type, fence, (unsigned long long)value,
                (unsigned long long)completed, completed < value);
    return BEER_S_OK;
}
static uint64_t __attribute__((ms_abi))
queue_get_timestamp_frequency(BeerD3D12CommandQueue *self, uint64_t *frequency)
{
    (void)self;
    if (!frequency) return BEER_E_INVALIDARG;
    *frequency = UINT64_C(1000000000);
    return BEER_S_OK;
}
static uint64_t __attribute__((ms_abi))
queue_get_clock_calibration(BeerD3D12CommandQueue *self,
                            uint64_t *gpu, uint64_t *cpu)
{
    (void)self;
    if (!gpu || !cpu) return BEER_E_INVALIDARG;
    *gpu = 0;
    *cpu = 0;
    return BEER_S_OK;
}
static void __attribute__((ms_abi))
queue_get_desc(BeerD3D12CommandQueue *self, uint32_t *description)
{
    if (!self || !description) return;
    description[0] = self->type;
    description[1] = self->priority;
    description[2] = self->flags;
    description[3] = self->node_mask;
}

static uint64_t __attribute__((ms_abi))
command_list_query_interface(BeerD3D12GraphicsCommandList *self,
                             const uint8_t *riid, void **output)
{
    if (!output) return BEER_E_INVALIDARG;
    *output = NULL;
    if (!self || (!iid_equal(riid, k_iid_iunknown) &&
                  !iid_equal(riid, k_iid_d3d12_graphics_command_list)))
        return BEER_E_NOINTERFACE;
    command_list_add_ref(self);
    *output = self;
    return BEER_S_OK;
}

static uint32_t __attribute__((ms_abi))
command_list_add_ref(BeerD3D12GraphicsCommandList *self)
{
    return self ? __atomic_add_fetch(&self->references, 1, __ATOMIC_RELAXED) : 0;
}

static void command_list_clear_dispatches(BeerD3D12GraphicsCommandList *self)
{
    BeerD3D12RecordedDispatch *dispatch = self ? self->dispatches : NULL;
    while (dispatch) {
        BeerD3D12RecordedDispatch *next = dispatch->next;
        if (dispatch->state.pipeline_state)
            pipeline_state_release((BeerD3D12PipelineState *)
                                   dispatch->state.pipeline_state);
        if (dispatch->state.root_signature)
            root_signature_release((BeerD3D12RootSignature *)
                                   dispatch->state.root_signature);
        for (uint32_t index = 0;
             index < dispatch->state.descriptor_heap_count; ++index) {
            if (dispatch->state.descriptor_heaps[index])
                descriptor_heap_release((BeerD3D12DescriptorHeap *)
                    dispatch->state.descriptor_heaps[index]);
        }
        free(dispatch);
        dispatch = next;
    }
    if (self) {
        self->dispatches = NULL;
        self->dispatch_tail = &self->dispatches;
    }
}

static uint32_t __attribute__((ms_abi))
command_list_release(BeerD3D12GraphicsCommandList *self)
{
    if (!self) return 0;
    uint32_t remaining = __atomic_sub_fetch(&self->references, 1,
                                             __ATOMIC_ACQ_REL);
    if (remaining == 0) {
        command_list_clear_dispatches(self);
        if (self->pipeline_state) pipeline_state_release(self->pipeline_state);
        if (self->compute_root_signature)
            root_signature_release(self->compute_root_signature);
        if (self->graphics_root_signature)
            root_signature_release(self->graphics_root_signature);
        for (uint32_t index = 0; index < self->descriptor_heap_count; ++index) {
            if (self->descriptor_heaps[index])
                descriptor_heap_release(self->descriptor_heaps[index]);
        }
        allocator_release(self->allocator);
        device_release(self->device);
        free(self);
    }
    return remaining;
}

static uint64_t __attribute__((ms_abi))
command_list_notimpl(BeerD3D12GraphicsCommandList *self)
{
    (void)self;
    return BEER_E_NOTIMPL;
}

#define DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(slot) \
    static uint64_t __attribute__((ms_abi)) command_list_unsupported_##slot( \
        BeerD3D12GraphicsCommandList *self, uint64_t a, uint64_t b, \
        uint64_t c, uint64_t d, uint64_t e, uint64_t f) \
    { \
        (void)self; \
        fprintf(stderr, "[D3D12] unsupported ID3D12GraphicsCommandList slot %u " \
                "args=%llx,%llx,%llx,%llx,%llx,%llx -> E_NOTIMPL\n", \
                (unsigned)(slot), (unsigned long long)a, (unsigned long long)b, \
                (unsigned long long)c, (unsigned long long)d, \
                (unsigned long long)e, (unsigned long long)f); \
        return BEER_E_NOTIMPL; \
    }

DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(11)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(12)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(13)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(14)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(15)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(16)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(17)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(18)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(19)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(20)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(21)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(22)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(23)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(24)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(25)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(26)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(27)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(28)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(29)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(30)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(31)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(32)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(33)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(34)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(35)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(36)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(37)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(38)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(39)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(40)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(41)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(42)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(43)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(44)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(45)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(46)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(47)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(48)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(49)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(50)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(51)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(52)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(53)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(54)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(55)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(56)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(57)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(58)
DECLARE_COMMAND_LIST_UNSUPPORTED_SLOT(59)

static uint64_t __attribute__((ms_abi))
command_list_get_device(BeerD3D12GraphicsCommandList *self,
                        const uint8_t *riid, void **device)
{
    if (!self || !self->device) {
        if (device) *device = NULL;
        return BEER_E_NOINTERFACE;
    }
    return device_query_interface(self->device, riid, device);
}

static uint32_t __attribute__((ms_abi))
command_list_get_type(BeerD3D12GraphicsCommandList *self)
{
    return self ? self->type : 0;
}

static uint64_t __attribute__((ms_abi))
command_list_close(BeerD3D12GraphicsCommandList *self)
{
    if (!self) return BEER_E_INVALIDARG;
    if (self->closed) return BEER_E_INVALIDARG;
    self->closed = 1;
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
command_list_reset(BeerD3D12GraphicsCommandList *self,
                   BeerD3D12CommandAllocator *allocator, void *initial_state)
{
    if (!self || !allocator || allocator->device != self->device ||
        allocator->type != self->type)
        return BEER_E_INVALIDARG;
    if (allocator != self->allocator) {
        allocator_add_ref(allocator);
        allocator_release(self->allocator);
        self->allocator = allocator;
    }
    BeerD3D12PipelineState *pipeline_state = initial_state;
    if (pipeline_state && pipeline_state->device != self->device)
        return BEER_E_INVALIDARG;
    if (pipeline_state != self->pipeline_state) {
        if (pipeline_state) pipeline_state_add_ref(pipeline_state);
        if (self->pipeline_state) pipeline_state_release(self->pipeline_state);
        self->pipeline_state = pipeline_state;
    }
    command_list_clear_dispatches(self);
    self->closed = 0;
    memset(self->operation_counts, 0, sizeof(self->operation_counts));
    memset(self->compute_bindings, 0, sizeof(self->compute_bindings));
    ++self->recording_generation;
    return BEER_S_OK;
}

static void __attribute__((ms_abi))
command_list_copy_buffer_region(BeerD3D12GraphicsCommandList *self,
                                BeerD3D12Resource *destination,
                                uint64_t destination_offset,
                                BeerD3D12Resource *source,
                                uint64_t source_offset, uint64_t byte_count)
{
    if (!self || self->closed || !destination || !source ||
        destination->device != self->device || source->device != self->device ||
        destination->description.dimension != 1 ||
        source->description.dimension != 1 ||
        destination_offset > destination->storage_size ||
        source_offset > source->storage_size ||
        byte_count > destination->storage_size - destination_offset ||
        byte_count > source->storage_size - source_offset)
        return;
    memmove(destination->storage + destination_offset,
            source->storage + source_offset, (size_t)byte_count);
    ++self->operation_counts[BEER_D3D12_OP_COPY_BUFFER];
}

/* D3D12_TEXTURE_COPY_LOCATION is 48 bytes on Win64. The compatibility
 * renderer has no texture execution backend yet, but buffer-footprint copies
 * are required by RE8's upload path. */
typedef struct BeerD3D12TextureCopyLocation {
    BeerD3D12Resource *resource;
    uint32_t type;
    uint32_t _padding;
    union {
        struct {
            uint64_t offset;
            uint32_t format;
            uint32_t width;
            uint32_t height;
            uint32_t depth;
            uint32_t row_pitch;
        } footprint;
        uint32_t subresource_index;
    } value;
} BeerD3D12TextureCopyLocation;

_Static_assert(sizeof(BeerD3D12TextureCopyLocation) == 48,
               "D3D12_TEXTURE_COPY_LOCATION must be 48 bytes on Win64");

static void __attribute__((ms_abi))
command_list_copy_resource(BeerD3D12GraphicsCommandList *self,
                           BeerD3D12Resource *destination,
                           BeerD3D12Resource *source)
{
    if (!self || self->closed || !destination || !source ||
        destination->device != self->device || source->device != self->device)
        return;
    size_t bytes = destination->storage_size < source->storage_size
                       ? destination->storage_size : source->storage_size;
    memmove(destination->storage, source->storage, bytes);
}

static void __attribute__((ms_abi))
command_list_draw_instanced(BeerD3D12GraphicsCommandList *self,
                            uint32_t vertex_count, uint32_t instance_count,
                            uint32_t start_vertex, uint32_t start_instance)
{
    (void)start_vertex;
    (void)start_instance;
    if (!self || self->closed || !vertex_count || !instance_count) return;
    ++self->operation_counts[BEER_D3D12_OP_DRAW_INSTANCED];
    D3D12_TRACE("[D3D12] DrawInstanced vertices=%u instances=%u pso=%p root=%p\n",
                vertex_count, instance_count, (void *)self->pipeline_state,
                (void *)self->graphics_root_signature);
}

static void __attribute__((ms_abi))
command_list_draw_indexed_instanced(BeerD3D12GraphicsCommandList *self,
                                    uint32_t index_count,
                                    uint32_t instance_count,
                                    uint32_t start_index,
                                    int32_t base_vertex,
                                    uint32_t start_instance)
{
    (void)start_index;
    (void)base_vertex;
    (void)start_instance;
    if (!self || self->closed || !index_count || !instance_count) return;
    ++self->operation_counts[BEER_D3D12_OP_DRAW_INDEXED_INSTANCED];
    D3D12_TRACE("[D3D12] DrawIndexedInstanced indices=%u instances=%u "
                "pso=%p root=%p\n", index_count, instance_count,
                (void *)self->pipeline_state,
                (void *)self->graphics_root_signature);
}

static void __attribute__((ms_abi))
command_list_set_render_targets(BeerD3D12GraphicsCommandList *self,
                                uint32_t render_target_count,
                                const uint64_t *render_targets,
                                uint32_t single_handle_range,
                                const uint64_t *depth_stencil)
{
    if (!self || self->closed || (render_target_count && !render_targets)) return;
    ++self->operation_counts[BEER_D3D12_OP_SET_RENDER_TARGETS];
    D3D12_TRACE("[D3D12] OMSetRenderTargets count=%u contiguous=%u rtv=%llx "
                "dsv=%llx\n", render_target_count, single_handle_range != 0,
                (unsigned long long)(render_target_count ? render_targets[0] : 0),
                (unsigned long long)(depth_stencil ? *depth_stencil : 0));
}

static void __attribute__((ms_abi))
command_list_set_blend_factor(BeerD3D12GraphicsCommandList *self,
                              const float *blend_factor)
{
    if (!self || self->closed) return;
    if (blend_factor) memcpy(self->blend_factor, blend_factor,
                             sizeof(self->blend_factor));
    else memset(self->blend_factor, 0, sizeof(self->blend_factor));
}

/* D3D12_RESOURCE_BARRIER starts with Type, Flags and a resource pointer for
 * transition/aliasing barriers. The CPU-backed model tracks transition state
 * so later command recording observes coherent resource metadata. */
typedef struct BeerD3D12ResourceBarrier {
    uint32_t type;
    uint32_t flags;
    BeerD3D12Resource *resource;
    uint32_t state_before;
    uint32_t state_after;
    uint32_t subresource;
    uint32_t _padding;
} BeerD3D12ResourceBarrier;

static void __attribute__((ms_abi))
command_list_resource_barrier(BeerD3D12GraphicsCommandList *self,
                              uint32_t count, const void *barriers_ptr)
{
    if (!self || self->closed || !barriers_ptr) return;
    const BeerD3D12ResourceBarrier *barriers = barriers_ptr;
    for (uint32_t index = 0; index < count; ++index) {
        const BeerD3D12ResourceBarrier *barrier = &barriers[index];
        if (barrier->type != 0 || !barrier->resource ||
            barrier->resource->device != self->device)
            continue;
        barrier->resource->state = barrier->state_after;
    }
}

static void __attribute__((ms_abi))
command_list_set_descriptor_heaps(BeerD3D12GraphicsCommandList *self,
                                  uint32_t count,
                                  BeerD3D12DescriptorHeap *const *heaps)
{
    if (!self || self->closed || (count && !heaps)) return;
    uint32_t valid_count = count > 2 ? 2 : count;
    for (uint32_t index = 0; index < valid_count; ++index) {
        if (!heaps[index] || heaps[index]->device != self->device) return;
    }
    for (uint32_t index = 0; index < valid_count; ++index)
        descriptor_heap_add_ref(heaps[index]);
    for (uint32_t index = 0; index < self->descriptor_heap_count; ++index) {
        if (self->descriptor_heaps[index])
            descriptor_heap_release(self->descriptor_heaps[index]);
    }
    memset(self->descriptor_heaps, 0, sizeof(self->descriptor_heaps));
    self->descriptor_heap_count = valid_count;
    if (valid_count)
        memcpy(self->descriptor_heaps, heaps,
               valid_count * sizeof(self->descriptor_heaps[0]));
}

static void set_command_list_root_signature(
    BeerD3D12GraphicsCommandList *self, BeerD3D12RootSignature **destination,
    BeerD3D12RootSignature *root_signature)
{
    if (!self || self->closed ||
        (root_signature && root_signature->device != self->device) ||
        *destination == root_signature)
        return;
    if (root_signature) root_signature_add_ref(root_signature);
    if (*destination) root_signature_release(*destination);
    *destination = root_signature;
}

static void __attribute__((ms_abi))
command_list_set_compute_root_signature(BeerD3D12GraphicsCommandList *self,
                                        BeerD3D12RootSignature *root_signature)
{
    if (!self) return;
    set_command_list_root_signature(self, &self->compute_root_signature,
                                    root_signature);
}

static void __attribute__((ms_abi))
command_list_set_graphics_root_signature(BeerD3D12GraphicsCommandList *self,
                                         BeerD3D12RootSignature *root_signature)
{
    if (!self) return;
    set_command_list_root_signature(self, &self->graphics_root_signature,
                                    root_signature);
}

static void __attribute__((ms_abi))
command_list_set_compute_root_descriptor_table(
    BeerD3D12GraphicsCommandList *self, uint32_t root_parameter,
    uint64_t descriptor)
{
    if (!self || self->closed ||
        root_parameter >= BEER_D3D12_MAX_ROOT_PARAMETERS)
        return;
    BeerD3D12ComputeBinding *binding = &self->compute_bindings[root_parameter];
    binding->kind = 1;
    binding->descriptor_table = descriptor;
}

static void __attribute__((ms_abi))
command_list_set_graphics_root_descriptor_table(
    BeerD3D12GraphicsCommandList *self, uint32_t root_parameter,
    uint64_t descriptor)
{
    (void)root_parameter;
    (void)descriptor;
    if (!self || self->closed) return;
}

static void __attribute__((ms_abi))
command_list_set_compute_root_constants(
    BeerD3D12GraphicsCommandList *self, uint32_t root_parameter,
    uint32_t count, const void *data, uint32_t destination_offset)
{
    if (!self || self->closed || !data ||
        root_parameter >= BEER_D3D12_MAX_ROOT_PARAMETERS ||
        destination_offset >= BEER_D3D12_MAX_ROOT_CONSTANTS)
        return;
    if (count > BEER_D3D12_MAX_ROOT_CONSTANTS - destination_offset)
        count = BEER_D3D12_MAX_ROOT_CONSTANTS - destination_offset;
    BeerD3D12ComputeBinding *binding = &self->compute_bindings[root_parameter];
    binding->kind = 2;
    memcpy(binding->constants + destination_offset, data,
           (size_t)count * sizeof(uint32_t));
    uint32_t end = destination_offset + count;
    if (end > binding->constant_count) binding->constant_count = end;
}

static void command_list_set_compute_root_descriptor(
    BeerD3D12GraphicsCommandList *self, uint32_t root_parameter,
    uint64_t address, uint32_t kind)
{
    if (!self || self->closed ||
        root_parameter >= BEER_D3D12_MAX_ROOT_PARAMETERS)
        return;
    BeerD3D12ComputeBinding *binding = &self->compute_bindings[root_parameter];
    binding->kind = kind;
    binding->buffer_location = address;
}

static void __attribute__((ms_abi))
command_list_set_compute_root_cbv(BeerD3D12GraphicsCommandList *self,
                                  uint32_t root_parameter, uint64_t address)
{
    command_list_set_compute_root_descriptor(self, root_parameter, address, 3);
}

static void __attribute__((ms_abi))
command_list_set_compute_root_srv(BeerD3D12GraphicsCommandList *self,
                                  uint32_t root_parameter, uint64_t address)
{
    command_list_set_compute_root_descriptor(self, root_parameter, address, 4);
}

static void __attribute__((ms_abi))
command_list_set_compute_root_uav(BeerD3D12GraphicsCommandList *self,
                                  uint32_t root_parameter, uint64_t address)
{
    command_list_set_compute_root_descriptor(self, root_parameter, address, 5);
}

static void __attribute__((ms_abi))
command_list_set_index_buffer(BeerD3D12GraphicsCommandList *self,
                              const void *view)
{
    (void)view;
    if (!self || self->closed) return;
}

static void __attribute__((ms_abi))
command_list_set_vertex_buffers(BeerD3D12GraphicsCommandList *self,
                                uint32_t start_slot, uint32_t count,
                                const void *views)
{
    (void)start_slot;
    (void)count;
    (void)views;
    if (!self || self->closed) return;
}

typedef struct BeerD3D12Rect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
} BeerD3D12Rect;

static uint8_t clear_channel(float value)
{
    if (!(value > 0.0f)) return 0;
    if (value >= 1.0f) return 255;
    return (uint8_t)(value * 255.0f + 0.5f);
}

static void clear_render_target_rectangle(BeerD3D12Resource *resource,
                                          const uint8_t rgba[4],
                                          const BeerD3D12Rect *rectangle)
{
    uint32_t width = resource->description.width > UINT32_MAX
                         ? UINT32_MAX : (uint32_t)resource->description.width;
    uint32_t height = resource->description.height;
    int32_t left = rectangle ? rectangle->left : 0;
    int32_t top = rectangle ? rectangle->top : 0;
    int32_t right = rectangle ? rectangle->right : (int32_t)width;
    int32_t bottom = rectangle ? rectangle->bottom : (int32_t)height;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > (int32_t)width) right = (int32_t)width;
    if (bottom > (int32_t)height) bottom = (int32_t)height;
    if (left >= right || top >= bottom) return;

    uint8_t pixel[4] = {rgba[0], rgba[1], rgba[2], rgba[3]};
    if (resource->description.format == 87 || resource->description.format == 91) {
        pixel[0] = rgba[2];
        pixel[2] = rgba[0];
    }
    size_t row_pitch = (size_t)width * 4;
    for (int32_t y = top; y < bottom; ++y) {
        uint8_t *row = resource->storage + (size_t)y * row_pitch;
        for (int32_t x = left; x < right; ++x)
            memcpy(row + (size_t)x * 4, pixel, sizeof(pixel));
    }
}

static void __attribute__((ms_abi))
command_list_clear_render_target_view(BeerD3D12GraphicsCommandList *self,
                                      uint64_t descriptor,
                                      const float *color, uint32_t rect_count,
                                      const void *rects)
{
    if (!self || self->closed || !descriptor || !color) return;
    BeerD3D12Resource *resource = NULL;
    memcpy(&resource, (const void *)(uintptr_t)descriptor, sizeof(resource));
    if (!resource || resource->device != self->device || !resource->storage ||
        resource->description.dimension != 3 ||
        (resource->description.format != 28 &&
         resource->description.format != 29 &&
         resource->description.format != 87 &&
         resource->description.format != 91))
        return;
    uint64_t required = resource->description.width *
        (uint64_t)resource->description.height * 4;
    if (required > resource->storage_size) return;

    uint64_t input_serial = resource->content_serial;
    uint64_t output_serial = input_serial + 1;
    if (!rect_count && g_clear_target &&
        g_clear_target(resource, input_serial, output_serial,
                       resource->storage, resource->storage_size,
                       (uint32_t)resource->description.width,
                       resource->description.height,
                       resource->description.format, color)) {
        resource->content_serial = output_serial;
        D3D12_TRACE("[D3D12] ClearRenderTargetView Vulkan resource=%p "
                    "%llux%u format=%u serial=%llu\n",
                    (void *)resource,
                    (unsigned long long)resource->description.width,
                    resource->description.height, resource->description.format,
                    (unsigned long long)resource->content_serial);
        return;
    }

    uint8_t rgba[4] = {
        clear_channel(color[0]), clear_channel(color[1]),
        clear_channel(color[2]), clear_channel(color[3]),
    };
    if (!rect_count || !rects) {
        clear_render_target_rectangle(resource, rgba, NULL);
    } else {
        const BeerD3D12Rect *rectangles = rects;
        for (uint32_t index = 0; index < rect_count; ++index)
            clear_render_target_rectangle(resource, rgba, &rectangles[index]);
    }
    resource->content_serial = output_serial;
    D3D12_TRACE("[D3D12] ClearRenderTargetView resource=%p %llux%u "
                "format=%u rgba=%u,%u,%u,%u rects=%u serial=%llu\n",
                (void *)resource,
                (unsigned long long)resource->description.width,
                resource->description.height, resource->description.format,
                rgba[0], rgba[1], rgba[2], rgba[3], rect_count,
                (unsigned long long)resource->content_serial);
}

static uint64_t monotonic_nanoseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static void __attribute__((ms_abi))
command_list_end_query(BeerD3D12GraphicsCommandList *self,
                       BeerD3D12QueryHeap *heap, uint32_t type,
                       uint32_t index)
{
    (void)type;
    if (!self || self->closed || !heap || heap->device != self->device ||
        index >= heap->description[1])
        return;
    heap->results[index] = monotonic_nanoseconds();
}

static void __attribute__((ms_abi))
command_list_resolve_query_data(BeerD3D12GraphicsCommandList *self,
                                BeerD3D12QueryHeap *heap, uint32_t type,
                                uint32_t start, uint32_t count,
                                BeerD3D12Resource *destination,
                                uint64_t destination_offset)
{
    (void)type;
    if (!self || self->closed || !heap || !destination ||
        heap->device != self->device || destination->device != self->device ||
        start > heap->description[1] || count > heap->description[1] - start ||
        destination_offset > destination->storage_size ||
        (uint64_t)count * sizeof(uint64_t) >
            destination->storage_size - destination_offset)
        return;
    memcpy(destination->storage + destination_offset, heap->results + start,
           (size_t)count * sizeof(uint64_t));
}

static void __attribute__((ms_abi))
command_list_copy_texture_region(BeerD3D12GraphicsCommandList *self,
                                 const void *destination_location_ptr,
                                 uint32_t destination_x, uint32_t destination_y,
                                 uint32_t destination_z,
                                 const void *source_location_ptr,
                                 const void *source_box)
{
    (void)destination_z;
    if (!self || self->closed || !destination_location_ptr ||
        !source_location_ptr)
        return;
    const BeerD3D12TextureCopyLocation *destination_location =
        destination_location_ptr;
    const BeerD3D12TextureCopyLocation *source_location = source_location_ptr;
    BeerD3D12Resource *destination = destination_location->resource;
    BeerD3D12Resource *source = source_location->resource;
    if (!destination || !source || destination->device != self->device ||
        source->device != self->device)
        return;

    /* Linear-buffer footprints are sufficient for the currently observed
     * upload path. Texture-subresource execution remains explicit future work. */
    if (destination_location->type != 1 || source_location->type != 1)
        return;
    uint32_t left = 0, top = 0, right = source_location->value.footprint.width;
    uint32_t bottom = source_location->value.footprint.height;
    if (source_box) {
        const uint32_t *box = source_box;
        left = box[0]; top = box[1]; right = box[3]; bottom = box[4];
    }
    if (right < left || bottom < top) return;
    uint32_t bytes_per_pixel = 4;
    uint64_t row_bytes = (uint64_t)(right - left) * bytes_per_pixel;
    uint64_t source_base = source_location->value.footprint.offset +
        (uint64_t)top * source_location->value.footprint.row_pitch +
        (uint64_t)left * bytes_per_pixel;
    uint64_t destination_base = destination_location->value.footprint.offset +
        (uint64_t)destination_y * destination_location->value.footprint.row_pitch +
        (uint64_t)destination_x * bytes_per_pixel;
    for (uint32_t row = 0; row < bottom - top; ++row) {
        uint64_t source_offset = source_base +
            (uint64_t)row * source_location->value.footprint.row_pitch;
        uint64_t destination_offset = destination_base +
            (uint64_t)row * destination_location->value.footprint.row_pitch;
        if (source_offset > source->storage_size ||
            destination_offset > destination->storage_size ||
            row_bytes > source->storage_size - source_offset ||
            row_bytes > destination->storage_size - destination_offset)
            return;
        memmove(destination->storage + destination_offset,
                source->storage + source_offset, (size_t)row_bytes);
    }
}

static uint64_t __attribute__((ms_abi))
fence_query_interface(BeerD3D12Fence *self, const uint8_t *riid, void **output)
{
    if (!output) return BEER_E_INVALIDARG;
    *output = NULL;
    if (!self || (!iid_equal(riid, k_iid_iunknown) &&
                  !iid_equal(riid, k_iid_d3d12_fence)))
        return BEER_E_NOINTERFACE;
    fence_add_ref(self);
    *output = self;
    return BEER_S_OK;
}

static uint32_t __attribute__((ms_abi)) fence_add_ref(BeerD3D12Fence *self)
{
    return self ? __atomic_add_fetch(&self->references, 1, __ATOMIC_RELAXED) : 0;
}

static uint32_t __attribute__((ms_abi)) fence_release(BeerD3D12Fence *self)
{
    if (!self) return 0;
    uint32_t remaining = __atomic_sub_fetch(&self->references, 1,
                                             __ATOMIC_ACQ_REL);
    if (remaining == 0) {
        fence_lock(self);
        BeerD3D12FenceWaiter *waiter = self->waiters;
        self->waiters = NULL;
        fence_unlock(self);
        while (waiter) {
            BeerD3D12FenceWaiter *next = waiter->next;
            free(waiter);
            waiter = next;
        }
        device_release(self->device);
        free(self);
    }
    return remaining;
}

static uint64_t __attribute__((ms_abi)) fence_notimpl(BeerD3D12Fence *self)
{
    (void)self;
    return BEER_E_NOTIMPL;
}

static uint64_t __attribute__((ms_abi))
fence_get_device(BeerD3D12Fence *self, const uint8_t *riid, void **device)
{
    if (!self || !self->device) {
        if (device) *device = NULL;
        return BEER_E_NOINTERFACE;
    }
    return device_query_interface(self->device, riid, device);
}

static uint64_t __attribute__((ms_abi))
fence_get_completed_value(BeerD3D12Fence *self)
{
    return self ? __atomic_load_n(&self->completed_value, __ATOMIC_ACQUIRE) : 0;
}

static uint64_t __attribute__((ms_abi))
fence_set_event_on_completion(BeerD3D12Fence *self, uint64_t value, void *event)
{
    if (!self || !event) return BEER_E_INVALIDARG;
    uint64_t completed = fence_get_completed_value(self);
    D3D12_TRACE("[D3D12] Fence=%p SetEventOnCompletion value=%llu completed=%llu event=%p\n",
                (void *)self, (unsigned long long)value,
                (unsigned long long)completed, event);
    if (completed >= value) {
        if (g_signal_event) g_signal_event((uint64_t)(uintptr_t)event);
        return BEER_S_OK;
    }

    BeerD3D12FenceWaiter *waiter = calloc(1, sizeof(*waiter));
    if (!waiter) return BEER_E_OUTOFMEMORY;
    waiter->value = value;
    waiter->event_handle = (uint64_t)(uintptr_t)event;

    fence_lock(self);
    if (fence_get_completed_value(self) >= value) {
        fence_unlock(self);
        if (g_signal_event) g_signal_event(waiter->event_handle);
        free(waiter);
        return BEER_S_OK;
    }
    waiter->next = self->waiters;
    self->waiters = waiter;
    fence_unlock(self);
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
fence_signal(BeerD3D12Fence *self, uint64_t value)
{
    if (!self) return BEER_E_INVALIDARG;
    uint64_t current = fence_get_completed_value(self);
    D3D12_TRACE("[D3D12] Fence=%p Signal value=%llu completed-before=%llu\n",
                (void *)self, (unsigned long long)value,
                (unsigned long long)current);
    while (current < value &&
           !__atomic_compare_exchange_n(&self->completed_value, &current, value,
                                        0, __ATOMIC_RELEASE,
                                        __ATOMIC_ACQUIRE)) { }

    BeerD3D12FenceWaiter *ready = NULL;
    fence_lock(self);
    BeerD3D12FenceWaiter **cursor = &self->waiters;
    uint64_t completed = fence_get_completed_value(self);
    while (*cursor) {
        BeerD3D12FenceWaiter *waiter = *cursor;
        if (waiter->value <= completed) {
            *cursor = waiter->next;
            waiter->next = ready;
            ready = waiter;
        } else {
            cursor = &waiter->next;
        }
    }
    fence_unlock(self);

    while (ready) {
        BeerD3D12FenceWaiter *next = ready->next;
        if (g_signal_event) g_signal_event(ready->event_handle);
        free(ready);
        ready = next;
    }
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
descriptor_heap_query_interface(BeerD3D12DescriptorHeap *self,
                                const uint8_t *riid, void **output)
{
    if (!output) return BEER_E_INVALIDARG;
    *output = NULL;
    if (!self || (!iid_equal(riid, k_iid_iunknown) &&
                  !iid_equal(riid, k_iid_d3d12_descriptor_heap)))
        return BEER_E_NOINTERFACE;
    descriptor_heap_add_ref(self);
    *output = self;
    return BEER_S_OK;
}

static uint32_t __attribute__((ms_abi))
descriptor_heap_add_ref(BeerD3D12DescriptorHeap *self)
{
    return self ? __atomic_add_fetch(&self->references, 1, __ATOMIC_RELAXED) : 0;
}

static uint32_t __attribute__((ms_abi))
descriptor_heap_release(BeerD3D12DescriptorHeap *self)
{
    if (!self) return 0;
    uint32_t remaining = __atomic_sub_fetch(&self->references, 1,
                                             __ATOMIC_ACQ_REL);
    if (remaining == 0) {
        free(self->storage);
        device_release(self->device);
        free(self);
    }
    return remaining;
}

static uint64_t __attribute__((ms_abi))
descriptor_heap_notimpl(BeerD3D12DescriptorHeap *self)
{
    (void)self;
    return BEER_E_NOTIMPL;
}

static uint64_t __attribute__((ms_abi))
descriptor_heap_get_device(BeerD3D12DescriptorHeap *self,
                           const uint8_t *riid, void **device)
{
    if (!self || !self->device) {
        if (device) *device = NULL;
        return BEER_E_NOINTERFACE;
    }
    return device_query_interface(self->device, riid, device);
}

static void __attribute__((ms_abi))
descriptor_heap_get_desc(BeerD3D12DescriptorHeap *self, uint32_t *description)
{
    if (self && description)
        memcpy(description, self->description, sizeof(self->description));
}

/* D3D12_CPU_DESCRIPTOR_HANDLE and D3D12_GPU_DESCRIPTOR_HANDLE are aggregate
 * return values under the Microsoft x64 ABI.  The caller supplies a hidden
 * result pointer in RDX; returning the scalar directly shifts every argument
 * and made RE8 store an instruction-address-sized value as the heap base. */
static uint64_t *__attribute__((ms_abi))
descriptor_heap_get_cpu_start(BeerD3D12DescriptorHeap *self, uint64_t *result)
{
    if (!result) return NULL;
    *result = self ? self->cpu_start : 0;
    return result;
}

static uint64_t *__attribute__((ms_abi))
descriptor_heap_get_gpu_start(BeerD3D12DescriptorHeap *self, uint64_t *result)
{
    if (!result) return NULL;
    *result = self ? self->gpu_start : 0;
    return result;
}

static uint64_t __attribute__((ms_abi))
query_heap_query_interface(BeerD3D12QueryHeap *self,
                           const uint8_t *riid, void **output)
{
    if (!output) return BEER_E_INVALIDARG;
    *output = NULL;
    if (!self || (!iid_equal(riid, k_iid_iunknown) &&
                  !iid_equal(riid, k_iid_d3d12_query_heap)))
        return BEER_E_NOINTERFACE;
    query_heap_add_ref(self);
    *output = self;
    return BEER_S_OK;
}

static uint32_t __attribute__((ms_abi))
query_heap_add_ref(BeerD3D12QueryHeap *self)
{
    return self ? __atomic_add_fetch(&self->references, 1, __ATOMIC_RELAXED) : 0;
}

static uint32_t __attribute__((ms_abi))
query_heap_release(BeerD3D12QueryHeap *self)
{
    if (!self) return 0;
    uint32_t remaining = __atomic_sub_fetch(&self->references, 1,
                                             __ATOMIC_ACQ_REL);
    if (remaining == 0) {
        free(self->results);
        device_release(self->device);
        free(self);
    }
    return remaining;
}

static uint64_t __attribute__((ms_abi))
query_heap_notimpl(BeerD3D12QueryHeap *self)
{
    (void)self;
    return BEER_E_NOTIMPL;
}

static uint64_t __attribute__((ms_abi))
query_heap_get_device(BeerD3D12QueryHeap *self,
                      const uint8_t *riid, void **device)
{
    if (!self || !self->device) {
        if (device) *device = NULL;
        return BEER_E_NOINTERFACE;
    }
    return device_query_interface(self->device, riid, device);
}

static uint64_t __attribute__((ms_abi))
command_signature_query_interface(BeerD3D12CommandSignature *self,
                                  const uint8_t *riid, void **output)
{
    if (!output) return BEER_E_INVALIDARG;
    *output = NULL;
    if (!self || (!iid_equal(riid, k_iid_iunknown) &&
                  !iid_equal(riid, k_iid_d3d12_command_signature)))
        return BEER_E_NOINTERFACE;
    command_signature_add_ref(self);
    *output = self;
    return BEER_S_OK;
}

static uint32_t __attribute__((ms_abi))
command_signature_add_ref(BeerD3D12CommandSignature *self)
{
    return self ? __atomic_add_fetch(&self->references, 1, __ATOMIC_RELAXED) : 0;
}

static uint32_t __attribute__((ms_abi))
command_signature_release(BeerD3D12CommandSignature *self)
{
    if (!self) return 0;
    uint32_t remaining = __atomic_sub_fetch(&self->references, 1,
                                             __ATOMIC_ACQ_REL);
    if (remaining == 0) {
        free(self->arguments);
        device_release(self->device);
        free(self);
    }
    return remaining;
}

static uint64_t __attribute__((ms_abi))
command_signature_notimpl(BeerD3D12CommandSignature *self)
{
    (void)self;
    return BEER_E_NOTIMPL;
}

static uint64_t __attribute__((ms_abi))
command_signature_get_device(BeerD3D12CommandSignature *self,
                             const uint8_t *riid, void **device)
{
    if (!self || !self->device) {
        if (device) *device = NULL;
        return BEER_E_NOINTERFACE;
    }
    return device_query_interface(self->device, riid, device);
}

static uint64_t __attribute__((ms_abi))
root_signature_query_interface(BeerD3D12RootSignature *self,
                               const uint8_t *riid, void **output)
{
    if (!output) return BEER_E_INVALIDARG;
    *output = NULL;
    if (!self || (!iid_equal(riid, k_iid_iunknown) &&
                  !iid_equal(riid, k_iid_d3d12_root_signature)))
        return BEER_E_NOINTERFACE;
    root_signature_add_ref(self);
    *output = self;
    return BEER_S_OK;
}

static uint32_t __attribute__((ms_abi))
root_signature_add_ref(BeerD3D12RootSignature *self)
{
    return self ? __atomic_add_fetch(&self->references, 1, __ATOMIC_RELAXED) : 0;
}

static void root_signature_free_private_data(BeerD3D12RootSignature *self)
{
    BeerD3D12PrivateData *entry = self ? self->private_data : NULL;
    while (entry) {
        BeerD3D12PrivateData *next = entry->next;
        if (entry->interface_value) {
            void **vtable = *(void ***)entry->interface_value;
            if (vtable && vtable[2]) {
                typedef uint32_t (__attribute__((ms_abi)) *ReleaseFn)(void *);
                ((ReleaseFn)vtable[2])(entry->interface_value);
            }
        }
        free(entry->bytes);
        free(entry);
        entry = next;
    }
}

static BeerD3D12PrivateData *root_signature_find_private_data(
    BeerD3D12RootSignature *self, const uint8_t *guid,
    BeerD3D12PrivateData ***link)
{
    BeerD3D12PrivateData **current = self ? &self->private_data : NULL;
    while (current && *current) {
        if (memcmp((*current)->guid, guid, 16) == 0) {
            if (link) *link = current;
            return *current;
        }
        current = &(*current)->next;
    }
    if (link) *link = current;
    return NULL;
}

static uint32_t __attribute__((ms_abi))
root_signature_release(BeerD3D12RootSignature *self)
{
    if (!self) return 0;
    uint32_t remaining = __atomic_sub_fetch(&self->references, 1,
                                             __ATOMIC_ACQ_REL);
    if (remaining == 0) {
        root_signature_free_private_data(self);
        free(self->name);
        free(self->blob);
        device_release(self->device);
        free(self);
    }
    return remaining;
}

static uint64_t __attribute__((ms_abi))
root_signature_get_private_data(BeerD3D12RootSignature *self,
                                const uint8_t *guid, uint32_t *size, void *data)
{
    if (!self || !guid || !size) return BEER_E_INVALIDARG;
    BeerD3D12PrivateData *entry = root_signature_find_private_data(self, guid, NULL);
    if (!entry) {
        *size = 0;
        return BEER_DXGI_ERROR_NOT_FOUND;
    }
    uint32_t required = entry->interface_value ? (uint32_t)sizeof(void *) : entry->size;
    if (!data || *size < required) {
        *size = required;
        return BEER_DXGI_ERROR_MORE_DATA;
    }
    if (entry->interface_value) {
        memcpy(data, &entry->interface_value, sizeof(entry->interface_value));
        void **vtable = *(void ***)entry->interface_value;
        if (vtable && vtable[1]) {
            typedef uint32_t (__attribute__((ms_abi)) *AddRefFn)(void *);
            ((AddRefFn)vtable[1])(entry->interface_value);
        }
    } else if (required) {
        memcpy(data, entry->bytes, required);
    }
    *size = required;
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
root_signature_set_private_data(BeerD3D12RootSignature *self,
                                const uint8_t *guid, uint32_t size,
                                const void *data)
{
    if (!self || !guid || (size && !data)) return BEER_E_INVALIDARG;
    BeerD3D12PrivateData **link = NULL;
    BeerD3D12PrivateData *entry = root_signature_find_private_data(self, guid, &link);
    if (size == 0) {
        if (!entry) return BEER_S_OK;
        *link = entry->next;
        if (entry->interface_value) {
            void **vtable = *(void ***)entry->interface_value;
            if (vtable && vtable[2]) {
                typedef uint32_t (__attribute__((ms_abi)) *ReleaseFn)(void *);
                ((ReleaseFn)vtable[2])(entry->interface_value);
            }
        }
        free(entry->bytes);
        free(entry);
        return BEER_S_OK;
    }
    uint8_t *copy = malloc(size);
    if (!copy) return BEER_E_OUTOFMEMORY;
    memcpy(copy, data, size);
    if (!entry) {
        entry = calloc(1, sizeof(*entry));
        if (!entry) {
            free(copy);
            return BEER_E_OUTOFMEMORY;
        }
        memcpy(entry->guid, guid, 16);
        *link = entry;
    }
    if (entry->interface_value) {
        void **vtable = *(void ***)entry->interface_value;
        if (vtable && vtable[2]) {
            typedef uint32_t (__attribute__((ms_abi)) *ReleaseFn)(void *);
            ((ReleaseFn)vtable[2])(entry->interface_value);
        }
        entry->interface_value = NULL;
    }
    free(entry->bytes);
    entry->bytes = copy;
    entry->size = size;
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
root_signature_set_private_data_interface(BeerD3D12RootSignature *self,
                                          const uint8_t *guid, void *data)
{
    if (!self || !guid) return BEER_E_INVALIDARG;
    BeerD3D12PrivateData **link = NULL;
    BeerD3D12PrivateData *entry = root_signature_find_private_data(self, guid, &link);
    if (!data) {
        if (!entry) return BEER_S_OK;
        *link = entry->next;
        if (entry->interface_value) {
            void **vtable = *(void ***)entry->interface_value;
            if (vtable && vtable[2]) {
                typedef uint32_t (__attribute__((ms_abi)) *ReleaseFn)(void *);
                ((ReleaseFn)vtable[2])(entry->interface_value);
            }
        }
        free(entry->bytes);
        free(entry);
        return BEER_S_OK;
    }
    void **vtable = *(void ***)data;
    if (!vtable || !vtable[1]) return BEER_E_INVALIDARG;
    typedef uint32_t (__attribute__((ms_abi)) *AddRefFn)(void *);
    ((AddRefFn)vtable[1])(data);
    if (!entry) {
        entry = calloc(1, sizeof(*entry));
        if (!entry) {
            typedef uint32_t (__attribute__((ms_abi)) *ReleaseFn)(void *);
            ((ReleaseFn)vtable[2])(data);
            return BEER_E_OUTOFMEMORY;
        }
        memcpy(entry->guid, guid, 16);
        *link = entry;
    }
    if (entry->interface_value) {
        void **old_vtable = *(void ***)entry->interface_value;
        if (old_vtable && old_vtable[2]) {
            typedef uint32_t (__attribute__((ms_abi)) *ReleaseFn)(void *);
            ((ReleaseFn)old_vtable[2])(entry->interface_value);
        }
    }
    free(entry->bytes);
    entry->bytes = NULL;
    entry->size = 0;
    entry->interface_value = data;
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
root_signature_set_name(BeerD3D12RootSignature *self, const uint16_t *name)
{
    if (!self) return BEER_E_INVALIDARG;
    size_t length = 0;
    if (name) while (name[length]) ++length;
    uint16_t *copy = NULL;
    if (name) {
        copy = malloc((length + 1) * sizeof(*copy));
        if (!copy) return BEER_E_OUTOFMEMORY;
        memcpy(copy, name, (length + 1) * sizeof(*copy));
    }
    free(self->name);
    self->name = copy;
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
root_signature_get_device(BeerD3D12RootSignature *self,
                          const uint8_t *riid, void **device)
{
    if (!self || !self->device) {
        if (device) *device = NULL;
        return BEER_E_NOINTERFACE;
    }
    return device_query_interface(self->device, riid, device);
}

static uint64_t hash_shader_bytes(const uint8_t *bytes, size_t size)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void pipeline_state_set_compute_shader(BeerD3D12PipelineState *object,
                                              size_t pointer_offset)
{
    if (!object || pointer_offset + 16 > object->stream_size) return;
    const uint8_t *shader = NULL;
    size_t shader_size = 0;
    memcpy(&shader, object->stream + pointer_offset, sizeof(shader));
    memcpy(&shader_size, object->stream + pointer_offset + 8,
           sizeof(shader_size));
    if (!shader || shader_size < 4 || shader_size > (size_t)64 * 1024 * 1024)
        return;
    uint8_t *owned_shader = malloc(shader_size);
    if (!owned_shader) return;
    memcpy(owned_shader, shader, shader_size);
    object->compute_shader = owned_shader;
    object->compute_shader_size = shader_size;
    object->compute_shader_hash = hash_shader_bytes(owned_shader, shader_size);
}

static uint64_t create_pipeline_state_copy(BeerD3D12Device *self,
                                           const void *description_data,
                                           size_t description_size,
                                           const uint8_t *riid,
                                           void **pipeline_state,
                                           const char *method)
{
    if (!self || !description_data || !riid || !pipeline_state ||
        description_size == 0 || description_size > (size_t)64 * 1024 * 1024)
        return BEER_E_INVALIDARG;
    *pipeline_state = NULL;
    if (!iid_equal(riid, k_iid_d3d12_pipeline_state) &&
        !iid_equal(riid, k_iid_iunknown))
        return BEER_E_NOINTERFACE;

    BeerD3D12PipelineState *object = calloc(1, sizeof(*object));
    if (!object) return BEER_E_OUTOFMEMORY;
    object->stream = malloc(description_size);
    if (!object->stream) {
        free(object);
        return BEER_E_OUTOFMEMORY;
    }
    memcpy(object->stream, description_data, description_size);
    object->stream_size = description_size;
    object->vtable = g_pipeline_state_vtable;
    object->references = 1;
    object->device = self;
    device_add_ref(self);
    *pipeline_state = object;
    D3D12_TRACE("[D3D12] %s bytes=%zu -> %p\n", method, description_size,
                (void *)object);
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
device_create_graphics_pipeline_state(BeerD3D12Device *self,
                                      const void *description_data,
                                      const uint8_t *riid,
                                      void **pipeline_state)
{
    /* D3D12_GRAPHICS_PIPELINE_STATE_DESC is 656 bytes on Win64. RE8 still
     * creates many legacy graphics PSOs even after obtaining ID3D12Device2;
     * rejecting slot 10 caused every one of those jobs to fail. Preserve the
     * complete opaque descriptor until shader translation is implemented. */
    return create_pipeline_state_copy(self, description_data, 656, riid,
                                      pipeline_state,
                                      "CreateGraphicsPipelineState");
}

static uint64_t __attribute__((ms_abi))
device_create_compute_pipeline_state(BeerD3D12Device *self,
                                     const void *description_data,
                                     const uint8_t *riid,
                                     void **pipeline_state)
{
    /* D3D12_COMPUTE_PIPELINE_STATE_DESC is 56 bytes on Win64. The CS
     * bytecode begins after the root-signature pointer. Copy it now because
     * applications commonly release their temporary shader storage after PSO
     * creation. */
    uint64_t result = create_pipeline_state_copy(self, description_data, 56,
                                                 riid, pipeline_state,
                                                 "CreateComputePipelineState");
    if (result == BEER_S_OK)
        pipeline_state_set_compute_shader(*pipeline_state, 8);
    return result;
}

static uint64_t __attribute__((ms_abi))
device2_create_pipeline_state(BeerD3D12Device *self, const void *description_data,
                              const uint8_t *riid, void **pipeline_state)
{
    if (!description_data) return BEER_E_INVALIDARG;

    /* D3D12_PIPELINE_STATE_STREAM_DESC is { SIZE_T, void * } on Win64. Keep
     * an owned copy of the opaque stream so the resulting COM object remains
     * valid after the caller releases its temporary builder storage. */
    size_t stream_size = 0;
    const void *stream = NULL;
    memcpy(&stream_size, description_data, sizeof(stream_size));
    memcpy(&stream, (const uint8_t *)description_data + 8, sizeof(stream));
    if (!stream) return BEER_E_INVALIDARG;
    return create_pipeline_state_copy(self, stream, stream_size, riid,
                                      pipeline_state,
                                      "Device2::CreatePipelineState");
}

static uint64_t __attribute__((ms_abi))
pipeline_state_query_interface(BeerD3D12PipelineState *self,
                               const uint8_t *riid, void **output)
{
    if (!output) return BEER_E_INVALIDARG;
    *output = NULL;
    if (!self || (!iid_equal(riid, k_iid_iunknown) &&
                  !iid_equal(riid, k_iid_d3d12_pipeline_state)))
        return BEER_E_NOINTERFACE;
    pipeline_state_add_ref(self);
    *output = self;
    return BEER_S_OK;
}

static uint32_t __attribute__((ms_abi))
pipeline_state_add_ref(BeerD3D12PipelineState *self)
{
    return self ? __atomic_add_fetch(&self->references, 1, __ATOMIC_RELAXED) : 0;
}

static uint32_t __attribute__((ms_abi))
pipeline_state_release(BeerD3D12PipelineState *self)
{
    if (!self) return 0;
    uint32_t remaining = __atomic_sub_fetch(&self->references, 1,
                                             __ATOMIC_ACQ_REL);
    if (remaining == 0) {
        free((void *)self->compute_shader);
        free(self->stream);
        device_release(self->device);
        free(self);
    }
    return remaining;
}

static uint64_t __attribute__((ms_abi))
pipeline_state_notimpl(BeerD3D12PipelineState *self)
{
    (void)self;
    return BEER_E_NOTIMPL;
}

static uint64_t __attribute__((ms_abi))
pipeline_state_get_device(BeerD3D12PipelineState *self,
                          const uint8_t *riid, void **device)
{
    if (!self || !self->device) {
        if (device) *device = NULL;
        return BEER_E_NOINTERFACE;
    }
    return device_query_interface(self->device, riid, device);
}

static uint64_t __attribute__((ms_abi))
pipeline_state_get_cached_blob(BeerD3D12PipelineState *self, void **blob)
{
    if (!blob) return BEER_E_INVALIDARG;
    *blob = NULL;
    if (!self || !self->stream || self->stream_size == 0)
        return BEER_E_INVALIDARG;
    BeerD3DBlob *object = calloc(1, sizeof(*object) + self->stream_size);
    if (!object) return BEER_E_OUTOFMEMORY;
    object->vtable = g_blob_vtable;
    object->references = 1;
    object->size = self->stream_size;
    memcpy(object->data, self->stream, self->stream_size);
    *blob = object;
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
blob_query_interface(BeerD3DBlob *self, const uint8_t *riid, void **output)
{
    if (!output) return BEER_E_INVALIDARG;
    *output = NULL;
    if (!self || (!iid_equal(riid, k_iid_iunknown) &&
                  !iid_equal(riid, k_iid_d3d_blob)))
        return BEER_E_NOINTERFACE;
    blob_add_ref(self);
    *output = self;
    return BEER_S_OK;
}

static uint32_t __attribute__((ms_abi)) blob_add_ref(BeerD3DBlob *self)
{
    return self ? __atomic_add_fetch(&self->references, 1, __ATOMIC_RELAXED) : 0;
}

static uint32_t __attribute__((ms_abi)) blob_release(BeerD3DBlob *self)
{
    if (!self) return 0;
    uint32_t remaining = __atomic_sub_fetch(&self->references, 1,
                                             __ATOMIC_ACQ_REL);
    if (remaining == 0) free(self);
    return remaining;
}

static void *__attribute__((ms_abi)) blob_get_buffer_pointer(BeerD3DBlob *self)
{
    return self ? self->data : NULL;
}

static size_t __attribute__((ms_abi)) blob_get_buffer_size(BeerD3DBlob *self)
{
    return self ? self->size : 0;
}

uint64_t __attribute__((ms_abi))
beer_d3d12_create_device(uint64_t adapter, uint32_t minimum_feature_level,
                         const uint8_t *riid, void **device)
{
    (void)adapter;
    if (!device) return BEER_E_INVALIDARG;
    *device = NULL;

    if (!iid_equal(riid, k_iid_d3d12_device) &&
        !iid_equal(riid, k_iid_iunknown))
        return BEER_E_NOINTERFACE;

    device_add_ref(&g_device);
    *device = &g_device;
    fprintf(stderr, "[D3D12] CreateDevice feature-level=0x%x -> %p\n",
            minimum_feature_level, (void *)&g_device);
    return BEER_S_OK;
}

uint64_t __attribute__((ms_abi))
beer_d3d12_get_debug_interface(const uint8_t *riid, void **debug)
{
    (void)riid;
    if (debug) *debug = NULL;
    return BEER_E_NOINTERFACE;
}

typedef struct BeerRootSignatureBytes {
    uint8_t *data;
    size_t size;
    size_t capacity;
} BeerRootSignatureBytes;

static int root_bytes_append(BeerRootSignatureBytes *bytes,
                             const void *source, size_t size)
{
    if (!bytes || (!source && size)) return 0;
    if (size > SIZE_MAX - bytes->size) return 0;
    size_t required = bytes->size + size;
    if (required > bytes->capacity) {
        size_t capacity = bytes->capacity ? bytes->capacity : 256;
        while (capacity < required) {
            if (capacity > SIZE_MAX / 2) {
                capacity = required;
                break;
            }
            capacity *= 2;
        }
        uint8_t *data = realloc(bytes->data, capacity);
        if (!data) return 0;
        bytes->data = data;
        bytes->capacity = capacity;
    }
    if (size) memcpy(bytes->data + bytes->size, source, size);
    bytes->size = required;
    return 1;
}

static int root_bytes_u32(BeerRootSignatureBytes *bytes, uint32_t value)
{
    return root_bytes_append(bytes, &value, sizeof(value));
}

uint64_t __attribute__((ms_abi))
beer_d3d12_serialize_versioned_root_signature(const void *description,
                                              void **blob,
                                              void **error_blob)
{
    if (blob) *blob = NULL;
    if (error_blob) *error_blob = NULL;
    if (!description || !blob) return BEER_E_INVALIDARG;

    /* D3D12_VERSIONED_ROOT_SIGNATURE_DESC is {Version, padding, union Desc} on
     * Win64.  RE8 reuses one descriptor builder for thousands of signatures,
     * so copying only the top-level pointer values collapses distinct layouts
     * into one cache key.  Beer does not pass this private blob to a native
     * D3D12 runtime; serialize the complete graph into a deterministic owned
     * representation consumed by BeerD3D12RootSignature/PipelineState. */
    const uint8_t *versioned = description;
    uint32_t version;
    uint32_t parameter_count;
    const uint8_t *parameters;
    uint32_t sampler_count;
    const uint8_t *samplers;
    uint32_t flags;
    memcpy(&version, versioned, sizeof(version));
    memcpy(&parameter_count, versioned + 8, sizeof(parameter_count));
    memcpy(&parameters, versioned + 16, sizeof(parameters));
    memcpy(&sampler_count, versioned + 24, sizeof(sampler_count));
    memcpy(&samplers, versioned + 32, sizeof(samplers));
    memcpy(&flags, versioned + 40, sizeof(flags));

    if (version < 1 || version > 3 || parameter_count > 4096 ||
        sampler_count > 4096 || (parameter_count && !parameters) ||
        (sampler_count && !samplers))
        return BEER_E_INVALIDARG;

    BeerRootSignatureBytes bytes = {0};
    static const uint8_t magic[8] = {'B','E','E','R','R','S','I','G'};
    int ok = root_bytes_append(&bytes, magic, sizeof(magic)) &&
             root_bytes_u32(&bytes, version) &&
             root_bytes_u32(&bytes, parameter_count) &&
             root_bytes_u32(&bytes, sampler_count) &&
             root_bytes_u32(&bytes, flags);

    /* ROOT_PARAMETER/ROOT_PARAMETER1 are 32 bytes on Win64.  Descriptor table
     * ranges are 20 bytes for v1.0 and 24 bytes for v1.1/v1.2. */
    size_t range_size = version == 1 ? 20 : 24;
    for (uint32_t i = 0; ok && i < parameter_count; ++i) {
        const uint8_t *parameter = parameters + (size_t)i * 32;
        uint32_t type;
        uint32_t visibility;
        memcpy(&type, parameter, sizeof(type));
        memcpy(&visibility, parameter + 24, sizeof(visibility));
        ok = root_bytes_u32(&bytes, type) && root_bytes_u32(&bytes, visibility);
        if (!ok) break;
        if (type == 0) { /* D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE */
            uint32_t range_count;
            const uint8_t *ranges;
            memcpy(&range_count, parameter + 8, sizeof(range_count));
            memcpy(&ranges, parameter + 16, sizeof(ranges));
            if (range_count > 65536 || (range_count && !ranges)) {
                ok = 0;
                break;
            }
            ok = root_bytes_u32(&bytes, range_count) &&
                 root_bytes_append(&bytes, ranges,
                                   (size_t)range_count * range_size);
        } else if (type == 1) { /* 32-bit constants */
            ok = root_bytes_append(&bytes, parameter + 8, 12);
        } else if (type == 2 || type == 3 || type == 4) {
            ok = root_bytes_append(&bytes, parameter + 8,
                                   version == 1 ? 8 : 12);
        } else {
            ok = 0;
        }
    }

    /* STATIC_SAMPLER_DESC is 52 bytes. Version 1.2 adds flags at byte 52. */
    size_t sampler_size = version >= 3 ? 56 : 52;
    if (ok)
        ok = root_bytes_append(&bytes, samplers,
                               (size_t)sampler_count * sampler_size);
    if (!ok || bytes.size == 0) {
        free(bytes.data);
        return BEER_E_INVALIDARG;
    }

    BeerD3DBlob *object = calloc(1, sizeof(*object) + bytes.size);
    if (!object) {
        free(bytes.data);
        return BEER_E_OUTOFMEMORY;
    }
    object->vtable = g_blob_vtable;
    object->references = 1;
    object->size = bytes.size;
    memcpy(object->data, bytes.data, bytes.size);
    free(bytes.data);
    *blob = object;
    D3D12_TRACE("[D3D12] SerializeVersionedRootSignature desc=%p parameters=%u "
                "samplers=%u bytes=%zu -> %p\n", description, parameter_count,
                sampler_count, object->size, (void *)object);
    return BEER_S_OK;
}
