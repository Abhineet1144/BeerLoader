#define _GNU_SOURCE
#include "vulkan_graphics.h"
#include "vulkan_context.h"
/* Compute-produced image mirrors and CPU-shadow publication came from the
 * removed host-snapshot D3D12 bridge; D3D11 rendering has neither, so these
 * are inert and every image is uploaded from its source. */
typedef struct BeerVulkanComputeImageMirror {
    uint64_t image;
    uint32_t format, width, height, depth, layers, dimension, mip_levels, layout;
    void *token;
} BeerVulkanComputeImageMirror;
static inline void beer_vulkan_compute_sync_buffer_resources(
    const void *const *resources, uint32_t count)
{ (void)resources; (void)count; }
static inline int beer_vulkan_compute_has_image_mirror(
    const void *resource, uint64_t serial, uint32_t format, uint32_t width,
    uint32_t height, uint32_t depth, uint32_t layers, uint32_t dimension,
    uint32_t mip_levels)
{ (void)resource; (void)serial; (void)format; (void)width; (void)height;
  (void)depth; (void)layers; (void)dimension; (void)mip_levels; return 0; }
static inline int beer_vulkan_compute_acquire_image_mirror(
    const void *resource, uint64_t serial, uint32_t format, uint32_t width,
    uint32_t height, uint32_t depth, uint32_t layers, uint32_t dimension,
    uint32_t mip_levels, BeerVulkanComputeImageMirror *mirror)
{ (void)resource; (void)serial; (void)format; (void)width; (void)height;
  (void)depth; (void)layers; (void)dimension; (void)mip_levels; (void)mirror;
  return 0; }
static inline void beer_vulkan_compute_release_image_mirror(
    BeerVulkanComputeImageMirror *mirror) { (void)mirror; }

#include <pthread.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_PIPELINES 128u
/* RE8's LUT/resource arrays can exceed 512 MiB when mirrored synchronously.
 * Keep a hard cap, but allow the validated 64-layer title resources. */
#define MAX_UPLOAD_BYTES ((size_t)1024 * 1024 * 1024)
#define ALIGNMENT 256u
/* Modern frame graphs can cycle hundreds of distinct image views between two
 * uses of a large immutable texture array. A 256-entry LRU produced a cyclic
 * miss pattern even while the byte budget had ample space, forcing unchanged
 * 64 MiB and 256 MiB arrays through host staging every draw. Keep identity
 * capacity independent from the authoritative byte budget. */
#define IMAGE_CACHE_SIZE 1024u
/* Samplers are immutable Vulkan objects. D3D12 titles commonly reuse a small
 * set of sampler descriptions across thousands of draws; creating and
 * destroying them in every scalar draw adds pure driver overhead. */
#define SAMPLER_CACHE_SIZE 256u
#define FRAMEBUFFER_CACHE_SIZE 256u
#define BUFFER_VIEW_CACHE_SIZE 512u
/* Persistent mirrors are an optimization, not part of resource ownership.
 * RE8 cycles many large title resources; retaining 64 unconstrained images can
 * consume several GiB and trigger the system OOM killer. Keep large images
 * usable for the current draw, but only retain a bounded working set. */
/* A current high-end frame can reference two immutable texture arrays totaling
 * 320 MiB plus several hundred MiB of post-process images. A 768 MiB budget
 * evicted those serial-0 arrays every draw and forced ~429 MiB of host upload.
 * Keep the cache bounded well below the process safety limit, but large enough
 * for one complete frame working set. */
/* Graphics shares the process budget with D3D12 CPU backing, compute mirrors,
 * and three mapped transfer arenas. One GiB retains the measured 320 MiB
 * immutable set plus current intermediates without allowing independent caches
 * to push the process beyond typical 6-8 GiB safety scopes. */
#define IMAGE_CACHE_MAX_BYTES ((size_t)640 * 1024 * 1024)
#define IMAGE_CACHE_MAX_ENTRY_BYTES ((size_t)384 * 1024 * 1024)

static uint64_t monotonic_nanoseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static void write_loss_blob(const char *directory, const char *name,
                            const void *data, size_t size)
{
    if (!directory || !*directory || !name || !data || !size) return;
    char path[512];
    if (snprintf(path, sizeof(path), "%s/%s", directory, name) <= 0) return;
    FILE *file = fopen(path, "wb");
    if (!file) return;
    fwrite(data, 1, size, file);
    fclose(file);
}

static void capture_graphics_device_loss(
    const void *vertex_spirv, size_t vertex_spirv_size,
    const void *pixel_spirv, size_t pixel_spirv_size,
    const BeerVulkanGraphicsBinding *bindings, uint32_t binding_count,
    const BeerVulkanGraphicsDrawInfo *draw,
    const BeerVulkanGraphicsState *state,
    uint32_t target_count, const BeerVulkanGraphicsDepthTarget *depth,
    const float viewport[6], const int32_t scissor[4])
{
    static int captured;
    if (captured) return;
    captured = 1;
    const char *directory = getenv("BEER_VULKAN_DEVICE_LOSS_DIR");
    if (!directory || !*directory) directory = "/tmp";

    char name[160];
    uint64_t vertex_hash = draw ? draw->debug_vertex_hash : 0;
    uint64_t pixel_hash = draw ? draw->debug_pixel_hash : 0;
    snprintf(name, sizeof(name), "beer-graphics-loss-%016llx-vs.spv",
             (unsigned long long)vertex_hash);
    write_loss_blob(directory, name, vertex_spirv, vertex_spirv_size);
    snprintf(name, sizeof(name), "beer-graphics-loss-%016llx-ps.spv",
             (unsigned long long)pixel_hash);
    write_loss_blob(directory, name, pixel_spirv, pixel_spirv_size);

    snprintf(name, sizeof(name), "beer-graphics-loss-%016llx-%016llx.txt",
             (unsigned long long)vertex_hash,
             (unsigned long long)pixel_hash);
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", directory, name);
    FILE *file = fopen(path, "w");
    if (!file) return;
    fprintf(file,
            "submission=%llu list=%u sequence=%llu vs=%016llx ps=%016llx\n",
            (unsigned long long)(draw ? draw->debug_submission : 0),
            draw ? draw->debug_list_index : 0,
            (unsigned long long)(draw ? draw->debug_sequence : 0),
            (unsigned long long)vertex_hash,
            (unsigned long long)pixel_hash);
    if (draw) {
        fprintf(file,
                "indexed=%u elements=%u instances=%u first=%u base=%d "
                "first-instance=%u topology=%u index-type=%u index-size=%zu "
                "vertex-bindings=%u vertex-attributes=%u\n",
                draw->indexed, draw->element_count, draw->instance_count,
                draw->start_element, draw->base_vertex, draw->start_instance,
                draw->topology, draw->index_type, draw->index_size,
                draw->vertex_binding_count, draw->vertex_attribute_count);
        for (uint32_t i = 0; i < draw->vertex_binding_count; ++i)
            fprintf(file,
                    "vb[%u] data=%p size=%zu stride=%u rate=%u\n", i,
                    (const void *)draw->vertex_bindings[i].data,
                    draw->vertex_bindings[i].size,
                    draw->vertex_bindings[i].stride,
                    draw->vertex_bindings[i].input_rate);
        for (uint32_t i = 0; i < draw->vertex_attribute_count; ++i)
            fprintf(file,
                    "attribute[%u] location=%u binding=%u format=%u offset=%u\n",
                    i, draw->vertex_attributes[i].location,
                    draw->vertex_attributes[i].binding,
                    draw->vertex_attributes[i].format,
                    draw->vertex_attributes[i].offset);
    }
    if (state)
        fprintf(file,
                "state blend=%u write=%x cull=%u front-ccw=%u restart=%u "
                "depth=%u/%u/%u stencil=%u read=%x write=%x flip-y=%u\n",
                state->blend_enable, state->write_mask, state->cull_mode,
                state->front_counter_clockwise,
                state->primitive_restart_enable, state->depth_test_enable,
                state->depth_write_enable, state->depth_compare_op,
                state->stencil_test_enable, state->stencil_read_mask,
                state->stencil_write_mask, state->flip_y);
    fprintf(file,
            "targets=%u depth=%p depth-size=%ux%u depth-format=%u "
            "viewport=%g,%g,%g,%g,%g,%g scissor=%d,%d,%d,%d bindings=%u\n",
            target_count, (const void *)depth, depth ? depth->width : 0,
            depth ? depth->height : 0, depth ? depth->format : 0,
            viewport ? viewport[0] : 0.0f, viewport ? viewport[1] : 0.0f,
            viewport ? viewport[2] : 0.0f, viewport ? viewport[3] : 0.0f,
            viewport ? viewport[4] : 0.0f, viewport ? viewport[5] : 0.0f,
            scissor ? scissor[0] : 0, scissor ? scissor[1] : 0,
            scissor ? scissor[2] : 0, scissor ? scissor[3] : 0,
            binding_count);
    for (uint32_t i = 0; i < binding_count; ++i) {
        const BeerVulkanGraphicsBinding *binding = &bindings[i];
        fprintf(file,
                "binding[%u] slot=%u kind=%u stages=%u resource=%p serial=%llu "
                "data=%p size=%zu format=%u texel=%u/%u image=%ux%ux%u "
                "layers=%u mips=%u dimension=%u cube=%u row=%u\n",
                i, binding->binding, binding->kind, binding->stage_flags,
                binding->resource,
                (unsigned long long)(binding->serial ?
                    *binding->serial : UINT64_MAX), binding->data, binding->size,
                binding->format, binding->texel_format, binding->texel_bytes,
                binding->width, binding->height,
                binding->depth ? binding->depth : 1,
                binding->layer_count ? binding->layer_count : 1,
                binding->mip_levels ? binding->mip_levels : 1,
                binding->image_dimension, binding->image_cube,
                binding->row_pitch);
    }
    fclose(file);

    if (draw && draw->index_data && draw->index_size) {
        snprintf(name, sizeof(name),
                 "beer-graphics-loss-%016llx-%016llx-index.bin",
                 (unsigned long long)vertex_hash,
                 (unsigned long long)pixel_hash);
        write_loss_blob(directory, name, draw->index_data,
                        draw->index_size > 4u * 1024u * 1024u ?
                            4u * 1024u * 1024u : draw->index_size);
    }
    if (draw) {
        for (uint32_t i = 0; i < draw->vertex_binding_count; ++i) {
            const BeerVulkanGraphicsVertexBinding *vertex =
                &draw->vertex_bindings[i];
            if (!vertex->data || !vertex->size) continue;
            snprintf(name, sizeof(name),
                     "beer-graphics-loss-%016llx-%016llx-vb%u.bin",
                     (unsigned long long)vertex_hash,
                     (unsigned long long)pixel_hash, i);
            write_loss_blob(directory, name, vertex->data,
                            vertex->size > 4u * 1024u * 1024u ?
                                4u * 1024u * 1024u : vertex->size);
        }
    }
    for (uint32_t i = 0; i < binding_count; ++i) {
        if (!bindings[i].data || !bindings[i].size ||
            bindings[i].size > 64u * 1024u) continue;
        snprintf(name, sizeof(name),
                 "beer-graphics-loss-%016llx-%016llx-binding%u.bin",
                 (unsigned long long)vertex_hash,
                 (unsigned long long)pixel_hash, i);
        write_loss_blob(directory, name, bindings[i].data, bindings[i].size);
    }
}

typedef struct PipelineEntry {
    const void *vertex_key;
    const void *pixel_key;
    uint32_t target_count;
    uint32_t target_formats[BEER_VULKAN_GRAPHICS_MAX_TARGETS];
    uint32_t depth_format;
    uint32_t binding_count;
    uint32_t bindings[BEER_VULKAN_GRAPHICS_MAX_BINDINGS];
    uint32_t binding_kinds[BEER_VULKAN_GRAPHICS_MAX_BINDINGS];
    uint32_t binding_stages[BEER_VULKAN_GRAPHICS_MAX_BINDINGS];
    uint32_t vertex_binding_count;
    uint32_t vertex_attribute_count;
    BeerVulkanGraphicsVertexBinding
        vertex_bindings[BEER_VULKAN_GRAPHICS_MAX_VERTEX_BINDINGS];
    BeerVulkanGraphicsVertexAttribute
        vertex_attributes[BEER_VULKAN_GRAPHICS_MAX_VERTEX_ATTRIBUTES];
    uint32_t topology;
    BeerVulkanGraphicsState state;
    VkDescriptorSetLayout set_layout;
    VkDescriptorSet descriptor_set;
    VkPipelineLayout layout;
    VkRenderPass render_pass;
    VkPipeline pipeline;
    const char *failure;
} PipelineEntry;

typedef struct ImageObject {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkImageAspectFlags aspect;
    VkImageUsageFlags usage;
    size_t allocation_size;
    const void *resource;
    uint64_t serial;
    VkImageLayout layout;
    uint32_t sample_count;
    int content_valid;
} ImageObject;

typedef struct CachedImage {
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t layers;
    uint32_t depth;
    uint32_t dimension;
    uint32_t cube;
    uint32_t mip_levels;
    uint32_t sample_count;
    VkImageUsageFlags usage;
    VkImageAspectFlags aspect;
    uint64_t last_use;
    ImageObject object;
} CachedImage;

typedef struct SamplerKey {
    uint32_t min_filter;
    uint32_t mag_filter;
    uint32_t address_u;
    uint32_t address_v;
    uint32_t address_w;
    uint32_t mipmap_mode;
    uint32_t anisotropy_enable;
    float max_anisotropy;
    uint32_t reduction_mode;
    uint32_t compare_enable;
    uint32_t compare_op;
    uint32_t border_color;
} SamplerKey;

typedef struct SamplerEntry {
    SamplerKey key;
    VkSampler sampler;
} SamplerEntry;

typedef struct FramebufferEntry {
    VkRenderPass render_pass;
    VkImageView attachments[BEER_VULKAN_GRAPHICS_MAX_TARGETS + 1];
    uint32_t attachment_count;
    uint32_t width;
    uint32_t height;
    uint64_t last_use;
    VkFramebuffer framebuffer;
} FramebufferEntry;

typedef struct BufferViewEntry {
    VkFormat format;
    VkDeviceSize offset;
    VkDeviceSize range;
    VkBufferView view;
} BufferViewEntry;

static struct {
    int initialized;
    int unavailable;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkPhysicalDeviceMemoryProperties memory_properties;
    VkCommandPool command_pool;
    VkCommandBuffer command_buffer;
    VkFence fence;
    VkDescriptorPool descriptor_pool;
    VkQueryPool statistics_query_pool;
    VkBuffer staging_buffer;
    VkDeviceMemory staging_memory;
    uint8_t *staging_map;
    size_t staging_capacity;
    PipelineEntry pipelines[MAX_PIPELINES];
    uint32_t pipeline_count;
    CachedImage image_cache[IMAGE_CACHE_SIZE];
    size_t image_cache_bytes;
    uint64_t image_use_serial;
    SamplerEntry sampler_cache[SAMPLER_CACHE_SIZE];
    uint32_t sampler_count;
    FramebufferEntry framebuffer_cache[FRAMEBUFFER_CACHE_SIZE];
    uint64_t framebuffer_use_serial;
    uint64_t framebuffer_cache_hits;
    uint64_t framebuffer_creations;
    BufferViewEntry buffer_view_cache[BUFFER_VIEW_CACHE_SIZE];
    uint32_t buffer_view_count;
    uint64_t buffer_view_cache_hits;
    uint64_t buffer_view_creations;
    uint64_t queue_submissions;
    uint64_t fence_waits;
    uint64_t draw_commands;
    uint64_t host_execution_ns;

    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkResetCommandBuffer ResetCommandBuffer;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkCreateFence CreateFence;
    PFN_vkDestroyFence DestroyFence;
    PFN_vkResetFences ResetFences;
    PFN_vkWaitForFences WaitForFences;
    PFN_vkCreateDescriptorPool CreateDescriptorPool;
    PFN_vkDestroyDescriptorPool DestroyDescriptorPool;
    PFN_vkResetDescriptorPool ResetDescriptorPool;
    PFN_vkAllocateDescriptorSets AllocateDescriptorSets;
    PFN_vkUpdateDescriptorSets UpdateDescriptorSets;
    PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout;
    PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout;
    PFN_vkCreateQueryPool CreateQueryPool;
    PFN_vkDestroyQueryPool DestroyQueryPool;
    PFN_vkCmdResetQueryPool CmdResetQueryPool;
    PFN_vkCmdBeginQuery CmdBeginQuery;
    PFN_vkCmdEndQuery CmdEndQuery;
    PFN_vkGetQueryPoolResults GetQueryPoolResults;
    PFN_vkCreatePipelineLayout CreatePipelineLayout;
    PFN_vkDestroyPipelineLayout DestroyPipelineLayout;
    PFN_vkCreateShaderModule CreateShaderModule;
    PFN_vkDestroyShaderModule DestroyShaderModule;
    PFN_vkCreateRenderPass CreateRenderPass;
    PFN_vkDestroyRenderPass DestroyRenderPass;
    PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines;
    PFN_vkDestroyPipeline DestroyPipeline;
    PFN_vkCreateFramebuffer CreateFramebuffer;
    PFN_vkDestroyFramebuffer DestroyFramebuffer;
    PFN_vkCreateBufferView CreateBufferView;
    PFN_vkDestroyBufferView DestroyBufferView;
    PFN_vkCreateBuffer CreateBuffer;
    PFN_vkDestroyBuffer DestroyBuffer;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkBindBufferMemory BindBufferMemory;
    PFN_vkMapMemory MapMemory;
    PFN_vkUnmapMemory UnmapMemory;
    PFN_vkCreateImage CreateImage;
    PFN_vkDestroyImage DestroyImage;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
    PFN_vkBindImageMemory BindImageMemory;
    PFN_vkCreateImageView CreateImageView;
    PFN_vkDestroyImageView DestroyImageView;
    PFN_vkCreateSampler CreateSampler;
    PFN_vkDestroySampler DestroySampler;
    PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage;
    PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer;
    PFN_vkCmdClearDepthStencilImage CmdClearDepthStencilImage;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkCmdBeginRenderPass CmdBeginRenderPass;
    PFN_vkCmdEndRenderPass CmdEndRenderPass;
    PFN_vkCmdBindPipeline CmdBindPipeline;
    PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets;
    PFN_vkCmdSetViewport CmdSetViewport;
    PFN_vkCmdSetScissor CmdSetScissor;
    PFN_vkCmdSetBlendConstants CmdSetBlendConstants;
    PFN_vkCmdBindVertexBuffers CmdBindVertexBuffers;
    PFN_vkCmdBindIndexBuffer CmdBindIndexBuffer;
    PFN_vkCmdDraw CmdDraw;
    PFN_vkCmdDrawIndexed CmdDrawIndexed;
    PFN_vkQueueSubmit QueueSubmit;
} g_graphics;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

#define LOAD_INSTANCE(name) do { \
    g_graphics.name = (PFN_vk##name)beer_vulkan_get_instance_proc_addr()( \
        beer_vulkan_instance(), "vk" #name); \
    if (!g_graphics.name) goto fail; \
} while (0)
#define LOAD_DEVICE(name) do { \
    g_graphics.name = (PFN_vk##name)beer_vulkan_get_device_proc_addr()( \
        g_graphics.device, "vk" #name); \
    if (!g_graphics.name) goto fail; \
} while (0)

