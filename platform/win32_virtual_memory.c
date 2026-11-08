#define _GNU_SOURCE
#include "win32_virtual_memory.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>


/* ---- Bottom-up placement ------------------------------------------------------------------------------------
 * Windows places VirtualAlloc regions, heaps and thread stacks bottom-up: the lowest free range that fits, from
 * 64 KiB upward (randomised only for ASLR-enabled images, and still low). Linux mmap is top-down from 0x7fff...,
 * so guest pointers had all 47 bits set. Code written against Windows can depend on the difference: RE9's
 * protection (an image without DYNAMIC_BASE) keeps pointers to its own allocations in 32 bits and rebuilt them
 * with the high half of an unrelated address. Ranges handed out here, and host mappings found in the way, are
 * kept in a sorted list so a search does not probe them again. BEER_TOPDOWN_VM=1 disables the placement. */
#define BOTTOM_UP_LOW  UINT64_C(0x10000)
#define BOTTOM_UP_HIGH UINT64_C(0x100000000)

typedef struct { uintptr_t start, end; int foreign; } BottomUpRange;
static BottomUpRange *g_ranges;
static size_t g_range_count, g_range_capacity;
static pthread_mutex_t g_range_mutex = PTHREAD_MUTEX_INITIALIZER;

int beer_vm_bottom_up_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *off = getenv("BEER_TOPDOWN_VM");
        enabled = !(off && *off && strcmp(off, "0"));
    }
    return enabled;
}

static void range_insert_locked(uintptr_t start, uintptr_t end, int foreign)
{
    if (g_range_count == g_range_capacity) {
        size_t capacity = g_range_capacity ? g_range_capacity * 2 : 256;
        BottomUpRange *grown = realloc(g_ranges, capacity * sizeof(*grown));
        if (!grown) return;
        g_ranges = grown;
        g_range_capacity = capacity;
    }
    size_t at = 0;
    while (at < g_range_count && g_ranges[at].start < start) ++at;
    memmove(g_ranges + at + 1, g_ranges + at, (g_range_count - at) * sizeof(*g_ranges));
    g_ranges[at] = (BottomUpRange){ start, end, foreign };
    ++g_range_count;
}

/* End of the host mapping that overlaps [start, end), from /proc/self/maps (0 if none). */
static uintptr_t host_mapping_end(uintptr_t start, uintptr_t end)
{
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) return 0;
    char line[512];
    uintptr_t found = 0;
    while (fgets(line, sizeof(line), maps)) {
        unsigned long a, b;
        if (sscanf(line, "%lx-%lx", &a, &b) != 2) continue;
        if (a >= end) break;
        if (b > start) { found = b; break; }
    }
    fclose(maps);
    return found;
}

void *beer_vm_map_bottom_up(size_t size, size_t alignment, int protection, int extra_flags)
{
#ifdef MAP_FIXED_NOREPLACE
    if (!beer_vm_bottom_up_enabled() || !size) return MAP_FAILED;
    if (alignment < 65536) alignment = 65536;
    size = (size + 0xffff) & ~(size_t)0xffff;
    pthread_mutex_lock(&g_range_mutex);
    uintptr_t candidate = BOTTOM_UP_LOW;
    void *result = MAP_FAILED;
    for (int attempts = 0; attempts < 4096; ++attempts) {
        candidate = (candidate + alignment - 1) & ~(uintptr_t)(alignment - 1);
        /* Skip known ranges that overlap the candidate (the list is sorted by start). */
        int moved = 1;
        while (moved) {
            moved = 0;
            for (size_t i = 0; i < g_range_count; ++i) {
                if (g_ranges[i].start >= candidate + size) break;
                if (g_ranges[i].end > candidate) {
                    candidate = (g_ranges[i].end + alignment - 1) & ~(uintptr_t)(alignment - 1);
                    moved = 1;
                }
            }
        }
        if (candidate + size > BOTTOM_UP_HIGH || candidate + size < candidate) break;
        void *mapped = mmap((void *)candidate, size, protection,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | extra_flags, -1, 0);
        if (mapped == (void *)candidate) {
            range_insert_locked(candidate, candidate + size, 0);
            result = mapped;
            break;
        }
        if (mapped != MAP_FAILED) munmap(mapped, size);          /* a kernel that treated the address as a hint */
        else if (errno != EEXIST) break;
        /* A host mapping is in the way: remember it and continue after it. */
        uintptr_t host_end = host_mapping_end(candidate, candidate + size);
        if (!host_end) host_end = candidate + 65536;
        range_insert_locked(candidate, host_end, 1);
        candidate = host_end;
    }
    pthread_mutex_unlock(&g_range_mutex);
    return result;
#else
    (void)size; (void)alignment; (void)protection; (void)extra_flags;
    return MAP_FAILED;
#endif
}

