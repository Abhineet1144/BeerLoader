#define _GNU_SOURCE
#include "vulkan_presenter.h"
#include "vulkan_renderer.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct _XDisplay Display;
typedef unsigned long Window;
typedef unsigned long VisualID;
#define VK_USE_PLATFORM_XLIB_KHR
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_xlib.h>

typedef struct {
    void *library;
    VkInstance instance;
    VkSurfaceKHR surface;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkSwapchainKHR swapchain;
    VkFormat format;
    VkExtent2D extent;
    VkImage *images;
    uint8_t *image_initialized;
    uint32_t image_count;
    VkImage upload_image;
    VkDeviceMemory upload_memory;
    VkExtent2D upload_extent;
    int upload_initialized;
    VkBuffer staging_buffer;
    VkDeviceMemory staging_memory;
    uint8_t *staging_map;
    VkDeviceSize staging_size;
    VkCommandPool command_pool;
    VkCommandBuffer command_buffer;
    VkSemaphore image_available;
    VkSemaphore render_finished;
    VkFence submit_fence;
    int initialized;
    int permanently_unavailable;

    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
    PFN_vkDestroyInstance DestroyInstance;
    PFN_vkCreateXlibSurfaceKHR CreateXlibSurfaceKHR;
    PFN_vkDestroySurfaceKHR DestroySurfaceKHR;
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR GetPhysicalDeviceSurfaceSupportKHR;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR GetPhysicalDeviceSurfaceFormatsKHR;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR GetPhysicalDeviceSurfacePresentModesKHR;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties;
    PFN_vkCreateDevice CreateDevice;
    PFN_vkDestroyDevice DestroyDevice;
    PFN_vkGetDeviceQueue GetDeviceQueue;
    PFN_vkCreateSwapchainKHR CreateSwapchainKHR;
    PFN_vkDestroySwapchainKHR DestroySwapchainKHR;
    PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR;
    PFN_vkAcquireNextImageKHR AcquireNextImageKHR;
    PFN_vkQueuePresentKHR QueuePresentKHR;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkCreateFence CreateFence;
    PFN_vkDestroyFence DestroyFence;
    PFN_vkResetFences ResetFences;
    PFN_vkWaitForFences WaitForFences;
    PFN_vkDeviceWaitIdle DeviceWaitIdle;
    PFN_vkCreateBuffer CreateBuffer;
    PFN_vkDestroyBuffer DestroyBuffer;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkBindBufferMemory BindBufferMemory;
    PFN_vkCreateImage CreateImage;
    PFN_vkDestroyImage DestroyImage;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
    PFN_vkBindImageMemory BindImageMemory;
    PFN_vkMapMemory MapMemory;
    PFN_vkUnmapMemory UnmapMemory;
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkResetCommandBuffer ResetCommandBuffer;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage;
    PFN_vkCmdBlitImage CmdBlitImage;
    PFN_vkCreateSemaphore CreateSemaphore;
    PFN_vkDestroySemaphore DestroySemaphore;
} VulkanPresenter;

static VulkanPresenter g_vk;

