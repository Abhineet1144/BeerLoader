#define _GNU_SOURCE
#include "vulkan_context.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    void *library;
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    uint32_t queue_family_size;   /* queues the family offers */
    VkQueue queues[BEER_VULKAN_MAX_QUEUES]; /* [0] == queue; more with BEER_BD12_MULTI_QUEUE=1 */
    uint32_t queue_count;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
    PFN_vkDestroyInstance DestroyInstance;
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
    PFN_vkGetPhysicalDeviceFeatures GetPhysicalDeviceFeatures;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceFeatures2KHR GetPhysicalDeviceFeatures2KHR;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties;
    VkPhysicalDeviceFeatures enabled_features;
    uint32_t max_sampler_anisotropy;
    int maintenance1;
    int robustness2;
    int pipeline_robustness;
    int sampler_filter_minmax;
    int ray_tracing;
    int timeline_semaphore;
    int texel_buffer_alignment;
    int descriptor_buffer_ext, mutable_descriptor_ext;
    int fragment_shading_rate_ext; /* VK_KHR_fragment_shading_rate requested (BEER_BD12_VRS=1) */
    int mesh_shader_ext;          /* VK_EXT_mesh_shader requested (BEER_BD12_MESH=1) */
    int nvx_cuda;                 /* VK_NVX_binary_import + VK_NVX_image_view_handle: NVAPI CUDA kernels (DLSS) */
    BeerVulkanModern modern;
    int device_lost;
    VkSemaphore timeline;
    _Atomic uint64_t timeline_next_value;
    PFN_vkCreateDevice CreateDevice;
    PFN_vkDestroyDevice DestroyDevice;
    PFN_vkGetDeviceQueue GetDeviceQueue;
    PFN_vkDeviceWaitIdle DeviceWaitIdle;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkCreateSemaphore CreateSemaphore;
    PFN_vkDestroySemaphore DestroySemaphore;
    PFN_vkWaitSemaphoresKHR WaitSemaphoresKHR;
    PFN_vkGetSemaphoreCounterValueKHR GetSemaphoreCounterValueKHR;
    int initialized;
    int unavailable;
} BeerVulkanContext;

static BeerVulkanContext g_context;
static pthread_mutex_t g_context_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_queue_lock = PTHREAD_MUTEX_INITIALIZER;
/* Locks for queues 1.. (queue 0 uses g_queue_lock, shared with the presenter and older renderers). */
static pthread_mutex_t g_queue_locks[BEER_VULKAN_MAX_QUEUES] = {
    PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER };

/* VK_KHR_fragment_shading_rate: which D3D12 VRS tier the device can back, and the tile size BD12 reports.
 * Tier 1 needs per-draw rates; tier 2 adds per-primitive rates and a screen-space rate image. Features the tier
 * does not need are switched off in `features` before it is chained into device creation. */
static void query_shading_rate(VkPhysicalDeviceFragmentShadingRateFeaturesKHR *features)
{
    BeerVulkanModern *m = &g_context.modern;
    if (!features->pipelineFragmentShadingRate) {
        features->primitiveFragmentShadingRate = features->attachmentFragmentShadingRate = VK_FALSE;
        return;
    }
    VkPhysicalDeviceFragmentShadingRatePropertiesKHR props = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR
    };
    PFN_vkGetPhysicalDeviceProperties2 get_properties2 = (PFN_vkGetPhysicalDeviceProperties2)
        g_context.GetInstanceProcAddr(g_context.instance, "vkGetPhysicalDeviceProperties2");
    PFN_vkGetPhysicalDeviceFragmentShadingRatesKHR get_rates = (PFN_vkGetPhysicalDeviceFragmentShadingRatesKHR)
        g_context.GetInstanceProcAddr(g_context.instance, "vkGetPhysicalDeviceFragmentShadingRatesKHR");
    if (!get_properties2 || !get_rates) {
        features->pipelineFragmentShadingRate = features->primitiveFragmentShadingRate =
            features->attachmentFragmentShadingRate = VK_FALSE;
        return;
    }
    VkPhysicalDeviceProperties2 properties2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                                .pNext = &props };
    get_properties2(g_context.physical_device, &properties2);
    /* D3D12 AdditionalShadingRatesSupported: 2x4, 4x2 and 4x4 at one sample. */
    uint32_t count = 0;
    get_rates(g_context.physical_device, &count, NULL);
    VkPhysicalDeviceFragmentShadingRateKHR rates[32];
    if (count > 32) count = 32;
    for (uint32_t i = 0; i < count; ++i)
        rates[i] = (VkPhysicalDeviceFragmentShadingRateKHR){
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_KHR };
    get_rates(g_context.physical_device, &count, rates);
    for (uint32_t i = 0; i < count; ++i)
        if (rates[i].fragmentSize.width == 4 && rates[i].fragmentSize.height == 4 &&
            (rates[i].sampleCounts & VK_SAMPLE_COUNT_1_BIT))
            m->vrs_additional_rates = 1;
    /* D3D12 tiles are square, 8, 16 or 32 pixels; take the smallest the driver accepts. */
    uint32_t tile = 0;
    for (uint32_t t = 8; t <= 32 && !tile; t *= 2)
        if (t >= props.minFragmentShadingRateAttachmentTexelSize.width &&
            t >= props.minFragmentShadingRateAttachmentTexelSize.height &&
            t <= props.maxFragmentShadingRateAttachmentTexelSize.width &&
            t <= props.maxFragmentShadingRateAttachmentTexelSize.height)
            tile = t;
    const int tier2 = features->primitiveFragmentShadingRate && features->attachmentFragmentShadingRate &&
                      tile && props.fragmentShadingRateNonTrivialCombinerOps;
    if (!tier2) features->primitiveFragmentShadingRate = features->attachmentFragmentShadingRate = VK_FALSE;
    m->vrs_tier = tier2 ? 2 : 1;
    m->vrs_tile = tier2 ? tile : 0;
    m->vrs_sum_combiner = props.fragmentShadingRateNonTrivialCombinerOps;
    m->vrs_primitive_multi_viewport = tier2 && props.primitiveFragmentShadingRateWithMultipleViewports;
}

/* VK_EXT_mesh_shader backs D3D12 MeshShaderTier 1 when the device meets the D3D12 limits (256 output vertices and
 * primitives, 128 threads per group, 16 KiB amplification payload, 28 KiB mesh groupshared memory). Only the task and
 * mesh stages are enabled; multiview and queries are not used. */
static void query_mesh_shader(VkPhysicalDeviceMeshShaderFeaturesEXT *features,
                              const VkPhysicalDeviceFragmentShadingRateFeaturesKHR *shading_rate)
{
    VkPhysicalDeviceMeshShaderPropertiesEXT props = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT
    };
    PFN_vkGetPhysicalDeviceProperties2 get_properties2 = (PFN_vkGetPhysicalDeviceProperties2)
        g_context.GetInstanceProcAddr(g_context.instance, "vkGetPhysicalDeviceProperties2");
    if (get_properties2) {
        VkPhysicalDeviceProperties2 properties2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                                    .pNext = &props };
        get_properties2(g_context.physical_device, &properties2);
    }
    const int ok = get_properties2 && features->meshShader && features->taskShader &&
                   props.maxMeshOutputVertices >= 256 && props.maxMeshOutputPrimitives >= 256 &&
                   props.maxMeshWorkGroupInvocations >= 128 && props.maxTaskWorkGroupInvocations >= 128 &&
                   props.maxTaskPayloadSize >= 16384 && props.maxMeshSharedMemorySize >= 28672;
    features->multiviewMeshShader = VK_FALSE;
    features->meshShaderQueries = VK_FALSE;
    /* SV_ShadingRate written by a mesh shader (D3D12 OPTIONS10) when VRS tier 2 is on. */
    features->primitiveFragmentShadingRateMeshShader = ok && features->primitiveFragmentShadingRateMeshShader &&
                                                       shading_rate->primitiveFragmentShadingRate;
    if (!ok) features->meshShader = features->taskShader = VK_FALSE;
    g_context.modern.mesh_shader_tier = ok ? 10 : 0;
    g_context.modern.mesh_shading_rate = features->primitiveFragmentShadingRateMeshShader;
}

