/* beer-steam: Steam session provider.
 *
 *   beer-steam serve            local socket for the manager ($XDG_RUNTIME_DIR/beer/steam.sock)
 *   beer-steam status           sign in with the stored session and report it
 *   beer-steam login-qr         sign in by approving a QR code in the Steam mobile app
 *   beer-steam login <account>  sign in with password + Steam Guard (prompts on the terminal)
 *   beer-steam logout           sign out and forget the stored session
 *   beer-steam library          sign in with the stored session and list the owned apps
 *
 * Socket protocol: one JSON object per line. Requests carry "op" and an optional "id" that is
 * echoed back. Replies and events are broadcast to every connected client. */
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <zlib.h>

#include "auth.h"
#include "cm.h"
#include "install.h"
#include "library.h"
#include "vdf.h"
#include "secret.h"

#define LOGON_TIMEOUT_MS 20000
#define LOGIN_TIMEOUT_S 300
#define MAX_CLIENTS 16

enum { STATE_SIGNED_OUT, STATE_CONNECTING, STATE_SIGNED_IN };
static const char *const STATE_NAMES[] = { "signed_out", "connecting", "signed_in" };

static struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    Cm *cm;
    int state;
    char account[128];
    char persona[128];
    uint64_t steamid;
    int lost;                      /* connection dropped while signed in */
    /* Interactive sign-in in progress. */
    int login_active, login_cancel;
    char login_id[64];
    char *guard_code;              /* code typed by the user, waiting to be submitted */
    int clients[MAX_CLIENTS];
    int client_count;
    int terminal;                  /* CLI mode: events go to stdout as text */
    /* Owned apps: licenses arrive after logon; a worker resolves them through PICS. */
    SteamLicense *licenses;
    size_t license_count;
    int library_wanted, library_busy;
    char *library;                 /* JSON array, NULL until known */
    int library_cached;            /* library came from the cache and is being refreshed */
    char library_error[256];
    Installer *installer;
    /* Game-connect tokens Steam hands out after logon; each auth session ticket uses one. */
    uint8_t gc_tokens[16][64];
    uint32_t gc_token_len[16];
    int gc_token_count;
    /* Auth session tickets registered with Steam through ClientAuthList. */
    struct { uint32_t handle, appid, crc, len; uint8_t data[160]; } tickets[64];
    int ticket_count;
    uint32_t next_ticket, auth_sequence, connection_count;
} g = { .lock = PTHREAD_MUTEX_INITIALIZER, .changed = PTHREAD_COND_INITIALIZER };

/* ---------------------------------------------------------------- JSON */

static void json_escape(char *out, size_t cap, const char *s)
{
    size_t n = 0;
    for (; s && *s && n + 7 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = (char)c; }
        else if (c < 0x20) n += (size_t)snprintf(out + n, cap - n, "\\u%04x", c);
        else out[n++] = (char)c;
    }
    out[n] = 0;
}

/* Value of a top-level string key in a flat JSON object; unescapes into out. */
static int json_get(const char *json, const char *key, char *out, size_t cap)
{
    char pattern[80];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = json;
    while ((p = strstr(p, pattern))) {
        p += strlen(pattern);
        while (*p == ' ' || *p == '\t') p++;
        if (*p != ':') continue;
        p++;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '"') return 0;
        size_t n = 0;
        for (p++; *p && *p != '"'; p++) {
            char c = *p;
            if (c == '\\' && p[1]) {
                p++;
                c = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p == 'u' ? '?' : *p;
                if (*p == 'u') { for (int i = 0; i < 4 && p[1]; i++) p++; }
            }
            if (n + 1 < cap) out[n++] = c;
        }
        out[n] = 0;
        return *p == '"';
    }
    return 0;
}

/* ---------------------------------------------------------------- events */

static void emit(const char *fmt, ...)
{
    char *line;
    va_list ap;
    va_start(ap, fmt);
    int n = vasprintf(&line, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    char *grown = realloc(line, (size_t)n + 2);
    if (!grown) { free(line); return; }
    line = grown;
    line[n++] = '\n';
    pthread_mutex_lock(&g.lock);
    if (g.terminal) {
        fwrite(line, 1, (size_t)n, stdout);
        fflush(stdout);
    }
    for (int i = 0; i < g.client_count; i++)
        if (send(g.clients[i], line, (size_t)n, MSG_NOSIGNAL) < 0) { /* reader thread drops it */ }
    pthread_mutex_unlock(&g.lock);
    free(line);
}

static void emit_state(const char *id)
{
    char account[300], persona[300], idj[140];
    pthread_mutex_lock(&g.lock);
    int state = g.state;
    json_escape(account, sizeof(account), g.account);
    json_escape(persona, sizeof(persona), g.persona);
    uint64_t steamid = g.steamid;
    pthread_mutex_unlock(&g.lock);
    json_escape(idj, sizeof(idj), id ? id : "");
    emit("{\"event\":\"state\",\"id\":\"%s\",\"ok\":true,\"state\":\"%s\",\"account\":\"%s\",\"persona\":\"%s\","
         "\"steamid\":\"%llu\",\"secure_storage\":%s}",
         idj, STATE_NAMES[state], state == STATE_SIGNED_IN ? account : "", state == STATE_SIGNED_IN ? persona : "",
         state == STATE_SIGNED_IN ? (unsigned long long)steamid : 0ULL, secret_available() ? "true" : "false");
}

static void emit_login(const char *id, int ok, const char *error)
{
    char idj[140], errj[512], account[300];
    json_escape(idj, sizeof(idj), id);
    json_escape(errj, sizeof(errj), error ? error : "");
    pthread_mutex_lock(&g.lock);
    json_escape(account, sizeof(account), g.account);
    pthread_mutex_unlock(&g.lock);
    if (ok) emit("{\"event\":\"login\",\"id\":\"%s\",\"ok\":true,\"account\":\"%s\"}", idj, account);
    else emit("{\"event\":\"login\",\"id\":\"%s\",\"ok\":false,\"error\":\"%s\"}", idj, errj);
}

static void set_state(int state)
{
    pthread_mutex_lock(&g.lock);
    int changed = g.state != state;
    g.state = state;
    if (state != STATE_SIGNED_IN) { g.persona[0] = 0; g.steamid = 0; }
    pthread_mutex_unlock(&g.lock);
    if (changed) emit_state(NULL);
}

/* ---------------------------------------------------------------- session */

/* ---------------------------------------------------------------- library */

static void emit_library(const char *id)
{
    char idj[140], errj[600];
    json_escape(idj, sizeof(idj), id ? id : "");
    pthread_mutex_lock(&g.lock);
    const char *state = g.library ? (g.library_cached || g.library_busy ? "refreshing" : "ready")
                                  : g.library_error[0] && !g.library_busy ? "error" : "loading";
    json_escape(errj, sizeof(errj), g.library_error);
    char *apps = g.library ? strdup(g.library) : NULL;
    pthread_mutex_unlock(&g.lock);
    emit("{\"event\":\"library\",\"id\":\"%s\",\"state\":\"%s\",\"error\":\"%s\",\"apps\":%s}",
         idj, state, errj, apps ? apps : "[]");
    free(apps);
}

/* ~/.beer/steam/library-<steamid>.json: the last known list, shown while it is refreshed. */
static int library_cache_path(uint64_t steamid, char *out, size_t cap)
{
    const char *home = getenv("HOME");
    if (!home || !steamid) return 0;
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.beer", home);
    mkdir(dir, 0755);
    snprintf(dir, sizeof(dir), "%s/.beer/steam", home);
    if (mkdir(dir, 0700) && errno != EEXIST) return 0;
    return snprintf(out, cap, "%s/library-%llu.json", dir, (unsigned long long)steamid) < (int)cap;
}

static void library_cache_save(uint64_t steamid, const char *json)
{
    char path[600], tmp[620];
    if (!library_cache_path(steamid, path, sizeof(path))) return;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    mode_t old = umask(0177);
    FILE *f = fopen(tmp, "w");
    umask(old);
    if (!f) return;
    int ok = fputs(json, f) >= 0;
    ok = fclose(f) == 0 && ok;
    if (ok) rename(tmp, path);
    else unlink(tmp);
}

static char *library_cache_load(uint64_t steamid)
{
    char path[600];
    if (!library_cache_path(steamid, path, sizeof(path))) return NULL;
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char *text = NULL;
    size_t cap = 0;
    ssize_t n = getdelim(&text, &cap, 0, f);
    fclose(f);
    if (n <= 1 || text[0] != '[') { free(text); return NULL; }
    return text;
}

static void library_cache_remove(uint64_t steamid)
{
    char path[600];
    if (library_cache_path(steamid, path, sizeof(path))) unlink(path);
}

static void *library_worker(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g.lock);
        while (!(g.library_wanted && g.state == STATE_SIGNED_IN && g.steamid))
            pthread_cond_wait(&g.changed, &g.lock);
        g.library_wanted = 0;
        g.library_busy = 1;
        size_t count = g.license_count;
        SteamLicense *licenses = malloc((count ? count : 1) * sizeof(*licenses));
        if (licenses) memcpy(licenses, g.licenses, count * sizeof(*licenses));
        uint64_t steamid = g.steamid;
        pthread_mutex_unlock(&g.lock);
        if (!licenses) continue;

        SteamApp *apps;
        size_t app_count;
        char err[256] = "";
        int ok = library_fetch(g.cm, (uint32_t)steamid, licenses, count, &apps, &app_count, err, sizeof(err));
        free(licenses);
        char *json = ok ? library_json(apps, app_count) : NULL;
        if (ok) library_free(apps, app_count);
        if (json) library_cache_save(steamid, json);
        fprintf(stderr, "[STEAM] library: %s (%zu licenses, %zu apps)\n", json ? "ready" : err,
                count, json ? app_count : 0);

        pthread_mutex_lock(&g.lock);
        int current = g.steamid == steamid;
        if (json && current) {
            free(g.library);
            g.library = json;
            json = NULL;
            g.library_cached = 0;
            g.library_error[0] = 0;
        } else if (current) {
            snprintf(g.library_error, sizeof(g.library_error), "%s", err[0] ? err : "could not read the library");
        }
        g.library_busy = 0;
        pthread_cond_broadcast(&g.changed);
        pthread_mutex_unlock(&g.lock);
        free(json);
        if (current) emit_library(NULL);
    }
    return NULL;
}

