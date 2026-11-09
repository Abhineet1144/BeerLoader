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

/* Task and mesh shaders read and write resources like the other pre-rasterization stages, so every state a shader
 * accesses covers them too. Their stage bits are valid only when the device enabled mesh shaders. */
static VkPipelineStageFlags2 bd12_mesh_stages(void)
{
    static int cached = -1;
    if (cached < 0) {
        const BeerVulkanModern *m = beer_vulkan_modern();
        cached = m && m->mesh_shader_tier ? 1 : 0;
    }
    return cached ? (VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT) : 0;
}

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
        s |= VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | BD12_ALL_SHADER_STAGES | bd12_mesh_stages();
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
        s |= BD12_ALL_SHADER_STAGES | bd12_mesh_stages();
        a |= VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
             VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    }
    if (state & (BD12_STATE_DEPTH_WRITE | BD12_STATE_DEPTH_READ | BD12_STATE_DEPTH_READ_STENCIL_WRITE)) {
        s |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
             VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        a |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        if (state & (BD12_STATE_DEPTH_WRITE | BD12_STATE_DEPTH_READ_STENCIL_WRITE))
            a |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }
    if (state & BD12_STATE_NON_PIXEL_SHADER_RESOURCE) {
        s |= BD12_NON_PIXEL_STAGES | bd12_mesh_stages();
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
    if (state & BD12_STATE_SHADING_RATE_SOURCE) {
        /* Read by the rasterizer as a fragment shading rate attachment (VRS tier 2). */
        const BeerVulkanModern *m = beer_vulkan_modern();
        if (m && m->vrs_tier >= 2) {
            s |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;
            a |= VK_ACCESS_2_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR;
        } else {
            s |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            a |= BD12_SHADER_READ;
        }
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
    bd12_set_cache_free(allocator);
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
    bd12_set_cache_clear(allocator);
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
    /* A new command buffer has no push constants: push the root arguments again. */
    bd12_cmd_invalidate_bindings(list);
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
    bd12_gpuprof_list_destroy(list);
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
    bd12_gpuprof_mark(list, (uint32_t)type, pipeline_hash);
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
    bd12_gpuprof_list_begin(list);
    {   /* D3D12 makes everything earlier work on the queue wrote visible to a command list (state decay and
         * promotion at ExecuteCommandLists boundaries). Every submission goes to one Vulkan queue, and a barrier's
         * first scope covers all earlier submissions on it, so this one barrier gives that ordering on the GPU
         * without chaining submissions through a semaphore wait. */
        const VkMemoryBarrier2 barrier = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT
        };
        const VkDependencyInfo dependency = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
                                              .pMemoryBarriers = &barrier };
        list->device->vk.CmdPipelineBarrier2(list->command_buffer, &dependency);
    }
    memset(list->heaps, 0, sizeof(list->heaps));
    list->compute_pipeline = NULL;
    memset(&list->compute, 0, sizeof(list->compute));
    memset(&list->graphics, 0, sizeof(list->graphics));
    memset(list->resolve_cache, 0, sizeof(list->resolve_cache));   /* its sets came from the reset allocator */
    memset(list->resolve_cache_next, 0, sizeof(list->resolve_cache_next));
    bd12_cmd_invalidate_bindings(list);
    list->barrier_batching = 0;
    list->batch_image_count = 0;
    memset(&list->batch_memory, 0, sizeof(list->batch_memory));
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
    bd12_gpuprof_list_end(list);
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

/* Barrier batching ----------------------------------------------------------------------------------------- */

void bd12_cmd_barrier_flush(BD12CmdList *list)
{
    const int memory = list->batch_memory.srcStageMask != 0;
    if (!memory && !list->batch_image_count) return;
    bd12_cmd_end_rendering(list);
    VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = memory ? 1u : 0u,
        .pMemoryBarriers = &list->batch_memory,
        .imageMemoryBarrierCount = list->batch_image_count,
        .pImageMemoryBarriers = list->batch_images
    };
    g_bd12_cmd_counts[BD12_COUNT_BARRIER]++;
    list->device->vk.CmdPipelineBarrier2(list->command_buffer, &dependency);
    list->batch_image_count = 0;
    memset(&list->batch_memory, 0, sizeof(list->batch_memory));
}

void bd12_cmd_barrier_batch_begin(BD12CmdList *list)
{
    list->barrier_batching = 1;
}

void bd12_cmd_barrier_batch_end(BD12CmdList *list)
{
    bd12_cmd_barrier_flush(list);
    list->barrier_batching = 0;
}

void bd12_cmd_add_image_barrier(BD12CmdList *list, const VkImageMemoryBarrier2 *barrier)
{
    if (list->barrier_batching) {
        /* Transitions of one image within a dependency are unordered: a second one for the same image starts a
         * new batch, which keeps D3D12's in-order semantics. */
        int split = list->batch_image_count == 32;
        for (uint32_t i = 0; i < list->batch_image_count && !split; ++i)
            split = list->batch_images[i].image == barrier->image;
        if (split) bd12_cmd_barrier_flush(list);
        list->batch_images[list->batch_image_count++] = *barrier;
        return;
    }
    bd12_cmd_end_rendering(list);
    VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = barrier
    };
    g_bd12_cmd_counts[BD12_COUNT_BARRIER]++;
    list->device->vk.CmdPipelineBarrier2(list->command_buffer, &dependency);
}

