#define _GNU_SOURCE
/* BD11 part 3: ID3D11DeviceContext. One implementation serves immediate and deferred contexts:
 * every method builds a B11Op; ctx_emit() either executes it (immediate) or records it
 * (deferred). ExecuteCommandList replays the recorded ops through the immediate path. */

#include "bd11.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { B11Obj *obj; uint32_t stride, offset; } VbEnt;
typedef struct {
    B11Obj *rtv[B11_MAX_RT];
    B11Obj *dsv;
    uint32_t rtv_count;
    uint32_t uav_start, uav_count;
    B11Obj *uav[B11_MAX_UAV];
    uint32_t uav_initial[B11_MAX_UAV];
} RtOp;

/* ---- shadow state ------------------------------------------------------------------------- */

static void set_obj(B11Obj **slot, B11Obj *obj)
{
    if (*slot == obj) return;
    b11_retain(obj);
    B11Obj *old = *slot;
    *slot = obj;
    if (old) b11_release(old);
}

void b11_state_clear(B11Context *ctx)
{
    B11PipelineState *s = &ctx->st;
    for (int st = 0; st < B11_STAGE_COUNT; ++st) {
        set_obj(&s->shader[st], NULL);
        for (int i = 0; i < B11_MAX_CBV; ++i) set_obj(&s->cb[st][i], NULL);
        for (int i = 0; i < B11_MAX_SRV; ++i) set_obj(&s->srv[st][i], NULL);
        for (int i = 0; i < B11_MAX_SAMPLER; ++i) set_obj(&s->sampler[st][i], NULL);
        for (int i = 0; i < B11_MAX_UAV; ++i) set_obj(&s->uav[st][i], NULL);
    }
    for (int i = 0; i < B11_MAX_VB; ++i) set_obj(&s->vb[i], NULL);
    set_obj(&s->ib, NULL);
    set_obj(&s->layout, NULL);
    for (int i = 0; i < B11_MAX_RT; ++i) set_obj(&s->rtv[i], NULL);
    set_obj(&s->dsv, NULL);
    set_obj(&s->blend, NULL);
    set_obj(&s->depth, NULL);
    set_obj(&s->raster, NULL);
    set_obj(&s->predicate, NULL);
    memset(s, 0, sizeof(*s));
    for (int i = 0; i < 4; ++i) s->blend_factor[i] = 1.0f;
    s->sample_mask = 0xffffffffu;
    ctx->dirty = B11_DIRTY_ALL;
}

static B11Obj *view_resource(B11Obj *view) { return view ? ((B11View *)view)->resource : NULL; }

/* D3D11 unbinds shader-resource/UAV bindings of a resource that becomes a render target. */
static void unbind_hazards(B11Context *ctx, B11Obj *resource)
{
    if (!resource) return;
    B11PipelineState *s = &ctx->st;
    for (int st = 0; st < B11_STAGE_COUNT; ++st) {
        for (int i = 0; i < B11_MAX_SRV; ++i)
            if (s->srv[st][i] && view_resource(s->srv[st][i]) == resource) set_obj(&s->srv[st][i], NULL);
        for (int i = 0; i < B11_MAX_UAV; ++i)
            if (s->uav[st][i] && view_resource(s->uav[st][i]) == resource) set_obj(&s->uav[st][i], NULL);
    }
}

static void apply_state(B11Context *ctx, const B11Op *op)
{
    B11PipelineState *s = &ctx->st;
    switch (op->type) {
    case B11_OP_SET_IA_LAYOUT: set_obj(&s->layout, op->obj[0]); ctx->dirty |= B11_DIRTY_PIPELINE; break;
    case B11_OP_SET_IA_VB: {
        const VbEnt *e = op->data;
        for (uint32_t i = 0; i < op->b && op->a + i < B11_MAX_VB; ++i) {
            const uint32_t slot = op->a + i;
            set_obj(&s->vb[slot], e[i].obj);
            s->vb_stride[slot] = e[i].stride;
            s->vb_offset[slot] = e[i].offset;
        }
        ctx->dirty |= B11_DIRTY_VB;
        break;
    }
    case B11_OP_SET_IA_IB:
        set_obj(&s->ib, op->obj[0]); s->ib_format = op->a; s->ib_offset = op->b; ctx->dirty |= B11_DIRTY_IB; break;
    case B11_OP_SET_TOPOLOGY: s->topology = op->a; ctx->dirty |= B11_DIRTY_TOPOLOGY | B11_DIRTY_PIPELINE; break;
    case B11_OP_SET_SHADER: if (op->a < B11_STAGE_COUNT) set_obj(&s->shader[op->a], op->obj[0]); ctx->dirty |= B11_DIRTY_PIPELINE; break;
    case B11_OP_SET_CB: {
        B11Obj *const *e = op->data;
        for (uint32_t i = 0; i < op->c && op->b + i < B11_MAX_CBV; ++i) set_obj(&s->cb[op->a][op->b + i], e[i]);
        break;
    }
    case B11_OP_SET_SRV: {
        B11Obj *const *e = op->data;
        for (uint32_t i = 0; i < op->c && op->b + i < B11_MAX_SRV; ++i) {
            B11Obj *res = view_resource(e[i]);
            /* A resource bound for writing cannot also be read. */
            if (res) {
                for (uint32_t r = 0; r < s->rtv_count; ++r)
                    if (view_resource(s->rtv[r]) == res) { res = NULL; break; }
                if (res && s->dsv && view_resource(s->dsv) == res &&
                    !(((B11View *)s->dsv)->flags & 1)) res = NULL;
            }
            set_obj(&s->srv[op->a][op->b + i], res || !e[i] ? e[i] : NULL);
        }
        break;
    }
    case B11_OP_SET_SAMPLER: {
        B11Obj *const *e = op->data;
        for (uint32_t i = 0; i < op->c && op->b + i < B11_MAX_SAMPLER; ++i) set_obj(&s->sampler[op->a][op->b + i], e[i]);
        break;
    }
    case B11_OP_SET_UAV_CS: {
        const struct { B11Obj *obj; uint32_t initial; } *e = op->data;
        for (uint32_t i = 0; i < op->b && op->a + i < B11_MAX_UAV; ++i) {
            set_obj(&s->uav[B11_STAGE_CS][op->a + i], e[i].obj);
            s->uav_initial[B11_STAGE_CS][op->a + i] = e[i].initial;
        }
        break;
    }
    case B11_OP_SET_RT: {
        const RtOp *r = op->data;
        for (uint32_t i = 0; i < B11_MAX_RT; ++i) set_obj(&s->rtv[i], i < r->rtv_count ? r->rtv[i] : NULL);
        set_obj(&s->dsv, r->dsv);
        s->rtv_count = r->rtv_count;
        for (uint32_t i = 0; i < r->rtv_count; ++i) unbind_hazards(ctx, view_resource(r->rtv[i]));
        if (r->dsv && !(((B11View *)r->dsv)->flags & 1)) unbind_hazards(ctx, view_resource(r->dsv));
        if (r->uav_start != 0xffffffffu)
            for (uint32_t i = 0; i < r->uav_count && r->uav_start + i < B11_MAX_UAV; ++i) {
                set_obj(&s->uav[B11_STAGE_PS][r->uav_start + i], r->uav[i]);
                s->uav_initial[B11_STAGE_PS][r->uav_start + i] = r->uav_initial[i];
                unbind_hazards(ctx, view_resource(r->uav[i]));
            }
        ctx->dirty |= B11_DIRTY_TARGETS | B11_DIRTY_PIPELINE | B11_DIRTY_VIEWPORT | B11_DIRTY_SCISSOR;
        break;
    }
    case B11_OP_SET_BLEND:
        set_obj(&s->blend, op->obj[0]);
        s->blend_factor[0] = *(const float *)&op->b; s->blend_factor[1] = *(const float *)&op->c;
        s->blend_factor[2] = *(const float *)&op->d; s->blend_factor[3] = *(const float *)&op->e;
        s->sample_mask = op->a;
        ctx->dirty |= B11_DIRTY_PIPELINE | B11_DIRTY_BLEND_CONST;
        break;
    case B11_OP_SET_DEPTH: set_obj(&s->depth, op->obj[0]); s->stencil_ref = op->a; ctx->dirty |= B11_DIRTY_PIPELINE | B11_DIRTY_STENCIL_REF; break;
    case B11_OP_SET_RASTER: set_obj(&s->raster, op->obj[0]); ctx->dirty |= B11_DIRTY_PIPELINE | B11_DIRTY_SCISSOR; break;
    case B11_OP_SET_VIEWPORTS:
        s->viewport_count = op->a > B11_MAX_VIEWPORTS ? B11_MAX_VIEWPORTS : op->a;
        if (op->data) memcpy(s->viewports, op->data, (size_t)s->viewport_count * 24);
        ctx->dirty |= B11_DIRTY_VIEWPORT | B11_DIRTY_SCISSOR;
        break;
    case B11_OP_SET_SCISSORS:
        s->scissor_count = op->a > B11_MAX_VIEWPORTS ? B11_MAX_VIEWPORTS : op->a;
        if (op->data) memcpy(s->scissors, op->data, (size_t)s->scissor_count * 16);
        ctx->dirty |= B11_DIRTY_SCISSOR;
        break;
    case B11_OP_SET_PREDICATION: set_obj(&s->predicate, op->obj[0]); s->predicate_value = op->a; break;
    case B11_OP_CLEAR_STATE: b11_state_clear(ctx); break;
    default: break;
    }
}

/* ---- texture state ------------------------------------------------------------------------- */

void b11_texture_use(B11Context *ctx, B11Texture *t, uint32_t state)
{
    if (!t->image || t->state == state) return;
    BD12CmdList *cmd = b11_cmd(ctx->obj.dev);
    bd12_cmd_transition_image(cmd, t->image, BD12_ALL_SUBRESOURCES, t->state, state);
    t->state = state;
}

static void buffer_mark_used(B11Buffer *b)
{
    if (b->desc.usage != B11_USAGE_DYNAMIC) return;
    b->backing_serial[b->current] = b->obj.dev->serial + 1;
}

/* ---- pipeline cache ------------------------------------------------------------------------ */

static const uint8_t *shader_code(B11Obj *s, size_t *size)
{
    if (!s) { *size = 0; return NULL; }
    *size = ((B11Shader *)s)->size;
    return ((B11Shader *)s)->bytecode;
}

