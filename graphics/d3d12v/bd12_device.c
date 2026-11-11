#define _GNU_SOURCE
#include "bd12.h"
#include "d3d12_dxil_translator.h"

#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/resource.h>
#include <sys/syscall.h>

static BD12Device *g_pc_device;
static uint64_t pc_now_ms(void);
/* Content hash of a pipeline cache blob (8 bytes at a time; a whole-cache rewrite is skipped when it matches). */
static uint64_t pc_hash(const void *data, size_t size)
{
    const uint8_t *p = data;
    uint64_t h = 0x9E3779B97F4A7C15ull ^ size;
    size_t i = 0;
    for (; i + 8 <= size; i += 8) { uint64_t w; memcpy(&w, p + i, 8); h = (h ^ w) * 0x100000001B3ull; h ^= h >> 29; }
    for (; i < size; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}
static void pc_atexit(void)
{
    if (g_pc_device && g_pc_device->pipeline_cache_new) bd12_pipeline_cache_save(g_pc_device);
}

#define BD12_BUFFER_USAGE                                                    \
    (VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |   \
     VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |                              \
     VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |                              \
     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |\
     VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |  \
     VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |                                   \
     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)

/* Ray tracing buffer usages exist only when the extensions are enabled. */
#define BD12_BUFFER_USAGE_RT                                                 \
    (VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |                \
     VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |  \
     VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR)
static VkBufferUsageFlags buffer_usage(void)
{
    return BD12_BUFFER_USAGE |
           (beer_vulkan_ray_tracing_enabled() ? BD12_BUFFER_USAGE_RT : 0);
}

static int32_t pick_memory_type(const VkPhysicalDeviceMemoryProperties *memory,
                                uint32_t type_bits,
                                VkMemoryPropertyFlags required,
                                VkMemoryPropertyFlags preferred,
                                VkMemoryPropertyFlags avoid)
{
    for (int pass = 0; pass < 3; ++pass) {
        for (uint32_t i = 0; i < memory->memoryTypeCount; ++i) {
            const VkMemoryPropertyFlags flags = memory->memoryTypes[i].propertyFlags;
            if (!(type_bits & (1u << i))) continue;
            if ((flags & required) != required) continue;
            if (pass < 2 && (flags & avoid)) continue;
            if (pass < 1 && (flags & preferred) != preferred) continue;
            return (int32_t)i;
        }
    }
    return -1;
}

/* Whether Beer keeps its own whole-cache VkPipelineCache file. Drivers with a reliable shader disk cache of their own
 * (NVIDIA proprietary; Mesa RADV, ANV, Turnip, NVK), given a per-program folder by the loader, serve repeat pipeline
 * builds from it, as they do for vkd3d-proton; the blob only duplicates that in memory (RE9: 1.1 GB file, +1.5 GB RSS,
 * load time unchanged without it). Other drivers keep the blob. BEER_BD12_PIPELINE_CACHE=1|0 forces it;
 * BEER_PIPELINE_CACHE_OFF=1 is the older off switch. */
static int use_pipeline_cache_blob(void)
{
    const char *force = getenv("BEER_BD12_PIPELINE_CACHE");
    if (force && *force) return strcmp(force, "0") != 0;
    if (getenv("BEER_PIPELINE_CACHE_OFF")) return 0;
    const BeerVulkanModern *m = beer_vulkan_modern();
    switch (m ? m->driver_id : 0) {
    case VK_DRIVER_ID_NVIDIA_PROPRIETARY:
    case VK_DRIVER_ID_MESA_RADV:
    case VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA:
    case VK_DRIVER_ID_MESA_TURNIP:
    case VK_DRIVER_ID_MESA_NVK:
        if (getenv("BEER_DRIVER_CACHE") && !strcmp(getenv("BEER_DRIVER_CACHE"), "0")) return 1;  /* no per-game cache */
        fprintf(stderr, "[BD12] pipeline cache: in memory only, repeat builds from the driver's shader disk cache\n");
        return 0;
    default:
        return 1;
    }
}

BD12Device *bd12_device_create(void)
{
    if (!beer_vulkan_context_initialize()) return NULL;
    const BeerVulkanModern *modern = beer_vulkan_modern();
    if (!modern->buffer_device_address || !modern->synchronization2 ||
        !beer_vulkan_timeline_enabled()) {
        fprintf(stderr, "[BD12] shared device lacks BDA/sync2/timeline\n");
        return NULL;
    }
    BD12Device *device = calloc(1, sizeof(*device));
    if (!device) return NULL;
    device->instance = beer_vulkan_instance();
    device->physical_device = beer_vulkan_physical_device();
    device->device = beer_vulkan_device();
    device->queue = beer_vulkan_queue();
    device->queue_family = beer_vulkan_queue_family();
    device->modern = modern;
    {
        const BeerD3D12DxilCaps caps = { modern->scalar_block_layout, modern->raw_access_chains, modern->vendor_id };
        beer_d3d12_dxil_set_caps(&caps);
    }
    pthread_mutex_init(&device->va_lock, NULL);
    pthread_mutex_init(&device->submit_barrier_lock, NULL);
    pthread_mutex_init(&device->view_lock, NULL);
    pthread_mutex_init(&device->zero_lock, NULL);

    PFN_vkGetDeviceProcAddr get_device = beer_vulkan_get_device_proc_addr();
    PFN_vkGetInstanceProcAddr get_instance = beer_vulkan_get_instance_proc_addr();
#define BD12_VK_FN(name)                                                      \
    device->vk.name = (PFN_vk##name)get_device(device->device, "vk" #name);   \
    if (!device->vk.name) {                                                   \
        fprintf(stderr, "[BD12] missing vk" #name "\n");                      \
        free(device);                                                         \
        return NULL;                                                          \
    }
#include "bd12_vk_functions.h"
#undef BD12_VK_FN
    if (modern->mesh_shader_tier) {
        device->CmdDrawMeshTasksEXT = (PFN_vkCmdDrawMeshTasksEXT)get_device(device->device, "vkCmdDrawMeshTasksEXT");
        device->CmdDrawMeshTasksIndirectEXT = (PFN_vkCmdDrawMeshTasksIndirectEXT)
            get_device(device->device, "vkCmdDrawMeshTasksIndirectEXT");
        device->CmdDrawMeshTasksIndirectCountEXT = (PFN_vkCmdDrawMeshTasksIndirectCountEXT)
            get_device(device->device, "vkCmdDrawMeshTasksIndirectCountEXT");
        if (!device->CmdDrawMeshTasksEXT || !device->CmdDrawMeshTasksIndirectEXT ||
            !device->CmdDrawMeshTasksIndirectCountEXT)
            device->CmdDrawMeshTasksEXT = NULL;
        else
            fprintf(stderr, "[BD12] mesh shaders (tier 1, SV_ShadingRate %d)\n", modern->mesh_shading_rate);
    }
    if (modern->vrs_tier) {
        device->CmdSetFragmentShadingRateKHR = (PFN_vkCmdSetFragmentShadingRateKHR)
            get_device(device->device, "vkCmdSetFragmentShadingRateKHR");
        if (device->CmdSetFragmentShadingRateKHR)
            fprintf(stderr, "[BD12] variable-rate shading tier %d (tile %u, 4x4 rates %d, sum combiner %d)\n",
                    modern->vrs_tier, modern->vrs_tile, modern->vrs_additional_rates, modern->vrs_sum_combiner);
    }

    PFN_vkGetPhysicalDeviceMemoryProperties get_memory =
        (PFN_vkGetPhysicalDeviceMemoryProperties)get_instance(
            device->instance, "vkGetPhysicalDeviceMemoryProperties");
    PFN_vkGetPhysicalDeviceProperties get_properties =
        (PFN_vkGetPhysicalDeviceProperties)get_instance(
            device->instance, "vkGetPhysicalDeviceProperties");
    device->get_format_properties = (PFN_vkGetPhysicalDeviceFormatProperties)get_instance(
        device->instance, "vkGetPhysicalDeviceFormatProperties");
    get_memory(device->physical_device, &device->memory);
    VkPhysicalDeviceProperties properties;
    get_properties(device->physical_device, &properties);
    snprintf(device->device_name, sizeof(device->device_name), "%s",
             properties.deviceName);
    {   /* Native wave width: the subgroup size range (Vulkan 1.3 subgroup size control). */
        PFN_vkGetPhysicalDeviceProperties2 get_properties2 =
            (PFN_vkGetPhysicalDeviceProperties2)get_instance(
                device->instance, "vkGetPhysicalDeviceProperties2");
        VkPhysicalDeviceSubgroupSizeControlProperties size_control = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES };
        VkPhysicalDeviceSubgroupProperties subgroup = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES, .pNext = &size_control };
        VkPhysicalDeviceProperties2 properties2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &subgroup };
        if (get_properties2) get_properties2(device->physical_device, &properties2);
        device->wave_lane_min = size_control.minSubgroupSize ? size_control.minSubgroupSize : subgroup.subgroupSize;
        device->wave_lane_max = size_control.maxSubgroupSize ? size_control.maxSubgroupSize : subgroup.subgroupSize;
        /* TotalLaneCount: games size persistent-thread work queues from it (one group per lane batch), so a
         * single wave's width makes them run on one wave. Vulkan has no direct query; derive it like vkd3d-proton:
         * NVIDIA SMs x warps per SM x 2, AMD compute units x 64, otherwise 32 waves. */
        PFN_vkEnumerateDeviceExtensionProperties enumerate =
            (PFN_vkEnumerateDeviceExtensionProperties)get_instance(device->instance,
                                                                   "vkEnumerateDeviceExtensionProperties");
        int nv_sm = 0, amd_core = 0;
        uint32_t count = 0;
        if (enumerate && enumerate(device->physical_device, NULL, &count, NULL) == VK_SUCCESS && count) {
            VkExtensionProperties *extensions = calloc(count, sizeof(*extensions));
            if (extensions && enumerate(device->physical_device, NULL, &count, extensions) == VK_SUCCESS)
                for (uint32_t i = 0; i < count; ++i) {
                    nv_sm |= !strcmp(extensions[i].extensionName, VK_NV_SHADER_SM_BUILTINS_EXTENSION_NAME);
                    amd_core |= !strcmp(extensions[i].extensionName, VK_AMD_SHADER_CORE_PROPERTIES_EXTENSION_NAME);
                }
            free(extensions);
        }
        VkPhysicalDeviceShaderSMBuiltinsPropertiesNV sm = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SM_BUILTINS_PROPERTIES_NV };
        VkPhysicalDeviceShaderCorePropertiesAMD core = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CORE_PROPERTIES_AMD };
        VkPhysicalDeviceProperties2 lanes2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        if (nv_sm) lanes2.pNext = &sm;
        else if (amd_core) lanes2.pNext = &core;
        if (get_properties2 && lanes2.pNext) get_properties2(device->physical_device, &lanes2);
        const uint32_t amd_units = core.shaderEngineCount * core.shaderArraysPerEngineCount *
                                   core.computeUnitsPerShaderArray;
        if (nv_sm && sm.shaderSMCount && sm.shaderWarpsPerSM)
            device->total_lane_count = sm.shaderSMCount * sm.shaderWarpsPerSM * 2;
        else if (amd_core && amd_units)
            device->total_lane_count = amd_units * 64;
        else
            device->total_lane_count = 32 * (subgroup.subgroupSize ? subgroup.subgroupSize : 32);
        fprintf(stderr, "[BD12] TotalLaneCount %u (%s)\n", device->total_lane_count,
                nv_sm ? "SMs x warps per SM x 2" : amd_units ? "compute units x 64" : "32 waves");
    }
    device->min_uniform_alignment =
        properties.limits.minUniformBufferOffsetAlignment;
    device->max_uniform_range = properties.limits.maxUniformBufferRange;
    { extern float g_bd12_timestamp_period; g_bd12_timestamp_period = properties.limits.timestampPeriod; }
    device->min_uniform_texel_alignment =
        modern->uniform_texel_buffer_offset_alignment ?
        modern->uniform_texel_buffer_offset_alignment :
        properties.limits.minTexelBufferOffsetAlignment;
    device->max_push_constants = properties.limits.maxPushConstantsSize;
    device->min_storage_buffer_alignment = properties.limits.minStorageBufferOffsetAlignment ?
        properties.limits.minStorageBufferOffsetAlignment : 1;
    device->min_storage_texel_alignment =
        modern->storage_texel_buffer_offset_alignment ?
        modern->storage_texel_buffer_offset_alignment :
        properties.limits.minTexelBufferOffsetAlignment;
    device->uniform_texel_single_alignment =
        modern->uniform_texel_buffer_offset_single_texel;
    device->storage_texel_single_alignment =
        modern->storage_texel_buffer_offset_single_texel;


    /* Identity of serialised pipeline libraries (ID3D12PipelineLibrary): GPU, driver and pipeline-cache UUID. It must
     * not depend on whether Beer keeps a cache file: a zero identity made RE9 reject its saved library and rebuild
     * every pipeline at the title screen. */
    memcpy(device->pipeline_identity, &properties.vendorID, 4);
    memcpy(device->pipeline_identity + 4, &properties.deviceID, 4);
    memcpy(device->pipeline_identity + 8, &properties.driverVersion, 4);
    memcpy(device->pipeline_identity + 12, properties.pipelineCacheUUID, 16);
    /* Persistent pipeline cache: driver pipeline compiles dominate first-run loading. */
    if (use_pipeline_cache_blob()) {
        /* BEER_CACHE_DIR (set by the manager) holds every shader cache; otherwise $XDG_CACHE_HOME/beer or
         * ~/.cache/beer. */
        const char *root = getenv("BEER_CACHE_DIR"), *xdg = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
        char dir[400];
        if (root && *root) snprintf(dir, sizeof(dir), "%s", root);
        else if (xdg && *xdg) snprintf(dir, sizeof(dir), "%s/beer", xdg);
        else snprintf(dir, sizeof(dir), "%s/.cache/beer", home ? home : "/tmp");
        for (size_t i = 1; dir[i]; ++i)                    /* mkdir -p */
            if (dir[i] == '/') { dir[i] = 0; mkdir(dir, 0700); dir[i] = '/'; }
        mkdir(dir, 0700);
        if (access(dir, W_OK) != 0)   /* e.g. NTFS mounted read-only by ntfs-3g after Windows hibernation */
            fprintf(stderr, "[BD12] WARNING: pipeline cache folder %s is not writable (%s): compiled pipelines will "
                            "not be saved, every launch compiles them again\n", dir, strerror(errno));
        /* One file per program (its executable name) and GPU/driver, as vkd3d-proton keeps one per application: a
         * single file shared by every game grew past the old 256 MB read limit, was then skipped at load, and the
         * next save replaced it with that session's few MB (Stellar Blade recompiled everything on every launch). */
        char program[96] = "unknown";
        {
            char exe[512];
            const char *guest = getenv("BEER_GUEST_EXE");
            const char *name = guest && *guest ? guest : NULL;
            if (!name) {
                const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
                if (n > 0) { exe[n] = 0; name = exe; }
            }
            if (name) {
                const char *base = name;
                for (const char *c = name; *c; ++c) if (*c == '/' || *c == '\\') base = c + 1;
                size_t k = 0;
                for (; base[k] && k + 1 < sizeof(program); ++k) {
                    const char ch = base[k];
                    program[k] = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                                 ch == '-' || ch == '_' || ch == '.' ? ch : '_';
                }
                program[k] = 0;
            }
        }
        snprintf(device->pipeline_cache_path, sizeof(device->pipeline_cache_path),
                 "%s/pipelines-%04x-%04x-%08x-%s.bin", dir, properties.vendorID, properties.deviceID,
                 properties.driverVersion, program);
        void *blob = NULL;
        size_t blob_size = 0;
        FILE *f = getenv("BEER_NO_PIPELINE_CACHE") ? NULL : fopen(device->pipeline_cache_path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (n > 0 && (blob = malloc((size_t)n)) && fread(blob, 1, (size_t)n, f) == (size_t)n) blob_size = (size_t)n;
            else { free(blob); blob = NULL; }
            fclose(f);
        }
        VkPipelineCacheCreateInfo cache_info = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
                                                 .initialDataSize = blob_size, .pInitialData = blob };
        if (device->vk.CreatePipelineCache(device->device, &cache_info, NULL, &device->pipeline_cache) != VK_SUCCESS) {
            cache_info.initialDataSize = 0; cache_info.pInitialData = NULL;
            if (device->vk.CreatePipelineCache(device->device, &cache_info, NULL, &device->pipeline_cache) != VK_SUCCESS)
                device->pipeline_cache = VK_NULL_HANDLE;
        }
        if (blob_size) fprintf(stderr, "[BD12] pipeline cache: loaded %zu bytes from %s\n", blob_size, device->pipeline_cache_path);
        device->pipeline_cache_hash = blob_size ? pc_hash(blob, blob_size) : 0;
        free(blob);
        /* With a loaded cache the first background save waits a full interval; a fresh one saves early. */
        device->pipeline_cache_saved_ms = blob_size ? pc_now_ms() : 0;
        g_pc_device = device;
        atexit(pc_atexit);
    } else {
        /* The driver's disk cache supplies repeat builds, but pipelines are still created through an in-memory
         * VkPipelineCache: with VK_NULL_HANDLE, RE9 at the same spot fell from ~70 to ~60 fps (GPU 61% -> 36%).
         * No path: nothing is loaded or saved, so the session cache never becomes a GB-sized file again. */
        const VkPipelineCacheCreateInfo cache_info = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
        if (device->vk.CreatePipelineCache(device->device, &cache_info, NULL, &device->pipeline_cache) != VK_SUCCESS)
            device->pipeline_cache = VK_NULL_HANDLE;
    }

    /* Probe the memory type bits every D3D12 buffer can use. */
    VkBufferCreateInfo probe_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 65536,
        .usage = buffer_usage(),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    VkBuffer probe;
    if (device->vk.CreateBuffer(device->device, &probe_info, NULL, &probe) !=
        VK_SUCCESS) {
        free(device);
        return NULL;
    }
    VkMemoryRequirements requirements;
    device->vk.GetBufferMemoryRequirements(device->device, probe,
                                           &requirements);
    device->vk.DestroyBuffer(device->device, probe, NULL);
    device->buffer_memory_bits = requirements.memoryTypeBits;

    device->memory_type[BD12_HEAP_DEFAULT] = pick_memory_type(
        &device->memory, device->buffer_memory_bits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    device->memory_type[BD12_HEAP_UPLOAD] = pick_memory_type(
        &device->memory, device->buffer_memory_bits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    /* UPLOAD heaps in device-local, host-visible memory when the whole VRAM is CPU-mappable (resizable BAR), as
     * vkd3d-proton does: games read constants, light lists and other data straight out of upload heaps in their
     * shaders, and from system memory every such read crosses PCIe (RE9: GPU "busy" at half power). CPU writes stay
     * write-combined, as Windows upload heaps are. A small (256 MB) BAR window is left alone; BEER_BD12_UPLOAD_SYSMEM=1
     * keeps system memory. */
    if (!getenv("BEER_BD12_UPLOAD_SYSMEM")) {
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32_t i = 0; i < device->memory.memoryTypeCount; ++i) {
            const VkMemoryType *t = &device->memory.memoryTypes[i];
            if (!(device->buffer_memory_bits & (1u << i)) || (t->propertyFlags & want) != want ||
                (t->propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ||
                device->memory.memoryHeaps[t->heapIndex].size < (4ull << 30))
                continue;
            device->memory_type[BD12_HEAP_UPLOAD] = (int32_t)i;
            fprintf(stderr, "[BD12] upload heaps in device-local host-visible memory (type %u, resizable BAR)\n", i);
            break;
        }
    }
    device->memory_type[BD12_HEAP_READBACK] = pick_memory_type(
        &device->memory, device->buffer_memory_bits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (device->memory_type[BD12_HEAP_DEFAULT] < 0 ||
        device->memory_type[BD12_HEAP_UPLOAD] < 0 ||
        device->memory_type[BD12_HEAP_READBACK] < 0) {
        fprintf(stderr, "[BD12] no suitable memory types\n");
        free(device);
        return NULL;
    }
    device->readback_coherent =
        (device->memory.memoryTypes[device->memory_type[BD12_HEAP_READBACK]]
             .propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    fprintf(stderr,
            "[BD12] device %s: memory types default=%d upload=%d readback=%d "
            "(readback coherent=%d)\n",
            device->device_name, device->memory_type[BD12_HEAP_DEFAULT],
            device->memory_type[BD12_HEAP_UPLOAD],
            device->memory_type[BD12_HEAP_READBACK], device->readback_coherent);
    bd12_rt_load(device);
    fprintf(stderr, "[BD12] ray tracing %s\n", device->rt_enabled ? "available" : "unavailable");
    bd12_canary_arm(device);
    bd12_db_init(device);
    return device;
}

static uint64_t pc_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

void bd12_pipeline_cache_save(BD12Device *device)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    if (!device || !device->pipeline_cache || !device->pipeline_cache_path[0]) return;
    pthread_mutex_lock(&lock);
    size_t size = 0;
    if (device->vk.GetPipelineCacheData(device->device, device->pipeline_cache, &size, NULL) == VK_SUCCESS && size) {
        void *blob = malloc(size);
        const uint64_t hash = blob && device->vk.GetPipelineCacheData(device->device, device->pipeline_cache, &size,
                                                                     blob) == VK_SUCCESS ? pc_hash(blob, size) : 0;
        /* Pipelines found in the cache count as builds too: when the driver's data did not change, keep the file. */
        if (hash && hash != device->pipeline_cache_hash) {
            char tmp[560];
            snprintf(tmp, sizeof(tmp), "%s.tmp", device->pipeline_cache_path);
            FILE *f = fopen(tmp, "wb");
            if (f) {
                const int ok = fwrite(blob, 1, size, f) == size;
                fclose(f);
                if (ok && rename(tmp, device->pipeline_cache_path) == 0) device->pipeline_cache_hash = hash;
                else unlink(tmp);
            }
        }
        free(blob);
    }
    device->pipeline_cache_new = 0;
    device->pipeline_cache_saved_ms = pc_now_ms();
    pthread_mutex_unlock(&lock);
}

/* Periodic saves run on their own thread. A save copies the whole driver cache out (RE9: 1.1 GB) and writes it to
 * disk; done inline it ran on whichever game thread built the triggering pipeline, and the whole game waited for it
 * (RE9: a 3.8-5 s freeze a minute into gameplay, every launch). The final save at exit stays synchronous. */
static pthread_mutex_t g_pc_saver_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pc_saver_cv = PTHREAD_COND_INITIALIZER;
static BD12Device *g_pc_saver_device;
static int g_pc_saver_running;

static void *pc_saver_main(void *arg)
{
    (void)arg;
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);   /* background work: below the game's threads */
    pthread_mutex_lock(&g_pc_saver_lock);
    for (;;) {
        while (!g_pc_saver_device) pthread_cond_wait(&g_pc_saver_cv, &g_pc_saver_lock);
        BD12Device *device = g_pc_saver_device;
        g_pc_saver_device = NULL;
        pthread_mutex_unlock(&g_pc_saver_lock);
        bd12_pipeline_cache_save(device);
        pthread_mutex_lock(&g_pc_saver_lock);
    }
    return NULL;
}

/* Seconds between background saves while new pipelines keep appearing (BEER_BD12_PIPELINE_CACHE_SAVE_S, default 300):
 * each save rewrites the whole file, so frequent saves only cost disk bandwidth and a cache-sized buffer. */
static uint64_t pc_save_interval_ms(void)
{
    static uint64_t ms;
    if (!ms) {
        const char *v = getenv("BEER_BD12_PIPELINE_CACHE_SAVE_S");
        const long s = v ? strtol(v, NULL, 10) : 300;
        ms = (uint64_t)(s > 0 ? s : 300) * 1000u;
    }
    return ms;
}

/* Called after every pipeline build: hands a save to the saver thread now and then, never blocks the caller. */
void bd12_pipeline_cache_note_new(BD12Device *device)
{
    if (!device || !device->pipeline_cache || !device->pipeline_cache_path[0]) return;   /* in-memory only */
    const uint32_t fresh = __atomic_add_fetch(&device->pipeline_cache_new, 1, __ATOMIC_RELAXED);
    if (fresh < 25) return;
    const uint64_t now = pc_now_ms();
    uint64_t last = __atomic_load_n(&device->pipeline_cache_saved_ms, __ATOMIC_RELAXED);
    if (now - last <= pc_save_interval_ms()) return;
    /* One caller claims this save; the saver thread sets the real time when it finishes. */
    if (!__atomic_compare_exchange_n(&device->pipeline_cache_saved_ms, &last, now, 0, __ATOMIC_RELAXED,
                                     __ATOMIC_RELAXED))
        return;
    pthread_mutex_lock(&g_pc_saver_lock);
    if (!g_pc_saver_running) {
        pthread_t thread;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        g_pc_saver_running = pthread_create(&thread, &attr, pc_saver_main, NULL) == 0;
        pthread_attr_destroy(&attr);
        if (g_pc_saver_running) pthread_setname_np(thread, "beer-pcache");
    }
    if (g_pc_saver_running) {
        g_pc_saver_device = device;
        pthread_cond_signal(&g_pc_saver_cv);
    }
    pthread_mutex_unlock(&g_pc_saver_lock);
}

void bd12_device_destroy(BD12Device *device)
{
    if (!device) return;
    if (device->pipeline_cache) {
        if (device->pipeline_cache_new) bd12_pipeline_cache_save(device);
        device->vk.DestroyPipelineCache(device->device, device->pipeline_cache, NULL);
    }
    for (size_t i = 0; i < device->sampler_count; ++i)
        device->vk.DestroySampler(device->device, device->samplers[i].sampler,
                                  NULL);
    free(device->samplers);
    free(device->va_table);
    pthread_mutex_destroy(&device->va_lock);
    pthread_mutex_destroy(&device->view_lock);
    free(device);
}

/* Heaps ----------------------------------------------------------------- */

/* Heap sub-allocation ------------------------------------------------------------------------------------------
 * Engines stream resources by creating and releasing thousands of committed resources and small heaps per second.
 * One Vulkan allocation each made every one a driver call pair (allocate + map, unmap + free), and the driver's
 * per-submit cost grows with the number of live allocations. Like vkd3d-proton and Windows drivers, heaps below
 * SUBALLOC_MAX are carved from large blocks, each allocated and mapped once with one buffer over it for zero
 * fills; bigger heaps keep dedicated memory. Freed ranges are reused once the heap is destroyed, which happens
 * only after the GPU finished with it. BEER_BD12_NO_SUBALLOC=1 gives every heap its own allocation. */
#define SUBALLOC_MAX ((VkDeviceSize)32 << 20)
#define BLOCK_DEFAULT ((VkDeviceSize)256 << 20)
#define BLOCK_HOST ((VkDeviceSize)128 << 20)
#define SUBALLOC_MIN_ALIGN ((VkDeviceSize)64 << 10)

typedef struct { VkDeviceSize offset, size; } MemRange;

typedef struct BD12MemBlock {
    VkDeviceMemory memory;
    void *mapped;
    VkBuffer fill_buffer;         /* whole block, TRANSFER_DST: zero fills of DEFAULT heaps */
    VkDeviceSize size, used;
    VkDeviceSize fresh;           /* [fresh, size) never handed out: still as the driver allocated it */
    MemRange *free;               /* sorted by offset, coalesced */
    uint32_t free_count, free_capacity;
} BD12MemBlock;

typedef struct BD12MemPool {
    pthread_mutex_t lock;
    BD12MemBlock **blocks;
    size_t count, capacity;
} BD12MemPool;

static int suballoc_enabled(void)
{
    static int value = -1;
    if (value < 0) value = getenv("BEER_BD12_NO_SUBALLOC") == NULL;
    return value;
}

static VkDeviceSize suballoc_alignment(VkDeviceSize size)
{
    /* The largest power of two not above the size (at most 4 MiB, the D3D12 MSAA placement alignment), so a
     * committed resource bound at the heap start meets its own alignment, which never exceeds its size. */
    VkDeviceSize a = SUBALLOC_MIN_ALIGN;
    while (a < ((VkDeviceSize)4 << 20) && a * 2 <= size) a *= 2;
    return a;
}

static int range_insert(BD12MemBlock *b, uint32_t at, MemRange r)
{
    if (b->free_count == b->free_capacity) {
        const uint32_t capacity = b->free_capacity ? b->free_capacity * 2 : 16;
        MemRange *grown = realloc(b->free, capacity * sizeof(*grown));
        if (!grown) return 0;
        b->free = grown;
        b->free_capacity = capacity;
    }
    memmove(b->free + at + 1, b->free + at, (b->free_count - at) * sizeof(*b->free));
    b->free[at] = r;
    ++b->free_count;
    return 1;
}

static void range_remove(BD12MemBlock *b, uint32_t at)
{
    memmove(b->free + at, b->free + at + 1, (b->free_count - at - 1) * sizeof(*b->free));
    --b->free_count;
}

/* First fit in `b`; returns the offset or UINT64_MAX. */
static VkDeviceSize block_take(BD12MemBlock *b, VkDeviceSize size, VkDeviceSize align)
{
    for (uint32_t i = 0; i < b->free_count; ++i) {
        const MemRange r = b->free[i];
        const VkDeviceSize start = (r.offset + align - 1) & ~(align - 1);
        if (start + size > r.offset + r.size) continue;
        const VkDeviceSize head = start - r.offset, tail = r.offset + r.size - (start + size);
        if (head && tail) {
            b->free[i].size = head;
            if (!range_insert(b, i + 1, (MemRange){ start + size, tail })) { b->free[i] = r; return UINT64_MAX; }
        } else if (head) {
            b->free[i].size = head;
        } else if (tail) {
            b->free[i] = (MemRange){ start + size, tail };
        } else {
            range_remove(b, i);
        }
        b->used += size;
        return start;
    }
    return UINT64_MAX;
}

static void block_give(BD12MemBlock *b, VkDeviceSize offset, VkDeviceSize size)
{
    uint32_t at = 0;
    while (at < b->free_count && b->free[at].offset < offset) ++at;
    b->used -= size;
    const int join_prev = at > 0 && b->free[at - 1].offset + b->free[at - 1].size == offset;
    const int join_next = at < b->free_count && offset + size == b->free[at].offset;
    if (join_prev && join_next) {
        b->free[at - 1].size += size + b->free[at].size;
        range_remove(b, at);
    } else if (join_prev) {
        b->free[at - 1].size += size;
    } else if (join_next) {
        b->free[at].offset = offset;
        b->free[at].size += size;
    } else if (!range_insert(b, at, (MemRange){ offset, size })) {
        static int warned;
        if (!warned++) fprintf(stderr, "[BD12] heap sub-allocator: free-list growth failed, range leaked\n");
    }
}

static BD12MemBlock *block_create(BD12Device *device, BD12HeapType type)
{
    const VkDeviceSize size = type == BD12_HEAP_DEFAULT ? BLOCK_DEFAULT : BLOCK_HOST;
    BD12MemBlock *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    VkMemoryAllocateFlagsInfo flags = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
    };
    VkMemoryAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &flags,
        .allocationSize = size,
        .memoryTypeIndex = (uint32_t)device->memory_type[type]
    };
    if (device->vk.AllocateMemory(device->device, &allocate, NULL, &b->memory) != VK_SUCCESS) goto fail;
    if (type != BD12_HEAP_DEFAULT &&
        device->vk.MapMemory(device->device, b->memory, 0, VK_WHOLE_SIZE, 0, &b->mapped) != VK_SUCCESS)
        goto fail_memory;
    if (type == BD12_HEAP_DEFAULT) {
        VkBufferCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE
        };
        VkMemoryRequirements req;
        if (device->vk.CreateBuffer(device->device, &info, NULL, &b->fill_buffer) != VK_SUCCESS) goto fail_memory;
        device->vk.GetBufferMemoryRequirements(device->device, b->fill_buffer, &req);
        if (req.size > size || !(req.memoryTypeBits & (1u << device->memory_type[type])) ||
            device->vk.BindBufferMemory(device->device, b->fill_buffer, b->memory, 0) != VK_SUCCESS) {
            device->vk.DestroyBuffer(device->device, b->fill_buffer, NULL);
            goto fail_memory;
        }
    }
    b->size = size;
    b->fresh = 0;
    if (!range_insert(b, 0, (MemRange){ 0, size })) goto fail_buffer;
    return b;
