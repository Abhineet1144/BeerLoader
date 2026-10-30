#include "win32_apiset.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define APISET_VERSION 6u
#define APISET_HASH_FACTOR 31u

typedef struct {
    uint32_t version, size, flags, count, entry_offset, hash_offset, hash_factor;
} ApiSetNamespace;
typedef struct {
    uint32_t flags, name_offset, name_length, hashed_length, value_offset, value_count;
} ApiSetEntry;
typedef struct {
    uint32_t flags, name_offset, name_length, value_offset, value_length;
} ApiSetValue;
typedef struct { uint32_t hash, index; } ApiSetHash;
typedef struct { char contract[160], host[96]; } NormalizedMapping;

static size_t align4(size_t value) { return (value + 3u) & ~(size_t)3u; }

static void normalize_name(char *out, size_t capacity, const char *input, int strip_dll)
{
    size_t length = 0;
    if (!capacity) return;
    while (input && input[length] && length + 1 < capacity) {
        char ch = input[length];
        out[length++] = ch >= 'A' && ch <= 'Z' ? (char)(ch + ('a' - 'A')) : ch;
    }
    out[length] = 0;
    if (strip_dll && length > 4 && !strcmp(out + length - 4, ".dll"))
        out[length - 4] = 0;
}

static int mapping_compare(const void *left, const void *right)
{
    return strcmp(((const NormalizedMapping *)left)->contract,
                  ((const NormalizedMapping *)right)->contract);
}

static uint32_t contract_hash(const char *name)
{
    uint32_t hash = 0;
    for (; *name; ++name) hash = hash * APISET_HASH_FACTOR + (uint8_t)*name;
    return hash;
}

static int hash_compare(const void *left, const void *right)
{
    const ApiSetHash *a = left, *b = right;
    if (a->hash != b->hash) return a->hash < b->hash ? -1 : 1;
    return a->index < b->index ? -1 : a->index != b->index;
}

static void write_utf16(uint8_t *destination, const char *text)
{
    uint16_t *wide = (uint16_t *)destination;
    size_t index = 0;
    do wide[index] = (uint8_t)text[index]; while (text[index++]);
}

