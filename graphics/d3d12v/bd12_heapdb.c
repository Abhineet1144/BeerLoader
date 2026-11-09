#define _GNU_SOURCE
#include "bd12.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* D3D12 descriptor heaps as Vulkan descriptor buffers (VK_EXT_descriptor_buffer + VK_EXT_mutable_descriptor_type).
 *
 * Every CBV/SRV/UAV and sampler heap carries the Vulkan descriptor bytes of its descriptors next to BD12's own
 * descriptor records. For a shader-visible heap those bytes live in a host-visible descriptor buffer, so creating
 * a view or copying descriptors into the heap is the GPU-visible write: no descriptor sets, update calls, locks or
 * per-draw resolving. CPU-only heaps keep the bytes in host memory and CopyDescriptors copies them over.
 *
 * Shaders translated for their root signature (bd12_rootpush.c, BEER_HEAP_MODEL_MUTABLE) see:
 *   set 0, binding 0: the CBV/SRV/UAV heap, one array of mutable descriptors indexed by heap slot;
 *   set 1, binding 0: the sampler heap;
 *   set 2, binding i: the root signature's static sampler i (embedded immutable samplers).
 * Sets 0 and 1 and the push-constant range are identical in every such pipeline layout, so the heaps stay bound
 * across pipeline and root-signature changes; only set 2 follows the root signature.
 *
 * A D3D12 CPU descriptor handle is a pointer into a heap's descriptor array. Heaps created through the API live in
 * granule-aligned mappings registered in a lock-free table, so the heap of any handle is found in O(1). */

#define GRANULE_SHIFT 20u                       /* 1 MiB: 8192 descriptors */
#define GRANULE ((size_t)1 << GRANULE_SHIFT)
#define REG_SLOTS 65536u
#define HEAP_SLOTS (1u << 20)                   /* descriptors addressable through set 0 */
#define SAMPLER_SLOTS 2048u                     /* D3D12 sampler heap limit */
#define MAX_STATIC 64u

/* T_ACCEL is last: it joins the mutable type list only with ray tracing on and a driver that accepts it
 * (db->mutable_count), so heaps without it keep exactly the layout and stride they had before. */
enum { T_SAMPLED, T_STORAGE_IMAGE, T_UNIFORM_TEXEL, T_STORAGE_TEXEL, T_UBO, T_SSBO, T_ACCEL, T_COUNT };
static const VkDescriptorType k_mutable_types[T_COUNT] = {
    VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,
    VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
    VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR
};

typedef struct BD12DbLayout {
    VkDescriptorSetLayout statics;              /* set 2 */
    VkPipelineLayout layout;
    uint32_t static_count;
    VkSampler samplers[MAX_STATIC];
} BD12DbLayout;

struct BD12DescriptorBuffers {
    PFN_vkGetDescriptorEXT GetDescriptor;
    PFN_vkGetDescriptorSetLayoutSizeEXT GetLayoutSize;
    PFN_vkGetDescriptorSetLayoutBindingOffsetEXT GetBindingOffset;
    PFN_vkCmdBindDescriptorBuffersEXT CmdBindBuffers;
    PFN_vkCmdSetDescriptorBufferOffsetsEXT CmdSetOffsets;
    PFN_vkCmdBindDescriptorBufferEmbeddedSamplersEXT CmdBindEmbedded;
    uint32_t type_size[T_COUNT];
    uint32_t mutable_count;       /* entries of k_mutable_types in the heap's mutable list (T_ACCEL or T_SSBO + 1) */
    uint32_t sampler_size;
    uint32_t resource_stride, sampler_stride;
    VkDeviceSize resource_offset, sampler_offset;   /* binding 0 within its set */
    VkDeviceSize offset_alignment;
    uint64_t max_texel_elements;
    int null_descriptor;
    VkDescriptorSetLayout resource_layout, sampler_layout;
    uint32_t push_size;
    VkShaderStageFlags push_stages;
    int32_t memory_type;
    /* Bound when a list has no heap of that kind: shaders must not read them, but sets 0 and 1 need a buffer. */
    BD12DescriptorHeap *dummy[2];
    pthread_mutex_t lock;                       /* layouts */
    BD12DbLayout **layouts;
    size_t layout_count, layout_capacity;
    /* Images referenced by heap descriptors whose first-use layout transition is not recorded yet. */
    pthread_mutex_t pending_lock;
    BD12Image **pending;
    size_t pending_count, pending_capacity;
};

/* Handle -> heap registry ----------------------------------------------------------------------------------------- */

typedef struct {
    uintptr_t key;                              /* granule index + 1; 0 = never used */
    BD12DescriptorHeap *heap;                   /* NULL = granule free again */
} RegSlot;
static RegSlot g_reg[REG_SLOTS];
static pthread_mutex_t g_reg_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t reg_hash(uintptr_t key)
{
    uint64_t h = key * 0x9E3779B97F4A7C15ull;
    return (uint32_t)(h >> 40) & (REG_SLOTS - 1);
}

