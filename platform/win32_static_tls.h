#ifndef BEER_WIN32_STATIC_TLS_H
#define BEER_WIN32_STATIC_TLS_H

#include <stddef.h>
#include <stdint.h>

/* Slot zero belongs to the executable. Published module ordinals never change
 * when recursive imports append more records to the loader's module table. */
static inline uint32_t beer_static_tls_slot(size_t module_ordinal, size_t slots)
{
    if (slots <= 1 || module_ordinal >= slots - 1 || module_ordinal >= UINT32_MAX)
        return UINT32_MAX;
    return (uint32_t)module_ordinal + 1;
}

#endif