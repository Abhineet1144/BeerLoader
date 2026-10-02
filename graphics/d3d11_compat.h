#ifndef D3D11_COMPAT_H
#define D3D11_COMPAT_H

#include <stdint.h>
#include <stddef.h>

/* Minimal D3D11 COM object definitions for Sekiro compatibility.
 * These are stub implementations that return S_OK without doing anything.
 * Goal: Pass game-init checks that verify graphics objects exist and have valid vtables.
 */

#define S_OK 0x00000000
#define E_NOINTERFACE 0x80004002

#if defined(__x86_64__) && defined(__GNUC__)
#define D3D11_MS_ABI __attribute__((ms_abi))
#else
#define D3D11_MS_ABI
#endif

typedef int32_t HRESULT;
typedef uint64_t REFIID;  /* Simplified: just a pointer, not full IID */
typedef void* LPVOID;

/* ============================================================================
 * ID3D11Device - Main graphics device object
 * ============================================================================ */

typedef struct ID3D11Device {
    /* VTable pointer (filled by implementation) */
    void** vtable;
} ID3D11Device;

enum {
    D3D11_DEVICE_SLOT_QUERY_INTERFACE = 0,
    D3D11_DEVICE_SLOT_ADD_REF = 1,
    D3D11_DEVICE_SLOT_RELEASE = 2,
    D3D11_DEVICE_SLOT_CREATE_BUFFER = 3,
    D3D11_DEVICE_SLOT_CREATE_TEXTURE2D = 5,
    D3D11_DEVICE_SLOT_CREATE_SHADER_RESOURCE_VIEW = 7,
    D3D11_DEVICE_SLOT_CREATE_UNORDERED_ACCESS_VIEW = 8,
    D3D11_DEVICE_SLOT_CREATE_RENDER_TARGET_VIEW = 9,
    D3D11_DEVICE_SLOT_CREATE_DEPTH_STENCIL_VIEW = 10,
    D3D11_DEVICE_SLOT_CREATE_INPUT_LAYOUT = 11,
    D3D11_DEVICE_SLOT_CREATE_VERTEX_SHADER = 12,
    D3D11_DEVICE_SLOT_CREATE_GEOMETRY_SHADER = 13,
    D3D11_DEVICE_SLOT_CREATE_PIXEL_SHADER = 15,
    D3D11_DEVICE_SLOT_CREATE_COMPUTE_SHADER = 18,
    D3D11_DEVICE_SLOT_CREATE_BLEND_STATE = 20,
    D3D11_DEVICE_SLOT_CREATE_DEPTH_STENCIL_STATE = 21,
    D3D11_DEVICE_SLOT_CREATE_RASTERIZER_STATE = 22,
    D3D11_DEVICE_SLOT_CREATE_SAMPLER_STATE = 23,
    D3D11_DEVICE_SLOT_CREATE_QUERY = 24,
    D3D11_DEVICE_SLOT_CREATE_DEFERRED_CONTEXT = 27,
    D3D11_DEVICE_SLOT_GET_FEATURE_LEVEL = 37,
    D3D11_DEVICE_SLOT_GET_CREATION_FLAGS = 38,
    D3D11_DEVICE_SLOT_GET_DEVICE_REMOVED_REASON = 39,
    D3D11_DEVICE_SLOT_GET_IMMEDIATE_CONTEXT = 40,
    D3D11_DEVICE_SLOT_SET_EXCEPTION_MODE = 41,
    D3D11_DEVICE_SLOT_GET_EXCEPTION_MODE = 42,
    D3D11_DEVICE_VTABLE_SLOTS = 43
};

typedef struct ID3D11Device_VTable {
    void *slots[D3D11_DEVICE_VTABLE_SLOTS];
} ID3D11Device_VTable;

/* ============================================================================
 * ID3D11DeviceContext - Deferred/Immediate rendering context
 * ============================================================================ */

typedef struct ID3D11DeviceContext {
    void** vtable;
} ID3D11DeviceContext;

