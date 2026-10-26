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
static uint64_t base_real_ns;       /* CLOCK_MONOTONIC at the last scale change */
static uint64_t base_virtual_ns;    /* guest monotonic time at that moment */
static uint64_t start_real_wall_ns; /* CLOCK_REALTIME captured with the first change */

static uint64_t clock_ns(clockid_t id)
{
    struct timespec ts;
    clock_gettime(id, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t virtual_from_real(uint64_t real)
{
    return base_virtual_ns + (real - base_real_ns) * atomic_load(&scale) / 1000u;
}

uint64_t beer_time_monotonic_ns(void)
{
    const uint64_t real = clock_ns(CLOCK_MONOTONIC);
    if (!atomic_load_explicit(&touched, memory_order_acquire)) return real;
    pthread_mutex_lock(&lock);
    const uint64_t value = virtual_from_real(real);
    pthread_mutex_unlock(&lock);
    return value;
}

uint64_t beer_time_realtime_ns(void)
{
    const uint64_t wall = clock_ns(CLOCK_REALTIME);
    if (!atomic_load_explicit(&touched, memory_order_acquire)) return wall;
    const uint64_t real = clock_ns(CLOCK_MONOTONIC);
    pthread_mutex_lock(&lock);
    const uint64_t value = start_real_wall_ns + virtual_from_real(real);
    pthread_mutex_unlock(&lock);
    return value;
}

void beer_time_set_scale_permille(uint32_t permille)
{
    if (permille > 10000u) permille = 10000u;   /* 10x cap keeps (elapsed * scale) inside 64 bits for days */
    pthread_mutex_lock(&lock);
    const uint64_t real = clock_ns(CLOCK_MONOTONIC);
    if (!atomic_load(&touched)) {
        /* Anchor so that virtual == real at the moment the feature is first used. */
        base_real_ns = real;
        base_virtual_ns = real;
        start_real_wall_ns = clock_ns(CLOCK_REALTIME) - real;   /* wall = anchor + virtual monotonic */
        atomic_store_explicit(&touched, 1, memory_order_release);
    } else {
        base_virtual_ns = virtual_from_real(real);
        base_real_ns = real;
    }
    atomic_store(&scale, permille);
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
