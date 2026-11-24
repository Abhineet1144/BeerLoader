/* Media Foundation's playback pipeline (see media_session.h).
 *
 * The game builds a topology (source stream nodes from a media source, output nodes holding sample grabber or audio
 * renderer activates), hands it to a media session and starts it. The session decodes each connected stream with
 * ffmpeg into the format its sink asked for, delivers the data on the presentation clock through the game's
 * IMFSampleGrabberSinkCallback, and reports progress through its event queue, as Windows' session does.
 *
 * Guest code is only called from guest-capable worker threads (beer_mf_start_worker): asynchronous callbacks and
 * events from a small work-queue pool, session commands from one thread per session, samples from one thread per
 * stream. Every COM object here is reference counted and its methods use the Windows x64 ABI. */
#define _GNU_SOURCE
#include "media_session.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Decoder threads per ffmpeg process (BEER_MF_DECODE_THREADS, default 4). ffmpeg's default (one frame thread per
 * core, each holding whole frames) took 910 MB and 72 threads to decode a 17 MB in-game video in Onimusha,
 * competing with the game for RAM and CPU; four threads decode game videos in real time. ffmpeg 8 also gave the
 * pixel-format conversion one thread per core (51 for a 4K clip): -filter_threads 2. */
static const char *mf_decode_threads(void)
{
    static char value[16];
    if (!value[0]) {
        const char *v = getenv("BEER_MF_DECODE_THREADS");
        const long n = v && *v ? strtol(v, NULL, 10) : 4;
        snprintf(value, sizeof(value), "%ld", n > 0 && n < 64 ? n : 4);
    }
    return value;
}

extern char **environ;
int beer_mf_start_worker(void *(*fn)(void *), void *arg);                          /* media_foundation.c */
uint64_t BEER_MF_MSABI beer_mf_create_attributes(void **attributes, uint32_t initial_size);
uint64_t BEER_MF_MSABI beer_mf_create_media_type(void **media_type);

#define MSABI BEER_MF_MSABI
typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
typedef int64_t s64;

/* ---- GUIDs ---------------------------------------------------------------------------------------------------- */
typedef struct { u32 d1; u16 d2, d3; u8 d4[8]; } Guid;
#define GUID_(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) { a, b, c, { d0, d1, d2, d3, d4, d5, d6, d7 } }
#define FOURCC_GUID(code) GUID_(code, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71)
static int guid_eq(const void *a, const void *b) { return a && b && !memcmp(a, b, sizeof(Guid)); }

static const Guid IID_IUnknown_ = GUID_(0x00000000, 0x0000, 0x0000, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
static const Guid IID_IMFAttributes_ = GUID_(0x2cd2d921, 0xc447, 0x44a7, 0xa1, 0x3c, 0x4a, 0xda, 0xbf, 0xc2, 0x47, 0xe3);
static const Guid IID_IMFMediaEvent_ = GUID_(0xdf598932, 0xf10c, 0x4e39, 0xbb, 0xa2, 0xc3, 0x08, 0xf1, 0x01, 0xda, 0xa3);
static const Guid IID_IMFMediaEventGenerator_ = GUID_(0x2cd0bd52, 0xbcd5, 0x4b89, 0xb6, 0x2c, 0xea, 0xdc, 0x0c, 0x03, 0x1e, 0x7d);
static const Guid IID_IMFMediaEventQueue_ = GUID_(0x36f846fc, 0x2256, 0x48b6, 0xb5, 0x8e, 0xe2, 0xb6, 0x38, 0x31, 0x65, 0x81);
static const Guid IID_IMFAsyncResult_ = GUID_(0xac6b7889, 0x0740, 0x4d51, 0x86, 0x19, 0x90, 0x59, 0x94, 0xa5, 0x5c, 0xc6);
static const Guid IID_IMFMediaSession_ = GUID_(0x90377834, 0x21d0, 0x4dee, 0x82, 0x14, 0xba, 0x2e, 0x3e, 0x6c, 0x11, 0x27);
static const Guid IID_IMFClock_ = GUID_(0x2eb1e945, 0x18b8, 0x4139, 0x9b, 0x1a, 0xd5, 0xd5, 0x84, 0x81, 0x85, 0x30);
static const Guid IID_IMFPresentationClock_ = GUID_(0x868ce85c, 0x8ea9, 0x4f55, 0xab, 0x82, 0xb0, 0x09, 0xa9, 0x10, 0xa8, 0x05);
static const Guid IID_IMFTopology_ = GUID_(0x83cf873a, 0xf6da, 0x4bc8, 0x82, 0x3f, 0xba, 0xcf, 0xd5, 0x5d, 0xc4, 0x33);
static const Guid IID_IMFTopologyNode_ = GUID_(0x83cf873a, 0xf6da, 0x4bc8, 0x82, 0x3f, 0xba, 0xcf, 0xd5, 0x5d, 0xc4, 0x30);
static const Guid IID_IMFSourceResolver_ = GUID_(0xfbe5a32d, 0xa497, 0x4b61, 0xbb, 0x85, 0x97, 0xb1, 0xa8, 0x48, 0xa6, 0xe3);
static const Guid IID_IMFMediaSource_ = GUID_(0x279a808d, 0xaec7, 0x40c8, 0x9c, 0x6b, 0xa6, 0xb4, 0x92, 0xc7, 0x8a, 0x66);
static const Guid IID_IMFPresentationDescriptor_ = GUID_(0x03cb2711, 0x24d7, 0x4db6, 0xa1, 0x7f, 0xf3, 0xa7, 0xa4, 0x79, 0xa5, 0x36);
static const Guid IID_IMFStreamDescriptor_ = GUID_(0x56c03d9c, 0x9dbb, 0x45f5, 0xab, 0x4b, 0xd8, 0x0f, 0x47, 0xc0, 0x59, 0x38);
static const Guid IID_IMFMediaTypeHandler_ = GUID_(0xe93dcf6c, 0x4b07, 0x4e1e, 0x81, 0x23, 0xaa, 0x16, 0xed, 0x6e, 0xad, 0xf5);
static const Guid IID_IMFActivate_ = GUID_(0x7fee9e9a, 0x4a89, 0x47a6, 0x89, 0x9c, 0xb6, 0xa5, 0x3a, 0x70, 0xfb, 0x67);
static const Guid IID_IMFRateControl_ = GUID_(0x88ddcd21, 0x03c3, 0x4275, 0x91, 0xed, 0x55, 0xee, 0x39, 0x29, 0x32, 0x8f);
static const Guid IID_IMFRateSupport_ = GUID_(0x0a9ccdbc, 0xd797, 0x4563, 0x96, 0x67, 0x94, 0xec, 0x5d, 0x79, 0x29, 0x2d);
static const Guid IID_IMFGetService_ = GUID_(0xfa993888, 0x4383, 0x415a, 0xa9, 0x30, 0xdd, 0x47, 0x2a, 0x8c, 0xf6, 0xf7);
static const Guid IID_IMFCollection_ = GUID_(0x5bc8a76b, 0x869a, 0x46a3, 0x9b, 0x03, 0xfa, 0x21, 0x8a, 0x66, 0xae, 0xbe);
static const Guid IID_IMFSample_ = GUID_(0xc40a00f2, 0xb93a, 0x4d80, 0xae, 0x8c, 0x5a, 0x1c, 0x63, 0x4f, 0x58, 0xe4);
static const Guid IID_IMFMediaBuffer_ = GUID_(0x045fa593, 0x8799, 0x42b8, 0xbc, 0x8d, 0x89, 0x68, 0xc6, 0x45, 0x35, 0x07);
static const Guid IID_IMFSimpleAudioVolume_ = GUID_(0x089edf13, 0xcf71, 0x4338, 0x8d, 0x13, 0x9e, 0x56, 0x9d, 0xbd, 0xc3, 0x19);

static const Guid MF_RATE_CONTROL_SERVICE_ = GUID_(0x866fa297, 0xb802, 0x4bf8, 0x9d, 0xc9, 0x5e, 0x3b, 0x6a, 0x9f, 0x53, 0xc9);
static const Guid MR_POLICY_VOLUME_SERVICE_ = GUID_(0x1abaa2ac, 0x9d3b, 0x47c6, 0xab, 0x48, 0xc5, 0x95, 0x06, 0xde, 0x78, 0x4d);
static const Guid MF_TOPONODE_STREAM_DESCRIPTOR_ = GUID_(0x835c58ee, 0xe075, 0x4bc7, 0xbc, 0xba, 0x4d, 0xe0, 0x00, 0xdf, 0x9a, 0xe6);
static const Guid MF_EVENT_TOPOLOGY_STATUS_ = GUID_(0x30c5018d, 0x9a53, 0x454b, 0xad, 0x9e, 0x6d, 0x5f, 0x8f, 0xa7, 0xc4, 0x3b);
static const Guid MF_EVENT_SESSIONCAPS_ = GUID_(0x7e5ebcd0, 0x11b8, 0x4abe, 0xaf, 0xad, 0x10, 0xf6, 0x59, 0x9a, 0x7f, 0x42);
static const Guid MF_EVENT_SESSIONCAPS_DELTA_ = GUID_(0x7e5ebcd1, 0x11b8, 0x4abe, 0xaf, 0xad, 0x10, 0xf6, 0x59, 0x9a, 0x7f, 0x42);
static const Guid MF_PD_DURATION_ = GUID_(0x6c990d33, 0xbb8e, 0x477a, 0x85, 0x98, 0x0d, 0x5d, 0x96, 0xfc, 0xd8, 0x8a);
static const Guid MF_PD_TOTAL_FILE_SIZE_ = GUID_(0x6c990d38, 0xbb8e, 0x477a, 0x85, 0x98, 0x0d, 0x5d, 0x96, 0xfc, 0xd8, 0x8a);
static const Guid MF_SAMPLEGRABBERSINK_IGNORE_CLOCK_ = GUID_(0x0efda2c0, 0x2b69, 0x4e2e, 0xab, 0x8d, 0x46, 0xdc, 0xbf, 0xf7, 0xd2, 0x5d);
static const Guid MF_SAMPLEGRABBERSINK_SAMPLE_TIME_OFFSET_ = GUID_(0x62e3d776, 0x8100, 0x4e03, 0xa6, 0xe8, 0xbd, 0x38, 0x57, 0xac, 0x9c, 0x47);
static const Guid MF_MT_MAJOR_TYPE_ = GUID_(0x48eba18e, 0xf8c9, 0x4687, 0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f);
static const Guid MF_MT_SUBTYPE_ = GUID_(0xf7e34c9a, 0x42e8, 0x4714, 0xb7, 0x4b, 0xcb, 0x29, 0xd7, 0x2c, 0x35, 0xe5);
static const Guid MF_MT_FRAME_SIZE_ = GUID_(0x1652c33d, 0xd6b2, 0x4012, 0xb8, 0x34, 0x72, 0x03, 0x08, 0x49, 0xa3, 0x7d);
static const Guid MF_MT_FRAME_RATE_ = GUID_(0xc459a2e8, 0x3d2c, 0x4e44, 0xb1, 0x32, 0xfe, 0xe5, 0x15, 0x6c, 0x7b, 0xb0);
static const Guid MF_MT_PIXEL_ASPECT_RATIO_ = GUID_(0xc6376a1e, 0x8d0a, 0x4027, 0xbe, 0x45, 0x6d, 0x9a, 0x0a, 0xd3, 0x9b, 0xb6);
static const Guid MF_MT_INTERLACE_MODE_ = GUID_(0xe2724bb8, 0xe676, 0x4806, 0xb4, 0xb2, 0xa8, 0xd6, 0xef, 0xb4, 0x4c, 0xcd);
static const Guid MF_MT_DEFAULT_STRIDE_ = GUID_(0x644b4e48, 0x1e02, 0x4516, 0xb0, 0xeb, 0xc0, 0x1c, 0xa9, 0xd4, 0x9a, 0xc6);
static const Guid MF_MT_AVG_BITRATE_ = GUID_(0x20332624, 0xfb0d, 0x4d9e, 0xbd, 0x0d, 0xcb, 0xf6, 0x78, 0x6c, 0x10, 0x2e);
static const Guid MF_MT_ALL_SAMPLES_INDEPENDENT_ = GUID_(0xc9173739, 0x5e56, 0x461c, 0xb7, 0x13, 0x46, 0xfb, 0x99, 0x5c, 0xb9, 0x5f);
static const Guid MF_MT_AUDIO_NUM_CHANNELS_ = GUID_(0x37e48bf5, 0x645e, 0x4c5b, 0x89, 0xde, 0xad, 0xa9, 0xe2, 0x9b, 0x69, 0x6a);
static const Guid MF_MT_AUDIO_SAMPLES_PER_SECOND_ = GUID_(0x5faeeae7, 0x0290, 0x4c31, 0x9e, 0x8a, 0xc5, 0x34, 0xf6, 0x8d, 0x9d, 0xba);
static const Guid MF_MT_AUDIO_BITS_PER_SAMPLE_ = GUID_(0xf2deb57f, 0x40fa, 0x4764, 0xaa, 0x33, 0xed, 0x4f, 0x2d, 0x1f, 0xf6, 0x69);
static const Guid MF_MT_AUDIO_BLOCK_ALIGNMENT_ = GUID_(0x322de230, 0x9eeb, 0x43bd, 0xab, 0x7a, 0xff, 0x41, 0x22, 0x51, 0x54, 0x1d);
static const Guid MF_MT_AUDIO_AVG_BYTES_PER_SECOND_ = GUID_(0x1aab75c8, 0xcfef, 0x451c, 0xab, 0x95, 0xac, 0x03, 0x4b, 0x8e, 0x17, 0x31);
static const Guid MFMediaType_Video_ = FOURCC_GUID(0x73646976);                 /* 'vids' */
static const Guid MFMediaType_Audio_ = FOURCC_GUID(0x73647561);                 /* 'auds' */

/* ---- Constants -------------------------------------------------------------------------------------------------- */
#define S_OK_ 0u
#define S_FALSE_ 1u
#define E_NOTIMPL_ 0x80004001u
#define E_NOINTERFACE_ 0x80004002u
#define E_POINTER_ 0x80004003u
#define E_FAIL_ 0x80004005u
#define E_OUTOFMEMORY_ 0x8007000Eu
#define E_INVALIDARG_ 0x80070057u
#define MF_E_INVALIDREQUEST_ 0xC00D36B2u
#define MF_E_UNSUPPORTED_SERVICE_ 0xC00D36BAu
#define MF_E_MULTIPLE_BEGIN_ 0xC00D36D9u
#define MF_E_UNSUPPORTED_SCHEME_ 0xC00D36C3u
#define MF_E_UNSUPPORTED_BYTESTREAM_TYPE_ 0xC00D36C4u
#define MF_E_INVALIDINDEX_ 0xC00D36BFu
#define MF_E_NOT_FOUND_ 0xC00D36D5u
#define MF_E_NO_EVENTS_AVAILABLE_ 0xC00D3E80u
#define MF_E_SHUTDOWN_ 0xC00D3E85u
#define MF_E_REVERSE_UNSUPPORTED_ 0xC00D3E9Bu
#define MF_E_UNSUPPORTED_RATE_ 0xC00D3E9Cu
#define MF_E_CLOCK_NO_TIME_SOURCE_ 0xC00D9C41u

enum { VT_EMPTY_ = 0, VT_R4_ = 4, VT_I8_ = 20, VT_UI8_ = 21, VT_UNKNOWN_ = 13 };
enum {                                                                           /* MediaEventType */
    MEError_ = 1, MESessionTopologySet_ = 101, MESessionTopologiesCleared_ = 102, MESessionStarted_ = 103,
    MESessionPaused_ = 104, MESessionStopped_ = 105, MESessionClosed_ = 106, MESessionEnded_ = 107,
    MESessionRateChanged_ = 108, MESessionCapabilitiesChanged_ = 110, MESessionTopologyStatus_ = 111,
    MESourceStarted_ = 201, MESourceStopped_ = 207, MESourcePaused_ = 209, MEEndOfPresentation_ = 211,
};
enum { MF_TOPOSTATUS_READY_ = 100, MF_TOPOSTATUS_STARTED_SOURCE_ = 200, MF_TOPOSTATUS_ENDED_ = 400 };
enum { MF_TOPOLOGY_OUTPUT_NODE_ = 0, MF_TOPOLOGY_SOURCESTREAM_NODE_ = 1 };
#define SESSION_CAPS (1u | 2u | 4u | 0x10u | 0x40u)   /* START, SEEK, PAUSE, RATE_FORWARD, DOES_NOT_USE_NETWORK */

/* ---- Diagnostics ------------------------------------------------------------------------------------------------ */
static int mfs_diag(void)
{
    static int on = -1;
    if (on < 0) on = getenv("BEER_MF_DIAGNOSTICS") != NULL;
    return on;
}
#define DIAG(...) do { if (mfs_diag()) fprintf(stderr, "[MF SESSION] " __VA_ARGS__); } while (0)
static void diag_guid(const char *what, const void *iid)
{
    if (!mfs_diag() || !iid) return;
    const Guid *g = iid;
    fprintf(stderr, "[MF SESSION] %s %08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x\n", what, g->d1, g->d2, g->d3,
            g->d4[0], g->d4[1], g->d4[2], g->d4[3], g->d4[4], g->d4[5], g->d4[6], g->d4[7]);
}

/* ---- COM helpers ------------------------------------------------------------------------------------------------ */
#define VTBL(object) (*(void ***)(object))
typedef u64 MSABI (*Fn1)(void *);
typedef u64 MSABI (*QueryFn)(void *, const void *, void **);
static void com_add_ref(void *object) { if (object) ((Fn1)VTBL(object)[1])(object); }
static void com_release(void *object) { if (object) ((Fn1)VTBL(object)[2])(object); }
static u64 com_query(void *object, const void *iid, void **out) { return ((QueryFn)VTBL(object)[0])(object, iid, out); }

/* IMFAttributes on a guest or host object, through its vtable. */
typedef u64 MSABI (*AttrGetU32Fn)(void *, const void *, u32 *);
typedef u64 MSABI (*AttrGetU64Fn)(void *, const void *, u64 *);
typedef u64 MSABI (*AttrGetGuidFn)(void *, const void *, Guid *);
typedef u64 MSABI (*AttrGetUnknownFn)(void *, const void *, const void *, void **);
typedef u64 MSABI (*AttrSetU32Fn)(void *, const void *, u32);
typedef u64 MSABI (*AttrSetU64Fn)(void *, const void *, u64);
typedef u64 MSABI (*AttrSetGuidFn)(void *, const void *, const void *);
typedef u64 MSABI (*AttrSetUnknownFn)(void *, const void *, void *);
static int attr_u32(void *attributes, const Guid *key, u32 *value)
{ return attributes && ((AttrGetU32Fn)VTBL(attributes)[7])(attributes, key, value) == S_OK_; }
static int attr_u64(void *attributes, const Guid *key, u64 *value)
{ return attributes && ((AttrGetU64Fn)VTBL(attributes)[8])(attributes, key, value) == S_OK_; }
static int attr_guid(void *attributes, const Guid *key, Guid *value)
{ return attributes && ((AttrGetGuidFn)VTBL(attributes)[10])(attributes, key, value) == S_OK_; }
static void *attr_unknown(void *attributes, const Guid *key)                      /* a new reference, or NULL */
{
    void *value = NULL;
    if (!attributes || ((AttrGetUnknownFn)VTBL(attributes)[17])(attributes, key, &IID_IUnknown_, &value) != S_OK_)
        return NULL;
    return value;
}
static void set_u32(void *attributes, const Guid *key, u32 value) { ((AttrSetU32Fn)VTBL(attributes)[21])(attributes, key, value); }
static void set_u64(void *attributes, const Guid *key, u64 value) { ((AttrSetU64Fn)VTBL(attributes)[22])(attributes, key, value); }
static void set_guid(void *attributes, const Guid *key, const Guid *value) { ((AttrSetGuidFn)VTBL(attributes)[24])(attributes, key, value); }

/* Objects that inherit IMFAttributes keep a store from media_foundation.c right after their vtable pointer: slots
 * 3..32 of their vtables are these thunks, which call the same slot of that store with every argument unchanged. */
__asm__(".text\n"
        ".macro MFS_ATTR_THUNK n\n"
        "mfs_attr_thunk_\\n:\n"
        "    movq 8(%rcx), %rcx\n"
        "    movq (%rcx), %rax\n"
        "    jmpq *(\\n * 8)(%rax)\n"
        ".endm\n"
        ".irp n, 3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32\n"
        "    MFS_ATTR_THUNK \\n\n"
        ".endr\n"
        ".section .data.rel.ro,\"aw\"\n"
        ".balign 8\n"
        "mfs_attr_thunks:\n"
        ".irp n, 3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32\n"
        "    .quad mfs_attr_thunk_\\n\n"
        ".endr\n"
        ".text\n");
extern void *const mfs_attr_thunks[30];
static void fill_attribute_slots(void **vtable) { for (int i = 0; i < 30; ++i) vtable[3 + i] = mfs_attr_thunks[i]; }

typedef struct AttrObject {                    /* common head of every object that inherits IMFAttributes */
    void **vtable;
    void *attributes;                          /* media_foundation.c store, at offset 8 for the thunks */
    _Atomic(u32) references;
} AttrObject;
static int attr_object_init(AttrObject *object, void **vtable)
{
    object->vtable = vtable;
    atomic_store(&object->references, 1);
    return beer_mf_create_attributes(&object->attributes, 4) == S_OK_;
}

/* ---- Time ------------------------------------------------------------------------------------------------------- */
static s64 now_100ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (s64)ts.tv_sec * 10000000 + ts.tv_nsec / 100;
}
uint64_t MSABI beer_mfs_get_system_time(void) { return (u64)now_100ns(); }

