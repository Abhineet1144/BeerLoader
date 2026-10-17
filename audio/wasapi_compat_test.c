#include "wasapi_compat.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define S_OK UINT64_C(0)

typedef uint64_t __attribute__((ms_abi)) (*ReleaseFn)(void *);
typedef uint64_t __attribute__((ms_abi)) (*EnumEndpointsFn)(void *, uint32_t, uint32_t, void **);
typedef uint64_t __attribute__((ms_abi)) (*GetDefaultFn)(void *, uint32_t, uint32_t, void **);
typedef uint64_t __attribute__((ms_abi)) (*GetCountFn)(void *, uint32_t *);
typedef uint64_t __attribute__((ms_abi)) (*ItemFn)(void *, uint32_t, void **);
typedef uint64_t __attribute__((ms_abi)) (*ActivateFn)(void *, const void *, uint32_t, void *, void **);
typedef uint64_t __attribute__((ms_abi)) (*InitializeFn)(void *, uint32_t, uint32_t, int64_t, int64_t, const void *, const void *);
typedef uint64_t __attribute__((ms_abi)) (*GetServiceFn)(void *, const void *, void **);
typedef uint64_t __attribute__((ms_abi)) (*StartFn)(void *);
typedef uint64_t __attribute__((ms_abi)) (*SetEventFn)(void *, uint64_t);
typedef uint64_t __attribute__((ms_abi)) (*GetPaddingFn)(void *, uint32_t *);
typedef uint64_t __attribute__((ms_abi)) (*GetBufferFn)(void *, uint32_t, uint8_t **);
typedef uint64_t __attribute__((ms_abi)) (*ReleaseBufferFn)(void *, uint32_t, uint32_t);
typedef uint64_t __attribute__((ms_abi)) (*ClockPositionFn)(void *, uint64_t *, uint64_t *);

typedef struct WaveFormat {
    uint16_t tag, channels;
    uint32_t rate, average_bytes;
    uint16_t block_align, bits, extra;
} __attribute__((packed)) WaveFormat;

static uint32_t signal_count;
static uint64_t signaled_event;

static void signal_event(uint64_t event)
{
    signal_count++;
    signaled_event = event;
}

