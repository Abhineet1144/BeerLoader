#include "win32_environment.h"

#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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

const char *beer_win32_environment_get(const char *name, char *const environment[])
{
    if (!name || !*name || strchr(name, '='))
        return NULL;
    size_t name_length = strlen(name);
    for (char *const *entry = environment; entry && *entry; ++entry) {
        const char *equals = strchr(*entry, '=');
        if (!equals || (size_t)(equals - *entry) != name_length)
            continue;
        if (!strncasecmp(*entry, name, name_length))
            return equals + 1;
    }
    return NULL;
}

int beer_win32_environment_set(const char *name, const char *value)
{
    if (!name || !*name || strchr(name, '='))
        return 0;
    return value ? setenv(name, value, 1) == 0 : unsetenv(name) == 0;
}

static int expanded_size_add(size_t *size, size_t amount)
{
    if (amount > SIZE_MAX - *size)
        return 0;
    *size += amount;
    return 1;
}

size_t beer_win32_environment_expand_a(const char *source, char *destination,
                                       size_t capacity, char *const environment[])
{
    if (!source)
        return 0;

    if (destination) {
        size_t required = beer_win32_environment_expand_a(source, NULL, 0, environment);
        if (!required || capacity < required)
            return required;
    }

    size_t required = 1;
    char *out = destination;
    for (const char *cursor = source; *cursor;) {
        const char *start = strchr(cursor, '%');
        size_t literal_length = start ? (size_t)(start - cursor) : strlen(cursor);
        if (!expanded_size_add(&required, literal_length)) return 0;
        if (out && capacity >= required)
            memcpy(out, cursor, literal_length);
        if (out) out += literal_length;
        if (!start) break;

        const char *end = strchr(start + 1, '%');
        if (!end) {
            size_t tail_length = strlen(start);
            if (!expanded_size_add(&required, tail_length)) return 0;
            if (out && capacity >= required)
                memcpy(out, start, tail_length);
            if (out) out += tail_length;
            break;
        }

        size_t name_length = (size_t)(end - start - 1);
        const char *value = NULL;
        char *name = NULL;
        if (name_length) {
            name = malloc(name_length + 1);
            if (!name) return 0;
            memcpy(name, start + 1, name_length);
            name[name_length] = 0;
            value = beer_win32_environment_get(name, environment);
            free(name);
        }
        size_t replacement_length = value ? strlen(value) : (size_t)(end - start + 1);
        if (!expanded_size_add(&required, replacement_length)) return 0;
        if (out && capacity >= required)
            memcpy(out, value ? value : start, replacement_length);
        if (out) out += replacement_length;
        cursor = end + 1;
    }

    if (destination && capacity >= required)
        *out = 0;
    return required;
}

size_t beer_win32_environment_expand_w(const uint16_t *source, uint16_t *destination,
                                       size_t capacity, char *const environment[])
{
    if (!source)
        return 0;

    size_t source_length = 0;
    while (source[source_length]) ++source_length;
    char *narrow_source = malloc(source_length + 1);
    if (!narrow_source) return 0;
    for (size_t i = 0; i <= source_length; ++i)
        narrow_source[i] = source[i] <= 0x7f ? (char)source[i] : '?';

    size_t required = beer_win32_environment_expand_a(narrow_source, NULL, 0, environment);
    if (!required || !destination || capacity < required) {
        free(narrow_source);
        return required;
    }

    char *narrow_result = malloc(required);
    if (!narrow_result) {
        free(narrow_source);
        return 0;
    }
    beer_win32_environment_expand_a(narrow_source, narrow_result, required, environment);
    for (size_t i = 0; i < required; ++i)
        destination[i] = (unsigned char)narrow_result[i];
    free(narrow_result);
    free(narrow_source);
    return required;
}
