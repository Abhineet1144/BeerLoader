#define _GNU_SOURCE
#include "bd12.h"

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

BD12GraphicsPipeline *bd12_graphics_pipeline_create(
    BD12Device *device, const BD12GraphicsPipelineDesc *desc)
{
    if (!desc->vs.spirv || desc->rt_count > BD12_MAX_RENDER_TARGETS) return NULL;
    BD12GraphicsPipeline *gp = calloc(1, sizeof(*gp));
    if (!gp) return NULL;
    gp->device = device;
    /* Stable diagnostic identity; no shader-specific behavior depends on it. */
    uint64_t hash = UINT64_C(1469598103934665603);
    const BD12ShaderCode *hash_stages[2] = { &desc->vs, &desc->ps };
    for (int s = 0; s < 2; ++s) {
        const uint8_t *bytes = (const uint8_t *)hash_stages[s]->spirv;
        for (size_t i = 0; bytes && i < hash_stages[s]->spirv_bytes; ++i) {
            hash ^= bytes[i];
            hash *= UINT64_C(1099511628211);
        }
    }
    gp->shader_hash = hash;
    /* Occlusion-proxy pipelines (no colour output, depth test without write, pixel
     * shader with UAV stores) mark objects visible when their proxy passes the depth
     * test. Their borderline comparisons against coplanar geometry resolve differently
     * on this driver than on D3D hardware and made whole batches (floors, characters)
     * pop in and out, so they always pass: conservative, only occlusion culling is lost.
     * BEER_BD12_PROXY_ALWAYS=0 restores the application's depth function. */
    int proxy_always = 0;
    const char *proxy_env = getenv("BEER_BD12_PROXY_ALWAYS");
    if ((!proxy_env || atoi(proxy_env)) && desc->rt_count == 0 && desc->depth_test &&
        !desc->depth_write)
        for (uint32_t i = 0; i < desc->ps.binding_count; ++i)
            if (desc->ps.bindings[i].type == BD12_RANGE_UAV) proxy_always = 1;
    /* BEER_BD12_PROXY_BIAS=<n>: instead of always passing, nudge such proxies n
     * depth ULPs toward the camera (reverse-Z) so ties against coplanar geometry
     * resolve as visible while clearly occluded objects are still culled. */
    float proxy_bias = 0.0f;
    if (proxy_always && getenv("BEER_BD12_PROXY_BIAS") && atoi(getenv("BEER_BD12_PROXY_BIAS")) > 0) {
        proxy_bias = (float)atoi(getenv("BEER_BD12_PROXY_BIAS"));
        proxy_always = 0;
    }
    if (getenv("BEER_BD12_PSO_LOG"))
        fprintf(stderr, "[PSO] %016llx topo=%d fill=%d cull=%d ccw=%d clip=%d bias=%d depth=%d/%d func=%d "
                "stencil=%d rt=%u blend0=%d wm0=%x ds=%d samples=%u\n",
                (unsigned long long)hash, (int)desc->topology, (int)desc->fill_mode,
                (int)desc->cull_mode, desc->front_ccw, desc->depth_clip_enable,
                desc->depth_bias_enable, desc->depth_test, desc->depth_write,
                (int)desc->depth_func, desc->stencil_test, desc->rt_count,
                desc->blend[0].blend_enable, desc->blend[0].write_mask,
                (int)desc->ds_format, desc->samples);

    /* Merge both stages' reflected bindings into one set-0 layout. */
    const BD12ShaderCode *stages[2] = { &desc->vs, &desc->ps };
    const uint32_t total = desc->vs.binding_count + desc->ps.binding_count;
    VkDescriptorSetLayoutBinding *layout_bindings =
        calloc(total ? total : 1, sizeof(*layout_bindings));
    gp->bindings = calloc(total ? total : 1, sizeof(*gp->bindings));
    gp->binding_types = calloc(total ? total : 1, sizeof(*gp->binding_types));
    VkShaderModule modules[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    BD12ShaderBinding *stage_binds[2] = { NULL, NULL };
    uint32_t stage_kept[2] = { 0, 0 };
    gp->bindless = calloc(total ? total : 1, sizeof(*gp->bindless));
    if (!gp->bindless) goto fail;
    uint32_t *dense_spirv[2] = { NULL, NULL };
    uint32_t next_binding = 0;
    if (!layout_bindings || !gp->bindings || !gp->binding_types) goto fail;
    for (int s = 0; s < 2; ++s) {
        if (!stages[s]->spirv) continue;
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
    for (int s = 0; s < 2; ++s) {
        for (uint32_t i = 0; stages[s]->spirv && i < stage_kept[s]; ++i) {
            BD12ShaderBinding b = stage_binds[s][i];
            VkDescriptorType type;
            if (!bd12_binding_descriptor_type(&b, &type)) goto fail;
            if (!b.count) b.count = 1;
            layout_bindings[gp->binding_count] = (VkDescriptorSetLayoutBinding){
                .binding = b.binding,
                .descriptorType = type,
                .descriptorCount = b.count,
                .stageFlags = s == 0 ? VK_SHADER_STAGE_VERTEX_BIT :
                              VK_SHADER_STAGE_FRAGMENT_BIT
            };
            gp->bindings[gp->binding_count] = b;
            gp->binding_types[gp->binding_count] = type;
            ++gp->binding_count;
        }
    }
    VkDescriptorSetLayoutCreateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = gp->binding_count,
        .pBindings = layout_bindings
    };
    if (device->vk.CreateDescriptorSetLayout(device->device, &set_info, NULL,
                                             &gp->set_layout) != VK_SUCCESS)
        goto fail;
    VkDescriptorSetLayout set_layouts[2] = { gp->set_layout, VK_NULL_HANDLE };
    VkPushConstantRange push_range = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .size = gp->bindless_words * 4
    };
    if (gp->bindless_words) {
        set_layouts[1] = bd12_mirror_layout(device);
        if (!set_layouts[1]) goto fail;
    }
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = gp->bindless_words ? 2u : 1u,
        .pSetLayouts = set_layouts,
        .pushConstantRangeCount = gp->bindless_words ? 1u : 0u,
        .pPushConstantRanges = &push_range
    };
    if (device->vk.CreatePipelineLayout(device->device, &layout_info, NULL,
                                        &gp->layout) != VK_SUCCESS)
        goto fail;

    VkPipelineShaderStageCreateInfo shader_stages[2];
    uint32_t stage_count = 0;
    BD12ShaderCode dense_vs = desc->vs, dense_ps = desc->ps;
    dense_vs.spirv = dense_spirv[0];
    dense_ps.spirv = dense_spirv[1];
    modules[0] = make_module(device, &dense_vs);
    if (!modules[0]) goto fail;
    shader_stages[stage_count++] = (VkPipelineShaderStageCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_VERTEX_BIT,
        .module = modules[0],
        .pName = "main"
    };
    if (desc->ps.spirv) {
        modules[1] = make_module(device, &dense_ps);
        if (!modules[1]) goto fail;
        shader_stages[stage_count++] = (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = modules[1],
            .pName = "main"
        };
    }
    for (int i = 0; i < 2; ++i) {
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
        .depthBiasEnable = proxy_bias > 0.0f ? 1 : desc->depth_bias_enable,
        .depthBiasConstantFactor = proxy_bias > 0.0f ? proxy_bias : desc->depth_bias,
        .depthBiasClamp = proxy_bias > 0.0f ? 0.0f : desc->depth_bias_clamp,
        .depthBiasSlopeFactor = proxy_bias > 0.0f ? 0.0f : desc->slope_scaled_bias,
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
        .depthCompareOp = proxy_always ? VK_COMPARE_OP_ALWAYS : desc->depth_func,
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
    const VkDynamicState dynamic_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT,
        VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT,
        VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE,
        VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE
    };
    VkPipelineDynamicStateCreateInfo dynamic = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = sizeof(dynamic_states) / sizeof(dynamic_states[0]),
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
        .stageCount = stage_count,
        .pStages = shader_stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
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
    for (int i = 0; i < 2; ++i)
        if (modules[i])
            device->vk.DestroyShaderModule(device->device, modules[i], NULL);
    free(layout_bindings);
    return gp;

