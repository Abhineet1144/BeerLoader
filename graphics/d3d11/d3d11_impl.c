#include "guest_cpu.h"
#include "d3d11_compat.h"
#include "../../platform/xwayland_backend.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>

#include "../renderer/vulkan/vulkan_renderer.h"
#include "../renderer/vulkan/vulkan_indexed_renderer.h"
#include "d3d12_shader_translator.h"
#include "vulkan_graphics.h"

static BeerD3D11Renderer g_renderer = BEER_D3D11_RENDERER_VULKAN;
static _Atomic(uint32_t) g_vulkan_migration_draws;
static int render_diagnostics_enabled(void);

int d3d11_set_renderer(BeerD3D11Renderer renderer)
{
    if (renderer != BEER_D3D11_RENDERER_VULKAN)
        return 0;
    g_renderer = renderer;
    return 1;
}

BeerD3D11Renderer d3d11_get_renderer(void)
{
    return g_renderer;
}

const char *d3d11_renderer_name(BeerD3D11Renderer renderer)
{
    return renderer == BEER_D3D11_RENDERER_VULKAN ? "vulkan" : "unknown";
}

static void trace_vulkan_migration_draw(const char *kind,
                                        uint32_t vertex_hash,
                                        uint32_t pixel_hash)
{
    uint32_t draw = atomic_fetch_add(&g_vulkan_migration_draws, 1) + 1;
    if (render_diagnostics_enabled() && (draw <= 8 || (draw % 128) == 0))
        fprintf(stderr,
                "[D3D11 VULKAN] observed draw=%u kind=%s VS=%08x PS=%08x\n",
                draw, kind, vertex_hash, pixel_hash);
}

static void __attribute__((noreturn)) strict_vulkan_failure(const char *operation)
{
    fprintf(stderr,
            "[D3D11 VULKAN FATAL] Vulkan renderer cannot execute %s; "
            "software rendering is not available\n",
            operation ? operation : "operation");
    fflush(stderr);
    abort();
}

/* ============================================================================
 * COM Object Header - Real state tracking
 * ============================================================================ */

typedef enum {
    COM_TYPE_DEVICE = 1,
    COM_TYPE_CONTEXT = 2,
    COM_TYPE_SWAPCHAIN = 3,
    COM_TYPE_RESOURCE = 4,
    COM_TYPE_VIEW = 5,
    COM_TYPE_STATE = 6,
    COM_TYPE_SHADER = 7,
    COM_TYPE_INPUT_LAYOUT = 8,
    COM_TYPE_COMMAND_LIST = 9,
} ComObjectType;

typedef struct {
    uint32_t magic;              /* 0xdeadbeef for validation */
    _Atomic(uint32_t) refcount;  /* Real reference counting */
    ComObjectType type;          /* Object type for validation */
    uint32_t feature_level;      /* D3D feature level if device */
    uint32_t pad;                /* Padding for alignment */
} ComObjectHeader;

#define COM_MAGIC 0xdeadbeef

/* Allocation helpers - allocate header + payload, return payload pointer */
static void* com_alloc(size_t payload_size, ComObjectType type, uint32_t feature_level) {
    size_t total_size = sizeof(ComObjectHeader) + payload_size;
    ComObjectHeader* hdr = (ComObjectHeader*)malloc(total_size);
    if (!hdr) return NULL;
    
    hdr->magic = COM_MAGIC;
    atomic_store(&hdr->refcount, 1);
    hdr->type = type;
    hdr->feature_level = feature_level;
    hdr->pad = 0;
    
    return (void*)((char*)hdr + sizeof(ComObjectHeader));
}

static ComObjectHeader* com_get_header(void* obj) {
    if (!obj) return NULL;
    ComObjectHeader* hdr = (ComObjectHeader*)((char*)obj - sizeof(ComObjectHeader));
    if (hdr->magic != COM_MAGIC) return NULL;
    return hdr;
}

static uint32_t D3D11_MS_ABI com_addref(void* obj) {
    ComObjectHeader* hdr = com_get_header(obj);
    if (!hdr) return 0;
    return atomic_fetch_add(&hdr->refcount, 1) + 1;
}

static uint32_t D3D11_MS_ABI com_release(void* obj) {
    ComObjectHeader* hdr = com_get_header(obj);
    if (!hdr) return 0;
    uint32_t old_count = atomic_fetch_sub(&hdr->refcount, 1);
    if (old_count == 1) {
        /* Actually free when refcount reaches 0 */
        free(hdr);
        return 0;
    }
    return old_count - 1;
}

/* ============================================================================
 * ID3D11Device - Enhanced implementation with real state
 * ============================================================================ */

static int iid_equal(REFIID riid, const uint8_t expected[16]) {
    const void *iid = (const void *)(uintptr_t)riid;
    return iid && memcmp(iid, expected, 16) == 0;
}

typedef struct {
    void **vtable;
    ID3D11Device *device;
    void *adapter;
} BeerDxgiDevice;

static void *g_dxgi_adapter;
static BeerDxgiDevice *g_dxgi_device;
static void *g_dxgi_device_vtable[12];

static HRESULT D3D11_MS_ABI dxgi_device_query_interface(
    BeerDxgiDevice *this, REFIID riid, LPVOID *ppvObj);
static uint32_t D3D11_MS_ABI device_addref(ID3D11Device *this);
static uint32_t D3D11_MS_ABI device_release(ID3D11Device *this);

static uint32_t D3D11_MS_ABI dxgi_device_addref(BeerDxgiDevice *this)
{
    return com_addref(this);
}

static uint32_t D3D11_MS_ABI dxgi_device_release(BeerDxgiDevice *this)
{
    return com_release(this);
}

static void addref_external_com_object(void *object)
{
    if (!object) return;
    void **vtable = *(void ***)object;
    if (!vtable || !vtable[1]) return;
    typedef uint32_t (D3D11_MS_ABI *AddRefFn)(void *);
    BEER_GUEST_CALL(((AddRefFn)vtable[1]), object);
}

static HRESULT D3D11_MS_ABI dxgi_device_get_parent(
    BeerDxgiDevice *this, REFIID riid, void **ppParent)
{
    static const uint8_t iid_idxgiadapter[16] = {
        0xec, 0x66, 0x71, 0x7b, 0xc7, 0x21, 0xae, 0x44,
        0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3, 0x69
    };
    if (!ppParent) return (HRESULT)0x80070057; /* E_INVALIDARG */
    *ppParent = NULL;
    if (!iid_equal(riid, iid_idxgiadapter)) return E_NOINTERFACE;
    if (!this->adapter) return (HRESULT)0x887a0002; /* DXGI_ERROR_NOT_FOUND */
    *ppParent = this->adapter;
    addref_external_com_object(this->adapter);
    return S_OK;
}

static HRESULT D3D11_MS_ABI dxgi_device_get_adapter(
    BeerDxgiDevice *this, void **ppAdapter)
{
    if (!ppAdapter) return (HRESULT)0x80070057; /* E_INVALIDARG */
    *ppAdapter = NULL;
    if (!this->adapter) return (HRESULT)0x887a0002; /* DXGI_ERROR_NOT_FOUND */
    *ppAdapter = this->adapter;
    addref_external_com_object(this->adapter);
    return S_OK;
}

static HRESULT D3D11_MS_ABI dxgi_device_unsupported(
    BeerDxgiDevice *this, void *a, void *b, void *c)
{
    (void)this; (void)a; (void)b; (void)c;
    return (HRESULT)0x80004001; /* E_NOTIMPL */
}

static BeerDxgiDevice *get_dxgi_device(ID3D11Device *device)
{
    if (!g_dxgi_device) {
        g_dxgi_device = com_alloc(sizeof(*g_dxgi_device), COM_TYPE_DEVICE, 0);
        if (!g_dxgi_device) return NULL;
        for (size_t i = 0; i < 12; ++i)
            g_dxgi_device_vtable[i] = (void *)dxgi_device_unsupported;
        g_dxgi_device_vtable[0] = (void *)dxgi_device_query_interface;
        g_dxgi_device_vtable[1] = (void *)dxgi_device_addref;
        g_dxgi_device_vtable[2] = (void *)dxgi_device_release;
        g_dxgi_device_vtable[6] = (void *)dxgi_device_get_parent;
        g_dxgi_device_vtable[7] = (void *)dxgi_device_get_adapter;
        g_dxgi_device->vtable = g_dxgi_device_vtable;
    }
    g_dxgi_device->device = device;
    g_dxgi_device->adapter = g_dxgi_adapter;
    return g_dxgi_device;
}

static HRESULT D3D11_MS_ABI device_query_interface(ID3D11Device* this, REFIID riid, LPVOID* ppvObj) {
    /* A D3D11 device exposes ID3D11Device and its documented IDXGIDevice
     * parent. Resource interfaces remain unsupported. */
    static const uint8_t iid_iunknown[16] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46
    };
    static const uint8_t iid_id3d11device[16] = {
        0xdb, 0x6d, 0x6f, 0xdb, 0x77, 0xac, 0x88, 0x4e,
        0x82, 0x53, 0x81, 0x9d, 0xf9, 0xbb, 0xf1, 0x40
    };
    static const uint8_t iid_idxgidevice[16] = {
        0xfa, 0x77, 0xec, 0x54, 0x77, 0x13, 0xe6, 0x44,
        0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c
    };

    if (!ppvObj) return (HRESULT)0x80070057;
    *ppvObj = NULL;
    if (iid_equal(riid, iid_idxgidevice)) {
        BeerDxgiDevice *dxgi = get_dxgi_device(this);
        if (!dxgi) return (HRESULT)0x8007000e;
        *ppvObj = dxgi;
        com_addref(dxgi);
        return S_OK;
    }
    if (!iid_equal(riid, iid_iunknown) && !iid_equal(riid, iid_id3d11device))
        return E_NOINTERFACE;
    *ppvObj = this;
    com_addref(this);
    return S_OK;
}

static HRESULT D3D11_MS_ABI dxgi_device_query_interface(
    BeerDxgiDevice *this, REFIID riid, LPVOID *ppvObj)
{
    static const uint8_t iid_iunknown[16] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46
    };
    static const uint8_t iid_idxgidevice[16] = {
        0xfa, 0x77, 0xec, 0x54, 0x77, 0x13, 0xe6, 0x44,
        0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c
    };
    if (!ppvObj) return (HRESULT)0x80070057;
    *ppvObj = NULL;
    if (iid_equal(riid, iid_iunknown) || iid_equal(riid, iid_idxgidevice)) {
        *ppvObj = this;
        com_addref(this);
        return S_OK;
    }
    return this->device
        ? device_query_interface(this->device, riid, ppvObj)
        : E_NOINTERFACE;
}

void d3d11_device_set_dxgi_adapter(void *adapter)
{
    g_dxgi_adapter = adapter;
    if (g_dxgi_device) g_dxgi_device->adapter = adapter;
}

static uint32_t D3D11_MS_ABI device_addref(ID3D11Device* this) {
    return com_addref(this);
}

static uint32_t D3D11_MS_ABI device_release(ID3D11Device* this) {
    return com_release(this);
}

static HRESULT D3D11_MS_ABI device_create_swapchain(ID3D11Device* this, void* pFactory, void* pDesc, void** ppSwapChain) {
    if (!ppSwapChain) return E_NOINTERFACE;
    *ppSwapChain = dxgi_swapchain_create();
    return S_OK;
}

typedef enum {
    BEER_VIEW_SHADER_RESOURCE,
    BEER_VIEW_UNORDERED_ACCESS,
    BEER_VIEW_RENDER_TARGET,
    BEER_VIEW_DEPTH_STENCIL,
} BeerD3D11ViewKind;

typedef struct {
    void **vtable;
    ID3D11Device *device;
    void *resource;
    BeerD3D11ViewKind kind;
    uint32_t desc_size;
    uint8_t desc[24];
} BeerD3D11View;

static void *g_view_vtable[9];

static const uint8_t *view_interface_iid(BeerD3D11ViewKind kind)
{
    static const uint8_t iids[][16] = {
        {0xe0,0x6f,0xe0,0xb0,0x92,0x81,0x1a,0x4e,0xb1,0xca,0x36,0xd7,0x41,0x47,0x10,0xb2},
        {0x09,0xf5,0xac,0x28,0x5c,0x7f,0xf6,0x48,0x86,0x11,0xf3,0x16,0x01,0x0a,0x63,0x80},
        {0x67,0xa0,0xdb,0xdf,0x8d,0x0b,0x65,0x48,0x87,0x5b,0xd7,0xb4,0x51,0x6c,0xc1,0x64},
        {0x2a,0xc9,0xda,0x9f,0x76,0x18,0xc3,0x48,0xaf,0xad,0x25,0xb9,0x4f,0x84,0xa9,0xb6},
    };
    return iids[kind];
}

static HRESULT D3D11_MS_ABI view_query_interface(
    BeerD3D11View *this, REFIID riid, LPVOID *out)
{
    static const uint8_t iid_iunknown[16] = {
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xc0,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x46
    };
    static const uint8_t iid_view[16] = {
        0x16,0x12,0x9d,0x83,0x2e,0xbb,0x2b,0x41,
        0xb7,0xf4,0xa9,0xdb,0xeb,0xe0,0x8e,0xd1
    };
    if (!out) return (HRESULT)0x80070057;
    *out = NULL;
    if (!iid_equal(riid, iid_iunknown) && !iid_equal(riid, iid_view) &&
        !iid_equal(riid, view_interface_iid(this->kind)))
        return E_NOINTERFACE;
    *out = this;
    com_addref(this);
    return S_OK;
}

static void D3D11_MS_ABI view_get_device(
    BeerD3D11View *this, ID3D11Device **device)
{
    if (!device) return;
    *device = this->device;
    if (this->device) device_addref(this->device);
}

static HRESULT D3D11_MS_ABI view_private_data_unsupported(
    BeerD3D11View *this, void *a, void *b, void *c)
{
    (void)this; (void)a; (void)b; (void)c;
    return (HRESULT)0x80004001;
}

static void D3D11_MS_ABI view_get_resource(
    BeerD3D11View *this, void **resource)
{
    if (!resource) return;
    *resource = this->resource;
    addref_external_com_object(this->resource);
}

static void D3D11_MS_ABI view_get_desc(BeerD3D11View *this, void *desc)
{
    if (desc) memcpy(desc, this->desc, this->desc_size);
}

static uint32_t D3D11_MS_ABI view_release(BeerD3D11View *this)
{
    ComObjectHeader *header = com_get_header(this);
    if (!header) return 0;
    uint32_t old_count = atomic_fetch_sub(&header->refcount, 1);
    if (old_count == 1) {
        if (this->resource) {
            void **vtable = *(void ***)this->resource;
            if (vtable && vtable[2]) {
                typedef uint32_t (D3D11_MS_ABI *ReleaseFn)(void *);
                BEER_GUEST_CALL(((ReleaseFn)vtable[2]), this->resource);
            }
        }
        free(header);
        return 0;
    }
    return old_count - 1;
}

static void initialize_default_view_desc(BeerD3D11View *view);

static BeerD3D11View *view_create(ID3D11Device *device, void *resource,
    const void *desc, BeerD3D11ViewKind kind, uint32_t desc_size)
{
    if (!device || !resource || desc_size > sizeof(((BeerD3D11View *)0)->desc))
        return NULL;
    if (!g_view_vtable[0]) {
        g_view_vtable[0] = (void *)view_query_interface;
        g_view_vtable[1] = (void *)com_addref;
        g_view_vtable[2] = (void *)view_release;
        g_view_vtable[3] = (void *)view_get_device;
        g_view_vtable[4] = (void *)view_private_data_unsupported;
        g_view_vtable[5] = (void *)view_private_data_unsupported;
        g_view_vtable[6] = (void *)view_private_data_unsupported;
        g_view_vtable[7] = (void *)view_get_resource;
        g_view_vtable[8] = (void *)view_get_desc;
    }
    BeerD3D11View *view = com_alloc(sizeof(*view), COM_TYPE_VIEW, 0);
    if (!view) return NULL;
    view->vtable = g_view_vtable;
    view->device = device;
    view->resource = resource;
    view->kind = kind;
    view->desc_size = desc_size;
    memset(view->desc, 0, sizeof(view->desc));
    if (desc) memcpy(view->desc, desc, desc_size);
    else initialize_default_view_desc(view);
    addref_external_com_object(resource);
    static _Atomic(uint32_t) view_calls;
    uint32_t call = atomic_fetch_add(&view_calls, 1) + 1;
    if (call <= 128) {
        if (desc) {
            fprintf(stderr, "[D3D11] created view #%u kind=%u resource=%p desc=explicit\n",
                    call, (unsigned)kind, resource);
        } else {
            const uint32_t *view_desc = (const uint32_t *)view->desc;
            fprintf(stderr,
                    "[D3D11] created view #%u kind=%u resource=%p desc=default "
                    "format=%u dimension=%u detail=(%u,%u,%u)\n",
                    call, (unsigned)kind, resource, view_desc[0], view_desc[1],
                    view_desc[2], view_desc[3], view_desc[4]);
        }
    }
    return view;
}

static HRESULT D3D11_MS_ABI device_create_rendertarget_view(
    ID3D11Device *this, void *resource, void *desc, void **out)
{
    if (!out || !resource) return (HRESULT)0x80070057;
    *out = view_create(this, resource, desc, BEER_VIEW_RENDER_TARGET, 20);
    return *out ? S_OK : (HRESULT)0x8007000e;
}

static HRESULT D3D11_MS_ABI device_create_depthstencil_view(
    ID3D11Device *this, void *resource, void *desc, void **out)
{
    if (!out || !resource) return (HRESULT)0x80070057;
    *out = view_create(this, resource, desc, BEER_VIEW_DEPTH_STENCIL, 24);
    return *out ? S_OK : (HRESULT)0x8007000e;
}

typedef struct {
    void **vtable;
    ID3D11Device *device;
    uint32_t dimension;
    uint32_t desc_size;
    uint8_t desc[44];
    uint8_t *pixels;
    size_t pixel_size;
    uint32_t row_pitch;
    _Atomic(uint64_t) write_serial;
    _Atomic(uint64_t) content_serial;
    uint32_t subresource_count;
    size_t *subresource_offsets;
    size_t *subresource_sizes;
    uint32_t *subresource_row_pitches;
    uint32_t *subresource_rows;
    uint8_t *decoded_rgba;
    size_t decoded_rgba_size;
    uint64_t decoded_content_serial;
    pthread_mutex_t decoded_lock;
    uint32_t diagnostic_hash;
    uint64_t diagnostic_hash_serial;
} BeerD3D11Resource;

static BeerD3D11Resource *validated_resource(void *object);
static void *g_resource_vtable[11];
static _Atomic(uint64_t) g_resource_write_serial;
static _Atomic(uint64_t) g_resource_content_serial;
static _Atomic(uint32_t) g_indexed_target_pending;
static BeerD3D11Resource *g_last_textured_indexed_target;

static uint64_t resource_mark_written(BeerD3D11Resource *resource)
{
    if (!resource) return 0;
    uint64_t serial = atomic_fetch_add(&g_resource_write_serial, 1) + 1;
    uint64_t content_serial = atomic_fetch_add(&g_resource_content_serial, 1) + 1;
    atomic_store(&resource->write_serial, serial);
    atomic_store(&resource->content_serial, content_serial);
    return serial;
}

/* Serializes render-target serial allocation with GPU submission.
 *
 * A Vulkan target mirror only accumulates correctly when the serial chain
 * matches the order in which draws reach the renderer: draw N must observe the
 * exact output serial published by draw N-1. Beer previously performed three
 * unsynchronized steps per draw (read input serial, allocate output serial,
 * submit). Sekiro issues UI work from several jobs against one 1920x1080
 * target, so those steps could interleave and hand a draw an input serial the
 * mirror never produced. The renderer then treated the CPU copy as newer and
 * re-uploaded it, discarding every layer already rasterized into the mirror
 * during that frame. That is the mechanism behind button backgrounds, text and
 * control layers disappearing or reappearing depending on hover timing.
 *
 * Callers must hold this lock across serial allocation and submission. */
static pthread_mutex_t g_gpu_target_order_lock = PTHREAD_MUTEX_INITIALIZER;

/* Atomically publishes a new content serial for a GPU write and reports the
 * serial the target held immediately before it. Must be called with
 * g_gpu_target_order_lock held so the pair is ordered against submission. */
static void resource_begin_gpu_write(BeerD3D11Resource *resource,
                                     uint64_t *input_serial,
                                     uint64_t *output_serial)
{
    if (!resource) {
        if (input_serial) *input_serial = 0;
        if (output_serial) *output_serial = 0;
        return;
    }
    uint64_t input = atomic_load(&resource->content_serial);
    uint64_t write = atomic_fetch_add(&g_resource_write_serial, 1) + 1;
    uint64_t output = atomic_fetch_add(&g_resource_content_serial, 1) + 1;
    atomic_store(&resource->write_serial, write);
    atomic_store(&resource->content_serial, output);
    if (input_serial) *input_serial = input;
    if (output_serial) *output_serial = output;
}

static void resource_sync_from_vulkan(BeerD3D11Resource *resource)
{
    if (!resource || !resource->pixels || !resource->pixel_size) return;
    uint64_t serial = atomic_load(&resource->content_serial);
    if (vulkan_renderer_sync_target(resource, serial,
                                    resource->pixels, resource->pixel_size))
        return;
    vulkan_indexed_renderer_sync_target(resource, serial,
                                        resource->pixels, resource->pixel_size);
}

static void initialize_default_view_desc(BeerD3D11View *view)
{
    BeerD3D11Resource *resource = validated_resource(view->resource);
    if (!resource || resource->desc_size < 20) return;

    const uint32_t *resource_desc = (const uint32_t *)resource->desc;
    uint32_t *view_desc = (uint32_t *)view->desc;
    view_desc[0] = resource_desc[4]; /* Format */
    if (resource->dimension == 3) { /* ID3D11Texture2D */
        if (view->kind == BEER_VIEW_DEPTH_STENCIL) {
            view_desc[1] = 3; /* D3D11_DSV_DIMENSION_TEXTURE2D */
            view_desc[2] = 0; /* MipSlice */
        } else {
            /* View-dimension enum values differ between SRV, RTV and UAV even
             * though all three unions describe Texture2D here. */
            view_desc[1] = view->kind == BEER_VIEW_SHADER_RESOURCE ? 4 :
                           view->kind == BEER_VIEW_RENDER_TARGET ? 3 : 4;
            view_desc[2] = 0; /* MostDetailedMip / MipSlice */
            if (view->kind == BEER_VIEW_SHADER_RESOURCE)
                view_desc[3] = resource_desc[2] ? resource_desc[2] : UINT32_MAX;
        }
    } else if (resource->dimension == 4) { /* ID3D11Texture3D */
        if (view->kind == BEER_VIEW_SHADER_RESOURCE) {
            view_desc[1] = 8; /* D3D11_SRV_DIMENSION_TEXTURE3D */
            view_desc[2] = 0;
            view_desc[3] = resource_desc[3] ? resource_desc[3] : UINT32_MAX;
        } else if (view->kind == BEER_VIEW_RENDER_TARGET ||
                   view->kind == BEER_VIEW_UNORDERED_ACCESS) {
            view_desc[1] = 8; /* D3D11_RTV/UAV_DIMENSION_TEXTURE3D */
            view_desc[2] = 0; /* MipSlice */
            view_desc[3] = 0; /* FirstWSlice */
            view_desc[4] = resource_desc[2]; /* WSize */
        }
    }
}

static HRESULT D3D11_MS_ABI resource_query_interface(
    BeerD3D11Resource *this, REFIID riid, LPVOID *ppvObj)
{
    static const uint8_t iid_iunknown[16] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46
    };
    static const uint8_t iid_resource[16] = {
        0xf3, 0x63, 0x8e, 0xdc, 0x2b, 0xd1, 0x52, 0x49,
        0xb4, 0x7b, 0x5e, 0x45, 0x02, 0x6a, 0x86, 0x2d
    };
    static const uint8_t iid_buffer[16] = {
        0x85, 0x0b, 0x57, 0x48, 0xee, 0xd1, 0xcd, 0x4f,
        0xa2, 0x50, 0xeb, 0x35, 0x07, 0x22, 0xb0, 0x37
    };
    static const uint8_t iid_texture2d[16] = {
        0xf2, 0xaa, 0x15, 0x6f, 0x08, 0xd2, 0x89, 0x4e,
        0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c
    };
    static const uint8_t iid_texture3d[16] = {
        0x24, 0xf9, 0x6a, 0x03, 0x95, 0xa6, 0x29, 0x4d,
        0xb9, 0x44, 0xa5, 0x05, 0x9b, 0xc4, 0x17, 0x9d
    };
    if (!ppvObj) return (HRESULT)0x80070057;
    *ppvObj = NULL;
    if (!iid_equal(riid, iid_iunknown) && !iid_equal(riid, iid_resource) &&
        !(this->dimension == 1 && iid_equal(riid, iid_buffer)) &&
        !(this->dimension == 3 && iid_equal(riid, iid_texture2d)) &&
        !(this->dimension == 4 && iid_equal(riid, iid_texture3d)))
        return E_NOINTERFACE;
    *ppvObj = this;
    com_addref(this);
    return S_OK;
}

static uint32_t D3D11_MS_ABI resource_release(BeerD3D11Resource *this)
{
    ComObjectHeader *header = com_get_header(this);
    if (!header) return 0;
    uint32_t old_count = atomic_fetch_sub(&header->refcount, 1);
    if (old_count == 1) {
        vulkan_renderer_forget_resource(this);
        vulkan_indexed_renderer_forget_resource(this);
        free(this->pixels);
        free(this->decoded_rgba);
        pthread_mutex_destroy(&this->decoded_lock);
        free(this->subresource_offsets);
        free(this->subresource_sizes);
        free(this->subresource_row_pitches);
        free(this->subresource_rows);
        free(header);
        return 0;
    }
    return old_count - 1;
}

static void D3D11_MS_ABI resource_get_device(
    BeerD3D11Resource *this, ID3D11Device **device)
{
    if (!device) return;
    *device = this->device;
    if (this->device) device_addref(this->device);
}

static HRESULT D3D11_MS_ABI resource_private_data_unsupported(
    BeerD3D11Resource *this, void *a, void *b, void *c)
{
    (void)this; (void)a; (void)b; (void)c;
    return (HRESULT)0x80004001;
}

static void D3D11_MS_ABI resource_get_type(
    BeerD3D11Resource *this, uint32_t *dimension)
{
    if (dimension) *dimension = this->dimension;
}

static HRESULT D3D11_MS_ABI resource_set_eviction_priority(
    BeerD3D11Resource *this, uint32_t priority)
{
    (void)this; (void)priority;
    return S_OK;
}

static uint32_t D3D11_MS_ABI resource_get_eviction_priority(
    BeerD3D11Resource *this)
{
    (void)this;
    return 0;
}

static void D3D11_MS_ABI resource_get_desc(
    BeerD3D11Resource *this, void *desc)
{
    if (desc) memcpy(desc, this->desc, this->desc_size);
}

static uint32_t texture_format_bytes_per_pixel(uint32_t format)
{
    switch (format) {
    case 24: /* DXGI_FORMAT_R10G10B10A2_UNORM */
    case 28: /* DXGI_FORMAT_R8G8B8A8_UNORM */
    case 29: /* DXGI_FORMAT_R8G8B8A8_UNORM_SRGB */
    case 87: /* DXGI_FORMAT_B8G8R8A8_UNORM */
        return 4;
    case 56: /* DXGI_FORMAT_R16_UINT */
        return 2;
    case 61: /* DXGI_FORMAT_R8_UNORM */
        return 1;
    /* NV12 is a planar YUV 4:2:0 format: Y plane (1 byte/pixel) followed by
     * interleaved UV plane (0.5 bytes/pixel), total 1.5 bytes per pixel.
     * We model storage as 1 byte per pixel over height * 3/2 rows so that
     * row_pitch == video_width and pixel_size == width * height * 3 / 2.
     * The RE Engine reads the Y plane via Lock2D (pitch == width) then reads
     * the UV plane at data + stride * height. */
    case 103: /* DXGI_FORMAT_NV12 */
        return 1; /* Y-plane stride in bytes per pixel; UV follows Y block */
    default:
        return 0;
    }
}

static uint32_t texture_format_block_bytes(uint32_t format)
{
    switch (format) {
    case 70: /* DXGI_FORMAT_BC1_TYPELESS */
    case 71: /* DXGI_FORMAT_BC1_UNORM */
    case 72: /* DXGI_FORMAT_BC1_UNORM_SRGB */
        return 8;
    case 73: /* DXGI_FORMAT_BC2_TYPELESS */
    case 74: /* DXGI_FORMAT_BC2_UNORM */
    case 75: /* DXGI_FORMAT_BC2_UNORM_SRGB */
    case 76: /* DXGI_FORMAT_BC3_TYPELESS */
    case 77: /* DXGI_FORMAT_BC3_UNORM */
    case 78: /* DXGI_FORMAT_BC3_UNORM_SRGB */
    case 79: /* DXGI_FORMAT_BC4_TYPELESS */
    case 80: /* DXGI_FORMAT_BC4_UNORM */
    case 81: /* DXGI_FORMAT_BC4_SNORM */
    case 82: /* DXGI_FORMAT_BC5_TYPELESS */
    case 83: /* DXGI_FORMAT_BC5_UNORM */
    case 84: /* DXGI_FORMAT_BC5_SNORM */
    case 97: /* DXGI_FORMAT_BC7_TYPELESS */
    case 98: /* DXGI_FORMAT_BC7_UNORM */
    case 99: /* DXGI_FORMAT_BC7_UNORM_SRGB */
        return 16;
    default:
        return 0;
    }
}

static int resource_allocate_texture2d_storage(BeerD3D11Resource *resource,
                                                const uint32_t *texture)
{
    uint32_t width = texture[0], height = texture[1];
    uint32_t mip_levels = texture[2], array_size = texture[3];
    uint32_t bytes_per_pixel = texture_format_bytes_per_pixel(texture[4]);
    uint32_t block_bytes = texture_format_block_bytes(texture[4]);
    int is_nv12 = (texture[4] == 103);
    /* NV12 storage uses bytes_per_pixel=1 for the Y plane; treat it as valid
     * even though bytes_per_pixel alone wouldn't describe its UV plane. */
    if (!width || !height || !mip_levels || !array_size ||
        (!bytes_per_pixel && !block_bytes && !is_nv12) ||
        mip_levels > UINT32_MAX / array_size)
        return 0;

    uint32_t count = mip_levels * array_size;
    resource->subresource_offsets = calloc(count, sizeof(size_t));
    resource->subresource_sizes = calloc(count, sizeof(size_t));
    resource->subresource_row_pitches = calloc(count, sizeof(uint32_t));
    resource->subresource_rows = calloc(count, sizeof(uint32_t));
    if (!resource->subresource_offsets || !resource->subresource_sizes ||
        !resource->subresource_row_pitches || !resource->subresource_rows)
        return 0;

    /* NV12 has a special planar layout: the Y plane occupies width*height bytes
     * and the interleaved UV plane occupies width*(height/2) bytes immediately
     * after. We store it as a single allocation with rows = height*3/2 and
     * row_pitch = width so that UpdateSubresource can copy the whole NV12
     * buffer in one pass and Lock2D returns the correct Y-plane pointer/pitch. */

    size_t total = 0;
    for (uint32_t array = 0; array < array_size; ++array) {
        uint32_t mip_width = width, mip_height = height;
        for (uint32_t mip = 0; mip < mip_levels; ++mip) {
            uint32_t index = array * mip_levels + mip;
            uint32_t rows;
            size_t pitch;
            if (is_nv12) {
                /* NV12: Y rows + UV rows (height/2, rounded up).
                 * pitch = width (1 byte per Y pixel). */
                rows = mip_height + ((mip_height + 1u) / 2u);
                pitch = (size_t)mip_width;
            } else if (block_bytes) {
                uint32_t blocks_wide = (mip_width + 3u) / 4u;
                rows = (mip_height + 3u) / 4u;
                if (!blocks_wide) blocks_wide = 1;
                if (!rows) rows = 1;
                pitch = (size_t)blocks_wide * block_bytes;
            } else {
                rows = mip_height;
                pitch = (size_t)mip_width * bytes_per_pixel;
            }
            if (pitch > UINT32_MAX || rows > SIZE_MAX / pitch ||
                total > SIZE_MAX - pitch * rows)
                return 0;
            resource->subresource_offsets[index] = total;
            resource->subresource_row_pitches[index] = (uint32_t)pitch;
            resource->subresource_rows[index] = rows;
            resource->subresource_sizes[index] = pitch * rows;
            total += pitch * rows;
            if (mip_width > 1) mip_width >>= 1;
            if (mip_height > 1) mip_height >>= 1;
        }
    }
    resource->subresource_count = count;
    resource->row_pitch = resource->subresource_row_pitches[0];
    resource->pixel_size = total;
    resource->pixels = calloc(1, total);
    return resource->pixels != NULL;
}

static BeerD3D11Resource *resource_create(
    ID3D11Device *device, uint32_t dimension, const void *desc, uint32_t desc_size)
{
    static int initialized;
    if (!desc || desc_size > sizeof(((BeerD3D11Resource *)0)->desc)) return NULL;
    if (!initialized) {
        initialized = 1;
        g_resource_vtable[0] = (void *)resource_query_interface;
        g_resource_vtable[1] = (void *)com_addref;
        g_resource_vtable[2] = (void *)resource_release;
        g_resource_vtable[3] = (void *)resource_get_device;
        g_resource_vtable[4] = (void *)resource_private_data_unsupported;
        g_resource_vtable[5] = (void *)resource_private_data_unsupported;
        g_resource_vtable[6] = (void *)resource_private_data_unsupported;
        g_resource_vtable[7] = (void *)resource_get_type;
        g_resource_vtable[8] = (void *)resource_set_eviction_priority;
        g_resource_vtable[9] = (void *)resource_get_eviction_priority;
        g_resource_vtable[10] = (void *)resource_get_desc;
    }
    BeerD3D11Resource *resource = com_alloc(sizeof(*resource), COM_TYPE_RESOURCE, 0);
    if (!resource) return NULL;
    memset(resource, 0, sizeof(*resource));
    pthread_mutex_init(&resource->decoded_lock, NULL);
    resource->vtable = g_resource_vtable;
    resource->device = device;
    resource->dimension = dimension;
    resource->desc_size = desc_size;
    memcpy(resource->desc, desc, desc_size);

    /* Buffers always have a byte-addressable CPU backing store. This makes
     * Map/Unmap coherent for dynamic vertex, index and constant buffers. */
    if (dimension == 1 && desc_size >= 4) {
        uint32_t byte_width = ((const uint32_t *)desc)[0];
        if (byte_width) {
            resource->row_pitch = byte_width;
            resource->pixel_size = byte_width;
            resource->pixels = calloc(1, resource->pixel_size);
        }
    }

    /* Texture2D storage follows D3D11 subresource ordering (array slice, then
     * mip level) and preserves native BC blocks rather than pretending they are
     * RGBA pixels. This lets compressed asset uploads retain their exact bytes
     * without claiming that Beer can render to a compressed format. */
    if (dimension == 3 && desc_size >= 20)
        resource_allocate_texture2d_storage(resource, desc);
    if (dimension == 4 && desc_size >= 24) {
        const uint32_t *texture = desc;
        uint32_t width = texture[0], height = texture[1], depth = texture[2];
        uint32_t format = texture[4];
        uint32_t bytes_per_pixel = texture_format_bytes_per_pixel(format);
        size_t slice_pitch;
        if (width && height && depth && bytes_per_pixel &&
            width <= UINT32_MAX / bytes_per_pixel &&
            height <= SIZE_MAX / ((size_t)width * bytes_per_pixel)) {
            resource->row_pitch = width * bytes_per_pixel;
            slice_pitch = (size_t)resource->row_pitch * height;
            if (depth <= SIZE_MAX / slice_pitch) {
                resource->pixel_size = slice_pitch * depth;
                resource->pixels = calloc(1, resource->pixel_size);
            }
        }
    }
    if (resource->pixel_size && !resource->pixels) {
        ComObjectHeader *header = com_get_header(resource);
        pthread_mutex_destroy(&resource->decoded_lock);
        free(resource->subresource_offsets);
        free(resource->subresource_sizes);
        free(resource->subresource_row_pitches);
        free(resource->subresource_rows);
        free(header);
        return NULL;
    }
    return resource;
}

typedef struct {
    const void *data;
    uint32_t row_pitch;
    uint32_t slice_pitch;
} BeerSubresourceData;

static void initialize_resource_data(BeerD3D11Resource *resource,
                                     const BeerSubresourceData *initial)
{
    if (!resource || !resource->pixels || !initial || !initial->data) return;
    if (resource->dimension == 1) {
        memcpy(resource->pixels, initial->data, resource->pixel_size);
        return;
    }
    if (resource->dimension == 3 && resource->subresource_count) {
        for (uint32_t subresource = 0; subresource < resource->subresource_count;
             ++subresource) {
            const BeerSubresourceData *data = &initial[subresource];
            if (!data->data) continue;
            uint32_t destination_pitch = resource->subresource_row_pitches[subresource];
            uint32_t source_pitch = data->row_pitch ? data->row_pitch : destination_pitch;
            size_t copy_pitch = source_pitch < destination_pitch
                ? source_pitch : destination_pitch;
            for (uint32_t row = 0; row < resource->subresource_rows[subresource]; ++row)
                memcpy(resource->pixels + resource->subresource_offsets[subresource] +
                           (size_t)row * destination_pitch,
                       (const uint8_t *)data->data + (size_t)row * source_pitch,
                       copy_pitch);
        }
        return;
    }
    const uint32_t *desc = (const uint32_t *)resource->desc;
    uint32_t height = desc[1];
    uint32_t depth_or_slices = resource->dimension == 4 ? desc[2] : desc[3];
    if (!depth_or_slices) depth_or_slices = 1;
    uint32_t source_pitch = initial->row_pitch ? initial->row_pitch : resource->row_pitch;
    size_t destination_slice_pitch = (size_t)resource->row_pitch * height;
    size_t source_slice_pitch = initial->slice_pitch
        ? initial->slice_pitch : (size_t)source_pitch * height;
    size_t copy_pitch = source_pitch < resource->row_pitch ? source_pitch : resource->row_pitch;
    for (uint32_t slice = 0; slice < depth_or_slices; ++slice) {
        for (uint32_t y = 0; y < height; ++y)
            memcpy(resource->pixels + (size_t)slice * destination_slice_pitch +
                       (size_t)y * resource->row_pitch,
                   (const uint8_t *)initial->data +
                       (size_t)slice * source_slice_pitch + (size_t)y * source_pitch,
                   copy_pitch);
    }
}

static HRESULT D3D11_MS_ABI device_create_texture2d(ID3D11Device* this, void* pDesc, void* pInitData, void** ppTexture2D) {
    if (!ppTexture2D || !pDesc) return (HRESULT)0x80070057;
    BeerD3D11Resource *resource = resource_create(this, 3, pDesc, 44);
    if (!resource) {
        *ppTexture2D = NULL;
        return (HRESULT)0x8007000e;
    }
    initialize_resource_data(resource, (const BeerSubresourceData *)pInitData);
    *ppTexture2D = resource;
    static _Atomic(uint32_t) calls;
    uint32_t call = atomic_fetch_add(&calls, 1) + 1;
    const uint32_t *desc = pDesc;
    if (call <= 96 || (desc[0] == 1920 && desc[1] == 1080) ||
        desc[4] == 61 || desc[4] == 87 || desc[4] == 103 /* NV12 */)
        fprintf(stderr, "[D3D11 TEXTURE] #%u resource=%p %ux%u mips=%u array=%u format=%u usage=%u bind=0x%x cpu=0x%x misc=0x%x initial=%s pixels=%p bytes=%zu\n",
                call, (void *)resource, desc[0], desc[1], desc[2], desc[3],
                desc[4], desc[7], desc[8], desc[9], desc[10],
                pInitData ? "yes" : "no", (void *)resource->pixels,
                resource->pixel_size);
    return S_OK;
}

