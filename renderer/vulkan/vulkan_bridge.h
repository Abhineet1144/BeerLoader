#ifndef BEER_VULKAN_BRIDGE_H
#define BEER_VULKAN_BRIDGE_H

#include <stdint.h>

/* Built-in "vulkan-1.dll" for guests: see vulkan_bridge.c. */

/* Nonzero when the host has a usable libvulkan. */
int beer_vkbridge_available(void);

/* Address a guest sees for vk<name> in vulkan-1.dll (Microsoft x64 ABI), or 0 when the host lacks it. */
uintptr_t beer_vkbridge_get_proc(const char *name);

/* For the mapper (vulkan_map.c): the host entry points behind the guest's calls.  Handles are passed as void *. */
void *beer_vkbridge_host_symbol(const char *name);                       /* exported libvulkan symbol or global proc */
void *beer_vkbridge_host_instance_proc(void *instance, const char *name);
void *beer_vkbridge_host_device_proc(void *device, const char *name);

#endif
