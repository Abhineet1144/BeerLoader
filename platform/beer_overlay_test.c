/* Overlay layout: the pinned box only appears while something is pinned, and the row layout
 * draws each box as one line (with the frame-time graph beside the text). */
#include "beer_overlay.h"
#include "overlay_font.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- stubs for the loader services the overlay reads ---- */
uint64_t beer_control_frames(void) { static uint64_t frames; return frames += 30; }
uint64_t beer_control_uptime_s(void) { return 75; }
int beer_control_time_scale_milli(void) { return 1000; }
int beer_control_paused(void) { return 0; }
void beer_control_proc_stats(uint64_t *ticks, uint64_t *rss_kb, uint64_t *threads)
{
    *ticks = 100; *rss_kb = 2048 * 1024; *threads = 70;
}
int beer_gpu_stats_get(int *util, uint64_t *used, uint64_t *total, uint64_t *mine)
{
    *util = 35; *used = 4757; *total = 16376; *mine = 3579;
    return 1;
}
int beer_memory_peek(unsigned long long addr, const char *type, char *out, size_t size)
{
    (void)type;
    snprintf(out, size, "%llu", addr & 0xff);
    return 1;
}
char *beer_getenv_site(const char *name, char **value, int *state)
{
    (void)value; (void)state;
    return getenv(name);
}
int beer_read_config(const char *name, char *out, size_t size) { (void)name; (void)out; (void)size; return 0; }

/* Minimal flat-JSON readers: "key":"text" and "key":number|true|false. */
static const char *find_value(const char *json, const char *key)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *at = strstr(json, pattern);
    return at ? at + strlen(pattern) : NULL;
}
int beer_json_string(const char *json, const char *key, char *out, size_t size)
{
    const char *v = find_value(json, key);
    if (!v || *v != '"') return 0;
    size_t n = strcspn(v + 1, "\"");
    if (n >= size) n = size - 1;
    memcpy(out, v + 1, n);
    out[n] = 0;
    return 1;
}
int beer_json_milli(const char *json, const char *key, int32_t *out)
{
    const char *v = find_value(json, key);
    if (!v) return 0;
    if (!strncmp(v, "true", 4)) { *out = 1000; return 1; }
    if (!strncmp(v, "false", 5)) { *out = 0; return 1; }
    *out = (int32_t)(strtod(v, NULL) * 1000.0);
    return 1;
}

static int frame(BeerOverlayRect rects[2])
{
    return beer_overlay_frame(1920, 1080, rects);
}

static void set(const char *json)
{
    char out[1024];
    beer_overlay_handle("overlay_set", json, out, sizeof(out));
    assert(strstr(out, "\"ok\":true"));
}

int main(void)
{
    char out[1024];
    BeerOverlayRect rects[2];
    const OverlayFont *font = &overlay_fonts[1];
    const int pad = font->cell_h / 3 + 2;

    set("{\"visible\":true,\"layout\":\"panel\",\"fields\":\"fps,frametime,cpu,ram,gpu,vram,clock\","
        "\"res_enabled\":true,\"res_size\":1,\"pin_enabled\":true,\"pin_size\":1,\"pin_y\":0.5}");

    /* Nothing pinned: only the resource box. */
    assert(frame(rects) == 1);
    const int panel_h = rects[0].h;
    assert(panel_h > font->cell_h * 5);

    /* A pin brings the pinned box; removing it hides the box again. */
    beer_overlay_handle("overlay_pin", "{\"addr\":\"0x1234\",\"type\":\"i32\",\"label\":\"hp\"}", out, sizeof(out));
    assert(strstr(out, "\"ok\":true"));
    assert(frame(rects) == 2);
    assert(rects[1].h == font->cell_h * 2 + pad * 2);          /* title + one value */
    beer_overlay_handle("overlay_unpin", "{\"addr\":\"0x1234\"}", out, sizeof(out));
    assert(frame(rects) == 1);

    /* Row layout: one line per box. */
    set("{\"layout\":\"row\"}");
    beer_overlay_handle("overlay_get", "{}", out, sizeof(out));
    assert(strstr(out, "\"layout\":\"row\""));
    assert(frame(rects) == 1);
    assert(rects[0].h == font->cell_h + pad * 2);
    assert(rects[0].w > rects[0].h * 10);
    const int row_w = rects[0].w;

    /* The graph sits beside the text: same height, wider box. */
    set("{\"fields\":\"fps,frametime,cpu,ram,gpu,vram,clock,graph\"}");
    beer_overlay_note_frame();
    beer_overlay_note_frame();
    assert(frame(rects) == 1);
    assert(rects[0].h == font->cell_h + pad * 2 && rects[0].w > row_w);

    /* Pins in the row layout are one line too. */
    beer_overlay_handle("overlay_pin", "{\"addr\":\"0x1234\",\"type\":\"i32\",\"label\":\"hp\"}", out, sizeof(out));
    beer_overlay_handle("overlay_pin", "{\"addr\":\"0x5678\",\"type\":\"i32\",\"label\":\"ammo\"}", out, sizeof(out));
    assert(frame(rects) == 2);
    assert(rects[1].h == font->cell_h + pad * 2);

    /* Back to panels. */
    set("{\"layout\":\"panel\"}");
    assert(frame(rects) == 2);
    assert(rects[0].h > font->cell_h * 5 && rects[1].h == font->cell_h * 3 + pad * 2);

    puts("overlay tests passed");
    return 0;
}