/* Readers never lock: a slot's key is written once and only its heap pointer changes afterwards. */
BD12DescriptorHeap *bd12_heap_from_cpu(const void *handle)
{
    const uintptr_t key = ((uintptr_t)handle >> GRANULE_SHIFT) + 1;
    for (uint32_t i = reg_hash(key), n = 0; n < REG_SLOTS; i = (i + 1) & (REG_SLOTS - 1), ++n) {
        const uintptr_t k = __atomic_load_n(&g_reg[i].key, __ATOMIC_ACQUIRE);
        if (!k) return NULL;
        if (k == key) {
            BD12DescriptorHeap *heap = __atomic_load_n(&g_reg[i].heap, __ATOMIC_ACQUIRE);
            if (!heap) return NULL;
            const BD12Descriptor *d = handle;
            return d >= heap->descriptors && d < heap->descriptors + heap->count ? heap : NULL;
        }
    }
    return NULL;
}

static int reg_set(uintptr_t first_granule, size_t granules, BD12DescriptorHeap *heap)
{
    pthread_mutex_lock(&g_reg_lock);
    size_t done = 0;
    for (; done < granules; ++done) {
        const uintptr_t key = first_granule + done + 1;
        uint32_t i = reg_hash(key), n = 0;
        for (; n < REG_SLOTS; i = (i + 1) & (REG_SLOTS - 1), ++n)
            if (!g_reg[i].key || g_reg[i].key == key) break;
        if (n == REG_SLOTS) break;
        __atomic_store_n(&g_reg[i].heap, heap, __ATOMIC_RELEASE);
        if (!g_reg[i].key) __atomic_store_n(&g_reg[i].key, key, __ATOMIC_RELEASE);
    }
    if (done < granules && heap) {   /* table full: undo, the heap stays unregistered */
        for (size_t j = 0; j < done; ++j) {
            const uintptr_t key = first_granule + j + 1;
            for (uint32_t i = reg_hash(key);; i = (i + 1) & (REG_SLOTS - 1))
                if (g_reg[i].key == key) { __atomic_store_n(&g_reg[i].heap, NULL, __ATOMIC_RELEASE); break; }
        }
    }
    pthread_mutex_unlock(&g_reg_lock);
    return done == granules;
}

/* Heap storage ---------------------------------------------------------------------------------------------------- */

static void *map_aligned(size_t bytes)
{
    const size_t total = bytes + GRANULE;
    uint8_t *raw = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (raw == MAP_FAILED) return NULL;
    uint8_t *start = (uint8_t *)(((uintptr_t)raw + GRANULE - 1) & ~(uintptr_t)(GRANULE - 1));
    if (start > raw) munmap(raw, (size_t)(start - raw));
    const size_t tail = (size_t)(raw + total - (start + bytes));
    if (tail) munmap(start + bytes, tail);
    return start;
}

static int heap_buffer_create(BD12Device *device, BD12DescriptorHeap *heap, VkDeviceSize bytes, int samplers)
{
    struct BD12DescriptorBuffers *db = device->db;
    VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes,
        .usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                 (samplers ? VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT
                           : VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    if (device->vk.CreateBuffer(device->device, &info, NULL, &heap->db_buffer) != VK_SUCCESS) return 0;
    VkMemoryRequirements req;
    device->vk.GetBufferMemoryRequirements(device->device, heap->db_buffer, &req);
    int32_t type = -1;
    for (int pass = 0; pass < 2 && type < 0; ++pass)
        for (uint32_t i = 0; i < device->memory.memoryTypeCount; ++i) {
            const VkMemoryPropertyFlags f = device->memory.memoryTypes[i].propertyFlags;
            const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                               (pass == 0 ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT : 0);
            if ((req.memoryTypeBits & (1u << i)) && (f & want) == want) { type = (int32_t)i; break; }
        }
    VkMemoryAllocateFlagsInfo flags = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
    };
    VkMemoryAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &flags,
        .allocationSize = req.size,
        .memoryTypeIndex = (uint32_t)type
    };
    void *mapped = NULL;
    if (type < 0 ||
        device->vk.AllocateMemory(device->device, &allocate, NULL, &heap->db_memory) != VK_SUCCESS ||
        device->vk.BindBufferMemory(device->device, heap->db_buffer, heap->db_memory, 0) != VK_SUCCESS ||
        device->vk.MapMemory(device->device, heap->db_memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        if (heap->db_memory) device->vk.FreeMemory(device->device, heap->db_memory, NULL);
        device->vk.DestroyBuffer(device->device, heap->db_buffer, NULL);
        heap->db_buffer = VK_NULL_HANDLE;
        heap->db_memory = VK_NULL_HANDLE;
        return 0;
    }
    memset(mapped, 0, bytes);
    VkBufferDeviceAddressInfo address = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = heap->db_buffer
    };
    heap->db_address = device->vk.GetBufferDeviceAddress(device->device, &address);
    heap->payload = (uint8_t *)mapped + (samplers ? db->sampler_offset : db->resource_offset);
    return 1;
}

