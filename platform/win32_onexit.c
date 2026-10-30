#define _GNU_SOURCE
#include "win32_onexit.h"
#include "guest_cpu.h"

#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>

/* Recursive because an exit callback may register another callback. The lock
 * also prevents concurrent table growth from invalidating the cursor. */
static pthread_mutex_t onexit_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

int __attribute__((ms_abi)) beer_onexit_initialize(BeerOnExitTable *table)
{
    if (!table) return -1;
    pthread_mutex_lock(&onexit_lock);
    table->first = table->last = table->end = NULL;
    pthread_mutex_unlock(&onexit_lock);
    return 0;
}

int __attribute__((ms_abi)) beer_onexit_register(BeerOnExitTable *table,
                                                BeerOnExitCallback callback)
{
    if (!table || !callback) return -1;
    pthread_mutex_lock(&onexit_lock);
    size_t count = table->first ? (size_t)(table->last - table->first) : 0;
    size_t capacity = table->first ? (size_t)(table->end - table->first) : 0;
    if (count == capacity) {
        if (capacity > SIZE_MAX / sizeof(*table->first) / 2) {
            pthread_mutex_unlock(&onexit_lock);
            return -1;
        }
        size_t next_capacity = capacity ? capacity * 2 : 32;
        BeerOnExitCallback *next = realloc(table->first,
                                           next_capacity * sizeof(*next));
        if (!next) {
            pthread_mutex_unlock(&onexit_lock);
            return -1;
        }
        table->first = next;
        table->last = next + count;
        table->end = next + next_capacity;
    }
    *table->last++ = callback;
    pthread_mutex_unlock(&onexit_lock);
    return 0;
}

int __attribute__((ms_abi)) beer_onexit_execute(BeerOnExitTable *table)
{
    if (!table) return -1;
    pthread_mutex_lock(&onexit_lock);
    while (table->first && table->last > table->first) {
        BeerOnExitCallback callback = *--table->last;
        /* Retain the Win64 function-pointer type: a SysV call here has no
         * 32-byte home space and corrupts the host frame for real CRT callbacks. */
        if (callback) BEER_GUEST_CALL(callback);
    }
    free(table->first);
    table->first = table->last = table->end = NULL;
    pthread_mutex_unlock(&onexit_lock);
    return 0;
}