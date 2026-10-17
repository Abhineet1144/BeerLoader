#include "d3d12_compat.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t g_signaled_event;
static uint32_t g_signal_count;
static uint32_t g_present_count;
static const void *g_present_resource;
static uint64_t g_present_serial;
static int g_present_width;
static int g_present_height;
static int g_present_pitch;
static uint8_t g_present_pixels[64];
static uint32_t g_clear_count;
static uint32_t g_forget_count;
static const void *g_forgotten_resource;
static const void *g_forget_history[64];
static uint32_t g_source_sync_count;
static const void *g_source_sync_history[64];
static uint32_t g_compute_count;
static BeerD3D12ComputeDispatch g_last_compute;
static uint32_t g_graphics_count;
static uint32_t g_graphics_batch_count;
static BeerD3D12GraphicsDraw g_last_graphics;
static uint8_t g_graphics_defer_history[64];
static uint32_t g_ordered_buffer_copy_draw_seen;
static const void *g_ordered_buffer_copy_resource;
static const uint8_t g_ordered_vertex_bytes[16] = {
    0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8,
    0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf, 0xb0
};
static const uint8_t g_ordered_index_bytes[6] = {0, 0, 1, 0, 2, 0};
static uint32_t g_compute_batch_begin_count;
static uint32_t g_compute_batch_end_count;
static uint8_t g_depth_snapshot[64];
static size_t g_depth_snapshot_size;
static uint8_t g_depth_history[4][64];
static size_t g_depth_history_sizes[4];
static uint32_t g_depth_history_count;
static char g_execution_order[32];
static uint32_t g_execution_order_count;
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
    if (g_execution_order_count < sizeof(g_execution_order))
        g_execution_order[g_execution_order_count++] = 'C';
    ++g_compute_count;
    return 1;
}

static uint32_t g_compute_ir_batch_count;
static uint32_t g_compute_ir_operation_count;

static uint32_t g_compute_ir_access_count;
static uint32_t g_compute_ir_hazards;

static void test_execute_compute_batch(
    const BeerD3D12ComputeCommandBatch *batch, uint8_t *results)
{
    ++g_compute_ir_batch_count;
    g_compute_ir_operation_count += batch->command_count;
    g_compute_ir_access_count += batch->access_count;
    for (uint32_t index = 0; index < batch->command_count; ++index) {
        g_compute_ir_hazards |= batch->commands[index].hazards_before;
        results[index] = (uint8_t)test_execute_compute(
            batch->commands[index].dispatch);
    }
}

static void test_begin_compute_batch(void)
{
    ++g_compute_batch_begin_count;
}

static void test_end_compute_batch(void)
{
    ++g_compute_batch_end_count;
}

static int test_execute_graphics(const BeerD3D12GraphicsDraw *draw)
{
    if (!draw || !draw->pipeline_state || !draw->element_count ||
        !draw->instance_count)
        return 0;
    g_last_graphics = *draw;
    if (draw->indexed && g_ordered_buffer_copy_resource &&
        draw->vertex_buffer_resources[0] == g_ordered_buffer_copy_resource &&
        draw->index_buffer_resource == g_ordered_buffer_copy_resource) {
        if (!draw->vertex_buffer_storage[0] ||
            draw->vertex_buffer_storage_size[0] !=
                sizeof(g_ordered_vertex_bytes) ||
            !draw->index_buffer_storage ||
            draw->index_buffer_storage_size != sizeof(g_ordered_index_bytes) ||
            memcmp(draw->vertex_buffer_storage[0], g_ordered_vertex_bytes,
                   sizeof(g_ordered_vertex_bytes)) != 0 ||
            memcmp(draw->index_buffer_storage, g_ordered_index_bytes,
                   sizeof(g_ordered_index_bytes)) != 0)
            return 0;
        ++g_ordered_buffer_copy_draw_seen;
    }
    if (draw->depth_stencil_storage && draw->depth_stencil_storage_size) {
        g_depth_snapshot_size = draw->depth_stencil_storage_size;
        if (g_depth_snapshot_size > sizeof(g_depth_snapshot))
            g_depth_snapshot_size = sizeof(g_depth_snapshot);
        memcpy(g_depth_snapshot, draw->depth_stencil_storage,
               g_depth_snapshot_size);
        if (g_depth_history_count < 4) {
            g_depth_history_sizes[g_depth_history_count] = g_depth_snapshot_size;
            memcpy(g_depth_history[g_depth_history_count],
                   draw->depth_stencil_storage, g_depth_snapshot_size);
            ++g_depth_history_count;
        }
    }
    if (g_execution_order_count < sizeof(g_execution_order))
        g_execution_order[g_execution_order_count++] = 'G';
    if (g_graphics_count < sizeof(g_graphics_defer_history))
        g_graphics_defer_history[g_graphics_count] =
            (uint8_t)draw->defer_render_target_publication;
    ++g_graphics_count;
    return 1;
}

static uint32_t test_execute_graphics_batch(
    const BeerD3D12GraphicsDraw *draw,
    const BeerD3D12GraphicsArguments *arguments, uint32_t argument_count)
{
    ++g_graphics_batch_count;
    if (!draw || !arguments) return 0;
    BeerD3D12GraphicsDraw operation = *draw;
    uint32_t executed = 0;
    for (uint32_t index = 0; index < argument_count; ++index) {
        operation.element_count = arguments[index].element_count;
        operation.instance_count = arguments[index].instance_count;
        operation.start_element = arguments[index].start_element;
        operation.base_vertex = arguments[index].base_vertex;
        operation.start_instance = arguments[index].start_instance;
        operation.defer_render_target_publication =
            arguments[index].defer_render_target_publication;
        executed += test_execute_graphics(&operation) != 0;
    }
    return executed;
}

