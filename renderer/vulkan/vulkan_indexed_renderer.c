#define _GNU_SOURCE
#include "vulkan_indexed_renderer.h"
#include "vulkan_context.h"
#include "target_mirror.h"

#include <dlfcn.h>
#include <float.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#define BUFFER_COUNT 6
#define SUBMISSION_RING_SIZE 3
#define TEXTURE_MIRROR_COUNT 8
#define TARGET_MIRROR_COUNT 4

typedef struct {
    const void *resource;
    size_t bytes;
    uint64_t serial;
    uint64_t last_use;
    uint32_t width;
    uint32_t height;
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint8_t *map;
    VkDeviceSize capacity;
} TextureMirror;

typedef struct {
    const void *resource;
    uint8_t *cpu_pixels;
    size_t bytes;
    uint64_t serial;
    uint64_t last_use;
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint8_t *map;
    VkDeviceSize capacity;
    int gpu_dirty;
    /* Zero until this slot's device memory actually holds content produced
     * for `serial`. Serial zero is a legitimate value for a freshly created
     * D3D11 target, so identity alone cannot distinguish "never uploaded"
     * from "uploaded at serial 0" on a recycled slot. */
    int content_valid;
} TargetMirror;

static BeerTargetMirrorState target_mirror_state(const TargetMirror *mirror)
{
    BeerTargetMirrorState state = {
        .resource = mirror->resource,
        .serial = mirror->serial,
        .bytes = mirror->bytes,
        .content_valid = mirror->content_valid
    };
    return state;
}

static void writeback_target_mirror(TargetMirror *mirror);
static int wait_for_pending_submission(void);

typedef struct {
    uint32_t width, height, vertex_offset, index_offset;
    uint32_t index_count;
    int32_t base_vertex;
    uint32_t mode, stride;
    float viewport[4];
    int32_t scissor[4];
    uint32_t blend_enable, source_blend, destination_blend, color_operation;
    uint32_t source_alpha, destination_alpha, alpha_operation, write_mask;
    float blend_factor[4];
    uint32_t texture_width, texture_height, sampler_flags;
    uint32_t dispatch_origin_x, dispatch_origin_y;
    uint32_t dispatch_width, dispatch_pixels;
    float constant_color[4];
} PushConstants;

_Static_assert(sizeof(PushConstants) == 156, "indexed Vulkan push constants must match GLSL");

typedef struct {
    VkCommandBuffer command_buffer;
    VkDescriptorSet descriptor_set;
    VkFence fence;
    VkBuffer buffers[BUFFER_COUNT];
    VkDeviceMemory memories[BUFFER_COUNT];
    uint8_t *maps[BUFFER_COUNT];
    VkDeviceSize capacities[BUFFER_COUNT];
    int pending;
} SubmissionSlot;

typedef struct {
    void *library;
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkCommandPool command_pool;
    VkDescriptorSetLayout descriptor_layout;
    VkDescriptorPool descriptor_pool;
    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;
    SubmissionSlot submissions[SUBMISSION_RING_SIZE];
    uint32_t next_submission;
    TextureMirror texture_mirrors[TEXTURE_MIRROR_COUNT];
    TargetMirror target_mirrors[TARGET_MIRROR_COUNT];
    uint64_t texture_use_serial;
    uint64_t texture_uploads;
    uint64_t texture_cache_hits;
    uint64_t target_use_serial;
    uint64_t target_uploads;
    uint64_t target_syncs;
    uint64_t frame_draw_signature;
    uint64_t frame_resource_signature;
    uint64_t frame_mode_draw_signatures[17];
    uint64_t frame_mode_resource_signatures[17];
    uint32_t frame_draw_count;
    uint32_t frame_mode_counts[17];
    int initialized;
    int unavailable;

    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
    PFN_vkDestroyInstance DestroyInstance;
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkCreateDevice CreateDevice;
    PFN_vkDestroyDevice DestroyDevice;
    PFN_vkGetDeviceQueue GetDeviceQueue;
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
    PFN_vkAllocateDescriptorSets AllocateDescriptorSets;
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
    PFN_vkCmdPushConstants CmdPushConstants;
    PFN_vkCmdDispatch CmdDispatch;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkCmdFillBuffer CmdFillBuffer;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkCreateFence CreateFence;
    PFN_vkDestroyFence DestroyFence;
    PFN_vkResetFences ResetFences;
    PFN_vkWaitForFences WaitForFences;
    PFN_vkDeviceWaitIdle DeviceWaitIdle;
} IndexedRenderer;

static IndexedRenderer g_indexed;
static pthread_mutex_t g_indexed_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t trace_mix(uint64_t hash, uint64_t value)
{
    hash ^= value + UINT64_C(0x9e3779b97f4a7c15) + (hash << 6) + (hash >> 2);
    return hash;
}

static uint64_t trace_bytes(uint64_t hash, const uint8_t *data, size_t size)
{
    if (!data || !size) return trace_mix(hash, size);
    /* Draw-local constants and dynamic buffers can be large. Sample evenly
     * across them so hover/state changes are visible without restoring the
     * expensive full-frame diagnostics. */
    size_t samples = size < 64 ? size : 64;
    for (size_t index = 0; index < samples; ++index) {
        size_t offset = samples == 1 ? 0 : index * (size - 1) / (samples - 1);
        hash = trace_mix(hash, data[offset]);
    }
    return trace_mix(hash, size);
}

#define LOAD_INSTANCE(name) do { \
    g_indexed.name = (PFN_vk##name)g_indexed.GetInstanceProcAddr(g_indexed.instance, "vk" #name); \
    if (!g_indexed.name) goto fail; \
} while (0)
#define LOAD_DEVICE(name) do { \
    g_indexed.name = (PFN_vk##name)g_indexed.GetDeviceProcAddr(g_indexed.device, "vk" #name); \
    if (!g_indexed.name) goto fail; \
} while (0)

static int read_shader(uint32_t **code, size_t *size)
{
    const char *path = getenv("BEER_VULKAN_INDEXED_SPV");
    char relative[4096];
    if (!path || !*path) {
        ssize_t length = readlink("/proc/self/exe", relative, sizeof(relative) - 1);
        if (length <= 0) return 0;
        relative[length] = '\0';
        char *separator = strrchr(relative, '/');
        if (!separator) return 0;
        separator[1] = '\0';
        strncat(relative, "renderer/vulkan/shaders/indexed_scaleform.comp.spv",
                sizeof(relative) - strlen(relative) - 1);
        path = relative;
    }
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "[D3D11 VULKAN] cannot open indexed SPIR-V %s\n", path);
        return 0;
    }
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return 0; }
    long length = ftell(file);
    if (length <= 0 || (length & 3) || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file); return 0;
    }
    *code = malloc((size_t)length);
    if (!*code) { fclose(file); return 0; }
    *size = (size_t)length;
    int ok = fread(*code, 1, *size, file) == *size;
    fclose(file);
    if (!ok) { free(*code); *code = NULL; }
    return ok;
}

static int find_memory_type(uint32_t bits, uint32_t *index)
{
    VkPhysicalDeviceMemoryProperties properties;
    g_indexed.GetPhysicalDeviceMemoryProperties(g_indexed.physical_device, &properties);
    VkMemoryPropertyFlags required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) &&
            (properties.memoryTypes[i].propertyFlags & required) == required) {
            *index = i;
            return 1;
        }
    }
    return 0;
}

static int wait_for_submission(SubmissionSlot *slot)
{
    if (!slot || !slot->pending) return 1;
    if (!g_indexed.device || !slot->fence ||
        g_indexed.WaitForFences(g_indexed.device, 1, &slot->fence,
                                VK_TRUE, UINT64_MAX) != VK_SUCCESS)
        return 0;
    slot->pending = 0;
    return 1;
}

