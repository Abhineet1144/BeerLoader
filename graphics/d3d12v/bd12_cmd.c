#define _GNU_SOURCE
#include <stdio.h>
#include "bd12.h"

#include <stdlib.h>
#include <string.h>

/* D3D12_RESOURCE_STATES -> synchronization2 ------------------------------ */

#define BD12_ALL_SHADER_STAGES                                               \
    (VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |                                 \
     VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT |                   \
     VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT |                \
     VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT |                               \
     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |                               \
     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT)
#define BD12_NON_PIXEL_STAGES \
    (BD12_ALL_SHADER_STAGES & ~VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT)
#define BD12_SHADER_READ                                                     \
    (VK_ACCESS_2_UNIFORM_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |    \
     VK_ACCESS_2_SHADER_STORAGE_READ_BIT)
#define BD12_WRITE_ACCESS                                                    \
    (VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |                                  \
     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |                                \
     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |                        \
     VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT |         \
     VK_ACCESS_2_SHADER_WRITE_BIT)

void bd12_state_sync(uint32_t state, int is_source,
                     VkPipelineStageFlags2 *stage, VkAccessFlags2 *access)
{
    VkPipelineStageFlags2 s = 0;
    VkAccessFlags2 a = 0;
    if (state == BD12_STATE_COMMON) {
        s = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        a = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    }
    if (state & BD12_STATE_VERTEX_AND_CONSTANT_BUFFER) {
        s |= VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | BD12_ALL_SHADER_STAGES;
        a |= VK_ACCESS_2_UNIFORM_READ_BIT |
             VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
    }
    if (state & BD12_STATE_INDEX_BUFFER) {
        s |= VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
        a |= VK_ACCESS_2_INDEX_READ_BIT;
    }
    if (state & BD12_STATE_RENDER_TARGET) {
        s |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        a |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
             VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    }
    if (state & BD12_STATE_UNORDERED_ACCESS) {
        s |= BD12_ALL_SHADER_STAGES;
        a |= VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
             VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    }
    if (state & (BD12_STATE_DEPTH_WRITE | BD12_STATE_DEPTH_READ)) {
        s |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
             VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        a |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        if (state & BD12_STATE_DEPTH_WRITE)
            a |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }
    if (state & BD12_STATE_NON_PIXEL_SHADER_RESOURCE) {
        s |= BD12_NON_PIXEL_STAGES;
        a |= BD12_SHADER_READ;
    }
    if (state & BD12_STATE_PIXEL_SHADER_RESOURCE) {
        s |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        a |= BD12_SHADER_READ;
    }
    if (state & BD12_STATE_STREAM_OUT) {
        s |= VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        a |= VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    }
    if (state & BD12_STATE_INDIRECT_ARGUMENT) {
        s |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
        a |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
    }
    if (state & BD12_STATE_COPY_DEST) {
        s |= VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        a |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
    }
    if (state & BD12_STATE_COPY_SOURCE) {
        s |= VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        a |= VK_ACCESS_2_TRANSFER_READ_BIT;
    }
    if (state & (BD12_STATE_RESOLVE_DEST | BD12_STATE_RESOLVE_SOURCE)) {
        s |= VK_PIPELINE_STAGE_2_RESOLVE_BIT;
        a |= VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    }
    /* A source scope only needs to make writes available; reads need an
     * execution dependency alone. */
    if (is_source) a &= BD12_WRITE_ACCESS;
    *stage = s;
    *access = a;
}

/* Allocators and lists ------------------------------------------------------ */

BD12CmdAllocator *bd12_cmd_allocator_create(BD12Device *device)
{
    BD12CmdAllocator *allocator = calloc(1, sizeof(*allocator));
    if (!allocator) return NULL;
    allocator->device = device;
    VkCommandPoolCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = device->queue_family
    };
    if (device->vk.CreateCommandPool(device->device, &info, NULL,
                                     &allocator->pool) != VK_SUCCESS) {
        free(allocator);
        return NULL;
    }
    return allocator;
}

