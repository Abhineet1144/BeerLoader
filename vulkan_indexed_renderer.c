#define _GNU_SOURCE
#include "vulkan_indexed_renderer.h"

#include <dlfcn.h>
#include <float.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#define BUFFER_COUNT 5
#define TEXTURE_MIRROR_COUNT 8
#define TARGET_MIRROR_COUNT 4

typedef struct {
    const uint8_t *resource;
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
    size_t bytes;
    uint64_t serial;
    uint64_t last_use;
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint8_t *map;
    VkDeviceSize capacity;
    int gpu_dirty;
} TargetMirror;

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
} PushConstants;

_Static_assert(sizeof(PushConstants) == 140, "indexed Vulkan push constants must match GLSL");

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
    VkBuffer buffers[BUFFER_COUNT];
    VkDeviceMemory memories[BUFFER_COUNT];
    uint8_t *maps[BUFFER_COUNT];
    VkDeviceSize capacities[BUFFER_COUNT];
    TextureMirror texture_mirrors[TEXTURE_MIRROR_COUNT];
    TargetMirror target_mirrors[TARGET_MIRROR_COUNT];
    uint64_t texture_use_serial;
    uint64_t texture_uploads;
    uint64_t texture_cache_hits;
    uint64_t target_use_serial;
    uint64_t target_uploads;
    uint64_t target_syncs;
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
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkCreateFence CreateFence;
    PFN_vkDestroyFence DestroyFence;
    PFN_vkResetFences ResetFences;
    PFN_vkWaitForFences WaitForFences;
    PFN_vkDeviceWaitIdle DeviceWaitIdle;
} IndexedRenderer;

static IndexedRenderer g_indexed;
static pthread_mutex_t g_indexed_lock = PTHREAD_MUTEX_INITIALIZER;

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
        strncat(relative, "shaders/indexed_scaleform.comp.spv",
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

static void destroy_buffers(void)
{
    if (!g_indexed.device) return;
    if (g_indexed.DeviceWaitIdle) g_indexed.DeviceWaitIdle(g_indexed.device);
    for (uint32_t i = 0; i < BUFFER_COUNT; ++i) {
        if (g_indexed.maps[i] && g_indexed.UnmapMemory)
            g_indexed.UnmapMemory(g_indexed.device, g_indexed.memories[i]);
        if (g_indexed.buffers[i] && g_indexed.DestroyBuffer)
            g_indexed.DestroyBuffer(g_indexed.device, g_indexed.buffers[i], NULL);
        if (g_indexed.memories[i] && g_indexed.FreeMemory)
            g_indexed.FreeMemory(g_indexed.device, g_indexed.memories[i], NULL);
        g_indexed.maps[i] = NULL;
        g_indexed.buffers[i] = VK_NULL_HANDLE;
        g_indexed.memories[i] = VK_NULL_HANDLE;
        g_indexed.capacities[i] = 0;
    }
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
        if (g_indexed.fence && g_indexed.DestroyFence)
            g_indexed.DestroyFence(g_indexed.device, g_indexed.fence, NULL);
        if (g_indexed.pipeline) g_indexed.DestroyPipeline(g_indexed.device, g_indexed.pipeline, NULL);
        if (g_indexed.pipeline_layout) g_indexed.DestroyPipelineLayout(g_indexed.device, g_indexed.pipeline_layout, NULL);
        if (g_indexed.descriptor_pool) g_indexed.DestroyDescriptorPool(g_indexed.device, g_indexed.descriptor_pool, NULL);
        if (g_indexed.descriptor_layout) g_indexed.DestroyDescriptorSetLayout(g_indexed.device, g_indexed.descriptor_layout, NULL);
        if (g_indexed.command_pool) g_indexed.DestroyCommandPool(g_indexed.device, g_indexed.command_pool, NULL);
        g_indexed.DestroyDevice(g_indexed.device, NULL);
    }
    if (g_indexed.instance && g_indexed.DestroyInstance)
        g_indexed.DestroyInstance(g_indexed.instance, NULL);
    if (g_indexed.library) dlclose(g_indexed.library);
    memset(&g_indexed, 0, sizeof(g_indexed));
}

