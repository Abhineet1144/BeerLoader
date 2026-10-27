/* Vulkan-on-Vulkan mapper.
 *
 * A native Vulkan guest owns its VkInstance/VkDevice (its extension and feature choices cannot be squeezed onto the
 * loader's shared device), so the mapper sits between the guest and the driver and acts on the calls that matter to
 * Beer's manager features:
 *   - vkQueuePresentKHR is the frame boundary: it feeds the frame counter, the overlay's frame-time graph and the
 *     live frame cap, and when effects, the overlay or a screenshot are active it runs the same final-image compute
 *     passes the Direct3D 12 presenter uses (vulkan_post.c) on the swapchain image before it is presented.
 *   - vkCreateSwapchainKHR adds the transfer usages those passes need; the swapchain format is never changed.
 *     8-bit RGBA/BGRA images are read and written back with copies and blits that never convert colour (sRGB
 *     BGRA goes through a UNORM staging image), so the passes see the stored bytes in RGBA order.
 *   - physical-device queries report the spoofed GPU identity (BEER_SPOOF_GPU_*), like DXGI does.
 * Everything else is forwarded untouched by the bridge. */
#define _GNU_SOURCE
#include "vulkan_map.h"
#include "vulkan_bridge.h"
#include "vulkan_context.h"
#include "vulkan_post.h"
#include "../../platform/beer_control.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MS_ABI __attribute__((ms_abi))
#define MAX_QUEUES 32
#define RING 2

typedef struct Swap Swap;

/* BEER_VKBRIDGE_TRACE=2 also logs the calls the mapper handles itself (3: with the guest code around each call site). */
void beer_vkbridge_trace_call(const char *name, uintptr_t guest_return);
#define trace(name) do { } while (0)   /* every call is logged by the bridge */

typedef struct {
    VkDevice device;
    VkPhysicalDevice physical;
    VkInstance instance;
    struct { VkQueue queue; uint32_t family; } queues[MAX_QUEUES];
    uint32_t queue_count;
    PFN_vkQueuePresentKHR QueuePresentKHR;
    PFN_vkCreateSwapchainKHR CreateSwapchainKHR;
    PFN_vkDestroySwapchainKHR DestroySwapchainKHR;
    PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR;
    PFN_vkGetDeviceQueue GetDeviceQueue;
    PFN_vkGetDeviceQueue2 GetDeviceQueue2;
    PFN_vkDestroyDevice DestroyDevice;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkCreateSemaphore CreateSemaphore;
    PFN_vkDestroySemaphore DestroySemaphore;
    PFN_vkCreateFence CreateFence;
    PFN_vkDestroyFence DestroyFence;
    PFN_vkWaitForFences WaitForFences;
    PFN_vkResetFences ResetFences;
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkResetCommandBuffer ResetCommandBuffer;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkCmdBlitImage CmdBlitImage;
    PFN_vkCmdCopyImage CmdCopyImage;
    PFN_vkCreateImage CreateImage;
    PFN_vkDestroyImage DestroyImage;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkBindImageMemory BindImageMemory;
    PFN_vkDeviceWaitIdle DeviceWaitIdle;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
} Dev;

typedef enum { MODE_OFF = 0, MODE_COPY, MODE_BLIT, MODE_BLIT_RAW } SwapMode;   /* see mode_for_format */

