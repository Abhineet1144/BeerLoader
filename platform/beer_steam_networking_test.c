#include "beer_steam_networking.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    assert(!beer_steam_networking_listen(7777, 0, NULL));
    assert(!beer_steam_networking_close_listener(0));
    assert(beer_steam_networking_receive_poll_group(0, NULL, 0) == -1);
    if (!getenv("BEER_STEAM_NETWORKING_LIBRARY")) {
        assert(!beer_steam_networking_start(UINT64_C(76561199000000001)));
        puts("Steam networking disabled-backend tests passed");
        return 0;
    }
    assert(beer_steam_networking_start(UINT64_C(76561199000000001)));
    assert(!beer_steam_networking_listen(-1, 0, NULL));
    assert(!beer_steam_networking_listen(65536, 0, NULL));
    assert(!beer_steam_networking_listen(7777, -1, NULL));
    assert(!beer_steam_networking_listen(7777, 1, NULL));
    uint32_t first = beer_steam_networking_listen(7777, 0, NULL);
    assert(first);
    /* A real backend owns the virtual port: a second listener must fail. */
    assert(!beer_steam_networking_listen(7777, 0, NULL));
    uint32_t group = beer_steam_networking_poll_group();
    assert(group);
    void *message = NULL;
    assert(beer_steam_networking_receive_poll_group(group, &message, 1) == 0);
    assert(!message);
    assert(!beer_steam_networking_set_poll_group(0, group));
    assert(beer_steam_networking_destroy_poll_group(group));
    assert(!beer_steam_networking_destroy_poll_group(group));
    assert(beer_steam_networking_close_listener(first));
    assert(!beer_steam_networking_close_listener(first));
    first = beer_steam_networking_listen(7777, 0, NULL);
    assert(first && beer_steam_networking_close_listener(first));
    puts("Steam networking real-resource tests passed");
    return 0;
}