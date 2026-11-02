#ifndef BEER_WIN32_SHELL_STRINGS_H
#define BEER_WIN32_SHELL_STRINGS_H
#include <stdint.h>
uint64_t __attribute__((ms_abi)) beer_PathCanonicalizeW(uint16_t *, const uint16_t *);
uint64_t __attribute__((ms_abi)) beer_PathCombineW(uint16_t *, const uint16_t *, const uint16_t *);
int __attribute__((ms_abi)) beer_wsprintfW(uint16_t *, const uint16_t *, ...);
int __attribute__((ms_abi)) beer_wvsprintfW(uint16_t *, const uint16_t *, const uint64_t *);
#endif