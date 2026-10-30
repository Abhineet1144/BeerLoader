#define _GNU_SOURCE
#include "bd12.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* Pipelines ----------------------------------------------------------------- */

BD12ComputePipeline *bd12_compute_pipeline_create(BD12Device *device,
                                                  const uint32_t *spirv,
                                                  size_t spirv_bytes,
                                                  uint32_t push_bytes)
{
    return bd12_compute_pipeline_build(device, spirv, spirv_bytes, push_bytes,
                                       VK_NULL_HANDLE, 0);
}

int bd12_binding_descriptor_type(const BD12ShaderBinding *b,
                                   VkDescriptorType *type)
{
    switch (b->type) {
    case BD12_RANGE_CBV: *type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; return 1;
    case BD12_RANGE_SRV:
        if (b->is_accel) { *type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR; return 1; }
        *type = b->is_buffer ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER :
                               VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        return 1;
    case BD12_RANGE_UAV:
        *type = b->is_buffer ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER :
                               VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        return 1;
    case BD12_RANGE_SAMPLER: *type = VK_DESCRIPTOR_TYPE_SAMPLER; return 1;
    case BD12_RANGE_TEXEL_OFFSETS: *type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; return 1;
    default: return 0;
    }
}

uint32_t *bd12_densify_bindings(const uint32_t *spirv, size_t spirv_bytes,
                                BD12ShaderBinding *bindings, uint32_t count,
                                uint32_t *next)
{
    uint32_t *copy = malloc(spirv_bytes ? spirv_bytes : 4);
    if (!copy) return NULL;
    if (spirv_bytes) memcpy(copy, spirv, spirv_bytes);
    for (uint32_t i = 0; i < count; ++i) {
        bindings[i].source_binding = bindings[i].binding;
        bindings[i].stage = bindings[i].binding >> 16;
        bindings[i].binding = (*next)++;
    }
    const size_t words = spirv_bytes / sizeof(uint32_t);
    for (size_t at = 5; at < words; ) {
        const uint32_t length = copy[at] >> 16;
        if (!length || length > words - at) break;
        /* OpDecorate <target> Binding <value> */
        if ((copy[at] & 0xffffu) == 71u && length >= 4 && copy[at + 2] == 33u) {
            for (uint32_t i = 0; i < count; ++i)
                if (bindings[i].source_binding == copy[at + 3]) {
                    copy[at + 3] = bindings[i].binding;
                    break;
                }
        }
        at += length;
    }
    return copy;
}

BD12ComputePipeline *bd12_compute_pipeline_create_bound(
    BD12Device *device, const uint32_t *spirv, size_t spirv_bytes,
    const BD12ShaderBinding *bindings, uint32_t binding_count)
{
    /* +1: room for the texel offsets table (bd12_texel_offset.c). */
    VkDescriptorSetLayoutBinding *layout_bindings =
        calloc(binding_count + 1, sizeof(*layout_bindings));
    BD12ShaderBinding *copy = calloc(binding_count + 1, sizeof(*copy));
    BD12ShaderBinding *bindless = calloc(binding_count ? binding_count : 1,
                                         sizeof(*bindless));
    uint32_t bindless_count = 0, bindless_words = 0;
    VkDescriptorType *types = calloc(binding_count + 1, sizeof(*types));
    BD12ComputePipeline *pipeline = NULL;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    uint32_t *dense_spirv = NULL;
    if (!layout_bindings || !copy || !types || !bindless) goto done;
    uint32_t next_binding = 0;
    {   /* Heap-array bindings leave set 0 for the heap mirror (set 1). */
        uint32_t kept = 0;
        for (uint32_t i = 0; i < binding_count; ++i) {
            if (bindings[i].bindless) {
                bindless[bindless_count++] = bindings[i];
                if (bindings[i].push_word + 1 > bindless_words)
                    bindless_words = bindings[i].push_word + 1;
            } else {
                copy[kept++] = bindings[i];
            }
        }
        binding_count = kept;
    }
    dense_spirv = bd12_densify_bindings(spirv, spirv_bytes, copy, binding_count,
                                        &next_binding);
    if (!dense_spirv) goto done;
    if (device->texel_offset_fixup) {
        uint32_t slots = 0;
        if (bd12_texel_offsets_apply(&dense_spirv, &spirv_bytes, copy, binding_count, next_binding, &slots))
            copy[binding_count++] = (BD12ShaderBinding){
                .type = BD12_RANGE_TEXEL_OFFSETS, .binding = next_binding++, .count = slots };
    }
    spirv = dense_spirv;
    for (uint32_t i = 0; i < binding_count; ++i) {
        if (!bd12_binding_descriptor_type(&copy[i], &types[i])) goto done;
        if (!copy[i].count) copy[i].count = 1;
        layout_bindings[i] = (VkDescriptorSetLayoutBinding){
            .binding = copy[i].binding,
            .descriptorType = types[i],
            .descriptorCount = copy[i].count,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
        };
    }
    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = binding_count,
        .pBindings = layout_bindings
    };
    if (device->vk.CreateDescriptorSetLayout(device->device, &layout_info, NULL,
                                             &set_layout) != VK_SUCCESS)
        goto done;
    pipeline = bd12_compute_pipeline_build(device, spirv, spirv_bytes, 0,
                                           set_layout, bindless_words);
    if (!pipeline) {
        device->vk.DestroyDescriptorSetLayout(device->device, set_layout, NULL);
        goto done;
    }
    pipeline->bindings = copy;
    pipeline->binding_types = types;
    pipeline->binding_count = binding_count;
    pipeline->bindless = bindless;
    pipeline->bindless_count = bindless_count;
    pipeline->bindless_words = bindless_words;
    copy = NULL;
    types = NULL;
    bindless = NULL;
