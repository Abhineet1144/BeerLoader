#include "d3d12_vulkan_graphics.h"
#include "d3d12_graphics.h"
#include "vulkan_graphics.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int dxgi_to_vulkan(uint32_t dxgi, uint32_t *format, uint32_t *bytes)
{
    static const struct { uint32_t dxgi, vk, bytes; } table[] = {
        {2, 109, 16}, {3, 107, 16}, {4, 108, 16},
        {6, 106, 12},
        {10, 97, 8}, {11, 91, 8}, {12, 95, 8}, {13, 92, 8}, {14, 96, 8},
        {16, 103, 8}, {17, 101, 8}, {18, 102, 8},
        {21, 126, 4}, /* R32_FLOAT_X8X24 -> D32_SFLOAT depth aspect */
        {24, 64, 4}, {25, 68, 4}, {26, 122, 4},
        {28, 37, 4}, {29, 43, 4}, {30, 41, 4}, {31, 38, 4}, {32, 42, 4},
        {34, 83, 4}, {35, 77, 4}, {36, 81, 4}, {37, 78, 4}, {38, 82, 4},
        {41, 100, 4}, {42, 98, 4}, {43, 99, 4},
        {49, 16, 2}, {50, 20, 2}, {51, 17, 2}, {52, 21, 2},
        {54, 76, 2}, {56, 70, 2}, {57, 74, 2}, {58, 71, 2}, {59, 75, 2},
        {61, 9, 1}, {62, 13, 1}, {63, 10, 1}, {64, 14, 1},
        {87, 44, 4}, {91, 50, 4}
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        if (table[i].dxgi == dxgi) {
            *format = table[i].vk;
            *bytes = table[i].bytes;
            return 1;
        }
    }
    return 0;
}

static void dxgi_to_texel_format(uint32_t dxgi, uint32_t *format,
                                 uint32_t *bytes)
{
    static const struct { uint32_t dxgi, vk, bytes; } table[] = {
        {2, 109, 16}, {3, 107, 16}, {4, 108, 16},
        {10, 97, 8}, {11, 91, 8}, {12, 95, 8},
        {16, 103, 8}, {17, 101, 8}, {18, 102, 8},
        {28, 37, 4}, {30, 41, 4}, {34, 83, 4}, {36, 81, 4},
        {41, 100, 4}, {42, 98, 4}, {43, 99, 4},
        {49, 16, 2}, {54, 76, 2}, {57, 74, 2},
        {61, 9, 1}, {62, 13, 1}
    };
    *format = 0;
    *bytes = 0;
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i)
        if (table[i].dxgi == dxgi) {
            *format = table[i].vk;
            *bytes = table[i].bytes;
            return;
        }
}

static int dxgi_compressed_to_vulkan(uint32_t dxgi, uint32_t *format,
                                     uint32_t *bytes_per_block)
{
    static const struct { uint32_t dxgi, vk, bytes; } table[] = {
        {71, 131, 8}, {72, 132, 8},   /* BC1 UNORM / SRGB */
        {74, 133, 16}, {75, 134, 16}, /* BC2 UNORM / SRGB */
        {77, 135, 16}, {78, 136, 16}, /* BC3 UNORM / SRGB */
        {80, 137, 8}, {81, 138, 8},   /* BC4 UNORM / SNORM */
        {83, 139, 16}, {84, 140, 16}, /* BC5 UNORM / SNORM */
        {95, 143, 16}, {96, 144, 16}, /* BC6H UF16 / SF16 */
        {98, 145, 16}, {99, 146, 16}  /* BC7 UNORM / SRGB */
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        if (table[i].dxgi == dxgi) {
            *format = table[i].vk;
            *bytes_per_block = table[i].bytes;
            return 1;
        }
    }
    return 0;
}

/* D3D12_BLEND -> VkBlendFactor. Returns -1 for factors without an exact
 * single-source equivalent (dual-source blending, undefined values). */
static int blend_factor_to_vulkan(uint32_t blend)
{
    switch (blend) {
    case 1: return 0;   /* ZERO */
    case 2: return 1;   /* ONE */
    case 3: return 2;   /* SRC_COLOR */
    case 4: return 3;   /* INV_SRC_COLOR */
    case 5: return 6;   /* SRC_ALPHA */
    case 6: return 7;   /* INV_SRC_ALPHA */
    case 7: return 8;   /* DEST_ALPHA */
    case 8: return 9;   /* INV_DEST_ALPHA */
    case 9: return 4;   /* DEST_COLOR */
    case 10: return 5;  /* INV_DEST_COLOR */
    case 11: return 14; /* SRC_ALPHA_SAT */
    case 14: return 10; /* BLEND_FACTOR */
    case 15: return 11; /* INV_BLEND_FACTOR */
    default: return -1;
    }
}

static int blend_op_to_vulkan(uint32_t op)
{
    return op >= 1 && op <= 5 ? (int)op - 1 : -1;
}

/* Decodes retained PSO blend/rasterizer/depth state. Anything Vulkan cannot
 * reproduce exactly is rejected with a reason rather than approximated. */
static int map_pipeline_state(const BeerD3D12GraphicsDraw *draw,
                              BeerVulkanGraphicsState *state,
                              const char **reason)
{
    const BeerD3D12PipelineStateInfo *pso = &draw->pso;
    memset(state, 0, sizeof(*state));
    if (!pso->valid) {
        *reason = "pipeline state not decoded";
        return 0;
    }
    const uint32_t sample_count = pso->sample_count ? pso->sample_count : 1u;
    const int valid_sample_count = sample_count == 1 || sample_count == 2 ||
        sample_count == 4 || sample_count == 8 || sample_count == 16 ||
        sample_count == 32 || sample_count == 64;
    /* Implement genuine multisampling first for depth-only passes. Color MSAA
     * also requires resolve semantics and remains explicitly unsupported. */
    if (pso->forced_sample_count || pso->conservative_raster ||
        !valid_sample_count || (sample_count > 1 && draw->render_target_count) ||
        pso->alpha_to_coverage || pso->sample_mask != UINT32_MAX) {
        static _Atomic uint32_t multisample_reports;
        if (atomic_fetch_add(&multisample_reports, 1) < 8)
            fprintf(stderr,
                "[D3D12 VULKAN GRAPHICS] multisample state forced=%u "
                "conservative=%u samples=%u a2c=%u mask=%08x rt=%u\n",
                (unsigned)pso->forced_sample_count,
                (unsigned)pso->conservative_raster,
                (unsigned)pso->sample_count, (unsigned)pso->alpha_to_coverage,
                (unsigned)pso->sample_mask, draw->render_target_count);
        *reason = "multisample raster state unsupported";
        return 0;
    }
    if (pso->fill_mode != 3) {
        *reason = "wireframe rasterization unsupported";
        return 0;
    }
    if (pso->stencil_enable) {
        if (!draw->depth_stencil || !draw->depth_stencil_storage ||
            !draw->depth_stencil_serial_location) {
            *reason = "stencil attachment unresolved";
            return 0;
        }
        const BeerD3D12StencilOp *faces[2] = {
            &pso->stencil_front, &pso->stencil_back
        };
        uint32_t *mapped[2][4] = {
            {&state->stencil_front_fail, &state->stencil_front_depth_fail,
             &state->stencil_front_pass, &state->stencil_front_compare},
            {&state->stencil_back_fail, &state->stencil_back_depth_fail,
             &state->stencil_back_pass, &state->stencil_back_compare}
        };
        for (uint32_t face = 0; face < 2; ++face) {
            if (faces[face]->fail < 1 || faces[face]->fail > 8 ||
                faces[face]->depth_fail < 1 ||
                faces[face]->depth_fail > 8 || faces[face]->pass < 1 ||
                faces[face]->pass > 8 || faces[face]->func < 1 ||
                faces[face]->func > 8) {
                *reason = "stencil operation unsupported";
                return 0;
            }
            *mapped[face][0] = faces[face]->fail - 1u;
            *mapped[face][1] = faces[face]->depth_fail - 1u;
            *mapped[face][2] = faces[face]->pass - 1u;
            *mapped[face][3] = faces[face]->func - 1u;
        }
        state->stencil_test_enable = 1;
        state->stencil_read_mask = pso->stencil_read_mask;
        state->stencil_write_mask = pso->stencil_write_mask;
        state->stencil_reference = draw->stencil_reference;
    }
    /* D3D12: with no depth-stencil view bound, depth testing is a no-op. */
    if (pso->depth_enable && draw->depth_stencil) {
        if (!draw->depth_stencil_storage ||
            !draw->depth_stencil_serial_location) {
            *reason = "depth attachment unresolved";
            return 0;
        }
        if (pso->depth_func < 1 || pso->depth_func > 8) {
            *reason = "depth comparison unsupported";
            return 0;
        }
        state->depth_test_enable = 1;
        state->depth_write_enable = pso->depth_write_mask ? 1u : 0u;
        state->depth_compare_op = pso->depth_func - 1u;
    }
    const BeerD3D12RenderTargetBlend *rt = &pso->render_target[0];
    if (rt->logic_op_enable) {
        *reason = "logic-op blending unsupported";
        return 0;
    }
    state->write_mask = rt->write_mask;
    /* Primitive restart is controlled exclusively by the D3D12 PSO's
     * IBStripCutValue. Inferring it from index contents changes valid guest
     * geometry and is not API-compatible. Vulkan selects the concrete restart
     * sentinel from the bound index type. */
    if ((draw->primitive_topology == 3 || draw->primitive_topology == 5) &&
        (pso->index_buffer_strip_cut_value == 1 ||
         pso->index_buffer_strip_cut_value == 2))
        state->primitive_restart_enable = 1;
    if (rt->blend_enable) {
        int src_color = blend_factor_to_vulkan(rt->src_blend);
        int dst_color = blend_factor_to_vulkan(rt->dest_blend);
        int src_alpha = blend_factor_to_vulkan(rt->src_blend_alpha);
        int dst_alpha = blend_factor_to_vulkan(rt->dest_blend_alpha);
        int color_op = blend_op_to_vulkan(rt->blend_op);
        int alpha_op = blend_op_to_vulkan(rt->blend_op_alpha);
        if (src_color < 0 || dst_color < 0 || src_alpha < 0 ||
            dst_alpha < 0 || color_op < 0 || alpha_op < 0) {
            *reason = "blend factor/operation unsupported";
            return 0;
        }
        state->blend_enable = 1;
        state->src_color_factor = (uint32_t)src_color;
        state->dst_color_factor = (uint32_t)dst_color;
        state->color_op = (uint32_t)color_op;
        state->src_alpha_factor = (uint32_t)src_alpha;
        state->dst_alpha_factor = (uint32_t)dst_alpha;
        state->alpha_op = (uint32_t)alpha_op;
    }
    switch (pso->cull_mode) {
    case 1: state->cull_mode = 0; break; /* NONE */
    case 2: state->cull_mode = 1; break; /* FRONT */
    case 3: state->cull_mode = 2; break; /* BACK */
    default:
        *reason = "cull mode unsupported";
        return 0;
    }
    state->front_counter_clockwise = pso->front_counter_clockwise ? 1u : 0u;
    state->sample_count = sample_count;
    state->flip_y = 1;
    memcpy(state->blend_constants, draw->blend_factor,
           sizeof(state->blend_constants));
    return 1;
}