static BD12GraphicsPipeline *get_pso(B11Context *ctx)
{
    B11Device *dev = ctx->obj.dev;
    b11_flush(dev, 1);
    B11PipelineState *s = &ctx->st;
    B11Shader *vs = (B11Shader *)s->shader[B11_STAGE_VS], *ps = (B11Shader *)s->shader[B11_STAGE_PS];
    if (!vs) return NULL;
    if (s->shader[B11_STAGE_HS] || s->shader[B11_STAGE_DS]) {
        static int warned;
        if (!warned++) fprintf(stderr, "[BD11] tessellation shaders bound: draw skipped (not implemented yet)\n");
        return NULL;
    }
    uint32_t topo_type = 3;
    switch (s->topology) {
    case 1: topo_type = 1; break;
    case 2: case 3: case 10: case 11: topo_type = 2; break;
    default: topo_type = 3; break;
    }
    if (s->topology >= 33) topo_type = 4;
    uint32_t rt_formats[B11_MAX_RT];
    for (uint32_t i = 0; i < s->rtv_count; ++i) rt_formats[i] = s->rtv[i] ? ((B11View *)s->rtv[i])->format : 0;
    const uint32_t ds_format = s->dsv ? ((B11View *)s->dsv)->format : 0;
    uint32_t samples = 1;
    if (s->rtv_count && s->rtv[0]) {
        B11Obj *r = ((B11View *)s->rtv[0])->resource;
        if (r && r->kind == B11_TEXTURE) samples = ((B11Texture *)r)->desc.samples;
    }
    const B11State *blend = (const B11State *)s->blend, *depth = (const B11State *)s->depth,
                   *raster = (const B11State *)s->raster;
    const B11Layout *layout = (const B11Layout *)s->layout;
    uint64_t key = 1469598103934665603ull;
#define MIX(v) do { key = (key ^ (uint64_t)(v)) * 1099511628211ull; } while (0)
    MIX(vs->hash); MIX(ps ? ps->hash : 0); MIX(layout ? layout->hash : 0);
    MIX(blend ? blend->hash : 1); MIX(depth ? depth->hash : 2); MIX(raster ? raster->hash : 3);
    MIX(topo_type); MIX(s->rtv_count); MIX(ds_format); MIX(samples);
    for (uint32_t i = 0; i < s->rtv_count; ++i) MIX(rt_formats[i]);
#undef MIX
    B11PsoEntry *e = dev->pso[key & 4095];
    for (; e; e = e->next) if (e->key == key) return e->pipeline;

    BD12PsoDesc pso;
    bd12_pso_defaults(&pso);
    size_t vs_size = 0, ps_size = 0;
    pso.vs = shader_code(&vs->obj, &vs_size); pso.vs_size = vs_size;
    pso.ps = shader_code(ps ? &ps->obj : NULL, &ps_size); pso.ps_size = ps_size;
    if (layout) { pso.input_elements = layout->elements; pso.input_count = layout->count; }
    if (blend) {
        const uint32_t *w = (const uint32_t *)blend->desc;
        pso.alpha_to_coverage = w[0];
        pso.independent_blend = w[1];
        for (int i = 0; i < 8; ++i) {
            const uint32_t *r = w + 2 + i * 8;
            pso.blend[i] = (BD12PsoBlend){ r[0], 0, r[1], r[2], r[3], r[4], r[5], r[6], 4, r[7] & 0xf };
        }
    }
    if (depth) {
        const uint32_t *w = (const uint32_t *)depth->desc;
        pso.depth_enable = w[0]; pso.depth_write_mask = w[1]; pso.depth_func = w[2]; pso.stencil_enable = w[3];
        pso.stencil_read_mask = depth->desc[16]; pso.stencil_write_mask = depth->desc[17];
        pso.stencil_front = (BD12PsoStencil){ w[5], w[6], w[7], w[8] };
        pso.stencil_back = (BD12PsoStencil){ w[9], w[10], w[11], w[12] };
    }
    if (raster) {
        const uint32_t *w = (const uint32_t *)raster->desc;
        pso.fill_mode = w[0]; pso.cull_mode = w[1]; pso.front_ccw = w[2];
        pso.depth_bias = (int32_t)w[3];
        memcpy(&pso.depth_bias_clamp, &w[4], 4); memcpy(&pso.slope_bias, &w[5], 4);
        pso.depth_clip = w[6];
    }
    pso.topology_type = topo_type;
    pso.strip_cut = (s->topology == 3 || s->topology == 5) ? 1 : 0;
    pso.rt_count = s->rtv_count;
    for (uint32_t i = 0; i < s->rtv_count; ++i) pso.rt_formats[i] = rt_formats[i];
    pso.ds_format = ds_format;
    pso.samples = samples;
    BD12GraphicsPipeline *pipeline = bd12_pso_create_graphics(dev->bd, &pso);
    if (!pipeline) fprintf(stderr, "[BD11] pipeline creation failed (vs=%016llx ps=%016llx)\n",
                           (unsigned long long)vs->hash, (unsigned long long)(ps ? ps->hash : 0));
    e = calloc(1, sizeof(*e));
    if (e) {
        e->key = key; e->pipeline = pipeline;
        e->next = dev->pso[key & 4095];
        dev->pso[key & 4095] = e;
    }
    return pipeline;
}

/* ---- descriptor resolution (called by BD12 at draw / dispatch) ----------------------------- */

static uint32_t translator_stage_to_b11(uint32_t stage)
{
    switch (stage) {
    case 0: return B11_STAGE_CS;
    case 1: return B11_STAGE_VS;
    case 2: return B11_STAGE_PS;
    case 5: return B11_STAGE_GS;
    case 6: return B11_STAGE_HS;
    case 7: return B11_STAGE_DS;
    default: return B11_STAGE_VS;
    }
}

VkDescriptorSet b11_resolve_hook(BD12CmdList *list, void *user, const BD12ShaderBinding *bindings,
                                 const VkDescriptorType *types, uint32_t count,
                                 VkDescriptorSetLayout layout, int compute)
{
    (void)compute;
    B11Context *ctx = user;
    B11Device *dev = ctx->obj.dev;
    BD12Device *bd = list->device;
    VkDescriptorSet set = bd12_allocate_descriptor_set(list->allocator, layout);
    if (!set) return VK_NULL_HANDLE;
    enum { MAX = 64 };
    VkWriteDescriptorSet writes[MAX];
    VkDescriptorBufferInfo buffer_infos[MAX];
    VkDescriptorImageInfo image_infos[MAX];
    VkBufferView views[MAX];
    uint32_t wn = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const BD12ShaderBinding *b = &bindings[i];
        const uint32_t stage = translator_stage_to_b11(b->stage);
        if (wn == MAX) {
            bd->vk.UpdateDescriptorSets(bd->device, wn, writes, 0, NULL);
            wn = 0;
        }
        VkWriteDescriptorSet *w = &writes[wn];
        *w = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
                                     .dstBinding = b->binding, .descriptorCount = 1, .descriptorType = types[i] };
        const uint32_t reg = b->reg;
        switch (types[i]) {
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: {
            B11Buffer *cb = reg < B11_MAX_CBV ? (B11Buffer *)ctx->st.cb[stage][reg] : NULL;
            VkBuffer vb = VK_NULL_HANDLE;
            VkDeviceSize range = 0;
            if (cb) {
                BD12Buffer *bk = b11_buffer_current(cb);
                buffer_mark_used(cb);
                vb = bk->buffer;
                range = (cb->desc.byte_width + 15u) & ~15u;
                if (range > bd->max_uniform_range) range = bd->max_uniform_range;
            } else if (dev->null_buffer) {
                vb = dev->null_buffer->buffer;
                range = 4096 * 16;
            }
            buffer_infos[wn] = (VkDescriptorBufferInfo){ vb, 0, range ? range : VK_WHOLE_SIZE };
            w->pBufferInfo = &buffer_infos[wn];
            break;
        }
        case VK_DESCRIPTOR_TYPE_SAMPLER: {
            B11State *ss = reg < B11_MAX_SAMPLER ? (B11State *)ctx->st.sampler[stage][reg] : NULL;
            VkSamplerCreateInfo info;
            if (ss) info = ss->sampler;
            else info = (VkSamplerCreateInfo){ .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
                .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = VK_LOD_CLAMP_NONE };
            image_infos[wn] = (VkDescriptorImageInfo){ bd12_sampler_get(bd, &info), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
            w->pImageInfo = &image_infos[wn];
            break;
        }
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: {
            B11View *v = NULL;
            if (types[i] == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) v = reg < B11_MAX_SRV ? (B11View *)ctx->st.srv[stage][reg] : NULL;
            else v = reg < B11_MAX_UAV ? (B11View *)ctx->st.uav[stage][reg] : NULL;
            VkImageView iv = VK_NULL_HANDLE;
            VkImageLayout layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
            if (v && v->desc && v->desc->image_view) {
                iv = v->desc->image_view;
                layout_ = v->desc->image_layout;
                if (v->desc->image) bd12_image_ensure_initialized(list, v->desc->image);
            }
            image_infos[wn] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, iv, layout_ };
            w->pImageInfo = &image_infos[wn];
            break;
        }
        default: {   /* texel buffers */
            B11View *v = NULL;
            if (types[i] == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) v = reg < B11_MAX_SRV ? (B11View *)ctx->st.srv[stage][reg] : NULL;
            else v = reg < B11_MAX_UAV ? (B11View *)ctx->st.uav[stage][reg] : NULL;
            VkBufferView bv = VK_NULL_HANDLE;
            if (v && v->desc) {
                bv = v->desc->buffer_view;
                if (!bv && v->desc->source_buffer)
                    bv = bd12_buffer_view(v->desc->source_buffer, v->desc->format, v->desc->offset, v->desc->range,
                                          types[i] == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER);
                if (v->resource && v->resource->kind == B11_BUFFER) buffer_mark_used((B11Buffer *)v->resource);
            }
            views[wn] = bv;
            w->pTexelBufferView = &views[wn];
            break;
        }
        }
        ++wn;
    }
    if (wn) bd->vk.UpdateDescriptorSets(bd->device, wn, writes, 0, NULL);
    return set;
}

/* ---- draw preparation --------------------------------------------------------------------- */

