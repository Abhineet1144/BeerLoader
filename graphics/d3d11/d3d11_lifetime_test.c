#include "d3d11_compat.h"

#include <stdint.h>

#define E_INVALIDARG ((HRESULT)0x80070057)

typedef uint32_t (D3D11_MS_ABI *RefFn)(void *);
typedef void (D3D11_MS_ABI *GetContextFn)(ID3D11Device *, void **);
typedef HRESULT (D3D11_MS_ABI *QueryInterfaceFn)(
    ID3D11Device *, REFIID, void **);

static const uint8_t iid_idxgidevice[16] = {
    0xfa, 0x77, 0xec, 0x54, 0x77, 0x13, 0xe6, 0x44,
    0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c
};

int main(void)
{
    ID3D11Device *device = d3d11_device_create();
    ID3D11DeviceContext *standalone = d3d11_device_context_create();
    IDXGISwapChain *swapchain = dxgi_swapchain_create();
    if (!device || !standalone || !swapchain) return 1;

    void **device_vtable = device->vtable;
    RefFn device_addref = (RefFn)device_vtable[D3D11_DEVICE_SLOT_ADD_REF];
    RefFn device_release = (RefFn)device_vtable[D3D11_DEVICE_SLOT_RELEASE];
    RefFn context_release = (RefFn)standalone->vtable[D3D11_CONTEXT_SLOT_RELEASE];
    RefFn swapchain_release = (RefFn)swapchain->vtable[2];
    GetContextFn get_context =
        (GetContextFn)device_vtable[D3D11_DEVICE_SLOT_GET_IMMEDIATE_CONTEXT];
    QueryInterfaceFn query_interface =
        (QueryInterfaceFn)device_vtable[D3D11_DEVICE_SLOT_QUERY_INTERFACE];

    /* A retained process owner plus two returned COM references must survive
     * independent consumer releases and remain reusable for a later playback. */
    if (device_addref(device) != 2 || device_addref(device) != 3) return 2;
    if (device_release(device) != 2 || device_release(device) != 1) return 3;

    void *first = NULL;
    get_context(device, &first);
    if (!first) return 4;
    if (((RefFn)(*(void ***)first)[2])(first) != 1) return 5;

    void *second = NULL;
    get_context(device, &second);
    if (!second || second != first) return 6;
    if (((RefFn)(*(void ***)second)[2])(second) != 1) return 7;

    /* IDXGIDevice is process-owned just like the immediate context. Guest
     * QueryInterface/Release pairs must leave that persistent owner intact so
     * a later movie stack can reuse the interface safely. */
    void *first_dxgi = NULL;
    if (query_interface(device, (REFIID)(uintptr_t)iid_idxgidevice,
                        &first_dxgi) != S_OK || !first_dxgi)
        return 8;
    if (((RefFn)(*(void ***)first_dxgi)[2])(first_dxgi) != 1) return 9;
    void *second_dxgi = NULL;
    if (query_interface(device, (REFIID)(uintptr_t)iid_idxgidevice,
                        &second_dxgi) != S_OK || !second_dxgi)
        return 10;
    if (((RefFn)(*(void ***)second_dxgi)[2])(second_dxgi) != 1) return 11;

    BeerDxgiSwapChainDesc desc = {0};
    if (dxgi_swapchain_configure(NULL, device, &desc) != E_INVALIDARG)
        return 12;

    context_release(standalone);
    /* Drop the compatibility module's persistent immediate-context owner. */
    if (((RefFn)(*(void ***)first)[2])(first) != 0) return 13;
    swapchain_release(swapchain);
    /* The process-owned immediate context keeps the shared device alive. */
    if (device_release(device) != 1 || device_release(device) != 0) return 14;
    return 0;
}
