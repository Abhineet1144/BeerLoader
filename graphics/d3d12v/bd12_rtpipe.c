#define _GNU_SOURCE
#include "bd12.h"
#include "d3d12_dxil_translator.h"
#include "d3d12_shader_translator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* DXR state objects on VK_KHR_ray_tracing_pipeline.
 *
 * Shader identifiers are the real Vulkan shader group handles (32 bytes on every driver tested), so a shader binding
 * table the application fills with identifiers from GetShaderIdentifier is directly usable by vkCmdTraceRaysKHR:
 * D3D12 and Vulkan agree on record alignment (32 bytes) and table alignment (64 bytes).
 *
 * All shaders of a state object share one descriptor layout: the global root signature drives set 0 exactly like a
 * compute dispatch (per-draw resolved set + the bindless heap mirror as set 1). Local root signatures are not
 * supported yet: records must be identifier-only (RE8's are). */

#define RT_ALL_STAGES (VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | \
                       VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_INTERSECTION_BIT_KHR | VK_SHADER_STAGE_CALLABLE_BIT_KHR)

struct BD12RtPipeline {
    BD12ComputePipeline base;     /* layout, set layout, bindings: shared with the compute dispatch path */
    BD12Device *device;
    BD12RtDesc desc;
    int built, ok;
    uint32_t group_count;
    char (*group_names)[128];
    uint8_t *handles;             /* group_count * handle size */
    uint32_t handle_size;
    pthread_mutex_t lock;
};

/* ---- blob interning: thousands of state objects share a few dozen libraries ---- */

typedef struct Blob { uint64_t hash; size_t size; uint8_t *data; struct Blob *next; } Blob;
static Blob *g_blobs[1024];
static pthread_mutex_t g_blob_lock = PTHREAD_MUTEX_INITIALIZER;

const uint8_t *bd12_rt_intern_blob(const uint8_t *code, size_t size)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) { h ^= code[i]; h *= 1099511628211ull; }
    pthread_mutex_lock(&g_blob_lock);
    Blob *b = g_blobs[h % 1024];
    for (; b; b = b->next)
        if (b->hash == h && b->size == size && !memcmp(b->data, code, size)) break;
    if (!b) {
        b = calloc(1, sizeof(*b));
        if (b) {
            b->data = malloc(size);
            if (!b->data) { free(b); b = NULL; }
            else {
                memcpy(b->data, code, size);
                b->hash = h; b->size = size;
                b->next = g_blobs[h % 1024];
                g_blobs[h % 1024] = b;
            }
        }
    }
    pthread_mutex_unlock(&g_blob_lock);
    return b ? b->data : NULL;
}

BD12RtPipeline *bd12_rt_pipeline_create(BD12Device *device, const BD12RtDesc *desc)
{
    BD12RtPipeline *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->device = device;
    p->desc = *desc;
    pthread_mutex_init(&p->lock, NULL);
    return p;
}

void bd12_rt_pipeline_destroy(BD12RtPipeline *p)
{
    if (!p) return;
    BD12Device *device = p->device;
    if (p->built && p->ok) {
        device->vk.DestroyPipeline(device->device, p->base.pipeline, NULL);
        device->vk.DestroyPipelineLayout(device->device, p->base.layout, NULL);
        if (p->base.set_layout) device->vk.DestroyDescriptorSetLayout(device->device, p->base.set_layout, NULL);
    }
    for (uint32_t i = 0; i < p->desc.library_count; ++i) free(p->desc.libraries[i].exports);
    free(p->desc.libraries);
    free(p->desc.hit_groups);
    free(p->base.bindings);
    free(p->base.bindless);
    free(p->base.binding_types);
    free(p->group_names);
    free(p->handles);
    free(p);
}

typedef struct {
    char name[128];
    uint32_t stage;               /* dxil-spirv stage */
    uint8_t *spirv;
    size_t spirv_size;
    VkShaderModule module;
} RtShader;

static VkShaderStageFlagBits vk_stage(uint32_t dxil_stage)
{
    switch (dxil_stage) {
    case 7: return VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
    case 8: return VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    case 9: return VK_SHADER_STAGE_MISS_BIT_KHR;
    case 10: return VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
    case 11: return VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    case 12: return VK_SHADER_STAGE_CALLABLE_BIT_KHR;
    default: return 0;
    }
}

