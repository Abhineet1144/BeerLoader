#define _GNU_SOURCE
#include "vulkan_renderer.h"
#include "vulkan_indexed_renderer.h"
#include "vulkan_context.h"
#include "target_mirror.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#define COMPOSITOR_MIRROR_COUNT 8

typedef struct {
    const void *resource;
    uint8_t *cpu_pixels;
    uint64_t serial;
    uint64_t last_use;
    VkDeviceSize bytes;
    VkDeviceSize capacity;
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint8_t *map;
    int gpu_dirty;
    /* See renderer/vulkan/target_mirror.h: identity plus serial is not
     * sufficient to prove this slot's device memory belongs to the caller's
     * frame, because serial 0 is valid and slots are recycled. */
    int content_valid;
} CompositorMirror;

static BeerTargetMirrorState compositor_mirror_state(
    const CompositorMirror *mirror)
{
    BeerTargetMirrorState state = {
        .resource = mirror->resource,
        .serial = mirror->serial,
        .bytes = (size_t)mirror->bytes,
        .content_valid = mirror->content_valid
    };
    return state;
}

typedef struct {
    void *library;
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkCommandPool command_pool;
    VkCommandBuffer command_buffer;
    VkDescriptorSetLayout descriptor_layout;
    VkDescriptorPool descriptor_pool;
    VkDescriptorSet descriptor_set;
    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;
    VkFence fence;
    VkBuffer buffers[2];
    VkDeviceMemory memories[2];
    uint8_t *maps[2];
    VkDeviceSize capacity;
    CompositorMirror output_mirrors[COMPOSITOR_MIRROR_COUNT];
    uint64_t output_use_serial;
    int submission_pending;
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
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkCreateFence CreateFence;
    PFN_vkDestroyFence DestroyFence;
    PFN_vkResetFences ResetFences;
    PFN_vkWaitForFences WaitForFences;
    PFN_vkDeviceWaitIdle DeviceWaitIdle;
} VulkanRenderer;

static VulkanRenderer g_renderer;
static pthread_mutex_t g_renderer_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_indexed_bridge_copies;
static uint64_t g_compositor_bridge_copies;
static uint64_t g_cpu_input_copies;
static uint64_t g_pending_writebacks;

static int wait_for_pending_submission(void)
{
    if (!g_renderer.submission_pending) return 1;
    if (g_renderer.WaitForFences(g_renderer.device, 1, &g_renderer.fence, VK_TRUE,
                                 UINT64_MAX) != VK_SUCCESS)
        return 0;
    g_renderer.submission_pending = 0;
    return 1;
}

static void writeback_output_mirror(CompositorMirror *mirror)
{
    if (!mirror || !mirror->gpu_dirty || !mirror->cpu_pixels || !mirror->map ||
        !mirror->bytes)
        return;
    memcpy(mirror->cpu_pixels, mirror->map, (size_t)mirror->bytes);
    mirror->gpu_dirty = 0;
    ++g_pending_writebacks;
    if (getenv("BEER_RENDER_DIAGNOSTICS") &&
        (g_pending_writebacks <= 4 || (g_pending_writebacks % 64u) == 0))
        fprintf(stderr, "[D3D11 VULKAN] compositor writeback=%llu resource=%p "
                "serial=%llu bytes=%zu\n",
                (unsigned long long)g_pending_writebacks, mirror->resource,
                (unsigned long long)mirror->serial, (size_t)mirror->bytes);
}

#define LOAD_INSTANCE(name) do { \
    g_renderer.name = (PFN_vk##name)g_renderer.GetInstanceProcAddr( \
        g_renderer.instance, "vk" #name); \
    if (!g_renderer.name) return 0; \
} while (0)
#define LOAD_DEVICE(name) do { \
    g_renderer.name = (PFN_vk##name)g_renderer.GetDeviceProcAddr( \
        g_renderer.device, "vk" #name); \
    if (!g_renderer.name) return 0; \
} while (0)