/* App IDs in the current library (for DLC depot ownership). Caller frees. */
static uint32_t *owned_apps(size_t *count)
{
    *count = 0;
    pthread_mutex_lock(&g.lock);
    char *json = g.library ? strdup(g.library) : NULL;
    pthread_mutex_unlock(&g.lock);
    if (!json) return NULL;
    size_t cap = 256;
    uint32_t *ids = malloc(cap * sizeof(*ids));
    for (char *p = json; ids && (p = strstr(p, "\"appid\":")); p += 8) {
        if (*count == cap) {
            uint32_t *grown = realloc(ids, (cap *= 2) * sizeof(*ids));
            if (!grown) break;
            ids = grown;
        }
        ids[(*count)++] = (uint32_t)strtoul(p + 8, NULL, 10);
    }
    free(json);
    return ids;
}

static void library_reset(void)
{
    pthread_mutex_lock(&g.lock);
    free(g.library);
    g.library = NULL;
    free(g.licenses);
    g.licenses = NULL;
    g.license_count = 0;
    g.library_cached = 0;
    g.library_error[0] = 0;
    pthread_mutex_unlock(&g.lock);
}

static void on_message(void *context, uint32_t emsg, const uint8_t *body, size_t len)
{
    (void)context;
    if (emsg == EMSG_CLIENT_GAME_CONNECT_TOKENS) {
        PbReader r = { body, body + len };
        PbField f;
        pthread_mutex_lock(&g.lock);
        while (pb_next(&r, &f) == 1) {
            if (f.field != 2 || f.type != PB_BYTES || !f.len || f.len > sizeof(g.gc_tokens[0])) continue;
            if (g.gc_token_count == 16) {           /* keep the newest */
                memmove(g.gc_tokens, g.gc_tokens[1], sizeof(g.gc_tokens[0]) * 15);
                memmove(g.gc_token_len, g.gc_token_len + 1, sizeof(g.gc_token_len[0]) * 15);
                g.gc_token_count--;
            }
            memcpy(g.gc_tokens[g.gc_token_count], f.data, f.len);
            g.gc_token_len[g.gc_token_count++] = (uint32_t)f.len;
        }
        pthread_mutex_unlock(&g.lock);
        return;
    }
    if (emsg == EMSG_CLIENT_LICENSE_LIST) {
        SteamLicense *licenses;
        size_t count;
        if (library_parse_licenses(body, len, &licenses, &count) != ERESULT_OK) { free(licenses); return; }
        pthread_mutex_lock(&g.lock);
        free(g.licenses);
        g.licenses = licenses;
        g.license_count = count;
        g.library_wanted = 1;
        pthread_cond_broadcast(&g.changed);
        pthread_mutex_unlock(&g.lock);
    } else if (emsg == EMSG_CLIENT_ACCOUNT_INFO) {
        PbField f;
        if (pb_find(body, len, 1, &f) && f.type == PB_BYTES) {
            pthread_mutex_lock(&g.lock);
            size_t n = f.len < sizeof(g.persona) - 1 ? f.len : sizeof(g.persona) - 1;
            memcpy(g.persona, f.data, n);
            g.persona[n] = 0;
            pthread_mutex_unlock(&g.lock);
            emit_state(NULL);
        }
    } else if (emsg == 0 || emsg == EMSG_CLIENT_LOGGED_OFF) {
        pthread_mutex_lock(&g.lock);
        if (g.state == STATE_SIGNED_IN) g.lost = 1;
        pthread_cond_broadcast(&g.changed);
        pthread_mutex_unlock(&g.lock);
    }
}

static int ensure_connected(char *err, size_t errlen)
{
    if (cm_connected(g.cm)) return 1;
    cm_disconnect(g.cm);   /* reap a thread from an earlier connection */
    return cm_connect(g.cm, err, errlen);
}