static BD12DescriptorHeap *descriptor_heap_create(BD12Device *device, uint32_t type, uint32_t count, uint32_t flags);

/* Vulkan descriptor storage for `heap` (called once its descriptors exist). */
static void heap_attach_payload(BD12DescriptorHeap *heap)
{
    BD12Device *device = heap->device;
    struct BD12DescriptorBuffers *db = device ? device->db : NULL;
    if (!db || (heap->flags & BD12_HEAP_FLAG_NO_PAYLOAD)) return;
    const int samplers = heap->type == 1;
    const uint32_t stride = samplers ? db->sampler_stride : db->resource_stride;
    const uint32_t limit = samplers ? SAMPLER_SLOTS : HEAP_SLOTS;
    heap->payload_stride = stride;
    if (heap->flags & BD12_HEAP_FLAG_SHADER_VISIBLE) {
        if (heap->count > limit) {
            fprintf(stderr, "[BD12] %s heap of %u descriptors exceeds the descriptor-buffer layout (%u)\n",
                    samplers ? "sampler" : "CBV/SRV/UAV", heap->count, limit);
            heap->payload_stride = 0;
            return;
        }
        const VkDeviceSize offset = samplers ? db->sampler_offset : db->resource_offset;
        if (!heap_buffer_create(device, heap, offset + (VkDeviceSize)heap->count * stride, samplers)) {
            fprintf(stderr, "[BD12] descriptor buffer for a %u-descriptor heap failed\n", heap->count);
            heap->payload_stride = 0;
        }
        return;
    }
    heap->payload = calloc(heap->count, stride);
    if (!heap->payload) heap->payload_stride = 0;
}

/* BEER_BD12_PROF: descriptor heaps created/destroyed (shader-visible ones get a driver allocation) and time spent. */
uint64_t g_bd12_dheap_stats[4];   /* created, shader-visible created, destroyed, ns */

BD12DescriptorHeap *bd12_descriptor_heap_create_ex(BD12Device *device, uint32_t type, uint32_t count, uint32_t flags)
{
    const uint64_t t0 = bd12_prof_now();
    BD12DescriptorHeap *heap = descriptor_heap_create(device, type, count, flags);
    __atomic_add_fetch(&g_bd12_dheap_stats[0], 1, __ATOMIC_RELAXED);
    if (flags & BD12_HEAP_FLAG_SHADER_VISIBLE) __atomic_add_fetch(&g_bd12_dheap_stats[1], 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_bd12_dheap_stats[3], bd12_prof_now() - t0, __ATOMIC_RELAXED);
    return heap;
}

static BD12DescriptorHeap *descriptor_heap_create(BD12Device *device, uint32_t type, uint32_t count, uint32_t flags)
{
    if (!count) return NULL;
    BD12DescriptorHeap *heap = calloc(1, sizeof(*heap));
    if (!heap) return NULL;
    heap->device = device;
    heap->type = type;
    heap->count = count;
    heap->flags = flags;
    heap->gpu_base = bd12_heap_next_gpu_base();
    const size_t bytes = ((size_t)count * sizeof(BD12Descriptor) + GRANULE - 1) & ~(GRANULE - 1);
    heap->region = map_aligned(bytes);
    if (!heap->region) { free(heap); return NULL; }
    heap->region_bytes = bytes;
    heap->descriptors = heap->region;
    if (!reg_set((uintptr_t)heap->region >> GRANULE_SHIFT, bytes >> GRANULE_SHIFT, heap)) {
        static int warned;
        if (!warned++) fprintf(stderr, "[BD12] descriptor heap registry full: heap writes skip Vulkan descriptors\n");
    } else {
        heap_attach_payload(heap);
    }
    return heap;
}

/* Releases what bd12_descriptor_heap_create_ex added; 1 when the heap was one of those. */
int bd12_heapdb_heap_destroy(BD12DescriptorHeap *heap)
{
    if (!heap->region) return 0;
    __atomic_add_fetch(&g_bd12_dheap_stats[2], 1, __ATOMIC_RELAXED);
    BD12Device *device = heap->device;
    reg_set((uintptr_t)heap->region >> GRANULE_SHIFT, heap->region_bytes >> GRANULE_SHIFT, NULL);
    if (heap->db_buffer) {
        device->vk.DestroyBuffer(device->device, heap->db_buffer, NULL);
        device->vk.FreeMemory(device->device, heap->db_memory, NULL);
    } else {
        free(heap->payload);
    }
    munmap(heap->region, heap->region_bytes);
    return 1;
}

/* Descriptor encoding --------------------------------------------------------------------------------------------- */

static uint32_t type_size(const struct BD12DescriptorBuffers *db, VkDescriptorType type)
{
    for (uint32_t i = 0; i < db->mutable_count; ++i)
        if (k_mutable_types[i] == type) return db->type_size[i];
    return type == VK_DESCRIPTOR_TYPE_SAMPLER ? db->sampler_size : 0;
}