fail:
    for (int i = 0; i < 2; ++i) {
        free(dense_spirv[i]);
        free(stage_binds[i]);
    }
    for (int i = 0; i < 2; ++i)
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
    if (gp->layout)
        device->vk.DestroyPipelineLayout(device->device, gp->layout, NULL);
    if (gp->set_layout)
        device->vk.DestroyDescriptorSetLayout(device->device, gp->set_layout,
                                              NULL);
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
    memset(list->graphics.arguments, 0, sizeof(list->graphics.arguments));
    list->graphics.root_signature = signature;
}

void bd12_cmd_set_graphics_root_constants(BD12CmdList *list, uint32_t parameter,
                                          const uint32_t *data, uint32_t count,
                                          uint32_t dest_offset)
{
    if (parameter >= BD12_MAX_ROOT_PARAMETERS ||
        dest_offset + count > BD12_MAX_ROOT_CONSTANTS)
        return;
    BD12RootArgument *arg = &list->graphics.arguments[parameter];
    memcpy(&arg->constants[dest_offset], data, count * sizeof(uint32_t));
    arg->cbv_valid = 0;
}

void bd12_cmd_set_graphics_root_va(BD12CmdList *list, uint32_t parameter,
                                   uint64_t gpu_va)
{
    if (parameter < BD12_MAX_ROOT_PARAMETERS)
        list->graphics.arguments[parameter].value = gpu_va;
}