done:
    free(bindless);
    free(dense_spirv);
    free(layout_bindings);
    free(copy);
    free(types);
    return pipeline;
}

/* Per-allocator scratch (constants) and descriptor sets ------------------------ */

#define BD12_SCRATCH_CHUNK (1u << 20)
#define BD12_POOL_SETS 1024u
#define BD12_POOL_DESCRIPTORS 16384u

static int scratch_alloc(BD12CmdAllocator *a, VkDeviceSize size,
                         BD12Buffer **buffer, VkDeviceSize *offset, void **ptr)
{
    BD12Device *device = a->device;
    const VkDeviceSize align = device->min_uniform_alignment ?
        device->min_uniform_alignment : 256;
    for (;;) {
        if (a->scratch_current < a->scratch_count) {
            BD12Buffer *chunk = a->scratch[a->scratch_current];
            VkDeviceSize at = (a->scratch_offset + align - 1) & ~(align - 1);
            if (at + size <= chunk->size) {
                *buffer = chunk;
                *offset = at;
                *ptr = (char *)chunk->cpu + at;
                a->scratch_offset = at + size;
                return 1;
            }
            ++a->scratch_current;
            a->scratch_offset = 0;
            continue;
        }
        if (a->scratch_count == a->scratch_capacity) {
            uint32_t capacity = a->scratch_capacity ? a->scratch_capacity * 2 : 4;
            BD12Buffer **grown = realloc(a->scratch, capacity * sizeof(*grown));
            if (!grown) return 0;
            a->scratch = grown;
            a->scratch_capacity = capacity;
        }
        VkDeviceSize chunk_size = size > BD12_SCRATCH_CHUNK ? size :
                                  BD12_SCRATCH_CHUNK;
        BD12Buffer *chunk = bd12_buffer_create_committed(device, BD12_HEAP_UPLOAD,
                                                         chunk_size);
        if (!chunk) return 0;
        a->scratch[a->scratch_count++] = chunk;
    }
}

int bd12_scratch_alloc(BD12CmdAllocator *a, VkDeviceSize size,
                       BD12Buffer **buffer, VkDeviceSize *offset, void **ptr)
{
    return scratch_alloc(a, size, buffer, offset, ptr);
}

static VkDescriptorSet allocate_set(BD12CmdAllocator *a,
                                    VkDescriptorSetLayout layout)
{
    BD12Device *device = a->device;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (a->descriptor_pool_current < a->descriptor_pool_count) {
            VkDescriptorSetAllocateInfo info = {
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = a->descriptor_pools[a->descriptor_pool_current],
                .descriptorSetCount = 1,
                .pSetLayouts = &layout
            };
            VkDescriptorSet set;
            VkResult result = device->vk.AllocateDescriptorSets(device->device,
                                                                &info, &set);
            if (result == VK_SUCCESS) return set;
            if (result != VK_ERROR_OUT_OF_POOL_MEMORY &&
                result != VK_ERROR_FRAGMENTED_POOL) {
                beer_vulkan_record_result(result, "BD12 descriptor set");
                return VK_NULL_HANDLE;
            }
            ++a->descriptor_pool_current;
        }
        if (a->descriptor_pool_current < a->descriptor_pool_count) {
            --attempt;                       /* try the next recycled pool */
            continue;
        }
        if (a->descriptor_pool_count == a->descriptor_pool_capacity) {
            uint32_t capacity = a->descriptor_pool_capacity ?
                a->descriptor_pool_capacity * 2 : 4;
            VkDescriptorPool *grown = realloc(a->descriptor_pools,
                                              capacity * sizeof(*grown));
            if (!grown) return VK_NULL_HANDLE;
            a->descriptor_pools = grown;
            a->descriptor_pool_capacity = capacity;
        }
        const VkDescriptorType kinds[] = {
            VK_DESCRIPTOR_TYPE_SAMPLER,
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
        };
        VkDescriptorPoolSize sizes[8];
        uint32_t size_count = 0;
        for (int i = 0; i < 6; ++i)
            sizes[size_count++] = (VkDescriptorPoolSize){ kinds[i], BD12_POOL_DESCRIPTORS };
        if (device->rt_enabled)
            sizes[size_count++] = (VkDescriptorPoolSize){ VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 256 };
        if (device->texel_offset_fixup)   /* one offsets table per set (bd12_texel_offset.c) */
            sizes[size_count++] = (VkDescriptorPoolSize){ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, BD12_POOL_SETS };
        VkDescriptorPoolCreateInfo pool_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = BD12_POOL_SETS,
            .poolSizeCount = size_count,
            .pPoolSizes = sizes
        };
        VkDescriptorPool pool;
        if (device->vk.CreateDescriptorPool(device->device, &pool_info, NULL,
                                            &pool) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        a->descriptor_pools[a->descriptor_pool_count] = pool;
        a->descriptor_pool_current = a->descriptor_pool_count++;
    }
    return VK_NULL_HANDLE;
}

