#include "d3d12_compat.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t g_signaled_event;
static uint32_t g_signal_count;
static uint32_t g_present_count;
static const void *g_present_resource;
static int g_present_width;
static int g_present_height;
static int g_present_pitch;
static uint8_t g_present_pixels[64];
static uint32_t g_clear_count;
static uint32_t g_compute_count;
static BeerD3D12ComputeDispatch g_last_compute;
static uint32_t g_submission_count;
static BeerD3D12SubmissionStats g_last_submission;

static void test_observe_submission(const BeerD3D12SubmissionStats *stats)
{
    if (!stats) return;
    g_last_submission = *stats;
    ++g_submission_count;
}

static int test_execute_compute(const BeerD3D12ComputeDispatch *dispatch)
{
    if (!dispatch || !dispatch->pipeline_state || !dispatch->shader ||
        !dispatch->shader_size || !dispatch->group_count_x ||
        !dispatch->group_count_y || !dispatch->group_count_z)
        return 0;
    g_last_compute = *dispatch;
    ++g_compute_count;
    return 1;
}

static int test_clear_target(const void *resource,
                             uint64_t input_serial,
                             uint64_t output_serial,
                             uint8_t *pixels, uint64_t bytes,
                             uint32_t width, uint32_t height,
                             uint32_t format, const float color[4])
{
    if (!resource || !pixels || bytes < (uint64_t)width * height * 4u ||
        !width || !height || !color || output_serial != input_serial + 1u ||
        (format != 28 && format != 29 && format != 87 && format != 91))
        return 0;
    uint8_t rgba[4];
    for (uint32_t channel = 0; channel < 4; ++channel) {
        float value = color[channel];
        rgba[channel] = !(value > 0.0f) ? 0 : value >= 1.0f
            ? 255 : (uint8_t)(value * 255.0f + 0.5f);
    }
    uint8_t pixel[4] = {rgba[0], rgba[1], rgba[2], rgba[3]};
    if (format == 87 || format == 91) {
        pixel[0] = rgba[2];
        pixel[2] = rgba[0];
    }
    for (uint64_t offset = 0; offset < (uint64_t)width * height * 4u; offset += 4)
        memcpy(pixels + offset, pixel, 4);
    ++g_clear_count;
    return 1;
}

static void test_signal_event(uint64_t event_handle)
{
    g_signaled_event = event_handle;
    ++g_signal_count;
}

static int test_present(const void *resource, uint64_t serial,
                        const uint8_t *pixels, int width, int height,
                        int row_pitch)
{
    if (!resource || !serial || !pixels) return 0;
    g_present_resource = resource;
    g_present_width = width;
    g_present_height = height;
    g_present_pitch = row_pitch;
    size_t byte_count = (size_t)row_pitch * (size_t)height;
    if (byte_count > sizeof(g_present_pixels)) byte_count = sizeof(g_present_pixels);
    memcpy(g_present_pixels, pixels, byte_count);
    ++g_present_count;
    return 1;
}

