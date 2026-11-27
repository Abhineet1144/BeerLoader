/* Real Win64 loader wrappers + real COM tables, with a local CPU constructor
 * injected into the production cache helper. No GPU or guest program needed. */
#define main beer_loader_unused_main
#include "../loader.c"
#undef main
#include "../graphics/d3d11v/bd11_device_cache.h"
#include <assert.h>

static B11DeviceCache cache = B11_DEVICE_CACHE_INIT;
static unsigned constructions, config_calls;
static uint32_t last_flags, last_level;
static int fail_construct, fail_swap;
static void fixture_device_destroy(B11Obj *o) { free(o); }
static B11Device *cpu_construct(uint32_t flags, uint32_t level)
{
    ++constructions;
    if (fail_construct) return NULL;
    B11Device *dev = calloc(1, sizeof(*dev));
    assert(dev);
    dev->obj = (B11Obj){.vtable = g_b11_device_vtable, .refs = 1,
        .kind = B11_DEVICE, .dev = dev, .destroy = fixture_device_destroy};
    dev->flags = flags;
    dev->feature_level = level;
    dev->immediate = b11_context_create(dev, 0, 0);
    assert(dev->immediate);
    b11_multithread_init(&dev->multithread, &dev->obj);
    return dev;
}

int32_t __wrap_b11_device_create_for_loader_config(uint32_t flags, uint32_t level,
                                                  void **dev, void **ctx)
{
    ++config_calls;
    last_flags = flags;
    last_level = level;
    return b11_device_cache_get(&cache, flags, level, cpu_construct, dev, ctx);
}
BD12Device *__wrap_bd12_device_create(void)
{
    assert(!"CPU contract test must not initialize a GPU");
    return NULL;
}
static void swap_destroy(B11Obj *o)
{
    b11_release(&o->dev->obj);
    free(o);
}
int32_t __wrap_b11_swapchain_create_for_loader(void *device, const void *desc, void **out)
{
    assert(device && desc && out);
    *out = NULL;
    if (fail_swap) return B11_E_FAIL;
    B11Obj *swap = calloc(1, sizeof(*swap));
    assert(swap);
    swap->vtable = g_b11_device_vtable; /* only real AddRef/Release used */
    atomic_init(&swap->refs, 1);
    swap->dev = device;
    swap->destroy = swap_destroy;
    b11_add_ref(&swap->dev->obj);
    *out = swap;
    return B11_S_OK;
}
typedef uint32_t (B11_MS *GetUintFn)(void *);
static uint32_t get_uint(void *o, unsigned slot)
{ return ((GetUintFn)(*(void ***)o)[slot])(o); }
static void check_baseline(void)
{
    for (B11DeviceCacheEntry *e = cache.head; e; e = e->next) {
        assert(atomic_load(&e->dev->obj.refs) == 2); /* process + context */
        assert(atomic_load(&e->dev->immediate->obj.refs) == 1);
    }
}
static void cache_contract(void)
{
    void *d = (void *)1, *c = (void *)1;
    const uint32_t unsupported[] = {2, 0x22, 4, 8, 0x10, 0x40, 0x80, 0x100, 0x80000000};
    for (unsigned i = 0; i < sizeof(unsupported)/sizeof(*unsupported); ++i) {
        B11HR expected = unsupported[i] & 2 ? B11_DXGI_ERROR_SDK_COMPONENT_MISSING : B11_E_INVALIDARG;
        assert(b11_device_cache_get(&cache, unsupported[i], 0xb000, cpu_construct, &d, &c) == expected);
        assert(!d && !c && !constructions);
    }
    fail_construct = 1;
    assert(b11_device_cache_get(&cache, 0, 0xb000, cpu_construct, &d, &c) == B11_E_FAIL);
    assert(!cache.head && !d && !c);
    fail_construct = 0;
    const uint32_t flags[] = {0, 1, 0x20, 0x21};
    for (unsigned i = 0; i < 4; ++i) {
        assert(!b11_device_cache_get(&cache, flags[i], 0xb000, cpu_construct, &d, &c));
        assert(get_uint(d, 38) == flags[i] && get_uint(d, 37) == 0xb000);
        void *same = NULL;
        assert(!b11_device_cache_get(&cache, flags[i], 0xb000, cpu_construct, &same, NULL));
        assert(same == d);
        b11_release(same); b11_release(d); b11_release(c);
    }
    assert(constructions == 5);
    check_baseline();
}
static void *cache_worker(void *unused)
{
    (void)unused;
    for (unsigned i = 0; i < 200; ++i) {
        void *d, *c;
        assert(!b11_device_cache_get(&cache, 0x21, 0xa000, cpu_construct, &d, &c));
        assert(get_uint(d, 38) == 0x21 && get_uint(d, 37) == 0xa000);
        b11_release(d); b11_release(c);
    }
    return NULL;
}
static void wrapper_contract(void)
{
    unsetenv("BEER_D3D11_LEGACY");
    unsetenv("BEER_FAKE_CAPS");
    beer_fake_caps_state = 1; /* select existing policy mode only in the fixture */
    u64 *old = NULL, *ctx = NULL;
    u32 level = 0;
    assert(!impl_D3D11CreateDevice(0, 1, 0, 0, NULL, 0, 7, &old, &level, &ctx));
    assert(level == 0xb000 && last_flags == 0 && last_level == level);
    assert(!impl_D3D11CreateDevice(0, 1, 0, 0x20, NULL, 0, 7, NULL, &level, NULL));
    assert(last_flags == 0x20 && get_uint(old, 38) == 0);
    b11_release((B11Obj *)old); b11_release((B11Obj *)ctx);
    for (unsigned outputs = 0; outputs < 4; ++outputs) {
        old = ctx = NULL;
        assert(!impl_D3D11CreateDevice(0, 1, 0, 0x21, NULL, 0, 7,
            outputs & 1 ? &old : NULL, &level, outputs & 2 ? &ctx : NULL));
        if (old) b11_release((B11Obj *)old);
        if (ctx) b11_release((B11Obj *)ctx);
        check_baseline();
    }
    /* Existing opt-in feature-level policy is not corrected here; ensure later
     * grants cannot overwrite previously published configuration. */
    /* Both the policy and getenv call-site are cached. Select the already
     * existing testing mode directly; production environment handling is intact. */
    beer_fake_caps_state = 2;
    u32 requested = 0xc100;
    assert(!impl_D3D11CreateDevice(0, 1, 0, 0x20, &requested, 1, 7, &old, &level, NULL));
    assert(level == 0xc100 && last_level == level && get_uint(old, 37) == level);
    beer_fake_caps_state = 1;
    assert(!impl_D3D11CreateDevice(0, 1, 0, 0x20, NULL, 0, 7, &ctx, &level, NULL));
    assert(ctx != old && get_uint(old, 37) == 0xc100 && get_uint(ctx, 37) == 0xb000);
    b11_release((B11Obj *)old); b11_release((B11Obj *)ctx);
    u64 desc[9] = {0}, *swap = NULL;
    for (unsigned failure = 0; failure < 2; ++failure) {
        fail_swap = failure;
        for (unsigned outputs = 0; outputs < 4; ++outputs) {
            old = ctx = swap = (void *)1;
            level = 0xfeed;
            u64 hr = impl_D3D11CreateDeviceAndSwapChain(0, 1, 0, 0x20, NULL, 0, 7,
                desc, &swap, outputs & 1 ? &old : NULL, &level, outputs & 2 ? &ctx : NULL);
            assert(last_flags == 0x20 && last_level == 0xb000);
            if (failure) {
                assert(hr == (uint32_t)B11_E_FAIL && !swap && level == 0xfeed);
                if (outputs & 1) assert(!old);
                if (outputs & 2) assert(!ctx);
            } else {
                assert(!hr && swap && level == 0xb000);
                if (outputs & 1) b11_release((B11Obj *)old);
                if (outputs & 2) b11_release((B11Obj *)ctx);
                b11_release((B11Obj *)swap);
            }
            check_baseline();
        }
    }
    for (unsigned with_swap = 0; with_swap < 2; ++with_swap) {
        old = ctx = swap = (void *)1;
        level = 0xfeed;
        u64 hr = with_swap ? impl_D3D11CreateDeviceAndSwapChain(0, 1, 0, 2, NULL, 0, 7,
            desc, &swap, &old, &level, &ctx) :
            impl_D3D11CreateDevice(0, 1, 0, 2, NULL, 0, 7, &old, &level, &ctx);
        assert(hr == (uint32_t)B11_DXGI_ERROR_SDK_COMPONENT_MISSING && !old && !ctx);
        assert(level == 0xfeed && (!with_swap || !swap));
    }
    unsigned calls = config_calls;
    assert(impl_D3D11CreateDeviceAndSwapChain(0, 1, 0, 0, NULL, 0, 7,
        NULL, &swap, &old, &level, &ctx) == (uint32_t)B11_E_INVALIDARG);
    assert(impl_D3D11CreateDeviceAndSwapChain(0, 1, 0, 0, NULL, 0, 7,
        desc, NULL, &old, &level, &ctx) == (uint32_t)B11_E_INVALIDARG);
    assert(config_calls == calls);
    check_baseline();
}
int32_t __real_b11_device_create_for_loader_config(uint32_t, uint32_t, void **, void **);
int main(void)
{
    b11_init_vtables();
    void *d = (void *)1, *c = (void *)1;
    assert(__real_b11_device_create_for_loader_config(2, 0xb000, &d, &c) == B11_DXGI_ERROR_SDK_COMPONENT_MISSING);
    assert(!d && !c);
    assert(__real_b11_device_create_for_loader_config(0x40, 0xb000, &d, &c) == B11_E_INVALIDARG);
    assert(!b11_device_create_configured(2, 0xb000));
    cache_contract();
    pthread_t workers[8];
    for (unsigned i = 0; i < 8; ++i) assert(!pthread_create(&workers[i], NULL, cache_worker, NULL));
    for (unsigned i = 0; i < 8; ++i) assert(!pthread_join(workers[i], NULL));
    assert(constructions == 6); /* concurrent first publication constructs once */
    check_baseline();
    wrapper_contract();
    /* Device3/Context3 are still refused; no incomplete interface upgrade. */
    const uint8_t device3[16] = {0x37,0x8c,0x5c,0xa0,0xc6,0xd2,0x32,0x47,0xb3,0xa0,0x9c,0xe0,0xb0,0xdc,0x9a,0xe6};
    const uint8_t context3[16] = {0x1d,0xc0,0xe3,0xb4,0x9e,0xe7,0x37,0x46,0x91,0xb2,0x51,0x0e,0x9f,0x4c,0x9b,0x8f};
    assert(b11_query_interface(&cache.head->dev->obj, device3, &d) == B11_E_NOINTERFACE && !d);
    assert(b11_query_interface(&cache.head->dev->immediate->obj, context3, &c) == B11_E_NOINTERFACE && !c);
    while (cache.head) {
        B11DeviceCacheEntry *e = cache.head;
        cache.head = e->next;
        b11_release(&e->dev->immediate->obj);
        b11_release(&e->dev->obj);
        free(e);
    }
    pthread_mutex_destroy(&cache.lock);
    puts("BD11 immutable configuration / guest creation ownership: PASS (CPU-only)");
    return 0;
}