#ifndef BEER_STEAM_NETWORKING_H
#define BEER_STEAM_NETWORKING_H
#include <stdint.h>

/* Optional standalone GameNetworkingSockets backend. Local listening/polling
 * resources only: Steam relay certificates and remote signaling are not supplied. */
int beer_steam_networking_start(uint64_t steamid);
uint32_t beer_steam_networking_listen(int port, int count, const void *options);
int beer_steam_networking_close_listener(uint32_t handle);
uint32_t beer_steam_networking_poll_group(void);
int beer_steam_networking_destroy_poll_group(uint32_t handle);
int beer_steam_networking_set_poll_group(uint32_t connection, uint32_t group);
int beer_steam_networking_receive_poll_group(uint32_t group, void **messages, int count);
#endif