/* ---- Host services ---------------------------------------------------------------------------------------------- */
static BeerMfPathResolveFn g_resolve_path;
static BeerMfWaitHandleFn g_wait_handle;
void beer_mf_session_set_host(BeerMfPathResolveFn resolve_path, BeerMfWaitHandleFn wait_handle)
{
    g_resolve_path = resolve_path;
    g_wait_handle = wait_handle;
}

/* ================================================================================================================
 * Async results and the work queue
 * ================================================================================================================ */
typedef struct AsyncResult {
    void **vtable;
    _Atomic(u32) references;
    void *callback, *state, *object;           /* references held */
    _Atomic(u32) status;
    void *event;                               /* IMFMediaEventQueue::EndGetEvent: the event this result carries */
} AsyncResult;
static void *g_async_result_vtable[8];

static u64 MSABI async_query(AsyncResult *self, const void *iid, void **out)
{
    if (!out) return E_POINTER_;
    *out = NULL;
    if (!guid_eq(iid, &IID_IUnknown_) && !guid_eq(iid, &IID_IMFAsyncResult_)) return E_NOINTERFACE_;
    atomic_fetch_add(&self->references, 1);
    *out = self;
    return S_OK_;
}
static u64 MSABI async_add_ref(AsyncResult *self) { return atomic_fetch_add(&self->references, 1) + 1; }
static u64 MSABI async_release(AsyncResult *self)
{
    const u32 left = atomic_fetch_sub(&self->references, 1) - 1;
    if (!left) {
        com_release(self->callback);
        com_release(self->state);
        com_release(self->object);
        com_release(self->event);
        free(self);
    }
    return left;
}
static u64 MSABI async_get_state(AsyncResult *self, void **state)
{
    if (!state) return E_POINTER_;
    *state = self->state;
    if (!self->state) return E_POINTER_;
    com_add_ref(self->state);
    return S_OK_;
}
static u64 MSABI async_get_status(AsyncResult *self) { return atomic_load(&self->status); }
static u64 MSABI async_set_status(AsyncResult *self, u32 status) { atomic_store(&self->status, status); return S_OK_; }
static u64 MSABI async_get_object(AsyncResult *self, void **object)
{
    if (!object) return E_POINTER_;
    *object = self->object;
    if (!self->object) return E_POINTER_;
    com_add_ref(self->object);
    return S_OK_;
}
static u64 MSABI async_get_state_no_add_ref(AsyncResult *self) { return (u64)(uintptr_t)self->state; }

static AsyncResult *async_new(void *object, void *callback, void *state)
{
    AsyncResult *result = calloc(1, sizeof(*result));
    if (!result) return NULL;
    result->vtable = g_async_result_vtable;
    atomic_store(&result->references, 1);
    result->object = object; com_add_ref(object);
    result->callback = callback; com_add_ref(callback);
    result->state = state; com_add_ref(state);
    return result;
}
uint64_t MSABI beer_mfs_create_async_result(void *object, void *callback, void *state, void **result)
{
    if (!result) return E_POINTER_;
    *result = async_new(object, callback, state);
    return *result ? S_OK_ : E_OUTOFMEMORY_;
}

/* The work queue: IMFAsyncCallback::Invoke(result) on a pool of guest-capable threads, in submission order per
 * thread. More threads start while all are busy, so a callback that waits for another one cannot stall the queue. */
typedef struct WorkItem { AsyncResult *result; struct WorkItem *next; } WorkItem;
static pthread_mutex_t g_work_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_work_cond = PTHREAD_COND_INITIALIZER;
static WorkItem *g_work_head, *g_work_tail;
static int g_work_threads, g_work_idle;
#define WORK_THREADS_MAX 16

static void invoke_result(AsyncResult *result)
{
    void *callback = result->callback;
    if (callback) ((u64 MSABI (*)(void *, void *))VTBL(callback)[4])(callback, result);   /* IMFAsyncCallback::Invoke */
}
static void *work_thread(void *unused)
{
    (void)unused;
    pthread_mutex_lock(&g_work_lock);
    for (;;) {
        while (!g_work_head) {
            ++g_work_idle;
            pthread_cond_wait(&g_work_cond, &g_work_lock);
            --g_work_idle;
        }
        WorkItem *item = g_work_head;
        g_work_head = item->next;
        if (!g_work_head) g_work_tail = NULL;
        pthread_mutex_unlock(&g_work_lock);
        invoke_result(item->result);
        async_release(item->result);
        free(item);
        pthread_mutex_lock(&g_work_lock);
    }
    return NULL;
}
/* Takes over the caller's reference to `result`. */
static u64 work_submit(AsyncResult *result)
{
    WorkItem *item = malloc(sizeof(*item));
    if (!item) { async_release(result); return E_OUTOFMEMORY_; }
    item->result = result;
    item->next = NULL;
    pthread_mutex_lock(&g_work_lock);
    if (g_work_tail) g_work_tail->next = item; else g_work_head = item;
    g_work_tail = item;
    const int start = g_work_idle == 0 && g_work_threads < WORK_THREADS_MAX;
    if (start) ++g_work_threads;
    pthread_cond_signal(&g_work_cond);
    pthread_mutex_unlock(&g_work_lock);
    if (start && !beer_mf_start_worker(work_thread, NULL)) {
        pthread_mutex_lock(&g_work_lock);
        --g_work_threads;
        pthread_mutex_unlock(&g_work_lock);
        if (!g_work_threads) fprintf(stderr, "[MF SESSION] cannot start a work-queue thread\n");
    }
    return S_OK_;
}
uint64_t MSABI beer_mfs_invoke_callback(void *result)
{
    if (!result) return E_POINTER_;
    if (VTBL(result) != g_async_result_vtable) return E_INVALIDARG_;   /* MFInvokeCallback takes MF's own results */
    async_add_ref(result);
    return work_submit(result);
}
uint64_t MSABI beer_mfs_put_work_item(uint32_t queue, void *callback, void *state)
{
    (void)queue;
    if (!callback) return E_POINTER_;
    AsyncResult *result = async_new(NULL, callback, state);
    return result ? work_submit(result) : E_OUTOFMEMORY_;
}
uint64_t MSABI beer_mfs_put_work_item_ex(uint32_t queue, void *result)
{
    (void)queue;
    return beer_mfs_invoke_callback(result);
}
/* MFPutWaitingWorkItem: the result's callback runs once the event handle is signaled. */
typedef struct { u64 handle; AsyncResult *result; } WaitingItem;
static void *waiting_item_thread(void *opaque)
{
    WaitingItem item = *(WaitingItem *)opaque;
    free(opaque);
    if (g_wait_handle) g_wait_handle(item.handle, 0xffffffffu);
    work_submit(item.result);
    return NULL;
}
uint64_t MSABI beer_mfs_put_waiting_work_item(uint64_t event, int32_t priority, void *result, uint64_t *key)
{
    (void)priority;
    if (!result || VTBL(result) != g_async_result_vtable) return E_INVALIDARG_;
    WaitingItem *item = malloc(sizeof(*item));
    if (!item) return E_OUTOFMEMORY_;
    item->handle = event;
    item->result = result;
    async_add_ref(result);
    if (key) *key = (u64)(uintptr_t)result;
    if (!event || !beer_mf_start_worker(waiting_item_thread, item)) {   /* no handle: run now */
        free(item);
        return work_submit(result);
    }
    return S_OK_;
}
uint64_t MSABI beer_mfs_cancel_work_item(uint64_t key) { (void)key; return MF_E_NOT_FOUND_; }

/* ================================================================================================================
 * Media events and event queues
 * ================================================================================================================ */
typedef struct MediaEvent {
    AttrObject base;
    u32 type;
    Guid extended_type;
    u32 status;
    u8 value[24];                              /* PROPVARIANT: VT_EMPTY, VT_R4, VT_I8/VT_UI8 or VT_UNKNOWN (held) */
} MediaEvent;
static void *g_media_event_vtable[37];

static u64 MSABI event_query(MediaEvent *self, const void *iid, void **out)
{
    if (!out) return E_POINTER_;
    *out = NULL;
    if (!guid_eq(iid, &IID_IUnknown_) && !guid_eq(iid, &IID_IMFAttributes_) && !guid_eq(iid, &IID_IMFMediaEvent_))
        return E_NOINTERFACE_;
    atomic_fetch_add(&self->base.references, 1);
    *out = self;
    return S_OK_;
}
static u64 MSABI event_add_ref(MediaEvent *self) { return atomic_fetch_add(&self->base.references, 1) + 1; }
static u64 MSABI event_release(MediaEvent *self)
{
    const u32 left = atomic_fetch_sub(&self->base.references, 1) - 1;
    if (!left) {
        if (*(u16 *)self->value == VT_UNKNOWN_) com_release(*(void **)(self->value + 8));
        com_release(self->base.attributes);
        free(self);
    }
    return left;
}
static u64 MSABI event_get_type(MediaEvent *self, u32 *type) { if (!type) return E_POINTER_; *type = self->type; return S_OK_; }
static u64 MSABI event_get_extended_type(MediaEvent *self, Guid *type)
{ if (!type) return E_POINTER_; *type = self->extended_type; return S_OK_; }
static u64 MSABI event_get_status(MediaEvent *self, u32 *status) { if (!status) return E_POINTER_; *status = self->status; return S_OK_; }
static u64 MSABI event_get_value(MediaEvent *self, u8 *value)
{
    if (!value) return E_POINTER_;
    memcpy(value, self->value, 24);
    if (*(u16 *)value == VT_UNKNOWN_) com_add_ref(*(void **)(value + 8));
    return S_OK_;
}
static MediaEvent *event_new(u32 type, const Guid *extended, u32 status, const u8 *value)
{
    MediaEvent *event = calloc(1, sizeof(*event));
    if (!event || !attr_object_init(&event->base, g_media_event_vtable)) { free(event); return NULL; }
    event->type = type;
    if (extended) event->extended_type = *extended;
    event->status = status;
    if (value) {
        const u16 vt = *(const u16 *)value;
        if (vt == VT_EMPTY_ || vt == VT_R4_ || vt == VT_I8_ || vt == VT_UI8_ || vt == VT_UNKNOWN_ || vt == 3 || vt == 19) {
            memcpy(event->value, value, 24);
            if (vt == VT_UNKNOWN_) com_add_ref(*(void **)(event->value + 8));
        }
    }
    return event;
}
uint64_t MSABI beer_mfs_create_media_event(uint32_t type, const void *extended_type, uint32_t status,
                                          const void *value, void **event)
{
    if (!event) return E_POINTER_;
    *event = event_new(type, extended_type, status, value);
    return *event ? S_OK_ : E_OUTOFMEMORY_;
}

typedef struct EventQueue {
    void **vtable;
    _Atomic(u32) references;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    void *events[512];
    u32 head, count;
    void *pending_callback, *pending_state;    /* BeginGetEvent waiting for an event */
    int shutdown;
} EventQueue;
static void *g_event_queue_vtable[10];

static u64 MSABI queue_query(EventQueue *self, const void *iid, void **out)
{
    if (!out) return E_POINTER_;
    *out = NULL;
    if (!guid_eq(iid, &IID_IUnknown_) && !guid_eq(iid, &IID_IMFMediaEventQueue_)) return E_NOINTERFACE_;
    atomic_fetch_add(&self->references, 1);
    *out = self;
    return S_OK_;
}
static u64 MSABI queue_add_ref(EventQueue *self) { return atomic_fetch_add(&self->references, 1) + 1; }
static void queue_drain_locked(EventQueue *self)
{
    while (self->count) {
        com_release(self->events[self->head]);
        self->head = (self->head + 1) % 512;
        --self->count;
    }
}
static u64 MSABI queue_release(EventQueue *self)
{
    const u32 left = atomic_fetch_sub(&self->references, 1) - 1;
    if (!left) {
        queue_drain_locked(self);
        com_release(self->pending_callback);
        com_release(self->pending_state);
        pthread_mutex_destroy(&self->lock);
        pthread_cond_destroy(&self->cond);
        free(self);
    }
    return left;
}
/* With the lock held: hands the oldest event to a waiting BeginGetEvent. Returns the result to submit. */
static AsyncResult *queue_match_locked(EventQueue *self)
{
    if (!self->pending_callback || !self->count) return NULL;
    void *event = self->events[self->head];
    self->head = (self->head + 1) % 512;
    --self->count;
    AsyncResult *result = async_new(NULL, self->pending_callback, self->pending_state);
    com_release(self->pending_callback);
    com_release(self->pending_state);
    self->pending_callback = self->pending_state = NULL;
    if (!result) { com_release(event); return NULL; }
    result->event = event;                     /* reference moves into the result */
    return result;
}
static u64 MSABI queue_get_event(EventQueue *self, u32 flags, void **event)
{
    if (!event) return E_POINTER_;
    *event = NULL;
    pthread_mutex_lock(&self->lock);
    if (self->pending_callback) { pthread_mutex_unlock(&self->lock); return MF_E_MULTIPLE_BEGIN_; }
    while (!self->count && !self->shutdown && !(flags & 1u)) pthread_cond_wait(&self->cond, &self->lock);
    u64 hr = S_OK_;
    if (self->shutdown) hr = MF_E_SHUTDOWN_;
    else if (!self->count) hr = MF_E_NO_EVENTS_AVAILABLE_;                       /* MF_EVENT_FLAG_NO_WAIT */
    else {
        *event = self->events[self->head];
        self->head = (self->head + 1) % 512;
        --self->count;
    }
    pthread_mutex_unlock(&self->lock);
    return hr;
}
static u64 MSABI queue_begin_get_event(EventQueue *self, void *callback, void *state)
{
    if (!callback) return E_INVALIDARG_;
    pthread_mutex_lock(&self->lock);
    if (self->shutdown) { pthread_mutex_unlock(&self->lock); return MF_E_SHUTDOWN_; }
    if (self->pending_callback) { pthread_mutex_unlock(&self->lock); return MF_E_MULTIPLE_BEGIN_; }
    self->pending_callback = callback; com_add_ref(callback);
    self->pending_state = state; com_add_ref(state);
    AsyncResult *result = queue_match_locked(self);
    pthread_mutex_unlock(&self->lock);
    if (result) work_submit(result);
    return S_OK_;
}
static u64 MSABI queue_end_get_event(EventQueue *self, void *result, void **event)
{
    (void)self;
    if (!result || !event) return E_POINTER_;
    *event = NULL;
    if (VTBL(result) != g_async_result_vtable) return E_INVALIDARG_;
    AsyncResult *async = result;
    if (!async->event) return E_FAIL_;
    *event = async->event;
    com_add_ref(*event);
    return S_OK_;
}
/* Takes over the caller's reference to `event`. */
static u64 queue_push(EventQueue *self, void *event)
{
    pthread_mutex_lock(&self->lock);
    if (self->shutdown) { pthread_mutex_unlock(&self->lock); com_release(event); return MF_E_SHUTDOWN_; }
    if (self->count == 512) { pthread_mutex_unlock(&self->lock); com_release(event); return E_OUTOFMEMORY_; }
    self->events[(self->head + self->count) % 512] = event;
    ++self->count;
    pthread_cond_broadcast(&self->cond);
    AsyncResult *result = queue_match_locked(self);
    pthread_mutex_unlock(&self->lock);
    if (result) work_submit(result);
    return S_OK_;
}
static u64 MSABI queue_queue_event(EventQueue *self, void *event)
{
    if (!event) return E_POINTER_;
    com_add_ref(event);
    return queue_push(self, event);
}
static u64 MSABI queue_queue_event_param_var(EventQueue *self, u32 type, const Guid *extended, u32 status, const u8 *value)
{
    MediaEvent *event = event_new(type, extended, status, value);
    return event ? queue_push(self, event) : E_OUTOFMEMORY_;
}
static u64 MSABI queue_queue_event_param_unk(EventQueue *self, u32 type, const Guid *extended, u32 status, void *unknown)
{
    u8 value[24] = {0};
    if (unknown) { *(u16 *)value = VT_UNKNOWN_; *(void **)(value + 8) = unknown; }
    return queue_queue_event_param_var(self, type, extended, status, value);
}
static u64 MSABI queue_shutdown(EventQueue *self)
{
    pthread_mutex_lock(&self->lock);
    self->shutdown = 1;
    queue_drain_locked(self);
    void *callback = self->pending_callback, *state = self->pending_state;
    self->pending_callback = self->pending_state = NULL;
    pthread_cond_broadcast(&self->cond);
    pthread_mutex_unlock(&self->lock);
    com_release(callback);
    com_release(state);
    return S_OK_;
}
static EventQueue *queue_new(void)
{
    EventQueue *queue = calloc(1, sizeof(*queue));
    if (!queue) return NULL;
    queue->vtable = g_event_queue_vtable;
    atomic_store(&queue->references, 1);
    pthread_mutex_init(&queue->lock, NULL);
    pthread_cond_init(&queue->cond, NULL);
    return queue;
}
uint64_t MSABI beer_mfs_create_event_queue(void **queue)
{
    if (!queue) return E_POINTER_;
    *queue = queue_new();
    return *queue ? S_OK_ : E_OUTOFMEMORY_;
}
/* A queued event with a plain value (VT_EMPTY when value is NULL) and optional UINT32 attributes. */
static void queue_post(EventQueue *queue, u32 type, u32 status, const u8 *value,
                       const Guid *key1, u32 value1, const Guid *key2, u32 value2)
{
    MediaEvent *event = event_new(type, NULL, status, value);
    if (!event) return;
    if (key1) set_u32(event->base.attributes, key1, value1);
    if (key2) set_u32(event->base.attributes, key2, value2);
    DIAG("event %u status 0x%x\n", type, status);
    queue_push(queue, event);
}

