#define _GNU_SOURCE
#include "bd12.h"
#include "bd12_pso.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pipelines ------------------------------------------------------------------- */

static VkShaderModule make_module(BD12Device *device, const BD12ShaderCode *code)
{
    VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = code->spirv_bytes,
        .pCode = code->spirv
    };
    VkShaderModule module = VK_NULL_HANDLE;
    if (device->vk.CreateShaderModule(device->device, &info, NULL, &module) !=
        VK_SUCCESS)
        return VK_NULL_HANDLE;
    return module;
}

/* ---- Shared layouts ----
 * Engines create tens of thousands of pipelines over a few hundred distinct binding shapes. One descriptor-set
 * layout and pipeline layout per pipeline multiplied the driver objects (and their memory) by the pipeline count;
 * identical ones are now shared and live as long as the device. A key is the layout's full description as words;
 * equal keys are equal layouts (Vulkan layouts are compared by definition, so sharing a handle changes nothing a
 * pipeline or descriptor set can observe). */
typedef struct { BD12Device *device; uint64_t hash; uint64_t *key; uint32_t words; uint64_t handle; } SharedLayout;
static SharedLayout *g_shared_layouts;
static uint32_t g_shared_layout_count, g_shared_layout_capacity;
static pthread_mutex_t g_shared_layout_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t shared_key_hash(const uint64_t *key, uint32_t words)
{
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    for (uint32_t i = 0; i < words; ++i) { h ^= key[i]; h *= UINT64_C(0x100000001b3); h ^= h >> 31; }
    return h ? h : 1;
}
/* Looks the key up; on a miss calls create() and remembers the result. kind separates the two layout kinds. */
static uint64_t shared_layout_get(BD12Device *device, uint64_t *key, uint32_t words,
                                  uint64_t (*create)(BD12Device *, const void *), const void *info)
{
    static int off = -1;                    /* BEER_BD12_SHARED_LAYOUTS=0 (diagnostic): one new layout per call */
    if (off < 0) { const char *v = getenv("BEER_BD12_SHARED_LAYOUTS"); off = v && !strcmp(v, "0"); }
    if (off) return create(device, info);
    const uint64_t hash = shared_key_hash(key, words);
    pthread_mutex_lock(&g_shared_layout_lock);
    for (uint32_t i = 0; i < g_shared_layout_count; ++i) {
        const SharedLayout *e = &g_shared_layouts[i];
        if (e->device == device && e->hash == hash && e->words == words &&
            !memcmp(e->key, key, words * sizeof(*key))) {
            const uint64_t handle = e->handle;
            pthread_mutex_unlock(&g_shared_layout_lock);
            return handle;
        }
    }
    const uint64_t handle = create(device, info);
    if (handle && g_shared_layout_count == g_shared_layout_capacity) {
        const uint32_t capacity = g_shared_layout_capacity ? g_shared_layout_capacity * 2 : 256;
        SharedLayout *grown = realloc(g_shared_layouts, capacity * sizeof(*grown));
        if (grown) { g_shared_layouts = grown; g_shared_layout_capacity = capacity; }
    }
    if (handle && g_shared_layout_count < g_shared_layout_capacity) {
        uint64_t *copy = malloc(words * sizeof(*copy));
        if (copy) {
            memcpy(copy, key, words * sizeof(*copy));
            g_shared_layouts[g_shared_layout_count++] = (SharedLayout){ device, hash, copy, words, handle };
            if (getenv("BEER_BD12_PIPELIB_LOG") && (g_shared_layout_count == 1 || !(g_shared_layout_count % 200)))
                fprintf(stderr, "[BD12] shared layouts: %u distinct\n", g_shared_layout_count);
        }
    }
    pthread_mutex_unlock(&g_shared_layout_lock);
    return handle;
}
static uint64_t create_set_layout(BD12Device *device, const void *info)
{
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    if (device->vk.CreateDescriptorSetLayout(device->device, info, NULL, &layout) != VK_SUCCESS) return 0;
    return (uint64_t)layout;
}
static uint64_t create_pipeline_layout(BD12Device *device, const void *info)
{
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (device->vk.CreatePipelineLayout(device->device, info, NULL, &layout) != VK_SUCCESS) return 0;
    return (uint64_t)layout;
}

VkDescriptorSetLayout bd12_shared_set_layout(BD12Device *device, const VkDescriptorSetLayoutCreateInfo *info)
{
    const VkDescriptorBindingFlags *flags = NULL;
    for (const VkBaseInStructure *next = info->pNext; next; next = next->pNext)
        if (next->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO) {
            const VkDescriptorSetLayoutBindingFlagsCreateInfo *f = (const void *)next;
            if (f->bindingCount == info->bindingCount) flags = f->pBindingFlags;
        } else {
            VkDescriptorSetLayout layout = VK_NULL_HANDLE;              /* an extension the key doesn't cover */
            return device->vk.CreateDescriptorSetLayout(device->device, info, NULL, &layout) == VK_SUCCESS
                   ? layout : VK_NULL_HANDLE;
        }
    uint32_t samplers = 0;
    for (uint32_t i = 0; i < info->bindingCount; ++i)
        if (info->pBindings[i].pImmutableSamplers) samplers += info->pBindings[i].descriptorCount;
    const uint32_t words = 3 + info->bindingCount * 3 + samplers;
    uint64_t stack_key[512], *key = words <= 512 ? stack_key : malloc(words * sizeof(*key));
    if (!key) return VK_NULL_HANDLE;
    uint32_t n = 0;
    key[n++] = 1;                                                         /* kind: set layout */
    key[n++] = info->flags;
    key[n++] = info->bindingCount;
    for (uint32_t i = 0; i < info->bindingCount; ++i) {
        const VkDescriptorSetLayoutBinding *b = &info->pBindings[i];
        key[n++] = (uint64_t)b->binding << 32 | (uint32_t)b->descriptorType;
        key[n++] = (uint64_t)b->descriptorCount << 32 | b->stageFlags;
        key[n++] = (uint64_t)(flags ? flags[i] : 0) << 1 | (b->pImmutableSamplers != NULL);
        for (uint32_t k = 0; b->pImmutableSamplers && k < b->descriptorCount; ++k)
            key[n++] = (uint64_t)b->pImmutableSamplers[k];
    }
    const uint64_t handle = shared_layout_get(device, key, n, create_set_layout, info);
    if (key != stack_key) free(key);
    return (VkDescriptorSetLayout)handle;
}

VkPipelineLayout bd12_shared_pipeline_layout(BD12Device *device, const VkPipelineLayoutCreateInfo *info)
{
    uint64_t key[3 + 8 + 2 * 4];
    if (info->pNext || info->setLayoutCount > 8 || info->pushConstantRangeCount > 4) {
        VkPipelineLayout layout = VK_NULL_HANDLE;
        return device->vk.CreatePipelineLayout(device->device, info, NULL, &layout) == VK_SUCCESS
               ? layout : VK_NULL_HANDLE;
    }
    uint32_t n = 0;
    key[n++] = 2;                                                         /* kind: pipeline layout */
    key[n++] = info->flags;
    key[n++] = (uint64_t)info->setLayoutCount << 32 | info->pushConstantRangeCount;
    for (uint32_t i = 0; i < info->setLayoutCount; ++i) key[n++] = (uint64_t)info->pSetLayouts[i];
    for (uint32_t i = 0; i < info->pushConstantRangeCount; ++i) {
        const VkPushConstantRange *r = &info->pPushConstantRanges[i];
        key[n++] = (uint64_t)r->stageFlags << 32 | r->offset;
        key[n++] = r->size;
    }
    return (VkPipelineLayout)shared_layout_get(device, key, n, create_pipeline_layout, info);
}

