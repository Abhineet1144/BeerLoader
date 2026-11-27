#ifndef BD11_DEVICE_CACHE_H
#define BD11_DEVICE_CACHE_H
#include "bd11.h"
#include <stdlib.h>

/* Process-owned cache. Constructor injection is local to the cache operation,
 * not a runtime backend override; CPU tests exercise this exact publication path. */
typedef struct B11DeviceCacheEntry {
    B11Device *dev;
    struct B11DeviceCacheEntry *next;
} B11DeviceCacheEntry;
typedef struct {
    pthread_mutex_t lock;
    B11DeviceCacheEntry *head;
} B11DeviceCache;
#define B11_DEVICE_CACHE_INIT { PTHREAD_MUTEX_INITIALIZER, NULL }

static inline B11HR b11_device_validate_creation_flags(uint32_t flags)
{
    if (flags & 0x2u) return B11_DXGI_ERROR_SDK_COMPONENT_MISSING;
    return flags & ~B11_CREATE_SUPPORTED_FLAGS ? B11_E_INVALIDARG : B11_S_OK;
}

static inline B11HR b11_device_cache_get(B11DeviceCache *cache, uint32_t flags,
    uint32_t level, B11Device *(*construct)(uint32_t, uint32_t),
    void **device, void **context)
{
    if (device) *device = NULL;
    if (context) *context = NULL;
    /* No debug layer, video/protection/debuggable/software capability implied. */
    B11HR hr = b11_device_validate_creation_flags(flags);
    if (hr != B11_S_OK) return hr;
    pthread_mutex_lock(&cache->lock);
    B11DeviceCacheEntry *entry = cache->head;
    while (entry && (entry->dev->flags != flags || entry->dev->feature_level != level))
        entry = entry->next;
    if (!entry) {
        entry = calloc(1, sizeof(*entry));
        if (!entry) { pthread_mutex_unlock(&cache->lock); return B11_E_OUTOFMEMORY; }
        entry->dev = construct(flags, level);
        if (!entry->dev) {
            free(entry);
            pthread_mutex_unlock(&cache->lock);
            return B11_E_FAIL;
        }
        /* Constructor returns a complete device with a process-owned immediate
         * context and configuration already installed. Never mutate on reuse. */
        entry->next = cache->head;
        cache->head = entry;
    }
    if (device) { b11_add_ref(&entry->dev->obj); *device = entry->dev; }
    if (context) {
        b11_add_ref(&entry->dev->immediate->obj);
        *context = entry->dev->immediate;
    }
    pthread_mutex_unlock(&cache->lock);
    return B11_S_OK;
}
#endif