/* Experiment (BEER_VULKAN_GPU_KEEPALIVE=<MiB per submit>): keep the GPU busy on
 * the shared queue so the driver does not drop to idle clocks between the
 * bridge's short synchronous submissions. */
static void *gpu_keepalive_thread(void *unused)
{
    (void)unused;
    const char *env = getenv("BEER_VULKAN_GPU_KEEPALIVE");
    VkDeviceSize bytes = (VkDeviceSize)(env ? strtoul(env, NULL, 10) : 64) <<
                         20;
    VkDevice device = g_context.device;
#define KA_LOAD(type, name) \
    PFN_vk##name name = (PFN_vk##name)g_context.GetDeviceProcAddr(device, "vk" #name); \
    (void)sizeof(type); if (!name) return NULL
    KA_LOAD(int, CreateBuffer); KA_LOAD(int, GetBufferMemoryRequirements);
    KA_LOAD(int, AllocateMemory); KA_LOAD(int, BindBufferMemory);
    KA_LOAD(int, CreateCommandPool); KA_LOAD(int, AllocateCommandBuffers);
    KA_LOAD(int, BeginCommandBuffer); KA_LOAD(int, EndCommandBuffer);
    KA_LOAD(int, CmdFillBuffer); KA_LOAD(int, CmdPipelineBarrier);
    KA_LOAD(int, CreateFence); KA_LOAD(int, ResetFences);
    KA_LOAD(int, WaitForFences); KA_LOAD(int, QueueSubmit);
#undef KA_LOAD
    PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)
        g_context.GetInstanceProcAddr(g_context.instance,
                                      "vkGetPhysicalDeviceMemoryProperties");
    if (!get_memory_properties) return NULL;
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    VkBuffer buffer;
    if (CreateBuffer(device, &buffer_info, NULL, &buffer) != VK_SUCCESS)
        return NULL;
    VkMemoryRequirements requirements;
    GetBufferMemoryRequirements(device, buffer, &requirements);
    VkPhysicalDeviceMemoryProperties properties;
    get_memory_properties(g_context.physical_device, &properties);
    int type = -1;
    for (uint32_t i = 0; i < properties.memoryTypeCount && type < 0; ++i)
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (properties.memoryTypes[i].propertyFlags &
             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            type = (int)i;
    if (type < 0) return NULL;
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = (uint32_t)type
    };
    VkDeviceMemory memory;
    if (AllocateMemory(device, &allocation, NULL, &memory) != VK_SUCCESS ||
        BindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS)
        return NULL;
    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = g_context.queue_family
    };
    VkCommandPool pool;
    if (CreateCommandPool(device, &pool_info, NULL, &pool) != VK_SUCCESS)
        return NULL;
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };
    VkCommandBuffer command;
    if (AllocateCommandBuffers(device, &command_info, &command) != VK_SUCCESS)
        return NULL;
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
    };
    BeginCommandBuffer(command, &begin);
    VkMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT
    };
    for (int i = 0; i < 4; ++i) {
        CmdFillBuffer(command, buffer, 0, bytes & ~(VkDeviceSize)3, 0x5a5a5a5au);
        CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0,
                           NULL, 0, NULL);
    }
    EndCommandBuffer(command);
    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
    };
    VkFence fence;
    if (CreateFence(device, &fence_info, NULL, &fence) != VK_SUCCESS)
        return NULL;
    fprintf(stderr, "[VULKAN] GPU keepalive running (%lu MiB x4 per submit)\n",
            (unsigned long)(bytes >> 20));
    for (;;) {
        VkSubmitInfo submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &command
        };
        ResetFences(device, 1, &fence);
        pthread_mutex_lock(&g_queue_lock);
        VkResult submitted = QueueSubmit(g_context.queue, 1, &submit, fence);
        pthread_mutex_unlock(&g_queue_lock);
        if (submitted != VK_SUCCESS) return NULL;
        WaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    }
}

/* Diagnostic: BEER_VK_DEBUG_UTILS=1 enables VK_EXT_debug_utils and prints every
 * driver message, including the driver's own reason for a device loss. */
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_utils_message(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT types,
    const VkDebugUtilsMessengerCallbackDataEXT *data, void *user)
{
    (void)types; (void)user;
    fprintf(stderr, "[VKDEBUG] sev=0x%x %s\n", (unsigned)severity,
            data && data->pMessage ? data->pMessage : "(no message)");
    return VK_FALSE;
}

#define LOAD_INSTANCE(name) do { \
    g_context.name = (PFN_vk##name)g_context.GetInstanceProcAddr( \
        g_context.instance, "vk" #name); \
    if (!g_context.name) goto fail; \
} while (0)
#define LOAD_DEVICE(name) do { \
    g_context.name = (PFN_vk##name)g_context.GetDeviceProcAddr( \
        g_context.device, "vk" #name); \
    if (!g_context.name) goto fail; \
} while (0)

