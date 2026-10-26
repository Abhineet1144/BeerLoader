#include "beer_overlay.h"
#include "beer_control.h"
#include "beer_memory.h"
#include "beer_gpu_stats.h"
#include "overlay_font.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { FIELD_FPS = 1, FIELD_FRAMETIME = 2, FIELD_CPU = 4, FIELD_RAM = 8, FIELD_THREADS = 16, FIELD_SPEED = 32, FIELD_UPTIME = 64,
       FIELD_GPU = 128, FIELD_VRAM = 256, FIELD_CLOCK = 512, FIELD_GRAPH = 1024 };
static const struct { const char *name; int bit; } field_names[] = {
    { "fps", FIELD_FPS }, { "frametime", FIELD_FRAMETIME }, { "cpu", FIELD_CPU }, { "ram", FIELD_RAM },
    { "threads", FIELD_THREADS }, { "speed", FIELD_SPEED }, { "uptime", FIELD_UPTIME },
    { "gpu", FIELD_GPU }, { "vram", FIELD_VRAM }, { "clock", FIELD_CLOCK }, { "graph", FIELD_GRAPH },
};
#define FIELD_COUNT (sizeof(field_names) / sizeof(field_names[0]))

typedef struct {
    int enabled;
    int x, y;            /* permille of the output size, top-left corner */
    int size;            /* 0 small, 1 medium, 2 large */
    uint8_t *pixels;
    size_t capacity;
    int w, h;
    uint64_t serial;
    uint64_t graph_stamp;
    char text[1536];     /* what the pixels currently show */
} Box;

#define MAX_PINS 16
typedef struct { int used; unsigned long long addr; char type[16]; char label[40]; } Pin;

static struct {
    pthread_mutex_t lock;
    int visible;
    unsigned long hotkey_keysym;
    unsigned hotkey_mods;
    int hotkey_held;
    uint32_t panel_color, text_color, accent_color;
    int opacity;         /* panel opacity, permille */
    int fields;
    Box box[2];          /* 0 = resources, 1 = pinned values */
    Pin pins[MAX_PINS];
    /* sampler */
    uint64_t sample_ns, sample_frames, sample_ticks;
    int fps_x10, cpu_x10;
    uint64_t rss_mb, threads;
    char resource_text[512];
    uint64_t pin_ns;
    char pin_text[1024];
} ov = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .hotkey_keysym = 0xffc7,   /* F10 */
    .panel_color = 0x12161c, .text_color = 0xe8e8e8, .accent_color = 0x8fe3a0, .opacity = 850,
    .fields = FIELD_FPS | FIELD_FRAMETIME | FIELD_CPU | FIELD_RAM | FIELD_THREADS,
    .box = { { .enabled = 1, .x = 10, .y = 10, .size = 1 }, { .enabled = 1, .x = 10, .y = 300, .size = 1 } },
};

/* Frame times (microseconds) of the most recent presents, for the graph. */
#define FRAME_RING 240
static _Atomic uint32_t frame_ring[FRAME_RING];
static _Atomic uint32_t frame_head;
static uint64_t last_frame_ns;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void beer_overlay_note_frame(void)
{
    const uint64_t now = now_ns();
    if (last_frame_ns) {
        const uint64_t dt = (now - last_frame_ns) / 1000u;
        const uint32_t head = atomic_fetch_add_explicit(&frame_head, 1, memory_order_relaxed);
        atomic_store_explicit(&frame_ring[head % FRAME_RING], dt > 4000000u ? 4000000u : (uint32_t)dt, memory_order_relaxed);
    }
    last_frame_ns = now;
}

/* ---- text rendering ---- */

static void put_pixel(uint8_t *px, uint32_t rgb, unsigned alpha)
{
    px[0] = (uint8_t)(rgb & 0xff); px[1] = (uint8_t)((rgb >> 8) & 0xff); px[2] = (uint8_t)((rgb >> 16) & 0xff); px[3] = (uint8_t)alpha;
}

static void blend_pixel(uint8_t *px, uint32_t rgb, unsigned alpha)
{
    const unsigned fg[3] = { rgb & 0xff, (rgb >> 8) & 0xff, (rgb >> 16) & 0xff };
    for (int c = 0; c < 3; ++c) px[c] = (uint8_t)((fg[c] * alpha + px[c] * (255 - alpha)) / 255);
    px[3] = (uint8_t)(alpha + px[3] * (255 - alpha) / 255);   /* text stays opaque even over a translucent panel */
}

