#define _GNU_SOURCE
#include "bd12.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Bindless descriptor heap mirror.
 *
 * BD12 normally resolves a draw's descriptors into a small set 0 at record
 * time. Shader model 6 titles index unbounded texture arrays that cover most
 * of a 1,000,000-entry CBV/SRV/UAV heap, which cannot be resolved per draw.
 * For those, the heap itself is mirrored into one update-after-bind Vulkan
 * set with four arrays (sampled images, storage images, uniform and storage
 * texel buffers), each indexed by heap slot. Every descriptor write updates
 * the mirror immediately; a shader finds its table through a push-constant
 * word holding the heap index of the array's first element. */

typedef struct BD12HeapMirror {
    VkDescriptorPool pool;
    VkDescriptorSet set;
    pthread_mutex_t lock;
} BD12HeapMirror;

#define MAX_HEAPS 64
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static BD12DescriptorHeap *g_heaps[MAX_HEAPS];
static VkDescriptorSetLayout g_layout;
static int g_layout_failed;

/* Images exposed through the mirror whose first-use layout transition has not
 * been recorded yet. */
static BD12Image **g_pending;
static size_t g_pending_count, g_pending_capacity;

static const VkDescriptorType k_types[4] = {
    VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER
};

VkDescriptorSetLayout bd12_mirror_layout(BD12Device *device)
{
    pthread_mutex_lock(&g_lock);
    if (!g_layout && !g_layout_failed) {
        VkDescriptorSetLayoutBinding bindings[4];
        VkDescriptorBindingFlags flags[4];
        for (uint32_t i = 0; i < 4; ++i) {
            bindings[i] = (VkDescriptorSetLayoutBinding){
                .binding = i,
                .descriptorType = k_types[i],
                .descriptorCount = BD12_MIRROR_COUNT,
                .stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT |
                              (device->rt_enabled ? (VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
                                                     VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR |
                                                     VK_SHADER_STAGE_INTERSECTION_BIT_KHR | VK_SHADER_STAGE_CALLABLE_BIT_KHR) : 0)
            };
            flags[i] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                       VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                       VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
        }
        VkDescriptorSetLayoutBindingFlagsCreateInfo flag_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
            .bindingCount = 4,
            .pBindingFlags = flags
        };
        VkDescriptorSetLayoutCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = &flag_info,
            .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
            .bindingCount = 4,
            .pBindings = bindings
        };
        if (device->vk.CreateDescriptorSetLayout(device->device, &info, NULL,
                                                 &g_layout) != VK_SUCCESS) {
            fprintf(stderr, "[BD12] bindless heap layout creation failed\n");
            g_layout = VK_NULL_HANDLE;
            g_layout_failed = 1;
        }
    }
    VkDescriptorSetLayout layout = g_layout;
    pthread_mutex_unlock(&g_lock);
    return layout;
}

void bd12_mirror_heap_register(BD12DescriptorHeap *heap)
{
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_HEAPS; ++i)
        if (!g_heaps[i]) { g_heaps[i] = heap; break; }
    pthread_mutex_unlock(&g_lock);
}

void bd12_mirror_heap_unregister(BD12DescriptorHeap *heap)
{
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_HEAPS; ++i)
        if (g_heaps[i] == heap) g_heaps[i] = NULL;
    pthread_mutex_unlock(&g_lock);
    BD12HeapMirror *mirror = heap->mirror;
    if (mirror) {
        heap->device->vk.DestroyDescriptorPool(heap->device->device, mirror->pool, NULL);
        pthread_mutex_destroy(&mirror->lock);
        free(mirror);
        heap->mirror = NULL;
    }
}

static void queue_init(BD12Image *image)
{
    if (!image) return;
    if (g_pending_count == g_pending_capacity) {
        size_t capacity = g_pending_capacity ? g_pending_capacity * 2 : 1024;
        BD12Image **grown = realloc(g_pending, capacity * sizeof(*grown));
        if (!grown) return;
        g_pending = grown;
        g_pending_capacity = capacity;
    }
    g_pending[g_pending_count++] = image;
}

/* Writes descriptors [first, first+count) of `heap` into its mirror. */
static void write_range(BD12DescriptorHeap *heap, uint32_t first, uint32_t count)
{
    BD12HeapMirror *mirror = heap->mirror;
    BD12Device *device = heap->device;
    enum { CHUNK = 128 };
    VkWriteDescriptorSet writes[CHUNK];
    VkDescriptorImageInfo images[CHUNK];
    VkBufferView views[CHUNK];
    uint32_t n = 0;
    pthread_mutex_lock(&mirror->lock);
    pthread_mutex_lock(&g_lock);
    for (uint32_t i = first; i < first + count && i < heap->count && i < BD12_MIRROR_COUNT; ++i) {
        const BD12Descriptor *d = &heap->descriptors[i];
        uint32_t binding;
        switch (d->kind) {
        case BD12_DESC_SRV_IMAGE: binding = 0; break;
        case BD12_DESC_UAV_IMAGE: binding = 1; break;
        case BD12_DESC_SRV_BUFFER: binding = 2; break;
        case BD12_DESC_UAV_BUFFER: binding = 3; break;
        default: continue;
        }
        if (!bd12_descriptor_alive(d)) continue;
        VkBufferView buffer_view = d->buffer_view;
        if (binding >= 2 && !buffer_view && d->source_buffer)
            buffer_view = bd12_buffer_view_pinned(d->source_buffer, d->format, d->offset,
                                                  d->range, binding == 3);
        if ((binding < 2 && !d->image_view) || (binding >= 2 && !buffer_view))
            continue;
        if (binding < 2) {
            images[n] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, d->image_view,
                d->image_layout ? d->image_layout : VK_IMAGE_LAYOUT_GENERAL };
            if (d->image) queue_init(d->image);
        } else {
            views[n] = buffer_view;
        }
        writes[n] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = mirror->set,
            .dstBinding = binding,
            .dstArrayElement = i,
            .descriptorCount = 1,
            .descriptorType = k_types[binding],
            .pImageInfo = binding < 2 ? &images[n] : NULL,
            .pTexelBufferView = binding >= 2 ? &views[n] : NULL
        };
        if (++n == CHUNK) {
            device->vk.UpdateDescriptorSets(device->device, n, writes, 0, NULL);
            n = 0;
        }
    }
    if (n) device->vk.UpdateDescriptorSets(device->device, n, writes, 0, NULL);
    pthread_mutex_unlock(&g_lock);
    pthread_mutex_unlock(&mirror->lock);
}

