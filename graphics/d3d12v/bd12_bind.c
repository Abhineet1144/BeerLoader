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

int bd12_partial_arrays_enabled(BD12Device *device)
{
    static int disabled = -1;
    if (disabled < 0) disabled = getenv("BEER_BD12_NO_PARTIAL_ARRAYS") != NULL;
    return !disabled && device->modern && device->modern->descriptor_indexing;
}

const void *bd12_layout_partial_arrays(BD12Device *device, const VkDescriptorSetLayoutBinding *bindings,
                                       uint32_t count, VkDescriptorBindingFlags *flags,
                                       VkDescriptorSetLayoutBindingFlagsCreateInfo *info)
{
    if (!bd12_partial_arrays_enabled(device)) return NULL;
    for (uint32_t i = 0; i < count; ++i)
        flags[i] = bindings[i].descriptorCount > 1 ? VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT : 0;
    *info = (VkDescriptorSetLayoutBindingFlagsCreateInfo){
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .bindingCount = count, .pBindingFlags = flags,
    };
    return info;
}

int bd12_binding_descriptor_type(const BD12ShaderBinding *b,
                                   VkDescriptorType *type)
{
    switch (b->type) {
    case BD12_RANGE_CBV: *type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; return 1;
    case BD12_RANGE_SRV:
        if (b->is_accel) { *type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR; return 1; }
        if (b->is_ssbo) { *type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; return 1; }
        *type = b->is_buffer ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER :
                               VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        return 1;
    case BD12_RANGE_UAV:
        if (b->is_ssbo) { *type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; return 1; }
        *type = b->is_buffer ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER :
                               VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        return 1;
    case BD12_RANGE_SAMPLER: *type = VK_DESCRIPTOR_TYPE_SAMPLER; return 1;
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
    VkDescriptorSetLayoutBinding *layout_bindings =
        calloc(binding_count ? binding_count : 1, sizeof(*layout_bindings));
    BD12ShaderBinding *copy = calloc(binding_count ? binding_count : 1,
                                     sizeof(*copy));
    BD12ShaderBinding *bindless = calloc(binding_count ? binding_count : 1,
                                         sizeof(*bindless));
    uint32_t bindless_count = 0, bindless_words = 0;
    VkDescriptorType *types = calloc(binding_count ? binding_count : 1,
                                     sizeof(*types));
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
    VkDescriptorBindingFlags *binding_flags = calloc(binding_count ? binding_count : 1, sizeof(*binding_flags));
    VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info;
    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = binding_flags ? bd12_layout_partial_arrays(device, layout_bindings, binding_count, binding_flags,
                                                            &flags_info) : NULL,
        .bindingCount = binding_count,
        .pBindings = layout_bindings
    };
    set_layout = bd12_shared_set_layout(device, &layout_info);          /* shared: never destroyed */
    free(binding_flags);
    if (!set_layout) goto done;
    pipeline = bd12_compute_pipeline_build(device, spirv, spirv_bytes, 0,
                                           set_layout, bindless_words);
    if (!pipeline) goto done;
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
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
        };
        VkDescriptorPoolSize sizes[8];
        for (int i = 0; i < 7; ++i)
            sizes[i] = (VkDescriptorPoolSize){ kinds[i], BD12_POOL_DESCRIPTORS };
        sizes[7] = (VkDescriptorPoolSize){ VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 256 };
        VkDescriptorPoolCreateInfo pool_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = BD12_POOL_SETS,
            .poolSizeCount = device->rt_enabled ? 8u : 7u,
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
    list->heaps[1] = sampler;   /* part of each resolve-cache key */
}

void bd12_cmd_set_compute_root_signature(BD12CmdList *list,
                                         BD12RootSignature *signature)
{
    if (list->compute.root_signature == signature) return;
    memset(list->compute.arguments, 0, sizeof(list->compute.arguments));
    list->compute.root_signature = signature;
    ++list->compute.epoch;
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
    if (arg->cbv_valid && !memcmp(&arg->constants[dest_offset], data, count * sizeof(uint32_t))) return;
    memcpy(&arg->constants[dest_offset], data, count * sizeof(uint32_t));
    arg->cbv_valid = 0;
    ++list->compute.epoch;
    BD12_ROOT_CHANGED(1, 0);
}

void bd12_cmd_set_compute_root_va(BD12CmdList *list, uint32_t parameter,
                                  uint64_t gpu_va)
{
    if (parameter < BD12_MAX_ROOT_PARAMETERS && list->compute.arguments[parameter].value != gpu_va) {
        list->compute.arguments[parameter].value = gpu_va;
        ++list->compute.epoch;
        BD12_ROOT_CHANGED(1, 1);
    }
}

