/* Two processes, two Steam identities, one installed GameNetworkingSockets library: custom P2P signaling carried
 * over a socketpair (standing in for Steam's CMsgClientP2PConnectionInfo relay), ICE with host candidates only.
 * Checks that a ConnectP2PCustomSignaling connection reaches the listening peer, is accepted, connects on both sides
 * and carries a reliable message each way. Library: $BEER_STEAM_NETWORKING_LIBRARY. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "../platform/steam_networking_identity.h"

typedef struct { int32_t value, type; union { int32_t i32; int64_t i64; float f; const char *s; void *p; } v; } ConfigValue;
typedef bool (*SendSignalFn)(void *, uint32_t, const void *info, const void *msg, int len);
typedef void (*ReleaseFn)(void *);
typedef void *(*OnConnectRequestFn)(void *, uint32_t, const void *identity, int port);
typedef void (*RejectFn)(void *, const void *identity, const void *msg, int len);
typedef struct { uint32_t conn; uint8_t info[696 + 4]; int32_t old_state; } StatusLinux;   /* pack 4: info at +4 */

static void *lib, *sockets, *utils;
static int channel = -1;
static int state = 0, accepted_conn = 0;
static uint32_t listen_socket;
#define F(name, type) static __typeof__(type) name
F(init, bool (*)(const void *, char *));
F(get_sockets, void *(*)(void));
F(get_utils, void *(*)(void));
F(set_config, bool (*)(void *, int, int, intptr_t, int, const void *));
F(set_status_cb, bool (*)(void *, void (*)(StatusLinux *)));
F(create_signaling, void *(*)(void *, SendSignalFn, ReleaseFn));
F(connect_custom, uint32_t (*)(void *, void *, const void *, int, int, const ConfigValue *));
F(received_signal, bool (*)(void *, const void *, int, void *, OnConnectRequestFn, RejectFn));
F(listen_p2p, uint32_t (*)(void *, int, int, const ConfigValue *));
F(accept_conn, int (*)(void *, uint32_t));
F(run_callbacks, void (*)(void *));
F(send_msg, int (*)(void *, uint32_t, const void *, uint32_t, int, int64_t *));
F(recv_msgs, int (*)(void *, uint32_t, void **, int));
F(release_msg, void (*)(void *));
F(set_debug, void (*)(void *, int, void (*)(int, const char *)));
static void debug_output(int type, const char *text) { fprintf(stderr, "[%d] GNS(%d): %s\n", getpid(), type, text); }

static bool send_signal(void *ctx, uint32_t conn, const void *info, const void *msg, int len)
{
    (void)ctx; (void)conn; (void)info;
    uint32_t n = (uint32_t)len;
    if (getenv("P2P_TEST_VERBOSE")) fprintf(stderr, "[%d] send signal %d bytes\n", getpid(), len);
    return write(channel, &n, 4) == 4 && write(channel, msg, (size_t)len) == len;
}
static void *on_connect_request(void *ctx, uint32_t conn, const void *identity, int port)
{
    (void)ctx; (void)identity;
    fprintf(stderr, "[%d] connect request conn=%u port=%d\n", getpid(), conn, port);
    return create_signaling(NULL, send_signal, NULL);
}
static void on_status(StatusLinux *s)
{
    int32_t new_state;
    memcpy(&new_state, s->info + 176, 4);                /* m_eState */
    uint32_t listen;
    memcpy(&listen, s->info + 144, 4);
    fprintf(stderr, "[%d] conn %u: %d -> %d (listen %u)\n", getpid(), s->conn, s->old_state, new_state, listen);
    if (new_state == 1 && listen) { accept_conn(sockets, s->conn); accepted_conn = (int)s->conn; }
    if (new_state == 3) state = 3;
    if (new_state == 4 || new_state == 5) {
        char debug[129] = "";
        memcpy(debug, s->info + 184, 128);
        fprintf(stderr, "[%d] closed: %s\n", getpid(), debug);
        state = -1;
    }
}

static void pump_signals(void)
{
    struct pollfd p = { channel, POLLIN, 0 };
    while (poll(&p, 1, 0) == 1) {
        uint32_t n;
        if (read(channel, &n, 4) != 4) { state = -1; return; }
        char *buf = malloc(n);
        size_t got = 0;
        while (got < n) { ssize_t r = read(channel, buf + got, n - got); if (r <= 0) break; got += (size_t)r; }
        bool ok = received_signal(sockets, buf, (int)n, NULL, on_connect_request, NULL);
        if (getenv("P2P_TEST_VERBOSE")) fprintf(stderr, "[%d] received signal %u bytes -> %d\n", getpid(), n, ok);
        free(buf);
    }
}

