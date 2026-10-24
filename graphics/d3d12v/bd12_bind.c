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
                                       VK_NULL_HANDLE);
}

int bd12_binding_descriptor_type(const BD12ShaderBinding *b,
                                   VkDescriptorType *type)
{
    switch (b->type) {
    case BD12_RANGE_CBV: *type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; return 1;
    case BD12_RANGE_SRV:
        *type = b->is_buffer ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER :
                               VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        return 1;
    case BD12_RANGE_UAV:
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
    VkDescriptorType *types = calloc(binding_count ? binding_count : 1,
                                     sizeof(*types));
    BD12ComputePipeline *pipeline = NULL;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    uint32_t *dense_spirv = NULL;
    if (!layout_bindings || !copy || !types) goto done;
    uint32_t next_binding = 0;
    for (uint32_t i = 0; i < binding_count; ++i) copy[i] = bindings[i];
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
    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = binding_count,
        .pBindings = layout_bindings
    };
    if (device->vk.CreateDescriptorSetLayout(device->device, &layout_info, NULL,
                                             &set_layout) != VK_SUCCESS)
        goto done;
    pipeline = bd12_compute_pipeline_build(device, spirv, spirv_bytes, 0,
                                           set_layout);
    if (!pipeline) {
        device->vk.DestroyDescriptorSetLayout(device->device, set_layout, NULL);
        goto done;
    }
    pipeline->bindings = copy;
    pipeline->binding_types = types;
    pipeline->binding_count = binding_count;
    copy = NULL;
    types = NULL;
done:
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
        VkDescriptorPoolSize sizes[6];
        for (int i = 0; i < 6; ++i)
            sizes[i] = (VkDescriptorPoolSize){ kinds[i], BD12_POOL_DESCRIPTORS };
        VkDescriptorPoolCreateInfo pool_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = BD12_POOL_SETS,
            .poolSizeCount = 6,
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
    for (uint32_t i = 0; i < rs->parameter_count; ++i) {
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
            out->view = bd12_buffer_view(buffer, VK_FORMAT_R32_UINT, offset,
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
            out->buffer = d->buffer;
            out->offset = d->offset;
            out->range = d->range;
            out->source_buffer = d->source_buffer;
            out->view = d->buffer_view;
            out->image_view = d->image_view;
            out->image_layout = d->image_layout;
            out->sampler = d->sampler;
            out->image = d->image;
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
    VkDescriptorSet set = allocate_set(list->allocator, set_layout);
    if (!set) return VK_NULL_HANDLE;

    enum { MAX_WRITES = 64, MAX_INFOS = 256 };
    VkWriteDescriptorSet writes[MAX_WRITES];
    VkDescriptorBufferInfo buffer_infos[MAX_INFOS];
    VkBufferView views[MAX_INFOS];
    VkDescriptorImageInfo image_infos[MAX_INFOS];
    uint32_t write_count = 0, info_count = 0;
    for (uint32_t i = 0; i < binding_count; ++i) {
        const BD12ShaderBinding *b = &bindings[i];
        const VkDescriptorType type = types[i];
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
        else if (type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
            w->pBufferInfo = &buffer_infos[info_count];
        else
            w->pTexelBufferView = &views[info_count];
        info_count += b->count;
    }
    if (write_count)
        device->vk.UpdateDescriptorSets(device->device, write_count, writes, 0,
                                        NULL);
    return set;
}

static void record_dispatch(BD12CmdList *list, uint32_t x, uint32_t y,
                            uint32_t z, int indirect)
{
    BD12ComputePipeline *pso = list->compute_pipeline;
    const uint64_t sequence = list->dispatch_count++;
    BD12DispatchFlight *entry =
        &list->dispatch_flight[sequence % BD12_FLIGHT_DISPATCHES];
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

VkDescriptorSet bd12_resolve_descriptor_set(
    BD12CmdList *list, BD12BindState *state, const BD12ShaderBinding *bindings,
    const VkDescriptorType *types, uint32_t binding_count,
    VkDescriptorSetLayout set_layout)
{
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
    if (!pso || !pso->set_layout || !list->compute.root_signature) return 0;
    BD12Device *device = list->device;
    bd12_cmd_end_rendering(list);
    VkDescriptorSet set = bd12_resolve_descriptor_set(
        list, &list->compute, pso->bindings, pso->binding_types,
        pso->binding_count, pso->set_layout);
    if (!set) return 0;
    record_dispatch(list, x, y, z, 0);
    device->vk.CmdBindPipeline(list->command_buffer,
                               VK_PIPELINE_BIND_POINT_COMPUTE, pso->pipeline);
    device->vk.CmdBindDescriptorSets(list->command_buffer,
                                     VK_PIPELINE_BIND_POINT_COMPUTE,
                                     pso->layout, 0, 1, &set, 0, NULL);
    device->vk.CmdDispatch(list->command_buffer, x, y, z);
    return 1;
}

int bd12_cmd_dispatch_indirect_bound(BD12CmdList *list, BD12Buffer *args,
                                     VkDeviceSize offset)
{
    BD12ComputePipeline *pso = list->compute_pipeline;
    if (!pso || !pso->set_layout || !list->compute.root_signature) return 0;
    BD12Device *device = list->device;
    bd12_cmd_end_rendering(list);
    VkDescriptorSet set = bd12_resolve_descriptor_set(
        list, &list->compute, pso->bindings, pso->binding_types,
        pso->binding_count, pso->set_layout);
    if (!set) return 0;
    record_dispatch(list, 0, 0, 0, 1);
    device->vk.CmdBindPipeline(list->command_buffer,
                               VK_PIPELINE_BIND_POINT_COMPUTE, pso->pipeline);
    device->vk.CmdBindDescriptorSets(list->command_buffer,
                                     VK_PIPELINE_BIND_POINT_COMPUTE,
                                     pso->layout, 0, 1, &set, 0, NULL);
    device->vk.CmdDispatchIndirect(list->command_buffer, args->buffer, offset);
    return 1;
}

