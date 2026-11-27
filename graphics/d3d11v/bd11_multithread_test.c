#define _GNU_SOURCE
#include "bd11.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* CPU-only contract fixture: real BD11 COM entry points and context state,
 * without a GPU, loader, Java assets or a program-specific graphics path. */
int (*g_beer_lazy_fault_hook)(void *);
typedef B11HR (B11_MS *QIFn)(void *, const uint8_t *, void **);
typedef uint32_t (B11_MS *RefFn)(void *);
typedef void (B11_MS *VoidFn)(void *);
typedef int32_t (B11_MS *SetFn)(void *, int32_t);
typedef int32_t (B11_MS *GetFn)(void *);
typedef void (B11_MS *TopologyFn)(void *, uint32_t);
typedef void (B11_MS *QueryFn)(void *, B11Obj *);
typedef B11HR (B11_MS *MapFn)(void *, B11Obj *, uint32_t, uint32_t, uint32_t, void *);
typedef void (B11_MS *GetRtFn)(void *, uint32_t, B11Obj **, B11Obj **);
typedef void (B11_MS *LayoutFn)(void *, B11Obj *);
typedef void (B11_MS *DepthFn)(void *, B11Obj *, uint32_t, float, uint8_t);
typedef float (B11_MS *LodFn)(void *, B11Obj *);
typedef B11HR (B11_MS *FinishFn)(void *, uint32_t, void **);
typedef void (B11_MS *DeviceFn)(void *, void **);
#define SLOT(o, n, type) ((type)(*(void ***)(o))[n])
static const uint8_t iid_mt[16] = { 0x00,0x4e,0x7e,0x9b,0x2c,0x34,0x06,0x41,0xa1,0x9f,0x4f,0x27,0x04,0xf6,0x89,0xf0 };
static const uint8_t iid_unknown[16] = { 0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46 };
static const uint8_t iid_context[16] = { 0x6c,0xa9,0xbf,0xc0,0x89,0xe0,0xfb,0x44,0x8e,0xaf,0x26,0xf8,0x79,0x61,0x90,0xda };
static const uint8_t iid_device[16] = { 0xdb,0x6d,0x6f,0xdb,0x77,0xac,0x88,0x4e,0x82,0x53,0x81,0x9d,0xf9,0xbb,0xf1,0x40 };

/* Failure injection is confined to this executable and occurs before Vulkan
 * initialization, or at the context allocation, never in production code. */
static int fail_init, fail_context_alloc, attr_destroyed;
int __real_pthread_mutexattr_init(pthread_mutexattr_t *);
int __real_pthread_mutexattr_settype(pthread_mutexattr_t *, int);
int __real_pthread_mutexattr_destroy(pthread_mutexattr_t *);
int __real_pthread_mutex_init(pthread_mutex_t *, const pthread_mutexattr_t *);
void *__real_calloc(size_t, size_t);
int __wrap_pthread_mutexattr_init(pthread_mutexattr_t *a)
{ return fail_init == 1 ? ENOMEM : __real_pthread_mutexattr_init(a); }
int __wrap_pthread_mutexattr_settype(pthread_mutexattr_t *a, int t)
{ return fail_init == 2 ? EINVAL : __real_pthread_mutexattr_settype(a, t); }
int __wrap_pthread_mutexattr_destroy(pthread_mutexattr_t *a)
{ ++attr_destroyed; return __real_pthread_mutexattr_destroy(a); }
int __wrap_pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a)
{ return (fail_init == 3 && a) || (fail_init == 4 && !a) ? ENOMEM : __real_pthread_mutex_init(m, a); }
void *__wrap_calloc(size_t n, size_t s)
{ return fail_context_alloc && n == 1 && s == sizeof(B11Context) ? NULL : __real_calloc(n, s); }