void bd12_cmd_allocator_destroy(BD12CmdAllocator *allocator)
{
    if (!allocator) return;
    BD12Device *device = allocator->device;
    for (uint32_t i = 0; i < allocator->descriptor_pool_count; ++i)
        device->vk.DestroyDescriptorPool(device->device,
                                         allocator->descriptor_pools[i], NULL);
    free(allocator->descriptor_pools);
    for (uint32_t i = 0; i < allocator->scratch_count; ++i)
        bd12_buffer_destroy(allocator->scratch[i]);
    free(allocator->scratch);
    device->vk.DestroyCommandPool(device->device, allocator->pool, NULL);
    free(allocator);
}

int bd12_cmd_allocator_reset(BD12CmdAllocator *allocator)
{
    BD12Device *device = allocator->device;
    for (uint32_t i = 0; i < allocator->descriptor_pool_count; ++i)
        device->vk.ResetDescriptorPool(device->device,
                                       allocator->descriptor_pools[i], 0);
    allocator->descriptor_pool_current = 0;
    allocator->scratch_current = 0;
    allocator->scratch_offset = 0;
    return device->vk.ResetCommandPool(device->device, allocator->pool, 0) ==
           VK_SUCCESS;
}

int bd12_cmd_append_segment(BD12CmdList *list)
{
    /* A new primary command buffer starts with no bound graphics state, but the
     * list-level tracker still believes the previous buffer's pipeline, vertex
     * and index buffers, viewport, scissor and dynamic state are bound. Draws
     * recorded after a segment split would otherwise run with nothing bound,
     * so force a full rebind on the next draw. */
    list->gfx.dirty = BD12_DIRTY_ALL;
    /* Segments already allocated by a previous recording of this list are
     * reused. The command pool is reset by the owning D3D12 allocator, so
     * reallocating here leaked one primary command buffer per segment per
     * frame and eventually exhausted the pool. */
    if (list->command_buffer_count < list->command_buffer_allocated) {
        list->command_buffer = list->command_buffers[list->command_buffer_count];
        ++list->command_buffer_count;
        return 1;
    }
    if (list->command_buffer_count == list->command_buffer_capacity) {

        uint32_t capacity = list->command_buffer_capacity ?
                            list->command_buffer_capacity * 2 : 4;
        VkCommandBuffer *buffers = realloc(list->command_buffers,
                                           capacity * sizeof(*buffers));
        if (!buffers) return 0;
        list->command_buffers = buffers;
        list->command_buffer_capacity = capacity;
    }
    VkCommandBufferAllocateInfo info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = list->allocator->pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };
    VkCommandBuffer command;
    if (list->device->vk.AllocateCommandBuffers(list->device->device, &info,
                                                &command) != VK_SUCCESS)
        return 0;
    list->command_buffers[list->command_buffer_count] = command;
    ++list->command_buffer_count;
    list->command_buffer_allocated = list->command_buffer_count;
    list->command_buffer = command;
    return 1;
}


BD12CmdList *bd12_cmd_list_create(BD12CmdAllocator *allocator)
{
    BD12CmdList *list = calloc(1, sizeof(*list));
    if (!list) return NULL;
    list->device = allocator->device;
    list->allocator = allocator;
    if (!bd12_cmd_append_segment(list)) {
        free(list->command_buffers);
        free(list);
        return NULL;
    }
    return list;
}

void bd12_cmd_list_destroy(BD12CmdList *list, BD12CmdAllocator *allocator)
{
    if (!list) return;
    if (list->command_buffer_allocated)
        list->device->vk.FreeCommandBuffers(list->device->device,
            allocator->pool, list->command_buffer_allocated,
            list->command_buffers);

    free(list->command_buffers);
    free(list);
}

void bd12_cmd_record_flight_op(BD12CmdList *list, BD12FlightOpType type,
                               uint64_t pipeline_hash, uint64_t amount,
                               uint32_t width, uint32_t height, uint32_t depth)
{
    bd12_ckpt_mark(list, (uint32_t)type, amount, pipeline_hash, width, height, depth);
    BD12FlightOp *entry = &list->op_flight[list->op_count++ % BD12_FLIGHT_OPS];
    *entry = (BD12FlightOp){ pipeline_hash, amount, type, width, height, depth,
                             list->command_buffer_count - 1 };
}