static int wait_for_pending_submission(void)
{
    for (uint32_t i = 0; i < SUBMISSION_RING_SIZE; ++i)
        if (!wait_for_submission(&g_indexed.submissions[i])) return 0;
    return 1;
}

static void destroy_submission_buffers(SubmissionSlot *slot)
{
    if (!slot || !g_indexed.device) return;
    if (!wait_for_submission(slot)) return;
    for (uint32_t i = 0; i < BUFFER_COUNT; ++i) {
        if (slot->maps[i] && g_indexed.UnmapMemory)
            g_indexed.UnmapMemory(g_indexed.device, slot->memories[i]);
        if (slot->buffers[i] && g_indexed.DestroyBuffer)
            g_indexed.DestroyBuffer(g_indexed.device, slot->buffers[i], NULL);
        if (slot->memories[i] && g_indexed.FreeMemory)
            g_indexed.FreeMemory(g_indexed.device, slot->memories[i], NULL);
        slot->maps[i] = NULL;
        slot->buffers[i] = VK_NULL_HANDLE;
        slot->memories[i] = VK_NULL_HANDLE;
        slot->capacities[i] = 0;
    }
}

static void destroy_transient_buffers(void)
{
    if (!g_indexed.device) return;
    for (uint32_t i = 0; i < SUBMISSION_RING_SIZE; ++i)
        destroy_submission_buffers(&g_indexed.submissions[i]);
}

static void destroy_buffers(void)
{
    if (!g_indexed.device) return;
    destroy_transient_buffers();
    for (uint32_t i = 0; i < TEXTURE_MIRROR_COUNT; ++i) {
        TextureMirror *mirror = &g_indexed.texture_mirrors[i];
        if (mirror->map && g_indexed.UnmapMemory)
            g_indexed.UnmapMemory(g_indexed.device, mirror->memory);
        if (mirror->buffer && g_indexed.DestroyBuffer)
            g_indexed.DestroyBuffer(g_indexed.device, mirror->buffer, NULL);
        if (mirror->memory && g_indexed.FreeMemory)
            g_indexed.FreeMemory(g_indexed.device, mirror->memory, NULL);
        memset(mirror, 0, sizeof(*mirror));
    }
    for (uint32_t i = 0; i < TARGET_MIRROR_COUNT; ++i) {
        TargetMirror *mirror = &g_indexed.target_mirrors[i];
        writeback_target_mirror(mirror);
        if (mirror->map && g_indexed.UnmapMemory)
            g_indexed.UnmapMemory(g_indexed.device, mirror->memory);
        if (mirror->buffer && g_indexed.DestroyBuffer)
            g_indexed.DestroyBuffer(g_indexed.device, mirror->buffer, NULL);
        if (mirror->memory && g_indexed.FreeMemory)
            g_indexed.FreeMemory(g_indexed.device, mirror->memory, NULL);
        memset(mirror, 0, sizeof(*mirror));
    }
}

void vulkan_indexed_renderer_destroy(void)
{
    destroy_buffers();
    if (g_indexed.device) {
        for (uint32_t i = 0; i < SUBMISSION_RING_SIZE; ++i) {
            SubmissionSlot *slot = &g_indexed.submissions[i];
            if (slot->fence && g_indexed.DestroyFence)
                g_indexed.DestroyFence(g_indexed.device, slot->fence, NULL);
        }
        if (g_indexed.pipeline) g_indexed.DestroyPipeline(g_indexed.device, g_indexed.pipeline, NULL);
        if (g_indexed.pipeline_layout) g_indexed.DestroyPipelineLayout(g_indexed.device, g_indexed.pipeline_layout, NULL);
        if (g_indexed.descriptor_pool) g_indexed.DestroyDescriptorPool(g_indexed.device, g_indexed.descriptor_pool, NULL);
        if (g_indexed.descriptor_layout) g_indexed.DestroyDescriptorSetLayout(g_indexed.device, g_indexed.descriptor_layout, NULL);
        if (g_indexed.command_pool) g_indexed.DestroyCommandPool(g_indexed.device, g_indexed.command_pool, NULL);
    }
    memset(&g_indexed, 0, sizeof(g_indexed));
}

static int initialize(void)
{
    const char *stage_name = "startup";
    if (g_indexed.initialized) return 1;
    if (g_indexed.unavailable) return 0;
    if (!beer_vulkan_context_initialize()) goto fail;
    g_indexed.instance = beer_vulkan_instance();
    g_indexed.physical_device = beer_vulkan_physical_device();
    g_indexed.device = beer_vulkan_device();
    g_indexed.queue = beer_vulkan_queue();
    g_indexed.queue_family = beer_vulkan_queue_family();
    g_indexed.GetInstanceProcAddr = beer_vulkan_get_instance_proc_addr();
    g_indexed.GetDeviceProcAddr = beer_vulkan_get_device_proc_addr();
    if (!g_indexed.instance || !g_indexed.physical_device ||
        !g_indexed.device || !g_indexed.queue ||
        !g_indexed.GetInstanceProcAddr || !g_indexed.GetDeviceProcAddr)
        goto fail;
    LOAD_INSTANCE(GetPhysicalDeviceMemoryProperties);
#define LOAD_ALL_DEVICE() do { \
    LOAD_DEVICE(DestroyDevice); LOAD_DEVICE(GetDeviceQueue); \
    LOAD_DEVICE(CreateCommandPool); LOAD_DEVICE(DestroyCommandPool); \
    LOAD_DEVICE(AllocateCommandBuffers); LOAD_DEVICE(ResetCommandBuffer); \
    LOAD_DEVICE(BeginCommandBuffer); LOAD_DEVICE(EndCommandBuffer); \
    LOAD_DEVICE(CreateDescriptorSetLayout); LOAD_DEVICE(DestroyDescriptorSetLayout); \
    LOAD_DEVICE(CreateDescriptorPool); LOAD_DEVICE(DestroyDescriptorPool); \
    LOAD_DEVICE(AllocateDescriptorSets); LOAD_DEVICE(UpdateDescriptorSets); \
    LOAD_DEVICE(CreateShaderModule); LOAD_DEVICE(DestroyShaderModule); \
    LOAD_DEVICE(CreatePipelineLayout); LOAD_DEVICE(DestroyPipelineLayout); \
    LOAD_DEVICE(CreateComputePipelines); LOAD_DEVICE(DestroyPipeline); \
    LOAD_DEVICE(CreateBuffer); LOAD_DEVICE(DestroyBuffer); \
    LOAD_DEVICE(GetBufferMemoryRequirements); LOAD_DEVICE(AllocateMemory); \
    LOAD_DEVICE(FreeMemory); LOAD_DEVICE(BindBufferMemory); \
    LOAD_DEVICE(MapMemory); LOAD_DEVICE(UnmapMemory); \
    LOAD_DEVICE(CmdBindPipeline); LOAD_DEVICE(CmdBindDescriptorSets); \
    LOAD_DEVICE(CmdPushConstants); LOAD_DEVICE(CmdDispatch); \
    LOAD_DEVICE(CmdPipelineBarrier); LOAD_DEVICE(CmdFillBuffer); \
    LOAD_DEVICE(QueueSubmit); LOAD_DEVICE(CreateFence); LOAD_DEVICE(DestroyFence); \
    LOAD_DEVICE(ResetFences); LOAD_DEVICE(WaitForFences); LOAD_DEVICE(DeviceWaitIdle); \
} while (0)
    LOAD_ALL_DEVICE();
