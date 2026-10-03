#define _GNU_SOURCE
#include "vulkan_renderer.h"
#include "vulkan_indexed_renderer.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

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
    VkBuffer buffers[3];
    VkDeviceMemory memories[3];
    uint8_t *maps[3];
    VkDeviceSize capacity;
    const void *pending_resource;
    uint8_t *pending_pixels;
    uint64_t pending_serial;
    VkDeviceSize pending_bytes;
    int pending_dirty;
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
} VulkanRenderer;

static VulkanRenderer g_renderer;
static pthread_mutex_t g_renderer_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_indexed_bridge_copies;
static uint64_t g_compositor_bridge_copies;
static uint64_t g_cpu_input_copies;

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
    if (g_renderer.DeviceWaitIdle) g_renderer.DeviceWaitIdle(g_renderer.device);
    for (uint32_t i = 0; i < 3; ++i) {
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
        g_renderer.DestroyDevice(g_renderer.device, NULL);
    }
    if (g_renderer.instance && g_renderer.DestroyInstance)
        g_renderer.DestroyInstance(g_renderer.instance, NULL);
    if (g_renderer.library) dlclose(g_renderer.library);
    memset(&g_renderer, 0, sizeof(g_renderer));
}

static int read_shader(uint32_t **code, size_t *size)
{
    const char *path = getenv("BEER_VULKAN_COMPOSITOR_SPV");
    char executable_relative[4096];
    if (!path || !*path) {
        path = "shaders/fullscreen_composite.comp.spv";
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
                            "shaders/fullscreen_composite.comp.spv",
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
    g_renderer.library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!g_renderer.library) goto fail;
    g_renderer.GetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(
        g_renderer.library, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance create_instance = (PFN_vkCreateInstance)dlsym(
        g_renderer.library, "vkCreateInstance");
    if (!g_renderer.GetInstanceProcAddr || !create_instance) goto fail;

    failure_stage = "create Vulkan instance";
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Beer D3D11 Vulkan Renderer",
        .apiVersion = VK_API_VERSION_1_0
    };
    VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app
    };
    if (create_instance(&instance_info, NULL, &g_renderer.instance) != VK_SUCCESS)
        goto fail;
    LOAD_INSTANCE(DestroyInstance);
    LOAD_INSTANCE(EnumeratePhysicalDevices);
    LOAD_INSTANCE(GetPhysicalDeviceQueueFamilyProperties);
    LOAD_INSTANCE(GetPhysicalDeviceMemoryProperties);
    LOAD_INSTANCE(CreateDevice);
    g_renderer.GetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)
        g_renderer.GetInstanceProcAddr(g_renderer.instance, "vkGetDeviceProcAddr");
    if (!g_renderer.GetDeviceProcAddr) goto fail;

    failure_stage = "select compute device";
    uint32_t physical_count = 0;
    if (g_renderer.EnumeratePhysicalDevices(g_renderer.instance, &physical_count, NULL) != VK_SUCCESS || !physical_count)
        goto fail;
    VkPhysicalDevice *physical = calloc(physical_count, sizeof(*physical));
    if (!physical) goto fail;
    g_renderer.EnumeratePhysicalDevices(g_renderer.instance, &physical_count, physical);
    for (uint32_t p = 0; p < physical_count && !g_renderer.physical_device; ++p) {
        uint32_t count = 0;
        g_renderer.GetPhysicalDeviceQueueFamilyProperties(physical[p], &count, NULL);
        VkQueueFamilyProperties *properties = calloc(count, sizeof(*properties));
        if (!properties) continue;
        g_renderer.GetPhysicalDeviceQueueFamilyProperties(physical[p], &count, properties);
        for (uint32_t q = 0; q < count; ++q) {
            if (properties[q].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                g_renderer.physical_device = physical[p];
                g_renderer.queue_family = q;
                break;
            }
        }
        free(properties);
    }
    free(physical);
    if (!g_renderer.physical_device) goto fail;

    failure_stage = "create compute device";
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = g_renderer.queue_family,
        .queueCount = 1,
        .pQueuePriorities = &priority
    };
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info
    };
    if (g_renderer.CreateDevice(g_renderer.physical_device, &device_info, NULL, &g_renderer.device) != VK_SUCCESS)
        goto fail;

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

