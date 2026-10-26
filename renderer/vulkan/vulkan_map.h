#ifndef BEER_VULKAN_MAP_H
#define BEER_VULKAN_MAP_H

#define VK_NO_PROTOTYPES
#include <stdint.h>
#include <vulkan/vulkan_core.h>

/* Vulkan-on-Vulkan mapper: the layer between a native Vulkan guest and the host driver (see vulkan_map.c).  The
 * bridge (vulkan_bridge.c) owns the calling-convention thunks; this module owns what Beer does to the calls so
 * Vulkan games get the same manager features as the Direct3D 12 core: frame counting and cap, overlay,
 * screenshots, final-image effects and GPU identity spoofing. */

/* Mapped (ms_abi) replacement for vk<name>, or NULL when the call is forwarded unchanged. */
void *beer_vkmap_override(const char *name);

void beer_vkmap_instance_created(VkInstance instance);

/* Registers a device the host created for the guest. */
void beer_vkmap_device_created(VkPhysicalDevice physical, VkDevice device);

#endif