static size_t align_up(size_t value)
{
    return (value + ALIGNMENT - 1) / ALIGNMENT * ALIGNMENT;
}

static SamplerKey sampler_key_from_binding(
    const BeerVulkanGraphicsBinding *binding)
{
    SamplerKey key = {
        .min_filter = binding->min_filter,
        .mag_filter = binding->mag_filter,
        .address_u = binding->address_u,
        .address_v = binding->address_v,
        .address_w = binding->address_w,
        .mipmap_mode = binding->mipmap_mode,
        .anisotropy_enable = binding->anisotropy_enable,
        .max_anisotropy = binding->anisotropy_enable ?
            (binding->max_anisotropy < beer_vulkan_max_sampler_anisotropy() ?
             binding->max_anisotropy : beer_vulkan_max_sampler_anisotropy()) :
            1.0f,
        .reduction_mode = binding->reduction_mode,
        .compare_enable = binding->compare_enable,
        .compare_op = binding->compare_op,
        .border_color = binding->border_color
    };
    return key;
}

static VkSampler find_or_create_sampler(
    const BeerVulkanGraphicsBinding *binding)
{
    SamplerKey key = sampler_key_from_binding(binding);
    for (uint32_t i = 0; i < g_graphics.sampler_count; ++i)
        if (memcmp(&g_graphics.sampler_cache[i].key, &key, sizeof(key)) == 0)
            return g_graphics.sampler_cache[i].sampler;
    if (g_graphics.sampler_count >= SAMPLER_CACHE_SIZE) return VK_NULL_HANDLE;

    VkSamplerCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = key.mag_filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = key.min_filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = key.mipmap_mode ? VK_SAMPLER_MIPMAP_MODE_LINEAR :
                                       VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = (VkSamplerAddressMode)key.address_u,
        .addressModeV = (VkSamplerAddressMode)key.address_v,
        .addressModeW = (VkSamplerAddressMode)key.address_w,
        .anisotropyEnable = key.anisotropy_enable,
        .maxAnisotropy = key.max_anisotropy,
        .compareEnable = key.compare_enable,
        .compareOp = (VkCompareOp)key.compare_op,
        .minLod = 0.0f,
        .maxLod = 1000.0f,
        .borderColor = key.border_color == 2 ?
            VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : key.border_color == 1 ?
            VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK :
            VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
    };
    VkSamplerReductionModeCreateInfo reduction = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO,
        .reductionMode = key.reduction_mode == 1 ?
            VK_SAMPLER_REDUCTION_MODE_MIN : VK_SAMPLER_REDUCTION_MODE_MAX
    };
    if (key.reduction_mode) info.pNext = &reduction;

    SamplerEntry *entry = &g_graphics.sampler_cache[g_graphics.sampler_count];
    if (g_graphics.CreateSampler(g_graphics.device, &info, NULL,
                                 &entry->sampler) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    entry->key = key;
    ++g_graphics.sampler_count;
    return entry->sampler;
}

#define MAX_IMAGE_MIPS 16u

static int binding_mip_layout(const BeerVulkanGraphicsBinding *image,
                              uint32_t level, size_t *offset, size_t *size,
                              uint32_t *width, uint32_t *height,
                              uint32_t *depth)
{
    uint32_t levels = image->mip_levels ? image->mip_levels : 1;
    uint32_t w = image->width, h = image->height;
    uint32_t d = image->depth ? image->depth : 1;
    uint32_t layers = image->layer_count ? image->layer_count : 1;
    size_t cursor = 0;
    if (level >= levels || levels > MAX_IMAGE_MIPS) return 0;
    for (uint32_t mip = 0; mip <= level; ++mip) {
        size_t row_bytes;
        uint32_t rows;
        if (image->block_width && image->block_height &&
            image->bytes_per_block) {
            row_bytes = (size_t)((w + image->block_width - 1) /
                image->block_width) * image->bytes_per_block;
            rows = (h + image->block_height - 1) / image->block_height;
        } else {
            if (!image->bytes_per_pixel || w > SIZE_MAX / image->bytes_per_pixel)
                return 0;
            row_bytes = (size_t)w * image->bytes_per_pixel;
            rows = h;
        }
        size_t pitch = mip == 0 && image->row_pitch ?
            image->row_pitch : row_bytes;
        uint32_t images = image->image_dimension == 3 ? d :
            (image->layer_stride ? 1u : layers);
        if (!pitch || pitch < row_bytes || !rows ||
            pitch > SIZE_MAX / (rows - 1 ? rows - 1 : 1) ||
            pitch * (rows - 1) > SIZE_MAX - row_bytes)
            return 0;
        size_t slice_size = pitch * (rows - 1) + row_bytes;
        if (images > SIZE_MAX / slice_size) return 0;
        size_t level_size = slice_size * images;
        if (mip == level) {
            if (offset) *offset = cursor;
            if (size) *size = level_size;
            if (width) *width = w;
            if (height) *height = h;
            if (depth) *depth = d;
            return 1;
        }
        if (cursor > SIZE_MAX - level_size) return 0;
        cursor += level_size;
        if (w > 1) w >>= 1;
        if (h > 1) h >>= 1;
        if (d > 1) d >>= 1;
    }
    return 0;
}

static int binding_total_size(const BeerVulkanGraphicsBinding *image,
                              size_t *total)
{
    uint32_t levels = image->mip_levels ? image->mip_levels : 1;
    size_t offset = 0, size = 0;
    if (!levels || levels > MAX_IMAGE_MIPS ||
        !binding_mip_layout(image, levels - 1, &offset, &size,
                            NULL, NULL, NULL) ||
        offset > SIZE_MAX - size)
        return 0;
    size_t packed = offset + size;
    uint32_t layers = image->layer_count ? image->layer_count : 1;
    if (image->layer_stride && layers > 1) {
        if (image->layer_stride < packed ||
            layers - 1 > (SIZE_MAX - packed) / image->layer_stride)
            return 0;
        *total = (size_t)(layers - 1) * image->layer_stride + packed;
    } else {
        *total = packed;
    }
    return 1;
}

static int memory_type(uint32_t bits, VkMemoryPropertyFlags wanted)
{
    for (uint32_t i = 0; i < g_graphics.memory_properties.memoryTypeCount; ++i)
        if ((bits & (1u << i)) &&
            (g_graphics.memory_properties.memoryTypes[i].propertyFlags & wanted)
                == wanted)
            return (int)i;
    return -1;
}

static void release_buffer_view_cache(void)
{
    for (uint32_t i = 0; i < g_graphics.buffer_view_count; ++i)
        if (g_graphics.buffer_view_cache[i].view)
            g_graphics.DestroyBufferView(g_graphics.device,
                g_graphics.buffer_view_cache[i].view, NULL);
    memset(g_graphics.buffer_view_cache, 0,
           sizeof(g_graphics.buffer_view_cache));
    g_graphics.buffer_view_count = 0;
}

static VkBufferView find_or_create_buffer_view(VkFormat format,
                                                VkDeviceSize offset,
                                                VkDeviceSize range)
{
    for (uint32_t i = 0; i < g_graphics.buffer_view_count; ++i) {
        BufferViewEntry *entry = &g_graphics.buffer_view_cache[i];
        if (entry->format == format && entry->offset == offset &&
            entry->range == range) {
            ++g_graphics.buffer_view_cache_hits;
            return entry->view;
        }
    }
    if (g_graphics.buffer_view_count >= BUFFER_VIEW_CACHE_SIZE)
        return VK_NULL_HANDLE;
    VkBufferViewCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
        .buffer = g_graphics.staging_buffer,
        .format = format,
        .offset = offset,
        .range = range
    };
    BufferViewEntry *entry =
        &g_graphics.buffer_view_cache[g_graphics.buffer_view_count];
    if (g_graphics.CreateBufferView(g_graphics.device, &info, NULL,
                                    &entry->view) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    entry->format = format;
    entry->offset = offset;
    entry->range = range;
    ++g_graphics.buffer_view_count;
    ++g_graphics.buffer_view_creations;
    return entry->view;
}

static void release_staging(void)
{
    release_buffer_view_cache();
    if (g_graphics.staging_map)
        g_graphics.UnmapMemory(g_graphics.device, g_graphics.staging_memory);
    if (g_graphics.staging_buffer)
        g_graphics.DestroyBuffer(g_graphics.device, g_graphics.staging_buffer,
                                 NULL);
    if (g_graphics.staging_memory)
        g_graphics.FreeMemory(g_graphics.device, g_graphics.staging_memory,
                              NULL);
    g_graphics.staging_map = NULL;
    g_graphics.staging_buffer = VK_NULL_HANDLE;
    g_graphics.staging_memory = VK_NULL_HANDLE;
    g_graphics.staging_capacity = 0;
}

static int ensure_staging(size_t bytes)
{
    if (bytes <= g_graphics.staging_capacity) return 1;
    if (!bytes || bytes > MAX_UPLOAD_BYTES) return 0;
    size_t capacity = g_graphics.staging_capacity ?
        g_graphics.staging_capacity : (size_t)4 * 1024 * 1024;
    if (bytes > (size_t)256 * 1024 * 1024) {
        /* A 528 MiB frame working set previously rounded to a persistent 1 GiB
         * mapping. Above 256 MiB, 64 MiB granularity bounds slack without
         * changing the accepted maximum or reallocating for small variations. */
        const size_t granularity = (size_t)64 * 1024 * 1024;
        if (bytes > SIZE_MAX - (granularity - 1)) return 0;
        capacity = (bytes + granularity - 1) & ~(granularity - 1);
    } else {
        while (capacity < bytes && capacity <= MAX_UPLOAD_BYTES / 2)
            capacity *= 2;
    }
    if (capacity < bytes || capacity > MAX_UPLOAD_BYTES) return 0;
    release_staging();
    VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = capacity,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    if (g_graphics.CreateBuffer(g_graphics.device, &info, NULL,
                                &g_graphics.staging_buffer) != VK_SUCCESS)
        goto fail;
    VkMemoryRequirements requirements;
    g_graphics.GetBufferMemoryRequirements(g_graphics.device,
                                            g_graphics.staging_buffer,
                                            &requirements);
    /* This arena is bidirectional: it supplies uploads and receives every
     * synchronous render-target/depth readback. CPU reads from write-combined
     * (non-cached) host memory are extremely slow on discrete GPUs and caused
     * tens to hundreds of milliseconds of finish time per draw. Prefer cached
     * coherent memory; fall back to any coherent host-visible type. */
    int type = memory_type(requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
        VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (type < 0)
        type = memory_type(requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type < 0) goto fail;
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = (uint32_t)type
    };
    if (g_graphics.AllocateMemory(g_graphics.device, &allocation, NULL,
                                  &g_graphics.staging_memory) != VK_SUCCESS ||
        g_graphics.BindBufferMemory(g_graphics.device,
                                    g_graphics.staging_buffer,
                                    g_graphics.staging_memory, 0) != VK_SUCCESS)
        goto fail;
    void *mapped = NULL;
    if (g_graphics.MapMemory(g_graphics.device, g_graphics.staging_memory, 0,
                             VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS ||
        !mapped)
        goto fail;
    g_graphics.staging_map = mapped;
    g_graphics.staging_capacity = capacity;
    if (getenv("BEER_D3D12_STARTUP_TIMING"))
        fprintf(stderr, "[VULKAN GRAPHICS] staging capacity=%zuMB\n",
                capacity >> 20);
    return 1;
fail:
    release_staging();
    return 0;
}

static VkFramebuffer find_or_create_framebuffer(
    VkRenderPass render_pass, const VkImageView *attachments,
    uint32_t attachment_count, uint32_t width, uint32_t height)
{
    FramebufferEntry *candidate = NULL;
    uint64_t oldest_use = UINT64_MAX;
    for (uint32_t i = 0; i < FRAMEBUFFER_CACHE_SIZE; ++i) {
        FramebufferEntry *entry = &g_graphics.framebuffer_cache[i];
        if (entry->framebuffer && entry->render_pass == render_pass &&
            entry->attachment_count == attachment_count &&
            entry->width == width && entry->height == height &&
            memcmp(entry->attachments, attachments,
                   attachment_count * sizeof(*attachments)) == 0) {
            entry->last_use = ++g_graphics.framebuffer_use_serial;
            ++g_graphics.framebuffer_cache_hits;
            return entry->framebuffer;
        }
        if (!entry->framebuffer) {
            if (!candidate) candidate = entry;
        } else if (!candidate || entry->last_use < oldest_use) {
            candidate = entry;
            oldest_use = entry->last_use;
        }
    }
    if (!candidate) return VK_NULL_HANDLE;
    if (candidate->framebuffer)
        g_graphics.DestroyFramebuffer(g_graphics.device,
                                      candidate->framebuffer, NULL);
    VkFramebufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = render_pass,
        .attachmentCount = attachment_count,
        .pAttachments = attachments,
        .width = width,
        .height = height,
        .layers = 1
    };
    memset(candidate, 0, sizeof(*candidate));
    if (g_graphics.CreateFramebuffer(g_graphics.device, &info, NULL,
                                     &candidate->framebuffer) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    candidate->render_pass = render_pass;
    candidate->attachment_count = attachment_count;
    memcpy(candidate->attachments, attachments,
           attachment_count * sizeof(*attachments));
    candidate->width = width;
    candidate->height = height;
    candidate->last_use = ++g_graphics.framebuffer_use_serial;
    ++g_graphics.framebuffer_creations;
    return candidate->framebuffer;
}

static void destroy_pipeline(PipelineEntry *entry)
{
    if (entry->pipeline)
        g_graphics.DestroyPipeline(g_graphics.device, entry->pipeline, NULL);
    if (entry->render_pass)
        g_graphics.DestroyRenderPass(g_graphics.device, entry->render_pass,
                                     NULL);
    if (entry->layout)
        g_graphics.DestroyPipelineLayout(g_graphics.device, entry->layout,
                                         NULL);
    if (entry->set_layout)
        g_graphics.DestroyDescriptorSetLayout(g_graphics.device,
                                              entry->set_layout, NULL);
    memset(entry, 0, sizeof(*entry));
}

static void release_image(ImageObject *image);

static void invalidate_framebuffers_for_view(VkImageView view)
{
    if (!view) return;
    for (uint32_t i = 0; i < FRAMEBUFFER_CACHE_SIZE; ++i) {
        FramebufferEntry *entry = &g_graphics.framebuffer_cache[i];
        if (!entry->framebuffer) continue;
        int referenced = 0;
        for (uint32_t attachment = 0;
             attachment < entry->attachment_count; ++attachment)
            referenced |= entry->attachments[attachment] == view;
        if (!referenced) continue;
        g_graphics.DestroyFramebuffer(g_graphics.device, entry->framebuffer,
                                      NULL);
        memset(entry, 0, sizeof(*entry));
    }
}

static void destroy_locked(void)
{
    if (g_graphics.device) {
        release_staging();
        for (uint32_t i = 0; i < FRAMEBUFFER_CACHE_SIZE; ++i)
            if (g_graphics.framebuffer_cache[i].framebuffer) {
                g_graphics.DestroyFramebuffer(g_graphics.device,
                    g_graphics.framebuffer_cache[i].framebuffer, NULL);
                memset(&g_graphics.framebuffer_cache[i], 0,
                       sizeof(g_graphics.framebuffer_cache[i]));
            }
        for (uint32_t i = 0; i < IMAGE_CACHE_SIZE; ++i)
            release_image(&g_graphics.image_cache[i].object);
        for (uint32_t i = 0; i < g_graphics.pipeline_count; ++i)
            destroy_pipeline(&g_graphics.pipelines[i]);
        for (uint32_t i = 0; i < g_graphics.sampler_count; ++i)
            if (g_graphics.sampler_cache[i].sampler)
                g_graphics.DestroySampler(g_graphics.device,
                    g_graphics.sampler_cache[i].sampler, NULL);
        if (g_graphics.descriptor_pool)
            g_graphics.DestroyDescriptorPool(g_graphics.device,
                                              g_graphics.descriptor_pool, NULL);
        if (g_graphics.statistics_query_pool)
            g_graphics.DestroyQueryPool(g_graphics.device,
                                        g_graphics.statistics_query_pool, NULL);
        if (g_graphics.fence)
            g_graphics.DestroyFence(g_graphics.device, g_graphics.fence, NULL);
        if (g_graphics.command_pool)
            g_graphics.DestroyCommandPool(g_graphics.device,
                                          g_graphics.command_pool, NULL);
    }
    memset(&g_graphics, 0, sizeof(g_graphics));
}

static int initialize_locked(void)
{
    if (g_graphics.initialized) return 1;
    if (g_graphics.unavailable || !beer_vulkan_context_initialize()) return 0;
    g_graphics.physical_device = beer_vulkan_physical_device();
    g_graphics.device = beer_vulkan_device();
    g_graphics.queue = beer_vulkan_queue();
    g_graphics.queue_family = beer_vulkan_queue_family();
    if (!g_graphics.device || !g_graphics.queue) goto fail;
    do {
        LOAD_INSTANCE(GetPhysicalDeviceMemoryProperties);
        LOAD_DEVICE(CreateCommandPool); LOAD_DEVICE(DestroyCommandPool);
        LOAD_DEVICE(AllocateCommandBuffers); LOAD_DEVICE(ResetCommandBuffer);
        LOAD_DEVICE(BeginCommandBuffer); LOAD_DEVICE(EndCommandBuffer);
        LOAD_DEVICE(CreateFence); LOAD_DEVICE(DestroyFence);
        LOAD_DEVICE(ResetFences); LOAD_DEVICE(WaitForFences);
        LOAD_DEVICE(CreateDescriptorPool); LOAD_DEVICE(DestroyDescriptorPool);
        LOAD_DEVICE(ResetDescriptorPool); LOAD_DEVICE(AllocateDescriptorSets);
        LOAD_DEVICE(UpdateDescriptorSets);
        LOAD_DEVICE(CreateDescriptorSetLayout);
        LOAD_DEVICE(DestroyDescriptorSetLayout);
        LOAD_DEVICE(CreateQueryPool); LOAD_DEVICE(DestroyQueryPool);
        LOAD_DEVICE(CmdResetQueryPool); LOAD_DEVICE(CmdBeginQuery);
        LOAD_DEVICE(CmdEndQuery); LOAD_DEVICE(GetQueryPoolResults);
        LOAD_DEVICE(CreatePipelineLayout); LOAD_DEVICE(DestroyPipelineLayout);
        LOAD_DEVICE(CreateShaderModule); LOAD_DEVICE(DestroyShaderModule);
        LOAD_DEVICE(CreateRenderPass); LOAD_DEVICE(DestroyRenderPass);
        LOAD_DEVICE(CreateGraphicsPipelines); LOAD_DEVICE(DestroyPipeline);
        LOAD_DEVICE(CreateFramebuffer); LOAD_DEVICE(DestroyFramebuffer);
        LOAD_DEVICE(CreateBufferView); LOAD_DEVICE(DestroyBufferView);
        LOAD_DEVICE(CreateBuffer); LOAD_DEVICE(DestroyBuffer);
        LOAD_DEVICE(GetBufferMemoryRequirements);
        LOAD_DEVICE(AllocateMemory); LOAD_DEVICE(FreeMemory);
        LOAD_DEVICE(BindBufferMemory); LOAD_DEVICE(MapMemory);
        LOAD_DEVICE(UnmapMemory); LOAD_DEVICE(CreateImage);
        LOAD_DEVICE(DestroyImage); LOAD_DEVICE(GetImageMemoryRequirements);
        LOAD_DEVICE(BindImageMemory); LOAD_DEVICE(CreateImageView);
        LOAD_DEVICE(DestroyImageView); LOAD_DEVICE(CreateSampler);
        LOAD_DEVICE(DestroySampler); LOAD_DEVICE(CmdCopyBufferToImage);
        LOAD_DEVICE(CmdCopyImageToBuffer); LOAD_DEVICE(CmdClearDepthStencilImage);
        LOAD_DEVICE(CmdPipelineBarrier);
        LOAD_DEVICE(CmdBeginRenderPass); LOAD_DEVICE(CmdEndRenderPass);
        LOAD_DEVICE(CmdBindPipeline); LOAD_DEVICE(CmdBindDescriptorSets);
        LOAD_DEVICE(CmdSetViewport); LOAD_DEVICE(CmdSetScissor);
        LOAD_DEVICE(CmdSetBlendConstants);
        LOAD_DEVICE(CmdBindVertexBuffers); LOAD_DEVICE(CmdBindIndexBuffer);
        LOAD_DEVICE(CmdDraw); LOAD_DEVICE(CmdDrawIndexed);
        LOAD_DEVICE(QueueSubmit);
    } while (0);
    g_graphics.GetPhysicalDeviceMemoryProperties(g_graphics.physical_device,
                                                 &g_graphics.memory_properties);
    VkCommandPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = g_graphics.queue_family
    };
    if (g_graphics.CreateCommandPool(g_graphics.device, &pool, NULL,
                                     &g_graphics.command_pool) != VK_SUCCESS)
        goto fail;
    VkCommandBufferAllocateInfo command = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = g_graphics.command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };
    if (g_graphics.AllocateCommandBuffers(g_graphics.device, &command,
                                          &g_graphics.command_buffer) !=
        VK_SUCCESS)
        goto fail;
    VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (g_graphics.CreateFence(g_graphics.device, &fence, NULL,
                               &g_graphics.fence) != VK_SUCCESS)
        goto fail;
    /* Descriptor layouts are immutable pipeline state. Retain one descriptor
     * set per cached pipeline instead of resetting and reallocating the pool
     * for every scalar draw. Updates remain draw-local and fence completion
     * guarantees a set is no longer in use before its next update. */
    VkDescriptorPoolSize sizes[5] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
         BEER_VULKAN_GRAPHICS_MAX_BINDINGS * MAX_PIPELINES},
        {VK_DESCRIPTOR_TYPE_SAMPLER,
         BEER_VULKAN_GRAPHICS_MAX_BINDINGS * MAX_PIPELINES},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
         BEER_VULKAN_GRAPHICS_MAX_BINDINGS * MAX_PIPELINES},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         BEER_VULKAN_GRAPHICS_MAX_BINDINGS * MAX_PIPELINES},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
         BEER_VULKAN_GRAPHICS_MAX_BINDINGS * MAX_PIPELINES}
    };
    VkDescriptorPoolCreateInfo descriptor_pool = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = MAX_PIPELINES,
        .poolSizeCount = 5,
        .pPoolSizes = sizes
    };
    if (g_graphics.CreateDescriptorPool(g_graphics.device, &descriptor_pool,
                                        NULL, &g_graphics.descriptor_pool) !=
        VK_SUCCESS)
        goto fail;
    if (getenv("BEER_VULKAN_DRAW_PROVENANCE") &&
        beer_vulkan_enabled_features()->pipelineStatisticsQuery) {
        VkQueryPoolCreateInfo query_pool = {
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS,
            .queryCount = 1,
            .pipelineStatistics =
                VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT |
                VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
                VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
                VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
                VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT
        };
        if (g_graphics.CreateQueryPool(g_graphics.device, &query_pool, NULL,
                                       &g_graphics.statistics_query_pool) !=
            VK_SUCCESS)
            fprintf(stderr,
                    "[VULKAN DRAW] pipeline statistics unavailable\n");
    }
    g_graphics.initialized = 1;
    return 1;
