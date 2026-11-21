/* Steam networking (ISteamNetworkingSockets012 / ISteamNetworkingUtils004) on the GameNetworkingSockets library.
 *
 * What the Steam client does for a running game, and what this module does for Beer:
 * - Connection setup between two players (P2P rendezvous) travels through Steam. The library produces the
 *   rendezvous blobs through its custom-signaling interface; Beer hands them to the injected transport, which sends
 *   them as CMsgClientP2PConnectionInfo through the signed-in session, and feeds the blobs Steam delivers back in.
 * - Data flows peer to peer over ICE (host and STUN-reflexive candidates). The optional first-party runtime
 *   bridge imports Valve-root-verified CA chains, sets the app identity and installs Steam-issued certificates.
 *   Unextended libraries retain custom/self-signed peer support only. Valve's relay transport (SDR) is not implemented.
 * - Connection status changes become SteamNetConnectionStatusChangedCallback_t events (Win64 layout), handed out by
 *   beer_steam_networking_next_status for the loader to post or dispatch.
 *
 * Structure layouts are the same in the library's Linux build and the Win64 SDK (SteamNetworkingIdentity,
 * SteamNetConnectionInfo_t, the real-time status structs, SteamNetworkingMessage_t, SteamNetworkingConfigValue_t),
 * except the status callback, whose m_info is at 4 (4-byte packing) natively and at 8 for Win64. Message objects
 * carry SysV function pointers; the loader copies them into Win64 objects (see win32_steam_networking.inc).
 * Portable C: no loader or guest ABI dependency. */
#include "beer_steam_networking.h"
#include "steam_networking_identity.h"
#include "steam_stun_resolver.h"
#include "../steam/pb.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static void *library, *sockets, *utils;
static int attempted;
static uint64_t self_steamid;

#define FN(name, ret, ...) static ret (*name)(__VA_ARGS__)
FN(listen_p2p, uint32_t, void *, int, int, const void *);
FN(listen_ip, uint32_t, void *, const void *, int, const void *);
FN(connect_ip, uint32_t, void *, const void *, int, const void *);
FN(connect_custom, uint32_t, void *, void *, const void *, int, int, const void *);
FN(accept_conn, int, void *, uint32_t);
FN(close_conn, bool, void *, uint32_t, int, const char *, bool);
FN(close_listener, bool, void *, uint32_t);
FN(set_user_data, bool, void *, uint32_t, int64_t);
FN(get_user_data, int64_t, void *, uint32_t);
FN(set_name, void, void *, uint32_t, const char *);
FN(get_name, bool, void *, uint32_t, char *, int);
FN(send_to, int, void *, uint32_t, const void *, uint32_t, int, int64_t *);
FN(flush, int, void *, uint32_t);
FN(receive_conn, int, void *, uint32_t, void **, int);
FN(get_info, bool, void *, uint32_t, void *);
FN(get_realtime, int, void *, uint32_t, void *, int, void *);
FN(get_detailed, int, void *, uint32_t, char *, int);
FN(get_listen_addr, bool, void *, uint32_t, void *);
FN(socket_pair, bool, void *, uint32_t *, uint32_t *, bool, const void *, const void *);
FN(configure_lanes, int, void *, uint32_t, int, const int *, const uint16_t *);
FN(init_auth, int, void *);
FN(get_auth_status, int, void *, void *);
FN(get_cert_request, bool, void *, int *, void *, char *);
FN(configure_steam, bool, void *, uint32_t, const void *, int, char *);
FN(install_steam_cert, bool, void *, uint32_t, const void *, int, char *);
FN(create_group, uint32_t, void *);
FN(destroy_group, bool, void *, uint32_t);
FN(set_group, bool, void *, uint32_t, uint32_t);
FN(receive_group, int, void *, uint32_t, void **, int);
FN(run_callbacks, void, void *);
FN(create_signaling, void *, void *, bool (*)(void *, uint32_t, const void *, const void *, int), void (*)(void *));
FN(received_signal, bool, void *, const void *, int, void *, void *(*)(void *, uint32_t, const void *, int),
   void (*)(void *, const void *, const void *, int));
