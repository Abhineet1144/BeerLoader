#define _GNU_SOURCE
/* CPU-only calls through the actual Win64 COM tables; no guest or GPU. */
#include "bd11.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int (*g_beer_lazy_fault_hook)(void *);

typedef B11HR (B11_MS *GetFn)(void *, const uint8_t *, uint32_t *, void *);
typedef B11HR (B11_MS *SetFn)(void *, const uint8_t *, uint32_t, const void *);
typedef B11HR (B11_MS *IfaceFn)(void *, const uint8_t *, const void *);
typedef uint32_t (B11_MS *RefFn)(void *);
typedef B11HR (B11_MS *FinishFn)(void *, uint32_t, void **);
typedef B11HR (B11_MS *RasterFn)(void *, const void *, void **);
#define SLOT(o,s,t) ((t)(*(void ***)(o))[s])
static const uint8_t key[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
static const uint8_t other[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,17};
static unsigned base(void *o) { return ((B11Obj *)o)->kind == B11_DEVICE ? 34 : 4; }
static B11HR get(void *o, const uint8_t *k, uint32_t *n, void *p)
{ return SLOT(o,base(o),GetFn)(o,k,n,p); }
static B11HR set(void *o, const uint8_t *k, uint32_t n, const void *p)
{ return SLOT(o,base(o)+1,SetFn)(o,k,n,p); }
static B11HR iface(void *o, const uint8_t *k, const void *p)
{ return SLOT(o,base(o)+2,IfaceFn)(o,k,p); }
static uint32_t release(void *o) { return SLOT(o,2,RefFn)(o); }

static _Thread_local int fail_malloc;
void *__real_malloc(size_t);
void *__wrap_malloc(size_t n)
{
    if (fail_malloc) { fail_malloc=0; return NULL; }
    return __real_malloc(n);
}

/* Deliberately NOT a B11Obj: foreign reference count is at a different offset. */
typedef struct Foreign {
    void **vtable;
    uint64_t poison[3];
    _Atomic uint32_t refs, adds, releases;
    void *owner;
    _Atomic int on_add, on_release;
    pthread_mutex_t lock;
    pthread_cond_t cv;
    int blocked, proceed;
} Foreign;

static void reenter(Foreign *f, int mode)
{
    if (!mode) return;
    uint32_t n=0, value=0x12345678;
    if (mode==3) { /* Owner final release: cannot republish during cleanup. */
        assert(get(f->owner,key,&n,NULL)==B11_E_INVALIDARG);
        assert(set(f->owner,key,4,&value)==B11_E_INVALIDARG);
        assert(iface(f->owner,key,f)==B11_E_INVALIDARG);
        return;
    }
    assert(get(f->owner,key,&n,NULL)==B11_S_OK);
    assert(set(f->owner,key,4,&value)==B11_S_OK);
    if (mode==2) { /* Getter's unlocked AddRef is held across replacement. */
        pthread_mutex_lock(&f->lock);
        f->blocked=1; pthread_cond_broadcast(&f->cv);
        while (!f->proceed) assert(!pthread_cond_wait(&f->cv,&f->lock));
        pthread_mutex_unlock(&f->lock);
    }
}
static uint32_t B11_MS foreign_add(Foreign *f)
{
    atomic_fetch_add(&f->adds,1);
    reenter(f,atomic_exchange(&f->on_add,0));
    return atomic_fetch_add(&f->refs,1)+1;
}
static uint32_t B11_MS foreign_release(Foreign *f)
{
    atomic_fetch_add(&f->releases,1);
    uint32_t r=atomic_fetch_sub(&f->refs,1)-1;
    /* A pin must prevent a concurrent overwrite from dropping the final
     * store reference before the getter has acquired its caller reference. */
    assert(r < UINT32_MAX);
    reenter(f,atomic_exchange(&f->on_release,0));
    return r;
}
static void *foreign_vt[3]={NULL,(void *)foreign_add,(void *)foreign_release};
static void foreign_init(Foreign *f, void *owner)
{
    memset(f,0,sizeof(*f)); f->vtable=foreign_vt; f->owner=owner;
    atomic_init(&f->refs,1);
    assert(!pthread_mutex_init(&f->lock,NULL)); assert(!pthread_cond_init(&f->cv,NULL));
}
static void foreign_done(Foreign *f)
{
    assert(atomic_load(&f->refs)==1);
    assert(atomic_load(&f->adds)==atomic_load(&f->releases));
    assert(!pthread_cond_destroy(&f->cv)); assert(!pthread_mutex_destroy(&f->lock));
}

static void byte_contract(void *o)
{
    unsigned char src[7]={7,6,5,4,3,2,1}, saved[7]; memcpy(saved,src,7);
    uint32_t n=99; unsigned char out[11]; memset(out,0xa5,sizeof(out));
    assert(get(o,key,&n,out+2)==B11_DXGI_ERROR_NOT_FOUND && n==0);
    assert(get(o,key,NULL,out)==B11_E_INVALIDARG);
    assert(get(o,NULL,&n,out)==B11_E_INVALIDARG);
    assert(set(o,NULL,7,src)==B11_E_INVALIDARG);
    assert(iface(o,NULL,NULL)==B11_E_INVALIDARG);
    assert(set(o,key,1,NULL)==B11_E_INVALIDARG);
    assert(set(o,key,0,NULL)==B11_S_FALSE);
    fail_malloc=1; assert(set(o,key,7,src)==B11_E_OUTOFMEMORY);
    n=7; assert(get(o,key,&n,NULL)==B11_DXGI_ERROR_NOT_FOUND && n==0);
    assert(set(o,key,7,src)==B11_S_OK); memset(src,0,7);
    assert(set(o,key,1,NULL)==B11_E_INVALIDARG); /* old bytes unchanged */
    assert(set(o,other,3,"abc")==B11_S_OK);
    n=0; assert(get(o,key,&n,NULL)==B11_S_OK && n==7);
    n=6; assert(get(o,key,&n,out+2)==B11_DXGI_ERROR_MORE_DATA && n==7);
    for (unsigned i=0;i<sizeof(out);++i) assert(out[i]==0xa5);
    n=9; assert(get(o,key,&n,out+2)==B11_S_OK && n==7);
    assert(!memcmp(out+2,saved,7)); assert(out[0]==0xa5 && out[1]==0xa5 && out[9]==0xa5 && out[10]==0xa5);
    n=7; assert(get(o,other,&n,out)==B11_S_OK && n==3 && !memcmp(out,"abc",3));
    assert(set(o,key,0,src)==B11_S_OK); /* non-NULL empty blob is NOT deletion */
    n=0; out[0]=0xa5; assert(get(o,key,&n,out)==B11_S_OK && n==0 && out[0]==0xa5);
    assert(set(o,key,0,NULL)==B11_S_OK);
    assert(set(o,key,0,NULL)==B11_S_FALSE);
    assert(set(o,other,0,NULL)==B11_S_OK);
    /* GUIDs are byte keys, not aligned struct accesses. */
    uint8_t unaligned[17]; memcpy(unaligned+1,key,16);
    assert(set(o,unaligned+1,7,saved)==B11_S_OK);
    n=7; assert(get(o,key,&n,out)==B11_S_OK && !memcmp(out,saved,7));
    assert(set(o,unaligned+1,0,NULL)==B11_S_OK);
}

static void pointer_contract(void *o)
{
    Foreign a,b; foreign_init(&a,o); foreign_init(&b,o);
    assert(iface(o,key,&a)==B11_S_OK && atomic_load(&a.refs)==2);
    assert(iface(o,key,&a)==B11_S_OK && atomic_load(&a.refs)==2); /* self replace */
    uint32_t adds=atomic_load(&a.adds), n=0;
    assert(get(o,key,&n,NULL)==B11_S_OK && n==sizeof(void *));
    n=99; assert(get(o,key,&n,NULL)==B11_S_OK && n==sizeof(void *));
    struct { uint64_t before; void *p; uint64_t after; } out={0x1234,(void *)0x5678,0x9abc};
    n=sizeof(void *)-1;
    assert(get(o,key,&n,&out.p)==B11_DXGI_ERROR_MORE_DATA && out.p==(void *)0x5678);
    assert(get(o,key,NULL,&out.p)==B11_E_INVALIDARG && out.p==(void *)0x5678);
    assert(atomic_load(&a.adds)==adds);
    n=sizeof(out); assert(get(o,key,&n,&out.p)==B11_S_OK && out.p==&a);
    assert(out.before==0x1234 && out.after==0x9abc && atomic_load(&a.refs)==3);
    release(out.p);
    fail_malloc=1; assert(iface(o,key,&b)==B11_E_OUTOFMEMORY);
    assert(atomic_load(&a.refs)==2 && atomic_load(&b.refs)==1 && atomic_load(&b.adds)==0);
    fail_malloc=1; assert(set(o,key,4,"fail")==B11_E_OUTOFMEMORY);
    n=sizeof(void *); assert(get(o,key,&n,&out.p)==B11_S_OK && out.p==&a); release(out.p);
    assert(iface(o,key,&b)==B11_S_OK && atomic_load(&a.refs)==1 && atomic_load(&b.refs)==2);
    assert(set(o,key,4,"copy")==B11_S_OK && atomic_load(&b.refs)==1);
    assert(iface(o,key,&a)==B11_S_OK);
    assert(iface(o,key,NULL)==B11_S_OK && atomic_load(&a.refs)==1);
    out.p=&a; n=sizeof(void *);
    assert(get(o,key,&n,&out.p)==B11_S_OK && n==sizeof(void *) && !out.p);
    assert(set(o,key,0,NULL)==B11_S_OK);
    assert(iface(o,key,&a)==B11_S_OK);
    assert(set(o,key,0,NULL)==B11_S_OK && atomic_load(&a.refs)==1);
    /* Ordinary COM cycle: store owns a real owner ref, explicitly broken. */
    uint32_t refs=atomic_load(&((B11Obj *)o)->refs);
    assert(iface(o,key,o)==B11_S_OK && atomic_load(&((B11Obj *)o)->refs)==refs+1);
    assert(set(o,key,0,NULL)==B11_S_OK && atomic_load(&((B11Obj *)o)->refs)==refs);
    foreign_done(&a); foreign_done(&b);
}

static void *one_get(void *arg)
{
    Foreign *f=arg; void *p=NULL; uint32_t n=sizeof(p);
    assert(get(f->owner,key,&n,&p)==B11_S_OK && p==f); release(p); return NULL;
}
static void reentrant_contract(void *o)
{
    Foreign f; foreign_init(&f,o);
    assert(set(o,key,4,"seed")==B11_S_OK);
    atomic_store(&f.on_add,1);
    assert(iface(o,key,&f)==B11_S_OK && atomic_load(&f.refs)==2);
    atomic_store(&f.on_release,1);
    assert(set(o,key,4,"next")==B11_S_OK && atomic_load(&f.refs)==1);
    uint32_t value=0,n=4;
    assert(get(o,key,&n,&value)==B11_S_OK && value==0x12345678);
    assert(iface(o,key,&f)==B11_S_OK);
    atomic_store(&f.on_add,2);
    pthread_t t; assert(!pthread_create(&t,NULL,one_get,&f));
    pthread_mutex_lock(&f.lock);
    while (!f.blocked) assert(!pthread_cond_wait(&f.cv,&f.lock));
    /* AddRef itself replaced the key. Its entry pin still owns one ref. */
    assert(atomic_load(&f.refs)==2);
    assert(set(o,key,0,NULL)==B11_S_OK);
    assert(iface(o,key,NULL)==B11_S_OK);
    f.proceed=1; pthread_cond_broadcast(&f.cv); pthread_mutex_unlock(&f.lock);
    assert(!pthread_join(t,NULL));
    assert(set(o,key,0,NULL)==B11_S_OK);
    foreign_done(&f);
}

typedef struct { void *owner; Foreign *f; int writer; } Work;
static void *stress(void *arg)
{
    Work *w=arg;
    for (unsigned i=0;i<4000;++i) {
        if (w->writer) {
            assert(iface(w->owner,key,w->f)==B11_S_OK);
            assert(iface(w->owner,key,NULL)==B11_S_OK);
            B11HR hr=set(w->owner,key,0,NULL); assert(hr==B11_S_OK || hr==B11_S_FALSE);
            uint64_t bytes=0x1122334455667788ull;
            assert(set(w->owner,other,sizeof(bytes),&bytes)==B11_S_OK);
            hr=set(w->owner,other,0,NULL); assert(hr==B11_S_OK || hr==B11_S_FALSE);
        } else {
            struct { uint64_t pre; void *p; uint64_t post; } out={123,NULL,456};
            uint32_t n=sizeof(out.p); B11HR hr=get(w->owner,key,&n,&out.p);
            assert(out.pre==123 && out.post==456);
            assert(hr==B11_S_OK || hr==B11_DXGI_ERROR_NOT_FOUND);
            if (hr==B11_S_OK) { assert(!out.p || out.p==w->f); if (out.p) release(out.p); }
            uint64_t bytes=0; n=sizeof(bytes); hr=get(w->owner,other,&n,&bytes);
            assert(hr==B11_DXGI_ERROR_NOT_FOUND || (hr==B11_S_OK && bytes==0x1122334455667788ull));
        }
    }
    return NULL;
}
static void concurrent_contract(void *o)
{
    Foreign f; foreign_init(&f,o); pthread_t threads[4]; Work work[4];
    for (int i=0;i<4;++i) { work[i]=(Work){o,&f,i<2}; assert(!pthread_create(&threads[i],NULL,stress,&work[i])); }
    for (int i=0;i<4;++i) assert(!pthread_join(threads[i],NULL));
    set(o,key,0,NULL); set(o,other,0,NULL); foreign_done(&f);
}

static int device_destroyed;
static void fixture_destroy(B11Obj *o) { (void)o; ++device_destroyed; }
static void destruction_contract(void *o)
{
    Foreign f,g; foreign_init(&f,o); foreign_init(&g,o);
    assert(iface(o,key,&f)==B11_S_OK); atomic_store(&f.on_release,3);
    assert(iface(o,other,&g)==B11_S_OK);
    const uint8_t bytes_key[16]={0};
    assert(set(o,bytes_key,4,"last")==B11_S_OK);
    assert(release(o)==0); foreign_done(&f); foreign_done(&g);
}

int main(void)
{
    b11_init_vtables();
    B11Device dev={0}; dev.obj.vtable=g_b11_device_vtable; dev.obj.dev=&dev;
    dev.obj.kind=B11_DEVICE; dev.obj.destroy=fixture_destroy; atomic_init(&dev.obj.refs,1);
    pthread_mutexattr_t attr; assert(!pthread_mutexattr_init(&attr));
    assert(!pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE));
    assert(!pthread_mutex_init(&dev.lock,&attr)); assert(!pthread_mutexattr_destroy(&attr));
    B11Context *im=b11_context_create(&dev,0,0), *de=b11_context_create(&dev,1,0);
    assert(im && de); dev.immediate=im;
    void *list=NULL; assert(SLOT(de,114,FinishFn)(de,0,&list)==B11_S_OK && list);
    B11RasterizerDesc1 desc={.fill_mode=3,.cull_mode=1,.depth_clip=1};
    void *child=NULL,*extended=NULL;
    assert(SLOT(&dev,22,RasterFn)(&dev,&desc,&child)==B11_S_OK);
    assert(b11_create_rasterizer_state1(&dev,&desc,&extended)==B11_S_OK);
    void *objects[]={&dev,im,de,list,child,extended};
    for (unsigned i=0;i<sizeof(objects)/sizeof(*objects);++i) {
        byte_contract(objects[i]); pointer_contract(objects[i]);
    }
    /* Same GUID differs across identities, including device/context/list. */
    for (unsigned i=0;i<6;++i) assert(set(objects[i],key,4,&i)==B11_S_OK);
    for (unsigned i=0;i<6;++i) { uint32_t n=4,v=99; assert(get(objects[i],key,&n,&v)==B11_S_OK && v==i); }
    for (unsigned i=0;i<6;++i) set(objects[i],key,0,NULL);
    /* Both protected and unprotected contexts use no backend lock for callbacks. */
    atomic_store(&im->multithread_protected,1);
    reentrant_contract(im); reentrant_contract(list); reentrant_contract(&dev);
    concurrent_contract(im);
    /* Final-release cleanup runs before all actual object destructors. */
    destruction_contract(child); destruction_contract(extended); destruction_contract(list);
    destruction_contract(de); destruction_contract(im);
    assert(atomic_load(&dev.obj.refs)==1); destruction_contract(&dev);
    assert(device_destroyed==1); assert(!pthread_mutex_destroy(&dev.lock));
    puts("BD11 private-data: typed ABI, ownership, errors, reentrancy, concurrency PASS");
    return 0;
}