void beer_vm_unmap_bottom_up(void *base, size_t size)
{
    const uintptr_t start = (uintptr_t)base;
    pthread_mutex_lock(&g_range_mutex);
    for (size_t i = 0; i < g_range_count; ++i)
        if (g_ranges[i].start == start && !g_ranges[i].foreign) {
            memmove(g_ranges + i, g_ranges + i + 1, (g_range_count - i - 1) * sizeof(*g_ranges));
            --g_range_count;
            break;
        }
    pthread_mutex_unlock(&g_range_mutex);
    munmap(base, size);
}

/* IsBadStringPtr is an obsolete snapshot probe, not a lifetime guarantee.
 * Read through the kernel rather than dereferencing guest memory or borrowing
 * the process-wide fault handler. A partial read can still contain the NUL
 * before an inaccessible page, so examine complete characters before failing. */
static int bad_string_pointer(const void *string, size_t maximum, size_t width)
{
    uintptr_t address = (uintptr_t)string;
    int saved_errno = errno;
    int bad = 0;
    while (maximum) {
        unsigned char bytes[256];
        size_t count = maximum < sizeof(bytes) / width ?
                       maximum : sizeof(bytes) / width;
        size_t available = (UINTPTR_MAX - address) / width;
        if (count > available) count = available;
        if (!address || !count) { bad = 1; break; }
        size_t length = count * width;
        struct iovec local = {bytes, length};
        struct iovec remote = {(void *)address, length};
        ssize_t got;
        do {
            got = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
        } while (got < 0 && errno == EINTR);
        if (got <= 0) { bad = 1; break; }
        for (size_t i = 0; i + width <= (size_t)got; i += width) {
            if (!bytes[i] && (width == 1 || !bytes[i + 1])) {
                errno = saved_errno;
                return 0;
            }
        }
        if ((size_t)got != length) { bad = 1; break; }
        address += length;
        maximum -= count;
    }
    errno = saved_errno;
    return bad;
}

int beer_is_bad_string_ptr_a(const char *string, size_t maximum)
{
    return bad_string_pointer(string, maximum, 1);
}

int beer_is_bad_string_ptr_w(const uint16_t *string, size_t maximum)
{
    return bad_string_pointer(string, maximum, 2);
}

typedef struct BeerAllocation {
    void *base;
    size_t size;
    uint32_t allocation_protect;
    uint32_t *page_protect; /* Requested guest protection; zero means reserved. */
} BeerAllocation;

/* Live VirtualAlloc regions, sorted by base and grown on demand: Windows bounds allocations only by address space
 * (an engine whose own arena fills falls back to one VirtualAlloc per object; Onimusha makes tens of thousands while
 * precompiling pipelines). The old fixed 16384-entry table failed every allocation past that and was scanned
 * linearly on each lookup. Entries move on insert/remove, so pointers into it are used only under the lock. */
static BeerAllocation *g_allocations;
static size_t g_allocation_count, g_allocation_capacity;
static pthread_mutex_t g_allocation_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Index of the first allocation whose base is above `value`. */
static size_t allocation_upper_bound_locked(uintptr_t value)
{
    size_t low = 0, high = g_allocation_count;
    while (low < high) {
        const size_t mid = low + (high - low) / 2;
        if ((uintptr_t)g_allocations[mid].base <= value) low = mid + 1; else high = mid;
    }
    return low;
}

static BeerAllocation *insert_allocation_locked(void *base)
{
    if (g_allocation_count == g_allocation_capacity) {
        const size_t capacity = g_allocation_capacity ? g_allocation_capacity * 2 : 1024;
        BeerAllocation *grown = realloc(g_allocations, capacity * sizeof(*grown));
        if (!grown) return NULL;
        g_allocations = grown;
        g_allocation_capacity = capacity;
    }
    const size_t at = allocation_upper_bound_locked((uintptr_t)base);
    memmove(&g_allocations[at + 1], &g_allocations[at], (g_allocation_count - at) * sizeof(*g_allocations));
    ++g_allocation_count;
    memset(&g_allocations[at], 0, sizeof(g_allocations[at]));
    return &g_allocations[at];
}

