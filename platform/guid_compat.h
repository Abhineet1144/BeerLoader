#ifndef BEER_GUID_COMPAT_H
#define BEER_GUID_COMPAT_H

#include <stdint.h>

/* A GUID is stored in Windows memory byte order. Data3 occupies bytes 6-7 in
 * little-endian order, so the RFC 4122 version nibble is in byte 7. Data4 is
 * byte ordered and its variant bits begin at byte 8. */
static inline void beer_guid_apply_v4_bits(uint8_t guid[16])
{
    guid[7] = (uint8_t)((guid[7] & 0x0fu) | 0x40u);
    guid[8] = (uint8_t)((guid[8] & 0x3fu) | 0x80u);
}

#endif