fail_buffer:
    if (b->fill_buffer) device->vk.DestroyBuffer(device->device, b->fill_buffer, NULL);
fail_memory:
    device->vk.FreeMemory(device->device, b->memory, NULL);
fail:
    free(b);
    return NULL;
}

static void block_destroy(BD12Device *device, BD12MemBlock *b)
{
    if (b->fill_buffer) device->vk.DestroyBuffer(device->device, b->fill_buffer, NULL);
    if (b->mapped) device->vk.UnmapMemory(device->device, b->memory);
    device->vk.FreeMemory(device->device, b->memory, NULL);
    free(b->free);
    free(b);
}

static BD12MemPool *mem_pools(BD12Device *device)
{
    BD12MemPool *pools = __atomic_load_n(&device->mem_pools, __ATOMIC_ACQUIRE);
    if (pools) return pools;
    pools = calloc(4, sizeof(*pools));
    if (!pools) return NULL;
    for (int i = 0; i < 4; ++i) pthread_mutex_init(&pools[i].lock, NULL);
    BD12MemPool *expected = NULL;
    if (!__atomic_compare_exchange_n(&device->mem_pools, &expected, pools, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        free(pools);
        return expected;
    }
    return pools;
}

/* Carves `size` bytes for `heap`; 0 when it must get dedicated memory. *reused: the range was handed out before. */
static int suballoc(BD12Heap *heap, int *reused)
{
    BD12Device *device = heap->device;
    BD12MemPool *pool = mem_pools(device);
    if (!pool) return 0;
    pool += heap->type;
    const VkDeviceSize align = suballoc_alignment(heap->size);
    pthread_mutex_lock(&pool->lock);
    BD12MemBlock *block = NULL;
    VkDeviceSize offset = UINT64_MAX;
    for (size_t i = 0; i < pool->count && offset == UINT64_MAX; ++i)
        if (pool->blocks[i]->size - pool->blocks[i]->used >= heap->size &&
            (offset = block_take(pool->blocks[i], heap->size, align)) != UINT64_MAX)
            block = pool->blocks[i];
    if (!block) {
        if (pool->count == pool->capacity) {
            const size_t capacity = pool->capacity ? pool->capacity * 2 : 8;
            BD12MemBlock **grown = realloc(pool->blocks, capacity * sizeof(*grown));
            if (!grown) { pthread_mutex_unlock(&pool->lock); return 0; }
            pool->blocks = grown;
            pool->capacity = capacity;
        }
        block = block_create(device, heap->type);
        if (!block || (offset = block_take(block, heap->size, align)) == UINT64_MAX) {
            if (block) block_destroy(device, block);
            pthread_mutex_unlock(&pool->lock);
            return 0;
        }
        pool->blocks[pool->count++] = block;
    }
    *reused = offset < block->fresh;
    if (offset + heap->size > block->fresh) block->fresh = offset + heap->size;
    pthread_mutex_unlock(&pool->lock);
    heap->block = block;
    heap->base = offset;
    heap->memory = block->memory;
    heap->mapped = block->mapped ? (char *)block->mapped + offset : NULL;
    return 1;
}

static void suballoc_release(BD12Heap *heap)
{
    BD12Device *device = heap->device;
    BD12MemPool *pool = device->mem_pools + heap->type;
    BD12MemBlock *empty = NULL;
    pthread_mutex_lock(&pool->lock);
    block_give(heap->block, heap->base, heap->size);
    if (!heap->block->used) {
        /* Keep one empty block per type for the next burst; release any other. */
        size_t empties = 0;
        for (size_t i = 0; i < pool->count; ++i) empties += !pool->blocks[i]->used;
        if (empties > 1) {
            for (size_t i = 0; i < pool->count; ++i)
                if (pool->blocks[i] == heap->block) { pool->blocks[i] = pool->blocks[--pool->count]; break; }
            empty = heap->block;
        }
    }
    pthread_mutex_unlock(&pool->lock);
    if (empty) {
        /* Zero fills recorded into its buffer must have run before the buffer goes. */
        bd12_zero_flush(device);
        uint64_t value = __atomic_load_n(&device->zero_submitted, __ATOMIC_ACQUIRE);
        if (value && device->zero_sem) {
            VkSemaphoreWaitInfo wait = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
                                         .pSemaphores = &device->zero_sem, .pValues = &value };
            device->vk.WaitSemaphores(device->device, &wait, UINT64_C(10000000000));
        }
        block_destroy(device, empty);
    }
}