int main(void)
{
    beer_d3d12_set_event_signaler(test_signal_event);
    beer_d3d12_set_presenter(test_present);
    beer_d3d12_set_clear_target(test_clear_target);
    beer_d3d12_set_compute_executor(test_execute_compute);
    beer_d3d12_set_submission_observer(test_observe_submission);
    static const uint8_t iid_device[16] = {
        0xf1, 0x19, 0x98, 0x18, 0xb6, 0x1d, 0x57, 0x4b,
        0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7
    };
    void *output = (void *)(uintptr_t)0x1;
    uint64_t result = beer_d3d12_create_device(0, 0xc000, iid_device, &output);
    if (result != 0 || output == NULL) return 1;

    void **vtable = *(void ***)output;
    typedef uint32_t (__attribute__((ms_abi)) *GetNodeCountFn)(void *);
    typedef uint64_t (__attribute__((ms_abi)) *CreateAllocatorFn)(
        void *, uint32_t, const uint8_t *, void **);
    typedef uint64_t (__attribute__((ms_abi)) *QueryInterfaceFn)(
        void *, const uint8_t *, void **);
    if (((GetNodeCountFn)vtable[7])(output) != 1) return 2;

    /* An unset ID3D12Object private-data key is a valid query that returns
     * DXGI_ERROR_NOT_FOUND, not E_NOTIMPL or fabricated success. */
    typedef uint64_t (__attribute__((ms_abi)) *GetPrivateDataFn)(
        void *, const uint8_t *, uint32_t *, void *);
    static const uint8_t private_key[16] = {
        0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe,
        0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef
    };
    uint32_t private_size = 64;
    uint8_t private_data[64];
    memset(private_data, 0xa5, sizeof(private_data));
    if (((GetPrivateDataFn)vtable[3])(
            output, private_key, &private_size, private_data) !=
            UINT64_C(0x887a0002) || private_size != 0 ||
        private_data[0] != 0xa5)
        return 2;

    static const uint8_t iid_downlevel_device[16] = {
        0x3f, 0xee, 0xea, 0x74, 0x4b, 0x2f, 0x6d, 0x47,
        0x82, 0xba, 0x2b, 0x85, 0xcb, 0x49, 0xe3, 0x10
    };
    void *downlevel_device = NULL;
    if (((QueryInterfaceFn)vtable[0])(
            output, iid_downlevel_device, &downlevel_device) != 0 ||
        downlevel_device != output)
        return 2;
    typedef uint32_t (__attribute__((ms_abi)) *ReleaseFn)(void *);
    ((ReleaseFn)vtable[2])(downlevel_device);

    /* RE8 probes ID3D12Device2 and uses its stream-based pipeline creation
     * method at slot 47. The inherited pointer must retain the extended vtable. */
    static const uint8_t iid_device2[16] = {
        0x1e, 0xa4, 0xba, 0x30, 0x5b, 0xb1, 0x5c, 0x47,
        0xa0, 0xbb, 0x1a, 0xf5, 0xc5, 0xb6, 0x43, 0x28
    };
    static const uint8_t iid_pipeline_state[16] = {
        0xf3, 0x30, 0x5a, 0x76, 0x24, 0xf6, 0x6f, 0x4c,
        0xa8, 0x28, 0xac, 0xe9, 0x48, 0x62, 0x24, 0x45
    };
    void *device2 = NULL;
    if (((QueryInterfaceFn)vtable[0])(output, iid_device2, &device2) != 0 ||
        device2 != output)
        return 2;
    typedef struct PipelineStreamDesc {
        size_t size;
        void *stream;
    } PipelineStreamDesc;
    typedef uint64_t (__attribute__((ms_abi)) *CreatePipelineStateFn)(
        void *, const PipelineStreamDesc *, const uint8_t *, void **);
    uint8_t pipeline_bytes[32];
    for (size_t i = 0; i < sizeof(pipeline_bytes); ++i)
        pipeline_bytes[i] = (uint8_t)(i * 3u + 1u);
    PipelineStreamDesc pipeline_desc = {
        .size = sizeof(pipeline_bytes),
        .stream = pipeline_bytes,
    };
    void *pipeline_state = NULL;
    void **device2_vtable = *(void ***)device2;
    if (((CreatePipelineStateFn)device2_vtable[47])(
            device2, &pipeline_desc, iid_pipeline_state, &pipeline_state) != 0 ||
        !pipeline_state)
        return 2;
    void **pipeline_vtable = *(void ***)pipeline_state;
    typedef uint64_t (__attribute__((ms_abi)) *GetCachedBlobFn)(void *, void **);
    void *cached_blob = NULL;
    if (((GetCachedBlobFn)pipeline_vtable[8])(pipeline_state, &cached_blob) != 0 ||
        !cached_blob)
        return 2;
    void **cached_blob_vtable = *(void ***)cached_blob;
    typedef void *(__attribute__((ms_abi)) *GetBufferPointerFn)(void *);
    typedef size_t (__attribute__((ms_abi)) *GetBufferSizeFn)(void *);
    if (((GetBufferSizeFn)cached_blob_vtable[4])(cached_blob) !=
            sizeof(pipeline_bytes) ||
        memcmp(((GetBufferPointerFn)cached_blob_vtable[3])(cached_blob),
               pipeline_bytes, sizeof(pipeline_bytes)) != 0)
        return 2;
    ((ReleaseFn)cached_blob_vtable[2])(cached_blob);
    ((ReleaseFn)pipeline_vtable[2])(pipeline_state);
    ((ReleaseFn)vtable[2])(device2);

    /* Base-device slots 10 and 11 are the legacy graphics/compute PSO paths.
     * RE8 uses both these methods and Device2's stream method. */
    typedef uint64_t (__attribute__((ms_abi)) *CreateLegacyPipelineStateFn)(
        void *, const void *, const uint8_t *, void **);
    uint8_t graphics_desc[656];
    uint8_t compute_desc[56];
    memset(graphics_desc, 0x3c, sizeof(graphics_desc));
    memset(compute_desc, 0xa7, sizeof(compute_desc));
    pipeline_state = NULL;
    if (((CreateLegacyPipelineStateFn)vtable[10])(
            output, graphics_desc, iid_pipeline_state, &pipeline_state) != 0 ||
        !pipeline_state)
        return 2;
    pipeline_vtable = *(void ***)pipeline_state;
    cached_blob = NULL;
    if (((GetCachedBlobFn)pipeline_vtable[8])(pipeline_state, &cached_blob) != 0 ||
        !cached_blob)
        return 2;
    cached_blob_vtable = *(void ***)cached_blob;
    if (((GetBufferSizeFn)cached_blob_vtable[4])(cached_blob) !=
            sizeof(graphics_desc) ||
        memcmp(((GetBufferPointerFn)cached_blob_vtable[3])(cached_blob),
               graphics_desc, sizeof(graphics_desc)) != 0)
        return 2;
    ((ReleaseFn)cached_blob_vtable[2])(cached_blob);
    ((ReleaseFn)pipeline_vtable[2])(pipeline_state);

    static uint8_t compute_shader[] = {'D','X','B','C',1,2,3,4};
    void *compute_root = NULL;
    const void *compute_shader_pointer = compute_shader;
    size_t compute_shader_size = sizeof(compute_shader);
    memset(compute_desc, 0, sizeof(compute_desc));
    memcpy(compute_desc, &compute_root, sizeof(compute_root));
    memcpy(compute_desc + 8, &compute_shader_pointer,
           sizeof(compute_shader_pointer));
    memcpy(compute_desc + 16, &compute_shader_size, sizeof(compute_shader_size));
    pipeline_state = NULL;
    if (((CreateLegacyPipelineStateFn)vtable[11])(
            output, compute_desc, iid_pipeline_state, &pipeline_state) != 0 ||
        !pipeline_state)
        return 2;
    pipeline_vtable = *(void ***)pipeline_state;
    cached_blob = NULL;
    if (((GetCachedBlobFn)pipeline_vtable[8])(pipeline_state, &cached_blob) != 0 ||
        !cached_blob)
        return 2;
    cached_blob_vtable = *(void ***)cached_blob;
    if (((GetBufferSizeFn)cached_blob_vtable[4])(cached_blob) !=
            sizeof(compute_desc) ||
        memcmp(((GetBufferPointerFn)cached_blob_vtable[3])(cached_blob),
               compute_desc, sizeof(compute_desc)) != 0)
        return 2;
    ((ReleaseFn)cached_blob_vtable[2])(cached_blob);
    /* Keep this compute PSO alive for command-state/dispatch execution tests. */
    void *compute_pipeline_state = pipeline_state;

    static const uint8_t iid_downlevel_queue[16] = {
        0xef, 0xc5, 0xa8, 0x38, 0xcb, 0x7c, 0x81, 0x4e,
        0x91, 0x4f, 0xa6, 0xe9, 0xd0, 0x72, 0xc4, 0x94
    };
    typedef uint64_t (__attribute__((ms_abi)) *CreateQueueFn)(
        void *, const uint32_t *, const uint8_t *, void **);
    uint32_t queue_desc[4] = {0, 0, 0, 0};
    void *queue = NULL;
    if (((CreateQueueFn)vtable[8])(output, queue_desc, iid_device, &queue) != 0 ||
        !queue)
        return 3;
    void **queue_vtable = *(void ***)queue;
    void *downlevel_queue = NULL;
    if (((QueryInterfaceFn)queue_vtable[0])(
            queue, iid_downlevel_queue, &downlevel_queue) != 0 ||
        !downlevel_queue || downlevel_queue == queue)
        return 3;

    void *allocator = NULL;
    void *copy_allocator = NULL;
    if (((CreateAllocatorFn)vtable[9])(output, 0, iid_device, &allocator) != 0 ||
        allocator == NULL) return 3;
    if (((CreateAllocatorFn)vtable[9])(output, 2, iid_device,
                                      &copy_allocator) != 0 ||
        copy_allocator == NULL || copy_allocator == allocator) return 3;

    static const uint8_t iid_descriptor_heap[16] = {
        0x1d, 0x47, 0xfb, 0x8e, 0x6c, 0x61, 0x49, 0x4f,
        0x90, 0xf7, 0x12, 0x7b, 0xb7, 0x63, 0xfa, 0x51
    };
    typedef uint64_t (__attribute__((ms_abi)) *CreateHeapFn)(
        void *, const uint32_t *, const uint8_t *, void **);
    uint32_t heap_desc[4] = {0, 4, 1, 0};
    void *heap = NULL;
    void *second_heap = NULL;
    if (((CreateHeapFn)vtable[14])(output, heap_desc, iid_descriptor_heap,
                                  &heap) != 0 || heap == NULL) return 4;
    heap_desc[1] = 8;
    if (((CreateHeapFn)vtable[14])(output, heap_desc, iid_descriptor_heap,
                                  &second_heap) != 0 || second_heap == NULL ||
        second_heap == heap) return 4;
    void **heap_vtable = *(void ***)heap;
    typedef uint64_t *(__attribute__((ms_abi)) *GetHandleFn)(
        void *, uint64_t *);
    uint64_t cpu_handle = 0;
    uint64_t gpu_handle = 0;
    if (((GetHandleFn)heap_vtable[9])(heap, &cpu_handle) != &cpu_handle ||
        ((GetHandleFn)heap_vtable[10])(heap, &gpu_handle) != &gpu_handle ||
        cpu_handle == 0 || gpu_handle == 0)
        return 5;
    void **allocator_vtable = *(void ***)allocator;
    typedef uint64_t (__attribute__((ms_abi)) *ResetFn)(void *);
    if (((ResetFn)allocator_vtable[8])(allocator) != 0) return 4;

    static const uint8_t iid_command_list[16] = {
        0x0f, 0x0d, 0x16, 0x5b, 0x1b, 0xac, 0x85, 0x41,
        0x8b, 0xa8, 0xb3, 0xae, 0x42, 0xa5, 0xa4, 0x55
    };
    typedef uint64_t (__attribute__((ms_abi)) *CreateListFn)(
        void *, uint32_t, uint32_t, void *, void *, const uint8_t *, void **);
    void *command_list = NULL;
    if (((CreateListFn)vtable[12])(output, 0, 0, allocator, NULL,
                                  iid_command_list, &command_list) != 0 ||
        command_list == NULL) return 5;
    void **list_vtable = *(void ***)command_list;
    typedef uint64_t (__attribute__((ms_abi)) *CloseFn)(void *);
    typedef uint32_t (__attribute__((ms_abi)) *GetTypeFn)(void *);
    if (((GetTypeFn)list_vtable[8])(command_list) != 0) return 6;
    if (((CloseFn)list_vtable[9])(command_list) != 0) return 7;

    static const uint8_t iid_fence[16] = {
        0xcf, 0x3d, 0x75, 0x0a, 0xd8, 0xc4, 0x91, 0x4b,
        0xad, 0xf6, 0xbe, 0x5a, 0x60, 0xd9, 0x5a, 0x76
    };
    typedef uint64_t (__attribute__((ms_abi)) *CreateFenceFn)(
        void *, uint64_t, uint32_t, const uint8_t *, void **);
    void *fence = NULL;
    if (((CreateFenceFn)vtable[36])(output, 7, 0, iid_fence, &fence) != 0 ||
        fence == NULL) return 8;
    void **fence_vtable = *(void ***)fence;
    typedef uint64_t (__attribute__((ms_abi)) *GetCompletedFn)(void *);
    typedef uint64_t (__attribute__((ms_abi)) *SignalFn)(void *, uint64_t);
    typedef uint64_t (__attribute__((ms_abi)) *SetEventFn)(void *, uint64_t, void *);
    if (((GetCompletedFn)fence_vtable[8])(fence) != 7) return 9;

    void *pending_event = (void *)(uintptr_t)0x1234;
    if (((SetEventFn)fence_vtable[9])(fence, 11, pending_event) != 0 ||
        g_signal_count != 0)
        return 10;
    if (((SignalFn)fence_vtable[10])(fence, 11) != 0 ||
        ((GetCompletedFn)fence_vtable[8])(fence) != 11 ||
        g_signal_count != 1 || g_signaled_event != (uint64_t)(uintptr_t)pending_event)
        return 10;

    void *completed_event = (void *)(uintptr_t)0x5678;
    if (((SetEventFn)fence_vtable[9])(fence, 10, completed_event) != 0 ||
        g_signal_count != 2 || g_signaled_event != (uint64_t)(uintptr_t)completed_event)
        return 10;

    /* A queue wait is an asynchronous GPU dependency. A value that is still
     * pending must be accepted with S_OK and must not itself complete or signal
     * the fence. RE8 uses this contract while bringing up its graphics queues. */
    typedef uint64_t (__attribute__((ms_abi)) *QueueWaitFn)(
        void *, void *, uint64_t);
    if (((QueueWaitFn)queue_vtable[15])(queue, fence, 19) != 0 ||
        ((GetCompletedFn)fence_vtable[8])(fence) != 11 ||
        g_signal_count != 2)
        return 10;
    if (((SignalFn)fence_vtable[10])(fence, 19) != 0 ||
        ((GetCompletedFn)fence_vtable[8])(fence) != 19)
        return 10;

    static const uint8_t iid_query_heap[16] = {
        0xae, 0x58, 0x96, 0x0d, 0x45, 0xed, 0x9e, 0x46,
        0xa6, 0x1d, 0x97, 0x0e, 0xc5, 0x83, 0xca, 0xb4
    };
    typedef uint64_t (__attribute__((ms_abi)) *CreateQueryHeapFn)(
        void *, const uint32_t *, const uint8_t *, void **);
    typedef struct AllocationInfo {
        uint64_t size_in_bytes;
        uint64_t alignment;
    } AllocationInfo;
    typedef struct ResourceDesc {
        uint32_t dimension;
        uint32_t padding0;
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
        uint32_t padding1;
    } ResourceDesc;
    typedef AllocationInfo *(__attribute__((ms_abi)) *GetAllocationInfoFn)(
        void *, AllocationInfo *, uint32_t, uint32_t, const ResourceDesc *);
    typedef uint64_t (__attribute__((ms_abi)) *CreateCommittedResourceFn)(
        void *, const uint32_t *, uint32_t, const ResourceDesc *, uint32_t,
        const void *, const uint8_t *, void **);
    typedef uint32_t (__attribute__((ms_abi)) *ReleaseFn)(void *);
    typedef uint64_t (__attribute__((ms_abi)) *GetDeviceFn)(
        void *, const uint8_t *, void **);
    uint32_t query_desc[3] = {0, 8, 0};
    void *query_heap = NULL;
    void *second_query_heap = NULL;
    if (((CreateQueryHeapFn)vtable[39])(output, query_desc, iid_query_heap,
                                       &query_heap) != 0 ||
        query_heap == NULL) return 11;
    query_desc[1] = 16;
    if (((CreateQueryHeapFn)vtable[39])(output, query_desc, iid_query_heap,
                                       &second_query_heap) != 0 ||
        second_query_heap == NULL || second_query_heap == query_heap) return 11;
    void **query_vtable = *(void ***)query_heap;
    void *query_device = NULL;
    if (((GetDeviceFn)query_vtable[7])(query_heap, iid_device,
                                      &query_device) != 0 ||
        query_device != output) return 12;
    ((ReleaseFn)vtable[2])(query_device);
    if (((ReleaseFn)query_vtable[2])(query_heap) != 0) return 13;
    void **second_query_vtable = *(void ***)second_query_heap;
    if (((ReleaseFn)second_query_vtable[2])(second_query_heap) != 0) return 13;

    ResourceDesc buffer_desc = {0};
    buffer_desc.dimension = 1;
    buffer_desc.width = 0xe00;
    buffer_desc.height = 1;
    buffer_desc.depth_or_array_size = 1;
    buffer_desc.mip_levels = 1;
    buffer_desc.sample_count = 1;
    AllocationInfo allocation = {0};
    if (((GetAllocationInfoFn)vtable[25])(
            output, &allocation, 0, 1, &buffer_desc) != &allocation ||
        allocation.alignment != 65536 || allocation.size_in_bytes != 65536)
        return 14;

    static const uint8_t iid_resource[16] = {
        0xbe, 0x42, 0x64, 0x69, 0x2e, 0xa7, 0x59, 0x40,
        0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad
    };
    uint32_t heap_properties[5] = {2, 0, 0, 0, 0};
    void *resource = NULL;
    if (((CreateCommittedResourceFn)vtable[27])(
            output, heap_properties, 0, &buffer_desc, 1, NULL,
            iid_resource, &resource) != 0 || resource == NULL)
        return 15;
    void **resource_vtable = *(void ***)resource;
    typedef uint64_t (__attribute__((ms_abi)) *MapFn)(
        void *, uint32_t, const void *, void **);
    typedef ResourceDesc *(__attribute__((ms_abi)) *GetDescFn)(
        void *, ResourceDesc *);
    typedef uint64_t (__attribute__((ms_abi)) *GetGpuAddressFn)(void *);
    void *mapped = NULL;
    if (((MapFn)resource_vtable[8])(resource, 0, NULL, &mapped) != 0 ||
        mapped == NULL || ((GetGpuAddressFn)resource_vtable[11])(resource) == 0)
        return 16;
    ResourceDesc returned_desc = {0};
    if (((GetDescFn)resource_vtable[10])(resource, &returned_desc) !=
            &returned_desc || returned_desc.width != buffer_desc.width)
        return 17;
    if (((ReleaseFn)resource_vtable[2])(resource) != 0) return 18;

    static const uint8_t iid_heap[16] = {
        0x02, 0x25, 0x3b, 0x6b, 0x51, 0x6e, 0xb3, 0x45,
        0x90, 0xee, 0x98, 0x84, 0x26, 0x5e, 0x8d, 0xf3
    };
    typedef struct HeapDesc {
        uint64_t size_in_bytes;
        uint32_t properties[5];
        uint32_t padding;
        uint64_t alignment;
        uint32_t flags;
        uint32_t padding2;
    } HeapDesc;
    typedef uint64_t (__attribute__((ms_abi)) *CreateResourceHeapFn)(
        void *, const HeapDesc *, const uint8_t *, void **);
    typedef uint64_t (__attribute__((ms_abi)) *CreatePlacedResourceFn)(
        void *, void *, uint64_t, const ResourceDesc *, uint32_t,
        const void *, const uint8_t *, void **);
    typedef HeapDesc *(__attribute__((ms_abi)) *GetHeapDescFn)(
        void *, HeapDesc *);
    HeapDesc resource_heap_desc = {
        .size_in_bytes = 131072,
        .properties = {2, 0, 0, 0, 0},
        .alignment = 65536,
    };
    void *resource_heap = NULL;
    if (((CreateResourceHeapFn)vtable[28])(
            output, &resource_heap_desc, iid_heap, &resource_heap) != 0 ||
        resource_heap == NULL)
        return 19;
    void **resource_heap_vtable = *(void ***)resource_heap;
    HeapDesc returned_heap_desc = {0};
    if (((GetHeapDescFn)resource_heap_vtable[8])(
            resource_heap, &returned_heap_desc) != &returned_heap_desc ||
        returned_heap_desc.size_in_bytes != resource_heap_desc.size_in_bytes ||
        returned_heap_desc.alignment != resource_heap_desc.alignment)
        return 20;
    void *placed_resource = NULL;
    if (((CreatePlacedResourceFn)vtable[29])(
            output, resource_heap, 65536, &buffer_desc, 1, NULL,
            iid_resource, &placed_resource) != 0 || placed_resource == NULL)
        return 21;
    void **placed_vtable = *(void ***)placed_resource;
    mapped = NULL;
    if (((MapFn)placed_vtable[8])(placed_resource, 0, NULL, &mapped) != 0 ||
        mapped == NULL)
        return 22;
    ((uint8_t *)mapped)[0] = 0x5a;
    if (((ReleaseFn)placed_vtable[2])(placed_resource) != 0) return 23;
    if (((ReleaseFn)resource_heap_vtable[2])(resource_heap) != 0) return 24;

    /* Command-list buffer copies must preserve the exact requested range. */
    typedef uint64_t (__attribute__((ms_abi)) *ResetListFn)(
        void *, void *, void *);
    if (((ResetListFn)list_vtable[10])(command_list, allocator, NULL) != 0)
        return 25;
    void *copy_source = NULL;
    void *copy_destination = NULL;
    buffer_desc.width = 256;
    if (((CreateCommittedResourceFn)vtable[27])(
            output, heap_properties, 0, &buffer_desc, 1, NULL,
            iid_resource, &copy_source) != 0 || copy_source == NULL ||
        ((CreateCommittedResourceFn)vtable[27])(
            output, heap_properties, 0, &buffer_desc, 1, NULL,
            iid_resource, &copy_destination) != 0 || copy_destination == NULL)
        return 25;
    void **copy_source_vtable = *(void ***)copy_source;
    void **copy_destination_vtable = *(void ***)copy_destination;
    void *copy_source_data = NULL;
    void *copy_destination_data = NULL;
    if (((MapFn)copy_source_vtable[8])(copy_source, 0, NULL,
                                      &copy_source_data) != 0 ||
        ((MapFn)copy_destination_vtable[8])(copy_destination, 0, NULL,
                                           &copy_destination_data) != 0)
        return 26;
    for (uint32_t index = 0; index < 64; ++index)
        ((uint8_t *)copy_source_data)[32 + index] = (uint8_t)(index + 1);
    typedef void (__attribute__((ms_abi)) *CopyBufferFn)(
        void *, void *, uint64_t, void *, uint64_t, uint64_t);
    ((CopyBufferFn)list_vtable[15])(command_list, copy_destination, 80,
                                   copy_source, 32, 64);
    if (memcmp((uint8_t *)copy_destination_data + 80,
               (uint8_t *)copy_source_data + 32, 64) != 0)
        return 27;
    /* Command-list state/query methods are void contracts and must update the
     * CPU-backed resource/query model without returning HRESULT garbage. */
    typedef void (__attribute__((ms_abi)) *ResourceBarrierFn)(
        void *, uint32_t, const void *);
    typedef void (__attribute__((ms_abi)) *EndQueryFn)(
        void *, void *, uint32_t, uint32_t);
    typedef void (__attribute__((ms_abi)) *ResolveQueryFn)(
        void *, void *, uint32_t, uint32_t, uint32_t, void *, uint64_t);
    struct {
        uint32_t type;
        uint32_t flags;
        void *resource;
        uint32_t before;
        uint32_t after;
        uint32_t subresource;
        uint32_t padding;
    } barrier = {0, 0, copy_destination, 1, 4, UINT32_MAX, 0};
    ((ResourceBarrierFn)list_vtable[26])(command_list, 1, &barrier);
    typedef void (__attribute__((ms_abi)) *SetPipelineStateFn)(void *, void *);
    typedef void (__attribute__((ms_abi)) *SetDescriptorHeapsFn)(
        void *, uint32_t, void *const *);
    typedef void (__attribute__((ms_abi)) *SetRootTableFn)(
        void *, uint32_t, uint64_t);
    typedef void (__attribute__((ms_abi)) *SetRootConstantsFn)(
        void *, uint32_t, uint32_t, const void *, uint32_t);
    typedef void (__attribute__((ms_abi)) *SetRootDescriptorFn)(
        void *, uint32_t, uint64_t);
    typedef void (__attribute__((ms_abi)) *DispatchFn)(
        void *, uint32_t, uint32_t, uint32_t);
    typedef uint64_t (__attribute__((ms_abi)) *CreateComputeRootFn)(
        void *, uint32_t, const void *, size_t, const uint8_t *, void **);
    typedef void (__attribute__((ms_abi)) *SetComputeRootFn)(void *, void *);
    static const uint8_t compute_root_iid[16] = {
        0x66, 0x6b, 0x4a, 0xc5, 0xdf, 0x72, 0xe8, 0x4e,
        0x8b, 0xe5, 0xa9, 0x46, 0xa1, 0x42, 0x92, 0x14
    };
    static const uint8_t root_signature_blob[] = {
        'D','X','B','C',0x42,0x45,0x45,0x52
    };
    void *compute_root_signature = NULL;
    if (((CreateComputeRootFn)vtable[16])(
            output, 0, root_signature_blob, sizeof(root_signature_blob),
            compute_root_iid, &compute_root_signature) != 0 ||
        !compute_root_signature)
        return 27;
    ((SetPipelineStateFn)list_vtable[25])(command_list, compute_pipeline_state);
    ((SetComputeRootFn)list_vtable[29])(command_list, compute_root_signature);
    void *compute_heaps[2] = {heap, second_heap};
    ((SetDescriptorHeapsFn)list_vtable[28])(command_list, 2, compute_heaps);
    ((SetRootTableFn)list_vtable[31])(command_list, 3, gpu_handle + 32);
    uint32_t root_constants[3] = {0x11, 0x22, 0x33};
    ((SetRootConstantsFn)list_vtable[35])(
        command_list, 4, 3, root_constants, 2);
    ((SetRootDescriptorFn)list_vtable[37])(
        command_list, 5, UINT64_C(0x12345000));
    ((SetRootDescriptorFn)list_vtable[39])(
        command_list, 6, UINT64_C(0x22345000));
    ((SetRootDescriptorFn)list_vtable[41])(
        command_list, 7, UINT64_C(0x32345000));
    ((DispatchFn)list_vtable[14])(command_list, 8, 4, 1);
    typedef void (__attribute__((ms_abi)) *DrawInstancedFn)(
        void *, uint32_t, uint32_t, uint32_t, uint32_t);
    typedef void (__attribute__((ms_abi)) *DrawIndexedInstancedFn)(
        void *, uint32_t, uint32_t, uint32_t, int32_t, uint32_t);
    typedef void (__attribute__((ms_abi)) *SetRenderTargetsFn)(
        void *, uint32_t, const uint64_t *, uint32_t, const uint64_t *);
    uint64_t telemetry_rtv = UINT64_C(0x12340000);
    uint64_t telemetry_dsv = UINT64_C(0x56780000);
    ((SetRenderTargetsFn)list_vtable[47])(
        command_list, 1, &telemetry_rtv, 1, &telemetry_dsv);
    ((DrawInstancedFn)list_vtable[11])(command_list, 3, 2, 0, 0);
    ((DrawIndexedInstancedFn)list_vtable[12])(
        command_list, 6, 1, 0, 0, 0);
    /* Zero-sized draws are legal no-ops and must not inflate telemetry. */
    ((DrawInstancedFn)list_vtable[11])(command_list, 0, 1, 0, 0);
    typedef void (__attribute__((ms_abi)) *ExecuteListsFn)(
        void *, uint32_t, void *const *);
    if (((CloseFn)list_vtable[9])(command_list) != 0) return 27;
    void *compute_lists[1] = {command_list};
    ((ExecuteListsFn)queue_vtable[10])(queue, 1, compute_lists);
    if (g_submission_count != 1 ||
        g_last_submission.submitted_list_count != 1 ||
        g_last_submission.valid_list_count != 1 ||
        g_last_submission.open_list_count != 0 ||
        g_last_submission.dispatch_count != 1 ||
        g_last_submission.executed_dispatch_count != 1 ||
        g_last_submission.rejected_dispatch_count != 0 ||
        g_last_submission.render_target_binding_count != 1 ||
        g_last_submission.draw_instanced_count != 1 ||
        g_last_submission.draw_indexed_instanced_count != 1)
        return 27;
    if (g_compute_count != 1 || g_last_compute.group_count_x != 8 ||
        g_last_compute.group_count_y != 4 || g_last_compute.group_count_z != 1 ||
        g_last_compute.shader_size != sizeof(compute_shader) ||
        memcmp(g_last_compute.shader, compute_shader, sizeof(compute_shader)) != 0 ||
        g_last_compute.root_signature_blob_size != sizeof(root_signature_blob) ||
        memcmp(g_last_compute.root_signature_blob, root_signature_blob,
               sizeof(root_signature_blob)) != 0 ||
        g_last_compute.descriptor_heap_count != 2 ||
        g_last_compute.descriptor_heap_gpu_start[0] != gpu_handle ||
        !g_last_compute.descriptor_heap_storage[0] ||
        g_last_compute.descriptor_heap_size[0] != 4 * 32 ||
        g_last_compute.descriptor_heap_type[0] != 0 ||
        g_last_compute.descriptor_heap_increment[0] != 32 ||
        g_last_compute.bindings[3].kind != 1 ||
        g_last_compute.bindings[3].descriptor_table != gpu_handle + 32 ||
        g_last_compute.bindings[3].resource != NULL ||
        g_last_compute.bindings[3].resource_storage != NULL ||
        g_last_compute.bindings[4].kind != 2 ||
        g_last_compute.bindings[4].constant_count != 5 ||
        memcmp(g_last_compute.bindings[4].constants + 2, root_constants,
               sizeof(root_constants)) != 0 ||
        g_last_compute.bindings[5].kind != 3 ||
        g_last_compute.bindings[6].kind != 4 ||
        g_last_compute.bindings[7].kind != 5)
        return 27;
    if (((ResetListFn)list_vtable[10])(
            command_list, allocator, compute_pipeline_state) != 0)
        return 27;

    query_desc[1] = 2;
    query_heap = NULL;
    if (((CreateQueryHeapFn)vtable[39])(output, query_desc, iid_query_heap,
                                       &query_heap) != 0 || !query_heap)
        return 27;
    memset(copy_destination_data, 0, 16);
    ((EndQueryFn)list_vtable[53])(command_list, query_heap, 1, 0);
    ((ResolveQueryFn)list_vtable[54])(command_list, query_heap, 1, 0, 1,
                                     copy_destination, 0);
    uint64_t resolved_query = 0;
    memcpy(&resolved_query, copy_destination_data, sizeof(resolved_query));
    if (!resolved_query) return 27;
    void **resolved_query_vtable = *(void ***)query_heap;
    ((ReleaseFn)resolved_query_vtable[2])(query_heap);

    ((ReleaseFn)copy_source_vtable[2])(copy_source);
    ((ReleaseFn)copy_destination_vtable[2])(copy_destination);

    /* RE8 uses the Windows 7 downlevel command-queue interface rather than a
     * DXGI swap chain. Present must forward a CPU-backed RGBA8 resource to the
     * host presenter without changing the base queue's COM identity. */
    ResourceDesc present_desc = {0};
    present_desc.dimension = 3;
    present_desc.width = 4;
    present_desc.height = 2;
    present_desc.depth_or_array_size = 1;
    present_desc.mip_levels = 1;
    present_desc.format = 28;
    present_desc.sample_count = 1;
    void *present_resource = NULL;
    if (((CreateCommittedResourceFn)vtable[27])(
            output, heap_properties, 0, &present_desc, 4, NULL,
            iid_resource, &present_resource) != 0 || !present_resource)
        return 28;
    typedef uint64_t (__attribute__((ms_abi)) *DownlevelPresentFn)(
        void *, void *, void *, uint64_t, uint32_t);
    void **downlevel_vtable = *(void ***)downlevel_queue;
    if (((DownlevelPresentFn)downlevel_vtable[3])(
            downlevel_queue, command_list, present_resource, 1, 0) != 0 ||
        g_present_count != 1 || g_present_resource != present_resource ||
        g_present_width != 4 || g_present_height != 2 || g_present_pitch != 16)
        return 28;
    void **present_resource_vtable = *(void ***)present_resource;
    ((ReleaseFn)present_resource_vtable[2])(present_resource);

    /* Descriptor writers are void methods. They must initialize the guest
     * descriptor storage rather than return E_NOTIMPL through a mismatched ABI. */
    typedef void (__attribute__((ms_abi)) *CreateResourceViewFn)(
        void *, void *, const void *, uint64_t);
    typedef void (__attribute__((ms_abi)) *CreateSamplerFn)(
        void *, const void *, uint64_t);
    uint8_t descriptor_storage[32];
    uint8_t descriptor_data[32];
    memset(descriptor_data, 0xa5, sizeof(descriptor_data));
    memset(descriptor_storage, 0, sizeof(descriptor_storage));
    ((CreateResourceViewFn)vtable[20])(output, copy_source, descriptor_data,
                                      (uint64_t)(uintptr_t)descriptor_storage);
    if (memcmp(descriptor_storage, &copy_source, sizeof(copy_source)) != 0 ||
        memcmp(descriptor_storage + 8, descriptor_data, 24) != 0)
        return 28;

    /* ClearRenderTargetView must resolve the opaque RTV descriptor, honor
     * rectangle bounds, and preserve native RGBA/BGRA channel ordering. */
    ResourceDesc clear_desc = {0};
    clear_desc.dimension = 3;
    clear_desc.width = 4;
    clear_desc.height = 3;
    clear_desc.depth_or_array_size = 1;
    clear_desc.mip_levels = 1;
    clear_desc.format = 28;
    clear_desc.sample_count = 1;
    void *clear_resource = NULL;
    if (((CreateCommittedResourceFn)vtable[27])(
            output, heap_properties, 0, &clear_desc, 4, NULL,
            iid_resource, &clear_resource) != 0 || !clear_resource)
        return 28;
    uint8_t clear_descriptor[32] = {0};
    ((CreateResourceViewFn)vtable[20])(
        output, clear_resource, NULL,
        (uint64_t)(uintptr_t)clear_descriptor);
    typedef void (__attribute__((ms_abi)) *ClearRenderTargetFn)(
        void *, uint64_t, const float *, uint32_t, const void *);
    typedef struct TestRect { int32_t left, top, right, bottom; } TestRect;
    const float clear_color[4] = {0.25f, 0.5f, 1.0f, 1.0f};
    TestRect clear_rect = {1, 1, 3, 3};
    ((ClearRenderTargetFn)list_vtable[48])(
        command_list, (uint64_t)(uintptr_t)clear_descriptor,
        clear_color, 1, &clear_rect);
    if (g_clear_count != 0) return 28;
    /* Full-target clears take the registered Vulkan/backend path. */
    ((ClearRenderTargetFn)list_vtable[48])(
        command_list, (uint64_t)(uintptr_t)clear_descriptor,
        clear_color, 0, NULL);
    if (g_clear_count != 1) return 28;
    void **clear_resource_vtable = *(void ***)clear_resource;
    const uint8_t expected_clear[4] = {64, 128, 255, 255};
    /* Present the cleared texture before release to prove the command-list
     * operation reaches the application-owned downlevel back buffer. */
    if (((DownlevelPresentFn)downlevel_vtable[3])(
            downlevel_queue, command_list, clear_resource, 1, 0) != 0 ||
        g_present_count != 2 ||
        memcmp(g_present_pixels, expected_clear, 4) != 0 ||
        memcmp(g_present_pixels + (1 * 4 + 1) * 4, expected_clear, 4) != 0)
        return 28;
    ((ReleaseFn)clear_resource_vtable[2])(clear_resource);
    memset(descriptor_storage, 0, sizeof(descriptor_storage));
    ((CreateResourceViewFn)vtable[21])(output, copy_destination,
                                      descriptor_data,
                                      (uint64_t)(uintptr_t)descriptor_storage);
    if (memcmp(descriptor_storage, &copy_destination,
               sizeof(copy_destination)) != 0)
        return 29;
    memset(descriptor_storage, 0, sizeof(descriptor_storage));
    ((CreateSamplerFn)vtable[22])(output, descriptor_data,
                                 (uint64_t)(uintptr_t)descriptor_storage);
    if (memcmp(descriptor_storage, descriptor_data, 16) != 0)
        return 30;

    /* Descriptor copies must preserve opaque descriptor records, including
     * overlapping ranges and multiple range sizes. */
    typedef void (__attribute__((ms_abi)) *CopyDescriptorsSimpleFn)(
        void *, uint32_t, uint64_t, uint64_t, uint32_t);
    typedef void (__attribute__((ms_abi)) *CopyDescriptorsFn)(
        void *, uint32_t, const uint64_t *, const uint32_t *,
        uint32_t, const uint64_t *, const uint32_t *, uint32_t);
    uint8_t descriptor_source[96];
    uint8_t descriptor_destination[96];
    for (uint32_t index = 0; index < sizeof(descriptor_source); ++index)
        descriptor_source[index] = (uint8_t)(index + 1);
    memset(descriptor_destination, 0, sizeof(descriptor_destination));
    ((CopyDescriptorsSimpleFn)vtable[24])(
        output, 3, (uint64_t)(uintptr_t)descriptor_destination,
        (uint64_t)(uintptr_t)descriptor_source, 0);
    if (memcmp(descriptor_destination, descriptor_source,
               sizeof(descriptor_source)) != 0)
        return 35;
    memset(descriptor_destination, 0, sizeof(descriptor_destination));
    uint64_t destination_starts[2] = {
        (uint64_t)(uintptr_t)descriptor_destination,
        (uint64_t)(uintptr_t)(descriptor_destination + 64),
    };
    uint32_t destination_sizes[2] = {2, 1};
    uint64_t source_starts[2] = {
        (uint64_t)(uintptr_t)(descriptor_source + 32),
        (uint64_t)(uintptr_t)descriptor_source,
    };
    uint32_t source_sizes[2] = {1, 2};
    ((CopyDescriptorsFn)vtable[23])(
        output, 2, destination_starts, destination_sizes,
        2, source_starts, source_sizes, 0);
    if (memcmp(descriptor_destination, descriptor_source + 32, 32) != 0 ||
        memcmp(descriptor_destination + 32, descriptor_source, 32) != 0 ||
        memcmp(descriptor_destination + 64, descriptor_source + 32, 32) != 0)
        return 36;

    typedef void (__attribute__((ms_abi)) *GetFootprintsFn)(
        void *, const ResourceDesc *, uint32_t, uint32_t, uint64_t,
        void *, uint32_t *, uint64_t *, uint64_t *);
    ResourceDesc texture_desc = {0};
    texture_desc.dimension = 3;
    texture_desc.width = 17;
    texture_desc.height = 9;
    texture_desc.depth_or_array_size = 1;
    texture_desc.mip_levels = 1;
    texture_desc.format = 28;
    texture_desc.sample_count = 1;
    uint8_t footprint[32] = {0};
    uint32_t row_count = 0;
    uint64_t row_size = 0;
    uint64_t total_size = 0;
    ((GetFootprintsFn)vtable[38])(output, &texture_desc, 0, 1, 0,
                                  footprint, &row_count, &row_size, &total_size);
    uint32_t footprint_width = 0;
    uint32_t row_pitch = 0;
    memcpy(&footprint_width, footprint + 12, sizeof(footprint_width));
    memcpy(&row_pitch, footprint + 24, sizeof(row_pitch));
    if (footprint_width != 17 || row_count != 9 || row_size != 68 ||
        row_pitch != 256 || total_size != 2304)
        return 25;

    /* Command signatures must be independently owned pageable objects with
     * a retained device relationship and copied argument descriptors. */
    static const uint8_t iid_command_signature[16] = {
        0x7c, 0x79, 0x6a, 0xc3, 0x80, 0xec, 0x0a, 0x4f,
        0x89, 0x85, 0xa7, 0xb2, 0x47, 0x50, 0x82, 0xd1
    };
    typedef struct CommandSignatureDesc {
        uint32_t byte_stride;
        uint32_t argument_count;
        const void *arguments;
        uint32_t node_mask;
        uint32_t padding;
    } CommandSignatureDesc;
    typedef uint64_t (__attribute__((ms_abi)) *CreateCommandSignatureFn)(
        void *, const CommandSignatureDesc *, void *, const uint8_t *, void **);
    uint32_t indirect_arguments[4] = {2, 0, 3, 0};
    CommandSignatureDesc signature_desc = {
        .byte_stride = 20,
        .argument_count = 2,
        .arguments = indirect_arguments,
        .node_mask = 0,
    };
    void *command_signature = NULL;
    if (((CreateCommandSignatureFn)vtable[41])(
            output, &signature_desc, NULL, iid_command_signature,
            &command_signature) != 0 || command_signature == NULL)
        return 31;
    void **signature_vtable = *(void ***)command_signature;
    void *signature_device = NULL;
    if (((GetDeviceFn)signature_vtable[7])(
            command_signature, iid_device, &signature_device) != 0 ||
        signature_device != output)
        return 32;
    ((ReleaseFn)vtable[2])(signature_device);
    if (((ReleaseFn)signature_vtable[2])(command_signature) != 0)
        return 33;
    /* Root-signature serialization must return a real ID3DBlob and include
     * pointed-to descriptor state rather than only top-level pointer values.
     * D3D12_VERSIONED_ROOT_SIGNATURE_DESC is 48 bytes on Win64. */
    uint8_t versioned_root_desc[48] = {0};
    uint32_t root_version = 1;
    uint32_t root_flags = 1;
    memcpy(versioned_root_desc, &root_version, sizeof(root_version));
    memcpy(versioned_root_desc + 40, &root_flags, sizeof(root_flags));
    void *serialized_root = NULL;
    void *serialization_error = (void *)(uintptr_t)0x1;
    if (beer_d3d12_serialize_versioned_root_signature(
            versioned_root_desc, &serialized_root, &serialization_error) != 0 ||
        !serialized_root || serialization_error != NULL)
        return 34;
    void **blob_vtable = *(void ***)serialized_root;
    typedef void *(__attribute__((ms_abi)) *GetBufferPointerFn)(void *);
    typedef size_t (__attribute__((ms_abi)) *GetBufferSizeFn)(void *);
    const uint8_t *root_bytes = ((GetBufferPointerFn)blob_vtable[3])(serialized_root);
    size_t root_size = ((GetBufferSizeFn)blob_vtable[4])(serialized_root);
    static const uint8_t root_magic[8] = {'B','E','E','R','R','S','I','G'};
    if (!root_bytes || root_size < 24 ||
        memcmp(root_bytes, root_magic, sizeof(root_magic)) != 0)
        return 34;

    uint8_t second_root_desc[48];
    memcpy(second_root_desc, versioned_root_desc, sizeof(second_root_desc));
    root_flags = 2;
    memcpy(second_root_desc + 40, &root_flags, sizeof(root_flags));
    void *second_serialized_root = NULL;
    if (beer_d3d12_serialize_versioned_root_signature(
            second_root_desc, &second_serialized_root, NULL) != 0 ||
        !second_serialized_root)
        return 34;
    void **second_blob_vtable = *(void ***)second_serialized_root;
    const uint8_t *second_root_bytes =
        ((GetBufferPointerFn)second_blob_vtable[3])(second_serialized_root);
    size_t second_root_size =
        ((GetBufferSizeFn)second_blob_vtable[4])(second_serialized_root);
    if (second_root_size != root_size ||
        memcmp(second_root_bytes, root_bytes, root_size) == 0)
        return 34;
    ((ReleaseFn)second_blob_vtable[2])(second_serialized_root);
    static const uint8_t iid_root_signature[16] = {
        0x66, 0x6b, 0x4a, 0xc5, 0xdf, 0x72, 0xe8, 0x4e,
        0x8b, 0xe5, 0xa9, 0x46, 0xa1, 0x42, 0x92, 0x14
    };
    typedef uint64_t (__attribute__((ms_abi)) *CreateRootSignatureFn)(
        void *, uint32_t, const void *, size_t, const uint8_t *, void **);
    void *root_signature = NULL;
    if (((CreateRootSignatureFn)vtable[16])(
            output, 0, root_bytes, root_size, iid_root_signature,
            &root_signature) != 0 || !root_signature)
        return 34;
    void **root_signature_vtable = *(void ***)root_signature;
    void *root_device = NULL;
    if (((GetDeviceFn)root_signature_vtable[7])(
            root_signature, iid_device, &root_device) != 0 || root_device != output)
        return 34;
    ((ReleaseFn)vtable[2])(root_device);

    typedef uint64_t (__attribute__((ms_abi)) *SetPrivateDataFn)(
        void *, const uint8_t *, uint32_t, const void *);
    typedef uint64_t (__attribute__((ms_abi)) *SetNameFn)(void *, const uint16_t *);
    static const uint8_t root_private_key[16] = {
        0x90, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde,
        0xf0, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde
    };
    static const uint8_t root_private_value[5] = {1, 3, 5, 7, 9};
    if (((SetPrivateDataFn)root_signature_vtable[4])(
            root_signature, root_private_key, sizeof(root_private_value),
            root_private_value) != 0)
        return 34;
    uint32_t root_private_size = 0;
    if (((GetPrivateDataFn)root_signature_vtable[3])(
            root_signature, root_private_key, &root_private_size, NULL) !=
            UINT64_C(0x887a0003) ||
        root_private_size != sizeof(root_private_value))
        return 34;
    uint8_t root_private_output[5] = {0};
    if (((GetPrivateDataFn)root_signature_vtable[3])(
            root_signature, root_private_key, &root_private_size,
            root_private_output) != 0 ||
        memcmp(root_private_output, root_private_value,
               sizeof(root_private_value)) != 0)
        return 34;
    uint32_t short_size = 2;
    if (((GetPrivateDataFn)root_signature_vtable[3])(
            root_signature, root_private_key, &short_size,
            root_private_output) != UINT64_C(0x887a0003) ||
        short_size != sizeof(root_private_value))
        return 34;
    static const uint16_t root_name[] = {'R','E','8',' ','R','o','o','t',0};
    if (((SetNameFn)root_signature_vtable[6])(root_signature, root_name) != 0)
        return 34;
    if (((SetPrivateDataFn)root_signature_vtable[4])(
            root_signature, root_private_key, 0, NULL) != 0)
        return 34;
    root_private_size = sizeof(root_private_output);
    if (((GetPrivateDataFn)root_signature_vtable[3])(
            root_signature, root_private_key, &root_private_size,
            root_private_output) != UINT64_C(0x887a0002) ||
        root_private_size != 0)
        return 34;

    ((ReleaseFn)root_signature_vtable[2])(root_signature);
    ((ReleaseFn)blob_vtable[2])(serialized_root);

    void *device_object = output;
    output = (void *)(uintptr_t)0x1;
    if (((CreateCommandSignatureFn)vtable[41])(
            device_object, &signature_desc, NULL, iid_device, &output) !=
            UINT64_C(0x80004002) || output != NULL)
        return 34;

    ((ReleaseFn)list_vtable[2])(command_list);
    void **compute_root_vtable = *(void ***)compute_root_signature;
    ((ReleaseFn)compute_root_vtable[2])(compute_root_signature);
    ((ReleaseFn)pipeline_vtable[2])(compute_pipeline_state);
    ((ReleaseFn)heap_vtable[2])(heap);
    void **second_heap_vtable = *(void ***)second_heap;
    ((ReleaseFn)second_heap_vtable[2])(second_heap);
    typedef uint32_t (__attribute__((ms_abi)) *ReleaseQueueFn)(void *);
    ((ReleaseQueueFn)downlevel_vtable[2])(downlevel_queue);
    ((ReleaseQueueFn)queue_vtable[2])(queue);

    output = (void *)(uintptr_t)0x1;
    result = beer_d3d12_get_debug_interface(NULL, &output);
    if (result != UINT64_C(0x80004002) || output != NULL) return 11;

    void *blob = (void *)(uintptr_t)0x1;
    void *error_blob = (void *)(uintptr_t)0x1;
    result = beer_d3d12_serialize_versioned_root_signature(NULL, &blob,
                                                           &error_blob);
    if (result != UINT64_C(0x80070057) || blob != NULL || error_blob != NULL)
        return 12;

    puts("D3D12 compatibility tests passed");
    return 0;
}