static void destroy_buffers(void)
{
    if (!g_renderer.device) return;
    if (!wait_for_pending_submission()) return;
    for (uint32_t i = 0; i < 2; ++i) {
        if (g_renderer.maps[i] && g_renderer.UnmapMemory)
            g_renderer.UnmapMemory(g_renderer.device, g_renderer.memories[i]);
        if (g_renderer.buffers[i] && g_renderer.DestroyBuffer)
            g_renderer.DestroyBuffer(g_renderer.device, g_renderer.buffers[i], NULL);
        if (g_renderer.memories[i] && g_renderer.FreeMemory)
            g_renderer.FreeMemory(g_renderer.device, g_renderer.memories[i], NULL);
        g_renderer.maps[i] = NULL;
        g_renderer.buffers[i] = VK_NULL_HANDLE;
        g_renderer.memories[i] = VK_NULL_HANDLE;
    }
    g_renderer.capacity = 0;
    for (uint32_t i = 0; i < COMPOSITOR_MIRROR_COUNT; ++i) {
        CompositorMirror *mirror = &g_renderer.output_mirrors[i];
        writeback_output_mirror(mirror);
        if (mirror->map && g_renderer.UnmapMemory)
            g_renderer.UnmapMemory(g_renderer.device, mirror->memory);
        if (mirror->buffer && g_renderer.DestroyBuffer)
            g_renderer.DestroyBuffer(g_renderer.device, mirror->buffer, NULL);
        if (mirror->memory && g_renderer.FreeMemory)
            g_renderer.FreeMemory(g_renderer.device, mirror->memory, NULL);
        memset(mirror, 0, sizeof(*mirror));
    }
}

void vulkan_renderer_destroy(void)
{
    destroy_buffers();
    if (g_renderer.device) {
        if (g_renderer.fence && g_renderer.DestroyFence)
            g_renderer.DestroyFence(g_renderer.device, g_renderer.fence, NULL);
        if (g_renderer.pipeline) g_renderer.DestroyPipeline(g_renderer.device, g_renderer.pipeline, NULL);
        if (g_renderer.pipeline_layout) g_renderer.DestroyPipelineLayout(g_renderer.device, g_renderer.pipeline_layout, NULL);
        if (g_renderer.descriptor_pool) g_renderer.DestroyDescriptorPool(g_renderer.device, g_renderer.descriptor_pool, NULL);
        if (g_renderer.descriptor_layout) g_renderer.DestroyDescriptorSetLayout(g_renderer.device, g_renderer.descriptor_layout, NULL);
        if (g_renderer.command_pool) g_renderer.DestroyCommandPool(g_renderer.device, g_renderer.command_pool, NULL);
    }
    memset(&g_renderer, 0, sizeof(g_renderer));
}

static int read_shader(uint32_t **code, size_t *size)
{
    const char *path = getenv("BEER_VULKAN_COMPOSITOR_SPV");
    char executable_relative[4096];
    if (!path || !*path) {
        path = "renderer/vulkan/shaders/fullscreen_composite.comp.spv";
        FILE *probe = fopen(path, "rb");
        if (probe) {
            fclose(probe);
        } else {
            ssize_t length = readlink("/proc/self/exe", executable_relative,
                                      sizeof(executable_relative) - 1);
            if (length > 0) {
                executable_relative[length] = '\0';
                char *separator = strrchr(executable_relative, '/');
                if (separator) {
                    separator[1] = '\0';
                    strncat(executable_relative,
                            "renderer/vulkan/shaders/fullscreen_composite.comp.spv",
                            sizeof(executable_relative) - strlen(executable_relative) - 1);
                    path = executable_relative;
                }
            }
        }
    }
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "[D3D11 VULKAN] cannot open SPIR-V %s\n", path);
        return 0;
    }
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return 0; }
    long length = ftell(file);
    if (length <= 0 || (length & 3) || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }
    *code = malloc((size_t)length);
    if (!*code) { fclose(file); return 0; }
    *size = (size_t)length;
    int ok = fread(*code, 1, *size, file) == *size;
    fclose(file);
    if (!ok) { free(*code); *code = NULL; }
    return ok;
}