struct Swap {
    VkSwapchainKHR handle;
    Dev *dev;
    VkFormat format;
    VkExtent2D extent;
    SwapMode mode;
    int ready, failed;
    uint32_t family;
    uint32_t count;
    VkImage *images;
    VkSemaphore *done;                  /* per image: the present waits on our work */
    VkImage scratch;                    /* RGBA8 copy of the frame the passes work on */
    VkDeviceMemory scratch_memory;
    VkImage stage;                      /* BGRA8 UNORM staging image (MODE_BLIT_RAW only) */
    VkDeviceMemory stage_memory;
    VkCommandPool pool;
    struct { VkCommandBuffer cmd; VkFence fence; int submitted; } slot[RING];
    unsigned frame;
    Swap *next;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static Dev *g_devs[8];
static Swap *g_swaps;
static VkInstance g_instance;
static int g_post_bound;

/* ---- registry ---- */
static Dev *dev_find(VkDevice device)
{
    for (size_t i = 0; i < sizeof(g_devs) / sizeof(*g_devs); ++i)
        if (g_devs[i] && g_devs[i]->device == device) return g_devs[i];
    return NULL;
}

static Dev *dev_for_queue(VkQueue queue, uint32_t *family)
{
    for (size_t i = 0; i < sizeof(g_devs) / sizeof(*g_devs); ++i) {
        Dev *d = g_devs[i];
        for (uint32_t q = 0; d && q < d->queue_count; ++q)
            if (d->queues[q].queue == queue) { if (family) *family = d->queues[q].family; return d; }
    }
    return NULL;
}

static void dev_note_queue(Dev *d, VkQueue queue, uint32_t family)
{
    pthread_mutex_lock(&g_lock);
    for (uint32_t q = 0; q < d->queue_count; ++q)
        if (d->queues[q].queue == queue) { pthread_mutex_unlock(&g_lock); return; }
    if (d->queue_count < MAX_QUEUES) {
        d->queues[d->queue_count].queue = queue;
        d->queues[d->queue_count].family = family;
        ++d->queue_count;
    }
    pthread_mutex_unlock(&g_lock);
}

void beer_vkmap_instance_created(VkInstance instance)
{
    g_instance = instance;
}

#define LOADD(field) d->field = (PFN_vk##field)beer_vkbridge_host_device_proc(device, "vk" #field)

void beer_vkmap_device_created(VkPhysicalDevice physical, VkDevice device)
{
    Dev *d = calloc(1, sizeof(*d));
    if (!d) return;
    d->device = device;
    d->physical = physical;
    d->instance = g_instance;
    LOADD(QueuePresentKHR); LOADD(CreateSwapchainKHR); LOADD(DestroySwapchainKHR); LOADD(GetSwapchainImagesKHR);
    LOADD(GetDeviceQueue); LOADD(GetDeviceQueue2); LOADD(DestroyDevice); LOADD(QueueSubmit);
    LOADD(CreateSemaphore); LOADD(DestroySemaphore); LOADD(CreateFence); LOADD(DestroyFence); LOADD(WaitForFences);
    LOADD(ResetFences); LOADD(CreateCommandPool); LOADD(DestroyCommandPool); LOADD(AllocateCommandBuffers);
    LOADD(BeginCommandBuffer); LOADD(EndCommandBuffer); LOADD(ResetCommandBuffer); LOADD(CmdPipelineBarrier);
    LOADD(CmdBlitImage); LOADD(CmdCopyImage); LOADD(CreateImage); LOADD(DestroyImage);
    LOADD(GetImageMemoryRequirements); LOADD(AllocateMemory); LOADD(FreeMemory); LOADD(BindImageMemory);
    LOADD(DeviceWaitIdle);
    d->GetPhysicalDeviceMemoryProperties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)beer_vkbridge_host_symbol("vkGetPhysicalDeviceMemoryProperties");
    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < sizeof(g_devs) / sizeof(*g_devs); ++i)
        if (!g_devs[i]) { g_devs[i] = d; d = NULL; break; }
    pthread_mutex_unlock(&g_lock);
    free(d);
}

/* ---- GPU identity ---- */
static void spoof_properties(VkPhysicalDeviceProperties *p)
{
    char name[256];
    uint32_t vendor = p->vendorID, device = p->deviceID;
    const int mask = beer_vulkan_gpu_spoof(name, sizeof(name), &vendor, &device, NULL);
    if (mask & 1) snprintf(p->deviceName, sizeof(p->deviceName), "%s", name);
    p->vendorID = vendor;
    p->deviceID = device;
}