void bd12_cmd_set_compute_root_table(BD12CmdList *list, uint32_t parameter,
                                     uint64_t gpu_handle)
{
    if (parameter < BD12_MAX_ROOT_PARAMETERS && list->compute.arguments[parameter].value != gpu_handle) {
        list->compute.arguments[parameter].value = gpu_handle;
        ++list->compute.epoch;
        BD12_ROOT_CHANGED(1, 2);
    }
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
} Resolved;

static const BD12Descriptor *table_descriptor(BD12CmdList *list,
                                              const BD12RootParameter *p,
                                              uint64_t handle, uint32_t type,
                                              uint32_t space, uint32_t reg,
                                              uint32_t *run)
{
    if (run) *run = 0;
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
        if (index >= heap->count) return NULL;
        if (run) {   /* descriptors from this one to the end of the range (or of the heap, for unbounded ranges) */
            uint32_t left = heap->count - index;
            if (range->count != UINT32_MAX && range->count - rel < left) left = range->count - rel;
            *run = left;
        }
        return &heap->descriptors[index];
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
    case 3: return visibility == 6;                    /* AMPLIFICATION */
    case 4: return visibility == 7;                    /* MESH */
    default: return 1;
    }
}

/* Candidate root parameters for a binding: those that can satisfy any register of [reg, reg + count), by the same
 * static tests resolve_one applies, in order. Array bindings use them too, so unbound elements of a large array do not
 * rescan every parameter. */
static void build_candidates(BD12ShaderBinding *b, const BD12RootSignature *rs)
{
    uint32_t found[32], n = 0, overflow = 0;
    const uint32_t first = b->reg, last = b->reg + (b->count ? b->count - 1 : 0);
    for (uint32_t i = 0; i < rs->parameter_count; ++i) {
        const BD12RootParameter *p = &rs->parameters[i];
        if (!visibility_matches(p->visibility, b->stage)) continue;
        int match = 0;
        switch (p->type) {
        case BD12_ROOT_CONSTANTS:
            match = b->type == BD12_RANGE_CBV && p->shader_register >= first && p->shader_register <= last &&
                    p->space == b->space;
            break;
        case BD12_ROOT_CBV: case BD12_ROOT_SRV: case BD12_ROOT_UAV: {
            const uint32_t want = p->type == BD12_ROOT_CBV ? BD12_RANGE_CBV :
                                  p->type == BD12_ROOT_SRV ? BD12_RANGE_SRV : BD12_RANGE_UAV;
            match = b->type == want && p->shader_register >= first && p->shader_register <= last &&
                    p->space == b->space;
            break;
        }
        case BD12_ROOT_TABLE:
            for (uint32_t r = 0; r < p->range_count && !match; ++r) {
                const BD12DescriptorRange *range = &p->ranges[r];
                if (range->type != b->type || range->space != b->space || last < range->base_register) continue;
                if (range->count != UINT32_MAX && first >= range->base_register + range->count) continue;
                match = 1;   /* [base_register, base_register + count) overlaps [first, last] */
            }
            break;
        default: match = 1; break;
        }
        if (!match) continue;
        if (n == 32) { overflow = 1; break; }
        found[n++] = i;
    }
    b->cand_count = overflow ? 0xff : n;
    for (uint32_t i = 0; i < n; ++i) b->cand[i] = (uint8_t)found[i];
    __atomic_store_n(&b->cand_serial, rs->serial, __ATOMIC_RELEASE);
}

/* A heap descriptor as a resolved binding (shared by resolve_one and consecutive array elements). */
/* want_view: the binding reads buffers through texel-buffer views (not storage buffers). */
static void resolve_from_descriptor(const BD12Descriptor *d, Resolved *out, int want_view)
{
    memset(out, 0, sizeof(*out));
    out->kind = d->kind;
    if (d->kind == BD12_DESC_SRV_AS) { out->as_va = d->offset; return; }
    /* A descriptor left behind by a destroyed resource acts as a null descriptor. */
    if (!bd12_descriptor_alive(d)) { out->kind = BD12_DESC_NONE; return; }
    out->buffer = d->buffer;
    out->offset = d->offset;
    out->range = d->range;
    out->source_buffer = d->source_buffer;
    out->view = d->buffer_view;
    if (want_view && (d->kind == BD12_DESC_SRV_BUFFER || d->kind == BD12_DESC_UAV_BUFFER) &&
        d->source_buffer && !d->buffer_view) {
        out->view = bd12_buffer_view(d->source_buffer, d->format, d->offset,
                                     d->range, d->kind == BD12_DESC_UAV_BUFFER);
        if (!out->view) {          /* invalid range/alignment: act as a null descriptor */
            memset(out, 0, sizeof(*out));
            return;
        }
    }
    out->image_view = d->image_view;
    out->image_layout = d->image_layout;
    out->sampler = d->sampler;
    out->image = d->image;
    out->view_mip = d->view_mip;
    out->view_base_layer = d->view_base_layer;
    out->view_layers = d->view_layers;
}

