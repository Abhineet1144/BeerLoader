/* Guest-visible Vulkan: a built-in "vulkan-1.dll" that forwards to the host's libvulkan (through the mapper in
 * vulkan_map.c for the calls Beer acts on).
 *
 * Native Vulkan games (and engines that probe Vulkan next to Direct3D 12) load vulkan-1.dll and resolve vk*
 * entry points from it.  The shipped Windows loader cannot run here (it looks for a Windows ICD), so Beer
 * answers for it: every entry point is a small thunk that converts the Microsoft x64 calling convention of the
 * guest into the System V convention of the host driver and tail-calls the real function.  Vulkan structures
 * have the same layout on both sides, so pointers pass through untouched; only the Windows-specific pieces are
 * translated:
 *   - VK_KHR_win32_surface is advertised and implemented on top of VK_KHR_xlib_surface using the guest's
 *     native window,
 *   - other *_win32 / full-screen-exclusive extensions are hidden from instance and device creation,
 *   - validation layers and guest debug callbacks (they would have to be called back in the wrong ABI) are
 *     dropped,
 *   - allocation callbacks are ignored.
 *
 * The thunk loads the host function into r11, then jumps to a shared bridge that shuffles the argument
 * registers, copies up to eight stack arguments, preserves the registers the Microsoft ABI treats as
 * callee-saved (rsi, rdi, xmm6-xmm15) and calls the host.  Four commands take float arguments by value, whose
 * xmm registers differ between the two ABIs; they get their own entry labels. */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
typedef struct _XDisplay Display;
typedef unsigned long Window;
typedef unsigned long VisualID;
#define VK_USE_PLATFORM_XLIB_KHR
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_xlib.h>

#include "vulkan_bridge.h"
#include "vulkan_map.h"
#include "../../platform/xwayland_backend.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define MS_ABI __attribute__((ms_abi))

