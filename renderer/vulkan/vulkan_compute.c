#define _GNU_SOURCE
#include "vulkan_compute.h"
#include "vulkan_context.h"
#include "vulkan_graphics.h"

#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PIPELINES 512u
#define MAX_STAGING_BYTES ((size_t)512 * 1024 * 1024)
#define REGION_ALIGNMENT 256u
#define IMAGE_CACHE_SIZE 64u
#define BUFFER_CACHE_SIZE 64u
#define BUFFER_CACHE_MAX_BYTES ((size_t)64 * 1024 * 1024)
/* Keep the current compute working set resident without letting this cache
 * independently consume most of the process budget. Cache misses remain a
 * correctness-safe upload fallback; the fixed LRU avoids the historical
 * one-slot thrash that originally motivated multi-GiB limits. */
#define IMAGE_CACHE_MAX_BYTES ((size_t)384 * 1024 * 1024)
/* Descriptor sets remain alive for a complete compute IR run. This is sized
 * above observed command-list batches while keeping descriptor-pool ownership
 * bounded; an oversized run is explicitly rejected instead of silently
 * resetting sets that may already be recorded into a shared command buffer. */
#define MAX_BATCH_DESCRIPTOR_SETS 512u
/* Tiled compute submissions need distinct command buffers until the final
 * fence completes. This ring covers the largest observed dispatch (920 tiles)
 * with one host wait while keeping command-buffer ownership bounded. */
#define MAX_TILE_COMMAND_BUFFERS 1024u
#define SAMPLER_CACHE_SIZE 256u

typedef struct ReflectedBinding {
    uint32_t binding;
    uint32_t uniform;
    uint32_t texel;       /* 0 none, 1 uniform texel, 2 storage texel */
    uint32_t texel_format; /* VkFormat required by a storage texel/image;
                            * zero when the shader declares Unknown */
    uint32_t image;       /* 0 none, 1 sampled image, 2 storage image */
    uint32_t image_dimension; /* 1, 2, or 3 for non-buffer images */
    uint32_t image_arrayed;
    uint32_t image_cube;
    uint32_t sampler;     /* separate sampler descriptor */
} ReflectedBinding;

typedef struct PipelineEntry {
    const void *key;
    size_t spirv_size;
    int failed;
    const char *failure;
    VkDescriptorSetLayout set_layout;
    VkDescriptorSet descriptor_set;
    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;
    uint32_t binding_count;
    uint32_t loop_count;
    uint32_t control_barrier_count;
    /* Learned safe quantum for barrier-free storage-image loops. The previous
     * implementation restarted at 16 groups every frame and paid the same
     * submit/fence ramp repeatedly. */
    uint32_t adaptive_tile_groups;
    ReflectedBinding bindings[BEER_VULKAN_COMPUTE_MAX_BINDINGS];
} PipelineEntry;

typedef struct Region {
    uint8_t *data;
    size_t size;
    size_t offset;
    size_t padded_size;
    int writable;
    BeerVulkanComputeBinding *image; /* first image binding, or NULL */
    int sampled;
    int storage;
    int skip_upload;
    int skip_buffer_upload;
} Region;

/* Objects created while preparing one dispatch. In scalar mode they are
 * destroyed before the call returns; in an outer IR batch successful
 * dispatches transfer them to batch ownership so descriptor references remain
 * valid until batch finalization. */
typedef struct Transients {
    VkBufferView buffer_views[BEER_VULKAN_COMPUTE_MAX_BINDINGS];
    uint32_t buffer_view_count;
    VkImageView image_views[BEER_VULKAN_COMPUTE_MAX_BINDINGS];
    uint32_t image_view_count;
    VkSampler samplers[BEER_VULKAN_COMPUTE_MAX_BINDINGS];
    uint32_t sampler_count;
    VkImage images[BEER_VULKAN_COMPUTE_MAX_BINDINGS];
    VkDeviceMemory image_memory[BEER_VULKAN_COMPUTE_MAX_BINDINGS];
    uint8_t host_writeback_pending[BEER_VULKAN_COMPUTE_MAX_BINDINGS];
    uint64_t published_serial[BEER_VULKAN_COMPUTE_MAX_BINDINGS];
    BeerVulkanComputeBinding host_binding[BEER_VULKAN_COMPUTE_MAX_BINDINGS];
    uint32_t image_count; /* indexed by region */
} Transients;

typedef struct BatchTransients {
    Transients operations[MAX_BATCH_DESCRIPTOR_SETS];
    uint32_t count;
} BatchTransients;

typedef struct SamplerKey {
    uint32_t min_filter;
    uint32_t mag_filter;
    uint32_t mipmap_mode;
    uint32_t address_u;
    uint32_t address_v;
    uint32_t address_w;
    uint32_t anisotropy_enable;
    float max_anisotropy;
    uint32_t compare_enable;
    uint32_t compare_op;
    uint32_t reduction_mode;
} SamplerKey;

typedef struct SamplerEntry {
    SamplerKey key;
    VkSampler sampler;
} SamplerEntry;

typedef struct CachedBuffer {
    const void *resource;
    uint64_t serial;
    size_t size;
    size_t allocation_size;
    uint64_t last_use;
    int content_valid;
    VkBuffer buffer;
    VkDeviceMemory memory;
} CachedBuffer;

typedef struct CachedImage {
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t layers;
    uint32_t mip_levels;
    uint32_t dimension;
    uint32_t cube;
    VkImageUsageFlags usage;
    size_t allocation_size;
    uint64_t last_use;
    uint32_t pin_count;
    const void *resource;
    uint64_t serial;
    VkImageLayout layout;
    int content_valid;
    /* Host-authoritative cache entries need one upload before use even when
     * their D3D content serial did not change. */
    int needs_upload;
    int host_writeback_pending;
    uint64_t published_serial;
    BeerVulkanComputeBinding host_binding;
    VkImage image;
    VkDeviceMemory memory;
} CachedImage;

static struct {
    int initialized;
    int unavailable;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkCommandPool command_pool;
    VkCommandBuffer command_buffers[MAX_TILE_COMMAND_BUFFERS];
    uint32_t command_buffer_count;
    VkFence fence;
    VkDescriptorPool descriptor_pool;
    VkBuffer staging_buffer;
    VkDeviceMemory staging_memory;
    uint8_t *staging_map;
    size_t staging_capacity;
    /* Host-cached readback buffer. Upload staging is write-combined, where
     * CPU reads run at uncached speeds (profiled at ~45% of main-thread CPU
     * during mirror writeback). */
    VkBuffer readback_buffer;
    VkDeviceMemory readback_memory;
    uint8_t *readback_map;
    size_t readback_capacity;
    uint32_t memory_type;
    VkPhysicalDeviceLimits limits;
    VkPhysicalDeviceMemoryProperties memory_properties;
    size_t image_cache_bytes;
    PipelineEntry *pipelines;
    uint32_t pipeline_count;
    uint64_t executed;
    uint64_t rejected;
    uint64_t queue_submissions;
    uint64_t completed_submissions;
    uint64_t host_fence_waits;
    uint64_t host_execution_ns;
    SamplerEntry sampler_cache[SAMPLER_CACHE_SIZE];
    uint32_t sampler_count;
    uint64_t sampler_cache_hits;
    uint64_t sampler_creations;
    uint64_t descriptor_set_cache_hits;
    uint64_t descriptor_set_allocations;
    uint64_t image_cache_hits;
    uint64_t image_cache_evictions;
    uint64_t epoch_calls;
    uint64_t epoch_operations;
    uint64_t multi_operation_epochs;
    uint64_t shareable_epoch_calls;
    uint64_t shareable_epoch_operations;
    uint64_t shared_epoch_calls;
    uint64_t shared_epoch_operations;
    uint64_t watchdog_epoch_fallbacks;
    uint32_t max_epoch_operations;
    CachedImage image_cache[IMAGE_CACHE_SIZE];
    uint64_t image_use_serial;
    CachedBuffer buffer_cache[BUFFER_CACHE_SIZE];
    size_t buffer_cache_bytes;
    uint64_t buffer_use_serial;

    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties;
    PFN_vkCreateBufferView CreateBufferView;
    PFN_vkDestroyBufferView DestroyBufferView;
    PFN_vkCreateImage CreateImage;
    PFN_vkDestroyImage DestroyImage;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
    PFN_vkBindImageMemory BindImageMemory;
    PFN_vkCreateImageView CreateImageView;
    PFN_vkDestroyImageView DestroyImageView;
    PFN_vkCreateSampler CreateSampler;
    PFN_vkDestroySampler DestroySampler;
    PFN_vkCmdCopyBuffer CmdCopyBuffer;
    PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage;
    PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer;
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkResetCommandBuffer ResetCommandBuffer;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout;
    PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout;
    PFN_vkCreateDescriptorPool CreateDescriptorPool;
    PFN_vkDestroyDescriptorPool DestroyDescriptorPool;
    PFN_vkResetDescriptorPool ResetDescriptorPool;
    PFN_vkAllocateDescriptorSets AllocateDescriptorSets;
    PFN_vkFreeDescriptorSets FreeDescriptorSets;
    PFN_vkUpdateDescriptorSets UpdateDescriptorSets;
    PFN_vkCreateShaderModule CreateShaderModule;
    PFN_vkDestroyShaderModule DestroyShaderModule;
    PFN_vkCreatePipelineLayout CreatePipelineLayout;
    PFN_vkDestroyPipelineLayout DestroyPipelineLayout;
    PFN_vkCreateComputePipelines CreateComputePipelines;
    PFN_vkDestroyPipeline DestroyPipeline;
    PFN_vkCreateBuffer CreateBuffer;
    PFN_vkDestroyBuffer DestroyBuffer;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkBindBufferMemory BindBufferMemory;
    PFN_vkMapMemory MapMemory;
    PFN_vkUnmapMemory UnmapMemory;
    PFN_vkCmdBindPipeline CmdBindPipeline;
    PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets;
    PFN_vkCmdDispatch CmdDispatch;
    PFN_vkCmdDispatchBase CmdDispatchBase;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkCreateFence CreateFence;
    PFN_vkDestroyFence DestroyFence;
    PFN_vkResetFences ResetFences;
    PFN_vkWaitForFences WaitForFences;
} g_compute;

static pthread_mutex_t g_compute_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned g_compute_batch_depth;
static uint32_t g_compute_batch_descriptor_sets;
static int g_compute_batch_descriptor_pool_ready;
static size_t g_compute_batch_staging_reservation;
static int g_compute_batch_staging_fixed;
static BatchTransients g_compute_batch_transients;

static uint64_t monotonic_nanoseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

#define LOAD_INSTANCE(name) do { \
    g_compute.name = (PFN_vk##name)beer_vulkan_get_instance_proc_addr()( \
        beer_vulkan_instance(), "vk" #name); \
    if (!g_compute.name) goto fail; \
} while (0)
#define LOAD_DEVICE(name) do { \
    g_compute.name = (PFN_vk##name)beer_vulkan_get_device_proc_addr()( \
        g_compute.device, "vk" #name); \
    if (!g_compute.name) goto fail; \
} while (0)

static void release_readback(void)
{
    if (g_compute.readback_map)
        g_compute.UnmapMemory(g_compute.device, g_compute.readback_memory);
    if (g_compute.readback_buffer)
        g_compute.DestroyBuffer(g_compute.device, g_compute.readback_buffer,
                                NULL);
    if (g_compute.readback_memory)
        g_compute.FreeMemory(g_compute.device, g_compute.readback_memory,
                             NULL);
    g_compute.readback_map = NULL;
    g_compute.readback_buffer = VK_NULL_HANDLE;
    g_compute.readback_memory = VK_NULL_HANDLE;
    g_compute.readback_capacity = 0;
}

/* Ensure a coherent, host-cached transfer-destination buffer of at least
 * `size` bytes. Returns 0 if no such memory type exists. All users wait on a
 * fence before reading, so the old buffer is never in flight when replaced. */
static int ensure_readback_buffer(size_t size)
{
    if (g_compute.readback_buffer && g_compute.readback_capacity >= size)
        return 1;
    release_readback();
    VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    if (g_compute.CreateBuffer(g_compute.device, &info, NULL,
                               &g_compute.readback_buffer) != VK_SUCCESS)
        goto fail;
    VkMemoryRequirements requirements;
    g_compute.GetBufferMemoryRequirements(g_compute.device,
                                          g_compute.readback_buffer,
                                          &requirements);
    const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    int type = -1;
    for (uint32_t t = 0; t < g_compute.memory_properties.memoryTypeCount; ++t)
        if ((requirements.memoryTypeBits & (1u << t)) &&
            (g_compute.memory_properties.memoryTypes[t].propertyFlags &
             wanted) == wanted) {
            type = (int)t;
            break;
        }
    if (type < 0) goto fail;
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = (uint32_t)type
    };
    void *mapped = NULL;
    if (g_compute.AllocateMemory(g_compute.device, &allocation, NULL,
                                 &g_compute.readback_memory) != VK_SUCCESS ||
        g_compute.BindBufferMemory(g_compute.device, g_compute.readback_buffer,
                                   g_compute.readback_memory, 0) != VK_SUCCESS ||
        g_compute.MapMemory(g_compute.device, g_compute.readback_memory, 0,
                            VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS || !mapped)
        goto fail;
    g_compute.readback_map = mapped;
    g_compute.readback_capacity = size;
    return 1;
fail:
    release_readback();
    return 0;
}

static void release_staging(void)
{
    release_readback();
    if (g_compute.staging_map)
        g_compute.UnmapMemory(g_compute.device, g_compute.staging_memory);
    if (g_compute.staging_buffer)
        g_compute.DestroyBuffer(g_compute.device, g_compute.staging_buffer,
                                NULL);
    if (g_compute.staging_memory)
        g_compute.FreeMemory(g_compute.device, g_compute.staging_memory, NULL);
    g_compute.staging_map = NULL;
    g_compute.staging_buffer = VK_NULL_HANDLE;
    g_compute.staging_memory = VK_NULL_HANDLE;
    g_compute.staging_capacity = 0;
}

static VkSampler find_or_create_sampler(
    const BeerVulkanComputeBinding *source)
{
    SamplerKey key = {
        .min_filter = source->min_filter,
        .mag_filter = source->mag_filter,
        .mipmap_mode = source->mipmap_mode,
        .address_u = source->address_u,
        .address_v = source->address_v,
        .address_w = source->address_w,
        .anisotropy_enable = source->anisotropy_enable,
        .max_anisotropy = source->anisotropy_enable ?
            (source->max_anisotropy < beer_vulkan_max_sampler_anisotropy() ?
             source->max_anisotropy : beer_vulkan_max_sampler_anisotropy()) :
            1.0f,
        .compare_enable = source->compare_enable,
        .compare_op = source->compare_op,
        .reduction_mode = source->reduction_mode
    };
    for (uint32_t i = 0; i < g_compute.sampler_count; ++i) {
        if (memcmp(&g_compute.sampler_cache[i].key, &key, sizeof(key)) == 0) {
            ++g_compute.sampler_cache_hits;
            return g_compute.sampler_cache[i].sampler;
        }
    }
    if (g_compute.sampler_count >= SAMPLER_CACHE_SIZE) return VK_NULL_HANDLE;

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
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
    };
    VkSamplerReductionModeCreateInfo reduction = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO,
        .reductionMode = key.reduction_mode == 1 ?
            VK_SAMPLER_REDUCTION_MODE_MIN : VK_SAMPLER_REDUCTION_MODE_MAX
    };
    if (key.reduction_mode) info.pNext = &reduction;

    SamplerEntry *entry = &g_compute.sampler_cache[g_compute.sampler_count];
    if (g_compute.CreateSampler(g_compute.device, &info, NULL,
                                &entry->sampler) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    entry->key = key;
    ++g_compute.sampler_count;
    ++g_compute.sampler_creations;
    return entry->sampler;
}

static void destroy_pipelines(void)
{
    for (uint32_t i = 0; i < g_compute.pipeline_count; ++i) {
        PipelineEntry *entry = &g_compute.pipelines[i];
        if (entry->pipeline)
            g_compute.DestroyPipeline(g_compute.device, entry->pipeline, NULL);
        if (entry->pipeline_layout)
            g_compute.DestroyPipelineLayout(g_compute.device,
                                            entry->pipeline_layout, NULL);
        if (entry->set_layout)
            g_compute.DestroyDescriptorSetLayout(g_compute.device,
                                                 entry->set_layout, NULL);
    }
    free(g_compute.pipelines);
    g_compute.pipelines = NULL;
    g_compute.pipeline_count = 0;
}

static void destroy_locked(void)
{
    if (g_compute.device) {
        release_staging();
        for (uint32_t i = 0; i < IMAGE_CACHE_SIZE; ++i) {
            if (g_compute.image_cache[i].image)
                g_compute.DestroyImage(g_compute.device,
                                       g_compute.image_cache[i].image, NULL);
            if (g_compute.image_cache[i].memory)
                g_compute.FreeMemory(g_compute.device,
                                     g_compute.image_cache[i].memory, NULL);
        }
        for (uint32_t i = 0; i < BUFFER_CACHE_SIZE; ++i) {
            if (g_compute.buffer_cache[i].buffer)
                g_compute.DestroyBuffer(g_compute.device,
                                        g_compute.buffer_cache[i].buffer, NULL);
            if (g_compute.buffer_cache[i].memory)
                g_compute.FreeMemory(g_compute.device,
                                     g_compute.buffer_cache[i].memory, NULL);
        }
        for (uint32_t i = 0; i < g_compute.sampler_count; ++i)
            if (g_compute.sampler_cache[i].sampler)
                g_compute.DestroySampler(g_compute.device,
                    g_compute.sampler_cache[i].sampler, NULL);
        destroy_pipelines();
        if (g_compute.fence)
            g_compute.DestroyFence(g_compute.device, g_compute.fence, NULL);
        if (g_compute.descriptor_pool)
            g_compute.DestroyDescriptorPool(g_compute.device,
                                            g_compute.descriptor_pool, NULL);
        if (g_compute.command_pool)
            g_compute.DestroyCommandPool(g_compute.device,
                                         g_compute.command_pool, NULL);
    }
    memset(&g_compute, 0, sizeof(g_compute));
}

static int initialize_locked(void)
{
    if (g_compute.initialized) return 1;
    if (g_compute.unavailable) return 0;
    if (!beer_vulkan_context_initialize()) goto fail;
    g_compute.physical_device = beer_vulkan_physical_device();
    g_compute.device = beer_vulkan_device();
    g_compute.queue = beer_vulkan_queue();
    g_compute.queue_family = beer_vulkan_queue_family();
    if (!g_compute.physical_device || !g_compute.device || !g_compute.queue ||
        !beer_vulkan_get_instance_proc_addr() ||
        !beer_vulkan_get_device_proc_addr())
        goto fail;

    do {
        LOAD_INSTANCE(GetPhysicalDeviceProperties);
        LOAD_INSTANCE(GetPhysicalDeviceMemoryProperties);
        LOAD_INSTANCE(GetPhysicalDeviceFormatProperties);
        LOAD_DEVICE(CreateBufferView); LOAD_DEVICE(DestroyBufferView);
        LOAD_DEVICE(CreateImage); LOAD_DEVICE(DestroyImage);
        LOAD_DEVICE(GetImageMemoryRequirements); LOAD_DEVICE(BindImageMemory);
        LOAD_DEVICE(CreateImageView); LOAD_DEVICE(DestroyImageView);
        LOAD_DEVICE(CreateSampler); LOAD_DEVICE(DestroySampler);
        LOAD_DEVICE(CmdCopyBuffer); LOAD_DEVICE(CmdCopyBufferToImage);
        LOAD_DEVICE(CmdCopyImageToBuffer);
        LOAD_DEVICE(CreateCommandPool); LOAD_DEVICE(DestroyCommandPool);
        LOAD_DEVICE(AllocateCommandBuffers); LOAD_DEVICE(ResetCommandBuffer);
        LOAD_DEVICE(BeginCommandBuffer); LOAD_DEVICE(EndCommandBuffer);
        LOAD_DEVICE(CreateDescriptorSetLayout);
        LOAD_DEVICE(DestroyDescriptorSetLayout);
        LOAD_DEVICE(CreateDescriptorPool); LOAD_DEVICE(DestroyDescriptorPool);
        LOAD_DEVICE(ResetDescriptorPool); LOAD_DEVICE(AllocateDescriptorSets);
        LOAD_DEVICE(FreeDescriptorSets); LOAD_DEVICE(UpdateDescriptorSets);
        LOAD_DEVICE(CreateShaderModule); LOAD_DEVICE(DestroyShaderModule);
        LOAD_DEVICE(CreatePipelineLayout); LOAD_DEVICE(DestroyPipelineLayout);
        LOAD_DEVICE(CreateComputePipelines); LOAD_DEVICE(DestroyPipeline);
        LOAD_DEVICE(CreateBuffer); LOAD_DEVICE(DestroyBuffer);
        LOAD_DEVICE(GetBufferMemoryRequirements);
        LOAD_DEVICE(AllocateMemory); LOAD_DEVICE(FreeMemory);
        LOAD_DEVICE(BindBufferMemory); LOAD_DEVICE(MapMemory);
        LOAD_DEVICE(UnmapMemory);
        LOAD_DEVICE(CmdBindPipeline); LOAD_DEVICE(CmdBindDescriptorSets);
        LOAD_DEVICE(CmdDispatch); LOAD_DEVICE(CmdDispatchBase);
        LOAD_DEVICE(CmdPipelineBarrier);
        LOAD_DEVICE(QueueSubmit); LOAD_DEVICE(CreateFence);
        LOAD_DEVICE(DestroyFence); LOAD_DEVICE(ResetFences);
        LOAD_DEVICE(WaitForFences);
    } while (0);

    VkPhysicalDeviceProperties properties;
    g_compute.GetPhysicalDeviceProperties(g_compute.physical_device,
                                          &properties);
    g_compute.limits = properties.limits;
    g_compute.GetPhysicalDeviceMemoryProperties(
        g_compute.physical_device, &g_compute.memory_properties);

    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = g_compute.queue_family
    };
    if (g_compute.CreateCommandPool(g_compute.device, &pool_info, NULL,
                                    &g_compute.command_pool) != VK_SUCCESS)
        goto fail;
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = g_compute.command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = MAX_TILE_COMMAND_BUFFERS
    };
    if (g_compute.AllocateCommandBuffers(g_compute.device, &command_info,
                                         g_compute.command_buffers) !=
        VK_SUCCESS)
        goto fail;
    g_compute.command_buffer_count = MAX_TILE_COMMAND_BUFFERS;
    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
    };
    if (g_compute.CreateFence(g_compute.device, &fence_info, NULL,
                              &g_compute.fence) != VK_SUCCESS)
        goto fail;

    VkDescriptorPoolSize sizes[7] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         BEER_VULKAN_COMPUTE_MAX_BINDINGS * MAX_BATCH_DESCRIPTOR_SETS},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
         BEER_VULKAN_COMPUTE_MAX_BINDINGS * MAX_BATCH_DESCRIPTOR_SETS},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
         BEER_VULKAN_COMPUTE_MAX_BINDINGS * MAX_BATCH_DESCRIPTOR_SETS},
        {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,
         BEER_VULKAN_COMPUTE_MAX_BINDINGS * MAX_BATCH_DESCRIPTOR_SETS},
        {VK_DESCRIPTOR_TYPE_SAMPLER,
         BEER_VULKAN_COMPUTE_MAX_BINDINGS * MAX_BATCH_DESCRIPTOR_SETS},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
         BEER_VULKAN_COMPUTE_MAX_BINDINGS * MAX_BATCH_DESCRIPTOR_SETS},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         BEER_VULKAN_COMPUTE_MAX_BINDINGS * MAX_BATCH_DESCRIPTOR_SETS}
    };
    VkDescriptorPoolCreateInfo descriptor_pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        /* FREE_DESCRIPTOR_SET allows individual sets to be returned to the pool
         * after the per-epoch fence completes, rather than requiring a full pool
         * reset.  The resource-bearing shared epoch recorder allocates one set
         * per operation and frees them in release_epoch_op_contexts(). */
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = MAX_BATCH_DESCRIPTOR_SETS,
        .poolSizeCount = 7,
        .pPoolSizes = sizes
    };
    if (g_compute.CreateDescriptorPool(g_compute.device,
                                       &descriptor_pool_info, NULL,
                                       &g_compute.descriptor_pool) !=
        VK_SUCCESS)
        goto fail;

    g_compute.initialized = 1;
    fprintf(stderr, "[D3D12 VULKAN COMPUTE] initialized\n");
    return 1;
