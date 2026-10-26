#define _GNU_SOURCE
#include "bd12_pso.h"

#include "bd12_formats.h"
#include "d3d12_shader_translator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static uint64_t fnv(const uint8_t *p, size_t n)
{
    uint64_t h = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= UINT64_C(1099511628211); }
    return h;
}

static uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static void parse_blend(BD12PsoDesc *pso, const uint8_t *data)
{
    pso->alpha_to_coverage = rd32(data);
    pso->independent_blend = rd32(data + 4);
    for (int i = 0; i < 8; ++i) {
        /* D3D12_RENDER_TARGET_BLEND_DESC: nine 32-bit fields, then a UINT8
         * write mask padded to 40 bytes. */
        const uint8_t *rt = data + 8 + (size_t)i * 40;
        pso->blend[i] = (BD12PsoBlend){
            rd32(rt), rd32(rt + 4), rd32(rt + 8), rd32(rt + 12), rd32(rt + 16),
            rd32(rt + 20), rd32(rt + 24), rd32(rt + 28), rd32(rt + 32), rt[36]
        };
    }
}

static void parse_rasterizer(BD12PsoDesc *pso, const uint8_t *d)
{
    pso->fill_mode = rd32(d);
    pso->cull_mode = rd32(d + 4);
    pso->front_ccw = rd32(d + 8);
    pso->depth_bias = (int32_t)rd32(d + 12);
    uint32_t bits = rd32(d + 16);
    memcpy(&pso->depth_bias_clamp, &bits, 4);
    bits = rd32(d + 20);
    memcpy(&pso->slope_bias, &bits, 4);
    pso->depth_clip = rd32(d + 24);
}

static void parse_depth_stencil(BD12PsoDesc *pso, const uint8_t *d)
{
    pso->depth_enable = rd32(d);
    pso->depth_write_mask = rd32(d + 4);
    pso->depth_func = rd32(d + 8);
    pso->stencil_enable = rd32(d + 12);
    pso->stencil_read_mask = d[16];
    pso->stencil_write_mask = d[17];
    pso->stencil_front = (BD12PsoStencil){ rd32(d + 20), rd32(d + 24),
                                           rd32(d + 28), rd32(d + 32) };
    pso->stencil_back = (BD12PsoStencil){ rd32(d + 36), rd32(d + 40),
                                          rd32(d + 44), rd32(d + 48) };
}

void bd12_pso_defaults(BD12PsoDesc *pso)
{
    memset(pso, 0, sizeof(*pso));
    for (int i = 0; i < 8; ++i) {
        pso->blend[i] = (BD12PsoBlend){ 0, 0, 2, 1, 1, 2, 1, 1, 4, 0xf };
    }
    pso->sample_mask = UINT32_MAX;
    pso->fill_mode = 3;
    pso->cull_mode = 3;
    pso->depth_clip = 1;
    pso->depth_enable = 1;
    pso->depth_write_mask = 1;
    pso->depth_func = 2;
    pso->stencil_read_mask = pso->stencil_write_mask = 0xff;
    pso->samples = 1;
    pso->topology_type = 3;
}

static void shader_at(const uint8_t *base, size_t offset, const uint8_t **code,
                      size_t *size)
{
    const uint8_t *ptr = NULL;
    size_t length = 0;
    memcpy(&ptr, base + offset, sizeof(ptr));
    memcpy(&length, base + offset + 8, sizeof(length));
    if (ptr && length >= 32) {
        *code = ptr;
        *size = length;
    }
}

void bd12_pso_parse_graphics(const uint8_t *desc, BD12PsoDesc *pso)
{
    bd12_pso_defaults(pso);
    memcpy(&pso->root_signature, desc, sizeof(void *));
    shader_at(desc, 8, &pso->vs, &pso->vs_size);
    shader_at(desc, 24, &pso->ps, &pso->ps_size);
    const uint8_t *unused_code = NULL;
    size_t unused_size = 0;
    for (size_t off = 40; off <= 72; off += 16) {   /* DS, HS, GS */
        shader_at(desc, off, &unused_code, &unused_size);
        if (unused_code) pso->has_tessellation_or_geometry = 1;
    }
    parse_blend(pso, desc + 120);
    pso->sample_mask = rd32(desc + 448);
    parse_rasterizer(pso, desc + 452);
    parse_depth_stencil(pso, desc + 496);
    memcpy(&pso->input_elements, desc + 552, sizeof(void *));
    pso->input_count = rd32(desc + 560);
    pso->strip_cut = rd32(desc + 568);
    pso->topology_type = rd32(desc + 572);
    pso->rt_count = rd32(desc + 576);
    for (int i = 0; i < 8; ++i) pso->rt_formats[i] = rd32(desc + 580 + i * 4);
    pso->ds_format = rd32(desc + 612);
    pso->samples = rd32(desc + 616) ? rd32(desc + 616) : 1;
}

