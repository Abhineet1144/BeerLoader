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
    ".globl beer_vk_common\n"
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
    /* Common entry of every thunk.  r10 -> VkThunk { host, entry, name }.  Runs the call on the thread's host call stack
     * (the guest's own stack can be a few tens of KiB on worker threads, far too little for a Vulkan driver), copies
     * the stack arguments across and returns to the guest on its own stack.  Not switched when already on the host
     * stack or when the thread has none. */
    "beer_vk_common:\n"
    "    cmpl $0, beer_vk_trace_level(%rip)\n"
    "    je 1f\n"
    "    pushq %rcx\n    pushq %rdx\n    pushq %r8\n    pushq %r9\n    pushq %r10\n    pushq %rsi\n    pushq %rdi\n"
    "    movq 16(%r10), %rdi\n"
    "    movq 56(%rsp), %rsi\n"                 /* the guest's return address */
    "    callq beer_vkbridge_trace_call\n"
    "    popq %rdi\n    popq %rsi\n    popq %r10\n    popq %r9\n    popq %r8\n    popq %rdx\n    popq %rcx\n"
    "1:\n"
    "    movq 0(%r10), %r11\n"
    "    movq 8(%r10), %r10\n"
    "    cmpq $0, %fs:g_host_call_stack_top@tpoff\n"
    "    je 3f\n"
    "    movq %rsp, %rax\n"
    "    subq %fs:g_host_call_stack_base@tpoff, %rax\n"
    "    cmpq $1048576, %rax\n"
    "    jb 3f\n"
    "    movq %rsp, %rax\n"
    "    movq %rsp, %fs:beer_vk_saved_rsp@tpoff\n"
    "    movq %fs:g_host_call_stack_top@tpoff, %rsp\n"
    "    andq $-16, %rsp\n"
    "    subq $0x80, %rsp\n"
    ".irp off,0,8,16,24,32,40,48,56,64,72,80,88\n"
    "    movq 0x28+\\off(%rax), %xmm4\n"
    "    movq %xmm4, 0x20+\\off(%rsp)\n"
    ".endr\n"
    "    callq *%r10\n"
    "    movq %fs:beer_vk_saved_rsp@tpoff, %rsp\n"
    "    ret\n"
    "3:\n"
    "    jmpq *%r10\n"
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
extern char beer_vk_common[];
extern __thread uint64_t g_host_call_stack_top;
__thread uint64_t beer_vk_saved_rsp;
int beer_vk_trace_level;
void beer_vkbridge_trace_call(const char *name, uintptr_t guest_return);
static int trace_calls(void);
#define OVERRIDE_TRACE(name) do { } while (0)   /* every call is logged by beer_vk_common */
extern char beer_vk_bridge[], beer_vk_bridge_f1[], beer_vk_bridge_f2[], beer_vk_bridge_f3[], beer_vk_bridge_g1[];

/* ---- Host library ---- */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static void *g_lib;
static PFN_vkGetInstanceProcAddr g_gipa;
static int g_init_tried;

static int trace_calls(void);
static int host_init(void)
{
    trace_calls();
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
    if (level < 0) { const char *v = getenv("BEER_VKBRIDGE_TRACE"); level = v ? atoi(v) : 0; beer_vk_trace_level = level >= 2; }
    return level >= 2;   /* 2 and 3 */
}