int beer_vulkan_context_initialize(void)
{
    pthread_mutex_lock(&g_context_lock);
    if (g_context.initialized) {
        pthread_mutex_unlock(&g_context_lock);
        return 1;
    }
    if (g_context.unavailable) {
        pthread_mutex_unlock(&g_context_lock);
        return 0;
    }

    const char *stage = "Vulkan loader";
    g_context.library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!g_context.library) goto fail;
    g_context.GetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)
        dlsym(g_context.library, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance create_instance = (PFN_vkCreateInstance)
        dlsym(g_context.library, "vkCreateInstance");
    if (!g_context.GetInstanceProcAddr || !create_instance) goto fail;

    stage = "shared instance";
    const char *instance_extensions[] = {
        "VK_KHR_surface",
        "VK_KHR_xlib_surface",
        "VK_KHR_get_physical_device_properties2",
        "VK_EXT_debug_utils"
    };
    const int debug_utils = getenv("BEER_VK_DEBUG_UTILS") != NULL;
    /* Request Vulkan 1.3 when the loader reports it so the physical device
     * can expose core 1.2/1.3 features; older stacks stay on 1.0. */
    uint32_t instance_version = VK_API_VERSION_1_0;
    PFN_vkEnumerateInstanceVersion enumerate_instance_version =
        (PFN_vkEnumerateInstanceVersion)g_context.GetInstanceProcAddr(
            NULL, "vkEnumerateInstanceVersion");
    if (enumerate_instance_version)
        enumerate_instance_version(&instance_version);
    VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Beer",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "Beer D3D11 Vulkan",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = instance_version >= VK_API_VERSION_1_3 ?
            VK_API_VERSION_1_3 : VK_API_VERSION_1_0
    };
    VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application,
        .enabledExtensionCount = debug_utils ? 4 : 3,
        .ppEnabledExtensionNames = instance_extensions
    };
    if (create_instance(&instance_info, NULL, &g_context.instance) != VK_SUCCESS)
        goto fail;
    if (debug_utils) {
        PFN_vkCreateDebugUtilsMessengerEXT create_messenger =
            (PFN_vkCreateDebugUtilsMessengerEXT)g_context.GetInstanceProcAddr(
                g_context.instance, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT messenger_info = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debug_utils_message
        };
        VkDebugUtilsMessengerEXT messenger;
        if (!create_messenger || create_messenger(g_context.instance,
                &messenger_info, NULL, &messenger) != VK_SUCCESS)
            fprintf(stderr, "[VULKAN] VK_EXT_debug_utils messenger unavailable\n");
    }
    LOAD_INSTANCE(DestroyInstance);
    LOAD_INSTANCE(EnumeratePhysicalDevices);
    LOAD_INSTANCE(GetPhysicalDeviceQueueFamilyProperties);
    LOAD_INSTANCE(GetPhysicalDeviceFeatures);
    LOAD_INSTANCE(GetPhysicalDeviceProperties);
    LOAD_INSTANCE(EnumerateDeviceExtensionProperties);
    LOAD_INSTANCE(CreateDevice);
    g_context.GetPhysicalDeviceFeatures2KHR =
        (PFN_vkGetPhysicalDeviceFeatures2KHR)g_context.GetInstanceProcAddr(
            g_context.instance, "vkGetPhysicalDeviceFeatures2KHR");
    if (!g_context.GetPhysicalDeviceFeatures2KHR) goto fail;
    g_context.GetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)
        g_context.GetInstanceProcAddr(g_context.instance, "vkGetDeviceProcAddr");
    if (!g_context.GetDeviceProcAddr) goto fail;

    stage = "shared graphics/compute queue";
    uint32_t physical_count = 0;
    if (g_context.EnumeratePhysicalDevices(g_context.instance, &physical_count,
                                            NULL) != VK_SUCCESS || !physical_count)
        goto fail;
    VkPhysicalDevice *physical_devices = calloc(physical_count,
                                                 sizeof(*physical_devices));
    if (!physical_devices) goto fail;
    g_context.EnumeratePhysicalDevices(g_context.instance, &physical_count,
                                        physical_devices);
    for (uint32_t p = 0; p < physical_count && !g_context.physical_device; ++p) {
        uint32_t queue_count = 0;
        g_context.GetPhysicalDeviceQueueFamilyProperties(physical_devices[p],
                                                         &queue_count, NULL);
        VkQueueFamilyProperties *queues = calloc(queue_count, sizeof(*queues));
        if (!queues) continue;
        g_context.GetPhysicalDeviceQueueFamilyProperties(physical_devices[p],
                                                         &queue_count, queues);
        for (uint32_t q = 0; q < queue_count; ++q) {
            VkQueueFlags required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            if ((queues[q].queueFlags & required) == required) {
                g_context.physical_device = physical_devices[p];
                g_context.queue_family = q;
                g_context.queue_family_size = queues[q].queueCount;
                break;
            }
        }
        free(queues);
    }
    free(physical_devices);
    if (!g_context.physical_device) goto fail;

    stage = "shared logical device";
    /* BEER_BD12_MULTI_QUEUE=1: extra queues of the same family so D3D12 compute and copy queues run beside the
     * direct queue (vkd3d-proton gives each D3D12 queue its own VkQueue). Same family: command pools, resource
     * sharing and barrier stages stay valid on every queue. */
    static const float priorities[BEER_VULKAN_MAX_QUEUES] = { 1.0f, 1.0f, 1.0f };
    uint32_t want_queues = 1;
    {
        const char *multi = getenv("BEER_BD12_MULTI_QUEUE");
        if (multi && *multi && *multi != '0') want_queues = BEER_VULKAN_MAX_QUEUES;
        if (want_queues > g_context.queue_family_size) want_queues = g_context.queue_family_size;
        if (!want_queues) want_queues = 1;
    }
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = g_context.queue_family,
        .queueCount = want_queues,
        .pQueuePriorities = priorities
    };
    /* VK_KHR_maintenance1 permits negative viewport heights, which graphics
     * execution uses to preserve D3D's upward-Y clip space. */
    const char *device_extensions[24] = { "VK_KHR_swapchain" };
    int rt_ext_as = 0, rt_ext_rtp = 0, rt_ext_rq = 0, rt_ext_dho = 0, nvx_binary = 0, nvx_view = 0;
    uint32_t device_extension_count = 1;
    /* D3D12 descriptor heaps as Vulkan descriptor buffers (graphics/d3d12v/bd12_heapdb.c): used by root-signature
     * translation (BEER_BD12_ROOT_PUSH=1) for now; BEER_NO_DESCRIPTOR_BUFFER=1 keeps them off. */
    const char *root_push_env = getenv("BEER_BD12_ROOT_PUSH");
    const int want_descriptor_buffer = root_push_env && *root_push_env && strcmp(root_push_env, "0") != 0 &&
                                       !getenv("BEER_NO_DESCRIPTOR_BUFFER");
    /* D3D12 variable-rate shading (BD12 OPTIONS6) on VK_KHR_fragment_shading_rate. Opt-in while it is validated. */
    const char *vrs_env = getenv("BEER_BD12_VRS");
    const int want_vrs = vrs_env && *vrs_env && strcmp(vrs_env, "0") != 0;
    /* D3D12 mesh and amplification shaders (OPTIONS7 MeshShaderTier) on VK_EXT_mesh_shader. Opt-in while validated. */
    const char *mesh_env = getenv("BEER_BD12_MESH");
    const int want_mesh = mesh_env && *mesh_env && strcmp(mesh_env, "0") != 0;
    {
        uint32_t available_count = 0;
        g_context.EnumerateDeviceExtensionProperties(
            g_context.physical_device, NULL, &available_count, NULL);
        VkExtensionProperties *available = available_count ?
            calloc(available_count, sizeof(*available)) : NULL;
        if (available &&
            g_context.EnumerateDeviceExtensionProperties(
                g_context.physical_device, NULL, &available_count,
                available) != VK_SUCCESS)
            available_count = 0;
        for (uint32_t i = 0; available && i < available_count; ++i) {
            const char *ext = available[i].extensionName;
            rt_ext_as  |= !strcmp(ext, "VK_KHR_acceleration_structure");
            rt_ext_rtp |= !strcmp(ext, "VK_KHR_ray_tracing_pipeline");
            rt_ext_rq  |= !strcmp(ext, "VK_KHR_ray_query");
            rt_ext_dho |= !strcmp(ext, "VK_KHR_deferred_host_operations");
            nvx_binary |= !strcmp(ext, "VK_NVX_binary_import");
            nvx_view   |= !strcmp(ext, "VK_NVX_image_view_handle");
            if (!g_context.maintenance1 &&
                strcmp(available[i].extensionName,
                       "VK_KHR_maintenance1") == 0) {
                device_extensions[device_extension_count++] =
                    "VK_KHR_maintenance1";
                g_context.maintenance1 = 1;
            } else if (!g_context.robustness2 &&
                       strcmp(available[i].extensionName,
                              VK_EXT_ROBUSTNESS_2_EXTENSION_NAME) == 0) {
                device_extensions[device_extension_count++] =
                    VK_EXT_ROBUSTNESS_2_EXTENSION_NAME;
                g_context.robustness2 = 1;
            } else if (!g_context.pipeline_robustness &&
                       strcmp(available[i].extensionName,
                              VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME) == 0) {
                device_extensions[device_extension_count++] =
                    VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME;
                g_context.pipeline_robustness = 1;
            } else if (!g_context.sampler_filter_minmax &&
                       strcmp(available[i].extensionName,
                              VK_EXT_SAMPLER_FILTER_MINMAX_EXTENSION_NAME) == 0) {
                device_extensions[device_extension_count++] =
                    VK_EXT_SAMPLER_FILTER_MINMAX_EXTENSION_NAME;
                g_context.sampler_filter_minmax = 1;
            } else if (!g_context.timeline_semaphore &&
                       strcmp(available[i].extensionName,
                              VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME) == 0) {
                device_extensions[device_extension_count++] =
                    VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME;
                g_context.timeline_semaphore = 1;
            } else if (!g_context.texel_buffer_alignment &&
                       strcmp(available[i].extensionName,
                              VK_EXT_TEXEL_BUFFER_ALIGNMENT_EXTENSION_NAME) == 0) {
                device_extensions[device_extension_count++] =
                    VK_EXT_TEXEL_BUFFER_ALIGNMENT_EXTENSION_NAME;
                g_context.texel_buffer_alignment = 1;
            } else if (!g_context.descriptor_buffer_ext && want_descriptor_buffer &&
                       strcmp(available[i].extensionName, "VK_EXT_descriptor_buffer") == 0) {
                device_extensions[device_extension_count++] = "VK_EXT_descriptor_buffer";
                g_context.descriptor_buffer_ext = 1;
            } else if (!g_context.mesh_shader_ext && want_mesh &&
                       strcmp(available[i].extensionName, "VK_EXT_mesh_shader") == 0) {
                device_extensions[device_extension_count++] = "VK_EXT_mesh_shader";
                g_context.mesh_shader_ext = 1;
            } else if (!g_context.fragment_shading_rate_ext && want_vrs &&
                       strcmp(available[i].extensionName, "VK_KHR_fragment_shading_rate") == 0) {
                device_extensions[device_extension_count++] = "VK_KHR_fragment_shading_rate";
                g_context.fragment_shading_rate_ext = 1;
            } else if (!g_context.mutable_descriptor_ext && want_descriptor_buffer &&
                       strcmp(available[i].extensionName, "VK_EXT_mutable_descriptor_type") == 0) {
                device_extensions[device_extension_count++] = "VK_EXT_mutable_descriptor_type";
                g_context.mutable_descriptor_ext = 1;
            } else if (getenv("BEER_VK_CHECKPOINTS") &&
                       strcmp(available[i].extensionName,
                              "VK_NV_device_diagnostic_checkpoints") == 0) {
                device_extensions[device_extension_count++] =
                    "VK_NV_device_diagnostic_checkpoints";
            }
        }
        free(available);
    }
    /* NVIDIA's CUDA-kernel interop (NVAPI D3D12 cubin shaders and CUDA texture/surface objects), used by NGX for
     * DLSS: loaded binaries and image-view handles. Both or neither; BEER_NO_NVX=1 leaves them off. */
    if (nvx_binary && nvx_view && !getenv("BEER_NO_NVX")) {
        device_extensions[device_extension_count++] = "VK_NVX_binary_import";
        device_extensions[device_extension_count++] = "VK_NVX_image_view_handle";
        g_context.nvx_cuda = 1;
    }
    /* DXR needs acceleration structures, ray tracing pipelines and ray query
     * together (plus deferred host operations, which acceleration structures
     * depend on). Enable them as a set and only when every feature is real. */
    VkPhysicalDeviceAccelerationStructureFeaturesKHR rt_as_features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR
    };
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rt_pipeline_features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR,
        .pNext = &rt_as_features
    };
    VkPhysicalDeviceRayQueryFeaturesKHR rt_query_features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR,
        .pNext = &rt_pipeline_features
    };
    if (rt_ext_as && rt_ext_rtp && rt_ext_rq && rt_ext_dho &&
        !getenv("BEER_NO_RT")) {
        VkPhysicalDeviceFeatures2 query = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext = &rt_query_features
        };
        g_context.GetPhysicalDeviceFeatures2KHR(g_context.physical_device,
                                                &query);
        if (rt_as_features.accelerationStructure &&
            rt_pipeline_features.rayTracingPipeline &&
            rt_query_features.rayQuery) {
            device_extensions[device_extension_count++] =
                "VK_KHR_acceleration_structure";
            device_extensions[device_extension_count++] =
                "VK_KHR_ray_tracing_pipeline";
            device_extensions[device_extension_count++] = "VK_KHR_ray_query";
            device_extensions[device_extension_count++] =
                "VK_KHR_deferred_host_operations";
            g_context.ray_tracing = 1;
        }
    }
    /* Translated D3D shaders need typed storage-image access and wider scalar
     * types. Enable only the optional features the device reports; the
     * compute executor rejects any shader whose capabilities are not covered. */
    VkPhysicalDeviceFeatures supported;
    memset(&supported, 0, sizeof(supported));
    g_context.GetPhysicalDeviceFeatures(g_context.physical_device, &supported);
    VkPhysicalDeviceRobustness2FeaturesEXT robustness2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT
    };
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline_features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES
    };
    VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptor_buffer = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT
    };
    VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT mutable_descriptor = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT
    };
    VkPhysicalDevicePipelineRobustnessFeaturesEXT pipeline_robustness = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT
    };
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR shading_rate = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR
    };
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh_shader = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT
    };
    VkPhysicalDeviceTexelBufferAlignmentPropertiesEXT texel_alignment = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_PROPERTIES_EXT
    };
    /* Core 1.2/1.3 feature structs replace the extension-specific timeline
     * struct (the two must not be chained together). */
    VkPhysicalDeviceProperties physical_properties;
    memset(&physical_properties, 0, sizeof(physical_properties));
    g_context.GetPhysicalDeviceProperties(g_context.physical_device,
                                          &physical_properties);
    if (g_context.texel_buffer_alignment) {
        PFN_vkGetPhysicalDeviceProperties2 get_properties2 =
            (PFN_vkGetPhysicalDeviceProperties2)g_context.GetInstanceProcAddr(
                g_context.instance, "vkGetPhysicalDeviceProperties2");
        if (get_properties2) {
            VkPhysicalDeviceProperties2 properties2 = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                .pNext = &texel_alignment
            };
            get_properties2(g_context.physical_device, &properties2);
        } else {
            g_context.texel_buffer_alignment = 0;
        }
    }
    const int core13 = (application.apiVersion >= VK_API_VERSION_1_3 &&
                        physical_properties.apiVersion >= VK_API_VERSION_1_3);
    VkPhysicalDeviceVulkan11Features v11_supported = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES
    };
    VkPhysicalDeviceVulkan12Features v12_supported = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &v11_supported
    };
    VkPhysicalDeviceVulkan13Features v13_supported = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext = &v12_supported
    };
    VkPhysicalDeviceVulkan11Features v11_enabled = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES
    };
    VkPhysicalDeviceVulkan12Features v12_enabled = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &v11_enabled
    };
    VkPhysicalDeviceVulkan13Features v13_enabled = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext = &v12_enabled
    };
    void *feature_chain = NULL;
    if (g_context.ray_tracing) {
        const VkBool32 traversal = rt_query_features.rayQuery;
        rt_as_features = (VkPhysicalDeviceAccelerationStructureFeaturesKHR){
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
            .accelerationStructure = VK_TRUE
        };
        rt_pipeline_features = (VkPhysicalDeviceRayTracingPipelineFeaturesKHR){
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR,
            .pNext = &rt_as_features,
            .rayTracingPipeline = VK_TRUE
        };
        rt_query_features = (VkPhysicalDeviceRayQueryFeaturesKHR){
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR,
            .pNext = &rt_pipeline_features,
            .rayQuery = traversal
        };
        feature_chain = &rt_query_features;
    }
    if (g_context.robustness2) {
        robustness2.pNext = feature_chain;
        feature_chain = &robustness2;
    }
    if (g_context.pipeline_robustness) {
        pipeline_robustness.pNext = feature_chain;
        feature_chain = &pipeline_robustness;
    }
    /* Both or neither: heaps are one mutable-type array per descriptor buffer. */
    if (g_context.descriptor_buffer_ext && g_context.mutable_descriptor_ext) {
        descriptor_buffer.pNext = feature_chain;
        mutable_descriptor.pNext = &descriptor_buffer;
        feature_chain = &mutable_descriptor;
    }
    if (g_context.fragment_shading_rate_ext) {
        shading_rate.pNext = feature_chain;
        feature_chain = &shading_rate;
    }
    if (g_context.mesh_shader_ext) {
        mesh_shader.pNext = feature_chain;
        feature_chain = &mesh_shader;
    }
    if (g_context.timeline_semaphore && !core13) {
        timeline_features.pNext = feature_chain;
        feature_chain = &timeline_features;
    }
    if (feature_chain) {
        VkPhysicalDeviceFeatures2 features2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext = feature_chain
        };
        g_context.GetPhysicalDeviceFeatures2KHR(g_context.physical_device,
                                                &features2);
    }
    g_context.pipeline_robustness = g_context.pipeline_robustness &&
        pipeline_robustness.pipelineRobustness;
    /* Only the base features: capture/replay and push descriptors cost driver work and are not used. */
    descriptor_buffer.descriptorBufferCaptureReplay = VK_FALSE;
    descriptor_buffer.descriptorBufferImageLayoutIgnored = VK_FALSE;
    descriptor_buffer.descriptorBufferPushDescriptors = VK_FALSE;
    g_context.modern.descriptor_buffer = g_context.descriptor_buffer_ext && g_context.mutable_descriptor_ext &&
                                         descriptor_buffer.descriptorBuffer &&
                                         mutable_descriptor.mutableDescriptorType;
    g_context.modern.null_descriptor = g_context.robustness2 && robustness2.nullDescriptor;
    if (g_context.fragment_shading_rate_ext) query_shading_rate(&shading_rate);
    if (g_context.mesh_shader_ext) query_mesh_shader(&mesh_shader, &shading_rate);
    if (core13) {
        VkPhysicalDeviceFeatures2 features2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext = &v13_supported
        };
        g_context.GetPhysicalDeviceFeatures2KHR(g_context.physical_device,
                                                &features2);
        const VkPhysicalDeviceVulkan12Features *s12 = &v12_supported;
        const VkPhysicalDeviceVulkan13Features *s13 = &v13_supported;
        v12_enabled.timelineSemaphore = s12->timelineSemaphore;
        v12_enabled.bufferDeviceAddress = s12->bufferDeviceAddress;
        v12_enabled.scalarBlockLayout = s12->scalarBlockLayout;
        v12_enabled.hostQueryReset = s12->hostQueryReset;
        v12_enabled.drawIndirectCount = s12->drawIndirectCount;
        v12_enabled.descriptorIndexing = s12->descriptorIndexing;
        v12_enabled.runtimeDescriptorArray = s12->runtimeDescriptorArray;
        v12_enabled.descriptorBindingPartiallyBound =
            s12->descriptorBindingPartiallyBound;
        v12_enabled.descriptorBindingVariableDescriptorCount =
            s12->descriptorBindingVariableDescriptorCount;
        v12_enabled.descriptorBindingUpdateUnusedWhilePending =
            s12->descriptorBindingUpdateUnusedWhilePending;
        v12_enabled.descriptorBindingSampledImageUpdateAfterBind =
            s12->descriptorBindingSampledImageUpdateAfterBind;
        v12_enabled.descriptorBindingStorageImageUpdateAfterBind =
            s12->descriptorBindingStorageImageUpdateAfterBind;
        v12_enabled.descriptorBindingStorageBufferUpdateAfterBind =
            s12->descriptorBindingStorageBufferUpdateAfterBind;
        v12_enabled.descriptorBindingUniformBufferUpdateAfterBind =
            s12->descriptorBindingUniformBufferUpdateAfterBind;
        v12_enabled.descriptorBindingUniformTexelBufferUpdateAfterBind =
            s12->descriptorBindingUniformTexelBufferUpdateAfterBind;
        v12_enabled.descriptorBindingStorageTexelBufferUpdateAfterBind =
            s12->descriptorBindingStorageTexelBufferUpdateAfterBind;
        v12_enabled.shaderSampledImageArrayNonUniformIndexing =
            s12->shaderSampledImageArrayNonUniformIndexing;
        v12_enabled.shaderStorageImageArrayNonUniformIndexing =
            s12->shaderStorageImageArrayNonUniformIndexing;
        v12_enabled.shaderStorageBufferArrayNonUniformIndexing =
            s12->shaderStorageBufferArrayNonUniformIndexing;
        v12_enabled.shaderUniformBufferArrayNonUniformIndexing =
            s12->shaderUniformBufferArrayNonUniformIndexing;
        v12_enabled.shaderUniformTexelBufferArrayNonUniformIndexing =
            s12->shaderUniformTexelBufferArrayNonUniformIndexing;
        v12_enabled.shaderStorageTexelBufferArrayNonUniformIndexing =
            s12->shaderStorageTexelBufferArrayNonUniformIndexing;
        /* Native 16-bit shader operations (D3D12 Native16BitShaderOpsSupported): DXIL with 16-bit types is
         * translated to Float16/Int16 arithmetic and 16-bit buffer loads/stores. */
        v12_enabled.shaderFloat16 = s12->shaderFloat16;
        /* 64-bit atomics on buffers and group-shared memory (D3D12 AtomicInt64OnDescriptorHeapResource /
         * AtomicInt64OnGroupShared); RE Engine titles refuse GPUs without them. */
        v12_enabled.shaderBufferInt64Atomics = s12->shaderBufferInt64Atomics;
        v12_enabled.shaderSharedInt64Atomics = s12->shaderSharedInt64Atomics;
        /* Shaders may declare these, and the translators emit them: DrawParameters (draw index, base vertex and
         * instance), wave operations on 8/16/64-bit types (mesh shaders decode packed vertices with them), and the
         * min/max sampler reduction VK_EXT_sampler_filter_minmax exposes (core 1.2 needs the feature bit too). */
        v11_enabled.shaderDrawParameters = v11_supported.shaderDrawParameters;
        v12_enabled.shaderSubgroupExtendedTypes = s12->shaderSubgroupExtendedTypes;
        v12_enabled.samplerFilterMinmax = s12->samplerFilterMinmax;
        /* Layer / viewport index written from vertex and domain shaders (D3D12 VPAndRTArrayIndexFromAnyShader...). */
        v12_enabled.shaderOutputViewportIndex = s12->shaderOutputViewportIndex;
        v12_enabled.shaderOutputLayer = s12->shaderOutputLayer;
        v11_enabled.storageBuffer16BitAccess = v11_supported.storageBuffer16BitAccess;
        v11_enabled.uniformAndStorageBuffer16BitAccess = v11_supported.uniformAndStorageBuffer16BitAccess;
        v13_enabled.synchronization2 = s13->synchronization2;
        v13_enabled.dynamicRendering = s13->dynamicRendering;
        v13_enabled.maintenance4 = s13->maintenance4;
        v13_enabled.shaderDemoteToHelperInvocation =
            s13->shaderDemoteToHelperInvocation;
        v11_enabled.pNext = feature_chain;
        feature_chain = &v13_enabled;
        g_context.timeline_semaphore = v12_enabled.timelineSemaphore;
        g_context.modern.api_version = physical_properties.apiVersion;
        g_context.modern.buffer_device_address =
            v12_enabled.bufferDeviceAddress;
        g_context.modern.synchronization2 = v13_enabled.synchronization2;
        g_context.modern.dynamic_rendering = v13_enabled.dynamicRendering;
        g_context.modern.descriptor_indexing =
            v12_enabled.descriptorIndexing &&
            v12_enabled.runtimeDescriptorArray &&
            v12_enabled.descriptorBindingPartiallyBound &&
            v12_enabled.descriptorBindingSampledImageUpdateAfterBind &&
            v12_enabled.descriptorBindingStorageImageUpdateAfterBind &&
            v12_enabled.descriptorBindingStorageBufferUpdateAfterBind;
        g_context.modern.scalar_block_layout = v12_enabled.scalarBlockLayout;
        g_context.modern.host_query_reset = v12_enabled.hostQueryReset;
        g_context.modern.maintenance4 = v13_enabled.maintenance4;
        g_context.modern.demote_to_helper =
            v13_enabled.shaderDemoteToHelperInvocation;
    } else {
        g_context.timeline_semaphore = g_context.timeline_semaphore &&
            timeline_features.timelineSemaphore;
    }
    if (g_context.texel_buffer_alignment) {
        g_context.modern.uniform_texel_buffer_offset_alignment =
            texel_alignment.uniformTexelBufferOffsetAlignmentBytes;
        g_context.modern.storage_texel_buffer_offset_alignment =
            texel_alignment.storageTexelBufferOffsetAlignmentBytes;
        g_context.modern.uniform_texel_buffer_offset_single_texel =
            texel_alignment.uniformTexelBufferOffsetSingleTexelAlignment;
        g_context.modern.storage_texel_buffer_offset_single_texel =
            texel_alignment.storageTexelBufferOffsetSingleTexelAlignment;
    }
    /* D3D requires defined out-of-bounds buffer and image behavior. Core
     * robustness covers buffers, while robustness2 extends this to images and
     * tighter buffer bounds used by translated structured/typed views. */
    g_context.enabled_features.robustBufferAccess =
        supported.robustBufferAccess;
    g_context.enabled_features.samplerAnisotropy = supported.samplerAnisotropy;
    const VkPhysicalDeviceProperties properties = physical_properties;
    g_context.max_sampler_anisotropy = supported.samplerAnisotropy ?
        (uint32_t)properties.limits.maxSamplerAnisotropy : 1u;
    robustness2.robustBufferAccess2 = robustness2.robustBufferAccess2;
    robustness2.robustImageAccess2 = robustness2.robustImageAccess2;
    robustness2.nullDescriptor = robustness2.nullDescriptor;
    g_context.enabled_features.shaderStorageImageExtendedFormats =
        supported.shaderStorageImageExtendedFormats;
    g_context.enabled_features.shaderStorageImageReadWithoutFormat =
        supported.shaderStorageImageReadWithoutFormat;
    g_context.enabled_features.shaderStorageImageWriteWithoutFormat =
        supported.shaderStorageImageWriteWithoutFormat;
    g_context.enabled_features.shaderImageGatherExtended =
        supported.shaderImageGatherExtended;
    /* Opt-in draw diagnostics use core Vulkan pipeline-statistics queries to
     * prove whether translated draws reach vertex, primitive and fragment
     * execution. Keep this conditional so unsupported devices still run. */
    g_context.enabled_features.pipelineStatisticsQuery =
        supported.pipelineStatisticsQuery;
    g_context.enabled_features.shaderInt16 = supported.shaderInt16;
    g_context.modern.native_16bit = core13 && v12_enabled.shaderFloat16 && supported.shaderInt16 &&
        v11_enabled.storageBuffer16BitAccess && v11_enabled.uniformAndStorageBuffer16BitAccess;
    g_context.modern.buffer_int64_atomics = core13 && supported.shaderInt64 && v12_enabled.shaderBufferInt64Atomics;
    g_context.modern.shared_int64_atomics = core13 && supported.shaderInt64 && v12_enabled.shaderSharedInt64Atomics;
    g_context.enabled_features.shaderInt64 = supported.shaderInt64;
    g_context.enabled_features.shaderFloat64 = supported.shaderFloat64;
    /* Features a D3D12-on-Vulkan core needs for full pipeline-state coverage;
     * each is enabled only when the device supports it. */
