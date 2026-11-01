#include "win32_tsf.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#define S_OK UINT64_C(0)
#define E_NOINTERFACE UINT64_C(0x80004002)
#define E_NOTIMPL UINT64_C(0x80004001)

typedef uint64_t (__attribute__((ms_abi)) *QueryInterfaceFn)(void *, const uint8_t *, void **);
typedef uint64_t (__attribute__((ms_abi)) *ReleaseFn)(void *);
typedef uint64_t (__attribute__((ms_abi)) *ActivateFn)(void *, uint32_t *);
typedef uint64_t (__attribute__((ms_abi)) *DeactivateFn)(void *);
typedef uint64_t (__attribute__((ms_abi)) *CreateDocumentMgrFn)(void *, void **);
typedef uint64_t (__attribute__((ms_abi)) *GetFocusFn)(void *, void **);
typedef uint64_t (__attribute__((ms_abi)) *SetFocusFn)(void *, void *);
typedef uint64_t (__attribute__((ms_abi)) *IsThreadFocusFn)(void *, int32_t *);

int main(void)
{
    static const uint8_t clsid[16] = {
        0x6b,0x9e,0x9a,0x52,0x87,0x65,0x23,0x4f,
        0xab,0x9e,0x9c,0x7d,0x68,0x3e,0x3c,0x50
    };
    static const uint8_t iid[16] = {
        0x01,0xe8,0x80,0xaa,0x21,0x20,0xd2,0x11,
        0x93,0xe0,0x00,0x60,0xb0,0x67,0xb8,0x6e
    };
    static const uint8_t unsupported_iid[16] = { 1 };
    void *manager = NULL;
    uint64_t result = UINT64_MAX;
    assert(beer_tsf_create_instance(clsid, iid, NULL, &manager, &result));
    assert(result == S_OK && manager);

    void **vtable = *(void ***)manager;
    assert(vtable);
    uint32_t client_id = 0;
    assert(((ActivateFn)vtable[3])(manager, &client_id) == S_OK);
    assert(client_id != 0);
    int32_t focused = 0;
    assert(((IsThreadFocusFn)vtable[10])(manager, &focused) == S_OK && focused);

    void *document = (void *)(uintptr_t)1;
    assert(((CreateDocumentMgrFn)vtable[5])(manager, &document) == E_NOTIMPL);
    assert(document == NULL);

    assert(((SetFocusFn)vtable[8])(manager, manager) == S_OK);
    void *focus = NULL;
    assert(((GetFocusFn)vtable[7])(manager, &focus) == S_OK && focus == manager);
    assert(((ReleaseFn)vtable[2])(focus) >= 1);

    void *unsupported = (void *)(uintptr_t)1;
    assert(((QueryInterfaceFn)vtable[0])(manager, unsupported_iid, &unsupported) == E_NOINTERFACE);
    assert(unsupported == NULL);
    assert(((DeactivateFn)vtable[4])(manager) == S_OK);
    assert(((ReleaseFn)vtable[2])(manager) == 0);

    puts("TSF thread-manager contract test passed");
    return 0;
}
