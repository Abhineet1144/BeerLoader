#define _GNU_SOURCE
#include "bd12.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Buffer view cache ------------------------------------------------------- */

#define VIEW_AGE_FRAMES 16
#define VIEW_GC_MIN 4096

static uint64_t view_key(VkFormat format, VkDeviceSize offset, VkDeviceSize range)
{
    uint64_t k = ((uint64_t)format * 0x9E3779B97F4A7C15ull) ^
                 (offset * 0xC2B2AE3D27D4EB4Full) ^
                 (range * 0x165667B19E3779F9ull);
    return k ^ (k >> 29);
}

/* Frees unpinned views not looked up for `age` epochs, compacts the array and
 * rebuilds the hash. Runs with view_lock held. The game binds slices of ring
 * buffers at a new offset every frame, so without this the cache grows without
 * bound and the driver eventually fails buffer-view creation. */
static void view_gc(BD12Buffer *buffer, uint64_t age)
{
    BD12Device *device = buffer->device;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < buffer->view_count; ++i) {
        BD12BufferView v = buffer->views[i];
        if (v.pinned || v.last_use + age >= device->epoch) {
            buffer->views[kept++] = v;
        } else {
            device->vk.DestroyBufferView(device->device, v.view, NULL);
        }
    }
    buffer->view_count = kept;
    if (buffer->view_hash) {
        memset(buffer->view_hash, 0, ((size_t)buffer->view_hash_mask + 1) * sizeof(uint32_t));
        for (uint32_t i = 0; i < kept; ++i) {
            const BD12BufferView *v = &buffer->views[i];
            uint32_t slot = (uint32_t)view_key(v->format, v->offset, v->range) &
                            buffer->view_hash_mask;
            while (buffer->view_hash[slot]) slot = (slot + 1) & buffer->view_hash_mask;
            buffer->view_hash[slot] = i + 1;
        }
    }
    buffer->view_gc_threshold = kept * 2 > VIEW_GC_MIN ? kept * 2 : VIEW_GC_MIN;
}

void bd12_device_advance_epoch(BD12Device *device)
{
    __atomic_add_fetch(&device->epoch, 1, __ATOMIC_RELAXED);
}

static VkBufferView buffer_view_impl(BD12Buffer *buffer, VkFormat format,
                                     VkDeviceSize offset, VkDeviceSize range,
                                     int storage, int pin);

VkBufferView bd12_buffer_view(BD12Buffer *buffer, VkFormat format,
                              VkDeviceSize offset, VkDeviceSize range,
                              int storage)
{
    return buffer_view_impl(buffer, format, offset, range, storage, 0);
}

VkBufferView bd12_buffer_view_pinned(BD12Buffer *buffer, VkFormat format,
                                     VkDeviceSize offset, VkDeviceSize range,
                                     int storage)
{
    return buffer_view_impl(buffer, format, offset, range, storage, 1);
}