void bd12_cmd_add_memory_barrier(BD12CmdList *list, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                                 VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access)
{
    if (!src_stage) src_stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    VkMemoryBarrier2 *m = &list->batch_memory;
    if (!m->srcStageMask) m->sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    m->srcStageMask |= src_stage;
    m->srcAccessMask |= src_access;
    m->dstStageMask |= dst_stage;
    m->dstAccessMask |= dst_access;
    if (!list->barrier_batching) bd12_cmd_barrier_flush(list);
}

static void buffer_barrier(BD12CmdList *list, BD12Buffer *buffer,
                           VkPipelineStageFlags2 src_stage,
                           VkAccessFlags2 src_access,
                           VkPipelineStageFlags2 dst_stage,
                           VkAccessFlags2 dst_access)
{
    if (list->barrier_batching) {   /* buffers have no layout: one merged memory barrier covers them all */
        bd12_cmd_add_memory_barrier(list, src_stage, src_access, dst_stage, dst_access);
        return;
    }
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
    g_bd12_cmd_counts[BD12_COUNT_BARRIER]++;
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
    extern __thread uint32_t g_bd12_compute_root_push_bytes;   /* bd12_translate.c, root-signature pipelines */
    if (g_bd12_compute_root_push_bytes && set_layout) {
        pipeline->root_push_bytes = g_bd12_compute_root_push_bytes;
        set_layouts[1] = bd12_mirror_layout(device);
        if (!set_layouts[1]) goto fail;
        set_layout_count = 2;
        push_bytes = pipeline->root_push_bytes;
    } else if (bindless_words) {
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
    if (!(pipeline->layout = bd12_shared_pipeline_layout(device, &layout_info)))   /* shared: never destroyed */
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
        device->device, device->pipeline_cache, 1, &info, NULL, &pipeline->pipeline);
    if (result == VK_SUCCESS) bd12_pipeline_cache_note_new(device);
    device->vk.DestroyShaderModule(device->device, module, NULL);
    if (result != VK_SUCCESS) goto fail;
    return pipeline;

fail_module:
    device->vk.DestroyShaderModule(device->device, module, NULL);
fail:
    free(pipeline);
    return NULL;
}

/* A root-signature compute pipeline on the device's descriptor-buffer layout (bd12_heapdb.c). */
BD12ComputePipeline *bd12_compute_pipeline_build_db(BD12Device *device, const uint32_t *spirv, size_t spirv_bytes,
                                                    const BD12DbLayout *db_layout, uint32_t root_push_bytes,
                                                    uint64_t root_layout_hash)
{
    BD12ComputePipeline *pipeline = calloc(1, sizeof(*pipeline));
    if (!pipeline) return NULL;
    pipeline->device = device;
    pipeline->db_layout = db_layout;
    pipeline->layout = bd12_db_pipeline_layout(db_layout);
    pipeline->root_push_bytes = root_push_bytes;
    pipeline->root_layout_hash = root_layout_hash;
    VkShaderModuleCreateInfo module_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = spirv_bytes,
        .pCode = spirv
    };
    VkShaderModule module;
    if (device->vk.CreateShaderModule(device->device, &module_info, NULL, &module) != VK_SUCCESS) {
        free(pipeline);
        return NULL;
    }
    VkComputePipelineCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        /* DISPATCH_BASE: see bd12_compute_pipeline_build. */
        .flags = VK_PIPELINE_CREATE_DISPATCH_BASE_BIT | VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = module,
            .pName = "main"
        },
        .layout = pipeline->layout
    };
    VkResult result = device->vk.CreateComputePipelines(
        device->device, device->pipeline_cache, 1, &info, NULL, &pipeline->pipeline);
    if (result == VK_SUCCESS) bd12_pipeline_cache_note_new(device);
    device->vk.DestroyShaderModule(device->device, module, NULL);
    if (result != VK_SUCCESS) {
        free(pipeline);
        return NULL;
    }
    return pipeline;
}

void bd12_compute_pipeline_destroy(BD12ComputePipeline *pipeline)
{
    if (!pipeline) return;
    pipeline->device->vk.DestroyPipeline(pipeline->device->device,
                                         pipeline->pipeline, NULL);
    /* layout and set_layout are shared (bd12_shared_*_layout) or owned by the descriptor-buffer layout. */
    free(pipeline->bindings);
    free(pipeline->bindless);
    free(pipeline->binding_types);
    free(pipeline);
}

void bd12_cmd_dispatch(BD12CmdList *list, BD12ComputePipeline *pipeline,
                       const void *push, uint32_t x, uint32_t y, uint32_t z)
{
    BD12Device *device = list->device;
    bd12_cmd_invalidate_point(list, 1);   /* an internal pipeline replaces the title's compute bindings */
    bd12_cmd_push_disturbed(list);        /* and its push constants replace every bind point's */
    device->vk.CmdBindPipeline(list->command_buffer,
                               VK_PIPELINE_BIND_POINT_COMPUTE,
                               pipeline->pipeline);
    if (pipeline->push_bytes && push)
        device->vk.CmdPushConstants(list->command_buffer, pipeline->layout,
                                    VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                    pipeline->push_bytes, push);
    device->vk.CmdDispatch(list->command_buffer, x, y, z);
}
