#include "beer_post.h"
#include "beer_control.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static struct {
    pthread_mutex_t lock;
    BeerPostParams p;
    unsigned long shot_keysym;
    unsigned shot_mods;
    int shot_held;
    int shot_pending, shot_clean;
    char shot_path[512];
    char game[96];
    uint32_t frame;
} post = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .p = { .contrast = 1000, .gamma = 1000, .saturation = 1000, .cb_strength = 1000 },
    .shot_keysym = 0xffc9,   /* F12 */
};

static int neutral(const BeerPostParams *p)
{
    return p->brightness == 0 && p->contrast == 1000 && p->gamma == 1000 && p->saturation == 1000 && p->vibrance == 0 &&
           p->sharpen == 0 && p->vignette == 0 && p->grain == 0 && p->temperature == 0 && p->colorblind == 0 && p->letterbox == 0;
}

int beer_post_params(BeerPostParams *out)
{
    pthread_mutex_lock(&post.lock);
    *out = post.p;
    out->seed = (int32_t)(post.frame++ & 0xffff);
    const int active = !neutral(&post.p);
    pthread_mutex_unlock(&post.lock);
    return active;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void apply(const char *request)
{
    int32_t v;
    char text[48];
    if (beer_json_milli(request, "brightness", &v)) post.p.brightness = clampi(v, -500, 500);
    if (beer_json_milli(request, "contrast", &v)) post.p.contrast = clampi(v, 300, 3000);
    if (beer_json_milli(request, "gamma", &v)) post.p.gamma = clampi(v, 300, 3000);
    if (beer_json_milli(request, "saturation", &v)) post.p.saturation = clampi(v, 0, 3000);
    if (beer_json_milli(request, "vibrance", &v)) post.p.vibrance = clampi(v, -1000, 2000);
    if (beer_json_milli(request, "sharpen", &v)) post.p.sharpen = clampi(v, 0, 3000);
    if (beer_json_milli(request, "vignette", &v)) post.p.vignette = clampi(v, 0, 1000);
    if (beer_json_milli(request, "grain", &v)) post.p.grain = clampi(v, 0, 1000);
    if (beer_json_milli(request, "temperature", &v)) post.p.temperature = clampi(v, -1000, 1000);
    if (beer_json_milli(request, "colorblind", &v)) post.p.colorblind = clampi(v / 1000, 0, 6);
    if (beer_json_milli(request, "cb_strength", &v)) post.p.cb_strength = clampi(v, 0, 1000);
    if (beer_json_milli(request, "letterbox", &v)) post.p.letterbox = clampi(v, 0, 300);
    if (beer_json_milli(request, "shot_keysym", &v) && v > 0) post.shot_keysym = (unsigned long)(v / 1000);
    if (beer_json_milli(request, "shot_mods", &v)) post.shot_mods = (unsigned)(v / 1000) & (1u | 4u | 8u);
    (void)text;
}

static void state_json(char *out, size_t size)
{
    const BeerPostParams *p = &post.p;
    snprintf(out, size,
             "{\"ok\":true,\"brightness\":%d,\"contrast\":%d,\"gamma\":%d,\"saturation\":%d,\"vibrance\":%d,\"sharpen\":%d,\"vignette\":%d,"
             "\"grain\":%d,\"temperature\":%d,\"colorblind\":%d,\"cb_strength\":%d,\"letterbox\":%d,\"shot_keysym\":%lu,\"shot_mods\":%u}\n",
             p->brightness, p->contrast, p->gamma, p->saturation, p->vibrance, p->sharpen, p->vignette, p->grain, p->temperature,
             p->colorblind, p->cb_strength, p->letterbox, post.shot_keysym, post.shot_mods);
}

void beer_post_init(const char *exe_path)
{
    const char *base = exe_path ? strrchr(exe_path, '/') : NULL;
    snprintf(post.game, sizeof(post.game), "%s", base ? base + 1 : exe_path ? exe_path : "game");
    char *dot = strrchr(post.game, '.');
    if (dot) *dot = 0;
    const char *json = getenv("BEER_POST_JSON");
    if (json && *json) {
        pthread_mutex_lock(&post.lock);
        apply(json);
        pthread_mutex_unlock(&post.lock);
    }
}

static void request_screenshot(int clean)
{
    const char *home = getenv("HOME");
    char dir[400];
    snprintf(dir, sizeof(dir), "%s/Pictures/Beer", home && *home ? home : "/tmp");
    char pictures[400];
    snprintf(pictures, sizeof(pictures), "%s/Pictures", home && *home ? home : "/tmp");
    mkdir(pictures, 0755);
    mkdir(dir, 0755);
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    snprintf(post.shot_path, sizeof(post.shot_path), "%s/%s-%04d%02d%02d-%02d%02d%02d.png", dir, post.game,
             tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday, tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    post.shot_clean = clean;
    post.shot_pending = 1;
}

int beer_post_take_screenshot(char *path, size_t size, int *clean)
{
    pthread_mutex_lock(&post.lock);
    const int pending = post.shot_pending;
    if (pending) {
        snprintf(path, size, "%s", post.shot_path);
        *clean = post.shot_clean;
        post.shot_pending = 0;
    }
    pthread_mutex_unlock(&post.lock);
    return pending;
}

int beer_post_hotkey_event(unsigned long keysym, unsigned state, int down)
{
    if (keysym != post.shot_keysym || (state & (1u | 4u | 8u)) != post.shot_mods) return 0;
    pthread_mutex_lock(&post.lock);
    if (down && !post.shot_held) request_screenshot(0);
    post.shot_held = down;
    pthread_mutex_unlock(&post.lock);
    return 1;
}

void beer_post_handle(const char *cmd, const char *request, char *out, size_t size)
{
    pthread_mutex_lock(&post.lock);
    if (!strcmp(cmd, "post_get")) {
        state_json(out, size);
    } else if (!strcmp(cmd, "post_set")) {
        apply(request);
        state_json(out, size);
    } else if (!strcmp(cmd, "screenshot")) {
        int32_t clean = 0;
        beer_json_milli(request, "clean", &clean);
        request_screenshot(clean != 0);
        char escaped[600];
        size_t k = 0;
        for (const char *c = post.shot_path; *c && k + 2 < sizeof(escaped); ++c) { if (*c == '"' || *c == '\\') escaped[k++] = '\\'; escaped[k++] = *c; }
        escaped[k] = 0;
        snprintf(out, size, "{\"ok\":true,\"path\":\"%s\"}\n", escaped);
    } else {
        snprintf(out, size, "{\"ok\":false,\"error\":\"unknown post command\"}\n");
    }
    pthread_mutex_unlock(&post.lock);
}
