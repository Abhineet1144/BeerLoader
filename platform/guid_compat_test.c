#include "guid_compat.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    uint8_t first[16];
    uint8_t second[16];
    memset(first, 0xff, sizeof(first));
    memset(second, 0x00, sizeof(second));

    beer_guid_apply_v4_bits(first);
    beer_guid_apply_v4_bits(second);

    if ((first[7] & 0xf0u) != 0x40u || (second[7] & 0xf0u) != 0x40u)
        return 1;
    if ((first[8] & 0xc0u) != 0x80u || (second[8] & 0xc0u) != 0x80u)
        return 2;
    if (first[6] != 0xffu || first[9] != 0xffu || second[6] != 0u ||
        second[9] != 0u)
        return 3;

    puts("GUID compatibility tests passed");
    return 0;
}
