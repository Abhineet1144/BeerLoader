#include "beer_control.h"
#include "beer_time.h"
#include "beer_memory.h"
#include "beer_objects.h"
#include "beer_overlay.h"
#include "beer_post.h"

#include <ctype.h>
#include <errno.h>
#include <malloc.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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
    { "fps_limit", "Frame cap", "Maximum frames per second the game may present (0 = no cap). Applies to D3D12 and Vulkan games.",
      SET_INT, 0, 1000, 0, 0 },
    { "paused", "Pause game time", "Freezes the clocks the game sees. Rendering and input keep running.",
      SET_BOOL, 0, 1, 0, 0 },
    /* DLSS overrides (platform/win32_ngx.inc), read whenever the game asks DLSS for its render size. */
    { "dlss_ratio", "DLSS internal resolution", "Render share of the output in percent (0 = the game's choice, 10..100).",
      SET_FLOAT, 0, 100000, 0, 0 },
    { "dlss_width", "DLSS render width", "Exact render width; with dlss_height overrides dlss_ratio (0 = off).",
      SET_INT, 0, 16384, 0, 0 },
    { "dlss_height", "DLSS render height", "Exact render height (0 = off).", SET_INT, 0, 16384, 0, 0 },
    { "dlss_preset", "DLSS render preset", "DLSS model preset: 1 = A, 10 = J, 11 = K ... 0 = the DLL default, -1 = the game's choice.",
      SET_INT, -1, 26, -1, -1 },
    { "dlss_bypass", "DLSS off (no upscaling)", "Shows the game's internal-resolution image stretched to the output instead of DLSS's result.",
      SET_BOOL, 0, 1, 0, 0 },
    /* Low-memory guard (memguard_thread below). */
    { "memguard", "Low-memory guard", "Pauses the game and shows a warning when memory is about to run out.",
      SET_BOOL, 0, 1, 1, 1 },
    { "memguard_free_mb", "Pause below free RAM (MB)", "System memory available (MemAvailable) below which the game is paused (0 = off).",
      SET_INT, 0, 65536, 1024, 1024 },
    { "memguard_margin_mb", "Pause near the memory cap (MB)", "When the game runs under a memory cap: pause when less than this is left (0 = off).",
      SET_INT, 0, 65536, 300, 300 },
    { "memguard_auto_resume", "Resume automatically", "Continue by itself once memory has recovered.",
      SET_BOOL, 0, 1, 0, 0 },
};
enum { SET_PAUSED = 2, SET_FPS = 1, SET_MEMGUARD = 8, SET_MG_FREE = 9, SET_MG_MARGIN = 10, SET_MG_AUTO = 11 };
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

/* Frame pacing since the last status request: the longest present-to-present gap and the number over 50 ms
 * (a visible hitch).  The average fps hides stutter. */
static _Atomic uint64_t g_last_present_ns, g_max_frame_ns, g_hitches;

void beer_control_note_frame(void)
{
    atomic_fetch_add_explicit(&g_frames, 1, memory_order_relaxed);
    const uint64_t now = wall_ns();
    const uint64_t last = atomic_exchange_explicit(&g_last_present_ns, now, memory_order_relaxed);
    if (last && now > last) {
        const uint64_t gap = now - last;
        uint64_t max = atomic_load_explicit(&g_max_frame_ns, memory_order_relaxed);
        while (gap > max && !atomic_compare_exchange_weak_explicit(&g_max_frame_ns, &max, gap, memory_order_relaxed,
                                                                   memory_order_relaxed)) {}
        if (gap > 50000000ull) atomic_fetch_add_explicit(&g_hitches, 1, memory_order_relaxed);
    }
    beer_overlay_note_frame();
}

uint64_t beer_control_frames(void) { return atomic_load_explicit(&g_frames, memory_order_relaxed); }
uint64_t beer_control_uptime_s(void) { return g_start_ns ? (wall_ns() - g_start_ns) / 1000000000ull : 0; }
int beer_control_time_scale_milli(void) { return atomic_load(&settings[0].value); }
int beer_control_paused(void) { return atomic_load(&settings[2].value); }
void beer_control_dlss(int32_t *ratio_milli_percent, int32_t *width, int32_t *height, int32_t *preset)
{
    *ratio_milli_percent = atomic_load(&settings[3].value);
    *width = atomic_load(&settings[4].value);
    *height = atomic_load(&settings[5].value);
    *preset = atomic_load(&settings[6].value);
}
int beer_control_dlss_bypass(void) { return atomic_load(&settings[7].value); }

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