/* Level 3 also dumps the guest code around each distinct call site (hex, see BEER_CRASH_CODE in loader.c). */
void beer_vkbridge_trace_call(const char *name, uintptr_t guest_return)
{
    fprintf(stderr, "[VKCALL] %s ret=0x%lx\n", name, (unsigned long)guest_return);
    const char *v = getenv("BEER_VKBRIDGE_TRACE");
    static uintptr_t seen[64];
    static int seen_count;
    static int range_done;
    const char *range = getenv("BEER_DUMP_RANGE");     /* "0xSTART:0xLENGTH": hex of guest memory at the first traced call */
    if (range && !range_done && guest_return) {
        range_done = 1;
        unsigned long start = 0, length = 0;
        if (sscanf(range, "%lx:%lx", &start, &length) == 2 && length <= 0x40000) {
            fprintf(stderr, "[RANGE] start=0x%lx\n[RANGE] ", start);
            for (unsigned long i = 0; i < length; ++i) fprintf(stderr, "%02x", ((const uint8_t *)start)[i]);
            fputs("\n", stderr);
        }
    }
    if (!guest_return || !v || atoi(v) < 3) return;
    for (int i = 0; i < seen_count; ++i)
        if (seen[i] == guest_return) return;
    if (seen_count < 64) seen[seen_count++] = guest_return;
    fprintf(stderr, "[CALLSITE] start=0x%lx\n[CALLSITE] ", (unsigned long)(guest_return - 0x300));
    for (int i = 0; i < 0x400; ++i) fprintf(stderr, "%02x", ((const uint8_t *)(guest_return - 0x300))[i]);
    fputs("\n", stderr);
}

static void *bridge_entry_for(const char *name)
{
    if (!strcmp(name, "vkCmdSetDepthBias")) return beer_vk_bridge_f3;
    if (!strcmp(name, "vkCmdSetDepthBounds")) return beer_vk_bridge_f2;
    if (!strcmp(name, "vkCmdSetLineWidth")) return beer_vk_bridge_f1;
    if (!strcmp(name, "vkSetDeviceMemoryPriorityEXT")) return beer_vk_bridge_g1;
    return beer_vk_bridge;
}

/* Per-command record the thunk points r10 at: the host function (loaded into r11 for the ABI bridge), the entry to run
 * (a bridge label for forwarded commands, the C function for Beer's own overrides) and the name for tracing. */
typedef struct { void *host; void *entry; const char *name; } VkThunk;