FN(set_status_callback, bool, void *, void (*)(void *));
FN(set_config, bool, void *, int, int, intptr_t, int, const void *);
FN(get_config, int, void *, int, int, intptr_t, int *, void *, size_t *);
FN(get_timestamp, int64_t, void *);
FN(set_debug, void, void *, int, void (*)(int, const char *));
FN(release_message, void, void *);
#undef FN

/* ---- signaling ---- */
static BeerP2PSignaling transport;
static uint32_t signaling_appid;
static int service_running;
static pthread_once_t stun_once = PTHREAD_ONCE_INIT;
static char *numeric_stun;
static int trust_configured;
static time_t cert_expiry, auth_retry;
static time_t config_refresh;
static atomic_bool steam_auth_required;

static int peer_authentication_ready(void)
{
    return !atomic_load(&steam_auth_required) ||
           (sockets && get_auth_status(sockets, NULL) == 100 /* Current */);
}

static void resolve_stun_once(void)
{
    const char *configured = getenv("BEER_STEAM_STUN_SERVERS");
    char error[512];
    if (!beer_steam_stun_resolve(configured ? configured : "stun.l.google.com:19302",
                                &numeric_stun, error, sizeof(error)))
        fprintf(stderr, "[STEAM-NET] STUN preflight failed: %s\n", error);
}

static int diagnostics(void)
{
    static int value = -1;
    if (value < 0) value = getenv("BEER_STEAM_DIAGNOSTICS") != NULL;
    return value;
}

/* The SteamID of an identity (k_ESteamNetworkingIdentityType_SteamID = 16), 0 for any other kind. */
static uint64_t identity_steamid(const unsigned char *identity)
{
    uint32_t type;
    uint64_t id;
    memcpy(&type, identity, 4);
    memcpy(&id, identity + 8, 8);
    return type == 16 ? id : 0;
}

/* ISteamNetworkingConnectionSignaling::SendSignal: the remote identity is the first field of the info. */
static bool send_signal(void *context, uint32_t connection, const void *info, const void *message, int length)
{
    (void)context;
    const uint64_t peer = identity_steamid(info);
    if (!peer || !transport.send || length <= 0) return false;
    if (diagnostics())
        fprintf(stderr, "[STEAM-NET] signal for connection %u -> %llu (%d bytes)\n", connection,
                (unsigned long long)peer, length);
    return transport.send(signaling_appid, peer, message, (size_t)length) != 0;
}

static void *on_connect_request(void *context, uint32_t connection, const void *identity, int port)
{
    (void)context;
    if (!peer_authentication_ready()) return NULL;
    fprintf(stderr, "[STEAM-NET] connection request %u from %llu on virtual port %d\n", connection,
            (unsigned long long)identity_steamid(identity), port);
    return create_signaling(NULL, send_signal, NULL);
}

static void send_rejection(void *context, const void *identity, const void *message, int length)
{
    (void)context;
    const uint64_t peer = identity_steamid(identity);
    if (peer && transport.send && length > 0) transport.send(signaling_appid, peer, message, (size_t)length);
}

/* ---- status events ---- */
#define STATUS_QUEUE 256
static unsigned char status_queue[STATUS_QUEUE][BEER_STEAM_STATUS_SIZE];
static unsigned status_head, status_count;
static pthread_mutex_t status_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char auth_queue[16][BEER_STEAM_AUTH_SIZE];
static unsigned auth_head, auth_count;
static unsigned char last_auth[BEER_STEAM_AUTH_SIZE];
static int have_last_auth;

static void on_auth_changed(void *native)
{
    pthread_mutex_lock(&status_lock);
    if (have_last_auth && !memcmp(last_auth, native, sizeof(last_auth))) {
        pthread_mutex_unlock(&status_lock);
        return;
    }
    memcpy(last_auth, native, sizeof(last_auth));
    have_last_auth = 1;
    if (auth_count == 16) { auth_head = (auth_head + 1) % 16; --auth_count; }
    memcpy(auth_queue[(auth_head + auth_count++) % 16], native, BEER_STEAM_AUTH_SIZE);
    pthread_mutex_unlock(&status_lock);
}

int beer_steam_networking_next_auth(void *out)
{
    if (!out) return 0;
    pthread_mutex_lock(&status_lock);
    int have = auth_count != 0;
    if (have) {
        memcpy(out, auth_queue[auth_head], BEER_STEAM_AUTH_SIZE);
        auth_head = (auth_head + 1) % 16;
        --auth_count;
    }
    pthread_mutex_unlock(&status_lock);
    return have;
}