#undef LOAD_ALL_DEVICE
    g_indexed.GetDeviceQueue(g_indexed.device, g_indexed.queue_family, 0, &g_indexed.queue);

    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = g_indexed.queue_family
    };
    if (g_indexed.CreateCommandPool(g_indexed.device, &pool_info, NULL, &g_indexed.command_pool) != VK_SUCCESS)
        goto fail;
    VkCommandBuffer command_buffers[SUBMISSION_RING_SIZE];
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = g_indexed.command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = SUBMISSION_RING_SIZE
    };
    if (g_indexed.AllocateCommandBuffers(g_indexed.device, &command_info, command_buffers) != VK_SUCCESS)
        goto fail;
    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
    };
    for (uint32_t i = 0; i < SUBMISSION_RING_SIZE; ++i) {
        g_indexed.submissions[i].command_buffer = command_buffers[i];
        if (g_indexed.CreateFence(g_indexed.device, &fence_info, NULL,
                                  &g_indexed.submissions[i].fence) != VK_SUCCESS)
            goto fail;
    }

    VkDescriptorSetLayoutBinding bindings[BUFFER_COUNT];
    memset(bindings, 0, sizeof(bindings));
    for (uint32_t i = 0; i < BUFFER_COUNT; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo descriptor_layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = BUFFER_COUNT,
        .pBindings = bindings
    };
    if (g_indexed.CreateDescriptorSetLayout(g_indexed.device, &descriptor_layout_info, NULL,
                                             &g_indexed.descriptor_layout) != VK_SUCCESS)
        goto fail;
    VkPushConstantRange push_range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(PushConstants)
    };
    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &g_indexed.descriptor_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range
    };
    if (g_indexed.CreatePipelineLayout(g_indexed.device, &pipeline_layout_info, NULL,
                                        &g_indexed.pipeline_layout) != VK_SUCCESS)
        goto fail;

    uint32_t *shader_code = NULL;
    size_t shader_size = 0;
    if (!read_shader(&shader_code, &shader_size)) goto fail;
    VkShaderModuleCreateInfo shader_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = shader_size,
        .pCode = shader_code
    };
    VkShaderModule shader_module = VK_NULL_HANDLE;
    VkResult shader_result = g_indexed.CreateShaderModule(g_indexed.device, &shader_info, NULL, &shader_module);
    free(shader_code);
    if (shader_result != VK_SUCCESS) goto fail;
    VkPipelineShaderStageCreateInfo shader_stage = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = shader_module,
        .pName = "main"
    };
    VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = shader_stage,
        .layout = g_indexed.pipeline_layout
    };
    VkResult pipeline_result = g_indexed.CreateComputePipelines(
        g_indexed.device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &g_indexed.pipeline);
    g_indexed.DestroyShaderModule(g_indexed.device, shader_module, NULL);
    if (pipeline_result != VK_SUCCESS) goto fail;

    VkDescriptorPoolSize pool_size = {
        .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = BUFFER_COUNT * SUBMISSION_RING_SIZE
    };
    VkDescriptorPoolCreateInfo descriptor_pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = SUBMISSION_RING_SIZE,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size
    };
    if (g_indexed.CreateDescriptorPool(g_indexed.device, &descriptor_pool_info, NULL,
                                        &g_indexed.descriptor_pool) != VK_SUCCESS)
        goto fail;
    VkDescriptorSetLayout set_layouts[SUBMISSION_RING_SIZE];
    VkDescriptorSet descriptor_sets[SUBMISSION_RING_SIZE];
    for (uint32_t i = 0; i < SUBMISSION_RING_SIZE; ++i)
        set_layouts[i] = g_indexed.descriptor_layout;
    VkDescriptorSetAllocateInfo descriptor_set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = g_indexed.descriptor_pool,
        .descriptorSetCount = SUBMISSION_RING_SIZE,
        .pSetLayouts = set_layouts
    };
    if (g_indexed.AllocateDescriptorSets(g_indexed.device, &descriptor_set_info,
                                          descriptor_sets) != VK_SUCCESS)
        goto fail;
    for (uint32_t i = 0; i < SUBMISSION_RING_SIZE; ++i)
        g_indexed.submissions[i].descriptor_set = descriptor_sets[i];

    g_indexed.initialized = 1;
    fprintf(stderr, "[D3D11 VULKAN] indexed compute renderer initialized\n");
    return 1;
fail:
    fprintf(stderr, "[D3D11 VULKAN] indexed renderer unavailable at %s\n",
            stage_name);
    vulkan_indexed_renderer_destroy();
    g_indexed.unavailable = 1;
    return 0;
}

static void destroy_texture_mirror(TextureMirror *mirror)
{
    if (!mirror || !g_indexed.device) return;
    if (mirror->map) g_indexed.UnmapMemory(g_indexed.device, mirror->memory);
    if (mirror->buffer) g_indexed.DestroyBuffer(g_indexed.device, mirror->buffer, NULL);
    if (mirror->memory) g_indexed.FreeMemory(g_indexed.device, mirror->memory, NULL);
    memset(mirror, 0, sizeof(*mirror));
}

static int allocate_host_buffer(VkDeviceSize capacity, VkBuffer *buffer,
                                VkDeviceMemory *memory, uint8_t **map)
{
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = capacity,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    if (g_indexed.CreateBuffer(g_indexed.device, &buffer_info, NULL, buffer) != VK_SUCCESS)
        return 0;
    VkMemoryRequirements requirements;
    g_indexed.GetBufferMemoryRequirements(g_indexed.device, *buffer, &requirements);
    uint32_t memory_type;
    if (!find_memory_type(requirements.memoryTypeBits, &memory_type)) return 0;
    VkMemoryAllocateInfo allocation_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type
    };
    return g_indexed.AllocateMemory(g_indexed.device, &allocation_info, NULL, memory) == VK_SUCCESS &&
        g_indexed.BindBufferMemory(g_indexed.device, *buffer, *memory, 0) == VK_SUCCESS &&
        g_indexed.MapMemory(g_indexed.device, *memory, 0, capacity, 0, (void **)map) == VK_SUCCESS;
}

static void writeback_target_mirror(TargetMirror *mirror)
{
    if (!mirror || !mirror->gpu_dirty || !mirror->cpu_pixels || !mirror->map ||
        !mirror->bytes)
        return;
    memcpy(mirror->cpu_pixels, mirror->map, mirror->bytes);
    mirror->gpu_dirty = 0;
}

static TargetMirror *acquire_target_mirror(const BeerVulkanIndexedDraw *draw)
{
    TargetMirror *candidate = NULL;
    for (uint32_t i = 0; i < TARGET_MIRROR_COUNT; ++i) {
        TargetMirror *mirror = &g_indexed.target_mirrors[i];
        if (mirror->resource == draw->target_resource &&
            mirror->bytes == draw->target_bytes) {
            candidate = mirror;
            break;
        }
        if (!candidate || !mirror->resource || mirror->last_use < candidate->last_use)
            candidate = mirror;
    }
    if (!candidate) return NULL;
    if ((candidate->resource && candidate->resource != draw->target_resource) ||
        candidate->bytes != draw->target_bytes ||
        candidate->capacity < draw->target_bytes) {
        /* The submission ring may still reference this persistent mirror.
         * Drain it before writeback, destruction, or reassignment. */
        if (!wait_for_pending_submission()) return NULL;
        writeback_target_mirror(candidate);
        /* Reassigning or resizing a slot discards whatever the device memory
         * held. Never let the next draw treat those bytes as this target's
         * accumulated frame. */
        candidate->content_valid = 0;
    }
    if (candidate->capacity < draw->target_bytes) {
        if (candidate->map) g_indexed.UnmapMemory(g_indexed.device, candidate->memory);
        if (candidate->buffer) g_indexed.DestroyBuffer(g_indexed.device, candidate->buffer, NULL);
        if (candidate->memory) g_indexed.FreeMemory(g_indexed.device, candidate->memory, NULL);
        memset(candidate, 0, sizeof(*candidate));
        if (!allocate_host_buffer(draw->target_bytes, &candidate->buffer,
                                  &candidate->memory, &candidate->map))
            return NULL;
        candidate->capacity = draw->target_bytes;
    }
    candidate->resource = draw->target_resource;
    candidate->cpu_pixels = draw->target;
    candidate->bytes = draw->target_bytes;
    candidate->last_use = ++g_indexed.target_use_serial;
    return candidate;
}

