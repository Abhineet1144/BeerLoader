#include "x64_fault_emulation.h"

#define X86_CF UINT64_C(0x001)
#define X86_PF UINT64_C(0x004)
#define X86_AF UINT64_C(0x010)
#define X86_ZF UINT64_C(0x040)
#define X86_SF UINT64_C(0x080)
#define X86_OF UINT64_C(0x800)
#define X86_ARITHMETIC_FLAGS (X86_CF | X86_PF | X86_AF | X86_ZF | X86_SF | X86_OF)

static int even_parity8(uint8_t value)
{
    value ^= (uint8_t)(value >> 4);
    value &= 0x0f;
    return ((UINT16_C(0x9669) >> value) & 1u) != 0;
}

int beer_x64_emulate_sbb_r11d(const uint8_t *instruction,
                               size_t available,
                               uint64_t r11,
                               uint32_t memory_value,
                               uint64_t rflags,
                               BeerX64FaultResult *result)
{
    if (!instruction || !result || available < 3 ||
        instruction[0] != 0x45 || instruction[1] != 0x1b ||
        instruction[2] != 0x19)
        return 0;

    uint32_t lhs = (uint32_t)r11;
    uint32_t borrow = (rflags & X86_CF) ? 1u : 0u;
    uint64_t subtrahend = (uint64_t)memory_value + borrow;
    uint32_t rhs = (uint32_t)subtrahend;
    uint32_t value = lhs - rhs;
    uint64_t flags = rflags & ~X86_ARITHMETIC_FLAGS;

    if ((uint64_t)lhs < subtrahend) flags |= X86_CF;
    if (even_parity8((uint8_t)value)) flags |= X86_PF;
    if (((lhs ^ rhs ^ value) & 0x10u) != 0) flags |= X86_AF;
    if (value == 0) flags |= X86_ZF;
    if ((value & UINT32_C(0x80000000)) != 0) flags |= X86_SF;
    if (((lhs ^ rhs) & (lhs ^ value) & UINT32_C(0x80000000)) != 0)
        flags |= X86_OF;

    /* A 32-bit destination write zero-extends into the full register. */
    result->register_value = value;
    result->rflags = flags;
    result->instruction_length = 3;
    return 1;
}