static int initialize(void)
{
    const char *failure_stage = "startup";
    if (g_renderer.initialized) return 1;
    if (g_renderer.unavailable) return 0;
    failure_stage = "load Vulkan library";
    if (!beer_vulkan_context_initialize()) goto fail;
    g_renderer.instance = beer_vulkan_instance();
    g_renderer.physical_device = beer_vulkan_physical_device();
    g_renderer.device = beer_vulkan_device();
    g_renderer.queue = beer_vulkan_queue();
    g_renderer.queue_family = beer_vulkan_queue_family();
    g_renderer.GetInstanceProcAddr = beer_vulkan_get_instance_proc_addr();
    g_renderer.GetDeviceProcAddr = beer_vulkan_get_device_proc_addr();
    if (!g_renderer.instance || !g_renderer.physical_device ||
        !g_renderer.device || !g_renderer.queue ||
        !g_renderer.GetInstanceProcAddr || !g_renderer.GetDeviceProcAddr)
        goto fail;
    LOAD_INSTANCE(GetPhysicalDeviceMemoryProperties);

    LOAD_DEVICE(DestroyDevice); LOAD_DEVICE(GetDeviceQueue);
    LOAD_DEVICE(CreateCommandPool); LOAD_DEVICE(DestroyCommandPool);
    LOAD_DEVICE(AllocateCommandBuffers); LOAD_DEVICE(ResetCommandBuffer);
    LOAD_DEVICE(BeginCommandBuffer); LOAD_DEVICE(EndCommandBuffer);
    LOAD_DEVICE(CreateDescriptorSetLayout); LOAD_DEVICE(DestroyDescriptorSetLayout);
    LOAD_DEVICE(CreateDescriptorPool); LOAD_DEVICE(DestroyDescriptorPool);
    LOAD_DEVICE(AllocateDescriptorSets); LOAD_DEVICE(UpdateDescriptorSets);
    LOAD_DEVICE(CreateShaderModule); LOAD_DEVICE(DestroyShaderModule);
    LOAD_DEVICE(CreatePipelineLayout); LOAD_DEVICE(DestroyPipelineLayout);
    LOAD_DEVICE(CreateComputePipelines); LOAD_DEVICE(DestroyPipeline);
    LOAD_DEVICE(CreateBuffer); LOAD_DEVICE(DestroyBuffer);
    LOAD_DEVICE(GetBufferMemoryRequirements); LOAD_DEVICE(AllocateMemory);
    LOAD_DEVICE(FreeMemory); LOAD_DEVICE(BindBufferMemory);
    LOAD_DEVICE(MapMemory); LOAD_DEVICE(UnmapMemory);
    LOAD_DEVICE(CmdBindPipeline); LOAD_DEVICE(CmdBindDescriptorSets);
    LOAD_DEVICE(CmdPushConstants); LOAD_DEVICE(CmdDispatch);
    LOAD_DEVICE(CmdPipelineBarrier);
    LOAD_DEVICE(QueueSubmit); LOAD_DEVICE(CreateFence); LOAD_DEVICE(DestroyFence);
    LOAD_DEVICE(ResetFences); LOAD_DEVICE(WaitForFences); LOAD_DEVICE(DeviceWaitIdle);
    g_renderer.GetDeviceQueue(g_renderer.device, g_renderer.queue_family, 0, &g_renderer.queue);

    failure_stage = "create command resources";
    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = g_renderer.queue_family
    };
    if (g_renderer.CreateCommandPool(g_renderer.device, &pool_info, NULL, &g_renderer.command_pool) != VK_SUCCESS)
        goto fail;
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = g_renderer.command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };
    if (g_renderer.AllocateCommandBuffers(g_renderer.device, &command_info, &g_renderer.command_buffer) != VK_SUCCESS)
        goto fail;
    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT
    };
    if (g_renderer.CreateFence(g_renderer.device, &fence_info, NULL,
                               &g_renderer.fence) != VK_SUCCESS)
        goto fail;

    failure_stage = "create descriptors";
    VkDescriptorSetLayoutBinding bindings[3];
    memset(bindings, 0, sizeof(bindings));
    for (uint32_t i = 0; i < 3; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 3,
        .pBindings = bindings
    };
    if (g_renderer.CreateDescriptorSetLayout(g_renderer.device, &layout_info, NULL, &g_renderer.descriptor_layout) != VK_SUCCESS)
        goto fail;
    VkPushConstantRange push_range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = 8
    };
    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &g_renderer.descriptor_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range
    };
    if (g_renderer.CreatePipelineLayout(g_renderer.device, &pipeline_layout_info, NULL, &g_renderer.pipeline_layout) != VK_SUCCESS)
        goto fail;

    failure_stage = "load compositor SPIR-V";
    uint32_t *code = NULL; size_t code_size = 0;
    if (!read_shader(&code, &code_size)) goto fail;
    VkShaderModuleCreateInfo shader_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = code_size,
        .pCode = code
    };
    VkShaderModule module = VK_NULL_HANDLE;
    VkResult shader_result = g_renderer.CreateShaderModule(g_renderer.device, &shader_info, NULL, &module);
    free(code);
    if (shader_result != VK_SUCCESS) goto fail;
    failure_stage = "create compute pipeline";
    VkPipelineShaderStageCreateInfo stage = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = module,
        .pName = "main"
    };
    VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = stage,
        .layout = g_renderer.pipeline_layout
    };
    VkResult pipeline_result = g_renderer.CreateComputePipelines(
        g_renderer.device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &g_renderer.pipeline);
    g_renderer.DestroyShaderModule(g_renderer.device, module, NULL);
    if (pipeline_result != VK_SUCCESS) goto fail;

    VkDescriptorPoolSize pool_size = {
        .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 3
    };
    VkDescriptorPoolCreateInfo pool_create = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size
    };
    if (g_renderer.CreateDescriptorPool(g_renderer.device, &pool_create, NULL, &g_renderer.descriptor_pool) != VK_SUCCESS)
        goto fail;
    VkDescriptorSetAllocateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = g_renderer.descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &g_renderer.descriptor_layout
    };
    if (g_renderer.AllocateDescriptorSets(g_renderer.device, &set_info, &g_renderer.descriptor_set) != VK_SUCCESS)
        goto fail;

    g_renderer.initialized = 1;
    fprintf(stderr, "[D3D11 VULKAN] compute renderer initialized\n");
    return 1;