static int test_graphics_attachment_run_policy(void)
{
    uint8_t target_a[4] = {0};
    uint8_t target_b[4] = {0};
    uint64_t serial_a = 0;
    uint64_t serial_b = 0;
    BeerD3D12GraphicsDraw first;
    BeerD3D12GraphicsDraw next;
    memset(&first, 0, sizeof(first));
    memset(&next, 0, sizeof(next));
    first.render_target_count = next.render_target_count = 1;
    first.render_targets[0] = next.render_targets[0] = target_a;
    first.render_target_storage[0] = next.render_target_storage[0] = target_a;
    first.render_target_serial_location[0] =
        next.render_target_serial_location[0] = &serial_a;
    first.render_target_format[0] = next.render_target_format[0] = 28;
    first.render_target_width[0] = next.render_target_width[0] = 1;
    first.render_target_height[0] = next.render_target_height[0] = 1;
    if (!beer_d3d12_graphics_attachments_match(&first, &next)) return 0;

    next.render_targets[0] = target_b;
    next.render_target_storage[0] = target_b;
    next.render_target_serial_location[0] = &serial_b;
    if (beer_d3d12_graphics_attachments_match(&first, &next)) return 0;
    next = first;
    next.render_target_format[0] = 87;
    if (beer_d3d12_graphics_attachments_match(&first, &next)) return 0;
    next = first;
    next.render_target_width[0] = 2;
    if (beer_d3d12_graphics_attachments_match(&first, &next)) return 0;
    next = first;
    next.depth_stencil = target_b;
    if (beer_d3d12_graphics_attachments_match(&first, &next)) return 0;
    next = first;
    next.render_target_count = 0;
    if (beer_d3d12_graphics_attachments_match(&first, &next)) return 0;
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

static void test_forget_resource(const void *resource)
{
    g_forgotten_resource = resource;
    if (g_forget_count < sizeof(g_forget_history) /
            sizeof(g_forget_history[0]))
        g_forget_history[g_forget_count] = resource;
    ++g_forget_count;
}

static void test_sync_source_resource(const void *resource)
{
    if (g_source_sync_count < sizeof(g_source_sync_history) /
            sizeof(g_source_sync_history[0]))
        g_source_sync_history[g_source_sync_count] = resource;
    ++g_source_sync_count;
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
    if (!resource || !pixels) return 0;
    g_present_resource = resource;
    g_present_serial = serial;
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
    if (!test_graphics_attachment_run_policy()) {
        fprintf(stderr, "graphics attachment-run policy test failed\n");
        return 50;
    }
    typedef struct TestResourceDesc {
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
    } TestResourceDesc;
    typedef struct TestResourceAllocationInfo {
        uint64_t size_in_bytes;
        uint64_t alignment;
    } TestResourceAllocationInfo;
    typedef TestResourceAllocationInfo *(__attribute__((ms_abi))
        *GetResourceAllocationInfoFn)(void *, TestResourceAllocationInfo *,
                                      uint32_t, uint32_t,
                                      const TestResourceDesc *);
    uint8_t hazard_storage[16] = {0};
    int hazard_resource;
    BeerD3D12ComputeResourceAccess hazard_accesses[4] = {
        { .resource = &hazard_resource, .storage = hazard_storage,
          .storage_size = sizeof(hazard_storage),
          .access = BEER_D3D12_COMPUTE_ACCESS_WRITE },
        { .resource = &hazard_resource, .storage = hazard_storage,
          .storage_size = sizeof(hazard_storage),
          .access = BEER_D3D12_COMPUTE_ACCESS_READ },
        { .resource = &hazard_resource, .storage = hazard_storage,
          .storage_size = sizeof(hazard_storage),
          .access = BEER_D3D12_COMPUTE_ACCESS_WRITE },
        { .storage = hazard_storage, .storage_size = sizeof(hazard_storage),
          .access = BEER_D3D12_COMPUTE_ACCESS_READ }
    };
    if (beer_d3d12_compute_hazards_before(hazard_accesses, 1, 1) !=
            BEER_D3D12_COMPUTE_HAZARD_RAW ||
        beer_d3d12_compute_hazards_before(hazard_accesses, 2, 1) !=
            (BEER_D3D12_COMPUTE_HAZARD_WAR |
             BEER_D3D12_COMPUTE_HAZARD_WAW) ||
        beer_d3d12_compute_hazards_before(hazard_accesses, 3, 1) != 0)
        return 100;
    uint8_t staging_other[7] = {0};
    BeerD3D12ComputeResourceAccess staging_accesses[4] = {
        { .resource = &hazard_resource, .storage = hazard_storage,
          .storage_size = sizeof(hazard_storage),
          .access = BEER_D3D12_COMPUTE_ACCESS_READ },
        { .resource = &hazard_resource, .storage = hazard_storage,
          .storage_size = sizeof(hazard_storage),
          .access = BEER_D3D12_COMPUTE_ACCESS_WRITE },
        { .storage = staging_other, .storage_size = sizeof(staging_other),
          .access = BEER_D3D12_COMPUTE_ACCESS_READ },
        { .access = BEER_D3D12_COMPUTE_ACCESS_READ }
    };
    size_t staging_offsets[4] = {0};
    size_t staging_total = 0;
    if (!beer_d3d12_compute_plan_staging(staging_accesses, 4, 256,
                                         staging_offsets, &staging_total) ||
        staging_offsets[0] != 0 || staging_offsets[1] != 0 ||
        staging_offsets[2] != 256 || staging_offsets[3] != SIZE_MAX ||
        staging_total != 512 ||
        beer_d3d12_compute_plan_staging(staging_accesses, 4, 192,
                                        staging_offsets, &staging_total))
        return 101;
    BeerD3D12ComputeCommand epoch_commands[5] = {
        { .hazards_before = 0 },
        { .hazards_before = 0 },
        { .hazards_before = BEER_D3D12_COMPUTE_HAZARD_RAW },
        { .hazards_before = 0 },
        { .hazards_before = BEER_D3D12_COMPUTE_HAZARD_WAW }
    };
    uint32_t epochs[5] = {0};
    uint32_t epoch_count = 0;
    if (!beer_d3d12_compute_plan_epochs(epoch_commands, 5, epochs,
                                        &epoch_count) ||
        epoch_count != 3 || epochs[0] != 0 || epochs[1] != 0 ||
        epochs[2] != 1 || epochs[3] != 1 || epochs[4] != 2 ||
        !beer_d3d12_compute_plan_epochs(NULL, 0, NULL, &epoch_count) ||
        epoch_count != 0 ||
        beer_d3d12_compute_plan_epochs(epoch_commands, 5, NULL,
                                       &epoch_count))
        return 102;
    BeerD3D12ComputeCommand planned_commands[3] = {
        { .first_access = 0, .access_count = 2 },
        { .first_access = 2, .access_count = 1 },
        { .first_access = 3, .access_count = 1,
          .hazards_before = BEER_D3D12_COMPUTE_HAZARD_RAW }
    };
    uint32_t planned_epochs[3] = {0, 0, 1};
    BeerD3D12ComputeCommandBatch planned_batch = {
        .commands = planned_commands, .command_count = 3,
        .accesses = staging_accesses, .access_count = 4
    };
    size_t planned_offsets[4] = {0};
    size_t planned_sizes[2] = {0};
    if (!beer_d3d12_compute_plan_epoch_staging(
            &planned_batch, planned_epochs, 2, 256, 64,
            planned_offsets, planned_sizes) ||
        planned_offsets[0] != 0 || planned_offsets[1] != 0 ||
        planned_offsets[2] != 256 || planned_offsets[3] != SIZE_MAX ||
        planned_sizes[0] != 640 || planned_sizes[1] != 64 ||
        beer_d3d12_compute_plan_epoch_staging(
            &planned_batch, planned_epochs, 1, 256, 64,
            planned_offsets, planned_sizes))
        return 103;
    beer_d3d12_set_event_signaler(test_signal_event);
    beer_d3d12_set_presenter(test_present);
    beer_d3d12_set_clear_target(test_clear_target);
    beer_d3d12_set_resource_forgetter(test_forget_resource);
    beer_d3d12_set_resource_source_forgetter(test_sync_source_resource);
    beer_d3d12_set_compute_executor(test_execute_compute);
    beer_d3d12_set_compute_batch_executor(test_execute_compute_batch);
    beer_d3d12_set_compute_batch_callbacks(test_begin_compute_batch,
                                           test_end_compute_batch);
    beer_d3d12_set_graphics_executor(test_execute_graphics);
    beer_d3d12_set_graphics_batch_executor(test_execute_graphics_batch);
    beer_d3d12_set_submission_observer(test_observe_submission);
    static const uint8_t iid_device[16] = {
        0xf1, 0x19, 0x98, 0x18, 0xb6, 0x1d, 0x57, 0x4b,
        0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7
    };
    void *output = (void *)(uintptr_t)0x1;
    uint64_t result = beer_d3d12_create_device(0, 0xc000, iid_device, &output);
    if (result != 0 || output == NULL) return 1;

    void **vtable = *(void ***)output;
    TestResourceDesc bc1_allocation_desc = {0};
    bc1_allocation_desc.dimension = 3;
    bc1_allocation_desc.width = 2048;
    bc1_allocation_desc.height = 2048;
    bc1_allocation_desc.depth_or_array_size = 32;
    bc1_allocation_desc.mip_levels = 1;
    bc1_allocation_desc.format = 70; /* DXGI_FORMAT_BC1_TYPELESS */
    bc1_allocation_desc.sample_count = 1;
    TestResourceAllocationInfo bc1_allocation = {0};
    if (((GetResourceAllocationInfoFn)vtable[25])(
            output, &bc1_allocation, 0, 1, &bc1_allocation_desc) !=
            &bc1_allocation ||
        bc1_allocation.size_in_bytes != UINT64_C(67108864) ||
        bc1_allocation.alignment != UINT64_C(65536))
        return 104;
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
    if (g_forget_count != 1 || g_forgotten_resource != resource) return 18;

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
    /* CopyBufferRegion is a GPU command, not an immediate CPU memcpy. Freeze
     * upload bytes at record time, defer destination visibility until submit,
     * and survive upload-ring reuse before execution. */
    uint8_t expected_buffer_copy[64];
    memcpy(expected_buffer_copy, (uint8_t *)copy_source_data + 32,
           sizeof(expected_buffer_copy));
    if (memcmp((uint8_t *)copy_destination_data + 80,
               expected_buffer_copy, sizeof(expected_buffer_copy)) == 0)
        return 27;
    /* CopyResource is an ordered GPU command. It must not expose its result
     * while recording, and it must execute between surrounding dispatch/draw
     * operations rather than copying stale pre-render host bytes. */
    typedef void (__attribute__((ms_abi)) *CopyResourceFn)(
        void *, void *, void *);
    memset(copy_source_data, 0xa7, 16);
    memset(copy_destination_data, 0, 16);
    ((CopyResourceFn)list_vtable[17])(
        command_list, copy_destination, copy_source);
    ((CopyBufferFn)list_vtable[15])(command_list, copy_destination, 80,
                                   copy_source, 32, 64);
    memset((uint8_t *)copy_source_data + 32, 0xee,
           sizeof(expected_buffer_copy));
    if (memcmp(copy_destination_data, copy_source_data, 16) == 0)
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
    typedef void (__attribute__((ms_abi)) *SetGraphicsRootFn)(void *, void *);
    typedef void (__attribute__((ms_abi)) *SetRootConstantFn)(
        void *, uint32_t, uint32_t, uint32_t);
    typedef void (__attribute__((ms_abi)) *SetTopologyFn)(void *, uint32_t);
    typedef void (__attribute__((ms_abi)) *SetViewportsFn)(
        void *, uint32_t, const void *);
    typedef void (__attribute__((ms_abi)) *SetScissorsFn)(
        void *, uint32_t, const void *);
    typedef void (__attribute__((ms_abi)) *SetVertexBuffersFn)(
        void *, uint32_t, uint32_t, const BeerD3D12BufferView *);
    typedef void (__attribute__((ms_abi)) *SetIndexBufferFn)(
        void *, const BeerD3D12BufferView *);
    ((SetGraphicsRootFn)list_vtable[30])(
        command_list, compute_root_signature);
    ((SetRootTableFn)list_vtable[32])(command_list, 0, gpu_handle);
    ((SetRootConstantFn)list_vtable[34])(command_list, 1, 0x44, 0);
    ((SetRootConstantsFn)list_vtable[36])(
        command_list, 2, 3, root_constants, 1);
    uint64_t graphics_cbv_address =
        ((GetGpuAddressFn)copy_source_vtable[11])(copy_source);
    ((uint8_t *)copy_source_data)[0] = 0x6a;
    ((SetRootDescriptorFn)list_vtable[38])(
        command_list, 3, graphics_cbv_address);
    ((SetRootDescriptorFn)list_vtable[40])(
        command_list, 4, UINT64_C(0x52345000));
    ((SetRootDescriptorFn)list_vtable[42])(
        command_list, 5, UINT64_C(0x62345000));
    ((SetTopologyFn)list_vtable[20])(command_list, 4);
    const float test_viewport[6] = {1.0f, 2.0f, 640.0f, 480.0f, 0.0f, 1.0f};
    const int32_t test_scissor[4] = {3, 4, 603, 404};
    ((SetViewportsFn)list_vtable[21])(command_list, 1, test_viewport);
    ((SetScissorsFn)list_vtable[22])(command_list, 1, test_scissor);
    uint64_t test_vertex_address =
        ((GetGpuAddressFn)copy_source_vtable[11])(copy_source);
    uint64_t test_index_address =
        ((GetGpuAddressFn)copy_destination_vtable[11])(copy_destination);
    BeerD3D12BufferView test_vertex = {test_vertex_address + 16, 128, 16};
    BeerD3D12BufferView test_index = {test_index_address + 32, 64, 57};
    memset((uint8_t *)copy_source_data + 16, 0x91, test_vertex.size);
    memset((uint8_t *)copy_destination_data + 32, 0x53, test_index.size);
    ((SetVertexBuffersFn)list_vtable[44])(
        command_list, 0, 1, &test_vertex);
    ((SetIndexBufferFn)list_vtable[43])(command_list, &test_index);
    typedef void (__attribute__((ms_abi)) *DrawInstancedFn)(
        void *, uint32_t, uint32_t, uint32_t, uint32_t);
    typedef void (__attribute__((ms_abi)) *DrawIndexedInstancedFn)(
        void *, uint32_t, uint32_t, uint32_t, int32_t, uint32_t);
    typedef void (__attribute__((ms_abi)) *SetRenderTargetsFn)(
        void *, uint32_t, const uint64_t *, uint32_t, const uint64_t *);
    typedef void (__attribute__((ms_abi)) *CreateRenderViewFn)(
        void *, void *, const void *, uint64_t);
    uint8_t telemetry_rtv_descriptor[32] = {0};
    uint64_t telemetry_rtv =
        (uint64_t)(uintptr_t)telemetry_rtv_descriptor;
    ((CreateRenderViewFn)vtable[20])(
        output, copy_destination, NULL, telemetry_rtv);
    ((SetRenderTargetsFn)list_vtable[46])(
        command_list, 1, &telemetry_rtv, 1, NULL);
    /* Slot 47 is ClearDepthStencilView. It is a seven-argument void method;
     * routing it through a six-argument HRESULT fallback corrupts Win64 calls. */
    typedef void (__attribute__((ms_abi)) *ClearDepthStencilFn)(
        void *, uint64_t, uint32_t, float, uint8_t, uint32_t, const void *);
    uint8_t depth_descriptor[32] = {0};
    typedef void (__attribute__((ms_abi)) *CreateDepthViewFn)(
        void *, void *, const void *, uint64_t);
    ((CreateDepthViewFn)vtable[21])(
        output, copy_destination, NULL, (uint64_t)(uintptr_t)depth_descriptor);
    ((ClearDepthStencilFn)list_vtable[47])(
        command_list, (uint64_t)(uintptr_t)depth_descriptor, 3, 1.0f, 0, 0,
        NULL);
    ((DrawInstancedFn)list_vtable[12])(command_list, 3, 2, 0, 0);
    ((DrawIndexedInstancedFn)list_vtable[13])(
        command_list, 6, 1, 0, 0, 0);
    /* Dynamic rings may be reused before queue submission. Recorded draws must
     * own the exact vertex/index bytes visible when each draw was recorded. */
    memset((uint8_t *)copy_source_data + 16, 0x22, test_vertex.size);
    memset((uint8_t *)copy_destination_data + 32, 0x44, test_index.size);
    /* CBVs are consumed when the command list executes, not when Draw is
     * recorded. Reusing a mapped constant ring before submission must expose
     * the latest bytes to graphics just as it does to compute. */
    ((uint8_t *)copy_source_data)[0] = 0x7b;
    /* Zero-sized draws are legal no-ops and must not inflate telemetry. */
    ((DrawInstancedFn)list_vtable[12])(command_list, 0, 1, 0, 0);
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
        g_last_submission.clear_depth_stencil_count != 1 ||
        g_last_submission.copy_buffer_count != 1 ||
        g_last_submission.copy_resource_count != 1 ||
        memcmp((uint8_t *)copy_destination_data + 80,
               expected_buffer_copy, sizeof(expected_buffer_copy)) != 0 ||
        memcmp(copy_destination_data, copy_source_data, 16) != 0 ||
        g_last_submission.render_target_binding_count != 1 ||
        g_last_submission.draw_instanced_count != 1 ||
        g_last_submission.draw_indexed_instanced_count != 1 ||
        g_last_submission.executed_draw_count != 2 ||
        g_last_submission.rejected_draw_count != 0 ||
        g_graphics_count != 2 || !g_last_graphics.indexed ||
        g_last_graphics.element_count != 6 ||
        g_last_graphics.instance_count != 1 ||
        g_last_graphics.root_signature != compute_root_signature ||
        g_last_graphics.descriptor_heap_count != 2 ||
        g_last_graphics.bindings[0].kind != 1 ||
        g_last_graphics.bindings[0].descriptor_table != gpu_handle ||
        g_last_graphics.render_target_count != 1 ||
        g_last_graphics.render_targets[0] != copy_destination ||
        !g_last_graphics.render_target_serial_location[0] ||
        g_last_graphics.render_target_storage[0] != copy_destination_data ||
        g_last_graphics.render_target_storage_size[0] != 65536 ||
        g_last_graphics.render_target_width[0] != 256 ||
        g_last_graphics.render_target_height[0] != 1 ||
        g_last_graphics.bindings[1].kind != 2 ||
        g_last_graphics.bindings[1].constants[0] != 0x44 ||
        g_last_graphics.bindings[2].kind != 2 ||
        g_last_graphics.bindings[2].constant_count != 4 ||
        g_last_graphics.bindings[2].constants[1] != 0x11 ||
        g_last_graphics.bindings[3].kind != 3 ||
        g_last_graphics.bindings[3].resource_storage == NULL ||
        g_last_graphics.bindings[3].resource_storage[0] != 0x7b ||
        g_last_graphics.bindings[3].resource_storage ==
            (uint8_t *)copy_source_data ||
        g_last_graphics.bindings[4].kind != 4 ||
        g_last_graphics.bindings[5].kind != 5 ||
        g_last_graphics.primitive_topology != 4 ||
        g_last_graphics.vertex_buffer_mask != 1 ||
        g_last_graphics.vertex_buffers[0].address != test_vertex.address ||
        !g_last_graphics.vertex_buffer_storage[0] ||
        g_last_graphics.vertex_buffer_storage[0] ==
            (uint8_t *)copy_source_data + 16 ||
        g_last_graphics.vertex_buffer_storage_size[0] != test_vertex.size ||
        g_last_graphics.vertex_buffer_storage[0][0] != 0x91 ||
        g_last_graphics.vertex_buffer_resources[0] != copy_source ||
        g_last_graphics.index_buffer.address != test_index.address ||
        !g_last_graphics.index_buffer_storage ||
        g_last_graphics.index_buffer_storage ==
            (uint8_t *)copy_destination_data + 32 ||
        g_last_graphics.index_buffer_storage[0] != 0x53 ||
        g_last_graphics.index_buffer_storage_size != test_index.size ||
        g_last_graphics.index_buffer_resource != copy_destination ||
        memcmp(g_last_graphics.viewport, test_viewport,
               sizeof(test_viewport)) != 0 ||
        memcmp(g_last_graphics.scissor, test_scissor,
               sizeof(test_scissor)) != 0)
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
    if (beer_d3d12_snapshot_bytes() == 0)
        return 27;
    if (((ResetListFn)list_vtable[10])(
            command_list, allocator, compute_pipeline_state) != 0)
        return 27;
    if (beer_d3d12_snapshot_bytes() != 0 ||
        beer_d3d12_snapshot_peak_bytes() == 0 ||
        beer_d3d12_snapshot_peak_bytes() > (size_t)256 * 1024 * 1024)
        return 27;

    /* A default-heap VB/IB produced by earlier CopyBufferRegion commands must
     * expose the copied bytes to the later draw, not the destination's stale
     * command-recording-time contents. This reproduces RE8's GUI upload path. */
    uint32_t default_heap_properties[5] = {1, 0, 0, 0, 0};
    void *ordered_destination = NULL;
    if (((CreateCommittedResourceFn)vtable[27])(
            output, default_heap_properties, 0, &buffer_desc, 1, NULL,
            iid_resource, &ordered_destination) != 0 || !ordered_destination)
        return 91;
    void **ordered_destination_vtable = *(void ***)ordered_destination;
    g_ordered_buffer_copy_resource = ordered_destination;
    memcpy((uint8_t *)copy_source_data + 128, g_ordered_vertex_bytes,
           sizeof(g_ordered_vertex_bytes));
    memcpy((uint8_t *)copy_source_data + 160, g_ordered_index_bytes,
           sizeof(g_ordered_index_bytes));
    ((CopyBufferFn)list_vtable[15])(
        command_list, ordered_destination, 0, copy_source, 128,
        sizeof(g_ordered_vertex_bytes));
    ((CopyBufferFn)list_vtable[15])(
        command_list, ordered_destination, 64, copy_source, 160,
        sizeof(g_ordered_index_bytes));
    memset((uint8_t *)copy_source_data + 128, 0xcc,
           sizeof(g_ordered_vertex_bytes));
    memset((uint8_t *)copy_source_data + 160, 0xdd,
           sizeof(g_ordered_index_bytes));
    uint64_t ordered_address =
        ((GetGpuAddressFn)ordered_destination_vtable[11])(ordered_destination);
    BeerD3D12BufferView ordered_vertex = {ordered_address, 16, 16};
    BeerD3D12BufferView ordered_index = {ordered_address + 64, 6, 57};
    ((SetVertexBuffersFn)list_vtable[44])(
        command_list, 0, 1, &ordered_vertex);
    ((SetIndexBufferFn)list_vtable[43])(command_list, &ordered_index);
    ((DrawIndexedInstancedFn)list_vtable[13])(
        command_list, 3, 1, 0, 0, 0);
    if (((CloseFn)list_vtable[9])(command_list) != 0) return 92;
    void *ordered_lists[1] = {command_list};
    ((ExecuteListsFn)queue_vtable[10])(queue, 1, ordered_lists);
    if (g_ordered_buffer_copy_draw_seen != 1) return 93;
    /* The recorded draw retains the default-heap resource until Reset. */
    if (((ReleaseFn)ordered_destination_vtable[2])(ordered_destination) == 0)
        return 94;
    if (((ResetListFn)list_vtable[10])(
            command_list, allocator, compute_pipeline_state) != 0)
        return 95;
    g_ordered_buffer_copy_resource = NULL;

    /* Legacy and stream PSOs must expose decoded blend, rasterizer, depth and
     * render-target state to graphics executors. */
    {
        uint8_t legacy_state[656];
        memset(legacy_state, 0, sizeof(legacy_state));
        uint32_t *words = (uint32_t *)legacy_state;
        words[120 / 4 + 2] = 1;            /* RT0 BlendEnable */
        words[120 / 4 + 4] = 5;            /* SrcBlend SRC_ALPHA */
        words[120 / 4 + 5] = 6;            /* DestBlend INV_SRC_ALPHA */
        words[120 / 4 + 6] = 1;            /* BlendOp ADD */
        words[120 / 4 + 7] = 2;            /* SrcBlendAlpha ONE */
        words[120 / 4 + 8] = 1;            /* DestBlendAlpha ZERO */
        words[120 / 4 + 9] = 1;            /* BlendOpAlpha ADD */
        legacy_state[120 + 8 + 36] = 0x7;  /* RT0 write mask RGB */
        words[448 / 4] = UINT32_MAX;       /* SampleMask */
        words[568 / 4] = 1;                /* IBStripCutValue 0xffff */
        words[452 / 4] = 3;                /* FillMode SOLID */
        words[452 / 4 + 1] = 1;            /* CullMode NONE */
        words[576 / 4] = 1;                /* NumRenderTargets */
        words[580 / 4] = 10;               /* RTV[0] R16G16B16A16_FLOAT */
        words[616 / 4] = 1;                /* SampleDesc.Count */
        void *legacy_pso = NULL;
        if (((CreateLegacyPipelineStateFn)vtable[10])(
                output, legacy_state, iid_pipeline_state, &legacy_pso) != 0 ||
            !legacy_pso)
            return 28;
        ((SetPipelineStateFn)list_vtable[25])(command_list, legacy_pso);
        ((DrawInstancedFn)list_vtable[12])(command_list, 3, 1, 0, 0);
        if (((CloseFn)list_vtable[9])(command_list) != 0) return 28;
        void *state_lists[1] = {command_list};
        uint32_t before = g_graphics_count;
        ((ExecuteListsFn)queue_vtable[10])(queue, 1, state_lists);
        const BeerD3D12PipelineStateInfo *state = &g_last_graphics.pso;
        if (g_graphics_count != before + 1 || !state->valid ||
            !state->render_target[0].blend_enable ||
            state->render_target[0].src_blend != 5 ||
            state->render_target[0].dest_blend != 6 ||
            state->render_target[0].src_blend_alpha != 2 ||
            state->render_target[0].write_mask != 0x7 ||
            state->sample_mask != UINT32_MAX ||
            state->index_buffer_strip_cut_value != 1 ||
            state->fill_mode != 3 ||
            state->cull_mode != 1 || state->render_target_format_count != 1 ||
            state->render_target_formats[0] != 10 || state->sample_count != 1)
            return 28;
        ((ReleaseFn)(*(void ***)legacy_pso)[2])(legacy_pso);

        /* Stream PSO records use an aligned type/payload layout. Keep strip-cut
         * (type 13) distinct from primitive-topology type (14): confusing these
         * disables primitive restart and turns a cut index into real geometry. */
        uint8_t stream[336 + 48 + 56 + 8 + 8];
        memset(stream, 0, sizeof(stream));
        size_t at = 0;
        uint32_t type = 8;
        memcpy(stream + at, &type, 4);
        uint32_t *blend = (uint32_t *)(stream + at + 4);
        blend[2] = 1; blend[4] = 2; blend[5] = 2; blend[6] = 1;
        blend[7] = 2; blend[8] = 2; blend[9] = 1;
        ((uint8_t *)blend)[8 + 36] = 0xf;
        at += 336;
        type = 10;
        memcpy(stream + at, &type, 4);
        uint32_t *raster = (uint32_t *)(stream + at + 4);
        raster[0] = 3; raster[1] = 3; raster[2] = 1;
        at += 48;
        type = 11;
        memcpy(stream + at, &type, 4);
        uint32_t *depth = (uint32_t *)(stream + at + 4);
        depth[0] = 1; depth[1] = 1; depth[2] = 2;
        at += 56;
        type = 13;
        memcpy(stream + at, &type, 4);
        uint32_t strip_cut = 1; /* D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF */
        memcpy(stream + at + 4, &strip_cut, 4);
        at += 8;
        type = 14;
        memcpy(stream + at, &type, 4);
        uint32_t topology_type = 3; /* D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE */
        memcpy(stream + at + 4, &topology_type, 4);
        PipelineStreamDesc stream_desc = {.size = sizeof(stream),
                                          .stream = stream};
        void *stream_pso = NULL;
        void *device2_state = NULL;
        if (((QueryInterfaceFn)vtable[0])(output, iid_device2,
                                          &device2_state) != 0 ||
            ((CreatePipelineStateFn)(*(void ***)device2_state)[47])(
                device2_state, &stream_desc, iid_pipeline_state,
                &stream_pso) != 0 || !stream_pso)
            return 29;
        ((ReleaseFn)(*(void ***)device2_state)[2])(device2_state);
        if (((ResetListFn)list_vtable[10])(command_list, allocator,
                                           stream_pso) != 0)
            return 29;
        ((DrawInstancedFn)list_vtable[12])(command_list, 3, 1, 0, 0);
        if (((CloseFn)list_vtable[9])(command_list) != 0) return 29;
        before = g_graphics_count;
        ((ExecuteListsFn)queue_vtable[10])(queue, 1, state_lists);
        state = &g_last_graphics.pso;
        if (g_graphics_count != before + 1 || !state->valid ||
            !state->render_target[0].blend_enable ||
            state->render_target[0].src_blend != 2 ||
            state->render_target[0].write_mask != 0xf ||
            state->index_buffer_strip_cut_value != 1 ||
            state->cull_mode != 3 || !state->front_counter_clockwise ||
            !state->depth_enable || state->depth_func != 2)
            return 29;
        ((ReleaseFn)(*(void ***)stream_pso)[2])(stream_pso);
        if (((ResetListFn)list_vtable[10])(
                command_list, allocator, compute_pipeline_state) != 0)
            return 29;
    }

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

    /* ClearUnorderedAccessViewUint and DiscardResource are void command-list
     * operations. RE8 reaches both immediately after frame pacing starts. */
    typedef void (__attribute__((ms_abi)) *ClearUavUintFn)(
        void *, uint64_t, uint64_t, void *, const uint32_t *, uint32_t,
        const void *);
    typedef void (__attribute__((ms_abi)) *DiscardResourceFn)(
        void *, void *, const void *);
    const uint32_t uav_values[4] = {
        UINT32_C(0x11223344), UINT32_C(0x55667788),
        UINT32_C(0x99aabbcc), UINT32_C(0xddeeff00)
    };
    ((ClearUavUintFn)list_vtable[49])(
        command_list, 0, 0, copy_destination, uav_values, 0, NULL);
    if (memcmp(copy_destination_data, uav_values, sizeof(uav_values)) != 0 ||
        memcmp((uint8_t *)copy_destination_data + sizeof(uav_values),
               uav_values, sizeof(uav_values)) != 0)
        return 27;
    ((DiscardResourceFn)list_vtable[51])(
        command_list, copy_destination, NULL);
    /* Discard preserves allocation/lifetime and leaves contents undefined;
     * Beer's compatibility model intentionally retains the existing bytes. */
    if (memcmp(copy_destination_data, uav_values, sizeof(uav_values)) != 0)
        return 27;

    /* RE8 uploads textures with CopyTextureRegion from a placed upload-buffer
     * footprint into a texture subresource. The copy must execute in command
     * order, strip the upload row padding, and preserve compressed blocks. */
    typedef struct TestTextureCopyLocation {
        void *resource;
        uint32_t type;
        uint32_t padding;
        union {
            struct {
                uint64_t offset;
                uint32_t format, width, height, depth, row_pitch;
            } footprint;
            uint32_t subresource;
        } value;
    } TestTextureCopyLocation;
    typedef void (__attribute__((ms_abi)) *CopyTextureFn)(
        void *, const TestTextureCopyLocation *, uint32_t, uint32_t, uint32_t,
        const TestTextureCopyLocation *, const void *);
    ResourceDesc upload_texture_desc = {0};
    upload_texture_desc.dimension = 3;
    upload_texture_desc.width = 4;
    upload_texture_desc.height = 3;
    upload_texture_desc.depth_or_array_size = 1;
    upload_texture_desc.mip_levels = 1;
    upload_texture_desc.format = 28;
    upload_texture_desc.sample_count = 1;
    void *upload_texture = NULL;
    if (((CreateCommittedResourceFn)vtable[27])(
            output, heap_properties, 0, &upload_texture_desc, 4, NULL,
            iid_resource, &upload_texture) != 0 || !upload_texture)
        return 37;
    void **upload_texture_vtable = *(void ***)upload_texture;
    memset(copy_destination_data, 0, 64);
    memset(copy_source_data, 0, 2048);
    for (uint32_t row = 0; row < 3; ++row)
        for (uint32_t column = 0; column < 4; ++column) {
            uint8_t *pixel = (uint8_t *)copy_source_data + 512 + row * 256 +
                column * 4;
            pixel[0] = (uint8_t)(0x10 + row);
            pixel[1] = (uint8_t)(0x20 + column);
            pixel[2] = 0x30;
            pixel[3] = 0xff;
        }
    TestTextureCopyLocation rgba_destination = {
        .resource = upload_texture, .type = 0, .value.subresource = 0
    };
    TestTextureCopyLocation rgba_source = {
        .resource = copy_source, .type = 1,
        .value.footprint = {512, 28, 4, 3, 1, 256}
    };
    uint32_t rgba_box[6] = {1, 1, 0, 4, 3, 1};
    if (((ResetListFn)list_vtable[10])(
            command_list, allocator, compute_pipeline_state) != 0)
        return 37;
    ((CopyTextureFn)list_vtable[16])(
        command_list, &rgba_destination, 0, 0, 0, &rgba_source, rgba_box);
    /* Upload-ring reuse after recording must not alter the recorded payload. */
    memset((uint8_t *)copy_source_data + 512, 0xee, 768);
    if (((CloseFn)list_vtable[9])(command_list) != 0) return 37;
    ((ExecuteListsFn)queue_vtable[10])(queue, 1, compute_lists);
    if (((ResetListFn)list_vtable[10])(
            command_list, allocator, compute_pipeline_state) != 0)
        return 37;
    ((CopyResourceFn)list_vtable[17])(
        command_list, copy_destination, upload_texture);
    if (((CloseFn)list_vtable[9])(command_list) != 0) return 37;
    ((ExecuteListsFn)queue_vtable[10])(queue, 1, compute_lists);
    for (uint32_t row = 0; row < 2; ++row)
        for (uint32_t column = 0; column < 3; ++column) {
            const uint8_t *pixel = (const uint8_t *)copy_destination_data +
                row * 16 + column * 4;
            if (pixel[0] != 0x11 + row || pixel[1] != 0x21 + column ||
                pixel[2] != 0x30 || pixel[3] != 0xff)
                return 37;
        }
    if (g_last_submission.copy_resource_count != 1) return 37;
    ((ReleaseFn)upload_texture_vtable[2])(upload_texture);

    ResourceDesc bc_texture_desc = upload_texture_desc;
    bc_texture_desc.width = 8;
    bc_texture_desc.height = 8;
    bc_texture_desc.format = 98; /* BC7_UNORM */
    void *bc_texture = NULL;
    if (((CreateCommittedResourceFn)vtable[27])(
            output, heap_properties, 0, &bc_texture_desc, 4, NULL,
            iid_resource, &bc_texture) != 0 || !bc_texture)
        return 37;
    void **bc_texture_vtable = *(void ***)bc_texture;
    memset(copy_destination_data, 0, 64);
    memset(copy_source_data, 0, 2048);
    for (uint32_t block_row = 0; block_row < 2; ++block_row)
        for (uint32_t byte = 0; byte < 32; ++byte)
            ((uint8_t *)copy_source_data)[1024 + block_row * 256 + byte] =
                (uint8_t)(1 + block_row * 32 + byte);
    TestTextureCopyLocation bc_destination = {
        .resource = bc_texture, .type = 0, .value.subresource = 0
    };
    TestTextureCopyLocation bc_source = {
        .resource = copy_source, .type = 1,
        .value.footprint = {1024, 98, 8, 8, 1, 256}
    };
    if (((ResetListFn)list_vtable[10])(
            command_list, allocator, compute_pipeline_state) != 0)
        return 37;
    ((CopyTextureFn)list_vtable[16])(
        command_list, &bc_destination, 0, 0, 0, &bc_source, NULL);
    if (((CloseFn)list_vtable[9])(command_list) != 0) return 37;
    ((ExecuteListsFn)queue_vtable[10])(queue, 1, compute_lists);
    if (((ResetListFn)list_vtable[10])(
            command_list, allocator, compute_pipeline_state) != 0)
        return 37;
    ((CopyResourceFn)list_vtable[17])(
        command_list, copy_destination, bc_texture);
    if (((CloseFn)list_vtable[9])(command_list) != 0) return 37;
    ((ExecuteListsFn)queue_vtable[10])(queue, 1, compute_lists);
    for (uint32_t byte = 0; byte < 64; ++byte)
        if (((uint8_t *)copy_destination_data)[byte] != byte + 1)
            return 37;
    ((ReleaseFn)bc_texture_vtable[2])(bc_texture);
    if (((ResetListFn)list_vtable[10])(
            command_list, allocator, compute_pipeline_state) != 0)
        return 37;

    /* Small committed lookup volumes use ID3D12Resource::WriteToSubresource
     * rather than a copy command list. Preserve row/depth pitch and partial
     * boxes, then verify the symmetric ReadFromSubresource path. */
    ResourceDesc write_volume_desc = upload_texture_desc;
    write_volume_desc.dimension = 4;
    write_volume_desc.width = 4;
    write_volume_desc.height = 3;
    write_volume_desc.depth_or_array_size = 2;
    void *volume = NULL;
    if (((CreateCommittedResourceFn)vtable[27])(
            output, heap_properties, 0, &write_volume_desc, 4, NULL,
            iid_resource, &volume) != 0 || !volume)
        return 38;
    void **volume_vtable = *(void ***)volume;
    typedef uint64_t (__attribute__((ms_abi)) *WriteSubresourceFn)(
        void *, uint32_t, const void *, const void *, uint32_t, uint32_t);
    typedef uint64_t (__attribute__((ms_abi)) *ReadSubresourceFn)(
        void *, void *, uint32_t, uint32_t, uint32_t, const void *);
    uint8_t volume_source[2 * 64] = {0};
    uint8_t volume_readback[2 * 80];
    memset(volume_readback, 0xcc, sizeof(volume_readback));
    for (uint32_t z = 0; z < 2; ++z)
        for (uint32_t y = 0; y < 3; ++y)
            for (uint32_t x = 0; x < 4; ++x) {
                uint8_t *pixel = volume_source + z * 64 + y * 20 + x * 4;
                pixel[0] = (uint8_t)(0x10 + z);
                pixel[1] = (uint8_t)(0x20 + y);
                pixel[2] = (uint8_t)(0x30 + x);
                pixel[3] = 0xff;
            }
    if (((WriteSubresourceFn)volume_vtable[14])(
            volume, 0, NULL, volume_source, 20, 64) != 0)
        return 38;
    if (((ReadSubresourceFn)volume_vtable[13])(
            volume, volume_readback, 24, 80, 0, NULL) != 0)
        return 38;
    for (uint32_t z = 0; z < 2; ++z)
        for (uint32_t y = 0; y < 3; ++y)
            for (uint32_t x = 0; x < 4; ++x) {
                const uint8_t *pixel = volume_readback + z * 80 + y * 24 + x * 4;
                if (pixel[0] != 0x10 + z || pixel[1] != 0x20 + y ||
                    pixel[2] != 0x30 + x || pixel[3] != 0xff)
                    return 38;
            }
    ((ReleaseFn)volume_vtable[2])(volume);

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
        g_present_serial != 0 || g_present_width != 4 ||
        g_present_height != 2 || g_present_pitch != 16)
        return 28;
    /* Present only consumes the resource. Repeated presents without an
     * intervening write must retain the exact content serial so a renderer
     * mirror produced at that serial remains discoverable. */
    if (((DownlevelPresentFn)downlevel_vtable[3])(
            downlevel_queue, command_list, present_resource, 1, 0) != 0 ||
        g_present_count != 2 || g_present_serial != 0)
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
     * rectangle bounds, preserve native RGBA/BGRA channel ordering, and defer
     * all mutation until queue submission so later recorded clears cannot erase
     * earlier recorded draws. */
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
    /* A second recorded clear must not mutate the target or invoke the backend
     * until queue submission. */
    ((ClearRenderTargetFn)list_vtable[48])(
        command_list, (uint64_t)(uintptr_t)clear_descriptor,
        clear_color, 0, NULL);
    if (g_clear_count != 0) return 28;
    if (((CloseFn)list_vtable[9])(command_list) != 0) return 28;
    void *clear_lists[1] = {command_list};
    ((ExecuteListsFn)queue_vtable[10])(queue, 1, clear_lists);
    if (g_clear_count != 1 ||
        g_last_submission.clear_render_target_count != 2)
        return 28;
    void **clear_resource_vtable = *(void ***)clear_resource;
    const uint8_t expected_clear[4] = {64, 128, 255, 255};
    /* Present the cleared texture after execution to prove the ordered command
     * reaches the application-owned downlevel back buffer. */
    if (((DownlevelPresentFn)downlevel_vtable[3])(
            downlevel_queue, command_list, clear_resource, 1, 0) != 0 ||
        g_present_count != 3 || g_present_serial == 0 ||
        memcmp(g_present_pixels, expected_clear, 4) != 0 ||
        memcmp(g_present_pixels + (1 * 4 + 1) * 4, expected_clear, 4) != 0)
        return 28;
    ((ReleaseFn)clear_resource_vtable[2])(clear_resource);
    /* Reusing descriptor storage after the referenced resource dies must not
     * let clear/OM operations dereference stale descriptor bytes. */
    ((ClearRenderTargetFn)list_vtable[48])(
        command_list, (uint64_t)(uintptr_t)clear_descriptor,
        clear_color, 0, NULL);
    if (((ResetListFn)list_vtable[10])(
            command_list, allocator, compute_pipeline_state) != 0)
        return 28;

    /* ClearDepthStencilView updates the authoritative CPU backing consumed by
     * later Vulkan depth/stencil attachments. Verify full and rectangular
     * clears preserve untouched texels and both packed aspects. */
    ResourceDesc depth_desc = clear_desc;
    depth_desc.width = 3;
    depth_desc.height = 2;
    depth_desc.format = 20; /* D32_FLOAT_S8X24_UINT */
    void *depth_resource = NULL;
    if (((CreateCommittedResourceFn)vtable[27])(
            output, heap_properties, 0, &depth_desc, 4, NULL,
            iid_resource, &depth_resource) != 0 || !depth_resource)
        return 29;
    void **depth_resource_vtable = *(void ***)depth_resource;
    uint8_t packed_depth_descriptor[32] = {0};
    ((CreateDepthViewFn)vtable[21])(
        output, depth_resource, NULL,
        (uint64_t)(uintptr_t)packed_depth_descriptor);
    if (((ResetListFn)list_vtable[10])(
            command_list, allocator, compute_pipeline_state) != 0)
        return 29;
    uint64_t packed_depth_handle =
        (uint64_t)(uintptr_t)packed_depth_descriptor;
    ((SetRenderTargetsFn)list_vtable[46])(
        command_list, 0, NULL, 0, &packed_depth_handle);
    g_depth_history_count = 0;
    g_execution_order_count = 0;
    g_compute_batch_begin_count = 0;
    g_compute_batch_end_count = 0;
    g_compute_ir_batch_count = 0;
    g_compute_ir_operation_count = 0;
    g_compute_ir_access_count = 0;
    g_compute_ir_hazards = 0;
    ((ClearDepthStencilFn)list_vtable[47])(
        command_list, (uint64_t)(uintptr_t)packed_depth_descriptor,
        3, 1.0f, 7, 0, NULL);
    ((DrawInstancedFn)list_vtable[12])(command_list, 3, 1, 0, 0);
    TestRect depth_rect = {1, 0, 3, 1};
    ((ClearDepthStencilFn)list_vtable[47])(
        command_list, (uint64_t)(uintptr_t)packed_depth_descriptor,
        1, 0.25f, 99, 1, &depth_rect);
    ((SetComputeRootFn)list_vtable[29])(command_list, compute_root_signature);
    ((SetRootDescriptorFn)list_vtable[41])(
        command_list, 7, test_index_address);
    ((DispatchFn)list_vtable[14])(command_list, 1, 1, 1);
    ((DrawInstancedFn)list_vtable[12])(command_list, 3, 1, 0, 0);
    if (((CloseFn)list_vtable[9])(command_list) != 0) return 29;
    void *depth_lists[1] = {command_list};
    ((ExecuteListsFn)queue_vtable[10])(queue, 1, depth_lists);
    if (g_depth_history_count != 2 ||
        g_depth_history_sizes[0] < 48 || g_depth_history_sizes[1] < 48 ||
        g_execution_order_count != 3 ||
        memcmp(g_execution_order, "GCG", 3) != 0 ||
        g_compute_batch_begin_count != 1 ||
        g_compute_batch_end_count != 1 ||
        g_compute_ir_batch_count != 1 ||
        g_compute_ir_operation_count != 1 ||
        g_compute_ir_hazards != 0)
        return 29;
    uint32_t depth_one = 0, depth_quarter = 0;
    float one = 1.0f, quarter = 0.25f;
    memcpy(&depth_one, &one, sizeof(depth_one));
    memcpy(&depth_quarter, &quarter, sizeof(depth_quarter));
    for (uint32_t texel = 0; texel < 6; ++texel) {
        uint32_t first_actual = 0, second_actual = 0;
        memcpy(&first_actual, g_depth_history[0] + texel * 8u,
               sizeof(first_actual));
        memcpy(&second_actual, g_depth_history[1] + texel * 8u,
               sizeof(second_actual));
        int inside = texel == 1 || texel == 2;
        if (first_actual != depth_one ||
            g_depth_history[0][texel * 8u + 4] != 7 ||
            second_actual != (inside ? depth_quarter : depth_one) ||
            g_depth_history[1][texel * 8u + 4] != 7)
            return 29;
    }
    ((ReleaseFn)depth_resource_vtable[2])(depth_resource);

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
    if (memcmp(descriptor_storage, descriptor_data, 24) != 0)
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

    /* A Texture3D has one subresource per mip, and each footprint covers all
     * depth slices at that mip. TotalBytes must include every slice rather
     * than only the first 2D plane. */
    ResourceDesc volume_desc = texture_desc;
    volume_desc.dimension = 4;
    volume_desc.width = 8;
    volume_desc.height = 4;
    volume_desc.depth_or_array_size = 4;
    volume_desc.mip_levels = 2;
    memset(footprint, 0, sizeof(footprint));
    row_count = 0;
    row_size = 0;
    total_size = 0;
    ((GetFootprintsFn)vtable[38])(output, &volume_desc, 1, 1, 0,
                                  footprint, &row_count, &row_size, &total_size);
    uint32_t footprint_height = 0, footprint_depth = 0;
    memcpy(&footprint_width, footprint + 12, sizeof(footprint_width));
    memcpy(&footprint_height, footprint + 16, sizeof(footprint_height));
    memcpy(&footprint_depth, footprint + 20, sizeof(footprint_depth));
    memcpy(&row_pitch, footprint + 24, sizeof(row_pitch));
    if (footprint_width != 4 || footprint_height != 2 ||
        footprint_depth != 2 || row_count != 2 || row_size != 16 ||
        row_pitch != 256 || total_size != 1024)
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
    uint32_t indirect_arguments[8] = {
        2, 0, 0, 0,
        3, 0, 0, 0,
    };
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

    /* ExecuteIndirect must decode standard draw, indexed-draw, and dispatch
     * records, honor byte strides/offsets, and clamp through a count buffer. */
    typedef void (__attribute__((ms_abi)) *ExecuteIndirectFn)(
        void *, void *, uint32_t, void *, uint64_t, void *, uint64_t);
    /* D3D12_INDIRECT_ARGUMENT_TYPE uses 0=draw, 1=draw-indexed,
     * 2=dispatch. Keep these values aligned with the public D3D12 ABI; the
     * former 2/3/4 test values masked enum-decoding regressions. */
    uint32_t draw_argument_desc[4] = {0, 0, 0, 0};
    uint32_t indexed_argument_desc[4] = {1, 0, 0, 0};
    uint32_t dispatch_argument_desc[4] = {2, 0, 0, 0};
    CommandSignatureDesc draw_signature_desc = {
        .byte_stride = 20, .argument_count = 1,
        .arguments = draw_argument_desc, .node_mask = 0,
    };
    CommandSignatureDesc indexed_signature_desc = {
        .byte_stride = 24, .argument_count = 1,
        .arguments = indexed_argument_desc, .node_mask = 0,
    };
    CommandSignatureDesc dispatch_signature_desc = {
        .byte_stride = 16, .argument_count = 1,
        .arguments = dispatch_argument_desc, .node_mask = 0,
    };
    void *draw_signature = NULL;
    void *indexed_signature = NULL;
    void *dispatch_signature = NULL;
    if (((CreateCommandSignatureFn)vtable[41])(
            output, &draw_signature_desc, NULL, iid_command_signature,
            &draw_signature) != 0 || !draw_signature ||
        ((CreateCommandSignatureFn)vtable[41])(
            output, &indexed_signature_desc, NULL, iid_command_signature,
            &indexed_signature) != 0 || !indexed_signature ||
        ((CreateCommandSignatureFn)vtable[41])(
            output, &dispatch_signature_desc, NULL, iid_command_signature,
            &dispatch_signature) != 0 || !dispatch_signature)
        return 33;
    if (((ResetListFn)list_vtable[10])(
            command_list, allocator, compute_pipeline_state) != 0)
        return 33;
    buffer_desc.width = 1024;
    void *indirect_buffer = NULL;
    if (((CreateCommittedResourceFn)vtable[27])(
            output, heap_properties, 0, &buffer_desc, 1, NULL,
            iid_resource, &indirect_buffer) != 0 || !indirect_buffer)
        return 33;
    void **indirect_buffer_vtable = *(void ***)indirect_buffer;
    void *indirect_data = NULL;
    if (((MapFn)indirect_buffer_vtable[8])(
            indirect_buffer, 0, NULL, &indirect_data) != 0 || !indirect_data)
        return 33;
    uint32_t indirect_draws[10] = {
        3, 1, 4, 5, 0xcccccccc,
        6, 2, 7, 8, 0xdddddddd,
    };
    uint32_t indirect_indexed[6] = {9, 3, 10, (uint32_t)-2, 11, 0};
    uint32_t indirect_dispatch[4] = {12, 13, 14, 0};
    uint32_t indirect_count = 2;
    memcpy((uint8_t *)indirect_data + 512, indirect_draws,
           sizeof(indirect_draws));
    memcpy((uint8_t *)indirect_data + 640, indirect_indexed,
           sizeof(indirect_indexed));
    memset((uint8_t *)indirect_data + 704, 0, sizeof(indirect_dispatch));
    memcpy((uint8_t *)copy_source_data + 256, indirect_dispatch,
           sizeof(indirect_dispatch));
    /* Indirect dispatch data is commonly produced by earlier GPU commands.
     * It must be resolved in queue order, not expanded from stale bytes while
     * ExecuteIndirect is being recorded. */
    ((CopyBufferFn)list_vtable[15])(
        command_list, indirect_buffer, 704, copy_source, 256,
        sizeof(indirect_dispatch));
    memset((uint8_t *)copy_source_data + 256, 0xee,
           sizeof(indirect_dispatch));
    memcpy((uint8_t *)indirect_data + 768, &indirect_count,
           sizeof(indirect_count));
    uint32_t graphics_before_indirect = g_graphics_count;
    uint32_t compute_before_indirect = g_compute_count;
    uint32_t source_sync_before_indirect = g_source_sync_count;
    ((ExecuteIndirectFn)list_vtable[59])(
        command_list, draw_signature, 4, indirect_buffer, 512,
        indirect_buffer, 768);
    ((ExecuteIndirectFn)list_vtable[59])(
        command_list, indexed_signature, 1, indirect_buffer, 640, NULL, 0);
    /* A second adjacent indexed expansion reuses the same VB/IB resources.
     * Queue replay must publish each distinct geometry resource once per
     * producer boundary, not once per ExecuteIndirect record. */
    ((ExecuteIndirectFn)list_vtable[59])(
        command_list, indexed_signature, 1, indirect_buffer, 640, NULL, 0);
    ((ExecuteIndirectFn)list_vtable[59])(
        command_list, dispatch_signature, 1, indirect_buffer, 704, NULL, 0);
    if (((CloseFn)list_vtable[9])(command_list) != 0) return 33;
    void *indirect_lists[1] = {command_list};
    ((ExecuteListsFn)queue_vtable[10])(queue, 1, indirect_lists);
    if (g_graphics_count != graphics_before_indirect + 4 ||
        g_graphics_batch_count != 2 ||
        g_compute_count != compute_before_indirect + 1 ||
        !g_last_graphics.indexed || g_last_graphics.element_count != 9 ||
        g_last_graphics.instance_count != 3 ||
        g_last_graphics.start_element != 10 ||
        g_last_graphics.base_vertex != -2 ||
        g_last_graphics.start_instance != 11 ||
        g_last_compute.group_count_x != 12 ||
        g_last_compute.group_count_y != 13 ||
        g_last_compute.group_count_z != 14 ||
        g_graphics_defer_history[graphics_before_indirect] != 1 ||
        g_graphics_defer_history[graphics_before_indirect + 1] != 0 ||
        /* The two adjacent indexed records merge into one backend batch: the
         * first retains GPU-authoritative targets and the final one publishes. */
        g_graphics_defer_history[graphics_before_indirect + 2] != 1 ||
        g_graphics_defer_history[graphics_before_indirect + 3] != 0 ||
        /* Indexed indirect replay must synchronize its retained default-heap
         * geometry as well as the GPU-generated argument/count buffers. */
        g_source_sync_count <= source_sync_before_indirect)
        return 33;
    /* Adjacent records sharing argument/count/VB/IB state must synchronize
     * each read-only source once without invoking destructive invalidation. */
    for (uint32_t i = source_sync_before_indirect;
         i < g_source_sync_count && i < 64; ++i)
        for (uint32_t j = i + 1; j < g_source_sync_count && j < 64; ++j)
            if (g_source_sync_history[i] == g_source_sync_history[j])
                return 33;
    ((ReleaseFn)(*(void ***)draw_signature)[2])(draw_signature);
    ((ReleaseFn)(*(void ***)indexed_signature)[2])(indexed_signature);
    ((ReleaseFn)(*(void ***)dispatch_signature)[2])(dispatch_signature);
    ((ReleaseFn)indirect_buffer_vtable[2])(indirect_buffer);

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

    /* Version 1.1 descriptor ranges place Flags at byte 16 and
     * OffsetInDescriptorsFromTableStart at byte 20. Keep them distinct: using
     * Flags as the table offset binds the wrong CBV/SRV while remaining ABI-
     * valid, and can turn shader loop bounds into arbitrary resource data. */
    uint8_t range_v11[24] = {0};
    uint32_t range_type = 2, range_count = 1, range_register = 1;
    uint32_t range_flags = 4, range_offset = 7;
    memcpy(range_v11, &range_type, 4);
    memcpy(range_v11 + 4, &range_count, 4);
    memcpy(range_v11 + 8, &range_register, 4);
    memcpy(range_v11 + 16, &range_flags, 4);
    memcpy(range_v11 + 20, &range_offset, 4);
    uint8_t parameter_v11[32] = {0};
    memcpy(parameter_v11 + 8, &range_count, 4);
    const void *range_pointer = range_v11;
    memcpy(parameter_v11 + 16, &range_pointer, sizeof(range_pointer));
    uint8_t root_desc_v11[48] = {0};
    root_version = 2;
    memcpy(root_desc_v11, &root_version, 4);
    memcpy(root_desc_v11 + 8, &range_count, 4);
    const void *parameter_pointer = parameter_v11;
    memcpy(root_desc_v11 + 16, &parameter_pointer, sizeof(parameter_pointer));
    void *serialized_v11 = NULL;
    if (beer_d3d12_serialize_versioned_root_signature(
            root_desc_v11, &serialized_v11, NULL) != 0 || !serialized_v11)
        return 34;
    void **v11_blob_vtable = *(void ***)serialized_v11;
    const uint8_t *v11_bytes =
        ((GetBufferPointerFn)v11_blob_vtable[3])(serialized_v11);
    size_t v11_size = ((GetBufferSizeFn)v11_blob_vtable[4])(serialized_v11);
    uint32_t serialized_flags = 0, serialized_offset = 0;
    if (!v11_bytes || v11_size < 60) return 34;
    memcpy(&serialized_flags, v11_bytes + 52, 4);
    memcpy(&serialized_offset, v11_bytes + 56, 4);
    if (serialized_flags != range_flags || serialized_offset != range_offset)
        return 34;
    ((ReleaseFn)v11_blob_vtable[2])(serialized_v11);

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
