#define _GNU_SOURCE
#include "bd12_pso.h"

#include "bd12_formats.h"
#include "d3d12_shader_translator.h"
#include "bd12_com.h"

#include <pthread.h>
#include <stddef.h>
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
    shader_at(desc, 40, &pso->ds, &pso->ds_size);
    shader_at(desc, 56, &pso->hs, &pso->hs_size);
    shader_at(desc, 72, &pso->gs, &pso->gs_size);
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
    memcpy(&pso->cached_pso, desc + 632, sizeof(void *));      /* after SampleDesc (616) and NodeMask (624) */
    memcpy(&pso->cached_pso_size, desc + 640, sizeof(size_t));
}

void bd12_pso_parse_compute(const uint8_t *desc, BD12PsoDesc *pso)
{
    bd12_pso_defaults(pso);
    memcpy(&pso->root_signature, desc, sizeof(void *));
    shader_at(desc, 8, &pso->cs, &pso->cs_size);
    memcpy(&pso->cached_pso, desc + 32, sizeof(void *));       /* after NodeMask (24) */
    memcpy(&pso->cached_pso_size, desc + 40, sizeof(size_t));
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
        [20] = {4, 4},  [21] = {4, 56}, [22] = {8, 24},
        /* 23 is not a subobject type. 24 AS and 25 MS are shader bytecode; 26 DEPTH_STENCIL2, 27 RASTERIZER1 and
         * 28 RASTERIZER2 are stepped over by size (their contents are not read yet). */
        [24] = {8, 16}, [25] = {8, 16}, [26] = {4, 60}, [27] = {4, 44}, [28] = {4, 40}
    };
    size_t offset = 0;
    while (offset + 8 <= size) {
        const uint32_t type = rd32(stream + offset);
        if (type >= sizeof(layout) / sizeof(layout[0])) return;
        if (!layout[type].align) return;               /* not a subobject type */
        const size_t align = layout[type].align;
        const size_t length = type == 8 ? 328 : layout[type].size;
        const size_t payload = (offset + 4 + align - 1) & ~(align - 1);
        if (payload + length > size) return;
        const uint8_t *d = stream + payload;
        switch (type) {
        case 0: memcpy(&pso->root_signature, d, sizeof(void *)); break;
        case 1: shader_at(d, 0, &pso->vs, &pso->vs_size); break;
        case 2: shader_at(d, 0, &pso->ps, &pso->ps_size); break;
        case 3: shader_at(d, 0, &pso->ds, &pso->ds_size); break;
        case 4: shader_at(d, 0, &pso->hs, &pso->hs_size); break;
        case 5: shader_at(d, 0, &pso->gs, &pso->gs_size); break;
        case 6: shader_at(d, 0, &pso->cs, &pso->cs_size); break;
        case 24: shader_at(d, 0, &pso->as, &pso->as_size); break;   /* D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS */
        case 25: shader_at(d, 0, &pso->ms, &pso->ms_size); break;   /* D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS */
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
        case 19:
            memcpy(&pso->cached_pso, d, sizeof(void *));
            memcpy(&pso->cached_pso_size, d + 8, sizeof(size_t));
            break;
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
            .heap_class = d->heap_class,
            .is_ssbo = d->ssbo
        };
    }
    return shader->descriptor_count;
}

/* Input patch size a hull shader declares, 0 when absent: dcl_input_control_point_count in DXBC (SHEX/SHDR),
 * PSVRuntimeInfo.HS.InputControlPointCount in DXIL (PSV0, the first word after the info's size). */
