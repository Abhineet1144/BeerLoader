#ifndef BEER_STEAM_NETWORKING_H
#define BEER_STEAM_NETWORKING_H
#include <stddef.h>
#include <stdint.h>

/* Steam networking on the GameNetworkingSockets library (BEER_STEAM_NETWORKING_LIBRARY), see
 * beer_steam_networking.c. Arguments and results use the SteamNetworkingSockets012 types, whose layouts match the
 * Win64 SDK; message objects returned by the receive calls are the library's (release them with
 * beer_steam_networking_release_message). */
int beer_steam_networking_start(uint64_t steamid);

/* Signaling transport for peer-to-peer connections: send delivers one rendezvous blob to `dest` for `appid` through
 * Steam (nonzero on success); next returns the next blob Steam delivered (its length, -1 when none), or with
 * *failed set a delivery failure. Starts the service thread; connections to peers need it. */
typedef struct {
    int (*send)(uint32_t appid, uint64_t dest, const void *body, size_t len);
    long (*next)(uint64_t *src, uint64_t *dest, uint32_t *appid, int *failed, uint8_t *out, size_t cap);
    long (*certificate)(uint32_t appid, const uint8_t key[32], uint8_t *out, size_t cap, char *err, size_t errlen);
    long (*config)(uint32_t appid, uint8_t *out, size_t cap, char *err, size_t errlen);
} BeerP2PSignaling;
int beer_steam_networking_set_signaling(uint32_t appid, const BeerP2PSignaling *signaling);
int beer_steam_networking_p2p_ready(void);

/* SteamNetConnectionStatusChangedCallback_t (callback 1221) in the Win64 layout. */
#define BEER_STEAM_STATUS_CALLBACK 1221
#define BEER_STEAM_STATUS_SIZE 712
int beer_steam_networking_next_status(void *out);
#define BEER_STEAM_AUTH_CALLBACK 1222
#define BEER_STEAM_AUTH_SIZE 260
int beer_steam_networking_next_auth(void *out);

/* ESteamNetworkingConfigValue values Beer handles itself. */
enum {
    BEER_NET_CONFIG_P2P_STUN_SERVER_LIST = 103,
    BEER_NET_CONFIG_CALLBACK_CONNECTION_STATUS = 201,
    BEER_NET_CONFIG_CALLBACK_FIRST = 201,
    BEER_NET_CONFIG_CALLBACK_LAST = 207,
};

uint32_t beer_steam_networking_listen(int port, int count, const void *options);
uint32_t beer_steam_networking_listen_ip(const void *address, int count, const void *options);
uint32_t beer_steam_networking_connect_ip(const void *address, int count, const void *options);
uint32_t beer_steam_networking_connect_p2p(const void *identity, int port, int count, const void *options);
int beer_steam_networking_accept(uint32_t connection);
int beer_steam_networking_close(uint32_t connection, int reason, const char *debug, int linger);
int beer_steam_networking_close_listener(uint32_t handle);
int beer_steam_networking_set_user_data(uint32_t connection, int64_t value);
int64_t beer_steam_networking_get_user_data(uint32_t connection);
void beer_steam_networking_set_name(uint32_t connection, const char *name);
int beer_steam_networking_get_name(uint32_t connection, char *out, int cap);
int beer_steam_networking_send(uint32_t connection, const void *data, uint32_t size, int flags, int64_t *number);
int beer_steam_networking_flush(uint32_t connection);
int beer_steam_networking_receive(uint32_t connection, void **messages, int count);
int beer_steam_networking_info(uint32_t connection, void *info);
int beer_steam_networking_realtime(uint32_t connection, void *status, int lanes, void *lane_status);
int beer_steam_networking_detailed(uint32_t connection, char *out, int cap);
int beer_steam_networking_listen_address(uint32_t handle, void *address);
int beer_steam_networking_socket_pair(uint32_t *a, uint32_t *b, int loopback, const void *identity1, const void *identity2);
int beer_steam_networking_configure_lanes(uint32_t connection, int lanes, const int *priorities, const uint16_t *weights);
int beer_steam_networking_init_auth(void);
int beer_steam_networking_auth_status(void *details);
uint32_t beer_steam_networking_poll_group(void);
int beer_steam_networking_destroy_poll_group(uint32_t handle);
int beer_steam_networking_set_poll_group(uint32_t connection, uint32_t group);
int beer_steam_networking_receive_poll_group(uint32_t group, void **messages, int count);
void beer_steam_networking_release_message(void *message);

int beer_steam_networking_set_config(int value, int scope, intptr_t object, int type, const void *argument);
int beer_steam_networking_get_config(int value, int scope, intptr_t object, int *type, void *result, size_t *size);
int64_t beer_steam_networking_timestamp(void);
#endif