#define BEER_ENABLE_FEATURE(name) \
    g_context.enabled_features.name = supported.name
    BEER_ENABLE_FEATURE(independentBlend);
    BEER_ENABLE_FEATURE(dualSrcBlend);
    BEER_ENABLE_FEATURE(logicOp);
    BEER_ENABLE_FEATURE(depthClamp);
    BEER_ENABLE_FEATURE(depthBiasClamp);
    BEER_ENABLE_FEATURE(depthBounds);
    BEER_ENABLE_FEATURE(fillModeNonSolid);
    BEER_ENABLE_FEATURE(wideLines);
    BEER_ENABLE_FEATURE(multiDrawIndirect);
    BEER_ENABLE_FEATURE(drawIndirectFirstInstance);
    BEER_ENABLE_FEATURE(fullDrawIndexUint32);
    BEER_ENABLE_FEATURE(geometryShader);
    BEER_ENABLE_FEATURE(tessellationShader);
    BEER_ENABLE_FEATURE(shaderClipDistance);
    BEER_ENABLE_FEATURE(shaderCullDistance);
    BEER_ENABLE_FEATURE(multiViewport);
    BEER_ENABLE_FEATURE(textureCompressionBC);
    BEER_ENABLE_FEATURE(imageCubeArray);
    BEER_ENABLE_FEATURE(sampleRateShading);
    BEER_ENABLE_FEATURE(fragmentStoresAndAtomics);
    BEER_ENABLE_FEATURE(vertexPipelineStoresAndAtomics);
    BEER_ENABLE_FEATURE(shaderStorageImageMultisample);
    BEER_ENABLE_FEATURE(occlusionQueryPrecise);