/* ================================================================================================================
 * Collections
 * ================================================================================================================ */
typedef struct Collection {
    void **vtable;
    _Atomic(u32) references;
    pthread_mutex_t lock;
    void **items;
    u32 count, capacity;
} Collection;
static void *g_collection_vtable[9];
static u64 MSABI collection_query(Collection *self, const void *iid, void **out)
{
    if (!out) return E_POINTER_;
    *out = NULL;
    if (!guid_eq(iid, &IID_IUnknown_) && !guid_eq(iid, &IID_IMFCollection_)) return E_NOINTERFACE_;
    atomic_fetch_add(&self->references, 1);
    *out = self;
    return S_OK_;
}
static u64 MSABI collection_add_ref(Collection *self) { return atomic_fetch_add(&self->references, 1) + 1; }
static u64 MSABI collection_remove_all(Collection *self)
{
    pthread_mutex_lock(&self->lock);
    for (u32 i = 0; i < self->count; ++i) com_release(self->items[i]);
    self->count = 0;
    pthread_mutex_unlock(&self->lock);
    return S_OK_;
}
static u64 MSABI collection_release(Collection *self)
{
    const u32 left = atomic_fetch_sub(&self->references, 1) - 1;
    if (!left) {
        collection_remove_all(self);
        free(self->items);
        pthread_mutex_destroy(&self->lock);
        free(self);
    }
    return left;
}
static u64 MSABI collection_get_count(Collection *self, u32 *count)
{ if (!count) return E_POINTER_; *count = self->count; return S_OK_; }
static u64 MSABI collection_get_element(Collection *self, u32 index, void **element)
{
    if (!element) return E_POINTER_;
    pthread_mutex_lock(&self->lock);
    const u64 hr = index < self->count ? S_OK_ : E_INVALIDARG_;
    *element = index < self->count ? self->items[index] : NULL;
    com_add_ref(*element);
    pthread_mutex_unlock(&self->lock);
    return hr;
}
static u64 MSABI collection_insert_at(Collection *self, u32 index, void *element)
{
    pthread_mutex_lock(&self->lock);
    if (index > self->count) index = self->count;
    if (self->count == self->capacity) {
        const u32 capacity = self->capacity ? self->capacity * 2 : 8;
        void **items = realloc(self->items, capacity * sizeof(*items));
        if (!items) { pthread_mutex_unlock(&self->lock); return E_OUTOFMEMORY_; }
        self->items = items;
        self->capacity = capacity;
    }
    memmove(self->items + index + 1, self->items + index, (self->count - index) * sizeof(*self->items));
    self->items[index] = element;
    com_add_ref(element);
    ++self->count;
    pthread_mutex_unlock(&self->lock);
    return S_OK_;
}
static u64 MSABI collection_add(Collection *self, void *element) { return collection_insert_at(self, 0xffffffffu, element); }
static u64 MSABI collection_remove(Collection *self, u32 index, void **element)
{
    if (!element) return E_POINTER_;
    pthread_mutex_lock(&self->lock);
    if (index >= self->count) { pthread_mutex_unlock(&self->lock); return E_INVALIDARG_; }
    *element = self->items[index];                                               /* the reference moves out */
    memmove(self->items + index, self->items + index + 1, (self->count - index - 1) * sizeof(*self->items));
    --self->count;
    pthread_mutex_unlock(&self->lock);
    return S_OK_;
}
static Collection *collection_new(void)
{
    Collection *collection = calloc(1, sizeof(*collection));
    if (!collection) return NULL;
    collection->vtable = g_collection_vtable;
    atomic_store(&collection->references, 1);
    pthread_mutex_init(&collection->lock, NULL);
    return collection;
}
uint64_t MSABI beer_mfs_create_collection(void **collection)
{
    if (!collection) return E_POINTER_;
    *collection = collection_new();
    return *collection ? S_OK_ : E_OUTOFMEMORY_;
}

/* ================================================================================================================
 * Topologies
 * ================================================================================================================ */
#define NODE_PINS 8
typedef struct TopologyNode TopologyNode;
struct TopologyNode {
    AttrObject base;
    pthread_mutex_t lock;
    u32 type;
    u64 id;
    void *object;                                        /* SetObject (held) */
    struct { TopologyNode *node; u32 index; } inputs[NODE_PINS], outputs[NODE_PINS];   /* outputs held, inputs weak */
    void *output_types[NODE_PINS], *input_types[NODE_PINS];
};
static void *g_node_vtable[49];
static _Atomic(u64) g_next_topology_id = 1;

static u64 MSABI node_query(TopologyNode *self, const void *iid, void **out)
{
    if (!out) return E_POINTER_;
    *out = NULL;
    if (!guid_eq(iid, &IID_IUnknown_) && !guid_eq(iid, &IID_IMFAttributes_) && !guid_eq(iid, &IID_IMFTopologyNode_))
        return E_NOINTERFACE_;
    atomic_fetch_add(&self->base.references, 1);
    *out = self;
    return S_OK_;
}
static u64 MSABI node_add_ref(TopologyNode *self) { return atomic_fetch_add(&self->base.references, 1) + 1; }
static u64 MSABI node_release(TopologyNode *self)
{
    const u32 left = atomic_fetch_sub(&self->base.references, 1) - 1;
    if (!left) {
        com_release(self->object);
        for (int i = 0; i < NODE_PINS; ++i) {
            if (self->outputs[i].node) com_release(self->outputs[i].node);
            com_release(self->output_types[i]);
            com_release(self->input_types[i]);
        }
        com_release(self->base.attributes);
        pthread_mutex_destroy(&self->lock);
        free(self);
    }
    return left;
}
static u64 MSABI node_set_object(TopologyNode *self, void *object)
{
    com_add_ref(object);
    pthread_mutex_lock(&self->lock);
    void *old = self->object;
    self->object = object;
    pthread_mutex_unlock(&self->lock);
    com_release(old);
    return S_OK_;
}
static u64 MSABI node_get_object(TopologyNode *self, void **object)
{
    if (!object) return E_POINTER_;
    pthread_mutex_lock(&self->lock);
    *object = self->object;
    com_add_ref(*object);
    pthread_mutex_unlock(&self->lock);
    return *object ? S_OK_ : E_FAIL_;
}
static u64 MSABI node_get_type(TopologyNode *self, u32 *type) { if (!type) return E_POINTER_; *type = self->type; return S_OK_; }
static u64 MSABI node_get_id(TopologyNode *self, u64 *id) { if (!id) return E_POINTER_; *id = self->id; return S_OK_; }
static u64 MSABI node_set_id(TopologyNode *self, u64 id) { self->id = id; return S_OK_; }
static u32 pins_used(TopologyNode *self, int outputs)
{
    u32 count = 0;
    for (u32 i = 0; i < NODE_PINS; ++i) if (outputs ? self->outputs[i].node != NULL : self->inputs[i].node != NULL) count = i + 1;
    return count;
}
static u64 MSABI node_get_input_count(TopologyNode *self, u32 *count) { if (!count) return E_POINTER_; *count = pins_used(self, 0); return S_OK_; }
static u64 MSABI node_get_output_count(TopologyNode *self, u32 *count) { if (!count) return E_POINTER_; *count = pins_used(self, 1); return S_OK_; }
static u64 MSABI node_connect_output(TopologyNode *self, u32 output, TopologyNode *downstream, u32 input)
{
    if (!downstream) return E_POINTER_;
    if (output >= NODE_PINS || input >= NODE_PINS || VTBL(downstream) != g_node_vtable) return E_INVALIDARG_;
    com_add_ref(downstream);
    pthread_mutex_lock(&self->lock);
    TopologyNode *old = self->outputs[output].node;
    self->outputs[output].node = downstream;
    self->outputs[output].index = input;
    pthread_mutex_unlock(&self->lock);
    if (old) com_release(old);
    pthread_mutex_lock(&downstream->lock);
    downstream->inputs[input].node = self;
    downstream->inputs[input].index = output;
    pthread_mutex_unlock(&downstream->lock);
    return S_OK_;
}
static u64 MSABI node_disconnect_output(TopologyNode *self, u32 output)
{
    if (output >= NODE_PINS) return E_INVALIDARG_;
    pthread_mutex_lock(&self->lock);
    TopologyNode *old = self->outputs[output].node;
    self->outputs[output].node = NULL;
    pthread_mutex_unlock(&self->lock);
    if (!old) return E_INVALIDARG_;
    pthread_mutex_lock(&old->lock);
    for (int i = 0; i < NODE_PINS; ++i) if (old->inputs[i].node == self) old->inputs[i].node = NULL;
    pthread_mutex_unlock(&old->lock);
    com_release(old);
    return S_OK_;
}
static u64 MSABI node_get_input(TopologyNode *self, u32 input, TopologyNode **upstream, u32 *output)
{
    if (!upstream || !output) return E_POINTER_;
    if (input >= NODE_PINS || !self->inputs[input].node) return E_INVALIDARG_;
    *upstream = self->inputs[input].node;
    *output = self->inputs[input].index;
    com_add_ref(*upstream);
    return S_OK_;
}
static u64 MSABI node_get_output(TopologyNode *self, u32 output, TopologyNode **downstream, u32 *input)
{
    if (!downstream || !input) return E_POINTER_;
    if (output >= NODE_PINS || !self->outputs[output].node) return E_INVALIDARG_;
    *downstream = self->outputs[output].node;
    *input = self->outputs[output].index;
    com_add_ref(*downstream);
    return S_OK_;
}
static u64 node_set_pref(void **slot, void *type)
{
    com_add_ref(type);
    void *old = *slot;
    *slot = type;
    com_release(old);
    return S_OK_;
}
static u64 node_get_pref(void *slot, void **type)
{
    if (!type) return E_POINTER_;
    *type = slot;
    com_add_ref(slot);
    return slot ? S_OK_ : E_FAIL_;
}
static u64 MSABI node_set_output_pref(TopologyNode *self, u32 i, void *type) { return i < NODE_PINS ? node_set_pref(&self->output_types[i], type) : E_INVALIDARG_; }
static u64 MSABI node_get_output_pref(TopologyNode *self, u32 i, void **type) { return i < NODE_PINS ? node_get_pref(self->output_types[i], type) : E_INVALIDARG_; }
static u64 MSABI node_set_input_pref(TopologyNode *self, u32 i, void *type) { return i < NODE_PINS ? node_set_pref(&self->input_types[i], type) : E_INVALIDARG_; }
static u64 MSABI node_get_input_pref(TopologyNode *self, u32 i, void **type) { return i < NODE_PINS ? node_get_pref(self->input_types[i], type) : E_INVALIDARG_; }
static u64 MSABI node_clone_from(TopologyNode *self, TopologyNode *other)
{
    if (!other || VTBL(other) != g_node_vtable) return E_INVALIDARG_;
    self->type = other->type;
    self->id = other->id;
    node_set_object(self, other->object);
    typedef u64 MSABI (*CopyFn)(void *, void *);
    ((CopyFn)VTBL(other->base.attributes)[32])(other->base.attributes, self->base.attributes);   /* CopyAllItems */
    for (int i = 0; i < NODE_PINS; ++i) {
        node_set_pref(&self->output_types[i], other->output_types[i]);
        node_set_pref(&self->input_types[i], other->input_types[i]);
    }
    return S_OK_;
}
static TopologyNode *node_new(u32 type)
{
    TopologyNode *node = calloc(1, sizeof(*node));
    if (!node || !attr_object_init(&node->base, g_node_vtable)) { free(node); return NULL; }
    pthread_mutex_init(&node->lock, NULL);
    node->type = type;
    node->id = atomic_fetch_add(&g_next_topology_id, 1);
    return node;
}
uint64_t MSABI beer_mfs_create_topology_node(uint32_t type, void **node)
{
    if (!node) return E_POINTER_;
    if (type > 3) return E_INVALIDARG_;
    *node = node_new(type);
    DIAG("MFCreateTopologyNode(%u) -> %p\n", type, *node);
    return *node ? S_OK_ : E_OUTOFMEMORY_;
}

#define TOPOLOGY_NODES 64
typedef struct Topology {
    AttrObject base;
    pthread_mutex_t lock;
    u64 id;
    TopologyNode *nodes[TOPOLOGY_NODES];       /* held */
    u32 count;
} Topology;
static void *g_topology_vtable[43];

static u64 MSABI topology_query(Topology *self, const void *iid, void **out)
{
    if (!out) return E_POINTER_;
    *out = NULL;
    if (!guid_eq(iid, &IID_IUnknown_) && !guid_eq(iid, &IID_IMFAttributes_) && !guid_eq(iid, &IID_IMFTopology_))
        return E_NOINTERFACE_;
    atomic_fetch_add(&self->base.references, 1);
    *out = self;
    return S_OK_;
}
static u64 MSABI topology_add_ref(Topology *self) { return atomic_fetch_add(&self->base.references, 1) + 1; }
static u64 MSABI topology_clear(Topology *self)
{
    pthread_mutex_lock(&self->lock);
    for (u32 i = 0; i < self->count; ++i) com_release(self->nodes[i]);
    self->count = 0;
    pthread_mutex_unlock(&self->lock);
    return S_OK_;
}
static u64 MSABI topology_release(Topology *self)
{
    const u32 left = atomic_fetch_sub(&self->base.references, 1) - 1;
    if (!left) {
        topology_clear(self);
        com_release(self->base.attributes);
        pthread_mutex_destroy(&self->lock);
        free(self);
    }
    return left;
}
static u64 MSABI topology_get_id(Topology *self, u64 *id) { if (!id) return E_POINTER_; *id = self->id; return S_OK_; }
static u64 MSABI topology_add_node(Topology *self, TopologyNode *node)
{
    if (!node) return E_POINTER_;
    if (VTBL(node) != g_node_vtable) return E_INVALIDARG_;
    pthread_mutex_lock(&self->lock);
    for (u32 i = 0; i < self->count; ++i)
        if (self->nodes[i] == node) { pthread_mutex_unlock(&self->lock); return E_INVALIDARG_; }
    if (self->count == TOPOLOGY_NODES) { pthread_mutex_unlock(&self->lock); return E_OUTOFMEMORY_; }
    self->nodes[self->count++] = node;
    com_add_ref(node);
    pthread_mutex_unlock(&self->lock);
    return S_OK_;
}
static u64 MSABI topology_remove_node(Topology *self, TopologyNode *node)
{
    pthread_mutex_lock(&self->lock);
    for (u32 i = 0; i < self->count; ++i)
        if (self->nodes[i] == node) {
            memmove(self->nodes + i, self->nodes + i + 1, (self->count - i - 1) * sizeof(*self->nodes));
            --self->count;
            pthread_mutex_unlock(&self->lock);
            com_release(node);
            return S_OK_;
        }
    pthread_mutex_unlock(&self->lock);
    return E_INVALIDARG_;
}
static u64 MSABI topology_get_node_count(Topology *self, u16 *count) { if (!count) return E_POINTER_; *count = (u16)self->count; return S_OK_; }
static u64 MSABI topology_get_node(Topology *self, u16 index, TopologyNode **node)
{
    if (!node) return E_POINTER_;
    pthread_mutex_lock(&self->lock);
    *node = index < self->count ? self->nodes[index] : NULL;
    com_add_ref(*node);
    pthread_mutex_unlock(&self->lock);
    return *node ? S_OK_ : MF_E_INVALIDINDEX_;
}
static u64 MSABI topology_clone_from(Topology *self, Topology *other)
{
    if (!other || VTBL(other) != g_topology_vtable || other == self) return E_INVALIDARG_;
    topology_clear(self);
    pthread_mutex_lock(&other->lock);
    for (u32 i = 0; i < other->count; ++i) { self->nodes[i] = other->nodes[i]; com_add_ref(self->nodes[i]); }
    self->count = other->count;
    pthread_mutex_unlock(&other->lock);
    return S_OK_;
}
static u64 MSABI topology_get_node_by_id(Topology *self, u64 id, TopologyNode **node)
{
    if (!node) return E_POINTER_;
    *node = NULL;
    pthread_mutex_lock(&self->lock);
    for (u32 i = 0; i < self->count && !*node; ++i) if (self->nodes[i]->id == id) { *node = self->nodes[i]; com_add_ref(*node); }
    pthread_mutex_unlock(&self->lock);
    return *node ? S_OK_ : MF_E_NOT_FOUND_;
}
static u64 topology_collect(Topology *self, u32 type, void **collection)
{
    if (!collection) return E_POINTER_;
    Collection *nodes = collection_new();
    if (!nodes) return E_OUTOFMEMORY_;
    pthread_mutex_lock(&self->lock);
    for (u32 i = 0; i < self->count; ++i) if (self->nodes[i]->type == type) collection_add(nodes, self->nodes[i]);
    pthread_mutex_unlock(&self->lock);
    *collection = nodes;
    return S_OK_;
}
static u64 MSABI topology_get_sources(Topology *self, void **c) { return topology_collect(self, MF_TOPOLOGY_SOURCESTREAM_NODE_, c); }
static u64 MSABI topology_get_outputs(Topology *self, void **c) { return topology_collect(self, MF_TOPOLOGY_OUTPUT_NODE_, c); }
uint64_t MSABI beer_mfs_create_topology(void **out)
{
    if (!out) return E_POINTER_;
    Topology *topology = calloc(1, sizeof(*topology));
    if (!topology || !attr_object_init(&topology->base, g_topology_vtable)) { free(topology); return E_OUTOFMEMORY_; }
    pthread_mutex_init(&topology->lock, NULL);
    topology->id = atomic_fetch_add(&g_next_topology_id, 1);
    *out = topology;
    DIAG("MFCreateTopology -> %p\n", *out);
    return S_OK_;
}