static HRESULT D3D11_MS_ABI device_create_texture3d(
    ID3D11Device *this, void *pDesc, void *pInitData, void **ppTexture3D)
{
    if (!ppTexture3D || !pDesc) return (HRESULT)0x80070057;
    BeerD3D11Resource *resource = resource_create(this, 4, pDesc, 36);
    if (!resource) {
        *ppTexture3D = NULL;
        return (HRESULT)0x8007000e;
    }
    initialize_resource_data(resource, (const BeerSubresourceData *)pInitData);
    *ppTexture3D = resource;
    return S_OK;
}

static HRESULT D3D11_MS_ABI device_create_buffer(ID3D11Device* this, void* pDesc, void* pInitData, void** ppBuffer) {
    if (!ppBuffer || !pDesc) return (HRESULT)0x80070057;
    BeerD3D11Resource *resource = resource_create(this, 1, pDesc, 24);
    if (!resource) {
        *ppBuffer = NULL;
        return (HRESULT)0x8007000e;
    }
    initialize_resource_data(resource, (const BeerSubresourceData *)pInitData);
    *ppBuffer = resource;
    return S_OK;
}

typedef enum {
    BEER_SHADER_VERTEX,
    BEER_SHADER_GEOMETRY,
    BEER_SHADER_PIXEL,
    BEER_SHADER_HULL,
    BEER_SHADER_DOMAIN,
    BEER_SHADER_COMPUTE,
} BeerD3D11ShaderKind;

typedef struct {
    void **vtable;
    ID3D11Device *device;
    BeerD3D11ShaderKind kind;
    size_t bytecode_size;
    uint8_t bytecode[];
} BeerD3D11Shader;

#define BEER_MAX_INPUT_ELEMENTS 16

typedef struct {
    char semantic[32];
    uint32_t semantic_index;
    uint32_t format;
    uint32_t input_slot;
    uint32_t aligned_byte_offset;
    uint32_t input_slot_class;
    uint32_t instance_step_rate;
} BeerD3D11InputElement;

typedef struct {
    const char *semantic_name;
    uint32_t semantic_index;
    uint32_t format;
    uint32_t input_slot;
    uint32_t aligned_byte_offset;
    uint32_t input_slot_class;
    uint32_t instance_step_rate;
} BeerD3D11InputElementDesc;

typedef struct {
    void **vtable;
    ID3D11Device *device;
    uint32_t element_count;
    size_t signature_size;
    BeerD3D11InputElement elements[BEER_MAX_INPUT_ELEMENTS];
    uint8_t signature[];
} BeerD3D11InputLayout;

static void *g_child_vtable[7];

static HRESULT D3D11_MS_ABI child_query_interface(
    void *object, REFIID riid, LPVOID *out)
{
    static const uint8_t iid_iunknown[16] = {
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xc0,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x46
    };
    if (!out) return (HRESULT)0x80070057;
    *out = NULL;
    if (!iid_equal(riid, iid_iunknown)) return E_NOINTERFACE;
    *out = object;
    com_addref(object);
    return S_OK;
}

static void D3D11_MS_ABI child_get_device(
    BeerD3D11Shader *object, ID3D11Device **device)
{
    if (!device) return;
    *device = object->device;
    if (*device) device_addref(*device);
}

static void init_child_vtable(void)
{
    if (g_child_vtable[0]) return;
    g_child_vtable[0] = (void *)child_query_interface;
    g_child_vtable[1] = (void *)com_addref;
    g_child_vtable[2] = (void *)com_release;
    g_child_vtable[3] = (void *)child_get_device;
    g_child_vtable[4] = (void *)resource_private_data_unsupported;
    g_child_vtable[5] = (void *)resource_private_data_unsupported;
    g_child_vtable[6] = (void *)resource_private_data_unsupported;
}

static HRESULT create_shader(ID3D11Device *device, const void *bytecode,
    size_t bytecode_size, void **output, BeerD3D11ShaderKind kind)
{
    if (!output) return (HRESULT)0x80070057;
    *output = NULL;
    if (!bytecode || !bytecode_size || bytecode_size > SIZE_MAX - sizeof(BeerD3D11Shader))
        return (HRESULT)0x80070057;
    init_child_vtable();
    BeerD3D11Shader *shader = com_alloc(sizeof(*shader) + bytecode_size,
        COM_TYPE_SHADER, 0);
    if (!shader) return (HRESULT)0x8007000e;
    shader->vtable = g_child_vtable;
    shader->device = device;
    shader->kind = kind;
    shader->bytecode_size = bytecode_size;
    memcpy(shader->bytecode, bytecode, bytecode_size);
    *output = shader;
    static _Atomic(uint32_t) calls;
    uint32_t call = atomic_fetch_add(&calls, 1) + 1;
    if (call <= 32)
        fprintf(stderr, "[D3D11 TRACE] CreateShader #%u kind=%u bytes=%zu object=%p\n",
                call, (unsigned)kind, bytecode_size, (void *)shader);
    return S_OK;
}

static HRESULT D3D11_MS_ABI device_create_input_layout(ID3D11Device* this, void* pInputElementDescs, uint32_t NumElements, void* pShaderBytecode, size_t BytecodeLength, void** ppInputLayout) {
    if (!ppInputLayout) return (HRESULT)0x80070057;
    *ppInputLayout = NULL;
    if ((!pInputElementDescs && NumElements) || NumElements > BEER_MAX_INPUT_ELEMENTS ||
        (!pShaderBytecode && BytecodeLength) ||
        BytecodeLength > SIZE_MAX - sizeof(BeerD3D11InputLayout))
        return (HRESULT)0x80070057;
    init_child_vtable();
    BeerD3D11InputLayout *layout = com_alloc(sizeof(*layout) + BytecodeLength,
        COM_TYPE_INPUT_LAYOUT, 0);
    if (!layout) return (HRESULT)0x8007000e;
    memset(layout, 0, sizeof(*layout));
    layout->vtable = g_child_vtable;
    layout->device = this;
    layout->element_count = NumElements;
    layout->signature_size = BytecodeLength;
    const BeerD3D11InputElementDesc *descs = pInputElementDescs;
    for (uint32_t i = 0; i < NumElements; ++i) {
        BeerD3D11InputElement *element = &layout->elements[i];
        if (descs[i].semantic_name)
            snprintf(element->semantic, sizeof(element->semantic), "%s",
                     descs[i].semantic_name);
        element->semantic_index = descs[i].semantic_index;
        element->format = descs[i].format;
        element->input_slot = descs[i].input_slot;
        element->aligned_byte_offset = descs[i].aligned_byte_offset;
        element->input_slot_class = descs[i].input_slot_class;
        element->instance_step_rate = descs[i].instance_step_rate;
    }
    if (BytecodeLength) memcpy(layout->signature, pShaderBytecode, BytecodeLength);
    *ppInputLayout = layout;
    static _Atomic(uint32_t) calls;
    uint32_t call = atomic_fetch_add(&calls, 1) + 1;
    if (call <= 24)
        fprintf(stderr, "[D3D11 TRACE] CreateInputLayout #%u elements=%u bytes=%zu object=%p\n",
                call, NumElements, BytecodeLength, (void *)layout);
    return S_OK;
}

static HRESULT D3D11_MS_ABI device_create_vertex_shader(ID3D11Device* this, void* pShaderBytecode, size_t BytecodeLength, void* pClassLinkage, void** ppVertexShader) {
    (void)pClassLinkage;
    return create_shader(this, pShaderBytecode, BytecodeLength, ppVertexShader, BEER_SHADER_VERTEX);
}

static HRESULT D3D11_MS_ABI device_create_pixel_shader(ID3D11Device* this, void* pShaderBytecode, size_t BytecodeLength, void* pClassLinkage, void** ppPixelShader) {
    (void)pClassLinkage;
    return create_shader(this, pShaderBytecode, BytecodeLength, ppPixelShader, BEER_SHADER_PIXEL);
}

static HRESULT D3D11_MS_ABI device_create_geometry_shader(ID3D11Device* this, void* pShaderBytecode, size_t BytecodeLength, void* pClassLinkage, void** ppGeometryShader) {
    (void)pClassLinkage;
    return create_shader(this, pShaderBytecode, BytecodeLength, ppGeometryShader, BEER_SHADER_GEOMETRY);
}

static HRESULT D3D11_MS_ABI device_create_hull_shader(ID3D11Device* this, void* pShaderBytecode, size_t BytecodeLength, void* pClassLinkage, void** ppHullShader) {
    (void)pClassLinkage;
    return create_shader(this, pShaderBytecode, BytecodeLength, ppHullShader, BEER_SHADER_HULL);
}

static HRESULT D3D11_MS_ABI device_create_domain_shader(ID3D11Device* this, void* pShaderBytecode, size_t BytecodeLength, void* pClassLinkage, void** ppDomainShader) {
    (void)pClassLinkage;
    return create_shader(this, pShaderBytecode, BytecodeLength, ppDomainShader, BEER_SHADER_DOMAIN);
}

static HRESULT D3D11_MS_ABI device_create_compute_shader(ID3D11Device* this, void* pShaderBytecode, size_t BytecodeLength, void* pClassLinkage, void** ppComputeShader) {
    (void)pClassLinkage;
    return create_shader(this, pShaderBytecode, BytecodeLength, ppComputeShader, BEER_SHADER_COMPUTE);
}

typedef enum {
    BEER_STATE_BLEND,
    BEER_STATE_DEPTH_STENCIL,
    BEER_STATE_RASTERIZER,
} BeerD3D11StateKind;

typedef struct {
    void **vtable;
    ID3D11Device *device;
    BeerD3D11StateKind kind;
    uint32_t desc_size;
    uint8_t desc[264];
} BeerD3D11State;

static void *g_pipeline_state_vtable[8];

static HRESULT D3D11_MS_ABI pipeline_state_query_interface(
    BeerD3D11State *this, REFIID riid, LPVOID *ppvObj)
{
    static const uint8_t iid_iunknown[16] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46
    };
    static const uint8_t iid_blend_state[16] = {
        0xaa, 0x8f, 0xb6, 0x75, 0x7d, 0x34, 0x59, 0x41,
        0x8f, 0x45, 0xa0, 0x64, 0x0f, 0x01, 0xcd, 0x9a
    };
    static const uint8_t iid_depth_stencil_state[16] = {
        0xfb, 0x3e, 0x82, 0x03, 0x8f, 0x8d, 0x1c, 0x4e,
        0x9a, 0xa2, 0xf6, 0x4b, 0xb2, 0xcb, 0xfd, 0xdf
    };
    static const uint8_t iid_rasterizer_state[16] = {
        0x81, 0xab, 0xb4, 0x9b, 0x1a, 0xab, 0x8f, 0x4d,
        0xb5, 0x06, 0xfc, 0x04, 0x20, 0x0b, 0x6e, 0xe7
    };
    const uint8_t *state_iid = this->kind == BEER_STATE_BLEND ? iid_blend_state :
        this->kind == BEER_STATE_DEPTH_STENCIL ? iid_depth_stencil_state :
        iid_rasterizer_state;

    if (!ppvObj) return (HRESULT)0x80070057;
    *ppvObj = NULL;
    if (!iid_equal(riid, iid_iunknown) && !iid_equal(riid, state_iid))
        return E_NOINTERFACE;
    *ppvObj = this;
    com_addref(this);
    return S_OK;
}

static void D3D11_MS_ABI pipeline_state_get_device(
    BeerD3D11State *this, ID3D11Device **device)
{
    if (!device) return;
    *device = this->device;
    if (this->device) device_addref(this->device);
}

static void D3D11_MS_ABI pipeline_state_get_desc(
    BeerD3D11State *this, void *desc)
{
    if (desc) memcpy(desc, this->desc, this->desc_size);
}

static BeerD3D11State *pipeline_state_create(
    ID3D11Device *device, BeerD3D11StateKind kind, const void *desc, uint32_t desc_size)
{
    if (!device || !desc || desc_size > sizeof(((BeerD3D11State *)0)->desc))
        return NULL;
    if (!g_pipeline_state_vtable[0]) {
        g_pipeline_state_vtable[0] = (void *)pipeline_state_query_interface;
        g_pipeline_state_vtable[1] = (void *)com_addref;
        g_pipeline_state_vtable[2] = (void *)com_release;
        g_pipeline_state_vtable[3] = (void *)pipeline_state_get_device;
        g_pipeline_state_vtable[4] = (void *)resource_private_data_unsupported;
        g_pipeline_state_vtable[5] = (void *)resource_private_data_unsupported;
        g_pipeline_state_vtable[6] = (void *)resource_private_data_unsupported;
        g_pipeline_state_vtable[7] = (void *)pipeline_state_get_desc;
    }
    BeerD3D11State *state = com_alloc(sizeof(*state), COM_TYPE_STATE, 0);
    if (!state) return NULL;
    state->vtable = g_pipeline_state_vtable;
    state->device = device;
    state->kind = kind;
    state->desc_size = desc_size;
    memcpy(state->desc, desc, desc_size);
    return state;
}

static HRESULT D3D11_MS_ABI device_create_rasterizer_state(
    ID3D11Device *this, void *desc, void **out)
{
    if (!out || !desc) return (HRESULT)0x80070057;
    *out = pipeline_state_create(this, BEER_STATE_RASTERIZER, desc, 40);
    return *out ? S_OK : (HRESULT)0x8007000e;
}

static HRESULT D3D11_MS_ABI device_create_blend_state(
    ID3D11Device *this, void *desc, void **out)
{
    if (!out || !desc) return (HRESULT)0x80070057;
    *out = pipeline_state_create(this, BEER_STATE_BLEND, desc, 264);
    return *out ? S_OK : (HRESULT)0x8007000e;
}

static HRESULT D3D11_MS_ABI device_create_depthstencil_state(
    ID3D11Device *this, void *desc, void **out)
{
    if (!out || !desc) return (HRESULT)0x80070057;
    *out = pipeline_state_create(this, BEER_STATE_DEPTH_STENCIL, desc, 52);
    return *out ? S_OK : (HRESULT)0x8007000e;
}

typedef struct {
    void **vtable;
    ID3D11Device *device;
    uint8_t desc[52];
} BeerD3D11SamplerState;

static void *g_sampler_state_vtable[8];

static HRESULT D3D11_MS_ABI sampler_query_interface(
    BeerD3D11SamplerState *this, REFIID riid, LPVOID *ppvObj)
{
    static const uint8_t iid_iunknown[16] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46
    };
    static const uint8_t iid_sampler[16] = {
        0x77, 0x6e, 0x9b, 0xda, 0x45, 0xe0, 0x9a, 0x49,
        0xa4, 0xea, 0xcd, 0xe3, 0x8f, 0xe5, 0x9d, 0x9f
    };
    if (!ppvObj) return (HRESULT)0x80070057;
    *ppvObj = NULL;
    if (!iid_equal(riid, iid_iunknown) && !iid_equal(riid, iid_sampler))
        return E_NOINTERFACE;
    *ppvObj = this;
    com_addref(this);
    return S_OK;
}

static void D3D11_MS_ABI sampler_get_device(
    BeerD3D11SamplerState *this, ID3D11Device **device)
{
    if (!device) return;
    *device = this->device;
    if (this->device) device_addref(this->device);
}

static void D3D11_MS_ABI sampler_get_desc(
    BeerD3D11SamplerState *this, void *desc)
{
    if (desc) memcpy(desc, this->desc, sizeof(this->desc));
}

static HRESULT D3D11_MS_ABI device_create_sampler_state(ID3D11Device* this, void* pSamplerDesc, void** ppSamplerState) {
    if (!ppSamplerState || !pSamplerDesc) return (HRESULT)0x80070057;
    *ppSamplerState = NULL;
    if (!g_sampler_state_vtable[0]) {
        g_sampler_state_vtable[0] = (void *)sampler_query_interface;
        g_sampler_state_vtable[1] = (void *)com_addref;
        g_sampler_state_vtable[2] = (void *)com_release;
        g_sampler_state_vtable[3] = (void *)sampler_get_device;
        g_sampler_state_vtable[4] = (void *)resource_private_data_unsupported;
        g_sampler_state_vtable[5] = (void *)resource_private_data_unsupported;
        g_sampler_state_vtable[6] = (void *)resource_private_data_unsupported;
        g_sampler_state_vtable[7] = (void *)sampler_get_desc;
    }
    BeerD3D11SamplerState *state = com_alloc(sizeof(*state), COM_TYPE_STATE, 0);
    if (!state) return (HRESULT)0x8007000e;
    state->vtable = g_sampler_state_vtable;
    state->device = this;
    memcpy(state->desc, pSamplerDesc, sizeof(state->desc));
    *ppSamplerState = state;
    return S_OK;
}

typedef struct {
    void **vtable;
    ID3D11Device *device;
    uint32_t query;
    uint32_t misc_flags;
    uint64_t value;
    int begun;
    int ended;
} BeerD3D11Query;

static void *g_query_vtable[9];

static HRESULT D3D11_MS_ABI query_query_interface(
    BeerD3D11Query *this, REFIID riid, LPVOID *out)
{
    static const uint8_t iid_iunknown[16] = {
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xc0,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x46
    };
    static const uint8_t iid_query[16] = {
        0x7b,0x6f,0x0b,0xd6,0x88,0x34,0x23,0x49,
        0x85,0x10,0xea,0x5a,0xd0,0x00,0x58,0x9f
    };
    static const uint8_t iid_async[16] = {
        0x33,0x7e,0xb4,0x4b,0xe4,0x28,0x75,0x4c,
        0xbe,0x5a,0xa2,0x4f,0x4f,0x7d,0x7a,0x2e
    };
    static const uint8_t iid_predicate[16] = {
        0x28,0xf6,0x33,0x9e,0xd7,0xd1,0xb2,0x42,
        0xab,0xb8,0xd1,0xb2,0xf3,0x10,0x7b,0x44
    };
    if (!out) return (HRESULT)0x80070057;
    *out = NULL;
    if (!iid_equal(riid, iid_iunknown) && !iid_equal(riid, iid_query) &&
        !iid_equal(riid, iid_async) && !iid_equal(riid, iid_predicate))
        return E_NOINTERFACE;
    *out = this;
    com_addref(this);
    return S_OK;
}

static void D3D11_MS_ABI query_get_device(
    BeerD3D11Query *this, ID3D11Device **device)
{
    if (!device) return;
    *device = this->device;
    if (this->device) device_addref(this->device);
}

static uint32_t D3D11_MS_ABI query_get_data_size(BeerD3D11Query *this)
{
    switch (this->query) {
    case 0: return 4;  /* D3D11_QUERY_EVENT: BOOL */
    case 1: return 8;  /* D3D11_QUERY_OCCLUSION: UINT64 */
    case 2: return 8;  /* D3D11_QUERY_TIMESTAMP: UINT64 */
    case 3: return 16; /* D3D11_QUERY_TIMESTAMP_DISJOINT */
    case 4: return 88; /* D3D11_QUERY_PIPELINE_STATISTICS */
    case 5: return 4;  /* D3D11_QUERY_OCCLUSION_PREDICATE: BOOL */
    default: return 8;
    }
}

static void D3D11_MS_ABI query_get_desc(BeerD3D11Query *this, void *desc)
{
    if (!desc) return;
    ((uint32_t *)desc)[0] = this->query;
    ((uint32_t *)desc)[1] = this->misc_flags;
}

static HRESULT create_async_object(ID3D11Device *this, const uint32_t *desc,
                                   void **out, const char *kind)
{
    if (!out || !desc) return (HRESULT)0x80070057;
    *out = NULL;
    if (!g_query_vtable[0]) {
        g_query_vtable[0] = (void *)query_query_interface;
        g_query_vtable[1] = (void *)com_addref;
        g_query_vtable[2] = (void *)com_release;
        g_query_vtable[3] = (void *)query_get_device;
        g_query_vtable[4] = (void *)resource_private_data_unsupported;
        g_query_vtable[5] = (void *)resource_private_data_unsupported;
        g_query_vtable[6] = (void *)resource_private_data_unsupported;
        g_query_vtable[7] = (void *)query_get_data_size;
        g_query_vtable[8] = (void *)query_get_desc;
    }
    BeerD3D11Query *query = com_alloc(sizeof(*query), COM_TYPE_RESOURCE, 0);
    if (!query) return (HRESULT)0x8007000e;
    query->vtable = g_query_vtable;
    query->device = this;
    query->query = desc[0];
    query->misc_flags = desc[1];
    query->value = 0;
    query->begun = 0;
    query->ended = 0;
    *out = query;
    static _Atomic(uint32_t) calls;
    uint32_t call = atomic_fetch_add(&calls, 1) + 1;
    if (call <= 24)
        fprintf(stderr, "[D3D11] Create%s #%u type=%u flags=0x%x -> %p\n",
                kind, call, query->query, query->misc_flags, (void *)query);
    return S_OK;
}

static HRESULT D3D11_MS_ABI device_create_query(
    ID3D11Device *this, const uint32_t *desc, void **out)
{
    return create_async_object(this, desc, out, "Query");
}

static HRESULT D3D11_MS_ABI device_create_predicate(
    ID3D11Device *this, const uint32_t *desc, void **out)
{
    return create_async_object(this, desc, out, "Predicate");
}

static HRESULT D3D11_MS_ABI device_check_format_support(
    ID3D11Device *this, uint32_t format, uint32_t *support)
{
    (void)this;
    if (!support) return (HRESULT)0x80070057;
    *support = 0;
    switch (format) {
    case 24: /* R10G10B10A2_UNORM */
    case 28: /* R8G8B8A8_UNORM */
    case 29: /* R8G8B8A8_UNORM_SRGB */
    case 56: /* R16_UINT */
    case 61: /* R8_UNORM */
    case 87: /* B8G8R8A8_UNORM */
        *support = 0x1 /* BUFFER */ | 0x2 /* IA_VERTEX_BUFFER */ |
                   0x4 /* IA_INDEX_BUFFER */ | 0x8 /* SO_BUFFER */ |
                   0x10 /* TEXTURE1D */ | 0x20 /* TEXTURE2D */ |
                   0x40 /* TEXTURE3D */ | 0x80 /* TEXTURECUBE */ |
                   0x100 /* SHADER_LOAD */ | 0x200 /* SHADER_SAMPLE */ |
                   0x400 /* SHADER_SAMPLE_COMPARISON */ |
                   0x800 /* SHADER_SAMPLE_MONO_TEXT */ |
                   0x1000 /* MIP */ | 0x2000 /* MIP_AUTOGEN */ |
                   0x4000 /* RENDER_TARGET */ | 0x8000 /* BLENDABLE */ |
                   0x20000 /* CPU_LOCKABLE */;
        break;
    case 103: /* DXGI_FORMAT_NV12 — advertise as a video decoder target so RE8
               * creates the NV12 texture through Beer's D3D11 implementation.
               * Beer converts NV12 → RGBA8 on the CPU before the draw
               * (ensure_nv12_cache / d3d11_dxgi_to_vulkan_image). */
        *support = 0x20 /* TEXTURE2D */ |
                   0x100 /* SHADER_LOAD */ | 0x200 /* SHADER_SAMPLE */ |
                   0x20000 /* CPU_LOCKABLE */;
        break;
    case 70: case 71: case 72: /* BC1 */
    case 73: case 74: case 75: /* BC2 */
    case 76: case 77: case 78: /* BC3 */
    case 79: case 80: case 81: /* BC4 */
    case 82: case 83: case 84: /* BC5 */
    case 97: case 98: case 99: /* BC7 */
        /* Beer retains complete native block-compressed mip chains for sampled
         * assets. Do not advertise render-target, blend, CPU-lock, or autogen
         * capabilities that the CPU renderer does not implement. */
        *support = 0x20 /* TEXTURE2D */ | 0x80 /* TEXTURECUBE */ |
                   0x100 /* SHADER_LOAD */ | 0x200 /* SHADER_SAMPLE */ |
                   0x1000 /* MIP */;
        break;
    default:
        /* The loader cannot back unknown formats yet. Report that honestly
         * instead of returning E_NOTIMPL with an untouched output mask. */
        return (HRESULT)0x80070057;
    }
    return S_OK;
}

static HRESULT D3D11_MS_ABI device_check_multisample_quality_levels(
    ID3D11Device *this, uint32_t format, uint32_t sample_count,
    uint32_t *quality_levels)
{
    (void)this;
    if (!quality_levels || sample_count == 0) return (HRESULT)0x80070057;
    *quality_levels = 0;
    if ((format == 24 || format == 28 || format == 29 || format == 56 ||
         format == 61 || format == 87) && sample_count == 1)
        *quality_levels = 1;
    return S_OK;
}

static HRESULT D3D11_MS_ABI device_check_feature_support(
    ID3D11Device *this, uint32_t feature, void *data, uint32_t data_size)
{
    (void)this;
    if (!data) return (HRESULT)0x80070057;
    memset(data, 0, data_size);
    if (feature == 0 && data_size >= 8) {
        /* D3D11_FEATURE_THREADING: Beer serializes object creation internally;
         * command-list execution is supported by the deferred-context shim. */
        ((uint32_t *)data)[0] = 0; /* DriverConcurrentCreates */
        ((uint32_t *)data)[1] = 1; /* DriverCommandLists */
        return S_OK;
    }
    return (HRESULT)0x80070057;
}

static HRESULT D3D11_MS_ABI device_create_shaderresource_view(
    ID3D11Device *this, void *resource, void *desc, void **out)
{
    if (!out || !resource) return (HRESULT)0x80070057;
    *out = view_create(this, resource, desc, BEER_VIEW_SHADER_RESOURCE, 24);
    return *out ? S_OK : (HRESULT)0x8007000e;
}

static HRESULT D3D11_MS_ABI device_create_unorderedaccess_view(
    ID3D11Device *this, void *resource, void *desc, void **out)
{
    if (!out || !resource) return (HRESULT)0x80070057;
    *out = view_create(this, resource, desc, BEER_VIEW_UNORDERED_ACCESS, 20);
    return *out ? S_OK : (HRESULT)0x8007000e;
}

static ID3D11DeviceContext *g_immediate_context;
static ID3D11DeviceContext *d3d11_device_context_create_typed(
    ID3D11Device *device, uint32_t context_type, uint32_t context_flags);

static void D3D11_MS_ABI device_get_immediate_context(ID3D11Device* this, void** ppImmediateContext) {
    (void)this;
    if (!ppImmediateContext) return;
    if (!g_immediate_context)
        g_immediate_context = d3d11_device_context_create_typed(this, 0, 0);
    *ppImmediateContext = g_immediate_context;
    if (g_immediate_context)
        com_addref(g_immediate_context);
    fprintf(stderr, "[D3D11] ID3D11Device::GetImmediateContext -> %p\n",
            (void *)g_immediate_context);
}

static HRESULT D3D11_MS_ABI device_create_deferred_context(ID3D11Device* this, uint32_t ContextFlags, void** ppDeferredContext) {
    (void)this;
    if (!ppDeferredContext) return (HRESULT)0x80070057;
    *ppDeferredContext = d3d11_device_context_create_typed(this, 1, ContextFlags);
    return *ppDeferredContext ? S_OK : (HRESULT)0x8007000e;
}

static HRESULT D3D11_MS_ABI device_not_implemented(void *this, void *a, void *b, void *c) {
    static _Atomic(uint32_t) calls;
    uint32_t call = atomic_fetch_add(&calls, 1) + 1;
    if (call <= 64) {
        void *caller = __builtin_return_address(0);
        fprintf(stderr,
                "[D3D11 DEVICE] unsupported call #%u caller=%p this=%p a=%p b=%p c=%p\n",
                call, caller, this, a, b, c);
    }
    return (HRESULT)0x80004001; /* E_NOTIMPL */
}

static uint32_t D3D11_MS_ABI device_get_feature_level(ID3D11Device *this) {
    (void)this;
    return 0xb000; /* D3D_FEATURE_LEVEL_11_0 */
}

static uint32_t D3D11_MS_ABI device_get_creation_flags(ID3D11Device *this) {
    (void)this;
    return 0;
}

static HRESULT D3D11_MS_ABI device_get_removed_reason(ID3D11Device *this) {
    (void)this;
    return S_OK;
}

static HRESULT D3D11_MS_ABI device_set_exception_mode(ID3D11Device *this, uint32_t flags) {
    (void)this; (void)flags;
    return S_OK;
}

static uint32_t D3D11_MS_ABI device_get_exception_mode(ID3D11Device *this) {
    (void)this;
    return 0;
}

/* BEER_D3D11_CALL_STATS=<file>: count how often the guest calls every vtable slot (device, context,
 * swap chain...), written to <file> every 5 s. Each slot is replaced by a tiny stub that increments a
 * counter and jumps to the original function (only r11, volatile in both ABIs, is used). */
#include <sys/mman.h>
#include <pthread.h>
#include <unistd.h>
typedef struct { const char *name; size_t count; volatile uint64_t *counters; } D3D11StatsTable;
static D3D11StatsTable g_stats_tables[16];
static int g_stats_table_count;
static void *d3d11_stats_thread(void *arg)
{
    const char *path = arg;
    for (;;) {
        sleep(5);
        FILE *f = fopen(path, "w");
        if (!f) continue;
        for (int t = 0; t < g_stats_table_count; ++t)
            for (size_t i = 0; i < g_stats_tables[t].count; ++i)
                if (g_stats_tables[t].counters[i])
                    fprintf(f, "%s %zu %llu\n", g_stats_tables[t].name, i,
                            (unsigned long long)g_stats_tables[t].counters[i]);
        fclose(f);
    }
    return NULL;
}
static void d3d11_stats_wrap(const char *name, void **slots, size_t count)
{
    const char *path = getenv("BEER_D3D11_CALL_STATS");
    if (!path || !*path || g_stats_table_count >= 16) return;
    volatile uint64_t *counters = calloc(count, sizeof(uint64_t));
    uint8_t *code = mmap(NULL, count * 48, PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (!counters || code == MAP_FAILED) return;
    for (size_t i = 0; i < count; ++i) {
        uint8_t *p = code + i * 48;
        uint64_t counter = (uint64_t)(uintptr_t)&counters[i], orig = (uint64_t)(uintptr_t)slots[i];
        size_t o = 0;
        p[o++] = 0x49; p[o++] = 0xBB; memcpy(p + o, &counter, 8); o += 8;     /* mov r11, &counter */
        p[o++] = 0xF0; p[o++] = 0x49; p[o++] = 0xFF; p[o++] = 0x03;           /* lock inc qword [r11] */
        p[o++] = 0x49; p[o++] = 0xBB; memcpy(p + o, &orig, 8); o += 8;        /* mov r11, orig */
        p[o++] = 0x41; p[o++] = 0xFF; p[o++] = 0xE3;                          /* jmp r11 */
        slots[i] = p;
    }
    g_stats_tables[g_stats_table_count++] = (D3D11StatsTable){ name, count, counters };
    static int thread_started;
    if (!thread_started) {
        thread_started = 1;
        pthread_t t;
        pthread_create(&t, NULL, d3d11_stats_thread, (void *)path);
        pthread_detach(t);
    }
}

static ID3D11Device_VTable g_device_vtable;

static void init_device_vtable(void) {
    static int initialized;
    if (initialized) return;
    initialized = 1;
    for (size_t i = 0; i < D3D11_DEVICE_VTABLE_SLOTS; ++i)
        g_device_vtable.slots[i] = (void *)device_not_implemented;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_QUERY_INTERFACE] = (void *)device_query_interface;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_ADD_REF] = (void *)device_addref;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_RELEASE] = (void *)device_release;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_CREATE_BUFFER] = (void *)device_create_buffer;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_CREATE_TEXTURE2D] = (void *)device_create_texture2d;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_CREATE_TEXTURE3D] = (void *)device_create_texture3d;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_CREATE_SHADER_RESOURCE_VIEW] = (void *)device_create_shaderresource_view;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_CREATE_UNORDERED_ACCESS_VIEW] = (void *)device_create_unorderedaccess_view;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_CREATE_RENDER_TARGET_VIEW] = (void *)device_create_rendertarget_view;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_CREATE_DEPTH_STENCIL_VIEW] = (void *)device_create_depthstencil_view;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_CREATE_INPUT_LAYOUT] = (void *)device_create_input_layout;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_CREATE_VERTEX_SHADER] = (void *)device_create_vertex_shader;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_CREATE_GEOMETRY_SHADER] = (void *)device_create_geometry_shader;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_CREATE_PIXEL_SHADER] = (void *)device_create_pixel_shader;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_CREATE_HULL_SHADER] = (void *)device_create_hull_shader;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_CREATE_DOMAIN_SHADER] = (void *)device_create_domain_shader;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_CREATE_COMPUTE_SHADER] = (void *)device_create_compute_shader;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_CREATE_BLEND_STATE] = (void *)device_create_blend_state;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_CREATE_DEPTH_STENCIL_STATE] = (void *)device_create_depthstencil_state;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_CREATE_RASTERIZER_STATE] = (void *)device_create_rasterizer_state;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_CREATE_SAMPLER_STATE] = (void *)device_create_sampler_state;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_CREATE_QUERY] = (void *)device_create_query;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_CREATE_PREDICATE] = (void *)device_create_predicate;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_CREATE_DEFERRED_CONTEXT] = (void *)device_create_deferred_context;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_CHECK_FORMAT_SUPPORT] = (void *)device_check_format_support;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_CHECK_MULTISAMPLE_QUALITY_LEVELS] = (void *)device_check_multisample_quality_levels;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_CHECK_FEATURE_SUPPORT] = (void *)device_check_feature_support;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_GET_FEATURE_LEVEL] = (void *)device_get_feature_level;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_GET_CREATION_FLAGS] = (void *)device_get_creation_flags;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_GET_DEVICE_REMOVED_REASON] = (void *)device_get_removed_reason;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_GET_IMMEDIATE_CONTEXT] = (void *)device_get_immediate_context;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_SET_EXCEPTION_MODE] = (void *)device_set_exception_mode;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_GET_EXCEPTION_MODE] = (void *)device_get_exception_mode;
    d3d11_stats_wrap("device", g_device_vtable.slots, D3D11_DEVICE_VTABLE_SLOTS);
}

ID3D11Device* d3d11_device_create(void) {
    init_device_vtable();
    ID3D11Device* device = (ID3D11Device*)com_alloc(sizeof(ID3D11Device), COM_TYPE_DEVICE, 0xb000);
    if (!device) return NULL;
    device->vtable = g_device_vtable.slots;
    return device;
}

/* ============================================================================
 * ID3D11DeviceContext - Stub implementation
 * ============================================================================ */

static HRESULT D3D11_MS_ABI context_query_interface(ID3D11DeviceContext* this, REFIID riid, LPVOID* ppvObj) {
    (void)riid;
    if (!ppvObj) return (HRESULT)0x80070057;
    *ppvObj = this;
    com_addref(this);
    return S_OK;
}

static void D3D11_MS_ABI context_get_device(
    ID3D11DeviceContext *this, ID3D11Device **device);

static uint32_t D3D11_MS_ABI context_addref(ID3D11DeviceContext* this) {
    return com_addref(this);
}

static uint32_t D3D11_MS_ABI context_release(ID3D11DeviceContext* this) {
    ComObjectHeader *header = com_get_header(this);
    if (!header) return 0;
    /* The process-global pointer is only a non-owning cache. Never leave it
     * pointing at an object whose final reference is about to be released. */
    if (atomic_load(&header->refcount) == 1 && g_immediate_context == this)
        g_immediate_context = NULL;
    return com_release(this);
}

typedef enum {
    BEER_COMMAND_UNKNOWN,
    BEER_COMMAND_IA_INPUT_LAYOUT,
    BEER_COMMAND_IA_VERTEX_BUFFERS,
    BEER_COMMAND_IA_INDEX_BUFFER,
    BEER_COMMAND_IA_TOPOLOGY,
    BEER_COMMAND_VS_SHADER,
    BEER_COMMAND_PS_SHADER,
    BEER_COMMAND_GS_SHADER,
    BEER_COMMAND_DRAW_INDEXED,
    BEER_COMMAND_DRAW,
    BEER_COMMAND_DRAW_INDEXED_INSTANCED,
    BEER_COMMAND_DRAW_INSTANCED,
    BEER_COMMAND_DRAW_AUTO,
    BEER_COMMAND_CLEAR_RENDER_TARGET,
    BEER_COMMAND_COPY_RESOURCE,
    BEER_COMMAND_UPDATE_SUBRESOURCE,
    BEER_COMMAND_BEGIN_QUERY,
    BEER_COMMAND_END_QUERY
} BeerD3D11CommandType;

#define BEER_MAX_VERTEX_BUFFERS 4
#define BEER_MAX_CONSTANT_BUFFERS 8
#define BEER_MAX_SHADER_RESOURCES 8
#define BEER_MAX_SAMPLERS 8
#define BEER_MAX_RENDER_TARGETS 8
#define BEER_MAX_VIEWPORTS 8

typedef struct {
    void *input_layout;
    void *vertex_shader;
    void *pixel_shader;
    void *geometry_shader;
    void *hull_shader;
    void *domain_shader;
    void *vertex_buffers[BEER_MAX_VERTEX_BUFFERS];
    uint32_t vertex_strides[BEER_MAX_VERTEX_BUFFERS];
    uint32_t vertex_offsets[BEER_MAX_VERTEX_BUFFERS];
    void *index_buffer;
    uint32_t index_format;
    uint32_t index_offset;
    void *vs_constant_buffers[BEER_MAX_CONSTANT_BUFFERS];
    void *ps_constant_buffers[BEER_MAX_CONSTANT_BUFFERS];
    void *hs_constant_buffers[BEER_MAX_CONSTANT_BUFFERS];
    void *ds_constant_buffers[BEER_MAX_CONSTANT_BUFFERS];
    void *ps_shader_resources[BEER_MAX_SHADER_RESOURCES];
    void *hs_shader_resources[BEER_MAX_SHADER_RESOURCES];
    void *ds_shader_resources[BEER_MAX_SHADER_RESOURCES];
    void *ps_samplers[BEER_MAX_SAMPLERS];
    void *hs_samplers[BEER_MAX_SAMPLERS];
    void *ds_samplers[BEER_MAX_SAMPLERS];
    void *render_targets[BEER_MAX_RENDER_TARGETS];
    void *depth_stencil_view;
    void *blend_state;
    float blend_factor[4];
    uint32_t sample_mask;
    void *depth_stencil_state;
    uint32_t stencil_ref;
    void *rasterizer_state;
    uint32_t topology;
    uint32_t viewport_count;
    float viewports[BEER_MAX_VIEWPORTS][6];
    uint32_t scissor_count;
    int32_t scissor_rects[BEER_MAX_VIEWPORTS][4];
} BeerD3D11PipelineState;

typedef struct {
    BeerD3D11CommandType type;
    void *object;
    void *second_object;
    int owns_reference;
    int owns_second_reference;
    BeerD3D11PipelineState *draw_state;
    union {
        struct { uint32_t a, b, c, d; int32_t signed_value; } integers;
        float color[4];
    } args;
    void *owned_data;
    size_t owned_data_size;
} BeerD3D11Command;

#define BEER_MAX_MAPPED_RESOURCES 32

