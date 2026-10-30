#define _GNU_SOURCE
#include "win32_gdi.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "overlay_font.h"
#include "win32_resource.h"
#include "xwayland_backend.h"

/* ---------------------------------------------------------------- objects */

enum { OBJ_NONE, OBJ_BITMAP, OBJ_FONT, OBJ_BRUSH, OBJ_PEN };

typedef struct {
    int w, h, bpp, stride, topdown, is_dib;
    uint8_t *bits;
} Surface;

typedef struct {
    int type, stock;
    Surface surface;             /* OBJ_BITMAP */
    uint8_t logfont[92];         /* OBJ_FONT (LOGFONTW) */
    uint32_t color;              /* OBJ_BRUSH / OBJ_PEN, COLORREF */
    int style, width;
} Obj;

#define OBJ_MAX 8192
#define OBJ_BASE 0xD1000000ULL
#define DC_MAX 256
#define DC_BASE 0xDC100000ULL

typedef struct {
    int used, is_window;
    uint64_t bitmap, font, brush, pen;
    uint32_t text_color, bk_color, text_align;
    int bk_mode;
} Dc;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static Obj *g_objects[OBJ_MAX];
static Dc g_dcs[DC_MAX];
static Dc g_window_dc;
static Surface g_window;
static int g_window_dirty;
static int g_initialised;
static uint64_t g_stock[20];
static uint64_t g_default_bitmap;

#define LOCK() pthread_mutex_lock(&g_lock)
#define UNLOCK() pthread_mutex_unlock(&g_lock)

static Obj *obj_get(uint64_t handle)
{
    if (handle < OBJ_BASE + 1 || handle >= OBJ_BASE + OBJ_MAX + 1) return NULL;
    return g_objects[handle - OBJ_BASE - 1];
}

static uint64_t obj_new(int type)
{
    for (int i = 0; i < OBJ_MAX; ++i)
        if (!g_objects[i]) {
            g_objects[i] = calloc(1, sizeof(Obj));
            if (!g_objects[i]) return 0;
            g_objects[i]->type = type;
            return OBJ_BASE + 1 + (uint64_t)i;
        }
    return 0;
}

static void obj_free(uint64_t handle)
{
    Obj *o = obj_get(handle);
    if (!o) return;
    if (o->type == OBJ_BITMAP) free(o->surface.bits);
    free(o);
    g_objects[handle - OBJ_BASE - 1] = NULL;
}

static uint64_t make_brush(uint32_t color, int style)
{
    uint64_t h = obj_new(OBJ_BRUSH);
    Obj *o = obj_get(h);
    if (!o) return 0;
    o->color = color;
    o->style = style;
    return h;
}

static uint64_t make_pen(uint32_t color, int style, int width)
{
    uint64_t h = obj_new(OBJ_PEN);
    Obj *o = obj_get(h);
    if (!o) return 0;
    o->color = color;
    o->style = style;
    o->width = width;
    return h;
}

static void fill_logfont(uint8_t *lf, int height, int width, int weight, int italic, int underline,
                         int strike, int charset, const uint16_t *face)
{
    memset(lf, 0, 92);
    memcpy(lf + 0, &height, 4);
    memcpy(lf + 4, &width, 4);
    memcpy(lf + 16, &weight, 4);
    lf[20] = (uint8_t)italic; lf[21] = (uint8_t)underline; lf[22] = (uint8_t)strike; lf[23] = (uint8_t)charset;
    for (int i = 0; face && i < 31 && face[i]; ++i) memcpy(lf + 28 + i * 2, &face[i], 2);
}

static uint64_t make_font(int height, int width, int weight, int italic, int underline, int strike,
                          int charset, const uint16_t *face)
{
    uint64_t h = obj_new(OBJ_FONT);
    Obj *o = obj_get(h);
    if (!o) return 0;
    fill_logfont(o->logfont, height, width, weight, italic, underline, strike, charset, face);
    return h;
}

static int surface_alloc(Surface *s, int w, int h, int bpp, int topdown)
{
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return 0;
    s->w = w; s->h = h; s->bpp = bpp; s->topdown = topdown;
    s->stride = ((w * bpp + 31) / 32) * 4;
    s->bits = calloc(1, (size_t)s->stride * (size_t)h);
    return s->bits != NULL;
}

static void gdi_init(void)
{
    if (g_initialised) return;
    g_initialised = 1;
    static const uint32_t brush_colors[] = { 0xFFFFFF, 0xC0C0C0, 0x808080, 0x404040, 0x000000 };
    for (int i = 0; i < 5; ++i) g_stock[i] = make_brush(brush_colors[i], 0);
    g_stock[5] = make_brush(0, 1);
    g_stock[6] = make_pen(0xFFFFFF, 0, 1);
    g_stock[7] = make_pen(0x000000, 0, 1);
    g_stock[8] = make_pen(0, 5, 0);
    static const uint16_t fixed[] = { 'C', 'o', 'u', 'r', 'i', 'e', 'r', 0 };
    static const uint16_t system[] = { 'S', 'y', 's', 't', 'e', 'm', 0 };
    static const uint16_t gui[] = { 'S', 'e', 'g', 'o', 'e', ' ', 'U', 'I', 0 };
    g_stock[10] = make_font(-12, 0, 400, 0, 0, 0, 0, fixed);
    g_stock[11] = make_font(-12, 0, 400, 0, 0, 0, 0, fixed);
    g_stock[12] = make_font(-13, 0, 400, 0, 0, 0, 0, gui);
    g_stock[13] = make_font(-13, 0, 700, 0, 0, 0, 0, system);
    g_stock[14] = make_font(-13, 0, 400, 0, 0, 0, 0, system);
    g_stock[16] = make_font(-12, 0, 400, 0, 0, 0, 0, fixed);
    g_stock[17] = make_font(-12, 0, 400, 0, 0, 0, 0, gui);
    g_stock[18] = make_brush(0xFFFFFF, 0);
    g_stock[19] = make_pen(0, 0, 1);
    for (int i = 0; i < 20; ++i) {
        Obj *o = obj_get(g_stock[i]);
        if (o) o->stock = 1;
    }
    g_default_bitmap = obj_new(OBJ_BITMAP);
    Obj *d = obj_get(g_default_bitmap);
    if (d) { surface_alloc(&d->surface, 1, 1, 32, 1); d->stock = 1; }
    g_window_dc.is_window = 1;
    g_window_dc.used = 1;
    g_window_dc.bitmap = g_default_bitmap;
    g_window_dc.font = g_stock[17];
    g_window_dc.brush = g_stock[0];
    g_window_dc.pen = g_stock[7];
    g_window_dc.bk_color = 0xFFFFFF;
    g_window_dc.bk_mode = 2;
}

