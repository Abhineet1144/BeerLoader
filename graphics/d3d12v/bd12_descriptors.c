#define _GNU_SOURCE
#include "bd12.h"

#include <stdlib.h>
#include <string.h>

/* Buffer view cache ------------------------------------------------------- */

VkBufferView bd12_buffer_view(BD12Buffer *buffer, VkFormat format,
                              VkDeviceSize offset, VkDeviceSize range,
                              int storage)
{
    BD12Device *device = buffer->device;
    if (offset >= buffer->size) return VK_NULL_HANDLE;
    if (range == VK_WHOLE_SIZE || offset + range > buffer->size)
        range = buffer->size - offset;
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
    if (offset % alignment) return VK_NULL_HANDLE;
    VkBufferView found = VK_NULL_HANDLE;
    pthread_mutex_lock(&device->view_lock);
    for (uint32_t i = 0; i < buffer->view_count; ++i) {
        const BD12BufferView *v = &buffer->views[i];
        if (v->format == format && v->offset == offset && v->range == range) {
            found = v->view;
            break;
        }
    }
    if (!found) {
        VkBufferViewCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
            .buffer = buffer->buffer,
            .format = format,
            .offset = offset,
            .range = range
        };
        if (device->vk.CreateBufferView(device->device, &info, NULL, &found) ==
            VK_SUCCESS) {
            if (buffer->view_count == buffer->view_capacity) {
                uint32_t capacity = buffer->view_capacity ?
                    buffer->view_capacity * 2 : 4;
                BD12BufferView *grown = realloc(buffer->views,
                                                capacity * sizeof(*grown));
                if (!grown) {
                    device->vk.DestroyBufferView(device->device, found, NULL);
                    found = VK_NULL_HANDLE;
                } else {
                    buffer->views = grown;
                    buffer->view_capacity = capacity;
                }
            }
            if (found)
                buffer->views[buffer->view_count++] = (BD12BufferView){
                    format, offset, range, found };
        } else {
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
    return heap;
}

void bd12_descriptor_heap_destroy(BD12DescriptorHeap *heap)
{
    if (!heap) return;
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
    VkBufferView view = bd12_buffer_view(buffer, format, offset, size,
                                         kind == BD12_DESC_UAV_BUFFER);
    if (!view) return 0;
    d->kind = kind;
    d->buffer = buffer->buffer;
    d->offset = offset;
    d->range = size == VK_WHOLE_SIZE || offset + size > buffer->size ?
               buffer->size - offset : size;
    d->source_buffer = buffer;
    d->buffer_view = view;
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