int bd12_cmd_list_begin(BD12CmdList *list)
{
    list->command_buffer_count = 1;
    list->command_buffer = list->command_buffers[0];
    VkCommandBufferBeginInfo info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = 0  /* D3D12 lists may be executed repeatedly */
    };
    if (list->device->vk.BeginCommandBuffer(list->command_buffer, &info) !=
        VK_SUCCESS)
        return 0;
    memset(list->heaps, 0, sizeof(list->heaps));
    list->compute_pipeline = NULL;
    memset(&list->compute, 0, sizeof(list->compute));
    memset(&list->graphics, 0, sizeof(list->graphics));
    memset(&list->gfx, 0, sizeof(list->gfx));
    memset(list->dispatch_flight, 0, sizeof(list->dispatch_flight));
    list->dispatch_count = 0;
    memset(list->op_flight, 0, sizeof(list->op_flight));
    list->op_count = 0;
    list->gfx.dirty = BD12_DIRTY_ALL;
    list->gfx.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    list->gfx.index_type = VK_INDEX_TYPE_UINT16;
    list->recording = 1;
    return 1;
}

int bd12_cmd_list_end(BD12CmdList *list)
{
    bd12_cmd_end_rendering(list);
    list->recording = 0;
    return list->device->vk.EndCommandBuffer(list->command_buffer) == VK_SUCCESS;
}

/* Commands ----------------------------------------------------------------- */

void bd12_cmd_copy_buffer(BD12CmdList *list, BD12Buffer *dst,
                          VkDeviceSize dst_offset, BD12Buffer *src,
                          VkDeviceSize src_offset, VkDeviceSize size)
{
    if (dst->heap && dst->heap->type == BD12_HEAP_READBACK && getenv("BEER_BD12_RB_LOG"))
        fprintf(stderr, "[RB] buffer->readback bytes=%llu src_va=%llx+%llu\n",
                (unsigned long long)size, (unsigned long long)src->va,
                (unsigned long long)src_offset);
    if (dst->heap && dst->heap->type == BD12_HEAP_READBACK)
        bd12_capture_readback(dst, dst_offset, size, src->va + src_offset);
    bd12_cmd_end_rendering(list);
    VkBufferCopy region = {
        .srcOffset = src_offset,
        .dstOffset = dst_offset,
        .size = size
    };
    bd12_cmd_record_flight_op(list, BD12_FLIGHT_COPY_BUFFER, 0, size, 0, 0, 0);
    list->device->vk.CmdCopyBuffer(list->command_buffer, src->buffer,
                                   dst->buffer, 1, &region);
}

static void buffer_barrier(BD12CmdList *list, BD12Buffer *buffer,
                           VkPipelineStageFlags2 src_stage,
                           VkAccessFlags2 src_access,
                           VkPipelineStageFlags2 dst_stage,
                           VkAccessFlags2 dst_access)
{
    bd12_cmd_end_rendering(list);
    VkBufferMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffer->buffer,
        .offset = 0,
        .size = VK_WHOLE_SIZE
    };
    VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &barrier
    };
    list->device->vk.CmdPipelineBarrier2(list->command_buffer, &dependency);
}

void bd12_cmd_transition_buffer(BD12CmdList *list, BD12Buffer *buffer,
                                uint32_t before, uint32_t after)
{
    if (before == after) return;
    VkPipelineStageFlags2 src_stage, dst_stage;
    VkAccessFlags2 src_access, dst_access;
    bd12_state_sync(before, 1, &src_stage, &src_access);
    bd12_state_sync(after, 0, &dst_stage, &dst_access);
    buffer_barrier(list, buffer, src_stage, src_access, dst_stage, dst_access);
}

void bd12_cmd_uav_barrier_buffer(BD12CmdList *list, BD12Buffer *buffer)
{
    VkPipelineStageFlags2 stage;
    VkAccessFlags2 access;
    bd12_state_sync(BD12_STATE_UNORDERED_ACCESS, 0, &stage, &access);
    buffer_barrier(list, buffer, stage, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   stage, access);
}