fail:
    fprintf(stderr, "[D3D11 VULKAN] compute renderer unavailable at %s\n",
            failure_stage);
    vulkan_renderer_destroy();
    g_renderer.unavailable = 1;
    return 0;
}

static int find_memory_type(uint32_t bits, VkMemoryPropertyFlags required, uint32_t *index)
{
    VkPhysicalDeviceMemoryProperties properties;
    g_renderer.GetPhysicalDeviceMemoryProperties(g_renderer.physical_device, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) &&
            (properties.memoryTypes[i].propertyFlags & required) == required) {
            *index = i;
            return 1;
        }
    }
    return 0;
}

static int allocate_output_buffer(VkDeviceSize capacity, VkBuffer *buffer,
                                  VkDeviceMemory *memory, uint8_t **map)
{
    VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = capacity,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    if (g_renderer.CreateBuffer(g_renderer.device, &info, NULL, buffer) != VK_SUCCESS)
        return 0;
    VkMemoryRequirements requirements;
    g_renderer.GetBufferMemoryRequirements(g_renderer.device, *buffer, &requirements);
    uint32_t memory_type;
    if (!find_memory_type(requirements.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &memory_type))
        return 0;
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type
    };
    return g_renderer.AllocateMemory(g_renderer.device, &allocation, NULL, memory) == VK_SUCCESS &&
        g_renderer.BindBufferMemory(g_renderer.device, *buffer, *memory, 0) == VK_SUCCESS &&
        g_renderer.MapMemory(g_renderer.device, *memory, 0, capacity, 0,
                             (void **)map) == VK_SUCCESS;
}

