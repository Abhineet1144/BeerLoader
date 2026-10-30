#ifndef BEER_WIN32_ENTRY_H
#define BEER_WIN32_ENTRY_H

#include <stdint.h>

typedef void __attribute__((ms_abi)) (*BeerGuestTlsRunner)(void);

/* SysV caller; stack is the final Win64 entry RSP (8 modulo 16).
 * Run process TLS on that stack, then tail-enter the PE without returning. */
void beer_guest_enter(uintptr_t stack, BeerGuestTlsRunner tls, uintptr_t entry)
    __attribute__((noreturn));

#endif