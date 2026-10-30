#include "win32_apiset.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    uint32_t version, size, flags, count, entry_offset, hash_offset, hash_factor;
} ApiSetNamespace;
typedef struct {
    uint32_t flags, name_offset, name_length, hashed_length, value_offset, value_count;
} ApiSetEntry;
typedef struct {
    uint32_t flags, name_offset, name_length, value_offset, value_length;
} ApiSetValue;

static int wide_equals(const uint8_t *map, uint32_t offset, uint32_t bytes,
                       const char *ascii)
{
    const uint16_t *wide = (const uint16_t *)(map + offset);
    size_t count = bytes / 2u, index = 0;
    for (; index < count && ascii[index]; ++index)
        if (wide[index] != (uint8_t)ascii[index]) return 0;
    return index == count && ascii[index] == 0;
}

int main(void)
{
    const BeerApiSetMapping mappings[] = {
        {"API-MS-WIN-CORE-REGISTRY-L1-1-0.DLL", "kernel32.dll"},
        {"api-ms-win-core-file-l1-2-0", "kernel32.dll"},
        {"api-ms-win-core-registry-l1-1-0", "ignored-duplicate.dll"},
        {"not-an-api-set.dll", "kernel32.dll"},
    };
    size_t size = 0;
    uint8_t *map = beer_win32_apiset_build(mappings,
        sizeof(mappings) / sizeof(mappings[0]), &size);
    assert(map && beer_win32_apiset_validate(map, size));
    ApiSetNamespace *header = (ApiSetNamespace *)map;
    assert(header->version == 6 && header->count == 2 && header->hash_factor == 31);
    ApiSetEntry *entries = (ApiSetEntry *)(map + header->entry_offset);
    assert(wide_equals(map, entries[0].name_offset, entries[0].name_length,
                       "api-ms-win-core-file-l1-2-0"));
    assert(wide_equals(map, entries[1].name_offset, entries[1].name_length,
                       "api-ms-win-core-registry-l1-1-0"));
    ApiSetValue *value = (ApiSetValue *)(map + entries[1].value_offset);
    assert(wide_equals(map, value->value_offset, value->value_length,
                       "kernel32.dll"));

    ((ApiSetNamespace *)map)->version = 5;
    assert(!beer_win32_apiset_validate(map, size));
    free(map);
    puts("Win32 API-set namespace tests passed");
    return 0;
}