static void draw_glyph(uint8_t *buffer, int stride_px, int x, int y, const OverlayFont *font, char ch, uint32_t color)
{
    if (ch < 32 || ch > 126) ch = '?';
    const uint8_t *glyph = font->glyphs + (size_t)(ch - 32) * font->cell_w * font->cell_h;
    for (int row = 0; row < font->cell_h; ++row)
        for (int col = 0; col < font->cell_w; ++col) {
            const unsigned a = glyph[row * font->cell_w + col];
            if (a) blend_pixel(buffer + ((size_t)(y + row) * stride_px + x + col) * 4, color, a);
        }
}

/* Renders `text` (lines separated by \n; a line starting with \x01 is the accent-coloured title). */
static int render_box(Box *box, const char *text, int graph, uint64_t graph_stamp)
{
    if (!strcmp(box->text, text) && box->pixels && (!graph || box->graph_stamp == graph_stamp)) return 0;
    const OverlayFont *font = &overlay_fonts[box->size < 0 ? 0 : box->size >= OVERLAY_FONT_COUNT ? OVERLAY_FONT_COUNT - 1 : box->size];
    int lines = 1, longest = 0, current = 0;
    for (const char *p = text; *p; ++p) {
        if (*p == '\n') { ++lines; current = 0; continue; }
        if (*p == '\x01') continue;
        if (++current > longest) longest = current;
    }
    const int pad = font->cell_h / 3 + 2;
    const int graph_h = graph ? font->cell_h * 3 : 0;
    int graph_w = longest * font->cell_w;
    if (graph && graph_w < font->cell_w * 28) graph_w = font->cell_w * 28;
    const int w = (graph ? graph_w : longest * font->cell_w) + pad * 2;
    const int h = lines * font->cell_h + pad * 2 + (graph ? graph_h + pad : 0);
    const size_t need = (size_t)w * h * 4;
    if (need > box->capacity) {
        uint8_t *grown = realloc(box->pixels, need);
        if (!grown) return 0;
        box->pixels = grown;
        box->capacity = need;
    }
    box->w = w; box->h = h;
    const unsigned panel_alpha = (unsigned)(ov.opacity * 255 / 1000);
    for (size_t i = 0; i < (size_t)w * h; ++i) put_pixel(box->pixels + i * 4, ov.panel_color, panel_alpha);
    /* thin accent border */
    for (int i = 0; i < w; ++i) { put_pixel(box->pixels + (size_t)i * 4, ov.accent_color, 255); put_pixel(box->pixels + ((size_t)(h - 1) * w + i) * 4, ov.accent_color, 255); }
    for (int i = 0; i < h; ++i) { put_pixel(box->pixels + (size_t)i * w * 4, ov.accent_color, 255); put_pixel(box->pixels + ((size_t)i * w + w - 1) * 4, ov.accent_color, 255); }
    int line = 0, col = 0;
    uint32_t color = ov.text_color;
    for (const char *p = text; *p; ++p) {
        if (*p == '\n') { ++line; col = 0; color = ov.text_color; continue; }
        if (*p == '\x01') { color = ov.accent_color; continue; }
        draw_glyph(box->pixels, w, pad + col * font->cell_w, pad + line * font->cell_h, font, *p, color);
        ++col;
    }
    if (graph) {
        /* Frame-time bars: newest on the right, green <= 16.7 ms, yellow <= 33.3 ms, red above; dim 60 fps guide line. */
        const int gx = pad, gy = pad + lines * font->cell_h + pad / 2;
        const unsigned head = atomic_load_explicit(&frame_head, memory_order_relaxed);
        const int bars = graph_w / 2 < FRAME_RING ? graph_w / 2 : FRAME_RING;
        uint32_t peak = 33333;
        for (int i = 0; i < bars && (unsigned)i < head; ++i) {
            const uint32_t t = atomic_load_explicit(&frame_ring[(head - 1 - (unsigned)i) % FRAME_RING], memory_order_relaxed);
            if (t > peak) peak = t;
        }
        if (peak > 100000) peak = 100000;
        const uint32_t darker = ((ov.panel_color & 0xff) * 6 / 10) | ((((ov.panel_color >> 8) & 0xff) * 6 / 10) << 8) | ((((ov.panel_color >> 16) & 0xff) * 6 / 10) << 16);
        for (int y = 0; y < graph_h; ++y)
            for (int x = 0; x < graph_w; ++x) put_pixel(box->pixels + ((size_t)(gy + y) * w + gx + x) * 4, darker, (unsigned)(ov.opacity * 255 / 1000));
        const int guide = graph_h - (int)((uint64_t)16667 * (uint64_t)graph_h / peak);
        if (guide >= 0 && guide < graph_h)
            for (int x = 0; x < graph_w; ++x) put_pixel(box->pixels + ((size_t)(gy + guide) * w + gx + x) * 4, 0x555b66, 255);
        for (int i = 0; i < bars && (unsigned)i < head; ++i) {
            const uint32_t t = atomic_load_explicit(&frame_ring[(head - 1 - (unsigned)i) % FRAME_RING], memory_order_relaxed);
            int bar_h = (int)((uint64_t)t * (uint64_t)graph_h / peak);
            if (bar_h < 1) bar_h = 1;
            if (bar_h > graph_h) bar_h = graph_h;
            const uint32_t color = t <= 16700 ? ov.accent_color : t <= 33400 ? 0xe6c34a : 0xe5534b;
            const int x0 = gx + graph_w - 2 * (i + 1);
            if (x0 < gx) break;
            for (int y = 0; y < bar_h; ++y)
                for (int dx = 0; dx < 2; ++dx) put_pixel(box->pixels + ((size_t)(gy + graph_h - 1 - y) * w + x0 + dx) * 4, color, 255);
        }
        box->graph_stamp = graph_stamp;
    }
    snprintf(box->text, sizeof(box->text), "%s", text);
    ++box->serial;
    const char *dump = getenv("BEER_OVERLAY_DUMP");   /* debug: writes <dir>/box<N>.ppm whenever a box is redrawn */
    if (dump) {
        char path[512];
        snprintf(path, sizeof(path), "%s/box%d.ppm", dump, box == &ov.box[0] ? 0 : 1);
        FILE *f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", w, h);
            for (size_t i = 0; i < (size_t)w * h; ++i) { const uint8_t *p4 = box->pixels + i * 4; const uint8_t rgb[3] = { p4[2], p4[1], p4[0] }; fwrite(rgb, 1, 3, f); }
            fclose(f);
        }
    }
    return 1;
}