#undef BEER_ENABLE_FEATURE
    const void *device_features = feature_chain;
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = device_features,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = device_extension_count,
        .ppEnabledExtensionNames = device_extensions,
        .pEnabledFeatures = &g_context.enabled_features
    };
    if (g_context.CreateDevice(g_context.physical_device, &device_info, NULL,
                               &g_context.device) != VK_SUCCESS)
        goto fail;
    LOAD_DEVICE(DestroyDevice);
    LOAD_DEVICE(GetDeviceQueue);
    LOAD_DEVICE(DeviceWaitIdle);
    LOAD_DEVICE(QueueSubmit);
    LOAD_DEVICE(CreateSemaphore);
    LOAD_DEVICE(DestroySemaphore);
    if (g_context.timeline_semaphore) {
        g_context.WaitSemaphoresKHR = (PFN_vkWaitSemaphoresKHR)
            g_context.GetDeviceProcAddr(g_context.device, "vkWaitSemaphoresKHR");
        g_context.GetSemaphoreCounterValueKHR =
            (PFN_vkGetSemaphoreCounterValueKHR)g_context.GetDeviceProcAddr(
                g_context.device, "vkGetSemaphoreCounterValueKHR");
        if (!g_context.WaitSemaphoresKHR ||
            !g_context.GetSemaphoreCounterValueKHR)
            g_context.timeline_semaphore = 0;
    }
    g_context.GetDeviceQueue(g_context.device, g_context.queue_family, 0,
                             &g_context.queue);
    g_context.queues[0] = g_context.queue;
    for (uint32_t i = 1; i < want_queues; ++i)
        g_context.GetDeviceQueue(g_context.device, g_context.queue_family, i, &g_context.queues[i]);
    g_context.queue_count = want_queues;
    if (g_context.timeline_semaphore) {
        VkSemaphoreTypeCreateInfo timeline_type = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
            .initialValue = 0
        };
        VkSemaphoreCreateInfo semaphore_info = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &timeline_type
        };
        if (g_context.CreateSemaphore(g_context.device, &semaphore_info, NULL,
                                      &g_context.timeline) != VK_SUCCESS)
            g_context.timeline_semaphore = 0;
    }
    g_context.initialized = 1;
    if (getenv("BEER_VULKAN_GPU_KEEPALIVE")) {
        pthread_t keepalive;
        if (pthread_create(&keepalive, NULL, gpu_keepalive_thread, NULL) == 0)
            pthread_detach(keepalive);
    }
    fprintf(stderr,
            "[VULKAN] shared device initialized (queue family %u, queues %u, "
            "robust-buffer=%u robust-buffer2=%u robust-image2=%u "
            "pipeline-robustness=%u timeline=%u)\n",
            g_context.queue_family, g_context.queue_count,
            g_context.enabled_features.robustBufferAccess,
            g_context.robustness2 ? robustness2.robustBufferAccess2 : 0,
            g_context.robustness2 ? robustness2.robustImageAccess2 : 0,
            g_context.pipeline_robustness, g_context.timeline_semaphore);
    fprintf(stderr,
            "[VULKAN] core features: api=%u.%u bda=%d sync2=%d dynrender=%d "
            "descriptor-indexing=%d scalar-layout=%d\n",
            VK_VERSION_MAJOR(g_context.modern.api_version),
            VK_VERSION_MINOR(g_context.modern.api_version),
            g_context.modern.buffer_device_address,
            g_context.modern.synchronization2,
            g_context.modern.dynamic_rendering,
            g_context.modern.descriptor_indexing,
            g_context.modern.scalar_block_layout);
    pthread_mutex_unlock(&g_context_lock);
    return 1;