/* ================================================================================================================
 * Activates: sample grabber sink, audio renderer, video renderer
 * ================================================================================================================ */
enum { ACTIVATE_SAMPLE_GRABBER = 1, ACTIVATE_AUDIO_RENDERER = 2, ACTIVATE_VIDEO_RENDERER = 3 };
typedef struct Activate {
    AttrObject base;
    u32 kind;
    void *media_type;                          /* sample grabber: the type it accepts (held) */
    void *callback;                            /* IMFSampleGrabberSinkCallback (held) */
} Activate;
static void *g_activate_vtable[36];

static u64 MSABI activate_query(Activate *self, const void *iid, void **out)
{
    if (!out) return E_POINTER_;
    *out = NULL;
    if (!guid_eq(iid, &IID_IUnknown_) && !guid_eq(iid, &IID_IMFAttributes_) && !guid_eq(iid, &IID_IMFActivate_)) {
        diag_guid("activate: no interface", iid);
        return E_NOINTERFACE_;
    }
    atomic_fetch_add(&self->base.references, 1);
    *out = self;
    return S_OK_;
}
static u64 MSABI activate_add_ref(Activate *self) { return atomic_fetch_add(&self->base.references, 1) + 1; }
static u64 MSABI activate_release(Activate *self)
{
    const u32 left = atomic_fetch_sub(&self->base.references, 1) - 1;
    if (!left) {
        com_release(self->media_type);
        com_release(self->callback);
        com_release(self->base.attributes);
        free(self);
    }
    return left;
}
/* The session drives the sinks itself; the game does not activate them in a session pipeline. */
static u64 MSABI activate_activate_object(Activate *self, const void *iid, void **out)
{
    (void)self;
    diag_guid("IMFActivate::ActivateObject (not provided)", iid);
    if (out) *out = NULL;
    return E_NOTIMPL_;
}
static u64 MSABI activate_shutdown_object(Activate *self) { (void)self; return S_OK_; }
static u64 MSABI activate_detach_object(Activate *self) { (void)self; return S_OK_; }
static Activate *activate_new(u32 kind)
{
    Activate *activate = calloc(1, sizeof(*activate));
    if (!activate || !attr_object_init(&activate->base, g_activate_vtable)) { free(activate); return NULL; }
    activate->kind = kind;
    return activate;
}
uint64_t MSABI beer_mfs_create_sample_grabber_sink_activate(void *media_type, void *callback, void **out)
{
    if (!out) return E_POINTER_;
    *out = NULL;
    if (!media_type || !callback) return E_POINTER_;
    Activate *activate = activate_new(ACTIVATE_SAMPLE_GRABBER);
    if (!activate) return E_OUTOFMEMORY_;
    activate->media_type = media_type; com_add_ref(media_type);
    activate->callback = callback; com_add_ref(callback);
    *out = activate;
    DIAG("MFCreateSampleGrabberSinkActivate(type %p, callback %p) -> %p\n", media_type, callback, *out);
    return S_OK_;
}
uint64_t MSABI beer_mfs_create_audio_renderer_activate(void **out)
{
    if (!out) return E_POINTER_;
    *out = activate_new(ACTIVATE_AUDIO_RENDERER);
    DIAG("MFCreateAudioRendererActivate -> %p\n", *out);
    return *out ? S_OK_ : E_OUTOFMEMORY_;
}
uint64_t MSABI beer_mfs_create_video_renderer_activate(uint64_t window, void **out)
{
    (void)window;
    if (!out) return E_POINTER_;
    *out = activate_new(ACTIVATE_VIDEO_RENDERER);
    return *out ? S_OK_ : E_OUTOFMEMORY_;
}

/* ================================================================================================================
 * Samples and memory buffers (MFCreateSample, MFCreateMemoryBuffer, MFCreateAlignedMemoryBuffer)
 * ================================================================================================================ */
typedef struct MemoryBuffer {
    void **vtable;
    _Atomic(u32) references;
    u8 *data;
    u32 current, maximum;
} MemoryBuffer;
static void *g_memory_buffer_vtable[8];
static u64 MSABI buffer_query(MemoryBuffer *self, const void *iid, void **out)
{
    if (!out) return E_POINTER_;
    *out = NULL;
    if (!guid_eq(iid, &IID_IUnknown_) && !guid_eq(iid, &IID_IMFMediaBuffer_)) return E_NOINTERFACE_;
    atomic_fetch_add(&self->references, 1);
    *out = self;
    return S_OK_;
}
static u64 MSABI buffer_add_ref(MemoryBuffer *self) { return atomic_fetch_add(&self->references, 1) + 1; }
static u64 MSABI buffer_release(MemoryBuffer *self)
{
    const u32 left = atomic_fetch_sub(&self->references, 1) - 1;
    if (!left) { free(self->data); free(self); }
    return left;
}
static u64 MSABI buffer_lock(MemoryBuffer *self, u8 **data, u32 *maximum, u32 *current)
{
    if (!data) return E_POINTER_;
    *data = self->data;
    if (maximum) *maximum = self->maximum;
    if (current) *current = self->current;
    return S_OK_;
}
static u64 MSABI buffer_unlock(MemoryBuffer *self) { (void)self; return S_OK_; }
static u64 MSABI buffer_get_current(MemoryBuffer *self, u32 *length) { if (!length) return E_POINTER_; *length = self->current; return S_OK_; }
static u64 MSABI buffer_set_current(MemoryBuffer *self, u32 length)
{ if (length > self->maximum) return E_INVALIDARG_; self->current = length; return S_OK_; }
static u64 MSABI buffer_get_max(MemoryBuffer *self, u32 *length) { if (!length) return E_POINTER_; *length = self->maximum; return S_OK_; }
static MemoryBuffer *buffer_new(u32 maximum, u32 alignment)
{
    MemoryBuffer *buffer = calloc(1, sizeof(*buffer));
    if (!buffer) return NULL;
    size_t align = (size_t)alignment + 1 < 16 ? 16 : (size_t)alignment + 1;       /* MF_*_ALIGNMENT values are mask */
    if (align & (align - 1)) align = 16;
    if (posix_memalign((void **)&buffer->data, align, maximum ? maximum : 1)) { free(buffer); return NULL; }
    buffer->vtable = g_memory_buffer_vtable;
    atomic_store(&buffer->references, 1);
    buffer->maximum = maximum;
    return buffer;
}
uint64_t MSABI beer_mfs_create_memory_buffer(uint32_t max_length, void **out)
{
    if (!out) return E_POINTER_;
    *out = buffer_new(max_length, 15);
    return *out ? S_OK_ : E_OUTOFMEMORY_;
}
uint64_t MSABI beer_mfs_create_aligned_memory_buffer(uint32_t max_length, uint32_t alignment, void **out)
{
    if (!out) return E_POINTER_;
    *out = buffer_new(max_length, alignment);
    return *out ? S_OK_ : E_OUTOFMEMORY_;
}

#define SAMPLE_BUFFERS 16
typedef struct Sample {
    AttrObject base;
    pthread_mutex_t lock;
    u32 flags;
    s64 time, duration;
    int has_time, has_duration;
    void *buffers[SAMPLE_BUFFERS];             /* held */
    u32 count;
} Sample;
static void *g_sample_vtable[47];
typedef u64 MSABI (*BufLockFn)(void *, u8 **, u32 *, u32 *);
typedef u64 MSABI (*BufLenFn)(void *, u32 *);

static u64 MSABI sample_query(Sample *self, const void *iid, void **out)
{
    if (!out) return E_POINTER_;
    *out = NULL;
    if (!guid_eq(iid, &IID_IUnknown_) && !guid_eq(iid, &IID_IMFAttributes_) && !guid_eq(iid, &IID_IMFSample_))
        return E_NOINTERFACE_;
    atomic_fetch_add(&self->base.references, 1);
    *out = self;
    return S_OK_;
}
static u64 MSABI sample_add_ref(Sample *self) { return atomic_fetch_add(&self->base.references, 1) + 1; }
static u64 MSABI sample_remove_all(Sample *self)
{
    pthread_mutex_lock(&self->lock);
    for (u32 i = 0; i < self->count; ++i) com_release(self->buffers[i]);
    self->count = 0;
    pthread_mutex_unlock(&self->lock);
    return S_OK_;
}
static u64 MSABI sample_release(Sample *self)
{
    const u32 left = atomic_fetch_sub(&self->base.references, 1) - 1;
    if (!left) {
        sample_remove_all(self);
        com_release(self->base.attributes);
        pthread_mutex_destroy(&self->lock);
        free(self);
    }
    return left;
}
static u64 MSABI sample_get_flags(Sample *self, u32 *flags) { if (!flags) return E_POINTER_; *flags = self->flags; return S_OK_; }
static u64 MSABI sample_set_flags(Sample *self, u32 flags) { self->flags = flags; return S_OK_; }
static u64 MSABI sample_get_time(Sample *self, s64 *time)
{ if (!time) return E_POINTER_; if (!self->has_time) return 0xC00D36F3u; /* MF_E_NO_SAMPLE_TIMESTAMP */ *time = self->time; return S_OK_; }
static u64 MSABI sample_set_time(Sample *self, s64 time) { self->time = time; self->has_time = 1; return S_OK_; }
static u64 MSABI sample_get_duration(Sample *self, s64 *duration)
{ if (!duration) return E_POINTER_; if (!self->has_duration) return 0xC00D36F4u; /* MF_E_NO_SAMPLE_DURATION */ *duration = self->duration; return S_OK_; }
static u64 MSABI sample_set_duration(Sample *self, s64 duration) { self->duration = duration; self->has_duration = 1; return S_OK_; }
static u64 MSABI sample_get_buffer_count(Sample *self, u32 *count) { if (!count) return E_POINTER_; *count = self->count; return S_OK_; }
static u64 MSABI sample_get_buffer(Sample *self, u32 index, void **buffer)
{
    if (!buffer) return E_POINTER_;
    pthread_mutex_lock(&self->lock);
    *buffer = index < self->count ? self->buffers[index] : NULL;
    com_add_ref(*buffer);
    pthread_mutex_unlock(&self->lock);
    return *buffer ? S_OK_ : E_INVALIDARG_;
}
static u64 MSABI sample_get_total_length(Sample *self, u32 *total)
{
    if (!total) return E_POINTER_;
    *total = 0;
    pthread_mutex_lock(&self->lock);
    for (u32 i = 0; i < self->count; ++i) {
        u32 length = 0;
        ((BufLenFn)VTBL(self->buffers[i])[5])(self->buffers[i], &length);        /* GetCurrentLength */
        *total += length;
    }
    pthread_mutex_unlock(&self->lock);
    return S_OK_;
}
/* Copies every buffer's current data into `out` (capacity `size`), returning the bytes copied. */
static u32 sample_gather(Sample *self, u8 *out, u32 size)
{
    u32 offset = 0;
    for (u32 i = 0; i < self->count; ++i) {
        u8 *data = NULL;
        u32 maximum = 0, current = 0;
        void *buffer = self->buffers[i];
        if (((BufLockFn)VTBL(buffer)[3])(buffer, &data, &maximum, &current) != S_OK_) continue;
        const u32 n = current < size - offset ? current : size - offset;
        memcpy(out + offset, data, n);
        offset += n;
        ((Fn1)VTBL(buffer)[4])(buffer);                                          /* Unlock */
    }
    return offset;
}
static u64 MSABI sample_convert_to_contiguous(Sample *self, void **out)
{
    pthread_mutex_lock(&self->lock);
    if (self->count == 1) {
        if (out) { *out = self->buffers[0]; com_add_ref(*out); }
        pthread_mutex_unlock(&self->lock);
        return S_OK_;
    }
    u32 total = 0;
    pthread_mutex_unlock(&self->lock);
    sample_get_total_length(self, &total);
    MemoryBuffer *joined = buffer_new(total, 15);
    if (!joined) return E_OUTOFMEMORY_;
    pthread_mutex_lock(&self->lock);
    joined->current = sample_gather(self, joined->data, total);
    for (u32 i = 0; i < self->count; ++i) com_release(self->buffers[i]);
    self->buffers[0] = joined;
    self->count = 1;
    pthread_mutex_unlock(&self->lock);
    if (out) { *out = joined; com_add_ref(joined); }
    return S_OK_;
}
static u64 MSABI sample_add_buffer(Sample *self, void *buffer)
{
    if (!buffer) return E_POINTER_;
    pthread_mutex_lock(&self->lock);
    if (self->count == SAMPLE_BUFFERS) { pthread_mutex_unlock(&self->lock); return E_OUTOFMEMORY_; }
    self->buffers[self->count++] = buffer;
    com_add_ref(buffer);
    pthread_mutex_unlock(&self->lock);
    return S_OK_;
}
static u64 MSABI sample_remove_buffer(Sample *self, u32 index)
{
    pthread_mutex_lock(&self->lock);
    if (index >= self->count) { pthread_mutex_unlock(&self->lock); return E_INVALIDARG_; }
    void *buffer = self->buffers[index];
    memmove(self->buffers + index, self->buffers + index + 1, (self->count - index - 1) * sizeof(void *));
    --self->count;
    pthread_mutex_unlock(&self->lock);
    com_release(buffer);
    return S_OK_;
}
static u64 MSABI sample_copy_to_buffer(Sample *self, void *destination)
{
    if (!destination) return E_POINTER_;
    u8 *data = NULL;
    u32 maximum = 0, current = 0, total = 0;
    sample_get_total_length(self, &total);
    if (((BufLockFn)VTBL(destination)[3])(destination, &data, &maximum, &current) != S_OK_) return E_FAIL_;
    if (maximum < total) { ((Fn1)VTBL(destination)[4])(destination); return 0x8007007Au; }   /* MF_E_BUFFERTOOSMALL-like */
    pthread_mutex_lock(&self->lock);
    const u32 copied = sample_gather(self, data, maximum);
    pthread_mutex_unlock(&self->lock);
    ((Fn1)VTBL(destination)[4])(destination);
    ((u64 MSABI (*)(void *, u32))VTBL(destination)[6])(destination, copied);          /* SetCurrentLength */
    return S_OK_;
}
uint64_t MSABI beer_mfs_create_sample(void **out)
{
    if (!out) return E_POINTER_;
    Sample *sample = calloc(1, sizeof(*sample));
    if (!sample || !attr_object_init(&sample->base, g_sample_vtable)) { free(sample); return E_OUTOFMEMORY_; }
    pthread_mutex_init(&sample->lock, NULL);
    *out = sample;
    return S_OK_;
}

/* ================================================================================================================
 * Media sources: probing, descriptors, media type handlers
 * ================================================================================================================ */
typedef struct StreamInfo {
    int ff_index;                              /* ffmpeg stream index (-map 0:N) */
    int kind;                                  /* 1 video, 2 audio */
    char codec[32];
    u32 width, height, rate_num, rate_den, sample_rate, channels;
    u64 bitrate;
} StreamInfo;
#define SOURCE_STREAMS 8

/* Runs a program with stdout into a pipe (posix_spawn, as the source reader starts ffmpeg: no copy of the game's
 * address space). Returns the read end, or -1. */