/* Logs on with a refresh token. Returns an EResult; on OK the state is signed in. */
static int logon_with(const char *account, const char *refresh_token)
{
    char err[256];
    if (!ensure_connected(err, sizeof(err))) return ERESULT_NO_CONNECTION;
    int result = cm_logon(g.cm, account, refresh_token, LOGON_TIMEOUT_MS);
    if (result == ERESULT_OK) {
        pthread_mutex_lock(&g.lock);
        snprintf(g.account, sizeof(g.account), "%s", account);
        g.lost = 0;
        pthread_mutex_unlock(&g.lock);
        set_state(STATE_SIGNED_IN);
        uint64_t steamid = cm_steamid(g.cm);
        char *cached = library_cache_load(steamid);
        pthread_mutex_lock(&g.lock);
        if (g.steamid != steamid) {
            free(g.library);
            g.library = cached;
            g.library_cached = cached != NULL;
            cached = NULL;
        }
        g.steamid = steamid;
        pthread_cond_broadcast(&g.changed);
        pthread_mutex_unlock(&g.lock);
        free(cached);
        emit_state(NULL);
        emit_library(NULL);
    }
    return result;
}

/* A stored token that Steam rejects for good is forgotten; network trouble keeps it. */
static int token_rejected(int result)
{
    return result == ERESULT_ACCESS_DENIED || result == ERESULT_EXPIRED || result == ERESULT_REVOKED ||
           result == ERESULT_INVALID_PASSWORD;
}

static int resume_stored_session(void)
{
    char *account, *token;
    if (secret_load_session(&account, &token) != 1) return ERESULT_FAIL;
    set_state(STATE_CONNECTING);
    int result = logon_with(account, token);
    if (token_rejected(result)) secret_clear_session();
    if (result != ERESULT_OK) set_state(STATE_SIGNED_OUT);
    free(account);
    secret_wipe(token);
    return result;
}

