#ifndef BEER_WIN32_CRT_INIT_H
#define BEER_WIN32_CRT_INIT_H

#include <stdint.h>

typedef void (__attribute__((ms_abi)) *BeerCrtInitializer)(void);
typedef int (__attribute__((ms_abi)) *BeerCrtErrorInitializer)(void);

uint64_t __attribute__((ms_abi)) beer_crt_initterm(BeerCrtInitializer *first,
                                                  BeerCrtInitializer *last);
int __attribute__((ms_abi)) beer_crt_initterm_e(BeerCrtErrorInitializer *first,
                                               BeerCrtErrorInitializer *last);

#endif