static int json_hex4(const char *p, unsigned *out)
{
    unsigned v = 0;
    for (int k = 0; k < 4; ++k) {
        const char c = p[k];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

int beer_json_string(const char *json, const char *key, char *out, size_t size)
{
    const char *p = beer_json_find(json, key);
    if (!p || *p != '"') return 0;
    ++p;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < size) {
        if (*p != '\\' || !p[1]) { out[i++] = *p++; continue; }
        ++p;
        const char e = *p++;
        if (e == 'n') out[i++] = '\n';
        else if (e == 't') out[i++] = '\t';
        else if (e == 'r') out[i++] = '\r';
        else if (e == 'b') out[i++] = '\b';
        else if (e == 'f') out[i++] = '\f';
        else if (e == 'u') {
            unsigned cp = 0;
            if (!json_hex4(p, &cp)) break;
            p += 4;
            if (cp >= 0xd800 && cp < 0xdc00 && p[0] == '\\' && p[1] == 'u') {   /* surrogate pair */
                unsigned lo = 0;
                if (json_hex4(p + 2, &lo) && lo >= 0xdc00 && lo < 0xe000) {
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                    p += 6;
                }
            }
            char utf8[4];
            size_t n = 0;
            if (cp < 0x80) utf8[n++] = (char)cp;
            else if (cp < 0x800) { utf8[n++] = (char)(0xc0 | cp >> 6); utf8[n++] = (char)(0x80 | (cp & 0x3f)); }
            else if (cp < 0x10000) {
                utf8[n++] = (char)(0xe0 | cp >> 12); utf8[n++] = (char)(0x80 | ((cp >> 6) & 0x3f));
                utf8[n++] = (char)(0x80 | (cp & 0x3f));
            } else {
                utf8[n++] = (char)(0xf0 | cp >> 18); utf8[n++] = (char)(0x80 | ((cp >> 12) & 0x3f));
                utf8[n++] = (char)(0x80 | ((cp >> 6) & 0x3f)); utf8[n++] = (char)(0x80 | (cp & 0x3f));
            }
            if (i + n >= size) break;
            memcpy(out + i, utf8, n);
            i += n;
        } else out[i++] = e;          /* \" \\ \/ */
    }
    out[i] = 0;
    return 1;
}

/* Parses a JSON number (or true/false) into milli units without floating point. */
int beer_read_config(const char *name, char *out, size_t size)
{
    const char *home = getenv("HOME");
    char path[1024];
    if (!home || !*home || !size || snprintf(path, sizeof(path), "%s/.beer/%s", home, name) >= (int)sizeof(path))
        return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(out, 1, size - 1, f);
    fclose(f);
    out[n] = 0;
    return n > 0;
}

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

void beer_control_inject_key(unsigned vk, unsigned dik, int extended, int down);   /* loader.c */

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

/* ---- low-memory guard ---------------------------------------------------------------------- */
/* Every 0.5 s: system MemAvailable and, when the game runs under a cgroup memory cap, the room left under the lower of
 * memory.max / memory.high. Below the thresholds the game is paused (game clocks frozen, frames capped to 10 so the
 * engine does little and allocates little), a banner is drawn on screen and the manager offers Continue. Continue
 * (memguard_resume, or unpausing) resumes and holds the guard off for 60 s; with auto-resume it also resumes once
 * memory is back above the threshold plus 512 MB (or twice the cap margin). */
static struct {
    _Atomic int active;
    int saved_fps;
    uint64_t pause_at_ns;          /* the clocks freeze here: the banner is drawn into a few frames first */
    uint64_t quiet_until_ns;
    uint64_t avail_mb, total_mb, cg_used_mb, cg_limit_mb;
    char reason[96];
} g_mg;
static pthread_mutex_t g_mg_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t read_u64_file(const char *path, int *unlimited)
{
    char text[64] = "";
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(text, sizeof(text), f)) text[0] = 0;
    fclose(f);
    if (unlimited) *unlimited = !strncmp(text, "max", 3);
    return strtoull(text, NULL, 10);
}