static int device_destroyed;
static int layout_destroyed;
static void fixture_destroy(B11Obj *o) { (void)o; ++device_destroyed; }
static void counted_layout_destroy(B11Obj *o) { (void)o; ++layout_destroyed; }
static void fixture_init(B11Device *d)
{
    memset(d, 0, sizeof(*d));
    pthread_mutexattr_t a;
    assert(!pthread_mutexattr_init(&a));
    assert(!pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE));
    assert(!pthread_mutex_init(&d->lock, &a));
    assert(!pthread_mutexattr_destroy(&a));
    d->obj.vtable = g_b11_device_vtable;
    atomic_init(&d->obj.refs, 1);
    d->obj.kind = B11_DEVICE;
    d->obj.dev = d;
    d->obj.destroy = fixture_destroy;
    b11_multithread_init(&d->multithread, &d->obj);
}
static void *qi(void *o, const uint8_t *iid)
{
    void *p = NULL;
    assert(SLOT(o, 0, QIFn)(o, iid, &p) == B11_S_OK && p);
    return p;
}
static void identity(void *o, void *mt, const uint8_t *iid)
{
    void *u = qi(mt, iid_unknown);
    assert(u == o);
    SLOT(u, 2, RefFn)(u);
    void *same = qi(mt, iid_mt);
    assert(same == mt);
    SLOT(same, 2, RefFn)(same);
    void *back = qi(mt, iid);
    assert(back == o);
    SLOT(back, 2, RefFn)(back);
    uint8_t invalid[16]; memset(invalid, 0xff, sizeof(invalid));
    void *out = (void *)1;
    assert(SLOT(mt, 0, QIFn)(mt, invalid, &out) == B11_E_NOINTERFACE && !out);
    assert(SLOT(mt, 0, QIFn)(mt, iid, NULL) == B11_E_INVALIDARG);
}

/* Condition-variable handshakes, with monotonic bounded waits. No sleeps or
 * polling. ETIMEDOUT is expected only while the tested lock must exclude work. */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cv;
    int ready, done, release, entered;
    int action;
    void *mt;
    B11Context *ctx;
    B11Query query;
} Work;
static struct timespec deadline(long ms)
{
    struct timespec t;
    assert(!clock_gettime(CLOCK_MONOTONIC, &t));
    t.tv_nsec += ms * 1000000;
    t.tv_sec += t.tv_nsec / 1000000000;
    t.tv_nsec %= 1000000000;
    return t;
}
static int wait_flag(Work *w, int *flag, long ms)
{
    struct timespec t = deadline(ms);
    assert(!pthread_mutex_lock(&w->lock));
    int e = 0;
    while (!*flag && !e) e = pthread_cond_timedwait(&w->cv, &w->lock, &t);
    assert(e == 0 || e == ETIMEDOUT);
    int value = *flag;
    assert(!pthread_mutex_unlock(&w->lock));
    return value;
}
static void signal_flag(Work *w, int *flag)
{
    assert(!pthread_mutex_lock(&w->lock));
    *flag = 1;
    assert(!pthread_cond_broadcast(&w->cv));
    assert(!pthread_mutex_unlock(&w->lock));
}
static void work_init(Work *w, B11Context *c, void *mt, int action)
{
    memset(w, 0, sizeof(*w));
    assert(!pthread_mutex_init(&w->lock, NULL));
    pthread_condattr_t a;
    assert(!pthread_condattr_init(&a));
    assert(!pthread_condattr_setclock(&a, CLOCK_MONOTONIC));
    assert(!pthread_cond_init(&w->cv, &a));
    assert(!pthread_condattr_destroy(&a));
    w->ctx = c; w->mt = mt; w->action = action;
}
static void work_destroy(Work *w)
{
    assert(!pthread_cond_destroy(&w->cv));
    assert(!pthread_mutex_destroy(&w->lock));
}
static void *worker(void *arg)
{
    Work *w = arg;
    B11Context *c = w->ctx;
    signal_flag(w, &w->ready);
    switch (w->action) {
    case 0: SLOT(w->mt, 3, VoidFn)(w->mt); SLOT(w->mt, 4, VoidFn)(w->mt); break;
    case 1: SLOT(c, 24, TopologyFn)(c, 3); break; /* redundant-state early return */
    case 2: SLOT(c, 24, TopologyFn)(c, 4); break; /* real shadow-state mutation */
    case 3: SLOT(c, 27, QueryFn)(c, &w->query.obj); break;
    case 4: SLOT(c, 28, QueryFn)(c, &w->query.obj); break;
    case 5: assert(SLOT(c, 14, MapFn)(c, NULL, 0, 1, 0, NULL) == B11_E_INVALIDARG); break;
    case 6: { B11Obj *r = (void *)1; SLOT(c, 89, GetRtFn)(c, 1, &r, NULL); assert(!r); break; }
    case 7: assert(SLOT(c, 112, GetFn)(c) == 0); break;
    case 8: SLOT(c, 110, VoidFn)(c); break;
    case 9: SLOT(c, 72, VoidFn)(c); break; /* existing unimplemented getter slot */
    case 10: SLOT(c, 17, LayoutFn)(c, NULL); break;
    default: abort();
    }
    signal_flag(w, &w->done);
    return NULL;
}
static void blocked_call(B11Context *c, void *hold_mt, void *worker_mt, int action, int nested)
{
    Work w; work_init(&w, c, worker_mt, action);
    SLOT(hold_mt, 3, VoidFn)(hold_mt);
    if (nested) SLOT(hold_mt, 3, VoidFn)(hold_mt);
    pthread_t t;
    assert(!pthread_create(&t, NULL, worker, &w));
    assert(wait_flag(&w, &w.ready, 2000));
    assert(!wait_flag(&w, &w.done, 100));
    if (nested) {
        SLOT(hold_mt, 4, VoidFn)(hold_mt);
        assert(!wait_flag(&w, &w.done, 100));
    }
    SLOT(hold_mt, 4, VoidFn)(hold_mt);
    assert(wait_flag(&w, &w.done, 2000));
    assert(!pthread_join(t, NULL));
    if (action == 2) assert(c->st.topology == 4);
    if (action == 3) assert(w.query.issued);
    if (action == 4) assert(w.query.ended);
    work_destroy(&w);
}

