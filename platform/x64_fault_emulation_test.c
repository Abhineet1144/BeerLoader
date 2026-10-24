#include "x64_fault_emulation.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#define X86_CF UINT64_C(0x001)
#define X86_PF UINT64_C(0x004)
#define X86_AF UINT64_C(0x010)
#define X86_ZF UINT64_C(0x040)
#define X86_SF UINT64_C(0x080)
#define X86_OF UINT64_C(0x800)

int main(void)
{
    static const uint8_t sbb_r11d_r9[] = {0x45, 0x1b, 0x19};
    static const uint8_t other[] = {0x45, 0x1b, 0x18};
    BeerX64FaultResult result;

    assert(beer_x64_emulate_sbb_r11d(sbb_r11d_r9, 3,
                                      UINT64_C(0xaaaaaaaa00000001),
                                      0, X86_CF, &result));
    assert(result.instruction_length == 3);
    assert(result.register_value == 0);
    assert((result.rflags & (X86_CF | X86_ZF | X86_PF)) ==
           (X86_ZF | X86_PF));

    assert(beer_x64_emulate_sbb_r11d(sbb_r11d_r9, 3, 0, 0, X86_CF,
                                      &result));
    assert(result.register_value == UINT32_MAX);
    assert((result.rflags & (X86_CF | X86_SF | X86_PF | X86_AF)) ==
           (X86_CF | X86_SF | X86_PF | X86_AF));

    assert(beer_x64_emulate_sbb_r11d(sbb_r11d_r9, 3,
                                      UINT32_C(0x80000000), 0, X86_CF,
                                      &result));
    assert(result.register_value == UINT32_C(0x7fffffff));
    assert((result.rflags & X86_OF) != 0);

    assert(beer_x64_emulate_sbb_r11d(sbb_r11d_r9, 3, 5, 3, 0, &result));
    assert(result.register_value == 2);

    assert(!beer_x64_emulate_sbb_r11d(other, 3, 1, 0, 0, &result));
    assert(!beer_x64_emulate_sbb_r11d(sbb_r11d_r9, 2, 1, 0, 0, &result));

    puts("x64 fault-emulation tests passed");
    return 0;
}
