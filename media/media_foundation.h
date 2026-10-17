#ifndef BEER_MEDIA_FOUNDATION_H
#define BEER_MEDIA_FOUNDATION_H

#include <stdint.h>

/* Called by the host to invoke IMFSourceReaderCallback::OnReadSample on the
 * guest object stored in the source reader's async-callback slot.
 * Signature matches: HRESULT OnReadSample(HRESULT, DWORD stream, DWORD flags,
 *                                         LONGLONG timestamp, IMFSample *) */
typedef uint64_t (*BeerMfAsyncCallbackInvokeFn)(
    void *callback_object,
    uint64_t hr, uint32_t stream_index, uint32_t stream_flags,
    int64_t timestamp, void *sample);

/* Called by the host to invoke IMFSourceReaderCallback::OnFlush(DWORD stream). */
typedef uint64_t (*BeerMfAsyncFlushInvokeFn)(
    void *callback_object, uint32_t stream_index);

/* Register host-side trampoline functions for calling back into guest code.
 * Must be called before any source reader is created. */
void beer_mf_set_async_callback_invoker(BeerMfAsyncCallbackInvokeFn on_read_sample,
                                        BeerMfAsyncFlushInvokeFn on_flush);

uint64_t __attribute__((ms_abi)) beer_mf_startup(uint32_t version, uint32_t flags);
uint64_t __attribute__((ms_abi)) beer_mf_shutdown(void);
uint64_t __attribute__((ms_abi)) beer_mf_create_dxgi_device_manager(
    uint32_t *reset_token, void **manager);
uint64_t __attribute__((ms_abi)) beer_mf_create_attributes(
    void **attributes, uint32_t initial_size);
uint64_t __attribute__((ms_abi)) beer_mf_create_media_type(void **media_type);
uint64_t __attribute__((ms_abi)) beer_mf_create_byte_stream_on_stream(
    void *stream, void **byte_stream);
uint64_t __attribute__((ms_abi)) beer_mf_create_source_reader_from_byte_stream(
    void *byte_stream, void *attributes, void **reader);

/* Test-only constructor for validating the IMFMediaBuffer/IMF2DBuffer ABI. */
void *beer_mf_test_create_nv12_sample(const uint8_t *data, uint32_t size,
                                      int64_t timestamp, int64_t duration);

#endif
