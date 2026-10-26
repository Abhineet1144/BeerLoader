#define _GNU_SOURCE
#include "bd12.h"
#include "bd12_formats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* DXR acceleration structures on VK_KHR_acceleration_structure.
 *
 * D3D12 places an acceleration structure at a GPU virtual address inside an
 * ordinary buffer, and TLAS instance descriptors refer to a BLAS by that
 * address. BD12 virtual addresses are real Vulkan buffer device addresses and
 * the 64-byte D3D12 instance descriptor has the same layout as
 * VkAccelerationStructureInstanceKHR, so a VkAccelerationStructureKHR created
 * at (buffer, offset) is addressed by exactly the VA the application uses. */

typedef struct { uint64_t va, stride; } RtVaStride;
typedef struct {
    uint64_t transform;
    uint32_t index_format, vertex_format, index_count, vertex_count;
    uint64_t index_buffer;
    RtVaStride vertex;
} RtTriangles;
typedef struct { uint64_t count; RtVaStride aabbs; } RtAabbs;
typedef struct {
    uint32_t type, flags;
    union { RtTriangles triangles; RtAabbs aabbs; };
} RtGeometry;
typedef struct {
    uint32_t type, flags, num_descs, layout;
    uint64_t descs;               /* GPU VA, or CPU pointer for geometry */
} RtInputs;
typedef struct {
    uint64_t dest;
    RtInputs inputs;
    uint64_t source, scratch;
} RtBuildDesc;

enum { RT_TLAS = 0, RT_BLAS = 1 };

void bd12_rt_load(BD12Device *device)
{
    if (!beer_vulkan_ray_tracing_enabled()) return;
    PFN_vkGetDeviceProcAddr get_device = beer_vulkan_get_device_proc_addr();
#define BD12_VK_RT_FN(name)                                                   \
    device->vk_rt.name =                                                      \
        (PFN_vk##name)get_device(device->device, "vk" #name);                 \
    if (!device->vk_rt.name) {                                                \
        fprintf(stderr, "[BD12] missing vk" #name ", DXR disabled\n");        \
        return;                                                               \
    }
#include "bd12_vk_rt_functions.h"
#undef BD12_VK_RT_FN
    {   /* Ray tracing pipeline limits: shader record handle size and alignments. */
        PFN_vkGetInstanceProcAddr get_instance = beer_vulkan_get_instance_proc_addr();
        PFN_vkGetPhysicalDeviceProperties2 props2 = (PFN_vkGetPhysicalDeviceProperties2)
            get_instance(beer_vulkan_instance(), "vkGetPhysicalDeviceProperties2");
        VkPhysicalDeviceRayTracingPipelinePropertiesKHR rt = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR };
        VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &rt };
        if (props2) props2(beer_vulkan_physical_device(), &p2);
        device->rt_handle_size = rt.shaderGroupHandleSize ? rt.shaderGroupHandleSize : 32;
        device->rt_base_alignment = rt.shaderGroupBaseAlignment ? rt.shaderGroupBaseAlignment : 64;
        device->rt_handle_alignment = rt.shaderGroupHandleAlignment ? rt.shaderGroupHandleAlignment : 32;
        device->rt_max_recursion = rt.maxRayRecursionDepth ? rt.maxRayRecursionDepth : 1;
        device->rt_max_stride = rt.maxShaderGroupStride ? rt.maxShaderGroupStride : 4096;
        fprintf(stderr, "[BD12 RT] shader group handle %u bytes, base alignment %u, handle alignment %u, max recursion %u\n",
                device->rt_handle_size, device->rt_base_alignment, device->rt_handle_alignment, device->rt_max_recursion);
    }
    device->rt_enabled = 1;
}

static VkFormat vertex_format(uint32_t dxgi)
{
    return bd12_dxgi_to_vk(dxgi);
}

/* Translates a D3D12 geometry list into Vulkan geometries. Returns the
 * geometry count; `counts` receives the primitive count of each. */
