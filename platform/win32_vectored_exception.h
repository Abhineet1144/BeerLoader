#ifndef BEER_WIN32_VECTORED_EXCEPTION_H
#define BEER_WIN32_VECTORED_EXCEPTION_H

#include <stdint.h>

#define BEER_EXCEPTION_CONTINUE_EXECUTION ((int32_t)-1)
#define BEER_EXCEPTION_CONTINUE_SEARCH    ((int32_t)0)

typedef int32_t (__attribute__((ms_abi)) *BeerVectoredExceptionHandler)(void *exception_pointers);

/* Register a Windows-style vectored exception callback. `first` inserts it
 * ahead of older handlers; otherwise it is appended. The returned opaque
 * handle is accepted by beer_veh_remove(). */
void *beer_veh_add(uint32_t first, BeerVectoredExceptionHandler handler);
int beer_veh_remove(void *handle);

/* Invoke a signal-safe snapshot of the current handler chain. Returns nonzero
 * only when a handler requests EXCEPTION_CONTINUE_EXECUTION. */
int beer_veh_dispatch(void *exception_pointers);

/* Same, but each handler is invoked through `caller` (NULL = direct call). The loader's caller marks a stack
 * boundary so a nested exception raised by a handler can unwind through it to the original fault context. */
typedef int32_t (*BeerVehCaller)(BeerVectoredExceptionHandler handler, void *exception_pointers);
int beer_veh_dispatch_with(void *exception_pointers, BeerVehCaller caller);

/* Test/process-reset helper. Do not call concurrently with dispatch. */
void beer_veh_reset(void);

#endif