static int descriptor_resource_class_matches(
    const BeerD3D12ComputeBinding *binding, uint32_t entry,
    const BeerD3D12ShaderDescriptor *descriptor)
{
    /* D3D permits stage-local declarations such as a vertex buffer t0 and a
     * pixel Texture2D t0 in the same pipeline. They have distinct semantic
     * Vulkan bindings, but register/type/space alone cannot select the right
     * D3D descriptor. D3D12_RESOURCE_DIMENSION_BUFFER is 1; texture resources
     * use dimensions 2 through 4. CBVs and samplers are handled separately and
     * do not require view-shape filtering here. */
    if (descriptor->type != BEER_D3D12_SHADER_DESCRIPTOR_SRV &&
        descriptor->type != BEER_D3D12_SHADER_DESCRIPTOR_UAV)
        return 1;
    if (beer_d3d12_graphics_descriptor_shape_matches(
            descriptor->resource_type,
            binding->descriptor_view_dimensions[entry],
            binding->descriptor_resource_dimensions[entry],
            binding->descriptor_resource_layers[entry],
            binding->descriptor_resource_samples[entry]))
        return 1;
    /* D3D12 permits a one-slice Texture2DArray SRV, starting at slice zero, to
     * satisfy a non-array Texture2D declaration. RE8's fullscreen title passes
     * use this form, so match it as a Texture2D view of that slice. Only slice
     * zero is accepted; nonzero first slices need an explicit storage offset. */
    return descriptor->type == BEER_D3D12_SHADER_DESCRIPTOR_SRV &&
        descriptor->resource_type == BEER_D3D12_SHADER_RESOURCE_TEXTURE_2D &&
        binding->descriptor_view_dimensions[entry] == 5 &&
        binding->descriptor_view_first_layers[entry] == 0 &&
        binding->descriptor_view_layer_counts[entry] == 1 &&
        beer_d3d12_graphics_descriptor_shape_matches(
            descriptor->resource_type, 4,
            binding->descriptor_resource_dimensions[entry],
            binding->descriptor_resource_layers[entry],
            binding->descriptor_resource_samples[entry]);
}

static const uint64_t *descriptor_content_serial_ptr(
    const BeerD3D12ComputeBinding *binding, uint32_t entry)
{
    const uint64_t *serial =
        binding->descriptor_resource_serial_locations[entry];
    if (getenv("BEER_D3D12_DIAGNOSTICS") && serial && entry == 0) {
        static uint32_t call_count = 0;
        if (++call_count <= 10)
            fprintf(stderr, "[D3D12 GRAPHICS] descriptor serial_ptr=%p value=%llu\n", 
                (const void *)serial, (unsigned long long)*serial);
    }
    return serial;  /* Return pointer, not dereferenced value */
}

static int root_visibility_matches_stage(uint32_t visibility, uint32_t stage)
{
    /* D3D12_SHADER_VISIBILITY_ALL == 0, VERTEX == 1, PIXEL == 5. Other
     * graphics-stage visibilities cannot satisfy a VS/PS declaration. */
    return visibility == 0 || visibility == (stage ? 5u : 1u);
}

static const BeerD3D12ComputeBinding *find_descriptor(
    const BeerD3D12GraphicsDraw *draw,
    const BeerD3D12ShaderDescriptor *descriptor, uint32_t stage,
    uint32_t *entry)
{
    for (uint32_t p = 0; p < BEER_D3D12_MAX_ROOT_PARAMETERS; ++p) {
        const BeerD3D12ComputeBinding *binding = &draw->bindings[p];
        if (!root_visibility_matches_stage(binding->shader_visibility, stage))
            continue;
        for (uint32_t i = 0; i < binding->descriptor_count &&
                             i < BEER_D3D12_MAX_RESOLVED_DESCRIPTORS; ++i) {
            if (binding->descriptor_types[i] == descriptor->type &&
                binding->descriptor_registers[i] ==
                    descriptor->register_index &&
                binding->descriptor_spaces[i] == descriptor->register_space &&
                descriptor_resource_class_matches(binding, i, descriptor)) {
                *entry = i;
                return binding;
            }
        }
    }
    return NULL;
}

int beer_d3d12_execute_vulkan_graphics_fast(
    const BeerD3D12GraphicsDraw *draw)
{
    const char *reason = NULL;
    return beer_d3d12_execute_vulkan_graphics_draw(draw, &reason);
}

uint32_t beer_d3d12_execute_vulkan_graphics_batch(
    const BeerD3D12GraphicsDraw *draw,
    const BeerD3D12GraphicsArguments *arguments, uint32_t argument_count)
{
    if (!draw || !arguments || !argument_count || !draw->vertex_shader ||
        !draw->vertex_shader_size)
        return 0;

    /* Every command in an ExecuteIndirect expansion shares immutable shader,
     * root, descriptor, attachment and fixed-function state. Translate that
     * state once at the batch boundary instead of re-entering the complete
     * scalar adapter for every generated command. Vulkan command recording is
     * still scalar for now, preserving the proven submission/fence behavior. */
    const BeerD3D12TranslatedShader *vertex =
        beer_d3d12_translate_shader(draw->vertex_shader,
                                    draw->vertex_shader_size,
                                    draw->vertex_shader_hash);
    const int vertex_only_depth = !draw->render_target_count &&
        (!draw->pixel_shader || !draw->pixel_shader_size);
    const BeerD3D12TranslatedShader *pixel = vertex_only_depth ? NULL :
        beer_d3d12_translate_shader(draw->pixel_shader,
                                    draw->pixel_shader_size,
                                    draw->pixel_shader_hash);
    if (!vertex || (!pixel && !vertex_only_depth))
        return 0;

    const char *reason = NULL;
    return beer_d3d12_execute_vulkan_graphics_prepared_batch(
        draw, vertex, pixel, arguments, argument_count, &reason);
}

