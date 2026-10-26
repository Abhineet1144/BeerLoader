#include "vulkan_post.h"
#include "vulkan_context.h"
#include "../../platform/beer_overlay.h"
#include "../../platform/beer_post.h"
#include "post_effects_spv.h"
#include "post_overlay_spv.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define OVERLAY_REGION_BYTES (4u * 1024u * 1024u)

#define FN(name) PFN_vk##name name
static struct {
    int tried, ready;
    VkDevice device;
    VkPhysicalDevice physical;
    FN(CreateShaderModule); FN(DestroyShaderModule); FN(CreateDescriptorSetLayout); FN(CreatePipelineLayout);
    FN(CreateComputePipelines); FN(CreateDescriptorPool); FN(AllocateDescriptorSets); FN(UpdateDescriptorSets);
    FN(CreateImageView); FN(DestroyImageView); FN(CmdBindPipeline); FN(CmdBindDescriptorSets); FN(CmdPushConstants);
    FN(CmdDispatch); FN(CmdPipelineBarrier); FN(CmdCopyImageToBuffer); FN(CreateImage); FN(DestroyImage);
    FN(GetImageMemoryRequirements); FN(AllocateMemory); FN(FreeMemory); FN(BindImageMemory); FN(CreateBuffer);
    FN(DestroyBuffer); FN(GetBufferMemoryRequirements); FN(BindBufferMemory); FN(MapMemory); FN(DeviceWaitIdle);
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;

    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    VkPipeline effects, overlay;
    VkDescriptorPool pool;
    VkDescriptorSet set;

    uint32_t width, height;
    VkImage upload_for;
    VkImage out_image;
    VkDeviceMemory out_memory;
    VkImageView out_view, upload_view;

    VkBuffer overlay_buffer;
    VkDeviceMemory overlay_memory;
    uint8_t *overlay_map;
    uint64_t overlay_serial[2];

    VkBuffer shot_buffer;
    VkDeviceMemory shot_memory;
    uint8_t *shot_map;
    size_t shot_capacity;
    int shot_waiting;
    uint32_t shot_w, shot_h;
    char shot_path[512];
} P;

#define LOAD(name) do { P.name = (PFN_vk##name)get(P.device, "vk" #name); if (!P.name) return 0; } while (0)

static int find_memory(uint32_t bits, VkMemoryPropertyFlags want, uint32_t *index)
{
    VkPhysicalDeviceMemoryProperties memory;
    P.GetPhysicalDeviceMemoryProperties(P.physical, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & want) == want) { *index = i; return 1; }
    return 0;
}

static int make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buffer, VkDeviceMemory *memory, uint8_t **map)
{
    VkBufferCreateInfo info = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkMemoryRequirements req;
    uint32_t type;
    if (P.CreateBuffer(P.device, &info, NULL, buffer) != VK_SUCCESS) return 0;
    P.GetBufferMemoryRequirements(P.device, *buffer, &req);
    if (!find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &type)) return 0;
    VkMemoryAllocateInfo alloc = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type };
    return P.AllocateMemory(P.device, &alloc, NULL, memory) == VK_SUCCESS &&
           P.BindBufferMemory(P.device, *buffer, *memory, 0) == VK_SUCCESS &&
           P.MapMemory(P.device, *memory, 0, size, 0, (void **)map) == VK_SUCCESS;
}

static int make_pipeline(const uint32_t *code, size_t bytes, VkPipeline *pipeline)
{
    VkShaderModuleCreateInfo module_info = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code };
    VkShaderModule module;
    if (P.CreateShaderModule(P.device, &module_info, NULL, &module) != VK_SUCCESS) return 0;
    VkComputePipelineCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main" },
        .layout = P.layout
    };
    const VkResult r = P.CreateComputePipelines(P.device, VK_NULL_HANDLE, 1, &info, NULL, pipeline);
    P.DestroyShaderModule(P.device, module, NULL);
    return r == VK_SUCCESS;
}