BD12GraphicsPipeline *bd12_graphics_pipeline_create(
    BD12Device *device, const BD12GraphicsPipelineDesc *desc)
{
    const int mesh = desc->ms.spirv != NULL;
    /* A mesh pipeline has a mesh shader (and optionally an amplification shader) instead of every vertex stage. */
    if (mesh ? (desc->vs.spirv || desc->hs.spirv || desc->ds.spirv || desc->gs.spirv || !device->CmdDrawMeshTasksEXT)
             : (!desc->vs.spirv || desc->as.spirv))
        return NULL;
    if (desc->rt_count > BD12_MAX_RENDER_TARGETS) return NULL;
    if (!desc->hs.spirv != !desc->ds.spirv) return NULL;
    BD12GraphicsPipeline *gp = calloc(1, sizeof(*gp));
    if (!gp) return NULL;
    gp->device = device;
    /* Stable diagnostic identity; no shader-specific behavior depends on it. */
    uint64_t hash = UINT64_C(1469598103934665603);
    /* Pipeline stages in Vulkan order; absent ones have no SPIR-V. */
    enum { STAGES = 7 };
    const BD12ShaderCode *stages[STAGES] = { &desc->vs, &desc->hs, &desc->ds, &desc->gs, &desc->ps,
                                             &desc->as, &desc->ms };
    static const VkShaderStageFlagBits stage_bits[STAGES] = {
        VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
        VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, VK_SHADER_STAGE_GEOMETRY_BIT,
        VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_TASK_BIT_EXT, VK_SHADER_STAGE_MESH_BIT_EXT
    };
    const BD12ShaderCode *const *hash_stages = stages;
    for (int s = 0; s < STAGES; ++s) {
        const uint8_t *bytes = (const uint8_t *)hash_stages[s]->spirv;
        for (size_t i = 0; bytes && i < hash_stages[s]->spirv_bytes; ++i) {
            hash ^= bytes[i];
            hash *= UINT64_C(1099511628211);
        }
    }
    gp->shader_hash = hash;
    if (getenv("BEER_BD12_PSO_LOG"))
        fprintf(stderr, "[PSO] %016llx topo=%d fill=%d cull=%d ccw=%d clip=%d bias=%d depth=%d/%d func=%d "
                "stencil=%d rt=%u blend0=%d wm0=%x ds=%d samples=%u\n",
                (unsigned long long)hash, (int)desc->topology, (int)desc->fill_mode,
                (int)desc->cull_mode, desc->front_ccw, desc->depth_clip_enable,
                desc->depth_bias_enable, desc->depth_test, desc->depth_write,
                (int)desc->depth_func, desc->stencil_test, desc->rt_count,
                desc->blend[0].blend_enable, desc->blend[0].write_mask,
                (int)desc->ds_format, desc->samples);

    /* Merge every stage's reflected bindings into one set-0 layout. */
    uint32_t total = 0;
    for (int s = 0; s < STAGES; ++s)
        if (stages[s]->spirv) {
            total += stages[s]->binding_count;
            gp->stages |= stage_bits[s];
        }
    gp->tessellated = desc->hs.spirv != NULL;
    gp->mesh = mesh;
    VkDescriptorSetLayoutBinding *layout_bindings =
        calloc(total ? total : 1, sizeof(*layout_bindings));
    gp->bindings = calloc(total ? total : 1, sizeof(*gp->bindings));
    gp->binding_types = calloc(total ? total : 1, sizeof(*gp->binding_types));
    VkShaderModule modules[STAGES] = { VK_NULL_HANDLE };
    BD12ShaderBinding *stage_binds[STAGES] = { NULL };
    uint32_t stage_kept[STAGES] = { 0 };
    gp->bindless = calloc(total ? total : 1, sizeof(*gp->bindless));
    if (!gp->bindless) goto fail;
    uint32_t *dense_spirv[STAGES] = { NULL };
    uint32_t next_binding = 0;
    /* Descriptor-buffer pipelines (bd12_heapdb.c) read everything through the heaps, static samplers and push
     * constants of a device-owned layout: no set 0 of their own, SPIR-V used as translated. */
    const int db = desc->db_layout != NULL;
    if (!layout_bindings || !gp->bindings || !gp->binding_types) goto fail;
    for (int s = 0; s < STAGES; ++s) {
        if (!stages[s]->spirv || db) continue;
        const uint32_t n = stages[s]->binding_count;
        stage_binds[s] = calloc(n ? n : 1, sizeof(*stage_binds[s]));
        if (!stage_binds[s]) goto fail;
        /* Heap-array bindings leave set 0 for the heap mirror (set 1). */
        for (uint32_t i = 0; i < n; ++i) {
            if (stages[s]->bindings[i].bindless) {
                gp->bindless[gp->bindless_count++] = stages[s]->bindings[i];
                if (stages[s]->bindings[i].push_word + 1 > gp->bindless_words)
                    gp->bindless_words = stages[s]->bindings[i].push_word + 1;
            } else {
                stage_binds[s][stage_kept[s]++] = stages[s]->bindings[i];
            }
        }
        dense_spirv[s] = bd12_densify_bindings(stages[s]->spirv,
                                               stages[s]->spirv_bytes,
                                               stage_binds[s], stage_kept[s],
                                               &next_binding);
        if (!dense_spirv[s]) goto fail;
    }
    for (int s = 0; s < STAGES && !db; ++s) {
        for (uint32_t i = 0; stages[s]->spirv && i < stage_kept[s]; ++i) {
            BD12ShaderBinding b = stage_binds[s][i];
            VkDescriptorType type;
            if (!bd12_binding_descriptor_type(&b, &type)) goto fail;
            if (!b.count) b.count = 1;
            layout_bindings[gp->binding_count] = (VkDescriptorSetLayoutBinding){
                .binding = b.binding,
                .descriptorType = type,
                .descriptorCount = b.count,
                .stageFlags = stage_bits[s]
            };
            gp->bindings[gp->binding_count] = b;
            gp->binding_types[gp->binding_count] = type;
            ++gp->binding_count;
        }
    }
    gp->root_push_bytes = desc->root_push_bytes;
    gp->root_layout_hash = desc->root_layout_hash;
    if (db) {
        gp->db_layout = desc->db_layout;
        gp->layout = bd12_db_pipeline_layout(desc->db_layout);   /* owned by the device */
        goto layout_done;
    }
    VkDescriptorBindingFlags *binding_flags = calloc(gp->binding_count ? gp->binding_count : 1,
                                                     sizeof(*binding_flags));
    VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info;
    VkDescriptorSetLayoutCreateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = binding_flags ? bd12_layout_partial_arrays(device, layout_bindings, gp->binding_count,
                                                            binding_flags, &flags_info) : NULL,
        .bindingCount = gp->binding_count,
        .pBindings = layout_bindings
    };
    gp->set_layout = bd12_shared_set_layout(device, &set_info);
    free(binding_flags);
    if (!gp->set_layout) goto fail;
    VkDescriptorSetLayout set_layouts[2] = { gp->set_layout, VK_NULL_HANDLE };
    /* Root-signature pipelines (bd12_rootpush.c): the push block holds the root arguments, set 1 is the heap
     * mirror, set 0 only static samplers. */
    const int uses_mirror = gp->bindless_words || gp->root_push_bytes;
    VkPushConstantRange push_range = {
        .stageFlags = gp->stages,
        .size = gp->root_push_bytes ? gp->root_push_bytes : gp->bindless_words * 4
    };
    if (uses_mirror) {
        set_layouts[1] = bd12_mirror_layout(device);
        if (!set_layouts[1]) goto fail;
    }
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = uses_mirror ? 2u : 1u,
        .pSetLayouts = set_layouts,
        .pushConstantRangeCount = uses_mirror ? 1u : 0u,
        .pPushConstantRanges = &push_range
    };
    if (!(gp->layout = bd12_shared_pipeline_layout(device, &layout_info)))
        goto fail;
