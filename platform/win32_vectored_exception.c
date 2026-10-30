#include "guest_cpu.h"
#include "win32_vectored_exception.h"

#include <stdatomic.h>
#include <stddef.h>

#define BEER_VEH_CAPACITY 64

typedef struct BeerVehEntry {
    _Atomic(uintptr_t) handler;
    _Atomic(uint64_t) sequence;
    _Atomic(uint32_t) active;
    uint32_t first;
} BeerVehEntry;

static BeerVehEntry g_entries[BEER_VEH_CAPACITY];
static _Atomic(uint64_t) g_sequence;

typedef struct BeerVehSnapshot {
    BeerVectoredExceptionHandler handler;
    uint64_t sequence;
    uint32_t first;
} BeerVehSnapshot;

void *beer_veh_add(uint32_t first, BeerVectoredExceptionHandler handler)
{
    if (!handler) return NULL;

    for (size_t i = 0; i < BEER_VEH_CAPACITY; ++i) {
        uint32_t expected = 0;
        if (!atomic_compare_exchange_strong_explicit(&g_entries[i].active,
                                                     &expected, 2,
                                                     memory_order_acq_rel,
                                                     memory_order_relaxed))
            continue;
        g_entries[i].first = first != 0;
        atomic_store_explicit(&g_entries[i].handler, (uintptr_t)handler,
                              memory_order_relaxed);
        atomic_store_explicit(&g_entries[i].sequence,
                              atomic_fetch_add_explicit(&g_sequence, 1,
                                                        memory_order_relaxed) + 1,
                              memory_order_relaxed);
        atomic_store_explicit(&g_entries[i].active, 1, memory_order_release);
        return &g_entries[i];
    }
    return NULL;
}

int beer_veh_remove(void *handle)
{
    if (!handle) return 0;
    BeerVehEntry *entry = handle;
    if (entry < g_entries || entry >= g_entries + BEER_VEH_CAPACITY) return 0;
    if (atomic_exchange_explicit(&entry->active, 0, memory_order_acq_rel) != 1)
        return 0;
    atomic_store_explicit(&entry->handler, 0, memory_order_release);
    return 1;
}

static int snapshot_before(const BeerVehSnapshot *left,
                           const BeerVehSnapshot *right)
{
    if (left->first != right->first) return left->first > right->first;
    if (left->first) return left->sequence > right->sequence;
    return left->sequence < right->sequence;
}

int beer_veh_dispatch(void *exception_pointers)
{
    BeerVehSnapshot snapshot[BEER_VEH_CAPACITY];
    size_t count = 0;
    for (size_t i = 0; i < BEER_VEH_CAPACITY; ++i) {
        if (atomic_load_explicit(&g_entries[i].active, memory_order_acquire) != 1)
            continue;
        uintptr_t handler = atomic_load_explicit(&g_entries[i].handler,
                                                 memory_order_acquire);
        if (!handler) continue;
        snapshot[count].handler = (BeerVectoredExceptionHandler)handler;
        snapshot[count].sequence = atomic_load_explicit(&g_entries[i].sequence,
                                                        memory_order_relaxed);
        snapshot[count].first = g_entries[i].first;
        ++count;
    }

    /* At most 64 entries: insertion sort avoids allocation and remains usable
     * from the Unix signal path. */
    for (size_t i = 1; i < count; ++i) {
        BeerVehSnapshot value = snapshot[i];
        size_t j = i;
        while (j && snapshot_before(&value, &snapshot[j - 1])) {
            snapshot[j] = snapshot[j - 1];
            --j;
        }
        snapshot[j] = value;
    }

    for (size_t i = 0; i < count; ++i) {
        int32_t result = BEER_GUEST_CALL(snapshot[i].handler, exception_pointers);
        if (result == BEER_EXCEPTION_CONTINUE_EXECUTION) return 1;
    }
    return 0;
}

void beer_veh_reset(void)
{
    for (size_t i = 0; i < BEER_VEH_CAPACITY; ++i) {
        atomic_store_explicit(&g_entries[i].active, 0, memory_order_relaxed);
        atomic_store_explicit(&g_entries[i].handler, 0, memory_order_relaxed);
        atomic_store_explicit(&g_entries[i].sequence, 0, memory_order_relaxed);
        g_entries[i].first = 0;
    }
    atomic_store_explicit(&g_sequence, 0, memory_order_relaxed);
}
