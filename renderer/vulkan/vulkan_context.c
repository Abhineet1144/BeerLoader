#define _GNU_SOURCE
#include "vulkan_context.h"

#include <dlfcn.h>
#include <pthread.h>
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
        "VK_KHR_xlib_surface"
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
        .enabledExtensionCount = 2,
        .ppEnabledExtensionNames = instance_extensions
    };
    if (create_instance(&instance_info, NULL, &g_context.instance) != VK_SUCCESS)
        goto fail;
    LOAD_INSTANCE(DestroyInstance);
    LOAD_INSTANCE(EnumeratePhysicalDevices);
    LOAD_INSTANCE(GetPhysicalDeviceQueueFamilyProperties);
    LOAD_INSTANCE(CreateDevice);
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
    const char *device_extensions[] = { "VK_KHR_swapchain" };
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = device_extensions
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
    fprintf(stderr, "[VULKAN] shared device initialized (queue family %u)\n",
            g_context.queue_family);
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