static Work *destruction_work;
static void held_layout_destroy(B11Obj *o)
{
    (void)o;
    signal_flag(destruction_work, &destruction_work->entered);
    assert(wait_flag(destruction_work, &destruction_work->release, 2000));
}
static void automatic_excludes_enter(B11Context *c, void *mt)
{
    Work inside; work_init(&inside, c, mt, 10);
    Work attempt; work_init(&attempt, c, mt, 0);
    B11Obj layout = { .kind = B11_LAYOUT, .dev = c->obj.dev, .destroy = held_layout_destroy };
    atomic_init(&layout.refs, 1);
    c->st.layout = &layout;
    destruction_work = &inside;
    pthread_t a, b;
    assert(!pthread_create(&a, NULL, worker, &inside));
    assert(wait_flag(&inside, &inside.entered, 2000));
    assert(!pthread_create(&b, NULL, worker, &attempt));
    assert(wait_flag(&attempt, &attempt.ready, 2000));
    assert(!wait_flag(&attempt, &attempt.done, 100));
    signal_flag(&inside, &inside.release);
    assert(wait_flag(&inside, &inside.done, 2000));
    assert(wait_flag(&attempt, &attempt.done, 2000));
    assert(!pthread_join(a, NULL)); assert(!pthread_join(b, NULL));
    work_destroy(&inside); work_destroy(&attempt);
    destruction_work = NULL;
    assert(!c->st.layout);
}

