/* GDI text with real fonts: FreeType (libfreetype.so.6, loaded at run time) renders the font a LOGFONT asks for,
 * mapped to a font file on this system (Wine's metric-compatible Tahoma for the Windows UI faces, Liberation for
 * Arial / Times New Roman / Courier New, Noto Sans for Segoe UI), antialiased and proportional. Without FreeType or a
 * font file the built-in fixed-width overlay font is used, as before.
 *
 * Included into win32_gdi.c (uses Surface, s_get/s_put, Obj and the overlay fonts; called under the GDI lock).
 * FreeType has no headers here: only the stable public ABI parts Beer reads are declared below (64-bit layouts). */

#include <dlfcn.h>
#include <unistd.h>

typedef struct {
    int loaded;
    void *library;
    int (*Init_FreeType)(void **library);
    int (*New_Face)(void *library, const char *path, long index, void **face);
    int (*Set_Pixel_Sizes)(void *face, unsigned width, unsigned height);
    int (*Load_Char)(void *face, unsigned long code, int32_t flags);
    void *(*Get_Sfnt_Table)(void *face, int tag);
    unsigned (*Get_Char_Index)(void *face, unsigned long code);
} FtApi;
static FtApi g_ft;

/* FT_FaceRec / FT_SizeRec / FT_GlyphSlotRec field offsets (FreeType 2, LP64). */
#define FT_FACE_UNITS_PER_EM 136   /* FT_UShort */
#define FT_FACE_ASCENDER     138   /* FT_Short */
#define FT_FACE_DESCENDER    140   /* FT_Short */
#define FT_FACE_GLYPH        152   /* FT_GlyphSlot */
#define FT_FACE_SIZE         160   /* FT_Size */
#define FT_SIZE_ASCENDER      48   /* FT_Size_Metrics.ascender, 26.6 */
#define FT_SIZE_DESCENDER     56
#define FT_SIZE_HEIGHT        64
#define FT_SLOT_ADVANCE_X    128   /* FT_Vector.x, 26.6 */
#define FT_SLOT_BITMAP       152   /* FT_Bitmap: rows u32, width u32, pitch s32, buffer at +16, pixel_mode u8 at +26 */
#define FT_SLOT_LEFT         192   /* FT_Int bitmap_left */
#define FT_SLOT_TOP          196   /* FT_Int bitmap_top */
#define FT_OS2_WIN_ASCENT     96   /* TT_OS2.usWinAscent (FT_Get_Sfnt_Table(face, FT_SFNT_OS2)) */
#define FT_OS2_WIN_DESCENT    98
#define FT_LOAD_RENDER       (1 << 2)
#define FT_LOAD_TARGET_LIGHT (1 << 16)

static int ft_load(void)
{
    if (g_ft.loaded) return g_ft.library != NULL;
    g_ft.loaded = 1;
    if (getenv("BEER_GDI_NO_FREETYPE")) return 0;
    void *so = dlopen("libfreetype.so.6", RTLD_NOW | RTLD_LOCAL);
    if (!so) return 0;
    g_ft.Init_FreeType = (int (*)(void **))dlsym(so, "FT_Init_FreeType");
    g_ft.New_Face = (int (*)(void *, const char *, long, void **))dlsym(so, "FT_New_Face");
    g_ft.Set_Pixel_Sizes = (int (*)(void *, unsigned, unsigned))dlsym(so, "FT_Set_Pixel_Sizes");
    g_ft.Load_Char = (int (*)(void *, unsigned long, int32_t))dlsym(so, "FT_Load_Char");
    g_ft.Get_Sfnt_Table = (void *(*)(void *, int))dlsym(so, "FT_Get_Sfnt_Table");    /* optional */
    g_ft.Get_Char_Index = (unsigned (*)(void *, unsigned long))dlsym(so, "FT_Get_Char_Index");   /* optional */
    if (!g_ft.Init_FreeType || !g_ft.New_Face || !g_ft.Set_Pixel_Sizes || !g_ft.Load_Char ||
        g_ft.Init_FreeType(&g_ft.library) != 0)
        g_ft.library = NULL;
    return g_ft.library != NULL;
}