/* Windows engines pair a Vulkan device with its DXGI adapter by LUID (they enumerate DXGI adapters and look for the
 * one whose LUID equals VkPhysicalDeviceIDProperties::deviceLUID).  Linux drivers report no LUID, so report the
 * one DXGI gives the same GPU. */
static void spoof_luid(VkPhysicalDeviceProperties2 *props)
{
    BeerHostGpuInfo gpu;
    if (!beer_vulkan_host_gpu_info(&gpu)) return;
    for (VkBaseOutStructure *n = (VkBaseOutStructure *)props->pNext; n; n = n->pNext) {
        uint8_t *luid = NULL;
        VkBool32 *valid = NULL;
        uint32_t *node_mask = NULL;
        if (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES) {
            VkPhysicalDeviceIDProperties *id = (VkPhysicalDeviceIDProperties *)n;
            luid = id->deviceLUID; valid = &id->deviceLUIDValid; node_mask = &id->deviceNodeMask;
        } else if (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES) {
            VkPhysicalDeviceVulkan11Properties *v = (VkPhysicalDeviceVulkan11Properties *)n;
            luid = v->deviceLUID; valid = &v->deviceLUIDValid; node_mask = &v->deviceNodeMask;
        }
        if (!luid) continue;
        memcpy(luid, &gpu.luid, 8);
        *valid = VK_TRUE;
        *node_mask = 1;
    }
}

static void MS_ABI map_GetPhysicalDeviceProperties(VkPhysicalDevice physical, VkPhysicalDeviceProperties *out)
{
    trace("vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceProperties host =
        (PFN_vkGetPhysicalDeviceProperties)beer_vkbridge_host_symbol("vkGetPhysicalDeviceProperties");
    host(physical, out);
    spoof_properties(out);
}

static void MS_ABI map_GetPhysicalDeviceProperties2(VkPhysicalDevice physical, VkPhysicalDeviceProperties2 *out)
{
    trace("vkGetPhysicalDeviceProperties2");
    PFN_vkGetPhysicalDeviceProperties2 host =
        (PFN_vkGetPhysicalDeviceProperties2)beer_vkbridge_host_symbol("vkGetPhysicalDeviceProperties2");
    if (!host) host = (PFN_vkGetPhysicalDeviceProperties2)beer_vkbridge_host_instance_proc(g_instance, "vkGetPhysicalDeviceProperties2KHR");
    host(physical, out);
    /* Only the GPU DXGI describes gets its LUID: a machine can also expose software rasterisers. */
    uint32_t real_vendor = 0, real_device = 0;
    const int is_host_gpu = beer_vulkan_real_gpu_ids(&real_vendor, &real_device) &&
                            out->properties.vendorID == real_vendor && out->properties.deviceID == real_device;
    spoof_properties(&out->properties);
    if (is_host_gpu) spoof_luid(out);
    if (getenv("BEER_VKBRIDGE_TRACE"))
        fprintf(stderr, "[VKMAP] physical device %p \"%s\" %04x:%04x type %d host-gpu=%d\n", (void *)physical,
                out->properties.deviceName, out->properties.vendorID, out->properties.deviceID,
                (int)out->properties.deviceType, is_host_gpu);
}

static void spoof_memory(VkPhysicalDeviceMemoryProperties *m)
{
    uint64_t vram = 0;
    if (!(beer_vulkan_gpu_spoof(NULL, 0, NULL, NULL, &vram) & 8)) return;
    int best = -1;
    for (uint32_t i = 0; i < m->memoryHeapCount; ++i)
        if ((m->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
            (best < 0 || m->memoryHeaps[i].size > m->memoryHeaps[best].size)) best = (int)i;
    if (best >= 0) m->memoryHeaps[best].size = vram;
}

static void MS_ABI map_GetPhysicalDeviceMemoryProperties(VkPhysicalDevice physical, VkPhysicalDeviceMemoryProperties *out)
{
    trace("vkGetPhysicalDeviceMemoryProperties");
    PFN_vkGetPhysicalDeviceMemoryProperties host =
        (PFN_vkGetPhysicalDeviceMemoryProperties)beer_vkbridge_host_symbol("vkGetPhysicalDeviceMemoryProperties");
    host(physical, out);
    spoof_memory(out);
}

static void MS_ABI map_GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physical, VkPhysicalDeviceMemoryProperties2 *out)
{
    trace("vkGetPhysicalDeviceMemoryProperties2");
    PFN_vkGetPhysicalDeviceMemoryProperties2 host =
        (PFN_vkGetPhysicalDeviceMemoryProperties2)beer_vkbridge_host_symbol("vkGetPhysicalDeviceMemoryProperties2");
    if (!host) host = (PFN_vkGetPhysicalDeviceMemoryProperties2)beer_vkbridge_host_instance_proc(g_instance, "vkGetPhysicalDeviceMemoryProperties2KHR");
    host(physical, out);
    spoof_memory(&out->memoryProperties);
}

/* ---- queues ---- */
static void MS_ABI map_GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue *queue)
{
    trace("vkGetDeviceQueue");
    Dev *d = dev_find(device);
    PFN_vkGetDeviceQueue host = d ? d->GetDeviceQueue : (PFN_vkGetDeviceQueue)beer_vkbridge_host_device_proc(device, "vkGetDeviceQueue");
    host(device, family, index, queue);
    if (d && queue && *queue) dev_note_queue(d, *queue, family);
}

