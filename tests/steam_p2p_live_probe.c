/* Opt-in diagnostic against the real signed-in provider; never part of the offline test suite.
 * Usage: steam-p2p-live-probe <peer SteamID> [app ID] [virtual port]. No tickets or secrets are logged. */
#define _GNU_SOURCE
#include "../platform/beer_steam_session.h"
#include "../platform/beer_steam_networking.h"
#include "../platform/steam_networking_identity.h"
#include "../steam/pb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static unsigned sends, arrivals;
/* Inspect only candidate addresses and route presence, never certificates or ICE passwords. */
static void signal_summary(const void *body, size_t len, const char *direction)
{
    PbReader r = { body, (const uint8_t *)body + len };
    PbField f;
    while (pb_next(&r, &f) == 1) {
        if (f.field == 2 && f.type == PB_BYTES)
            fprintf(stderr, "probe: %s SDR routes present (%zu bytes)\n", direction, f.len);
        if (f.field != 13 || f.type != PB_BYTES) continue;
        PbReader reliable = { f.data, f.data + f.len };
        PbField rf;
        while (pb_next(&reliable, &rf) == 1) {
            if (rf.field != 1 || rf.type != PB_BYTES) continue;
            PbReader ice = { rf.data, rf.data + rf.len };
            PbField candidate;
            while (pb_next(&ice, &candidate) == 1) {
                if (candidate.field != 1 || candidate.type != PB_BYTES) continue;
                PbReader add = { candidate.data, candidate.data + candidate.len };
                PbField text;
                while (pb_next(&add, &text) == 1)
                    if (text.field == 3 && text.type == PB_BYTES)
                        fprintf(stderr, "probe: %s ICE candidate %.*s\n", direction,
                                (int)text.len, (const char *)text.data);
            }
        }
    }
}
static int send_signal(uint32_t app, uint64_t peer, const void *body, size_t len)
{
    signal_summary(body, len, "outgoing");
    int ok = beer_steam_session_p2p_send(app, peer, body, len);
    fprintf(stderr, "probe: signaling send %u (%zu bytes), provider accepted=%d\n", ++sends, len, ok);
    return ok;
}
static long next_signal(uint64_t *src, uint64_t *dest, uint32_t *app, int *failed, uint8_t *out, size_t cap)
{
    long n = beer_steam_session_next_p2p_event(src, dest, app, failed, out, cap);
    if (n >= 0) fprintf(stderr, "probe: received %u from %llu app=%u failed=%d (%ld bytes)\n",
                        ++arrivals, (unsigned long long)*src, *app, *failed, n);
    if (n >= 0 && !*failed) signal_summary(out, (size_t)n, "incoming");
    return n;
}
int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "peer SteamID required\n"); return 2; }
    uint64_t peer = strtoull(argv[1], NULL, 10);
    uint32_t app = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 10) : 480;
    int port = argc > 3 ? atoi(argv[3]) : 27015;
    char error[256] = "";
    if (!peer || !beer_steam_session_open(app, error, sizeof(error))) {
        fprintf(stderr, "probe: session unavailable: %s\n", error); return 2;
    }
    uint64_t self = beer_steam_session_steamid();
    if (!beer_steam_networking_start(self)) return 2;
    const BeerP2PSignaling signaling = { send_signal, next_signal, beer_steam_session_networking_certificate,
                                       beer_steam_session_networking_config };
    if (!beer_steam_networking_set_signaling(app, &signaling)) return 2;
    unsigned char identity[136];
    beer_steam_networking_identity(identity, peer);
    uint32_t connection = beer_steam_networking_connect_p2p(identity, port, 0, NULL);
    if (!connection) return 2;
    time_t deadline = time(NULL) + 25;
    time_t report_at = time(NULL) + 5;
    while (time(NULL) < deadline) {
        if (report_at && time(NULL) >= report_at) {
            char details[16384];
            if (beer_steam_networking_detailed(connection, details, sizeof(details)) == 0)
                fprintf(stderr, "probe: pending connection details:\n%s\n", details);
            report_at = 0;
        }
        unsigned char status[BEER_STEAM_STATUS_SIZE];
        while (beer_steam_networking_next_status(status)) {
            uint32_t conn;
            int state;
            memcpy(&conn, status, 4);
            memcpy(&state, status + 8 + 176, 4);
            if (conn != connection) continue;
            if (state == 3 || state == 4 || state == 5) {
                fprintf(stderr, "probe: final state=%d, sends=%u arrivals=%u\n", state, sends, arrivals);
                char details[16384];
                if (beer_steam_networking_detailed(connection, details, sizeof(details)) == 0)
                    fprintf(stderr, "probe: connection details:\n%s\n", details);
                beer_steam_networking_close(connection, 1000, "diagnostic complete", 0);
                return state == 3 ? 0 : 1;
            }
        }
        usleep(10000);
    }
    fprintf(stderr, "probe: deadline, sends=%u arrivals=%u\n", sends, arrivals);
    beer_steam_networking_close(connection, 1000, "diagnostic complete", 0);
    return 1;
}