/* Keeps a signed-in session alive across dropped connections. */
static void *supervisor(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g.lock);
        while (!g.lost) pthread_cond_wait(&g.changed, &g.lock);
        g.lost = 0;
        pthread_mutex_unlock(&g.lock);
        set_state(STATE_CONNECTING);
        for (int delay = 2; ; delay = delay < 60 ? delay * 2 : 60) {
            int result = resume_stored_session();
            if (result == ERESULT_OK || token_rejected(result) || result == ERESULT_FAIL) break;
            set_state(STATE_CONNECTING);
            sleep((unsigned)delay);
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------- interactive sign-in */

typedef struct {
    char id[64];
    int qr;
    char account[128];
    char *password;
} LoginRequest;

static const char *guard_name(int type)
{
    switch (type) {
    case GUARD_EMAIL_CODE: return "email_code";
    case GUARD_DEVICE_CODE: return "device_code";
    case GUARD_DEVICE_CONFIRMATION: return "device_confirmation";
    case GUARD_EMAIL_CONFIRMATION: return "email_confirmation";
    case GUARD_MACHINE_TOKEN: return "machine_token";
    default: return "unknown";
    }
}

static void emit_guard(const char *id, const AuthSession *s, const char *error)
{
    char types[256] = "", hint[300], idj[140], errj[300];
    size_t n = 0;
    for (int i = 0; i < s->guard_count; i++)
        n += (size_t)snprintf(types + n, sizeof(types) - n, "%s\"%s\"", i ? "," : "", guard_name(s->guards[i]));
    json_escape(hint, sizeof(hint), s->guard_hint ? s->guard_hint : "");
    json_escape(idj, sizeof(idj), id);
    json_escape(errj, sizeof(errj), error ? error : "");
    emit("{\"event\":\"guard\",\"id\":\"%s\",\"types\":[%s],\"hint\":\"%s\",\"error\":\"%s\"}", idj, types, hint, errj);
}

static int has_guard(const AuthSession *s, int type)
{
    for (int i = 0; i < s->guard_count; i++) if (s->guards[i] == type) return 1;
    return 0;
}

static int code_guard_type(const AuthSession *s)
{
    if (has_guard(s, GUARD_DEVICE_CODE)) return GUARD_DEVICE_CODE;
    if (has_guard(s, GUARD_EMAIL_CODE)) return GUARD_EMAIL_CODE;
    return 0;
}

static void *login_thread(void *arg)
{
    LoginRequest *req = arg;
    char err[256] = "";
    AuthSession session;
    memset(&session, 0, sizeof(session));
    int result = ERESULT_FAIL;
    if (!ensure_connected(err, sizeof(err))) {
        emit_login(req->id, 0, err);
        goto finish;
    }
    if (req->qr) {
        result = auth_begin_qr(g.cm, &session);
        if (result == ERESULT_OK) {
            char url[1024], idj[140];
            json_escape(url, sizeof(url), session.challenge_url);
            json_escape(idj, sizeof(idj), req->id);
            emit("{\"event\":\"qr\",\"id\":\"%s\",\"url\":\"%s\"}", idj, url);
        }
    } else {
        char *guard = secret_load_guard(req->account);
        result = auth_begin_password(g.cm, req->account, req->password, guard, &session);
        secret_wipe(guard);
        secret_wipe(req->password);
        req->password = NULL;
        if (result == ERESULT_OK && !has_guard(&session, GUARD_NONE)) emit_guard(req->id, &session, NULL);
    }
    if (result != ERESULT_OK) {
        emit_login(req->id, 0, cm_eresult_name(result));
        goto finish;
    }

    time_t deadline = time(NULL) + LOGIN_TIMEOUT_S;
    for (;;) {
        pthread_mutex_lock(&g.lock);
        struct timespec wait;
        clock_gettime(CLOCK_REALTIME, &wait);
        wait.tv_sec += session.interval_ms / 1000;
        wait.tv_nsec += (long)(session.interval_ms % 1000) * 1000000L;
        if (wait.tv_nsec >= 1000000000L) { wait.tv_sec++; wait.tv_nsec -= 1000000000L; }
        while (!g.login_cancel && !g.guard_code)
            if (pthread_cond_timedwait(&g.changed, &g.lock, &wait) == ETIMEDOUT) break;
        int cancel = g.login_cancel;
        char *code = g.guard_code;
        g.guard_code = NULL;
        pthread_mutex_unlock(&g.lock);
        if (cancel) { secret_wipe(code); emit_login(req->id, 0, "cancelled"); break; }
        if (time(NULL) > deadline) { secret_wipe(code); emit_login(req->id, 0, "sign-in timed out"); break; }
        if (code) {
            int type = code_guard_type(&session);
            int submitted = type ? auth_submit_code(g.cm, &session, code, type) : ERESULT_FAIL;
            secret_wipe(code);
            if (submitted != ERESULT_OK) {
                emit_guard(req->id, &session, type ? cm_eresult_name(submitted) : "this sign-in does not take a code");
                continue;
            }
        }
        AuthTokens tokens;
        int url_changed, poll_result;
        int status = auth_poll(g.cm, &session, &tokens, &url_changed, &poll_result);
        if (status < 0) { emit_login(req->id, 0, cm_eresult_name(poll_result)); break; }
        if (url_changed && req->qr) {
            char url[1024], idj[140];
            json_escape(url, sizeof(url), session.challenge_url);
            json_escape(idj, sizeof(idj), req->id);
            emit("{\"event\":\"qr\",\"id\":\"%s\",\"url\":\"%s\"}", idj, url);
        }
        if (status == 0) continue;

        if (tokens.guard_data) secret_save_guard(tokens.account_name, tokens.guard_data);
        int stored = secret_save_session(tokens.account_name, tokens.refresh_token);
        int logon = logon_with(tokens.account_name, tokens.refresh_token);
        auth_tokens_free(&tokens);
        if (logon != ERESULT_OK) { emit_login(req->id, 0, cm_eresult_name(logon)); break; }
        emit_login(req->id, 1, NULL);
        if (!stored)
            emit("{\"event\":\"warning\",\"message\":\"No keyring available: you will need to sign in again next time\"}");
        break;
    }

finish:
    auth_session_free(&session);
    secret_wipe(req->password);
    free(req);
    pthread_mutex_lock(&g.lock);
    g.login_active = 0;
    g.login_cancel = 0;
    secret_wipe(g.guard_code);
    g.guard_code = NULL;
    pthread_mutex_unlock(&g.lock);
    return NULL;
}

static int start_login(const char *id, int qr, const char *account, const char *password)
{
    pthread_mutex_lock(&g.lock);
    int busy = g.login_active || g.state == STATE_SIGNED_IN;
    if (!busy) { g.login_active = 1; g.login_cancel = 0; }
    pthread_mutex_unlock(&g.lock);
    if (busy) {
        emit_login(id, 0, g.state == STATE_SIGNED_IN ? "already signed in" : "a sign-in is already in progress");
        return 0;
    }
    LoginRequest *req = calloc(1, sizeof(*req));
    if (!req) return 0;
    snprintf(req->id, sizeof(req->id), "%s", id);
    req->qr = qr;
    snprintf(req->account, sizeof(req->account), "%s", account ? account : "");
    req->password = password ? strdup(password) : NULL;
    pthread_t thread;
    if (pthread_create(&thread, NULL, login_thread, req)) {
        secret_wipe(req->password);
        free(req);
        pthread_mutex_lock(&g.lock);
        g.login_active = 0;
        pthread_mutex_unlock(&g.lock);
        return 0;
    }
    pthread_detach(thread);
    return 1;
}

static void do_logout(void)
{
    /* Forget the session first, so the server ending it does not look like a dropped connection. */
    pthread_mutex_lock(&g.lock);
    uint64_t steamid = g.steamid;
    pthread_mutex_unlock(&g.lock);
    library_cache_remove(steamid);
    library_reset();
    /* End the session on Steam's side too, so the refresh token cannot be used again. */
    char *account = NULL, *token = NULL;
    if (cm_logged_on(g.cm) && secret_load_session(&account, &token) == 1) {
        PbBuf request = {0}, response;
        int eresult;
        pb_string(&request, 1, token);
        pb_u64(&request, 2, 0);                        /* k_EAuthTokenRevokeLogout */
        cm_call(g.cm, "Authentication.RevokeToken#1", &request, 1, 10000, &response, &eresult);
        fprintf(stderr, "[STEAM] sign-out: token revoke -> %s\n", cm_eresult_name(eresult));
        if (request.data) memset(request.data, 0, request.len);
        pb_free(&request);
        pb_free(&response);
    }
    free(account);
    secret_wipe(token);
    pthread_mutex_lock(&g.lock);
    g.ticket_count = 0;
    g.gc_token_count = 0;
    pthread_mutex_unlock(&g.lock);
    secret_clear_session();
    set_state(STATE_SIGNED_OUT);
    cm_logoff(g.cm);
    cm_disconnect(g.cm);
    pthread_mutex_lock(&g.lock);
    g.account[0] = 0;
    g.lost = 0;
    pthread_mutex_unlock(&g.lock);
}

/* ---------------------------------------------------------------- downloads */

static void on_install_event(void *context, const char *json)
{
    (void)context;
    emit("%s", json);
}

/* Library folders arrive as one string with one folder per line. */
static size_t split_folders(char *text, const char **folders, size_t cap)
{
    size_t count = 0;
    for (char *line = strtok(text, "\n"); line && count < cap; line = strtok(NULL, "\n"))
        if (*line == '/') folders[count++] = line;
    return count;
}

static void emit_installs(const char *id, char *folder_text)
{
    const char *folders[32];
    size_t count = split_folders(folder_text, folders, 32);
    char *installs = installs_json(folders, count);
    char idj[140];
    json_escape(idj, sizeof(idj), id);
    emit("{\"event\":\"installs\",\"id\":\"%s\",\"apps\":%s}", idj, installs ? installs : "[]");
    free(installs);
}

typedef struct {
    char id[64];
    char folders[8192];
} UpdateCheck;

/* Compares each installed build with the branch's current build on Steam. */
static void *update_check_thread(void *arg)
{
    UpdateCheck *check = arg;
    const char *folders[32];
    size_t count = split_folders(check->folders, folders, 32);
    char *installs = installs_json(folders, count);
    char list[4096] = "";
    size_t len = 0;
    for (char *p = installs; p && (p = strstr(p, "{\"appid\":")); p++) {
        uint32_t appid = (uint32_t)strtoul(p + 9, NULL, 10);
        char branch[64] = "public", buildid[32] = "";
        char *b = strstr(p, "\"branch\":\""), *v = strstr(p, "\"buildid\":\"");
        if (b) sscanf(b + 10, "%63[^\"]", branch);
        if (v) sscanf(v + 11, "%31[^\"]", buildid);
        VdfNode *root = library_app_info(g.cm, appid);
        char path[128];
        snprintf(path, sizeof(path), "appinfo/depots/branches/%s/buildid", branch);
        const char *current = root ? vdf_string(root, path, "") : "";
        if (*current && strcmp(current, buildid) && len + 16 < sizeof(list))
            len += (size_t)snprintf(list + len, sizeof(list) - len, "%s%u", len ? "," : "", appid);
        vdf_free(root);
    }
    free(installs);
    char idj[140];
    json_escape(idj, sizeof(idj), check->id);
    emit("{\"event\":\"updates\",\"id\":\"%s\",\"apps\":[%s]}", idj, list);
    free(check);
    return NULL;
}

static void game_error(const char *event, const char *id, const char *error);

/* Branches of an app (public and betas) from its PICS record. */
static void emit_branches(const char *id, uint32_t appid)
{
    char idj[140];
    json_escape(idj, sizeof(idj), id);
    VdfNode *root = library_app_info(g.cm, appid);
    VdfNode *branches = root ? vdf_path(root, "appinfo/depots/branches") : NULL;
    size_t cap = 1024, len = 0;
    char *list = malloc(cap);
    if (list) list[0] = 0;
    for (VdfNode *b = branches ? branches->child : NULL; b && list; b = b->next) {
        if (!b->child) continue;
        char name[200], description[600], key[300], item[1200];
        json_escape(name, sizeof(name), b->key);
        json_escape(description, sizeof(description), vdf_string(b, "description", ""));
        snprintf(key, sizeof(key), "%u/%s", appid, b->key);
        int password = !strcmp(vdf_string(b, "pwdrequired", "0"), "1");
        char *stored = password ? secret_load_beta(key) : NULL;
        int n = snprintf(item, sizeof(item), "%s{\"name\":\"%s\",\"buildid\":\"%s\",\"description\":\"%s\","
                         "\"password\":%s,\"unlocked\":%s,\"updated\":%s}", len ? "," : "", name,
                         vdf_string(b, "buildid", ""), description, password ? "true" : "false",
                         !password || stored ? "true" : "false", vdf_string(b, "timeupdated", "0"));
        secret_wipe(stored);
        if (n < 0) continue;
        if (len + (size_t)n + 1 > cap) {
            while (len + (size_t)n + 1 > cap) cap *= 2;
            char *grown = realloc(list, cap);
            if (!grown) break;
            list = grown;
        }
        memcpy(list + len, item, (size_t)n + 1);
        len += (size_t)n;
    }
    vdf_free(root);
    emit("{\"event\":\"app_branches\",\"id\":\"%s\",\"appid\":%u,\"ok\":%s,\"branches\":[%s]}", idj, appid,
         branches ? "true" : "false", list ? list : "");
    free(list);
}

/* Checks a beta password with Steam; the branch keys it unlocks are kept in the keyring. */
static void check_beta_password(const char *id, uint32_t appid, const char *password)
{
    PbBuf request = {0};
    pb_u64(&request, 1, appid);
    pb_string(&request, 2, password);
    CmPart *parts;
    int answered = cm_request(g.cm, EMSG_CLIENT_CHECK_APP_BETA_PASSWORD, &request, 0, 20000, &parts);
    if (request.data) memset(request.data, 0, request.len);
    pb_free(&request);
    char idj[140], names[1024] = "";
    json_escape(idj, sizeof(idj), id);
    if (!answered) { game_error("beta_password", id, "Steam did not answer"); return; }
    PbField f;
    int eresult = pb_find(parts->body.data, parts->body.len, 1, &f) ? (int)(int32_t)f.value : ERESULT_FAIL;
    size_t len = 0;
    PbReader r = { parts->body.data, parts->body.data + parts->body.len };
    while (eresult == ERESULT_OK && pb_next(&r, &f) == 1) {
        if (f.field != 4 || f.type != PB_BYTES) continue;
        PbField name, key;
        if (!pb_find(f.data, f.len, 1, &name) || !pb_find(f.data, f.len, 2, &key)) continue;
        char *branch = pb_strdup(&name), *hex = pb_strdup(&key), label[300], escaped[200];
        if (branch && hex) {
            snprintf(label, sizeof(label), "%u/%s", appid, branch);
            secret_save_beta(label, hex);
            json_escape(escaped, sizeof(escaped), branch);
            if (len + strlen(escaped) + 4 < sizeof(names))
                len += (size_t)snprintf(names + len, sizeof(names) - len, "%s\"%s\"", len ? "," : "", escaped);
        }
        free(branch);
        secret_wipe(hex);
    }
    cm_parts_free(parts);
    if (eresult != ERESULT_OK || !len) { game_error("beta_password", id, "Steam did not accept that beta password"); return; }
    emit("{\"event\":\"beta_password\",\"id\":\"%s\",\"appid\":%u,\"ok\":true,\"branches\":[%s]}", idj, appid, names);
}

static void handle_download_request(const char *op, const char *id, const char *line)
{
    char appid_text[16] = "", folder[1024] = "", branch[64] = "", language[32] = "";
    json_get(line, "appid", appid_text, sizeof(appid_text));
    json_get(line, "folder", folder, sizeof(folder));
    json_get(line, "branch", branch, sizeof(branch));
    json_get(line, "language", language, sizeof(language));
    uint32_t appid = (uint32_t)strtoul(appid_text, NULL, 10);
    char idj[140], err[512] = "";
    json_escape(idj, sizeof(idj), id);
    int ok = 1;
    if (!strcmp(op, "install") || !strcmp(op, "update") || !strcmp(op, "verify")) {
        if (!appid || folder[0] != '/') { ok = 0; snprintf(err, sizeof(err), "an app and an absolute library folder are required"); }
        else {
            InstallRequest request;
            memset(&request, 0, sizeof(request));
            request.appid = appid;
            json_get(line, "name", request.name, sizeof(request.name));
            snprintf(request.folder, sizeof(request.folder), "%s", folder);
            snprintf(request.branch, sizeof(request.branch), "%s", branch);
            snprintf(request.language, sizeof(request.language), "%s", language[0] ? language : "english");
            if (branch[0] && strcmp(branch, "public")) {
                char label[300];
                snprintf(label, sizeof(label), "%u/%s", appid, branch);
                char *key = secret_load_beta(label);
                if (key) snprintf(request.beta_key, sizeof(request.beta_key), "%s", key);
                secret_wipe(key);
            }
            request.action = !strcmp(op, "install") ? INSTALL_ACTION_INSTALL
                           : !strcmp(op, "update") ? INSTALL_ACTION_UPDATE : INSTALL_ACTION_VERIFY;
            size_t owned_count;
            uint32_t *owned = owned_apps(&owned_count);
            request.owned = owned;
            request.owned_count = owned_count;
            ok = installer_enqueue(g.installer, &request, err, sizeof(err));
            free(owned);
        }
    } else if (!strcmp(op, "move")) {
        InstallRequest request;
        memset(&request, 0, sizeof(request));
        request.appid = appid;
        request.action = INSTALL_ACTION_MOVE;
        json_get(line, "name", request.name, sizeof(request.name));
        snprintf(request.folder, sizeof(request.folder), "%s", folder);
        json_get(line, "to", request.target, sizeof(request.target));
        if (!appid || folder[0] != '/' || request.target[0] != '/') {
            ok = 0;
            snprintf(err, sizeof(err), "an app, its folder and an absolute target folder are required");
        } else {
            ok = installer_enqueue(g.installer, &request, err, sizeof(err));
        }
    } else if (!strcmp(op, "app_branches")) {
        emit_branches(id, appid);
    } else if (!strcmp(op, "beta_password")) {
        char password[256] = "";
        json_get(line, "password", password, sizeof(password));
        check_beta_password(id, appid, password);
        memset(password, 0, sizeof(password));
    } else if (!strcmp(op, "pause")) {
        installer_pause(g.installer, appid);
    } else if (!strcmp(op, "resume")) {
        installer_resume(g.installer, appid);
    } else if (!strcmp(op, "cancel")) {
        installer_cancel(g.installer, appid);
    } else if (!strcmp(op, "uninstall")) {
        if (installer_busy_with(g.installer, appid)) { ok = 0; snprintf(err, sizeof(err), "cancel the download first"); }
        else ok = install_remove(folder, appid, err, sizeof(err));
        if (ok) emit("{\"event\":\"uninstalled\",\"id\":\"%s\",\"appid\":%u}", idj, appid);
    } else if (!strcmp(op, "downloads")) {
        char *queue = installer_queue_json(g.installer);
        emit("{\"event\":\"downloads\",\"id\":\"%s\",\"jobs\":%s}", idj, queue ? queue : "[]");
        free(queue);
    } else if (!strcmp(op, "bandwidth")) {
        char value[32] = "0";
        json_get(line, "bytes_per_second", value, sizeof(value));
        installer_set_bandwidth(g.installer, strtoull(value, NULL, 10));
    }
    if (!ok) {
        char errj[1100];
        json_escape(errj, sizeof(errj), err);
        emit("{\"event\":\"error\",\"id\":\"%s\",\"op\":\"%s\",\"appid\":%u,\"ok\":false,\"error\":\"%s\"}",
             idj, op, appid, errj);
    }
}

/* ---------------------------------------------------------------- game sessions */

static int app_owned(uint32_t appid)
{
    size_t count;
    uint32_t *owned = owned_apps(&count);
    int found = 0;
    for (size_t i = 0; i < count && !found; i++) found = owned[i] == appid;
    free(owned);
    return found;
}

static void hex_text(const uint8_t *data, size_t len, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) { out[2 * i] = digits[data[i] >> 4]; out[2 * i + 1] = digits[data[i] & 15]; }
    out[2 * len] = 0;
}

