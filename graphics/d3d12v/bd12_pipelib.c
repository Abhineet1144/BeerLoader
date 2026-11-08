#include "bd12_pipelib.h"

#include <stdlib.h>
#include <string.h>

/* Blob: "BD12PLIB", u32 version, identity, u32 count, then per entry u32 kind, u32 length, length UTF-16 units. */
static const char pipelib_magic[8] = { 'B', 'D', '1', '2', 'P', 'L', 'I', 'B' };
#define PIPELIB_VERSION 1u
#define PIPELIB_HEADER (8 + 4 + BD12_PIPELIB_IDENTITY_SIZE + 4)
#define HR_INVALIDARG UINT64_C(0x80070057)

void bd12_pipelib_init(Bd12Pipelib *lib, const uint8_t identity[BD12_PIPELIB_IDENTITY_SIZE])
{
    memset(lib, 0, sizeof(*lib));
    memcpy(lib->identity, identity, BD12_PIPELIB_IDENTITY_SIZE);
}

void bd12_pipelib_clear(Bd12Pipelib *lib)
{
    for (uint32_t i = 0; i < lib->count; ++i) free(lib->entries[i].name);
    free(lib->entries);
    lib->entries = NULL;
    lib->count = lib->capacity = 0;
}

static uint32_t wide_length(const uint16_t *name)
{
    uint32_t n = 0;
    while (name[n]) ++n;
    return n;
}

static int append(Bd12Pipelib *lib, const uint16_t *name, uint32_t length, uint32_t kind)
{
    if (lib->count == lib->capacity) {
        const uint32_t capacity = lib->capacity ? lib->capacity * 2 : 64;
        Bd12PipelibEntry *grown = realloc(lib->entries, capacity * sizeof(*grown));
        if (!grown) return -2;
        lib->entries = grown;
        lib->capacity = capacity;
    }
    uint16_t *copy = malloc(((size_t)length + 1) * sizeof(uint16_t));
    if (!copy) return -2;
    memcpy(copy, name, (size_t)length * sizeof(uint16_t));
    copy[length] = 0;
    lib->entries[lib->count++] = (Bd12PipelibEntry){ copy, length, kind };
    return 0;
}

uint64_t bd12_pipelib_load(Bd12Pipelib *lib, const void *blob, size_t size)
{
    if (!blob || !size) return 0;
    const uint8_t *p = blob;
    if (size < sizeof(pipelib_magic) || memcmp(p, pipelib_magic, sizeof(pipelib_magic)))
        return BD12_PIPELIB_HR_DRIVER_VERSION_MISMATCH;            /* another implementation's library */
    if (size < PIPELIB_HEADER) return HR_INVALIDARG;
    uint32_t version, count;
    memcpy(&version, p + 8, 4);
    if (version != PIPELIB_VERSION) return BD12_PIPELIB_HR_DRIVER_VERSION_MISMATCH;
    const uint8_t *identity = p + 12;
    if (memcmp(identity, lib->identity, 8)) return BD12_PIPELIB_HR_ADAPTER_NOT_FOUND;   /* vendor, device */
    if (memcmp(identity + 8, lib->identity + 8, BD12_PIPELIB_IDENTITY_SIZE - 8))
        return BD12_PIPELIB_HR_DRIVER_VERSION_MISMATCH;
    memcpy(&count, p + 12 + BD12_PIPELIB_IDENTITY_SIZE, 4);
    size_t at = PIPELIB_HEADER;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t kind, length;
        if (size - at < 8) { bd12_pipelib_clear(lib); return HR_INVALIDARG; }
        memcpy(&kind, p + at, 4);
        memcpy(&length, p + at + 4, 4);
        at += 8;
        if (kind > 2 || length == 0 || (size - at) / 2 < length) { bd12_pipelib_clear(lib); return HR_INVALIDARG; }
        uint16_t *name = malloc((size_t)length * sizeof(uint16_t));
        if (!name) { bd12_pipelib_clear(lib); return UINT64_C(0x8007000e); }
        memcpy(name, p + at, (size_t)length * 2);
        at += (size_t)length * 2;
        int duplicate = 0;
        for (uint32_t j = 0; j < lib->count && !duplicate; ++j)
            duplicate = lib->entries[j].name_length == length && !memcmp(lib->entries[j].name, name, (size_t)length * 2);
        const int result = duplicate ? -1 : append(lib, name, length, kind);
        free(name);
        if (result) { bd12_pipelib_clear(lib); return result == -2 ? UINT64_C(0x8007000e) : HR_INVALIDARG; }
    }
    if (at != size) { bd12_pipelib_clear(lib); return HR_INVALIDARG; }
    return 0;
}

int bd12_pipelib_find(const Bd12Pipelib *lib, const uint16_t *name)
{
    if (!name) return -1;
    const uint32_t length = wide_length(name);
    for (uint32_t i = 0; i < lib->count; ++i)
        if (lib->entries[i].name_length == length && !memcmp(lib->entries[i].name, name, (size_t)length * 2))
            return (int)i;
    return -1;
}