enum {
    D3D11_CONTEXT_SLOT_QUERY_INTERFACE = 0,
    D3D11_CONTEXT_SLOT_ADD_REF = 1,
    D3D11_CONTEXT_SLOT_RELEASE = 2,
    D3D11_CONTEXT_SLOT_VS_SET_CONSTANT_BUFFERS = 7,
    D3D11_CONTEXT_SLOT_PS_SET_SHADER_RESOURCES = 8,
    D3D11_CONTEXT_SLOT_PS_SET_SHADER = 9,
    D3D11_CONTEXT_SLOT_PS_SET_SAMPLERS = 10,
    D3D11_CONTEXT_SLOT_VS_SET_SHADER = 11,
    D3D11_CONTEXT_SLOT_DRAW_INDEXED = 12,
    D3D11_CONTEXT_SLOT_DRAW = 13,
    D3D11_CONTEXT_SLOT_MAP = 14,
    D3D11_CONTEXT_SLOT_UNMAP = 15,
    D3D11_CONTEXT_SLOT_PS_SET_CONSTANT_BUFFERS = 16,
    D3D11_CONTEXT_SLOT_IA_SET_INPUT_LAYOUT = 17,
    D3D11_CONTEXT_SLOT_IA_SET_VERTEX_BUFFERS = 18,
    D3D11_CONTEXT_SLOT_IA_SET_INDEX_BUFFER = 19,
    D3D11_CONTEXT_SLOT_DRAW_INDEXED_INSTANCED = 20,
    D3D11_CONTEXT_SLOT_DRAW_INSTANCED = 21,
    D3D11_CONTEXT_SLOT_GS_SET_SHADER = 23,
    D3D11_CONTEXT_SLOT_IA_SET_PRIMITIVE_TOPOLOGY = 24,
    D3D11_CONTEXT_SLOT_BEGIN = 27,
    D3D11_CONTEXT_SLOT_END = 28,
    D3D11_CONTEXT_SLOT_GET_DATA = 29,
    D3D11_CONTEXT_SLOT_DRAW_AUTO = 38,
    D3D11_CONTEXT_SLOT_DRAW_INDEXED_INSTANCED_INDIRECT = 39,
    D3D11_CONTEXT_SLOT_DRAW_INSTANCED_INDIRECT = 40,
    D3D11_CONTEXT_SLOT_DISPATCH = 41,
    D3D11_CONTEXT_SLOT_DISPATCH_INDIRECT = 42,
    D3D11_CONTEXT_SLOT_OM_SET_RENDER_TARGETS = 33,
    D3D11_CONTEXT_SLOT_OM_SET_BLEND_STATE = 35,
    D3D11_CONTEXT_SLOT_OM_SET_DEPTH_STENCIL_STATE = 36,
    D3D11_CONTEXT_SLOT_RS_SET_STATE = 43,
    D3D11_CONTEXT_SLOT_RS_SET_VIEWPORTS = 44,
    D3D11_CONTEXT_SLOT_RS_SET_SCISSOR_RECTS = 45,
    D3D11_CONTEXT_SLOT_COPY_SUBRESOURCE_REGION = 46,
    D3D11_CONTEXT_SLOT_COPY_RESOURCE = 47,
    D3D11_CONTEXT_SLOT_UPDATE_SUBRESOURCE = 48,
    D3D11_CONTEXT_SLOT_CLEAR_RENDER_TARGET_VIEW = 50,
    D3D11_CONTEXT_SLOT_CLEAR_DEPTH_STENCIL_VIEW = 53,
    D3D11_CONTEXT_SLOT_RESOLVE_SUBRESOURCE = 57,
    D3D11_CONTEXT_SLOT_EXECUTE_COMMAND_LIST = 58,
    D3D11_CONTEXT_SLOT_CLEAR_STATE = 110,
    D3D11_CONTEXT_SLOT_FLUSH = 111,
    D3D11_CONTEXT_SLOT_GET_TYPE = 112,
    D3D11_CONTEXT_SLOT_GET_CONTEXT_FLAGS = 113,
    D3D11_CONTEXT_SLOT_FINISH_COMMAND_LIST = 114,
    D3D11_CONTEXT_VTABLE_SLOTS = 115
};

typedef struct ID3D11DeviceContext_VTable {
    void *slots[D3D11_CONTEXT_VTABLE_SLOTS];
} ID3D11DeviceContext_VTable;

/* ============================================================================
 * IDXGISwapChain - Display swapchain
 * ============================================================================ */

typedef struct IDXGISwapChain {
    void** vtable;
} IDXGISwapChain;

typedef struct {
    uint32_t Numerator;
    uint32_t Denominator;
} BeerDxgiRational;

