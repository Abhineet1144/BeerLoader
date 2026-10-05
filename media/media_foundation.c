#include "media_foundation.h"

#include <stdatomic.h>
#include <stdio.h>

#define BEER_S_OK      UINT64_C(0)
#define BEER_E_NOTIMPL UINT64_C(0x80004001)
#define BEER_E_INVALIDARG UINT64_C(0x80070057)

static _Atomic(uint32_t) g_startup_references;

uint64_t __attribute__((ms_abi))
beer_mf_startup(uint32_t version, uint32_t flags)
{
    /* Platform startup is independent from individual object factories. Keep
     * a real reference count so clients can probe Media Foundation and then
     * receive explicit failures only for interfaces Beer has not implemented. */
    if ((version & 0xffff0000u) != 0x00020000u) return BEER_E_INVALIDARG;
    atomic_fetch_add_explicit(&g_startup_references, 1, memory_order_relaxed);
    fprintf(stderr, "[MF] MFStartup version=0x%x flags=0x%x -> S_OK\n",
            version, flags);
    return BEER_S_OK;
}

uint64_t __attribute__((ms_abi)) beer_mf_shutdown(void)
{
    uint32_t current = atomic_load_explicit(&g_startup_references,
                                            memory_order_relaxed);
    while (current != 0 &&
           !atomic_compare_exchange_weak_explicit(
               &g_startup_references, &current, current - 1,
               memory_order_relaxed, memory_order_relaxed)) { }
    return current ? BEER_S_OK : BEER_E_NOTIMPL;
}

uint64_t __attribute__((ms_abi))
beer_mf_create_dxgi_device_manager(uint32_t *reset_token, void **manager)
{
    if (reset_token) *reset_token = 0;
    if (manager) *manager = NULL;
    fprintf(stderr, "[MF] MFCreateDXGIDeviceManager -> E_NOTIMPL\n");
    return manager ? BEER_E_NOTIMPL : BEER_E_INVALIDARG;
}

uint64_t __attribute__((ms_abi))
beer_mf_create_attributes(void **attributes, uint32_t initial_size)
{
    if (!attributes) return BEER_E_INVALIDARG;
    *attributes = NULL;
    fprintf(stderr, "[MF] MFCreateAttributes initial=%u -> E_NOTIMPL\n",
            initial_size);
    return BEER_E_NOTIMPL;
}

uint64_t __attribute__((ms_abi)) beer_mf_create_media_type(void **media_type)
{
    if (!media_type) return BEER_E_INVALIDARG;
    *media_type = NULL;
    fprintf(stderr, "[MF] MFCreateMediaType -> E_NOTIMPL\n");
    return BEER_E_NOTIMPL;
}

uint64_t __attribute__((ms_abi))
beer_mf_create_byte_stream_on_stream(void *stream, void **byte_stream)
{
    if (!byte_stream) return BEER_E_INVALIDARG;
    *byte_stream = NULL;
    fprintf(stderr, "[MF] MFCreateMFByteStreamOnStream stream=%p -> E_NOTIMPL\n",
            stream);
    return BEER_E_NOTIMPL;
}

uint64_t __attribute__((ms_abi))
beer_mf_create_source_reader_from_byte_stream(void *byte_stream,
                                               void *attributes,
                                               void **reader)
{
    if (!reader) return BEER_E_INVALIDARG;
    *reader = NULL;
    fprintf(stderr,
            "[MF] MFCreateSourceReaderFromByteStream stream=%p attrs=%p "
            "-> E_NOTIMPL\n",
            byte_stream, attributes);
    return BEER_E_NOTIMPL;
}
