#define _GNU_SOURCE
#include "vulkan_presenter.h"
#include "vulkan_renderer.h"
#include "vulkan_indexed_renderer.h"
#include "vulkan_context.h"
#include "../../platform/beer_overlay.h"
#include "vulkan_post.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    VkSemaphore render_finished_image[16]; /* direct-image path: one per swapchain image */
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
    VkBuffer overlay_buffer;
    VkDeviceMemory overlay_memory;
    uint8_t *overlay_map;
    uint64_t overlay_serial[2];
    PFN_vkCmdCopyImage CmdCopyImage;
    PFN_vkCreateSemaphore CreateSemaphore;
    PFN_vkDestroySemaphore DestroySemaphore;
} VulkanPresenter;

static VulkanPresenter g_vk;

/* Opt-in crash-safe presenter breadcrumb. It complements the BD12 submission
 * recorder because presentation submits directly to the shared Vulkan queue. */
static void record_direct_present(uint64_t sequence, const char *stage,
                                  uint64_t source_image, uint32_t source_layout,
                                  int width, int height, int output_width,
                                  int output_height, int image_index,
                                  VkResult result)
{
    const char *directory = getenv("BEER_BD12_FLIGHT_RECORDER");
    if (!directory || !*directory) return;
    if (!strcmp(directory, "1")) directory = "/tmp";
    char path[512], temporary[544];
    snprintf(path, sizeof(path), "%s/beer-bd12-present.log", directory);
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    FILE *out = fopen(temporary, "w");
    if (!out) return;
    fprintf(out, "present=%llu stage=%s source=%016llx layout=%u "
                 "source-size=%d,%d output-size=%d,%d swap-image=%d result=%d\n",
            (unsigned long long)sequence, stage,
            (unsigned long long)source_image, source_layout, width, height,
            output_width, output_height, image_index, (int)result);
    if (fclose(out) == 0) rename(temporary, path);
    else remove(temporary);
}

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
    vulkan_post_forget_upload();
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
    /* The shared Vulkan device outlives all three stages. Destroy compute-stage
     * resources before the presenter releases the final surface resources. */
    vulkan_renderer_destroy();
    vulkan_indexed_renderer_destroy();
    destroy_swapchain();
    if (g_vk.device) {
        if (g_vk.submit_fence && g_vk.DestroyFence)
            g_vk.DestroyFence(g_vk.device, g_vk.submit_fence, NULL);
        if (g_vk.image_available) g_vk.DestroySemaphore(g_vk.device, g_vk.image_available, NULL);
        if (g_vk.render_finished) g_vk.DestroySemaphore(g_vk.device, g_vk.render_finished, NULL);
        for (int i = 0; i < 16; ++i)
            if (g_vk.render_finished_image[i])
                g_vk.DestroySemaphore(g_vk.device, g_vk.render_finished_image[i], NULL);
        if (g_vk.command_pool) g_vk.DestroyCommandPool(g_vk.device, g_vk.command_pool, NULL);
    }
    if (g_vk.surface && g_vk.DestroySurfaceKHR)
        g_vk.DestroySurfaceKHR(g_vk.instance, g_vk.surface, NULL);
    memset(&g_vk, 0, sizeof(g_vk));
    beer_vulkan_context_destroy();
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
    LOAD_DEVICE(CmdCopyImage);
    LOAD_DEVICE(CreateSemaphore);
    LOAD_DEVICE(DestroySemaphore);
    return 1;
}