fail:
    fprintf(stderr, "[VULKAN] shared context unavailable at %s\n", stage);
    if (g_context.device && g_context.DestroyDevice)
        g_context.DestroyDevice(g_context.device, NULL);
    if (g_context.instance && g_context.DestroyInstance)
        g_context.DestroyInstance(g_context.instance, NULL);
    if (g_context.library) dlclose(g_context.library);
    memset(&g_context, 0, sizeof(g_context));
    g_context.unavailable = 1;
    pthread_mutex_unlock(&g_context_lock);
    return 0;
}

VkInstance beer_vulkan_instance(void) { return g_context.instance; }
VkPhysicalDevice beer_vulkan_physical_device(void) { return g_context.physical_device; }
VkDevice beer_vulkan_device(void) { return g_context.device; }
VkQueue beer_vulkan_queue(void) { return g_context.queue; }
uint32_t beer_vulkan_queue_family(void) { return g_context.queue_family; }
const VkPhysicalDeviceFeatures *beer_vulkan_enabled_features(void)
{
    return &g_context.enabled_features;
}
const BeerVulkanModern *beer_vulkan_modern(void)
{
    return &g_context.modern;
}
int beer_vulkan_maintenance1_enabled(void)
{
    return g_context.maintenance1;
}
int beer_vulkan_nvx_cuda_enabled(void)
{
    return g_context.nvx_cuda;
}