fail:
    fprintf(stderr, "[D3D12 VULKAN COMPUTE] unavailable\n");
    destroy_locked();
    g_compute.unavailable = 1;
    return 0;
}

/* SPIR-V ImageFormat -> VkFormat for formats whose storage-image/texel use is
 * meaningful for D3D (no SNORM variants). Returns 0 when unmapped. */
static uint32_t spirv_format_to_vk(uint32_t format)
{
    static const uint16_t table[40] = {
        [1] = 109, [2] = 97, [3] = 100, [4] = 37, [6] = 103, [7] = 83,
        [8] = 122, [9] = 76, [10] = 91, [11] = 64, [12] = 77, [13] = 16,
        [14] = 70, [15] = 9, [21] = 108, [22] = 96, [23] = 42, [24] = 99,
        [25] = 102, [26] = 82, [27] = 17, [28] = 75, [29] = 14, [30] = 107,
        [31] = 95, [32] = 41, [33] = 98, [34] = 68, [35] = 101, [36] = 81,
        [37] = 15, [38] = 74, [39] = 13
    };
    return format < 40 ? table[format] : 0;
}

static int capability_supported(uint32_t capability)
{
    const VkPhysicalDeviceFeatures *features = beer_vulkan_enabled_features();
    switch (capability) {
    case 0:  /* Matrix */
    case 1:  /* Shader */
    case 43: /* Sampled1D */
    case 44: /* Image1D */
    case 46: /* SampledBuffer */
    case 47: /* ImageBuffer */
    case 50: /* ImageQuery */
        return 1;
    case 10: return features->shaderFloat64;
    case 11: return features->shaderInt64;
    case 22: return features->shaderInt16;
    case 25: return features->shaderImageGatherExtended;
    case 49: return features->shaderStorageImageExtendedFormats;
    case 55: return features->shaderStorageImageReadWithoutFormat;
    case 56: return features->shaderStorageImageWriteWithoutFormat;
    default: return 0;
    }
}

/* Minimal SPIR-V reflection. Returns NULL on success or a static rejection
 * reason. Descriptors backed by uniform/storage buffers, typed texel buffers,
 * 2D sampled/storage images, and samplers in set zero are accepted; descriptor
 * arrays, other image shapes, and any capability the device was not created
 * with are rejected. */
static const char *reflect_spirv(const uint32_t *code, size_t word_count,
                                 ReflectedBinding *out, uint32_t *out_count)
{
    enum { KIND_NONE, KIND_STRUCT, KIND_ARRAY, KIND_POINTER, KIND_TEXEL,
           KIND_STORAGE_TEXEL, KIND_IMAGE_SAMPLED, KIND_IMAGE_STORAGE,
           KIND_SAMPLER };
    int unsupported_capability = 0;
    *out_count = 0;
    if (word_count < 5 || code[0] != 0x07230203u) return "invalid SPIR-V";
    uint32_t bound = code[3];
    if (!bound || bound > (1u << 22)) return "invalid SPIR-V id bound";

    uint32_t *binding = calloc(bound, sizeof(*binding));
    uint32_t *set = calloc(bound, sizeof(*set));
    uint8_t *flags = calloc(bound, 1); /* 1=binding 2=block 4=bufferblock */
    uint8_t *kind = calloc(bound, 1);
    uint32_t *pointee = calloc(bound, sizeof(*pointee));
    uint32_t *pointer_class = calloc(bound, sizeof(*pointer_class));
    uint32_t *image_dimension = calloc(bound, sizeof(*image_dimension));
    uint8_t *image_arrayed = calloc(bound, sizeof(*image_arrayed));
    uint8_t *image_cube = calloc(bound, sizeof(*image_cube));
    uint32_t *variable_id = calloc(bound, sizeof(*variable_id));
    uint32_t *variable_type = calloc(bound, sizeof(*variable_type));
    uint32_t *variable_class = calloc(bound, sizeof(*variable_class));
    uint32_t variable_count = 0;
    const char *reason = NULL;
    if (!binding || !set || !flags || !kind || !pointee || !pointer_class ||
        !image_dimension || !image_arrayed || !image_cube || !variable_id ||
        !variable_type ||
        !variable_class) {
        reason = "out of memory";
        goto done;
    }

    size_t i = 5;
    while (i < word_count) {
        uint32_t word = code[i];
        uint32_t opcode = word & 0xffffu;
        uint32_t length = word >> 16;
        if (!length || length > word_count - i) {
            reason = "malformed SPIR-V";
            goto done;
        }
        const uint32_t *op = code + i;
        switch (opcode) {
        case 17: /* OpCapability */
            if (length < 2 || !capability_supported(op[1])) {
                fprintf(stderr,
                        "[D3D12 VULKAN COMPUTE] shader requires SPIR-V "
                        "capability %u\n", length < 2 ? 0u : op[1]);
                unsupported_capability = 1;
            }
            break;
        case 71: /* OpDecorate */
            if (length >= 3 && op[1] < bound) {
                if (op[2] == 33 && length >= 4) {
                    binding[op[1]] = op[3];
                    flags[op[1]] |= 1;
                } else if (op[2] == 34 && length >= 4) {
                    set[op[1]] = op[3];
                } else if (op[2] == 2) {
                    flags[op[1]] |= 2;
                } else if (op[2] == 3) {
                    flags[op[1]] |= 4;
                }
            }
            break;
        case 25: /* OpTypeImage: result, sampled type, dim, depth, arrayed,
                  * ms, sampled, format */
            if (length >= 9 && op[1] < bound && op[4] != 1 && op[5] <= 1 &&
                op[6] == 0) {
                uint32_t vk_format = spirv_format_to_vk(op[8]);
                if (op[3] == 5 && op[7] == 1) {
                    kind[op[1]] = KIND_TEXEL;
                } else if (op[3] == 5 && op[7] == 2 &&
                           (op[8] == 0 || vk_format)) {
                    kind[op[1]] = KIND_STORAGE_TEXEL;
                    pointee[op[1]] = vk_format;
                } else if ((op[3] <= 2 || op[3] == 3) && op[7] == 1) {
                    kind[op[1]] = KIND_IMAGE_SAMPLED;
                    /* DimCube is a 2D cube-compatible image; other dimensions
                     * map directly to Vulkan 1D/2D/3D. */
                    image_dimension[op[1]] = op[3] == 3 ? 2 : op[3] + 1;
                    image_arrayed[op[1]] = op[5] != 0;
                    image_cube[op[1]] = op[3] == 3;
                } else if (op[3] <= 2 && op[7] == 2 &&
                           (op[8] == 0 || vk_format)) {
                    kind[op[1]] = KIND_IMAGE_STORAGE;
                    pointee[op[1]] = vk_format;
                    image_dimension[op[1]] = op[3] + 1;
                    image_arrayed[op[1]] = op[5] != 0;
                }
            }
            break;
        case 26: /* OpTypeSampler */
            if (length >= 2 && op[1] < bound) kind[op[1]] = KIND_SAMPLER;
            break;
        case 30: /* OpTypeStruct */
            if (length >= 2 && op[1] < bound) kind[op[1]] = KIND_STRUCT;
            break;
        case 28: /* OpTypeArray */
        case 29: /* OpTypeRuntimeArray */
            if (length >= 3 && op[1] < bound) {
                kind[op[1]] = KIND_ARRAY;
                pointee[op[1]] = op[2];
            }
            break;
        case 32: /* OpTypePointer */
            if (length >= 4 && op[1] < bound) {
                kind[op[1]] = KIND_POINTER;
                pointer_class[op[1]] = op[2];
                pointee[op[1]] = op[3];
            }
            break;
        case 59: /* OpVariable */
            if (length >= 4 && op[1] < bound && op[2] < bound) {
                variable_type[variable_count] = op[1];
                variable_id[variable_count] = op[2];
                variable_class[variable_count] = op[3];
                ++variable_count;
            }
            break;
        default:
            break;
        }
        i += length;
    }

    for (uint32_t v = 0; v < variable_count; ++v) {
        uint32_t id = variable_id[v];
        if (!(flags[id] & 1)) continue;
        if (set[id] != 0) { reason = "descriptor set other than zero"; goto done; }
        if (variable_class[v] == 0) {
            uint32_t image_pointer = variable_type[v];
            uint32_t image_type = kind[image_pointer] == KIND_POINTER ?
                pointee[image_pointer] : bound;
            if (image_type >= bound ||
                (kind[image_type] != KIND_TEXEL &&
                 kind[image_type] != KIND_STORAGE_TEXEL &&
                 kind[image_type] != KIND_IMAGE_SAMPLED &&
                 kind[image_type] != KIND_IMAGE_STORAGE &&
                 kind[image_type] != KIND_SAMPLER)) {
                reason = "unsupported image shape or descriptor array";
                goto done;
            }
            if (*out_count >= BEER_VULKAN_COMPUTE_MAX_BINDINGS) {
                reason = "too many descriptors";
                goto done;
            }
            memset(&out[*out_count], 0, sizeof(out[*out_count]));
            uint32_t image_kind = kind[image_type];
            out[*out_count].texel = image_kind == KIND_TEXEL ? 1 :
                image_kind == KIND_STORAGE_TEXEL ? 2 : 0;
            out[*out_count].image = image_kind == KIND_IMAGE_SAMPLED ? 1 :
                image_kind == KIND_IMAGE_STORAGE ? 2 : 0;
            out[*out_count].image_dimension = out[*out_count].image ?
                image_dimension[image_type] : 0;
            out[*out_count].image_arrayed = out[*out_count].image ?
                image_arrayed[image_type] : 0;
            out[*out_count].image_cube = out[*out_count].image ?
                image_cube[image_type] : 0;
            out[*out_count].sampler = image_kind == KIND_SAMPLER;
            out[*out_count].texel_format = (image_kind == KIND_STORAGE_TEXEL ||
                image_kind == KIND_IMAGE_STORAGE) ? pointee[image_type] : 0;
            out[*out_count].binding = binding[id];
            ++*out_count;
            continue;
        }
        if (variable_class[v] != 2) {
            reason = "StorageBuffer-class descriptor";
            goto done;
        }
        uint32_t pointer = variable_type[v];
        if (kind[pointer] != KIND_POINTER) { reason = "untyped variable"; goto done; }
        uint32_t target = pointee[pointer];
        if (target >= bound || kind[target] != KIND_STRUCT) {
            reason = "descriptor array or non-struct buffer";
            goto done;
        }
        if (*out_count >= BEER_VULKAN_COMPUTE_MAX_BINDINGS) {
            reason = "too many descriptors";
            goto done;
        }
        if (flags[target] & 2) {
            out[*out_count].uniform = 1;
        } else if (flags[target] & 4) {
            out[*out_count].uniform = 0;
        } else {
            reason = "unrecognized buffer block";
            goto done;
        }
        out[*out_count].binding = binding[id];
        ++*out_count;
    }
    if (!reason && unsupported_capability)
        reason = "unsupported SPIR-V capability";
done:
    free(binding); free(set); free(flags); free(kind); free(pointee);
    free(pointer_class); free(image_dimension); free(image_arrayed);
    free(image_cube); free(variable_id); free(variable_type);
    free(variable_class);
    return reason;
}

static void inspect_spirv_scheduling(const uint32_t *spirv, size_t word_count,
                                     uint32_t *loop_count,
                                     uint32_t *control_barrier_count)
{
    *loop_count = 0;
    *control_barrier_count = 0;
    if (!spirv || word_count < 5) return;
    for (size_t cursor = 5; cursor < word_count; ) {
        uint32_t instruction = spirv[cursor];
        uint32_t words = instruction >> 16;
        uint32_t opcode = instruction & 0xffffu;
        if (!words || cursor + words > word_count) break;
        if (opcode == 246u) ++*loop_count; /* OpLoopMerge */
        if (opcode == 224u) ++*control_barrier_count; /* OpControlBarrier */
        cursor += words;
    }
}

static PipelineEntry *find_or_create_pipeline(const void *key,
                                              const uint32_t *spirv,
                                              size_t spirv_size)
{
    for (uint32_t i = 0; i < g_compute.pipeline_count; ++i)
        if (g_compute.pipelines[i].key == key &&
            g_compute.pipelines[i].spirv_size == spirv_size)
            return &g_compute.pipelines[i];
    if (g_compute.pipeline_count >= MAX_PIPELINES) return NULL;
    PipelineEntry *grown = realloc(g_compute.pipelines,
        sizeof(*grown) * (g_compute.pipeline_count + 1));
    if (!grown) return NULL;
    g_compute.pipelines = grown;
    PipelineEntry *entry = &grown[g_compute.pipeline_count++];
    memset(entry, 0, sizeof(*entry));
    entry->key = key;
    entry->spirv_size = spirv_size;
    inspect_spirv_scheduling(spirv, spirv_size / 4, &entry->loop_count,
                             &entry->control_barrier_count);

    const char *reason = reflect_spirv(spirv, spirv_size / 4, entry->bindings,
                                       &entry->binding_count);
    if (reason) {
        entry->failed = 1;
        entry->failure = reason;
        return entry;
    }

    VkDescriptorSetLayoutBinding layout_bindings[BEER_VULKAN_COMPUTE_MAX_BINDINGS];
    memset(layout_bindings, 0, sizeof(layout_bindings));
    for (uint32_t b = 0; b < entry->binding_count; ++b) {
        layout_bindings[b].binding = entry->bindings[b].binding;
        layout_bindings[b].descriptorType = entry->bindings[b].sampler ?
            VK_DESCRIPTOR_TYPE_SAMPLER :
            entry->bindings[b].image == 2 ?
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE :
            entry->bindings[b].image ?
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE :
            entry->bindings[b].texel == 2 ?
            VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER :
            entry->bindings[b].texel ?
            VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER :
            entry->bindings[b].uniform ?
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER :
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        layout_bindings[b].descriptorCount = 1;
        layout_bindings[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = entry->binding_count,
        .pBindings = layout_bindings
    };
    VkShaderModule module = VK_NULL_HANDLE;
    if (g_compute.CreateDescriptorSetLayout(g_compute.device, &set_info, NULL,
                                            &entry->set_layout) != VK_SUCCESS) {
        entry->failure = "descriptor layout creation failed";
        goto fail;
    }
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &entry->set_layout
    };
    if (g_compute.CreatePipelineLayout(g_compute.device, &layout_info, NULL,
                                       &entry->pipeline_layout) != VK_SUCCESS) {
        entry->failure = "pipeline layout creation failed";
        goto fail;
    }
    VkShaderModuleCreateInfo module_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = spirv_size,
        .pCode = spirv
    };
    if (g_compute.CreateShaderModule(g_compute.device, &module_info, NULL,
                                     &module) != VK_SUCCESS) {
        entry->failure = "shader module rejected by driver";
        goto fail;
    }
    VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        /* Large D3D12 dispatches may be tiled into multiple Vulkan commands.
         * DispatchBase preserves SV_GroupID for every tile. */
        .flags = VK_PIPELINE_CREATE_DISPATCH_BASE_BIT,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = module,
            .pName = "main"
        },
        .layout = entry->pipeline_layout
    };
    VkResult result = g_compute.CreateComputePipelines(
        g_compute.device, VK_NULL_HANDLE, 1, &pipeline_info, NULL,
        &entry->pipeline);
    g_compute.DestroyShaderModule(g_compute.device, module, NULL);
    if (result != VK_SUCCESS) {
        entry->pipeline = VK_NULL_HANDLE;
        entry->failure = "compute pipeline creation failed";
        goto fail;
    }
    return entry;
fail:
    entry->failed = 1;
    if (entry->pipeline_layout) {
        g_compute.DestroyPipelineLayout(g_compute.device,
                                        entry->pipeline_layout, NULL);
        entry->pipeline_layout = VK_NULL_HANDLE;
    }
    if (entry->set_layout) {
        g_compute.DestroyDescriptorSetLayout(g_compute.device,
                                             entry->set_layout, NULL);
        entry->set_layout = VK_NULL_HANDLE;
    }
    return entry;
}

static int ensure_staging(size_t bytes)
{
    if (bytes <= g_compute.staging_capacity) return 1;
    if (bytes > MAX_STAGING_BYTES) return 0;
    size_t capacity = g_compute.staging_capacity ? g_compute.staging_capacity :
        (size_t)4 * 1024 * 1024;
    while (capacity < bytes) capacity *= 2;
    release_staging();

    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = capacity,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                 VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                 VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    if (g_compute.CreateBuffer(g_compute.device, &buffer_info, NULL,
                               &g_compute.staging_buffer) != VK_SUCCESS)
        goto fail;
    VkMemoryRequirements requirements;
    g_compute.GetBufferMemoryRequirements(g_compute.device,
                                          g_compute.staging_buffer,
                                          &requirements);
    VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    int found = 0;
    /* Upload-heavy staging is written far more than it is read. Prefer the
     * coherent, non-cached host type (write-combined on discrete GPUs); the
     * previous HOST_CACHED preference made 200-330 MiB frame uploads dominate
     * CPU time. Fall back to any coherent host-visible type when necessary. */
    for (int pass = 0; pass < 2 && !found; ++pass) {
        VkMemoryPropertyFlags preferred = wanted;
        for (uint32_t t = 0;
             t < g_compute.memory_properties.memoryTypeCount; ++t) {
            if ((requirements.memoryTypeBits & (1u << t)) &&
                (g_compute.memory_properties.memoryTypes[t].propertyFlags &
                 preferred) == preferred) {
                g_compute.memory_type = t;
                found = 1;
                break;
            }
        }
    }
    if (!found) goto fail;
    VkMemoryAllocateInfo allocate_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = g_compute.memory_type
    };
    if (g_compute.AllocateMemory(g_compute.device, &allocate_info, NULL,
                                 &g_compute.staging_memory) != VK_SUCCESS)
        goto fail;
    if (g_compute.BindBufferMemory(g_compute.device, g_compute.staging_buffer,
                                   g_compute.staging_memory, 0) != VK_SUCCESS)
        goto fail;
    void *mapped = NULL;
    if (g_compute.MapMemory(g_compute.device, g_compute.staging_memory, 0,
                            VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS || !mapped)
        goto fail;
    g_compute.staging_map = mapped;
    g_compute.staging_capacity = capacity;
    return 1;
fail:
    release_staging();
    return 0;
}

static size_t align_up(size_t value, size_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

static void destroy_cached_buffer(CachedBuffer *entry)
{
    if (!entry) return;
    if (entry->buffer)
        g_compute.DestroyBuffer(g_compute.device, entry->buffer, NULL);
    if (entry->memory)
        g_compute.FreeMemory(g_compute.device, entry->memory, NULL);
    if (entry->allocation_size <= g_compute.buffer_cache_bytes)
        g_compute.buffer_cache_bytes -= entry->allocation_size;
    memset(entry, 0, sizeof(*entry));
}

static CachedBuffer *find_cached_buffer(const BeerVulkanComputeBinding *source)
{
    if (!source || !source->resource || !source->serial_location ||
        source->resource_heap_type != 1 || source->writable ||
        source->uniform || source->kind != BEER_VULKAN_COMPUTE_BINDING_BUFFER)
        return NULL;
    for (uint32_t i = 0; i < BUFFER_CACHE_SIZE; ++i) {
        CachedBuffer *entry = &g_compute.buffer_cache[i];
        if (entry->content_valid && entry->resource == source->resource &&
            entry->serial == source->serial && entry->size == source->size &&
            entry->buffer) {
            entry->last_use = ++g_compute.buffer_use_serial;
            return entry;
        }
    }
    return NULL;
}

static CachedBuffer *allocate_cached_buffer(
    const BeerVulkanComputeBinding *source)
{
    if (!source || !source->resource || !source->serial_location ||
        source->resource_heap_type != 1 || source->writable ||
        source->uniform || source->kind != BEER_VULKAN_COMPUTE_BINDING_BUFFER ||
        !source->size || source->size > BUFFER_CACHE_MAX_BYTES)
        return NULL;

    CachedBuffer *candidate = NULL;
    uint64_t oldest = UINT64_MAX;
    for (uint32_t i = 0; i < BUFFER_CACHE_SIZE; ++i) {
        CachedBuffer *entry = &g_compute.buffer_cache[i];
        if (!entry->buffer) {
            candidate = entry;
            break;
        }
        if (entry->last_use < oldest) {
            oldest = entry->last_use;
            candidate = entry;
        }
    }
    if (!candidate) return NULL;
    destroy_cached_buffer(candidate);

    VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = source->size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    if (g_compute.CreateBuffer(g_compute.device, &info, NULL,
                               &candidate->buffer) != VK_SUCCESS)
        goto fail;
    VkMemoryRequirements requirements;
    g_compute.GetBufferMemoryRequirements(g_compute.device, candidate->buffer,
                                          &requirements);
    int memory_type = -1;
    for (uint32_t i = 0; i < g_compute.memory_properties.memoryTypeCount; ++i) {
        VkMemoryPropertyFlags flags =
            g_compute.memory_properties.memoryTypes[i].propertyFlags;
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memory_type = (int)i;
            break;
        }
    }
    if (memory_type < 0)
        for (uint32_t i = 0;
             i < g_compute.memory_properties.memoryTypeCount; ++i)
            if (requirements.memoryTypeBits & (1u << i)) {
                memory_type = (int)i;
                break;
            }
    if (memory_type < 0 || requirements.size > BUFFER_CACHE_MAX_BYTES)
        goto fail;
    while (g_compute.buffer_cache_bytes >
           BUFFER_CACHE_MAX_BYTES - (size_t)requirements.size) {
        CachedBuffer *victim = NULL;
        oldest = UINT64_MAX;
        for (uint32_t i = 0; i < BUFFER_CACHE_SIZE; ++i) {
            CachedBuffer *entry = &g_compute.buffer_cache[i];
            if (entry != candidate && entry->buffer && entry->last_use < oldest) {
                oldest = entry->last_use;
                victim = entry;
            }
        }
        if (!victim) goto fail;
        destroy_cached_buffer(victim);
    }
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = (uint32_t)memory_type
    };
    if (g_compute.AllocateMemory(g_compute.device, &allocation, NULL,
                                 &candidate->memory) != VK_SUCCESS ||
        g_compute.BindBufferMemory(g_compute.device, candidate->buffer,
                                   candidate->memory, 0) != VK_SUCCESS)
        goto fail;
    candidate->resource = source->resource;
    candidate->serial = source->serial;
    candidate->size = source->size;
    candidate->allocation_size = (size_t)requirements.size;
    candidate->last_use = ++g_compute.buffer_use_serial;
    candidate->content_valid = 0;
    g_compute.buffer_cache_bytes += candidate->allocation_size;
    return candidate;
