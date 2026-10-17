#define _GNU_SOURCE
#include "vulkan_context.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    void *library;
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
    PFN_vkDestroyInstance DestroyInstance;
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
    PFN_vkGetPhysicalDeviceFeatures GetPhysicalDeviceFeatures;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceFeatures2KHR GetPhysicalDeviceFeatures2KHR;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties;
    VkPhysicalDeviceFeatures enabled_features;
    uint32_t max_sampler_anisotropy;
    int maintenance1;
    int robustness2;
    int pipeline_robustness;
    int sampler_filter_minmax;
    int device_lost;
    PFN_vkCreateDevice CreateDevice;
    PFN_vkDestroyDevice DestroyDevice;
    PFN_vkGetDeviceQueue GetDeviceQueue;
    PFN_vkDeviceWaitIdle DeviceWaitIdle;
    int initialized;
    int unavailable;
} BeerVulkanContext;

static BeerVulkanContext g_context;
static pthread_mutex_t g_context_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_queue_lock = PTHREAD_MUTEX_INITIALIZER;

/* Experiment (BEER_VULKAN_GPU_KEEPALIVE=<MiB per submit>): keep the GPU busy on
 * the shared queue so the driver does not drop to idle clocks between the
 * bridge's short synchronous submissions. */
static void *gpu_keepalive_thread(void *unused)
{
    (void)unused;
    const char *env = getenv("BEER_VULKAN_GPU_KEEPALIVE");
    VkDeviceSize bytes = (VkDeviceSize)(env ? strtoul(env, NULL, 10) : 64) <<
                         20;
    VkDevice device = g_context.device;
#define KA_LOAD(type, name) \
    PFN_vk##name name = (PFN_vk##name)g_context.GetDeviceProcAddr(device, "vk" #name); \
    (void)sizeof(type); if (!name) return NULL
    KA_LOAD(int, CreateBuffer); KA_LOAD(int, GetBufferMemoryRequirements);
    KA_LOAD(int, AllocateMemory); KA_LOAD(int, BindBufferMemory);
    KA_LOAD(int, CreateCommandPool); KA_LOAD(int, AllocateCommandBuffers);
    KA_LOAD(int, BeginCommandBuffer); KA_LOAD(int, EndCommandBuffer);
    KA_LOAD(int, CmdFillBuffer); KA_LOAD(int, CmdPipelineBarrier);
    KA_LOAD(int, CreateFence); KA_LOAD(int, ResetFences);
    KA_LOAD(int, WaitForFences); KA_LOAD(int, QueueSubmit);
#undef KA_LOAD
    PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)
        g_context.GetInstanceProcAddr(g_context.instance,
                                      "vkGetPhysicalDeviceMemoryProperties");
    if (!get_memory_properties) return NULL;
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    VkBuffer buffer;
    if (CreateBuffer(device, &buffer_info, NULL, &buffer) != VK_SUCCESS)
        return NULL;
    VkMemoryRequirements requirements;
    GetBufferMemoryRequirements(device, buffer, &requirements);
    VkPhysicalDeviceMemoryProperties properties;
    get_memory_properties(g_context.physical_device, &properties);
    int type = -1;
    for (uint32_t i = 0; i < properties.memoryTypeCount && type < 0; ++i)
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (properties.memoryTypes[i].propertyFlags &
             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            type = (int)i;
    if (type < 0) return NULL;
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = (uint32_t)type
    };
    VkDeviceMemory memory;
    if (AllocateMemory(device, &allocation, NULL, &memory) != VK_SUCCESS ||
        BindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS)
        return NULL;
    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = g_context.queue_family
    };
    VkCommandPool pool;
    if (CreateCommandPool(device, &pool_info, NULL, &pool) != VK_SUCCESS)
        return NULL;
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };
    VkCommandBuffer command;
    if (AllocateCommandBuffers(device, &command_info, &command) != VK_SUCCESS)
        return NULL;
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
    };
    BeginCommandBuffer(command, &begin);
    VkMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT
    };
    for (int i = 0; i < 4; ++i) {
        CmdFillBuffer(command, buffer, 0, bytes & ~(VkDeviceSize)3, 0x5a5a5a5au);
        CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0,
                           NULL, 0, NULL);
    }
    EndCommandBuffer(command);
    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
    };
    VkFence fence;
    if (CreateFence(device, &fence_info, NULL, &fence) != VK_SUCCESS)
        return NULL;
    fprintf(stderr, "[VULKAN] GPU keepalive running (%lu MiB x4 per submit)\n",
            (unsigned long)(bytes >> 20));
    for (;;) {
        VkSubmitInfo submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &command
        };
        ResetFences(device, 1, &fence);
        pthread_mutex_lock(&g_queue_lock);
        VkResult submitted = QueueSubmit(g_context.queue, 1, &submit, fence);
        pthread_mutex_unlock(&g_queue_lock);
        if (submitted != VK_SUCCESS) return NULL;
        WaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    }
}

