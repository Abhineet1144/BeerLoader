#include "beer_paths.h"
#include "beer_control.h"
#include "beer_time.h"
#include "beer_memory.h"
#include "beer_overlay.h"
#include "beer_post.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

extern void bd12_cbt_handle(const char *cmd, const char *request, char *out, size_t size);

/* ---- settings registry ---------------------------------------------------------------------- */

typedef enum { SET_FLOAT, SET_BOOL, SET_INT } SettingType;

typedef struct {
    const char *name, *label, *description;
    SettingType type;
    int32_t min, max, def;      /* milli units for floats, 0/1 for bools */
    _Atomic int32_t value;
} Setting;

static Setting settings[] = {
    { "time_scale", "Game speed", "Multiplier for the clocks the game sees (0.05 = very slow, 10 = ten times faster).",
      SET_FLOAT, 50, 10000, 1000, 1000 },
    { "fps_limit", "Frame cap", "Maximum frames per second the game may present (0 = no cap). Applies to D3D12 games.",
      SET_INT, 0, 1000, 0, 0 },
    { "paused", "Pause game time", "Freezes the clocks the game sees. Rendering and input keep running.",
      SET_BOOL, 0, 1, 0, 0 },
};
#define SETTING_COUNT (sizeof(settings) / sizeof(settings[0]))

static void apply_time(void)
{
    const int paused = atomic_load(&settings[2].value);
    /* Paused = clocks frozen and threads asking the time parked (see beer_time.h); resuming restores the speed. */
    /* Order matters: close the gate before freezing the clock, and restore the clock before opening the gate, so no
     * thread can ever read a frozen clock without being parked first (that made the game hang on resume). */
    if (paused) {
        beer_time_set_paused(1);
        beer_time_set_scale_permille(0u);
    } else {
        beer_time_set_scale_permille((uint32_t)atomic_load(&settings[0].value));
        beer_time_set_paused(0);
    }
}

/* ---- process statistics ---------------------------------------------------------------------- */

static char g_exe[512];
static uint64_t g_start_ns;
static _Atomic uint64_t g_frames;

static uint64_t wall_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void beer_control_note_frame(void)
{
    atomic_fetch_add_explicit(&g_frames, 1, memory_order_relaxed);
    beer_overlay_note_frame();
}

uint64_t beer_control_frames(void) { return atomic_load_explicit(&g_frames, memory_order_relaxed); }
uint64_t beer_control_uptime_s(void) { return g_start_ns ? (wall_ns() - g_start_ns) / 1000000000ull : 0; }
int beer_control_time_scale_milli(void) { return atomic_load(&settings[0].value); }
int beer_control_paused(void) { return atomic_load(&settings[2].value); }

/* Frame cap: called once per presented frame, sleeps (coarse nanosleep, then a short spin) so frames
 * are at least 1/limit apart. Uses the real monotonic clock, not the scaled game clock. */
void beer_control_frame_limit_wait(void)
{
    static uint64_t next_deadline;
    const int32_t limit = atomic_load_explicit(&settings[1].value, memory_order_relaxed);
    if (limit <= 0) { next_deadline = 0; return; }
    const uint64_t interval = 1000000000ull / (uint64_t)limit;
    uint64_t now = wall_ns();
    if (!next_deadline || now > next_deadline + interval) {   /* first frame, or we fell behind: restart the pacing */
        next_deadline = now + interval;
        return;
    }
    while (now < next_deadline) {
        const uint64_t left = next_deadline - now;
        if (left > 400000ull) {
            struct timespec ts = { 0, (long)(left - 300000ull) };
            nanosleep(&ts, NULL);
        }
        now = wall_ns();
    }
    next_deadline += interval;
}

typedef struct { uint64_t cpu_ticks; uint64_t at_ns; uint64_t frames; } Sample;

void beer_control_proc_stats(uint64_t *cpu_ticks, uint64_t *rss_kb, uint64_t *threads);
static void read_proc(uint64_t *cpu_ticks, uint64_t *rss_kb, uint64_t *threads)
{
    *cpu_ticks = *rss_kb = *threads = 0;
    FILE *f = fopen("/proc/self/stat", "r");
    if (f) {
        char line[1024];
        if (fgets(line, sizeof(line), f)) {
            const char *p = strrchr(line, ')');
            if (p) {
                unsigned long long utime = 0, stime = 0, nthreads = 0;
                long long rss_pages = 0;
                /* fields after the comm: state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime ... */
                if (sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu %*d %*d %*d %*d %llu %*d %*u %*u %lld",
                           &utime, &stime, &nthreads, &rss_pages) >= 3) {
                    *cpu_ticks = utime + stime;
                    *threads = nthreads;
                    *rss_kb = (uint64_t)(rss_pages > 0 ? rss_pages : 0) * (uint64_t)(sysconf(_SC_PAGESIZE) / 1024);
                }
            }
        }
        fclose(f);
    }
}