/* ---- content ---- */

static void sample_resources(void)
{
    const uint64_t now = now_ns();
    if (ov.sample_ns && now - ov.sample_ns < 500000000ull && ov.resource_text[0]) return;
    uint64_t ticks, rss_kb, threads;
    beer_control_proc_stats(&ticks, &rss_kb, &threads);
    const uint64_t frames = beer_control_frames();
    if (ov.sample_ns) {
        const uint64_t dt = now - ov.sample_ns;
        ov.fps_x10 = (int)((frames - ov.sample_frames) * 10000000000ull / dt);
        ov.cpu_x10 = (int)((ticks - ov.sample_ticks) * 1000000000ull * 1000 / (uint64_t)sysconf(_SC_CLK_TCK) / dt);
    }
    ov.sample_ns = now; ov.sample_frames = frames; ov.sample_ticks = ticks;
    ov.rss_mb = rss_kb / 1024; ov.threads = threads;
    char *c = ov.resource_text, *end = c + sizeof(ov.resource_text);
    c += snprintf(c, (size_t)(end - c), "\x01" "Resources");
    if (ov.fields & FIELD_FPS) c += snprintf(c, (size_t)(end - c), "\nFPS        %d.%d", ov.fps_x10 / 10, ov.fps_x10 % 10);
    if ((ov.fields & FIELD_FRAMETIME) && ov.fps_x10 > 0) {
        const int ms_x10 = 100000 / ov.fps_x10;
        c += snprintf(c, (size_t)(end - c), "\nFrame time %d.%d ms", ms_x10 / 10, ms_x10 % 10);
    }
    if (ov.fields & FIELD_CPU) {
        /* Share of the whole machine (like a task manager), plus how many cores' worth of work that is. */
        const long logical = sysconf(_SC_NPROCESSORS_ONLN) > 0 ? sysconf(_SC_NPROCESSORS_ONLN) : 1;
        const int total_x10 = ov.cpu_x10 / (int)logical;
        c += snprintf(c, (size_t)(end - c), "\nCPU        %d.%d%% (%d.%d cores)", total_x10 / 10, total_x10 % 10, ov.cpu_x10 / 1000, (ov.cpu_x10 / 100) % 10);
    }
    if (ov.fields & FIELD_RAM) c += snprintf(c, (size_t)(end - c), "\nRAM        %llu MB", (unsigned long long)ov.rss_mb);
    if (ov.fields & FIELD_THREADS) c += snprintf(c, (size_t)(end - c), "\nThreads    %llu", (unsigned long long)ov.threads);
    if (ov.fields & FIELD_SPEED) {
        const int s = beer_control_paused() ? 0 : beer_control_time_scale_milli();
        c += snprintf(c, (size_t)(end - c), "\nSpeed      %d.%02dx%s", s / 1000, (s % 1000) / 10, beer_control_paused() ? " (paused)" : "");
    }
    if (ov.fields & (FIELD_GPU | FIELD_VRAM)) {
        int util = 0;
        uint64_t used = 0, total = 0, mine = 0;
        const int have = beer_gpu_stats_get(&util, &used, &total, &mine);
        if (ov.fields & FIELD_GPU) {
            if (have) c += snprintf(c, (size_t)(end - c), "\nGPU        %d%%", util);
            else c += snprintf(c, (size_t)(end - c), "\nGPU        n/a");
        }
        if (ov.fields & FIELD_VRAM) {
            if (have && total) {
                c += snprintf(c, (size_t)(end - c), "\nVRAM       %llu / %llu MB", (unsigned long long)used, (unsigned long long)total);
                if (mine) c += snprintf(c, (size_t)(end - c), "\n  this game %llu MB", (unsigned long long)mine);
            } else c += snprintf(c, (size_t)(end - c), "\nVRAM       n/a");
        }
    }
    if (ov.fields & FIELD_CLOCK) {
        time_t t = time(NULL);
        struct tm local;
        localtime_r(&t, &local);
        c += snprintf(c, (size_t)(end - c), "\nTime       %02d:%02d:%02d", local.tm_hour, local.tm_min, local.tm_sec);
    }
    if (ov.fields & FIELD_UPTIME) {
        const unsigned long long up = beer_control_uptime_s();
        c += snprintf(c, (size_t)(end - c), "\nUptime     %llu:%02llu", up / 60, up % 60);
    }
}