int beer_vulkan_ray_tracing_enabled(void)
{
    return g_context.ray_tracing;
}
int beer_vulkan_sampler_filter_minmax_enabled(void)
{
    return g_context.sampler_filter_minmax;
}
uint32_t beer_vulkan_max_sampler_anisotropy(void)
{
    return g_context.max_sampler_anisotropy > 1u ?
        g_context.max_sampler_anisotropy : 1u;
}
int beer_vulkan_pipeline_robustness_enabled(void)
{
    return g_context.pipeline_robustness;
}
int beer_vulkan_device_lost(void)
{
    return __atomic_load_n(&g_context.device_lost, __ATOMIC_ACQUIRE);
}
void beer_vulkan_record_result(VkResult result, const char *operation)
{
    if (result != VK_ERROR_DEVICE_LOST) return;
    if (!__atomic_exchange_n(&g_context.device_lost, 1, __ATOMIC_ACQ_REL))
        fprintf(stderr,
                "[VULKAN] device lost during %s; suppressing further GPU work\n",
                operation ? operation : "unknown operation");
}
PFN_vkGetInstanceProcAddr beer_vulkan_get_instance_proc_addr(void)
{
    return g_context.GetInstanceProcAddr;
}
PFN_vkGetDeviceProcAddr beer_vulkan_get_device_proc_addr(void)
{
    return g_context.GetDeviceProcAddr;
}
void beer_vulkan_queue_lock(void) { pthread_mutex_lock(&g_queue_lock); }
void beer_vulkan_queue_unlock(void) { pthread_mutex_unlock(&g_queue_lock); }
uint32_t beer_vulkan_queue_count(void) { return g_context.queue_count ? g_context.queue_count : 1; }
VkQueue beer_vulkan_queue_at(uint32_t index)
{
    return index < g_context.queue_count ? g_context.queues[index] : g_context.queue;
}
void beer_vulkan_queue_lock_at(uint32_t index)
{
    if (!index || index >= g_context.queue_count) pthread_mutex_lock(&g_queue_lock);
    else pthread_mutex_lock(&g_queue_locks[index]);
}
void beer_vulkan_queue_unlock_at(uint32_t index)
{
    if (!index || index >= g_context.queue_count) pthread_mutex_unlock(&g_queue_lock);
    else pthread_mutex_unlock(&g_queue_locks[index]);
}

int beer_vulkan_timeline_enabled(void)
{
    return g_context.timeline_semaphore && g_context.timeline;
}

uint64_t beer_vulkan_timeline_submit(const VkCommandBuffer *commands,
                                     uint32_t command_count)
{
    if (!beer_vulkan_timeline_enabled() || !commands || !command_count ||
        beer_vulkan_device_lost())
        return 0;
    /* Timeline values and queue submission are one externally synchronized
     * operation. Assigning the value outside the queue lock allowed concurrent
     * graphics/compute callers to race the monotonically increasing payload. */
    beer_vulkan_queue_lock();
    const uint64_t value = atomic_fetch_add_explicit(
        &g_context.timeline_next_value, 1, memory_order_relaxed) + 1;
    VkTimelineSemaphoreSubmitInfo timeline_info = {
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .signalSemaphoreValueCount = 1,
        .pSignalSemaphoreValues = &value
    };
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = &timeline_info,
        .commandBufferCount = command_count,
        .pCommandBuffers = commands,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &g_context.timeline
    };
    VkResult result = g_context.QueueSubmit(g_context.queue, 1, &submit,
                                             VK_NULL_HANDLE);
    beer_vulkan_queue_unlock();
    beer_vulkan_record_result(result, "timeline queue submission");
    return result == VK_SUCCESS ? value : 0;
}

int beer_vulkan_timeline_wait(uint64_t value, uint64_t timeout_ns)
{
    if (!value || !beer_vulkan_timeline_enabled()) return 0;
    VkSemaphoreWaitInfo wait_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores = &g_context.timeline,
        .pValues = &value
    };
    VkResult result = g_context.WaitSemaphoresKHR(
        g_context.device, &wait_info, timeout_ns);
    beer_vulkan_record_result(result, "timeline semaphore wait");
    return result == VK_SUCCESS;
}

uint64_t beer_vulkan_timeline_completed(void)
{
    uint64_t value = 0;
    if (!beer_vulkan_timeline_enabled() ||
        g_context.GetSemaphoreCounterValueKHR(g_context.device,
                                               g_context.timeline,
                                               &value) != VK_SUCCESS)
        return 0;
    return value;
}