#define LOAD_INSTANCE(name) do { \
    g_context.name = (PFN_vk##name)g_context.GetInstanceProcAddr( \
        g_context.instance, "vk" #name); \
    if (!g_context.name) goto fail; \
} while (0)
#define LOAD_DEVICE(name) do { \
    g_context.name = (PFN_vk##name)g_context.GetDeviceProcAddr( \
        g_context.device, "vk" #name); \
    if (!g_context.name) goto fail; \
} while (0)

int beer_vulkan_context_initialize(void)
{
    pthread_mutex_lock(&g_context_lock);
    if (g_context.initialized) {
        pthread_mutex_unlock(&g_context_lock);
        return 1;
    }
    if (g_context.unavailable) {
        pthread_mutex_unlock(&g_context_lock);
        return 0;
    }

    const char *stage = "Vulkan loader";
    g_context.library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!g_context.library) goto fail;
    g_context.GetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)
        dlsym(g_context.library, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance create_instance = (PFN_vkCreateInstance)
        dlsym(g_context.library, "vkCreateInstance");
    if (!g_context.GetInstanceProcAddr || !create_instance) goto fail;

    stage = "shared instance";
    const char *instance_extensions[] = {
        "VK_KHR_surface",
        "VK_KHR_xlib_surface",
        "VK_KHR_get_physical_device_properties2"
    };
    VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Beer",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "Beer D3D11 Vulkan",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_0
    };
    VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application,
        .enabledExtensionCount = 3,
        .ppEnabledExtensionNames = instance_extensions
    };
    if (create_instance(&instance_info, NULL, &g_context.instance) != VK_SUCCESS)
        goto fail;
    LOAD_INSTANCE(DestroyInstance);
    LOAD_INSTANCE(EnumeratePhysicalDevices);
    LOAD_INSTANCE(GetPhysicalDeviceQueueFamilyProperties);
    LOAD_INSTANCE(GetPhysicalDeviceFeatures);
    LOAD_INSTANCE(GetPhysicalDeviceProperties);
    LOAD_INSTANCE(EnumerateDeviceExtensionProperties);
    LOAD_INSTANCE(CreateDevice);
    g_context.GetPhysicalDeviceFeatures2KHR =
        (PFN_vkGetPhysicalDeviceFeatures2KHR)g_context.GetInstanceProcAddr(
            g_context.instance, "vkGetPhysicalDeviceFeatures2KHR");
    if (!g_context.GetPhysicalDeviceFeatures2KHR) goto fail;
    g_context.GetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)
        g_context.GetInstanceProcAddr(g_context.instance, "vkGetDeviceProcAddr");
    if (!g_context.GetDeviceProcAddr) goto fail;

    stage = "shared graphics/compute queue";
    uint32_t physical_count = 0;
    if (g_context.EnumeratePhysicalDevices(g_context.instance, &physical_count,
                                            NULL) != VK_SUCCESS || !physical_count)
        goto fail;
    VkPhysicalDevice *physical_devices = calloc(physical_count,
                                                 sizeof(*physical_devices));
    if (!physical_devices) goto fail;
    g_context.EnumeratePhysicalDevices(g_context.instance, &physical_count,
                                        physical_devices);
    for (uint32_t p = 0; p < physical_count && !g_context.physical_device; ++p) {
        uint32_t queue_count = 0;
        g_context.GetPhysicalDeviceQueueFamilyProperties(physical_devices[p],
                                                         &queue_count, NULL);
        VkQueueFamilyProperties *queues = calloc(queue_count, sizeof(*queues));
        if (!queues) continue;
        g_context.GetPhysicalDeviceQueueFamilyProperties(physical_devices[p],
                                                         &queue_count, queues);
        for (uint32_t q = 0; q < queue_count; ++q) {
            VkQueueFlags required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            if ((queues[q].queueFlags & required) == required) {
                g_context.physical_device = physical_devices[p];
                g_context.queue_family = q;
                break;
            }
        }
        free(queues);
    }
    free(physical_devices);
    if (!g_context.physical_device) goto fail;

    stage = "shared logical device";
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = g_context.queue_family,
        .queueCount = 1,
        .pQueuePriorities = &priority
    };
    /* VK_KHR_maintenance1 permits negative viewport heights, which graphics
     * execution uses to preserve D3D's upward-Y clip space. */
    const char *device_extensions[5] = { "VK_KHR_swapchain" };
    uint32_t device_extension_count = 1;
    {
        uint32_t available_count = 0;
        g_context.EnumerateDeviceExtensionProperties(
            g_context.physical_device, NULL, &available_count, NULL);
        VkExtensionProperties *available = available_count ?
            calloc(available_count, sizeof(*available)) : NULL;
        if (available &&
            g_context.EnumerateDeviceExtensionProperties(
                g_context.physical_device, NULL, &available_count,
                available) != VK_SUCCESS)
            available_count = 0;
        for (uint32_t i = 0; available && i < available_count; ++i) {
            if (!g_context.maintenance1 &&
                strcmp(available[i].extensionName,
                       "VK_KHR_maintenance1") == 0) {
                device_extensions[device_extension_count++] =
                    "VK_KHR_maintenance1";
                g_context.maintenance1 = 1;
            } else if (!g_context.robustness2 &&
                       strcmp(available[i].extensionName,
                              VK_EXT_ROBUSTNESS_2_EXTENSION_NAME) == 0) {
                device_extensions[device_extension_count++] =
                    VK_EXT_ROBUSTNESS_2_EXTENSION_NAME;
                g_context.robustness2 = 1;
            } else if (!g_context.pipeline_robustness &&
                       strcmp(available[i].extensionName,
                              VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME) == 0) {
                device_extensions[device_extension_count++] =
                    VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME;
                g_context.pipeline_robustness = 1;
            } else if (!g_context.sampler_filter_minmax &&
                       strcmp(available[i].extensionName,
                              VK_EXT_SAMPLER_FILTER_MINMAX_EXTENSION_NAME) == 0) {
                device_extensions[device_extension_count++] =
                    VK_EXT_SAMPLER_FILTER_MINMAX_EXTENSION_NAME;
                g_context.sampler_filter_minmax = 1;
            }
        }
        free(available);
    }
    /* Translated D3D shaders need typed storage-image access and wider scalar
     * types. Enable only the optional features the device reports; the
     * compute executor rejects any shader whose capabilities are not covered. */
    VkPhysicalDeviceFeatures supported;
    memset(&supported, 0, sizeof(supported));
    g_context.GetPhysicalDeviceFeatures(g_context.physical_device, &supported);
    VkPhysicalDeviceRobustness2FeaturesEXT robustness2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT
    };
    VkPhysicalDevicePipelineRobustnessFeaturesEXT pipeline_robustness = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT,
        .pNext = g_context.robustness2 ? &robustness2 : NULL
    };
    if (g_context.robustness2 || g_context.pipeline_robustness) {
        VkPhysicalDeviceFeatures2 features2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext = g_context.pipeline_robustness ?
                (void *)&pipeline_robustness : (void *)&robustness2
        };
        g_context.GetPhysicalDeviceFeatures2KHR(g_context.physical_device,
                                                &features2);
    }
    g_context.pipeline_robustness = g_context.pipeline_robustness &&
        pipeline_robustness.pipelineRobustness;
    /* D3D requires defined out-of-bounds buffer and image behavior. Core
     * robustness covers buffers, while robustness2 extends this to images and
     * tighter buffer bounds used by translated structured/typed views. */
    g_context.enabled_features.robustBufferAccess =
        supported.robustBufferAccess;
    g_context.enabled_features.samplerAnisotropy = supported.samplerAnisotropy;
    VkPhysicalDeviceProperties properties;
    memset(&properties, 0, sizeof(properties));
    g_context.GetPhysicalDeviceProperties(g_context.physical_device,
                                          &properties);
    g_context.max_sampler_anisotropy = supported.samplerAnisotropy ?
        (uint32_t)properties.limits.maxSamplerAnisotropy : 1u;
    robustness2.robustBufferAccess2 = robustness2.robustBufferAccess2;
    robustness2.robustImageAccess2 = robustness2.robustImageAccess2;
    robustness2.nullDescriptor = robustness2.nullDescriptor;
    g_context.enabled_features.shaderStorageImageExtendedFormats =
        supported.shaderStorageImageExtendedFormats;
    g_context.enabled_features.shaderStorageImageReadWithoutFormat =
        supported.shaderStorageImageReadWithoutFormat;
    g_context.enabled_features.shaderStorageImageWriteWithoutFormat =
        supported.shaderStorageImageWriteWithoutFormat;
    g_context.enabled_features.shaderImageGatherExtended =
        supported.shaderImageGatherExtended;
    /* Opt-in draw diagnostics use core Vulkan pipeline-statistics queries to
     * prove whether translated draws reach vertex, primitive and fragment
     * execution. Keep this conditional so unsupported devices still run. */
    g_context.enabled_features.pipelineStatisticsQuery =
        supported.pipelineStatisticsQuery;
    g_context.enabled_features.shaderInt16 = supported.shaderInt16;
    g_context.enabled_features.shaderInt64 = supported.shaderInt64;
    g_context.enabled_features.shaderFloat64 = supported.shaderFloat64;
    const void *device_features = g_context.pipeline_robustness ?
        (const void *)&pipeline_robustness :
        g_context.robustness2 ? (const void *)&robustness2 : NULL;
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = device_features,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = device_extension_count,
        .ppEnabledExtensionNames = device_extensions,
        .pEnabledFeatures = &g_context.enabled_features
    };
    if (g_context.CreateDevice(g_context.physical_device, &device_info, NULL,
                               &g_context.device) != VK_SUCCESS)
        goto fail;
    LOAD_DEVICE(DestroyDevice);
    LOAD_DEVICE(GetDeviceQueue);
    LOAD_DEVICE(DeviceWaitIdle);
    g_context.GetDeviceQueue(g_context.device, g_context.queue_family, 0,
                             &g_context.queue);
    g_context.initialized = 1;
    if (getenv("BEER_VULKAN_GPU_KEEPALIVE")) {
        pthread_t keepalive;
        if (pthread_create(&keepalive, NULL, gpu_keepalive_thread, NULL) == 0)
            pthread_detach(keepalive);
    }
    fprintf(stderr,
            "[VULKAN] shared device initialized (queue family %u, "
            "robust-buffer=%u robust-buffer2=%u robust-image2=%u "
            "pipeline-robustness=%u)\n",
            g_context.queue_family,
            g_context.enabled_features.robustBufferAccess,
            g_context.robustness2 ? robustness2.robustBufferAccess2 : 0,
            g_context.robustness2 ? robustness2.robustImageAccess2 : 0,
            g_context.pipeline_robustness);
    pthread_mutex_unlock(&g_context_lock);
    return 1;