/* ---- The ABI bridge ---- */
__asm__(
    ".text\n"
    ".globl beer_vk_bridge_f1\n"
    ".globl beer_vk_bridge_f2\n"
    ".globl beer_vk_bridge_f3\n"
    ".globl beer_vk_bridge_g1\n"
    ".globl beer_vk_bridge\n"
    ".globl beer_vk_bridge_trace\n"
    ".type beer_vk_bridge,@function\n"
    /* Float arguments: Microsoft passes the Nth argument's float in xmmN-1, System V in the next free xmm. */
    "beer_vk_bridge_f3:\n"
    "    movaps %xmm1, %xmm0\n"
    "    movaps %xmm2, %xmm1\n"
    "    movaps %xmm3, %xmm2\n"
    "    jmp beer_vk_bridge\n"
    "beer_vk_bridge_f2:\n"
    "    movaps %xmm1, %xmm0\n"
    "    movaps %xmm2, %xmm1\n"
    "    jmp beer_vk_bridge\n"
    "beer_vk_bridge_f1:\n"
    "    movaps %xmm1, %xmm0\n"
    "    jmp beer_vk_bridge\n"
    "beer_vk_bridge_g1:\n"
    "    movaps %xmm2, %xmm0\n"
    "    jmp beer_vk_bridge\n"
    /* BEER_VKBRIDGE_TRACE=2: r10 holds the command name; log it, then continue into the bridge unchanged. */
    "beer_vk_bridge_trace:\n"
    "    pushq %rcx\n    pushq %rdx\n    pushq %r8\n    pushq %r9\n    pushq %r11\n    pushq %rsi\n    pushq %rdi\n"
    "    movq %r10, %rdi\n"
    "    callq beer_vkbridge_trace_call\n"
    "    popq %rdi\n    popq %rsi\n    popq %r11\n    popq %r9\n    popq %r8\n    popq %rdx\n    popq %rcx\n"
    "    jmp beer_vk_bridge\n"
    "beer_vk_bridge:\n"
    "    pushq %rbp\n"
    "    movq %rsp, %rbp\n"
    "    pushq %rsi\n"
    "    pushq %rdi\n"
    "    pushq %rbx\n"
    "    subq $232, %rsp\n"
    "    movups %xmm6, 64(%rsp)\n"
    "    movups %xmm7, 80(%rsp)\n"
    "    movups %xmm8, 96(%rsp)\n"
    "    movups %xmm9, 112(%rsp)\n"
    "    movups %xmm10, 128(%rsp)\n"
    "    movups %xmm11, 144(%rsp)\n"
    "    movups %xmm12, 160(%rsp)\n"
    "    movups %xmm13, 176(%rsp)\n"
    "    movups %xmm14, 192(%rsp)\n"
    "    movups %xmm15, 208(%rsp)\n"
    /* guest stack arguments 7..14 become the host's stack arguments */
    "    movq 0x40(%rbp), %rax\n    movq %rax, 0(%rsp)\n"
    "    movq 0x48(%rbp), %rax\n    movq %rax, 8(%rsp)\n"
    "    movq 0x50(%rbp), %rax\n    movq %rax, 16(%rsp)\n"
    "    movq 0x58(%rbp), %rax\n    movq %rax, 24(%rsp)\n"
    "    movq 0x60(%rbp), %rax\n    movq %rax, 32(%rsp)\n"
    "    movq 0x68(%rbp), %rax\n    movq %rax, 40(%rsp)\n"
    "    movq 0x70(%rbp), %rax\n    movq %rax, 48(%rsp)\n"
    "    movq 0x78(%rbp), %rax\n    movq %rax, 56(%rsp)\n"
    /* register arguments: rcx,rdx,r8,r9,stack,stack -> rdi,rsi,rdx,rcx,r8,r9 */
    "    movq %rcx, %rdi\n"
    "    movq %rdx, %rsi\n"
    "    movq %r8, %rdx\n"
    "    movq %r9, %rcx\n"
    "    movq 0x30(%rbp), %r8\n"
    "    movq 0x38(%rbp), %r9\n"
    "    xorl %eax, %eax\n"
    "    callq *%r11\n"
    "    movups 64(%rsp), %xmm6\n"
    "    movups 80(%rsp), %xmm7\n"
    "    movups 96(%rsp), %xmm8\n"
    "    movups 112(%rsp), %xmm9\n"
    "    movups 128(%rsp), %xmm10\n"
    "    movups 144(%rsp), %xmm11\n"
    "    movups 160(%rsp), %xmm12\n"
    "    movups 176(%rsp), %xmm13\n"
    "    movups 192(%rsp), %xmm14\n"
    "    movups 208(%rsp), %xmm15\n"
    "    addq $232, %rsp\n"
    "    popq %rbx\n"
    "    popq %rdi\n"
    "    popq %rsi\n"
    "    popq %rbp\n"
    "    ret\n"
    ".size beer_vk_bridge, .-beer_vk_bridge\n"
);
extern char beer_vk_bridge_trace[];
void beer_vkbridge_trace_call(const char *name);
extern char beer_vk_bridge[], beer_vk_bridge_f1[], beer_vk_bridge_f2[], beer_vk_bridge_f3[], beer_vk_bridge_g1[];

/* ---- Host library ---- */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static void *g_lib;
static PFN_vkGetInstanceProcAddr g_gipa;
static int g_init_tried;

static int host_init(void)
{
    pthread_mutex_lock(&g_lock);
    if (!g_init_tried) {
        g_init_tried = 1;
        g_lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        if (g_lib) g_gipa = (PFN_vkGetInstanceProcAddr)dlsym(g_lib, "vkGetInstanceProcAddr");
        if (!g_gipa) fprintf(stderr, "[VKBRIDGE] host libvulkan.so.1 is not available\n");
    }
    pthread_mutex_unlock(&g_lock);
    return g_gipa != NULL;
}

int beer_vkbridge_available(void) { return host_init(); }

/* Exported core commands come straight from libvulkan, the rest through vkGetInstanceProcAddr(NULL, ...). */
static void *host_symbol(const char *name)
{
    void *p = dlsym(g_lib, name);
    if (!p) p = (void *)g_gipa(VK_NULL_HANDLE, name);
    return p;
}

void *beer_vkbridge_host_symbol(const char *name)
{
    return host_init() ? host_symbol(name) : NULL;
}

