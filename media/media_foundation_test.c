#include "media_foundation.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define S_OK UINT64_C(0)
#define E_INVALIDARG UINT64_C(0x80070057)
#define E_NOTIMPL UINT64_C(0x80004001)

typedef uint64_t __attribute__((ms_abi)) (*ReleaseFn)(void *);
typedef uint64_t __attribute__((ms_abi)) (*GetUint32Fn)(void *, const void *, uint32_t *);
typedef uint64_t __attribute__((ms_abi)) (*GetUint64Fn)(void *, const void *, uint64_t *);
typedef uint64_t __attribute__((ms_abi)) (*GetGuidFn)(void *, const void *, void *);
typedef uint64_t __attribute__((ms_abi)) (*SetUint32Fn)(void *, const void *, uint32_t);
typedef uint64_t __attribute__((ms_abi)) (*GetCountFn)(void *, uint32_t *);
typedef uint64_t __attribute__((ms_abi)) (*OpenHandleFn)(void *, void **);
typedef uint64_t __attribute__((ms_abi)) (*GetSelectionFn)(void *, uint32_t, int *);
typedef uint64_t __attribute__((ms_abi)) (*SetSelectionFn)(void *, uint32_t, int);
typedef uint64_t __attribute__((ms_abi)) (*SetMediaTypeFn)(void *, uint32_t, uint32_t *, void *);
typedef uint64_t __attribute__((ms_abi)) (*GetMediaTypeFn)(void *, uint32_t, void **);
typedef uint64_t __attribute__((ms_abi)) (*GetNativeMediaTypeFn)(void *, uint32_t,
                                                                  uint32_t, void **);
typedef uint64_t __attribute__((ms_abi)) (*ReadSampleFn)(void *, uint32_t, uint32_t,
                                                         uint32_t *, uint32_t *,
                                                         int64_t *, void **);
typedef uint64_t __attribute__((ms_abi)) (*FlushFn)(void *, uint32_t);
typedef uint64_t __attribute__((ms_abi)) (*SetPositionFn)(void *, const void *,
                                                           const void *);
typedef uint64_t __attribute__((ms_abi)) (*GetAttributeFn)(void *, uint32_t,
                                                           const void *, void *);
typedef uint64_t __attribute__((ms_abi)) (*GetSampleTimeFn)(void *, int64_t *);
typedef uint64_t __attribute__((ms_abi)) (*QueryInterfaceFn)(void *, const void *,
                                                              void **);
typedef void *__attribute__((ms_abi)) (*GetAudioFormatFn)(void *);
typedef uint64_t __attribute__((ms_abi)) (*ConvertBufferFn)(void *, void **);
typedef uint64_t __attribute__((ms_abi)) (*Lock2DFn)(void *, uint8_t **, int32_t *);
typedef uint64_t __attribute__((ms_abi)) (*Unlock2DFn)(void *);
typedef uint64_t __attribute__((ms_abi)) (*GetContiguousLengthFn)(void *, uint32_t *);

typedef struct FakeUnknown {
    void **vtable;
    uint32_t references;
} FakeUnknown;

/* ── IMFSourceReaderCallback fake — must be at file scope in C99 ── */
typedef struct FakeCallback {
    void **vtable;
    int refs;
    int on_read_called;
    int on_flush_called;
    uint32_t last_stream;
    uint32_t last_flags;
    int64_t  last_ts;
} FakeCallback;

static FakeCallback g_fcb;

static uint64_t __attribute__((ms_abi))
test_fake_cb_qi(FakeCallback *self, const void *iid, void **out)
{
    (void)iid;
    if (!out) return UINT64_C(0x80004003);
    *out = self; self->refs++;
    return S_OK;
}
static uint64_t __attribute__((ms_abi)) test_fake_cb_addref(FakeCallback *self)
{ return ++self->refs; }
static uint64_t __attribute__((ms_abi)) test_fake_cb_release(FakeCallback *self)
{ return --self->refs; }
static uint64_t __attribute__((ms_abi))
test_fake_on_read_sample(FakeCallback *self, uint64_t hr,
                         uint32_t stream, uint32_t flags,
                         int64_t ts, void *samp)
{
    (void)hr; (void)samp;
    self->on_read_called++;
    self->last_stream = stream;
    self->last_flags  = flags;
    self->last_ts     = ts;
    return S_OK;
}
static uint64_t __attribute__((ms_abi))
test_fake_on_flush(FakeCallback *self, uint32_t stream)
{ (void)stream; self->on_flush_called++; return S_OK; }