/* State setters ------------------------------------------------------------------- */

void bd12_cmd_set_descriptor_heaps(BD12CmdList *list,
                                   BD12DescriptorHeap *cbv_srv_uav,
                                   BD12DescriptorHeap *sampler)
{
    list->heaps[0] = cbv_srv_uav;
    list->heaps[1] = sampler;
}

void bd12_cmd_set_compute_root_signature(BD12CmdList *list,
                                         BD12RootSignature *signature)
{
    if (list->compute.root_signature == signature) return;
    memset(list->compute.arguments, 0, sizeof(list->compute.arguments));
    list->compute.root_signature = signature;
}

void bd12_cmd_set_pipeline(BD12CmdList *list, BD12ComputePipeline *pipeline)
{
    list->compute_pipeline = pipeline;
}

void bd12_cmd_set_compute_root_constants(BD12CmdList *list, uint32_t parameter,
                                         const uint32_t *data, uint32_t count,
                                         uint32_t dest_offset)
{
    if (parameter >= BD12_MAX_ROOT_PARAMETERS ||
        dest_offset + count > BD12_MAX_ROOT_CONSTANTS)
        return;
    BD12RootArgument *arg = &list->compute.arguments[parameter];
    memcpy(&arg->constants[dest_offset], data, count * sizeof(uint32_t));
    arg->cbv_valid = 0;
}

void bd12_cmd_set_compute_root_va(BD12CmdList *list, uint32_t parameter,
                                  uint64_t gpu_va)
{
    if (parameter < BD12_MAX_ROOT_PARAMETERS)
        list->compute.arguments[parameter].value = gpu_va;
}

void bd12_cmd_set_compute_root_table(BD12CmdList *list, uint32_t parameter,
                                     uint64_t gpu_handle)
{
    if (parameter < BD12_MAX_ROOT_PARAMETERS)
        list->compute.arguments[parameter].value = gpu_handle;
}

/* Record-time descriptor resolution ------------------------------------------------- */

typedef struct {
    uint32_t kind;                /* BD12DescriptorKind, NONE = null */
    VkBuffer buffer;
    VkDeviceSize offset, range;
    BD12Buffer *source_buffer;
    VkBufferView view;
    VkImageView image_view;
    VkImageLayout image_layout;
    VkSampler sampler;
    BD12Image *image;
    uint32_t view_mip, view_base_layer, view_layers;
    uint64_t as_va;               /* BD12_DESC_SRV_AS */
    uint32_t delta;               /* texel_offset_fixup: elements the view starts before the D3D offset */
} Resolved;

static const BD12Descriptor *table_descriptor(BD12CmdList *list,
                                              const BD12RootParameter *p,
                                              uint64_t handle, uint32_t type,
                                              uint32_t space, uint32_t reg)
{
    BD12DescriptorHeap *heap = list->heaps[type == BD12_RANGE_SAMPLER ? 1 : 0];
    if (!heap || handle < heap->gpu_base ||
        handle >= heap->gpu_base + (uint64_t)heap->count * BD12_DESCRIPTOR_INCREMENT)
        return NULL;
    const uint32_t base = (uint32_t)((handle - heap->gpu_base) /
                                     BD12_DESCRIPTOR_INCREMENT);
    uint32_t running = 0;
    for (uint32_t r = 0; r < p->range_count; ++r) {
        const BD12DescriptorRange *range = &p->ranges[r];
        const uint32_t offset = range->offset == BD12_RANGE_APPEND ? running :
                                range->offset;
        running = offset + (range->count == UINT32_MAX ? 0 : range->count);
        if (range->type != type || range->space != space) continue;
        if (reg < range->base_register) continue;
        const uint32_t rel = reg - range->base_register;
        if (range->count != UINT32_MAX && rel >= range->count) continue;
        const uint32_t index = base + offset + rel;
        return index < heap->count ? &heap->descriptors[index] : NULL;
    }
    return NULL;
}

/* D3D12_SHADER_VISIBILITY of a root parameter vs. the shader stage encoded in a
 * translator stage namespace (BD12ShaderBinding.stage, see d3d12_shader_translator.c:
 * 0 compute, 1 vertex, 2 pixel, 5 geometry, 6 hull, 7 domain). The same
 * register (b1, t0, ...) can name different resources per stage, so a
 * stage-specific parameter must only satisfy bindings of its own stage. */
static int visibility_matches(uint32_t visibility, uint32_t stage)
{
    if (visibility == 0 || stage == 0) return 1;      /* ALL, or compute */
    switch (stage) {
    case 1: return visibility == 1;                    /* VERTEX */
    case 2: return visibility == 5;                    /* PIXEL */
    case 5: return visibility == 4;                    /* GEOMETRY */
    case 6: return visibility == 2;                    /* HULL */
    case 7: return visibility == 3;                    /* DOMAIN */
    default: return 1;
    }
}