static VkBufferView buffer_view_impl(BD12Buffer *buffer, VkFormat format,
                                     VkDeviceSize offset, VkDeviceSize range,
                                     int storage, int pin)
{
    BD12Device *device = buffer->device;
    if (offset >= buffer->size) return VK_NULL_HANDLE;
    if (range == VK_WHOLE_SIZE || offset + range > buffer->size)
        range = buffer->size - offset;
    {   /* A texel-buffer range must be a whole number of texels. */
        uint32_t texel_bytes = 0;
        bd12_format_block(format, VK_IMAGE_ASPECT_COLOR_BIT, NULL, NULL, &texel_bytes);
        if (texel_bytes > 1) range -= range % texel_bytes;
        if (!range) return VK_NULL_HANDLE;
    }
    VkDeviceSize alignment = storage ? device->min_storage_texel_alignment :
                                       device->min_uniform_texel_alignment;
    const int single_texel = storage ? device->storage_texel_single_alignment :
                                       device->uniform_texel_single_alignment;
    if (single_texel) {
        uint32_t block_bytes = 0;
        bd12_format_block(format, VK_IMAGE_ASPECT_COLOR_BIT, NULL, NULL,
                          &block_bytes);
        if (block_bytes && block_bytes < alignment) alignment = block_bytes;
    }
    if (!alignment) {
        static int warned;
        if (!warned++)
            fprintf(stderr, "[BD12] texel buffer alignment is 0 (uniform=%llu storage=%llu device=%p buffer=%p); treating as 1\n",
                    (unsigned long long)device->min_uniform_texel_alignment,
                    (unsigned long long)device->min_storage_texel_alignment, (void *)device, (void *)buffer);
        alignment = 1;
    }
    if (offset % alignment) {
        static unsigned logged_bad;
        if (getenv("BEER_BD12_VIEW_LOG") && logged_bad++ < 40)
            fprintf(stderr, "[BD12 VIEWFAIL] misaligned fmt=%d offset=%llu align=%llu\n",
                    (int)format, (unsigned long long)offset,
                    (unsigned long long)alignment);
        return VK_NULL_HANDLE;
    }
    {   /* Diagnostic: BEER_BD12_VIEW_LOG=1 reports views that exceed a typical
         * maxTexelBufferElements (2^27 texels). */
        static int log_views = -1;
        static unsigned logged;
        if (log_views < 0) log_views = getenv("BEER_BD12_VIEW_LOG") != NULL;
        if (log_views && range > (1ull << 27) && logged++ < 40)
            fprintf(stderr, "[BD12 VIEW] fmt=%d offset=%llu range=%llu bufsize=%llu storage=%d\n",
                    (int)format, (unsigned long long)offset,
                    (unsigned long long)range, (unsigned long long)buffer->size, storage);
    }
    VkBufferView found = VK_NULL_HANDLE;
    pthread_mutex_lock(&device->view_lock);
    /* A single buffer can accumulate thousands of (format, offset, range)
     * views (RE8 binds slices of a few huge buffers), so lookups go through an
     * open-addressed hash instead of scanning the array. */
    uint64_t key = ((uint64_t)format * 0x9E3779B97F4A7C15ull) ^
                   (offset * 0xC2B2AE3D27D4EB4Full) ^
                   (range * 0x165667B19E3779F9ull);
    key ^= key >> 29;
    if (buffer->view_hash) {
        for (uint32_t slot = (uint32_t)key & buffer->view_hash_mask;;
             slot = (slot + 1) & buffer->view_hash_mask) {
            const uint32_t entry = buffer->view_hash[slot];
            if (!entry) break;
            const BD12BufferView *v = &buffer->views[entry - 1];
            if (v->format == format && v->offset == offset && v->range == range) {
                found = v->view;
                buffer->views[entry - 1].last_use = device->epoch;
                if (pin) buffer->views[entry - 1].pinned = 1;
                break;
            }
        }
    }
    if (!found) {
        if (buffer->view_count >= (buffer->view_gc_threshold ? buffer->view_gc_threshold : VIEW_GC_MIN) &&
            device->epoch > VIEW_AGE_FRAMES)
            view_gc(buffer, VIEW_AGE_FRAMES);
        VkBufferViewCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
            .buffer = buffer->buffer,
            .format = format,
            .offset = offset,
            .range = range
        };
        VkResult view_result =
            device->vk.CreateBufferView(device->device, &info, NULL, &found);
        if (view_result != VK_SUCCESS && buffer->view_count && device->epoch > 2) {
            /* Device memory pressure: drop everything idle for a couple of frames and retry. */
            view_gc(buffer, 2);
            view_result = device->vk.CreateBufferView(device->device, &info, NULL, &found);
        }
        {   /* BEER_BD12_VIEW_LOG=1: running count of cached views and failure codes. */
            static unsigned long created, failed;
            static int log_views2 = -1;
            if (log_views2 < 0) log_views2 = getenv("BEER_BD12_VIEW_LOG") != NULL;
            if (view_result == VK_SUCCESS) ++created; else ++failed;
            if (log_views2 && ((view_result == VK_SUCCESS && created % 20000 == 0) ||
                               (view_result != VK_SUCCESS && failed <= 20)))
                fprintf(stderr, "[BD12 VIEWS] created=%lu failed=%lu last=%d fmt=%d offset=%llu range=%llu bufsize=%llu\n",
                        created, failed, (int)view_result, (int)format,
                        (unsigned long long)offset, (unsigned long long)range,
                        (unsigned long long)buffer->size);
        }
        if (view_result == VK_SUCCESS) {
            int stored = 1;
            if (buffer->view_count == buffer->view_capacity) {
                uint32_t capacity = buffer->view_capacity ?
                    buffer->view_capacity * 2 : 4;
                BD12BufferView *grown = realloc(buffer->views,
                                                capacity * sizeof(*grown));
                if (!grown) stored = 0;
                else { buffer->views = grown; buffer->view_capacity = capacity; }
            }
            /* Keep the hash at most half full: rebuild when it would not be. */
            if (stored && (buffer->view_count + 1) * 2 > buffer->view_hash_mask) {
                uint32_t slots = buffer->view_hash_mask ?
                    (buffer->view_hash_mask + 1) * 2 : 16;
                uint32_t *table = calloc(slots, sizeof(*table));
                if (!table) stored = 0;
                else {
                    for (uint32_t i = 0; i < buffer->view_count; ++i) {
                        const BD12BufferView *v = &buffer->views[i];
                        uint64_t k = ((uint64_t)v->format * 0x9E3779B97F4A7C15ull) ^
                                     (v->offset * 0xC2B2AE3D27D4EB4Full) ^
                                     (v->range * 0x165667B19E3779F9ull);
                        k ^= k >> 29;
                        uint32_t slot = (uint32_t)k & (slots - 1);
                        while (table[slot]) slot = (slot + 1) & (slots - 1);
                        table[slot] = i + 1;
                    }
                    free(buffer->view_hash);
                    buffer->view_hash = table;
                    buffer->view_hash_mask = slots - 1;
                }
            }
            if (stored) {
                buffer->views[buffer->view_count] = (BD12BufferView){
                    format, offset, range, found, device->epoch, pin };
                uint32_t slot = (uint32_t)key & buffer->view_hash_mask;
                while (buffer->view_hash[slot])
                    slot = (slot + 1) & buffer->view_hash_mask;
                buffer->view_hash[slot] = ++buffer->view_count;
            } else {
                /* Could not cache it: the view is leaked to the buffer's
                 * lifetime like before is not possible; destroy it and fail. */
                device->vk.DestroyBufferView(device->device, found, NULL);
                found = VK_NULL_HANDLE;
            }
        } else {
            static unsigned logged_fail;
            if (getenv("BEER_BD12_VIEW_LOG") && logged_fail++ < 40)
                fprintf(stderr, "[BD12 VIEWFAIL] create fmt=%d offset=%llu range=%llu\n",
                        (int)format, (unsigned long long)offset,
                        (unsigned long long)range);
            found = VK_NULL_HANDLE;
        }
    }
    pthread_mutex_unlock(&device->view_lock);
    return found;
}

