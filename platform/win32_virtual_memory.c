#define _GNU_SOURCE
#include "win32_virtual_memory.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define BEER_VM_CAPACITY 1024u

typedef struct BeerAllocation {
    void *base;
    size_t size;
    uint32_t allocation_protect;
} BeerAllocation;

static BeerAllocation g_allocations[BEER_VM_CAPACITY];
static pthread_mutex_t g_allocation_mutex = PTHREAD_MUTEX_INITIALIZER;

static size_t page_size(void)
{
    static size_t value;
    if (!value) {
        long queried = sysconf(_SC_PAGESIZE);
        value = queried > 0 ? (size_t)queried : 4096u;
    }
    return value;
}

static uintptr_t align_down(uintptr_t value, size_t alignment)
{
    return value & ~((uintptr_t)alignment - 1u);
}

static size_t align_up_size(size_t value, size_t alignment)
{
    if (value > SIZE_MAX - (alignment - 1u)) return 0;
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static int windows_to_host_protection(uint32_t protection)
{
    switch (protection & UINT32_C(0xff)) {
    case BEER_PAGE_NOACCESS: return PROT_NONE;
    case BEER_PAGE_READONLY: return PROT_READ;
    case BEER_PAGE_READWRITE: return PROT_READ | PROT_WRITE;
    case BEER_PAGE_EXECUTE: return PROT_EXEC;
    case BEER_PAGE_EXECUTE_READ: return PROT_EXEC | PROT_READ;
    case BEER_PAGE_EXECUTE_READWRITE: return PROT_EXEC | PROT_READ | PROT_WRITE;
    default: return -1;
    }
}

static uint32_t host_to_windows_protection(int read, int write, int execute)
{
    if (execute) {
        if (write) return BEER_PAGE_EXECUTE_READWRITE;
        if (read) return BEER_PAGE_EXECUTE_READ;
        return BEER_PAGE_EXECUTE;
    }
    if (write) return BEER_PAGE_READWRITE;
    if (read) return BEER_PAGE_READONLY;
    return BEER_PAGE_NOACCESS;
}

static BeerAllocation *find_allocation_locked(const void *address)
{
    uintptr_t value = (uintptr_t)address;
    for (size_t index = 0; index < BEER_VM_CAPACITY; ++index) {
        uintptr_t base = (uintptr_t)g_allocations[index].base;
        if (base && value >= base && value - base < g_allocations[index].size)
            return &g_allocations[index];
    }
    return NULL;
}

uint64_t g_beer_vm_live_bytes;   /* mapped by VirtualAlloc, not yet released */

void *beer_virtual_alloc(void *address, size_t size, uint32_t allocation_type,
                         uint32_t protection)
{
    if (!size || !(allocation_type & (BEER_MEM_RESERVE | BEER_MEM_COMMIT))) {
        errno = EINVAL;
        return NULL;
    }
    int host_protection = windows_to_host_protection(protection);
    if (host_protection < 0) {
        errno = EINVAL;
        return NULL;
    }
    size_t page = page_size();
    uintptr_t requested = align_down((uintptr_t)address, page);
    size_t prefix = address ? (uintptr_t)address - requested : 0;
    size_t mapped_size = align_up_size(size + prefix, page);
    if (!mapped_size) {
        errno = ENOMEM;
        return NULL;
    }
    if (!(allocation_type & BEER_MEM_COMMIT)) host_protection = PROT_NONE;
    if (address) {
        /* Commit (or re-commit) pages inside a region this loader already reserved. */
        pthread_mutex_lock(&g_allocation_mutex);
        BeerAllocation *existing = find_allocation_locked((void *)requested);
        int inside = existing &&
            requested + mapped_size <= (uintptr_t)existing->base + existing->size;
        pthread_mutex_unlock(&g_allocation_mutex);
        if (existing) {
            if (!inside || !(allocation_type & BEER_MEM_COMMIT) ||
                (allocation_type & BEER_MEM_RESERVE && !(allocation_type & BEER_MEM_COMMIT))) {
                errno = EEXIST;
                return NULL;
            }
            if (mprotect((void *)requested, mapped_size, host_protection) != 0) return NULL;
            return address;
        }
    }
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_FIXED_NOREPLACE
    if (address) flags |= MAP_FIXED_NOREPLACE;
#endif
    void *mapped;
    if (!address) {
        /* Windows reserves in 64 KiB allocation-granularity units and returns
         * 64 KiB-aligned bases; runtimes and allocators rely on that. */
        const size_t granularity = 65536;
        size_t region = align_up_size(mapped_size, granularity);
        if (!region) { errno = ENOMEM; return NULL; }
        size_t span = region + granularity;
        if (span < region) { errno = ENOMEM; return NULL; }
        void *raw = mmap(NULL, span, host_protection, flags, -1, 0);
        if (raw == MAP_FAILED) return NULL;
        uintptr_t base = ((uintptr_t)raw + granularity - 1) & ~(uintptr_t)(granularity - 1);
        if (base > (uintptr_t)raw) munmap(raw, base - (uintptr_t)raw);
        uintptr_t tail = base + region, raw_end = (uintptr_t)raw + span;
        if (raw_end > tail) munmap((void *)tail, raw_end - tail);
        mapped = (void *)base;
        mapped_size = region;
    } else {
        mapped = mmap((void *)requested, mapped_size, host_protection, flags, -1, 0);
    }
    if (mapped == MAP_FAILED) return NULL;
#ifndef MAP_FIXED_NOREPLACE
    if (address && mapped != (void *)requested) {
        munmap(mapped, mapped_size);
        errno = EEXIST;
        return NULL;
    }
#endif
    pthread_mutex_lock(&g_allocation_mutex);
    BeerAllocation *slot = NULL;
    for (size_t index = 0; index < BEER_VM_CAPACITY; ++index)
        if (!g_allocations[index].base) { slot = &g_allocations[index]; break; }
    if (slot) {
        g_beer_vm_live_bytes += mapped_size;
        slot->base = mapped;
        slot->size = mapped_size;
        slot->allocation_protect = protection;
    }
    pthread_mutex_unlock(&g_allocation_mutex);
    if (!slot) {
        munmap(mapped, mapped_size);
        errno = ENOMEM;
        return NULL;
    }
    return mapped;
}

int beer_virtual_free(void *address, size_t size, uint32_t free_type)
{
    if (!address) return 0;
    if (free_type == BEER_MEM_RELEASE) {
        if (size != 0) return 0;
        pthread_mutex_lock(&g_allocation_mutex);
        BeerAllocation *allocation = find_allocation_locked(address);
        if (!allocation || allocation->base != address) {
            pthread_mutex_unlock(&g_allocation_mutex);
            return 0;
        }
        void *base = allocation->base;
        size_t allocation_size = allocation->size;
        g_beer_vm_live_bytes -= allocation_size;
        memset(allocation, 0, sizeof(*allocation));
        pthread_mutex_unlock(&g_allocation_mutex);
        return munmap(base, allocation_size) == 0;
    }
    if (free_type == BEER_MEM_DECOMMIT && size) {
        size_t page = page_size();
        uintptr_t base = align_down((uintptr_t)address, page);
        size_t length = align_up_size((uintptr_t)address - base + size, page);
        if (!length || mprotect((void *)base, length, PROT_NONE) != 0) return 0;
        (void)madvise((void *)base, length, MADV_DONTNEED);
        return 1;
    }
    return 0;
}

static int query_proc_map(uintptr_t address, uintptr_t *start, uintptr_t *end,
                          int *read, int *write, int *execute, int *shared,
                          uintptr_t *next_start)
{
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) return 0;
    char line[1024];
    int found = 0;
    *next_start = UINTPTR_MAX;
    while (fgets(line, sizeof(line), maps)) {
        unsigned long first = 0, last = 0;
        char permissions[5] = {0};
        if (sscanf(line, "%lx-%lx %4s", &first, &last, permissions) != 3) continue;
        if ((uintptr_t)first > address && (uintptr_t)first < *next_start)
            *next_start = (uintptr_t)first;
        if (address >= (uintptr_t)first && address < (uintptr_t)last) {
            *start = (uintptr_t)first;
            *end = (uintptr_t)last;
            *read = permissions[0] == 'r';
            *write = permissions[1] == 'w';
            *execute = permissions[2] == 'x';
            *shared = permissions[3] == 's';
            found = 1;
            break;
        }
    }
    fclose(maps);
    return found;
}