int beer_d3d12_execute_vulkan_graphics_draw(
    const BeerD3D12GraphicsDraw *draw, const char **reason)
{
    static const char *unused;
    if (!reason) reason = &unused;
    *reason = "shader translation failed";
    if (!draw || !draw->vertex_shader || !draw->vertex_shader_size)
        return 0;
    const BeerD3D12TranslatedShader *vertex =
        beer_d3d12_translate_shader(draw->vertex_shader,
                                    draw->vertex_shader_size,
                                    draw->vertex_shader_hash);
    const int vertex_only_depth = !draw->render_target_count &&
        (!draw->pixel_shader || !draw->pixel_shader_size);
    const BeerD3D12TranslatedShader *pixel = vertex_only_depth ? NULL :
        beer_d3d12_translate_shader(draw->pixel_shader,
                                    draw->pixel_shader_size,
                                    draw->pixel_shader_hash);
    if (!vertex || (!pixel && !vertex_only_depth)) {
        const char *error = beer_d3d12_shader_translation_error();
        if (error && *error) *reason = error;
        return 0;
    }
    return beer_d3d12_execute_vulkan_graphics(draw, vertex, pixel, reason);
}

static int beer_d3d12_execute_vulkan_graphics_state(
    const BeerD3D12GraphicsDraw *draw,
    const BeerD3D12TranslatedShader *vertex,
    const BeerD3D12TranslatedShader *pixel,
    const BeerVulkanGraphicsDrawArguments *commands, uint32_t command_count,
    const char **reason);

static uint32_t beer_d3d12_execute_vulkan_graphics_batch_state(
    const BeerD3D12GraphicsDraw *draw,
    const BeerD3D12TranslatedShader *vertex,
    const BeerD3D12TranslatedShader *pixel,
    const BeerVulkanGraphicsDrawArguments *commands, uint32_t command_count,
    const char **reason)
{
    if (!commands || !command_count) return 0;
    return beer_d3d12_execute_vulkan_graphics_state(
        draw, vertex, pixel, commands, command_count, reason) ?
        command_count : 0;
}

uint32_t beer_d3d12_execute_vulkan_graphics_prepared_batch(
    const BeerD3D12GraphicsDraw *draw,
    const BeerD3D12TranslatedShader *vertex,
    const BeerD3D12TranslatedShader *pixel,
    const BeerD3D12GraphicsArguments *arguments, uint32_t argument_count,
    const char **reason)
{
    if (!draw || !vertex || !arguments || !argument_count) return 0;
    BeerVulkanGraphicsDrawArguments *commands =
        calloc(argument_count, sizeof(*commands));
    if (!commands) return 0;
    for (uint32_t i = 0; i < argument_count; ++i) {
        commands[i] = (BeerVulkanGraphicsDrawArguments){
            .element_count = arguments[i].element_count,
            .instance_count = arguments[i].instance_count,
            .start_element = arguments[i].start_element,
            .base_vertex = arguments[i].base_vertex,
            .start_instance = arguments[i].start_instance
        };
    }
    BeerD3D12GraphicsDraw operation = *draw;
    operation.element_count = commands[0].element_count;
    operation.instance_count = commands[0].instance_count;
    operation.start_element = commands[0].start_element;
    operation.base_vertex = commands[0].base_vertex;
    operation.start_instance = commands[0].start_instance;
    /* A Vulkan batch has one publication boundary. The final D3D command's
     * policy therefore applies to the complete batch. */
    operation.defer_render_target_publication =
        arguments[argument_count - 1].defer_render_target_publication;
    uint32_t executed = beer_d3d12_execute_vulkan_graphics_batch_state(
        &operation, vertex, pixel, commands, argument_count, reason);
    free(commands);
    return executed;
}

int beer_d3d12_execute_vulkan_graphics(
    const BeerD3D12GraphicsDraw *draw,
    const BeerD3D12TranslatedShader *vertex,
    const BeerD3D12TranslatedShader *pixel,
    const char **reason)
{
    return beer_d3d12_execute_vulkan_graphics_state(
        draw, vertex, pixel, NULL, 0, reason);
}