/* Descriptor heaps -------------------------------------------------------------- */

BD12DescriptorHeap *bd12_descriptor_heap_create(BD12Device *device,
                                                uint32_t type, uint32_t count)
{
    static uint64_t next_id;
    if (!count) return NULL;
    BD12DescriptorHeap *heap = calloc(1, sizeof(*heap));
    if (!heap) return NULL;
    heap->descriptors = calloc(count, sizeof(*heap->descriptors));
    if (!heap->descriptors) {
        free(heap);
        return NULL;
    }
    heap->device = device;
    heap->type = type;
    heap->count = count;
    /* Disjoint 2^36-byte windows: a GPU handle identifies its heap. */
    heap->gpu_base = (__atomic_add_fetch(&next_id, 1, __ATOMIC_RELAXED) + 1)
                     << 36;
    if (type == 0) bd12_mirror_heap_register(heap);
    return heap;
}

void bd12_descriptor_heap_destroy(BD12DescriptorHeap *heap)
{
    if (!heap) return;
    if (heap->type == 0) bd12_mirror_heap_unregister(heap);
    free(heap->descriptors);
    free(heap);
}

static int write_texel(BD12DescriptorHeap *heap, uint32_t index,
                       BD12DescriptorKind kind, BD12Buffer *buffer,
                       VkDeviceSize offset, VkDeviceSize size, VkFormat format)
{
    if (index >= heap->count) return 0;
    BD12Descriptor *d = &heap->descriptors[index];
    memset(d, 0, sizeof(*d));
    if (!buffer) return 1;                     /* null descriptor */
    /* No Vulkan view is made here: the game rewrites ring-buffer descriptors every
     * frame, so views are looked up (and aged out) when a draw resolves them. */
    if (offset >= buffer->size) return 0;
    d->format = format;
    d->kind = kind;
    d->buffer = buffer->buffer;
    d->offset = offset;
    d->range = size == VK_WHOLE_SIZE || offset + size > buffer->size ?
               buffer->size - offset : size;
    d->source_buffer = buffer;
    d->buffer_serial = buffer->serial;
    d->buffer_view = VK_NULL_HANDLE;
    return 1;
}

