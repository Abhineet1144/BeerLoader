#include "steam_interface_contract.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    uint64_t vtable[32] = {0};
    beer_steam_utils_install_async_slots(vtable, 32, 0x11, 0x12, 0x13, 0x14);
    assert(vtable[11] == 0x11);
    assert(vtable[12] == 0x12);
    assert(vtable[13] == 0x13);
    assert(vtable[14] == 0x14);

    assert(beer_steam_is_offline_input_interface("steaminput006"));
    assert(beer_steam_is_offline_input_interface("steamcontroller008"));
    assert(!beer_steam_is_offline_input_interface("steamutils010"));
    assert(!beer_steam_is_offline_input_interface(NULL));

    puts("steam interface contract test passed");
    return 0;
}