static void queue_init(struct BD12DescriptorBuffers *db, BD12Image *image)
{
    if (!image || __atomic_load_n(&image->initialized, __ATOMIC_ACQUIRE)) return;
    pthread_mutex_lock(&db->pending_lock);
    for (size_t i = db->pending_count; i-- > 0 && i + 8 >= db->pending_count; )
        if (db->pending[i] == image) { pthread_mutex_unlock(&db->pending_lock); return; }
    if (db->pending_count == db->pending_capacity) {
        const size_t capacity = db->pending_capacity ? db->pending_capacity * 2 : 256;
        BD12Image **grown = realloc(db->pending, capacity * sizeof(*grown));
        if (!grown) { pthread_mutex_unlock(&db->pending_lock); return; }
        db->pending = grown;
        db->pending_capacity = capacity;
    }
    db->pending[db->pending_count++] = image;
    pthread_mutex_unlock(&db->pending_lock);
}

void bd12_db_forget_image(BD12Image *image)
{
    struct BD12DescriptorBuffers *db = image && image->device ? image->device->db : NULL;
    if (!db) return;
    pthread_mutex_lock(&db->pending_lock);
    for (size_t i = 0; i < db->pending_count; ) {
        if (db->pending[i] == image) db->pending[i] = db->pending[--db->pending_count];
        else ++i;
    }
    pthread_mutex_unlock(&db->pending_lock);
}

/* A texel-buffer view of `d`, checked like bd12_buffer_view: 0 when the range is not addressable. */
static int texel_address(BD12Device *device, const BD12Descriptor *d, int storage, VkDescriptorAddressInfoEXT *out)
{
    const BD12Buffer *buffer = d->source_buffer;
    if (!buffer || !buffer->va || d->format == VK_FORMAT_UNDEFINED || d->offset >= buffer->size) return 0;
    uint32_t texel = 0;
    bd12_format_block(d->format, VK_IMAGE_ASPECT_COLOR_BIT, NULL, NULL, &texel);
    if (!texel) return 0;
    VkDeviceSize alignment = storage ? device->min_storage_texel_alignment : device->min_uniform_texel_alignment;
    if ((storage ? device->storage_texel_single_alignment : device->uniform_texel_single_alignment) &&
        texel < alignment)
        alignment = texel;
    if (alignment > 1 && d->offset % alignment) return 0;
    VkDeviceSize range = d->range;
    if (range == VK_WHOLE_SIZE || d->offset + range > buffer->size) range = buffer->size - d->offset;
    range -= range % texel;
    const uint64_t max_bytes = device->db->max_texel_elements * texel;
    if (range > max_bytes) range = max_bytes;
    if (!range) return 0;
    *out = (VkDescriptorAddressInfoEXT){
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT,
        .address = buffer->va + d->offset,
        .range = range,
        .format = d->format
    };
    return 1;
}

/* Writes the Vulkan descriptor for `d` to `out` (`stride` bytes). `null_type` is the descriptor type a null
 * descriptor in this slot stands for (VK_DESCRIPTOR_TYPE_MAX_ENUM: unknown, zero bytes). */
static void encode(BD12Device *device, const BD12Descriptor *d, VkDescriptorType null_type, uint8_t *out,
                   uint32_t stride)
{
    struct BD12DescriptorBuffers *db = device->db;
    VkDescriptorImageInfo image;
    VkDescriptorAddressInfoEXT address;
    VkDescriptorGetInfoEXT info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT };
    switch (d->kind) {
    case BD12_DESC_SRV_IMAGE:
    case BD12_DESC_UAV_IMAGE: {
        const int storage = d->kind == BD12_DESC_UAV_IMAGE;
        info.type = storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        if (!d->image_view) break;
        image = (VkDescriptorImageInfo){ VK_NULL_HANDLE, d->image_view,
                                         d->image_layout ? d->image_layout : VK_IMAGE_LAYOUT_GENERAL };
        if (storage) info.data.pStorageImage = &image; else info.data.pSampledImage = &image;
        queue_init(db, d->image);
        break;
    }
    case BD12_DESC_SRV_BUFFER:
    case BD12_DESC_UAV_BUFFER: {
        const int storage = d->kind == BD12_DESC_UAV_BUFFER;
        info.type = storage ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        if (!texel_address(device, d, storage, &address)) break;
        if (storage) info.data.pStorageTexelBuffer = &address; else info.data.pUniformTexelBuffer = &address;
        break;
    }
    case BD12_DESC_CBV:
        info.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        if (!d->source_buffer || !d->source_buffer->va || !d->range) break;
        address = (VkDescriptorAddressInfoEXT){
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT,
            .address = d->source_buffer->va + d->offset,
            .range = d->range
        };
        info.data.pUniformBuffer = &address;
        break;
    case BD12_DESC_SRV_AS:           /* a TLAS SRV: `offset` is the structure's GPU address (bd12_raytracing.c) */
        info.type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        info.data.accelerationStructure = d->offset;
        break;
    case BD12_DESC_SAMPLER:
        info.type = VK_DESCRIPTOR_TYPE_SAMPLER;
        if (d->sampler) info.data.pSampler = &d->sampler;
        break;
    default:
        info.type = null_type;
        break;
    }
    const int is_null = !info.data.pSampler;   /* every member of the union is a pointer */
    const uint32_t size = info.type == VK_DESCRIPTOR_TYPE_MAX_ENUM ? 0 : type_size(db, info.type);
    if (!size || size > stride || (is_null && (!db->null_descriptor || info.type == VK_DESCRIPTOR_TYPE_SAMPLER))) {
        memset(out, 0, stride);
        return;
    }
    db->GetDescriptor(device->device, &info, size, out);
    if (size < stride) memset(out + size, 0, stride - size);
}