static size_t unhex(const char *text, uint8_t *out, size_t cap)
{
    size_t n = 0;
    for (; text[0] && text[1] && n < cap; text += 2) {
        unsigned value;
        if (sscanf(text, "%2x", &value) != 1) break;
        out[n++] = (uint8_t)value;
    }
    return n;
}

static void game_error(const char *event, const char *id, const char *error)
{
    char idj[140], errj[600];
    json_escape(idj, sizeof(idj), id);
    json_escape(errj, sizeof(errj), error);
    emit("{\"event\":\"%s\",\"id\":\"%s\",\"ok\":false,\"error\":\"%s\"}", event, idj, errj);
}

/* Sends every active auth ticket to Steam, so servers that validate them see them as issued.
 * Caller holds g.lock. */
static void send_auth_list_locked(void)
{
    PbBuf list = {0};
    pb_u64(&list, 1, (uint64_t)g.gc_token_count);
    pb_u64(&list, 2, ++g.auth_sequence);
    pb_u64(&list, 3, 0);
    uint32_t apps[64];
    int app_count = 0;
    for (int i = 0; i < g.ticket_count; i++) {
        PbBuf t = {0};
        pb_u64(&t, 1, 0);                              /* estate */
        pb_u64(&t, 2, ERESULT_OK);
        pb_fixed64(&t, 3, g.steamid);
        pb_fixed64(&t, 4, g.tickets[i].appid);
        pb_u64(&t, 5, 1);                              /* h_steam_pipe */
        pb_u64(&t, 6, g.tickets[i].crc);
        pb_bytes(&t, 7, g.tickets[i].data, g.tickets[i].len);
        pb_msg(&list, 4, &t);
        pb_free(&t);
        int seen = 0;
        for (int a = 0; a < app_count; a++) seen |= apps[a] == g.tickets[i].appid;
        if (!seen) apps[app_count++] = g.tickets[i].appid;
    }
    for (int a = 0; a < app_count; a++) pb_u64(&list, 5, apps[a]);
    pb_u64(&list, 6, g.auth_sequence);
    cm_send(g.cm, EMSG_CLIENT_AUTH_LIST, &list);
    pb_free(&list);
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* Auth session ticket: [u32 n][game-connect token], the 24-byte session header, then
 * [u32 n][Steam-signed ownership ticket]. The first two parts are registered with Steam. */
static void issue_auth_ticket(const char *id, uint32_t appid)
{
    char idj[140];
    json_escape(idj, sizeof(idj), id);
    PbBuf request = {0};
    pb_u64(&request, 1, appid);
    CmPart *parts;
    int answered = cm_request(g.cm, EMSG_CLIENT_GET_APP_OWNERSHIP_TICKET, &request, 0, 20000, &parts);
    pb_free(&request);
    PbField result, owner;
    if (!answered) { game_error("auth_ticket", id, "Steam did not answer"); return; }
    int ok = pb_find(parts->body.data, parts->body.len, 1, &result) && (int)result.value == ERESULT_OK &&
             pb_find(parts->body.data, parts->body.len, 3, &owner) && owner.len && owner.len <= 1024;
    if (!ok) { cm_parts_free(parts); game_error("auth_ticket", id, "Steam refused the ownership ticket"); return; }

    pthread_mutex_lock(&g.lock);
    if (!g.gc_token_count || g.ticket_count == 64) {
        pthread_mutex_unlock(&g.lock);
        cm_parts_free(parts);
        game_error("auth_ticket", id, g.ticket_count == 64 ? "too many active tickets" : "Steam has not issued game connect tokens");
        return;
    }
    uint8_t ticket[2048];
    size_t n = 0;
    uint32_t token_len = g.gc_token_len[0];
    put32(ticket, token_len); n = 4;
    memcpy(ticket + n, g.gc_tokens[0], token_len); n += token_len;
    memmove(g.gc_tokens, g.gc_tokens[1], sizeof(g.gc_tokens[0]) * 15);
    memmove(g.gc_token_len, g.gc_token_len + 1, sizeof(g.gc_token_len[0]) * 15);
    g.gc_token_count--;
    uint32_t header[7] = { 24, 1, 2, cm_public_ip(g.cm), 0, cm_logon_age(g.cm) * 1000u, ++g.connection_count };
    for (int i = 0; i < 7; i++, n += 4) put32(ticket + n, header[i]);
    size_t registered = n;
    put32(ticket + n, (uint32_t)owner.len); n += 4;
    memcpy(ticket + n, owner.data, owner.len); n += owner.len;
    cm_parts_free(parts);

    uint32_t handle = ++g.next_ticket;
    if (!handle) handle = ++g.next_ticket;
    g.tickets[g.ticket_count].handle = handle;
    g.tickets[g.ticket_count].appid = appid;
    g.tickets[g.ticket_count].len = (uint32_t)registered;
    memcpy(g.tickets[g.ticket_count].data, ticket, registered);
    g.tickets[g.ticket_count].crc = (uint32_t)crc32(0, ticket, (uInt)registered);
    g.ticket_count++;
    send_auth_list_locked();
    pthread_mutex_unlock(&g.lock);

    char *hex = malloc(n * 2 + 1);
    if (!hex) return;
    hex_text(ticket, n, hex);
    emit("{\"event\":\"auth_ticket\",\"id\":\"%s\",\"ok\":true,\"appid\":%u,\"handle\":%u,\"ticket\":\"%s\"}",
         idj, appid, handle, hex);
    free(hex);
}

static void cancel_auth_ticket(const char *id, uint32_t handle)
{
    pthread_mutex_lock(&g.lock);
    for (int i = 0; i < g.ticket_count; i++)
        if (g.tickets[i].handle == handle) {
            g.tickets[i] = g.tickets[--g.ticket_count];
            send_auth_list_locked();
            break;
        }
    pthread_mutex_unlock(&g.lock);
    char idj[140];
    json_escape(idj, sizeof(idj), id);
    emit("{\"event\":\"cancel_auth_ticket\",\"id\":\"%s\",\"ok\":true,\"handle\":%u}", idj, handle);
}

/* Requests a game process makes for the app it runs. Each is answered from the signed-in session;
 * nothing is produced locally, so with no session the game sees Steam's own "not available". */
static void handle_game_request(const char *op, const char *id, const char *line)
{
    char appid_text[16] = "", idj[140];
    json_get(line, "appid", appid_text, sizeof(appid_text));
    uint32_t appid = (uint32_t)strtoul(appid_text, NULL, 10);
    json_escape(idj, sizeof(idj), id);
    char event[40];
    snprintf(event, sizeof(event), "%s", op);
    pthread_mutex_lock(&g.lock);
    int signed_in = g.state == STATE_SIGNED_IN;
    pthread_mutex_unlock(&g.lock);
    if (!signed_in) { game_error(event, id, "not signed in to Steam"); return; }
    if (!appid || !app_owned(appid)) { game_error(event, id, "this account does not own the app"); return; }

    if (!strcmp(op, "auth_ticket")) { issue_auth_ticket(id, appid); return; }
    if (!strcmp(op, "cancel_auth_ticket")) {
        char handle[16] = "";
        json_get(line, "handle", handle, sizeof(handle));
        cancel_auth_ticket(id, (uint32_t)strtoul(handle, NULL, 10));
        return;
    }
    if (!strcmp(op, "game_session")) {
        size_t count;
        uint32_t *owned = owned_apps(&count);
        size_t cap = 64 + count * 12, len = 0;
        char *list = malloc(cap);
        if (list) {
            list[0] = 0;
            for (size_t i = 0; i < count; i++)
                len += (size_t)snprintf(list + len, cap - len, "%s%u", i ? "," : "", owned[i]);
        }
        free(owned);
        char account[300], persona[300];
        pthread_mutex_lock(&g.lock);
        json_escape(account, sizeof(account), g.account);
        json_escape(persona, sizeof(persona), g.persona);
        uint64_t steamid = g.steamid;
        pthread_mutex_unlock(&g.lock);
        emit("{\"event\":\"game_session\",\"id\":\"%s\",\"ok\":true,\"appid\":%u,\"steamid\":\"%llu\","
             "\"account\":\"%s\",\"persona\":\"%s\",\"owned\":[%s]}",
             idj, appid, (unsigned long long)steamid, account, persona, list ? list : "");
        free(list);
        return;
    }

    PbBuf request = {0};
    uint32_t emsg;
    if (!strcmp(op, "app_ticket")) {
        emsg = EMSG_CLIENT_GET_APP_OWNERSHIP_TICKET;
        pb_u64(&request, 1, appid);
    } else {
        char hex[2100] = "";
        uint8_t userdata[1024];
        json_get(line, "userdata", hex, sizeof(hex));
        size_t n = unhex(hex, userdata, sizeof(userdata));
        emsg = EMSG_CLIENT_REQUEST_ENCRYPTED_APP_TICKET;
        pb_u64(&request, 1, appid);
        if (n) pb_bytes(&request, 2, userdata, n);
    }
    CmPart *parts;
    int answered = cm_request(g.cm, emsg, &request, 0, 20000, &parts);
    pb_free(&request);
    if (!answered) { game_error(event, id, "Steam did not answer"); return; }
    /* Ownership tickets carry eresult in field 1 and the ticket in 3; encrypted tickets carry eresult
     * in 2 and the EncryptedAppTicket message in 3. */
    PbField result, ticket;
    int eresult_field = emsg == EMSG_CLIENT_GET_APP_OWNERSHIP_TICKET ? 1 : 2;
    int eresult = pb_find(parts->body.data, parts->body.len, (uint32_t)eresult_field, &result)
                ? (int)(int32_t)result.value : ERESULT_FAIL;
    if (eresult != ERESULT_OK || !pb_find(parts->body.data, parts->body.len, 3, &ticket) || !ticket.len ||
        ticket.len > 8192) {
        cm_parts_free(parts);
        char error[96];
        snprintf(error, sizeof(error), "Steam refused the ticket (EResult %d: %s)", eresult, cm_eresult_name(eresult));
        game_error(event, id, error);
        return;
    }
    char *hex = malloc(ticket.len * 2 + 1);
    if (hex) {
        hex_text(ticket.data, ticket.len, hex);
        emit("{\"event\":\"%s\",\"id\":\"%s\",\"ok\":true,\"appid\":%u,\"ticket\":\"%s\"}",
             event, idj, appid, hex);
        free(hex);
    }
    cm_parts_free(parts);
}

/* ---------------------------------------------------------------- socket server */

static void handle_request(char *line)
{
    char op[64] = "", id[64] = "";
    json_get(line, "op", op, sizeof(op));
    json_get(line, "id", id, sizeof(id));
    if (!strcmp(op, "status")) {
        emit_state(id);
    } else if (!strcmp(op, "library")) {
        emit_library(id);
    } else if (!strcmp(op, "game_session") || !strcmp(op, "app_ticket") || !strcmp(op, "encrypted_ticket") ||
               !strcmp(op, "auth_ticket") || !strcmp(op, "cancel_auth_ticket")) {
        handle_game_request(op, id, line);
    } else if (!strcmp(op, "installs")) {
        char folders[8192] = "";
        json_get(line, "folders", folders, sizeof(folders));
        emit_installs(id, folders);
    } else if (!strcmp(op, "check_updates")) {
        UpdateCheck *check = calloc(1, sizeof(*check));
        pthread_t thread;
        if (check) {
            snprintf(check->id, sizeof(check->id), "%s", id);
            json_get(line, "folders", check->folders, sizeof(check->folders));
            if (!pthread_create(&thread, NULL, update_check_thread, check)) pthread_detach(thread);
            else free(check);
        }
    } else if (!strcmp(op, "install") || !strcmp(op, "update") || !strcmp(op, "verify") || !strcmp(op, "pause") ||
               !strcmp(op, "resume") || !strcmp(op, "cancel") || !strcmp(op, "uninstall") ||
               !strcmp(op, "downloads") || !strcmp(op, "bandwidth") || !strcmp(op, "move") ||
               !strcmp(op, "app_branches") || !strcmp(op, "beta_password")) {
        handle_download_request(op, id, line);
    } else if (!strcmp(op, "login_qr")) {
        start_login(id, 1, NULL, NULL);
    } else if (!strcmp(op, "login_password")) {
        char account[128] = "", password[512] = "";
        json_get(line, "account", account, sizeof(account));
        json_get(line, "password", password, sizeof(password));
        if (!*account || !*password) emit_login(id, 0, "account name and password are required");
        else start_login(id, 0, account, password);
        memset(password, 0, sizeof(password));
    } else if (!strcmp(op, "guard_code")) {
        char code[64] = "";
        json_get(line, "code", code, sizeof(code));
        pthread_mutex_lock(&g.lock);
        if (g.login_active && *code) {
            secret_wipe(g.guard_code);
            g.guard_code = strdup(code);
            pthread_cond_broadcast(&g.changed);
        }
        pthread_mutex_unlock(&g.lock);
        memset(code, 0, sizeof(code));
    } else if (!strcmp(op, "login_cancel")) {
        pthread_mutex_lock(&g.lock);
        if (g.login_active) { g.login_cancel = 1; pthread_cond_broadcast(&g.changed); }
        pthread_mutex_unlock(&g.lock);
    } else if (!strcmp(op, "logout")) {
        do_logout();
    } else {
        char opj[140], idj[140];
        json_escape(opj, sizeof(opj), op);
        json_escape(idj, sizeof(idj), id);
        emit("{\"event\":\"error\",\"id\":\"%s\",\"ok\":false,\"error\":\"unknown op '%s'\"}", idj, opj);
    }
    /* The line may hold a password. */
    memset(line, 0, strlen(line));
}

static void *client_thread(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char buf[8192];
    size_t len = 0;
    for (;;) {
        ssize_t got = recv(fd, buf + len, sizeof(buf) - 1 - len, 0);
        if (got <= 0) break;
        len += (size_t)got;
        buf[len] = 0;
        char *start = buf, *newline;
        while ((newline = strchr(start, '\n'))) {
            *newline = 0;
            if (newline > start) handle_request(start);
            start = newline + 1;
        }
        len -= (size_t)(start - buf);
        memmove(buf, start, len);
        if (len == sizeof(buf) - 1) len = 0;   /* oversized line: drop it */
    }
    memset(buf, 0, sizeof(buf));
    pthread_mutex_lock(&g.lock);
    for (int i = 0; i < g.client_count; i++)
        if (g.clients[i] == fd) { g.clients[i] = g.clients[--g.client_count]; break; }
    pthread_mutex_unlock(&g.lock);
    close(fd);
    return NULL;
}

static int socket_path(char *out, size_t cap)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    char dir[256];
    if (runtime && *runtime) snprintf(dir, sizeof(dir), "%s/beer", runtime);
    else snprintf(dir, sizeof(dir), "/tmp/beer-%d", (int)getuid());
    if (mkdir(dir, 0700) && errno != EEXIST) return 0;
    chmod(dir, 0700);
    return snprintf(out, cap, "%s/steam.sock", dir) < (int)cap;
}