static int initialize(void)
{
    const char *stage_name = "startup";
    if (g_indexed.initialized) return 1;
    if (g_indexed.unavailable) return 0;
    g_indexed.library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!g_indexed.library) goto fail;
    g_indexed.GetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(g_indexed.library, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance create_instance = (PFN_vkCreateInstance)dlsym(g_indexed.library, "vkCreateInstance");
    if (!g_indexed.GetInstanceProcAddr || !create_instance) goto fail;

    stage_name = "instance";
    VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Beer indexed D3D11 renderer",
        .apiVersion = VK_API_VERSION_1_0
    };
    VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application
    };
    if (create_instance(&instance_info, NULL, &g_indexed.instance) != VK_SUCCESS) goto fail;
    LOAD_INSTANCE(DestroyInstance);
    LOAD_INSTANCE(EnumeratePhysicalDevices);
    LOAD_INSTANCE(GetPhysicalDeviceQueueFamilyProperties);
    LOAD_INSTANCE(GetPhysicalDeviceMemoryProperties);
    LOAD_INSTANCE(CreateDevice);
    g_indexed.GetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)
        g_indexed.GetInstanceProcAddr(g_indexed.instance, "vkGetDeviceProcAddr");
    if (!g_indexed.GetDeviceProcAddr) goto fail;

    stage_name = "physical device";
    uint32_t physical_count = 0;
    if (g_indexed.EnumeratePhysicalDevices(g_indexed.instance, &physical_count, NULL) != VK_SUCCESS || !physical_count)
        goto fail;
    VkPhysicalDevice *physical_devices = calloc(physical_count, sizeof(*physical_devices));
    if (!physical_devices) goto fail;
    g_indexed.EnumeratePhysicalDevices(g_indexed.instance, &physical_count, physical_devices);
    for (uint32_t p = 0; p < physical_count && !g_indexed.physical_device; ++p) {
        uint32_t queue_count = 0;
        g_indexed.GetPhysicalDeviceQueueFamilyProperties(physical_devices[p], &queue_count, NULL);
        VkQueueFamilyProperties *queues = calloc(queue_count, sizeof(*queues));
        if (!queues) continue;
        g_indexed.GetPhysicalDeviceQueueFamilyProperties(physical_devices[p], &queue_count, queues);
        for (uint32_t q = 0; q < queue_count; ++q) {
            if (queues[q].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                g_indexed.physical_device = physical_devices[p];
                g_indexed.queue_family = q;
                break;
            }
        }
        free(queues);
    }
    free(physical_devices);
    if (!g_indexed.physical_device) goto fail;

    stage_name = "device";
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = g_indexed.queue_family,
        .queueCount = 1,
        .pQueuePriorities = &priority
    };
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info
    };
    if (g_indexed.CreateDevice(g_indexed.physical_device, &device_info, NULL, &g_indexed.device) != VK_SUCCESS)
        goto fail;
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
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = g_indexed.command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };
    if (g_indexed.AllocateCommandBuffers(g_indexed.device, &command_info, &g_indexed.command_buffer) != VK_SUCCESS)
        goto fail;
    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
    };
    if (g_indexed.CreateFence(g_indexed.device, &fence_info, NULL,
                              &g_indexed.fence) != VK_SUCCESS)
        goto fail;

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
        .descriptorCount = BUFFER_COUNT
    };
    VkDescriptorPoolCreateInfo descriptor_pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size
    };
    if (g_indexed.CreateDescriptorPool(g_indexed.device, &descriptor_pool_info, NULL,
                                        &g_indexed.descriptor_pool) != VK_SUCCESS)
        goto fail;
    VkDescriptorSetAllocateInfo descriptor_set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = g_indexed.descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &g_indexed.descriptor_layout
    };
    if (g_indexed.AllocateDescriptorSets(g_indexed.device, &descriptor_set_info,
                                          &g_indexed.descriptor_set) != VK_SUCCESS)
        goto fail;

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
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
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
    candidate->bytes = draw->target_bytes;
    candidate->last_use = ++g_indexed.target_use_serial;
    return candidate;
}

