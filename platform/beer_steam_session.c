#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "beer_steam_session.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define MAX_OWNED 8192
#define REPLY_TIMEOUT_S 30

static struct {
    pthread_mutex_t lock;
    int fd;
    int active;
    uint32_t appid;
    uint64_t steamid;
    char persona[128];
    uint32_t *owned;
    size_t owned_count;
    unsigned next_id;
    char *pending;               /* received bytes not yet split into lines */
    size_t pending_len;
    char *events[256];           /* matchmaking event lines kept for the game (oldest first) */
    unsigned event_count;
} g = { PTHREAD_MUTEX_INITIALIZER, -1, 0, 0, 0, "", NULL, 0, 1, NULL, 0, { NULL }, 0 };

/* Keeps a broadcast line the game must see later (matchmaking events); others are dropped. Takes the line. */
static void keep_event_line(char *line)
{
    if (!strstr(line, "\"event\":\"mms_event\"")) { free(line); return; }
    if (g.event_count == sizeof(g.events) / sizeof(*g.events)) {      /* full: drop the oldest */
        free(g.events[0]);
        memmove(g.events, g.events + 1, sizeof(g.events) - sizeof(*g.events));
        g.event_count--;
    }
    g.events[g.event_count++] = line;
}

/* Splits complete lines out of g.pending; returns the line containing want (caller frees) or NULL. */
static char *take_lines(const char *want)
{
    for (;;) {
        char *newline = g.pending ? memchr(g.pending, '\n', g.pending_len) : NULL;
        if (!newline) return NULL;
        size_t len = (size_t)(newline - g.pending);
        char *line = malloc(len + 1);
        if (!line) return NULL;
        memcpy(line, g.pending, len);
        line[len] = 0;
        memmove(g.pending, newline + 1, g.pending_len - len - 1);
        g.pending_len -= len + 1;
        if (want && strstr(line, want)) return line;
        keep_event_line(line);
    }
}

int beer_steam_session_socket_path(char *out, size_t cap)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    int n = runtime && *runtime ? snprintf(out, cap, "%s/beer/steam.sock", runtime)
                                : snprintf(out, cap, "/tmp/beer-%d/steam.sock", (int)getuid());
    return n > 0 && (size_t)n < cap;
}

static int connect_provider(void)
{
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (!beer_steam_session_socket_path(addr.sun_path, sizeof(addr.sun_path))) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr))) { close(fd); return -1; }
    struct timeval tv = { REPLY_TIMEOUT_S, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

/* Value of a top-level key in one JSON line: a string (unescaped into out) or a bare number. */
static int json_value(const char *json, const char *key, char *out, size_t cap)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(json, pattern);
    if (!p || !cap) return 0;
    p += strlen(pattern);
    size_t n = 0;
    if (*p == '"') {
        for (p++; *p && *p != '"'; p++) {
            char c = *p;
            if (c == '\\' && p[1]) c = *++p;
            if (n + 1 < cap) out[n++] = c;
        }
    } else {
        while (*p && *p != ',' && *p != '}' && n + 1 < cap) out[n++] = *p++;
    }
    out[n] = 0;
    return 1;
}

/* Sends a request and returns the reply line carrying the same id (other broadcast events, such as
 * download progress, are skipped). Caller holds the lock and frees the line. */