static TextureMirror *acquire_texture_mirror(const BeerVulkanIndexedDraw *draw)
{
    TextureMirror *candidate = NULL;
    for (uint32_t i = 0; i < TEXTURE_MIRROR_COUNT; ++i) {
        TextureMirror *mirror = &g_indexed.texture_mirrors[i];
        if (mirror->resource == draw->texture_resource &&
            mirror->bytes == draw->texture_bytes &&
            mirror->width == draw->texture_width && mirror->height == draw->texture_height) {
            candidate = mirror;
            break;
        }
        if (!candidate || !mirror->resource || mirror->last_use < candidate->last_use)
            candidate = mirror;
    }
    if (!candidate) return NULL;
    if (candidate->capacity < draw->texture_bytes) {
        /* A queued draw can still sample this cache entry. */
        if (!wait_for_pending_submission()) return NULL;
        destroy_texture_mirror(candidate);
        if (!allocate_host_buffer(draw->texture_bytes, &candidate->buffer,
                                  &candidate->memory, &candidate->map)) {
            destroy_texture_mirror(candidate);
            return NULL;
        }
        candidate->capacity = draw->texture_bytes;
    }
    int content_changed = candidate->resource != draw->texture_resource ||
        candidate->serial != draw->texture_serial ||
        candidate->bytes != draw->texture_bytes ||
        candidate->width != draw->texture_width ||
        candidate->height != draw->texture_height;
    if (content_changed) {
        /* Do not mutate host-visible texture storage while an in-flight slot
         * may still sample it. Unchanged cached textures remain asynchronous. */
        if (!wait_for_pending_submission()) return NULL;
        memcpy(candidate->map, draw->texture, draw->texture_bytes);
        candidate->resource = draw->texture_resource;
        candidate->serial = draw->texture_serial;
        candidate->bytes = draw->texture_bytes;
        candidate->width = draw->texture_width;
        candidate->height = draw->texture_height;
        ++g_indexed.texture_uploads;
        if (getenv("BEER_RENDER_DIAGNOSTICS") &&
            (g_indexed.texture_uploads <= 4 || (g_indexed.texture_uploads % 64u) == 0))
            fprintf(stderr, "[D3D11 VULKAN] texture mirror upload=%llu serial=%llu "
                    "bytes=%zu %ux%u\n",
                    (unsigned long long)g_indexed.texture_uploads,
                    (unsigned long long)draw->texture_serial,
                    draw->texture_bytes, draw->texture_width, draw->texture_height);
    } else {
        ++g_indexed.texture_cache_hits;
    }
    candidate->last_use = ++g_indexed.texture_use_serial;
    return candidate;
}

static int ensure_buffers(SubmissionSlot *slot,
                          const VkDeviceSize required[BUFFER_COUNT])
{
    int recreate = 0;
    for (uint32_t i = 0; i < BUFFER_COUNT; ++i)
        if (slot->capacities[i] < required[i]) recreate = 1;
    if (!recreate) return 1;
    /* Growing one submission slot must not evict persistent texture or
     * render-target mirrors, or disturb other in-flight ring slots. */
    destroy_submission_buffers(slot);

    VkDescriptorBufferInfo infos[BUFFER_COUNT];
    VkWriteDescriptorSet writes[BUFFER_COUNT];
    memset(infos, 0, sizeof(infos));
    memset(writes, 0, sizeof(writes));
    for (uint32_t i = 0; i < BUFFER_COUNT; ++i) {
        VkDeviceSize capacity = required[i] ? required[i] : 4;
        VkBufferCreateInfo buffer_info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = capacity,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE
        };
        if (g_indexed.CreateBuffer(g_indexed.device, &buffer_info, NULL,
                                   &slot->buffers[i]) != VK_SUCCESS)
            return 0;
        VkMemoryRequirements memory_requirements;
        g_indexed.GetBufferMemoryRequirements(g_indexed.device, slot->buffers[i],
                                               &memory_requirements);
        uint32_t memory_type;
        if (!find_memory_type(memory_requirements.memoryTypeBits, &memory_type)) return 0;
        VkMemoryAllocateInfo allocation_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = memory_requirements.size,
            .memoryTypeIndex = memory_type
        };
        if (g_indexed.AllocateMemory(g_indexed.device, &allocation_info, NULL,
                                     &slot->memories[i]) != VK_SUCCESS ||
            g_indexed.BindBufferMemory(g_indexed.device, slot->buffers[i],
                                       slot->memories[i], 0) != VK_SUCCESS ||
            g_indexed.MapMemory(g_indexed.device, slot->memories[i], 0,
                                capacity, 0, (void **)&slot->maps[i]) != VK_SUCCESS)
            return 0;
        slot->capacities[i] = capacity;
        infos[i].buffer = slot->buffers[i];
        infos[i].range = capacity;
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = slot->descriptor_set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &infos[i];
    }
    g_indexed.UpdateDescriptorSets(g_indexed.device, BUFFER_COUNT, writes, 0, NULL);
    return 1;
}

