#define _GNU_SOURCE
#include "bd12.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

/* D3D12 sampler-feedback resolves: opaque R64 feedback maps <-> the R8_UINT MinMip / MipRegionUsed layouts. The
 * transcode runs on buffers (bd12_feedback.c has no descriptors): the map and the R8 data are copied into scratch
 * memory, a push-constant compute shader (shaders/sampler_feedback.comp) converts between them by device address,
 * and the result is copied to its destination. Feedback maps are small (one texel per mip region), and resolves are
 * rare, so the copies cost little and the destination needs no storage-image usage. */

#include "bd12_sampler_feedback_spv.h"

typedef struct {
    uint64_t map, bytes;
    uint32_t src_pitch, src_rows, out_w, out_h, mips, mip, x0, y0, layer_bytes;
} FeedbackArgs;

static BD12ComputePipeline *feedback_pipeline(BD12Device *device, BD12FeedbackVariant variant)
{
    static BD12ComputePipeline *pipelines[4];
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static const struct { const uint32_t *spv; size_t size; } code[4] = {
        { k_sf_decode_min_mip_spv, sizeof(k_sf_decode_min_mip_spv) },
        { k_sf_decode_mip_used_spv, sizeof(k_sf_decode_mip_used_spv) },
        { k_sf_encode_min_mip_spv, sizeof(k_sf_encode_min_mip_spv) },
        { k_sf_encode_mip_used_spv, sizeof(k_sf_encode_mip_used_spv) },
    };
    if ((unsigned)variant >= 4) return NULL;
    pthread_mutex_lock(&lock);
    if (!pipelines[variant])
        pipelines[variant] = bd12_compute_pipeline_create(device, code[variant].spv, code[variant].size,
                                                          (uint32_t)sizeof(FeedbackArgs));
    BD12ComputePipeline *p = pipelines[variant];
    pthread_mutex_unlock(&lock);
    return p;
}

static void barrier(BD12CmdList *list, VkPipelineStageFlags2 src, VkAccessFlags2 src_access,
                    VkPipelineStageFlags2 dst, VkAccessFlags2 dst_access)
{
    bd12_cmd_add_memory_barrier(list, src, src_access, dst, dst_access);
}

int bd12_cmd_feedback_transcode(BD12CmdList *list, const BD12FeedbackOp *op)
{
    BD12Device *device = list->device;
    const int encode = op->variant == BD12_FEEDBACK_ENCODE_MIN_MIP || op->variant == BD12_FEEDBACK_ENCODE_MIP_USED;
    BD12ComputePipeline *pipeline = feedback_pipeline(device, op->variant);
    if (!pipeline || !op->map || !op->w || !op->h || !op->layers) return 0;
    if (!op->r8_image && !op->r8_buffer) return 0;

    const VkDeviceSize map_layer = (VkDeviceSize)op->map_pitch * op->map_rows * 8u;
    const uint32_t layer_bytes = op->w * op->h;
    const VkDeviceSize bytes_size = ((VkDeviceSize)layer_bytes * op->layers + 3u) & ~(VkDeviceSize)3u;
    BD12Buffer *map_buf, *bytes_buf;
    VkDeviceSize map_at, bytes_at;
    void *unused;
    if (!bd12_scratch_alloc(list->allocator, map_layer * op->layers, &map_buf, &map_at, &unused) ||
        !bd12_scratch_alloc(list->allocator, bytes_size, &bytes_buf, &bytes_at, &unused))
        return 0;

    bd12_cmd_end_rendering(list);
    /* The application's barrier moved the resources into RESOLVE_SOURCE / RESOLVE_DEST (resolve stage); the copies
     * and the dispatch below run in other stages, so order them against all earlier work here. */
    barrier(list, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);

    /* Inputs into scratch; the output scratch is zeroed (the shader ORs into it). */
    if (encode) {
        device->vk.CmdFillBuffer(list->command_buffer, map_buf->buffer, map_at, map_layer * op->layers, 0);
        for (uint32_t i = 0; i < op->layers; ++i) {
            if (op->r8_image) {
                const BD12ImagePoint from = { op->r8_mip + (op->r8_layer + i) * op->r8_mips, op->r8_x, op->r8_y, 0 };
                bd12_cmd_copy_image_to_buffer(list, bytes_buf, bytes_at + (VkDeviceSize)i * layer_bytes, op->w,
                                              op->r8_image, from, op->w, op->h, 1);
            } else {
                const VkBufferCopy region = { op->r8_offset + (VkDeviceSize)i * layer_bytes,
                                              bytes_at + (VkDeviceSize)i * layer_bytes, layer_bytes };
                device->vk.CmdCopyBuffer(list->command_buffer, op->r8_buffer->buffer, bytes_buf->buffer, 1, &region);
            }
        }
    } else {
        device->vk.CmdFillBuffer(list->command_buffer, bytes_buf->buffer, bytes_at, bytes_size, 0);
        for (uint32_t i = 0; i < op->layers; ++i) {
            const BD12ImagePoint from = { op->map_layer + i, 0, 0, 0 };
            bd12_cmd_copy_image_to_buffer(list, map_buf, map_at + map_layer * i, op->map_pitch * 8u, op->map, from,
                                          op->map_pitch, op->map_rows, 1);
        }
    }
    barrier(list, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);

    const FeedbackArgs args = { map_buf->va + map_at, bytes_buf->va + bytes_at, op->map_pitch, op->map_rows, op->w, op->h,
                                op->mips, op->mip, op->x0, op->y0, layer_bytes };
    bd12_cmd_dispatch(list, pipeline, &args, (op->w + 7u) / 8u, (op->h + 7u) / 8u, op->layers);
    barrier(list, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

    /* Results to their destination. */
    for (uint32_t i = 0; i < op->layers; ++i) {
        if (encode) {
            const BD12ImagePoint at = { op->map_layer + i, 0, 0, 0 };
            bd12_cmd_copy_buffer_to_image(list, op->map, at, map_buf, map_at + map_layer * i, op->map_pitch * 8u,
                                          op->map_pitch, op->map_rows, 1);
        } else if (op->r8_image) {
            const BD12ImagePoint at = { op->r8_mip + (op->r8_layer + i) * op->r8_mips, op->r8_x, op->r8_y, 0 };
            bd12_cmd_copy_buffer_to_image(list, op->r8_image, at, bytes_buf, bytes_at + (VkDeviceSize)i * layer_bytes,
                                          op->w, op->w, op->h, 1);
        } else {
            const VkBufferCopy region = { bytes_at + (VkDeviceSize)i * layer_bytes,
                                          op->r8_offset + (VkDeviceSize)i * layer_bytes, layer_bytes };
            device->vk.CmdCopyBuffer(list->command_buffer, bytes_buf->buffer, op->r8_buffer->buffer, 1, &region);
        }
    }
    barrier(list, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    return 1;
}