static uint32_t hull_input_control_points(const uint8_t *dxbc, size_t size)
{
    if (!dxbc || size < 32 || memcmp(dxbc, "DXBC", 4) != 0) return 0;
    const uint32_t chunks = rd32(dxbc + 28);
    for (uint32_t c = 0; c < chunks && 32 + (size_t)c * 4 + 4 <= size; ++c) {
        const uint32_t offset = rd32(dxbc + 32 + c * 4);
        if ((size_t)offset + 8 > size) continue;
        if (!memcmp(dxbc + offset, "PSV0", 4)) {
            const size_t chunk = rd32(dxbc + offset + 4);
            if (chunk < 8 || (size_t)offset + 8 + chunk > size || rd32(dxbc + offset + 8) < 4) continue;
            const uint32_t count = rd32(dxbc + offset + 12);
            if (count >= 1 && count <= 32) return count;
            continue;
        }
        if (memcmp(dxbc + offset, "SHEX", 4) && memcmp(dxbc + offset, "SHDR", 4)) continue;
        const size_t end = offset + 8 + (size_t)rd32(dxbc + offset + 4);
        size_t at = offset + 16;                /* skip version and length tokens */
        while (at + 4 <= end && end <= size) {
            const uint32_t token = rd32(dxbc + at);
            const uint32_t opcode = token & 0x7ff;
            if (opcode == 147) return (token >> 11) & 0x3f;
            uint32_t length = (token >> 24) & 0x7f;
            if (opcode == 0x35) {               /* customdata: length in the next token */
                if (at + 8 > end) break;
                length = rd32(dxbc + at + 4);
            }
            if (!length) break;
            at += (size_t)length * 4;
        }
    }
    return 0;
}

static const BeerD3D12TranslatedShader *translate_stage(const char *name,
                                                         const uint8_t *code,
                                                         size_t size)
{
    if (!code) return NULL;
    const BeerD3D12TranslatedShader *shader =
        beer_d3d12_translate_shader(code, size, fnv(code, size));
    if (!shader)
        fprintf(stderr, "[BD12] %s translation failed: %s\n", name,
                beer_d3d12_shader_translation_error());
    return shader;
}

static BD12GraphicsPipeline *create_graphics(BD12Device *device, const BD12PsoDesc *pso,
                                             int no_extra_stages, const BD12RootSignature *rs);
static void fixed_state(BD12GraphicsPipelineDesc *desc, const BD12PsoDesc *pso);
static BD12GraphicsPipeline *create_mesh(BD12Device *device, const BD12PsoDesc *pso, const BD12RootSignature *rs);

/* The root signature a pipeline description names, when root-signature translation applies to it. */
static BD12RootSignature *root_push_signature(BD12Device *device, const BD12PsoDesc *pso)
{
    const ComRootSig *crs = pso->root_signature;
    if (!crs || !crs->rs || !bd12_root_push_enabled(device)) return NULL;
    return bd12_root_layout(crs->rs) ? crs->rs : NULL;
}

BD12GraphicsPipeline *bd12_pso_create_graphics(BD12Device *device,
                                               const BD12PsoDesc *pso)
{
    BD12RootSignature *rs = root_push_signature(device, pso);
    if (rs) {
        BD12GraphicsPipeline *pipeline = create_graphics(device, pso, 0, rs);
        bd12_root_push_count(1, pipeline != NULL);
        if (pipeline) return pipeline;   /* otherwise: per-draw descriptors */
    }
    BD12GraphicsPipeline *pipeline = create_graphics(device, pso, 0, NULL);
    if (!pipeline && (pso->hs || pso->gs)) {
        fprintf(stderr, "[BD12] pipeline with hull/domain/geometry stages failed: retrying with VS/PS only\n");
        pipeline = create_graphics(device, pso, 1, NULL);
    }
    return pipeline;
}

/* With `rs`, every stage must translate for its layout (no stage is dropped); NULL then means "use the per-draw
 * path", not a failure. */
