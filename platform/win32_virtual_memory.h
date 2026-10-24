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
int beer_virtual_free(void *address, size_t size, uint32_t free_type);
int beer_virtual_protect(void *address, size_t size, uint32_t new_protection,
                         uint32_t *old_protection);
size_t beer_virtual_query(const void *address, BeerMemoryBasicInformation *info,
                          size_t info_size);
void beer_virtual_memory_reset(void);

#endif
