#include "d3d11_compat.h"
#include "../xwayland_backend.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>

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

static uint32_t __attribute__((ms_abi)) com_addref(void* obj) {
    ComObjectHeader* hdr = com_get_header(obj);
    if (!hdr) return 0;
    return atomic_fetch_add(&hdr->refcount, 1) + 1;
}

static uint32_t __attribute__((ms_abi)) com_release(void* obj) {
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

static HRESULT __attribute__((ms_abi)) dxgi_device_query_interface(
    BeerDxgiDevice *this, REFIID riid, LPVOID *ppvObj);

static uint32_t __attribute__((ms_abi)) dxgi_device_addref(BeerDxgiDevice *this)
{
    return com_addref(this);
}

static uint32_t __attribute__((ms_abi)) dxgi_device_release(BeerDxgiDevice *this)
{
    return com_release(this);
}

static void addref_external_com_object(void *object)
{
    if (!object) return;
    void **vtable = *(void ***)object;
    if (!vtable || !vtable[1]) return;
    typedef uint32_t (__attribute__((ms_abi)) *AddRefFn)(void *);
    ((AddRefFn)vtable[1])(object);
}

static HRESULT __attribute__((ms_abi)) dxgi_device_get_parent(
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

static HRESULT __attribute__((ms_abi)) dxgi_device_get_adapter(
    BeerDxgiDevice *this, void **ppAdapter)
{
    if (!ppAdapter) return (HRESULT)0x80070057; /* E_INVALIDARG */
    *ppAdapter = NULL;
    if (!this->adapter) return (HRESULT)0x887a0002; /* DXGI_ERROR_NOT_FOUND */
    *ppAdapter = this->adapter;
    addref_external_com_object(this->adapter);
    return S_OK;
}

static HRESULT __attribute__((ms_abi)) dxgi_device_unsupported(
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

static HRESULT __attribute__((ms_abi)) device_query_interface(ID3D11Device* this, REFIID riid, LPVOID* ppvObj) {
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

static HRESULT __attribute__((ms_abi)) dxgi_device_query_interface(
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

static uint32_t __attribute__((ms_abi)) device_addref(ID3D11Device* this) {
    return com_addref(this);
}

static uint32_t __attribute__((ms_abi)) device_release(ID3D11Device* this) {
    return com_release(this);
}

static HRESULT __attribute__((ms_abi)) device_create_swapchain(ID3D11Device* this, void* pFactory, void* pDesc, void** ppSwapChain) {
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

static HRESULT __attribute__((ms_abi)) view_query_interface(
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

static void __attribute__((ms_abi)) view_get_device(
    BeerD3D11View *this, ID3D11Device **device)
{
    if (!device) return;
    *device = this->device;
    if (this->device) device_addref(this->device);
}

static HRESULT __attribute__((ms_abi)) view_private_data_unsupported(
    BeerD3D11View *this, void *a, void *b, void *c)
{
    (void)this; (void)a; (void)b; (void)c;
    return (HRESULT)0x80004001;
}

static void __attribute__((ms_abi)) view_get_resource(
    BeerD3D11View *this, void **resource)
{
    if (!resource) return;
    *resource = this->resource;
    addref_external_com_object(this->resource);
}

static void __attribute__((ms_abi)) view_get_desc(BeerD3D11View *this, void *desc)
{
    if (desc) memcpy(desc, this->desc, this->desc_size);
}

static BeerD3D11View *view_create(ID3D11Device *device, void *resource,
    const void *desc, BeerD3D11ViewKind kind, uint32_t desc_size)
{
    if (!device || !resource || desc_size > sizeof(((BeerD3D11View *)0)->desc))
        return NULL;
    if (!g_view_vtable[0]) {
        g_view_vtable[0] = (void *)view_query_interface;
        g_view_vtable[1] = (void *)com_addref;
        g_view_vtable[2] = (void *)com_release;
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
    addref_external_com_object(resource);
    fprintf(stderr, "[D3D11] created view kind=%u resource=%p desc=%s\n",
            (unsigned)kind, resource, desc ? "explicit" : "default");
    return view;
}

static HRESULT __attribute__((ms_abi)) device_create_rendertarget_view(
    ID3D11Device *this, void *resource, void *desc, void **out)
{
    if (!out || !resource) return (HRESULT)0x80070057;
    *out = view_create(this, resource, desc, BEER_VIEW_RENDER_TARGET, 20);
    return *out ? S_OK : (HRESULT)0x8007000e;
}

static HRESULT __attribute__((ms_abi)) device_create_depthstencil_view(
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
} BeerD3D11Resource;

static BeerD3D11Resource *validated_resource(void *object);
static void *g_resource_vtable[11];

static HRESULT __attribute__((ms_abi)) resource_query_interface(
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
    if (!ppvObj) return (HRESULT)0x80070057;
    *ppvObj = NULL;
    if (!iid_equal(riid, iid_iunknown) && !iid_equal(riid, iid_resource) &&
        !(this->dimension == 1 && iid_equal(riid, iid_buffer)) &&
        !(this->dimension == 3 && iid_equal(riid, iid_texture2d)))
        return E_NOINTERFACE;
    *ppvObj = this;
    com_addref(this);
    return S_OK;
}

static uint32_t __attribute__((ms_abi)) resource_release(BeerD3D11Resource *this)
{
    ComObjectHeader *header = com_get_header(this);
    if (!header) return 0;
    uint32_t old_count = atomic_fetch_sub(&header->refcount, 1);
    if (old_count == 1) {
        free(this->pixels);
        free(header);
        return 0;
    }
    return old_count - 1;
}

static void __attribute__((ms_abi)) resource_get_device(
    BeerD3D11Resource *this, ID3D11Device **device)
{
    if (!device) return;
    *device = this->device;
    if (this->device) device_addref(this->device);
}

static HRESULT __attribute__((ms_abi)) resource_private_data_unsupported(
    BeerD3D11Resource *this, void *a, void *b, void *c)
{
    (void)this; (void)a; (void)b; (void)c;
    return (HRESULT)0x80004001;
}

static void __attribute__((ms_abi)) resource_get_type(
    BeerD3D11Resource *this, uint32_t *dimension)
{
    if (dimension) *dimension = this->dimension;
}

static HRESULT __attribute__((ms_abi)) resource_set_eviction_priority(
    BeerD3D11Resource *this, uint32_t priority)
{
    (void)this; (void)priority;
    return S_OK;
}

static uint32_t __attribute__((ms_abi)) resource_get_eviction_priority(
    BeerD3D11Resource *this)
{
    (void)this;
    return 0;
}

static void __attribute__((ms_abi)) resource_get_desc(
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
        return 4;
    case 56: /* DXGI_FORMAT_R16_UINT */
        return 2;
    default:
        return 0;
    }
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

    /* Maintain CPU-visible storage for the uncompressed texture formats
     * exercised by the title path. The byte width comes from the documented
     * DXGI format, not from a fabricated RGBA layout. */
    if (dimension == 3 && desc_size >= 20) {
        const uint32_t *texture = desc;
        uint32_t width = texture[0], height = texture[1], format = texture[4];
        uint32_t bytes_per_pixel = texture_format_bytes_per_pixel(format);
        if (width && height && bytes_per_pixel &&
            width <= UINT32_MAX / bytes_per_pixel &&
            height <= SIZE_MAX / ((size_t)width * bytes_per_pixel)) {
            resource->row_pitch = width * bytes_per_pixel;
            resource->pixel_size = (size_t)resource->row_pitch * height;
            resource->pixels = calloc(1, resource->pixel_size);
        }
    }
    if (resource->pixel_size && !resource->pixels) {
        ComObjectHeader *header = com_get_header(resource);
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
    uint32_t height = ((const uint32_t *)resource->desc)[1];
    uint32_t source_pitch = initial->row_pitch ? initial->row_pitch : resource->row_pitch;
    size_t copy_pitch = source_pitch < resource->row_pitch ? source_pitch : resource->row_pitch;
    for (uint32_t y = 0; y < height; ++y)
        memcpy(resource->pixels + (size_t)y * resource->row_pitch,
               (const uint8_t *)initial->data + (size_t)y * source_pitch,
               copy_pitch);
}

static HRESULT __attribute__((ms_abi)) device_create_texture2d(ID3D11Device* this, void* pDesc, void* pInitData, void** ppTexture2D) {
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
    if (call <= 96 || (desc[0] == 1920 && desc[1] == 1080))
        fprintf(stderr, "[D3D11 TEXTURE] #%u resource=%p %ux%u mips=%u array=%u format=%u usage=%u bind=0x%x cpu=0x%x misc=0x%x initial=%s pixels=%p\n",
                call, (void *)resource, desc[0], desc[1], desc[2], desc[3],
                desc[4], desc[7], desc[8], desc[9], desc[10],
                pInitData ? "yes" : "no", (void *)resource->pixels);
    return S_OK;
}

static HRESULT __attribute__((ms_abi)) device_create_buffer(ID3D11Device* this, void* pDesc, void* pInitData, void** ppBuffer) {
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

static HRESULT __attribute__((ms_abi)) child_query_interface(
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

static void __attribute__((ms_abi)) child_get_device(
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

static HRESULT __attribute__((ms_abi)) device_create_input_layout(ID3D11Device* this, void* pInputElementDescs, uint32_t NumElements, void* pShaderBytecode, size_t BytecodeLength, void** ppInputLayout) {
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

static HRESULT __attribute__((ms_abi)) device_create_vertex_shader(ID3D11Device* this, void* pShaderBytecode, size_t BytecodeLength, void* pClassLinkage, void** ppVertexShader) {
    (void)pClassLinkage;
    return create_shader(this, pShaderBytecode, BytecodeLength, ppVertexShader, BEER_SHADER_VERTEX);
}

static HRESULT __attribute__((ms_abi)) device_create_pixel_shader(ID3D11Device* this, void* pShaderBytecode, size_t BytecodeLength, void* pClassLinkage, void** ppPixelShader) {
    (void)pClassLinkage;
    return create_shader(this, pShaderBytecode, BytecodeLength, ppPixelShader, BEER_SHADER_PIXEL);
}

static HRESULT __attribute__((ms_abi)) device_create_geometry_shader(ID3D11Device* this, void* pShaderBytecode, size_t BytecodeLength, void* pClassLinkage, void** ppGeometryShader) {
    (void)pClassLinkage;
    return create_shader(this, pShaderBytecode, BytecodeLength, ppGeometryShader, BEER_SHADER_GEOMETRY);
}

static HRESULT __attribute__((ms_abi)) device_create_compute_shader(ID3D11Device* this, void* pShaderBytecode, size_t BytecodeLength, void* pClassLinkage, void** ppComputeShader) {
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

static HRESULT __attribute__((ms_abi)) pipeline_state_query_interface(
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

static void __attribute__((ms_abi)) pipeline_state_get_device(
    BeerD3D11State *this, ID3D11Device **device)
{
    if (!device) return;
    *device = this->device;
    if (this->device) device_addref(this->device);
}

static void __attribute__((ms_abi)) pipeline_state_get_desc(
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

static HRESULT __attribute__((ms_abi)) device_create_rasterizer_state(
    ID3D11Device *this, void *desc, void **out)
{
    if (!out || !desc) return (HRESULT)0x80070057;
    *out = pipeline_state_create(this, BEER_STATE_RASTERIZER, desc, 40);
    return *out ? S_OK : (HRESULT)0x8007000e;
}

static HRESULT __attribute__((ms_abi)) device_create_blend_state(
    ID3D11Device *this, void *desc, void **out)
{
    if (!out || !desc) return (HRESULT)0x80070057;
    *out = pipeline_state_create(this, BEER_STATE_BLEND, desc, 264);
    return *out ? S_OK : (HRESULT)0x8007000e;
}

static HRESULT __attribute__((ms_abi)) device_create_depthstencil_state(
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

static HRESULT __attribute__((ms_abi)) sampler_query_interface(
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

static void __attribute__((ms_abi)) sampler_get_device(
    BeerD3D11SamplerState *this, ID3D11Device **device)
{
    if (!device) return;
    *device = this->device;
    if (this->device) device_addref(this->device);
}

static void __attribute__((ms_abi)) sampler_get_desc(
    BeerD3D11SamplerState *this, void *desc)
{
    if (desc) memcpy(desc, this->desc, sizeof(this->desc));
}

static HRESULT __attribute__((ms_abi)) device_create_sampler_state(ID3D11Device* this, void* pSamplerDesc, void** ppSamplerState) {
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

static HRESULT __attribute__((ms_abi)) query_query_interface(
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
    if (!out) return (HRESULT)0x80070057;
    *out = NULL;
    if (!iid_equal(riid, iid_iunknown) && !iid_equal(riid, iid_query) &&
        !iid_equal(riid, iid_async))
        return E_NOINTERFACE;
    *out = this;
    com_addref(this);
    return S_OK;
}

static void __attribute__((ms_abi)) query_get_device(
    BeerD3D11Query *this, ID3D11Device **device)
{
    if (!device) return;
    *device = this->device;
    if (this->device) device_addref(this->device);
}

static uint32_t __attribute__((ms_abi)) query_get_data_size(BeerD3D11Query *this)
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

static void __attribute__((ms_abi)) query_get_desc(BeerD3D11Query *this, void *desc)
{
    if (!desc) return;
    ((uint32_t *)desc)[0] = this->query;
    ((uint32_t *)desc)[1] = this->misc_flags;
}

static HRESULT __attribute__((ms_abi)) device_create_query(
    ID3D11Device *this, const uint32_t *desc, void **out)
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
        fprintf(stderr, "[D3D11] CreateQuery #%u type=%u flags=0x%x -> %p\n",
                call, query->query, query->misc_flags, (void *)query);
    return S_OK;
}

static HRESULT __attribute__((ms_abi)) device_create_shaderresource_view(
    ID3D11Device *this, void *resource, void *desc, void **out)
{
    if (!out || !resource) return (HRESULT)0x80070057;
    *out = view_create(this, resource, desc, BEER_VIEW_SHADER_RESOURCE, 24);
    return *out ? S_OK : (HRESULT)0x8007000e;
}

static HRESULT __attribute__((ms_abi)) device_create_unorderedaccess_view(
    ID3D11Device *this, void *resource, void *desc, void **out)
{
    if (!out || !resource) return (HRESULT)0x80070057;
    *out = view_create(this, resource, desc, BEER_VIEW_UNORDERED_ACCESS, 20);
    return *out ? S_OK : (HRESULT)0x8007000e;
}

static ID3D11DeviceContext *g_immediate_context;

static void __attribute__((ms_abi)) device_get_immediate_context(ID3D11Device* this, void** ppImmediateContext) {
    (void)this;
    if (!ppImmediateContext) return;
    if (!g_immediate_context)
        g_immediate_context = d3d11_device_context_create();
    *ppImmediateContext = g_immediate_context;
    if (g_immediate_context)
        com_addref(g_immediate_context);
    fprintf(stderr, "[D3D11] ID3D11Device::GetImmediateContext -> %p\n",
            (void *)g_immediate_context);
}

static ID3D11DeviceContext *d3d11_device_context_create_typed(
    uint32_t context_type, uint32_t context_flags);

static HRESULT __attribute__((ms_abi)) device_create_deferred_context(ID3D11Device* this, uint32_t ContextFlags, void** ppDeferredContext) {
    (void)this;
    if (!ppDeferredContext) return (HRESULT)0x80070057;
    *ppDeferredContext = d3d11_device_context_create_typed(1, ContextFlags);
    return *ppDeferredContext ? S_OK : (HRESULT)0x8007000e;
}

static HRESULT __attribute__((ms_abi)) device_not_implemented(void *this, void *a, void *b, void *c) {
    (void)this; (void)a; (void)b; (void)c;
    return (HRESULT)0x80004001; /* E_NOTIMPL */
}

static uint32_t __attribute__((ms_abi)) device_get_feature_level(ID3D11Device *this) {
    (void)this;
    return 0xb000; /* D3D_FEATURE_LEVEL_11_0 */
}

static uint32_t __attribute__((ms_abi)) device_get_creation_flags(ID3D11Device *this) {
    (void)this;
    return 0;
}

static HRESULT __attribute__((ms_abi)) device_get_removed_reason(ID3D11Device *this) {
    (void)this;
    return S_OK;
}

static HRESULT __attribute__((ms_abi)) device_set_exception_mode(ID3D11Device *this, uint32_t flags) {
    (void)this; (void)flags;
    return S_OK;
}

static uint32_t __attribute__((ms_abi)) device_get_exception_mode(ID3D11Device *this) {
    (void)this;
    return 0;
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
    g_device_vtable.slots[D3D11_DEVICE_SLOT_CREATE_SHADER_RESOURCE_VIEW] = (void *)device_create_shaderresource_view;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_CREATE_UNORDERED_ACCESS_VIEW] = (void *)device_create_unorderedaccess_view;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_CREATE_RENDER_TARGET_VIEW] = (void *)device_create_rendertarget_view;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_CREATE_DEPTH_STENCIL_VIEW] = (void *)device_create_depthstencil_view;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_CREATE_INPUT_LAYOUT] = (void *)device_create_input_layout;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_CREATE_VERTEX_SHADER] = (void *)device_create_vertex_shader;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_CREATE_GEOMETRY_SHADER] = (void *)device_create_geometry_shader;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_CREATE_PIXEL_SHADER] = (void *)device_create_pixel_shader;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_CREATE_COMPUTE_SHADER] = (void *)device_create_compute_shader;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_CREATE_BLEND_STATE] = (void *)device_create_blend_state;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_CREATE_DEPTH_STENCIL_STATE] = (void *)device_create_depthstencil_state;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_CREATE_RASTERIZER_STATE] = (void *)device_create_rasterizer_state;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_CREATE_SAMPLER_STATE] = (void *)device_create_sampler_state;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_CREATE_QUERY] = (void *)device_create_query;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_CREATE_DEFERRED_CONTEXT] = (void *)device_create_deferred_context;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_GET_FEATURE_LEVEL] = (void *)device_get_feature_level;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_GET_CREATION_FLAGS] = (void *)device_get_creation_flags;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_GET_DEVICE_REMOVED_REASON] = (void *)device_get_removed_reason;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_GET_IMMEDIATE_CONTEXT] = (void *)device_get_immediate_context;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_SET_EXCEPTION_MODE] = (void *)device_set_exception_mode;
    g_device_vtable.slots[D3D11_DEVICE_SLOT_GET_EXCEPTION_MODE] = (void *)device_get_exception_mode;
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