fail:
    destroy_locked();
    g_graphics.unavailable = 1;
    return 0;
}

static const char *create_image(uint32_t format, uint32_t width,
                                uint32_t height, uint32_t layers,
                                uint32_t depth, uint32_t dimension,
                                uint32_t cube, uint32_t mip_levels,
                                uint32_t sample_count, VkImageUsageFlags usage,
                                VkImageAspectFlags aspect, ImageObject *image)
{
    if (!layers) layers = 1;
    if (!depth) depth = 1;
    if (!dimension) dimension = 2;
    if (!mip_levels) mip_levels = 1;
    if (!sample_count) sample_count = 1;
    if ((sample_count & (sample_count - 1u)) || sample_count > 64u ||
        (sample_count > 1 && (dimension != 2 || layers != 1 || depth != 1 ||
                              mip_levels != 1 || cube)))
        return "graphics multisample image shape unsupported";
    if (dimension < 1 || dimension > 3 ||
        (dimension == 3 && layers > 1) ||
        (dimension != 3 && depth > 1) ||
        (dimension == 1 && height != 1))
        return "graphics image shape unsupported";
    VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = dimension == 1 ? VK_IMAGE_TYPE_1D :
            dimension == 3 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
        .format = (VkFormat)format,
        .extent = {width, height, depth},
        .mipLevels = mip_levels,
        .arrayLayers = layers,
        .samples = (VkSampleCountFlagBits)sample_count,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };
    if (g_graphics.CreateImage(g_graphics.device, &info, NULL,
                               &image->image) != VK_SUCCESS)
        return "image creation failed";
    VkMemoryRequirements requirements;
    g_graphics.GetImageMemoryRequirements(g_graphics.device, image->image,
                                          &requirements);
    if (requirements.size > SIZE_MAX) return "image allocation too large";
    image->allocation_size = (size_t)requirements.size;
    int type = memory_type(requirements.memoryTypeBits,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) type = memory_type(requirements.memoryTypeBits, 0);
    if (type < 0) return "image memory type unavailable";
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = (uint32_t)type
    };
    if (g_graphics.AllocateMemory(g_graphics.device, &allocation, NULL,
                                  &image->memory) != VK_SUCCESS ||
        g_graphics.BindImageMemory(g_graphics.device, image->image,
                                   image->memory, 0) != VK_SUCCESS)
        return "image memory allocation failed";
    VkImageViewCreateInfo view = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image->image,
        .viewType = cube ?
            (layers > 6 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY :
                          VK_IMAGE_VIEW_TYPE_CUBE) :
            dimension == 1 ?
            (layers > 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D) :
            dimension == 3 ? VK_IMAGE_VIEW_TYPE_3D :
            layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
        .format = (VkFormat)format,
        .subresourceRange = {
            .aspectMask = aspect,
            .levelCount = mip_levels,
            .layerCount = layers
        }
    };
    if (g_graphics.CreateImageView(g_graphics.device, &view, NULL,
                                   &image->view) != VK_SUCCESS)
        return "image view creation failed";
    image->aspect = aspect;
    image->usage = usage;
    image->sample_count = sample_count;
    return NULL;
}

static void release_image(ImageObject *image)
{
    if (image->view) {
        invalidate_framebuffers_for_view(image->view);
        g_graphics.DestroyImageView(g_graphics.device, image->view, NULL);
    }
    if (image->image)
        g_graphics.DestroyImage(g_graphics.device, image->image, NULL);
    if (image->memory)
        g_graphics.FreeMemory(g_graphics.device, image->memory, NULL);
    memset(image, 0, sizeof(*image));
}

static void cache_census(const char *tag)
{
    uint32_t n = 0;
    size_t sum = 0;
    for (uint32_t i = 0; i < IMAGE_CACHE_SIZE; ++i) {
        const CachedImage *e = &g_graphics.image_cache[i];
        if (!e->object.image) continue;
        ++n;
        sum += e->object.allocation_size;
    }
    fprintf(stderr, "[VULKAN CACHE] census %s entries=%u sum=%zu counter=%zu\n",
            tag, n, sum, g_graphics.image_cache_bytes);
}

static int cached_target_matches(const BeerVulkanGraphicsTarget *target)
{
    if (!target || !target->resource || !target->serial) return 0;
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    for (uint32_t i = 0; i < IMAGE_CACHE_SIZE; ++i) {
        const CachedImage *entry = &g_graphics.image_cache[i];
        if (entry->object.image && entry->format == target->format &&
            entry->width == target->width && entry->height == target->height &&
            entry->layers == 1 && entry->depth == 1 &&
            entry->dimension == 2 && entry->mip_levels == 1 &&
            entry->usage == usage &&
            entry->aspect == VK_IMAGE_ASPECT_COLOR_BIT &&
            entry->object.content_valid &&
            entry->object.resource == target->resource &&
            entry->object.serial == *target->serial &&
            entry->object.layout != VK_IMAGE_LAYOUT_UNDEFINED)
            return 1;
    }
    if ((size_t)target->width * target->height >= 1024u * 1024u &&
        getenv("BEER_VULKAN_CACHE_TRACE")) {
        int any = 0;
        for (uint32_t i = 0; i < IMAGE_CACHE_SIZE; ++i) {
            const CachedImage *e = &g_graphics.image_cache[i];
            if (!e->object.image || e->object.resource != target->resource)
                continue;
            any = 1;
            fprintf(stderr,
                    "[VULKAN CACHE] target-miss resource=%p tser=%llu "
                    "eser=%llu fmt=%d wh=%d use=%d valid=%d layout=%d\n",
                    target->resource, (unsigned long long)*target->serial,
                    (unsigned long long)e->object.serial,
                    e->format != target->format,
                    e->width != target->width || e->height != target->height,
                    e->usage != usage, !e->object.content_valid,
                    (int)e->object.layout);
        }
        if (!any) {
            fprintf(stderr, "[VULKAN CACHE] target-miss resource=%p tser=%llu "
                    "not-cached\n", target->resource,
                    (unsigned long long)*target->serial);
            cache_census("plan");
        }
    }
    return 0;
}

static int cached_image_matches_binding(
    const BeerVulkanGraphicsBinding *binding)
{
    if (!binding || !binding->resource) return 0;
    uint32_t layers = binding->layer_count ? binding->layer_count : 1;
    uint32_t depth = binding->depth ? binding->depth : 1;
    uint32_t dimension = binding->image_dimension ? binding->image_dimension : 2;
    uint32_t mip_levels = binding->mip_levels ? binding->mip_levels : 1;
    uint32_t cube = binding->image_cube ? 1u : 0u;
    uint32_t sample_count = binding->sample_count ? binding->sample_count : 1;
    VkImageAspectFlags aspect = binding->kind ==
        BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_DEPTH ?
        VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    uint64_t binding_serial = binding->serial ? *binding->serial : UINT64_MAX;
    int found_match = 0;
    for (uint32_t i = 0; i < IMAGE_CACHE_SIZE; ++i) {
        const CachedImage *entry = &g_graphics.image_cache[i];
        if (entry->object.image && entry->format == binding->format &&
            entry->width == binding->width && entry->height == binding->height &&
            entry->layers == layers && entry->depth == depth &&
            entry->dimension == dimension && entry->cube == cube &&
            entry->mip_levels == mip_levels &&
            entry->sample_count == sample_count &&
            (entry->usage & usage) == usage &&
            entry->aspect == aspect && entry->object.content_valid &&
            entry->object.resource == binding->resource &&
            entry->object.serial == binding_serial &&
            entry->object.layout != VK_IMAGE_LAYOUT_UNDEFINED) {
            found_match = 1;
            break;
        }
    }
    if (!found_match && binding->size >= 16u * 1024u * 1024u &&
        getenv("BEER_VULKAN_CACHE_TRACE")) {
        int any = 0;
        for (uint32_t i = 0; i < IMAGE_CACHE_SIZE; ++i) {
            const CachedImage *e = &g_graphics.image_cache[i];
            if (!e->object.image || e->object.resource != binding->resource)
                continue;
            any = 1;
            fprintf(stderr,
                    "[VULKAN CACHE] miss resource=%p bser=%llu eser=%llu "
                    "mask fmt=%d wh=%d lay=%d dep=%d dim=%d cube=%d mip=%d "
                    "sc=%d use=%d asp=%d valid=%d layout=%d\n",
                    binding->resource, (unsigned long long)binding_serial,
                    (unsigned long long)e->object.serial,
                    e->format != binding->format,
                    e->width != binding->width || e->height != binding->height,
                    e->layers != layers, e->depth != depth,
                    e->dimension != dimension, e->cube != cube,
                    e->mip_levels != mip_levels,
                    e->sample_count != sample_count,
                    (e->usage & usage) != usage, e->aspect != aspect,
                    !e->object.content_valid,
                    (int)e->object.layout);
        }
        if (!any)
            fprintf(stderr, "[VULKAN CACHE] miss resource=%p bser=%llu "
                    "not-cached\n", binding->resource,
                    (unsigned long long)binding_serial);
    }
    if (getenv("BEER_D3D12_DIAGNOSTICS")) {
        static uint32_t call_count = 0;
        if (++call_count <= 20) {
            fprintf(stderr, "[CACHE MATCH] resource=%p serial_ptr=%p serial_val=%llu match=%d\n",
                binding->resource, (const void *)binding->serial, 
                (unsigned long long)binding_serial, found_match);
        }
    }
    return found_match;
}

static const char *acquire_cached_image(uint32_t format, uint32_t width,
                                        uint32_t height, uint32_t layers,
                                        uint32_t depth, uint32_t dimension,
                                        uint32_t cube, uint32_t mip_levels,
                                        uint32_t sample_count,
                                        VkImageUsageFlags usage,
                                        VkImageAspectFlags aspect,
                                        const void *resource, uint64_t serial,
                                        ImageObject *image)
{
    CachedImage *candidate = NULL;
    CachedImage *shape_match = NULL;
    uint64_t oldest_use = UINT64_MAX;
    if (!layers) layers = 1;
    if (!depth) depth = 1;
    if (!dimension) dimension = 2;
    if (!mip_levels) mip_levels = 1;
    if (!sample_count) sample_count = 1;
    for (uint32_t i = 0; i < IMAGE_CACHE_SIZE; ++i) {
        CachedImage *entry = &g_graphics.image_cache[i];
        if (entry->object.image && entry->format == format &&
            entry->width == width && entry->height == height &&
            entry->layers == layers && entry->depth == depth &&
            entry->dimension == dimension && entry->cube == cube &&
            entry->mip_levels == mip_levels &&
            (entry->usage & usage) == usage &&
            entry->aspect == aspect) {
            if (resource && entry->object.content_valid &&
                entry->object.resource == resource &&
                entry->object.serial == serial) {
                *image = entry->object;
                if (entry->object.allocation_size <=
                    g_graphics.image_cache_bytes)
                    g_graphics.image_cache_bytes -=
                        entry->object.allocation_size;
                if (getenv("BEER_VULKAN_CACHE_TRACE") &&
                    image->allocation_size >= 16u * 1024u * 1024u)
                    fprintf(stderr,
                            "[VULKAN CACHE] hit resource=%p serial=%llu "
                            "allocation=%zu cached=%zu\n",
                            resource, (unsigned long long)serial,
                            image->allocation_size,
                            g_graphics.image_cache_bytes);
                memset(&entry->object, 0, sizeof(entry->object));
                memset(entry, 0, sizeof(*entry));
                return NULL;
            }
            /* Only anonymous mirrors are interchangeable. Taking a
             * resource-bound entry here destroyed serial-valid SRV/target
             * mirrors of the same shape on every anonymous full-screen
             * target and forced a host round trip on the next use. */
            if (!shape_match && !resource && !entry->object.resource)
                shape_match = entry;
        }
        if (!entry->object.image) {
            if (!candidate) candidate = entry;
        } else if (!candidate &&
                   !(entry->object.resource && entry->object.serial == 0) &&
                   entry->last_use < oldest_use) {
            /* Serial-zero resource mirrors are immutable until the frontend
             * explicitly advances their serial. Prefer recycling dynamic frame
             * intermediates so large static texture arrays survive between
             * consecutive draws instead of cycling through host staging. */
            candidate = entry;
            oldest_use = entry->last_use;
        }
    }
    /* Do not steal another resource's persistent mirror merely because its
     * shape matches. That would evict a useful immutable SRV and force both
     * resources to be uploaded again. Anonymous targets can still reuse any
     * shape-compatible object. */
    if (shape_match && !resource) {
        *image = shape_match->object;
        if (shape_match->object.allocation_size <=
            g_graphics.image_cache_bytes)
            g_graphics.image_cache_bytes -=
                shape_match->object.allocation_size;
        image->content_valid = 0;
        image->resource = NULL;
        image->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        memset(&shape_match->object, 0, sizeof(shape_match->object));
        memset(shape_match, 0, sizeof(*shape_match));
        return NULL;
    }
    (void)candidate;
    return create_image(format, width, height, layers, depth, dimension, cube,
                        mip_levels, sample_count, usage, aspect, image);
}