void bd12_heap_written(const void *first, uint32_t count, VkDescriptorType null_type)
{
    BD12DescriptorHeap *heap = bd12_heap_from_cpu(first);
    if (!heap) { BD12_DESCRIPTORS_WRITTEN(); return; }   /* unknown heap: every resolved set may be stale */
    bd12_heap_note_write(heap);
    const uint32_t index = (uint32_t)((const BD12Descriptor *)first - heap->descriptors);
    if (index + count > heap->count) count = heap->count - index;
    if (heap->payload_stride) {
        for (uint32_t i = 0; i < count; ++i)
            encode(heap->device, &heap->descriptors[index + i], null_type,
                   heap->payload + (size_t)(index + i) * heap->payload_stride, heap->payload_stride);
    }
    if (heap->mirror) bd12_mirror_write(heap, index, count);
}

void bd12_heap_copied(const void *dst, const void *src, uint32_t count)
{
    BD12DescriptorHeap *to = bd12_heap_from_cpu(dst);
    if (!to) { BD12_DESCRIPTORS_WRITTEN(); return; }
    bd12_heap_note_write(to);
    const uint32_t index = (uint32_t)((const BD12Descriptor *)dst - to->descriptors);
    if (index + count > to->count) count = to->count - index;
    if (to->payload_stride) {
        BD12DescriptorHeap *from = bd12_heap_from_cpu(src);
        const uint32_t from_index = from ? (uint32_t)((const BD12Descriptor *)src - from->descriptors) : 0;
        if (from && from->payload_stride == to->payload_stride && from_index + count <= from->count) {
            memmove(to->payload + (size_t)index * to->payload_stride,
                    from->payload + (size_t)from_index * from->payload_stride,
                    (size_t)count * to->payload_stride);
        } else {   /* source without Vulkan descriptors (or another kind of heap): encode from the records */
            for (uint32_t i = 0; i < count; ++i)
                encode(to->device, &to->descriptors[index + i], VK_DESCRIPTOR_TYPE_MAX_ENUM,
                       to->payload + (size_t)(index + i) * to->payload_stride, to->payload_stride);
        }
    }
    if (to->mirror) bd12_mirror_write(to, index, count);
}

/* Layouts --------------------------------------------------------------------------------------------------------- */

static VkDescriptorSetLayout heap_set_layout_flags(BD12Device *device, int samplers, uint32_t count,
                                                   VkDescriptorBindingFlags flags);

/* The heap array (binding 0, the set's last binding) is variable-count: a bound descriptor heap is often smaller than
 * the layout's maximum, and a fixed count would describe descriptors past the end of the bound descriptor buffer
 * (VUID-vkCmdSetDescriptorBufferOffsetsEXT-pOffsets-08063; RE8 crashed in the NVIDIA driver's vkCmdDispatch). */
static VkDescriptorSetLayout heap_set_layout(BD12Device *device, int samplers, uint32_t count)
{
    VkDescriptorSetLayout layout = heap_set_layout_flags(device, samplers, count,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT);
    return layout ? layout : heap_set_layout_flags(device, samplers, count, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);
}

static VkDescriptorSetLayout heap_set_layout_flags(BD12Device *device, int samplers, uint32_t count,
                                                   VkDescriptorBindingFlags flags)
{
    const VkMutableDescriptorTypeListEXT list = { device->db->mutable_count, k_mutable_types };
    const VkMutableDescriptorTypeCreateInfoEXT mutable_info = {
        .sType = VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT,
        .mutableDescriptorTypeListCount = 1,
        .pMutableDescriptorTypeLists = &list
    };
    const VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .pNext = samplers ? NULL : &mutable_info,
        .bindingCount = 1,
        .pBindingFlags = &flags
    };
    const VkDescriptorSetLayoutBinding binding = {
        .binding = 0,
        .descriptorType = samplers ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_MUTABLE_EXT,
        .descriptorCount = count,
        .stageFlags = VK_SHADER_STAGE_ALL
    };
    const VkDescriptorSetLayoutCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = &flags_info,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
        .bindingCount = 1,
        .pBindings = &binding
    };
    PFN_vkGetDescriptorSetLayoutSupport support =
        (PFN_vkGetDescriptorSetLayoutSupport)beer_vulkan_get_device_proc_addr()(device->device,
                                                                                "vkGetDescriptorSetLayoutSupport");
    if (support) {
        VkDescriptorSetLayoutSupport result = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT };
        support(device->device, &info, &result);
        if (!result.supported) return VK_NULL_HANDLE;
    }
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    if (device->vk.CreateDescriptorSetLayout(device->device, &info, NULL, &layout) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return layout;
}