void bd12_mirror_note(const void *first, uint32_t count)
{
    if (!first || !count) return;
    BD12DescriptorHeap *found = NULL;
    uint32_t index = 0;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_HEAPS; ++i) {
        BD12DescriptorHeap *h = g_heaps[i];
        if (!h || !h->mirror) continue;
        const BD12Descriptor *base = h->descriptors;
        if ((const BD12Descriptor *)first >= base &&
            (const BD12Descriptor *)first < base + h->count) {
            found = h;
            index = (uint32_t)((const BD12Descriptor *)first - base);
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    if (found) write_range(found, index, count);
}

static BD12HeapMirror *mirror_ensure(BD12DescriptorHeap *heap)
{
    if (heap->mirror) return heap->mirror;
    BD12Device *device = heap->device;
    VkDescriptorSetLayout layout = bd12_mirror_layout(device);
    if (!layout) return NULL;
    BD12HeapMirror *mirror = calloc(1, sizeof(*mirror));
    if (!mirror) return NULL;
    pthread_mutex_init(&mirror->lock, NULL);
    VkDescriptorPoolSize sizes[4];
    for (uint32_t i = 0; i < 4; ++i)
        sizes[i] = (VkDescriptorPoolSize){ k_types[i], BD12_MIRROR_COUNT };
    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
        .maxSets = 1,
        .poolSizeCount = 4,
        .pPoolSizes = sizes
    };
    VkDescriptorSetAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorSetCount = 1,
        .pSetLayouts = &layout
    };
    if (device->vk.CreateDescriptorPool(device->device, &pool_info, NULL,
                                        &mirror->pool) != VK_SUCCESS)
        goto fail;
    alloc.descriptorPool = mirror->pool;
    if (device->vk.AllocateDescriptorSets(device->device, &alloc, &mirror->set) !=
        VK_SUCCESS) {
        device->vk.DestroyDescriptorPool(device->device, mirror->pool, NULL);
        goto fail;
    }
    /* Descriptors written before the mirror existed are not replayed: heap
     * slots routinely keep handles of destroyed resources, and handing those to
     * the driver crashes it. Mirrors are therefore created with the heap. */
    heap->mirror = mirror;
    fprintf(stderr, "[BD12] descriptor heap mirror created (%u slots)\n",
            heap->count < BD12_MIRROR_COUNT ? heap->count : BD12_MIRROR_COUNT);
    return mirror;
fail:
    fprintf(stderr, "[BD12] descriptor heap mirror creation failed\n");
    pthread_mutex_destroy(&mirror->lock);
    free(mirror);
    return NULL;
}

/* A destroyed image must not stay queued for initialization. */
void bd12_mirror_forget_image(BD12Image *image)
{
    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < g_pending_count; ) {
        if (g_pending[i] == image) g_pending[i] = g_pending[--g_pending_count];
        else ++i;
    }
    pthread_mutex_unlock(&g_lock);
}

void bd12_mirror_enable(BD12DescriptorHeap *heap)
{
    if (heap && heap->type == 0) mirror_ensure(heap);
}

int bd12_mirror_prepare(BD12CmdList *list)
{
    BD12DescriptorHeap *heap = list->heaps[0];
    if (!heap) return 0;
    if (!mirror_ensure(heap)) return 0;
    pthread_mutex_lock(&g_lock);
    BD12Image **pending = g_pending;
    size_t count = g_pending_count;
    g_pending = NULL;
    g_pending_count = g_pending_capacity = 0;
    pthread_mutex_unlock(&g_lock);
    for (size_t i = 0; i < count; ++i)
        bd12_image_ensure_initialized(list, pending[i]);
    free(pending);
    return 1;
}

int bd12_cmd_bind_bindless(BD12CmdList *list, BD12BindState *state,
                           VkPipelineBindPoint point, VkPipelineLayout layout,
                           VkShaderStageFlags stages,
                           const BD12ShaderBinding *bindless, uint32_t count,
                           uint32_t words)
{
    BD12DescriptorHeap *heap = list->heaps[0];
    if (!heap || !heap->mirror) return 0;
    uint32_t values[32] = { 0 };
    if (words > 32) words = 32;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t index;
        if (bindless[i].push_word < words &&
            bd12_bindless_heap_index(list, state, &bindless[i], &index))
            values[bindless[i].push_word] = index;
    }
    BD12Device *device = list->device;
    device->vk.CmdBindDescriptorSets(list->command_buffer, point, layout, 1, 1,
                                     &heap->mirror->set, 0, NULL);
    if (words)
        device->vk.CmdPushConstants(list->command_buffer, layout, stages, 0,
                                    words * 4, values);
    return 1;
}