static void mg_sample(void)
{
    uint64_t avail = 0, total = 0;
    FILE *f = fopen("/proc/meminfo", "r");
    char line[256];
    while (f && fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "MemTotal:", 9)) total = strtoull(line + 9, NULL, 10) >> 10;
        else if (!strncmp(line, "MemAvailable:", 13)) avail = strtoull(line + 13, NULL, 10) >> 10;
    }
    if (f) fclose(f);
    uint64_t used = 0, limit = 0;
    f = fopen("/proc/self/cgroup", "r");
    if (f && fgets(line, sizeof(line), f) && !strncmp(line, "0::", 3)) {
        line[strcspn(line, "\n")] = 0;
        char path[600];
        int unlimited = 1;
        snprintf(path, sizeof(path), "/sys/fs/cgroup%s/memory.current", line + 3);
        uint64_t current = read_u64_file(path, NULL);
        /* memory.current includes the page cache of files the game read (several GB for big games); the kernel drops
         * it as needed, so it is not pressure: subtract reclaimable file pages (file minus shmem). */
        snprintf(path, sizeof(path), "/sys/fs/cgroup%s/memory.stat", line + 3);
        FILE *stat = fopen(path, "r");
        uint64_t file = 0, shmem = 0;
        char row[128];
        while (stat && fgets(row, sizeof(row), stat)) {
            if (!strncmp(row, "file ", 5)) file = strtoull(row + 5, NULL, 10);
            else if (!strncmp(row, "shmem ", 6)) shmem = strtoull(row + 6, NULL, 10);
        }
        if (stat) fclose(stat);
        const uint64_t cache = file > shmem ? file - shmem : 0;
        used = (current > cache ? current - cache : 0) >> 20;
        snprintf(path, sizeof(path), "/sys/fs/cgroup%s/memory.max", line + 3);
        uint64_t max = read_u64_file(path, &unlimited);
        if (!unlimited && max) limit = max >> 20;
        snprintf(path, sizeof(path), "/sys/fs/cgroup%s/memory.high", line + 3);
        uint64_t high = read_u64_file(path, &unlimited);
        if (!unlimited && high && (!limit || (high >> 20) < limit)) limit = high >> 20;
    }
    if (f) fclose(f);
    g_mg.avail_mb = avail; g_mg.total_mb = total; g_mg.cg_used_mb = used; g_mg.cg_limit_mb = limit;
}

static void mg_banner(void)
{
    char text[400];
    if (g_mg.cg_limit_mb)
        snprintf(text, sizeof(text), "\x01LOW MEMORY - game paused\x02\n%s\nRAM free: %llu MB of %llu MB\n"
                 "Game: %llu MB of its %llu MB cap\nClose other apps, then press Continue in Beer Manager",
                 g_mg.reason, (unsigned long long)g_mg.avail_mb, (unsigned long long)g_mg.total_mb,
                 (unsigned long long)g_mg.cg_used_mb, (unsigned long long)g_mg.cg_limit_mb);
    else
        snprintf(text, sizeof(text), "\x01LOW MEMORY - game paused\x02\n%s\nRAM free: %llu MB of %llu MB\n"
                 "Close other apps, then press Continue in Beer Manager",
                 g_mg.reason, (unsigned long long)g_mg.avail_mb, (unsigned long long)g_mg.total_mb);
    beer_overlay_set_alert(text);
}

static void mg_engage(const char *reason)
{
    pthread_mutex_lock(&g_mg_lock);
    if (!atomic_load(&g_mg.active)) {
        snprintf(g_mg.reason, sizeof(g_mg.reason), "%s", reason);
        g_mg.saved_fps = atomic_load(&settings[SET_FPS].value);
        atomic_store(&settings[SET_FPS].value, 10);
        /* Pausing parks the threads that read the clock, render thread included, so presents stop: show the banner
         * for a few frames first; the last frame (with the banner) then stays on screen while paused. */
        mg_banner();
        g_mg.pause_at_ns = wall_ns() + 700000000ull;
        atomic_store(&g_mg.active, 1);
        fprintf(stderr, "[MEMGUARD] paused: %s (free %llu MB, cgroup %llu/%llu MB)\n", reason,
                (unsigned long long)g_mg.avail_mb, (unsigned long long)g_mg.cg_used_mb, (unsigned long long)g_mg.cg_limit_mb);
    }
    pthread_mutex_unlock(&g_mg_lock);
}