static BD12DbLayout *layout_create(BD12Device *device, const VkSampler *samplers, uint32_t count)
{
    struct BD12DescriptorBuffers *db = device->db;
    BD12DbLayout *entry = calloc(1, sizeof(*entry));
    if (!entry) return NULL;
    entry->static_count = count;
    memcpy(entry->samplers, samplers, count * sizeof(*samplers));
    VkDescriptorSetLayoutBinding bindings[MAX_STATIC];
    for (uint32_t i = 0; i < count; ++i)
        bindings[i] = (VkDescriptorSetLayoutBinding){
            .binding = i,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_ALL,
            .pImmutableSamplers = &entry->samplers[i]
        };
    const VkDescriptorSetLayoutCreateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT |
                 (count ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_EMBEDDED_IMMUTABLE_SAMPLERS_BIT_EXT : 0),
        .bindingCount = count,
        .pBindings = bindings
    };
    if (device->vk.CreateDescriptorSetLayout(device->device, &set_info, NULL, &entry->statics) != VK_SUCCESS) {
        free(entry);
        return NULL;
    }
    const VkDescriptorSetLayout sets[3] = { db->resource_layout, db->sampler_layout, entry->statics };
    const VkPushConstantRange range = { db->push_stages, 0, db->push_size };
    const VkPipelineLayoutCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 3,
        .pSetLayouts = sets,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range
    };
    if (device->vk.CreatePipelineLayout(device->device, &info, NULL, &entry->layout) != VK_SUCCESS) {
        device->vk.DestroyDescriptorSetLayout(device->device, entry->statics, NULL);
        free(entry);
        return NULL;
    }
    return entry;
}

/* The pipeline layout for root signature `rs`. Layouts are shared by every root signature with the same static
 * samplers and live as long as the device. */
const BD12DbLayout *bd12_db_layout(const BD12RootSignature *rs)
{
    BD12Device *device = rs ? rs->device : NULL;
    struct BD12DescriptorBuffers *db = device ? device->db : NULL;
    if (!db) return NULL;
    const BD12DbLayout *cached = __atomic_load_n(&rs->db_layout, __ATOMIC_ACQUIRE);
    if (cached) return cached;
    if (rs->static_count > MAX_STATIC) return NULL;
    VkSampler samplers[MAX_STATIC];
    for (uint32_t i = 0; i < rs->static_count; ++i) {
        samplers[i] = bd12_sampler_get(device, &rs->statics[i].info);
        if (!samplers[i]) return NULL;
    }
    pthread_mutex_lock(&db->lock);
    BD12DbLayout *found = NULL;
    for (size_t i = 0; i < db->layout_count && !found; ++i)
        if (db->layouts[i]->static_count == rs->static_count &&
            !memcmp(db->layouts[i]->samplers, samplers, rs->static_count * sizeof(*samplers)))
            found = db->layouts[i];
    if (!found && (found = layout_create(device, samplers, rs->static_count))) {
        if (db->layout_count == db->layout_capacity) {
            const size_t capacity = db->layout_capacity ? db->layout_capacity * 2 : 32;
            BD12DbLayout **grown = realloc(db->layouts, capacity * sizeof(*grown));
            if (grown) { db->layouts = grown; db->layout_capacity = capacity; }
        }
        if (db->layout_count < db->layout_capacity) db->layouts[db->layout_count++] = found;
        /* else: kept alive by the root signature pointer below; never freed */
    }
    pthread_mutex_unlock(&db->lock);
    /* The root signature's cache of this lookup (the layout itself is immutable). */
    if (found) __atomic_store_n(&((BD12RootSignature *)rs)->db_layout, found, __ATOMIC_RELEASE);
    return found;
}

VkPipelineLayout bd12_db_pipeline_layout(const BD12DbLayout *layout)
{
    return layout ? layout->layout : VK_NULL_HANDLE;
}

uint32_t bd12_db_push_size(const BD12Device *device)
{
    return device && device->db ? device->db->push_size : 0;
}

VkShaderStageFlags bd12_db_push_stages(const BD12Device *device)
{
    return device && device->db ? device->db->push_stages : 0;
}

/* Heap descriptors can be acceleration structures: root-signature shaders read a TLAS straight from the heap. */
int bd12_db_accel_in_heap(const BD12Device *device)
{
    return device && device->db && device->db->mutable_count == T_COUNT;
}