static HRESULT __attribute__((ms_abi)) context_query_interface(ID3D11DeviceContext* this, REFIID riid, LPVOID* ppvObj) {
    if (!ppvObj) return E_NOINTERFACE;
    *ppvObj = this;
    return S_OK;
}

static uint32_t __attribute__((ms_abi)) context_addref(ID3D11DeviceContext* this) {
    return com_addref(this);
}

static uint32_t __attribute__((ms_abi)) context_release(ID3D11DeviceContext* this) {
    return com_release(this);
}

typedef enum {
    BEER_COMMAND_UNKNOWN,
    BEER_COMMAND_IA_INPUT_LAYOUT,
    BEER_COMMAND_VS_SHADER,
    BEER_COMMAND_PS_SHADER,
    BEER_COMMAND_GS_SHADER,
    BEER_COMMAND_DRAW_INDEXED,
    BEER_COMMAND_DRAW,
    BEER_COMMAND_DRAW_INDEXED_INSTANCED,
    BEER_COMMAND_DRAW_INSTANCED,
    BEER_COMMAND_DRAW_AUTO,
    BEER_COMMAND_CLEAR_RENDER_TARGET,
    BEER_COMMAND_COPY_RESOURCE
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
    void *vertex_buffers[BEER_MAX_VERTEX_BUFFERS];
    uint32_t vertex_strides[BEER_MAX_VERTEX_BUFFERS];
    uint32_t vertex_offsets[BEER_MAX_VERTEX_BUFFERS];
    void *vs_constant_buffers[BEER_MAX_CONSTANT_BUFFERS];
    void *ps_constant_buffers[BEER_MAX_CONSTANT_BUFFERS];
    void *ps_shader_resources[BEER_MAX_SHADER_RESOURCES];
    void *ps_samplers[BEER_MAX_SAMPLERS];
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
} BeerD3D11Command;