int main(void)
{
    b11_init_vtables();
    for (fail_init = 1; fail_init <= 4; ++fail_init) {
        int before = attr_destroyed;
        assert(!b11_device_create());
        assert(attr_destroyed == before + (fail_init != 1));
    }
    fail_init = 0;
    B11Device d; fixture_init(&d);
    fail_context_alloc = 1;
    assert(!b11_context_create(&d, 0, 0));
    assert(atomic_load(&d.obj.refs) == 1);
    fail_context_alloc = 0;
    assert(!b11_context_create(NULL, 0, 0));
    assert(!b11_context_create(&d, 2, 0));
    d.immediate = b11_context_create(&d, 0, 0);
    assert(d.immediate);
    const uint8_t iid_device3[16] = {0x37,0x8c,0x5c,0xa0,0xc6,0xd2,0x32,0x47,0xb3,0xa0,0x9c,0xe0,0xb0,0xdc,0x9a,0xe6};
    void *unsupported = (void *)1;
    assert(SLOT(&d, 0, QIFn)(&d, iid_device3, &unsupported) == B11_E_NOINTERFACE && !unsupported);
    B11Context *c = d.immediate;
    /* Verified against d3d11_1/2/3.h, not similarly named DXGI interfaces. */
    const uint8_t device_extensions[][16] = {
        {0x29,0xfb,0x4b,0xa0,0xef,0x08,0xd6,0x43,0xa4,0x9c,0xa9,0xbd,0xbd,0xcb,0xe6,0x86},
        {0xfa,0xdf,0x06,0x9d,0xe5,0xd1,0x07,0x4d,0x83,0xa8,0x1b,0xb1,0x23,0xf2,0xf8,0x41},
        {0x37,0x8c,0x5c,0xa0,0xc6,0xd2,0x32,0x47,0xb3,0xa0,0x9c,0xe0,0xb0,0xdc,0x9a,0xe6}
    };
    const uint8_t context_extensions[][16] = {
        {0xaa,0x6f,0x2c,0xbb,0xfb,0xb5,0x82,0x40,0x8e,0x6b,0x38,0x8b,0x8c,0xfa,0x90,0xe1},
        {0x32,0x5b,0x0d,0x42,0x0c,0xb9,0xa4,0x4d,0xbe,0xf0,0x35,0x9f,0x6a,0x24,0xa8,0x3a},
        {0x1d,0xc0,0xe3,0xb4,0x9e,0xe7,0x37,0x46,0x91,0xb2,0x51,0x0e,0x9f,0x4c,0x9b,0x8f}
    };
    uint32_t device_refs = atomic_load(&d.obj.refs), context_refs = atomic_load(&c->obj.refs);
    for (size_t i = 0; i < 3; ++i) {
        unsupported = (void *)1;
        assert(SLOT(&d, 0, QIFn)(&d, device_extensions[i], &unsupported) == B11_E_NOINTERFACE && !unsupported);
        unsupported = (void *)1;
        assert(SLOT(c, 0, QIFn)(c, context_extensions[i], &unsupported) == B11_E_NOINTERFACE && !unsupported);
    }
    assert(atomic_load(&d.obj.refs) == device_refs && atomic_load(&c->obj.refs) == context_refs);
    void *cm = qi(c, iid_mt), *dm = qi(&d, iid_mt);
    assert(cm != c && dm != &d && cm != dm);
    identity(c, cm, iid_context); identity(&d, dm, iid_device);
    uint32_t added = SLOT(cm, 1, RefFn)(cm);
    assert(added == atomic_load(&c->obj.refs));
    SLOT(cm, 2, RefFn)(cm);
    assert(SLOT(cm, 6, GetFn)(cm) == 0);
    blocked_call(c, cm, dm, 0, 1); /* Enter works even with protection disabled */
    assert(SLOT(dm, 5, SetFn)(dm, 7) == 0);
    assert(SLOT(cm, 6, GetFn)(cm) == 1);
    assert(SLOT(cm, 5, SetFn)(cm, 1) == 1);
    SLOT(c, 24, TopologyFn)(c, 3);
    for (int action = 1; action <= 9; ++action) blocked_call(c, dm, cm, action, 0);
    automatic_excludes_enter(c, dm);
    assert(SLOT(cm, 5, SetFn)(cm, 0) == 1);
    assert(SLOT(dm, 5, SetFn)(dm, 0) == 0);
    /* Disabled mode genuinely skips the automatic guard on a getter. */
    Work w; work_init(&w, c, cm, 7);
    SLOT(dm, 3, VoidFn)(dm);
    pthread_t t; assert(!pthread_create(&t, NULL, worker, &w));
    assert(wait_flag(&w, &w.done, 2000));
    SLOT(dm, 4, VoidFn)(dm);
    assert(!pthread_join(t, NULL)); work_destroy(&w);

    B11Context *deferred = b11_context_create(&d, 1, 0x42);
    assert(deferred);
    void *out = (void *)1;
    assert(SLOT(deferred, 0, QIFn)(deferred, iid_mt, &out) == B11_E_NOINTERFACE && !out);
    SLOT(deferred, 53, DepthFn)(deferred, NULL, 3, 0.375f, 0xa7);
    assert(deferred->op_count == 1);
    float depth; memcpy(&depth, &deferred->ops[0].b, 4);
    assert(depth == 0.375f && deferred->ops[0].a == 3 && deferred->ops[0].c == 0xa7);
    assert(SLOT(cm, 5, SetFn)(cm, 1) == 0);
    assert(SLOT(c, 56, LodFn)(c, NULL) == 0.0f);
    SLOT(c, 53, DepthFn)(c, NULL, 3, 0.375f, 0xa7);
    assert(SLOT(cm, 5, SetFn)(cm, 0) == 1);
    B11Obj retained_layout = { .kind = B11_LAYOUT, .dev = &d, .destroy = counted_layout_destroy };
    atomic_init(&retained_layout.refs, 1);
    SLOT(deferred, 17, LayoutFn)(deferred, &retained_layout);
    assert(atomic_load(&retained_layout.refs) == 3); /* caller + shadow + op */
    void *list = NULL;
    assert(SLOT(deferred, 114, FinishFn)(deferred, 0, &list) == B11_S_OK && list);
    assert(atomic_load(&retained_layout.refs) == 2);
    assert(b11_release(&retained_layout) == 1);
    uint32_t list_refs = atomic_load(&d.obj.refs);
    assert(SLOT(deferred, 2, RefFn)(deferred) == 0);
    assert(atomic_load(&d.obj.refs) == list_refs - 1 && !layout_destroyed);
    assert(SLOT(list, 7, GetFn)(list) == 0x42);
    void *list_device = NULL;
    SLOT(list, 3, DeviceFn)(list, &list_device);
    assert(list_device == &d);
    SLOT(list_device, 2, RefFn)(list_device);
    out = (void *)1;
    assert(SLOT(list, 0, QIFn)(list, iid_mt, &out) == B11_E_NOINTERFACE && !out);
    assert(SLOT(list, 2, RefFn)(list) == 0 && layout_destroyed == 1);

    /* A context tear-off keeps its controlling identity and device alive even
     * after the ordinary context reference is dropped. */
    B11Context *standalone = b11_context_create(&d, 0, 0);
    void *sm = qi(standalone, iid_mt);
    assert(SLOT(standalone, 2, RefFn)(standalone) == 1);
    identity(standalone, sm, iid_context);
    assert(SLOT(sm, 5, SetFn)(sm, 1) == 0);
    assert(SLOT(cm, 6, GetFn)(cm) == 0); /* independent context state */
    uint32_t before = atomic_load(&d.obj.refs);
    assert(SLOT(sm, 2, RefFn)(sm) == 0);
    assert(atomic_load(&d.obj.refs) == before - 1);
    SLOT(cm, 2, RefFn)(cm); SLOT(dm, 2, RefFn)(dm);
    d.immediate = NULL;
    assert(SLOT(c, 2, RefFn)(c) == 0);
    assert(atomic_load(&d.obj.refs) == 1);
    assert(SLOT(&d, 2, RefFn)(&d) == 0 && device_destroyed == 1);
    assert(!pthread_mutex_destroy(&d.lock));
    puts("BD11 ID3D11Multithread: ABI, identity, lifetime, toggles, recursion, exclusion and init failures PASS");
    return 0;
}