void bd12_cmd_set_graphics_root_table(BD12CmdList *list, uint32_t parameter,
                                      uint64_t gpu_handle)
{
    if (parameter < BD12_MAX_ROOT_PARAMETERS)
        list->graphics.arguments[parameter].value = gpu_handle;
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
    list->gfx.dirty |= BD12_DIRTY_TOPOLOGY;
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
                                uint32_t stencil)
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
    device->vk.CmdBeginRendering(list->command_buffer, &info);
}

static void begin_draw_rendering(BD12CmdList *list)
{
    BD12GfxState *g = &list->gfx;
    if (g->rendering) return;
    begin_rendering_for(list, g->rtv, g->rtv_count, g->has_dsv ? &g->dsv : NULL,
                        0, NULL, 0, 0, 0.0f, 0);
    g->rendering = 1;
}

/* Draws -------------------------------------------------------------------------------- */

static VkBuffer live_buffer(BD12Device *device, uint64_t va, VkBuffer expected);

static int prepare_draw(BD12CmdList *list)
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
    if (bd12_debug_sync_all()) bd12_cmd_global_barrier(list);
    /* Resolve first: it can emit first-use layout barriers, which must not
     * land inside a rendering scope. */
    VkDescriptorSet set = bd12_resolve_descriptor_set(
        list, &list->graphics, gp->bindings, gp->binding_types,
        gp->binding_count, gp->set_layout);
    if (!set) {
        BD12_DROP(BD12_DROP_DRAW_NO_DESCRIPTORS);
        return 0;
    }
    if (gp->bindless_words && !bd12_mirror_prepare(list)) {
        BD12_DROP(BD12_DROP_DRAW_NO_DESCRIPTORS);
        return 0;
    }
    begin_draw_rendering(list);
    VkCommandBuffer cb = list->command_buffer;
    if (g->dirty & BD12_DIRTY_PIPELINE)
        device->vk.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                   gp->pipeline);
    device->vk.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                     gp->layout, 0, 1, &set, 0, NULL);
    if (gp->bindless_words)
        bd12_cmd_bind_bindless(list, &list->graphics, VK_PIPELINE_BIND_POINT_GRAPHICS,
                               gp->layout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               gp->bindless, gp->bindless_count, gp->bindless_words);
    if ((g->dirty & BD12_DIRTY_VIEWPORT) && g->viewport_count)
        device->vk.CmdSetViewportWithCount(cb, g->viewport_count, g->viewports);
    if ((g->dirty & BD12_DIRTY_SCISSOR) && g->scissor_count)
        device->vk.CmdSetScissorWithCount(cb, g->scissor_count, g->scissors);
    if (g->dirty & BD12_DIRTY_TOPOLOGY)
        device->vk.CmdSetPrimitiveTopology(cb, g->topology);
    if (g->dirty & (BD12_DIRTY_VERTEX | BD12_DIRTY_PIPELINE)) {
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
        begin_rendering_for(list, rtv, 1, NULL, 1, color, 0, 0, 0.0f, 0);
    } else {
        begin_rendering_for(list, rtv, 1, NULL, 0, NULL, 0, 0, 0.0f, 0);
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
        begin_rendering_for(list, NULL, 0, dsv, 0, NULL, cd, cs, depth, stencil);
    } else {
        begin_rendering_for(list, NULL, 0, dsv, 0, NULL, 0, 0, 0.0f, 0);
        device->vk.CmdClearAttachments(list->command_buffer, 1, &attachment, n,
                                       clear_rects);
    }
    device->vk.CmdEndRendering(list->command_buffer);
}