void bd12_pso_parse_compute(const uint8_t *desc, BD12PsoDesc *pso)
{
    bd12_pso_defaults(pso);
    memcpy(&pso->root_signature, desc, sizeof(void *));
    shader_at(desc, 8, &pso->cs, &pso->cs_size);
}

void bd12_pso_parse_stream(const uint8_t *stream, size_t size, BD12PsoDesc *pso)
{
    bd12_pso_defaults(pso);
    static const struct { uint8_t align, size; } layout[] = {
        [0] = {8, 8},   [1] = {8, 16},  [2] = {8, 16},  [3] = {8, 16},
        [4] = {8, 16},  [5] = {8, 16},  [6] = {8, 16},  [7] = {8, 32},
        [8] = {4, 0},   [9] = {4, 4},   [10] = {4, 44}, [11] = {4, 52},
        [12] = {8, 16}, [13] = {4, 4},  [14] = {4, 4},  [15] = {4, 36},
        [16] = {4, 4},  [17] = {4, 8},  [18] = {4, 4},  [19] = {8, 16},
        [20] = {4, 4},  [21] = {4, 56}, [22] = {8, 24}
    };
    size_t offset = 0;
    while (offset + 8 <= size) {
        const uint32_t type = rd32(stream + offset);
        if (type >= sizeof(layout) / sizeof(layout[0])) return;
        const size_t align = layout[type].align;
        const size_t length = type == 8 ? 328 : layout[type].size;
        const size_t payload = (offset + 4 + align - 1) & ~(align - 1);
        if (payload + length > size) return;
        const uint8_t *d = stream + payload;
        switch (type) {
        case 0: memcpy(&pso->root_signature, d, sizeof(void *)); break;
        case 1: shader_at(d, 0, &pso->vs, &pso->vs_size); break;
        case 2: shader_at(d, 0, &pso->ps, &pso->ps_size); break;
        case 3: case 4: case 5: {
            const uint8_t *c = NULL;
            size_t n = 0;
            shader_at(d, 0, &c, &n);
            if (c) pso->has_tessellation_or_geometry = 1;
            break;
        }
        case 6: shader_at(d, 0, &pso->cs, &pso->cs_size); break;
        case 8: parse_blend(pso, d); break;
        case 9: pso->sample_mask = rd32(d); break;
        case 10: parse_rasterizer(pso, d); break;
        case 11: case 21: parse_depth_stencil(pso, d); break;
        case 12:
            memcpy(&pso->input_elements, d, sizeof(void *));
            pso->input_count = rd32(d + 8);
            break;
        case 13: pso->strip_cut = rd32(d); break;
        case 14: pso->topology_type = rd32(d); break;
        case 15:
            for (int i = 0; i < 8; ++i) pso->rt_formats[i] = rd32(d + i * 4);
            pso->rt_count = rd32(d + 32);
            break;
        case 16: pso->ds_format = rd32(d); break;
        case 17: pso->samples = rd32(d) ? rd32(d) : 1; break;
        default: break;
        }
        offset = (payload + length + 7) & ~(size_t)7;
    }
}

/* Enum conversion ---------------------------------------------------------- */