static TextureMirror *acquire_texture_mirror(const BeerVulkanIndexedDraw *draw)
{
    TextureMirror *candidate = NULL;
    for (uint32_t i = 0; i < TEXTURE_MIRROR_COUNT; ++i) {
        TextureMirror *mirror = &g_indexed.texture_mirrors[i];
        if (mirror->resource == draw->texture && mirror->bytes == draw->texture_bytes &&
            mirror->width == draw->texture_width && mirror->height == draw->texture_height) {
            candidate = mirror;
            break;
        }
        if (!candidate || !mirror->resource || mirror->last_use < candidate->last_use)
            candidate = mirror;
    }
    if (!candidate) return NULL;
    if (candidate->capacity < draw->texture_bytes) {
        destroy_texture_mirror(candidate);
        if (!allocate_host_buffer(draw->texture_bytes, &candidate->buffer,
                                  &candidate->memory, &candidate->map)) {
            destroy_texture_mirror(candidate);
            return NULL;
        }
        candidate->capacity = draw->texture_bytes;
    }
    int content_changed = candidate->resource != draw->texture ||
        candidate->serial != draw->texture_serial ||
        candidate->bytes != draw->texture_bytes ||
        candidate->width != draw->texture_width ||
        candidate->height != draw->texture_height;
    if (content_changed) {
        memcpy(candidate->map, draw->texture, draw->texture_bytes);
        candidate->resource = draw->texture;
        candidate->serial = draw->texture_serial;
        candidate->bytes = draw->texture_bytes;
        candidate->width = draw->texture_width;
        candidate->height = draw->texture_height;
        ++g_indexed.texture_uploads;
        if (g_indexed.texture_uploads <= 4 || (g_indexed.texture_uploads % 64u) == 0)
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

static int ensure_buffers(const VkDeviceSize required[BUFFER_COUNT])
{
    int recreate = 0;
    for (uint32_t i = 0; i < BUFFER_COUNT; ++i)
        if (g_indexed.capacities[i] < required[i]) recreate = 1;
    if (!recreate) return 1;
    destroy_buffers();

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
                                   &g_indexed.buffers[i]) != VK_SUCCESS)
            return 0;
        VkMemoryRequirements memory_requirements;
        g_indexed.GetBufferMemoryRequirements(g_indexed.device, g_indexed.buffers[i],
                                               &memory_requirements);
        uint32_t memory_type;
        if (!find_memory_type(memory_requirements.memoryTypeBits, &memory_type)) return 0;
        VkMemoryAllocateInfo allocation_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = memory_requirements.size,
            .memoryTypeIndex = memory_type
        };
        if (g_indexed.AllocateMemory(g_indexed.device, &allocation_info, NULL,
                                     &g_indexed.memories[i]) != VK_SUCCESS ||
            g_indexed.BindBufferMemory(g_indexed.device, g_indexed.buffers[i],
                                       g_indexed.memories[i], 0) != VK_SUCCESS ||
            g_indexed.MapMemory(g_indexed.device, g_indexed.memories[i], 0,
                                capacity, 0, (void **)&g_indexed.maps[i]) != VK_SUCCESS)
            return 0;
        g_indexed.capacities[i] = capacity;
        infos[i].buffer = g_indexed.buffers[i];
        infos[i].range = capacity;
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = g_indexed.descriptor_set;
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
    if (!draw || !draw->vertices || !draw->indices || !draw->constants ||
        !draw->target || !draw->width || !draw->height || !draw->index_count ||
        draw->mode < 1 || draw->mode > 5 ||
        (draw->vertex_stride != 16 && draw->vertex_stride != 12 &&
         draw->vertex_stride != 20) ||
        draw->target_bytes < (size_t)draw->width * draw->height * 4 ||
        (draw->mode >= 3 && (!draw->texture || !draw->texture_width ||
                            !draw->texture_height ||
                            draw->texture_bytes < (size_t)draw->texture_width *
                                draw->texture_height * (draw->mode == 5 ? 1u : 4u))))
        return 0;

    /* D3D11 deferred command lists can execute concurrently on Beer worker
     * threads. This backend owns one command buffer, fence, descriptor set and
     * mapped staging set, so serialize a complete dispatch/readback transaction.
     * Without this, draw constants and targets can be overwritten by another
     * splash draw while the GPU is consuming them, producing cycling colors. */
    pthread_mutex_lock(&g_indexed_lock);
    int result = 0;
    VkDeviceSize required[BUFFER_COUNT] = {
        draw->vertex_bytes, draw->index_bytes, draw->constant_bytes, 4, 4
    };
    if (!initialize() || !ensure_buffers(required)) goto done;
    memcpy(g_indexed.maps[0], draw->vertices, draw->vertex_bytes);
    memcpy(g_indexed.maps[1], draw->indices, draw->index_bytes);
    memcpy(g_indexed.maps[2], draw->constants, draw->constant_bytes);

    TargetMirror *target_mirror = acquire_target_mirror(draw);
    if (!target_mirror) goto done;
    int upload_target = target_mirror->serial != draw->target_input_serial;
    if (upload_target) {
        memcpy(target_mirror->map, draw->target, draw->target_bytes);
        target_mirror->serial = draw->target_input_serial;
        target_mirror->gpu_dirty = 0;
        ++g_indexed.target_uploads;
    }
    VkDescriptorBufferInfo target_info = {
        .buffer = target_mirror->buffer,
        .range = draw->target_bytes
    };
    VkWriteDescriptorSet target_write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = g_indexed.descriptor_set,
        .dstBinding = 3,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &target_info
    };
    g_indexed.UpdateDescriptorSets(g_indexed.device, 1, &target_write, 0, NULL);

    VkBuffer texture_buffer = g_indexed.buffers[4];
    VkDeviceSize texture_range = 4;
    if (draw->mode >= 3) {
        TextureMirror *mirror = acquire_texture_mirror(draw);
        if (!mirror) goto done;
        texture_buffer = mirror->buffer;
        texture_range = draw->texture_bytes;
    } else {
        memset(g_indexed.maps[4], 0, 4);
    }
    VkDescriptorBufferInfo texture_info = {
        .buffer = texture_buffer,
        .range = texture_range
    };
    VkWriteDescriptorSet texture_write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = g_indexed.descriptor_set,
        .dstBinding = 4,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &texture_info
    };
    g_indexed.UpdateDescriptorSets(g_indexed.device, 1, &texture_write, 0, NULL);

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
                              draw->mode == 5u) ? 8u : 0u;
    int valid_bounds = constant_count >= transform_row + 8u;
    for (uint32_t first = 0; valid_bounds && first < draw->index_count; ++first) {
        int64_t vertex_index = (int64_t)index_data[first] + draw->base_vertex;
        size_t position_offset = draw->vertex_offset;
        if (vertex_index < 0 ||
            (size_t)vertex_index > (SIZE_MAX - position_offset) / draw->vertex_stride) {
            valid_bounds = 0;
            break;
        }
        position_offset += (size_t)vertex_index * draw->vertex_stride +
            (draw->vertex_stride == 16u ? 8u : draw->vertex_stride == 20u ? 12u : 4u);
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
        result = 1;
        goto done;
    }
    push.dispatch_origin_x = (uint32_t)left;
    push.dispatch_origin_y = (uint32_t)top;
    push.dispatch_width = (uint32_t)(right - left);
    push.dispatch_pixels = push.dispatch_width * (uint32_t)(bottom - top);

    g_indexed.ResetCommandBuffer(g_indexed.command_buffer, 0);
    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    if (g_indexed.BeginCommandBuffer(g_indexed.command_buffer, &begin_info) != VK_SUCCESS)
        goto done;
    g_indexed.CmdBindPipeline(g_indexed.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                              g_indexed.pipeline);
    g_indexed.CmdBindDescriptorSets(g_indexed.command_buffer,
                                    VK_PIPELINE_BIND_POINT_COMPUTE,
                                    g_indexed.pipeline_layout, 0, 1,
                                    &g_indexed.descriptor_set, 0, NULL);
    g_indexed.CmdPushConstants(g_indexed.command_buffer, g_indexed.pipeline_layout,
                               VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    g_indexed.CmdDispatch(g_indexed.command_buffer,
                          (push.dispatch_pixels + 255u) / 256u, 1, 1);
    if (g_indexed.EndCommandBuffer(g_indexed.command_buffer) != VK_SUCCESS) goto done;
    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &g_indexed.command_buffer
    };
    if (g_indexed.ResetFences(g_indexed.device, 1, &g_indexed.fence) != VK_SUCCESS ||
        g_indexed.QueueSubmit(g_indexed.queue, 1, &submit_info, g_indexed.fence) != VK_SUCCESS ||
        g_indexed.WaitForFences(g_indexed.device, 1, &g_indexed.fence, VK_TRUE,
                                UINT64_MAX) != VK_SUCCESS)
        goto done;
    target_mirror->serial = draw->target_output_serial;
    target_mirror->gpu_dirty = 1;
    static uint32_t executions;
    static uint32_t mode_executions[6];
    ++executions;
    uint32_t mode_execution = ++mode_executions[draw->mode];
    if (mode_execution <= 4 || mode_execution % 64 == 0)
        fprintf(stderr, "[D3D11 VULKAN] indexed execution=%u mode=%u "
                "mode-execution=%u indices=%u %ux%u\n",
                executions, draw->mode, mode_execution, draw->index_count,
                draw->width, draw->height);
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
    for (uint32_t i = 0; i < TARGET_MIRROR_COUNT; ++i) {
        TargetMirror *mirror = &g_indexed.target_mirrors[i];
        if (mirror->resource != resource || mirror->bytes != bytes) continue;
        if (mirror->serial != serial) break;
        if (mirror->gpu_dirty) {
            memcpy(pixels, mirror->map, bytes);
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
    for (uint32_t i = 0; i < TARGET_MIRROR_COUNT; ++i) {
        TargetMirror *mirror = &g_indexed.target_mirrors[i];
        if (mirror->resource != resource || mirror->bytes != bytes) continue;
        if (mirror->serial != serial) break;
        memcpy(destination, mirror->map, bytes);
        mirror->last_use = ++g_indexed.target_use_serial;
        found = 1;
        break;
    }
    pthread_mutex_unlock(&g_indexed_lock);
    return found;
}

void vulkan_indexed_renderer_forget_resource(const void *resource)
{
    if (!resource) return;
    pthread_mutex_lock(&g_indexed_lock);
    for (uint32_t i = 0; i < TARGET_MIRROR_COUNT; ++i) {
        TargetMirror *mirror = &g_indexed.target_mirrors[i];
        if (mirror->resource == resource) {
            mirror->resource = NULL;
            mirror->serial = 0;
            mirror->gpu_dirty = 0;
        }
    }
    pthread_mutex_unlock(&g_indexed_lock);
}