typedef struct {
    void *resource;
    uint32_t subresource;
    uint32_t row_pitch;
    uint32_t depth_pitch;
    uint32_t map_type;
    void *data;
    void *baseline;
    size_t size;
} BeerD3D11MappedResource;

typedef struct {
    void **vtable;
    uint32_t type;
    uint32_t flags;
    ID3D11Device *device;
    BeerD3D11Command *commands;
    size_t command_count;
    size_t command_capacity;
    BeerD3D11PipelineState state;
    BeerD3D11MappedResource mapped[BEER_MAX_MAPPED_RESOURCES];
} BeerD3D11DeviceContext;

typedef struct {
    void **vtable;
    ID3D11Device *device;
    BeerD3D11Command *commands;
    size_t command_count;
    uint32_t context_flags;
    uint32_t indexed_draw_count;
    uint32_t finalize_sequence;
    _Atomic(uint32_t) executed;
} BeerD3D11CommandList;

static void D3D11_MS_ABI context_get_device(
    ID3D11DeviceContext *this, ID3D11Device **device)
{
    if (!device) return;
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    *device = context ? context->device : NULL;
    if (*device) device_addref(*device);
}

static void state_replace_object(void **slot, void *object)
{
    if (*slot == object) return;
    if (object && com_get_header(object)) com_addref(object);
    if (*slot && com_get_header(*slot)) com_release(*slot);
    *slot = object;
}

static void pipeline_state_release(BeerD3D11PipelineState *state)
{
    if (!state) return;
    state_replace_object(&state->input_layout, NULL);
    state_replace_object(&state->vertex_shader, NULL);
    state_replace_object(&state->pixel_shader, NULL);
    state_replace_object(&state->geometry_shader, NULL);
    state_replace_object(&state->hull_shader, NULL);
    state_replace_object(&state->domain_shader, NULL);
    for (size_t i = 0; i < BEER_MAX_VERTEX_BUFFERS; ++i)
        state_replace_object(&state->vertex_buffers[i], NULL);
    state_replace_object(&state->index_buffer, NULL);
    for (size_t i = 0; i < BEER_MAX_CONSTANT_BUFFERS; ++i) {
        state_replace_object(&state->vs_constant_buffers[i], NULL);
        state_replace_object(&state->ps_constant_buffers[i], NULL);
        state_replace_object(&state->hs_constant_buffers[i], NULL);
        state_replace_object(&state->ds_constant_buffers[i], NULL);
    }
    for (size_t i = 0; i < BEER_MAX_SHADER_RESOURCES; ++i) {
        state_replace_object(&state->ps_shader_resources[i], NULL);
        state_replace_object(&state->hs_shader_resources[i], NULL);
        state_replace_object(&state->ds_shader_resources[i], NULL);
    }
    for (size_t i = 0; i < BEER_MAX_SAMPLERS; ++i) {
        state_replace_object(&state->ps_samplers[i], NULL);
        state_replace_object(&state->hs_samplers[i], NULL);
        state_replace_object(&state->ds_samplers[i], NULL);
    }
    for (size_t i = 0; i < BEER_MAX_RENDER_TARGETS; ++i)
        state_replace_object(&state->render_targets[i], NULL);
    state_replace_object(&state->depth_stencil_view, NULL);
    state_replace_object(&state->blend_state, NULL);
    state_replace_object(&state->depth_stencil_state, NULL);
    state_replace_object(&state->rasterizer_state, NULL);
}

static BeerD3D11PipelineState *pipeline_state_clone(
    const BeerD3D11PipelineState *source)
{
    BeerD3D11PipelineState *copy = calloc(1, sizeof(*copy));
    if (!copy) return NULL;
    *copy = *source;
#define RETAIN_STATE_OBJECT(field) \
    do { if (copy->field && com_get_header(copy->field)) com_addref(copy->field); } while (0)
    RETAIN_STATE_OBJECT(input_layout);
    RETAIN_STATE_OBJECT(vertex_shader);
    RETAIN_STATE_OBJECT(pixel_shader);
    RETAIN_STATE_OBJECT(geometry_shader);
    RETAIN_STATE_OBJECT(hull_shader);
    RETAIN_STATE_OBJECT(domain_shader);
    for (size_t i = 0; i < BEER_MAX_VERTEX_BUFFERS; ++i)
        if (copy->vertex_buffers[i] && com_get_header(copy->vertex_buffers[i]))
            com_addref(copy->vertex_buffers[i]);
    RETAIN_STATE_OBJECT(index_buffer);
    for (size_t i = 0; i < BEER_MAX_CONSTANT_BUFFERS; ++i) {
        if (copy->vs_constant_buffers[i] && com_get_header(copy->vs_constant_buffers[i]))
            com_addref(copy->vs_constant_buffers[i]);
        if (copy->ps_constant_buffers[i] && com_get_header(copy->ps_constant_buffers[i]))
            com_addref(copy->ps_constant_buffers[i]);
        if (copy->hs_constant_buffers[i] && com_get_header(copy->hs_constant_buffers[i]))
            com_addref(copy->hs_constant_buffers[i]);
        if (copy->ds_constant_buffers[i] && com_get_header(copy->ds_constant_buffers[i]))
            com_addref(copy->ds_constant_buffers[i]);
    }
    for (size_t i = 0; i < BEER_MAX_SHADER_RESOURCES; ++i) {
        if (copy->ps_shader_resources[i] && com_get_header(copy->ps_shader_resources[i]))
            com_addref(copy->ps_shader_resources[i]);
        if (copy->hs_shader_resources[i] && com_get_header(copy->hs_shader_resources[i]))
            com_addref(copy->hs_shader_resources[i]);
        if (copy->ds_shader_resources[i] && com_get_header(copy->ds_shader_resources[i]))
            com_addref(copy->ds_shader_resources[i]);
    }
    for (size_t i = 0; i < BEER_MAX_SAMPLERS; ++i) {
        if (copy->ps_samplers[i] && com_get_header(copy->ps_samplers[i]))
            com_addref(copy->ps_samplers[i]);
        if (copy->hs_samplers[i] && com_get_header(copy->hs_samplers[i]))
            com_addref(copy->hs_samplers[i]);
        if (copy->ds_samplers[i] && com_get_header(copy->ds_samplers[i]))
            com_addref(copy->ds_samplers[i]);
    }
    for (size_t i = 0; i < BEER_MAX_RENDER_TARGETS; ++i)
        if (copy->render_targets[i] && com_get_header(copy->render_targets[i]))
            com_addref(copy->render_targets[i]);
    RETAIN_STATE_OBJECT(depth_stencil_view);
    RETAIN_STATE_OBJECT(blend_state);
    RETAIN_STATE_OBJECT(depth_stencil_state);
    RETAIN_STATE_OBJECT(rasterizer_state);
#undef RETAIN_STATE_OBJECT
    return copy;
}

static const char *command_type_name(BeerD3D11CommandType type)
{
    switch (type) {
    case BEER_COMMAND_IA_INPUT_LAYOUT: return "IASetInputLayout";
    case BEER_COMMAND_IA_VERTEX_BUFFERS: return "IASetVertexBuffers";
    case BEER_COMMAND_IA_INDEX_BUFFER: return "IASetIndexBuffer";
    case BEER_COMMAND_IA_TOPOLOGY: return "IASetPrimitiveTopology";
    case BEER_COMMAND_VS_SHADER: return "VSSetShader";
    case BEER_COMMAND_PS_SHADER: return "PSSetShader";
    case BEER_COMMAND_GS_SHADER: return "GSSetShader";
    case BEER_COMMAND_DRAW_INDEXED: return "DrawIndexed";
    case BEER_COMMAND_DRAW: return "Draw";
    case BEER_COMMAND_DRAW_INDEXED_INSTANCED: return "DrawIndexedInstanced";
    case BEER_COMMAND_DRAW_INSTANCED: return "DrawInstanced";
    case BEER_COMMAND_DRAW_AUTO: return "DrawAuto";
    case BEER_COMMAND_CLEAR_RENDER_TARGET: return "ClearRenderTargetView";
    case BEER_COMMAND_COPY_RESOURCE: return "CopyResource";
    case BEER_COMMAND_UPDATE_SUBRESOURCE: return "UpdateSubresource";
    case BEER_COMMAND_BEGIN_QUERY: return "Begin";
    case BEER_COMMAND_END_QUERY: return "End";
    default: return "Unknown";
    }
}

static int context_record_command(ID3D11DeviceContext *object,
                                  const BeerD3D11Command *command)
{
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)object;
    if (!context || context->type != 1) return 0;
    if (context->command_count == context->command_capacity) {
        size_t capacity = context->command_capacity
            ? context->command_capacity * 2 : 64;
        BeerD3D11Command *commands = realloc(
            context->commands, capacity * sizeof(*commands));
        if (!commands) return 1;
        context->commands = commands;
        context->command_capacity = capacity;
    }
    context->commands[context->command_count] = *command;
    context->commands[context->command_count].owns_reference = 0;
    context->commands[context->command_count].owns_second_reference = 0;
    /* draw_state is already an ownership-bearing snapshot. Transfer it into
     * the command array and clear it in the caller-visible command so a later
     * recording failure cannot leave an ambiguous second owner. */
    if (command->object && com_get_header(command->object)) {
        com_addref(command->object);
        context->commands[context->command_count].owns_reference = 1;
    }
    if (command->second_object && com_get_header(command->second_object)) {
        com_addref(command->second_object);
        context->commands[context->command_count].owns_second_reference = 1;
    }
    ++context->command_count;
    return 1;
}

static void command_release_references(BeerD3D11Command *commands, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (commands[i].owns_reference) com_release(commands[i].object);
        if (commands[i].owns_second_reference)
            com_release(commands[i].second_object);
        if (commands[i].type == BEER_COMMAND_IA_VERTEX_BUFFERS &&
            commands[i].owned_data) {
            void **buffers = commands[i].owned_data;
            for (uint32_t slot = 0; slot < commands[i].args.integers.b; ++slot)
                if (buffers[slot] && com_get_header(buffers[slot]))
                    com_release(buffers[slot]);
        }
        free(commands[i].owned_data);
        if (commands[i].draw_state) {
            pipeline_state_release(commands[i].draw_state);
            free(commands[i].draw_state);
        }
    }
}

static void trace_binding(const char *name, const void *object, uint32_t count)
{
    static _Atomic(uint32_t) calls;
    uint32_t call = atomic_fetch_add(&calls, 1) + 1;
    if (call <= 80)
        fprintf(stderr, "[D3D11 BIND] %s #%u object=%p count=%u\n",
                name, call, object, count);
}

static void D3D11_MS_ABI context_ia_set_input_layout(ID3D11DeviceContext* this, void* pInputLayout) {
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (context) state_replace_object(&context->state.input_layout, pInputLayout);
    BeerD3D11Command command = { .type = BEER_COMMAND_IA_INPUT_LAYOUT,
                                 .object = pInputLayout };
    if (context_record_command(this, &command)) return;
    trace_binding("IASetInputLayout", pInputLayout, pInputLayout ? 1 : 0);
}

static void trace_resource_binding(const char *name, ID3D11DeviceContext *context,
                                   uint32_t start_slot, uint32_t count,
                                   void *const *objects, const uint32_t *values_a,
                                   const uint32_t *values_b)
{
    static _Atomic(uint32_t) calls;
    uint32_t call = atomic_fetch_add(&calls, 1) + 1;
    if (call > 96) return;
    fprintf(stderr, "[D3D11 STATE] %s #%u context=%p start=%u count=%u",
            name, call, (void *)context, start_slot, count);
    uint32_t shown = count < 4 ? count : 4;
    for (uint32_t i = 0; i < shown; ++i) {
        void *object = objects ? objects[i] : NULL;
        fprintf(stderr, " [%u]=%p", start_slot + i, object);
        if (values_a) fprintf(stderr, "/%u", values_a[i]);
        if (values_b) fprintf(stderr, "/%u", values_b[i]);
        if (object && com_get_header(object)) {
            BeerD3D11Resource *resource = (BeerD3D11Resource *)object;
            if (resource->dimension == 1 && resource->desc_size >= 24)
                fprintf(stderr, "{bytes=%u bind=0x%x cpu=0x%x}",
                        ((uint32_t *)resource->desc)[0],
                        ((uint32_t *)resource->desc)[4],
                        ((uint32_t *)resource->desc)[5]);
        }
    }
    if (count > shown) fprintf(stderr, " ...");
    fputc('\n', stderr);
}

static void D3D11_MS_ABI context_ia_set_vertex_buffers(ID3D11DeviceContext* this, uint32_t StartSlot, uint32_t NumBuffers, void* ppVertexBuffers, void* pStrides, void* pOffsets) {
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    void *const *buffers = ppVertexBuffers;
    const uint32_t *strides = pStrides, *offsets = pOffsets;
    uint32_t limit = 0;
    if (context && StartSlot < BEER_MAX_VERTEX_BUFFERS) {
        limit = NumBuffers;
        if (limit > BEER_MAX_VERTEX_BUFFERS - StartSlot)
            limit = BEER_MAX_VERTEX_BUFFERS - StartSlot;
        for (uint32_t i = 0; i < limit; ++i) {
            state_replace_object(&context->state.vertex_buffers[StartSlot + i],
                                 buffers ? buffers[i] : NULL);
            context->state.vertex_strides[StartSlot + i] = strides ? strides[i] : 0;
            context->state.vertex_offsets[StartSlot + i] = offsets ? offsets[i] : 0;
        }
    }
    BeerD3D11Command command = { .type = BEER_COMMAND_IA_VERTEX_BUFFERS };
    command.args.integers.a = StartSlot;
    command.args.integers.b = limit;
    if (limit) {
        size_t pointer_bytes = (size_t)limit * sizeof(void *);
        size_t value_bytes = (size_t)limit * sizeof(uint32_t);
        command.owned_data_size = pointer_bytes + value_bytes * 2;
        command.owned_data = calloc(1, command.owned_data_size);
        if (command.owned_data) {
            void **saved_buffers = command.owned_data;
            uint32_t *saved_strides = (uint32_t *)((uint8_t *)command.owned_data +
                                                   pointer_bytes);
            uint32_t *saved_offsets = saved_strides + limit;
            for (uint32_t i = 0; i < limit; ++i) {
                saved_buffers[i] = buffers ? buffers[i] : NULL;
                if (saved_buffers[i] && com_get_header(saved_buffers[i]))
                    com_addref(saved_buffers[i]);
                saved_strides[i] = strides ? strides[i] : 0;
                saved_offsets[i] = offsets ? offsets[i] : 0;
            }
        }
    }
    if (context_record_command(this, &command)) return;
    if (command.owned_data) {
        void **saved_buffers = command.owned_data;
        for (uint32_t i = 0; i < limit; ++i)
            if (saved_buffers[i] && com_get_header(saved_buffers[i]))
                com_release(saved_buffers[i]);
        free(command.owned_data);
    }
    trace_resource_binding("IASetVertexBuffers", this, StartSlot, NumBuffers,
                           buffers, strides, offsets);
}

static void D3D11_MS_ABI context_ia_set_index_buffer(ID3D11DeviceContext* this, void* pIndexBuffer, uint32_t Format, uint32_t Offset) {
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (context) {
        state_replace_object(&context->state.index_buffer, pIndexBuffer);
        context->state.index_format = Format;
        context->state.index_offset = Offset;
    }
    BeerD3D11Command command = { .type = BEER_COMMAND_IA_INDEX_BUFFER,
                                 .object = pIndexBuffer };
    command.args.integers.a = Format;
    command.args.integers.b = Offset;
    if (context_record_command(this, &command)) return;
    void *objects[1] = { pIndexBuffer };
    uint32_t formats[1] = { Format }, offsets[1] = { Offset };
    trace_resource_binding("IASetIndexBuffer", this, 0, 1, objects, formats, offsets);
}

static void D3D11_MS_ABI context_ia_set_primitive_topology(ID3D11DeviceContext* this, uint32_t Topology) {
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (context) context->state.topology = Topology;
    BeerD3D11Command command = { .type = BEER_COMMAND_IA_TOPOLOGY };
    command.args.integers.a = Topology;
    if (context_record_command(this, &command)) return;
    static _Atomic(uint32_t) calls;
    uint32_t call = atomic_fetch_add(&calls, 1) + 1;
    if (render_diagnostics_enabled() && call <= 64)
        fprintf(stderr, "[D3D11 STATE] topology #%u context=%p value=%u\n",
                call, (void *)this, Topology);
}

static void D3D11_MS_ABI context_vs_set_shader(ID3D11DeviceContext* this, void* pVertexShader, void* ppClassInstances, uint32_t NumClassInstances) {
    (void)ppClassInstances;
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (context) state_replace_object(&context->state.vertex_shader, pVertexShader);
    BeerD3D11Command command = { .type = BEER_COMMAND_VS_SHADER,
                                 .object = pVertexShader };
    command.args.integers.a = NumClassInstances;
    if (context_record_command(this, &command)) return;
    trace_binding("VSSetShader", pVertexShader, NumClassInstances);
}

static void set_state_objects(void **slots, uint32_t capacity,
                              uint32_t start, uint32_t count, void *objects)
{
    void *const *source = objects;
    if (start >= capacity) return;
    if (count > capacity - start) count = capacity - start;
    for (uint32_t i = 0; i < count; ++i)
        state_replace_object(&slots[start + i], source ? source[i] : NULL);
}

static void D3D11_MS_ABI context_vs_set_constant_buffers(ID3D11DeviceContext* this, uint32_t StartSlot, uint32_t NumBuffers, void* ppConstantBuffers) {
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (context) set_state_objects(context->state.vs_constant_buffers,
        BEER_MAX_CONSTANT_BUFFERS, StartSlot, NumBuffers, ppConstantBuffers);
    trace_resource_binding("VSSetConstantBuffers", this, StartSlot, NumBuffers,
                           (void *const *)ppConstantBuffers, NULL, NULL);
}

static void D3D11_MS_ABI context_ps_set_shader(ID3D11DeviceContext* this, void* pPixelShader, void* ppClassInstances, uint32_t NumClassInstances) {
    (void)ppClassInstances;
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (context) state_replace_object(&context->state.pixel_shader, pPixelShader);
    BeerD3D11Command command = { .type = BEER_COMMAND_PS_SHADER,
                                 .object = pPixelShader };
    command.args.integers.a = NumClassInstances;
    if (context_record_command(this, &command)) return;
    trace_binding("PSSetShader", pPixelShader, NumClassInstances);
}

static void D3D11_MS_ABI context_ps_set_constant_buffers(ID3D11DeviceContext* this, uint32_t StartSlot, uint32_t NumBuffers, void* ppConstantBuffers) {
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (context) set_state_objects(context->state.ps_constant_buffers,
        BEER_MAX_CONSTANT_BUFFERS, StartSlot, NumBuffers, ppConstantBuffers);
    trace_resource_binding("PSSetConstantBuffers", this, StartSlot, NumBuffers,
                           (void *const *)ppConstantBuffers, NULL, NULL);
}

static void D3D11_MS_ABI context_ps_set_shader_resources(ID3D11DeviceContext* this, uint32_t StartSlot, uint32_t NumViews, void* ppShaderResourceViews) {
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (context) set_state_objects(context->state.ps_shader_resources,
        BEER_MAX_SHADER_RESOURCES, StartSlot, NumViews, ppShaderResourceViews);
    trace_resource_binding("PSSetShaderResources", this, StartSlot, NumViews,
                           (void *const *)ppShaderResourceViews, NULL, NULL);
}

static void D3D11_MS_ABI context_ps_set_samplers(ID3D11DeviceContext* this, uint32_t StartSlot, uint32_t NumSamplers, void* ppSamplers) {
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (context) set_state_objects(context->state.ps_samplers,
        BEER_MAX_SAMPLERS, StartSlot, NumSamplers, ppSamplers);
    trace_resource_binding("PSSetSamplers", this, StartSlot, NumSamplers,
                           (void *const *)ppSamplers, NULL, NULL);
}

static void D3D11_MS_ABI context_gs_set_shader(ID3D11DeviceContext* this, void* pGeometryShader, void* ppClassInstances, uint32_t NumClassInstances) {
    (void)ppClassInstances;
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (context) state_replace_object(&context->state.geometry_shader, pGeometryShader);
    BeerD3D11Command command = { .type = BEER_COMMAND_GS_SHADER,
                                 .object = pGeometryShader };
    command.args.integers.a = NumClassInstances;
    if (context_record_command(this, &command)) return;
    trace_binding("GSSetShader", pGeometryShader, NumClassInstances);
}

static uint32_t fnv1a_bytes(const uint8_t *data, size_t size);

static int execute_initial_indexed_draw(const BeerD3D11PipelineState *state,
                                        uint32_t index_count,
                                        uint32_t start_index,
                                        int32_t base_vertex);
static void capture_indexed_draw_snapshot(const BeerD3D11PipelineState *state,
                                          uint32_t index_count,
                                          uint32_t start_index,
                                          int32_t base_vertex);

static void D3D11_MS_ABI context_draw_indexed(ID3D11DeviceContext* this, uint32_t IndexCount, uint32_t StartIndexLocation, int32_t BaseVertexLocation) {
    BeerD3D11Command command = { .type = BEER_COMMAND_DRAW_INDEXED };
    command.args.integers.a = IndexCount;
    command.args.integers.b = StartIndexLocation;
    command.args.integers.signed_value = BaseVertexLocation;
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (context && context->type == 1)
        command.draw_state = pipeline_state_clone(&context->state);
    if (context_record_command(this, &command)) return;
    capture_indexed_draw_snapshot(context ? &context->state : NULL,
                                  IndexCount, StartIndexLocation,
                                  BaseVertexLocation);
    int executed = execute_initial_indexed_draw(
        context ? &context->state : NULL, IndexCount, StartIndexLocation,
        BaseVertexLocation);
    if (!executed) {
        const BeerD3D11PipelineState *failed = context ? &context->state : NULL;
        BeerD3D11Shader *failed_vs = failed ? failed->vertex_shader : NULL;
        BeerD3D11Shader *failed_ps = failed ? failed->pixel_shader : NULL;
        fprintf(stderr,
                "[D3D11 VULKAN REJECT] DrawIndexed indices=%u start=%u base=%d "
                "topology=%u stride=%u index-format=%u viewports=%u "
                "VS=%08x PS=%08x VB=%p IB=%p RT=%p\n",
                IndexCount, StartIndexLocation, BaseVertexLocation,
                failed ? failed->topology : 0,
                failed ? failed->vertex_strides[0] : 0,
                failed ? failed->index_format : 0,
                failed ? failed->viewport_count : 0,
                failed_vs && com_get_header(failed_vs)
                    ? fnv1a_bytes(failed_vs->bytecode, failed_vs->bytecode_size) : 0,
                failed_ps && com_get_header(failed_ps)
                    ? fnv1a_bytes(failed_ps->bytecode, failed_ps->bytecode_size) : 0,
                failed ? failed->vertex_buffers[0] : NULL,
                failed ? failed->index_buffer : NULL,
                failed ? failed->render_targets[0] : NULL);
        strict_vulkan_failure("unsupported DrawIndexed state");
    }
    if (g_last_textured_indexed_target &&
        atomic_load(&g_last_textured_indexed_target->write_serial)) {
        static _Atomic(uint32_t) flow_calls;
        uint32_t flow_call = atomic_fetch_add(&flow_calls, 1) + 1;
        if (flow_call <= 64) {
            const BeerD3D11PipelineState *flow_state = context ? &context->state : NULL;
            BeerD3D11Shader *flow_vs = flow_state ? flow_state->vertex_shader : NULL;
            BeerD3D11Shader *flow_ps = flow_state ? flow_state->pixel_shader : NULL;
            BeerD3D11View *flow_target_view = flow_state
                ? flow_state->render_targets[0] : NULL;
            BeerD3D11Resource *flow_target = flow_target_view &&
                com_get_header(flow_target_view)
                ? validated_resource(flow_target_view->resource) : NULL;
            BeerD3D11View *flow_source_view = flow_state
                ? flow_state->ps_shader_resources[0] : NULL;
            BeerD3D11Resource *flow_source = flow_source_view &&
                com_get_header(flow_source_view)
                ? validated_resource(flow_source_view->resource) : NULL;
            fprintf(stderr,
                    "[D3D11 FLOW DRAW] #%u executed=%d VS=%08x PS=%08x "
                    "target=%p target-serial=%llu source0=%p source0-serial=%llu\n",
                    flow_call, executed,
                    flow_vs && com_get_header(flow_vs)
                        ? fnv1a_bytes(flow_vs->bytecode, flow_vs->bytecode_size) : 0,
                    flow_ps && com_get_header(flow_ps)
                        ? fnv1a_bytes(flow_ps->bytecode, flow_ps->bytecode_size) : 0,
                    (void *)flow_target,
                    (unsigned long long)(flow_target
                        ? atomic_load(&flow_target->write_serial) : 0),
                    (void *)flow_source,
                    (unsigned long long)(flow_source
                        ? atomic_load(&flow_source->write_serial) : 0));
        }
    }
    static _Atomic(uint32_t) calls;
    uint32_t call = atomic_fetch_add(&calls, 1) + 1;
    if (!executed) {
        BeerD3D11Shader *debug_vs = context && context->state.vertex_shader
            ? (BeerD3D11Shader *)context->state.vertex_shader : NULL;
        BeerD3D11Shader *debug_ps = context && context->state.pixel_shader
            ? (BeerD3D11Shader *)context->state.pixel_shader : NULL;
        uint32_t debug_vs_hash = debug_vs && com_get_header(debug_vs)
            ? fnv1a_bytes(debug_vs->bytecode, debug_vs->bytecode_size) : 0;
        uint32_t debug_ps_hash = debug_ps && com_get_header(debug_ps)
            ? fnv1a_bytes(debug_ps->bytecode, debug_ps->bytecode_size) : 0;
        if (debug_vs_hash == 0x94e79d4e && debug_ps_hash == 0xacdd04cc)
            fprintf(stderr, "[D3D11 REFERENCE] textured draw rejected call=%u "
                    "indices=%u stride=%u viewport-count=%u\n", call,
                    IndexCount, context->state.vertex_strides[0],
                    context->state.viewport_count);
    }
    if (render_diagnostics_enabled() && (call <= 8 || (call % 32) == 0)) {
        const BeerD3D11PipelineState *state = context ? &context->state : NULL;
        BeerD3D11Shader *vs = state ? state->vertex_shader : NULL;
        BeerD3D11Shader *ps = state ? state->pixel_shader : NULL;
        uint32_t vs_hash = vs && com_get_header(vs)
            ? fnv1a_bytes(vs->bytecode, vs->bytecode_size) : 0;
        uint32_t ps_hash = ps && com_get_header(ps)
            ? fnv1a_bytes(ps->bytecode, ps->bytecode_size) : 0;
        BeerD3D11View *target_view = state ? state->render_targets[0] : NULL;
        BeerD3D11Resource *target = target_view && com_get_header(target_view)
            ? validated_resource(target_view->resource) : NULL;
        const uint32_t *target_desc = target ? (const uint32_t *)target->desc : NULL;
        fprintf(stderr,
                "[D3D11 TRACE] DrawIndexed #%u indices=%u start=%u base=%d "
                "topology=%u VB=%p/%u/%u IB=%p/F%u/%u VS=%08x PS=%08x "
                "RT=%p/%ux%u/F%u\n",
                call, IndexCount, StartIndexLocation, BaseVertexLocation,
                state ? state->topology : 0,
                state ? state->vertex_buffers[0] : NULL,
                state ? state->vertex_strides[0] : 0,
                state ? state->vertex_offsets[0] : 0,
                state ? state->index_buffer : NULL,
                state ? state->index_format : 0,
                state ? state->index_offset : 0,
                vs_hash, ps_hash, target,
                target_desc ? target_desc[0] : 0,
                target_desc ? target_desc[1] : 0,
                target_desc ? target_desc[4] : 0);
        if (call <= 8 || executed)
            fprintf(stderr, "[D3D11 TRACE] DrawIndexed #%u rendered=%d backend=%s\n",
                    call, executed, d3d11_renderer_name(g_renderer));
    }
    (void)this;
}

static void capture_draw_blob(const char *kind, uint32_t hash,
                              const void *data, size_t size)
{
    const char *directory = getenv("BEER_D3D11_CAPTURE_DIR");
    if (!directory || !*directory || !kind || !data || !size) return;
    if (mkdir(directory, 0700) != 0 && errno != EEXIST) return;

    char path[512];
    int length = snprintf(path, sizeof(path), "%s/%s-%08x.bin",
                          directory, kind, hash);
    if (length <= 0 || (size_t)length >= sizeof(path)) return;

    FILE *existing = fopen(path, "rb");
    if (existing) {
        fclose(existing);
        return;
    }
    FILE *output = fopen(path, "wb");
    if (!output) return;
    if (fwrite(data, 1, size, output) != size)
        fprintf(stderr, "[D3D11 CAPTURE] short write for %s\n", path);
    else
        fprintf(stderr, "[D3D11 CAPTURE] wrote %s (%zu bytes)\n", path, size);
    fclose(output);
}

static void trace_draw_snapshot(const BeerD3D11PipelineState *state,
                                uint32_t vertex_count, uint32_t start_vertex)
{
    static _Atomic(uint32_t) snapshots;
    static uint64_t captured_pairs[32];
    static pthread_mutex_t capture_mutex = PTHREAD_MUTEX_INITIALIZER;
    uint32_t snapshot = atomic_fetch_add(&snapshots, 1) + 1;
    if (!state) return;

    const char *capture_directory = getenv("BEER_D3D11_CAPTURE_DIR");
    if (snapshot > 8 && (!capture_directory || !*capture_directory)) return;
    if (capture_directory && *capture_directory) {
        BeerD3D11Shader *vs = (BeerD3D11Shader *)state->vertex_shader;
        BeerD3D11Shader *ps = (BeerD3D11Shader *)state->pixel_shader;
        if (!vs || !ps || !com_get_header(vs) || !com_get_header(ps)) return;
        uint32_t vs_hash = fnv1a_bytes(vs->bytecode, vs->bytecode_size);
        uint32_t ps_hash = fnv1a_bytes(ps->bytecode, ps->bytecode_size);
        uint64_t pair = ((uint64_t)vs_hash << 32) | ps_hash;
        int fresh = 0;
        pthread_mutex_lock(&capture_mutex);
        for (uint32_t i = 0; i < 32; ++i) {
            if (captured_pairs[i] == pair) break;
            if (!captured_pairs[i]) {
                captured_pairs[i] = pair;
                fresh = 1;
                break;
            }
        }
        pthread_mutex_unlock(&capture_mutex);
        if (!fresh) return;
    }
    fprintf(stderr, "[D3D11 DRAW STATE] #%u vertices=%u start=%u topology=%u layout=%p VS=%p HS=%p DS=%p PS=%p VB=%p stride=%u offset=%u RTV=%p viewports=%u",
            snapshot, vertex_count, start_vertex, state->topology,
            state->input_layout, state->vertex_shader, state->hull_shader,
            state->domain_shader, state->pixel_shader,
            state->vertex_buffers[0], state->vertex_strides[0],
            state->vertex_offsets[0], state->render_targets[0],
            state->viewport_count);
    if (state->viewport_count) {
        uint32_t bits[6];
        memcpy(bits, state->viewports[0], sizeof(bits));
        fprintf(stderr, " viewport-bits=(%08x,%08x %08xx%08x %08x..%08x)",
                bits[0], bits[1], bits[2], bits[3], bits[4], bits[5]);
    }
    fputc('\n', stderr);
    BeerD3D11Resource *vertex_buffer = validated_resource(state->vertex_buffers[0]);
    size_t offset = (size_t)state->vertex_offsets[0] +
        (size_t)start_vertex * state->vertex_strides[0];
    size_t bytes = (size_t)vertex_count * state->vertex_strides[0];
    if (vertex_buffer && vertex_buffer->pixels && offset <= vertex_buffer->pixel_size &&
        bytes <= vertex_buffer->pixel_size - offset) {
        const uint8_t *data = vertex_buffer->pixels + offset;
        size_t shown = bytes < 192 ? bytes : 192;
        fprintf(stderr, "[D3D11 DRAW DATA] vertex-bytes=%zu", bytes);
        for (size_t i = 0; i < shown; ++i)
            fprintf(stderr, "%s%02x", i % 16 ? " " : "\n  ", data[i]);
        fputc('\n', stderr);
    }
    if (state->input_layout && com_get_header(state->input_layout)) {
        BeerD3D11InputLayout *layout = state->input_layout;
        for (uint32_t i = 0; i < layout->element_count; ++i) {
            BeerD3D11InputElement *element = &layout->elements[i];
            fprintf(stderr, "[D3D11 DRAW LAYOUT] element=%u semantic=%s%u format=%u slot=%u offset=%u class=%u step=%u\n",
                    i, element->semantic, element->semantic_index, element->format,
                    element->input_slot, element->aligned_byte_offset,
                    element->input_slot_class, element->instance_step_rate);
        }
    }
    BeerD3D11Shader *shaders[2] = {
        (BeerD3D11Shader *)state->vertex_shader,
        (BeerD3D11Shader *)state->pixel_shader
    };
    const char *shader_names[2] = { "VS", "PS" };
    for (uint32_t i = 0; i < 2; ++i) {
        BeerD3D11Shader *shader = shaders[i];
        if (!shader || !com_get_header(shader)) continue;
        uint32_t hash = 2166136261u;
        for (size_t j = 0; j < shader->bytecode_size; ++j)
            hash = (hash ^ shader->bytecode[j]) * 16777619u;
        fprintf(stderr, "[D3D11 DRAW SHADER] stage=%s bytes=%zu fnv1a=%08x header=",
                shader_names[i], shader->bytecode_size, hash);
        capture_draw_blob(shader_names[i], hash, shader->bytecode,
                          shader->bytecode_size);
        size_t shown = shader->bytecode_size < 32 ? shader->bytecode_size : 32;
        for (size_t j = 0; j < shown; ++j) fprintf(stderr, "%02x", shader->bytecode[j]);
        fputc('\n', stderr);
    }
    for (uint32_t i = 0; i < BEER_MAX_CONSTANT_BUFFERS; ++i) {
        BeerD3D11Resource *vs_buffer = validated_resource(state->vs_constant_buffers[i]);
        BeerD3D11Resource *ps_buffer = validated_resource(state->ps_constant_buffers[i]);
        BeerD3D11Resource *buffers[2] = { vs_buffer, ps_buffer };
        const char *stages[2] = { "VS", "PS" };
        for (uint32_t stage = 0; stage < 2; ++stage) {
            BeerD3D11Resource *buffer = buffers[stage];
            if (!buffer || !buffer->pixels) continue;
            size_t shown = buffer->pixel_size < 96 ? buffer->pixel_size : 96;
            fprintf(stderr, "[D3D11 DRAW CONSTANT] stage=%s slot=%u bytes=%zu",
                    stages[stage], i, buffer->pixel_size);
            for (size_t j = 0; j < shown; ++j)
                fprintf(stderr, "%s%02x", j % 16 ? " " : "\n  ", buffer->pixels[j]);
            fputc('\n', stderr);
        }
    }
    for (uint32_t i = 0; i < BEER_MAX_SHADER_RESOURCES; ++i) {
        BeerD3D11View *view = (BeerD3D11View *)state->ps_shader_resources[i];
        if (!view || !com_get_header(view) || view->kind != BEER_VIEW_SHADER_RESOURCE)
            continue;
        BeerD3D11Resource *resource = validated_resource(view->resource);
        if (!resource) continue;
        const uint32_t *desc = (const uint32_t *)resource->desc;
        uint32_t hash = 2166136261u;
        for (size_t j = 0; j < resource->pixel_size; ++j)
            hash = (hash ^ resource->pixels[j]) * 16777619u;
        fprintf(stderr, "[D3D11 DRAW SRV] slot=%u view=%p resource=%p dimension=%u width=%u height=%u format=%u bytes=%zu fnv1a=%08x\n",
                i, (void *)view, (void *)resource, resource->dimension,
                desc[0], desc[1], desc[4], resource->pixel_size, hash);
        char resource_kind[32];
        snprintf(resource_kind, sizeof(resource_kind), "SRV%u-F%u", i, desc[4]);
        capture_draw_blob(resource_kind, hash, resource->pixels,
                          resource->pixel_size);
    }
    BeerD3D11View *target_view = (BeerD3D11View *)state->render_targets[0];
    BeerD3D11Resource *target = target_view && com_get_header(target_view)
        ? validated_resource(target_view->resource) : NULL;
    if (target) {
        const uint32_t *desc = (const uint32_t *)target->desc;
        fprintf(stderr, "[D3D11 DRAW TARGET] width=%u height=%u format=%u row-pitch=%zu bytes=%zu\n",
                desc[0], desc[1], desc[4], target->row_pitch, target->pixel_size);
    }
}

static uint32_t fnv1a_bytes(const uint8_t *data, size_t size)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * 16777619u;
    return hash;
}

static int render_diagnostics_enabled(void)
{
    static int initialized;
    static int enabled;
    if (!initialized) {
        const char *value = getenv("BEER_RENDER_DIAGNOSTICS");
        enabled = value && *value && strcmp(value, "0") != 0;
        initialized = 1;
    }
    return enabled;
}

static uint32_t resource_diagnostic_hash(BeerD3D11Resource *resource)
{
    if (!render_diagnostics_enabled() || !resource || !resource->pixels) return 0;
    uint64_t serial = atomic_load(&resource->write_serial);
    if (resource->diagnostic_hash_serial != serial) {
        resource->diagnostic_hash = fnv1a_bytes(resource->pixels,
                                                resource->pixel_size);
        resource->diagnostic_hash_serial = serial;
    }
    return resource->diagnostic_hash;
}

static void unpack_rgba(const BeerD3D11Resource *resource, size_t pixel,
                        uint32_t rgba[4])
{
    const uint32_t format = ((const uint32_t *)resource->desc)[4];
    const uint8_t *source = resource->pixels + pixel * 4;
    if (format == 24) { /* DXGI_FORMAT_R10G10B10A2_UNORM */
        uint32_t packed;
        memcpy(&packed, source, sizeof(packed));
        rgba[0] = packed & 0x3ff;
        rgba[1] = (packed >> 10) & 0x3ff;
        rgba[2] = (packed >> 20) & 0x3ff;
        rgba[3] = ((packed >> 30) & 3) * 341;
    } else {
        rgba[0] = (uint32_t)source[0] * 4;
        rgba[1] = (uint32_t)source[1] * 4;
        rgba[2] = (uint32_t)source[2] * 4;
        rgba[3] = (uint32_t)source[3] * 4;
    }
}

static void pack_rgba(BeerD3D11Resource *resource, size_t pixel,
                      const uint32_t rgba[4])
{
    const uint32_t format = ((const uint32_t *)resource->desc)[4];
    uint8_t *destination = resource->pixels + pixel * 4;
    if (format == 24) {
        uint32_t packed = (rgba[0] > 1023 ? 1023 : rgba[0]) |
            ((rgba[1] > 1023 ? 1023 : rgba[1]) << 10) |
            ((rgba[2] > 1023 ? 1023 : rgba[2]) << 20) |
            (((rgba[3] >= 852 ? 3 : (rgba[3] + 170) / 341) & 3) << 30);
        memcpy(destination, &packed, sizeof(packed));
    } else {
        destination[0] = (uint8_t)((rgba[0] > 1023 ? 1023 : rgba[0]) * 255 / 1023);
        destination[1] = (uint8_t)((rgba[1] > 1023 ? 1023 : rgba[1]) * 255 / 1023);
        destination[2] = (uint8_t)((rgba[2] > 1023 ? 1023 : rgba[2]) * 255 / 1023);
        destination[3] = (uint8_t)((rgba[3] > 1023 ? 1023 : rgba[3]) * 255 / 1023);
    }
}

