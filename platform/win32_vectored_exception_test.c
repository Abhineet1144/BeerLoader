#include "win32_vectored_exception.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static int calls[8];
static int call_count;

static int32_t __attribute__((ms_abi)) tail_one(void *p)
{
    assert(p == (void *)(uintptr_t)0x1234);
    calls[call_count++] = 1;
    return BEER_EXCEPTION_CONTINUE_SEARCH;
}

static int32_t __attribute__((ms_abi)) tail_two(void *p)
{
    (void)p;
    calls[call_count++] = 2;
    return BEER_EXCEPTION_CONTINUE_SEARCH;
}

static int32_t __attribute__((ms_abi)) first_handler(void *p)
{
    (void)p;
    calls[call_count++] = 3;
    return BEER_EXCEPTION_CONTINUE_EXECUTION;
}

int main(void)
{
    beer_veh_reset();
    assert(!beer_veh_add(0, NULL));
    void *one = beer_veh_add(0, tail_one);
    void *two = beer_veh_add(0, tail_two);
    void *first = beer_veh_add(1, first_handler);
    assert(one && two && first);

    assert(beer_veh_dispatch((void *)(uintptr_t)0x1234));
    assert(call_count == 1 && calls[0] == 3);

    assert(beer_veh_remove(first));
    assert(!beer_veh_remove(first));
    call_count = 0;
    assert(!beer_veh_dispatch((void *)(uintptr_t)0x1234));
    assert(call_count == 2 && calls[0] == 1 && calls[1] == 2);

    assert(beer_veh_remove(one));
    assert(beer_veh_remove(two));
    puts("win32 vectored exception tests passed");
    return 0;
}
