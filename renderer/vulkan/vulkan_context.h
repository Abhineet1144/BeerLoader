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
/* Optional device features enabled at device creation. */
const VkPhysicalDeviceFeatures *beer_vulkan_enabled_features(void);
/* Nonzero when VK_KHR_maintenance1 (negative viewport height) is enabled. */
int beer_vulkan_maintenance1_enabled(void);
/* Nonzero when min/max sampler reduction is supported and enabled. */
int beer_vulkan_sampler_filter_minmax_enabled(void);
/* Maximum sampler anisotropy enabled on the shared device; 1 when unsupported. */
uint32_t beer_vulkan_max_sampler_anisotropy(void);
/* Nonzero when VK_EXT_pipeline_robustness is enabled. Graphics pipelines use
 * it to make GPU-generated indirect vertex/index accesses D3D-robust. */
int beer_vulkan_pipeline_robustness_enabled(void);
/* A lost Vulkan device is terminal. Callers must stop submitting work once the
 * shared context records VK_ERROR_DEVICE_LOST. */
int beer_vulkan_device_lost(void);
void beer_vulkan_record_result(VkResult result, const char *operation);
PFN_vkGetInstanceProcAddr beer_vulkan_get_instance_proc_addr(void);
PFN_vkGetDeviceProcAddr beer_vulkan_get_device_proc_addr(void);

/* Vulkan queues require external synchronization across host threads. */
void beer_vulkan_queue_lock(void);
void beer_vulkan_queue_unlock(void);

/* Diagnostic accounting of host time spent in generic compute/graphics
 * dispatches. out = {compute count, compute ns, graphics count, graphics ns};
 * take() returns totals since the previous take() and resets them. */
void beer_vulkan_perf_account(int graphics, uint64_t ns);
uint64_t beer_vulkan_perf_threshold_ns(void);
void beer_vulkan_perf_take(uint64_t out[4]);

/* Used during final process teardown after all stage-owned resources are gone. */
void beer_vulkan_context_destroy(void);

#endif
