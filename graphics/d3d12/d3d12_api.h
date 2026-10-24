#ifndef BEER_D3D12_API_H
#define BEER_D3D12_API_H

#include <stdint.h>

typedef void (*BeerD3D12SignalEventFn)(uint64_t event_handle);
void beer_d3d12_set_event_signaler(BeerD3D12SignalEventFn signaler);
BeerD3D12SignalEventFn beer_d3d12_get_event_signaler(void);

uint64_t __attribute__((ms_abi))
beer_d3d12_create_device(uint64_t adapter, uint32_t minimum_feature_level,
                         const uint8_t *riid, void **device);
uint64_t __attribute__((ms_abi))
beer_d3d12_get_debug_interface(const uint8_t *riid, void **debug);
uint64_t __attribute__((ms_abi))
beer_d3d12_serialize_versioned_root_signature(const void *description,
                                              void **blob, void **error_blob);

#endif
