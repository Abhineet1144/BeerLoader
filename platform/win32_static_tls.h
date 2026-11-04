#ifndef BEER_WIN32_STATIC_TLS_H
#define BEER_WIN32_STATIC_TLS_H

#include <stddef.h>
#include <stdint.h>

/* Static TLS indexes as the Windows loader assigns them: slot zero belongs to the executable, and each module
 * that has a TLS directory takes the next free index when its TLS is set up (load order). Modules without TLS
 * take none, so the first DLL with TLS gets index 1 whatever its place in the module list; code that reads
 * ThreadLocalStoragePointer[_tls_index] relies on that numbering. Returns UINT32_MAX when the table is full. */
static inline uint32_t beer_static_tls_next_slot(uint32_t *next, size_t slots)
{
    if (!next || slots <= 1) return UINT32_MAX;
    if (*next == 0) *next = 1;
    if (*next >= slots) return UINT32_MAX;
    return (*next)++;
}

#endif