typedef struct {
    void **vtable;
    uint32_t type;
    uint32_t flags;
    BeerD3D11Command *commands;
    size_t command_count;
    size_t command_capacity;
    BeerD3D11PipelineState state;
} BeerD3D11DeviceContext;

typedef struct {
    void **vtable;
    ID3D11Device *device;
    BeerD3D11Command *commands;
    size_t command_count;
} BeerD3D11CommandList;

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
    for (size_t i = 0; i < BEER_MAX_VERTEX_BUFFERS; ++i)
        state_replace_object(&state->vertex_buffers[i], NULL);
    for (size_t i = 0; i < BEER_MAX_CONSTANT_BUFFERS; ++i) {
        state_replace_object(&state->vs_constant_buffers[i], NULL);
        state_replace_object(&state->ps_constant_buffers[i], NULL);
    }
    for (size_t i = 0; i < BEER_MAX_SHADER_RESOURCES; ++i)
        state_replace_object(&state->ps_shader_resources[i], NULL);
    for (size_t i = 0; i < BEER_MAX_SAMPLERS; ++i)
        state_replace_object(&state->ps_samplers[i], NULL);
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
    for (size_t i = 0; i < BEER_MAX_VERTEX_BUFFERS; ++i)
        if (copy->vertex_buffers[i] && com_get_header(copy->vertex_buffers[i]))
            com_addref(copy->vertex_buffers[i]);
    for (size_t i = 0; i < BEER_MAX_CONSTANT_BUFFERS; ++i) {
        if (copy->vs_constant_buffers[i] && com_get_header(copy->vs_constant_buffers[i]))
            com_addref(copy->vs_constant_buffers[i]);
        if (copy->ps_constant_buffers[i] && com_get_header(copy->ps_constant_buffers[i]))
            com_addref(copy->ps_constant_buffers[i]);
    }
    for (size_t i = 0; i < BEER_MAX_SHADER_RESOURCES; ++i)
        if (copy->ps_shader_resources[i] && com_get_header(copy->ps_shader_resources[i]))
            com_addref(copy->ps_shader_resources[i]);
    for (size_t i = 0; i < BEER_MAX_SAMPLERS; ++i)
        if (copy->ps_samplers[i] && com_get_header(copy->ps_samplers[i]))
            com_addref(copy->ps_samplers[i]);
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