static BD12GraphicsPipeline *create_graphics(BD12Device *device, const BD12PsoDesc *pso,
                                             int no_extra_stages, const BD12RootSignature *rs)
{
    if (pso->ms) return create_mesh(device, pso, rs);
    if (!pso->vs) return NULL;
    const BeerD3D12RootLayout *layout = rs ? rs->root_layout : NULL;
    if (layout) {
        if (!beer_d3d12_translate_shader_rs(pso->vs, pso->vs_size, fnv(pso->vs, pso->vs_size), layout) ||
            (pso->ps && !beer_d3d12_translate_shader_rs(pso->ps, pso->ps_size, fnv(pso->ps, pso->ps_size), layout)) ||
            (pso->hs && !beer_d3d12_translate_shader_rs(pso->hs, pso->hs_size, fnv(pso->hs, pso->hs_size), layout)) ||
            (pso->ds && !beer_d3d12_translate_shader_rs(pso->ds, pso->ds_size, fnv(pso->ds, pso->ds_size), layout)) ||
            (pso->gs && !beer_d3d12_translate_shader_rs(pso->gs, pso->gs_size, fnv(pso->gs, pso->gs_size), layout)) ||
            (!pso->hs != !pso->ds))
            return NULL;
    }
#define TRANSLATE(code, size) (layout ? beer_d3d12_translate_shader_rs((code), (size), fnv((code), (size)), layout) \
                                      : beer_d3d12_translate_shader((code), (size), fnv((code), (size))))
    const BeerD3D12TranslatedShader *vs = TRANSLATE(pso->vs, pso->vs_size);
    if (!vs) {
        fprintf(stderr, "[BD12] VS translation failed: %s\n",
                beer_d3d12_shader_translation_error());
        return NULL;
    }
    const BeerD3D12TranslatedShader *ps = NULL;
    if (pso->ps) {
        ps = TRANSLATE(pso->ps, pso->ps_size);
        if (!ps) {
            fprintf(stderr, "[BD12] PS translation failed: %s\n",
                    beer_d3d12_shader_translation_error());
            return NULL;
        }
        if (getenv("BEER_BD12_FORCE_PS_MAGENTA")) {      /* diagnostic: every pixel shader writes opaque magenta */
            static const uint32_t magenta[] = {
                0x7230203,0x10000,0x0,0xc,0x0,0x20011,0x1,0x3000e,0x0,0x0,0x6000f,0x4,0x7,0x6e69616d,0x0,0x6,0x30010,0x7,0x7,
                0x40047,0x6,0x1e,0x0,0x20013,0x1,0x30021,0x2,0x1,0x30016,0x3,0x20,0x40017,0x4,0x3,0x4,0x40020,0x5,0x3,0x4,
                0x4003b,0x5,0x6,0x3,0x4002b,0x3,0x9,0x3f800000,0x4002b,0x3,0xa,0x0,0x7002c,0x4,0xb,0x9,0xa,0x9,0x9,
                0x50036,0x1,0x7,0x0,0x2,0x200f8,0x8,0x3003e,0x6,0xb,0x100fd,0x10038 };
            static BeerD3D12TranslatedShader forced;
            memset(&forced, 0, sizeof forced);
            forced.spirv = (void *)magenta;
            forced.spirv_size = sizeof magenta;
            forced.shader_hash = 0x6d6167656e7461ull;
            ps = &forced;
        }
    }

    /* Hull and domain shaders only work as a pair. A stage that does not translate is left out, so the draw
     * still renders untessellated / without its geometry shader as it did before BD12 had these stages. */
    const BeerD3D12TranslatedShader *hs = NULL, *ds = NULL, *gs = NULL;
    if (layout) {   /* all stages translated above */
        if (pso->hs) { hs = TRANSLATE(pso->hs, pso->hs_size); ds = TRANSLATE(pso->ds, pso->ds_size); }
        if (pso->gs) gs = TRANSLATE(pso->gs, pso->gs_size);
    } else {
        if (pso->hs && pso->ds && !no_extra_stages &&
            (!(hs = translate_stage("HS", pso->hs, pso->hs_size)) ||
             !(ds = translate_stage("DS", pso->ds, pso->ds_size))))
            hs = ds = NULL;
        if (pso->gs && !no_extra_stages) gs = translate_stage("GS", pso->gs, pso->gs_size);
    }