/* Candidate root parameters for a single-register binding: the same static tests resolve_one applies, in order. */
static void build_candidates(BD12ShaderBinding *b, const BD12RootSignature *rs)
{
    uint32_t found[12], n = 0, overflow = 0;
    for (uint32_t i = 0; i < rs->parameter_count; ++i) {
        const BD12RootParameter *p = &rs->parameters[i];
        if (!visibility_matches(p->visibility, b->stage)) continue;
        int match = 0;
        switch (p->type) {
        case BD12_ROOT_CONSTANTS:
            match = b->type == BD12_RANGE_CBV && p->shader_register == b->reg && p->space == b->space;
            break;
        case BD12_ROOT_CBV: case BD12_ROOT_SRV: case BD12_ROOT_UAV: {
            const uint32_t want = p->type == BD12_ROOT_CBV ? BD12_RANGE_CBV :
                                  p->type == BD12_ROOT_SRV ? BD12_RANGE_SRV : BD12_RANGE_UAV;
            match = b->type == want && p->shader_register == b->reg && p->space == b->space;
            break;
        }
        case BD12_ROOT_TABLE:
            for (uint32_t r = 0; r < p->range_count && !match; ++r) {
                const BD12DescriptorRange *range = &p->ranges[r];
                if (range->type != b->type || range->space != b->space || b->reg < range->base_register) continue;
                if (range->count != UINT32_MAX && b->reg - range->base_register >= range->count) continue;
                match = 1;
            }
            break;
        default: match = 1; break;
        }
        if (!match) continue;
        if (n == 12) { overflow = 1; break; }
        found[n++] = i;
    }
    b->cand_count = overflow ? 0xff : n;
    for (uint32_t i = 0; i < n; ++i) b->cand[i] = (uint8_t)found[i];
    __atomic_store_n(&b->cand_serial, rs->serial, __ATOMIC_RELEASE);
}

static int resolve_one(BD12CmdList *list, BD12BindState *state,
                       const BD12ShaderBinding *b, uint32_t reg, Resolved *out)
{
    BD12Device *device = list->device;
    const BD12RootSignature *rs = state->root_signature;
    memset(out, 0, sizeof(*out));
    if (b->type == BD12_RANGE_SAMPLER) {
        for (uint32_t i = 0; i < rs->static_count; ++i) {
            if (rs->statics[i].reg == reg && rs->statics[i].space == b->space) {
                out->kind = BD12_DESC_SAMPLER;
                out->sampler = bd12_sampler_get(device, &rs->statics[i].info);
                return 1;
            }
        }
    }
    /* Only parameters that can match this slot matter; work them out once per (binding, root signature). */
    uint32_t scan = rs->parameter_count;
    const uint8_t *cands = NULL;
    if (b->count == 1) {
        if (__atomic_load_n(&b->cand_serial, __ATOMIC_ACQUIRE) != rs->serial)
            build_candidates((BD12ShaderBinding *)b, rs);
        if (b->cand_serial == rs->serial && b->cand_count != 0xff) {
            scan = b->cand_count;
            cands = b->cand;
        }
    }
    for (uint32_t ci = 0; ci < scan; ++ci) {
        const uint32_t i = cands ? cands[ci] : ci;
        const BD12RootParameter *p = &rs->parameters[i];
        BD12RootArgument *arg = &state->arguments[i];
        if (!visibility_matches(p->visibility, b->stage)) continue;
        switch (p->type) {
        case BD12_ROOT_CONSTANTS:
            if (b->type != BD12_RANGE_CBV || p->shader_register != reg ||
                p->space != b->space)
                break;
            if (!arg->cbv_valid) {
                BD12Buffer *buffer; VkDeviceSize offset; void *ptr;
                if (!scratch_alloc(list->allocator, p->dword_count * 4u, &buffer,
                                   &offset, &ptr))
                    return 0;
                memcpy(ptr, arg->constants, p->dword_count * 4u);
                arg->cbv_buffer = buffer->buffer;
                arg->cbv_offset = offset;
                arg->cbv_valid = 1;
            }
            out->kind = BD12_DESC_CBV;
            out->buffer = arg->cbv_buffer;
            out->offset = arg->cbv_offset;
            out->range = p->dword_count * 4u;
            /* Root constants are already copied to allocator upload scratch;
             * retain the CPU words separately in the diagnostic capture. */
            return 1;
        case BD12_ROOT_CBV:
        case BD12_ROOT_SRV:
        case BD12_ROOT_UAV: {
            const uint32_t want = p->type == BD12_ROOT_CBV ? BD12_RANGE_CBV :
                                  p->type == BD12_ROOT_SRV ? BD12_RANGE_SRV :
                                  BD12_RANGE_UAV;
            if (b->type != want || p->shader_register != reg ||
                p->space != b->space)
                break;
            if (want == BD12_RANGE_SRV && b->is_accel) {   /* root SRV holding an acceleration structure address */
                out->kind = BD12_DESC_SRV_AS;
                out->as_va = arg->value;
                return 1;
            }
            VkDeviceSize offset = 0;
            BD12Buffer *buffer = arg->value ?
                bd12_va_resolve(device, arg->value, &offset) : NULL;
            if (!buffer) return 1;               /* null descriptor */
            VkDeviceSize range = buffer->size - offset;
            if (want == BD12_RANGE_CBV) {
                if (offset % device->min_uniform_alignment) return 0;
                if (range > device->max_uniform_range)
                    range = device->max_uniform_range;
                out->kind = BD12_DESC_CBV;
                out->buffer = buffer->buffer;
                out->offset = offset;
                out->range = range;
                out->source_buffer = buffer;
                return 1;
            }
            if (!b->is_buffer) return 1;
            out->source_buffer = buffer;
            out->buffer = buffer->buffer;
            out->offset = offset;
            out->range = range;
            out->view = b->offset_slot ?
                bd12_buffer_view_offset(buffer, VK_FORMAT_R32_UINT, offset, range & ~(VkDeviceSize)3,
                                        want == BD12_RANGE_UAV, &out->delta) :
                bd12_buffer_view(buffer, VK_FORMAT_R32_UINT, offset,
                                 range & ~(VkDeviceSize)3,
                                 want == BD12_RANGE_UAV);
            if (!out->view) return 0;
            out->kind = want == BD12_RANGE_SRV ? BD12_DESC_SRV_BUFFER :
                                                 BD12_DESC_UAV_BUFFER;
            return 1;
        }
        case BD12_ROOT_TABLE: {
            const BD12Descriptor *d = table_descriptor(
                list, p, arg->value, b->type, b->space, reg);
            if (!d) break;
            out->kind = d->kind;
            if (d->kind == BD12_DESC_SRV_AS) { out->as_va = d->offset; return 1; }
            /* A descriptor left behind by a destroyed resource acts as a null descriptor. */
            if (!bd12_descriptor_alive(d)) {
                memset(out, 0, sizeof(*out));
                return 1;
            }
            out->buffer = d->buffer;
            out->offset = d->offset;
            out->range = d->range;
            out->source_buffer = d->source_buffer;
            out->view = d->buffer_view;
            if ((d->kind == BD12_DESC_SRV_BUFFER || d->kind == BD12_DESC_UAV_BUFFER) &&
                d->source_buffer && !d->buffer_view) {
                out->view = b->offset_slot ?
                    bd12_buffer_view_offset(d->source_buffer, d->format, d->offset, d->range,
                                            d->kind == BD12_DESC_UAV_BUFFER, &out->delta) :
                    bd12_buffer_view(d->source_buffer, d->format, d->offset,
                                     d->range, d->kind == BD12_DESC_UAV_BUFFER);
                if (!out->view) {          /* invalid range/alignment: act as a null descriptor */
                    memset(out, 0, sizeof(*out));
                    return 1;
                }
            }
            out->image_view = d->image_view;
            out->image_layout = d->image_layout;
            out->sampler = d->sampler;
            out->image = d->image;
            out->view_mip = d->view_mip;
            out->view_base_layer = d->view_base_layer;
            out->view_layers = d->view_layers;
            return 1;
        }
        }
    }
    return 1;                                    /* unresolved: null */
}