static int init(void)
{
    if (P.tried) return P.ready;
    P.tried = 1;
    if (getenv("BEER_NO_POST")) return 0;
    P.device = beer_vulkan_device();
    P.physical = beer_vulkan_physical_device();
    PFN_vkGetDeviceProcAddr get = beer_vulkan_get_device_proc_addr();
    PFN_vkGetInstanceProcAddr get_instance = beer_vulkan_get_instance_proc_addr();
    if (!P.device || !P.physical || !get || !get_instance) return 0;
    P.GetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)get_instance(beer_vulkan_instance(), "vkGetPhysicalDeviceMemoryProperties");
    if (!P.GetPhysicalDeviceMemoryProperties) return 0;
    LOAD(CreateShaderModule); LOAD(DestroyShaderModule); LOAD(CreateDescriptorSetLayout); LOAD(CreatePipelineLayout);
    LOAD(CreateComputePipelines); LOAD(CreateDescriptorPool); LOAD(AllocateDescriptorSets); LOAD(UpdateDescriptorSets);
    LOAD(CreateImageView); LOAD(DestroyImageView); LOAD(CmdBindPipeline); LOAD(CmdBindDescriptorSets); LOAD(CmdPushConstants);
    LOAD(CmdDispatch); LOAD(CmdPipelineBarrier); LOAD(CmdCopyImageToBuffer); LOAD(CreateImage); LOAD(DestroyImage);
    LOAD(GetImageMemoryRequirements); LOAD(AllocateMemory); LOAD(FreeMemory); LOAD(BindImageMemory); LOAD(CreateBuffer);
    LOAD(DestroyBuffer); LOAD(GetBufferMemoryRequirements); LOAD(BindBufferMemory); LOAD(MapMemory); LOAD(DeviceWaitIdle);

    VkDescriptorSetLayoutBinding bindings[3] = {
        { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
    };
    VkDescriptorSetLayoutCreateInfo layout_info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = bindings };
    if (P.CreateDescriptorSetLayout(P.device, &layout_info, NULL, &P.set_layout) != VK_SUCCESS) return 0;
    VkPushConstantRange push = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 64 };
    VkPipelineLayoutCreateInfo pl_info = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &P.set_layout,
                                           .pushConstantRangeCount = 1, .pPushConstantRanges = &push };
    if (P.CreatePipelineLayout(P.device, &pl_info, NULL, &P.layout) != VK_SUCCESS) return 0;
    if (!make_pipeline(post_effects_spv, sizeof(post_effects_spv), &P.effects) ||
        !make_pipeline(post_overlay_spv, sizeof(post_overlay_spv), &P.overlay)) return 0;
    VkDescriptorPoolSize sizes[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 } };
    VkDescriptorPoolCreateInfo pool_info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = sizes };
    if (P.CreateDescriptorPool(P.device, &pool_info, NULL, &P.pool) != VK_SUCCESS) return 0;
    VkDescriptorSetAllocateInfo set_info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = P.pool, .descriptorSetCount = 1, .pSetLayouts = &P.set_layout };
    if (P.AllocateDescriptorSets(P.device, &set_info, &P.set) != VK_SUCCESS) return 0;
    if (!make_buffer(2u * OVERLAY_REGION_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &P.overlay_buffer, &P.overlay_memory, &P.overlay_map)) return 0;
    P.ready = 1;
    fprintf(stderr, "[POST] final-image compute passes ready\n");
    return 1;
}

/* The presenter calls this whenever it destroys its upload image: Vulkan may hand the same handle value to the next image,
 * and the cached view would then point at freed memory (a GPU write fault at address 0). */
void vulkan_post_forget_upload(void)
{
    P.upload_for = VK_NULL_HANDLE;
}

static int prepare_images(VkImage upload, uint32_t w, uint32_t h)
{
    if (P.upload_for == upload && P.width == w && P.height == h && P.out_image) return 1;
    P.DeviceWaitIdle(P.device);
    if (P.out_view) P.DestroyImageView(P.device, P.out_view, NULL);
    if (P.upload_view) P.DestroyImageView(P.device, P.upload_view, NULL);
    if (P.out_image) P.DestroyImage(P.device, P.out_image, NULL);
    if (P.out_memory) P.FreeMemory(P.device, P.out_memory, NULL);
    P.out_view = P.upload_view = VK_NULL_HANDLE;
    P.out_image = VK_NULL_HANDLE;
    P.out_memory = VK_NULL_HANDLE;
    VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };
    VkMemoryRequirements req;
    uint32_t type;
    if (P.CreateImage(P.device, &info, NULL, &P.out_image) != VK_SUCCESS) return 0;
    P.GetImageMemoryRequirements(P.device, P.out_image, &req);
    if (!find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type)) return 0;
    VkMemoryAllocateInfo alloc = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type };
    if (P.AllocateMemory(P.device, &alloc, NULL, &P.out_memory) != VK_SUCCESS ||
        P.BindImageMemory(P.device, P.out_image, P.out_memory, 0) != VK_SUCCESS) return 0;
    VkImageViewCreateInfo view = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                                   .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    view.image = P.out_image;
    if (P.CreateImageView(P.device, &view, NULL, &P.out_view) != VK_SUCCESS) return 0;
    view.image = upload;
    if (P.CreateImageView(P.device, &view, NULL, &P.upload_view) != VK_SUCCESS) return 0;
    VkDescriptorImageInfo images[2] = { { VK_NULL_HANDLE, P.upload_view, VK_IMAGE_LAYOUT_GENERAL }, { VK_NULL_HANDLE, P.out_view, VK_IMAGE_LAYOUT_GENERAL } };
    VkDescriptorBufferInfo buffer = { P.overlay_buffer, 0, 2u * OVERLAY_REGION_BYTES };
    VkWriteDescriptorSet writes[3] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = P.set, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &images[0] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = P.set, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &images[1] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = P.set, .dstBinding = 2, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &buffer },
    };
    P.UpdateDescriptorSets(P.device, 3, writes, 0, NULL);
    P.upload_for = upload;
    P.width = w;
    P.height = h;
    return 1;
}

