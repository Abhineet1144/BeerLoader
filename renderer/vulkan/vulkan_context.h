#ifndef BEER_VULKAN_CONTEXT_H
#define BEER_VULKAN_CONTEXT_H

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

/* Process-wide Vulkan context shared by indexed rendering, compositing, and
 * presentation. The context owns one instance/device/queue for Beer’s full
 * lifetime; individual stages continue to own their pipelines and resources. */
int beer_vulkan_context_initialize(void);
VkInstance beer_vulkan_instance(void);
VkPhysicalDevice beer_vulkan_physical_device(void);
VkDevice beer_vulkan_device(void);
VkQueue beer_vulkan_queue(void);
uint32_t beer_vulkan_queue_family(void);
PFN_vkGetInstanceProcAddr beer_vulkan_get_instance_proc_addr(void);
PFN_vkGetDeviceProcAddr beer_vulkan_get_device_proc_addr(void);

/* Vulkan queues require external synchronization across host threads. */
void beer_vulkan_queue_lock(void);
void beer_vulkan_queue_unlock(void);

/* Used during final process teardown after all stage-owned resources are gone. */
void beer_vulkan_context_destroy(void);

#endif
