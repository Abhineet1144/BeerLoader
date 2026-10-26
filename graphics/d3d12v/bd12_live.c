#define _GNU_SOURCE
#include "bd12.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Registry of live BD12Buffer / BD12Image objects. Descriptors keep raw pointers to the resource behind them, and D3D12
 * lets an application destroy a resource while stale descriptors still sit in a heap. Ray tracing binds whole 128-entry
 * descriptor ranges, so those stale entries get resolved too; dereferencing them crashed or corrupted the loader. */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;   /* writers only; lookups are lock-free */
#define CAP (1u << 21)          /* 2M slots (16 MiB), never resized so readers need no lock */
static uintptr_t *g_table;
static size_t g_count;
#define TOMB ((uintptr_t)1)

static size_t slot_for(uintptr_t p)
{
    return (size_t)(((p >> 4) * 0x9E3779B97F4A7C15ull) >> 20) & (CAP - 1);
}

void bd12_live_add(const void *ptr)
{
    if (!ptr) return;
    pthread_mutex_lock(&g_lock);
    if (!g_table) {
        uintptr_t *t = calloc(CAP, sizeof(*t));
        if (!t) { pthread_mutex_unlock(&g_lock); return; }
        __atomic_store_n(&g_table, t, __ATOMIC_RELEASE);
    }
    size_t s = slot_for((uintptr_t)ptr);
    size_t tomb = (size_t)-1;
    for (size_t n = 0; n < CAP; ++n, s = (s + 1) & (CAP - 1)) {
        const uintptr_t v = g_table[s];
        if (v == (uintptr_t)ptr) { pthread_mutex_unlock(&g_lock); return; }
        if (v == TOMB && tomb == (size_t)-1) tomb = s;
        if (!v) { if (tomb != (size_t)-1) s = tomb; break; }
    }
    if (g_count < CAP * 3 / 4) {
        __atomic_store_n(&g_table[s], (uintptr_t)ptr, __ATOMIC_RELEASE);
        ++g_count;
    }
    pthread_mutex_unlock(&g_lock);
}

void bd12_live_remove(const void *ptr)
{
    if (!ptr || !g_table) return;
    pthread_mutex_lock(&g_lock);
    size_t s = slot_for((uintptr_t)ptr);
    for (size_t n = 0; n < CAP && g_table[s]; ++n, s = (s + 1) & (CAP - 1))
        if (g_table[s] == (uintptr_t)ptr) { __atomic_store_n(&g_table[s], TOMB, __ATOMIC_RELEASE); --g_count; break; }
    pthread_mutex_unlock(&g_lock);
}

int bd12_live_has(const void *ptr)
{
    uintptr_t *t = __atomic_load_n(&g_table, __ATOMIC_ACQUIRE);
    if (!ptr || !t) return 0;
    size_t s = slot_for((uintptr_t)ptr);
    for (size_t n = 0; n < CAP; ++n, s = (s + 1) & (CAP - 1)) {
        const uintptr_t v = __atomic_load_n(&t[s], __ATOMIC_ACQUIRE);
        if (v == (uintptr_t)ptr) return 1;
        if (!v) return 0;
    }
    return 0;
}

uint32_t bd12_live_serial(void)
{
    static uint32_t next;
    return __atomic_add_fetch(&next, 1, __ATOMIC_RELAXED);
}

int bd12_descriptor_alive(const BD12Descriptor *d)
{
    if (d->source_buffer && !(bd12_live_has(d->source_buffer) && d->source_buffer->serial == d->buffer_serial)) return 0;
    if (d->image && !(bd12_live_has(d->image) && d->image->serial == d->image_serial)) return 0;
    return 1;
}