fail:
    destroy_cached_buffer(candidate);
    return NULL;
}

#define MAX_IMAGE_MIPS 16u

static int image_mip_layout(const BeerVulkanComputeBinding *image,
                            uint32_t level, size_t *offset, size_t *size,
                            uint32_t *width, uint32_t *height,
                            uint32_t *depth)
{
    uint32_t levels = image->image_mip_levels ? image->image_mip_levels : 1;
    uint32_t w = image->image_width, h = image->image_height;
    uint32_t d = image->image_depth ? image->image_depth : 1;
    uint32_t layers = image->image_layers ? image->image_layers : 1;
    size_t cursor = 0;
    if (level >= levels || levels > MAX_IMAGE_MIPS) return 0;
    for (uint32_t mip = 0; mip <= level; ++mip) {
        size_t pitch, row_bytes;
        uint32_t rows;
        if (image->image_block_width && image->image_block_height &&
            image->image_block_bytes) {
            row_bytes = (size_t)((w + image->image_block_width - 1) /
                image->image_block_width) * image->image_block_bytes;
            rows = (h + image->image_block_height - 1) /
                image->image_block_height;
        } else {
            if (!image->image_bytes || w > SIZE_MAX / image->image_bytes)
                return 0;
            row_bytes = (size_t)w * image->image_bytes;
            rows = h;
        }
        /* The selected base subresource may retain host row padding. Lower
         * levels are packed consecutively by the D3D12 resource layout. */
        pitch = mip == 0 && image->image_row_pitch ?
            image->image_row_pitch : row_bytes;
        uint32_t images = image->image_dimension == 3 ? d : layers;
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

static int image_total_size(const BeerVulkanComputeBinding *image,
                            size_t *total)
{
    uint32_t levels = image->image_mip_levels ? image->image_mip_levels : 1;
    size_t offset = 0, size = 0;
    if (!levels || levels > MAX_IMAGE_MIPS ||
        !image_mip_layout(image, levels - 1, &offset, &size, NULL, NULL, NULL) ||
        offset > SIZE_MAX - size)
        return 0;
    size_t packed = offset + size;
    uint32_t layers = image->image_layers ? image->image_layers : 1;
    if (image->image_layer_stride && layers > 1) {
        size_t layer_size = packed / layers;
        if (image->image_layer_stride < layer_size ||
            layers - 1 > (SIZE_MAX - layer_size) / image->image_layer_stride)
            return 0;
        *total = (size_t)(layers - 1) * image->image_layer_stride + layer_size;
    } else {
        *total = packed;
    }
    return 1;
}

static void release_transients(Transients *t)
{
    for (uint32_t i = 0; i < t->buffer_view_count; ++i)
        g_compute.DestroyBufferView(g_compute.device, t->buffer_views[i], NULL);
    for (uint32_t i = 0; i < t->image_view_count; ++i)
        g_compute.DestroyImageView(g_compute.device, t->image_views[i], NULL);
    for (uint32_t i = 0; i < t->sampler_count; ++i)
        g_compute.DestroySampler(g_compute.device, t->samplers[i], NULL);
    for (uint32_t i = 0; i < t->image_count; ++i) {
        if (t->images[i])
            g_compute.DestroyImage(g_compute.device, t->images[i], NULL);
        if (t->image_memory[i])
            g_compute.FreeMemory(g_compute.device, t->image_memory[i], NULL);
    }
    memset(t, 0, sizeof(*t));
}

static void release_batch_transients_locked(void)
{
    for (uint32_t i = 0; i < g_compute_batch_transients.count; ++i)
        release_transients(&g_compute_batch_transients.operations[i]);
    /* Each slot is fully overwritten when retained and release_transients()
     * already cleared it; zeroing the whole 512-slot array (~15 MiB) on every
     * batch boundary was ~18% of main-thread CPU. */
    g_compute_batch_transients.count = 0;
}

static int retain_batch_transients_locked(Transients *transients)
{
    if (!g_compute_batch_depth) return 0;
    if (g_compute_batch_transients.count >= MAX_BATCH_DESCRIPTOR_SETS)
        return 0;
    g_compute_batch_transients.operations[g_compute_batch_transients.count++] =
        *transients;
    memset(transients, 0, sizeof(*transients));
    return 1;
}

static VkImageUsageFlags region_image_usage(const Region *region)
{
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        (region->sampled ? VK_IMAGE_USAGE_SAMPLED_BIT : 0) |
        (region->storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0) |
        (region->writable ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    /* D3D12 resources may change from UAV to SRV between command lists. Add
     * sampled usage whenever the storage format supports it so graphics can
     * borrow the current compute mirror instead of round-tripping through the
     * host copy. Unsupported sampled formats retain their original contract. */
    if (region->image && region->image->image_format) {
        VkFormatProperties properties;
        g_compute.GetPhysicalDeviceFormatProperties(g_compute.physical_device,
            (VkFormat)region->image->image_format, &properties);
        if (properties.optimalTilingFeatures &
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
            usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    return usage;
}

static int cached_image_matches(const CachedImage *entry,
                                const Region *region)
{
    const BeerVulkanComputeBinding *source = region->image;
    return entry->image && entry->format == source->image_format &&
        entry->width == source->image_width &&
        entry->height == source->image_height &&
        entry->depth == (source->image_depth ? source->image_depth : 1) &&
        entry->layers == (source->image_layers ? source->image_layers : 1) &&
        entry->mip_levels ==
            (source->image_mip_levels ? source->image_mip_levels : 1) &&
        entry->dimension ==
            (source->image_dimension ? source->image_dimension : 2) &&
        entry->cube == (source->image_cube ? 1u : 0u) &&
        entry->usage == region_image_usage(region);
}

static int cached_image_has_current_content(const CachedImage *entry,
                                            const Region *region)
{
    const BeerVulkanComputeBinding *source = region->image;
    return source && source->resource &&
        cached_image_matches(entry, region) && entry->content_valid &&
        !entry->needs_upload && entry->resource == source->resource &&
        entry->serial == source->serial &&
        entry->layout == VK_IMAGE_LAYOUT_GENERAL;
}

static int cached_region_has_current_content(const Region *region)
{
    if (!region || !region->image || !region->image->resource)
        return 0;
    for (uint32_t c = 0; c < IMAGE_CACHE_SIZE; ++c)
        if (cached_image_has_current_content(&g_compute.image_cache[c], region))
            return 1;
    return 0;
}

static int cached_image_copy_to_host(const CachedImage *entry,
                                     const BeerVulkanComputeBinding *binding)
{
    if (!entry || !binding || !binding->data || !entry->image ||
        !binding->size || binding->size > g_compute.staging_capacity)
        return 0;
    const int cached_readback = ensure_readback_buffer(binding->size);
    VkCommandBuffer command = g_compute.command_buffers[0];
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    if (g_compute.ResetCommandBuffer(command, 0) != VK_SUCCESS ||
        g_compute.BeginCommandBuffer(command, &begin) != VK_SUCCESS)
        return 0;
    VkImageMemoryBarrier to_copy = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = entry->image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
            binding->image_layers ? binding->image_layers : 1 }
    };
    g_compute.CmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_copy);
    VkBufferImageCopy copy = {
        .bufferOffset = 0,
        .bufferRowLength = binding->image_block_width ?
            (binding->image_row_pitch / binding->image_block_bytes) *
                binding->image_block_width :
            binding->image_row_pitch / binding->image_bytes,
        .bufferImageHeight = binding->image_block_height ?
            ((binding->image_height + binding->image_block_height - 1) /
             binding->image_block_height) * binding->image_block_height :
            binding->image_height,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0,
            binding->image_layers ? binding->image_layers : 1 },
        .imageExtent = { binding->image_width, binding->image_height,
            binding->image_depth ? binding->image_depth : 1 }
    };
    g_compute.CmdCopyImageToBuffer(command, entry->image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        cached_readback ? g_compute.readback_buffer : g_compute.staging_buffer,
        1, &copy);
    VkImageMemoryBarrier restore = to_copy;
    restore.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    restore.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    restore.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    restore.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    g_compute.CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &restore);
    if (g_compute.EndCommandBuffer(command) != VK_SUCCESS ||
        g_compute.ResetFences(g_compute.device, 1, &g_compute.fence) != VK_SUCCESS)
        return 0;
    VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &command };
    beer_vulkan_queue_lock();
    VkResult submitted = g_compute.QueueSubmit(g_compute.queue, 1, &submit,
                                                g_compute.fence);
    beer_vulkan_queue_unlock();
    if (submitted == VK_SUCCESS) ++g_compute.queue_submissions;
    if (submitted != VK_SUCCESS) {
        beer_vulkan_record_result(submitted, "compute mirror queue submission");
        return 0;
    }
    ++g_compute.host_fence_waits;
    VkResult waited = g_compute.WaitForFences(
        g_compute.device, 1, &g_compute.fence, VK_TRUE, UINT64_MAX);
    if (waited != VK_SUCCESS) {
        beer_vulkan_record_result(waited, "compute mirror fence wait");
        return 0;
    }
    memcpy(binding->data,
           cached_readback ? g_compute.readback_map : g_compute.staging_map,
           binding->size);
    return 1;
}

static void flush_pending_writebacks_locked(void)
{
    for (uint32_t i = 0; i < IMAGE_CACHE_SIZE; ++i) {
        CachedImage *entry = &g_compute.image_cache[i];
        if (!entry->host_writeback_pending) continue;
        if (cached_image_copy_to_host(entry, &entry->host_binding)) {
            if (entry->host_binding.serial_location &&
                *entry->host_binding.serial_location < entry->published_serial)
                *entry->host_binding.serial_location = entry->published_serial;
            entry->host_writeback_pending = 0;
            memset(&entry->host_binding, 0, sizeof(entry->host_binding));
        }
    }
}

static void evict_cached_image(CachedImage *entry)
{
    if (entry->image) ++g_compute.image_cache_evictions;
    if (entry->host_writeback_pending)
        (void)cached_image_copy_to_host(entry, &entry->host_binding);
    if (entry->image)
        g_compute.DestroyImage(g_compute.device, entry->image, NULL);
    if (entry->memory)
        g_compute.FreeMemory(g_compute.device, entry->memory, NULL);
    if (entry->allocation_size <= g_compute.image_cache_bytes)
        g_compute.image_cache_bytes -= entry->allocation_size;
    memset(entry, 0, sizeof(*entry));
}

/* Oldest unpinned occupied slot, or NULL. A graphics SRV can borrow a compute
 * mirror while holding the graphics lock; pinned images are never recycled. */
static CachedImage *find_lru_cached_image(void)
{
    CachedImage *oldest = NULL;
    for (uint32_t c = 0; c < IMAGE_CACHE_SIZE; ++c) {
        CachedImage *entry = &g_compute.image_cache[c];
        if (!entry->image || entry->pin_count) continue;
        if (!oldest || entry->last_use < oldest->last_use) oldest = entry;
    }
    return oldest;
}

static void recycle_transient_images(Transients *t, const Region *regions,
                                     uint32_t region_count)
{
    for (uint32_t i = 0; i < t->image_count && i < region_count; ++i) {
        if (!t->images[i]) continue;
        const BeerVulkanComputeBinding *source = regions[i].image;
        VkMemoryRequirements requirements;
        g_compute.GetImageMemoryRequirements(g_compute.device, t->images[i],
                                             &requirements);
        bool fits = requirements.size <= IMAGE_CACHE_MAX_BYTES;
        while (fits && g_compute.image_cache_bytes >
                   IMAGE_CACHE_MAX_BYTES - (size_t)requirements.size) {
            CachedImage *victim = find_lru_cached_image();
            if (!victim) { fits = false; break; }
            evict_cached_image(victim);
        }
        CachedImage *candidate = NULL;
        if (fits) {
            for (uint32_t c = 0; c < IMAGE_CACHE_SIZE; ++c) {
                if (!g_compute.image_cache[c].image) {
                    candidate = &g_compute.image_cache[c];
                    break;
                }
            }
            if (!candidate) {
                candidate = find_lru_cached_image();
                if (candidate) evict_cached_image(candidate);
            }
        }
        if (!candidate) {
            g_compute.DestroyImage(g_compute.device, t->images[i], NULL);
            g_compute.FreeMemory(g_compute.device, t->image_memory[i], NULL);
            t->images[i] = VK_NULL_HANDLE;
            t->image_memory[i] = VK_NULL_HANDLE;
            continue;
        }
        *candidate = (CachedImage){
            .format = source->image_format,
            .width = source->image_width,
            .height = source->image_height,
            .depth = source->image_depth ? source->image_depth : 1,
            .layers = source->image_layers ? source->image_layers : 1,
            .mip_levels = source->image_mip_levels ?
                source->image_mip_levels : 1,
            .dimension = source->image_dimension ? source->image_dimension : 2,
            .cube = source->image_cube ? 1u : 0u,
            .usage = region_image_usage(&regions[i]),
            .allocation_size = (size_t)requirements.size,
            .last_use = ++g_compute.image_use_serial,
            .resource = source->resource,
            .serial = source->serial,
            .layout = VK_IMAGE_LAYOUT_GENERAL,
            /* Writable results were synchronized to host storage. Keep their
             * allocations reusable, but require a fresh upload before another
             * dispatch consumes them; only read-only mirrors may skip it. */
            .content_valid = source->resource != NULL,
            /* Copying the output to host does not invalidate the Vulkan image.
             * Retain this produced serial so compute and graphics consumers can
             * reuse it until a CPU-side mutation advances the resource serial. */
            .needs_upload = 0,
            .host_writeback_pending = t->host_writeback_pending[i],
            .published_serial = t->published_serial[i],
            .host_binding = t->host_binding[i],
            .image = t->images[i],
            .memory = t->image_memory[i]
        };
        g_compute.image_cache_bytes += (size_t)requirements.size;
        t->images[i] = VK_NULL_HANDLE;
        t->image_memory[i] = VK_NULL_HANDLE;
    }
}

/* Creates or reuses a 1D/2D/3D device image with the binding's consecutive
 * mip range. Reuse removes vkCreateImage/vkAllocateMemory churn while each
 * dispatch still uploads authoritative host bytes before shader execution. */
static const char *create_region_image(const Region *region, Transients *t,
                                       uint32_t index)
{
    const BeerVulkanComputeBinding *source = region->image;
    const uint32_t dimension = source->image_dimension ?
        source->image_dimension : 2;
    const uint32_t layers = source->image_layers ? source->image_layers : 1;
    const uint32_t depth = source->image_depth ? source->image_depth : 1;
    CachedImage *shape_match = NULL;
    for (uint32_t c = 0; c < IMAGE_CACHE_SIZE; ++c) {
        CachedImage *entry = &g_compute.image_cache[c];
        if (!cached_image_matches(entry, region)) continue;
        /* Claim this resource's own mirror first, even when its serial changed:
         * changed content can be uploaded into the existing allocation. Never
         * steal another immutable resource's persistent mirror merely because
         * the Vulkan image shape matches. */
        if (source->resource && entry->resource == source->resource &&
            (!region->skip_upload ||
             cached_image_has_current_content(entry, region))) {
            t->images[index] = entry->image;
            t->image_memory[index] = entry->memory;
            t->host_writeback_pending[index] = entry->host_writeback_pending;
            t->published_serial[index] = entry->published_serial;
            t->host_binding[index] = entry->host_binding;
            if (entry->allocation_size <= g_compute.image_cache_bytes)
                g_compute.image_cache_bytes -= entry->allocation_size;
            ++g_compute.image_cache_hits;
            memset(entry, 0, sizeof(*entry));
            return NULL;
        }
        /* Never recycle another identified resource's persistent mirror. The
         * staging preflight may already have selected that exact entry for a
         * later binding in this dispatch. Stealing it here would leave the
         * later region marked skip_upload with no image to claim. Anonymous
         * allocations remain safe shape-compatible scratch storage. */
        if (!shape_match && !entry->resource) shape_match = entry;
    }
    /* Anonymous allocations may be reused by transient or writable images;
     * identified resources retain independent mirrors until invalidated. */
    if (shape_match && (!source->resource || region->writable)) {
        if (shape_match->host_writeback_pending)
            (void)cached_image_copy_to_host(shape_match,
                                            &shape_match->host_binding);
        t->images[index] = shape_match->image;
        t->image_memory[index] = shape_match->memory;
        if (shape_match->allocation_size <= g_compute.image_cache_bytes)
            g_compute.image_cache_bytes -= shape_match->allocation_size;
        memset(shape_match, 0, sizeof(*shape_match));
        return NULL;
    }
    /* A current-content hit was observed before staging layout was built. It
     * must still exist here; falling through would leave no upload bytes. */
    if (region->skip_upload) return "persistent image mirror unavailable";
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = dimension == 1 ? VK_IMAGE_TYPE_1D :
            dimension == 3 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
        .format = (VkFormat)source->image_format,
        .extent = { source->image_width, source->image_height, depth },
        .mipLevels = source->image_mip_levels ? source->image_mip_levels : 1,
        .arrayLayers = layers,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .flags = source->image_cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
        .usage = region_image_usage(region),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };
    if (g_compute.CreateImage(g_compute.device, &image_info, NULL,
                              &t->images[index]) != VK_SUCCESS) {
        t->images[index] = VK_NULL_HANDLE;
        return "image creation failed";
    }
    VkMemoryRequirements requirements;
    g_compute.GetImageMemoryRequirements(g_compute.device, t->images[index],
                                         &requirements);
    int type = -1;
    for (int pass = 0; pass < 2 && type < 0; ++pass) {
        for (uint32_t m = 0; m < g_compute.memory_properties.memoryTypeCount;
             ++m) {
            if (!(requirements.memoryTypeBits & (1u << m))) continue;
            if (pass == 0 && !(g_compute.memory_properties.memoryTypes[m]
                    .propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                continue;
            type = (int)m;
            break;
        }
    }
    if (type < 0) return "no memory type for image";
    VkMemoryAllocateInfo allocate_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = (uint32_t)type
    };
    if (g_compute.AllocateMemory(g_compute.device, &allocate_info, NULL,
                                 &t->image_memory[index]) != VK_SUCCESS) {
        t->image_memory[index] = VK_NULL_HANDLE;
        return "image memory allocation failed";
    }
    if (g_compute.BindImageMemory(g_compute.device, t->images[index],
                                  t->image_memory[index], 0) != VK_SUCCESS)
        return "image memory bind failed";
    return NULL;
}

static const char *begin_compute_recording_locked(VkCommandBuffer command)
{
    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    if (g_compute.ResetCommandBuffer(command, 0) != VK_SUCCESS)
        return "command buffer reset failed";
    if (g_compute.BeginCommandBuffer(command, &begin_info) != VK_SUCCESS)
        return "command buffer begin failed";
    return NULL;
}

static const char *end_compute_recording_locked(VkCommandBuffer command)
{
    return g_compute.EndCommandBuffer(command) == VK_SUCCESS ? NULL :
        "command buffer end failed";
}

/* Submit the recorded command buffers as one ordered batch. Storage-image
 * watchdog workloads flush one command buffer at a time before reaching this
 * helper; safe buffer-only kernels amortize driver submission overhead across
 * the full tile wave. The final fence covers every command buffer in the batch. */
static const char *finalize_compute_recordings_locked(
    uint32_t command_count, uint64_t *submit_ns, uint64_t *complete_ns)
{
    if (!command_count || command_count > g_compute.command_buffer_count)
        return "invalid compute submission count";
    if (g_compute.ResetFences(g_compute.device, 1, &g_compute.fence) !=
        VK_SUCCESS)
        return "fence reset failed";

    VkSubmitInfo submission = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = command_count,
        .pCommandBuffers = g_compute.command_buffers
    };
    if (submit_ns) *submit_ns = monotonic_nanoseconds();
    beer_vulkan_queue_lock();
    VkResult submitted = g_compute.QueueSubmit(
        g_compute.queue, 1, &submission, g_compute.fence);
    beer_vulkan_queue_unlock();
    if (submitted == VK_SUCCESS)
        g_compute.queue_submissions += command_count;
    if (submitted != VK_SUCCESS) {
        beer_vulkan_record_result(submitted, "compute queue submission");
        fprintf(stderr, "[D3D12 VULKAN COMPUTE] vkQueueSubmit result=%d\n",
                submitted);
        return "queue submission failed";
    }

    ++g_compute.host_fence_waits;
    VkResult waited = g_compute.WaitForFences(
        g_compute.device, 1, &g_compute.fence, VK_TRUE, UINT64_MAX);
    if (waited != VK_SUCCESS) {
        beer_vulkan_record_result(waited, "compute fence wait");
        fprintf(stderr, "[D3D12 VULKAN COMPUTE] vkWaitForFences result=%d\n",
                waited);
        return "fence wait failed";
    }
    g_compute.completed_submissions += command_count;
    if (complete_ns) *complete_ns = monotonic_nanoseconds();
    return NULL;
}

#define REJECT(text) do { *reason = (text); ++g_compute.rejected; \
    pthread_mutex_unlock(&g_compute_lock); return 0; } while (0)