static int initialize(void *display, unsigned long window)
{
    if (g_vk.initialized) return 1;
    if (g_vk.permanently_unavailable) return 0;

    if (!beer_vulkan_context_initialize()) goto unavailable;
    g_vk.instance = beer_vulkan_instance();
    g_vk.physical_device = beer_vulkan_physical_device();
    g_vk.device = beer_vulkan_device();
    g_vk.queue = beer_vulkan_queue();
    g_vk.queue_family = beer_vulkan_queue_family();
    g_vk.GetInstanceProcAddr = beer_vulkan_get_instance_proc_addr();
    g_vk.GetDeviceProcAddr = beer_vulkan_get_device_proc_addr();
    if (!g_vk.instance || !g_vk.physical_device || !g_vk.device || !g_vk.queue ||
        !g_vk.GetInstanceProcAddr || !g_vk.GetDeviceProcAddr)
        goto unavailable;
    if (!load_instance_functions() || !load_device_functions()) goto unavailable;

    VkXlibSurfaceCreateInfoKHR surface_info = {
        .sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
        .dpy = (Display *)display,
        .window = (Window)window
    };
    if (g_vk.CreateXlibSurfaceKHR(g_vk.instance, &surface_info, NULL,
                                  &g_vk.surface) != VK_SUCCESS)
        goto unavailable;
    VkBool32 present_supported = VK_FALSE;
    if (g_vk.GetPhysicalDeviceSurfaceSupportKHR(
            g_vk.physical_device, g_vk.queue_family, g_vk.surface,
            &present_supported) != VK_SUCCESS || !present_supported)
        goto unavailable;

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
    for (int i = 0; i < 16; ++i)
        if (g_vk.CreateSemaphore(g_vk.device, &semaphore_info, NULL,
                                 &g_vk.render_finished_image[i]) != VK_SUCCESS)
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

/* In-game overlay: the boxes are CPU-rendered (platform/beer_overlay.c) into a persistent host-visible buffer
 * and copied onto the swapchain image after the frame was blitted into it. Called with the image in
 * TRANSFER_DST layout. Boxes are opaque; the cost while hidden is one mutex-free branch. */
#define OVERLAY_REGION_BYTES (4u * 1024u * 1024u)
static void record_overlay(VkImage destination)
{
    BeerOverlayRect rects[2];
    const int count = beer_overlay_frame((int)g_vk.extent.width, (int)g_vk.extent.height, rects);
    if (count <= 0) return;
    if (!g_vk.overlay_buffer) {
        VkBufferCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = 2u * OVERLAY_REGION_BYTES,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE
        };
        VkMemoryRequirements requirements;
        uint32_t type = 0;
        if (g_vk.CreateBuffer(g_vk.device, &info, NULL, &g_vk.overlay_buffer) != VK_SUCCESS) return;
        g_vk.GetBufferMemoryRequirements(g_vk.device, g_vk.overlay_buffer, &requirements);
        VkMemoryAllocateInfo allocation = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size };
        if (!find_memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &type) ||
            (allocation.memoryTypeIndex = type, g_vk.AllocateMemory(g_vk.device, &allocation, NULL, &g_vk.overlay_memory) != VK_SUCCESS) ||
            g_vk.BindBufferMemory(g_vk.device, g_vk.overlay_buffer, g_vk.overlay_memory, 0) != VK_SUCCESS ||
            g_vk.MapMemory(g_vk.device, g_vk.overlay_memory, 0, info.size, 0, (void **)&g_vk.overlay_map) != VK_SUCCESS) {
            g_vk.overlay_buffer = VK_NULL_HANDLE;
            return;
        }
    }
    for (int i = 0; i < count; ++i) {
        const BeerOverlayRect *r = &rects[i];
        const size_t bytes = (size_t)r->w * (size_t)r->h * 4u;
        if (bytes > OVERLAY_REGION_BYTES || r->w <= 0 || r->h <= 0) continue;
        const int slot = (int)(r->serial & 1u);   /* box index is encoded in the serial's low bit */
        uint8_t *region = g_vk.overlay_map + (size_t)slot * OVERLAY_REGION_BYTES;
        if (g_vk.overlay_serial[slot] != r->serial) {
            memcpy(region, r->bgra, bytes);
            g_vk.overlay_serial[slot] = r->serial;
        }
        VkBufferImageCopy copy = {
            .bufferOffset = (VkDeviceSize)slot * OVERLAY_REGION_BYTES,
            .bufferRowLength = (uint32_t)r->w,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageOffset = { r->x, r->y, 0 },
            .imageExtent = { (uint32_t)r->w, (uint32_t)r->h, 1 }
        };
        g_vk.CmdCopyBufferToImage(g_vk.command_buffer, g_vk.overlay_buffer, destination,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    }
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

    /* FIFO makes every present wait for a compositor frame callback; under
     * XWayland an unfocused/occluded window receives those about once per
     * second, which capped the whole game at 1 FPS. The guest paces itself, so
     * prefer MAILBOX (no tearing, never blocks), then IMMEDIATE. FIFO stays
     * available through BEER_PRESENT_MODE=fifo. */
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    {
        const char *requested = getenv("BEER_PRESENT_MODE");
        VkPresentModeKHR wanted[3] = {
            VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR,
            VK_PRESENT_MODE_FIFO_KHR
        };
        if (requested && !strcmp(requested, "fifo"))
            wanted[0] = VK_PRESENT_MODE_FIFO_KHR;
        else if (requested && !strcmp(requested, "immediate")) {
            wanted[0] = VK_PRESENT_MODE_IMMEDIATE_KHR;
            wanted[1] = VK_PRESENT_MODE_MAILBOX_KHR;
        }
        uint32_t mode_count = 0;
        VkPresentModeKHR modes[16];
        if (g_vk.GetPhysicalDeviceSurfacePresentModesKHR &&
            g_vk.GetPhysicalDeviceSurfacePresentModesKHR(
                g_vk.physical_device, g_vk.surface, &mode_count, NULL) == VK_SUCCESS &&
            mode_count) {
            if (mode_count > 16) mode_count = 16;
            if (g_vk.GetPhysicalDeviceSurfacePresentModesKHR(
                    g_vk.physical_device, g_vk.surface, &mode_count, modes) == VK_SUCCESS) {
                for (int w = 0; w < 3; ++w) {
                    int found = 0;
                    for (uint32_t m = 0; m < mode_count; ++m)
                        found |= modes[m] == wanted[w];
                    if (found) { present_mode = wanted[w]; break; }
                }
            }
        }
        fprintf(stderr, "[VULKAN] present mode=%d\n", (int)present_mode);
    }
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
        .presentMode = present_mode,
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

    /* Keep the upload image at the guest source resolution. The buffer copy
     * below consumes tightly packed source rows, then VkCmdBlitImage scales
     * that image to the native swapchain extent. Enlarging this image to the
     * output extent makes Vulkan interpret source rows with the output width
     * and read staging bytes that were never populated. */
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
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 ((rgba_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)
                      ? VK_IMAGE_USAGE_STORAGE_BIT : 0),   /* final-image compute passes (vulkan_post.c) */
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
    static int timing = -1;
    static uint64_t stage_ns[5];
    static uint32_t stage_calls;
    if (timing < 0) timing = getenv("BEER_PRESENT_TIMING") != NULL;
#define PT_MARK(slot, t0) do { if (timing) { \
        struct timespec pts; clock_gettime(CLOCK_MONOTONIC, &pts); \
        uint64_t pn = (uint64_t)pts.tv_sec * 1000000000ull + pts.tv_nsec; \
        if (slot >= 0) { stage_ns[slot] += pn - t0; } t0 = pn; } } while (0)
    uint64_t pt = 0;
    PT_MARK(-1, pt);
    if (timing && !(++stage_calls % 30)) {
        fprintf(stderr, "[VULKAN PRESENT TIMING] ms/30: fence=%llu copy=%llu "
                "acquire=%llu submit=%llu present=%llu\n",
                (unsigned long long)(stage_ns[0] / 1000000),
                (unsigned long long)(stage_ns[1] / 1000000),
                (unsigned long long)(stage_ns[2] / 1000000),
                (unsigned long long)(stage_ns[3] / 1000000),
                (unsigned long long)(stage_ns[4] / 1000000));
        memset(stage_ns, 0, sizeof stage_ns);
    }
    if (!pixels || width <= 0 || height <= 0 || row_pitch < width * 4 ||
        output_width <= 0 || output_height <= 0)
        return 0;
    if (beer_vulkan_device_lost()) return 0;
    if (!initialize(display, window)) return 0;
    /* The staging buffer and upload image are sized for the source extent, so
     * a source of a different size than the previous present (a 1920x1080 movie
     * after 2560x1440 game frames) must resize them too; copying with the old
     * extent reads the tightly packed rows at the wrong stride and tiles the
     * picture. */
    if (!g_vk.swapchain || g_vk.extent.width != (uint32_t)output_width ||
        g_vk.extent.height != (uint32_t)output_height ||
        g_vk.upload_extent.width != (uint32_t)width ||
        g_vk.upload_extent.height != (uint32_t)height) {
        if (!recreate_swapchain(output_width, output_height, width, height)) return 0;
    }

    /* Presentation is asynchronous. Wait only when this frame reuses the
     * single staging buffer/command buffer from the preceding submission,
     * rather than blocking again immediately after QueuePresentKHR. This lets
     * guest frame construction overlap the compositor's presentation work. */
    VkResult fence_result = g_vk.WaitForFences(
        g_vk.device, 1, &g_vk.submit_fence, VK_TRUE, UINT64_MAX);
    PT_MARK(0, pt);
    if (fence_result != VK_SUCCESS) {
        beer_vulkan_record_result(fence_result, "presenter fence wait");
        return 0;
    }
    vulkan_post_collect();

    size_t bytes = (size_t)width * (size_t)height * 4u;
    uint64_t shared_handle = 0;
    /* A compatibility resource can temporarily exist in more than one mirror
     * cache. The generic D3D12 path synchronously publishes its authoritative
     * CPU readback, but an older indexed clear can coincidentally advertise the
     * same resource/serial. Allow correctness profiles to bypass that ambiguous
     * zero-copy lookup and upload the supplied authoritative pixels instead. */
    int direct_gpu_mirror = !getenv("BEER_PRESENTER_CPU_STAGING") &&
        row_pitch == width * 4 && resource &&
        (vulkan_renderer_get_target_buffer(resource, serial, bytes,
                                           &shared_handle) ||
         vulkan_indexed_renderer_get_target_buffer(resource, serial, bytes,
                                                    &shared_handle));
    VkBuffer frame_buffer = direct_gpu_mirror
        ? (VkBuffer)(uintptr_t)shared_handle : g_vk.staging_buffer;
    if (!direct_gpu_mirror)
        copy_rgba8_to_staging(pixels, width, height, row_pitch);
    static uint64_t gpu_mirror_presents;
    if (direct_gpu_mirror) {
        uint64_t count = ++gpu_mirror_presents;
        if (getenv("BEER_RENDER_DIAGNOSTICS") &&
            (count <= 4 || (count % 256u) == 0))
            fprintf(stderr, "[VULKAN] presenter source=compositor-mirror-direct "
                    "count=%llu serial=%llu bytes=%zu\n",
                    (unsigned long long)count,
                    (unsigned long long)serial, bytes);
    }
    uint32_t image_index = 0;
    PT_MARK(1, pt);
    /* A compositor that withholds swapchain images (occluded or throttled
     * XWayland surface) must not stall the guest: vkAcquireNextImageKHR with
     * an unbounded timeout blocked about 850 ms per frame. Drop the frame
     * instead; the fence and staging state are untouched so the next present
     * proceeds normally. BEER_PRESENT_ACQUIRE_TIMEOUT_MS=0 restores the
     * unbounded wait. */
    static uint64_t acquire_timeout_ns = UINT64_MAX - 1;
    static uint64_t dropped_presents;
    if (acquire_timeout_ns == UINT64_MAX - 1) {
        const char *env = getenv("BEER_PRESENT_ACQUIRE_TIMEOUT_MS");
        long ms = env ? atol(env) : 50;
        acquire_timeout_ns = ms > 0 ? (uint64_t)ms * 1000000ull : UINT64_MAX;
    }
    VkResult result = g_vk.AcquireNextImageKHR(g_vk.device, g_vk.swapchain,
                                                acquire_timeout_ns,
                                                g_vk.image_available,
                                                VK_NULL_HANDLE, &image_index);
    PT_MARK(2, pt);
    if (result == VK_TIMEOUT || result == VK_NOT_READY) {
        ++dropped_presents;
        if (!(dropped_presents % 64) || dropped_presents == 1)
            fprintf(stderr, "[VULKAN] present dropped: swapchain image not "
                    "available within %llu ms (total %llu)\n",
                    (unsigned long long)(acquire_timeout_ns / 1000000ull),
                    (unsigned long long)dropped_presents);
        return 1;
    }
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        return recreate_swapchain(output_width, output_height, width, height);
    }
    if (result != VK_SUCCESS) {
        beer_vulkan_record_result(result, "swapchain image acquisition");
        return 0;
    }

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
    if (direct_gpu_mirror) {
        /* The source buffer was written by the fullscreen compute stage on
         * this shared queue. Submission order alone is not a memory
         * dependency: make those shader writes visible to the transfer read
         * before copying into the upload image. */
        VkMemoryBarrier source_ready = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT
        };
        g_vk.CmdPipelineBarrier(g_vk.command_buffer,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
            1, &source_ready, 0, NULL, 0, NULL);
    }
    g_vk.CmdPipelineBarrier(g_vk.command_buffer,
        (g_vk.upload_initialized || g_vk.image_initialized[image_index])
            ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT
            : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, barriers);
    VkBufferImageCopy copy = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { g_vk.upload_extent.width, g_vk.upload_extent.height, 1 }
    };
    g_vk.CmdCopyBufferToImage(g_vk.command_buffer, frame_buffer,
                              g_vk.upload_image,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    int overlay_blended = 0;
    VkImage processed = vulkan_post_apply(g_vk.command_buffer, g_vk.upload_image, (uint32_t)width, (uint32_t)height,
                                          g_vk.extent.width, g_vk.extent.height, &overlay_blended);
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
    g_vk.CmdBlitImage(g_vk.command_buffer, processed ? processed : g_vk.upload_image,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      g_vk.images[image_index],
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      1, &blit, VK_FILTER_LINEAR);
    if (!overlay_blended) record_overlay(g_vk.images[image_index]);
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
    /* Reset only once the frame is fully acquired and recorded.  Resetting
     * before AcquireNextImageKHR/recreation left this fence permanently
     * unsignaled on early returns, freezing the next Present (and MangoHud). */
    if (g_vk.ResetFences(g_vk.device, 1, &g_vk.submit_fence) != VK_SUCCESS)
        return 0;
    beer_vulkan_queue_lock();
    VkResult submit_result = g_vk.QueueSubmit(g_vk.queue, 1, &submit,
                                               g_vk.submit_fence);
    beer_vulkan_queue_unlock();
    PT_MARK(3, pt);
    if (submit_result != VK_SUCCESS) {
        beer_vulkan_record_result(submit_result, "presenter queue submission");
        return 0;
    }
    VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &g_vk.render_finished,
        .swapchainCount = 1,
        .pSwapchains = &g_vk.swapchain,
        .pImageIndices = &image_index
    };
    /* Queue submissions and presentation each require external host
     * synchronization, but they do not need one uninterrupted critical
     * section. QueuePresentKHR may block for the compositor/vblank; retaining
     * the process-wide queue mutex across it starved D3D compute and graphics
     * submissions for tens of milliseconds. The render-finished semaphore
     * preserves the submit->present dependency even when another queue call is
     * interleaved between these two API calls. */
    beer_vulkan_queue_lock();
    result = g_vk.QueuePresentKHR(g_vk.queue, &present);
    beer_vulkan_queue_unlock();
    PT_MARK(4, pt);
    beer_vulkan_record_result(result, "queue presentation");
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