void *beer_win32_apiset_build(const BeerApiSetMapping *mappings, size_t count,
                              size_t *namespace_size)
{
    if (namespace_size) *namespace_size = 0;
    if (!mappings || !count || count > UINT32_MAX) return NULL;

    NormalizedMapping *normalized = calloc(count, sizeof(*normalized));
    if (!normalized) return NULL;
    size_t unique = 0;
    for (size_t index = 0; index < count; ++index) {
        NormalizedMapping candidate = {{0}, {0}};
        normalize_name(candidate.contract, sizeof(candidate.contract),
                       mappings[index].contract, 1);
        normalize_name(candidate.host, sizeof(candidate.host), mappings[index].host, 0);
        if (strncmp(candidate.contract, "api-", 4) &&
            strncmp(candidate.contract, "ext-", 4)) continue;
        if (!candidate.host[0]) continue;
        normalized[unique++] = candidate;
    }
    if (!unique) { free(normalized); return NULL; }
    qsort(normalized, unique, sizeof(*normalized), mapping_compare);
    size_t compact = 0;
    for (size_t index = 0; index < unique; ++index) {
        if (compact && !strcmp(normalized[compact - 1].contract,
                               normalized[index].contract)) continue;
        if (compact != index) normalized[compact] = normalized[index];
        ++compact;
    }
    unique = compact;

    size_t entries_offset = sizeof(ApiSetNamespace);
    size_t hashes_offset = entries_offset + unique * sizeof(ApiSetEntry);
    size_t values_offset = hashes_offset + unique * sizeof(ApiSetHash);
    size_t strings_offset = align4(values_offset + unique * sizeof(ApiSetValue));
    size_t total = strings_offset;
    for (size_t index = 0; index < unique; ++index)
        total += (strlen(normalized[index].contract) + 1u) * sizeof(uint16_t) +
                 (strlen(normalized[index].host) + 1u) * sizeof(uint16_t);
    if (total > UINT32_MAX) { free(normalized); return NULL; }

    uint8_t *map = calloc(1, total);
    if (!map) { free(normalized); return NULL; }
    ApiSetNamespace *header = (ApiSetNamespace *)map;
    ApiSetEntry *entries = (ApiSetEntry *)(map + entries_offset);
    ApiSetHash *hashes = (ApiSetHash *)(map + hashes_offset);
    ApiSetValue *values = (ApiSetValue *)(map + values_offset);
    header->version = APISET_VERSION;
    header->size = (uint32_t)total;
    header->count = (uint32_t)unique;
    header->entry_offset = (uint32_t)entries_offset;
    header->hash_offset = (uint32_t)hashes_offset;
    header->hash_factor = APISET_HASH_FACTOR;

    size_t cursor = strings_offset;
    for (size_t index = 0; index < unique; ++index) {
        size_t contract_length = strlen(normalized[index].contract);
        size_t host_length = strlen(normalized[index].host);
        entries[index].name_offset = (uint32_t)cursor;
        entries[index].name_length = (uint32_t)(contract_length * sizeof(uint16_t));
        entries[index].hashed_length = entries[index].name_length;
        entries[index].value_offset = (uint32_t)(values_offset + index * sizeof(ApiSetValue));
        entries[index].value_count = 1;
        write_utf16(map + cursor, normalized[index].contract);
        cursor += (contract_length + 1u) * sizeof(uint16_t);

        values[index].value_offset = (uint32_t)cursor;
        values[index].value_length = (uint32_t)(host_length * sizeof(uint16_t));
        write_utf16(map + cursor, normalized[index].host);
        cursor += (host_length + 1u) * sizeof(uint16_t);

        hashes[index].hash = contract_hash(normalized[index].contract);
        hashes[index].index = (uint32_t)index;
    }
    qsort(hashes, unique, sizeof(*hashes), hash_compare);
    free(normalized);
    if (namespace_size) *namespace_size = total;
    return map;
}

static int span_valid(size_t offset, size_t length, size_t total)
{
    return offset <= total && length <= total - offset;
}

int beer_win32_apiset_validate(const void *namespace_map, size_t namespace_size)
{
    if (!namespace_map || namespace_size < sizeof(ApiSetNamespace)) return 0;
    const uint8_t *map = namespace_map;
    const ApiSetNamespace *header = namespace_map;
    if (header->version != APISET_VERSION || header->size != namespace_size ||
        header->hash_factor != APISET_HASH_FACTOR) return 0;
    if (!span_valid(header->entry_offset,
                    (size_t)header->count * sizeof(ApiSetEntry), namespace_size) ||
        !span_valid(header->hash_offset,
                    (size_t)header->count * sizeof(ApiSetHash), namespace_size)) return 0;
    const ApiSetEntry *entries = (const ApiSetEntry *)(map + header->entry_offset);
    const ApiSetHash *hashes = (const ApiSetHash *)(map + header->hash_offset);
    for (uint32_t index = 0; index < header->count; ++index) {
        const ApiSetEntry *entry = &entries[index];
        if (!entry->name_length || (entry->name_length & 1u) ||
            !span_valid(entry->name_offset, entry->name_length, namespace_size) ||
            entry->hashed_length > entry->name_length || entry->value_count != 1 ||
            !span_valid(entry->value_offset, sizeof(ApiSetValue), namespace_size) ||
            hashes[index].index >= header->count ||
            (index && hashes[index - 1].hash > hashes[index].hash)) return 0;
        const ApiSetValue *value = (const ApiSetValue *)(map + entry->value_offset);
        if (!value->value_length || (value->value_length & 1u) ||
            !span_valid(value->value_offset, value->value_length, namespace_size)) return 0;
    }
    return 1;
}