static int serve(void)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    if (!socket_path(addr.sun_path, sizeof(addr.sun_path))) {
        fprintf(stderr, "beer-steam: no usable runtime directory\n");
        return 1;
    }
    int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (probe >= 0 && connect(probe, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        fprintf(stderr, "beer-steam: already running (%s)\n", addr.sun_path);
        close(probe);
        return 1;
    }
    if (probe >= 0) close(probe);
    unlink(addr.sun_path);
    int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    mode_t old = umask(0177);
    int bound = listener >= 0 && bind(listener, (struct sockaddr *)&addr, sizeof(addr)) == 0;
    umask(old);
    if (!bound || listen(listener, 8)) {
        fprintf(stderr, "beer-steam: cannot listen on %s: %s\n", addr.sun_path, strerror(errno));
        return 1;
    }
    fprintf(stderr, "[STEAM] provider listening on %s (keyring %s)\n", addr.sun_path,
            secret_available() ? "available" : "unavailable");

    pthread_t thread;
    pthread_create(&thread, NULL, supervisor, NULL);
    pthread_detach(thread);
    pthread_create(&thread, NULL, library_worker, NULL);
    pthread_detach(thread);
    resume_stored_session();

    for (;;) {
        int fd = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
        if (fd < 0) { if (errno == EINTR) continue; break; }
        struct ucred cred;
        socklen_t cl = sizeof(cred);
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &cl) || cred.uid != getuid()) { close(fd); continue; }
        pthread_mutex_lock(&g.lock);
        int full = g.client_count >= MAX_CLIENTS;
        if (!full) g.clients[g.client_count++] = fd;
        pthread_mutex_unlock(&g.lock);
        if (full || pthread_create(&thread, NULL, client_thread, (void *)(intptr_t)fd)) {
            if (!full) {
                pthread_mutex_lock(&g.lock);
                g.client_count--;
                pthread_mutex_unlock(&g.lock);
            }
            close(fd);
            continue;
        }
        pthread_detach(thread);
    }
    return 1;
}