/* Forward declarations for helpers defined later in this file. */
static uint32_t d3d11_to_vk_address_mode(uint32_t d3d);
static int d3d11_dxgi_to_vulkan_image(uint32_t dxgi, uint32_t *vk_format,
                                       uint32_t *bytes_per_pixel);
static int ensure_nv12_cache(BeerD3D11Resource *texture);

/* Auto-translated path for non-indexed (Draw) calls.  Mirrors the approach of
 * try_auto_translate_indexed_draw but for topology-4 / topology-5 fullscreen
 * and Bink passes.  Returns 1 on success, 0 on any rejection. */
static int try_auto_translate_fullscreen_draw(const BeerD3D11PipelineState *state,
                                              uint32_t vertex_count,
                                              uint32_t start_vertex)
{
    if (!state || !state->vertex_shader || !state->pixel_shader ||
        !state->render_targets[0])
        return 0;
    /* vertex_buffers[0] may be NULL for procedural fullscreen quads that
     * generate positions from SV_VertexID (e.g. RE8's NV12 movie compositor).
     * The vertex-binding section below handles this gracefully.
     * viewport_count may be 0 when the game relies on D3D11's implicit
     * fullscreen viewport; we fall back to the render-target dimensions. */

    BeerD3D11Shader *vs = state->vertex_shader;
    BeerD3D11Shader *ps = state->pixel_shader;
    if (!com_get_header(vs) || !com_get_header(ps)) return 0;

    uint64_t vs_hash = ((uint64_t)fnv1a_bytes(vs->bytecode, vs->bytecode_size) << 32) |
                        fnv1a_bytes(vs->bytecode + vs->bytecode_size / 2,
                                    vs->bytecode_size - vs->bytecode_size / 2);
    uint64_t ps_hash = ((uint64_t)fnv1a_bytes(ps->bytecode, ps->bytecode_size) << 32) |
                        fnv1a_bytes(ps->bytecode + ps->bytecode_size / 2,
                                    ps->bytecode_size - ps->bytecode_size / 2);

    const BeerD3D12TranslatedShader *tvs =
        beer_d3d12_translate_shader(vs->bytecode, vs->bytecode_size, vs_hash);
    const BeerD3D12TranslatedShader *tps =
        beer_d3d12_translate_shader(ps->bytecode, ps->bytecode_size, ps_hash);
    if (!tvs || !tps) {
        /* Log every unique VS+PS fail pair — deduplicate so the log stays
         * readable even when the same rejected pair fires every frame. */
        {
            static uint64_t failed_pairs[32];
            static uint32_t failed_count;
            static pthread_mutex_t flock = PTHREAD_MUTEX_INITIALIZER;
            uint64_t fkey = vs_hash ^ (ps_hash * 0x9e3779b97f4a7c15ULL);
            pthread_mutex_lock(&flock);
            int fseen = 0;
            for (uint32_t fi = 0; fi < failed_count && fi < 32; ++fi)
                if (failed_pairs[fi] == fkey) { fseen = 1; break; }
            if (!fseen && failed_count < 32) failed_pairs[failed_count++] = fkey;
            pthread_mutex_unlock(&flock);
            if (!fseen) {
                fprintf(stderr,
                        "[D3D11 FS AUTO TRANSLATE FAIL] VS=%08x PS=%08x: %s "
                        "verts=%u vs-bytes=%zu ps-bytes=%zu\n",
                        (uint32_t)vs_hash, (uint32_t)ps_hash,
                        beer_d3d12_shader_translation_error(),
                        vertex_count, vs->bytecode_size, ps->bytecode_size);
                /* Dump bytecode to /tmp so it can be inspected offline. */
                {
                    char path[128];
                    snprintf(path, sizeof(path),
                             "/tmp/beer-fs-fail-vs-%08x.dxbc",
                             (uint32_t)vs_hash);
                    FILE *fv = fopen(path, "wb");
                    if (fv) { fwrite(vs->bytecode, 1, vs->bytecode_size, fv);
                              fclose(fv); }
                    snprintf(path, sizeof(path),
                             "/tmp/beer-fs-fail-ps-%08x.dxbc",
                             (uint32_t)ps_hash);
                    FILE *fp = fopen(path, "wb");
                    if (fp) { fwrite(ps->bytecode, 1, ps->bytecode_size, fp);
                              fclose(fp); }
                }
            }
        }
        return 0;
    }

    /* Log first translation for each unique pair. */
    {
        static uint64_t logged_pairs[32];
        static uint32_t logged_count;
        static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
        uint64_t key = vs_hash ^ (ps_hash * 0x9e3779b97f4a7c15ULL);
        pthread_mutex_lock(&log_lock);
        int already = 0;
        for (uint32_t i = 0; i < logged_count && i < 32; ++i)
            if (logged_pairs[i] == key) { already = 1; break; }
        if (!already && logged_count < 32) logged_pairs[logged_count++] = key;
        pthread_mutex_unlock(&log_lock);
        if (!already)
            fprintf(stderr,
                    "[D3D11 FS AUTO TRANSLATE OK] VS=%08x PS=%08x "
                    "vs-desc=%u ps-desc=%u vs-spirv=%zu ps-spirv=%zu\n",
                    (uint32_t)vs_hash, (uint32_t)ps_hash,
                    tvs->descriptor_count, tps->descriptor_count,
                    tvs->spirv_size, tps->spirv_size);
    }

    /* ---- Descriptor bindings ---- */
    BeerVulkanGraphicsBinding bindings[BEER_VULKAN_GRAPHICS_MAX_BINDINGS];
    memset(bindings, 0, sizeof(bindings));
    uint32_t binding_count = 0;

    for (uint32_t stage = 0; stage < 2; ++stage) {
        const BeerD3D12TranslatedShader *sh = stage ? tps : tvs;
        uint32_t sf = stage ? BEER_VULKAN_GRAPHICS_STAGE_FRAGMENT
                            : BEER_VULKAN_GRAPHICS_STAGE_VERTEX;
        for (uint32_t d = 0; d < sh->descriptor_count; ++d) {
            const BeerD3D12ShaderDescriptor *desc = &sh->descriptors[d];
            if (binding_count >= BEER_VULKAN_GRAPHICS_MAX_BINDINGS) return 0;
            BeerVulkanGraphicsBinding *b = &bindings[binding_count++];
            b->binding     = desc->binding;
            b->stage_flags = sf;
            uint32_t slot  = desc->register_index;
            if (desc->type == BEER_D3D12_SHADER_DESCRIPTOR_CBV) {
                BeerD3D11Resource *cbuf = validated_resource(
                    stage ? state->ps_constant_buffers[slot < 8 ? slot : 0]
                          : state->vs_constant_buffers[slot < 8 ? slot : 0]);
                if (!cbuf || !cbuf->pixels) return 0;
                b->kind = BEER_VULKAN_GRAPHICS_BINDING_UNIFORM_BUFFER;
                b->data = cbuf->pixels;
                b->size = cbuf->pixel_size;
            } else if (desc->type == BEER_D3D12_SHADER_DESCRIPTOR_SRV && stage == 1) {
                BeerD3D11View *view = slot < BEER_MAX_SHADER_RESOURCES
                    ? state->ps_shader_resources[slot] : NULL;
                BeerD3D11Resource *res = view && com_get_header(view)
                    ? validated_resource(view->resource) : NULL;
                if (!res || !res->pixels || res->desc_size < 20) return 0;
                const uint32_t *rdesc = (const uint32_t *)res->desc;
                uint32_t vk_fmt = 0, bpp = 0;
                if (!d3d11_dxgi_to_vulkan_image(rdesc[4], &vk_fmt, &bpp)) return 0;
                b->kind    = BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_IMAGE;
                b->resource = res;
                b->serial   = (const uint64_t *)&res->content_serial;
                b->format   = vk_fmt;
                b->width    = rdesc[0];
                b->height   = rdesc[1];
                /* NV12: the raw NV12 pixels are decoded to RGBA8 in
                 * ensure_nv12_cache(). Bind the decoded plane instead of the
                 * raw YUV plane so the generic Vulkan sampler path works. */
                if (rdesc[4] == 103 /* NV12 */) {
                    if (!ensure_nv12_cache(res)) return 0;
                    b->data = res->decoded_rgba;
                    b->size = res->decoded_rgba_size;
                    b->bytes_per_pixel = 4;
                    b->row_pitch = rdesc[0] * 4u;
                } else if (bpp == 0) {
                    b->data = res->pixels;
                    b->size = res->pixel_size;
                    uint32_t bpb = (vk_fmt == 131 || vk_fmt == 132 ||
                                    vk_fmt == 139 || vk_fmt == 140) ? 8u : 16u;
                    b->block_width     = 4;
                    b->block_height    = 4;
                    b->bytes_per_block = bpb;
                    b->bytes_per_pixel = 0;
                    uint32_t blocks_wide = (rdesc[0] + 3u) / 4u;
                    b->row_pitch = res->row_pitch ? res->row_pitch : blocks_wide * bpb;
                } else {
                    b->data = res->pixels;
                    b->size = res->pixel_size;
                    b->bytes_per_pixel = bpp;
                    b->row_pitch = res->row_pitch ? res->row_pitch : rdesc[0] * bpp;
                }
            } else if (desc->type == BEER_D3D12_SHADER_DESCRIPTOR_SAMPLER && stage == 1) {
                BeerD3D11SamplerState *samp = slot < 8
                    ? state->ps_samplers[slot] : NULL;
                const uint32_t *sdesc = samp && com_get_header(samp)
                    ? (const uint32_t *)samp->desc : NULL;
                b->kind       = BEER_VULKAN_GRAPHICS_BINDING_SAMPLER;
                b->min_filter = sdesc && sdesc[0] == 0x15 ? 1u : 0u;
                b->mag_filter = b->min_filter;
                b->address_u  = sdesc ? d3d11_to_vk_address_mode(sdesc[1]) : 2u;
                b->address_v  = sdesc ? d3d11_to_vk_address_mode(sdesc[2]) : 2u;
                b->address_w  = 2u;
            } else {
                return 0;
            }
        }
    }

    /* ---- Vertex binding (non-indexed, no index buffer) ---- */
    /* RE8's NV12 movie compositor uses a procedural fullscreen quad with no
     * bound VB — positions come from SV_VertexID in the vertex shader.
     * Allow a NULL vertex_buffers[0] and emit zero vertex bindings/attributes
     * so the Vulkan pipeline uses gl_VertexIndex only. */
    BeerD3D11Resource *vbuf = state->vertex_buffers[0]
        ? validated_resource(state->vertex_buffers[0]) : NULL;

    BeerVulkanGraphicsVertexBinding  vb[1];
    BeerVulkanGraphicsVertexAttribute va[BEER_VULKAN_GRAPHICS_MAX_VERTEX_ATTRIBUTES];
    memset(vb, 0, sizeof(vb));
    memset(va, 0, sizeof(va));
    uint32_t vb_count = 0;

    if (vbuf && vbuf->pixels) {
        vb[0].data   = vbuf->pixels + state->vertex_offsets[0];
        vb[0].size   = vbuf->pixel_size > state->vertex_offsets[0]
                       ? vbuf->pixel_size - state->vertex_offsets[0] : 0;
        vb[0].stride = state->vertex_strides[0];
        if (vb[0].size && vb[0].stride)
            vb_count = 1;
    }

    BeerD3D11InputLayout *layout = state->input_layout;
    uint32_t va_count = 0;
    if (vb_count && layout && com_get_header(layout)) {
        uint32_t slot_offsets[1] = {0};
        for (uint32_t e = 0; e < layout->element_count &&
             va_count < BEER_VULKAN_GRAPHICS_MAX_VERTEX_ATTRIBUTES; ++e) {
            const BeerD3D11InputElement *elem = &layout->elements[e];
            if (elem->input_slot != 0) return 0; /* only slot 0 for fullscreen */
            uint32_t vk_fmt = 0, bpp = 0;
            if (!d3d11_dxgi_to_vulkan_image(elem->format, &vk_fmt, &bpp)) return 0;
            uint32_t byte_offset = elem->aligned_byte_offset;
            if (byte_offset == 0xffffffffu)
                byte_offset = slot_offsets[0];
            else
                slot_offsets[0] = byte_offset;
            slot_offsets[0] = byte_offset + bpp;
            va[va_count].location = va_count;
            va[va_count].binding  = 0;
            va[va_count].format   = vk_fmt;
            va[va_count].offset   = byte_offset;
            ++va_count;
        }
    }
    /* Zero vertex bindings and zero attributes = procedural SV_VertexID quad.
     * Vulkan allows this; the vertex shader reads gl_VertexIndex directly. */

    /* ---- Render target ---- */
    BeerD3D11View *rtv = state->render_targets[0];
    BeerD3D11Resource *rt = rtv && com_get_header(rtv)
        ? validated_resource(rtv->resource) : NULL;
    if (!rt || !rt->pixels || rt->desc_size < 20) return 0;
    const uint32_t *rdesc = (const uint32_t *)rt->desc;
    uint32_t rt_vk_fmt = 0, rt_bpp = 0;
    if (!d3d11_dxgi_to_vulkan_image(rdesc[4], &rt_vk_fmt, &rt_bpp)) return 0;

    BeerVulkanGraphicsTarget target_out;
    memset(&target_out, 0, sizeof(target_out));
    target_out.data            = rt->pixels;
    target_out.size            = rt->pixel_size;
    target_out.serial          = (uint64_t *)&rt->content_serial;
    target_out.format          = rt_vk_fmt;
    target_out.bytes_per_pixel = rt_bpp;
    target_out.width           = rdesc[0];
    target_out.height          = rdesc[1];
    target_out.row_pitch       = rdesc[0] * rt_bpp;

    /* ---- Fixed-function state ---- */
    BeerVulkanGraphicsState gs;
    memset(&gs, 0, sizeof(gs));
    gs.flip_y = 0;
    BeerD3D11State *blend = (BeerD3D11State *)state->blend_state;
    if (blend && com_get_header(blend) && blend->kind == BEER_STATE_BLEND &&
        blend->desc_size >= 40) {
        const uint8_t *rt0 = blend->desc + 8;
        uint32_t be; memcpy(&be, rt0, 4);
        if (be) {
            static const int8_t d3d_to_vk[] = {
                -1,0,1,2,3,6,7,8,9,4,5,14,-1,-1,10,11
            };
            uint32_t sb,db,ba,sba,dba,bba;
            memcpy(&sb,  rt0+4,  4); memcpy(&db,  rt0+8,  4);
            memcpy(&ba,  rt0+12, 4); memcpy(&sba, rt0+16, 4);
            memcpy(&dba, rt0+20, 4); memcpy(&bba, rt0+24, 4);
            if (sb<16 && db<16 && sba<16 && dba<16 &&
                d3d_to_vk[sb]>=0 && d3d_to_vk[db]>=0 &&
                d3d_to_vk[sba]>=0 && d3d_to_vk[dba]>=0) {
                gs.blend_enable     = 1;
                gs.src_color_factor = (uint32_t)d3d_to_vk[sb];
                gs.dst_color_factor = (uint32_t)d3d_to_vk[db];
                gs.color_op         = (ba>=1&&ba<=5) ? ba-1u : 0u;
                gs.src_alpha_factor = (uint32_t)d3d_to_vk[sba];
                gs.dst_alpha_factor = (uint32_t)d3d_to_vk[dba];
                gs.alpha_op         = (bba>=1&&bba<=5) ? bba-1u : 0u;
            } else return 0;
        }
        gs.write_mask = rt0[28];
    } else {
        gs.write_mask = 0x0f;
    }
    memcpy(gs.blend_constants, state->blend_factor, sizeof(gs.blend_constants));
    gs.cull_mode = 0;

    /* ---- Draw info ---- */
    BeerVulkanGraphicsDrawInfo di;
    memset(&di, 0, sizeof(di));
    if (vb_count) di.vertex_bindings[0] = vb[0];
    di.vertex_binding_count   = vb_count;
    for (uint32_t i = 0; i < va_count; ++i) di.vertex_attributes[i] = va[i];
    di.vertex_attribute_count = va_count;
    di.indexed                = 0;
    /* D3D11 topology 4 = TRIANGLELIST, 5 = TRIANGLESTRIP */
    di.topology    = (state->topology == 5) ? 1u  /* TRIANGLE_STRIP */
                                             : 0u; /* TRIANGLE_LIST  */
    di.element_count  = vertex_count;
    di.instance_count = 1;
    di.base_vertex    = (int32_t)start_vertex;

    float viewport[6] = {0};
    if (state->viewport_count > 0) {
        memcpy(viewport, state->viewports[0], sizeof(float) * 4);
    } else {
        /* No viewport set — use the render-target dimensions as an implicit
         * fullscreen viewport (D3D11 default behaviour). */
        viewport[0] = 0.0f;
        viewport[1] = 0.0f;
        viewport[2] = (float)target_out.width;
        viewport[3] = (float)target_out.height;
    }
    viewport[4] = 0.0f; viewport[5] = 1.0f;

    int32_t scissor[4] = {
        0, 0, (int32_t)target_out.width, (int32_t)target_out.height
    };
    BeerD3D11State *rast = (BeerD3D11State *)state->rasterizer_state;
    if (state->scissor_count && rast && com_get_header(rast) &&
        rast->kind == BEER_STATE_RASTERIZER && rast->desc_size >= 32 &&
        *(const int32_t *)(rast->desc + 28)) {
        scissor[0] = state->scissor_rects[0][0];
        scissor[1] = state->scissor_rects[0][1];
        scissor[2] = state->scissor_rects[0][2];
        scissor[3] = state->scissor_rects[0][3];
    }

    const char *reason = NULL;
    int ok = beer_vulkan_graphics_draw_state(
        vs, (const uint32_t *)tvs->spirv, tvs->spirv_size,
        ps, (const uint32_t *)tps->spirv, tps->spirv_size,
        bindings, binding_count, &di, &gs,
        &target_out, 1, NULL, viewport, scissor, &reason);

    if (ok) {
        resource_mark_written(rt);
        static _Atomic(uint32_t) auto_count;
        uint32_t n = atomic_fetch_add(&auto_count, 1) + 1;
        if (n <= 16 || (n % 64) == 0)
            fprintf(stderr,
                    "[D3D11 FS AUTO] draw #%u VS=%08x PS=%08x "
                    "verts=%u target=%ux%u bindings=%u\n",
                    n, (uint32_t)vs_hash, (uint32_t)ps_hash,
                    vertex_count, target_out.width, target_out.height,
                    binding_count);
    } else {
        static _Atomic(uint32_t) reject_count;
        uint32_t rn = atomic_fetch_add(&reject_count, 1);
        if (rn < 16)
            fprintf(stderr,
                    "[D3D11 FS AUTO REJECT] VS=%08x PS=%08x "
                    "verts=%u bindings=%u target=%ux%u reason=%s\n",
                    (uint32_t)vs_hash, (uint32_t)ps_hash,
                    vertex_count, binding_count,
                    target_out.width, target_out.height,
                    reason ? reason : "unknown");
    }
    return ok;
}

/* Execute verified fullscreen shaders used by Sekiro's initial presentation
 * path (Bink RGBA, Bink filtered composite, uniform-color overlay, and the
 * fullscreen copy pair). Auto-translation runs first; manual paths below are
 * retained as fallback for formats not yet supported by the generic path. */
static int execute_initial_fullscreen_draw(const BeerD3D11PipelineState *state,
                                           uint32_t vertex_count,
                                           uint32_t start_vertex)
{
    if (!state || !state->vertex_shader || !state->pixel_shader ||
        !state->render_targets[0])
        return 0;
    /* vertex_buffers[0] may be NULL for procedural quads (NV12 movie draw). */

    /* Auto-translate first — catches all newly observed pairs without needing
     * a manual hash entry.  Falls through to the manual paths on any rejection
     * (e.g. unsupported format, missing resource, VS SRV).
     *
     * Exception: the legacy compositor pair (d2d4e0b9 / bf96cb08 and
     * d2d4e0b9 / da42b236) must always use the manual path below because that
     * path calls vulkan_renderer_composite_rgba8() which writes directly to the
     * swap-chain back buffer via the Vulkan compositor.  The auto path would
     * write to whatever intermediate RT is currently bound and the back buffer
     * would stay black. */
    {
        BeerD3D11Shader *_vs = state->vertex_shader;
        BeerD3D11Shader *_ps = state->pixel_shader;
        uint32_t _vh = (_vs && com_get_header(_vs))
            ? fnv1a_bytes(_vs->bytecode, _vs->bytecode_size) : 0;
        uint32_t _ph = (_ps && com_get_header(_ps))
            ? fnv1a_bytes(_ps->bytecode, _ps->bytecode_size) : 0;
        int _is_compositor = (_vh == 0xd2d4e0b9) &&
            (_ph == 0xbf96cb08 || _ph == 0xda42b236);
        if (!_is_compositor &&
            try_auto_translate_fullscreen_draw(state, vertex_count, start_vertex))
            return 1;
    }

    BeerD3D11Shader *vertex_shader = state->vertex_shader;
    BeerD3D11Shader *pixel_shader = state->pixel_shader;
    if (!com_get_header(vertex_shader) || !com_get_header(pixel_shader)) return 0;
    uint32_t vertex_hash = fnv1a_bytes(vertex_shader->bytecode,
                                       vertex_shader->bytecode_size);
    uint32_t pixel_hash = fnv1a_bytes(pixel_shader->bytecode,
                                      pixel_shader->bytecode_size);
    BeerD3D11Resource *vertices = state->vertex_buffers[0]
        ? validated_resource(state->vertex_buffers[0]) : NULL;
    if (vertex_hash == 0x759406e5 && pixel_hash == 0x39b87d72 &&
        vertex_count == 6 && start_vertex == 0 && state->topology == 4 &&
        state->vertex_strides[0] == 12 && state->viewport_count > 0) {
        BeerD3D11Resource *constants = validated_resource(state->vs_constant_buffers[0]);
        BeerD3D11View *source_view = state->ps_shader_resources[0];
        BeerD3D11Resource *source = source_view && com_get_header(source_view)
            ? validated_resource(source_view->resource) : NULL;
        BeerD3D11View *target_view = state->render_targets[0];
        BeerD3D11Resource *target = target_view && com_get_header(target_view)
            ? validated_resource(target_view->resource) : NULL;
        if (!vertices || !constants || !source || !target ||
            !vertices->pixels || !constants->pixels || !source->pixels ||
            !target->pixels || constants->pixel_size < 96 ||
            source->desc_size < 20 || target->desc_size < 20 ||
            ((const uint32_t *)source->desc)[4] != 28 ||
            ((const uint32_t *)target->desc)[4] != 28)
            return 0;

        const uint32_t *source_desc = (const uint32_t *)source->desc;
        const uint32_t *target_desc = (const uint32_t *)target->desc;
        size_t vertex_offset = state->vertex_offsets[0];
        if (vertex_offset > vertices->pixel_size ||
            (size_t)vertex_count * 12u > vertices->pixel_size - vertex_offset)
            return 0;

        static const uint16_t sequential_indices[6] = { 0, 1, 2, 3, 4, 5 };
        BeerVulkanIndexedDraw draw;
        memset(&draw, 0, sizeof(draw));
        draw.vertices = vertices->pixels;
        draw.vertex_bytes = vertices->pixel_size;
        draw.indices = (const uint8_t *)sequential_indices;
        draw.index_bytes = sizeof(sequential_indices);
        draw.constants = constants->pixels;
        draw.constant_bytes = constants->pixel_size;
        draw.target = target->pixels;
        draw.target_bytes = target->pixel_size;
        draw.target_resource = target;
        draw.target_input_serial = atomic_load(&target->content_serial);
        draw.width = target_desc[0];
        draw.height = target_desc[1];
        draw.vertex_offset = (uint32_t)vertex_offset;
        draw.vertex_stride = 12;
        draw.index_count = 6;
        draw.mode = 15;
        draw.texture = source->pixels;
        draw.texture_resource = source;
        draw.texture_bytes = source->pixel_size;
        draw.texture_serial = atomic_load(&source->content_serial);
        draw.texture_width = source_desc[0];
        draw.texture_height = source_desc[1];
        memcpy(draw.viewport, state->viewports[0], sizeof(draw.viewport));
        draw.scissor[0] = 0;
        draw.scissor[1] = 0;
        draw.scissor[2] = (int32_t)draw.width;
        draw.scissor[3] = (int32_t)draw.height;
        draw.write_mask = 0x0f;
        memcpy(draw.blend_factor, state->blend_factor, sizeof(draw.blend_factor));
        BeerD3D11SamplerState *sampler = state->ps_samplers[0];
        const uint32_t *sampler_desc = sampler && com_get_header(sampler)
            ? (const uint32_t *)sampler->desc : NULL;
        draw.texture_linear = sampler_desc && sampler_desc[0] == 0x15;
        draw.texture_address_u = sampler_desc ? sampler_desc[1] : 3;
        draw.texture_address_v = sampler_desc ? sampler_desc[2] : 3;
        BeerD3D11State *blend = (BeerD3D11State *)state->blend_state;
        if (blend && com_get_header(blend) && blend->kind == BEER_STATE_BLEND &&
            blend->desc_size >= 40) {
            const uint8_t *render_target = blend->desc + 8;
            memcpy(&draw.blend_enable, render_target, 4);
            memcpy(&draw.source_blend, render_target + 4, 4);
            memcpy(&draw.destination_blend, render_target + 8, 4);
            memcpy(&draw.color_operation, render_target + 12, 4);
            memcpy(&draw.source_alpha, render_target + 16, 4);
            memcpy(&draw.destination_alpha, render_target + 20, 4);
            memcpy(&draw.alpha_operation, render_target + 24, 4);
            draw.write_mask = render_target[28];
        }
        pthread_mutex_lock(&g_gpu_target_order_lock);
        resource_begin_gpu_write(target, &draw.target_input_serial,
                                 &draw.target_output_serial);
        int bink_rgba_executed = vulkan_indexed_renderer_draw(&draw);
        pthread_mutex_unlock(&g_gpu_target_order_lock);
        if (!bink_rgba_executed)
            strict_vulkan_failure("Bink RGBA conversion draw");
        trace_vulkan_migration_draw("Bink RGBA", vertex_hash, pixel_hash);
        return 1;
    }
    if (vertex_hash == 0x759406e5 && pixel_hash == 0xd804183a &&
        vertex_count == 6 && start_vertex == 0 && state->topology == 4 &&
        state->vertex_strides[0] == 12 && state->viewport_count > 0) {
        BeerD3D11Resource *constants = validated_resource(state->vs_constant_buffers[0]);
        BeerD3D11Resource *pixel_constants = validated_resource(state->ps_constant_buffers[0]);
        BeerD3D11View *source0_view = state->ps_shader_resources[0];
        BeerD3D11View *source1_view = state->ps_shader_resources[1];
        BeerD3D11Resource *source0 = source0_view && com_get_header(source0_view)
            ? validated_resource(source0_view->resource) : NULL;
        BeerD3D11Resource *source1 = source1_view && com_get_header(source1_view)
            ? validated_resource(source1_view->resource) : NULL;
        BeerD3D11View *target_view = state->render_targets[0];
        BeerD3D11Resource *target = target_view && com_get_header(target_view)
            ? validated_resource(target_view->resource) : NULL;
        if (!vertices || !constants || !pixel_constants || !source0 || !source1 ||
            !target || !vertices->pixels || !constants->pixels ||
            !pixel_constants->pixels || !source0->pixels || !source1->pixels ||
            !target->pixels || constants->pixel_size < 96 ||
            pixel_constants->pixel_size < 16 || source0->desc_size < 20 ||
            source1->desc_size < 20 || target->desc_size < 20 ||
            ((const uint32_t *)source0->desc)[4] != 28 ||
            ((const uint32_t *)source1->desc)[4] != 28 ||
            ((const uint32_t *)target->desc)[4] != 28)
            return 0;
        const uint32_t *source0_desc = (const uint32_t *)source0->desc;
        const uint32_t *source1_desc = (const uint32_t *)source1->desc;
        const uint32_t *target_desc = (const uint32_t *)target->desc;
        if (source0_desc[0] != source1_desc[0] ||
            source0_desc[1] != source1_desc[1])
            return 0;
        size_t vertex_offset = state->vertex_offsets[0];
        if (vertex_offset > vertices->pixel_size ||
            (size_t)vertex_count * 12u > vertices->pixel_size - vertex_offset)
            return 0;
        static const uint16_t sequential_indices[6] = { 0, 1, 2, 3, 4, 5 };
        BeerVulkanIndexedDraw draw;
        memset(&draw, 0, sizeof(draw));
        draw.vertices = vertices->pixels;
        draw.vertex_bytes = vertices->pixel_size;
        draw.indices = (const uint8_t *)sequential_indices;
        draw.index_bytes = sizeof(sequential_indices);
        draw.constants = constants->pixels;
        draw.constant_bytes = constants->pixel_size;
        draw.target = target->pixels;
        draw.target_bytes = target->pixel_size;
        draw.target_resource = target;
        draw.target_input_serial = atomic_load(&target->content_serial);
        draw.width = target_desc[0];
        draw.height = target_desc[1];
        draw.vertex_offset = (uint32_t)vertex_offset;
        draw.vertex_stride = 12;
        draw.index_count = 6;
        draw.mode = 16;
        draw.texture = source0->pixels;
        draw.texture_resource = source0;
        draw.texture_bytes = source0->pixel_size;
        draw.texture_serial = atomic_load(&source0->content_serial);
        draw.texture_width = source0_desc[0];
        draw.texture_height = source0_desc[1];
        draw.texture2 = source1->pixels;
        draw.texture2_resource = source1;
        draw.texture2_bytes = source1->pixel_size;
        draw.texture2_serial = atomic_load(&source1->content_serial);
        draw.texture2_width = source1_desc[0];
        draw.texture2_height = source1_desc[1];
        memcpy(draw.viewport, state->viewports[0], sizeof(draw.viewport));
        draw.scissor[0] = 0;
        draw.scissor[1] = 0;
        draw.scissor[2] = (int32_t)draw.width;
        draw.scissor[3] = (int32_t)draw.height;
        draw.write_mask = 0x0f;
        memcpy(draw.blend_factor, state->blend_factor, sizeof(draw.blend_factor));
        memcpy(draw.constant_color, pixel_constants->pixels, sizeof(draw.constant_color));
        BeerD3D11SamplerState *sampler = state->ps_samplers[0];
        const uint32_t *sampler_desc = sampler && com_get_header(sampler)
            ? (const uint32_t *)sampler->desc : NULL;
        draw.texture_linear = sampler_desc && sampler_desc[0] == 0x15;
        draw.texture_address_u = sampler_desc ? sampler_desc[1] : 3;
        draw.texture_address_v = sampler_desc ? sampler_desc[2] : 3;
        BeerD3D11State *blend = (BeerD3D11State *)state->blend_state;
        if (blend && com_get_header(blend) && blend->kind == BEER_STATE_BLEND &&
            blend->desc_size >= 40) {
            const uint8_t *render_target = blend->desc + 8;
            memcpy(&draw.blend_enable, render_target, 4);
            memcpy(&draw.source_blend, render_target + 4, 4);
            memcpy(&draw.destination_blend, render_target + 8, 4);
            memcpy(&draw.color_operation, render_target + 12, 4);
            memcpy(&draw.source_alpha, render_target + 16, 4);
            memcpy(&draw.destination_alpha, render_target + 20, 4);
            memcpy(&draw.alpha_operation, render_target + 24, 4);
            draw.write_mask = render_target[28];
        }
        pthread_mutex_lock(&g_gpu_target_order_lock);
        resource_begin_gpu_write(target, &draw.target_input_serial,
                                 &draw.target_output_serial);
        int bink_filtered_executed = vulkan_indexed_renderer_draw(&draw);
        pthread_mutex_unlock(&g_gpu_target_order_lock);
        if (!bink_filtered_executed)
            strict_vulkan_failure("Bink filtered composition draw");
        trace_vulkan_migration_draw("Bink filtered", vertex_hash, pixel_hash);
        return 1;
    }
    if (vertex_hash == 0xa7b2c1e7 && pixel_hash == 0x7bf3bccb &&
        vertex_count == 6 && start_vertex == 0 && state->topology == 4 &&
        state->vertex_strides[0] == 12 && state->viewport_count > 0) {
        BeerD3D11Resource *constants = validated_resource(state->vs_constant_buffers[0]);
        BeerD3D11Resource *pixel_constants = validated_resource(
            state->ps_constant_buffers[0]);
        BeerD3D11View *target_view = state->render_targets[0];
        BeerD3D11Resource *target = target_view && com_get_header(target_view)
            ? validated_resource(target_view->resource) : NULL;
        if (!vertices || !constants || !pixel_constants || !target ||
            !vertices->pixels || !constants->pixels || !pixel_constants->pixels ||
            constants->pixel_size < 64 || pixel_constants->pixel_size < 16 ||
            ((const uint32_t *)target->desc)[4] != 28) {
            fprintf(stderr,
                    "[D3D11 VULKAN REJECT] uniform-color resources VB=%p/%p "
                    "VSCB=%p/%p/%zu PSCB=%p/%p/%zu RT=%p/%p/F%u\n",
                    (void *)vertices, vertices ? (void *)vertices->pixels : NULL,
                    (void *)constants, constants ? (void *)constants->pixels : NULL,
                    constants ? constants->pixel_size : 0,
                    (void *)pixel_constants,
                    pixel_constants ? (void *)pixel_constants->pixels : NULL,
                    pixel_constants ? pixel_constants->pixel_size : 0,
                    (void *)target, target ? (void *)target->pixels : NULL,
                    target ? ((const uint32_t *)target->desc)[4] : 0);
            return 0;
        }
        const uint32_t width = ((const uint32_t *)target->desc)[0];
        const uint32_t height = ((const uint32_t *)target->desc)[1];
        size_t vertex_offset = state->vertex_offsets[0];
        if (vertex_offset > vertices->pixel_size ||
            (size_t)vertex_count * 12 > vertices->pixel_size - vertex_offset)
            return 0;

        static const uint16_t quad_indices[6] = { 0, 1, 2, 2, 1, 3 };
        BeerVulkanIndexedDraw draw;
        memset(&draw, 0, sizeof(draw));
        draw.vertices = vertices->pixels;
        draw.vertex_bytes = vertices->pixel_size;
        draw.indices = (const uint8_t *)quad_indices;
        draw.index_bytes = sizeof(quad_indices);
        draw.constants = constants->pixels;
        draw.constant_bytes = constants->pixel_size;
        draw.target = target->pixels;
        draw.target_bytes = target->pixel_size;
        draw.target_resource = target;
        draw.target_input_serial = atomic_load(&target->content_serial);
        draw.width = width;
        draw.height = height;
        draw.vertex_offset = (uint32_t)vertex_offset;
        draw.vertex_stride = 12;
        draw.index_offset = 0;
        draw.index_count = 6;
        draw.base_vertex = 0;
        draw.mode = 11;
        memcpy(draw.viewport, state->viewports[0], sizeof(draw.viewport));
        draw.scissor[0] = 0;
        draw.scissor[1] = 0;
        draw.scissor[2] = (int32_t)width;
        draw.scissor[3] = (int32_t)height;
        draw.write_mask = 0x0f;
        memcpy(draw.constant_color, pixel_constants->pixels,
               sizeof(draw.constant_color));
        memcpy(draw.blend_factor, state->blend_factor,
               sizeof(draw.blend_factor));
        BeerD3D11State *blend = (BeerD3D11State *)state->blend_state;
        if (blend && com_get_header(blend) && blend->kind == BEER_STATE_BLEND &&
            blend->desc_size >= 40) {
            const uint8_t *render_target = blend->desc + 8;
            memcpy(&draw.blend_enable, render_target, 4);
            memcpy(&draw.source_blend, render_target + 4, 4);
            memcpy(&draw.destination_blend, render_target + 8, 4);
            memcpy(&draw.color_operation, render_target + 12, 4);
            memcpy(&draw.source_alpha, render_target + 16, 4);
            memcpy(&draw.destination_alpha, render_target + 20, 4);
            memcpy(&draw.alpha_operation, render_target + 24, 4);
            draw.write_mask = render_target[28];
        }
        pthread_mutex_lock(&g_gpu_target_order_lock);
        resource_begin_gpu_write(target, &draw.target_input_serial,
                                 &draw.target_output_serial);
        int uniform_color_executed = vulkan_indexed_renderer_draw(&draw);
        pthread_mutex_unlock(&g_gpu_target_order_lock);
        if (!uniform_color_executed) {
            fprintf(stderr,
                    "[D3D11 VULKAN REJECT] uniform-color dispatch VB-bytes=%zu "
                    "CB-bytes=%zu target-bytes=%zu size=%ux%u mode=%u stride=%u "
                    "color=(%08x,%08x,%08x,%08x)\n",
                    draw.vertex_bytes, draw.constant_bytes, draw.target_bytes,
                    draw.width, draw.height, draw.mode, draw.vertex_stride,
                    ((const uint32_t *)draw.constant_color)[0],
                    ((const uint32_t *)draw.constant_color)[1],
                    ((const uint32_t *)draw.constant_color)[2],
                    ((const uint32_t *)draw.constant_color)[3]);
            strict_vulkan_failure("non-indexed uniform-color draw");
        }
        return 1;
    }

    if (vertex_count != 4 || state->topology != 5 ||
        vertex_hash != 0xd2d4e0b9 ||
        (pixel_hash != 0xbf96cb08 && pixel_hash != 0xda42b236))
        return 0;
    trace_vulkan_migration_draw("fullscreen", vertex_hash, pixel_hash);

    BeerD3D11View *target_view = state->render_targets[0];
    BeerD3D11Resource *target = target_view && com_get_header(target_view)
        ? validated_resource(target_view->resource) : NULL;
    BeerD3D11View *source_view = state->ps_shader_resources[0];
    BeerD3D11Resource *source = source_view && com_get_header(source_view)
        ? validated_resource(source_view->resource) : NULL;
    if (!vertices || !target || !source || !vertices->pixels ||
        !target->pixels || !source->pixels || state->vertex_strides[0] != 48)
        return 0;

    const uint32_t *target_desc = (const uint32_t *)target->desc;
    const uint32_t *source_desc = (const uint32_t *)source->desc;
    uint32_t width = target_desc[0], height = target_desc[1];
    if (!width || !height || source_desc[0] != width || source_desc[1] != height ||
        !((target_desc[4] == 24 && pixel_hash == 0xbf96cb08) ||
          (target_desc[4] == 28 && pixel_hash == 0xda42b236)))
        return 0;

    size_t vertex_offset = (size_t)state->vertex_offsets[0] +
        (size_t)start_vertex * state->vertex_strides[0];
    static const uint32_t expected_positions[8] = {
        0xbf800000, 0x3f800000, 0x3f800000, 0x3f800000,
        0xbf800000, 0xbf800000, 0x3f800000, 0xbf800000
    };
    if (vertex_offset > vertices->pixel_size ||
        4 * 48 > vertices->pixel_size - vertex_offset)
        return 0;
    for (uint32_t i = 0; i < 4; ++i) {
        uint32_t position[2];
        memcpy(position, vertices->pixels + vertex_offset + i * 48,
               sizeof(position));
        if (position[0] != expected_positions[i * 2] ||
            position[1] != expected_positions[i * 2 + 1])
            return 0;
    }

    BeerD3D11Resource *second_source = NULL;
    if (pixel_hash == 0xbf96cb08) {
        BeerD3D11View *second_view = state->ps_shader_resources[1];
        second_source = second_view && com_get_header(second_view)
            ? validated_resource(second_view->resource) : NULL;
        if (g_last_textured_indexed_target) {
            static _Atomic(uint32_t) compositor_traces;
            uint32_t trace = atomic_fetch_add(&compositor_traces, 1) + 1;
            if (trace <= 16) {
                BeerD3D11Resource *sources[3] = { NULL, NULL, NULL };
                for (uint32_t slot = 0; slot < 3; ++slot) {
                    BeerD3D11View *view = state->ps_shader_resources[slot];
                    sources[slot] = view && com_get_header(view)
                        ? validated_resource(view->resource) : NULL;
                }
                fprintf(stderr,
                        "[D3D11 COMPOSITOR] #%u target=%p sources=(%p,%p,%p) "
                        "scaleform=%p serials=(%llu,%llu,%llu/%llu)\n",
                        trace, (void *)target, (void *)sources[0],
                        (void *)sources[1], (void *)sources[2],
                        (void *)g_last_textured_indexed_target,
                        (unsigned long long)(sources[0]
                            ? atomic_load(&sources[0]->write_serial) : 0),
                        (unsigned long long)(sources[1]
                            ? atomic_load(&sources[1]->write_serial) : 0),
                        (unsigned long long)(sources[2]
                            ? atomic_load(&sources[2]->write_serial) : 0),
                        (unsigned long long)atomic_load(
                            &g_last_textured_indexed_target->write_serial));
            }
        }
        BeerD3D11Resource *constants = validated_resource(state->ps_constant_buffers[0]);
        uint32_t exponent_bits = 0;
        if (constants && constants->pixels && constants->pixel_size >= 4)
            memcpy(&exponent_bits, constants->pixels, sizeof(exponent_bits));
        /* The observed shader computes pow(source.rgb * source1.a, exponent).
         * Its title-path exponent is exactly 1, allowing exact fixed-point
         * execution without introducing a host floating-point ABI dependency. */
        if (!second_source || !second_source->pixels || exponent_bits != 0x3f800000)
            return 0;
    } else {
        BeerD3D11Resource *condition = validated_resource(state->ps_constant_buffers[1]);
        /* cb1.x == 0 (or absent) selects the pass-through/original-texture
         * path, which is what splash draws use.  A non-zero value activates
         * the gamma branch that is not yet supported; reject only that case.
         * The previous guard defaulted to UINT32_MAX when cb1 was absent and
         * therefore rejected all splash draws, making the text invisible. */
        if (condition && condition->pixels && condition->pixel_size >= 4) {
            uint32_t condition_bits = 0;
            memcpy(&condition_bits, condition->pixels, sizeof(condition_bits));
            if (condition_bits != 0) return 0;
        }
    }

    /* The Vulkan compositor consumes pending indexed/compositor mirrors
     * directly. Keep the guest CPU copies stale until a real CPU consumer
     * (Map/copy/capture) explicitly requests synchronization. */
    uint32_t mode = second_source ? 1u : 2u;
    /* The compositor consumes its inputs by (resource, content serial). Read
     * those serials, publish the target serial and submit as one ordered step.
     * Without this, a UI draw running on another job can publish a newer
     * serial for an input between the read and the mirror lookup, so the
     * compositor misses the GPU mirror and silently falls back to the stale
     * CPU copy of that layer. */
    pthread_mutex_lock(&g_gpu_target_order_lock);
    uint64_t source_serial = atomic_load(&source->content_serial);
    uint64_t second_source_serial = second_source
        ? atomic_load(&second_source->content_serial) : 0;
    uint64_t target_serial = 0;
    resource_begin_gpu_write(target, NULL, &target_serial);
    int vulkan_executed = vulkan_renderer_composite_rgba8(
        source, source_serial, source->pixels,
        second_source, second_source_serial,
        second_source ? second_source->pixels : NULL,
        target, target_serial, target->pixels, width, height, mode);
    pthread_mutex_unlock(&g_gpu_target_order_lock);
    if (!vulkan_executed)
        strict_vulkan_failure("fullscreen compositor draw");
    if (target == g_last_textured_indexed_target)
        atomic_store(&g_indexed_target_pending, 0);
    static _Atomic(uint32_t) executions;
    uint32_t execution = atomic_fetch_add(&executions, 1) + 1;
    if (render_diagnostics_enabled() &&
        (execution <= 8 || (execution % 64) == 0))
        fprintf(stderr, "[D3D11 %s] fullscreen execution=%u shader=%08x source=%p "
                "source-hash=%08x target=%p target-hash=%08x %ux%u\n",
                vulkan_executed ? "VULKAN" : "SOFTWARE", execution,
                pixel_hash, (void *)source, resource_diagnostic_hash(source),
                (void *)target, resource_diagnostic_hash(target), width, height);
    return 1;
}