static void MS_ABI map_GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2 *info, VkQueue *queue)
{
    trace("vkGetDeviceQueue2");
    Dev *d = dev_find(device);
    PFN_vkGetDeviceQueue2 host = d ? d->GetDeviceQueue2 : (PFN_vkGetDeviceQueue2)beer_vkbridge_host_device_proc(device, "vkGetDeviceQueue2");
    host(device, info, queue);
    if (d && info && queue && *queue) dev_note_queue(d, *queue, info->queueFamilyIndex);
}

/* ---- swapchains ---- */
static SwapMode mode_for_format(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: return MODE_COPY;   /* raw bytes, same order */
    case VK_FORMAT_B8G8R8A8_UNORM: return MODE_BLIT;                                  /* the blit swizzles */
    case VK_FORMAT_B8G8R8A8_SRGB: return MODE_BLIT_RAW;                               /* swizzle without sRGB decode */
    default: return MODE_OFF;                                                         /* HDR, 10-bit, float */
    }
}

static Swap *swap_find(VkSwapchainKHR handle)
{
    for (Swap *s = g_swaps; s; s = s->next)
        if (s->handle == handle) return s;
    return NULL;
}

static VkResult MS_ABI map_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *info, const void *allocator,
                                              VkSwapchainKHR *out)
{
    trace("vkCreateSwapchainKHR");
    (void)allocator;
    Dev *d = dev_find(device);
    if (!d) {
        PFN_vkCreateSwapchainKHR host = (PFN_vkCreateSwapchainKHR)beer_vkbridge_host_device_proc(device, "vkCreateSwapchainKHR");
        return host ? host(device, info, NULL, out) : VK_ERROR_INITIALIZATION_FAILED;
    }
    VkSwapchainCreateInfoKHR copy = *info;
    SwapMode mode = getenv("BEER_NO_POST") ? MODE_OFF : mode_for_format(info->imageFormat);
    if (mode != MODE_OFF) {
        /* The passes read and write the image with transfer operations: only ask for what the surface offers. */
        PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR caps_fn = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)
            beer_vkbridge_host_instance_proc(d->instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        VkSurfaceCapabilitiesKHR caps;
        const VkImageUsageFlags need = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (caps_fn && caps_fn(d->physical, info->surface, &caps) == VK_SUCCESS &&
            (caps.supportedUsageFlags & need) == need)
            copy.imageUsage |= need;
        else
            mode = MODE_OFF;
    }
    VkResult r = d->CreateSwapchainKHR(device, &copy, NULL, out);
    if (r != VK_SUCCESS && mode != MODE_OFF) {
        /* The driver refused the extra usage: give the guest exactly what it asked for, without effects. */
        mode = MODE_OFF;
        r = d->CreateSwapchainKHR(device, info, NULL, out);
    }
    if (r != VK_SUCCESS) {
        fprintf(stderr, "[VKMAP] vkCreateSwapchainKHR failed: %d (%ux%u format %d colorspace %d usage 0x%x images %u present mode %d "
                "surface %p flags 0x%x sharing %d transform 0x%x alpha 0x%x)\n", (int)r, info->imageExtent.width,
                info->imageExtent.height, (int)info->imageFormat, (int)info->imageColorSpace, info->imageUsage,
                info->minImageCount, (int)info->presentMode, (void *)(uintptr_t)info->surface, info->flags,
                (int)info->imageSharingMode, (unsigned)info->preTransform, (unsigned)info->compositeAlpha);
        return r;
    }
    Swap *s = calloc(1, sizeof(*s));
    if (s) {
        s->handle = *out;
        s->dev = d;
        s->format = info->imageFormat;
        s->extent = info->imageExtent;
        s->mode = mode;
        pthread_mutex_lock(&g_lock);
        s->next = g_swaps;
        g_swaps = s;
        pthread_mutex_unlock(&g_lock);
    }
    fprintf(stderr, "[VKMAP] swapchain %ux%u format %d -> overlay/effects %s\n", info->imageExtent.width,
            info->imageExtent.height, (int)info->imageFormat, mode == MODE_OFF ? "unavailable" : "available");
    return r;
}