#undef TRANSLATE
    const int tessellated = hs != NULL;

    BD12ShaderBinding hs_bindings[BEER_D3D12_MAX_SHADER_DESCRIPTORS];
    BD12ShaderBinding ds_bindings[BEER_D3D12_MAX_SHADER_DESCRIPTORS];
    BD12ShaderBinding gs_bindings[BEER_D3D12_MAX_SHADER_DESCRIPTORS];
    BD12ShaderBinding vs_bindings[BEER_D3D12_MAX_SHADER_DESCRIPTORS];
    BD12ShaderBinding ps_bindings[BEER_D3D12_MAX_SHADER_DESCRIPTORS];
    BD12GraphicsPipelineDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.root_push_bytes = layout ? rs->root_push_bytes : 0;
    desc.root_layout_hash = layout ? layout->hash : 0;
    if (layout && layout->heap_model == BEER_HEAP_MODEL_MUTABLE &&
        !(desc.db_layout = bd12_db_layout(rs)))
        return NULL;   /* no descriptor-buffer layout: per-draw descriptors */
    desc.vs = (BD12ShaderCode){ (const uint32_t *)vs->spirv, vs->spirv_size,
                                vs_bindings, to_bindings(vs, vs_bindings) };
    if (ps)
        desc.ps = (BD12ShaderCode){ (const uint32_t *)ps->spirv, ps->spirv_size,
                                    ps_bindings, to_bindings(ps, ps_bindings) };
    if (hs) {
        desc.hs = (BD12ShaderCode){ (const uint32_t *)hs->spirv, hs->spirv_size,
                                    hs_bindings, to_bindings(hs, hs_bindings) };
        desc.ds = (BD12ShaderCode){ (const uint32_t *)ds->spirv, ds->spirv_size,
                                    ds_bindings, to_bindings(ds, ds_bindings) };
        desc.patch_control_points = hull_input_control_points(pso->hs, pso->hs_size);
    }
    if (gs)
        desc.gs = (BD12ShaderCode){ (const uint32_t *)gs->spirv, gs->spirv_size,
                                    gs_bindings, to_bindings(gs, gs_bindings) };

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

    desc.topology = tessellated ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST :
                    pso->topology_type == 1 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST :
                    pso->topology_type == 2 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST :
                    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    fixed_state(&desc, pso);
    {
        BD12GraphicsPipeline *created = bd12_graphics_pipeline_create(device, &desc);
        if (created && getenv("BEER_BD12_PSO_LOG"))
            fprintf(stderr, "[PSOSHADERS] %016llx vs=%016llx ps=%016llx hs=%016llx ds=%016llx gs=%016llx\n",
                    (unsigned long long)created->shader_hash,
                    (unsigned long long)vs->shader_hash,
                    (unsigned long long)(ps ? ps->shader_hash : 0),
                    (unsigned long long)(hs ? hs->shader_hash : 0),
                    (unsigned long long)(ds ? ds->shader_hash : 0),
                    (unsigned long long)(gs ? gs->shader_hash : 0));
        return created;
    }
}

