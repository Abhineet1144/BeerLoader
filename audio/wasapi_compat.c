#include "wasapi_compat.h"
#include "audio_backend.h"

#include <stdatomic.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

#define S_OK UINT64_C(0)
#define S_FALSE UINT64_C(1)
#define E_NOINTERFACE UINT64_C(0x80004002)
#define E_POINTER UINT64_C(0x80004003)
#define E_FAIL UINT64_C(0x80004005)
#define E_INVALIDARG UINT64_C(0x80070057)
#define AUDCLNT_E_NOT_INITIALIZED UINT64_C(0x88890001)
#define AUDCLNT_E_ALREADY_INITIALIZED UINT64_C(0x88890002)
#define AUDCLNT_E_UNSUPPORTED_FORMAT UINT64_C(0x88890008)
#define AUDCLNT_E_OUT_OF_ORDER UINT64_C(0x88890007)
#define AUDCLNT_E_BUFFER_SIZE_ERROR UINT64_C(0x88890016)

/* GUIDs use their in-memory Win64 byte order. */
static const uint8_t clsid_mmdevice_enumerator[16] = {
    0x95,0x03,0xde,0xbc,0x2f,0xe5,0x7c,0x46,0x8e,0x3d,0xc4,0x57,0x92,0x91,0x69,0x2e
};
static const uint8_t iid_iunknown[16] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x46
};
static const uint8_t iid_device_enumerator[16] = {
    0xd2,0x64,0x56,0xa9,0x14,0x96,0x35,0x4f,0xa7,0x46,0xde,0x8d,0xb6,0x36,0x17,0xe6
};
static const uint8_t iid_device[16] = {
    0x3f,0x06,0x66,0xd6,0x87,0x15,0x43,0x4e,0x81,0xf1,0xb9,0x48,0xe8,0x07,0x36,0x3f
};
static const uint8_t iid_audio_client[16] = {
    0x4c,0xad,0xb9,0x1c,0xfa,0xdb,0x32,0x4c,0xb1,0x78,0xc2,0xf5,0x68,0xa7,0x03,0xb2
};
static const uint8_t iid_render_client[16] = {
    0xfc,0xac,0x94,0xf2,0x46,0x31,0x83,0x44,0xa7,0xbf,0xad,0xdc,0xa7,0xc2,0x60,0xe2
};
static const uint8_t iid_session_control[16] = {   /* IAudioSessionControl */
    0x99,0xa5,0xb1,0xf4,0x66,0x72,0x19,0x43,0xa8,0xca,0xe7,0x0a,0xcb,0x11,0xe8,0xcd
};
static const uint8_t iid_audio_clock[16] = {
    0x4f,0x31,0x63,0xcd,0xba,0x3f,0x1b,0x4a,0x81,0x2c,0xef,0x96,0x35,0x87,0x28,0xe7
};

typedef struct WaveFormat {
    uint16_t tag;
    uint16_t channels;
    uint32_t rate;
    uint32_t average_bytes;
    uint16_t block_align;
    uint16_t bits;
    uint16_t extra;
} __attribute__((packed)) WaveFormat;

typedef struct BeerWasapi BeerWasapi;
typedef struct BeerWasapiInterface {
    void **vtable;
    BeerWasapi *owner;
} BeerWasapiInterface;

struct BeerWasapi {
    atomic_uint references;
    BeerWasapiInterface enumerator;
    BeerWasapiInterface collection;
    BeerWasapiInterface device;
    BeerWasapiInterface audio_client;
    BeerWasapiInterface render_client;
    BeerWasapiInterface audio_clock;
    BeerWasapiInterface session;          /* IAudioSessionControl */
    BeerWasapiInterface property_store;   /* IPropertyStore of the endpoint */
    WaveFormat format;
    uint32_t buffer_frames;
    uint32_t acquired_frames;
    size_t acquired_capacity;
    int float_staging;            /* the engine mixes floats in place: sticky once seen (a packet's tail can be silent) */
    uint8_t *acquired_buffer;
    atomic_uint_fast64_t submitted_frames;
    atomic_uint_fast64_t event_handle;
    atomic_uint_fast64_t clock_frames;
    atomic_int started;
    atomic_int worker_running;
    pthread_t worker;
    int initialized;
};

static BeerWasapiSignalEventFn g_signal_event;

static int guid_equal(const void *a, const uint8_t b[16])
{ return a && memcmp(a, b, 16) == 0; }

static BeerWasapiInterface *wasapi_interface(void *self)
{ return (BeerWasapiInterface *)self; }

static uint64_t __attribute__((ms_abi)) wasapi_add_ref(void *self)
{
    BeerWasapi *owner = wasapi_interface(self)->owner;
    return atomic_fetch_add_explicit(&owner->references, 1, memory_order_relaxed) + 1;
}

