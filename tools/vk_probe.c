/* Minimal Vulkan bring-up probe: the same first steps the loader's
 * vulkan_context.c takes (dlopen libvulkan.so.1, create an instance with the
 * Xlib surface extension, enumerate GPUs). Needs no game, window or shaders.
 *
 * Build (x86-64, so it can run under Box64):
 *   gcc -O2 -Wall -I<dir containing vulkan/vulkan_core.h> tools/vk_probe.c -ldl -o vk_probe
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan_core.h>

int main(void)
{
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        printf("FAIL dlopen(libvulkan.so.1): %s\n", dlerror());
        return 1;
    }
    PFN_vkGetInstanceProcAddr gipa =
        (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    if (!gipa) { printf("FAIL: no vkGetInstanceProcAddr\n"); return 1; }
    printf("OK   dlopen libvulkan.so.1\n");

    PFN_vkEnumerateInstanceVersion ver =
        (PFN_vkEnumerateInstanceVersion)gipa(NULL, "vkEnumerateInstanceVersion");
    uint32_t v = VK_API_VERSION_1_0;
    if (ver) ver(&v);
    printf("OK   instance version %u.%u.%u\n", VK_VERSION_MAJOR(v), VK_VERSION_MINOR(v),
           VK_VERSION_PATCH(v));

    PFN_vkEnumerateInstanceExtensionProperties eie =
        (PFN_vkEnumerateInstanceExtensionProperties)gipa(NULL, "vkEnumerateInstanceExtensionProperties");
    uint32_t n = 0;
    eie(NULL, &n, NULL);
    VkExtensionProperties *ext = calloc(n ? n : 1, sizeof(*ext));
    eie(NULL, &n, ext);
    int has_surface = 0, has_xlib = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!strcmp(ext[i].extensionName, "VK_KHR_surface")) has_surface = 1;
        if (!strcmp(ext[i].extensionName, "VK_KHR_xlib_surface")) has_xlib = 1;
    }
    free(ext);
    printf("%s VK_KHR_surface\n%s VK_KHR_xlib_surface (needed by the presenter)\n",
           has_surface ? "OK  " : "FAIL", has_xlib ? "OK  " : "FAIL");

    const char *want[] = {"VK_KHR_surface", "VK_KHR_xlib_surface"};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .apiVersion = v >= VK_API_VERSION_1_1 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                .pApplicationInfo = &app,
                                .enabledExtensionCount = (has_surface && has_xlib) ? 2 : 0,
                                .ppEnabledExtensionNames = want};
    PFN_vkCreateInstance ci = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = ci(&ici, NULL, &inst);
    if (r != VK_SUCCESS) { printf("FAIL vkCreateInstance: %d\n", r); return 1; }
    printf("OK   vkCreateInstance\n");

    PFN_vkEnumeratePhysicalDevices epd =
        (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties gpp =
        (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
    uint32_t count = 0;
    epd(inst, &count, NULL);
    printf("%s %u physical device(s)\n", count ? "OK  " : "FAIL", count);
    VkPhysicalDevice devs[8];
    if (count > 8) count = 8;
    epd(inst, &count, devs);
    for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDeviceProperties p;
        gpp(devs[i], &p);
        printf("     [%u] %s  api %u.%u.%u  driver 0x%x  type %d\n", i, p.deviceName,
               VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion),
               VK_VERSION_PATCH(p.apiVersion), p.driverVersion, (int)p.deviceType);
    }
    return count ? 0 : 1;
}