static int dispatch_with_prepared_pipeline(
                                 const void *shader_key,
                                 const uint32_t *spirv, size_t spirv_size,
                                 const BeerVulkanComputeBinding *bindings,
                                 uint32_t binding_count,
                                 uint32_t group_count_x,
                                 uint32_t group_count_y,
                                 uint32_t group_count_z,
                                 uint32_t hazards_before,
                                 PipelineEntry *prepared_pipeline,
                                 const char **reason)
{
    static const char *unused_reason;
    if (!reason) reason = &unused_reason;
    *reason = "invalid arguments";
    if (!shader_key || !spirv || spirv_size < 20 || (spirv_size & 3u) ||
        (binding_count && !bindings) ||
        binding_count > BEER_VULKAN_COMPUTE_MAX_BINDINGS)
        return 0;

    pthread_mutex_lock(&g_compute_lock);
    uint64_t profile_start_ns = monotonic_nanoseconds();
    uint64_t profile_layout_ns = profile_start_ns;
    uint64_t profile_copy_ns = profile_start_ns;
    uint64_t profile_descriptor_ns = profile_start_ns;
    uint64_t profile_record_ns = profile_start_ns;
    uint64_t profile_submit_ns = 0;
    uint64_t profile_complete_ns = 0;
    if (!initialize_locked()) REJECT("Vulkan unavailable");
    if (beer_vulkan_device_lost()) REJECT("Vulkan device lost");
    /* Alternate/scalar batch callers may supply a dependent dispatch directly.
     * Publish deferred image output before this dispatch snapshots its host-
     * backed inputs. The D3D12 epoch planner normally closes the producer epoch
     * first, so production command replay pays this only when data is pending. */
    if (hazards_before && g_compute_batch_depth)
        flush_pending_writebacks_locked();
    if (group_count_x > g_compute.limits.maxComputeWorkGroupCount[0] ||
        group_count_y > g_compute.limits.maxComputeWorkGroupCount[1] ||
        group_count_z > g_compute.limits.maxComputeWorkGroupCount[2])
        REJECT("group count exceeds device limit");

    PipelineEntry *pipeline = prepared_pipeline ? prepared_pipeline :
        find_or_create_pipeline(shader_key, spirv, spirv_size);
    if (!pipeline) REJECT("pipeline cache exhausted");
    if (pipeline->key != shader_key || pipeline->spirv_size != spirv_size)
        REJECT("prepared pipeline does not match operation");
    if (pipeline->failed) REJECT(pipeline->failure ? pipeline->failure :
                                 "unsupported shader");

    /* Every reflected binding must be supplied with the matching kind. */
    const BeerVulkanComputeBinding *matched[BEER_VULKAN_COMPUTE_MAX_BINDINGS];
    for (uint32_t r = 0; r < pipeline->binding_count; ++r) {
        matched[r] = NULL;
        for (uint32_t b = 0; b < binding_count; ++b) {
            if (bindings[b].binding == pipeline->bindings[r].binding) {
                matched[r] = &bindings[b];
                break;
            }
        }
        if (!matched[r] || matched[r]->kind != (pipeline->bindings[r].sampler ?
                BEER_VULKAN_COMPUTE_BINDING_SAMPLER :
                pipeline->bindings[r].image ?
                BEER_VULKAN_COMPUTE_BINDING_IMAGE :
                BEER_VULKAN_COMPUTE_BINDING_BUFFER)) {
            if (getenv("BEER_D3D12_DIAGNOSTICS"))
                fprintf(stderr,
                        "[D3D12 VULKAN COMPUTE] binding %u: shader sampler=%u "
                        "image=%u; supplied %s kind=%u\n",
                        pipeline->bindings[r].binding,
                        pipeline->bindings[r].sampler,
                        pipeline->bindings[r].image,
                        matched[r] ? "present" : "missing",
                        matched[r] ? matched[r]->kind : 0u);
            REJECT("binding missing or wrong descriptor kind");
        }
        if (pipeline->bindings[r].sampler) {
            if (matched[r]->min_filter > 1 || matched[r]->mag_filter > 1 ||
                matched[r]->mipmap_mode > 1 ||
                matched[r]->reduction_mode > 2 ||
                matched[r]->compare_enable > 1 ||
                matched[r]->compare_op > 7 ||
                matched[r]->address_u > 2 || matched[r]->address_v > 2 ||
                matched[r]->address_w > 2)
                REJECT("sampler state not representable");
            if (matched[r]->anisotropy_enable > 1 ||
                (matched[r]->anisotropy_enable &&
                 (beer_vulkan_max_sampler_anisotropy() <= 1.0f ||
                  matched[r]->max_anisotropy < 1.0f)))
                REJECT("sampler anisotropy unavailable");
            if (matched[r]->reduction_mode &&
                !beer_vulkan_sampler_filter_minmax_enabled())
                REJECT("sampler min/max reduction unavailable");
            if (matched[r]->reduction_mode && matched[r]->compare_enable)
                REJECT("sampler min/max comparison invalid");
            continue;
        }
        if (!matched[r]->data || !matched[r]->size ||
            matched[r]->uniform != pipeline->bindings[r].uniform ||
            (pipeline->bindings[r].texel && !matched[r]->texel_format)) {
            if (getenv("BEER_D3D12_DIAGNOSTICS"))
                fprintf(stderr,
                        "[D3D12 VULKAN COMPUTE] binding %u: shader texel=%u "
                        "uniform=%u; supplied %s texel_format=%u size=%zu\n",
                        pipeline->bindings[r].binding,
                        pipeline->bindings[r].texel,
                        pipeline->bindings[r].uniform,
                        matched[r] ? "present" : "missing",
                        matched[r] ? matched[r]->texel_format : 0u,
                        matched[r] ? matched[r]->size : (size_t)0);
            REJECT("binding missing or wrong buffer kind");
        }
        if (pipeline->bindings[r].image) {
            const BeerVulkanComputeBinding *image = matched[r];
            VkFormatProperties format_properties;
            const int compressed = image->image_block_width &&
                image->image_block_height && image->image_block_bytes;
            size_t image_extent = 0;
            const uint32_t dimension = image->image_dimension ?
                image->image_dimension : 2;
            const uint32_t layers = image->image_layers ?
                image->image_layers : 1;
            const uint32_t depth = image->image_depth ? image->image_depth : 1;
            if (!image->image_width || !image->image_height ||
                !image->image_format || dimension < 1 || dimension > 3 ||
                (dimension == 1 && image->image_height != 1) ||
                (dimension == 3 && layers != 1) ||
                (dimension != 3 && depth != 1) ||
                image->image_width > (dimension == 1 ?
                    g_compute.limits.maxImageDimension1D :
                    dimension == 3 ? g_compute.limits.maxImageDimension3D :
                    g_compute.limits.maxImageDimension2D) ||
                image->image_height > (dimension == 3 ?
                    g_compute.limits.maxImageDimension3D :
                    g_compute.limits.maxImageDimension2D) ||
                depth > g_compute.limits.maxImageDimension3D ||
                layers > g_compute.limits.maxImageArrayLayers ||
                pipeline->bindings[r].image_dimension != dimension ||
                !!pipeline->bindings[r].image_arrayed != !!image->image_arrayed ||
                !!pipeline->bindings[r].image_cube != !!image->image_cube ||
                (image->image_cube && (layers < 6 || layers % 6)) ||
                (!image->image_arrayed && !image->image_cube && layers != 1)) {
                if (getenv("BEER_D3D12_DIAGNOSTICS"))
                    fprintf(stderr,
                        "[D3D12 VULKAN COMPUTE] image binding=%u shape "
                        "shader-dim=%u array=%u cube=%u supplied-dim=%u "
                        "array=%u cube=%u size=%ux%ux%u layers=%u "
                        "mips=%u format=%u row=%u bytes=%u storage=%zu\n",
                        pipeline->bindings[r].binding,
                        pipeline->bindings[r].image_dimension,
                        pipeline->bindings[r].image_arrayed,
                        pipeline->bindings[r].image_cube, dimension,
                        image->image_arrayed, image->image_cube,
                        image->image_width, image->image_height, depth, layers,
                        image->image_mip_levels ? image->image_mip_levels : 1,
                        image->image_format, image->image_row_pitch,
                        image->image_bytes, image->size);
                REJECT("image layout inconsistent");
            }
            if (compressed) {
                uint32_t blocks_x = (image->image_width +
                    image->image_block_width - 1) / image->image_block_width;
                uint32_t blocks_y = (image->image_height +
                    image->image_block_height - 1) / image->image_block_height;
                if (!blocks_x || !blocks_y ||
                    image->image_row_pitch <
                        (size_t)blocks_x * image->image_block_bytes ||
                    image->image_row_pitch > SIZE_MAX / blocks_y)
                    REJECT("compressed image layout inconsistent");
                image_extent = (size_t)image->image_row_pitch * blocks_y;
            } else {
                if (!image->image_bytes ||
                    image->image_row_pitch % image->image_bytes ||
                    image->image_row_pitch <
                        (size_t)image->image_width * image->image_bytes)
                    REJECT("image layout inconsistent");
                image_extent = (size_t)image->image_row_pitch *
                    (image->image_height - 1) +
                    (size_t)image->image_width * image->image_bytes;
            }
            uint32_t image_count = dimension == 3 ? depth : layers;
            if (!image_extent || image_count > SIZE_MAX / image_extent)
                REJECT("image layout inconsistent");
            image_extent *= image_count;
            uint32_t mip_levels = image->image_mip_levels ?
                image->image_mip_levels : 1;
            if (mip_levels > g_compute.limits.maxImageDimension2D ||
                !image_total_size(image, &image_extent) ||
                image->size < image_extent) {
                if (getenv("BEER_D3D12_DIAGNOSTICS"))
                    fprintf(stderr,
                        "[D3D12 VULKAN COMPUTE] image binding=%u extent "
                        "expected=%zu supplied=%zu mips=%u layers=%u "
                        "stride=%zu row=%u size=%ux%u\n",
                        pipeline->bindings[r].binding, image_extent,
                        image->size, mip_levels, layers,
                        image->image_layer_stride, image->image_row_pitch,
                        image->image_width, image->image_height);
                REJECT("image layout inconsistent");
            }
            if (image->writable && mip_levels != 1)
                REJECT("writable multi-mip images unsupported");
            if ((pipeline->bindings[r].image == 1 && image->writable) ||
                (pipeline->bindings[r].image == 2 &&
                 pipeline->bindings[r].texel_format &&
                 image->image_format != pipeline->bindings[r].texel_format))
                REJECT("image view does not match shader declaration");
            g_compute.GetPhysicalDeviceFormatProperties(
                g_compute.physical_device, (VkFormat)image->image_format,
                &format_properties);
            if (!(format_properties.optimalTilingFeatures &
                  (pipeline->bindings[r].image == 2 ?
                   VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT :
                   VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)))
                REJECT("image format unsupported by device");
        } else if (pipeline->bindings[r].texel) {
            VkFormatProperties format_properties;
            if (!matched[r]->texel_bytes ||
                (pipeline->bindings[r].texel == 1 && matched[r]->writable) ||
                (pipeline->bindings[r].texel == 2 &&
                 pipeline->bindings[r].texel_format &&
                 matched[r]->texel_format != pipeline->bindings[r].texel_format))
                REJECT("texel buffer view does not match shader declaration");
            g_compute.GetPhysicalDeviceFormatProperties(
                g_compute.physical_device, (VkFormat)matched[r]->texel_format,
                &format_properties);
            if (!(format_properties.bufferFeatures &
                  (pipeline->bindings[r].texel == 2 ?
                   VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT :
                   VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT)))
                REJECT("texel buffer format unsupported by device");
            if (matched[r]->size / matched[r]->texel_bytes == 0 ||
                matched[r]->size / matched[r]->texel_bytes >
                    g_compute.limits.maxTexelBufferElements)
                REJECT("texel buffer element count unsupported");
        } else if (matched[r]->uniform) {
            if (align_up(matched[r]->size, REGION_ALIGNMENT) >
                g_compute.limits.maxUniformBufferRange)
                REJECT("constant buffer exceeds device limit");
        } else {
            if ((matched[r]->size & ~(size_t)3) == 0 ||
                matched[r]->size > g_compute.limits.maxStorageBufferRange)
                REJECT("storage buffer size unsupported");
        }
    }

    /* Deduplicate identical ranges; reject partial overlaps. */
    Region regions[BEER_VULKAN_COMPUTE_MAX_BINDINGS];
    uint32_t region_index[BEER_VULKAN_COMPUTE_MAX_BINDINGS];
    uint32_t region_count = 0;
    size_t alignment = REGION_ALIGNMENT;
    if (g_compute.limits.minStorageBufferOffsetAlignment > alignment)
        alignment = (size_t)g_compute.limits.minStorageBufferOffsetAlignment;
    if (g_compute.limits.minUniformBufferOffsetAlignment > alignment)
        alignment = (size_t)g_compute.limits.minUniformBufferOffsetAlignment;
    if (g_compute.limits.minTexelBufferOffsetAlignment > alignment)
        alignment = (size_t)g_compute.limits.minTexelBufferOffsetAlignment;
    size_t total = 0;
    for (uint32_t r = 0; r < pipeline->binding_count; ++r) {
        BeerVulkanComputeBinding *source =
            (BeerVulkanComputeBinding *)matched[r];
        region_index[r] = UINT32_MAX;
        if (pipeline->bindings[r].sampler) continue;
        uint32_t found = UINT32_MAX;
        for (uint32_t g = 0; g < region_count; ++g) {
            uint8_t *a0 = regions[g].data, *a1 = a0 + regions[g].size;
            uint8_t *b0 = source->data, *b1 = b0 + source->size;
            if (a0 == b0 && regions[g].size == source->size) {
                found = g;
                break;
            }
            if (a0 < b1 && b0 < a1)
                REJECT("partially overlapping resource ranges");
        }
        if (found == UINT32_MAX) {
            found = region_count++;
            regions[found].data = source->data;
            regions[found].size = source->size;
            regions[found].padded_size = align_up(source->size, alignment);
            regions[found].offset = total;
            regions[found].writable = 0;
            regions[found].image = NULL;
            regions[found].sampled = 0;
            regions[found].storage = 0;
            regions[found].skip_upload = 0;
            regions[found].skip_buffer_upload = 0;
            total += regions[found].padded_size;
        }
        if (source->writable) regions[found].writable = 1;
        if (pipeline->bindings[r].image) {
            const BeerVulkanComputeBinding *first = regions[found].image;
            if (!first) {
                regions[found].image = source;
            } else if (first->image_format != source->image_format ||
                       first->image_width != source->image_width ||
                       first->image_height != source->image_height ||
                       first->image_row_pitch != source->image_row_pitch ||
                       first->image_dimension != source->image_dimension ||
                       first->image_arrayed != source->image_arrayed ||
                       first->image_cube != source->image_cube ||
                       first->image_layers != source->image_layers ||
                       first->image_depth != source->image_depth ||
                       first->image_mip_levels != source->image_mip_levels ||
                       first->image_layer_stride != source->image_layer_stride ||
                       first->image_bytes != source->image_bytes ||
                       first->image_block_width != source->image_block_width ||
                       first->image_block_height != source->image_block_height ||
                       first->image_block_bytes != source->image_block_bytes) {
                REJECT("resource bound as differently shaped images");
            }
            if (pipeline->bindings[r].image == 2) regions[found].storage = 1;
            else regions[found].sampled = 1;
        } else if (regions[found].image) {
            REJECT("resource bound as both image and buffer");
        }
        region_index[r] = found;
    }
    for (uint32_t r = 0; r < pipeline->binding_count; ++r)
        if (region_index[r] != UINT32_MAX && !pipeline->bindings[r].image &&
            regions[region_index[r]].image)
            REJECT("resource bound as both image and buffer");

    /* Decide mirror hits before assigning staging offsets. This is essential:
     * skipping only vkCmdCopyBufferToImage still paid the dominant CPU memcpy
     * cost for RE8's hundreds-of-MiB immutable image bindings. */
    total = 0;
    for (uint32_t g = 0; g < region_count; ++g) {
        regions[g].skip_upload = cached_region_has_current_content(&regions[g]);
        if (!regions[g].image && !regions[g].writable) {
            /* Staging may be skipped only when every binding of this region
             * is served by the device-local cache. */
            int all_hit = 1, any = 0;
            for (uint32_t r = 0; r < pipeline->binding_count; ++r) {
                if (region_index[r] != g) continue;
                any = 1;
                if (pipeline->bindings[r].uniform ||
                    pipeline->bindings[r].texel == 2 ||
                    !find_cached_buffer(matched[r])) {
                    all_hit = 0;
                    break;
                }
            }
            if (any && all_hit) regions[g].skip_buffer_upload = 1;
        }
        regions[g].offset = total;
        if (!regions[g].skip_upload && !regions[g].skip_buffer_upload) {
            if (total > SIZE_MAX - regions[g].padded_size)
                REJECT("staging layout overflow");
            total += regions[g].padded_size;
        }
    }
    profile_layout_ns = monotonic_nanoseconds();
    Transients transients;
    memset(&transients, 0, sizeof(transients));
    transients.image_count = region_count;
#define REJECT_VIEWS(text) do { \
    recycle_transient_images(&transients, regions, region_count); \
    release_transients(&transients); REJECT(text); } while (0)
    size_t required_staging = total ? total : alignment;
    if (g_compute_batch_depth && g_compute_batch_staging_fixed &&
        required_staging > g_compute_batch_staging_reservation)
        REJECT_VIEWS("compute batch staging reservation exceeded");
    if (!ensure_staging(required_staging))
        REJECT_VIEWS("staging allocation failed");
    VkBuffer operation_staging = g_compute.staging_buffer;
    uint8_t *operation_staging_map = g_compute.staging_map;
    for (uint32_t g = 0; g < region_count; ++g) {
        if (regions[g].skip_upload || regions[g].skip_buffer_upload) continue;
        memcpy(operation_staging_map + regions[g].offset, regions[g].data,
               regions[g].size);
        memset(operation_staging_map + regions[g].offset + regions[g].size, 0,
               regions[g].padded_size - regions[g].size);
    }
    profile_copy_ns = monotonic_nanoseconds();

    /* Scalar epoch execution fence-completes every dispatch before returning,
     * so a pipeline's immutable-layout descriptor set can be updated safely on
     * its next use. Retain one set per cached pipeline instead of resetting the
     * pool and allocating for every dispatch. This removes pure driver/object
     * churn without changing queue ordering or synchronization semantics. */
    VkDescriptorSet set = pipeline->descriptor_set;
    if (!set) {
        VkDescriptorSetAllocateInfo allocate_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = g_compute.descriptor_pool,
            .descriptorSetCount = 1,
            .pSetLayouts = &pipeline->set_layout
        };
        if (g_compute.AllocateDescriptorSets(g_compute.device, &allocate_info,
                                             &set) != VK_SUCCESS)
            REJECT("descriptor set allocation failed");
        pipeline->descriptor_set = set;
        ++g_compute.descriptor_set_allocations;
    } else {
        ++g_compute.descriptor_set_cache_hits;
    }

    VkDescriptorBufferInfo buffer_infos[BEER_VULKAN_COMPUTE_MAX_BINDINGS];
    VkDescriptorImageInfo image_infos[BEER_VULKAN_COMPUTE_MAX_BINDINGS];
    VkWriteDescriptorSet writes[BEER_VULKAN_COMPUTE_MAX_BINDINGS];
    CachedBuffer *cached_buffers[BEER_VULKAN_COMPUTE_MAX_BINDINGS] = {0};
    uint8_t cached_buffer_uploads[BEER_VULKAN_COMPUTE_MAX_BINDINGS] = {0};
    int any_image_writable = 0;
    for (uint32_t g = 0; g < region_count; ++g) {
        if (!regions[g].image) continue;
        const char *image_failure = create_region_image(&regions[g],
                                                        &transients, g);
        if (image_failure) REJECT_VIEWS(image_failure);
        if (regions[g].writable) any_image_writable = 1;
    }
    for (uint32_t r = 0; r < pipeline->binding_count; ++r) {
        int uniform = pipeline->bindings[r].uniform;
        int texel = pipeline->bindings[r].texel;
        memset(&writes[r], 0, sizeof(writes[r]));
        writes[r].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[r].dstSet = set;
        writes[r].dstBinding = pipeline->bindings[r].binding;
        writes[r].descriptorCount = 1;
        if (pipeline->bindings[r].sampler) {
            const BeerVulkanComputeBinding *source = matched[r];
            image_infos[r].sampler = find_or_create_sampler(source);
            if (!image_infos[r].sampler)
                REJECT_VIEWS("sampler cache exhausted");
            image_infos[r].imageView = VK_NULL_HANDLE;
            image_infos[r].imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            writes[r].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            writes[r].pImageInfo = &image_infos[r];
            continue;
        }
        const Region *region = &regions[region_index[r]];
        const BeerVulkanComputeBinding *source = matched[r];
        const char *buffer_cache_mode = getenv("BEER_VULKAN_BUFFER_CACHE");
        const int buffer_cache_enabled = !buffer_cache_mode ||
            strcmp(buffer_cache_mode, "0") != 0;
        if (buffer_cache_enabled && !pipeline->bindings[r].image && !uniform &&
            source && source->kind == BEER_VULKAN_COMPUTE_BINDING_BUFFER &&
            !source->writable && texel != 2) {
            cached_buffers[r] = find_cached_buffer(source);
            if (!cached_buffers[r]) {
                cached_buffers[r] = allocate_cached_buffer(source);
                if (cached_buffers[r]) cached_buffer_uploads[r] = 1;
            }
        }
        if (cached_buffers[r] && !cached_buffer_uploads[r])
            ((Region *)region)->skip_buffer_upload = 1;
        buffer_infos[r].buffer = cached_buffers[r] ?
            cached_buffers[r]->buffer : operation_staging;
        buffer_infos[r].offset = cached_buffers[r] ? 0 : region->offset;
        buffer_infos[r].range = uniform ? region->padded_size :
            (region->size & ~(size_t)3);
        if (pipeline->bindings[r].image) {
            const uint32_t dimension = matched[r]->image_dimension ?
                matched[r]->image_dimension : 2;
            const uint32_t layers = matched[r]->image_layers ?
                matched[r]->image_layers : 1;
            const int arrayed = matched[r]->image_arrayed != 0;
            const int cube = matched[r]->image_cube != 0;
            VkImageViewCreateInfo view_info = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = transients.images[region_index[r]],
                .viewType = cube ?
                    (arrayed ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY :
                               VK_IMAGE_VIEW_TYPE_CUBE) :
                    dimension == 1 ?
                    (arrayed ? VK_IMAGE_VIEW_TYPE_1D_ARRAY :
                               VK_IMAGE_VIEW_TYPE_1D) :
                    dimension == 3 ? VK_IMAGE_VIEW_TYPE_3D :
                    (arrayed ? VK_IMAGE_VIEW_TYPE_2D_ARRAY :
                               VK_IMAGE_VIEW_TYPE_2D),
                .format = (VkFormat)matched[r]->image_format,
                .subresourceRange = {
                    VK_IMAGE_ASPECT_COLOR_BIT, 0,
                    matched[r]->image_mip_levels ?
                        matched[r]->image_mip_levels : 1,
                    0, layers
                }
            };
            if (g_compute.CreateImageView(g_compute.device, &view_info, NULL,
                    &transients.image_views[transients.image_view_count]) !=
                VK_SUCCESS)
                REJECT_VIEWS("image view creation failed");
            image_infos[r].sampler = VK_NULL_HANDLE;
            image_infos[r].imageView =
                transients.image_views[transients.image_view_count++];
            image_infos[r].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            writes[r].descriptorType = pipeline->bindings[r].image == 2 ?
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE :
                VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            writes[r].pImageInfo = &image_infos[r];
        } else if (texel) {
            const BeerVulkanComputeBinding *source = matched[r];
            VkBufferViewCreateInfo view_info = {
                .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
                .buffer = cached_buffers[r] ? cached_buffers[r]->buffer :
                    operation_staging,
                .format = (VkFormat)source->texel_format,
                .offset = cached_buffers[r] ? 0 : region->offset,
                .range = (source->size / source->texel_bytes) *
                         source->texel_bytes
            };
            if (g_compute.CreateBufferView(g_compute.device, &view_info, NULL,
                    &transients.buffer_views[transients.buffer_view_count]) !=
                VK_SUCCESS)
                REJECT_VIEWS("texel buffer view creation failed");
            writes[r].pTexelBufferView =
                &transients.buffer_views[transients.buffer_view_count++];
            writes[r].descriptorType = pipeline->bindings[r].texel == 2 ?
                VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER :
                VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        } else {
            writes[r].descriptorType = uniform ?
                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER :
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[r].pBufferInfo = &buffer_infos[r];
        }
    }
    if (pipeline->binding_count)
        g_compute.UpdateDescriptorSets(g_compute.device,
                                       pipeline->binding_count, writes, 0,
                                       NULL);
    profile_descriptor_ns = monotonic_nanoseconds();

    VkCommandBuffer command = g_compute.command_buffers[0];
    const char *recording_failure = begin_compute_recording_locked(command);
    if (recording_failure) REJECT_VIEWS(recording_failure);
    g_compute.CmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                              pipeline->pipeline);
    g_compute.CmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    pipeline->pipeline_layout, 0, 1, &set, 0,
                                    NULL);
    /* The backend-neutral IR has already classified hazards against preceding
     * dispatches in this run. Today each dispatch still uses a separate Vulkan
     * submission, but queue order plus a host fence is not a Vulkan memory
     * dependency. Emit the same conservative compute dependency here that the
     * future shared command-buffer compiler will place between operations.
     *
     * RAW/WAW require prior shader writes to become visible. WAR only orders
     * prior reads before a later write; use the union because one barrier is
     * cheaper and less error-prone than descriptor-kind-specific barriers. */
    if (hazards_before) {
        VkMemoryBarrier ir_hazard = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_READ_BIT |
                             VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                             VK_ACCESS_SHADER_WRITE_BIT
        };
        g_compute.CmdPipelineBarrier(command,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
            1, &ir_hazard, 0, NULL, 0, NULL);
    }
    uint32_t cached_buffer_upload_count = 0;
    for (uint32_t r = 0; r < pipeline->binding_count; ++r) {
        if (!cached_buffer_uploads[r] || !cached_buffers[r]) continue;
        const Region *region = &regions[region_index[r]];
        VkBufferCopy copy = {
            .srcOffset = region->offset,
            .dstOffset = 0,
            .size = region->size
        };
        g_compute.CmdCopyBuffer(command, operation_staging,
                                cached_buffers[r]->buffer, 1, &copy);
        cached_buffers[r]->content_valid = 1;
        ++cached_buffer_upload_count;
    }
    if (cached_buffer_upload_count) {
        VkMemoryBarrier buffer_upload = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT
        };
        g_compute.CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &buffer_upload,
            0, NULL, 0, NULL);
    }
    /* Upload changed images from staging and move them to GENERAL. Persistent
     * read-only mirrors already remain in GENERAL and need no barrier/copy. */
    VkImageSubresourceRange color_range = {
        VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1
    };
    VkImageMemoryBarrier image_barriers[BEER_VULKAN_COMPUTE_MAX_BINDINGS];
    uint32_t barrier_count = 0;
    for (uint32_t g = 0; g < region_count; ++g) {
        if (!regions[g].image || regions[g].skip_upload) continue;
        VkImageSubresourceRange range = color_range;
        range.levelCount = regions[g].image->image_mip_levels ?
            regions[g].image->image_mip_levels : 1;
        range.layerCount = regions[g].image->image_layers ?
            regions[g].image->image_layers : 1;
        image_barriers[barrier_count++] = (VkImageMemoryBarrier){
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = transients.images[g],
            .subresourceRange = range
        };
    }
    if (barrier_count) {
        g_compute.CmdPipelineBarrier(command,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
            barrier_count, image_barriers);
        for (uint32_t g = 0; g < region_count; ++g) {
            if (!regions[g].image || regions[g].skip_upload) continue;
            const BeerVulkanComputeBinding *image = regions[g].image;
            uint32_t levels = image->image_mip_levels ?
                image->image_mip_levels : 1;
            VkBufferImageCopy copies[MAX_IMAGE_MIPS *
                                      BEER_VULKAN_COMPUTE_MAX_BINDINGS];
            uint32_t copy_count = 0;
            uint32_t layers = image->image_layers ? image->image_layers : 1;
            for (uint32_t level = 0; level < levels; ++level) {
                size_t level_offset = 0, level_size = 0;
                uint32_t width = 0, height = 0, depth = 0;
                if (!image_mip_layout(image, level, &level_offset, &level_size,
                                      &width, &height, &depth))
                    REJECT_VIEWS("image mip layout invalid");
                (void)level_size;
                uint32_t copy_layers = image->image_layer_stride ? layers : 1;
                for (uint32_t layer = 0; layer < copy_layers; ++layer) {
                    if (copy_count >= sizeof(copies) / sizeof(copies[0]))
                        REJECT_VIEWS("too many image copy regions");
                    copies[copy_count++] = (VkBufferImageCopy){
                        .bufferOffset = regions[g].offset + level_offset +
                            (size_t)layer * image->image_layer_stride,
                        .bufferRowLength = level == 0 ?
                            (image->image_block_width ?
                                (image->image_row_pitch /
                                 image->image_block_bytes) *
                                    image->image_block_width :
                                image->image_row_pitch / image->image_bytes) : 0,
                        .bufferImageHeight = level == 0 ?
                            (image->image_block_height ?
                                ((image->image_height +
                                  image->image_block_height - 1) /
                                 image->image_block_height) *
                                    image->image_block_height :
                                image->image_height) : 0,
                        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level,
                            layer, image->image_layer_stride ? 1u : layers },
                        .imageExtent = { width, height, depth }
                    };
                }
            }
            g_compute.CmdCopyBufferToImage(command, operation_staging,
                transients.images[g], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                copy_count, copies);
        }
        for (uint32_t b = 0; b < barrier_count; ++b) {
            image_barriers[b].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            image_barriers[b].dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                              VK_ACCESS_SHADER_WRITE_BIT;
            image_barriers[b].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            image_barriers[b].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        }
        g_compute.CmdPipelineBarrier(command,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL,
            barrier_count, image_barriers);
    }
    /* Queue submission order is only an execution dependency; it does not make
     * shader writes to a retained image visible to a later dispatch. Images
     * whose upload was skipped are GPU-authoritative mirrors and may have been
     * written by the preceding dispatch. Establish the D3D12 UAV/resource
     * ordering contract before they are read or written again. This is also
     * required when the two dispatches use separate command buffers and the
     * host waits for the first fence between submissions. */
    VkImageMemoryBarrier retained_barriers[BEER_VULKAN_COMPUTE_MAX_BINDINGS];
    uint32_t retained_barrier_count = 0;
    for (uint32_t g = 0; g < region_count; ++g) {
        if (!regions[g].image || !regions[g].skip_upload) continue;
        const BeerVulkanComputeBinding *image = regions[g].image;
        retained_barriers[retained_barrier_count++] = (VkImageMemoryBarrier){
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                             VK_ACCESS_SHADER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = transients.images[g],
            .subresourceRange = {
                VK_IMAGE_ASPECT_COLOR_BIT, 0,
                image->image_mip_levels ? image->image_mip_levels : 1,
                0, image->image_layers ? image->image_layers : 1
            }
        };
    }
    if (retained_barrier_count)
        g_compute.CmdPipelineBarrier(command,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL,
            retained_barrier_count, retained_barriers);
    /* Keep synchronization-heavy loops bounded without imposing the same tiny
     * scheduling quantum on every kernel. Storage-image writers retain the
     * proven 16-group completed boundary. For buffer-only kernels, live RE8
     * profiling found 512 groups to be materially faster than 4096 or one
     * untiled dispatch: the larger submissions increased NVIDIA driver setup
     * time despite sub-millisecond GPU execution. */
    enum {
        WATCHDOG_STORAGE_IMAGE_GROUPS_PER_TILE = 16,
        WATCHDOG_COMPLEX_BUFFER_GROUPS_PER_TILE = 512,
        WATCHDOG_SINGLE_BARRIER_GROUPS_PER_TILE = 512
    };
    int writes_storage_image = 0;
    int writes_3d_storage_image = 0;
    for (uint32_t reflected = 0; reflected < pipeline->binding_count;
         ++reflected)
        if (pipeline->bindings[reflected].image == 2) {
            writes_storage_image = 1;
            if (pipeline->bindings[reflected].image_dimension == 3)
                writes_3d_storage_image = 1;
        }
    /* Storage-image loops require completed scheduling boundaries to avoid the
     * NVIDIA watchdog. Buffer-only loops with barriers also benefit from
     * bounded command buffers: live profiling measured the 512-group policy at
     * roughly 250-330 ms/frame, while one large/coalesced dispatch regressed to
     * 600-900 ms/frame. Buffer tiles remain asynchronously submitted in queue
     * order with one final host fence wait. Barrier-free storage-image writers
     * retain their adaptive safety quantum. */
    const int barrier_free_loop_tiling = pipeline->loop_count != 0 &&
        pipeline->control_barrier_count == 0 && writes_storage_image;
    /* The watchdog resets observed in live captures require both a loop and a
     * translated workgroup barrier. Barrier-free image loops are finite shader
     * work and splitting them into hundreds of dispatches changes scheduling
     * without providing a proven safety benefit; it also cost tens of
     * milliseconds per frame. */
    /* All captured watchdog kernels write 3D storage images. The causal
     * distinction is the volume UAV itself; some failing shaders had loops but
     * no translated OpControlBarrier. Keep completed 16-group boundaries for
     * that proven class while ordinary buffer and 1D/2D image kernels execute
     * as one dispatch. */
    const int watchdog_tiling = pipeline->loop_count != 0 &&
        writes_3d_storage_image;
    const uint64_t total_group_count = (uint64_t)group_count_x *
        group_count_y * group_count_z;
    uint32_t max_groups_per_tile = total_group_count > UINT32_MAX ? UINT32_MAX :
        (uint32_t)total_group_count;
    uint32_t adaptive_tile_cap = 0;
    if (watchdog_tiling) {
        max_groups_per_tile = writes_storage_image ?
            WATCHDOG_STORAGE_IMAGE_GROUPS_PER_TILE :
            pipeline->control_barrier_count > 1 ?
                WATCHDOG_COMPLEX_BUFFER_GROUPS_PER_TILE :
                WATCHDOG_SINGLE_BARRIER_GROUPS_PER_TILE;
        adaptive_tile_cap = barrier_free_loop_tiling ? 1024u :
                            max_groups_per_tile;
        if (barrier_free_loop_tiling && pipeline->adaptive_tile_groups)
            max_groups_per_tile = pipeline->adaptive_tile_groups;
    }
    const char *tile_override = getenv("BEER_VULKAN_COMPUTE_TILE_GROUPS");
    if (tile_override && *tile_override) {
        char *end = NULL;
        unsigned long parsed = strtoul(tile_override, &end, 10);
        if (end && !*end && parsed && parsed <= UINT32_MAX) {
            max_groups_per_tile = (uint32_t)parsed;
            adaptive_tile_cap = 0;
        }
    }
    /* Multiple VkSubmitInfo records passed to one vkQueueSubmit preserve Vulkan
     * queue order, but NVIDIA may still schedule them as one uninterrupted
     * workload. For loop/barrier kernels writing storage images, use a real
     * fence-completed host boundary after each tile; these are the workloads
     * observed to hit the context-switch watchdog even with 16-group async
     * tiles. Buffer-only kernels retain batched asynchronous submissions. */
    /* Only 3D storage-image loops have reproduced NVIDIA Xid 109 watchdog
     * resets in live captures. Treating every storage image as that hazardous
     * forced ordinary 2D passes through hundreds of synchronous fence waves
     * (for example 540 one-row tiles), dominating frame time while their GPU
     * work remained sub-millisecond. Keep real completed boundaries for the
     * proven 3D volume class; 1D/2D image and buffer kernels retain ordered
     * asynchronous submissions plus one final wait. */
    const int synchronous_watchdog_tiles = watchdog_tiling &&
        writes_3d_storage_image;
    /* Keep each buffer-only tile as a separate submission batch. Combining all
     * tiles into one command buffer looked cheaper in isolation but regressed
     * measured frame time from ~250-330 ms to ~600-900 ms on NVIDIA. These
     * submissions remain asynchronous and share one final fence wait, unlike
     * storage-image watchdog tiles which require completed boundaries. */
    const int coalesce_buffer_tiles = 0;
    const uint32_t initial_groups_per_tile = max_groups_per_tile;
    uint32_t recorded_tile_count = 0;
    uint32_t completed_tile_waves = 0;
    uint32_t command_index = 0;
    for (uint32_t base_z = 0; base_z < group_count_z; ) {
        uint32_t tile_z = 1;
        uint32_t z_remaining = group_count_z - base_z;
        if (group_count_x == 1 && group_count_y == 1 &&
            z_remaining > max_groups_per_tile)
            tile_z = max_groups_per_tile;
        if (tile_z > z_remaining) tile_z = z_remaining;
        for (uint32_t base_y = 0; base_y < group_count_y; ) {
            uint32_t rows = max_groups_per_tile / group_count_x;
            if (!rows) rows = 1;
            uint32_t y_remaining = group_count_y - base_y;
            if (rows > y_remaining) rows = y_remaining;
            for (uint32_t base_x = 0; base_x < group_count_x; ) {
                uint32_t columns = group_count_x - base_x;
                uint32_t per_row = max_groups_per_tile / (rows * tile_z);
                if (!per_row) per_row = 1;
                if (columns > per_row) columns = per_row;
                g_compute.CmdDispatchBase(command, base_x, base_y, base_z,
                                          columns, rows, tile_z);
                ++recorded_tile_count;

                const int final_tile =
                    base_x + columns == group_count_x &&
                    base_y + rows == group_count_y &&
                    base_z + tile_z == group_count_z;
                if (!final_tile) {
                    const char *tile_failure = NULL;
                    if (!coalesce_buffer_tiles) {
                        tile_failure = end_compute_recording_locked(command);
                        if (tile_failure) REJECT_VIEWS(tile_failure);
                        ++command_index;
                    }
                    if (!coalesce_buffer_tiles &&
                        (synchronous_watchdog_tiles ||
                         command_index >= g_compute.command_buffer_count)) {
                        tile_failure = finalize_compute_recordings_locked(
                            command_index, &profile_submit_ns,
                            &profile_complete_ns);
                        if (!tile_failure) ++completed_tile_waves;
                        command_index = 0;
                        if (!tile_failure && synchronous_watchdog_tiles &&
                            adaptive_tile_cap &&
                            profile_complete_ns >= profile_submit_ns) {
                            const uint64_t tile_ns =
                                profile_complete_ns - profile_submit_ns;
                            if (tile_ns > UINT64_C(8000000) &&
                                max_groups_per_tile > 1)
                                max_groups_per_tile =
                                    (max_groups_per_tile + 1) / 2;
                            else if (tile_ns < UINT64_C(1000000) &&
                                     max_groups_per_tile < adaptive_tile_cap) {
                                max_groups_per_tile *= 2;
                                if (max_groups_per_tile > adaptive_tile_cap)
                                    max_groups_per_tile = adaptive_tile_cap;
                            }
                            pipeline->adaptive_tile_groups =
                                max_groups_per_tile;
                        }
                    }
                    if (tile_failure) {
                        fprintf(stderr,
                                "[VULKAN COMPUTE TILE FAILURE] base=%u,%u,%u "
                                "groups=%u,%u,%u quantum=%u synchronous=%d "
                                "reason=%s\n",
                                base_x, base_y, base_z, columns, rows, tile_z,
                                max_groups_per_tile, synchronous_watchdog_tiles,
                                tile_failure);
                        REJECT_VIEWS(tile_failure);
                    }
                    if (!coalesce_buffer_tiles) {
                        command = g_compute.command_buffers[command_index];
                        tile_failure = begin_compute_recording_locked(command);
                        if (tile_failure) REJECT_VIEWS(tile_failure);
                        g_compute.CmdBindPipeline(
                            command, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline->pipeline);
                        g_compute.CmdBindDescriptorSets(
                            command, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline->pipeline_layout, 0, 1, &set, 0, NULL);
                    }
                    /* These commands are tiles of one original D3D Dispatch.
                     * D3D does not provide a device-wide memory dependency
                     * between workgroups in that dispatch, so adding a Vulkan
                     * barrier between coalesced buffer-only tiles both changes
                     * semantics and costs several milliseconds per barrier on
                     * NVIDIA. Real storage-image watchdog boundaries use
                     * separate submissions and retain the conservative
                     * dependency before the next command buffer. */
                    if (!coalesce_buffer_tiles) {
                        VkMemoryBarrier tile_dependency = {
                            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                             VK_ACCESS_SHADER_WRITE_BIT
                        };
                        g_compute.CmdPipelineBarrier(
                            command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                            1, &tile_dependency, 0, NULL, 0, NULL);
                    }
                }
                base_x += columns;
            }
            base_y += rows;
        }
        base_z += tile_z;
    }
    /* Dependency epochs are the authoritative host-publication boundary.
     * Inside an outer batch, keep writable images GPU-resident and publish only
     * when end_batch closes the epoch. Scalar callers remain fully synchronous.
     * Hazard planning guarantees that a producer-dependent operation starts a
     * new epoch after the preceding epoch has been flushed. */
    const int defer_image_writeback = g_compute_batch_depth != 0;
    if (any_image_writable) {
        uint32_t readback_count = 0;
        for (uint32_t g = 0; g < region_count; ++g) {
            if (!regions[g].image || !regions[g].writable) continue;
            image_barriers[readback_count++] = (VkImageMemoryBarrier){
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = transients.images[g],
                .subresourceRange = {
                    VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                    regions[g].image->image_layers ?
                        regions[g].image->image_layers : 1
                }
            };
        }
        g_compute.CmdPipelineBarrier(command,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
            readback_count, image_barriers);
        for (uint32_t g = 0; g < region_count; ++g) {
            if (!regions[g].image || !regions[g].writable) continue;
            VkBufferImageCopy copy = {
                .bufferOffset = regions[g].offset,
                .bufferRowLength = regions[g].image->image_block_width ?
                    (regions[g].image->image_row_pitch /
                     regions[g].image->image_block_bytes) *
                        regions[g].image->image_block_width :
                    regions[g].image->image_row_pitch /
                        regions[g].image->image_bytes,
                .bufferImageHeight = regions[g].image->image_block_height ?
                    ((regions[g].image->image_height +
                      regions[g].image->image_block_height - 1) /
                     regions[g].image->image_block_height) *
                        regions[g].image->image_block_height :
                    regions[g].image->image_height,
                .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0,
                    regions[g].image->image_layers ?
                        regions[g].image->image_layers : 1 },
                .imageExtent = { regions[g].image->image_width,
                                 regions[g].image->image_height,
                    regions[g].image->image_depth ?
                        regions[g].image->image_depth : 1 }
            };
            g_compute.CmdCopyImageToBuffer(command, transients.images[g],
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                operation_staging, 1, &copy);
        }
        /* Writable mirrors are retained after synchronized readback. Restore
         * their real Vulkan layout before publishing them as GENERAL in the
         * cache; otherwise a producer->consumer dispatch can skip upload and
         * access an image that is still in TRANSFER_SRC_OPTIMAL. */
        uint32_t restore_count = 0;
        for (uint32_t g = 0; g < region_count; ++g) {
            if (!regions[g].image || !regions[g].writable) continue;
            image_barriers[restore_count++] = (VkImageMemoryBarrier){
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                 VK_ACCESS_SHADER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = transients.images[g],
                .subresourceRange = {
                    VK_IMAGE_ASPECT_COLOR_BIT, 0,
                    regions[g].image->image_mip_levels ?
                        regions[g].image->image_mip_levels : 1,
                    0, regions[g].image->image_layers ?
                        regions[g].image->image_layers : 1
                }
            };
        }
        if (restore_count)
            g_compute.CmdPipelineBarrier(command,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL,
                restore_count, image_barriers);
    }
    /* Writable buffer descriptors execute directly against the mapped staging
     * arena. Persistent read-only buffer mirrors are populated by transfer but
     * never copied back. The staging arena is write-combined, where CPU reads
     * are extremely slow, so writable buffer regions are first copied by the
     * GPU into a host-cached readback buffer at the same offsets. */
    int use_buffer_readback = 0;
    for (uint32_t g = 0; g < region_count && !use_buffer_readback; ++g)
        if (regions[g].writable && !regions[g].image && regions[g].size)
            use_buffer_readback = 1;
    if (use_buffer_readback && !ensure_readback_buffer(required_staging))
        use_buffer_readback = 0;
    if (use_buffer_readback) {
        VkMemoryBarrier to_transfer = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT |
                             VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT
        };
        g_compute.CmdPipelineBarrier(command,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &to_transfer, 0, NULL, 0,
            NULL);
        for (uint32_t g = 0; g < region_count; ++g) {
            if (!regions[g].writable || regions[g].image || !regions[g].size)
                continue;
            VkBufferCopy copy = {
                .srcOffset = regions[g].offset,
                .dstOffset = regions[g].offset,
                .size = regions[g].size
            };
            g_compute.CmdCopyBuffer(command, operation_staging,
                                    g_compute.readback_buffer, 1, &copy);
        }
    }
    VkMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT |
                         VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT
    };
    g_compute.CmdPipelineBarrier(command,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier,
                                 0, NULL, 0, NULL);
    if (getenv("BEER_VULKAN_CAPTURE_COMMAND_TRACE")) {
        fprintf(stderr,
                "[VULKAN COMPUTE COMMAND] pipeline=%p layout=%p set=%p "
                "command=%p bindings=%u regions=%u staging=%zu groups=%ux%ux%u "
                "hazards=%u uploads=%zu writable-images=%d\n",
                (void *)(uintptr_t)pipeline->pipeline,
                (void *)(uintptr_t)pipeline->pipeline_layout,
                (void *)(uintptr_t)set, (void *)(uintptr_t)command,
                pipeline->binding_count, region_count, required_staging,
                group_count_x, group_count_y, group_count_z, hazards_before,
                total, any_image_writable);
    }
    profile_record_ns = monotonic_nanoseconds();
    const char *finalize_failure = end_compute_recording_locked(command);
    if (!finalize_failure)
        finalize_failure = finalize_compute_recordings_locked(
            command_index + 1u, &profile_submit_ns, &profile_complete_ns);
    if (finalize_failure) REJECT_VIEWS(finalize_failure);
    for (uint32_t g = 0; g < region_count; ++g) {
        if (!regions[g].writable) continue;
        if (!regions[g].image || !defer_image_writeback)
            memcpy(regions[g].data,
                   (use_buffer_readback && !regions[g].image ?
                        g_compute.readback_map : operation_staging_map) +
                       regions[g].offset,
                   regions[g].size);
        if (regions[g].image && regions[g].image->serial_location) {
            ++*regions[g].image->serial_location;
            regions[g].image->serial = *regions[g].image->serial_location;
            if (defer_image_writeback) {
                transients.host_writeback_pending[g] = 1;
                transients.published_serial[g] = regions[g].image->serial;
                transients.host_binding[g] = *regions[g].image;
            }
        }
    }
    recycle_transient_images(&transients, regions, region_count);
    /* Descriptor sets may be recorded into a future shared command buffer.
     * Keep every view and sampler referenced by a successful dispatch alive
     * until the outer IR batch is finalized. Images moved into the persistent
     * cache were cleared by recycle_transient_images(); uncached images and
     * their memory follow the same batch lifetime. Scalar execution retains
     * immediate destruction. */
    if (!retain_batch_transients_locked(&transients))
        release_transients(&transients);
    ++g_compute.executed;
    uint64_t profile_end_ns = monotonic_nanoseconds();
    if (profile_end_ns >= profile_start_ns) {
        const uint64_t elapsed_ns = profile_end_ns - profile_start_ns;
        g_compute.host_execution_ns += elapsed_ns;
        beer_vulkan_perf_account(0, elapsed_ns);
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
                "[VULKAN PERF] compute total-ms=%llu.%03llu "
                "setup-ms=%llu.%03llu wait-ms=%llu.%03llu "
                "finish-ms=%llu.%03llu bindings=%u groups=%u,%u,%u bytes=%zu "
                "loops=%u barriers=%u\n",
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
                binding_count, group_count_x, group_count_y, group_count_z,
                total, pipeline->loop_count, pipeline->control_barrier_count);
        fprintf(stderr,
                "[VULKAN PERF PHASE] layout-ms=%llu.%03llu copy-ms=%llu.%03llu "
                "descriptor-ms=%llu.%03llu record-ms=%llu.%03llu "
                "tiles=%u waves=%u quantum=%u->%u sync=%d\n",
                (unsigned long long)((profile_layout_ns - profile_start_ns) /
                                     UINT64_C(1000000)),
                (unsigned long long)(((profile_layout_ns - profile_start_ns) /
                                      UINT64_C(1000)) % 1000),
                (unsigned long long)((profile_copy_ns - profile_layout_ns) /
                                     UINT64_C(1000000)),
                (unsigned long long)(((profile_copy_ns - profile_layout_ns) /
                                      UINT64_C(1000)) % 1000),
                (unsigned long long)((profile_descriptor_ns - profile_copy_ns) /
                                     UINT64_C(1000000)),
                (unsigned long long)(((profile_descriptor_ns - profile_copy_ns) /
                                      UINT64_C(1000)) % 1000),
                (unsigned long long)((profile_record_ns - profile_descriptor_ns) /
                                     UINT64_C(1000000)),
                (unsigned long long)(((profile_record_ns - profile_descriptor_ns) /
                                      UINT64_C(1000)) % 1000),
                recorded_tile_count, completed_tile_waves,
                initial_groups_per_tile, max_groups_per_tile,
                synchronous_watchdog_tiles);
        for (uint32_t g = 0; g < region_count; ++g) {
            if (regions[g].size < 256u * 1024u) continue;
            const BeerVulkanComputeBinding *first_binding = NULL;
            for (uint32_t r = 0; r < pipeline->binding_count; ++r)
                if (region_index[r] == g) { first_binding = matched[r]; break; }
            fprintf(stderr,
                    "[VULKAN PERF REGION] shader=%p size=%zu %s%s%s skip=%d/%d "
                    "serial=%llu heap=%u fmt=%ux%u texel=%u uniform=%u "
                    "resource=%p\n",
                    shader_key, regions[g].size,
                    regions[g].image ? "image" : "buffer",
                    regions[g].writable ? " writable" : "",
                    regions[g].storage ? " storage" : "",
                    regions[g].skip_upload, regions[g].skip_buffer_upload,
                    (unsigned long long)(first_binding ? first_binding->serial : 0),
                    first_binding ? first_binding->resource_heap_type : 0u,
                    regions[g].image ? regions[g].image->image_width : 0u,
                    regions[g].image ? regions[g].image->image_height : 0u,
                    first_binding ? first_binding->texel_bytes : 0u,
                    first_binding ? first_binding->uniform : 0u,
                    first_binding ? first_binding->resource : NULL);
        }
    }
    *reason = NULL;
    pthread_mutex_unlock(&g_compute_lock);
    return 1;
}

