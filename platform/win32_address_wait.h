#ifndef BEER_WIN32_ADDRESS_WAIT_H
#define BEER_WIN32_ADDRESS_WAIT_H

#include <stddef.h>
#include <stdint.h>

/* Return values used by the Win32 wrapper: success, timeout, invalid input. */
enum {
    BEER_ADDRESS_WAIT_INVALID = -1,
    BEER_ADDRESS_WAIT_TIMEOUT = 0,
    BEER_ADDRESS_WAIT_SUCCESS = 1,
};

int beer_address_wait(const volatile void *address, const void *compare,
                      size_t size, uint32_t timeout_ms);
void beer_address_wake_one(const void *address);
void beer_address_wake_all(const void *address);

#endif