layout_done:;

    VkPipelineShaderStageCreateInfo shader_stages[STAGES];
    uint32_t stage_count = 0;
    for (int s = 0; s < STAGES; ++s) {
        if (!stages[s]->spirv) continue;
        BD12ShaderCode dense = *stages[s];
        if (!db) dense.spirv = dense_spirv[s];
        modules[s] = make_module(device, &dense);
        if (!modules[s]) goto fail;
        shader_stages[stage_count++] = (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = stage_bits[s],
            .module = modules[s],
            .pName = "main"
        };
    }
    for (int i = 0; i < STAGES; ++i) {
        free(dense_spirv[i]); dense_spirv[i] = NULL;
        free(stage_binds[i]); stage_binds[i] = NULL;
    }

    /* Vertex input: strides are dynamic (they come from the vertex buffer
     * views), so only slots and attributes are baked. */
    VkVertexInputBindingDescription vertex_bindings[BD12_MAX_VERTEX_BUFFERS];
    uint32_t vertex_binding_count = 0;
    VkVertexInputAttributeDescription attributes[32];
    if (desc->attribute_count > 32) goto fail;
    for (uint32_t i = 0; i < desc->attribute_count; ++i) {
        const BD12VertexAttribute *a = &desc->attributes[i];
        if (a->binding >= BD12_MAX_VERTEX_BUFFERS) goto fail;
        attributes[i] = (VkVertexInputAttributeDescription){
            a->location, a->binding, a->format, a->offset };
        uint32_t j;
        for (j = 0; j < vertex_binding_count; ++j)
            if (vertex_bindings[j].binding == a->binding) break;
        if (j == vertex_binding_count) {
            vertex_bindings[vertex_binding_count++] =
                (VkVertexInputBindingDescription){
                    a->binding, 0, a->per_instance ?
                        VK_VERTEX_INPUT_RATE_INSTANCE :
                        VK_VERTEX_INPUT_RATE_VERTEX };
        }
        if (a->binding + 1 > gp->vertex_slot_count)
            gp->vertex_slot_count = a->binding + 1;
    }
    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = vertex_binding_count,
        .pVertexBindingDescriptions = vertex_bindings,
        .vertexAttributeDescriptionCount = desc->attribute_count,
        .pVertexAttributeDescriptions = attributes
    };
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = desc->topology,
        .primitiveRestartEnable = desc->primitive_restart
    };
    VkPipelineTessellationStateCreateInfo tessellation = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
        .patchControlPoints = desc->patch_control_points ? desc->patch_control_points : 3
    };
    VkPipelineViewportStateCreateInfo viewport = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO
    };
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = !desc->depth_clip_enable,
        .polygonMode = desc->fill_mode,
        .cullMode = desc->cull_mode,
        .frontFace = desc->front_ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE :
                                       VK_FRONT_FACE_CLOCKWISE,
        .depthBiasEnable = desc->depth_bias_enable,
        .depthBiasConstantFactor = desc->depth_bias,
        .depthBiasClamp = desc->depth_bias_clamp,
        .depthBiasSlopeFactor = desc->slope_scaled_bias,
        .lineWidth = 1.0f
    };
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = desc->samples ?
            (VkSampleCountFlagBits)desc->samples : VK_SAMPLE_COUNT_1_BIT,
        .alphaToCoverageEnable = desc->alpha_to_coverage
    };
    VkPipelineDepthStencilStateCreateInfo depth_stencil = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = desc->depth_test,
        .depthWriteEnable = desc->depth_write,
        .depthCompareOp = desc->depth_func,
        .stencilTestEnable = desc->stencil_test,
        .front = desc->stencil_front,
        .back = desc->stencil_back
    };
    VkPipelineColorBlendAttachmentState blend_attachments[BD12_MAX_RENDER_TARGETS];
    VkBool32 any_logic = VK_FALSE;
    VkLogicOp logic_op = VK_LOGIC_OP_COPY;
    for (uint32_t i = 0; i < desc->rt_count; ++i) {
        const BD12BlendTarget *t = &desc->blend[i];
        blend_attachments[i] = (VkPipelineColorBlendAttachmentState){
            .blendEnable = t->blend_enable,
            .srcColorBlendFactor = t->src_color,
            .dstColorBlendFactor = t->dst_color,
            .colorBlendOp = t->color_op,
            .srcAlphaBlendFactor = t->src_alpha,
            .dstAlphaBlendFactor = t->dst_alpha,
            .alphaBlendOp = t->alpha_op,
            .colorWriteMask = t->write_mask
        };
        if (t->logic_enable) { any_logic = VK_TRUE; logic_op = t->logic_op; }
    }
    VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = any_logic,
        .logicOp = logic_op,
        .attachmentCount = desc->rt_count,
        .pAttachments = blend_attachments
    };
    VkDynamicState dynamic_states[7] = {
        VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT,
        VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE,
        VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY
    };
    /* Vertex input and topology do not exist in a mesh pipeline and must not be dynamic there. */
    uint32_t dynamic_count = mesh ? 4 : 6;
    /* VRS: the per-draw rate and combiners are dynamic in every pipeline, so RSSetShadingRate never needs a new
     * pipeline; with a rate image (tier 2) every pipeline may also render with the shading-rate attachment. */
    const int vrs = device->CmdSetFragmentShadingRateKHR != NULL;
    const int vrs_image = vrs && device->modern->vrs_tier >= 2;
    if (vrs) dynamic_states[dynamic_count++] = VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR;
    VkPipelineDynamicStateCreateInfo dynamic = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = dynamic_count,
        .pDynamicStates = dynamic_states
    };
    const VkImageAspectFlags ds_aspect =
        desc->ds_format == VK_FORMAT_UNDEFINED ? 0 :
        (desc->ds_format == VK_FORMAT_S8_UINT ? VK_IMAGE_ASPECT_STENCIL_BIT :
         (desc->ds_format == VK_FORMAT_D16_UNORM ||
          desc->ds_format == VK_FORMAT_D32_SFLOAT ||
          desc->ds_format == VK_FORMAT_X8_D24_UNORM_PACK32) ?
             VK_IMAGE_ASPECT_DEPTH_BIT :
             (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT));
    VkPipelineRenderingCreateInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = desc->rt_count,
        .pColorAttachmentFormats = desc->rt_formats,
        .depthAttachmentFormat = (ds_aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ?
            desc->ds_format : VK_FORMAT_UNDEFINED,
        .stencilAttachmentFormat = (ds_aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ?
            desc->ds_format : VK_FORMAT_UNDEFINED
    };
    VkGraphicsPipelineCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering,
        .flags = (db ? VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT : 0) |
                 (vrs_image ? VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR : 0),
        .stageCount = stage_count,
        .pStages = shader_stages,
        .pVertexInputState = mesh ? NULL : &vertex_input,
        .pInputAssemblyState = mesh ? NULL : &input_assembly,
        .pTessellationState = gp->tessellated ? &tessellation : NULL,
        .pViewportState = &viewport,
        .pRasterizationState = &raster,
        .pMultisampleState = &multisample,
        .pDepthStencilState = &depth_stencil,
        .pColorBlendState = &blend,
        .pDynamicState = &dynamic,
        .layout = gp->layout
    };
    VkResult result = device->vk.CreateGraphicsPipelines(
        device->device, device->pipeline_cache, 1, &info, NULL, &gp->pipeline);
    if (result == VK_SUCCESS) bd12_pipeline_cache_note_new(device);
    beer_vulkan_record_result(result, "BD12 graphics pipeline");
    if (result != VK_SUCCESS) goto fail;
    for (int i = 0; i < STAGES; ++i)
        if (modules[i])
            device->vk.DestroyShaderModule(device->device, modules[i], NULL);
    free(layout_bindings);
    return gp;

fail:
    for (int i = 0; i < STAGES; ++i) {
        free(dense_spirv[i]);
        free(stage_binds[i]);
    }
    for (int i = 0; i < STAGES; ++i)
        if (modules[i])
            device->vk.DestroyShaderModule(device->device, modules[i], NULL);
    free(layout_bindings);
    bd12_graphics_pipeline_destroy(gp);
    return NULL;
}