int beer_vulkan_compute_dispatch_with_hazards(
                                 const void *shader_key,
                                 const uint32_t *spirv, size_t spirv_size,
                                 const BeerVulkanComputeBinding *bindings,
                                 uint32_t binding_count,
                                 uint32_t group_count_x,
                                 uint32_t group_count_y,
                                 uint32_t group_count_z,
                                 uint32_t hazards_before,
                                 const char **reason)
{
    return dispatch_with_prepared_pipeline(
        shader_key, spirv, spirv_size, bindings, binding_count,
        group_count_x, group_count_y, group_count_z, hazards_before, NULL,
        reason);
}

int beer_vulkan_compute_dispatch(const void *shader_key,
                                 const uint32_t *spirv, size_t spirv_size,
                                 const BeerVulkanComputeBinding *bindings,
                                 uint32_t binding_count,
                                 uint32_t group_count_x,
                                 uint32_t group_count_y,
                                 uint32_t group_count_z,
                                 const char **reason)
{
    return beer_vulkan_compute_dispatch_with_hazards(
        shader_key, spirv, spirv_size, bindings, binding_count,
        group_count_x, group_count_y, group_count_z, 0, reason);
}

typedef struct PreparedEpochOperation {
    const BeerVulkanComputeOperation *operation;
    uint32_t pipeline_index;
    uint8_t ready;
} PreparedEpochOperation;