void *beer_vkbridge_host_instance_proc(void *instance, const char *name)
{
    return host_init() ? (void *)g_gipa((VkInstance)instance, name) : NULL;
}

void *beer_vkbridge_host_device_proc(void *device, const char *name)
{
    PFN_vkGetDeviceProcAddr gdpa = host_init() ? (PFN_vkGetDeviceProcAddr)dlsym(g_lib, "vkGetDeviceProcAddr") : NULL;
    return gdpa ? (void *)gdpa((VkDevice)device, name) : NULL;
}

/* ---- Thunks ---- */
#define THUNK_SIZE 32
#define THUNK_CACHE 8192
static uint8_t *g_thunks;
static size_t g_thunk_used, g_thunk_capacity;
static struct { void *host; void *thunk; } g_thunk_cache[THUNK_CACHE];

static int trace_calls(void)
{
    static int level = -1;
    if (level < 0) { const char *v = getenv("BEER_VKBRIDGE_TRACE"); level = v ? atoi(v) : 0; }
    return level >= 2;
}

void beer_vkbridge_trace_call(const char *name)
{
    fprintf(stderr, "[VKCALL] %s\n", name);
}

static void *bridge_entry_for(const char *name)
{
    if (!strcmp(name, "vkCmdSetDepthBias")) return beer_vk_bridge_f3;
    if (!strcmp(name, "vkCmdSetDepthBounds")) return beer_vk_bridge_f2;
    if (!strcmp(name, "vkCmdSetLineWidth")) return beer_vk_bridge_f1;
    if (!strcmp(name, "vkSetDeviceMemoryPriorityEXT")) return beer_vk_bridge_g1;
    return trace_calls() ? (void *)beer_vk_bridge_trace : (void *)beer_vk_bridge;
}