void bd12_graphics_pipeline_destroy(BD12GraphicsPipeline *gp)
{
    if (!gp) return;
    BD12Device *device = gp->device;
    if (gp->pipeline)
        device->vk.DestroyPipeline(device->device, gp->pipeline, NULL);
    /* gp->layout and gp->set_layout are shared (bd12_shared_*_layout) or owned by the descriptor-buffer layout. */
    free(gp->bindings);
    free(gp->bindless);
    free(gp->binding_types);
    free(gp);
}

/* State setters ------------------------------------------------------------------ */

void bd12_cmd_set_graphics_root_signature(BD12CmdList *list,
                                          BD12RootSignature *signature)
{
    if (list->graphics.root_signature == signature) return;
    bd12_root_arguments_carry(list->graphics.root_signature, signature, list->graphics.arguments);   /* bd12_bind.c */
    list->graphics.root_signature = signature;
    ++list->graphics.epoch;
}

void bd12_cmd_set_graphics_root_constants(BD12CmdList *list, uint32_t parameter,
                                          const uint32_t *data, uint32_t count,
                                          uint32_t dest_offset)
{
    if (parameter >= BD12_MAX_ROOT_PARAMETERS ||
        dest_offset + count > BD12_MAX_ROOT_CONSTANTS)
        return;
    BD12RootArgument *arg = &list->graphics.arguments[parameter];
    if (arg->cbv_valid && !memcmp(&arg->constants[dest_offset], data, count * sizeof(uint32_t))) return;
    memcpy(&arg->constants[dest_offset], data, count * sizeof(uint32_t));
    arg->cbv_valid = 0;
    ++list->graphics.epoch;
    BD12_ROOT_CHANGED(0, 0);
}

void bd12_cmd_set_graphics_root_va(BD12CmdList *list, uint32_t parameter,
                                   uint64_t gpu_va)
{
    if (parameter < BD12_MAX_ROOT_PARAMETERS && list->graphics.arguments[parameter].value != gpu_va) {
        list->graphics.arguments[parameter].value = gpu_va;
        ++list->graphics.epoch;
        BD12_ROOT_CHANGED(0, 1);
    }
}

void bd12_cmd_set_graphics_root_table(BD12CmdList *list, uint32_t parameter,
                                      uint64_t gpu_handle)
{
    if (parameter < BD12_MAX_ROOT_PARAMETERS && list->graphics.arguments[parameter].value != gpu_handle) {
        list->graphics.arguments[parameter].value = gpu_handle;
        ++list->graphics.epoch;
        BD12_ROOT_CHANGED(0, 2);
    }
}

void bd12_cmd_set_graphics_pipeline(BD12CmdList *list,
                                    BD12GraphicsPipeline *pipeline)
{
    if (list->gfx.pipeline != pipeline) list->gfx.dirty |= BD12_DIRTY_PIPELINE;
    list->gfx.pipeline = pipeline;
}

void bd12_cmd_set_render_targets(BD12CmdList *list,
                                 const BD12Descriptor *const *rtvs,
                                 uint32_t rtv_count, const BD12Descriptor *dsv)
{
    BD12GfxState *g = &list->gfx;
    if (rtv_count > BD12_MAX_RENDER_TARGETS) rtv_count = BD12_MAX_RENDER_TARGETS;
    bd12_cmd_end_rendering(list);
    g->rtv_count = rtv_count;
    memset(g->rtv, 0, sizeof(g->rtv));
    for (uint32_t i = 0; i < rtv_count; ++i)
        if (rtvs[i]) g->rtv[i] = *rtvs[i];
    g->has_dsv = dsv && dsv->kind == BD12_DESC_DSV;
    if (g->has_dsv) g->dsv = *dsv;
}

void bd12_cmd_set_viewports(BD12CmdList *list, const VkViewport *viewports,
                            uint32_t count)
{
    BD12GfxState *g = &list->gfx;
    if (count > BD12_MAX_VIEWPORTS) count = BD12_MAX_VIEWPORTS;
    for (uint32_t i = 0; i < count; ++i) {
        /* D3D clip space has +y up; a negative-height viewport (core since
         * VK_KHR_maintenance1) flips Vulkan's y-down NDC accordingly. */
        g->viewports[i] = viewports[i];
        g->viewports[i].y = viewports[i].y + viewports[i].height;
        g->viewports[i].height = -viewports[i].height;
    }
    g->viewport_count = count;
    g->dirty |= BD12_DIRTY_VIEWPORT;
}

void bd12_cmd_set_scissors(BD12CmdList *list, const VkRect2D *rects,
                           uint32_t count)
{
    BD12GfxState *g = &list->gfx;
    if (count > BD12_MAX_VIEWPORTS) count = BD12_MAX_VIEWPORTS;
    memcpy(g->scissors, rects, count * sizeof(*rects));
    g->scissor_count = count;
    g->dirty |= BD12_DIRTY_SCISSOR;
}

void bd12_cmd_set_topology(BD12CmdList *list, VkPrimitiveTopology topology)
{
    list->gfx.topology = topology;
    list->gfx.patch_points = 0;
}

void bd12_cmd_set_d3d_topology(BD12CmdList *list, uint32_t d3d_topology)
{
    uint32_t patch = 0;
    VkPrimitiveTopology vk = bd12_topology_to_vk(d3d_topology, &patch);
    bd12_cmd_set_topology(list, vk);
    list->gfx.patch_points = patch;
}

void bd12_cmd_set_vertex_buffers(BD12CmdList *list, uint32_t start,
                                 const uint64_t *gpu_va, const uint32_t *sizes,
                                 const uint32_t *strides, uint32_t count)
{
    BD12GfxState *g = &list->gfx;
    for (uint32_t i = 0; i < count && start + i < BD12_MAX_VERTEX_BUFFERS; ++i) {
        BD12VertexBuffer *vb = &g->vertex_buffers[start + i];
        VkDeviceSize offset = 0;
        BD12Buffer *buffer = gpu_va[i] ?
            bd12_va_resolve(list->device, gpu_va[i], &offset) : NULL;
        vb->buffer = buffer ? buffer->buffer : VK_NULL_HANDLE;
        vb->va = buffer ? gpu_va[i] : 0;
        vb->offset = offset;
        vb->size = buffer ? sizes[i] : 0;
        if (buffer && offset + vb->size > buffer->size)
            vb->size = buffer->size - offset;
        vb->stride = strides[i];
    }
    g->dirty |= BD12_DIRTY_VERTEX;
}

void bd12_cmd_set_index_buffer(BD12CmdList *list, uint64_t gpu_va,
                               uint32_t size, VkIndexType type)
{
    (void)size;
    BD12GfxState *g = &list->gfx;
    VkDeviceSize offset = 0;
    BD12Buffer *buffer = gpu_va ? bd12_va_resolve(list->device, gpu_va, &offset)
                                : NULL;
    g->index_buffer = buffer ? buffer->buffer : VK_NULL_HANDLE;
    g->index_va = buffer ? gpu_va : 0;
    g->index_offset = offset;
    g->index_type = type;
    g->dirty |= BD12_DIRTY_INDEX;
}

void bd12_cmd_set_blend_constants(BD12CmdList *list, const float rgba[4])
{
    memcpy(list->gfx.blend_constants, rgba, sizeof(list->gfx.blend_constants));
    list->gfx.dirty |= BD12_DIRTY_BLEND;
}

void bd12_cmd_set_stencil_ref(BD12CmdList *list, uint32_t ref)
{
    list->gfx.stencil_ref = ref;
    list->gfx.dirty |= BD12_DIRTY_STENCIL;
}

void bd12_cmd_set_shading_rate(BD12CmdList *list, uint32_t rate, const uint32_t *combiners)
{
    if (!list->device->CmdSetFragmentShadingRateKHR) return;
    { static int logged; if (!logged++) fprintf(stderr, "[BD12] first RSSetShadingRate(rate=0x%x, combiners %u,%u)\n",
                                                rate, combiners ? combiners[0] : 0, combiners ? combiners[1] : 0); }
    BD12GfxState *g = &list->gfx;
    g->shading_rate = rate & 0xfu;
    g->shading_combiners[0] = combiners ? combiners[0] : 0;
    g->shading_combiners[1] = combiners ? combiners[1] : 0;
    g->dirty |= BD12_DIRTY_SHADING_RATE;
}

