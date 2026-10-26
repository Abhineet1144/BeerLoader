/* Freestanding Windows Vulkan guest used to exercise the Vulkan mapper (renderer/vulkan/vulkan_map.c):
 * creates a window, an instance/device/swapchain through vulkan-1.dll, then clears and presents N frames
 * with a slowly changing colour.  Prefers an sRGB swapchain when the command line contains "srgb".
 * Build with build.sh; run with: loader tests/vulkan_guest/vkclear.exe [srgb] [frames=N] */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

typedef void *HANDLE;
typedef unsigned long DWORD;
typedef struct { VkStructureType sType; const void *pNext; VkFlags flags; HANDLE hinstance; HANDLE hwnd; } Win32SurfaceInfo;
#define STRUCTURE_TYPE_WIN32_SURFACE 1000009000

#define IMPORT __declspec(dllimport)
IMPORT HANDLE __stdcall GetModuleHandleA(const char *);
IMPORT HANDLE __stdcall GetStdHandle(DWORD);
IMPORT int __stdcall WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
IMPORT const char *__stdcall GetCommandLineA(void);
IMPORT void __stdcall ExitProcess(unsigned);
IMPORT unsigned short __stdcall RegisterClassA(const void *);
IMPORT HANDLE __stdcall CreateWindowExA(DWORD, const char *, const char *, DWORD, int, int, int, int, HANDLE, HANDLE, HANDLE, void *);
IMPORT int __stdcall ShowWindow(HANDLE, int);
IMPORT int __stdcall PeekMessageA(void *, HANDLE, unsigned, unsigned, unsigned);
IMPORT int __stdcall TranslateMessage(const void *);
IMPORT long long __stdcall DispatchMessageA(const void *);
IMPORT long long __stdcall DefWindowProcA(HANDLE, unsigned, unsigned long long, long long);

#define V(name) IMPORT PFN_##name name
IMPORT VkResult vkCreateInstance(const VkInstanceCreateInfo *, const void *, VkInstance *);
IMPORT VkResult vkEnumeratePhysicalDevices(VkInstance, uint32_t *, VkPhysicalDevice *);
IMPORT void vkGetPhysicalDeviceProperties(VkPhysicalDevice, VkPhysicalDeviceProperties *);
IMPORT void vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice, uint32_t *, VkQueueFamilyProperties *);
IMPORT VkResult vkCreateWin32SurfaceKHR(VkInstance, const Win32SurfaceInfo *, const void *, VkSurfaceKHR *);
IMPORT VkResult vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32 *);
IMPORT VkResult vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice, VkSurfaceKHR, uint32_t *, VkSurfaceFormatKHR *);
IMPORT VkResult vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR *);
IMPORT VkResult vkCreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo *, const void *, VkDevice *);
IMPORT void vkGetDeviceQueue(VkDevice, uint32_t, uint32_t, VkQueue *);
IMPORT VkResult vkCreateSwapchainKHR(VkDevice, const VkSwapchainCreateInfoKHR *, const void *, VkSwapchainKHR *);
IMPORT VkResult vkGetSwapchainImagesKHR(VkDevice, VkSwapchainKHR, uint32_t *, VkImage *);
IMPORT VkResult vkCreateCommandPool(VkDevice, const VkCommandPoolCreateInfo *, const void *, VkCommandPool *);
IMPORT VkResult vkAllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo *, VkCommandBuffer *);
IMPORT VkResult vkCreateSemaphore(VkDevice, const VkSemaphoreCreateInfo *, const void *, VkSemaphore *);
IMPORT VkResult vkCreateFence(VkDevice, const VkFenceCreateInfo *, const void *, VkFence *);
IMPORT VkResult vkWaitForFences(VkDevice, uint32_t, const VkFence *, VkBool32, uint64_t);
IMPORT VkResult vkResetFences(VkDevice, uint32_t, const VkFence *);
IMPORT VkResult vkAcquireNextImageKHR(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t *);
IMPORT VkResult vkBeginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo *);
IMPORT VkResult vkEndCommandBuffer(VkCommandBuffer);
IMPORT VkResult vkResetCommandBuffer(VkCommandBuffer, VkFlags);
IMPORT void vkCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkFlags, uint32_t, const VkMemoryBarrier *,
                                 uint32_t, const VkBufferMemoryBarrier *, uint32_t, const VkImageMemoryBarrier *);