int bd12_pipelib_add(Bd12Pipelib *lib, const uint16_t *name, uint32_t kind)
{
    if (bd12_pipelib_find(lib, name) >= 0) return -1;
    return append(lib, name, wide_length(name), kind);
}

size_t bd12_pipelib_serialized_size(const Bd12Pipelib *lib)
{
    size_t size = PIPELIB_HEADER;
    for (uint32_t i = 0; i < lib->count; ++i) size += 8 + (size_t)lib->entries[i].name_length * 2;
    return size;
}

int bd12_pipelib_serialize(const Bd12Pipelib *lib, void *data, size_t size)
{
    if (!data || size < bd12_pipelib_serialized_size(lib)) return 0;
    uint8_t *p = data;
    const uint32_t version = PIPELIB_VERSION;
    memcpy(p, pipelib_magic, 8);
    memcpy(p + 8, &version, 4);
    memcpy(p + 12, lib->identity, BD12_PIPELIB_IDENTITY_SIZE);
    memcpy(p + 12 + BD12_PIPELIB_IDENTITY_SIZE, &lib->count, 4);
    size_t at = PIPELIB_HEADER;
    for (uint32_t i = 0; i < lib->count; ++i) {
        memcpy(p + at, &lib->entries[i].kind, 4);
        memcpy(p + at + 4, &lib->entries[i].name_length, 4);
        memcpy(p + at + 8, lib->entries[i].name, (size_t)lib->entries[i].name_length * 2);
        at += 8 + (size_t)lib->entries[i].name_length * 2;
    }
    return 1;
}

#ifndef BD12_PIPELIB_STANDALONE
/* COM object ---------------------------------------------------------------------------------------------- */
#include "bd12_com.h"
#include <pthread.h>
#include <stdio.h>

int com_pso_is_compute(ComObj *pso);   /* bd12_com_list.c */

static const uint8_t iid_pipeline_library[16] = { 0xa8, 0x26, 0x42, 0xc6, 0x01, 0x92, 0xaf, 0x46,
                                                  0xb4, 0xcc, 0x53, 0xfb, 0x9f, 0xf7, 0x41, 0x4f };
static const uint8_t iid_pipeline_library1[16] = { 0x42, 0xbf, 0xea, 0x80, 0x68, 0x25, 0x5e, 0x4e,
                                                   0xbd, 0x82, 0xc3, 0x7f, 0x86, 0x96, 0x1d, 0xc3 };

typedef struct {
    ComObj obj;
    pthread_mutex_t lock;
    Bd12Pipelib lib;          /* names and kinds only: Load*Pipeline rebuilds from the caller's description, so stored
                               * pipeline objects are not kept (a BD12 pipeline holds its shader bytecode, and
                               * engines store tens of thousands) */
    uint32_t loads, misses;
    /* Stored pipeline objects: the library holds a reference to each until it is released, as D3D12 specifies
     * (a pipeline that was not compiled yet costs only its description; shader bytecode is shared). */
    ComObj **held;
    uint32_t held_count, held_capacity;
} ComPipelib;

static void pipelib_destroy(ComObj *o)
{
    ComPipelib *self = (ComPipelib *)o;
    fprintf(stderr, "[BD12] pipeline library released: %u pipelines, %u loads, %u misses\n",
            self->lib.count, self->loads, self->misses);
    bd12_pipelib_clear(&self->lib);
    for (uint32_t i = 0; i < self->held_count; ++i) com_release(self->held[i]);
    free(self->held);
    pthread_mutex_destroy(&self->lock);
    free(self);
}

static HR MS pipelib_query(ComObj *s, const uint8_t *riid, void **out)
{ return com_obj_query(s, riid, out, iid_pipeline_library, iid_pipeline_library1); }

/* BEER_BD12_PIPELIB_LOG=1: every 500th call of each method, with totals (what an engine's precompile step does). */
static uint64_t g_pipelib_calls[5];   /* store, load hit, load miss, serialized-size, serialize */
static void pipelib_count(int which)
{
    static int log = -1;
    if (log < 0) log = getenv("BEER_BD12_PIPELIB_LOG") != NULL;
    const uint64_t n = __atomic_add_fetch(&g_pipelib_calls[which], 1, __ATOMIC_RELAXED);
    if (log && (n == 1 || n % 500 == 0))
        fprintf(stderr, "[PIPELIB] store=%llu load-hit=%llu load-miss=%llu size=%llu serialize=%llu\n",
                (unsigned long long)g_pipelib_calls[0], (unsigned long long)g_pipelib_calls[1],
                (unsigned long long)g_pipelib_calls[2], (unsigned long long)g_pipelib_calls[3],
                (unsigned long long)g_pipelib_calls[4]);
}