static char *request(const char *op, const char *fields)
{
    if (g.fd < 0 && (g.fd = connect_provider()) < 0) return NULL;
    char id[32];
    snprintf(id, sizeof(id), "game%u", g.next_id++);
    char *line = NULL;
    int n = asprintf(&line, "{\"op\":\"%s\",\"id\":\"%s\"%s%s}\n", op, id, fields ? "," : "",
                     fields ? fields : "");
    if (n <= 0 || send(g.fd, line, (size_t)n, MSG_NOSIGNAL) != n) { free(line); goto lost; }
    free(line);
    char want[64];
    snprintf(want, sizeof(want), "\"id\":\"%s\"", id);
    for (;;) {
        char *reply = take_lines(want);
        if (reply) return reply;
        char chunk[16384];
        ssize_t got = recv(g.fd, chunk, sizeof(chunk), 0);
        if (got <= 0) goto lost;
        char *grown = realloc(g.pending, g.pending_len + (size_t)got);
        if (!grown) goto lost;
        g.pending = grown;
        memcpy(g.pending + g.pending_len, chunk, (size_t)got);
        g.pending_len += (size_t)got;
    }
lost:
    if (g.fd >= 0) close(g.fd);
    g.fd = -1;
    free(g.pending);
    g.pending = NULL;
    g.pending_len = 0;
    return NULL;
}

static int reply_ok(const char *reply, char *err, size_t errlen)
{
    char ok[8] = "";
    if (reply && json_value(reply, "ok", ok, sizeof(ok)) && !strcmp(ok, "true")) return 1;
    if (err && errlen) {
        if (!reply) snprintf(err, errlen, "the Beer Steam provider is not running");
        else if (!json_value(reply, "error", err, errlen)) snprintf(err, errlen, "the Steam provider refused");
    }
    return 0;
}

int beer_steam_session_open(uint32_t appid, char *err, size_t errlen)
{
    char fields[64];
    snprintf(fields, sizeof(fields), "\"appid\":\"%u\"", appid);
    pthread_mutex_lock(&g.lock);
    char *reply = request("game_session", fields);
    int ok = reply_ok(reply, err, errlen);
    if (ok) {
        char text[64] = "";
        json_value(reply, "steamid", text, sizeof(text));
        g.steamid = strtoull(text, NULL, 10);
        json_value(reply, "persona", g.persona, sizeof(g.persona));
        free(g.owned);
        g.owned = calloc(MAX_OWNED, sizeof(*g.owned));
        g.owned_count = 0;
        const char *list = strstr(reply, "\"owned\":[");
        for (const char *p = list ? list + 9 : NULL; p && g.owned && *p && *p != ']' && g.owned_count < MAX_OWNED;) {
            char *end;
            unsigned long id = strtoul(p, &end, 10);
            if (end == p) break;
            g.owned[g.owned_count++] = (uint32_t)id;
            p = *end == ',' ? end + 1 : end;
        }
        g.appid = appid;
        g.active = g.steamid != 0;
        if (!g.active && err) snprintf(err, errlen, "the provider returned no SteamID");
        ok = g.active;
    }
    pthread_mutex_unlock(&g.lock);
    free(reply);
    return ok;
}

int beer_steam_session_active(void)
{
    pthread_mutex_lock(&g.lock);
    int active = g.active;
    pthread_mutex_unlock(&g.lock);
    return active;
}

uint64_t beer_steam_session_steamid(void)
{
    pthread_mutex_lock(&g.lock);
    uint64_t id = g.active ? g.steamid : 0;
    pthread_mutex_unlock(&g.lock);
    return id;
}

const char *beer_steam_session_persona(void)
{
    return g.active ? g.persona : "";
}

int beer_steam_session_owns(uint32_t appid)
{
    pthread_mutex_lock(&g.lock);
    int owned = 0;
    for (size_t i = 0; g.active && i < g.owned_count && !owned; i++) owned = g.owned[i] == appid;
    pthread_mutex_unlock(&g.lock);
    return owned;
}