static int ensure_buffers(VkDeviceSize required)
{
    if (g_renderer.capacity >= required) return 1;
    destroy_buffers();
    for (uint32_t i = 0; i < 3; ++i) {
        VkBufferCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = required,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
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
    VkDescriptorBufferInfo buffer_info[3];
    VkWriteDescriptorSet writes[3];
    memset(buffer_info, 0, sizeof(buffer_info));
    memset(writes, 0, sizeof(writes));
    for (uint32_t i = 0; i < 3; ++i) {
        buffer_info[i].buffer = g_renderer.buffers[i];
        buffer_info[i].range = required;
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = g_renderer.descriptor_set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &buffer_info[i];
    }
    g_renderer.UpdateDescriptorSets(g_renderer.device, 3, writes, 0, NULL);
    return 1;
}

static void copy_compositor_input(const void *resource, uint64_t serial,
                                  const uint8_t *cpu_pixels,
                                  uint8_t *destination, size_t bytes)
{
    uint64_t count;
    const char *source;
    if (g_renderer.pending_resource == resource &&
        g_renderer.pending_serial == serial &&
        g_renderer.pending_bytes == bytes) {
        memcpy(destination, g_renderer.maps[2], bytes);
        count = ++g_compositor_bridge_copies;
        source = "compositor-mirror";
    } else if (vulkan_indexed_renderer_copy_target(resource, serial,
                                                    destination, bytes)) {
        count = ++g_indexed_bridge_copies;
        source = "indexed-mirror";
    } else {
        memcpy(destination, cpu_pixels, bytes);
        count = ++g_cpu_input_copies;
        source = "cpu";
    }
    if (count <= 4 || (count % 256u) == 0)
        fprintf(stderr, "[D3D11 VULKAN] compositor input source=%s count=%llu "
                "serial=%llu bytes=%zu\n", source,
                (unsigned long long)count, (unsigned long long)serial, bytes);
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
    if (!initialize() || !ensure_buffers(bytes)) goto done;
    copy_compositor_input(source_resource, source_serial, source,
                          g_renderer.maps[0], (size_t)bytes);
    if (overlay)
        copy_compositor_input(overlay_resource, overlay_serial, overlay,
                              g_renderer.maps[1], (size_t)bytes);
    else
        memset(g_renderer.maps[1], 0, (size_t)bytes);

    g_renderer.ResetCommandBuffer(g_renderer.command_buffer, 0);
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    if (g_renderer.BeginCommandBuffer(g_renderer.command_buffer, &begin) != VK_SUCCESS)
        goto done;
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
    if (g_renderer.ResetFences(g_renderer.device, 1, &g_renderer.fence) != VK_SUCCESS ||
        g_renderer.QueueSubmit(g_renderer.queue, 1, &submit, g_renderer.fence) != VK_SUCCESS ||
        g_renderer.WaitForFences(g_renderer.device, 1, &g_renderer.fence, VK_TRUE,
                                 UINT64_MAX) != VK_SUCCESS)
        goto done;
    g_renderer.pending_resource = target_resource;
    g_renderer.pending_pixels = target;
    g_renderer.pending_serial = target_serial;
    g_renderer.pending_bytes = bytes;
    g_renderer.pending_dirty = 1;
    static uint32_t executions;
    ++executions;
    if (executions <= 8 || executions % 64 == 0)
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
    int found = g_renderer.pending_resource == resource &&
        g_renderer.pending_serial == serial &&
        g_renderer.pending_bytes == bytes;
    if (found && g_renderer.pending_dirty) {
        memcpy(pixels, g_renderer.maps[2], bytes);
        g_renderer.pending_dirty = 0;
    }
    pthread_mutex_unlock(&g_renderer_lock);
    return found;
}

int vulkan_renderer_copy_target(const void *resource, uint64_t serial,
                                uint8_t *destination, size_t bytes)
{
    if (!resource || !destination || !bytes) return 0;
    pthread_mutex_lock(&g_renderer_lock);
    int found = g_renderer.pending_resource == resource &&
        g_renderer.pending_serial == serial &&
        g_renderer.pending_bytes == bytes;
    if (found) memcpy(destination, g_renderer.maps[2], bytes);
    pthread_mutex_unlock(&g_renderer_lock);
    return found;
}

void vulkan_renderer_forget_resource(const void *resource)
{
    if (!resource) return;
    pthread_mutex_lock(&g_renderer_lock);
    if (g_renderer.pending_resource == resource) {
        g_renderer.pending_resource = NULL;
        g_renderer.pending_pixels = NULL;
        g_renderer.pending_serial = 0;
        g_renderer.pending_bytes = 0;
        g_renderer.pending_dirty = 0;
    }
    pthread_mutex_unlock(&g_renderer_lock);
}