static uint32_t make_geometries(const RtInputs *in,
                                VkAccelerationStructureGeometryKHR *geoms,
                                uint32_t *counts)
{
    const uint32_t n = in->num_descs;
    if (in->type == RT_TLAS) {
        geoms[0] = (VkAccelerationStructureGeometryKHR){
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
            .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
            .geometry.instances = {
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
                .arrayOfPointers = in->layout == 1,
                .data.deviceAddress = in->descs
            }
        };
        counts[0] = n;
        return 1;
    }
    for (uint32_t i = 0; i < n; ++i) {
        const RtGeometry *g = in->layout == 1 ?
            ((const RtGeometry *const *)(uintptr_t)in->descs)[i] :
            &((const RtGeometry *)(uintptr_t)in->descs)[i];
        VkAccelerationStructureGeometryKHR *out = &geoms[i];
        *out = (VkAccelerationStructureGeometryKHR){
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
            .flags = g->flags & 3u    /* OPAQUE, NO_DUPLICATE_ANYHIT_INVOCATION */
        };
        if (g->type == 0) {
            const RtTriangles *t = &g->triangles;
            out->geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            out->geometry.triangles = (VkAccelerationStructureGeometryTrianglesDataKHR){
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                .vertexFormat = vertex_format(t->vertex_format),
                .vertexData.deviceAddress = t->vertex.va,
                .vertexStride = t->vertex.stride,
                .maxVertex = t->vertex_count ? t->vertex_count - 1 : 0,
                .indexType = t->index_format == 57 ? VK_INDEX_TYPE_UINT16 :
                             t->index_format == 42 ? VK_INDEX_TYPE_UINT32 :
                                                     VK_INDEX_TYPE_NONE_KHR,
                .indexData.deviceAddress = t->index_buffer,
                .transformData.deviceAddress = t->transform
            };
            counts[i] = (t->index_format ? t->index_count : t->vertex_count) / 3;
        } else {
            out->geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
            out->geometry.aabbs = (VkAccelerationStructureGeometryAabbsDataKHR){
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR,
                .data.deviceAddress = g->aabbs.aabbs.va,
                .stride = g->aabbs.aabbs.stride
            };
            counts[i] = (uint32_t)g->aabbs.count;
        }
    }
    return n;
}

static VkAccelerationStructureBuildGeometryInfoKHR make_build_info(
    const RtInputs *in, const VkAccelerationStructureGeometryKHR *geoms,
    uint32_t geometry_count)
{
    return (VkAccelerationStructureBuildGeometryInfoKHR){
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = in->type == RT_TLAS ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR :
                                      VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
        /* D3D12 and Vulkan share the low five build flag bits. */
        .flags = in->flags & 0x1fu,
        .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
        .geometryCount = geometry_count,
        .pGeometries = geoms
    };
}

static VkDeviceSize align256(VkDeviceSize v) { return (v + 255) & ~(VkDeviceSize)255; }

int bd12_rt_prebuild(BD12Device *device, const void *inputs, uint64_t sizes[3])
{
    const RtInputs *in = inputs;
    if (!device->rt_enabled || !in) return 0;
    const uint32_t cap = in->type == RT_TLAS ? 1 : (in->num_descs ? in->num_descs : 1);
    VkAccelerationStructureGeometryKHR *geoms = calloc(cap, sizeof(*geoms));
    uint32_t *counts = calloc(cap, sizeof(*counts));
    if (!geoms || !counts) { free(geoms); free(counts); return 0; }
    const uint32_t n = in->num_descs || in->type == RT_TLAS ?
                       make_geometries(in, geoms, counts) : 0;
    VkAccelerationStructureBuildGeometryInfoKHR info = make_build_info(in, geoms, n);
    VkAccelerationStructureBuildSizesInfoKHR out = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR
    };
    device->vk_rt.GetAccelerationStructureBuildSizesKHR(
        device->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info,
        counts, &out);
    free(geoms);
    free(counts);
    sizes[0] = align256(out.accelerationStructureSize);
    sizes[1] = align256(out.buildScratchSize);
    sizes[2] = align256(out.updateScratchSize);
    return 1;
}

/* Acceleration structure objects ------------------------------------------- */

static BD12AccelStruct *find_accel(BD12Buffer *buffer, VkDeviceSize offset)
{
    for (uint32_t i = 0; i < buffer->accel_count; ++i)
        if (buffer->accel[i].offset == offset) return &buffer->accel[i];
    return NULL;
}

/* Returns the acceleration structure at `va`, creating (or recreating, when
 * type or size changed) it. `size` 0 keeps an existing one. */
