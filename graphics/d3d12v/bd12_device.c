#define _GNU_SOURCE
#include "bd12.h"
#include "beer_arch.h"

#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static BD12Device *g_pc_device;
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
    pthread_mutex_init(&device->va_lock, NULL);
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

    PFN_vkGetPhysicalDeviceMemoryProperties get_memory =
        (PFN_vkGetPhysicalDeviceMemoryProperties)get_instance(
            device->instance, "vkGetPhysicalDeviceMemoryProperties");
    PFN_vkGetPhysicalDeviceProperties get_properties =
        (PFN_vkGetPhysicalDeviceProperties)get_instance(
            device->instance, "vkGetPhysicalDeviceProperties");
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
    }
    device->min_uniform_alignment =
        properties.limits.minUniformBufferOffsetAlignment;
    device->max_uniform_range = properties.limits.maxUniformBufferRange;
    device->min_uniform_texel_alignment =
        modern->uniform_texel_buffer_offset_alignment ?
        modern->uniform_texel_buffer_offset_alignment :
        properties.limits.minTexelBufferOffsetAlignment;
    device->min_storage_texel_alignment =
        modern->storage_texel_buffer_offset_alignment ?
        modern->storage_texel_buffer_offset_alignment :
        properties.limits.minTexelBufferOffsetAlignment;
    device->uniform_texel_single_alignment =
        modern->uniform_texel_buffer_offset_single_texel;
    device->storage_texel_single_alignment =
        modern->storage_texel_buffer_offset_single_texel;


    /* Persistent pipeline cache: driver pipeline compiles dominate first-run loading. */
    {
        const char *xdg = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
        char dir[400];
        if (xdg && *xdg) snprintf(dir, sizeof(dir), "%s/beer", xdg);
        else snprintf(dir, sizeof(dir), "%s/.cache/beer", home ? home : "/tmp");
        mkdir(dir, 0700);
        snprintf(device->pipeline_cache_path, sizeof(device->pipeline_cache_path), "%s/pipelines-%04x-%04x-%08x.bin",
                 dir, properties.vendorID, properties.deviceID, properties.driverVersion);
        void *blob = NULL;
        size_t blob_size = 0;
        FILE *f = getenv("BEER_NO_PIPELINE_CACHE") ? NULL : fopen(device->pipeline_cache_path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (n > 0 && n < (256l << 20) && (blob = malloc((size_t)n)) && fread(blob, 1, (size_t)n, f) == (size_t)n) blob_size = (size_t)n;
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
        free(blob);
        device->pipeline_cache_saved_ms = 0;
        g_pc_device = device;
        atexit(pc_atexit);
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
        if (blob && device->vk.GetPipelineCacheData(device->device, device->pipeline_cache, &size, blob) == VK_SUCCESS) {
            char tmp[560];
            snprintf(tmp, sizeof(tmp), "%s.tmp", device->pipeline_cache_path);
            FILE *f = fopen(tmp, "wb");
            if (f) {
                const int ok = fwrite(blob, 1, size, f) == size;
                fclose(f);
                if (ok) rename(tmp, device->pipeline_cache_path); else unlink(tmp);
            }
        }
        free(blob);
    }
    device->pipeline_cache_new = 0;
    device->pipeline_cache_saved_ms = pc_now_ms();
    pthread_mutex_unlock(&lock);
}

