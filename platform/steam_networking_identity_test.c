#include "steam_networking_identity.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    unsigned char storage[138];
    memset(storage, 0xa5, sizeof(storage));
    const uint64_t id = UINT64_C(76561199000000001);
    assert(!beer_steam_networking_identity(NULL, id));
    assert(beer_steam_networking_identity(storage + 1, id));
    uint32_t type, size;
    uint64_t got;
    memcpy(&type, storage + 1, 4);
    memcpy(&size, storage + 5, 4);
    memcpy(&got, storage + 9, 8);
    assert(type == 16 && size == 8 && got == id);
    for (int i = 17; i < 137; ++i) assert(storage[i] == 0);
    assert(storage[0] == 0xa5 && storage[137] == 0xa5);
    assert(!beer_steam_networking_identity(storage + 1, 0));
    for (int i = 1; i < 137; ++i) assert(storage[i] == 0);
    assert(storage[0] == 0xa5 && storage[137] == 0xa5);
    puts("Steam networking identity tests passed");
    return 0;
}