typedef struct {
    uint32_t Width;
    uint32_t Height;
    BeerDxgiRational RefreshRate;
    uint32_t Format;
    uint32_t ScanlineOrdering;
    uint32_t Scaling;
} BeerDxgiModeDesc;

typedef struct {
    uint32_t Count;
    uint32_t Quality;
} BeerDxgiSampleDesc;

typedef struct {
    BeerDxgiModeDesc BufferDesc;
    BeerDxgiSampleDesc SampleDesc;
    uint32_t BufferUsage;
    uint32_t BufferCount;
    void *OutputWindow;
    int32_t Windowed;
    uint32_t SwapEffect;
    uint32_t Flags;
} BeerDxgiSwapChainDesc;

_Static_assert(sizeof(BeerDxgiSwapChainDesc) == 72,
               "DXGI_SWAP_CHAIN_DESC must use the Windows x64 layout");

typedef struct IDXGISwapChain_VTable {
    /* IUnknown */
    HRESULT (D3D11_MS_ABI *QueryInterface)(IDXGISwapChain* this, REFIID riid, LPVOID* ppvObj);
    uint32_t (D3D11_MS_ABI *AddRef)(IDXGISwapChain* this);
    uint32_t (D3D11_MS_ABI *Release)(IDXGISwapChain* this);

    /* IDXGIObject */
    HRESULT (D3D11_MS_ABI *SetPrivateData)(IDXGISwapChain* this, REFIID Name, uint32_t DataSize, const void* pData);
    HRESULT (D3D11_MS_ABI *SetPrivateDataInterface)(IDXGISwapChain* this, REFIID Name, const void* pUnknown);
    HRESULT (D3D11_MS_ABI *GetPrivateData)(IDXGISwapChain* this, REFIID Name, uint32_t* pDataSize, void* pData);
    HRESULT (D3D11_MS_ABI *GetParent)(IDXGISwapChain* this, REFIID riid, void** ppParent);

    /* IDXGIDeviceSubObject */
    HRESULT (D3D11_MS_ABI *GetDevice)(IDXGISwapChain* this, REFIID riid, void** ppDevice);

    /* IDXGISwapChain */
    HRESULT (D3D11_MS_ABI *Present)(IDXGISwapChain* this, uint32_t SyncInterval, uint32_t Flags);
    HRESULT (D3D11_MS_ABI *GetBuffer)(IDXGISwapChain* this, uint32_t Buffer, REFIID riid, LPVOID* ppSurface);
    HRESULT (D3D11_MS_ABI *SetFullscreenState)(IDXGISwapChain* this, int Fullscreen, void* pTarget);
    HRESULT (D3D11_MS_ABI *GetFullscreenState)(IDXGISwapChain* this, int* pFullscreen, void* ppTarget);
    HRESULT (D3D11_MS_ABI *GetDesc)(IDXGISwapChain* this, void* pDesc);
    HRESULT (D3D11_MS_ABI *ResizeBuffers)(IDXGISwapChain* this, uint32_t BufferCount, uint32_t Width, uint32_t Height, uint32_t NewFormat, uint32_t SwapChainFlags);
    HRESULT (D3D11_MS_ABI *ResizeTarget)(IDXGISwapChain* this, void* pNewTargetParameters);
    HRESULT (D3D11_MS_ABI *GetContainingOutput)(IDXGISwapChain* this, void* ppOutput);
    HRESULT (D3D11_MS_ABI *GetFrameStatistics)(IDXGISwapChain* this, void* pStats);
    HRESULT (D3D11_MS_ABI *GetLastPresentCount)(IDXGISwapChain* this, uint32_t* pLastPresentCount);
} IDXGISwapChain_VTable;

/* ============================================================================
 * Creation functions - return allocated COM objects with filled vtables
 * ============================================================================ */

ID3D11Device* d3d11_device_create(void);
ID3D11DeviceContext* d3d11_device_context_create(void);
IDXGISwapChain* dxgi_swapchain_create(void);
HRESULT dxgi_swapchain_configure(IDXGISwapChain *swapchain,
                                 ID3D11Device *device,
                                 const BeerDxgiSwapChainDesc *desc);

/* Associates the device's IDXGIDevice interface with the adapter exposed by
 * the loader's DXGI factory. The adapter remains owned by the loader. */
void d3d11_device_set_dxgi_adapter(void *adapter);

#endif /* D3D11_COMPAT_H */
