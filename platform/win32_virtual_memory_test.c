#include "win32_virtual_memory.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

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

int main(void)
{
    test_allocate_query_protect_release();
    test_reserve_and_validation();
    beer_virtual_memory_reset();
    puts("Win32 virtual-memory tests passed");
    return 0;
}