static void *thunk_for(const char *name, void *host)
{
    if (!host) return NULL;
    pthread_mutex_lock(&g_lock);
    size_t slot = ((uintptr_t)host >> 4) % THUNK_CACHE;
    for (size_t probe = 0; probe < THUNK_CACHE; ++probe) {
        size_t i = (slot + probe) % THUNK_CACHE;
        if (g_thunk_cache[i].host == host) {
            void *thunk = g_thunk_cache[i].thunk;
            pthread_mutex_unlock(&g_lock);
            return thunk;
        }
        if (!g_thunk_cache[i].host) { slot = i; break; }
    }
    if (!g_thunks) {
        g_thunk_capacity = (size_t)THUNK_SIZE * THUNK_CACHE;
        g_thunks = mmap(NULL, g_thunk_capacity, PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (g_thunks == MAP_FAILED) { g_thunks = NULL; pthread_mutex_unlock(&g_lock); return NULL; }
    }
    if (g_thunk_used + THUNK_SIZE > g_thunk_capacity) { pthread_mutex_unlock(&g_lock); return NULL; }
    uint8_t *code = g_thunks + g_thunk_used;
    g_thunk_used += THUNK_SIZE;
    uint64_t target = (uint64_t)(uintptr_t)host, bridge = (uint64_t)(uintptr_t)bridge_entry_for(name);
    size_t o = 0;
    if (trace_calls()) {
        const char *copy = strdup(name);                                          /* mov r10, name (for the trace entry) */
        uint64_t text = (uint64_t)(uintptr_t)copy;
        code[o++] = 0x49; code[o++] = 0xBA; memcpy(code + o, &text, 8); o += 8;
    }
    code[o++] = 0x49; code[o++] = 0xBB; memcpy(code + o, &target, 8); o += 8;     /* mov r11, host */
    code[o++] = 0x48; code[o++] = 0xB8; memcpy(code + o, &bridge, 8); o += 8;     /* mov rax, bridge */
    code[o++] = 0xFF; code[o++] = 0xE0;                                           /* jmp rax */
    g_thunk_cache[slot].host = host;
    g_thunk_cache[slot].thunk = code;
    pthread_mutex_unlock(&g_lock);
    return code;
}

/* ---- Windows-specific translations ---- */
static int name_is_hidden_extension(const char *name)
{
    return strstr(name, "win32") || strstr(name, "_WIN32") || !strcmp(name, "VK_EXT_full_screen_exclusive") ||
           !strcmp(name, "VK_NV_acquire_winrt_display") || !strcmp(name, "VK_EXT_directfb_surface") ||
           !strcmp(name, "VK_KHR_xlib_surface") || !strcmp(name, "VK_KHR_xcb_surface") ||
           !strcmp(name, "VK_KHR_wayland_surface");
}

/* Copies the extension list of a create-info, mapping the win32 surface onto xlib and hiding what cannot work. */
static uint32_t filter_extensions(const char *const *in, uint32_t count, const char **out, uint32_t capacity,
                                  int instance_level)
{
    uint32_t n = 0;
    int want_xlib = 0, have_xlib = 0;
    for (uint32_t i = 0; i < count && n < capacity; ++i) {
        const char *e = in[i];
        if (instance_level && !strcmp(e, "VK_KHR_win32_surface")) { want_xlib = 1; continue; }
        if (!strcmp(e, "VK_KHR_xlib_surface")) have_xlib = 1;
        if (strstr(e, "win32") || strstr(e, "_WIN32") || !strcmp(e, "VK_EXT_full_screen_exclusive") ||
            !strcmp(e, "VK_NV_acquire_winrt_display"))
            continue;
        out[n++] = e;
    }
    if (want_xlib && !have_xlib && n < capacity) out[n++] = "VK_KHR_xlib_surface";
    return n;
}

typedef struct { VkStructureType sType; const void *pNext; } BaseIn;

/* Unlinks structures whose callbacks are guest functions; returns the head to use and records the splice. */
static int strip_callback_structs(const void **head_link, BaseIn **splice_owner, const void **splice_old)
{
    *splice_owner = NULL;
    *splice_old = NULL;
    BaseIn *prev = NULL;
    for (BaseIn *node = (BaseIn *)*head_link; node; prev = node, node = (BaseIn *)node->pNext) {
        if (node->sType == VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT ||
            node->sType == VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT) {
            if (prev) { *splice_owner = prev; *splice_old = prev->pNext; prev->pNext = node->pNext; }
            else { *splice_old = *head_link; *head_link = node->pNext; }
            return 1;
        }
    }
    return 0;
}

static VkResult MS_ABI bridge_CreateInstance(const VkInstanceCreateInfo *info, const void *allocator, VkInstance *instance)
{
    (void)allocator;
    if (!info || !instance || !host_init()) return VK_ERROR_INCOMPATIBLE_DRIVER;
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)g_gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (!create) return VK_ERROR_INCOMPATIBLE_DRIVER;
    const char *names[128];
    VkInstanceCreateInfo copy = *info;
    copy.enabledLayerCount = 0;
    copy.ppEnabledLayerNames = NULL;
    copy.enabledExtensionCount = filter_extensions(info->ppEnabledExtensionNames, info->enabledExtensionCount,
                                                   names, 128, 1);
    copy.ppEnabledExtensionNames = names;
    /* The debug messenger in pNext carries a guest function; splice it out for the duration of the call. */
    const void *head = copy.pNext, *old = NULL;
    BaseIn *owner = NULL;
    int stripped = strip_callback_structs(&head, &owner, &old);
    copy.pNext = head;
    VkResult r = create(&copy, NULL, instance);
    if (stripped && owner) owner->pNext = old;      /* restore the guest's chain */
    fprintf(stderr, "[VKBRIDGE] vkCreateInstance(%u extensions) -> %d\n", copy.enabledExtensionCount, (int)r);
    if (r == VK_SUCCESS) beer_vkmap_instance_created(*instance);
    return r;
}