static void swap_release(Swap *s)
{
    Dev *d = s->dev;
    if (s->ready) {
        d->DeviceWaitIdle(d->device);
        vulkan_post_forget_upload();
        for (int i = 0; i < RING; ++i)
            if (s->slot[i].fence) d->DestroyFence(d->device, s->slot[i].fence, NULL);
        if (s->pool) d->DestroyCommandPool(d->device, s->pool, NULL);
        for (uint32_t i = 0; i < s->count; ++i)
            if (s->done && s->done[i]) d->DestroySemaphore(d->device, s->done[i], NULL);
        if (s->scratch) d->DestroyImage(d->device, s->scratch, NULL);
        if (s->scratch_memory) d->FreeMemory(d->device, s->scratch_memory, NULL);
        if (s->stage) d->DestroyImage(d->device, s->stage, NULL);
        if (s->stage_memory) d->FreeMemory(d->device, s->stage_memory, NULL);
    }
    free(s->images);
    free(s->done);
    free(s);
}

static void MS_ABI map_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const void *allocator)
{
    trace("vkDestroySwapchainKHR");
    (void)allocator;
    Dev *d = dev_find(device);
    pthread_mutex_lock(&g_lock);
    Swap **link = &g_swaps;
    while (*link && (*link)->handle != swapchain) link = &(*link)->next;
    Swap *s = *link;
    if (s) *link = s->next;
    pthread_mutex_unlock(&g_lock);
    if (s) swap_release(s);
    PFN_vkDestroySwapchainKHR host = d ? d->DestroySwapchainKHR : (PFN_vkDestroySwapchainKHR)beer_vkbridge_host_device_proc(device, "vkDestroySwapchainKHR");
    host(device, swapchain, NULL);
}

static void MS_ABI map_DestroyDevice(VkDevice device, const void *allocator)
{
    trace("vkDestroyDevice");
    (void)allocator;
    pthread_mutex_lock(&g_lock);
    Dev *d = NULL;
    for (size_t i = 0; i < sizeof(g_devs) / sizeof(*g_devs); ++i)
        if (g_devs[i] && g_devs[i]->device == device) { d = g_devs[i]; g_devs[i] = NULL; }
    Swap *mine = NULL, **link = &g_swaps;
    while (*link) {
        if ((*link)->dev == d) { Swap *s = *link; *link = s->next; s->next = mine; mine = s; }
        else link = &(*link)->next;
    }
    pthread_mutex_unlock(&g_lock);
    while (mine) { Swap *s = mine; mine = s->next; swap_release(s); }
    PFN_vkDestroyDevice host = d ? d->DestroyDevice : (PFN_vkDestroyDevice)beer_vkbridge_host_device_proc(device, "vkDestroyDevice");
    host(device, NULL);
    free(d);
}