static void remove_allocation_locked(BeerAllocation *allocation)
{
    const size_t at = (size_t)(allocation - g_allocations);
    memmove(&g_allocations[at], &g_allocations[at + 1], (g_allocation_count - at - 1) * sizeof(*g_allocations));
    --g_allocation_count;
}

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
    /* Conventional Windows x86 page tables cannot prohibit data reads while
     * allowing execution. Linux PROT_EXEC alone can instead enable an implicit
     * execute-only protection key. Preserve the x86 guest's effective access,
     * independently of the host, and keep requested flags in page metadata. */
    case BEER_PAGE_EXECUTE: return PROT_EXEC | PROT_READ;
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
    const uintptr_t value = (uintptr_t)address;
    const size_t after = allocation_upper_bound_locked(value);
    if (!after) return NULL;
    BeerAllocation *candidate = &g_allocations[after - 1];
    return value - (uintptr_t)candidate->base < candidate->size ? candidate : NULL;
}

uint64_t g_beer_vm_live_bytes;   /* mapped by VirtualAlloc, not yet released */

static void *virtual_alloc_aligned(void *address, size_t size, uint32_t allocation_type,
                                   uint32_t protection, size_t alignment)
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
    if (size > SIZE_MAX - prefix) { errno = ENOMEM; return NULL; }
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
            mapped_size <= existing->size - (requested - (uintptr_t)existing->base);
        if (existing) {
            if (!inside || !(allocation_type & BEER_MEM_COMMIT) ||
                (allocation_type & BEER_MEM_RESERVE)) {
                pthread_mutex_unlock(&g_allocation_mutex);
                errno = EEXIST;
                return NULL;
            }
            if (mprotect((void *)requested, mapped_size, host_protection) != 0) {
                pthread_mutex_unlock(&g_allocation_mutex);
                return NULL;
            }
            size_t first = (requested - (uintptr_t)existing->base) / page;
            for (size_t i = 0; i < mapped_size / page; ++i)
                existing->page_protect[first + i] = protection;
            pthread_mutex_unlock(&g_allocation_mutex);
            return (void *)requested;
        }
        pthread_mutex_unlock(&g_allocation_mutex);
        /* A nonnull commit-only address must belong to an existing reservation;
         * mmap must not silently turn an invalid commit into a fresh reserve. */
        if (!(allocation_type & BEER_MEM_RESERVE)) { errno = EINVAL; return NULL; }
    }
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_FIXED_NOREPLACE
    if (address) flags |= MAP_FIXED_NOREPLACE;
#endif
    void *mapped;
    if (!address) {
        /* Windows reserves in 64 KiB allocation-granularity units and returns
         * 64 KiB-aligned bases; runtimes and allocators rely on that. */
        size_t granularity = 65536;
        if (alignment > granularity) granularity = alignment;
        size_t region = align_up_size(mapped_size, granularity);
        if (!region) { errno = ENOMEM; return NULL; }
        size_t span = region + granularity;
        if (span < region) { errno = ENOMEM; return NULL; }
        /* Bottom-up like Windows, unless the caller asked for MEM_TOP_DOWN. */
        void *low = (allocation_type & UINT32_C(0x00100000)) ? MAP_FAILED
                  : beer_vm_map_bottom_up(region, granularity, host_protection, 0);
        if (low != MAP_FAILED) {
            mapped = low;
            mapped_size = region;
            goto mapped_done;
        }
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
mapped_done:
    if (mapped == MAP_FAILED) return NULL;
#ifndef MAP_FIXED_NOREPLACE
    if (address && mapped != (void *)requested) {
        munmap(mapped, mapped_size);
        errno = EEXIST;
        return NULL;
    }
#endif
    uint32_t *page_protect = calloc(mapped_size / page, sizeof(*page_protect));
    if (!page_protect) { munmap(mapped, mapped_size); errno = ENOMEM; return NULL; }
    if (allocation_type & BEER_MEM_COMMIT)
        for (size_t i = 0; i < mapped_size / page; ++i) page_protect[i] = protection;
    pthread_mutex_lock(&g_allocation_mutex);
    BeerAllocation *slot = insert_allocation_locked(mapped);
    if (slot) {
        g_beer_vm_live_bytes += mapped_size;
        slot->base = mapped;
        slot->size = mapped_size;
        slot->allocation_protect = protection;
        slot->page_protect = page_protect;
    }
    pthread_mutex_unlock(&g_allocation_mutex);
    if (!slot) {
        free(page_protect);
        munmap(mapped, mapped_size);
        errno = ENOMEM;
        return NULL;
    }
    return mapped;
}

void *beer_virtual_alloc(void *address, size_t size, uint32_t allocation_type,
                         uint32_t protection)
{
    return virtual_alloc_aligned(address, size, allocation_type, protection, 0);
}