static void recycle_cached_image(uint32_t format, uint32_t width,
                                 uint32_t height, uint32_t layers,
                                 uint32_t depth, uint32_t dimension,
                                 uint32_t cube, uint32_t mip_levels,
                                 uint32_t sample_count,
                                 VkImageUsageFlags usage,
                                 VkImageAspectFlags aspect,
                                 const void *resource, uint64_t serial,
                                 ImageObject *image)
{
    if (!image || !image->image) return;
    (void)usage;
    /* Serials only advance, so an older serial of the same resource can never
     * hit again. Release those mirrors now instead of letting them consume
     * identity slots and byte budget until LRU eviction. */
    if (resource && serial != UINT64_MAX) {
        for (uint32_t i = 0; i < IMAGE_CACHE_SIZE; ++i) {
            CachedImage *stale = &g_graphics.image_cache[i];
            if (!stale->object.image || stale->object.resource != resource ||
                stale->object.serial >= serial)
                continue;
            if (stale->object.allocation_size >= 16u * 1024u * 1024u &&
                getenv("BEER_VULKAN_CACHE_TRACE"))
                fprintf(stderr, "[VULKAN CACHE] evict-stale resource=%p "
                        "old=%llu new=%llu\n", resource,
                        (unsigned long long)stale->object.serial,
                        (unsigned long long)serial);
            if (stale->object.allocation_size <= g_graphics.image_cache_bytes)
                g_graphics.image_cache_bytes -= stale->object.allocation_size;
            release_image(&stale->object);
            memset(stale, 0, sizeof(*stale));
        }
    }
    CachedImage *candidate = NULL;
    CachedImage *oldest_immutable = NULL;
    uint64_t oldest_use = UINT64_MAX;
    uint64_t oldest_immutable_use = UINT64_MAX;
    for (uint32_t i = 0; i < IMAGE_CACHE_SIZE; ++i) {
        CachedImage *entry = &g_graphics.image_cache[i];
        if (!entry->object.image) {
            candidate = entry;
            break;
        }
        if (entry->object.resource && entry->object.serial == 0) {
            if (entry->last_use < oldest_immutable_use) {
                oldest_immutable = entry;
                oldest_immutable_use = entry->last_use;
            }
        } else if (entry->last_use < oldest_use) {
            candidate = entry;
            oldest_use = entry->last_use;
        }
    }
    /* Serial zero means unchanged, not immortal. When every identity slot is
     * occupied, replace the globally least-recently-used mirror. Always
     * preferring dynamic entries evicted the images stored by the previous
     * draw (the cache holds hundreds of small immutable textures, so only a
     * handful of slots are dynamic) and forced a host upload per draw. */
    if (!candidate || candidate->object.image) {
        if (oldest_immutable &&
            (!candidate || oldest_immutable_use < oldest_use))
            candidate = oldest_immutable;
    }
    if (!candidate) {
        release_image(image);
        return;
    }
    if (image->allocation_size > IMAGE_CACHE_MAX_ENTRY_BYTES) {
        if (getenv("BEER_VULKAN_CACHE_TRACE"))
            fprintf(stderr,
                    "[VULKAN CACHE] drop-too-large resource=%p serial=%llu "
                    "allocation=%zu limit=%zu\n",
                    resource, (unsigned long long)serial,
                    image->allocation_size, IMAGE_CACHE_MAX_ENTRY_BYTES);
        release_image(image);
        return;
    }
    while (g_graphics.image_cache_bytes >
           IMAGE_CACHE_MAX_BYTES - image->allocation_size) {
        CachedImage *oldest_dynamic = NULL;
        CachedImage *oldest_immutable_budget = NULL;
        uint64_t oldest_dynamic_use = UINT64_MAX;
        uint64_t oldest_immutable_budget_use = UINT64_MAX;
        for (uint32_t i = 0; i < IMAGE_CACHE_SIZE; ++i) {
            CachedImage *entry = &g_graphics.image_cache[i];
            if (entry == candidate || !entry->object.image) continue;
            if (entry->object.resource && entry->object.serial == 0) {
                if (entry->last_use < oldest_immutable_budget_use) {
                    oldest_immutable_budget = entry;
                    oldest_immutable_budget_use = entry->last_use;
                }
            } else if (entry->last_use < oldest_dynamic_use) {
                oldest_dynamic = entry;
                oldest_dynamic_use = entry->last_use;
            }
        }
        /* Immutable means reusable while its serial remains zero, not pinned.
         * Prefer discarding dynamic frame intermediates, but allow the oldest
         * immutable mirror to leave under hard byte pressure. Otherwise a
         * cache filled by old serial-zero assets rejects every current-frame
         * image and turns the byte cap into permanent upload thrashing. */
        CachedImage *oldest = oldest_dynamic ? oldest_dynamic :
                              oldest_immutable_budget;
        if (!oldest) {
            if (getenv("BEER_VULKAN_CACHE_TRACE"))
                fprintf(stderr,
                        "[VULKAN CACHE] drop-budget resource=%p serial=%llu "
                        "allocation=%zu cached=%zu budget=%zu\n",
                        resource, (unsigned long long)serial,
                        image->allocation_size, g_graphics.image_cache_bytes,
                        IMAGE_CACHE_MAX_BYTES);
            release_image(image);
            return;
        }
        if (oldest->object.allocation_size >= 16u * 1024u * 1024u &&
            getenv("BEER_VULKAN_CACHE_TRACE"))
            fprintf(stderr, "[VULKAN CACHE] evict-budget resource=%p "
                    "serial=%llu for=%p\n", oldest->object.resource,
                    (unsigned long long)oldest->object.serial, resource);
        if (oldest->object.allocation_size <= g_graphics.image_cache_bytes)
            g_graphics.image_cache_bytes -= oldest->object.allocation_size;
        release_image(&oldest->object);
        memset(oldest, 0, sizeof(*oldest));
    }
    if (candidate->object.image &&
        candidate->object.allocation_size >= 16u * 1024u * 1024u &&
        getenv("BEER_VULKAN_CACHE_TRACE"))
        fprintf(stderr, "[VULKAN CACHE] evict-slot resource=%p serial=%llu "
                "for=%p\n", candidate->object.resource,
                (unsigned long long)candidate->object.serial, resource);
    if (candidate->object.allocation_size <= g_graphics.image_cache_bytes)
        g_graphics.image_cache_bytes -= candidate->object.allocation_size;
    release_image(&candidate->object);
    candidate->format = format;
    candidate->width = width;
    candidate->height = height;
    candidate->layers = layers ? layers : 1;
    candidate->depth = depth ? depth : 1;
    candidate->dimension = dimension ? dimension : 2;
    candidate->cube = cube ? 1u : 0u;
    candidate->mip_levels = mip_levels ? mip_levels : 1;
    candidate->sample_count = sample_count ? sample_count : 1;
    /* Preserve the allocation's complete usage contract. A render target may
     * subsequently become an SRV; narrowing this metadata to the caller's
     * current role would make the same Vulkan image unusable as a target on
     * the following pass and reintroduce a full host round trip. */
    candidate->usage = image->usage;
    candidate->aspect = aspect;
    candidate->last_use = ++g_graphics.image_use_serial;
    image->resource = resource;
    image->serial = serial;
    candidate->object = *image;
    g_graphics.image_cache_bytes += candidate->object.allocation_size;
    if (getenv("BEER_VULKAN_CACHE_TRACE") &&
        candidate->object.allocation_size >= 16u * 1024u * 1024u)
        fprintf(stderr,
                "[VULKAN CACHE] store resource=%p serial=%llu allocation=%zu "
                "cached=%zu usage=%x\n",
                resource, (unsigned long long)serial,
                candidate->object.allocation_size,
                g_graphics.image_cache_bytes, candidate->usage);
    memset(image, 0, sizeof(*image));
}

static PipelineEntry *find_or_create_pipeline(
    const void *vertex_key, const uint32_t *vertex_spirv,
    size_t vertex_size, const void *pixel_key, const uint32_t *pixel_spirv,
    size_t pixel_size, const BeerVulkanGraphicsBinding *bindings,
    uint32_t binding_count, const BeerVulkanGraphicsDrawInfo *draw_info,
    const BeerVulkanGraphicsState *state, const uint32_t *target_formats,
    uint32_t target_count, uint32_t depth_format)
{
    const size_t state_identity_size =
        offsetof(BeerVulkanGraphicsState, blend_constants);
    for (uint32_t i = 0; i < g_graphics.pipeline_count; ++i) {
        PipelineEntry *entry = &g_graphics.pipelines[i];
        uint32_t vertex_binding_count = draw_info ?
            draw_info->vertex_binding_count : 0;
        uint32_t vertex_attribute_count = draw_info ?
            draw_info->vertex_attribute_count : 0;
        uint32_t topology = draw_info ? draw_info->topology :
            VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        if (entry->vertex_key != vertex_key || entry->pixel_key != pixel_key ||
            entry->target_count != target_count ||
            memcmp(entry->target_formats, target_formats,
                   target_count * sizeof(*target_formats)) != 0 ||
            entry->depth_format != depth_format ||
            entry->binding_count != binding_count ||
            entry->vertex_binding_count != vertex_binding_count ||
            entry->vertex_attribute_count != vertex_attribute_count ||
            entry->topology != topology ||
            memcmp(&entry->state, state, state_identity_size) != 0)
            continue;
        int compatible = 1;
        for (uint32_t b = 0; b < binding_count; ++b)
            if (entry->bindings[b] != bindings[b].binding ||
                entry->binding_kinds[b] != bindings[b].kind ||
                entry->binding_stages[b] != bindings[b].stage_flags)
                compatible = 0;
        if (draw_info) {
            for (uint32_t b = 0; b < vertex_binding_count; ++b)
                if (entry->vertex_bindings[b].stride !=
                        draw_info->vertex_bindings[b].stride ||
                    entry->vertex_bindings[b].input_rate !=
                        draw_info->vertex_bindings[b].input_rate)
                    compatible = 0;
            for (uint32_t a = 0; a < vertex_attribute_count; ++a)
                if (memcmp(&entry->vertex_attributes[a],
                           &draw_info->vertex_attributes[a],
                           sizeof(entry->vertex_attributes[a])) != 0)
                    compatible = 0;
        }
        if (compatible) return entry;
    }
    if (g_graphics.pipeline_count >= MAX_PIPELINES) return NULL;
    PipelineEntry *entry = &g_graphics.pipelines[g_graphics.pipeline_count++];
    memset(entry, 0, sizeof(*entry));
    entry->vertex_key = vertex_key;
    entry->pixel_key = pixel_key;
    entry->target_count = target_count;
    memcpy(entry->target_formats, target_formats,
           target_count * sizeof(*target_formats));
    entry->depth_format = depth_format;
    entry->binding_count = binding_count;
    entry->vertex_binding_count = draw_info ?
        draw_info->vertex_binding_count : 0;
    entry->vertex_attribute_count = draw_info ?
        draw_info->vertex_attribute_count : 0;
    entry->topology = draw_info ? draw_info->topology :
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    entry->state = *state;
    if (draw_info) {
        memcpy(entry->vertex_bindings, draw_info->vertex_bindings,
               sizeof(entry->vertex_bindings));
        memcpy(entry->vertex_attributes, draw_info->vertex_attributes,
               sizeof(entry->vertex_attributes));
    }
    VkDescriptorSetLayoutBinding layouts[BEER_VULKAN_GRAPHICS_MAX_BINDINGS];
    for (uint32_t i = 0; i < binding_count; ++i) {
        entry->bindings[i] = bindings[i].binding;
        entry->binding_kinds[i] = bindings[i].kind;
        entry->binding_stages[i] = bindings[i].stage_flags;
        VkDescriptorType descriptor_type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        if (bindings[i].kind == BEER_VULKAN_GRAPHICS_BINDING_SAMPLER)
            descriptor_type = VK_DESCRIPTOR_TYPE_SAMPLER;
        else if (bindings[i].kind ==
                 BEER_VULKAN_GRAPHICS_BINDING_UNIFORM_BUFFER)
            descriptor_type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        else if (bindings[i].kind ==
                 BEER_VULKAN_GRAPHICS_BINDING_STORAGE_BUFFER)
            descriptor_type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        else if (bindings[i].kind ==
                 BEER_VULKAN_GRAPHICS_BINDING_UNIFORM_TEXEL_BUFFER)
            descriptor_type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        VkShaderStageFlags stages = 0;
        if (bindings[i].stage_flags & BEER_VULKAN_GRAPHICS_STAGE_VERTEX)
            stages |= VK_SHADER_STAGE_VERTEX_BIT;
        if (bindings[i].stage_flags & BEER_VULKAN_GRAPHICS_STAGE_FRAGMENT)
            stages |= VK_SHADER_STAGE_FRAGMENT_BIT;
        layouts[i] = (VkDescriptorSetLayoutBinding){
            .binding = bindings[i].binding,
            .descriptorType = descriptor_type,
            .descriptorCount = 1,
            .stageFlags = stages
        };
    }
    VkDescriptorSetLayoutCreateInfo set = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = binding_count,
        .pBindings = layouts
    };
    if (g_graphics.CreateDescriptorSetLayout(g_graphics.device, &set, NULL,
                                             &entry->set_layout) != VK_SUCCESS) {
        entry->failure = "descriptor layout creation failed";
        return entry;
    }
    VkPipelineLayoutCreateInfo layout = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &entry->set_layout
    };
    if (g_graphics.CreatePipelineLayout(g_graphics.device, &layout, NULL,
                                        &entry->layout) != VK_SUCCESS) {
        entry->failure = "pipeline layout creation failed";
        return entry;
    }
    VkAttachmentDescription attachments[BEER_VULKAN_GRAPHICS_MAX_TARGETS + 1];
    VkAttachmentReference references[BEER_VULKAN_GRAPHICS_MAX_TARGETS];
    for (uint32_t i = 0; i < target_count; ++i) {
        attachments[i] = (VkAttachmentDescription){
            .format = (VkFormat)target_formats[i],
            .samples = (VkSampleCountFlagBits)state->sample_count,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
        };
        references[i] = (VkAttachmentReference){
            .attachment = i,
            .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
        };
    }
    VkAttachmentReference depth_reference = {
        .attachment = target_count,
        .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    };
    if (depth_format) {
        attachments[target_count] = (VkAttachmentDescription){
            .format = (VkFormat)depth_format,
            .samples = (VkSampleCountFlagBits)state->sample_count,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = state->stencil_test_enable ?
                VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = state->stencil_test_enable ?
                VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
        };
    }
    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = target_count,
        .pColorAttachments = references,
        .pDepthStencilAttachment = depth_format ? &depth_reference : NULL
    };
    VkRenderPassCreateInfo render_pass = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = target_count + (depth_format ? 1u : 0u),
        .pAttachments = attachments,
        .subpassCount = 1,
        .pSubpasses = &subpass
    };
    if (g_graphics.CreateRenderPass(g_graphics.device, &render_pass, NULL,
                                    &entry->render_pass) != VK_SUCCESS) {
        entry->failure = "render pass creation failed";
        return entry;
    }
    VkShaderModule modules[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkShaderModuleCreateInfo module = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO
    };
    module.codeSize = vertex_size; module.pCode = vertex_spirv;
    if (g_graphics.CreateShaderModule(g_graphics.device, &module, NULL,
                                      &modules[0]) != VK_SUCCESS) {
        entry->failure = "vertex module rejected";
        goto modules_done;
    }
    if (pixel_spirv && pixel_size) {
        module.codeSize = pixel_size; module.pCode = pixel_spirv;
        if (g_graphics.CreateShaderModule(g_graphics.device, &module, NULL,
                                          &modules[1]) != VK_SUCCESS) {
            entry->failure = "pixel module rejected";
            goto modules_done;
        }
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = modules[0],
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = modules[1],
         .pName = "main"}
    };
    VkVertexInputBindingDescription vertex_bindings[
        BEER_VULKAN_GRAPHICS_MAX_VERTEX_BINDINGS];
    VkVertexInputAttributeDescription vertex_attributes[
        BEER_VULKAN_GRAPHICS_MAX_VERTEX_ATTRIBUTES];
    for (uint32_t i = 0; i < entry->vertex_binding_count; ++i) {
        vertex_bindings[i] = (VkVertexInputBindingDescription){
            .binding = i,
            .stride = entry->vertex_bindings[i].stride,
            .inputRate = entry->vertex_bindings[i].input_rate ?
                VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX
        };
    }
    for (uint32_t i = 0; i < entry->vertex_attribute_count; ++i) {
        vertex_attributes[i] = (VkVertexInputAttributeDescription){
            .location = entry->vertex_attributes[i].location,
            .binding = entry->vertex_attributes[i].binding,
            .format = (VkFormat)entry->vertex_attributes[i].format,
            .offset = entry->vertex_attributes[i].offset
        };
    }
    VkPipelineVertexInputStateCreateInfo vertex = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = entry->vertex_binding_count,
        .pVertexBindingDescriptions = vertex_bindings,
        .vertexAttributeDescriptionCount = entry->vertex_attribute_count,
        .pVertexAttributeDescriptions = vertex_attributes
    };
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = (VkPrimitiveTopology)entry->topology,
        .primitiveRestartEnable = entry->state.primitive_restart_enable ?
            VK_TRUE : VK_FALSE
    };
    VkPipelineViewportStateCreateInfo viewport = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1
    };
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = (VkCullModeFlags)entry->state.cull_mode,
        .frontFace = entry->state.front_counter_clockwise ?
            VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE,
        .lineWidth = 1.0f
    };
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = (VkSampleCountFlagBits)
            (entry->state.sample_count ? entry->state.sample_count : 1u)
    };
    VkPipelineColorBlendAttachmentState blend_attachments[
        BEER_VULKAN_GRAPHICS_MAX_TARGETS];
    for (uint32_t i = 0; i < target_count; ++i) {
        blend_attachments[i] = (VkPipelineColorBlendAttachmentState){
            .blendEnable = entry->state.blend_enable ? VK_TRUE : VK_FALSE,
            .srcColorBlendFactor = (VkBlendFactor)entry->state.src_color_factor,
            .dstColorBlendFactor = (VkBlendFactor)entry->state.dst_color_factor,
            .colorBlendOp = (VkBlendOp)entry->state.color_op,
            .srcAlphaBlendFactor = (VkBlendFactor)entry->state.src_alpha_factor,
            .dstAlphaBlendFactor = (VkBlendFactor)entry->state.dst_alpha_factor,
            .alphaBlendOp = (VkBlendOp)entry->state.alpha_op,
            .colorWriteMask = (VkColorComponentFlags)entry->state.write_mask
        };
    }
    VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = target_count,
        .pAttachments = blend_attachments
    };
    VkPipelineDepthStencilStateCreateInfo depth_stencil = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = entry->state.depth_test_enable ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = entry->state.depth_write_enable ? VK_TRUE : VK_FALSE,
        .depthCompareOp = (VkCompareOp)entry->state.depth_compare_op,
        .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable = entry->state.stencil_test_enable ? VK_TRUE : VK_FALSE,
        .front = {
            .failOp = (VkStencilOp)entry->state.stencil_front_fail,
            .passOp = (VkStencilOp)entry->state.stencil_front_pass,
            .depthFailOp = (VkStencilOp)entry->state.stencil_front_depth_fail,
            .compareOp = (VkCompareOp)entry->state.stencil_front_compare,
            .compareMask = entry->state.stencil_read_mask,
            .writeMask = entry->state.stencil_write_mask,
            .reference = entry->state.stencil_reference
        },
        .back = {
            .failOp = (VkStencilOp)entry->state.stencil_back_fail,
            .passOp = (VkStencilOp)entry->state.stencil_back_pass,
            .depthFailOp = (VkStencilOp)entry->state.stencil_back_depth_fail,
            .compareOp = (VkCompareOp)entry->state.stencil_back_compare,
            .compareMask = entry->state.stencil_read_mask,
            .writeMask = entry->state.stencil_write_mask,
            .reference = entry->state.stencil_reference
        }
    };
    VkDynamicState dynamic_states[3] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS
    };
    VkPipelineDynamicStateCreateInfo dynamic = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 3,
        .pDynamicStates = dynamic_states
    };
    VkPipelineRobustnessCreateInfoEXT robustness = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO_EXT,
        .storageBuffers = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2_EXT,
        .uniformBuffers = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2_EXT,
        .vertexInputs = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2_EXT,
        .images = VK_PIPELINE_ROBUSTNESS_IMAGE_BEHAVIOR_ROBUST_IMAGE_ACCESS_2_EXT
    };
    VkGraphicsPipelineCreateInfo pipeline = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = beer_vulkan_pipeline_robustness_enabled() ? &robustness : NULL,
        .stageCount = modules[1] ? 2u : 1u, .pStages = stages,
        .pVertexInputState = &vertex,
        .pInputAssemblyState = &assembly,
        .pViewportState = &viewport,
        .pRasterizationState = &raster,
        .pMultisampleState = &multisample,
        .pDepthStencilState = &depth_stencil,
        .pColorBlendState = &blend,
        .pDynamicState = &dynamic,
        .layout = entry->layout,
        .renderPass = entry->render_pass
    };
    if (g_graphics.CreateGraphicsPipelines(g_graphics.device, VK_NULL_HANDLE,
                                           1, &pipeline, NULL,
                                           &entry->pipeline) != VK_SUCCESS)
        entry->failure = "graphics pipeline creation failed";
