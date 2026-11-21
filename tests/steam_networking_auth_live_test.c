/* Explicitly opt-in: authenticate through the real signed-in Steam provider, without contacting
 * any remote player. Checks actual certificate installation, status events and negative controls. */
#define _GNU_SOURCE
#include "../platform/beer_steam_networking.h"
#include "../platform/beer_steam_session.h"
#include "../steam/pb.h"
#include <assert.h>
#include <dlfcn.h>
#include <openssl/evp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    const char *path = getenv("BEER_STEAM_NETWORKING_LIBRARY");
    if (!path) { puts("real Steam authentication test requires an explicit runtime path"); return 2; }
    void *library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    assert(library);
    void *(*get_sockets)(void) = dlsym(library, "SteamNetworkingSockets_LibV12");
    bool (*get_request)(void *, int *, void *, char *) = dlsym(library, "SteamAPI_ISteamNetworkingSockets_GetCertificateRequest");
    bool (*install)(void *, uint32_t, const void *, int, char *) = dlsym(library, "BeerGNS_SetSteamCertificate");
    assert(get_sockets && get_request && install);
    char error[1024] = "";
    assert(beer_steam_session_open(480, error, sizeof(error)));
    assert(beer_steam_networking_start(beer_steam_session_steamid()));
    const BeerP2PSignaling transport = {
        beer_steam_session_p2p_send, beer_steam_session_next_p2p_event,
        beer_steam_session_networking_certificate, beer_steam_session_networking_config
    };
    assert(beer_steam_networking_set_signaling(480, &transport));
    unsigned char auth[BEER_STEAM_AUTH_SIZE] = {0};
    assert(beer_steam_networking_auth_status(auth) == 100); /* Current, from real GNS state */
    assert(beer_steam_networking_next_auth(auth));
    int32_t availability;
    memcpy(&availability, auth, 4);
    assert(availability == 100);
    assert(!beer_steam_networking_next_auth(auth));

    unsigned char request[512], certificate[8192];
    int size = sizeof(request);
    assert(get_request(get_sockets(), &size, request, error));
    PbField cert, key;
    assert(pb_find(request, size, 1, &cert) && pb_find(cert.data, cert.len, 2, &key) && key.len == 32);
    long n = beer_steam_session_networking_certificate(480, key.data, certificate, sizeof(certificate), error, sizeof(error));
    assert(n > 0);
    assert(install(get_sockets(), 480, certificate, (int)n, error));
    assert(!install(get_sockets(), 481, certificate, (int)n, error));
    assert(strstr(error, "app identity"));
    certificate[n - 1] ^= 1;
    assert(!install(get_sockets(), 480, certificate, (int)n, error)); /* signed data or signature altered */
    certificate[n - 1] ^= 1;
    assert(beer_steam_networking_auth_status(auth) == 100); /* rejected cert did not replace the valid one */

    /* Ask Steam to sign an independently generated PUBLIC key. It is a real, correctly signed
     * certificate for our account/app, but must not replace the instance's key. */
    EVP_PKEY *other_key = EVP_PKEY_Q_keygen(NULL, NULL, "ED25519");
    assert(other_key);
    unsigned char public_key[32];
    size_t public_size = sizeof(public_key);
    assert(EVP_PKEY_get_raw_public_key(other_key, public_key, &public_size) && public_size == 32);
    EVP_PKEY_free(other_key);
    n = beer_steam_session_networking_certificate(480, public_key, certificate, sizeof(certificate), error, sizeof(error));
    assert(n > 0);
    assert(!install(get_sockets(), 480, certificate, (int)n, error));
    assert(strstr(error, "public key"));
    assert(beer_steam_networking_auth_status(auth) == 100);
    puts("real Steam authentication: verified cert/app/key, Current status/callback, tamper and key-mismatch controls PASS");
    return 0;
}