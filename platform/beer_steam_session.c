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
} g = { PTHREAD_MUTEX_INITIALIZER, -1, 0, 0, 0, "", NULL, 0, 1, NULL, 0 };

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
    char id[32], line[4400];
    snprintf(id, sizeof(id), "game%u", g.next_id++);
    int n = snprintf(line, sizeof(line), "{\"op\":\"%s\",\"id\":\"%s\"%s%s}\n", op, id, fields ? "," : "",
                     fields ? fields : "");
    if (n <= 0 || (size_t)n >= sizeof(line) || send(g.fd, line, (size_t)n, MSG_NOSIGNAL) != n) goto lost;
    char want[64];
    snprintf(want, sizeof(want), "\"id\":\"%s\"", id);
    for (;;) {
        char *newline = g.pending ? memchr(g.pending, '\n', g.pending_len) : NULL;
        if (newline) {
            size_t len = (size_t)(newline - g.pending);
            char *reply = malloc(len + 1);
            if (!reply) goto lost;
            memcpy(reply, g.pending, len);
            reply[len] = 0;
            memmove(g.pending, newline + 1, g.pending_len - len - 1);
            g.pending_len -= len + 1;
            if (strstr(reply, want)) return reply;
            free(reply);
            continue;
        }
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