fail:
    fprintf(stderr, "[VULKAN] shared context unavailable at %s\n", stage);
    if (g_context.device && g_context.DestroyDevice)
        g_context.DestroyDevice(g_context.device, NULL);
    if (g_context.instance && g_context.DestroyInstance)
        g_context.DestroyInstance(g_context.instance, NULL);
    if (g_context.library) dlclose(g_context.library);
    memset(&g_context, 0, sizeof(g_context));
    g_context.unavailable = 1;
    pthread_mutex_unlock(&g_context_lock);
    return 0;
}

VkInstance beer_vulkan_instance(void) { return g_context.instance; }
VkPhysicalDevice beer_vulkan_physical_device(void) { return g_context.physical_device; }
VkDevice beer_vulkan_device(void) { return g_context.device; }
VkQueue beer_vulkan_queue(void) { return g_context.queue; }
uint32_t beer_vulkan_queue_family(void) { return g_context.queue_family; }
const VkPhysicalDeviceFeatures *beer_vulkan_enabled_features(void)
{
    return &g_context.enabled_features;
}
int beer_vulkan_maintenance1_enabled(void)
{
    return g_context.maintenance1;
}
int beer_vulkan_sampler_filter_minmax_enabled(void)
{
    return g_context.sampler_filter_minmax;
}
uint32_t beer_vulkan_max_sampler_anisotropy(void)
{
    return g_context.max_sampler_anisotropy > 1u ?
        g_context.max_sampler_anisotropy : 1u;
}
int beer_vulkan_pipeline_robustness_enabled(void)
{
    return g_context.pipeline_robustness;
}
int beer_vulkan_device_lost(void)
{
    return __atomic_load_n(&g_context.device_lost, __ATOMIC_ACQUIRE);
}
void beer_vulkan_record_result(VkResult result, const char *operation)
{
    if (result != VK_ERROR_DEVICE_LOST) return;
    if (!__atomic_exchange_n(&g_context.device_lost, 1, __ATOMIC_ACQ_REL))
        fprintf(stderr,
                "[VULKAN] device lost during %s; suppressing further GPU work\n",
                operation ? operation : "unknown operation");
}
PFN_vkGetInstanceProcAddr beer_vulkan_get_instance_proc_addr(void)
{
    return g_context.GetInstanceProcAddr;
}
PFN_vkGetDeviceProcAddr beer_vulkan_get_device_proc_addr(void)
{
    return g_context.GetDeviceProcAddr;
}
void beer_vulkan_queue_lock(void) { pthread_mutex_lock(&g_queue_lock); }
void beer_vulkan_queue_unlock(void) { pthread_mutex_unlock(&g_queue_lock); }

