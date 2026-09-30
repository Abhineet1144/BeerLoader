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

typedef struct ID3D11Device_VTable {
    /* IUnknown methods */
    HRESULT (*QueryInterface)(ID3D11Device* this, REFIID riid, LPVOID* ppvObj);
    uint32_t (*AddRef)(ID3D11Device* this);
    uint32_t (*Release)(ID3D11Device* this);
    
    /* ID3D11Device methods - minimal set for game init */
    HRESULT (*CreateSwapChain)(ID3D11Device* this, void* pFactory, void* pDesc, void** ppSwapChain);
    HRESULT (*CreateRenderTargetView)(ID3D11Device* this, void* pResource, void* pDesc, void** ppRTView);
    HRESULT (*CreateDepthStencilView)(ID3D11Device* this, void* pResource, void* pDesc, void** ppDSView);
    HRESULT (*CreateTexture2D)(ID3D11Device* this, void* pDesc, void* pInitData, void** ppTexture2D);
    HRESULT (*CreateBuffer)(ID3D11Device* this, void* pDesc, void* pInitData, void** ppBuffer);
    HRESULT (*CreateInputLayout)(ID3D11Device* this, void* pInputElementDescs, uint32_t NumElements, void* pShaderBytecode, size_t BytecodeLength, void** ppInputLayout);
    HRESULT (*CreateVertexShader)(ID3D11Device* this, void* pShaderBytecode, size_t BytecodeLength, void* pClassLinkage, void** ppVertexShader);
    HRESULT (*CreatePixelShader)(ID3D11Device* this, void* pShaderBytecode, size_t BytecodeLength, void* pClassLinkage, void** ppPixelShader);
    HRESULT (*CreateGeometryShader)(ID3D11Device* this, void* pShaderBytecode, size_t BytecodeLength, void* pClassLinkage, void** ppGeometryShader);
    HRESULT (*CreateComputeShader)(ID3D11Device* this, void* pShaderBytecode, size_t BytecodeLength, void* pClassLinkage, void** ppComputeShader);
    HRESULT (*CreateRasterizerState)(ID3D11Device* this, void* pRasterizerDesc, void** ppRasterizerState);
    HRESULT (*CreateBlendState)(ID3D11Device* this, void* pBlendStateDesc, void** ppBlendState);
    HRESULT (*CreateDepthStencilState)(ID3D11Device* this, void* pDepthStencilDesc, void** ppDepthStencilState);
    HRESULT (*CreateSamplerState)(ID3D11Device* this, void* pSamplerDesc, void** ppSamplerState);
    HRESULT (*CreateShaderResourceView)(ID3D11Device* this, void* pResource, void* pDesc, void** ppSRView);
    HRESULT (*CreateUnorderedAccessView)(ID3D11Device* this, void* pResource, void* pDesc, void** ppUAView);
    HRESULT (*GetImmediateContext)(ID3D11Device* this, void** ppImmediateContext);
    HRESULT (*CreateDeferredContext)(ID3D11Device* this, uint32_t ContextFlags, void** ppDeferredContext);
} ID3D11Device_VTable;

/* ============================================================================
 * ID3D11DeviceContext - Deferred/Immediate rendering context
 * ============================================================================ */

typedef struct ID3D11DeviceContext {
    void** vtable;
} ID3D11DeviceContext;