/* D3D12 heaps are zero-initialised; Vulkan memory is not. Shaders that read
 * counters or loop bounds from never-written buffers would otherwise run on
 * recycled garbage (GPU hang). The fill is queued ahead of every later submit
 * and its resources are released with the heap (already GPU-deferred). */
static void record_zero_fill(BD12Heap *heap, VkBuffer buffer, VkDeviceSize offset);

static void heap_zero_fill(BD12Heap *heap)
{
    BD12Device *device = heap->device;
    if (heap->block) {   /* sub-allocated: fill the range through the block's buffer */
        record_zero_fill(heap, heap->block->fill_buffer, heap->base);
        return;
    }
    VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = heap->size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    if (device->vk.CreateBuffer(device->device, &info, NULL,
                                &heap->zero_buffer) != VK_SUCCESS) {
        heap->zero_buffer = VK_NULL_HANDLE;
        return;
    }
    VkMemoryRequirements requirements;
    device->vk.GetBufferMemoryRequirements(device->device, heap->zero_buffer,
                                           &requirements);
    if (requirements.size > heap->size ||
        !(requirements.memoryTypeBits & (1u << device->memory_type[heap->type])) ||
        device->vk.BindBufferMemory(device->device, heap->zero_buffer,
                                    heap->memory, 0) != VK_SUCCESS)
        return;
    record_zero_fill(heap, heap->zero_buffer, 0);
}