/* Recording ------------------------------------------------------------------------------------------------------- */

void bd12_cmd_invalidate_point(BD12CmdList *list, int which)
{
    list->root_push_key[which] = 0;
    list->db_offsets_valid[which] = 0;
    list->db_static[which] = NULL;
}

void bd12_cmd_invalidate_bindings(BD12CmdList *list)
{
    bd12_cmd_invalidate_point(list, 0);
    bd12_cmd_invalidate_point(list, 1);
    list->db_heaps[0] = list->db_heaps[1] = NULL;
}

/* Before a draw or dispatch, outside rendering: records pending first-use image transitions. 0 when the bound
 * heaps cannot be read through descriptor buffers. */
int bd12_db_prepare(BD12CmdList *list)
{
    struct BD12DescriptorBuffers *db = list->device->db;
    if (!db) return 0;
    const BD12DescriptorHeap *resources = list->heaps[0], *samplers = list->heaps[1];
    if ((resources && !resources->db_buffer) || (samplers && !samplers->db_buffer)) {
        static int warned;
        if (!warned++) fprintf(stderr, "[BD12] bound descriptor heap has no descriptor buffer: draws dropped\n");
        return 0;
    }
    if (__atomic_load_n(&db->pending_count, __ATOMIC_RELAXED)) {
        pthread_mutex_lock(&db->pending_lock);
        BD12Image **pending = db->pending;
        const size_t count = db->pending_count;
        db->pending = NULL;
        db->pending_count = db->pending_capacity = 0;
        pthread_mutex_unlock(&db->pending_lock);
        for (size_t i = 0; i < count; ++i) bd12_image_ensure_initialized(list, pending[i]);
        free(pending);
    }
    return 1;
}

/* Binds the heaps (sets 0 and 1) and `layout`'s static samplers (set 2) where the list's bindings differ. */
void bd12_db_bind(BD12CmdList *list, VkPipelineBindPoint point, const BD12DbLayout *layout)
{
    struct BD12DescriptorBuffers *db = list->device->db;
    const int which = point == VK_PIPELINE_BIND_POINT_COMPUTE;
    const BD12DescriptorHeap *heaps[2] = {
        list->heaps[0] ? list->heaps[0] : db->dummy[0],
        list->heaps[1] ? list->heaps[1] : db->dummy[1]
    };
    if (heaps[0] != list->db_heaps[0] || heaps[1] != list->db_heaps[1]) {
        const VkDescriptorBufferBindingInfoEXT buffers[2] = {
            { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT, .address = heaps[0]->db_address,
              .usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT },
            { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT, .address = heaps[1]->db_address,
              .usage = VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT }
        };
        db->CmdBindBuffers(list->command_buffer, 2, buffers);
        list->db_heaps[0] = heaps[0];
        list->db_heaps[1] = heaps[1];
        list->db_offsets_valid[0] = list->db_offsets_valid[1] = 0;
    }
    if (!list->db_offsets_valid[which]) {
        static const uint32_t indices[2] = { 0, 1 };
        static const VkDeviceSize offsets[2] = { 0, 0 };
        db->CmdSetOffsets(list->command_buffer, point, layout->layout, 0, 2, indices, offsets);
        list->db_offsets_valid[which] = 1;
        list->db_static[which] = NULL;
    }
    if (list->db_static[which] != layout) {
        if (layout->static_count) db->CmdBindEmbedded(list->command_buffer, point, layout->layout, 2);
        list->db_static[which] = layout;
    }
}

/* Device ---------------------------------------------------------------------------------------------------------- */

