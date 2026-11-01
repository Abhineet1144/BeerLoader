#include "win32_tsf.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TSF_S_OK UINT64_C(0)
#define TSF_E_NOINTERFACE UINT64_C(0x80004002)
#define TSF_E_POINTER UINT64_C(0x80004003)
#define TSF_E_OUTOFMEMORY UINT64_C(0x8007000e)
#define TSF_E_NOTIMPL UINT64_C(0x80004001)
#define TSF_CLASS_E_NOAGGREGATION UINT64_C(0x80040110)

typedef struct BeerTsfThreadManager BeerTsfThreadManager;
typedef uint64_t (__attribute__((ms_abi)) *BeerComAddRef)(void *object);
typedef uint64_t (__attribute__((ms_abi)) *BeerComRelease)(void *object);

struct BeerTsfThreadManager {
    void **vtable;
    _Atomic(uint32_t) references;
    uint32_t active;
    uint32_t client_id;
    void *focus;
};

static const uint8_t k_clsid_tf_thread_mgr[16] = {
    0x6b,0x9e,0x9a,0x52,0x87,0x65,0x23,0x4f,
    0xab,0x9e,0x9c,0x7d,0x68,0x3e,0x3c,0x50
};
static const uint8_t k_iid_iunknown[16] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xc0,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x46
};
static const uint8_t k_iid_itf_thread_mgr[16] = {
    0x01,0xe8,0x80,0xaa,0x21,0x20,0xd2,0x11,
    0x93,0xe0,0x00,0x60,0xb0,0x67,0xb8,0x6e
};

static int guid_equal(const uint8_t *left, const uint8_t *right)
{
    return left && right && memcmp(left, right, 16) == 0;
}

static void object_add_ref(void *object)
{
    if (!object) return;
    void **vtable = *(void ***)object;
    if (vtable && vtable[1]) ((BeerComAddRef)vtable[1])(object);
}

static void object_release(void *object)
{
    if (!object) return;
    void **vtable = *(void ***)object;
    if (vtable && vtable[2]) ((BeerComRelease)vtable[2])(object);
}

static uint64_t __attribute__((ms_abi))
tsf_query_interface(BeerTsfThreadManager *manager, const uint8_t *iid, void **output)
{
    if (!output) return TSF_E_POINTER;
    *output = NULL;
    if (!manager || (!guid_equal(iid, k_iid_iunknown) &&
                     !guid_equal(iid, k_iid_itf_thread_mgr)))
        return TSF_E_NOINTERFACE;
    atomic_fetch_add_explicit(&manager->references, 1, memory_order_relaxed);
    *output = manager;
    return TSF_S_OK;
}

static uint64_t __attribute__((ms_abi)) tsf_add_ref(BeerTsfThreadManager *manager)
{
    return manager ? atomic_fetch_add_explicit(&manager->references, 1,
                                                memory_order_relaxed) + 1 : 0;
}

static uint64_t __attribute__((ms_abi)) tsf_release(BeerTsfThreadManager *manager)
{
    if (!manager) return 0;
    uint32_t old = atomic_fetch_sub_explicit(&manager->references, 1,
                                             memory_order_acq_rel);
    if (old == 1) {
        object_release(manager->focus);
        free(manager);
        return 0;
    }
    return old - 1;
}

static uint64_t __attribute__((ms_abi))
tsf_activate(BeerTsfThreadManager *manager, uint32_t *client_id)
{
    static _Atomic(uint32_t) next_client_id = 1;
    if (!manager || !client_id) return TSF_E_POINTER;
    if (!manager->active) {
        manager->client_id = atomic_fetch_add_explicit(&next_client_id, 1,
                                                       memory_order_relaxed);
        if (!manager->client_id)
            manager->client_id = atomic_fetch_add_explicit(&next_client_id, 1,
                                                           memory_order_relaxed);
        manager->active = 1;
    }
    *client_id = manager->client_id;
    return TSF_S_OK;
}

static uint64_t __attribute__((ms_abi)) tsf_deactivate(BeerTsfThreadManager *manager)
{
    if (!manager) return TSF_E_POINTER;
    manager->active = 0;
    manager->client_id = 0;
    object_release(manager->focus);
    manager->focus = NULL;
    return TSF_S_OK;
}