int vulkan_indexed_renderer_draw(const BeerVulkanIndexedDraw *draw)
{
    const int textured_mode = draw &&
        ((draw->mode >= 3 && draw->mode <= 10) || draw->mode == 12 ||
         draw->mode == 13 || draw->mode == 14 || draw->mode == 15 ||
         draw->mode == 16);
    const size_t texture_bpp = draw &&
        (draw->mode == 5 || draw->mode == 10 || draw->mode == 14) ? 1u : 4u;
    const size_t required_texture_bytes = draw && draw->texture_width &&
        draw->texture_height && draw->texture_width <= SIZE_MAX / draw->texture_height &&
        (size_t)draw->texture_width * draw->texture_height <= SIZE_MAX / texture_bpp
        ? (size_t)draw->texture_width * draw->texture_height * texture_bpp : SIZE_MAX;
    if (!draw || !draw->vertices || !draw->indices || !draw->constants ||
        !draw->target || !draw->width || !draw->height || !draw->index_count ||
        draw->index_count > 255u || draw->index_count % 3u != 0u ||
        draw->mode < 1 || draw->mode > 16 ||
        (draw->vertex_stride != 8 && draw->vertex_stride != 16 &&
         draw->vertex_stride != 12 && draw->vertex_stride != 20 &&
         draw->vertex_stride != 24) ||
        draw->target_bytes < (size_t)draw->width * draw->height * 4 ||
        (textured_mode &&
         (!draw->texture || !draw->texture_resource ||
          !draw->texture_width || !draw->texture_height ||
          draw->texture_bytes < required_texture_bytes))) {
        if (draw && draw->mode == 14)
            fprintf(stderr, "[D3D11 VULKAN MODE14 REJECT] validation "
                    "vertices=%p/%zu indices=%p/%zu constants=%p/%zu "
                    "target=%p/%zu/%ux%u texture=%p resource=%p/%zu/%ux%u "
                    "required=%zu count=%u stride=%u\n",
                    (const void *)draw->vertices, draw->vertex_bytes,
                    (const void *)draw->indices, draw->index_bytes,
                    (const void *)draw->constants, draw->constant_bytes,
                    (void *)draw->target, draw->target_bytes,
                    draw->width, draw->height, (const void *)draw->texture,
                    draw->texture_resource, draw->texture_bytes,
                    draw->texture_width, draw->texture_height,
                    required_texture_bytes, draw->index_count,
                    draw->vertex_stride);
        return 0;
    }

    /* D3D11 deferred command lists can execute concurrently on Beer worker
     * threads. Serialize host recording, but rotate independent command,
     * descriptor, staging and fence slots so several queue submissions can
     * remain in flight. Wait only when the selected ring slot is reused. */
    pthread_mutex_lock(&g_indexed_lock);
    int result = 0;
    VkDeviceSize required[BUFFER_COUNT] = {
        draw->vertex_bytes, draw->index_bytes, draw->constant_bytes, 4, 4,
        draw->mode == 16 ? draw->texture2_bytes : 4
    };
    if (!initialize()) goto done;
    SubmissionSlot *slot = &g_indexed.submissions[g_indexed.next_submission];
    if (!wait_for_submission(slot) || !ensure_buffers(slot, required)) goto done;
    memcpy(slot->maps[0], draw->vertices, draw->vertex_bytes);
    memcpy(slot->maps[1], draw->indices, draw->index_bytes);
    memcpy(slot->maps[2], draw->constants, draw->constant_bytes);

    TargetMirror *target_mirror = acquire_target_mirror(draw);
    if (!target_mirror) goto done;
    BeerTargetMirrorState mirror_state = target_mirror_state(target_mirror);
    int upload_target = beer_target_mirror_needs_upload(
        &mirror_state, draw->target_resource, draw->target_input_serial,
        draw->target_bytes);
    if (upload_target) {
        /* A CPU clear/update supersedes the mirror. Drain queued users before
         * replacing its contents. */
        if (!wait_for_pending_submission()) goto done;
        memcpy(target_mirror->map, draw->target, draw->target_bytes);
        target_mirror->serial = draw->target_input_serial;
        target_mirror->content_valid = 1;
        target_mirror->gpu_dirty = 0;
        ++g_indexed.target_uploads;
    }
    VkDescriptorBufferInfo target_info = {
        .buffer = target_mirror->buffer,
        .range = draw->target_bytes
    };
    VkWriteDescriptorSet target_write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = slot->descriptor_set,
        .dstBinding = 3,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &target_info
    };
    g_indexed.UpdateDescriptorSets(g_indexed.device, 1, &target_write, 0, NULL);

    VkBuffer texture_buffer = slot->buffers[4];
    VkDeviceSize texture_range = 4;
    if ((draw->mode >= 3 && draw->mode <= 10) || draw->mode == 12 ||
        draw->mode == 13 || draw->mode == 14 || draw->mode == 15 ||
        draw->mode == 16) {
        TextureMirror *mirror = acquire_texture_mirror(draw);
        if (!mirror) goto done;
        texture_buffer = mirror->buffer;
        texture_range = draw->texture_bytes;
    } else {
        memset(slot->maps[4], 0, 4);
    }
    VkDescriptorBufferInfo texture_info = {
        .buffer = texture_buffer,
        .range = texture_range
    };
    VkWriteDescriptorSet texture_write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = slot->descriptor_set,
        .dstBinding = 4,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &texture_info
    };
    g_indexed.UpdateDescriptorSets(g_indexed.device, 1, &texture_write, 0, NULL);

    VkBuffer texture2_buffer = slot->buffers[5];
    VkDeviceSize texture2_range = 4;
    if (draw->mode == 16) {
        if (!draw->texture2 || !draw->texture2_resource ||
            draw->texture2_bytes < (size_t)draw->texture2_width *
                draw->texture2_height * 4u)
            goto done;
        /* Mode 16 is the only observed dual-texture path. Its 64x64 Bink
         * conversion surfaces are tiny; keep the second texture in the
         * submission slot until the general mirror cache supports multiple
         * SRVs per draw. */
        memcpy(slot->maps[5], draw->texture2, draw->texture2_bytes);
        texture2_range = draw->texture2_bytes;
    } else {
        memset(slot->maps[5], 0, 4);
    }
    VkDescriptorBufferInfo texture2_info = {
        .buffer = texture2_buffer,
        .range = texture2_range
    };
    VkWriteDescriptorSet texture2_write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = slot->descriptor_set,
        .dstBinding = 5,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &texture2_info
    };
    g_indexed.UpdateDescriptorSets(g_indexed.device, 1, &texture2_write, 0, NULL);

    PushConstants push = {
        .width = draw->width,
        .height = draw->height,
        .vertex_offset = draw->vertex_offset,
        .index_offset = draw->index_offset,
        .index_count = draw->index_count,
        .base_vertex = draw->base_vertex,
        .mode = draw->mode,
        .stride = draw->vertex_stride,
        .blend_enable = draw->blend_enable,
        .source_blend = draw->source_blend,
        .destination_blend = draw->destination_blend,
        .color_operation = draw->color_operation,
        .source_alpha = draw->source_alpha,
        .destination_alpha = draw->destination_alpha,
        .alpha_operation = draw->alpha_operation,
        .write_mask = draw->write_mask
    };
    memcpy(push.viewport, draw->viewport, sizeof(push.viewport));
    memcpy(push.scissor, draw->scissor, sizeof(push.scissor));
    memcpy(push.blend_factor, draw->blend_factor, sizeof(push.blend_factor));
    memcpy(push.constant_color, draw->constant_color, sizeof(push.constant_color));
    push.texture_width = draw->texture_width;
    push.texture_height = draw->texture_height;
    push.sampler_flags = (draw->texture_linear ? 1u : 0u) |
        ((draw->texture_address_u & 7u) << 1u) |
        ((draw->texture_address_v & 7u) << 4u);

    /* This compute rasterizer previously dispatched every target pixel and
     * tested every triangle. Scaleform draws usually cover a small rectangle,
     * so derive a conservative screen-space bound from the indexed vertices
     * and dispatch only that region. Fall back to the scissor/viewport extent
     * if an input is malformed; correctness remains identical. */
    float minimum_x = FLT_MAX, minimum_y = FLT_MAX;
    float maximum_x = -FLT_MAX, maximum_y = -FLT_MAX;
    const uint16_t *index_data = (const uint16_t *)(draw->indices + draw->index_offset);
    const float *constant = (const float *)draw->constants;
    uint32_t constant_count = (uint32_t)(draw->constant_bytes / sizeof(float));
    uint32_t transform_row = (draw->mode == 2u || draw->mode == 3u ||
                              draw->mode == 5u || draw->mode == 6u ||
                              draw->mode == 7u || draw->mode == 11u) ? 8u : 0u;
    /* Modes 9, 10 and 13 select constant-buffer transform rows per vertex.
     * Let the shader evaluate those dynamic rows and conservatively dispatch
     * the scissor rather than deriving an incorrect host-side rectangle. */
    int valid_bounds = draw->mode != 9u && draw->mode != 10u &&
        draw->mode != 13u && constant_count >= transform_row + 8u;
    for (uint32_t first = 0; valid_bounds && first < draw->index_count; ++first) {
        int64_t vertex_index = (int64_t)index_data[first] + draw->base_vertex;
        size_t position_offset = draw->vertex_offset;
        if (vertex_index < 0 ||
            (size_t)vertex_index > (SIZE_MAX - position_offset) / draw->vertex_stride) {
            valid_bounds = 0;
            break;
        }
        position_offset += (size_t)vertex_index * draw->vertex_stride +
            (draw->mode == 11u ? 0u :
             draw->mode == 7u ? 8u :
             draw->vertex_stride == 16u ? 8u :
             (draw->vertex_stride == 20u || draw->vertex_stride == 24u) ? 12u : 4u);
        if (position_offset + 8u > draw->vertex_bytes) {
            valid_bounds = 0;
            break;
        }
        float input_x, input_y;
        memcpy(&input_x, draw->vertices + position_offset, sizeof(input_x));
        memcpy(&input_y, draw->vertices + position_offset + 4u, sizeof(input_y));
        float clip_x = input_x * constant[transform_row] +
            input_y * constant[transform_row + 1u] + constant[transform_row + 3u];
        float clip_y = input_x * constant[transform_row + 4u] +
            input_y * constant[transform_row + 5u] + constant[transform_row + 7u];
        float screen_x = draw->viewport[0] + (clip_x + 1.0f) * draw->viewport[2] * 0.5f;
        float screen_y = draw->viewport[1] + (1.0f - clip_y) * draw->viewport[3] * 0.5f;
        if (screen_x < minimum_x) minimum_x = screen_x;
        if (screen_x > maximum_x) maximum_x = screen_x;
        if (screen_y < minimum_y) minimum_y = screen_y;
        if (screen_y > maximum_y) maximum_y = screen_y;
    }
    int32_t left = draw->scissor[0] > 0 ? draw->scissor[0] : 0;
    int32_t top = draw->scissor[1] > 0 ? draw->scissor[1] : 0;
    int32_t right = draw->scissor[2] < (int32_t)draw->width
        ? draw->scissor[2] : (int32_t)draw->width;
    int32_t bottom = draw->scissor[3] < (int32_t)draw->height
        ? draw->scissor[3] : (int32_t)draw->height;
    if (valid_bounds && minimum_x <= maximum_x && minimum_y <= maximum_y) {
        int32_t bound_left = (int32_t)minimum_x - 1;
        int32_t bound_top = (int32_t)minimum_y - 1;
        int32_t bound_right = (int32_t)maximum_x + 2;
        int32_t bound_bottom = (int32_t)maximum_y + 2;
        if (bound_left > left) left = bound_left;
        if (bound_top > top) top = bound_top;
        if (bound_right < right) right = bound_right;
        if (bound_bottom < bottom) bottom = bound_bottom;
    }
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > (int32_t)draw->width) right = (int32_t)draw->width;
    if (bottom > (int32_t)draw->height) bottom = (int32_t)draw->height;
    if (right <= left || bottom <= top) {
        /* Bounds are only a performance hint.  They are reconstructed on the
         * host from a small set of known Scaleform vertex programs and must
         * never suppress a real D3D11 draw when constants/geometry evolve.
         * Fall back to the guest scissor (or the complete target) and let the
         * Vulkan shader perform the authoritative vertex transform/coverage
         * test.  The old early-success path silently dropped the draw and also
         * hid it from UI tracing, producing missing retained UI layers. */
        left = draw->scissor[0] > 0 ? draw->scissor[0] : 0;
        top = draw->scissor[1] > 0 ? draw->scissor[1] : 0;
        right = draw->scissor[2] > left &&
            draw->scissor[2] < (int32_t)draw->width
            ? draw->scissor[2] : (int32_t)draw->width;
        bottom = draw->scissor[3] > top &&
            draw->scissor[3] < (int32_t)draw->height
            ? draw->scissor[3] : (int32_t)draw->height;
    }
    push.dispatch_origin_x = (uint32_t)left;
    push.dispatch_origin_y = (uint32_t)top;
    push.dispatch_width = (uint32_t)(right - left);
    push.dispatch_pixels = push.dispatch_width * (uint32_t)(bottom - top);

    if (getenv("BEER_UI_CONTROL_TRACE")) {
        int overlaps_buttons = bottom > (int32_t)(draw->height * 3u / 4u);
        int overlaps_scrollbar = right > (int32_t)(draw->width * 7u / 8u);
        if (overlaps_buttons || overlaps_scrollbar) {
            uint64_t signature = UINT64_C(0xcbf29ce484222325);
            signature = trace_mix(signature, draw->mode);
            signature = trace_mix(signature, draw->index_count);
            signature = trace_mix(signature, draw->index_offset);
            signature = trace_mix(signature, draw->vertex_offset);
            signature = trace_mix(signature, (uint32_t)left);
            signature = trace_mix(signature, (uint32_t)top);
            signature = trace_mix(signature, (uint32_t)right);
            signature = trace_mix(signature, (uint32_t)bottom);
            signature = trace_mix(signature,
                (uint64_t)(uintptr_t)draw->texture_resource);
            static uint64_t seen[128];
            static uint32_t seen_count;
            int fresh = 1;
            for (uint32_t i = 0; i < seen_count; ++i) {
                if (seen[i] == signature) {
                    fresh = 0;
                    break;
                }
            }
            if (fresh && seen_count < 128) {
                seen[seen_count++] = signature;
                fprintf(stderr,
                        "[D3D11 UI CONTROL] mode=%u indices=%u index-offset=%u "
                        "vertex-offset=%u/%u bounds=[%d,%d..%d,%d] "
                        "scissor=[%d,%d..%d,%d] texture=%p/%ux%u "
                        "blend=%u/%u/%u/%u mask=%02x\n",
                        draw->mode, draw->index_count, draw->index_offset,
                        draw->vertex_offset, draw->vertex_stride,
                        left, top, right, bottom,
                        draw->scissor[0], draw->scissor[1],
                        draw->scissor[2], draw->scissor[3],
                        draw->texture_resource, draw->texture_width,
                        draw->texture_height, draw->blend_enable,
                        draw->source_blend, draw->destination_blend,
                        draw->color_operation, draw->write_mask);
            }
        }
    }

    g_indexed.ResetCommandBuffer(slot->command_buffer, 0);
    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    if (g_indexed.BeginCommandBuffer(slot->command_buffer, &begin_info) != VK_SUCCESS)
        goto done;
    /* Queue order alone is not a Vulkan memory dependency. Publish prior
     * indexed target writes and host-coherent staging uploads before this draw
     * reads or blends them. This is required now that the submission ring can
     * keep several draws in flight. */
    VkMemoryBarrier draw_ready = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
    };
    g_indexed.CmdPipelineBarrier(slot->command_buffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
        1, &draw_ready, 0, NULL, 0, NULL);
    g_indexed.CmdBindPipeline(slot->command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                              g_indexed.pipeline);
    g_indexed.CmdBindDescriptorSets(slot->command_buffer,
                                    VK_PIPELINE_BIND_POINT_COMPUTE,
                                    g_indexed.pipeline_layout, 0, 1,
                                    &slot->descriptor_set, 0, NULL);
    g_indexed.CmdPushConstants(slot->command_buffer, g_indexed.pipeline_layout,
                               VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    g_indexed.CmdDispatch(slot->command_buffer,
                          (push.dispatch_pixels + 255u) / 256u, 1, 1);
    if (g_indexed.EndCommandBuffer(slot->command_buffer) != VK_SUCCESS) goto done;
    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &slot->command_buffer
    };
    if (g_indexed.ResetFences(g_indexed.device, 1, &slot->fence) != VK_SUCCESS)
        goto done;
    beer_vulkan_queue_lock();
    VkResult submit_result = g_indexed.QueueSubmit(
        g_indexed.queue, 1, &submit_info, slot->fence);
    beer_vulkan_queue_unlock();
    if (submit_result != VK_SUCCESS) goto done;
    slot->pending = 1;
    g_indexed.next_submission =
        (g_indexed.next_submission + 1u) % SUBMISSION_RING_SIZE;
    target_mirror->serial = draw->target_output_serial;
    target_mirror->content_valid = 1;
    target_mirror->gpu_dirty = 1;
    static uint32_t executions;
    /* Modes are numbered 1..16. Keep index 0 unused so the runtime mode can
     * be used directly without corrupting adjacent static state. */
    static uint32_t mode_executions[17];
    ++executions;
    uint32_t mode_execution = ++mode_executions[draw->mode];
    if (getenv("BEER_UI_STATE_TRACE")) {
        uint64_t draw_hash = UINT64_C(0xcbf29ce484222325);
        draw_hash = trace_mix(draw_hash, draw->mode);
        draw_hash = trace_mix(draw_hash, draw->index_count);
        draw_hash = trace_mix(draw_hash, draw->index_offset);
        draw_hash = trace_mix(draw_hash, draw->vertex_offset);
        draw_hash = trace_mix(draw_hash, draw->vertex_stride);
        draw_hash = trace_bytes(draw_hash, draw->constants, draw->constant_bytes);
        draw_hash = trace_bytes(draw_hash, draw->vertices, draw->vertex_bytes);
        draw_hash = trace_bytes(draw_hash, draw->indices, draw->index_bytes);
        g_indexed.frame_draw_signature = trace_mix(
            g_indexed.frame_draw_signature, draw_hash);
        g_indexed.frame_resource_signature = trace_mix(
            g_indexed.frame_resource_signature,
            (uint64_t)(uintptr_t)draw->texture_resource);
        g_indexed.frame_resource_signature = trace_mix(
            g_indexed.frame_resource_signature, draw->texture_serial);
        g_indexed.frame_resource_signature = trace_mix(
            g_indexed.frame_resource_signature,
            (uint64_t)(uintptr_t)draw->target_resource);
        g_indexed.frame_mode_draw_signatures[draw->mode] = trace_mix(
            g_indexed.frame_mode_draw_signatures[draw->mode], draw_hash);
        uint64_t mode_resource_hash = UINT64_C(0xcbf29ce484222325);
        mode_resource_hash = trace_mix(mode_resource_hash,
            (uint64_t)(uintptr_t)draw->texture_resource);
        mode_resource_hash = trace_mix(mode_resource_hash, draw->texture_serial);
        mode_resource_hash = trace_mix(mode_resource_hash,
            (uint64_t)(uintptr_t)draw->target_resource);
        g_indexed.frame_mode_resource_signatures[draw->mode] = trace_mix(
            g_indexed.frame_mode_resource_signatures[draw->mode],
            mode_resource_hash);
        ++g_indexed.frame_draw_count;
        ++g_indexed.frame_mode_counts[draw->mode];
        if (draw->mode == 3 || draw->mode == 12) {
            static uint64_t ui_draw_sequence;
            fprintf(stderr,
                    "[D3D11 UI DRAW] seq=%llu mode=%u draw=%016llx resource=%016llx "
                    "indices=%u/%u vertices=%u/%u texture=%p serial=%llu %ux%u "
                    "target=%p output=%llu blend=%u/%u/%u/%u mask=%02x\n",
                    (unsigned long long)++ui_draw_sequence, draw->mode,
                    (unsigned long long)draw_hash,
                    (unsigned long long)mode_resource_hash,
                    draw->index_offset, draw->index_count,
                    draw->vertex_offset, draw->vertex_stride,
                    draw->texture_resource,
                    (unsigned long long)draw->texture_serial,
                    draw->texture_width, draw->texture_height,
                    draw->target_resource,
                    (unsigned long long)draw->target_output_serial,
                    draw->blend_enable, draw->source_blend,
                    draw->destination_blend, draw->color_operation,
                    draw->write_mask);
        }
    }
    if (getenv("BEER_RENDER_DIAGNOSTICS") &&
        (mode_execution <= 4 || mode_execution % 64 == 0))
        fprintf(stderr, "[D3D11 VULKAN] indexed execution=%u mode=%u "
                "mode-execution=%u indices=%u %ux%u\n",
                executions, draw->mode, mode_execution, draw->index_count,
                draw->width, draw->height);
    result = 1;
