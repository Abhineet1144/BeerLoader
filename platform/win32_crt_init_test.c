#include "win32_crt_init.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>

unsigned beer_test_initializer_calls;
extern void __attribute__((ms_abi)) beer_test_initializer(void);

/* A Win64 initializer may use all four home slots. This is intentionally an
 * assembly callback so optimizing a C callback cannot hide a missing home area. */
__asm__(
    ".pushsection .text\n"
    ".global beer_test_initializer\n"
    "beer_test_initializer:\n"
    "    movaps %xmm0, 8(%rsp)\n"
    "    movq $0, 24(%rsp)\n"
    "    movq $0, 32(%rsp)\n"
    "    incl beer_test_initializer_calls(%rip)\n"
    "    ret\n"
    ".popsection\n"
);

static unsigned sequence[8], length;
static void __attribute__((ms_abi)) first(void) { sequence[length++] = 1; }
static void __attribute__((ms_abi)) second(void) { sequence[length++] = 2; }
static int __attribute__((ms_abi)) success(void) { sequence[length++] = 3; return 0; }
static int __attribute__((ms_abi)) failure(void) { sequence[length++] = 4; return -7; }
static int __attribute__((ms_abi)) never(void) { sequence[length++] = 5; return 99; }

int main(void)
{
    BeerCrtInitializer constructors[] = {
        NULL, first, beer_test_initializer, NULL, second, beer_test_initializer
    };
    assert(!beer_crt_initterm(NULL, NULL));
    assert(!beer_crt_initterm(constructors, constructors));
    assert(!beer_crt_initterm(constructors, constructors + 6));
    assert(length == 2 && sequence[0] == 1 && sequence[1] == 2);
    assert(beer_test_initializer_calls == 2);
    BeerCrtErrorInitializer initializers[] = { NULL, success, failure, never };
    assert(!beer_crt_initterm_e(NULL, NULL));
    assert(!beer_crt_initterm_e(initializers, initializers));
    assert(beer_crt_initterm_e(initializers, initializers + 4) == -7);
    assert(length == 4 && sequence[2] == 3 && sequence[3] == 4);
    assert(!beer_crt_initterm_e(initializers, initializers + 2));
    assert(length == 5 && sequence[4] == 3);
    puts("Win64 CRT initializer ABI and ordering tests passed");
    return 0;
}