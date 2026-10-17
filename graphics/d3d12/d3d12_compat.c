#include "d3d12_compat.h"

#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
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

/* Recorded upload/constant snapshots preserve D3D12 command-list semantics,
 * but they must never be allowed to consume all host memory. The budget is
 * global because RE8 keeps several closed command lists alive concurrently. */
#define BEER_D3D12_SNAPSHOT_BUDGET ((size_t)256 * 1024 * 1024)
static _Atomic(size_t) g_snapshot_bytes;
static _Atomic(size_t) g_snapshot_peak;
static _Atomic(size_t) g_backing_bytes;
static _Atomic(size_t) g_backing_peak;

static void backing_account_allocate(size_t size)
{
    size_t current = atomic_fetch_add_explicit(
        &g_backing_bytes, size, memory_order_relaxed) + size;
    size_t peak = atomic_load_explicit(&g_backing_peak, memory_order_relaxed);
    while (current > peak &&
           !atomic_compare_exchange_weak_explicit(
               &g_backing_peak, &peak, current,
               memory_order_relaxed, memory_order_relaxed)) { }
}

static void backing_account_release(size_t size)
{
    if (size)
        atomic_fetch_sub_explicit(&g_backing_bytes, size, memory_order_relaxed);
}

size_t beer_d3d12_backing_bytes(void)
{
    return atomic_load_explicit(&g_backing_bytes, memory_order_relaxed);
}

size_t beer_d3d12_backing_peak_bytes(void)
{
    return atomic_load_explicit(&g_backing_peak, memory_order_relaxed);
}
static _Atomic(uint32_t) g_snapshot_budget_warnings;

/* Recorded draw/dispatch records are ~0.5 MiB each because they embed 64
 * root-parameter bindings. calloc() from the brk heap memsets them (dynamic
 * mmap threshold), so ~700 draws per frame consumed GBs of RSS. Map them
 * lazily so untouched binding slots never become resident. */
static void *recorded_alloc(size_t size)
{
    void *memory = mmap(NULL, size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return memory == MAP_FAILED ? NULL : memory;
}

static void recorded_free(void *memory, size_t size)
{
    if (memory) munmap(memory, size);
}

static void copy_used_bindings(BeerD3D12ComputeBinding *destination,
                               const BeerD3D12ComputeBinding *source)
{
    static const BeerD3D12ComputeBinding zero;
    uint32_t used = BEER_D3D12_MAX_ROOT_PARAMETERS;
    while (used && !memcmp(&source[used - 1], &zero, sizeof(zero))) --used;
    memcpy(destination, source, (size_t)used * sizeof(*source));
}

static void *snapshot_allocate(size_t size)
{
    if (!size) return NULL;
    size_t current = atomic_load_explicit(&g_snapshot_bytes,
                                          memory_order_relaxed);
    for (;;) {
        if (size > BEER_D3D12_SNAPSHOT_BUDGET ||
            current > BEER_D3D12_SNAPSHOT_BUDGET - size) {
            if (atomic_fetch_add_explicit(&g_snapshot_budget_warnings, 1,
                                          memory_order_relaxed) < 8)
                fprintf(stderr,
                        "[D3D12 MEMORY] snapshot budget exhausted "
                        "current=%zu request=%zu budget=%zu\n",
                        current, size, BEER_D3D12_SNAPSHOT_BUDGET);
            return NULL;
        }
        if (atomic_compare_exchange_weak_explicit(
                &g_snapshot_bytes, &current, current + size,
                memory_order_relaxed, memory_order_relaxed))
            break;
    }
    void *memory = malloc(size);
    if (!memory) {
        atomic_fetch_sub_explicit(&g_snapshot_bytes, size,
                                  memory_order_relaxed);
        return NULL;
    }
    size_t peak = atomic_load_explicit(&g_snapshot_peak,
                                       memory_order_relaxed);
    while (peak < current + size &&
           !atomic_compare_exchange_weak_explicit(
               &g_snapshot_peak, &peak, current + size,
               memory_order_relaxed, memory_order_relaxed)) { }
    return memory;
}

static void snapshot_release(void *memory, size_t size)
{
    if (!memory) return;
    free(memory);
    atomic_fetch_sub_explicit(&g_snapshot_bytes, size, memory_order_relaxed);
}

size_t beer_d3d12_snapshot_bytes(void)
{
    return atomic_load_explicit(&g_snapshot_bytes, memory_order_relaxed);
}

size_t beer_d3d12_snapshot_peak_bytes(void)
{
    return atomic_load_explicit(&g_snapshot_peak, memory_order_relaxed);
}

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
typedef struct BeerD3D12Resource BeerD3D12Resource;
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
    BEER_D3D12_OP_CLEAR_DSV,
    BEER_D3D12_OP_CLEAR_RTV,
    BEER_D3D12_OP_CLEAR_UAV_UINT,
    BEER_D3D12_OP_DISCARD_RESOURCE,
    BEER_D3D12_OP_DISPATCH,
    BEER_D3D12_OP_DRAW_INSTANCED,
    BEER_D3D12_OP_DRAW_INDEXED_INSTANCED,
    BEER_D3D12_OP_SET_RENDER_TARGETS,
    BEER_D3D12_OP_END_QUERY,
    BEER_D3D12_OP_RESOLVE_QUERY,
    BEER_D3D12_OP_COUNT
} BeerD3D12RecordedOperation;

typedef struct BeerD3D12Rect BeerD3D12Rect;

typedef struct BeerD3D12RecordedDispatch {
    BeerD3D12ComputeDispatch state;
    uint8_t *root_binding_copies[BEER_D3D12_MAX_ROOT_PARAMETERS];
    size_t root_binding_copy_sizes[BEER_D3D12_MAX_ROOT_PARAMETERS];
    uint8_t *descriptor_copies[BEER_D3D12_MAX_ROOT_PARAMETERS]
                              [BEER_D3D12_MAX_RESOLVED_DESCRIPTORS];
    size_t descriptor_copy_sizes[BEER_D3D12_MAX_ROOT_PARAMETERS]
                                [BEER_D3D12_MAX_RESOLVED_DESCRIPTORS];
    BeerD3D12Resource *indirect_arguments;
    BeerD3D12Resource *indirect_count;
    uint64_t indirect_argument_offset;
    uint64_t indirect_count_offset;
    uint32_t indirect_stride;
    uint32_t indirect_maximum;
    uint32_t indirect_index;
    uint64_t sequence;
    struct BeerD3D12RecordedDispatch *next;
} BeerD3D12RecordedDispatch;

typedef struct BeerD3D12RecordedDepthClear {
    BeerD3D12Resource *resource;
    uint32_t flags;
    uint32_t depth_bits;
    uint8_t stencil;
    uint32_t rect_count;
    BeerD3D12Rect *rects;
    uint64_t sequence;
    struct BeerD3D12RecordedDepthClear *next;
} BeerD3D12RecordedDepthClear;

typedef struct BeerD3D12RecordedColorClear {
    BeerD3D12Resource *resource;
    uint32_t color_bits[4];
    uint32_t rect_count;
    BeerD3D12Rect *rects;
    uint64_t sequence;
    struct BeerD3D12RecordedColorClear *next;
} BeerD3D12RecordedColorClear;

typedef struct BeerD3D12RecordedResourceCopy {
    BeerD3D12Resource *destination;
    BeerD3D12Resource *source;
    uint64_t destination_offset;
    uint64_t source_offset;
    uint64_t byte_count;
    uint8_t *source_snapshot;
    uint64_t sequence;
    struct BeerD3D12RecordedResourceCopy *next;
} BeerD3D12RecordedResourceCopy;

typedef struct BeerD3D12RecordedTextureCopy {
    BeerD3D12Resource *destination;
    uint32_t destination_subresource;
    uint32_t destination_x, destination_y, destination_z;
    uint32_t source_format;
    uint32_t source_width, source_height, source_depth;
    uint32_t source_row_pitch;
    uint32_t source_left, source_top, source_front;
    uint32_t copy_width, copy_height, copy_depth;
    uint8_t *source_snapshot;
    size_t source_snapshot_size;
    uint64_t sequence;
    struct BeerD3D12RecordedTextureCopy *next;
} BeerD3D12RecordedTextureCopy;

static void apply_texture_copy(BeerD3D12RecordedTextureCopy *copy);

typedef struct BeerD3D12RecordedDraw {
    BeerD3D12GraphicsDraw state;
    uint64_t sequence;
    BeerD3D12Resource *vertex_buffer_resources[BEER_D3D12_MAX_VERTEX_BUFFERS];
    BeerD3D12Resource *index_buffer_resource;
    uint8_t *vertex_buffer_copies[BEER_D3D12_MAX_VERTEX_BUFFERS];
    size_t vertex_buffer_copy_sizes[BEER_D3D12_MAX_VERTEX_BUFFERS];
    uint8_t *index_buffer_copy;
    size_t index_buffer_copy_size;
    uint8_t *root_binding_copies[BEER_D3D12_MAX_ROOT_PARAMETERS];
    size_t root_binding_copy_sizes[BEER_D3D12_MAX_ROOT_PARAMETERS];
    uint8_t *descriptor_copies[BEER_D3D12_MAX_ROOT_PARAMETERS]
                              [BEER_D3D12_MAX_RESOLVED_DESCRIPTORS];
    size_t descriptor_copy_sizes[BEER_D3D12_MAX_ROOT_PARAMETERS]
                                [BEER_D3D12_MAX_RESOLVED_DESCRIPTORS];
    /* ExecuteIndirect draws: one recorded state, expanded at replay from the
     * GPU-written argument/count buffers. */
    BeerD3D12Resource *indirect_arguments;
    BeerD3D12Resource *indirect_count;
    uint64_t indirect_argument_offset;
    uint64_t indirect_count_offset;
    uint32_t indirect_stride;
    uint32_t indirect_maximum;
    struct BeerD3D12RecordedDraw *next;
} BeerD3D12RecordedDraw;

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
    BeerD3D12ComputeBinding graphics_bindings[BEER_D3D12_MAX_ROOT_PARAMETERS];
    BeerD3D12RecordedDispatch *dispatches;
    BeerD3D12RecordedDispatch **dispatch_tail;
    BeerD3D12RecordedDraw *draws;
    BeerD3D12RecordedDraw **draw_tail;
    BeerD3D12RecordedDepthClear *depth_clears;
    BeerD3D12RecordedDepthClear **depth_clear_tail;
    BeerD3D12RecordedColorClear *color_clears;
    BeerD3D12RecordedColorClear **color_clear_tail;
    BeerD3D12RecordedResourceCopy *resource_copies;
    BeerD3D12RecordedResourceCopy **resource_copy_tail;
    BeerD3D12RecordedTextureCopy *texture_copies;
    BeerD3D12RecordedTextureCopy **texture_copy_tail;
    BeerD3D12BufferView vertex_buffers[BEER_D3D12_MAX_VERTEX_BUFFERS];
    BeerD3D12BufferView index_buffer;
    BeerD3D12Resource *render_targets[BEER_D3D12_MAX_RENDER_TARGETS];
    BeerD3D12Resource *depth_stencil;
    uint32_t render_target_count;
    uint32_t vertex_buffer_mask;
    float viewport[6];
    int32_t scissor[4];
    float blend_factor[4];
    uint32_t primitive_topology;
    uint32_t stencil_reference;
    uint32_t operation_counts[BEER_D3D12_OP_COUNT];
    uint64_t recording_sequence;
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
static BeerD3D12ForgetResourceFn g_forget_resource;
static BeerD3D12ExecuteComputeFn g_execute_compute;
static BeerD3D12ExecuteComputeBatchFn g_execute_compute_batch;
static BeerD3D12ComputeBatchFn g_begin_compute_batch;
static BeerD3D12ComputeBatchFn g_end_compute_batch;
static BeerD3D12ExecuteGraphicsFn g_execute_graphics;
static BeerD3D12ExecuteGraphicsBatchFn g_execute_graphics_batch;
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

void beer_d3d12_set_resource_forgetter(BeerD3D12ForgetResourceFn forgetter)
{
    g_forget_resource = forgetter;
}

static BeerD3D12ForgetResourceFn g_forget_source_resource;

void beer_d3d12_set_resource_source_forgetter(
    BeerD3D12ForgetResourceFn forgetter)
{
    g_forget_source_resource = forgetter;
}

void beer_d3d12_set_compute_executor(BeerD3D12ExecuteComputeFn executor)
{
    g_execute_compute = executor;
}

void beer_d3d12_set_compute_batch_executor(
    BeerD3D12ExecuteComputeBatchFn executor)
{
    g_execute_compute_batch = executor;
}

void beer_d3d12_set_compute_batch_callbacks(
    BeerD3D12ComputeBatchFn begin, BeerD3D12ComputeBatchFn end)
{
    g_begin_compute_batch = begin;
    g_end_compute_batch = end;
}

static void append_compute_access(
    BeerD3D12ComputeResourceAccess *accesses, uint32_t capacity,
    uint32_t *count, const void *resource, uint8_t *storage,
    size_t storage_size, uint64_t *serial_location, uint32_t access)
{
    if ((!resource && !storage) || !access || !accesses || !count) return;
    for (uint32_t index = 0; index < *count; ++index) {
        BeerD3D12ComputeResourceAccess *existing = &accesses[index];
        if ((resource && existing->resource == resource) ||
            (!resource && existing->storage == storage &&
             existing->storage_size == storage_size)) {
            existing->access |= access;
            return;
        }
    }
    if (*count >= capacity) return;
    accesses[*count] = (BeerD3D12ComputeResourceAccess){
        .resource = resource,
        .storage = storage,
        .storage_size = storage_size,
        .serial_location = serial_location,
        .access = access
    };
    ++*count;
}

static uint32_t collect_compute_accesses(
    const BeerD3D12ComputeDispatch *dispatch,
    BeerD3D12ComputeResourceAccess *accesses, uint32_t capacity)
{
    uint32_t count = 0;
    if (!dispatch || !accesses) return 0;
    for (uint32_t parameter = 0;
         parameter < BEER_D3D12_MAX_ROOT_PARAMETERS; ++parameter) {
        const BeerD3D12ComputeBinding *binding = &dispatch->bindings[parameter];
        uint32_t root_access = binding->kind == 5 ?
            (BEER_D3D12_COMPUTE_ACCESS_READ |
             BEER_D3D12_COMPUTE_ACCESS_WRITE) :
            (binding->kind == 3 || binding->kind == 4) ?
                BEER_D3D12_COMPUTE_ACCESS_READ : 0;
        append_compute_access(accesses, capacity, &count,
            binding->resolved_root_resource, binding->resource_storage,
            binding->resource_storage_size, NULL, root_access);
        for (uint32_t descriptor = 0;
             descriptor < binding->descriptor_count &&
             descriptor < BEER_D3D12_MAX_RESOLVED_DESCRIPTORS; ++descriptor) {
            uint32_t type = binding->descriptor_types[descriptor];
            uint32_t descriptor_access = type == 1 ?
                (BEER_D3D12_COMPUTE_ACCESS_READ |
                 BEER_D3D12_COMPUTE_ACCESS_WRITE) :
                (type == 0 || type == 2) ?
                    BEER_D3D12_COMPUTE_ACCESS_READ : 0;
            append_compute_access(accesses, capacity, &count,
                binding->descriptor_resources[descriptor],
                binding->descriptor_resource_storage[descriptor],
                binding->descriptor_resource_storage_size[descriptor],
                (uint64_t *)binding->descriptor_resource_serial_locations[
                    descriptor], descriptor_access);
        }
    }
    return count;
}

static int compute_access_identity_equal(
    const BeerD3D12ComputeResourceAccess *left,
    const BeerD3D12ComputeResourceAccess *right)
{
    if (left->resource || right->resource)
        return left->resource && left->resource == right->resource;
    return left->storage && left->storage == right->storage &&
        left->storage_size == right->storage_size;
}

uint32_t beer_d3d12_compute_hazards_before(
    const BeerD3D12ComputeResourceAccess *accesses, uint32_t first,
    uint32_t count)
{
    uint32_t hazards = 0;
    for (uint32_t current = first; current < first + count; ++current) {
        for (uint32_t prior = 0; prior < first; ++prior) {
            if (!compute_access_identity_equal(&accesses[current],
                                               &accesses[prior]))
                continue;
            if ((accesses[prior].access & BEER_D3D12_COMPUTE_ACCESS_WRITE) &&
                (accesses[current].access & BEER_D3D12_COMPUTE_ACCESS_READ))
                hazards |= BEER_D3D12_COMPUTE_HAZARD_RAW;
            if ((accesses[prior].access & BEER_D3D12_COMPUTE_ACCESS_READ) &&
                (accesses[current].access & BEER_D3D12_COMPUTE_ACCESS_WRITE))
                hazards |= BEER_D3D12_COMPUTE_HAZARD_WAR;
            if ((accesses[prior].access & BEER_D3D12_COMPUTE_ACCESS_WRITE) &&
                (accesses[current].access & BEER_D3D12_COMPUTE_ACCESS_WRITE))
                hazards |= BEER_D3D12_COMPUTE_HAZARD_WAW;
        }
    }
    return hazards;
}

static int compute_staging_range_equal(
    const BeerD3D12ComputeResourceAccess *left,
    const BeerD3D12ComputeResourceAccess *right)
{
    return left->storage && left->storage == right->storage &&
        left->storage_size == right->storage_size;
}

int beer_d3d12_compute_plan_staging(
    const BeerD3D12ComputeResourceAccess *accesses, uint32_t count,
    size_t alignment, size_t *offsets, size_t *total_size)
{
    if (!total_size || (count && (!accesses || !offsets)) || !alignment ||
        (alignment & (alignment - 1u)))
        return 0;

    size_t total = 0;
    for (uint32_t index = 0; index < count; ++index) {
        if (!accesses[index].storage || !accesses[index].storage_size) {
            offsets[index] = SIZE_MAX;
            continue;
        }
        uint32_t duplicate = UINT32_MAX;
        for (uint32_t prior = 0; prior < index; ++prior) {
            if (compute_staging_range_equal(&accesses[index],
                                            &accesses[prior])) {
                duplicate = prior;
                break;
            }
        }
        if (duplicate != UINT32_MAX) {
            offsets[index] = offsets[duplicate];
            continue;
        }
        if (total > SIZE_MAX - (alignment - 1u)) return 0;
        total = (total + alignment - 1u) & ~(alignment - 1u);
        offsets[index] = total;
        if (accesses[index].storage_size > SIZE_MAX - total) return 0;
        total += accesses[index].storage_size;
    }
    if (total) {
        if (total > SIZE_MAX - (alignment - 1u)) return 0;
        total = (total + alignment - 1u) & ~(alignment - 1u);
    }
    *total_size = total;
    return 1;
}

int beer_d3d12_compute_plan_epochs(
    const BeerD3D12ComputeCommand *commands, uint32_t count,
    uint32_t *epochs, uint32_t *epoch_count)
{
    if (!epoch_count || (count && (!commands || !epochs))) return 0;
    if (!count) {
        *epoch_count = 0;
        return 1;
    }

    uint32_t epoch = 0;
    epochs[0] = epoch;
    for (uint32_t index = 1; index < count; ++index) {
        if (commands[index].hazards_before) {
            if (epoch == UINT32_MAX) return 0;
            ++epoch;
        }
        epochs[index] = epoch;
    }
    *epoch_count = epoch + 1u;
    return 1;
}

int beer_d3d12_compute_plan_epoch_staging(
    const BeerD3D12ComputeCommandBatch *batch, const uint32_t *epochs,
    uint32_t epoch_count, size_t alignment, size_t per_command_bytes,
    size_t *access_offsets, size_t *epoch_sizes)
{
    if (!batch || (batch->command_count && (!batch->commands || !epochs)) ||
        (batch->access_count && (!batch->accesses || !access_offsets)) ||
        (epoch_count && !epoch_sizes) || !alignment ||
        (alignment & (alignment - 1u)))
        return 0;
    if (!batch->command_count)
        return epoch_count == 0 && batch->access_count == 0;
    if (!epoch_count) return 0;

    for (uint32_t access = 0; access < batch->access_count; ++access)
        access_offsets[access] = SIZE_MAX;
    memset(epoch_sizes, 0, (size_t)epoch_count * sizeof(*epoch_sizes));

    uint32_t command = 0;
    for (uint32_t epoch = 0; epoch < epoch_count; ++epoch) {
        uint32_t first_command = command;
        while (command < batch->command_count && epochs[command] == epoch)
            ++command;
        if (first_command == command) return 0;
        if (command < batch->command_count && epochs[command] != epoch + 1u)
            return 0;

        uint32_t first_access = batch->commands[first_command].first_access;
        uint32_t end_access = first_access;
        for (uint32_t index = first_command; index < command; ++index) {
            const BeerD3D12ComputeCommand *item = &batch->commands[index];
            if (item->first_access < first_access ||
                item->first_access > batch->access_count ||
                item->access_count > batch->access_count - item->first_access)
                return 0;
            uint32_t item_end = item->first_access + item->access_count;
            if (item_end > end_access) end_access = item_end;
        }

        size_t resource_bytes = 0;
        if (!beer_d3d12_compute_plan_staging(
                batch->accesses + first_access, end_access - first_access,
                alignment, access_offsets + first_access, &resource_bytes))
            return 0;
        size_t command_count = (size_t)(command - first_command);
        if (per_command_bytes &&
            command_count > (SIZE_MAX - resource_bytes) / per_command_bytes)
            return 0;
        epoch_sizes[epoch] = resource_bytes + command_count * per_command_bytes;
    }
    return command == batch->command_count;
}

void beer_d3d12_set_graphics_executor(BeerD3D12ExecuteGraphicsFn executor)
{
    g_execute_graphics = executor;
}