/* Records a zero fill of `heap` (at `offset` in `buffer`) into the shared batch; bd12_zero_flush submits it before
 * the next queue submit. */
static void record_zero_fill(BD12Heap *heap, VkBuffer buffer, VkDeviceSize offset)
{
    BD12Device *device = heap->device;
    pthread_mutex_lock(&device->zero_lock);
    if (!device->zero_pool) {
        VkCommandPoolCreateInfo pool = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = device->queue_family
        };
        VkSemaphoreTypeCreateInfo type = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE
        };
        VkSemaphoreCreateInfo sem = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type };
        if (device->vk.CreateCommandPool(device->device, &pool, NULL, &device->zero_pool) != VK_SUCCESS ||
            device->vk.CreateSemaphore(device->device, &sem, NULL, &device->zero_sem) != VK_SUCCESS) {
            device->zero_pool = VK_NULL_HANDLE;
            pthread_mutex_unlock(&device->zero_lock);
            return;
        }
    }
    if (!device->zero_cb) {
        if (device->zero_inflight_count == 16) {          /* wait for the oldest batch to retire */
            VkSemaphoreWaitInfo wait = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
                                         .pSemaphores = &device->zero_sem, .pValues = &device->zero_inflight_value[0] };
            device->vk.WaitSemaphores(device->device, &wait, UINT64_C(10000000000));
        }
        uint64_t done = 0;
        device->vk.GetSemaphoreCounterValue(device->device, device->zero_sem, &done);
        uint32_t keep = 0;
        for (uint32_t i = 0; i < device->zero_inflight_count; ++i) {
            if (device->zero_inflight_value[i] <= done) {
                device->vk.FreeCommandBuffers(device->device, device->zero_pool, 1, &device->zero_inflight[i]);
            } else {
                device->zero_inflight[keep] = device->zero_inflight[i];
                device->zero_inflight_value[keep++] = device->zero_inflight_value[i];
            }
        }
        device->zero_inflight_count = keep;
        VkCommandBufferAllocateInfo allocate = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = device->zero_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1
        };
        VkCommandBufferBeginInfo begin = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
        };
        if (device->vk.AllocateCommandBuffers(device->device, &allocate, &device->zero_cb) != VK_SUCCESS) {
            device->zero_cb = VK_NULL_HANDLE;
            pthread_mutex_unlock(&device->zero_lock);
            return;
        }
        device->vk.BeginCommandBuffer(device->zero_cb, &begin);
        device->zero_recorded = device->zero_submitted + 1;
    }
    device->vk.CmdFillBuffer(device->zero_cb, buffer, offset, heap->size & ~(VkDeviceSize)3, 0);
    heap->zero_value = device->zero_recorded;
    pthread_mutex_unlock(&device->zero_lock);
}