void bd12_cmd_set_shading_rate_image(BD12CmdList *list, BD12Image *image)
{
    BD12Device *device = list->device;
    if (!device->CmdSetFragmentShadingRateKHR || device->modern->vrs_tier < 2) return;
    { static int logged; if (image && !logged++) fprintf(stderr, "[BD12] first RSSetShadingRateImage(%ux%u)\n",
                                                         image->desc.width, image->desc.height); }
    BD12GfxState *g = &list->gfx;
    if (image && !(image->usage & VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR)) {
        static int logged;
        if (!logged++) fprintf(stderr, "[BD12] RSSetShadingRateImage: image is not a 2D R8_UINT texture; ignored\n");
        image = NULL;
    }
    if (image == g->shading_rate_image) return;
    /* The rate image is part of the rendering scope: an open one ends, the next draw begins a new one. */
    bd12_cmd_end_rendering(list);
    g->shading_rate_image = image;
    g->shading_rate_view = VK_NULL_HANDLE;
    if (image) {
        bd12_image_ensure_initialized(list, image);
        g->shading_rate_view = bd12_image_view(image, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8_UINT,
                                               VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);
        if (!g->shading_rate_view) g->shading_rate_image = NULL;
    }
}

/* Rendering scopes ---------------------------------------------------------------- */

void bd12_cmd_end_rendering(BD12CmdList *list)
{
    if (!list->gfx.rendering) return;
    list->device->vk.CmdEndRendering(list->command_buffer);
    list->gfx.rendering = 0;
    if (getenv("BEER_BD12_CAPTURE_DEBUG") && bd12_trace_active()) fprintf(stderr, "[CAPTURE] end rendering cap=%u\n", list->gfx.cap_rtv_count);
    if (list->gfx.cap_rtv_count || list->gfx.cap_has_dsv) {
        /* Render-target dump (BEER_BD12_CAPTURE_RT): the scope just closed. */
        bd12_capture_rt_pass(list);
        list->gfx.cap_rtv_count = 0;
        list->gfx.cap_has_dsv = 0;
    }
}

static void begin_rendering_for(BD12CmdList *list,
                                const BD12Descriptor *rtvs, uint32_t rtv_count,
                                const BD12Descriptor *dsv, int clear_color,
                                const float *color, int clear_depth,
                                int clear_stencil, float depth,
                                uint32_t stencil, int with_shading_rate)
{
    BD12Device *device = list->device;
    VkRenderingAttachmentInfo colors[BD12_MAX_RENDER_TARGETS];
    VkRenderingAttachmentInfo depth_info = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingAttachmentInfo stencil_info = depth_info;
    uint32_t width = UINT32_MAX, height = UINT32_MAX, layers = UINT32_MAX;
    for (uint32_t i = 0; i < rtv_count; ++i) {
        const BD12Descriptor *r = &rtvs[i];
        colors[i] = (VkRenderingAttachmentInfo){
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = r->kind == BD12_DESC_RTV ? r->image_view :
                         VK_NULL_HANDLE,
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = clear_color ? VK_ATTACHMENT_LOAD_OP_CLEAR :
                                    VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE
        };
        if (clear_color) memcpy(colors[i].clearValue.color.float32, color,
                                4 * sizeof(float));
        if (r->kind != BD12_DESC_RTV || colors[i].imageView == VK_NULL_HANDLE) {
            /* A null/unbound RTV slot is legal D3D12; keep the Vulkan slot unused instead of
             * handing the driver a null view with real load/store ops. */
            colors[i].imageView = VK_NULL_HANDLE;
            colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            colors[i].storeOp = VK_ATTACHMENT_STORE_OP_NONE;
            continue;
        }
        bd12_image_ensure_initialized(list, r->image);
        if (r->width < width) width = r->width;
        if (r->height < height) height = r->height;
        if (r->layer_count < layers) layers = r->layer_count;
    }
    if (dsv && dsv->image_view == VK_NULL_HANDLE) dsv = NULL;   /* never pass a null depth view to the driver */
    if (dsv) {
        bd12_image_ensure_initialized(list, dsv->image);
        if (dsv->width < width) width = dsv->width;
        if (dsv->height < height) height = dsv->height;
        if (dsv->layer_count < layers) layers = dsv->layer_count;
        depth_info.imageView = stencil_info.imageView = dsv->image_view;
        depth_info.imageLayout = stencil_info.imageLayout = dsv->image_layout;
        depth_info.storeOp = stencil_info.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth_info.loadOp = clear_depth ? VK_ATTACHMENT_LOAD_OP_CLEAR :
                                          VK_ATTACHMENT_LOAD_OP_LOAD;
        stencil_info.loadOp = clear_stencil ? VK_ATTACHMENT_LOAD_OP_CLEAR :
                                              VK_ATTACHMENT_LOAD_OP_LOAD;
        depth_info.clearValue.depthStencil.depth = depth;
        stencil_info.clearValue.depthStencil.stencil = stencil;
    }
    if (width == UINT32_MAX) { width = height = 1; layers = 1; }
    VkRenderingInfo info = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = { { 0, 0 }, { width, height } },
        .layerCount = layers == UINT32_MAX ? 1 : layers,
        .colorAttachmentCount = rtv_count,
        .pColorAttachments = colors,
        .pDepthAttachment = dsv && (dsv->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ?
                            &depth_info : NULL,
        .pStencilAttachment = dsv && (dsv->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ?
                              &stencil_info : NULL
    };
    /* RSSetShadingRateImage: the rate image covers the render area in tiles. One that is too small for this
     * render area cannot be attached (Vulkan requires full coverage); it is then ignored, i.e. rate 1x1 from it. */
    VkRenderingFragmentShadingRateAttachmentInfoKHR shading_rate = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR
    };
    if (with_shading_rate && list->gfx.shading_rate_view && device->modern->vrs_tile) {
        const BD12Image *image = list->gfx.shading_rate_image;
        const uint32_t tile = device->modern->vrs_tile;
        if (image && (uint64_t)image->desc.width * tile >= info.renderArea.extent.width &&
            (uint64_t)image->desc.height * tile >= info.renderArea.extent.height) {
            shading_rate.imageView = list->gfx.shading_rate_view;
            shading_rate.imageLayout = bd12_layout_for_state(BD12_STATE_SHADING_RATE_SOURCE, 0);
            shading_rate.shadingRateAttachmentTexelSize = (VkExtent2D){ tile, tile };
            info.pNext = &shading_rate;
        }
    }
    if (getenv("BEER_BD12_CAPTURE_DEBUG") && bd12_trace_active()) fprintf(stderr, "[CAPTURE] begin rendering rtv=%u en=%d\n", rtv_count, bd12_capture_rt_enabled());
    if (bd12_capture_rt_enabled()) {
        for (uint32_t i = 0; i < rtv_count && i < BD12_MAX_RENDER_TARGETS; ++i)
            list->gfx.cap_rtv[i] = rtvs[i];
        list->gfx.cap_rtv_count = rtv_count;
        list->gfx.cap_has_dsv = dsv != NULL;
        list->gfx.cap_clear_depth = clear_depth;
        list->gfx.cap_draws = 0;
        if (dsv) list->gfx.cap_dsv = *dsv;
        list->gfx.cap_hash = list->gfx.pipeline ? list->gfx.pipeline->shader_hash : 0;
    }
    g_bd12_cmd_counts[BD12_COUNT_RENDER_PASS]++;
    device->vk.CmdBeginRendering(list->command_buffer, &info);
}

static void begin_draw_rendering(BD12CmdList *list)
{
    BD12GfxState *g = &list->gfx;
    if (g->rendering) return;
    begin_rendering_for(list, g->rtv, g->rtv_count, g->has_dsv ? &g->dsv : NULL,
                        0, NULL, 0, 0, 0.0f, 0, 1);
    g->rendering = 1;
}

/* Draws -------------------------------------------------------------------------------- */