static void capture_indexed_draw_snapshot(const BeerD3D11PipelineState *state,
                                          uint32_t index_count,
                                          uint32_t start_index,
                                          int32_t base_vertex)
{
    static _Atomic(uint32_t) captures;
    if (!state || !getenv("BEER_D3D11_CAPTURE_DIR")) return;

    BeerD3D11Shader *vs = state->vertex_shader;
    BeerD3D11Shader *ps = state->pixel_shader;
    if (!vs || !ps || !com_get_header(vs) || !com_get_header(ps)) return;
    uint32_t vs_hash = fnv1a_bytes(vs->bytecode, vs->bytecode_size);
    uint32_t ps_hash = fnv1a_bytes(ps->bytecode, ps->bytecode_size);
    static uint64_t captured_pairs[128];
    static pthread_mutex_t capture_mutex = PTHREAD_MUTEX_INITIALIZER;
    uint64_t pair = ((uint64_t)vs_hash << 32) | ps_hash;
    pthread_mutex_lock(&capture_mutex);
    uint32_t capture = 0;
    for (uint32_t i = 0; i < 128; ++i) {
        if (captured_pairs[i] == pair) {
            pthread_mutex_unlock(&capture_mutex);
            return;
        }
        if (!captured_pairs[i] && !capture) capture = i + 1;
    }
    if (capture) captured_pairs[capture - 1] = pair;
    pthread_mutex_unlock(&capture_mutex);
    if (!capture) return;
    atomic_fetch_add(&captures, 1);

    capture_draw_blob("IDX-VS", vs_hash, vs->bytecode, vs->bytecode_size);
    capture_draw_blob("IDX-PS", ps_hash, ps->bytecode, ps->bytecode_size);
    BeerD3D11InputLayout *layout = state->input_layout;
    if (layout && com_get_header(layout)) {
        for (uint32_t i = 0; i < layout->element_count; ++i) {
            const BeerD3D11InputElement *element = &layout->elements[i];
            fprintf(stderr,
                    "[D3D11 INDEXED LAYOUT] element=%u semantic=%s%u format=%u "
                    "slot=%u offset=%u class=%u step=%u\n",
                    i, element->semantic, element->semantic_index,
                    element->format, element->input_slot,
                    element->aligned_byte_offset, element->input_slot_class,
                    element->instance_step_rate);
        }
    }

    BeerD3D11Resource *vb = validated_resource(state->vertex_buffers[0]);
    BeerD3D11Resource *ib = validated_resource(state->index_buffer);
    if (vb && vb->pixels) {
        uint32_t hash = fnv1a_bytes(vb->pixels, vb->pixel_size);
        capture_draw_blob("IDX-VB", hash, vb->pixels, vb->pixel_size);
    }
    if (ib && ib->pixels) {
        uint32_t hash = fnv1a_bytes(ib->pixels, ib->pixel_size);
        capture_draw_blob("IDX-IB", hash, ib->pixels, ib->pixel_size);
    }
    for (uint32_t i = 0; i < BEER_MAX_CONSTANT_BUFFERS; ++i) {
        BeerD3D11Resource *buffers[2] = {
            validated_resource(state->vs_constant_buffers[i]),
            validated_resource(state->ps_constant_buffers[i])
        };
        const char *stages[2] = { "IDX-VSCB", "IDX-PSCB" };
        for (uint32_t stage = 0; stage < 2; ++stage) {
            BeerD3D11Resource *buffer = buffers[stage];
            if (!buffer || !buffer->pixels) continue;
            fprintf(stderr,
                    "[D3D11 INDEXED CB] stage=%s slot=%u bytes=%zu hash=%08x\n",
                    stage ? "PS" : "VS", i, buffer->pixel_size,
                    fnv1a_bytes(buffer->pixels, buffer->pixel_size));
            char kind[32];
            snprintf(kind, sizeof(kind), "%s%u", stages[stage], i);
            capture_draw_blob(kind, fnv1a_bytes(buffer->pixels, buffer->pixel_size),
                              buffer->pixels, buffer->pixel_size);
        }
    }
    for (uint32_t i = 0; i < BEER_MAX_SHADER_RESOURCES; ++i) {
        BeerD3D11View *view = state->ps_shader_resources[i];
        BeerD3D11Resource *resource = view && com_get_header(view)
            ? validated_resource(view->resource) : NULL;
        if (!resource || !resource->pixels) continue;
        const uint32_t *desc = (const uint32_t *)resource->desc;
        fprintf(stderr,
                "[D3D11 INDEXED SRV] slot=%u width=%u height=%u format=%u "
                "bytes=%zu hash=%08x\n",
                i, desc[0], desc[1], desc[4], resource->pixel_size,
                fnv1a_bytes(resource->pixels, resource->pixel_size));
        char kind[32];
        snprintf(kind, sizeof(kind), "IDX-SRV%u-F%u", i, desc[4]);
        capture_draw_blob(kind, fnv1a_bytes(resource->pixels, resource->pixel_size),
                          resource->pixels, resource->pixel_size);
    }
    fprintf(stderr,
            "[D3D11 INDEXED CAPTURE] indices=%u start=%u base=%d topology=%u "
            "stride=%u vb-offset=%u index-format=%u index-offset=%u "
            "sampler0=%p viewport-count=%u\n",
            index_count, start_index, base_vertex, state->topology,
            state->vertex_strides[0], state->vertex_offsets[0],
            state->index_format, state->index_offset, state->ps_samplers[0],
            state->viewport_count);
}

typedef struct {
    float x, y;
    float color0[4];
    float color1[4];
    float u, v;
} BeerSoftwareVertex;

typedef struct {
    const uint8_t *data;
    uint32_t bit;
} BeerBitReader;

static uint32_t beer_read_bits(BeerBitReader *reader, uint32_t count)
{
    uint32_t value = 0;
    for (uint32_t i = 0; i < count; ++i) {
        value |= (uint32_t)((reader->data[reader->bit >> 3] >>
                            (reader->bit & 7)) & 1u) << i;
        ++reader->bit;
    }
    return value;
}

/* BC7 modes 0 and 7 use partitioned endpoint sets. Each packed partition
 * stores one two-bit subset number per texel; each anchor mask marks texels
 * whose interpolation index omits its high bit. Mode 0 only addresses the
 * first sixteen three-subset partitions, while mode 7 uses all two-subset
 * partitions. */
static const uint32_t g_bc7_partition2[64] = {
    0x50505050u, 0x40404040u, 0x54545454u, 0x54505040u,
    0x50404000u, 0x55545450u, 0x55545040u, 0x54504000u,
    0x50400000u, 0x55555450u, 0x55544000u, 0x54400000u,
    0x55555440u, 0x55550000u, 0x55555500u, 0x55000000u,
    0x55150100u, 0x00004054u, 0x15010000u, 0x00405054u,
    0x00004050u, 0x15050100u, 0x05010000u, 0x40505054u,
    0x00404050u, 0x05010100u, 0x14141414u, 0x05141450u,
    0x01155440u, 0x00555500u, 0x15014054u, 0x05414150u,
    0x44444444u, 0x55005500u, 0x11441144u, 0x05055050u,
    0x05500550u, 0x11114444u, 0x41144114u, 0x44111144u,
    0x15055054u, 0x01055040u, 0x05041050u, 0x05455150u,
    0x14414114u, 0x50050550u, 0x41411414u, 0x00141400u,
    0x00041504u, 0x00105410u, 0x10541000u, 0x04150400u,
    0x50410514u, 0x41051450u, 0x05415014u, 0x14054150u,
    0x41050514u, 0x41505014u, 0x40011554u, 0x54150140u,
    0x50505500u, 0x00555050u, 0x15151010u, 0x54540404u
};

static const uint16_t g_bc7_anchor2[64] = {
    0x8001u, 0x8001u, 0x8001u, 0x8001u, 0x8001u, 0x8001u, 0x8001u, 0x8001u,
    0x8001u, 0x8001u, 0x8001u, 0x8001u, 0x8001u, 0x8001u, 0x8001u, 0x8001u,
    0x8001u, 0x0005u, 0x0101u, 0x0005u, 0x0005u, 0x0101u, 0x0101u, 0x8001u,
    0x0005u, 0x0101u, 0x0005u, 0x0005u, 0x0101u, 0x0101u, 0x0005u, 0x0005u,
    0x8001u, 0x8001u, 0x0041u, 0x0101u, 0x0005u, 0x0101u, 0x8001u, 0x8001u,
    0x0005u, 0x0101u, 0x0005u, 0x0005u, 0x0005u, 0x8001u, 0x8001u, 0x0041u,
    0x0041u, 0x0005u, 0x0041u, 0x0101u, 0x8001u, 0x8001u, 0x0005u, 0x0005u,
    0x8001u, 0x8001u, 0x8001u, 0x8001u, 0x8001u, 0x0005u, 0x0005u, 0x8001u
};

static const uint32_t g_bc7_partition3_mode0[16] = {
    0xaa685050u, 0x6a5a5040u, 0x5a5a4200u, 0x5450a0a8u,
    0xa5a50000u, 0xa0a05050u, 0x5555a0a0u, 0x5a5a5050u,
    0xaa550000u, 0xaa555500u, 0xaaaa5500u, 0x90909090u,
    0x94949494u, 0xa4a4a4a4u, 0xa9a59450u, 0x2a0a4250u
};

static const uint16_t g_bc7_anchor3_mode0[16] = {
    0x8009u, 0x0109u, 0x8101u, 0x8009u,
    0x8101u, 0x8009u, 0x8009u, 0x8101u,
    0x8101u, 0x8101u, 0x8041u, 0x8041u,
    0x8041u, 0x8021u, 0x8009u, 0x0109u
};

static uint32_t bc7_expand_endpoint(uint32_t value, uint32_t bits)
{
    value <<= 8 - bits;
    return value | (value >> bits);
}

/* Decode every BC7 mode observed in Sekiro's Scaleform atlas. Partitioned
 * modes use the exact BC7 subset/anchor tables above. Modes 4 and 5 carry a
 * second index stream and optional channel rotation. Invalid mode prefixes
 * remain explicit failures instead of producing replacement colors. */
static int decode_bc7_block(const uint8_t block[16], uint8_t rgba[16][4])
{
    static const uint8_t color_bits_by_mode[8] = { 4, 6, 5, 7, 5, 7, 7, 5 };
    static const uint8_t alpha_bits_by_mode[8] = { 0, 0, 0, 0, 6, 8, 7, 5 };
    static const uint8_t weights2[4] = { 0, 21, 43, 64 };
    static const uint8_t weights3[8] = { 0, 9, 18, 27, 37, 46, 55, 64 };
    static const uint8_t weights4[16] = {
        0, 4, 9, 13, 17, 21, 26, 30,
        34, 38, 43, 47, 51, 55, 60, 64
    };

    uint32_t mode = 0;
    while (mode < 8 && !(block[mode >> 3] & (1u << (mode & 7)))) ++mode;
    if (mode >= 8 || mode == 2) return 0;

    BeerBitReader reader = { block, mode + 1 };
    uint32_t subsets = (mode == 0) ? 3 :
        (mode == 1 || mode == 3 || mode == 7) ? 2 : 1;
    uint32_t partition = mode == 0 ? beer_read_bits(&reader, 4) :
        subsets > 1 ? beer_read_bits(&reader, 6) : 0;
    uint32_t rotation = (mode == 4 || mode == 5)
        ? beer_read_bits(&reader, 2) : 0;
    uint32_t index_selection = mode == 4 ? beer_read_bits(&reader, 1) : 0;
    uint32_t endpoint_count = subsets * 2;
    uint32_t color_bits = color_bits_by_mode[mode];
    uint32_t alpha_bits = alpha_bits_by_mode[mode];
    uint32_t endpoint[6][4] = { { 0 } };

    for (uint32_t channel = 0; channel < 3; ++channel)
        for (uint32_t i = 0; i < endpoint_count; ++i)
            endpoint[i][channel] = beer_read_bits(&reader, color_bits);
    if (alpha_bits)
        for (uint32_t i = 0; i < endpoint_count; ++i)
            endpoint[i][3] = beer_read_bits(&reader, alpha_bits);

    if (mode == 1) {
        uint32_t shared_pbit[2] = {
            beer_read_bits(&reader, 1), beer_read_bits(&reader, 1)
        };
        for (uint32_t i = 0; i < endpoint_count; ++i)
            for (uint32_t channel = 0; channel < 3; ++channel)
                endpoint[i][channel] = (endpoint[i][channel] << 1) |
                    shared_pbit[i / 2];
        ++color_bits;
    } else if (mode == 0 || mode == 3 || mode == 6 || mode == 7) {
        for (uint32_t i = 0; i < endpoint_count; ++i) {
            uint32_t pbit = beer_read_bits(&reader, 1);
            for (uint32_t channel = 0; channel < 3; ++channel)
                endpoint[i][channel] = (endpoint[i][channel] << 1) | pbit;
            if (alpha_bits)
                endpoint[i][3] = (endpoint[i][3] << 1) | pbit;
        }
        ++color_bits;
        if (alpha_bits) ++alpha_bits;
    }

    for (uint32_t i = 0; i < endpoint_count; ++i) {
        for (uint32_t channel = 0; channel < 3; ++channel)
            endpoint[i][channel] = bc7_expand_endpoint(
                endpoint[i][channel], color_bits);
        endpoint[i][3] = alpha_bits
            ? bc7_expand_endpoint(endpoint[i][3], alpha_bits) : 255;
    }

    uint32_t primary_bits = (mode == 0 || mode == 1) ? 3 :
        mode == 6 ? 4 : 2;
    uint32_t secondary_bits = mode == 4 ? 3 : mode == 5 ? 2 : 0;
    const uint8_t *primary_weights = primary_bits == 4 ? weights4 :
        primary_bits == 3 ? weights3 : weights2;
    const uint8_t *secondary_weights = secondary_bits == 3 ? weights3 : weights2;
    uint32_t partition_map = subsets == 3 ? g_bc7_partition3_mode0[partition] :
        subsets == 2 ? g_bc7_partition2[partition] : 0;
    uint16_t anchor_map = subsets == 3 ? g_bc7_anchor3_mode0[partition] :
        subsets == 2 ? g_bc7_anchor2[partition] : 1;
    uint8_t primary_index[16];

    for (uint32_t texel = 0; texel < 16; ++texel) {
        uint32_t bits = primary_bits - ((anchor_map >> texel) & 1u);
        primary_index[texel] = (uint8_t)beer_read_bits(&reader, bits);
    }

    for (uint32_t texel = 0; texel < 16; ++texel) {
        uint32_t subset = (partition_map >> (texel * 2)) & 3u;
        uint32_t color_index = primary_index[texel];
        uint32_t alpha_index = color_index;
        const uint8_t *color_weights = primary_weights;
        const uint8_t *alpha_weights = primary_weights;
        if (secondary_bits) {
            uint32_t secondary_index = beer_read_bits(
                &reader, secondary_bits - (texel == 0));
            if (index_selection) {
                color_index = secondary_index;
                color_weights = secondary_weights;
            } else {
                alpha_index = secondary_index;
                alpha_weights = secondary_weights;
            }
        }
        uint32_t color_weight = color_weights[color_index];
        uint32_t alpha_weight = alpha_weights[alpha_index];
        for (uint32_t channel = 0; channel < 3; ++channel)
            rgba[texel][channel] = (uint8_t)(
                (endpoint[subset * 2][channel] * (64 - color_weight) +
                 endpoint[subset * 2 + 1][channel] * color_weight + 32) >> 6);
        rgba[texel][3] = (uint8_t)(
            (endpoint[subset * 2][3] * (64 - alpha_weight) +
             endpoint[subset * 2 + 1][3] * alpha_weight + 32) >> 6);
        if (rotation) {
            uint32_t channel = rotation - 1;
            uint8_t temporary = rgba[texel][3];
            rgba[texel][3] = rgba[texel][channel];
            rgba[texel][channel] = temporary;
        }
    }
    return 1;
}

/* ---------------------------------------------------------------------------
 * NV12 → RGBA8 software conversion cache.
 *
 * RE8's Media Foundation movie compositor binds the NV12 texture as an SRV
 * and reads it with a pixel shader that converts YUV → RGB.  Beer cannot
 * hand an NV12 buffer to Vulkan as a plain sampled image, so we decode it
 * into a full RGBA8 plane on the CPU and expose that plane to the generic
 * Vulkan image path (format 37 = VK_FORMAT_R8G8B8A8_UNORM, 4 bpp).
 *
 * The decoded cache is stored in resource->decoded_rgba, exactly as BC7.
 * The serial gate guarantees the conversion runs at most once per
 * UpdateSubresource call.
 * ---------------------------------------------------------------------------*/
static int ensure_nv12_cache(BeerD3D11Resource *texture)
{
    if (!texture || !texture->pixels || texture->desc_size < 20 ||
        ((const uint32_t *)texture->desc)[4] != 103)
        return 0;
    const uint32_t *desc = (const uint32_t *)texture->desc;
    uint32_t width = desc[0], height = desc[1];
    if (!width || !height ||
        (size_t)width > SIZE_MAX / 4u / (size_t)height)
        return 0;

    uint64_t serial = atomic_load(&texture->content_serial);
    size_t decoded_size = (size_t)width * height * 4;
    pthread_mutex_lock(&texture->decoded_lock);
    if (texture->decoded_rgba && texture->decoded_rgba_size == decoded_size &&
        texture->decoded_content_serial == serial) {
        pthread_mutex_unlock(&texture->decoded_lock);
        return 1;
    }

    uint8_t *out = realloc(texture->decoded_rgba, decoded_size);
    if (!out) {
        pthread_mutex_unlock(&texture->decoded_lock);
        return 0;
    }
    texture->decoded_rgba = out;
    texture->decoded_rgba_size = decoded_size;

    /* NV12 layout: Y plane  = pixels[0 .. width*height - 1]
     *              UV plane = pixels[width*height .. width*height*3/2 - 1]
     *              Each UV pair covers a 2×2 luma block. */
    const uint8_t *y_plane  = texture->pixels;
    const uint8_t *uv_plane = texture->pixels + (size_t)width * height;

    for (uint32_t row = 0; row < height; ++row) {
        uint32_t uv_row = row >> 1;
        for (uint32_t col = 0; col < width; ++col) {
            uint32_t uv_col = col & ~1u; /* even column for interleaved UV */
            int y  = (int)(uint8_t)y_plane[(size_t)row * width + col];
            int cb = (int)(uint8_t)uv_plane[(size_t)uv_row * width + uv_col];
            int cr = (int)(uint8_t)uv_plane[(size_t)uv_row * width + uv_col + 1];
            /* BT.601 limited-range YCbCr → linear RGB (integer fast path) */
            int c = y  - 16;
            int d = cb - 128;
            int e = cr - 128;
            int r = (298 * c           + 409 * e + 128) >> 8;
            int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
            int b = (298 * c + 516 * d           + 128) >> 8;
            if (r < 0) r = 0; if (r > 255) r = 255;
            if (g < 0) g = 0; if (g > 255) g = 255;
            if (b < 0) b = 0; if (b > 255) b = 255;
            uint8_t *dst = out + ((size_t)row * width + col) * 4;
            dst[0] = (uint8_t)r;
            dst[1] = (uint8_t)g;
            dst[2] = (uint8_t)b;
            dst[3] = 255;
        }
    }
    texture->decoded_content_serial = serial;
    pthread_mutex_unlock(&texture->decoded_lock);
    return 1;
}

static _Atomic(uint32_t) g_bc7_unsupported_blocks;

static int ensure_bc7_cache(BeerD3D11Resource *texture)
{
    if (!texture || !texture->pixels || texture->desc_size < 20 ||
        ((const uint32_t *)texture->desc)[4] != 98)
        return 0;
    const uint32_t *desc = (const uint32_t *)texture->desc;
    uint32_t width = desc[0], height = desc[1];
    if (!width || !height || !texture->subresource_row_pitches ||
        !texture->subresource_offsets ||
        (size_t)width > SIZE_MAX / 4u / (size_t)height)
        return 0;

    uint64_t serial = atomic_load(&texture->content_serial);
    size_t decoded_size = (size_t)width * height * 4;
    pthread_mutex_lock(&texture->decoded_lock);
    if (texture->decoded_rgba && texture->decoded_rgba_size == decoded_size &&
        texture->decoded_content_serial == serial) {
        pthread_mutex_unlock(&texture->decoded_lock);
        return 1;
    }

    uint8_t *decoded_texture = realloc(texture->decoded_rgba, decoded_size);
    if (!decoded_texture) {
        pthread_mutex_unlock(&texture->decoded_lock);
        return 0;
    }
    texture->decoded_rgba = decoded_texture;
    texture->decoded_rgba_size = decoded_size;
    memset(decoded_texture, 0, decoded_size);

    for (uint32_t block_y = 0; block_y < (height + 3u) / 4u; ++block_y) {
        for (uint32_t block_x = 0; block_x < (width + 3u) / 4u; ++block_x) {
            size_t offset = texture->subresource_offsets[0] +
                (size_t)block_y * texture->subresource_row_pitches[0] +
                (size_t)block_x * 16;
            if (offset > texture->pixel_size || 16 > texture->pixel_size - offset)
                continue;
            uint8_t block[16][4];
            if (!decode_bc7_block(texture->pixels + offset, block)) {
                atomic_fetch_add(&g_bc7_unsupported_blocks, 1);
                continue;
            }
            for (uint32_t local_y = 0; local_y < 4; ++local_y) {
                uint32_t y = block_y * 4 + local_y;
                if (y >= height) break;
                for (uint32_t local_x = 0; local_x < 4; ++local_x) {
                    uint32_t x = block_x * 4 + local_x;
                    if (x >= width) break;
                    memcpy(decoded_texture + ((size_t)y * width + x) * 4,
                           block[local_y * 4 + local_x], 4);
                }
            }
        }
    }
    texture->decoded_content_serial = serial;
    pthread_mutex_unlock(&texture->decoded_lock);
    return 1;
}

static void software_address_coordinate(float *coordinate, uint32_t mode)
{
    if (mode == 3) { /* D3D11_TEXTURE_ADDRESS_CLAMP */
        if (*coordinate < 0.0f) *coordinate = 0.0f;
        if (*coordinate > 1.0f) *coordinate = 1.0f;
        return;
    }

    int32_t integral = (int32_t)*coordinate;
    if (*coordinate < 0.0f && (float)integral != *coordinate) --integral;
    *coordinate -= (float)integral;
    if (mode == 2 && (integral & 1)) /* D3D11_TEXTURE_ADDRESS_MIRROR */
        *coordinate = 1.0f - *coordinate;
    /* D3D11_TEXTURE_ADDRESS_WRAP (1) and the conservative fallback both use
     * the fractional coordinate. BORDER/MIRROR_ONCE are not observed here. */
}

static int sample_bc7(BeerD3D11Resource *texture,
                      float u, float v, int linear,
                      uint32_t address_u, uint32_t address_v, float rgba[4])
{
    if (!ensure_bc7_cache(texture)) return 0;
    const uint32_t *desc = (const uint32_t *)texture->desc;
    uint32_t width = desc[0], height = desc[1];

    software_address_coordinate(&u, address_u);
    software_address_coordinate(&v, address_v);

    float texel_x = u * (float)width - 0.5f;
    float texel_y = v * (float)height - 0.5f;
    int32_t x0 = (int32_t)texel_x;
    int32_t y0 = (int32_t)texel_y;
    if (texel_x < 0.0f && (float)x0 != texel_x) --x0;
    if (texel_y < 0.0f && (float)y0 != texel_y) --y0;
    float fraction_x = texel_x - (float)x0;
    float fraction_y = texel_y - (float)y0;
    if (!linear) {
        x0 = (int32_t)(u * (float)width);
        y0 = (int32_t)(v * (float)height);
        fraction_x = fraction_y = 0.0f;
    }

    for (uint32_t channel = 0; channel < 4; ++channel) rgba[channel] = 0.0f;
    for (uint32_t sample_y = 0; sample_y < (linear ? 2u : 1u); ++sample_y) {
        int32_t y = y0 + (int32_t)sample_y;
        if (y < 0) y = 0;
        if (y >= (int32_t)height) y = (int32_t)height - 1;
        float weight_y = sample_y ? fraction_y : 1.0f - fraction_y;
        if (!linear) weight_y = 1.0f;
        for (uint32_t sample_x = 0; sample_x < (linear ? 2u : 1u); ++sample_x) {
            int32_t x = x0 + (int32_t)sample_x;
            if (x < 0) x = 0;
            if (x >= (int32_t)width) x = (int32_t)width - 1;
            float weight_x = sample_x ? fraction_x : 1.0f - fraction_x;
            if (!linear) weight_x = 1.0f;
            const uint8_t *sample = texture->decoded_rgba +
                ((size_t)y * width + (uint32_t)x) * 4;
            for (uint32_t channel = 0; channel < 4; ++channel)
                rgba[channel] += (float)sample[channel] / 255.0f * weight_x * weight_y;
        }
    }
    return 1;
}

static int software_vertex_from_index(const BeerD3D11PipelineState *state,
                                      const BeerD3D11Resource *vertices,
                                      uint32_t vertex_index,
                                      uint32_t vertex_shader_hash,
                                      BeerSoftwareVertex *out)
{
    if (!state || !vertices || !vertices->pixels || !out ||
        (state->vertex_strides[0] != 16 && state->vertex_strides[0] != 12))
        return 0;
    size_t offset = (size_t)state->vertex_offsets[0] +
        (size_t)vertex_index * state->vertex_strides[0];
    if (offset > vertices->pixel_size ||
        state->vertex_strides[0] > vertices->pixel_size - offset)
        return 0;

    const uint8_t *source = vertices->pixels + offset;
    for (uint32_t channel = 0; channel < 4; ++channel) {
        out->color0[channel] = (float)source[channel] / 255.0f;
        out->color1[channel] = state->vertex_strides[0] == 16
            ? (float)source[4 + channel] / 255.0f : 1.0f;
    }
    float position[2];
    memcpy(position, source + (state->vertex_strides[0] == 16 ? 8 : 4),
           sizeof(position));

    BeerD3D11Resource *constants = validated_resource(state->vs_constant_buffers[0]);
    if (!constants || !constants->pixels || constants->pixel_size < 96)
        return 0;
    const float *constant = (const float *)constants->pixels;
    /* Scaleform's compact vertex shaders use different constant-row pairs.
     * The output signatures identify which register is SV_Position:
     * - VS 94e79d4e: o4 (position) uses cb0[2:3], o3 (UV) uses cb0[4:5].
     * - VS 23ac3836: o2 (position) uses cb0[0:1], o1 (UV) uses cb0[2:3]. */
    uint32_t transform_row =
        (vertex_shader_hash == 0x222ff8d7 || vertex_shader_hash == 0x94e79d4e)
            ? 8 : 0;
    float clip_x = position[0] * constant[transform_row] +
        position[1] * constant[transform_row + 1] + constant[transform_row + 3];
    float clip_y = position[0] * constant[transform_row + 4] +
        position[1] * constant[transform_row + 5] + constant[transform_row + 7];
    if (vertex_shader_hash == 0x94e79d4e) {
        out->u = position[0] * constant[16] + position[1] * constant[17] + constant[19];
        out->v = position[0] * constant[20] + position[1] * constant[21] + constant[23];
    } else if (vertex_shader_hash == 0x23ac3836) {
        out->u = position[0] * constant[8] + position[1] * constant[9] + constant[11];
        out->v = position[0] * constant[12] + position[1] * constant[13] + constant[15];
    } else {
        out->u = out->v = 0.0f;
    }
    if (vertex_shader_hash == 0x222ff8d7) {
        /* VS 222ff8d7 forwards cb0[0:1]; PS 625c789a computes
         * inputColor * cb0[1] + cb0[0], then multiplies alpha by COLOR1.a. */
        for (uint32_t channel = 0; channel < 4; ++channel)
            out->color0[channel] = out->color0[channel] * constant[4 + channel] +
                constant[channel];
    }

    const float *viewport = state->viewports[0];
    out->x = viewport[0] + (clip_x + 1.0f) * viewport[2] * 0.5f;
    out->y = viewport[1] + (1.0f - clip_y) * viewport[3] * 0.5f;
    return 1;
}

static void software_blend_factor(uint32_t factor, uint32_t channel,
                                  const float source[4],
                                  const float destination[4],
                                  const float blend_factor[4], float *result)
{
    switch (factor) {
    case 1: *result = 0.0f; break; /* D3D11_BLEND_ZERO */
    case 2: *result = 1.0f; break; /* D3D11_BLEND_ONE */
    case 3: *result = source[channel]; break;
    case 4: *result = 1.0f - source[channel]; break;
    case 5: *result = source[3]; break;
    case 6: *result = 1.0f - source[3]; break;
    case 7: *result = destination[3]; break;
    case 8: *result = 1.0f - destination[3]; break;
    case 9: *result = destination[channel]; break;
    case 10: *result = 1.0f - destination[channel]; break;
    case 11: { /* D3D11_BLEND_SRC_ALPHA_SAT */
        float saturated = source[3] < 1.0f - destination[3]
            ? source[3] : 1.0f - destination[3];
        *result = channel == 3 ? 1.0f : saturated;
        break;
    }
    case 14: *result = blend_factor[channel]; break;
    case 15: *result = 1.0f - blend_factor[channel]; break;
    default: *result = 1.0f; break;
    }
}

static void software_blend_operation(uint32_t operation,
                                     float source, float destination,
                                     float *result)
{
    switch (operation) {
    case 1: *result = source + destination; break; /* D3D11_BLEND_OP_ADD */
    case 2: *result = source - destination; break;
    case 3: *result = destination - source; break;
    case 4: *result = source < destination ? source : destination; break;
    case 5: *result = source > destination ? source : destination; break;
    default: *result = source; break;
    }
}

static int software_blend_pixel(BeerD3D11Resource *target, size_t pixel,
                                const float source[4],
                                const BeerD3D11PipelineState *state)
{
    uint32_t destination_packed[4];
    unpack_rgba(target, pixel, destination_packed);
    float destination[4];
    float output[4];
    for (uint32_t channel = 0; channel < 4; ++channel) {
        destination[channel] = (float)destination_packed[channel] / 1023.0f;
        output[channel] = source[channel];
    }

    uint8_t write_mask = 0x0f;
    BeerD3D11State *blend = (BeerD3D11State *)state->blend_state;
    if (blend && com_get_header(blend) && blend->kind == BEER_STATE_BLEND &&
        blend->desc_size >= 40) {
        /* D3D11_BLEND_DESC starts with two BOOLs followed by the first
         * 32-byte D3D11_RENDER_TARGET_BLEND_DESC. */
        const uint8_t *render_target = blend->desc + 8;
        uint32_t blend_enable, source_blend, destination_blend, color_operation;
        uint32_t source_alpha, destination_alpha, alpha_operation;
        memcpy(&blend_enable, render_target, 4);
        memcpy(&source_blend, render_target + 4, 4);
        memcpy(&destination_blend, render_target + 8, 4);
        memcpy(&color_operation, render_target + 12, 4);
        memcpy(&source_alpha, render_target + 16, 4);
        memcpy(&destination_alpha, render_target + 20, 4);
        memcpy(&alpha_operation, render_target + 24, 4);
        write_mask = render_target[28];
        if (blend_enable) {
            for (uint32_t channel = 0; channel < 3; ++channel) {
                float source_factor, destination_factor;
                software_blend_factor(source_blend, channel, source, destination,
                                      state->blend_factor, &source_factor);
                software_blend_factor(destination_blend, channel, source,
                                      destination, state->blend_factor,
                                      &destination_factor);
                float source_term = source[channel] * source_factor;
                float destination_term = destination[channel] * destination_factor;
                software_blend_operation(color_operation, source_term,
                                         destination_term, &output[channel]);
            }
            float source_factor, destination_factor;
            software_blend_factor(source_alpha, 3, source, destination,
                                  state->blend_factor, &source_factor);
            software_blend_factor(destination_alpha, 3, source, destination,
                                  state->blend_factor, &destination_factor);
            software_blend_operation(alpha_operation, source[3] * source_factor,
                                     destination[3] * destination_factor,
                                     &output[3]);
        }
    }

    uint32_t packed[4];
    for (uint32_t channel = 0; channel < 4; ++channel) {
        float value = (write_mask & (1u << channel))
            ? output[channel] : destination[channel];
        if (!(value > 0.0f)) value = 0.0f;
        if (value > 1.0f) value = 1.0f;
        packed[channel] = (uint32_t)(value * 1023.0f + 0.5f);
    }
    int changed = 0;
    for (uint32_t channel = 0; channel < 4; ++channel)
        if (packed[channel] != destination_packed[channel]) changed = 1;
    pack_rgba(target, pixel, packed);
    return changed;
}

/* ---------------------------------------------------------------------------
 * Auto-translated indexed draw path (Sekiro UI migration).
 *
 * Translates the bound VS+PS DXBC through the shared vkd3d-shader translator,
 * maps D3D11 pipeline state to Vulkan descriptor / vertex / target structs,
 * and dispatches through beer_vulkan_graphics_draw_state(). Returns 1 on
 * success and 0 on any rejection without aborting. Per-hash manual paths in
 * execute_initial_indexed_draw remain as fallback until all pairs migrate.
 * ---------------------------------------------------------------------------
 *
 * D3D11 address-mode enum -> VkSamplerAddressMode:
 *   1 WRAP   -> 0 REPEAT
 *   2 MIRROR -> 1 MIRRORED_REPEAT
 *   3 CLAMP  -> 2 CLAMP_TO_EDGE
 *   4 BORDER -> 3 CLAMP_TO_BORDER
 */
static uint32_t d3d11_to_vk_address_mode(uint32_t d3d)
{
    if (d3d >= 1 && d3d <= 4) return d3d - 1u;
    return 2; /* default clamp */
}

/* Minimal DXGI-format -> (VkFormat, bytes-per-pixel) needed for D3D11 SRVs.
 * Only uncompressed formats that appear in Sekiro's UI pipeline are listed;
 * BC7 (98) is intentionally absent (the manual path handles that via the
 * software BC7 decoder).
 * NV12 (103) is exposed as RGBA8 (VkFormat 37, 4 bpp) because Beer converts
 * the YUV planes to RGBA8 in ensure_nv12_cache() before handing the buffer
 * to Vulkan.  The caller must call ensure_nv12_cache() and substitute
 * decoded_rgba / decoded_rgba_size instead of pixels / pixel_size. */