static uint64_t __attribute__((ms_abi)) wasapi_release(void *self)
{
    BeerWasapi *owner = wasapi_interface(self)->owner;
    uint32_t references = atomic_fetch_sub_explicit(&owner->references, 1,
                                                     memory_order_acq_rel) - 1;
    if (!references) {
        if (atomic_exchange_explicit(&owner->worker_running, 0,
                                     memory_order_acq_rel))
            pthread_join(owner->worker, NULL);
        free(owner->acquired_buffer);
        free(owner);
    }
    return references;
}

static void *select_interface(BeerWasapi *owner, const void *iid)
{
    if (guid_equal(iid, iid_iunknown)) return &owner->enumerator;
    if (guid_equal(iid, iid_device_enumerator)) return &owner->enumerator;
    if (guid_equal(iid, iid_device)) return &owner->device;
    if (guid_equal(iid, iid_audio_client)) return &owner->audio_client;
    if (guid_equal(iid, iid_render_client)) return &owner->render_client;
    if (guid_equal(iid, iid_audio_clock)) return &owner->audio_clock;
    if (guid_equal(iid, iid_session_control)) return &owner->session;
    return NULL;
}

/* Interfaces the compat layer does not provide: logged (a few times) so a game's failed probe is visible. */
static void wasapi_log_unsupported(const char *what, const void *iid)
{
    static _Atomic(unsigned) count;
    if (atomic_fetch_add(&count, 1) >= 24) return;
    const uint8_t *g = iid;
    if (!g) { fprintf(stderr, "[WASAPI] %s\n", what); return; }
    fprintf(stderr, "[WASAPI] %s {%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x} -> E_NOINTERFACE\n", what,
            g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6], g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

static uint64_t __attribute__((ms_abi)) wasapi_query(void *self, const void *iid,
                                                      void **output)
{
    if (!output) return E_POINTER;
    *output = NULL;
    BeerWasapi *owner = wasapi_interface(self)->owner;
    void *selected = select_interface(owner, iid);
    if (!selected) { wasapi_log_unsupported("QueryInterface", iid); return E_NOINTERFACE; }
    wasapi_add_ref(selected);
    *output = selected;
    return S_OK;
}

static uint64_t __attribute__((ms_abi)) enumerator_get_default(void *self,
                                                               uint32_t flow,
                                                               uint32_t role,
                                                               void **device)
{
    (void)flow; (void)role;
    if (!device) return E_POINTER;
    BeerWasapi *owner = wasapi_interface(self)->owner;
    wasapi_add_ref(&owner->device);
    *device = &owner->device;
    return S_OK;
}

static uint64_t __attribute__((ms_abi)) collection_get_count(void *self,
                                                             uint32_t *count)
{
    (void)self;
    if (!count) return E_POINTER;
    *count = 1;
    return S_OK;
}

static uint64_t __attribute__((ms_abi)) collection_item(void *self, uint32_t index,
                                                        void **device)
{
    if (!device) return E_POINTER;
    *device = NULL;
    if (index != 0) return E_INVALIDARG;
    BeerWasapi *owner = wasapi_interface(self)->owner;
    wasapi_add_ref(&owner->device);
    *device = &owner->device;
    return S_OK;
}

static uint64_t __attribute__((ms_abi)) enumerator_enum_endpoints(
    void *self, uint32_t flow, uint32_t state_mask, void **collection)
{
    (void)flow;
    (void)state_mask;
    if (!collection) return E_POINTER;
    BeerWasapi *owner = wasapi_interface(self)->owner;
    wasapi_add_ref(&owner->collection);
    *collection = &owner->collection;
    return S_OK;
}

static uint64_t __attribute__((ms_abi)) enumerator_notimpl(void *self, ...)
{ (void)self; wasapi_log_unsupported("IMMDeviceEnumerator method not implemented", NULL); return E_FAIL; }
static uint64_t __attribute__((ms_abi)) enumerator_notify(void *self, void *callback)
{ (void)self; (void)callback; return S_OK; }

static uint64_t __attribute__((ms_abi)) device_activate(void *self, const void *iid,
                                                        uint32_t context,
                                                        void *parameters,
                                                        void **object)
{
    (void)parameters;
    if (!object) return E_POINTER;
    *object = NULL;
    BeerWasapi *owner = wasapi_interface(self)->owner;
    void *selected = select_interface(owner, iid);
    static atomic_uint diagnostics;
    unsigned diagnostic = atomic_fetch_add_explicit(&diagnostics, 1,
                                                     memory_order_relaxed);
    if (diagnostic < 32) {
        const uint8_t zero[16] = {0};
        const uint8_t *id = iid ? iid : zero;
        fprintf(stderr,
                "[WASAPI] IMMDevice::Activate context=0x%x "
                "iid=%02x%02x%02x%02x-%02x%02x-%02x%02x-"
                "%02x%02x-%02x%02x%02x%02x%02x%02x -> %s\n",
                context, id[3], id[2], id[1], id[0], id[5], id[4],
                id[7], id[6], id[8], id[9], id[10], id[11], id[12],
                id[13], id[14], id[15], selected ? "S_OK" : "E_NOINTERFACE");
    }
    if (!selected) return E_NOINTERFACE;
    wasapi_add_ref(selected);
    *object = selected;
    return S_OK;
}

static uint64_t __attribute__((ms_abi)) device_get_state(void *self, uint32_t *state)
{ (void)self; if (!state) return E_POINTER; *state = 1; return S_OK; }
/* Strings handed to the guest are released with CoTaskMemFree (free). */
static uint16_t *wasapi_wide_dup(const char *text)
{
    size_t n = strlen(text);
    uint16_t *wide = calloc(n + 1, sizeof(*wide));
    for (size_t i = 0; wide && i < n; ++i) wide[i] = (uint8_t)text[i];
    return wide;
}

static uint64_t __attribute__((ms_abi)) device_open_property_store(void *self, uint32_t access, void **store)
{
    (void)access;
    if (!store) return E_POINTER;
    BeerWasapi *owner = wasapi_interface(self)->owner;
    wasapi_add_ref(&owner->property_store);
    *store = &owner->property_store;
    return S_OK;
}

static uint64_t __attribute__((ms_abi)) device_get_id(void *self, uint16_t **id)
{
    (void)self;
    if (!id) return E_POINTER;
    *id = wasapi_wide_dup("{0.0.0.00000000}.{beer00000-0000-0000-0000-000000000001}");
    return *id ? S_OK : E_FAIL;
}

/* IPropertyStore: every value is the endpoint's friendly name (PKEY_Device_FriendlyName and its siblings). */
static uint64_t __attribute__((ms_abi)) store_get_count(void *self, uint32_t *count)
{ (void)self; if (!count) return E_POINTER; *count = 0; return S_OK; }
static uint64_t __attribute__((ms_abi)) store_get_at(void *self, uint32_t index, void *key)
{ (void)self; (void)index; (void)key; return E_FAIL; }
static uint64_t __attribute__((ms_abi)) store_get_value(void *self, const void *key, uint8_t *value)
{
    (void)self; (void)key;
    if (!value) return E_POINTER;
    memset(value, 0, 24);                                  /* PROPVARIANT */
    *(uint16_t *)value = 31;                               /* VT_LPWSTR */
    *(uint16_t **)(value + 8) = wasapi_wide_dup("Beer Audio Output");
    return S_OK;
}
static uint64_t __attribute__((ms_abi)) store_ok(void *self, ...)
{ (void)self; return S_OK; }

/* IAudioSessionControl: one always-active session; names and notifications are accepted and ignored. */
static uint64_t __attribute__((ms_abi)) session_get_state(void *self, int *state)
{ (void)self; if (!state) return E_POINTER; *state = 1; return S_OK; }   /* AudioSessionStateActive */
static uint64_t __attribute__((ms_abi)) session_get_string(void *self, uint16_t **text)
{ (void)self; if (!text) return E_POINTER; *text = wasapi_wide_dup(""); return *text ? S_OK : E_FAIL; }
static uint64_t __attribute__((ms_abi)) session_get_grouping(void *self, uint8_t *guid)
{ (void)self; if (!guid) return E_POINTER; memset(guid, 0, 16); return S_OK; }
static uint64_t __attribute__((ms_abi)) session_ok(void *self, ...)
{ (void)self; return S_OK; }

static int supported_format(const WaveFormat *format)
{
    return format && format->tag == 1 && format->channels >= 1 &&
           format->channels <= 2 && format->rate && format->bits == 16 &&
           format->block_align == format->channels * 2u;
}

static uint64_t __attribute__((ms_abi)) audio_client_initialize(
    void *self, uint32_t share_mode, uint32_t stream_flags,
    int64_t buffer_duration, int64_t periodicity, const WaveFormat *format,
    const void *session_guid)
{
    (void)session_guid;
    BeerWasapi *owner = wasapi_interface(self)->owner;
    fprintf(stderr,
            "[WASAPI] IAudioClient::Initialize mode=%u flags=0x%x "
            "duration=%lld periodicity=%lld format=%u/%u/%u/%u\n",
            share_mode, stream_flags, (long long)buffer_duration,
            (long long)periodicity, format ? format->tag : 0,
            format ? format->channels : 0, format ? format->rate : 0,
            format ? format->bits : 0);
    if (owner->initialized) return AUDCLNT_E_ALREADY_INITIALIZED;
    if (!supported_format(format)) return AUDCLNT_E_UNSUPPORTED_FORMAT;
    owner->format = *format;
    uint64_t duration = buffer_duration > 0 ? (uint64_t)buffer_duration : UINT64_C(1000000);
    owner->buffer_frames = (uint32_t)((duration * format->rate + 9999999u) / 10000000u);
    if (owner->buffer_frames < format->rate / 100u) owner->buffer_frames = format->rate / 100u;
    if (!beer_audio_init(format->rate, format->channels)) return E_FAIL;
    owner->initialized = 1;
    return S_OK;
}

static uint64_t __attribute__((ms_abi)) audio_client_get_buffer_size(void *self,
                                                                     uint32_t *frames)
{
    BeerWasapi *owner = wasapi_interface(self)->owner;
    if (!frames) return E_POINTER;
    if (!owner->initialized) return AUDCLNT_E_NOT_INITIALIZED;
    *frames = owner->buffer_frames;
    return S_OK;
}
static uint64_t __attribute__((ms_abi)) audio_client_get_latency(void *self, int64_t *latency)
{ if (!latency) return E_POINTER; if (!wasapi_interface(self)->owner->initialized) return AUDCLNT_E_NOT_INITIALIZED; *latency = 1000000; return S_OK; }
static uint32_t wasapi_current_padding(const BeerWasapi *owner)
{
    uint64_t submitted = atomic_load_explicit(&owner->submitted_frames,
                                              memory_order_acquire);
    uint64_t consumed = atomic_load_explicit(&owner->clock_frames,
                                             memory_order_acquire);
    uint64_t pending = submitted > consumed ? submitted - consumed : 0;
    if (pending > owner->buffer_frames) pending = owner->buffer_frames;
    return (uint32_t)pending;
}

static uint64_t __attribute__((ms_abi)) audio_client_get_padding(void *self, uint32_t *frames)
{
    if (!frames) return E_POINTER;
    BeerWasapi *owner = wasapi_interface(self)->owner;
    if (!owner->initialized) return AUDCLNT_E_NOT_INITIALIZED;
    *frames = wasapi_current_padding(owner);
    if (getenv("BEER_WASAPI_DIAGNOSTICS")) {
        static atomic_uint diagnostics;
        unsigned index = atomic_fetch_add_explicit(
            &diagnostics, 1, memory_order_relaxed);
        if (index < 32)
            fprintf(stderr, "[WASAPI] IAudioClient::GetCurrentPadding -> %u\n",
                    *frames);
    }
    return S_OK;
}
static uint64_t __attribute__((ms_abi)) audio_client_is_supported(void *self,
    uint32_t mode, const WaveFormat *format, WaveFormat **closest)
{ (void)self; (void)mode; if (closest) *closest=NULL; return supported_format(format) ? S_OK : AUDCLNT_E_UNSUPPORTED_FORMAT; }
static uint64_t __attribute__((ms_abi)) audio_client_get_mix_format(void *self,
                                                                    WaveFormat **format)
{
    (void)self;
    if (!format) return E_POINTER;
    *format = calloc(1, sizeof(**format));
    if (!*format) return E_FAIL;
    **format = (WaveFormat){1, 2, 48000, 192000, 4, 16, 0};
    return S_OK;
}
static uint64_t __attribute__((ms_abi)) audio_client_get_period(void *self,
    int64_t *default_period, int64_t *minimum_period)
{ (void)self; if(default_period)*default_period=100000; if(minimum_period)*minimum_period=100000; return S_OK; }

static void *wasapi_event_worker(void *opaque)
{
    BeerWasapi *owner = opaque;
    const struct timespec period = {0, 10000000}; /* 10 ms device period */
    struct timespec last_tick;
    clock_gettime(CLOCK_MONOTONIC, &last_tick);
    while (atomic_load_explicit(&owner->worker_running, memory_order_acquire)) {
        nanosleep(&period, NULL);
        /* A late tick (busy CPU) must still advance the device clock by the real time that passed. A fixed 10 ms step
         * made the game believe the buffer was fuller than the sound server's was, so it mixed too little audio and
         * the stream ran dry (the audible blip when the load rises). */
        struct timespec now_ts;
        clock_gettime(CLOCK_MONOTONIC, &now_ts);
        int64_t elapsed_ns = (int64_t)(now_ts.tv_sec - last_tick.tv_sec) * 1000000000ll + (now_ts.tv_nsec - last_tick.tv_nsec);
        last_tick = now_ts;
        if (!atomic_load_explicit(&owner->started, memory_order_acquire)) continue;
        if (elapsed_ns < 10000000ll) elapsed_ns = 10000000ll;
        if (elapsed_ns > 200000000ll) elapsed_ns = 200000000ll;
        /* A render endpoint's clock reports the position consumed from its
         * submitted stream, not wall-clock time since Start. Cap the synthetic
         * device period at the number of frames actually released by the
         * render client so media schedulers cannot observe unsubmitted audio. */
        uint32_t rate = owner->format.rate ? owner->format.rate : 48000;
        uint64_t current = atomic_load_explicit(&owner->clock_frames,
                                                memory_order_relaxed);
        uint64_t submitted = atomic_load_explicit(&owner->submitted_frames,
                                                  memory_order_acquire);
        uint64_t advanced = current + (uint64_t)elapsed_ns * rate / 1000000000ull;
        if (advanced > submitted) advanced = submitted;
        atomic_store_explicit(&owner->clock_frames, advanced,
                              memory_order_release);
        uint64_t event = atomic_load_explicit(&owner->event_handle,
                                              memory_order_acquire);
        if (event && g_signal_event) {
            g_signal_event(event);
            static atomic_uint diagnostics;
            unsigned index = atomic_fetch_add_explicit(&diagnostics, 1,
                                                        memory_order_relaxed);
            if (getenv("BEER_WASAPI_DIAGNOSTICS") && index < 16)
                fprintf(stderr, "[WASAPI] periodic event=%#llx position=%llu\n",
                        (unsigned long long)event,
                        (unsigned long long)atomic_load_explicit(
                            &owner->clock_frames, memory_order_relaxed));
        }
    }
    return NULL;
}

static uint64_t __attribute__((ms_abi)) audio_client_start(void *self)
{
    BeerWasapi *owner = wasapi_interface(self)->owner;
    if (!owner->initialized) return AUDCLNT_E_NOT_INITIALIZED;
    atomic_store_explicit(&owner->started, 1, memory_order_release);
    uint64_t event = atomic_load_explicit(&owner->event_handle,
                                          memory_order_acquire);
    if (getenv("BEER_WASAPI_DIAGNOSTICS"))
        fprintf(stderr, "[WASAPI] IAudioClient::Start event=%#llx\n",
                (unsigned long long)event);
    if (event && g_signal_event) g_signal_event(event);
    return S_OK;
}
static uint64_t __attribute__((ms_abi)) audio_client_stop(void *self)
{
    BeerWasapi *owner=wasapi_interface(self)->owner;
    if(!owner->initialized)return AUDCLNT_E_NOT_INITIALIZED;
    atomic_store_explicit(&owner->started, 0, memory_order_release);
    return S_OK;
}
static uint64_t __attribute__((ms_abi)) audio_client_reset(void *self)
{
    BeerWasapi *owner=wasapi_interface(self)->owner;
    if(!owner->initialized)return AUDCLNT_E_NOT_INITIALIZED;
    beer_audio_reset();
    atomic_store_explicit(&owner->submitted_frames, 0, memory_order_release);
    atomic_store_explicit(&owner->clock_frames, 0, memory_order_release);
    return S_OK;
}
static uint64_t __attribute__((ms_abi)) audio_client_set_event(void *self, uint64_t event)
{
    BeerWasapi *owner = wasapi_interface(self)->owner;
    if (!event) return E_INVALIDARG;
    atomic_store_explicit(&owner->event_handle, event, memory_order_release);
    if (getenv("BEER_WASAPI_DIAGNOSTICS"))
        fprintf(stderr, "[WASAPI] IAudioClient::SetEventHandle(%#llx)\n",
                (unsigned long long)event);
    if (atomic_load_explicit(&owner->started, memory_order_acquire) &&
        g_signal_event) g_signal_event(event);
    return S_OK;
}
static uint64_t __attribute__((ms_abi)) audio_client_get_service(void *self,
                                                                 const void *iid,
                                                                 void **service)
{
    if (!service) return E_POINTER;
    *service = NULL;
    BeerWasapi *owner = wasapi_interface(self)->owner;
    void *selected = select_interface(owner, iid);
    if (selected != &owner->render_client && selected != &owner->audio_clock && selected != &owner->session) {
        wasapi_log_unsupported("IAudioClient::GetService", iid);
        return E_NOINTERFACE;
    }
    if (getenv("BEER_WASAPI_DIAGNOSTICS"))
        fprintf(stderr, "[WASAPI] IAudioClient::GetService -> %s\n",
                selected == &owner->render_client ? "render" : selected == &owner->session ? "session" : "clock");
    wasapi_add_ref(selected);
    *service = selected;
    return S_OK;
}

static uint64_t __attribute__((ms_abi)) render_get_buffer(void *self, uint32_t frames,
                                                           uint8_t **data)
{
    if (!data) return E_POINTER;
    *data = NULL;
    BeerWasapi *owner = wasapi_interface(self)->owner;
    if (!owner->initialized) return AUDCLNT_E_NOT_INITIALIZED;
    if (owner->acquired_buffer) return AUDCLNT_E_OUT_OF_ORDER;
    if (!frames || frames > owner->buffer_frames) return AUDCLNT_E_BUFFER_SIZE_ERROR;
    /* Reserve engine-mix staging capacity in addition to the endpoint's
     * declared PCM payload. Some clients perform an in-place float-to-PCM
     * conversion in the render packet; ReleaseBuffer still submits only the
     * byte count described by WAVEFORMATEX. */
    size_t staging_align = (size_t)owner->format.channels * sizeof(float);
    if (staging_align < owner->format.block_align)
        staging_align = owner->format.block_align;
    size_t capacity = (size_t)frames * staging_align;
    owner->acquired_buffer = calloc(1, capacity);
    if (!owner->acquired_buffer) return E_FAIL;
    owner->acquired_frames = frames;
    owner->acquired_capacity = capacity;
    *data = owner->acquired_buffer;
    if (getenv("BEER_WASAPI_DIAGNOSTICS")) {
        static atomic_uint diagnostics;
        unsigned index = atomic_fetch_add_explicit(
            &diagnostics, 1, memory_order_relaxed);
        if (index < 32)
            fprintf(stderr,
                    "[WASAPI] IAudioRenderClient::GetBuffer frames=%u\n",
                    frames);
    }
    return S_OK;
}

int16_t beer_wasapi_float_bits_to_s16(uint32_t bits)
{
    uint32_t sign = bits >> 31;
    uint32_t exponent = (bits >> 23) & UINT32_C(0xff);
    uint32_t fraction = bits & UINT32_C(0x7fffff);

    /* Silence denormals and NaNs. Infinities and values at or beyond unity
     * saturate, matching a conventional float engine-mix conversion. */
    if (!exponent || (exponent == UINT32_C(0xff) && fraction)) return 0;
    if (exponent >= 127)
        return sign ? INT16_MIN : INT16_MAX;
    if (exponent <= 111) return 0;

    uint32_t mantissa = UINT32_C(0x800000) | fraction;
    uint32_t shift = 135 - exponent;
    uint32_t magnitude = (mantissa + (UINT32_C(1) << (shift - 1))) >> shift;
    if (sign)
        return magnitude >= 32768 ? INT16_MIN : (int16_t)-(int32_t)magnitude;
    return magnitude >= 32767 ? INT16_MAX : (int16_t)magnitude;
}

static int convert_float_staging_packet(BeerWasapi *owner, uint32_t frames)
{
    if (owner->format.tag != 1 || owner->format.bits != 16 ||
        !owner->format.channels ||
        owner->format.block_align != owner->format.channels * sizeof(int16_t))
        return 0;

    size_t samples = (size_t)frames * owner->format.channels;
    size_t float_bytes = samples * sizeof(uint32_t);
    size_t pcm_bytes = samples * sizeof(int16_t);
    if (owner->acquired_capacity < float_bytes || pcm_bytes >= float_bytes)
        return 0;

    /* A PCM16 client normally writes only the declared packet bytes. Some
     * engine mixers use the extra endpoint buffer capacity as an in-place
     * float staging area before conversion. Detect that representation from
     * the complete packet instead of selecting behavior by executable name. */
    size_t tail_nonzero = 0;
    for (size_t offset = pcm_bytes; offset < float_bytes; ++offset)
        tail_nonzero += owner->acquired_buffer[offset] != 0;
    /* The tail only proves the float layout while the sound is playing; the last packet of a short sound has a silent
     * tail and was mistaken for PCM16, which played raw float bits as a click. Once seen, the layout is sticky. */
    if (!tail_nonzero && !owner->float_staging) return 0;

    size_t finite_unit_samples = 0;
    for (size_t sample = 0; sample < samples; ++sample) {
        uint32_t bits;
        memcpy(&bits, owner->acquired_buffer + sample * sizeof(bits), sizeof(bits));
        uint32_t absolute = bits & UINT32_C(0x7fffffff);
        uint32_t exponent = absolute & UINT32_C(0x7f800000);
        if (exponent != UINT32_C(0x7f800000) &&
            absolute <= UINT32_C(0x3f800000))
            ++finite_unit_samples;
    }
    if (finite_unit_samples < samples * 7u / 8u) return 0;

    owner->float_staging = 1;
    for (size_t sample = 0; sample < samples; ++sample) {
        uint32_t bits;
        int16_t pcm;
        memcpy(&bits, owner->acquired_buffer + sample * sizeof(bits), sizeof(bits));
        pcm = beer_wasapi_float_bits_to_s16(bits);
        memcpy(owner->acquired_buffer + sample * sizeof(pcm), &pcm, sizeof(pcm));
    }

    if (getenv("BEER_WASAPI_DIAGNOSTICS")) {
        static atomic_uint diagnostics;
        unsigned index = atomic_fetch_add_explicit(
            &diagnostics, 1, memory_order_relaxed);
        if (index < 8)
            fprintf(stderr,
                    "[WASAPI] converted detected float staging to PCM16 (%zu samples)\n",
                    samples);
    }
    return 1;
}

static void diagnose_render_packet(const BeerWasapi *owner, uint32_t frames,
                                   uint32_t flags, size_t bytes)
{
    const char *setting = getenv("BEER_WASAPI_PACKET_DIAGNOSTICS");
    if (!setting || setting[0] != '1' || (flags & 2u) || !frames) return;

    static atomic_uint packet_count;
    unsigned packet = atomic_fetch_add_explicit(&packet_count, 1,
                                                memory_order_relaxed) + 1;
    if (packet > 64) return;

    uint32_t pcm_peak = 0;
    size_t pcm_nonzero = 0;
    size_t pcm_clipped = 0;
    for (size_t offset = 0; offset + sizeof(int16_t) <= bytes;
         offset += sizeof(int16_t)) {
        int16_t sample;
        memcpy(&sample, owner->acquired_buffer + offset, sizeof(sample));
        int32_t magnitude = sample < 0 ? -(int32_t)sample : sample;
        if ((uint32_t)magnitude > pcm_peak) pcm_peak = (uint32_t)magnitude;
        if (sample) pcm_nonzero++;
        if (magnitude >= 32760) pcm_clipped++;
    }

    /* Inspect possible IEEE-754 staging values without converting them so
     * this diagnostic remains safe in the loader's no-SIMD boundary. */
    size_t float_values = 0;
    size_t float_unit_range = 0;
    size_t float_nan_or_inf = 0;
    for (size_t offset = 0; offset + sizeof(uint32_t) <= owner->acquired_capacity;
         offset += sizeof(uint32_t)) {
        uint32_t bits;
        memcpy(&bits, owner->acquired_buffer + offset, sizeof(bits));
        uint32_t absolute = bits & UINT32_C(0x7fffffff);
        if (absolute) float_values++;
        if ((absolute & UINT32_C(0x7f800000)) == UINT32_C(0x7f800000))
            float_nan_or_inf++;
        else if (absolute <= UINT32_C(0x3f800000))
            float_unit_range++;
    }

    size_t staging_tail_nonzero = 0;
    for (size_t offset = bytes; offset < owner->acquired_capacity; ++offset)
        if (owner->acquired_buffer[offset]) staging_tail_nonzero++;

    fprintf(stderr,
            "[WASAPI PACKET] index=%u frames=%u bytes=%zu capacity=%zu "
            "pcm-nonzero=%zu pcm-peak=%u pcm-clipped=%zu "
            "float-nonzero=%zu float-unit=%zu float-nan-inf=%zu tail-nonzero=%zu\n",
            packet, frames, bytes, owner->acquired_capacity, pcm_nonzero,
            pcm_peak, pcm_clipped, float_values, float_unit_range,
            float_nan_or_inf, staging_tail_nonzero);
}

static uint64_t __attribute__((ms_abi)) render_release_buffer(void *self,
                                                               uint32_t frames,
                                                               uint32_t flags)
{
    BeerWasapi *owner = wasapi_interface(self)->owner;
    if (!owner->acquired_buffer || frames > owner->acquired_frames)
        return AUDCLNT_E_OUT_OF_ORDER;
    size_t bytes = (size_t)frames * owner->format.block_align;
    diagnose_render_packet(owner, frames, flags, bytes);
    if (frames) {
        /* A silent packet still has to reach the sound server as zeros: dropping it starves the stream and the
         * restart after the gap clicks. */
        if (flags & 2u) memset(owner->acquired_buffer, 0, bytes);
        else convert_float_staging_packet(owner, frames);
        if (!beer_audio_submit(owner->acquired_buffer, bytes, NULL, NULL))
            return E_FAIL;
    }
    owner->submitted_frames += frames;
    if (getenv("BEER_WASAPI_DIAGNOSTICS")) {
        static atomic_uint diagnostics;
        unsigned index = atomic_fetch_add_explicit(
            &diagnostics, 1, memory_order_relaxed);
        if (index < 32)
            fprintf(stderr,
                    "[WASAPI] IAudioRenderClient::ReleaseBuffer frames=%u "
                    "flags=0x%x total=%llu\n",
                    frames, flags,
                    (unsigned long long)owner->submitted_frames);
    }
    free(owner->acquired_buffer);
    owner->acquired_buffer = NULL;
    owner->acquired_frames = 0;
    owner->acquired_capacity = 0;
    return S_OK;
}

static uint64_t __attribute__((ms_abi)) clock_get_frequency(void *self, uint64_t *frequency)
{ BeerWasapi *o=wasapi_interface(self)->owner; if(!frequency)return E_POINTER; *frequency=o->format.rate ? o->format.rate : 48000; return S_OK; }
static uint64_t __attribute__((ms_abi)) clock_get_position(void *self,
                                                           uint64_t *position,
                                                           uint64_t *qpc)
{
    BeerWasapi *owner = wasapi_interface(self)->owner;
    if (!position) return E_POINTER;
    uint64_t device_position = atomic_load_explicit(&owner->clock_frames,
                                                    memory_order_acquire);
    uint32_t align = owner->format.block_align ? owner->format.block_align : 4;
    uint64_t played_position = beer_audio_bytes_played() / align;
    *position = device_position > played_position ? device_position : played_position;
    if (qpc) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        /* IAudioClock::GetPosition reports the correlation timestamp in the
         * process' QueryPerformanceCounter units. Beer exposes a 1 GHz QPC,
         * so preserve CLOCK_MONOTONIC nanoseconds rather than returning
         * 100-nanosecond Media Foundation ticks. */
        *qpc = (uint64_t)now.tv_sec * UINT64_C(1000000000) +
               (uint64_t)now.tv_nsec;
    }
    static atomic_uint diagnostics;
    unsigned index = atomic_fetch_add_explicit(&diagnostics, 1, memory_order_relaxed);
    if (index < 32)
        fprintf(stderr, "[WASAPI] IAudioClock::GetPosition -> %llu\n",
                (unsigned long long)*position);
    return S_OK;
}
static uint64_t __attribute__((ms_abi)) clock_get_characteristics(void *self,
                                                                  uint32_t *flags)
{ (void)self; if(!flags)return E_POINTER; *flags=1; return S_OK; }