done:
    if (!result && draw && draw->mode == 14)
        fprintf(stderr, "[D3D11 VULKAN MODE14 REJECT] runtime "
                "initialized=%d unavailable=%d next-slot=%u\n",
                g_indexed.initialized, g_indexed.unavailable,
                g_indexed.next_submission);
    pthread_mutex_unlock(&g_indexed_lock);
    return result;
}

static uint8_t clear_channel(float value)
{
    if (!(value > 0.0f)) return 0;
    if (value >= 1.0f) return 255;
    return (uint8_t)(value * 255.0f + 0.5f);
}

int vulkan_indexed_renderer_clear_target(const void *resource,
                                         uint64_t input_serial,
                                         uint64_t output_serial,
                                         uint8_t *pixels, size_t bytes,
                                         uint32_t width, uint32_t height,
                                         uint32_t format,
                                         const float color[4])
{
    if (!resource || !pixels || !bytes || !width || !height || !color ||
        (format != 28 && format != 29 && format != 87 && format != 91) ||
        (size_t)width > SIZE_MAX / 4u / (size_t)height ||
        bytes < (size_t)width * (size_t)height * 4u)
        return 0;

    pthread_mutex_lock(&g_indexed_lock);
    int result = 0;
    if (!initialize()) goto done;

    SubmissionSlot *slot = &g_indexed.submissions[g_indexed.next_submission];
    if (!wait_for_submission(slot)) goto done;

    BeerVulkanIndexedDraw mirror_description = {
        .target = pixels,
        .target_bytes = bytes,
        .target_resource = resource,
    };
    TargetMirror *mirror = acquire_target_mirror(&mirror_description);
    if (!mirror) goto done;

    BeerTargetMirrorState mirror_state = target_mirror_state(mirror);
    if (beer_target_mirror_needs_upload(&mirror_state, resource, input_serial,
                                        bytes)) {
        /* A CPU-side update supersedes the cached GPU contents. Drain all
         * readers before replacing the host-visible mirror. */
        if (!wait_for_pending_submission()) goto done;
        memcpy(mirror->map, pixels, bytes);
        mirror->serial = input_serial;
        /* The device memory now provably holds this resource's input frame;
         * without this the clear below would be dropped by the next draw's
         * re-upload of the pre-clear CPU copy. */
        mirror->content_valid = 1;
        mirror->gpu_dirty = 0;
        ++g_indexed.target_uploads;
    }

    uint8_t red = clear_channel(color[0]);
    uint8_t green = clear_channel(color[1]);
    uint8_t blue = clear_channel(color[2]);
    uint8_t alpha = clear_channel(color[3]);
    uint32_t packed;
    if (format == 87 || format == 91)
        packed = (uint32_t)blue | ((uint32_t)green << 8u) |
            ((uint32_t)red << 16u) | ((uint32_t)alpha << 24u);
    else
        packed = (uint32_t)red | ((uint32_t)green << 8u) |
            ((uint32_t)blue << 16u) | ((uint32_t)alpha << 24u);

    g_indexed.ResetCommandBuffer(slot->command_buffer, 0);
    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (g_indexed.BeginCommandBuffer(slot->command_buffer, &begin_info) != VK_SUCCESS)
        goto done;

    VkMemoryBarrier before_clear = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
    };
    g_indexed.CmdPipelineBarrier(slot->command_buffer,
        VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
        1, &before_clear, 0, NULL, 0, NULL);
    g_indexed.CmdFillBuffer(slot->command_buffer, mirror->buffer, 0,
                            (VkDeviceSize)((size_t)width * height * 4u), packed);
    VkMemoryBarrier after_clear = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                         VK_ACCESS_SHADER_WRITE_BIT |
                         VK_ACCESS_TRANSFER_READ_BIT,
    };
    g_indexed.CmdPipelineBarrier(slot->command_buffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 1, &after_clear, 0, NULL, 0, NULL);
    if (g_indexed.EndCommandBuffer(slot->command_buffer) != VK_SUCCESS)
        goto done;

    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &slot->command_buffer,
    };
    if (g_indexed.ResetFences(g_indexed.device, 1, &slot->fence) != VK_SUCCESS)
        goto done;
    beer_vulkan_queue_lock();
    VkResult submit_result = g_indexed.QueueSubmit(
        g_indexed.queue, 1, &submit_info, slot->fence);
    beer_vulkan_queue_unlock();
    if (submit_result != VK_SUCCESS) goto done;

    slot->pending = 1;
    g_indexed.next_submission =
        (g_indexed.next_submission + 1u) % SUBMISSION_RING_SIZE;
    mirror->serial = output_serial;
    mirror->content_valid = 1;
    mirror->gpu_dirty = 1;
    mirror->last_use = ++g_indexed.target_use_serial;
    result = 1;

