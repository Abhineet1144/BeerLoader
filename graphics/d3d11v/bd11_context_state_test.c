#define _GNU_SOURCE
#include "bd11_context_state.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* CPU-only: real base Win64 setters, slot58 ExecuteCommandList, slot114
 * FinishCommandList and slot89 OMGetRenderTargets. No Vulkan device is made. */
int (*g_beer_lazy_fault_hook)(void *);
#define SLOT(o, n, type) ((type)(*(void ***)(o))[n])
typedef void (B11_MS *ArrayFn)(B11Context *, uint32_t, uint32_t, B11Obj *const *);
typedef void (B11_MS *ShaderFn)(B11Context *, B11Obj *, void *, uint32_t);
typedef void (B11_MS *ObjFn)(B11Context *, B11Obj *);
typedef void (B11_MS *TopologyFn)(B11Context *, uint32_t);
typedef void (B11_MS *VbFn)(B11Context *, uint32_t, uint32_t, B11Obj *const *, const uint32_t *, const uint32_t *);
typedef void (B11_MS *IbFn)(B11Context *, B11Obj *, uint32_t, uint32_t);
typedef void (B11_MS *RtFn)(B11Context *, uint32_t, B11Obj *const *, B11Obj *);
typedef void (B11_MS *GetRtFn)(B11Context *, uint32_t, B11Obj **, B11Obj **);
typedef void (B11_MS *BlendFn)(B11Context *, B11Obj *, const float *, uint32_t);
typedef void (B11_MS *DepthFn)(B11Context *, B11Obj *, uint32_t);
typedef void (B11_MS *RectsFn)(B11Context *, uint32_t, const void *);
typedef void (B11_MS *UavFn)(B11Context *, uint32_t, uint32_t, B11Obj *const *, const uint32_t *);
typedef void (B11_MS *PredFn)(B11Context *, B11Obj *, uint32_t);
typedef void (B11_MS *ExecuteFn)(B11Context *, B11Obj *, uint32_t);
typedef B11HR (B11_MS *FinishFn)(B11Context *, uint32_t, void **);

static int fail_bank_alloc, destroyed, device_destroyed, replay_checks, replay_complete;
void *__real_calloc(size_t, size_t);
void *__wrap_calloc(size_t n, size_t size)
{
    return fail_bank_alloc && n == 1 && size == sizeof(B11ContextState) ? NULL : __real_calloc(n, size);
}
static void object_destroy(B11Obj *o) { ++destroyed; free(o); }
static void view_destroy(B11Obj *o)
{
    b11_release(((B11View *)o)->resource);
    ++destroyed;
    free(o);
}
static void device_destroy(B11Obj *o) { (void)o; ++device_destroyed; }
static void device_init(B11Device *d)
{
    memset(d, 0, sizeof(*d));
    pthread_mutexattr_t attr;
    assert(!pthread_mutexattr_init(&attr));
    assert(!pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE));
    assert(!pthread_mutex_init(&d->lock, &attr));
    assert(!pthread_mutexattr_destroy(&attr));
    atomic_init(&d->obj.refs, 1);
    d->obj.kind = B11_DEVICE; d->obj.dev = d; d->obj.destroy = device_destroy;
    d->feature_level = 0xb000;
    d->cmd_generation = 45;
}
static void bind(B11Obj **slot, B11Obj *o) { *slot = o; b11_retain(o); }

/* All owned slots populated, with alias duplication across arrays and scalar
 * bindings. Distinct numeric patterns catch partial/field-by-field copies. */
