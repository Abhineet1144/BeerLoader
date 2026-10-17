#ifndef BEER_WASAPI_COMPAT_H
#define BEER_WASAPI_COMPAT_H

#include <stdint.h>

typedef void (*BeerWasapiSignalEventFn)(uint64_t event_handle);

uint64_t beer_wasapi_create_instance(const void *class_id, const void *interface_id,
                                     void **object);
void beer_wasapi_set_event_signaler(BeerWasapiSignalEventFn signal_event);
int16_t beer_wasapi_float_bits_to_s16(uint32_t bits);
void beer_wasapi_shutdown(void);

#endif