int bd12_descriptor_write_buffer_srv(BD12DescriptorHeap *heap, uint32_t index,
                                     BD12Buffer *buffer, VkDeviceSize offset,
                                     VkDeviceSize size, VkFormat format)
{
    return write_texel(heap, index, BD12_DESC_SRV_BUFFER, buffer, offset, size,
                       format);
}

int bd12_descriptor_write_buffer_uav(BD12DescriptorHeap *heap, uint32_t index,
                                     BD12Buffer *buffer, VkDeviceSize offset,
                                     VkDeviceSize size, VkFormat format)
{
    return write_texel(heap, index, BD12_DESC_UAV_BUFFER, buffer, offset, size,
                       format);
}

int bd12_descriptor_write_cbv(BD12DescriptorHeap *heap, uint32_t index,
                              BD12Buffer *buffer, VkDeviceSize offset,
                              VkDeviceSize size)
{
    if (index >= heap->count) return 0;
    BD12Descriptor *d = &heap->descriptors[index];
    memset(d, 0, sizeof(*d));
    if (!buffer) return 1;
    BD12Device *device = heap->device;
    if (offset % device->min_uniform_alignment || offset >= buffer->size)
        return 0;
    if (size == VK_WHOLE_SIZE || offset + size > buffer->size)
        size = buffer->size - offset;
    if (size > device->max_uniform_range) size = device->max_uniform_range;
    d->kind = BD12_DESC_CBV;
    d->buffer = buffer->buffer;
    d->offset = offset;
    d->range = size;
    d->source_buffer = buffer;
    d->buffer_serial = buffer->serial;
    return 1;
}

void bd12_descriptor_copy(BD12DescriptorHeap *dst, uint32_t dst_index,
                          const BD12DescriptorHeap *src, uint32_t src_index,
                          uint32_t count)
{
    if (dst_index + count > dst->count || src_index + count > src->count)
        return;
    memmove(&dst->descriptors[dst_index], &src->descriptors[src_index],
            count * sizeof(*dst->descriptors));
}

/* Root signatures ------------------------------------------------------------------ */

BD12RootSignature *bd12_root_signature_create(
    BD12Device *device, const BD12RootParameter *parameters, uint32_t count)
{
    if (count > BD12_MAX_ROOT_PARAMETERS) return NULL;
    uint32_t dwords = 0;
    for (uint32_t i = 0; i < count; ++i) {
        switch (parameters[i].type) {
        case BD12_ROOT_CONSTANTS:
            if (parameters[i].dword_count > BD12_MAX_ROOT_CONSTANTS) return NULL;
            dwords += parameters[i].dword_count;
            break;
        case BD12_ROOT_TABLE: dwords += 1; break;
        default: dwords += 2; break;
        }
    }
    if (dwords > 64) return NULL;
    BD12RootSignature *signature = calloc(1, sizeof(*signature));
    if (!signature) return NULL;
    signature->device = device;
    signature->serial = bd12_live_serial();
    signature->parameter_count = count;
    for (uint32_t i = 0; i < count; ++i) {
        BD12RootParameter *p = &signature->parameters[i];
        *p = parameters[i];
        p->ranges = NULL;
        if (p->type == BD12_ROOT_TABLE && parameters[i].range_count) {
            p->ranges = malloc(parameters[i].range_count * sizeof(*p->ranges));
            if (!p->ranges) {
                bd12_root_signature_destroy(signature);
                return NULL;
            }
            memcpy(p->ranges, parameters[i].ranges,
                   parameters[i].range_count * sizeof(*p->ranges));
        }
    }
    return signature;
}

void bd12_root_signature_destroy(BD12RootSignature *signature)
{
    if (!signature) return;
    for (uint32_t i = 0; i < signature->parameter_count; ++i)
        free(signature->parameters[i].ranges);
    free(signature->statics);
    free(signature);
}

int bd12_root_signature_set_static_samplers(BD12RootSignature *signature,
                                            const BD12StaticSampler *samplers,
                                            uint32_t count)
{
    if (!count) return 1;
    BD12StaticSampler *copy = malloc(count * sizeof(*copy));
    if (!copy) return 0;
    memcpy(copy, samplers, count * sizeof(*copy));
    free(signature->statics);
    signature->statics = copy;
    signature->static_count = count;
    return 1;
}
