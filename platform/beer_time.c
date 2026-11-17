#include "beer_time.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t pause_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pause_cv = PTHREAD_COND_INITIALIZER;
static atomic_int paused;
static void pause_gate(void);
static atomic_uint scale = 1000;
static atomic_int touched;          /* set once the scale ever differed from 1.0 */
/* The scaled clock is read on every QueryPerformanceCounter/GetTickCount, on any thread, also by threads that
 * SuspendThread (a signal) stops at any instruction. Readers therefore take no lock: a sequence counter (odd while a
 * change is being written, seqlock) lets them retry instead. With a mutex, a guest thread suspended inside a clock
 * read held it and every other thread blocked on its next clock read (Onimusha froze as soon as the clock was first
 * scaled). Writers (scale changes, rare) still serialize on `lock`. */
static atomic_uint seq;
static _Atomic uint64_t base_real_ns;       /* CLOCK_MONOTONIC at the last scale change */
static _Atomic uint64_t base_virtual_ns;    /* guest monotonic time at that moment */
static uint64_t start_real_wall_ns; /* CLOCK_REALTIME captured with the first change */

static uint64_t clock_ns(clockid_t id)
{
    struct timespec ts;
    clock_gettime(id, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Caller holds `lock` (writers only). */
static uint64_t virtual_from_real(uint64_t real)
{
    return atomic_load_explicit(&base_virtual_ns, memory_order_relaxed) +
           (real - atomic_load_explicit(&base_real_ns, memory_order_relaxed)) * atomic_load(&scale) / 1000u;
}

/* Lock-free read of the scaled clock at real time `real` (seqlock reader). */
static uint64_t virtual_now(uint64_t real)
{
    for (;;) {
        const unsigned before = atomic_load_explicit(&seq, memory_order_acquire);
        if (before & 1u) continue;                      /* a change is being written: retry */
        const uint64_t base_real = atomic_load_explicit(&base_real_ns, memory_order_relaxed);
        const uint64_t base_virtual = atomic_load_explicit(&base_virtual_ns, memory_order_relaxed);
        const uint64_t factor = atomic_load_explicit(&scale, memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&seq, memory_order_relaxed) != before) continue;
        /* A reader that sampled `real` just before a change was published can see a base newer than its sample. */
        return base_virtual + (real > base_real ? (real - base_real) * factor / 1000u : 0);
    }
}

uint64_t beer_time_monotonic_ns(void)
{
    const uint64_t real = clock_ns(CLOCK_MONOTONIC);
    if (!atomic_load_explicit(&touched, memory_order_acquire)) return real;
    return virtual_now(real);
}

uint64_t beer_time_realtime_ns(void)
{
    const uint64_t wall = clock_ns(CLOCK_REALTIME);
    if (!atomic_load_explicit(&touched, memory_order_acquire)) return wall;
    return start_real_wall_ns + virtual_now(clock_ns(CLOCK_MONOTONIC));
}

void beer_time_set_scale_permille(uint32_t permille)
{
    if (permille > 10000u) permille = 10000u;   /* 10x cap keeps (elapsed * scale) inside 64 bits for days */
    pthread_mutex_lock(&lock);
    const uint64_t real = clock_ns(CLOCK_MONOTONIC);
    atomic_fetch_add_explicit(&seq, 1u, memory_order_acq_rel);      /* odd: readers retry */
    atomic_thread_fence(memory_order_release);
    if (!atomic_load(&touched)) {
        /* Anchor so that virtual == real at the moment the feature is first used. */
        atomic_store_explicit(&base_real_ns, real, memory_order_relaxed);
        atomic_store_explicit(&base_virtual_ns, real, memory_order_relaxed);
        start_real_wall_ns = clock_ns(CLOCK_REALTIME) - real;   /* wall = anchor + virtual monotonic */
    } else {
        atomic_store_explicit(&base_virtual_ns, virtual_from_real(real), memory_order_relaxed);
        atomic_store_explicit(&base_real_ns, real, memory_order_relaxed);
    }
    atomic_store_explicit(&scale, permille, memory_order_relaxed);
    atomic_fetch_add_explicit(&seq, 1u, memory_order_release);      /* even: published */
    atomic_store_explicit(&touched, 1, memory_order_release);
    pthread_mutex_unlock(&lock);
}

uint32_t beer_time_scale_permille(void)
{
    return atomic_load(&scale);
}

static int raw_mask(void)
{
    static int mask = -1;
    if (mask >= 0) return mask;
    mask = 0;
    const char *e = getenv("BEER_TIME_RAW");
    if (e) {
        if (strstr(e, "qpc")) mask |= BEER_CLOCK_QPC;
        if (strstr(e, "tick")) mask |= BEER_CLOCK_TICK;
        if (strstr(e, "kuser")) mask |= BEER_CLOCK_KUSER;
        if (strstr(e, "filetime")) mask |= BEER_CLOCK_FILETIME;
    }
    return mask;
}

uint64_t beer_time_monotonic_ns_for(int clock)
{
    if (clock & (BEER_CLOCK_QPC | BEER_CLOCK_TICK)) pause_gate();
    return (raw_mask() & clock) ? clock_ns(CLOCK_MONOTONIC) : beer_time_monotonic_ns();
}

uint64_t beer_time_realtime_ns_for(int clock)
{
    return (raw_mask() & clock) ? clock_ns(CLOCK_REALTIME) : beer_time_realtime_ns();
}

void beer_time_set_paused(int value)
{
    pthread_mutex_lock(&pause_lock);
    atomic_store(&paused, value);
    if (!value) pthread_cond_broadcast(&pause_cv);
    pthread_mutex_unlock(&pause_lock);
}

static void pause_gate(void)
{
    if (!atomic_load_explicit(&paused, memory_order_relaxed)) return;
    pthread_mutex_lock(&pause_lock);
    while (atomic_load(&paused)) pthread_cond_wait(&pause_cv, &pause_lock);
    pthread_mutex_unlock(&pause_lock);
}