/* No network requests under GNS's global lock. Renewal is on its own thread, so CM/API latency
 * cannot delay ICE signaling or connection status callbacks. The private key never leaves GNS. */
static void poll_authentication_status(void)
{
    unsigned char status[BEER_STEAM_AUTH_SIZE] = {0};
    get_auth_status(sockets, status);
    on_auth_changed(status);
}

static int authenticate_steam(void)
{
    char error[1024] = "invalid networking certificate request";
    if (!trust_configured || time(NULL) >= config_refresh) {
        unsigned char *config = malloc(1024 * 1024);
        long length = config ? transport.config(signaling_appid, config, 1024 * 1024, error, sizeof(error)) : -1;
        int ok = length > 0 && length <= 1024 * 1024 && configure_steam(sockets, signaling_appid, config, (int)length, error);
        free(config);
        if (!ok) goto failed;
        trust_configured = 1;
        config_refresh = time(NULL) + 3600;
        fprintf(stderr, "[STEAM-NET] Valve certificate trust chain verified; app identity %u configured\n", signaling_appid);
    }
    unsigned char request[512], certificate[8192];
    int size = sizeof(request);
    PbField cert, key, expiry;
    int valid = get_cert_request(sockets, &size, request, error) && size > 0 && size <= (int)sizeof(request) &&
                pb_find(request, (size_t)size, 1, &cert) && cert.type == PB_BYTES &&
                pb_find(cert.data, cert.len, 2, &key) && key.type == PB_BYTES && key.len == 32;
    long length = valid ? transport.certificate(signaling_appid, key.data, certificate, sizeof(certificate), error, sizeof(error)) : -1;
    if (length <= 0 || length > (long)sizeof(certificate) ||
        !install_steam_cert(sockets, signaling_appid, certificate, (int)length, error)) goto failed;
    cert_expiry = 0;
    if (pb_find(certificate, (size_t)length, 4, &cert) && cert.type == PB_BYTES &&
        pb_find(cert.data, cert.len, 9, &expiry) && expiry.type == PB_FIXED32) cert_expiry = (time_t)expiry.value;
    auth_retry = time(NULL) + 60;
    fprintf(stderr, "[STEAM-NET] Steam-issued certificate verified and installed for app %u (expiry %lld); SDR transport still unavailable\n",
            signaling_appid, (long long)cert_expiry);
            poll_authentication_status();
    return 1;
failed:
    auth_retry = time(NULL) + 60;
    fprintf(stderr, "[STEAM-NET] Steam networking authentication failed: %s\n", error);
    poll_authentication_status();
    return 0;
}

static void *authentication_service(void *arg)
{
    (void)arg;
    for (;;) {
        time_t now = time(NULL);
        if (now >= auth_retry && (!cert_expiry || cert_expiry <= now + 300 || now >= config_refresh)) authenticate_steam();
        /* Open-source GNS dispatches authentication callbacks only with its SDR build flag.
         * Query the real status instead of enabling that unsupported callback path. */
        poll_authentication_status();
        struct timespec pause = { 1, 0 };
        nanosleep(&pause, NULL);
    }
    return NULL;
}

/* SteamNetConnectionStatusChangedCallback_t, native 4-byte packing: m_hConn @0, m_info @4 (696), m_eOldState @700.
 * Win64 (8-byte packing): m_hConn @0, m_info @8, m_eOldState @704, 712 bytes. */
static void on_status_changed(void *native)
{
    const unsigned char *in = native;
    unsigned char out[BEER_STEAM_STATUS_SIZE];
    memset(out, 0, sizeof(out));
    memcpy(out, in, 4);
    memcpy(out + 8, in + 4, 696);
    memcpy(out + 704, in + 700, 4);
    int32_t state, old_state;
    memcpy(&state, in + 4 + 176, 4);
    memcpy(&old_state, in + 700, 4);
    uint32_t connection;
    memcpy(&connection, in, 4);
    char debug[129] = "";
    memcpy(debug, in + 4 + 184, 128);
    fprintf(stderr, "[STEAM-NET] connection %u with %llu: state %d -> %d%s%s\n", connection,
            (unsigned long long)identity_steamid(in + 4), old_state, state, debug[0] ? " " : "", debug);
    pthread_mutex_lock(&status_lock);
    if (status_count == STATUS_QUEUE) {                      /* full: the oldest is lost, as in a full queue */
        status_head = (status_head + 1) % STATUS_QUEUE;
        status_count--;
    }
    memcpy(status_queue[(status_head + status_count) % STATUS_QUEUE], out, sizeof(out));
    status_count++;
    pthread_mutex_unlock(&status_lock);
}