static int prepare_epoch_operations(
    const BeerVulkanComputeOperation *operations, uint32_t operation_count,
    PreparedEpochOperation *prepared)
{
    int complete = 1;

    pthread_mutex_lock(&g_compute_lock);
    if (!initialize_locked() || beer_vulkan_device_lost()) {
        const char *failure = g_compute.initialized ? "Vulkan device lost" :
            "Vulkan unavailable";
        for (uint32_t index = 0; index < operation_count; ++index) {
            prepared[index].operation = &operations[index];
            if (operations[index].result) *operations[index].result = 0;
            if (operations[index].reason) *operations[index].reason = failure;
        }
        pthread_mutex_unlock(&g_compute_lock);
        return 0;
    }

    for (uint32_t index = 0; index < operation_count; ++index) {
        const BeerVulkanComputeOperation *operation = &operations[index];
        const char *failure = NULL;
        prepared[index].operation = operation;
        if (operation->result) *operation->result = 0;
        if (operation->reason) *operation->reason = NULL;

        if (!operation->shader_key || !operation->spirv ||
            operation->spirv_size < 20 || (operation->spirv_size & 3u) ||
            (operation->binding_count && !operation->bindings) ||
            operation->binding_count > BEER_VULKAN_COMPUTE_MAX_BINDINGS) {
            failure = "invalid arguments";
        } else if (operation->group_count_x >
                       g_compute.limits.maxComputeWorkGroupCount[0] ||
                   operation->group_count_y >
                       g_compute.limits.maxComputeWorkGroupCount[1] ||
                   operation->group_count_z >
                       g_compute.limits.maxComputeWorkGroupCount[2]) {
            failure = "group count exceeds device limit";
        } else {
            PipelineEntry *pipeline = find_or_create_pipeline(
                operation->shader_key, operation->spirv,
                operation->spirv_size);
            if (!pipeline)
                failure = "pipeline cache exhausted";
            else if (pipeline->failed)
                failure = pipeline->failure ? pipeline->failure :
                    "unsupported shader";
            else
                prepared[index].pipeline_index =
                    (uint32_t)(pipeline - g_compute.pipelines);
        }

        if (failure) {
            complete = 0;
            if (operation->reason) *operation->reason = failure;
        } else {
            prepared[index].ready = 1;
        }
    }
    pthread_mutex_unlock(&g_compute_lock);
    return complete;
}

/* Minimum operations an epoch must contain before the shared recording path
 * is worth its per-epoch overhead (4x calloc, N descriptor set allocations,
 * readback buffer sizing).  Below this threshold the scalar per-dispatch path
 * is cheaper.  Empirically: 8 is the break-even on NVIDIA for RE8 splash
 * workloads; raise to 16 to give the 3D title scene's large epochs a clear
 * win without regressing the light splash path. */
#define MIN_SHAREABLE_EPOCH_OPERATIONS 8u

static int epoch_is_shareable_locked(
    const PreparedEpochOperation *prepared, uint32_t operation_count,
    int *watchdog_fallback)
{
    if (watchdog_fallback) *watchdog_fallback = 0;
    if (operation_count < MIN_SHAREABLE_EPOCH_OPERATIONS) return 0;

    for (uint32_t index = 0; index < operation_count; ++index) {
        const BeerVulkanComputeOperation *operation = prepared[index].operation;
        if (!prepared[index].ready || !operation || operation->hazards_before)
            return 0;
        const PipelineEntry *pipeline =
            &g_compute.pipelines[prepared[index].pipeline_index];
        if (!pipeline->loop_count) continue;
        for (uint32_t binding = 0; binding < pipeline->binding_count; ++binding) {
            if (pipeline->bindings[binding].image == 2 &&
                pipeline->bindings[binding].image_dimension == 3) {
                if (watchdog_fallback) *watchdog_fallback = 1;
                return 0;
            }
        }
    }
    return 1;
}

/* First shared-recording vertical slice. Binding-free epochs need no staging,
 * descriptor updates, transient views, or publication, so they can safely
 * prove the one-command-buffer lifecycle without weakening any resource or
 * watchdog contract. Resource-bearing epochs retain the scalar fallback until
 * their stable staging slices are recorded atomically with the commands. */
static int epoch_is_binding_free_locked(
    const PreparedEpochOperation *prepared, uint32_t operation_count)
{
    if (operation_count < 2) return 0;
    for (uint32_t index = 0; index < operation_count; ++index) {
        const BeerVulkanComputeOperation *operation = prepared[index].operation;
        if (!prepared[index].ready || !operation || operation->hazards_before ||
            operation->binding_count ||
            g_compute.pipelines[prepared[index].pipeline_index].binding_count)
            return 0;
    }
    return 1;
}

/* Per-operation context allocated on the stack during shared epoch recording.
 * Holds all Vulkan objects that must stay alive until the single fence wait
 * completes. Released in bulk by release_epoch_op_contexts(). */
typedef struct EpochOpContext {
    VkDescriptorSet set;
    VkBufferView buffer_views[BEER_VULKAN_COMPUTE_MAX_BINDINGS];
    uint32_t buffer_view_count;
    VkImageView image_views[BEER_VULKAN_COMPUTE_MAX_BINDINGS];
    uint32_t image_view_count;
    /* Images created for this operation — may be moved into the cache after
     * the fence completes. Indices parallel the region array. */
    VkImage images[BEER_VULKAN_COMPUTE_MAX_BINDINGS];
    VkDeviceMemory image_memory[BEER_VULKAN_COMPUTE_MAX_BINDINGS];
    uint8_t host_writeback_pending[BEER_VULKAN_COMPUTE_MAX_BINDINGS];
    uint64_t published_serial[BEER_VULKAN_COMPUTE_MAX_BINDINGS];
    BeerVulkanComputeBinding host_binding[BEER_VULKAN_COMPUTE_MAX_BINDINGS];
    uint32_t image_count;
} EpochOpContext;

/* Release all Vulkan views that were created during shared epoch preparation.
 * Images are recycled into the cache first; uncacheable ones are destroyed. */
static void release_epoch_op_contexts(EpochOpContext *contexts,
                                      uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i) {
        EpochOpContext *ctx = &contexts[i];
        /* Return the per-operation descriptor set to the pool.  The pool was
         * created with FREE_DESCRIPTOR_SET_BIT so individual frees are valid
         * without resetting the whole pool. */
        if (ctx->set)
            g_compute.FreeDescriptorSets(g_compute.device,
                                         g_compute.descriptor_pool,
                                         1, &ctx->set);
        for (uint32_t v = 0; v < ctx->buffer_view_count; ++v)
            g_compute.DestroyBufferView(g_compute.device, ctx->buffer_views[v],
                                        NULL);
        for (uint32_t v = 0; v < ctx->image_view_count; ++v)
            g_compute.DestroyImageView(g_compute.device, ctx->image_views[v],
                                       NULL);
        for (uint32_t img = 0; img < ctx->image_count; ++img) {
            if (!ctx->images[img]) continue;
            g_compute.DestroyImage(g_compute.device, ctx->images[img], NULL);
            if (ctx->image_memory[img])
                g_compute.FreeMemory(g_compute.device, ctx->image_memory[img],
                                     NULL);
        }
    }
}

/* Shared epoch recorder for resource-bearing, hazard-free epochs.
 *
 * Prerequisite: epoch_is_shareable_locked() returned 1 and the epoch has
 * bindings (i.e., it did NOT qualify as binding-free). All staging is packed
 * into a single non-overlapping arena allocated once before any command is
 * recorded. All operations are recorded into command_buffers[0] separated by
 * memory barriers. One vkQueueSubmit + vkWaitForFences covers the whole epoch.
 *
 * Rationale: the prior scalar path called dispatch_with_prepared_pipeline()
 * per operation, each doing one submit + one fence wait. For a 40-operation
 * epoch that is 40 round trips; this function does 1.
 *
 * Contract: caller holds g_compute_lock. Lock is released and re-acquired
 * around fence wait (not needed — fence wait does not need the lock).
 * Actually: lock is held throughout; fence wait is a blocking call with no
 * lock re-entry risk. */
