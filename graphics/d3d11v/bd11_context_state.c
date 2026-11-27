#include "bd11_context_state.h"
#include <stdlib.h>
#include <string.h>

/* Keep the owned-pointer inventory in one place. Never walk srv_res, and never
 * limit ownership walks to srv_end/rtv_count: all represented slots are owned. */
static void state_refs(B11PipelineState *s, int retain)
{
#define REF(p) do { if (retain) b11_retain(p); else if (p) b11_release(p); } while (0)
    for (int stage = 0; stage < B11_STAGE_COUNT; ++stage) {
        REF(s->shader[stage]);
        for (int i = 0; i < B11_MAX_CBV; ++i) REF(s->cb[stage][i]);
        for (int i = 0; i < B11_MAX_SRV; ++i) REF(s->srv[stage][i]);
        for (int i = 0; i < B11_MAX_SAMPLER; ++i) REF(s->sampler[stage][i]);
        for (int i = 0; i < B11_MAX_UAV; ++i) REF(s->uav[stage][i]);
    }
    for (int i = 0; i < B11_MAX_VB; ++i) REF(s->vb[i]);
    for (int i = 0; i < B11_MAX_RT; ++i) REF(s->rtv[i]);
    REF(s->ib); REF(s->layout); REF(s->dsv);
    REF(s->blend); REF(s->depth); REF(s->raster); REF(s->predicate);
#undef REF
}

void b11_pipeline_state_init(B11PipelineState *s)
{
    memset(s, 0, sizeof(*s));
    for (int i = 0; i < 4; ++i) s->blend_factor[i] = 1.0f;
    s->sample_mask = UINT32_MAX;
}

void b11_pipeline_state_capture(B11PipelineState *dst, const B11PipelineState *src)
{
    if (dst == src) return;
    memcpy(dst, src, sizeof(*dst));
    state_refs(dst, 1);
}

void b11_pipeline_state_dispose(B11PipelineState *s)
{
    /* Detach first, including for recursive destruction callbacks. */
    B11PipelineState old = *s;
    memset(s, 0, sizeof(*s));
    state_refs(&old, 0);
}

void b11_pipeline_state_copy(B11PipelineState *dst, const B11PipelineState *src)
{
    if (dst == src) return;
    B11PipelineState copy;
    b11_pipeline_state_capture(&copy, src); /* retain before releasing aliases */
    b11_pipeline_state_dispose(dst);
    *dst = copy;
}

void b11_pipeline_state_reset(B11PipelineState *s)
{
    b11_pipeline_state_dispose(s);
    b11_pipeline_state_init(s);
}

void b11_context_state_invalidate(B11Context *ctx)
{
    ctx->dirty = B11_DIRTY_ALL;
    ctx->generation = ctx->obj.dev->cmd_generation - 1; /* guaranteed mismatch, including wrap */
    ctx->rendering_valid = 0;
    memset(ctx->bound_vb_va, 0, sizeof(ctx->bound_vb_va));
    ctx->bound_ib_va = 0;
}

B11HR b11_context_state_create(B11Device *dev, uint32_t level, B11ContextState **out)
{
    if (!out) return B11_E_INVALIDARG;
    *out = NULL;
    if (!dev || !dev->feature_level || level != dev->feature_level) return B11_E_INVALIDARG;
    B11ContextState *s = calloc(1, sizeof(*s));
    if (!s) return B11_E_OUTOFMEMORY;
    atomic_init(&s->refs, 1);
    s->dev = dev;
    s->feature_level = level;
    b11_retain(&dev->obj);
    b11_pipeline_state_init(&s->st);
    *out = s;
    return B11_S_OK;
}

void b11_context_state_retain(B11ContextState *s)
{
    if (s) atomic_fetch_add(&s->refs, 1);
}

void b11_context_state_release(B11ContextState *s)
{
    if (!s || atomic_fetch_sub(&s->refs, 1) != 1) return;
    B11Device *dev = s->dev;
    b11_pipeline_state_dispose(&s->st);
    free(s);
    b11_release(&dev->obj);
}