int vulkan_presenter_present_image(void *display, unsigned long window,
                                   uint64_t source_image, uint32_t source_layout,
                                   int width, int height, int direct_blit,
                                   int output_width, int output_height)
{
    static uint64_t present_sequence;
    const uint64_t sequence = ++present_sequence;
    VkImage source = (VkImage)(uintptr_t)source_image;
    if (beer_vulkan_device_lost()) return 0;
    if (getenv("BEER_PRESENT_SKIP_ALL")) return 1;
    record_direct_present(sequence, "enter", source_image, source_layout,
                          width, height, output_width, output_height, -1,
                          VK_SUCCESS);
    if (!source || width <= 0 || height <= 0 || output_width <= 0 ||
        output_height <= 0)
        return 0;
    if (beer_vulkan_device_lost()) return 0;
    if (!initialize(display, window)) return 0;
    if (!g_vk.swapchain || g_vk.extent.width != (uint32_t)output_width ||
        g_vk.extent.height != (uint32_t)output_height ||
        g_vk.upload_extent.width != (uint32_t)width ||
        g_vk.upload_extent.height != (uint32_t)height) {
        if (!recreate_swapchain(output_width, output_height, width, height))
            return 0;
    }
    /* The direct-image callback must not retain the guest image after it
     * returns: D3D12 may release a splash/title-transition back buffer
     * immediately after Present.  Bound waits so a wedged GPU cannot hang the
     * guest forever; the same timeout is also used below to consume this
     * frame's source image before returning. */
    static uint64_t source_wait_timeout_ns = UINT64_MAX;
    if (source_wait_timeout_ns == UINT64_MAX) {
        const char *env = getenv("BEER_PRESENT_SOURCE_WAIT_TIMEOUT_MS");
        long ms = env ? atol(env) : 5000;
        source_wait_timeout_ns = ms > 0 ? (uint64_t)ms * 1000000ull :
                                         5000000000ull;
    }
    VkResult fence_result = g_vk.WaitForFences(
        g_vk.device, 1, &g_vk.submit_fence, VK_TRUE, source_wait_timeout_ns);
    if (fence_result != VK_SUCCESS) {
        if (fence_result == VK_TIMEOUT)
            fprintf(stderr, "[VULKAN] timed out waiting for the previous "
                    "direct-image presentation\n");
        beer_vulkan_record_result(fence_result, "presenter fence wait");
        return 0;
    }
    vulkan_post_collect();
    static uint64_t acquire_timeout_ns = UINT64_MAX - 1;
    if (acquire_timeout_ns == UINT64_MAX - 1) {
        const char *env = getenv("BEER_PRESENT_ACQUIRE_TIMEOUT_MS");
        long ms = env ? atol(env) : 50;
        acquire_timeout_ns = ms > 0 ? (uint64_t)ms * 1000000ull : UINT64_MAX;
    }
    uint32_t image_index = 0;
    VkResult result = g_vk.AcquireNextImageKHR(g_vk.device, g_vk.swapchain,
                                                acquire_timeout_ns,
                                                g_vk.image_available,
                                                VK_NULL_HANDLE, &image_index);
    if (result == VK_TIMEOUT || result == VK_NOT_READY) return 1;
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        return recreate_swapchain(output_width, output_height, width, height);
    if (result != VK_SUCCESS) {
        record_direct_present(sequence, "acquire-failed", source_image,
                              source_layout, width, height, output_width,
                              output_height, -1, result);
        beer_vulkan_record_result(result, "swapchain image acquisition");
        return 0;
    }
    record_direct_present(sequence, "acquired", source_image, source_layout,
                          width, height, output_width, output_height,
                          (int)image_index, result);

    g_vk.ResetCommandBuffer(g_vk.command_buffer, 0);
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    if (g_vk.BeginCommandBuffer(g_vk.command_buffer, &begin) != VK_SUCCESS)
        return 0;
    VkImageSubresourceRange color = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageMemoryBarrier to_read[3] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = (VkImageLayout)source_layout,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = source, .subresourceRange = color
        },
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
            .image = g_vk.upload_image, .subresourceRange = color
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
            .image = g_vk.images[image_index], .subresourceRange = color
        }
    };
    /* The barrier scope includes every earlier submission on this queue, so
     * the D3D12 frame's writes are visible to the transfer read below. */
    g_vk.CmdPipelineBarrier(g_vk.command_buffer,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
        0, NULL, 0, NULL, direct_blit ? 1 : 3, to_read);
    if (direct_blit)
        g_vk.CmdPipelineBarrier(g_vk.command_buffer,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, NULL, 0, NULL, 1, &to_read[2]);

    int overlay_blended = 0;
    VkImage blit_source = source;
    VkImageLayout blit_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (!direct_blit) {
        VkImageCopy copy = {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .extent = { (uint32_t)width, (uint32_t)height, 1 }
        };
        g_vk.CmdCopyImage(g_vk.command_buffer, source,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_vk.upload_image,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        VkImage processed = vulkan_post_apply(g_vk.command_buffer, g_vk.upload_image, (uint32_t)width, (uint32_t)height,
                                              g_vk.extent.width, g_vk.extent.height, &overlay_blended);
        VkImageMemoryBarrier upload_read = to_read[1];
        upload_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        upload_read.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        upload_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        upload_read.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        g_vk.CmdPipelineBarrier(g_vk.command_buffer,
                                VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                0, NULL, 0, NULL, 1, &upload_read);
        blit_source = processed ? processed : g_vk.upload_image;
    }
    VkImageBlit blit = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffsets = { { 0, 0, 0 }, { width, height, 1 } },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets = { { 0, 0, 0 },
                        { (int32_t)g_vk.extent.width,
                          (int32_t)g_vk.extent.height, 1 } }
    };
    g_vk.CmdBlitImage(g_vk.command_buffer, blit_source, blit_layout,
                      g_vk.images[image_index],
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      1, &blit, VK_FILTER_LINEAR);
    if (!overlay_blended) record_overlay(g_vk.images[image_index]);
    VkImageMemoryBarrier after[2] = { to_read[2], to_read[0] };
    after[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    after[0].dstAccessMask = 0;
    after[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    after[0].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    /* Return the D3D12 image to the layout its state tracking expects. */
    after[1].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    after[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    after[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    after[1].newLayout = (VkImageLayout)source_layout;
    g_vk.CmdPipelineBarrier(g_vk.command_buffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, 0, NULL, 0, NULL, 2, after);
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
        .pSignalSemaphores = &g_vk.render_finished_image[image_index]
    };
    if (g_vk.ResetFences(g_vk.device, 1, &g_vk.submit_fence) != VK_SUCCESS)
        return 0;
    beer_vulkan_queue_lock();
    VkResult submit_result = g_vk.QueueSubmit(g_vk.queue, 1, &submit,
                                               g_vk.submit_fence);
    beer_vulkan_queue_unlock();
    record_direct_present(sequence, "submitted", source_image, source_layout,
                          width, height, output_width, output_height,
                          (int)image_index, submit_result);
    if (submit_result != VK_SUCCESS) {
        beer_vulkan_record_result(submit_result, "presenter queue submission");
        return 0;
    }
    /* The blit references a guest-owned VkImage but is not waited on here:
     * the next present waits for it (above), and the D3D12 layer calls
     * vulkan_presenter_wait_source() before destroying any resource. This
     * lets the CPU record the next frame while the GPU finishes this one. */
    VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &g_vk.render_finished_image[image_index],
        .swapchainCount = 1,
        .pSwapchains = &g_vk.swapchain,
        .pImageIndices = &image_index
    };
    beer_vulkan_queue_lock();
    result = getenv("BEER_PRESENT_SKIP_FLIP") ? VK_SUCCESS :
             g_vk.QueuePresentKHR(g_vk.queue, &present);
    beer_vulkan_queue_unlock();
    record_direct_present(sequence, "presented", source_image, source_layout,
                          width, height, output_width, output_height,
                          (int)image_index, result);
    beer_vulkan_record_result(result, "queue presentation");
    if (!direct_blit) g_vk.upload_initialized = 1;
    g_vk.image_initialized[image_index] = 1;
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        return recreate_swapchain(output_width, output_height, width, height);
    return result == VK_SUCCESS;
}

int vulkan_presenter_is_active(void)
{
    return g_vk.initialized;
}

/* Blocks until the last direct-image blit has finished reading its source. */
int vulkan_presenter_wait_source(void)
{
    if (!g_vk.initialized || !g_vk.submit_fence) return 1;
    return g_vk.WaitForFences(g_vk.device, 1, &g_vk.submit_fence, VK_TRUE,
                              5000000000ull) == VK_SUCCESS;
}