int beer_steam_networking_next_status(void *out)
{
    pthread_mutex_lock(&status_lock);
    const int have = status_count != 0;
    if (have) {
        memcpy(out, status_queue[status_head], BEER_STEAM_STATUS_SIZE);
        status_head = (status_head + 1) % STATUS_QUEUE;
        status_count--;
    }
    pthread_mutex_unlock(&status_lock);
    return have;
}

static void debug_output(int type, const char *text)
{
    /* Upstream verbose reliable-message dumps include temporary ICE passwords. */
    if (text && strstr(text, "pwd_frag")) {
        fprintf(stderr, "[STEAM-NET] (%d) ICE authentication signaling (credentials omitted)\n", type);
        return;
    }
    fprintf(stderr, "[STEAM-NET] (%d) %s\n", type, text ? text : "");
}

/* The service thread: delivers signaling that arrived through Steam and runs the library's callbacks, as the
 * Steam client's networking thread does; status events wait in the queue for the game's callback pump. */
static void *service(void *arg)
{
    (void)arg;
    unsigned char *blob = malloc(65536);
    if (!blob) return NULL;
    for (;;) {
        uint64_t src = 0, dest = 0;
        uint32_t appid = 0;
        int failed = 0;
        long length;
        while ((length = transport.next(&src, &dest, &appid, &failed, blob, 65536)) >= 0) {
            if (appid != signaling_appid || (dest && dest != self_steamid)) continue;
            if (failed) {
                uint32_t reason = 0;
                if (length >= 4) memcpy(&reason, blob, 4);
                fprintf(stderr, "[STEAM-NET] Steam could not deliver signaling to %llu (reason %u)\n",
                        (unsigned long long)src, reason);
                continue;
            }
            const bool ok = received_signal(sockets, blob, (int)length, NULL, on_connect_request, send_rejection);
            if (diagnostics() || !ok)
                fprintf(stderr, "[STEAM-NET] signal from %llu (%ld bytes)%s\n", (unsigned long long)src, length,
                        ok ? "" : " rejected by the library");
        }
        run_callbacks(sockets);
        struct timespec pause = { 0, 2 * 1000 * 1000 };
        nanosleep(&pause, NULL);
    }
    return NULL;
}