static void *enumerator_vtable[8] = {
    wasapi_query, wasapi_add_ref, wasapi_release, enumerator_enum_endpoints,
    enumerator_get_default, enumerator_notimpl, enumerator_notify, enumerator_notify
};
static void *collection_vtable[5] = {
    wasapi_query, wasapi_add_ref, wasapi_release,
    collection_get_count, collection_item
};
static void *device_vtable[7] = {
    wasapi_query, wasapi_add_ref, wasapi_release, device_activate,
    device_open_property_store, device_get_id, device_get_state
};
static void *property_store_vtable[8] = {
    wasapi_query, wasapi_add_ref, wasapi_release, store_get_count, store_get_at, store_get_value, store_ok, store_ok
};
static void *session_vtable[12] = {
    wasapi_query, wasapi_add_ref, wasapi_release, session_get_state, session_get_string, session_ok,
    session_get_string, session_ok, session_get_grouping, session_ok, session_ok, session_ok
};
static void *audio_client_vtable[15] = {
    wasapi_query, wasapi_add_ref, wasapi_release, audio_client_initialize,
    audio_client_get_buffer_size, audio_client_get_latency, audio_client_get_padding,
    audio_client_is_supported, audio_client_get_mix_format, audio_client_get_period,
    audio_client_start, audio_client_stop, audio_client_reset,
    audio_client_set_event, audio_client_get_service
};
static void *render_client_vtable[5] = {
    wasapi_query, wasapi_add_ref, wasapi_release,
    render_get_buffer, render_release_buffer
};
static void *audio_clock_vtable[6] = {
    wasapi_query, wasapi_add_ref, wasapi_release,
    clock_get_frequency, clock_get_position, clock_get_characteristics
};