/* *table_run (optional): when the slot came from a descriptor table, the heap descriptor and how many
 * consecutive descriptors follow it in the same range, so array elements can be read without searching again. */
static int resolve_one(BD12CmdList *list, BD12BindState *state,
                       const BD12ShaderBinding *b, uint32_t reg, Resolved *out,
                       const BD12Descriptor **table_desc, uint32_t *table_run)
{
    if (table_desc) *table_desc = NULL;
    if (table_run) *table_run = 0;
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
    if (__atomic_load_n(&b->cand_serial, __ATOMIC_ACQUIRE) != rs->serial)
        build_candidates((BD12ShaderBinding *)b, rs);
    if (b->cand_serial == rs->serial && b->cand_count != 0xff) {
        scan = b->cand_count;
        cands = b->cand;
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
            out->kind = want == BD12_RANGE_SRV ? BD12_DESC_SRV_BUFFER : BD12_DESC_UAV_BUFFER;
            if (b->is_ssbo) return 1;            /* storage buffer: no view */
            out->view = bd12_buffer_view(buffer, VK_FORMAT_R32_UINT, offset,
                                         range & ~(VkDeviceSize)3,
                                         want == BD12_RANGE_UAV);
            if (!out->view) return 0;
            out->kind = want == BD12_RANGE_SRV ? BD12_DESC_SRV_BUFFER :
                                                 BD12_DESC_UAV_BUFFER;
            return 1;
        }
        case BD12_ROOT_TABLE: {
            uint32_t run = 0;
            const BD12Descriptor *d = table_descriptor(
                list, p, arg->value, b->type, b->space, reg, &run);
            if (!d) break;
            resolve_from_descriptor(d, out, !b->is_ssbo);
            if (table_desc) *table_desc = d;
            if (table_run) *table_run = run;
            return 1;
        }
        }
    }
    return 1;                                    /* unresolved: null */
}

/* All elements of an array binding at once (b->count > 1, not samplers, candidates built for this root signature):
 * candidate parameters in resolve_one's order; each table range that overlaps the array fills the elements it covers
 * that no earlier parameter filled, straight from the heap; registers named by a root descriptor go through
 * resolve_one; anything left is a null descriptor. Same results as resolving every element, without searching the
 * root signature again per element. Returns 0 to fall back to per-element resolution. */
/* filled[e] (caller's array, b->count entries) says which elements were written; the others are null and their
 * Resolved entries are left untouched (zeroing ~1000 112-byte entries per draw was a large part of the cost). */