static VkBuffer live_buffer(BD12Device *device, uint64_t va, VkBuffer expected);

static int prepare_draw_state(BD12CmdList *list);

static int prepare_graphics(BD12CmdList *list, int mesh);
static int prepare_draw(BD12CmdList *list) { return prepare_graphics(list, 0); }

/* Everything a draw (mesh = 0) or a mesh dispatch (mesh = 1) needs before the Vulkan draw command. The two only
 * differ in vertex-pipeline state: patch lists, topology and vertex buffers do not exist for mesh pipelines. */
static int prepare_graphics(BD12CmdList *list, int mesh)
{
    BD12Device *device = list->device;
    BD12GfxState *g = &list->gfx;
    g->cap_draws++;
    BD12GraphicsPipeline *gp = g->pipeline;
    if (!gp || (!list->graphics.root_signature && !list->resolve_hook)) {
        BD12_DROP(BD12_DROP_DRAW_NO_STATE);
        { static unsigned logged; if (logged++ < 5) fprintf(stderr, "[BD12] draw dropped: pipeline=%p root_signature=%p resolve_hook=%p\n", (void *)gp, (void *)list->graphics.root_signature, (void *)(uintptr_t)list->resolve_hook); }
        return 0;
    }
    if (gp->mesh != mesh) {
        /* Draw* with a mesh pipeline, or DispatchMesh with a vertex pipeline: invalid D3D12, nothing to run. */
        static int logged;
        if (logged++ < 5)
            fprintf(stderr, "[BD12] %s with a %s pipeline: dropped\n", mesh ? "DispatchMesh" : "draw",
                    gp->mesh ? "mesh" : "vertex");
        BD12_DROP(BD12_DROP_DRAW_NO_STATE);
        return 0;
    }
    if (!mesh) {
    /* A patch list needs the pipeline's hull/domain stages, and those
     * stages need a patch list. A patch list reaching a pipeline without
     * tessellation is invalid Vulkan (Mesa ANV crashes in vkCmdDraw):
     * three-point patches still draw as their untessellated triangles,
     * other patch sizes have no equivalent and their draws are dropped. */
    const int patches = g->topology == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    if (patches != gp->tessellated && (gp->tessellated || g->patch_points != 3)) {
        static uint64_t logged;
        const uint32_t bit = gp->tessellated ? 0 : g->patch_points & 63;
        if (!(__atomic_fetch_or(&logged, 1ull << bit, __ATOMIC_RELAXED) & (1ull << bit)))
            fprintf(stderr, gp->tessellated ?
                    "[BD12] tessellation pipeline drawn without a patch list: draws dropped\n" :
                    "[BD12] %u-control-point patch list without tessellation: draws dropped\n",
                    g->patch_points);
        BD12_DROP(BD12_DROP_DRAW_PATCH_LIST);
        return 0;
    }
    const VkPrimitiveTopology topology = patches && !gp->tessellated ?
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST : g->topology;
    if (topology != g->bound_topology) {
        g->bound_topology = topology;
        g->dirty |= BD12_DIRTY_TOPOLOGY;
    }
    }
    if (bd12_debug_sync_all()) bd12_cmd_global_barrier(list);
    if (gp->db_layout) {
        /* Descriptor-buffer pipeline: the heaps are bound once, the root arguments are push constants. Pending
         * first-use image transitions are recorded before rendering begins. */
        if (!bd12_db_prepare(list)) {
            BD12_DROP(BD12_DROP_DRAW_NO_DESCRIPTORS);
            return 0;
        }
        begin_draw_rendering(list);
        if (g->dirty & BD12_DIRTY_PIPELINE)
            device->vk.CmdBindPipeline(list->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, gp->pipeline);
        g->bound_set = VK_NULL_HANDLE;
        if (!bd12_cmd_push_root_arguments(list, &list->graphics, VK_PIPELINE_BIND_POINT_GRAPHICS, gp->layout,
                                          gp->stages, gp->root_push_bytes, gp->root_layout_hash, gp->db_layout)) {
            BD12_DROP(BD12_DROP_DRAW_NO_DESCRIPTORS);
            return 0;
        }
        return prepare_draw_state(list);
    }
    /* Resolve first: it can emit first-use layout barriers, which must not
     * land inside a rendering scope. */
    VkDescriptorSet set = bd12_resolve_descriptor_set(
        list, &list->graphics, gp->bindings, gp->binding_types,
        gp->binding_count, gp->set_layout);
    if (!set) {
        BD12_DROP(BD12_DROP_DRAW_NO_DESCRIPTORS);
        return 0;
    }
    if ((gp->bindless_words || gp->root_push_bytes) && !bd12_mirror_prepare(list)) {
        BD12_DROP(BD12_DROP_DRAW_NO_DESCRIPTORS);
        return 0;
    }
    begin_draw_rendering(list);
    VkCommandBuffer cb = list->command_buffer;
    if (g->dirty & BD12_DIRTY_PIPELINE)
        device->vk.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                   gp->pipeline);
    /* Cached descriptor sets repeat across draws: rebind only when the set or the pipeline changed. */
    if (set != g->bound_set || (g->dirty & BD12_DIRTY_PIPELINE)) {
        device->vk.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                         gp->layout, 0, 1, &set, 0, NULL);
        g->bound_set = set;
        bd12_cmd_note_set_binding(list, 0);
    }
    if (gp->root_push_bytes) {
        if (!bd12_cmd_push_root_arguments(list, &list->graphics, VK_PIPELINE_BIND_POINT_GRAPHICS, gp->layout,
                                          gp->stages, gp->root_push_bytes, gp->root_layout_hash, NULL)) {
            BD12_DROP(BD12_DROP_DRAW_NO_DESCRIPTORS);
            return 0;
        }
    } else if (gp->bindless_words)
        bd12_cmd_bind_bindless(list, &list->graphics, VK_PIPELINE_BIND_POINT_GRAPHICS,
                               gp->layout,
                               gp->stages,
                               gp->bindless, gp->bindless_count, gp->bindless_words);
    return prepare_draw_state(list);
}