/* Host-side invoker trampolines used by the async reader test. */
static uint64_t test_invoke_on_read(void *cb, uint64_t hr, uint32_t si,
                                    uint32_t sf, int64_t ts, void *samp)
{
    void **vt = *(void ***)cb;
    typedef uint64_t __attribute__((ms_abi)) (*Fn)(
        void *, uint64_t, uint32_t, uint32_t, int64_t, void *);
    return ((Fn)vt[3])(cb, hr, si, sf, ts, samp);
}
static uint64_t test_invoke_on_flush(void *cb, uint32_t si)
{
    void **vt = *(void ***)cb;
    typedef uint64_t __attribute__((ms_abi)) (*Fn)(void *, uint32_t);
    return ((Fn)vt[4])(cb, si);
}

static uint64_t __attribute__((ms_abi)) fake_query(FakeUnknown *self,
                                                    const void *iid,
                                                    void **output)
{
    (void)iid;
    if (!output) return UINT64_C(0x80004003);
    *output = self;
    self->references++;
    return S_OK;
}

static uint64_t __attribute__((ms_abi)) fake_add_ref(FakeUnknown *self)
{
    return ++self->references;
}

static uint64_t __attribute__((ms_abi)) fake_release(FakeUnknown *self)
{
    return --self->references;
}

static uint64_t __attribute__((ms_abi)) fake_stream_read(
    FakeUnknown *self, void *data, uint32_t bytes, uint32_t *read)
{
    (void)self; (void)data; (void)bytes;
    if (read) *read = 0;
    return S_OK;
}

static uint64_t __attribute__((ms_abi)) fake_stream_write(void *self, ...)
{
    (void)self;
    return E_NOTIMPL;
}

static uint64_t __attribute__((ms_abi)) fake_stream_seek(
    FakeUnknown *self, int64_t offset, uint32_t origin, uint64_t *position)
{
    (void)self; (void)offset; (void)origin;
    if (position) *position = 0;
    return S_OK;
}

