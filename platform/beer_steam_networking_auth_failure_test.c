/* Offline fail-closed test: no Steam provider, remote host, or usable certificate. */
#include "beer_steam_networking.h"
#include "steam_networking_identity.h"
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int send_signal(uint32_t app, uint64_t peer, const void *body, size_t len)
{ (void)app; (void)peer; (void)body; (void)len; abort(); }
static long next_signal(uint64_t *src, uint64_t *dest, uint32_t *app, int *failed, uint8_t *out, size_t cap)
{ (void)src; (void)dest; (void)app; (void)failed; (void)out; (void)cap; return -1; }
static long refuse_config(uint32_t app, uint8_t *out, size_t cap, char *err, size_t errlen)
{ (void)app; (void)out; (void)cap; snprintf(err, errlen, "offline fixture refusal"); return -1; }
static long refuse_certificate(uint32_t app, const uint8_t key[32], uint8_t *out, size_t cap, char *err, size_t errlen)
{ (void)app; (void)key; (void)out; (void)cap; snprintf(err, errlen, "offline fixture refusal"); return -1; }
int main(void)
{
    const char *path = getenv("BEER_STEAM_NETWORKING_LIBRARY");
    void *lib = path ? dlopen(path, RTLD_NOW | RTLD_LOCAL) : NULL;
    if (!lib || !dlsym(lib, "BeerGNS_AuthBridgeVersion")) {
        puts("Steam authentication failure fixture skipped: extended runtime unavailable");
        if (lib) dlclose(lib);
        return 0;
    }
    const uint64_t self = UINT64_C(76561198000000001);
    assert(beer_steam_networking_start(self));
    const BeerP2PSignaling transport = {send_signal, next_signal, refuse_certificate, refuse_config};
    assert(beer_steam_networking_set_signaling(480, &transport));
    unsigned char identity[136], address[18] = {0};
    beer_steam_networking_identity(identity, UINT64_C(76561198000000002));
    assert(beer_steam_networking_auth_status(NULL) != 100);
    assert(!beer_steam_networking_connect_p2p(identity, 27015, 0, NULL));
    assert(!beer_steam_networking_listen(27015, 0, NULL));
    assert(!beer_steam_networking_connect_ip(address, 0, NULL));
    assert(!beer_steam_networking_listen_ip(address, 0, NULL));
    puts("Steam authentication refusal: no self-signed connection/listener fallback PASS");
    return 0;
}