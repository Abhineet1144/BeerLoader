#include "media_foundation.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define BEER_S_OK          UINT64_C(0)
#define BEER_E_NOTIMPL     UINT64_C(0x80004001)
#define BEER_E_NOINTERFACE UINT64_C(0x80004002)
#define BEER_E_POINTER     UINT64_C(0x80004003)
#define BEER_E_INVALIDARG  UINT64_C(0x80070057)
#define BEER_MF_E_NOT_FOUND UINT64_C(0xc00d36e6)
#define BEER_MF_SOURCE_READER_ANY_STREAM UINT32_C(0xfffffffe)
#define BEER_MF_SOURCE_READERF_ENDOFSTREAM UINT32_C(0x00000002)

#define BEER_MF_ATTRIBUTE_UINT32 1u
#define BEER_MF_ATTRIBUTE_UINT64 2u
#define BEER_MF_ATTRIBUTE_GUID   3u
#define BEER_MF_ATTRIBUTE_UNKNOWN 4u

typedef struct BeerMfGuid {
    uint8_t bytes[16];
} BeerMfGuid;

typedef struct BeerMfAttributeEntry {
    BeerMfGuid key;
    uint32_t type;
    union {
        uint32_t u32;
        uint64_t u64;
        BeerMfGuid guid;
        void *unknown;
    } value;
} BeerMfAttributeEntry;

typedef struct BeerMfAttributes {
    void **vtable;
    _Atomic(uint32_t) references;
    atomic_flag lock;
    uint32_t count;
    uint32_t capacity;
    BeerMfAttributeEntry *entries;
    uint32_t media_type_kind; /* 0=attributes, 1=video, 2=audio */
    uint8_t wave_format[18];  /* WAVEFORMATEX returned by IMFAudioMediaType. */
} BeerMfAttributes;

typedef struct BeerMfDxgiManager {
    void **vtable;
    _Atomic(uint32_t) references;
    atomic_flag lock;
    uint32_t reset_token;
    void *device;
    uint32_t lock_count;
} BeerMfDxgiManager;

static _Atomic(uint32_t) g_startup_references;
static _Atomic(uint32_t) g_next_dxgi_token = 1;
static _Atomic(uint32_t) g_mf_diagnostic_count;
static _Atomic(uint32_t) g_mf_reader_state_diagnostic_count;
static _Atomic(uint32_t) g_mf_sample_diagnostic_count;

/* Host-side trampolines for invoking guest IMFSourceReaderCallback methods.
 * Set once at startup via beer_mf_set_async_callback_invoker(). */
static BeerMfAsyncCallbackInvokeFn g_async_on_read_sample;
static BeerMfAsyncFlushInvokeFn    g_async_on_flush;

void beer_mf_set_async_callback_invoker(BeerMfAsyncCallbackInvokeFn on_read_sample,
                                        BeerMfAsyncFlushInvokeFn on_flush)
{
    g_async_on_read_sample = on_read_sample;
    g_async_on_flush       = on_flush;
}

static int mf_guid_equal(const BeerMfGuid *a, const BeerMfGuid *b);

static void mf_sample_diagnostic(const char *operation, const void *object,
                                 uint64_t value)
{
    if (!getenv("BEER_MF_DIAGNOSTICS") ||
        atomic_fetch_add_explicit(&g_mf_sample_diagnostic_count, 1,
                                  memory_order_relaxed) >= 128)
        return;
    fprintf(stderr, "[MF SAMPLE] %s object=%p value=%llu caller=%p\n",
            operation, object, (unsigned long long)value,
            __builtin_return_address(0));
}

/* Media Foundation GUIDs use their in-memory little-endian representation. */
static const BeerMfGuid g_mf_mt_major_type = {{
    0x8e, 0xa1, 0xeb, 0x48, 0xc9, 0xf8, 0x87, 0x46,
    0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f
}};
static const BeerMfGuid g_mf_mt_subtype = {{
    0x9a, 0x4c, 0xe3, 0xf7, 0xe8, 0x42, 0x14, 0x47,
    0xb7, 0x4b, 0xcb, 0x29, 0xd7, 0x2c, 0x35, 0xe5
}};
static const BeerMfGuid g_mf_media_type_video = {{
    0x76, 0x69, 0x64, 0x73, 0x00, 0x00, 0x10, 0x00,
    0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71
}};
static const BeerMfGuid g_mf_video_format_nv12 = {{
    0x4e, 0x56, 0x31, 0x32, 0x00, 0x00, 0x10, 0x00,
    0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71
}};
static const BeerMfGuid g_mf_media_type_audio = {{
    0x61, 0x75, 0x64, 0x73, 0x00, 0x00, 0x10, 0x00,
    0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71
}};
/* MFAudioFormat_PCM / WAVE_FORMAT_PCM. RE8 negotiates this output type. */
static const BeerMfGuid g_mf_audio_format_pcm = {{
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
    0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71
}};
/* Interfaces queried by RE8 while constructing its audio playback clock. */
static const BeerMfGuid g_iid_imf_audio_media_type = {{
    0xc3, 0xad, 0xa0, 0x26, 0x26, 0xce, 0x72, 0x46,
    0x93, 0x04, 0x69, 0x55, 0x2e, 0xdd, 0x3f, 0xaf
}};
static const BeerMfGuid g_mf_mt_frame_size = {{
    0xe2, 0xae, 0x85, 0x16, 0x87, 0x0f, 0x57, 0x4a,
    0xb7, 0xa3, 0x33, 0xda, 0xe3, 0x13, 0x76, 0x55
}};
static const BeerMfGuid g_mf_mt_frame_rate = {{
    0xc5, 0xc6, 0x47, 0xc4, 0x8c, 0x4e, 0xb5, 0x4b,
    0x8a, 0xf4, 0xb0, 0x46, 0x4d, 0x18, 0x3a, 0xc8
}};
/* MF_PD_DURATION: presentation duration in 100-nanosecond units. */
static const BeerMfGuid g_mf_pd_duration = {{
    0x33, 0x0d, 0x99, 0x6c, 0x8e, 0xbb, 0x7a, 0x47,
    0x85, 0x98, 0x0d, 0x5d, 0x96, 0xfc, 0xd8, 0x8a
}};
/* MF_SOURCE_READER_ASYNC_CALLBACK: {9159BB7C-6A2C-4F75-A3CE-6E5D63F28090}
 * Callers store an IMFSourceReaderCallback via SetUnknown on the attributes
 * bag passed to MFCreateSourceReaderFromByteStream.  Beer extracts it at
 * construction time and calls OnReadSample/OnFlush after each operation. */
static const BeerMfGuid g_mf_source_reader_async_callback = {{
    0x7c, 0xbb, 0x59, 0x91, 0x2c, 0x6a, 0x75, 0x4f,
    0xa3, 0xce, 0x6e, 0x5d, 0x63, 0xf2, 0x80, 0x90
}};
/* Interfaces queried by RE8 after IMFSourceReader::ReadSample. */
static const BeerMfGuid g_iid_imf_2d_buffer = {{
    0xf9, 0xd5, 0xc9, 0x7d, 0xd9, 0x9e, 0xec, 0x44,
    0x9b, 0xbf, 0x06, 0x00, 0xbb, 0x58, 0x9f, 0xbb
}};
static const BeerMfGuid g_iid_imf_2d_buffer2 = {{
    0xa6, 0x5e, 0xae, 0x33, 0x16, 0x43, 0x6f, 0x43,
    0x8d, 0xdd, 0xd7, 0x3d, 0x22, 0xf8, 0x29, 0xec
}};

static int mf_diagnostic_trace(void)
{
    return getenv("BEER_MF_DIAGNOSTICS") &&
        atomic_fetch_add_explicit(&g_mf_diagnostic_count, 1,
                                  memory_order_relaxed) < 256;
}

static void mf_diagnostic_guid(const char *operation, const BeerMfGuid *key,
                               uint32_t type, const void *value)
{
    if (!mf_diagnostic_trace()) return;
    if (!key) {
        fprintf(stderr, "[MF DIAG] %s key=NULL type=%u value=%p\n",
                operation, type, value);
        return;
    }
    fprintf(stderr,
            "[MF DIAG] %s key=%02x%02x%02x%02x-%02x%02x-%02x%02x-"
            "%02x%02x-%02x%02x%02x%02x%02x%02x type=%u value=%p",
            operation,
            key->bytes[3], key->bytes[2], key->bytes[1], key->bytes[0],
            key->bytes[5], key->bytes[4], key->bytes[7], key->bytes[6],
            key->bytes[8], key->bytes[9], key->bytes[10], key->bytes[11],
            key->bytes[12], key->bytes[13], key->bytes[14], key->bytes[15],
            type, value);
    if (type == BEER_MF_ATTRIBUTE_GUID && value) {
        const BeerMfGuid *guid = value;
        fprintf(stderr,
                " guid=%02x%02x%02x%02x-%02x%02x-%02x%02x-"
                "%02x%02x-%02x%02x%02x%02x%02x%02x",
                guid->bytes[3], guid->bytes[2], guid->bytes[1], guid->bytes[0],
                guid->bytes[5], guid->bytes[4], guid->bytes[7], guid->bytes[6],
                guid->bytes[8], guid->bytes[9], guid->bytes[10], guid->bytes[11],
                guid->bytes[12], guid->bytes[13], guid->bytes[14], guid->bytes[15]);
    }
    fputc('\n', stderr);
}

static void mf_lock(atomic_flag *lock)
{
    while (atomic_flag_test_and_set_explicit(lock, memory_order_acquire)) { }
}

static void mf_unlock(atomic_flag *lock)
{
    atomic_flag_clear_explicit(lock, memory_order_release);
}

static uint64_t mf_unknown_add_ref(void *object)
{
    if (!object) return 0;
    void ***vtable = (void ***)object;
    typedef uint64_t __attribute__((ms_abi)) (*AddRefFn)(void *);
    return ((AddRefFn)(*vtable)[1])(object);
}

static uint64_t mf_unknown_release(void *object)
{
    if (!object) return 0;
    void ***vtable = (void ***)object;
    typedef uint64_t __attribute__((ms_abi)) (*ReleaseFn)(void *);
    return ((ReleaseFn)(*vtable)[2])(object);
}