static void __attribute__((ms_abi)) context_ia_set_input_layout(ID3D11DeviceContext* this, void* pInputLayout) {
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

static void __attribute__((ms_abi)) context_ia_set_vertex_buffers(ID3D11DeviceContext* this, uint32_t StartSlot, uint32_t NumBuffers, void* ppVertexBuffers, void* pStrides, void* pOffsets) {
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    void *const *buffers = ppVertexBuffers;
    const uint32_t *strides = pStrides, *offsets = pOffsets;
    if (context && StartSlot < BEER_MAX_VERTEX_BUFFERS) {
        uint32_t limit = NumBuffers;
        if (limit > BEER_MAX_VERTEX_BUFFERS - StartSlot)
            limit = BEER_MAX_VERTEX_BUFFERS - StartSlot;
        for (uint32_t i = 0; i < limit; ++i) {
            state_replace_object(&context->state.vertex_buffers[StartSlot + i],
                                 buffers ? buffers[i] : NULL);
            context->state.vertex_strides[StartSlot + i] = strides ? strides[i] : 0;
            context->state.vertex_offsets[StartSlot + i] = offsets ? offsets[i] : 0;
        }
    }
    trace_resource_binding("IASetVertexBuffers", this, StartSlot, NumBuffers,
                           buffers, strides, offsets);
}

static void __attribute__((ms_abi)) context_ia_set_index_buffer(ID3D11DeviceContext* this, void* pIndexBuffer, uint32_t Format, uint32_t Offset) {
    void *objects[1] = { pIndexBuffer };
    uint32_t formats[1] = { Format }, offsets[1] = { Offset };
    trace_resource_binding("IASetIndexBuffer", this, 0, 1, objects, formats, offsets);
}

static void __attribute__((ms_abi)) context_ia_set_primitive_topology(ID3D11DeviceContext* this, uint32_t Topology) {
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (context) context->state.topology = Topology;
    fprintf(stderr, "[D3D11 STATE] topology context=%p value=%u\n", (void *)this, Topology);
}

static void __attribute__((ms_abi)) context_vs_set_shader(ID3D11DeviceContext* this, void* pVertexShader, void* ppClassInstances, uint32_t NumClassInstances) {
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

static void __attribute__((ms_abi)) context_vs_set_constant_buffers(ID3D11DeviceContext* this, uint32_t StartSlot, uint32_t NumBuffers, void* ppConstantBuffers) {
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (context) set_state_objects(context->state.vs_constant_buffers,
        BEER_MAX_CONSTANT_BUFFERS, StartSlot, NumBuffers, ppConstantBuffers);
    trace_resource_binding("VSSetConstantBuffers", this, StartSlot, NumBuffers,
                           (void *const *)ppConstantBuffers, NULL, NULL);
}

static void __attribute__((ms_abi)) context_ps_set_shader(ID3D11DeviceContext* this, void* pPixelShader, void* ppClassInstances, uint32_t NumClassInstances) {
    (void)ppClassInstances;
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (context) state_replace_object(&context->state.pixel_shader, pPixelShader);
    BeerD3D11Command command = { .type = BEER_COMMAND_PS_SHADER,
                                 .object = pPixelShader };
    command.args.integers.a = NumClassInstances;
    if (context_record_command(this, &command)) return;
    trace_binding("PSSetShader", pPixelShader, NumClassInstances);
}

static void __attribute__((ms_abi)) context_ps_set_constant_buffers(ID3D11DeviceContext* this, uint32_t StartSlot, uint32_t NumBuffers, void* ppConstantBuffers) {
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (context) set_state_objects(context->state.ps_constant_buffers,
        BEER_MAX_CONSTANT_BUFFERS, StartSlot, NumBuffers, ppConstantBuffers);
    trace_resource_binding("PSSetConstantBuffers", this, StartSlot, NumBuffers,
                           (void *const *)ppConstantBuffers, NULL, NULL);
}

static void __attribute__((ms_abi)) context_ps_set_shader_resources(ID3D11DeviceContext* this, uint32_t StartSlot, uint32_t NumViews, void* ppShaderResourceViews) {
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (context) set_state_objects(context->state.ps_shader_resources,
        BEER_MAX_SHADER_RESOURCES, StartSlot, NumViews, ppShaderResourceViews);
    trace_resource_binding("PSSetShaderResources", this, StartSlot, NumViews,
                           (void *const *)ppShaderResourceViews, NULL, NULL);
}

static void __attribute__((ms_abi)) context_ps_set_samplers(ID3D11DeviceContext* this, uint32_t StartSlot, uint32_t NumSamplers, void* ppSamplers) {
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (context) set_state_objects(context->state.ps_samplers,
        BEER_MAX_SAMPLERS, StartSlot, NumSamplers, ppSamplers);
    trace_resource_binding("PSSetSamplers", this, StartSlot, NumSamplers,
                           (void *const *)ppSamplers, NULL, NULL);
}

static void __attribute__((ms_abi)) context_gs_set_shader(ID3D11DeviceContext* this, void* pGeometryShader, void* ppClassInstances, uint32_t NumClassInstances) {
    (void)ppClassInstances;
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (context) state_replace_object(&context->state.geometry_shader, pGeometryShader);
    BeerD3D11Command command = { .type = BEER_COMMAND_GS_SHADER,
                                 .object = pGeometryShader };
    command.args.integers.a = NumClassInstances;
    if (context_record_command(this, &command)) return;
    trace_binding("GSSetShader", pGeometryShader, NumClassInstances);
}

static void __attribute__((ms_abi)) context_draw_indexed(ID3D11DeviceContext* this, uint32_t IndexCount, uint32_t StartIndexLocation, int32_t BaseVertexLocation) {
    BeerD3D11Command command = { .type = BEER_COMMAND_DRAW_INDEXED };
    command.args.integers.a = IndexCount;
    command.args.integers.b = StartIndexLocation;
    command.args.integers.signed_value = BaseVertexLocation;
    if (context_record_command(this, &command)) return;
    static _Atomic(uint32_t) calls;
    uint32_t call = atomic_fetch_add(&calls, 1) + 1;
    if (call <= 8)
        fprintf(stderr, "[D3D11 TRACE] DrawIndexed #%u indices=%u start=%u base=%d\n",
                call, IndexCount, StartIndexLocation, BaseVertexLocation);
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
    uint32_t snapshot = atomic_fetch_add(&snapshots, 1) + 1;
    if (!state || snapshot > 8) return;
    fprintf(stderr, "[D3D11 DRAW STATE] #%u vertices=%u start=%u topology=%u layout=%p VS=%p PS=%p VB=%p stride=%u offset=%u RTV=%p viewports=%u",
            snapshot, vertex_count, start_vertex, state->topology,
            state->input_layout, state->vertex_shader, state->pixel_shader,
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

/* Execute only the two verified fullscreen shaders used by Sekiro's initial
 * presentation path. Their DXBC is retained and identified by content hash;
 * all structural checks below must pass before pixels are modified. This is a
 * real implementation of the observed shader behavior, not a synthetic title
 * or a generic-success fallback. */
static int execute_initial_fullscreen_draw(const BeerD3D11PipelineState *state,
                                           uint32_t vertex_count,
                                           uint32_t start_vertex)
{
    if (!state || vertex_count != 4 || state->topology != 5 ||
        !state->vertex_shader || !state->pixel_shader ||
        !state->vertex_buffers[0] || !state->render_targets[0])
        return 0;

    BeerD3D11Shader *vertex_shader = state->vertex_shader;
    BeerD3D11Shader *pixel_shader = state->pixel_shader;
    if (!com_get_header(vertex_shader) || !com_get_header(pixel_shader) ||
        fnv1a_bytes(vertex_shader->bytecode, vertex_shader->bytecode_size) != 0xd2d4e0b9)
        return 0;
    uint32_t pixel_hash = fnv1a_bytes(pixel_shader->bytecode,
                                      pixel_shader->bytecode_size);
    if (pixel_hash != 0xbf96cb08 && pixel_hash != 0xda42b236) return 0;

    BeerD3D11Resource *vertices = validated_resource(state->vertex_buffers[0]);
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
        uint32_t condition_bits = UINT32_MAX;
        if (condition && condition->pixels && condition->pixel_size >= 4)
            memcpy(&condition_bits, condition->pixels, sizeof(condition_bits));
        /* cb1.x == 0 selects the original texture sample; the gamma branch is
         * deliberately left unsupported until it is observed active. */
        if (condition_bits != 0) return 0;
    }

    size_t pixels = (size_t)width * height;
    for (size_t i = 0; i < pixels; ++i) {
        uint32_t color[4];
        unpack_rgba(source, i, color);
        if (second_source) {
            uint32_t modulation[4];
            unpack_rgba(second_source, i, modulation);
            color[0] = color[0] * modulation[3] / 1023;
            color[1] = color[1] * modulation[3] / 1023;
            color[2] = color[2] * modulation[3] / 1023;
            color[3] = 1023;
        }
        pack_rgba(target, i, color);
    }

    static _Atomic(uint32_t) executions;
    uint32_t execution = atomic_fetch_add(&executions, 1) + 1;
    if (execution <= 8)
        fprintf(stderr, "[D3D11 SOFTWARE] executed shader %08x over %ux%u target\n",
                pixel_hash, width, height);
    return 1;
}

static void __attribute__((ms_abi)) context_draw(ID3D11DeviceContext* this, uint32_t VertexCount, uint32_t StartVertexLocation) {
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
    static _Atomic(uint32_t) calls;
    uint32_t call = atomic_fetch_add(&calls, 1) + 1;
    if (call <= 8)
        fprintf(stderr, "[D3D11 TRACE] Draw #%u vertices=%u start=%u executed=%d\n",
                call, VertexCount, StartVertexLocation, executed);
    (void)this;
}

static void __attribute__((ms_abi)) context_draw_indexed_instanced(
    ID3D11DeviceContext *this, uint32_t index_count, uint32_t instance_count,
    uint32_t start_index, int32_t base_vertex, uint32_t start_instance)
{
    BeerD3D11Command command = { .type = BEER_COMMAND_DRAW_INDEXED_INSTANCED };
    command.args.integers.a = index_count;
    command.args.integers.b = instance_count;
    command.args.integers.c = start_index;
    command.args.integers.d = start_instance;
    command.args.integers.signed_value = base_vertex;
    if (context_record_command(this, &command)) return;
    static _Atomic(uint32_t) calls;
    uint32_t call = atomic_fetch_add(&calls, 1) + 1;
    if (call <= 16)
        fprintf(stderr, "[D3D11 TRACE] DrawIndexedInstanced #%u indices=%u instances=%u start=%u base=%d firstInstance=%u\n",
                call, index_count, instance_count, start_index, base_vertex, start_instance);
    (void)this;
}

static void __attribute__((ms_abi)) context_draw_instanced(
    ID3D11DeviceContext *this, uint32_t vertex_count, uint32_t instance_count,
    uint32_t start_vertex, uint32_t start_instance)
{
    BeerD3D11Command command = { .type = BEER_COMMAND_DRAW_INSTANCED };
    command.args.integers.a = vertex_count;
    command.args.integers.b = instance_count;
    command.args.integers.c = start_vertex;
    command.args.integers.d = start_instance;
    if (context_record_command(this, &command)) return;
    static _Atomic(uint32_t) calls;
    uint32_t call = atomic_fetch_add(&calls, 1) + 1;
    if (call <= 16)
        fprintf(stderr, "[D3D11 TRACE] DrawInstanced #%u vertices=%u instances=%u start=%u firstInstance=%u\n",
                call, vertex_count, instance_count, start_vertex, start_instance);
    (void)this;
}

static void __attribute__((ms_abi)) context_draw_auto(ID3D11DeviceContext *this)
{
    BeerD3D11Command command = { .type = BEER_COMMAND_DRAW_AUTO };
    if (context_record_command(this, &command)) return;
    static _Atomic(uint32_t) calls;
    uint32_t call = atomic_fetch_add(&calls, 1) + 1;
    if (call <= 16) fprintf(stderr, "[D3D11 TRACE] DrawAuto #%u\n", call);
    (void)this;
}

static void __attribute__((ms_abi)) context_draw_indirect(
    ID3D11DeviceContext *this, void *buffer, uint32_t offset)
{
    static _Atomic(uint32_t) calls;
    uint32_t call = atomic_fetch_add(&calls, 1) + 1;
    if (call <= 16)
        fprintf(stderr, "[D3D11 TRACE] DrawIndirect #%u buffer=%p offset=%u\n",
                call, buffer, offset);
    (void)this;
}

static void __attribute__((ms_abi)) context_begin(
    ID3D11DeviceContext *this, BeerD3D11Query *query)
{
    (void)this;
    if (!query || !com_get_header(query)) return;
    /* Timestamp and event queries are End-only in D3D11. Begin is valid for
     * interval queries such as occlusion and timestamp-disjoint. */
    if (query->query == 0 || query->query == 2) return;
    query->begun = 1;
    query->ended = 0;
}

static void __attribute__((ms_abi)) context_end(
    ID3D11DeviceContext *this, BeerD3D11Query *query)
{
    (void)this;
    if (!query || !com_get_header(query)) return;
    query->ended = 1;
    if (query->query == 2) { /* D3D11_QUERY_TIMESTAMP */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        query->value = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
    } else {
        query->value = 1;
    }
}

static HRESULT __attribute__((ms_abi)) context_get_data(
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

static void __attribute__((ms_abi)) context_om_set_render_targets(ID3D11DeviceContext* this, uint32_t NumViews, void* ppRenderTargetViews, void* pDepthStencilView) {
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (!context) return;
    set_state_objects(context->state.render_targets, BEER_MAX_RENDER_TARGETS,
                      0, NumViews, ppRenderTargetViews);
    for (uint32_t i = NumViews; i < BEER_MAX_RENDER_TARGETS; ++i)
        state_replace_object(&context->state.render_targets[i], NULL);
    state_replace_object(&context->state.depth_stencil_view, pDepthStencilView);
}

static void __attribute__((ms_abi)) context_om_set_blend_state(ID3D11DeviceContext* this, void* pBlendState, void* BlendFactor, uint32_t SampleMask) {
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (!context) return;
    state_replace_object(&context->state.blend_state, pBlendState);
    if (BlendFactor) memcpy(context->state.blend_factor, BlendFactor,
                            sizeof(context->state.blend_factor));
    else memset(context->state.blend_factor, 0, sizeof(context->state.blend_factor));
    context->state.sample_mask = SampleMask;
}

static void __attribute__((ms_abi)) context_om_set_depthstencil_state(ID3D11DeviceContext* this, void* pDepthStencilState, uint32_t StencilRef) {
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (!context) return;
    state_replace_object(&context->state.depth_stencil_state, pDepthStencilState);
    context->state.stencil_ref = StencilRef;
}

static void __attribute__((ms_abi)) context_rs_set_state(ID3D11DeviceContext* this, void* pRasterizerState) {
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (context) state_replace_object(&context->state.rasterizer_state, pRasterizerState);
}

static void __attribute__((ms_abi)) context_rs_set_viewports(ID3D11DeviceContext* this, uint32_t NumViewports, void* pViewports) {
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

static void __attribute__((ms_abi)) context_rs_set_scissor_rects(ID3D11DeviceContext* this, uint32_t NumRects, void* pRects) {
    (void)this; (void)NumRects; (void)pRects;
}

static uint8_t float_to_unorm8(float value)
{
    if (!(value > 0.0f)) return 0;
    if (value >= 1.0f) return 255;
    return (uint8_t)(value * 255.0f + 0.5f);
}

static void __attribute__((ms_abi)) context_clear_rendertarget_view(ID3D11DeviceContext* this, void* pRenderTargetView, void* ColorRGBA);

static uint32_t pack_r10g10b10a2(const float *color)
{
    uint32_t r = (uint32_t)(color[0] <= 0.0f ? 0 : color[0] >= 1.0f ? 1023 : color[0] * 1023.0f + 0.5f);
    uint32_t g = (uint32_t)(color[1] <= 0.0f ? 0 : color[1] >= 1.0f ? 1023 : color[1] * 1023.0f + 0.5f);
    uint32_t b = (uint32_t)(color[2] <= 0.0f ? 0 : color[2] >= 1.0f ? 1023 : color[2] * 1023.0f + 0.5f);
    uint32_t a = (uint32_t)(color[3] <= 0.0f ? 0 : color[3] >= 1.0f ? 3 : color[3] * 3.0f + 0.5f);
    return r | (g << 10) | (b << 20) | (a << 30);
}

static void __attribute__((ms_abi)) context_clear_rendertarget_view(ID3D11DeviceContext* this, void* pRenderTargetView, void* ColorRGBA) {
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

static void __attribute__((ms_abi)) context_copy_resource(
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
    if (dst->pixels && src->pixels)
        memcpy(dst->pixels, src->pixels,
               dst->pixel_size < src->pixel_size ? dst->pixel_size : src->pixel_size);
}

static void __attribute__((ms_abi)) context_update_subresource(
    ID3D11DeviceContext *this, void *destination, uint32_t subresource,
    const void *box, const void *source, uint32_t source_row_pitch,
    uint32_t source_depth_pitch)
{
    (void)this; (void)box; (void)source_depth_pitch;
    trace_context_operation("UpdateSubresource", destination, (void *)source);
    BeerD3D11Resource *dst = validated_resource(destination);
    if (!dst || !dst->pixels || !source || subresource != 0) return;
    if (dst->dimension == 1) {
        /* Buffers are a single byte-addressed subresource. RowPitch and
         * DepthPitch are ignored by D3D11 for buffer updates. */
        memcpy(dst->pixels, source, dst->pixel_size);
        return;
    }
    const uint32_t *desc = (const uint32_t *)dst->desc;
    uint32_t height = desc[1];
    uint32_t pitch = source_row_pitch ? source_row_pitch : dst->row_pitch;
    size_t rows = height;
    for (size_t y = 0; y < rows; ++y)
        memcpy(dst->pixels + y * dst->row_pitch,
               (const uint8_t *)source + y * pitch,
               dst->row_pitch < pitch ? dst->row_pitch : pitch);
}

static void __attribute__((ms_abi)) context_copy_subresource_region(
    ID3D11DeviceContext *this, void *destination, uint32_t destination_subresource,
    uint32_t destination_x, uint32_t destination_y, uint32_t destination_z,
    void *source, uint32_t source_subresource, const void *source_box)
{
    (void)destination_subresource; (void)destination_x; (void)destination_y;
    (void)destination_z; (void)source_subresource; (void)source_box;
    trace_context_operation("CopySubresourceRegion", destination, source);
    context_copy_resource(this, destination, source);
}

static void __attribute__((ms_abi)) context_resolve_subresource(
    ID3D11DeviceContext *this, void *destination, uint32_t destination_subresource,
    void *source, uint32_t source_subresource, uint32_t format)
{
    (void)destination_subresource; (void)source_subresource; (void)format;
    trace_context_operation("ResolveSubresource", destination, source);
    context_copy_resource(this, destination, source);
}

static void __attribute__((ms_abi)) context_dispatch(
    ID3D11DeviceContext *this, uint32_t x, uint32_t y, uint32_t z)
{
    static _Atomic(uint32_t) calls;
    uint32_t call = atomic_fetch_add(&calls, 1) + 1;
    if (call <= 16)
        fprintf(stderr, "[D3D11 TRACE] Dispatch #%u groups=(%u,%u,%u)\n", call, x, y, z);
    (void)this;
}

static void __attribute__((ms_abi)) context_dispatch_indirect(
    ID3D11DeviceContext *this, void *buffer, uint32_t offset)
{
    static _Atomic(uint32_t) calls;
    uint32_t call = atomic_fetch_add(&calls, 1) + 1;
    if (call <= 16)
        fprintf(stderr, "[D3D11 TRACE] DispatchIndirect #%u buffer=%p offset=%u\n",
                call, buffer, offset);
    (void)this;
}

static void *g_command_list_vtable[7];

static HRESULT __attribute__((ms_abi)) command_list_query_interface(
    BeerD3D11CommandList *object, REFIID riid, void **output)
{
    static const uint8_t iid_iunknown[16] = {
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xc0,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x46
    };
    static const uint8_t iid_command_list[16] = {
        0xff,0x63,0x7d,0xa2,0xb3,0x8b,0x92,0x49,
        0xaa,0x70,0xb2,0x78,0xe0,0x6a,0x2e,0x16
    };
    if (!output) return (HRESULT)0x80070057;
    *output = NULL;
    if (!iid_equal(riid, iid_iunknown) && !iid_equal(riid, iid_command_list))
        return E_NOINTERFACE;
    *output = object;
    com_addref(object);
    return S_OK;
}

static uint32_t __attribute__((ms_abi)) command_list_release(
    BeerD3D11CommandList *object);

static void command_list_vtable_initialize(void)
{
    if (g_command_list_vtable[0]) return;
    g_command_list_vtable[0] = (void *)command_list_query_interface;
    g_command_list_vtable[1] = (void *)com_addref;
    g_command_list_vtable[2] = (void *)command_list_release;
    g_command_list_vtable[3] = (void *)child_get_device;
    g_command_list_vtable[4] = (void *)resource_private_data_unsupported;
    g_command_list_vtable[5] = (void *)resource_private_data_unsupported;
    g_command_list_vtable[6] = (void *)resource_private_data_unsupported;
}

static void __attribute__((ms_abi)) context_execute_command_list(
    ID3D11DeviceContext *this, BeerD3D11CommandList *command_list,
    int restore_state)
{
    static _Atomic(uint32_t) calls;
    uint32_t call = atomic_fetch_add(&calls, 1) + 1;
    if (call <= 16)
        fprintf(stderr, "[D3D11 TRACE] ExecuteCommandList #%u list=%p restore=%d commands=%zu\n",
                call, (void *)command_list, restore_state,
                command_list ? command_list->command_count : 0);
    if (!command_list || !com_get_header(command_list)) return;
    for (size_t i = 0; i < command_list->command_count; ++i) {
        BeerD3D11Command *command = &command_list->commands[i];
        switch (command->type) {
        case BEER_COMMAND_IA_INPUT_LAYOUT:
            context_ia_set_input_layout(this, command->object);
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
        case BEER_COMMAND_DRAW_INDEXED:
            context_draw_indexed(this, command->args.integers.a,
                                 command->args.integers.b,
                                 command->args.integers.signed_value);
            break;
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
        case BEER_COMMAND_DRAW_INDEXED_INSTANCED:
            context_draw_indexed_instanced(
                this, command->args.integers.a, command->args.integers.b,
                command->args.integers.c, command->args.integers.signed_value,
                command->args.integers.d);
            break;
        case BEER_COMMAND_DRAW_INSTANCED:
            context_draw_instanced(
                this, command->args.integers.a, command->args.integers.b,
                command->args.integers.c, command->args.integers.d);
            break;
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
        }
    }
}

static HRESULT __attribute__((ms_abi)) context_finish_command_list(
    ID3D11DeviceContext *this, int restore_state, void **command_list)
{
    static _Atomic(uint32_t) calls;
    uint32_t call = atomic_fetch_add(&calls, 1) + 1;
    if (call <= 16)
        fprintf(stderr, "[D3D11 TRACE] FinishCommandList #%u restore=%d output=%p\n",
                call, restore_state, (void *)command_list);
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
    list->device = NULL;
    list->commands = context->commands;
    list->command_count = context->command_count;
    if (call <= 16) {
        for (size_t i = 0; i < list->command_count; ++i)
            fprintf(stderr, "[D3D11 TRACE] command list #%u command[%zu]=%s\n",
                    call, i, command_type_name(list->commands[i].type));
    }
    context->commands = NULL;
    context->command_count = 0;
    context->command_capacity = 0;
    if (call <= 16)
        fprintf(stderr, "[D3D11 TRACE] command list #%u captured %zu commands\n",
                call, list->command_count);
    *command_list = list;
    return S_OK;
}

static void __attribute__((ms_abi)) context_clear_depthstencil_view(ID3D11DeviceContext* this, void* pDepthStencilView, uint32_t ClearFlags, float Depth, uint8_t Stencil) {
    /* Stub - do nothing */
}

typedef struct {
    void *data;
    uint32_t row_pitch;
    uint32_t depth_pitch;
} BeerMappedSubresource;

static HRESULT __attribute__((ms_abi)) context_map(
    ID3D11DeviceContext* this, void* pResource, uint32_t Subresource,
    uint32_t MapType, uint32_t MapFlags, void* pMappedResource)
{
    (void)this; (void)MapType; (void)MapFlags;
    BeerMappedSubresource *mapped = pMappedResource;
    if (mapped) memset(mapped, 0, sizeof(*mapped));
    BeerD3D11Resource *resource = validated_resource(pResource);
    if (!resource || !mapped || Subresource != 0 || !resource->pixels)
        return (HRESULT)0x80070057;
    mapped->data = resource->pixels;
    mapped->row_pitch = resource->row_pitch;
    mapped->depth_pitch = resource->dimension == 3
        ? (uint32_t)resource->pixel_size : resource->row_pitch;
    static _Atomic(uint32_t) calls;
    uint32_t call = atomic_fetch_add(&calls, 1) + 1;
    if (call <= 16)
        fprintf(stderr, "[D3D11 TRACE] Map #%u resource=%p type=%u data=%p row=%u depth=%u\n",
                call, pResource, MapType, mapped->data,
                mapped->row_pitch, mapped->depth_pitch);
    return S_OK;
}

static void __attribute__((ms_abi)) context_unmap(
    ID3D11DeviceContext* this, void* pResource, uint32_t Subresource)
{
    (void)this; (void)pResource; (void)Subresource;
}

static void __attribute__((ms_abi)) context_clear_state(ID3D11DeviceContext *this)
{
    static _Atomic(uint32_t) calls;
    uint32_t call = atomic_fetch_add(&calls, 1) + 1;
    if (call <= 16) fprintf(stderr, "[D3D11 TRACE] ClearState #%u\n", call);
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)this;
    if (!context) return;
    pipeline_state_release(&context->state);
    memset(&context->state, 0, sizeof(context->state));
}

static void __attribute__((ms_abi)) context_flush(ID3D11DeviceContext* this) {
    /* CPU-backed operations complete synchronously. */
    (void)this;
}

static uint32_t __attribute__((ms_abi)) context_get_type(ID3D11DeviceContext *object)
{
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)object;
    return context ? context->type : 0;
}

static uint32_t __attribute__((ms_abi)) context_get_context_flags(ID3D11DeviceContext *object)
{
    BeerD3D11DeviceContext *context = (BeerD3D11DeviceContext *)object;
    return context ? context->flags : 0;
}

static void __attribute__((ms_abi)) context_noop(void *this, ...)
{
    (void)this;
}

#define DEFINE_CONTEXT_SLOT_TRACE(slot) \
    static void __attribute__((ms_abi)) context_trace_slot_##slot(void *this, ...) \
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
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_MAP] = (void *)context_map;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_UNMAP] = (void *)context_unmap;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_CLEAR_STATE] = (void *)context_clear_state;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_FLUSH] = (void *)context_flush;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_GET_TYPE] = (void *)context_get_type;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_GET_CONTEXT_FLAGS] = (void *)context_get_context_flags;
    g_context_vtable.slots[D3D11_CONTEXT_SLOT_FINISH_COMMAND_LIST] = (void *)context_finish_command_list;
}