static void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
                          VkAccessFlags dst_access, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = src_access, .dstAccessMask = dst_access,
        .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
    };
    P.CmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &barrier);
}

static void compute_barrier(VkCommandBuffer cmd)
{
    VkMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT };
    P.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
}

/* Copies the finished image into the screenshot buffer (image GENERAL -> GENERAL). */
static int record_screenshot(VkCommandBuffer cmd, const char *path)
{
    const size_t bytes = (size_t)P.width * P.height * 4;
    if (bytes > P.shot_capacity) {
        if (P.shot_buffer) { P.DeviceWaitIdle(P.device); P.DestroyBuffer(P.device, P.shot_buffer, NULL); P.FreeMemory(P.device, P.shot_memory, NULL); }
        P.shot_buffer = VK_NULL_HANDLE; P.shot_capacity = 0;
        if (!make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &P.shot_buffer, &P.shot_memory, &P.shot_map)) return 0;
        P.shot_capacity = bytes;
    }
    if (P.shot_waiting) return 0;   /* the previous screenshot is still being collected */
    image_barrier(cmd, P.out_image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
                  VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { P.width, P.height, 1 } };
    P.CmdCopyImageToBuffer(cmd, P.out_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, P.shot_buffer, 1, &region);
    image_barrier(cmd, P.out_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    snprintf(P.shot_path, sizeof(P.shot_path), "%s", path);
    P.shot_w = P.width;
    P.shot_h = P.height;
    P.shot_waiting = 1;
    return 1;
}