static int beer_d3d12_execute_vulkan_graphics_state(
    const BeerD3D12GraphicsDraw *draw,
    const BeerD3D12TranslatedShader *vertex,
    const BeerD3D12TranslatedShader *pixel,
    const BeerVulkanGraphicsDrawArguments *commands, uint32_t command_count,
    const char **reason)
{
    static const char *unused;
    if (!reason) reason = &unused;
    *reason = "invalid arguments";
    if (!draw || !vertex || !vertex->spirv ||
        (pixel && !pixel->spirv) ||
        (!pixel && draw->render_target_count) ||
        !draw->element_count || !draw->instance_count ||
        draw->render_target_count > BEER_VULKAN_GRAPHICS_MAX_TARGETS ||
        (!draw->render_target_count && !draw->depth_stencil) ||
        (draw->render_target_count &&
         (!draw->render_target_storage[0] ||
          !draw->render_target_serial_location[0] ||
          !draw->render_target_width[0] || !draw->render_target_height[0]))) {
        if (getenv("BEER_D3D12_DIAGNOSTICS") && draw) {
            static unsigned argument_traces;
            if (argument_traces++ < 16)
                fprintf(stderr,
                    "[D3D12 VULKAN GRAPHICS] invalid draw arguments "
                    "elements=%u instances=%u targets=%u storage=%p serial=%p "
                    "size=%ux%u\n",
                    draw->element_count, draw->instance_count,
                    draw->render_target_count,
                    (void *)draw->render_target_storage[0],
                    (void *)draw->render_target_serial_location[0],
                    draw->render_target_width[0], draw->render_target_height[0]);
        }
        return 0;
    }
    if (vertex->descriptor_count + (pixel ? pixel->descriptor_count : 0) >
        BEER_VULKAN_GRAPHICS_MAX_BINDINGS) {
        *reason = "too many graphics descriptors";
        return 0;
    }

    BeerVulkanGraphicsBinding bindings[BEER_VULKAN_GRAPHICS_MAX_BINDINGS];
    memset(bindings, 0, sizeof(bindings));
    uint32_t binding_count = 0;
    for (uint32_t stage = 0; stage < 2; ++stage) {
      const BeerD3D12TranslatedShader *shader = stage ? pixel : vertex;
      if (!shader) continue;
      const uint32_t stage_flag = stage ? BEER_VULKAN_GRAPHICS_STAGE_FRAGMENT :
          BEER_VULKAN_GRAPHICS_STAGE_VERTEX;
      for (uint32_t d = 0; d < shader->descriptor_count; ++d) {
        const BeerD3D12ShaderDescriptor *descriptor = &shader->descriptors[d];
        uint32_t output = binding_count;
        for (uint32_t existing = 0; existing < binding_count; ++existing) {
            if (bindings[existing].binding == descriptor->binding) {
                output = existing;
                break;
            }
        }
        if (output == binding_count) {
            if (binding_count >= BEER_VULKAN_GRAPHICS_MAX_BINDINGS) {
                *reason = "too many merged graphics descriptors";
                return 0;
            }
            ++binding_count;
        }
        BeerVulkanGraphicsBinding *mapped = &bindings[output];
        if (mapped->stage_flags) {
                uint32_t expected_kind = descriptor->type ==
                BEER_D3D12_SHADER_DESCRIPTOR_SAMPLER ?
                BEER_VULKAN_GRAPHICS_BINDING_SAMPLER :
                descriptor->type == BEER_D3D12_SHADER_DESCRIPTOR_CBV ?
                BEER_VULKAN_GRAPHICS_BINDING_UNIFORM_BUFFER :
                descriptor->resource_type ==
                    BEER_D3D12_SHADER_RESOURCE_BUFFER ?
                BEER_VULKAN_GRAPHICS_BINDING_STORAGE_BUFFER :
                BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_IMAGE;
            if (mapped->kind != expected_kind &&
                !(expected_kind ==
                      BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_IMAGE &&
                  mapped->kind ==
                      BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_DEPTH)) {
                if (getenv("BEER_D3D12_DIAGNOSTICS")) {
                    static unsigned collision_traces;
                    if (collision_traces++ < 16)
                        fprintf(stderr,
                            "[D3D12 VULKAN GRAPHICS] binding collision "
                            "b%u stage=%u type=%u resource=%u existing-kind=%u "
                            "expected-kind=%u reg=%u space=%u\n",
                            descriptor->binding, stage, descriptor->type,
                            descriptor->resource_type, mapped->kind,
                            expected_kind, descriptor->register_index,
                            descriptor->register_space);
                }
                *reason = "graphics descriptor binding collision";
                return 0;
            }
            mapped->stage_flags |= stage_flag;
            continue;
        }
        mapped->binding = descriptor->binding;
        mapped->stage_flags = stage_flag;
        if (descriptor->count != 1) {
            *reason = "fullscreen descriptor array not supported";
            return 0;
        }
        uint32_t entry = 0;
        const BeerD3D12ComputeBinding *source =
            find_descriptor(draw, descriptor, stage, &entry);
        if (descriptor->type == BEER_D3D12_SHADER_DESCRIPTOR_SAMPLER) {
            if (!source) {
                *reason = "fullscreen sampler not resolved";
                return 0;
            }
            const uint32_t *sampler = source->descriptor_samplers[entry];
            uint32_t filter = sampler[0];
            uint32_t reduction = (filter >> 7) & 3u;
            uint32_t comparison = sampler[6];
            if (reduction == 1u && (comparison < 1u || comparison > 8u)) {
                *reason = "fullscreen comparison sampler invalid";
                return 0;
            }
            mapped->kind = BEER_VULKAN_GRAPHICS_BINDING_SAMPLER;
            mapped->min_filter = (filter >> 4) & 1u;
            mapped->mag_filter = (filter >> 2) & 1u;
            mapped->mipmap_mode = filter & 1u;
            mapped->anisotropy_enable = (filter & 0x40u) != 0;
            mapped->max_anisotropy = mapped->anisotropy_enable ?
                (float)(sampler[5] ? sampler[5] : 1u) : 1.0f;
            mapped->reduction_mode = reduction == 2u ? 1u :
                                     reduction == 3u ? 2u : 0u;
            mapped->compare_enable = reduction == 1u;
            mapped->compare_op = comparison ? comparison - 1u : 0u;
            mapped->border_color = sampler[7];
            uint32_t *addresses[3] = {&mapped->address_u,
                                      &mapped->address_v,
                                      &mapped->address_w};
            for (uint32_t axis = 0; axis < 3; ++axis) {
                uint32_t address = sampler[axis + 1];
                if (address < 1u || address > 5u) {
                    *reason = "fullscreen sampler address unsupported";
                    return 0;
                }
                static const uint32_t address_map[5] = {
                    0, /* WRAP -> REPEAT */
                    1, /* MIRROR -> MIRRORED_REPEAT */
                    2, /* CLAMP -> CLAMP_TO_EDGE */
                    3, /* BORDER -> CLAMP_TO_BORDER */
                    4  /* MIRROR_ONCE -> MIRROR_CLAMP_TO_EDGE */
                };
                *addresses[axis] = address_map[address - 1u];
            }
            continue;
        }
        if (descriptor->type == BEER_D3D12_SHADER_DESCRIPTOR_CBV) {
            if (!source) {
                *reason = "fullscreen CBV not resolved";
                return 0;
            }
            const uint8_t *data = NULL;
            size_t size = 0;
            if (source->descriptor_resource_storage[entry] &&
                source->descriptor_resource_storage_size[entry]) {
                data = source->descriptor_resource_storage[entry];
                size = source->descriptor_resource_storage_size[entry];
            } else if (source->resource_storage &&
                       source->resource_storage_size) {
                data = source->resource_storage;
                size = source->resource_storage_size;
            }
            if (!data || !size || size > 65536) {
                *reason = "fullscreen CBV storage unavailable";
                return 0;
            }
            mapped->kind = BEER_VULKAN_GRAPHICS_BINDING_UNIFORM_BUFFER;
            mapped->data = data;
            mapped->size = size;
            continue;
        }
        if ((descriptor->type == BEER_D3D12_SHADER_DESCRIPTOR_SRV ||
             descriptor->type == BEER_D3D12_SHADER_DESCRIPTOR_UAV) &&
            descriptor->resource_type == BEER_D3D12_SHADER_RESOURCE_BUFFER) {
            if (!source || !source->descriptor_resource_storage[entry] ||
                !source->descriptor_resource_storage_size[entry]) {
                *reason = "fullscreen buffer SRV not resolved";
                return 0;
            }
            size_t buffer_offset = 0;
            uint64_t first = source->descriptor_view_first_elements[entry];
            uint32_t view_format = source->descriptor_view_formats[entry];
            if (view_format == 39 || view_format == 41 ||
                view_format == 42 || view_format == 43) {
                if (first > SIZE_MAX / 4u) {
                    *reason = "fullscreen buffer SRV offset invalid";
                    return 0;
                }
                buffer_offset = (size_t)first * 4u;
            }
            if (buffer_offset >=
                source->descriptor_resource_storage_size[entry]) {
                *reason = "fullscreen buffer SRV storage too small";
                return 0;
            }
            mapped->kind = descriptor->type ==
                BEER_D3D12_SHADER_DESCRIPTOR_UAV ?
                BEER_VULKAN_GRAPHICS_BINDING_STORAGE_BUFFER :
                BEER_VULKAN_GRAPHICS_BINDING_UNIFORM_TEXEL_BUFFER;
            mapped->data = source->descriptor_resource_storage[entry] +
                           buffer_offset;
            mapped->size = source->descriptor_resource_storage_size[entry] -
                           buffer_offset;
            /* vkd3d-shader represents raw/structured Buffer<T> SRVs as
             * usamplerBuffer. Raw and UNKNOWN views are R32_UINT; typed views
             * retain their declared DXGI format. Pixel-shader UAV buffers use
             * a Vulkan storage buffer and therefore need no texel view. */
            if (descriptor->type == BEER_D3D12_SHADER_DESCRIPTOR_SRV) {
                if (view_format == 0 || view_format == 39)
                    dxgi_to_texel_format(42, &mapped->texel_format,
                                         &mapped->texel_bytes);
                else
                    dxgi_to_texel_format(view_format, &mapped->texel_format,
                                         &mapped->texel_bytes);
                if (!mapped->texel_format || !mapped->texel_bytes ||
                    mapped->size < mapped->texel_bytes) {
                    *reason = "fullscreen buffer SRV format unsupported";
                    return 0;
                }
                mapped->size -= mapped->size % mapped->texel_bytes;
            }
            continue;
        }
        const int texture_1d = descriptor->resource_type ==
            BEER_D3D12_SHADER_RESOURCE_TEXTURE_1D;
        const int texture_1d_array = descriptor->resource_type ==
            BEER_D3D12_SHADER_RESOURCE_TEXTURE_1D_ARRAY;
        const int texture_2d = descriptor->resource_type ==
            BEER_D3D12_SHADER_RESOURCE_TEXTURE_2D;
        const int texture_2d_array = descriptor->resource_type ==
            BEER_D3D12_SHADER_RESOURCE_TEXTURE_2D_ARRAY;
        const int texture_3d = descriptor->resource_type ==
            BEER_D3D12_SHADER_RESOURCE_TEXTURE_3D;
        const int texture_cube = descriptor->resource_type ==
            BEER_D3D12_SHADER_RESOURCE_TEXTURE_CUBE;
        const int texture_cube_array = descriptor->resource_type ==
            BEER_D3D12_SHADER_RESOURCE_TEXTURE_CUBE_ARRAY;
        if (descriptor->type != BEER_D3D12_SHADER_DESCRIPTOR_SRV ||
            (!texture_1d && !texture_1d_array && !texture_2d &&
             !texture_2d_array && !texture_3d && !texture_cube &&
             !texture_cube_array)) {
            if (getenv("BEER_D3D12_DIAGNOSTICS"))
                fprintf(stderr,
                    "[D3D12 VULKAN GRAPHICS] unsupported descriptor "
                    "class binding=%u type=%u resource=%u data=%u count=%u "
                    "space=%u reg=%u view-dim=%u resource-dim=%u layers=%u\n",
                    descriptor->binding, descriptor->type,
                    descriptor->resource_type, descriptor->resource_data_type,
                    descriptor->count, descriptor->register_space,
                    descriptor->register_index,
                    source ? source->descriptor_view_dimensions[entry] : 0,
                    source ? source->descriptor_resource_dimensions[entry] : 0,
                    source ? source->descriptor_resource_layers[entry] : 0);
            *reason = "fullscreen descriptor class not supported";
            return 0;
        }
        if (!source || !source->descriptor_resource_storage[entry] ||
            !source->descriptor_widths[entry] ||
            !source->descriptor_heights[entry]) {
            if (getenv("BEER_D3D12_DIAGNOSTICS")) {
                static unsigned srv_unresolved_traces;
                if (srv_unresolved_traces++ < 12) {
                    fprintf(stderr,
                        "[D3D12 VULKAN GRAPHICS] fullscreen SRV unresolved "
                        "stage=%u b%u t%u space%u shader-resource=%u "
                        "elements=%u matched=%d\n",
                        stage, descriptor->binding,
                        descriptor->register_index,
                        descriptor->register_space,
                        descriptor->resource_type, draw->element_count,
                        source != NULL);
                    if (source) {
                        fprintf(stderr,
                            "[D3D12 VULKAN GRAPHICS]   entry=%u resource=%p "
                            "storage=%p storage-size=%zu size=%ux%u "
                            "view-dim=%u resource-dim=%u layers=%u samples=%u "
                            "view-first-layer=%u view-layer-count=%u "
                            "view-mips=%u raw-size=%u\n",
                            entry,
                            (const void *)source->descriptor_resources[entry],
                            (const void *)
                                source->descriptor_resource_storage[entry],
                            source->descriptor_resource_storage_size[entry],
                            source->descriptor_widths[entry],
                            source->descriptor_heights[entry],
                            source->descriptor_view_dimensions[entry],
                            source->descriptor_resource_dimensions[entry],
                            source->descriptor_resource_layers[entry],
                            source->descriptor_resource_samples[entry],
                            source->descriptor_view_first_layers[entry],
                            source->descriptor_view_layer_counts[entry],
                            source->descriptor_view_mip_levels[entry],
                            source->descriptor_raw_sizes[entry]);
                        fprintf(stderr, "[D3D12 VULKAN GRAPHICS]   raw");
                        for (uint32_t byte = 0;
                             byte < source->descriptor_raw_sizes[entry] &&
                             byte < 32; ++byte)
                            fprintf(stderr, " %02x",
                                    source->descriptor_raw_bytes[entry][byte]);
                        fprintf(stderr, "\n");
                    }
                    for (uint32_t p = 0;
                         p < BEER_D3D12_MAX_ROOT_PARAMETERS; ++p) {
                        const BeerD3D12ComputeBinding *b = &draw->bindings[p];
                        for (uint32_t i = 0;
                             i < b->descriptor_count &&
                             i < BEER_D3D12_MAX_RESOLVED_DESCRIPTORS; ++i) {
                            if (b->descriptor_types[i] !=
                                BEER_D3D12_SHADER_DESCRIPTOR_SRV)
                                continue;
                            fprintf(stderr,
                                "[D3D12 VULKAN GRAPHICS]   draw-srv param=%u "
                                "visibility=%u t%u space%u view-dim=%u "
                                "resource-dim=%u layers=%u samples=%u "
                                "view-first-layer=%u view-layer-count=%u "
                                "view-mips=%u resource=%p storage=%p raw",
                                p,
                                b->shader_visibility,
                                b->descriptor_registers[i],
                                b->descriptor_spaces[i],
                                b->descriptor_view_dimensions[i],
                                b->descriptor_resource_dimensions[i],
                                b->descriptor_resource_layers[i],
                                b->descriptor_resource_samples[i],
                                b->descriptor_view_first_layers[i],
                                b->descriptor_view_layer_counts[i],
                                b->descriptor_view_mip_levels[i],
                                (const void *)b->descriptor_resources[i],
                                (const void *)
                                    b->descriptor_resource_storage[i]);
                            for (uint32_t byte = 0;
                                 byte < b->descriptor_raw_sizes[i] &&
                                 byte < 32; ++byte)
                                fprintf(stderr, " %02x",
                                        b->descriptor_raw_bytes[i][byte]);
                            fprintf(stderr, "\n");
                        }
                    }
                }
            }
            *reason = "fullscreen SRV not resolved";
            return 0;
        }
        uint32_t resource_dxgi = source->descriptor_formats[entry];
        uint32_t dxgi = source->descriptor_view_formats[entry];
        if (!dxgi) dxgi = resource_dxgi;
        uint32_t format = 0, bytes = 0, block_bytes = 0;
        const int compressed =
            dxgi_compressed_to_vulkan(dxgi, &format, &block_bytes);
        if ((!compressed &&
             (!dxgi_to_vulkan(dxgi, &format, &bytes) ||
              (bytes != 1 && bytes != 2 && bytes != 4 && bytes != 8)))) {
            if (getenv("BEER_D3D12_DIAGNOSTICS")) {
                static unsigned format_traces;
                if (format_traces++ < 24)
                    fprintf(stderr,
                        "[D3D12 VULKAN GRAPHICS] unsupported SRV format "
                        "b%u t%u space%u resource-type=%u resource-format=%u "
                        "view-format=%u size=%ux%u\n",
                        descriptor->binding, descriptor->register_index,
                        descriptor->register_space, descriptor->resource_type,
                        resource_dxgi, dxgi,
                        source->descriptor_widths[entry],
                        source->descriptor_heights[entry]);
            }
            *reason = "fullscreen SRV format unsupported";
            return 0;
        }
        const int depth_aspect = resource_dxgi == 19 && dxgi == 21;
        if ((resource_dxgi == 19 || dxgi == 21) && !depth_aspect) {
            *reason = "fullscreen depth view combination unsupported";
            return 0;
        }
        /* Descriptor resolution already advances storage to the selected
         * subresource and shrinks its available size. The absolute offset is
         * retained only as metadata; applying it again here corrupts every
         * nonzero mip or array-layer view. */
        size_t offset = source->descriptor_subresource_offsets[entry];
        size_t available = source->descriptor_resource_storage_size[entry];
        uint32_t width = source->descriptor_widths[entry];
        uint32_t height = source->descriptor_heights[entry];
        size_t pitch = source->descriptor_row_pitches[entry];
        uint32_t layer_count = 1;
        uint32_t depth = texture_3d ?
            source->descriptor_depths[entry] : 1;
        if (!depth) depth = 1;
        if (texture_3d &&
            (source->descriptor_view_dimensions[entry] != 8 ||
             source->descriptor_resource_dimensions[entry] != 4 ||
             source->descriptor_resource_samples[entry] != 1)) {
            *reason = "fullscreen 3D view metadata invalid";
            return 0;
        }
        if ((texture_1d || texture_1d_array) && height != 1) {
            *reason = "fullscreen 1D view height invalid";
            return 0;
        }
        if (texture_1d_array || texture_2d_array || texture_cube ||
            texture_cube_array) {
            uint32_t first_layer = texture_cube ? 0u :
                source->descriptor_view_first_layers[entry];
            uint32_t resource_layers =
                source->descriptor_resource_layers[entry];
            if (texture_cube) {
                layer_count = beer_d3d12_graphics_resolve_layer_count(
                    0, 6, resource_layers);
            } else if (texture_cube_array) {
                /* D3D12 stores NumCubes in the cube-array SRV. Vulkan views
                 * require the corresponding face count. UINT_MAX denotes all
                 * remaining cubes, just as it does for ordinary array SRVs. */
                uint32_t cube_count =
                    source->descriptor_view_layer_counts[entry];
                uint32_t available_faces = first_layer < resource_layers ?
                    resource_layers - first_layer : 0;
                if (cube_count == UINT32_MAX)
                    layer_count = available_faces - available_faces % 6u;
                else if (cube_count && cube_count <= UINT32_MAX / 6u)
                    layer_count = cube_count * 6u;
                else
                    layer_count = 0;
                layer_count = beer_d3d12_graphics_resolve_layer_count(
                    first_layer, layer_count, resource_layers);
            } else {
                layer_count = beer_d3d12_graphics_resolve_layer_count(
                    first_layer,
                    source->descriptor_view_layer_counts[entry],
                    resource_layers);
            }
            if (!layer_count) {
                if (getenv("BEER_D3D12_DIAGNOSTICS"))
                    fprintf(stderr,
                        "[D3D12 VULKAN GRAPHICS] invalid array view "
                        "binding=%u shader-resource=%u view-dim=%u "
                        "first=%u declared=%u resource-layers=%u\n",
                        descriptor->binding, descriptor->resource_type,
                        source->descriptor_view_dimensions[entry], first_layer,
                        source->descriptor_view_layer_counts[entry],
                        resource_layers);
                *reason = texture_cube_array ?
                    "fullscreen cube-array view range invalid" :
                    "fullscreen array view range invalid";
                return 0;
            }
            if (source->descriptor_resource_samples[entry] != 1) {
                *reason = "fullscreen multisample array unsupported";
                return 0;
            }
        }
        uint32_t source_bytes = bytes;
        if (compressed) {
            uint32_t blocks_x = (width + 3u) / 4u;
            uint32_t blocks_y = (height + 3u) / 4u;
            size_t minimum_pitch = (size_t)blocks_x * block_bytes;
            if (!pitch || pitch < minimum_pitch) pitch = minimum_pitch;
            if (pitch > UINT32_MAX || !blocks_y ||
                pitch > SIZE_MAX / blocks_y ||
                pitch * blocks_y > available) {
                *reason = "fullscreen compressed SRV layout invalid";
                return 0;
            }
            mapped->kind = BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_IMAGE;
            mapped->resource = source->descriptor_resources[entry];
            mapped->serial = descriptor_content_serial_ptr(source, entry);
            mapped->data = source->descriptor_resource_storage[entry];
            mapped->size = available;
            mapped->format = format;
            mapped->block_width = 4;
            mapped->block_height = 4;
            mapped->bytes_per_block = block_bytes;
            mapped->width = width;
            mapped->height = height;
            mapped->row_pitch = (uint32_t)pitch;
            mapped->layer_count = layer_count;
            mapped->depth = depth;
            mapped->image_dimension = (texture_1d || texture_1d_array) ? 1u :
                texture_3d ? 3u : 2u;
            mapped->image_cube = texture_cube || texture_cube_array;
            uint32_t resource_mips = source->descriptor_resource_mips[entry] ?
                source->descriptor_resource_mips[entry] : 1u;
            uint32_t selected_mip =
                source->descriptor_subresources[entry] % resource_mips;
            uint32_t mip_levels = source->descriptor_view_mip_levels[entry];
            if (!mip_levels || mip_levels == UINT32_MAX)
                mip_levels = resource_mips - selected_mip;
            if (!mip_levels || selected_mip >= resource_mips ||
                mip_levels > resource_mips - selected_mip) {
                *reason = "fullscreen compressed mip range invalid";
                return 0;
            }
            mapped->mip_levels = mip_levels;
            if (mip_levels > 1 &&
                (texture_1d_array || texture_2d_array)) {
                size_t layer_stride = 0;
                uint32_t full_width = width << selected_mip;
                uint32_t full_height = height << selected_mip;
                for (uint32_t mip = 0; mip < resource_mips; ++mip) {
                    uint32_t mip_width = full_width >> mip;
                    uint32_t mip_height = full_height >> mip;
                    if (!mip_width) mip_width = 1;
                    if (!mip_height) mip_height = 1;
                    size_t mip_pitch =
                        (size_t)((mip_width + 3u) / 4u) * block_bytes;
                    uint32_t mip_rows = (mip_height + 3u) / 4u;
                    if (mip_pitch > SIZE_MAX / mip_rows ||
                        layer_stride > SIZE_MAX - mip_pitch * mip_rows) {
                        *reason = "fullscreen compressed layer stride overflow";
                        return 0;
                    }
                    layer_stride += mip_pitch * mip_rows;
                }
                mapped->layer_stride = layer_stride;
            }
            continue;
        }
        if (depth_aspect) {
            /* Beer historically allocates some format-19 resources as a
             * depth-only 4-byte plane, while other callers retain the native
             * 8-byte R32G8X24 texel. Follow the captured host row layout: both
             * representations expose the same leading R32_FLOAT aspect. */
            if (pitch >= (size_t)width * 8u)
                source_bytes = 8u;
            else if (!pitch && height &&
                     (size_t)width <= SIZE_MAX / height / 8u &&
                     (size_t)width * height * 8u <=
                         source->descriptor_resource_storage_size[entry] -
                         offset)
                source_bytes = 8u;
        }
        const size_t minimum_pitch = (size_t)width * source_bytes;
        if (!pitch) pitch = minimum_pitch;
        if (pitch > UINT32_MAX || pitch < minimum_pitch) {
            if (getenv("BEER_D3D12_DIAGNOSTICS")) {
                static unsigned pitch_traces;
                if (pitch_traces++ < 24)
                    fprintf(stderr,
                        "[D3D12 VULKAN GRAPHICS] invalid SRV pitch "
                        "b%u t%u space%u resource-type=%u resource-format=%u "
                        "view-format=%u size=%ux%u pitch=%zu texel=%u "
                        "offset=%zu storage=%zu\n",
                        descriptor->binding, descriptor->register_index,
                        descriptor->register_space, descriptor->resource_type,
                        resource_dxgi, dxgi, width, height, pitch, source_bytes,
                        offset, source->descriptor_resource_storage_size[entry]);
            }
            *reason = "fullscreen SRV pitch invalid";
            return 0;
        }
        size_t layer_extent = pitch * (height - 1) +
                              (size_t)width * source_bytes;
        uint32_t image_count = texture_3d ? depth : layer_count;
        if (!layer_extent || image_count > SIZE_MAX / layer_extent ||
            layer_extent * image_count > available) {
            *reason = "fullscreen SRV storage too small";
            return 0;
        }
        mapped->kind = depth_aspect ?
            BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_DEPTH :
            BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_IMAGE;
        mapped->resource = source->descriptor_resources[entry];
        mapped->serial = descriptor_content_serial_ptr(source, entry);
        mapped->data = source->descriptor_resource_storage[entry];
        mapped->size = available;
        mapped->format = format;
        mapped->bytes_per_pixel = bytes;
        mapped->source_bytes_per_pixel = source_bytes;
        mapped->width = width;
        mapped->height = height;
        mapped->row_pitch = (uint32_t)pitch;
        mapped->layer_count = layer_count;
        mapped->depth = depth;
        mapped->image_dimension = (texture_1d || texture_1d_array) ? 1u :
            texture_3d ? 3u : 2u;
        mapped->image_cube = texture_cube || texture_cube_array;
        uint32_t resource_mips = source->descriptor_resource_mips[entry] ?
            source->descriptor_resource_mips[entry] : 1u;
        uint32_t selected_mip =
            source->descriptor_subresources[entry] % resource_mips;
        uint32_t mip_levels = source->descriptor_view_mip_levels[entry];
        if (!mip_levels || mip_levels == UINT32_MAX)
            mip_levels = resource_mips - selected_mip;
        if (!mip_levels || selected_mip >= resource_mips ||
            mip_levels > resource_mips - selected_mip) {
            *reason = "fullscreen texture mip range invalid";
            return 0;
        }
        mapped->mip_levels = mip_levels;
        if (mip_levels > 1 && (texture_1d_array || texture_2d_array)) {
            size_t layer_stride = 0;
            uint32_t full_width = width << selected_mip;
            uint32_t full_height = height << selected_mip;
            for (uint32_t mip = 0; mip < resource_mips; ++mip) {
                uint32_t mip_width = full_width >> mip;
                uint32_t mip_height = full_height >> mip;
                if (!mip_width) mip_width = 1;
                if (!mip_height) mip_height = 1;
                size_t mip_pitch = compressed ?
                    (size_t)((mip_width + 3u) / 4u) * block_bytes :
                    (size_t)mip_width * bytes;
                uint32_t mip_rows = compressed ?
                    (mip_height + 3u) / 4u : mip_height;
                if (mip_pitch > SIZE_MAX / mip_rows ||
                    layer_stride > SIZE_MAX - mip_pitch * mip_rows) {
                    *reason = "fullscreen texture layer stride overflow";
                    return 0;
                }
                layer_stride += mip_pitch * mip_rows;
            }
            mapped->layer_stride = layer_stride;
        }
        mapped->aspect = depth_aspect ? 1u : 0u;
      }
    }

    BeerVulkanGraphicsTarget targets[BEER_VULKAN_GRAPHICS_MAX_TARGETS];
    memset(targets, 0, sizeof(targets));
    for (uint32_t i = 0; i < draw->render_target_count; ++i) {
        uint32_t target_format = 0, target_bytes = 0;
        if (!draw->render_target_storage[i] ||
            !draw->render_target_serial_location[i] ||
            !draw->render_target_width[i] || !draw->render_target_height[i] ||
            !dxgi_to_vulkan(draw->render_target_format[i], &target_format,
                            &target_bytes) ||
            (target_bytes != 1 && target_bytes != 2 &&
             target_bytes != 4 && target_bytes != 8)) {
            if (getenv("BEER_D3D12_DIAGNOSTICS")) {
                static unsigned target_traces;
                if (target_traces++ < 16)
                    fprintf(stderr,
                        "[D3D12 VULKAN GRAPHICS] unsupported render target "
                        "index=%u dxgi=%u size=%ux%u storage=%zu\n", i,
                        draw->render_target_format[i],
                        draw->render_target_width[i],
                        draw->render_target_height[i],
                        draw->render_target_storage_size[i]);
            }
            *reason = "render-target format unsupported";
            return 0;
        }
        if (i && (draw->render_target_width[i] !=
                      draw->render_target_width[0] ||
                  draw->render_target_height[i] !=
                      draw->render_target_height[0])) {
            *reason = "render-target dimensions mismatch";
            return 0;
        }
        targets[i] = (BeerVulkanGraphicsTarget){
            .resource = draw->render_targets[i],
            .data = draw->render_target_storage[i],
            .size = draw->render_target_storage_size[i],
            .serial = draw->render_target_serial_location[i],
            .format = target_format,
            .bytes_per_pixel = target_bytes,
            .width = draw->render_target_width[i],
            .height = draw->render_target_height[i],
            .row_pitch = draw->render_target_width[i] * target_bytes,
            .defer_publication = draw->defer_render_target_publication
        };
    }
    BeerVulkanGraphicsDrawInfo draw_info;
    BeerVulkanGraphicsDrawInfo *draw_info_ptr = &draw_info;
    memset(&draw_info, 0, sizeof(draw_info));
    draw_info.topology =
        beer_d3d12_graphics_map_primitive_topology(draw->primitive_topology);
    if (draw_info.topology == UINT32_MAX) {
        *reason = "graphics topology unsupported";
        return 0;
    }
    /* A pipeline can retain an input-layout declaration while a draw binds no
     * vertex buffers and generates positions from SV_VertexID. In that case
     * D3D12 does not require the stale layout to become active IA state; emit
     * an empty Vulkan vertex-input state instead of rejecting the draw. Bound
     * buffers with no declared elements are likewise irrelevant to the VS. */
    if (draw->input_element_count && draw->vertex_buffer_mask) {
        for (uint32_t slot = 0;
             slot < BEER_D3D12_MAX_VERTEX_BUFFERS; ++slot) {
            if (!(draw->vertex_buffer_mask & (UINT32_C(1) << slot))) continue;
            if (slot >= BEER_VULKAN_GRAPHICS_MAX_VERTEX_BINDINGS ||
                !draw->vertex_buffer_storage[slot] ||
                !draw->vertex_buffer_storage_size[slot] ||
                !draw->vertex_buffers[slot].stride_or_format) {
                *reason = "graphics vertex buffer unresolved";
                return 0;
            }
            draw_info.vertex_bindings[slot] =
                (BeerVulkanGraphicsVertexBinding){
                    .data = draw->vertex_buffer_storage[slot],
                    .size = draw->vertex_buffer_storage_size[slot],
                    .stride = draw->vertex_buffers[slot].stride_or_format,
                    .input_rate = UINT32_MAX
                };
            if (draw_info.vertex_binding_count <= slot)
                draw_info.vertex_binding_count = slot + 1;
        }
        for (uint32_t element = 0;
             element < draw->input_element_count; ++element) {
            const BeerD3D12InputElement *input =
                &draw->input_elements[element];
            uint32_t format = 0, bytes = 0;
            if (!dxgi_to_vulkan(input->format, &format, &bytes) ||
                input->input_slot >= draw_info.vertex_binding_count ||
                input->input_class > 1 ||
                (input->input_class == 0 && input->instance_step_rate) ||
                (input->input_class == 1 && input->instance_step_rate != 1)) {
                *reason = "graphics input layout unsupported";
                return 0;
            }
            uint32_t input_rate = input->input_class ? 1u : 0u;
            BeerVulkanGraphicsVertexBinding *vertex_binding =
                &draw_info.vertex_bindings[input->input_slot];
            if (vertex_binding->input_rate != UINT32_MAX &&
                vertex_binding->input_rate != input_rate) {
                *reason = "mixed input rates in one vertex slot";
                return 0;
            }
            vertex_binding->input_rate = input_rate;
            draw_info.vertex_attributes[element] =
                (BeerVulkanGraphicsVertexAttribute){
                    .location = element,
                    .binding = input->input_slot,
                    .format = format,
                    .offset = input->aligned_byte_offset
                };
        }
        draw_info.vertex_attribute_count = draw->input_element_count;
        for (uint32_t slot = 0; slot < draw_info.vertex_binding_count; ++slot)
            if (draw_info.vertex_bindings[slot].input_rate == UINT32_MAX)
                draw_info.vertex_bindings[slot].input_rate = 0;
    }
    if (draw->indexed) {
        if (!draw->index_buffer_storage ||
            (draw->index_buffer.stride_or_format != 57 &&
             draw->index_buffer.stride_or_format != 42)) {
            *reason = "indexed graphics state unsupported";
            return 0;
        }
        draw_info.index_data = draw->index_buffer_storage;
        draw_info.index_size = draw->index_buffer_storage_size;
        draw_info.index_type =
            draw->index_buffer.stride_or_format == 42 ? 1u : 0u;
        const size_t index_stride = draw_info.index_type ? 4u : 2u;
        if ((size_t)draw->start_element > SIZE_MAX / index_stride ||
            (size_t)draw->element_count >
                (SIZE_MAX / index_stride) - (size_t)draw->start_element) {
            *reason = "indexed draw range overflow";
            return 0;
        }
        size_t required = ((size_t)draw->start_element +
                           (size_t)draw->element_count) * index_stride;
        if (required > draw_info.index_size && !draw->robust_index_access) {
            *reason = "indexed draw range exceeds index-buffer view";
            return 0;
        }
        draw_info.robust_index_access = draw->robust_index_access;
        draw_info.indexed = 1;
    }
    draw_info.element_count = draw->element_count;
    draw_info.instance_count = draw->instance_count;
    draw_info.start_element = draw->start_element;
    draw_info.base_vertex = draw->base_vertex;
    draw_info.start_instance = draw->start_instance;
    draw_info.batch_arguments = commands;
    draw_info.batch_argument_count = command_count;
    draw_info.debug_submission = draw->debug_submission;
    draw_info.debug_sequence = draw->debug_sequence;
    draw_info.debug_list_index = draw->debug_list_index;
    draw_info.debug_vertex_hash = draw->vertex_shader_hash;
    draw_info.debug_pixel_hash = draw->pixel_shader_hash;
    BeerVulkanGraphicsState graphics_state;
    if (!map_pipeline_state(draw, &graphics_state, reason)) return 0;
    BeerVulkanGraphicsDepthTarget depth_target;
    BeerVulkanGraphicsDepthTarget *depth_target_ptr = NULL;
    if (graphics_state.depth_test_enable || graphics_state.stencil_test_enable) {
        uint32_t source_bytes =
            draw->depth_stencil_source_bytes_per_pixel;
        uint32_t vk_format = 126; /* VK_FORMAT_D32_SFLOAT */
        uint32_t depth_format = draw->depth_stencil_format;
        switch (depth_format) {
        case 19: /* R32G8X24_TYPELESS */
        case 20: /* D32_FLOAT_S8X24_UINT */
            if (source_bytes != 4 && source_bytes != 8) source_bytes = 8;
            if (graphics_state.stencil_test_enable) {
                if (source_bytes != 8) {
                    *reason = "stencil backing layout unsupported";
                    return 0;
                }
                vk_format = 130; /* VK_FORMAT_D32_SFLOAT_S8_UINT */
            }
            break;
        case 39: /* R32_TYPELESS backing for a D32 view */
        case 40: /* D32_FLOAT */
            source_bytes = 4;
            if (graphics_state.stencil_test_enable) {
                *reason = "stencil attachment format unsupported";
                return 0;
            }
            break;
        case 44: /* R24G8_TYPELESS */
        case 45: /* D24_UNORM_S8_UINT */
            source_bytes = 4;
            vk_format = graphics_state.stencil_test_enable ?
                130u : 126u; /* Expand packed D24(/S8) into D32(/S8). */
            break;
        case 53: /* R16_TYPELESS */
            /* Accepting the RE8 512x512 R16_TYPELESS shadow draws (vs
             * 03903ab3...) reproducibly loses the Vulkan device within a few
             * draws; keep them rejected until that is understood. */
            if (!getenv("BEER_D3D12_ALLOW_R16_TYPELESS_DEPTH")) {
                *reason = "depth attachment format unsupported";
                return 0;
            }
            /* fallthrough */
        case 55: /* D16_UNORM */
            source_bytes = 2;
            vk_format = 126; /* Expand normalized D16 into D32_SFLOAT. */
            if (graphics_state.stencil_test_enable) {
                *reason = "D16 stencil attachment unsupported";
                return 0;
            }
            break;
        default: {
            static _Atomic uint32_t unsupported_depth_reports;
            if (getenv("BEER_D3D12_DIAGNOSTICS") ||
                atomic_fetch_add(&unsupported_depth_reports, 1) < 8)
                fprintf(stderr,
                    "[D3D12 VULKAN GRAPHICS] unsupported depth attachment "
                    "resource-format=%u pso-format=%u source-bytes=%u "
                    "size=%ux%u storage=%zu\n",
                    draw->depth_stencil_format,
                    draw->pso.depth_stencil_format, source_bytes,
                    draw->depth_stencil_width, draw->depth_stencil_height,
                    draw->depth_stencil_storage_size);
            *reason = "depth attachment format unsupported";
            return 0;
        }
        }
        if (!draw->depth_stencil_width || !draw->depth_stencil_height ||
            draw->depth_stencil_width > UINT32_MAX / source_bytes) {
            *reason = "depth attachment dimensions invalid";
            return 0;
        }
        uint32_t row_pitch = draw->depth_stencil_width * source_bytes;
        size_t required = (size_t)row_pitch * draw->depth_stencil_height;
        if (required > draw->depth_stencil_storage_size) {
            *reason = "depth attachment storage too small";
            return 0;
        }
        depth_target = (BeerVulkanGraphicsDepthTarget){
            .resource = draw->depth_stencil,
            .data = draw->depth_stencil_storage,
            .size = draw->depth_stencil_storage_size,
            .serial = draw->depth_stencil_serial_location,
            .format = vk_format,
            .bytes_per_pixel = 4,
            .source_bytes_per_pixel = source_bytes,
            .source_format = depth_format,
            .width = draw->depth_stencil_width,
            .height = draw->depth_stencil_height,
            .row_pitch = row_pitch,
            .sample_count = graphics_state.sample_count,
            .has_stencil = graphics_state.stencil_test_enable,
            .stencil_byte_offset =
                (depth_format == 44 || depth_format == 45) ? 3u : 4u
        };
        depth_target_ptr = &depth_target;
    }
    const int trace_legacy_black_pass =
        getenv("BEER_D3D12_TRACE_BLACK_PASS") &&
        draw->vertex_shader_hash == UINT64_C(0x856602cd4329df1f) &&
        draw->pixel_shader_hash == UINT64_C(0x2c6f3c919d128903);
    const int trace_title_pipeline =
        getenv("BEER_D3D12_TRACE_TEMPORAL_INPUTS") != NULL;
    const int trace_hdr_convert_pass = trace_title_pipeline &&
        draw->vertex_shader_hash == UINT64_C(0xe5f478f2bdc20aa9) &&
        draw->pixel_shader_hash == UINT64_C(0x2f8c988900533e4c);
    const int trace_scene_pass = trace_title_pipeline &&
        draw->vertex_shader_hash == UINT64_C(0x0de6bdd383cec2ba) &&
        draw->pixel_shader_hash == UINT64_C(0x446eecd94f8eb3ed);
    const int trace_temporal_pass = trace_title_pipeline &&
        draw->vertex_shader_hash == UINT64_C(0x856602cd4329df1f) &&
        draw->pixel_shader_hash == UINT64_C(0x413d419559c6d998);
    if (trace_legacy_black_pass || trace_hdr_convert_pass || trace_scene_pass ||
        trace_temporal_pass) {
        static unsigned pass_input_traces;
        if (pass_input_traces++ < 256) {
            const char *trace_name = trace_temporal_pass ? "TEMPORAL" :
                trace_scene_pass ? "SCENE" :
                trace_hdr_convert_pass ? "HDR CONVERT" : "BLACK";
            fprintf(stderr,
                    "[D3D12 %s PASS] submission=%llu list=%u sequence=%llu "
                    "target=%p target-serial=%llu before target-nonzero=",
                    trace_name, (unsigned long long)draw->debug_submission,
                    draw->debug_list_index,
                    (unsigned long long)draw->debug_sequence,
                    draw->render_targets[0],
                    (unsigned long long)*draw->render_target_serial_location[0]);
            size_t target_nonzero = 0;
            for (size_t byte = 0; byte < targets[0].size; ++byte)
                target_nonzero += targets[0].data[byte] != 0;
            fprintf(stderr,
                    "%zu blend=%u/write=%x depth=%u/%u/%u cull=%u "
                    "viewport=%08x,%08x,%08x,%08x scissor=%d,%d,%d,%d\n",
                    target_nonzero, graphics_state.blend_enable,
                    graphics_state.write_mask, graphics_state.depth_test_enable,
                    graphics_state.depth_write_enable,
                    graphics_state.depth_compare_op, graphics_state.cull_mode,
                    ((const uint32_t *)draw->viewport)[0],
                    ((const uint32_t *)draw->viewport)[1],
                    ((const uint32_t *)draw->viewport)[2],
                    ((const uint32_t *)draw->viewport)[3], draw->scissor[0],
                    draw->scissor[1], draw->scissor[2], draw->scissor[3]);
            for (uint32_t input = 0; input < binding_count; ++input) {
                const BeerVulkanGraphicsBinding *binding = &bindings[input];
                if (!binding->data || !binding->size) continue;
                size_t nonzero = 0;
                uint64_t hash = UINT64_C(1469598103934665603);
                size_t samples = binding->size < 4096u ?
                    binding->size : 4096u;
                for (size_t sample = 0; sample < samples; ++sample) {
                    size_t offset = sample * binding->size / samples;
                    uint8_t value = binding->data[offset];
                    nonzero += value != 0;
                    hash = (hash ^ value) * UINT64_C(1099511628211);
                }
                uint32_t words[4] = {0};
                size_t prefix = binding->size < sizeof(words) ?
                    binding->size : sizeof(words);
                memcpy(words, binding->data, prefix);
                size_t alpha_nonzero = 0;
                if (binding->kind == BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_IMAGE &&
                    binding->format == 122 && binding->bytes_per_pixel == 4) {
                    size_t texels = (size_t)binding->width * binding->height;
                    if (texels > binding->size / 4u)
                        texels = binding->size / 4u;
                    const uint32_t *packed = (const uint32_t *)binding->data;
                    for (size_t texel = 0; texel < texels; ++texel)
                        alpha_nonzero += (packed[texel] >> 27) != 0;
                }
                fprintf(stderr,
                        "[D3D12 %s PASS INPUT] binding=%u kind=%u "
                        "resource=%p serial=%llu format=%u bytes=%zu "
                        "nonzero=%zu hash=%016llx alpha-nonzero=%zu "
                        "size=%ux%u words=%08x,%08x,%08x,%08x\n",
                        trace_name, binding->binding, binding->kind,
                        binding->resource,
                        (unsigned long long)(binding->serial ?
                            *binding->serial : UINT64_MAX),
                        binding->format, binding->size, nonzero,
                        (unsigned long long)hash, alpha_nonzero,
                        binding->width, binding->height, words[0], words[1],
                        words[2], words[3]);
            }
        }
    }
    int executed = beer_vulkan_graphics_draw_state(
        vertex, (const uint32_t *)vertex->spirv, vertex->spirv_size,
        pixel, pixel ? (const uint32_t *)pixel->spirv : NULL,
        pixel ? pixel->spirv_size : 0,
        bindings, binding_count, draw_info_ptr, &graphics_state, targets,
        draw->render_target_count, depth_target_ptr, draw->viewport,
        draw->scissor, reason);
    if (getenv("BEER_D3D12_DIAGNOSTICS")) {
        fprintf(stderr, "[GRAPHICS DRAW RESULT] executed=%d targets=%u\n", 
            executed, draw->render_target_count);
    }
    if (executed && getenv("BEER_D3D12_TARGET_DIAGNOSTICS")) {
        static unsigned target_traces;
        if (target_traces < 256) {
            for (uint32_t target = 0;
                 target < draw->render_target_count && target_traces < 256;
                 ++target, ++target_traces) {
                const uint8_t *bytes = draw->render_target_storage[target];
                size_t size = draw->render_target_storage_size[target];
                uint64_t hash = UINT64_C(1469598103934665603);
                size_t nonzero = 0;
                for (size_t i = 0; i < size; ++i) {
                    nonzero += bytes[i] != 0;
                    hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
                }
                fprintf(stderr,
                    "[D3D12 TARGET] vs=%016llx ps=%016llx target=%u "
                    "size=%ux%u format=%u bytes=%zu nonzero=%zu hash=%016llx "
                    "serial=%llu\n",
                    (unsigned long long)draw->vertex_shader_hash,
                    (unsigned long long)draw->pixel_shader_hash, target,
                    draw->render_target_width[target],
                    draw->render_target_height[target],
                    draw->render_target_format[target], size, nonzero,
                    (unsigned long long)hash,
                    (unsigned long long)*draw->render_target_serial_location[target]);
                if (!nonzero) {
                    static unsigned input_traces;
                    for (uint32_t input = 0;
                         input < binding_count && input_traces < 256;
                         ++input) {
                        const BeerVulkanGraphicsBinding *binding =
                            &bindings[input];
                        if (!binding->data || !binding->size) continue;
                        size_t input_nonzero = 0;
                        uint64_t input_hash = UINT64_C(1469598103934665603);
                        for (size_t byte = 0; byte < binding->size; ++byte) {
                            input_nonzero += binding->data[byte] != 0;
                            input_hash = (input_hash ^ binding->data[byte]) *
                                UINT64_C(1099511628211);
                        }
                        fprintf(stderr,
                            "[D3D12 TARGET INPUT] ps=%016llx binding=%u "
                            "kind=%u format=%u bytes=%zu nonzero=%zu "
                            "hash=%016llx size=%ux%u layers=%u depth=%u\n",
                            (unsigned long long)draw->pixel_shader_hash,
                            binding->binding, binding->kind, binding->format,
                            binding->size, input_nonzero,
                            (unsigned long long)input_hash, binding->width,
                            binding->height, binding->layer_count,
                            binding->depth);
                        ++input_traces;
                    }
                }
            }
        }
    }
    return executed;
}