/* ---------------------------------------------------------------- terminal commands */

static void read_hidden(const char *prompt, char *out, size_t cap)
{
    fprintf(stderr, "%s", prompt);
    struct termios old, quiet;
    int tty = tcgetattr(STDIN_FILENO, &old) == 0;
    if (tty) { quiet = old; quiet.c_lflag &= ~(tcflag_t)ECHO; tcsetattr(STDIN_FILENO, TCSANOW, &quiet); }
    if (!fgets(out, (int)cap, stdin)) out[0] = 0;
    if (tty) { tcsetattr(STDIN_FILENO, TCSANOW, &old); fprintf(stderr, "\n"); }
    out[strcspn(out, "\r\n")] = 0;
}

static int wait_login(void)
{
    for (;;) {
        pthread_mutex_lock(&g.lock);
        int active = g.login_active;
        pthread_mutex_unlock(&g.lock);
        if (!active) break;
        struct pollfd p = { STDIN_FILENO, POLLIN, 0 };
        if (poll(&p, 1, 250) == 1 && (p.revents & POLLIN)) {
            char code[64];
            if (!fgets(code, sizeof(code), stdin)) { usleep(250000); continue; }
            code[strcspn(code, "\r\n")] = 0;
            pthread_mutex_lock(&g.lock);
            if (*code) { secret_wipe(g.guard_code); g.guard_code = strdup(code); pthread_cond_broadcast(&g.changed); }
            pthread_mutex_unlock(&g.lock);
            memset(code, 0, sizeof(code));
        }
    }
    return g.state == STATE_SIGNED_IN ? 0 : 1;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    const char *command = argc > 1 ? argv[1] : "";
    g.cm = cm_new(on_message, NULL);
    if (!g.cm) return 1;
    g.installer = installer_new(g.cm, on_install_event, NULL);
    if (!g.installer) return 1;
    if (!strcmp(command, "serve")) return serve();

    g.terminal = 1;
    if (!strcmp(command, "status")) {
        int result = resume_stored_session();
        if (result != ERESULT_OK && result != ERESULT_FAIL)
            fprintf(stderr, "stored session not usable: %s\n", cm_eresult_name(result));
        emit_state(NULL);
        cm_logoff(g.cm);
        cm_free(g.cm);
        return 0;
    }
    if (!strcmp(command, "login-qr")) {
        fprintf(stderr, "Encode the url below as a QR code and scan it with the Steam mobile app.\n");
        if (!start_login("cli", 1, NULL, NULL)) return 1;
        int status = wait_login();
        cm_logoff(g.cm);
        cm_free(g.cm);
        return status;
    }
    if (!strcmp(command, "login") && argc > 2) {
        char password[512];
        read_hidden("Steam password: ", password, sizeof(password));
        fprintf(stderr, "If Steam asks for a code, type it and press Enter.\n");
        int started = start_login("cli", 0, argv[2], password);
        memset(password, 0, sizeof(password));
        int status = started ? wait_login() : 1;
        cm_logoff(g.cm);
        cm_free(g.cm);
        return status;
    }
    if (!strcmp(command, "library")) {
        g.terminal = 0;
        pthread_t thread;
        pthread_create(&thread, NULL, library_worker, NULL);
        pthread_detach(thread);
        int result = resume_stored_session();
        if (result != ERESULT_OK) {
            fprintf(stderr, "not signed in: %s\n", cm_eresult_name(result));
            return 1;
        }
        time_t deadline = time(NULL) + 180;
        pthread_mutex_lock(&g.lock);
        while (time(NULL) < deadline && (g.library_wanted || g.library_busy || !g.license_count ||
                                         (!g.library_error[0] && (!g.library || g.library_cached)))) {
            struct timespec wait;
            clock_gettime(CLOCK_REALTIME, &wait);
            wait.tv_sec += 1;
            pthread_cond_timedwait(&g.changed, &g.lock, &wait);
        }
        int ok = g.library && !g.library_cached;
        if (ok) printf("%s\n", g.library);
        else fprintf(stderr, "library failed: %s\n", g.library_error[0] ? g.library_error : "timed out");
        pthread_mutex_unlock(&g.lock);
        cm_logoff(g.cm);
        return ok ? 0 : 1;
    }
    if (!strcmp(command, "logout")) {
        secret_clear_session();
        fprintf(stderr, "Stored Steam session removed.\n");
        return 0;
    }
    fprintf(stderr, "usage: beer-steam serve | status | login-qr | login <account> | library | logout\n");
    return 2;
}