static HR MS pipelib_store(ComPipelib *self, const uint16_t *name, ComObj *pso)
{
    pipelib_count(0);
    if (!name || !pso) return HR_E_INVALIDARG;
    pthread_mutex_lock(&self->lock);
    int result = bd12_pipelib_add(&self->lib, name, com_pso_is_compute(pso) ? 1u : 0u);
    if (!result && !getenv("BEER_BD12_PIPELIB_NO_HOLD")) {
        if (self->held_count == self->held_capacity) {
            const uint32_t capacity = self->held_capacity ? self->held_capacity * 2 : 256;
            ComObj **grown = realloc(self->held, capacity * sizeof(*grown));
            if (grown) { self->held = grown; self->held_capacity = capacity; }
        }
        if (self->held_count < self->held_capacity) {
            com_add_ref(pso);
            self->held[self->held_count++] = pso;
        }
    }
    pthread_mutex_unlock(&self->lock);
    return result == -1 ? HR_E_INVALIDARG                       /* the name is taken */
         : result ? HR_E_OUTOFMEMORY : HR_OK;
}

/* kind: 0 graphics desc, 1 compute desc, 2 stream desc. */
static HR load(ComPipelib *self, const uint16_t *name, const void *desc, int kind, const uint8_t *riid, void **out)
{
    if (!out) return HR_E_INVALIDARG;
    *out = NULL;
    if (!name || !desc) return HR_E_INVALIDARG;
    pthread_mutex_lock(&self->lock);
    const int index = bd12_pipelib_find(&self->lib, name);
    const int found = index >= 0 && (kind == 2 || self->lib.entries[index].kind == 2 ||
                                     self->lib.entries[index].kind == (uint32_t)kind);
    if (found) ++self->loads; else ++self->misses;
    pthread_mutex_unlock(&self->lock);
    pipelib_count(found ? 1 : 2);
    if (!found) return HR_E_INVALIDARG;                         /* not in the library: the caller compiles */
    extern __thread int g_bd12_pso_from_library;   /* bd12_com_list.c: see compile_at_create */
    g_bd12_pso_from_library = 1;
    const HR hr = com_create_pso(self->obj.dev, desc, kind, riid, out);
    g_bd12_pso_from_library = 0;
    return hr;
}

static HR MS pipelib_load_graphics(ComPipelib *self, const uint16_t *name, const void *desc, const uint8_t *riid,
                                   void **out)
{ return load(self, name, desc, 0, riid, out); }
static HR MS pipelib_load_compute(ComPipelib *self, const uint16_t *name, const void *desc, const uint8_t *riid,
                                  void **out)
{ return load(self, name, desc, 1, riid, out); }
static HR MS pipelib_load_stream(ComPipelib *self, const uint16_t *name, const void *desc, const uint8_t *riid,
                                 void **out)
{ return load(self, name, desc, 2, riid, out); }

static uint64_t MS pipelib_serialized_size(ComPipelib *self)
{
    pipelib_count(3);
    pthread_mutex_lock(&self->lock);
    const size_t size = bd12_pipelib_serialized_size(&self->lib);
    pthread_mutex_unlock(&self->lock);
    return size;
}

static HR MS pipelib_serialize(ComPipelib *self, void *data, uint64_t size)
{
    pipelib_count(4);
    pthread_mutex_lock(&self->lock);
    const int ok = bd12_pipelib_serialize(&self->lib, data, (size_t)size);
    const uint32_t count = self->lib.count;
    pthread_mutex_unlock(&self->lock);
    if (ok) fprintf(stderr, "[BD12] pipeline library serialized: %u pipelines\n", count);
    return ok ? HR_OK : HR_E_INVALIDARG;
}

static void *pipelib_vtable[14] = {
    [0] = pipelib_query, [1] = com_add_ref, [2] = com_release,
    [3] = com_obj_get_private, [4] = com_obj_set_private, [5] = com_obj_set_private_iface,
    [6] = com_obj_set_name, [7] = com_obj_get_device,
    [8] = pipelib_store, [9] = pipelib_load_graphics, [10] = pipelib_load_compute,
    [11] = pipelib_serialized_size, [12] = pipelib_serialize,
    /* ID3D12PipelineLibrary1 */ [13] = pipelib_load_stream,
};

HR com_create_pipeline_library(ComDevice *dev, const void *blob, size_t size, const uint8_t *riid, void **out)
{
    if (!out) return HR_E_INVALIDARG;
    *out = NULL;
    if (!riid || (!com_iid_equal(riid, iid_pipeline_library) && !com_iid_equal(riid, iid_pipeline_library1) &&
                  !com_iid_equal(riid, com_iid_iunknown)))
        return HR_E_NOINTERFACE;
    ComPipelib *self = calloc(1, sizeof(*self));
    if (!self) return HR_E_OUTOFMEMORY;
    bd12_pipelib_init(&self->lib, dev->bd->pipeline_identity);
    const HR hr = bd12_pipelib_load(&self->lib, blob, size);
    fprintf(stderr, "[BD12] CreatePipelineLibrary(%zu-byte blob) -> 0x%llx, %u pipelines\n", size,
            (unsigned long long)hr, self->lib.count);
    if (hr) { bd12_pipelib_clear(&self->lib); free(self); return hr; }
    pthread_mutex_init(&self->lock, NULL);
    com_obj_init(&self->obj, COM_KIND_COUNT + 2 /* after the swap chain's COM_KIND_COUNT + 1 */, pipelib_vtable, dev,
                 pipelib_destroy);
    *out = self;
    return HR_OK;
}
#endif