typedef struct ID3D11DeviceContext_VTable {
    /* IUnknown methods */
    HRESULT (*QueryInterface)(ID3D11DeviceContext* this, REFIID riid, LPVOID* ppvObj);
    uint32_t (*AddRef)(ID3D11DeviceContext* this);
    uint32_t (*Release)(ID3D11DeviceContext* this);
    
    /* ID3D11DeviceContext methods - minimal set */
    void (*IASetInputLayout)(ID3D11DeviceContext* this, void* pInputLayout);
    void (*IASetVertexBuffers)(ID3D11DeviceContext* this, uint32_t StartSlot, uint32_t NumBuffers, void* ppVertexBuffers, void* pStrides, void* pOffsets);
    void (*IASetIndexBuffer)(ID3D11DeviceContext* this, void* pIndexBuffer, uint32_t Format, uint32_t Offset);
    void (*IASetPrimitiveTopology)(ID3D11DeviceContext* this, uint32_t Topology);
    void (*VSSetShader)(ID3D11DeviceContext* this, void* pVertexShader, void* ppClassInstances, uint32_t NumClassInstances);
    void (*VSSetConstantBuffers)(ID3D11DeviceContext* this, uint32_t StartSlot, uint32_t NumBuffers, void* ppConstantBuffers);
    void (*PSSetShader)(ID3D11DeviceContext* this, void* pPixelShader, void* ppClassInstances, uint32_t NumClassInstances);
    void (*PSSetConstantBuffers)(ID3D11DeviceContext* this, uint32_t StartSlot, uint32_t NumBuffers, void* ppConstantBuffers);
    void (*PSSetShaderResources)(ID3D11DeviceContext* this, uint32_t StartSlot, uint32_t NumViews, void* ppShaderResourceViews);
    void (*PSSetSamplers)(ID3D11DeviceContext* this, uint32_t StartSlot, uint32_t NumSamplers, void* ppSamplers);
    void (*GSSetShader)(ID3D11DeviceContext* this, void* pGeometryShader, void* ppClassInstances, uint32_t NumClassInstances);
    void (*DrawIndexed)(ID3D11DeviceContext* this, uint32_t IndexCount, uint32_t StartIndexLocation, int32_t BaseVertexLocation);
    void (*Draw)(ID3D11DeviceContext* this, uint32_t VertexCount, uint32_t StartVertexLocation);
    void (*OMSetRenderTargets)(ID3D11DeviceContext* this, uint32_t NumViews, void* ppRenderTargetViews, void* pDepthStencilView);
    void (*OMSetBlendState)(ID3D11DeviceContext* this, void* pBlendState, void* BlendFactor, uint32_t SampleMask);
    void (*OMSetDepthStencilState)(ID3D11DeviceContext* this, void* pDepthStencilState, uint32_t StencilRef);
    void (*RSSetState)(ID3D11DeviceContext* this, void* pRasterizerState);
    void (*RSSetViewports)(ID3D11DeviceContext* this, uint32_t NumViewports, void* pViewports);
    void (*RSSetScissorRects)(ID3D11DeviceContext* this, uint32_t NumRects, void* pRects);
    void (*ClearRenderTargetView)(ID3D11DeviceContext* this, void* pRenderTargetView, void* ColorRGBA);
    void (*ClearDepthStencilView)(ID3D11DeviceContext* this, void* pDepthStencilView, uint32_t ClearFlags, float Depth, uint8_t Stencil);
    HRESULT (*Map)(ID3D11DeviceContext* this, void* pResource, uint32_t Subresource, uint32_t MapType, uint32_t MapFlags, void* pMappedResource);
    void (*Unmap)(ID3D11DeviceContext* this, void* pResource, uint32_t Subresource);
    void (*Flush)(ID3D11DeviceContext* this);
} ID3D11DeviceContext_VTable;

/* ============================================================================
 * IDXGISwapChain - Display swapchain
 * ============================================================================ */

typedef struct IDXGISwapChain {
    void** vtable;
} IDXGISwapChain;

typedef struct IDXGISwapChain_VTable {
    /* IUnknown methods */
    HRESULT (*QueryInterface)(IDXGISwapChain* this, REFIID riid, LPVOID* ppvObj);
    uint32_t (*AddRef)(IDXGISwapChain* this);
    uint32_t (*Release)(IDXGISwapChain* this);
    
    /* IDXGISwapChain methods - minimal set */
    HRESULT (*Present)(IDXGISwapChain* this, uint32_t SyncInterval, uint32_t Flags);
    HRESULT (*GetBuffer)(IDXGISwapChain* this, uint32_t Buffer, REFIID riid, LPVOID* ppSurface);
    HRESULT (*SetFullscreenState)(IDXGISwapChain* this, int Fullscreen, void* pTarget);
    HRESULT (*GetFullscreenState)(IDXGISwapChain* this, int* pFullscreen, void* ppTarget);
    HRESULT (*GetDesc)(IDXGISwapChain* this, void* pDesc);
    HRESULT (*ResizeBuffers)(IDXGISwapChain* this, uint32_t BufferCount, uint32_t Width, uint32_t Height, uint32_t NewFormat, uint32_t SwapChainFlags);
    HRESULT (*ResizeTarget)(IDXGISwapChain* this, void* pNewTargetParameters);
    HRESULT (*GetContainingOutput)(IDXGISwapChain* this, void* ppOutput);
    HRESULT (*GetFrameStatistics)(IDXGISwapChain* this, void* pStats);
    HRESULT (*GetLastPresentCount)(IDXGISwapChain* this, uint32_t* pLastPresentCount);
} IDXGISwapChain_VTable;

/* ============================================================================
 * Creation functions - return allocated COM objects with filled vtables
 * ============================================================================ */

ID3D11Device* d3d11_device_create(void);
ID3D11DeviceContext* d3d11_device_context_create(void);
IDXGISwapChain* dxgi_swapchain_create(void);

#endif /* D3D11_COMPAT_H */