/* Submits the pending zero-fill batch. Must run before any other submission that could touch the filled memory. */
void bd12_zero_flush(BD12Device *device)
{
    if (!__atomic_load_n(&device->zero_cb, __ATOMIC_ACQUIRE)) return;
    pthread_mutex_lock(&device->zero_lock);
    VkCommandBuffer cb = device->zero_cb;
    if (cb) {
        VkMemoryBarrier2 barrier = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT
        };
        VkDependencyInfo dependency = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &barrier };
        device->vk.CmdPipelineBarrier2(cb, &dependency);
        device->vk.EndCommandBuffer(cb);
        VkCommandBufferSubmitInfo cb_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb };
        VkSemaphoreSubmitInfo signal = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = device->zero_sem,
                                         .value = device->zero_recorded, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT };
        VkSubmitInfo2 submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &cb_info,
                                 .signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &signal };
        beer_vulkan_queue_lock();
        VkResult result = device->vk.QueueSubmit2(device->queue, 1, &submit, VK_NULL_HANDLE);
        beer_vulkan_queue_unlock();
        beer_vulkan_record_result(result, "BD12 heap zero fill");
        device->zero_submitted = device->zero_recorded;
        device->zero_inflight[device->zero_inflight_count] = cb;
        device->zero_inflight_value[device->zero_inflight_count++] = device->zero_recorded;
        __atomic_store_n(&device->zero_cb, VK_NULL_HANDLE, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&device->zero_lock);
}

uint64_t g_bd12_heap_live[4], g_bd12_heap_bytes[4];


static BD12Heap *heap_create_impl(BD12Device *device, BD12HeapType type,
                                  VkDeviceSize size, int zero);

BD12Heap *bd12_heap_create(BD12Device *device, BD12HeapType type,
                           VkDeviceSize size)
{
    BD12_PROF_BEGIN();
    BD12Heap *heap = heap_create_impl(device, type, size, 1);
    BD12_PROF_END(BD12_PROF_HEAP_CREATE);
    return heap;
}

/* Image memory is never read before the application writes it, and a buffer
 * fill of optimally tiled image memory has no defined texel meaning, so a
 * committed image skips the per-allocation zero-fill submission. */
BD12Heap *bd12_heap_create_unzeroed(BD12Device *device, BD12HeapType type,
                                    VkDeviceSize size)
{
    BD12_PROF_BEGIN();
    BD12Heap *heap = heap_create_impl(device, type, size, 0);
    BD12_PROF_END(BD12_PROF_HEAP_CREATE);
    return heap;
}

static BD12Heap *heap_create_impl(BD12Device *device, BD12HeapType type,
                                  VkDeviceSize size, int zero)
{
    if (!device || !size || type < BD12_HEAP_DEFAULT || type > BD12_HEAP_READBACK)
        return NULL;
    BD12Heap *heap = calloc(1, sizeof(*heap));
    if (!heap) return NULL;
    heap->device = device;
    heap->size = size;
    heap->type = type;
    int reused = 0;
    if (size <= SUBALLOC_MAX && suballoc_enabled() && suballoc(heap, &reused)) {
        /* D3D12 heaps start zeroed. A range handed out before is cleared again: on the GPU for DEFAULT heaps (as
         * every DEFAULT heap is), on the CPU for mapped ones (fresh driver memory is already zero). */
        if (type == BD12_HEAP_DEFAULT) { if (zero) heap_zero_fill(heap); }
        else if (reused) memset(heap->mapped, 0, size);
        __atomic_add_fetch(&g_bd12_heap_live[type], 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_bd12_heap_bytes[type], size, __ATOMIC_RELAXED);
        return heap;
    }
    VkMemoryAllocateFlagsInfo flags = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
    };
    VkMemoryAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &flags,
        .allocationSize = size,
        .memoryTypeIndex = (uint32_t)device->memory_type[type]
    };
    VkResult result = device->vk.AllocateMemory(device->device, &allocate, NULL,
                                                &heap->memory);
    beer_vulkan_record_result(result, "BD12 heap allocation");
    if (result != VK_SUCCESS) {
        free(heap);
        return NULL;
    }
    if (type != BD12_HEAP_DEFAULT &&
        device->vk.MapMemory(device->device, heap->memory, 0, VK_WHOLE_SIZE, 0,
                             &heap->mapped) != VK_SUCCESS) {
        device->vk.FreeMemory(device->device, heap->memory, NULL);
        free(heap);
        return NULL;
    }
    if (type == BD12_HEAP_DEFAULT && zero) heap_zero_fill(heap);
    __atomic_add_fetch(&g_bd12_heap_live[type], 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_bd12_heap_bytes[type], size, __ATOMIC_RELAXED);
    return heap;
}

static void heap_destroy_impl(BD12Heap *heap);
void bd12_heap_destroy(BD12Heap *heap)
{
    BD12_PROF_BEGIN();
    heap_destroy_impl(heap);
    BD12_PROF_END(BD12_PROF_HEAP_DESTROY);
}

static void heap_destroy_impl(BD12Heap *heap)
{
    if (!heap) return;
    if (heap->block) {   /* the range returns to its block; a later fill of it is ordered after this one's */
        suballoc_release(heap);
        __atomic_sub_fetch(&g_bd12_heap_live[heap->type], 1, __ATOMIC_RELAXED);
        __atomic_sub_fetch(&g_bd12_heap_bytes[heap->type], heap->size, __ATOMIC_RELAXED);
        free(heap);
        return;
    }
    /* The zero fill runs on its own timeline; it must finish before the buffer and the heap memory are released. */
    if (heap->zero_value) {
        BD12Device *device = heap->device;
        bd12_zero_flush(device);
        VkSemaphoreWaitInfo wait = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
                                     .pSemaphores = &device->zero_sem, .pValues = &heap->zero_value };
        device->vk.WaitSemaphores(device->device, &wait, UINT64_C(10000000000));
    }
    if (heap->mapped)
        heap->device->vk.UnmapMemory(heap->device->device, heap->memory);
    if (heap->zero_buffer)
        heap->device->vk.DestroyBuffer(heap->device->device,
                                       heap->zero_buffer, NULL);
    heap->device->vk.FreeMemory(heap->device->device, heap->memory, NULL);
    __atomic_sub_fetch(&g_bd12_heap_live[heap->type], 1, __ATOMIC_RELAXED);
    __atomic_sub_fetch(&g_bd12_heap_bytes[heap->type], heap->size, __ATOMIC_RELAXED);
    free(heap);
}

/* GPU virtual address table ---------------------------------------------- */

static size_t va_lower_bound(const BD12Device *device, uint64_t va)
{
    size_t lo = 0, hi = device->va_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (device->va_table[mid].va < va) lo = mid + 1; else hi = mid;
    }
    return lo;
}

static int va_insert(BD12Device *device, BD12Buffer *buffer)
{
    pthread_mutex_lock(&device->va_lock);
    if (device->va_count == device->va_capacity) {
        size_t capacity = device->va_capacity ? device->va_capacity * 2 : 256;
        BD12VaEntry *grown = realloc(device->va_table,
                                     capacity * sizeof(*grown));
        if (!grown) {
            pthread_mutex_unlock(&device->va_lock);
            return 0;
        }
        device->va_table = grown;
        device->va_capacity = capacity;
    }
    size_t index = va_lower_bound(device, buffer->va);
    memmove(&device->va_table[index + 1], &device->va_table[index],
            (device->va_count - index) * sizeof(*device->va_table));
    device->va_table[index] = (BD12VaEntry){ buffer->va, buffer->size, buffer };
    ++device->va_count;
    pthread_mutex_unlock(&device->va_lock);
    return 1;
}

static void va_remove(BD12Device *device, const BD12Buffer *buffer)
{
    pthread_mutex_lock(&device->va_lock);
    /* Placed resources that alias the same heap offset share one device address, so several entries can have this
     * VA. Checking only the first left a destroyed buffer in the table when another alias sorted ahead of it, and a
     * later root-descriptor lookup returned freed memory (RE9). */
    for (size_t index = va_lower_bound(device, buffer->va);
         index < device->va_count && device->va_table[index].va == buffer->va; ++index) {
        if (device->va_table[index].buffer != buffer) continue;
        memmove(&device->va_table[index], &device->va_table[index + 1],
                (device->va_count - index - 1) * sizeof(*device->va_table));
        --device->va_count;
        break;
    }
    pthread_mutex_unlock(&device->va_lock);
}

BD12Buffer *bd12_va_resolve(BD12Device *device, uint64_t va,
                            VkDeviceSize *offset)
{
    BD12Buffer *found = NULL;
    pthread_mutex_lock(&device->va_lock);
    size_t index = va_lower_bound(device, va);
    if (index < device->va_count && device->va_table[index].va == va)
        ++index;
    if (index > 0) {
        const BD12VaEntry *entry = &device->va_table[index - 1];
        if (va >= entry->va && va - entry->va < entry->size) {
            found = entry->buffer;
            if (offset) *offset = va - entry->va;
        }
    }
    pthread_mutex_unlock(&device->va_lock);
    return found;
}

/* Buffers ----------------------------------------------------------------- */

BD12Buffer *bd12_buffer_create_placed(BD12Heap *heap, VkDeviceSize offset,
                                      VkDeviceSize size)
{
    if (!heap || !size) return NULL;
    BD12Device *device = heap->device;
    BD12Buffer *buffer = calloc(1, sizeof(*buffer));
    if (!buffer) return NULL;
    buffer->device = device;
    buffer->heap = heap;
    buffer->heap_offset = offset;
    buffer->size = size;
    VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = buffer_usage(),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    if (device->vk.CreateBuffer(device->device, &info, NULL, &buffer->buffer) !=
        VK_SUCCESS)
        goto fail;
    VkMemoryRequirements requirements;
    device->vk.GetBufferMemoryRequirements(device->device, buffer->buffer,
                                           &requirements);
    if ((offset % requirements.alignment) ||
        offset + requirements.size > heap->size ||
        !(requirements.memoryTypeBits &
          (1u << device->memory_type[heap->type])))
        goto fail_buffer;
    if (((heap->base + offset) % requirements.alignment) ||
        device->vk.BindBufferMemory(device->device, buffer->buffer,
                                    heap->memory, heap->base + offset) != VK_SUCCESS)
        goto fail_buffer;
    VkBufferDeviceAddressInfo address = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = buffer->buffer
    };
    buffer->va = device->vk.GetBufferDeviceAddress(device->device, &address);
    if (!buffer->va) goto fail_buffer;
    buffer->cpu = heap->mapped ? (char *)heap->mapped + offset : NULL;
    if (!va_insert(device, buffer)) goto fail_buffer;
    buffer->serial = bd12_live_serial();
    bd12_live_add(buffer);
    return buffer;