int main(void)
{
    if (beer_wasapi_float_bits_to_s16(UINT32_C(0x00000000)) != 0 ||
        beer_wasapi_float_bits_to_s16(UINT32_C(0x3f000000)) != 16384 ||
        beer_wasapi_float_bits_to_s16(UINT32_C(0xbf000000)) != -16384 ||
        beer_wasapi_float_bits_to_s16(UINT32_C(0x3f800000)) != INT16_MAX ||
        beer_wasapi_float_bits_to_s16(UINT32_C(0xbf800000)) != INT16_MIN ||
        beer_wasapi_float_bits_to_s16(UINT32_C(0x7fc00000)) != 0) return 18;

    static const uint8_t clsid[16] = {0x95,0x03,0xde,0xbc,0x2f,0xe5,0x7c,0x46,0x8e,0x3d,0xc4,0x57,0x92,0x91,0x69,0x2e};
    static const uint8_t iid_enum[16] = {0xd2,0x64,0x56,0xa9,0x14,0x96,0x35,0x4f,0xa7,0x46,0xde,0x8d,0xb6,0x36,0x17,0xe6};
    static const uint8_t iid_client[16] = {0x4c,0xad,0xb9,0x1c,0xfa,0xdb,0x32,0x4c,0xb1,0x78,0xc2,0xf5,0x68,0xa7,0x03,0xb2};
    static const uint8_t iid_render[16] = {0xfc,0xac,0x94,0xf2,0x46,0x31,0x83,0x44,0xa7,0xbf,0xad,0xdc,0xa7,0xc2,0x60,0xe2};
    static const uint8_t iid_clock[16] = {0x4f,0x31,0x63,0xcd,0xba,0x3f,0x1b,0x4a,0x81,0x2c,0xef,0x96,0x35,0x87,0x28,0xe7};
    void *enumerator = NULL, *collection = NULL, *device = NULL;
    void *client = NULL, *render = NULL, *clock = NULL;
    if (beer_wasapi_create_instance(clsid, iid_enum, &enumerator) != S_OK || !enumerator) return 1;
    void **v = *(void ***)enumerator;
    if (((EnumEndpointsFn)v[3])(enumerator, 0, 1, &collection) != S_OK || !collection) return 2;
    uint32_t count = 0;
    v = *(void ***)collection;
    if (((GetCountFn)v[3])(collection, &count) != S_OK || count != 1) return 3;
    if (((ItemFn)v[4])(collection, 0, &device) != S_OK || !device) return 4;
    ((ReleaseFn)(*(void ***)device)[2])(device);
    device = NULL;
    v = *(void ***)enumerator;
    if (((GetDefaultFn)v[4])(enumerator, 0, 0, &device) != S_OK || !device) return 5;
    v = *(void ***)device;
    if (((ActivateFn)v[3])(device, iid_client, 1, NULL, &client) != S_OK || !client) return 6;
    WaveFormat format = {1, 2, 48000, 192000, 4, 16, 0};
    v = *(void ***)client;
    if (((InitializeFn)v[3])(client, 0, 0, 1000000, 0, &format, NULL) != S_OK) return 7;
    beer_wasapi_set_event_signaler(signal_event);
    if (((SetEventFn)v[13])(client, 0x1234) != S_OK) return 8;
    if (((StartFn)v[10])(client) != S_OK || signal_count < 1 ||
        signaled_event != 0x1234) return 9;
    if (((GetServiceFn)v[14])(client, iid_render, &render) != S_OK || !render) return 10;
    if (((GetServiceFn)v[14])(client, iid_clock, &clock) != S_OK || !clock) return 11;
    uint8_t *data = NULL;
    v = *(void ***)render;
    if (((GetBufferFn)v[3])(render, 2400, &data) != S_OK || !data) return 12;
    /* Engine mixers may stage float stereo in an oversized PCM16 render packet.
     * The compatibility layer detects and converts that representation without
     * selecting behavior by executable name. */
    uint32_t *staged = (uint32_t *)data;
    for (size_t sample = 0; sample < 2400 * 2; ++sample)
        staged[sample] = sample & 1 ? UINT32_C(0xbf000000) : UINT32_C(0x3f000000);

    if (((ReleaseBufferFn)v[4])(render, 2400, 0) != S_OK) return 13;
    uint32_t padding = 0;
    v = *(void ***)client;
    if (((GetPaddingFn)v[6])(client, &padding) != S_OK || !padding ||
        padding > 2400) return 14;
    struct timespec wait = {0, 80000000};
    nanosleep(&wait, NULL);
    if (signal_count < 2 || signaled_event != 0x1234) return 15;
    if (((GetPaddingFn)v[6])(client, &padding) != S_OK || padding != 0) return 16;
    uint64_t position = UINT64_MAX, qpc = 0;
    v = *(void ***)clock;
    if (((ClockPositionFn)v[4])(clock, &position, &qpc) != S_OK ||
        position < 2400 || !qpc) return 17;
    uint64_t first_qpc = qpc;
    struct timespec qpc_wait = {0, 20000000};
    nanosleep(&qpc_wait, NULL);
    if (((ClockPositionFn)v[4])(clock, &position, &qpc) != S_OK ||
        qpc <= first_qpc || qpc - first_qpc < UINT64_C(5000000) ||
        qpc - first_qpc > UINT64_C(200000000)) return 19;
    ((ReleaseFn)(*(void ***)clock)[2])(clock);
    ((ReleaseFn)(*(void ***)render)[2])(render);
    ((ReleaseFn)(*(void ***)client)[2])(client);
    ((ReleaseFn)(*(void ***)device)[2])(device);
    ((ReleaseFn)(*(void ***)collection)[2])(collection);
    ((ReleaseFn)(*(void ***)enumerator)[2])(enumerator);
    beer_wasapi_shutdown();
    return 0;
}