static int d3d11_dxgi_to_vulkan_image(uint32_t dxgi,
                                       uint32_t *vk_format,
                                       uint32_t *bytes_per_pixel)
{
    /* Uncompressed formats: {dxgi, vk_format, bytes_per_pixel} */
    static const struct { uint32_t dxgi, vk, bpp; } table[] = {
        { 2, 109, 16}, /* R32G32B32A32_FLOAT */
        { 6, 106, 12}, /* R32G32B32_FLOAT */
        {16, 103,  8}, /* R32G32_FLOAT */
        {41,  97,  4}, /* R32_FLOAT */
        {28,  37,  4}, /* R8G8B8A8_UNORM */
        {29,  43,  4}, /* R8G8B8A8_UNORM_SRGB */
        {87,  44,  4}, /* B8G8R8A8_UNORM */
        {91,  50,  4}, /* B8G8R8A8_UNORM_SRGB */
        {61,   9,  1}, /* R8_UNORM */
        {63,  10,  1}, /* R8_UINT  */
        {49,  16,  2}, /* R16_UNORM */
        {57,  74,  2}, /* R16_UINT (index / texel) */
        {24,  64,  4}, /* R10G10B10A2_UNORM */
        {26, 122,  4}, /* R11G11B10_FLOAT */
        {54,  76,  2}, /* R16G16_FLOAT */
        {35,  98,  8}, /* R16G16B16A16_FLOAT */
        /* NV12: decoded to RGBA8 by ensure_nv12_cache(); advertised as
         * VK_FORMAT_R8G8B8A8_UNORM (37) so the generic SRV path can sample it.
         * The SRV binding code below checks for format 103 and switches to
         * decoded_rgba / decoded_rgba_size automatically. */
        {103,  37,  4}, /* DXGI_FORMAT_NV12 -> RGBA8 after CPU YUV decode */
    };
    for (size_t i = 0; i < sizeof(table)/sizeof(table[0]); ++i) {
        if (table[i].dxgi == dxgi) {
            *vk_format       = table[i].vk;
            *bytes_per_pixel = table[i].bpp;
            return 1;
        }
    }
    /* Block-compressed formats signal via bpp = 0 (caller must use block
     * fields instead).  vk_format is still returned for image creation. */
    *bytes_per_pixel = 0;
    switch (dxgi) {
        case 71: *vk_format = 131; return 1; /* BC1_UNORM        VK 131 */
        case 72: *vk_format = 132; return 1; /* BC1_UNORM_SRGB   VK 132 */
        case 74: *vk_format = 135; return 1; /* BC2_UNORM        VK 135 */
        case 75: *vk_format = 136; return 1; /* BC2_UNORM_SRGB   VK 136 */
        case 77: *vk_format = 137; return 1; /* BC3_UNORM        VK 137 */
        case 78: *vk_format = 138; return 1; /* BC3_UNORM_SRGB   VK 138 */
        case 80: *vk_format = 139; return 1; /* BC4_UNORM        VK 139 */
        case 81: *vk_format = 140; return 1; /* BC4_SNORM        VK 140 */
        case 83: *vk_format = 141; return 1; /* BC5_UNORM        VK 141 */
        case 84: *vk_format = 142; return 1; /* BC5_SNORM        VK 142 */
        case 95: *vk_format = 143; return 1; /* BC6H_UF16        VK 143 */
        case 96: *vk_format = 144; return 1; /* BC6H_SF16        VK 144 */
        case 98: *vk_format = 145; return 1; /* BC7_UNORM        VK 145 */
        case 99: *vk_format = 146; return 1; /* BC7_UNORM_SRGB   VK 146 */
        default: return 0;
    }
}

static int try_auto_translate_indexed_draw(const BeerD3D11PipelineState *state,
                                           uint32_t index_count,
                                           uint32_t start_index,
                                           int32_t  base_vertex)
{
    if (!state || !state->vertex_shader || !state->pixel_shader ||
        !state->vertex_buffers[0] || !state->index_buffer ||
        !state->render_targets[0] || state->viewport_count == 0)
        return 0;

    BeerD3D11Shader *vs = state->vertex_shader;
    BeerD3D11Shader *ps = state->pixel_shader;
    if (!com_get_header(vs) || !com_get_header(ps)) return 0;

    uint64_t vs_hash = ((uint64_t)fnv1a_bytes(vs->bytecode, vs->bytecode_size) << 32) |
                        fnv1a_bytes(vs->bytecode + vs->bytecode_size / 2,
                                    vs->bytecode_size - vs->bytecode_size / 2);
    uint64_t ps_hash = ((uint64_t)fnv1a_bytes(ps->bytecode, ps->bytecode_size) << 32) |
                        fnv1a_bytes(ps->bytecode + ps->bytecode_size / 2,
                                    ps->bytecode_size - ps->bytecode_size / 2);

    const BeerD3D12TranslatedShader *tvs =
        beer_d3d12_translate_shader(vs->bytecode, vs->bytecode_size, vs_hash);
    const BeerD3D12TranslatedShader *tps =
        beer_d3d12_translate_shader(ps->bytecode, ps->bytecode_size, ps_hash);
    if (!tvs || !tps) {
        static _Atomic(uint32_t) fail_count;
        uint32_t fn = atomic_fetch_add(&fail_count, 1);
        /* Always log — unlimited, deduplicated by VS+PS pair via counter cap. */
        if (fn < 32)
            fprintf(stderr,
                    "[D3D11 AUTO TRANSLATE FAIL] #%u VS=%08x PS=%08x: %s "
                    "(tvs=%p tps=%p bytecodes=%zu/%zu)\n",
                    fn + 1, (uint32_t)vs_hash, (uint32_t)ps_hash,
                    beer_d3d12_shader_translation_error(),
                    (void *)tvs, (void *)tps,
                    vs->bytecode_size, ps->bytecode_size);
        return 0;
    }
    /* Log first translation for each unique VS+PS pair only. */
    {
        static uint64_t logged_pairs[64];
        static uint32_t logged_count;
        static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
        uint64_t pair_key = vs_hash ^ (ps_hash * 0x9e3779b97f4a7c15ULL);
        pthread_mutex_lock(&log_lock);
        int already = 0;
        for (uint32_t _i = 0; _i < logged_count && _i < 64; ++_i)
            if (logged_pairs[_i] == pair_key) { already = 1; break; }
        if (!already && logged_count < 64)
            logged_pairs[logged_count++] = pair_key;
        pthread_mutex_unlock(&log_lock);
        if (!already)
            fprintf(stderr,
                    "[D3D11 AUTO TRANSLATE OK] VS=%08x PS=%08x "
                    "vs-desc=%u ps-desc=%u vs-spirv=%zu ps-spirv=%zu\n",
                    (uint32_t)vs_hash, (uint32_t)ps_hash,
                    tvs->descriptor_count, tps->descriptor_count,
                    tvs->spirv_size, tps->spirv_size);
    }

    /* ---- Build descriptor bindings from translated shader declarations ---- */
    BeerVulkanGraphicsBinding bindings[BEER_VULKAN_GRAPHICS_MAX_BINDINGS];
    memset(bindings, 0, sizeof(bindings));
    uint32_t binding_count = 0;

    for (uint32_t stage = 0; stage < 2; ++stage) {
        const BeerD3D12TranslatedShader *shader = stage ? tps : tvs;
        uint32_t stage_flag = stage ? BEER_VULKAN_GRAPHICS_STAGE_FRAGMENT
                                    : BEER_VULKAN_GRAPHICS_STAGE_VERTEX;
        for (uint32_t d = 0; d < shader->descriptor_count; ++d) {
            const BeerD3D12ShaderDescriptor *desc = &shader->descriptors[d];
            if (binding_count >= BEER_VULKAN_GRAPHICS_MAX_BINDINGS) return 0;
            BeerVulkanGraphicsBinding *b = &bindings[binding_count++];
            b->binding    = desc->binding;
            b->stage_flags = stage_flag;
            uint32_t slot = desc->register_index;

            if (desc->type == BEER_D3D12_SHADER_DESCRIPTOR_CBV) {
                /* VS or PS constant buffer slot */
                BeerD3D11Resource *cbuf = validated_resource(
                    stage ? state->ps_constant_buffers[slot < 8 ? slot : 0]
                          : state->vs_constant_buffers[slot < 8 ? slot : 0]);
                if (!cbuf || !cbuf->pixels) return 0;
                b->kind = BEER_VULKAN_GRAPHICS_BINDING_UNIFORM_BUFFER;
                b->data = cbuf->pixels;
                b->size = cbuf->pixel_size;
            } else if (desc->type == BEER_D3D12_SHADER_DESCRIPTOR_SRV &&
                       stage == 1 /* PS only */) {
                BeerD3D11View *view = slot < BEER_MAX_SHADER_RESOURCES
                    ? state->ps_shader_resources[slot] : NULL;
                BeerD3D11Resource *res = view && com_get_header(view)
                    ? validated_resource(view->resource) : NULL;
                if (!res || !res->pixels || res->desc_size < 20) return 0;
                const uint32_t *rdesc = (const uint32_t *)res->desc;
                uint32_t vk_fmt = 0, bpp = 0;
                if (!d3d11_dxgi_to_vulkan_image(rdesc[4], &vk_fmt, &bpp)) return 0;
                b->kind            = BEER_VULKAN_GRAPHICS_BINDING_SAMPLED_IMAGE;
                b->resource        = res;
                b->serial          = (const uint64_t *)&res->content_serial;
                b->format          = vk_fmt;
                b->width           = rdesc[0];
                b->height          = rdesc[1];
                /* NV12: bind the CPU-decoded RGBA8 plane rather than raw YUV. */
                if (rdesc[4] == 103 /* NV12 */) {
                    if (!ensure_nv12_cache(res)) return 0;
                    b->data = res->decoded_rgba;
                    b->size = res->decoded_rgba_size;
                    b->bytes_per_pixel = 4;
                    b->row_pitch = rdesc[0] * 4u;
                } else if (bpp == 0) {
                    b->data = res->pixels;
                    b->size = res->pixel_size;
                    /* Block-compressed texture (BC1-BC7).  Determine
                     * bytes-per-block from the VkFormat number:
                     *   BC1/BC4 (131,132,139,140) -> 4x4 block, 8 bytes
                     *   BC2/BC3/BC5/BC6H/BC7 (135-138,141-146) -> 4x4, 16 bytes */
                    uint32_t bpb = (vk_fmt == 131 || vk_fmt == 132 ||
                                    vk_fmt == 139 || vk_fmt == 140) ? 8u : 16u;
                    b->block_width    = 4;
                    b->block_height   = 4;
                    b->bytes_per_block = bpb;
                    b->bytes_per_pixel = 0;
                    /* Row pitch in bytes covering full-block rows */
                    uint32_t blocks_wide = (rdesc[0] + 3u) / 4u;
                    b->row_pitch = res->row_pitch ? res->row_pitch : blocks_wide * bpb;
                } else {
                    b->data = res->pixels;
                    b->size = res->pixel_size;
                    b->bytes_per_pixel = bpp;
                    b->row_pitch = res->row_pitch ? res->row_pitch : rdesc[0] * bpp;
                }
            } else if (desc->type == BEER_D3D12_SHADER_DESCRIPTOR_SAMPLER &&
                       stage == 1 /* PS only */) {
                BeerD3D11SamplerState *samp = slot < 8
                    ? state->ps_samplers[slot] : NULL;
                const uint32_t *sdesc = samp && com_get_header(samp)
                    ? (const uint32_t *)samp->desc : NULL;
                b->kind       = BEER_VULKAN_GRAPHICS_BINDING_SAMPLER;
                b->min_filter = sdesc && sdesc[0] == 0x15 ? 1u : 0u;
                b->mag_filter = b->min_filter;
                b->address_u  = sdesc ? d3d11_to_vk_address_mode(sdesc[1]) : 2u;
                b->address_v  = sdesc ? d3d11_to_vk_address_mode(sdesc[2]) : 2u;
                b->address_w  = 2u; /* clamp */
            } else {
                /* VS SRVs and other unsupported descriptor types */
                return 0;
            }
        }
    }

    /* ---- Vertex bindings from input layout + bound VB ---- */
    BeerD3D11InputLayout *layout = state->input_layout;
    if (!layout || !com_get_header(layout)) return 0;

    BeerVulkanGraphicsVertexBinding  vb[BEER_VULKAN_GRAPHICS_MAX_VERTEX_BINDINGS];
    BeerVulkanGraphicsVertexAttribute va[BEER_VULKAN_GRAPHICS_MAX_VERTEX_ATTRIBUTES];
    memset(vb, 0, sizeof(vb));
    memset(va, 0, sizeof(va));
    uint32_t vb_count = 0, va_count = 0;

    /* Per-slot running byte offsets for D3D11_APPEND_ALIGNED_ELEMENT (0xffffffff). */
    uint32_t slot_offsets[BEER_VULKAN_GRAPHICS_MAX_VERTEX_BINDINGS];
    memset(slot_offsets, 0, sizeof(slot_offsets));

    for (uint32_t e = 0; e < layout->element_count &&
         va_count < BEER_VULKAN_GRAPHICS_MAX_VERTEX_ATTRIBUTES; ++e) {
        const BeerD3D11InputElement *elem = &layout->elements[e];
        uint32_t slot_idx = elem->input_slot;
        if (slot_idx >= BEER_VULKAN_GRAPHICS_MAX_VERTEX_BINDINGS) return 0;
        BeerD3D11Resource *vbuf =
            validated_resource(state->vertex_buffers[slot_idx]);
        if (!vbuf || !vbuf->pixels) return 0;

        /* Resolve D3D11_APPEND_ALIGNED_ELEMENT sentinel. */
        uint32_t byte_offset = elem->aligned_byte_offset;
        uint32_t vk_fmt = 0, bpp = 0;
        if (!d3d11_dxgi_to_vulkan_image(elem->format, &vk_fmt, &bpp)) return 0;
        if (byte_offset == 0xffffffffu) {
            byte_offset = slot_offsets[slot_idx];
        } else {
            /* Explicit offset resets the running tracker for this slot. */
            slot_offsets[slot_idx] = byte_offset;
        }
        slot_offsets[slot_idx] = byte_offset + bpp;

        /* Add vertex binding slot once */
        int found = 0;
        for (uint32_t i = 0; i < vb_count; ++i)
            if (vb[i].stride == state->vertex_strides[slot_idx] &&
                vb[i].data == vbuf->pixels + state->vertex_offsets[slot_idx])
                found = 1;
        if (!found) {
            if (vb_count >= BEER_VULKAN_GRAPHICS_MAX_VERTEX_BINDINGS) return 0;
            vb[vb_count].data   = vbuf->pixels +
                                   state->vertex_offsets[slot_idx];
            vb[vb_count].size   = vbuf->pixel_size -
                                   state->vertex_offsets[slot_idx];
            vb[vb_count].stride = state->vertex_strides[slot_idx];
            ++vb_count;
        }

        va[va_count].location = va_count;
        va[va_count].binding  = slot_idx;
        va[va_count].format   = vk_fmt;
        va[va_count].offset   = byte_offset;
        ++va_count;
    }
    if (!vb_count) return 0;

    /* ---- Index buffer ---- */
    BeerD3D11Resource *ibuf = validated_resource(state->index_buffer);
    if (!ibuf || !ibuf->pixels) return 0;
    size_t idx_byte_offset = (size_t)state->index_offset +
                              (size_t)start_index * 2u;
    if (idx_byte_offset > ibuf->pixel_size) return 0;

    /* ---- Render target ---- */
    BeerD3D11View *rtv = state->render_targets[0];
    BeerD3D11Resource *rt = rtv && com_get_header(rtv)
        ? validated_resource(rtv->resource) : NULL;
    if (!rt || !rt->pixels || rt->desc_size < 20) return 0;
    const uint32_t *rdesc = (const uint32_t *)rt->desc;
    uint32_t rt_vk_fmt = 0, rt_bpp = 0;
    if (!d3d11_dxgi_to_vulkan_image(rdesc[4], &rt_vk_fmt, &rt_bpp)) return 0;

    BeerVulkanGraphicsTarget target_out;
    memset(&target_out, 0, sizeof(target_out));
    target_out.data   = rt->pixels;
    target_out.size   = rt->pixel_size;
    target_out.serial = (uint64_t *)&rt->content_serial;
    target_out.format = rt_vk_fmt;
    target_out.bytes_per_pixel = rt_bpp;
    target_out.width  = rdesc[0];
    target_out.height = rdesc[1];
    target_out.row_pitch = rdesc[0] * rt_bpp; /* tightly packed */

    /* ---- Fixed-function state ---- */
    BeerVulkanGraphicsState gs;
    memset(&gs, 0, sizeof(gs));
    gs.flip_y = 0; /* D3D11 clip Y already matches screen Y */
    BeerD3D11State *blend = (BeerD3D11State *)state->blend_state;
    if (blend && com_get_header(blend) && blend->kind == BEER_STATE_BLEND &&
        blend->desc_size >= 40) {
        const uint8_t *rt0 = blend->desc + 8;
        uint32_t be; memcpy(&be, rt0, 4);
        if (be) {
            /* D3D11 blend factors -> Vulkan (same mapping as D3D12 path) */
            static const int8_t d3d_to_vk[] = {
                -1, 0, 1, 2, 3, 6, 7, 8, 9, 4, 5, 14, -1, -1, 10, 11
            };
            uint32_t sb, db, ba, sba, dba, bba;
            memcpy(&sb,  rt0 + 4,  4); memcpy(&db,  rt0 + 8,  4);
            memcpy(&ba,  rt0 + 12, 4); memcpy(&sba, rt0 + 16, 4);
            memcpy(&dba, rt0 + 20, 4); memcpy(&bba, rt0 + 24, 4);
            if (sb < 16 && db < 16 && sba < 16 && dba < 16 &&
                d3d_to_vk[sb] >= 0 && d3d_to_vk[db] >= 0 &&
                d3d_to_vk[sba] >= 0 && d3d_to_vk[dba] >= 0) {
                gs.blend_enable      = 1;
                gs.src_color_factor  = (uint32_t)d3d_to_vk[sb];
                gs.dst_color_factor  = (uint32_t)d3d_to_vk[db];
                gs.color_op          = (ba >= 1 && ba <= 5) ? ba - 1u : 0u;
                gs.src_alpha_factor  = (uint32_t)d3d_to_vk[sba];
                gs.dst_alpha_factor  = (uint32_t)d3d_to_vk[dba];
                gs.alpha_op          = (bba >= 1 && bba <= 5) ? bba - 1u : 0u;
            } else {
                return 0; /* unsupported blend factors */
            }
        }
        gs.write_mask = rt0[28];
    } else {
        gs.write_mask = 0x0f;
    }
    memcpy(gs.blend_constants, state->blend_factor, sizeof(gs.blend_constants));
    gs.cull_mode = 0; /* NONE — Scaleform UI has no culling */

    /* ---- Draw info ---- */
    BeerVulkanGraphicsDrawInfo di;
    memset(&di, 0, sizeof(di));
    for (uint32_t i = 0; i < vb_count; ++i) di.vertex_bindings[i] = vb[i];
    for (uint32_t i = 0; i < va_count; ++i) di.vertex_attributes[i] = va[i];
    di.vertex_binding_count   = vb_count;
    di.vertex_attribute_count = va_count;
    di.index_data   = ibuf->pixels + idx_byte_offset;
    di.index_size   = ibuf->pixel_size - idx_byte_offset;
    di.index_type   = 0; /* VK_INDEX_TYPE_UINT16 */
    di.topology     = 0; /* VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST */
    di.indexed      = 1;
    di.element_count  = index_count;
    di.instance_count = 1;
    di.base_vertex    = base_vertex;

    float viewport[6] = { 0.0f };
    if (state->viewport_count)
        memcpy(viewport, state->viewports[0], sizeof(float) * 4);
    viewport[4] = 0.0f; viewport[5] = 1.0f; /* depth min/max */

    int32_t scissor[4] = {
        0, 0, (int32_t)target_out.width, (int32_t)target_out.height
    };
    BeerD3D11State *rast = (BeerD3D11State *)state->rasterizer_state;
    if (state->scissor_count && rast && com_get_header(rast) &&
        rast->kind == BEER_STATE_RASTERIZER && rast->desc_size >= 32 &&
        *(const int32_t *)(rast->desc + 28)) {
        scissor[0] = state->scissor_rects[0][0];
        scissor[1] = state->scissor_rects[0][1];
        scissor[2] = state->scissor_rects[0][2];
        scissor[3] = state->scissor_rects[0][3];
    }

    const char *reason = NULL;
    int ok = beer_vulkan_graphics_draw_state(
        vs, (const uint32_t *)tvs->spirv, tvs->spirv_size,
        ps, (const uint32_t *)tps->spirv, tps->spirv_size,
        bindings, binding_count, &di, &gs,
        &target_out, 1, NULL, viewport, scissor, &reason);

    if (ok) {
        resource_mark_written(rt);
        /* Publish the render target so the compositor fullscreen pass can find
         * and blend the Scaleform UI layer into the back buffer.  The manual
         * paths used to set this explicitly; the auto path must do the same. */
        g_last_textured_indexed_target = rt;
        atomic_store(&g_indexed_target_pending, 1);
        static _Atomic(uint32_t) auto_count;
        uint32_t n = atomic_fetch_add(&auto_count, 1) + 1;
        if (n <= 32 || (n % 64) == 0)
            fprintf(stderr,
                    "[D3D11 AUTO] indexed draw #%u VS=%08x PS=%08x "
                    "indices=%u target=%ux%u bindings=%u\n",
                    n, (uint32_t)vs_hash, (uint32_t)ps_hash,
                    index_count, target_out.width, target_out.height,
                    binding_count);
    } else {
        /* Log every unique VS+PS rejection unconditionally. */
        fprintf(stderr,
                "[D3D11 AUTO REJECT] VS=%08x PS=%08x "
                "indices=%u bindings=%u target=%ux%u reason=%s\n",
                (uint32_t)vs_hash, (uint32_t)ps_hash,
                index_count, binding_count,
                target_out.width, target_out.height,
                reason ? reason : "unknown");
    }
    return ok;
}

/* Faithful software execution for the first observed Scaleform solid-color
 * shader pair. The DXBC is structurally simple: VS passes both colors and
 * transforms POSITION.xy with cb0; PS writes COLOR0.rgb and COLOR0.a*COLOR1.a. */
static int execute_initial_indexed_draw(const BeerD3D11PipelineState *state,
                                        uint32_t index_count,
                                        uint32_t start_index,
                                        int32_t base_vertex)
{
    if (!state || !state->vertex_shader || !state->pixel_shader ||
        !state->vertex_buffers[0] || !state->index_buffer ||
        !state->render_targets[0] || state->topology != 4 ||
        state->index_format != 57 || state->viewport_count == 0) {
        fprintf(stderr,
                "[D3D11 VULKAN REJECT] incomplete indexed state state=%p "
                "VS=%p PS=%p VB=%p IB=%p RT=%p topology=%u index-format=%u "
                "viewports=%u indices=%u start=%u base=%d\n",
                (const void *)state,
                state ? state->vertex_shader : NULL,
                state ? state->pixel_shader : NULL,
                state ? state->vertex_buffers[0] : NULL,
                state ? state->index_buffer : NULL,
                state ? state->render_targets[0] : NULL,
                state ? state->topology : 0,
                state ? state->index_format : 0,
                state ? state->viewport_count : 0,
                index_count, start_index, base_vertex);
        return 0;
    }

    return try_auto_translate_indexed_draw(state, index_count,
                                           start_index, base_vertex);
}

/* ---- Dead legacy code deleted ---- */

static void D3D11_MS_ABI context_draw(ID3D11DeviceContext* this, uint32_t VertexCount, uint32_t StartVertexLocation) {
    BeerD3D11Command command = { .type = BEER_COMMAND_DRAW };
    command.args.integers.a = VertexCount;
    command.args.integers.b = StartVertexLocation;
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (context && context->type == 1)
        command.draw_state = pipeline_state_clone(&context->state);
    if (context_record_command(this, &command)) return;
    trace_draw_snapshot(context ? &context->state : NULL,
                        VertexCount, StartVertexLocation);
    int executed = execute_initial_fullscreen_draw(
        context ? &context->state : NULL, VertexCount, StartVertexLocation);
    if (!executed) {
        const BeerD3D11PipelineState *state = context ? &context->state : NULL;
        BeerD3D11Shader *vs = state ? state->vertex_shader : NULL;
        BeerD3D11Shader *ps = state ? state->pixel_shader : NULL;
        uint32_t vertex_hash = com_get_header(vs)
            ? fnv1a_bytes(vs->bytecode, vs->bytecode_size) : 0;
        uint32_t pixel_hash = com_get_header(ps)
            ? fnv1a_bytes(ps->bytecode, ps->bytecode_size) : 0;
        fprintf(stderr,
                "[D3D11 VULKAN REJECT] Draw vertices=%u start=%u topology=%u "
                "stride=%u viewports=%u VS=%08x PS=%08x VB=%p RT=%p\n",
                VertexCount, StartVertexLocation, state ? state->topology : 0,
                state ? state->vertex_strides[0] : 0,
                state ? state->viewport_count : 0, vertex_hash, pixel_hash,
                state ? state->vertex_buffers[0] : NULL,
                state ? state->render_targets[0] : NULL);
        strict_vulkan_failure("unsupported Draw state");
    }
    static _Atomic(uint32_t) calls;
    uint32_t call = atomic_fetch_add(&calls, 1) + 1;
    if (render_diagnostics_enabled() && (call <= 8 || (call % 64) == 0))
        fprintf(stderr, "[D3D11 TRACE] Draw #%u vertices=%u start=%u executed=%d\n",
                call, VertexCount, StartVertexLocation, executed);
    (void)this;
}

static void D3D11_MS_ABI context_draw_indexed_instanced(
    ID3D11DeviceContext *this, uint32_t index_count, uint32_t instance_count,
    uint32_t start_index, int32_t base_vertex, uint32_t start_instance)
{
    BeerD3D11Command command = { .type = BEER_COMMAND_DRAW_INDEXED_INSTANCED };
    command.args.integers.a = index_count;
    command.args.integers.b = instance_count;
    command.args.integers.c = start_index;
    command.args.integers.d = start_instance;
    command.args.integers.signed_value = base_vertex;
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (context && context->type == 1)
        command.draw_state = pipeline_state_clone(&context->state);
    if (context_record_command(this, &command)) return;
    static _Atomic(uint32_t) calls;
    uint32_t call = atomic_fetch_add(&calls, 1) + 1;
    if (call <= 16)
        fprintf(stderr, "[D3D11 TRACE] DrawIndexedInstanced #%u indices=%u instances=%u start=%u base=%d firstInstance=%u\n",
                call, index_count, instance_count, start_index, base_vertex, start_instance);
    (void)this;
}

static void D3D11_MS_ABI context_draw_instanced(
    ID3D11DeviceContext *this, uint32_t vertex_count, uint32_t instance_count,
    uint32_t start_vertex, uint32_t start_instance)
{
    BeerD3D11Command command = { .type = BEER_COMMAND_DRAW_INSTANCED };
    command.args.integers.a = vertex_count;
    command.args.integers.b = instance_count;
    command.args.integers.c = start_vertex;
    command.args.integers.d = start_instance;
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (context && context->type == 1)
        command.draw_state = pipeline_state_clone(&context->state);
    if (context_record_command(this, &command)) return;
    static _Atomic(uint32_t) calls;
    uint32_t call = atomic_fetch_add(&calls, 1) + 1;
    if (call <= 16)
        fprintf(stderr, "[D3D11 TRACE] DrawInstanced #%u vertices=%u instances=%u start=%u firstInstance=%u\n",
                call, vertex_count, instance_count, start_vertex, start_instance);
    (void)this;
}

static void D3D11_MS_ABI context_draw_auto(ID3D11DeviceContext *this)
{
    BeerD3D11Command command = { .type = BEER_COMMAND_DRAW_AUTO };
    if (context_record_command(this, &command)) return;
    static _Atomic(uint32_t) calls;
    uint32_t call = atomic_fetch_add(&calls, 1) + 1;
    if (call <= 16) fprintf(stderr, "[D3D11 TRACE] DrawAuto #%u\n", call);
    (void)this;
}

static void D3D11_MS_ABI context_draw_indirect(
    ID3D11DeviceContext *this, void *buffer, uint32_t offset)
{
    static _Atomic(uint32_t) calls;
    uint32_t call = atomic_fetch_add(&calls, 1) + 1;
    if (call <= 16)
        fprintf(stderr, "[D3D11 TRACE] DrawIndirect #%u buffer=%p offset=%u\n",
                call, buffer, offset);
    (void)this;
}

static void D3D11_MS_ABI context_begin(
    ID3D11DeviceContext *this, BeerD3D11Query *query)
{
    if (!query || !com_get_header(query)) return;
    BeerD3D11Command command = {
        .type = BEER_COMMAND_BEGIN_QUERY,
        .object = query
    };
    if (context_record_command(this, &command)) return;
    /* Timestamp and event queries are End-only in D3D11. Begin is valid for
     * interval queries such as occlusion and timestamp-disjoint. */
    if (query->query == 0 || query->query == 2) return;
    query->begun = 1;
    query->ended = 0;
}

static void D3D11_MS_ABI context_end(
    ID3D11DeviceContext *this, BeerD3D11Query *query)
{
    if (!query || !com_get_header(query)) return;
    BeerD3D11Command command = {
        .type = BEER_COMMAND_END_QUERY,
        .object = query
    };
    if (context_record_command(this, &command)) return;
    query->ended = 1;
    if (query->query == 2) { /* D3D11_QUERY_TIMESTAMP */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        query->value = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
    } else {
        query->value = 1;
    }
}

static HRESULT D3D11_MS_ABI context_get_data(
    ID3D11DeviceContext *this, BeerD3D11Query *query,
    void *data, uint32_t data_size, uint32_t flags)
{
    (void)this;
    (void)flags;
    if (!query || !com_get_header(query)) return (HRESULT)0x80070057;
    if (!query->ended) return 1; /* S_FALSE */
    if (!data || !data_size) return S_OK;

    if (query->query == 3) { /* D3D11_QUERY_TIMESTAMP_DISJOINT */
        struct {
            uint64_t frequency;
            uint32_t disjoint;
            uint32_t padding;
        } result = { 1000000000ULL, 0, 0 };
        if (data_size < sizeof(result)) return (HRESULT)0x80070057;
        memcpy(data, &result, sizeof(result));
    } else {
        uint32_t expected = query_get_data_size(query);
        if (data_size < expected) return (HRESULT)0x80070057;
        memset(data, 0, expected);
        memcpy(data, &query->value,
               expected < sizeof(query->value) ? expected : sizeof(query->value));
    }
    return S_OK;
}

static void D3D11_MS_ABI context_om_set_render_targets(ID3D11DeviceContext* this, uint32_t NumViews, void* ppRenderTargetViews, void* pDepthStencilView) {
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (!context) return;
    set_state_objects(context->state.render_targets, BEER_MAX_RENDER_TARGETS,
                      0, NumViews, ppRenderTargetViews);
    for (uint32_t i = NumViews; i < BEER_MAX_RENDER_TARGETS; ++i)
        state_replace_object(&context->state.render_targets[i], NULL);
    state_replace_object(&context->state.depth_stencil_view, pDepthStencilView);
}

static void D3D11_MS_ABI context_om_get_render_targets(
    ID3D11DeviceContext *this, uint32_t NumViews,
    void **ppRenderTargetViews, void **ppDepthStencilView)
{
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (ppRenderTargetViews) {
        for (uint32_t i = 0; i < NumViews; ++i) {
            void *view = context && i < BEER_MAX_RENDER_TARGETS
                ? context->state.render_targets[i] : NULL;
            ppRenderTargetViews[i] = view;
            if (view && com_get_header(view)) com_addref(view);
        }
    }
    if (ppDepthStencilView) {
        void *view = context ? context->state.depth_stencil_view : NULL;
        *ppDepthStencilView = view;
        if (view && com_get_header(view)) com_addref(view);
    }
    static _Atomic(uint32_t) calls;
    uint32_t call = atomic_fetch_add(&calls, 1) + 1;
    if (call <= 16)
        fprintf(stderr,
                "[D3D11 STATE] OMGetRenderTargets #%u context=%p count=%u rtv0=%p dsv=%p\n",
                call, (void *)this, NumViews,
                ppRenderTargetViews && NumViews ? ppRenderTargetViews[0] : NULL,
                ppDepthStencilView ? *ppDepthStencilView : NULL);
}

static void D3D11_MS_ABI context_om_set_blend_state(ID3D11DeviceContext* this, void* pBlendState, void* BlendFactor, uint32_t SampleMask) {
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (!context) return;
    state_replace_object(&context->state.blend_state, pBlendState);
    if (BlendFactor) memcpy(context->state.blend_factor, BlendFactor,
                            sizeof(context->state.blend_factor));
    else memset(context->state.blend_factor, 0, sizeof(context->state.blend_factor));
    context->state.sample_mask = SampleMask;
}

static void D3D11_MS_ABI context_om_set_depthstencil_state(ID3D11DeviceContext* this, void* pDepthStencilState, uint32_t StencilRef) {
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (!context) return;
    state_replace_object(&context->state.depth_stencil_state, pDepthStencilState);
    context->state.stencil_ref = StencilRef;
}

static void D3D11_MS_ABI context_rs_set_state(ID3D11DeviceContext* this, void* pRasterizerState) {
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (context) state_replace_object(&context->state.rasterizer_state, pRasterizerState);
}

static void D3D11_MS_ABI context_rs_set_viewports(ID3D11DeviceContext* this, uint32_t NumViewports, void* pViewports) {
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (!context) return;
    uint32_t count = NumViewports < BEER_MAX_VIEWPORTS ? NumViewports : BEER_MAX_VIEWPORTS;
    context->state.viewport_count = count;
    if (count && pViewports)
        memcpy(context->state.viewports, pViewports,
               (size_t)count * sizeof(context->state.viewports[0]));
    static _Atomic(uint32_t) calls;
    uint32_t call = atomic_fetch_add(&calls, 1) + 1;
    if (call <= 16 && count) {
        uint32_t bits[6];
        memcpy(bits, context->state.viewports[0], sizeof(bits));
        fprintf(stderr, "[D3D11 STATE] RSSetViewports #%u context=%p count=%u first-bits=(%08x,%08x %08xx%08x %08x..%08x)\n",
                call, (void *)this, NumViewports,
                bits[0], bits[1], bits[2], bits[3], bits[4], bits[5]);
    }
}

static void D3D11_MS_ABI context_rs_set_scissor_rects(ID3D11DeviceContext* this, uint32_t NumRects, void* pRects) {
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (!context) return;
    uint32_t count = NumRects < BEER_MAX_VIEWPORTS ? NumRects : BEER_MAX_VIEWPORTS;
    context->state.scissor_count = count;
    if (count && pRects)
        memcpy(context->state.scissor_rects, pRects,
               (size_t)count * sizeof(context->state.scissor_rects[0]));
}

static uint8_t float_to_unorm8(float value)
{
    if (!(value > 0.0f)) return 0;
    if (value >= 1.0f) return 255;
    return (uint8_t)(value * 255.0f + 0.5f);
}

static void D3D11_MS_ABI context_clear_rendertarget_view(ID3D11DeviceContext* this, void* pRenderTargetView, void* ColorRGBA);

static uint32_t pack_r10g10b10a2(const float *color)
{
    uint32_t r = (uint32_t)(color[0] <= 0.0f ? 0 : color[0] >= 1.0f ? 1023 : color[0] * 1023.0f + 0.5f);
    uint32_t g = (uint32_t)(color[1] <= 0.0f ? 0 : color[1] >= 1.0f ? 1023 : color[1] * 1023.0f + 0.5f);
    uint32_t b = (uint32_t)(color[2] <= 0.0f ? 0 : color[2] >= 1.0f ? 1023 : color[2] * 1023.0f + 0.5f);
    uint32_t a = (uint32_t)(color[3] <= 0.0f ? 0 : color[3] >= 1.0f ? 3 : color[3] * 3.0f + 0.5f);
    return r | (g << 10) | (b << 20) | (a << 30);
}

static void D3D11_MS_ABI context_clear_rendertarget_view(ID3D11DeviceContext* this, void* pRenderTargetView, void* ColorRGBA) {
    BeerD3D11Command command = { .type = BEER_COMMAND_CLEAR_RENDER_TARGET,
                                 .object = pRenderTargetView };
    if (ColorRGBA) memcpy(command.args.color, ColorRGBA, sizeof(command.args.color));
    if (context_record_command(this, &command)) return;
    static _Atomic(uint32_t) calls;
    uint32_t call = atomic_fetch_add(&calls, 1) + 1;
    if (call <= 8) {
        const float *color = (const float *)ColorRGBA;
        fprintf(stderr, "[D3D11 TRACE] ClearRenderTargetView #%u view=%p rgba=(%.3f,%.3f,%.3f,%.3f)\n",
                call, pRenderTargetView,
                color ? color[0] : 0.0f, color ? color[1] : 0.0f,
                color ? color[2] : 0.0f, color ? color[3] : 0.0f);
    }
    BeerD3D11View *view = pRenderTargetView;
    BeerD3D11Resource *resource = view && com_get_header(view) &&
        view->kind == BEER_VIEW_RENDER_TARGET ? view->resource : NULL;
    const float *color = ColorRGBA;
    if (resource && com_get_header(resource) && resource->pixels && color) {
        /* A clear is a CPU-side replacement of the complete resource. Forget
         * any older GPU mirror before advancing the serial, otherwise a later
         * eviction/writeback can resurrect pre-clear UI pixels.
         *
         * Hold the GPU ordering lock so the clear cannot land between another
         * job's serial allocation and its submission; that interleaving made
         * the cleared frame and the following UI layers disagree about which
         * copy of the target was authoritative. */
        pthread_mutex_lock(&g_gpu_target_order_lock);
        vulkan_indexed_renderer_forget_resource(resource);
        vulkan_renderer_forget_resource(resource);
        resource_mark_written(resource);
        uint32_t format = ((const uint32_t *)resource->desc)[4];
        if (format == 24) {
            uint32_t packed = pack_r10g10b10a2(color);
            for (size_t offset = 0; offset < resource->pixel_size; offset += 4)
                memcpy(resource->pixels + offset, &packed, sizeof(packed));
        } else if (format == 28 || format == 29) {
            uint8_t rgba[4] = {
                float_to_unorm8(color[0]), float_to_unorm8(color[1]),
                float_to_unorm8(color[2]), float_to_unorm8(color[3])
            };
            for (size_t offset = 0; offset < resource->pixel_size; offset += 4)
                memcpy(resource->pixels + offset, rgba, sizeof(rgba));
        }
        pthread_mutex_unlock(&g_gpu_target_order_lock);
    }
    (void)this;
}

static BeerD3D11Resource *validated_resource(void *object)
{
    ComObjectHeader *header = com_get_header(object);
    return header && header->type == COM_TYPE_RESOURCE ? object : NULL;
}

static void trace_context_operation(const char *name, void *destination, void *source)
{
    static _Atomic(uint32_t) calls;
    uint32_t call = atomic_fetch_add(&calls, 1) + 1;
    if (call <= 24)
        fprintf(stderr, "[D3D11 TRACE] %s #%u destination=%p source=%p\n",
                name, call, destination, source);
}

static void D3D11_MS_ABI context_copy_resource(
    ID3D11DeviceContext *this, void *destination, void *source)
{
    trace_context_operation("CopyResource", destination, source);
    BeerD3D11Resource *dst = validated_resource(destination);
    BeerD3D11Resource *src = validated_resource(source);
    if (!dst || !src) return;
    BeerD3D11Command command = { .type = BEER_COMMAND_COPY_RESOURCE,
                                 .object = destination,
                                 .second_object = source };
    if (context_record_command(this, &command)) return;
    if (dst->pixels && src->pixels) {
        resource_sync_from_vulkan(src);
        memcpy(dst->pixels, src->pixels,
               dst->pixel_size < src->pixel_size ? dst->pixel_size : src->pixel_size);
        resource_mark_written(dst);
        atomic_store(&dst->content_serial, atomic_load(&src->content_serial));
    }
}

