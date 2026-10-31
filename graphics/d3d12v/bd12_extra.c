#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#define _GNU_SOURCE
#include "bd12.h"


#include <string.h>

/* Clears, resolves and small updates that D3D12 treats as ordinary commands.
 * Each brackets its transfer with a global memory dependency, because a D3D12
 * UAV clear/resolve is ordered against shader accesses by the application's
 * UAV barriers, which carry no Vulkan stage information on their own. */

static void global_barrier(BD12CmdList *list, VkPipelineStageFlags2 src_stage,
                           VkAccessFlags2 src_access,
                           VkPipelineStageFlags2 dst_stage,
                           VkAccessFlags2 dst_access)
{
    VkMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access
    };
    VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier
    };
    g_bd12_cmd_counts[BD12_COUNT_BARRIER]++;
    list->device->vk.CmdPipelineBarrier2(list->command_buffer, &dependency);
}

static const VkAccessFlags2 k_all_access =
    VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;

/* Diagnostic: BEER_BD12_SYNC_ALL=1 serializes every draw/dispatch so a missing
 * dependency can be told apart from a logic fault. */
/* BEER_BD12_TRACE=<n> prints every dispatch and indirect draw of present #n. */
static int g_trace_target = -2;
static int g_trace_frame;
static int g_trace_manual;
void bd12_trace_frame_advance(void)
{
    if (g_trace_target == -2) {
        const char *v = getenv("BEER_BD12_TRACE");
        g_trace_target = v ? atoi(v) : -1;
        g_trace_manual = !v;
    }
    ++g_trace_frame;
    bd12_cbt_frame_advance();
    if (g_trace_target >= 0 && g_trace_frame >= g_trace_target - 1 && g_trace_frame <= g_trace_target + 2) fprintf(stderr, "[BD12 TRACE] advance frame=%d target=%d\n", g_trace_frame, g_trace_target);
    /* Manual trigger: creating the BEER_BD12_TRACE_FILE path traces the next
     * present, as often as it is created. */
    if ((g_trace_target == -1 || (g_trace_target >= 0 && g_trace_manual &&
                                  g_trace_frame > g_trace_target)) &&
        getenv("BEER_BD12_TRACE_FILE") &&
        access(getenv("BEER_BD12_TRACE_FILE"), F_OK) == 0) {
        unlink(getenv("BEER_BD12_TRACE_FILE"));
        g_trace_target = g_trace_frame + 1;
    }
}
int bd12_trace_active(void)
{
    return g_trace_target >= 0 && g_trace_frame == g_trace_target;
}

int bd12_debug_sync_all(void)
{
    static int value = -1;
    if (value < 0) value = getenv("BEER_BD12_SYNC_ALL") != NULL;
    return value;
}

void bd12_cmd_global_barrier(BD12CmdList *list)
{
    bd12_cmd_end_rendering(list);
    global_barrier(list, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, k_all_access,
                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, k_all_access);
}

void bd12_cmd_clear_uav_buffer(BD12CmdList *list, BD12Buffer *buffer,
                               VkDeviceSize offset, VkDeviceSize size,
                               uint32_t value)
{
    if (!buffer || offset >= buffer->size) return;
    bd12_cmd_end_rendering(list);
    if (size == VK_WHOLE_SIZE || offset + size > buffer->size)
        size = buffer->size - offset;
    size &= ~(VkDeviceSize)3;
    if (!size) return;
    global_barrier(list, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, k_all_access,
                   VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT);
    bd12_cmd_record_flight_op(list, BD12_FLIGHT_CLEAR, 0, size, 0, 0, 0);
    list->device->vk.CmdFillBuffer(list->command_buffer, buffer->buffer,
                                   offset, size, value);
    global_barrier(list, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, k_all_access);
}

void bd12_cmd_clear_uav_image(BD12CmdList *list, const BD12Descriptor *uav,
                              const VkClearColorValue *color)
{
    if (!uav || uav->kind != BD12_DESC_UAV_IMAGE || !uav->image) return;
    bd12_cmd_end_rendering(list);
    bd12_image_ensure_initialized(list, uav->image);
    const VkImageSubresourceRange range = {
        VK_IMAGE_ASPECT_COLOR_BIT, uav->view_mip, 1, uav->view_base_layer,
        uav->view_layers ? uav->view_layers : VK_REMAINING_ARRAY_LAYERS
    };
    global_barrier(list, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, k_all_access,
                   VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT);
    bd12_cmd_record_flight_op(list, BD12_FLIGHT_CLEAR, 0,
        (uint64_t)uav->width * uav->height * uav->layer_count,
        uav->width, uav->height, uav->layer_count);
    list->device->vk.CmdClearColorImage(list->command_buffer, uav->image->image,
                                        VK_IMAGE_LAYOUT_GENERAL, color, 1,
                                        &range);
    global_barrier(list, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, k_all_access);
}