size_t beer_virtual_query(const void *address, BeerMemoryBasicInformation *info,
                          size_t info_size)
{
    if (!info || info_size < sizeof(*info)) return 0;
    uintptr_t value = (uintptr_t)address;
    uintptr_t start = 0, end = 0, next = UINTPTR_MAX;
    int read = 0, write = 0, execute = 0, shared = 0;
    memset(info, 0, sizeof(*info));
    if (!query_proc_map(value, &start, &end, &read, &write, &execute, &shared, &next)) {
        size_t page = page_size();
        start = align_down(value, page);
        end = next == UINTPTR_MAX ? UINTPTR_MAX : next;
        info->base_address = start;
        info->region_size = end > start ? end - start : page;
        info->state = BEER_MEM_FREE;
        return sizeof(*info);
    }
    info->base_address = start;
    info->region_size = end - start;
    info->state = BEER_MEM_COMMIT;
    info->protect = host_to_windows_protection(read, write, execute);
    info->type = shared ? BEER_MEM_MAPPED : BEER_MEM_PRIVATE;

    pthread_mutex_lock(&g_allocation_mutex);
    BeerAllocation *allocation = find_allocation_locked(address);
    if (allocation) {
        info->allocation_base = (uint64_t)(uintptr_t)allocation->base;
        info->allocation_protect = allocation->allocation_protect;
        info->type = BEER_MEM_PRIVATE;
    } else {
        info->allocation_base = start;
        info->allocation_protect = info->protect;
    }
    pthread_mutex_unlock(&g_allocation_mutex);
    return sizeof(*info);
}

int beer_virtual_protect(void *address, size_t size, uint32_t new_protection,
                         uint32_t *old_protection)
{
    if (!address || !size) return 0;
    BeerMemoryBasicInformation info;
    if (!beer_virtual_query(address, &info, sizeof(info)) || info.state != BEER_MEM_COMMIT)
        return 0;
    int host_protection = windows_to_host_protection(new_protection);
    if (host_protection < 0) return 0;
    size_t page = page_size();
    uintptr_t base = align_down((uintptr_t)address, page);
    size_t length = align_up_size((uintptr_t)address - base + size, page);
    if (!length || mprotect((void *)base, length, host_protection) != 0) return 0;
    if (old_protection) *old_protection = info.protect;
    return 1;
}

void beer_virtual_memory_reset(void)
{
    pthread_mutex_lock(&g_allocation_mutex);
    for (size_t index = 0; index < BEER_VM_CAPACITY; ++index) {
        if (g_allocations[index].base)
            munmap(g_allocations[index].base, g_allocations[index].size);
        memset(&g_allocations[index], 0, sizeof(g_allocations[index]));
    }
    pthread_mutex_unlock(&g_allocation_mutex);
}
