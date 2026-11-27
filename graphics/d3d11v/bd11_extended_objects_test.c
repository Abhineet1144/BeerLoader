#define _GNU_SOURCE
#include "bd11.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* Real Win64 creation functions and object slots; only GPU descriptor/resource
 * allocation is injected. Validation, normalization, COM and rollback are real. */
int (*g_beer_lazy_fault_hook)(void *);
typedef B11HR (B11_MS *QIFn)(void *, const uint8_t *, void **);
typedef uint32_t (B11_MS *RefFn)(void *);
typedef void (B11_MS *GetFn)(void *, void *);
typedef B11HR (B11_MS *RasterFn)(B11Device *, const B11RasterizerDesc1 *, void **);
typedef B11HR (B11_MS *Srv1Fn)(B11Device *, B11Obj *, const B11ShaderResourceViewDesc1 *, void **);
typedef B11HR (B11_MS *ViewFn)(B11Device *, B11Obj *, const uint32_t *, void **);
typedef B11HR (B11_MS *StateFn)(B11Device *, const void *, void **);
typedef B11HR (B11_MS *BufferFn)(B11Device *, const B11BufferDesc *, const void *, void **);
#define SLOT(o,n,t) ((t)(*(void ***)(o))[n])
static RasterFn raster1 = b11_create_rasterizer_state1;
static Srv1Fn srv1 = b11_create_shader_resource_view1;
/* Independent little-endian IID bytes transcribed from the downloaded headers. */
static const uint8_t iid_raster1[16] = {0xa6,0xd7,0x17,0x12,0x39,0x50,0x8c,0x41,0xb0,0x42,0x9c,0xbe,0x25,0x6a,0xfd,0x6e};
static const uint8_t iid_srv1[16] = {0x87,0x8b,0x30,0x91,0x40,0x90,0x1d,0x41,0x8c,0x67,0xc3,0x92,0x53,0xce,0x38,0x02};
static const uint8_t iid_raster[16] = {0x81,0xab,0xb4,0x9b,0x1a,0xab,0x8f,0x4d,0xb5,0x06,0xfc,0x04,0x20,0x0b,0x6e,0xe7};
static const uint8_t iid_srv[16] = {0xe0,0x6f,0xe0,0xb0,0x92,0x81,0x1a,0x4e,0xb1,0xca,0x36,0xd7,0x41,0x47,0x10,0xb2};
static const uint8_t iid_child[16] = {0xc8,0xe5,0x41,0x18,0xb0,0x16,0x9b,0x48,0xbc,0xc8,0x44,0xcf,0xb0,0xd5,0xde,0xae};
static const uint8_t iid_resource[16] = {0xf3,0x63,0x8e,0xdc,0x2b,0xd1,0x52,0x49,0xb4,0x7b,0x5e,0x45,0x02,0x6a,0x86,0x2d};
static const uint8_t iid_unknown[16] = {0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
static const uint8_t iid_device3[16] = {0x37,0x8c,0x5c,0xa0,0xc6,0xd2,0x32,0x47,0xb3,0xa0,0x9c,0xe0,0xb0,0xdc,0x9a,0xe6};
static const uint8_t iid_raster2[16] = {0xfb,0x02,0xbd,0x6f,0x9f,0x20,0xc4,0x46,0xb0,0x59,0x2e,0xd1,0x55,0x86,0xa6,0xac};

static size_t fail_size;
static unsigned allocations, heaps, heap_frees, images, image_frees, writes, buffer_writes;
static int fail_heap, fail_write;
static struct { VkImageViewType type; VkFormat format; uint32_t mip,mips,layer,layers,swizzle; BD12Image *image; } last;
static VkDeviceSize last_offset, last_range;
void *__real_calloc(size_t, size_t);
void *__wrap_calloc(size_t n, size_t s)
{
    ++allocations;
    if (fail_size && n == 1 && s == fail_size) return NULL;
    return __real_calloc(n, s);
}
BD12DescriptorHeap *__wrap_bd12_descriptor_heap_create(BD12Device *d, uint32_t type, uint32_t n)
{
    ++heaps; assert(type == 0 && n == 1);
    if (fail_heap) return NULL;
    BD12DescriptorHeap *h = calloc(1, sizeof(*h)); assert(h);
    h->device = d; h->count = n;
    h->descriptors = aligned_alloc(128, sizeof(BD12Descriptor)); assert(h->descriptors);
    memset(h->descriptors, 0, sizeof(BD12Descriptor));
    return h;
}
void __wrap_bd12_descriptor_heap_destroy(BD12DescriptorHeap *h)
{ ++heap_frees; free(h->descriptors); free(h); }
BD12Image *__wrap_bd12_image_create_committed(BD12Device *d, const BD12ImageDesc *desc, uint32_t state)
{
    ++images;
    BD12Image *i = calloc(1, sizeof(*i)); assert(i);
    i->device = d; i->desc = *desc; i->initial_state = state;
    i->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    return i;
}
void __wrap_bd12_image_destroy(BD12Image *i) { ++image_frees; free(i); }
uint64_t __wrap_bd12_fence_completed(BD12Fence *f) { (void)f; return UINT64_MAX; }
int __wrap_bd12_descriptor_write_image_srv_mapped(BD12DescriptorHeap *h, uint32_t idx,
    BD12Image *image, VkImageViewType type, VkFormat format, uint32_t mip,
    uint32_t mips, uint32_t layer, uint32_t layers, uint32_t swizzle)
{
    ++writes; assert(idx == 0 && h && image && mips && layers);
    assert(mip < image->desc.mip_levels && mips <= image->desc.mip_levels - mip);
    last.type=type; last.format=format; last.mip=mip; last.mips=mips;
    last.layer=layer; last.layers=layers; last.swizzle=swizzle; last.image=image;
    h->descriptors[0].image = image; h->descriptors[0].format = format;
    return !fail_write;
}
int __wrap_bd12_descriptor_write_image_srv_aspect(BD12DescriptorHeap *h, uint32_t idx,
    BD12Image *i, VkImageViewType t, VkFormat f, uint32_t m, uint32_t n, uint32_t l,
    uint32_t c, VkImageAspectFlags a)
{
    (void)h;(void)idx;(void)i;(void)t;(void)f;(void)m;(void)n;(void)l;(void)c;(void)a;
    assert(!"extended depth/stencil must never call backend"); return 0;
}
BD12Buffer *__wrap_bd12_buffer_create_committed(BD12Device *d, BD12HeapType t, VkDeviceSize n)
{
    (void)t; BD12Buffer *b=calloc(1,sizeof(*b)); assert(b); b->device=d; b->size=n; return b;
}
void __wrap_bd12_buffer_destroy(BD12Buffer *b) { free(b); }
int __wrap_bd12_descriptor_write_buffer_srv(BD12DescriptorHeap *h, uint32_t idx,
    BD12Buffer *b, VkDeviceSize off, VkDeviceSize range, VkFormat f)
{
    (void)f; assert(h && idx==0 && b && off <= b->size && range <= b->size-off);
    ++buffer_writes; last_offset=off; last_range=range; return !fail_write;
}

static void init_device(B11Device *d)
{
    memset(d,0,sizeof(*d)); d->obj.vtable=g_b11_device_vtable; d->obj.kind=B11_DEVICE;
    d->obj.dev=d; d->feature_level=0xb000; atomic_init(&d->obj.refs,1);
    assert(!pthread_mutex_init(&d->grave_lock,NULL));
}
static void release(void *o) { SLOT(o,2,RefFn)(o); }
static void qi_same(void *o, const uint8_t *iid)
{
    void *p=NULL; assert(SLOT(o,0,QIFn)(o,iid,&p)==B11_S_OK && p==o); release(p);
}
static void qi_no(void *o, const uint8_t *iid)
{
    void *p=(void *)1; assert(SLOT(o,0,QIFn)(o,iid,&p)==B11_E_NOINTERFACE && !p);
}
static void get_exact(void *o, unsigned slot, const void *expected, size_t size)
{
    uint8_t bytes[80]; memset(bytes,0xa5,sizeof(bytes));
    SLOT(o,slot,GetFn)(o,bytes+8);
    assert(!memcmp(bytes+8,expected,size));
    for (size_t i=0;i<8;++i) assert(bytes[i]==0xa5);
    for (size_t i=8+size;i<sizeof(bytes);++i) assert(bytes[i]==0xa5);
    long page=sysconf(_SC_PAGESIZE); assert(page>0);
    uint8_t *p=mmap(NULL,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(p!=MAP_FAILED && !mprotect(p+page,page,PROT_NONE));
    SLOT(o,slot,GetFn)(o,p+page-size); /* Detect even one-byte overwrite. */
    assert(!memcmp(p+page-size,expected,size)); assert(!munmap(p,page*2));
    SLOT(o,slot,GetFn)(o,NULL);
}
static void private_roundtrip(void *o)
{
    typedef B11HR (B11_MS *GetPrivateFn)(void *, const uint8_t *, uint32_t *, void *);
    typedef B11HR (B11_MS *SetPrivateFn)(void *, const uint8_t *, uint32_t, const void *);
    typedef B11HR (B11_MS *SetInterfaceFn)(void *, const uint8_t *, void *);
    uint32_t size=4, data=0xaabbccdd, refs=atomic_load(&((B11Obj *)o)->refs);
    assert(SLOT(o,4,GetPrivateFn)(o,iid_unknown,&size,&data)==B11_DXGI_ERROR_NOT_FOUND);
    assert(size==0 && data==0xaabbccdd);
    assert(SLOT(o,5,SetPrivateFn)(o,iid_unknown,4,&data)==B11_S_OK);
    data=0; size=4;
    assert(SLOT(o,4,GetPrivateFn)(o,iid_unknown,&size,&data)==B11_S_OK);
    assert(size==4 && data==0xaabbccdd);
    assert(SLOT(o,6,SetInterfaceFn)(o,iid_unknown,o)==B11_S_OK);
    assert(atomic_load(&((B11Obj *)o)->refs)==refs+1);
    void *ptr=NULL; size=sizeof(ptr);
    assert(SLOT(o,4,GetPrivateFn)(o,iid_unknown,&size,&ptr)==B11_S_OK && ptr==o);
    assert(atomic_load(&((B11Obj *)o)->refs)==refs+2);
    SLOT(ptr,2,RefFn)(ptr);
    assert(SLOT(o,5,SetPrivateFn)(o,iid_unknown,0,NULL)==B11_S_OK);
    assert(atomic_load(&((B11Obj *)o)->refs)==refs);
}
static void *boundary_desc(const void *desc, size_t size, void **mapping)
{
    long page=sysconf(_SC_PAGESIZE);
    uint8_t *p=mmap(NULL,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(p!=MAP_FAILED && !mprotect(p+page,page,PROT_NONE));
    memcpy(p+page-size,desc,size); *mapping=p; return p+page-size;
}
static void unmap_desc(void *p) { assert(!munmap(p,sysconf(_SC_PAGESIZE)*2)); }
static void raster_failure(B11Device *d, B11RasterizerDesc1 *desc, B11HR expected)
{
    unsigned a=allocations,h=heaps,w=writes; uint32_t refs=atomic_load(&d->obj.refs);
    void *o=(void *)1; assert(raster1(d,desc,&o)==expected && !o);
    assert(raster1(d,desc,NULL)==expected);
    assert(a==allocations && h==heaps && w==writes && refs==atomic_load(&d->obj.refs));
}
static void test_raster(B11Device *d)
{
    B11RasterizerDesc1 desc={.fill_mode=3,.cull_mode=3,.depth_clip=1,
        .depth_bias=-17,.depth_bias_clamp=-1.5f,.slope_scaled_depth_bias=2.25f,.scissor=1};
    unsigned a=allocations;
    assert(raster1(d,&desc,NULL)==B11_S_FALSE && a==allocations);
    void *map,*o=NULL; const B11RasterizerDesc1 *p=boundary_desc(&desc,44,&map);
    assert(raster1(d,p,&o)==B11_S_OK && o); unmap_desc(map);
    for (unsigned i=0;i<9;++i) assert((*(void ***)o)[i]);
    private_roundtrip(o);
    qi_same(o,iid_raster1); qi_same(o,iid_raster); qi_same(o,iid_child); qi_same(o,iid_unknown);
    qi_no(o,iid_srv1); qi_no(o,iid_resource); qi_no(o,iid_device3); qi_no(o,iid_raster2);
    get_exact(o,7,&desc,40); get_exact(o,8,&desc,44);
    void *device=NULL; SLOT(o,3,GetFn)(o,&device); assert(device==d); release(device);
    void *base=NULL; p=boundary_desc(&desc,40,&map);
    assert(SLOT(d,22,StateFn)(d,p,&base)==B11_S_OK); unmap_desc(map);
    assert(((B11State *)base)->hash==((B11State *)o)->hash);
    qi_no(base,iid_raster1); get_exact(base,7,&desc,40); release(base); release(o);
    assert(atomic_load(&d->obj.refs)==1);
    const uint32_t valid[]={1,2,4,8,16};
    for(size_t i=0;i<sizeof(valid)/sizeof(*valid);++i) {
        desc.forced_sample_count=valid[i]; raster_failure(d,&desc,B11_E_NOTIMPL);
    }
    const uint32_t invalid[]={3,5,15,32,UINT32_MAX};
    for(size_t i=0;i<sizeof(invalid)/sizeof(*invalid);++i) {
        desc.forced_sample_count=invalid[i]; raster_failure(d,&desc,B11_E_INVALIDARG);
    }
    desc.forced_sample_count=0;
    desc.multisample=1; raster_failure(d,&desc,B11_E_NOTIMPL); desc.multisample=0;
    desc.antialiased_line=1; raster_failure(d,&desc,B11_E_NOTIMPL); desc.antialiased_line=0;
    desc.fill_mode=0; raster_failure(d,&desc,B11_E_INVALIDARG); desc.fill_mode=3;
    desc.cull_mode=4; raster_failure(d,&desc,B11_E_INVALIDARG); desc.cull_mode=3;
    desc.depth_bias_clamp=NAN; raster_failure(d,&desc,B11_E_INVALIDARG); desc.depth_bias_clamp=0;
    desc.slope_scaled_depth_bias=INFINITY; raster_failure(d,&desc,B11_E_INVALIDARG); desc.slope_scaled_depth_bias=0;
    desc.depth_clip=-1; raster_failure(d,&desc,B11_E_INVALIDARG); desc.depth_clip=1;
    desc.front_ccw=2; raster_failure(d,&desc,B11_E_INVALIDARG); desc.front_ccw=0;
    desc.scissor=2; raster_failure(d,&desc,B11_E_INVALIDARG); desc.scissor=1;
    desc.multisample=2; raster_failure(d,&desc,B11_E_INVALIDARG); desc.multisample=0;
    desc.antialiased_line=-1; raster_failure(d,&desc,B11_E_INVALIDARG); desc.antialiased_line=0;
    raster_failure(d,NULL,B11_E_INVALIDARG);
    desc.fill_mode=2; desc.cull_mode=1;
    assert(raster1(d,&desc,&o)==B11_S_OK); get_exact(o,8,&desc,44); release(o);
    fail_size=sizeof(B11State); o=(void *)1;
    assert(raster1(d,&desc,&o)==B11_E_OUTOFMEMORY && !o); fail_size=0;
    assert(atomic_load(&d->obj.refs)==1);
}
static B11Texture *texture(B11Device *d, uint32_t dimension, uint32_t format,
                            uint32_t layers, uint32_t samples, uint32_t misc)
{
    B11TextureDesc td={.dimension=dimension,.width=16,.height=16,.depth=8,.mips=samples>1?1:4,
        .array_size=layers,.format=format,.samples=samples,.bind=B11_BIND_SRV,.misc=misc};
    void *o=NULL; assert(b11_texture_create(d,&td,NULL,&o)==B11_S_OK); return o;
}
static B11ShaderResourceViewDesc1 view_desc(uint32_t format, uint32_t dim,
    uint32_t mip,uint32_t mips,uint32_t layer,uint32_t layers,uint32_t plane)
{
    B11ShaderResourceViewDesc1 d={.format=format,.dimension=dim,.u.words={mip,mips,layer,layers,plane}};
    return d;
}
static void srv_failure(B11Device *d, B11Obj *r, B11ShaderResourceViewDesc1 *desc, B11HR expected)
{
    unsigned a=allocations,h=heaps,w=writes,b=buffer_writes;
    uint32_t refs=r?atomic_load(&r->refs):0,dr=atomic_load(&d->obj.refs);
    void *o=(void *)1; assert(srv1(d,r,desc,&o)==expected && !o);
    assert(srv1(d,r,desc,NULL)==expected);
    assert(a==allocations && h==heaps && w==writes && b==buffer_writes);
    assert((!r || refs==atomic_load(&r->refs)) && dr==atomic_load(&d->obj.refs));
}
static void test_srv(B11Device *d)
{
    B11Texture *t=texture(d,2,28,1,1,0);
    B11ShaderResourceViewDesc1 desc=view_desc(28,4,1,UINT32_MAX,0,0,0),normalized=desc;
    normalized.u.words[1]=3;
    unsigned a=allocations,w=writes; assert(srv1(d,&t->obj,&desc,NULL)==B11_S_FALSE);
    assert(a==allocations && w==writes);
    void *map,*o=NULL; const B11ShaderResourceViewDesc1 *p=boundary_desc(&desc,28,&map);
    assert(srv1(d,&t->obj,p,&o)==B11_S_OK); unmap_desc(map);
    assert(last.image==t->image && last.type==VK_IMAGE_VIEW_TYPE_2D && last.format==VK_FORMAT_R8G8B8A8_UNORM);
    assert(last.mip==1 && last.mips==3 && last.layer==0 && last.layers==1);
    for(unsigned i=0;i<10;++i) assert((*(void ***)o)[i]);
    private_roundtrip(o);
    qi_same(o,iid_srv1); qi_same(o,iid_srv); qi_same(o,iid_child); qi_same(o,iid_unknown);
    qi_no(o,iid_raster1); qi_no(o,iid_resource); qi_no(o,iid_device3);
    get_exact(o,8,&normalized,24); get_exact(o,9,&normalized,28);
    void *device=NULL; SLOT(o,3,GetFn)(o,&device); assert(device==d); release(device);
    void *r=NULL; SLOT(o,7,GetFn)(o,&r); assert(r==t && atomic_load(&t->obj.refs)==3);
    qi_same(r,iid_resource); release(t); release(o);
    assert(atomic_load(&((B11Obj *)r)->refs)==1); release(r); b11_reap(d);
    assert(atomic_load(&d->obj.refs)==1);

    t=texture(d,2,28,3,1,0);
    assert(srv1(d,&t->obj,NULL,&o)==B11_S_OK);
    normalized=view_desc(28,5,0,4,0,3,0);
    get_exact(o,8,&normalized,24); get_exact(o,9,&normalized,28); release(o);
    desc=view_desc(0,5,2,UINT32_MAX,1,UINT32_MAX,0); normalized=desc;
    normalized.format=28; normalized.u.words[1]=2; normalized.u.words[3]=2;
    assert(srv1(d,&t->obj,&desc,&o)==B11_S_OK);
    assert(last.mip==2 && last.mips==2 && last.layer==1 && last.layers==2);
    get_exact(o,8,&normalized,24); get_exact(o,9,&normalized,28); release(o);
    desc.u.texture2d_array.plane=1; srv_failure(d,&t->obj,&desc,B11_E_NOTIMPL); desc.u.words[4]=0;
    desc.u.words[0]=4; srv_failure(d,&t->obj,&desc,B11_E_INVALIDARG); desc.u.words[0]=2;
    desc.u.words[1]=0; srv_failure(d,&t->obj,&desc,B11_E_INVALIDARG);
    desc.u.words[1]=UINT32_MAX-1; srv_failure(d,&t->obj,&desc,B11_E_INVALIDARG); desc.u.words[1]=2;
    desc.u.words[2]=UINT32_MAX; srv_failure(d,&t->obj,&desc,B11_E_INVALIDARG); desc.u.words[2]=1;
    desc.u.words[3]=UINT32_MAX-1; srv_failure(d,&t->obj,&desc,B11_E_INVALIDARG); desc.u.words[3]=2;
    desc.dimension=4; srv_failure(d,&t->obj,&desc,B11_E_INVALIDARG); desc.dimension=5;
    desc.format=41; srv_failure(d,&t->obj,&desc,B11_E_INVALIDARG); desc.format=27;
    srv_failure(d,&t->obj,&desc,B11_E_INVALIDARG); desc.format=28;
    t->desc.bind=0; srv_failure(d,&t->obj,&desc,B11_E_INVALIDARG); t->desc.bind=B11_BIND_SRV;
    B11Device foreign; init_device(&foreign);
    srv_failure(&foreign,&t->obj,&desc,B11_E_INVALIDARG);
    assert(!pthread_mutex_destroy(&foreign.grave_lock));
    srv_failure(d,NULL,&desc,B11_E_INVALIDARG);
    B11Obj alien={.dev=d,.kind=B11_TEXTURE,.vtable=g_b11_device_vtable}; atomic_init(&alien.refs,1);
    srv_failure(d,&alien,&desc,B11_E_INVALIDARG);
    fail_size=sizeof(B11View); o=(void *)1;
    unsigned before_writes=writes,before_heaps=heaps;
    assert(srv1(d,&t->obj,&desc,&o)==B11_E_OUTOFMEMORY && !o); fail_size=0;
    assert(writes==before_writes && heaps==before_heaps && atomic_load(&t->obj.refs)==1);
    fail_heap=1; o=(void *)1;
    assert(srv1(d,&t->obj,&desc,&o)==B11_E_OUTOFMEMORY && !o); fail_heap=0;
    assert(writes==before_writes && atomic_load(&t->obj.refs)==1 && atomic_load(&d->obj.refs)==1);
    unsigned hf=heap_frees; fail_write=1; o=(void *)1;
    assert(srv1(d,&t->obj,&desc,&o)==B11_E_FAIL && !o); fail_write=0; b11_reap(d);
    assert(heap_frees>hf && atomic_load(&t->obj.refs)==1 && atomic_load(&d->obj.refs)==1);

    /* Actual legacy device slot7 with a 24-byte page-boundary input. */
    uint32_t base_desc[6]={28,5,0,4,0,3}; p=boundary_desc(base_desc,24,&map);
    assert(SLOT(d,7,ViewFn)(d,&t->obj,(const uint32_t *)p,&o)==B11_S_OK); unmap_desc(map);
    get_exact(o,8,base_desc,24); qi_no(o,iid_srv1);
    /* The old shared vtable stays non-extended for every kind using it. */
    for(uint32_t kind=B11_UAV;kind<=B11_DSV;++kind) {
        ((B11Obj *)o)->kind=kind; qi_no(o,iid_srv1); qi_no(o,iid_raster1);
    }
    ((B11Obj *)o)->kind=B11_SRV; release(o); release(t); b11_reap(d);
    t=texture(d,2,28,1,1,0); desc=view_desc(28,4,0,4,0,0,0);
    assert(srv1(d,&t->obj,NULL,&o)==B11_S_OK);
    get_exact(o,9,&desc,28); get_exact(o,8,&desc,24); release(o);
    /* Texture2D plane is DWORD4, NOT DWORD6. */
    desc.u.texture2d.plane=1; srv_failure(d,&t->obj,&desc,B11_E_NOTIMPL);
    desc.u.texture2d.plane=0; desc.u.words[4]=0xabcdef01;
    assert(srv1(d,&t->obj,&desc,&o)==B11_S_OK); get_exact(o,9,&desc,28); release(o);
    t->desc.format=103; desc.format=61; srv_failure(d,&t->obj,&desc,B11_E_NOTIMPL);
    t->desc.format=19; desc.format=21; srv_failure(d,&t->obj,&desc,B11_E_NOTIMPL);
    t->desc.format=44; desc.format=47; srv_failure(d,&t->obj,&desc,B11_E_NOTIMPL);
    t->desc.format=28; desc.format=28; t->image->aspect=VK_IMAGE_ASPECT_DEPTH_BIT;
    srv_failure(d,&t->obj,&desc,B11_E_NOTIMPL); release(t); b11_reap(d);
    t=texture(d,2,27,1,1,0); desc=view_desc(30,4,0,4,0,0,0);
    assert(srv1(d,&t->obj,&desc,&o)==B11_S_OK && last.format==VK_FORMAT_R8G8B8A8_UINT);
    release(o); srv_failure(d,&t->obj,NULL,B11_E_INVALIDARG); release(t); b11_reap(d);
}
static void test_dimensions(B11Device *d)
{
    const struct { uint32_t resource_dim,layers,samples,misc,view_dim; VkImageViewType type; } cases[]={
        {1,1,1,0,2,VK_IMAGE_VIEW_TYPE_1D},{1,3,1,0,3,VK_IMAGE_VIEW_TYPE_1D_ARRAY},
        {3,1,1,0,8,VK_IMAGE_VIEW_TYPE_3D},{2,6,1,B11_MISC_CUBE,9,VK_IMAGE_VIEW_TYPE_CUBE},
        {2,12,1,B11_MISC_CUBE,10,VK_IMAGE_VIEW_TYPE_CUBE_ARRAY},
        {2,1,4,0,6,VK_IMAGE_VIEW_TYPE_2D},{2,3,4,0,7,VK_IMAGE_VIEW_TYPE_2D_ARRAY}
    };
    for(size_t i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        B11Texture *t=texture(d,cases[i].resource_dim,28,cases[i].layers,cases[i].samples,cases[i].misc);
        void *o=NULL; assert(srv1(d,&t->obj,NULL,&o)==B11_S_OK);
        assert(((B11View *)o)->dimension==cases[i].view_dim && last.type==cases[i].type);
        assert(last.mips==(cases[i].samples>1?1:4)); release(o);
        B11ShaderResourceViewDesc1 desc=view_desc(28,cases[i].view_dim,0,4,0,1,0);
        if(desc.dimension==6) {
            /* Only UnusedField_NothingToDefine exists in the MS union. Never parse it as mip. */
            desc.u.words[0]=0x12345678; desc.u.words[1]=0;
            assert(srv1(d,&t->obj,&desc,&o)==B11_S_OK && last.mip==0 && last.mips==1);
            get_exact(o,9,&desc,28); release(o);
        }
        if(desc.dimension==10) {
            desc.u.words[2]=1; srv_failure(d,&t->obj,&desc,B11_E_INVALIDARG);
            desc.u.words[2]=6; desc.u.words[3]=UINT32_MAX-1;
            srv_failure(d,&t->obj,&desc,B11_E_INVALIDARG);
        }
        desc.dimension=11; srv_failure(d,&t->obj,&desc,B11_E_INVALIDARG);
        release(t); b11_reap(d);
    }
}
static void test_buffer(B11Device *d)
{
    B11BufferDesc bd={.byte_width=100,.bind=B11_BIND_SRV}; void *b=NULL,*o=NULL;
    assert(SLOT(d,3,BufferFn)(d,&bd,NULL,&b)==B11_S_OK);
    B11ShaderResourceViewDesc1 desc=view_desc(42,1,1,24,0,0,0);
    assert(srv1(d,b,&desc,&o)==B11_S_OK && last_offset==4 && last_range==96);
    get_exact(o,8,&desc,24); get_exact(o,9,&desc,28); release(o);
    desc.u.words[1]=25; srv_failure(d,b,&desc,B11_E_INVALIDARG); /* padded backing must not enlarge view */
    desc.u.words[0]=UINT32_MAX; srv_failure(d,b,&desc,B11_E_INVALIDARG);
    desc.u.words[0]=0; desc.u.words[1]=1; desc.dimension=4; srv_failure(d,b,&desc,B11_E_INVALIDARG);
    desc.dimension=11; desc.u.words[2]=2; srv_failure(d,b,&desc,B11_E_INVALIDARG);
    desc.u.words[2]=1; desc.format=39; srv_failure(d,b,&desc,B11_E_INVALIDARG);
    ((B11Buffer *)b)->desc.misc=B11_MISC_RAW; srv_failure(d,b,&desc,B11_E_NOTIMPL);
    ((B11Buffer *)b)->desc.misc=B11_MISC_STRUCTURED; ((B11Buffer *)b)->desc.stride=4;
    desc.dimension=1; desc.format=0; desc.u.words[2]=0; srv_failure(d,b,&desc,B11_E_NOTIMPL);
    srv_failure(d,b,NULL,B11_E_NOTIMPL); release(b); b11_reap(d);
}
int main(void)
{
    b11_init_vtables(); B11Device d; init_device(&d);
    assert(sizeof(g_b11_device_vtable)/sizeof(void *)==43); qi_no(&d,iid_device3);
    test_raster(&d); puts("extended raster: ABI/9 slots/QI/44+40 bytes/validation/rollback PASS");
    test_srv(&d); puts("extended SRV: ABI/10 slots/QI/lifetime/defaults/28+24 bytes/guard pages/rollback/base regression PASS");
    test_dimensions(&d); test_buffer(&d);
    assert(!d.grave_head && atomic_load(&d.obj.refs)==1 && images==image_frees);
    assert(heaps==heap_frees+1); /* exactly one deliberately failed heap allocation */
    assert(!pthread_mutex_destroy(&d.grave_lock));
    printf("extended objects: color dimensions/typed buffers/unsupported rejection PASS (image writers=%u buffer writers=%u)\n",writes,buffer_writes);
    return 0;
}