int beer_steam_networking_start(uint64_t steamid)
{
    const char *configured_library = getenv("BEER_STEAM_NETWORKING_LIBRARY");
    if (!steamid || !configured_library || !*configured_library) return 0;
    /* DNS must not run inside GNS's global lock: the installed ICE runtime synchronously resolves
     * hostnames there, consuming the initial connection deadline before sending any rendezvous. */
    pthread_once(&stun_once, resolve_stun_once);
    if (!numeric_stun) return 0;
    pthread_mutex_lock(&lock);
    if (!attempted) {
        attempted = 1;
        const char *path = getenv("BEER_STEAM_NETWORKING_LIBRARY");
        if (path && *path && steamid) {
            library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
            bool (*init)(const void *, char *) = NULL;
            void *(*get_sockets)(void) = NULL, *(*get_utils)(void) = NULL;
            int missing = 0;
#define LOAD(var, name) do { *(void **)(&(var)) = library ? dlsym(library, name) : NULL; if (!(var)) missing = 1; } while (0)
            LOAD(init, "GameNetworkingSockets_Init");
            LOAD(get_sockets, "SteamNetworkingSockets_LibV12");
            LOAD(get_utils, "SteamNetworkingUtils_LibV4");
            LOAD(listen_p2p, "SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P");
            LOAD(listen_ip, "SteamAPI_ISteamNetworkingSockets_CreateListenSocketIP");
            LOAD(connect_ip, "SteamAPI_ISteamNetworkingSockets_ConnectByIPAddress");
            LOAD(connect_custom, "SteamAPI_ISteamNetworkingSockets_ConnectP2PCustomSignaling");
            LOAD(accept_conn, "SteamAPI_ISteamNetworkingSockets_AcceptConnection");
            LOAD(close_conn, "SteamAPI_ISteamNetworkingSockets_CloseConnection");
            LOAD(close_listener, "SteamAPI_ISteamNetworkingSockets_CloseListenSocket");
            LOAD(set_user_data, "SteamAPI_ISteamNetworkingSockets_SetConnectionUserData");
            LOAD(get_user_data, "SteamAPI_ISteamNetworkingSockets_GetConnectionUserData");
            LOAD(set_name, "SteamAPI_ISteamNetworkingSockets_SetConnectionName");
            LOAD(get_name, "SteamAPI_ISteamNetworkingSockets_GetConnectionName");
            LOAD(send_to, "SteamAPI_ISteamNetworkingSockets_SendMessageToConnection");
            LOAD(flush, "SteamAPI_ISteamNetworkingSockets_FlushMessagesOnConnection");
            LOAD(receive_conn, "SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnConnection");
            LOAD(get_info, "SteamAPI_ISteamNetworkingSockets_GetConnectionInfo");
            LOAD(get_realtime, "SteamAPI_ISteamNetworkingSockets_GetConnectionRealTimeStatus");
            LOAD(get_detailed, "SteamAPI_ISteamNetworkingSockets_GetDetailedConnectionStatus");
            LOAD(get_listen_addr, "SteamAPI_ISteamNetworkingSockets_GetListenSocketAddress");
            LOAD(socket_pair, "SteamAPI_ISteamNetworkingSockets_CreateSocketPair");
            LOAD(configure_lanes, "SteamAPI_ISteamNetworkingSockets_ConfigureConnectionLanes");
            LOAD(init_auth, "SteamAPI_ISteamNetworkingSockets_InitAuthentication");
            LOAD(get_auth_status, "SteamAPI_ISteamNetworkingSockets_GetAuthenticationStatus");
            LOAD(get_cert_request, "SteamAPI_ISteamNetworkingSockets_GetCertificateRequest");
            LOAD(create_group, "SteamAPI_ISteamNetworkingSockets_CreatePollGroup");
            LOAD(destroy_group, "SteamAPI_ISteamNetworkingSockets_DestroyPollGroup");
            LOAD(set_group, "SteamAPI_ISteamNetworkingSockets_SetConnectionPollGroup");
            LOAD(receive_group, "SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnPollGroup");
            LOAD(run_callbacks, "SteamAPI_ISteamNetworkingSockets_RunCallbacks");
            LOAD(create_signaling, "SteamAPI_ISteamNetworkingSockets_CreateCustomSignaling");
            LOAD(received_signal, "SteamAPI_ISteamNetworkingSockets_ReceivedP2PCustomSignal2");
            LOAD(set_status_callback, "SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamNetConnectionStatusChanged");
            LOAD(set_config, "SteamAPI_ISteamNetworkingUtils_SetConfigValue");
            LOAD(get_config, "SteamAPI_ISteamNetworkingUtils_GetConfigValue");
            LOAD(get_timestamp, "SteamAPI_ISteamNetworkingUtils_GetLocalTimestamp");
            LOAD(set_debug, "SteamAPI_ISteamNetworkingUtils_SetDebugOutputFunction");
            LOAD(release_message, "SteamAPI_SteamNetworkingMessage_t_Release");
#undef LOAD
            /* Optional first-party ABI extension: a stock library remains supported for custom peers. */
            unsigned (*bridge_version)(void) = library ? dlsym(library, "BeerGNS_AuthBridgeVersion") : NULL;
            if (bridge_version && bridge_version() == 1) {
                *(void **)(&configure_steam) = dlsym(library, "BeerGNS_ConfigureSteam");
                *(void **)(&install_steam_cert) = dlsym(library, "BeerGNS_SetSteamCertificate");
            }
            unsigned char identity[136];
            char error[1024] = "missing GameNetworkingSockets entry point";
            beer_steam_networking_identity(identity, steamid);
            if (!missing && init(identity, error)) {
                sockets = get_sockets();
                utils = get_utils();
            }
            if (sockets && utils) {
                self_steamid = steamid;
                set_debug(utils, diagnostics() ? 6 : 2, debug_output);   /* Verbose with diagnostics, else errors */
                if (diagnostics()) {
                    const int verbose = 6;
                    set_config(utils, 17 /* LogLevel_P2PRendezvous */, 1, 0, 1, &verbose);
                }
                set_status_callback(utils, on_status_changed);
                /* STUN for the reflexive ICE candidates players behind NAT need; the Steam client uses Valve's own
                 * servers. BEER_STEAM_STUN_SERVERS overrides (comma separated, empty for none). */
                set_config(utils, BEER_NET_CONFIG_P2P_STUN_SERVER_LIST, 1 /* global */, 0, 4 /* string */,
                           numeric_stun);
                fprintf(stderr, "[STEAM-NET] GameNetworkingSockets ready for %llu\n", (unsigned long long)steamid);
            } else {
                sockets = utils = NULL;
                fprintf(stderr, "[STEAM-NET] networking library unavailable: %s\n", library ? error : dlerror());
            }
        }
    }
    int ready = sockets != NULL;
    pthread_mutex_unlock(&lock);
    return ready;
}