static void build_pins(void)
{
    const uint64_t now = now_ns();
    if (ov.pin_ns && now - ov.pin_ns < 150000000ull) return;
    ov.pin_ns = now;
    char *c = ov.pin_text, *end = c + sizeof(ov.pin_text);
    c += snprintf(c, (size_t)(end - c), "\x01" "Pinned memory");
    int any = 0;
    for (int i = 0; i < MAX_PINS && end - c > 80; ++i) {
        if (!ov.pins[i].used) continue;
        any = 1;
        char value[48];
        if (!beer_memory_peek(ov.pins[i].addr, ov.pins[i].type, value, sizeof(value))) snprintf(value, sizeof(value), "??");
        c += snprintf(c, (size_t)(end - c), "\n%s: %s", ov.pins[i].label, value);
    }
    if (!any) snprintf(c, (size_t)(end - c), "\nNothing pinned yet.\nPin values from the manager's\nMemory window.");
}

int beer_overlay_frame(int out_w, int out_h, BeerOverlayRect rects[2])
{
    pthread_mutex_lock(&ov.lock);
    int count = 0;
    if (ov.visible && out_w > 0 && out_h > 0) {
        sample_resources();
        build_pins();
        const char *texts[2] = { ov.resource_text, ov.pin_text };
        for (int i = 0; i < 2; ++i) {
            Box *box = &ov.box[i];
            if (!box->enabled) continue;
            render_box(box, texts[i], i == 0 && (ov.fields & FIELD_GRAPH), now_ns() / 100000000ull);
            if (!box->pixels) continue;
            int x = (int)((long long)out_w * box->x / 1000), y = (int)((long long)out_h * box->y / 1000);
            if (x + box->w > out_w) x = out_w - box->w;
            if (y + box->h > out_h) y = out_h - box->h;
            if (x < 0) x = 0;
            if (y < 0) y = 0;
            if (box->w > out_w || box->h > out_h) continue;
            rects[count++] = (BeerOverlayRect){ x, y, box->w, box->h, box->pixels, box->serial * 2 + (uint64_t)i };
        }
    }
    pthread_mutex_unlock(&ov.lock);
    return count;
}

