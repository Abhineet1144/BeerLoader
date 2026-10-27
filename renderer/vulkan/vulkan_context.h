#ifndef BEER_VULKAN_CONTEXT_H
#define BEER_VULKAN_CONTEXT_H

#define VK_NO_PROTOTYPES
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>

/* Process-wide Vulkan context shared by indexed rendering, compositing, and
 * presentation. The context owns one instance/device/queue for Beer’s full
 * lifetime; individual stages continue to own their pipelines and resources. */
int beer_vulkan_context_initialize(void);

/* Properties of the physical device Beer renders on, for guest-visible
 * adapter/capability reporting (DXGI adapter description, D3D12 feature
 * support). Initializes the context when needed; returns 0 if unavailable. */
typedef struct {
    char name[256];
    uint32_t vendor_id, device_id, revision;
    uint64_t dedicated_video_memory;   /* largest DEVICE_LOCAL-only heap */
    uint64_t shared_system_memory;     /* largest non-device-local heap */
    uint64_t luid;                     /* stable per-device identifier */
    int ray_tracing_pipeline;          /* VK_KHR_ray_tracing_pipeline usable */
    int acceleration_structure;        /* VK_KHR_acceleration_structure usable */
    int ray_query;                     /* VK_KHR_ray_query usable */
    int mesh_shader;                   /* VK_EXT_mesh_shader usable */
    int fragment_shading_rate;         /* VK_KHR_fragment_shading_rate usable */
} BeerHostGpuInfo;
int beer_vulkan_host_gpu_info(BeerHostGpuInfo *out);
/* Vendor/device id of the GPU Beer renders on, before any BEER_SPOOF_GPU_* override. */
int beer_vulkan_real_gpu_ids(uint32_t *vendor, uint32_t *device);
/* BEER_SPOOF_GPU_NAME / _VENDOR / _DEVICE (hex) / _VRAM_MB: writes only the fields whose variable is set and returns
 * a mask (1 name, 2 vendor, 4 device, 8 vram).  Any pointer may be NULL. */
int beer_vulkan_gpu_spoof(char *name, size_t name_size, uint32_t *vendor, uint32_t *device, uint64_t *vram_bytes);
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
/* Nonzero when acceleration structures, ray tracing pipelines and ray query
 * are enabled on the shared device (disable with BEER_NO_RT=1). */
int beer_vulkan_ray_tracing_enabled(void);
/* Maximum sampler anisotropy enabled on the shared device; 1 when unsupported. */
uint32_t beer_vulkan_max_sampler_anisotropy(void);
/* Nonzero when VK_EXT_pipeline_robustness is enabled. Graphics pipelines use
 * it to make GPU-generated indirect vertex/index accesses D3D-robust. */
int beer_vulkan_pipeline_robustness_enabled(void);
/* Vulkan 1.2/1.3 core features enabled on the shared device (all zero when
 * the instance or GPU is older than 1.3). Consumed by the D3D12-on-Vulkan
 * core, which relies on buffer device addresses, synchronization2, dynamic
 * rendering and update-after-bind descriptor indexing. */
typedef struct {
    uint32_t api_version;
    int buffer_device_address;
    int synchronization2;
    int dynamic_rendering;
    int descriptor_indexing;   /* runtime arrays + partially bound + update-after-bind */
    int scalar_block_layout;
    int host_query_reset;
    int maintenance4;
    int demote_to_helper;
    VkDeviceSize uniform_texel_buffer_offset_alignment;
    VkDeviceSize storage_texel_buffer_offset_alignment;
    int uniform_texel_buffer_offset_single_texel;
    int storage_texel_buffer_offset_single_texel;
} BeerVulkanModern;
const BeerVulkanModern *beer_vulkan_modern(void);
/* A lost Vulkan device is terminal. Callers must stop submitting work once the
 * shared context records VK_ERROR_DEVICE_LOST. */
int beer_vulkan_device_lost(void);
void beer_vulkan_record_result(VkResult result, const char *operation);
PFN_vkGetInstanceProcAddr beer_vulkan_get_instance_proc_addr(void);
PFN_vkGetDeviceProcAddr beer_vulkan_get_device_proc_addr(void);

/* Vulkan queues require external synchronization across host threads. */
void beer_vulkan_queue_lock(void);
void beer_vulkan_queue_unlock(void);

/* Shared timeline used by the command-list compiler. Submissions are
 * asynchronous; callers wait only at a genuine CPU visibility/fence boundary.
 * Zero is returned when timeline semaphores are unavailable or submission
 * fails, allowing the existing fence-based scalar path to remain the fallback. */
int beer_vulkan_timeline_enabled(void);
uint64_t beer_vulkan_timeline_submit(const VkCommandBuffer *commands,
                                     uint32_t command_count);
int beer_vulkan_timeline_wait(uint64_t value, uint64_t timeout_ns);
uint64_t beer_vulkan_timeline_completed(void);
/* Most recently queued timeline value. D3D12 Queue::Signal snapshots this
 * value so fence completion follows GPU progress instead of host replay. */
uint64_t beer_vulkan_timeline_last_submitted(void);

/* Diagnostic accounting of host time spent in generic compute/graphics
 * dispatches. out = {compute count, compute ns, graphics count, graphics ns};
 * take() returns totals since the previous take() and resets them. */
void beer_vulkan_perf_account(int graphics, uint64_t ns);
uint64_t beer_vulkan_perf_threshold_ns(void);
void beer_vulkan_perf_take(uint64_t out[4]);

/* Used during final process teardown after all stage-owned resources are gone. */
void beer_vulkan_context_destroy(void);

#endif