static int spawn_reader(const char *program, char *const argv[], pid_t *pid)
{
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) return -1;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &all);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    const int rc = posix_spawnp(pid, program, &actions, &attr, argv, environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    if (rc != 0) { close(fds[0]); *pid = -1; return -1; }
    return fds[0];
}
static void parse_rational(const char *text, u32 *num, u32 *den)
{
    unsigned long a = 0, b = 1;
    if (sscanf(text, "%lu/%lu", &a, &b) < 1) return;
    if (a && b) { *num = (u32)a; *den = (u32)b; }
}
/* ffprobe: duration (100 ns), size and the audio and video streams of `path`. */
static int probe_file(const char *path, StreamInfo *streams, u32 *count, u64 *duration, u64 *size)
{
    char *argv[] = { "ffprobe", "-v", "error", "-show_entries",
                     "format=duration,size:stream=index,codec_type,codec_name,width,height,avg_frame_rate,"
                     "r_frame_rate,sample_rate,channels,bit_rate", "-of", "default=noprint_wrappers=1",
                     (char *)path, NULL };
    pid_t pid;
    const int fd = spawn_reader("ffprobe", argv, &pid);
    if (fd < 0) { fprintf(stderr, "[MF SESSION] ffprobe could not be started (is ffmpeg installed?)\n"); return 0; }
    FILE *in = fdopen(fd, "r");
    char line[256];
    StreamInfo current;
    int in_stream = 0;
    *count = 0;
    *duration = *size = 0;
    memset(&current, 0, sizeof(current));
#define FLUSH_STREAM() do { if (in_stream && current.kind && *count < SOURCE_STREAMS) streams[(*count)++] = current; } while (0)
    while (in && fgets(line, sizeof(line), in)) {
        line[strcspn(line, "\r\n")] = 0;
        char *value = strchr(line, '=');
        if (!value) continue;
        *value++ = 0;
        if (!strcmp(line, "index")) {
            FLUSH_STREAM();
            memset(&current, 0, sizeof(current));
            current.ff_index = atoi(value);
            in_stream = 1;
        } else if (!strcmp(line, "codec_type")) current.kind = !strcmp(value, "video") ? 1 : !strcmp(value, "audio") ? 2 : 0;
        else if (!strcmp(line, "codec_name")) snprintf(current.codec, sizeof(current.codec), "%s", value);
        else if (!strcmp(line, "width")) current.width = (u32)atoi(value);
        else if (!strcmp(line, "height")) current.height = (u32)atoi(value);
        else if (!strcmp(line, "avg_frame_rate")) parse_rational(value, &current.rate_num, &current.rate_den);
        else if (!strcmp(line, "r_frame_rate") && !current.rate_num) parse_rational(value, &current.rate_num, &current.rate_den);
        else if (!strcmp(line, "sample_rate")) current.sample_rate = (u32)atoi(value);
        else if (!strcmp(line, "channels")) current.channels = (u32)atoi(value);
        else if (!strcmp(line, "bit_rate") && in_stream) current.bitrate = strtoull(value, NULL, 10);
        else if (!strcmp(line, "duration")) { double seconds = atof(value); if (seconds > 0) *duration = (u64)(seconds * 1e7); }
        else if (!strcmp(line, "size")) *size = strtoull(value, NULL, 10);
    }
    FLUSH_STREAM();
#undef FLUSH_STREAM
    if (in) fclose(in); else close(fd);
    waitpid(pid, NULL, 0);
    for (u32 i = 0; i < *count; ++i)
        DIAG("probe %s: stream %d %s %s %ux%u %u/%u fps, %u Hz %u ch\n", path, streams[i].ff_index,
             streams[i].kind == 1 ? "video" : "audio", streams[i].codec, streams[i].width, streams[i].height,
             streams[i].rate_num, streams[i].rate_den, streams[i].sample_rate, streams[i].channels);
    return *count > 0;
}

/* The Media Foundation subtype of a compressed stream (what Windows' MPEG-4 source reports). */
static Guid codec_subtype(const StreamInfo *info)
{
    static const struct { const char *name; u32 fourcc; } map[] = {
        { "h264", 0x34363248 }, { "hevc", 0x43564548 }, { "vp9", 0x30395056 }, { "vp8", 0x30385056 },
        { "av1", 0x31305641 }, { "mpeg4", 0x5634504d }, { "wmv3", 0x33564d57 }, { "mjpeg", 0x47504a4d },
        { "aac", 0x1610 }, { "mp3", 0x55 }, { "flac", 0xf1ac }, { "opus", 0x704f }, { "wmav2", 0x161 },
        { "pcm_s16le", 1 }, { "pcm_f32le", 3 },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(*map); ++i)
        if (!strcmp(info->codec, map[i].name)) { Guid g = FOURCC_GUID(0); g.d1 = map[i].fourcc; return g; }
    if (!strcmp(info->codec, "mpeg2video")) { Guid g = GUID_(0xe06d8026, 0xdb46, 0x11cf, 0xb4, 0xd1, 0x00, 0x80, 0x5f, 0x6c, 0xbb, 0xea); return g; }
    if (!strcmp(info->codec, "ac3")) { Guid g = GUID_(0xe06d802c, 0xdb46, 0x11cf, 0xb4, 0xd1, 0x00, 0x80, 0x5f, 0x6c, 0xbb, 0xea); return g; }
    Guid fallback = FOURCC_GUID(0);
    fallback.d1 = info->kind == 1 ? 0x34363248 : 0x1610;                         /* H.264 / AAC */
    return fallback;
}
static void *native_media_type(const StreamInfo *info)
{
    void *type = NULL;
    if (beer_mf_create_media_type(&type) != S_OK_ || !type) return NULL;
    const Guid subtype = codec_subtype(info);
    set_guid(type, &MF_MT_MAJOR_TYPE_, info->kind == 1 ? &MFMediaType_Video_ : &MFMediaType_Audio_);
    set_guid(type, &MF_MT_SUBTYPE_, &subtype);
    if (info->kind == 1) {
        set_u64(type, &MF_MT_FRAME_SIZE_, (u64)info->width << 32 | info->height);
        if (info->rate_num) set_u64(type, &MF_MT_FRAME_RATE_, (u64)info->rate_num << 32 | info->rate_den);
        set_u64(type, &MF_MT_PIXEL_ASPECT_RATIO_, (u64)1 << 32 | 1);
        set_u32(type, &MF_MT_INTERLACE_MODE_, 2);                                    /* progressive */
        if (info->bitrate) set_u32(type, &MF_MT_AVG_BITRATE_, (u32)info->bitrate);
    } else {
        set_u32(type, &MF_MT_AUDIO_NUM_CHANNELS_, info->channels ? info->channels : 2);
        set_u32(type, &MF_MT_AUDIO_SAMPLES_PER_SECOND_, info->sample_rate ? info->sample_rate : 48000);
        set_u32(type, &MF_MT_AUDIO_BITS_PER_SAMPLE_, 16);
        if (info->bitrate) set_u32(type, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND_, (u32)(info->bitrate / 8));
    }
    return type;
}

typedef struct TypeHandler {
    void **vtable;
    _Atomic(u32) references;
    pthread_mutex_t lock;
    Guid major;
    void *type;                                /* the stream's one media type (held) */
    void *current;                             /* held */
} TypeHandler;
static void *g_type_handler_vtable[9];
static u64 MSABI handler_query(TypeHandler *self, const void *iid, void **out)
{
    if (!out) return E_POINTER_;
    *out = NULL;
    if (!guid_eq(iid, &IID_IUnknown_) && !guid_eq(iid, &IID_IMFMediaTypeHandler_)) return E_NOINTERFACE_;
    atomic_fetch_add(&self->references, 1);
    *out = self;
    return S_OK_;
}
static u64 MSABI handler_add_ref(TypeHandler *self) { return atomic_fetch_add(&self->references, 1) + 1; }
static u64 MSABI handler_release(TypeHandler *self)
{
    const u32 left = atomic_fetch_sub(&self->references, 1) - 1;
    if (!left) { com_release(self->type); com_release(self->current); pthread_mutex_destroy(&self->lock); free(self); }
    return left;
}
static u64 MSABI handler_is_supported(TypeHandler *self, void *type, void **closest)
{
    if (closest) *closest = NULL;
    Guid major;
    if (!type || !attr_guid(type, &MF_MT_MAJOR_TYPE_, &major)) return E_INVALIDARG_;
    return guid_eq(&major, &self->major) ? S_OK_ : 0xC00D36B4u;                      /* MF_E_INVALIDMEDIATYPE */
}
static u64 MSABI handler_get_count(TypeHandler *self, u32 *count) { (void)self; if (!count) return E_POINTER_; *count = 1; return S_OK_; }
static u64 MSABI handler_get_by_index(TypeHandler *self, u32 index, void **type)
{
    if (!type) return E_POINTER_;
    if (index) { *type = NULL; return 0xC00D36B9u; }                                 /* MF_E_NO_MORE_TYPES */
    *type = self->type;
    com_add_ref(*type);
    return S_OK_;
}
static u64 MSABI handler_set_current(TypeHandler *self, void *type)
{
    if (!type) return E_POINTER_;
    com_add_ref(type);
    pthread_mutex_lock(&self->lock);
    void *old = self->current;
    self->current = type;
    pthread_mutex_unlock(&self->lock);
    com_release(old);
    return S_OK_;
}
static u64 MSABI handler_get_current(TypeHandler *self, void **type)
{
    if (!type) return E_POINTER_;
    pthread_mutex_lock(&self->lock);
    *type = self->current;
    com_add_ref(*type);
    pthread_mutex_unlock(&self->lock);
    return S_OK_;
}
static u64 MSABI handler_get_major(TypeHandler *self, Guid *major) { if (!major) return E_POINTER_; *major = self->major; return S_OK_; }

typedef struct StreamDescriptor {
    AttrObject base;
    u32 id;
    TypeHandler *handler;                      /* held */
    StreamInfo info;
    char *path;                                /* the file ffmpeg decodes */
} StreamDescriptor;
static void *g_stream_descriptor_vtable[35];
static u64 MSABI sd_query(StreamDescriptor *self, const void *iid, void **out)
{
    if (!out) return E_POINTER_;
    *out = NULL;
    if (!guid_eq(iid, &IID_IUnknown_) && !guid_eq(iid, &IID_IMFAttributes_) && !guid_eq(iid, &IID_IMFStreamDescriptor_))
        return E_NOINTERFACE_;
    atomic_fetch_add(&self->base.references, 1);
    *out = self;
    return S_OK_;
}
static u64 MSABI sd_add_ref(StreamDescriptor *self) { return atomic_fetch_add(&self->base.references, 1) + 1; }
static u64 MSABI sd_release(StreamDescriptor *self)
{
    const u32 left = atomic_fetch_sub(&self->base.references, 1) - 1;
    if (!left) { com_release(self->handler); com_release(self->base.attributes); free(self->path); free(self); }
    return left;
}
static u64 MSABI sd_get_id(StreamDescriptor *self, u32 *id) { if (!id) return E_POINTER_; *id = self->id; return S_OK_; }
static u64 MSABI sd_get_handler(StreamDescriptor *self, void **handler)
{
    if (!handler) return E_POINTER_;
    *handler = self->handler;
    com_add_ref(*handler);
    return S_OK_;
}
static StreamDescriptor *sd_new(u32 id, const StreamInfo *info, const char *path)
{
    StreamDescriptor *sd = calloc(1, sizeof(*sd));
    TypeHandler *handler = calloc(1, sizeof(*handler));
    if (!sd || !handler || !attr_object_init(&sd->base, g_stream_descriptor_vtable)) { free(sd); free(handler); return NULL; }
    handler->vtable = g_type_handler_vtable;
    atomic_store(&handler->references, 1);
    pthread_mutex_init(&handler->lock, NULL);
    handler->major = info->kind == 1 ? MFMediaType_Video_ : MFMediaType_Audio_;
    handler->type = native_media_type(info);
    handler->current = handler->type;
    com_add_ref(handler->current);
    sd->id = id;
    sd->handler = handler;
    sd->info = *info;
    sd->path = strdup(path);
    return sd;
}

typedef struct PresentationDescriptor {
    AttrObject base;
    pthread_mutex_t lock;
    StreamDescriptor *streams[SOURCE_STREAMS]; /* held */
    int selected[SOURCE_STREAMS];
    u32 count;
} PresentationDescriptor;
static void *g_pd_vtable[38];
static u64 MSABI pd_query(PresentationDescriptor *self, const void *iid, void **out)
{
    if (!out) return E_POINTER_;
    *out = NULL;
    if (!guid_eq(iid, &IID_IUnknown_) && !guid_eq(iid, &IID_IMFAttributes_) && !guid_eq(iid, &IID_IMFPresentationDescriptor_))
        return E_NOINTERFACE_;
    atomic_fetch_add(&self->base.references, 1);
    *out = self;
    return S_OK_;
}
static u64 MSABI pd_add_ref(PresentationDescriptor *self) { return atomic_fetch_add(&self->base.references, 1) + 1; }
static u64 MSABI pd_release(PresentationDescriptor *self)
{
    const u32 left = atomic_fetch_sub(&self->base.references, 1) - 1;
    if (!left) {
        for (u32 i = 0; i < self->count; ++i) com_release(self->streams[i]);
        com_release(self->base.attributes);
        pthread_mutex_destroy(&self->lock);
        free(self);
    }
    return left;
}
static u64 MSABI pd_get_count(PresentationDescriptor *self, u32 *count) { if (!count) return E_POINTER_; *count = self->count; return S_OK_; }
static u64 MSABI pd_get_by_index(PresentationDescriptor *self, u32 index, int32_t *selected, void **sd)
{
    if (!selected || !sd) return E_POINTER_;
    if (index >= self->count) return E_INVALIDARG_;
    pthread_mutex_lock(&self->lock);
    *selected = self->selected[index];
    *sd = self->streams[index];
    com_add_ref(*sd);
    pthread_mutex_unlock(&self->lock);
    return S_OK_;
}
static u64 MSABI pd_select(PresentationDescriptor *self, u32 index)
{ if (index >= self->count) return E_INVALIDARG_; self->selected[index] = 1; return S_OK_; }
static u64 MSABI pd_deselect(PresentationDescriptor *self, u32 index)
{ if (index >= self->count) return E_INVALIDARG_; self->selected[index] = 0; return S_OK_; }
static PresentationDescriptor *pd_new(void)
{
    PresentationDescriptor *pd = calloc(1, sizeof(*pd));
    if (!pd || !attr_object_init(&pd->base, g_pd_vtable)) { free(pd); return NULL; }
    pthread_mutex_init(&pd->lock, NULL);
    return pd;
}
static PresentationDescriptor *pd_clone(PresentationDescriptor *self)
{
    PresentationDescriptor *copy = pd_new();
    if (!copy) return NULL;
    pthread_mutex_lock(&self->lock);
    for (u32 i = 0; i < self->count; ++i) {
        copy->streams[i] = self->streams[i];
        com_add_ref(copy->streams[i]);
        copy->selected[i] = self->selected[i];
    }
    copy->count = self->count;
    pthread_mutex_unlock(&self->lock);
    typedef u64 MSABI (*CopyFn)(void *, void *);
    ((CopyFn)VTBL(self->base.attributes)[32])(self->base.attributes, copy->base.attributes);
    return copy;
}
static u64 MSABI pd_clone_method(PresentationDescriptor *self, void **out)
{
    if (!out) return E_POINTER_;
    *out = pd_clone(self);
    return *out ? S_OK_ : E_OUTOFMEMORY_;
}

/* The rate range Beer's ffmpeg-fed playback offers: forward 0 (scrubbing) to 4x, no reverse. */
typedef struct { void **vtable; void *owner; } SubObject;
static u64 rate_bounds(u32 direction, float *rate, float bound)
{
    if (!rate) return E_POINTER_;
    if (direction) return MF_E_REVERSE_UNSUPPORTED_;
    *rate = bound;
    return S_OK_;
}

typedef struct MediaSource {
    void **vtable;
    _Atomic(u32) references;
    EventQueue *queue;
    PresentationDescriptor *pd;                /* template; CreatePresentationDescriptor hands out clones */
    char *path;
    int temporary;                             /* path is a copy of a byte stream, removed with the source */
    int shutdown;
    SubObject rate_support;
} MediaSource;
static void *g_source_vtable[13];
static void *g_rate_support_vtable[6];
static u64 MSABI source_query(MediaSource *self, const void *iid, void **out)
{
    if (!out) return E_POINTER_;
    *out = NULL;
    if (!guid_eq(iid, &IID_IUnknown_) && !guid_eq(iid, &IID_IMFMediaEventGenerator_) && !guid_eq(iid, &IID_IMFMediaSource_)) {
        diag_guid("media source: no interface", iid);
        return E_NOINTERFACE_;
    }
    atomic_fetch_add(&self->references, 1);
    *out = self;
    return S_OK_;
}
static u64 MSABI source_add_ref(MediaSource *self) { return atomic_fetch_add(&self->references, 1) + 1; }
static u64 MSABI source_release(MediaSource *self)
{
    const u32 left = atomic_fetch_sub(&self->references, 1) - 1;
    if (!left) {
        queue_shutdown(self->queue);
        queue_release(self->queue);
        com_release(self->pd);
        if (self->temporary && self->path) unlink(self->path);
        free(self->path);
        free(self);
    }
    return left;
}
static u64 MSABI source_get_event(MediaSource *self, u32 flags, void **event) { return queue_get_event(self->queue, flags, event); }
static u64 MSABI source_begin_get_event(MediaSource *self, void *callback, void *state) { return queue_begin_get_event(self->queue, callback, state); }
static u64 MSABI source_end_get_event(MediaSource *self, void *result, void **event) { return queue_end_get_event(self->queue, result, event); }
static u64 MSABI source_queue_event(MediaSource *self, u32 type, const Guid *ext, u32 status, const u8 *value)
{ return queue_queue_event_param_var(self->queue, type, ext, status, value); }
static u64 MSABI source_get_characteristics(MediaSource *self, u32 *characteristics)
{
    if (!characteristics) return E_POINTER_;
    if (self->shutdown) return MF_E_SHUTDOWN_;
    *characteristics = 0x2 | 0x4;                                                    /* CAN_PAUSE | CAN_SEEK */
    return S_OK_;
}
static u64 MSABI source_create_pd(MediaSource *self, void **pd)
{
    if (!pd) return E_POINTER_;
    if (self->shutdown) return MF_E_SHUTDOWN_;
    *pd = pd_clone(self->pd);
    return *pd ? S_OK_ : E_OUTOFMEMORY_;
}
/* The session decodes the streams itself; the source only reports the state changes the game may ask for. */
static u64 MSABI source_start(MediaSource *self, void *pd, const Guid *format, const u8 *position)
{
    (void)pd; (void)format;
    queue_queue_event_param_var(self->queue, MESourceStarted_, NULL, S_OK_, position);
    return S_OK_;
}
static u64 MSABI source_stop(MediaSource *self) { queue_queue_event_param_var(self->queue, MESourceStopped_, NULL, S_OK_, NULL); return S_OK_; }
static u64 MSABI source_pause(MediaSource *self) { queue_queue_event_param_var(self->queue, MESourcePaused_, NULL, S_OK_, NULL); return S_OK_; }
static u64 MSABI source_shutdown(MediaSource *self) { self->shutdown = 1; queue_shutdown(self->queue); return S_OK_; }