int beer_steam_networking_set_signaling(uint32_t appid, const BeerP2PSignaling *signaling)
{
    if (!sockets || !signaling || !signaling->send || !signaling->next) return 0;
    pthread_mutex_lock(&lock);
    int ok = 1;
    if (!service_running) {
        transport = *signaling;
        signaling_appid = appid;
        int authenticated_backend = transport.certificate && transport.config && configure_steam && install_steam_cert;
        atomic_store(&steam_auth_required, authenticated_backend != 0);
        if (authenticated_backend) {
            /* Steam peers must authenticate, including localhost. Do not silently replace a failed
             * Steam certificate request with an unauthenticated success-shaped connection. */
            const int require_auth = 0;
            set_config(utils, 23 /* IP_AllowWithoutAuth */, 1, 0, 1, &require_auth);
            set_config(utils, 52 /* IPLocalHost_AllowWithoutAuth */, 1, 0, 1, &require_auth);
            authenticate_steam();
        }
        else fprintf(stderr, "[STEAM-NET] custom ICE backend: Steam authentication bridge unavailable; stock Steam multiplayer is not supported\n");
        pthread_t thread;
        ok = pthread_create(&thread, NULL, service, NULL) == 0;
        if (ok) { pthread_detach(thread); service_running = 1; }
        if (ok && authenticated_backend) {
            if (!pthread_create(&thread, NULL, authentication_service, NULL)) pthread_detach(thread);
            else fprintf(stderr, "[STEAM-NET] certificate renewal thread could not start\n");
        }
    }
    pthread_mutex_unlock(&lock);
    if (ok) fprintf(stderr, "[STEAM-NET] peer-to-peer connections through Steam signaling for app %u\n", appid);
    return ok;
}
int beer_steam_networking_p2p_ready(void) { return sockets && service_running; }