/* Dynamic state, vertex buffers and the rest of a draw's bindings that do not depend on descriptors. */
static int prepare_draw_state(BD12CmdList *list)
{
    BD12Device *device = list->device;
    BD12GfxState *g = &list->gfx;
    BD12GraphicsPipeline *gp = g->pipeline;
    VkCommandBuffer cb = list->command_buffer;
    if ((g->dirty & BD12_DIRTY_VIEWPORT) && g->viewport_count)
        device->vk.CmdSetViewportWithCount(cb, g->viewport_count, g->viewports);
    if ((g->dirty & BD12_DIRTY_SCISSOR) && g->scissor_count)
        device->vk.CmdSetScissorWithCount(cb, g->scissor_count, g->scissors);
    if (gp->mesh) {
        /* No topology or vertex input here. A mesh pipeline does not carry the vertex pipeline's dynamic state, so
         * the next vertex draw sets its topology again (vertex buffers follow the pipeline change anyway). */
        g->bound_topology = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
        g->dirty &= ~(BD12_DIRTY_TOPOLOGY | BD12_DIRTY_VERTEX);
    }
    if (g->dirty & BD12_DIRTY_TOPOLOGY)
        device->vk.CmdSetPrimitiveTopology(cb, g->bound_topology);
    if (!gp->mesh && (g->dirty & (BD12_DIRTY_VERTEX | BD12_DIRTY_PIPELINE))) {
        VkBuffer buffers[BD12_MAX_VERTEX_BUFFERS];
        VkDeviceSize offsets[BD12_MAX_VERTEX_BUFFERS];
        VkDeviceSize sizes[BD12_MAX_VERTEX_BUFFERS];
        VkDeviceSize strides[BD12_MAX_VERTEX_BUFFERS];
        for (uint32_t i = 0; i < gp->vertex_slot_count; ++i) {
            g->vertex_buffers[i].buffer = live_buffer(device, g->vertex_buffers[i].va,
                                                      g->vertex_buffers[i].buffer);
            buffers[i] = g->vertex_buffers[i].buffer;
            offsets[i] = g->vertex_buffers[i].offset;
            sizes[i] = g->vertex_buffers[i].buffer ?
                       g->vertex_buffers[i].size : VK_WHOLE_SIZE;
            strides[i] = g->vertex_buffers[i].stride;
        }
        if (gp->vertex_slot_count)
            device->vk.CmdBindVertexBuffers2(cb, 0, gp->vertex_slot_count,
                                             buffers, offsets, sizes, strides);
    }
    if (g->dirty & BD12_DIRTY_BLEND)
        device->vk.CmdSetBlendConstants(cb, g->blend_constants);
    if (g->dirty & BD12_DIRTY_STENCIL)
        device->vk.CmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK,
                                          g->stencil_ref);
    if ((g->dirty & BD12_DIRTY_SHADING_RATE) && device->CmdSetFragmentShadingRateKHR) {
        /* D3D12_SHADING_RATE and Vulkan's fragment size share the log2 encoding; D3D12_SHADING_RATE_COMBINER
         * PASSTHROUGH, OVERRIDE, MIN, MAX, SUM map to KEEP, REPLACE, MIN, MAX, MUL (a sum of log2 sizes is a
         * product of sizes). */
        static const VkFragmentShadingRateCombinerOpKHR ops[5] = {
            VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR, VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR,
            VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MIN_KHR, VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MAX_KHR,
            VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MUL_KHR
        };
        const VkExtent2D size = { 1u << ((g->shading_rate >> 2) & 3u), 1u << (g->shading_rate & 3u) };
        VkFragmentShadingRateCombinerOpKHR combiners[2];
        for (int i = 0; i < 2; ++i) {
            const uint32_t c = g->shading_combiners[i];
            combiners[i] = c < 5 ? ops[c] : VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
            if (device->modern->vrs_tier < 2) combiners[i] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
            else if (combiners[i] == VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MUL_KHR && !device->modern->vrs_sum_combiner)
                combiners[i] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MAX_KHR;
        }
        device->CmdSetFragmentShadingRateKHR(cb, &size, combiners);
    }
    g->dirty &= BD12_DIRTY_INDEX;  /* index binding is handled per indexed draw */
    return 1;
}

/* A buffer bound earlier may have been released since (resources are destroyed
 * once the GPU is done with submitted work, but a list can keep its binding
 * state across that point). Passing a destroyed VkBuffer to the driver crashes
 * it, so re-resolve the GPU VA and bind nothing if the buffer is gone. */
static VkBuffer live_buffer(BD12Device *device, uint64_t va, VkBuffer expected)
{
    if (!va || !expected) return VK_NULL_HANDLE;
    VkDeviceSize offset;
    BD12Buffer *buffer = bd12_va_resolve(device, va, &offset);
    return buffer && buffer->buffer == expected ? expected : VK_NULL_HANDLE;
}

static void bind_index(BD12CmdList *list)
{
    BD12GfxState *g = &list->gfx;
    if (!(g->dirty & BD12_DIRTY_INDEX)) return;
    g->index_buffer = live_buffer(list->device, g->index_va, g->index_buffer);
    list->device->vk.CmdBindIndexBuffer(list->command_buffer, g->index_buffer,
                                        g->index_offset, g->index_type);
    g->dirty &= ~BD12_DIRTY_INDEX;
}

int bd12_cmd_draw(BD12CmdList *list, uint32_t vertex_count,
                  uint32_t instance_count, uint32_t first_vertex,
                  uint32_t first_instance)
{
    if (!prepare_draw(list)) return 0;
    bd12_cmd_record_flight_op(list, BD12_FLIGHT_DRAW,
        list->gfx.pipeline->shader_hash, (uint64_t)vertex_count * instance_count,
        vertex_count, instance_count, 0);
    list->device->vk.CmdDraw(list->command_buffer, vertex_count, instance_count,
                             first_vertex, first_instance);
    return 1;
}

int bd12_cmd_draw_indexed(BD12CmdList *list, uint32_t index_count,
                          uint32_t instance_count, uint32_t first_index,
                          int32_t base_vertex, uint32_t first_instance)
{
    if (!list->gfx.index_buffer && !(list->gfx.dirty & BD12_DIRTY_INDEX))
        return 0;
    if (!prepare_draw(list)) return 0;
    bind_index(list);
    bd12_cmd_record_flight_op(list, BD12_FLIGHT_DRAW_INDEXED,
        list->gfx.pipeline->shader_hash, (uint64_t)index_count * instance_count,
        index_count, instance_count, 0);
    list->device->vk.CmdDrawIndexed(list->command_buffer, index_count,
                                    instance_count, first_index, base_vertex,
                                    first_instance);
    return 1;
}

int bd12_cmd_draw_indirect(BD12CmdList *list, BD12Buffer *args,
                           VkDeviceSize offset, uint32_t count, uint32_t stride)
{
    if (!prepare_draw(list)) return 0;
    bd12_cmd_record_flight_op(list, BD12_FLIGHT_DRAW_INDIRECT,
        list->gfx.pipeline->shader_hash, count, count, stride ? stride : 16, 0);
    list->device->vk.CmdDrawIndirect(list->command_buffer, args->buffer, offset,
                                     count, stride ? stride : 16);
    return 1;
}

int bd12_cmd_draw_indexed_indirect(BD12CmdList *list, BD12Buffer *args,
                                   VkDeviceSize offset, uint32_t count,
                                   uint32_t stride)
{
    if (!prepare_draw(list)) return 0;
    bind_index(list);
    bd12_cmd_record_flight_op(list, BD12_FLIGHT_DRAW_INDEXED_INDIRECT,
        list->gfx.pipeline->shader_hash, count, count, stride ? stride : 20, 0);
    bd12_ckpt_args(args->va + offset, 0);
    list->device->vk.CmdDrawIndexedIndirect(list->command_buffer, args->buffer,
                                            offset, count, stride ? stride : 20);
    return 1;
}

int bd12_cmd_draw_indirect_count(BD12CmdList *list, BD12Buffer *args,
                                 VkDeviceSize offset, BD12Buffer *count_buffer,
                                 VkDeviceSize count_offset, uint32_t max_count,
                                 uint32_t stride)
{
    if (!prepare_draw(list)) return 0;
    bd12_cmd_record_flight_op(list, BD12_FLIGHT_DRAW_INDIRECT,
        list->gfx.pipeline->shader_hash, max_count, max_count,
        stride ? stride : 16, 1);
    list->device->vk.CmdDrawIndirectCount(
        list->command_buffer, args->buffer, offset, count_buffer->buffer,
        count_offset, max_count, stride ? stride : 16);
    return 1;
}

/* Mesh dispatches ------------------------------------------------------------------- */

int bd12_cmd_dispatch_mesh(BD12CmdList *list, uint32_t x, uint32_t y, uint32_t z)
{
    { static int logged; if (!logged++) fprintf(stderr, "[BD12] first DispatchMesh(%u, %u, %u)\n", x, y, z); }
    if (!list->device->CmdDrawMeshTasksEXT || !prepare_graphics(list, 1)) return 0;
    if (bd12_trace_active())
        fprintf(stderr, "[TRACE] mesh-direct pipeline=%016llx groups=%u,%u,%u\n",
                (unsigned long long)list->gfx.pipeline->shader_hash, x, y, z);
    if (!x || !y || !z) return 1;
    list->device->CmdDrawMeshTasksEXT(list->command_buffer, x, y, z);
    return 1;
}