fail_buffer:
    device->vk.DestroyBuffer(device->device, buffer->buffer, NULL);
fail:
    free(buffer);
    return NULL;
}

BD12Buffer *bd12_buffer_create_committed(BD12Device *device, BD12HeapType type,
                                         VkDeviceSize size)
{
    /* Round up to 64 KiB, the D3D12 placement granularity. */
    VkDeviceSize heap_size = (size + 65535) & ~(VkDeviceSize)65535;
    BD12Heap *heap = bd12_heap_create(device, type, heap_size);
    if (!heap) return NULL;
    BD12Buffer *buffer = bd12_buffer_create_placed(heap, 0, size);
    if (!buffer) {
        bd12_heap_destroy(heap);
        return NULL;
    }
    buffer->owns_heap = 1;
    return buffer;
}

void bd12_buffer_destroy(BD12Buffer *buffer)
{
    if (!buffer) return;
    BD12Device *device = buffer->device;
    bd12_live_remove(buffer);
    va_remove(device, buffer);
    bd12_rt_buffer_release(buffer);
    bd12_buffer_views_release(buffer);
    device->vk.DestroyBuffer(device->device, buffer->buffer, NULL);
    if (buffer->owns_heap) bd12_heap_destroy(buffer->heap);
    free(buffer);
}

/* Fences ---------------------------------------------------------------- */

extern uint64_t g_bd12_fences_released, g_bd12_fence_release_calls, g_bd12_fence_completed_calls;
uint64_t g_bd12_fences_live;   /* profiler: D3D12 fences alive (each owns a timeline semaphore) */

BD12Fence *bd12_fence_create(BD12Device *device, uint64_t initial)
{
    BD12Fence *fence = calloc(1, sizeof(*fence));
    if (!fence) return NULL;
    fence->device = device;
    fence->signaled = initial;
    VkSemaphoreTypeCreateInfo type = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = initial
    };
    VkSemaphoreCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &type
    };
    if (device->vk.CreateSemaphore(device->device, &info, NULL,
                                   &fence->semaphore) != VK_SUCCESS) {
        free(fence);
        return NULL;
    }
    __atomic_add_fetch(&g_bd12_fences_live, 1, __ATOMIC_RELAXED);
    return fence;
}

void bd12_fence_destroy(BD12Fence *fence)
{
    if (!fence) return;
    __atomic_sub_fetch(&g_bd12_fences_live, 1, __ATOMIC_RELAXED);
    fence->device->vk.DestroySemaphore(fence->device->device, fence->semaphore,
                                       NULL);
    free(fence);
}

uint64_t bd12_fence_completed(BD12Fence *fence)
{
    uint64_t value = 0;
    VkResult result = fence->device->vk.GetSemaphoreCounterValue(
        fence->device->device, fence->semaphore, &value);
    beer_vulkan_record_result(result, "BD12 fence query");
    return value;
}

/* D3D12 allows Signal with a value the fence already has (RE8 signals 0 on a
 * fresh fence); a Vulkan timeline signal must exceed every earlier one, and
 * Mesa reports a violation as device loss. Returns 0 when the signal cannot
 * advance the timeline and must be dropped. */
int bd12_fence_claim(BD12Fence *fence, uint64_t value)
{
    uint64_t current = __atomic_load_n(&fence->signaled, __ATOMIC_ACQUIRE);
    while (value > current)
        if (__atomic_compare_exchange_n(&fence->signaled, &current, value, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return 1;
    return 0;
}

int bd12_fence_signal_cpu(BD12Fence *fence, uint64_t value)
{
    if (!bd12_fence_claim(fence, value)) return 1;
    VkSemaphoreSignalInfo info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
        .semaphore = fence->semaphore,
        .value = value
    };
    return fence->device->vk.SignalSemaphore(fence->device->device, &info) ==
           VK_SUCCESS;
}

int bd12_fence_wait(BD12Fence *fence, uint64_t value, uint64_t timeout_ns)
{
    VkSemaphoreWaitInfo info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores = &fence->semaphore,
        .pValues = &value
    };
    VkResult result = fence->device->vk.WaitSemaphores(fence->device->device,
                                                       &info, timeout_ns);
    beer_vulkan_record_result(result, "BD12 fence wait");
    return result == VK_SUCCESS;
}

/* Queue ----------------------------------------------------------------- */

BD12Queue *bd12_queue_create_at(BD12Device *device, uint32_t index)
{
    BD12Queue *queue = calloc(1, sizeof(*queue));
    if (!queue) return NULL;
    if (index >= beer_vulkan_queue_count()) index = 0;
    queue->device = device;
    queue->index = index;
    queue->queue = index ? beer_vulkan_queue_at(index) : device->queue;
    return queue;
}

BD12Queue *bd12_queue_create(BD12Device *device) { return bd12_queue_create_at(device, 0); }

/* The prebuilt barrier command buffer for BD12_LIST_BARRIER_SUBMIT (simultaneous use: every queue of the family
 * submits the same one), or VK_NULL_HANDLE in the other modes. */
static VkCommandBuffer submit_barrier(BD12Device *device)
{
    if (bd12_list_barrier_mode() != BD12_LIST_BARRIER_SUBMIT) return VK_NULL_HANDLE;
    VkCommandBuffer cb = __atomic_load_n(&device->submit_barrier_cb, __ATOMIC_ACQUIRE);
    if (cb) return cb;
    pthread_mutex_lock(&device->submit_barrier_lock);
    if (!device->submit_barrier_cb) {
        const VkCommandPoolCreateInfo pool = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                               .queueFamilyIndex = device->queue_family };
        if (!device->submit_barrier_pool &&
            device->vk.CreateCommandPool(device->device, &pool, NULL, &device->submit_barrier_pool) != VK_SUCCESS)
            device->submit_barrier_pool = VK_NULL_HANDLE;
        const VkCommandBufferAllocateInfo allocate = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                       .commandPool = device->submit_barrier_pool,
                                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                       .commandBufferCount = 1 };
        VkCommandBuffer made = VK_NULL_HANDLE;
        if (device->submit_barrier_pool &&
            device->vk.AllocateCommandBuffers(device->device, &allocate, &made) == VK_SUCCESS) {
            const VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                                     .flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT };
            const VkMemoryBarrier2 barrier = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT,
                .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT |
                                 VK_ACCESS_2_HOST_READ_BIT
            };
            const VkDependencyInfo dependency = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                                  .memoryBarrierCount = 1, .pMemoryBarriers = &barrier };
            device->vk.BeginCommandBuffer(made, &begin);
            device->vk.CmdPipelineBarrier2(made, &dependency);
            if (device->vk.EndCommandBuffer(made) == VK_SUCCESS)
                __atomic_store_n(&device->submit_barrier_cb, made, __ATOMIC_RELEASE);
        }
    }
    cb = device->submit_barrier_cb;
    pthread_mutex_unlock(&device->submit_barrier_lock);
    return cb;
}

void bd12_queue_destroy(BD12Queue *queue)
{
    if (!queue) return;
    free(queue);
}

