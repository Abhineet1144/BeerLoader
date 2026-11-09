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
/* Removals leave tombstones, and a lookup only stops at an empty slot. Games that stream resources (RE8 while
 * exploring) create and destroy them all the time, so tombstones piled up until a miss walked most of the 2M-slot
 * table: descriptor resolution, which checks every descriptor, then took milliseconds per draw and the game ran at a
 * frame every few seconds. Once tombstones reach 1/16 of the table, the writer rebuilds it with only the live entries
 * and publishes the new table; readers stay lock-free. A replaced table is freed at the following rebuild, long after
 * any lookup could still be walking it. */
static size_t g_tombs;
static uintptr_t *g_retired;
#define TOMB_LIMIT (CAP / 16)

static size_t slot_for(uintptr_t p)
{
    return (size_t)(((p >> 4) * 0x9E3779B97F4A7C15ull) >> 20) & (CAP - 1);
}

/* Caller holds g_lock. */
static void rebuild_locked(void)
{
    uintptr_t *fresh = calloc(CAP, sizeof(*fresh));
    if (!fresh) return;                                  /* keep the old table: still correct, only slower */
    size_t count = 0;
    for (size_t i = 0; i < CAP; ++i) {
        const uintptr_t v = g_table[i];
        if (!v || v == TOMB) continue;
        size_t s = slot_for(v);
        while (fresh[s]) s = (s + 1) & (CAP - 1);
        fresh[s] = v;
        ++count;
    }
    free(g_retired);                                     /* replaced one rebuild ago: no reader still holds it */
    g_retired = g_table;
    __atomic_store_n(&g_table, fresh, __ATOMIC_RELEASE);
    g_count = count;
    g_tombs = 0;
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
        if (g_table[s] == TOMB && g_tombs) --g_tombs;
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
        if (g_table[s] == (uintptr_t)ptr) {
            __atomic_store_n(&g_table[s], TOMB, __ATOMIC_RELEASE);
            --g_count;
            if (++g_tombs >= TOMB_LIMIT) rebuild_locked();
            break;
        }
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
    static int off = -1;
    if (off < 0) off = getenv("BEER_BD12_NO_ALIVE") != NULL;     /* diagnostic: trust every descriptor */
    if (off) return 1;
    /* Each pointer is read once: another thread may rewrite this descriptor slot while a command list resolves it
     * (games update descriptors concurrently), and re-reading d->image after the liveness check returned NULL
     * (Ghost of Tsushima: access violation at 0x78). */
    BD12Buffer *const buffer = __atomic_load_n(&d->source_buffer, __ATOMIC_ACQUIRE);
    const uint32_t buffer_serial = __atomic_load_n(&d->buffer_serial, __ATOMIC_RELAXED);
    BD12Image *const image = __atomic_load_n(&d->image, __ATOMIC_ACQUIRE);
    const uint32_t image_serial = __atomic_load_n(&d->image_serial, __ATOMIC_RELAXED);
    if (buffer && !(bd12_live_has(buffer) && buffer->serial == buffer_serial)) return 0;
    if (image && !(bd12_live_has(image) && image->serial == image_serial)) return 0;
    return 1;
}