done:
    pthread_mutex_unlock(&g_indexed_lock);
    return result;
}

int vulkan_indexed_renderer_sync_target(const void *resource, uint64_t serial,
                                        uint8_t *pixels, size_t bytes)
{
    if (!resource || !pixels || !bytes) return 0;
    pthread_mutex_lock(&g_indexed_lock);
    int found = 0;
    if (!wait_for_pending_submission()) {
        pthread_mutex_unlock(&g_indexed_lock);
        return 0;
    }
    for (uint32_t i = 0; i < TARGET_MIRROR_COUNT; ++i) {
        TargetMirror *mirror = &g_indexed.target_mirrors[i];
        BeerTargetMirrorState state = target_mirror_state(mirror);
        if (!beer_target_mirror_matches(&state, resource, serial, bytes))
            continue;
        if (mirror->gpu_dirty) {
            memcpy(pixels, mirror->map, bytes);
            mirror->cpu_pixels = pixels;
            mirror->gpu_dirty = 0;
            ++g_indexed.target_syncs;
        }
        mirror->last_use = ++g_indexed.target_use_serial;
        found = 1;
        break;
    }
    pthread_mutex_unlock(&g_indexed_lock);
    return found;
}

int vulkan_indexed_renderer_copy_target(const void *resource, uint64_t serial,
                                        uint8_t *destination, size_t bytes)
{
    if (!resource || !destination || !bytes) return 0;
    pthread_mutex_lock(&g_indexed_lock);
    int found = 0;
    if (!wait_for_pending_submission()) {
        pthread_mutex_unlock(&g_indexed_lock);
        return 0;
    }
    for (uint32_t i = 0; i < TARGET_MIRROR_COUNT; ++i) {
        TargetMirror *mirror = &g_indexed.target_mirrors[i];
        BeerTargetMirrorState state = target_mirror_state(mirror);
        if (!beer_target_mirror_matches(&state, resource, serial, bytes))
            continue;
        memcpy(destination, mirror->map, bytes);
        mirror->last_use = ++g_indexed.target_use_serial;
        found = 1;
        break;
    }
    pthread_mutex_unlock(&g_indexed_lock);
    return found;
}