int main(void)
{
    void *output = (void *)(uintptr_t)1;
    uint32_t token = 99;

    if (beer_mf_startup(0x20070, 0) != S_OK) return 1;
    if (beer_mf_create_dxgi_device_manager(NULL, &output) != E_INVALIDARG)
        return 2;
    if (beer_mf_create_dxgi_device_manager(&token, &output) != S_OK ||
        !output || !token)
        return 3;

    void **manager_vtable = *(void ***)output;
    void *handle = (void *)(uintptr_t)1;
    if (((OpenHandleFn)manager_vtable[6])(output, &handle) != E_NOTIMPL || handle)
        return 4;

    void *attributes = NULL;
    if (beer_mf_create_attributes(&attributes, 2) != S_OK || !attributes)
        return 5;
    void **attributes_vtable = *(void ***)attributes;
    uint8_t key[16] = {1, 2, 3, 4};
    if (((SetUint32Fn)attributes_vtable[21])(attributes, key, 0x12345678) != S_OK)
        return 6;
    uint32_t value = 0;
    if (((GetUint32Fn)attributes_vtable[7])(attributes, key, &value) != S_OK ||
        value != 0x12345678)
        return 7;
    uint32_t count = 0;
    if (((GetCountFn)attributes_vtable[30])(attributes, &count) != S_OK || count != 1)
        return 8;

    void *media_type = NULL;
    if (beer_mf_create_media_type(&media_type) != S_OK || !media_type)
        return 9;

    void *fake_vtable[3] = {fake_query, fake_add_ref, fake_release};
    FakeUnknown device = {fake_vtable, 1};
    typedef uint64_t __attribute__((ms_abi)) (*ResetFn)(void *, void *, uint32_t);
    if (((ResetFn)manager_vtable[7])(output, &device, token) != S_OK ||
        device.references != 2)
        return 10;
    if (((OpenHandleFn)manager_vtable[6])(output, &handle) != S_OK ||
        handle != output)
        return 11;

    ((ReleaseFn)attributes_vtable[2])(attributes);
    ((ReleaseFn)manager_vtable[2])(output);
    if (device.references != 1) return 12;

    void *fake_stream_vtable[6] = {
        fake_query, fake_add_ref, fake_release,
        fake_stream_read, fake_stream_write, fake_stream_seek
    };
    FakeUnknown stream = {fake_stream_vtable, 1};
    output = NULL;
    if (beer_mf_create_byte_stream_on_stream(&stream, &output) != S_OK ||
        !output || stream.references != 2)
        return 13;
    void *reader = NULL;
    if (beer_mf_create_source_reader_from_byte_stream(output, NULL, &reader) !=
            S_OK || !reader)
        return 14;
    void **reader_vtable = *(void ***)reader;
    void *native_type = NULL;
    if (((GetNativeMediaTypeFn)reader_vtable[5])(reader, 0, 0, &native_type) != S_OK ||
        !native_type)
        return 15;
    static const uint8_t mf_mt_major_type[16] = {
        0x8e, 0xa1, 0xeb, 0x48, 0xc9, 0xf8, 0x87, 0x46,
        0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f
    };
    static const uint8_t mf_mt_subtype[16] = {
        0x9a, 0x4c, 0xe3, 0xf7, 0xe8, 0x42, 0x14, 0x47,
        0xb7, 0x4b, 0xcb, 0x29, 0xd7, 0x2c, 0x35, 0xe5
    };
    static const uint8_t mf_media_type_video[16] = {
        0x76, 0x69, 0x64, 0x73, 0x00, 0x00, 0x10, 0x00,
        0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71
    };
    static const uint8_t mf_video_format_nv12[16] = {
        0x4e, 0x56, 0x31, 0x32, 0x00, 0x00, 0x10, 0x00,
        0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71
    };
    static const uint8_t mf_mt_frame_size[16] = {
        0xe2, 0xae, 0x85, 0x16, 0x87, 0x0f, 0x57, 0x4a,
        0xb7, 0xa3, 0x33, 0xda, 0xe3, 0x13, 0x76, 0x55
    };
    static const uint8_t mf_mt_frame_rate[16] = {
        0xc5, 0xc6, 0x47, 0xc4, 0x8c, 0x4e, 0xb5, 0x4b,
        0x8a, 0xf4, 0xb0, 0x46, 0x4d, 0x18, 0x3a, 0xc8
    };
    static const uint8_t mf_pd_duration[16] = {
        0x33, 0x0d, 0x99, 0x6c, 0x8e, 0xbb, 0x7a, 0x47,
        0x85, 0x98, 0x0d, 0x5d, 0x96, 0xfc, 0xd8, 0x8a
    };
    uint8_t guid[16] = {0};
    void **native_vtable = *(void ***)native_type;
    if (((GetGuidFn)native_vtable[10])(native_type, mf_mt_major_type, guid) != S_OK ||
        memcmp(guid, mf_media_type_video, sizeof(guid)) != 0)
        return 24;
    memset(guid, 0, sizeof(guid));
    if (((GetGuidFn)native_vtable[10])(native_type, mf_mt_subtype, guid) != S_OK ||
        memcmp(guid, mf_video_format_nv12, sizeof(guid)) != 0)
        return 25;
    uint64_t packed = 0;
    if (((GetUint64Fn)native_vtable[8])(native_type, mf_mt_frame_size, &packed) != S_OK ||
        packed != ((UINT64_C(1920) << 32) | UINT64_C(1080)))
        return 29;
    if (((GetUint64Fn)native_vtable[8])(native_type, mf_mt_frame_rate, &packed) != S_OK ||
        packed != ((UINT64_C(60) << 32) | UINT64_C(1)))
        return 30;
    uint8_t duration_variant[24] = {0};
    if (((GetAttributeFn)reader_vtable[12])(
            reader, UINT32_C(0xffffffff), mf_pd_duration,
            duration_variant) == S_OK)
        return 36;
    ((ReleaseFn)native_vtable[2])(native_type);
    native_type = (void *)(uintptr_t)1;
    if (((GetNativeMediaTypeFn)reader_vtable[5])(reader, 0, 1, &native_type) == S_OK ||
        native_type)
        return 16;
    int selected = 0;
    if (((GetSelectionFn)reader_vtable[3])(reader, 0, &selected) != S_OK ||
        !selected)
        return 17;
    if (((SetSelectionFn)reader_vtable[4])(reader, 0, 0) != S_OK ||
        ((GetSelectionFn)reader_vtable[3])(reader, 0, &selected) != S_OK ||
        selected)
        return 18;
    if (((SetMediaTypeFn)reader_vtable[7])(reader, 0, NULL, media_type) != S_OK)
        return 19;
    void *native_audio_type = NULL;
    if (((GetNativeMediaTypeFn)reader_vtable[5])(
            reader, 1, 0, &native_audio_type) != S_OK || !native_audio_type)
        return 40;
    void *requested_audio_type = NULL;
    if (beer_mf_create_media_type(&requested_audio_type) != S_OK ||
        !requested_audio_type)
        return 41;
    if (((SetMediaTypeFn)reader_vtable[7])(
            reader, 1, NULL, requested_audio_type) != S_OK)
        return 42;
    ((ReleaseFn)(*(void ***)requested_audio_type)[2])(requested_audio_type);
    ((ReleaseFn)(*(void ***)native_audio_type)[2])(native_audio_type);
    void *current_type = NULL;
    if (((GetMediaTypeFn)reader_vtable[6])(reader, 0, &current_type) != S_OK ||
        current_type != media_type)
        return 20;
    ((ReleaseFn)(*(void ***)current_type)[2])(current_type);
    current_type = NULL;
    if (((GetMediaTypeFn)reader_vtable[6])(
            reader, UINT32_C(0xfffffffd), &current_type) != S_OK ||
        !current_type)
        return 37;
    void **audio_vtable = *(void ***)current_type;
    static const uint8_t iid_audio_media_type[16] = {
        0xc3, 0xad, 0xa0, 0x26, 0x26, 0xce, 0x72, 0x46,
        0x93, 0x04, 0x69, 0x55, 0x2e, 0xdd, 0x3f, 0xaf
    };
    void *audio_interface = NULL;
    if (((QueryInterfaceFn)audio_vtable[0])(
            current_type, iid_audio_media_type, &audio_interface) != S_OK ||
        !audio_interface)
        return 38;
    void **audio_interface_vtable = *(void ***)audio_interface;
    const uint8_t *wave_format = ((GetAudioFormatFn)audio_interface_vtable[38])(
        audio_interface);
    if (!wave_format || *(const uint16_t *)(wave_format + 0) != 1 ||
        *(const uint16_t *)(wave_format + 2) != 2 ||
        *(const uint32_t *)(wave_format + 4) != 48000 ||
        *(const uint32_t *)(wave_format + 8) != 192000 ||
        *(const uint16_t *)(wave_format + 12) != 4 ||
        *(const uint16_t *)(wave_format + 14) != 16)
        return 39;
    ((ReleaseFn)audio_interface_vtable[2])(audio_interface);
    ((ReleaseFn)audio_vtable[2])(current_type);
    uint32_t actual_stream = UINT32_MAX;
    uint32_t stream_flags = 0;
    int64_t timestamp = -1;
    void *sample = (void *)(uintptr_t)1;
    if (((ReadSampleFn)reader_vtable[9])(reader, 0, 0, &actual_stream,
                                        &stream_flags, &timestamp, &sample) != S_OK ||
        actual_stream != 0 || stream_flags != 2 || timestamp != 0 || sample)
        return 19;
    /* Verify the decoded sample/buffer ABI independently of an installed
     * decoder by constructing one through the reader is not possible with the
     * empty fake stream. The production buffer object is exercised through a
     * small test hook below. */
    extern void *beer_mf_test_create_nv12_sample(const uint8_t *, uint32_t,
                                                  int64_t, int64_t);
    uint8_t nv12[24] = {0};
    void *test_sample = beer_mf_test_create_nv12_sample(nv12, sizeof(nv12), 10, 9);
    if (!test_sample) return 31;
    void **sample_vtable = *(void ***)test_sample;
    int64_t sample_time = 0;
    if (((GetSampleTimeFn)sample_vtable[35])(test_sample, &sample_time) != S_OK ||
        sample_time != 10)
        return 43;
    void *test_buffer = NULL;
    if (((ConvertBufferFn)sample_vtable[41])(test_sample, &test_buffer) != S_OK ||
        !test_buffer)
        return 32;
    void **buffer_vtable = *(void ***)test_buffer;
    static const uint8_t iid_2d_buffer[16] = {
        0xf9, 0xd5, 0xc9, 0x7d, 0xd9, 0x9e, 0xec, 0x44,
        0x9b, 0xbf, 0x06, 0x00, 0xbb, 0x58, 0x9f, 0xbb
    };
    void *buffer_2d = NULL;
    if (((QueryInterfaceFn)buffer_vtable[0])(test_buffer, iid_2d_buffer,
                                             &buffer_2d) != S_OK ||
        !buffer_2d || buffer_2d == test_buffer)
        return 33;
    buffer_vtable = *(void ***)buffer_2d;
    uint8_t *scanline = NULL;
    int32_t pitch = 0;
    if (((Lock2DFn)buffer_vtable[3])(buffer_2d, &scanline, &pitch) != S_OK ||
        !scanline || memcmp(scanline, nv12, sizeof(nv12)) != 0 || pitch != 1920)
        return 34;
    ((Unlock2DFn)buffer_vtable[4])(buffer_2d);
    uint32_t contiguous_length = 0;
    if (((GetContiguousLengthFn)buffer_vtable[7])(
            buffer_2d, &contiguous_length) != S_OK ||
        contiguous_length != sizeof(nv12))
        return 35;
    ((ReleaseFn)buffer_vtable[2])(buffer_2d);
    ((ReleaseFn)(*(void ***)test_buffer)[2])(test_buffer);
    ((ReleaseFn)sample_vtable[2])(test_sample);

    actual_stream = UINT32_MAX;
    stream_flags = UINT32_MAX;
    timestamp = -1;
    sample = (void *)(uintptr_t)1;
    if (((ReadSampleFn)reader_vtable[9])(reader, UINT32_C(0xfffffffe), 0,
                                        &actual_stream, &stream_flags,
                                        &timestamp, &sample) != S_OK ||
        actual_stream != 0 || stream_flags != 2 || timestamp != 0 || sample)
        return 21;
    if (((FlushFn)reader_vtable[10])(reader, UINT32_C(0xfffffffe)) != S_OK)
        return 26;
    uint16_t position[12] = {20}; /* VT_I8 PROPVARIANT with zero position. */
    if (((SetPositionFn)reader_vtable[8])(reader, NULL, position) != S_OK)
        return 27;
    actual_stream = UINT32_MAX;
    stream_flags = 0;
    if (((ReadSampleFn)reader_vtable[9])(reader, UINT32_C(0xfffffffe), 0,
                                        &actual_stream, &stream_flags,
                                        &timestamp, &sample) != S_OK ||
        actual_stream != 0 || stream_flags != 2 || sample)
        return 28;
    ((ReleaseFn)reader_vtable[2])(reader);

    /* ── Async-callback registration test ──────────────────────────── */
    /* Build a fake IMFSourceReaderCallback: slots 0-2 IUnknown, slot 3
     * OnReadSample, slot 4 OnFlush.  The test verifies that Beer extracts
     * the callback from the attribute bag, stores it, and invokes it after
     * ReadSample/Flush. Helpers live at file scope — C99 forbids local fns. */
    void *fake_cb_vtable[5] = {
        test_fake_cb_qi, test_fake_cb_addref, test_fake_cb_release,
        test_fake_on_read_sample, test_fake_on_flush
    };
    g_fcb.vtable = fake_cb_vtable;
    g_fcb.refs = 1;

    /* MF_SOURCE_READER_ASYNC_CALLBACK GUID:
     * {9159BB7C-6A2C-4F75-A3CE-6E5D63F28090} */
    static const uint8_t mf_src_rdr_async_cb_key[16] = {
        0x7c, 0xbb, 0x59, 0x91, 0x2c, 0x6a, 0x75, 0x4f,
        0xa3, 0xce, 0x6e, 0x5d, 0x63, 0xf2, 0x80, 0x90
    };
    void *cb_attrs = NULL;
    if (beer_mf_create_attributes(&cb_attrs, 4) != S_OK || !cb_attrs)
        return 50;
    void **cb_attr_vt = *(void ***)cb_attrs;
    typedef uint64_t __attribute__((ms_abi)) (*SetUnknownFn)(void *, const void *, void *);
    if (((SetUnknownFn)cb_attr_vt[27])(cb_attrs, mf_src_rdr_async_cb_key, &g_fcb) != S_OK)
        return 51;
    /* g_fcb.refs must be 2: initial 1 + AddRef from SetUnknown */
    if (g_fcb.refs != 2) return 52;

    beer_mf_set_async_callback_invoker(test_invoke_on_read, test_invoke_on_flush);

    /* Create a reader with the callback in its attributes bag. */
    FakeUnknown stream2 = {fake_stream_vtable, 1};
    void *byte_stream2 = NULL;
    if (beer_mf_create_byte_stream_on_stream(&stream2, &byte_stream2) != S_OK)
        return 53;
    void *async_reader = NULL;
    if (beer_mf_create_source_reader_from_byte_stream(
            byte_stream2, cb_attrs, &async_reader) != S_OK || !async_reader)
        return 54;
    /* The reader extracted the callback: g_fcb.refs should now be 3
     * (initial 1, SetUnknown, reader extraction via GetUnknown → AddRef). */
    if (g_fcb.refs < 3) return 55;

    /* ReadSample should call OnReadSample — with an empty stream it returns EOS. */
    uint32_t as2 = UINT32_MAX, sf2 = 0;
    int64_t  ts2 = -1;
    void    *samp2 = (void *)(uintptr_t)1;
    void **async_vt = *(void ***)async_reader;
    if (((ReadSampleFn)async_vt[9])(async_reader, 0, 0, &as2, &sf2, &ts2, &samp2) != S_OK)
        return 56;
    /* OnReadSample must have been called (EOS path fires it too). */
    if (g_fcb.on_read_called < 1) return 57;

    /* Flush should call OnFlush. */
    if (((FlushFn)async_vt[10])(async_reader, UINT32_C(0xfffffffe)) != S_OK)
        return 58;
    if (g_fcb.on_flush_called < 1) return 59;

    ((ReleaseFn)async_vt[2])(async_reader);
    ((ReleaseFn)(*(void ***)byte_stream2)[2])(byte_stream2);
    ((ReleaseFn)cb_attr_vt[2])(cb_attrs);
    /* All reader references gone; g_fcb.refs back to 1 (initial). */
    if (g_fcb.refs != 1) return 60;

    ((ReleaseFn)(*(void ***)media_type)[2])(media_type);
    ((ReleaseFn)(*(void ***)output)[2])(output);
    if (stream.references != 1) return 22;
    if (beer_mf_shutdown() != S_OK) return 23;
    puts("Media Foundation compatibility tests passed");
    return 0;
}