static uint64_t __attribute__((ms_abi))
mf_attributes_query_interface(BeerMfAttributes *self, const void *iid, void **output)
{
    if (!output) return BEER_E_POINTER;
    *output = NULL;
    /* The attributes object also implements IMFMediaType. Only audio media
     * types expose IMFAudioMediaType's GetAudioFormat method at slot 38. */
    if (iid && mf_guid_equal(iid, &g_iid_imf_audio_media_type) &&
        self->media_type_kind != 2)
        return BEER_E_NOINTERFACE;
    *output = self;
    atomic_fetch_add_explicit(&self->references, 1, memory_order_relaxed);
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi)) mf_attributes_add_ref(BeerMfAttributes *self)
{
    return atomic_fetch_add_explicit(&self->references, 1, memory_order_relaxed) + 1;
}

static void mf_attribute_entry_clear(BeerMfAttributeEntry *entry)
{
    if (entry->type == BEER_MF_ATTRIBUTE_UNKNOWN && entry->value.unknown)
        mf_unknown_release(entry->value.unknown);
    memset(entry, 0, sizeof(*entry));
}

static uint64_t __attribute__((ms_abi)) mf_attributes_release(BeerMfAttributes *self)
{
    uint32_t references = atomic_fetch_sub_explicit(&self->references, 1,
                                                     memory_order_acq_rel) - 1;
    if (!references) {
        for (uint32_t i = 0; i < self->count; ++i)
            mf_attribute_entry_clear(&self->entries[i]);
        free(self->entries);
        free(self);
    }
    return references;
}

static int mf_guid_equal(const BeerMfGuid *a, const BeerMfGuid *b)
{
    return a && b && memcmp(a, b, sizeof(*a)) == 0;
}

static int mf_attributes_find(BeerMfAttributes *self, const BeerMfGuid *key)
{
    for (uint32_t i = 0; i < self->count; ++i)
        if (mf_guid_equal(&self->entries[i].key, key)) return (int)i;
    return -1;
}

static BeerMfAttributeEntry *mf_attributes_set_entry(BeerMfAttributes *self,
                                                       const BeerMfGuid *key)
{
    int index = mf_attributes_find(self, key);
    if (index >= 0) {
        mf_attribute_entry_clear(&self->entries[index]);
        self->entries[index].key = *key;
        return &self->entries[index];
    }
    if (self->count == self->capacity) {
        uint32_t capacity = self->capacity ? self->capacity * 2 : 4;
        BeerMfAttributeEntry *entries = realloc(self->entries,
                                                 capacity * sizeof(*entries));
        if (!entries) return NULL;
        memset(entries + self->capacity, 0,
               (capacity - self->capacity) * sizeof(*entries));
        self->entries = entries;
        self->capacity = capacity;
    }
    BeerMfAttributeEntry *entry = &self->entries[self->count++];
    memset(entry, 0, sizeof(*entry));
    entry->key = *key;
    return entry;
}

static uint64_t __attribute__((ms_abi))
mf_attributes_get_uint32(BeerMfAttributes *self, const BeerMfGuid *key,
                          uint32_t *value)
{
    if (!key || !value) return BEER_E_INVALIDARG;
    mf_lock(&self->lock);
    int index = mf_attributes_find(self, key);
    if (index < 0 || self->entries[index].type != BEER_MF_ATTRIBUTE_UINT32) {
        mf_unlock(&self->lock);
        return BEER_MF_E_NOT_FOUND;
    }
    *value = self->entries[index].value.u32;
    mf_unlock(&self->lock);
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
mf_attributes_get_uint64(BeerMfAttributes *self, const BeerMfGuid *key,
                          uint64_t *value)
{
    if (!key || !value) return BEER_E_INVALIDARG;
    mf_lock(&self->lock);
    int index = mf_attributes_find(self, key);
    if (index < 0 || self->entries[index].type != BEER_MF_ATTRIBUTE_UINT64) {
        mf_unlock(&self->lock);
        return BEER_MF_E_NOT_FOUND;
    }
    *value = self->entries[index].value.u64;
    mf_unlock(&self->lock);
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
mf_attributes_get_guid(BeerMfAttributes *self, const BeerMfGuid *key,
                        BeerMfGuid *value)
{
    if (!key || !value) return BEER_E_INVALIDARG;
    mf_lock(&self->lock);
    int index = mf_attributes_find(self, key);
    if (index < 0 || self->entries[index].type != BEER_MF_ATTRIBUTE_GUID) {
        mf_unlock(&self->lock);
        return BEER_MF_E_NOT_FOUND;
    }
    *value = self->entries[index].value.guid;
    mf_unlock(&self->lock);
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
mf_attributes_get_unknown(BeerMfAttributes *self, const BeerMfGuid *key,
                           const void *iid, void **value)
{
    if (!key || !value) return BEER_E_INVALIDARG;
    *value = NULL;
    mf_lock(&self->lock);
    int index = mf_attributes_find(self, key);
    if (index < 0 || self->entries[index].type != BEER_MF_ATTRIBUTE_UNKNOWN) {
        mf_unlock(&self->lock);
        return BEER_MF_E_NOT_FOUND;
    }
    void *unknown = self->entries[index].value.unknown;
    if (unknown) mf_unknown_add_ref(unknown);
    *value = unknown;
    mf_unlock(&self->lock);
    (void)iid;
    return unknown ? BEER_S_OK : BEER_MF_E_NOT_FOUND;
}

static uint64_t __attribute__((ms_abi))
mf_attributes_delete_item(BeerMfAttributes *self, const BeerMfGuid *key)
{
    if (!key) return BEER_E_INVALIDARG;
    mf_lock(&self->lock);
    int index = mf_attributes_find(self, key);
    if (index >= 0) {
        mf_attribute_entry_clear(&self->entries[index]);
        self->entries[index] = self->entries[--self->count];
        memset(&self->entries[self->count], 0, sizeof(self->entries[0]));
    }
    mf_unlock(&self->lock);
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi)) mf_attributes_delete_all(BeerMfAttributes *self)
{
    mf_lock(&self->lock);
    for (uint32_t i = 0; i < self->count; ++i)
        mf_attribute_entry_clear(&self->entries[i]);
    self->count = 0;
    mf_unlock(&self->lock);
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
mf_attributes_set_uint32(BeerMfAttributes *self, const BeerMfGuid *key,
                          uint32_t value)
{
    if (!key) return BEER_E_INVALIDARG;
    if (mf_diagnostic_trace()) {
        mf_diagnostic_guid("SetUINT32", key, BEER_MF_ATTRIBUTE_UINT32, NULL);
        fprintf(stderr, "[MF DIAG]   uint32=%u\n", value);
    }
    mf_lock(&self->lock);
    BeerMfAttributeEntry *entry = mf_attributes_set_entry(self, key);
    if (entry) {
        entry->type = BEER_MF_ATTRIBUTE_UINT32;
        entry->value.u32 = value;
    }
    mf_unlock(&self->lock);
    return entry ? BEER_S_OK : BEER_E_NOTIMPL;
}

static uint64_t __attribute__((ms_abi))
mf_attributes_set_uint64(BeerMfAttributes *self, const BeerMfGuid *key,
                          uint64_t value)
{
    if (!key) return BEER_E_INVALIDARG;
    mf_diagnostic_guid("SetUINT64", key, BEER_MF_ATTRIBUTE_UINT64, NULL);
    mf_lock(&self->lock);
    BeerMfAttributeEntry *entry = mf_attributes_set_entry(self, key);
    if (entry) {
        entry->type = BEER_MF_ATTRIBUTE_UINT64;
        entry->value.u64 = value;
    }
    mf_unlock(&self->lock);
    return entry ? BEER_S_OK : BEER_E_NOTIMPL;
}

static uint64_t __attribute__((ms_abi))
mf_attributes_set_guid(BeerMfAttributes *self, const BeerMfGuid *key,
                        const BeerMfGuid *value)
{
    if (!key || !value) return BEER_E_INVALIDARG;
    mf_diagnostic_guid("SetGUID", key, BEER_MF_ATTRIBUTE_GUID, value);
    mf_lock(&self->lock);
    BeerMfAttributeEntry *entry = mf_attributes_set_entry(self, key);
    if (entry) {
        entry->type = BEER_MF_ATTRIBUTE_GUID;
        entry->value.guid = *value;
    }
    mf_unlock(&self->lock);
    return entry ? BEER_S_OK : BEER_E_NOTIMPL;
}

static uint64_t __attribute__((ms_abi))
mf_attributes_set_unknown(BeerMfAttributes *self, const BeerMfGuid *key,
                           void *unknown)
{
    if (!key) return BEER_E_INVALIDARG;
    mf_diagnostic_guid("SetUnknown", key, BEER_MF_ATTRIBUTE_UNKNOWN, unknown);
    if (unknown) mf_unknown_add_ref(unknown);
    mf_lock(&self->lock);
    BeerMfAttributeEntry *entry = mf_attributes_set_entry(self, key);
    if (entry) {
        entry->type = BEER_MF_ATTRIBUTE_UNKNOWN;
        entry->value.unknown = unknown;
    }
    mf_unlock(&self->lock);
    if (!entry && unknown) mf_unknown_release(unknown);
    return entry ? BEER_S_OK : BEER_E_NOTIMPL;
}

static uint64_t __attribute__((ms_abi))
mf_attributes_get_count(BeerMfAttributes *self, uint32_t *count)
{
    if (!count) return BEER_E_POINTER;
    mf_lock(&self->lock);
    *count = self->count;
    mf_unlock(&self->lock);
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi)) mf_attributes_ok(BeerMfAttributes *self)
{
    (void)self;
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi)) mf_attributes_notimpl(void *self, ...)
{
    (void)self;
    return BEER_E_NOTIMPL;
}

static void *__attribute__((ms_abi))
mf_audio_media_type_get_audio_format(BeerMfAttributes *self)
{
    return self && self->media_type_kind == 2 ? self->wave_format : NULL;
}

static void *g_attributes_vtable[39] = {
    mf_attributes_query_interface, mf_attributes_add_ref, mf_attributes_release,
    mf_attributes_notimpl, mf_attributes_notimpl, mf_attributes_notimpl,
    mf_attributes_notimpl, mf_attributes_get_uint32, mf_attributes_get_uint64,
    mf_attributes_notimpl, mf_attributes_get_guid, mf_attributes_notimpl,
    mf_attributes_notimpl, mf_attributes_notimpl, mf_attributes_notimpl,
    mf_attributes_notimpl, mf_attributes_notimpl, mf_attributes_get_unknown,
    mf_attributes_notimpl, mf_attributes_delete_item, mf_attributes_delete_all,
    mf_attributes_set_uint32, mf_attributes_set_uint64, mf_attributes_notimpl,
    mf_attributes_set_guid, mf_attributes_notimpl, mf_attributes_notimpl,
    mf_attributes_set_unknown, mf_attributes_ok, mf_attributes_ok,
    mf_attributes_get_count, mf_attributes_notimpl, mf_attributes_notimpl,
    mf_attributes_notimpl, mf_attributes_notimpl, mf_attributes_notimpl,
    mf_attributes_notimpl, mf_attributes_notimpl,
    mf_audio_media_type_get_audio_format
};

static uint64_t __attribute__((ms_abi))
mf_manager_query_interface(BeerMfDxgiManager *self, const void *iid, void **output)
{
    (void)iid;
    if (!output) return BEER_E_POINTER;
    *output = self;
    atomic_fetch_add_explicit(&self->references, 1, memory_order_relaxed);
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi)) mf_manager_add_ref(BeerMfDxgiManager *self)
{
    return atomic_fetch_add_explicit(&self->references, 1, memory_order_relaxed) + 1;
}

