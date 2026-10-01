#include "d3d11_compat.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>

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

static uint32_t com_addref(void* obj) {
    ComObjectHeader* hdr = com_get_header(obj);
    if (!hdr) return 0;
    return atomic_fetch_add(&hdr->refcount, 1) + 1;
}

static uint32_t com_release(void* obj) {
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

static HRESULT device_query_interface(ID3D11Device* this, REFIID riid, LPVOID* ppvObj) {
    if (!ppvObj) return E_NOINTERFACE;
    /* For now, all interfaces just return this (simplified) */
    *ppvObj = this;
    return S_OK;
}

static uint32_t device_addref(ID3D11Device* this) {
    return com_addref(this);
}

static uint32_t device_release(ID3D11Device* this) {
    return com_release(this);
}

static HRESULT device_create_swapchain(ID3D11Device* this, void* pFactory, void* pDesc, void** ppSwapChain) {
    if (!ppSwapChain) return E_NOINTERFACE;
    *ppSwapChain = dxgi_swapchain_create();
    return S_OK;
}

static HRESULT device_create_rendertarget_view(ID3D11Device* this, void* pResource, void* pDesc, void** ppRTView) {
    if (!ppRTView) return E_NOINTERFACE;
    /* Allocate minimal structure */
    void* view = malloc(16);
    if (!view) return 0x80000002;  /* E_OUTOFMEMORY */
    memset(view, 0, 16);
    *ppRTView = view;
    return S_OK;
}

static HRESULT device_create_depthstencil_view(ID3D11Device* this, void* pResource, void* pDesc, void** ppDSView) {
    if (!ppDSView) return E_NOINTERFACE;
    void* view = malloc(16);
    if (!view) return 0x80000002;
    memset(view, 0, 16);
    *ppDSView = view;
    return S_OK;
}

static HRESULT device_create_texture2d(ID3D11Device* this, void* pDesc, void* pInitData, void** ppTexture2D) {
    if (!ppTexture2D) return E_NOINTERFACE;
    void* tex = malloc(64);
    if (!tex) return 0x80000002;
    memset(tex, 0, 64);
    *ppTexture2D = tex;
    return S_OK;
}

static HRESULT device_create_buffer(ID3D11Device* this, void* pDesc, void* pInitData, void** ppBuffer) {
    if (!ppBuffer) return E_NOINTERFACE;
    void* buf = malloc(64);
    if (!buf) return 0x80000002;
    memset(buf, 0, 64);
    *ppBuffer = buf;
    return S_OK;
}

static HRESULT device_create_input_layout(ID3D11Device* this, void* pInputElementDescs, uint32_t NumElements, void* pShaderBytecode, size_t BytecodeLength, void** ppInputLayout) {
    if (!ppInputLayout) return E_NOINTERFACE;
    void* layout = malloc(32);
    if (!layout) return 0x80000002;
    memset(layout, 0, 32);
    *ppInputLayout = layout;
    return S_OK;
}

static HRESULT device_create_vertex_shader(ID3D11Device* this, void* pShaderBytecode, size_t BytecodeLength, void* pClassLinkage, void** ppVertexShader) {
    if (!ppVertexShader) return E_NOINTERFACE;
    void* shader = malloc(32);
    if (!shader) return 0x80000002;
    memset(shader, 0, 32);
    *ppVertexShader = shader;
    return S_OK;
}

static HRESULT device_create_pixel_shader(ID3D11Device* this, void* pShaderBytecode, size_t BytecodeLength, void* pClassLinkage, void** ppPixelShader) {
    if (!ppPixelShader) return E_NOINTERFACE;
    void* shader = malloc(32);
    if (!shader) return 0x80000002;
    memset(shader, 0, 32);
    *ppPixelShader = shader;
    return S_OK;
}

static HRESULT device_create_geometry_shader(ID3D11Device* this, void* pShaderBytecode, size_t BytecodeLength, void* pClassLinkage, void** ppGeometryShader) {
    if (!ppGeometryShader) return E_NOINTERFACE;
    void* shader = malloc(32);
    if (!shader) return 0x80000002;
    memset(shader, 0, 32);
    *ppGeometryShader = shader;
    return S_OK;
}

static HRESULT device_create_compute_shader(ID3D11Device* this, void* pShaderBytecode, size_t BytecodeLength, void* pClassLinkage, void** ppComputeShader) {
    if (!ppComputeShader) return E_NOINTERFACE;
    void* shader = malloc(32);
    if (!shader) return 0x80000002;
    memset(shader, 0, 32);
    *ppComputeShader = shader;
    return S_OK;
}

static HRESULT device_create_rasterizer_state(ID3D11Device* this, void* pRasterizerDesc, void** ppRasterizerState) {
    if (!ppRasterizerState) return E_NOINTERFACE;
    void* state = malloc(32);
    if (!state) return 0x80000002;
    memset(state, 0, 32);
    *ppRasterizerState = state;
    return S_OK;
}

static HRESULT device_create_blend_state(ID3D11Device* this, void* pBlendStateDesc, void** ppBlendState) {
    if (!ppBlendState) return E_NOINTERFACE;
    void* state = malloc(32);
    if (!state) return 0x80000002;
    memset(state, 0, 32);
    *ppBlendState = state;
    return S_OK;
}

static HRESULT device_create_depthstencil_state(ID3D11Device* this, void* pDepthStencilDesc, void** ppDepthStencilState) {
    if (!ppDepthStencilState) return E_NOINTERFACE;
    void* state = malloc(32);
    if (!state) return 0x80000002;
    memset(state, 0, 32);
    *ppDepthStencilState = state;
    return S_OK;
}

static HRESULT device_create_sampler_state(ID3D11Device* this, void* pSamplerDesc, void** ppSamplerState) {
    if (!ppSamplerState) return E_NOINTERFACE;
    void* state = malloc(32);
    if (!state) return 0x80000002;
    memset(state, 0, 32);
    *ppSamplerState = state;
    return S_OK;
}

static HRESULT device_create_shaderresource_view(ID3D11Device* this, void* pResource, void* pDesc, void** ppSRView) {
    if (!ppSRView) return E_NOINTERFACE;
    void* view = malloc(32);
    if (!view) return 0x80000002;
    memset(view, 0, 32);
    *ppSRView = view;
    return S_OK;
}

static HRESULT device_create_unorderedaccess_view(ID3D11Device* this, void* pResource, void* pDesc, void** ppUAView) {
    if (!ppUAView) return E_NOINTERFACE;
    void* view = malloc(32);
    if (!view) return 0x80000002;
    memset(view, 0, 32);
    *ppUAView = view;
    return S_OK;
}

static HRESULT device_get_immediate_context(ID3D11Device* this, void** ppImmediateContext) {
    if (!ppImmediateContext) return E_NOINTERFACE;
    *ppImmediateContext = d3d11_device_context_create();
    return S_OK;
}

static HRESULT device_create_deferred_context(ID3D11Device* this, uint32_t ContextFlags, void** ppDeferredContext) {
    if (!ppDeferredContext) return E_NOINTERFACE;
    *ppDeferredContext = d3d11_device_context_create();
    return S_OK;
}

static ID3D11Device_VTable g_device_vtable = {
    .QueryInterface = device_query_interface,
    .AddRef = device_addref,
    .Release = device_release,
    .CreateSwapChain = device_create_swapchain,
    .CreateRenderTargetView = device_create_rendertarget_view,
    .CreateDepthStencilView = device_create_depthstencil_view,
    .CreateTexture2D = device_create_texture2d,
    .CreateBuffer = device_create_buffer,
    .CreateInputLayout = device_create_input_layout,
    .CreateVertexShader = device_create_vertex_shader,
    .CreatePixelShader = device_create_pixel_shader,
    .CreateGeometryShader = device_create_geometry_shader,
    .CreateComputeShader = device_create_compute_shader,
    .CreateRasterizerState = device_create_rasterizer_state,
    .CreateBlendState = device_create_blend_state,
    .CreateDepthStencilState = device_create_depthstencil_state,
    .CreateSamplerState = device_create_sampler_state,
    .CreateShaderResourceView = device_create_shaderresource_view,
    .CreateUnorderedAccessView = device_create_unorderedaccess_view,
    .GetImmediateContext = device_get_immediate_context,
    .CreateDeferredContext = device_create_deferred_context,
};

ID3D11Device* d3d11_device_create(void) {
    ID3D11Device* device = (ID3D11Device*)com_alloc(sizeof(ID3D11Device), COM_TYPE_DEVICE, 0);
    if (!device) return NULL;
    device->vtable = (void**)&g_device_vtable;
    return device;
}

/* ============================================================================
 * ID3D11DeviceContext - Stub implementation
 * ============================================================================ */

static HRESULT context_query_interface(ID3D11DeviceContext* this, REFIID riid, LPVOID* ppvObj) {
    if (!ppvObj) return E_NOINTERFACE;
    *ppvObj = this;
    return S_OK;
}

static uint32_t context_addref(ID3D11DeviceContext* this) {
    return com_addref(this);
}

static uint32_t context_release(ID3D11DeviceContext* this) {
    return com_release(this);
}

static void context_ia_set_input_layout(ID3D11DeviceContext* this, void* pInputLayout) {
    /* Stub - do nothing */
}

static void context_ia_set_vertex_buffers(ID3D11DeviceContext* this, uint32_t StartSlot, uint32_t NumBuffers, void* ppVertexBuffers, void* pStrides, void* pOffsets) {
    /* Stub - do nothing */
}

static void context_ia_set_index_buffer(ID3D11DeviceContext* this, void* pIndexBuffer, uint32_t Format, uint32_t Offset) {
    /* Stub - do nothing */
}

static void context_ia_set_primitive_topology(ID3D11DeviceContext* this, uint32_t Topology) {
    /* Stub - do nothing */
}

static void context_vs_set_shader(ID3D11DeviceContext* this, void* pVertexShader, void* ppClassInstances, uint32_t NumClassInstances) {
    /* Stub - do nothing */
}

static void context_vs_set_constant_buffers(ID3D11DeviceContext* this, uint32_t StartSlot, uint32_t NumBuffers, void* ppConstantBuffers) {
    /* Stub - do nothing */
}

static void context_ps_set_shader(ID3D11DeviceContext* this, void* pPixelShader, void* ppClassInstances, uint32_t NumClassInstances) {
    /* Stub - do nothing */
}

static void context_ps_set_constant_buffers(ID3D11DeviceContext* this, uint32_t StartSlot, uint32_t NumBuffers, void* ppConstantBuffers) {
    /* Stub - do nothing */
}

static void context_ps_set_shader_resources(ID3D11DeviceContext* this, uint32_t StartSlot, uint32_t NumViews, void* ppShaderResourceViews) {
    /* Stub - do nothing */
}

static void context_ps_set_samplers(ID3D11DeviceContext* this, uint32_t StartSlot, uint32_t NumSamplers, void* ppSamplers) {
    /* Stub - do nothing */
}

static void context_gs_set_shader(ID3D11DeviceContext* this, void* pGeometryShader, void* ppClassInstances, uint32_t NumClassInstances) {
    /* Stub - do nothing */
}

static void context_draw_indexed(ID3D11DeviceContext* this, uint32_t IndexCount, uint32_t StartIndexLocation, int32_t BaseVertexLocation) {
    /* Stub - do nothing */
}

static void context_draw(ID3D11DeviceContext* this, uint32_t VertexCount, uint32_t StartVertexLocation) {
    /* Stub - do nothing */
}

static void context_om_set_render_targets(ID3D11DeviceContext* this, uint32_t NumViews, void* ppRenderTargetViews, void* pDepthStencilView) {
    /* Stub - do nothing */
}

static void context_om_set_blend_state(ID3D11DeviceContext* this, void* pBlendState, void* BlendFactor, uint32_t SampleMask) {
    /* Stub - do nothing */
}

static void context_om_set_depthstencil_state(ID3D11DeviceContext* this, void* pDepthStencilState, uint32_t StencilRef) {
    /* Stub - do nothing */
}

static void context_rs_set_state(ID3D11DeviceContext* this, void* pRasterizerState) {
    /* Stub - do nothing */
}

static void context_rs_set_viewports(ID3D11DeviceContext* this, uint32_t NumViewports, void* pViewports) {
    /* Stub - do nothing */
}

static void context_rs_set_scissor_rects(ID3D11DeviceContext* this, uint32_t NumRects, void* pRects) {
    /* Stub - do nothing */
}

static void context_clear_rendertarget_view(ID3D11DeviceContext* this, void* pRenderTargetView, void* ColorRGBA) {
    /* Stub - do nothing */
}

static void context_clear_depthstencil_view(ID3D11DeviceContext* this, void* pDepthStencilView, uint32_t ClearFlags, float Depth, uint8_t Stencil) {
    /* Stub - do nothing */
}

static HRESULT context_map(ID3D11DeviceContext* this, void* pResource, uint32_t Subresource, uint32_t MapType, uint32_t MapFlags, void* pMappedResource) {
    return S_OK;
}

static void context_unmap(ID3D11DeviceContext* this, void* pResource, uint32_t Subresource) {
    /* Stub - do nothing */
}

static void context_flush(ID3D11DeviceContext* this) {
    /* Stub - do nothing */
}

static ID3D11DeviceContext_VTable g_context_vtable = {
    .QueryInterface = context_query_interface,
    .AddRef = context_addref,
    .Release = context_release,
    .IASetInputLayout = context_ia_set_input_layout,
    .IASetVertexBuffers = context_ia_set_vertex_buffers,
    .IASetIndexBuffer = context_ia_set_index_buffer,
    .IASetPrimitiveTopology = context_ia_set_primitive_topology,
    .VSSetShader = context_vs_set_shader,
    .VSSetConstantBuffers = context_vs_set_constant_buffers,
    .PSSetShader = context_ps_set_shader,
    .PSSetConstantBuffers = context_ps_set_constant_buffers,
    .PSSetShaderResources = context_ps_set_shader_resources,
    .PSSetSamplers = context_ps_set_samplers,
    .GSSetShader = context_gs_set_shader,
    .DrawIndexed = context_draw_indexed,
    .Draw = context_draw,
    .OMSetRenderTargets = context_om_set_render_targets,
    .OMSetBlendState = context_om_set_blend_state,
    .OMSetDepthStencilState = context_om_set_depthstencil_state,
    .RSSetState = context_rs_set_state,
    .RSSetViewports = context_rs_set_viewports,
    .RSSetScissorRects = context_rs_set_scissor_rects,
    .ClearRenderTargetView = context_clear_rendertarget_view,
    .ClearDepthStencilView = context_clear_depthstencil_view,
    .Map = context_map,
    .Unmap = context_unmap,
    .Flush = context_flush,
};

ID3D11DeviceContext* d3d11_device_context_create(void) {
    ID3D11DeviceContext* context = (ID3D11DeviceContext*)com_alloc(sizeof(ID3D11DeviceContext), COM_TYPE_CONTEXT, 0);
    if (!context) return NULL;
    context->vtable = (void**)&g_context_vtable;
    return context;
}

/* ============================================================================
 * IDXGISwapChain - Stub implementation
 * ============================================================================ */

static HRESULT swapchain_query_interface(IDXGISwapChain* this, REFIID riid, LPVOID* ppvObj) {
    if (!ppvObj) return E_NOINTERFACE;
    *ppvObj = this;
    return S_OK;
}

static uint32_t swapchain_addref(IDXGISwapChain* this) {
    return com_addref(this);
}

static uint32_t swapchain_release(IDXGISwapChain* this) {
    return com_release(this);
}

static HRESULT swapchain_present(IDXGISwapChain* this, uint32_t SyncInterval, uint32_t Flags) {
    return S_OK;
}

static HRESULT swapchain_get_buffer(IDXGISwapChain* this, uint32_t Buffer, REFIID riid, LPVOID* ppSurface) {
    if (!ppSurface) return E_NOINTERFACE;
    void* surf = malloc(64);
    if (!surf) return 0x80000002;
    memset(surf, 0, 64);
    *ppSurface = surf;
    return S_OK;
}

static HRESULT swapchain_set_fullscreen_state(IDXGISwapChain* this, int Fullscreen, void* pTarget) {
    return S_OK;
}

static HRESULT swapchain_get_fullscreen_state(IDXGISwapChain* this, int* pFullscreen, void* ppTarget) {
    if (pFullscreen) *pFullscreen = 0;
    return S_OK;
}

static HRESULT swapchain_get_desc(IDXGISwapChain* this, void* pDesc) {
    return S_OK;
}

static HRESULT swapchain_resize_buffers(IDXGISwapChain* this, uint32_t BufferCount, uint32_t Width, uint32_t Height, uint32_t NewFormat, uint32_t SwapChainFlags) {
    return S_OK;
}

static HRESULT swapchain_resize_target(IDXGISwapChain* this, void* pNewTargetParameters) {
    return S_OK;
}

static HRESULT swapchain_get_containing_output(IDXGISwapChain* this, void* ppOutput) {
    return S_OK;
}

static HRESULT swapchain_get_frame_statistics(IDXGISwapChain* this, void* pStats) {
    return S_OK;
}

static HRESULT swapchain_get_last_present_count(IDXGISwapChain* this, uint32_t* pLastPresentCount) {
    if (pLastPresentCount) *pLastPresentCount = 0;
    return S_OK;
}

static IDXGISwapChain_VTable g_swapchain_vtable = {
    .QueryInterface = swapchain_query_interface,
    .AddRef = swapchain_addref,
    .Release = swapchain_release,
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

IDXGISwapChain* dxgi_swapchain_create(void) {
    IDXGISwapChain* swapchain = (IDXGISwapChain*)com_alloc(sizeof(IDXGISwapChain), COM_TYPE_SWAPCHAIN, 0);
    if (!swapchain) return NULL;
    swapchain->vtable = (void**)&g_swapchain_vtable;
    return swapchain;
}