/* ---- tiny JSON helpers ---------------------------------------------------------------------- */

/* Finds the raw text after "key": ; returns pointer to the value start or NULL. */
const char *beer_json_find(const char *json, const char *key)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return NULL;
    p += strlen(needle);
    while (*p && isspace((unsigned char)*p)) ++p;
    if (*p != ':') return NULL;
    ++p;
    while (*p && isspace((unsigned char)*p)) ++p;
    return p;
}

int beer_json_string(const char *json, const char *key, char *out, size_t size)
{
    const char *p = beer_json_find(json, key);
    if (!p || *p != '"') return 0;
    ++p;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < size) {
        if (*p == '\\' && p[1]) ++p;
        out[i++] = *p++;
    }
    out[i] = 0;
    return 1;
}

/* Parses a JSON number (or true/false) into milli units without floating point. */
int beer_json_milli(const char *json, const char *key, int32_t *out)
{
    const char *p = beer_json_find(json, key);
    if (!p) return 0;
    if (!strncmp(p, "true", 4)) { *out = 1000; return 1; }
    if (!strncmp(p, "false", 5)) { *out = 0; return 1; }
    int negative = 0;
    if (*p == '-') { negative = 1; ++p; }
    if (!isdigit((unsigned char)*p) && *p != '.') return 0;
    int64_t whole = 0;
    while (isdigit((unsigned char)*p)) { whole = whole * 10 + (*p - '0'); if (whole > 1000000) return 0; ++p; }
    int64_t frac = 0, scale = 100;
    if (*p == '.') {
        ++p;
        while (isdigit((unsigned char)*p)) { if (scale) { frac += (*p - '0') * scale; scale /= 10; } ++p; }
    }
    const int64_t value = whole * 1000 + frac;
    *out = (int32_t)(negative ? -value : value);
    return 1;
}

static void format_milli(char *out, size_t size, int32_t milli)
{
    snprintf(out, size, "%s%d.%03d", milli < 0 ? "-" : "", (int)(abs(milli) / 1000), (int)(abs(milli) % 1000));
}

static void json_escape(char *out, size_t size, const char *text)
{
    size_t i = 0;
    for (; *text && i + 2 < size; ++text) {
        if (*text == '"' || *text == '\\') out[i++] = '\\';
        if ((unsigned char)*text < 0x20) continue;
        out[i++] = *text;
    }
    out[i] = 0;
}

/* ---- request handling ---------------------------------------------------------------------- */

static void reply_error(char *out, size_t size, const char *message)
{
    snprintf(out, size, "{\"ok\":false,\"error\":\"%s\"}\n", message);
}

static void setting_json(char *out, size_t size, const Setting *s)
{
    char value[24], min[24], max[24], def[24], desc[256];
    json_escape(desc, sizeof(desc), s->description);
    if (s->type == SET_FLOAT) {
        format_milli(value, sizeof(value), atomic_load(&s->value));
        format_milli(min, sizeof(min), s->min);
        format_milli(max, sizeof(max), s->max);
        format_milli(def, sizeof(def), s->def);
        snprintf(out, size, "{\"name\":\"%s\",\"label\":\"%s\",\"description\":\"%s\",\"type\":\"float\",\"min\":%s,\"max\":%s,\"default\":%s,\"value\":%s,\"live\":true}",
                 s->name, s->label, desc, min, max, def, value);
    } else if (s->type == SET_INT) {
        snprintf(out, size, "{\"name\":\"%s\",\"label\":\"%s\",\"description\":\"%s\",\"type\":\"int\",\"min\":%d,\"max\":%d,\"default\":%d,\"value\":%d,\"live\":true}",
                 s->name, s->label, desc, (int)s->min, (int)s->max, (int)s->def, (int)atomic_load(&s->value));
    } else {
        snprintf(out, size, "{\"name\":\"%s\",\"label\":\"%s\",\"description\":\"%s\",\"type\":\"bool\",\"default\":%s,\"value\":%s,\"live\":true}",
                 s->name, s->label, desc, s->def ? "true" : "false", atomic_load(&s->value) ? "true" : "false");
    }
}