static int find_shader(const RtShader *shaders, uint32_t count, const char *name)
{
    if (!name || !*name) return -1;
    for (uint32_t i = 0; i < count; ++i)
        if (!strcmp(shaders[i].name, name)) return (int)i;
    return -1;
}

static int build(BD12RtPipeline *p)
{
    BD12Device *device = p->device;
    BD12RtDesc *d = &p->desc;
    BeerD3D12TranslatedShader *shared = calloc(1, sizeof(*shared));
    RtShader shaders[64];
    uint32_t shader_count = 0;
    memset(shaders, 0, sizeof(shaders));
    VkPipelineShaderStageCreateInfo stages[64];
    VkRayTracingShaderGroupCreateInfoKHR groups[96];
    BD12ShaderBinding bindings[BEER_D3D12_MAX_SHADER_DESCRIPTORS];
    BD12ShaderBinding *copy = NULL, *bindless = NULL;
    VkDescriptorType *types = NULL;
    VkDescriptorSetLayoutBinding *layout_bindings = NULL;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    uint32_t group_count = 0, set_count = 0, bindless_count = 0, bindless_words = 0, copy_count = 0;
    int ok = 0;
    if (!shared) return 0;

    /* 1. translate every library, keeping the exported entry points */
    for (uint32_t l = 0; l < d->library_count; ++l) {
        BeerD3D12RtShader translated[48];
        uint32_t count = 0;
        const BD12RtLibrary *lib = &d->libraries[l];
        if (!beer_d3d12_dxil_translate_library(lib->code, lib->size, shared, translated, 48, &count)) {
            fprintf(stderr, "[BD12 RT] DXIL library %u could not be translated\n", l);
            goto done;
        }
        for (uint32_t i = 0; i < count; ++i) {
            const char *export_name = NULL;
            if (!lib->export_count) export_name = translated[i].name;
            for (uint32_t e = 0; e < lib->export_count && !export_name; ++e) {
                const char *from = lib->exports[e].rename_from[0] ? lib->exports[e].rename_from : lib->exports[e].name;
                if (!strcmp(from, translated[i].name)) export_name = lib->exports[e].name;
            }
            if (!export_name || shader_count == 64 || find_shader(shaders, shader_count, export_name) >= 0) { free(translated[i].spirv); continue; }
            RtShader *s = &shaders[shader_count++];
            snprintf(s->name, sizeof(s->name), "%s", export_name);
            s->stage = translated[i].stage;
            s->spirv = translated[i].spirv;
            s->spirv_size = translated[i].spirv_size;
        }
    }
    if (!shader_count) goto done;

    /* 2. one descriptor layout for the whole pipeline */
    for (uint32_t i = 0; i < shared->descriptor_count; ++i) {
        const BeerD3D12ShaderDescriptor *s = &shared->descriptors[i];
        bindings[i] = (BD12ShaderBinding){
            .type = s->type, .space = s->register_space, .reg = s->register_index,
            .is_buffer = s->resource_type == BEER_D3D12_SHADER_RESOURCE_BUFFER,
            .is_accel = s->resource_type == BEER_D3D12_SHADER_RESOURCE_ACCEL,
            .count = s->count ? s->count : 1, .binding = s->binding, .bindless = s->bindless,
            .push_word = s->push_word, .heap_class = s->heap_class
        };
    }
    copy = calloc(shared->descriptor_count ? shared->descriptor_count : 1, sizeof(*copy));
    bindless = calloc(shared->descriptor_count ? shared->descriptor_count : 1, sizeof(*bindless));
    types = calloc(shared->descriptor_count ? shared->descriptor_count : 1, sizeof(*types));
    layout_bindings = calloc(shared->descriptor_count ? shared->descriptor_count : 1, sizeof(*layout_bindings));
    if (!copy || !bindless || !types || !layout_bindings) goto done;
    for (uint32_t i = 0; i < shared->descriptor_count; ++i) {
        if (bindings[i].bindless) {
            bindless[bindless_count++] = bindings[i];
            if (bindings[i].push_word + 1 > bindless_words) bindless_words = bindings[i].push_word + 1;
        } else copy[copy_count++] = bindings[i];
    }
    /* Dense binding numbers (deterministic: array order), applied to every module. */
    {
        BD12ShaderBinding scratch[BEER_D3D12_MAX_SHADER_DESCRIPTORS];
        for (uint32_t s = 0; s < shader_count; ++s) {
            uint32_t next = 0;
            memcpy(scratch, copy, copy_count * sizeof(*copy));
            uint32_t *dense = bd12_densify_bindings((const uint32_t *)shaders[s].spirv, shaders[s].spirv_size, scratch, copy_count, &next);
            if (!dense) goto done;
            free(shaders[s].spirv);
            shaders[s].spirv = (uint8_t *)dense;
            if (s == 0) memcpy(copy, scratch, copy_count * sizeof(*copy));
        }
        if (!shader_count) goto done;
    }
    for (uint32_t i = 0; i < copy_count; ++i) {
        if (!bd12_binding_descriptor_type(&copy[i], &types[i])) goto done;
        layout_bindings[i] = (VkDescriptorSetLayoutBinding){
            .binding = copy[i].binding, .descriptorType = types[i], .descriptorCount = copy[i].count ? copy[i].count : 1,
            .stageFlags = RT_ALL_STAGES
        };
    }
    {
        VkDescriptorSetLayoutCreateInfo info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                 .bindingCount = copy_count, .pBindings = layout_bindings };
        if (device->vk.CreateDescriptorSetLayout(device->device, &info, NULL, &set_layout) != VK_SUCCESS) goto done;
    }
    {
        VkDescriptorSetLayout set_layouts[2] = { set_layout, VK_NULL_HANDLE };
        set_count = 1;
        if (bindless_words) {
            set_layouts[1] = bd12_mirror_layout(device);
            if (!set_layouts[1]) goto done;
            set_count = 2;
        }
        VkPushConstantRange range = { .stageFlags = RT_ALL_STAGES, .size = bindless_words * 4 };
        VkPipelineLayoutCreateInfo info = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                            .setLayoutCount = set_count, .pSetLayouts = set_layouts,
                                            .pushConstantRangeCount = bindless_words ? 1u : 0u, .pPushConstantRanges = &range };
        if (device->vk.CreatePipelineLayout(device->device, &info, NULL, &layout) != VK_SUCCESS) goto done;
    }

    /* 3. modules, stages and groups */
    for (uint32_t i = 0; i < shader_count; ++i) {
        VkShaderModuleCreateInfo info = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                          .codeSize = shaders[i].spirv_size, .pCode = (const uint32_t *)shaders[i].spirv };
        if (device->vk.CreateShaderModule(device->device, &info, NULL, &shaders[i].module) != VK_SUCCESS) {
            fprintf(stderr, "[BD12 RT] shader module for %s rejected\n", shaders[i].name);
            goto done;
        }
        stages[i] = (VkPipelineShaderStageCreateInfo){ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                       .stage = vk_stage(shaders[i].stage), .module = shaders[i].module, .pName = "main" };
    }
    p->group_names = calloc(96, sizeof(*p->group_names));
    if (!p->group_names) goto done;
    for (uint32_t i = 0; i < shader_count && group_count < 96; ++i) {
        const uint32_t st = shaders[i].stage;
        if (st != 9 && st != 11 && st != 12) continue;       /* general groups: miss, raygen, callable */
        groups[group_count] = (VkRayTracingShaderGroupCreateInfoKHR){
            .sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR, .type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,
            .generalShader = i, .closestHitShader = VK_SHADER_UNUSED_KHR, .anyHitShader = VK_SHADER_UNUSED_KHR,
            .intersectionShader = VK_SHADER_UNUSED_KHR };
        snprintf(p->group_names[group_count++], 128, "%s", shaders[i].name);
    }
    for (uint32_t h = 0; h < d->hit_group_count && group_count < 96; ++h) {
        const BD12RtHitGroup *hg = &d->hit_groups[h];
        groups[group_count] = (VkRayTracingShaderGroupCreateInfoKHR){
            .sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
            .type = hg->type ? VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR : VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR,
            .generalShader = VK_SHADER_UNUSED_KHR, .closestHitShader = VK_SHADER_UNUSED_KHR, .anyHitShader = VK_SHADER_UNUSED_KHR,
            .intersectionShader = VK_SHADER_UNUSED_KHR };
        int index;
        if ((index = find_shader(shaders, shader_count, hg->closest_hit)) >= 0) groups[group_count].closestHitShader = (uint32_t)index;
        if ((index = find_shader(shaders, shader_count, hg->any_hit)) >= 0) groups[group_count].anyHitShader = (uint32_t)index;
        if ((index = find_shader(shaders, shader_count, hg->intersection)) >= 0) groups[group_count].intersectionShader = (uint32_t)index;
        snprintf(p->group_names[group_count++], 128, "%s", hg->name);
    }
    {
        VkRayTracingPipelineCreateInfoKHR info = {
            .sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR,
            .stageCount = shader_count, .pStages = stages, .groupCount = group_count, .pGroups = groups,
            .maxPipelineRayRecursionDepth = d->max_recursion ? (d->max_recursion < device->rt_max_recursion ? d->max_recursion : device->rt_max_recursion) : 1,
            .layout = layout
        };
        const VkResult r = device->vk_rt.CreateRayTracingPipelinesKHR(device->device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &info, NULL, &pipeline);
        if (r != VK_SUCCESS) { fprintf(stderr, "[BD12 RT] vkCreateRayTracingPipelinesKHR failed: %d\n", (int)r); goto done; }
    }
    p->handle_size = device->rt_handle_size;
    p->handles = malloc((size_t)group_count * p->handle_size);
    if (!p->handles || device->vk_rt.GetRayTracingShaderGroupHandlesKHR(device->device, pipeline, 0, group_count,
                                                                         (size_t)group_count * p->handle_size, p->handles) != VK_SUCCESS)
        goto done;
    p->group_count = group_count;
    p->base.device = device;
    p->base.layout = layout;
    p->base.pipeline = pipeline;
    p->base.set_layout = set_layout;
    p->base.bindings = copy;
    p->base.binding_types = types;
    p->base.binding_count = copy_count;
    p->base.bindless = bindless;
    p->base.bindless_count = bindless_count;
    p->base.bindless_words = bindless_words;
    copy = NULL; types = NULL; bindless = NULL;
    set_layout = VK_NULL_HANDLE; layout = VK_NULL_HANDLE; pipeline = VK_NULL_HANDLE;
    ok = 1;
    if (getenv("BEER_DXR_PROBE")) {
        fprintf(stderr, "[BD12 RT] pipeline %p built: %u shaders, %u groups, %u descriptors (%u bindless words)\n", (void *)p, shader_count, group_count, copy_count, bindless_words);
        if (getenv("BEER_DXR_LIST") && copy_count <= 6) {
            for (uint32_t i = 0; i < shader_count; ++i)
                fprintf(stderr, "[BD12 RT]   shader %s stage=%u\n", shaders[i].name, shaders[i].stage);
            for (uint32_t i = 0; i < p->base.binding_count; ++i)
                fprintf(stderr, "[BD12 RT]   binding type=%u space=%u reg=%u count=%u accel=%u\n", p->base.bindings[i].type,
                        p->base.bindings[i].space, p->base.bindings[i].reg, p->base.bindings[i].count, p->base.bindings[i].is_accel);
        }
    }