static int resolve_array(BD12CmdList *list, BD12BindState *state, const BD12ShaderBinding *b, Resolved *out,
                         uint8_t *filled)
{
    const BD12RootSignature *rs = state->root_signature;
    /* Constant-buffer arrays too: engines declare b0..b3 as large arrays (RE9: 256 each) of which a root table
     * fills a few, so per-element resolution searched the root signature ~1000 times per draw for empty slots. */
    if (b->count <= 1 || b->type == BD12_RANGE_SAMPLER) return 0;
    if (__atomic_load_n(&b->cand_serial, __ATOMIC_ACQUIRE) != rs->serial) build_candidates((BD12ShaderBinding *)b, rs);
    if (b->cand_serial != rs->serial || b->cand_count == 0xff) return 0;
    if (b->count > 256) return 0;
    memset(filled, 0, b->count);
    const uint32_t first = b->reg, last = b->reg + b->count - 1;
    for (uint32_t ci = 0; ci < b->cand_count; ++ci) {
        const uint32_t i = b->cand[ci];
        const BD12RootParameter *p = &rs->parameters[i];
        if (p->type != BD12_ROOT_TABLE) {
            /* A root descriptor names one register: resolve it the ordinary way if nothing earlier claimed it. */
            if (p->shader_register >= first && p->shader_register <= last && !filled[p->shader_register - first]) {
                const uint32_t e = p->shader_register - first;
                if (!resolve_one(list, state, b, p->shader_register, &out[e], NULL, NULL)) return 0;
                filled[e] = 1;
            }
            continue;
        }
        BD12DescriptorHeap *heap = list->heaps[0];
        const uint64_t handle = state->arguments[i].value;
        if (!heap || handle < heap->gpu_base ||
            handle >= heap->gpu_base + (uint64_t)heap->count * BD12_DESCRIPTOR_INCREMENT)
            continue;
        const uint32_t base = (uint32_t)((handle - heap->gpu_base) / BD12_DESCRIPTOR_INCREMENT);
        uint32_t running = 0;
        for (uint32_t r = 0; r < p->range_count; ++r) {
            const BD12DescriptorRange *range = &p->ranges[r];
            const uint32_t offset = range->offset == BD12_RANGE_APPEND ? running : range->offset;
            running = offset + (range->count == UINT32_MAX ? 0 : range->count);
            if (range->type != b->type || range->space != b->space) continue;
            const uint32_t lo = range->base_register > first ? range->base_register : first;
            uint32_t hi = last;
            if (range->count != UINT32_MAX && range->base_register + range->count - 1 < hi)
                hi = range->base_register + range->count - 1;
            for (uint32_t reg = lo; reg <= hi && reg >= lo; ++reg) {
                const uint32_t e = reg - first;
                if (filled[e]) continue;
                const uint32_t index = base + offset + (reg - range->base_register);
                if (index >= heap->count) break;
                resolve_from_descriptor(&heap->descriptors[index], &out[e], !b->is_ssbo);
                filled[e] = 1;
            }
        }
    }
    return 1;
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
    /* Allocated up front only when the writes overflow the local arrays; otherwise the finished writes go
     * through the allocator's set cache. */
    VkDescriptorSet set = VK_NULL_HANDLE;

    enum { MAX_WRITES = 64, MAX_INFOS = 256 };
    VkWriteDescriptorSet writes[MAX_WRITES];
    VkDescriptorBufferInfo buffer_infos[MAX_INFOS];
    VkBufferView views[MAX_INFOS];
    VkDescriptorImageInfo image_infos[MAX_INFOS];
    VkAccelerationStructureKHR as_handles[MAX_INFOS];
    VkWriteDescriptorSetAccelerationStructureKHR as_writes[MAX_WRITES];
    uint32_t write_count = 0, info_count = 0;
    const int capture = bd12_capture_enabled();   /* diagnostic capture: read once, not per element */
    const int partial_arrays = bd12_partial_arrays_enabled(device);
    /* Per-element diagnostics (bind logs, captures, constant-buffer tweaks), decided once per set: draws resolve
     * hundreds of array elements each. */
    const int diagnostics = capture || bd12_trace_active() || __atomic_load_n(&g_bd12_cbt_active, __ATOMIC_RELAXED);
    for (uint32_t i = 0; i < binding_count; ++i) {
        const BD12ShaderBinding *b = &bindings[i];
        const VkDescriptorType type = types[i];
        if (b->count > MAX_INFOS) return VK_NULL_HANDLE;
        if (write_count == MAX_WRITES || info_count + b->count > MAX_INFOS) {
            if (!set && !(set = allocate_set(list->allocator, set_layout))) return VK_NULL_HANDLE;
            for (uint32_t k = 0; k < write_count; ++k) writes[k].dstSet = set;
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
        /* Array bindings are resolved in one pass (resolve_array); single descriptors one at a time. */
        static __thread Resolved array_out[MAX_INFOS];
        static __thread uint8_t array_filled[MAX_INFOS];
        static const Resolved null_resolved;
        const int batched = resolve_array(list, state, b, array_out, array_filled);
        /* Partially bound arrays (bd12_layout_partial_arrays): write only up to the last element the root signature
         * supplies, nothing when it supplies none. Engines declare large arrays and fill a few slots. */
        uint32_t element_count = b->count;
        if (batched && partial_arrays) {
            while (element_count && !array_filled[element_count - 1]) --element_count;
            if (!element_count) { --write_count; continue; }
            w->descriptorCount = element_count;
        }
        for (uint32_t e = 0; e < element_count; ++e) {
            Resolved single;
            const Resolved *rp = &single;
            if (batched) rp = array_filled[e] ? &array_out[e] : &null_resolved;
            else if (!resolve_one(list, state, b, b->reg + e, &single, NULL, NULL)) return VK_NULL_HANDLE;
            if (diagnostics) {
            const Resolved r_ = *rp;
#define r r_
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
            if (capture && getenv("BEER_BD12_BIND_LOG"))
                fprintf(stderr, "[BIND] type=%u space=%u reg=%u+%u binding=%u bindless=%u accel=%u -> kind=%u buf=%p img=%p\n",
                        b->type, b->space, b->reg, e, b->binding, b->bindless, b->is_accel, r.kind, (void *)(uintptr_t)r.buffer, (void *)r.image);
            if (capture && r.kind == BD12_DESC_UAV_IMAGE && r.image)
                bd12_capture_note_image(b->reg + e, b->space, r.image, r.view_mip);
            if (capture) {
                if (r.kind == BD12_DESC_CBV)
                    bd12_capture_note(0, b->space, b->reg + e, r.buffer, r.offset, r.range);
                else if ((r.kind == BD12_DESC_SRV_BUFFER || r.kind == BD12_DESC_UAV_BUFFER) &&
                         r.source_buffer)
                    bd12_capture_note(r.kind == BD12_DESC_SRV_BUFFER ? 1 : 2, b->space,
                                      b->reg + e, r.source_buffer->buffer, r.offset, r.range);
            }
            }
#undef r
#define r (*rp)
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
            } else if (type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
                const int bound = (r.kind == BD12_DESC_SRV_BUFFER || r.kind == BD12_DESC_UAV_BUFFER) && r.buffer;
                VkDeviceSize offset = bound ? r.offset : 0, range = bound ? r.range : VK_WHOLE_SIZE;
                const VkDeviceSize misaligned = offset % device->min_storage_buffer_alignment;
                if (bound && misaligned) {
                    /* The shader indexes from the view's first byte; Vulkan wants an aligned start. Rare (views
                     * usually start on 16 bytes); the elements shift by the remainder. */
                    static int warned;
                    if (!warned++)
                        fprintf(stderr, "[BD12] storage-buffer view at offset %llu is not %llu-byte aligned\n",
                                (unsigned long long)offset,
                                (unsigned long long)device->min_storage_buffer_alignment);
                    offset -= misaligned;
                    range += misaligned;
                }
                buffer_infos[info_count + e] = (VkDescriptorBufferInfo){ bound ? r.buffer : VK_NULL_HANDLE, offset, range };
            } else {
                views[info_count + e] = r.view;
            }
#undef r
        }
        if (type == VK_DESCRIPTOR_TYPE_SAMPLER ||
            type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
            type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            w->pImageInfo = &image_infos[info_count];
        else if (type == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) {
            VkWriteDescriptorSetAccelerationStructureKHR *aw = &as_writes[write_count - 1];
            *aw = (VkWriteDescriptorSetAccelerationStructureKHR){
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
                .accelerationStructureCount = element_count,
                .pAccelerationStructures = &as_handles[info_count]
            };
            w->pNext = aw;
        } else if (type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
            w->pBufferInfo = &buffer_infos[info_count];
        else
            w->pTexelBufferView = &views[info_count];
        info_count += element_count;
    }
    const uint64_t t_update = bd12_prof_now();
    if (!set) {
        set = bd12_descriptor_set_for_writes(list->allocator, set_layout, writes, write_count);
    } else if (write_count) {
        for (uint32_t k = 0; k < write_count; ++k) writes[k].dstSet = set;
        device->vk.UpdateDescriptorSets(device->device, write_count, writes, 0,
                                        NULL);
    }
    g_bd12_prof_ns[BD12_PROF_UPDATE_SETS] += bd12_prof_now() - t_update;
    ++g_bd12_prof_calls[BD12_PROF_UPDATE_SETS];
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

uint64_t g_bd12_descriptor_epoch;

/* What a root argument contributes to a resolved set: the table handle or VA, and for root constants the scratch
 * copy the set points at (cbv_valid drops when the constants change). */
static void resolve_cache_arg(const BD12BindState *state, uint32_t i, uint64_t *value, uint64_t *extra)
{
    const BD12RootArgument *arg = &state->arguments[i];
    if (state->root_signature->parameters[i].type == BD12_ROOT_CONSTANTS) {
        *value = arg->cbv_valid ? (uint64_t)(uintptr_t)arg->cbv_buffer : 0;
        *extra = arg->cbv_valid ? (uint64_t)arg->cbv_offset : UINT64_MAX;
    } else {
        *value = arg->value;
        *extra = 0;
    }
}

static void resolve_cache_record_args(BD12ResolveCacheEntry *e, const BD12BindState *state,
                                      const BD12ShaderBinding *bindings, uint32_t binding_count)
{
    const BD12RootSignature *rs = state->root_signature;
    e->root_signature = rs;
    e->nrel = 0xff;
    if (!rs) return;
    uint8_t n = 0;
    for (uint32_t i = 0; i < binding_count; ++i) {
        const BD12ShaderBinding *b = &bindings[i];
        if (b->cand_serial != rs->serial || b->cand_count == 0xff) return;   /* unknown: fall back to the epoch */
        for (uint32_t c = 0; c < b->cand_count; ++c) {
            const uint8_t p = b->cand[c];
            int seen = 0;
            for (uint8_t k = 0; k < n; ++k) seen |= e->rel[k] == p;
            if (seen) continue;
            if (n == 16) return;
            e->rel[n++] = p;
        }
    }
    for (uint8_t k = 0; k < n; ++k) resolve_cache_arg(state, e->rel[k], &e->rel_value[k], &e->rel_extra[k]);
    e->nrel = n;
}

static int resolve_cache_args_match(const BD12ResolveCacheEntry *e, const BD12BindState *state)
{
    for (uint8_t k = 0; k < e->nrel; ++k) {
        uint64_t value, extra;
        resolve_cache_arg(state, e->rel[k], &value, &extra);
        if (value != e->rel_value[k] || extra != e->rel_extra[k]) return 0;
    }
    return 1;
}
/* Resolve-cache outcomes for BEER_BD12_PROF: hit, miss with no entry for the layout, miss because the bind state
 * changed, miss because descriptors were written (state unchanged). */
uint64_t g_bd12_resolve_cache_stats[4];
uint64_t g_bd12_root_changes[2][3];

VkDescriptorSet bd12_resolve_descriptor_set(
    BD12CmdList *list, BD12BindState *state, const BD12ShaderBinding *bindings,
    const VkDescriptorType *types, uint32_t binding_count,
    VkDescriptorSetLayout set_layout)
{
    if (list->resolve_hook)
        return list->resolve_hook(list, list->resolve_user, bindings, types, binding_count,
                                  set_layout, state == &list->compute);
    /* The same layout resolved again with nothing it reads changed (root signature and arguments, bound heaps,
     * descriptor contents) gives the same set: reuse it. Draws usually rebind identical state. BEER_BD12_NO_RESOLVE_CACHE=1
     * resolves every time. */
    static int cache_off = -1;
    if (cache_off < 0) cache_off = getenv("BEER_BD12_NO_RESOLVE_CACHE") != NULL;
    const int which = state == &list->compute;
    /* Descriptor changes: resource destruction (global) and writes to the bound heaps, which count writes once
     * they are marked as read here. Every counter only grows, so the sum changes whenever one of them does. */
    uint64_t descriptor_epoch = __atomic_load_n(&g_bd12_descriptor_epoch, __ATOMIC_RELAXED);
    for (int h = 0; h < 2; ++h) {
        BD12DescriptorHeap *heap = list->heaps[h];
        if (!heap) continue;
        if (!__atomic_load_n(&heap->legacy_reads, __ATOMIC_RELAXED))
            __atomic_store_n(&heap->legacy_reads, 1, __ATOMIC_SEQ_CST);
        descriptor_epoch += __atomic_load_n(&heap->write_epoch, __ATOMIC_ACQUIRE);
    }
    if (!cache_off) {
        int reason = 1;
        for (uint32_t i = 0; i < BD12_RESOLVE_CACHE; ++i) {
            const BD12ResolveCacheEntry *e = &list->resolve_cache[which][i];
            if (!e->set || e->layout != set_layout || e->bindings != bindings || e->binding_count != binding_count ||
                e->heaps[0] != list->heaps[0] || e->heaps[1] != list->heaps[1])
                continue;
            if (e->root_signature != state->root_signature) { if (reason < 2) reason = 2; continue; }
            if (e->nrel == 0xff ? e->state_epoch != state->epoch : !resolve_cache_args_match(e, state)) {
                if (reason < 2) reason = 2;
                continue;
            }
            if (e->descriptor_epoch != descriptor_epoch) { reason = 3; continue; }
            __atomic_add_fetch(&g_bd12_resolve_cache_stats[0], 1, __ATOMIC_RELAXED);
            return e->set;
        }
        __atomic_add_fetch(&g_bd12_resolve_cache_stats[reason], 1, __ATOMIC_RELAXED);
    }
    BD12_PROF_BEGIN();
    VkDescriptorSet set = resolve_set_impl(list, state, bindings, types,
                                           binding_count, set_layout);
    BD12_PROF_END(BD12_PROF_RESOLVE);
    if (!cache_off && set) {
        BD12ResolveCacheEntry *e = &list->resolve_cache[which][list->resolve_cache_next[which]++ % BD12_RESOLVE_CACHE];
        *e = (BD12ResolveCacheEntry){ set_layout, bindings, binding_count, state->epoch, descriptor_epoch,
                                      { list->heaps[0], list->heaps[1] }, set };
        resolve_cache_record_args(e, state, bindings, binding_count);
    }
    return set;
}

/* Descriptor-buffer compute pipeline: heaps bound once, root arguments pushed (bd12_heapdb.c). */
static int bind_compute_db(BD12CmdList *list, BD12ComputePipeline *pso)
{
    if (!bd12_db_prepare(list)) return 0;
    list->device->vk.CmdBindPipeline(list->command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pso->pipeline);
    return bd12_cmd_push_root_arguments(list, &list->compute, VK_PIPELINE_BIND_POINT_COMPUTE, pso->layout,
                                        VK_SHADER_STAGE_COMPUTE_BIT, pso->root_push_bytes, pso->root_layout_hash,
                                        pso->db_layout);
}

int bd12_cmd_dispatch_bound(BD12CmdList *list, uint32_t x, uint32_t y,
                            uint32_t z)
{
    BD12ComputePipeline *pso = list->compute_pipeline;
    if (pso && pso->db_layout && list->compute.root_signature) {
        if (bd12_debug_sync_all()) bd12_cmd_global_barrier(list);
        bd12_cmd_end_rendering(list);
        if (!bind_compute_db(list, pso)) {
            BD12_DROP(BD12_DROP_DISPATCH_NO_DESCRIPTORS);
            return 0;
        }
        record_dispatch(list, x, y, z, 0);
        list->device->vk.CmdDispatch(list->command_buffer, x, y, z);
        return 1;
    }
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
    if (!set || ((pso->bindless_words || pso->root_push_bytes) && !bd12_mirror_prepare(list))) {
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
    bd12_cmd_note_set_binding(list, 1);
    if (pso->root_push_bytes) {
        if (!bd12_cmd_push_root_arguments(list, &list->compute, VK_PIPELINE_BIND_POINT_COMPUTE, pso->layout,
                                          VK_SHADER_STAGE_COMPUTE_BIT, pso->root_push_bytes, pso->root_layout_hash,
                                          NULL))
            return 0;
    } else if (pso->bindless_words)
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
    if (pso && pso->db_layout && list->compute.root_signature) {
        bd12_cmd_end_rendering(list);
        if (!bind_compute_db(list, pso)) return 0;
        record_dispatch(list, 0, 0, 0, 1);
        list->device->vk.CmdDispatchIndirect(list->command_buffer, args->buffer, offset);
        return 1;
    }
    if (!pso || !pso->set_layout || (!list->compute.root_signature && !list->resolve_hook)) return 0;
    BD12Device *device = list->device;
    bd12_cmd_end_rendering(list);
    VkDescriptorSet set = bd12_resolve_descriptor_set(
        list, &list->compute, pso->bindings, pso->binding_types,
        pso->binding_count, pso->set_layout);
    if (!set || ((pso->bindless_words || pso->root_push_bytes) && !bd12_mirror_prepare(list))) return 0;
    record_dispatch(list, 0, 0, 0, 1);
    device->vk.CmdBindPipeline(list->command_buffer,
                               VK_PIPELINE_BIND_POINT_COMPUTE, pso->pipeline);
    device->vk.CmdBindDescriptorSets(list->command_buffer,
                                     VK_PIPELINE_BIND_POINT_COMPUTE,
                                     pso->layout, 0, 1, &set, 0, NULL);
    bd12_cmd_note_set_binding(list, 1);
    if (pso->root_push_bytes) {
        if (!bd12_cmd_push_root_arguments(list, &list->compute, VK_PIPELINE_BIND_POINT_COMPUTE, pso->layout,
                                          VK_SHADER_STAGE_COMPUTE_BIT, pso->root_push_bytes, pso->root_layout_hash,
                                          NULL))
            return 0;
    } else if (pso->bindless_words)
        bd12_cmd_bind_bindless(list, &list->compute, VK_PIPELINE_BIND_POINT_COMPUTE,
                               pso->layout, VK_SHADER_STAGE_COMPUTE_BIT,
                               pso->bindless, pso->bindless_count,
                               pso->bindless_words);
    device->vk.CmdDispatchIndirect(list->command_buffer, args->buffer, offset);
    return 1;
}


/* Descriptor set reuse -------------------------------------------------------
 * Keyed by the exact descriptor contents (handles, offsets, ranges, layouts),
 * so a hit is the same set a fresh write would produce. Entries live until the
 * allocator is reset (when its pools are) and only within one device epoch:
 * views are recycled after they go unused for several epochs, so a handle
 * value seen this frame still names the same object. */
#define BD12_SET_CACHE_SLOTS 4096u          /* power of two */
#define BD12_SET_CACHE_MAX_KEY 1024u        /* words; larger sets are not cached */

typedef struct {
    uint64_t hash;
    VkDescriptorSetLayout layout;
    VkDescriptorSet set;
    uint32_t key_offset, key_words;
} BD12SetCacheEntry;

struct BD12SetCache {
    BD12SetCacheEntry slots[BD12_SET_CACHE_SLOTS];
    uint32_t used;
    uint64_t epoch;
    uint64_t *keys;
    size_t key_used, key_capacity;
};

void bd12_set_cache_clear(BD12CmdAllocator *allocator)
{
    struct BD12SetCache *c = allocator->set_cache;
    if (!c || !c->used) return;
    memset(c->slots, 0, sizeof(c->slots));
    c->used = 0;
    c->key_used = 0;
}

void bd12_set_cache_free(BD12CmdAllocator *allocator)
{
    struct BD12SetCache *c = allocator->set_cache;
    if (!c) return;
    free(c->keys);
    free(c);
    allocator->set_cache = NULL;
}

/* Serialises the writes into key words; 0 when they do not fit. */
static uint32_t set_key(VkDescriptorSetLayout layout, const VkWriteDescriptorSet *writes,
                        uint32_t count, uint64_t *key)
{
    uint32_t n = 0;
    key[n++] = (uint64_t)(uintptr_t)layout;
    for (uint32_t i = 0; i < count; ++i) {
        const VkWriteDescriptorSet *w = &writes[i];
        if (n + 2 + (size_t)w->descriptorCount * 3 > BD12_SET_CACHE_MAX_KEY) return 0;
        key[n++] = (uint64_t)w->dstBinding | (uint64_t)w->descriptorType << 32;
        key[n++] = (uint64_t)w->dstArrayElement | (uint64_t)w->descriptorCount << 32;
        for (uint32_t e = 0; e < w->descriptorCount; ++e) {
            switch (w->descriptorType) {
            case VK_DESCRIPTOR_TYPE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                key[n++] = (uint64_t)(uintptr_t)w->pImageInfo[e].sampler;
                key[n++] = (uint64_t)(uintptr_t)w->pImageInfo[e].imageView;
                key[n++] = (uint64_t)w->pImageInfo[e].imageLayout;
                break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
                key[n++] = (uint64_t)(uintptr_t)w->pBufferInfo[e].buffer;
                key[n++] = w->pBufferInfo[e].offset;
                key[n++] = w->pBufferInfo[e].range;
                break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                key[n++] = (uint64_t)(uintptr_t)w->pTexelBufferView[e];
                break;
            case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR: {
                const VkWriteDescriptorSetAccelerationStructureKHR *as = w->pNext;
                if (!as || as->sType != VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR)
                    return 0;
                key[n++] = (uint64_t)(uintptr_t)as->pAccelerationStructures[e];
                break;
            }
            default:
                return 0;
            }
        }
    }
    return n;
}

static VkDescriptorSet write_new_set(BD12CmdAllocator *allocator, VkDescriptorSetLayout layout,
                                     VkWriteDescriptorSet *writes, uint32_t count)
{
    VkDescriptorSet set = allocate_set(allocator, layout);
    if (!set) return VK_NULL_HANDLE;
    for (uint32_t i = 0; i < count; ++i) writes[i].dstSet = set;
    if (count)
        allocator->device->vk.UpdateDescriptorSets(allocator->device->device, count, writes, 0, NULL);
    return set;
}

VkDescriptorSet bd12_descriptor_set_for_writes(BD12CmdAllocator *allocator,
                                               VkDescriptorSetLayout layout,
                                               VkWriteDescriptorSet *writes,
                                               uint32_t count)
{
    if (getenv("BEER_BD12_NO_SET_CACHE")) return write_new_set(allocator, layout, writes, count);
    uint64_t key[BD12_SET_CACHE_MAX_KEY];
    const uint32_t words = set_key(layout, writes, count, key);
    if (!words) return write_new_set(allocator, layout, writes, count);
    struct BD12SetCache *c = allocator->set_cache;
    if (!c) {
        c = allocator->set_cache = calloc(1, sizeof(*c));
        if (!c) return write_new_set(allocator, layout, writes, count);
    }
    const uint64_t epoch = allocator->device->epoch;
    if (c->epoch != epoch || c->used >= BD12_SET_CACHE_SLOTS / 2) {
        bd12_set_cache_clear(allocator);
        c->epoch = epoch;
    }
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    for (uint32_t i = 0; i < words; ++i) {
        hash ^= key[i];
        hash *= UINT64_C(0x100000001b3);
        hash ^= hash >> 29;
    }
    if (!hash) hash = 1;
    uint32_t slot = (uint32_t)hash & (BD12_SET_CACHE_SLOTS - 1);
    for (;; slot = (slot + 1) & (BD12_SET_CACHE_SLOTS - 1)) {
        BD12SetCacheEntry *e = &c->slots[slot];
        if (!e->hash) break;
        if (e->hash == hash && e->layout == layout && e->key_words == words &&
            !memcmp(c->keys + e->key_offset, key, words * sizeof(uint64_t)))
            return e->set;
    }
    VkDescriptorSet set = write_new_set(allocator, layout, writes, count);
    if (!set) return VK_NULL_HANDLE;
    if (c->key_used + words > c->key_capacity) {
        size_t capacity = c->key_capacity ? c->key_capacity * 2 : 65536;
        while (capacity < c->key_used + words) capacity *= 2;
        uint64_t *grown = realloc(c->keys, capacity * sizeof(uint64_t));
        if (!grown) return set;
        c->keys = grown;
        c->key_capacity = capacity;
    }
    memcpy(c->keys + c->key_used, key, words * sizeof(uint64_t));
    c->slots[slot] = (BD12SetCacheEntry){ hash, layout, set, (uint32_t)c->key_used, words };
    c->key_used += words;
    ++c->used;
    return set;
}