static size_t ticket_request(const char *op, uint32_t appid, const void *userdata, size_t userdata_len,
                             uint8_t *out, size_t cap)
{
    char fields[2200];
    int n = snprintf(fields, sizeof(fields), "\"appid\":\"%u\"", appid);
    if (userdata && userdata_len && userdata_len <= 1024) {
        n += snprintf(fields + n, sizeof(fields) - (size_t)n, ",\"userdata\":\"");
        for (size_t i = 0; i < userdata_len; i++)
            n += snprintf(fields + n, sizeof(fields) - (size_t)n, "%02x", ((const uint8_t *)userdata)[i]);
        snprintf(fields + n, sizeof(fields) - (size_t)n, "\"");
    }
    pthread_mutex_lock(&g.lock);
    int active = g.active;
    char *reply = active ? request(op, fields) : NULL;
    pthread_mutex_unlock(&g.lock);
    char err[256];
    size_t len = 0;
    if (!active) {
        fprintf(stderr, "[STEAM] %s for app %u: no Steam session is open\n", op, appid);
        return 0;
    }
    if (reply_ok(reply, err, sizeof(err))) {
        const char *hex = strstr(reply, "\"ticket\":\"");
        for (hex = hex ? hex + 10 : NULL; hex && hex[0] && hex[1] && hex[0] != '"' && len < cap; hex += 2) {
            unsigned value;
            if (sscanf(hex, "%2x", &value) != 1) break;
            out[len++] = (uint8_t)value;
        }
        if (hex && *hex != '"') len = 0;       /* did not fit */
    } else {
        fprintf(stderr, "[STEAM] %s for app %u failed: %s\n", op, appid, err);
    }
    free(reply);
    return len;
}

size_t beer_steam_session_auth_ticket(uint32_t appid, uint8_t *out, size_t cap, uint32_t *handle)
{
    char fields[64];
    snprintf(fields, sizeof(fields), "\"appid\":\"%u\"", appid);
    pthread_mutex_lock(&g.lock);
    char *reply = g.active ? request("auth_ticket", fields) : NULL;
    pthread_mutex_unlock(&g.lock);
    char err[256], text[32] = "";
    size_t len = 0;
    *handle = 0;
    if (reply_ok(reply, err, sizeof(err))) {
        json_value(reply, "handle", text, sizeof(text));
        *handle = (uint32_t)strtoul(text, NULL, 10);
        const char *hex = strstr(reply, "\"ticket\":\"");
        for (hex = hex ? hex + 10 : NULL; hex && hex[0] && hex[1] && hex[0] != '"' && len < cap; hex += 2) {
            unsigned value;
            if (sscanf(hex, "%2x", &value) != 1) break;
            out[len++] = (uint8_t)value;
        }
        if (!hex || *hex != '"' || !*handle) len = 0;
    } else {
        fprintf(stderr, "[STEAM] auth_ticket for app %u failed: %s\n", appid, err);
    }
    free(reply);
    return len;
}

void beer_steam_session_cancel_auth_ticket(uint32_t appid, uint32_t handle)
{
    char fields[96];
    snprintf(fields, sizeof(fields), "\"appid\":\"%u\",\"handle\":\"%u\"", appid, handle);
    pthread_mutex_lock(&g.lock);
    char *reply = g.active ? request("cancel_auth_ticket", fields) : NULL;
    pthread_mutex_unlock(&g.lock);
    free(reply);
}

size_t beer_steam_session_app_ticket(uint32_t appid, uint8_t *out, size_t cap)
{
    return ticket_request("app_ticket", appid, NULL, 0, out, cap);
}

size_t beer_steam_session_encrypted_ticket(uint32_t appid, const void *userdata, size_t userdata_len,
                                           uint8_t *out, size_t cap)
{
    return ticket_request("encrypted_ticket", appid, userdata, userdata_len, out, cap);
}

/* ---- Matchmaking relay ---- */

static char *hex_of(const void *data, size_t len)
{
    char *text = malloc(len * 2 + 1);
    if (!text) return NULL;
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        text[2 * i] = digits[((const uint8_t *)data)[i] >> 4];
        text[2 * i + 1] = digits[((const uint8_t *)data)[i] & 15];
    }
    text[2 * len] = 0;
    return text;
}