void bd12_cmd_resolve_image(BD12CmdList *list, BD12Image *dst,
                            uint32_t dst_subresource, BD12Image *src,
                            uint32_t src_subresource)
{
    bd12_cmd_end_rendering(list);
    bd12_image_ensure_initialized(list, dst);
    bd12_image_ensure_initialized(list, src);
    const uint32_t dst_mips = dst->desc.mip_levels ? dst->desc.mip_levels : 1;
    const uint32_t src_mips = src->desc.mip_levels ? src->desc.mip_levels : 1;
    bd12_cmd_record_flight_op(list, BD12_FLIGHT_RESOLVE, 0,
        (uint64_t)src->desc.width * src->desc.height,
        src->desc.width, src->desc.height, 1);
    VkImageResolve region = {
        .srcSubresource = { src->aspect & VK_IMAGE_ASPECT_COLOR_BIT ?
                                VK_IMAGE_ASPECT_COLOR_BIT : src->aspect,
                            src_subresource % src_mips,
                            src_subresource / src_mips, 1 },
        .dstSubresource = { dst->aspect & VK_IMAGE_ASPECT_COLOR_BIT ?
                                VK_IMAGE_ASPECT_COLOR_BIT : dst->aspect,
                            dst_subresource % dst_mips,
                            dst_subresource / dst_mips, 1 },
        .extent = { src->desc.width >> (src_subresource % src_mips) ?
                        src->desc.width >> (src_subresource % src_mips) : 1,
                    src->desc.height >> (src_subresource % src_mips) ?
                        src->desc.height >> (src_subresource % src_mips) : 1,
                    1 }
    };
    list->device->vk.CmdResolveImage(list->command_buffer, src->image,
                                     VK_IMAGE_LAYOUT_GENERAL,
                                     dst->image,
                                     VK_IMAGE_LAYOUT_GENERAL, 1,
                                     &region);
}

void bd12_cmd_update_buffer(BD12CmdList *list, BD12Buffer *dst,
                            VkDeviceSize offset, VkDeviceSize size,
                            const void *data)
{
    if (!dst || !size || (size & 3) || (offset & 3) || size > 65536) return;
    bd12_cmd_end_rendering(list);
    global_barrier(list, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, k_all_access,
                   VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT);
    list->device->vk.CmdUpdateBuffer(list->command_buffer, dst->buffer, offset,
                                     size, data);
    global_barrier(list, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, k_all_access);
}

/* Queries -------------------------------------------------------------------- */

/* Reset, begin and end are recorded outside a rendering scope: Vulkan requires
 * vkCmdResetQueryPool and vkCmdCopyQueryPoolResults outside one, and an
 * occlusion query begun outside may stay active across several scopes. */
void bd12_cmd_begin_query(BD12CmdList *list, VkQueryPool pool, uint32_t index)
{
    if (!pool) return;
    bd12_cmd_end_rendering(list);
    list->device->vk.CmdResetQueryPool(list->command_buffer, pool, index, 1);
    list->device->vk.CmdBeginQuery(list->command_buffer, pool, index, 0);
}

void bd12_cmd_end_query(BD12CmdList *list, VkQueryPool pool, uint32_t index)
{
    if (!pool) return;
    bd12_cmd_end_rendering(list);
    list->device->vk.CmdEndQuery(list->command_buffer, pool, index);
}

void bd12_cmd_write_timestamp(BD12CmdList *list, VkQueryPool pool, uint32_t index)
{
    if (!pool) return;
    bd12_cmd_end_rendering(list);
    list->device->vk.CmdResetQueryPool(list->command_buffer, pool, index, 1);
    list->device->vk.CmdWriteTimestamp2(list->command_buffer,
                                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        pool, index);
}

BD12Buffer *g_ts_dst; VkDeviceSize g_ts_offset; uint32_t g_ts_count;
void bd12_cmd_copy_query_results(BD12CmdList *list, VkQueryPool pool,
                                 uint32_t first, uint32_t count,
                                 BD12Buffer *dst, VkDeviceSize offset)
{
    if (!pool || !count || !dst) return;
    g_ts_dst = dst; g_ts_offset = offset; g_ts_count = count;
    bd12_cmd_end_rendering(list);
    list->device->vk.CmdCopyQueryPoolResults(
        list->command_buffer, pool, first, count, dst->buffer, offset, 8,
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
}