static CompositorMirror *acquire_output_mirror(const void *resource,
                                                uint8_t *cpu_pixels,
                                                uint64_t serial,
                                                VkDeviceSize bytes)
{
    CompositorMirror *candidate = NULL;
    for (uint32_t i = 0; i < COMPOSITOR_MIRROR_COUNT; ++i) {
        CompositorMirror *mirror = &g_renderer.output_mirrors[i];
        if (mirror->resource == resource && mirror->bytes == bytes) {
            candidate = mirror;
            break;
        }
        if (!candidate || !mirror->resource || mirror->last_use < candidate->last_use)
            candidate = mirror;
    }
    if (!candidate) return NULL;
    if (candidate->resource && candidate->resource != resource) {
        writeback_output_mirror(candidate);
        candidate->content_valid = 0;
    }
    if (candidate->bytes != bytes) candidate->content_valid = 0;
    if (candidate->capacity < bytes) {
        writeback_output_mirror(candidate);
        candidate->content_valid = 0;
        if (candidate->map)
            g_renderer.UnmapMemory(g_renderer.device, candidate->memory);
        if (candidate->buffer)
            g_renderer.DestroyBuffer(g_renderer.device, candidate->buffer, NULL);
        if (candidate->memory)
            g_renderer.FreeMemory(g_renderer.device, candidate->memory, NULL);
        memset(candidate, 0, sizeof(*candidate));
        if (!allocate_output_buffer(bytes, &candidate->buffer,
                                    &candidate->memory, &candidate->map))
            return NULL;
        candidate->capacity = bytes;
    }
    candidate->resource = resource;
    candidate->cpu_pixels = cpu_pixels;
    candidate->bytes = bytes;
    /* Claiming a slot for an upcoming composite does not produce content.
     * Publishing `serial` here would let another worker's lookup match this
     * slot and read the previous frame's pixels as if they were the result
     * of the dispatch that has not run yet. The serial and validity flag are
     * published together only after the submission succeeds. */
    BeerTargetMirrorState claim = compositor_mirror_state(candidate);
    candidate->content_valid = beer_target_mirror_claim_valid(
        &claim, resource, serial, (size_t)bytes);
    candidate->last_use = ++g_renderer.output_use_serial;
    return candidate;
}

static int ensure_buffers(VkDeviceSize required)
{
    if (g_renderer.capacity >= required) return 1;
    destroy_buffers();
    for (uint32_t i = 0; i < 2; ++i) {
        VkBufferCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = required,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE
        };
        if (g_renderer.CreateBuffer(g_renderer.device, &info, NULL, &g_renderer.buffers[i]) != VK_SUCCESS)
            return 0;
        VkMemoryRequirements memory_requirements;
        g_renderer.GetBufferMemoryRequirements(g_renderer.device, g_renderer.buffers[i], &memory_requirements);
        uint32_t memory_type;
        if (!find_memory_type(memory_requirements.memoryTypeBits,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              &memory_type)) return 0;
        VkMemoryAllocateInfo allocation = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = memory_requirements.size,
            .memoryTypeIndex = memory_type
        };
        if (g_renderer.AllocateMemory(g_renderer.device, &allocation, NULL, &g_renderer.memories[i]) != VK_SUCCESS ||
            g_renderer.BindBufferMemory(g_renderer.device, g_renderer.buffers[i], g_renderer.memories[i], 0) != VK_SUCCESS ||
            g_renderer.MapMemory(g_renderer.device, g_renderer.memories[i], 0, required, 0, (void **)&g_renderer.maps[i]) != VK_SUCCESS)
            return 0;
    }
    g_renderer.capacity = required;
    VkDescriptorBufferInfo buffer_info[2];
    VkWriteDescriptorSet writes[2];
    memset(buffer_info, 0, sizeof(buffer_info));
    memset(writes, 0, sizeof(writes));
    for (uint32_t i = 0; i < 2; ++i) {
        buffer_info[i].buffer = g_renderer.buffers[i];
        buffer_info[i].range = required;
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = g_renderer.descriptor_set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &buffer_info[i];
    }
    g_renderer.UpdateDescriptorSets(g_renderer.device, 2, writes, 0, NULL);
    return 1;
}

