#define _GNU_SOURCE
#include "bd12.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Formats ------------------------------------------------------------------- */

static VkImageAspectFlags format_aspect(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D32_SFLOAT:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_S8_UINT:
        return VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default:
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

void bd12_format_block(VkFormat f, VkImageAspectFlags aspect,
                       uint32_t *block_width, uint32_t *block_height,
                       uint32_t *block_bytes)
{
    uint32_t w = 1, h = 1, bytes = 4;
    if (aspect == VK_IMAGE_ASPECT_STENCIL_BIT) {
        bytes = 1;
    } else if (aspect == VK_IMAGE_ASPECT_DEPTH_BIT) {
        bytes = (f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D16_UNORM_S8_UINT) ?
                2 : 4;
    } else if (f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK &&
               f <= VK_FORMAT_BC7_SRGB_BLOCK) {
        w = h = 4;
        bytes = (f <= VK_FORMAT_BC1_RGBA_SRGB_BLOCK ||
                 f == VK_FORMAT_BC4_UNORM_BLOCK ||
                 f == VK_FORMAT_BC4_SNORM_BLOCK) ? 8 : 16;
    } else {
        switch (f) {
        case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM:
        case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT:
        case VK_FORMAT_S8_UINT:
            bytes = 1; break;
        case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM:
        case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT:
        case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SNORM:
        case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT:
        case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_D16_UNORM:
            bytes = 2; break;
        case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SNORM:
        case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R16G16B16A16_SINT:
        case VK_FORMAT_R16G16B16A16_SFLOAT:
        case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT:
        case VK_FORMAT_R32G32_SFLOAT:
            bytes = 8; break;
        case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32_SINT:
        case VK_FORMAT_R32G32B32_SFLOAT:
            bytes = 12; break;
        case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT:
        case VK_FORMAT_R32G32B32A32_SFLOAT:
            bytes = 16; break;
        default:
            bytes = 4; break;
        }
    }
    if (block_width) *block_width = w;
    if (block_height) *block_height = h;
    if (block_bytes) *block_bytes = bytes;
}

/* States -> layouts ------------------------------------------------------------ */

VkImageLayout bd12_layout_for_state(uint32_t state, int ds)
{
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
#define MERGE(l)                                                             \
    do {                                                                     \
        if (layout == VK_IMAGE_LAYOUT_UNDEFINED) layout = (l);               \
        else if (layout != (l)) return VK_IMAGE_LAYOUT_GENERAL;              \
    } while (0)
    if (state & BD12_STATE_RENDER_TARGET)
        MERGE(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (state & BD12_STATE_UNORDERED_ACCESS) MERGE(VK_IMAGE_LAYOUT_GENERAL);
    if (state & BD12_STATE_DEPTH_WRITE)
        MERGE(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    if (state & BD12_STATE_DEPTH_READ)
        MERGE(VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    if (state & (BD12_STATE_NON_PIXEL_SHADER_RESOURCE |
                 BD12_STATE_PIXEL_SHADER_RESOURCE))
        MERGE(ds ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL :
                   VK_IMAGE_LAYOUT_GENERAL);
    if (state & (BD12_STATE_COPY_DEST | BD12_STATE_RESOLVE_DEST))
        MERGE(VK_IMAGE_LAYOUT_GENERAL);
    if (state & (BD12_STATE_COPY_SOURCE | BD12_STATE_RESOLVE_SOURCE))
        MERGE(VK_IMAGE_LAYOUT_GENERAL);
#undef MERGE
    return layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_GENERAL : layout;
}

/* Creation ----------------------------------------------------------------------- */

static VkImageUsageFlags image_usage_for(const BD12ImageDesc *desc)
{
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (desc->allow_render_target) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (desc->allow_depth_stencil)
        usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (desc->allow_uav) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    return usage;
}

static VkImageCreateInfo image_create_info(const BD12ImageDesc *desc,
                                           VkImageUsageFlags usage)
{
    const int is_3d = desc->type == VK_IMAGE_TYPE_3D;
    VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
                 (desc->cube_compatible ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0) |
                 (is_3d && (desc->allow_render_target || desc->allow_uav) ?
                      VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT : 0),
        .imageType = desc->type,
        .format = desc->format,
        .extent = { desc->width, desc->height ? desc->height : 1,
                    is_3d ? desc->depth_or_layers : 1 },
        .mipLevels = desc->mip_levels ? desc->mip_levels : 1,
        .arrayLayers = is_3d ? 1 : (desc->depth_or_layers ? desc->depth_or_layers : 1),
        .samples = desc->samples ? (VkSampleCountFlagBits)desc->samples :
                   VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };
    return info;
}

int bd12_image_requirements(BD12Device *device, const BD12ImageDesc *desc,
                            VkDeviceSize *size, VkDeviceSize *alignment)
{
    VkImageCreateInfo info = image_create_info(desc, image_usage_for(desc));
    VkImage image;
    if (device->vk.CreateImage(device->device, &info, NULL, &image) !=
        VK_SUCCESS)
        return 0;
    VkMemoryRequirements requirements;
    device->vk.GetImageMemoryRequirements(device->device, image, &requirements);
    device->vk.DestroyImage(device->device, image, NULL);
    *size = requirements.size;
    *alignment = requirements.alignment;
    return 1;
}

static BD12Image *image_create(BD12Device *device, BD12Heap *heap,
                               VkDeviceSize offset, const BD12ImageDesc *desc,
                               uint32_t initial_state)
{
    if (!device || !desc || (heap && heap->type != BD12_HEAP_DEFAULT))
        return NULL;
    BD12Image *image = calloc(1, sizeof(*image));
    if (!image) return NULL;
    image->device = device;
    image->desc = *desc;
    image->initial_state = initial_state;
    image->aspect = format_aspect(desc->format);
    image->usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                   VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (desc->allow_render_target)
        image->usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (desc->allow_depth_stencil)
        image->usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (desc->allow_uav) image->usage |= VK_IMAGE_USAGE_STORAGE_BIT;

    VkImageCreateInfo info = image_create_info(desc, image->usage);
    const int is_3d = desc->type == VK_IMAGE_TYPE_3D;
    (void)is_3d;
    image->desc.mip_levels = info.mipLevels;
    image->desc.depth_or_layers = is_3d ? info.extent.depth : info.arrayLayers;
    VkResult result = device->vk.CreateImage(device->device, &info, NULL,
                                             &image->image);
    if (result != VK_SUCCESS) {
        beer_vulkan_record_result(result, "BD12 image create");
        free(image);
        return NULL;
    }
    VkMemoryRequirements requirements;
    device->vk.GetImageMemoryRequirements(device->device, image->image,
                                          &requirements);
    if (!(requirements.memoryTypeBits &
          (1u << device->memory_type[BD12_HEAP_DEFAULT])))
        goto fail;
    if (heap) {
        if ((offset % requirements.alignment) ||
            offset + requirements.size > heap->size)
            goto fail;
    } else {
        VkDeviceSize size = (requirements.size + 65535) & ~(VkDeviceSize)65535;
        /* D3D12 zero-initializes committed resources. Games lean on it (accumulation
         * buffers, partially redrawn atlases), and recycled device memory otherwise shows
         * stale pixels from earlier resources. BEER_BD12_NO_IMAGE_ZERO=1 skips the fill. */
        heap = getenv("BEER_BD12_NO_IMAGE_ZERO") ?
               bd12_heap_create_unzeroed(device, BD12_HEAP_DEFAULT, size) :
               bd12_heap_create(device, BD12_HEAP_DEFAULT, size);
        if (!heap) goto fail;
        image->heap = heap;
        image->owns_heap = 1;
        offset = 0;
    }
    image->heap = heap;
    image->heap_offset = offset;
    if (device->vk.BindImageMemory(device->device, image->image, heap->memory,
                                   offset) != VK_SUCCESS)
        goto fail;
    image->serial = bd12_live_serial();
    bd12_live_add(image);
    return image;

fail:
    if (image->owns_heap) bd12_heap_destroy(image->heap);
    device->vk.DestroyImage(device->device, image->image, NULL);
    free(image);
    return NULL;
}

BD12Image *bd12_image_create_placed(BD12Heap *heap, VkDeviceSize offset,
                                    const BD12ImageDesc *desc,
                                    uint32_t initial_state)
{
    return heap ? image_create(heap->device, heap, offset, desc, initial_state)
                : NULL;
}

BD12Image *bd12_image_create_committed(BD12Device *device,
                                       const BD12ImageDesc *desc,
                                       uint32_t initial_state)
{
    return image_create(device, NULL, 0, desc, initial_state);
}

void bd12_image_destroy(BD12Image *image)
{
    if (!image) return;
    BD12Device *device = image->device;
    bd12_live_remove(image);
    bd12_mirror_forget_image(image);
    for (uint32_t i = 0; i < image->view_count; ++i)
        device->vk.DestroyImageView(device->device, image->views[i].view, NULL);
    free(image->views);
    device->vk.DestroyImage(device->device, image->image, NULL);
    if (image->owns_heap) bd12_heap_destroy(image->heap);
    free(image);
}

/* Views -------------------------------------------------------------------------- */

VkImageView bd12_image_view(BD12Image *image, VkImageViewType type,
                            VkFormat format, VkImageAspectFlags aspect,
                            uint32_t base_mip, uint32_t mip_count,
                            uint32_t base_layer, uint32_t layer_count)
{
    BD12Device *device = image->device;
    const uint32_t mips = image->desc.mip_levels;
    const uint32_t layers = image->desc.type == VK_IMAGE_TYPE_3D ? 1 :
                            image->desc.depth_or_layers;
    if (format == VK_FORMAT_UNDEFINED) format = image->desc.format;
    /* D3D12 depth SRVs use colour-typed formats (R32_FLOAT, R16_UNORM, ...);
     * Vulkan requires the image's own depth format with a depth aspect. A
     * colour-format view of a depth image is invalid and makes NVIDIA read
     * the compressed depth surface incorrectly. */
    if (image->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
        format = image->desc.format;
    if (!aspect) {
        aspect = (image->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ?
                 VK_IMAGE_ASPECT_DEPTH_BIT : image->aspect;
    }
    if (base_mip >= mips || base_layer >= layers) return VK_NULL_HANDLE;
    if (mip_count == UINT32_MAX || base_mip + mip_count > mips)
        mip_count = mips - base_mip;
    if (layer_count == UINT32_MAX || base_layer + layer_count > layers)
        layer_count = layers - base_layer;

    VkImageView found = VK_NULL_HANDLE;
    pthread_mutex_lock(&device->view_lock);
    for (uint32_t i = 0; i < image->view_count; ++i) {
        const BD12ImageViewEntry *v = &image->views[i];
        if (v->type == type && v->format == format && v->aspect == aspect &&
            v->base_mip == base_mip && v->mip_count == mip_count &&
            v->base_layer == base_layer && v->layer_count == layer_count) {
            found = v->view;
            break;
        }
    }
    if (!found) {
        VkImageViewCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = image->image,
            .viewType = type,
            .format = format,
            .subresourceRange = { aspect, base_mip, mip_count, base_layer,
                                  layer_count }
        };
        if (device->vk.CreateImageView(device->device, &info, NULL, &found) ==
            VK_SUCCESS) {
            if (image->view_count == image->view_capacity) {
                uint32_t capacity = image->view_capacity ?
                    image->view_capacity * 2 : 4;
                BD12ImageViewEntry *grown = realloc(image->views,
                                                    capacity * sizeof(*grown));
                if (!grown) {
                    device->vk.DestroyImageView(device->device, found, NULL);
                    found = VK_NULL_HANDLE;
                } else {
                    image->views = grown;
                    image->view_capacity = capacity;
                }
            }
            if (found)
                image->views[image->view_count++] = (BD12ImageViewEntry){
                    type, format, aspect, base_mip, mip_count, base_layer,
                    layer_count, found };
        } else {
            found = VK_NULL_HANDLE;
        }
    }
    pthread_mutex_unlock(&device->view_lock);
    return found;
}

/* Barriers ---------------------------------------------------------------------- */

static int image_is_ds(const BD12Image *image)
{
    return (image->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT |
                             VK_IMAGE_ASPECT_STENCIL_BIT)) != 0;
}

static void image_barrier(BD12CmdList *list, BD12Image *image,
                          const VkImageSubresourceRange *range,
                          VkPipelineStageFlags2 src_stage,
                          VkAccessFlags2 src_access, VkImageLayout old_layout,
                          VkPipelineStageFlags2 dst_stage,
                          VkAccessFlags2 dst_access, VkImageLayout new_layout)
{
    bd12_cmd_end_rendering(list);
    VkImageMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image->image,
        .subresourceRange = *range
    };
    VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier
    };
    list->device->vk.CmdPipelineBarrier2(list->command_buffer, &dependency);
}

static VkImageSubresourceRange whole_range(const BD12Image *image)
{
    return (VkImageSubresourceRange){ image->aspect, 0, VK_REMAINING_MIP_LEVELS,
                                      0, VK_REMAINING_ARRAY_LAYERS };
}

/* First command-list use: UNDEFINED -> layout of the creation state. */
void bd12_image_ensure_initialized(BD12CmdList *list, BD12Image *image)
{
    if (__atomic_load_n(&image->initialized, __ATOMIC_ACQUIRE)) return;   /* lock-free fast path */
    BD12Device *device = image->device;
    pthread_mutex_lock(&device->view_lock);
    const int done = image->initialized;
    __atomic_store_n(&image->initialized, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&device->view_lock);
    if (done) return;
    VkPipelineStageFlags2 stage;
    VkAccessFlags2 access;
    bd12_state_sync(image->initial_state, 0, &stage, &access);
    const VkImageSubresourceRange range = whole_range(image);
    image_barrier(list, image, &range, VK_PIPELINE_STAGE_2_NONE, 0,
                  VK_IMAGE_LAYOUT_UNDEFINED, stage, access,
                  bd12_layout_for_state(image->initial_state,
                                        image_is_ds(image)));
}

typedef struct {
    VkImageAspectFlags aspect;
    uint32_t mip, layer;
} SubresourceInfo;

static SubresourceInfo decode_subresource(const BD12Image *image,
                                          uint32_t subresource)
{
    const uint32_t mips = image->desc.mip_levels;
    const uint32_t layers = image->desc.type == VK_IMAGE_TYPE_3D ? 1 :
                            image->desc.depth_or_layers;
    const uint32_t plane = subresource / (mips * layers);
    const uint32_t rest = subresource % (mips * layers);
    SubresourceInfo info = { image->aspect, rest % mips, rest / mips };
    if (image->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
        info.aspect = plane ? VK_IMAGE_ASPECT_STENCIL_BIT :
                              VK_IMAGE_ASPECT_DEPTH_BIT;
    return info;
}

void bd12_cmd_transition_image(BD12CmdList *list, BD12Image *image,
                               uint32_t subresource, uint32_t before,
                               uint32_t after)
{
    bd12_image_ensure_initialized(list, image);
    if (before == after) return;
    VkPipelineStageFlags2 src_stage, dst_stage;
    VkAccessFlags2 src_access, dst_access;
    bd12_state_sync(before, 1, &src_stage, &src_access);
    bd12_state_sync(after, 0, &dst_stage, &dst_access);
    VkImageSubresourceRange range = whole_range(image);
    if (subresource != BD12_ALL_SUBRESOURCES) {
        const SubresourceInfo s = decode_subresource(image, subresource);
        range = (VkImageSubresourceRange){ s.aspect, s.mip, 1, s.layer, 1 };
    }
    const int ds = image_is_ds(image);
    image_barrier(list, image, &range, src_stage, src_access,
                  bd12_layout_for_state(before, ds), dst_stage, dst_access,
                  bd12_layout_for_state(after, ds));
}

void bd12_cmd_uav_barrier_image(BD12CmdList *list, BD12Image *image)
{
    bd12_image_ensure_initialized(list, image);
    VkPipelineStageFlags2 stage;
    VkAccessFlags2 access;
    bd12_state_sync(BD12_STATE_UNORDERED_ACCESS, 0, &stage, &access);
    const VkImageSubresourceRange range = whole_range(image);
    image_barrier(list, image, &range, stage,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_IMAGE_LAYOUT_GENERAL, stage, access,
                  VK_IMAGE_LAYOUT_GENERAL);
}

/* Copies --------------------------------------------------------------------------- */

static VkImageSubresourceLayers copy_layers(const BD12Image *image,
                                            uint32_t subresource)
{
    const SubresourceInfo s = decode_subresource(image, subresource);
    return (VkImageSubresourceLayers){ s.aspect, s.mip, s.layer, 1 };
}

static void copy_geometry(const BD12Image *image, uint32_t depth,
                          uint32_t *out_depth, uint32_t *layer_count)
{
    (void)layer_count;
    *out_depth = image->desc.type == VK_IMAGE_TYPE_3D ? depth : 1;
}

static VkBufferImageCopy buffer_image_region(const BD12Image *image,
                                             BD12ImagePoint at,
                                             VkDeviceSize buffer_offset,
                                             uint32_t row_pitch, uint32_t width,
                                             uint32_t height, uint32_t depth)
{
    VkBufferImageCopy region = { .bufferOffset = buffer_offset };
    region.imageSubresource = copy_layers(image, at.subresource);
    uint32_t block_w, block_bytes;
    bd12_format_block(image->desc.format, region.imageSubresource.aspectMask,
                      &block_w, NULL, &block_bytes);
    region.bufferRowLength = row_pitch / block_bytes * block_w;
    region.bufferImageHeight = 0;
    region.imageOffset = (VkOffset3D){ at.x, at.y, at.z };
    uint32_t d, unused;
    copy_geometry(image, depth, &d, &unused);
    region.imageExtent = (VkExtent3D){ width, height, d };
    return region;
}

void bd12_cmd_copy_buffer_to_image(BD12CmdList *list, BD12Image *dst,
                                   BD12ImagePoint at, BD12Buffer *src,
                                   VkDeviceSize src_offset, uint32_t row_pitch,
                                   uint32_t width, uint32_t height,
                                   uint32_t depth)
{
    bd12_cmd_end_rendering(list);
    bd12_image_ensure_initialized(list, dst);
    if (getenv("BEER_BD12_RES_LOG") && width >= 480)
        fprintf(stderr, "[BD12 RES] copy buffer->image %ux%u fmt=%d pitch=%u dst-subres=%u\n", width, height, (int)dst->desc.format, row_pitch, at.subresource);
    const VkBufferImageCopy region = buffer_image_region(
        dst, at, src_offset, row_pitch, width, height, depth);
    bd12_cmd_record_flight_op(list, BD12_FLIGHT_COPY_BUFFER_IMAGE, 0,
        (uint64_t)width * height * depth, width, height, depth);
    list->device->vk.CmdCopyBufferToImage(
        list->command_buffer, src->buffer, dst->image,
        VK_IMAGE_LAYOUT_GENERAL, 1, &region);
}

void bd12_cmd_copy_image_to_buffer(BD12CmdList *list, BD12Buffer *dst,
                                   VkDeviceSize dst_offset, uint32_t row_pitch,
                                   BD12Image *src, BD12ImagePoint from,
                                   uint32_t width, uint32_t height,
                                   uint32_t depth)
{
    if (dst->heap && dst->heap->type == BD12_HEAP_READBACK && getenv("BEER_BD12_RB_LOG"))
        fprintf(stderr, "[RB] image->readback %ux%ux%u fmt=%d\n", width, height, depth,
                (int)src->desc.format);
    bd12_cmd_end_rendering(list);
    bd12_image_ensure_initialized(list, src);
    const VkBufferImageCopy region = buffer_image_region(
        src, from, dst_offset, row_pitch, width, height, depth);
    bd12_cmd_record_flight_op(list, BD12_FLIGHT_COPY_IMAGE_BUFFER, 0,
        (uint64_t)width * height * depth, width, height, depth);
    list->device->vk.CmdCopyImageToBuffer(
        list->command_buffer, src->image, VK_IMAGE_LAYOUT_GENERAL,
        dst->buffer, 1, &region);
}

void bd12_cmd_copy_image(BD12CmdList *list, BD12Image *dst, BD12ImagePoint at,
                         BD12Image *src, BD12ImagePoint from, uint32_t width,
                         uint32_t height, uint32_t depth)
{
    bd12_cmd_end_rendering(list);
    bd12_image_ensure_initialized(list, dst);
    bd12_image_ensure_initialized(list, src);
    uint32_t d, unused;
    copy_geometry(src, depth, &d, &unused);
    if (bd12_trace_active() && getenv("BEER_BD12_COPY_LOG"))
        fprintf(stderr, "[COPYIMG] dst %ux%u vk%d samples=%u sub=%u at=%d,%d,%d <- src %ux%u vk%d samples=%u sub=%u from=%d,%d,%d size=%ux%ux%u\n",
                dst->desc.width, dst->desc.height, (int)dst->desc.format, dst->desc.samples,
                at.subresource, at.x, at.y, at.z,
                src->desc.width, src->desc.height, (int)src->desc.format, src->desc.samples,
                from.subresource, from.x, from.y, from.z, width, height, depth);
    bd12_cmd_record_flight_op(list, BD12_FLIGHT_COPY_IMAGE, 0,
        (uint64_t)width * height * d, width, height, d);
    VkImageCopy region = {
        .srcSubresource = copy_layers(src, from.subresource),
        .srcOffset = { from.x, from.y, from.z },
        .dstSubresource = copy_layers(dst, at.subresource),
        .dstOffset = { at.x, at.y, at.z },
        .extent = { width, height, d }
    };
    list->device->vk.CmdCopyImage(list->command_buffer, src->image,
                                  VK_IMAGE_LAYOUT_GENERAL,
                                  dst->image,
                                  VK_IMAGE_LAYOUT_GENERAL, 1,
                                  &region);
}

/* Image and sampler descriptors ----------------------------------------------------- */

static int write_image(BD12DescriptorHeap *heap, uint32_t index,
                       BD12DescriptorKind kind, BD12Image *image,
                       VkImageViewType type, VkFormat format, uint32_t base_mip,
                       uint32_t mip_count, uint32_t base_layer,
                       uint32_t layer_count)
{
    if (index >= heap->count) return 0;
    BD12Descriptor *d = &heap->descriptors[index];
    memset(d, 0, sizeof(*d));
    if (!image) return 1;                      /* null descriptor */
    VkImageView view = bd12_image_view(image, type, format, 0, base_mip,
                                       mip_count, base_layer, layer_count);
    if (!view) return 0;
    d->kind = kind;
    d->image = image;
    d->image_serial = image->serial;
    d->image_view = view;
    d->view_mip = base_mip;
    d->view_base_layer = base_layer;
    d->view_layers = layer_count;
    d->image_layout = kind == BD12_DESC_UAV_IMAGE ? VK_IMAGE_LAYOUT_GENERAL :
        bd12_layout_for_state(BD12_STATE_PIXEL_SHADER_RESOURCE,
                              image_is_ds(image));
    return 1;
}

int bd12_descriptor_write_image_srv(BD12DescriptorHeap *heap, uint32_t index,
                                    BD12Image *image, VkImageViewType type,
                                    VkFormat format, uint32_t base_mip,
                                    uint32_t mip_count, uint32_t base_layer,
                                    uint32_t layer_count)
{
    return write_image(heap, index, BD12_DESC_SRV_IMAGE, image, type, format,
                       base_mip, mip_count, base_layer, layer_count);
}

int bd12_descriptor_write_image_uav(BD12DescriptorHeap *heap, uint32_t index,
                                    BD12Image *image, VkImageViewType type,
                                    VkFormat format, uint32_t mip,
                                    uint32_t base_layer, uint32_t layer_count)
{
    return write_image(heap, index, BD12_DESC_UAV_IMAGE, image, type, format,
                       mip, 1, base_layer, layer_count);
}

static int sampler_equal(const VkSamplerCreateInfo *a,
                         const VkSamplerCreateInfo *b)
{
    return a->flags == b->flags && a->magFilter == b->magFilter &&
           a->minFilter == b->minFilter && a->mipmapMode == b->mipmapMode &&
           a->addressModeU == b->addressModeU &&
           a->addressModeV == b->addressModeV &&
           a->addressModeW == b->addressModeW &&
           a->mipLodBias == b->mipLodBias &&
           a->anisotropyEnable == b->anisotropyEnable &&
           a->maxAnisotropy == b->maxAnisotropy &&
           a->compareEnable == b->compareEnable &&
           a->compareOp == b->compareOp && a->minLod == b->minLod &&
           a->maxLod == b->maxLod && a->borderColor == b->borderColor &&
           a->unnormalizedCoordinates == b->unnormalizedCoordinates;
}

VkSampler bd12_sampler_get(BD12Device *device, const VkSamplerCreateInfo *info)
{
    VkSampler sampler = VK_NULL_HANDLE;
    pthread_mutex_lock(&device->view_lock);
    for (size_t i = 0; i < device->sampler_count; ++i) {
        if (sampler_equal(&device->samplers[i].info, info)) {
            sampler = device->samplers[i].sampler;
            break;
        }
    }
    if (!sampler) {
        VkSamplerCreateInfo create = *info;
        create.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        create.pNext = NULL;
        if (device->vk.CreateSampler(device->device, &create, NULL, &sampler) ==
            VK_SUCCESS) {
            if (device->sampler_count == device->sampler_capacity) {
                size_t capacity = device->sampler_capacity ?
                    device->sampler_capacity * 2 : 64;
                BD12SamplerEntry *grown = realloc(device->samplers,
                                                  capacity * sizeof(*grown));
                if (!grown) {
                    device->vk.DestroySampler(device->device, sampler, NULL);
                    sampler = VK_NULL_HANDLE;
                } else {
                    device->samplers = grown;
                    device->sampler_capacity = capacity;
                }
            }
            if (sampler) {
                device->samplers[device->sampler_count++] =
                    (BD12SamplerEntry){ create, sampler };
            }
        } else {
            sampler = VK_NULL_HANDLE;
        }
    }
    pthread_mutex_unlock(&device->view_lock);
    return sampler;
}

int bd12_descriptor_write_sampler(BD12DescriptorHeap *heap, uint32_t index,
                                  const VkSamplerCreateInfo *info)
{
    if (index >= heap->count) return 0;
    BD12Descriptor *d = &heap->descriptors[index];
    memset(d, 0, sizeof(*d));
    if (!info) return 1;
    VkSampler sampler = bd12_sampler_get(heap->device, info);
    if (!sampler) return 0;
    d->kind = BD12_DESC_SAMPLER;
    d->sampler = sampler;
    return 1;
}

/* Render target and depth-stencil descriptors ---------------------------------------- */

static int write_attachment(BD12DescriptorHeap *heap, uint32_t index,
                            BD12DescriptorKind kind, BD12Image *image,
                            VkFormat format, uint32_t mip, uint32_t base_layer,
                            uint32_t layer_count, VkImageLayout layout)
{
    if (index >= heap->count) return 0;
    BD12Descriptor *d = &heap->descriptors[index];
    memset(d, 0, sizeof(*d));
    if (!image) return 1;
    const int array = image->desc.type == VK_IMAGE_TYPE_3D ||
                      image->desc.depth_or_layers > 1 || layer_count > 1;
    VkImageAspectFlags aspect = image->aspect;
    VkImageView view = bd12_image_view(
        image, array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
        format, aspect, mip, 1, base_layer, layer_count);
    if (!view) return 0;
    const uint32_t layers = layer_count == UINT32_MAX ?
        (image->desc.type == VK_IMAGE_TYPE_3D ? image->desc.depth_or_layers :
         image->desc.depth_or_layers) - base_layer : layer_count;
    d->kind = kind;
    d->image = image;
    d->image_serial = image->serial;
    d->image_view = view;
    d->image_layout = layout;
    d->format = format == VK_FORMAT_UNDEFINED ? image->desc.format : format;
    d->aspect = aspect;
    d->width = image->desc.width >> mip ? image->desc.width >> mip : 1;
    d->height = image->desc.height >> mip ? image->desc.height >> mip : 1;
    d->layer_count = layers;
    return 1;
}

int bd12_descriptor_write_rtv(BD12DescriptorHeap *heap, uint32_t index,
                              BD12Image *image, VkFormat format, uint32_t mip,
                              uint32_t base_layer, uint32_t layer_count)
{
    return write_attachment(heap, index, BD12_DESC_RTV, image, format, mip,
                            base_layer, layer_count,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

int bd12_descriptor_write_dsv(BD12DescriptorHeap *heap, uint32_t index,
                              BD12Image *image, VkFormat format, uint32_t mip,
                              uint32_t base_layer, uint32_t layer_count,
                              int read_only_depth, int read_only_stencil)
{
    const int read_only = read_only_depth &&
        (read_only_stencil || !(image && (image->aspect &
                                          VK_IMAGE_ASPECT_STENCIL_BIT)));
    return write_attachment(heap, index, BD12_DESC_DSV, image, format, mip,
                            base_layer, layer_count,
                            read_only ?
                                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL :
                                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
}