done:
    for (uint32_t i = 0; i < shader_count; ++i) {
        if (shaders[i].module) device->vk.DestroyShaderModule(device->device, shaders[i].module, NULL);
        free(shaders[i].spirv);
    }
    if (pipeline) device->vk.DestroyPipeline(device->device, pipeline, NULL);
    if (layout) device->vk.DestroyPipelineLayout(device->device, layout, NULL);
    if (set_layout) device->vk.DestroyDescriptorSetLayout(device->device, set_layout, NULL);
    free(copy); free(bindless); free(types); free(layout_bindings);
    free(shared);
    return ok;
}

static int ensure_built(BD12RtPipeline *p)
{
    pthread_mutex_lock(&p->lock);
    if (!p->built) { p->built = 1; p->ok = build(p); }
    const int ok = p->ok;
    pthread_mutex_unlock(&p->lock);
    return ok;
}

const uint8_t *bd12_rt_pipeline_identifier(BD12RtPipeline *p, const char *name)
{
    if (!p || !name || !ensure_built(p)) return NULL;
    for (uint32_t i = 0; i < p->group_count; ++i)
        if (!strcmp(p->group_names[i], name)) return p->handles + (size_t)i * p->handle_size;
    return NULL;
}

/* D3D12_DISPATCH_RAYS_DESC: raygen record {va, size}, then miss, hit group and callable tables {va, size, stride}, then
 * width, height, depth. */