static uint64_t __attribute__((ms_abi)) tsf_create_document_manager(
    BeerTsfThreadManager *manager, void **output)
{
    (void)manager;
    if (!output) return TSF_E_POINTER;
    *output = NULL;
    return TSF_E_NOTIMPL;
}

static uint64_t __attribute__((ms_abi)) tsf_enum_document_managers(
    BeerTsfThreadManager *manager, void **output)
{
    (void)manager;
    if (!output) return TSF_E_POINTER;
    *output = NULL;
    return TSF_E_NOTIMPL;
}

static uint64_t __attribute__((ms_abi))
tsf_get_focus(BeerTsfThreadManager *manager, void **output)
{
    if (!output) return TSF_E_POINTER;
    *output = NULL;
    if (!manager) return TSF_E_POINTER;
    object_add_ref(manager->focus);
    *output = manager->focus;
    return TSF_S_OK;
}

static uint64_t __attribute__((ms_abi))
tsf_set_focus(BeerTsfThreadManager *manager, void *document_manager)
{
    if (!manager) return TSF_E_POINTER;
    object_add_ref(document_manager);
    void *old = manager->focus;
    manager->focus = document_manager;
    object_release(old);
    return TSF_S_OK;
}

static uint64_t __attribute__((ms_abi)) tsf_associate_focus(
    BeerTsfThreadManager *manager, uint64_t window, void *document_manager,
    void **previous)
{
    (void)manager;
    (void)window;
    (void)document_manager;
    if (!previous) return TSF_E_POINTER;
    *previous = NULL;
    return TSF_E_NOTIMPL;
}

static uint64_t __attribute__((ms_abi))
tsf_is_thread_focus(BeerTsfThreadManager *manager, int32_t *focused)
{
    if (!focused) return TSF_E_POINTER;
    *focused = manager && manager->active ? 1 : 0;
    return manager ? TSF_S_OK : TSF_E_POINTER;
}

static uint64_t __attribute__((ms_abi)) tsf_get_function_provider(
    BeerTsfThreadManager *manager, const uint8_t *class_id, void **output)
{
    (void)manager;
    (void)class_id;
    if (!output) return TSF_E_POINTER;
    *output = NULL;
    return TSF_E_NOTIMPL;
}

static uint64_t __attribute__((ms_abi)) tsf_enum_function_providers(
    BeerTsfThreadManager *manager, void **output)
{
    (void)manager;
    if (!output) return TSF_E_POINTER;
    *output = NULL;
    return TSF_E_NOTIMPL;
}

static uint64_t __attribute__((ms_abi)) tsf_get_global_compartment(
    BeerTsfThreadManager *manager, void **output)
{
    (void)manager;
    if (!output) return TSF_E_POINTER;
    *output = NULL;
    return TSF_E_NOTIMPL;
}

static void *g_tsf_thread_manager_vtable[14] = {
    (void *)tsf_query_interface,
    (void *)tsf_add_ref,
    (void *)tsf_release,
    (void *)tsf_activate,
    (void *)tsf_deactivate,
    (void *)tsf_create_document_manager,
    (void *)tsf_enum_document_managers,
    (void *)tsf_get_focus,
    (void *)tsf_set_focus,
    (void *)tsf_associate_focus,
    (void *)tsf_is_thread_focus,
    (void *)tsf_get_function_provider,
    (void *)tsf_enum_function_providers,
    (void *)tsf_get_global_compartment
};

int beer_tsf_create_instance(const uint8_t *class_id, const uint8_t *interface_id,
                             void *outer, void **output, uint64_t *result)
{
    if (!guid_equal(class_id, k_clsid_tf_thread_mgr)) return 0;
    if (!result) return 1;
    if (!output) {
        *result = TSF_E_POINTER;
        return 1;
    }
    *output = NULL;
    if (outer) {
        *result = TSF_CLASS_E_NOAGGREGATION;
        return 1;
    }
    if (!guid_equal(interface_id, k_iid_iunknown) &&
        !guid_equal(interface_id, k_iid_itf_thread_mgr)) {
        *result = TSF_E_NOINTERFACE;
        return 1;
    }
    BeerTsfThreadManager *manager = calloc(1, sizeof(*manager));
    if (!manager) {
        *result = TSF_E_OUTOFMEMORY;
        return 1;
    }
    manager->vtable = g_tsf_thread_manager_vtable;
    atomic_init(&manager->references, 1);
    *output = manager;
    *result = TSF_S_OK;
    return 1;
}