VkResult bd12_queue_submit(BD12Queue *queue, BD12CmdList *const *lists,
                           uint32_t list_count, const BD12FenceOp *waits,
                           uint32_t wait_count, const BD12FenceOp *signals,
                           uint32_t signal_count)
{
    bd12_zero_flush(queue->device);
    bd12_canary_check("queue submit");
    if (beer_vulkan_device_lost()) return VK_ERROR_DEVICE_LOST;
    BD12Device *device = queue->device;
    /* A D3D12 list may have been recorded into several primary command buffers
     * (segmented dispatches, isolation boundaries). Every recorded segment must
     * be submitted, in order. Submitting only the list's final segment silently
     * discarded its copies, barriers and dispatches, which left later shaders
     * reading uninitialised memory and could hang the GPU. */
    uint32_t command_count = 0;
    for (uint32_t i = 0; i < list_count; ++i)
        command_count += lists[i]->command_buffer_count ?
                         lists[i]->command_buffer_count : 1u;
    /* Heap zero fills go to the main queue (bd12_zero_flush above). Another queue is not ordered after them, so
     * it waits for the newest submitted batch before touching memory. */
    uint64_t zero_wait = 0;
    if (queue->index) {
        pthread_mutex_lock(&device->zero_lock);
        zero_wait = device->zero_submitted;
        pthread_mutex_unlock(&device->zero_lock);
        if (zero_wait) {
            uint64_t done = 0;
            device->vk.GetSemaphoreCounterValue(device->device, device->zero_sem, &done);
            if (done >= zero_wait) zero_wait = 0;
        }
    }
    const uint32_t extra_waits = zero_wait ? 1u : 0u;
    const VkCommandBuffer order_barrier = submit_barrier(device);
    if (order_barrier) ++command_count;
    const uint32_t total = command_count + wait_count + extra_waits + signal_count;
    /* One allocation holds all three info arrays. */
    size_t bytes = command_count * sizeof(VkCommandBufferSubmitInfo) +
                   (wait_count + extra_waits + signal_count) * sizeof(VkSemaphoreSubmitInfo);
    VkCommandBufferSubmitInfo stack_storage[16];
    void *storage = total <= 5 ? (void *)stack_storage :
                    malloc(bytes ? bytes : 1);
    if (!storage) return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkCommandBufferSubmitInfo *command_infos = storage;
    VkSemaphoreSubmitInfo *wait_infos = (VkSemaphoreSubmitInfo *)
        (command_infos + command_count);
    VkSemaphoreSubmitInfo *signal_infos = wait_infos + wait_count + extra_waits;
    uint32_t at = 0;
    for (uint32_t i = 0; i < list_count; ++i) {
        const BD12CmdList *list = lists[i];
        if (!list->command_buffer_count) {
            command_infos[at++] = (VkCommandBufferSubmitInfo){
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                .commandBuffer = list->command_buffer
            };
            continue;
        }
        for (uint32_t s = 0; s < list->command_buffer_count; ++s)
            command_infos[at++] = (VkCommandBufferSubmitInfo){
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                .commandBuffer = list->command_buffers[s]
            };
    }

    if (order_barrier)   /* after the lists: their writes are visible to every later submission and the host */
        command_infos[at++] = (VkCommandBufferSubmitInfo){ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                                                           .commandBuffer = order_barrier };
    for (uint32_t i = 0; i < wait_count; ++i)
        wait_infos[i] = (VkSemaphoreSubmitInfo){
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = waits[i].fence->semaphore,
            .value = waits[i].value,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
        };
    if (zero_wait)
        wait_infos[wait_count] = (VkSemaphoreSubmitInfo){
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = device->zero_sem,
            .value = zero_wait,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
        };
    for (uint32_t i = 0; i < signal_count; ++i)
        signal_infos[i] = (VkSemaphoreSubmitInfo){
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = signals[i].fence->semaphore,
            .value = signals[i].value,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
        };
    VkSubmitInfo2 submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = wait_count + extra_waits,
        .pWaitSemaphoreInfos = wait_infos,
        .commandBufferInfoCount = command_count,
        .pCommandBufferInfos = command_infos,

        .signalSemaphoreInfoCount = signal_count,
        .pSignalSemaphoreInfos = signal_infos
    };
    beer_vulkan_queue_lock_at(queue->index);
    VkResult result = device->vk.QueueSubmit2(queue->queue, 1, &submit,
                                              VK_NULL_HANDLE);
    beer_vulkan_queue_unlock_at(queue->index);
    __atomic_add_fetch(&queue->submit_count, 1, __ATOMIC_RELAXED);
    beer_vulkan_record_result(result, "BD12 queue submit");
    if (result == VK_ERROR_DEVICE_LOST) {
        bd12_ckpt_dump();
        fprintf(stderr, "[BD12] lost submit: %u command buffer(s)\n", command_count);
        for (uint32_t i = 0; i < wait_count; ++i)
            fprintf(stderr, "[BD12]   wait sem=%p value=%llu\n", (void *)wait_infos[i].semaphore,
                    (unsigned long long)wait_infos[i].value);
        for (uint32_t i = 0; i < signal_count; ++i)
            fprintf(stderr, "[BD12]   signal sem=%p value=%llu\n", (void *)signal_infos[i].semaphore,
                    (unsigned long long)signal_infos[i].value);
    }
    if (storage != (void *)stack_storage) free(storage);
    return result;
}


#define _GNU_SOURCE_PROF
#include <dlfcn.h>
#include <malloc.h>
#include <execinfo.h>
#include <signal.h>
#include <sys/time.h>
#include <ucontext.h>
#include <sys/syscall.h>
#include <unistd.h>

/* BEER_BD12_SAMPLE=1: SIGPROF instruction-pointer histogram, printed with the
 * profile window. Diagnostic only. */
#define SAMPLE_SLOTS 8192
static struct { uint64_t rip; uint64_t tid; uint64_t count; } g_samples[SAMPLE_SLOTS];

#define STACK_SLOTS 512
static struct { void *frames[14]; uint64_t count; } g_stacks[STACK_SLOTS];
uint64_t g_bd12_render_tid;
uint64_t g_bd12_render_tid;