static void fill(B11PipelineState *s, B11Obj *o, B11View *v)
{
    b11_pipeline_state_reset(s);
    for (int st = 0; st < B11_STAGE_COUNT; ++st) {
        bind(&s->shader[st], o);
        for (int i = 0; i < B11_MAX_CBV; ++i) {
            bind(&s->cb[st][i], o);
            s->cb_first[st][i] = 16 * (i + st);
            s->cb_count[st][i] = 16 * (14 - i);
        }
        for (int i = 0; i < B11_MAX_SRV; ++i) {
            bind(&s->srv[st][i], &v->obj);
            s->srv_res[st][i] = v->resource;
        }
        s->srv_end[st] = B11_MAX_SRV;
        for (int i = 0; i < B11_MAX_SAMPLER; ++i) bind(&s->sampler[st][i], o);
        for (int i = 0; i < B11_MAX_UAV; ++i) {
            bind(&s->uav[st][i], &v->obj);
            s->uav_initial[st][i] = 100 * st + i;
        }
    }
    for (int i = 0; i < B11_MAX_VB; ++i) {
        bind(&s->vb[i], o); s->vb_stride[i] = i + 4; s->vb_offset[i] = i * 32;
    }
    for (int i = 0; i < B11_MAX_RT; ++i) bind(&s->rtv[i], &v->obj);
    bind(&s->ib, o); bind(&s->layout, o); bind(&s->dsv, &v->obj);
    bind(&s->blend, o); bind(&s->depth, o); bind(&s->raster, o); bind(&s->predicate, o);
    s->ib_format = 42; s->ib_offset = 96; s->topology = 4;
    s->rtv_count = 8; s->stencil_ref = 79; s->sample_mask = 0x12345678;
    s->predicate_value = 1; s->viewport_count = s->scissor_count = 16;
    for (int i = 0; i < 4; ++i) s->blend_factor[i] = .25f * i;
    for (int i = 0; i < B11_MAX_VIEWPORTS; ++i) {
        for (int j = 0; j < 6; ++j) s->viewports[i][j] = (float)(i * 6 + j);
        for (int j = 0; j < 4; ++j) s->scissors[i][j] = i * 4 + j;
    }
}
static void defaults(const B11PipelineState *s)
{
    B11PipelineState def; b11_pipeline_state_init(&def);
    assert(!memcmp(s, &def, sizeof(def)));
}
static void invalidated(B11Context *c)
{
    assert(c->dirty == B11_DIRTY_ALL && !c->rendering_valid);
    assert(c->generation != c->obj.dev->cmd_generation && !c->bound_ib_va);
    for (int i = 0; i < B11_MAX_VB; ++i) assert(!c->bound_vb_va[i]);
}
static void poison_cache(B11Context *c)
{
    c->dirty = 0; c->rendering_valid = 1; c->generation = c->obj.dev->cmd_generation;
    c->bound_ib_va = 123;
    for (int i = 0; i < B11_MAX_VB; ++i) c->bound_vb_va[i] = 321;
}

/* Observe the first real setter's retain. Unlike wrapping replay (defined and
 * called in the same translation unit), this external reference is interposed
 * by the linker. The recorded object is absent from the saved original state. */
static B11Context *observed_context;
static B11Obj *recorded_object;
static B11View *recorded_srv, *recorded_target;
static int observe_first, observing;
void __real_b11_retain(B11Obj *);
void __wrap_b11_retain(B11Obj *o)
{
    if (observe_first && o == recorded_object) {
        defaults(&observed_context->st);
        invalidated(observed_context);
        observe_first = 0;
        ++replay_checks;
        poison_cache(observed_context); /* restoration must invalidate these */
    }
    if (observing && o == recorded_object &&
        observed_context->st.uav[B11_STAGE_CS][7] == &recorded_target->obj) {
        /* The final predication setter retains recorded_object. All preceding
         * setters have run now: validate the real replay, not just recording. */
        B11PipelineState *s = &observed_context->st;
        for (int stage = 0; stage < B11_STAGE_COUNT; ++stage) {
            assert(s->shader[stage] == recorded_object);
            assert(s->cb[stage][13] == recorded_object);
            assert(s->sampler[stage][15] == recorded_object);
            assert(s->srv[stage][127] == &recorded_srv->obj);
            assert(s->srv_res[stage][127] == recorded_srv->resource && s->srv_end[stage] == 128);
            assert(!s->cb_first[stage][13] && !s->cb_count[stage][13]);
        }
        assert(s->vb[15] == recorded_object && s->vb_stride[15] == 24 && s->vb_offset[15] == 48);
        assert(s->ib == recorded_object && s->ib_format == 42 && s->ib_offset == 64);
        assert(s->layout == recorded_object && s->topology == 3);
        assert(s->rtv_count == 1 && s->rtv[0] == &recorded_target->obj && s->dsv == &recorded_target->obj);
        assert(s->blend == recorded_object && s->sample_mask == 0xabcdef);
        assert(s->depth == recorded_object && s->stencil_ref == 17 && s->raster == recorded_object);
        assert(s->viewport_count == 1 && s->viewports[0][2] == 640);
        assert(s->scissor_count == 1 && s->scissors[0][2] == 643);
        assert(s->uav_initial[B11_STAGE_CS][7] == UINT32_MAX);
        observing = 0;
        ++replay_complete;
    }
    __real_b11_retain(o);
}
static void execute_list(B11Context *c, B11CmdList *list, uint32_t restore)
{
    observed_context = c;
    observe_first = observing = 1;
    SLOT(c, 58, ExecuteFn)(c, &list->obj, restore);
    assert(!observe_first && !observing);
}

