#include "win32_version.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    uint8_t *block = NULL;
    uint32_t size = 0;
    if (!beer_system_version_resource(&block, &size) || !block || size < 64) {
        fprintf(stderr, "failed to build system version resource\n");
        return 1;
    }

    static const uint16_t root[] = {'\\', 0};
    const void *value = NULL;
    uint32_t length = 0;
    if (!beer_version_query(block, size, root, &value, &length) ||
        !value || length != 13 * sizeof(uint32_t)) {
        fprintf(stderr, "failed to query VS_FIXEDFILEINFO\n");
        free(block);
        return 1;
    }

    const uint32_t *fixed = value;
    const int ok = fixed[0] == 0xFEEF04BDu && fixed[1] == 0x00010000u &&
                   fixed[2] == 0x000A0000u && fixed[3] == 0x4A650000u &&
                   fixed[4] == 0x000A0000u && fixed[5] == 0x4A650000u;
    free(block);
    if (!ok) {
        fprintf(stderr, "unexpected synthetic Windows version metadata\n");
        return 1;
    }
    puts("win32 version contract test passed");
    return 0;
}