#define LOAD_INSTANCE(name) do { \
    g_vk.name = (PFN_vk##name)g_vk.GetInstanceProcAddr(g_vk.instance, "vk" #name); \
    if (!g_vk.name) { fprintf(stderr, "[VULKAN] missing vk%s\n", #name); return 0; } \
} while (0)
#define LOAD_DEVICE(name) do { \
    g_vk.name = (PFN_vk##name)g_vk.GetDeviceProcAddr(g_vk.device, "vk" #name); \
    if (!g_vk.name) { fprintf(stderr, "[VULKAN] missing vk%s\n", #name); return 0; } \
} while (0)

static void destroy_swapchain(void)
{
    if (g_vk.device && g_vk.DeviceWaitIdle) g_vk.DeviceWaitIdle(g_vk.device);
    if (g_vk.staging_map && g_vk.UnmapMemory)
        g_vk.UnmapMemory(g_vk.device, g_vk.staging_memory);
    g_vk.staging_map = NULL;
    if (g_vk.staging_buffer && g_vk.DestroyBuffer)
        g_vk.DestroyBuffer(g_vk.device, g_vk.staging_buffer, NULL);
    if (g_vk.staging_memory && g_vk.FreeMemory)
        g_vk.FreeMemory(g_vk.device, g_vk.staging_memory, NULL);
    if (g_vk.upload_image && g_vk.DestroyImage)
        g_vk.DestroyImage(g_vk.device, g_vk.upload_image, NULL);
    if (g_vk.upload_memory && g_vk.FreeMemory)
        g_vk.FreeMemory(g_vk.device, g_vk.upload_memory, NULL);
    if (g_vk.swapchain && g_vk.DestroySwapchainKHR)
        g_vk.DestroySwapchainKHR(g_vk.device, g_vk.swapchain, NULL);
    free(g_vk.images);
    free(g_vk.image_initialized);
    g_vk.images = NULL;
    g_vk.image_initialized = NULL;
    g_vk.image_count = 0;
    g_vk.swapchain = VK_NULL_HANDLE;
    g_vk.staging_buffer = VK_NULL_HANDLE;
    g_vk.staging_memory = VK_NULL_HANDLE;
    g_vk.staging_size = 0;
    g_vk.upload_image = VK_NULL_HANDLE;
    g_vk.upload_memory = VK_NULL_HANDLE;
    g_vk.upload_initialized = 0;
    memset(&g_vk.upload_extent, 0, sizeof(g_vk.upload_extent));
    memset(&g_vk.extent, 0, sizeof(g_vk.extent));
}

void vulkan_presenter_destroy(void)
{
    destroy_swapchain();
    if (g_vk.device) {
        if (g_vk.submit_fence && g_vk.DestroyFence)
            g_vk.DestroyFence(g_vk.device, g_vk.submit_fence, NULL);
        if (g_vk.image_available) g_vk.DestroySemaphore(g_vk.device, g_vk.image_available, NULL);
        if (g_vk.render_finished) g_vk.DestroySemaphore(g_vk.device, g_vk.render_finished, NULL);
        if (g_vk.command_pool) g_vk.DestroyCommandPool(g_vk.device, g_vk.command_pool, NULL);
        g_vk.DestroyDevice(g_vk.device, NULL);
    }
    if (g_vk.surface && g_vk.DestroySurfaceKHR)
        g_vk.DestroySurfaceKHR(g_vk.instance, g_vk.surface, NULL);
    if (g_vk.instance && g_vk.DestroyInstance)
        g_vk.DestroyInstance(g_vk.instance, NULL);
    if (g_vk.library) dlclose(g_vk.library);
    memset(&g_vk, 0, sizeof(g_vk));
}

static int load_instance_functions(void)
{
    LOAD_INSTANCE(DestroyInstance);
    LOAD_INSTANCE(CreateXlibSurfaceKHR);
    LOAD_INSTANCE(DestroySurfaceKHR);
    LOAD_INSTANCE(EnumeratePhysicalDevices);
    LOAD_INSTANCE(GetPhysicalDeviceQueueFamilyProperties);
    LOAD_INSTANCE(GetPhysicalDeviceSurfaceSupportKHR);
    LOAD_INSTANCE(GetPhysicalDeviceSurfaceCapabilitiesKHR);
    LOAD_INSTANCE(GetPhysicalDeviceSurfaceFormatsKHR);
    LOAD_INSTANCE(GetPhysicalDeviceSurfacePresentModesKHR);
    LOAD_INSTANCE(GetPhysicalDeviceMemoryProperties);
    LOAD_INSTANCE(GetPhysicalDeviceFormatProperties);
    LOAD_INSTANCE(CreateDevice);
    g_vk.GetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)
        g_vk.GetInstanceProcAddr(g_vk.instance, "vkGetDeviceProcAddr");
    return g_vk.GetDeviceProcAddr != NULL;
}

static int load_device_functions(void)
{
    LOAD_DEVICE(DestroyDevice);
    LOAD_DEVICE(GetDeviceQueue);
    LOAD_DEVICE(CreateSwapchainKHR);
    LOAD_DEVICE(DestroySwapchainKHR);
    LOAD_DEVICE(GetSwapchainImagesKHR);
    LOAD_DEVICE(AcquireNextImageKHR);
    LOAD_DEVICE(QueuePresentKHR);
    LOAD_DEVICE(QueueSubmit);
    LOAD_DEVICE(CreateFence);
    LOAD_DEVICE(DestroyFence);
    LOAD_DEVICE(ResetFences);
    LOAD_DEVICE(WaitForFences);
    LOAD_DEVICE(DeviceWaitIdle);
    LOAD_DEVICE(CreateBuffer);
    LOAD_DEVICE(DestroyBuffer);
    LOAD_DEVICE(GetBufferMemoryRequirements);
    LOAD_DEVICE(AllocateMemory);
    LOAD_DEVICE(FreeMemory);
    LOAD_DEVICE(BindBufferMemory);
    LOAD_DEVICE(CreateImage);
    LOAD_DEVICE(DestroyImage);
    LOAD_DEVICE(GetImageMemoryRequirements);
    LOAD_DEVICE(BindImageMemory);
    LOAD_DEVICE(MapMemory);
    LOAD_DEVICE(UnmapMemory);
    LOAD_DEVICE(CreateCommandPool);
    LOAD_DEVICE(DestroyCommandPool);
    LOAD_DEVICE(AllocateCommandBuffers);
    LOAD_DEVICE(ResetCommandBuffer);
    LOAD_DEVICE(BeginCommandBuffer);
    LOAD_DEVICE(EndCommandBuffer);
    LOAD_DEVICE(CmdPipelineBarrier);
    LOAD_DEVICE(CmdCopyBufferToImage);
    LOAD_DEVICE(CmdBlitImage);
    LOAD_DEVICE(CreateSemaphore);
    LOAD_DEVICE(DestroySemaphore);
    return 1;
}

static int initialize(void *display, unsigned long window)
{
    if (g_vk.initialized) return 1;
    if (g_vk.permanently_unavailable) return 0;

    g_vk.library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!g_vk.library) goto unavailable;
    g_vk.GetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)
        dlsym(g_vk.library, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance create_instance = (PFN_vkCreateInstance)
        dlsym(g_vk.library, "vkCreateInstance");
    if (!g_vk.GetInstanceProcAddr || !create_instance) goto unavailable;

    const char *extensions[] = { VK_KHR_SURFACE_EXTENSION_NAME,
                                 VK_KHR_XLIB_SURFACE_EXTENSION_NAME };
    VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Beer",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "Beer CPU D3D11",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_0
    };
    VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application,
        .enabledExtensionCount = 2,
        .ppEnabledExtensionNames = extensions
    };
    if (create_instance(&instance_info, NULL, &g_vk.instance) != VK_SUCCESS)
        goto unavailable;
    if (!load_instance_functions()) goto unavailable;

    VkXlibSurfaceCreateInfoKHR surface_info = {
        .sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
        .dpy = (Display *)display,
        .window = (Window)window
    };
    if (g_vk.CreateXlibSurfaceKHR(g_vk.instance, &surface_info, NULL,
                                  &g_vk.surface) != VK_SUCCESS)
        goto unavailable;

    uint32_t physical_count = 0;
    if (g_vk.EnumeratePhysicalDevices(g_vk.instance, &physical_count, NULL) != VK_SUCCESS ||
        physical_count == 0) goto unavailable;
    VkPhysicalDevice *physical = calloc(physical_count, sizeof(*physical));
    if (!physical) goto unavailable;
    g_vk.EnumeratePhysicalDevices(g_vk.instance, &physical_count, physical);
    for (uint32_t p = 0; p < physical_count && !g_vk.physical_device; ++p) {
        uint32_t queue_count = 0;
        g_vk.GetPhysicalDeviceQueueFamilyProperties(physical[p], &queue_count, NULL);
        VkQueueFamilyProperties *queues = calloc(queue_count, sizeof(*queues));
        if (!queues) continue;
        g_vk.GetPhysicalDeviceQueueFamilyProperties(physical[p], &queue_count, queues);
        for (uint32_t q = 0; q < queue_count; ++q) {
            VkBool32 present = VK_FALSE;
            g_vk.GetPhysicalDeviceSurfaceSupportKHR(physical[p], q, g_vk.surface, &present);
            if (present && (queues[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                g_vk.physical_device = physical[p];
                g_vk.queue_family = q;
                break;
            }
        }
        free(queues);
    }
    free(physical);
    if (!g_vk.physical_device) goto unavailable;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = g_vk.queue_family,
        .queueCount = 1,
        .pQueuePriorities = &priority
    };
    const char *device_extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = device_extensions
    };
    if (g_vk.CreateDevice(g_vk.physical_device, &device_info, NULL,
                          &g_vk.device) != VK_SUCCESS)
        goto unavailable;
    if (!load_device_functions()) goto unavailable;
    g_vk.GetDeviceQueue(g_vk.device, g_vk.queue_family, 0, &g_vk.queue);

    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = g_vk.queue_family
    };
    if (g_vk.CreateCommandPool(g_vk.device, &pool_info, NULL,
                               &g_vk.command_pool) != VK_SUCCESS)
        goto unavailable;
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = g_vk.command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };
    if (g_vk.AllocateCommandBuffers(g_vk.device, &command_info,
                                    &g_vk.command_buffer) != VK_SUCCESS)
        goto unavailable;
    VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
    };
    if (g_vk.CreateSemaphore(g_vk.device, &semaphore_info, NULL,
                             &g_vk.image_available) != VK_SUCCESS ||
        g_vk.CreateSemaphore(g_vk.device, &semaphore_info, NULL,
                             &g_vk.render_finished) != VK_SUCCESS)
        goto unavailable;
    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT
    };
    if (g_vk.CreateFence(g_vk.device, &fence_info, NULL,
                         &g_vk.submit_fence) != VK_SUCCESS)
        goto unavailable;

    g_vk.initialized = 1;
    fprintf(stderr, "[VULKAN] XWayland swapchain presenter initialized\n");
    return 1;