static VkBlendFactor blend_factor(uint32_t v)
{
    switch (v) {
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 14: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 16: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 17: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 18: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 19: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 20: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 21: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

static VkBlendOp blend_op(uint32_t v)
{
    switch (v) {
    case 2: return VK_BLEND_OP_SUBTRACT;
    case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: return VK_BLEND_OP_MIN;
    case 5: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

static VkLogicOp logic_op(uint32_t v)
{
    static const VkLogicOp map[16] = {
        VK_LOGIC_OP_CLEAR, VK_LOGIC_OP_SET, VK_LOGIC_OP_COPY,
        VK_LOGIC_OP_COPY_INVERTED, VK_LOGIC_OP_NO_OP, VK_LOGIC_OP_INVERT,
        VK_LOGIC_OP_AND, VK_LOGIC_OP_NAND, VK_LOGIC_OP_OR, VK_LOGIC_OP_NOR,
        VK_LOGIC_OP_XOR, VK_LOGIC_OP_EQUIVALENT, VK_LOGIC_OP_AND_REVERSE,
        VK_LOGIC_OP_AND_INVERTED, VK_LOGIC_OP_OR_REVERSE,
        VK_LOGIC_OP_OR_INVERTED
    };
    return v < 16 ? map[v] : VK_LOGIC_OP_COPY;
}

static VkCompareOp compare_op(uint32_t v)
{
    return v >= 1 && v <= 8 ? (VkCompareOp)(v - 1) : VK_COMPARE_OP_ALWAYS;
}

static VkStencilOp stencil_op(uint32_t v)
{
    return v >= 1 && v <= 8 ? (VkStencilOp)(v - 1) : VK_STENCIL_OP_KEEP;
}

static VkStencilOpState stencil_state(const BD12PsoStencil *s, uint8_t read,
                                      uint8_t write)
{
    return (VkStencilOpState){
        .failOp = stencil_op(s->fail),
        .passOp = stencil_op(s->pass),
        .depthFailOp = stencil_op(s->depth_fail),
        .compareOp = compare_op(s->func),
        .compareMask = read,
        .writeMask = write
    };
}

VkPrimitiveTopology bd12_topology_to_vk(uint32_t t, uint32_t *patch_points)
{
    if (patch_points) *patch_points = 0;
    switch (t) {
    case 1: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case 2: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case 4: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case 5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case 10: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
    case 11: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
    case 12: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
    case 13: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
    default:
        if (t >= 33 && t <= 64) {
            if (patch_points) *patch_points = t - 32;
            return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
        }
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

/* Vertex input signature ---------------------------------------------------- */

typedef struct {
    char name[32];
    uint32_t index, reg, system_value;
} SigElement;

/* Parses the ISGN/ISG1 chunk of a DXBC container. */
static uint32_t read_input_signature(const uint8_t *dxbc, size_t size,
                                     SigElement *out, uint32_t max)
{
    if (size < 32 || memcmp(dxbc, "DXBC", 4) != 0) return 0;
    const uint32_t chunk_count = rd32(dxbc + 28);
    for (uint32_t c = 0; c < chunk_count && 32 + c * 4 + 4 <= size; ++c) {
        const uint32_t chunk_offset = rd32(dxbc + 32 + c * 4);
        if (chunk_offset + 8 > size) continue;
        const uint8_t *chunk = dxbc + chunk_offset;
        const int is_isgn = memcmp(chunk, "ISGN", 4) == 0;
        const int is_isg1 = memcmp(chunk, "ISG1", 4) == 0;
        if (!is_isgn && !is_isg1) continue;
        const uint32_t chunk_size = rd32(chunk + 4);
        if (chunk_offset + 8 + chunk_size > size) return 0;
        const uint8_t *data = chunk + 8;
        const uint32_t count = rd32(data);
        const uint32_t stride = is_isg1 ? 32 : 24;
        uint32_t written = 0;
        for (uint32_t i = 0; i < count && written < max; ++i) {
            if (8 + (size_t)(i + 1) * stride > chunk_size) break;
            const uint8_t *e = data + 8 + (size_t)i * stride;
            /* ISG1: {stream u32} precedes the name offset. */
            const uint8_t *f = is_isg1 ? e + 4 : e;
            const uint32_t name_offset = rd32(f);
            if (name_offset >= chunk_size) continue;
            SigElement *el = &out[written++];
            const char *name = (const char *)data + name_offset;
            size_t n = strnlen(name, chunk_size - name_offset);
            if (n >= sizeof(el->name)) n = sizeof(el->name) - 1;
            memcpy(el->name, name, n);
            el->name[n] = 0;
            el->index = rd32(f + 4);
            el->system_value = rd32(f + 8);
            el->reg = rd32(f + 16);
        }
        return written;
    }
    return 0;
}

static uint32_t to_bindings(const BeerD3D12TranslatedShader *shader,
                            BD12ShaderBinding *out)
{
    for (uint32_t i = 0; i < shader->descriptor_count; ++i) {
        const BeerD3D12ShaderDescriptor *d = &shader->descriptors[i];
        out[i] = (BD12ShaderBinding){
            .type = d->type,
            .space = d->register_space,
            .reg = d->register_index,
            .is_buffer = d->resource_type == BEER_D3D12_SHADER_RESOURCE_BUFFER,
            .count = d->count ? d->count : 1,
            .binding = d->binding,
            .bindless = d->bindless,
            .push_word = d->push_word,
            .heap_class = d->heap_class
        };
    }
    return shader->descriptor_count;
}

BD12GraphicsPipeline *bd12_pso_create_graphics(BD12Device *device,
                                               const BD12PsoDesc *pso)
{
    if (!pso->vs) return NULL;
    const BeerD3D12TranslatedShader *vs =
        beer_d3d12_translate_shader(pso->vs, pso->vs_size,
                                    fnv(pso->vs, pso->vs_size));
    if (!vs) {
        fprintf(stderr, "[BD12] VS translation failed: %s\n",
                beer_d3d12_shader_translation_error());
        return NULL;
    }
    const BeerD3D12TranslatedShader *ps = NULL;
    if (pso->ps) {
        ps = beer_d3d12_translate_shader(pso->ps, pso->ps_size,
                                        fnv(pso->ps, pso->ps_size));
        if (!ps) {
            fprintf(stderr, "[BD12] PS translation failed: %s\n",
                    beer_d3d12_shader_translation_error());
            return NULL;
        }
    }

    BD12ShaderBinding vs_bindings[BEER_D3D12_MAX_SHADER_DESCRIPTORS];
    BD12ShaderBinding ps_bindings[BEER_D3D12_MAX_SHADER_DESCRIPTORS];
    BD12GraphicsPipelineDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.vs = (BD12ShaderCode){ (const uint32_t *)vs->spirv, vs->spirv_size,
                                vs_bindings, to_bindings(vs, vs_bindings) };
    if (ps)
        desc.ps = (BD12ShaderCode){ (const uint32_t *)ps->spirv, ps->spirv_size,
                                    ps_bindings, to_bindings(ps, ps_bindings) };

    /* Vertex attributes: match each input element to the vertex shader's
     * input signature by semantic; the shader location is the register. */
    SigElement signature[32];
    const uint32_t signature_count =
        read_input_signature(pso->vs, pso->vs_size, signature, 32);
    BD12VertexAttribute attributes[32];
    uint32_t attribute_count = 0;
    uint32_t slot_offsets[BD12_MAX_VERTEX_BUFFERS] = { 0 };
    for (uint32_t i = 0; i < pso->input_count; ++i) {
        const uint8_t *e = pso->input_elements + (size_t)i * 32;
        const char *name = NULL;
        memcpy(&name, e, sizeof(name));
        const uint32_t index = rd32(e + 8), format = rd32(e + 12),
                       slot = rd32(e + 16), step_class = rd32(e + 24);
        uint32_t offset = rd32(e + 20);
        if (slot >= BD12_MAX_VERTEX_BUFFERS) continue;
        if (offset == 0xffffffffu) offset = slot_offsets[slot];
        slot_offsets[slot] = offset + bd12_dxgi_bytes(format);
        if (!name) continue;
        for (uint32_t s = 0; s < signature_count; ++s) {
            if (signature[s].system_value ||
                signature[s].index != index ||
                strcasecmp(signature[s].name, name) != 0)
                continue;
            const VkFormat vk_format = bd12_dxgi_to_vk(format);
            if (vk_format == VK_FORMAT_UNDEFINED || attribute_count == 32)
                break;
            attributes[attribute_count++] = (BD12VertexAttribute){
                .location = signature[s].reg,
                .binding = slot,
                .offset = offset,
                .format = vk_format,
                .per_instance = step_class == 1
            };
            break;
        }
    }
    desc.attributes = attributes;
    desc.attribute_count = attribute_count;

    uint32_t patch_points;
    desc.topology = pso->topology_type == 1 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST :
                    pso->topology_type == 2 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST :
                    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    (void)patch_points;
    desc.fill_mode = pso->fill_mode == 2 ? VK_POLYGON_MODE_LINE :
                                           VK_POLYGON_MODE_FILL;
    desc.cull_mode = pso->cull_mode == 1 ? VK_CULL_MODE_NONE :
                     pso->cull_mode == 2 ? VK_CULL_MODE_FRONT_BIT :
                                           VK_CULL_MODE_BACK_BIT;
    desc.front_ccw = pso->front_ccw;
    desc.depth_clip_enable = pso->depth_clip;
    desc.depth_bias_enable = pso->depth_bias != 0 || pso->slope_bias != 0.0f;
    desc.depth_bias = (float)pso->depth_bias;
    desc.depth_bias_clamp = pso->depth_bias_clamp;
    desc.slope_scaled_bias = pso->slope_bias;
    desc.depth_test = pso->depth_enable;
    desc.depth_write = pso->depth_enable && pso->depth_write_mask;
    desc.depth_func = compare_op(pso->depth_func);
    desc.stencil_test = pso->stencil_enable;
    desc.stencil_front = stencil_state(&pso->stencil_front,
                                       pso->stencil_read_mask,
                                       pso->stencil_write_mask);
    desc.stencil_back = stencil_state(&pso->stencil_back,
                                      pso->stencil_read_mask,
                                      pso->stencil_write_mask);
    desc.rt_count = pso->rt_count > 8 ? 8 : pso->rt_count;
    for (uint32_t i = 0; i < desc.rt_count; ++i) {
        desc.rt_formats[i] = bd12_dxgi_to_vk(pso->rt_formats[i]);
        const BD12PsoBlend *b = &pso->blend[pso->independent_blend ? i : 0];
        desc.blend[i] = (BD12BlendTarget){
            .blend_enable = b->enable,
            .src_color = blend_factor(b->src),
            .dst_color = blend_factor(b->dst),
            .src_alpha = blend_factor(b->src_alpha),
            .dst_alpha = blend_factor(b->dst_alpha),
            .color_op = blend_op(b->op),
            .alpha_op = blend_op(b->op_alpha),
            .write_mask = b->write_mask & 0xf,
            .logic_enable = b->logic_enable,
            .logic_op = logic_op(b->logic_op)
        };
    }
    desc.ds_format = bd12_dxgi_resource_to_vk(pso->ds_format, 1);
    if (pso->ds_format == 0) desc.ds_format = VK_FORMAT_UNDEFINED;
    desc.samples = pso->samples;
    desc.alpha_to_coverage = pso->alpha_to_coverage;
    desc.primitive_restart = pso->strip_cut != 0;
    {
        BD12GraphicsPipeline *created = bd12_graphics_pipeline_create(device, &desc);
        if (created && getenv("BEER_BD12_PSO_LOG"))
            fprintf(stderr, "[PSOSHADERS] %016llx vs=%016llx ps=%016llx\n",
                    (unsigned long long)created->shader_hash,
                    (unsigned long long)vs->shader_hash,
                    (unsigned long long)(ps ? ps->shader_hash : 0));
        return created;
    }
}

/* Clone ---------------------------------------------------------------------- */

int bd12_pso_clone(const BD12PsoDesc *src, BD12PsoDesc *dst)
{
    *dst = *src;
    size_t total = src->vs_size + src->ps_size + src->cs_size +
                   (size_t)src->input_count * 32;
    for (uint32_t i = 0; i < src->input_count; ++i) {
        const char *name = NULL;
        memcpy(&name, src->input_elements + (size_t)i * 32, sizeof(name));
        total += (name ? strlen(name) : 0) + 1;
    }
    uint8_t *blob = malloc(total ? total : 1);
    if (!blob) return 0;
    uint8_t *cursor = blob;
    if (src->vs_size) {
        memcpy(cursor, src->vs, src->vs_size);
        dst->vs = cursor; cursor += src->vs_size;
    }
    if (src->ps_size) {
        memcpy(cursor, src->ps, src->ps_size);
        dst->ps = cursor; cursor += src->ps_size;
    }
    if (src->cs_size) {
        memcpy(cursor, src->cs, src->cs_size);
        dst->cs = cursor; cursor += src->cs_size;
    }
    if (src->input_count) {
        uint8_t *elements = cursor;
        cursor += (size_t)src->input_count * 32;
        memcpy(elements, src->input_elements, (size_t)src->input_count * 32);
        for (uint32_t i = 0; i < src->input_count; ++i) {
            const char *name = NULL;
            memcpy(&name, elements + (size_t)i * 32, sizeof(name));
            size_t length = name ? strlen(name) : 0;
            if (name) memcpy(cursor, name, length);
            cursor[length] = 0;
            const char *copied = (const char *)cursor;
            memcpy(elements + (size_t)i * 32, &copied, sizeof(copied));
            cursor += length + 1;
        }
        dst->input_elements = elements;
    }
    /* The blob is addressed through whichever pointer comes first; keep the
     * base in a side field so release can free it. */
    dst->owned_blob = blob;
    return 1;
}

void bd12_pso_release(BD12PsoDesc *pso)
{
    free(pso->owned_blob);
    pso->owned_blob = NULL;
}