static const char *execute_resource_bearing_epoch_locked(
    const PreparedEpochOperation *prepared, uint32_t operation_count,
    uint64_t *submit_ns, uint64_t *complete_ns)
{
    /* ------------------------------------------------------------------
     * Phase 1 — compute total staging requirement and check image cache
     * ------------------------------------------------------------------ */
    size_t alignment = REGION_ALIGNMENT;
    if (g_compute.limits.minStorageBufferOffsetAlignment > alignment)
        alignment = (size_t)g_compute.limits.minStorageBufferOffsetAlignment;
    if (g_compute.limits.minUniformBufferOffsetAlignment > alignment)
        alignment = (size_t)g_compute.limits.minUniformBufferOffsetAlignment;
    if (g_compute.limits.minTexelBufferOffsetAlignment > alignment)
        alignment = (size_t)g_compute.limits.minTexelBufferOffsetAlignment;

    /* Total staging bytes needed across the entire epoch. Each operation's
     * regions are packed consecutively so we can use a single staging buffer
     * and one command buffer with no re-upload between operations. */
    size_t total_staging = 0;

    /* Per-operation staging base offsets. Stack-allocated since operation_count
     * is bounded by MAX_BATCH_DESCRIPTOR_SETS. */
    if (operation_count > MAX_BATCH_DESCRIPTOR_SETS)
        return "epoch exceeds descriptor set limit";

    /* We need to walk the binding/region logic for every operation once to
     * determine total staging. Store minimal metadata so Phase 3 (record) can
     * use it without repeating the walk. */

    /* Allocate per-operation context on the heap — each one is ~3 KB and
     * operation_count can be up to 512. */
    EpochOpContext *contexts = calloc(operation_count, sizeof(*contexts));
    if (!contexts) return "epoch context allocation failed";

    /* Per-operation region arrays — we need these between Phase 1 and Phase 3.
     * Each operation has at most BEER_VULKAN_COMPUTE_MAX_BINDINGS regions. */
    const uint32_t max_regions = BEER_VULKAN_COMPUTE_MAX_BINDINGS;
    Region *all_regions = calloc(
        (size_t)operation_count * max_regions, sizeof(*all_regions));
    uint32_t *all_region_counts = calloc(operation_count,
                                          sizeof(*all_region_counts));
    uint32_t (*all_region_indices)[BEER_VULKAN_COMPUTE_MAX_BINDINGS] =
        calloc(operation_count,
               sizeof(*all_region_indices));
    const BeerVulkanComputeBinding *(*all_matched)[BEER_VULKAN_COMPUTE_MAX_BINDINGS] =
        calloc(operation_count, sizeof(*all_matched));

    if (!all_regions || !all_region_counts || !all_region_indices ||
        !all_matched) {
        free(contexts);
        free(all_regions);
        free(all_region_counts);
        free(all_region_indices);
        free(all_matched);
        return "epoch scratch allocation failed";
    }

    const char *failure = NULL;

    /* Phase 1: re-read pipeline pointers by index after all
     * find_or_create_pipeline calls have completed (prepare_epoch_operations
     * already ran them; the pipeline array is stable now). */
    for (uint32_t op = 0; op < operation_count; ++op) {
        if (!prepared[op].ready) {
            fprintf(stderr,
                    "[EPOCH DEBUG] op %u not ready (pipeline_index=%u)\n",
                    op, prepared[op].pipeline_index);
            failure = "operation not ready"; goto done;
        }
    }

    for (uint32_t op = 0; op < operation_count; ++op) {
        const BeerVulkanComputeOperation *operation = prepared[op].operation;
        /* Re-fetch by index — array may have been reallocated by
         * prepare_epoch_operations; indices remain valid, pointers do not. */
        const PipelineEntry *pipeline =
            &g_compute.pipelines[prepared[op].pipeline_index];
        Region *regions = &all_regions[(size_t)op * max_regions];
        uint32_t *region_index = all_region_indices[op];
        const BeerVulkanComputeBinding **matched = all_matched[op];
        uint32_t region_count = 0;
        size_t op_total = 0;

        /* Match bindings (mirrors dispatch_with_prepared_pipeline logic). */
        for (uint32_t r = 0; r < pipeline->binding_count; ++r) {
            matched[r] = NULL;
            for (uint32_t b = 0; b < operation->binding_count; ++b) {
                if (operation->bindings[b].binding ==
                    pipeline->bindings[r].binding) {
                    matched[r] = &operation->bindings[b];
                    break;
                }
            }
            if (!matched[r]) {
                failure = "binding missing"; goto done;
            }
        }

        for (uint32_t r = 0; r < pipeline->binding_count; ++r) {
            region_index[r] = UINT32_MAX;
            if (pipeline->bindings[r].sampler) continue;
            const BeerVulkanComputeBinding *source = matched[r];
            uint32_t found = UINT32_MAX;
            for (uint32_t g = 0; g < region_count; ++g) {
                uint8_t *a0 = regions[g].data;
                uint8_t *b0 = source->data;
                if (a0 == b0 && regions[g].size == source->size) {
                    found = g;
                    break;
                }
                if (a0 < b0 + source->size && b0 < a0 + regions[g].size) {
                    failure = "partially overlapping resources in epoch";
                    goto done;
                }
            }
            if (found == UINT32_MAX) {
                found = region_count++;
                regions[found].data = source->data;
                regions[found].size = source->size;
                regions[found].padded_size = align_up(source->size, alignment);
                regions[found].writable = 0;
                regions[found].image = NULL;
                regions[found].sampled = 0;
                regions[found].storage = 0;
                regions[found].skip_upload = 0;
                regions[found].skip_buffer_upload = 0;
            }
            if (source->writable) regions[found].writable = 1;
            if (pipeline->bindings[r].image) {
                if (!regions[found].image) regions[found].image = (BeerVulkanComputeBinding *)source;
                if (pipeline->bindings[r].image == 2) regions[found].storage=1;
                else regions[found].sampled = 1;
            }
            region_index[r] = found;
        }

        /* Check image cache and compute skip flags.
         * NOTE: skip_upload is intentionally NOT set for image regions in the
         * epoch path.  The image cache entry for a given resource is consumed
         * (claimed and zeroed) by create_region_image() in Phase 3.  If two
         * operations in the same epoch share an image resource, op 0 would
         * claim the cache entry and op 1 would find nothing — yet skip_upload
         * would still be 1, triggering "persistent image mirror unavailable".
         * We always re-upload images for every operation in an epoch; only
         * buffer-cache hits (stable GPU-resident allocations, never consumed)
         * are safe to skip. */
        for (uint32_t g = 0; g < region_count; ++g) {
            regions[g].skip_upload = regions[g].image
                ? 0
                : cached_region_has_current_content(&regions[g]);
            regions[g].skip_buffer_upload = 0;
            if (!regions[g].image && !regions[g].writable) {
                int all_hit = 1, any = 0;
                for (uint32_t r = 0; r < pipeline->binding_count; ++r) {
                    if (all_region_indices[op][r] != g) continue;
                    any = 1;
                    if (pipeline->bindings[r].uniform ||
                        pipeline->bindings[r].texel == 2 ||
                        !find_cached_buffer(matched[r])) {
                        all_hit = 0;
                        break;
                    }
                }
                if (any && all_hit) regions[g].skip_buffer_upload = 1;
            }
            regions[g].offset = total_staging + op_total;
            if (!regions[g].skip_upload && !regions[g].skip_buffer_upload) {
                if (op_total > SIZE_MAX - regions[g].padded_size) {
                    failure = "staging layout overflow";
                    goto done;
                }
                op_total += regions[g].padded_size;
            }
        }

        if (total_staging > SIZE_MAX - op_total) {
            failure = "total epoch staging overflow";
            goto done;
        }
        total_staging += op_total;
        all_region_counts[op] = region_count;
    }

    /* Ensure staging covers the full epoch in one allocation. */
    if (!ensure_staging(total_staging ? total_staging : alignment)) {
        failure = "epoch staging allocation failed";
        goto done;
    }

    /* Pre-allocate the readback buffer to cover the full epoch staging footprint
     * before any command recording begins.  ensure_readback_buffer() can realloc
     * the buffer; calling it after begin_compute_recording_locked() would
     * invalidate state tracked by the recording helper.  A failure here is non-
     * fatal: writable buffer readback simply falls back to staging_map reads
     * (which are slow on write-combined memory but correct). */
    if (total_staging)
        ensure_readback_buffer(total_staging);

    /* ------------------------------------------------------------------
     * Phase 2 — upload all host data into staging (CPU work, no GPU yet)
     * ------------------------------------------------------------------ */
    for (uint32_t op = 0; op < operation_count; ++op) {
        Region *regions = &all_regions[(size_t)op * max_regions];
        uint32_t region_count = all_region_counts[op];
        for (uint32_t g = 0; g < region_count; ++g) {
            if (regions[g].skip_upload || regions[g].skip_buffer_upload)
                continue;
            memcpy(g_compute.staging_map + regions[g].offset,
                   regions[g].data, regions[g].size);
            memset(g_compute.staging_map + regions[g].offset + regions[g].size,
                   0, regions[g].padded_size - regions[g].size);
        }
    }

    /* ------------------------------------------------------------------
     * Phase 3 — allocate descriptors, create image/buffer views
     * ------------------------------------------------------------------ */
    for (uint32_t op = 0; op < operation_count; ++op) {
        PipelineEntry *pipeline =
            &g_compute.pipelines[prepared[op].pipeline_index];
        Region *regions = &all_regions[(size_t)op * max_regions];
        uint32_t region_count = all_region_counts[op];
        uint32_t *region_index = all_region_indices[op];
        const BeerVulkanComputeBinding **matched = all_matched[op];
        EpochOpContext *ctx = &contexts[op];
        ctx->image_count = region_count;

        /* Descriptor set — allocate a fresh set per operation within this
         * epoch.  All operations are recorded into the same command buffer
         * before any submission, so every operation's descriptors must be
         * independently bound: reusing pipeline->descriptor_set across
         * multiple operations in the same epoch would let UpdateDescriptorSets
         * for op N silently overwrite the bindings that op M already recorded
         * into the in-flight command buffer. */
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (pipeline->set_layout) {
            VkDescriptorSetAllocateInfo ai = {
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = g_compute.descriptor_pool,
                .descriptorSetCount = 1,
                .pSetLayouts = &pipeline->set_layout
            };
            if (g_compute.AllocateDescriptorSets(g_compute.device, &ai, &set)
                != VK_SUCCESS) {
                failure = "descriptor set allocation failed";
                goto done;
            }
            ++g_compute.descriptor_set_allocations;
        }
        ctx->set = set;

        /* Create device images for image bindings. */
        for (uint32_t g = 0; g < region_count; ++g) {
            if (!regions[g].image) {
                ctx->images[g] = VK_NULL_HANDLE;
                ctx->image_memory[g] = VK_NULL_HANDLE;
                continue;
            }
            /* Temporarily build a Transients record so create_region_image
             * can use the existing cache lookup/allocation code unchanged. */
            Transients tmp;
            memset(&tmp, 0, sizeof(tmp));
            tmp.image_count = region_count;
            const char *img_failure = create_region_image(&regions[g], &tmp, g);
            ctx->images[g] = tmp.images[g];
            ctx->image_memory[g] = tmp.image_memory[g];
            ctx->host_writeback_pending[g] = tmp.host_writeback_pending[g];
            ctx->published_serial[g] = tmp.published_serial[g];
            ctx->host_binding[g] = tmp.host_binding[g];
            /* Clear the tmp slot so release_transients() won't double-free. */
            tmp.images[g] = VK_NULL_HANDLE;
            tmp.image_memory[g] = VK_NULL_HANDLE;
            if (img_failure) { failure = img_failure; goto done; }
        }

        /* Buffer/texel/image views and descriptor writes. */
        VkDescriptorBufferInfo buffer_infos[BEER_VULKAN_COMPUTE_MAX_BINDINGS];
        VkDescriptorImageInfo image_infos[BEER_VULKAN_COMPUTE_MAX_BINDINGS];
        VkWriteDescriptorSet writes[BEER_VULKAN_COMPUTE_MAX_BINDINGS];
        CachedBuffer *cached_buffers[BEER_VULKAN_COMPUTE_MAX_BINDINGS] = {0};
        uint8_t cached_buffer_uploads[BEER_VULKAN_COMPUTE_MAX_BINDINGS] = {0};

        for (uint32_t r = 0; r < pipeline->binding_count; ++r) {
            memset(&writes[r], 0, sizeof(writes[r]));
            writes[r].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[r].dstSet = set;
            writes[r].dstBinding = pipeline->bindings[r].binding;
            writes[r].descriptorCount = 1;

            if (pipeline->bindings[r].sampler) {
                VkSampler s = find_or_create_sampler(matched[r]);
                if (!s) { failure = "sampler cache exhausted"; goto done; }
                image_infos[r].sampler = s;
                image_infos[r].imageView = VK_NULL_HANDLE;
                image_infos[r].imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                writes[r].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
                writes[r].pImageInfo = &image_infos[r];
                continue;
            }

            const Region *region = &regions[region_index[r]];
            const BeerVulkanComputeBinding *source = matched[r];
            int uniform = pipeline->bindings[r].uniform;
            int texel = pipeline->bindings[r].texel;

            /* Buffer cache lookup. */
            if (!pipeline->bindings[r].image && !uniform && source &&
                source->kind == BEER_VULKAN_COMPUTE_BINDING_BUFFER &&
                !source->writable && texel != 2) {
                cached_buffers[r] = find_cached_buffer(source);
                if (!cached_buffers[r]) {
                    cached_buffers[r] = allocate_cached_buffer(source);
                    if (cached_buffers[r]) cached_buffer_uploads[r] = 1;
                }
            }

            buffer_infos[r].buffer = cached_buffers[r] ?
                cached_buffers[r]->buffer : g_compute.staging_buffer;
            buffer_infos[r].offset = cached_buffers[r] ? 0 : region->offset;
            buffer_infos[r].range = uniform ? region->padded_size :
                (region->size & ~(size_t)3);

            if (pipeline->bindings[r].image) {
                const uint32_t dimension = source->image_dimension ?
                    source->image_dimension : 2;
                const uint32_t layers = source->image_layers ?
                    source->image_layers : 1;
                const int arrayed = source->image_arrayed != 0;
                const int cube = source->image_cube != 0;
                VkImageViewCreateInfo vi = {
                    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                    .image = ctx->images[region_index[r]],
                    .viewType = cube ?
                        (arrayed ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY :
                                   VK_IMAGE_VIEW_TYPE_CUBE) :
                        dimension == 1 ?
                        (arrayed ? VK_IMAGE_VIEW_TYPE_1D_ARRAY :
                                   VK_IMAGE_VIEW_TYPE_1D) :
                        dimension == 3 ? VK_IMAGE_VIEW_TYPE_3D :
                        (arrayed ? VK_IMAGE_VIEW_TYPE_2D_ARRAY :
                                   VK_IMAGE_VIEW_TYPE_2D),
                    .format = (VkFormat)source->image_format,
                    .subresourceRange = {
                        VK_IMAGE_ASPECT_COLOR_BIT, 0,
                        source->image_mip_levels ? source->image_mip_levels : 1,
                        0, layers
                    }
                };
                if (g_compute.CreateImageView(g_compute.device, &vi, NULL,
                        &ctx->image_views[ctx->image_view_count]) != VK_SUCCESS) {
                    failure = "image view creation failed";
                    goto done;
                }
                image_infos[r].sampler = VK_NULL_HANDLE;
                image_infos[r].imageView =
                    ctx->image_views[ctx->image_view_count++];
                image_infos[r].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                writes[r].descriptorType = pipeline->bindings[r].image == 2 ?
                    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE :
                    VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                writes[r].pImageInfo = &image_infos[r];
            } else if (texel) {
                VkBufferViewCreateInfo bvi = {
                    .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
                    .buffer = cached_buffers[r] ?
                        cached_buffers[r]->buffer : g_compute.staging_buffer,
                    .format = (VkFormat)source->texel_format,
                    .offset = cached_buffers[r] ? 0 : region->offset,
                    .range = (source->size / source->texel_bytes) *
                        source->texel_bytes
                };
                if (g_compute.CreateBufferView(g_compute.device, &bvi, NULL,
                        &ctx->buffer_views[ctx->buffer_view_count]) !=
                    VK_SUCCESS) {
                    failure = "texel buffer view creation failed";
                    goto done;
                }
                writes[r].pTexelBufferView =
                    &ctx->buffer_views[ctx->buffer_view_count++];
                writes[r].descriptorType = texel == 2 ?
                    VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER :
                    VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
            } else {
                writes[r].descriptorType = uniform ?
                    VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER :
                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[r].pBufferInfo = &buffer_infos[r];
            }
        }

        if (pipeline->binding_count)
            g_compute.UpdateDescriptorSets(g_compute.device,
                                           pipeline->binding_count, writes,
                                           0, NULL);

        /* Store cached_buffer_uploads in the context for Phase 4. */
        (void)cached_buffer_uploads; /* used inline in Phase 4 below */
    }

    /* ------------------------------------------------------------------
     * Phase 4 — record one command buffer for the entire epoch
     * ------------------------------------------------------------------ */
    {
        VkCommandBuffer command = g_compute.command_buffers[0];
        failure = begin_compute_recording_locked(command);
        if (failure) goto done;

        for (uint32_t op = 0; op < operation_count; ++op) {
            const BeerVulkanComputeOperation *operation = prepared[op].operation;
            PipelineEntry *pipeline =
                &g_compute.pipelines[prepared[op].pipeline_index];
            Region *regions = &all_regions[(size_t)op * max_regions];
            uint32_t region_count = all_region_counts[op];
            uint32_t *region_index = all_region_indices[op];
            const BeerVulkanComputeBinding **matched = all_matched[op];
            EpochOpContext *ctx = &contexts[op];

            g_compute.CmdBindPipeline(command,
                                      VK_PIPELINE_BIND_POINT_COMPUTE,
                                      pipeline->pipeline);
            g_compute.CmdBindDescriptorSets(command,
                                             VK_PIPELINE_BIND_POINT_COMPUTE,
                                             pipeline->pipeline_layout, 0, 1,
                                             &ctx->set, 0, NULL);

            /* Inter-operation memory barrier (matches scalar path semantics).
             * First operation has no preceding GPU work in this command buffer,
             * so skip it unless the caller signalled a hazard (epoch_is_shareable
             * already guarantees hazards_before == 0 for all ops, but be safe). */
            if (op > 0 || operation->hazards_before) {
                VkMemoryBarrier bar = {
                    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                    .srcAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                     VK_ACCESS_SHADER_WRITE_BIT |
                                     VK_ACCESS_TRANSFER_WRITE_BIT,
                    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                     VK_ACCESS_SHADER_WRITE_BIT |
                                     VK_ACCESS_TRANSFER_READ_BIT |
                                     VK_ACCESS_TRANSFER_WRITE_BIT
                };
                g_compute.CmdPipelineBarrier(command,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                    1, &bar, 0, NULL, 0, NULL);
            }

            /* Cached buffer uploads. */
            uint32_t buf_upload_count = 0;
            for (uint32_t r = 0; r < pipeline->binding_count; ++r) {
                if (pipeline->bindings[r].sampler ||
                    pipeline->bindings[r].image || pipeline->bindings[r].uniform)
                    continue;
                /* Re-derive the cached buffer for this operation. */
                if (!matched[r] ||
                    matched[r]->kind != BEER_VULKAN_COMPUTE_BINDING_BUFFER ||
                    matched[r]->writable || pipeline->bindings[r].texel == 2)
                    continue;
                CachedBuffer *cb = find_cached_buffer(matched[r]);
                if (!cb) { cb = allocate_cached_buffer(matched[r]); }
                if (cb && !cb->content_valid) {
                    const Region *region = &regions[region_index[r]];
                    VkBufferCopy copy = {
                        .srcOffset = region->offset,
                        .dstOffset = 0,
                        .size = region->size
                    };
                    g_compute.CmdCopyBuffer(command, g_compute.staging_buffer,
                                            cb->buffer, 1, &copy);
                    cb->content_valid = 1;
                    ++buf_upload_count;
                }
            }
            if (buf_upload_count) {
                VkMemoryBarrier bar = {
                    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT
                };
                g_compute.CmdPipelineBarrier(command,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                    1, &bar, 0, NULL, 0, NULL);
            }

            /* Image upload barriers and copies. */
            VkImageMemoryBarrier img_barriers[BEER_VULKAN_COMPUTE_MAX_BINDINGS];
            uint32_t barrier_count = 0;
            for (uint32_t g = 0; g < region_count; ++g) {
                if (!regions[g].image || regions[g].skip_upload) continue;
                VkImageSubresourceRange range = {
                    VK_IMAGE_ASPECT_COLOR_BIT, 0,
                    regions[g].image->image_mip_levels ?
                        regions[g].image->image_mip_levels : 1,
                    0, regions[g].image->image_layers ?
                        regions[g].image->image_layers : 1
                };
                img_barriers[barrier_count++] = (VkImageMemoryBarrier){
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = ctx->images[g],
                    .subresourceRange = range
                };
            }
            if (barrier_count) {
                g_compute.CmdPipelineBarrier(command,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                    0, NULL, 0, NULL, barrier_count, img_barriers);

                for (uint32_t g = 0; g < region_count; ++g) {
                    if (!regions[g].image || regions[g].skip_upload) continue;
                    const BeerVulkanComputeBinding *img = regions[g].image;
                    uint32_t levels = img->image_mip_levels ?
                        img->image_mip_levels : 1;
                    uint32_t layers = img->image_layers ? img->image_layers : 1;
                    VkBufferImageCopy copies[MAX_IMAGE_MIPS];
                    uint32_t copy_count = 0;
                    for (uint32_t level = 0; level < levels &&
                         copy_count < MAX_IMAGE_MIPS; ++level) {
                        size_t level_offset = 0, level_size = 0;
                        uint32_t width = 0, height = 0, depth = 0;
                        if (!image_mip_layout(img, level, &level_offset,
                                              &level_size, &width, &height,
                                              &depth))
                            continue;
                        (void)level_size;
                        uint32_t copy_layers = img->image_layer_stride ?
                            layers : 1;
                        for (uint32_t layer = 0; layer < copy_layers &&
                             copy_count < MAX_IMAGE_MIPS; ++layer) {
                            copies[copy_count++] = (VkBufferImageCopy){
                                .bufferOffset = regions[g].offset +
                                    level_offset +
                                    (size_t)layer * img->image_layer_stride,
                                .bufferRowLength = level == 0 ?
                                    (img->image_block_width ?
                                     (img->image_row_pitch /
                                      img->image_block_bytes) *
                                     img->image_block_width :
                                     img->image_row_pitch / img->image_bytes) :
                                    0,
                                .bufferImageHeight = level == 0 ?
                                    (img->image_block_height ?
                                     ((img->image_height +
                                       img->image_block_height - 1) /
                                      img->image_block_height) *
                                     img->image_block_height :
                                     img->image_height) : 0,
                                .imageSubresource = {
                                    VK_IMAGE_ASPECT_COLOR_BIT, level,
                                    layer,
                                    img->image_layer_stride ? 1u : layers
                                },
                                .imageExtent = { width, height, depth }
                            };
                        }
                    }
                    g_compute.CmdCopyBufferToImage(command,
                        g_compute.staging_buffer, ctx->images[g],
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        copy_count, copies);
                }

                for (uint32_t b = 0; b < barrier_count; ++b) {
                    img_barriers[b].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    img_barriers[b].dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                                    VK_ACCESS_SHADER_WRITE_BIT;
                    img_barriers[b].oldLayout =
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    img_barriers[b].newLayout = VK_IMAGE_LAYOUT_GENERAL;
                }
                g_compute.CmdPipelineBarrier(command,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                    0, NULL, 0, NULL, barrier_count, img_barriers);
            }

            /* Retained image barriers (cache hits that may have been written
             * by a prior operation in this same epoch). */
            VkImageMemoryBarrier retained[BEER_VULKAN_COMPUTE_MAX_BINDINGS];
            uint32_t retained_count = 0;
            for (uint32_t g = 0; g < region_count; ++g) {
                if (!regions[g].image || !regions[g].skip_upload) continue;
                const BeerVulkanComputeBinding *img = regions[g].image;
                retained[retained_count++] = (VkImageMemoryBarrier){
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                     VK_ACCESS_SHADER_WRITE_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = ctx->images[g],
                    .subresourceRange = {
                        VK_IMAGE_ASPECT_COLOR_BIT, 0,
                        img->image_mip_levels ? img->image_mip_levels : 1,
                        0, img->image_layers ? img->image_layers : 1
                    }
                };
            }
            if (retained_count)
                g_compute.CmdPipelineBarrier(command,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                    0, NULL, 0, NULL, retained_count, retained);

            /* Writable image readback into staging (before dispatch — we read
             * back after the dispatch, recorded here in command buffer order). */
            int any_image_writable = 0;
            for (uint32_t g = 0; g < region_count; ++g)
                if (regions[g].image && regions[g].writable)
                    any_image_writable = 1;

            /* Dispatch. */
            g_compute.CmdDispatch(command, operation->group_count_x,
                                   operation->group_count_y,
                                   operation->group_count_z);

            /* Post-dispatch readback barriers for writable images. */
            if (any_image_writable) {
                uint32_t readback_count = 0;
                for (uint32_t g = 0; g < region_count; ++g) {
                    if (!regions[g].image || !regions[g].writable) continue;
                    img_barriers[readback_count++] = (VkImageMemoryBarrier){
                        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = ctx->images[g],
                        .subresourceRange = {
                            VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                            regions[g].image->image_layers ?
                                regions[g].image->image_layers : 1
                        }
                    };
                }
                g_compute.CmdPipelineBarrier(command,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                    0, NULL, 0, NULL, readback_count, img_barriers);

                for (uint32_t g = 0; g < region_count; ++g) {
                    if (!regions[g].image || !regions[g].writable) continue;
                    VkBufferImageCopy copy = {
                        .bufferOffset = regions[g].offset,
                        .bufferRowLength = regions[g].image->image_block_width ?
                            (regions[g].image->image_row_pitch /
                             regions[g].image->image_block_bytes) *
                            regions[g].image->image_block_width :
                            regions[g].image->image_row_pitch /
                            regions[g].image->image_bytes,
                        .bufferImageHeight = regions[g].image->image_block_height ?
                            ((regions[g].image->image_height +
                              regions[g].image->image_block_height - 1) /
                             regions[g].image->image_block_height) *
                            regions[g].image->image_block_height :
                            regions[g].image->image_height,
                        .imageSubresource = {
                            VK_IMAGE_ASPECT_COLOR_BIT, 0, 0,
                            regions[g].image->image_layers ?
                                regions[g].image->image_layers : 1
                        },
                        .imageExtent = {
                            regions[g].image->image_width,
                            regions[g].image->image_height,
                            regions[g].image->image_depth ?
                                regions[g].image->image_depth : 1
                        }
                    };
                    /* Write image readback into the dedicated readback buffer,
                     * NOT staging_buffer.  staging_buffer holds upload data at
                     * these same offsets and overwriting it would corrupt
                     * subsequent operations' inputs (the staging aliasing bug
                     * that previously caused device loss). */
                    g_compute.CmdCopyImageToBuffer(command, ctx->images[g],
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        g_compute.readback_buffer, 1, &copy);
                }

                /* Restore layout to GENERAL for cache reuse. */
                uint32_t restore_count = 0;
                for (uint32_t g = 0; g < region_count; ++g) {
                    if (!regions[g].image || !regions[g].writable) continue;
                    img_barriers[restore_count++] = (VkImageMemoryBarrier){
                        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                         VK_ACCESS_SHADER_WRITE_BIT,
                        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = ctx->images[g],
                        .subresourceRange = {
                            VK_IMAGE_ASPECT_COLOR_BIT, 0,
                            regions[g].image->image_mip_levels ?
                                regions[g].image->image_mip_levels : 1,
                            0, regions[g].image->image_layers ?
                                regions[g].image->image_layers : 1
                        }
                    };
                }
                if (restore_count)
                    g_compute.CmdPipelineBarrier(command,
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                        0, NULL, 0, NULL, restore_count, img_barriers);
            }

            /* Writable buffer readback into readback buffer. */
            int use_readback = 0;
            for (uint32_t g = 0; g < region_count && !use_readback; ++g)
                if (regions[g].writable && !regions[g].image && regions[g].size)
                    use_readback = 1;
            /* readback_buffer was already allocated (or attempted) before
             * recording began.  Do not call ensure_readback_buffer() here —
             * that can realloc, invalidating the mapped pointer used by Phase 5.
             * If pre-allocation failed, silently skip the readback copy; Phase 5
             * will read from staging_map instead (slow but correct). */
            if (use_readback && !g_compute.readback_buffer)
                use_readback = 0;
            if (use_readback) {
                VkMemoryBarrier to_xfer = {
                    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                    .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT |
                                     VK_ACCESS_TRANSFER_WRITE_BIT,
                    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT
                };
                g_compute.CmdPipelineBarrier(command,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                    1, &to_xfer, 0, NULL, 0, NULL);
                for (uint32_t g = 0; g < region_count; ++g) {
                    if (!regions[g].writable || regions[g].image ||
                        !regions[g].size) continue;
                    VkBufferCopy copy = {
                        .srcOffset = regions[g].offset,
                        .dstOffset = regions[g].offset,
                        .size = regions[g].size
                    };
                    g_compute.CmdCopyBuffer(command, g_compute.staging_buffer,
                                            g_compute.readback_buffer, 1, &copy);
                }
            }
        } /* end per-operation loop */

        /* Global host-read barrier covering all writes in the epoch. */
        VkMemoryBarrier host_bar = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT |
                             VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT
        };
        g_compute.CmdPipelineBarrier(command,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, 0,
            1, &host_bar, 0, NULL, 0, NULL);

        failure = end_compute_recording_locked(command);
        if (!failure)
            failure = finalize_compute_recordings_locked(1, submit_ns,
                                                          complete_ns);
        if (failure) goto done;

        /* ------------------------------------------------------------------
         * Phase 5 — writeback and image cache recycling (after fence wait)
         * ------------------------------------------------------------------ */
        for (uint32_t op = 0; op < operation_count; ++op) {
            Region *regions = &all_regions[(size_t)op * max_regions];
            uint32_t region_count = all_region_counts[op];
            EpochOpContext *ctx = &contexts[op];

            for (uint32_t g = 0; g < region_count; ++g) {
                if (!regions[g].writable) continue;
                /* Both writable buffers and writable images were copied into
                 * readback_buffer by the command buffer (images via
                 * CmdCopyImageToBuffer, buffers via CmdCopyBuffer).  Always
                 * read back from readback_map — never from staging_map, which
                 * holds upload data at the same offsets and would return stale
                 * or overwritten values. */
                const int have_rb = g_compute.readback_buffer &&
                                    g_compute.readback_map &&
                                    regions[g].size;
                if (!regions[g].image || !g_compute_batch_depth)
                    memcpy(regions[g].data,
                           (have_rb ? g_compute.readback_map :
                                      g_compute.staging_map) + regions[g].offset,
                           regions[g].size);
                if (regions[g].image && regions[g].image->serial_location) {
                    ++*regions[g].image->serial_location;
                    regions[g].image->serial =
                        *regions[g].image->serial_location;
                    if (g_compute_batch_depth) {
                        ctx->host_writeback_pending[g] = 1;
                        ctx->published_serial[g] = regions[g].image->serial;
                        ctx->host_binding[g] = *regions[g].image;
                    }
                }
            }

            /* Recycle images into the cache. */
            Transients tmp;
            memset(&tmp, 0, sizeof(tmp));
            tmp.image_count = region_count;
            for (uint32_t g = 0; g < region_count; ++g) {
                tmp.images[g] = ctx->images[g];
                tmp.image_memory[g] = ctx->image_memory[g];
                tmp.host_writeback_pending[g] = ctx->host_writeback_pending[g];
                tmp.published_serial[g] = ctx->published_serial[g];
                tmp.host_binding[g] = ctx->host_binding[g];
                ctx->images[g] = VK_NULL_HANDLE;
                ctx->image_memory[g] = VK_NULL_HANDLE;
            }
            recycle_transient_images(&tmp, regions, region_count);
            release_transients(&tmp); /* frees any uncached remainder */

            /* Mark results. */
            if (prepared[op].operation->result)
                *prepared[op].operation->result = 1;
        }
    }

done:
    release_epoch_op_contexts(contexts, operation_count);
    free(contexts);
    free(all_regions);
    free(all_region_counts);
    free(all_region_indices);
    free(all_matched);
    return failure;
}

static const char *execute_binding_free_epoch_locked(
    const PreparedEpochOperation *prepared, uint32_t operation_count,
    uint64_t *submit_ns, uint64_t *complete_ns)
{
    VkCommandBuffer command = g_compute.command_buffers[0];
    const char *failure = begin_compute_recording_locked(command);
    if (failure) return failure;

    for (uint32_t index = 0; index < operation_count; ++index) {
        const BeerVulkanComputeOperation *operation = prepared[index].operation;
        const PipelineEntry *pipeline =
            &g_compute.pipelines[prepared[index].pipeline_index];
        g_compute.CmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                  pipeline->pipeline);
        g_compute.CmdDispatch(command, operation->group_count_x,
                              operation->group_count_y,
                              operation->group_count_z);
    }

    failure = end_compute_recording_locked(command);
    if (!failure)
        failure = finalize_compute_recordings_locked(1, submit_ns, complete_ns);
    return failure;
}