unavailable:
    fprintf(stderr, "[VULKAN] presenter unavailable\n");
    vulkan_presenter_destroy();
    g_vk.permanently_unavailable = 1;
    return 0;
}

static int find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags required,
                            uint32_t *index)
{
    VkPhysicalDeviceMemoryProperties memory;
    g_vk.GetPhysicalDeviceMemoryProperties(g_vk.physical_device, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) &&
            (memory.memoryTypes[i].propertyFlags & required) == required) {
            *index = i;
            return 1;
        }
    }
    return 0;
}

static int recreate_swapchain(int requested_width, int requested_height,
                              int source_width, int source_height)
{
    destroy_swapchain();
    VkSurfaceCapabilitiesKHR capabilities;
    if (g_vk.GetPhysicalDeviceSurfaceCapabilitiesKHR(
            g_vk.physical_device, g_vk.surface, &capabilities) != VK_SUCCESS)
        return 0;

    uint32_t format_count = 0;
    g_vk.GetPhysicalDeviceSurfaceFormatsKHR(g_vk.physical_device, g_vk.surface,
                                             &format_count, NULL);
    if (!format_count) return 0;
    VkSurfaceFormatKHR *formats = calloc(format_count, sizeof(*formats));
    if (!formats) return 0;
    g_vk.GetPhysicalDeviceSurfaceFormatsKHR(g_vk.physical_device, g_vk.surface,
                                             &format_count, formats);
    VkSurfaceFormatKHR chosen = formats[0];
    for (uint32_t i = 0; i < format_count; ++i) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM ||
            formats[i].format == VK_FORMAT_B8G8R8A8_SRGB) {
            chosen = formats[i];
            if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM) break;
        }
    }
    free(formats);
    if (!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        return 0;
    VkImageUsageFlags image_usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    /* Overlay layers render into swapchain images. Advertise color-attachment
     * usage when the surface supports it even though Beer itself only copies. */
    if (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
        image_usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = (uint32_t)requested_width;
        extent.height = (uint32_t)requested_height;
        if (extent.width < capabilities.minImageExtent.width)
            extent.width = capabilities.minImageExtent.width;
        if (extent.width > capabilities.maxImageExtent.width)
            extent.width = capabilities.maxImageExtent.width;
        if (extent.height < capabilities.minImageExtent.height)
            extent.height = capabilities.minImageExtent.height;
        if (extent.height > capabilities.maxImageExtent.height)
            extent.height = capabilities.maxImageExtent.height;
    }
    uint32_t image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount && image_count > capabilities.maxImageCount)
        image_count = capabilities.maxImageCount;
    VkCompositeAlphaFlagBitsKHR composite = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(capabilities.supportedCompositeAlpha & composite))
        composite = (VkCompositeAlphaFlagBitsKHR)
            (capabilities.supportedCompositeAlpha & -capabilities.supportedCompositeAlpha);

    VkSwapchainCreateInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = g_vk.surface,
        .minImageCount = image_count,
        .imageFormat = chosen.format,
        .imageColorSpace = chosen.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = image_usage,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = capabilities.currentTransform,
        .compositeAlpha = composite,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE
    };
    if (g_vk.CreateSwapchainKHR(g_vk.device, &info, NULL,
                                &g_vk.swapchain) != VK_SUCCESS)
        return 0;
    g_vk.format = chosen.format;
    g_vk.extent = extent;
    g_vk.GetSwapchainImagesKHR(g_vk.device, g_vk.swapchain,
                               &g_vk.image_count, NULL);
    g_vk.images = calloc(g_vk.image_count, sizeof(*g_vk.images));
    g_vk.image_initialized = calloc(g_vk.image_count, 1);
    if (!g_vk.images || !g_vk.image_initialized) return 0;
    g_vk.GetSwapchainImagesKHR(g_vk.device, g_vk.swapchain,
                               &g_vk.image_count, g_vk.images);

    VkFormatProperties rgba_properties;
    g_vk.GetPhysicalDeviceFormatProperties(g_vk.physical_device,
                                            VK_FORMAT_R8G8B8A8_UNORM,
                                            &rgba_properties);
    if (!(rgba_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT))
        return 0;
    VkFormatProperties swapchain_properties;
    g_vk.GetPhysicalDeviceFormatProperties(g_vk.physical_device, chosen.format,
                                            &swapchain_properties);
    if (!(swapchain_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT))
        return 0;

    /* Keep the CPU renderer's native RGBA8 frame at guest resolution. Vulkan
     * performs channel conversion and scaling while blitting this image into
     * the XWayland swapchain, removing the previous CPU full-window loop. */
    g_vk.upload_extent = (VkExtent2D) {
        (uint32_t)source_width, (uint32_t)source_height
    };
    VkImageCreateInfo upload_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { g_vk.upload_extent.width, g_vk.upload_extent.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };
    if (g_vk.CreateImage(g_vk.device, &upload_info, NULL,
                         &g_vk.upload_image) != VK_SUCCESS)
        return 0;
    VkMemoryRequirements image_requirements;
    g_vk.GetImageMemoryRequirements(g_vk.device, g_vk.upload_image,
                                    &image_requirements);
    uint32_t image_memory_type = 0;
    if (!find_memory_type(image_requirements.memoryTypeBits,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                          &image_memory_type))
        return 0;
    VkMemoryAllocateInfo image_allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = image_requirements.size,
        .memoryTypeIndex = image_memory_type
    };
    if (g_vk.AllocateMemory(g_vk.device, &image_allocation, NULL,
                            &g_vk.upload_memory) != VK_SUCCESS ||
        g_vk.BindImageMemory(g_vk.device, g_vk.upload_image,
                             g_vk.upload_memory, 0) != VK_SUCCESS)
        return 0;

    g_vk.staging_size = (VkDeviceSize)g_vk.upload_extent.width *
                        g_vk.upload_extent.height * 4;
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = g_vk.staging_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    if (g_vk.CreateBuffer(g_vk.device, &buffer_info, NULL,
                          &g_vk.staging_buffer) != VK_SUCCESS)
        return 0;
    VkMemoryRequirements requirements;
    g_vk.GetBufferMemoryRequirements(g_vk.device, g_vk.staging_buffer, &requirements);
    uint32_t memory_type = 0;
    if (!find_memory_type(requirements.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &memory_type)) return 0;
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type
    };
    if (g_vk.AllocateMemory(g_vk.device, &allocation, NULL,
                            &g_vk.staging_memory) != VK_SUCCESS ||
        g_vk.BindBufferMemory(g_vk.device, g_vk.staging_buffer,
                              g_vk.staging_memory, 0) != VK_SUCCESS ||
        g_vk.MapMemory(g_vk.device, g_vk.staging_memory, 0, g_vk.staging_size,
                       0, (void **)&g_vk.staging_map) != VK_SUCCESS)
        return 0;
    fprintf(stderr, "[VULKAN] swapchain %ux%u format=%u images=%u\n",
            extent.width, extent.height, chosen.format, g_vk.image_count);
    return 1;
}