/* D3D12_DISPATCH_MESH_ARGUMENTS and VkDrawMeshTasksIndirectCommandEXT are the same three group counts. */
int bd12_cmd_dispatch_mesh_indirect(BD12CmdList *list, BD12Buffer *args, VkDeviceSize offset,
                                    BD12Buffer *count_buffer, VkDeviceSize count_offset,
                                    uint32_t max_count, uint32_t stride)
{
    BD12Device *device = list->device;
    { static int logged; if (!logged++) fprintf(stderr, "[BD12] first indirect DispatchMesh (max %u, count buffer %d)\n",
                                                max_count, count_buffer != NULL); }
    if (!device->CmdDrawMeshTasksEXT || !args || !max_count || !prepare_graphics(list, 1)) return 0;
    if (!stride) stride = 12;
    if (bd12_trace_active())     /* traced frame: every indirect mesh dispatch */
        fprintf(stderr, "[TRACE] mesh-indirect pipeline=%016llx args=%llx+%llu max=%u stride=%u count=%llx+%llu\n",
                (unsigned long long)list->gfx.pipeline->shader_hash, (unsigned long long)args->va,
                (unsigned long long)offset, max_count, stride,
                (unsigned long long)(count_buffer ? count_buffer->va : 0), (unsigned long long)count_offset);
    if (count_buffer)
        device->CmdDrawMeshTasksIndirectCountEXT(list->command_buffer, args->buffer, offset, count_buffer->buffer,
                                                 count_offset, max_count, stride);
    else
        device->CmdDrawMeshTasksIndirectEXT(list->command_buffer, args->buffer, offset, max_count, stride);
    return 1;
}

#include "bd12_exec_patch_spv.h"

/* Diagnostic BEER_BD12_FORCE_INST=1|2: draw from a copy of the arguments whose
 * instanceCount is forced to 1 (2 also ignores the GPU draw count). Tells
 * whether the GPU culling result is what hides geometry. */
static int patch_indexed_args(BD12CmdList *list, BD12Buffer **args,
                              VkDeviceSize *offset, uint32_t max_count,
                              uint32_t stride)
{
    static BD12ComputePipeline *patch;
    if (!patch)
        patch = bd12_compute_pipeline_create(list->device, k_exec_patch_spv,
                                             sizeof(k_exec_patch_spv), 24);
    if (!patch || stride != 20 || !max_count || max_count > 70000) return 0;
    BD12Buffer *scratch; VkDeviceSize at; void *ptr;
    if (!bd12_scratch_alloc(list->allocator, (VkDeviceSize)max_count * stride,
                            &scratch, &at, &ptr))
        return 0;
    struct { uint64_t src, dst; uint32_t count, words; } push = {
        (*args)->va + *offset, scratch->va + at, max_count, stride / 4 };
    bd12_cmd_end_rendering(list);
    bd12_cmd_global_barrier(list);
    bd12_cmd_dispatch(list, patch, &push, (max_count + 63) / 64, 1, 1);
    bd12_cmd_global_barrier(list);
    *args = scratch;
    *offset = at;
    return 1;
}

int bd12_cmd_draw_indexed_indirect_count(
    BD12CmdList *list, BD12Buffer *args, VkDeviceSize offset,
    BD12Buffer *count_buffer, VkDeviceSize count_offset, uint32_t max_count,
    uint32_t stride)
{
    {
        static int mode = -1;
        if (mode < 0) mode = getenv("BEER_BD12_FORCE_INST") ?
                             atoi(getenv("BEER_BD12_FORCE_INST")) : 0;
        if (mode && patch_indexed_args(list, &args, &offset, max_count, stride ? stride : 20)) {
            if (!prepare_draw(list)) return 0;
            bind_index(list);
            if (mode == 2)
                list->device->vk.CmdDrawIndexedIndirect(list->command_buffer, args->buffer,
                                                        offset, max_count, stride ? stride : 20);
            else
                list->device->vk.CmdDrawIndexedIndirectCount(
                    list->command_buffer, args->buffer, offset, count_buffer->buffer,
                    count_offset, max_count, stride ? stride : 20);
            return 1;
        }
    }
    if (!prepare_draw(list)) return 0;
    bind_index(list);
    bd12_cmd_record_flight_op(list, BD12_FLIGHT_DRAW_INDEXED_INDIRECT,
        list->gfx.pipeline->shader_hash, max_count, max_count,
        stride ? stride : 20, 1);
    bd12_ckpt_args(args->va + offset, count_buffer->va + count_offset);
    list->device->vk.CmdDrawIndexedIndirectCount(
        list->command_buffer, args->buffer, offset, count_buffer->buffer,
        count_offset, max_count, stride ? stride : 20);
    return 1;
}

/* Clears -------------------------------------------------------------------------------- */

void bd12_cmd_clear_rtv(BD12CmdList *list, const BD12Descriptor *rtv,
                        const float color[4], uint32_t rect_count,
                        const VkRect2D *rects)
{
    if (!rtv || rtv->kind != BD12_DESC_RTV) return;
    BD12Device *device = list->device;
    BD12GfxState *g = &list->gfx;
    VkClearRect clear_rects[16];
    uint32_t n = rect_count > 16 ? 16 : rect_count;
    for (uint32_t i = 0; i < n; ++i)
        clear_rects[i] = (VkClearRect){ rects[i], 0, rtv->layer_count };
    if (!n) {
        clear_rects[0] = (VkClearRect){ { { 0, 0 }, { rtv->width, rtv->height } },
                                        0, rtv->layer_count };
        n = 1;
    }
    VkClearAttachment attachment = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT };
    memcpy(attachment.clearValue.color.float32, color, 4 * sizeof(float));
    if (g->rendering) {
        for (uint32_t i = 0; i < g->rtv_count; ++i) {
            if (g->rtv[i].kind == BD12_DESC_RTV &&
                g->rtv[i].image_view == rtv->image_view) {
                attachment.colorAttachment = i;
                device->vk.CmdClearAttachments(list->command_buffer, 1,
                                               &attachment, n, clear_rects);
                return;
            }
        }
        bd12_cmd_end_rendering(list);
    }
    if (!rect_count) {
        begin_rendering_for(list, rtv, 1, NULL, 1, color, 0, 0, 0.0f, 0, 0);
    } else {
        begin_rendering_for(list, rtv, 1, NULL, 0, NULL, 0, 0, 0.0f, 0, 0);
        device->vk.CmdClearAttachments(list->command_buffer, 1, &attachment, n,
                                       clear_rects);
    }
    device->vk.CmdEndRendering(list->command_buffer);
}

void bd12_cmd_clear_dsv(BD12CmdList *list, const BD12Descriptor *dsv,
                        VkImageAspectFlags aspects, float depth,
                        uint32_t stencil, uint32_t rect_count,
                        const VkRect2D *rects)
{
    if (!dsv || dsv->kind != BD12_DESC_DSV) return;
    BD12Device *device = list->device;
    BD12GfxState *g = &list->gfx;
    aspects &= dsv->aspect;
    if (!aspects) return;
    VkClearRect clear_rects[16];
    uint32_t n = rect_count > 16 ? 16 : rect_count;
    for (uint32_t i = 0; i < n; ++i)
        clear_rects[i] = (VkClearRect){ rects[i], 0, dsv->layer_count };
    if (!n) {
        clear_rects[0] = (VkClearRect){ { { 0, 0 }, { dsv->width, dsv->height } },
                                        0, dsv->layer_count };
        n = 1;
    }
    VkClearAttachment attachment = { .aspectMask = aspects };
    attachment.clearValue.depthStencil.depth = depth;
    attachment.clearValue.depthStencil.stencil = stencil;
    if (g->rendering) {
        if (g->has_dsv && g->dsv.image_view == dsv->image_view) {
            device->vk.CmdClearAttachments(list->command_buffer, 1, &attachment,
                                           n, clear_rects);
            return;
        }
        bd12_cmd_end_rendering(list);
    }
    const int cd = (aspects & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
    const int cs = (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
    if (!rect_count) {
        begin_rendering_for(list, NULL, 0, dsv, 0, NULL, cd, cs, depth, stencil, 0);
    } else {
        begin_rendering_for(list, NULL, 0, dsv, 0, NULL, 0, 0, 0.0f, 0, 0);
        device->vk.CmdClearAttachments(list->command_buffer, 1, &attachment, n,
                                       clear_rects);
    }
    device->vk.CmdEndRendering(list->command_buffer);
}