void bd12_cmd_host_visibility(BD12CmdList *list, BD12Buffer *buffer)
{
    buffer_barrier(list, buffer,
                   VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT |
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT |
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
}

/* Compute pipelines ----------------------------------------------------------- */

BD12ComputePipeline *bd12_compute_pipeline_build(
    BD12Device *device, const uint32_t *spirv, size_t spirv_bytes,
    uint32_t push_bytes, VkDescriptorSetLayout set_layout,
    uint32_t bindless_words)
{
    BD12ComputePipeline *pipeline = calloc(1, sizeof(*pipeline));
    if (!pipeline) return NULL;
    pipeline->device = device;
    pipeline->push_bytes = push_bytes;
    pipeline->bindless_words = bindless_words;
    VkDescriptorSetLayout set_layouts[2] = { set_layout, VK_NULL_HANDLE };
    uint32_t set_layout_count = set_layout ? 1u : 0u;
    if (bindless_words) {
        set_layouts[1] = bd12_mirror_layout(device);
        if (!set_layouts[1] || !set_layout) goto fail;
        set_layout_count = 2;
        push_bytes = bindless_words * 4;
    }
    pipeline->set_layout = set_layout;
    VkShaderModuleCreateInfo module_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = spirv_bytes,
        .pCode = spirv
    };
    VkShaderModule module;
    const VkResult module_result = device->vk.CreateShaderModule(
        device->device, &module_info, NULL, &module);
    if (module_result != VK_SUCCESS)
        goto fail;
    VkPushConstantRange range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .size = push_bytes
    };
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = push_bytes ? 1u : 0u,
        .pPushConstantRanges = &range,
        .setLayoutCount = set_layout_count,
        .pSetLayouts = set_layouts
    };
    if (device->vk.CreatePipelineLayout(device->device, &layout_info, NULL,
                                        &pipeline->layout) != VK_SUCCESS)
        goto fail_module;
    VkComputePipelineCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        /* Watchdog-sensitive dispatches are split with vkCmdDispatchBase so
         * every tile preserves D3D12 SV_GroupID. Vulkan requires this flag on
         * any pipeline used by that command; omitting it is invalid and caused
         * undefined NVIDIA execution at the first nonzero-base tile. */
        .flags = VK_PIPELINE_CREATE_DISPATCH_BASE_BIT,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = module,
            .pName = "main"
        },
        .layout = pipeline->layout
    };
    VkResult result = device->vk.CreateComputePipelines(
        device->device, VK_NULL_HANDLE, 1, &info, NULL, &pipeline->pipeline);
    device->vk.DestroyShaderModule(device->device, module, NULL);
    if (result != VK_SUCCESS) {
        device->vk.DestroyPipelineLayout(device->device, pipeline->layout, NULL);
        goto fail;
    }
    return pipeline;

fail_module:
    device->vk.DestroyShaderModule(device->device, module, NULL);
fail:
    free(pipeline);
    return NULL;
}

void bd12_compute_pipeline_destroy(BD12ComputePipeline *pipeline)
{
    if (!pipeline) return;
    pipeline->device->vk.DestroyPipeline(pipeline->device->device,
                                         pipeline->pipeline, NULL);
    pipeline->device->vk.DestroyPipelineLayout(pipeline->device->device,
                                               pipeline->layout, NULL);
    if (pipeline->set_layout)
        pipeline->device->vk.DestroyDescriptorSetLayout(
            pipeline->device->device, pipeline->set_layout, NULL);
    free(pipeline->bindings);
    free(pipeline->bindless);
    free(pipeline->binding_types);
    free(pipeline);
}

void bd12_cmd_dispatch(BD12CmdList *list, BD12ComputePipeline *pipeline,
                       const void *push, uint32_t x, uint32_t y, uint32_t z)
{
    BD12Device *device = list->device;
    device->vk.CmdBindPipeline(list->command_buffer,
                               VK_PIPELINE_BIND_POINT_COMPUTE,
                               pipeline->pipeline);
    if (pipeline->push_bytes && push)
        device->vk.CmdPushConstants(list->command_buffer, pipeline->layout,
                                    VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                    pipeline->push_bytes, push);
    device->vk.CmdDispatch(list->command_buffer, x, y, z);
}