/* Decodes the hex string value of key in line into out; returns the byte count, -1 if absent or too big. */
static long hex_field(const char *line, const char *key, uint8_t *out, size_t cap)
{
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *hex = strstr(line, pattern);
    if (!hex) return -1;
    hex += strlen(pattern);
    size_t len = 0;
    for (; hex[0] && hex[1] && hex[0] != '"'; hex += 2) {
        unsigned value;
        if (len >= cap || sscanf(hex, "%2x", &value) != 1) return -1;
        out[len++] = (uint8_t)value;
    }
    return *hex == '"' ? (long)len : -1;
}

static char *mms_line(const char *op, uint32_t appid, uint32_t emsg, const void *body, size_t len)
{
    char *hex = hex_of(body, len), *fields = NULL;
    if (!hex) return NULL;
    if (asprintf(&fields, "\"appid\":\"%u\",\"emsg\":\"%u\",\"body\":\"%s\"", appid, emsg, hex) < 0) fields = NULL;
    free(hex);
    pthread_mutex_lock(&g.lock);
    char *reply = fields && g.active ? request(op, fields) : NULL;
    pthread_mutex_unlock(&g.lock);
    free(fields);
    return reply;
}

long beer_steam_session_mms_request(uint32_t appid, uint32_t emsg, const void *body, size_t len,
                                    uint8_t *out, size_t cap)
{
    char *reply = mms_line("mms_request", appid, emsg, body, len);
    char err[256];
    long got = -1;
    if (reply_ok(reply, err, sizeof(err))) got = hex_field(reply, "body", out, cap);
    else fprintf(stderr, "[STEAM] matchmaking request %u for app %u failed: %s\n", emsg, appid, err);
    free(reply);
    return got;
}

int beer_steam_session_mms_send(uint32_t appid, uint32_t emsg, const void *body, size_t len)
{
    char *reply = mms_line("mms_send", appid, emsg, body, len);
    char err[256];
    int ok = reply_ok(reply, err, sizeof(err));
    if (!ok) fprintf(stderr, "[STEAM] matchmaking message %u for app %u failed: %s\n", emsg, appid, err);
    free(reply);
    return ok;
}

int beer_steam_session_games_played(uint32_t appid, uint32_t pid)
{
    char fields[96], err[256];
    snprintf(fields, sizeof(fields), "\"appid\":\"%u\",\"pid\":\"%u\"", appid, pid);
    pthread_mutex_lock(&g.lock);
    char *reply = g.active ? request("games_played", fields) : NULL;
    pthread_mutex_unlock(&g.lock);
    int ok = reply_ok(reply, err, sizeof(err));
    if (!ok) fprintf(stderr, "[STEAM] games_played for app %u failed: %s\n", appid, err);
    free(reply);
    return ok;
}

long beer_steam_session_next_mms_event(uint32_t *emsg, uint8_t *out, size_t cap)
{
    /* Called from the game's callback pump: never wait behind a request that is waiting for Steam (its reply
     * loop queues the events that arrive meanwhile). */
    if (pthread_mutex_trylock(&g.lock)) return -1;
    if (g.fd >= 0) {                              /* collect what arrived since the last request */
        for (;;) {
            char chunk[16384];
            ssize_t got = recv(g.fd, chunk, sizeof(chunk), MSG_DONTWAIT);
            if (got <= 0) break;
            char *grown = realloc(g.pending, g.pending_len + (size_t)got);
            if (!grown) break;
            g.pending = grown;
            memcpy(g.pending + g.pending_len, chunk, (size_t)got);
            g.pending_len += (size_t)got;
        }
        take_lines(NULL);
    }
    long len = -1;
    while (g.event_count && len < 0) {
        char *line = g.events[0];
        memmove(g.events, g.events + 1, (g.event_count - 1) * sizeof(*g.events));
        g.event_count--;
        char text[16] = "";
        json_value(line, "emsg", text, sizeof(text));
        *emsg = (uint32_t)strtoul(text, NULL, 10);
        len = hex_field(line, "body", out, cap);
        free(line);
    }
    pthread_mutex_unlock(&g.lock);
    return len;
}