static int run(uint64_t self, uint64_t peer, int connector)
{
    lib = dlopen(getenv("BEER_STEAM_NETWORKING_LIBRARY"), RTLD_NOW);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
#define L(var, name) *(void **)&var = dlsym(lib, name); if (!var) { fprintf(stderr, "missing %s\n", name); return 2; }
    L(init, "GameNetworkingSockets_Init"); L(get_sockets, "SteamNetworkingSockets_LibV12"); L(get_utils, "SteamNetworkingUtils_LibV4");
    L(set_config, "SteamAPI_ISteamNetworkingUtils_SetConfigValue");
    L(set_status_cb, "SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamNetConnectionStatusChanged");
    L(create_signaling, "SteamAPI_ISteamNetworkingSockets_CreateCustomSignaling");
    L(connect_custom, "SteamAPI_ISteamNetworkingSockets_ConnectP2PCustomSignaling");
    L(received_signal, "SteamAPI_ISteamNetworkingSockets_ReceivedP2PCustomSignal2");
    L(listen_p2p, "SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P");
    L(accept_conn, "SteamAPI_ISteamNetworkingSockets_AcceptConnection");
    L(run_callbacks, "SteamAPI_ISteamNetworkingSockets_RunCallbacks");
    L(send_msg, "SteamAPI_ISteamNetworkingSockets_SendMessageToConnection");
    L(recv_msgs, "SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnConnection");
    L(release_msg, "SteamAPI_SteamNetworkingMessage_t_Release");
    unsigned char identity[136], peer_identity[136];
    beer_steam_networking_identity(identity, self);
    beer_steam_networking_identity(peer_identity, peer);
    char error[1024];
    if (!init(identity, error)) { fprintf(stderr, "init: %s\n", error); return 2; }
    sockets = get_sockets();
    utils = get_utils();
    if (getenv("P2P_TEST_VERBOSE")) {
        L(set_debug, "SteamAPI_ISteamNetworkingUtils_SetDebugOutputFunction");
        set_debug(utils, 6, debug_output);
    }
    int32_t all = 0x7fffffff;
    set_config(utils, 104, 1, 0, 1, &all);                /* P2P_Transport_ICE_Enable = All, global, int32 */
    set_config(utils, 103, 1, 0, 4, "");                  /* P2P_STUN_ServerList: none (host candidates) */
    set_status_cb(utils, on_status);
    uint32_t conn = 0;
    if (connector) {
        usleep(200000);
        conn = connect_custom(sockets, create_signaling(NULL, send_signal, NULL), peer_identity, 7, 0, NULL);
        fprintf(stderr, "[%d] ConnectP2PCustomSignaling -> %u\n", getpid(), conn);
    } else {
        listen_socket = listen_p2p(sockets, 7, 0, NULL);
        fprintf(stderr, "[%d] listen -> %u\n", getpid(), listen_socket);
    }
    int sent = 0, received = 0;
    for (int i = 0; i < 1500 && state >= 0 && !(sent && received); ++i) {
        pump_signals();
        run_callbacks(sockets);
        uint32_t c = connector ? conn : (uint32_t)accepted_conn;
        if (state == 3 && c && !sent) {
            const char *text = connector ? "hello from connector" : "hello from listener";
            send_msg(sockets, c, text, (uint32_t)strlen(text) + 1, 8, NULL);   /* reliable */
            sent = 1;
        }
        void *msgs[4];
        int n = c ? recv_msgs(sockets, c, msgs, 4) : 0;
        for (int k = 0; k < n; ++k) {
            fprintf(stderr, "[%d] received: %s\n", getpid(), (const char *)*(void **)msgs[k]);
            release_msg(msgs[k]);
            received = 1;
        }
        usleep(10000);
    }
    for (int i = 0; i < 50; ++i) { pump_signals(); run_callbacks(sockets); usleep(10000); }  /* let the peer finish */
    return received ? 0 : 1;
}

int main(void)
{
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) return 2;
    const uint64_t a = 76561198000000001ull, b = 76561198000000002ull;
    pid_t child = fork();
    if (!child) { close(pair[0]); channel = pair[1]; _exit(run(b, a, 0)); }
    close(pair[1]);
    channel = pair[0];
    int mine = run(a, b, 1), status = 0;
    waitpid(child, &status, 0);
    const int ok = mine == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    puts(ok ? "steam p2p custom signaling: PASS" : "steam p2p custom signaling: FAIL");
    return ok ? 0 : 1;
}