static VkResult MS_ABI bridge_EnumerateInstanceExtensionProperties(const char *layer, uint32_t *count,
                                                                    VkExtensionProperties *props)
{
    if (!count || !host_init()) return VK_ERROR_INITIALIZATION_FAILED;
    if (layer) { *count = 0; return VK_ERROR_LAYER_NOT_PRESENT; }
    PFN_vkEnumerateInstanceExtensionProperties host =
        (PFN_vkEnumerateInstanceExtensionProperties)g_gipa(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    uint32_t total = 0;
    if (!host || host(NULL, &total, NULL) != VK_SUCCESS) return VK_ERROR_INITIALIZATION_FAILED;
    VkExtensionProperties *all = calloc(total + 1, sizeof(*all));
    if (!all || host(NULL, &total, all) < 0) { free(all); return VK_ERROR_INITIALIZATION_FAILED; }
    VkExtensionProperties *keep = calloc(total + 2, sizeof(*keep));
    uint32_t n = 0;
    int have_xlib = 0;
    for (uint32_t i = 0; i < total; ++i) {
        if (!strcmp(all[i].extensionName, "VK_KHR_xlib_surface")) have_xlib = 1;
        if (!name_is_hidden_extension(all[i].extensionName)) keep[n++] = all[i];
    }
    if (have_xlib) {
        VkExtensionProperties win32 = { .specVersion = 6 };
        snprintf(win32.extensionName, sizeof(win32.extensionName), "VK_KHR_win32_surface");
        keep[n++] = win32;
    }
    VkResult r = VK_SUCCESS;
    if (!props) { *count = n; }
    else {
        uint32_t room = *count < n ? *count : n;
        memcpy(props, keep, room * sizeof(*keep));
        r = room < n ? VK_INCOMPLETE : VK_SUCCESS;
        *count = room;
    }
    free(all); free(keep);
    return r;
}

static VkResult MS_ABI bridge_EnumerateInstanceLayerProperties(uint32_t *count, VkLayerProperties *props)
{
    (void)props;
    if (count) *count = 0;                   /* no layers: validation would call back into guest code */
    return VK_SUCCESS;
}

static VkResult MS_ABI bridge_CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                                           const void *allocator, VkDevice *device)
{
    (void)allocator;
    beer_vkbridge_trace_call("vkCreateDevice");
    if (!info || !device || !host_init()) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)dlsym(g_lib, "vkCreateDevice");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    const char *names[256];
    VkDeviceCreateInfo copy = *info;
    copy.enabledLayerCount = 0;
    copy.ppEnabledLayerNames = NULL;
    copy.enabledExtensionCount = filter_extensions(info->ppEnabledExtensionNames, info->enabledExtensionCount,
                                                   names, 256, 0);
    copy.ppEnabledExtensionNames = names;
    VkResult r = create(physical, &copy, NULL, device);
    fprintf(stderr, "[VKBRIDGE] vkCreateDevice(%u extensions) -> %d\n", copy.enabledExtensionCount, (int)r);
    if (r == VK_SUCCESS) beer_vkmap_device_created(physical, *device);
    return r;
}

/* VkWin32SurfaceCreateInfoKHR: { sType, pNext, flags, HINSTANCE, HWND }.  Beer has a single native window. */
static VkResult MS_ABI bridge_CreateWin32SurfaceKHR(VkInstance instance, const void *info, const void *allocator,
                                                    VkSurfaceKHR *surface)
{
    (void)info; (void)allocator;
    void *display = NULL;
    unsigned long window = 0;
    if (!surface || !host_init() || !xwayland_window_native(&display, &window) || !display || !window)
        return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkCreateXlibSurfaceKHR create = (PFN_vkCreateXlibSurfaceKHR)g_gipa(instance, "vkCreateXlibSurfaceKHR");
    if (!create) return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkXlibSurfaceCreateInfoKHR xlib = {
        .sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
        .dpy = (Display *)display,
        .window = (Window)window,
    };
    VkResult r = create(instance, &xlib, NULL, surface);
    fprintf(stderr, "[VKBRIDGE] vkCreateWin32SurfaceKHR -> xlib surface -> %d\n", (int)r);
    return r;
}

static VkBool32 MS_ABI bridge_GetPhysicalDeviceWin32PresentationSupportKHR(VkPhysicalDevice physical, uint32_t family)
{
    (void)physical; (void)family;
    return VK_TRUE;
}