int vulkan_indexed_renderer_get_target_buffer(const void *resource,
                                              uint64_t serial, size_t bytes,
                                              uint64_t *buffer_handle)
{
    if (!resource || !bytes || !buffer_handle) return 0;
    pthread_mutex_lock(&g_indexed_lock);
    int found = 0;
    for (uint32_t i = 0; i < TARGET_MIRROR_COUNT; ++i) {
        TargetMirror *mirror = &g_indexed.target_mirrors[i];
        BeerTargetMirrorState state = target_mirror_state(mirror);
        if (!beer_target_mirror_matches(&state, resource, serial, bytes) ||
            !mirror->buffer)
            continue;
        /* The compositor submits to the same Vulkan queue. Queue order makes
         * this buffer dependency safe without a host fence wait; CPU readers
         * still synchronize through sync/copy_target above. */
        *buffer_handle = (uint64_t)(uintptr_t)mirror->buffer;
        mirror->last_use = ++g_indexed.target_use_serial;
        found = 1;
        break;
    }
    pthread_mutex_unlock(&g_indexed_lock);
    return found;
}

int vulkan_indexed_renderer_consume_frame_trace(
    uint64_t *draw_signature, uint64_t *resource_signature,
    uint32_t *draw_count, uint32_t mode_counts[17],
    uint64_t mode_draw_signatures[17],
    uint64_t mode_resource_signatures[17])
{
    if (!draw_signature || !resource_signature || !draw_count || !mode_counts ||
        !mode_draw_signatures || !mode_resource_signatures)
        return 0;
    pthread_mutex_lock(&g_indexed_lock);
    *draw_signature = g_indexed.frame_draw_signature;
    *resource_signature = g_indexed.frame_resource_signature;
    *draw_count = g_indexed.frame_draw_count;
    memcpy(mode_counts, g_indexed.frame_mode_counts,
           sizeof(g_indexed.frame_mode_counts));
    memcpy(mode_draw_signatures, g_indexed.frame_mode_draw_signatures,
           sizeof(g_indexed.frame_mode_draw_signatures));
    memcpy(mode_resource_signatures,
           g_indexed.frame_mode_resource_signatures,
           sizeof(g_indexed.frame_mode_resource_signatures));
    g_indexed.frame_draw_signature = 0;
    g_indexed.frame_resource_signature = 0;
    g_indexed.frame_draw_count = 0;
    memset(g_indexed.frame_mode_counts, 0,
           sizeof(g_indexed.frame_mode_counts));
    memset(g_indexed.frame_mode_draw_signatures, 0,
           sizeof(g_indexed.frame_mode_draw_signatures));
    memset(g_indexed.frame_mode_resource_signatures, 0,
           sizeof(g_indexed.frame_mode_resource_signatures));
    pthread_mutex_unlock(&g_indexed_lock);
    return 1;
}

void vulkan_indexed_renderer_forget_resource(const void *resource)
{
    if (!resource) return;
    pthread_mutex_lock(&g_indexed_lock);
    if (!wait_for_pending_submission()) {
        pthread_mutex_unlock(&g_indexed_lock);
        return;
    }
    for (uint32_t i = 0; i < TARGET_MIRROR_COUNT; ++i) {
        TargetMirror *mirror = &g_indexed.target_mirrors[i];
        if (mirror->resource == resource) {
            writeback_target_mirror(mirror);
            mirror->resource = NULL;
            mirror->cpu_pixels = NULL;
            mirror->serial = 0;
            mirror->bytes = 0;
            mirror->gpu_dirty = 0;
            /* The device memory keeps the forgotten surface's pixels. Mark it
             * unusable so a recycled slot cannot be mistaken for valid
             * content at the next serial-0 target. */
            mirror->content_valid = 0;
        }
    }
    for (uint32_t i = 0; i < TEXTURE_MIRROR_COUNT; ++i) {
        TextureMirror *mirror = &g_indexed.texture_mirrors[i];
        if (mirror->resource == resource) {
            /* Keep the allocation for reuse, but invalidate identity/content.
             * D3D11 resource addresses and decoded pixel buffers are both
             * allocator-reusable; retaining either as cache identity can bind
             * stale black texture data to a new disclaimer resource. */
            mirror->resource = NULL;
            mirror->bytes = 0;
            mirror->serial = 0;
            mirror->last_use = 0;
            mirror->width = 0;
            mirror->height = 0;
        }
    }
    pthread_mutex_unlock(&g_indexed_lock);
}
