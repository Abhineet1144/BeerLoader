#include "win32_virtual_memory.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static void test_string_pointer_probes(void)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    char *memory = mmap(NULL, page * 2, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(memory != MAP_FAILED);
    memset(memory, 'x', page);
    assert(mprotect(memory + page, page, PROT_NONE) == 0);

    assert(!beer_is_bad_string_ptr_a(NULL, 0));
    assert(!beer_is_bad_string_ptr_w(NULL, 0));
    assert(beer_is_bad_string_ptr_a(NULL, 1));
    assert(beer_is_bad_string_ptr_w(NULL, 1));
    assert(beer_is_bad_string_ptr_a(memory + page, 1));
    assert(beer_is_bad_string_ptr_w((uint16_t *)(memory + page), 1));
    assert(!beer_is_bad_string_ptr_a(memory, page));
    assert(beer_is_bad_string_ptr_a(memory, page + 1));
    assert(!beer_is_bad_string_ptr_w((uint16_t *)memory, page / 2));
    assert(beer_is_bad_string_ptr_w((uint16_t *)memory, page / 2 + 1));

    /* A terminator before an unreadable page must win over the short read. */
    memory[page - 1] = 0;
    assert(!beer_is_bad_string_ptr_a(memory + page - 3, SIZE_MAX));
    assert(!beer_is_bad_string_ptr_a(memory, SIZE_MAX));
    assert(beer_is_bad_string_ptr_w((uint16_t *)(memory + page - 2), 2));
    memory[page - 2] = 0;
    assert(!beer_is_bad_string_ptr_w((uint16_t *)(memory + page - 4), SIZE_MAX));
    assert(!beer_is_bad_string_ptr_w((uint16_t *)memory, SIZE_MAX));

    /* UTF-16 counts code units, including unaligned units across page edges. */
    memory[page - 2] = 'x';
    memory[page - 1] = 'x';
    assert(beer_is_bad_string_ptr_w((uint16_t *)(memory + page - 1), 1));
    assert(mprotect(memory + page, page, PROT_READ | PROT_WRITE) == 0);
    memory[page - 1] = memory[page] = 0;
    assert(!beer_is_bad_string_ptr_w((uint16_t *)(memory + page - 1), 1));
    assert(mprotect(memory, page, PROT_READ) == 0);
    errno = EDOM;
    assert(!beer_is_bad_string_ptr_a(memory + page - 1, 1));
    assert(errno == EDOM);
    assert(beer_is_bad_string_ptr_a((const char *)(UINTPTR_MAX - 1), SIZE_MAX));
    assert(beer_is_bad_string_ptr_w((const uint16_t *)(UINTPTR_MAX - 1), SIZE_MAX));
    assert(errno == EDOM);
    assert(munmap(memory, page * 2) == 0);
    assert(beer_is_bad_string_ptr_a(memory, 1));
}

static void test_allocate_query_protect_release(void)
{
    void *memory = beer_virtual_alloc(NULL, 6000, BEER_MEM_RESERVE | BEER_MEM_COMMIT,
                                      BEER_PAGE_READWRITE);
    assert(memory);
    BeerMemoryBasicInformation info;
    assert(beer_virtual_query(memory, &info, sizeof(info)) == sizeof(info));
    assert(info.base_address == (uint64_t)(uintptr_t)memory);
    assert(info.allocation_base == (uint64_t)(uintptr_t)memory);
    assert(info.region_size >= 6000);
    assert(info.state == BEER_MEM_COMMIT);
    assert(info.protect == BEER_PAGE_READWRITE);
    assert(info.type == BEER_MEM_PRIVATE);

    uint32_t old = 0;
    assert(beer_virtual_protect(memory, 4096, BEER_PAGE_READONLY, &old));
    assert(old == BEER_PAGE_READWRITE);
    assert(beer_virtual_query(memory, &info, sizeof(info)) == sizeof(info));
    assert(info.protect == BEER_PAGE_READONLY);
    assert(beer_virtual_free(memory, 0, BEER_MEM_RELEASE));
    assert(beer_virtual_query(memory, &info, sizeof(info)) == sizeof(info));
    assert(info.state == BEER_MEM_FREE);
}

static void test_reserve_and_validation(void)
{
    void *memory = beer_virtual_alloc(NULL, 4096, BEER_MEM_RESERVE,
                                      BEER_PAGE_READWRITE);
    assert(memory);
    BeerMemoryBasicInformation info;
    assert(beer_virtual_query(memory, &info, sizeof(info)) == sizeof(info));
    assert(info.protect == BEER_PAGE_NOACCESS);
    assert(!beer_virtual_free(memory, 4096, BEER_MEM_RELEASE));
    assert(beer_virtual_free(memory, 0, BEER_MEM_RELEASE));
    assert(!beer_virtual_query(memory, &info, sizeof(info) - 1));
}