/* ---- final-image passes on the guest's swapchain ---- */
static int find_memory(Dev *d, uint32_t bits, VkMemoryPropertyFlags want, uint32_t *index)
{
    VkPhysicalDeviceMemoryProperties m;
    d->GetPhysicalDeviceMemoryProperties(d->physical, &m);
    for (uint32_t i = 0; i < m.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (m.memoryTypes[i].propertyFlags & want) == want) { *index = i; return 1; }
    return 0;
}

static int make_image(Dev *d, VkExtent2D extent, VkFormat format, VkImageUsageFlags usage, VkImage *image,
                      VkDeviceMemory *memory)
{
    VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = format,
        .extent = { extent.width, extent.height, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };
    VkMemoryRequirements req;
    uint32_t type;
    if (d->CreateImage(d->device, &info, NULL, image) != VK_SUCCESS) return 0;
    d->GetImageMemoryRequirements(d->device, *image, &req);
    if (!find_memory(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type)) return 0;
    VkMemoryAllocateInfo alloc = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                   .memoryTypeIndex = type };
    return d->AllocateMemory(d->device, &alloc, NULL, memory) == VK_SUCCESS &&
           d->BindImageMemory(d->device, *image, *memory, 0) == VK_SUCCESS;
}

static int swap_setup(Swap *s, uint32_t family)
{
    Dev *d = s->dev;
    VkDevice dev = d->device;
    if (!g_post_bound) {
        if (!vulkan_post_bind_device(d->instance, d->physical, dev,
                                     (PFN_vkGetInstanceProcAddr)beer_vkbridge_host_symbol("vkGetInstanceProcAddr"),
                                     (PFN_vkGetDeviceProcAddr)beer_vkbridge_host_symbol("vkGetDeviceProcAddr")))
            return 0;
        g_post_bound = 1;
    }
    uint32_t count = 0;
    if (d->GetSwapchainImagesKHR(dev, s->handle, &count, NULL) != VK_SUCCESS || count == 0) return 0;
    s->images = calloc(count, sizeof(*s->images));
    s->done = calloc(count, sizeof(*s->done));
    if (!s->images || !s->done || d->GetSwapchainImagesKHR(dev, s->handle, &count, s->images) < 0) return 0;
    s->count = count;
    s->family = family;
    s->ready = 1;                               /* from here swap_release cleans up whatever got created */

    if (!make_image(d, s->extent, VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    &s->scratch, &s->scratch_memory)) return 0;
    if (s->mode == MODE_BLIT_RAW &&
        !make_image(d, s->extent, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    &s->stage, &s->stage_memory)) return 0;

    VkCommandPoolCreateInfo pool = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                     .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family };
    if (d->CreateCommandPool(dev, &pool, NULL, &s->pool) != VK_SUCCESS) return 0;
    VkCommandBuffer cmds[RING];
    VkCommandBufferAllocateInfo cmd_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = s->pool,
                                             .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = RING };
    if (d->AllocateCommandBuffers(dev, &cmd_info, cmds) != VK_SUCCESS) return 0;
    VkFenceCreateInfo fence = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    for (int i = 0; i < RING; ++i) {
        s->slot[i].cmd = cmds[i];
        if (d->CreateFence(dev, &fence, NULL, &s->slot[i].fence) != VK_SUCCESS) return 0;
    }
    VkSemaphoreCreateInfo sem = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (uint32_t i = 0; i < count; ++i)
        if (d->CreateSemaphore(dev, &sem, NULL, &s->done[i]) != VK_SUCCESS) return 0;
    return 1;
}

static void barrier(Dev *d, VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                    VkAccessFlags src_access, VkAccessFlags dst_access, VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = src_access, .dstAccessMask = dst_access,
        .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
    };
    d->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