uint64_t beer_wasapi_create_instance(const void *class_id, const void *interface_id,
                                     void **object)
{
    if (!object) return E_POINTER;
    *object = NULL;
    if (!guid_equal(class_id, clsid_mmdevice_enumerator) ||
        !guid_equal(interface_id, iid_device_enumerator))
        return E_NOINTERFACE;
    BeerWasapi *owner = calloc(1, sizeof(*owner));
    if (!owner) return E_FAIL;
    atomic_init(&owner->references, 1);
    owner->enumerator = (BeerWasapiInterface){enumerator_vtable, owner};
    owner->collection = (BeerWasapiInterface){collection_vtable, owner};
    owner->device = (BeerWasapiInterface){device_vtable, owner};
    owner->audio_client = (BeerWasapiInterface){audio_client_vtable, owner};
    owner->render_client = (BeerWasapiInterface){render_client_vtable, owner};
    owner->audio_clock = (BeerWasapiInterface){audio_clock_vtable, owner};
    owner->session = (BeerWasapiInterface){session_vtable, owner};
    owner->property_store = (BeerWasapiInterface){property_store_vtable, owner};
    atomic_init(&owner->event_handle, 0);
    atomic_init(&owner->clock_frames, 0);
    atomic_init(&owner->started, 0);
    atomic_init(&owner->worker_running, 1);
    if (pthread_create(&owner->worker, NULL, wasapi_event_worker, owner) != 0) {
        atomic_store_explicit(&owner->worker_running, 0, memory_order_release);
        free(owner);
        return E_FAIL;
    }
    *object = &owner->enumerator;
    return S_OK;
}

void beer_wasapi_set_event_signaler(BeerWasapiSignalEventFn signal_event)
{
    g_signal_event = signal_event;
}

void beer_wasapi_shutdown(void)
{
    if (beer_audio_available()) beer_audio_shutdown();
}
