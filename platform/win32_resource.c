#define _GNU_SOURCE
#include "win32_resource.h"

#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

static uint64_t g_default_module;

void beer_resource_set_default_module(uint64_t image_base) { g_default_module = image_base; }

/* Guest module handles can be synthetic (never mapped), so every read of image memory is checked
 * through process_vm_readv instead of dereferencing the address. */
static int rd(uint64_t address, void *out, size_t size)
{
    struct iovec local = { out, size }, remote = { (void *)(uintptr_t)address, size };
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)size;
}

static int image_directory(uint64_t base, uint32_t *rva, uint32_t *size)
{
    uint16_t mz;
    uint32_t pe_offset, signature;
    uint16_t magic;
    if (!rd(base, &mz, 2) || mz != 0x5a4d) return 0;
    if (!rd(base + 0x3c, &pe_offset, 4) || pe_offset > 0x1000) return 0;
    if (!rd(base + pe_offset, &signature, 4) || signature != 0x4550) return 0;
    if (!rd(base + pe_offset + 24, &magic, 2) || magic != 0x20b) return 0;
    /* IMAGE_DIRECTORY_ENTRY_RESOURCE is index 2 of the PE32+ data directory (offset 112). */
    uint32_t entry[2];
    if (!rd(base + pe_offset + 24 + 112 + 2 * 8, entry, 8) || !entry[0]) return 0;
    *rva = entry[0];
    *size = entry[1];
    return 1;
}

static int wide_equal_nocase(const uint16_t *a, const uint16_t *b, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        uint16_t x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y) return 0;
    }
    return 1;
}

/* One directory level: finds the entry for id/name; returns the raw OffsetToData word. */
static int find_entry(uint64_t root, uint32_t directory, uint64_t key, uint16_t language, int level3,
                      uint32_t *target)
{
    struct { uint32_t characteristics, timestamp; uint16_t major, minor, named, ids; } header;
    if (!rd(root + directory, &header, sizeof(header))) return 0;
    uint32_t count = (uint32_t)header.named + header.ids;
    if (count > 4096) return 0;
    const int by_name = key > 0xffff;
    uint16_t name[256];
    size_t name_len = 0;
    if (by_name) {
        const uint16_t *s = (const uint16_t *)(uintptr_t)key;
        while (name_len < 255 && s[name_len]) { name[name_len] = s[name_len]; ++name_len; }
    }
    uint32_t first = 0;
    int have_first = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t entry[2];
        if (!rd(root + directory + 16 + i * 8, entry, 8)) return 0;
        const int named = i < header.named;
        if (level3) {
            /* Language level: take the exact language, else remember the first. */
            if (!have_first) { first = entry[1]; have_first = 1; }
            if (language && language != 0xffff && !named && (entry[0] & 0xffff) == language) {
                *target = entry[1];
                return 1;
            }
            continue;
        }
        if (named != by_name) continue;
        if (!by_name) {
            if (entry[0] == (uint32_t)key) { *target = entry[1]; return 1; }
            continue;
        }
        uint16_t length;
        uint16_t text[256];
        uint32_t string_offset = entry[0] & 0x7fffffffu;
        if (!rd(root + string_offset, &length, 2) || length != name_len || length > 255) continue;
        if (!rd(root + string_offset + 2, text, (size_t)length * 2)) continue;
        if (wide_equal_nocase(text, name, length)) { *target = entry[1]; return 1; }
    }
    if (level3 && have_first) { *target = first; return 1; }
    return 0;
}

uint64_t beer_resource_find(uint64_t module, uint64_t name, uint64_t type, uint16_t language)
{
    uint64_t base = module ? module : g_default_module;
    uint32_t rva, size, target;
    if (!base || !image_directory(base, &rva, &size)) return 0;
    uint64_t root = base + rva;
    if (!find_entry(root, 0, type, 0, 0, &target) || !(target & 0x80000000u)) return 0;
    if (!find_entry(root, target & 0x7fffffffu, name, 0, 0, &target) || !(target & 0x80000000u)) return 0;
    if (!find_entry(root, target & 0x7fffffffu, 0, language, 1, &target) || (target & 0x80000000u)) return 0;
    return root + target; /* IMAGE_RESOURCE_DATA_ENTRY */
}

const uint8_t *beer_resource_data(uint64_t module, uint64_t resource, uint32_t *size)
{
    uint64_t base = module ? module : g_default_module;
    uint32_t entry[2];
    if (!base || !resource || !rd(resource, entry, 8)) return NULL;
    if (size) *size = entry[1];
    return (const uint8_t *)(uintptr_t)(base + entry[0]);
}

uint64_t __attribute__((ms_abi)) beer_FindResourceExW(uint64_t module, uint64_t type, uint64_t name,
                                                       uint16_t language)
{
    return beer_resource_find(module, name, type, language);
}

uint64_t __attribute__((ms_abi)) beer_FindResourceW(uint64_t module, uint64_t name, uint64_t type)
{
    return beer_resource_find(module, name, type, 0);
}

/* ANSI variants widen string names/types; MAKEINTRESOURCE values pass through. */
static uint64_t widen(uint64_t value, uint16_t *buffer, size_t capacity)
{
    if (value <= 0xffff) return value;
    const char *s = (const char *)(uintptr_t)value;
    size_t i = 0;
    for (; i + 1 < capacity && s[i]; ++i) buffer[i] = (uint8_t)s[i];
    buffer[i] = 0;
    return (uint64_t)(uintptr_t)buffer;
}

uint64_t __attribute__((ms_abi)) beer_FindResourceExA(uint64_t module, uint64_t type, uint64_t name,
                                                       uint16_t language)
{
    uint16_t n[256], t[256];
    return beer_resource_find(module, widen(name, n, 256), widen(type, t, 256), language);
}

uint64_t __attribute__((ms_abi)) beer_FindResourceA(uint64_t module, uint64_t name, uint64_t type)
{
    return beer_FindResourceExA(module, type, name, 0);
}

uint64_t __attribute__((ms_abi)) beer_LoadResource(uint64_t module, uint64_t resource)
{
    return (uint64_t)(uintptr_t)beer_resource_data(module, resource, NULL);
}

uint64_t __attribute__((ms_abi)) beer_LockResource(uint64_t data) { return data; }

uint64_t __attribute__((ms_abi)) beer_SizeofResource(uint64_t module, uint64_t resource)
{
    uint32_t size = 0;
    return beer_resource_data(module, resource, &size) ? size : 0;
}

uint64_t __attribute__((ms_abi)) beer_FreeResource(uint64_t data) { (void)data; return 0; }