static uint64_t __attribute__((ms_abi)) mf_manager_release(BeerMfDxgiManager *self)
{
    uint32_t references = atomic_fetch_sub_explicit(&self->references, 1,
                                                     memory_order_acq_rel) - 1;
    if (!references) {
        if (self->device) mf_unknown_release(self->device);
        free(self);
    }
    return references;
}

static uint64_t __attribute__((ms_abi))
mf_manager_reset_device(BeerMfDxgiManager *self, void *device, uint32_t token)
{
    if (!device || token != self->reset_token) return BEER_E_INVALIDARG;
    mf_unknown_add_ref(device);
    mf_lock(&self->lock);
    void *old = self->device;
    self->device = device;
    mf_unlock(&self->lock);
    if (old) mf_unknown_release(old);
    fprintf(stderr, "[MF] IMFDXGIDeviceManager::ResetDevice device=%p token=%u -> S_OK\n",
            device, token);
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
mf_manager_open_handle(BeerMfDxgiManager *self, void **handle)
{
    if (!handle) return BEER_E_POINTER;
    mf_lock(&self->lock);
    *handle = self->device ? self : NULL;
    mf_unlock(&self->lock);
    return *handle ? BEER_S_OK : BEER_E_NOTIMPL;
}

static uint64_t __attribute__((ms_abi))
mf_manager_close_handle(BeerMfDxgiManager *self, void *handle)
{
    return handle == self ? BEER_S_OK : BEER_E_INVALIDARG;
}

static uint64_t __attribute__((ms_abi))
mf_manager_test_device(BeerMfDxgiManager *self, void *handle)
{
    return handle == self && self->device ? BEER_S_OK : BEER_E_INVALIDARG;
}

static uint64_t __attribute__((ms_abi))
mf_manager_lock_device(BeerMfDxgiManager *self, void *handle, const void *iid,
                       void **device, int block)
{
    (void)iid;
    (void)block;
    if (!device) return BEER_E_POINTER;
    *device = NULL;
    mf_lock(&self->lock);
    if (handle == self && self->device) {
        mf_unknown_add_ref(self->device);
        *device = self->device;
        self->lock_count++;
    }
    mf_unlock(&self->lock);
    return *device ? BEER_S_OK : BEER_E_INVALIDARG;
}

static uint64_t __attribute__((ms_abi))
mf_manager_unlock_device(BeerMfDxgiManager *self, void *handle, int save_state)
{
    (void)save_state;
    if (handle != self) return BEER_E_INVALIDARG;
    mf_lock(&self->lock);
    if (self->lock_count) self->lock_count--;
    mf_unlock(&self->lock);
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
mf_manager_get_video_service(BeerMfDxgiManager *self, void *handle,
                             const void *iid, void **service)
{
    return mf_manager_lock_device(self, handle, iid, service, 1);
}

/* IMFDXGIDeviceManager's inherited COM order is fixed by the Windows ABI:
 * IUnknown (0..2), CloseDeviceHandle, GetVideoService, LockDevice,
 * OpenDeviceHandle, ResetDevice, TestDevice, UnlockDevice. */
static void *g_manager_vtable[10] = {
    mf_manager_query_interface, mf_manager_add_ref, mf_manager_release,
    mf_manager_close_handle, mf_manager_get_video_service,
    mf_manager_lock_device, mf_manager_open_handle, mf_manager_reset_device,
    mf_manager_test_device, mf_manager_unlock_device
};

typedef struct BeerMfShell {
    void **vtable;
    _Atomic(uint32_t) references;
    void *owner;
} BeerMfShell;

typedef uint64_t __attribute__((ms_abi)) (*BeerStreamReadFn)(
    void *, void *, uint32_t, uint32_t *);
typedef uint64_t __attribute__((ms_abi)) (*BeerStreamSeekFn)(
    void *, int64_t, uint32_t, uint64_t *);

static void mf_probe_stream_header(BeerMfShell *byte_stream)
{
    if (!getenv("BEER_MF_DIAGNOSTICS") || !byte_stream || !byte_stream->owner)
        return;
    void *stream = byte_stream->owner;
    void **vtable = *(void ***)stream;
    if (!vtable || !vtable[3] || !vtable[5]) return;
    uint64_t original = 0;
    if (((BeerStreamSeekFn)vtable[5])(stream, 0, 1, &original) != BEER_S_OK)
        return;
    uint64_t ignored = 0;
    if (((BeerStreamSeekFn)vtable[5])(stream, 0, 0, &ignored) != BEER_S_OK)
        return;
    uint8_t header[64] = {0};
    uint32_t read = 0;
    uint64_t result = ((BeerStreamReadFn)vtable[3])(
        stream, header, (uint32_t)sizeof(header), &read);
    ((BeerStreamSeekFn)vtable[5])(stream, (int64_t)original, 0, &ignored);
    fprintf(stderr, "[MF STREAM] hr=%#llx position=%llu bytes=%u header=",
            (unsigned long long)result, (unsigned long long)original, read);
    uint32_t shown = read < 32 ? read : 32;
    for (uint32_t i = 0; i < shown; i++) fprintf(stderr, "%02x", header[i]);
    fputc('\n', stderr);
}

static void mf_probe_stream_with_ffmpeg(BeerMfShell *byte_stream)
{
    if (!getenv("BEER_MF_PROBE_STREAM") || !byte_stream || !byte_stream->owner)
        return;
    void *stream = byte_stream->owner;
    void **vtable = *(void ***)stream;
    if (!vtable || !vtable[3] || !vtable[5]) return;
    uint64_t original = 0, ignored = 0;
    if (((BeerStreamSeekFn)vtable[5])(stream, 0, 1, &original) != BEER_S_OK ||
        ((BeerStreamSeekFn)vtable[5])(stream, 0, 0, &ignored) != BEER_S_OK)
        return;
    char path[] = "/tmp/beer-mf-stream-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return;
    uint64_t total = 0;
    uint8_t block[64 * 1024];
    for (;;) {
        uint32_t read = 0;
        uint64_t result = ((BeerStreamReadFn)vtable[3])(
            stream, block, (uint32_t)sizeof(block), &read);
        if (result != BEER_S_OK || !read) break;
        uint32_t offset = 0;
        while (offset < read) {
            ssize_t written = write(fd, block + offset, read - offset);
            if (written <= 0) { read = offset; break; }
            offset += (uint32_t)written;
        }
        total += offset;
        if (offset != read || total > UINT64_C(512) * 1024 * 1024) break;
    }
    close(fd);
    ((BeerStreamSeekFn)vtable[5])(stream, (int64_t)original, 0, &ignored);
    fprintf(stderr, "[MF STREAM] dumped %llu bytes to %s\n",
            (unsigned long long)total, path);
    char command[1024];
    snprintf(command, sizeof(command),
             "ffprobe -v error -show_entries format=format_name,duration "
             "-show_entries stream=index,codec_name,codec_type,width,height "
             "-of compact=p=0:nk=0 '%s' 2>&1", path);
    FILE *probe = popen(command, "r");
    if (probe) {
        char line[512];
        while (fgets(line, sizeof(line), probe))
            fprintf(stderr, "[MF FFPROBE] %s", line);
        pclose(probe);
    }
    unlink(path);
}

typedef struct BeerMfMediaBuffer BeerMfMediaBuffer;

typedef struct BeerMf2DBuffer {
    void **vtable;
    BeerMfMediaBuffer *owner;
} BeerMf2DBuffer;

struct BeerMfMediaBuffer {
    void **vtable;
    _Atomic(uint32_t) references;
    BeerMf2DBuffer buffer_2d;
    uint8_t *data;
    uint32_t current_length;
    uint32_t maximum_length;
    uint32_t locked;
    uint32_t width;
    uint32_t height;
    int32_t stride;
    uint32_t contiguous_length;
};

typedef struct BeerMfSample {
    void **vtable;
    _Atomic(uint32_t) references;
    BeerMfMediaBuffer *buffer;
    int64_t timestamp;
    int64_t duration;
    uint32_t stream_index;
} BeerMfSample;

typedef struct BeerMfSourceReader {
    void **vtable;
    _Atomic(uint32_t) references;
    void *owner;
    void *native_media_type;
    void *current_media_type;
    void *audio_media_type;
    void *current_audio_media_type;
    /* Guest IMFSourceReaderCallback registered via MF_SOURCE_READER_ASYNC_CALLBACK.
     * When non-NULL, Beer fires OnReadSample/OnFlush after each synchronous
     * operation so RE8's state machine can observe the completion notification
     * it expects from an asynchronous source reader. */
    void *async_callback;
    uint32_t video_selected;
    uint32_t audio_selected;
    int64_t next_video_timestamp;
    int64_t next_audio_timestamp;
    _Atomic(uint32_t) end_of_stream;
    _Atomic(uint32_t) eos_reported;
    _Atomic(uint32_t) read_count;
    _Atomic(uint32_t) reset_count;
    _Atomic(uint32_t) terminal_eos;
    char *decoder_input_path;
    FILE *decoder_output;
    pid_t decoder_pid;
    FILE *audio_decoder_output;
    pid_t audio_decoder_pid;
    uint8_t *frame_data;
    uint32_t frame_size;
    uint32_t frame_index;
    uint32_t video_width;
    uint32_t video_height;
    uint32_t video_rate_numerator;
    uint32_t video_rate_denominator;
    int64_t presentation_start_100ns;
    uint8_t *audio_data;
    uint32_t audio_frame_size;
    uint32_t audio_frame_index;
    uint64_t duration_100ns;
} BeerMfSourceReader;

static uint64_t __attribute__((ms_abi))
mf_shell_query_interface(BeerMfShell *self, const void *iid, void **output)
{
    (void)iid;
    if (!output) return BEER_E_POINTER;
    *output = self;
    atomic_fetch_add_explicit(&self->references, 1, memory_order_relaxed);
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi)) mf_shell_add_ref(BeerMfShell *self)
{
    return atomic_fetch_add_explicit(&self->references, 1, memory_order_relaxed) + 1;
}

static uint64_t __attribute__((ms_abi)) mf_shell_release(BeerMfShell *self)
{
    uint32_t references = atomic_fetch_sub_explicit(&self->references, 1,
                                                     memory_order_acq_rel) - 1;
    if (!references) {
        if (self->owner) mf_unknown_release(self->owner);
        free(self);
    }
    return references;
}

static uint64_t __attribute__((ms_abi)) mf_shell_notimpl(void *self, ...)
{
    (void)self;
    return BEER_E_NOTIMPL;
}

static uint64_t __attribute__((ms_abi))
mf_media_buffer_query(BeerMfMediaBuffer *self, const BeerMfGuid *iid, void **output)
{
    if (!output) return BEER_E_POINTER;
    *output = NULL;
    if (iid && (mf_guid_equal(iid, &g_iid_imf_2d_buffer) ||
                mf_guid_equal(iid, &g_iid_imf_2d_buffer2))) {
        *output = &self->buffer_2d;
        atomic_fetch_add_explicit(&self->references, 1, memory_order_relaxed);
        mf_sample_diagnostic("buffer-query-2d", self,
                             mf_guid_equal(iid, &g_iid_imf_2d_buffer2) ? 2 : 1);
        return BEER_S_OK;
    }
    *output = self;
    atomic_fetch_add_explicit(&self->references, 1, memory_order_relaxed);
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi)) mf_media_buffer_add_ref(BeerMfMediaBuffer *self)
{
    return atomic_fetch_add_explicit(&self->references, 1, memory_order_relaxed) + 1;
}

