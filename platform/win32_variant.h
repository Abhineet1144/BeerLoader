#ifndef BEER_WIN32_VARIANT_H
#define BEER_WIN32_VARIANT_H
#include <stdint.h>

/* OLEAUT32 VARIANT helpers. Compiled with SSE (floating-point conversions); BSTRs come from the loader's allocator,
 * registered with beer_variant_set_bstr_allocator before use. */
typedef uint64_t (__attribute__((ms_abi)) *BeerBstrAlloc)(const uint16_t *text, uint32_t length);
typedef uint64_t (__attribute__((ms_abi)) *BeerBstrFree)(uint16_t *text);
void beer_variant_set_bstr_allocator(BeerBstrAlloc alloc, BeerBstrFree release);

uint64_t __attribute__((ms_abi)) beer_VariantInit(uint8_t *variant);
uint64_t __attribute__((ms_abi)) beer_VariantClear(uint8_t *variant);
uint64_t __attribute__((ms_abi)) beer_VariantCopy(uint8_t *destination, const uint8_t *source);
uint64_t __attribute__((ms_abi)) beer_VariantChangeTypeEx(uint8_t *destination, const uint8_t *source, uint32_t locale,
                                                          uint16_t flags, uint16_t vt);
uint64_t __attribute__((ms_abi)) beer_VariantChangeType(uint8_t *destination, const uint8_t *source, uint16_t flags,
                                                        uint16_t vt);
#endif
