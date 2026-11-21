/* beer_steam_networking.c end to end in two processes: Steam's signaling relay is replaced by a socketpair carrying
 * (destination, blob) records. The listener's connection request arrives, is accepted, both sides reach Connected
 * through the status events (Win64 callback layout) and exchange a reliable message.
 * Needs BEER_STEAM_NETWORKING_LIBRARY; skipped without it. */
#define _GNU_SOURCE
#include "beer_steam_networking.h"
#include "steam_networking_identity.h"
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int channel = -1;
static uint64_t self_id, peer_id;
static const uint32_t appid = 480;

static int send_blob(uint32_t app, uint64_t dest, const void *body, size_t len)
{
    if (app != appid || dest != peer_id) return 0;
    uint32_t n = (uint32_t)len;
    return write(channel, &self_id, 8) == 8 && write(channel, &n, 4) == 4 && write(channel, body, len) == (ssize_t)len;
}
static long next_blob(uint64_t *src, uint64_t *dest, uint32_t *app, int *failed, uint8_t *out, size_t cap)
{
    struct pollfd p = { channel, POLLIN, 0 };
    if (poll(&p, 1, 0) != 1) return -1;
    uint32_t n;
    if (read(channel, src, 8) != 8 || read(channel, &n, 4) != 4 || n > cap) return -1;
    size_t got = 0;
    while (got < n) { ssize_t r = read(channel, out + got, n - got); if (r <= 0) return -1; got += (size_t)r; }
    *dest = self_id;
    *app = appid;
    *failed = 0;
    return (long)n;
}

static int run(int connector)
{
    if (!beer_steam_networking_start(self_id)) return 2;
    const BeerP2PSignaling signaling = { send_blob, next_blob, NULL, NULL };
    if (!beer_steam_networking_set_signaling(appid, &signaling) || !beer_steam_networking_p2p_ready()) return 3;
    uint32_t connection = 0, listener = 0;
    if (connector) {
        usleep(300000);
        unsigned char identity[136];
        beer_steam_networking_identity(identity, peer_id);
        connection = beer_steam_networking_connect_p2p(identity, 3, 0, NULL);
        if (!connection) return 4;
    } else {
        listener = beer_steam_networking_listen(3, 0, NULL);
        if (!listener) return 5;
    }
    int connected = 0, sent = 0, received = 0;
    for (int i = 0; i < 2000 && !(sent && received); ++i) {
        unsigned char status[BEER_STEAM_STATUS_SIZE];
        while (beer_steam_networking_next_status(status)) {
            uint32_t conn, listen;
            int32_t state, old_state;
            memcpy(&conn, status, 4);                       /* Win64: m_hConn @0, m_info @8, m_eOldState @704 */
            memcpy(&listen, status + 8 + 144, 4);
            memcpy(&state, status + 8 + 176, 4);
            memcpy(&old_state, status + 704, 4);
            if (state == 1 && listen == listener && listener) {
                if (beer_steam_networking_accept(conn) != 1) return 6;
                connection = conn;
            }
            if (state == 3 && conn == connection) connected = 1;
            if (state == 4 || state == 5) return 7;
            if (old_state < -3 || old_state > 5) return 8;  /* the layout conversion kept m_eOldState */
        }
        if (connected && !sent) {
            const char *text = connector ? "ping" : "pong";
            int64_t number = 0;
            if (beer_steam_networking_send(connection, text, 5, 8, &number) != 1) return 9;
            sent = 1;
        }
        void *messages[4];
        const int n = connection ? beer_steam_networking_receive(connection, messages, 4) : 0;
        for (int k = 0; k < n; ++k) {
            const char *data;
            memcpy(&data, messages[k], 8);
            if (!strcmp(data, connector ? "pong" : "ping")) received = 1;
            beer_steam_networking_release_message(messages[k]);
        }
        usleep(5000);
    }
    usleep(300000);
    return sent && received ? 0 : 10;
}

int main(void)
{
    if (!getenv("BEER_STEAM_NETWORKING_LIBRARY")) { puts("Steam networking P2P test skipped (no library)"); return 0; }
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) return 1;
    const uint64_t a = UINT64_C(76561198000000011), b = UINT64_C(76561198000000012);
    const pid_t child = fork();
    if (!child) { close(pair[0]); channel = pair[1]; self_id = b; peer_id = a; _exit(run(0)); }
    close(pair[1]);
    channel = pair[0];
    self_id = a;
    peer_id = b;
    const int mine = run(1);
    int status = 0;
    waitpid(child, &status, 0);
    const int theirs = WIFEXITED(status) ? WEXITSTATUS(status) : 99;
    printf("Steam networking P2P through the adapter: %s (connector %d, listener %d)\n",
           !mine && !theirs ? "PASS" : "FAIL", mine, theirs);
    return !mine && !theirs ? 0 : 1;
}
