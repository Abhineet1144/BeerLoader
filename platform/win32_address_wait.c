#include "win32_address_wait.h"

#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

/* Address waits need an atomic compare-and-sleep relationship with wake calls,
 * not a persistent object stored in guest memory. Hashing addresses onto keyed
 * condition variables provides that relationship. Hash collisions may produce
 * a spurious wake, which WaitOnAddress explicitly permits callers to handle by
 * rechecking their value. */
#define BEER_ADDRESS_WAIT_BUCKETS 256u

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    uint64_t generation;
} BeerAddressWaitBucket;

static BeerAddressWaitBucket g_buckets[BEER_ADDRESS_WAIT_BUCKETS];
static pthread_once_t g_buckets_once = PTHREAD_ONCE_INIT;

static void address_wait_init(void)
{
    for (size_t i = 0; i < BEER_ADDRESS_WAIT_BUCKETS; ++i) {
        pthread_mutex_init(&g_buckets[i].mutex, NULL);
        pthread_cond_init(&g_buckets[i].condition, NULL);
    }
}

static BeerAddressWaitBucket *address_bucket(const void *address)
{
    uintptr_t key = (uintptr_t)address;
    key ^= key >> 17;
    key ^= key >> 9;
    return &g_buckets[key & (BEER_ADDRESS_WAIT_BUCKETS - 1u)];
}

static int supported_size(size_t size)
{
    return size == 1 || size == 2 || size == 4 || size == 8;
}

static int values_equal(const volatile void *address, const void *compare,
                        size_t size)
{
    unsigned char current[8];
    for (size_t i = 0; i < size; ++i)
        current[i] = ((const volatile unsigned char *)address)[i];
    return memcmp(current, compare, size) == 0;
}

static int realtime_deadline(struct timespec *deadline, uint32_t timeout_ms)
{
    if (clock_gettime(CLOCK_REALTIME, deadline) != 0) return -1;
    deadline->tv_sec += timeout_ms / 1000u;
    deadline->tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        ++deadline->tv_sec;
        deadline->tv_nsec -= 1000000000L;
    }
    return 0;
}

int beer_address_wait(const volatile void *address, const void *compare,
                      size_t size, uint32_t timeout_ms)
{
    if (!address || !compare || !supported_size(size))
        return BEER_ADDRESS_WAIT_INVALID;

    pthread_once(&g_buckets_once, address_wait_init);
    BeerAddressWaitBucket *bucket = address_bucket((const void *)address);
    pthread_mutex_lock(&bucket->mutex);

    if (!values_equal(address, compare, size)) {
        pthread_mutex_unlock(&bucket->mutex);
        return BEER_ADDRESS_WAIT_SUCCESS;
    }
    if (timeout_ms == 0) {
        pthread_mutex_unlock(&bucket->mutex);
        return BEER_ADDRESS_WAIT_TIMEOUT;
    }

    const uint64_t generation = bucket->generation;
    int rc = 0;
    if (timeout_ms == UINT32_MAX) {
        while (bucket->generation == generation && rc == 0)
            rc = pthread_cond_wait(&bucket->condition, &bucket->mutex);
    } else {
        struct timespec deadline;
        if (realtime_deadline(&deadline, timeout_ms) != 0) rc = EINVAL;
        while (bucket->generation == generation && rc == 0)
            rc = pthread_cond_timedwait(&bucket->condition, &bucket->mutex,
                                        &deadline);
    }

    pthread_mutex_unlock(&bucket->mutex);
    if (rc == ETIMEDOUT) return BEER_ADDRESS_WAIT_TIMEOUT;
    if (rc != 0) return BEER_ADDRESS_WAIT_INVALID;
    return BEER_ADDRESS_WAIT_SUCCESS;
}

static void address_wake(const void *address, int all)
{
    if (!address) return;
    pthread_once(&g_buckets_once, address_wait_init);
    BeerAddressWaitBucket *bucket = address_bucket(address);
    pthread_mutex_lock(&bucket->mutex);
    ++bucket->generation;
    if (all) pthread_cond_broadcast(&bucket->condition);
    else pthread_cond_signal(&bucket->condition);
    pthread_mutex_unlock(&bucket->mutex);
}

void beer_address_wake_one(const void *address)
{
    address_wake(address, 0);
}

void beer_address_wake_all(const void *address)
{
    address_wake(address, 1);
}
