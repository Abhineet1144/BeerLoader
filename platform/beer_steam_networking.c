#include "beer_steam_networking.h"
#include "steam_networking_identity.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static void *library, *sockets;
static int attempted;
static uint32_t (*listen_p2p)(void *, int, int, const void *);
static bool (*close_listener)(void *, uint32_t);
static uint32_t (*create_group)(void *);
static bool (*destroy_group)(void *, uint32_t);
static bool (*set_group)(void *, uint32_t, uint32_t);
static int (*receive_group)(void *, uint32_t, void **, int);

int beer_steam_networking_start(uint64_t steamid)
{
    pthread_mutex_lock(&lock);
    if (!attempted) {
        attempted = 1;
        const char *path = getenv("BEER_STEAM_NETWORKING_LIBRARY");
        if (path && *path && steamid) {
            library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
            bool (*init)(const void *, char *) = NULL;
            void *(*get_sockets)(void) = NULL;
#define LOAD(var, name) do { *(void **)(&(var)) = library ? dlsym(library, name) : NULL; } while (0)
            LOAD(init, "GameNetworkingSockets_Init");
            LOAD(get_sockets, "SteamNetworkingSockets_LibV12");
            LOAD(listen_p2p, "SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P");
            LOAD(close_listener, "SteamAPI_ISteamNetworkingSockets_CloseListenSocket");
            LOAD(create_group, "SteamAPI_ISteamNetworkingSockets_CreatePollGroup");
            LOAD(destroy_group, "SteamAPI_ISteamNetworkingSockets_DestroyPollGroup");
            LOAD(set_group, "SteamAPI_ISteamNetworkingSockets_SetConnectionPollGroup");
            LOAD(receive_group, "SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnPollGroup");
#undef LOAD
            unsigned char identity[136];
            char error[1024] = "missing GameNetworkingSockets entry point";
            beer_steam_networking_identity(identity, steamid);
            if (init && get_sockets && listen_p2p && close_listener && create_group && destroy_group &&
                set_group && receive_group && init(identity, error))
                sockets = get_sockets();
            if (sockets)
                fprintf(stderr, "[STEAM-NET] GameNetworkingSockets local listeners enabled; remote Steam signaling/relay authentication unavailable\n");
            else
                fprintf(stderr, "[STEAM-NET] local transport unavailable: %s\n", library ? error : dlerror());
        }
    }
    int ready = sockets != NULL;
    pthread_mutex_unlock(&lock);
    return ready;
}

uint32_t beer_steam_networking_listen(int port, int count, const void *options)
{
    if (!sockets || port < 0 || port > 65535 || count < 0 || (count && !options)) return 0;
    /* Callback configuration holds guest Win64 function pointers. The native
     * backend cannot invoke these directly; reject rather than cross the ABI. */
    const unsigned char *p = options;
    for (int i = 0; i < count; ++i, p += 16) {
        int32_t key;
        memcpy(&key, p, 4);
        if (key >= 201 && key <= 207) return 0;
    }
    uint32_t handle = listen_p2p(sockets, port, count, options);
    if (getenv("BEER_STEAM_DIAGNOSTICS"))
        fprintf(stderr, "[STEAM-NET] CreateListenSocketP2P port=%d -> %u\n", port, handle);
    return handle;
}
int beer_steam_networking_close_listener(uint32_t h) { return sockets && close_listener(sockets, h); }
uint32_t beer_steam_networking_poll_group(void) { return sockets ? create_group(sockets) : 0; }
int beer_steam_networking_destroy_poll_group(uint32_t h) { return sockets && destroy_group(sockets, h); }
int beer_steam_networking_set_poll_group(uint32_t c, uint32_t g) { return sockets && set_group(sockets, c, g); }
int beer_steam_networking_receive_poll_group(uint32_t group, void **messages, int count)
{
    if (!sockets || count < 0 || (count && !messages)) return -1;
    /* No connection creation or external signaling is exposed by this adapter,
     * so groups cannot contain native message objects with a SysV Release(). */
    return receive_group(sockets, group, messages, count);
}