void bd12_db_init(BD12Device *device)
{
    const char *on = getenv("BEER_BD12_ROOT_PUSH");
    if (!on || !*on || !strcmp(on, "0") || !device->modern->descriptor_buffer) return;
    struct BD12DescriptorBuffers *db = calloc(1, sizeof(*db));
    if (!db) return;
    PFN_vkGetDeviceProcAddr get = beer_vulkan_get_device_proc_addr();
#define LOAD(field, name) db->field = (void *)get(device->device, name)
    LOAD(GetDescriptor, "vkGetDescriptorEXT");
    LOAD(GetLayoutSize, "vkGetDescriptorSetLayoutSizeEXT");
    LOAD(GetBindingOffset, "vkGetDescriptorSetLayoutBindingOffsetEXT");
    LOAD(CmdBindBuffers, "vkCmdBindDescriptorBuffersEXT");
    LOAD(CmdSetOffsets, "vkCmdSetDescriptorBufferOffsetsEXT");
    LOAD(CmdBindEmbedded, "vkCmdBindDescriptorBufferEmbeddedSamplersEXT");
#undef LOAD
    PFN_vkGetPhysicalDeviceProperties2 get_properties2 = (PFN_vkGetPhysicalDeviceProperties2)
        beer_vulkan_get_instance_proc_addr()(device->instance, "vkGetPhysicalDeviceProperties2");
    if (!db->GetDescriptor || !db->GetLayoutSize || !db->GetBindingOffset || !db->CmdBindBuffers ||
        !db->CmdSetOffsets || !db->CmdBindEmbedded || !get_properties2) {
        free(db);
        return;
    }
    VkPhysicalDeviceDescriptorBufferPropertiesEXT props = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT
    };
    VkPhysicalDeviceProperties2 properties = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                               .pNext = &props };
    get_properties2(device->physical_device, &properties);
    const int robust = beer_vulkan_enabled_features()->robustBufferAccess;
    db->type_size[T_SAMPLED] = (uint32_t)props.sampledImageDescriptorSize;
    db->type_size[T_STORAGE_IMAGE] = (uint32_t)props.storageImageDescriptorSize;
    db->type_size[T_UNIFORM_TEXEL] = (uint32_t)(robust ? props.robustUniformTexelBufferDescriptorSize
                                                       : props.uniformTexelBufferDescriptorSize);
    db->type_size[T_STORAGE_TEXEL] = (uint32_t)(robust ? props.robustStorageTexelBufferDescriptorSize
                                                       : props.storageTexelBufferDescriptorSize);
    db->type_size[T_UBO] = (uint32_t)(robust ? props.robustUniformBufferDescriptorSize
                                             : props.uniformBufferDescriptorSize);
    db->type_size[T_SSBO] = (uint32_t)(robust ? props.robustStorageBufferDescriptorSize
                                              : props.storageBufferDescriptorSize);
    db->sampler_size = (uint32_t)props.samplerDescriptorSize;
    db->type_size[T_ACCEL] = (uint32_t)props.accelerationStructureDescriptorSize;
    db->mutable_count = device->rt_enabled && db->type_size[T_ACCEL] ? T_COUNT : T_ACCEL;
    db->offset_alignment = props.descriptorBufferOffsetAlignment;
    db->max_texel_elements = properties.properties.limits.maxTexelBufferElements;
    db->null_descriptor = device->modern->null_descriptor;
    db->push_size = device->max_push_constants < 256 ? device->max_push_constants : 256;
    db->push_stages = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;
    if (device->modern->mesh_shader_tier)          /* mesh pipelines share the one root-argument push range */
        db->push_stages |= VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT;
    pthread_mutex_init(&db->lock, NULL);
    pthread_mutex_init(&db->pending_lock, NULL);
    device->db = db;   /* heap_set_layout and the dummy heaps read it */
    db->resource_layout = heap_set_layout(device, 0, HEAP_SLOTS);
    if (!db->resource_layout && db->mutable_count == T_COUNT) {   /* the driver refuses AS in a mutable list */
        db->mutable_count = T_ACCEL;
        db->resource_layout = heap_set_layout(device, 0, HEAP_SLOTS);
    }
    db->sampler_layout = heap_set_layout(device, 1, SAMPLER_SLOTS);
    if (!db->resource_layout || !db->sampler_layout) goto fail;
    VkDeviceSize resource_size = 0, sampler_size = 0;
    db->GetLayoutSize(device->device, db->resource_layout, &resource_size);
    db->GetLayoutSize(device->device, db->sampler_layout, &sampler_size);
    db->GetBindingOffset(device->device, db->resource_layout, 0, &db->resource_offset);
    db->GetBindingOffset(device->device, db->sampler_layout, 0, &db->sampler_offset);
    /* A mutable descriptor's size is the largest of its types (the binding's stride). */
    db->resource_stride = (uint32_t)((resource_size - db->resource_offset) / HEAP_SLOTS);
    db->sampler_stride = (uint32_t)((sampler_size - db->sampler_offset) / SAMPLER_SLOTS);
    uint32_t largest = 0;
    for (uint32_t i = 0; i < db->mutable_count; ++i) if (db->type_size[i] > largest) largest = db->type_size[i];
    if (!db->resource_stride || db->resource_stride < largest || db->sampler_stride < db->sampler_size) goto fail;
    db->dummy[0] = bd12_descriptor_heap_create_ex(device, 0, 1, BD12_HEAP_FLAG_SHADER_VISIBLE);
    db->dummy[1] = bd12_descriptor_heap_create_ex(device, 1, 1, BD12_HEAP_FLAG_SHADER_VISIBLE);
    if (!db->dummy[0] || !db->dummy[0]->db_buffer || !db->dummy[1] || !db->dummy[1]->db_buffer) goto fail;
    fprintf(stderr, "[BD12] descriptor heaps are descriptor buffers (resource stride %u, sampler stride %u, "
            "null descriptors %d, acceleration structures %d)\n", db->resource_stride, db->sampler_stride,
            db->null_descriptor, db->mutable_count == T_COUNT);
    return;
fail:
    fprintf(stderr, "[BD12] descriptor buffers unusable: root signatures use the descriptor-set heap\n");
    device->db = NULL;
    /* The layouts and any dummy heap are leaked: this runs once at device creation. */
}