static void copy_rgba8_to_staging(const uint8_t *pixels, int width, int height,
                                  int row_pitch)
{
    size_t row_bytes = (size_t)width * 4;
    for (int y = 0; y < height; ++y)
        memcpy(g_vk.staging_map + (size_t)y * row_bytes,
               pixels + (size_t)y * (size_t)row_pitch, row_bytes);
}

static int present_rgba8_staging(void *display, unsigned long window,
                                 const void *resource, uint64_t serial,
                                 const uint8_t *pixels,
                                 int width, int height, int row_pitch,
                                 int output_width, int output_height)
{
    if (!pixels || width <= 0 || height <= 0 || row_pitch < width * 4 ||
        output_width <= 0 || output_height <= 0)
        return 0;
    if (!initialize(display, window)) return 0;
    if (!g_vk.swapchain || g_vk.extent.width != (uint32_t)output_width ||
        g_vk.extent.height != (uint32_t)output_height) {
        if (!recreate_swapchain(output_width, output_height, width, height)) return 0;
    }

    size_t bytes = (size_t)width * (size_t)height * 4u;
    int copied_gpu_mirror = row_pitch == width * 4 && resource &&
        vulkan_renderer_copy_target(resource, serial, g_vk.staging_map, bytes);
    if (!copied_gpu_mirror)
        copy_rgba8_to_staging(pixels, width, height, row_pitch);
    static uint64_t gpu_mirror_presents;
    if (copied_gpu_mirror) {
        uint64_t count = ++gpu_mirror_presents;
        if (count <= 4 || (count % 256u) == 0)
            fprintf(stderr, "[VULKAN] presenter source=compositor-mirror count=%llu "
                    "serial=%llu bytes=%zu\n", (unsigned long long)count,
                    (unsigned long long)serial, bytes);
    }
    uint32_t image_index = 0;
    VkResult result = g_vk.AcquireNextImageKHR(g_vk.device, g_vk.swapchain,
                                                UINT64_MAX,
                                                g_vk.image_available,
                                                VK_NULL_HANDLE, &image_index);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        return recreate_swapchain(output_width, output_height, width, height);
    }
    if (result != VK_SUCCESS) return 0;

    g_vk.ResetCommandBuffer(g_vk.command_buffer, 0);
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    if (g_vk.BeginCommandBuffer(g_vk.command_buffer, &begin) != VK_SUCCESS)
        return 0;
    VkImageMemoryBarrier barriers[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = g_vk.upload_initialized
                ? VK_ACCESS_TRANSFER_READ_BIT : 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = g_vk.upload_initialized
                ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = g_vk.upload_image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = g_vk.image_initialized[image_index]
                ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = g_vk.images[image_index],
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
        }
    };
    g_vk.CmdPipelineBarrier(g_vk.command_buffer,
        (g_vk.upload_initialized || g_vk.image_initialized[image_index])
            ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT
            : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, barriers);
    VkBufferImageCopy copy = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { g_vk.upload_extent.width, g_vk.upload_extent.height, 1 }
    };
    g_vk.CmdCopyBufferToImage(g_vk.command_buffer, g_vk.staging_buffer,
                              g_vk.upload_image,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    VkImageMemoryBarrier upload_read = barriers[0];
    upload_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    upload_read.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    upload_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    upload_read.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    g_vk.CmdPipelineBarrier(g_vk.command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                            0, NULL, 0, NULL, 1, &upload_read);
    VkImageBlit blit = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffsets = { { 0, 0, 0 },
                        { (int32_t)g_vk.upload_extent.width,
                          (int32_t)g_vk.upload_extent.height, 1 } },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets = { { 0, 0, 0 },
                        { (int32_t)g_vk.extent.width,
                          (int32_t)g_vk.extent.height, 1 } }
    };
    g_vk.CmdBlitImage(g_vk.command_buffer, g_vk.upload_image,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      g_vk.images[image_index],
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      1, &blit, VK_FILTER_LINEAR);
    VkImageMemoryBarrier after = barriers[1];
    after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    after.dstAccessMask = 0;
    after.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    after.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    g_vk.CmdPipelineBarrier(g_vk.command_buffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, NULL, 0, NULL, 1, &after);
    if (g_vk.EndCommandBuffer(g_vk.command_buffer) != VK_SUCCESS) return 0;

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &g_vk.image_available,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &g_vk.command_buffer,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &g_vk.render_finished
    };
    if (g_vk.ResetFences(g_vk.device, 1, &g_vk.submit_fence) != VK_SUCCESS ||
        g_vk.QueueSubmit(g_vk.queue, 1, &submit, g_vk.submit_fence) != VK_SUCCESS)
        return 0;
    VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &g_vk.render_finished,
        .swapchainCount = 1,
        .pSwapchains = &g_vk.swapchain,
        .pImageIndices = &image_index
    };
    result = g_vk.QueuePresentKHR(g_vk.queue, &present);
    if (g_vk.WaitForFences(g_vk.device, 1, &g_vk.submit_fence, VK_TRUE,
                           UINT64_MAX) != VK_SUCCESS)
        return 0;
    g_vk.upload_initialized = 1;
    g_vk.image_initialized[image_index] = 1;
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        return recreate_swapchain(output_width, output_height, width, height);
    }
    return result == VK_SUCCESS;
}

int vulkan_presenter_present_rgba8(void *display, unsigned long window,
                                   const uint8_t *pixels,
                                   int width, int height, int row_pitch,
                                   int output_width, int output_height)
{
    return present_rgba8_staging(display, window, NULL, 0, pixels,
                                 width, height, row_pitch,
                                 output_width, output_height);
}

int vulkan_presenter_present_resource_rgba8(void *display, unsigned long window,
                                            const void *resource,
                                            uint64_t serial,
                                            const uint8_t *pixels,
                                            int width, int height, int row_pitch,
                                            int output_width, int output_height)
{
    return present_rgba8_staging(display, window, resource, serial, pixels,
                                 width, height, row_pitch,
                                 output_width, output_height);
}

int vulkan_presenter_is_active(void)
{
    return g_vk.initialized;
}