/* ---- face names -> font files ---- */
enum { GF_SANS_UI, GF_SANS, GF_SANS_SEGOE, GF_SERIF, GF_MONO, GF_MS_SANS, GF_FAMILIES };
static const char *const g_font_files[GF_FAMILIES][2][4] = {     /* [family][bold] candidates */
    { { "/usr/share/wine/fonts/tahoma.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", NULL },
      { "/usr/share/wine/fonts/tahomabd.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", NULL } },
    { { "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf", "/usr/share/wine/fonts/tahoma.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", NULL },
      { "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf", "/usr/share/wine/fonts/tahomabd.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", NULL } },
    { { "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf", "/usr/share/wine/fonts/tahoma.ttf", NULL },
      { "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf", "/usr/share/wine/fonts/tahomabd.ttf", NULL } },
    { { "/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf", NULL, NULL },
      { "/usr/share/fonts/truetype/liberation/LiberationSerif-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf", NULL, NULL } },
    { { "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", NULL, NULL },
      { "/usr/share/fonts/truetype/liberation/LiberationMono-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf", NULL, NULL } },
    { { "/usr/share/wine/fonts/tahoma.ttf",     /* Wine's ms_sans_serif.ttf has only bitmap strikes: not scalable */
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf", NULL, NULL },
      { "/usr/share/wine/fonts/tahomabd.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf", NULL,
        NULL } },
};
/* "MS Sans Serif" is a bitmap font on Windows (sserife.fon): only these sizes exist, and a request is answered with
 * the largest one whose character height fits (else the smallest). Inno Setup measures it to scale its wizard:
 * lfHeight -10 must still give the 13-pixel 8 pt font. { character height, tmHeight, tmAscent } at 96 dpi. */
static const int g_ms_sans_sizes[][3] = { { 11, 13, 11 }, { 13, 16, 13 }, { 16, 20, 16 }, { 19, 24, 19 },
                                          { 24, 29, 23 }, { 29, 37, 29 } };
static int face_name_is(const uint16_t *face, const char *name)
{
    size_t i = 0;
    for (; name[i]; ++i) {
        uint16_t c = face[i];
        if (c >= 'A' && c <= 'Z') c = (uint16_t)(c + 32);
        char n = name[i];
        if (n >= 'A' && n <= 'Z') n = (char)(n + 32);
        if (c != (uint16_t)(unsigned char)n) return 0;
    }
    return face[i] == 0;
}
static int face_family(const uint16_t *face, int pitch_family)
{
    static const char *const mono[] = { "Courier New", "Courier", "Consolas", "Lucida Console", "Fixedsys", "Terminal",
                                        "Liberation Mono", "DejaVu Sans Mono", NULL };
    static const char *const serif[] = { "Times New Roman", "Times", "Georgia", "Cambria", "Book Antiqua", "Garamond",
                                         NULL };
    static const char *const sans[] = { "Arial", "Helvetica", "Arial Black", "Calibri", "Liberation Sans", NULL };
    static const char *const segoe[] = { "Segoe UI", "Segoe UI Semibold", "Segoe UI Light", "Verdana", NULL };
    for (int i = 0; mono[i]; ++i) if (face_name_is(face, mono[i])) return GF_MONO;
    for (int i = 0; serif[i]; ++i) if (face_name_is(face, serif[i])) return GF_SERIF;
    for (int i = 0; sans[i]; ++i) if (face_name_is(face, sans[i])) return GF_SANS;
    for (int i = 0; segoe[i]; ++i) if (face_name_is(face, segoe[i])) return GF_SANS_SEGOE;
    if (face_name_is(face, "MS Sans Serif")) return GF_MS_SANS;
    if (!face[0] && (pitch_family & 3) == 1) return GF_MONO;          /* FIXED_PITCH without a face name */
    if (!face[0] && (pitch_family & 0xf0) == 0x10) return GF_SERIF;    /* FF_ROMAN */
    if (!face[0] && (pitch_family & 0xf0) == 0x30) return GF_MONO;     /* FF_MODERN */
    return GF_SANS_UI;                                                  /* Tahoma, MS Shell Dlg, MS Sans Serif, ... */
}

/* ---- faces at a pixel size, with a glyph cache ---- */
typedef struct {
    uint32_t code;
    int16_t left, top, advance;
    uint16_t w, h;
    uint8_t *alpha;                    /* w x h coverage, NULL for an empty glyph */
} GdiGlyph;
typedef struct {
    int used, family, bold, em;        /* key */
    void *face;                        /* FT_Face; NULL: the overlay font */
    int ascent, descent, height, avg_width, max_width;
    GdiGlyph *glyphs;                  /* open addressing on code */
    size_t glyph_capacity, glyph_count;
    const OverlayFont *overlay;
    int fixed;
    int pixel_em;                      /* the size the face is set to */
    void *fallback;                    /* DejaVu Sans at the same size, for characters the face lacks (0: not opened) */
    int fallback_tried;
} GdiFont;
#define GDI_FONT_CACHE 48
static GdiFont g_gdi_fonts[GDI_FONT_CACHE];

static GdiGlyph *gf_glyph(GdiFont *f, uint32_t code)
{
    if (!f->face) return NULL;
    if (f->glyph_count * 2 + 2 >= f->glyph_capacity) {               /* grow and rehash */
        const size_t capacity = f->glyph_capacity ? f->glyph_capacity * 2 : 256;
        GdiGlyph *table = calloc(capacity, sizeof(*table));
        if (!table) return NULL;
        for (size_t i = 0; i < f->glyph_capacity; ++i) {
            const GdiGlyph *g = &f->glyphs[i];
            if (!g->code && !g->advance && !g->alpha) continue;
            size_t j = (size_t)g->code * 2654435761u % capacity;
            while (table[j].code || table[j].advance || table[j].alpha) j = (j + 1) % capacity;
            table[j] = *g;
        }
        free(f->glyphs);
        f->glyphs = table; f->glyph_capacity = capacity;
    }
    size_t j = (size_t)code * 2654435761u % f->glyph_capacity;
    while (f->glyphs[j].code || f->glyphs[j].advance || f->glyphs[j].alpha) {
        if (f->glyphs[j].code == code) return &f->glyphs[j];
        j = (j + 1) % f->glyph_capacity;
    }
    GdiGlyph *g = &f->glyphs[j];
    memset(g, 0, sizeof(*g));
    g->code = code;
    /* A character the face does not have comes from DejaVu Sans (broad symbol coverage: check marks, arrows). */
    void *face = f->face;
    if (g_ft.Get_Char_Index && code > 0x7f && !g_ft.Get_Char_Index(face, code)) {
        if (!f->fallback_tried) {
            f->fallback_tried = 1;
            void *fallback = NULL;
            if (g_ft.New_Face(g_ft.library, "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 0, &fallback) == 0) {
                g_ft.Set_Pixel_Sizes(fallback, 0, (unsigned)(f->pixel_em > 0 ? f->pixel_em : 11));
                f->fallback = fallback;
            }
        }
        if (f->fallback && g_ft.Get_Char_Index(f->fallback, code)) face = f->fallback;
    }
    if (g_ft.Load_Char(face, code, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) != 0 &&
        g_ft.Load_Char(face = f->face, '?', FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) != 0) {
        g->advance = (int16_t)(f->avg_width > 0 ? f->avg_width : 1);
        ++f->glyph_count;
        return g;
    }
    const uint8_t *slot = *(uint8_t *const *)((const uint8_t *)face + FT_FACE_GLYPH);
    const long advance = *(const long *)(slot + FT_SLOT_ADVANCE_X);
    const uint32_t rows = *(const uint32_t *)(slot + FT_SLOT_BITMAP);
    const uint32_t width = *(const uint32_t *)(slot + FT_SLOT_BITMAP + 4);
    const int32_t pitch = *(const int32_t *)(slot + FT_SLOT_BITMAP + 8);
    const uint8_t *buffer = *(uint8_t *const *)(slot + FT_SLOT_BITMAP + 16);
    const uint8_t mode = *(slot + FT_SLOT_BITMAP + 26);
    g->advance = (int16_t)((advance + 32) >> 6);
    if (!g->advance && code != 0) g->advance = 1;
    g->left = (int16_t)*(const int32_t *)(slot + FT_SLOT_LEFT);
    g->top = (int16_t)*(const int32_t *)(slot + FT_SLOT_TOP);
    if (rows && width && buffer && rows < 512 && width < 512) {
        g->alpha = malloc((size_t)rows * width);
        if (g->alpha) {
            g->w = (uint16_t)width; g->h = (uint16_t)rows;
            for (uint32_t y = 0; y < rows; ++y) {
                const uint8_t *row = buffer + (pitch >= 0 ? (size_t)y * (size_t)pitch
                                                          : (size_t)(rows - 1 - y) * (size_t)(-pitch));
                for (uint32_t x = 0; x < width; ++x)
                    g->alpha[(size_t)y * width + x] = mode == 1 ? ((row[x >> 3] >> (7 - (x & 7))) & 1 ? 255 : 0)
                                                                : row[x];
            }
        }
    }
    ++f->glyph_count;
    return g;
}

static GdiFont *gf_open(int family, int bold, int em, int cell)
{
    for (int i = 0; i < GDI_FONT_CACHE; ++i) {
        GdiFont *f = &g_gdi_fonts[i];
        if (f->used && f->family == family && f->bold == bold && f->em == (cell ? -cell : em)) return f;
    }
    GdiFont *f = NULL;
    for (int i = 0; i < GDI_FONT_CACHE && !f; ++i) if (!g_gdi_fonts[i].used) f = &g_gdi_fonts[i];
    if (!f) {                                                              /* evict the oldest entry */
        f = &g_gdi_fonts[0];
        for (size_t i = 0; i < f->glyph_capacity; ++i) free(f->glyphs[i].alpha);
        free(f->glyphs);
        memmove(&g_gdi_fonts[0], &g_gdi_fonts[1], sizeof(g_gdi_fonts) - sizeof(g_gdi_fonts[0]));
        f = &g_gdi_fonts[GDI_FONT_CACHE - 1];
    }
    memset(f, 0, sizeof(*f));
    f->used = 1; f->family = family; f->bold = bold; f->em = cell ? -cell : em;
    if (ft_load()) {
        for (int c = 0; c < 4 && !f->face; ++c) {
            const char *path = g_font_files[family][bold][c];
            if (!path) break;
            void *face = NULL;
            if (access(path, R_OK) == 0 && g_ft.New_Face(g_ft.library, path, 0, &face) == 0) f->face = face;
        }
    }
    if (f->face) {
        /* Windows' ascent and descent are the OS/2 table's usWinAscent / usWinDescent (FreeType's face values may be the
         * typographic ones: Tahoma's give 10 pixels at 8 pt where Windows has 13); hhea when there is no OS/2 table. */
        int asc_units = *(const int16_t *)((const uint8_t *)f->face + FT_FACE_ASCENDER);
        int desc_units = -*(const int16_t *)((const uint8_t *)f->face + FT_FACE_DESCENDER);
        const uint8_t *os2 = g_ft.Get_Sfnt_Table ? g_ft.Get_Sfnt_Table(f->face, 2) : NULL;
        if (os2) {
            const int win_ascent = *(const uint16_t *)(os2 + FT_OS2_WIN_ASCENT);
            const int win_descent = *(const uint16_t *)(os2 + FT_OS2_WIN_DESCENT);
            if (win_ascent + win_descent > 0) { asc_units = win_ascent; desc_units = win_descent; }
        }
        int pixel_em = em;
        if (cell) {                                           /* LOGFONT height > 0: the cell height (ascent + descent) */
            const int units = *(const uint16_t *)((const uint8_t *)f->face + FT_FACE_UNITS_PER_EM);
            pixel_em = asc_units + desc_units > 0 ? (cell * units + (asc_units + desc_units) / 2) / (asc_units + desc_units)
                                                  : cell;
        }
        if (pixel_em < 1) pixel_em = 1;
        if (pixel_em > 400) pixel_em = 400;
        g_ft.Set_Pixel_Sizes(f->face, 0, (unsigned)pixel_em);
        f->pixel_em = pixel_em;
        /* Ascent and descent as Windows reports them: the font's design values scaled to the size and rounded to the
         * nearest pixel (FreeType's size metrics round up: 8 pt Tahoma would be 15 pixels high instead of 13, and
         * dialog layouts scaled from tmHeight would come out too tall). */
        const int units = *(const uint16_t *)((const uint8_t *)f->face + FT_FACE_UNITS_PER_EM);
        if (units > 0) {
            f->ascent = (pixel_em * asc_units + units / 2) / units;
            f->descent = (pixel_em * desc_units + units / 2) / units;
        } else {
            const uint8_t *size = *(uint8_t *const *)((const uint8_t *)f->face + FT_FACE_SIZE);
            f->ascent = (int)((*(const long *)(size + FT_SIZE_ASCENDER) + 32) >> 6);
            f->descent = (int)((-*(const long *)(size + FT_SIZE_DESCENDER) + 32) >> 6);
        }
        if (family == GF_MS_SANS)                                 /* the bitmap font's metrics for this size */
            for (size_t k = 0; k < sizeof(g_ms_sans_sizes) / sizeof(*g_ms_sans_sizes); ++k)
                if (g_ms_sans_sizes[k][0] == pixel_em) {
                    f->ascent = g_ms_sans_sizes[k][2];
                    f->descent = g_ms_sans_sizes[k][1] - g_ms_sans_sizes[k][2];
                }
        f->height = f->ascent + f->descent;
        f->fixed = family == GF_MONO;
        const GdiGlyph *x = gf_glyph(f, 'x');
        f->avg_width = x ? x->advance : pixel_em / 2;
        const GdiGlyph *w = gf_glyph(f, 'W');
        f->max_width = w ? w->advance : pixel_em;
        if (f->max_width < f->avg_width) f->max_width = f->avg_width;
        return f;
    }
    /* No FreeType or font file: the overlay font closest in height. */
    const int h = cell ? cell : em + em / 4;
    const int index = h <= 15 ? 0 : h <= 22 ? 1 : 2;
    f->overlay = &overlay_fonts[index];
    f->height = overlay_fonts[index].cell_h;
    f->ascent = f->height - f->height / 5;
    f->descent = f->height / 5;
    f->avg_width = f->max_width = overlay_fonts[index].cell_w;
    f->fixed = 1;
    return f;
}

/* The font a GDI font object (or the default GUI font) draws with. Never NULL. */
static GdiFont *gf_for(uint64_t hfont)
{
    Obj *o = obj_get(hfont);
    int32_t height = -11, weight = 400;
    uint16_t face[32] = {0};
    int pitch_family = 0;
    if (o && o->type == OBJ_FONT) {
        memcpy(&height, o->logfont, 4);
        memcpy(&weight, o->logfont + 16, 4);
        pitch_family = o->logfont[27];
        memcpy(face, o->logfont + 28, 64);
        face[31] = 0;
    }
    if (height == 0) height = -11;                                       /* default: 8 pt at 96 dpi */
    const int family = face_family(face, pitch_family);
    const int bold = weight >= 600;
    if (family == GF_MS_SANS) {                                          /* snap to an existing bitmap size */
        const size_t sizes = sizeof(g_ms_sans_sizes) / sizeof(*g_ms_sans_sizes);
        size_t pick = 0;
        for (size_t k = 0; k < sizes; ++k)
            if (height < 0 ? g_ms_sans_sizes[k][0] <= -height : g_ms_sans_sizes[k][1] <= height) pick = k;
        return gf_open(family, bold, g_ms_sans_sizes[pick][0], 0);
    }
    return height < 0 ? gf_open(family, bold, -height, 0) : gf_open(family, bold, 0, height);
}
static int gf_underline(uint64_t hfont)
{
    Obj *o = obj_get(hfont);
    return o && o->type == OBJ_FONT && o->logfont[21];
}

/* A tab advances to the next stop, every 8 average character widths from the start (DrawText DT_EXPANDTABS). */
static int gf_tab_stop(const GdiFont *f, int position)
{
    const int stop = 8 * (f->avg_width > 0 ? f->avg_width : 6);
    return (position / stop + 1) * stop;
}
static int gf_width(GdiFont *f, const uint16_t *text, int count)
{
    int width = 0;
    for (int i = 0; i < count; ++i) {
        if (text[i] == '\t') { width = gf_tab_stop(f, width); continue; }
        if (!f->face) { width += f->avg_width; continue; }
        const GdiGlyph *g = gf_glyph(f, text[i]);
        width += g ? g->advance : f->avg_width;
    }
    return width;
}

/* Draws text with its cell's top-left corner at (x, y), blending `color` (0xAARRGGBB) by coverage. */
static void gf_draw(GdiFont *f, Surface *s, int x, int y, const uint16_t *text, int count, uint32_t color)
{
    const int x0 = x;
    if (!f->face) {
        for (int i = 0; i < count; ++i) {
            if (text[i] == '\t') { x = x0 + gf_tab_stop(f, x - x0); continue; }
            draw_glyph(s, x, y, f->overlay, text[i], color);
            x += f->overlay->cell_w;
        }
        return;
    }
    const int baseline = y + f->ascent;
    for (int i = 0; i < count; ++i) {
        if (text[i] == '\t') { x = x0 + gf_tab_stop(f, x - x0); continue; }
        const GdiGlyph *g = gf_glyph(f, text[i]);
        if (!g) continue;
        if (g->alpha) {
            const int gx = x + g->left, gy = baseline - g->top;
            for (int row = 0; row < g->h; ++row) {
                const int py = gy + row;
                if (py < 0 || py >= s->h) continue;
                for (int col = 0; col < g->w; ++col) {
                    const unsigned a = g->alpha[(size_t)row * g->w + col];
                    const int px = gx + col;
                    if (!a || px < 0 || px >= s->w) continue;
                    uint32_t out = 0xFF000000u;
                    if (a == 255) out = color | 0xFF000000u;
                    else {
                        const uint32_t d = s_get(s, px, py);
                        for (int sh = 0; sh <= 16; sh += 8) {
                            const unsigned dc = (d >> sh) & 0xff, sc = (color >> sh) & 0xff;
                            out |= (uint32_t)((dc * (255 - a) + sc * a) / 255) << sh;
                        }
                    }
                    s_put(s, px, py, out);
                }
            }
        }
        x += g->advance;
    }
}