modules_done:
    if (modules[0]) g_graphics.DestroyShaderModule(g_graphics.device,
                                                   modules[0], NULL);
    if (modules[1]) g_graphics.DestroyShaderModule(g_graphics.device,
                                                   modules[1], NULL);
    return entry;
}

#define REJECT(text) do { *reason = (text); pthread_mutex_unlock(&g_lock); \
    return 0; } while (0)

int beer_vulkan_graphics_draw_state(
    const void *vertex_key, const uint32_t *vertex_spirv,
    size_t vertex_spirv_size, const void *pixel_key,
    const uint32_t *pixel_spirv, size_t pixel_spirv_size,
    const BeerVulkanGraphicsBinding *bindings, uint32_t binding_count,
    const BeerVulkanGraphicsDrawInfo *draw_info,
    const BeerVulkanGraphicsState *state_in,
    BeerVulkanGraphicsTarget *targets, uint32_t target_count,
    BeerVulkanGraphicsDepthTarget *depth, const float viewport[6],
    const int32_t scissor[4], const char **reason)
{
    static const char *unused;
    if (!reason) reason = &unused;
    *reason = "invalid arguments";
    /* Publish only resident buffers this draw reads on the CPU. Publishing all
     * dirty DEFAULT buffers at every draw defeated R6 residency and inserted a
     * readback submission/fence before unrelated graphics work. */
    const void *read_resources[BEER_VULKAN_GRAPHICS_MAX_VERTEX_BINDINGS +
                               BEER_VULKAN_GRAPHICS_MAX_BINDINGS + 1];
    uint32_t read_resource_count = 0;
    if (draw_info) {
        for (uint32_t slot = 0;
             slot < draw_info->vertex_binding_count &&
             slot < BEER_VULKAN_GRAPHICS_MAX_VERTEX_BINDINGS; ++slot)
            if (draw_info->vertex_bindings[slot].resource)
                read_resources[read_resource_count++] =
                    draw_info->vertex_bindings[slot].resource;
        if (draw_info->indexed && draw_info->index_resource)
            read_resources[read_resource_count++] = draw_info->index_resource;
    }
    for (uint32_t binding = 0; binding < binding_count; ++binding)
        if (bindings[binding].resource &&
            (bindings[binding].kind ==
                 BEER_VULKAN_GRAPHICS_BINDING_UNIFORM_BUFFER ||
             bindings[binding].kind ==
                 BEER_VULKAN_GRAPHICS_BINDING_STORAGE_BUFFER ||
             bindings[binding].kind ==
                 BEER_VULKAN_GRAPHICS_BINDING_UNIFORM_TEXEL_BUFFER))
            read_resources[read_resource_count++] = bindings[binding].resource;
    beer_vulkan_compute_sync_buffer_resources(read_resources,
                                               read_resource_count);
    const uint32_t entry_target_count = target_count;
    BeerVulkanGraphicsDepthTarget *const entry_depth = depth;
    BeerVulkanGraphicsState state;
    if (state_in) {
        state = *state_in;
    } else {
        memset(&state, 0, sizeof(state));
        state.write_mask = 0xf;
    }
    if (!state.sample_count) state.sample_count = 1;
    if ((state.write_mask & ~0xfu) || state.cull_mode > 3u ||
        state.src_color_factor > 18u || state.dst_color_factor > 18u ||
        state.src_alpha_factor > 18u || state.dst_alpha_factor > 18u ||
        state.color_op > 4u || state.alpha_op > 4u ||
        state.depth_compare_op > 7u || state.stencil_front_fail > 7u ||
        state.stencil_front_pass > 7u ||
        state.stencil_front_depth_fail > 7u ||
        state.stencil_front_compare > 7u || state.stencil_back_fail > 7u ||
        state.stencil_back_pass > 7u ||
        state.stencil_back_depth_fail > 7u ||
        state.stencil_back_compare > 7u)
        return 0;
    if (!vertex_key || !vertex_spirv ||
        vertex_spirv_size < 20 || (vertex_spirv_size & 3u) ||
        ((pixel_spirv || pixel_spirv_size || pixel_key) &&
         (!pixel_key || !pixel_spirv || pixel_spirv_size < 20 ||
          (pixel_spirv_size & 3u))) ||
        (!pixel_spirv && target_count) ||
        binding_count > BEER_VULKAN_GRAPHICS_MAX_BINDINGS ||
        (binding_count && !bindings) ||
        target_count > BEER_VULKAN_GRAPHICS_MAX_TARGETS ||
        (target_count && !targets) || (!target_count && !depth) ||
        !viewport || !scissor)
        return 0;
    for (uint32_t i = 0; i < target_count; ++i)
        if (!targets[i].data || !targets[i].serial || !targets[i].width ||
            !targets[i].height ||
            (targets[i].bytes_per_pixel != 1 &&
             targets[i].bytes_per_pixel != 2 &&
             targets[i].bytes_per_pixel != 4 &&
             targets[i].bytes_per_pixel != 8) ||
            targets[i].row_pitch <
                targets[i].width * targets[i].bytes_per_pixel ||
            targets[i].width != targets[0].width ||
            targets[i].height != targets[0].height)
            return 0;
    if ((state.depth_test_enable || state.stencil_test_enable || !target_count) &&
        (!depth || !depth->data || !depth->serial ||
         (depth->format != VK_FORMAT_D32_SFLOAT &&
          depth->format != VK_FORMAT_D32_SFLOAT_S8_UINT) ||
         depth->bytes_per_pixel != 4 ||
         (state.stencil_test_enable &&
          (depth->format != VK_FORMAT_D32_SFLOAT_S8_UINT ||
           !depth->has_stencil)) ||
         (target_count && (depth->width != targets[0].width ||
                           depth->height != targets[0].height)) ||
         depth->row_pitch < depth->width *
             (depth->source_bytes_per_pixel ?
              depth->source_bytes_per_pixel : depth->bytes_per_pixel)))
        return 0;
    if (draw_info &&
        (!draw_info->element_count || !draw_info->instance_count ||
         draw_info->vertex_binding_count >
             BEER_VULKAN_GRAPHICS_MAX_VERTEX_BINDINGS ||
         draw_info->vertex_attribute_count >
             BEER_VULKAN_GRAPHICS_MAX_VERTEX_ATTRIBUTES ||
         (draw_info->indexed && (!draw_info->index_data ||
                                 !draw_info->index_size ||
                                 draw_info->index_type > 1))))
        return 0;
    if (draw_info && draw_info->batch_argument_count &&
        !draw_info->batch_arguments)
        return 0;
    if (draw_info && draw_info->indexed) {
        const size_t index_stride = draw_info->index_type ? 4u : 2u;
        const uint32_t command_count = draw_info->batch_argument_count ?
            draw_info->batch_argument_count : 1u;
        for (uint32_t command = 0; command < command_count; ++command) {
            const uint32_t start = draw_info->batch_argument_count ?
                draw_info->batch_arguments[command].start_element :
                draw_info->start_element;
            const uint32_t count = draw_info->batch_argument_count ?
                draw_info->batch_arguments[command].element_count :
                draw_info->element_count;
            if (!count || (size_t)start > SIZE_MAX / index_stride ||
                (size_t)count > (SIZE_MAX / index_stride) - (size_t)start)
                return 0;
            const size_t required = ((size_t)start + count) * index_stride;
            if (required > draw_info->index_size &&
                !draw_info->robust_index_access)
                return 0;
        }
    }
    size_t target_extents[BEER_VULKAN_GRAPHICS_MAX_TARGETS];
    size_t target_offsets[BEER_VULKAN_GRAPHICS_MAX_TARGETS];
    size_t target_total = 0;
    for (uint32_t i = 0; i < target_count; ++i) {
        target_extents[i] =
            (size_t)targets[i].row_pitch * (targets[i].height - 1) +
            (size_t)targets[i].width * targets[i].bytes_per_pixel;
        if (target_extents[i] > targets[i].size ||
            target_total > SIZE_MAX - align_up(target_extents[i]))
            return 0;
        target_offsets[i] = target_total;
        target_total += align_up(target_extents[i]);
    }
    size_t depth_source_extent = 0;
    size_t depth_upload_extent = 0;
    size_t stencil_upload_extent = 0;
    uint32_t depth_source_bytes = 0;
    if (depth) {
        depth_source_bytes = depth->source_bytes_per_pixel ?
            depth->source_bytes_per_pixel : depth->bytes_per_pixel;
        if ((depth_source_bytes != 2 && depth_source_bytes != 4 &&
             depth_source_bytes != 8) ||
            depth->row_pitch < depth->width * depth_source_bytes)
            return 0;
        depth_source_extent = (size_t)depth->row_pitch * (depth->height - 1) +
                              (size_t)depth->width * depth_source_bytes;
        depth_upload_extent = (size_t)depth->width * depth->height *
                              depth->bytes_per_pixel;
        stencil_upload_extent = depth->has_stencil ?
            (size_t)depth->width * depth->height : 0;
        if (depth_source_extent > depth->size) return 0;
    }
    pthread_mutex_lock(&g_lock);
    uint64_t profile_start_ns = monotonic_nanoseconds();
    uint64_t profile_submit_ns = 0;
    uint64_t profile_complete_ns = 0;
    if (!initialize_locked()) REJECT("Vulkan unavailable");
    if (beer_vulkan_device_lost()) REJECT("Vulkan device lost");
    if (state.flip_y && !beer_vulkan_maintenance1_enabled())
        REJECT("negative viewport height unavailable");
    uint32_t target_formats[BEER_VULKAN_GRAPHICS_MAX_TARGETS];
    for (uint32_t i = 0; i < target_count; ++i)
        target_formats[i] = targets[i].format;
    PipelineEntry *pipeline = find_or_create_pipeline(
        vertex_key, vertex_spirv, vertex_spirv_size, pixel_key, pixel_spirv,
        pixel_spirv_size, bindings, binding_count, draw_info, &state,
        target_formats, target_count, depth ? depth->format : 0);
    if (!pipeline) REJECT("pipeline cache exhausted");
    if (pipeline->failure || !pipeline->pipeline)
        REJECT(pipeline->failure ? pipeline->failure : "pipeline unavailable");

    size_t offsets[BEER_VULKAN_GRAPHICS_MAX_BINDINGS];
    /* 0 uploads host bytes, 1 reuses a graphics mirror, 2 borrows compute. */
    uint8_t skip_image_upload[BEER_VULKAN_GRAPHICS_MAX_BINDINGS];
    uint8_t target_uploads[BEER_VULKAN_GRAPHICS_MAX_TARGETS];
    size_t uploaded_image_bytes = 0;
    size_t graphics_mirror_bytes = 0;
    size_t compute_mirror_bytes = 0;
    size_t uploaded_buffer_bytes = 0;
    size_t uploaded_target_bytes = 0;
    memset(skip_image_upload, 0, sizeof(skip_image_upload));
    for (uint32_t i = 0; i < target_count; ++i)
        target_uploads[i] = !cached_target_matches(&targets[i]);
    size_t vertex_offsets[BEER_VULKAN_GRAPHICS_MAX_VERTEX_BINDINGS];
    size_t index_offset = 0;
    size_t depth_offset = target_total;
    size_t stencil_offset = depth_offset + align_up(depth_upload_extent);
    size_t total = depth ? stencil_offset + align_up(stencil_upload_extent) :
                           depth_offset;
    for (uint32_t i = 0; i < binding_count; ++i) {
        const BeerVulkanGraphicsBinding *binding = &bindings[i];
        offsets[i] = 0;
        if (!binding->stage_flags ||
            (binding->stage_flags & ~(BEER_VULKAN_GRAPHICS_STAGE_VERTEX |
                                      BEER_VULKAN_GRAPHICS_STAGE_FRAGMENT)))
            REJECT("invalid graphics descriptor stage");
        if (binding->kind == BEER_VULKAN_GRAPHICS_BINDING_SAMPLER) {
            if (binding->min_filter > 1 || binding->mag_filter > 1 ||
                binding->mipmap_mode > 1 || binding->reduction_mode > 2 ||
                binding->compare_enable > 1 || binding->compare_op > 7 ||
                binding->border_color > 2 ||
                binding->anisotropy_enable > 1 ||
                (binding->anisotropy_enable &&
                 (beer_vulkan_max_sampler_anisotropy() <= 1.0f ||
                  binding->max_anisotropy < 1.0f)) ||
                (binding->reduction_mode &&
                 !beer_vulkan_sampler_filter_minmax_enabled()) ||
                (binding->reduction_mode && binding->compare_enable) ||
                binding->address_u > 4 || binding->address_v > 4 ||
                binding->address_w > 4)
                REJECT("unsupported sampler state");
            continue;
        }
        if (binding->kind == BEER_VULKAN_GRAPHICS_BINDING_UNIFORM_BUFFER ||
            binding->kind == BEER_VULKAN_GRAPHICS_BINDING_STORAGE_BUFFER ||
            binding->kind ==
                BEER_VULKAN_GRAPHICS_BINDING_UNIFORM_TEXEL_BUFFER) {
            if (!binding->data || !binding->size ||
                (binding->kind == BEER_VULKAN_GRAPHICS_BINDING_UNIFORM_BUFFER &&
                 binding->size > 65536) ||
                (binding->kind ==
                     BEER_VULKAN_GRAPHICS_BINDING_UNIFORM_TEXEL_BUFFER &&
                 (!binding->texel_format || !binding->texel_bytes ||
                  (binding->size % binding->texel_bytes))))
                REJECT("graphics buffer layout invalid");
            if (total > SIZE_MAX - align_up(binding->size))
                REJECT("graphics resource layout invalid");
            offsets[i] = total;
            total += align_up(binding->size);
            continue;
        }
        if ((binding->kind != BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_IMAGE &&
             binding->kind != BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_DEPTH) ||
            !binding->data || !binding->size || !binding->width ||
            !binding->height)
            REJECT("unsupported graphics descriptor");
        const int compressed = binding->block_width && binding->block_height &&
            binding->bytes_per_block;
        const uint64_t binding_serial = binding->serial ?
            *binding->serial : UINT64_MAX;
        const uint32_t binding_layers = binding->layer_count ?
            binding->layer_count : 1;
        const uint32_t binding_depth = binding->depth ? binding->depth : 1;
        const uint32_t binding_dimension = binding->image_dimension ?
            binding->image_dimension : 2;
        const uint32_t binding_mips = binding->mip_levels ?
            binding->mip_levels : 1;
        if (binding->kind == BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_IMAGE &&
            beer_vulkan_compute_has_image_mirror(binding->resource,
                binding_serial, binding->format, binding->width,
                binding->height, binding_depth, binding_layers,
                binding_dimension, binding_mips)) {
            skip_image_upload[i] = 2;
            compute_mirror_bytes += binding->size;
            continue;
        }
        if (cached_image_matches_binding(binding)) {
            skip_image_upload[i] = 1;
            graphics_mirror_bytes += binding->size;
            continue;
        }
        uint32_t source_bytes = binding->source_bytes_per_pixel ?
            binding->source_bytes_per_pixel : binding->bytes_per_pixel;
        if (compressed) {
            uint32_t blocks_x = (binding->width + binding->block_width - 1) /
                                binding->block_width;
            if (!blocks_x ||
                binding->row_pitch < blocks_x * binding->bytes_per_block)
                REJECT("unsupported graphics compressed layout");
        } else if ((binding->bytes_per_pixel != 1 &&
                    binding->bytes_per_pixel != 2 &&
                    binding->bytes_per_pixel != 4 &&
                    binding->bytes_per_pixel != 8 &&
                    binding->bytes_per_pixel != 12 &&
                    binding->bytes_per_pixel != 16) ||
                   (source_bytes != 1 && source_bytes != 2 &&
                    source_bytes != 4 && source_bytes != 8 &&
                    source_bytes != 12 && source_bytes != 16) ||
                   binding->row_pitch < binding->width * source_bytes) {
            REJECT("unsupported graphics image layout");
        }
        size_t extent = 0;
        if (!binding_total_size(binding, &extent) || extent > binding->size ||
            total > SIZE_MAX - align_up(extent)) {
            if (getenv("BEER_D3D12_DIAGNOSTICS"))
                fprintf(stderr,
                        "[VULKAN GRAPHICS LAYOUT] binding=%u kind=%u "
                        "format=%u size=%zu required=%zu dimensions=%ux%ux%u "
                        "layers=%u mips=%u row=%u layer-stride=%zu "
                        "block=%ux%u/%u texel=%u/%u\n",
                        binding->binding, binding->kind, binding->format,
                        binding->size, extent, binding->width, binding->height,
                        binding->depth, binding->layer_count,
                        binding->mip_levels, binding->row_pitch,
                        binding->layer_stride, binding->block_width,
                        binding->block_height, binding->bytes_per_block,
                        binding->bytes_per_pixel,
                        binding->source_bytes_per_pixel);
            REJECT("graphics resource layout invalid");
        }
        offsets[i] = total;
        total += align_up(extent);
    }
    if (draw_info) {
        for (uint32_t i = 0; i < draw_info->vertex_binding_count; ++i) {
            const BeerVulkanGraphicsVertexBinding *vertex =
                &draw_info->vertex_bindings[i];
            if (!vertex->data || !vertex->size || !vertex->stride ||
                total > SIZE_MAX - align_up(vertex->size))
                REJECT("graphics vertex buffer invalid");
            vertex_offsets[i] = total;
            total += align_up(vertex->size);
        }
        if (draw_info->indexed) {
            size_t index_stride = draw_info->index_type ? 4u : 2u;
            size_t required = 0;
            const uint32_t command_count = draw_info->batch_argument_count ?
                draw_info->batch_argument_count : 1u;
            for (uint32_t command = 0; command < command_count; ++command) {
                const uint32_t start = draw_info->batch_argument_count ?
                    draw_info->batch_arguments[command].start_element :
                    draw_info->start_element;
                const uint32_t count = draw_info->batch_argument_count ?
                    draw_info->batch_arguments[command].element_count :
                    draw_info->element_count;
                const size_t command_required =
                    ((size_t)start + count) * index_stride;
                if (command_required > required) required = command_required;
            }
            size_t upload_size = draw_info->index_size;
            if (required > upload_size) {
                if (!draw_info->robust_index_access)
                    REJECT("graphics index buffer invalid");
                upload_size = required;
            }
            if (total > SIZE_MAX - align_up(upload_size))
                REJECT("graphics index buffer invalid");
            index_offset = total;
            total += align_up(upload_size);
        }
    }
    if (!ensure_staging(total)) REJECT("staging allocation failed");
    for (uint32_t i = 0; i < target_count; ++i)
        if (target_uploads[i]) {
            memcpy(g_graphics.staging_map + target_offsets[i], targets[i].data,
                   target_extents[i]);
            uploaded_target_bytes += target_extents[i];
        }
    /* Vulkan does not permit buffer-to-image copies into multisampled images.
     * Their authoritative contents therefore remain GPU-side; a newly created
     * D3D depth resource starts at the API-default far depth and subsequent
     * passes reuse the retained resource mirror. */
    if (depth && state.sample_count == 1) {
        for (uint32_t y = 0; y < depth->height; ++y) {
            const uint8_t *source = depth->data +
                (size_t)y * depth->row_pitch;
            uint8_t *destination = g_graphics.staging_map + depth_offset +
                (size_t)y * depth->width * depth->bytes_per_pixel;
            uint8_t *stencil = depth->has_stencil ?
                g_graphics.staging_map + stencil_offset +
                    (size_t)y * depth->width : NULL;
            for (uint32_t x = 0; x < depth->width; ++x) {
                const uint8_t *source_texel = source +
                    (size_t)x * depth_source_bytes;
                float value = 0.0f;
                if (depth->source_format == 53 || depth->source_format == 55) {
                    uint16_t packed = 0;
                    memcpy(&packed, source_texel, sizeof(packed));
                    value = (float)packed / 65535.0f;
                } else if (depth->source_format == 44 ||
                           depth->source_format == 45) {
                    uint32_t packed = 0;
                    memcpy(&packed, source_texel, sizeof(packed));
                    value = (float)(packed & UINT32_C(0x00ffffff)) /
                        16777215.0f;
                    if (stencil) stencil[x] = (uint8_t)(packed >> 24);
                } else {
                    memcpy(&value, source_texel, sizeof(value));
                    if (stencil &&
                        depth->stencil_byte_offset < depth_source_bytes)
                        stencil[x] = source_texel[depth->stencil_byte_offset];
                }
                memcpy(destination + (size_t)x * depth->bytes_per_pixel,
                       &value, sizeof(value));
            }
        }
    }
    for (uint32_t i = 0; i < binding_count; ++i) {
        if (bindings[i].kind == BEER_VULKAN_GRAPHICS_BINDING_SAMPLER ||
            skip_image_upload[i])
            continue;
        size_t extent = bindings[i].size;
        if (bindings[i].kind == BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_IMAGE ||
            bindings[i].kind == BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_DEPTH) {
            if (getenv("BEER_D3D12_DIAGNOSTICS")) {
                static uint32_t upload_traces = 0;
                if (++upload_traces <= 10) {
                    size_t nonzero = 0;
                    for (size_t j = 0; j < bindings[i].size && j < 10000; j += 4)
                        nonzero += ((uint32_t *)bindings[i].data)[j/4] != 0 ? 1 : 0;
                    fprintf(stderr, "[GRAPHICS UPLOAD] binding=%u size=%zu data=%p "
                        "nonzero_samples=%zu\n",
                        bindings[i].binding, bindings[i].size,
                        (const void *)bindings[i].data, nonzero);
                }
            }
            uint32_t source_bytes = bindings[i].source_bytes_per_pixel ?
                bindings[i].source_bytes_per_pixel :
                bindings[i].bytes_per_pixel;
            if ((bindings[i].mip_levels ? bindings[i].mip_levels : 1) > 1 ||
                bindings[i].layer_stride) {
                if (!binding_total_size(&bindings[i], &extent))
                    REJECT("graphics mip upload layout invalid");
                memcpy(g_graphics.staging_map + offsets[i], bindings[i].data,
                       extent);
            } else if (bindings[i].block_width && bindings[i].block_height &&
                       bindings[i].bytes_per_block) {
                uint32_t block_rows =
                    (bindings[i].height + bindings[i].block_height - 1) /
                    bindings[i].block_height;
                extent = (size_t)bindings[i].row_pitch * block_rows *
                    (bindings[i].layer_count ? bindings[i].layer_count : 1) *
                    (bindings[i].depth ? bindings[i].depth : 1);
                memcpy(g_graphics.staging_map + offsets[i], bindings[i].data,
                       extent);
            } else if (source_bytes == bindings[i].bytes_per_pixel) {
                size_t layer_extent = (size_t)bindings[i].row_pitch *
                    (bindings[i].height - 1) +
                    (size_t)bindings[i].width * source_bytes;
                uint32_t layers = bindings[i].layer_count ?
                    bindings[i].layer_count : 1;
                uint32_t depth_slices = bindings[i].depth ?
                    bindings[i].depth : 1;
                extent = layer_extent * layers * depth_slices;
                memcpy(g_graphics.staging_map + offsets[i], bindings[i].data,
                       extent);
            } else {
                /* Extract the selected 32-bit depth aspect from each packed
                 * R32G8X24 texel into a tightly packed D32 staging region. */
                uint32_t layers = bindings[i].layer_count ?
                    bindings[i].layer_count : 1;
                uint32_t depth_slices = bindings[i].depth ?
                    bindings[i].depth : 1;
                if (depth_slices != 1)
                    REJECT("packed depth volume unsupported");
                size_t source_layer_extent =
                    (size_t)bindings[i].row_pitch * (bindings[i].height - 1) +
                    (size_t)bindings[i].width * source_bytes;
                size_t destination_layer_extent =
                    (size_t)bindings[i].width * bindings[i].height *
                    bindings[i].bytes_per_pixel;
                extent = destination_layer_extent * layers;
                for (uint32_t layer = 0; layer < layers; ++layer) {
                  for (uint32_t y = 0; y < bindings[i].height; ++y) {
                    const uint8_t *source = bindings[i].data +
                        (size_t)layer * source_layer_extent +
                        (size_t)y * bindings[i].row_pitch;
                    uint8_t *destination = g_graphics.staging_map + offsets[i] +
                        (size_t)layer * destination_layer_extent +
                        (size_t)y * bindings[i].width *
                        bindings[i].bytes_per_pixel;
                    for (uint32_t x = 0; x < bindings[i].width; ++x)
                        memcpy(destination +
                                   (size_t)x * bindings[i].bytes_per_pixel,
                               source + (size_t)x * source_bytes,
                               bindings[i].bytes_per_pixel);
                  }
                }
            }
        } else {
            memcpy(g_graphics.staging_map + offsets[i], bindings[i].data,
                   extent);
        }
        if (bindings[i].kind == BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_IMAGE ||
            bindings[i].kind == BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_DEPTH)
            uploaded_image_bytes += extent;
        else
            uploaded_buffer_bytes += extent;
    }
    if (draw_info) {
        for (uint32_t i = 0; i < draw_info->vertex_binding_count; ++i)
            memcpy(g_graphics.staging_map + vertex_offsets[i],
                   draw_info->vertex_bindings[i].data,
                   draw_info->vertex_bindings[i].size);
        if (draw_info->indexed) {
            const size_t index_stride = draw_info->index_type ? 4u : 2u;
            size_t required = 0;
            const uint32_t command_count = draw_info->batch_argument_count ?
                draw_info->batch_argument_count : 1u;
            for (uint32_t command = 0; command < command_count; ++command) {
                const uint32_t start = draw_info->batch_argument_count ?
                    draw_info->batch_arguments[command].start_element :
                    draw_info->start_element;
                const uint32_t count = draw_info->batch_argument_count ?
                    draw_info->batch_arguments[command].element_count :
                    draw_info->element_count;
                const size_t command_required =
                    ((size_t)start + count) * index_stride;
                if (command_required > required) required = command_required;
            }
            memcpy(g_graphics.staging_map + index_offset,
                   draw_info->index_data, draw_info->index_size);
            if (required > draw_info->index_size)
                memset(g_graphics.staging_map + index_offset +
                           draw_info->index_size,
                       0, required - draw_info->index_size);
        }
    }

    ImageObject target_images[BEER_VULKAN_GRAPHICS_MAX_TARGETS];
    memset(target_images, 0, sizeof(target_images));
    ImageObject depth_image = {0};
    uint8_t initialize_multisampled_depth = 0;
    ImageObject images[BEER_VULKAN_GRAPHICS_MAX_BINDINGS];
    BeerVulkanComputeImageMirror
        compute_mirrors[BEER_VULKAN_GRAPHICS_MAX_BINDINGS];
    uint8_t image_uploads[BEER_VULKAN_GRAPHICS_MAX_BINDINGS];
    VkSampler samplers[BEER_VULKAN_GRAPHICS_MAX_BINDINGS];
    memset(images, 0, sizeof(images));
    memset(compute_mirrors, 0, sizeof(compute_mirrors));
    memset(image_uploads, 0, sizeof(image_uploads));
    memset(samplers, 0, sizeof(samplers));
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    const char *failure = NULL;
    for (uint32_t i = 0; i < target_count && !failure; ++i) {
        const uint64_t serial = targets[i].serial ? *targets[i].serial : 0;
        failure = acquire_cached_image(targets[i].format, targets[i].width,
            targets[i].height, 1, 1, 2, 0, 1, 1,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, targets[i].resource, serial,
            &target_images[i]);
        if (!failure)
            target_uploads[i] = !(targets[i].resource &&
                target_images[i].content_valid &&
                target_images[i].resource == targets[i].resource &&
                target_images[i].serial == serial);
    }
    if (failure) goto cleanup;
    for (uint32_t i = 0; i < binding_count && !failure; ++i) {
        if (bindings[i].kind == BEER_VULKAN_GRAPHICS_BINDING_SAMPLER) {
            samplers[i] = find_or_create_sampler(&bindings[i]);
            if (!samplers[i]) failure = "sampler cache exhausted";
        } else if (bindings[i].kind ==
                       BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_IMAGE ||
                   bindings[i].kind ==
                       BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_DEPTH) {
            VkImageAspectFlags aspect = bindings[i].kind ==
                BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_DEPTH ?
                VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
            const uint64_t serial = bindings[i].serial ?
                *bindings[i].serial : UINT64_MAX;
            const uint32_t layers = bindings[i].layer_count ?
                bindings[i].layer_count : 1;
            const uint32_t depth = bindings[i].depth ? bindings[i].depth : 1;
            const uint32_t dimension = bindings[i].image_dimension ?
                bindings[i].image_dimension : 2;
            const uint32_t mip_levels = bindings[i].mip_levels ?
                bindings[i].mip_levels : 1;
            if (bindings[i].kind ==
                    BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_IMAGE &&
                beer_vulkan_compute_acquire_image_mirror(
                    bindings[i].resource, serial, bindings[i].format,
                    bindings[i].width, bindings[i].height, depth, layers,
                    dimension, mip_levels, &compute_mirrors[i])) {
                images[i] = (ImageObject){
                    .image = (VkImage)(uintptr_t)compute_mirrors[i].image,
                    .aspect = aspect,
                    .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
                    .resource = bindings[i].resource,
                    .serial = serial,
                    .layout = (VkImageLayout)compute_mirrors[i].layout,
                    .content_valid = 1
                };
                image_uploads[i] = 0;
            } else {
                failure = acquire_cached_image(bindings[i].format,
                    bindings[i].width, bindings[i].height, layers, depth,
                    dimension, bindings[i].image_cube, mip_levels,
                    bindings[i].sample_count ? bindings[i].sample_count : 1,
                    VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    aspect, bindings[i].resource, serial, &images[i]);
                if (!failure)
                    image_uploads[i] = !(bindings[i].resource &&
                        images[i].content_valid &&
                        images[i].resource == bindings[i].resource &&
                        images[i].serial == serial);
            }
        }
    }
    if (failure) goto cleanup;
    if (depth) {
        const int multisampled_depth =
            (depth->sample_count ? depth->sample_count : 1) > 1;
        failure = acquire_cached_image(depth->format, depth->width,
            depth->height, 1, 1, 2, 0, 1,
            depth->sample_count ? depth->sample_count : 1,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            (multisampled_depth ? VK_IMAGE_USAGE_SAMPLED_BIT :
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT),
            VK_IMAGE_ASPECT_DEPTH_BIT |
                (depth->has_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0),
            multisampled_depth ? depth->resource : NULL,
            depth->serial ? *depth->serial : 0, &depth_image);
        if (failure) goto cleanup;
        initialize_multisampled_depth = multisampled_depth &&
            !depth_image.content_valid;
    }
    VkImageView framebuffer_attachments[
        BEER_VULKAN_GRAPHICS_MAX_TARGETS + 1];
    for (uint32_t i = 0; i < target_count; ++i)
        framebuffer_attachments[i] = target_images[i].view;
    if (depth) framebuffer_attachments[target_count] = depth_image.view;
    framebuffer = find_or_create_framebuffer(
        pipeline->render_pass, framebuffer_attachments,
        target_count + (depth ? 1u : 0u),
        target_count ? targets[0].width : depth->width,
        target_count ? targets[0].height : depth->height);
    if (!framebuffer) {
        failure = "framebuffer creation failed";
        goto cleanup;
    }
    if (!pipeline->descriptor_set) {
        VkDescriptorSetAllocateInfo set_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = g_graphics.descriptor_pool,
            .descriptorSetCount = 1,
            .pSetLayouts = &pipeline->set_layout
        };
        if (g_graphics.AllocateDescriptorSets(g_graphics.device, &set_info,
                                              &pipeline->descriptor_set) !=
            VK_SUCCESS) {
            failure = "descriptor allocation failed";
            goto cleanup;
        }
    }
    VkDescriptorSet descriptor_set = pipeline->descriptor_set;
    VkDescriptorImageInfo image_infos[BEER_VULKAN_GRAPHICS_MAX_BINDINGS];
    VkDescriptorBufferInfo buffer_infos[BEER_VULKAN_GRAPHICS_MAX_BINDINGS];
    VkBufferView buffer_views[BEER_VULKAN_GRAPHICS_MAX_BINDINGS];
    VkWriteDescriptorSet writes[BEER_VULKAN_GRAPHICS_MAX_BINDINGS];
    memset(buffer_views, 0, sizeof(buffer_views));
    memset(writes, 0, sizeof(writes));
    for (uint32_t i = 0; i < binding_count; ++i) {
        image_infos[i] = (VkDescriptorImageInfo){
            .sampler = samplers[i],
            .imageView = images[i].view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        };
        buffer_infos[i] = (VkDescriptorBufferInfo){
            .buffer = g_graphics.staging_buffer,
            .offset = offsets[i],
            .range = bindings[i].size
        };
        if (bindings[i].kind ==
                BEER_VULKAN_GRAPHICS_BINDING_UNIFORM_TEXEL_BUFFER) {
            buffer_views[i] = find_or_create_buffer_view(
                (VkFormat)bindings[i].texel_format, offsets[i],
                bindings[i].size);
            if (!buffer_views[i]) {
                failure = "texel buffer view cache exhausted";
                goto cleanup;
            }
        }
        VkDescriptorType descriptor_type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        const VkDescriptorImageInfo *image_info = &image_infos[i];
        const VkDescriptorBufferInfo *buffer_info = NULL;
        if (bindings[i].kind == BEER_VULKAN_GRAPHICS_BINDING_SAMPLER)
            descriptor_type = VK_DESCRIPTOR_TYPE_SAMPLER;
        else if (bindings[i].kind ==
                 BEER_VULKAN_GRAPHICS_BINDING_UNIFORM_BUFFER ||
                 bindings[i].kind ==
                 BEER_VULKAN_GRAPHICS_BINDING_STORAGE_BUFFER) {
            descriptor_type = bindings[i].kind ==
                BEER_VULKAN_GRAPHICS_BINDING_UNIFORM_BUFFER ?
                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER :
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            image_info = NULL;
            buffer_info = &buffer_infos[i];
        } else if (bindings[i].kind ==
                   BEER_VULKAN_GRAPHICS_BINDING_UNIFORM_TEXEL_BUFFER) {
            descriptor_type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
            image_info = NULL;
        }
        writes[i] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = descriptor_set,
            .dstBinding = bindings[i].binding,
            .descriptorCount = 1,
            .descriptorType = descriptor_type,
            .pImageInfo = image_info,
            .pBufferInfo = buffer_info,
            .pTexelBufferView = descriptor_type ==
                VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ?
                &buffer_views[i] : NULL
        };
    }
    if (binding_count)
        g_graphics.UpdateDescriptorSets(g_graphics.device, binding_count,
                                        writes, 0, NULL);
    if (g_graphics.ResetCommandBuffer(g_graphics.command_buffer, 0) !=
        VK_SUCCESS) {
        failure = "command buffer reset failed";
        goto cleanup;
    }
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    if (g_graphics.BeginCommandBuffer(g_graphics.command_buffer, &begin) !=
        VK_SUCCESS) {
        failure = "command buffer begin failed";
        goto cleanup;
    }
    VkImageMemoryBarrier barriers[BEER_VULKAN_GRAPHICS_MAX_BINDINGS +
                                  BEER_VULKAN_GRAPHICS_MAX_TARGETS + 1];
    uint32_t barrier_count = 0;
    for (uint32_t i = 0; i < target_count; ++i) {
        const int upload = target_uploads[i] != 0;
        barriers[barrier_count++] = (VkImageMemoryBarrier){
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = upload ? 0 :
                (VK_ACCESS_TRANSFER_READ_BIT |
                 VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT),
            .dstAccessMask = upload ? VK_ACCESS_TRANSFER_WRITE_BIT :
                (VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT),
            .oldLayout = upload ?
                (target_images[i].content_valid ? target_images[i].layout :
                 VK_IMAGE_LAYOUT_UNDEFINED) : target_images[i].layout,
            .newLayout = upload ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL :
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = target_images[i].image,
            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                 .levelCount = 1, .layerCount = 1}
        };
    }
    if (depth) {
        const int multisampled_depth = state.sample_count > 1;
        barriers[barrier_count++] = (VkImageMemoryBarrier){
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = multisampled_depth && depth_image.content_valid ?
                (VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) : 0,
            .dstAccessMask = multisampled_depth ?
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT :
                VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = multisampled_depth && depth_image.content_valid ?
                depth_image.layout : VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = multisampled_depth ?
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL :
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = depth_image.image,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT |
                    (depth->has_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0),
                .levelCount = 1, .layerCount = 1}
        };
    }
    for (uint32_t i = 0; i < binding_count; ++i) {
        if (bindings[i].kind != BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_IMAGE &&
            bindings[i].kind != BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_DEPTH)
            continue;
        const int upload = image_uploads[i] != 0;
        if (!upload && images[i].layout ==
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
            continue;
        VkAccessFlags source_access = 0;
        if (!upload) {
            if (images[i].layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
                source_access = VK_ACCESS_TRANSFER_READ_BIT;
            else if (images[i].layout ==
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
                source_access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            else if (images[i].layout ==
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                source_access = VK_ACCESS_SHADER_READ_BIT;
        }
        barriers[barrier_count++] = (VkImageMemoryBarrier){
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = source_access,
            .dstAccessMask = upload ? VK_ACCESS_TRANSFER_WRITE_BIT :
                                      VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = images[i].content_valid ? images[i].layout :
                VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = upload ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL :
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = images[i].image,
            .subresourceRange = {.aspectMask = images[i].aspect,
                                 .levelCount = bindings[i].mip_levels ?
                                     bindings[i].mip_levels : 1,
                                 .layerCount = bindings[i].depth > 1 ? 1 :
                                     (bindings[i].layer_count ?
                                      bindings[i].layer_count : 1)}
        };
        if (!upload)
            images[i].layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    g_graphics.CmdPipelineBarrier(g_graphics.command_buffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT |
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT |
            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, 0, NULL, 0, NULL, barrier_count, barriers);
    VkBufferImageCopy copy = {
        .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .layerCount = 1}
    };
    for (uint32_t i = 0; i < target_count; ++i) {
        if (!target_uploads[i]) continue;
        copy.bufferOffset = target_offsets[i];
        copy.bufferRowLength =
            targets[i].row_pitch / targets[i].bytes_per_pixel;
        copy.bufferImageHeight = targets[i].height;
        copy.imageExtent = (VkExtent3D){targets[i].width,
                                        targets[i].height, 1};
        g_graphics.CmdCopyBufferToImage(g_graphics.command_buffer,
            g_graphics.staging_buffer, target_images[i].image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    }
    if (depth) {
        copy.bufferRowLength = depth->width;
        copy.bufferImageHeight = depth->height;
        copy.imageExtent = (VkExtent3D){depth->width, depth->height, 1};
        if (state.sample_count == 1) {
            copy.bufferOffset = depth_offset;
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            g_graphics.CmdCopyBufferToImage(g_graphics.command_buffer,
                g_graphics.staging_buffer, depth_image.image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            if (depth->has_stencil) {
                copy.bufferOffset = stencil_offset;
                copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
                g_graphics.CmdCopyBufferToImage(g_graphics.command_buffer,
                    g_graphics.staging_buffer, depth_image.image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            }
        } else if (initialize_multisampled_depth) {
            const VkClearDepthStencilValue initial = {1.0f, 0};
            VkImageSubresourceRange range = {
                .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT |
                    (depth->has_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0),
                .levelCount = 1,
                .layerCount = 1
            };
            g_graphics.CmdClearDepthStencilImage(g_graphics.command_buffer,
                depth_image.image,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                &initial, 1, &range);
        }
    }
    for (uint32_t i = 0; i < binding_count; ++i) {
        if ((bindings[i].kind != BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_IMAGE &&
             bindings[i].kind != BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_DEPTH) ||
            !image_uploads[i])
            continue;
        copy.bufferOffset = offsets[i];
        if (bindings[i].block_width && bindings[i].block_height &&
            bindings[i].bytes_per_block) {
            uint32_t blocks_per_row = bindings[i].row_pitch /
                                      bindings[i].bytes_per_block;
            copy.bufferRowLength = blocks_per_row * bindings[i].block_width;
            copy.bufferImageHeight =
                ((bindings[i].height + bindings[i].block_height - 1) /
                 bindings[i].block_height) * bindings[i].block_height;
        } else {
            copy.bufferRowLength = bindings[i].source_bytes_per_pixel &&
                bindings[i].source_bytes_per_pixel !=
                    bindings[i].bytes_per_pixel ?
                bindings[i].width : bindings[i].row_pitch /
                    bindings[i].bytes_per_pixel;
            copy.bufferImageHeight = bindings[i].height;
        }
        uint32_t levels = bindings[i].mip_levels ?
            bindings[i].mip_levels : 1;
        VkBufferImageCopy copies[MAX_IMAGE_MIPS *
                                  BEER_VULKAN_GRAPHICS_MAX_BINDINGS];
        uint32_t copy_count = 0;
        const uint32_t copy_capacity =
            (uint32_t)(sizeof(copies) / sizeof(copies[0]));
        uint32_t layers = bindings[i].layer_count ?
            bindings[i].layer_count : 1;
        for (uint32_t level = 0; level < levels; ++level) {
            size_t level_offset = 0, level_size = 0;
            uint32_t width = 0, height = 0, depth = 0;
            if (!binding_mip_layout(&bindings[i], level, &level_offset,
                                    &level_size, &width, &height, &depth)) {
                failure = "graphics mip copy layout invalid";
                goto cleanup;
            }
            (void)level_size;
            uint32_t copy_layers = bindings[i].layer_stride ? layers : 1;
            for (uint32_t layer = 0; layer < copy_layers; ++layer) {
                copies[copy_count++] = (VkBufferImageCopy){
                    .bufferOffset = offsets[i] + level_offset +
                        (size_t)layer * bindings[i].layer_stride,
                    .bufferRowLength = level == 0 ? copy.bufferRowLength : 0,
                    .bufferImageHeight = level == 0 ?
                        copy.bufferImageHeight : 0,
                    .imageSubresource = { images[i].aspect, level, layer,
                        bindings[i].layer_stride ? 1u : layers },
                    .imageExtent = { width, height, depth }
                };
                if (copy_count == copy_capacity) {
                    g_graphics.CmdCopyBufferToImage(g_graphics.command_buffer,
                        g_graphics.staging_buffer, images[i].image,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, copy_count,
                        copies);
                    copy_count = 0;
                }
            }
        }
        if (copy_count)
            g_graphics.CmdCopyBufferToImage(g_graphics.command_buffer,
                g_graphics.staging_buffer, images[i].image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, copy_count, copies);
    }
    barrier_count = 0;
    for (uint32_t i = 0; i < target_count; ++i)
        if (target_uploads[i])
            barriers[barrier_count++] = (VkImageMemoryBarrier){
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = target_images[i].image,
                .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                     .levelCount = 1, .layerCount = 1}
            };
    if (depth && state.sample_count == 1) {
        barriers[barrier_count++] = (VkImageMemoryBarrier){
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = depth_image.image,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT |
                    (depth->has_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0),
                .levelCount = 1, .layerCount = 1}
        };
    }
    for (uint32_t i = 0; i < binding_count; ++i) {
        if ((bindings[i].kind != BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_IMAGE &&
             bindings[i].kind != BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_DEPTH) ||
            !image_uploads[i])
            continue;
        barriers[barrier_count++] = (VkImageMemoryBarrier){
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = images[i].image,
            .subresourceRange = {.aspectMask = images[i].aspect,
                                 .levelCount = bindings[i].mip_levels ?
                                     bindings[i].mip_levels : 1,
                                 .layerCount = bindings[i].depth > 1 ? 1 :
                                     (bindings[i].layer_count ?
                                      bindings[i].layer_count : 1)}
        };
    }
    g_graphics.CmdPipelineBarrier(g_graphics.command_buffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
        0, NULL, 0, NULL, barrier_count, barriers);
    VkRenderPassBeginInfo render = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = pipeline->render_pass,
        .framebuffer = framebuffer,
        .renderArea = {.extent = {
            target_count ? targets[0].width : depth->width,
            target_count ? targets[0].height : depth->height}}
    };
    const int collect_draw_statistics = draw_info &&
        g_graphics.statistics_query_pool != VK_NULL_HANDLE &&
        getenv("BEER_VULKAN_DRAW_PROVENANCE") != NULL;
    if (collect_draw_statistics) {
        g_graphics.CmdResetQueryPool(g_graphics.command_buffer,
                                     g_graphics.statistics_query_pool, 0, 1);
        g_graphics.CmdBeginQuery(g_graphics.command_buffer,
                                 g_graphics.statistics_query_pool, 0, 0);
    }
    g_graphics.CmdBeginRenderPass(g_graphics.command_buffer, &render,
                                  VK_SUBPASS_CONTENTS_INLINE);
    g_graphics.CmdBindPipeline(g_graphics.command_buffer,
                               VK_PIPELINE_BIND_POINT_GRAPHICS,
                               pipeline->pipeline);
    if (binding_count)
        g_graphics.CmdBindDescriptorSets(g_graphics.command_buffer,
            VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->layout, 0, 1,
            &descriptor_set, 0, NULL);
    VkViewport vk_viewport = {
        .x = viewport[0], .y = viewport[1],
        .width = viewport[2], .height = viewport[3],
        .minDepth = viewport[4], .maxDepth = viewport[5]
    };
    if (state.flip_y) {
        vk_viewport.y = viewport[1] + viewport[3];
        vk_viewport.height = -viewport[3];
    }
    VkRect2D vk_scissor = {
        .offset = {scissor[0], scissor[1]},
        .extent = {(uint32_t)(scissor[2] - scissor[0]),
                   (uint32_t)(scissor[3] - scissor[1])}
    };
    g_graphics.CmdSetViewport(g_graphics.command_buffer, 0, 1, &vk_viewport);
    g_graphics.CmdSetScissor(g_graphics.command_buffer, 0, 1, &vk_scissor);
    g_graphics.CmdSetBlendConstants(g_graphics.command_buffer,
                                    state.blend_constants);
    if (draw_info && draw_info->vertex_binding_count) {
        VkBuffer buffers[BEER_VULKAN_GRAPHICS_MAX_VERTEX_BINDINGS];
        VkDeviceSize buffer_offsets[BEER_VULKAN_GRAPHICS_MAX_VERTEX_BINDINGS];
        for (uint32_t i = 0; i < draw_info->vertex_binding_count; ++i) {
            buffers[i] = g_graphics.staging_buffer;
            buffer_offsets[i] = vertex_offsets[i];
        }
        g_graphics.CmdBindVertexBuffers(g_graphics.command_buffer, 0,
            draw_info->vertex_binding_count, buffers, buffer_offsets);
    }
    uint64_t recorded_draw_commands = 0;
    if (draw_info && draw_info->indexed) {
        g_graphics.CmdBindIndexBuffer(g_graphics.command_buffer,
            g_graphics.staging_buffer, index_offset,
            draw_info->index_type ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
        const uint32_t command_count = draw_info->batch_argument_count ?
            draw_info->batch_argument_count : 1u;
        for (uint32_t command = 0; command < command_count; ++command) {
            const BeerVulkanGraphicsDrawArguments *arguments =
                draw_info->batch_argument_count ?
                    &draw_info->batch_arguments[command] : NULL;
            g_graphics.CmdDrawIndexed(g_graphics.command_buffer,
                arguments ? arguments->element_count : draw_info->element_count,
                arguments ? arguments->instance_count : draw_info->instance_count,
                arguments ? arguments->start_element : draw_info->start_element,
                arguments ? arguments->base_vertex : draw_info->base_vertex,
                arguments ? arguments->start_instance : draw_info->start_instance);
            ++recorded_draw_commands;
        }
    } else if (draw_info) {
        const uint32_t command_count = draw_info->batch_argument_count ?
            draw_info->batch_argument_count : 1u;
        for (uint32_t command = 0; command < command_count; ++command) {
            const BeerVulkanGraphicsDrawArguments *arguments =
                draw_info->batch_argument_count ?
                    &draw_info->batch_arguments[command] : NULL;
            g_graphics.CmdDraw(g_graphics.command_buffer,
                arguments ? arguments->element_count : draw_info->element_count,
                arguments ? arguments->instance_count : draw_info->instance_count,
                arguments ? arguments->start_element : draw_info->start_element,
                arguments ? arguments->start_instance : draw_info->start_instance);
            ++recorded_draw_commands;
        }
    } else {
        g_graphics.CmdDraw(g_graphics.command_buffer, 3, 1, 0, 0);
        recorded_draw_commands = 1;
    }
    g_graphics.CmdEndRenderPass(g_graphics.command_buffer);
    if (collect_draw_statistics)
        g_graphics.CmdEndQuery(g_graphics.command_buffer,
                               g_graphics.statistics_query_pool, 0);

    /* Render-pass finalLayout is COLOR_ATTACHMENT_OPTIMAL. Readback is a
     * transfer operation and therefore requires an explicit layout/access
     * transition. Copying directly while declaring TRANSFER_SRC_OPTIMAL was
     * invalid Vulkan and allowed undefined/stale frame data to reach Present. */
    VkImageMemoryBarrier target_readback_barriers[
        BEER_VULKAN_GRAPHICS_MAX_TARGETS];
    uint32_t target_readback_count = 0;
    for (uint32_t i = 0; i < target_count; ++i) {
        if (targets[i].defer_publication) continue;
        target_readback_barriers[target_readback_count++] =
            (VkImageMemoryBarrier){
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = target_images[i].image,
                .subresourceRange = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .levelCount = 1,
                    .layerCount = 1
                }
            };
    }
    if (target_readback_count)
        g_graphics.CmdPipelineBarrier(g_graphics.command_buffer,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
            target_readback_count, target_readback_barriers);

    /* Multisampled images cannot be copied directly to a buffer. Preserve
     * their authoritative depth contents in the retained Vulkan mirror until
     * a proper resolve/sampled-MS consumer is implemented. */
    const int depth_writeback = depth && state.sample_count == 1 &&
        state.depth_write_enable;
    const int stencil_writeback = depth && state.sample_count == 1 &&
        depth->has_stencil && state.stencil_test_enable &&
        state.stencil_write_mask;
    if (depth_writeback || stencil_writeback) {
        VkImageMemoryBarrier depth_readback_barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = depth_image.image,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT |
                    (depth->has_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0),
                .levelCount = 1,
                .layerCount = 1
            }
        };
        g_graphics.CmdPipelineBarrier(g_graphics.command_buffer,
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
            &depth_readback_barrier);
        copy.bufferRowLength = depth->width;
        copy.bufferImageHeight = depth->height;
        copy.imageSubresource.baseArrayLayer = 0;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = (VkExtent3D){depth->width, depth->height, 1};
        if (depth_writeback) {
            copy.bufferOffset = depth_offset;
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            g_graphics.CmdCopyImageToBuffer(g_graphics.command_buffer,
                depth_image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                g_graphics.staging_buffer, 1, &copy);
        }
        if (stencil_writeback) {
            copy.bufferOffset = stencil_offset;
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
            g_graphics.CmdCopyImageToBuffer(g_graphics.command_buffer,
                depth_image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                g_graphics.staging_buffer, 1, &copy);
        }
    }
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    for (uint32_t i = 0; i < target_count; ++i) {
        if (targets[i].defer_publication) continue;
        copy.bufferOffset = target_offsets[i];
        copy.bufferRowLength =
            targets[i].row_pitch / targets[i].bytes_per_pixel;
        copy.bufferImageHeight = targets[i].height;
        copy.imageExtent = (VkExtent3D){targets[i].width,
                                        targets[i].height, 1};
        g_graphics.CmdCopyImageToBuffer(g_graphics.command_buffer,
            target_images[i].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            g_graphics.staging_buffer, 1, &copy);
    }
    for (uint32_t i = 0; i < binding_count; ++i) {
        if (bindings[i].kind != BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_IMAGE &&
            bindings[i].kind != BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_DEPTH)
            continue;
        if (image_uploads[i]) {
            images[i].resource = bindings[i].resource;
            images[i].serial = bindings[i].serial ? *bindings[i].serial : UINT64_MAX;
            images[i].content_valid = bindings[i].resource != NULL;
            images[i].layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
    }
    if (g_graphics.EndCommandBuffer(g_graphics.command_buffer) != VK_SUCCESS) {
        failure = "command buffer end failed";
        goto cleanup;
    }
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &g_graphics.command_buffer
    };
    profile_submit_ns = monotonic_nanoseconds();
    VkResult submitted = VK_SUCCESS;
    VkResult waited = VK_SUCCESS;
    if (beer_vulkan_timeline_enabled()) {
        const uint64_t value = beer_vulkan_timeline_submit(
            &g_graphics.command_buffer, 1);
        if (!value)
            submitted = beer_vulkan_device_lost() ? VK_ERROR_DEVICE_LOST :
                VK_ERROR_INITIALIZATION_FAILED;
        else if (!beer_vulkan_timeline_wait(value, UINT64_MAX))
            waited = beer_vulkan_device_lost() ? VK_ERROR_DEVICE_LOST :
                VK_TIMEOUT;
    } else {
        if (g_graphics.ResetFences(g_graphics.device, 1, &g_graphics.fence) !=
            VK_SUCCESS) {
            failure = "fence reset failed";
            goto cleanup;
        }
        beer_vulkan_queue_lock();
        submitted = g_graphics.QueueSubmit(g_graphics.queue, 1, &submit,
                                            g_graphics.fence);
        beer_vulkan_queue_unlock();
        if (submitted == VK_SUCCESS)
            waited = g_graphics.WaitForFences(
                g_graphics.device, 1, &g_graphics.fence, VK_TRUE, UINT64_MAX);
    }
    if (submitted == VK_SUCCESS) {
        ++g_graphics.queue_submissions;
        g_graphics.draw_commands += recorded_draw_commands;
    }
    if (submitted != VK_SUCCESS) {
        beer_vulkan_record_result(submitted, "graphics queue submission");
        fprintf(stderr, "[VULKAN GRAPHICS] queue submission result=%d\n",
                submitted);
        failure = "graphics submission failed";
        goto cleanup;
    }
    ++g_graphics.fence_waits;
    if (waited != VK_SUCCESS) {
        beer_vulkan_record_result(waited, "graphics completion wait");
        fprintf(stderr, "[VULKAN GRAPHICS] completion wait result=%d "
                "after-ms=%.1f vs=%016llx ps=%016llx elements=%u "
                "depth=%ux%u ptr=%p depth-test=%u target-count=%u\n", waited,
                (monotonic_nanoseconds() - profile_submit_ns) / 1e6,
                draw_info ? (unsigned long long)draw_info->debug_vertex_hash : 0ull,
                draw_info ? (unsigned long long)draw_info->debug_pixel_hash : 0ull,
                draw_info ? draw_info->element_count : 0u,
                depth ? depth->width : 0u, depth ? depth->height : 0u,
                (void *)depth, state.depth_test_enable, target_count);
        fprintf(stderr, "[VULKAN GRAPHICS] entry target-count=%u depth=%p "
                "depth-test=%u stencil=%u write-mask=%x\n",
                entry_target_count, (void *)entry_depth,
                state.depth_test_enable, state.stencil_test_enable,
                state.write_mask);
        capture_graphics_device_loss(
            vertex_spirv, vertex_spirv_size, pixel_spirv, pixel_spirv_size,
            bindings, binding_count, draw_info, &state, entry_target_count,
            entry_depth, viewport, scissor);
        failure = "graphics submission failed";
        goto cleanup;
    }
    profile_complete_ns = monotonic_nanoseconds();
    /* Optionally trace indexed triangle strips at the Vulkan boundary. This is
     * state-based so diagnostics remain useful for every D3D application. */
    if (draw_info && draw_info->indexed &&
        draw_info->topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP &&
        getenv("BEER_VULKAN_GUI_DRAW_TRACE")) {
        static uint32_t gui_draw_traces;
        if (gui_draw_traces++ < 32) {
            uint64_t changed_pixels = 0;
            uint32_t min_x = targets[0].width, min_y = targets[0].height;
            uint32_t max_x = 0, max_y = 0;
            const uint8_t *after = g_graphics.staging_map + target_offsets[0];
            for (uint32_t y = 0; y < targets[0].height; ++y) {
                const uint8_t *before_row = targets[0].data +
                    (size_t)y * targets[0].row_pitch;
                const uint8_t *after_row = after +
                    (size_t)y * targets[0].row_pitch;
                for (uint32_t x = 0; x < targets[0].width; ++x) {
                    size_t pixel = (size_t)x * targets[0].bytes_per_pixel;
                    if (memcmp(before_row + pixel, after_row + pixel,
                               targets[0].bytes_per_pixel) == 0)
                        continue;
                    ++changed_pixels;
                    if (x < min_x) min_x = x;
                    if (y < min_y) min_y = y;
                    if (x > max_x) max_x = x;
                    if (y > max_y) max_y = y;
                }
            }
            fprintf(stderr,
                    "[VULKAN GUI DRAW] submission=%llu list=%u sequence=%llu "
                    "first=%u base=%d changed=%llu bounds=%u,%u-%u,%u "
                    "blend=%u factors=%u/%u ops=%u depth=%u/%u/%u "
                    "write=%x restart=%u indices=",
                    (unsigned long long)draw_info->debug_submission,
                    draw_info->debug_list_index,
                    (unsigned long long)draw_info->debug_sequence,
                    draw_info->start_element, draw_info->base_vertex,
                    (unsigned long long)changed_pixels,
                    changed_pixels ? min_x : 0, changed_pixels ? min_y : 0,
                    changed_pixels ? max_x : 0, changed_pixels ? max_y : 0,
                    state.blend_enable, state.src_color_factor,
                    state.dst_color_factor, state.color_op,
                    state.depth_test_enable, state.depth_write_enable,
                    state.depth_compare_op, state.write_mask,
                    state.primitive_restart_enable);
            const size_t index_stride = draw_info->index_type ? 4u : 2u;
            for (uint32_t i = 0; i < draw_info->element_count; ++i) {
                size_t offset = ((size_t)draw_info->start_element + i) *
                    index_stride;
                uint32_t index = 0;
                if (offset + index_stride <= draw_info->index_size)
                    memcpy(&index, draw_info->index_data + offset,
                           index_stride);
                int64_t vertex = (int64_t)index + draw_info->base_vertex;
                fprintf(stderr, "%s%u(%lld)", i ? "," : "", index,
                        (long long)vertex);
            }
            fputs(" positions=", stderr);
            const BeerVulkanGraphicsVertexAttribute *position = NULL;
            for (uint32_t i = 0; i < draw_info->vertex_attribute_count; ++i)
                if (draw_info->vertex_attributes[i].location == 0) {
                    position = &draw_info->vertex_attributes[i];
                    break;
                }
            if (position && position->binding <
                    draw_info->vertex_binding_count) {
                const BeerVulkanGraphicsVertexBinding *vertex_binding =
                    &draw_info->vertex_bindings[position->binding];
                for (uint32_t i = 0; i < draw_info->element_count; ++i) {
                    size_t index_offset =
                        ((size_t)draw_info->start_element + i) * index_stride;
                    uint32_t index = 0;
                    if (index_offset + index_stride <= draw_info->index_size)
                        memcpy(&index, draw_info->index_data + index_offset,
                               index_stride);
                    int64_t vertex = (int64_t)index + draw_info->base_vertex;
                    float xy[2] = {0.0f, 0.0f};
                    if (vertex >= 0 &&
                        (size_t)vertex <=
                            (SIZE_MAX - position->offset) /
                                vertex_binding->stride) {
                        size_t offset = (size_t)vertex *
                            vertex_binding->stride + position->offset;
                        if (offset + sizeof(xy) <= vertex_binding->size)
                            memcpy(xy, vertex_binding->data + offset,
                                   sizeof(xy));
                    }
                    fprintf(stderr, "%s%.3f/%.3f", i ? "," : "",
                            xy[0], xy[1]);
                }
            } else {
                fputs("unresolved", stderr);
            }
            fputc('\n', stderr);
        }
    }
    if (collect_draw_statistics) {
        uint64_t statistics[5] = {0};
        VkResult result = g_graphics.GetQueryPoolResults(
            g_graphics.device, g_graphics.statistics_query_pool, 0, 1,
            sizeof(statistics), statistics, sizeof(statistics[0]),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        if (result == VK_SUCCESS) {
            fprintf(stderr,
                    "[VULKAN DRAW] submission=%llu list=%u sequence=%llu "
                    "vs=%016llx ps=%016llx indexed=%u elements=%u "
                    "instances=%u first=%u base=%d first-instance=%u "
                    "ia-vertices=%llu ia-primitives=%llu vs-invocations=%llu "
                    "clipped-primitives=%llu fs-invocations=%llu\n",
                    (unsigned long long)draw_info->debug_submission,
                    draw_info->debug_list_index,
                    (unsigned long long)draw_info->debug_sequence,
                    (unsigned long long)draw_info->debug_vertex_hash,
                    (unsigned long long)draw_info->debug_pixel_hash,
                    draw_info->indexed, draw_info->element_count,
                    draw_info->instance_count, draw_info->start_element,
                    draw_info->base_vertex, draw_info->start_instance,
                    (unsigned long long)statistics[0],
                    (unsigned long long)statistics[1],
                    (unsigned long long)statistics[2],
                    (unsigned long long)statistics[3],
                    (unsigned long long)statistics[4]);
        } else {
            fprintf(stderr,
                    "[VULKAN DRAW] submission=%llu list=%u sequence=%llu "
                    "statistics-result=%d\n",
                    (unsigned long long)draw_info->debug_submission,
                    draw_info->debug_list_index,
                    (unsigned long long)draw_info->debug_sequence, result);
        }
    }
    for (uint32_t i = 0; i < target_count; ++i) {
        if (!targets[i].defer_publication)
            memcpy(targets[i].data,
                   g_graphics.staging_map + target_offsets[i],
                   target_extents[i]);
        ++*targets[i].serial;
        target_images[i].resource = targets[i].resource;
        target_images[i].serial = *targets[i].serial;
        target_images[i].content_valid = targets[i].resource != NULL;
        target_images[i].layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }
    if (depth_writeback || stencil_writeback) {
        for (uint32_t y = 0; y < depth->height; ++y) {
            const uint8_t *source = g_graphics.staging_map + depth_offset +
                (size_t)y * depth->width * depth->bytes_per_pixel;
            const uint8_t *stencil_source = g_graphics.staging_map +
                stencil_offset + (size_t)y * depth->width;
            uint8_t *destination = depth->data + (size_t)y * depth->row_pitch;
            for (uint32_t x = 0; x < depth->width; ++x) {
                uint8_t *destination_texel = destination +
                    (size_t)x * depth_source_bytes;
                if (depth_writeback) {
                    float value = 0.0f;
                    memcpy(&value,
                           source + (size_t)x * depth->bytes_per_pixel,
                           sizeof(value));
                    if (value < 0.0f) value = 0.0f;
                    if (value > 1.0f) value = 1.0f;
                    if (depth->source_format == 53 ||
                        depth->source_format == 55) {
                        uint16_t packed =
                            (uint16_t)(value * 65535.0f + 0.5f);
                        memcpy(destination_texel, &packed, sizeof(packed));
                    } else if (depth->source_format == 44 ||
                               depth->source_format == 45) {
                        uint32_t packed =
                            (uint32_t)(value * 16777215.0f + 0.5f);
                        if (depth->has_stencil)
                            packed |= (uint32_t)destination_texel[3] << 24;
                        memcpy(destination_texel, &packed, sizeof(packed));
                    } else {
                        memcpy(destination_texel, &value, sizeof(value));
                    }
                }
                if (stencil_writeback) {
                    if (depth->source_format == 44 ||
                        depth->source_format == 45)
                        destination_texel[3] = stencil_source[x];
                    else if (depth->stencil_byte_offset < depth_source_bytes)
                        destination_texel[depth->stencil_byte_offset] =
                            stencil_source[x];
                }
            }
        }
        ++*depth->serial;
    }
cleanup:
    /* Texel-buffer views reference the persistent staging allocation and are
     * retained in a bounded cache until that allocation is replaced. */
    /* Framebuffers are immutable and retained in the bounded cache. */
    for (uint32_t i = 0; i < target_count; ++i)
        recycle_cached_image(targets[i].format, targets[i].width,
            targets[i].height, 1, 1, 2, 0, 1, 1,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, targets[i].resource,
            targets[i].serial ? *targets[i].serial : 0, &target_images[i]);
    if (depth)
        recycle_cached_image(depth->format, depth->width, depth->height,
            1, 1, 2, 0, 1, depth->sample_count ? depth->sample_count : 1,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            (depth->sample_count > 1 ? VK_IMAGE_USAGE_SAMPLED_BIT :
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT),
            VK_IMAGE_ASPECT_DEPTH_BIT |
                (depth->has_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0),
            depth->sample_count > 1 ? depth->resource : NULL,
            depth->serial ? *depth->serial : 0, &depth_image);
    for (uint32_t i = 0; i < binding_count; ++i) {
        if (bindings[i].kind == BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_IMAGE ||
            bindings[i].kind == BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_DEPTH) {
            if (compute_mirrors[i].token) {
                beer_vulkan_compute_release_image_mirror(&compute_mirrors[i]);
                memset(&images[i], 0, sizeof(images[i]));
            } else {
                VkImageAspectFlags aspect = bindings[i].kind ==
                    BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_DEPTH ?
                    VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
                recycle_cached_image(bindings[i].format, bindings[i].width,
                    bindings[i].height,
                    bindings[i].layer_count ? bindings[i].layer_count : 1,
                    bindings[i].depth ? bindings[i].depth : 1,
                    bindings[i].image_dimension ?
                        bindings[i].image_dimension : 2,
                    bindings[i].image_cube,
                    bindings[i].mip_levels ? bindings[i].mip_levels : 1,
                    bindings[i].sample_count ? bindings[i].sample_count : 1,
                    VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    aspect, bindings[i].resource, bindings[i].serial ?
                        *bindings[i].serial : UINT64_MAX, &images[i]);
            }
        }
    }
    if (getenv("BEER_VULKAN_CACHE_TRACE")) cache_census("end");
    if (failure) REJECT(failure);
    uint64_t profile_end_ns = monotonic_nanoseconds();
    if (profile_end_ns >= profile_start_ns) {
        const uint64_t elapsed_ns = profile_end_ns - profile_start_ns;
        g_graphics.host_execution_ns += elapsed_ns;
        beer_vulkan_perf_account(1, elapsed_ns);
    }
    if (getenv("BEER_VULKAN_PERF") && profile_end_ns >= profile_start_ns &&
        profile_end_ns - profile_start_ns >= beer_vulkan_perf_threshold_ns()) {
        uint64_t setup_ns = profile_submit_ns >= profile_start_ns ?
            profile_submit_ns - profile_start_ns : 0;
        uint64_t wait_ns = profile_complete_ns >= profile_submit_ns ?
            profile_complete_ns - profile_submit_ns : 0;
        uint64_t finish_ns = profile_end_ns >= profile_complete_ns ?
            profile_end_ns - profile_complete_ns : 0;
        fprintf(stderr,
                "[VULKAN PERF] graphics total-ms=%llu.%03llu "
                "setup-ms=%llu.%03llu wait-ms=%llu.%03llu "
                "finish-ms=%llu.%03llu targets=%u bindings=%u bytes=%zu\n",
                (unsigned long long)((profile_end_ns - profile_start_ns) /
                                     UINT64_C(1000000)),
                (unsigned long long)(((profile_end_ns - profile_start_ns) /
                                      UINT64_C(1000)) % 1000),
                (unsigned long long)(setup_ns / UINT64_C(1000000)),
                (unsigned long long)((setup_ns / UINT64_C(1000)) % 1000),
                (unsigned long long)(wait_ns / UINT64_C(1000000)),
                (unsigned long long)((wait_ns / UINT64_C(1000)) % 1000),
                (unsigned long long)(finish_ns / UINT64_C(1000000)),
                (unsigned long long)((finish_ns / UINT64_C(1000)) % 1000),
                target_count, binding_count, total);
        size_t geometry_bytes = 0;
        if (draw_info) {
            for (uint32_t v = 0; v < draw_info->vertex_binding_count; ++v)
                geometry_bytes += draw_info->vertex_bindings[v].size;
            if (draw_info->indexed) geometry_bytes += draw_info->index_size;
        }
        fprintf(stderr,
                "[VULKAN PERF DETAIL] upload-images=%zu upload-buffers=%zu "
                "upload-targets=%zu reuse-graphics=%zu reuse-compute=%zu "
                "geometry=%zu\n",
                uploaded_image_bytes, uploaded_buffer_bytes,
                uploaded_target_bytes, graphics_mirror_bytes,
                compute_mirror_bytes, geometry_bytes);
        for (uint32_t i = 0; i < binding_count; ++i) {
            if ((bindings[i].kind != BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_IMAGE &&
                 bindings[i].kind != BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_DEPTH) ||
                skip_image_upload[i] || bindings[i].size < 16u * 1024u * 1024u)
                continue;
            fprintf(stderr,
                    "[VULKAN PERF UPLOAD] binding=%u resource=%p serial=%llu "
                    "bytes=%zu format=%u size=%ux%ux%u layers=%u mips=%u\n",
                    bindings[i].binding, bindings[i].resource,
                    (unsigned long long)(bindings[i].serial ?
                        *bindings[i].serial : UINT64_MAX), bindings[i].size,
                    bindings[i].format, bindings[i].width, bindings[i].height,
                    bindings[i].depth ? bindings[i].depth : 1,
                    bindings[i].layer_count ? bindings[i].layer_count : 1,
                    bindings[i].mip_levels ? bindings[i].mip_levels : 1);
        }
    }
    *reason = NULL;
    pthread_mutex_unlock(&g_lock);
    return 1;
}

int beer_vulkan_graphics_draw_fullscreen(
    const void *vertex_key, const uint32_t *vertex_spirv,
    size_t vertex_spirv_size, const void *pixel_key,
    const uint32_t *pixel_spirv, size_t pixel_spirv_size,
    const BeerVulkanGraphicsBinding *bindings, uint32_t binding_count,
    const BeerVulkanGraphicsDrawInfo *draw_info,
    BeerVulkanGraphicsTarget *target, uint32_t target_count,
    BeerVulkanGraphicsDepthTarget *depth, const float viewport[6],
    const int32_t scissor[4], const char **reason)
{
    return beer_vulkan_graphics_draw_state(
        vertex_key, vertex_spirv, vertex_spirv_size, pixel_key, pixel_spirv,
        pixel_spirv_size, bindings, binding_count, draw_info, NULL, target,
        target_count, depth, viewport, scissor, reason);
}

void beer_vulkan_graphics_get_stats(BeerVulkanGraphicsStats *stats)
{
    if (!stats) return;
    pthread_mutex_lock(&g_lock);
    stats->framebuffer_cache_hits = g_graphics.framebuffer_cache_hits;
    stats->framebuffer_creations = g_graphics.framebuffer_creations;
    stats->buffer_view_cache_hits = g_graphics.buffer_view_cache_hits;
    stats->buffer_view_creations = g_graphics.buffer_view_creations;
    stats->queue_submissions = g_graphics.queue_submissions;
    stats->fence_waits = g_graphics.fence_waits;
    stats->draw_commands = g_graphics.draw_commands;
    stats->host_execution_ns = g_graphics.host_execution_ns;
    pthread_mutex_unlock(&g_lock);
}

int beer_vulkan_graphics_sync_resource(const void *resource, uint64_t serial,
                                       uint8_t *data, size_t size,
                                       uint32_t width, uint32_t height,
                                       uint32_t format,
                                       uint32_t bytes_per_pixel)
{
    if (!resource || !data || !size || !width || !height ||
        !bytes_per_pixel || (size_t)width * bytes_per_pixel > SIZE_MAX / height)
        return 0;
    const size_t extent = (size_t)width * height * bytes_per_pixel;
    if (extent > size) return 0;

    pthread_mutex_lock(&g_lock);
    if (!initialize_locked() || beer_vulkan_device_lost()) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    CachedImage *match = NULL;
    for (uint32_t i = 0; i < IMAGE_CACHE_SIZE; ++i) {
        CachedImage *entry = &g_graphics.image_cache[i];
        if (entry->object.image && entry->object.content_valid &&
            entry->object.resource == resource && entry->object.serial == serial &&
            (!format || entry->format == format) && entry->width == width &&
            entry->height == height && entry->layers == 1 && entry->depth == 1 &&
            entry->dimension == 2 && entry->mip_levels == 1 &&
            entry->aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
            match = entry;
            break;
        }
    }
    if (!match || !ensure_staging(extent) ||
        g_graphics.ResetCommandBuffer(g_graphics.command_buffer, 0) != VK_SUCCESS) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    if (g_graphics.BeginCommandBuffer(g_graphics.command_buffer, &begin) !=
        VK_SUCCESS) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = match->object.layout,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = match->object.image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}
    };
    g_graphics.CmdPipelineBarrier(g_graphics.command_buffer,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &barrier);
    VkBufferImageCopy copy = {
        .bufferOffset = 0,
        .bufferRowLength = width,
        .bufferImageHeight = height,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {width, height, 1}
    };
    g_graphics.CmdCopyImageToBuffer(g_graphics.command_buffer,
        match->object.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        g_graphics.staging_buffer, 1, &copy);
    if (g_graphics.EndCommandBuffer(g_graphics.command_buffer) != VK_SUCCESS ||
        g_graphics.ResetFences(g_graphics.device, 1, &g_graphics.fence) !=
            VK_SUCCESS) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &g_graphics.command_buffer};
    beer_vulkan_queue_lock();
    VkResult submitted = g_graphics.QueueSubmit(g_graphics.queue, 1, &submit,
                                                 g_graphics.fence);
    beer_vulkan_queue_unlock();
    VkResult waited = submitted == VK_SUCCESS ? g_graphics.WaitForFences(
        g_graphics.device, 1, &g_graphics.fence, VK_TRUE, UINT64_MAX) : submitted;
    if (submitted != VK_SUCCESS || waited != VK_SUCCESS) {
        beer_vulkan_record_result(submitted != VK_SUCCESS ? submitted : waited,
                                  "graphics resource synchronization");
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    memcpy(data, g_graphics.staging_map, extent);
    match->object.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    match->last_use = ++g_graphics.image_use_serial;
    pthread_mutex_unlock(&g_lock);
    return 1;
}