IMPORT void vkCmdClearColorImage(VkCommandBuffer, VkImage, VkImageLayout, const VkClearColorValue *, uint32_t, const VkImageSubresourceRange *);
IMPORT VkResult vkQueueSubmit(VkQueue, uint32_t, const VkSubmitInfo *, VkFence);
IMPORT VkResult vkQueuePresentKHR(VkQueue, const VkPresentInfoKHR *);
IMPORT VkResult vkDeviceWaitIdle(VkDevice);

int _fltused;   /* freestanding MSVC-target build with floating point */

/* The compiler lowers large struct initialisers to these; there is no C runtime to provide them. */
void *memset(void *dst, int value, unsigned long long n)
{
    volatile unsigned char *p = dst;
    while (n--) *p++ = (unsigned char)value;
    return dst;
}
void *memcpy(void *dst, const void *src, unsigned long long n)
{
    volatile unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

static void out(const char *text)
{
    DWORD n = 0, len = 0;
    while (text[len]) ++len;
    WriteFile(GetStdHandle((DWORD)-11), text, len, &n, 0);
}

static void out_num(const char *label, long long value)
{
    char buf[48];
    int i = 47;
    buf[i] = 0;
    int neg = value < 0;
    unsigned long long v = neg ? -value : value;
    do { buf[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    if (neg) buf[--i] = '-';
    out(label);
    out(buf + i);
    out("\n");
}

static int has_word(const char *text, const char *word)
{
    for (; *text; ++text) {
        const char *a = text, *b = word;
        while (*a && *b && *a == *b) { ++a; ++b; }
        if (!*b) return 1;
    }
    return 0;
}

static long long number_after(const char *text, const char *key, long long fallback)
{
    for (; *text; ++text) {
        const char *a = text, *b = key;
        while (*a && *b && *a == *b) { ++a; ++b; }
        if (!*b) { long long v = 0; while (*a >= '0' && *a <= '9') v = v * 10 + (*a++ - '0'); return v; }
    }
    return fallback;
}

static long long __stdcall wndproc(HANDLE h, unsigned m, unsigned long long w, long long l) { return DefWindowProcA(h, m, w, l); }

typedef struct { unsigned style; void *proc; int cls, wnd; HANDLE inst, icon, cursor, brush; const char *menu, *name; } WndClass;

#define CHECK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { out("FAILED: " #call "\n"); out_num("  result ", r_); ExitProcess(2); } } while (0)

static VkImage g_images[8];

void start(void)
{
    const char *cmd = GetCommandLineA();
    const int want_srgb = has_word(cmd, "srgb");
    const long long frames = number_after(cmd, "frames=", 300);

    static WndClass wc;
    wc.proc = (void *)wndproc;
    wc.inst = GetModuleHandleA(0);
    wc.name = "VkClearClass";
    RegisterClassA(&wc);
    HANDLE hwnd = CreateWindowExA(0, "VkClearClass", "vkclear", 0x10CF0000, 100, 100, 1280, 720, 0, 0, wc.inst, 0);
    ShowWindow(hwnd, 5);

    const char *instance_ext[] = { "VK_KHR_surface", "VK_KHR_win32_surface" };
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vkclear", .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app, .enabledExtensionCount = 2,
                                 .ppEnabledExtensionNames = instance_ext };
    VkInstance instance;
    CHECK(vkCreateInstance(&ici, 0, &instance));

    uint32_t count = 1;
    VkPhysicalDevice physical;
    VkResult enum_result = vkEnumeratePhysicalDevices(instance, &count, &physical);
    if (count == 0 || (enum_result != VK_SUCCESS && enum_result != VK_INCOMPLETE)) { out("no physical device\n"); ExitProcess(3); }
    static VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical, &props);
    out("device: "); out(props.deviceName); out("\n");
    out_num("vendor ", props.vendorID);

    Win32SurfaceInfo si = { .sType = STRUCTURE_TYPE_WIN32_SURFACE, .hinstance = wc.inst, .hwnd = hwnd };
    VkSurfaceKHR surface;
    CHECK(vkCreateWin32SurfaceKHR(instance, &si, 0, &surface));

    uint32_t families = 0, family = 0xFFFFFFFFu;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, 0);
    static VkQueueFamilyProperties fam[16];
    if (families > 16) families = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, fam);
    for (uint32_t i = 0; i < families && family == 0xFFFFFFFFu; ++i) {
        VkBool32 present = 0;
        vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, surface, &present);
        if (present && (fam[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) family = i;
    }
    if (family == 0xFFFFFFFFu) { out("no graphics+present queue\n"); ExitProcess(4); }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority };
    const char *device_ext[] = { "VK_KHR_swapchain" };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                               .enabledExtensionCount = 1, .ppEnabledExtensionNames = device_ext };
    VkDevice device;
    CHECK(vkCreateDevice(physical, &dci, 0, &device));
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);

    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count, 0);
    static VkSurfaceFormatKHR formats[32];
    if (format_count > 32) format_count = 32;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count, formats);
    VkSurfaceFormatKHR chosen = formats[0];
    for (uint32_t i = 0; i < format_count; ++i) {
        const int srgb = formats[i].format == VK_FORMAT_B8G8R8A8_SRGB || formats[i].format == VK_FORMAT_R8G8B8A8_SRGB;
        const int bgra_or_rgba = srgb || formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM;
        if (bgra_or_rgba && srgb == want_srgb) { chosen = formats[i]; break; }
    }
    out_num("swapchain format ", chosen.format);

    VkSurfaceCapabilitiesKHR caps;
    CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps));
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) { extent.width = 1280; extent.height = 720; }
    out_num("extent ", (long long)extent.width * 10000 + extent.height);
    uint32_t want_images = caps.minImageCount + 1;
    if (caps.maxImageCount && want_images > caps.maxImageCount) want_images = caps.maxImageCount;
    VkSwapchainCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surface, .minImageCount = want_images,
        .imageFormat = chosen.format, .imageColorSpace = chosen.colorSpace, .imageExtent = extent, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform, .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE
    };
    VkSwapchainKHR swapchain;
    CHECK(vkCreateSwapchainKHR(device, &sci, 0, &swapchain));
    uint32_t image_count = 8;
    CHECK(vkGetSwapchainImagesKHR(device, swapchain, &image_count, g_images));
    out_num("swapchain images ", image_count);

    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family };
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(device, &pci, 0, &pool));
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cmdbuf;
    CHECK(vkAllocateCommandBuffers(device, &cai, &cmdbuf));
    VkSemaphoreCreateInfo semi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkSemaphore acquire_sem, render_sem;
    CHECK(vkCreateSemaphore(device, &semi, 0, &acquire_sem));
    CHECK(vkCreateSemaphore(device, &semi, 0, &render_sem));
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    VkFence fence;
    CHECK(vkCreateFence(device, &fi, 0, &fence));

    for (long long frame = 0; frame < frames; ++frame) {
        static unsigned char msg[64];
        while (PeekMessageA(msg, 0, 0, 0, 1)) { TranslateMessage(msg); DispatchMessageA(msg); }
        CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, ~0ull));
        CHECK(vkResetFences(device, 1, &fence));
        uint32_t index;
        VkResult acq = vkAcquireNextImageKHR(device, swapchain, ~0ull, acquire_sem, 0, &index);
        if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) { out_num("acquire failed ", acq); ExitProcess(5); }

        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        vkResetCommandBuffer(cmdbuf, 0);
                CHECK(vkBeginCommandBuffer(cmdbuf, &bi));
        VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkImageMemoryBarrier to_dst = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = ~0u,
            .dstQueueFamilyIndex = ~0u, .image = g_images[index], .subresourceRange = range };
        vkCmdPipelineBarrier(cmdbuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &to_dst);
        /* left half red→green ramp over time, right half constant blue-ish gray, so channel order and gamma are visible */
        const float t = (float)(frame % 120) / 120.0f;
        VkClearColorValue color = { .float32 = { 1.0f - t, t, 0.25f, 1.0f } };
        vkCmdClearColorImage(cmdbuf, g_images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
        VkImageMemoryBarrier to_present = to_dst;
        to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_present.dstAccessMask = 0;
        to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(cmdbuf, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, 0, 0, 0, 1, &to_present);
        CHECK(vkEndCommandBuffer(cmdbuf));

        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &acquire_sem,
            .pWaitDstStageMask = &wait_stage, .commandBufferCount = 1, .pCommandBuffers = &cmdbuf, .signalSemaphoreCount = 1, .pSignalSemaphores = &render_sem };
        CHECK(vkQueueSubmit(queue, 1, &submit, fence));
        VkPresentInfoKHR present = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &render_sem,
            .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &index };
        VkResult pr = vkQueuePresentKHR(queue, &present);
        if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR) { out_num("present failed ", pr); ExitProcess(6); }
        if (frame % 120 == 0) out_num("frame ", frame);
    }
    vkDeviceWaitIdle(device);
    out("vkclear: done\n");
    ExitProcess(0);
}
