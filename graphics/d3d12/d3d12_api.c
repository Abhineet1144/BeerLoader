/* D3D12 entry points that remain after the host-snapshot compatibility device
 * was removed: D3D12CreateDevice (always the GPU-resident BD12 core),
 * D3D12GetDebugInterface, D3D12SerializeVersionedRootSignature (with its
 * ID3DBlob) and the guest event signaler BD12 uses for fence events. */
#include "d3d12_api.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BEER_S_OK          UINT64_C(0)
#define BEER_E_NOINTERFACE UINT64_C(0x80004002)
#define BEER_E_INVALIDARG  UINT64_C(0x80070057)
#define BEER_E_OUTOFMEMORY UINT64_C(0x8007000e)

static const uint8_t k_iid_iunknown[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46
};
static const uint8_t k_iid_d3d_blob[16] = {
    0x08, 0xfb, 0xa5, 0x8b, 0x95, 0x51, 0xe2, 0x40,
    0xac, 0x58, 0x0d, 0x98, 0x9c, 0x3a, 0x01, 0x02
};

static int iid_equal(const uint8_t *left, const uint8_t *right)
{
    return left && memcmp(left, right, 16) == 0;
}

typedef struct BeerD3DBlob {
    void **vtable;
    uint32_t references;
    size_t size;
    uint8_t data[];
} BeerD3DBlob;

static uint32_t __attribute__((ms_abi)) blob_add_ref(BeerD3DBlob *self);

static uint64_t __attribute__((ms_abi))
blob_query_interface(BeerD3DBlob *self, const uint8_t *riid, void **output)
{
    if (!output) return BEER_E_INVALIDARG;
    *output = NULL;
    if (!self || (!iid_equal(riid, k_iid_iunknown) &&
                  !iid_equal(riid, k_iid_d3d_blob)))
        return BEER_E_NOINTERFACE;
    blob_add_ref(self);
    *output = self;
    return BEER_S_OK;
}

static uint32_t __attribute__((ms_abi)) blob_add_ref(BeerD3DBlob *self)
{
    return self ? __atomic_add_fetch(&self->references, 1, __ATOMIC_RELAXED) : 0;
}

static uint32_t __attribute__((ms_abi)) blob_release(BeerD3DBlob *self)
{
    if (!self) return 0;
    uint32_t remaining = __atomic_sub_fetch(&self->references, 1,
                                             __ATOMIC_ACQ_REL);
    if (remaining == 0) free(self);
    return remaining;
}

static void *__attribute__((ms_abi)) blob_get_buffer_pointer(BeerD3DBlob *self)
{
    return self ? self->data : NULL;
}

static size_t __attribute__((ms_abi)) blob_get_buffer_size(BeerD3DBlob *self)
{
    return self ? self->size : 0;
}


static void *g_blob_vtable[5] = {
    [0] = blob_query_interface,
    [1] = blob_add_ref,
    [2] = blob_release,
    [3] = blob_get_buffer_pointer,
    [4] = blob_get_buffer_size,
};

static BeerD3D12SignalEventFn g_signal_event;

void beer_d3d12_set_event_signaler(BeerD3D12SignalEventFn signaler)
{
    g_signal_event = signaler;
}

BeerD3D12SignalEventFn beer_d3d12_get_event_signaler(void)
{
    return g_signal_event;
}

/* GPU-resident D3D12 core (graphics/d3d12v). */
extern uint64_t __attribute__((ms_abi))
beer_bd12_create_device(const uint8_t *riid, void **device);

uint64_t __attribute__((ms_abi))
beer_d3d12_create_device(uint64_t adapter, uint32_t minimum_feature_level,
                         const uint8_t *riid, void **device)
{
    (void)adapter;
    (void)minimum_feature_level;
    if (!device) return BEER_E_INVALIDARG;
    *device = NULL;
    return beer_bd12_create_device(riid, device);
}

uint64_t __attribute__((ms_abi))
beer_d3d12_get_debug_interface(const uint8_t *riid, void **debug)
{
    (void)riid;
    if (debug) *debug = NULL;
    return BEER_E_NOINTERFACE;
}

typedef struct BeerRootSignatureBytes {
    uint8_t *data;
    size_t size;
    size_t capacity;
} BeerRootSignatureBytes;

static int root_bytes_append(BeerRootSignatureBytes *bytes,
                             const void *source, size_t size)
{
    if (!bytes || (!source && size)) return 0;
    if (size > SIZE_MAX - bytes->size) return 0;
    size_t required = bytes->size + size;
    if (required > bytes->capacity) {
        size_t capacity = bytes->capacity ? bytes->capacity : 256;
        while (capacity < required) {
            if (capacity > SIZE_MAX / 2) {
                capacity = required;
                break;
            }
            capacity *= 2;
        }
        uint8_t *data = realloc(bytes->data, capacity);
        if (!data) return 0;
        bytes->data = data;
        bytes->capacity = capacity;
    }
    if (size) memcpy(bytes->data + bytes->size, source, size);
    bytes->size = required;
    return 1;
}