static BD12AccelStruct *get_accel(BD12Device *device, uint64_t va,
                                  VkAccelerationStructureTypeKHR type,
                                  VkDeviceSize size)
{
    VkDeviceSize offset;
    BD12Buffer *buffer = bd12_va_resolve(device, va, &offset);
    if (!buffer) return NULL;
    pthread_mutex_lock(&device->view_lock);
    BD12AccelStruct *entry = find_accel(buffer, offset);
    if (entry && (!size || (entry->size == size && entry->type == type))) {
        pthread_mutex_unlock(&device->view_lock);
        return entry;
    }
    if (!size) { pthread_mutex_unlock(&device->view_lock); return NULL; }
    if (offset + size > buffer->size) size = buffer->size - offset;
    if (entry) {
        device->vk_rt.DestroyAccelerationStructureKHR(device->device,
                                                      entry->handle, NULL);
        entry->handle = VK_NULL_HANDLE;
    } else {
        if (buffer->accel_count == buffer->accel_capacity) {
            uint32_t capacity = buffer->accel_capacity ? buffer->accel_capacity * 2 : 2;
            BD12AccelStruct *grown = realloc(buffer->accel, capacity * sizeof(*grown));
            if (!grown) { pthread_mutex_unlock(&device->view_lock); return NULL; }
            buffer->accel = grown;
            buffer->accel_capacity = capacity;
        }
        entry = &buffer->accel[buffer->accel_count++];
    }
    VkAccelerationStructureCreateInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
        .buffer = buffer->buffer,
        .offset = offset,
        .size = size,
        .type = type
    };
    *entry = (BD12AccelStruct){ offset, size, type, VK_NULL_HANDLE };
    if (device->vk_rt.CreateAccelerationStructureKHR(device->device, &info, NULL,
                                                     &entry->handle) != VK_SUCCESS) {
        fprintf(stderr, "[BD12 RT] CreateAccelerationStructure failed va=%llx size=%llu\n",
                (unsigned long long)va, (unsigned long long)size);
        --buffer->accel_count;
        pthread_mutex_unlock(&device->view_lock);
        return NULL;
    }
    {   /* The design relies on AS address == buffer VA + offset. */
        static int checked;
        if (!checked) {
            checked = 1;
            VkAccelerationStructureDeviceAddressInfoKHR address = {
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
                .accelerationStructure = entry->handle
            };
            const uint64_t got =
                device->vk_rt.GetAccelerationStructureDeviceAddressKHR(device->device,
                                                                        &address);
            fprintf(stderr, "[BD12 RT] acceleration structure address %s (as=%llx va=%llx)\n",
                    got == va ? "matches VA" : "DIFFERS FROM VA",
                    (unsigned long long)got, (unsigned long long)va);
        }
    }
    pthread_mutex_unlock(&device->view_lock);
    return entry;
}

/* The acceleration structure object living at `va` (created by a build or copy), or NULL. */
VkAccelerationStructureKHR bd12_rt_as_from_va(BD12Device *device, uint64_t va)
{
    if (!device->rt_enabled) return VK_NULL_HANDLE;
    BD12AccelStruct *entry = get_accel(device, va, 0, 0);
    return entry ? entry->handle : VK_NULL_HANDLE;
}

void bd12_rt_buffer_release(BD12Buffer *buffer)
{
    if (!buffer || !buffer->accel) return;
    BD12Device *device = buffer->device;
    for (uint32_t i = 0; i < buffer->accel_count; ++i)
        if (buffer->accel[i].handle)
            device->vk_rt.DestroyAccelerationStructureKHR(
                device->device, buffer->accel[i].handle, NULL);
    free(buffer->accel);
    buffer->accel = NULL;
    buffer->accel_count = buffer->accel_capacity = 0;
}

/* Recording ---------------------------------------------------------------- */