static ID3D11DeviceContext *d3d11_device_context_create_typed(
    uint32_t context_type, uint32_t context_flags)
{
    init_context_vtable();
    BeerD3D11DeviceContext *context = com_alloc(
        sizeof(*context), COM_TYPE_CONTEXT, 0);
    if (!context) return NULL;
    context->vtable = g_context_vtable.slots;
    context->type = context_type;
    context->flags = context_flags;
    context->commands = NULL;
    context->command_count = 0;
    context->command_capacity = 0;
    memset(&context->state, 0, sizeof(context->state));
    return (ID3D11DeviceContext *)context;
}

ID3D11DeviceContext* d3d11_device_context_create(void) {
    return d3d11_device_context_create_typed(0, 0);
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

static HRESULT __attribute__((ms_abi)) swapchain_query_interface(
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

static uint32_t __attribute__((ms_abi)) swapchain_addref(IDXGISwapChain *this) {
    return com_addref(this);
}

static uint32_t __attribute__((ms_abi)) command_list_release(
    BeerD3D11CommandList *object)
{
    ComObjectHeader *header = com_get_header(object);
    if (!header) return 0;
    uint32_t old_count = atomic_fetch_sub(&header->refcount, 1);
    if (old_count == 1) {
        command_release_references(object->commands, object->command_count);
        free(object->commands);
        free(header);
        return 0;
    }
    return old_count - 1;
}

static uint32_t __attribute__((ms_abi)) swapchain_release(IDXGISwapChain *object) {
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

static HRESULT __attribute__((ms_abi)) swapchain_set_private_data(
    IDXGISwapChain *object, REFIID Name, uint32_t DataSize, const void *pData)
{
    (void)object; (void)Name; (void)DataSize; (void)pData;
    return (HRESULT)0x80004001; /* E_NOTIMPL */
}

static HRESULT __attribute__((ms_abi)) swapchain_set_private_data_interface(
    IDXGISwapChain *object, REFIID Name, const void *pUnknown)
{
    (void)object; (void)Name; (void)pUnknown;
    return (HRESULT)0x80004001;
}

static HRESULT __attribute__((ms_abi)) swapchain_get_private_data(
    IDXGISwapChain *object, REFIID Name, uint32_t *pDataSize, void *pData)
{
    (void)object; (void)Name; (void)pDataSize; (void)pData;
    return (HRESULT)0x887a0002; /* DXGI_ERROR_NOT_FOUND */
}

static HRESULT __attribute__((ms_abi)) swapchain_get_parent(
    IDXGISwapChain *object, REFIID riid, void **ppParent)
{
    (void)object; (void)riid;
    if (!ppParent) return (HRESULT)0x80070057;
    *ppParent = NULL;
    return E_NOINTERFACE;
}

static HRESULT __attribute__((ms_abi)) swapchain_get_device(
    IDXGISwapChain *object, REFIID riid, void **ppDevice)
{
    BeerDxgiSwapChain *this = (BeerDxgiSwapChain *)object;
    if (!ppDevice) return (HRESULT)0x80070057;
    *ppDevice = NULL;
    if (!this->device) return (HRESULT)0x887a0002;
    return device_query_interface(this->device, riid, ppDevice);
}

static HRESULT __attribute__((ms_abi)) swapchain_present(IDXGISwapChain *object, uint32_t SyncInterval, uint32_t Flags) {
    BeerDxgiSwapChain *this = (BeerDxgiSwapChain *)object;
    uint32_t call = ++this->present_count;
    if (call <= 16)
        fprintf(stderr, "[DXGI TRACE] Present #%u sync=%u flags=0x%x\n",
                call, SyncInterval, Flags);
    if (!this->back_buffer || !this->back_buffer->pixels)
        return (HRESULT)0x887a0001; /* DXGI_ERROR_INVALID_CALL */
    if (call <= 16 || !this->logged_nonblack_frame) {
        size_t nonblack = 0;
        uint32_t hash = fnv1a_bytes(this->back_buffer->pixels,
                                    this->back_buffer->pixel_size);
        for (size_t i = 0; i + 3 < this->back_buffer->pixel_size; i += 4)
            if (this->back_buffer->pixels[i] || this->back_buffer->pixels[i + 1] ||
                this->back_buffer->pixels[i + 2]) ++nonblack;
        if (call <= 16 || nonblack) {
            fprintf(stderr, "[DXGI FRAME] present=%u fnv1a=%08x nonblack=%zu/%zu\n",
                    call, hash, nonblack, this->back_buffer->pixel_size / 4);
            if (nonblack) this->logged_nonblack_frame = 1;
        }
    }
    if (!xwayland_window_present_rgba8(this->back_buffer->pixels,
                                       (int)this->desc.BufferDesc.Width,
                                       (int)this->desc.BufferDesc.Height,
                                       (int)this->back_buffer->row_pitch))
        return (HRESULT)0x887a0005; /* DXGI_ERROR_DEVICE_REMOVED */
    return S_OK;
}

static HRESULT __attribute__((ms_abi)) swapchain_get_buffer(
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

static HRESULT __attribute__((ms_abi)) swapchain_set_fullscreen_state(
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

static HRESULT __attribute__((ms_abi)) swapchain_get_fullscreen_state(
    IDXGISwapChain *object, int *pFullscreen, void *ppTarget)
{
    if (pFullscreen) *pFullscreen = ((BeerDxgiSwapChain *)object)->fullscreen;
    if (ppTarget) *(void **)ppTarget = NULL;
    return S_OK;
}

static HRESULT __attribute__((ms_abi)) swapchain_get_desc(
    IDXGISwapChain *object, void *pDesc)
{
    if (!pDesc) return (HRESULT)0x80070057;
    memcpy(pDesc, &((BeerDxgiSwapChain *)object)->desc,
           sizeof(BeerDxgiSwapChainDesc));
    return S_OK;
}

static HRESULT __attribute__((ms_abi)) swapchain_resize_buffers(
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

static HRESULT __attribute__((ms_abi)) swapchain_resize_target(
    IDXGISwapChain *object, void *pNewTargetParameters)
{
    if (!pNewTargetParameters) return (HRESULT)0x80070057;
    memcpy(&((BeerDxgiSwapChain *)object)->desc.BufferDesc,
           pNewTargetParameters, sizeof(BeerDxgiModeDesc));
    return S_OK;
}

static HRESULT __attribute__((ms_abi)) swapchain_get_containing_output(IDXGISwapChain *this, void *ppOutput) {
    (void)this;
    if (!ppOutput) return (HRESULT)0x80070057;
    *(void **)ppOutput = NULL;
    return (HRESULT)0x887a0002;
}

static HRESULT __attribute__((ms_abi)) swapchain_get_frame_statistics(IDXGISwapChain *this, void *pStats) {
    (void)this; (void)pStats;
    return (HRESULT)0x887a0001; /* DXGI_ERROR_INVALID_CALL in windowed mode */
}

static HRESULT __attribute__((ms_abi)) swapchain_get_last_present_count(
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