static MediaSource *source_open(const char *path, int temporary)
{
    StreamInfo streams[SOURCE_STREAMS];
    u32 count = 0;
    u64 duration = 0, size = 0;
    if (!probe_file(path, streams, &count, &duration, &size)) return NULL;
    MediaSource *source = calloc(1, sizeof(*source));
    if (!source) return NULL;
    source->vtable = g_source_vtable;
    atomic_store(&source->references, 1);
    source->queue = queue_new();
    source->pd = pd_new();
    source->path = strdup(path);
    source->temporary = temporary;
    source->rate_support.vtable = g_rate_support_vtable;
    source->rate_support.owner = source;
    if (!source->queue || !source->pd || !source->path) { source_release(source); return NULL; }
    for (u32 i = 0; i < count; ++i) {
        StreamDescriptor *sd = sd_new(i, &streams[i], path);
        if (!sd) continue;
        source->pd->streams[source->pd->count] = sd;
        source->pd->selected[source->pd->count++] = 1;
    }
    if (duration) set_u64(source->pd->base.attributes, &MF_PD_DURATION_, duration);
    if (size) set_u64(source->pd->base.attributes, &MF_PD_TOTAL_FILE_SIZE_, size);
    DIAG("media source %p for %s: %u streams, %.3f s\n", (void *)source, path, source->pd->count, duration / 1e7);
    return source;
}

/* ---- Source resolver -------------------------------------------------------------------------------------------- */
typedef struct { void **vtable; _Atomic(u32) references; } Resolver;
static void *g_resolver_vtable[10];
static u64 MSABI resolver_query(Resolver *self, const void *iid, void **out)
{
    if (!out) return E_POINTER_;
    *out = NULL;
    if (!guid_eq(iid, &IID_IUnknown_) && !guid_eq(iid, &IID_IMFSourceResolver_)) return E_NOINTERFACE_;
    atomic_fetch_add(&self->references, 1);
    *out = self;
    return S_OK_;
}
static u64 MSABI resolver_add_ref(Resolver *self) { return atomic_fetch_add(&self->references, 1) + 1; }
static u64 MSABI resolver_release(Resolver *self)
{
    const u32 left = atomic_fetch_sub(&self->references, 1) - 1;
    if (!left) free(self);
    return left;
}
/* A file: URL or a plain Windows path as a host path. */
static u64 url_to_host_path(const u16 *url, char *host, size_t size)
{
    if (!url) return E_POINTER_;
    char text[4096];
    size_t n = 0;
    for (size_t i = 0; url[i] && n + 4 < sizeof(text); ++i) {                        /* UTF-16 to UTF-8 */
        u32 c = url[i];
        if (c >= 0xd800 && c < 0xdc00 && url[i + 1] >= 0xdc00 && url[i + 1] < 0xe000) c = 0x10000 + ((c - 0xd800) << 10) + (url[++i] - 0xdc00);
        if (c < 0x80) text[n++] = (char)c;
        else if (c < 0x800) { text[n++] = (char)(0xc0 | c >> 6); text[n++] = (char)(0x80 | (c & 0x3f)); }
        else if (c < 0x10000) { text[n++] = (char)(0xe0 | c >> 12); text[n++] = (char)(0x80 | (c >> 6 & 0x3f)); text[n++] = (char)(0x80 | (c & 0x3f)); }
        else { text[n++] = (char)(0xf0 | c >> 18); text[n++] = (char)(0x80 | (c >> 12 & 0x3f)); text[n++] = (char)(0x80 | (c >> 6 & 0x3f)); text[n++] = (char)(0x80 | (c & 0x3f)); }
    }
    text[n] = 0;
    char *path = text;
    if (!strncasecmp(path, "file:", 5)) {
        path += 5;
        while (*path == '/') ++path;                                                 /* file:///C:/x or file://C:/x */
        char *out = path;                                                            /* %XX escapes */
        for (char *in = path; *in; ++in) {
            if (in[0] == '%' && in[1] && in[2]) { char hex[3] = { in[1], in[2], 0 }; *out++ = (char)strtol(hex, NULL, 16); in += 2; }
            else *out++ = *in;
        }
        *out = 0;
    } else if (strstr(path, "://")) {
        DIAG("unsupported URL scheme: %s\n", path);
        return MF_E_UNSUPPORTED_SCHEME_;
    }
    if (g_resolve_path && g_resolve_path(path, host, size)) return S_OK_;
    if (path[0] == '/' && access(path, R_OK) == 0) { snprintf(host, size, "%s", path); return S_OK_; }
    DIAG("cannot resolve %s\n", path);
    return 0x80070002u;                                                              /* HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) */
}
static u64 open_url(const u16 *url, void **source)
{
    char host[4096];
    const u64 hr = url_to_host_path(url, host, sizeof(host));
    if (hr != S_OK_) return hr;
    *source = source_open(host, 0);
    return *source ? S_OK_ : MF_E_UNSUPPORTED_BYTESTREAM_TYPE_;
}
/* IMFByteStream contents into a temporary file ffmpeg can read. */
static u64 open_byte_stream(void *stream, void **source)
{
    if (!stream) return E_POINTER_;
    void **vt = VTBL(stream);
    u64 length = 0, position = 0;
    typedef u64 MSABI (*U64OutFn)(void *, u64 *);
    typedef u64 MSABI (*SetPosFn)(void *, u64);
    typedef u64 MSABI (*ReadFn)(void *, u8 *, u32, u32 *);
    ((U64OutFn)vt[4])(stream, &length);                                              /* GetLength */
    ((U64OutFn)vt[6])(stream, &position);                                            /* GetCurrentPosition */
    char path[] = "/tmp/beer-mf-session-XXXXXX";
    const int fd = mkstemp(path);
    if (fd < 0) return E_FAIL_;
    ((SetPosFn)vt[7])(stream, 0);
    u8 *block = malloc(1 << 20);
    u64 total = 0;
    while (block) {
        u32 got = 0;
        if (((ReadFn)vt[9])(stream, block, 1 << 20, &got) != S_OK_ || !got) break;   /* Read */
        if (write(fd, block, got) != (ssize_t)got) break;
        total += got;
    }
    free(block);
    close(fd);
    ((SetPosFn)vt[7])(stream, position);
    DIAG("byte stream (%llu bytes of %llu) copied to %s\n", (unsigned long long)total, (unsigned long long)length, path);
    *source = source_open(path, 1);
    if (!*source) { unlink(path); return MF_E_UNSUPPORTED_BYTESTREAM_TYPE_; }
    return S_OK_;
}
static u64 MSABI resolver_create_from_url(Resolver *self, const u16 *url, u32 flags, void *props, u32 *type, void **object)
{
    (void)self; (void)flags; (void)props;
    if (!object || !type) return E_POINTER_;
    *object = NULL;
    const u64 hr = open_url(url, object);
    *type = 0;                                                                       /* MF_OBJECT_MEDIASOURCE */
    DIAG("CreateObjectFromURL -> 0x%llx\n", (unsigned long long)hr);
    return hr;
}
static u64 MSABI resolver_create_from_stream(Resolver *self, void *stream, const u16 *url, u32 flags, void *props,
                                            u32 *type, void **object)
{
    (void)self; (void)url; (void)flags; (void)props;
    if (!object || !type) return E_POINTER_;
    *object = NULL;
    const u64 hr = open_byte_stream(stream, object);
    *type = 0;
    DIAG("CreateObjectFromByteStream -> 0x%llx\n", (unsigned long long)hr);
    return hr;
}
/* Asynchronous forms: the source is opened now; the callback learns about it on a work-queue thread. */
static u64 begin_create(u64 hr, void *object, void **cancel, void *callback, void *state)
{
    if (cancel) *cancel = NULL;
    AsyncResult *result = async_new(object, callback, state);
    com_release(object);
    if (!result) return E_OUTOFMEMORY_;
    atomic_store(&result->status, (u32)hr);
    return work_submit(result);
}
static u64 MSABI resolver_begin_from_url(Resolver *self, const u16 *url, u32 flags, void *props, void **cancel,
                                        void *callback, void *state)
{
    (void)self; (void)flags; (void)props;
    if (!callback) return E_POINTER_;
    void *object = NULL;
    const u64 hr = open_url(url, &object);
    DIAG("BeginCreateObjectFromURL -> 0x%llx\n", (unsigned long long)hr);
    return begin_create(hr, object, cancel, callback, state);
}
static u64 MSABI resolver_begin_from_stream(Resolver *self, void *stream, const u16 *url, u32 flags, void *props,
                                           void **cancel, void *callback, void *state)
{
    (void)self; (void)url; (void)flags; (void)props;
    if (!callback) return E_POINTER_;
    void *object = NULL;
    const u64 hr = open_byte_stream(stream, &object);
    DIAG("BeginCreateObjectFromByteStream -> 0x%llx\n", (unsigned long long)hr);
    return begin_create(hr, object, cancel, callback, state);
}
static u64 MSABI resolver_end(Resolver *self, AsyncResult *result, u32 *type, void **object)
{
    (void)self;
    if (!result || !type || !object) return E_POINTER_;
    *object = NULL;
    if (VTBL(result) != g_async_result_vtable) return E_INVALIDARG_;
    const u32 status = atomic_load(&result->status);
    if (status) return status;
    *type = 0;
    *object = result->object;
    com_add_ref(*object);
    return *object ? S_OK_ : E_FAIL_;
}
static u64 MSABI resolver_cancel(Resolver *self, void *cookie) { (void)self; (void)cookie; return S_OK_; }
uint64_t MSABI beer_mfs_create_source_resolver(void **out);

/* ================================================================================================================
 * The media session
 * ================================================================================================================ */
#define SESSION_STREAMS 8
typedef struct SessionStream {
    StreamInfo info;
    char *path;
    int kind;                                  /* 1 video, 2 audio */
    void *callback;                            /* IMFSampleGrabberSinkCallback (held); NULL for a renderer */
    void *type;                                /* the sink's media type (held) */
    Guid major;
    const char *format;                        /* ffmpeg pixel / sample format */
    u32 width, height, frame_bytes;            /* video */
    u32 channels, rate, bits;                  /* audio */
    s64 frame_duration;
    int ignore_clock;
    s64 time_offset;
    pid_t pid;                                 /* the running decoder, killed to interrupt it */
    int eos;
} SessionStream;

enum { STATE_STOPPED, STATE_RUNNING, STATE_PAUSED, STATE_CLOSED };
enum { CMD_SET_TOPOLOGY, CMD_CLEAR_TOPOLOGIES, CMD_START, CMD_PAUSE, CMD_STOP, CMD_CLOSE, CMD_RATE, CMD_END };
typedef struct Command { int type; s64 position; int has_position; float rate; void *topology; u32 generation; } Command;

typedef struct Session {
    void **vtable;
    _Atomic(u32) references;
    EventQueue *queue;
    SubObject clock, rate_control, rate_support, get_service;
    pthread_mutex_t lock;
    pthread_cond_t cond;                       /* state changes, commands */
    Topology *topology;                        /* held */
    SessionStream streams[SESSION_STREAMS];
    u32 count;
    int state, shutdown, started_once, thread_running;
    s64 position, anchor;                      /* clock: position (100 ns) at system time anchor */
    float rate;
    u32 generation;                            /* bumped to stop the decoders of the previous start */
    u32 decoders;                              /* decoder threads still running */
    Command commands[64];
    u32 command_head, command_count;
} Session;
static void *g_session_vtable[17];
static void *g_clock_vtable[16];
static void *g_rate_control_vtable[5];
static void *g_get_service_vtable[4];

static s64 clock_time_locked(Session *s)
{
    if (s->state != STATE_RUNNING) return s->position;
    return s->position + (s64)((now_100ns() - s->anchor) * (double)s->rate);
}

/* IMFClockStateSink / IMFSampleGrabberSinkCallback on every sample grabber of the topology. */
typedef u64 MSABI (*SinkTimeFn)(void *, s64);
typedef u64 MSABI (*SinkStartFn)(void *, s64, s64);
typedef u64 MSABI (*SinkRateFn)(void *, s64, float);
typedef u64 MSABI (*SinkClockFn)(void *, void *);
typedef u64 MSABI (*SinkSampleFn)(void *, const Guid *, u32, s64, s64, const u8 *, u32);
enum { SINK_START = 3, SINK_STOP = 4, SINK_PAUSE = 5, SINK_RESTART = 6, SINK_RATE = 7, SINK_SET_CLOCK = 8, SINK_SHUTDOWN = 10 };
static void notify_sinks(Session *s, int slot, s64 a, s64 b, float rate)
{
    void *callbacks[SESSION_STREAMS];
    u32 n = 0;
    pthread_mutex_lock(&s->lock);
    for (u32 i = 0; i < s->count; ++i) if (s->streams[i].callback) { callbacks[n] = s->streams[i].callback; com_add_ref(callbacks[n++]); }
    pthread_mutex_unlock(&s->lock);
    for (u32 i = 0; i < n; ++i) {
        void *cb = callbacks[i];
        switch (slot) {
        case SINK_START: ((SinkStartFn)VTBL(cb)[3])(cb, a, b); break;
        case SINK_STOP: case SINK_PAUSE: case SINK_RESTART: ((SinkTimeFn)VTBL(cb)[slot])(cb, a); break;
        case SINK_RATE: ((SinkRateFn)VTBL(cb)[7])(cb, a, rate); break;
        case SINK_SET_CLOCK: ((SinkClockFn)VTBL(cb)[8])(cb, &s->clock); break;
        case SINK_SHUTDOWN: ((Fn1)VTBL(cb)[10])(cb); break;
        }
        com_release(cb);
    }
}

static void session_add_ref_internal(Session *s) { atomic_fetch_add(&s->references, 1); }
static u64 MSABI session_release(Session *s);

/* ---- Decoding --------------------------------------------------------------------------------------------------- */
typedef struct DecodeJob { Session *session; u32 stream, generation; s64 start; } DecodeJob;
static int read_full(int fd, u8 *buffer, u32 size)
{
    u32 done = 0;
    while (done < size) {
        const ssize_t n = read(fd, buffer + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        done += (u32)n;
    }
    return 1;
}
static void *decode_thread(void *opaque)
{
    DecodeJob job = *(DecodeJob *)opaque;
    free(opaque);
    Session *s = job.session;
    pthread_mutex_lock(&s->lock);
    SessionStream *st = &s->streams[job.stream];
    char start[32], index[16], size[64], channels[16], rate[16];
    snprintf(start, sizeof(start), "%.4f", job.start / 1e7);
    snprintf(index, sizeof(index), "0:%d", st->info.ff_index);
    snprintf(size, sizeof(size), "scale=%u:%u:threads=2", st->width, st->height);   /* conversion threads: 2 */
    snprintf(channels, sizeof(channels), "%u", st->channels);
    snprintf(rate, sizeof(rate), "%u", st->rate);
    char *video_argv[] = { "ffmpeg", "-nostdin", "-v", "error", "-threads", (char *)mf_decode_threads(), "-filter_threads", "2", "-ss", start,
                           "-i", st->path, "-map", index, "-an", "-sn",
                           "-vsync", "passthrough", "-pix_fmt", (char *)st->format, "-vf", size, "-f", "rawvideo", "pipe:1", NULL };
    char *audio_argv[] = { "ffmpeg", "-nostdin", "-v", "error", "-threads", (char *)mf_decode_threads(), "-filter_threads", "2", "-ss", start,
                           "-i", st->path, "-map", index, "-vn", "-sn",
                           "-ac", channels, "-ar", rate, "-f", (char *)st->format, "pipe:1", NULL };
    const int video = st->kind == 1;
    const u32 bytes = video ? st->frame_bytes : 1024 * st->channels * (st->bits / 8);
    const s64 duration = video ? st->frame_duration : (s64)1024 * 10000000 / (st->rate ? st->rate : 48000);
    void *callback = st->callback;
    com_add_ref(callback);
    const Guid major = st->major;
    const int ignore_clock = st->ignore_clock;
    const s64 offset = st->time_offset;
    pid_t pid = -1;
    const int fd = job.generation == s->generation ? spawn_reader("ffmpeg", video ? video_argv : audio_argv, &pid) : -1;
    if (job.generation == s->generation) st->pid = pid;
    pthread_mutex_unlock(&s->lock);
    DIAG("decoder for stream %u (%s %s) from %s s -> pid %d\n", job.stream, video ? "video" : "audio", st->format, start, (int)pid);

    u8 *buffer = bytes ? malloc(bytes) : NULL;
    const s64 lead = video ? 200000 : 2000000;           /* deliver up to 20 ms (video) / 200 ms (audio) early */
    s64 time = job.start;
    int finished = 0;
    while (fd >= 0 && buffer) {
        if (!read_full(fd, buffer, bytes)) { finished = 1; break; }
        pthread_mutex_lock(&s->lock);
        for (;;) {
            if (s->generation != job.generation || s->shutdown || s->state == STATE_STOPPED || s->state == STATE_CLOSED) break;
            if (s->state == STATE_RUNNING && (ignore_clock || s->rate <= 0 ? ignore_clock : clock_time_locked(s) >= time - lead)) break;
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_nsec += 5 * 1000 * 1000;
            if (until.tv_nsec >= 1000000000) { until.tv_nsec -= 1000000000; ++until.tv_sec; }
            pthread_cond_timedwait(&s->cond, &s->lock, &until);
        }
        const int live = s->generation == job.generation && !s->shutdown && s->state == STATE_RUNNING;
        pthread_mutex_unlock(&s->lock);
        if (!live) break;
        if (callback) ((SinkSampleFn)VTBL(callback)[9])(callback, &major, 0, time + offset, duration, buffer, bytes);
        time += duration;
    }
    free(buffer);
    if (fd >= 0) close(fd);
    if (pid > 0) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); }
    com_release(callback);
    pthread_mutex_lock(&s->lock);
    if (s->generation == job.generation) {
        st->pid = -1;
        if (finished) {
            st->eos = 1;
            int all = 1;
            for (u32 i = 0; i < s->count; ++i) if (!s->streams[i].eos) all = 0;
            if (all && s->command_count < 64) {                                     /* end of the presentation */
                Command *c = &s->commands[(s->command_head + s->command_count++) % 64];
                memset(c, 0, sizeof(*c));
                c->type = CMD_END;
                c->generation = job.generation;
                pthread_cond_broadcast(&s->cond);
            }
        }
    }
    --s->decoders;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
    DIAG("decoder for stream %u done (%s)\n", job.stream, finished ? "end of stream" : "stopped");
    session_release(s);
    return NULL;
}
/* With the lock held: ends the decoders of the current start. */
static void stop_decoders_locked(Session *s)
{
    ++s->generation;
    for (u32 i = 0; i < s->count; ++i) if (s->streams[i].pid > 0) { kill(s->streams[i].pid, SIGKILL); s->streams[i].pid = -1; }
    pthread_cond_broadcast(&s->cond);
}
static void start_decoders_locked(Session *s, s64 position)
{
    stop_decoders_locked(s);
    for (u32 i = 0; i < s->count; ++i) {
        s->streams[i].eos = 0;
        DecodeJob *job = malloc(sizeof(*job));
        if (!job) continue;
        *job = (DecodeJob){ s, i, s->generation, position };
        session_add_ref_internal(s);
        ++s->decoders;
        if (!beer_mf_start_worker(decode_thread, job)) { free(job); --s->decoders; atomic_fetch_sub(&s->references, 1); }
    }
}