void beer_vulkan_graphics_forget_resource(const void *resource)
{
    if (!resource) return;
    pthread_mutex_lock(&g_lock);
    for (uint32_t i = 0; i < IMAGE_CACHE_SIZE; ++i) {
        CachedImage *entry = &g_graphics.image_cache[i];
        if (entry->object.resource != resource) continue;
        if (entry->object.allocation_size >= 16u * 1024u * 1024u &&
            getenv("BEER_VULKAN_CACHE_TRACE")) {
            void *frames[6];
            int depth = backtrace(frames, 6);
            Dl_info info;
            size_t base = dladdr((void *)beer_vulkan_graphics_forget_resource,
                                 &info) ? (size_t)info.dli_fbase : 0;
            fprintf(stderr, "[VULKAN CACHE] forget resource=%p serial=%llu "
                    "bt=", resource,
                    (unsigned long long)entry->object.serial);
            for (int f = 1; f < depth; ++f)
                fprintf(stderr, "%zx ", (size_t)frames[f] - base);
            fprintf(stderr, "\n");
        }
        if (entry->object.allocation_size <= g_graphics.image_cache_bytes)
            g_graphics.image_cache_bytes -= entry->object.allocation_size;
        release_image(&entry->object);
        memset(entry, 0, sizeof(*entry));
    }
    pthread_mutex_unlock(&g_lock);
}

void beer_vulkan_graphics_invalidate_image_cache(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_graphics.initialized) {
        /* Clear all cached images to force re-upload after compute batch.
         * This ensures graphics sees fresh GPU data from compute-modified resources. */
        uint32_t cleared_count = 0;
        for (uint32_t i = 0; i < IMAGE_CACHE_SIZE; ++i) {
            CachedImage *entry = &g_graphics.image_cache[i];
            if (entry->object.resource) {
                if (entry->object.allocation_size <=
                    g_graphics.image_cache_bytes)
                    g_graphics.image_cache_bytes -=
                        entry->object.allocation_size;
                release_image(&entry->object);
                memset(entry, 0, sizeof(*entry));
                ++cleared_count;
            }
        }
        if (getenv("BEER_D3D12_DIAGNOSTICS"))
            fprintf(stderr, "[VULKAN GRAPHICS] cache invalidated, cleared %u entries\n", cleared_count);
    }
    pthread_mutex_unlock(&g_lock);
}

void beer_vulkan_graphics_destroy(void)
{
    pthread_mutex_lock(&g_lock);
    destroy_locked();
    pthread_mutex_unlock(&g_lock);
}
