#ifndef BEER_WIN32_ONEXIT_H
#define BEER_WIN32_ONEXIT_H

#include <stdint.h>

typedef int (__attribute__((ms_abi)) *BeerOnExitCallback)(void);

typedef struct {
    BeerOnExitCallback *first;
    BeerOnExitCallback *last;
    BeerOnExitCallback *end;
} BeerOnExitTable;

int __attribute__((ms_abi)) beer_onexit_initialize(BeerOnExitTable *table);
int __attribute__((ms_abi)) beer_onexit_register(BeerOnExitTable *table,
                                                BeerOnExitCallback callback);
int __attribute__((ms_abi)) beer_onexit_execute(BeerOnExitTable *table);

#endif