/* Debug callbacks are guest functions in the wrong ABI: accept the object and never call them. */
static VkResult MS_ABI bridge_CreateDebugCallback(VkInstance instance, const void *info, const void *allocator,
                                                  uint64_t *handle)
{
    (void)instance; (void)info; (void)allocator;
    if (handle) *handle = 0x5EED0001ull;
    return VK_SUCCESS;
}
static void MS_ABI bridge_DestroyDebugCallback(VkInstance instance, uint64_t handle, const void *allocator)
{
    (void)instance; (void)handle; (void)allocator;
}

static PFN_vkVoidFunction MS_ABI bridge_GetInstanceProcAddr(VkInstance instance, const char *name);
static PFN_vkVoidFunction MS_ABI bridge_GetDeviceProcAddr(VkDevice device, const char *name);

static void *override_for(const char *name)
{
    void *mapped = beer_vkmap_override(name);
    if (mapped) return mapped;
    static const struct { const char *name; void *fn; } table[] = {
        { "vkCreateInstance", bridge_CreateInstance },
        { "vkEnumerateInstanceExtensionProperties", bridge_EnumerateInstanceExtensionProperties },
        { "vkEnumerateInstanceLayerProperties", bridge_EnumerateInstanceLayerProperties },
        { "vkCreateDevice", bridge_CreateDevice },
        { "vkCreateWin32SurfaceKHR", bridge_CreateWin32SurfaceKHR },
        { "vkGetPhysicalDeviceWin32PresentationSupportKHR", bridge_GetPhysicalDeviceWin32PresentationSupportKHR },
        { "vkCreateDebugUtilsMessengerEXT", bridge_CreateDebugCallback },
        { "vkDestroyDebugUtilsMessengerEXT", bridge_DestroyDebugCallback },
        { "vkCreateDebugReportCallbackEXT", bridge_CreateDebugCallback },
        { "vkDestroyDebugReportCallbackEXT", bridge_DestroyDebugCallback },
        { "vkGetInstanceProcAddr", bridge_GetInstanceProcAddr },
        { "vkGetDeviceProcAddr", bridge_GetDeviceProcAddr },
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(*table); ++i)
        if (!strcmp(table[i].name, name)) return table[i].fn;
    return NULL;
}

/* BEER_VKBRIDGE_TRACE=1 logs every entry point the guest resolves (how far its Vulkan start-up got). */
static void trace_resolve(const char *how, const char *name, int found)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("BEER_VKBRIDGE_TRACE") != NULL;
    if (enabled) fprintf(stderr, "[VKBRIDGE] %s(%s) -> %s\n", how, name, found ? "ok" : "NULL");
}

static PFN_vkVoidFunction MS_ABI bridge_GetInstanceProcAddr(VkInstance instance, const char *name)
{
    if (!name || !host_init()) return NULL;
    void *fn = override_for(name);
    if (fn) return (PFN_vkVoidFunction)fn;
    void *host = (void *)g_gipa(instance, name);
    if (!host && !instance) host = host_symbol(name);
    trace_resolve("vkGetInstanceProcAddr", name, host != NULL);
    return (PFN_vkVoidFunction)thunk_for(name, host);
}

static PFN_vkVoidFunction MS_ABI bridge_GetDeviceProcAddr(VkDevice device, const char *name)
{
    if (!name || !host_init() || !device) return NULL;
    void *fn = override_for(name);
    if (fn) return (PFN_vkVoidFunction)fn;
    PFN_vkGetDeviceProcAddr host_gdpa = (PFN_vkGetDeviceProcAddr)dlsym(g_lib, "vkGetDeviceProcAddr");
    void *host = host_gdpa ? (void *)host_gdpa(device, name) : NULL;
    trace_resolve("vkGetDeviceProcAddr", name, host != NULL);
    return (PFN_vkVoidFunction)thunk_for(name, host);
}

uintptr_t beer_vkbridge_get_proc(const char *name)
{
    if (!name || strncmp(name, "vk", 2) != 0 || !host_init()) return 0;
    void *fn = override_for(name);
    if (fn) return (uintptr_t)fn;
    void *host = host_symbol(name);
    trace_resolve("GetProcAddress", name, host != NULL);
    return (uintptr_t)thunk_for(name, host);
}