static void apply_update_subresource(
    void *destination, uint32_t subresource, const void *box, const void *source,
    uint32_t source_row_pitch, uint32_t source_depth_pitch)
{
    BeerD3D11Resource *dst = validated_resource(destination);
    if (!dst || !dst->pixels || !source) return;
    resource_mark_written(dst);
    if (dst->dimension == 1) {
        if (subresource != 0) return;
        /* D3D11 buffer boxes address bytes through left/right. Beer used to
         * ignore the box and copy the entire allocation, over-reading partial
         * Scaleform updates and replacing unrelated index/vertex ranges. */
        size_t left = 0, right = dst->pixel_size;
        if (box) {
            const uint32_t *region = box;
            left = region[0];
            right = region[3];
            if (region[1] != 0 || region[2] != 0 || region[4] != 1 ||
                region[5] != 1 || left > right || right > dst->pixel_size)
                return;
        }
        memcpy(dst->pixels + left, source, right - left);
        return;
    }
    const uint32_t *desc = (const uint32_t *)dst->desc;
    if (dst->dimension == 3 && dst->subresource_count) {
        if (subresource >= dst->subresource_count) return;
        /* NV12 plane updates: plane 0 is the Y plane (width*height bytes),
         * plane 1 is the interleaved UV plane (width*(height/2) bytes).
         * RE8 uploads each plane via UpdateSubresource with subresource 0 or 1.
         * The whole NV12 buffer is stored contiguously: UV follows Y. */
        if (desc[4] == 103 /* NV12 */ && dst->pixels) {
            uint32_t nv12_width = desc[0], nv12_height = desc[1];
            size_t y_size = (size_t)nv12_width * nv12_height;
            size_t uv_size = (size_t)nv12_width * ((nv12_height + 1u) / 2u);
            if (subresource == 0 && y_size <= dst->pixel_size) {
                /* Y plane */
                uint32_t pitch = source_row_pitch ? source_row_pitch : nv12_width;
                for (uint32_t row = 0; row < nv12_height; ++row) {
                    size_t dst_off = (size_t)row * nv12_width;
                    size_t src_off = (size_t)row * pitch;
                    size_t copy = pitch < nv12_width ? pitch : nv12_width;
                    if (dst_off + copy <= y_size)
                        memcpy(dst->pixels + dst_off,
                               (const uint8_t *)source + src_off, copy);
                }
                resource_mark_written(dst);
                return;
            } else if (subresource == 1 && y_size + uv_size <= dst->pixel_size) {
                /* UV plane */
                uint32_t uv_rows = (nv12_height + 1u) / 2u;
                uint32_t pitch = source_row_pitch ? source_row_pitch : nv12_width;
                for (uint32_t row = 0; row < uv_rows; ++row) {
                    size_t dst_off = y_size + (size_t)row * nv12_width;
                    size_t src_off = (size_t)row * pitch;
                    size_t copy = pitch < nv12_width ? pitch : nv12_width;
                    if (dst_off + copy <= y_size + uv_size)
                        memcpy(dst->pixels + dst_off,
                               (const uint8_t *)source + src_off, copy);
                }
                resource_mark_written(dst);
                return;
            }
            /* Fallthrough: whole-buffer NV12 upload without subresource split */
            if (subresource == 0 && !box) {
                size_t total = y_size + uv_size;
                if (total <= dst->pixel_size) {
                    memcpy(dst->pixels, source, total);
                    resource_mark_written(dst);
                }
                return;
            }
        }
        uint32_t block_bytes = texture_format_block_bytes(desc[4]);
        if (block_bytes && box) {
            /* Compressed update boxes are specified in texels but copied in
             * whole 4x4 blocks. The title path currently initializes BC mip
             * chains at creation; reject partial BC writes until observed. */
            return;
        }
        if (!box) {
            uint32_t destination_pitch = dst->subresource_row_pitches[subresource];
            uint32_t pitch = source_row_pitch ? source_row_pitch : destination_pitch;
            size_t copy_pitch = pitch < destination_pitch ? pitch : destination_pitch;
            for (uint32_t row = 0; row < dst->subresource_rows[subresource]; ++row)
                memcpy(dst->pixels + dst->subresource_offsets[subresource] +
                           (size_t)row * destination_pitch,
                       (const uint8_t *)source + (size_t)row * pitch,
                       copy_pitch);
            return;
        }
    }
    uint32_t mip_levels = desc[2];
    uint32_t array_size = dst->dimension == 3 ? desc[3] : 1;
    if (mip_levels != 1 || subresource >= array_size) return;

    uint32_t left = 0, top = 0, front = 0;
    uint32_t right = desc[0], bottom = desc[1], back = 1;
    if (box) {
        const uint32_t *region = box;
        left = region[0]; top = region[1]; front = region[2];
        right = region[3]; bottom = region[4]; back = region[5];
    }
    if (left > right || top > bottom || right > desc[0] || bottom > desc[1] ||
        front != 0 || back != 1)
        return;
    uint32_t bytes_per_pixel = texture_format_bytes_per_pixel(desc[4]);
    if (!bytes_per_pixel || left > UINT32_MAX / bytes_per_pixel) return;
    size_t destination_slice_pitch = (size_t)dst->row_pitch * desc[1];
    size_t destination_offset = (size_t)subresource * destination_slice_pitch +
                                (size_t)top * dst->row_pitch +
                                (size_t)left * bytes_per_pixel;
    size_t copy_pitch = (size_t)(right - left) * bytes_per_pixel;
    uint32_t pitch = source_row_pitch ? source_row_pitch : (uint32_t)copy_pitch;
    if (copy_pitch > pitch) copy_pitch = pitch;
    size_t rows = bottom - top;
    for (size_t y = 0; y < rows; ++y)
        memcpy(dst->pixels + destination_offset + y * dst->row_pitch,
               (const uint8_t *)source + y * pitch, copy_pitch);
    (void)source_depth_pitch;
}

static void D3D11_MS_ABI context_update_subresource(
    ID3D11DeviceContext *this, void *destination, uint32_t subresource,
    const void *box, const void *source, uint32_t source_row_pitch,
    uint32_t source_depth_pitch)
{
    trace_context_operation("UpdateSubresource", destination, (void *)source);
    BeerD3D11Resource *dst = validated_resource(destination);
    if (!dst || !dst->pixels || !source) return;

    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (context && context->type == 1) {
        size_t source_size;
        if (dst->dimension == 1) {
            if (box) {
                const uint32_t *region = box;
                if (region[0] > region[3] || region[3] > dst->pixel_size ||
                    region[1] != 0 || region[2] != 0 || region[4] != 1 ||
                    region[5] != 1)
                    return;
                source_size = (size_t)region[3] - region[0];
            } else {
                source_size = dst->pixel_size;
            }
        } else if (dst->dimension == 3 &&
                   subresource < dst->subresource_count) {
            uint32_t pitch = source_row_pitch ? source_row_pitch
                : dst->subresource_row_pitches[subresource];
            source_size = source_depth_pitch ? source_depth_pitch
                : (size_t)pitch * dst->subresource_rows[subresource];
        } else {
            source_size = source_depth_pitch ? source_depth_pitch
                : (size_t)source_row_pitch * ((const uint32_t *)dst->desc)[1];
        }
        BeerD3D11Command command = {
            .type = BEER_COMMAND_UPDATE_SUBRESOURCE,
            .object = destination
        };
        command.args.integers.a = subresource;
        command.args.integers.b = source_row_pitch;
        command.args.integers.c = source_depth_pitch;
        command.args.integers.d = box ? 1u : 0u;
        command.owned_data_size = source_size + (box ? 24u : 0u);
        command.owned_data = malloc(command.owned_data_size);
        if (!command.owned_data) return;
        if (box) memcpy(command.owned_data, box, 24);
        memcpy((uint8_t *)command.owned_data + (box ? 24u : 0u), source, source_size);
        if (context_record_command(this, &command)) return;
        free(command.owned_data);
        return;
    }

    apply_update_subresource(destination, subresource, box, source,
                             source_row_pitch, source_depth_pitch);
}

static void D3D11_MS_ABI context_copy_subresource_region(
    ID3D11DeviceContext *this, void *destination, uint32_t destination_subresource,
    uint32_t destination_x, uint32_t destination_y, uint32_t destination_z,
    void *source, uint32_t source_subresource, const void *source_box)
{
    (void)destination_subresource; (void)destination_x; (void)destination_y;
    (void)destination_z; (void)source_subresource; (void)source_box;
    trace_context_operation("CopySubresourceRegion", destination, source);
    context_copy_resource(this, destination, source);
}

static void D3D11_MS_ABI context_resolve_subresource(
    ID3D11DeviceContext *this, void *destination, uint32_t destination_subresource,
    void *source, uint32_t source_subresource, uint32_t format)
{
    (void)destination_subresource; (void)source_subresource; (void)format;
    trace_context_operation("ResolveSubresource", destination, source);
    context_copy_resource(this, destination, source);
}

static void trace_shader_stage(const char *stage, ID3D11DeviceContext *this,
                               void *shader, uint32_t class_count)
{
    static _Atomic(uint32_t) calls;
    uint32_t call = atomic_fetch_add(&calls, 1) + 1;
    if (call <= 32) {
        uint32_t hash = 0;
        BeerD3D11Shader *typed = shader;
        if (typed && com_get_header(typed))
            hash = fnv1a_bytes(typed->bytecode, typed->bytecode_size);
        fprintf(stderr,
                "[D3D11 STAGE] %s #%u context=%p type=%u shader=%p hash=%08x classes=%u\n",
                stage, call, (void *)this,
                this ? ((BeerD3D11DeviceContext *)this)->type : 0, shader, hash,
                class_count);
    }
}

static void D3D11_MS_ABI context_hs_set_shader_resources(
    ID3D11DeviceContext *this, uint32_t start_slot, uint32_t count, void *views)
{
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (context) set_state_objects(context->state.hs_shader_resources,
        BEER_MAX_SHADER_RESOURCES, start_slot, count, views);
    trace_resource_binding("HSSetShaderResources", this, start_slot, count,
                           (void *const *)views, NULL, NULL);
}

static void D3D11_MS_ABI context_hs_set_shader(
    ID3D11DeviceContext *this, void *shader, void *class_instances,
    uint32_t class_count)
{
    (void)class_instances;
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (context) state_replace_object(&context->state.hull_shader, shader);
    trace_shader_stage("HSSetShader", this, shader, class_count);
}

static void D3D11_MS_ABI context_hs_set_samplers(
    ID3D11DeviceContext *this, uint32_t start_slot, uint32_t count, void *samplers)
{
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (context) set_state_objects(context->state.hs_samplers,
        BEER_MAX_SAMPLERS, start_slot, count, samplers);
    trace_resource_binding("HSSetSamplers", this, start_slot, count,
                           (void *const *)samplers, NULL, NULL);
}

static void D3D11_MS_ABI context_hs_set_constant_buffers(
    ID3D11DeviceContext *this, uint32_t start_slot, uint32_t count, void *buffers)
{
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (context) set_state_objects(context->state.hs_constant_buffers,
        BEER_MAX_CONSTANT_BUFFERS, start_slot, count, buffers);
    trace_resource_binding("HSSetConstantBuffers", this, start_slot, count,
                           (void *const *)buffers, NULL, NULL);
}

static void D3D11_MS_ABI context_ds_set_shader_resources(
    ID3D11DeviceContext *this, uint32_t start_slot, uint32_t count, void *views)
{
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (context) set_state_objects(context->state.ds_shader_resources,
        BEER_MAX_SHADER_RESOURCES, start_slot, count, views);
    trace_resource_binding("DSSetShaderResources", this, start_slot, count,
                           (void *const *)views, NULL, NULL);
}

static void D3D11_MS_ABI context_ds_set_shader(
    ID3D11DeviceContext *this, void *shader, void *class_instances,
    uint32_t class_count)
{
    (void)class_instances;
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (context) state_replace_object(&context->state.domain_shader, shader);
    trace_shader_stage("DSSetShader", this, shader, class_count);
}

static void D3D11_MS_ABI context_ds_set_samplers(
    ID3D11DeviceContext *this, uint32_t start_slot, uint32_t count, void *samplers)
{
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (context) set_state_objects(context->state.ds_samplers,
        BEER_MAX_SAMPLERS, start_slot, count, samplers);
    trace_resource_binding("DSSetSamplers", this, start_slot, count,
                           (void *const *)samplers, NULL, NULL);
}

static void D3D11_MS_ABI context_ds_set_constant_buffers(
    ID3D11DeviceContext *this, uint32_t start_slot, uint32_t count, void *buffers)
{
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (context) set_state_objects(context->state.ds_constant_buffers,
        BEER_MAX_CONSTANT_BUFFERS, start_slot, count, buffers);
    trace_resource_binding("DSSetConstantBuffers", this, start_slot, count,
                           (void *const *)buffers, NULL, NULL);
}

static void D3D11_MS_ABI context_cs_set_shader_resources(
    ID3D11DeviceContext *this, uint32_t start_slot, uint32_t count, void *views)
{
    trace_resource_binding("CSSetShaderResources", this, start_slot, count,
                           (void *const *)views, NULL, NULL);
}

static void D3D11_MS_ABI context_cs_set_unordered_access_views(
    ID3D11DeviceContext *this, uint32_t start_slot, uint32_t count, void *views,
    const uint32_t *initial_counts)
{
    trace_resource_binding("CSSetUnorderedAccessViews", this, start_slot, count,
                           (void *const *)views, initial_counts, NULL);
}

static void D3D11_MS_ABI context_cs_set_shader(
    ID3D11DeviceContext *this, void *shader, void *class_instances,
    uint32_t class_count)
{
    (void)class_instances;
    trace_shader_stage("CSSetShader", this, shader, class_count);
}

static void D3D11_MS_ABI context_cs_set_samplers(
    ID3D11DeviceContext *this, uint32_t start_slot, uint32_t count, void *samplers)
{
    trace_resource_binding("CSSetSamplers", this, start_slot, count,
                           (void *const *)samplers, NULL, NULL);
}

static void D3D11_MS_ABI context_cs_set_constant_buffers(
    ID3D11DeviceContext *this, uint32_t start_slot, uint32_t count, void *buffers)
{
    trace_resource_binding("CSSetConstantBuffers", this, start_slot, count,
                           (void *const *)buffers, NULL, NULL);
}

static void D3D11_MS_ABI context_dispatch(
    ID3D11DeviceContext *this, uint32_t x, uint32_t y, uint32_t z)
{
    static _Atomic(uint32_t) calls;
    uint32_t call = atomic_fetch_add(&calls, 1) + 1;
    if (call <= 16)
        fprintf(stderr, "[D3D11 TRACE] Dispatch #%u context=%p type=%u groups=(%u,%u,%u)\n",
                call, (void *)this,
                this ? ((BeerD3D11DeviceContext *)this)->type : 0, x, y, z);
}

static void D3D11_MS_ABI context_dispatch_indirect(
    ID3D11DeviceContext *this, void *buffer, uint32_t offset)
{
    static _Atomic(uint32_t) calls;
    uint32_t call = atomic_fetch_add(&calls, 1) + 1;
    if (call <= 16)
        fprintf(stderr, "[D3D11 TRACE] DispatchIndirect #%u buffer=%p offset=%u\n",
                call, buffer, offset);
    (void)this;
}

/* ID3D11CommandList inherits ID3D11DeviceChild (slots 0..6) and adds
 * GetContextFlags at slot 7. */
static void *g_command_list_vtable[8];

static HRESULT D3D11_MS_ABI command_list_query_interface(
    BeerD3D11CommandList *object, REFIID riid, void **output)
{
    static const uint8_t iid_iunknown[16] = {
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xc0,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x46
    };
    /* IID_ID3D11CommandList = A24BC4D1-769E-43F7-8013-98FF566C18E2. */
    static const uint8_t iid_command_list[16] = {
        0xd1,0xc4,0x4b,0xa2,0x9e,0x76,0xf7,0x43,
        0x80,0x13,0x98,0xff,0x56,0x6c,0x18,0xe2
    };
    if (!output) return (HRESULT)0x80070057;
    *output = NULL;
    if (!iid_equal(riid, iid_iunknown) && !iid_equal(riid, iid_command_list))
        return E_NOINTERFACE;
    *output = object;
    com_addref(object);
    return S_OK;
}

static uint32_t D3D11_MS_ABI command_list_release(
    BeerD3D11CommandList *object);

static void D3D11_MS_ABI command_list_get_device(
    BeerD3D11CommandList *object, ID3D11Device **device)
{
    if (!device) return;
    *device = object ? object->device : NULL;
    if (*device) device_addref(*device);
}

static uint32_t D3D11_MS_ABI command_list_get_context_flags(
    BeerD3D11CommandList *object)
{
    return object ? object->context_flags : 0;
}

static _Atomic(uint32_t) g_finalized_indexed_command_lists;
static _Atomic(uint32_t) g_executed_indexed_command_lists;
static _Atomic(uint32_t) g_released_indexed_command_lists;
/* ID3D11DeviceContext immediate-state mutation is ordered. Sekiro can finish
 * deferred lists on many workers, but replay into the one immediate context
 * must remain atomic: interleaving setters from separate lists mixes IA,
 * shader, resource and render-target state and produces unstable UI frames. */
static pthread_mutex_t g_immediate_context_replay_lock = PTHREAD_MUTEX_INITIALIZER;

static void command_list_vtable_initialize(void)
{
    if (g_command_list_vtable[0]) return;
    g_command_list_vtable[0] = (void *)command_list_query_interface;
    g_command_list_vtable[1] = (void *)com_addref;
    g_command_list_vtable[2] = (void *)command_list_release;
    g_command_list_vtable[3] = (void *)command_list_get_device;
    g_command_list_vtable[4] = (void *)resource_private_data_unsupported;
    g_command_list_vtable[5] = (void *)resource_private_data_unsupported;
    g_command_list_vtable[6] = (void *)resource_private_data_unsupported;
    g_command_list_vtable[7] = (void *)command_list_get_context_flags;
}

static void D3D11_MS_ABI context_execute_command_list(
    ID3D11DeviceContext *this, BeerD3D11CommandList *command_list,
    int restore_state)
{
    static _Atomic(uint32_t) calls;
    uint32_t call = atomic_fetch_add(&calls, 1) + 1;
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (!context || context->type != 0 || !command_list ||
        !com_get_header(command_list))
        return;
    pthread_mutex_lock(&g_immediate_context_replay_lock);
    if (render_diagnostics_enabled() && (call <= 16 || (call % 256) == 0))
        fprintf(stderr, "[D3D11 TRACE] ExecuteCommandList #%u list=%p restore=%d commands=%zu\n",
                call, (void *)command_list, restore_state,
                command_list->command_count);
    if (command_list->indexed_draw_count) {
        uint32_t indexed_execute =
            atomic_fetch_add(&g_executed_indexed_command_lists, 1) + 1;
        if (indexed_execute <= 8 || (indexed_execute % 128) == 0)
            fprintf(stderr,
                    "[D3D11 TRACE] executing indexed list=%p sequence=%u "
                    "finalized-sequence=%u indexed-sequence=%u draws=%u commands=%zu\n",
                    (void *)command_list, call, command_list->finalize_sequence,
                    indexed_execute, command_list->indexed_draw_count,
                    command_list->command_count);
    }
    atomic_store(&command_list->executed, 1);

    /* ExecuteCommandList has explicit immediate-context state semantics:
     * TRUE restores the state present before execution, while FALSE resets the
     * immediate context to defaults afterward.  Retain a real snapshot for the
     * TRUE case because replayed setters release/replace COM references. */
    BeerD3D11PipelineState *saved_state = restore_state
        ? pipeline_state_clone(&context->state) : NULL;
    for (size_t i = 0; i < command_list->command_count; ++i) {
        BeerD3D11Command *command = &command_list->commands[i];
        switch (command->type) {
        case BEER_COMMAND_IA_INPUT_LAYOUT:
            context_ia_set_input_layout(this, command->object);
            break;
        case BEER_COMMAND_IA_VERTEX_BUFFERS: {
            uint32_t count = command->args.integers.b;
            size_t pointer_bytes = (size_t)count * sizeof(void *);
            void **buffers = command->owned_data;
            uint32_t *strides = (uint32_t *)((uint8_t *)command->owned_data +
                                             pointer_bytes);
            uint32_t *offsets = strides + count;
            context_ia_set_vertex_buffers(this, command->args.integers.a, count,
                                          buffers, strides, offsets);
            break;
        }
        case BEER_COMMAND_IA_INDEX_BUFFER:
            context_ia_set_index_buffer(this, command->object,
                                        command->args.integers.a,
                                        command->args.integers.b);
            break;
        case BEER_COMMAND_IA_TOPOLOGY:
            context_ia_set_primitive_topology(this, command->args.integers.a);
            break;
        case BEER_COMMAND_VS_SHADER:
            context_vs_set_shader(this, command->object, NULL,
                                  command->args.integers.a);
            break;
        case BEER_COMMAND_PS_SHADER:
            context_ps_set_shader(this, command->object, NULL,
                                  command->args.integers.a);
            break;
        case BEER_COMMAND_GS_SHADER:
            context_gs_set_shader(this, command->object, NULL,
                                  command->args.integers.a);
            break;
        case BEER_COMMAND_DRAW_INDEXED: {
            BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
            BeerD3D11PipelineState saved;
            int has_snapshot = command->draw_state != NULL;
            BeerD3D11PipelineState merged;
            if (has_snapshot) {
                saved = context->state;
                merged = *command->draw_state;
                /* IA bindings are ordered command-list state. Use the values
                 * replayed immediately before this draw rather than the
                 * recording-time snapshot, whose buffers may still reflect
                 * an earlier dynamic-ring offset. Other state remains in the
                 * ownership-bearing draw snapshot until its setters are
                 * represented as commands too. */
                memcpy(merged.vertex_buffers, saved.vertex_buffers,
                       sizeof(merged.vertex_buffers));
                memcpy(merged.vertex_strides, saved.vertex_strides,
                       sizeof(merged.vertex_strides));
                memcpy(merged.vertex_offsets, saved.vertex_offsets,
                       sizeof(merged.vertex_offsets));
                merged.index_buffer = saved.index_buffer;
                merged.index_format = saved.index_format;
                merged.index_offset = saved.index_offset;
                merged.topology = saved.topology;
                context->state = merged;
                trace_draw_snapshot(&context->state, command->args.integers.a, 0);
            }
            context_draw_indexed(this, command->args.integers.a,
                                 command->args.integers.b,
                                 command->args.integers.signed_value);
            if (has_snapshot) context->state = saved;
            break;
        }
        case BEER_COMMAND_DRAW: {
            BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
            BeerD3D11PipelineState saved;
            int has_snapshot = command->draw_state != NULL;
            if (has_snapshot) {
                saved = context->state;
                context->state = *command->draw_state;
            }
            context_draw(this, command->args.integers.a,
                         command->args.integers.b);
            if (has_snapshot) context->state = saved;
            break;
        }
        case BEER_COMMAND_DRAW_INDEXED_INSTANCED: {
            BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
            BeerD3D11PipelineState saved;
            int has_snapshot = command->draw_state != NULL;
            if (has_snapshot) {
                saved = context->state;
                context->state = *command->draw_state;
                trace_draw_snapshot(&context->state, command->args.integers.a, 0);
            }
            context_draw_indexed_instanced(
                this, command->args.integers.a, command->args.integers.b,
                command->args.integers.c, command->args.integers.signed_value,
                command->args.integers.d);
            if (has_snapshot) context->state = saved;
            break;
        }
        case BEER_COMMAND_DRAW_INSTANCED: {
            BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
            BeerD3D11PipelineState saved;
            int has_snapshot = command->draw_state != NULL;
            if (has_snapshot) {
                saved = context->state;
                context->state = *command->draw_state;
                trace_draw_snapshot(&context->state, command->args.integers.a,
                                    command->args.integers.c);
            }
            context_draw_instanced(
                this, command->args.integers.a, command->args.integers.b,
                command->args.integers.c, command->args.integers.d);
            if (has_snapshot) context->state = saved;
            break;
        }
        case BEER_COMMAND_DRAW_AUTO:
            context_draw_auto(this);
            break;
        case BEER_COMMAND_CLEAR_RENDER_TARGET:
            context_clear_rendertarget_view(this, command->object,
                                            command->args.color);
            break;
        case BEER_COMMAND_COPY_RESOURCE:
            context_copy_resource(this, command->object,
                                  command->second_object);
            break;
        case BEER_COMMAND_UPDATE_SUBRESOURCE: {
            const void *box = command->args.integers.d
                ? command->owned_data : NULL;
            const uint8_t *source = (const uint8_t *)command->owned_data +
                (command->args.integers.d ? 24u : 0u);
            apply_update_subresource(command->object,
                command->args.integers.a, box, source,
                command->args.integers.b, command->args.integers.c);
            break;
        }
        case BEER_COMMAND_BEGIN_QUERY:
            context_begin(this, command->object);
            break;
        case BEER_COMMAND_END_QUERY:
            context_end(this, command->object);
            break;
        }
    }

    pipeline_state_release(&context->state);
    memset(&context->state, 0, sizeof(context->state));
    if (restore_state && saved_state) {
        context->state = *saved_state;
        free(saved_state);
    }
    pthread_mutex_unlock(&g_immediate_context_replay_lock);
}

static HRESULT D3D11_MS_ABI context_finish_command_list(
    ID3D11DeviceContext *this, int restore_state, void **command_list)
{
    static _Atomic(uint32_t) calls;
    uint32_t call = atomic_fetch_add(&calls, 1) + 1;
    if (render_diagnostics_enabled() && (call <= 64 || (call % 256) == 0)) {
        uintptr_t caller = (uintptr_t)__builtin_return_address(0);
        size_t pending = this
            ? ((BeerD3D11DeviceContext *)this)->command_count : 0;
        fprintf(stderr,
                "[D3D11 TRACE] FinishCommandList #%u context=%p thread=%#lx "
                "caller=%p restore=%d output=%p pending=%zu\n",
                call, (void *)this, (unsigned long)pthread_self(), (void *)caller,
                restore_state, (void *)command_list, pending);
    }
    if (!command_list) return (HRESULT)0x80070057;
    *command_list = NULL;
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (!context || context->type != 1)
        return (HRESULT)0x80070057;
    command_list_vtable_initialize();
    BeerD3D11CommandList *list = com_alloc(
        sizeof(*list), COM_TYPE_COMMAND_LIST, 0);
    if (!list) return (HRESULT)0x8007000e;
    list->vtable = g_command_list_vtable;
    list->device = context->device;
    if (list->device) device_addref(list->device);
    list->commands = context->commands;
    list->command_count = context->command_count;
    list->context_flags = context->flags;
    list->indexed_draw_count = 0;
    list->finalize_sequence = call;
    atomic_store(&list->executed, 0);
    for (size_t i = 0; i < list->command_count; ++i)
        if (list->commands[i].type == BEER_COMMAND_DRAW_INDEXED ||
            list->commands[i].type == BEER_COMMAND_DRAW_INDEXED_INSTANCED)
            ++list->indexed_draw_count;
    uint32_t indexed_list = list->indexed_draw_count
        ? atomic_fetch_add(&g_finalized_indexed_command_lists, 1) + 1 : 0;
    if (indexed_list && indexed_list <= 16) {
        for (size_t i = 0; i < list->command_count; ++i) {
            BeerD3D11Command *draw = &list->commands[i];
            if (draw->type != BEER_COMMAND_DRAW_INDEXED || !draw->draw_state) continue;
            BeerD3D11PipelineState *state = draw->draw_state;
            BeerD3D11Shader *vs = state->vertex_shader;
            BeerD3D11Shader *ps = state->pixel_shader;
            uint32_t vs_hash = vs && com_get_header(vs)
                ? fnv1a_bytes(vs->bytecode, vs->bytecode_size) : 0;
            uint32_t ps_hash = ps && com_get_header(ps)
                ? fnv1a_bytes(ps->bytecode, ps->bytecode_size) : 0;
            BeerD3D11View *target_view = state->render_targets[0];
            BeerD3D11Resource *target = target_view && com_get_header(target_view)
                ? validated_resource(target_view->resource) : NULL;
            const uint32_t *desc = target ? (const uint32_t *)target->desc : NULL;
            fprintf(stderr,
                    "[D3D11 RETAINED DRAW] list=%u command=%zu indices=%u start=%u base=%d "
                    "topology=%u VB=%p/%u/%u IB=%p/F%u/%u VS=%08x PS=%08x "
                    "RT=%p/%ux%u/F%u\n",
                    indexed_list, i, draw->args.integers.a,
                    draw->args.integers.b, draw->args.integers.signed_value,
                    state->topology, state->vertex_buffers[0],
                    state->vertex_strides[0], state->vertex_offsets[0],
                    state->index_buffer, state->index_format, state->index_offset,
                    vs_hash, ps_hash, target,
                    desc ? desc[0] : 0, desc ? desc[1] : 0, desc ? desc[4] : 0);
        }
    }
    if (render_diagnostics_enabled() &&
        (call <= 16 || ((call % 256) == 0 && list->command_count) ||
         (indexed_list && indexed_list <= 32))) {
        fprintf(stderr,
                "[D3D11 TRACE] finalized list=%p sequence=%u commands=%zu indexed-sequence=%u\n",
                (void *)list, call, list->command_count, indexed_list);
        for (size_t i = 0; i < list->command_count; ++i)
            fprintf(stderr, "[D3D11 TRACE] command list #%u command[%zu]=%s snapshot=%p\n",
                    call, i, command_type_name(list->commands[i].type),
                    (void *)list->commands[i].draw_state);
    }
    context->commands = NULL;
    context->command_count = 0;
    context->command_capacity = 0;

    /* D3D11 specifies that FALSE resets a deferred context to its default
     * state after the command list is finalized. Keeping Beer’s retained
     * bindings alive across frames made later jobs inherit stale shaders,
     * resources and targets that the guest had not set for that list. */
    if (!restore_state) {
        pipeline_state_release(&context->state);
        memset(&context->state, 0, sizeof(context->state));
    }

    if (call <= 16)
        fprintf(stderr, "[D3D11 TRACE] command list #%u captured %zu commands\n",
                call, list->command_count);
    *command_list = list;
    return S_OK;
}

static void D3D11_MS_ABI context_clear_depthstencil_view(ID3D11DeviceContext* this, void* pDepthStencilView, uint32_t ClearFlags, float Depth, uint8_t Stencil) {
    /* Stub - do nothing */
}
BEER_HOST_FLOAT_FN(context_clear_depthstencil_view, 1u << 3)   /* float Depth is the 4th argument */

typedef struct {
    void *data;
    uint32_t row_pitch;
    uint32_t depth_pitch;
} BeerMappedSubresource;

static HRESULT D3D11_MS_ABI context_map(
    ID3D11DeviceContext* this, void* pResource, uint32_t Subresource,
    uint32_t MapType, uint32_t MapFlags, void* pMappedResource)
{
    (void)MapFlags;
    BeerMappedSubresource *mapped = pMappedResource;
    if (mapped) memset(mapped, 0, sizeof(*mapped));
    BeerD3D11Resource *resource = validated_resource(pResource);
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    BeerD3D11MappedResource *mapping = NULL;
    if (context) {
        for (uint32_t i = 0; i < BEER_MAX_MAPPED_RESOURCES; ++i) {
            if (context->mapped[i].resource == pResource &&
                context->mapped[i].subresource == Subresource) {
                mapping = &context->mapped[i];
                break;
            }
            if (!mapping && !context->mapped[i].resource)
                mapping = &context->mapped[i];
        }
    }
    if (!resource || !context || !mapped || !mapping || mapping->resource ||
        Subresource >= (resource && resource->subresource_count
            ? resource->subresource_count : 1) || !resource->pixels) {
        static _Atomic(uint32_t) rejected_maps;
        uint32_t rejection = atomic_fetch_add(&rejected_maps, 1) + 1;
        if (rejection <= 32)
            fprintf(stderr,
                    "[D3D11 MAP REJECT] #%u context=%p resource=%p validated=%p "
                    "subresource=%u count=%u type=%u flags=0x%x pixels=%p mapping=%p\n",
                    rejection, (void *)this, pResource, (void *)resource,
                    Subresource, resource ? resource->subresource_count : 0,
                    MapType, MapFlags, resource ? (void *)resource->pixels : NULL,
                    (void *)mapping);
        return (HRESULT)0x80070057;
    }
    resource_sync_from_vulkan(resource);

    /* A deferred context may only map dynamic resources with WRITE_DISCARD or
     * WRITE_NO_OVERWRITE. Its returned memory is temporary command data: the
     * write does not become visible to the resource until Unmap records it in
     * the command list. Returning the live backing store here made Sekiro's
     * Resource Update jobs appear empty and applied uploads out of order.
     *
     * WRITE_NO_OVERWRITE is a sparse append/update contract. Preserve a
     * recording-time baseline only to identify the bytes the guest actually
     * changed; replaying a full stale buffer would overwrite ranges produced
     * by other deferred lists between recording and execution. */
    if (context->type == 1) {
        if (resource->dimension != 1 || (MapType != 4 && MapType != 5))
            return (HRESULT)0x80070057;
        mapping->data = calloc(1, resource->pixel_size);
        if (!mapping->data) return (HRESULT)0x8007000e;
        if (MapType == 5) {
            mapping->baseline = malloc(resource->pixel_size);
            if (!mapping->baseline) {
                free(mapping->data);
                mapping->data = NULL;
                return (HRESULT)0x8007000e;
            }
            memcpy(mapping->data, resource->pixels, resource->pixel_size);
            memcpy(mapping->baseline, resource->pixels, resource->pixel_size);
        }
        mapping->resource = pResource;
        mapping->subresource = Subresource;
        mapping->row_pitch = resource->row_pitch;
        mapping->depth_pitch = resource->row_pitch;
        mapping->map_type = MapType;
        mapping->size = resource->pixel_size;
        mapped->data = mapping->data;
        mapped->row_pitch = mapping->row_pitch;
        mapped->depth_pitch = mapping->depth_pitch;
    } else {
        if (resource->subresource_count) {
            mapped->data = resource->pixels + resource->subresource_offsets[Subresource];
            mapped->row_pitch = resource->subresource_row_pitches[Subresource];
            mapped->depth_pitch = (uint32_t)resource->subresource_sizes[Subresource];
        } else {
            mapped->data = resource->pixels;
            mapped->row_pitch = resource->row_pitch;
            mapped->depth_pitch = resource->dimension == 3
                ? (uint32_t)resource->pixel_size : resource->row_pitch;
        }
    }

    static _Atomic(uint32_t) calls;
    uint32_t call = atomic_fetch_add(&calls, 1) + 1;
    if (call <= 32)
        fprintf(stderr, "[D3D11 TRACE] Map #%u context=%p deferred=%d resource=%p "
                "type=%u data=%p row=%u depth=%u\n",
                call, (void *)this, context->type == 1, pResource, MapType,
                mapped->data, mapped->row_pitch, mapped->depth_pitch);
    return S_OK;
}

static void D3D11_MS_ABI context_unmap(
    ID3D11DeviceContext* this, void* pResource, uint32_t Subresource)
{
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (!context || context->type != 1) return;
    BeerD3D11MappedResource *mapping = NULL;
    for (uint32_t i = 0; i < BEER_MAX_MAPPED_RESOURCES; ++i)
        if (context->mapped[i].resource == pResource &&
            context->mapped[i].subresource == Subresource) {
            mapping = &context->mapped[i];
            break;
        }
    if (!mapping || !mapping->data) {
        static _Atomic(uint32_t) mismatched_unmaps;
        uint32_t mismatch = atomic_fetch_add(&mismatched_unmaps, 1) + 1;
        if (mismatch <= 32)
            fprintf(stderr,
                    "[D3D11 UNMAP MISMATCH] #%u context=%p resource=%p "
                    "subresource=%u\n",
                    mismatch, (void *)this, pResource, Subresource);
        return;
    }

    BeerD3D11Command command = {
        .type = BEER_COMMAND_UPDATE_SUBRESOURCE,
        .object = pResource
    };
    command.args.integers.a = Subresource;
    command.args.integers.b = mapping->row_pitch;
    command.args.integers.c = mapping->depth_pitch;
    command.args.integers.d = 0;
    if (mapping->map_type == 5 && mapping->baseline) {
        size_t first = 0;
        while (first < mapping->size &&
               ((uint8_t *)mapping->data)[first] ==
                   ((uint8_t *)mapping->baseline)[first])
            ++first;
        size_t last = mapping->size;
        while (last > first &&
               ((uint8_t *)mapping->data)[last - 1] ==
                   ((uint8_t *)mapping->baseline)[last - 1])
            --last;
        if (first < last && last <= UINT32_MAX) {
            command.args.integers.d = 1;
            command.owned_data_size = 24u + last - first;
            command.owned_data = malloc(command.owned_data_size);
            if (command.owned_data) {
                uint32_t box[6] = {
                    (uint32_t)first, 0, 0, (uint32_t)last, 1, 1
                };
                memcpy(command.owned_data, box, sizeof(box));
                memcpy((uint8_t *)command.owned_data + sizeof(box),
                       (uint8_t *)mapping->data + first, last - first);
            }
        }
    } else {
        command.owned_data = mapping->data;
        command.owned_data_size = mapping->size;
        mapping->data = NULL;
    }
    free(mapping->data);
    free(mapping->baseline);
    memset(mapping, 0, sizeof(*mapping));
    int recorded = command.owned_data
        ? context_record_command(this, &command) : 1;
    static _Atomic(uint32_t) unmap_calls;
    uint32_t call = atomic_fetch_add(&unmap_calls, 1) + 1;
    if (call <= 32)
        fprintf(stderr, "[D3D11 TRACE] Unmap #%u context=%p deferred=1 resource=%p "
                "bytes=%zu recorded=%d pending=%zu\n",
                call, (void *)this, pResource, command.owned_data_size, recorded,
                context->command_count);
    if (!recorded)
        free(command.owned_data);
}

static void D3D11_MS_ABI context_clear_state(ID3D11DeviceContext *this)
{
    static _Atomic(uint32_t) calls;
    uint32_t call = atomic_fetch_add(&calls, 1) + 1;
    if (call <= 16) fprintf(stderr, "[D3D11 TRACE] ClearState #%u\n", call);
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (!context) return;
    pipeline_state_release(&context->state);
    memset(&context->state, 0, sizeof(context->state));
}

static void D3D11_MS_ABI context_flush(ID3D11DeviceContext* this) {
    /* CPU-backed operations complete synchronously. */
    (void)this;
}

static uint32_t D3D11_MS_ABI context_get_type(ID3D11DeviceContext *object)
{
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)object;
    return context ? context->type : 0;
}

static uint32_t D3D11_MS_ABI context_get_context_flags(ID3D11DeviceContext *object)
{
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)object;
    return context ? context->flags : 0;
}

static void D3D11_MS_ABI context_noop(void *this, ...)
{
    (void)this;
}

#define DEFINE_CONTEXT_SLOT_TRACE(slot) \
    static void D3D11_MS_ABI context_trace_slot_##slot(void *this, ...) \
    { \
        static _Atomic(uint32_t) calls; \
        uint32_t call = atomic_fetch_add(&calls, 1) + 1; \
        if (call <= 8) \
            fprintf(stderr, "[D3D11 SLOT] context slot " #slot " #%u context=%p type=%u\\n", \
                    call, this, context_get_type((ID3D11DeviceContext *)this)); \
    }

