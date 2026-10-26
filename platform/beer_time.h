#ifndef BEER_TIME_H
#define BEER_TIME_H

#include <stdint.h>

/* Guest-visible clocks with a live speed multiplier. The scale is fixed point (permille, 1000 = 1.0x) so
 * the loader, which is built without SSE, never needs floating point. Changing the scale keeps the
 * guest clock continuous: virtual time only advances at the new rate from the moment of the change. */
/* Which guest-visible clock is asking; BEER_TIME_RAW=qpc,tick,kuser,filetime leaves those clocks unscaled (diagnostics). */
enum { BEER_CLOCK_QPC = 1, BEER_CLOCK_TICK = 2, BEER_CLOCK_KUSER = 4, BEER_CLOCK_FILETIME = 8 };
uint64_t beer_time_monotonic_ns_for(int clock);
uint64_t beer_time_realtime_ns_for(int clock);
uint64_t beer_time_monotonic_ns(void);
uint64_t beer_time_realtime_ns(void);   /* wall clock: always real time (see beer_time.c) */
void beer_time_set_scale_permille(uint32_t permille);
uint32_t beer_time_scale_permille(void);

/* Pause: the clocks stop AND any thread that asks the performance counter or tick count blocks until resume, so no
 * game code ever sees a frozen clock (zero frame deltas, spin loops waiting for time to move). Time stays continuous. */
void beer_time_set_paused(int paused);

#endif