static Dc *dc_get(uint64_t handle)
{
    gdi_init();
    if (handle == 0 || handle == BEER_GDI_WINDOW_DC) return &g_window_dc;
    if (handle >= DC_BASE && handle < DC_BASE + DC_MAX && g_dcs[handle - DC_BASE].used)
        return &g_dcs[handle - DC_BASE];
    return NULL;
}

/* -------------------------------------------------------------- surfaces */

static void window_surface_ensure(void)
{
    XwaylandWindowState state;
    int w = 1280, h = 720;
    if (xwayland_window_get_state(&state) && state.client_width > 0 && state.client_height > 0) {
        w = state.client_width;
        h = state.client_height;
    }
    if (g_window.bits && g_window.w == w && g_window.h == h) return;
    free(g_window.bits);
    memset(&g_window, 0, sizeof(g_window));
    surface_alloc(&g_window, w, h, 32, 1);
}

static Surface *dc_surface(Dc *dc)
{
    if (!dc) return NULL;
    if (dc->is_window) {
        window_surface_ensure();
        return g_window.bits ? &g_window : NULL;
    }
    Obj *bmp = obj_get(dc->bitmap);
    if (!bmp || bmp->type != OBJ_BITMAP) bmp = obj_get(g_default_bitmap);
    return bmp && bmp->surface.bits ? &bmp->surface : NULL;
}

static uint32_t s_get(const Surface *s, int x, int y)
{
    int row = s->topdown ? y : s->h - 1 - y;
    const uint8_t *p = s->bits + (size_t)row * s->stride + (size_t)x * (s->bpp / 8);
    if (s->bpp == 32) return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | 0xFF000000u;
}

static void s_put(Surface *s, int x, int y, uint32_t px)
{
    int row = s->topdown ? y : s->h - 1 - y;
    uint8_t *p = s->bits + (size_t)row * s->stride + (size_t)x * (s->bpp / 8);
    p[0] = (uint8_t)px; p[1] = (uint8_t)(px >> 8); p[2] = (uint8_t)(px >> 16);
    if (s->bpp == 32) p[3] = (uint8_t)(px >> 24);
}

static uint32_t from_colorref(uint32_t c)
{
    return ((c >> 16) & 0xff) | (c & 0xff00) | ((c & 0xff) << 16) | 0xFF000000u;
}

static uint32_t to_colorref(uint32_t px)
{
    return ((px >> 16) & 0xff) | (px & 0xff00) | ((px & 0xff) << 16);
}

static void mark_dirty(Dc *dc, const Surface *s)
{
    if (dc && dc->is_window && s == &g_window) g_window_dirty = 1;
}

static void fill_rect_px(Surface *s, int x0, int y0, int x1, int y1, uint32_t px)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) s_put(s, x, y, px);
}

/* ------------------------------------------------------------------- DIBs */

typedef struct {
    uint32_t size;
    int32_t width, height;
    uint16_t planes, bit_count;
    uint32_t compression, size_image;
    int32_t x_ppm, y_ppm;
    uint32_t clr_used, clr_important;
} Bih;