uint64_t beer_vulkan_timeline_last_submitted(void)
{
    return beer_vulkan_timeline_enabled() ? atomic_load_explicit(
        &g_context.timeline_next_value, memory_order_acquire) : 0;
}

static _Atomic uint64_t g_perf[4];

void beer_vulkan_perf_account(int graphics, uint64_t ns)
{
    atomic_fetch_add(&g_perf[graphics ? 2 : 0], 1);
    atomic_fetch_add(&g_perf[graphics ? 3 : 1], ns);
}

uint64_t beer_vulkan_perf_threshold_ns(void)
{
    static uint64_t threshold = UINT64_MAX;
    if (threshold == UINT64_MAX) {
        const char *text = getenv("BEER_VULKAN_PERF_MIN_US");
        threshold = text ? (uint64_t)strtoull(text, NULL, 10) * 1000u
                         : UINT64_C(50000000);
    }
    return threshold;
}

void beer_vulkan_perf_take(uint64_t out[4])
{
    for (int i = 0; i < 4; ++i) out[i] = atomic_exchange(&g_perf[i], 0);
}

void beer_vulkan_context_destroy(void)
{
    pthread_mutex_lock(&g_context_lock);
    if (g_context.device && g_context.DeviceWaitIdle)
        g_context.DeviceWaitIdle(g_context.device);
    if (g_context.device && g_context.timeline && g_context.DestroySemaphore)
        g_context.DestroySemaphore(g_context.device, g_context.timeline, NULL);
    if (g_context.device && g_context.DestroyDevice)
        g_context.DestroyDevice(g_context.device, NULL);
    if (g_context.instance && g_context.DestroyInstance)
        g_context.DestroyInstance(g_context.instance, NULL);
    if (g_context.library) dlclose(g_context.library);
    memset(&g_context, 0, sizeof(g_context));
    pthread_mutex_unlock(&g_context_lock);
}

static int host_gpu_info_real(BeerHostGpuInfo *out)
{
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!beer_vulkan_context_initialize() || !g_context.physical_device)
        return 0;
    VkPhysicalDeviceProperties props;
    memset(&props, 0, sizeof(props));
    g_context.GetPhysicalDeviceProperties(g_context.physical_device, &props);
    snprintf(out->name, sizeof(out->name), "%s", props.deviceName);
    out->vendor_id = props.vendorID;
    out->device_id = props.deviceID;
    out->luid = (((uint64_t)props.vendorID << 16) | props.deviceID) & 0x7fffffffu;   /* HighPart 0, like Windows LUIDs */

    PFN_vkGetPhysicalDeviceMemoryProperties get_memory =
        (PFN_vkGetPhysicalDeviceMemoryProperties)g_context.GetInstanceProcAddr(
            g_context.instance, "vkGetPhysicalDeviceMemoryProperties");
    if (get_memory) {
        VkPhysicalDeviceMemoryProperties mem;
        get_memory(g_context.physical_device, &mem);
        for (uint32_t i = 0; i < mem.memoryHeapCount; ++i) {
            const uint64_t size = mem.memoryHeaps[i].size;
            if (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                /* A small BAR window heap is also DEVICE_LOCAL; the real VRAM
                 * heap is the largest one. */
                if (size > out->dedicated_video_memory)
                    out->dedicated_video_memory = size;
            } else if (size > out->shared_system_memory) {
                out->shared_system_memory = size;
            }
        }
    }

    PFN_vkEnumerateDeviceExtensionProperties enumerate =
        (PFN_vkEnumerateDeviceExtensionProperties)
            g_context.GetInstanceProcAddr(
                g_context.instance, "vkEnumerateDeviceExtensionProperties");
    PFN_vkGetPhysicalDeviceFeatures2 features2_fn =
        (PFN_vkGetPhysicalDeviceFeatures2)g_context.GetInstanceProcAddr(
            g_context.instance, "vkGetPhysicalDeviceFeatures2");
    if (!enumerate || !features2_fn) return 1;
    uint32_t count = 0;
    enumerate(g_context.physical_device, NULL, &count, NULL);
    VkExtensionProperties *extensions = count ?
        calloc(count, sizeof(*extensions)) : NULL;
    int has_rt = 0, has_as = 0, has_rq = 0, has_mesh = 0, has_vrs = 0;
    if (extensions &&
        enumerate(g_context.physical_device, NULL, &count, extensions) ==
            VK_SUCCESS) {
        for (uint32_t i = 0; i < count; ++i) {
            const char *n = extensions[i].extensionName;
            has_rt   |= !strcmp(n, "VK_KHR_ray_tracing_pipeline");
            has_as   |= !strcmp(n, "VK_KHR_acceleration_structure");
            has_rq   |= !strcmp(n, "VK_KHR_ray_query");
            has_mesh |= !strcmp(n, "VK_EXT_mesh_shader");
            has_vrs  |= !strcmp(n, "VK_KHR_fragment_shading_rate");
        }
    }
    free(extensions);
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rt = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR };
    VkPhysicalDeviceAccelerationStructureFeaturesKHR as = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
        .pNext = &rt };
    VkPhysicalDeviceRayQueryFeaturesKHR rq = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR,
        .pNext = &as };
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT,
        .pNext = &rq };
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR vrs = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR,
        .pNext = &mesh };
    VkPhysicalDeviceFeatures2 features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &vrs };
    features2_fn(g_context.physical_device, &features);
    out->ray_tracing_pipeline = has_rt && rt.rayTracingPipeline;
    out->acceleration_structure = has_as && as.accelerationStructure;
    out->ray_query = has_rq && rq.rayQuery;
    out->mesh_shader = has_mesh && mesh.meshShader;
    out->fragment_shading_rate = has_vrs && vrs.pipelineFragmentShadingRate;
    return 1;
}

/* What the guest is told about the GPU. BEER_SPOOF_GPU_NAME / _VENDOR / _DEVICE (hex) / _VRAM_MB override
 * the identity DXGI and the display-device APIs report; the real Vulkan device still does the rendering. */
int beer_vulkan_real_gpu_ids(uint32_t *vendor, uint32_t *device)
{
    BeerHostGpuInfo info;
    if (!host_gpu_info_real(&info)) return 0;
    *vendor = info.vendor_id;
    *device = info.device_id;
    return 1;
}

int beer_vulkan_gpu_spoof(char *name, size_t name_size, uint32_t *vendor, uint32_t *device, uint64_t *vram_bytes)
{
    const char *n = getenv("BEER_SPOOF_GPU_NAME");
    const char *v = getenv("BEER_SPOOF_GPU_VENDOR");
    const char *d = getenv("BEER_SPOOF_GPU_DEVICE");
    const char *m = getenv("BEER_SPOOF_GPU_VRAM_MB");
    int mask = 0;
    if (n && *n && name) { snprintf(name, name_size, "%s", n); mask |= 1; }
    if (v && *v && vendor) { *vendor = (uint32_t)strtoul(v, NULL, 16); mask |= 2; }
    if (d && *d && device) { *device = (uint32_t)strtoul(d, NULL, 16); mask |= 4; }
    if (m && *m && vram_bytes) { *vram_bytes = strtoull(m, NULL, 10) * 1024ull * 1024ull; mask |= 8; }
    return mask;
}

int beer_vulkan_host_gpu_info(BeerHostGpuInfo *out)
{
    const int have = host_gpu_info_real(out);
    if (!have) return 0;
    const int mask = beer_vulkan_gpu_spoof(out->name, sizeof(out->name), &out->vendor_id, &out->device_id,
                                           &out->dedicated_video_memory);
    if (mask & (2 | 4))
        out->luid = (((uint64_t)out->vendor_id << 16) | out->device_id) & 0x7fffffffu;
    return 1;
}