/* Builds the set-0 descriptor set for one draw/dispatch from the root
 * signature arguments and descriptor heaps bound in state. */
static VkDescriptorSet resolve_set_impl(
    BD12CmdList *list, BD12BindState *state, const BD12ShaderBinding *bindings,
    const VkDescriptorType *types, uint32_t binding_count,
    VkDescriptorSetLayout set_layout)
{
    if (!set_layout || !state->root_signature) return VK_NULL_HANDLE;
    BD12Device *device = list->device;
    BD12_PROF_BEGIN();
    VkDescriptorSet set = allocate_set(list->allocator, set_layout);
    BD12_PROF_END(BD12_PROF_ALLOC_SET);
    if (!set) return VK_NULL_HANDLE;

    enum { MAX_WRITES = 64, MAX_INFOS = 256 };
    VkWriteDescriptorSet writes[MAX_WRITES];
    VkDescriptorBufferInfo buffer_infos[MAX_INFOS];
    VkBufferView views[MAX_INFOS];
    VkDescriptorImageInfo image_infos[MAX_INFOS];
    VkAccelerationStructureKHR as_handles[MAX_INFOS];
    VkWriteDescriptorSetAccelerationStructureKHR as_writes[MAX_WRITES];
    uint32_t deltas[MAX_INFOS];   /* texel_offset_fixup: per offsets-table slot */
    uint32_t write_count = 0, info_count = 0;
    for (uint32_t i = 0; i < binding_count; ++i) {
        const BD12ShaderBinding *b = &bindings[i];
        const VkDescriptorType type = types[i];
        if (b->type == BD12_RANGE_TEXEL_OFFSETS) {   /* always last: every slot is resolved by now */
            if (write_count == MAX_WRITES || info_count == MAX_INFOS) {
                device->vk.UpdateDescriptorSets(device->device, write_count, writes, 0, NULL);
                write_count = 0;
                info_count = 0;
            }
            const VkDeviceSize bytes = (VkDeviceSize)(b->count ? b->count : 1) * 4;
            BD12Buffer *scratch = NULL;
            VkDeviceSize at = 0;
            void *cpu = NULL;
            if (b->count > MAX_INFOS || !scratch_alloc(list->allocator, bytes, &scratch, &at, &cpu))
                return VK_NULL_HANDLE;
            memcpy(cpu, deltas, bytes);
            buffer_infos[info_count] = (VkDescriptorBufferInfo){ scratch->buffer, at, bytes };
            writes[write_count++] = (VkWriteDescriptorSet){
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = set,
                .dstBinding = b->binding,
                .descriptorCount = 1,
                .descriptorType = type,
                .pBufferInfo = &buffer_infos[info_count++]
            };
            continue;
        }
        if (b->count > MAX_INFOS) return VK_NULL_HANDLE;
        if (write_count == MAX_WRITES || info_count + b->count > MAX_INFOS) {
            device->vk.UpdateDescriptorSets(device->device, write_count, writes,
                                            0, NULL);
            write_count = 0;
            info_count = 0;
        }
        VkWriteDescriptorSet *w = &writes[write_count++];
        *w = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set,
            .dstBinding = b->binding,
            .descriptorCount = b->count,
            .descriptorType = type
        };
        for (uint32_t e = 0; e < b->count; ++e) {
            Resolved r;
            if (!resolve_one(list, state, b, b->reg + e, &r)) return VK_NULL_HANDLE;
            if (b->offset_slot && b->offset_slot - 1 + e < MAX_INFOS) deltas[b->offset_slot - 1 + e] = r.delta;
            if (state == &list->graphics && bd12_trace_active() && getenv("BEER_BD12_BIND_LOG") &&
                list->gfx.pipeline && r.kind == BD12_DESC_UAV_BUFFER && r.source_buffer)
                fprintf(stderr, "[BINDG] pso=%016llx u%u va=%llx range=%llu\n",
                        (unsigned long long)list->gfx.pipeline->shader_hash, b->reg + e,
                        (unsigned long long)(r.source_buffer->va + r.offset),
                        (unsigned long long)r.range);
            if (state == &list->compute && bd12_trace_active() && getenv("BEER_BD12_BIND_LOG") &&
                list->compute_pipeline &&
                (r.kind == BD12_DESC_SRV_BUFFER || r.kind == BD12_DESC_UAV_BUFFER) &&
                r.source_buffer)
                fprintf(stderr, "[BIND] cs=%016llx %c%u va=%llx range=%llu\n",
                        (unsigned long long)list->compute_pipeline->shader_hash,
                        r.kind == BD12_DESC_SRV_BUFFER ? 't' : 'u', b->reg + e,
                        (unsigned long long)(r.source_buffer->va + r.offset),
                        (unsigned long long)r.range);
            if (state == &list->compute && r.kind == BD12_DESC_SRV_IMAGE && r.image &&
                list->compute_pipeline && bd12_trace_active() && getenv("BEER_BD12_CAPTURE_SRV_DEPTH"))
                bd12_capture_srv_depth(list, r.image, r.view_mip, r.view_base_layer,
                                       r.view_layers, list->compute_pipeline->shader_hash,
                                       b->reg + e);
            if (__atomic_load_n(&g_bd12_cbt_active, __ATOMIC_RELAXED) && r.kind == BD12_DESC_CBV && r.source_buffer) {
                const uint64_t pso = state == &list->graphics ? (list->gfx.pipeline ? list->gfx.pipeline->shader_hash : 0)
                                                              : (list->compute_pipeline ? list->compute_pipeline->shader_hash : 0);
                bd12_cbt_observe(pso, b->stage, b->space, b->reg + e, r.source_buffer, r.offset, r.range);
            }
            if (bd12_capture_enabled() && getenv("BEER_BD12_BIND_LOG"))
                fprintf(stderr, "[BIND] type=%u space=%u reg=%u+%u binding=%u bindless=%u accel=%u -> kind=%u buf=%p img=%p\n",
                        b->type, b->space, b->reg, e, b->binding, b->bindless, b->is_accel, r.kind, (void *)(uintptr_t)r.buffer, (void *)r.image);
            if (bd12_capture_enabled() && r.kind == BD12_DESC_UAV_IMAGE && r.image)
                bd12_capture_note_image(b->reg + e, b->space, r.image, r.view_mip);
            if (bd12_capture_enabled()) {
                if (r.kind == BD12_DESC_CBV)
                    bd12_capture_note(0, b->space, b->reg + e, r.buffer, r.offset, r.range);
                else if ((r.kind == BD12_DESC_SRV_BUFFER || r.kind == BD12_DESC_UAV_BUFFER) &&
                         r.source_buffer)
                    bd12_capture_note(r.kind == BD12_DESC_SRV_BUFFER ? 1 : 2, b->space,
                                      b->reg + e, r.source_buffer->buffer, r.offset, r.range);
            }
            if (type == VK_DESCRIPTOR_TYPE_SAMPLER) {
                VkSampler sampler = r.sampler;
                if (!sampler) {
                    const VkSamplerCreateInfo fallback = {
                        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                        .magFilter = VK_FILTER_NEAREST,
                        .minFilter = VK_FILTER_NEAREST,
                        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                        .maxLod = VK_LOD_CLAMP_NONE
                    };
                    sampler = bd12_sampler_get(device, &fallback);
                    if (!sampler) return VK_NULL_HANDLE;
                }
                image_infos[info_count + e] =
                    (VkDescriptorImageInfo){ sampler, VK_NULL_HANDLE,
                                             VK_IMAGE_LAYOUT_UNDEFINED };
            } else if (type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
                       type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
                if (r.image) bd12_image_ensure_initialized(list, r.image);
                image_infos[info_count + e] =
                    (VkDescriptorImageInfo){ VK_NULL_HANDLE, r.image_view,
                                             r.image_view ? r.image_layout :
                                             VK_IMAGE_LAYOUT_UNDEFINED };
            } else if (type == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) {
                as_handles[info_count + e] = r.kind == BD12_DESC_SRV_AS && r.as_va ?
                    bd12_rt_as_from_va(device, r.as_va) : VK_NULL_HANDLE;
            } else if (type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                buffer_infos[info_count + e] = (VkDescriptorBufferInfo){
                    r.kind == BD12_DESC_CBV ? r.buffer : VK_NULL_HANDLE,
                    r.offset, r.kind == BD12_DESC_CBV ? r.range : VK_WHOLE_SIZE
                };
            } else {
                views[info_count + e] = r.view;
            }
        }
        if (type == VK_DESCRIPTOR_TYPE_SAMPLER ||
            type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
            type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            w->pImageInfo = &image_infos[info_count];
        else if (type == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) {
            VkWriteDescriptorSetAccelerationStructureKHR *aw = &as_writes[write_count - 1];
            *aw = (VkWriteDescriptorSetAccelerationStructureKHR){
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
                .accelerationStructureCount = b->count,
                .pAccelerationStructures = &as_handles[info_count]
            };
            w->pNext = aw;
        } else if (type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
            w->pBufferInfo = &buffer_infos[info_count];
        else
            w->pTexelBufferView = &views[info_count];
        info_count += b->count;
    }
    if (write_count) {
        const uint64_t t_update = bd12_prof_now();
        device->vk.UpdateDescriptorSets(device->device, write_count, writes, 0,
                                        NULL);
        g_bd12_prof_ns[BD12_PROF_UPDATE_SETS] += bd12_prof_now() - t_update;
        ++g_bd12_prof_calls[BD12_PROF_UPDATE_SETS];
    }
    return set;
}

static void record_dispatch(BD12CmdList *list, uint32_t x, uint32_t y,
                            uint32_t z, int indirect)
{
    BD12ComputePipeline *pso = list->compute_pipeline;
    const uint64_t sequence = list->dispatch_count++;
    BD12DispatchFlight *entry =
        &list->dispatch_flight[sequence % BD12_FLIGHT_DISPATCHES];
    bd12_ckpt_mark(list, 100u + (indirect != 0), pso ? pso->shader_hash : 0, 0, x, y, z);
    *entry = (BD12DispatchFlight){
        .shader_hash = pso ? pso->shader_hash : 0,
        .x = x, .y = y, .z = z,
        .loop_count = pso ? pso->loop_count : 0,
        .control_barrier_count = pso ? pso->control_barrier_count : 0,
        .segment = list->command_buffer_count - 1,
        .indirect = indirect != 0,
        .writes_3d_storage_image = pso && pso->writes_3d_storage_image
    };
}

/* Heap index of the first element of a bindless array: the root table whose
 * range holds register b->reg. */
int bd12_bindless_heap_index(BD12CmdList *list, BD12BindState *state,
                             const BD12ShaderBinding *b, uint32_t *index)
{
    BD12DescriptorHeap *heap = list->heaps[b->type == BD12_RANGE_SAMPLER ? 1 : 0];
    const BD12RootSignature *rs = state->root_signature;
    if (!heap || !rs) return 0;
    for (uint32_t i = 0; i < rs->parameter_count; ++i) {
        const BD12RootParameter *p = &rs->parameters[i];
        if (p->type != BD12_ROOT_TABLE || !visibility_matches(p->visibility, b->stage))
            continue;
        const uint64_t handle = state->arguments[i].value;
        if (handle < heap->gpu_base ||
            handle >= heap->gpu_base + (uint64_t)heap->count * BD12_DESCRIPTOR_INCREMENT)
            continue;
        const uint32_t base = (uint32_t)((handle - heap->gpu_base) /
                                         BD12_DESCRIPTOR_INCREMENT);
        uint32_t running = 0;
        for (uint32_t r = 0; r < p->range_count; ++r) {
            const BD12DescriptorRange *range = &p->ranges[r];
            const uint32_t offset = range->offset == BD12_RANGE_APPEND ? running :
                                    range->offset;
            running = offset + (range->count == UINT32_MAX ? 0 : range->count);
            if (range->type != b->type || range->space != b->space) continue;
            if (b->reg < range->base_register) continue;
            const uint32_t rel = b->reg - range->base_register;
            if (range->count != UINT32_MAX && rel >= range->count) continue;
            *index = base + offset + rel;
            return 1;
        }
    }
    return 0;
}

VkDescriptorSet bd12_allocate_descriptor_set(BD12CmdAllocator *allocator,
                                             VkDescriptorSetLayout layout)
{
    return allocate_set(allocator, layout);
}

VkDescriptorSet bd12_resolve_descriptor_set(
    BD12CmdList *list, BD12BindState *state, const BD12ShaderBinding *bindings,
    const VkDescriptorType *types, uint32_t binding_count,
    VkDescriptorSetLayout set_layout)
{
    if (list->resolve_hook) {
        /* The hook (BD11) knows only D3D bindings; the offsets table, if any, gets zeros (exact view offsets). */
        const int table = binding_count && bindings[binding_count - 1].type == BD12_RANGE_TEXEL_OFFSETS;
        VkDescriptorSet set = list->resolve_hook(list, list->resolve_user, bindings, types,
                                                 binding_count - (table ? 1 : 0), set_layout,
                                                 state == &list->compute);
        if (set && table) {
            const BD12ShaderBinding *b = &bindings[binding_count - 1];
            const VkDeviceSize bytes = (VkDeviceSize)(b->count ? b->count : 1) * 4;
            BD12Buffer *scratch = NULL;
            VkDeviceSize at = 0;
            void *cpu = NULL;
            if (!scratch_alloc(list->allocator, bytes, &scratch, &at, &cpu)) return VK_NULL_HANDLE;
            memset(cpu, 0, bytes);
            const VkDescriptorBufferInfo info = { scratch->buffer, at, bytes };
            const VkWriteDescriptorSet write = {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = b->binding,
                .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &info
            };
            list->device->vk.UpdateDescriptorSets(list->device->device, 1, &write, 0, NULL);
        }
        return set;
    }
    BD12_PROF_BEGIN();
    VkDescriptorSet set = resolve_set_impl(list, state, bindings, types,
                                           binding_count, set_layout);
    BD12_PROF_END(BD12_PROF_RESOLVE);
    return set;
}

int bd12_cmd_dispatch_bound(BD12CmdList *list, uint32_t x, uint32_t y,
                            uint32_t z)
{
    BD12ComputePipeline *pso = list->compute_pipeline;
    if (!pso || !pso->set_layout || (!list->compute.root_signature && !list->resolve_hook)) {
        BD12_DROP(BD12_DROP_DISPATCH_NO_STATE);
        return 0;
    }
    if (bd12_debug_sync_all()) bd12_cmd_global_barrier(list);
    BD12Device *device = list->device;
    bd12_cmd_end_rendering(list);
    const int capture = bd12_capture_wants(pso->shader_hash) ||
        (pso->writes_3d_storage_image && getenv("BEER_BD12_CAPTURE_3D") &&
         getenv("BEER_BD12_CAPTURE_DIR") && bd12_trace_active());
    if (capture) bd12_capture_begin();
    VkDescriptorSet set = bd12_resolve_descriptor_set(
        list, &list->compute, pso->bindings, pso->binding_types,
        pso->binding_count, pso->set_layout);
    if (capture) bd12_capture_record(list, 0, pso->shader_hash, x, y, z);
    if (!set || (pso->bindless_words && !bd12_mirror_prepare(list))) {
        if (capture) bd12_capture_end();
        BD12_DROP(BD12_DROP_DISPATCH_NO_DESCRIPTORS);
        return 0;
    }
    if (bd12_trace_active())
        fprintf(stderr, "[TRACE] dispatch cs=%016llx groups=%u,%u,%u uav3d=%d\n",
                (unsigned long long)pso->shader_hash, x, y, z, pso->writes_3d_storage_image);
    record_dispatch(list, x, y, z, 0);
    device->vk.CmdBindPipeline(list->command_buffer,
                               VK_PIPELINE_BIND_POINT_COMPUTE, pso->pipeline);
    device->vk.CmdBindDescriptorSets(list->command_buffer,
                                     VK_PIPELINE_BIND_POINT_COMPUTE,
                                     pso->layout, 0, 1, &set, 0, NULL);
    if (pso->bindless_words)
        bd12_cmd_bind_bindless(list, &list->compute, VK_PIPELINE_BIND_POINT_COMPUTE,
                               pso->layout, VK_SHADER_STAGE_COMPUTE_BIT,
                               pso->bindless, pso->bindless_count,
                               pso->bindless_words);
    device->vk.CmdDispatch(list->command_buffer, x, y, z);
    if (capture) {
        bd12_capture_record(list, 1, pso->shader_hash, x, y, z);
        if (getenv("BEER_BD12_CAPTURE_TWICE")) {
            /* Determinism check: run the identical dispatch again. */
            bd12_cmd_global_barrier(list);
            device->vk.CmdDispatch(list->command_buffer, x, y, z);
            bd12_capture_record(list, 2, pso->shader_hash, x, y, z);
        }
        bd12_capture_end();
    }
    return 1;
}

int bd12_cmd_dispatch_indirect_bound(BD12CmdList *list, BD12Buffer *args,
                                     VkDeviceSize offset)
{
    BD12ComputePipeline *pso = list->compute_pipeline;
    if (!pso || !pso->set_layout || (!list->compute.root_signature && !list->resolve_hook)) return 0;
    BD12Device *device = list->device;
    bd12_cmd_end_rendering(list);
    VkDescriptorSet set = bd12_resolve_descriptor_set(
        list, &list->compute, pso->bindings, pso->binding_types,
        pso->binding_count, pso->set_layout);
    if (!set || (pso->bindless_words && !bd12_mirror_prepare(list))) return 0;
    record_dispatch(list, 0, 0, 0, 1);
    device->vk.CmdBindPipeline(list->command_buffer,
                               VK_PIPELINE_BIND_POINT_COMPUTE, pso->pipeline);
    device->vk.CmdBindDescriptorSets(list->command_buffer,
                                     VK_PIPELINE_BIND_POINT_COMPUTE,
                                     pso->layout, 0, 1, &set, 0, NULL);
    if (pso->bindless_words)
        bd12_cmd_bind_bindless(list, &list->compute, VK_PIPELINE_BIND_POINT_COMPUTE,
                               pso->layout, VK_SHADER_STAGE_COMPUTE_BIT,
                               pso->bindless, pso->bindless_count,
                               pso->bindless_words);
    device->vk.CmdDispatchIndirect(list->command_buffer, args->buffer, offset);
    return 1;
}