#define REPLY_SIZE (256 * 1024)
typedef struct { Sample last; } Session;

static void handle(Session *session, const char *request, char *out, size_t size)
{
    char cmd[32] = "";
    if (!beer_json_string(request, "cmd", cmd, sizeof(cmd))) { reply_error(out, size, "missing cmd"); return; }
    if (!strcmp(cmd, "hello")) {
        char exe[600];
        json_escape(exe, sizeof(exe), g_exe);
        snprintf(out, size, "{\"ok\":true,\"protocol\":1,\"pid\":%d,\"exe\":\"%s\"}\n", (int)getpid(), exe);
    } else if (!strcmp(cmd, "status")) {
        uint64_t ticks, rss, threads;
        read_proc(&ticks, &rss, &threads);
        const uint64_t now = wall_ns();
        const uint64_t frames = atomic_load(&g_frames);
        uint64_t cpu_x10 = 0, fps_x10 = 0, cpu_total_x10 = 0;
        if (session->last.at_ns && now > session->last.at_ns) {
            const uint64_t dt = now - session->last.at_ns;
            const uint64_t hz = (uint64_t)sysconf(_SC_CLK_TCK);
            cpu_x10 = (ticks - session->last.cpu_ticks) * 1000000000ull * 1000 / hz / dt;   /* percent*10 */
            fps_x10 = (frames - session->last.frames) * 10000000000ull / dt;
            long logical = sysconf(_SC_NPROCESSORS_ONLN);
            cpu_total_x10 = cpu_x10 / (uint64_t)(logical > 0 ? logical : 1);   /* share of the whole machine */
        }
        session->last = (Sample){ ticks, now, frames };
        char scale[24];
        format_milli(scale, sizeof(scale), (int32_t)atomic_load(&settings[0].value));
        snprintf(out, size,
                 "{\"ok\":true,\"pid\":%d,\"uptime_s\":%llu,\"frames\":%llu,\"fps\":%llu.%llu,\"cpu_percent\":%llu.%llu,\"cpu_cores\":%llu.%llu,\"rss_mb\":%llu,\"threads\":%llu,\"time_scale\":%s,\"fps_limit\":%d,\"paused\":%s}\n",
                 (int)getpid(), (unsigned long long)((now - g_start_ns) / 1000000000ull), (unsigned long long)frames,
                 (unsigned long long)(fps_x10 / 10), (unsigned long long)(fps_x10 % 10),
                 (unsigned long long)(cpu_total_x10 / 10), (unsigned long long)(cpu_total_x10 % 10),
                 (unsigned long long)(cpu_x10 / 1000), (unsigned long long)((cpu_x10 / 100) % 10),
                 (unsigned long long)(rss / 1024), (unsigned long long)threads, scale,
                 (int)atomic_load(&settings[1].value), atomic_load(&settings[2].value) ? "true" : "false");
    } else if (!strcmp(cmd, "settings")) {
        size_t used = (size_t)snprintf(out, size, "{\"ok\":true,\"settings\":[");
        for (size_t i = 0; i < SETTING_COUNT && used + 600 < size; ++i) {
            if (i) out[used++] = ',';
            char one[600];
            setting_json(one, sizeof(one), &settings[i]);
            used += (size_t)snprintf(out + used, size - used, "%s", one);
        }
        snprintf(out + used, size - used, "]}\n");
    } else if (!strcmp(cmd, "set")) {
        char name[48] = "";
        int32_t value;
        if (!beer_json_string(request, "name", name, sizeof(name)) || !beer_json_milli(request, "value", &value)) {
            reply_error(out, size, "set needs name and value");
            return;
        }
        for (size_t i = 0; i < SETTING_COUNT; ++i) {
            if (strcmp(settings[i].name, name)) continue;
            if (settings[i].type == SET_BOOL) value = value != 0;
            else if (settings[i].type == SET_INT) value /= 1000;
            if (value < settings[i].min) value = settings[i].min;
            if (value > settings[i].max) value = settings[i].max;
            atomic_store(&settings[i].value, value);
            apply_time();
            char one[600];
            setting_json(one, sizeof(one), &settings[i]);
            snprintf(out, size, "{\"ok\":true,\"setting\":%s}\n", one);
            fprintf(stderr, "[CONTROL] %s = %d (milli)\n", name, (int)value);
            return;
        }
        reply_error(out, size, "unknown setting");
    } else if (!strncmp(cmd, "gfx_", 4)) {
        bd12_cbt_handle(cmd, request, out, size);
    } else if (!strncmp(cmd, "post_", 5) || !strcmp(cmd, "screenshot")) {
        beer_post_handle(cmd, request, out, size);
    } else if (!strncmp(cmd, "overlay_", 8)) {
        beer_overlay_handle(cmd, request, out, size);
    } else if (!strncmp(cmd, "mem_", 4)) {
        beer_memory_handle(cmd, request, out, size);
    } else {
        reply_error(out, size, "unknown cmd");
    }
}