DEFINE_CONTEXT_SLOT_TRACE(3)
DEFINE_CONTEXT_SLOT_TRACE(4)
DEFINE_CONTEXT_SLOT_TRACE(5)
DEFINE_CONTEXT_SLOT_TRACE(6)
DEFINE_CONTEXT_SLOT_TRACE(22)
DEFINE_CONTEXT_SLOT_TRACE(25)
DEFINE_CONTEXT_SLOT_TRACE(26)
DEFINE_CONTEXT_SLOT_TRACE(30)
DEFINE_CONTEXT_SLOT_TRACE(31)
DEFINE_CONTEXT_SLOT_TRACE(32)
DEFINE_CONTEXT_SLOT_TRACE(34)
DEFINE_CONTEXT_SLOT_TRACE(37)
DEFINE_CONTEXT_SLOT_TRACE(49)
DEFINE_CONTEXT_SLOT_TRACE(51)
DEFINE_CONTEXT_SLOT_TRACE(52)
DEFINE_CONTEXT_SLOT_TRACE(54)
DEFINE_CONTEXT_SLOT_TRACE(55)
DEFINE_CONTEXT_SLOT_TRACE(56)
DEFINE_CONTEXT_SLOT_TRACE(59)
DEFINE_CONTEXT_SLOT_TRACE(60)
DEFINE_CONTEXT_SLOT_TRACE(61)
DEFINE_CONTEXT_SLOT_TRACE(62)
DEFINE_CONTEXT_SLOT_TRACE(63)
DEFINE_CONTEXT_SLOT_TRACE(64)
DEFINE_CONTEXT_SLOT_TRACE(65)
DEFINE_CONTEXT_SLOT_TRACE(66)
DEFINE_CONTEXT_SLOT_TRACE(67)
DEFINE_CONTEXT_SLOT_TRACE(68)
DEFINE_CONTEXT_SLOT_TRACE(69)
DEFINE_CONTEXT_SLOT_TRACE(70)
DEFINE_CONTEXT_SLOT_TRACE(71)
DEFINE_CONTEXT_SLOT_TRACE(72)
DEFINE_CONTEXT_SLOT_TRACE(73)
DEFINE_CONTEXT_SLOT_TRACE(74)
DEFINE_CONTEXT_SLOT_TRACE(75)
DEFINE_CONTEXT_SLOT_TRACE(76)
DEFINE_CONTEXT_SLOT_TRACE(77)
DEFINE_CONTEXT_SLOT_TRACE(78)
DEFINE_CONTEXT_SLOT_TRACE(79)
DEFINE_CONTEXT_SLOT_TRACE(80)
DEFINE_CONTEXT_SLOT_TRACE(81)
DEFINE_CONTEXT_SLOT_TRACE(82)
DEFINE_CONTEXT_SLOT_TRACE(83)
DEFINE_CONTEXT_SLOT_TRACE(84)
DEFINE_CONTEXT_SLOT_TRACE(85)
DEFINE_CONTEXT_SLOT_TRACE(86)
DEFINE_CONTEXT_SLOT_TRACE(87)
DEFINE_CONTEXT_SLOT_TRACE(88)
DEFINE_CONTEXT_SLOT_TRACE(89)
DEFINE_CONTEXT_SLOT_TRACE(90)
DEFINE_CONTEXT_SLOT_TRACE(91)
DEFINE_CONTEXT_SLOT_TRACE(92)
DEFINE_CONTEXT_SLOT_TRACE(93)
DEFINE_CONTEXT_SLOT_TRACE(94)
DEFINE_CONTEXT_SLOT_TRACE(95)
DEFINE_CONTEXT_SLOT_TRACE(96)
DEFINE_CONTEXT_SLOT_TRACE(97)
DEFINE_CONTEXT_SLOT_TRACE(98)
DEFINE_CONTEXT_SLOT_TRACE(99)
DEFINE_CONTEXT_SLOT_TRACE(100)
DEFINE_CONTEXT_SLOT_TRACE(101)
DEFINE_CONTEXT_SLOT_TRACE(102)
DEFINE_CONTEXT_SLOT_TRACE(103)
DEFINE_CONTEXT_SLOT_TRACE(104)
DEFINE_CONTEXT_SLOT_TRACE(105)
DEFINE_CONTEXT_SLOT_TRACE(106)
DEFINE_CONTEXT_SLOT_TRACE(107)
DEFINE_CONTEXT_SLOT_TRACE(108)
DEFINE_CONTEXT_SLOT_TRACE(109)

static ID3D11DeviceContext_VTable g_context_vtable;

static void init_context_vtable(void)
{
    static int initialized;
    if (initialized) return;
    initialized = 1;
    for (size_t i = 0; i < D3D11_CONTEXT_VTABLE_SLOTS; ++i)
        g_context_vtable.slots[i] = (void *)context_noop;
#define INSTALL_CONTEXT_SLOT_TRACE(slot) \
    g_context_vtable.slots[slot] = (void *)context_trace_slot_##slot
    INSTALL_CONTEXT_SLOT_TRACE(3); INSTALL_CONTEXT_SLOT_TRACE(4);
    INSTALL_CONTEXT_SLOT_TRACE(5); INSTALL_CONTEXT_SLOT_TRACE(6);
    INSTALL_CONTEXT_SLOT_TRACE(22); INSTALL_CONTEXT_SLOT_TRACE(25);
    INSTALL_CONTEXT_SLOT_TRACE(26); INSTALL_CONTEXT_SLOT_TRACE(30);
    INSTALL_CONTEXT_SLOT_TRACE(31); INSTALL_CONTEXT_SLOT_TRACE(32);
    INSTALL_CONTEXT_SLOT_TRACE(34); INSTALL_CONTEXT_SLOT_TRACE(37);
    INSTALL_CONTEXT_SLOT_TRACE(49); INSTALL_CONTEXT_SLOT_TRACE(51);
    INSTALL_CONTEXT_SLOT_TRACE(52); INSTALL_CONTEXT_SLOT_TRACE(54);
    INSTALL_CONTEXT_SLOT_TRACE(55); INSTALL_CONTEXT_SLOT_TRACE(56);
    INSTALL_CONTEXT_SLOT_TRACE(59); INSTALL_CONTEXT_SLOT_TRACE(60);
    INSTALL_CONTEXT_SLOT_TRACE(61); INSTALL_CONTEXT_SLOT_TRACE(62);
    INSTALL_CONTEXT_SLOT_TRACE(63); INSTALL_CONTEXT_SLOT_TRACE(64);
    INSTALL_CONTEXT_SLOT_TRACE(65); INSTALL_CONTEXT_SLOT_TRACE(66);
    INSTALL_CONTEXT_SLOT_TRACE(67); INSTALL_CONTEXT_SLOT_TRACE(68);
    INSTALL_CONTEXT_SLOT_TRACE(69); INSTALL_CONTEXT_SLOT_TRACE(70);
    INSTALL_CONTEXT_SLOT_TRACE(71); INSTALL_CONTEXT_SLOT_TRACE(72);
    INSTALL_CONTEXT_SLOT_TRACE(73); INSTALL_CONTEXT_SLOT_TRACE(74);
    INSTALL_CONTEXT_SLOT_TRACE(75); INSTALL_CONTEXT_SLOT_TRACE(76);
    INSTALL_CONTEXT_SLOT_TRACE(77); INSTALL_CONTEXT_SLOT_TRACE(78);
    INSTALL_CONTEXT_SLOT_TRACE(79); INSTALL_CONTEXT_SLOT_TRACE(80);
    INSTALL_CONTEXT_SLOT_TRACE(81); INSTALL_CONTEXT_SLOT_TRACE(82);
    INSTALL_CONTEXT_SLOT_TRACE(83); INSTALL_CONTEXT_SLOT_TRACE(84);
    INSTALL_CONTEXT_SLOT_TRACE(85); INSTALL_CONTEXT_SLOT_TRACE(86);
    INSTALL_CONTEXT_SLOT_TRACE(87); INSTALL_CONTEXT_SLOT_TRACE(88);
    INSTALL_CONTEXT_SLOT_TRACE(89); INSTALL_CONTEXT_SLOT_TRACE(90);
    INSTALL_CONTEXT_SLOT_TRACE(91); INSTALL_CONTEXT_SLOT_TRACE(92);
    INSTALL_CONTEXT_SLOT_TRACE(93); INSTALL_CONTEXT_SLOT_TRACE(94);
    INSTALL_CONTEXT_SLOT_TRACE(95); INSTALL_CONTEXT_SLOT_TRACE(96);
    INSTALL_CONTEXT_SLOT_TRACE(97); INSTALL_CONTEXT_SLOT_TRACE(98);
    INSTALL_CONTEXT_SLOT_TRACE(99); INSTALL_CONTEXT_SLOT_TRACE(100);
    INSTALL_CONTEXT_SLOT_TRACE(101); INSTALL_CONTEXT_SLOT_TRACE(102);
    INSTALL_CONTEXT_SLOT_TRACE(103); INSTALL_CONTEXT_SLOT_TRACE(104);
    INSTALL_CONTEXT_SLOT_TRACE(105); INSTALL_CONTEXT_SLOT_TRACE(106);
    INSTALL_CONTEXT_SLOT_TRACE(107); INSTALL_CONTEXT_SLOT_TRACE(108);
    INSTALL_CONTEXT_SLOT_TRACE(109);
#undef INSTALL_CONTEXT_SLOT_TRACE
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_QUERY_INTERFACE] = (void *)context_query_interface;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_ADD_REF] = (void *)context_addref;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_RELEASE] = (void *)context_release;
    g_context_vtable.slots[3] = (void *)context_get_device;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_IA_SET_INPUT_LAYOUT] = (void *)context_ia_set_input_layout;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_IA_SET_VERTEX_BUFFERS] = (void *)context_ia_set_vertex_buffers;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_IA_SET_INDEX_BUFFER] = (void *)context_ia_set_index_buffer;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_IA_SET_PRIMITIVE_TOPOLOGY] = (void *)context_ia_set_primitive_topology;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_VS_SET_SHADER] = (void *)context_vs_set_shader;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_VS_SET_CONSTANT_BUFFERS] = (void *)context_vs_set_constant_buffers;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_PS_SET_SHADER] = (void *)context_ps_set_shader;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_PS_SET_CONSTANT_BUFFERS] = (void *)context_ps_set_constant_buffers;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_PS_SET_SHADER_RESOURCES] = (void *)context_ps_set_shader_resources;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_PS_SET_SAMPLERS] = (void *)context_ps_set_samplers;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_GS_SET_SHADER] = (void *)context_gs_set_shader;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_DRAW_INDEXED] = (void *)context_draw_indexed;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_DRAW] = (void *)context_draw;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_DRAW_INDEXED_INSTANCED] = (void *)context_draw_indexed_instanced;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_DRAW_INSTANCED] = (void *)context_draw_instanced;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_BEGIN] = (void *)context_begin;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_END] = (void *)context_end;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_GET_DATA] = (void *)context_get_data;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_DRAW_AUTO] = (void *)context_draw_auto;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_DRAW_INDEXED_INSTANCED_INDIRECT] = (void *)context_draw_indirect;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_DRAW_INSTANCED_INDIRECT] = (void *)context_draw_indirect;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_DISPATCH] = (void *)context_dispatch;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_DISPATCH_INDIRECT] = (void *)context_dispatch_indirect;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_OM_SET_RENDER_TARGETS] = (void *)context_om_set_render_targets;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_OM_SET_BLEND_STATE] = (void *)context_om_set_blend_state;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_OM_SET_DEPTH_STENCIL_STATE] = (void *)context_om_set_depthstencil_state;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_RS_SET_STATE] = (void *)context_rs_set_state;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_RS_SET_VIEWPORTS] = (void *)context_rs_set_viewports;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_RS_SET_SCISSOR_RECTS] = (void *)context_rs_set_scissor_rects;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_COPY_SUBRESOURCE_REGION] = (void *)context_copy_subresource_region;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_COPY_RESOURCE] = (void *)context_copy_resource;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_UPDATE_SUBRESOURCE] = (void *)context_update_subresource;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_CLEAR_RENDER_TARGET_VIEW] = (void *)context_clear_rendertarget_view;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_CLEAR_DEPTH_STENCIL_VIEW] = (void *)context_clear_depthstencil_view;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_RESOLVE_SUBRESOURCE] = (void *)context_resolve_subresource;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_EXECUTE_COMMAND_LIST] = (void *)context_execute_command_list;
    /* Official D3D11 context slots after ExecuteCommandList: hull, domain and
     * compute-stage setters. These were previously shared no-ops, hiding the
     * producer pipeline that runs before the title post-processing passes. */
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_HS_SET_SHADER_RESOURCES] = (void *)context_hs_set_shader_resources;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_HS_SET_SHADER] = (void *)context_hs_set_shader;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_HS_SET_SAMPLERS] = (void *)context_hs_set_samplers;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_HS_SET_CONSTANT_BUFFERS] = (void *)context_hs_set_constant_buffers;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_DS_SET_SHADER_RESOURCES] = (void *)context_ds_set_shader_resources;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_DS_SET_SHADER] = (void *)context_ds_set_shader;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_DS_SET_SAMPLERS] = (void *)context_ds_set_samplers;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_DS_SET_CONSTANT_BUFFERS] = (void *)context_ds_set_constant_buffers;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_CS_SET_SHADER_RESOURCES] = (void *)context_cs_set_shader_resources;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_CS_SET_UNORDERED_ACCESS_VIEWS] = (void *)context_cs_set_unordered_access_views;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_CS_SET_SHADER] = (void *)context_cs_set_shader;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_CS_SET_SAMPLERS] = (void *)context_cs_set_samplers;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_CS_SET_CONSTANT_BUFFERS] = (void *)context_cs_set_constant_buffers;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_MAP] = (void *)context_map;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_UNMAP] = (void *)context_unmap;
    /* ID3D11DeviceContext slot 89 is OMGetRenderTargets. Returning without
     * initializing its mandatory outputs made the engine lose the active
     * producer target while assembling later render jobs. */
    g_context_vtable.slots[89] = (void *)context_om_get_render_targets;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_CLEAR_STATE] = (void *)context_clear_state;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_FLUSH] = (void *)context_flush;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_GET_TYPE] = (void *)context_get_type;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_GET_CONTEXT_FLAGS] = (void *)context_get_context_flags;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_FINISH_COMMAND_LIST] = (void *)context_finish_command_list;
    d3d11_stats_wrap("context", g_context_vtable.slots, D3D11_CONTEXT_VTABLE_SLOTS);
}

static ID3D11DeviceContext *d3d11_device_context_create_typed(
    ID3D11Device *device, uint32_t context_type, uint32_t context_flags)
{
    init_context_vtable();
    BeerD3D11DeviceContext *context = com_alloc(
        sizeof(*context), COM_TYPE_CONTEXT, 0);
    if (!context) return NULL;
    context->vtable = g_context_vtable.slots;
    context->type = context_type;
    context->flags = context_flags;
    context->device = device;
    if (device) device_addref(device);
    context->commands = NULL;
    context->command_count = 0;
    context->command_capacity = 0;
    memset(&context->state, 0, sizeof(context->state));
    memset(context->mapped, 0, sizeof(context->mapped));
    return (ID3D11DeviceContext *)context;
}

ID3D11DeviceContext* d3d11_device_context_create(void) {
    return d3d11_device_context_create_typed(NULL, 0, 0);
}

ID3D11DeviceContext *d3d11_device_get_immediate_context_owned(
    ID3D11Device *device)
{
    void *context = NULL;
    if (!device) return NULL;
    device_get_immediate_context(device, &context);
    return context;
}

/* ============================================================================
 * IDXGISwapChain - descriptor-backed swap chain and typed back buffer
 * ============================================================================ */

typedef struct {
    void **vtable;
    ID3D11Device *device;
    BeerDxgiSwapChainDesc desc;
    BeerD3D11Resource *back_buffer;
    uint32_t present_count;
    uint32_t logged_nonblack_frame;
    uint64_t last_presented_write_serial;
    uint64_t last_presented_content_serial;
    int fullscreen;
} BeerDxgiSwapChain;

static const uint8_t iid_iunknown[16] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xc0,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x46
};
static const uint8_t iid_idxgiswapchain[16] = {
    0x85,0x3a,0xbf,0x31,0xa4,0xd2,0x30,0x4b,
    0xa5,0x5f,0x4d,0x30,0x3a,0xc0,0x3a,0x2f
};
static const uint8_t iid_id3d11texture2d[16] = {
    0xf2,0xaa,0x15,0x6f,0x08,0xd2,0x89,0x4e,
    0x9a,0xb4,0x48,0x95,0x35,0xd3,0x4f,0x9c
};

static void swapchain_release_back_buffer(BeerDxgiSwapChain *this)
{
    if (this->back_buffer) {
        com_release(this->back_buffer);
        this->back_buffer = NULL;
    }
}

static HRESULT swapchain_recreate_back_buffer(BeerDxgiSwapChain *this)
{
    uint32_t desc[11] = {
        this->desc.BufferDesc.Width,
        this->desc.BufferDesc.Height,
        1, 1,
        this->desc.BufferDesc.Format,
        this->desc.SampleDesc.Count,
        this->desc.SampleDesc.Quality,
        0, /* D3D11_USAGE_DEFAULT */
        this->desc.BufferUsage,
        0, 0
    };
    if (!desc[0] || !desc[1] || !desc[4] || !desc[5])
        return (HRESULT)0x80070057;
    /* ResizeBuffers is only legal after all external back-buffer references are
     * released. Do not invalidate a texture that the guest still owns. */
    if (this->back_buffer) {
        ComObjectHeader *header = com_get_header(this->back_buffer);
        if (!header || atomic_load(&header->refcount) > 1)
            return (HRESULT)0x887a0001; /* DXGI_ERROR_INVALID_CALL */
    }
    BeerD3D11Resource *buffer = resource_create(this->device, 3, desc, sizeof(desc));
    if (!buffer) return (HRESULT)0x8007000e;
    swapchain_release_back_buffer(this);
    this->back_buffer = buffer;
    return S_OK;
}

static HRESULT D3D11_MS_ABI swapchain_query_interface(
    IDXGISwapChain *object, REFIID riid, LPVOID *ppvObj)
{
    if (!ppvObj) return (HRESULT)0x80070057;
    *ppvObj = NULL;
    if (!iid_equal(riid, iid_iunknown) && !iid_equal(riid, iid_idxgiswapchain))
        return E_NOINTERFACE;
    *ppvObj = object;
    com_addref(object);
    return S_OK;
}

static uint32_t D3D11_MS_ABI swapchain_addref(IDXGISwapChain *this) {
    return com_addref(this);
}

static uint32_t D3D11_MS_ABI command_list_release(
    BeerD3D11CommandList *object)
{
    ComObjectHeader *header = com_get_header(object);
    if (!header) return 0;
    uint32_t old_count = atomic_fetch_sub(&header->refcount, 1);
    if (old_count == 1) {
        if (object->indexed_draw_count) {
            uint32_t released =
                atomic_fetch_add(&g_released_indexed_command_lists, 1) + 1;
            if (render_diagnostics_enabled() &&
                (released <= 32 || (released % 256) == 0))
                fprintf(stderr,
                        "[D3D11 TRACE] released indexed command list #%u object=%p "
                        "executed=%u draws=%u commands=%zu totals=(finalized=%u executed=%u released=%u)\n",
                        released, (void *)object, atomic_load(&object->executed),
                        object->indexed_draw_count, object->command_count,
                        atomic_load(&g_finalized_indexed_command_lists),
                        atomic_load(&g_executed_indexed_command_lists),
                        atomic_load(&g_released_indexed_command_lists));
        }
        command_release_references(object->commands, object->command_count);
        free(object->commands);
        if (object->device) device_release(object->device);
        free(header);
        return 0;
    }
    return old_count - 1;
}

static uint32_t D3D11_MS_ABI swapchain_release(IDXGISwapChain *object) {
    BeerDxgiSwapChain *this = (BeerDxgiSwapChain *)object;
    ComObjectHeader *header = com_get_header(this);
    if (!header) return 0;
    uint32_t old_count = atomic_fetch_sub(&header->refcount, 1);
    if (old_count == 1) {
        swapchain_release_back_buffer(this);
        free(header);
        return 0;
    }
    return old_count - 1;
}

static HRESULT D3D11_MS_ABI swapchain_set_private_data(
    IDXGISwapChain *object, REFIID Name, uint32_t DataSize, const void *pData)
{
    (void)object; (void)Name; (void)DataSize; (void)pData;
    return (HRESULT)0x80004001; /* E_NOTIMPL */
}

static HRESULT D3D11_MS_ABI swapchain_set_private_data_interface(
    IDXGISwapChain *object, REFIID Name, const void *pUnknown)
{
    (void)object; (void)Name; (void)pUnknown;
    return (HRESULT)0x80004001;
}

static HRESULT D3D11_MS_ABI swapchain_get_private_data(
    IDXGISwapChain *object, REFIID Name, uint32_t *pDataSize, void *pData)
{
    (void)object; (void)Name; (void)pDataSize; (void)pData;
    return (HRESULT)0x887a0002; /* DXGI_ERROR_NOT_FOUND */
}

static HRESULT D3D11_MS_ABI swapchain_get_parent(
    IDXGISwapChain *object, REFIID riid, void **ppParent)
{
    (void)object; (void)riid;
    if (!ppParent) return (HRESULT)0x80070057;
    *ppParent = NULL;
    return E_NOINTERFACE;
}

static HRESULT D3D11_MS_ABI swapchain_get_device(
    IDXGISwapChain *object, REFIID riid, void **ppDevice)
{
    BeerDxgiSwapChain *this = (BeerDxgiSwapChain *)object;
    if (!ppDevice) return (HRESULT)0x80070057;
    *ppDevice = NULL;
    if (!this->device) return (HRESULT)0x887a0002;
    return device_query_interface(this->device, riid, ppDevice);
}

static void capture_first_visible_frame(const BeerD3D11Resource *back_buffer,
                                        uint32_t width, uint32_t height,
                                        uint32_t present_count)
{
    static _Atomic(uint32_t) captured;
    const char *path = getenv("BEER_FRAME_CAPTURE_PATH");
    const char *minimum_present_text = getenv("BEER_FRAME_CAPTURE_MIN_PRESENT");
    uint32_t minimum_present = minimum_present_text
        ? (uint32_t)strtoul(minimum_present_text, NULL, 10) : 0;
    if (!path || !*path || !back_buffer || !back_buffer->pixels ||
        present_count < minimum_present || atomic_exchange(&captured, 1))
        return;

    FILE *output = fopen(path, "wb");
    if (!output) {
        atomic_store(&captured, 0);
        return;
    }
    fprintf(output, "P6\n%u %u\n255\n", width, height);
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t *row = back_buffer->pixels + (size_t)y * back_buffer->row_pitch;
        for (uint32_t x = 0; x < width; ++x)
            fwrite(row + (size_t)x * 4, 1, 3, output);
    }
    fclose(output);
    fprintf(stderr, "[DXGI CAPTURE] wrote first visible frame to %s\n", path);
}

/* Writes the presented back buffer repeatedly so UI regressions can be
 * inspected without a desktop screenshot. The Vulkan/XWayland surface bypasses
 * X11 readback, so xwd/grim capture only black; the authentic pixels exist
 * solely in the Vulkan target mirror. This dump synchronizes that mirror into
 * the CPU copy and serializes it as a binary PPM.
 *
 * BEER_FRAME_DUMP_DIR       destination directory (enables the dump)
 * BEER_FRAME_DUMP_INTERVAL  presents between dumps (default 60)
 * BEER_FRAME_DUMP_LIMIT     maximum files written (default 240, 0 = unlimited)
 *
 * Diagnostics only: nothing here alters guest-visible state. */
static void dump_presented_frame(BeerD3D11Resource *back_buffer,
                                 uint32_t width, uint32_t height,
                                 uint32_t present_count)
{
    static int initialized;
    static const char *directory;
    static uint32_t interval = 60;
    static uint32_t limit = 240;
    static _Atomic(uint32_t) written;

    if (!initialized) {
        /* Racing presenters resolve to the same environment values. */
        const char *interval_text = getenv("BEER_FRAME_DUMP_INTERVAL");
        const char *limit_text = getenv("BEER_FRAME_DUMP_LIMIT");
        if (interval_text && *interval_text) {
            unsigned long value = strtoul(interval_text, NULL, 10);
            interval = value ? (uint32_t)value : 1;
        }
        if (limit_text && *limit_text)
            limit = (uint32_t)strtoul(limit_text, NULL, 10);
        directory = getenv("BEER_FRAME_DUMP_DIR");
        initialized = 1;
    }
    if (!directory || !*directory || !back_buffer || !back_buffer->pixels ||
        !width || !height)
        return;
    if (present_count % interval) return;
    if (limit && atomic_load(&written) >= limit) return;

    resource_sync_from_vulkan(back_buffer);

    char path[4096];
    int length = snprintf(path, sizeof(path), "%s/present-%06u.ppm",
                          directory, present_count);
    if (length <= 0 || (size_t)length >= sizeof(path)) return;
    FILE *output = fopen(path, "wb");
    if (!output) return;
    fprintf(output, "P6\n%u %u\n255\n", width, height);
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t *row =
            back_buffer->pixels + (size_t)y * back_buffer->row_pitch;
        for (uint32_t x = 0; x < width; ++x)
            fwrite(row + (size_t)x * 4, 1, 3, output);
    }
    fclose(output);
    uint32_t count = atomic_fetch_add(&written, 1) + 1;
    fprintf(stderr, "[DXGI DUMP] present=%u wrote %s (%u/%u)\n",
            present_count, path, count, limit);
}

static HRESULT D3D11_MS_ABI swapchain_present(IDXGISwapChain *object, uint32_t SyncInterval, uint32_t Flags) {
    BeerDxgiSwapChain *this = (BeerDxgiSwapChain *)object;
    uint32_t call = ++this->present_count;
    uint64_t write_serial = this->back_buffer
        ? atomic_load(&this->back_buffer->write_serial) : 0;
    uint64_t content_serial = this->back_buffer
        ? atomic_load(&this->back_buffer->content_serial) : 0;
    BeerD3D11Resource *diagnostic_target = g_last_textured_indexed_target;
    int new_back_buffer_write = write_serial != this->last_presented_write_serial;
    int new_back_buffer_content = content_serial != this->last_presented_content_serial;
    if (call <= 8 || (call % 120) == 0)
        fprintf(stderr, "[DXGI TRACE] Present #%u sync=%u flags=0x%x "
                "write-serial=%llu new-write=%d content-serial=%llu new-content=%d\n",
                call, SyncInterval, Flags, (unsigned long long)write_serial,
                new_back_buffer_write, (unsigned long long)content_serial,
                new_back_buffer_content);
    if (!this->back_buffer || !this->back_buffer->pixels)
        return (HRESULT)0x887a0001; /* DXGI_ERROR_INVALID_CALL */
    /* Present consumes the pending Vulkan compositor mirror directly. The
     * guest CPU back buffer stays stale until diagnostics/capture or another
     * genuine CPU consumer explicitly synchronizes it. */
    if (!xwayland_window_present_resource_rgba8(
            this->back_buffer, content_serial, this->back_buffer->pixels,
            (int)this->desc.BufferDesc.Width,
            (int)this->desc.BufferDesc.Height,
            (int)this->back_buffer->row_pitch))
        return (HRESULT)0x887a0005; /* DXGI_ERROR_DEVICE_REMOVED */
    dump_presented_frame(this->back_buffer, this->desc.BufferDesc.Width,
                         this->desc.BufferDesc.Height, call);
    const char *capture_path = getenv("BEER_FRAME_CAPTURE_PATH");
    int inspect_frame = render_diagnostics_enabled() ||
        (capture_path && *capture_path && !this->logged_nonblack_frame);
    if (inspect_frame &&
        (call <= 16 || (call % 60) == 0 || !this->logged_nonblack_frame)) {
        resource_sync_from_vulkan(this->back_buffer);
        size_t nonblack = 0, visible = 0;
        uint32_t maximum_luma = 0;
        uint32_t hash = resource_diagnostic_hash(this->back_buffer);
        for (size_t i = 0; i + 3 < this->back_buffer->pixel_size; i += 4) {
            uint32_t red = this->back_buffer->pixels[i];
            uint32_t green = this->back_buffer->pixels[i + 1];
            uint32_t blue = this->back_buffer->pixels[i + 2];
            uint32_t luma = (54u * red + 183u * green + 19u * blue) >> 8;
            if (red || green || blue) ++nonblack;
            if (luma >= 8u) ++visible;
            if (luma > maximum_luma) maximum_luma = luma;
        }
        if (render_diagnostics_enabled() && (call <= 16 || visible))
            fprintf(stderr, "[DXGI FRAME] present=%u back-buffer=%p fnv1a=%08x "
                    "nonblack=%zu visible=%zu max-luma=%u/%zu\n",
                    call, (void *)this->back_buffer, hash, nonblack, visible,
                    maximum_luma, this->back_buffer->pixel_size / 4);
        if (visible) {
            this->logged_nonblack_frame = 1;
            capture_first_visible_frame(this->back_buffer,
                                        this->desc.BufferDesc.Width,
                                        this->desc.BufferDesc.Height, call);
        }
    }
    if (render_diagnostics_enabled() && diagnostic_target &&
        diagnostic_target->pixels &&
        content_serial == atomic_load(&diagnostic_target->content_serial))
        fprintf(stderr,
                "[DXGI FLOW] present=%u authentic indexed content reached back buffer "
                "content-serial=%llu\n",
                call, (unsigned long long)content_serial);
    if (render_diagnostics_enabled() && diagnostic_target &&
        diagnostic_target->pixels &&
        diagnostic_target->pixel_size == this->back_buffer->pixel_size &&
        (call <= 16 || new_back_buffer_write)) {
        size_t diagnostic_nonblack = 0;
        for (size_t i = 0; i + 3 < diagnostic_target->pixel_size; i += 4)
            if (diagnostic_target->pixels[i] || diagnostic_target->pixels[i + 1] ||
                diagnostic_target->pixels[i + 2]) ++diagnostic_nonblack;
        if (diagnostic_nonblack)
            fprintf(stderr,
                    "[DXGI FLOW] present=%u back-buffer=%p serial=%llu content=%llu "
                    "hash=%08x last-textured-target=%p serial=%llu content=%llu "
                    "hash=%08x nonblack=%zu\n",
                    call, (void *)this->back_buffer,
                    (unsigned long long)write_serial,
                    (unsigned long long)content_serial,
                    resource_diagnostic_hash(this->back_buffer),
                    (void *)diagnostic_target,
                    (unsigned long long)atomic_load(&diagnostic_target->write_serial),
                    (unsigned long long)atomic_load(&diagnostic_target->content_serial),
                    resource_diagnostic_hash(diagnostic_target),
                    diagnostic_nonblack);
    }
    if (getenv("BEER_UI_STATE_TRACE")) {
        uint64_t draw_signature = 0, resource_signature = 0;
        uint64_t mode_draw_signatures[17] = {0};
        uint64_t mode_resource_signatures[17] = {0};
        uint32_t draw_count = 0, mode_counts[17] = {0};
        if (vulkan_indexed_renderer_consume_frame_trace(
                &draw_signature, &resource_signature, &draw_count,
                mode_counts, mode_draw_signatures,
                mode_resource_signatures)) {
            static uint64_t previous_draw_signature;
            static uint64_t previous_resource_signature;
            static uint32_t previous_draw_count;
            int changed = draw_signature != previous_draw_signature ||
                resource_signature != previous_resource_signature ||
                draw_count != previous_draw_count;
            if (changed || call <= 8 || (call % 120u) == 0) {
                fprintf(stderr,
                        "[D3D11 UI FRAME] present=%u draws=%u draw=%016llx "
                        "resources=%016llx modes=",
                        call, draw_count,
                        (unsigned long long)draw_signature,
                        (unsigned long long)resource_signature);
                for (uint32_t mode = 1; mode < 17; ++mode)
                    if (mode_counts[mode])
                        fprintf(stderr, "%s%u:%u",
                                mode == 1 ? "" : ",", mode,
                                mode_counts[mode]);
                fputc('\n', stderr);
                for (uint32_t mode = 1; mode < 17; ++mode) {
                    if (!mode_counts[mode]) continue;
                    fprintf(stderr,
                            "[D3D11 UI MODE] present=%u mode=%u count=%u "
                            "draw=%016llx resources=%016llx\n",
                            call, mode, mode_counts[mode],
                            (unsigned long long)mode_draw_signatures[mode],
                            (unsigned long long)mode_resource_signatures[mode]);
                }
            }
            previous_draw_signature = draw_signature;
            previous_resource_signature = resource_signature;
            previous_draw_count = draw_count;
        }
    }
    this->last_presented_write_serial = write_serial;
    this->last_presented_content_serial = content_serial;
    return S_OK;
}

static HRESULT D3D11_MS_ABI swapchain_get_buffer(
    IDXGISwapChain *object, uint32_t Buffer, REFIID riid, LPVOID *ppSurface)
{
    BeerDxgiSwapChain *this = (BeerDxgiSwapChain *)object;
    static _Atomic(uint32_t) calls;
    uint32_t call = atomic_fetch_add(&calls, 1) + 1;
    if (call <= 8)
        fprintf(stderr, "[DXGI TRACE] GetBuffer #%u index=%u iid=%p\n",
                call, Buffer, (void *)(uintptr_t)riid);
    if (!ppSurface) return (HRESULT)0x80070057;
    *ppSurface = NULL;
    if (Buffer != 0 || !this->back_buffer) return (HRESULT)0x887a0002;
    if (!iid_equal(riid, iid_iunknown) && !iid_equal(riid, iid_id3d11texture2d))
        return E_NOINTERFACE;
    *ppSurface = this->back_buffer;
    com_addref(this->back_buffer);
    return S_OK;
}

static HRESULT D3D11_MS_ABI swapchain_set_fullscreen_state(
    IDXGISwapChain *object, int Fullscreen, void *pTarget)
{
    (void)pTarget;
    BeerDxgiSwapChain *this = (BeerDxgiSwapChain *)object;
    int requested = Fullscreen != 0;
    if (!xwayland_window_set_fullscreen(requested))
        return (HRESULT)0x887a0001; /* DXGI_ERROR_INVALID_CALL */
    this->fullscreen = requested;
    this->desc.Windowed = !requested;
    fprintf(stderr, "[DXGI] fullscreen state -> %s\n",
            requested ? "fullscreen" : "windowed");
    return S_OK;
}

static HRESULT D3D11_MS_ABI swapchain_get_fullscreen_state(
    IDXGISwapChain *object, int *pFullscreen, void *ppTarget)
{
    if (pFullscreen) *pFullscreen = ((BeerDxgiSwapChain *)object)->fullscreen;
    if (ppTarget) *(void **)ppTarget = NULL;
    return S_OK;
}

static HRESULT D3D11_MS_ABI swapchain_get_desc(
    IDXGISwapChain *object, void *pDesc)
{
    if (!pDesc) return (HRESULT)0x80070057;
    memcpy(pDesc, &((BeerDxgiSwapChain *)object)->desc,
           sizeof(BeerDxgiSwapChainDesc));
    return S_OK;
}

static HRESULT D3D11_MS_ABI swapchain_resize_buffers(
    IDXGISwapChain *object, uint32_t BufferCount, uint32_t Width,
    uint32_t Height, uint32_t NewFormat, uint32_t SwapChainFlags)
{
    BeerDxgiSwapChain *this = (BeerDxgiSwapChain *)object;
    if (BufferCount) this->desc.BufferCount = BufferCount;
    if (Width) this->desc.BufferDesc.Width = Width;
    if (Height) this->desc.BufferDesc.Height = Height;
    if (NewFormat) this->desc.BufferDesc.Format = NewFormat;
    this->desc.Flags = SwapChainFlags;
    return swapchain_recreate_back_buffer(this);
}

static HRESULT D3D11_MS_ABI swapchain_resize_target(
    IDXGISwapChain *object, void *pNewTargetParameters)
{
    if (!pNewTargetParameters) return (HRESULT)0x80070057;
    memcpy(&((BeerDxgiSwapChain *)object)->desc.BufferDesc,
           pNewTargetParameters, sizeof(BeerDxgiModeDesc));
    return S_OK;
}

static HRESULT D3D11_MS_ABI swapchain_get_containing_output(IDXGISwapChain *this, void *ppOutput) {
    (void)this;
    if (!ppOutput) return (HRESULT)0x80070057;
    *(void **)ppOutput = NULL;
    return (HRESULT)0x887a0002;
}

static HRESULT D3D11_MS_ABI swapchain_get_frame_statistics(IDXGISwapChain *this, void *pStats) {
    (void)this; (void)pStats;
    return (HRESULT)0x887a0001; /* DXGI_ERROR_INVALID_CALL in windowed mode */
}

static HRESULT D3D11_MS_ABI swapchain_get_last_present_count(
    IDXGISwapChain *object, uint32_t *pLastPresentCount)
{
    if (!pLastPresentCount) return (HRESULT)0x80070057;
    *pLastPresentCount = ((BeerDxgiSwapChain *)object)->present_count;
    return S_OK;
}

static IDXGISwapChain_VTable g_swapchain_vtable = {
    .QueryInterface = swapchain_query_interface,
    .AddRef = swapchain_addref,
    .Release = swapchain_release,
    .SetPrivateData = swapchain_set_private_data,
    .SetPrivateDataInterface = swapchain_set_private_data_interface,
    .GetPrivateData = swapchain_get_private_data,
    .GetParent = swapchain_get_parent,
    .GetDevice = swapchain_get_device,
    .Present = swapchain_present,
    .GetBuffer = swapchain_get_buffer,
    .SetFullscreenState = swapchain_set_fullscreen_state,
    .GetFullscreenState = swapchain_get_fullscreen_state,
    .GetDesc = swapchain_get_desc,
    .ResizeBuffers = swapchain_resize_buffers,
    .ResizeTarget = swapchain_resize_target,
    .GetContainingOutput = swapchain_get_containing_output,
    .GetFrameStatistics = swapchain_get_frame_statistics,
    .GetLastPresentCount = swapchain_get_last_present_count,
};

IDXGISwapChain *dxgi_swapchain_create(void)
{
    BeerDxgiSwapChain *swapchain = com_alloc(sizeof(*swapchain), COM_TYPE_SWAPCHAIN, 0);
    if (!swapchain) return NULL;
    memset(swapchain, 0, sizeof(*swapchain));
    swapchain->vtable = (void **)&g_swapchain_vtable;
    return (IDXGISwapChain *)swapchain;
}

HRESULT dxgi_swapchain_configure(IDXGISwapChain *object,
                                 ID3D11Device *device,
                                 const BeerDxgiSwapChainDesc *desc)
{
    if (!object || !device || !desc || !desc->OutputWindow ||
        !desc->BufferDesc.Width || !desc->BufferDesc.Height ||
        !desc->BufferDesc.Format || !desc->SampleDesc.Count ||
        !desc->BufferCount)
        return (HRESULT)0x80070057;
    BeerDxgiSwapChain *this = (BeerDxgiSwapChain *)object;
    this->device = device;
    memcpy(&this->desc, desc, sizeof(this->desc));
    this->present_count = 0;
    this->logged_nonblack_frame = 0;
    this->last_presented_write_serial = UINT64_MAX;
    this->last_presented_content_serial = UINT64_MAX;
    this->fullscreen = !desc->Windowed;
    fprintf(stderr, "[DXGI] configured swap chain %ux%u format=%u buffers=%u window=%p mode=%s\n",
            desc->BufferDesc.Width, desc->BufferDesc.Height,
            desc->BufferDesc.Format, desc->BufferCount, desc->OutputWindow,
            desc->Windowed ? "windowed" : "fullscreen");
    HRESULT result = swapchain_recreate_back_buffer(this);
    if (result == S_OK && !desc->Windowed &&
        !xwayland_window_set_fullscreen(1))
        return (HRESULT)0x887a0001;
    return result;
}
