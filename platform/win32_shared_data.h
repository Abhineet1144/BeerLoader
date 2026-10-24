#ifndef BEER_WIN32_SHARED_DATA_H
#define BEER_WIN32_SHARED_DATA_H

#include <stddef.h>
#include <stdint.h>

#define BEER_WIN32_SHARED_DATA_ADDRESS ((uintptr_t)0x7ffe0000u)
#define BEER_WIN32_SHARED_DATA_SIZE 0x10000u

void beer_win32_shared_data_initialize(void *memory, size_t size);
void beer_win32_shared_data_refresh(void *memory, size_t size,
                                    uint64_t realtime_100ns,
                                    uint64_t monotonic_100ns);
int beer_win32_shared_data_map(void);

#endif