int bd12_cmd_dispatch_rays(BD12CmdList *list, BD12RtPipeline *p, const uint8_t *desc)
{
    bd12_canary_check("DispatchRays entry");
    if (!p || !desc || !ensure_built(p)) return 0;
    { static unsigned shown; if (shown < 40 && getenv("BEER_DXR_PROBE")) { ++shown; fprintf(stderr, "[DXR] dispatch %ux%ux%u with pipeline %p\n", *(const uint32_t *)(desc + 88), *(const uint32_t *)(desc + 92), *(const uint32_t *)(desc + 96), (void *)p); } }
    { static void *last; if (last != (void *)p && getenv("BEER_DXR_PROBE")) { last = p; fprintf(stderr, "[DXR] dispatching pipeline %p (%u bindings, %u bindless words)\n", (void *)p, p->base.binding_count, p->base.bindless_words); } }
    bd12_canary_check("RT pipeline build");
    BD12Device *device = list->device;
    BD12ComputePipeline *pso = &p->base;
    if (!list->compute.root_signature && !list->resolve_hook) return 0;
    uint64_t v[11];
    memcpy(v, desc, sizeof(v));          /* 88 bytes */
    uint32_t dims[3];
    memcpy(dims, desc + 88, sizeof(dims));
    if (!dims[0] || !dims[1] || !dims[2]) return 1;
    bd12_cmd_end_rendering(list);
    /* Capture (BEER_BD12_CAPTURE_DIR + BEER_BD12_CAPTURE_CS=d7a0 + a traced frame): buffers and storage images bound to
     * the dispatch, before and after, like compute shaders. */
    static const uint64_t capture_hash = 0xd7a0000000000000ull;
    const int capture = bd12_capture_wants(capture_hash);
    if (capture) bd12_capture_begin();
    VkDescriptorSet set = bd12_resolve_descriptor_set(list, &list->compute, pso->bindings, pso->binding_types, pso->binding_count, pso->set_layout);
    if (capture) bd12_capture_record(list, 0, capture_hash, (uint32_t)(v[0] ? dims[0] : 0), dims[1], dims[2]);
    bd12_canary_check("DispatchRays descriptor resolve");
    if (!set || (pso->bindless_words && !bd12_mirror_prepare(list))) {
        if (capture) bd12_capture_end();
        return 0;
    }
    device->vk.CmdBindPipeline(list->command_buffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pso->pipeline);
    device->vk.CmdBindDescriptorSets(list->command_buffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pso->layout, 0, 1, &set, 0, NULL);
    if (pso->bindless_words)
        bd12_cmd_bind_bindless(list, &list->compute, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pso->layout, RT_ALL_STAGES,
                               pso->bindless, pso->bindless_count, pso->bindless_words);
    VkStridedDeviceAddressRegionKHR raygen = { v[0], v[1], v[1] };
    VkStridedDeviceAddressRegionKHR miss = { v[2], v[4], v[3] };
    VkStridedDeviceAddressRegionKHR hit = { v[5], v[7], v[6] };
    VkStridedDeviceAddressRegionKHR callable = { v[8], v[10], v[9] };
    bd12_canary_check("DispatchRays bind");
    device->vk_rt.CmdTraceRaysKHR(list->command_buffer, &raygen, &miss, &hit, &callable, dims[0], dims[1], dims[2]);
    bd12_canary_check("vkCmdTraceRays");
    if (capture) {
        bd12_capture_record(list, 1, capture_hash, dims[0], dims[1], dims[2]);
        bd12_capture_end();
    }
    return 1;
}