static void sample_handler(int sig, siginfo_t *info, void *ctx)
{
    (void)sig; (void)info;
    if ((uint64_t)syscall(SYS_gettid) == g_bd12_render_tid) {
        void *fr[14]; int n = backtrace(fr, 14);
        uint64_t key = 1469598103934665603ull;
        for (int i = 0; i < n; ++i) { key ^= (uint64_t)fr[i]; key *= 1099511628211ull; }
        uint32_t h = (uint32_t)(key >> 20) % STACK_SLOTS;
        for (int probe = 0; probe < 32; ++probe, h = (h + 1) % STACK_SLOTS) {
            if (!g_stacks[h].count) { memcpy(g_stacks[h].frames, fr, sizeof(void *) * n); if (n < 14) g_stacks[h].frames[n] = NULL; g_stacks[h].count = 1; break; }
            int same = 1;
            for (int i = 0; i < 14; ++i) { void *f = i < n ? fr[i] : NULL; if (g_stacks[h].frames[i] != f) { same = 0; break; } }
            if (same) { ++g_stacks[h].count; break; }
        }
    }
    const uint64_t rip = ((ucontext_t *)ctx)->uc_mcontext.gregs[REG_RIP];
    const uint64_t tid = (uint64_t)syscall(SYS_gettid);
    uint32_t h = (uint32_t)((rip * 0x9E3779B97F4A7C15ull) >> 40) % SAMPLE_SLOTS;
    for (int probe = 0; probe < 64; ++probe, h = (h + 1) % SAMPLE_SLOTS) {
        uint64_t expected = 0;
        if (__atomic_load_n(&g_samples[h].rip, __ATOMIC_RELAXED) == rip &&
            g_samples[h].tid == tid) {
            __atomic_add_fetch(&g_samples[h].count, 1, __ATOMIC_RELAXED);
            return;
        }
        if (__atomic_compare_exchange_n(&g_samples[h].rip, &expected, rip, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            g_samples[h].tid = tid;
            __atomic_add_fetch(&g_samples[h].count, 1, __ATOMIC_RELAXED);
            return;
        }
    }
}

static void sample_start(void)
{
    struct sigaction sa = { .sa_sigaction = sample_handler,
                            .sa_flags = SA_SIGINFO | SA_RESTART };
    sigaction(SIGPROF, &sa, NULL);
    struct itimerval it = { { 0, 2000 }, { 0, 2000 } };
    setitimer(ITIMER_PROF, &it, NULL);
}

static void sample_report(void)
{
    for (int n = 0; n < 6; ++n) {
        int best = -1;
        for (int i = 0; i < STACK_SLOTS; ++i)
            if (g_stacks[i].count && (best < 0 || g_stacks[i].count > g_stacks[best].count)) best = i;
        if (best < 0) break;
        fprintf(stderr, "[BD12 STACK] count=%llu\n", (unsigned long long)g_stacks[best].count);
        for (int i = 0; i < 14 && g_stacks[best].frames[i]; ++i) {
            Dl_info di;
            const int ok = dladdr(g_stacks[best].frames[i], &di);
            fprintf(stderr, "[BD12 STACK]   #%d %p off=%llx %s %s\n", i, g_stacks[best].frames[i],
                    ok ? (unsigned long long)((uintptr_t)g_stacks[best].frames[i] - (uintptr_t)di.dli_fbase) : 0ull,
                    ok && di.dli_fname ? di.dli_fname : "?", ok && di.dli_sname ? di.dli_sname : "?");
        }
        g_stacks[best].count = 0;
    }
    memset(g_stacks, 0, sizeof(g_stacks));
    uint64_t total = 0;
    for (int i = 0; i < SAMPLE_SLOTS; ++i) total += g_samples[i].count;
    fprintf(stderr, "[BD12 SAMPLE] total=%llu\n", (unsigned long long)total);
    for (int n = 0; n < 40; ++n) {
        int best = -1;
        for (int i = 0; i < SAMPLE_SLOTS; ++i)
            if (g_samples[i].count && (best < 0 || g_samples[i].count > g_samples[best].count))
                best = i;
        if (best < 0) break;
        Dl_info di;
        const int ok = dladdr((void *)g_samples[best].rip, &di);
        fprintf(stderr, "[BD12 SAMPLE] %5llu tid=%llu rip=%llx off=%llx %s %s\n",
                (unsigned long long)g_samples[best].count,
                (unsigned long long)g_samples[best].tid,
                (unsigned long long)g_samples[best].rip,
                ok ? (unsigned long long)(g_samples[best].rip - (uint64_t)di.dli_fbase) : 0ull,
                ok && di.dli_fname ? di.dli_fname : "?", ok && di.dli_sname ? di.dli_sname : "?");
        g_samples[best].count = 0;
    }
    memset(g_samples, 0, sizeof(g_samples));
}

uint64_t g_bd12_drops[BD12_DROP_COUNT];
static void bd12_report_drops(void)
{
    static uint64_t last_report, last_total;
    const uint64_t now = bd12_prof_now();
    if (!last_report) last_report = now;
    if (now - last_report < 5000000000ull) return;
    last_report = now;
    static const char *names[BD12_DROP_COUNT] = {
        "draw(no pipeline/root signature)", "draw(descriptor resolve failed)",
        "dispatch(no pipeline/root signature)", "dispatch(descriptor resolve failed)",
        "ExecuteIndirect(multi-argument signature)",
        "ExecuteIndirect dispatch(count buffer ignored)", "PSO(compile failed)",
        "draw(patch list without tessellation)" };
    uint64_t total = 0;
    for (int i = 0; i < BD12_DROP_COUNT; ++i) total += g_bd12_drops[i];
    if (total == last_total) return;
    last_total = total;
    fprintf(stderr, "[BD12 DROP] cumulative:");
    for (int i = 0; i < BD12_DROP_COUNT; ++i)
        if (g_bd12_drops[i])
            fprintf(stderr, " %s=%llu", names[i], (unsigned long long)g_bd12_drops[i]);
    fputc('\n', stderr);
}

uint64_t g_bd12_prof_ns[BD12_PROF_COUNT];
uint64_t g_bd12_prof_calls[BD12_PROF_COUNT];

uint64_t g_bd12_cmd_counts[4];

static uint64_t self_read_bytes(void)
{
    FILE *f = fopen("/proc/self/io", "r");
    if (!f) return 0;
    char line[96];
    uint64_t v = 0;
    while (fgets(line, sizeof(line), f))
        if (!strncmp(line, "read_bytes:", 11)) { v = strtoull(line + 11, NULL, 10); break; }
    fclose(f);
    return v;
}

/* This thread's counters: minor/major faults, user/system CPU (clock ticks), voluntary/involuntary switches. */
static void thread_counters(uint64_t out[6])
{
    memset(out, 0, 6 * sizeof(uint64_t));
    char buf[1024];
    FILE *f = fopen("/proc/thread-self/stat", "r");
    if (f) {
        const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = 0;
        const char *p = strrchr(buf, ')');
        unsigned long long v[16] = { 0 };
        /* after ")": state(3) ppid pgrp session tty tpgid flags minflt(10) cminflt majflt(12) cmajflt utime(14) stime(15) */
        if (p && sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %llu %*u %llu %*u %llu %llu", &v[0], &v[1], &v[2], &v[3]) == 4) {
            out[0] = v[0]; out[1] = v[1]; out[2] = v[2]; out[3] = v[3];
        }
    }
    f = fopen("/proc/thread-self/status", "r");
    if (f) {
        while (fgets(buf, sizeof(buf), f)) {
            if (!strncmp(buf, "voluntary_ctxt_switches:", 24)) out[4] = strtoull(buf + 24, NULL, 10);
            else if (!strncmp(buf, "nonvoluntary_ctxt_switches:", 27)) out[5] = strtoull(buf + 27, NULL, 10);
        }
        fclose(f);
    }
}

extern uint64_t beer_thread_wait_stats(uint64_t *max_ns, uint64_t *max_handle, uint64_t *max_caller)
    __attribute__((weak));

void bd12_hitch_present(void)
{
    static int64_t threshold_ns = -1;
    if (threshold_ns < 0) {
        const char *v = getenv("BEER_BD12_HITCH");
        threshold_ns = v && *v ? (int64_t)strtol(v, NULL, 10) * 1000000 : 0;
    }
    if (!threshold_ns) return;
    static uint64_t last, frame, ns[BD12_PROF_COUNT], calls[BD12_PROF_COUNT], read_bytes;
    const uint64_t now = bd12_prof_now();
    const uint64_t io = self_read_bytes();
    uint64_t wait_max = 0, wait_handle = 0, wait_caller = 0;
    const uint64_t waited = beer_thread_wait_stats ? beer_thread_wait_stats(&wait_max, &wait_handle, &wait_caller) : 0;
    static uint64_t counters[6];
    uint64_t now_counters[6];
    thread_counters(now_counters);
    ++frame;
    if (last && (int64_t)(now - last) > threshold_ns) {
        static const char *names[] = { "execute", "submit", "pso", "resolve", "fence-wait", "finish", "present",
                                       "heap-create", "heap-destroy", "alloc-set", "update-sets", "submit-lock",
                                       "vk-submit", "reap", "list-finish", "resource-create", "view-create", "descriptor-copy" };
        fprintf(stderr, "[HITCH] frame %llu took %.1f ms:", (unsigned long long)frame, (now - last) / 1e6);
        for (int i = 0; i < BD12_PROF_COUNT; ++i) {
            /* BEER_BD12_PROF resets the counters every 2 s: a smaller value means a reset, count from zero. */
            const uint64_t dn = g_bd12_prof_ns[i] >= ns[i] ? g_bd12_prof_ns[i] - ns[i] : g_bd12_prof_ns[i];
            const uint64_t dc = g_bd12_prof_calls[i] >= calls[i] ? g_bd12_prof_calls[i] - calls[i] : g_bd12_prof_calls[i];
            if (dn >= 1000000 || (i == BD12_PROF_PSO && dc) || (i == BD12_PROF_HEAP_CREATE && dc) ||
                (i == BD12_PROF_RESOURCE && dc))
                fprintf(stderr, " %s=%.1fms/%llu", names[i], dn / 1e6, (unsigned long long)dc);
        }
        fprintf(stderr, " waited=%.1fms (longest %.1fms on %llx from 0x%llx)", waited / 1e6, wait_max / 1e6,
                (unsigned long long)wait_handle, (unsigned long long)wait_caller);
        fprintf(stderr, " thread: user=%llums sys=%llums minflt=%llu majflt=%llu csw=%llu/%llu",
                (unsigned long long)((now_counters[2] - counters[2]) * 10), (unsigned long long)((now_counters[3] - counters[3]) * 10),
                (unsigned long long)(now_counters[0] - counters[0]), (unsigned long long)(now_counters[1] - counters[1]),
                (unsigned long long)(now_counters[4] - counters[4]), (unsigned long long)(now_counters[5] - counters[5]));
        fprintf(stderr, " heaps-live=%llu/%llu/%llu read=%lluKB\n",
                (unsigned long long)g_bd12_heap_live[BD12_HEAP_DEFAULT], (unsigned long long)g_bd12_heap_live[BD12_HEAP_UPLOAD],
                (unsigned long long)g_bd12_heap_live[BD12_HEAP_READBACK],
                (unsigned long long)((io >= read_bytes ? io - read_bytes : 0) >> 10));
    }
    last = now;
    memcpy(counters, now_counters, sizeof(counters));
    memcpy(ns, g_bd12_prof_ns, sizeof(ns));
    memcpy(calls, g_bd12_prof_calls, sizeof(calls));
    read_bytes = io;
}

void bd12_prof_report(void)
{
    bd12_report_drops();
    static int enabled = -1;
    static uint64_t last;
    static int sampling;
    if (enabled < 0) {
        enabled = getenv("BEER_BD12_PROF") != NULL;
        sampling = getenv("BEER_BD12_SAMPLE") != NULL;
        if (sampling) sample_start();
    }
    if (!enabled && !sampling) return;
    const uint64_t now = bd12_prof_now();
    if (!last) last = now;
    if (now - last < 2000000000ull) return;
    static const char *names[] = { "execute", "submit", "pso", "resolve",
                                   "fence-wait", "finish", "present", "heap-create", "heap-destroy",
                                   "alloc-set", "update-sets", "submit-lock", "vk-submit", "reap", "list-finish", "resource-create", "view-create", "descriptor-copy" };
    fprintf(stderr, "[BD12 PROF] %llu ms window:", (unsigned long long)((now - last) / 1000000));
    for (int i = 0; i < BD12_PROF_COUNT; ++i) {
        fprintf(stderr, " %s=%llums/%llu", names[i],
                (unsigned long long)(g_bd12_prof_ns[i] / 1000000),
                (unsigned long long)g_bd12_prof_calls[i]);
        g_bd12_prof_ns[i] = g_bd12_prof_calls[i] = 0;
    }
    fprintf(stderr, " barriers=%llu full=%llu render-passes=%llu",
            (unsigned long long)g_bd12_cmd_counts[BD12_COUNT_BARRIER], (unsigned long long)g_bd12_cmd_counts[3],
            (unsigned long long)g_bd12_cmd_counts[BD12_COUNT_RENDER_PASS]);
    g_bd12_cmd_counts[BD12_COUNT_BARRIER] = g_bd12_cmd_counts[3] = g_bd12_cmd_counts[BD12_COUNT_RENDER_PASS] = 0;
    fprintf(stderr, " heaps(default/upload/readback)=%llu/%llu/%llu live, %llu/%llu/%llu MiB",
            (unsigned long long)g_bd12_heap_live[BD12_HEAP_DEFAULT], (unsigned long long)g_bd12_heap_live[BD12_HEAP_UPLOAD],
            (unsigned long long)g_bd12_heap_live[BD12_HEAP_READBACK],
            (unsigned long long)(g_bd12_heap_bytes[BD12_HEAP_DEFAULT] >> 20),
            (unsigned long long)(g_bd12_heap_bytes[BD12_HEAP_UPLOAD] >> 20),
            (unsigned long long)(g_bd12_heap_bytes[BD12_HEAP_READBACK] >> 20));
    {
        extern uint64_t g_beer_vm_live_bytes;
        struct mallinfo2 mi = mallinfo2();
        fprintf(stderr, " vm-live=%lluMiB malloc-arena=%lluMiB malloc-mmap=%lluMiB",
                (unsigned long long)(g_beer_vm_live_bytes >> 20),
                (unsigned long long)((unsigned long long)mi.arena >> 20),
                (unsigned long long)((unsigned long long)mi.hblkhd >> 20));
    }
    {
        fprintf(stderr, " root-changes gfx(const=%llu va=%llu table=%llu) cs(const=%llu va=%llu table=%llu)",
                (unsigned long long)__atomic_exchange_n(&g_bd12_root_changes[0][0], 0, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_exchange_n(&g_bd12_root_changes[0][1], 0, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_exchange_n(&g_bd12_root_changes[0][2], 0, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_exchange_n(&g_bd12_root_changes[1][0], 0, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_exchange_n(&g_bd12_root_changes[1][1], 0, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_exchange_n(&g_bd12_root_changes[1][2], 0, __ATOMIC_RELAXED));
        extern uint64_t g_bd12_dheap_stats[4];
        fprintf(stderr, " descriptor-heaps(created=%llu visible=%llu destroyed=%llu %llums)",
                (unsigned long long)__atomic_exchange_n(&g_bd12_dheap_stats[0], 0, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_exchange_n(&g_bd12_dheap_stats[1], 0, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_exchange_n(&g_bd12_dheap_stats[2], 0, __ATOMIC_RELAXED),
                (unsigned long long)(__atomic_exchange_n(&g_bd12_dheap_stats[3], 0, __ATOMIC_RELAXED) / 1000000));
        extern uint64_t g_bd12_resolve_cache_stats[4];
        fprintf(stderr, " resolve-cache hit=%llu miss(new=%llu state=%llu descriptors=%llu)",
                (unsigned long long)__atomic_exchange_n(&g_bd12_resolve_cache_stats[0], 0, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_exchange_n(&g_bd12_resolve_cache_stats[1], 0, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_exchange_n(&g_bd12_resolve_cache_stats[2], 0, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_exchange_n(&g_bd12_resolve_cache_stats[3], 0, __ATOMIC_RELAXED));
        uint64_t views, pinned;
        bd12_view_stats(&views, &pinned);
        fprintf(stderr, " buffer-views=%llu pinned=%llu fences-live=%llu released=%llu (release-calls=%llu completed-calls=%llu)",
                (unsigned long long)views, (unsigned long long)pinned,
                (unsigned long long)__atomic_load_n(&g_bd12_fences_live, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&g_bd12_fences_released, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&g_bd12_fence_release_calls, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&g_bd12_fence_completed_calls, __ATOMIC_RELAXED));
    }
    fputc('\n', stderr);
    bd12_gpuprof_report();
    if (sampling) sample_report();
    last = now;
}