static VkBuffer select_compositor_input(const void *resource, uint64_t serial,
                                        const uint8_t *cpu_pixels,
                                        uint8_t *staging, VkBuffer staging_buffer,
                                        size_t bytes)
{
    uint64_t count;
    const char *source;
    uint64_t shared_handle = 0;
    VkBuffer selected = staging_buffer;
    for (uint32_t i = 0; i < COMPOSITOR_MIRROR_COUNT; ++i) {
        CompositorMirror *mirror = &g_renderer.output_mirrors[i];
        BeerTargetMirrorState state = compositor_mirror_state(mirror);
        if (beer_target_mirror_matches(&state, resource, serial, bytes) &&
            mirror->buffer) {
            selected = mirror->buffer;
            mirror->last_use = ++g_renderer.output_use_serial;
            count = ++g_compositor_bridge_copies;
            source = "compositor-mirror-direct";
            goto selected_input;
        }
    }
    if (vulkan_indexed_renderer_get_target_buffer(
            resource, serial, bytes, &shared_handle)) {
        /* All stages share one VkDevice. Bind the indexed target buffer
         * directly instead of copying a full 1080p frame through host memory. */
        selected = (VkBuffer)(uintptr_t)shared_handle;
        count = ++g_indexed_bridge_copies;
        source = "indexed-mirror-direct";
    } else {
        memcpy(staging, cpu_pixels, bytes);
        count = ++g_cpu_input_copies;
        source = "cpu";
    }
selected_input:
    if (getenv("BEER_RENDER_DIAGNOSTICS") &&
        (count <= 4 || (count % 256u) == 0))
        fprintf(stderr, "[D3D11 VULKAN] compositor input source=%s count=%llu "
                "serial=%llu bytes=%zu\n", source,
                (unsigned long long)count, (unsigned long long)serial, bytes);
    return selected;
}