/* Rasterizer, depth/stencil, blend and render-target state shared by vertex and mesh pipelines. */
static void fixed_state(BD12GraphicsPipelineDesc *desc, const BD12PsoDesc *pso)
{
    desc->fill_mode = pso->fill_mode == 2 ? VK_POLYGON_MODE_LINE :
                                           VK_POLYGON_MODE_FILL;
    desc->cull_mode = pso->cull_mode == 1 ? VK_CULL_MODE_NONE :
                     pso->cull_mode == 2 ? VK_CULL_MODE_FRONT_BIT :
                                           VK_CULL_MODE_BACK_BIT;
    desc->front_ccw = pso->front_ccw;
    desc->depth_clip_enable = pso->depth_clip;
    desc->depth_bias_enable = pso->depth_bias != 0 || pso->slope_bias != 0.0f;
    desc->depth_bias = (float)pso->depth_bias;
    desc->depth_bias_clamp = pso->depth_bias_clamp;
    desc->slope_scaled_bias = pso->slope_bias;
    desc->depth_test = pso->depth_enable;
    desc->depth_write = pso->depth_enable && pso->depth_write_mask;
    desc->depth_func = compare_op(pso->depth_func);
    desc->stencil_test = pso->stencil_enable;
    desc->stencil_front = stencil_state(&pso->stencil_front,
                                       pso->stencil_read_mask,
                                       pso->stencil_write_mask);
    desc->stencil_back = stencil_state(&pso->stencil_back,
                                      pso->stencil_read_mask,
                                      pso->stencil_write_mask);
    desc->rt_count = pso->rt_count > 8 ? 8 : pso->rt_count;
    for (uint32_t i = 0; i < desc->rt_count; ++i) {
        desc->rt_formats[i] = bd12_dxgi_to_vk(pso->rt_formats[i]);
        const BD12PsoBlend *b = &pso->blend[pso->independent_blend ? i : 0];
        desc->blend[i] = (BD12BlendTarget){
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
    desc->ds_format = bd12_dxgi_resource_to_vk(pso->ds_format, 1);
    if (pso->ds_format == 0) desc->ds_format = VK_FORMAT_UNDEFINED;
    desc->samples = pso->samples;
    desc->alpha_to_coverage = pso->alpha_to_coverage;
    desc->primitive_restart = pso->strip_cut != 0;
}

/* Mesh pipeline: an optional amplification shader, a mesh shader and the pixel shader; no input layout or topology.
 * With a root-signature layout every stage must translate for it, as for vertex pipelines. */
static BD12GraphicsPipeline *create_mesh(BD12Device *device, const BD12PsoDesc *pso, const BD12RootSignature *rs)
{
    if (!device->CmdDrawMeshTasksEXT) return NULL;
    const BeerD3D12RootLayout *layout = rs ? rs->root_layout : NULL;
#define TRANSLATE(code, size) (layout ? beer_d3d12_translate_shader_rs((code), (size), fnv((code), (size)), layout) \
                                      : beer_d3d12_translate_shader((code), (size), fnv((code), (size))))
    const BeerD3D12TranslatedShader *ms = TRANSLATE(pso->ms, pso->ms_size);
    const BeerD3D12TranslatedShader *as = pso->as ? TRANSLATE(pso->as, pso->as_size) : NULL;
    /* The pixel shader is linked to the mesh shader's outputs: locations by semantic, per-primitive attributes as
     * PerPrimitiveEXT (D3D12 links mesh and pixel stages by semantic; Vulkan by location and qualifier). */
    const BeerD3D12TranslatedShader *ps = !pso->ps ? NULL
        : layout ? beer_d3d12_translate_shader_rs_linked(pso->ps, pso->ps_size, fnv(pso->ps, pso->ps_size), layout,
                                                         pso->ms, pso->ms_size)
                 : TRANSLATE(pso->ps, pso->ps_size);
#undef TRANSLATE
    if (!ms || (pso->as && !as) || (pso->ps && !ps)) {
        if (!layout)
            fprintf(stderr, "[BD12] mesh pipeline: %s translation failed: %s\n",
                    !ms ? "MS" : (pso->as && !as) ? "AS" : "PS", beer_d3d12_shader_translation_error());
        return NULL;
    }
    BD12ShaderBinding ms_bindings[BEER_D3D12_MAX_SHADER_DESCRIPTORS];
    BD12ShaderBinding as_bindings[BEER_D3D12_MAX_SHADER_DESCRIPTORS];
    BD12ShaderBinding ps_bindings[BEER_D3D12_MAX_SHADER_DESCRIPTORS];
    BD12GraphicsPipelineDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.root_push_bytes = layout ? rs->root_push_bytes : 0;
    desc.root_layout_hash = layout ? layout->hash : 0;
    if (layout && layout->heap_model == BEER_HEAP_MODEL_MUTABLE && !(desc.db_layout = bd12_db_layout(rs)))
        return NULL;
    desc.ms = (BD12ShaderCode){ (const uint32_t *)ms->spirv, ms->spirv_size, ms_bindings, to_bindings(ms, ms_bindings) };
    if (as)
        desc.as = (BD12ShaderCode){ (const uint32_t *)as->spirv, as->spirv_size, as_bindings,
                                    to_bindings(as, as_bindings) };
    if (ps)
        desc.ps = (BD12ShaderCode){ (const uint32_t *)ps->spirv, ps->spirv_size, ps_bindings,
                                    to_bindings(ps, ps_bindings) };
    desc.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;   /* unused by mesh pipelines */
    fixed_state(&desc, pso);
    BD12GraphicsPipeline *created = bd12_graphics_pipeline_create(device, &desc);
    if (!created && !layout) fprintf(stderr, "[BD12] mesh pipeline creation failed\n");
    if (created) {
        static uint32_t count;
        const uint32_t n = __atomic_add_fetch(&count, 1, __ATOMIC_RELAXED);
        if (n == 1 || n % 100 == 0) fprintf(stderr, "[BD12] mesh pipelines created: %u (amplification %d)\n", n, as != NULL);
    }
    return created;
}

/* Clone ---------------------------------------------------------------------- */

/* Shader bytecode shared between pipeline descriptions. Engines create tens of thousands of pipelines from a few
 * thousand shaders (Onimusha's first-run shader compilation stores ~40000); a private copy per pipeline held
 * several GiB until each pipeline compiled. One reference-counted copy per distinct shader. */
typedef struct ShaderBytes {
    struct ShaderBytes *next;
    uint64_t hash;
    size_t size;
    uint32_t refs;
    uint8_t data[];
} ShaderBytes;
#define SHADER_BUCKETS 8192u
static ShaderBytes *g_shader_buckets[SHADER_BUCKETS];
static pthread_mutex_t g_shader_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t shader_hash(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull ^ n;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static const uint8_t *shader_intern(const uint8_t *bytes, size_t size)
{
    const uint64_t hash = shader_hash(bytes, size);
    ShaderBytes **bucket = &g_shader_buckets[hash % SHADER_BUCKETS];
    pthread_mutex_lock(&g_shader_lock);
    for (ShaderBytes *e = *bucket; e; e = e->next)
        if (e->hash == hash && e->size == size && !memcmp(e->data, bytes, size)) {
            ++e->refs;
            pthread_mutex_unlock(&g_shader_lock);
            return e->data;
        }
    ShaderBytes *e = malloc(sizeof(*e) + size);
    if (e) {
        e->hash = hash;
        e->size = size;
        e->refs = 1;
        memcpy(e->data, bytes, size);
        e->next = *bucket;
        *bucket = e;
    }
    pthread_mutex_unlock(&g_shader_lock);
    return e ? e->data : NULL;
}

static void shader_release(const uint8_t *data)
{
    if (!data) return;
    ShaderBytes *e = (ShaderBytes *)(void *)(data - offsetof(ShaderBytes, data));
    pthread_mutex_lock(&g_shader_lock);
    if (--e->refs == 0) {
        for (ShaderBytes **link = &g_shader_buckets[e->hash % SHADER_BUCKETS]; *link; link = &(*link)->next)
            if (*link == e) { *link = e->next; break; }
        free(e);
    }
    pthread_mutex_unlock(&g_shader_lock);
}

int bd12_pso_clone(const BD12PsoDesc *src, BD12PsoDesc *dst)
{
    *dst = *src;
    size_t total = (size_t)src->input_count * 32;
    for (uint32_t i = 0; i < src->input_count; ++i) {
        const char *name = NULL;
        memcpy(&name, src->input_elements + (size_t)i * 32, sizeof(name));
        total += (name ? strlen(name) : 0) + 1;
    }
    uint8_t *blob = malloc(total ? total : 1);
    if (!blob) return 0;
    const uint8_t **stages[8] = { &dst->vs, &dst->ps, &dst->cs, &dst->hs, &dst->ds, &dst->gs, &dst->as, &dst->ms };
    const size_t sizes[8] = { src->vs_size, src->ps_size, src->cs_size, src->hs_size, src->ds_size, src->gs_size,
                              src->as_size, src->ms_size };
    for (int i = 0; i < 8; ++i) {
        if (!sizes[i]) { *stages[i] = NULL; continue; }
        *stages[i] = shader_intern(*stages[i], sizes[i]);
        if (!*stages[i]) {
            for (int j = 0; j < i; ++j) shader_release(*stages[j]);
            free(blob);
            return 0;
        }
    }
    uint8_t *cursor = blob;
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
    /* Input layouts live in owned_blob; shader stages are shared references (see shader_intern). */
    dst->owned_blob = blob;
    return 1;
}

void bd12_pso_release(BD12PsoDesc *pso)
{
    if (pso->owned_blob) {
        const uint8_t **stages[8] = { &pso->vs, &pso->ps, &pso->cs, &pso->hs, &pso->ds, &pso->gs, &pso->as, &pso->ms };
        for (int i = 0; i < 8; ++i) { shader_release(*stages[i]); *stages[i] = NULL; }
        pso->vs_size = pso->ps_size = pso->cs_size = pso->hs_size = pso->ds_size = pso->gs_size = 0;
        pso->as_size = pso->ms_size = 0;
    }
    free(pso->owned_blob);
    pso->owned_blob = NULL;
}