/* ---- ISteamNetworkingSockets ---- */
uint32_t beer_steam_networking_listen(int port, int count, const void *options)
{
    if (!sockets || port < 0 || port > 65535 || count < 0 || (count && !options)) return 0;
    if (!peer_authentication_ready()) return 0;
    uint32_t handle = listen_p2p(sockets, port, count, options);
    if (diagnostics()) fprintf(stderr, "[STEAM-NET] CreateListenSocketP2P port=%d -> %u\n", port, handle);
    return handle;
}
uint32_t beer_steam_networking_listen_ip(const void *address, int count, const void *options)
{ return sockets && address && count >= 0 && peer_authentication_ready() ? listen_ip(sockets, address, count, options) : 0; }
uint32_t beer_steam_networking_connect_ip(const void *address, int count, const void *options)
{ return sockets && address && count >= 0 && peer_authentication_ready() ? connect_ip(sockets, address, count, options) : 0; }
uint32_t beer_steam_networking_connect_p2p(const void *identity, int port, int count, const void *options)
{
    if (!sockets || !identity || count < 0 || !service_running) return 0;
    if (!peer_authentication_ready()) {
        fprintf(stderr, "[STEAM-NET] ConnectP2P refused: Steam authentication is not Current\n");
        return 0;
    }
    const uint64_t peer = identity_steamid(identity);
    void *signaling = create_signaling(NULL, send_signal, NULL);
    uint32_t handle = signaling ? connect_custom(sockets, signaling, identity, port, count, options) : 0;
    fprintf(stderr, "[STEAM-NET] ConnectP2P(%llu, virtual port %d) -> %u\n", (unsigned long long)peer, port, handle);
    return handle;
}
int beer_steam_networking_accept(uint32_t c) { return sockets ? accept_conn(sockets, c) : 2; }   /* k_EResultFail */
int beer_steam_networking_close(uint32_t c, int reason, const char *debug, int linger)
{ return sockets && close_conn(sockets, c, reason, debug, linger != 0); }
int beer_steam_networking_close_listener(uint32_t h) { return sockets && close_listener(sockets, h); }
int beer_steam_networking_set_user_data(uint32_t c, int64_t v) { return sockets && set_user_data(sockets, c, v); }
int64_t beer_steam_networking_get_user_data(uint32_t c) { return sockets ? get_user_data(sockets, c) : -1; }
void beer_steam_networking_set_name(uint32_t c, const char *name) { if (sockets && name) set_name(sockets, c, name); }
int beer_steam_networking_get_name(uint32_t c, char *out, int cap) { return sockets && out && get_name(sockets, c, out, cap); }
int beer_steam_networking_send(uint32_t c, const void *data, uint32_t size, int flags, int64_t *number)
{ return sockets ? send_to(sockets, c, data, size, flags, number) : 3; }                       /* k_EResultNoConnection */
int beer_steam_networking_flush(uint32_t c) { return sockets ? flush(sockets, c) : 3; }
int beer_steam_networking_receive(uint32_t c, void **messages, int count)
{ return sockets && count >= 0 && (messages || !count) ? receive_conn(sockets, c, messages, count) : -1; }
int beer_steam_networking_info(uint32_t c, void *info) { return sockets && info && get_info(sockets, c, info); }
int beer_steam_networking_realtime(uint32_t c, void *status, int lanes, void *lane_status)
{ return sockets ? get_realtime(sockets, c, status, lanes, lane_status) : 3; }
int beer_steam_networking_detailed(uint32_t c, char *out, int cap) { return sockets ? get_detailed(sockets, c, out, cap) : -1; }
int beer_steam_networking_listen_address(uint32_t h, void *address) { return sockets && address && get_listen_addr(sockets, h, address); }
int beer_steam_networking_socket_pair(uint32_t *a, uint32_t *b, int loopback, const void *id1, const void *id2)
{ return sockets && a && b && socket_pair(sockets, a, b, loopback != 0, id1, id2); }
int beer_steam_networking_configure_lanes(uint32_t c, int lanes, const int *priorities, const uint16_t *weights)
{ return sockets ? configure_lanes(sockets, c, lanes, priorities, weights) : 3; }
int beer_steam_networking_init_auth(void) { return sockets ? init_auth(sockets) : -102; }     /* CannotTry */
int beer_steam_networking_auth_status(void *details) { return sockets ? get_auth_status(sockets, details) : -102; }
uint32_t beer_steam_networking_poll_group(void) { return sockets ? create_group(sockets) : 0; }
int beer_steam_networking_destroy_poll_group(uint32_t h) { return sockets && destroy_group(sockets, h); }
int beer_steam_networking_set_poll_group(uint32_t c, uint32_t g) { return sockets && set_group(sockets, c, g); }
int beer_steam_networking_receive_poll_group(uint32_t group, void **messages, int count)
{
    if (!sockets || count < 0 || (count && !messages)) return -1;
    return receive_group(sockets, group, messages, count);
}
void beer_steam_networking_release_message(void *message) { if (message && release_message) release_message(message); }

/* ---- ISteamNetworkingUtils ---- */
int beer_steam_networking_set_config(int value, int scope, intptr_t object, int type, const void *argument)
{ return utils && set_config(utils, value, scope, object, type, argument); }
int beer_steam_networking_get_config(int value, int scope, intptr_t object, int *type, void *result, size_t *size)
{ return utils ? get_config(utils, value, scope, object, type, result, size) : -1; }   /* BadValue */
int64_t beer_steam_networking_timestamp(void)
{
    if (utils) return get_timestamp(utils);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}