VkImage vulkan_post_apply(VkCommandBuffer cmd, VkImage upload, uint32_t width, uint32_t height,
                          uint32_t output_width, uint32_t output_height, int *overlay_done)
{
    if (overlay_done) *overlay_done = 0;
    if (!init() || width == 0 || height == 0) return VK_NULL_HANDLE;
    BeerPostParams params;
    const int effects = beer_post_params(&params);
    BeerOverlayRect rects[2];
    const int boxes = beer_overlay_frame((int)output_width, (int)output_height, rects);
    char shot_path[512];
    int shot_clean = 0;
    const int shot = beer_post_take_screenshot(shot_path, sizeof(shot_path), &shot_clean);
    if (!effects && boxes == 0 && !shot) return VK_NULL_HANDLE;
    if (!prepare_images(upload, width, height)) return VK_NULL_HANDLE;

    /* Overlay pixels go into the storage buffer (cached per box by serial). */
    uint32_t offsets[2] = {0, 0};
    for (int i = 0; i < boxes; ++i) {
        const size_t bytes = (size_t)rects[i].w * rects[i].h * 4;
        const int slot = (int)(rects[i].serial & 1u);
        if (bytes > OVERLAY_REGION_BYTES) { rects[i].w = 0; continue; }
        offsets[i] = (uint32_t)((size_t)slot * OVERLAY_REGION_BYTES / 4);
        if (P.overlay_serial[slot] != rects[i].serial) {
            memcpy(P.overlay_map + (size_t)slot * OVERLAY_REGION_BYTES, rects[i].bgra, bytes);
            P.overlay_serial[slot] = rects[i].serial;
        }
    }

    image_barrier(cmd, upload, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    image_barrier(cmd, P.out_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    P.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, P.layout, 0, 1, &P.set, 0, NULL);

    int32_t effect_push[16] = { (int32_t)width, (int32_t)height, params.brightness, params.contrast, params.gamma, params.saturation,
                                params.vibrance, params.sharpen, params.vignette, params.grain, params.temperature, params.colorblind,
                                params.cb_strength, params.letterbox, params.seed, 0 };
    P.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, P.effects);
    P.CmdPushConstants(cmd, P.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(effect_push), effect_push);
    P.CmdDispatch(cmd, (width + 15) / 16, (height + 15) / 16, 1);

    int screenshot_taken = 0;
    if (shot && (shot_clean || boxes == 0)) { compute_barrier(cmd); screenshot_taken = record_screenshot(cmd, shot_path); }
    int blended = 0;
    if (boxes > 0) {
        compute_barrier(cmd);
        P.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, P.overlay);
        for (int i = 0; i < boxes; ++i) {
            if (rects[i].w <= 0) continue;
            /* The boxes are placed in output pixels; scale them into source pixels when the window differs from the game. */
            const int32_t rx = (int32_t)((int64_t)rects[i].x * width / output_width), ry = (int32_t)((int64_t)rects[i].y * height / output_height);
            const int32_t rw = (int32_t)((int64_t)rects[i].w * width / output_width), rh = (int32_t)((int64_t)rects[i].h * height / output_height);
            if (rw <= 0 || rh <= 0) continue;
            int32_t push[8] = { rx, ry, rw, rh, rects[i].w, rects[i].h, (int32_t)offsets[i], 1000 };
            P.CmdPushConstants(cmd, P.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
            P.CmdDispatch(cmd, ((uint32_t)rw + 15) / 16, ((uint32_t)rh + 15) / 16, 1);
            compute_barrier(cmd);
            blended = 1;
        }
        if (shot && !shot_clean && !screenshot_taken) screenshot_taken = record_screenshot(cmd, shot_path);
    }
    (void)screenshot_taken;
    image_barrier(cmd, P.out_image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
                  VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    image_barrier(cmd, upload, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    if (overlay_done) *overlay_done = blended || boxes > 0;
    return P.out_image;
}

/* ---- screenshot encoding (off the render thread) ---- */

typedef struct { char path[512]; uint8_t *rgba; uint32_t w, h; } ShotJob;

static void put_be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static void write_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t length)
{
    uint8_t head[8];
    put_be32(head, length);
    memcpy(head + 4, type, 4);
    fwrite(head, 1, 8, f);
    if (length) fwrite(data, 1, length, f);
    uLong crc = crc32(0, (const Bytef *)type, 4);
    if (length) crc = crc32(crc, data, length);
    uint8_t tail[4];
    put_be32(tail, (uint32_t)crc);
    fwrite(tail, 1, 4, f);
}

static void *shot_thread(void *opaque)
{
    ShotJob *job = opaque;
    const size_t row = (size_t)job->w * 3 + 1;
    uint8_t *raw = malloc(row * job->h);
    if (raw) {
        for (uint32_t y = 0; y < job->h; ++y) {
            uint8_t *dst = raw + y * row;
            const uint8_t *src = job->rgba + (size_t)y * job->w * 4;
            *dst++ = 0;   /* filter: none */
            for (uint32_t x = 0; x < job->w; ++x) { dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst += 3; src += 4; }
        }
        uLongf packed_size = compressBound((uLong)(row * job->h));
        uint8_t *packed = malloc(packed_size);
        if (packed && compress2(packed, &packed_size, raw, (uLong)(row * job->h), 3) == Z_OK) {
            char tmp[540];
            snprintf(tmp, sizeof(tmp), "%s.part", job->path);
            FILE *f = fopen(tmp, "wb");
            if (f) {
                static const uint8_t signature[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
                fwrite(signature, 1, 8, f);
                uint8_t ihdr[13];
                put_be32(ihdr, job->w); put_be32(ihdr + 4, job->h);
                ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
                write_chunk(f, "IHDR", ihdr, 13);
                write_chunk(f, "IDAT", packed, (uint32_t)packed_size);
                write_chunk(f, "IEND", NULL, 0);
                fclose(f);
                rename(tmp, job->path);
                fprintf(stderr, "[POST] screenshot saved: %s\n", job->path);
            }
        }
        free(packed);
        free(raw);
    }
    free(job->rgba);
    free(job);
    return NULL;
}

void vulkan_post_collect(void)
{
    if (!P.shot_waiting) return;
    P.shot_waiting = 0;
    ShotJob *job = calloc(1, sizeof(*job));
    if (!job) return;
    const size_t bytes = (size_t)P.shot_w * P.shot_h * 4;
    job->rgba = malloc(bytes);
    if (!job->rgba) { free(job); return; }
    memcpy(job->rgba, P.shot_map, bytes);
    job->w = P.shot_w;
    job->h = P.shot_h;
    snprintf(job->path, sizeof(job->path), "%s", P.shot_path);
    pthread_t thread;
    if (pthread_create(&thread, NULL, shot_thread, job) == 0) pthread_detach(thread);
    else { free(job->rgba); free(job); }
}
