#include "win32_crt_init.h"
#include "guest_cpu.h"

uint64_t __attribute__((ms_abi)) beer_crt_initterm(BeerCrtInitializer *first,
                                                  BeerCrtInitializer *last)
{
    if (!first || !last) return 0;
    for (; first < last; ++first)
        if (*first) BEER_GUEST_CALL(*first);
    return 0;
}

int __attribute__((ms_abi)) beer_crt_initterm_e(BeerCrtErrorInitializer *first,
                                               BeerCrtErrorInitializer *last)
{
    if (!first || !last) return 0;
    for (; first < last; ++first) {
        if (!*first) continue;
        int result = BEER_GUEST_CALL(*first);
        if (result) return result;
    }
    return 0;
}