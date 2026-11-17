#ifndef BEER_STEAM_NETWORKING_IDENTITY_H
#define BEER_STEAM_NETWORKING_IDENTITY_H

#include <stdint.h>
#include <string.h>

/* SteamNetworkingSockets012: SteamNetworkingIdentity is packed to one byte,
 * with a 32-bit type, 32-bit payload size, and 128-byte payload union.
 * Identity is distinct from transport availability or authentication. */
static inline int beer_steam_networking_identity(void *out, uint64_t steamid)
{
    if (!out) return 0;
    unsigned char *p = out;
    memset(p, 0, 136);
    if (!steamid) return 0;
    const uint32_t type = 16, size = 8;
    memcpy(p, &type, 4);
    memcpy(p + 4, &size, 4);
    memcpy(p + 8, &steamid, 8);
    return 1;
}

#endif