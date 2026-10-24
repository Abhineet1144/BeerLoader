#ifndef BEER_X64_FAULT_EMULATION_H
#define BEER_X64_FAULT_EMULATION_H

#include <stddef.h>
#include <stdint.h>

typedef struct BeerX64FaultResult {
    uint64_t register_value;
    uint64_t rflags;
    size_t instruction_length;
} BeerX64FaultResult;

/* Emulate the packed-code form `sbb r11d, dword ptr [r9]` when the memory
 * operand faulted and compatibility recovery supplies a zero value. */
int beer_x64_emulate_sbb_r11d(const uint8_t *instruction,
                               size_t available,
                               uint64_t r11,
                               uint32_t memory_value,
                               uint64_t rflags,
                               BeerX64FaultResult *result);

#endif