static int root_bytes_u32(BeerRootSignatureBytes *bytes, uint32_t value)
{
    return root_bytes_append(bytes, &value, sizeof(value));
}

uint64_t __attribute__((ms_abi))
beer_d3d12_serialize_versioned_root_signature(const void *description,
                                              void **blob,
                                              void **error_blob)
{
    if (blob) *blob = NULL;
    if (error_blob) *error_blob = NULL;
    if (!description || !blob) return BEER_E_INVALIDARG;

    /* D3D12_VERSIONED_ROOT_SIGNATURE_DESC is {Version, padding, union Desc} on
     * Win64.  RE8 reuses one descriptor builder for thousands of signatures,
     * so copying only the top-level pointer values collapses distinct layouts
     * into one cache key.  Beer does not pass this private blob to a native
     * D3D12 runtime; serialize the complete graph into a deterministic owned
     * representation consumed by BeerD3D12RootSignature/PipelineState. */
    const uint8_t *versioned = description;
    uint32_t version;
    uint32_t parameter_count;
    const uint8_t *parameters;
    uint32_t sampler_count;
    const uint8_t *samplers;
    uint32_t flags;
    memcpy(&version, versioned, sizeof(version));
    memcpy(&parameter_count, versioned + 8, sizeof(parameter_count));
    memcpy(&parameters, versioned + 16, sizeof(parameters));
    memcpy(&sampler_count, versioned + 24, sizeof(sampler_count));
    memcpy(&samplers, versioned + 32, sizeof(samplers));
    memcpy(&flags, versioned + 40, sizeof(flags));

    if (version < 1 || version > 3 || parameter_count > 4096 ||
        sampler_count > 4096 || (parameter_count && !parameters) ||
        (sampler_count && !samplers))
        return BEER_E_INVALIDARG;

    BeerRootSignatureBytes bytes = {0};
    static const uint8_t magic[8] = {'B','E','E','R','R','S','I','G'};
    int ok = root_bytes_append(&bytes, magic, sizeof(magic)) &&
             root_bytes_u32(&bytes, version) &&
             root_bytes_u32(&bytes, parameter_count) &&
             root_bytes_u32(&bytes, sampler_count) &&
             root_bytes_u32(&bytes, flags);

    /* ROOT_PARAMETER/ROOT_PARAMETER1 are 32 bytes on Win64.  Descriptor table
     * ranges are 20 bytes for v1.0 and 24 bytes for v1.1/v1.2. */
    size_t range_size = version == 1 ? 20 : 24;
    for (uint32_t i = 0; ok && i < parameter_count; ++i) {
        const uint8_t *parameter = parameters + (size_t)i * 32;
        uint32_t type;
        uint32_t visibility;
        memcpy(&type, parameter, sizeof(type));
        memcpy(&visibility, parameter + 24, sizeof(visibility));
        ok = root_bytes_u32(&bytes, type) && root_bytes_u32(&bytes, visibility);
        if (!ok) break;
        if (type == 0) { /* D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE */
            uint32_t range_count;
            const uint8_t *ranges;
            memcpy(&range_count, parameter + 8, sizeof(range_count));
            memcpy(&ranges, parameter + 16, sizeof(ranges));
            if (range_count > 65536 || (range_count && !ranges)) {
                ok = 0;
                break;
            }
            ok = root_bytes_u32(&bytes, range_count) &&
                 root_bytes_append(&bytes, ranges,
                                   (size_t)range_count * range_size);
        } else if (type == 1) { /* 32-bit constants */
            ok = root_bytes_append(&bytes, parameter + 8, 12);
        } else if (type == 2 || type == 3 || type == 4) {
            ok = root_bytes_append(&bytes, parameter + 8,
                                   version == 1 ? 8 : 12);
        } else {
            ok = 0;
        }
    }

    /* STATIC_SAMPLER_DESC is 52 bytes. Version 1.2 adds flags at byte 52. */
    size_t sampler_size = version >= 3 ? 56 : 52;
    if (ok)
        ok = root_bytes_append(&bytes, samplers,
                               (size_t)sampler_count * sampler_size);
    if (!ok || bytes.size == 0) {
        free(bytes.data);
        return BEER_E_INVALIDARG;
    }

    BeerD3DBlob *object = calloc(1, sizeof(*object) + bytes.size);
    if (!object) {
        free(bytes.data);
        return BEER_E_OUTOFMEMORY;
    }
    object->vtable = g_blob_vtable;
    object->references = 1;
    object->size = bytes.size;
    memcpy(object->data, bytes.data, bytes.size);
    free(bytes.data);
    *blob = object;
    return BEER_S_OK;
}