void bd12_cmd_build_as(BD12CmdList *list, const void *desc)
{
    BD12Device *device = list->device;
    const RtBuildDesc *d = desc;
    if (!device->rt_enabled || !d) return;
    const RtInputs *in = &d->inputs;
    uint64_t sizes[3];
    if (!bd12_rt_prebuild(device, in, sizes)) return;
    const VkAccelerationStructureTypeKHR type = in->type == RT_TLAS ?
        VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR :
        VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    const int update = (in->flags & 0x20u) != 0 && d->source;
    BD12AccelStruct *dst = get_accel(device, d->dest, type, update ? 0 : sizes[0]);
    BD12AccelStruct *src = update ? get_accel(device, d->source, type, 0) : NULL;
    if (!dst) dst = get_accel(device, d->dest, type, sizes[0]);
    if (getenv("BEER_DXR_PROBE")) {
        static unsigned blas, tlas;
        unsigned *counter = in->type == RT_TLAS ? &tlas : &blas;
        ++*counter;
        if (*counter <= 3 || !(*counter % 500))
            fprintf(stderr, "[BD12 RT] build #%u %s: descs=%u flags=0x%x update=%d size=%llu scratch=%llu %s\n", *counter,
                    in->type == RT_TLAS ? "TLAS" : "BLAS", in->num_descs, in->flags, update, (unsigned long long)sizes[0],
                    (unsigned long long)sizes[2], dst ? "ok" : "NO DEST");
    }
    if (!dst) return;

    const uint32_t cap = in->type == RT_TLAS ? 1 : (in->num_descs ? in->num_descs : 1);
    VkAccelerationStructureGeometryKHR *geoms = calloc(cap, sizeof(*geoms));
    uint32_t *counts = calloc(cap, sizeof(*counts));
    VkAccelerationStructureBuildRangeInfoKHR *ranges = calloc(cap, sizeof(*ranges));
    if (!geoms || !counts || !ranges) goto done;
    const uint32_t n = make_geometries(in, geoms, counts);
    VkAccelerationStructureBuildGeometryInfoKHR info = make_build_info(in, geoms, n);
    info.dstAccelerationStructure = dst->handle;
    info.scratchData.deviceAddress = d->scratch;
    if (src) {
        info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
        info.srcAccelerationStructure = src->handle;
    }
    for (uint32_t i = 0; i < n; ++i) ranges[i].primitiveCount = counts[i];
    const VkAccelerationStructureBuildRangeInfoKHR *range_ptr = ranges;
    bd12_cmd_global_barrier(list);
    device->vk_rt.CmdBuildAccelerationStructuresKHR(list->command_buffer, 1, &info,
                                                    &range_ptr);
    bd12_cmd_global_barrier(list);
done:
    free(geoms);
    free(counts);
    free(ranges);
}

void bd12_cmd_copy_as(BD12CmdList *list, uint64_t dst_va, uint64_t src_va,
                      uint32_t mode)
{
    BD12Device *device = list->device;
    if (!device->rt_enabled) return;
    BD12AccelStruct *src = get_accel(device, src_va, 0, 0);
    if (!src) return;
    /* COMPACT is executed as a clone: the compacted size reported to the
     * application is the full size, so its destination is large enough. */
    BD12AccelStruct *dst = get_accel(device, dst_va, src->type, src->size);
    if (!dst) return;
    (void)mode;
    VkCopyAccelerationStructureInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR,
        .src = src->handle,
        .dst = dst->handle,
        .mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_CLONE_KHR
    };
    bd12_cmd_global_barrier(list);
    device->vk_rt.CmdCopyAccelerationStructureKHR(list->command_buffer, &info);
    bd12_cmd_global_barrier(list);
}

void bd12_cmd_emit_as_info(BD12CmdList *list, uint64_t dest, uint32_t type,
                           uint32_t source_count, const uint64_t *sources)
{
    BD12Device *device = list->device;
    VkDeviceSize offset;
    BD12Buffer *buffer = bd12_va_resolve(device, dest, &offset);
    if (getenv("BEER_DXR_PROBE")) {
        static unsigned emits;
        if (++emits <= 5 || !(emits % 500))
            fprintf(stderr, "[BD12 RT] postbuild info #%u type=%u sources=%u dest=%s\n", emits, type, source_count, buffer ? "ok" : "UNRESOLVED");
    }
    if (!buffer) return;
    /* COMPACTED_SIZE(0), VISUALIZATION(1), CURRENT_SIZE(3): one u64 each;
     * SERIALIZATION(2): serialized size plus BLAS pointer count. */
    const uint32_t words = type == 2 ? 2 : 1;
    for (uint32_t i = 0; i < source_count; ++i) {
        BD12AccelStruct *src = get_accel(device, sources[i], 0, 0);
        uint64_t data[2] = { src ? src->size : 0, 0 };
        bd12_cmd_update_buffer(list, buffer, offset + (uint64_t)i * words * 8,
                               words * 8, data);
    }
}