/* Called after every pipeline build: save in the background of normal work, but not on every pipeline. */
void bd12_pipeline_cache_note_new(BD12Device *device)
{
    if (!device || !device->pipeline_cache) return;
    const uint32_t fresh = __atomic_add_fetch(&device->pipeline_cache_new, 1, __ATOMIC_RELAXED);
    if (fresh >= 25 && pc_now_ms() - device->pipeline_cache_saved_ms > 20000) bd12_pipeline_cache_save(device);
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

/* D3D12 heaps are zero-initialised; Vulkan memory is not. Shaders that read
 * counters or loop bounds from never-written buffers would otherwise run on
 * recycled garbage (GPU hang). The fill is queued ahead of every later submit
 * and its resources are released with the heap (already GPU-deferred). */
static void heap_zero_fill(BD12Heap *heap)
{
    BD12Device *device = heap->device;
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
    /* Recorded into the shared batch; bd12_zero_flush submits it before the next queue submit. */
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
    device->vk.CmdFillBuffer(device->zero_cb, heap->zero_buffer, 0, heap->size & ~(VkDeviceSize)3, 0);
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
    size_t index = va_lower_bound(device, buffer->va);
    if (index < device->va_count && device->va_table[index].buffer == buffer) {
        memmove(&device->va_table[index], &device->va_table[index + 1],
                (device->va_count - index - 1) * sizeof(*device->va_table));
        --device->va_count;
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
    if (device->vk.BindBufferMemory(device->device, buffer->buffer,
                                    heap->memory, offset) != VK_SUCCESS)
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
    for (uint32_t i = 0; i < buffer->view_count; ++i)
        device->vk.DestroyBufferView(device->device, buffer->views[i].view,
                                     NULL);
    free(buffer->views);
    free(buffer->view_hash);
    device->vk.DestroyBuffer(device->device, buffer->buffer, NULL);
    if (buffer->owns_heap) bd12_heap_destroy(buffer->heap);
    free(buffer);
}

/* Fences ---------------------------------------------------------------- */

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
    return fence;
}

void bd12_fence_destroy(BD12Fence *fence)
{
    if (!fence) return;
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

BD12Queue *bd12_queue_create(BD12Device *device)
{
    BD12Queue *queue = calloc(1, sizeof(*queue));
    if (!queue) return NULL;
    queue->device = device;
    queue->queue = device->queue;
    return queue;
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
    const uint32_t total = command_count + wait_count + signal_count;
    /* One allocation holds all three info arrays. */
    size_t bytes = command_count * sizeof(VkCommandBufferSubmitInfo) +
                   (wait_count + signal_count) * sizeof(VkSemaphoreSubmitInfo);
    VkCommandBufferSubmitInfo stack_storage[16];
    void *storage = total <= 5 ? (void *)stack_storage :
                    malloc(bytes ? bytes : 1);
    if (!storage) return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkCommandBufferSubmitInfo *command_infos = storage;
    VkSemaphoreSubmitInfo *wait_infos = (VkSemaphoreSubmitInfo *)
        (command_infos + command_count);
    VkSemaphoreSubmitInfo *signal_infos = wait_infos + wait_count;
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

    for (uint32_t i = 0; i < wait_count; ++i)
        wait_infos[i] = (VkSemaphoreSubmitInfo){
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = waits[i].fence->semaphore,
            .value = waits[i].value,
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
        .waitSemaphoreInfoCount = wait_count,
        .pWaitSemaphoreInfos = wait_infos,
        .commandBufferInfoCount = command_count,
        .pCommandBufferInfos = command_infos,

        .signalSemaphoreInfoCount = signal_count,
        .pSignalSemaphoreInfos = signal_infos
    };
    beer_vulkan_queue_lock();
    VkResult result = device->vk.QueueSubmit2(queue->queue, 1, &submit,
                                              VK_NULL_HANDLE);
    beer_vulkan_queue_unlock();
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


uint64_t g_bd12_render_tid;

#include <dlfcn.h>
#include <malloc.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <unistd.h>

/* BEER_BD12_SAMPLE needs glibc backtrace() and x86 register context. */
#if BEER_HAVE_GLIBC_DIAG
#define _GNU_SOURCE_PROF
#include <execinfo.h>
#include <ucontext.h>

/* BEER_BD12_SAMPLE=1: SIGPROF instruction-pointer histogram, printed with the
 * profile window. Diagnostic only. */
#define SAMPLE_SLOTS 8192
static struct { uint64_t rip; uint64_t tid; uint64_t count; } g_samples[SAMPLE_SLOTS];

#define STACK_SLOTS 512
static struct { void *frames[14]; uint64_t count; } g_stacks[STACK_SLOTS];

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
#else
static void sample_start(void) {}
static void sample_report(void) {}
#endif

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
                                   "alloc-set", "update-sets" };
    fprintf(stderr, "[BD12 PROF] %llu ms window:", (unsigned long long)((now - last) / 1000000));
    for (int i = 0; i < BD12_PROF_COUNT; ++i) {
        fprintf(stderr, " %s=%llums/%llu", names[i],
                (unsigned long long)(g_bd12_prof_ns[i] / 1000000),
                (unsigned long long)g_bd12_prof_calls[i]);
        g_bd12_prof_ns[i] = g_bd12_prof_calls[i] = 0;
    }
    fprintf(stderr, " heaps(default/upload/readback)=%llu/%llu/%llu live, %llu/%llu/%llu MiB",
            (unsigned long long)g_bd12_heap_live[0], (unsigned long long)g_bd12_heap_live[1], (unsigned long long)g_bd12_heap_live[2],
            (unsigned long long)(g_bd12_heap_bytes[0] >> 20), (unsigned long long)(g_bd12_heap_bytes[1] >> 20), (unsigned long long)(g_bd12_heap_bytes[2] >> 20));
    {
        extern uint64_t g_beer_vm_live_bytes;
        struct mallinfo2 mi = mallinfo2();
        fprintf(stderr, " vm-live=%lluMiB malloc-arena=%lluMiB malloc-mmap=%lluMiB",
                (unsigned long long)(g_beer_vm_live_bytes >> 20),
                (unsigned long long)((unsigned long long)mi.arena >> 20),
                (unsigned long long)((unsigned long long)mi.hblkhd >> 20));
    }
    fputc('\n', stderr);
    if (sampling) sample_report();
    last = now;
}