static void *make_thunk(const char *name, void *host, void *entry)
{
    void *key = host ? host : entry;
    if (!key) return NULL;
    pthread_mutex_lock(&g_lock);
    size_t slot = ((uintptr_t)key >> 4) % THUNK_CACHE;
    for (size_t probe = 0; probe < THUNK_CACHE; ++probe) {
        size_t i = (slot + probe) % THUNK_CACHE;
        if (g_thunk_cache[i].host == key) {
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
    VkThunk *info = malloc(sizeof(*info));
    if (!info || g_thunk_used + THUNK_SIZE > g_thunk_capacity) { free(info); pthread_mutex_unlock(&g_lock); return NULL; }
    info->host = host;
    info->entry = entry;
    info->name = strdup(name);
    uint8_t *code = g_thunks + g_thunk_used;
    g_thunk_used += THUNK_SIZE;
    uint64_t record = (uint64_t)(uintptr_t)info, common = (uint64_t)(uintptr_t)beer_vk_common;
    size_t o = 0;
    code[o++] = 0x49; code[o++] = 0xBA; memcpy(code + o, &record, 8); o += 8;     /* mov r10, record */
    code[o++] = 0x48; code[o++] = 0xB8; memcpy(code + o, &common, 8); o += 8;     /* mov rax, beer_vk_common */
    code[o++] = 0xFF; code[o++] = 0xE0;                                           /* jmp rax */
    g_thunk_cache[slot].host = key;
    g_thunk_cache[slot].thunk = code;
    pthread_mutex_unlock(&g_lock);
    return code;
}

/* Forwarded command: guest -> ABI bridge -> host function. */
static void *thunk_for(const char *name, void *host)
{
    return host ? make_thunk(name, host, bridge_entry_for(name)) : NULL;
}

/* Beer's own implementation (an ms_abi C function), still entered on the host stack. */
static void *thunk_for_override(const char *name, void *fn)
{
    return make_thunk(name, NULL, fn);
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
    if (want_xlib && xwayland_headless()) {            /* no display: surfaces come from the loader's headless implementation */
        if (n < capacity) out[n++] = "VK_EXT_headless_surface";
    } else if (want_xlib && !have_xlib && n < capacity) out[n++] = "VK_KHR_xlib_surface";
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
    OVERRIDE_TRACE("vkCreateInstance");
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
    OVERRIDE_TRACE("vkEnumerateInstanceExtensionProperties");
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
    if (have_xlib || xwayland_headless()) {
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
    OVERRIDE_TRACE("vkEnumerateInstanceLayerProperties");
    (void)props;
    if (count) *count = 0;                   /* no layers: validation would call back into guest code */
    return VK_SUCCESS;
}

static VkResult MS_ABI bridge_CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                                           const void *allocator, VkDevice *device)
{
    OVERRIDE_TRACE("vkCreateDevice");
    (void)allocator;
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
    OVERRIDE_TRACE("vkCreateWin32SurfaceKHR");
    (void)info; (void)allocator;
    void *display = NULL;
    unsigned long window = 0;
    if (surface && host_init() && xwayland_headless()) {
        PFN_vkCreateHeadlessSurfaceEXT headless = (PFN_vkCreateHeadlessSurfaceEXT)g_gipa(instance, "vkCreateHeadlessSurfaceEXT");
        if (!headless) return VK_ERROR_EXTENSION_NOT_PRESENT;
        VkHeadlessSurfaceCreateInfoEXT hinfo = { .sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT };
        VkResult hr = headless(instance, &hinfo, NULL, surface);
        fprintf(stderr, "[VKBRIDGE] vkCreateWin32SurfaceKHR -> headless surface -> %d\n", (int)hr);
        return hr;
    }
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

/* Headless surface queries: the host reports whatever the headless implementation can do, which is often nothing
 * (no formats, undefined extent); give the guest the answers a 2560x1440 sRGB window would. */
static void *headless_host(const char *name)
{
    void *fn = (void *)g_gipa(VK_NULL_HANDLE, name);
    return fn ? fn : host_symbol(name);
}
static VkResult MS_ABI bridge_GetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physical, VkSurfaceKHR surface, uint32_t *count,
                                                                 VkSurfaceFormatKHR *formats)
{
    OVERRIDE_TRACE("vkGetPhysicalDeviceSurfaceFormatsKHR");
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR host = (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)headless_host("vkGetPhysicalDeviceSurfaceFormatsKHR");
    if (!xwayland_headless()) return host ? host(physical, surface, count, formats) : VK_ERROR_EXTENSION_NOT_PRESENT;
    static const VkSurfaceFormatKHR table[] = {
        { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR }, { VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
        { VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },  { VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
        { VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR }, { VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR } };
    const uint32_t total = sizeof(table) / sizeof(*table);
    if (!count) return VK_ERROR_INITIALIZATION_FAILED;
    if (!formats) { *count = total; return VK_SUCCESS; }
    uint32_t room = *count < total ? *count : total;
    memcpy(formats, table, room * sizeof(*table));
    *count = room;
    return room < total ? VK_INCOMPLETE : VK_SUCCESS;
}
static VkResult MS_ABI bridge_GetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physical, VkSurfaceKHR surface, uint32_t *count,
                                                                      VkPresentModeKHR *modes)
{
    OVERRIDE_TRACE("vkGetPhysicalDeviceSurfacePresentModesKHR");
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR host = (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)headless_host("vkGetPhysicalDeviceSurfacePresentModesKHR");
    if (!xwayland_headless()) return host ? host(physical, surface, count, modes) : VK_ERROR_EXTENSION_NOT_PRESENT;
    static const VkPresentModeKHR table[] = { VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR };
    const uint32_t total = 3;
    if (!count) return VK_ERROR_INITIALIZATION_FAILED;
    if (!modes) { *count = total; return VK_SUCCESS; }
    uint32_t room = *count < total ? *count : total;
    memcpy(modes, table, room * sizeof(*table));
    *count = room;
    return room < total ? VK_INCOMPLETE : VK_SUCCESS;
}
static VkResult MS_ABI bridge_GetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physical, VkSurfaceKHR surface,
                                                                      VkSurfaceCapabilitiesKHR *caps)
{
    OVERRIDE_TRACE("vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR host = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)headless_host("vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    if (!xwayland_headless() || !caps) return host ? host(physical, surface, caps) : VK_ERROR_EXTENSION_NOT_PRESENT;
    memset(caps, 0, sizeof(*caps));
    caps->minImageCount = 2; caps->maxImageCount = 8;
    caps->currentExtent = (VkExtent2D){ 2560, 1440 };
    caps->minImageExtent = (VkExtent2D){ 1, 1 }; caps->maxImageExtent = (VkExtent2D){ 16384, 16384 };
    caps->maxImageArrayLayers = 1;
    caps->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR; caps->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    caps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    caps->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    return VK_SUCCESS;
}

/* Headless surfaces have no windowing system to ask: every queue family may present (the images go nowhere). */
static VkResult MS_ABI bridge_GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physical, uint32_t family, VkSurfaceKHR surface,
                                                                 VkBool32 *supported)
{
    OVERRIDE_TRACE("vkGetPhysicalDeviceSurfaceSupportKHR");
    if (xwayland_headless()) { if (supported) *supported = VK_TRUE; return VK_SUCCESS; }
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR host = (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)g_gipa(VK_NULL_HANDLE, "vkGetPhysicalDeviceSurfaceSupportKHR");
    if (!host) host = (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)host_symbol("vkGetPhysicalDeviceSurfaceSupportKHR");
    return host ? host(physical, family, surface, supported) : VK_ERROR_EXTENSION_NOT_PRESENT;
}

static VkBool32 MS_ABI bridge_GetPhysicalDeviceWin32PresentationSupportKHR(VkPhysicalDevice physical, uint32_t family)
{
    OVERRIDE_TRACE("vkGetPhysicalDeviceWin32PresentationSupportKHR");
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
        { "vkGetPhysicalDeviceSurfaceSupportKHR", bridge_GetPhysicalDeviceSurfaceSupportKHR },
        { "vkGetPhysicalDeviceSurfaceFormatsKHR", bridge_GetPhysicalDeviceSurfaceFormatsKHR },
        { "vkGetPhysicalDeviceSurfacePresentModesKHR", bridge_GetPhysicalDeviceSurfacePresentModesKHR },
        { "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", bridge_GetPhysicalDeviceSurfaceCapabilitiesKHR },
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
    if (fn) return (PFN_vkVoidFunction)thunk_for_override(name, fn);
    void *host = (void *)g_gipa(instance, name);
    if (!host && !instance) host = host_symbol(name);
    trace_resolve("vkGetInstanceProcAddr", name, host != NULL);
    return (PFN_vkVoidFunction)thunk_for(name, host);
}

static PFN_vkVoidFunction MS_ABI bridge_GetDeviceProcAddr(VkDevice device, const char *name)
{
    if (!name || !host_init() || !device) return NULL;
    void *fn = override_for(name);
    if (fn) return (PFN_vkVoidFunction)thunk_for_override(name, fn);
    PFN_vkGetDeviceProcAddr host_gdpa = (PFN_vkGetDeviceProcAddr)dlsym(g_lib, "vkGetDeviceProcAddr");
    void *host = host_gdpa ? (void *)host_gdpa(device, name) : NULL;
    trace_resolve("vkGetDeviceProcAddr", name, host != NULL);
    return (PFN_vkVoidFunction)thunk_for(name, host);
}

uintptr_t beer_vkbridge_get_proc(const char *name)
{
    if (!name || strncmp(name, "vk", 2) != 0 || !host_init()) return 0;
    void *fn = override_for(name);
    if (fn) return (uintptr_t)thunk_for_override(name, fn);
    void *host = host_symbol(name);
    trace_resolve("GetProcAddress", name, host != NULL);
    return (uintptr_t)thunk_for(name, host);
}