/* ---- Topology resolution ---------------------------------------------------------------------------------------- */
static void release_streams_locked(Session *s)
{
    for (u32 i = 0; i < s->count; ++i) {
        com_release(s->streams[i].callback);
        com_release(s->streams[i].type);
        free(s->streams[i].path);
    }
    memset(s->streams, 0, sizeof(s->streams));
    s->count = 0;
}
/* The ffmpeg output format for a sink's media type, completing the attributes the game left unset. */
static int configure_stream(SessionStream *st, void *type)
{
    Guid subtype = FOURCC_GUID(0);
    const int have_subtype = type && attr_guid(type, &MF_MT_SUBTYPE_, &subtype);
    if (st->kind == 1) {
        u64 frame_size = 0;
        if (type && attr_u64(type, &MF_MT_FRAME_SIZE_, &frame_size) && frame_size) {
            st->width = (u32)(frame_size >> 32);
            st->height = (u32)frame_size;
        } else {
            st->width = st->info.width;
            st->height = st->info.height;
            if (type) set_u64(type, &MF_MT_FRAME_SIZE_, (u64)st->width << 32 | st->height);
        }
        const u32 w = st->width, h = st->height, cw = (w + 1) / 2, ch = (h + 1) / 2;
        u32 stride = w;
        switch (have_subtype ? subtype.d1 : 0x3231564e) {
        case 0x16: st->format = "bgr0"; st->frame_bytes = w * h * 4; stride = w * 4; break;     /* RGB32 */
        case 0x15: st->format = "bgra"; st->frame_bytes = w * h * 4; stride = w * 4; break;     /* ARGB32 */
        case 0x32595559: st->format = "yuyv422"; st->frame_bytes = w * h * 2; stride = w * 2; break;   /* YUY2 */
        case 0x59565955: st->format = "uyvy422"; st->frame_bytes = w * h * 2; stride = w * 2; break;   /* UYVY */
        case 0x30323449: case 0x56555949: st->format = "yuv420p"; st->frame_bytes = w * h + 2 * cw * ch; break;   /* I420 / IYUV */
        default: {
            st->format = "nv12";
            st->frame_bytes = w * h + 2 * cw * ch;
            if (!have_subtype && type) { Guid nv12 = FOURCC_GUID(0x3231564e); set_guid(type, &MF_MT_SUBTYPE_, &nv12); }
        }
        }
        u32 existing = 0;
        if (type && !attr_u32(type, &MF_MT_DEFAULT_STRIDE_, &existing)) set_u32(type, &MF_MT_DEFAULT_STRIDE_, stride);
        u32 num = st->info.rate_num ? st->info.rate_num : 30, den = st->info.rate_num ? st->info.rate_den : 1;
        u64 rate = 0;
        if (type && !attr_u64(type, &MF_MT_FRAME_RATE_, &rate)) set_u64(type, &MF_MT_FRAME_RATE_, (u64)num << 32 | den);
        st->frame_duration = (s64)((10000000ull * den + num / 2) / num);
        if (type) set_u32(type, &MF_MT_ALL_SAMPLES_INDEPENDENT_, 1);
        return st->width && st->height;
    }
    u32 value = 0;
    st->channels = type && attr_u32(type, &MF_MT_AUDIO_NUM_CHANNELS_, &value) && value ? value : st->info.channels ? st->info.channels : 2;
    st->rate = type && attr_u32(type, &MF_MT_AUDIO_SAMPLES_PER_SECOND_, &value) && value ? value : st->info.sample_rate ? st->info.sample_rate : 48000;
    const int is_float = have_subtype && subtype.d1 == 3;
    st->bits = type && attr_u32(type, &MF_MT_AUDIO_BITS_PER_SAMPLE_, &value) && value ? value : is_float ? 32 : 16;
    if (is_float) { st->format = "f32le"; st->bits = 32; }
    else st->format = st->bits == 8 ? "u8" : st->bits == 24 ? "s24le" : st->bits == 32 ? "s32le" : (st->bits = 16, "s16le");
    if (type) {
        if (!have_subtype) { Guid pcm = FOURCC_GUID(1); set_guid(type, &MF_MT_SUBTYPE_, &pcm); }
        set_u32(type, &MF_MT_AUDIO_NUM_CHANNELS_, st->channels);
        set_u32(type, &MF_MT_AUDIO_SAMPLES_PER_SECOND_, st->rate);
        set_u32(type, &MF_MT_AUDIO_BITS_PER_SAMPLE_, st->bits);
        set_u32(type, &MF_MT_AUDIO_BLOCK_ALIGNMENT_, st->channels * st->bits / 8);
        set_u32(type, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND_, st->rate * st->channels * st->bits / 8);
    }
    return 1;
}
static int resolve_topology_locked(Session *s, Topology *topology)
{
    release_streams_locked(s);
    pthread_mutex_lock(&topology->lock);
    for (u32 i = 0; i < topology->count && s->count < SESSION_STREAMS; ++i) {
        TopologyNode *node = topology->nodes[i];
        if (node->type != MF_TOPOLOGY_SOURCESTREAM_NODE_) continue;
        StreamDescriptor *sd = attr_unknown(node->base.attributes, &MF_TOPONODE_STREAM_DESCRIPTOR_);
        if (!sd || VTBL(sd) != g_stream_descriptor_vtable) {
            DIAG("source node %llu has no Beer stream descriptor\n", (unsigned long long)node->id);
            com_release(sd);
            continue;
        }
        TopologyNode *out = node->outputs[0].node;
        for (int hops = 0; out && out->type != MF_TOPOLOGY_OUTPUT_NODE_ && hops < 8; ++hops) out = out->outputs[0].node;
        Activate *activate = out ? out->object : NULL;
        if (!activate || VTBL(activate) != g_activate_vtable) {
            DIAG("stream %u: output node without a Beer sink activate (object %p); not played\n", sd->id, out ? out->object : NULL);
            com_release(sd);
            continue;
        }
        SessionStream *st = &s->streams[s->count];
        memset(st, 0, sizeof(*st));
        st->info = sd->info;
        st->path = strdup(sd->path);
        st->kind = sd->info.kind;
        st->major = st->kind == 1 ? MFMediaType_Video_ : MFMediaType_Audio_;
        st->pid = -1;
        if (activate->kind == ACTIVATE_SAMPLE_GRABBER) {
            st->callback = activate->callback; com_add_ref(st->callback);
            st->type = activate->media_type; com_add_ref(st->type);
            u32 ignore = 0;
            u64 offset = 0;
            st->ignore_clock = attr_u32(activate->base.attributes, &MF_SAMPLEGRABBERSINK_IGNORE_CLOCK_, &ignore) && ignore;
            if (attr_u64(activate->base.attributes, &MF_SAMPLEGRABBERSINK_SAMPLE_TIME_OFFSET_, &offset)) st->time_offset = (s64)offset;
        } else if (activate->kind == ACTIVATE_AUDIO_RENDERER) {
            DIAG("stream %u goes to the audio renderer: decoded for timing only (no output device yet)\n", sd->id);
        }
        if (!configure_stream(st, st->type)) { com_release(st->callback); com_release(st->type); free(st->path); com_release(sd); continue; }
        DIAG("stream %u: %s %s -> %s %ux%u %u Hz %u ch%s\n", sd->id, st->kind == 1 ? "video" : "audio", sd->info.codec,
             st->format, st->width, st->height, st->rate, st->channels, st->callback ? "" : " (renderer)");
        ++s->count;
        com_release(sd);
    }
    pthread_mutex_unlock(&topology->lock);
    return s->count > 0;
}

/* ---- Commands --------------------------------------------------------------------------------------------------- */
static u64 push_command(Session *s, const Command *command)
{
    pthread_mutex_lock(&s->lock);
    if (s->shutdown) { pthread_mutex_unlock(&s->lock); return MF_E_SHUTDOWN_; }
    if (s->command_count == 64) { pthread_mutex_unlock(&s->lock); return E_OUTOFMEMORY_; }
    s->commands[(s->command_head + s->command_count++) % 64] = *command;
    if (command->topology) com_add_ref(command->topology);
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
    return S_OK_;
}
static void post_unknown(Session *s, u32 type, u32 status, void *unknown, const Guid *key, u32 value)
{
    u8 variant[24] = {0};
    if (unknown) { *(u16 *)variant = VT_UNKNOWN_; *(void **)(variant + 8) = unknown; }
    queue_post(s->queue, type, status, variant, key, value, NULL, 0);
}
static void run_command(Session *s, Command *c)
{
    const s64 system = now_100ns();
    switch (c->type) {
    case CMD_SET_TOPOLOGY: {
        Topology *topology = c->topology;
        pthread_mutex_lock(&s->lock);
        stop_decoders_locked(s);
        const int ok = topology && VTBL(topology) == g_topology_vtable && resolve_topology_locked(s, topology);
        if (ok) { com_add_ref(topology); com_release(s->topology); s->topology = topology; }
        s->state = STATE_STOPPED;
        s->position = 0;
        pthread_mutex_unlock(&s->lock);
        if (!ok) {
            DIAG("SetTopology: nothing playable\n");
            post_unknown(s, MESessionTopologySet_, 0xC00D5212u, topology, NULL, 0);         /* MF_E_TOPO_UNSUPPORTED */
            break;
        }
        post_unknown(s, MESessionTopologySet_, S_OK_, topology, NULL, 0);
        notify_sinks(s, SINK_SET_CLOCK, 0, 0, 0);
        queue_post(s->queue, MESessionCapabilitiesChanged_, S_OK_, NULL, &MF_EVENT_SESSIONCAPS_, SESSION_CAPS,
                   &MF_EVENT_SESSIONCAPS_DELTA_, SESSION_CAPS);
        post_unknown(s, MESessionTopologyStatus_, S_OK_, topology, &MF_EVENT_TOPOLOGY_STATUS_, MF_TOPOSTATUS_READY_);
        break;
    }
    case CMD_CLEAR_TOPOLOGIES:
        pthread_mutex_lock(&s->lock);
        stop_decoders_locked(s);
        release_streams_locked(s);
        com_release(s->topology);
        s->topology = NULL;
        s->state = STATE_STOPPED;
        pthread_mutex_unlock(&s->lock);
        queue_post(s->queue, MESessionTopologiesCleared_, S_OK_, NULL, NULL, 0, NULL, 0);
        break;
    case CMD_START: {
        pthread_mutex_lock(&s->lock);
        if (!s->count) { pthread_mutex_unlock(&s->lock); queue_post(s->queue, MESessionStarted_, MF_E_INVALIDREQUEST_, NULL, NULL, 0, NULL, 0); break; }
        const int resume = s->state == STATE_PAUSED && !c->has_position;
        const int restart = !resume;
        if (restart) {
            s->position = c->has_position ? c->position : s->state == STATE_RUNNING ? clock_time_locked(s) : 0;
            start_decoders_locked(s, s->position);
        }
        s->anchor = system;
        s->state = STATE_RUNNING;
        const s64 position = s->position;
        const int first = !s->started_once;
        s->started_once = 1;
        Topology *topology = s->topology;
        com_add_ref(topology);
        pthread_cond_broadcast(&s->cond);
        pthread_mutex_unlock(&s->lock);
        if (resume) notify_sinks(s, SINK_RESTART, system, 0, 0);
        else notify_sinks(s, SINK_START, system, position, 0);
        if (first) post_unknown(s, MESessionTopologyStatus_, S_OK_, topology, &MF_EVENT_TOPOLOGY_STATUS_, MF_TOPOSTATUS_STARTED_SOURCE_);
        com_release(topology);
        queue_post(s->queue, MESessionStarted_, S_OK_, NULL, NULL, 0, NULL, 0);
        DIAG("started at %.3f s%s\n", position / 1e7, resume ? " (resumed)" : "");
        break;
    }
    case CMD_PAUSE:
        pthread_mutex_lock(&s->lock);
        if (s->state == STATE_RUNNING) { s->position = clock_time_locked(s); s->state = STATE_PAUSED; }
        pthread_cond_broadcast(&s->cond);
        pthread_mutex_unlock(&s->lock);
        notify_sinks(s, SINK_PAUSE, system, 0, 0);
        queue_post(s->queue, MESessionPaused_, S_OK_, NULL, NULL, 0, NULL, 0);
        break;
    case CMD_STOP:
    case CMD_CLOSE:
        pthread_mutex_lock(&s->lock);
        stop_decoders_locked(s);
        s->state = c->type == CMD_CLOSE ? STATE_CLOSED : STATE_STOPPED;
        s->position = 0;
        pthread_mutex_unlock(&s->lock);
        notify_sinks(s, SINK_STOP, system, 0, 0);
        queue_post(s->queue, c->type == CMD_CLOSE ? MESessionClosed_ : MESessionStopped_, S_OK_, NULL, NULL, 0, NULL, 0);
        break;
    case CMD_RATE: {
        pthread_mutex_lock(&s->lock);
        if (s->state == STATE_RUNNING) { s->position = clock_time_locked(s); s->anchor = system; }
        s->rate = c->rate;
        pthread_cond_broadcast(&s->cond);
        pthread_mutex_unlock(&s->lock);
        notify_sinks(s, SINK_RATE, system, 0, c->rate);
        u8 variant[24] = {0};
        *(u16 *)variant = VT_R4_;
        memcpy(variant + 8, &c->rate, 4);
        queue_post(s->queue, MESessionRateChanged_, S_OK_, variant, NULL, 0, NULL, 0);
        break;
    }
    case CMD_END: {
        pthread_mutex_lock(&s->lock);
        const int current = c->generation == s->generation && s->state == STATE_RUNNING;
        Topology *topology = s->topology;
        if (current) { s->position = clock_time_locked(s); s->state = STATE_STOPPED; com_add_ref(topology); }
        pthread_mutex_unlock(&s->lock);
        if (!current) break;
        notify_sinks(s, SINK_STOP, system, 0, 0);
        post_unknown(s, MESessionTopologyStatus_, S_OK_, topology, &MF_EVENT_TOPOLOGY_STATUS_, MF_TOPOSTATUS_ENDED_);
        com_release(topology);
        queue_post(s->queue, MEEndOfPresentation_, S_OK_, NULL, NULL, 0, NULL, 0);
        queue_post(s->queue, MESessionEnded_, S_OK_, NULL, NULL, 0, NULL, 0);
        DIAG("presentation ended\n");
        break;
    }
    }
    com_release(c->topology);
}
static void *command_thread(void *opaque)
{
    Session *s = opaque;
    pthread_mutex_lock(&s->lock);
    for (;;) {
        while (!s->command_count && !s->shutdown) pthread_cond_wait(&s->cond, &s->lock);
        if (s->shutdown) break;
        Command c = s->commands[s->command_head];
        s->command_head = (s->command_head + 1) % 64;
        --s->command_count;
        pthread_mutex_unlock(&s->lock);
        run_command(s, &c);
        pthread_mutex_lock(&s->lock);
    }
    while (s->command_count) {                                                       /* drop what is left */
        com_release(s->commands[s->command_head].topology);
        s->command_head = (s->command_head + 1) % 64;
        --s->command_count;
    }
    s->thread_running = 0;
    pthread_mutex_unlock(&s->lock);
    session_release(s);
    return NULL;
}

/* ---- IMFMediaSession -------------------------------------------------------------------------------------------- */
static u64 MSABI session_query(Session *s, const void *iid, void **out)
{
    if (!out) return E_POINTER_;
    *out = NULL;
    if (guid_eq(iid, &IID_IUnknown_) || guid_eq(iid, &IID_IMFMediaEventGenerator_) || guid_eq(iid, &IID_IMFMediaSession_))
        *out = s;
    else if (guid_eq(iid, &IID_IMFGetService_)) *out = &s->get_service;
    else { diag_guid("media session: no interface", iid); return E_NOINTERFACE_; }
    atomic_fetch_add(&s->references, 1);
    return S_OK_;
}
static u64 MSABI session_add_ref(Session *s) { return atomic_fetch_add(&s->references, 1) + 1; }
static u64 MSABI session_release(Session *s)
{
    const u32 left = atomic_fetch_sub(&s->references, 1) - 1;
    if (!left) {
        queue_shutdown(s->queue);
        queue_release(s->queue);
        release_streams_locked(s);
        com_release(s->topology);
        pthread_mutex_destroy(&s->lock);
        pthread_cond_destroy(&s->cond);
        free(s);
    }
    return left;
}
static u64 MSABI session_get_event(Session *s, u32 flags, void **event) { return queue_get_event(s->queue, flags, event); }
static u64 MSABI session_begin_get_event(Session *s, void *callback, void *state) { return queue_begin_get_event(s->queue, callback, state); }
static u64 MSABI session_end_get_event(Session *s, void *result, void **event) { return queue_end_get_event(s->queue, result, event); }
static u64 MSABI session_queue_event(Session *s, u32 type, const Guid *ext, u32 status, const u8 *value)
{ return queue_queue_event_param_var(s->queue, type, ext, status, value); }
static u64 MSABI session_set_topology(Session *s, u32 flags, void *topology)
{
    DIAG("SetTopology(flags 0x%x, %p)\n", flags, topology);
    if (!topology) return E_POINTER_;
    Command c = { .type = CMD_SET_TOPOLOGY, .topology = topology };
    return push_command(s, &c);
}
static u64 MSABI session_clear_topologies(Session *s) { Command c = { .type = CMD_CLEAR_TOPOLOGIES }; return push_command(s, &c); }
static u64 MSABI session_start(Session *s, const Guid *format, const u8 *position)
{
    Command c = { .type = CMD_START };
    if (format && !guid_eq(format, &(Guid){0})) return 0xC00D36D6u;                 /* MF_E_UNSUPPORTED_TIME_FORMAT */
    if (position && (*(const u16 *)position == VT_I8_ || *(const u16 *)position == VT_UI8_)) {
        c.has_position = 1;
        memcpy(&c.position, position + 8, 8);
        if (c.position < 0) c.position = 0;
    }
    DIAG("Start(%s%.3f s)\n", c.has_position ? "" : "resume/", c.position / 1e7);
    return push_command(s, &c);
}
static u64 MSABI session_pause(Session *s) { DIAG("Pause\n"); Command c = { .type = CMD_PAUSE }; return push_command(s, &c); }
static u64 MSABI session_stop(Session *s) { DIAG("Stop\n"); Command c = { .type = CMD_STOP }; return push_command(s, &c); }
static u64 MSABI session_close(Session *s) { DIAG("Close\n"); Command c = { .type = CMD_CLOSE }; return push_command(s, &c); }
static u64 MSABI session_shutdown(Session *s)
{
    DIAG("Shutdown\n");
    pthread_mutex_lock(&s->lock);
    if (s->shutdown) { pthread_mutex_unlock(&s->lock); return MF_E_SHUTDOWN_; }
    stop_decoders_locked(s);
    s->shutdown = 1;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
    notify_sinks(s, SINK_SHUTDOWN, 0, 0, 0);
    queue_shutdown(s->queue);
    return S_OK_;
}
static u64 MSABI session_get_clock(Session *s, void **clock)
{
    if (!clock) return E_POINTER_;
    *clock = &s->clock;
    session_add_ref_internal(s);
    return S_OK_;
}
static u64 MSABI session_get_caps(Session *s, u32 *caps) { (void)s; if (!caps) return E_POINTER_; *caps = SESSION_CAPS; return S_OK_; }
static u64 MSABI session_get_full_topology(Session *s, u32 flags, u64 id, void **topology)
{
    (void)flags; (void)id;
    if (!topology) return E_POINTER_;
    pthread_mutex_lock(&s->lock);
    *topology = s->topology;
    com_add_ref(*topology);
    pthread_mutex_unlock(&s->lock);
    return *topology ? S_OK_ : MF_E_INVALIDREQUEST_;
}