int beer_overlay_hotkey_event(unsigned long keysym, unsigned state, int down)
{
    if (keysym != ov.hotkey_keysym) return 0;
    if ((state & (1u | 4u | 8u)) != ov.hotkey_mods) return 0;   /* Shift, Control, Alt */
    pthread_mutex_lock(&ov.lock);
    if (down && !ov.hotkey_held) ov.visible = !ov.visible;
    ov.hotkey_held = down;
    pthread_mutex_unlock(&ov.lock);
    return 1;
}

/* ---- configuration and commands ---- */

static void fields_from_string(const char *list)
{
    int mask = 0;
    char token[24];
    for (const char *p = list; *p;) {
        size_t n = strcspn(p, ", ");
        if (n && n < sizeof(token)) {
            memcpy(token, p, n);
            token[n] = 0;
            for (size_t i = 0; i < FIELD_COUNT; ++i)
                if (!strcmp(token, field_names[i].name)) mask |= field_names[i].bit;
        }
        p += n;
        while (*p == ',' || *p == ' ') ++p;
    }
    ov.fields = mask;
}

static void apply_config(const char *request)
{
    int32_t v;
    char text[160];
    if (beer_json_milli(request, "visible", &v)) ov.visible = v != 0;
    if (beer_json_milli(request, "hotkey_keysym", &v) && v > 0) ov.hotkey_keysym = (unsigned long)(v / 1000);
    if (beer_json_milli(request, "hotkey_mods", &v)) ov.hotkey_mods = (unsigned)(v / 1000) & (1u | 4u | 8u);
    if (beer_json_string(request, "panel_color", text, sizeof(text))) ov.panel_color = (uint32_t)strtoul(text, NULL, 16) & 0xffffff;
    if (beer_json_string(request, "text_color", text, sizeof(text))) ov.text_color = (uint32_t)strtoul(text, NULL, 16) & 0xffffff;
    if (beer_json_string(request, "accent_color", text, sizeof(text))) ov.accent_color = (uint32_t)strtoul(text, NULL, 16) & 0xffffff;
    if (beer_json_milli(request, "opacity", &v)) ov.opacity = v < 100 ? 100 : v > 1000 ? 1000 : v;
    if (beer_json_string(request, "fields", text, sizeof(text))) fields_from_string(text);
    static const char *const prefix[2] = { "res_", "pin_" };
    for (int i = 0; i < 2; ++i) {
        char key[24];
        snprintf(key, sizeof(key), "%senabled", prefix[i]);
        if (beer_json_milli(request, key, &v)) ov.box[i].enabled = v != 0;
        snprintf(key, sizeof(key), "%sx", prefix[i]);
        if (beer_json_milli(request, key, &v)) ov.box[i].x = v < 0 ? 0 : v > 1000 ? 1000 : v;   /* fraction -> permille is milli already */
        snprintf(key, sizeof(key), "%sy", prefix[i]);
        if (beer_json_milli(request, key, &v)) ov.box[i].y = v < 0 ? 0 : v > 1000 ? 1000 : v;
        snprintf(key, sizeof(key), "%ssize", prefix[i]);
        if (beer_json_milli(request, key, &v)) ov.box[i].size = (v / 1000) < 0 ? 0 : (v / 1000) > 2 ? 2 : v / 1000;
        ov.box[i].text[0] = 0;   /* force a re-render with the new look */
    }
}