static B11CmdList *record(B11Context *d, B11Obj *o, B11View *srv, B11View *rt)
{
    static const int cb[] = {7,62,66,22,16,71};
    static const int sr[] = {25,59,63,31,8,67};
    static const int sa[] = {26,61,65,32,10,70};
    static const int sh[] = {11,60,64,23,9,69};
    for (int st = 0; st < B11_STAGE_COUNT; ++st) {
        SLOT(d, cb[st], ArrayFn)(d, 13, 1, &o);
        B11Obj *view = &srv->obj;
        SLOT(d, sr[st], ArrayFn)(d, 127, 1, &view);
        SLOT(d, sa[st], ArrayFn)(d, 15, 1, &o);
        SLOT(d, sh[st], ShaderFn)(d, o, NULL, 0);
    }
    SLOT(d, 17, ObjFn)(d, o);
    uint32_t stride = 24, offset = 48;
    SLOT(d, 18, VbFn)(d, 15, 1, &o, &stride, &offset);
    SLOT(d, 19, IbFn)(d, o, 42, 64);
    SLOT(d, 24, TopologyFn)(d, 3);
    B11Obj *target = &rt->obj;
    SLOT(d, 33, RtFn)(d, 1, &target, &rt->obj);
    float factors[] = {.2f,.4f,.6f,.8f};
    SLOT(d, 35, BlendFn)(d, o, factors, 0xabcdef);
    SLOT(d, 36, DepthFn)(d, o, 17);
    SLOT(d, 43, ObjFn)(d, o);
    float vp[6] = {3,4,640,480,0,1}; int32_t sc[4] = {3,4,643,484};
    SLOT(d, 44, RectsFn)(d, 1, vp); SLOT(d, 45, RectsFn)(d, 1, sc);
    B11Obj *uav = &rt->obj; uint32_t initial = UINT32_MAX;
    SLOT(d, 68, UavFn)(d, 7, 1, &uav, &initial);
    SLOT(d, 30, PredFn)(d, o, 1);
    B11PipelineState saved;
    b11_pipeline_state_capture(&saved, &d->st);
    void *list = NULL;
    assert(SLOT(d, 114, FinishFn)(d, 1, &list) == B11_S_OK && list);
    assert(!memcmp(&saved, &d->st, sizeof(saved)) && !d->op_count);
    b11_pipeline_state_dispose(&saved);
    /* FALSE also returns a real empty list, resetting deferred state. */
    void *empty = NULL;
    assert(SLOT(d, 114, FinishFn)(d, 0, &empty) == B11_S_OK && empty);
    defaults(&d->st); b11_release(empty);
    return list;
}

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cv;
    int ready, done, swap;
    B11Context *ctx;
    B11ContextState *state, *previous;
    B11CmdList *list;
} Work;
static struct timespec deadline(int ms)
{
    struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC, &t));
    t.tv_nsec += ms * 1000000L; t.tv_sec += t.tv_nsec / 1000000000L;
    t.tv_nsec %= 1000000000L; return t;
}
static int wait_flag(Work *w, int *flag, int ms)
{
    struct timespec t = deadline(ms);
    assert(!pthread_mutex_lock(&w->lock));
    int e = 0;
    while (!*flag && !e) e = pthread_cond_timedwait(&w->cv, &w->lock, &t);
    assert(!e || e == ETIMEDOUT);
    int value = *flag; assert(!pthread_mutex_unlock(&w->lock)); return value;
}
static void signal_flag(Work *w, int *flag)
{
    assert(!pthread_mutex_lock(&w->lock)); *flag = 1;
    assert(!pthread_cond_broadcast(&w->cv)); assert(!pthread_mutex_unlock(&w->lock));
}
static void *worker(void *arg)
{
    Work *w = arg; signal_flag(w, &w->ready);
    if (w->swap) assert(b11_context_state_swap(w->ctx, w->state, &w->previous) == B11_S_OK);
    else execute_list(w->ctx, w->list, 1);
    signal_flag(w, &w->done); return NULL;
}
static void protected_change(B11Context *c, B11CmdList *list, B11ContextState *bank)
{
    Work w = { .ctx = c, .list = list, .state = bank, .swap = bank != NULL };
    assert(!pthread_mutex_init(&w.lock, NULL));
    pthread_condattr_t attr; assert(!pthread_condattr_init(&attr));
    assert(!pthread_condattr_setclock(&attr, CLOCK_MONOTONIC));
    assert(!pthread_cond_init(&w.cv, &attr)); assert(!pthread_condattr_destroy(&attr));
    atomic_store(&c->multithread_protected, 1);
    assert(!pthread_mutex_lock(&c->obj.dev->lock));
    assert(!pthread_mutex_lock(&c->obj.dev->lock)); /* recursive Enter equivalent */
    pthread_t thread; assert(!pthread_create(&thread, NULL, worker, &w));
    assert(wait_flag(&w, &w.ready, 2000)); assert(!wait_flag(&w, &w.done, 100));
    SLOT(c, 24, TopologyFn)(c, 5); /* must not restore a pre-lock stale snapshot */
    assert(!pthread_mutex_unlock(&c->obj.dev->lock));
    assert(!wait_flag(&w, &w.done, 100));
    assert(!pthread_mutex_unlock(&c->obj.dev->lock));
    assert(wait_flag(&w, &w.done, 2000)); assert(!pthread_join(thread, NULL));
    if (bank) {
        assert(w.previous->st.topology == 5);
        assert(b11_context_state_swap(c, w.previous, NULL) == B11_S_OK);
        b11_context_state_release(w.previous);
    }
    assert(c->st.topology == 5);
    assert(!pthread_cond_destroy(&w.cv)); assert(!pthread_mutex_destroy(&w.lock));
}