static void capture_epoch_device_loss(
    const BeerVulkanComputeOperation *operation, const PipelineEntry *pipeline)
{
    static int captured;
    if (!operation || captured) return;
    captured = 1;

    const char *directory = getenv("BEER_VULKAN_DEVICE_LOSS_DIR");
    if (!directory || !*directory) directory = "/tmp";
    char path[512];
    FILE *file = NULL;

    if (operation->spirv && operation->spirv_size) {
        snprintf(path, sizeof(path), "%s/beer-device-loss-%016llx.spv",
                 directory, (unsigned long long)operation->shader_hash);
        file = fopen(path, "wb");
        if (file) {
            fwrite(operation->spirv, 1, operation->spirv_size, file);
            fclose(file);
        }
    }
    if (operation->dxbc && operation->dxbc_size) {
        snprintf(path, sizeof(path), "%s/beer-device-loss-%016llx.dxbc",
                 directory, (unsigned long long)operation->shader_hash);
        file = fopen(path, "wb");
        if (file) {
            fwrite(operation->dxbc, 1, operation->dxbc_size, file);
            fclose(file);
        }
    }
    if (operation->root_signature && operation->root_signature_size) {
        snprintf(path, sizeof(path), "%s/beer-device-loss-%016llx.rootsig",
                 directory, (unsigned long long)operation->shader_hash);
        file = fopen(path, "wb");
        if (file) {
            fwrite(operation->root_signature, 1,
                   operation->root_signature_size, file);
            fclose(file);
        }
    }

    fprintf(stderr,
            "[D3D12 DEVICE LOSS] submission=%llu list=%u sequence=%llu "
            "shader=%016llx groups=%ux%ux%u descriptors=%u "
            "pipeline-bindings=%u pipeline=%p layout=%p set-layout=%p\n",
            (unsigned long long)operation->debug_submission,
            operation->debug_list_index,
            (unsigned long long)operation->debug_sequence,
            (unsigned long long)operation->shader_hash,
            operation->group_count_x, operation->group_count_y,
            operation->group_count_z, operation->binding_count,
            pipeline ? pipeline->binding_count : 0,
            pipeline ? (void *)(uintptr_t)pipeline->pipeline : NULL,
            pipeline ? (void *)(uintptr_t)pipeline->pipeline_layout : NULL,
            pipeline ? (void *)(uintptr_t)pipeline->set_layout : NULL);
    if (pipeline) {
        for (uint32_t index = 0; index < pipeline->binding_count; ++index) {
            const ReflectedBinding *reflected = &pipeline->bindings[index];
            fprintf(stderr,
                    "[D3D12 DEVICE LOSS REFLECTION] index=%u b=%u uniform=%u "
                    "texel=%u texel-format=%u image=%u dimension=%u "
                    "arrayed=%u cube=%u sampler=%u\n",
                    index, reflected->binding, reflected->uniform,
                    reflected->texel, reflected->texel_format,
                    reflected->image, reflected->image_dimension,
                    reflected->image_arrayed, reflected->image_cube,
                    reflected->sampler);
        }
    }
    for (uint32_t index = 0; index < operation->binding_count; ++index) {
        const BeerVulkanComputeBinding *binding = &operation->bindings[index];
        uint64_t relative_offset = binding->source_buffer_location >=
                binding->source_resource_gpu_base ?
            binding->source_buffer_location - binding->source_resource_gpu_base :
            UINT64_MAX;
        fprintf(stderr,
                "[D3D12 DEVICE LOSS BINDING] b=%u kind=%u writable=%u "
                "resource=%p bytes=%zu format=%u image=%ux%ux%u layers=%u "
                "mips=%u row=%u serial=%llu root=%u descriptor=%u "
                "type=%u reg=%u space=%u table=%016llx handle=%016llx "
                "gpuva=%016llx resource-base=%016llx relative=%016llx "
                "resource-bytes=%zu heap-type=%u descriptor-bytes=%u\n",
                binding->binding, binding->kind, binding->writable,
                binding->resource, binding->size, binding->image_format,
                binding->image_width, binding->image_height,
                binding->image_depth, binding->image_layers,
                binding->image_mip_levels, binding->image_row_pitch,
                (unsigned long long)(binding->serial_location ?
                    *binding->serial_location : binding->serial),
                binding->source_root_parameter,
                binding->source_descriptor_index,
                binding->source_descriptor_type, binding->source_register,
                binding->source_register_space,
                (unsigned long long)binding->source_descriptor_table,
                (unsigned long long)binding->source_descriptor_handle,
                (unsigned long long)binding->source_buffer_location,
                (unsigned long long)binding->source_resource_gpu_base,
                (unsigned long long)relative_offset,
                binding->source_resource_total_size,
                binding->source_resource_heap_type,
                binding->source_descriptor_size);
        if (binding->source_descriptor_size) {
            fprintf(stderr, "[D3D12 DEVICE LOSS DESCRIPTOR] b=%u raw=",
                    binding->binding);
            for (uint32_t byte = 0; byte < binding->source_descriptor_size;
                 ++byte)
                fprintf(stderr, "%02x", binding->source_descriptor_bytes[byte]);
            fputc('\n', stderr);
        }
        snprintf(path, sizeof(path),
                 "%s/beer-device-loss-%016llx-binding-%u.meta",
                 directory, (unsigned long long)operation->shader_hash,
                 binding->binding);
        file = fopen(path, "w");
        if (file) {
            fprintf(file,
                    "binding=%u kind=%u writable=%u uniform=%u resource=%p "
                    "data=%p bytes=%zu serial=%llu format=%u texel-format=%u "
                    "texel-bytes=%u image=%ux%ux%u layers=%u mips=%u row=%u "
                    "dimension=%u arrayed=%u cube=%u root=%u descriptor=%u "
                    "type=%u reg=%u space=%u table=%016llx handle=%016llx "
                    "gpuva=%016llx resource-base=%016llx relative=%016llx "
                    "resource-bytes=%zu heap-type=%u raw=",
                    binding->binding, binding->kind, binding->writable,
                    binding->uniform, binding->resource, binding->data,
                    binding->size,
                    (unsigned long long)(binding->serial_location ?
                        *binding->serial_location : binding->serial),
                    binding->image_format, binding->texel_format,
                    binding->texel_bytes, binding->image_width,
                    binding->image_height, binding->image_depth,
                    binding->image_layers, binding->image_mip_levels,
                    binding->image_row_pitch, binding->image_dimension,
                    binding->image_arrayed, binding->image_cube,
                    binding->source_root_parameter,
                    binding->source_descriptor_index,
                    binding->source_descriptor_type,
                    binding->source_register,
                    binding->source_register_space,
                    (unsigned long long)binding->source_descriptor_table,
                    (unsigned long long)binding->source_descriptor_handle,
                    (unsigned long long)binding->source_buffer_location,
                    (unsigned long long)binding->source_resource_gpu_base,
                    (unsigned long long)relative_offset,
                    binding->source_resource_total_size,
                    binding->source_resource_heap_type);
            for (uint32_t byte = 0; byte < binding->source_descriptor_size;
                 ++byte)
                fprintf(file, "%02x", binding->source_descriptor_bytes[byte]);
            fputc('\n', file);
            fclose(file);
        }
        if (binding->data && binding->size &&
            binding->size <= 64u * 1024u) {
            snprintf(path, sizeof(path),
                     "%s/beer-device-loss-%016llx-binding-%u.bin",
                     directory, (unsigned long long)operation->shader_hash,
                     binding->binding);
            file = fopen(path, "wb");
            if (file) {
                fwrite(binding->data, 1, binding->size, file);
                fclose(file);
            }
        }
    }
}

int beer_vulkan_compute_execute_epoch(
    const BeerVulkanComputeOperation *operations, uint32_t operation_count)
{
    if (operation_count && !operations) return 0;
    if (!operation_count) return 1;

    pthread_mutex_lock(&g_compute_lock);
    ++g_compute.epoch_calls;
    g_compute.epoch_operations += operation_count;
    if (operation_count > 1) ++g_compute.multi_operation_epochs;
    if (operation_count > g_compute.max_epoch_operations)
        g_compute.max_epoch_operations = operation_count;
    pthread_mutex_unlock(&g_compute_lock);

    PreparedEpochOperation inline_prepared[64];
    PreparedEpochOperation *prepared = operation_count <= 64 ?
        inline_prepared : calloc(operation_count, sizeof(*prepared));
    if (operation_count <= 64)
        memset(prepared, 0, operation_count * sizeof(*prepared));
    if (!prepared) {
        for (uint32_t index = 0; index < operation_count; ++index) {
            if (operations[index].result) *operations[index].result = 0;
            if (operations[index].reason)
                *operations[index].reason = "epoch preparation allocation failed";
        }
        return 0;
    }

    int complete = prepare_epoch_operations(operations, operation_count,
                                             prepared);
    int watchdog_fallback = 0;
    int shareable = 0;
    int binding_free = 0;
    pthread_mutex_lock(&g_compute_lock);
    shareable = epoch_is_shareable_locked(prepared, operation_count,
                                           &watchdog_fallback);
    if (shareable) {
        ++g_compute.shareable_epoch_calls;
        g_compute.shareable_epoch_operations += operation_count;
        binding_free = epoch_is_binding_free_locked(prepared, operation_count);
    } else if (watchdog_fallback) {
        ++g_compute.watchdog_epoch_fallbacks;
    }
    pthread_mutex_unlock(&g_compute_lock);

    if (shareable) {
        uint64_t submit_ns = 0, complete_ns = 0;
        pthread_mutex_lock(&g_compute_lock);
        const char *failure = binding_free
            ? execute_binding_free_epoch_locked(
                prepared, operation_count, &submit_ns, &complete_ns)
            : execute_resource_bearing_epoch_locked(
                prepared, operation_count, &submit_ns, &complete_ns);
        if (!failure) {
            g_compute.executed += operation_count;
            ++g_compute.shared_epoch_calls;
            g_compute.shared_epoch_operations += operation_count;
        }
        pthread_mutex_unlock(&g_compute_lock);

        /* If the fast path failed, fall through to the scalar loop below.
         * Do not mark results or return here on failure. */
        if (!failure) {
            for (uint32_t index = 0; index < operation_count; ++index) {
                if (operations[index].result && !*operations[index].result)
                    *operations[index].result = 1;
            }
            if (prepared != inline_prepared) free(prepared);
            return complete;
        }
        /* Fast path failed or was disabled — fall through to scalar loop. */
    }

    for (uint32_t index = 0; index < operation_count; ++index) {
        const BeerVulkanComputeOperation *operation =
            prepared[index].operation;
        if (!prepared[index].ready) continue;
        static const char *unused_reason;
        const char **reason = operation->reason ? operation->reason :
            &unused_reason;
        /* The D3D12 planner normally starts a new epoch at every hazard, but
         * this renderer API is public to backend tests and alternate callers.
         * Preserve its contract when a dependent operation is supplied in the
         * same call: publish pending GPU outputs before that operation resolves
         * live host-backed inputs. */
        if (operation->hazards_before && g_compute_batch_depth) {
            pthread_mutex_lock(&g_compute_lock);
            flush_pending_writebacks_locked();
            pthread_mutex_unlock(&g_compute_lock);
        }
        PipelineEntry *pipeline =
            &g_compute.pipelines[prepared[index].pipeline_index];
        uint8_t executed = (uint8_t)dispatch_with_prepared_pipeline(
            operation->shader_key, operation->spirv, operation->spirv_size,
            operation->bindings, operation->binding_count,
            operation->group_count_x, operation->group_count_y,
            operation->group_count_z, operation->hazards_before,
            pipeline, reason);
        if (operation->result) *operation->result = executed;
        if (!executed) complete = 0;
        if (beer_vulkan_device_lost()) {
            capture_epoch_device_loss(operation, pipeline);
            for (++index; index < operation_count; ++index) {
                if (operations[index].result) *operations[index].result = 0;
                if (operations[index].reason)
                    *operations[index].reason = "Vulkan device lost";
            }
            if (prepared != inline_prepared) free(prepared);
            return 0;
        }
    }
    if (prepared != inline_prepared) free(prepared);
    return complete;
}

int beer_vulkan_compute_begin_batch_with_staging(size_t staging_bytes)
{
    int ready = 0;
    pthread_mutex_lock(&g_compute_lock);
    if (!g_compute_batch_depth) {
        g_compute_batch_descriptor_sets = 0;
        g_compute_batch_descriptor_pool_ready = 0;
        g_compute_batch_staging_reservation = 0;
        g_compute_batch_staging_fixed = 1;
        release_batch_transients_locked();
        if (staging_bytes <= MAX_STAGING_BYTES && initialize_locked() &&
            ensure_staging(staging_bytes ? staging_bytes : REGION_ALIGNMENT)) {
            /* Descriptor sets are retained by pipeline and owned by the
             * context pool. The current epoch executor fence-completes each
             * operation before a retained set can be updated again. */
            g_compute_batch_descriptor_pool_ready = 1;
            g_compute_batch_staging_reservation =
                staging_bytes ? staging_bytes : REGION_ALIGNMENT;
            ++g_compute_batch_depth;
            ready = 1;
        }
    } else {
        ++g_compute_batch_depth;
        ready = 1;
    }
    pthread_mutex_unlock(&g_compute_lock);
    return ready;
}

void beer_vulkan_compute_begin_batch(void)
{
    if (beer_vulkan_compute_begin_batch_with_staging(MAX_STAGING_BYTES)) {
        pthread_mutex_lock(&g_compute_lock);
        g_compute_batch_staging_fixed = 0;
        pthread_mutex_unlock(&g_compute_lock);
    }
}

void beer_vulkan_compute_end_batch(void)
{
    pthread_mutex_lock(&g_compute_lock);
    if (g_compute_batch_depth && --g_compute_batch_depth == 0 &&
        g_compute.initialized) {
        /* Completed storage-image writes remain authoritative in their Vulkan
         * mirrors. Eagerly copying every result into a sparse DEFAULT-heap CPU
         * shadow faults gigabytes of otherwise untouched host pages during a
         * large frame. A later compute epoch and graphics can consume the exact
         * mirror/serial directly; explicit host consumers call the resource
         * synchronizer (or the conservative forget path) before reading.
         * Keep a diagnostic rollback for visual/performance A/B validation. */
        const char *eager_publication =
            getenv("BEER_D3D12_EAGER_COMPUTE_PUBLICATION");
        if (eager_publication && *eager_publication &&
            strcmp(eager_publication, "0"))
            flush_pending_writebacks_locked();
        if (getenv("BEER_D3D12_DIAGNOSTICS")) {
            uint32_t pending = 0;
            for (uint32_t i = 0; i < IMAGE_CACHE_SIZE; ++i)
                pending += g_compute.image_cache[i].host_writeback_pending != 0;
            fprintf(stderr,
                    "[VULKAN COMPUTE] end_batch: retained %u GPU-authoritative resources\n",
                    pending);
        }
        /* All submissions in the conservative adapter have completed by this
         * point, so descriptor-referenced views and samplers may now be
         * destroyed. Keeping this ownership at the batch boundary prepares the
         * same objects for a later single-submit command buffer safely. */
        release_batch_transients_locked();
        g_compute_batch_descriptor_sets = 0;
        g_compute_batch_descriptor_pool_ready = 0;
        g_compute_batch_staging_reservation = 0;
        g_compute_batch_staging_fixed = 0;
    }
    pthread_mutex_unlock(&g_compute_lock);
}

static CachedImage *find_exportable_image_mirror_locked(
    const void *resource, uint64_t serial, uint32_t format, uint32_t width,
    uint32_t height, uint32_t depth, uint32_t layers, uint32_t dimension,
    uint32_t mip_levels)
{
    if (!resource || !g_compute.initialized || beer_vulkan_device_lost())
        return NULL;
    if (!depth) depth = 1;
    if (!layers) layers = 1;
    if (!dimension) dimension = 2;
    if (!mip_levels) mip_levels = 1;
    for (uint32_t i = 0; i < IMAGE_CACHE_SIZE; ++i) {
        CachedImage *entry = &g_compute.image_cache[i];
        if (!entry->image || !entry->content_valid || entry->needs_upload ||
            entry->resource != resource || entry->serial != serial ||
            entry->format != format ||
            entry->width != width || entry->height != height ||
            entry->depth != depth || entry->layers != layers ||
            entry->dimension != dimension || entry->mip_levels != mip_levels ||
            entry->layout != VK_IMAGE_LAYOUT_GENERAL ||
            !(entry->usage & VK_IMAGE_USAGE_SAMPLED_BIT))
            continue;
        return entry;
    }
    return NULL;
}

int beer_vulkan_compute_has_image_mirror(
    const void *resource, uint64_t serial, uint32_t format, uint32_t width,
    uint32_t height, uint32_t depth, uint32_t layers, uint32_t dimension,
    uint32_t mip_levels)
{
    pthread_mutex_lock(&g_compute_lock);
    int found = find_exportable_image_mirror_locked(resource, serial, format,
        width, height, depth, layers, dimension, mip_levels) != NULL;
    pthread_mutex_unlock(&g_compute_lock);
    return found;
}

int beer_vulkan_compute_acquire_image_mirror(
    const void *resource, uint64_t serial, uint32_t format, uint32_t width,
    uint32_t height, uint32_t depth, uint32_t layers, uint32_t dimension,
    uint32_t mip_levels, BeerVulkanComputeImageMirror *mirror)
{
    if (!resource || !mirror) return 0;
    memset(mirror, 0, sizeof(*mirror));
    pthread_mutex_lock(&g_compute_lock);
    CachedImage *entry = find_exportable_image_mirror_locked(resource, serial,
        format, width, height, depth, layers, dimension, mip_levels);
    if (entry) {
        ++entry->pin_count;
        entry->last_use = ++g_compute.image_use_serial;
        mirror->image = (uint64_t)(uintptr_t)entry->image;
        mirror->format = entry->format;
        mirror->width = entry->width;
        mirror->height = entry->height;
        mirror->depth = entry->depth;
        mirror->layers = entry->layers;
        mirror->dimension = entry->dimension;
        mirror->mip_levels = entry->mip_levels;
        mirror->layout = (uint32_t)entry->layout;
        mirror->token = entry;
    }
    pthread_mutex_unlock(&g_compute_lock);
    return entry != NULL;
}

void beer_vulkan_compute_release_image_mirror(
    BeerVulkanComputeImageMirror *mirror)
{
    if (!mirror || !mirror->token) return;
    pthread_mutex_lock(&g_compute_lock);
    CachedImage *entry = (CachedImage *)mirror->token;
    if (entry >= g_compute.image_cache &&
        entry < g_compute.image_cache + IMAGE_CACHE_SIZE && entry->pin_count)
        --entry->pin_count;
    pthread_mutex_unlock(&g_compute_lock);
    memset(mirror, 0, sizeof(*mirror));
}

int beer_vulkan_compute_sync_resource(const void *resource, uint64_t serial)
{
    if (!resource) return 0;
    int synchronized = 0;
    pthread_mutex_lock(&g_compute_lock);
    if (g_compute.initialized && !beer_vulkan_device_lost()) {
        for (uint32_t i = 0; i < IMAGE_CACHE_SIZE; ++i) {
            CachedImage *entry = &g_compute.image_cache[i];
            if (!entry->image || !entry->host_writeback_pending ||
                entry->resource != resource || entry->serial != serial)
                continue;
            if (cached_image_copy_to_host(entry, &entry->host_binding)) {
                if (entry->host_binding.serial_location &&
                    *entry->host_binding.serial_location <
                        entry->published_serial)
                    *entry->host_binding.serial_location =
                        entry->published_serial;
                entry->host_writeback_pending = 0;
                memset(&entry->host_binding, 0, sizeof(entry->host_binding));
                entry->last_use = ++g_compute.image_use_serial;
                synchronized = 1;
            }
            break;
        }
    }
    pthread_mutex_unlock(&g_compute_lock);
    return synchronized;
}

void beer_vulkan_compute_forget_resource(const void *resource)
{
    if (!resource) return;
    pthread_mutex_lock(&g_compute_lock);
    if (g_compute.initialized) {
        for (uint32_t i = 0; i < IMAGE_CACHE_SIZE; ++i) {
            CachedImage *entry = &g_compute.image_cache[i];
            if (entry->resource != resource) continue;
            /* Resource teardown is serialized by the frontend. A nonzero pin
             * here indicates an internal lifetime bug; do not free an image
             * still referenced by a graphics descriptor. */
            if (entry->pin_count) continue;
            if (entry->host_writeback_pending)
                (void)cached_image_copy_to_host(entry, &entry->host_binding);
            if (entry->image)
                g_compute.DestroyImage(g_compute.device, entry->image, NULL);
            if (entry->memory)
                g_compute.FreeMemory(g_compute.device, entry->memory, NULL);
            if (entry->allocation_size <= g_compute.image_cache_bytes)
                g_compute.image_cache_bytes -= entry->allocation_size;
            memset(entry, 0, sizeof(*entry));
        }
    }
    pthread_mutex_unlock(&g_compute_lock);
}

void beer_vulkan_compute_get_stats(BeerVulkanComputeStats *stats)
{
    if (!stats) return;
    pthread_mutex_lock(&g_compute_lock);
    stats->queue_submissions = g_compute.queue_submissions;
    stats->completed_submissions = g_compute.completed_submissions;
    stats->host_fence_waits = g_compute.host_fence_waits;
    stats->host_execution_ns = g_compute.host_execution_ns;
    stats->sampler_cache_hits = g_compute.sampler_cache_hits;
    stats->sampler_creations = g_compute.sampler_creations;
    stats->descriptor_set_cache_hits = g_compute.descriptor_set_cache_hits;
    stats->descriptor_set_allocations = g_compute.descriptor_set_allocations;
    stats->image_cache_hits = g_compute.image_cache_hits;
    stats->image_cache_evictions = g_compute.image_cache_evictions;
    stats->epoch_calls = g_compute.epoch_calls;
    stats->epoch_operations = g_compute.epoch_operations;
    stats->multi_operation_epochs = g_compute.multi_operation_epochs;
    stats->shareable_epoch_calls = g_compute.shareable_epoch_calls;
    stats->shareable_epoch_operations = g_compute.shareable_epoch_operations;
    stats->shared_epoch_calls = g_compute.shared_epoch_calls;
    stats->shared_epoch_operations = g_compute.shared_epoch_operations;
    stats->watchdog_epoch_fallbacks = g_compute.watchdog_epoch_fallbacks;
    stats->max_epoch_operations = g_compute.max_epoch_operations;
    pthread_mutex_unlock(&g_compute_lock);
}

void beer_vulkan_compute_destroy(void)
{
    pthread_mutex_lock(&g_compute_lock);
    g_compute_batch_depth = 0;
    g_compute_batch_descriptor_sets = 0;
    g_compute_batch_descriptor_pool_ready = 0;
    g_compute_batch_staging_reservation = 0;
    g_compute_batch_staging_fixed = 0;
    if (g_compute.initialized) {
        flush_pending_writebacks_locked();
        release_batch_transients_locked();
    } else {
        memset(&g_compute_batch_transients, 0,
               sizeof(g_compute_batch_transients));
    }
    destroy_locked();
    pthread_mutex_unlock(&g_compute_lock);
}