static void config_json(char *out, size_t size)
{
    snprintf(out, size,
             "{\"ok\":true,\"visible\":%s,\"hotkey_keysym\":%lu,\"hotkey_mods\":%u,\"panel_color\":\"%06x\",\"text_color\":\"%06x\",\"accent_color\":\"%06x\",\"opacity\":%d.%03d,"
             "\"res_enabled\":%s,\"res_x\":%d.%03d,\"res_y\":%d.%03d,\"res_size\":%d,\"pin_enabled\":%s,\"pin_x\":%d.%03d,\"pin_y\":%d.%03d,\"pin_size\":%d}\n",
             ov.visible ? "true" : "false", ov.hotkey_keysym, ov.hotkey_mods, ov.panel_color, ov.text_color, ov.accent_color, ov.opacity / 1000, ov.opacity % 1000,
             ov.box[0].enabled ? "true" : "false", ov.box[0].x / 1000, ov.box[0].x % 1000, ov.box[0].y / 1000, ov.box[0].y % 1000, ov.box[0].size,
             ov.box[1].enabled ? "true" : "false", ov.box[1].x / 1000, ov.box[1].x % 1000, ov.box[1].y / 1000, ov.box[1].y % 1000, ov.box[1].size);
}

void beer_overlay_init(void)
{
    const char *json = getenv("BEER_OVERLAY_JSON");
    if (json && *json) {
        pthread_mutex_lock(&ov.lock);
        apply_config(json);
        pthread_mutex_unlock(&ov.lock);
    }
}

void beer_overlay_handle(const char *cmd, const char *request, char *out, size_t size)
{
    pthread_mutex_lock(&ov.lock);
    if (!strcmp(cmd, "overlay_get")) {
        config_json(out, size);
    } else if (!strcmp(cmd, "overlay_set")) {
        apply_config(request);
        config_json(out, size);
    } else if (!strcmp(cmd, "overlay_toggle")) {
        ov.visible = !ov.visible;
        config_json(out, size);
    } else if (!strcmp(cmd, "overlay_pin")) {
        char addr[40] = "", type[16] = "i32", label[48] = "";
        beer_json_string(request, "addr", addr, sizeof(addr));
        beer_json_string(request, "type", type, sizeof(type));
        beer_json_string(request, "label", label, sizeof(label));
        const unsigned long long a = strtoull(addr, NULL, 0);
        if (!a) { snprintf(out, size, "{\"ok\":false,\"error\":\"addr missing\"}\n"); goto done; }
        int slot = -1;
        for (int i = 0; i < MAX_PINS; ++i) {
            if (ov.pins[i].used && ov.pins[i].addr == a) { slot = i; break; }
            if (!ov.pins[i].used && slot < 0) slot = i;
        }
        if (slot < 0) { snprintf(out, size, "{\"ok\":false,\"error\":\"too many pinned values (max %d)\"}\n", MAX_PINS); goto done; }
        ov.pins[slot].used = 1;
        ov.pins[slot].addr = a;
        snprintf(ov.pins[slot].type, sizeof(ov.pins[slot].type), "%s", type);
        for (char *p = label; *p; ++p) if (*p == '\n' || *p == '\x01') *p = ' ';
        snprintf(ov.pins[slot].label, sizeof(ov.pins[slot].label), "%.30s", label[0] ? label : addr);
        ov.pin_ns = 0;
        snprintf(out, size, "{\"ok\":true}\n");
    } else if (!strcmp(cmd, "overlay_unpin")) {
        char addr[40] = "";
        beer_json_string(request, "addr", addr, sizeof(addr));
        const unsigned long long a = strtoull(addr, NULL, 0);
        for (int i = 0; i < MAX_PINS; ++i) if (ov.pins[i].used && ov.pins[i].addr == a) ov.pins[i].used = 0;
        ov.pin_ns = 0;
        snprintf(out, size, "{\"ok\":true}\n");
    } else if (!strcmp(cmd, "overlay_pins")) {
        char *c = out;
        c += snprintf(c, size, "{\"ok\":true,\"pins\":[");
        int first = 1;
        for (int i = 0; i < MAX_PINS; ++i) {
            if (!ov.pins[i].used) continue;
            c += snprintf(c, (size_t)(out + size - c), "%s{\"addr\":\"0x%llx\",\"type\":\"%s\",\"label\":\"%s\"}", first ? "" : ",", ov.pins[i].addr, ov.pins[i].type, ov.pins[i].label);
            first = 0;
        }
        snprintf(c, (size_t)(out + size - c), "]}\n");
    } else {
        snprintf(out, size, "{\"ok\":false,\"error\":\"unknown overlay command\"}\n");
    }
done:
    pthread_mutex_unlock(&ov.lock);
}