/* Decodes a packed DIB (header + palette + pixels) into a 32bpp top-down surface. */
static int decode_dib(const uint8_t *info, const uint8_t *bits_override, Surface *out)
{
    Bih h;
    memcpy(&h, info, sizeof(h));
    if (h.size < 40 || h.planes != 1 || h.width <= 0 || h.height == 0) return 0;
    if (h.compression != 0 && h.compression != 3) return 0;
    int w = h.width, ht = h.height < 0 ? -h.height : h.height;
    int topdown = h.height < 0;
    const uint8_t *palette = info + h.size;
    uint32_t colors = h.bit_count <= 8 ? (h.clr_used ? h.clr_used : 1u << h.bit_count) : 0;
    size_t header_extra = (h.compression == 3 && h.size == 40) ? 12 : 0;
    const uint8_t *bits = bits_override ? bits_override : info + h.size + header_extra + (size_t)colors * 4;
    if (h.bit_count == 16 && h.compression == 0) { /* 5-5-5 */ }
    if (!surface_alloc(out, w, ht, 32, 1)) return 0;
    size_t stride = (((size_t)w * h.bit_count + 31) / 32) * 4;
    for (int y = 0; y < ht; ++y) {
        const uint8_t *row = bits + stride * (size_t)(topdown ? y : ht - 1 - y);
        for (int x = 0; x < w; ++x) {
            uint32_t px = 0xFF000000u;
            switch (h.bit_count) {
            case 32: px = (uint32_t)row[x*4] | (uint32_t)row[x*4+1] << 8 | (uint32_t)row[x*4+2] << 16 |
                          (uint32_t)row[x*4+3] << 24; break;
            case 24: px |= (uint32_t)row[x*3] | (uint32_t)row[x*3+1] << 8 | (uint32_t)row[x*3+2] << 16; break;
            case 16: {
                uint32_t v = row[x*2] | row[x*2+1] << 8;
                uint32_t r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
                px |= (b << 3 | b >> 2) | (g << 3 | g >> 2) << 8 | (r << 3 | r >> 2) << 16; break;
            }
            case 8: { uint32_t i = row[x]; if (i < colors) px |= (uint32_t)palette[i*4] | (uint32_t)palette[i*4+1] << 8 | (uint32_t)palette[i*4+2] << 16; break; }
            case 4: { uint32_t i = (row[x/2] >> ((x & 1) ? 0 : 4)) & 15; if (i < colors) px |= (uint32_t)palette[i*4] | (uint32_t)palette[i*4+1] << 8 | (uint32_t)palette[i*4+2] << 16; break; }
            case 1: { uint32_t i = (row[x/8] >> (7 - (x & 7))) & 1; if (i < colors) px |= (uint32_t)palette[i*4] | (uint32_t)palette[i*4+1] << 8 | (uint32_t)palette[i*4+2] << 16; break; }
            default: free(out->bits); out->bits = NULL; return 0;
            }
            s_put(out, x, y, px);
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ blits */

static uint32_t apply_rop(uint32_t rop, uint32_t s, uint32_t d, uint32_t p)
{
    switch (rop) {
    case 0x00CC0020: return s;
    case 0x00EE0086: return s | d;
    case 0x008800C6: return s & d;
    case 0x00660046: return s ^ d;
    case 0x00440328: return s & ~d;
    case 0x00330008: return ~s;
    case 0x001100A6: return ~(s | d);
    case 0x00BB0226: return ~s | d;
    case 0x00C000CA: return s & p;
    case 0x00F00021: return p;
    case 0x00FB0A09: return p | ~s | d;
    case 0x005A0049: return p ^ d;
    case 0x00550009: return ~d;
    case 0x00000042: return 0;
    case 0x00FF0062: return 0xFFFFFFFFu;
    default: return s;
    }
}

static uint32_t brush_pixel(uint64_t hbrush)
{
    Obj *b = obj_get(hbrush);
    if (b && b->type == OBJ_BRUSH) return from_colorref(b->color);
    if (hbrush >= 1 && hbrush <= 31) {          /* COLOR_xxx + 1 */
        switch (hbrush - 1) {
        case 5: return 0xFFFFFFFFu;             /* COLOR_WINDOW */
        case 8: return 0xFF000000u;             /* COLOR_WINDOWTEXT */
        case 1: return from_colorref(0x6e3d1d); /* COLOR_BACKGROUND stays dark */
        default: return from_colorref(0xF0F0F0);
        }
    }
    return 0xFFFFFFFFu;
}

static void blit_generic(Dc *ddc, Surface *d, int dx, int dy, int dw, int dh, const Surface *s, int sx,
                         int sy, int sw, int sh, uint32_t rop)
{
    if (!dw || !dh || (s && (!sw || !sh))) return;
    int adw = dw < 0 ? -dw : dw, adh = dh < 0 ? -dh : dh;
    uint32_t pattern = brush_pixel(ddc ? ddc->brush : 0);
    const int need_src = s != NULL;
    /* Fast path: unscaled 32bpp copy. */
    if (need_src && rop == 0x00CC0020 && adw == sw && adh == sh && dw > 0 && dh > 0 && sw > 0 && sh > 0 &&
        d->bpp == 32 && s->bpp == 32 && d->topdown == s->topdown && sx >= 0 && sy >= 0 &&
        sx + sw <= s->w && sy + sh <= s->h && dx >= 0 && dy >= 0 && dx + dw <= d->w && dy + dh <= d->h) {
        for (int y = 0; y < dh; ++y) {
            int sr = s->topdown ? sy + y : s->h - 1 - (sy + y);
            int dr = d->topdown ? dy + y : d->h - 1 - (dy + y);
            memcpy(d->bits + (size_t)dr * d->stride + (size_t)dx * 4,
                   s->bits + (size_t)sr * s->stride + (size_t)sx * 4, (size_t)dw * 4);
        }
        return;
    }
    for (int j = 0; j < adh; ++j) {
        int y = dy + (dh < 0 ? adh - 1 - j : j);
        if (y < 0 || y >= d->h) continue;
        for (int i = 0; i < adw; ++i) {
            int x = dx + (dw < 0 ? adw - 1 - i : i);
            if (x < 0 || x >= d->w) continue;
            uint32_t sp = 0;
            if (need_src) {
                int fx = (int)(((int64_t)i * (sw < 0 ? -sw : sw)) / adw);
                int fy = (int)(((int64_t)j * (sh < 0 ? -sh : sh)) / adh);
                int px = sx + (sw < 0 ? -fx : fx), py = sy + (sh < 0 ? -fy : fy);
                if (px < 0 || py < 0 || px >= s->w || py >= s->h) continue;
                sp = s_get(s, px, py);
            }
            uint32_t out = apply_rop(rop, sp, s_get(d, x, y), pattern);
            s_put(d, x, y, out | 0xFF000000u);
        }
    }
}

static int do_stretch(uint64_t hdst, int x, int y, int w, int h, uint64_t hsrc, int sx, int sy, int sw, int sh,
                      uint32_t rop)
{
    Dc *dd = dc_get(hdst), *sd = dc_get(hsrc);
    Surface *d = dc_surface(dd);
    if (!d) return 0;
    Surface *s = NULL;
    /* ROPs that do not use a source (PATCOPY, DSTINVERT, BLACKNESS, ...) ignore hsrc. */
    int uses_src = !(rop == 0x00F00021 || rop == 0x005A0049 || rop == 0x00550009 || rop == 0x00000042 ||
                     rop == 0x00FF0062);
    if (uses_src) {
        s = dc_surface(sd);
        if (!s) return 0;
    }
    blit_generic(dd, d, x, y, w, h, s, sx, sy, sw, sh, rop);
    mark_dirty(dd, d);
    return 1;
}

/* ------------------------------------------------------------------- text */

static const OverlayFont *font_for(uint64_t hfont, int *height_out)
{
    Obj *f = obj_get(hfont);
    int height = 16;
    if (f && f->type == OBJ_FONT) memcpy(&height, f->logfont, 4);
    int h = height < 0 ? -height : height;
    if (!h) h = 16;
    int idx = h <= 15 ? 0 : h <= 22 ? 1 : 2;
    if (height_out) *height_out = overlay_fonts[idx].cell_h;
    return &overlay_fonts[idx];
}

static void draw_glyph(Surface *s, int x, int y, const OverlayFont *font, unsigned ch, uint32_t color_px)
{
    if (ch < 32 || ch > 126) ch = '?';
    const uint8_t *glyph = font->glyphs + (size_t)(ch - 32) * font->cell_w * font->cell_h;
    for (int row = 0; row < font->cell_h; ++row) {
        int py = y + row;
        if (py < 0 || py >= s->h) continue;
        for (int col = 0; col < font->cell_w; ++col) {
            int px = x + col;
            unsigned a = glyph[row * font->cell_w + col];
            if (!a || px < 0 || px >= s->w) continue;
            uint32_t d = s_get(s, px, py);
            uint32_t out = 0xFF000000u;
            for (int sh = 0; sh <= 16; sh += 8) {
                unsigned dc = (d >> sh) & 0xff, sc = (color_px >> sh) & 0xff;
                out |= (uint32_t)((dc * (255 - a) + sc * a) / 255) << sh;
            }
            s_put(s, px, py, out);
        }
    }
}

static int text_width(const OverlayFont *font, int count) { return count * font->cell_w; }

static void draw_text(Dc *dc, Surface *s, int x, int y, const uint16_t *text, int count, uint64_t hfont)
{
    int cell_h;
    const OverlayFont *font = font_for(hfont, &cell_h);
    int width = text_width(font, count);
    uint32_t align = dc->text_align;
    if ((align & 6) == 6) x -= width / 2;
    else if (align & 2) x -= width;
    if ((align & 24) == 24) y -= cell_h - cell_h / 5;
    else if (align & 8) y -= cell_h;
    if (dc->bk_mode == 2) fill_rect_px(s, x, y, x + width, y + cell_h, from_colorref(dc->bk_color));
    uint32_t color = from_colorref(dc->text_color);
    for (int i = 0; i < count; ++i) draw_glyph(s, x + i * font->cell_w, y, font, text[i], color);
}

static int wlen(const uint16_t *t, int count)
{
    if (count >= 0) return count;
    int n = 0;
    while (t[n]) ++n;
    return n;
}

/* ----------------------------------------------------------- present path */

void beer_gdi_flush_window(void)
{
    LOCK();
    if (g_window_dirty && g_window.bits) {
        static uint8_t *rgba;
        static size_t capacity;
        size_t need = (size_t)g_window.w * g_window.h * 4;
        if (capacity < need) { free(rgba); rgba = malloc(need); capacity = rgba ? need : 0; }
        if (rgba) {
            for (int y = 0; y < g_window.h; ++y)
                for (int x = 0; x < g_window.w; ++x) {
                    uint32_t px = s_get(&g_window, x, y);
                    uint8_t *o = rgba + ((size_t)y * g_window.w + x) * 4;
                    o[0] = (uint8_t)(px >> 16); o[1] = (uint8_t)(px >> 8); o[2] = (uint8_t)px; o[3] = 0xFF;
                }
            xwayland_window_present_rgba8(rgba, g_window.w, g_window.h, g_window.w * 4);
        }
        g_window_dirty = 0;
    }
    UNLOCK();
}

int beer_gdi_window_dirty(void) { return g_window_dirty; }

/* ------------------------------------------------------------- USER32 API */

uint64_t GDI_MS beer_GetWindowDC(uint64_t window) { (void)window; return BEER_GDI_WINDOW_DC; }

static int class_is(const char *class_name, const char *expected)
{
    return class_name && !strcasecmp(class_name, expected);
}

int beer_gdi_paint_standard_control(const char *class_name, const uint16_t *text,
                                    int x, int y, int width, int height)
{
    const int is_static = class_is(class_name, "Static");
    const int is_button = class_is(class_name, "Button");
    if (!is_static && !is_button) return 0;
    if (width <= 0 || height <= 0) return 1;

    static const uint16_t empty[] = {0};
    if (!text || (uintptr_t)text <= 0xffffu) text = empty;

    LOCK();
    Dc *dc = dc_get(BEER_GDI_WINDOW_DC);
    Surface *surface = dc_surface(dc);
    if (dc && surface) {
        const uint32_t face = from_colorref(0xF0F0F0u); /* COLOR_BTNFACE */
        fill_rect_px(surface, x, y, x + width, y + height, face);
        if (is_button) {
            /* A simple native-looking push-button frame. This is deliberately
             * generic USER32 behavior, not dialog- or title-specific drawing. */
            fill_rect_px(surface, x, y, x + width, y + 1, 0xFFFFFFFFu);
            fill_rect_px(surface, x, y, x + 1, y + height, 0xFFFFFFFFu);
            fill_rect_px(surface, x, y + height - 1, x + width, y + height,
                         0xFF606060u);
            fill_rect_px(surface, x + width - 1, y, x + width, y + height,
                         0xFF606060u);
        }

        int count = 0;
        while (text[count] && count < 1023) ++count;
        int cell_h = 0;
        const OverlayFont *font = font_for(dc->font, &cell_h);
        int text_x = x + (is_button ? (width - text_width(font, count)) / 2 : 0);
        int text_y = y + (height - cell_h) / 2;
        uint32_t saved_background = dc->bk_color;
        int saved_mode = dc->bk_mode;
        dc->bk_color = 0xF0F0F0u;
        dc->bk_mode = 1; /* TRANSPARENT: the control background is already filled. */
        draw_text(dc, surface, text_x, text_y, text, count, dc->font);
        dc->bk_color = saved_background;
        dc->bk_mode = saved_mode;
        mark_dirty(dc, surface);
    }
    UNLOCK();
    if (getenv("BEER_GDI_DIAGNOSTICS"))
        fprintf(stderr, "[GDI] painted %s child control at %d,%d %dx%d\n",
                class_name, x, y, width, height);
    beer_gdi_flush_window();
    return 1;
}

static uint64_t bitmap_from_surface(Surface *s)
{
    uint64_t h = obj_new(OBJ_BITMAP);
    Obj *o = obj_get(h);
    if (!o) { free(s->bits); return 0; }
    o->surface = *s;
    return h;
}

uint64_t GDI_MS beer_LoadBitmapW(uint64_t instance, uint64_t name)
{
    uint64_t res = beer_resource_find(instance, name, 2 /* RT_BITMAP */, 0);
    uint32_t size = 0;
    const uint8_t *data = res ? beer_resource_data(instance, res, &size) : NULL;
    if (!data || size < 40) return 0;
    Surface s = {0};
    LOCK();
    gdi_init();
    uint64_t h = decode_dib(data, NULL, &s) ? bitmap_from_surface(&s) : 0;
    UNLOCK();
    return h;
}

uint64_t GDI_MS beer_LoadBitmapA(uint64_t instance, uint64_t name)
{
    uint16_t wide[256];
    if (name > 0xffff) {
        const char *a = (const char *)(uintptr_t)name;
        size_t i = 0;
        for (; i < 255 && a[i]; ++i) wide[i] = (uint8_t)a[i];
        wide[i] = 0;
        name = (uint64_t)(uintptr_t)wide;
    }
    return beer_LoadBitmapW(instance, name);
}

uint64_t GDI_MS beer_LoadImageW(uint64_t instance, uint64_t name, uint32_t type, int cx, int cy, uint32_t flags)
{
    (void)cx; (void)cy;
    if (type != 0 /* IMAGE_BITMAP */ || (flags & 0x10 /* LR_LOADFROMFILE */)) return 0;
    return beer_LoadBitmapW(instance, name);
}

uint64_t GDI_MS beer_LoadImageA(uint64_t instance, uint64_t name, uint32_t type, int cx, int cy, uint32_t flags)
{
    (void)cx; (void)cy;
    if (type != 0 || (flags & 0x10)) return 0;
    return beer_LoadBitmapA(instance, name);
}

int GDI_MS beer_FillRect(uint64_t dc, const int32_t *rect, uint64_t brush)
{
    if (!rect) return 0;
    LOCK();
    Dc *d = dc_get(dc);
    Surface *s = dc_surface(d);
    if (s) { fill_rect_px(s, rect[0], rect[1], rect[2], rect[3], brush_pixel(brush)); mark_dirty(d, s); }
    UNLOCK();
    return s != NULL;
}

/* One line of DrawText: returns the pixel width; draws only when `draw`. */
static int draw_text_block(Dc *d, Surface *s, int32_t *rect, const uint16_t *text, int n, uint32_t format,
                           int draw, int *out_width)
{
    int cell_h;
    uint64_t hfont = d->font;
    const OverlayFont *font = font_for(hfont, &cell_h);
    int box_w = rect[2] - rect[0];
    uint16_t clean[1024];
    int m = 0;
    for (int i = 0; i < n && m < 1023; ++i) {
        if (!(format & 0x800) && text[i] == '&' && i + 1 < n) { ++i; }   /* DT_NOPREFIX off: drop '&' */
        clean[m++] = text[i];
    }
    /* Split into lines. */
    int line_start[256], line_len[256], lines = 0;
    int start = 0;
    for (int i = 0; i <= m && lines < 255; ++i) {
        int is_break = i == m || (!(format & 0x20) && clean[i] == '\n');
        int too_wide = (format & 0x10) && !(format & 0x20) && box_w > 0 &&
                       (i - start + 1) * font->cell_w > box_w && i < m && clean[i] == ' ';
        if (is_break || too_wide) {
            int len = i - start;
            if (len > 0 && clean[start + len - 1] == '\r') --len;
            line_start[lines] = start; line_len[lines] = len; ++lines;
            start = i + 1;
        }
    }
    if (!lines) { line_start[0] = 0; line_len[0] = 0; lines = 1; }
    int total_h = lines * cell_h, max_w = 0;
    for (int l = 0; l < lines; ++l)
        if (text_width(font, line_len[l]) > max_w) max_w = text_width(font, line_len[l]);
    if (out_width) *out_width = max_w;
    if (!draw) return total_h;
    int y = rect[1];
    if (format & 8) y = rect[3] - total_h;
    else if (format & 4) y = rect[1] + (rect[3] - rect[1] - total_h) / 2;
    uint32_t saved_align = d->text_align;
    d->text_align = 0;
    for (int l = 0; l < lines; ++l, y += cell_h) {
        int w = text_width(font, line_len[l]);
        int x = rect[0];
        if (format & 1) x = rect[0] + (box_w - w) / 2;
        else if (format & 2) x = rect[2] - w;
        draw_text(d, s, x, y, clean + line_start[l], line_len[l], hfont);
    }
    d->text_align = saved_align;
    return total_h;
}

int GDI_MS beer_DrawTextW(uint64_t dc, const uint16_t *text, int count, int32_t *rect, uint32_t format)
{
    if (!text || !rect) return 0;
    LOCK();
    Dc *d = dc_get(dc);
    Surface *s = dc_surface(d);
    int height = 0;
    if (d && s) {
        int n = wlen(text, count), width = 0;
        if (format & 0x400) { /* DT_CALCRECT */
            height = draw_text_block(d, s, rect, text, n, format, 0, &width);
            rect[3] = rect[1] + height;
            if (!((format & 0x10) && rect[2] - rect[0] > 0 && width > rect[2] - rect[0])) rect[2] = rect[0] + width;
        } else {
            height = draw_text_block(d, s, rect, text, n, format, 1, &width);
            mark_dirty(d, s);
        }
    }
    UNLOCK();
    return height;
}

int GDI_MS beer_DrawTextA(uint64_t dc, const char *text, int count, int32_t *rect, uint32_t format)
{
    uint16_t wide[1024];
    if (!text) return 0;
    int n = 0;
    while (n < 1023 && (count < 0 ? text[n] : n < count)) { wide[n] = (uint8_t)text[n]; ++n; }
    wide[n] = 0;
    return beer_DrawTextW(dc, wide, n, rect, format);
}

/* -------------------------------------------------------------- GDI32 API */

uint64_t GDI_MS beer_GetStockObject(int index)
{
    LOCK();
    gdi_init();
    uint64_t h = index >= 0 && index < 20 ? g_stock[index] : 0;
    if (index == 15) h = 0;
    UNLOCK();
    return h;
}

uint64_t GDI_MS beer_CreateCompatibleDC(uint64_t dc)
{
    (void)dc;
    LOCK();
    gdi_init();
    uint64_t h = 0;
    for (int i = 0; i < DC_MAX; ++i)
        if (!g_dcs[i].used) {
            memset(&g_dcs[i], 0, sizeof(g_dcs[i]));
            g_dcs[i].used = 1;
            g_dcs[i].bitmap = g_default_bitmap;
            g_dcs[i].font = g_stock[17];
            g_dcs[i].brush = g_stock[0];
            g_dcs[i].pen = g_stock[7];
            g_dcs[i].bk_color = 0xFFFFFF;
            g_dcs[i].bk_mode = 2;
            h = DC_BASE + (uint64_t)i;
            break;
        }
    UNLOCK();
    return h;
}

uint64_t GDI_MS beer_CreateCompatibleBitmap(uint64_t dc, int width, int height)
{
    (void)dc;
    Surface s = {0};
    if (!surface_alloc(&s, width, height, 32, 1)) return 0;
    LOCK();
    gdi_init();
    uint64_t h = bitmap_from_surface(&s);
    UNLOCK();
    return h;
}

uint64_t GDI_MS beer_CreateBitmap(int width, int height, uint32_t planes, uint32_t bits, const void *data)
{
    (void)planes;
    Surface s = {0};
    if (!surface_alloc(&s, width, height, 32, 1)) return 0;
    if (data && bits == 32) memcpy(s.bits, data, (size_t)s.stride * height);
    LOCK();
    gdi_init();
    uint64_t h = bitmap_from_surface(&s);
    UNLOCK();
    return h;
}

uint64_t GDI_MS beer_CreateDIBSection(uint64_t dc, const void *info, uint32_t usage, void **bits,
                                      uint64_t section, uint32_t offset)
{
    (void)dc; (void)usage; (void)section; (void)offset;
    if (bits) *bits = NULL;
    if (!info) return 0;
    Bih h;
    memcpy(&h, info, sizeof(h));
    if (h.size < 40 || (h.bit_count != 32 && h.bit_count != 24) || (h.compression != 0 && h.compression != 3))
        return 0;
    Surface s = {0};
    if (!surface_alloc(&s, h.width, h.height < 0 ? -h.height : h.height, h.bit_count, h.height < 0)) return 0;
    s.is_dib = 1;
    void *pixels = s.bits;
    LOCK();
    gdi_init();
    uint64_t handle = bitmap_from_surface(&s);
    UNLOCK();
    if (handle && bits) *bits = pixels;
    return handle;
}

int GDI_MS beer_DeleteDC(uint64_t dc)
{
    LOCK();
    int ok = 0;
    if (dc >= DC_BASE && dc < DC_BASE + DC_MAX && g_dcs[dc - DC_BASE].used) {
        g_dcs[dc - DC_BASE].used = 0;
        ok = 1;
    }
    UNLOCK();
    return ok;
}

int GDI_MS beer_DeleteObject(uint64_t object)
{
    LOCK();
    Obj *o = obj_get(object);
    int ok = 0;
    if (o) {
        ok = 1;
        if (!o->stock) obj_free(object);
    }
    UNLOCK();
    return ok;
}

uint64_t GDI_MS beer_SelectObject(uint64_t dc, uint64_t object)
{
    LOCK();
    Dc *d = dc_get(dc);
    Obj *o = obj_get(object);
    uint64_t previous = 0;
    if (d && o) {
        switch (o->type) {
        case OBJ_BITMAP:
            if (!d->is_window) { previous = d->bitmap; d->bitmap = object; }
            break;
        case OBJ_FONT: previous = d->font; d->font = object; break;
        case OBJ_BRUSH: previous = d->brush; d->brush = object; break;
        case OBJ_PEN: previous = d->pen; d->pen = object; break;
        }
    }
    UNLOCK();
    return previous;
}

int GDI_MS beer_GetObjectW(uint64_t object, int size, void *out)
{
    LOCK();
    Obj *o = obj_get(object);
    int written = 0;
    if (o && !out) {
        written = o->type == OBJ_BITMAP ? (o->surface.is_dib ? 96 : 32) : o->type == OBJ_FONT ? 92 :
                  o->type == OBJ_BRUSH ? 16 : o->type == OBJ_PEN ? 16 : 0;
    } else if (o) {
        uint8_t tmp[96] = {0};
        int full = 0;
        if (o->type == OBJ_BITMAP) {
            const Surface *s = &o->surface;
            int32_t v[4] = { 0, s->w, s->h, s->stride };
            uint16_t planes = 1, bpp = (uint16_t)s->bpp;
            memcpy(tmp, v, 16);
            memcpy(tmp + 16, &planes, 2);
            memcpy(tmp + 18, &bpp, 2);
            uint64_t bits = s->is_dib ? (uint64_t)(uintptr_t)s->bits : 0;
            memcpy(tmp + 24, &bits, 8);
            full = 32;
            if (s->is_dib) {
                Bih h = { 40, s->w, s->topdown ? -s->h : s->h, 1, (uint16_t)s->bpp, 0, (uint32_t)(s->stride * s->h),
                          0, 0, 0, 0 };
                memcpy(tmp + 32, &h, 40);
                full = 96;
            }
        } else if (o->type == OBJ_FONT) {
            memcpy(tmp, o->logfont, 92);
            full = 92;
        } else if (o->type == OBJ_BRUSH) {
            uint32_t v[2] = { (uint32_t)o->style, o->color };
            memcpy(tmp, v, 8);
            full = 16;
        } else if (o->type == OBJ_PEN) {
            uint32_t v[4] = { (uint32_t)o->style, (uint32_t)o->width, 0, o->color };
            memcpy(tmp, v, 16);
            full = 16;
        }
        written = size < full ? size : full;
        if (o->type == OBJ_BITMAP && size < 32) written = 0;
        if (written > 0) memcpy(out, tmp, (size_t)written);
    }
    UNLOCK();
    return written;
}

uint32_t GDI_MS beer_GetObjectType(uint64_t object)
{
    LOCK();
    Obj *o = obj_get(object);
    uint32_t t = 0;
    if (o) t = o->type == OBJ_PEN ? 1 : o->type == OBJ_BRUSH ? 2 : o->type == OBJ_BITMAP ? 7 : 6;
    else if (object == BEER_GDI_WINDOW_DC || (object >= DC_BASE && object < DC_BASE + DC_MAX)) t = 3;
    UNLOCK();
    return t;
}

int GDI_MS beer_GetDeviceCaps(uint64_t dc, int index)
{
    (void)dc;
    int sw = 1920, sh = 1080;
    xwayland_get_screen_size(&sw, &sh);
    switch (index) {
    case 0: return 0x0400;                 /* DRIVERVERSION */
    case 2: return 1;                      /* TECHNOLOGY: DT_RASDISPLAY */
    case 4: return sw * 254 / 960;         /* HORZSIZE (mm at 96 dpi) */
    case 6: return sh * 254 / 960;         /* VERTSIZE */
    case 8: return sw;                     /* HORZRES */
    case 10: return sh;                    /* VERTRES */
    case 12: return 32;                    /* BITSPIXEL */
    case 14: return 1;                     /* PLANES */
    case 16: return -1;                    /* NUMBRUSHES */
    case 18: return -1;                    /* NUMPENS */
    case 22: return 0;                     /* NUMFONTS */
    case 24: return -1;                    /* NUMCOLORS */
    case 38: return 0x7e99;                /* RASTERCAPS */
    case 40: case 42: return 36;           /* ASPECTX / ASPECTY */
    case 44: return 51;                    /* ASPECTXY */
    case 88: case 90: return 96;           /* LOGPIXELSX / LOGPIXELSY */
    case 104: return 0;                    /* SIZEPALETTE */
    case 108: return 24;                   /* COLORRES */
    case 110: return sw;                   /* PHYSICALWIDTH */
    case 111: return sh;                   /* PHYSICALHEIGHT */
    case 114: case 115: return 1000;       /* SCALINGFACTORX/Y */
    case 116: { int hz = 60; return hz; }  /* VREFRESH */
    default: return 0;
    }
}

uint64_t GDI_MS beer_CreateFontW(int height, int width, int escapement, int orientation, int weight,
                                 uint32_t italic, uint32_t underline, uint32_t strike, uint32_t charset,
                                 uint32_t out_precision, uint32_t clip_precision, uint32_t quality,
                                 uint32_t pitch_family, const uint16_t *face)
{
    (void)escapement; (void)orientation; (void)out_precision; (void)clip_precision; (void)quality;
    (void)pitch_family;
    LOCK();
    gdi_init();
    uint64_t h = make_font(height, width, weight, (int)italic, (int)underline, (int)strike, (int)charset, face);
    UNLOCK();
    return h;
}

uint64_t GDI_MS beer_CreateFontA(int height, int width, int escapement, int orientation, int weight,
                                 uint32_t italic, uint32_t underline, uint32_t strike, uint32_t charset,
                                 uint32_t out_precision, uint32_t clip_precision, uint32_t quality,
                                 uint32_t pitch_family, const char *face)
{
    uint16_t wide[32] = {0};
    for (int i = 0; face && i < 31 && face[i]; ++i) wide[i] = (uint8_t)face[i];
    return beer_CreateFontW(height, width, escapement, orientation, weight, italic, underline, strike,
                            charset, out_precision, clip_precision, quality, pitch_family, wide);
}

uint64_t GDI_MS beer_CreateFontIndirectW(const uint8_t *lf)
{
    if (!lf) return 0;
    int32_t v[5];
    memcpy(v, lf, 20);
    uint16_t face[32];
    memcpy(face, lf + 28, 64);
    face[31] = 0;
    return beer_CreateFontW(v[0], v[1], v[2], v[3], v[4], lf[20], lf[21], lf[22], lf[23], 0, 0, 0, 0, face);
}

uint64_t GDI_MS beer_CreateSolidBrush(uint32_t color)
{
    LOCK();
    gdi_init();
    uint64_t h = make_brush(color, 0);
    UNLOCK();
    return h;
}

uint64_t GDI_MS beer_CreatePen(int style, int width, uint32_t color)
{
    LOCK();
    gdi_init();
    uint64_t h = make_pen(color, style, width);
    UNLOCK();
    return h;
}

#define DC_SETTER(NAME, FIELD, TYPE) \
    TYPE GDI_MS NAME(uint64_t dc, TYPE value) \
    { \
        LOCK(); \
        Dc *d = dc_get(dc); \
        TYPE previous = d ? (TYPE)d->FIELD : 0; \
        if (d) d->FIELD = value; \
        UNLOCK(); \
        return previous; \
    }

DC_SETTER(beer_SetBkColor, bk_color, uint32_t)
DC_SETTER(beer_SetTextColor, text_color, uint32_t)
DC_SETTER(beer_SetTextAlign, text_align, uint32_t)

int GDI_MS beer_SetBkMode(uint64_t dc, int mode)
{
    LOCK();
    Dc *d = dc_get(dc);
    int previous = d ? d->bk_mode : 0;
    if (d) d->bk_mode = mode;
    UNLOCK();
    return previous;
}

uint32_t GDI_MS beer_GetBkColor(uint64_t dc)
{
    LOCK();
    Dc *d = dc_get(dc);
    uint32_t c = d ? d->bk_color : 0;
    UNLOCK();
    return c;
}

uint32_t GDI_MS beer_GetTextColor(uint64_t dc)
{
    LOCK();
    Dc *d = dc_get(dc);
    uint32_t c = d ? d->text_color : 0;
    UNLOCK();
    return c;
}

int GDI_MS beer_GetTextExtentPoint32W(uint64_t dc, const uint16_t *text, int count, int32_t *size)
{
    if (!size) return 0;
    LOCK();
    Dc *d = dc_get(dc);
    int cell_h = 0;
    const OverlayFont *font = font_for(d ? d->font : 0, &cell_h);
    size[0] = text_width(font, text ? wlen(text, count) : 0);
    size[1] = cell_h;
    UNLOCK();
    return 1;
}

int GDI_MS beer_GetTextExtentPoint32A(uint64_t dc, const char *text, int count, int32_t *size)
{
    if (!size) return 0;
    int n = 0;
    while (text && (count < 0 ? text[n] : n < count)) ++n;
    uint16_t blank[1] = {0};
    (void)blank;
    LOCK();
    Dc *d = dc_get(dc);
    int cell_h = 0;
    const OverlayFont *font = font_for(d ? d->font : 0, &cell_h);
    size[0] = text_width(font, n);
    size[1] = cell_h;
    UNLOCK();
    return 1;
}

int GDI_MS beer_GetTextMetricsW(uint64_t dc, uint8_t *metrics)
{
    if (!metrics) return 0;
    LOCK();
    Dc *d = dc_get(dc);
    int cell_h = 0;
    const OverlayFont *font = font_for(d ? d->font : 0, &cell_h);
    int32_t v[11] = { cell_h, cell_h - cell_h / 5, cell_h / 5, 0, 0, font->cell_w, font->cell_w, 400, 0, 96, 96 };
    memset(metrics, 0, 60);
    memcpy(metrics, v, 44);
    uint16_t chars[4] = { 32, 126, '?', ' ' };
    memcpy(metrics + 44, chars, 8);
    metrics[56] = 1; /* TMPF_FIXED_PITCH is cleared for variable; report fixed pitch family */
    UNLOCK();
    return 1;
}

int GDI_MS beer_TextOutW(uint64_t dc, int x, int y, const uint16_t *text, int count)
{
    if (!text) return 0;
    LOCK();
    Dc *d = dc_get(dc);
    Surface *s = dc_surface(d);
    if (d && s) { draw_text(d, s, x, y, text, wlen(text, count), d->font); mark_dirty(d, s); }
    UNLOCK();
    return s != NULL;
}

int GDI_MS beer_TextOutA(uint64_t dc, int x, int y, const char *text, int count)
{
    uint16_t wide[1024];
    if (!text) return 0;
    int n = 0;
    while (n < 1023 && (count < 0 ? text[n] : n < count)) { wide[n] = (uint8_t)text[n]; ++n; }
    return beer_TextOutW(dc, x, y, wide, n);
}

int GDI_MS beer_ExtTextOutW(uint64_t dc, int x, int y, uint32_t options, const int32_t *rect,
                            const uint16_t *text, uint32_t count, const int *dx)
{
    (void)dx;
    LOCK();
    Dc *d = dc_get(dc);
    Surface *s = dc_surface(d);
    if (d && s) {
        if ((options & 2 /* ETO_OPAQUE */) && rect)
            fill_rect_px(s, rect[0], rect[1], rect[2], rect[3], from_colorref(d->bk_color));
        if (text && count) {
            int saved = d->bk_mode;
            if (options & 2) d->bk_mode = 1;
            draw_text(d, s, x, y, text, (int)count, d->font);
            d->bk_mode = saved;
        }
        mark_dirty(d, s);
    }
    UNLOCK();
    return s != NULL;
}

int GDI_MS beer_BitBlt(uint64_t dst, int x, int y, int w, int h, uint64_t src, int sx, int sy, uint32_t rop)
{
    LOCK();
    int ok = do_stretch(dst, x, y, w, h, src, sx, sy, w, h, rop);
    UNLOCK();
    return ok;
}

int GDI_MS beer_StretchBlt(uint64_t dst, int x, int y, int w, int h, uint64_t src, int sx, int sy, int sw, int sh,
                           uint32_t rop)
{
    LOCK();
    int ok = do_stretch(dst, x, y, w, h, src, sx, sy, sw, sh, rop);
    UNLOCK();
    return ok;
}

int GDI_MS beer_PatBlt(uint64_t dc, int x, int y, int w, int h, uint32_t rop)
{
    LOCK();
    int ok = do_stretch(dc, x, y, w, h, 0, 0, 0, w, h, rop);
    UNLOCK();
    return ok;
}

int GDI_MS beer_Rectangle(uint64_t dc, int left, int top, int right, int bottom)
{
    LOCK();
    Dc *d = dc_get(dc);
    Surface *s = dc_surface(d);
    if (d && s) {
        Obj *brush = obj_get(d->brush), *pen = obj_get(d->pen);
        int hollow = brush && brush->style == 1;
        int no_pen = pen && pen->style == 5;
        if (!hollow) fill_rect_px(s, left, top, right, bottom, from_colorref(brush ? brush->color : 0xFFFFFF));
        if (!no_pen) {
            uint32_t c = from_colorref(pen ? pen->color : 0);
            fill_rect_px(s, left, top, right, top + 1, c);
            fill_rect_px(s, left, bottom - 1, right, bottom, c);
            fill_rect_px(s, left, top, left + 1, bottom, c);
            fill_rect_px(s, right - 1, top, right, bottom, c);
        }
        mark_dirty(d, s);
    }
    UNLOCK();
    return s != NULL;
}

int GDI_MS beer_StretchDIBits(uint64_t dc, int xd, int yd, int wd, int hd, int xs, int ys, int ws, int hs,
                              const void *bits, const void *info, uint32_t usage, uint32_t rop)
{
    (void)usage;
    if (!info || !bits) return 0;
    Surface src = {0};
    LOCK();
    Dc *d = dc_get(dc);
    Surface *s = dc_surface(d);
    int lines = 0;
    if (s && decode_dib(info, bits, &src)) {
        /* Source rows are given bottom-up in DIB space; decode already returned a top-down image,
         * but ys counts from the bottom of a bottom-up DIB. */
        Bih h;
        memcpy(&h, info, sizeof(h));
        int sy = h.height > 0 ? src.h - ys - hs : ys;
        blit_generic(d, s, xd, yd, wd, hd, &src, xs, sy, ws, hs, rop);
        mark_dirty(d, s);
        lines = hs < 0 ? -hs : hs;
    }
    free(src.bits);
    UNLOCK();
    return lines;
}

int GDI_MS beer_SetDIBitsToDevice(uint64_t dc, int xd, int yd, uint32_t w, uint32_t h, int xs, int ys,
                                  uint32_t start, uint32_t lines, const void *bits, const void *info,
                                  uint32_t usage)
{
    (void)start; (void)lines;
    return beer_StretchDIBits(dc, xd, yd, (int)w, (int)h, xs, ys, (int)w, (int)h, bits, info, usage, 0x00CC0020);
}

uint32_t GDI_MS beer_SetPixel(uint64_t dc, int x, int y, uint32_t color)
{
    LOCK();
    Dc *d = dc_get(dc);
    Surface *s = dc_surface(d);
    uint32_t result = 0xFFFFFFFFu;
    if (s && x >= 0 && y >= 0 && x < s->w && y < s->h) {
        s_put(s, x, y, from_colorref(color));
        mark_dirty(d, s);
        result = color;
    }
    UNLOCK();
    return result;
}

uint32_t GDI_MS beer_GetPixel(uint64_t dc, int x, int y)
{
    LOCK();
    Dc *d = dc_get(dc);
    Surface *s = dc_surface(d);
    uint32_t result = 0xFFFFFFFFu;
    if (s && x >= 0 && y >= 0 && x < s->w && y < s->h) result = to_colorref(s_get(s, x, y));
    UNLOCK();
    return result;
}

int GDI_MS beer_GdiFlush(void)
{
    beer_gdi_flush_window();
    return 1;
}

int GDI_MS beer_SetStretchBltMode(uint64_t dc, int mode) { (void)dc; (void)mode; return 1; }
int GDI_MS beer_SaveDC(uint64_t dc) { (void)dc; return 1; }
int GDI_MS beer_RestoreDC(uint64_t dc, int saved) { (void)dc; (void)saved; return 1; }

int GDI_MS beer_AlphaBlend(uint64_t dst, int x, int y, int w, int h, uint64_t src, int sx, int sy, int sw, int sh,
                           uint32_t blend)
{
    LOCK();
    Dc *dd = dc_get(dst), *sd = dc_get(src);
    Surface *d = dc_surface(dd), *s = dc_surface(sd);
    int ok = d && s && w > 0 && h > 0 && sw > 0 && sh > 0;
    if (ok) {
        unsigned constant = (blend >> 16) & 0xff;
        int per_pixel = (blend >> 24) & 1;
        for (int j = 0; j < h; ++j)
            for (int i = 0; i < w; ++i) {
                int px = x + i, py = y + j;
                int qx = sx + (int)((int64_t)i * sw / w), qy = sy + (int)((int64_t)j * sh / h);
                if (px < 0 || py < 0 || px >= d->w || py >= d->h || qx < 0 || qy < 0 || qx >= s->w || qy >= s->h)
                    continue;
                uint32_t sp = s_get(s, qx, qy), dp = s_get(d, px, py);
                unsigned sa = per_pixel ? (sp >> 24) : 255;
                unsigned k = sa * constant / 255;       /* effective source weight */
                uint32_t out = 0xFF000000u;
                for (int sh2 = 0; sh2 <= 16; sh2 += 8) {
                    unsigned sc = (sp >> sh2) & 0xff, dc = (dp >> sh2) & 0xff;
                    /* Source colours are premultiplied when AC_SRC_ALPHA is used. */
                    unsigned contrib = per_pixel ? sc * constant / 255 : sc * constant / 255;
                    unsigned v = contrib + dc * (255 - k) / 255;
                    out |= (uint32_t)(v > 255 ? 255 : v) << sh2;
                }
                s_put(d, px, py, out);
            }
        mark_dirty(dd, d);
    }
    UNLOCK();
    return ok;
}

int GDI_MS beer_TransparentBlt(uint64_t dst, int x, int y, int w, int h, uint64_t src, int sx, int sy, int sw,
                               int sh, uint32_t color)
{
    LOCK();
    Dc *dd = dc_get(dst), *sd = dc_get(src);
    Surface *d = dc_surface(dd), *s = dc_surface(sd);
    int ok = d && s && w > 0 && h > 0 && sw > 0 && sh > 0;
    if (ok) {
        uint32_t key = from_colorref(color) & 0xFFFFFF;
        for (int j = 0; j < h; ++j)
            for (int i = 0; i < w; ++i) {
                int px = x + i, py = y + j;
                int qx = sx + (int)((int64_t)i * sw / w), qy = sy + (int)((int64_t)j * sh / h);
                if (px < 0 || py < 0 || px >= d->w || py >= d->h || qx < 0 || qy < 0 || qx >= s->w || qy >= s->h)
                    continue;
                uint32_t sp = s_get(s, qx, qy);
                if ((sp & 0xFFFFFF) != key) s_put(d, px, py, sp | 0xFF000000u);
            }
        mark_dirty(dd, d);
    }
    UNLOCK();
    return ok;
}