static void test_forward_query_and_commit_boundaries(void)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    unsigned char *memory = beer_virtual_alloc(NULL, page * 4,
        BEER_MEM_RESERVE | BEER_MEM_COMMIT, BEER_PAGE_READWRITE);
    assert(memory);
    BeerMemoryBasicInformation info, allocation;
    assert(beer_virtual_query(memory, &allocation, sizeof(allocation)));
    for (size_t i = 0; i < 4; ++i) {
        assert(beer_virtual_query(memory + i * page + 19, &info, sizeof(info)));
        assert(info.base_address == (uintptr_t)memory + i * page);
        assert(info.region_size == allocation.region_size - i * page);
        assert(info.allocation_base == (uintptr_t)memory);
    }
    uint32_t old;
    assert(beer_virtual_protect(memory + 3 * page, page, BEER_PAGE_READONLY, &old));
    assert(beer_virtual_query(memory + page + 19, &info, sizeof(info)));
    assert(info.base_address == (uintptr_t)memory + page && info.region_size == 2 * page);
    assert(beer_virtual_free(memory + page, 2 * page, BEER_MEM_DECOMMIT));
    assert(beer_virtual_query(memory + 2 * page + 19, &info, sizeof(info)));
    assert(info.base_address == (uintptr_t)memory + 2 * page && info.region_size == page);
    assert(info.state == BEER_MEM_RESERVE);
    assert(beer_virtual_alloc(memory + page + 19, page, BEER_MEM_COMMIT,
        BEER_PAGE_READWRITE) == memory + page);
    memory[page + 19] = 0xA5;
    assert(beer_virtual_alloc(memory + page + 19, 1, BEER_MEM_COMMIT,
        BEER_PAGE_READWRITE) == memory + page);
    assert(memory[page + 19] == 0xA5); /* Recommit preserves existing data. */
    assert(!beer_virtual_alloc(memory, page, BEER_MEM_RESERVE | BEER_MEM_COMMIT,
        BEER_PAGE_READWRITE));
    assert(beer_virtual_free(memory, 0, BEER_MEM_RELEASE));
    assert(!beer_virtual_alloc(memory, page, BEER_MEM_COMMIT, BEER_PAGE_READWRITE));

    /* Apply the same forward-query contract to host-backed mappings outside
     * Beer's VirtualAlloc registry (e.g. PE image mappings). */
    memory = mmap(NULL, page * 4, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(memory != MAP_FAILED);
    assert(mprotect(memory + 3 * page, page, PROT_NONE) == 0);
    assert(beer_virtual_query(memory + page + 19, &info, sizeof(info)));
    assert(info.base_address == (uintptr_t)memory + page && info.region_size == 2 * page);
    assert(munmap(memory, page * 4) == 0);
}

int main(void)
{
    test_forward_query_and_commit_boundaries();
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    unsigned char *memory = beer_virtual_alloc(NULL, page * 2,
        BEER_MEM_RESERVE | BEER_MEM_COMMIT, BEER_PAGE_READWRITE);
    assert(memory);
    memory[0] = 0xA5;
    uint32_t old = 0;
    BeerMemoryBasicInformation info;
    assert(beer_virtual_protect(memory, page, BEER_PAGE_EXECUTE, &old));
    assert(old == BEER_PAGE_READWRITE);
    assert(*(volatile unsigned char *)memory == 0xA5);
    assert(beer_virtual_query(memory, &info, sizeof(info)));
    assert(info.protect == BEER_PAGE_EXECUTE && info.region_size == page);
    assert(info.allocation_protect == BEER_PAGE_READWRITE);
    assert(beer_virtual_query(memory + page, &info, sizeof(info)));
    assert(info.protect == BEER_PAGE_READWRITE);
    assert(beer_virtual_protect(memory, page, BEER_PAGE_EXECUTE_READ, &old));
    assert(old == BEER_PAGE_EXECUTE);
    assert(beer_virtual_query(memory, &info, sizeof(info)));
    assert(info.protect == BEER_PAGE_EXECUTE_READ);
    assert(!beer_virtual_protect(memory, SIZE_MAX, BEER_PAGE_READWRITE, &old));
    assert(beer_virtual_free(memory, page, BEER_MEM_DECOMMIT));
    assert(beer_virtual_query(memory, &info, sizeof(info)));
    assert(info.state == BEER_MEM_RESERVE && info.protect == BEER_PAGE_NOACCESS);
    assert(!beer_virtual_protect(memory, page * 2, BEER_PAGE_READWRITE, &old));
    assert(beer_virtual_alloc(memory, page, BEER_MEM_COMMIT, BEER_PAGE_EXECUTE) == memory);
    assert(*(volatile unsigned char *)memory == 0);
    assert(beer_virtual_query(memory, &info, sizeof(info)));
    assert(info.state == BEER_MEM_COMMIT && info.protect == BEER_PAGE_EXECUTE);
    assert(beer_virtual_free(memory, 0, BEER_MEM_RELEASE));
    test_allocate_query_protect_release();
    test_reserve_and_validation();
    test_string_pointer_probes();
    beer_virtual_memory_reset();
    puts("Win32 virtual-memory tests passed");
    return 0;
}
