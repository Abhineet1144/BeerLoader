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

/* -----------------------------------------------------------------------
 * Inject the platform present function pointer before any source reader is
 * used.  fn must be a xwayland_window_present_rgba8-compatible function.
 * ----------------------------------------------------------------------- */
void beer_mf_set_present_fn(void *fn);

/* -----------------------------------------------------------------------
 * Active-frame compositor interface
 *
 * When RE8's MF pipeline calls Lock2DSize it gets a raw NV12 pointer.
 * It does NOT call UpdateSubresource or any D3D11 draw — instead it calls
 * IDXGISwapChain::Present on the factory swapchain immediately after.
 * Beer intercepts that Present via beer_mf_composite_pending_frame(), which
 * converts the last-locked NV12 buffer directly to RGBA8 and blits it to
 * the XWayland window.
 *
 * beer_mf_notify_lock() is called by mf_media_buffer_lock_2d_size().
 * beer_mf_notify_unlock() is called by mf_media_buffer_unlock().
 * beer_mf_composite_pending_frame() is called by dxgi_Present().
 * -----------------------------------------------------------------------*/
void beer_mf_notify_lock(const uint8_t *nv12_data, uint32_t width,
                         uint32_t height, int32_t stride);
void beer_mf_notify_unlock(void);
/* Returns 1 if a frame was composited, 0 if nothing was pending. */
int  beer_mf_composite_pending_frame(void);

#endif
