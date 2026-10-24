#include "win32_environment.h"

#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static int environment_block_units(char *const environment[], size_t *units_out)
{
    size_t units = 1; /* extra terminator after the final entry */

    for (char *const *entry = environment; entry && *entry; ++entry) {
        size_t length = strlen(*entry);
        if (length > SIZE_MAX - units - 1)
            return 0;
        units += length + 1;
    }

    /* An empty Windows environment is represented by two terminators. */
    if (units == 1)
        units = 2;
    *units_out = units;
    return 1;
}

char *beer_win32_environment_block_a(char *const environment[])
{
    size_t units;
    if (!environment_block_units(environment, &units))
        return NULL;

    char *block = calloc(units, sizeof(*block));
    if (!block)
        return NULL;

    char *out = block;
    for (char *const *entry = environment; entry && *entry; ++entry) {
        size_t length = strlen(*entry);
        memcpy(out, *entry, length);
        out += length + 1;
    }
    return block;
}

uint16_t *beer_win32_environment_block_w(char *const environment[])
{
    size_t units;
    if (!environment_block_units(environment, &units) ||
        units > SIZE_MAX / sizeof(uint16_t))
        return NULL;

    uint16_t *block = calloc(units, sizeof(*block));
    if (!block)
        return NULL;

    uint16_t *out = block;
    for (char *const *entry = environment; entry && *entry; ++entry) {
        const unsigned char *src = (const unsigned char *)*entry;
        while (*src)
            *out++ = *src < 0x80 ? *src++ : (src++, (uint16_t)'?');
        ++out;
    }
    return block;
}

void beer_win32_environment_block_free(void *block)
{
    free(block);
}