static void mg_release(const char *why, int quiet)
{
    pthread_mutex_lock(&g_mg_lock);
    if (atomic_load(&g_mg.active)) {
        atomic_store(&g_mg.active, 0);
        g_mg.pause_at_ns = 0;
        if (atomic_load(&settings[SET_FPS].value) == 10) atomic_store(&settings[SET_FPS].value, g_mg.saved_fps);
        atomic_store(&settings[SET_PAUSED].value, 0);
        apply_time();
        beer_overlay_set_alert("");
        if (quiet) g_mg.quiet_until_ns = wall_ns() + 60ull * 1000000000ull;
        fprintf(stderr, "[MEMGUARD] resumed (%s; free %llu MB)\n", why, (unsigned long long)g_mg.avail_mb);
    }
    pthread_mutex_unlock(&g_mg_lock);
}

static void *memguard_thread(void *unused)
{
    (void)unused;
    for (;;) {
        struct timespec half = { 0, 500000000L };
        nanosleep(&half, NULL);
        mg_sample();
        const uint64_t free_mb = (uint64_t)atomic_load(&settings[SET_MG_FREE].value);
        const uint64_t margin = (uint64_t)atomic_load(&settings[SET_MG_MARGIN].value);
        const int low_ram = free_mb && g_mg.avail_mb && g_mg.avail_mb < free_mb;
        const int near_cap = margin && g_mg.cg_limit_mb && g_mg.cg_used_mb + margin > g_mg.cg_limit_mb;
        if (atomic_load(&g_mg.active)) {
            pthread_mutex_lock(&g_mg_lock);
            if (atomic_load(&g_mg.active) && g_mg.pause_at_ns && wall_ns() >= g_mg.pause_at_ns) {
                g_mg.pause_at_ns = 0;
                atomic_store(&settings[SET_PAUSED].value, 1);
                apply_time();
            }
            pthread_mutex_unlock(&g_mg_lock);
            mg_banner();                                   /* live numbers (seen while frames still render) */
            const int recovered = (!free_mb || g_mg.avail_mb > free_mb + 512) &&
                                  (!margin || !g_mg.cg_limit_mb || g_mg.cg_used_mb + 2 * margin < g_mg.cg_limit_mb);
            if (!atomic_load(&settings[SET_MEMGUARD].value)) mg_release("guard turned off", 0);
            else if (recovered && atomic_load(&settings[SET_MG_AUTO].value)) mg_release("memory recovered", 0);
            continue;
        }
        if (!atomic_load(&settings[SET_MEMGUARD].value) || wall_ns() < g_mg.quiet_until_ns ||
            wall_ns() - g_start_ns < 10ull * 1000000000ull)
            continue;
        if (near_cap) mg_engage("The game is close to its memory cap.");
        else if (low_ram) mg_engage("System memory is almost used up.");
    }
    return NULL;
}