int vulkan_renderer_composite_rgba8(const void *source_resource,
                                    uint64_t source_serial,
                                    const uint8_t *source,
                                    const void *overlay_resource,
                                    uint64_t overlay_serial,
                                    const uint8_t *overlay,
                                    const void *target_resource,
                                    uint64_t target_serial,
                                    uint8_t *target,
                                    uint32_t width,
                                    uint32_t height,
                                    uint32_t mode)
{
    if (!source || !source_resource || !target || !target_resource || !width || !height ||
        (mode != 1 && mode != 2) || (mode == 1 && !overlay) ||
        width > UINT32_MAX / height)
        return 0;

    /* The compositor is reached from multiple guest worker contexts but uses
     * one Vulkan command buffer and one mapped staging set. Keep each upload,
     * dispatch and readback atomic so frame inputs cannot be mixed. */
    pthread_mutex_lock(&g_renderer_lock);
    int result = 0;
    uint32_t pixel_count = width * height;
    VkDeviceSize bytes = (VkDeviceSize)pixel_count * 4;
    if (!initialize() || !wait_for_pending_submission() || !ensure_buffers(bytes))
        goto done;
    CompositorMirror *output_mirror = acquire_output_mirror(
        target_resource, target, target_serial, bytes);
    if (!output_mirror) goto done;
    VkBuffer source_buffer = select_compositor_input(
        source_resource, source_serial, source, g_renderer.maps[0],
        g_renderer.buffers[0], (size_t)bytes);
    VkBuffer overlay_buffer = g_renderer.buffers[1];
    if (overlay)
        overlay_buffer = select_compositor_input(
            overlay_resource, overlay_serial, overlay, g_renderer.maps[1],
            g_renderer.buffers[1], (size_t)bytes);
    else
        memset(g_renderer.maps[1], 0, (size_t)bytes);

    VkDescriptorBufferInfo input_infos[2] = {
        { .buffer = source_buffer, .range = bytes },
        { .buffer = overlay_buffer, .range = bytes }
    };
    VkWriteDescriptorSet input_writes[2];
    memset(input_writes, 0, sizeof(input_writes));
    for (uint32_t i = 0; i < 2; ++i) {
        input_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        input_writes[i].dstSet = g_renderer.descriptor_set;
        input_writes[i].dstBinding = i;
        input_writes[i].descriptorCount = 1;
        input_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        input_writes[i].pBufferInfo = &input_infos[i];
    }
    VkDescriptorBufferInfo output_info = {
        .buffer = output_mirror->buffer,
        .range = bytes
    };
    VkWriteDescriptorSet output_write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = g_renderer.descriptor_set,
        .dstBinding = 2,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &output_info
    };
    g_renderer.UpdateDescriptorSets(g_renderer.device, 2, input_writes, 0, NULL);
    g_renderer.UpdateDescriptorSets(g_renderer.device, 1, &output_write, 0, NULL);

    g_renderer.ResetCommandBuffer(g_renderer.command_buffer, 0);
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    if (g_renderer.BeginCommandBuffer(g_renderer.command_buffer, &begin) != VK_SUCCESS)
        goto done;
    /* Indexed targets and earlier compositor outputs are bound directly on
     * the shared queue. Establish shader-write to shader-read visibility, and
     * publish host-coherent writes for CPU-backed staging inputs. */
    VkMemoryBarrier inputs_ready = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
    };
    g_renderer.CmdPipelineBarrier(g_renderer.command_buffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
        1, &inputs_ready, 0, NULL, 0, NULL);
    g_renderer.CmdBindPipeline(g_renderer.command_buffer,
                               VK_PIPELINE_BIND_POINT_COMPUTE,
                               g_renderer.pipeline);
    g_renderer.CmdBindDescriptorSets(g_renderer.command_buffer,
                                     VK_PIPELINE_BIND_POINT_COMPUTE,
                                     g_renderer.pipeline_layout, 0, 1,
                                     &g_renderer.descriptor_set, 0, NULL);
    uint32_t parameters[2] = { pixel_count, mode };
    g_renderer.CmdPushConstants(g_renderer.command_buffer,
                                g_renderer.pipeline_layout,
                                VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                sizeof(parameters), parameters);
    g_renderer.CmdDispatch(g_renderer.command_buffer, (pixel_count + 255) / 256, 1, 1);
    if (g_renderer.EndCommandBuffer(g_renderer.command_buffer) != VK_SUCCESS)
        goto done;
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &g_renderer.command_buffer
    };
    if (g_renderer.ResetFences(g_renderer.device, 1, &g_renderer.fence) != VK_SUCCESS)
        goto done;
    beer_vulkan_queue_lock();
    VkResult submit_result = g_renderer.QueueSubmit(
        g_renderer.queue, 1, &submit, g_renderer.fence);
    beer_vulkan_queue_unlock();
    if (submit_result != VK_SUCCESS) goto done;
    g_renderer.submission_pending = 1;
    output_mirror->serial = target_serial;
    output_mirror->content_valid = 1;
    output_mirror->gpu_dirty = 1;
    output_mirror->last_use = ++g_renderer.output_use_serial;
    static uint32_t executions;
    ++executions;
    if (getenv("BEER_RENDER_DIAGNOSTICS") &&
        (executions <= 8 || executions % 64 == 0))
        fprintf(stderr, "[D3D11 VULKAN] fullscreen compute execution=%u mode=%u %ux%u\n",
                executions, mode, width, height);
    result = 1;