/* ---- Sub-objects: clock, rate control, rate support, IMFGetService --------------------------------------------- */
#define OWNER(sub) ((Session *)((SubObject *)(sub))->owner)
static u64 MSABI sub_add_ref(SubObject *sub) { return com_add_ref(sub->owner), 2; }
static u64 MSABI sub_release(SubObject *sub) { com_release(sub->owner); return 1; }

static u64 MSABI clock_query(SubObject *sub, const void *iid, void **out)
{
    if (!out) return E_POINTER_;
    *out = NULL;
    if (!guid_eq(iid, &IID_IUnknown_) && !guid_eq(iid, &IID_IMFClock_) && !guid_eq(iid, &IID_IMFPresentationClock_))
        return E_NOINTERFACE_;
    com_add_ref(sub->owner);
    *out = sub;
    return S_OK_;
}
static u64 MSABI clock_characteristics(SubObject *sub, u32 *c) { (void)sub; if (!c) return E_POINTER_; *c = 0x2; return S_OK_; }   /* 10 MHz */
static u64 MSABI clock_correlated(SubObject *sub, u32 reserved, s64 *clock_time, s64 *system)
{
    (void)reserved;
    if (!clock_time || !system) return E_POINTER_;
    Session *s = OWNER(sub);
    pthread_mutex_lock(&s->lock);
    *system = now_100ns();
    *clock_time = clock_time_locked(s);
    pthread_mutex_unlock(&s->lock);
    return S_OK_;
}
static u64 MSABI clock_continuity(SubObject *sub, u32 *key) { (void)sub; if (!key) return E_POINTER_; *key = 0; return S_OK_; }
static u64 MSABI clock_state(SubObject *sub, u32 reserved, u32 *state)
{
    (void)reserved;
    if (!state) return E_POINTER_;
    const int st = OWNER(sub)->state;
    *state = st == STATE_RUNNING ? 1 : st == STATE_PAUSED ? 3 : 2;                  /* MFCLOCK_STATE */
    return S_OK_;
}
static u64 MSABI clock_properties(SubObject *sub, void *properties) { (void)sub; (void)properties; return E_NOTIMPL_; }
static u64 MSABI clock_set_time_source(SubObject *sub, void *source) { (void)sub; (void)source; return S_OK_; }
static u64 MSABI clock_get_time_source(SubObject *sub, void **source) { (void)sub; if (source) *source = NULL; return MF_E_CLOCK_NO_TIME_SOURCE_; }
static u64 MSABI clock_get_time(SubObject *sub, s64 *time)
{
    if (!time) return E_POINTER_;
    Session *s = OWNER(sub);
    pthread_mutex_lock(&s->lock);
    *time = clock_time_locked(s);
    pthread_mutex_unlock(&s->lock);
    return S_OK_;
}
static u64 MSABI clock_sink(SubObject *sub, void *sink) { (void)sub; (void)sink; return S_OK_; }
static u64 MSABI clock_control(SubObject *sub, ...) { (void)sub; return MF_E_INVALIDREQUEST_; }  /* the session runs it */

static u64 MSABI rate_control_query(SubObject *sub, const void *iid, void **out)
{
    if (!out) return E_POINTER_;
    *out = NULL;
    if (!guid_eq(iid, &IID_IUnknown_) && !guid_eq(iid, &IID_IMFRateControl_)) return E_NOINTERFACE_;
    com_add_ref(sub->owner);
    *out = sub;
    return S_OK_;
}
static u64 MSABI rate_set(SubObject *sub, int32_t thin, float rate)
{
    (void)thin;
    if (rate < 0 || rate > 4) return MF_E_UNSUPPORTED_RATE_;
    DIAG("SetRate(%.2f)\n", rate);
    Command c = { .type = CMD_RATE, .rate = rate };
    return push_command(OWNER(sub), &c);
}
static u64 MSABI rate_get(SubObject *sub, int32_t *thin, float *rate)
{
    if (thin) *thin = 0;
    if (!rate) return E_POINTER_;
    *rate = OWNER(sub)->rate;
    return S_OK_;
}
static u64 MSABI rate_support_query(SubObject *sub, const void *iid, void **out)
{
    if (!out) return E_POINTER_;
    *out = NULL;
    if (!guid_eq(iid, &IID_IUnknown_) && !guid_eq(iid, &IID_IMFRateSupport_)) return E_NOINTERFACE_;
    com_add_ref(sub->owner);
    *out = sub;
    return S_OK_;
}
static u64 MSABI rate_slowest(SubObject *sub, u32 direction, int32_t thin, float *rate) { (void)sub; (void)thin; return rate_bounds(direction, rate, 0.0f); }
static u64 MSABI rate_fastest(SubObject *sub, u32 direction, int32_t thin, float *rate) { (void)sub; (void)thin; return rate_bounds(direction, rate, 4.0f); }
static u64 MSABI rate_is_supported(SubObject *sub, int32_t thin, float rate, float *nearest)
{
    (void)sub; (void)thin;
    const float clamped = rate < 0 ? 0 : rate > 4 ? 4 : rate;
    if (nearest) *nearest = clamped;
    return clamped == rate ? S_OK_ : MF_E_UNSUPPORTED_RATE_;
}
static u64 MSABI get_service_query(SubObject *sub, const void *iid, void **out) { return session_query(OWNER(sub), iid, out); }
static u64 MSABI get_service_get(SubObject *sub, const Guid *service, const Guid *iid, void **out)
{ return beer_mfs_get_service(sub->owner, service, iid, out); }

uint64_t MSABI beer_mfs_get_service(void *object, const void *service, const void *iid, void **out)
{
    if (!object || !out) return E_POINTER_;
    *out = NULL;
    if (VTBL(object) == g_session_vtable) {
        Session *s = object;
        if (guid_eq(service, &MF_RATE_CONTROL_SERVICE_)) {
            if (guid_eq(iid, &IID_IMFRateControl_)) { *out = &s->rate_control; session_add_ref_internal(s); return S_OK_; }
            if (guid_eq(iid, &IID_IMFRateSupport_)) { *out = &s->rate_support; session_add_ref_internal(s); return S_OK_; }
            return E_NOINTERFACE_;
        }
        if (guid_eq(service, &MR_POLICY_VOLUME_SERVICE_) && guid_eq(iid, &IID_IMFSimpleAudioVolume_))
            DIAG("audio volume service requested (not provided)\n");
        diag_guid("MFGetService(session): unsupported service", service);
        return MF_E_UNSUPPORTED_SERVICE_;
    }
    if (VTBL(object) == g_source_vtable) {
        MediaSource *source = object;
        if (guid_eq(service, &MF_RATE_CONTROL_SERVICE_) && guid_eq(iid, &IID_IMFRateSupport_)) {
            *out = &source->rate_support;
            com_add_ref(source);
            return S_OK_;
        }
        diag_guid("MFGetService(source): unsupported service", service);
        return MF_E_UNSUPPORTED_SERVICE_;
    }
    /* Anything else that implements IMFGetService answers for itself. */
    void *getter = NULL;
    if (com_query(object, &IID_IMFGetService_, &getter) != S_OK_ || !getter) return MF_E_UNSUPPORTED_SERVICE_;
    const u64 hr = ((u64 MSABI (*)(void *, const void *, const void *, void **))VTBL(getter)[3])(getter, service, iid, out);
    com_release(getter);
    return hr;
}

/* ================================================================================================================
 * Vtables and exported constructors
 * ================================================================================================================ */
static pthread_once_t g_vtables_once = PTHREAD_ONCE_INIT;
static void init_vtables(void)
{
    void **v;
    v = g_async_result_vtable;
    v[0] = async_query; v[1] = async_add_ref; v[2] = async_release; v[3] = async_get_state; v[4] = async_get_status;
    v[5] = async_set_status; v[6] = async_get_object; v[7] = async_get_state_no_add_ref;

    v = g_media_event_vtable;
    v[0] = event_query; v[1] = event_add_ref; v[2] = event_release; fill_attribute_slots(v);
    v[33] = event_get_type; v[34] = event_get_extended_type; v[35] = event_get_status; v[36] = event_get_value;

    v = g_event_queue_vtable;
    v[0] = queue_query; v[1] = queue_add_ref; v[2] = queue_release; v[3] = queue_get_event; v[4] = queue_begin_get_event;
    v[5] = queue_end_get_event; v[6] = queue_queue_event; v[7] = queue_queue_event_param_var;
    v[8] = queue_queue_event_param_unk; v[9] = queue_shutdown;

    v = g_collection_vtable;
    v[0] = collection_query; v[1] = collection_add_ref; v[2] = collection_release; v[3] = collection_get_count;
    v[4] = collection_get_element; v[5] = collection_add; v[6] = collection_remove; v[7] = collection_insert_at;
    v[8] = collection_remove_all;

    v = g_node_vtable;
    v[0] = node_query; v[1] = node_add_ref; v[2] = node_release; fill_attribute_slots(v);
    v[33] = node_set_object; v[34] = node_get_object; v[35] = node_get_type; v[36] = node_get_id; v[37] = node_set_id;
    v[38] = node_get_input_count; v[39] = node_get_output_count; v[40] = node_connect_output;
    v[41] = node_disconnect_output; v[42] = node_get_input; v[43] = node_get_output; v[44] = node_set_output_pref;
    v[45] = node_get_output_pref; v[46] = node_set_input_pref; v[47] = node_get_input_pref; v[48] = node_clone_from;

    v = g_topology_vtable;
    v[0] = topology_query; v[1] = topology_add_ref; v[2] = topology_release; fill_attribute_slots(v);
    v[33] = topology_get_id; v[34] = topology_add_node; v[35] = topology_remove_node; v[36] = topology_get_node_count;
    v[37] = topology_get_node; v[38] = topology_clear; v[39] = topology_clone_from; v[40] = topology_get_node_by_id;
    v[41] = topology_get_sources; v[42] = topology_get_outputs;

    v = g_activate_vtable;
    v[0] = activate_query; v[1] = activate_add_ref; v[2] = activate_release; fill_attribute_slots(v);
    v[33] = activate_activate_object; v[34] = activate_shutdown_object; v[35] = activate_detach_object;

    v = g_memory_buffer_vtable;
    v[0] = buffer_query; v[1] = buffer_add_ref; v[2] = buffer_release; v[3] = buffer_lock; v[4] = buffer_unlock;
    v[5] = buffer_get_current; v[6] = buffer_set_current; v[7] = buffer_get_max;

    v = g_sample_vtable;
    v[0] = sample_query; v[1] = sample_add_ref; v[2] = sample_release; fill_attribute_slots(v);
    v[33] = sample_get_flags; v[34] = sample_set_flags; v[35] = sample_get_time; v[36] = sample_set_time;
    v[37] = sample_get_duration; v[38] = sample_set_duration; v[39] = sample_get_buffer_count; v[40] = sample_get_buffer;
    v[41] = sample_convert_to_contiguous; v[42] = sample_add_buffer; v[43] = sample_remove_buffer;
    v[44] = sample_remove_all; v[45] = sample_get_total_length; v[46] = sample_copy_to_buffer;

    v = g_type_handler_vtable;
    v[0] = handler_query; v[1] = handler_add_ref; v[2] = handler_release; v[3] = handler_is_supported;
    v[4] = handler_get_count; v[5] = handler_get_by_index; v[6] = handler_set_current; v[7] = handler_get_current;
    v[8] = handler_get_major;

    v = g_stream_descriptor_vtable;
    v[0] = sd_query; v[1] = sd_add_ref; v[2] = sd_release; fill_attribute_slots(v); v[33] = sd_get_id; v[34] = sd_get_handler;

    v = g_pd_vtable;
    v[0] = pd_query; v[1] = pd_add_ref; v[2] = pd_release; fill_attribute_slots(v); v[33] = pd_get_count;
    v[34] = pd_get_by_index; v[35] = pd_select; v[36] = pd_deselect; v[37] = pd_clone_method;

    v = g_source_vtable;
    v[0] = source_query; v[1] = source_add_ref; v[2] = source_release; v[3] = source_get_event;
    v[4] = source_begin_get_event; v[5] = source_end_get_event; v[6] = source_queue_event;
    v[7] = source_get_characteristics; v[8] = source_create_pd; v[9] = source_start; v[10] = source_stop;
    v[11] = source_pause; v[12] = source_shutdown;

    v = g_resolver_vtable;
    v[0] = resolver_query; v[1] = resolver_add_ref; v[2] = resolver_release; v[3] = resolver_create_from_url;
    v[4] = resolver_create_from_stream; v[5] = resolver_begin_from_url; v[6] = resolver_end;
    v[7] = resolver_begin_from_stream; v[8] = resolver_end; v[9] = resolver_cancel;

    v = g_session_vtable;
    v[0] = session_query; v[1] = session_add_ref; v[2] = session_release; v[3] = session_get_event;
    v[4] = session_begin_get_event; v[5] = session_end_get_event; v[6] = session_queue_event;
    v[7] = session_set_topology; v[8] = session_clear_topologies; v[9] = session_start; v[10] = session_pause;
    v[11] = session_stop; v[12] = session_close; v[13] = session_shutdown; v[14] = session_get_clock;
    v[15] = session_get_caps; v[16] = session_get_full_topology;

    v = g_clock_vtable;
    v[0] = clock_query; v[1] = sub_add_ref; v[2] = sub_release; v[3] = clock_characteristics; v[4] = clock_correlated;
    v[5] = clock_continuity; v[6] = clock_state; v[7] = clock_properties; v[8] = clock_set_time_source;
    v[9] = clock_get_time_source; v[10] = clock_get_time; v[11] = clock_sink; v[12] = clock_sink;
    v[13] = clock_control; v[14] = clock_control; v[15] = clock_control;

    v = g_rate_control_vtable;
    v[0] = rate_control_query; v[1] = sub_add_ref; v[2] = sub_release; v[3] = rate_set; v[4] = rate_get;

    v = g_rate_support_vtable;
    v[0] = rate_support_query; v[1] = sub_add_ref; v[2] = sub_release; v[3] = rate_slowest; v[4] = rate_fastest;
    v[5] = rate_is_supported;

    v = g_get_service_vtable;
    v[0] = get_service_query; v[1] = sub_add_ref; v[2] = sub_release; v[3] = get_service_get;
}
static void ensure_vtables(void) { pthread_once(&g_vtables_once, init_vtables); }

uint64_t MSABI beer_mfs_create_source_resolver(void **out)
{
    ensure_vtables();
    if (!out) return E_POINTER_;
    Resolver *resolver = calloc(1, sizeof(*resolver));
    if (!resolver) return E_OUTOFMEMORY_;
    resolver->vtable = g_resolver_vtable;
    atomic_store(&resolver->references, 1);
    *out = resolver;
    return S_OK_;
}
uint64_t MSABI beer_mfs_create_media_session(void *configuration, void **out)
{
    (void)configuration;
    ensure_vtables();
    if (!out) return E_POINTER_;
    *out = NULL;
    Session *s = calloc(1, sizeof(*s));
    if (!s) return E_OUTOFMEMORY_;
    s->vtable = g_session_vtable;
    atomic_store(&s->references, 1);
    s->queue = queue_new();
    s->clock = (SubObject){ g_clock_vtable, s };
    s->rate_control = (SubObject){ g_rate_control_vtable, s };
    s->rate_support = (SubObject){ g_rate_support_vtable, s };
    s->get_service = (SubObject){ g_get_service_vtable, s };
    s->rate = 1.0f;
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->cond, NULL);
    if (!s->queue) { free(s); return E_OUTOFMEMORY_; }
    session_add_ref_internal(s);                                                     /* the command thread's */
    s->thread_running = 1;
    if (!beer_mf_start_worker(command_thread, s)) {
        fprintf(stderr, "[MF SESSION] cannot start the session thread\n");
        s->thread_running = 0;
        atomic_fetch_sub(&s->references, 1);
        session_release(s);
        return E_FAIL_;
    }
    *out = s;
    DIAG("MFCreateMediaSession -> %p\n", *out);
    return S_OK_;
}
uint64_t MSABI beer_mfs_enum_device_sources(void *attributes, void ***sources, uint32_t *count)
{
    (void)attributes;
    if (!sources || !count) return E_POINTER_;
    *sources = NULL;                                                                 /* no cameras or microphones */
    *count = 0;
    return S_OK_;
}

/* Every constructor that may come first sets the vtables up. */
__attribute__((constructor)) static void media_session_init(void) { ensure_vtables(); }