/* VirtualAlloc2 with an alignment requirement (power of two, 0 = allocation granularity). */
void *beer_virtual_alloc_aligned(size_t size, size_t alignment, uint32_t allocation_type,
                                 uint32_t protection)
{
    if (alignment & (alignment - 1)) { errno = EINVAL; return NULL; }
    return virtual_alloc_aligned(NULL, size, allocation_type, protection, alignment);
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
        free(allocation->page_protect);
        remove_allocation_locked(allocation);
        pthread_mutex_unlock(&g_allocation_mutex);
        beer_vm_unmap_bottom_up(base, allocation_size);
        return 1;
    }
    if (free_type == BEER_MEM_DECOMMIT && size) {
        size_t page = page_size();
        uintptr_t base = align_down((uintptr_t)address, page);
        size_t prefix = (uintptr_t)address - base;
        if (size > SIZE_MAX - prefix) return 0;
        size_t length = align_up_size(prefix + size, page);
        if (!length) return 0;
        pthread_mutex_lock(&g_allocation_mutex);
        BeerAllocation *allocation = find_allocation_locked((void *)base);
        if (!allocation || length > allocation->size - (base - (uintptr_t)allocation->base) ||
            mprotect((void *)base, length, PROT_NONE) != 0) {
            pthread_mutex_unlock(&g_allocation_mutex);
            return 0;
        }
        (void)madvise((void *)base, length, MADV_DONTNEED);
        size_t first = (base - (uintptr_t)allocation->base) / page;
        memset(allocation->page_protect + first, 0, length / page * sizeof(uint32_t));
        pthread_mutex_unlock(&g_allocation_mutex);
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
    /* VirtualQuery starts at the requested page, then scans FORWARD. Returning
     * an earlier VMA/allocation base gives incorrect sizes to region walkers. */
    uintptr_t query_start = align_down(value, page_size());
    info->base_address = query_start;
    info->region_size = end - query_start;
    info->state = BEER_MEM_COMMIT;
    info->protect = host_to_windows_protection(read, write, execute);
    info->type = shared ? BEER_MEM_MAPPED : BEER_MEM_PRIVATE;

    pthread_mutex_lock(&g_allocation_mutex);
    BeerAllocation *allocation = find_allocation_locked(address);
    if (allocation) {
        info->allocation_base = (uint64_t)(uintptr_t)allocation->base;
        info->allocation_protect = allocation->allocation_protect;
        info->type = BEER_MEM_PRIVATE;
        size_t page = page_size();
        size_t index = (value - (uintptr_t)allocation->base) / page;
        uint32_t protection = allocation->page_protect[index];
        size_t first = index, last = index + 1, count = allocation->size / page;
        while (last < count && allocation->page_protect[last] == protection) ++last;
        info->base_address = (uintptr_t)allocation->base + first * page;
        info->region_size = (last - first) * page;
        info->state = protection ? BEER_MEM_COMMIT : BEER_MEM_RESERVE;
        info->protect = protection ? protection : BEER_PAGE_NOACCESS;
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
    size_t prefix = (uintptr_t)address - base;
    if (size > SIZE_MAX - prefix) return 0;
    size_t length = align_up_size(prefix + size, page);
    if (!length) return 0;
    pthread_mutex_lock(&g_allocation_mutex);
    BeerAllocation *allocation = find_allocation_locked((void *)base);
    size_t first = allocation ? (base - (uintptr_t)allocation->base) / page : 0;
    if (allocation) {
        if (length > allocation->size - (base - (uintptr_t)allocation->base)) {
            pthread_mutex_unlock(&g_allocation_mutex);
            return 0;
        }
        for (size_t i = 0; i < length / page; ++i)
            if (!allocation->page_protect[first + i]) {
                pthread_mutex_unlock(&g_allocation_mutex);
                return 0;
            }
        info.protect = allocation->page_protect[first];
    }
    if (mprotect((void *)base, length, host_protection) != 0) {
        pthread_mutex_unlock(&g_allocation_mutex);
        return 0;
    }
    if (allocation)
        for (size_t i = 0; i < length / page; ++i)
            allocation->page_protect[first + i] = new_protection;
    pthread_mutex_unlock(&g_allocation_mutex);
    if (old_protection) *old_protection = info.protect;
    return 1;
}

void beer_virtual_memory_reset(void)
{
    pthread_mutex_lock(&g_allocation_mutex);
    for (size_t index = 0; index < g_allocation_count; ++index) {
        munmap(g_allocations[index].base, g_allocations[index].size);
        free(g_allocations[index].page_protect);
    }
    g_allocation_count = 0;
    g_beer_vm_live_bytes = 0;
    pthread_mutex_unlock(&g_allocation_mutex);
}
