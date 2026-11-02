#ifndef BEER_WIN32_VIRTUAL_MEMORY_H
#define BEER_WIN32_VIRTUAL_MEMORY_H

#include <stddef.h>
#include <stdint.h>

#define BEER_MEM_COMMIT      UINT32_C(0x00001000)
#define BEER_MEM_RESERVE     UINT32_C(0x00002000)
#define BEER_MEM_DECOMMIT    UINT32_C(0x00004000)
#define BEER_MEM_RELEASE     UINT32_C(0x00008000)
#define BEER_MEM_FREE        UINT32_C(0x00010000)
#define BEER_MEM_PRIVATE     UINT32_C(0x00020000)
#define BEER_MEM_MAPPED      UINT32_C(0x00040000)

#define BEER_PAGE_NOACCESS          UINT32_C(0x01)
#define BEER_PAGE_READONLY          UINT32_C(0x02)
#define BEER_PAGE_READWRITE         UINT32_C(0x04)
#define BEER_PAGE_EXECUTE           UINT32_C(0x10)
#define BEER_PAGE_EXECUTE_READ      UINT32_C(0x20)
#define BEER_PAGE_EXECUTE_READWRITE UINT32_C(0x40)

/* Windows AMD64 MEMORY_BASIC_INFORMATION layout (48 bytes). */
typedef struct BeerMemoryBasicInformation {
    uint64_t base_address;
    uint64_t allocation_base;
    uint32_t allocation_protect;
    uint32_t alignment1;
    uint64_t region_size;
    uint32_t state;
    uint32_t protect;
    uint32_t type;
    uint32_t alignment2;
} BeerMemoryBasicInformation;

void *beer_virtual_alloc(void *address, size_t size, uint32_t allocation_type,
                         uint32_t protection);
void *beer_virtual_alloc_aligned(size_t size, size_t alignment, uint32_t allocation_type,
                                 uint32_t protection);
int beer_virtual_free(void *address, size_t size, uint32_t free_type);
int beer_virtual_protect(void *address, size_t size, uint32_t new_protection,
                         uint32_t *old_protection);
/* BaseAddress is the requested address rounded DOWN to a page. RegionSize
 * covers only subsequent pages sharing state/protection/allocation identity;
 * AllocationBase alone identifies the beginning of the whole allocation. */
size_t beer_virtual_query(const void *address, BeerMemoryBasicInformation *info,
                          size_t info_size);
int beer_is_bad_string_ptr_a(const char *string, size_t maximum);
int beer_is_bad_string_ptr_w(const uint16_t *string, size_t maximum);
void beer_virtual_memory_reset(void);

/* Windows-style bottom-up placement for guest memory: the lowest free range in [64 KiB, 4 GiB), aligned to
 * `alignment` (at least the 64 KiB allocation granularity), mapped with MAP_FIXED_NOREPLACE so host mappings are
 * never replaced. Returns MAP_FAILED when the low range is full (callers then place memory as before).
 * beer_vm_unmap_bottom_up releases a range obtained here (or ignores any other range after unmapping it). */
void *beer_vm_map_bottom_up(size_t size, size_t alignment, int protection, int extra_flags);
void beer_vm_unmap_bottom_up(void *base, size_t size);
int beer_vm_bottom_up_enabled(void);

#endif