void beer_d3d12_set_graphics_batch_executor(
    BeerD3D12ExecuteGraphicsBatchFn executor)
{
    g_execute_graphics_batch = executor;
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
    const uint8_t *vertex_shader;
    size_t vertex_shader_size;
    uint64_t vertex_shader_hash;
    const uint8_t *pixel_shader;
    size_t pixel_shader_size;
    uint64_t pixel_shader_hash;
    BeerD3D12InputElement input_elements[BEER_D3D12_MAX_INPUT_ELEMENTS];
    uint32_t input_element_count;
    BeerD3D12PipelineStateInfo state;
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

struct BeerD3D12Resource {
    void **vtable;
    uint32_t references;
    BeerD3D12Device *device;
    BeerD3D12Heap *heap;
    BeerD3D12ResourceDesc description;
    uint8_t *storage;
    size_t storage_size;
    uint64_t gpu_virtual_address;
    uint8_t *cpu_map;
    uint64_t content_serial;
    uint32_t heap_properties[5];
    uint32_t heap_flags;
    uint32_t owns_storage;
    uint32_t storage_is_mapping;
    uint32_t state;
    struct BeerD3D12Resource *registry_next;
};

static BeerD3D12Resource *g_resource_registry;
static atomic_flag g_resource_registry_lock = ATOMIC_FLAG_INIT;

typedef struct BeerD3D12CpuDescriptorEntry {
    uint64_t handle;
    BeerD3D12Resource *resource;
    struct BeerD3D12CpuDescriptorEntry *next;
} BeerD3D12CpuDescriptorEntry;

static BeerD3D12CpuDescriptorEntry *g_cpu_descriptor_registry;
static atomic_flag g_cpu_descriptor_registry_lock = ATOMIC_FLAG_INIT;

static void cpu_descriptor_registry_lock(void)
{
    while (atomic_flag_test_and_set_explicit(&g_cpu_descriptor_registry_lock,
                                             memory_order_acquire)) { }
}

static void cpu_descriptor_registry_unlock(void)
{
    atomic_flag_clear_explicit(&g_cpu_descriptor_registry_lock,
                               memory_order_release);
}

static void cpu_descriptor_registry_set(uint64_t handle,
                                        BeerD3D12Resource *resource)
{
    if (!handle) return;
    cpu_descriptor_registry_lock();
    BeerD3D12CpuDescriptorEntry **link = &g_cpu_descriptor_registry;
    while (*link && (*link)->handle != handle) link = &(*link)->next;
    if (!resource) {
        if (*link) {
            BeerD3D12CpuDescriptorEntry *entry = *link;
            *link = entry->next;
            free(entry);
        }
        cpu_descriptor_registry_unlock();
        return;
    }
    if (!*link) {
        *link = calloc(1, sizeof(**link));
        if (!*link) {
            cpu_descriptor_registry_unlock();
            return;
        }
        (*link)->handle = handle;
    }
    (*link)->resource = resource;
    cpu_descriptor_registry_unlock();
}

static BeerD3D12Resource *cpu_descriptor_registry_get(uint64_t handle,
                                                       BeerD3D12Device *device)
{
    if (!handle) return NULL;
    cpu_descriptor_registry_lock();
    BeerD3D12CpuDescriptorEntry *entry = g_cpu_descriptor_registry;
    while (entry && entry->handle != handle) entry = entry->next;
    BeerD3D12Resource *resource = entry ? entry->resource : NULL;
    if (resource && resource->device != device) resource = NULL;
    cpu_descriptor_registry_unlock();
    return resource;
}

static void cpu_descriptor_registry_copy(uint64_t destination, uint64_t source)
{
    if (!destination || !source) return;
    cpu_descriptor_registry_lock();
    BeerD3D12CpuDescriptorEntry *source_entry = g_cpu_descriptor_registry;
    while (source_entry && source_entry->handle != source)
        source_entry = source_entry->next;
    BeerD3D12CpuDescriptorEntry **destination_link = &g_cpu_descriptor_registry;
    while (*destination_link && (*destination_link)->handle != destination)
        destination_link = &(*destination_link)->next;
    if (!source_entry) {
        if (*destination_link) {
            BeerD3D12CpuDescriptorEntry *entry = *destination_link;
            *destination_link = entry->next;
            free(entry);
        }
    } else {
        if (!*destination_link) {
            *destination_link = calloc(1, sizeof(**destination_link));
            if (*destination_link) (*destination_link)->handle = destination;
        }
        if (*destination_link)
            (*destination_link)->resource = source_entry->resource;
    }
    cpu_descriptor_registry_unlock();
}

static void cpu_descriptor_registry_forget_resource(
    BeerD3D12Resource *resource)
{
    if (!resource) return;
    cpu_descriptor_registry_lock();
    BeerD3D12CpuDescriptorEntry **link = &g_cpu_descriptor_registry;
    while (*link) {
        BeerD3D12CpuDescriptorEntry *entry = *link;
        if (entry->resource == resource) {
            *link = entry->next;
            free(entry);
        } else {
            link = &entry->next;
        }
    }
    cpu_descriptor_registry_unlock();
}

static void resource_registry_lock(void)
{
    while (atomic_flag_test_and_set_explicit(&g_resource_registry_lock,
                                             memory_order_acquire)) { }
}

static void resource_registry_unlock(void)
{
    atomic_flag_clear_explicit(&g_resource_registry_lock,
                               memory_order_release);
}

static void resource_registry_add(BeerD3D12Resource *resource)
{
    resource_registry_lock();
    resource->registry_next = g_resource_registry;
    g_resource_registry = resource;
    resource_registry_unlock();
}

static void resource_registry_remove(BeerD3D12Resource *resource)
{
    resource_registry_lock();
    BeerD3D12Resource **link = &g_resource_registry;
    while (*link && *link != resource) link = &(*link)->registry_next;
    if (*link == resource) *link = resource->registry_next;
    resource->registry_next = NULL;
    resource_registry_unlock();
}

/* Resolves a D3D12 GPU virtual address while retaining the resource. The
 * returned storage remains valid until the caller releases the resource. */
static BeerD3D12Resource *resource_registry_resolve_buffer(
    uint64_t address, size_t requested, const uint8_t **storage)
{
    if (storage) *storage = NULL;
    if (!address || !requested || !storage) return NULL;
    resource_registry_lock();
    BeerD3D12Resource *match = NULL;
    uint64_t match_offset = 0;
    for (BeerD3D12Resource *resource = g_resource_registry; resource;
         resource = resource->registry_next) {
        if (!resource->gpu_virtual_address || !resource->storage ||
            address < resource->gpu_virtual_address) continue;
        uint64_t offset64 = address - resource->gpu_virtual_address;
        if (offset64 > resource->storage_size) continue;
        size_t offset = (size_t)offset64;
        if (requested > resource->storage_size - offset) continue;
        /* Placed resources can overlap a larger alias in the same heap. D3D12
         * GPU virtual addresses identify the most specific view whose base is
         * nearest to the requested address; registry insertion order must not
         * decide which resource metadata a vertex/index/root view receives. */
        if (!match || resource->gpu_virtual_address >
                          match->gpu_virtual_address ||
            (resource->gpu_virtual_address == match->gpu_virtual_address &&
             resource->storage_size < match->storage_size)) {
            match = resource;
            match_offset = offset64;
        }
    }
    if (match) {
        __atomic_add_fetch(&match->references, 1, __ATOMIC_RELAXED);
        *storage = (match->cpu_map ? match->cpu_map : match->storage) +
            (size_t)match_offset;
    }
    resource_registry_unlock();
    return match;
}

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
    size_t storage_size;
    uint32_t storage_is_mapping;
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
static uint64_t __attribute__((ms_abi))
resource_read_from_subresource(BeerD3D12Resource *self, void *destination,
                               uint32_t destination_row_pitch,
                               uint32_t destination_depth_pitch,
                               uint32_t source_subresource,
                               const void *source_box);
static uint64_t __attribute__((ms_abi))
resource_write_to_subresource(BeerD3D12Resource *self,
                              uint32_t destination_subresource,
                              const void *destination_box, const void *source,
                              uint32_t source_row_pitch,
                              uint32_t source_depth_pitch);
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
static void apply_resource_copy(BeerD3D12RecordedResourceCopy *copy);
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
static int root_signature_resolve_compute_descriptors(
    BeerD3D12GraphicsCommandList *self, BeerD3D12ComputeDispatch *state);
static void __attribute__((ms_abi))
command_list_dispatch(BeerD3D12GraphicsCommandList *self,
                      uint32_t group_count_x, uint32_t group_count_y,
                      uint32_t group_count_z)
{
    if (!self || self->closed || !group_count_x || !group_count_y ||
        !group_count_z)
        return;
    BeerD3D12RecordedDispatch *dispatch =
        recorded_alloc(sizeof(BeerD3D12RecordedDispatch));
    if (!dispatch) return;
    dispatch->state.pipeline_state = self->pipeline_state;
    dispatch->state.root_signature = self->compute_root_signature;
    dispatch->state.group_count_x = group_count_x;
    dispatch->state.group_count_y = group_count_y;
    dispatch->state.group_count_z = group_count_z;
    dispatch->sequence = ++self->recording_sequence;
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
    copy_used_bindings(dispatch->state.bindings, self->compute_bindings);
    /* Root CBV/SRV/UAV descriptors carry GPU virtual addresses directly,
     * outside descriptor heaps. Resolve and retain those resources in the
     * immutable dispatch snapshot just like heap descriptors. */
    for (uint32_t parameter = 0;
         parameter < BEER_D3D12_MAX_ROOT_PARAMETERS; ++parameter) {
        BeerD3D12ComputeBinding *binding =
            &dispatch->state.bindings[parameter];
        if (binding->kind < 3 || binding->kind > 5 ||
            !binding->buffer_location)
            continue;
        const uint8_t *storage = NULL;
        BeerD3D12Resource *resource = resource_registry_resolve_buffer(
            binding->buffer_location, 1, &storage);
        if (!resource) continue;
        size_t offset = (size_t)(binding->buffer_location -
                                 resource->gpu_virtual_address);
        binding->resource_storage = (uint8_t *)storage;
        binding->resource_storage_size = resource->storage_size - offset;
        binding->resource = resource;
        binding->resource_serial_location = &resource->content_serial;
        binding->resource_heap_type = resource->heap_properties[0];
        binding->resolved_root_resource = resource;
    }
    /* Expand descriptor tables using root-signature range metadata.
     * This mirrors root_signature_resolve_draw_descriptors for compute
     * dispatches and is required for shaders that access descriptors beyond
     * the first slot in a large shared table (e.g. blur passes using a
     * 128-SRV / 8-UAV / 14-CBV shared root signature). */
    root_signature_resolve_compute_descriptors(self, &dispatch->state);
    /* Descriptor tables and root descriptors that read upload/readback heaps
     * are CPU ring snapshots at Dispatch time. RE8 reuses those rings before
     * ExecuteCommandLists; retaining their live pointer makes an earlier fast
     * copy consume a later command's bytes. Default/custom resources remain
     * live because preceding GPU commands in this submission can produce them. */
    for (uint32_t parameter = 0;
         parameter < BEER_D3D12_MAX_ROOT_PARAMETERS; ++parameter) {
        BeerD3D12ComputeBinding *binding =
            &dispatch->state.bindings[parameter];
        BeerD3D12Resource *root_resource = (BeerD3D12Resource *)
            binding->resolved_root_resource;
        if (root_resource && binding->kind == 3 &&
            binding->resource_storage && binding->resource_storage_size &&
            (root_resource->heap_properties[0] == 2 ||
             root_resource->heap_properties[0] == 3)) {
            size_t size = binding->resource_storage_size;
            /* Only constants require an immutable per-command copy. Root
             * SRV/UAV descriptors identify retained resources and are consumed
             * live in queue order, as D3D12 requires. Cloning the remainder of
             * RE8's 32 MiB upload ring for every SRV/UAV dispatch retained
             * several GiB and eventually invoked the system OOM killer. */
            if (size > 65536) size = 65536;
            uint8_t *copy = snapshot_allocate(size);
            if (copy) {
                memcpy(copy, binding->resource_storage, size);
                dispatch->root_binding_copies[parameter] = copy;
                dispatch->root_binding_copy_sizes[parameter] = size;
                binding->resource_storage = copy;
                binding->resource_storage_size = size;
            } else {
                binding->resource_storage = NULL;
                binding->resource_storage_size = 0;
            }
        }
        for (uint32_t descriptor = 0;
             descriptor < binding->descriptor_count; ++descriptor) {
            BeerD3D12Resource *resource = (BeerD3D12Resource *)
                binding->descriptor_resources[descriptor];
            /* Descriptor-table SRV/UAV resources remain live and retained;
             * only CBV bytes must be frozen against mapped-ring reuse. */
            if (!resource || binding->descriptor_types[descriptor] != 2 ||
                !binding->descriptor_resource_storage[descriptor] ||
                !binding->descriptor_resource_storage_size[descriptor] ||
                (resource->heap_properties[0] != 2 &&
                 resource->heap_properties[0] != 3))
                continue;
            size_t size =
                binding->descriptor_resource_storage_size[descriptor];
            if (size > 65536) size = 65536;
            uint8_t *copy = snapshot_allocate(size);
            if (!copy) {
                binding->descriptor_resource_storage[descriptor] = NULL;
                binding->descriptor_resource_storage_size[descriptor] = 0;
                continue;
            }
            memcpy(copy, binding->descriptor_resource_storage[descriptor],
                   size);
            dispatch->descriptor_copies[parameter][descriptor] = copy;
            dispatch->descriptor_copy_sizes[parameter][descriptor] = size;
            binding->descriptor_resource_storage[descriptor] = copy;
            binding->descriptor_resource_storage_size[descriptor] = size;
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
command_list_clear_state(BeerD3D12GraphicsCommandList *self,
                         BeerD3D12PipelineState *pipeline_state);
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
command_list_execute_indirect(BeerD3D12GraphicsCommandList *self,
                              BeerD3D12CommandSignature *signature,
                              uint32_t maximum_command_count,
                              BeerD3D12Resource *argument_buffer,
                              uint64_t argument_buffer_offset,
                              BeerD3D12Resource *count_buffer,
                              uint64_t count_buffer_offset);
static void __attribute__((ms_abi))
command_list_execute_indirect(BeerD3D12GraphicsCommandList *self,
                              BeerD3D12CommandSignature *signature,
                              uint32_t maximum_command_count,
                              BeerD3D12Resource *argument_buffer,
                              uint64_t argument_buffer_offset,
                              BeerD3D12Resource *count_buffer,
                              uint64_t count_buffer_offset)
{
    if (getenv("BEER_D3D12_TRACE_INDIRECT")) {
        static _Atomic(uint32_t) indirect_calls;
        uint32_t call = atomic_fetch_add(&indirect_calls, 1);
        if (call < 40 || call % 200 == 0)
            fprintf(stderr,
                "[D3D12 INDIRECT] call=%u arguments=%u bytes=%zu stride=%u "
                "maximum=%u count-buffer=%d\n", call,
                signature ? signature->argument_count : 0,
                signature ? signature->arguments_size : 0,
                signature ? signature->byte_stride : 0,
                maximum_command_count, count_buffer != NULL);
    }
    D3D12_TRACE("[D3D12] ExecuteIndirect entry self=%p closed=%u signature=%p "
                "arguments=%u bytes=%zu stride=%u argument-buffer=%p "
                "maximum=%u offset=%llu count-buffer=%p count-offset=%llu\n",
                (void *)self, self ? self->closed : 0, (void *)signature,
                signature ? signature->argument_count : 0,
                signature ? signature->arguments_size : 0,
                signature ? signature->byte_stride : 0,
                (void *)argument_buffer, maximum_command_count,
                (unsigned long long)argument_buffer_offset,
                (void *)count_buffer, (unsigned long long)count_buffer_offset);
    if (!self || self->closed || !signature || !argument_buffer ||
        signature->device != self->device ||
        argument_buffer->device != self->device ||
        argument_buffer->vtable != g_resource_vtable ||
        !maximum_command_count || signature->argument_count != 1 ||
        signature->arguments_size < 8 || !signature->byte_stride) {
        if (getenv("BEER_D3D12_TRACE_INDIRECT")) {
            static _Atomic(uint32_t) basic_rejects;
            uint32_t n = atomic_fetch_add(&basic_rejects, 1);
            if (n < 20 || n % 500 == 0)
                fprintf(stderr, "[D3D12 INDIRECT REJECT basic] n=%u closed=%u "
                        "sig=%p buf=%p max=%u args=%u size=%zu stride=%u "
                        "sigdev=%d bufdev=%d vtable=%d\n", n,
                        self ? self->closed : 0, (void *)signature,
                        (void *)argument_buffer, maximum_command_count,
                        signature ? signature->argument_count : 0,
                        signature ? signature->arguments_size : 0,
                        signature ? signature->byte_stride : 0,
                        signature && self ? signature->device == self->device : -1,
                        argument_buffer && self ?
                            argument_buffer->device == self->device : -1,
                        argument_buffer ?
                            argument_buffer->vtable == g_resource_vtable : -1);
        }
        D3D12_TRACE("[D3D12] ExecuteIndirect rejected basic validation "
                    "signature-device=%p list-device=%p buffer-device=%p "
                    "buffer-vtable=%p expected-vtable=%p\n",
                    signature ? (void *)signature->device : NULL,
                    self ? (void *)self->device : NULL,
                    argument_buffer ? (void *)argument_buffer->device : NULL,
                    argument_buffer ? (void *)argument_buffer->vtable : NULL,
                    (void *)g_resource_vtable);
        return;
    }

    uint32_t argument_type = 0;
    memcpy(&argument_type, signature->arguments, sizeof(argument_type));
    /* D3D12_INDIRECT_ARGUMENT_TYPE: 0 draw, 1 draw-indexed, 2 dispatch. */
    size_t payload_size = argument_type == 0 ? 16u :
        argument_type == 1 ? 20u : argument_type == 2 ? 12u : 0u;
    if (!payload_size || signature->byte_stride < payload_size ||
        argument_buffer_offset > argument_buffer->storage_size) {
        if (getenv("BEER_D3D12_TRACE_INDIRECT")) {
            static _Atomic(uint32_t) type_rejects;
            uint32_t n = atomic_fetch_add(&type_rejects, 1);
            if (n < 20 || n % 500 == 0)
                fprintf(stderr, "[D3D12 INDIRECT REJECT type] n=%u type=%u "
                        "payload=%zu stride=%u offset=%llu storage=%zu\n", n,
                        argument_type, payload_size, signature->byte_stride,
                        (unsigned long long)argument_buffer_offset,
                        argument_buffer->storage_size);
        }
        D3D12_TRACE("[D3D12] ExecuteIndirect rejected type=%u payload=%zu "
                    "storage=%zu\n", argument_type, payload_size,
                    argument_buffer->storage_size);
        return;
    }

    uint32_t command_count = maximum_command_count;
    if (count_buffer) {
        if (count_buffer->device != self->device ||
            count_buffer->vtable != g_resource_vtable ||
            count_buffer_offset > count_buffer->storage_size ||
            count_buffer->storage_size - (size_t)count_buffer_offset < 4)
            return;
        uint32_t requested_count = 0;
        memcpy(&requested_count, count_buffer->storage + count_buffer_offset,
               sizeof(requested_count));
        if (requested_count < command_count) command_count = requested_count;
    }

    size_t available = argument_buffer->storage_size -
        (size_t)argument_buffer_offset;
    uint32_t available_commands = available < payload_size ? 0 :
        1u + (uint32_t)((available - payload_size) / signature->byte_stride);
    if (command_count > available_commands) command_count = available_commands;

    if (argument_type <= 1) {
        /* Draw arguments and counts are GPU-generated; the CPU backing is
         * stale at record time. Record one draw state and expand it at queue
         * replay after preceding GPU writes have landed. */
        uint32_t slots = available_commands < maximum_command_count ?
            available_commands : maximum_command_count;
        if (!slots) return;
        BeerD3D12RecordedDraw **tail_before = self->draw_tail;
        if (argument_type == 0)
            command_list_draw_instanced(self, 1, 1, 0, 0);
        else
            command_list_draw_indexed_instanced(self, 1, 1, 0, 0, 0);
        BeerD3D12RecordedDraw *recorded =
            tail_before ? *tail_before : self->draws;
        if (!recorded || recorded->next) return;
        recorded->indirect_arguments = argument_buffer;
        recorded->indirect_count = count_buffer;
        recorded->indirect_argument_offset = argument_buffer_offset;
        recorded->indirect_count_offset = count_buffer_offset;
        recorded->indirect_stride = signature->byte_stride;
        recorded->indirect_maximum = slots;
        resource_add_ref(argument_buffer);
        if (count_buffer) resource_add_ref(count_buffer);
        return;
    }

    const uint8_t *arguments = argument_buffer->storage +
        (size_t)argument_buffer_offset;
    /* GPU-written indirect dispatch counts are not authoritative until queue
     * replay. Record every command allowed by the buffer and API maximum; the
     * live count buffer decides which records execute at that point. */
    if (argument_type == 2 && count_buffer)
        command_count = available_commands < maximum_command_count ?
            available_commands : maximum_command_count;
    for (uint32_t command = 0; command < command_count; ++command) {
        const uint8_t *data = arguments +
            (size_t)command * signature->byte_stride;
        if (argument_type == 0) {
            uint32_t values[4];
            memcpy(values, data, sizeof(values));
            command_list_draw_instanced(self, values[0], values[1],
                                        values[2], values[3]);
        } else if (argument_type == 1) {
            uint32_t values[5];
            memcpy(values, data, sizeof(values));
            command_list_draw_indexed_instanced(
                self, values[0], values[1], values[2], (int32_t)values[3],
                values[4]);
        } else {
            /* Dispatch arguments are GPU-generated data. Keep the ordinary
             * state snapshot, but resolve the argument and optional count
             * buffers at queue replay after preceding GPU writes. Expanding
             * them here reads stale CPU backing and can launch arbitrary work. */
            BeerD3D12RecordedDispatch **before = self->dispatch_tail;
            command_list_dispatch(self, 1, 1, 1);
            BeerD3D12RecordedDispatch *dispatch = before ? *before : NULL;
            if (!dispatch) break;
            dispatch->indirect_arguments = argument_buffer;
            dispatch->indirect_count = count_buffer;
            dispatch->indirect_argument_offset = argument_buffer_offset;
            dispatch->indirect_count_offset = count_buffer_offset;
            dispatch->indirect_stride = signature->byte_stride;
            dispatch->indirect_maximum = maximum_command_count;
            dispatch->indirect_index = command;
            resource_add_ref(argument_buffer);
            if (count_buffer) resource_add_ref(count_buffer);
        }
    }
    if (getenv("BEER_D3D12_TRACE_INDIRECT")) {
        static _Atomic(uint64_t) total_calls, total_maximum, total_executed,
            zero_count_calls, count_buffer_calls, nonzero_first_args,
            first_index_sum;
        uint64_t calls = atomic_fetch_add(&total_calls, 1) + 1;
        atomic_fetch_add(&total_maximum, maximum_command_count);
        atomic_fetch_add(&total_executed, command_count);
        if (count_buffer) atomic_fetch_add(&count_buffer_calls, 1);
        if (!command_count) atomic_fetch_add(&zero_count_calls, 1);
        if (command_count && argument_type == 1) {
            uint32_t first[5];
            memcpy(first, arguments, sizeof(first));
            if (first[0]) {
                atomic_fetch_add(&nonzero_first_args, 1);
                atomic_fetch_add(&first_index_sum, first[0]);
            }
        }
        if (calls % 2000 == 0)
            fprintf(stderr,
                "[D3D12 INDIRECT TOTAL] calls=%llu type=%u maximum-sum=%llu "
                "executed-sum=%llu zero-count-calls=%llu "
                "count-buffer-calls=%llu first-args-nonzero=%llu "
                "first-index-sum=%llu\n", (unsigned long long)calls,
                argument_type, (unsigned long long)total_maximum,
                (unsigned long long)total_executed,
                (unsigned long long)zero_count_calls,
                (unsigned long long)count_buffer_calls,
                (unsigned long long)nonzero_first_args,
                (unsigned long long)first_index_sum);
    }
    D3D12_TRACE("[D3D12] ExecuteIndirect type=%u maximum=%u executed=%u "
                "stride=%u argument-offset=%llu count-buffer=%p\n",
                argument_type, maximum_command_count, command_count,
                signature->byte_stride,
                (unsigned long long)argument_buffer_offset,
                (void *)count_buffer);
}

static void __attribute__((ms_abi))
command_list_set_render_targets(BeerD3D12GraphicsCommandList *self,
                                uint32_t render_target_count,
                                const uint64_t *render_targets,
                                uint32_t single_handle_range,
                                const uint64_t *depth_stencil);
static void __attribute__((ms_abi))
command_list_set_primitive_topology(BeerD3D12GraphicsCommandList *self,
                                    uint32_t primitive_topology);
static void __attribute__((ms_abi))
command_list_set_viewports(BeerD3D12GraphicsCommandList *self,
                           uint32_t count, const void *viewports);
static void __attribute__((ms_abi))
command_list_set_scissor_rects(BeerD3D12GraphicsCommandList *self,
                               uint32_t count, const void *rects);
static void __attribute__((ms_abi))
command_list_set_blend_factor(BeerD3D12GraphicsCommandList *self,
                              const float *blend_factor);
static void __attribute__((ms_abi))
command_list_set_stencil_reference(BeerD3D12GraphicsCommandList *self,
                                   uint32_t stencil_reference);
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
command_list_set_compute_root_constant(
    BeerD3D12GraphicsCommandList *self, uint32_t root_parameter,
    uint32_t value, uint32_t destination_offset);
static void __attribute__((ms_abi))
command_list_set_graphics_root_constant(
    BeerD3D12GraphicsCommandList *self, uint32_t root_parameter,
    uint32_t value, uint32_t destination_offset);
static void __attribute__((ms_abi))
command_list_set_compute_root_constants(
    BeerD3D12GraphicsCommandList *self, uint32_t root_parameter,
    uint32_t count, const void *data, uint32_t destination_offset);
static void __attribute__((ms_abi))
command_list_set_graphics_root_constants(
    BeerD3D12GraphicsCommandList *self, uint32_t root_parameter,
    uint32_t count, const void *data, uint32_t destination_offset);
static void __attribute__((ms_abi))
command_list_set_compute_root_cbv(BeerD3D12GraphicsCommandList *self,
                                  uint32_t root_parameter, uint64_t address);
static void __attribute__((ms_abi))
command_list_set_graphics_root_cbv(BeerD3D12GraphicsCommandList *self,
                                   uint32_t root_parameter, uint64_t address);
static void __attribute__((ms_abi))
command_list_set_compute_root_srv(BeerD3D12GraphicsCommandList *self,
                                  uint32_t root_parameter, uint64_t address);
static void __attribute__((ms_abi))
command_list_set_graphics_root_srv(BeerD3D12GraphicsCommandList *self,
                                   uint32_t root_parameter, uint64_t address);
static void __attribute__((ms_abi))
command_list_set_compute_root_uav(BeerD3D12GraphicsCommandList *self,
                                  uint32_t root_parameter, uint64_t address);
static void __attribute__((ms_abi))
command_list_set_graphics_root_uav(BeerD3D12GraphicsCommandList *self,
                                   uint32_t root_parameter, uint64_t address);
static void __attribute__((ms_abi))
command_list_set_index_buffer(BeerD3D12GraphicsCommandList *self,
                              const void *view);
static void __attribute__((ms_abi))
command_list_set_vertex_buffers(BeerD3D12GraphicsCommandList *self,
                                uint32_t start_slot, uint32_t count,
                                const void *views);
static void apply_depth_stencil_clear(BeerD3D12Resource *resource,
                                      uint32_t flags, uint32_t depth_bits,
                                      uint8_t stencil, uint32_t rect_count,
                                      const BeerD3D12Rect *rects);
static void apply_render_target_clear(BeerD3D12Resource *resource,
                                      const uint32_t color_bits[4],
                                      uint32_t rect_count,
                                      const BeerD3D12Rect *rects);
static void __attribute__((ms_abi))
command_list_clear_depth_stencil_view(BeerD3D12GraphicsCommandList *self,
                                      uint64_t descriptor, uint32_t flags,
                                      float depth, uint8_t stencil,
                                      uint32_t rect_count, const void *rects);
static void __attribute__((ms_abi))
command_list_clear_render_target_view(BeerD3D12GraphicsCommandList *self,
                                      uint64_t descriptor,
                                      const float *color, uint32_t rect_count,
                                      const void *rects);
static void __attribute__((ms_abi))
command_list_clear_unordered_access_view_uint(
    BeerD3D12GraphicsCommandList *self, uint64_t gpu_descriptor,
    uint64_t cpu_descriptor, BeerD3D12Resource *resource,
    const uint32_t values[4], uint32_t rect_count, const void *rects);
static void __attribute__((ms_abi))
command_list_discard_resource(BeerD3D12GraphicsCommandList *self,
                              BeerD3D12Resource *resource,
                              const void *region);
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
static void pipeline_state_copy_input_layout(BeerD3D12PipelineState *object,
                                             size_t payload_offset);
static void pipeline_state_parse_legacy_state(BeerD3D12PipelineState *object);
static void pipeline_state_parse_stream_state(BeerD3D12PipelineState *object);

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
    [11] = command_list_clear_state,
    [12] = command_list_draw_instanced,
    [13] = command_list_draw_indexed_instanced,
    [14] = command_list_dispatch,
    [15] = command_list_copy_buffer_region,
    [16] = command_list_copy_texture_region,
    [17] = command_list_copy_resource,
    [18] = command_list_unsupported_18,
    [19] = command_list_unsupported_19,
    [20] = command_list_set_primitive_topology,
    [21] = command_list_set_viewports,
    [22] = command_list_set_scissor_rects,
    [23] = command_list_set_blend_factor,
    [24] = command_list_set_stencil_reference,
    [25] = command_list_set_pipeline_state,
    [26] = command_list_resource_barrier,
    [27] = command_list_unsupported_27,
    [28] = command_list_set_descriptor_heaps,
    [29] = command_list_set_compute_root_signature,
    [30] = command_list_set_graphics_root_signature,
    [31] = command_list_set_compute_root_descriptor_table,
    [32] = command_list_set_graphics_root_descriptor_table,
    [33] = command_list_set_compute_root_constant,
    [34] = command_list_set_graphics_root_constant,
    [35] = command_list_set_compute_root_constants,
    [36] = command_list_set_graphics_root_constants,
    [37] = command_list_set_compute_root_cbv,
    [38] = command_list_set_graphics_root_cbv,
    [39] = command_list_set_compute_root_srv,
    [40] = command_list_set_graphics_root_srv,
    [41] = command_list_set_compute_root_uav,
    [42] = command_list_set_graphics_root_uav,
    [43] = command_list_set_index_buffer,
    [44] = command_list_set_vertex_buffers,
    [45] = command_list_unsupported_45,
    [46] = command_list_set_render_targets,
    [47] = command_list_clear_depth_stencil_view,
    [48] = command_list_clear_render_target_view,
    [49] = command_list_clear_unordered_access_view_uint,
    [50] = command_list_unsupported_50,
    [51] = command_list_discard_resource,
    [52] = command_list_unsupported_52,
    [53] = command_list_end_query,
    [54] = command_list_resolve_query_data,
    [55] = command_list_unsupported_55,
    [56] = command_list_unsupported_56,
    [57] = command_list_unsupported_57,
    [58] = command_list_unsupported_58,
    [59] = command_list_execute_indirect,
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
    [13] = resource_read_from_subresource,
    [14] = resource_write_to_subresource,
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
    if (description) {
        uint8_t *private_descriptor =
            (uint8_t *)(uintptr_t)destination;
        memcpy(private_descriptor + 8, description, 24);
        uint32_t view_dimension = 0;
        memcpy(&view_dimension, (const uint8_t *)description + 4, 4);
        if (view_dimension == 1) {
            /* A native buffer SRV's union starts at byte 16 and extends beyond
             * Beer's fixed descriptor slot. Repack the complete buffer view
             * (FirstElement, NumElements, StructureByteStride) into +16..+31;
             * Shader4ComponentMapping is irrelevant to buffer addressing. */
            memcpy(private_descriptor + 16,
                   (const uint8_t *)description + 16, 16);
        } else if (view_dimension == 3 || view_dimension == 5 ||
                   view_dimension == 10) {
            /* D3D12_SRV_DIMENSION_TEXTURE1DARRAY/TEXTURE2DARRAY/
             * TEXTURECUBEARRAY. The native SRV descriptor is larger than
             * Beer's fixed 32-byte descriptor slot. Preserve FirstArraySlice
             * and ArraySize, or First2DArrayFace and NumCubes, in words
             * occupied by Shader4ComponentMapping/padding after translation;
             * mip fields remain in private bytes 24/28. */
            memcpy(private_descriptor + 16,
                   (const uint8_t *)description + 24, 4);
            memcpy(private_descriptor + 20,
                   (const uint8_t *)description + 28, 4);
        }
    }
}

static void __attribute__((ms_abi))
device_create_unordered_access_view(BeerD3D12Device *self, void *resource,
                                    void *counter_resource,
                                    const void *description,
                                    uint64_t destination)
{
    (void)self;
    (void)counter_resource;
    if (!destination) return;
    /* Keep SRV and UAV descriptors in the same private 32-byte layout:
     * resource pointer followed by the first 24 bytes of the native view desc.
     * The previous UAV layout inserted the counter resource at +8 and moved the
     * view to +16, losing Texture2D::MipSlice and preventing subresource-aware
     * execution. Counter-resource metadata is not consumed by Beer yet. */
    memset((void *)(uintptr_t)destination, 0, 32);
    memcpy((void *)(uintptr_t)destination, &resource, sizeof(resource));
    if (description)
        memcpy((uint8_t *)(uintptr_t)destination + 8, description, 24);
}

static void write_resource_descriptor(void *resource, const void *description,
                                      uint64_t destination)
{
    if (!destination) return;
    memset((void *)(uintptr_t)destination, 0, 32);
    memcpy((void *)(uintptr_t)destination, &resource, sizeof(resource));
    if (description)
        memcpy((uint8_t *)(uintptr_t)destination + 8, description, 24);
    cpu_descriptor_registry_set(destination, resource);
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
    uint32_t compact[8] = {0};
    if (description) {
        const uint8_t *source = description;
        /* D3D12_SAMPLER_DESC has a 4-byte pad before MinLOD on Win64. The
         * descriptor heap only has 32 bytes, so retain fields required by the
         * Vulkan bridge and classify the four legal D3D border colors. */
        memcpy(compact, source, 24); /* Filter through MaxAnisotropy. */
        memcpy(&compact[6], source + 24, 4); /* ComparisonFunc. */
        float border[4];
        memcpy(border, source + 28, sizeof(border));
        if (border[0] == 1.0f && border[1] == 1.0f &&
            border[2] == 1.0f && border[3] == 1.0f)
            compact[7] = 2; /* opaque white */
        else if (border[3] == 1.0f)
            compact[7] = 1; /* opaque black */
    }
    memcpy((void *)(uintptr_t)destination, compact, sizeof(compact));
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
    for (uint32_t index = 0; index < count; ++index)
        cpu_descriptor_registry_copy(destination + (uint64_t)index * stride,
                                     source + (uint64_t)index * stride);
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
        for (uint32_t index = 0; index < copy_count; ++index)
            cpu_descriptor_registry_copy(
                destination + (uint64_t)index * stride,
                source + (uint64_t)index * stride);
        destination_offset += copy_count;
        source_offset += copy_count;
    }
}

static uint32_t d3d12_format_block_bytes(uint32_t format)
{
    switch (format) {
    case 70: case 71: case 72: case 79: case 80: case 81:
        return 8;
    case 73: case 74: case 75: case 76: case 77: case 78:
    case 82: case 83: case 84: case 94: case 95: case 96:
    case 97: case 98: case 99:
        return 16;
    default:
        return 0;
    }
}

static uint32_t d3d12_format_bytes_per_pixel(uint32_t format)
{
    /* DXGI_FORMAT numeric groups. Keep this table exact: it defines Beer's
     * authoritative texture storage layout, and a wider guessed texel makes
     * SRV row pitches disagree with the Vulkan view format. */
    switch (format) {
    case 2: case 3: case 4: /* R32G32B32A32 */
        return 16;
    case 5: case 6: case 7: case 8: /* R32G32B32 */
        return 12;
    case 9: case 10: case 11: case 12: case 13: case 14: /* R16G16B16A16 */
    case 15: case 16: case 17: case 18: /* R32G32 */
    case 19: case 20: case 21: case 22: /* R32G8X24 */
        return 8;
    case 23: case 24: case 25: case 26: /* packed 10/11-bit */
    case 27: case 28: case 29: case 30: case 31: case 32: /* R8G8B8A8 */
    case 33: case 34: case 35: case 36: case 37: case 38: /* R16G16 */
    case 39: case 40: case 41: case 42: case 43: case 44: case 45:
    case 46: case 47: /* R32 / D32 */
    case 68: case 69: /* shared-exp and packed 8-bit pairs */
    case 87: case 88: case 89: case 90: case 91: case 92: /* BGRA/BGRX */
        return 4;
    case 48: case 49: case 50: case 51: case 52: /* R8G8 */
    case 53: case 54: case 55: case 56: case 57: case 58: case 59: /* R16 */
    case 85: case 86: /* B5G6R5 / B5G5R5A1 */
        return 2;
    case 60: case 61: case 62: case 63: case 64: case 65: case 66: /* R8/A8 */
        return 1;
    case 67: /* R1_UNORM is bit-packed; no current title resource uses it. */
        return 0;
    default:
        /* Block-compressed formats are handled before this function. Unknown
         * uncompressed formats must not silently become four-byte textures. */
        return 0;
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
    uint32_t block_bytes = d3d12_format_block_bytes(description->format);
    uint32_t bytes_per_pixel = block_bytes ? 0 :
        d3d12_format_bytes_per_pixel(description->format);
    for (uint32_t index = 0; index < subresource_count; ++index) {
        uint32_t subresource = first_subresource + index;
        uint32_t mip = description->mip_levels
                           ? subresource % description->mip_levels : 0;
        uint64_t width = description->width >> mip;
        uint32_t height = description->height >> mip;
        if (!width) width = 1;
        if (!height) height = 1;
        uint64_t row_size = block_bytes ? ((width + 3u) / 4u) * block_bytes :
            width * bytes_per_pixel;
        uint32_t rows = block_bytes ? (height + 3u) / 4u : height;
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
        if (row_counts) row_counts[index] = rows;
        if (row_sizes) row_sizes[index] = row_size;
        uint32_t depth = description->dimension == 4
                             ? description->depth_or_array_size >> mip : 1;
        if (!depth) depth = 1;
        if ((uint64_t)row_pitch > UINT64_MAX / rows ||
            (uint64_t)row_pitch * rows > UINT64_MAX / depth) {
            if (total_bytes) *total_bytes = UINT64_MAX;
            return;
        }
        cursor += (uint64_t)row_pitch * rows * depth;
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
    D3D12_TRACE("[D3D12] CreateCommandQueue type=%u priority=%u flags=0x%x -> %p\n",
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
    D3D12_TRACE("[D3D12] CreateDescriptorHeap type=%u count=%u flags=0x%x -> %p\n",
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
    D3D12_TRACE("[D3D12] CreateCommandAllocator type=%u -> %p\n",
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
    object->draw_tail = &object->draws;
    object->depth_clear_tail = &object->depth_clears;
    object->color_clear_tail = &object->color_clears;
    object->resource_copy_tail = &object->resource_copies;
    object->texture_copy_tail = &object->texture_copies;
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
    D3D12_TRACE("[D3D12] CreateCommandList type=%u -> %p\n",
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
    D3D12_TRACE("[D3D12] CreateFence initial=%llu -> %p\n",
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
    D3D12_TRACE("[D3D12] CreateQueryHeap type=%u count=%u node=%u -> %p\n",
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

    /* D3D12_INDIRECT_ARGUMENT_DESC is 16 bytes: the four-byte type followed
     * by its largest twelve-byte union member (root constants). */
    size_t arguments_size = (size_t)argument_count * 16u;
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

static uint32_t d3d12_format_bytes_per_pixel(uint32_t format);

static int resource_texture_layout(const BeerD3D12ResourceDesc *description,
                                   uint32_t subresource, size_t *offset,
                                   size_t *row_pitch, uint32_t *width,
                                   uint32_t *height)
{
    if (!description || description->dimension == 1) return 0;
    uint32_t mip_levels = description->mip_levels ? description->mip_levels : 1;
    uint32_t array_size = description->depth_or_array_size ?
        description->depth_or_array_size : 1;
    if (description->dimension == 4) array_size = 1;
    if (subresource >= mip_levels * array_size) return 0;

    uint32_t block_bytes = d3d12_format_block_bytes(description->format);
    uint32_t bytes_per_pixel = block_bytes ? 0 :
        d3d12_format_bytes_per_pixel(description->format);
    size_t cursor = 0;
    for (uint32_t index = 0; index <= subresource; ++index) {
        uint32_t mip = index % mip_levels;
        uint64_t mip_width64 = description->width >> mip;
        uint32_t mip_height = description->height >> mip;
        uint32_t mip_depth = description->dimension == 4 ?
            description->depth_or_array_size >> mip : 1;
        if (!mip_width64) mip_width64 = 1;
        if (!mip_height) mip_height = 1;
        if (!mip_depth) mip_depth = 1;
        uint64_t row_units = block_bytes ? (mip_width64 + 3u) / 4u :
            mip_width64;
        uint32_t rows = block_bytes ? (mip_height + 3u) / 4u : mip_height;
        uint32_t unit_bytes = block_bytes ? block_bytes : bytes_per_pixel;
        if (mip_width64 > UINT32_MAX || row_units > SIZE_MAX / unit_bytes)
            return 0;
        size_t mip_row_pitch = (size_t)row_units * unit_bytes;
        if ((size_t)rows > SIZE_MAX / mip_row_pitch ||
            mip_row_pitch * rows > SIZE_MAX / mip_depth ||
            cursor > SIZE_MAX - mip_row_pitch * rows * mip_depth)
            return 0;
        if (index == subresource) {
            if (offset) *offset = cursor;
            if (row_pitch) *row_pitch = mip_row_pitch;
            if (width) *width = (uint32_t)mip_width64;
            if (height) *height = mip_height;
            return 1;
        }
        cursor += mip_row_pitch * rows * mip_depth;
    }
    return 0;
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
            uint32_t mip_levels = description->mip_levels ?
                description->mip_levels : 1;
            uint32_t layers = description->dimension == 4 ? 1u :
                (description->depth_or_array_size ?
                 description->depth_or_array_size : 1u);
            size_t last_offset = 0, last_row_pitch = 0;
            uint32_t last_width = 0, last_height = 0;
            uint32_t last_subresource = mip_levels * layers - 1;
            if (!resource_texture_layout(description, last_subresource,
                                         &last_offset, &last_row_pitch,
                                         &last_width, &last_height)) {
                result->size_in_bytes = UINT64_MAX;
                return 0;
            }
            uint32_t last_depth = description->dimension == 4 ?
                description->depth_or_array_size >> last_subresource : 1;
            if (!last_depth) last_depth = 1;
            uint32_t block_bytes =
                d3d12_format_block_bytes(description->format);
            uint32_t last_rows = block_bytes ?
                (last_height + 3u) / 4u : last_height;
            if ((size_t)last_rows > SIZE_MAX / last_row_pitch ||
                last_row_pitch * last_rows > SIZE_MAX / last_depth ||
                last_offset > SIZE_MAX -
                    last_row_pitch * last_rows * last_depth) {
                result->size_in_bytes = UINT64_MAX;
                return 0;
            }
            resource_size = last_offset +
                last_row_pitch * last_rows * last_depth;
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
    D3D12_TRACE(
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
    /* DEFAULT resources are GPU-authoritative and may be very large. Reserve a
     * zero-filled shadow lazily instead of touching every page at creation.
     * Upload/readback heaps remain ordinary allocations because callers map and
     * populate them immediately. */
    const int lazy_default_storage = heap_properties[0] == 1 &&
        allocation.size_in_bytes >= UINT64_C(4) * 1024 * 1024;
    if (lazy_default_storage) {
        object->storage = mmap(NULL, (size_t)allocation.size_in_bytes,
                               PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                               -1, 0);
        if (object->storage == MAP_FAILED) object->storage = NULL;
    } else {
        object->storage = calloc(1, (size_t)allocation.size_in_bytes);
    }
    if (!object->storage) {
        free(object);
        return BEER_E_OUTOFMEMORY;
    }
    backing_account_allocate((size_t)allocation.size_in_bytes);
    object->vtable = g_resource_vtable;
    object->references = 1;
    object->device = self;
    object->description = *description;
    object->state = initial_state;
    object->storage_size = (size_t)allocation.size_in_bytes;
    object->owns_storage = 1;
    object->storage_is_mapping = lazy_default_storage;
    object->gpu_virtual_address = description->dimension == 1
        ? (uint64_t)(uintptr_t)object->storage : 0;
    memcpy(object->heap_properties, heap_properties,
           sizeof(object->heap_properties));
    object->heap_flags = heap_flags;
    device_add_ref(self);
    resource_registry_add(object);
    *resource = object;
    D3D12_TRACE(
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
    /* D3D12 heaps are virtual address reservations. Committing and zeroing the
     * complete reservation here made multi-GiB DEFAULT heaps immediately
     * resident even when only a small subset of placed resources was touched.
     * Anonymous mmap provides the same zero-filled CPU shadow lazily: Linux
     * commits pages on first write and can discard untouched pages under
     * pressure. The requested heap alignment is preserved for placed-resource
     * offsets because D3D12 heap sizes/alignments are page multiples. */
    object->storage = mmap(NULL, (size_t)storage_size,
                           PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                           -1, 0);
    if (object->storage == MAP_FAILED) {
        object->storage = NULL;
        free(object);
        return BEER_E_OUTOFMEMORY;
    }
    object->storage_size = (size_t)storage_size;
    object->storage_is_mapping = 1;
    backing_account_allocate((size_t)storage_size);
    object->vtable = g_heap_vtable;
    object->references = 1;
    object->device = self;
    object->description = description;
    object->description.alignment = alignment;
    device_add_ref(self);
    *heap = object;
    D3D12_TRACE("[D3D12] CreateHeap size=%llu align=%llu flags=0x%x -> %p\n",
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
    resource_registry_add(object);
    *resource = object;
    D3D12_TRACE(
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
        resource_registry_remove(self);
        cpu_descriptor_registry_forget_resource(self);
        /* Persistent Vulkan mirrors can retain this identity and its host
         * backing for deferred writeback. Flush/invalidate them before the
         * authoritative storage is released. */
        if (g_forget_resource) g_forget_resource(self);
        if (self->owns_storage) {
            backing_account_release(self->storage_size);
            if (self->storage_is_mapping)
                munmap(self->storage, self->storage_size);
            else
                free(self->storage);
        }
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
    self->cpu_map = self->storage;
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

static uint64_t resource_copy_subresource_rows(
    BeerD3D12Resource *self, uint32_t subresource, const void *box_ptr,
    uint8_t *external, uint32_t external_row_pitch,
    uint32_t external_depth_pitch, int write)
{
    if (!self || !external || self->description.dimension == 1)
        return BEER_E_INVALIDARG;
    size_t texture_offset = 0, texture_row_pitch = 0;
    uint32_t texture_width = 0, texture_height = 0;
    if (!resource_texture_layout(&self->description, subresource,
                                 &texture_offset, &texture_row_pitch,
                                 &texture_width, &texture_height))
        return BEER_E_INVALIDARG;

    uint32_t block_bytes = d3d12_format_block_bytes(self->description.format);
    uint32_t unit_bytes = block_bytes ? block_bytes :
        d3d12_format_bytes_per_pixel(self->description.format);
    uint32_t block_width = block_bytes ? 4u : 1u;
    uint32_t block_height = block_bytes ? 4u : 1u;
    uint32_t mip_levels = self->description.mip_levels ?
        self->description.mip_levels : 1u;
    uint32_t mip = subresource % mip_levels;
    uint32_t texture_depth = self->description.dimension == 4 ?
        self->description.depth_or_array_size >> mip : 1u;
    if (!texture_depth) texture_depth = 1;

    uint32_t left = 0, top = 0, front = 0;
    uint32_t right = texture_width, bottom = texture_height, back = texture_depth;
    if (box_ptr) {
        const uint32_t *box = box_ptr;
        left = box[0]; top = box[1]; front = box[2];
        right = box[3]; bottom = box[4]; back = box[5];
    }
    if (!unit_bytes || right <= left || bottom <= top || back <= front ||
        right > texture_width || bottom > texture_height || back > texture_depth ||
        left % block_width || top % block_height ||
        (block_bytes && right != texture_width && right % block_width) ||
        (block_bytes && bottom != texture_height && bottom % block_height))
        return BEER_E_INVALIDARG;

    uint32_t columns = (right - left + block_width - 1u) / block_width;
    uint32_t rows = (bottom - top + block_height - 1u) / block_height;
    size_t row_bytes = (size_t)columns * unit_bytes;
    size_t texture_rows = (texture_height + block_height - 1u) / block_height;
    if (!external_row_pitch) external_row_pitch = (uint32_t)row_bytes;
    if (!external_depth_pitch) {
        if ((size_t)external_row_pitch > UINT32_MAX / rows)
            return BEER_E_INVALIDARG;
        external_depth_pitch = external_row_pitch * rows;
    }
    if (external_row_pitch < row_bytes ||
        (size_t)external_row_pitch > external_depth_pitch ||
        rows > external_depth_pitch / external_row_pitch)
        return BEER_E_INVALIDARG;
    size_t texture_slice_pitch = texture_row_pitch * texture_rows;
    uint32_t unit_left = left / block_width;
    uint32_t unit_top = top / block_height;
    for (uint32_t z = 0; z < back - front; ++z) {
        for (uint32_t row = 0; row < rows; ++row) {
            size_t texture_byte = texture_offset +
                (size_t)(front + z) * texture_slice_pitch +
                (size_t)(unit_top + row) * texture_row_pitch +
                (size_t)unit_left * unit_bytes;
            size_t external_byte = (size_t)z * external_depth_pitch +
                (size_t)row * external_row_pitch;
            if (texture_byte > self->storage_size ||
                row_bytes > self->storage_size - texture_byte)
                return BEER_E_INVALIDARG;
            if (write)
                memmove(self->storage + texture_byte,
                        external + external_byte, row_bytes);
            else
                memmove(external + external_byte,
                        self->storage + texture_byte, row_bytes);
        }
    }
    if (write) {
        if (g_end_compute_batch) g_end_compute_batch();
        if (g_forget_resource) g_forget_resource(self);
        ++self->content_serial;
    }
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
resource_read_from_subresource(BeerD3D12Resource *self, void *destination,
                               uint32_t destination_row_pitch,
                               uint32_t destination_depth_pitch,
                               uint32_t source_subresource,
                               const void *source_box)
{
    if (g_end_compute_batch) g_end_compute_batch();
    if (g_forget_resource && self) g_forget_resource(self);
    return resource_copy_subresource_rows(
        self, source_subresource, source_box, destination,
        destination_row_pitch, destination_depth_pitch, 0);
}

static uint64_t __attribute__((ms_abi))
resource_write_to_subresource(BeerD3D12Resource *self,
                              uint32_t destination_subresource,
                              const void *destination_box, const void *source,
                              uint32_t source_row_pitch,
                              uint32_t source_depth_pitch)
{
    return resource_copy_subresource_rows(
        self, destination_subresource, destination_box,
        (uint8_t *)(uintptr_t)source, source_row_pitch, source_depth_pitch, 1);
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
        backing_account_release(self->storage_size);
        if (self->storage_is_mapping)
            munmap(self->storage, self->storage_size);
        else
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

    /* Present is a read of the application-owned back buffer, not a resource
     * write. Advancing the serial here made GPU mirror lookup miss the frame
     * just rendered at the previous serial, so the presenter fell back to the
     * stale CPU backing. That alternated valid gray frames with old/partially
     * initialized pixels as RE8's title workload moved onto retained mirrors. */
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
static void refresh_graphics_cbvs(BeerD3D12RecordedDraw *draw)
{
    if (!draw || getenv("BEER_D3D12_GRAPHICS_CBV_SNAPSHOT")) return;
    for (uint32_t parameter = 0;
         parameter < BEER_D3D12_MAX_ROOT_PARAMETERS; ++parameter) {
        BeerD3D12ComputeBinding *binding = &draw->state.bindings[parameter];
        size_t root_size = draw->root_binding_copy_sizes[parameter];
        if (root_size && draw->root_binding_copies[parameter] &&
            binding->kind == 3 && binding->buffer_location) {
            const uint8_t *live = NULL;
            BeerD3D12Resource *resource = resource_registry_resolve_buffer(
                binding->buffer_location, root_size, &live);
            if (resource) {
                if (live)
                    memcpy(draw->root_binding_copies[parameter], live,
                           root_size);
                resource_release(resource);
            }
        }
        for (uint32_t descriptor = 0;
             descriptor < binding->descriptor_count; ++descriptor) {
            size_t size = draw->descriptor_copy_sizes[parameter][descriptor];
            if (!size || !draw->descriptor_copies[parameter][descriptor] ||
                binding->descriptor_types[descriptor] != 2 ||
                !binding->descriptor_buffer_locations[descriptor])
                continue;
            const uint8_t *live = NULL;
            BeerD3D12Resource *resource = resource_registry_resolve_buffer(
                binding->descriptor_buffer_locations[descriptor], size, &live);
            if (resource) {
                if (live)
                    memcpy(draw->descriptor_copies[parameter][descriptor], live,
                           size);
                resource_release(resource);
            }
        }
    }
}

/* ExecuteIndirect arguments, counts, and geometry are all consumed in queue
 * execution order. The argument/count buffers were already synchronized here,
 * but retaining record-time host pointers for default-heap VB/IB resources
 * allowed a GPU producer earlier in the submission to remain only in a Vulkan
 * mirror. Publish each distinct generated geometry resource before expanding
 * the indirect commands. Upload/readback heaps use immutable draw snapshots and
 * need no replay-time synchronization. */
int beer_d3d12_graphics_attachments_match(
    const BeerD3D12GraphicsDraw *current,
    const BeerD3D12GraphicsDraw *next)
{
    if (!current || !next || !current->render_target_count ||
        current->render_target_count != next->render_target_count ||
        current->depth_stencil != next->depth_stencil)
        return 0;
    for (uint32_t target = 0; target < current->render_target_count; ++target)
        if (current->render_targets[target] != next->render_targets[target] ||
            current->render_target_format[target] !=
                next->render_target_format[target] ||
            current->render_target_width[target] !=
                next->render_target_width[target] ||
            current->render_target_height[target] !=
                next->render_target_height[target])
            return 0;
    return 1;
}

int beer_d3d12_graphics_publication_deferral_enabled(void)
{
    const char *eager = getenv("BEER_D3D12_EAGER_GRAPHICS_PUBLICATION");
    return !eager || !*eager || !strcmp(eager, "0");
}

static int draw_can_defer_to_next(
    const BeerD3D12RecordedDraw *draw,
    const BeerD3D12RecordedDispatch *dispatch,
    const BeerD3D12RecordedDepthClear *clear,
    const BeerD3D12RecordedColorClear *color_clear,
    const BeerD3D12RecordedResourceCopy *resource_copy,
    const BeerD3D12RecordedTextureCopy *texture_copy)
{
    if (!beer_d3d12_graphics_publication_deferral_enabled()) return 0;
    const BeerD3D12RecordedDraw *next = draw ? draw->next : NULL;
    if (!next || !beer_d3d12_graphics_attachments_match(
                     &draw->state, &next->state))
        return 0;
    const uint64_t sequence = next->sequence;
    return (!dispatch || sequence < dispatch->sequence) &&
        (!clear || sequence < clear->sequence) &&
        (!color_clear || sequence < color_clear->sequence) &&
        (!resource_copy || sequence < resource_copy->sequence) &&
        (!texture_copy || sequence < texture_copy->sequence);
}

static int graphics_binding_state_matches(
    const BeerD3D12ComputeBinding *a, const BeerD3D12ComputeBinding *b)
{
    if (a->descriptor_table != b->descriptor_table ||
        a->buffer_location != b->buffer_location ||
        a->resource != b->resource ||
        a->resource_heap_type != b->resource_heap_type ||
        a->resolved_root_resource != b->resolved_root_resource ||
        a->constant_count != b->constant_count || a->kind != b->kind ||
        a->shader_visibility != b->shader_visibility ||
        a->descriptor_count != b->descriptor_count ||
        memcmp(a->constants, b->constants,
               (size_t)a->constant_count * sizeof(a->constants[0])))
        return 0;
    for (uint32_t i = 0; i < a->descriptor_count; ++i) {
#define SAME_FIELD(field) (a->field[i] == b->field[i])
        if (!SAME_FIELD(descriptor_resources) ||
            !SAME_FIELD(descriptor_resource_serial_locations) ||
            !SAME_FIELD(descriptor_types) ||
            !SAME_FIELD(descriptor_registers) ||
            !SAME_FIELD(descriptor_spaces) ||
            !SAME_FIELD(descriptor_handles) ||
            !SAME_FIELD(descriptor_buffer_locations) ||
            !SAME_FIELD(descriptor_resource_gpu_bases) ||
            !SAME_FIELD(descriptor_resource_total_sizes) ||
            !SAME_FIELD(descriptor_resource_heap_types) ||
            !SAME_FIELD(descriptor_widths) || !SAME_FIELD(descriptor_heights) ||
            !SAME_FIELD(descriptor_formats) ||
            !SAME_FIELD(descriptor_subresources) ||
            !SAME_FIELD(descriptor_subresource_offsets) ||
            !SAME_FIELD(descriptor_row_pitches) ||
            !SAME_FIELD(descriptor_view_formats) ||
            !SAME_FIELD(descriptor_view_first_elements) ||
            !SAME_FIELD(descriptor_view_element_counts) ||
            !SAME_FIELD(descriptor_view_structure_strides) ||
            !SAME_FIELD(descriptor_view_dimensions) ||
            !SAME_FIELD(descriptor_view_mip_levels) ||
            !SAME_FIELD(descriptor_view_first_layers) ||
            !SAME_FIELD(descriptor_view_layer_counts) ||
            !SAME_FIELD(descriptor_resource_dimensions) ||
            !SAME_FIELD(descriptor_resource_mips) ||
            !SAME_FIELD(descriptor_resource_layers) ||
            !SAME_FIELD(descriptor_depths) ||
            !SAME_FIELD(descriptor_resource_samples) ||
            a->descriptor_raw_sizes[i] != b->descriptor_raw_sizes[i] ||
            memcmp(a->descriptor_raw_bytes[i], b->descriptor_raw_bytes[i],
                   a->descriptor_raw_sizes[i]) ||
            memcmp(a->descriptor_samplers[i], b->descriptor_samplers[i],
                   sizeof(a->descriptor_samplers[i])))
            return 0;
#undef SAME_FIELD
    }
    return 1;
}

/* Adjacent ExecuteIndirect records can share one Vulkan render pass/submission
 * only when every immutable D3D12 input is semantically identical. Snapshot
 * storage pointers intentionally are not compared: separate command records may
 * own equivalent copies of the same descriptor/CBV state. */
static int indirect_draw_state_matches(const BeerD3D12RecordedDraw *a,
                                       const BeerD3D12RecordedDraw *b)
{
    if (!a || !b || !a->indirect_arguments || !b->indirect_arguments)
        return 0;
    const BeerD3D12GraphicsDraw *x = &a->state, *y = &b->state;
    if (x->pipeline_state != y->pipeline_state ||
        x->root_signature != y->root_signature ||
        x->vertex_shader_hash != y->vertex_shader_hash ||
        x->pixel_shader_hash != y->pixel_shader_hash ||
        x->vertex_shader_size != y->vertex_shader_size ||
        x->pixel_shader_size != y->pixel_shader_size ||
        x->descriptor_heap_count != y->descriptor_heap_count ||
        x->render_target_count != y->render_target_count ||
        x->depth_stencil != y->depth_stencil ||
        x->depth_stencil_width != y->depth_stencil_width ||
        x->depth_stencil_height != y->depth_stencil_height ||
        x->depth_stencil_format != y->depth_stencil_format ||
        x->depth_stencil_source_bytes_per_pixel !=
            y->depth_stencil_source_bytes_per_pixel ||
        x->input_element_count != y->input_element_count ||
        x->vertex_buffer_mask != y->vertex_buffer_mask ||
        x->index_buffer_resource != y->index_buffer_resource ||
        memcmp(&x->index_buffer, &y->index_buffer, sizeof(x->index_buffer)) ||
        memcmp(x->viewport, y->viewport, sizeof(x->viewport)) ||
        memcmp(x->scissor, y->scissor, sizeof(x->scissor)) ||
        memcmp(x->blend_factor, y->blend_factor, sizeof(x->blend_factor)) ||
        x->primitive_topology != y->primitive_topology ||
        x->stencil_reference != y->stencil_reference ||
        x->indexed != y->indexed || memcmp(&x->pso, &y->pso, sizeof(x->pso)) ||
        memcmp(x->input_elements, y->input_elements,
               (size_t)x->input_element_count * sizeof(x->input_elements[0])))
        return 0;
    for (uint32_t i = 0; i < x->descriptor_heap_count; ++i)
        if (x->descriptor_heaps[i] != y->descriptor_heaps[i] ||
            x->descriptor_heap_gpu_start[i] != y->descriptor_heap_gpu_start[i] ||
            x->descriptor_heap_type[i] != y->descriptor_heap_type[i] ||
            x->descriptor_heap_increment[i] != y->descriptor_heap_increment[i])
            return 0;
    for (uint32_t i = 0; i < x->render_target_count; ++i)
        if (x->render_targets[i] != y->render_targets[i] ||
            x->render_target_serial_location[i] !=
                y->render_target_serial_location[i] ||
            x->render_target_format[i] != y->render_target_format[i] ||
            x->render_target_width[i] != y->render_target_width[i] ||
            x->render_target_height[i] != y->render_target_height[i])
            return 0;
    for (uint32_t i = 0; i < BEER_D3D12_MAX_VERTEX_BUFFERS; ++i)
        if (x->vertex_buffer_resources[i] != y->vertex_buffer_resources[i] ||
            memcmp(&x->vertex_buffers[i], &y->vertex_buffers[i],
                   sizeof(x->vertex_buffers[i])))
            return 0;
    for (uint32_t i = 0; i < BEER_D3D12_MAX_ROOT_PARAMETERS; ++i)
        if (!graphics_binding_state_matches(&x->bindings[i], &y->bindings[i]))
            return 0;
    return 1;
}

#define BEER_D3D12_MAX_SYNCED_GRAPHICS_RESOURCES 64

static int synchronize_resource_once(
    BeerD3D12Resource *resource,
    const BeerD3D12Resource **synchronized,
    uint32_t *synchronized_count)
{
    if (!resource || (!g_forget_source_resource && !g_forget_resource) ||
        !synchronized || !synchronized_count)
        return 0;
    for (uint32_t i = 0; i < *synchronized_count; ++i)
        if (synchronized[i] == resource) return 1;
    /* ExecuteIndirect only reads argument/count/geometry resources. Publish a
     * pending GPU producer, but preserve serial-valid graphics mirrors. Full
     * invalidation here destroyed the same VB/IB mirrors on every generated
     * scene record and forced avoidable re-uploads. */
    if (g_forget_source_resource)
        g_forget_source_resource(resource);
    else
        g_forget_resource(resource);
    if (*synchronized_count < BEER_D3D12_MAX_SYNCED_GRAPHICS_RESOURCES)
        synchronized[(*synchronized_count)++] = resource;
    return 1;
}

static void synchronize_indirect_geometry(
    BeerD3D12RecordedDraw *draw,
    const BeerD3D12Resource **synchronized,
    uint32_t *synchronized_count)
{
    if (!draw || !g_forget_resource) return;
    for (uint32_t slot = 0; slot < BEER_D3D12_MAX_VERTEX_BUFFERS; ++slot) {
        BeerD3D12Resource *resource = draw->vertex_buffer_resources[slot];
        if (!resource || resource->heap_properties[0] == 2 ||
            resource->heap_properties[0] == 3)
            continue;
        (void)synchronize_resource_once(resource, synchronized,
                                        synchronized_count);
    }
    BeerD3D12Resource *index = draw->index_buffer_resource;
    if (!index || index->heap_properties[0] == 2 ||
        index->heap_properties[0] == 3)
        return;
    (void)synchronize_resource_once(index, synchronized, synchronized_count);
}

static int append_indirect_graphics_arguments(
    const BeerD3D12RecordedDraw *draw, BeerD3D12GraphicsArguments **arguments,
    uint32_t *argument_count, uint32_t *argument_capacity)
{
    if (!draw || !draw->indirect_arguments || !arguments || !argument_count ||
        !argument_capacity)
        return 0;
    uint32_t count = draw->indirect_maximum;
    if (draw->indirect_count) {
        uint32_t requested = 0;
        if (draw->indirect_count_offset > draw->indirect_count->storage_size ||
            draw->indirect_count->storage_size -
                (size_t)draw->indirect_count_offset < sizeof(requested))
            return 0;
        memcpy(&requested, draw->indirect_count->storage +
                   (size_t)draw->indirect_count_offset, sizeof(requested));
        if (requested < count) count = requested;
    }
    const size_t payload = draw->state.indexed ? 20u : 16u;
    for (uint32_t command = 0; command < count; ++command) {
        if (draw->indirect_stride && command >
                (SIZE_MAX - (size_t)draw->indirect_argument_offset) /
                    draw->indirect_stride)
            break;
        size_t offset = (size_t)draw->indirect_argument_offset +
            (size_t)command * draw->indirect_stride;
        if (offset > draw->indirect_arguments->storage_size ||
            draw->indirect_arguments->storage_size - offset < payload)
            break;
        uint32_t values[5] = {0};
        memcpy(values, draw->indirect_arguments->storage + offset, payload);
        if (!values[0] || !values[1]) continue;
        if (*argument_count == *argument_capacity) {
            uint32_t capacity = *argument_capacity ? *argument_capacity * 2u : 16u;
            if (capacity < *argument_capacity) return 0;
            void *grown = realloc(*arguments,
                                  (size_t)capacity * sizeof(**arguments));
            if (!grown) return 0;
            *arguments = grown;
            *argument_capacity = capacity;
        }
        BeerD3D12GraphicsArguments *output =
            &(*arguments)[(*argument_count)++];
        *output = (BeerD3D12GraphicsArguments){
            .element_count = values[0],
            .instance_count = values[1],
            .start_element = values[2],
            .base_vertex = draw->state.indexed ? (int32_t)values[3] : 0,
            .start_instance = draw->state.indexed ? values[4] : values[3],
            .defer_render_target_publication = 1
        };
    }
    return 1;
}

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
    uint32_t executed_draws = 0;
    uint32_t rejected_draws = 0;
    static _Atomic(uint64_t) submission_serial;
    uint64_t submission = atomic_fetch_add(&submission_serial, 1) + 1;
    for (uint32_t index = 0; index < count; ++index) {
        BeerD3D12GraphicsCommandList *list = lists[index];
        if (!list || list->device != self->device || list->type != self->type)
            continue;
        ++valid_lists;
        if (!list->closed) ++open_lists;
        for (uint32_t operation = 0; operation < BEER_D3D12_OP_COUNT; ++operation)
            totals[operation] += list->operation_counts[operation];
        BeerD3D12RecordedDispatch *dispatch = list->dispatches;
        BeerD3D12RecordedDraw *draw = list->draws;
        BeerD3D12RecordedDepthClear *clear = list->depth_clears;
        BeerD3D12RecordedColorClear *color_clear = list->color_clears;
        BeerD3D12RecordedResourceCopy *resource_copy = list->resource_copies;
        BeerD3D12RecordedTextureCopy *texture_copy = list->texture_copies;
        uint32_t compute_ir_capacity =
            (uint32_t)list->operation_counts[BEER_D3D12_OP_DISPATCH];
        BeerD3D12ComputeCommand *compute_ir = compute_ir_capacity ?
            calloc(compute_ir_capacity, sizeof(*compute_ir)) : NULL;
        const uint32_t accesses_per_dispatch =
            BEER_D3D12_MAX_ROOT_PARAMETERS *
            (BEER_D3D12_MAX_RESOLVED_DESCRIPTORS + 1u);
        uint32_t compute_access_capacity = compute_ir_capacity <=
            UINT32_MAX / accesses_per_dispatch ?
            compute_ir_capacity * accesses_per_dispatch : 0;
        BeerD3D12ComputeResourceAccess *compute_accesses =
            compute_access_capacity ?
            calloc(compute_access_capacity, sizeof(*compute_accesses)) : NULL;
        uint8_t *compute_results = compute_ir_capacity ?
            calloc(compute_ir_capacity, sizeof(*compute_results)) : NULL;
        uint32_t compute_ir_count = 0;
        uint32_t compute_access_count = 0;
        const BeerD3D12Resource *synchronized_graphics_resources[
            BEER_D3D12_MAX_SYNCED_GRAPHICS_RESOURCES] = {0};
        uint32_t synchronized_graphics_resource_count = 0;
#define RESET_GRAPHICS_SYNC_CACHE() do { \
    synchronized_graphics_resource_count = 0; \
} while (0)
#define END_COMPUTE_BATCH() do { \
    if (compute_ir_count) { \
        memset(compute_results, 0, compute_ir_count); \
        if (g_begin_compute_batch) g_begin_compute_batch(); \
        if (g_execute_compute_batch) { \
            BeerD3D12ComputeCommandBatch compute_batch = { \
                .commands = compute_ir, \
                .command_count = compute_ir_count, \
                .accesses = compute_accesses, \
                .access_count = compute_access_count \
            }; \
            g_execute_compute_batch(&compute_batch, compute_results); \
        } else if (g_execute_compute) { \
            for (uint32_t compute_index = 0; \
                 compute_index < compute_ir_count; ++compute_index) \
                compute_results[compute_index] = (uint8_t) \
                    g_execute_compute(compute_ir[compute_index].dispatch); \
        } \
        if (g_end_compute_batch) g_end_compute_batch(); \
        for (uint32_t compute_index = 0; \
             compute_index < compute_ir_count; ++compute_index) { \
            if (compute_results[compute_index]) ++executed_dispatches; \
            else ++rejected_dispatches; \
        } \
        compute_ir_count = 0; \
        compute_access_count = 0; \
    } \
} while (0)
        while (dispatch || draw || clear || color_clear || resource_copy ||
               texture_copy) {
            uint64_t dispatch_sequence = dispatch ? dispatch->sequence : UINT64_MAX;
            uint64_t draw_sequence = draw ? draw->sequence : UINT64_MAX;
            uint64_t clear_sequence = clear ? clear->sequence : UINT64_MAX;
            uint64_t color_clear_sequence = color_clear
                ? color_clear->sequence : UINT64_MAX;
            uint64_t resource_copy_sequence = resource_copy
                ? resource_copy->sequence : UINT64_MAX;
            uint64_t texture_copy_sequence = texture_copy
                ? texture_copy->sequence : UINT64_MAX;
            if (texture_copy_sequence < dispatch_sequence &&
                texture_copy_sequence < draw_sequence &&
                texture_copy_sequence < clear_sequence &&
                texture_copy_sequence < color_clear_sequence &&
                texture_copy_sequence < resource_copy_sequence) {
                END_COMPUTE_BATCH();
                apply_texture_copy(texture_copy);
                texture_copy = texture_copy->next;
                RESET_GRAPHICS_SYNC_CACHE();
                continue;
            }
            if (resource_copy_sequence < dispatch_sequence &&
                resource_copy_sequence < draw_sequence &&
                resource_copy_sequence < clear_sequence &&
                resource_copy_sequence < color_clear_sequence &&
                resource_copy_sequence < texture_copy_sequence) {
                END_COMPUTE_BATCH();
                apply_resource_copy(resource_copy);
                resource_copy = resource_copy->next;
                RESET_GRAPHICS_SYNC_CACHE();
                continue;
            }
            if (color_clear_sequence < dispatch_sequence &&
                color_clear_sequence < draw_sequence &&
                color_clear_sequence < clear_sequence &&
                color_clear_sequence < resource_copy_sequence &&
                color_clear_sequence < texture_copy_sequence) {
                END_COMPUTE_BATCH();
                apply_render_target_clear(color_clear->resource,
                                          color_clear->color_bits,
                                          color_clear->rect_count,
                                          color_clear->rects);
                color_clear = color_clear->next;
                RESET_GRAPHICS_SYNC_CACHE();
                continue;
            }
            if (clear_sequence < dispatch_sequence &&
                clear_sequence < draw_sequence &&
                clear_sequence < resource_copy_sequence &&
                clear_sequence < texture_copy_sequence) {
                END_COMPUTE_BATCH();
                apply_depth_stencil_clear(clear->resource, clear->flags,
                                          clear->depth_bits, clear->stencil,
                                          clear->rect_count, clear->rects);
                clear = clear->next;
                RESET_GRAPHICS_SYNC_CACHE();
                continue;
            }
            if (dispatch_sequence < draw_sequence &&
                dispatch_sequence < resource_copy_sequence &&
                dispatch_sequence < texture_copy_sequence) {
                if (getenv("BEER_VULKAN_DRAW_PROVENANCE")) {
                    for (uint32_t parameter = 0;
                         parameter < BEER_D3D12_MAX_ROOT_PARAMETERS;
                         ++parameter) {
                        BeerD3D12ComputeBinding *binding =
                            &dispatch->state.bindings[parameter];
                        for (uint32_t descriptor = 0;
                             descriptor < binding->descriptor_count;
                             ++descriptor) {
                            if (binding->descriptor_types[descriptor] != 1 ||
                                !binding->descriptor_resources[descriptor])
                                continue;
                            for (uint32_t list_index = 0;
                                 list_index < count; ++list_index) {
                                BeerD3D12GraphicsCommandList *candidate_list =
                                    lists[list_index];
                                if (!candidate_list) continue;
                                for (BeerD3D12RecordedDraw *candidate =
                                         candidate_list->draws;
                                     candidate; candidate = candidate->next) {
                                    if (!candidate->state.indexed) continue;
                                    const void *resource =
                                        binding->descriptor_resources[descriptor];
                                    const char *role = NULL;
                                    if (resource ==
                                        candidate->state.index_buffer_resource)
                                        role = "index";
                                    for (uint32_t slot = 0;
                                         !role && slot <
                                             BEER_D3D12_MAX_VERTEX_BUFFERS;
                                         ++slot)
                                        if (resource == candidate->state.
                                                vertex_buffer_resources[slot])
                                            role = "vertex";
                                    if (role)
                                        fprintf(stderr,
                                            "[D3D12 DRAW INPUT PRODUCER] "
                                            "submission=%llu dispatch-list=%u "
                                            "dispatch-sequence=%llu "
                                            "shader=%016llx p=%u d=%u "
                                            "resource=%p role=%s "
                                            "draw-list=%u draw-sequence=%llu\n",
                                            (unsigned long long)submission,
                                            index,
                                            (unsigned long long)
                                                dispatch->sequence,
                                            (unsigned long long)
                                                dispatch->state.shader_hash,
                                            parameter, descriptor, resource,
                                            role, list_index,
                                            (unsigned long long)
                                                candidate->sequence);
                                }
                            }
                        }
                    }
                }
                dispatch->state.debug_submission = submission;
                dispatch->state.debug_sequence = dispatch->sequence;
                dispatch->state.debug_list_index = index;
                int indirect_enabled = 1;
                if (dispatch->indirect_arguments) {
                    /* Earlier UAV/copy work may own the newest argument bytes.
                     * Flush mirrors before reading GPU-generated indirect data. */
                    END_COMPUTE_BATCH();
                    if (g_forget_resource)
                        g_forget_resource(dispatch->indirect_arguments);
                    if (dispatch->indirect_count && g_forget_resource)
                        g_forget_resource(dispatch->indirect_count);
                    uint32_t count = dispatch->indirect_maximum;
                    if (dispatch->indirect_count) {
                        if (dispatch->indirect_count_offset >
                                dispatch->indirect_count->storage_size ||
                            dispatch->indirect_count->storage_size -
                                (size_t)dispatch->indirect_count_offset < 4) {
                            indirect_enabled = 0;
                        } else {
                            uint32_t requested = 0;
                            memcpy(&requested,
                                   dispatch->indirect_count->storage +
                                       (size_t)dispatch->indirect_count_offset,
                                   sizeof(requested));
                            if (requested < count) count = requested;
                        }
                    }
                    size_t offset = 0;
                    if (dispatch->indirect_index >
                            (SIZE_MAX - (size_t)dispatch->indirect_argument_offset) /
                                dispatch->indirect_stride) {
                        indirect_enabled = 0;
                    } else {
                        offset = (size_t)dispatch->indirect_argument_offset +
                            (size_t)dispatch->indirect_index *
                                dispatch->indirect_stride;
                    }
                    if (dispatch->indirect_index >= count ||
                        offset > dispatch->indirect_arguments->storage_size ||
                        dispatch->indirect_arguments->storage_size - offset < 12) {
                        indirect_enabled = 0;
                    } else {
                        uint32_t groups[3];
                        memcpy(groups,
                               dispatch->indirect_arguments->storage + offset,
                               sizeof(groups));
                        dispatch->state.group_count_x = groups[0];
                        dispatch->state.group_count_y = groups[1];
                        dispatch->state.group_count_z = groups[2];
                        if (!groups[0] || !groups[1] || !groups[2])
                            indirect_enabled = 0;
                    }
                }
                static int cbv_compare = -1, cbv_live = -1;
                if (cbv_compare < 0) {
                    cbv_compare = getenv("BEER_D3D12_CBV_COMPARE") != NULL;
                    cbv_live = getenv("BEER_D3D12_CBV_SNAPSHOT") == NULL;
                }
                if (cbv_compare || cbv_live) {
                    for (uint32_t p = 0; p < BEER_D3D12_MAX_ROOT_PARAMETERS; ++p) {
                        BeerD3D12ComputeBinding *b = &dispatch->state.bindings[p];
                        for (uint32_t d = 0; d < b->descriptor_count; ++d) {
                            size_t full = dispatch->descriptor_copy_sizes[p][d];
                            size_t n = full;
                            const uint8_t *live = NULL;
                            if (!n || !dispatch->descriptor_copies[p][d])
                                continue;
                            BeerD3D12Resource *live_resource =
                                resource_registry_resolve_buffer(
                                    b->descriptor_buffer_locations[d],
                                    full, &live);
                            if (!live_resource) continue;
                            /* D3D12 reads CBV bytes at GPU execution time, so
                             * writes made after recording but before
                             * ExecuteCommandLists are architecturally visible. */
                            if (cbv_live && live)
                                memcpy(dispatch->descriptor_copies[p][d], live,
                                       full);
                            resource_release(live_resource);
                            if (!cbv_compare || cbv_live || !live) continue;
                            if (n > 256) n = 256;
                            if (memcmp(live, dispatch->descriptor_copies[p][d], n)) {
                                uint32_t lv = 0, sv = 0;
                                if (n >= 108) {
                                    memcpy(&lv, live + 104, 4);
                                    memcpy(&sv, dispatch->descriptor_copies[p][d] + 104, 4);
                                }
                                fprintf(stderr,
                                        "[D3D12 CBV COMPARE] MISMATCH shader=%016llx "
                                        "root=%u desc=%u gpuva=%llx live[26]=%08x snap[26]=%08x\n",
                                        (unsigned long long)
                                            dispatch->state.shader_hash,
                                        p, d,
                                        (unsigned long long)
                                            b->descriptor_buffer_locations[d],
                                        lv, sv);
                            }
                        }
                    }
                }
                int queued = indirect_enabled && compute_ir &&
                    compute_accesses && compute_results &&
                    compute_ir_count < compute_ir_capacity;
                if (queued) {
                    BeerD3D12ComputeCommand *command =
                        &compute_ir[compute_ir_count++];
                    command->dispatch = &dispatch->state;
                    command->first_access = compute_access_count;
                    command->access_count = collect_compute_accesses(
                        &dispatch->state, compute_accesses + compute_access_count,
                        compute_access_capacity - compute_access_count);
                    command->hazards_before =
                        beer_d3d12_compute_hazards_before(
                            compute_accesses, command->first_access,
                            command->access_count);
                    compute_access_count += command->access_count;
                } else {
                    ++rejected_dispatches;
                }
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
                            dispatch->state.descriptor_heap_count,
                            queued != 0);
                dispatch = dispatch->next;
                RESET_GRAPHICS_SYNC_CACHE();
                continue;
            }
            END_COMPUTE_BATCH();
            draw->state.debug_submission = submission;
            draw->state.debug_sequence = draw->sequence;
            draw->state.debug_list_index = index;
            if (getenv("BEER_D3D12_TRACE_ORDERED_DRAWS")) {
                fprintf(stderr,
                        "[D3D12 ORDERED DRAW] submission=%llu list=%u "
                        "sequence=%llu vs=%016llx ps=%016llx indexed=%u "
                        "elements=%u target=%p size=%ux%u format=%u "
                        "depth=%p depth-size=%ux%u\n",
                        (unsigned long long)submission, index,
                        (unsigned long long)draw->sequence,
                        (unsigned long long)draw->state.vertex_shader_hash,
                        (unsigned long long)draw->state.pixel_shader_hash,
                        draw->state.indexed, draw->state.element_count,
                        draw->state.render_targets[0],
                        draw->state.render_target_width[0],
                        draw->state.render_target_height[0],
                        draw->state.render_target_format[0],
                        draw->state.depth_stencil,
                        draw->state.depth_stencil_width,
                        draw->state.depth_stencil_height);
            }
            refresh_graphics_cbvs(draw);
            if (draw->indirect_arguments) {
                /* Adjacent ExecuteIndirect records often contain one real draw
                 * each even though their complete immutable graphics state is
                 * identical. Merge that uninterrupted run into the existing
                 * Vulkan multi-draw backend. Any state change or non-graphics
                 * command remains a hard boundary. */
                BeerD3D12RecordedDraw *batch_first = draw;
                BeerD3D12RecordedDraw *batch_last = draw;
                BeerD3D12GraphicsArguments *graphics_arguments = NULL;
                uint32_t graphics_argument_count = 0;
                uint32_t graphics_argument_capacity = 0;
                for (;;) {
                    (void)synchronize_resource_once(
                        batch_last->indirect_arguments,
                        synchronized_graphics_resources,
                        &synchronized_graphics_resource_count);
                    if (batch_last->indirect_count)
                        (void)synchronize_resource_once(
                            batch_last->indirect_count,
                            synchronized_graphics_resources,
                            &synchronized_graphics_resource_count);
                    synchronize_indirect_geometry(
                        batch_last, synchronized_graphics_resources,
                        &synchronized_graphics_resource_count);
                    if (!append_indirect_graphics_arguments(
                            batch_last, &graphics_arguments,
                            &graphics_argument_count,
                            &graphics_argument_capacity))
                        break;

                    BeerD3D12RecordedDraw *candidate = batch_last->next;
                    if (!candidate ||
                        !indirect_draw_state_matches(batch_first, candidate))
                        break;
                    const uint64_t sequence = candidate->sequence;
                    if ((dispatch && dispatch->sequence < sequence) ||
                        (clear && clear->sequence < sequence) ||
                        (color_clear && color_clear->sequence < sequence) ||
                        (resource_copy && resource_copy->sequence < sequence) ||
                        (texture_copy && texture_copy->sequence < sequence))
                        break;
                    refresh_graphics_cbvs(candidate);
                    batch_last = candidate;
                }

                if (graphics_argument_count) {
                    graphics_arguments[graphics_argument_count - 1]
                        .defer_render_target_publication = draw_can_defer_to_next(
                            batch_last, dispatch, clear, color_clear,
                            resource_copy, texture_copy);
                }
                if (graphics_argument_count && g_execute_graphics_batch) {
                    batch_first->state.robust_index_access = 1;
                    uint32_t executed = g_execute_graphics_batch(
                        &batch_first->state, graphics_arguments,
                        graphics_argument_count);
                    if (executed > graphics_argument_count)
                        executed = graphics_argument_count;
                    executed_draws += executed;
                    rejected_draws += graphics_argument_count - executed;
                } else {
                    for (uint32_t command = 0;
                         command < graphics_argument_count; ++command) {
                        BeerD3D12GraphicsArguments *arguments =
                            &graphics_arguments[command];
                        batch_first->state.element_count =
                            arguments->element_count;
                        batch_first->state.instance_count =
                            arguments->instance_count;
                        batch_first->state.start_element =
                            arguments->start_element;
                        batch_first->state.base_vertex = arguments->base_vertex;
                        batch_first->state.start_instance =
                            arguments->start_instance;
                        batch_first->state.robust_index_access = 1;
                        batch_first->state.defer_render_target_publication =
                            arguments->defer_render_target_publication;
                        int executed_indirect = g_execute_graphics &&
                            g_execute_graphics(&batch_first->state);
                        if (executed_indirect) ++executed_draws;
                        else ++rejected_draws;
                    }
                }
                free(graphics_arguments);
                draw = batch_last->next;
                continue;
            }
            draw->state.defer_render_target_publication =
                draw_can_defer_to_next(draw, dispatch, clear, color_clear,
                                       resource_copy, texture_copy);
            int executed = g_execute_graphics &&
                g_execute_graphics(&draw->state);
            if (executed) ++executed_draws;
            else ++rejected_draws;
            draw = draw->next;
        }
        END_COMPUTE_BATCH();
#undef END_COMPUTE_BATCH
#undef RESET_GRAPHICS_SYNC_CACHE
        free(compute_results);
        free(compute_accesses);
        free(compute_ir);
    }
    BeerD3D12SubmissionStats stats = {
        .submitted_list_count = count,
        .valid_list_count = valid_lists,
        .open_list_count = open_lists,
        .copy_buffer_count = totals[BEER_D3D12_OP_COPY_BUFFER],
        .copy_texture_count = totals[BEER_D3D12_OP_COPY_TEXTURE],
        .copy_resource_count = totals[BEER_D3D12_OP_COPY_RESOURCE],
        .barrier_count = totals[BEER_D3D12_OP_BARRIER],
        .clear_depth_stencil_count = totals[BEER_D3D12_OP_CLEAR_DSV],
        .clear_render_target_count = totals[BEER_D3D12_OP_CLEAR_RTV],
        .clear_unordered_access_uint_count =
            totals[BEER_D3D12_OP_CLEAR_UAV_UINT],
        .discard_resource_count = totals[BEER_D3D12_OP_DISCARD_RESOURCE],
        .dispatch_count = totals[BEER_D3D12_OP_DISPATCH],
        .draw_instanced_count = totals[BEER_D3D12_OP_DRAW_INSTANCED],
        .draw_indexed_instanced_count =
            totals[BEER_D3D12_OP_DRAW_INDEXED_INSTANCED],
        .render_target_binding_count =
            totals[BEER_D3D12_OP_SET_RENDER_TARGETS],
        .executed_dispatch_count = executed_dispatches,
        .rejected_dispatch_count = rejected_dispatches,
        .executed_draw_count = executed_draws,
        .rejected_draw_count = rejected_draws,
    };
    if (g_submission_observer) g_submission_observer(&stats);
    if (getenv("BEER_D3D12_TRACE_SUBMISSIONS"))
        fprintf(stderr,
                "[D3D12 SUBMIT] n=%llu lists=%u dispatch=%llu/%u/%u "
                "draw=%llu draw-indexed=%llu executed-draws=%u "
                "rejected-draws=%u copy-res=%llu copy-tex=%llu copy-buf=%llu "
                "clear-rtv=%llu clear-dsv=%llu\n",
                (unsigned long long)submission, count,
                (unsigned long long)totals[BEER_D3D12_OP_DISPATCH],
                executed_dispatches, rejected_dispatches,
                (unsigned long long)totals[BEER_D3D12_OP_DRAW_INSTANCED],
                (unsigned long long)totals[BEER_D3D12_OP_DRAW_INDEXED_INSTANCED],
                executed_draws, rejected_draws,
                (unsigned long long)totals[BEER_D3D12_OP_COPY_RESOURCE],
                (unsigned long long)totals[BEER_D3D12_OP_COPY_TEXTURE],
                (unsigned long long)totals[BEER_D3D12_OP_COPY_BUFFER],
                (unsigned long long)totals[BEER_D3D12_OP_CLEAR_RTV],
                (unsigned long long)totals[BEER_D3D12_OP_CLEAR_DSV]);    D3D12_TRACE("[D3D12] ExecuteCommandLists count=%u valid=%u open=%u "
                "copy-buffer=%llu copy-texture=%llu copy-resource=%llu "
                "barrier=%llu clear-dsv=%llu clear=%llu clear-uav=%llu "
                "discard=%llu dispatch=%llu draw=%llu draw-indexed=%llu "
                "set-rt=%llu executed=%u rejected=%u end-query=%llu "
                "resolve-query=%llu\n",
                count, valid_lists, open_lists,
                (unsigned long long)totals[BEER_D3D12_OP_COPY_BUFFER],
                (unsigned long long)totals[BEER_D3D12_OP_COPY_TEXTURE],
                (unsigned long long)totals[BEER_D3D12_OP_COPY_RESOURCE],
                (unsigned long long)totals[BEER_D3D12_OP_BARRIER],
                (unsigned long long)totals[BEER_D3D12_OP_CLEAR_DSV],
                (unsigned long long)totals[BEER_D3D12_OP_CLEAR_RTV],
                (unsigned long long)totals[BEER_D3D12_OP_CLEAR_UAV_UINT],
                (unsigned long long)totals[BEER_D3D12_OP_DISCARD_RESOURCE],
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
        if (dispatch->indirect_arguments)
            resource_release(dispatch->indirect_arguments);
        if (dispatch->indirect_count)
            resource_release(dispatch->indirect_count);
        for (uint32_t index = 0;
             index < dispatch->state.descriptor_heap_count; ++index) {
            if (dispatch->state.descriptor_heaps[index])
                descriptor_heap_release((BeerD3D12DescriptorHeap *)
                    dispatch->state.descriptor_heaps[index]);
        }
        for (uint32_t parameter = 0;
             parameter < BEER_D3D12_MAX_ROOT_PARAMETERS; ++parameter) {
            BeerD3D12ComputeBinding *binding =
                &dispatch->state.bindings[parameter];
            if (binding->resolved_root_resource)
                resource_release((BeerD3D12Resource *)
                    binding->resolved_root_resource);
            snapshot_release(dispatch->root_binding_copies[parameter],
                             dispatch->root_binding_copy_sizes[parameter]);
            for (uint32_t descriptor = 0;
                 descriptor < binding->descriptor_count; ++descriptor) {
                if (binding->descriptor_resources[descriptor])
                    resource_release((BeerD3D12Resource *)
                        binding->descriptor_resources[descriptor]);
                snapshot_release(
                    dispatch->descriptor_copies[parameter][descriptor],
                    dispatch->descriptor_copy_sizes[parameter][descriptor]);
            }
        }
        recorded_free(dispatch, sizeof(BeerD3D12RecordedDispatch));
        dispatch = next;
    }
    if (self) {
        self->dispatches = NULL;
        self->dispatch_tail = &self->dispatches;
    }
}

static void command_list_clear_depth_clears(BeerD3D12GraphicsCommandList *self)
{
    BeerD3D12RecordedDepthClear *clear = self ? self->depth_clears : NULL;
    while (clear) {
        BeerD3D12RecordedDepthClear *next = clear->next;
        if (clear->resource) resource_release(clear->resource);
        free(clear->rects);
        free(clear);
        clear = next;
    }
    if (self) {
        self->depth_clears = NULL;
        self->depth_clear_tail = &self->depth_clears;
    }
}

static void command_list_clear_color_clears(BeerD3D12GraphicsCommandList *self)
{
    BeerD3D12RecordedColorClear *clear = self ? self->color_clears : NULL;
    while (clear) {
        BeerD3D12RecordedColorClear *next = clear->next;
        if (clear->resource) resource_release(clear->resource);
        free(clear->rects);
        free(clear);
        clear = next;
    }
    if (self) {
        self->color_clears = NULL;
        self->color_clear_tail = &self->color_clears;
    }
}

static void command_list_clear_resource_copies(
    BeerD3D12GraphicsCommandList *self)
{
    BeerD3D12RecordedResourceCopy *copy = self ? self->resource_copies : NULL;
    while (copy) {
        BeerD3D12RecordedResourceCopy *next = copy->next;
        resource_release(copy->destination);
        resource_release(copy->source);
        snapshot_release(copy->source_snapshot, (size_t)copy->byte_count);
        free(copy);
        copy = next;
    }
    if (self) {
        self->resource_copies = NULL;
        self->resource_copy_tail = &self->resource_copies;
    }
}

static void command_list_clear_texture_copies(
    BeerD3D12GraphicsCommandList *self)
{
    BeerD3D12RecordedTextureCopy *copy = self ? self->texture_copies : NULL;
    while (copy) {
        BeerD3D12RecordedTextureCopy *next = copy->next;
        resource_release(copy->destination);
        snapshot_release(copy->source_snapshot, copy->source_snapshot_size);
        free(copy);
        copy = next;
    }
    if (self) {
        self->texture_copies = NULL;
        self->texture_copy_tail = &self->texture_copies;
    }
}

static void command_list_clear_draws(BeerD3D12GraphicsCommandList *self)
{
    BeerD3D12RecordedDraw *draw = self ? self->draws : NULL;
    while (draw) {
        BeerD3D12RecordedDraw *next = draw->next;
        if (draw->state.pipeline_state)
            pipeline_state_release((BeerD3D12PipelineState *)
                                   draw->state.pipeline_state);
        if (draw->state.root_signature)
            root_signature_release((BeerD3D12RootSignature *)
                                   draw->state.root_signature);
        for (uint32_t index = 0;
             index < draw->state.descriptor_heap_count; ++index) {
            if (draw->state.descriptor_heaps[index])
                descriptor_heap_release((BeerD3D12DescriptorHeap *)
                    draw->state.descriptor_heaps[index]);
        }
        for (uint32_t index = 0;
             index < draw->state.render_target_count; ++index) {
            if (draw->state.render_targets[index])
                resource_release((BeerD3D12Resource *)
                    draw->state.render_targets[index]);
        }
        for (uint32_t parameter = 0;
             parameter < BEER_D3D12_MAX_ROOT_PARAMETERS; ++parameter) {
            BeerD3D12ComputeBinding *binding = &draw->state.bindings[parameter];
            if (binding->resolved_root_resource)
                resource_release((BeerD3D12Resource *)
                    binding->resolved_root_resource);
            snapshot_release(draw->root_binding_copies[parameter],
                             draw->root_binding_copy_sizes[parameter]);
            for (uint32_t descriptor = 0;
                 descriptor < binding->descriptor_count; ++descriptor) {
                if (binding->descriptor_resources[descriptor])
                    resource_release((BeerD3D12Resource *)
                        binding->descriptor_resources[descriptor]);
                snapshot_release(draw->descriptor_copies[parameter][descriptor],
                    draw->descriptor_copy_sizes[parameter][descriptor]);
            }
        }
        if (draw->state.depth_stencil)
            resource_release((BeerD3D12Resource *)draw->state.depth_stencil);
        if (draw->indirect_arguments)
            resource_release(draw->indirect_arguments);
        if (draw->indirect_count) resource_release(draw->indirect_count);
        for (uint32_t index = 0;
             index < BEER_D3D12_MAX_VERTEX_BUFFERS; ++index) {
            if (draw->vertex_buffer_resources[index])
                resource_release(draw->vertex_buffer_resources[index]);
            snapshot_release(draw->vertex_buffer_copies[index],
                             draw->vertex_buffer_copy_sizes[index]);
        }
        if (draw->index_buffer_resource)
            resource_release(draw->index_buffer_resource);
        snapshot_release(draw->index_buffer_copy,
                         draw->index_buffer_copy_size);
        recorded_free(draw, sizeof(BeerD3D12RecordedDraw));
        draw = next;
    }
    if (self) {
        self->draws = NULL;
        self->draw_tail = &self->draws;
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
        command_list_clear_depth_clears(self);
        command_list_clear_color_clears(self);
        command_list_clear_resource_copies(self);
        command_list_clear_texture_copies(self);
        command_list_clear_draws(self);
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
    command_list_clear_depth_clears(self);
    command_list_clear_color_clears(self);
    command_list_clear_resource_copies(self);
    command_list_clear_texture_copies(self);
    command_list_clear_draws(self);
    self->closed = 0;
    memset(self->operation_counts, 0, sizeof(self->operation_counts));
    memset(self->compute_bindings, 0, sizeof(self->compute_bindings));
    memset(self->graphics_bindings, 0, sizeof(self->graphics_bindings));
    memset(self->vertex_buffers, 0, sizeof(self->vertex_buffers));
    memset(&self->index_buffer, 0, sizeof(self->index_buffer));
    memset(self->render_targets, 0, sizeof(self->render_targets));
    self->render_target_count = 0;
    self->depth_stencil = NULL;
    self->vertex_buffer_mask = 0;
    memset(self->viewport, 0, sizeof(self->viewport));
    memset(self->scissor, 0, sizeof(self->scissor));
    self->recording_sequence = 0;
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
    BeerD3D12RecordedResourceCopy *copy = calloc(1, sizeof(*copy));
    if (!copy) return;
    copy->source_snapshot = byte_count ?
        snapshot_allocate((size_t)byte_count) : NULL;
    if (byte_count && !copy->source_snapshot) {
        free(copy);
        return;
    }
    if (byte_count)
        memcpy(copy->source_snapshot, source->storage + source_offset,
               (size_t)byte_count);
    copy->destination = destination;
    copy->source = source;
    copy->destination_offset = destination_offset;
    copy->source_offset = source_offset;
    copy->byte_count = byte_count;
    copy->sequence = ++self->recording_sequence;
    resource_add_ref(destination);
    resource_add_ref(source);
    if (!self->resource_copy_tail)
        self->resource_copy_tail = &self->resource_copies;
    *self->resource_copy_tail = copy;
    self->resource_copy_tail = &copy->next;
    ++self->operation_counts[BEER_D3D12_OP_COPY_BUFFER];
}

/* D3D12_TEXTURE_COPY_LOCATION is 48 bytes on Win64. RE8's upload path uses
 * placed buffer footprints as sources for texture-subresource copies. */
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

static void apply_resource_copy(BeerD3D12RecordedResourceCopy *copy)
{
    if (!copy || !copy->destination || !copy->source) return;
    BeerD3D12Resource *destination = copy->destination;
    BeerD3D12Resource *source = copy->source;
    uint64_t destination_offset = copy->destination_offset;
    uint64_t source_offset = copy->source_offset;
    size_t bytes;
    if (copy->byte_count) {
        bytes = (size_t)copy->byte_count;
    } else {
        bytes = destination->storage_size < source->storage_size
            ? destination->storage_size : source->storage_size;
        destination_offset = 0;
        source_offset = 0;
    }
    /* A Vulkan draw/dispatch may own the newest source bytes. Flushing all
     * mirrors before the CPU copy preserves D3D12 command ordering and avoids
     * copying stale host storage into the destination. Upload/readback source
     * bytes are frozen when recorded, matching D3D12 command-list semantics
     * when the application reuses a persistently mapped ring before submit. */
    if (g_end_compute_batch) g_end_compute_batch();
    if (g_forget_source_resource) g_forget_source_resource(source);
    else if (g_forget_resource) g_forget_resource(source);
    if (destination != source && g_forget_resource)
        g_forget_resource(destination);
    const uint8_t *source_bytes = copy->source_snapshot ?
        copy->source_snapshot : source->storage + (size_t)source_offset;
    memmove(destination->storage + (size_t)destination_offset, source_bytes,
            bytes);
    ++destination->content_serial;
}

static void __attribute__((ms_abi))
command_list_copy_resource(BeerD3D12GraphicsCommandList *self,
                           BeerD3D12Resource *destination,
                           BeerD3D12Resource *source)
{
    if (!self || self->closed || !destination || !source ||
        destination->device != self->device || source->device != self->device)
        return;
    BeerD3D12RecordedResourceCopy *copy = calloc(1, sizeof(*copy));
    if (!copy) return;
    copy->destination = destination;
    copy->source = source;
    copy->destination_offset = 0;
    copy->source_offset = 0;
    copy->byte_count = 0;
    copy->sequence = ++self->recording_sequence;
    resource_add_ref(destination);
    resource_add_ref(source);
    if (!self->resource_copy_tail)
        self->resource_copy_tail = &self->resource_copies;
    *self->resource_copy_tail = copy;
    self->resource_copy_tail = &copy->next;
    ++self->operation_counts[BEER_D3D12_OP_COPY_RESOURCE];
}

static void __attribute__((ms_abi))
command_list_clear_state(BeerD3D12GraphicsCommandList *self,
                         BeerD3D12PipelineState *pipeline_state)
{
    if (!self || self->closed ||
        (pipeline_state && pipeline_state->device != self->device))
        return;
    if (pipeline_state) pipeline_state_add_ref(pipeline_state);
    if (self->pipeline_state) pipeline_state_release(self->pipeline_state);
    self->pipeline_state = pipeline_state;
    self->descriptor_heap_count = 0;
    memset(self->compute_bindings, 0, sizeof(self->compute_bindings));
    memset(self->graphics_bindings, 0, sizeof(self->graphics_bindings));
    memset(self->vertex_buffers, 0, sizeof(self->vertex_buffers));
    memset(&self->index_buffer, 0, sizeof(self->index_buffer));
    memset(self->render_targets, 0, sizeof(self->render_targets));
    self->render_target_count = 0;
    self->depth_stencil = NULL;
    self->vertex_buffer_mask = 0;
    memset(self->viewport, 0, sizeof(self->viewport));
    memset(self->scissor, 0, sizeof(self->scissor));
    memset(self->blend_factor, 0, sizeof(self->blend_factor));
    self->primitive_topology = 0;
    self->stencil_reference = 0;
}

static uint32_t descriptor_texture_subresource(const uint8_t *descriptor,
                                               uint32_t range_type,
                                               const BeerD3D12Resource *resource)
{
    if (!descriptor || !resource || resource->description.dimension == 1 ||
        (range_type != 0 && range_type != 1))
        return 0;
    uint32_t view_dimension = 0;
    memcpy(&view_dimension, descriptor + 12, sizeof(view_dimension));
    /* The private descriptor has an eight-byte resource prefix. SRV unions are
     * aligned to native byte 16 (private byte 24), while UAV unions start at
     * private byte 16. Texture2DArray uses the same leading mip field as
     * Texture2D, followed by FirstArraySlice and ArraySize. */
    if (view_dimension != 2 && view_dimension != 3 &&
        view_dimension != 4 && view_dimension != 5 && view_dimension != 8 &&
        view_dimension != 9 && view_dimension != 10)
        return 0;
    uint32_t mip = 0, first_layer = 0;
    size_t mip_offset = range_type == 0 ? 24u : 16u;
    memcpy(&mip, descriptor + mip_offset, sizeof(mip));
    if (view_dimension == 3 || view_dimension == 5 || view_dimension == 10)
        memcpy(&first_layer, descriptor + 16, sizeof(first_layer));
    uint32_t mip_levels = resource->description.mip_levels ?
        resource->description.mip_levels : 1;
    uint32_t layers = resource->description.depth_or_array_size ?
        resource->description.depth_or_array_size : 1;
    if (getenv("BEER_D3D12_SUBRESOURCE_DIAGNOSTICS")) {
        static unsigned descriptor_traces;
        if (descriptor_traces++ < 64) {
            uint32_t words[6] = {0};
            memcpy(words, descriptor + 8, sizeof(words));
            fprintf(stderr,
                    "[D3D12 VIEW] type=%u dim=%u mip=%u layer=%u levels=%u "
                    "desc=%08x,%08x,%08x,%08x,%08x,%08x resource=%ux%u/f%u\n",
                    range_type, view_dimension, mip, first_layer, mip_levels,
                    words[0], words[1], words[2], words[3], words[4], words[5],
                    resource->description.width > UINT32_MAX ? UINT32_MAX :
                        (uint32_t)resource->description.width,
                    resource->description.height, resource->description.format);
        }
    }
    if (mip >= mip_levels || first_layer >= layers) return 0;
    return first_layer * mip_levels + mip;
}

static int root_signature_resolve_compute_descriptors(
    BeerD3D12GraphicsCommandList *self, BeerD3D12ComputeDispatch *state)
{
    if (!self || !state || !self->compute_root_signature) return 0;
    const uint8_t *blob = self->compute_root_signature->blob;
    size_t blob_size = self->compute_root_signature->blob_size;
    static const uint8_t magic[8] = {'B','E','E','R','R','S','I','G'};
    if (!blob || blob_size < 24 || memcmp(blob, magic, sizeof(magic)) != 0)
        return 0;

    uint32_t version = 0, parameter_count = 0;
    memcpy(&version, blob + 8, sizeof(version));
    memcpy(&parameter_count, blob + 12, sizeof(parameter_count));
    if (version < 1 || version > 3 || parameter_count > 4096) return 0;
    size_t range_size = version == 1 ? 20u : 24u;
    size_t cursor = 24;
    uint32_t resolved = 0;
    for (uint32_t parameter = 0; parameter < parameter_count; ++parameter) {
        if (cursor > blob_size || blob_size - cursor < 8) return 0;
        uint32_t type = 0, visibility = 0;
        memcpy(&type, blob + cursor, sizeof(type));
        memcpy(&visibility, blob + cursor + 4, sizeof(visibility));
        cursor += 8; /* type and shader visibility */
        BeerD3D12ComputeBinding *parameter_binding = parameter <
            BEER_D3D12_MAX_ROOT_PARAMETERS ? &state->bindings[parameter] : NULL;
        if (parameter_binding)
            parameter_binding->shader_visibility = visibility;
        if (type == 0) {
            if (blob_size - cursor < 4) return 0;
            uint32_t range_count = 0;
            memcpy(&range_count, blob + cursor, sizeof(range_count));
            cursor += 4;
            if (range_count > (blob_size - cursor) / range_size) return 0;
            BeerD3D12ComputeBinding *binding = parameter <
                BEER_D3D12_MAX_ROOT_PARAMETERS ?
                &state->bindings[parameter] : NULL;
            uint32_t append_offset = 0;
            for (uint32_t range_index = 0; range_index < range_count;
                 ++range_index, cursor += range_size) {
                uint32_t range_type = 0, descriptor_count = 0;
                uint32_t base_register = 0, register_space = 0;
                uint32_t table_offset = UINT32_MAX;
                memcpy(&range_type, blob + cursor, 4);
                memcpy(&descriptor_count, blob + cursor + 4, 4);
                memcpy(&base_register, blob + cursor + 8, 4);
                memcpy(&register_space, blob + cursor + 12, 4);
                /* D3D12_DESCRIPTOR_RANGE uses offset 16. RANGE1 inserts Flags
                 * there and moves OffsetInDescriptorsFromTableStart to 20. */
                memcpy(&table_offset,
                       blob + cursor + (version == 1 ? 16u : 20u), 4);
                if (table_offset == UINT32_MAX) table_offset = append_offset;
                if (descriptor_count != UINT32_MAX &&
                    table_offset <= UINT32_MAX - descriptor_count)
                    append_offset = table_offset + descriptor_count;
                else
                    append_offset = UINT32_MAX;
                if (!binding || binding->kind != 1 ||
                    !binding->descriptor_table || range_type > 3)
                    continue;
                uint32_t available = BEER_D3D12_MAX_RESOLVED_DESCRIPTORS -
                    binding->descriptor_count;
                uint32_t count = descriptor_count;
                if (count == UINT32_MAX || count > available) count = available;
                for (uint32_t descriptor = 0; descriptor < count;
                     ++descriptor) {
                    uint64_t handle = binding->descriptor_table +
                        (uint64_t)(table_offset + descriptor) *
                        descriptor_stride(range_type == 3 ? 1 : 0);
                    for (uint32_t heap_index = 0;
                         heap_index < self->descriptor_heap_count; ++heap_index) {
                        BeerD3D12DescriptorHeap *heap =
                            self->descriptor_heaps[heap_index];
                        uint32_t expected_heap = range_type == 3 ? 1u : 0u;
                        if (!heap || heap->description[0] != expected_heap ||
                            handle < heap->gpu_start ||
                            handle - heap->gpu_start > heap->storage_size ||
                            heap->storage_size -
                                (size_t)(handle - heap->gpu_start) <
                                sizeof(BeerD3D12Resource *))
                            continue;
                        const uint8_t *descriptor_bytes = heap->storage +
                            (size_t)(handle - heap->gpu_start);
                        BeerD3D12Resource *resource = NULL;
                        const uint8_t *cbv_storage = NULL;
                        uint32_t cbv_size = 0;
                        if (range_type == 2) {
                            uint64_t address = 0;
                            memcpy(&address, descriptor_bytes, sizeof(address));
                            memcpy(&cbv_size, descriptor_bytes + 8,
                                   sizeof(cbv_size));
                            if (cbv_size)
                                resource = resource_registry_resolve_buffer(
                                    address, cbv_size, &cbv_storage);
                        } else if (range_type == 3) {
                            /* Sampler descriptors hold the sampler state, not
                             * a resource pointer. */
                            resource = NULL;
                        } else {
                            memcpy(&resource, descriptor_bytes,
                                   sizeof(resource));
                            if (!resource ||
                                resource->vtable != g_resource_vtable ||
                                resource->device != self->device)
                                resource = NULL;
                            else
                                resource_add_ref(resource);
                        }
                        uint32_t output = binding->descriptor_count++;
                        binding->descriptor_types[output] = range_type;
                        binding->descriptor_registers[output] =
                            base_register + descriptor;
                        binding->descriptor_spaces[output] = register_space;
                        binding->descriptor_handles[output] = handle;
                        size_t descriptor_available = heap->storage_size -
                            (size_t)(handle - heap->gpu_start);
                        size_t descriptor_raw_size = descriptor_available < 32u ?
                            descriptor_available : 32u;
                        memcpy(binding->descriptor_raw_bytes[output],
                               descriptor_bytes, descriptor_raw_size);
                        binding->descriptor_raw_sizes[output] =
                            (uint32_t)descriptor_raw_size;
                        if (range_type == 2) {
                            memcpy(&binding->descriptor_buffer_locations[output],
                                   descriptor_bytes, sizeof(uint64_t));
                        }
                        if (range_type == 3 && heap->storage_size -
                            (size_t)(handle - heap->gpu_start) >= 32)
                            memcpy(binding->descriptor_samplers[output],
                                   descriptor_bytes, 32);
                        if (resource) {
                            binding->descriptor_resources[output] = resource;
                            binding->descriptor_resource_serial_locations[output] =
                                &resource->content_serial;
                            binding->descriptor_resource_gpu_bases[output] =
                                resource->gpu_virtual_address;
                            binding->descriptor_resource_total_sizes[output] =
                                resource->storage_size;
                            binding->descriptor_resource_heap_types[output] =
                                resource->heap_properties[0];
                            if (range_type == 2) {
                                binding->descriptor_resource_storage[output] =
                                    (uint8_t *)cbv_storage;
                                binding->descriptor_resource_storage_size[output] =
                                    cbv_size;
                                binding->descriptor_widths[output] = cbv_size;
                                binding->descriptor_heights[output] = 1;
                                ++resolved;
                                break;
                            }
                            uint32_t subresource =
                                descriptor_texture_subresource(
                                    descriptor_bytes, range_type, resource);
                            size_t offset = 0, row_pitch = 0;
                            uint32_t width = 0, height = 0;
                            if (!resource_texture_layout(&resource->description,
                                                         subresource, &offset,
                                                         &row_pitch, &width,
                                                         &height) ||
                                offset > resource->storage_size) {
                                offset = 0;
                                row_pitch = 0;
                                width = resource->description.width > UINT32_MAX ?
                                    UINT32_MAX :
                                    (uint32_t)resource->description.width;
                                height = resource->description.height;
                                subresource = 0;
                            }
                            binding->descriptor_resource_storage[output] =
                                resource->storage + offset;
                            binding->descriptor_resource_storage_size[output] =
                                resource->storage_size - offset;
                            binding->descriptor_widths[output] = width;
                            binding->descriptor_heights[output] = height;
                            binding->descriptor_depths[output] =
                                resource->description.dimension == 4 ?
                                resource->description.depth_or_array_size >>
                                    subresource : 1;
                            if (!binding->descriptor_depths[output])
                                binding->descriptor_depths[output] = 1;
                            binding->descriptor_formats[output] =
                                resource->description.format;
                            binding->descriptor_subresources[output] = subresource;
                            binding->descriptor_subresource_offsets[output] = offset;
                            binding->descriptor_row_pitches[output] = row_pitch;
                            if (range_type <= 1 && heap->storage_size -
                                (size_t)(handle - heap->gpu_start) >= 32) {
                                memcpy(&binding->descriptor_view_formats[output],
                                       descriptor_bytes + 8, 4);
                                memcpy(&binding->descriptor_view_dimensions[output],
                                       descriptor_bytes + 12, 4);
                                                if (range_type == 0) {
                                    memcpy(&binding->descriptor_view_mip_levels[output],
                                           descriptor_bytes + 28, 4);
                                    if (binding->descriptor_view_dimensions[output] == 3 ||
                                        binding->descriptor_view_dimensions[output] == 5 ||
                                        binding->descriptor_view_dimensions[output] == 10) {
                                        memcpy(&binding->descriptor_view_first_layers[output],
                                               descriptor_bytes + 16, 4);
                                        memcpy(&binding->descriptor_view_layer_counts[output],
                                               descriptor_bytes + 20, 4);
                                    }
                                } else if (range_type == 1 &&
                                           (binding->descriptor_view_dimensions[output] == 3 ||
                                            binding->descriptor_view_dimensions[output] == 5)) {
                                    /* UAV array unions fit in the retained
                                     * native descriptor prefix. */
                                    memcpy(&binding->descriptor_view_first_layers[output],
                                           descriptor_bytes + 20, 4);
                                    memcpy(&binding->descriptor_view_layer_counts[output],
                                           descriptor_bytes + 24, 4);
                                }
                                binding->descriptor_resource_dimensions[output] =
                                    resource->description.dimension;
                                binding->descriptor_resource_mips[output] =
                                    resource->description.mip_levels ?
                                    resource->description.mip_levels : 1;
                                binding->descriptor_resource_layers[output] =
                                    resource->description.depth_or_array_size ?
                                    resource->description.depth_or_array_size : 1;
                                binding->descriptor_resource_samples[output] =
                                    resource->description.sample_count ?
                                    resource->description.sample_count : 1;
                                if (binding->descriptor_view_dimensions[output] == 1) {
                                    /* Buffer SRV/UAV descriptors use the shared
                                     * packed union at +16. */
                                    memcpy(&binding->descriptor_view_first_elements[output],
                                           descriptor_bytes + 16, 8);
                                    memcpy(&binding->descriptor_view_element_counts[output],
                                           descriptor_bytes + 24, 4);
                                    memcpy(&binding->descriptor_view_structure_strides[output],
                                           descriptor_bytes + 28, 4);
                                }
                            }
                            ++resolved;
                        }
                        break;
                    }
                }
            }
        } else {
            size_t payload = type == 1 ? 12u :
                (type >= 2 && type <= 4 ? (version == 1 ? 8u : 12u) : 0u);
            if (!payload || payload > blob_size - cursor) return 0;
            cursor += payload;
        }
    }
    return resolved != 0;
}

static int root_signature_resolve_draw_descriptors(
    BeerD3D12GraphicsCommandList *self, BeerD3D12GraphicsDraw *draw)
{
    if (!self || !draw || !self->graphics_root_signature) return 0;
    const uint8_t *blob = self->graphics_root_signature->blob;
    size_t blob_size = self->graphics_root_signature->blob_size;
    static const uint8_t magic[8] = {'B','E','E','R','R','S','I','G'};
    if (!blob || blob_size < 24 || memcmp(blob, magic, sizeof(magic)) != 0)
        return 0;

    uint32_t version = 0, parameter_count = 0;
    memcpy(&version, blob + 8, sizeof(version));
    memcpy(&parameter_count, blob + 12, sizeof(parameter_count));
    if (version < 1 || version > 3 || parameter_count > 4096) return 0;
    size_t range_size = version == 1 ? 20u : 24u;
    size_t cursor = 24;
    uint32_t resolved = 0;
    for (uint32_t parameter = 0; parameter < parameter_count; ++parameter) {
        if (cursor > blob_size || blob_size - cursor < 8) return 0;
        uint32_t type = 0, visibility = 0;
        memcpy(&type, blob + cursor, sizeof(type));
        memcpy(&visibility, blob + cursor + 4, sizeof(visibility));
        cursor += 8; /* type and shader visibility */
        BeerD3D12ComputeBinding *parameter_binding = parameter <
            BEER_D3D12_MAX_ROOT_PARAMETERS ? &draw->bindings[parameter] : NULL;
        if (parameter_binding)
            parameter_binding->shader_visibility = visibility;
        if (type == 0) {
            if (blob_size - cursor < 4) return 0;
            uint32_t range_count = 0;
            memcpy(&range_count, blob + cursor, sizeof(range_count));
            cursor += 4;
            if (range_count > (blob_size - cursor) / range_size) return 0;
            BeerD3D12ComputeBinding *binding = parameter <
                BEER_D3D12_MAX_ROOT_PARAMETERS ?
                &draw->bindings[parameter] : NULL;
            uint32_t append_offset = 0;
            for (uint32_t range_index = 0; range_index < range_count;
                 ++range_index, cursor += range_size) {
                uint32_t range_type = 0, descriptor_count = 0;
                uint32_t base_register = 0, register_space = 0;
                uint32_t table_offset = UINT32_MAX;
                memcpy(&range_type, blob + cursor, 4);
                memcpy(&descriptor_count, blob + cursor + 4, 4);
                memcpy(&base_register, blob + cursor + 8, 4);
                memcpy(&register_space, blob + cursor + 12, 4);
                /* D3D12_DESCRIPTOR_RANGE uses offset 16. RANGE1 inserts Flags
                 * there and moves OffsetInDescriptorsFromTableStart to 20. */
                memcpy(&table_offset,
                       blob + cursor + (version == 1 ? 16u : 20u), 4);
                if (table_offset == UINT32_MAX) table_offset = append_offset;
                if (descriptor_count != UINT32_MAX &&
                    table_offset <= UINT32_MAX - descriptor_count)
                    append_offset = table_offset + descriptor_count;
                else
                    append_offset = UINT32_MAX;
                if (!binding || binding->kind != 1 ||
                    !binding->descriptor_table || range_type > 3)
                    continue;
                uint32_t available = BEER_D3D12_MAX_RESOLVED_DESCRIPTORS -
                    binding->descriptor_count;
                uint32_t count = descriptor_count;
                if (count == UINT32_MAX || count > available) count = available;
                for (uint32_t descriptor = 0; descriptor < count;
                     ++descriptor) {
                    uint64_t handle = binding->descriptor_table +
                        (uint64_t)(table_offset + descriptor) *
                        descriptor_stride(range_type == 3 ? 1 : 0);
                    for (uint32_t heap_index = 0;
                         heap_index < self->descriptor_heap_count; ++heap_index) {
                        BeerD3D12DescriptorHeap *heap =
                            self->descriptor_heaps[heap_index];
                        uint32_t expected_heap = range_type == 3 ? 1u : 0u;
                        if (!heap || heap->description[0] != expected_heap ||
                            handle < heap->gpu_start ||
                            handle - heap->gpu_start > heap->storage_size ||
                            heap->storage_size -
                                (size_t)(handle - heap->gpu_start) <
                                sizeof(BeerD3D12Resource *))
                            continue;
                        const uint8_t *descriptor_bytes = heap->storage +
                            (size_t)(handle - heap->gpu_start);
                        BeerD3D12Resource *resource = NULL;
                        const uint8_t *cbv_storage = NULL;
                        uint32_t cbv_size = 0;
                        if (range_type == 2) {
                            uint64_t address = 0;
                            memcpy(&address, descriptor_bytes, sizeof(address));
                            memcpy(&cbv_size, descriptor_bytes + 8,
                                   sizeof(cbv_size));
                            if (cbv_size)
                                resource = resource_registry_resolve_buffer(
                                    address, cbv_size, &cbv_storage);
                        } else if (range_type != 3) {
                            memcpy(&resource, descriptor_bytes,
                                   sizeof(resource));
                            if (!resource || resource->vtable != g_resource_vtable ||
                                resource->device != self->device)
                                resource = NULL;
                            else
                                resource_add_ref(resource);
                        }
                        uint32_t output = binding->descriptor_count++;
                        binding->descriptor_types[output] = range_type;
                        binding->descriptor_registers[output] =
                            base_register + descriptor;
                        binding->descriptor_spaces[output] = register_space;
                        binding->descriptor_handles[output] = handle;
                        size_t descriptor_available = heap->storage_size -
                            (size_t)(handle - heap->gpu_start);
                        size_t descriptor_raw_size = descriptor_available < 32u ?
                            descriptor_available : 32u;
                        memcpy(binding->descriptor_raw_bytes[output],
                               descriptor_bytes, descriptor_raw_size);
                        binding->descriptor_raw_sizes[output] =
                            (uint32_t)descriptor_raw_size;
                        if (range_type == 2) {
                            memcpy(&binding->descriptor_buffer_locations[output],
                                   descriptor_bytes, sizeof(uint64_t));
                        }
                        if (range_type == 3 && heap->storage_size -
                            (size_t)(handle - heap->gpu_start) >= 32) {
                            memcpy(binding->descriptor_samplers[output],
                                   descriptor_bytes, 32);
                            ++resolved;
                        }
                        if (resource) {
                            binding->descriptor_resources[output] = resource;
                            binding->descriptor_resource_serial_locations[output] =
                                &resource->content_serial;
                            binding->descriptor_resource_gpu_bases[output] =
                                resource->gpu_virtual_address;
                            binding->descriptor_resource_total_sizes[output] =
                                resource->storage_size;
                            binding->descriptor_resource_heap_types[output] =
                                resource->heap_properties[0];
                            if (range_type == 2) {
                                binding->descriptor_resource_storage[output] =
                                    (uint8_t *)cbv_storage;
                                binding->descriptor_resource_storage_size[output] =
                                    cbv_size;
                                binding->descriptor_widths[output] = cbv_size;
                                binding->descriptor_heights[output] = 1;
                                ++resolved;
                                break;
                            }
                            uint32_t subresource = descriptor_texture_subresource(
                                descriptor_bytes, range_type, resource);
                            size_t offset = 0, row_pitch = 0;
                            uint32_t width = 0, height = 0;
                            if (!resource_texture_layout(&resource->description,
                                                         subresource, &offset,
                                                         &row_pitch, &width,
                                                         &height) ||
                                offset > resource->storage_size) {
                                offset = 0;
                                row_pitch = 0;
                                width = resource->description.width > UINT32_MAX ?
                                    UINT32_MAX :
                                    (uint32_t)resource->description.width;
                                height = resource->description.height;
                                subresource = 0;
                            }
                            binding->descriptor_resource_storage[output] =
                                resource->storage + offset;
                            binding->descriptor_resource_storage_size[output] =
                                resource->storage_size - offset;
                            binding->descriptor_widths[output] = width;
                            binding->descriptor_heights[output] = height;
                            binding->descriptor_depths[output] =
                                resource->description.dimension == 4 ?
                                resource->description.depth_or_array_size >>
                                    subresource : 1;
                            if (!binding->descriptor_depths[output])
                                binding->descriptor_depths[output] = 1;
                            binding->descriptor_formats[output] =
                                resource->description.format;
                            binding->descriptor_subresources[output] = subresource;
                            binding->descriptor_subresource_offsets[output] = offset;
                            binding->descriptor_row_pitches[output] = row_pitch;
                            if (range_type <= 1 && heap->storage_size -
                                (size_t)(handle - heap->gpu_start) >= 32) {
                                memcpy(&binding->descriptor_view_formats[output],
                                       descriptor_bytes + 8, 4);
                                memcpy(&binding->descriptor_view_dimensions[output],
                                       descriptor_bytes + 12, 4);
                                                if (range_type == 0) {
                                    memcpy(&binding->descriptor_view_mip_levels[output],
                                           descriptor_bytes + 28, 4);
                                    if (binding->descriptor_view_dimensions[output] == 3 ||
                                        binding->descriptor_view_dimensions[output] == 5 ||
                                        binding->descriptor_view_dimensions[output] == 10) {
                                        memcpy(&binding->descriptor_view_first_layers[output],
                                               descriptor_bytes + 16, 4);
                                        memcpy(&binding->descriptor_view_layer_counts[output],
                                               descriptor_bytes + 20, 4);
                                    }
                                } else if (range_type == 1 &&
                                           (binding->descriptor_view_dimensions[output] == 3 ||
                                            binding->descriptor_view_dimensions[output] == 5)) {
                                    /* UAV array unions fit in the retained
                                     * native descriptor prefix. */
                                    memcpy(&binding->descriptor_view_first_layers[output],
                                           descriptor_bytes + 20, 4);
                                    memcpy(&binding->descriptor_view_layer_counts[output],
                                           descriptor_bytes + 24, 4);
                                }
                                binding->descriptor_resource_dimensions[output] =
                                    resource->description.dimension;
                                binding->descriptor_resource_mips[output] =
                                    resource->description.mip_levels ?
                                    resource->description.mip_levels : 1;
                                binding->descriptor_resource_layers[output] =
                                    resource->description.depth_or_array_size ?
                                    resource->description.depth_or_array_size : 1;
                                binding->descriptor_resource_samples[output] =
                                    resource->description.sample_count ?
                                    resource->description.sample_count : 1;
                                if (binding->descriptor_view_dimensions[output] == 1) {
                                    memcpy(&binding->descriptor_view_first_elements[output],
                                           descriptor_bytes + 16, 8);
                                    memcpy(&binding->descriptor_view_element_counts[output],
                                           descriptor_bytes + 24, 4);
                                    memcpy(&binding->descriptor_view_structure_strides[output],
                                           descriptor_bytes + 28, 4);
                                }
                            }
                            ++resolved;
                        }
                        break;
                    }
                }
            }
        } else {
            size_t payload = type == 1 ? 12u :
                (type >= 2 && type <= 4 ? (version == 1 ? 8u : 12u) : 0u);
            if (!payload || payload > blob_size - cursor) return 0;
            cursor += payload;
        }
    }
    return resolved != 0;
}

static void command_list_record_draw(BeerD3D12GraphicsCommandList *self,
                                     uint32_t indexed, uint32_t element_count,
                                     uint32_t instance_count,
                                     uint32_t start_element,
                                     int32_t base_vertex,
                                     uint32_t start_instance)
{
    if (!self || self->closed || !element_count || !instance_count) return;
    BeerD3D12RecordedDraw *draw = recorded_alloc(sizeof(BeerD3D12RecordedDraw));
    if (!draw) return;
    draw->state.pipeline_state = self->pipeline_state;
    draw->state.root_signature = self->graphics_root_signature;
    draw->state.indexed = indexed;
    draw->state.element_count = element_count;
    draw->state.instance_count = instance_count;
    draw->state.start_element = start_element;
    draw->state.base_vertex = base_vertex;
    draw->state.start_instance = start_instance;
    draw->sequence = ++self->recording_sequence;
    draw->state.primitive_topology = self->primitive_topology;
    draw->state.stencil_reference = self->stencil_reference;
    draw->state.vertex_buffer_mask = self->vertex_buffer_mask;
    draw->state.render_target_count = self->render_target_count;
    draw->state.depth_stencil = self->depth_stencil;
    copy_used_bindings(draw->state.bindings, self->graphics_bindings);
    draw->state.descriptor_heap_count = self->descriptor_heap_count;
    for (uint32_t index = 0; index < self->descriptor_heap_count; ++index) {
        BeerD3D12DescriptorHeap *heap = self->descriptor_heaps[index];
        if (!heap) continue;
        descriptor_heap_add_ref(heap);
        draw->state.descriptor_heaps[index] = heap;
        draw->state.descriptor_heap_gpu_start[index] = heap->gpu_start;
        draw->state.descriptor_heap_storage[index] = heap->storage;
        draw->state.descriptor_heap_size[index] = heap->storage_size;
        draw->state.descriptor_heap_type[index] = heap->description[0];
        draw->state.descriptor_heap_increment[index] =
            descriptor_stride(heap->description[0]);
    }
    memcpy(draw->state.render_targets, self->render_targets,
           sizeof(draw->state.render_targets));
    for (uint32_t index = 0; index < self->render_target_count; ++index) {
        BeerD3D12Resource *target = self->render_targets[index];
        if (!target) continue;
        resource_add_ref(target);
        draw->state.render_target_serial_location[index] =
            &target->content_serial;
        draw->state.render_target_storage[index] = target->storage;
        draw->state.render_target_storage_size[index] = target->storage_size;
        draw->state.render_target_serial[index] = target->content_serial;
        draw->state.render_target_width[index] =
            target->description.width > UINT32_MAX
                ? UINT32_MAX : (uint32_t)target->description.width;
        draw->state.render_target_height[index] = target->description.height;
        draw->state.render_target_format[index] = target->description.format;
    }
    root_signature_resolve_draw_descriptors(self, &draw->state);
    /* Draw execution is deferred until queue submission. Snapshot constant
     * bytes now because RE8 reuses its upload/constant-buffer ring before the
     * recorded draw is consumed. Resource references alone do not make those
     * mutable bytes immutable. */
    for (uint32_t parameter = 0;
         parameter < BEER_D3D12_MAX_ROOT_PARAMETERS; ++parameter) {
        BeerD3D12ComputeBinding *binding = &draw->state.bindings[parameter];
        if (binding->kind == 3 && binding->buffer_location) {
            const uint8_t *storage = NULL;
            BeerD3D12Resource *resource = resource_registry_resolve_buffer(
                binding->buffer_location, 1, &storage);
            if (resource) {
                size_t offset = (size_t)(binding->buffer_location -
                                         resource->gpu_virtual_address);
                size_t size = resource->storage_size - offset;
                if (size > 65536) size = 65536;
                uint8_t *copy = size ? snapshot_allocate(size) : NULL;
                if (copy) {
                    memcpy(copy, storage, size);
                    draw->root_binding_copies[parameter] = copy;
                    draw->root_binding_copy_sizes[parameter] = size;
                    binding->resource_storage = copy;
                    binding->resource_storage_size = size;
                    binding->resource = resource;
                    binding->resolved_root_resource = resource;
                } else {
                    binding->resource_storage = NULL;
                    binding->resource_storage_size = 0;
                    resource_release(resource);
                }
            }
        }
        for (uint32_t descriptor = 0;
             descriptor < binding->descriptor_count; ++descriptor) {
            if (binding->descriptor_types[descriptor] != 2 ||
                !binding->descriptor_resource_storage[descriptor] ||
                !binding->descriptor_resource_storage_size[descriptor])
                continue;
            size_t size = binding->descriptor_resource_storage_size[descriptor];
            if (size > 65536) size = 65536;
            uint8_t *copy = snapshot_allocate(size);
            if (!copy) {
                binding->descriptor_resource_storage[descriptor] = NULL;
                binding->descriptor_resource_storage_size[descriptor] = 0;
                continue;
            }
            memcpy(copy, binding->descriptor_resource_storage[descriptor], size);
            draw->descriptor_copies[parameter][descriptor] = copy;
            draw->descriptor_copy_sizes[parameter][descriptor] = size;
            binding->descriptor_resource_storage[descriptor] = copy;
            binding->descriptor_resource_storage_size[descriptor] = size;
        }
    }
    if (self->depth_stencil) {
        resource_add_ref(self->depth_stencil);
        draw->state.depth_stencil_serial_location =
            &self->depth_stencil->content_serial;
        draw->state.depth_stencil_storage = self->depth_stencil->storage;
        draw->state.depth_stencil_storage_size =
            self->depth_stencil->storage_size;
        draw->state.depth_stencil_source_bytes_per_pixel = 4u;
        if (self->depth_stencil->description.format == 19 ||
            self->depth_stencil->description.format == 20) {
            uint64_t texels = self->depth_stencil->description.width;
            if (self->depth_stencil->description.height &&
                texels <= UINT64_MAX /
                    self->depth_stencil->description.height)
                texels *= self->depth_stencil->description.height;
            if (texels <= SIZE_MAX / 8u &&
                self->depth_stencil->storage_size >= (size_t)texels * 8u)
                draw->state.depth_stencil_source_bytes_per_pixel = 8u;
        }
        draw->state.depth_stencil_width =
            self->depth_stencil->description.width > UINT32_MAX ? UINT32_MAX :
            (uint32_t)self->depth_stencil->description.width;
        draw->state.depth_stencil_height =
            self->depth_stencil->description.height;
        draw->state.depth_stencil_format =
            self->depth_stencil->description.format;
    }
    memcpy(draw->state.vertex_buffers, self->vertex_buffers,
           sizeof(draw->state.vertex_buffers));
    for (uint32_t view = 0; view < BEER_D3D12_MAX_VERTEX_BUFFERS; ++view) {
        if (!(self->vertex_buffer_mask & (UINT32_C(1) << view))) continue;
        const uint8_t *storage = NULL;
        BeerD3D12Resource *resource = resource_registry_resolve_buffer(
            self->vertex_buffers[view].address,
            self->vertex_buffers[view].size, &storage);
        if (!resource) continue;
        draw->vertex_buffer_resources[view] = resource;
        draw->state.vertex_buffer_resources[view] = resource;
        draw->state.vertex_buffer_storage_size[view] =
            self->vertex_buffers[view].size;
        /* Upload/readback heaps are CPU rings and must be frozen at Draw time.
         * Default/custom GPU heaps can be produced by earlier commands in the
         * same submission, so retaining a pre-execution copy would permanently
         * hide those writes from the draw. Keep their retained live storage. */
        if (resource->heap_properties[0] == 2 ||
            resource->heap_properties[0] == 3) {
            uint8_t *copy = snapshot_allocate(self->vertex_buffers[view].size);
            if (!copy) {
                resource_release(resource);
                draw->vertex_buffer_resources[view] = NULL;
                draw->state.vertex_buffer_resources[view] = NULL;
                draw->state.vertex_buffer_storage_size[view] = 0;
                continue;
            }
            memcpy(copy, storage, self->vertex_buffers[view].size);
            draw->vertex_buffer_copies[view] = copy;
            draw->vertex_buffer_copy_sizes[view] =
                self->vertex_buffers[view].size;
            draw->state.vertex_buffer_storage[view] = copy;
        } else {
            draw->state.vertex_buffer_storage[view] = storage;
        }
    }
    draw->state.index_buffer = self->index_buffer;
    if (self->index_buffer.address && self->index_buffer.size) {
        const uint8_t *storage = NULL;
        BeerD3D12Resource *resource = resource_registry_resolve_buffer(
            self->index_buffer.address, self->index_buffer.size, &storage);
        if (resource) {
            draw->index_buffer_resource = resource;
            draw->state.index_buffer_resource = resource;
            draw->state.index_buffer_storage_size = self->index_buffer.size;
            if (resource->heap_properties[0] == 2 ||
                resource->heap_properties[0] == 3) {
                uint8_t *copy = snapshot_allocate(self->index_buffer.size);
                if (copy) {
                    memcpy(copy, storage, self->index_buffer.size);
                    draw->index_buffer_copy = copy;
                    draw->index_buffer_copy_size = self->index_buffer.size;
                    draw->state.index_buffer_storage = copy;
                } else {
                    resource_release(resource);
                    draw->index_buffer_resource = NULL;
                    draw->state.index_buffer_resource = NULL;
                    draw->state.index_buffer_storage_size = 0;
                }
            } else {
                draw->state.index_buffer_storage = storage;
            }
        }
    }
    memcpy(draw->state.viewport, self->viewport, sizeof(draw->state.viewport));
    memcpy(draw->state.scissor, self->scissor, sizeof(draw->state.scissor));
    memcpy(draw->state.blend_factor, self->blend_factor,
           sizeof(draw->state.blend_factor));
    if (self->pipeline_state) {
        pipeline_state_add_ref(self->pipeline_state);
        draw->state.input_element_count =
            self->pipeline_state->input_element_count;
        memcpy(draw->state.input_elements,
               self->pipeline_state->input_elements,
               sizeof(draw->state.input_elements));
        draw->state.pso = self->pipeline_state->state;
        draw->state.vertex_shader = self->pipeline_state->vertex_shader;
        draw->state.vertex_shader_size =
            self->pipeline_state->vertex_shader_size;
        draw->state.vertex_shader_hash =
            self->pipeline_state->vertex_shader_hash;
        draw->state.pixel_shader = self->pipeline_state->pixel_shader;
        draw->state.pixel_shader_size = self->pipeline_state->pixel_shader_size;
        draw->state.pixel_shader_hash = self->pipeline_state->pixel_shader_hash;
    }
    if (self->graphics_root_signature) {
        root_signature_add_ref(self->graphics_root_signature);
        draw->state.root_signature_blob = self->graphics_root_signature->blob;
        draw->state.root_signature_blob_size =
            self->graphics_root_signature->blob_size;
    }
    if (!self->draw_tail) self->draw_tail = &self->draws;
    *self->draw_tail = draw;
    self->draw_tail = &draw->next;
}

static void __attribute__((ms_abi))
command_list_draw_instanced(BeerD3D12GraphicsCommandList *self,
                            uint32_t vertex_count, uint32_t instance_count,
                            uint32_t start_vertex, uint32_t start_instance)
{
    if (!self || self->closed || !vertex_count || !instance_count) return;
    command_list_record_draw(self, 0, vertex_count, instance_count,
                             start_vertex, 0, start_instance);
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
    if (!self || self->closed || !index_count || !instance_count) return;
    command_list_record_draw(self, 1, index_count, instance_count,
                             start_index, base_vertex, start_instance);
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
    if (render_target_count > BEER_D3D12_MAX_RENDER_TARGETS)
        render_target_count = BEER_D3D12_MAX_RENDER_TARGETS;
    memset(self->render_targets, 0, sizeof(self->render_targets));
    self->render_target_count = render_target_count;
    for (uint32_t index = 0; index < render_target_count; ++index) {
        uint64_t handle = single_handle_range
            ? render_targets[0] + (uint64_t)index * descriptor_stride(2)
            : render_targets[index];
        if (handle)
            self->render_targets[index] =
                cpu_descriptor_registry_get(handle, self->device);
    }
    self->depth_stencil = NULL;
    if (depth_stencil && *depth_stencil)
        self->depth_stencil =
            cpu_descriptor_registry_get(*depth_stencil, self->device);
    ++self->operation_counts[BEER_D3D12_OP_SET_RENDER_TARGETS];
    D3D12_TRACE("[D3D12] OMSetRenderTargets count=%u contiguous=%u rtv=%llx "
                "dsv=%llx\n", render_target_count, single_handle_range != 0,
                (unsigned long long)(render_target_count ? render_targets[0] : 0),
                (unsigned long long)(depth_stencil ? *depth_stencil : 0));
}

static void __attribute__((ms_abi))
command_list_set_primitive_topology(BeerD3D12GraphicsCommandList *self,
                                    uint32_t primitive_topology)
{
    if (!self || self->closed) return;
    self->primitive_topology = primitive_topology;
}

static void __attribute__((ms_abi))
command_list_set_viewports(BeerD3D12GraphicsCommandList *self,
                           uint32_t count, const void *viewports)
{
    if (!self || self->closed || (count && !viewports)) return;
    memset(self->viewport, 0, sizeof(self->viewport));
    if (!count) return;
    /* GCC's ms_abi implementation can miscompile aggregate float copies in
     * this boundary. Copy each 32-bit lane explicitly to preserve the exact
     * D3D12_VIEWPORT bytes supplied by the guest. */
    uint32_t *destination = (uint32_t *)self->viewport;
    const uint32_t *source = viewports;
    for (uint32_t lane = 0; lane < 6; ++lane) destination[lane] = source[lane];
}

static void __attribute__((ms_abi))
command_list_set_scissor_rects(BeerD3D12GraphicsCommandList *self,
                               uint32_t count, const void *rects)
{
    if (!self || self->closed || (count && !rects)) return;
    memset(self->scissor, 0, sizeof(self->scissor));
    if (!count) return;
    const int32_t *source = rects;
    for (uint32_t lane = 0; lane < 4; ++lane) self->scissor[lane] = source[lane];
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

static void __attribute__((ms_abi))
command_list_set_stencil_reference(BeerD3D12GraphicsCommandList *self,
                                   uint32_t stencil_reference)
{
    if (!self || self->closed) return;
    self->stencil_reference = stencil_reference;
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
    if (!self || self->closed ||
        root_parameter >= BEER_D3D12_MAX_ROOT_PARAMETERS)
        return;
    BeerD3D12ComputeBinding *binding = &self->graphics_bindings[root_parameter];
    binding->kind = 1;
    binding->descriptor_table = descriptor;
}

static void command_list_set_root_constants(
    BeerD3D12GraphicsCommandList *self, BeerD3D12ComputeBinding *bindings,
    uint32_t root_parameter, uint32_t count, const void *data,
    uint32_t destination_offset)
{
    if (!self || self->closed || !data ||
        root_parameter >= BEER_D3D12_MAX_ROOT_PARAMETERS ||
        destination_offset >= BEER_D3D12_MAX_ROOT_CONSTANTS)
        return;
    if (count > BEER_D3D12_MAX_ROOT_CONSTANTS - destination_offset)
        count = BEER_D3D12_MAX_ROOT_CONSTANTS - destination_offset;
    BeerD3D12ComputeBinding *binding = &bindings[root_parameter];
    binding->kind = 2;
    memcpy(binding->constants + destination_offset, data,
           (size_t)count * sizeof(uint32_t));
    uint32_t end = destination_offset + count;
    if (end > binding->constant_count) binding->constant_count = end;
}

static void __attribute__((ms_abi))
command_list_set_compute_root_constant(
    BeerD3D12GraphicsCommandList *self, uint32_t root_parameter,
    uint32_t value, uint32_t destination_offset)
{
    command_list_set_root_constants(self, self->compute_bindings,
                                    root_parameter, 1, &value,
                                    destination_offset);
}

static void __attribute__((ms_abi))
command_list_set_graphics_root_constant(
    BeerD3D12GraphicsCommandList *self, uint32_t root_parameter,
    uint32_t value, uint32_t destination_offset)
{
    command_list_set_root_constants(self, self->graphics_bindings,
                                    root_parameter, 1, &value,
                                    destination_offset);
}

static void __attribute__((ms_abi))
command_list_set_compute_root_constants(
    BeerD3D12GraphicsCommandList *self, uint32_t root_parameter,
    uint32_t count, const void *data, uint32_t destination_offset)
{
    command_list_set_root_constants(self, self->compute_bindings,
                                    root_parameter, count, data,
                                    destination_offset);
}

static void __attribute__((ms_abi))
command_list_set_graphics_root_constants(
    BeerD3D12GraphicsCommandList *self, uint32_t root_parameter,
    uint32_t count, const void *data, uint32_t destination_offset)
{
    command_list_set_root_constants(self, self->graphics_bindings,
                                    root_parameter, count, data,
                                    destination_offset);
}

static void command_list_set_root_descriptor(
    BeerD3D12GraphicsCommandList *self, BeerD3D12ComputeBinding *bindings,
    uint32_t root_parameter, uint64_t address, uint32_t kind)
{
    if (!self || self->closed ||
        root_parameter >= BEER_D3D12_MAX_ROOT_PARAMETERS)
        return;
    BeerD3D12ComputeBinding *binding = &bindings[root_parameter];
    binding->kind = kind;
    binding->buffer_location = address;
}

static void __attribute__((ms_abi))
command_list_set_compute_root_cbv(BeerD3D12GraphicsCommandList *self,
                                  uint32_t root_parameter, uint64_t address)
{
    command_list_set_root_descriptor(self, self->compute_bindings,
                                     root_parameter, address, 3);
}

static void __attribute__((ms_abi))
command_list_set_graphics_root_cbv(BeerD3D12GraphicsCommandList *self,
                                   uint32_t root_parameter, uint64_t address)
{
    command_list_set_root_descriptor(self, self->graphics_bindings,
                                     root_parameter, address, 3);
}

static void __attribute__((ms_abi))
command_list_set_compute_root_srv(BeerD3D12GraphicsCommandList *self,
                                  uint32_t root_parameter, uint64_t address)
{
    command_list_set_root_descriptor(self, self->compute_bindings,
                                     root_parameter, address, 4);
}

static void __attribute__((ms_abi))
command_list_set_graphics_root_srv(BeerD3D12GraphicsCommandList *self,
                                   uint32_t root_parameter, uint64_t address)
{
    command_list_set_root_descriptor(self, self->graphics_bindings,
                                     root_parameter, address, 4);
}

static void __attribute__((ms_abi))
command_list_set_compute_root_uav(BeerD3D12GraphicsCommandList *self,
                                  uint32_t root_parameter, uint64_t address)
{
    command_list_set_root_descriptor(self, self->compute_bindings,
                                     root_parameter, address, 5);
}

static void __attribute__((ms_abi))
command_list_set_graphics_root_uav(BeerD3D12GraphicsCommandList *self,
                                   uint32_t root_parameter, uint64_t address)
{
    command_list_set_root_descriptor(self, self->graphics_bindings,
                                     root_parameter, address, 5);
}

static void __attribute__((ms_abi))
command_list_set_index_buffer(BeerD3D12GraphicsCommandList *self,
                              const void *view)
{
    if (!self || self->closed) return;
    memset(&self->index_buffer, 0, sizeof(self->index_buffer));
    if (view) memcpy(&self->index_buffer, view, sizeof(self->index_buffer));
}

static void __attribute__((ms_abi))
command_list_set_vertex_buffers(BeerD3D12GraphicsCommandList *self,
                                uint32_t start_slot, uint32_t count,
                                const void *views)
{
    if (!self || self->closed || (count && !views) ||
        start_slot >= BEER_D3D12_MAX_VERTEX_BUFFERS)
        return;
    if (count > BEER_D3D12_MAX_VERTEX_BUFFERS - start_slot)
        count = BEER_D3D12_MAX_VERTEX_BUFFERS - start_slot;
    if (!count) return;
    memcpy(self->vertex_buffers + start_slot, views,
           (size_t)count * sizeof(self->vertex_buffers[0]));
    uint32_t mask = count == 32 ? UINT32_MAX : ((UINT32_C(1) << count) - 1u);
    self->vertex_buffer_mask |= mask << start_slot;
}

struct BeerD3D12Rect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
};

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

static void apply_depth_stencil_clear(BeerD3D12Resource *resource,
                                      uint32_t flags, uint32_t depth_bits,
                                      uint8_t stencil, uint32_t rect_count,
                                      const BeerD3D12Rect *rects)
{
    if (!resource || !resource->storage) return;
    uint32_t width = resource->description.width > UINT32_MAX ? UINT32_MAX :
        (uint32_t)resource->description.width;
    uint32_t height = resource->description.height;
    uint32_t texel_bytes = resource->description.format == 40 ? 4u :
        (resource->description.format == 19 ||
         resource->description.format == 20) ? 8u : 0u;
    if (!width || !height || !texel_bytes ||
        width > SIZE_MAX / texel_bytes ||
        (size_t)width * texel_bytes > SIZE_MAX / height ||
        (size_t)width * height * texel_bytes > resource->storage_size)
        return;
    uint32_t rectangle_count = rect_count ? rect_count : 1u;
    BeerD3D12Rect whole = {0, 0, (int32_t)width, (int32_t)height};
    const BeerD3D12Rect *rectangles = rect_count ? rects : &whole;
    for (uint32_t rectangle = 0; rectangle < rectangle_count; ++rectangle) {
        int32_t left = rectangles[rectangle].left < 0 ? 0 :
            rectangles[rectangle].left;
        int32_t top = rectangles[rectangle].top < 0 ? 0 :
            rectangles[rectangle].top;
        int32_t right = rectangles[rectangle].right > (int32_t)width ?
            (int32_t)width : rectangles[rectangle].right;
        int32_t bottom = rectangles[rectangle].bottom > (int32_t)height ?
            (int32_t)height : rectangles[rectangle].bottom;
        if (right <= left || bottom <= top) continue;
        for (int32_t y = top; y < bottom; ++y) {
            uint8_t *row = resource->storage +
                (size_t)y * width * texel_bytes;
            for (int32_t x = left; x < right; ++x) {
                uint8_t *texel = row + (size_t)x * texel_bytes;
                if (flags & 1u) memcpy(texel, &depth_bits, 4);
                if ((flags & 2u) && texel_bytes == 8u) texel[4] = stencil;
            }
        }
    }
    ++resource->content_serial;
}

static void __attribute__((ms_abi))
command_list_clear_depth_stencil_view(BeerD3D12GraphicsCommandList *self,
                                      uint64_t descriptor, uint32_t flags,
                                      float depth, uint8_t stencil,
                                      uint32_t rect_count, const void *rects)
{
    if (!self || self->closed || !descriptor || (rect_count && !rects)) return;
    BeerD3D12Resource *resource =
        cpu_descriptor_registry_get(descriptor, self->device);
    if (!resource || resource->vtable != g_resource_vtable ||
        !resource->storage)
        return;

    /* Preserve command-list ordering. Mutating the resource while recording
     * lets a later clear overwrite the backing seen by every earlier recorded
     * draw. Store an immutable clear command and replay it at queue submission
     * alongside dispatches and draws in sequence order. */
    BeerD3D12RecordedDepthClear *clear = calloc(1, sizeof(*clear));
    if (!clear) return;
    clear->resource = resource;
    clear->flags = flags;
    memcpy(&clear->depth_bits, &depth, sizeof(clear->depth_bits));
    clear->stencil = stencil;
    clear->rect_count = rect_count;
    clear->sequence = ++self->recording_sequence;
    if (rect_count) {
        if (rect_count > SIZE_MAX / sizeof(*clear->rects)) {
            free(clear);
            return;
        }
        clear->rects = malloc((size_t)rect_count * sizeof(*clear->rects));
        if (!clear->rects) {
            free(clear);
            return;
        }
        memcpy(clear->rects, rects,
               (size_t)rect_count * sizeof(*clear->rects));
    }
    resource_add_ref(resource);
    if (!self->depth_clear_tail)
        self->depth_clear_tail = &self->depth_clears;
    *self->depth_clear_tail = clear;
    self->depth_clear_tail = &clear->next;
    ++self->operation_counts[BEER_D3D12_OP_CLEAR_DSV];
    D3D12_TRACE("[D3D12] ClearDepthStencilView resource=%p flags=%x "
                "depth=%g stencil=%u rects=%u serial=%llu\n",
                (void *)resource, flags, (double)depth, (unsigned)stencil,
                rect_count, (unsigned long long)resource->content_serial);
}

static void apply_render_target_clear(BeerD3D12Resource *resource,
                                      const uint32_t color_bits[4],
                                      uint32_t rect_count,
                                      const BeerD3D12Rect *rects)
{
    if (!resource || !resource->storage || !color_bits) return;
    float color[4];
    memcpy(color, color_bits, sizeof(color));
    uint64_t input_serial = resource->content_serial;
    uint64_t output_serial = input_serial + 1;
    if (!rect_count && g_clear_target &&
        g_clear_target(resource, input_serial, output_serial,
                       resource->storage, resource->storage_size,
                       (uint32_t)resource->description.width,
                       resource->description.height,
                       resource->description.format, color)) {
        resource->content_serial = output_serial;
        return;
    }
    uint8_t rgba[4] = {
        clear_channel(color[0]), clear_channel(color[1]),
        clear_channel(color[2]), clear_channel(color[3]),
    };
    if (!rect_count) {
        clear_render_target_rectangle(resource, rgba, NULL);
    } else {
        for (uint32_t index = 0; index < rect_count; ++index)
            clear_render_target_rectangle(resource, rgba, &rects[index]);
    }
    resource->content_serial = output_serial;
}

static void __attribute__((ms_abi))
command_list_clear_render_target_view(BeerD3D12GraphicsCommandList *self,
                                      uint64_t descriptor,
                                      const float *color, uint32_t rect_count,
                                      const void *rects)
{
    if (!self || self->closed || !descriptor || !color ||
        (rect_count && !rects))
        return;
    BeerD3D12Resource *resource =
        cpu_descriptor_registry_get(descriptor, self->device);
    if (!resource || resource->vtable != g_resource_vtable ||
        !resource->storage || resource->description.dimension != 3 ||
        (resource->description.format != 28 &&
         resource->description.format != 29 &&
         resource->description.format != 87 &&
         resource->description.format != 91))
        return;
    uint64_t required = resource->description.width *
        (uint64_t)resource->description.height * 4;
    if (required > resource->storage_size) return;

    BeerD3D12RecordedColorClear *clear = calloc(1, sizeof(*clear));
    if (!clear) return;
    clear->resource = resource;
    memcpy(clear->color_bits, color, sizeof(clear->color_bits));
    clear->rect_count = rect_count;
    clear->sequence = ++self->recording_sequence;
    if (rect_count) {
        if (rect_count > SIZE_MAX / sizeof(*clear->rects)) {
            free(clear);
            return;
        }
        clear->rects = malloc((size_t)rect_count * sizeof(*clear->rects));
        if (!clear->rects) {
            free(clear);
            return;
        }
        memcpy(clear->rects, rects,
               (size_t)rect_count * sizeof(*clear->rects));
    }
    resource_add_ref(resource);
    if (!self->color_clear_tail)
        self->color_clear_tail = &self->color_clears;
    *self->color_clear_tail = clear;
    self->color_clear_tail = &clear->next;
    ++self->operation_counts[BEER_D3D12_OP_CLEAR_RTV];
    D3D12_TRACE("[D3D12] ClearRenderTargetView recorded resource=%p "
                "%llux%u format=%u rects=%u sequence=%llu\n",
                (void *)resource,
                (unsigned long long)resource->description.width,
                resource->description.height, resource->description.format,
                rect_count, (unsigned long long)clear->sequence);
}

static void __attribute__((ms_abi))
command_list_clear_unordered_access_view_uint(
    BeerD3D12GraphicsCommandList *self, uint64_t gpu_descriptor,
    uint64_t cpu_descriptor, BeerD3D12Resource *resource,
    const uint32_t values[4], uint32_t rect_count, const void *rects)
{
    (void)gpu_descriptor;
    (void)cpu_descriptor;
    if (!self || self->closed || !resource || !values ||
        resource->vtable != g_resource_vtable ||
        resource->device != self->device || !resource->storage ||
        (rect_count && !rects))
        return;

    /* RE8 currently clears whole CPU-backed UAV buffers. Repeat the native
     * four-component UINT clear value across storage; a trailing partial
     * element is initialized without overrunning the allocation. */
    if (rect_count != 0) {
        D3D12_TRACE("[D3D12] ClearUnorderedAccessViewUint resource=%p "
                    "rects=%u unsupported-partial-clear\n",
                    (void *)resource, rect_count);
        return;
    }
    const uint8_t *pattern = (const uint8_t *)values;
    for (size_t offset = 0; offset < resource->storage_size; offset += 16) {
        size_t bytes = resource->storage_size - offset;
        if (bytes > 16) bytes = 16;
        memcpy(resource->storage + offset, pattern, bytes);
    }
    ++resource->content_serial;
    ++self->operation_counts[BEER_D3D12_OP_CLEAR_UAV_UINT];
    D3D12_TRACE("[D3D12] ClearUnorderedAccessViewUint resource=%p bytes=%zu "
                "value=%08x,%08x,%08x,%08x serial=%llu\n",
                (void *)resource, resource->storage_size, values[0], values[1],
                values[2], values[3],
                (unsigned long long)resource->content_serial);
}

static void __attribute__((ms_abi))
command_list_discard_resource(BeerD3D12GraphicsCommandList *self,
                              BeerD3D12Resource *resource,
                              const void *region)
{
    if (!self || self->closed || !resource ||
        resource->vtable != g_resource_vtable ||
        resource->device != self->device)
        return;
    /* DiscardResource makes prior contents undefined; it does not release or
     * clear the allocation. Region metadata is only a driver optimization. */
    ++self->operation_counts[BEER_D3D12_OP_DISCARD_RESOURCE];
    D3D12_TRACE("[D3D12] DiscardResource resource=%p region=%p\n",
                (void *)resource, region);
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

static void apply_texture_copy(BeerD3D12RecordedTextureCopy *copy)
{
    if (!copy || !copy->destination || !copy->source_snapshot) return;
    BeerD3D12Resource *destination = copy->destination;
    size_t destination_offset = 0, destination_pitch = 0;
    uint32_t destination_width = 0, destination_height = 0;
    if (!resource_texture_layout(&destination->description,
                                 copy->destination_subresource,
                                 &destination_offset, &destination_pitch,
                                 &destination_width, &destination_height))
        return;
    uint32_t block_bytes = d3d12_format_block_bytes(copy->source_format);
    uint32_t unit_bytes = block_bytes ? block_bytes :
        d3d12_format_bytes_per_pixel(copy->source_format);
    uint32_t block_width = block_bytes ? 4u : 1u;
    uint32_t block_height = block_bytes ? 4u : 1u;
    if (copy->source_format != destination->description.format ||
        copy->destination_x % block_width || copy->destination_y % block_height ||
        copy->source_left % block_width || copy->source_top % block_height)
        return;
    uint32_t source_x = copy->source_left / block_width;
    uint32_t source_y = copy->source_top / block_height;
    if (copy->destination_x > destination_width ||
        copy->copy_width > destination_width - copy->destination_x ||
        copy->destination_y > destination_height ||
        copy->copy_height > destination_height - copy->destination_y)
        return;
    uint32_t destination_depth = destination->description.dimension == 4 ?
        destination->description.depth_or_array_size >>
            (copy->destination_subresource %
             (destination->description.mip_levels ?
              destination->description.mip_levels : 1)) : 1;
    if (!destination_depth) destination_depth = 1;
    if (copy->destination_z > destination_depth ||
        copy->copy_depth > destination_depth - copy->destination_z)
        return;
    uint32_t destination_x = copy->destination_x / block_width;
    uint32_t destination_y = copy->destination_y / block_height;
    uint32_t columns = (copy->copy_width + block_width - 1u) / block_width;
    uint32_t rows = (copy->copy_height + block_height - 1u) / block_height;
    size_t row_bytes = (size_t)columns * unit_bytes;
    size_t source_slice = (size_t)copy->source_row_pitch *
        ((copy->source_height + block_height - 1u) / block_height);
    size_t destination_slice = destination_pitch *
        ((destination_height + block_height - 1u) / block_height);
    if (g_end_compute_batch) g_end_compute_batch();
    if (g_forget_resource) g_forget_resource(destination);
    for (uint32_t z = 0; z < copy->copy_depth; ++z) {
        for (uint32_t row = 0; row < rows; ++row) {
            size_t source_offset = (size_t)(copy->source_front + z) * source_slice +
                (size_t)(source_y + row) * copy->source_row_pitch +
                (size_t)source_x * unit_bytes;
            size_t target_offset = destination_offset +
                (size_t)(copy->destination_z + z) * destination_slice +
                (size_t)(destination_y + row) * destination_pitch +
                (size_t)destination_x * unit_bytes;
            if (source_offset > copy->source_snapshot_size ||
                target_offset > destination->storage_size ||
                row_bytes > copy->source_snapshot_size - source_offset ||
                row_bytes > destination->storage_size - target_offset)
                return;
            memmove(destination->storage + target_offset,
                    copy->source_snapshot + source_offset, row_bytes);
        }
    }
    ++destination->content_serial;
}

static void __attribute__((ms_abi))
command_list_copy_texture_region(BeerD3D12GraphicsCommandList *self,
                                 const void *destination_location_ptr,
                                 uint32_t destination_x, uint32_t destination_y,
                                 uint32_t destination_z,
                                 const void *source_location_ptr,
                                 const void *source_box)
{
    if (!self || self->closed || !destination_location_ptr ||
        !source_location_ptr)
        return;
    const BeerD3D12TextureCopyLocation *destination_location =
        destination_location_ptr;
    const BeerD3D12TextureCopyLocation *source_location = source_location_ptr;
    BeerD3D12Resource *destination = destination_location->resource;
    BeerD3D12Resource *source = source_location->resource;
    if (!destination || !source || destination->device != self->device ||
        source->device != self->device || destination_location->type != 0 ||
        source_location->type != 1 || destination->description.dimension == 1 ||
        source->description.dimension != 1)
        return;

    uint32_t left = 0, top = 0, front = 0;
    uint32_t right = source_location->value.footprint.width;
    uint32_t bottom = source_location->value.footprint.height;
    uint32_t back = source_location->value.footprint.depth;
    if (source_box) {
        const uint32_t *box = source_box;
        left = box[0]; top = box[1]; front = box[2];
        right = box[3]; bottom = box[4]; back = box[5];
    }
    if (right <= left || bottom <= top || back <= front ||
        right > source_location->value.footprint.width ||
        bottom > source_location->value.footprint.height ||
        back > source_location->value.footprint.depth)
        return;
    uint32_t block_bytes = d3d12_format_block_bytes(
        source_location->value.footprint.format);
    uint32_t unit_bytes = block_bytes ? block_bytes : d3d12_format_bytes_per_pixel(
        source_location->value.footprint.format);
    uint32_t block_height = block_bytes ? 4u : 1u;
    uint32_t rows = (source_location->value.footprint.height + block_height - 1u) /
        block_height;
    if (!unit_bytes || !source_location->value.footprint.row_pitch ||
        rows > SIZE_MAX / source_location->value.footprint.row_pitch)
        return;
    size_t slice = (size_t)rows * source_location->value.footprint.row_pitch;
    if (!slice || source_location->value.footprint.depth > SIZE_MAX / slice)
        return;
    size_t snapshot_size = slice * source_location->value.footprint.depth;
    uint64_t source_offset = source_location->value.footprint.offset;
    if (source_offset > source->storage_size ||
        snapshot_size > source->storage_size - source_offset)
        return;

    BeerD3D12RecordedTextureCopy *copy = calloc(1, sizeof(*copy));
    if (!copy) return;
    copy->source_snapshot = snapshot_allocate(snapshot_size);
    if (!copy->source_snapshot) { free(copy); return; }
    memcpy(copy->source_snapshot, source->storage + source_offset, snapshot_size);
    copy->source_snapshot_size = snapshot_size;
    copy->destination = destination;
    copy->destination_subresource = destination_location->value.subresource_index;
    copy->destination_x = destination_x;
    copy->destination_y = destination_y;
    copy->destination_z = destination_z;
    copy->source_format = source_location->value.footprint.format;
    copy->source_width = source_location->value.footprint.width;
    copy->source_height = source_location->value.footprint.height;
    copy->source_depth = source_location->value.footprint.depth;
    copy->source_row_pitch = source_location->value.footprint.row_pitch;
    copy->source_left = left; copy->source_top = top; copy->source_front = front;
    copy->copy_width = right - left;
    copy->copy_height = bottom - top;
    copy->copy_depth = back - front;
    copy->sequence = ++self->recording_sequence;
    resource_add_ref(destination);
    if (!self->texture_copy_tail) self->texture_copy_tail = &self->texture_copies;
    *self->texture_copy_tail = copy;
    self->texture_copy_tail = &copy->next;
    ++self->operation_counts[BEER_D3D12_OP_COPY_TEXTURE];
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

static void pipeline_state_copy_shader(const BeerD3D12PipelineState *object,
                                       size_t pointer_offset,
                                       const uint8_t **owned_output,
                                       size_t *size_output,
                                       uint64_t *hash_output)
{
    if (!object || !owned_output || !size_output || !hash_output ||
        pointer_offset + 16 > object->stream_size)
        return;
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
    *owned_output = owned_shader;
    *size_output = shader_size;
    *hash_output = hash_shader_bytes(owned_shader, shader_size);
}

static void pipeline_state_set_compute_shader(BeerD3D12PipelineState *object,
                                              size_t pointer_offset)
{
    pipeline_state_copy_shader(object, pointer_offset,
                               &object->compute_shader,
                               &object->compute_shader_size,
                               &object->compute_shader_hash);
}

static void pipeline_state_set_graphics_shaders(BeerD3D12PipelineState *object,
                                                 size_t vertex_offset,
                                                 size_t pixel_offset)
{
    pipeline_state_copy_shader(object, vertex_offset, &object->vertex_shader,
                               &object->vertex_shader_size,
                               &object->vertex_shader_hash);
    pipeline_state_copy_shader(object, pixel_offset, &object->pixel_shader,
                               &object->pixel_shader_size,
                               &object->pixel_shader_hash);
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
    uint64_t result = create_pipeline_state_copy(
        self, description_data, 656, riid, pipeline_state,
        "CreateGraphicsPipelineState");
    if (result == BEER_S_OK) {
        BeerD3D12PipelineState *object = *pipeline_state;
        pipeline_state_set_graphics_shaders(object, 8, 24);
        /* Win64 D3D12_GRAPHICS_PIPELINE_STATE_DESC: BlendState 120 (328
         * bytes), SampleMask 448, RasterizerState 452 (44), DepthStencil
         * 496 (52), InputLayout 552 ({pointer, count, padding}),
         * PrimitiveTopologyType 572, NumRenderTargets 576, RTVFormats 580,
         * DSVFormat 612, SampleDesc 616. */
        pipeline_state_copy_input_layout(object, 552);
        pipeline_state_parse_legacy_state(object);
    }
    return result;
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

typedef struct BeerD3D12InputElementDesc {
    const char *semantic_name;
    uint32_t semantic_index;
    uint32_t format;
    uint32_t input_slot;
    uint32_t aligned_byte_offset;
    uint32_t input_class;
    uint32_t instance_step_rate;
} BeerD3D12InputElementDesc;

static void pipeline_state_copy_input_layout(BeerD3D12PipelineState *object,
                                             size_t payload_offset)
{
    if (!object || payload_offset + 16 > object->stream_size) return;
    const BeerD3D12InputElementDesc *elements = NULL;
    uint32_t count = 0;
    memcpy(&elements, object->stream + payload_offset, sizeof(elements));
    memcpy(&count, object->stream + payload_offset + 8, sizeof(count));
    if (!elements || !count || count > BEER_D3D12_MAX_INPUT_ELEMENTS) return;
    for (uint32_t index = 0; index < count; ++index) {
        object->input_elements[index] = (BeerD3D12InputElement){
            .semantic_index = elements[index].semantic_index,
            .format = elements[index].format,
            .input_slot = elements[index].input_slot,
            .aligned_byte_offset = elements[index].aligned_byte_offset,
            .input_class = elements[index].input_class,
            .instance_step_rate = elements[index].instance_step_rate
        };
    }
    object->input_element_count = count;
}

static uint32_t pso_u32(const uint8_t *data)
{
    uint32_t value;
    memcpy(&value, data, sizeof(value));
    return value;
}

static void pso_parse_blend(BeerD3D12PipelineStateInfo *info,
                            const uint8_t *data)
{
    info->alpha_to_coverage = pso_u32(data);
    info->independent_blend = pso_u32(data + 4);
    for (uint32_t i = 0; i < BEER_D3D12_MAX_RENDER_TARGETS; ++i) {
        /* D3D12_RENDER_TARGET_BLEND_DESC: nine 32-bit fields, then a UINT8
         * write mask, padded to 40 bytes. */
        const uint8_t *rt = data + 8 + (size_t)i * 40;
        info->render_target[i] = (BeerD3D12RenderTargetBlend){
            .blend_enable = pso_u32(rt),
            .logic_op_enable = pso_u32(rt + 4),
            .src_blend = pso_u32(rt + 8),
            .dest_blend = pso_u32(rt + 12),
            .blend_op = pso_u32(rt + 16),
            .src_blend_alpha = pso_u32(rt + 20),
            .dest_blend_alpha = pso_u32(rt + 24),
            .blend_op_alpha = pso_u32(rt + 28),
            .logic_op = pso_u32(rt + 32),
            .write_mask = rt[36]
        };
    }
}

static void pso_parse_rasterizer(BeerD3D12PipelineStateInfo *info,
                                 const uint8_t *data)
{
    info->fill_mode = pso_u32(data);
    info->cull_mode = pso_u32(data + 4);
    info->front_counter_clockwise = pso_u32(data + 8);
    uint32_t bias = pso_u32(data + 12);
    memcpy(&info->depth_bias, &bias, sizeof(bias));
    uint32_t clamp = pso_u32(data + 16);
    memcpy(&info->depth_bias_clamp, &clamp, sizeof(clamp));
    uint32_t slope = pso_u32(data + 20);
    memcpy(&info->slope_scaled_depth_bias, &slope, sizeof(slope));
    info->depth_clip = pso_u32(data + 24);
    info->multisample = pso_u32(data + 28);
    info->antialiased_line = pso_u32(data + 32);
    info->forced_sample_count = pso_u32(data + 36);
    info->conservative_raster = pso_u32(data + 40);
}

static void pso_parse_depth_stencil(BeerD3D12PipelineStateInfo *info,
                                    const uint8_t *data)
{
    info->depth_enable = pso_u32(data);
    info->depth_write_mask = pso_u32(data + 4);
    info->depth_func = pso_u32(data + 8);
    info->stencil_enable = pso_u32(data + 12);
    info->stencil_read_mask = data[16];
    info->stencil_write_mask = data[17];
    info->stencil_front = (BeerD3D12StencilOp){
        pso_u32(data + 20), pso_u32(data + 24), pso_u32(data + 28),
        pso_u32(data + 32)};
    info->stencil_back = (BeerD3D12StencilOp){
        pso_u32(data + 36), pso_u32(data + 40), pso_u32(data + 44),
        pso_u32(data + 48)};
}

/* D3D12 defaults for subobjects absent from a pipeline-state stream. */
static void pso_set_defaults(BeerD3D12PipelineStateInfo *info)
{
    memset(info, 0, sizeof(*info));
    for (uint32_t i = 0; i < BEER_D3D12_MAX_RENDER_TARGETS; ++i) {
        info->render_target[i].src_blend = 2;   /* ONE */
        info->render_target[i].dest_blend = 1;  /* ZERO */
        info->render_target[i].blend_op = 1;    /* ADD */
        info->render_target[i].src_blend_alpha = 2;
        info->render_target[i].dest_blend_alpha = 1;
        info->render_target[i].blend_op_alpha = 1;
        info->render_target[i].logic_op = 4;    /* NOOP */
        info->render_target[i].write_mask = 0xf;
    }
    info->sample_mask = UINT32_MAX;
    info->fill_mode = 3;                         /* SOLID */
    info->cull_mode = 3;                         /* BACK */
    info->depth_clip = 1;
    info->depth_enable = 1;
    info->depth_write_mask = 1;                  /* ALL */
    info->depth_func = 2;                        /* LESS */
    info->stencil_read_mask = 0xff;
    info->stencil_write_mask = 0xff;
    info->sample_count = 1;
}

static void pipeline_state_parse_legacy_state(BeerD3D12PipelineState *object)
{
    if (!object || !object->stream || object->stream_size < 656) return;
    const uint8_t *desc = object->stream;
    BeerD3D12PipelineStateInfo *info = &object->state;
    pso_set_defaults(info);
    pso_parse_blend(info, desc + 120);
    info->sample_mask = pso_u32(desc + 448);
    pso_parse_rasterizer(info, desc + 452);
    pso_parse_depth_stencil(info, desc + 496);
    /* D3D12_GRAPHICS_PIPELINE_STATE_DESC::IBStripCutValue follows the input
     * layout and precedes PrimitiveTopologyType. */
    info->index_buffer_strip_cut_value = pso_u32(desc + 568);
    info->render_target_format_count = pso_u32(desc + 576);
    if (info->render_target_format_count > BEER_D3D12_MAX_RENDER_TARGETS)
        return;
    for (uint32_t i = 0; i < BEER_D3D12_MAX_RENDER_TARGETS; ++i)
        info->render_target_formats[i] = pso_u32(desc + 580 + (size_t)i * 4);
    info->depth_stencil_format = pso_u32(desc + 612);
    info->sample_count = pso_u32(desc + 616);
    info->valid = 1;
}

static void pipeline_state_parse_stream_state(BeerD3D12PipelineState *object)
{
    if (!object || !object->stream) return;
    BeerD3D12PipelineStateInfo *info = &object->state;
    pso_set_defaults(info);
    /* Each subobject is a pointer-aligned {type, payload} record; payloads
     * with 8-byte alignment (shaders, input layout, root signature) start at
     * +8 while 4-byte payloads start at +4. Walk strictly by size so payload
     * bytes cannot be mistaken for a subobject type. */
    static const struct { uint8_t align, size; } layout[] = {
        [0] = {8, 8},   [1] = {8, 16},  [2] = {8, 16},  [3] = {8, 16},
        [4] = {8, 16},  [5] = {8, 16},  [6] = {8, 16},  [7] = {8, 32},
        [8] = {4, 255}, /* BLEND is 328 bytes; handled below. */
        [9] = {4, 4},   [10] = {4, 44}, [11] = {4, 52}, [12] = {8, 16},
        [13] = {4, 4},  [14] = {4, 4},  [15] = {4, 36}, [16] = {4, 4},
        [17] = {4, 8},  [18] = {4, 4},  [19] = {8, 16}, [20] = {4, 4},
        [21] = {4, 56}, [22] = {8, 24}
    };
    size_t offset = 0;
    while (offset + 8 <= object->stream_size) {
        uint32_t type = pso_u32(object->stream + offset);
        if (type >= sizeof(layout) / sizeof(layout[0])) return;
        size_t align = layout[type].align;
        size_t size = type == 8 ? 328 : layout[type].size;
        size_t payload = (offset + 4 + align - 1) & ~(align - 1);
        if (payload + size > object->stream_size) return;
        const uint8_t *data = object->stream + payload;
        switch (type) {
        case 8: pso_parse_blend(info, data); break;
        case 9: info->sample_mask = pso_u32(data); break;
        case 10: pso_parse_rasterizer(info, data); break;
        case 11:
        case 21: pso_parse_depth_stencil(info, data); break;
        case 13: info->index_buffer_strip_cut_value = pso_u32(data); break;
        case 15:
            for (uint32_t i = 0; i < BEER_D3D12_MAX_RENDER_TARGETS; ++i)
                info->render_target_formats[i] = pso_u32(data + i * 4);
            info->render_target_format_count = pso_u32(data + 32);
            if (info->render_target_format_count >
                BEER_D3D12_MAX_RENDER_TARGETS)
                return;
            break;
        case 16: info->depth_stencil_format = pso_u32(data); break;
        case 17: info->sample_count = pso_u32(data); break;
        default: break;
        }
        offset = (payload + size + 7) & ~(size_t)7;
    }
    /* A trailing partial record means the stream was not decoded fully. */
    info->valid = offset == object->stream_size ||
                  offset + 8 > object->stream_size;
}

static void pipeline_state_scan_stream_shaders(BeerD3D12PipelineState *object)
{
    if (!object || !object->stream) return;
    /* Pipeline-state stream subobjects are pointer-aligned. Shader and input
     * layout subobjects contain a type followed by an aligned payload. Scan
     * aligned candidates and validate every pointer/count before retaining
     * state because stream subobject payload sizes vary by SDK revision. */
    for (size_t offset = 0; offset + 24 <= object->stream_size; offset += 8) {
        uint32_t type = UINT32_MAX;
        uint32_t padding = UINT32_MAX;
        memcpy(&type, object->stream + offset, sizeof(type));
        memcpy(&padding, object->stream + offset + 4, sizeof(padding));
        (void)padding; /* SDK stream padding is not required to be initialized. */
        if (type == 12 && !object->input_element_count) {
            pipeline_state_copy_input_layout(object, offset + 8);
            continue;
        }
        if (type != 1 && type != 2) continue;
        const uint8_t *shader = NULL;
        size_t shader_size = 0;
        memcpy(&shader, object->stream + offset + 8, sizeof(shader));
        memcpy(&shader_size, object->stream + offset + 16,
               sizeof(shader_size));
        if ((uintptr_t)shader < UINT64_C(0x10000) || shader_size < 32 ||
            shader_size > (size_t)64 * 1024 * 1024 ||
            memcmp(shader, "DXBC", 4) != 0)
            continue;
        if (type == 1 && !object->vertex_shader)
            pipeline_state_copy_shader(object, offset + 8,
                                       &object->vertex_shader,
                                       &object->vertex_shader_size,
                                       &object->vertex_shader_hash);
        else if (type == 2 && !object->pixel_shader)
            pipeline_state_copy_shader(object, offset + 8,
                                       &object->pixel_shader,
                                       &object->pixel_shader_size,
                                       &object->pixel_shader_hash);
    }
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
    uint64_t result = create_pipeline_state_copy(
        self, stream, stream_size, riid, pipeline_state,
        "Device2::CreatePipelineState");
    if (result == BEER_S_OK) {
        pipeline_state_scan_stream_shaders(*pipeline_state);
        pipeline_state_parse_stream_state(*pipeline_state);
    }
    return result;
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
        free((void *)self->vertex_shader);
        free((void *)self->pixel_shader);
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
