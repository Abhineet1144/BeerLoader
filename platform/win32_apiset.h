#ifndef BEER_WIN32_APISET_H
#define BEER_WIN32_APISET_H

#include <stddef.h>

typedef struct BeerApiSetMapping {
    const char *contract;
    const char *host;
} BeerApiSetMapping;

/* Build a Windows 10 API-set namespace (schema version 6). Contract names are
 * normalized to lower case without a trailing .dll. The returned mapping is
 * process-lifetime storage owned by the caller and may be released with free(). */
void *beer_win32_apiset_build(const BeerApiSetMapping *mappings, size_t count,
                              size_t *namespace_size);

/* Structural verifier used by focused tests and by the loader before exposing
 * the namespace through PEB->ApiSetMap. */
int beer_win32_apiset_validate(const void *namespace_map, size_t namespace_size);

#endif