static uint64_t __attribute__((ms_abi)) mf_media_buffer_release(BeerMfMediaBuffer *self)
{
    uint32_t references = atomic_fetch_sub_explicit(&self->references, 1,
                                                     memory_order_acq_rel) - 1;
    mf_sample_diagnostic("buffer-release", self, references);
    if (!references) { free(self->data); free(self); }
    return references;
}

static uint64_t __attribute__((ms_abi))
mf_media_buffer_lock(BeerMfMediaBuffer *self, uint8_t **data,
                     uint32_t *maximum_length, uint32_t *current_length)
{
    mf_sample_diagnostic("buffer-lock", self, self->current_length);
    if (!data) return BEER_E_POINTER;
    *data = self->data;
    if (maximum_length) *maximum_length = self->maximum_length;
    if (current_length) *current_length = self->current_length;
    self->locked = 1;
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi)) mf_media_buffer_unlock(BeerMfMediaBuffer *self)
{
    mf_sample_diagnostic("buffer-unlock", self, self->current_length);
    self->locked = 0;
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
mf_media_buffer_get_current_length(BeerMfMediaBuffer *self, uint32_t *length)
{
    if (!length) return BEER_E_POINTER;
    *length = self->current_length;
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
mf_media_buffer_set_current_length(BeerMfMediaBuffer *self, uint32_t length)
{
    if (length > self->maximum_length) return BEER_E_INVALIDARG;
    self->current_length = length;
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
mf_media_buffer_get_max_length(BeerMfMediaBuffer *self, uint32_t *length)
{
    if (!length) return BEER_E_POINTER;
    *length = self->maximum_length;
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
mf_media_buffer_lock_2d(BeerMfMediaBuffer *self, uint8_t **scanline,
                        int32_t *pitch)
{
    mf_sample_diagnostic("buffer-lock-2d", self, self->current_length);
    if (!scanline || !pitch) return BEER_E_POINTER;
    *scanline = self->data;
    *pitch = self->stride;
    self->locked = 1;
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
mf_media_buffer_unlock_2d(BeerMfMediaBuffer *self)
{
    return mf_media_buffer_unlock(self);
}

static uint64_t __attribute__((ms_abi))
mf_media_buffer_get_scanline(BeerMfMediaBuffer *self, uint8_t **scanline,
                             int32_t *pitch)
{
    mf_sample_diagnostic("buffer-scanline", self, self->current_length);
    if (!scanline || !pitch) return BEER_E_POINTER;
    *scanline = self->data;
    *pitch = self->stride;
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
mf_media_buffer_is_contiguous(BeerMfMediaBuffer *self, int *contiguous)
{
    (void)self;
    if (!contiguous) return BEER_E_POINTER;
    *contiguous = 1;
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
mf_media_buffer_get_contiguous_length(BeerMfMediaBuffer *self,
                                      uint32_t *length)
{
    if (!length) return BEER_E_POINTER;
    *length = self->contiguous_length;
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
mf_media_buffer_copy_to(BeerMfMediaBuffer *self, uint8_t *destination,
                        uint32_t length)
{
    if (!destination) return BEER_E_POINTER;
    if (length < self->contiguous_length) return BEER_E_INVALIDARG;
    memcpy(destination, self->data, self->contiguous_length);
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
mf_media_buffer_copy_from(BeerMfMediaBuffer *self, const uint8_t *source,
                          uint32_t length)
{
    if (!source) return BEER_E_POINTER;
    if (length < self->contiguous_length) return BEER_E_INVALIDARG;
    memcpy(self->data, source, self->contiguous_length);
    self->current_length = self->contiguous_length;
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
mf_media_buffer_lock_2d_size(BeerMfMediaBuffer *self, uint32_t flags,
                             uint8_t **scanline, int32_t *pitch,
                             uint8_t **buffer_start, uint32_t *buffer_length)
{
    mf_sample_diagnostic("buffer-lock-2d-size", self,
                         ((uint64_t)flags << 32) | self->current_length);
    if (!scanline || !pitch || !buffer_start || !buffer_length)
        return BEER_E_POINTER;
    *scanline = self->data;
    *pitch = self->stride;
    *buffer_start = self->data;
    *buffer_length = self->contiguous_length;
    self->locked = 1;
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
mf_media_buffer_copy_2d_to(BeerMfMediaBuffer *self, void *destination)
{
    if (!destination) return BEER_E_POINTER;
    void **vtable = *(void ***)destination;
    if (!vtable || !vtable[15]) return BEER_E_NOINTERFACE;
    uint8_t *scanline = NULL, *start = NULL;
    int32_t pitch = 0;
    uint32_t length = 0;
    typedef uint64_t __attribute__((ms_abi)) (*Lock2DSizeFn)(
        void *, uint32_t, uint8_t **, int32_t *, uint8_t **, uint32_t *);
    typedef uint64_t __attribute__((ms_abi)) (*Unlock2DFn)(void *);
    uint64_t result = ((Lock2DSizeFn)vtable[15])(
        destination, 2, &scanline, &pitch, &start, &length);
    if (result != BEER_S_OK) return result;
    if (length < self->contiguous_length || !start) result = BEER_E_INVALIDARG;
    else memcpy(start, self->data, self->contiguous_length);
    ((Unlock2DFn)vtable[9])(destination);
    return result;
}

static uint64_t __attribute__((ms_abi))
mf_2d_buffer_query(BeerMf2DBuffer *self, const BeerMfGuid *iid, void **output)
{
    return mf_media_buffer_query(self->owner, iid, output);
}
static uint64_t __attribute__((ms_abi)) mf_2d_buffer_add_ref(BeerMf2DBuffer *self)
{ return mf_media_buffer_add_ref(self->owner); }
static uint64_t __attribute__((ms_abi)) mf_2d_buffer_release(BeerMf2DBuffer *self)
{ return mf_media_buffer_release(self->owner); }
static uint64_t __attribute__((ms_abi))
mf_2d_buffer_lock_2d(BeerMf2DBuffer *self, uint8_t **scanline, int32_t *pitch)
{ return mf_media_buffer_lock_2d(self->owner, scanline, pitch); }
static uint64_t __attribute__((ms_abi)) mf_2d_buffer_unlock_2d(BeerMf2DBuffer *self)
{ return mf_media_buffer_unlock_2d(self->owner); }
static uint64_t __attribute__((ms_abi))
mf_2d_buffer_get_scanline(BeerMf2DBuffer *self, uint8_t **scanline, int32_t *pitch)
{ return mf_media_buffer_get_scanline(self->owner, scanline, pitch); }
static uint64_t __attribute__((ms_abi))
mf_2d_buffer_is_contiguous(BeerMf2DBuffer *self, int *contiguous)
{ return mf_media_buffer_is_contiguous(self->owner, contiguous); }
static uint64_t __attribute__((ms_abi))
mf_2d_buffer_get_contiguous_length(BeerMf2DBuffer *self, uint32_t *length)
{ return mf_media_buffer_get_contiguous_length(self->owner, length); }
static uint64_t __attribute__((ms_abi))
mf_2d_buffer_copy_to(BeerMf2DBuffer *self, uint8_t *destination, uint32_t length)
{ return mf_media_buffer_copy_to(self->owner, destination, length); }
static uint64_t __attribute__((ms_abi))
mf_2d_buffer_copy_from(BeerMf2DBuffer *self, const uint8_t *source, uint32_t length)
{ return mf_media_buffer_copy_from(self->owner, source, length); }
static uint64_t __attribute__((ms_abi))
mf_2d_buffer_lock_2d_size(BeerMf2DBuffer *self, uint32_t flags,
                          uint8_t **scanline, int32_t *pitch,
                          uint8_t **buffer_start, uint32_t *buffer_length)
{
    return mf_media_buffer_lock_2d_size(self->owner, flags, scanline, pitch,
                                        buffer_start, buffer_length);
}
static uint64_t __attribute__((ms_abi))
mf_2d_buffer_copy_2d_to(BeerMf2DBuffer *self, void *destination)
{
    mf_sample_diagnostic("buffer-copy-2d", self->owner,
                         self->owner->current_length);
    return mf_media_buffer_copy_2d_to(self->owner, destination);
}

static void *g_media_buffer_vtable[8] = {
    mf_media_buffer_query, mf_media_buffer_add_ref, mf_media_buffer_release,
    mf_media_buffer_lock, mf_media_buffer_unlock,
    mf_media_buffer_get_current_length, mf_media_buffer_set_current_length,
    mf_media_buffer_get_max_length
};

/* IMF2DBuffer derives directly from IUnknown, so its methods begin at slot 3,
 * not after IMFMediaBuffer's methods. IMF2DBuffer2 appends slots 10..11. */
static void *g_2d_buffer_vtable[12] = {
    mf_2d_buffer_query, mf_2d_buffer_add_ref, mf_2d_buffer_release,
    mf_2d_buffer_lock_2d, mf_2d_buffer_unlock_2d,
    mf_2d_buffer_get_scanline, mf_2d_buffer_is_contiguous,
    mf_2d_buffer_get_contiguous_length, mf_2d_buffer_copy_to,
    mf_2d_buffer_copy_from, mf_2d_buffer_lock_2d_size,
    mf_2d_buffer_copy_2d_to
};

static uint64_t __attribute__((ms_abi))
mf_sample_query(BeerMfSample *self, const void *iid, void **output)
{
    (void)iid;
    if (!output) return BEER_E_POINTER;
    *output = self;
    atomic_fetch_add_explicit(&self->references, 1, memory_order_relaxed);
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi)) mf_sample_add_ref(BeerMfSample *self)
{
    return atomic_fetch_add_explicit(&self->references, 1, memory_order_relaxed) + 1;
}

static uint64_t __attribute__((ms_abi)) mf_sample_release(BeerMfSample *self)
{
    uint32_t references = atomic_fetch_sub_explicit(&self->references, 1,
                                                     memory_order_acq_rel) - 1;
    mf_sample_diagnostic("sample-release", self, references);
    if (!references) {
        if (self->buffer) mf_media_buffer_release(self->buffer);
        free(self);
    }
    return references;
}

static uint64_t __attribute__((ms_abi)) mf_sample_get_time(BeerMfSample *self, int64_t *value)
{ mf_sample_diagnostic("sample-time", self, (uint64_t)self->timestamp); if (!value) return BEER_E_POINTER; *value = self->timestamp; return BEER_S_OK; }
static uint64_t __attribute__((ms_abi)) mf_sample_set_time(BeerMfSample *self, int64_t value)
{ self->timestamp = value; return BEER_S_OK; }
static uint64_t __attribute__((ms_abi)) mf_sample_get_duration(BeerMfSample *self, int64_t *value)
{ if (!value) return BEER_E_POINTER; *value = self->duration; return BEER_S_OK; }
static uint64_t __attribute__((ms_abi)) mf_sample_set_duration(BeerMfSample *self, int64_t value)
{ self->duration = value; return BEER_S_OK; }
static uint64_t __attribute__((ms_abi)) mf_sample_get_buffer_count(BeerMfSample *self, uint32_t *count)
{ if (!count) return BEER_E_POINTER; *count = self->buffer ? 1 : 0; return BEER_S_OK; }
static uint64_t __attribute__((ms_abi))
mf_sample_get_buffer(BeerMfSample *self, uint32_t index, void **buffer)
{
    mf_sample_diagnostic("sample-buffer", self, index);
    if (!buffer) return BEER_E_POINTER;
    *buffer = NULL;
    if (index || !self->buffer) return BEER_E_INVALIDARG;
    mf_media_buffer_add_ref(self->buffer);
    *buffer = self->buffer;
    return BEER_S_OK;
}
static uint64_t __attribute__((ms_abi))
mf_sample_convert_to_contiguous(BeerMfSample *self, void **buffer)
{
    mf_sample_diagnostic("sample-contiguous", self,
                         self->buffer ? self->buffer->current_length : 0);
    if (!buffer) return BEER_E_POINTER;
    *buffer = NULL;
    if (!self->buffer) return BEER_E_NOTIMPL;
    mf_media_buffer_add_ref(self->buffer);
    *buffer = self->buffer;
    return BEER_S_OK;
}
static uint64_t __attribute__((ms_abi)) mf_sample_get_total_length(BeerMfSample *self, uint32_t *length)
{ if (!length) return BEER_E_POINTER; *length = self->buffer ? self->buffer->current_length : 0; return BEER_S_OK; }

static void *g_sample_vtable[47] = {
    [0] = mf_sample_query, [1] = mf_sample_add_ref, [2] = mf_sample_release,
    [3 ... 34] = mf_shell_notimpl,
    [35] = mf_sample_get_time, [36] = mf_sample_set_time,
    [37] = mf_sample_get_duration, [38] = mf_sample_set_duration,
    [39] = mf_sample_get_buffer_count, [40] = mf_sample_get_buffer,
    [41] = mf_sample_convert_to_contiguous, [42 ... 44] = mf_shell_notimpl,
    [45] = mf_sample_get_total_length, [46] = mf_shell_notimpl
};

static BeerMfSample *mf_sample_create_ex(const uint8_t *data, uint32_t size,
                                         int64_t timestamp, int64_t duration,
                                         uint32_t width, uint32_t height,
                                         int32_t stride)
{
    BeerMfMediaBuffer *buffer = calloc(1, sizeof(*buffer));
    BeerMfSample *sample = calloc(1, sizeof(*sample));
    if (!buffer || !sample) { free(buffer); free(sample); return NULL; }
    buffer->data = malloc(size);
    if (!buffer->data) { free(buffer); free(sample); return NULL; }
    memcpy(buffer->data, data, size);
    buffer->vtable = g_media_buffer_vtable;
    buffer->buffer_2d.vtable = g_2d_buffer_vtable;
    buffer->buffer_2d.owner = buffer;
    atomic_init(&buffer->references, 1);
    buffer->current_length = buffer->maximum_length = size;
    buffer->width = width;
    buffer->height = height;
    buffer->stride = stride;
    buffer->contiguous_length = size;
    sample->vtable = g_sample_vtable;
    atomic_init(&sample->references, 1);
    sample->buffer = buffer;
    sample->timestamp = timestamp;
    sample->duration = duration;
    return sample;
}

static BeerMfSample *mf_sample_create(const uint8_t *data, uint32_t size,
                                      int64_t timestamp, int64_t duration)
{
    return mf_sample_create_ex(data, size, timestamp, duration,
                               1920, 1080, 1920);
}

void *beer_mf_test_create_nv12_sample(const uint8_t *data, uint32_t size,
                                      int64_t timestamp, int64_t duration)
{
    if (!data || !size) return NULL;
    return mf_sample_create(data, size, timestamp, duration);
}

static void mf_source_reader_stop_decoder(BeerMfSourceReader *self)
{
    if (self->decoder_output) { fclose(self->decoder_output); self->decoder_output = NULL; }
    if (self->decoder_pid > 0) { waitpid(self->decoder_pid, NULL, 0); self->decoder_pid = 0; }
    if (self->audio_decoder_output) {
        fclose(self->audio_decoder_output);
        self->audio_decoder_output = NULL;
    }
    if (self->audio_decoder_pid > 0) {
        waitpid(self->audio_decoder_pid, NULL, 0);
        self->audio_decoder_pid = 0;
    }
}

static int mf_dump_stream(BeerMfShell *byte_stream, char **path_out)
{
    if (!byte_stream || !byte_stream->owner || !path_out) return 0;
    void *stream = byte_stream->owner;
    void **vtable = *(void ***)stream;
    if (!vtable || !vtable[3] || !vtable[5]) return 0;
    uint64_t original = 0, ignored = 0;
    if (((BeerStreamSeekFn)vtable[5])(stream, 0, 1, &original) != BEER_S_OK ||
        ((BeerStreamSeekFn)vtable[5])(stream, 0, 0, &ignored) != BEER_S_OK)
        return 0;
    char path[] = "/tmp/beer-mf-decode-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return 0;
    uint8_t block[64 * 1024];
    uint64_t total = 0;
    int ok = 1;
    for (;;) {
        uint32_t read = 0;
        uint64_t result = ((BeerStreamReadFn)vtable[3])(
            stream, block, sizeof(block), &read);
        /* COM IStream::Read commonly returns S_FALSE at a clean short/zero-byte
         * EOF. Bytes already returned are valid; only a failing call that also
         * produced no data terminates extraction. */
        if (!read) {
            if (result != BEER_S_OK && result != UINT64_C(1)) ok = 0;
            break;
        }
        uint32_t offset = 0;
        while (offset < read) {
            ssize_t written = write(fd, block + offset, read - offset);
            if (written <= 0) { ok = 0; break; }
            offset += (uint32_t)written;
        }
        if (!ok || (total += read) > UINT64_C(512) * 1024 * 1024) { ok = 0; break; }
    }
    close(fd);
    ((BeerStreamSeekFn)vtable[5])(stream, (int64_t)original, 0, &ignored);
    if (!ok || !total) {
        if (getenv("BEER_MF_DIAGNOSTICS"))
            fprintf(stderr, "[MF DECODE] stream extraction failed ok=%d bytes=%llu\n",
                    ok, (unsigned long long)total);
        unlink(path);
        return 0;
    }
    *path_out = strdup(path);
    if (getenv("BEER_MF_DIAGNOSTICS"))
        fprintf(stderr, "[MF DECODE] extracted %llu bytes to %s\n",
                (unsigned long long)total, path);
    return *path_out != NULL;
}

static int mf_parse_rate(const char *text, uint32_t *numerator,
                         uint32_t *denominator)
{
    if (!text || !numerator || !denominator) return 0;
    char *separator = NULL;
    unsigned long n = strtoul(text, &separator, 10);
    if (!separator || *separator != '/' || !n || n > UINT32_MAX) return 0;
    char *end = NULL;
    unsigned long d = strtoul(separator + 1, &end, 10);
    if (!end || (*end && *end != '\n') || !d || d > UINT32_MAX) return 0;
    *numerator = (uint32_t)n;
    *denominator = (uint32_t)d;
    return 1;
}

static void mf_probe_decoder_metadata(BeerMfSourceReader *self)
{
    if (!self || !self->decoder_input_path) return;
    char command[1024];
    snprintf(command, sizeof(command),
             "ffprobe -v error -select_streams v:0 "
             "-show_entries stream=width,height,avg_frame_rate,start_time "
             "-show_entries format=duration -of default=nw=1 '%s' 2>/dev/null",
             self->decoder_input_path);
    FILE *probe = popen(command, "r");
    if (!probe) return;
    char line[256];
    while (fgets(line, sizeof(line), probe)) {
        char *value = strchr(line, '=');
        if (!value) continue;
        *value++ = '\0';
        if (!strcmp(line, "width")) {
            unsigned long parsed = strtoul(value, NULL, 10);
            if (parsed && parsed <= UINT32_MAX) self->video_width = parsed;
        } else if (!strcmp(line, "height")) {
            unsigned long parsed = strtoul(value, NULL, 10);
            if (parsed && parsed <= UINT32_MAX) self->video_height = parsed;
        } else if (!strcmp(line, "avg_frame_rate")) {
            uint32_t numerator = 0, denominator = 0;
            if (mf_parse_rate(value, &numerator, &denominator)) {
                self->video_rate_numerator = numerator;
                self->video_rate_denominator = denominator;
            }
        } else if (!strcmp(line, "start_time")) {
            char *end = NULL;
            unsigned long long whole = strtoull(value, &end, 10);
            uint64_t ticks = whole <= UINT64_MAX / UINT64_C(10000000) ?
                whole * UINT64_C(10000000) : 0;
            if (end && *end == '.') {
                uint32_t scale = 1000000;
                while (*++end >= '0' && *end <= '9' && scale) {
                    ticks += (uint64_t)(*end - '0') * scale;
                    scale /= 10;
                }
            }
            if (ticks <= INT64_MAX)
                self->presentation_start_100ns = (int64_t)ticks;
        } else if (!strcmp(line, "duration")) {
            char *end = NULL;
            unsigned long long whole = strtoull(value, &end, 10);
            uint64_t ticks = whole <= UINT64_MAX / UINT64_C(10000000) ?
                whole * UINT64_C(10000000) : 0;
            if (end && *end == '.') {
                uint32_t scale = 1000000;
                while (*++end >= '0' && *end <= '9' && scale) {
                    ticks += (uint64_t)(*end - '0') * scale;
                    scale /= 10;
                }
            }
            self->duration_100ns = ticks;
        }
    }
    pclose(probe);
}

static int mf_source_reader_start_decoder(BeerMfSourceReader *self)
{
    if (!self->decoder_input_path) return 0;
    mf_source_reader_stop_decoder(self);
    int video_pipe[2], audio_pipe[2];
    if (pipe(video_pipe) != 0) return 0;
    if (pipe(audio_pipe) != 0) {
        close(video_pipe[0]); close(video_pipe[1]);
        return 0;
    }
    pid_t video_pid = fork();
    if (video_pid < 0) {
        close(video_pipe[0]); close(video_pipe[1]);
        close(audio_pipe[0]); close(audio_pipe[1]);
        return 0;
    }
    if (!video_pid) {
        dup2(video_pipe[1], STDOUT_FILENO);
        int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0) dup2(null_fd, STDERR_FILENO);
        close(video_pipe[0]); close(video_pipe[1]);
        close(audio_pipe[0]); close(audio_pipe[1]);
        execlp("ffmpeg", "ffmpeg", "-nostdin", "-v", "error", "-i",
               self->decoder_input_path, "-map", "0:v:0", "-an", "-sn",
               "-vsync", "0", "-pix_fmt", "nv12", "-f", "rawvideo",
               "pipe:1", (char *)NULL);
        _exit(127);
    }
    pid_t audio_pid = fork();
    if (audio_pid < 0) {
        close(video_pipe[0]); close(video_pipe[1]);
        close(audio_pipe[0]); close(audio_pipe[1]);
        kill(video_pid, SIGTERM); waitpid(video_pid, NULL, 0);
        return 0;
    }
    if (!audio_pid) {
        dup2(audio_pipe[1], STDOUT_FILENO);
        int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0) dup2(null_fd, STDERR_FILENO);
        close(video_pipe[0]); close(video_pipe[1]);
        close(audio_pipe[0]); close(audio_pipe[1]);
        execlp("ffmpeg", "ffmpeg", "-nostdin", "-v", "error", "-i",
               self->decoder_input_path, "-map", "0:a:0", "-vn", "-sn",
               "-ac", "2", "-ar", "48000", "-f", "s16le", "pipe:1",
               (char *)NULL);
        _exit(127);
    }
    close(video_pipe[1]);
    close(audio_pipe[1]);
    self->decoder_output = fdopen(video_pipe[0], "rb");
    self->audio_decoder_output = fdopen(audio_pipe[0], "rb");
    if (!self->decoder_output || !self->audio_decoder_output) {
        if (self->decoder_output) fclose(self->decoder_output); else close(video_pipe[0]);
        if (self->audio_decoder_output) fclose(self->audio_decoder_output); else close(audio_pipe[0]);
        self->decoder_output = self->audio_decoder_output = NULL;
        kill(video_pid, SIGTERM); kill(audio_pid, SIGTERM);
        waitpid(video_pid, NULL, 0); waitpid(audio_pid, NULL, 0);
        return 0;
    }
    self->decoder_pid = video_pid;
    self->audio_decoder_pid = audio_pid;
    self->frame_index = 0;
    self->audio_frame_index = 0;
    /* Preserve the source presentation origin for both streams. Media
     * schedulers compare these timestamps against the audio clock; inventing a
     * title-specific offset or cadence breaks normal preroll retirement. */
    self->next_video_timestamp = self->presentation_start_100ns;
    self->next_audio_timestamp = self->presentation_start_100ns;
    if (getenv("BEER_MF_DIAGNOSTICS"))
        fprintf(stderr,
                "[MF DECODE] video-pid=%d audio-pid=%d input=%s "
                "video-bytes=%u audio-bytes=%u\n",
                (int)video_pid, (int)audio_pid, self->decoder_input_path,
                self->frame_size, self->audio_frame_size);
    return 1;
}

static uint64_t __attribute__((ms_abi))
mf_source_reader_release(BeerMfSourceReader *self)
{
    uint32_t references = atomic_fetch_sub_explicit(&self->references, 1,
                                                     memory_order_acq_rel) - 1;
    if (!references) {
        mf_source_reader_stop_decoder(self);
        if (self->decoder_input_path) { unlink(self->decoder_input_path); free(self->decoder_input_path); }
        free(self->frame_data);
        free(self->audio_data);
        if (self->async_callback) mf_unknown_release(self->async_callback);
        if (self->current_media_type) mf_unknown_release(self->current_media_type);
        if (self->current_audio_media_type)
            mf_unknown_release(self->current_audio_media_type);
        if (self->native_media_type) mf_unknown_release(self->native_media_type);
        if (self->audio_media_type) mf_unknown_release(self->audio_media_type);
        if (self->owner) mf_unknown_release(self->owner);
        free(self);
    }
    return references;
}

static int *mf_source_reader_selection(BeerMfSourceReader *self, uint32_t stream)
{
    if (stream == 0 || stream == UINT32_C(0xfffffffc) ||
        stream == BEER_MF_SOURCE_READER_ANY_STREAM)
        return (int *)&self->video_selected;
    if (stream == 1 || stream == UINT32_C(0xfffffffd))
        return (int *)&self->audio_selected;
    return NULL;
}

static uint64_t __attribute__((ms_abi))
mf_source_reader_get_stream_selection(BeerMfSourceReader *self, uint32_t stream,
                                      int *selected)
{
    if (mf_diagnostic_trace())
        fprintf(stderr, "[MF DIAG] GetStreamSelection stream=%u\n", stream);
    if (!selected) return BEER_E_POINTER;
    int *state = mf_source_reader_selection(self, stream);
    if (!state) { *selected = 0; return BEER_MF_E_NOT_FOUND; }
    *selected = *state != 0;
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
mf_source_reader_set_stream_selection(BeerMfSourceReader *self, uint32_t stream,
                                      int selected)
{
    if (mf_diagnostic_trace())
        fprintf(stderr, "[MF DIAG] SetStreamSelection stream=%u selected=%d\n",
                stream, selected);
    if (stream == UINT32_C(0xfffffffe) || stream == UINT32_C(0xfffffffb)) {
        self->video_selected = selected != 0;
        self->audio_selected = selected != 0;
        return BEER_S_OK;
    }
    int *state = mf_source_reader_selection(self, stream);
    if (!state) return BEER_MF_E_NOT_FOUND;
    *state = selected != 0;
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
mf_source_reader_get_native_media_type(BeerMfSourceReader *self, uint32_t stream,
                                       uint32_t index, void **media_type)
{
    if (mf_diagnostic_trace())
        fprintf(stderr, "[MF DIAG] GetNativeMediaType stream=%u index=%u\n",
                stream, index);
    if (!media_type) return BEER_E_POINTER;
    *media_type = NULL;
    if (index != 0) return BEER_MF_E_NOT_FOUND;
    void *type = stream == 0 ? self->native_media_type
        : stream == 1 ? self->audio_media_type : NULL;
    if (!type) return BEER_MF_E_NOT_FOUND;
    mf_unknown_add_ref(type);
    *media_type = type;
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
mf_source_reader_get_current_media_type(BeerMfSourceReader *self,
                                        uint32_t stream, void **media_type)
{
    if (!media_type) return BEER_E_POINTER;
    *media_type = NULL;
    /* Expose decoded audio through both its concrete stream index and the
     * Source Reader FIRST_AUDIO_STREAM selector. */
    if (stream == UINT32_C(0xfffffffd) || stream == 1) {
        *media_type = self->current_audio_media_type;
    } else if (stream == 0 || stream == UINT32_C(0xfffffffc) ||
               stream == BEER_MF_SOURCE_READER_ANY_STREAM) {
        *media_type = self->current_media_type;
    } else {
        return BEER_MF_E_NOT_FOUND;
    }
    if (!*media_type) return BEER_MF_E_NOT_FOUND;
    if (mf_diagnostic_trace())
        fprintf(stderr, "[MF DIAG] GetCurrentMediaType stream=%u type=%p\n",
                stream, *media_type);
    mf_unknown_add_ref(*media_type);
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
mf_source_reader_set_current_media_type(BeerMfSourceReader *self,
                                        uint32_t stream, uint32_t *reserved,
                                        void *media_type)
{
    if (mf_diagnostic_trace())
        fprintf(stderr, "[MF DIAG] SetCurrentMediaType stream=%u type=%p\n",
                stream, media_type);
    if (reserved || !media_type) return BEER_E_INVALIDARG;
    void **slot = NULL;
    void *accepted_type = media_type;
    if (stream == 0 || stream == UINT32_C(0xfffffffc)) {
        slot = &self->current_media_type;
    } else if (stream == 1 || stream == UINT32_C(0xfffffffd)) {
        slot = &self->current_audio_media_type;
        /* Preserve the canonical audio object's IMFAudioMediaType identity while
         * accepting an equivalent requested PCM output type. */
        accepted_type = self->audio_media_type;
    } else {
        return BEER_MF_E_NOT_FOUND;
    }
    mf_unknown_add_ref(accepted_type);
    if (*slot) mf_unknown_release(*slot);
    *slot = accepted_type;
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
mf_source_reader_set_current_position(BeerMfSourceReader *self,
                                      const void *time_format,
                                      const void *position)
{
    (void)time_format;
    if (!position) return BEER_E_INVALIDARG;
    if (mf_diagnostic_trace())
        fprintf(stderr, "[MF DIAG] SetCurrentPosition position=%p\n", position);
    self->end_of_stream = 0;
    self->eos_reported = 0;
    self->next_video_timestamp = self->presentation_start_100ns;
    self->next_audio_timestamp = self->presentation_start_100ns;
    self->read_count = 0;
    self->reset_count++;
    atomic_store_explicit(&self->terminal_eos, 0, memory_order_release);
    if (self->decoder_input_path && !mf_source_reader_start_decoder(self))
        return BEER_E_NOTIMPL;
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
mf_source_reader_read_sample(BeerMfSourceReader *self, uint32_t stream,
                             uint32_t flags, uint32_t *actual_stream,
                             uint32_t *stream_flags, int64_t *timestamp,
                             void **sample)
{
    if (mf_diagnostic_trace())
        fprintf(stderr, "[MF DIAG] ReadSample stream=%u flags=0x%x\n",
                stream, flags);
    /* Stream selection and A/V ordering come from the requested source-reader
     * stream and decoded timestamps. Outstanding sample count is owned by the
     * caller and must not alter selection semantics. */
    /* MF_SOURCE_READER_ANY_STREAM is a selector, not an actual stream index.
     * Return the selected concrete stream with EOS so callers can retire that
     * stream instead of polling the selector forever. */
    uint32_t resolved_stream = stream;
    if (stream == BEER_MF_SOURCE_READER_ANY_STREAM) {
        if (self->video_selected && self->decoder_output &&
            (!self->audio_selected || !self->audio_decoder_output ||
             self->next_video_timestamp <= self->next_audio_timestamp))
            resolved_stream = 0;
        else if (self->audio_selected && self->audio_decoder_output)
            resolved_stream = 1;
        else
            resolved_stream = 0;
    } else if (stream == UINT32_C(0xfffffffc)) {
        resolved_stream = 0;
    } else if (stream == UINT32_C(0xfffffffd)) {
        resolved_stream = 1;
    }
    if (actual_stream) *actual_stream = resolved_stream;
    if (timestamp) *timestamp = 0;
    if (sample) *sample = NULL;
    uint32_t read_count = atomic_fetch_add_explicit(&self->read_count, 1,
                                                      memory_order_relaxed) + 1;
    uint32_t reset_count = atomic_load_explicit(&self->reset_count,
                                                 memory_order_relaxed);
    if (getenv("BEER_MF_DIAGNOSTICS") &&
        atomic_fetch_add_explicit(&g_mf_reader_state_diagnostic_count, 1,
                                  memory_order_relaxed) < 96)
        fprintf(stderr,
                "[MF STATE] ReadSample caller=%p count=%u reset=%u eos=%u "
                "actual=%p flags=%p timestamp=%p sample=%p\n",
                __builtin_return_address(0), read_count, reset_count,
                atomic_load_explicit(&self->end_of_stream, memory_order_relaxed),
                (void *)actual_stream, (void *)stream_flags, (void *)timestamp,
                (void *)sample);
    if (resolved_stream == 0 && self->video_selected && self->decoder_output &&
        self->frame_data && self->frame_size) {
        size_t received = fread(self->frame_data, 1, self->frame_size,
                                self->decoder_output);
        if (received == self->frame_size) {
            uint32_t numerator = self->video_rate_numerator ?
                self->video_rate_numerator : 60u;
            uint32_t denominator = self->video_rate_denominator ?
                self->video_rate_denominator : 1u;
            int64_t frame_duration =
                (INT64_C(10000000) * denominator + numerator / 2u) / numerator;
            BeerMfSample *decoded = mf_sample_create(
                self->frame_data, self->frame_size,
                self->next_video_timestamp, frame_duration);
            if (!decoded) return BEER_E_NOTIMPL;
            if (stream_flags) *stream_flags = 0;
            if (timestamp) *timestamp = decoded->timestamp;
            if (sample) *sample = decoded;
            else mf_sample_release(decoded);
            self->frame_index++;
            self->next_video_timestamp += frame_duration;
            mf_sample_diagnostic("read-video-sample", decoded,
                                 (uint64_t)decoded->timestamp);
            self->end_of_stream = 0;
            self->eos_reported = 0;
            atomic_store_explicit(&self->terminal_eos, 0, memory_order_release);
            /* Async Source Reader contract: complete the pending read through
             * IMFSourceReaderCallback when the caller supplied one. */
            if (self->async_callback && g_async_on_read_sample) {
                void *cb_sample = sample ? *sample : NULL;
                if (cb_sample) mf_media_buffer_add_ref(((BeerMfSample *)cb_sample)->buffer);
                if (cb_sample) atomic_fetch_add_explicit(
                    &((BeerMfSample *)cb_sample)->references, 1, memory_order_relaxed);
                g_async_on_read_sample(self->async_callback,
                                       BEER_S_OK, resolved_stream, 0,
                                       decoded->timestamp, cb_sample);
            }
            return BEER_S_OK;
        }
        int decoder_errno = errno;
        int decoder_eof = feof(self->decoder_output);
        int decoder_error = ferror(self->decoder_output);
        fclose(self->decoder_output);
        self->decoder_output = NULL;
        if (self->decoder_pid > 0) {
            int decoder_status = 0;
            waitpid(self->decoder_pid, &decoder_status, 0);
            if (getenv("BEER_MF_DIAGNOSTICS"))
                fprintf(stderr,
                        "[MF DECODE] video ended frame=%u bytes=%zu eof=%d "
                        "error=%d errno=%d status=%#x\n",
                        self->frame_index, received, decoder_eof, decoder_error,
                        decoder_errno, decoder_status);
            self->decoder_pid = 0;
        }
    }
    if (resolved_stream == 1 && self->audio_selected &&
        self->audio_decoder_output && self->audio_data && self->audio_frame_size) {
        size_t received = fread(self->audio_data, 1, self->audio_frame_size,
                                self->audio_decoder_output);
        if (received == self->audio_frame_size) {
            int64_t frame_duration = INT64_C(10000000) / 100;
            BeerMfSample *decoded = mf_sample_create_ex(
                self->audio_data, self->audio_frame_size,
                self->next_audio_timestamp, frame_duration,
                self->audio_frame_size / 8, 1, self->audio_frame_size);
            if (!decoded) return BEER_E_NOTIMPL;
            if (stream_flags) *stream_flags = 0;
            if (timestamp) *timestamp = decoded->timestamp;
            if (sample) *sample = decoded;
            else mf_sample_release(decoded);
            self->audio_frame_index++;
            self->next_audio_timestamp += frame_duration;
            if (getenv("BEER_MF_AUDIO_DUMP")) {
                const char *dump_path = getenv("BEER_MF_AUDIO_DUMP");
                FILE *dump = fopen(dump_path && *dump_path ? dump_path :
                                  "/tmp/beer-mf-audio.pcm", "ab");
                if (dump) {
                    fwrite(self->audio_data, 1, self->audio_frame_size, dump);
                    fclose(dump);
                }
            }
            mf_sample_diagnostic("read-audio-sample", decoded,
                                 (uint64_t)decoded->timestamp);
            self->end_of_stream = 0;
            self->eos_reported = 0;
            atomic_store_explicit(&self->terminal_eos, 0, memory_order_release);
            if (self->async_callback && g_async_on_read_sample) {
                void *cb_sample = sample ? *sample : NULL;
                if (cb_sample) mf_media_buffer_add_ref(((BeerMfSample *)cb_sample)->buffer);
                if (cb_sample) atomic_fetch_add_explicit(
                    &((BeerMfSample *)cb_sample)->references, 1, memory_order_relaxed);
                g_async_on_read_sample(self->async_callback,
                                       BEER_S_OK, resolved_stream, 0,
                                       decoded->timestamp, cb_sample);
            }
            return BEER_S_OK;
        }
        fclose(self->audio_decoder_output);
        self->audio_decoder_output = NULL;
        if (self->audio_decoder_pid > 0) {
            waitpid(self->audio_decoder_pid, NULL, 0);
            self->audio_decoder_pid = 0;
        }
    }
    /* End-of-stream remains a persistent stream state after the decoder drains. */
    if (stream_flags) *stream_flags = BEER_MF_SOURCE_READERF_ENDOFSTREAM;
    if (!atomic_exchange_explicit(&self->terminal_eos, 1, memory_order_acq_rel))
        fprintf(stderr, "[MF] IMFSourceReader::ReadSample stream=%u -> EOS\n", stream);
    self->eos_reported = 1;
    self->end_of_stream = 1;
    if (self->async_callback && g_async_on_read_sample)
        g_async_on_read_sample(self->async_callback,
                               BEER_S_OK, resolved_stream,
                               BEER_MF_SOURCE_READERF_ENDOFSTREAM,
                               0, NULL);
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
mf_source_reader_flush(BeerMfSourceReader *self, uint32_t stream)
{
    if (mf_diagnostic_trace())
        fprintf(stderr, "[MF DIAG] Flush stream=%u\n", stream);
    self->read_count = 0;
    self->reset_count++;
    if (!atomic_load_explicit(&self->terminal_eos, memory_order_relaxed)) {
        self->end_of_stream = 0;
        self->eos_reported = 0;
    }
    if (self->async_callback && g_async_on_flush)
        g_async_on_flush(self->async_callback, stream);
    return BEER_S_OK;
}

static uint64_t __attribute__((ms_abi))
mf_source_reader_get_service(BeerMfSourceReader *self, uint32_t stream,
                             const void *service, const void *iid, void **object)
{
    (void)self;
    (void)stream;
    (void)service;
    (void)iid;
    if (!object) return BEER_E_POINTER;
    *object = NULL;
    return BEER_E_NOINTERFACE;
}

static uint64_t __attribute__((ms_abi))
mf_source_reader_get_attribute(BeerMfSourceReader *self, uint32_t stream,
                               const BeerMfGuid *key, void *value)
{
    if (!value) return BEER_E_POINTER;
    /* PROPVARIANT is 24 bytes on Win64. RE8 requests MF_PD_DURATION from the
     * media source before it starts retiring its six queued decoded frames. */
    memset(value, 0, 24);
    if (stream == UINT32_C(0xffffffff) && key &&
        mf_guid_equal(key, &g_mf_pd_duration) && self->duration_100ns) {
        uint16_t variant_type = 21; /* VT_UI8 */
        memcpy(value, &variant_type, sizeof(variant_type));
        memcpy((uint8_t *)value + 8, &self->duration_100ns,
               sizeof(self->duration_100ns));
        if (mf_diagnostic_trace())
            fprintf(stderr, "[MF DIAG] GetPresentationAttribute duration=%llu\n",
                    (unsigned long long)self->duration_100ns);
        return BEER_S_OK;
    }
    return BEER_MF_E_NOT_FOUND;
}

/* IMFByteStream has 23 methods through SetCurrentPosition. The compatibility
 * shell owns the caller's IStream and safely rejects unimplemented I/O. */
static void *g_byte_stream_vtable[23] = {
    mf_shell_query_interface, mf_shell_add_ref, mf_shell_release,
    mf_shell_notimpl, mf_shell_notimpl, mf_shell_notimpl, mf_shell_notimpl,
    mf_shell_notimpl, mf_shell_notimpl, mf_shell_notimpl, mf_shell_notimpl,
    mf_shell_notimpl, mf_shell_notimpl, mf_shell_notimpl, mf_shell_notimpl,
    mf_shell_notimpl, mf_shell_notimpl, mf_shell_notimpl, mf_shell_notimpl,
    mf_shell_notimpl, mf_shell_notimpl, mf_shell_notimpl, mf_shell_notimpl
};

/* IMFSourceReader ABI slots 0..12. The compatibility reader reports a clean
 * end-of-stream until a decoder backend is available, while preserving the
 * media-type and stream-selection state expected by callers. */
static void *g_source_reader_vtable[13] = {
    mf_shell_query_interface, mf_shell_add_ref, mf_source_reader_release,
    mf_source_reader_get_stream_selection, mf_source_reader_set_stream_selection,
    mf_source_reader_get_native_media_type, mf_source_reader_get_current_media_type,
    mf_source_reader_set_current_media_type, mf_source_reader_set_current_position,
    mf_source_reader_read_sample, mf_source_reader_flush,
    mf_source_reader_get_service, mf_source_reader_get_attribute
};

static BeerMfShell *mf_shell_create(void **vtable, void *owner)
{
    BeerMfShell *shell = calloc(1, sizeof(*shell));
    if (!shell) return NULL;
    shell->vtable = vtable;
    atomic_init(&shell->references, 1);
    shell->owner = owner;
    if (owner) mf_unknown_add_ref(owner);
    return shell;
}

uint64_t __attribute__((ms_abi))
beer_mf_startup(uint32_t version, uint32_t flags)
{
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
    if (!reset_token || !manager) return BEER_E_INVALIDARG;
    *manager = NULL;
    *reset_token = 0;
    BeerMfDxgiManager *object = calloc(1, sizeof(*object));
    if (!object) return BEER_E_NOTIMPL;
    object->vtable = g_manager_vtable;
    atomic_init(&object->references, 1);
    atomic_flag_clear(&object->lock);
    object->reset_token = atomic_fetch_add_explicit(&g_next_dxgi_token, 1,
                                                     memory_order_relaxed);
    if (!object->reset_token)
        object->reset_token = atomic_fetch_add_explicit(&g_next_dxgi_token, 1,
                                                         memory_order_relaxed);
    *reset_token = object->reset_token;
    *manager = object;
    fprintf(stderr, "[MF] MFCreateDXGIDeviceManager token=%u -> %p\n",
            *reset_token, *manager);
    return BEER_S_OK;
}

uint64_t __attribute__((ms_abi))
beer_mf_create_attributes(void **attributes, uint32_t initial_size)
{
    if (!attributes) return BEER_E_INVALIDARG;
    *attributes = NULL;
    BeerMfAttributes *object = calloc(1, sizeof(*object));
    if (!object) return BEER_E_NOTIMPL;
    object->vtable = g_attributes_vtable;
    atomic_init(&object->references, 1);
    atomic_flag_clear(&object->lock);
    object->capacity = initial_size ? initial_size : 4;
    object->entries = calloc(object->capacity, sizeof(*object->entries));
    if (!object->entries) {
        free(object);
        return BEER_E_NOTIMPL;
    }
    *attributes = object;
    fprintf(stderr, "[MF] MFCreateAttributes initial=%u -> %p\n",
            initial_size, *attributes);
    return BEER_S_OK;
}

uint64_t __attribute__((ms_abi)) beer_mf_create_media_type(void **media_type)
{
    return beer_mf_create_attributes(media_type, 8);
}

uint64_t __attribute__((ms_abi))
beer_mf_create_byte_stream_on_stream(void *stream, void **byte_stream)
{
    if (!stream || !byte_stream) return BEER_E_INVALIDARG;
    *byte_stream = NULL;
    BeerMfShell *shell = mf_shell_create(g_byte_stream_vtable, stream);
    if (!shell) return BEER_E_NOTIMPL;
    *byte_stream = shell;
    fprintf(stderr, "[MF] MFCreateMFByteStreamOnStream stream=%p -> %p\n",
            stream, *byte_stream);
    return BEER_S_OK;
}

uint64_t __attribute__((ms_abi))
beer_mf_create_source_reader_from_byte_stream(void *byte_stream,
                                               void *attributes,
                                               void **reader)
{
    if (!byte_stream || !reader) return BEER_E_INVALIDARG;
    *reader = NULL;
    BeerMfSourceReader *shell = calloc(1, sizeof(*shell));
    if (!shell) return BEER_E_NOTIMPL;
    shell->vtable = g_source_reader_vtable;
    atomic_init(&shell->references, 1);
    shell->owner = byte_stream;
    shell->video_selected = 1;
    shell->audio_selected = 1;
    shell->end_of_stream = 1;
    shell->video_width = 1920;
    shell->video_height = 1080;
    shell->video_rate_numerator = 60;
    shell->video_rate_denominator = 1;
    shell->audio_frame_size = 480u * 2u * sizeof(int16_t); /* 10 ms at 48 kHz. */
    mf_probe_stream_header(byte_stream);
    mf_probe_stream_with_ffmpeg(byte_stream);
    if (mf_dump_stream(byte_stream, &shell->decoder_input_path))
        mf_probe_decoder_metadata(shell);
    if (!shell->video_width || !shell->video_height ||
        shell->video_width > UINT32_MAX / shell->video_height ||
        shell->video_width * shell->video_height > UINT32_MAX / 3u * 2u) {
        if (shell->decoder_input_path) {
            unlink(shell->decoder_input_path);
            free(shell->decoder_input_path);
        }
        free(shell);
        return BEER_E_NOTIMPL;
    }
    shell->frame_size = shell->video_width * shell->video_height * 3u / 2u;
    shell->frame_data = malloc(shell->frame_size);
    shell->audio_data = malloc(shell->audio_frame_size);
    if (shell->frame_data && shell->audio_data && shell->decoder_input_path &&
        !mf_source_reader_start_decoder(shell)) {
        unlink(shell->decoder_input_path);
        free(shell->decoder_input_path);
        shell->decoder_input_path = NULL;
    }
    if (beer_mf_create_media_type(&shell->native_media_type) != BEER_S_OK) {
        free(shell);
        return BEER_E_NOTIMPL;
    }
    BeerMfAttributes *native_type = shell->native_media_type;
    if (mf_attributes_set_guid(native_type, &g_mf_mt_major_type,
                               &g_mf_media_type_video) != BEER_S_OK ||
        mf_attributes_set_guid(native_type, &g_mf_mt_subtype,
                               &g_mf_video_format_nv12) != BEER_S_OK ||
        mf_attributes_set_uint64(native_type, &g_mf_mt_frame_size,
                                 ((uint64_t)shell->video_width << 32) |
                                     shell->video_height) != BEER_S_OK ||
        mf_attributes_set_uint64(native_type, &g_mf_mt_frame_rate,
                                 ((uint64_t)shell->video_rate_numerator << 32) |
                                     shell->video_rate_denominator) != BEER_S_OK) {
        mf_unknown_release(shell->native_media_type);
        free(shell);
        return BEER_E_NOTIMPL;
    }
    native_type->media_type_kind = 1;
    shell->current_media_type = shell->native_media_type;
    mf_unknown_add_ref(shell->current_media_type);

    if (beer_mf_create_media_type(&shell->audio_media_type) != BEER_S_OK) {
        mf_unknown_release(shell->current_media_type);
        mf_unknown_release(shell->native_media_type);
        free(shell->frame_data);
        free(shell->audio_data);
        free(shell);
        return BEER_E_NOTIMPL;
    }
    BeerMfAttributes *audio_type = shell->audio_media_type;
    audio_type->media_type_kind = 2;
    if (mf_attributes_set_guid(audio_type, &g_mf_mt_major_type,
                               &g_mf_media_type_audio) != BEER_S_OK ||
        mf_attributes_set_guid(audio_type, &g_mf_mt_subtype,
                               &g_mf_audio_format_pcm) != BEER_S_OK) {
        mf_unknown_release(shell->audio_media_type);
        mf_unknown_release(shell->current_media_type);
        mf_unknown_release(shell->native_media_type);
        free(shell->frame_data);
        free(shell->audio_data);
        free(shell);
        return BEER_E_NOTIMPL;
    }
    /* WAVEFORMATEX: signed PCM, stereo, 48 kHz, 16-bit. */
    uint16_t wave_tag = 1, channels = 2, block_align = 4, bits = 16;
    uint32_t sample_rate = 48000, average_bytes = 192000;
    memcpy(audio_type->wave_format + 0, &wave_tag, sizeof(wave_tag));
    memcpy(audio_type->wave_format + 2, &channels, sizeof(channels));
    memcpy(audio_type->wave_format + 4, &sample_rate, sizeof(sample_rate));
    memcpy(audio_type->wave_format + 8, &average_bytes, sizeof(average_bytes));
    memcpy(audio_type->wave_format + 12, &block_align, sizeof(block_align));
    memcpy(audio_type->wave_format + 14, &bits, sizeof(bits));
    shell->current_audio_media_type = shell->audio_media_type;
    mf_unknown_add_ref(shell->current_audio_media_type);

    /* Extract IMFSourceReaderCallback from the caller's attribute bag.
     * RE8 stores it via MF_SOURCE_READER_ASYNC_CALLBACK / SetUnknown before
     * calling MFCreateSourceReaderFromByteStream.  We hold one reference for
     * the lifetime of the reader. */
    if (attributes) {
        void *cb = NULL;
        typedef uint64_t __attribute__((ms_abi)) (*GetUnknownFn)(
            void *, const void *, const void *, void **);
        void **attr_vtable = *(void ***)attributes;
        if (attr_vtable && attr_vtable[17]) {
            uint64_t hr = ((GetUnknownFn)attr_vtable[17])(
                attributes, &g_mf_source_reader_async_callback, NULL, &cb);
            if (hr == BEER_S_OK && cb) {
                shell->async_callback = cb; /* already AddRef'd by GetUnknown */
                fprintf(stderr,
                        "[MF] IMFSourceReaderCallback registered: %p\n", cb);
            }
        }
    }

    mf_unknown_add_ref(byte_stream);
    *reader = shell;
    fprintf(stderr,
            "[MF] MFCreateSourceReaderFromByteStream stream=%p attrs=%p "
            "-> %p async_cb=%p\n",
            byte_stream, attributes, *reader, shell->async_callback);
    return BEER_S_OK;
}
