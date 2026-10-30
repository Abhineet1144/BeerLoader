#include "win32_shared_data.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t read_u32(const uint8_t *data, size_t offset)
{
    uint32_t value;
    memcpy(&value, data + offset, sizeof(value));
    return value;
}

int main(void)
{
    uint8_t memory[BEER_WIN32_SHARED_DATA_SIZE];
    beer_win32_shared_data_initialize(memory, sizeof(memory));

    assert(read_u32(memory, 0x004) == (1u << 24));
    assert(read_u32(memory, 0x260) == 19045);
    assert(read_u32(memory, 0x264) == 1);
    assert(read_u32(memory, 0x26c) == 10);
    assert(read_u32(memory, 0x244) == 0x200000u);
    assert(read_u32(memory, 0x2e8) == 0); /* ActiveConsoleId */
    assert(read_u32(memory, 0x2f0) == 0); /* ComPlusPackage */
    assert(read_u32(memory, 0x2f8) > 0);  /* NumberOfPhysicalPages */
    assert(read_u32(memory, 0x300) == 0); /* SharedDataFlags */
    assert(read_u32(memory, 0x310) == 10000000u); /* QpcFrequency low */
    assert(read_u32(memory, 0x314) == 0);         /* QpcFrequency high */
    assert(memory[0x268] == 1);
    assert(memory[0x030] == 'C' && memory[0x031] == 0);
    assert(memory[0x378] == 0); /* Direct reads anywhere in the ABI page are safe. */

    /* Direct readers use KUSER_SHARED_DATA as a live clock, not a startup
     * snapshot. Verify both KSYSTEM_TIME triples and TickCount advance. */
    beer_win32_shared_data_refresh(memory, sizeof(memory),
                                   0x123456789abcdef0ull,
                                   0x0000000200003000ull);
    assert(read_u32(memory, 0x008) == 0x00003000u);
    assert(read_u32(memory, 0x00c) == 0x00000002u);
    assert(read_u32(memory, 0x010) == 0x00000002u);
    assert(read_u32(memory, 0x014) == 0x9abcdef0u);
    assert(read_u32(memory, 0x018) == 0x12345678u);
    assert(read_u32(memory, 0x01c) == 0x12345678u);
    assert(read_u32(memory, 0x2f4) == 0x000d1b72u);
    assert(read_u32(memory, 0x320) == 0x000d1b72u);

    puts("Win32 shared-data tests passed");
    return 0;
}