static _Atomic uint64_t g_perf[4];

void beer_vulkan_perf_account(int graphics, uint64_t ns)
{
    atomic_fetch_add(&g_perf[graphics ? 2 : 0], 1);
    atomic_fetch_add(&g_perf[graphics ? 3 : 1], ns);
}

uint64_t beer_vulkan_perf_threshold_ns(void)
{
    static uint64_t threshold = UINT64_MAX;
    if (threshold == UINT64_MAX) {
        const char *text = getenv("BEER_VULKAN_PERF_MIN_US");
        threshold = text ? (uint64_t)strtoull(text, NULL, 10) * 1000u
                         : UINT64_C(50000000);
    }
    return threshold;
}

void beer_vulkan_perf_take(uint64_t out[4])
{
    for (int i = 0; i < 4; ++i) out[i] = atomic_exchange(&g_perf[i], 0);
}

void beer_vulkan_context_destroy(void)
{
    pthread_mutex_lock(&g_context_lock);
    if (g_context.device && g_context.DeviceWaitIdle)
        g_context.DeviceWaitIdle(g_context.device);
    if (g_context.device && g_context.DestroyDevice)
        g_context.DestroyDevice(g_context.device, NULL);
    if (g_context.instance && g_context.DestroyInstance)
        g_context.DestroyInstance(g_context.instance, NULL);
    if (g_context.library) dlclose(g_context.library);
    memset(&g_context, 0, sizeof(g_context));
    pthread_mutex_unlock(&g_context_lock);
}