static void mg_status_json(char *out, size_t size)
{
    char reason[200];
    json_escape(reason, sizeof(reason), g_mg.reason);
    snprintf(out, size, "{\"active\":%s,\"reason\":\"%s\",\"avail_mb\":%llu,\"total_mb\":%llu,\"cg_used_mb\":%llu,"
             "\"cg_limit_mb\":%llu}", atomic_load(&g_mg.active) ? "true" : "false", atomic_load(&g_mg.active) ? reason : "",
             (unsigned long long)g_mg.avail_mb, (unsigned long long)g_mg.total_mb,
             (unsigned long long)g_mg.cg_used_mb, (unsigned long long)g_mg.cg_limit_mb);
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
    } else if (!strcmp(cmd, "trim")) {
        /* Hands free memory inside the host allocator's arenas back to the system. */
        uint64_t ticks, before, after, threads;
        read_proc(&ticks, &before, &threads);
        const uint64_t start = wall_ns();
        malloc_trim(0);
        const uint64_t took = wall_ns() - start;
        read_proc(&ticks, &after, &threads);
        snprintf(out, size, "{\"ok\":true,\"rss_before_mb\":%llu,\"rss_after_mb\":%llu,\"ms\":%llu}\n",
                 (unsigned long long)(before / 1024), (unsigned long long)(after / 1024), (unsigned long long)(took / 1000000));
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
        const uint64_t max_frame_ns = atomic_exchange(&g_max_frame_ns, 0);
        const uint64_t hitches = atomic_exchange(&g_hitches, 0);
        char scale[24], memguard[400];
        format_milli(scale, sizeof(scale), (int32_t)atomic_load(&settings[0].value));
        mg_status_json(memguard, sizeof(memguard));
        snprintf(out, size,
                 "{\"ok\":true,\"pid\":%d,\"uptime_s\":%llu,\"frames\":%llu,\"fps\":%llu.%llu,\"cpu_percent\":%llu.%llu,\"cpu_cores\":%llu.%llu,\"rss_mb\":%llu,\"threads\":%llu,\"time_scale\":%s,\"fps_limit\":%d,\"paused\":%s,\"max_frame_ms\":%llu,\"hitches\":%llu,\"memguard\":%s}\n",
                 (int)getpid(), (unsigned long long)((now - g_start_ns) / 1000000000ull), (unsigned long long)frames,
                 (unsigned long long)(fps_x10 / 10), (unsigned long long)(fps_x10 % 10),
                 (unsigned long long)(cpu_total_x10 / 10), (unsigned long long)(cpu_total_x10 % 10),
                 (unsigned long long)(cpu_x10 / 1000), (unsigned long long)((cpu_x10 / 100) % 10),
                 (unsigned long long)(rss / 1024), (unsigned long long)threads, scale,
                 (int)atomic_load(&settings[1].value), atomic_load(&settings[2].value) ? "true" : "false",
                 (unsigned long long)(max_frame_ns / 1000000u), (unsigned long long)hitches, memguard);
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
            if (i == SET_PAUSED && !value && atomic_load(&g_mg.active)) mg_release("unpaused", 1);   /* = Continue */
            else if (i == 0 || i == 2) apply_time();     /* only the clock settings (time_scale, paused) */
            char one[600];
            setting_json(one, sizeof(one), &settings[i]);
            snprintf(out, size, "{\"ok\":true,\"setting\":%s}\n", one);
            fprintf(stderr, "[CONTROL] %s = %d (milli)\n", name, (int)value);
            return;
        }
        reply_error(out, size, "unknown setting");
    } else if (!strcmp(cmd, "memguard_resume")) {   /* the manager's Continue */
        mg_release("Continue", 1);
        snprintf(out, size, "{\"ok\":true}\n");
    } else if (!strcmp(cmd, "key")) {
        /* {"cmd":"key","vk":13,"dik":28,"down":1}: one key transition, delivered like a real keyboard event
         * (lets scripts drive menus in headless runs and on compositors that ignore synthetic X input). */
        int32_t vk = 0, dik = 0, down = 1000, extended = 0;
        if (!beer_json_milli(request, "vk", &vk)) { reply_error(out, size, "key needs vk"); return; }
        beer_json_milli(request, "dik", &dik);
        beer_json_milli(request, "down", &down);
        beer_json_milli(request, "extended", &extended);
        beer_control_inject_key((unsigned)(vk / 1000), (unsigned)(dik / 1000), extended != 0, down != 0);
        snprintf(out, size, "{\"ok\":true}\n");
    } else if (!strncmp(cmd, "gfx_", 4)) {
        bd12_cbt_handle(cmd, request, out, size);
    } else if (!strncmp(cmd, "post_", 5) || !strcmp(cmd, "screenshot")) {
        beer_post_handle(cmd, request, out, size);
    } else if (!strncmp(cmd, "overlay_", 8) || !strncmp(cmd, "hotkey_", 7)) {
        beer_overlay_handle(cmd, request, out, size);
    } else if (!strncmp(cmd, "mem_", 4)) {
        beer_memory_handle(cmd, request, out, size);
    } else if (!strncmp(cmd, "obj_", 4)) {
        beer_objects_handle(cmd, request, out, size);
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

/* Games allocate and free heavily from many threads, and glibc keeps freed memory inside its per-thread arenas: RDR2
 * held ~400 MB of free arena memory after start-up.  Handing it back periodically costs ~60 ms of background work and
 * no frame time.  BEER_NO_MALLOC_TRIM=1 disables it; BEER_MALLOC_TRIM_S sets the interval (default 20 s). */
static void *trim_thread(void *arg)
{
    const unsigned interval = (unsigned)(uintptr_t)arg;
    for (;;) {
        sleep(interval);
        const uint64_t start = wall_ns();
        malloc_trim(0);
        const uint64_t took = wall_ns() - start;
        if (took > 2000000ull)                         /* it holds allocator locks: a slow trim stalls other threads */
            fprintf(stderr, "[TRIM] malloc_trim took %.1f ms\n", took / 1e6);
    }
    return NULL;
}

static void start_trim_thread(void)
{
    if (getenv("BEER_NO_MALLOC_TRIM")) return;
    const char *value = getenv("BEER_MALLOC_TRIM_S");
    int interval = value ? atoi(value) : 20;
    if (interval <= 0) return;
    pthread_t thread;
    if (pthread_create(&thread, NULL, trim_thread, (void *)(uintptr_t)interval) == 0) pthread_detach(thread);
}

void beer_control_start(const char *exe_path)
{
    if (g_listen_fd >= 0) return;
    snprintf(g_exe, sizeof(g_exe), "%s", exe_path ? exe_path : "");
    g_start_ns = wall_ns();
    start_trim_thread();
    {   /* low-memory guard: BEER_MEMGUARD=0 off, BEER_MEMGUARD_FREE_MB, BEER_MEMGUARD_MARGIN_MB, BEER_MEMGUARD_AUTO_RESUME=1 */
        const char *v;
        if ((v = getenv("BEER_MEMGUARD")) && *v) atomic_store(&settings[SET_MEMGUARD].value, atoi(v) != 0);
        if ((v = getenv("BEER_MEMGUARD_FREE_MB")) && *v && atoi(v) >= 0) atomic_store(&settings[SET_MG_FREE].value, atoi(v));
        if ((v = getenv("BEER_MEMGUARD_MARGIN_MB")) && *v && atoi(v) >= 0) atomic_store(&settings[SET_MG_MARGIN].value, atoi(v));
        if ((v = getenv("BEER_MEMGUARD_AUTO_RESUME")) && *v) atomic_store(&settings[SET_MG_AUTO].value, atoi(v) != 0);
        pthread_t thread;
        if (pthread_create(&thread, NULL, memguard_thread, NULL) == 0) pthread_detach(thread);
    }
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
    /* Starting DLSS overrides: BEER_DLSS_RATIO=<percent>, BEER_DLSS_RENDER=<W>x<H>, BEER_DLSS_PRESET=<letter|number>. */
    const char *dlss_ratio = getenv("BEER_DLSS_RATIO");
    if (dlss_ratio && *dlss_ratio) {
        char json[64];
        int32_t milli;
        snprintf(json, sizeof(json), "{\"v\":%s}", dlss_ratio);
        if (beer_json_milli(json, "v", &milli) && milli >= 10000 && milli <= 100000) atomic_store(&settings[3].value, milli);
        else fprintf(stderr, "[CONTROL] BEER_DLSS_RATIO=%s ignored (10..100 percent)\n", dlss_ratio);
    }
    unsigned render_w = 0, render_h = 0;
    const char *dlss_render = getenv("BEER_DLSS_RENDER");
    if (dlss_render && sscanf(dlss_render, "%ux%u", &render_w, &render_h) == 2 && render_w >= 64 && render_h >= 64 &&
        render_w <= 16384 && render_h <= 16384) {
        atomic_store(&settings[4].value, (int32_t)render_w);
        atomic_store(&settings[5].value, (int32_t)render_h);
    }
    const char *dlss_preset = getenv("BEER_DLSS_PRESET");
    if (dlss_preset && *dlss_preset) {
        const int c = (unsigned char)dlss_preset[0];
        const int value = (c >= 'a' && c <= 'z') ? c - 'a' + 1 : (c >= 'A' && c <= 'Z') ? c - 'A' + 1 : atoi(dlss_preset);
        if (!strcasecmp(dlss_preset, "none")) atomic_store(&settings[7].value, 1);
        else if (value >= 0 && value <= 26) atomic_store(&settings[6].value, value);
    }
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    char directory[256];
    if (runtime && *runtime) snprintf(directory, sizeof(directory), "%s/beer", runtime);
    else snprintf(directory, sizeof(directory), "/tmp/beer-%d", (int)getuid());
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