done:
    pthread_mutex_unlock(&g_renderer_lock);
    return result;
}

int vulkan_renderer_sync_target(const void *resource, uint64_t serial,
                                uint8_t *pixels, size_t bytes)
{
    if (!resource || !pixels || !bytes) return 0;
    pthread_mutex_lock(&g_renderer_lock);
    if (!wait_for_pending_submission()) {
        pthread_mutex_unlock(&g_renderer_lock);
        return 0;
    }
    int found = 0;
    for (uint32_t i = 0; i < COMPOSITOR_MIRROR_COUNT; ++i) {
        CompositorMirror *mirror = &g_renderer.output_mirrors[i];
        BeerTargetMirrorState state = compositor_mirror_state(mirror);
        if (!beer_target_mirror_matches(&state, resource, serial, bytes))
            continue;
        if (mirror->gpu_dirty) {
            memcpy(pixels, mirror->map, bytes);
            mirror->cpu_pixels = pixels;
            mirror->gpu_dirty = 0;
        }
        mirror->last_use = ++g_renderer.output_use_serial;
        found = 1;
        break;
    }
    pthread_mutex_unlock(&g_renderer_lock);
    return found;
}

int vulkan_renderer_copy_target(const void *resource, uint64_t serial,
                                uint8_t *destination, size_t bytes)
{
    if (!resource || !destination || !bytes) return 0;
    pthread_mutex_lock(&g_renderer_lock);
    if (!wait_for_pending_submission()) {
        pthread_mutex_unlock(&g_renderer_lock);
        return 0;
    }
    int found = 0;
    for (uint32_t i = 0; i < COMPOSITOR_MIRROR_COUNT; ++i) {
        CompositorMirror *mirror = &g_renderer.output_mirrors[i];
        BeerTargetMirrorState state = compositor_mirror_state(mirror);
        if (!beer_target_mirror_matches(&state, resource, serial, bytes))
            continue;
        memcpy(destination, mirror->map, bytes);
        mirror->last_use = ++g_renderer.output_use_serial;
        found = 1;
        break;
    }
    pthread_mutex_unlock(&g_renderer_lock);
    return found;
}

int vulkan_renderer_get_target_buffer(const void *resource, uint64_t serial,
                                      size_t bytes, uint64_t *buffer_handle)
{
    if (!resource || !bytes || !buffer_handle) return 0;
    pthread_mutex_lock(&g_renderer_lock);
    int found = 0;
    for (uint32_t i = 0; i < COMPOSITOR_MIRROR_COUNT; ++i) {
        CompositorMirror *mirror = &g_renderer.output_mirrors[i];
        BeerTargetMirrorState state = compositor_mirror_state(mirror);
        if (!beer_target_mirror_matches(&state, resource, serial, bytes) ||
            !mirror->buffer)
            continue;
        *buffer_handle = (uint64_t)(uintptr_t)mirror->buffer;
        mirror->last_use = ++g_renderer.output_use_serial;
        found = 1;
        break;
    }
    pthread_mutex_unlock(&g_renderer_lock);
    return found;
}

void vulkan_renderer_forget_resource(const void *resource)
{
    if (!resource) return;
    pthread_mutex_lock(&g_renderer_lock);
    if (!wait_for_pending_submission()) {
        pthread_mutex_unlock(&g_renderer_lock);
        return;
    }
    for (uint32_t i = 0; i < COMPOSITOR_MIRROR_COUNT; ++i) {
        CompositorMirror *mirror = &g_renderer.output_mirrors[i];
        if (mirror->resource != resource) continue;
        writeback_output_mirror(mirror);
        mirror->resource = NULL;
        mirror->cpu_pixels = NULL;
        mirror->serial = 0;
        mirror->bytes = 0;
        mirror->last_use = 0;
        mirror->gpu_dirty = 0;
        mirror->content_valid = 0;
    }
    pthread_mutex_unlock(&g_renderer_lock);
}