int main(void)
{
    b11_init_vtables();
    B11Device dev, foreign; device_init(&dev); device_init(&foreign);
    B11Context *c = b11_context_create(&dev, 0, 0), *d = b11_context_create(&dev, 1, 0);
    assert(c && d);
    B11Obj *o = b11_obj_new(&dev, B11_BUFFER, sizeof(B11Obj), NULL, object_destroy);
    B11Obj *r = b11_obj_new(&dev, B11_BUFFER, sizeof(B11Obj), NULL, object_destroy);
    B11View *view = b11_obj_new(&dev, B11_SRV, sizeof(B11View), NULL, view_destroy);
    B11View *target = b11_obj_new(&dev, B11_RTV, sizeof(B11View), NULL, view_destroy);
    assert(o && r && view && target);
    view->resource = o; b11_retain(o); target->resource = r; b11_retain(r);

    B11PipelineState a, b, original;
    b11_pipeline_state_init(&a); b11_pipeline_state_init(&b);
    fill(&a, o, view);
    uint32_t refs_o = atomic_load(&o->refs), refs_v = atomic_load(&view->obj.refs);
    assert(refs_v == 1 + 6 * (128 + 8) + 8 + 1);
    assert(refs_o == 2 + 6 * (1 + 14 + 16) + 16 + 6);
    b11_pipeline_state_copy(&b, &a);
    assert(atomic_load(&view->obj.refs) == 2 * refs_v - 1);
    assert(atomic_load(&o->refs) == 2 * refs_o - 2); /* no srv_res retain */
    b11_pipeline_state_copy(&b, &b); assert(!memcmp(&a, &b, sizeof(a)));
    b11_pipeline_state_copy(&a, &b); assert(!memcmp(&a, &b, sizeof(a)));
    /* Hidden high slots still release even with an empty scan bound. */
    memset(a.srv_end, 0, sizeof(a.srv_end)); a.rtv_count = 0;
    b11_pipeline_state_dispose(&a); b11_pipeline_state_dispose(&a);
    b11_pipeline_state_reset(&b); defaults(&b);
    assert(atomic_load(&o->refs) == 2 && atomic_load(&view->obj.refs) == 1);

    fill(&c->st, o, view);
    b11_pipeline_state_capture(&original, &c->st);
    recorded_object = r; recorded_srv = view; recorded_target = target;
    B11CmdList *list = record(d, r, view, target);
    refs_o = atomic_load(&o->refs); refs_v = atomic_load(&view->obj.refs);
    poison_cache(c);
    execute_list(c, list, 1);
    assert(!memcmp(&original, &c->st, sizeof(original))); invalidated(c);
    assert(atomic_load(&o->refs) == refs_o && atomic_load(&view->obj.refs) == refs_v);
    B11Obj *rtvs[8], *dsv;
    SLOT(c, 89, GetRtFn)(c, 8, rtvs, &dsv);
    for (int i = 0; i < 8; ++i) { assert(rtvs[i] == &view->obj); b11_release(rtvs[i]); }
    assert(dsv == &view->obj); b11_release(dsv);
    execute_list(c, list, 0); defaults(&c->st); invalidated(c);
    execute_list(c, list, 1); defaults(&c->st); invalidated(c);
    /* Foreign and deferred executions rejected without touching state. */
    list->obj.dev = &foreign;
    SLOT(c, 58, ExecuteFn)(c, &list->obj, 1); defaults(&c->st);
    list->obj.dev = &dev;
    SLOT(d, 58, ExecuteFn)(d, &list->obj, 0); defaults(&d->st);
    b11_pipeline_state_copy(&c->st, &original);

    B11ContextState *bank = NULL, *other = NULL, *prev = (void *)1;
    assert(b11_context_state_create(&dev, 0xa000, &bank) == B11_E_INVALIDARG && !bank);
    assert(b11_context_state_create(&dev, 0xc000, &bank) == B11_E_INVALIDARG && !bank);
    assert(b11_context_state_create(&dev, dev.feature_level, NULL) == B11_E_INVALIDARG);
    uint32_t dev_refs = atomic_load(&dev.obj.refs);
    fail_bank_alloc = 1;
    assert(b11_context_state_create(&dev, dev.feature_level, &bank) == B11_E_OUTOFMEMORY && !bank);
    assert(atomic_load(&dev.obj.refs) == dev_refs);
    fail_bank_alloc = 0;
    assert(b11_context_state_create(&dev, dev.feature_level, &bank) == B11_S_OK);
    assert(b11_context_state_create(&foreign, foreign.feature_level, &other) == B11_S_OK);
    assert(b11_context_state_swap(c, NULL, &prev) == B11_S_OK && prev == (void *)1);
    assert(b11_context_state_swap(d, bank, &prev) == B11_E_INVALIDARG && prev == (void *)1);
    assert(b11_context_state_swap(c, other, &prev) == B11_E_INVALIDARG && prev == (void *)1);
    fail_bank_alloc = 1; poison_cache(c);
    B11Context before_failure = *c;
    dev_refs = atomic_load(&dev.obj.refs);
    uint32_t bank_refs = atomic_load(&bank->refs);
    assert(b11_context_state_swap(c, bank, &prev) == B11_E_OUTOFMEMORY && prev == (void *)1);
    assert(!memcmp(c, &before_failure, sizeof(*c)));
    assert(atomic_load(&dev.obj.refs) == dev_refs && atomic_load(&bank->refs) == bank_refs);
    assert(!c->active_state && !c->default_state && !memcmp(&c->st, &original, sizeof(original)));
    assert(!c->dirty && c->rendering_valid); fail_bank_alloc = 0;

    B11Query query = { .obj = { .refs = 1, .kind = B11_QUERY, .dev = &dev },
                       .value = 1234, .submit_serial = 91 };
    typedef void (B11_MS *QueryFn)(B11Context *, B11Obj *);
    SLOT(c, 27, QueryFn)(c, &query.obj);
    SLOT(c, 30, PredFn)(c, &query.obj, 1);
    b11_pipeline_state_dispose(&original);
    b11_pipeline_state_capture(&original, &c->st);
    c->mapped[0].resource = o; c->mapped[0].data = (void *)0x1234;
    c->pending_serial = 99; c->draw_count = 10;
    assert(b11_context_state_swap(c, bank, &prev) == B11_S_OK);
    assert(prev == c->default_state && !memcmp(&prev->st, &original, sizeof(original)));
    defaults(&c->st); invalidated(c);
    assert(query.issued && !query.ended && query.value == 1234 && query.submit_serial == 91);
    assert(c->mapped[0].resource == o && c->mapped[0].data == (void *)0x1234);
    assert(c->pending_serial == 99 && c->draw_count == 10);
    SLOT(c, 24, TopologyFn)(c, 2);
    B11ContextState *same = NULL; poison_cache(c);
    assert(b11_context_state_swap(c, bank, &same) == B11_S_OK && same == bank);
    assert(c->st.topology == 2 && !c->dirty && c->rendering_valid);
    b11_context_state_release(same);
    assert(b11_context_state_swap(c, prev, NULL) == B11_S_OK);
    assert(bank->st.topology == 2 && !memcmp(&c->st, &original, sizeof(original)));
    SLOT(c, 24, TopologyFn)(c, 6);
    assert(b11_context_state_swap(c, bank, NULL) == B11_S_OK && c->st.topology == 2);
    assert(prev->st.topology == 6);
    execute_list(c, list, 1); assert(c->st.topology == 2);
    execute_list(c, list, 0); defaults(&c->st);
    assert(b11_context_state_swap(c, prev, NULL) == B11_S_OK && c->st.topology == 6);
    defaults(&bank->st); /* FALSE changes remembered live state on switch-out */
    protected_change(c, list, NULL);
    protected_change(c, list, bank);

    b11_pipeline_state_dispose(&original);
    b11_release(&list->obj);
    b11_context_state_release(other); b11_context_state_release(prev);
    /* External ownership gone: live/default/inactive banks keep bindings alive. */
    b11_release(o); b11_release(r); b11_release(&view->obj); b11_release(&target->obj);
    assert(destroyed == 2); /* target + its resource, no longer bound */
    b11_release(&d->obj);
    b11_context_state_release(bank);
    b11_release(&c->obj);
    assert(destroyed == 4 && atomic_load(&dev.obj.refs) == 1);
    assert(atomic_load(&foreign.obj.refs) == 1 && !device_destroyed);
    assert(atomic_load(&query.obj.refs) == 1);
    assert(replay_checks == 6 && replay_complete == 6);
    assert(!pthread_mutex_destroy(&dev.lock)); assert(!pthread_mutex_destroy(&foreign.lock));
    puts("BD11 owned pipeline state, Win64 command-list restore, mutable banks, failure atomicity and recursive protection: PASS");
    return 0;
}