static void copy_image(Swap *s, VkCommandBuffer cmd, VkImage from, VkImage to)
{
    VkImageCopy region = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                           .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                           .extent = { s->extent.width, s->extent.height, 1 } };
    s->dev->CmdCopyImage(cmd, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

static void blit_image(Swap *s, VkCommandBuffer cmd, VkImage from, VkImage to)
{
    VkImageBlit region = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                           .srcOffsets = { { 0, 0, 0 }, { (int32_t)s->extent.width, (int32_t)s->extent.height, 1 } },
                           .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                           .dstOffsets = { { 0, 0, 0 }, { (int32_t)s->extent.width, (int32_t)s->extent.height, 1 } } };
    s->dev->CmdBlitImage(cmd, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                         VK_FILTER_NEAREST);
}

/* Swapchain image (TRANSFER_SRC) -> scratch (TRANSFER_DST), colour bytes untouched, channels in RGBA order. */
static void frame_in(Swap *s, VkCommandBuffer cmd, VkImage image)
{
    Dev *d = s->dev;
    if (s->mode == MODE_COPY) { copy_image(s, cmd, image, s->scratch); return; }
    if (s->mode == MODE_BLIT) { blit_image(s, cmd, image, s->scratch); return; }
    /* sRGB BGRA: a blit would decode sRGB, so move the raw bytes into a UNORM image first. */
    barrier(d, cmd, s->stage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    copy_image(s, cmd, image, s->stage);
    barrier(d, cmd, s->stage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    blit_image(s, cmd, s->stage, s->scratch);
}

/* Processed image `out` (TRANSFER_SRC) -> swapchain image (TRANSFER_DST). */
static void frame_out(Swap *s, VkCommandBuffer cmd, VkImage out, VkImage image)
{
    Dev *d = s->dev;
    if (s->mode == MODE_COPY) { copy_image(s, cmd, out, image); return; }
    if (s->mode == MODE_BLIT) { blit_image(s, cmd, out, image); return; }
    barrier(d, cmd, s->stage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    blit_image(s, cmd, out, s->stage);
    barrier(d, cmd, s->stage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    copy_image(s, cmd, s->stage, image);
}

static void wait_slot(Swap *s, int i)
{
    if (!s->slot[i].submitted) return;
    s->dev->WaitForFences(s->dev->device, 1, &s->slot[i].fence, VK_TRUE, 5000000000ull);
    s->slot[i].submitted = 0;
}

/* Records and submits the passes for one presented image.  Returns the semaphore the present must wait on, or
 * VK_NULL_HANDLE when nothing needed doing (then no work was submitted). */
static VkSemaphore post_frame(Swap *s, VkQueue queue, uint32_t family, const VkPresentInfoKHR *info)
{
    Dev *d = s->dev;
    if (s->failed || s->mode == MODE_OFF) return VK_NULL_HANDLE;
    if (!s->ready && !swap_setup(s, family)) { s->failed = 1; return VK_NULL_HANDLE; }
    const uint32_t index = info->pImageIndices[0];
    if (family != s->family || index >= s->count) return VK_NULL_HANDLE;

    const int slot = (int)(s->frame % RING);
    wait_slot(s, slot);
    if (vulkan_post_screenshot_waiting()) {          /* the buffer is only valid once every submitted pass is done */
        for (int i = 0; i < RING; ++i) wait_slot(s, i);
        vulkan_post_collect();
    }
    VkCommandBuffer cmd = s->slot[slot].cmd;
    d->ResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (d->BeginCommandBuffer(cmd, &begin) != VK_SUCCESS) return VK_NULL_HANDLE;

    /* Earlier passes of this swapchain finish before these touch the shared scratch images. */
    VkMemoryBarrier memory = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                               .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
                               .dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT };
    d->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &memory, 0,
                          NULL, 0, NULL);
    VkImage image = s->images[index];
    barrier(d, cmd, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    barrier(d, cmd, s->scratch, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    frame_in(s, cmd, image);

    int overlay_done = 0;
    VkImage out = vulkan_post_apply(cmd, s->scratch, s->extent.width, s->extent.height, s->extent.width, s->extent.height,
                                    &overlay_done);
    if (!out) {                                      /* nothing active this frame: drop the recording */
        d->EndCommandBuffer(cmd);
        return VK_NULL_HANDLE;
    }
    barrier(d, cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    frame_out(s, cmd, out, image);
    barrier(d, cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT,
            0, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    if (d->EndCommandBuffer(cmd) != VK_SUCCESS) return VK_NULL_HANDLE;

    VkPipelineStageFlags wait_stages[16];
    uint32_t waits = info->waitSemaphoreCount < 16 ? info->waitSemaphoreCount : 16;
    for (uint32_t i = 0; i < waits; ++i) wait_stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = waits,
                            .pWaitSemaphores = info->pWaitSemaphores, .pWaitDstStageMask = wait_stages,
                            .commandBufferCount = 1, .pCommandBuffers = &cmd, .signalSemaphoreCount = 1,
                            .pSignalSemaphores = &s->done[index] };
    d->ResetFences(d->device, 1, &s->slot[slot].fence);
    if (d->QueueSubmit(queue, 1, &submit, s->slot[slot].fence) != VK_SUCCESS) {
        s->failed = 1;                                /* the guest's wait semaphores were not consumed: present as-is */
        return VK_NULL_HANDLE;
    }
    s->slot[slot].submitted = 1;
    ++s->frame;
    return s->done[index];
}

static VkResult MS_ABI map_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *info)
{
    trace("vkQueuePresentKHR");
    beer_control_note_frame();                       /* frame counter, overlay graph, manager fps */
    beer_control_frame_limit_wait();                 /* live frame cap */
    uint32_t family = 0;
    Dev *d = dev_for_queue(queue, &family);
    PFN_vkQueuePresentKHR host = d ? d->QueuePresentKHR : (PFN_vkQueuePresentKHR)beer_vkbridge_host_symbol("vkQueuePresentKHR");
    if (!d || !info || info->swapchainCount != 1) return host(queue, info);
    VkSemaphore done = VK_NULL_HANDLE;
    pthread_mutex_lock(&g_lock);
    Swap *s = swap_find(info->pSwapchains[0]);
    pthread_mutex_unlock(&g_lock);                   /* the queue is externally synchronized by the guest */
    if (s) done = post_frame(s, queue, family, info);
    if (!done) return host(queue, info);
    VkPresentInfoKHR patched = *info;
    patched.waitSemaphoreCount = 1;
    patched.pWaitSemaphores = &done;
    return host(queue, &patched);
}

void *beer_vkmap_override(const char *name)
{
    static const struct { const char *name; void *fn; } table[] = {
        { "vkGetPhysicalDeviceProperties", map_GetPhysicalDeviceProperties },
        { "vkGetPhysicalDeviceProperties2", map_GetPhysicalDeviceProperties2 },
        { "vkGetPhysicalDeviceProperties2KHR", map_GetPhysicalDeviceProperties2 },
        { "vkGetPhysicalDeviceMemoryProperties", map_GetPhysicalDeviceMemoryProperties },
        { "vkGetPhysicalDeviceMemoryProperties2", map_GetPhysicalDeviceMemoryProperties2 },
        { "vkGetPhysicalDeviceMemoryProperties2KHR", map_GetPhysicalDeviceMemoryProperties2 },
        { "vkGetDeviceQueue", map_GetDeviceQueue },
        { "vkGetDeviceQueue2", map_GetDeviceQueue2 },
        { "vkCreateSwapchainKHR", map_CreateSwapchainKHR },
        { "vkDestroySwapchainKHR", map_DestroySwapchainKHR },
        { "vkDestroyDevice", map_DestroyDevice },
        { "vkQueuePresentKHR", map_QueuePresentKHR },
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(*table); ++i)
        if (!strcmp(table[i].name, name)) return table[i].fn;
    return NULL;
}