static void use_srvs(B11Context *ctx, int compute)
{
    B11PipelineState *s = &ctx->st;
    const int first = compute ? B11_STAGE_CS : 0, last = compute ? B11_STAGE_CS : B11_STAGE_PS;
    for (int st = first; st <= last; ++st) {
        if (!s->shader[st]) continue;
        for (int i = 0; i < B11_MAX_SRV; ++i) {
            B11View *v = (B11View *)s->srv[st][i];
            if (v && v->resource && v->resource->kind == B11_TEXTURE)
                b11_texture_use(ctx, (B11Texture *)v->resource, BD12_STATE_PIXEL_SHADER_RESOURCE | BD12_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        for (int i = 0; i < B11_MAX_UAV; ++i) {
            B11View *v = (B11View *)s->uav[st][i];
            if (v && v->resource && v->resource->kind == B11_TEXTURE)
                b11_texture_use(ctx, (B11Texture *)v->resource, BD12_STATE_UNORDERED_ACCESS);
        }
    }
}

uint64_t g_b11_stats[16];
enum { ST_DRAW, ST_DRAW_SKIPPED, ST_CLEAR_RTV, ST_CLEAR_DSV, ST_COPY, ST_UPDATE, ST_BUFWRITE, ST_REPLAY_OPS, ST_LISTS };

static int prepare_draw(B11Context *ctx)
{
    B11Device *dev = ctx->obj.dev;
    B11PipelineState *s = &ctx->st;
    BD12CmdList *cmd = b11_cmd(dev);
    if (ctx->generation != dev->cmd_generation) {
        ctx->generation = dev->cmd_generation;
        ctx->dirty = B11_DIRTY_ALL;
        memset(ctx->bound_vb_va, 0, sizeof(ctx->bound_vb_va));
        ctx->bound_ib_va = 0;
    }
    cmd->resolve_user = ctx;
    BD12GraphicsPipeline *pipeline = get_pso(ctx);
    if (!pipeline) { g_b11_stats[ST_DRAW_SKIPPED]++; return 0; }
    g_b11_stats[ST_DRAW]++;

    /* Resource states first: transitions cannot happen inside a rendering scope. */
    use_srvs(ctx, 0);
    BD12Descriptor rtv[B11_MAX_RT];
    const BD12Descriptor *rtv_ptr[B11_MAX_RT];
    uint32_t rt_w = 0, rt_h = 0;
    for (uint32_t i = 0; i < s->rtv_count; ++i) {
        B11View *v = (B11View *)s->rtv[i];
        if (!v || !v->desc) { memset(&rtv[i], 0, sizeof(rtv[i])); rtv_ptr[i] = &rtv[i]; continue; }
        if (v->resource && v->resource->kind == B11_TEXTURE) b11_texture_use(ctx, (B11Texture *)v->resource, BD12_STATE_RENDER_TARGET);
        rtv[i] = *v->desc;
        rtv_ptr[i] = &rtv[i];
        if (!rt_w) { rt_w = v->desc->width; rt_h = v->desc->height; }
    }
    BD12Descriptor dsv;
    const BD12Descriptor *dsv_ptr = NULL;
    if (s->dsv) {
        B11View *v = (B11View *)s->dsv;
        if (v->desc) {
            const uint32_t ro = (v->flags & 1) ? BD12_STATE_DEPTH_READ : BD12_STATE_DEPTH_WRITE;
            if (v->resource && v->resource->kind == B11_TEXTURE) b11_texture_use(ctx, (B11Texture *)v->resource, ro);
            dsv = *v->desc;
            dsv_ptr = &dsv;
            if (!rt_w) { rt_w = v->desc->width; rt_h = v->desc->height; }
        }
    }
    if (ctx->dirty & (B11_DIRTY_TARGETS)) {
        bd12_cmd_set_render_targets(cmd, rtv_ptr, s->rtv_count, dsv_ptr);
    }
    if (ctx->dirty & B11_DIRTY_PIPELINE) bd12_cmd_set_graphics_pipeline(cmd, pipeline);
    else bd12_cmd_set_graphics_pipeline(cmd, pipeline);   /* cheap: BD12 skips an unchanged pipeline */

    if (ctx->dirty & B11_DIRTY_VIEWPORT) {
        VkViewport vp[B11_MAX_VIEWPORTS];
        uint32_t n = s->viewport_count;
        if (!n) { vp[0] = (VkViewport){ 0, 0, (float)rt_w, (float)rt_h, 0.0f, 1.0f }; n = 1; }
        else for (uint32_t i = 0; i < n; ++i)
            vp[i] = (VkViewport){ s->viewports[i][0], s->viewports[i][1], s->viewports[i][2], s->viewports[i][3],
                                  s->viewports[i][4], s->viewports[i][5] };
        bd12_cmd_set_viewports(cmd, vp, n);
    }
    if (ctx->dirty & B11_DIRTY_SCISSOR) {
        VkRect2D sc[B11_MAX_VIEWPORTS];
        uint32_t n = s->viewport_count ? s->viewport_count : 1;
        const B11State *raster = (const B11State *)s->raster;
        const int enabled = raster && ((const uint32_t *)raster->desc)[7];
        for (uint32_t i = 0; i < n; ++i) {
            if (enabled && i < s->scissor_count) {
                const int32_t *r = s->scissors[i];
                sc[i] = (VkRect2D){ { r[0], r[1] }, { (uint32_t)(r[2] > r[0] ? r[2] - r[0] : 0),
                                                       (uint32_t)(r[3] > r[1] ? r[3] - r[1] : 0) } };
            } else {
                sc[i] = (VkRect2D){ { 0, 0 }, { rt_w ? rt_w : 16384, rt_h ? rt_h : 16384 } };
            }
        }
        bd12_cmd_set_scissors(cmd, sc, n);
    }
    if (ctx->dirty & B11_DIRTY_TOPOLOGY) {
        uint32_t patch;
        bd12_cmd_set_topology(cmd, bd12_topology_to_vk(s->topology ? s->topology : 4, &patch));
    }
    {   /* A Map(DISCARD) renamed backing changes the VA without any Set* call: re-validate. */
        uint64_t ib_va = 0;
        if (s->ib) ib_va = b11_buffer_current((B11Buffer *)s->ib)->va + s->ib_offset;
        if (ib_va != ctx->bound_ib_va) { ctx->dirty |= B11_DIRTY_IB; ctx->bound_ib_va = ib_va; }
        for (uint32_t i = 0; i < B11_MAX_VB; ++i) {
            uint64_t va = s->vb[i] ? b11_buffer_current((B11Buffer *)s->vb[i])->va + s->vb_offset[i] : 0;
            if (va != ctx->bound_vb_va[i]) { ctx->dirty |= B11_DIRTY_VB; ctx->bound_vb_va[i] = va; }
        }
    }
    if (ctx->dirty & B11_DIRTY_VB) {
        uint64_t va[B11_MAX_VB]; uint32_t sizes[B11_MAX_VB], strides[B11_MAX_VB];
        uint32_t n = 0;
        for (uint32_t i = 0; i < B11_MAX_VB; ++i) {
            B11Buffer *vb = (B11Buffer *)s->vb[i];
            if (vb) {
                BD12Buffer *bk = b11_buffer_current(vb);
                va[i] = bk->va + s->vb_offset[i];
                sizes[i] = vb->desc.byte_width > s->vb_offset[i] ? vb->desc.byte_width - s->vb_offset[i] : 0;
                strides[i] = s->vb_stride[i];
                n = i + 1;
            } else { va[i] = 0; sizes[i] = 0; strides[i] = 0; }
        }
        if (n) bd12_cmd_set_vertex_buffers(cmd, 0, va, sizes, strides, n);
    }
    if (ctx->dirty & B11_DIRTY_IB) {
        B11Buffer *ib = (B11Buffer *)s->ib;
        if (ib) {
            BD12Buffer *bk = b11_buffer_current(ib);
            bd12_cmd_set_index_buffer(cmd, bk->va + s->ib_offset, ib->desc.byte_width - s->ib_offset,
                                      s->ib_format == 42 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
        }
    }
    if (ctx->dirty & B11_DIRTY_BLEND_CONST) bd12_cmd_set_blend_constants(cmd, s->blend_factor);
    if (ctx->dirty & B11_DIRTY_STENCIL_REF) bd12_cmd_set_stencil_ref(cmd, s->stencil_ref);
    ctx->dirty = 0;
    /* Dynamic vertex/index/constant data written by Map must not be clobbered while in flight. */
    for (uint32_t i = 0; i < B11_MAX_VB; ++i) if (s->vb[i]) buffer_mark_used((B11Buffer *)s->vb[i]);
    if (s->ib) buffer_mark_used((B11Buffer *)s->ib);
    ctx->draw_count++;
    return 1;
}

/* BEER_BD11_SPLIT_DRAWS=1: close the rendering scope after every draw so BD12's per-pass capture
 * (BEER_BD12_CAPTURE_RT) writes one image per draw. */
static void after_draw(B11Context *ctx)
{
    static int split = -1;
    if (split < 0) split = getenv("BEER_BD11_SPLIT_DRAWS") != NULL;
    if (split) bd12_cmd_end_rendering(b11_cmd(ctx->obj.dev));
    /* BEER_BD11_DUMP_DRAWS=<dir> with BEER_BD11_TRACE=<frame>: write RT0 after every draw of that frame as PPM. */
    if (getenv("BEER_BD11_DUMP_DRAWS") && getenv("BEER_BD11_TRACE") && (long)ctx->obj.dev->frame == strtol(getenv("BEER_BD11_TRACE"), NULL, 10) &&
        ctx->st.rtv_count && ctx->st.rtv[0]) {
        B11Device *dev = ctx->obj.dev;
        B11View *v = (B11View *)ctx->st.rtv[0];
        B11Texture *t = v->resource && v->resource->kind == B11_TEXTURE ? (B11Texture *)v->resource : NULL;
        static unsigned n;
        if (t && t->image && (t->desc.format == 28 || t->desc.format == 24)) {
            const uint32_t w = t->desc.width, h = t->desc.height;
            BD12Buffer *rb = bd12_buffer_create_committed(dev->bd, BD12_HEAP_READBACK, (size_t)w * h * 4);
            if (rb) {
                BD12ImagePoint origin = {0};
                const uint32_t old = t->state;
                b11_texture_use(ctx, t, BD12_STATE_COPY_SOURCE);
                bd12_cmd_global_barrier(b11_cmd(dev));
                bd12_cmd_copy_image_to_buffer(b11_cmd(dev), rb, 0, w * 4, t->image, origin, w, h, 1);
                b11_texture_use(ctx, t, old);
                b11_flush(dev, 1);
                char path[600];
                snprintf(path, sizeof(path), "%s/draw%02u_%ux%u_f%u.ppm", getenv("BEER_BD11_DUMP_DRAWS"), n++, w, h, t->desc.format);
                FILE *f = fopen(path, "wb");
                if (f) {
                    fprintf(f, "P6\n%u %u\n255\n", w, h);
                    const uint32_t *px = rb->cpu;
                    for (size_t i = 0; i < (size_t)w * h; ++i) {
                        uint8_t rgb[3];
                        if (t->desc.format == 28) { rgb[0] = px[i] & 255; rgb[1] = (px[i] >> 8) & 255; rgb[2] = (px[i] >> 16) & 255; }
                        else { rgb[0] = ((px[i] & 1023) >> 2); rgb[1] = (((px[i] >> 10) & 1023) >> 2); rgb[2] = (((px[i] >> 20) & 1023) >> 2); }
                        fwrite(rgb, 1, 3, f);
                    }
                    fclose(f);
                }
                bd12_buffer_destroy(rb);
            }
        }
    }
    static int sync = -1;
    if (sync < 0) sync = getenv("BEER_BD11_SYNC_DRAWS") != NULL;
    if (sync) b11_flush(ctx->obj.dev, 1);
}

/* ---- operations with GPU effect ------------------------------------------------------------- */

static void do_clear_rtv(B11Context *ctx, const B11Op *op)
{
    B11View *v = (B11View *)op->obj[0];
    if (!v || !v->desc) return;
    if (v->resource && v->resource->kind == B11_TEXTURE) b11_texture_use(ctx, (B11Texture *)v->resource, BD12_STATE_RENDER_TARGET);
    float color[4];
    memcpy(color, op->data, 16);
    if (getenv("BEER_BD11_TRACE") && (long)ctx->obj.dev->frame == strtol(getenv("BEER_BD11_TRACE"), NULL, 10))
        fprintf(stderr, "[BD11 CLEAR] rtv res=%p %g %g %g %g\n", (void *)v->resource, color[0], color[1], color[2], color[3]);
    bd12_cmd_clear_rtv(b11_cmd(ctx->obj.dev), v->desc, color, 0, NULL);
}

static void do_clear_dsv(B11Context *ctx, const B11Op *op)
{
    B11View *v = (B11View *)op->obj[0];
    if (!v || !v->desc) return;
    if (v->resource && v->resource->kind == B11_TEXTURE) b11_texture_use(ctx, (B11Texture *)v->resource, BD12_STATE_DEPTH_WRITE);
    VkImageAspectFlags aspects = 0;
    if (op->a & 1) aspects |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (op->a & 2) aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
    float depth;
    memcpy(&depth, &op->b, 4);
    bd12_cmd_clear_dsv(b11_cmd(ctx->obj.dev), v->desc, aspects, depth, op->c, 0, NULL);
}

/* Copies between full resources or boxes; buffer<->buffer and image<->image (+ staging images). */
static void texture_extent(const B11Texture *t, uint32_t mip, uint32_t *w, uint32_t *h, uint32_t *d)
{
    *w = t->desc.width >> mip; if (!*w) *w = 1;
    *h = t->desc.dimension == 1 ? 1 : t->desc.height >> mip; if (!*h) *h = 1;
    *d = t->desc.dimension == 3 ? t->desc.depth >> mip : 1; if (!*d) *d = 1;
}

static void do_copy(B11Context *ctx, B11Obj *dst_o, uint32_t dst_sub, uint32_t dx, uint32_t dy, uint32_t dz,
                    B11Obj *src_o, uint32_t src_sub, const uint32_t *box, int whole)
{
    B11Device *dev = ctx->obj.dev;
    BD12CmdList *cmd = b11_cmd(dev);
    if (!dst_o || !src_o) return;
    if (dst_o->kind == B11_BUFFER && src_o->kind == B11_BUFFER) {
        B11Buffer *d = (B11Buffer *)dst_o, *s = (B11Buffer *)src_o;
        BD12Buffer *db = b11_buffer_write_backing(d, 0), *sb = b11_buffer_current(s);
        uint64_t sz = s->desc.byte_width, off = 0;
        if (!whole && box) { off = box[0]; sz = box[3] - box[0]; }
        if (dx + sz > d->desc.byte_width) sz = d->desc.byte_width - dx;
        bd12_cmd_global_barrier(cmd);
        bd12_cmd_copy_buffer(cmd, db, dx, sb, off, sz);
        bd12_cmd_global_barrier(cmd);
        buffer_mark_used(d); buffer_mark_used(s);
        return;
    }
    if (dst_o->kind != B11_TEXTURE || src_o->kind != B11_TEXTURE) return;
    B11Texture *d = (B11Texture *)dst_o, *s = (B11Texture *)src_o;
    const uint32_t mips_s = s->desc.mips, mips_d = d->desc.mips;
    const uint32_t smip = src_sub % mips_s, dmip = dst_sub % mips_d;
    uint32_t sw, sh, sd;
    texture_extent(s, smip, &sw, &sh, &sd);
    uint32_t bx = 0, by = 0, bz = 0, w = sw, h = sh, depth = sd;
    if (!whole && box) { bx = box[0]; by = box[1]; bz = box[2]; w = box[3] - box[0]; h = box[4] - box[1]; depth = box[5] - box[2]; }
    if (d->image && s->image) {
        b11_texture_use(ctx, s, BD12_STATE_COPY_SOURCE);
        b11_texture_use(ctx, d, BD12_STATE_COPY_DEST);
        bd12_cmd_copy_image(cmd, d->image, (BD12ImagePoint){ dst_sub, (int32_t)dx, (int32_t)dy, (int32_t)dz },
                            s->image, (BD12ImagePoint){ src_sub, (int32_t)bx, (int32_t)by, (int32_t)bz }, w, h, depth);
    } else if (d->staging && s->image) {            /* GPU -> staging readback */
        b11_texture_use(ctx, s, BD12_STATE_COPY_SOURCE);
        bd12_cmd_copy_image_to_buffer(cmd, d->staging, d->sub_offset[dst_sub], d->sub_pitch[dst_sub], s->image,
                                      (BD12ImagePoint){ src_sub, (int32_t)bx, (int32_t)by, (int32_t)bz }, w, h, depth);
        bd12_cmd_host_visibility(cmd, d->staging);
        d->last_use_serial = dev->serial + 1;
    } else if (d->image && s->staging) {            /* staging -> GPU */
        b11_texture_use(ctx, d, BD12_STATE_COPY_DEST);
        bd12_cmd_copy_buffer_to_image(cmd, d->image, (BD12ImagePoint){ dst_sub, (int32_t)dx, (int32_t)dy, (int32_t)dz },
                                      s->staging, s->sub_offset[src_sub], s->sub_pitch[src_sub], w, h, depth);
    }
}

static void do_update(B11Context *ctx, const B11Op *op)
{
    B11Device *dev = ctx->obj.dev;
    BD12CmdList *cmd = b11_cmd(dev);
    const uint8_t *payload = op->data;
    if (!payload || !op->obj[0]) return;
    const uint32_t *box = op->d ? (const uint32_t *)payload : NULL;
    const uint8_t *src = payload + (op->d ? 24 : 0);
    if (op->obj[0]->kind == B11_BUFFER) {
        B11Buffer *b = (B11Buffer *)op->obj[0];
        const uint32_t off = box ? box[0] : 0;
        const uint32_t size = box ? box[3] - box[0] : b->desc.byte_width;
        BD12Buffer *bk = b11_buffer_write_backing(b, 0);
        if (b->host_visible && bk->cpu) { memcpy((uint8_t *)bk->cpu + off, src, size); buffer_mark_used(b); return; }
        if (size <= 65536 && (off & 3) == 0 && (size & 3) == 0) bd12_cmd_update_buffer(cmd, bk, off, size, src);
        else {
            BD12Buffer *stage = bd12_buffer_create_committed(dev->bd, BD12_HEAP_UPLOAD, (size + 255u) & ~255u);
            if (!stage || !stage->cpu) return;
            memcpy(stage->cpu, src, size);
            bd12_cmd_global_barrier(cmd);                 /* earlier draws may still read this buffer */
            bd12_cmd_copy_buffer(cmd, bk, off, stage, 0, size);
            b11_defer(dev, b11_free_buffer_fn, stage);
        }
        bd12_cmd_global_barrier(cmd);
        return;
    }
    if (op->obj[0]->kind != B11_TEXTURE) return;
    B11Texture *t = (B11Texture *)op->obj[0];
    if (!t->image) return;
    const uint32_t sub = op->a, mip = sub % t->desc.mips;
    uint32_t w, h, d;
    texture_extent(t, mip, &w, &h, &d);
    uint32_t x0 = 0, y0 = 0, z0 = 0;
    if (box) { x0 = box[0]; y0 = box[1]; z0 = box[2]; w = box[3] - box[0]; h = box[4] - box[1]; d = box[5] - box[2]; }
    const VkFormat f = b11_vk_format(t->desc.format);
    uint32_t bw = 1, bh = 1, bytes = bd12_dxgi_bytes(t->desc.format);
    if (f != VK_FORMAT_UNDEFINED) bd12_format_block(f, VK_IMAGE_ASPECT_COLOR_BIT, &bw, &bh, &bytes);
    const uint32_t row_bytes = ((w + bw - 1) / bw) * bytes, rows = (h + bh - 1) / bh;
    BD12Buffer *stage = bd12_buffer_create_committed(dev->bd, BD12_HEAP_UPLOAD, ((uint64_t)row_bytes * rows * d + 255u) & ~255ull);
    if (!stage || !stage->cpu) return;
    for (uint32_t z = 0; z < d; ++z)
        for (uint32_t r = 0; r < rows; ++r)
            memcpy((uint8_t *)stage->cpu + ((uint64_t)z * rows + r) * row_bytes,
                   src + (uint64_t)z * op->c + (uint64_t)r * op->b, row_bytes);
    b11_texture_use(ctx, t, BD12_STATE_COPY_DEST);
    bd12_cmd_copy_buffer_to_image(cmd, t->image, (BD12ImagePoint){ sub, (int32_t)x0, (int32_t)y0, (int32_t)z0 },
                                  stage, 0, row_bytes, w, h, d);
    b11_defer(dev, b11_free_buffer_fn, stage);
}

static void do_buffer_write(B11Context *ctx, const B11Op *op)
{
    (void)ctx;
    B11Buffer *b = (B11Buffer *)op->obj[0];
    if (!b || !op->data) return;
    BD12Buffer *bk = b11_buffer_write_backing(b, op->a != 0);
    if (bk && bk->cpu) memcpy((uint8_t *)bk->cpu + op->b, op->data, op->data_size);
    buffer_mark_used(b);
}

static void do_dispatch(B11Context *ctx, uint32_t x, uint32_t y, uint32_t z)
{
    (void)x; (void)y; (void)z;
    static int warned;
    if (!warned++) fprintf(stderr, "[BD11] compute dispatch not implemented yet\n");
    (void)ctx;
}


/* BEER_BD11_TRACE=<frame>: describe every draw of that presented frame. */
static void trace_draw(B11Context *ctx, const char *kind, const B11Op *op)
{
    B11Device *dev = ctx->obj.dev;
    static long want = -2;
    if (want == -2) { const char *e = getenv("BEER_BD11_TRACE"); want = e ? strtol(e, NULL, 10) : -1; }
    if (want < 0 || (long)dev->frame != want) return;
    B11PipelineState *s = &ctx->st;
    B11Shader *vs = (B11Shader *)s->shader[B11_STAGE_VS], *ps = (B11Shader *)s->shader[B11_STAGE_PS];
    fprintf(stderr, "[BD11 DRAW] %s a=%u b=%u c=%u d=%u vs=%016llx ps=%016llx topo=%u rt=%u fmt0=%u vp=%u(%.0f,%.0f %.0fx%.0f z %.2f-%.2f)\n",
            kind, op->a, op->b, op->c, op->d, (unsigned long long)(vs ? vs->hash : 0), (unsigned long long)(ps ? ps->hash : 0),
            s->topology, s->rtv_count, s->rtv_count && s->rtv[0] ? ((B11View *)s->rtv[0])->format : 0, s->viewport_count,
            s->viewports[0][0], s->viewports[0][1], s->viewports[0][2], s->viewports[0][3], s->viewports[0][4], s->viewports[0][5]);
    for (uint32_t i = 0; i < s->rtv_count; ++i) if (s->rtv[i]) fprintf(stderr, "    rtv[%u] res=%p\n", i, (void *)((B11View *)s->rtv[i])->resource);
    for (int i = 0; i < B11_MAX_SRV; ++i) if (s->srv[4][i]) fprintf(stderr, "    srv4[%d] res=%p\n", i, (void *)((B11View *)s->srv[4][i])->resource);
    const B11State *bl = (const B11State *)s->blend, *dp = (const B11State *)s->depth, *rs = (const B11State *)s->raster;
    if (bl) { const uint32_t *w = (const uint32_t *)bl->desc; fprintf(stderr, "    blend: en=%u src=%u dst=%u op=%u a:%u/%u/%u mask=%x\n", w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9]); }
    if (dp) { const uint32_t *w = (const uint32_t *)dp->desc; fprintf(stderr, "    depth: en=%u write=%u func=%u stencil=%u\n", w[0], w[1], w[2], w[3]); }
    if (rs) { const uint32_t *w = (const uint32_t *)rs->desc; fprintf(stderr, "    raster: fill=%u cull=%u ccw=%u clip=%u scissor=%u\n", w[0], w[1], w[2], w[6], w[7]); }
    for (int st = B11_STAGE_VS; st <= B11_STAGE_PS; ++st) {
        for (int i = 0; i < B11_MAX_SRV; ++i) {
            B11View *v = (B11View *)s->srv[st][i];
            if (!v) continue;
            if (v->resource->kind == B11_TEXTURE) {
                B11Texture *t = (B11Texture *)v->resource;
                fprintf(stderr, "    srv[%d][%d] tex %ux%u fmt=%u mips=%u view fmt=%u mip=%u+%u layers=%u+%u state=%x\n", st, i, t->desc.width, t->desc.height,
                        t->desc.format, t->desc.mips, v->format, v->base_mip, v->mip_count, v->base_layer, v->layer_count, t->state);
            } else fprintf(stderr, "    srv[%d][%d] buffer bytes=%u first=%u count=%u\n", st, i, ((B11Buffer *)v->resource)->desc.byte_width, v->first, v->count);
        }
        for (int i = 0; i < B11_MAX_CBV; ++i) {
            B11Buffer *b = (B11Buffer *)s->cb[st][i];
            if (b) fprintf(stderr, "    cb[%d][%d] %u bytes\n", st, i, b->desc.byte_width);
        }
        for (int i = 0; i < B11_MAX_SAMPLER; ++i)
            if (s->sampler[st][i]) {
                const uint32_t *w = (const uint32_t *)((B11State *)s->sampler[st][i])->desc;
                fprintf(stderr, "    sampler[%d][%d] filter=%x addr=%u,%u,%u\n", st, i, w[0], w[1], w[2], w[3]);
            }
    }
    for (int i = 0; i < B11_MAX_VB; ++i)
        if (s->vb[i]) fprintf(stderr, "    vb[%d] bytes=%u stride=%u offset=%u\n", i, ((B11Buffer *)s->vb[i])->desc.byte_width, s->vb_stride[i], s->vb_offset[i]);
    if (s->ib) fprintf(stderr, "    ib bytes=%u fmt=%u offset=%u\n", ((B11Buffer *)s->ib)->desc.byte_width, s->ib_format, s->ib_offset);
    if (s->ib && s->vb[0] && (op->type == B11_OP_DRAW_INDEXED)) {
        B11Buffer *ib = (B11Buffer *)s->ib, *vb = (B11Buffer *)s->vb[0];
        BD12Buffer *ibk = b11_buffer_current(ib), *vbk = b11_buffer_current(vb);
        if (ibk->cpu && vbk->cpu) {
            const uint32_t isz = s->ib_format == 42 ? 4 : 2;
            fprintf(stderr, "    indices:");
            for (uint32_t i = 0; i < op->a && i < 12; ++i) {
                uint32_t v = 0;
                memcpy(&v, (const uint8_t *)ibk->cpu + s->ib_offset + (size_t)(op->b + i) * isz, isz);
                fprintf(stderr, " %u", v);
            }
            fprintf(stderr, "\n    verts@%u:", s->vb_offset[0]);
            const uint32_t stride = s->vb_stride[0] ? s->vb_stride[0] : 16;
            for (uint32_t i = 0; i < 4; ++i) {
                fprintf(stderr, " [");
                const uint8_t *p = (const uint8_t *)vbk->cpu + s->vb_offset[0] + (size_t)((int32_t)op->c + i) * stride;
                for (uint32_t k = 0; k < stride; ++k) fprintf(stderr, "%02x", p[k]);
                fprintf(stderr, "]");
            }
            fprintf(stderr, "\n");
        }
    }
    if (s->cb[B11_STAGE_VS][0]) {
        BD12Buffer *cbk = b11_buffer_current((B11Buffer *)s->cb[B11_STAGE_VS][0]);
        if (cbk->cpu) {
            const float *f = (const float *)cbk->cpu;
            fprintf(stderr, "    vscb0:");
            for (int i = 0; i < 24; ++i) fprintf(stderr, " %g", f[i]);
            fprintf(stderr, "\n");
        }
    }
    B11Layout *ly = (B11Layout *)s->layout;
    if (ly) for (uint32_t i = 0; i < ly->count; ++i) {
        const uint32_t *e = (const uint32_t *)(ly->elements + (size_t)i * 32);
        fprintf(stderr, "    input %s%u fmt=%u slot=%u off=%u class=%u\n", ly->names[i], e[2], e[3], e[4], e[5], e[6]);
    }
}

static void execute(B11Context *ctx, const B11Op *op)
{
    apply_state(ctx, op);
    B11PipelineState *s = &ctx->st;
    (void)s;
    switch (op->type) {
    case B11_OP_DRAW:
        trace_draw(ctx, "Draw", op);
        if (prepare_draw(ctx)) { bd12_cmd_draw(b11_cmd(ctx->obj.dev), op->a, 1, op->b, 0); after_draw(ctx); }
        break;
    case B11_OP_DRAW_INSTANCED:
        trace_draw(ctx, "DrawInstanced", op);
        if (prepare_draw(ctx)) { bd12_cmd_draw(b11_cmd(ctx->obj.dev), op->a, op->b, op->c, op->d); after_draw(ctx); }
        break;
    case B11_OP_DRAW_INDEXED:
        trace_draw(ctx, "DrawIndexed", op);
        if (prepare_draw(ctx)) { bd12_cmd_draw_indexed(b11_cmd(ctx->obj.dev), op->a, 1, op->b, (int32_t)op->c, 0); after_draw(ctx); }
        break;
    case B11_OP_DRAW_INDEXED_INSTANCED:
        trace_draw(ctx, "DrawIndexedInstanced", op);
        if (prepare_draw(ctx)) { bd12_cmd_draw_indexed(b11_cmd(ctx->obj.dev), op->a, op->b, op->c, (int32_t)op->d, op->e); after_draw(ctx); }
        break;
    case B11_OP_DISPATCH: do_dispatch(ctx, op->a, op->b, op->c); break;
    case B11_OP_CLEAR_RTV: g_b11_stats[ST_CLEAR_RTV]++; do_clear_rtv(ctx, op); break;
    case B11_OP_CLEAR_DSV: g_b11_stats[ST_CLEAR_DSV]++; do_clear_dsv(ctx, op); break;
    case B11_OP_COPY_RESOURCE: do_copy(ctx, op->obj[0], 0, 0, 0, 0, op->obj[1], 0, NULL, 1); break;
    case B11_OP_COPY_REGION:
        do_copy(ctx, op->obj[0], op->a, op->b, op->c, op->d, op->obj[1], op->e, op->f ? (const uint32_t *)op->data : NULL, !op->f);
        break;
    case B11_OP_UPDATE: g_b11_stats[ST_UPDATE]++; do_update(ctx, op); break;
    case B11_OP_BUFFER_WRITE: g_b11_stats[ST_BUFWRITE]++; do_buffer_write(ctx, op); break;
    default: break;
    }
}

/* ---- emit / record / replay --------------------------------------------------------------- */

static void op_release(B11Op *op)
{
    for (int i = 0; i < 3; ++i) if (op->obj[i]) b11_release(op->obj[i]);
    if (op->data) {
        if (op->type == B11_OP_SET_IA_VB) { VbEnt *e = op->data; for (uint32_t i = 0; i < op->b; ++i) if (e[i].obj) b11_release(e[i].obj); }
        else if (op->type == B11_OP_SET_CB || op->type == B11_OP_SET_SRV || op->type == B11_OP_SET_SAMPLER) {
            B11Obj **e = op->data; for (uint32_t i = 0; i < op->c; ++i) if (e[i]) b11_release(e[i]);
        } else if (op->type == B11_OP_SET_UAV_CS) {
            struct { B11Obj *obj; uint32_t initial; } *e = op->data;
            for (uint32_t i = 0; i < op->b; ++i) if (e[i].obj) b11_release(e[i].obj);
        } else if (op->type == B11_OP_SET_RT) {
            RtOp *r = op->data;
            for (uint32_t i = 0; i < r->rtv_count; ++i) if (r->rtv[i]) b11_release(r->rtv[i]);
            if (r->dsv) b11_release(r->dsv);
            for (uint32_t i = 0; i < r->uav_count && i < B11_MAX_UAV; ++i) if (r->uav[i]) b11_release(r->uav[i]);
        }
        free(op->data);
    }
    memset(op, 0, sizeof(*op));
}

/* Takes ownership of op->data. Objects in op->obj[] and payload arrays are retained here. */
static void retain_op_refs(B11Op *op)
{
    for (int i = 0; i < 3; ++i) b11_retain(op->obj[i]);
    if (!op->data) return;
    if (op->type == B11_OP_SET_IA_VB) { VbEnt *e = op->data; for (uint32_t i = 0; i < op->b; ++i) b11_retain(e[i].obj); }
    else if (op->type == B11_OP_SET_CB || op->type == B11_OP_SET_SRV || op->type == B11_OP_SET_SAMPLER) {
        B11Obj **e = op->data; for (uint32_t i = 0; i < op->c; ++i) b11_retain(e[i]);
    } else if (op->type == B11_OP_SET_UAV_CS) {
        struct { B11Obj *obj; uint32_t initial; } *e = op->data;
        for (uint32_t i = 0; i < op->b; ++i) b11_retain(e[i].obj);
    } else if (op->type == B11_OP_SET_RT) {
        RtOp *r = op->data;
        for (uint32_t i = 0; i < r->rtv_count; ++i) b11_retain(r->rtv[i]);
        b11_retain(r->dsv);
        for (uint32_t i = 0; i < r->uav_count && i < B11_MAX_UAV; ++i) b11_retain(r->uav[i]);
    }
}

static void ctx_emit(B11Context *ctx, B11Op *op)
{
    B11Device *dev = ctx->obj.dev;
    if (ctx->type == 1) {                       /* deferred: shadow + record */
        apply_state(ctx, op);
        if (ctx->op_count == ctx->op_capacity) {
            const size_t cap = ctx->op_capacity ? ctx->op_capacity * 2 : 256;
            B11Op *n = realloc(ctx->ops, cap * sizeof(*n));
            if (!n) { free(op->data); return; }
            ctx->ops = n; ctx->op_capacity = cap;
        }
        retain_op_refs(op);
        ctx->ops[ctx->op_count++] = *op;
        return;
    }
    pthread_mutex_lock(&dev->lock);
    execute(ctx, op);
    pthread_mutex_unlock(&dev->lock);
    free(op->data);
}

void b11_context_replay(B11Context *imm, B11CmdList *list)
{
    B11Device *dev = imm->obj.dev;
    pthread_mutex_lock(&dev->lock);
    g_b11_stats[ST_LISTS]++;
    g_b11_stats[ST_REPLAY_OPS] += list->count;
    for (size_t i = 0; i < list->count; ++i) execute(imm, &list->ops[i]);
    pthread_mutex_unlock(&dev->lock);
}

static void cmdlist_destroy(B11Obj *o)
{
    B11CmdList *l = (B11CmdList *)o;
    for (size_t i = 0; i < l->count; ++i) op_release(&l->ops[i]);
    free(l->ops);
    free(l);
}

/* ---- ID3D11DeviceContext methods ------------------------------------------------------------ */

static void *g_ctx_vtable[115];
static void *vt_cmdlist[8];

static B11HR B11_MS ctx_qi(B11Obj *self, const uint8_t *riid, void **out) { return b11_query_interface(self, riid, out); }
static uint32_t B11_MS ctx_addref(B11Obj *self) { return b11_add_ref(self); }
static uint32_t B11_MS ctx_release(B11Obj *self) { return b11_release(self); }
static void B11_MS ctx_get_device(B11Context *self, void **dev)
{
    if (!dev) return;
    *dev = self->obj.dev;
    b11_add_ref(&self->obj.dev->obj);
}
static B11HR B11_MS ctx_private(void) { return B11_S_OK; }

static B11Op op_new(B11OpType t) { B11Op op; memset(&op, 0, sizeof(op)); op.type = t; return op; }

static void *dup_objs(B11Obj *const *objs, uint32_t n)
{
    B11Obj **copy = calloc(n ? n : 1, sizeof(*copy));
    if (copy && objs) memcpy(copy, objs, (size_t)n * sizeof(*copy));
    return copy;
}

static void stage_set_cb(B11Context *c, uint32_t stage, uint32_t start, uint32_t num, B11Obj *const *objs)
{
    B11Op op = op_new(B11_OP_SET_CB); op.a = stage; op.b = start; op.c = num; op.data = dup_objs(objs, num);
    ctx_emit(c, &op);
}
static void stage_set_srv(B11Context *c, uint32_t stage, uint32_t start, uint32_t num, B11Obj *const *objs)
{
    B11Op op = op_new(B11_OP_SET_SRV); op.a = stage; op.b = start; op.c = num; op.data = dup_objs(objs, num);
    ctx_emit(c, &op);
}
static void stage_set_sampler(B11Context *c, uint32_t stage, uint32_t start, uint32_t num, B11Obj *const *objs)
{
    B11Op op = op_new(B11_OP_SET_SAMPLER); op.a = stage; op.b = start; op.c = num; op.data = dup_objs(objs, num);
    ctx_emit(c, &op);
}
static void stage_set_shader(B11Context *c, uint32_t stage, B11Obj *shader)
{
    if (c->st.shader[stage] == shader) return;       /* redundant set */
    B11Op op = op_new(B11_OP_SET_SHADER); op.a = stage; op.obj[0] = shader;
    ctx_emit(c, &op);
}

#define STAGE_FUNCS(NAME, STAGE) \
static void B11_MS ctx_##NAME##_cb(B11Context *c, uint32_t s, uint32_t n, B11Obj *const *o) { stage_set_cb(c, STAGE, s, n, o); } \
static void B11_MS ctx_##NAME##_srv(B11Context *c, uint32_t s, uint32_t n, B11Obj *const *o) { stage_set_srv(c, STAGE, s, n, o); } \
static void B11_MS ctx_##NAME##_samp(B11Context *c, uint32_t s, uint32_t n, B11Obj *const *o) { stage_set_sampler(c, STAGE, s, n, o); } \
static void B11_MS ctx_##NAME##_shader(B11Context *c, B11Obj *sh, void *ci, uint32_t n) { (void)ci; (void)n; stage_set_shader(c, STAGE, sh); }
STAGE_FUNCS(vs, B11_STAGE_VS)
STAGE_FUNCS(hs, B11_STAGE_HS)
STAGE_FUNCS(ds, B11_STAGE_DS)
STAGE_FUNCS(gs, B11_STAGE_GS)
STAGE_FUNCS(ps, B11_STAGE_PS)
STAGE_FUNCS(cs, B11_STAGE_CS)

static void B11_MS ctx_draw_indexed(B11Context *c, uint32_t count, uint32_t start, int32_t base)
{ B11Op op = op_new(B11_OP_DRAW_INDEXED); op.a = count; op.b = start; op.c = (uint32_t)base; ctx_emit(c, &op); }
static void B11_MS ctx_draw(B11Context *c, uint32_t count, uint32_t start)
{ B11Op op = op_new(B11_OP_DRAW); op.a = count; op.b = start; ctx_emit(c, &op); }
static void B11_MS ctx_draw_indexed_instanced(B11Context *c, uint32_t count, uint32_t inst, uint32_t start, int32_t base, uint32_t sinst)
{ B11Op op = op_new(B11_OP_DRAW_INDEXED_INSTANCED); op.a = count; op.b = inst; op.c = start; op.d = (uint32_t)base; op.e = sinst; ctx_emit(c, &op); }
static void B11_MS ctx_draw_instanced(B11Context *c, uint32_t count, uint32_t inst, uint32_t start, uint32_t sinst)
{ B11Op op = op_new(B11_OP_DRAW_INSTANCED); op.a = count; op.b = inst; op.c = start; op.d = sinst; ctx_emit(c, &op); }
static void B11_MS ctx_dispatch(B11Context *c, uint32_t x, uint32_t y, uint32_t z)
{ B11Op op = op_new(B11_OP_DISPATCH); op.a = x; op.b = y; op.c = z; ctx_emit(c, &op); }

static void B11_MS ctx_ia_layout(B11Context *c, B11Obj *layout)
{
    if (c->st.layout == layout) return;
    B11Op op = op_new(B11_OP_SET_IA_LAYOUT); op.obj[0] = layout; ctx_emit(c, &op);
}
static void B11_MS ctx_ia_vb(B11Context *c, uint32_t start, uint32_t num, B11Obj *const *bufs, const uint32_t *strides, const uint32_t *offsets)
{
    VbEnt *e = calloc(num ? num : 1, sizeof(*e));
    if (!e) return;
    for (uint32_t i = 0; i < num; ++i) {
        e[i].obj = bufs ? bufs[i] : NULL;
        e[i].stride = strides ? strides[i] : 0;
        e[i].offset = offsets ? offsets[i] : 0;
    }
    B11Op op = op_new(B11_OP_SET_IA_VB); op.a = start; op.b = num; op.data = e; ctx_emit(c, &op);
}
static void B11_MS ctx_ia_ib(B11Context *c, B11Obj *buf, uint32_t format, uint32_t offset)
{
    B11Op op = op_new(B11_OP_SET_IA_IB); op.obj[0] = buf; op.a = format; op.b = offset; ctx_emit(c, &op);
}
static void B11_MS ctx_ia_topology(B11Context *c, uint32_t topology)
{
    if (c->st.topology == topology) return;
    B11Op op = op_new(B11_OP_SET_TOPOLOGY); op.a = topology; ctx_emit(c, &op);
}

static void B11_MS ctx_om_rt(B11Context *c, uint32_t num, B11Obj *const *rtvs, B11Obj *dsv)
{
    RtOp *r = calloc(1, sizeof(*r));
    if (!r) return;
    r->rtv_count = num > B11_MAX_RT ? B11_MAX_RT : num;
    for (uint32_t i = 0; i < r->rtv_count; ++i) r->rtv[i] = rtvs ? rtvs[i] : NULL;
    r->dsv = dsv;
    r->uav_start = 0xffffffffu;
    B11Op op = op_new(B11_OP_SET_RT); op.data = r; ctx_emit(c, &op);
}
static void B11_MS ctx_om_rt_uav(B11Context *c, uint32_t num, B11Obj *const *rtvs, B11Obj *dsv,
                                 uint32_t uav_start, uint32_t uav_count, B11Obj *const *uavs, const uint32_t *initial)
{
    RtOp *r = calloc(1, sizeof(*r));
    if (!r) return;
    const int keep_rt = num == 0xffffffffu;
    r->rtv_count = keep_rt ? c->st.rtv_count : (num > B11_MAX_RT ? B11_MAX_RT : num);
    for (uint32_t i = 0; i < r->rtv_count; ++i) r->rtv[i] = keep_rt ? c->st.rtv[i] : (rtvs ? rtvs[i] : NULL);
    r->dsv = keep_rt ? c->st.dsv : dsv;
    r->uav_start = uav_count == 0xffffffffu ? 0xffffffffu : uav_start;
    r->uav_count = uav_count == 0xffffffffu ? 0 : (uav_count > B11_MAX_UAV ? B11_MAX_UAV : uav_count);
    for (uint32_t i = 0; i < r->uav_count; ++i) {
        r->uav[i] = uavs ? uavs[i] : NULL;
        r->uav_initial[i] = initial ? initial[i] : 0xffffffffu;
    }
    B11Op op = op_new(B11_OP_SET_RT); op.data = r; ctx_emit(c, &op);
}
static void B11_MS ctx_om_blend(B11Context *c, B11Obj *state, const float *factor, uint32_t mask)
{
    static const float ones[4] = { 1, 1, 1, 1 };
    if (!factor) factor = ones;
    B11Op op = op_new(B11_OP_SET_BLEND); op.obj[0] = state; op.a = mask;
    memcpy(&op.b, &factor[0], 4); memcpy(&op.c, &factor[1], 4); memcpy(&op.d, &factor[2], 4); memcpy(&op.e, &factor[3], 4);
    ctx_emit(c, &op);
}
static void B11_MS ctx_om_depth(B11Context *c, B11Obj *state, uint32_t ref)
{
    if (c->st.depth == state && c->st.stencil_ref == ref) return;
    B11Op op = op_new(B11_OP_SET_DEPTH); op.obj[0] = state; op.a = ref; ctx_emit(c, &op);
}
static void B11_MS ctx_rs_state(B11Context *c, B11Obj *state)
{
    if (c->st.raster == state) return;
    B11Op op = op_new(B11_OP_SET_RASTER); op.obj[0] = state; ctx_emit(c, &op);
}
static void B11_MS ctx_rs_viewports(B11Context *c, uint32_t num, const float *vp)
{
    B11Op op = op_new(B11_OP_SET_VIEWPORTS); op.a = num;
    if (vp && num) { op.data = malloc((size_t)num * 24); if (op.data) memcpy(op.data, vp, (size_t)num * 24); }
    ctx_emit(c, &op);
}
static void B11_MS ctx_rs_scissors(B11Context *c, uint32_t num, const int32_t *rects)
{
    B11Op op = op_new(B11_OP_SET_SCISSORS); op.a = num;
    if (rects && num) { op.data = malloc((size_t)num * 16); if (op.data) memcpy(op.data, rects, (size_t)num * 16); }
    ctx_emit(c, &op);
}
static void B11_MS ctx_set_predication(B11Context *c, B11Obj *pred, int value)
{
    B11Op op = op_new(B11_OP_SET_PREDICATION); op.obj[0] = pred; op.a = value != 0; ctx_emit(c, &op);
}
static void B11_MS ctx_cs_uav(B11Context *c, uint32_t start, uint32_t num, B11Obj *const *uavs, const uint32_t *initial)
{
    struct { B11Obj *obj; uint32_t initial; } *e = calloc(num ? num : 1, sizeof(*e));
    if (!e) return;
    for (uint32_t i = 0; i < num; ++i) { e[i].obj = uavs ? uavs[i] : NULL; e[i].initial = initial ? initial[i] : 0xffffffffu; }
    B11Op op = op_new(B11_OP_SET_UAV_CS); op.a = start; op.b = num; op.data = e; ctx_emit(c, &op);
}

static void B11_MS ctx_clear_rtv(B11Context *c, B11Obj *rtv, const float *color)
{
    B11Op op = op_new(B11_OP_CLEAR_RTV); op.obj[0] = rtv;
    op.data = malloc(16);
    if (!op.data) return;
    memcpy(op.data, color, 16);
    ctx_emit(c, &op);
}
static void B11_MS ctx_clear_dsv(B11Context *c, B11Obj *dsv, uint32_t flags, float depth, uint8_t stencil)
{
    B11Op op = op_new(B11_OP_CLEAR_DSV); op.obj[0] = dsv; op.a = flags; memcpy(&op.b, &depth, 4); op.c = stencil;
    ctx_emit(c, &op);
}
static void B11_MS ctx_clear_uav(B11Context *c, B11Obj *uav, const uint32_t *values)
{ (void)c; (void)uav; (void)values; }

static void B11_MS ctx_copy_resource(B11Context *c, B11Obj *dst, B11Obj *src)
{
    B11Op op = op_new(B11_OP_COPY_RESOURCE); op.obj[0] = dst; op.obj[1] = src; ctx_emit(c, &op);
}
static void B11_MS ctx_copy_region(B11Context *c, B11Obj *dst, uint32_t dsub, uint32_t dx, uint32_t dy, uint32_t dz,
                                   B11Obj *src, uint32_t ssub, const uint32_t *box)
{
    B11Op op = op_new(B11_OP_COPY_REGION); op.obj[0] = dst; op.obj[1] = src;
    op.a = dsub; op.b = dx; op.c = dy; op.d = dz; op.e = ssub; op.f = box != NULL;
    if (box) { op.data = malloc(24); if (!op.data) return; memcpy(op.data, box, 24); }
    ctx_emit(c, &op);
}
static void B11_MS ctx_update(B11Context *c, B11Obj *dst, uint32_t sub, const uint32_t *box, const void *data,
                              uint32_t row_pitch, uint32_t slice_pitch)
{
    if (!dst || !data) return;
    size_t size = 0;
    if (dst->kind == B11_BUFFER) {
        const B11Buffer *b = (const B11Buffer *)dst;
        size = box ? box[3] - box[0] : b->desc.byte_width;
    } else if (dst->kind == B11_TEXTURE) {
        const B11Texture *t = (const B11Texture *)dst;
        const uint32_t mip = sub % t->desc.mips;
        uint32_t w, h, d;
        texture_extent(t, mip, &w, &h, &d);
        if (box) { h = box[4] - box[1]; d = box[5] - box[2]; }
        if (!slice_pitch) slice_pitch = row_pitch * h;
        const VkFormat f = b11_vk_format(t->desc.format);
        uint32_t bw = 1, bh = 1, bytes = 4;
        if (f != VK_FORMAT_UNDEFINED) bd12_format_block(f, VK_IMAGE_ASPECT_COLOR_BIT, &bw, &bh, &bytes);
        const uint32_t rows = (h + bh - 1) / bh;
        size = (size_t)slice_pitch * (d ? d - 1 : 0) + (size_t)row_pitch * (rows ? rows - 1 : 0) + row_pitch;
        if (!row_pitch) size = 0;
    }
    if (!size) return;
    uint8_t *payload = malloc(24 + size);
    if (!payload) return;
    if (box) memcpy(payload, box, 24);
    memcpy(payload + (box ? 24 : 0), data, size);
    B11Op op = op_new(B11_OP_UPDATE); op.obj[0] = dst; op.a = sub; op.b = row_pitch; op.c = slice_pitch;
    op.d = box != NULL; op.data = payload; op.data_size = size;
    ctx_emit(c, &op);
}
static void B11_MS ctx_generate_mips(B11Context *c, B11Obj *srv)
{
    (void)c; (void)srv;
    static int warned;
    if (!warned++) fprintf(stderr, "[BD11] GenerateMips not implemented yet\n");
}
static void B11_MS ctx_set_min_lod(B11Context *c, B11Obj *res, float lod) { (void)c; (void)res; (void)lod; }
static float B11_MS ctx_get_min_lod(B11Context *c, B11Obj *res) { (void)c; (void)res; return 0.0f; }
static void B11_MS ctx_resolve(B11Context *c, B11Obj *dst, uint32_t dsub, B11Obj *src, uint32_t ssub, uint32_t fmt)
{
    (void)c; (void)dst; (void)dsub; (void)src; (void)ssub; (void)fmt;
    static int warned;
    if (!warned++) fprintf(stderr, "[BD11] ResolveSubresource not implemented yet\n");
}

/* Map / Unmap ------------------------------------------------------------------------------ */

static B11HR B11_MS ctx_map(B11Context *c, B11Obj *res, uint32_t sub, uint32_t type, uint32_t flags, uint8_t *mapped)
{
    (void)flags;
    B11Device *dev = c->obj.dev;
    if (!res || !mapped) return B11_E_INVALIDARG;
    memset(mapped, 0, 16);
    void *data = NULL;
    uint32_t row_pitch = 0, depth_pitch = 0;
    size_t map_size = 0;
    if (res->kind == B11_BUFFER) {
        B11Buffer *b = (B11Buffer *)res;
        map_size = b->desc.byte_width;
        if (c->type == 1) {                        /* deferred: scratch the unmap turns into an op */
            data = calloc(1, map_size);
            if (!data) return B11_E_OUTOFMEMORY;
            if (type != B11_MAP_WRITE_DISCARD) {
                BD12Buffer *bk = b11_buffer_current(b);
                if (bk && bk->cpu) memcpy(data, bk->cpu, map_size);
            }
        } else {
            pthread_mutex_lock(&dev->lock);
            if (b->desc.usage == B11_USAGE_STAGING) {
                if (type & B11_MAP_READ) { b11_flush(dev, 1); }
                data = b11_buffer_current(b)->cpu;
            } else {
                BD12Buffer *bk = b11_buffer_write_backing(b, type == B11_MAP_WRITE_DISCARD);
                data = bk ? bk->cpu : NULL;
            }
            pthread_mutex_unlock(&dev->lock);
            if (!data) return B11_E_FAIL;
        }
        row_pitch = depth_pitch = b->desc.byte_width;
    } else if (res->kind == B11_TEXTURE) {
        B11Texture *t = (B11Texture *)res;
        if (t->staging) {
            if (c->type == 0) {
                pthread_mutex_lock(&dev->lock);
                if ((type & B11_MAP_READ) && t->last_use_serial) { b11_flush(dev, 0); b11_wait_serial(dev, t->last_use_serial); }
                pthread_mutex_unlock(&dev->lock);
            }
            data = (uint8_t *)t->staging->cpu + t->sub_offset[sub];
            row_pitch = t->sub_pitch[sub];
            depth_pitch = t->sub_slice[sub];
        } else {
            static int warned;
            if (!warned++) fprintf(stderr, "[BD11] Map of a GPU-only texture (usage %u) not implemented\n", t->desc.usage);
            return B11_E_FAIL;
        }
    } else return B11_E_INVALIDARG;
    int slot = -1;
    for (int i = 0; i < 64; ++i) if (!c->mapped[i].resource) { slot = i; break; }
    if (slot < 0) { if (c->type == 1 && res->kind == B11_BUFFER) free(data); return B11_E_FAIL; }
    c->mapped[slot].resource = res;
    c->mapped[slot].subresource = sub;
    c->mapped[slot].map_type = type;
    c->mapped[slot].data = data;
    c->mapped[slot].size = map_size;
    memcpy(mapped, &data, 8);
    memcpy(mapped + 8, &row_pitch, 4);
    memcpy(mapped + 12, &depth_pitch, 4);
    return B11_S_OK;
}

static void B11_MS ctx_unmap(B11Context *c, B11Obj *res, uint32_t sub)
{
    for (int i = 0; i < 64; ++i) {
        if (c->mapped[i].resource != res || c->mapped[i].subresource != sub) continue;
        if (c->type == 1 && res->kind == B11_BUFFER && (c->mapped[i].map_type & (B11_MAP_WRITE | B11_MAP_WRITE_DISCARD | B11_MAP_WRITE_NO_OVERWRITE))) {
            B11Buffer *b = (B11Buffer *)res;
            uint8_t *data = c->mapped[i].data;
            size_t first = 0, last = b->desc.byte_width;
            if (c->mapped[i].map_type == B11_MAP_WRITE_DISCARD) {
                while (last > 0 && data[last - 1] == 0) --last;
                last = (last + 15u) & ~(size_t)15u;
                if (last > b->desc.byte_width) last = b->desc.byte_width;
            } else {
                BD12Buffer *bk = b11_buffer_current(b);
                if (bk && bk->cpu) {
                    const uint8_t *base = bk->cpu;
                    while (first < last && data[first] == base[first]) ++first;
                    while (last > first && data[last - 1] == base[last - 1]) --last;
                }
            }
            if (last > first) {
                B11Op op = op_new(B11_OP_BUFFER_WRITE);
                op.obj[0] = res;
                op.a = c->mapped[i].map_type == B11_MAP_WRITE_DISCARD;
                op.b = (uint32_t)first;
                op.data_size = last - first;
                op.data = malloc(op.data_size);
                if (op.data) { memcpy(op.data, data + first, op.data_size); ctx_emit(c, &op); }
            }
            free(data);
        }
        memset(&c->mapped[i], 0, sizeof(c->mapped[i]));
        return;
    }
}

/* Queries (M1: results are immediately available) ---------------------------------------------- */

static void B11_MS ctx_begin(B11Context *c, B11Obj *q) { (void)c; if (q) ((B11Query *)q)->issued = 1; }
static void B11_MS ctx_end(B11Context *c, B11Obj *q)
{
    (void)c;
    if (!q) return;
    B11Query *query = (B11Query *)q;
    query->issued = 1;
    query->ended = 1;
}
static B11HR B11_MS ctx_get_data(B11Context *c, B11Obj *q, void *data, uint32_t size, uint32_t flags)
{
    (void)c; (void)flags;
    if (!q) return B11_E_INVALIDARG;
    B11Query *query = (B11Query *)q;
    if (!query->ended) return B11_S_FALSE;
    if (!data || !size) return B11_S_OK;
    memset(data, 0, size);
    switch (query->type) {
    case 0: *(uint32_t *)data = 1; break;                                    /* EVENT: done */
    case 1: *(uint64_t *)data = 1; break;                                    /* OCCLUSION: visible */
    case 2: { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
              *(uint64_t *)data = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; break; }
    case 3: if (size >= 16) { *(uint64_t *)data = 1000000000ull; ((uint32_t *)data)[2] = 0; } break;  /* DISJOINT */
    case 5: *(uint32_t *)data = 1; break;                                    /* OCCLUSION_PREDICATE */
    default: break;
    }
    return B11_S_OK;
}

/* Misc ---------------------------------------------------------------------------------- */

static void B11_MS ctx_clear_state(B11Context *c) { B11Op op = op_new(B11_OP_CLEAR_STATE); ctx_emit(c, &op); }
static void B11_MS ctx_flush(B11Context *c)
{
    if (c->type == 0) b11_flush(c->obj.dev, 0);
}
static uint32_t B11_MS ctx_get_type(B11Context *c) { return c->type; }
static uint32_t B11_MS ctx_get_flags(B11Context *c) { return c->flags; }

static void B11_MS ctx_execute_list(B11Context *c, B11Obj *list, uint32_t restore)
{
    if (!list || list->kind != B11_CMDLIST || c->type != 0) return;
    B11CmdList *l = (B11CmdList *)list;
    if (!restore) b11_state_clear(c);
    b11_context_replay(c, l);
    if (!restore) b11_state_clear(c);
}

static B11HR B11_MS ctx_finish_list(B11Context *c, uint32_t restore, void **out)
{
    if (c->type != 1) return B11_E_FAIL;
    B11CmdList *l = b11_obj_new(c->obj.dev, B11_CMDLIST, sizeof(*l), vt_cmdlist, cmdlist_destroy);
    if (!l) return B11_E_OUTOFMEMORY;
    l->ops = c->ops;
    l->count = c->op_count;
    l->restore_state = restore;
    c->ops = NULL; c->op_count = c->op_capacity = 0;
    if (!restore) b11_state_clear(c);
    for (int i = 0; i < 64; ++i) {                       /* unmatched deferred maps are dropped */
        if (c->mapped[i].resource && c->mapped[i].resource->kind == B11_BUFFER && c->type == 1) free(c->mapped[i].data);
        memset(&c->mapped[i], 0, sizeof(c->mapped[i]));
    }
    if (out) *out = l; else b11_release(&l->obj);
    return B11_S_OK;
}

static void B11_MS ctx_get_om_rt(B11Context *c, uint32_t num, B11Obj **rtvs, B11Obj **dsv)
{
    for (uint32_t i = 0; rtvs && i < num; ++i) {
        rtvs[i] = i < B11_MAX_RT ? c->st.rtv[i] : NULL;
        b11_retain(rtvs[i]);
    }
    if (dsv) { *dsv = c->st.dsv; b11_retain(*dsv); }
}
static void B11_MS ctx_get_noop(void) {}

static void ctx_destroy(B11Obj *o) { (void)o; }

B11Context *b11_context_create(B11Device *dev, uint32_t type, uint32_t flags)
{
    b11_init_vtables();
    B11Context *c = b11_obj_new(dev, B11_CONTEXT, sizeof(*c), g_ctx_vtable, ctx_destroy);
    if (!c) return NULL;
    c->type = type;
    c->flags = flags;
    b11_state_clear(c);
    return c;
}

void b11_context_init_vtable(void)
{
    void **v = g_ctx_vtable;
    for (int i = 0; i < 115; ++i) v[i] = (void *)ctx_get_noop;
    v[0] = (void *)ctx_qi; v[1] = (void *)ctx_addref; v[2] = (void *)ctx_release;
    v[3] = (void *)ctx_get_device; v[4] = (void *)ctx_private; v[5] = (void *)ctx_private; v[6] = (void *)ctx_private;
    v[7] = (void *)ctx_vs_cb; v[8] = (void *)ctx_ps_srv; v[9] = (void *)ctx_ps_shader; v[10] = (void *)ctx_ps_samp;
    v[11] = (void *)ctx_vs_shader; v[12] = (void *)ctx_draw_indexed; v[13] = (void *)ctx_draw;
    v[14] = (void *)ctx_map; v[15] = (void *)ctx_unmap; v[16] = (void *)ctx_ps_cb;
    v[17] = (void *)ctx_ia_layout; v[18] = (void *)ctx_ia_vb; v[19] = (void *)ctx_ia_ib;
    v[20] = (void *)ctx_draw_indexed_instanced; v[21] = (void *)ctx_draw_instanced;
    v[22] = (void *)ctx_gs_cb; v[23] = (void *)ctx_gs_shader; v[24] = (void *)ctx_ia_topology;
    v[25] = (void *)ctx_vs_srv; v[26] = (void *)ctx_vs_samp;
    v[27] = (void *)ctx_begin; v[28] = (void *)ctx_end; v[29] = (void *)ctx_get_data; v[30] = (void *)ctx_set_predication;
    v[31] = (void *)ctx_gs_srv; v[32] = (void *)ctx_gs_samp;
    v[33] = (void *)ctx_om_rt; v[34] = (void *)ctx_om_rt_uav; v[35] = (void *)ctx_om_blend; v[36] = (void *)ctx_om_depth;
    v[41] = (void *)ctx_dispatch;
    v[43] = (void *)ctx_rs_state; v[44] = (void *)ctx_rs_viewports; v[45] = (void *)ctx_rs_scissors;
    v[46] = (void *)ctx_copy_region; v[47] = (void *)ctx_copy_resource; v[48] = (void *)ctx_update;
    v[50] = (void *)ctx_clear_rtv; v[51] = (void *)ctx_clear_uav; v[52] = (void *)ctx_clear_uav;
    v[53] = (void *)ctx_clear_dsv; v[54] = (void *)ctx_generate_mips; v[55] = (void *)ctx_set_min_lod;
    v[56] = (void *)ctx_get_min_lod; v[57] = (void *)ctx_resolve; v[58] = (void *)ctx_execute_list;
    v[59] = (void *)ctx_hs_srv; v[60] = (void *)ctx_hs_shader; v[61] = (void *)ctx_hs_samp; v[62] = (void *)ctx_hs_cb;
    v[63] = (void *)ctx_ds_srv; v[64] = (void *)ctx_ds_shader; v[65] = (void *)ctx_ds_samp; v[66] = (void *)ctx_ds_cb;
    v[67] = (void *)ctx_cs_srv; v[68] = (void *)ctx_cs_uav; v[69] = (void *)ctx_cs_shader; v[70] = (void *)ctx_cs_samp;
    v[71] = (void *)ctx_cs_cb;
    v[89] = (void *)ctx_get_om_rt;
    v[110] = (void *)ctx_clear_state; v[111] = (void *)ctx_flush; v[112] = (void *)ctx_get_type;
    v[113] = (void *)ctx_get_flags; v[114] = (void *)ctx_finish_list;
    /* ID3D11CommandList */
    vt_cmdlist[0] = (void *)ctx_qi; vt_cmdlist[1] = (void *)ctx_addref; vt_cmdlist[2] = (void *)ctx_release;
    vt_cmdlist[3] = (void *)ctx_get_device; vt_cmdlist[4] = (void *)ctx_private; vt_cmdlist[5] = (void *)ctx_private;
    vt_cmdlist[6] = (void *)ctx_private; vt_cmdlist[7] = (void *)ctx_get_flags;
}