static void *client_thread(void *opaque)
{
    const int fd = (int)(intptr_t)opaque;
    Session session = {0};
    char *reply = malloc(REPLY_SIZE);
    if (!reply) { close(fd); return NULL; }
    char buffer[32768];
    size_t have = 0;
    for (;;) {
        const ssize_t got = read(fd, buffer + have, sizeof(buffer) - 1 - have);
        if (got <= 0) break;
        have += (size_t)got;
        buffer[have] = 0;
        char *newline;
        while ((newline = memchr(buffer, '\n', have))) {
            *newline = 0;
            handle(&session, buffer, reply, REPLY_SIZE);
            const size_t length = strlen(reply);
            size_t sent = 0;
            while (sent < length) {
                const ssize_t w = write(fd, reply + sent, length - sent);
                if (w <= 0) { free(reply); close(fd); return NULL; }
                sent += (size_t)w;
            }
            const size_t consumed = (size_t)(newline - buffer) + 1;
            memmove(buffer, buffer + consumed, have - consumed);
            have -= consumed;
        }
        if (have >= sizeof(buffer) - 1) have = 0;   /* oversized line: drop it */
    }
    free(reply);
    close(fd);
    return NULL;
}

static char g_socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
static int g_listen_fd = -1;

static void control_cleanup(void)
{
    if (g_listen_fd >= 0) close(g_listen_fd);
    if (g_socket_path[0]) unlink(g_socket_path);
}

static void *accept_thread(void *opaque)
{
    (void)opaque;
    for (;;) {
        const int fd = accept(g_listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        pthread_t thread;
        if (pthread_create(&thread, NULL, client_thread, (void *)(intptr_t)fd) == 0) pthread_detach(thread);
        else close(fd);
    }
    return NULL;
}

void beer_control_start(const char *exe_path)
{
    if (g_listen_fd >= 0) return;
    snprintf(g_exe, sizeof(g_exe), "%s", exe_path ? exe_path : "");
    g_start_ns = wall_ns();
    const char *initial = getenv("BEER_TIME_SCALE");
    if (initial) {
        char json[64];
        int32_t milli;
        snprintf(json, sizeof(json), "{\"v\":%s}", initial);
        if (beer_json_milli(json, "v", &milli) && milli >= settings[0].min && milli <= settings[0].max) {
            atomic_store(&settings[0].value, milli);
            apply_time();
        }
    }
    const char *cap = getenv("BEER_FPS_LIMIT");
    if (cap && atoi(cap) > 0 && atoi(cap) <= settings[1].max) atomic_store(&settings[1].value, atoi(cap));
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    char directory[256];
    if (runtime && *runtime) snprintf(directory, sizeof(directory), "%s/beer", runtime);
    else snprintf(directory, sizeof(directory), "%s/beer-%d", beer_tmp_dir(), (int)getuid());
    mkdir(directory, 0700);
    chmod(directory, 0700);
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    if (snprintf(address.sun_path, sizeof(address.sun_path), "%s/%d.sock", directory, (int)getpid()) >= (int)sizeof(address.sun_path)) {
        fprintf(stderr, "[CONTROL] socket path too long\n");
        return;
    }
    snprintf(g_socket_path, sizeof(g_socket_path), "%s", address.sun_path);
    unlink(g_socket_path);
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(fd, 4) != 0) {
        fprintf(stderr, "[CONTROL] cannot open %s: %s\n", g_socket_path, strerror(errno));
        if (fd >= 0) close(fd);
        g_socket_path[0] = 0;
        return;
    }
    chmod(g_socket_path, 0600);
    g_listen_fd = fd;
    atexit(control_cleanup);
    pthread_t thread;
    if (pthread_create(&thread, NULL, accept_thread, NULL) == 0) pthread_detach(thread);
    fprintf(stderr, "[CONTROL] listening on %s\n", g_socket_path);
}

void beer_control_proc_stats(uint64_t *cpu_ticks, uint64_t *rss_kb, uint64_t *threads)
{
    read_proc(cpu_ticks, rss_kb, threads);
}
