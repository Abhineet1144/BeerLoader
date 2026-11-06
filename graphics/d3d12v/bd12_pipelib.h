#ifndef BD12_PIPELIB_H
#define BD12_PIPELIB_H
/* ID3D12PipelineLibrary / ID3D12PipelineLibrary1.
 *
 * A Windows driver's library holds compiled pipelines by name and serializes them so the next run skips
 * compilation; a blob from another GPU or driver is refused, and the application then compiles again and stores
 * the results (the "compiling shaders" step of many engines). BD12 compiles from the description the
 * application passes to Load*Pipeline and keeps compiled code in its own Vulkan pipeline cache on disk, so the
 * library records which names it holds (and what kind of pipeline each is) together with the GPU identity. */
#include <stddef.h>
#include <stdint.h>

#define BD12_PIPELIB_IDENTITY_SIZE 28
#define BD12_PIPELIB_HR_ADAPTER_NOT_FOUND UINT64_C(0x887e0001)
#define BD12_PIPELIB_HR_DRIVER_VERSION_MISMATCH UINT64_C(0x887e0002)

/* Pipeline kinds, as com_create_pso numbers them: 0 graphics desc, 1 compute desc, 2 stream desc. */
typedef struct {
    uint16_t *name;          /* NUL-terminated UTF-16 */
    uint32_t name_length;    /* characters, without the terminator */
    uint32_t kind;
} Bd12PipelibEntry;

typedef struct {
    uint8_t identity[BD12_PIPELIB_IDENTITY_SIZE];
    Bd12PipelibEntry *entries;
    uint32_t count, capacity;
} Bd12Pipelib;

void bd12_pipelib_init(Bd12Pipelib *lib, const uint8_t identity[BD12_PIPELIB_IDENTITY_SIZE]);
void bd12_pipelib_clear(Bd12Pipelib *lib);
/* Reads a serialized library. An empty blob gives an empty library. Returns 0 (S_OK),
 * BD12_PIPELIB_HR_ADAPTER_NOT_FOUND for another GPU, BD12_PIPELIB_HR_DRIVER_VERSION_MISMATCH for another driver
 * or a blob in a format BD12 does not write (another D3D12 implementation's), or E_INVALIDARG when a BD12 blob
 * is truncated or corrupt. */
uint64_t bd12_pipelib_load(Bd12Pipelib *lib, const void *blob, size_t size);
/* -1 when absent, otherwise the entry index. */
int bd12_pipelib_find(const Bd12Pipelib *lib, const uint16_t *name);
/* 0 on success, -1 when the name is already present, -2 when out of memory. */
int bd12_pipelib_add(Bd12Pipelib *lib, const uint16_t *name, uint32_t kind);
size_t bd12_pipelib_serialized_size(const Bd12Pipelib *lib);
/* Writes exactly bd12_pipelib_serialized_size bytes; returns 0 when size is too small. */
int bd12_pipelib_serialize(const Bd12Pipelib *lib, void *data, size_t size);

#endif
