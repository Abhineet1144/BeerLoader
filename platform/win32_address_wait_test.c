#include "win32_address_wait.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

static _Atomic(uint32_t) g_word;
static _Atomic(int) g_ready;
static _Atomic(int) g_result;

static void *waiter(void *unused)
{
    (void)unused;
    const uint32_t expected = 7;
    atomic_fetch_add(&g_ready, 1);
    int result = beer_address_wait(&g_word, &expected, sizeof(expected), 2000);
    atomic_store(&g_result, result);
    return NULL;
}

int main(void)
{
    uint32_t expected = 7;
    atomic_store(&g_word, 8);
    assert(beer_address_wait(&g_word, &expected, sizeof(expected), 0) ==
           BEER_ADDRESS_WAIT_SUCCESS);

    atomic_store(&g_word, 7);
    assert(beer_address_wait(&g_word, &expected, sizeof(expected), 5) ==
           BEER_ADDRESS_WAIT_TIMEOUT);
    assert(beer_address_wait(&g_word, &expected, 3, 0) ==
           BEER_ADDRESS_WAIT_INVALID);
    assert(beer_address_wait(NULL, &expected, sizeof(expected), 0) ==
           BEER_ADDRESS_WAIT_INVALID);

    atomic_store(&g_ready, 0);
    atomic_store(&g_result, BEER_ADDRESS_WAIT_INVALID);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, waiter, NULL) == 0);
    while (!atomic_load(&g_ready)) {
        struct timespec delay = {0, 1000000L};
        nanosleep(&delay, NULL);
    }
    struct timespec delay = {0, 10000000L};
    nanosleep(&delay, NULL);
    atomic_store(&g_word, 9);
    beer_address_wake_one(&g_word);
    assert(pthread_join(thread, NULL) == 0);
    assert(atomic_load(&g_result) == BEER_ADDRESS_WAIT_SUCCESS);

    puts("win32 address-wait tests passed");
    return 0;
}
