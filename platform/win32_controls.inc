/* Standard window controls as data: window text, LISTBOX / COMBOBOX item lists, BUTTON check state, PROGRESS range.
 *
 * Beer draws no controls, but programs that build their dialogs from them (Inno Setup's Delphi VCL wizard, MFC and
 * Win32 dialogs) read the state back: a list box must answer LB_GETCOUNT with the number of strings added, a window
 * must answer WM_GETTEXT with the text it was given. This file keeps that state per window, behind the same window
 * procedures Windows' own classes have, so the program sees a coherent control even though nothing is painted.
 * Included into loader.c ahead of impl_DefWindowProcW (which uses the window text helpers). */

#define BEER_COMBO_BOX_H 21                      /* a drop-down combo box's height (its box) */
typedef struct {
    int used;
    u64 hwnd;
    u16 *text;                                   /* window text (WM_SETTEXT), NUL terminated */
    u32 style;                                   /* style the window was created with */
    s32 id;                                      /* control ID (CreateWindow's hMenu for a child) */
    /* LISTBOX / COMBOBOX */
    u16 **items; u64 *item_data; u8 *selected; u32 count, capacity;
    s32 cursel, top, caret;
    /* BUTTON */
    u32 check;
    /* PROGRESS */
    s32 range_min, range_max, position, step;
    int range_set;                        /* progress: a range was set (otherwise the default 0..100) */
    /* drawing */
    u64 font;                                    /* WM_SETFONT */
    int pressed;                                 /* a button held down with the mouse or the space bar */
    int select_all;                              /* EDIT: the whole text is selected (typing replaces it) */
    u64 dropdown;                                /* COMBOBOX: its open drop-down list window */
    s32 scroll;                                  /* multi-line EDIT: first visible line */
    u16 *heights;                                /* LISTBOX: per-item heights (owner-draw variable), 0 = unmeasured */
    s32 item_height;                             /* LISTBOX / COMBOBOX: the fixed item height, 0 = default */
    s32 dropped_h;                               /* COMBOBOX: the height asked for (the drop-down list's) */
    int is_combo;
} BeerCtl;
#define BEER_CTL_MAX 1024
static BeerCtl g_beer_ctl[BEER_CTL_MAX];
static pthread_mutex_t g_beer_ctl_mtx = PTHREAD_MUTEX_INITIALIZER;

static size_t beer_ctl_wlen(const u16 *s) { size_t n = 0; while (s && s[n]) ++n; return n; }
static u16 *beer_ctl_wdup(const u16 *s)
{
    const size_t n = beer_ctl_wlen(s);
    u16 *copy = malloc((n + 1) * sizeof(u16));
    if (copy) { if (n) memcpy(copy, s, n * sizeof(u16)); copy[n] = 0; }
    return copy;
}
static int beer_ctl_wcmp_ci(const u16 *a, const u16 *b, size_t limit)   /* limit 0 = whole strings */
{
    for (size_t i = 0; !limit || i < limit; ++i) {
        u16 x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
    return 0;
}

/* The control record of a window, created on first use. Caller holds g_beer_ctl_mtx. */
static BeerCtl *beer_ctl_get(u64 hwnd, int create)
{
    BeerCtl *free_slot = NULL;
    for (int i = 0; i < BEER_CTL_MAX; ++i) {
        if (g_beer_ctl[i].used && g_beer_ctl[i].hwnd == hwnd) return &g_beer_ctl[i];
        if (!g_beer_ctl[i].used && !free_slot) free_slot = &g_beer_ctl[i];
    }
    if (!create || !free_slot) return NULL;
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->used = 1; free_slot->hwnd = hwnd; free_slot->cursel = -1; free_slot->caret = -1;
    free_slot->range_max = 100; free_slot->step = 10;
    return free_slot;
}
static void beer_ctl_clear_items(BeerCtl *c)
{
    for (u32 i = 0; i < c->count; ++i) free(c->items[i]);
    free(c->items); free(c->item_data); free(c->selected); free(c->heights);
    c->items = NULL; c->item_data = NULL; c->selected = NULL; c->heights = NULL; c->count = c->capacity = 0;
    c->cursel = -1; c->top = 0; c->caret = -1;
}
static void beer_ctl_destroy(u64 hwnd)
{
    pthread_mutex_lock(&g_beer_ctl_mtx);
    BeerCtl *c = beer_ctl_get(hwnd, 0);
    if (c) { beer_ctl_clear_items(c); free(c->text); c->used = 0; }
    pthread_mutex_unlock(&g_beer_ctl_mtx);
}

/* ---- window text ---- */
static void beer_ctl_set_text(u64 hwnd, const u16 *text)
{
    u16 *copy = beer_ctl_wdup(text ? text : (const u16 *)u"");
    pthread_mutex_lock(&g_beer_ctl_mtx);
    BeerCtl *c = beer_ctl_get(hwnd, 1);
    if (c) { free(c->text); c->text = copy; copy = NULL; }
    pthread_mutex_unlock(&g_beer_ctl_mtx);
    free(copy);
}
/* Copies up to `capacity - 1` characters; returns the number copied (or the full length when buffer is NULL). */
static u64 beer_ctl_get_text(u64 hwnd, u16 *buffer, u64 capacity)
{
    u64 n = 0;
    pthread_mutex_lock(&g_beer_ctl_mtx);
    const BeerCtl *c = beer_ctl_get(hwnd, 0);
    const u16 *text = c && c->text ? c->text : (const u16 *)u"";
    n = beer_ctl_wlen(text);
    if (buffer) {
        if (!capacity) n = 0;
        else {
            if (n > capacity - 1) n = capacity - 1;
            if (n) memcpy(buffer, text, n * sizeof(u16));
            buffer[n] = 0;
        }
    }
    pthread_mutex_unlock(&g_beer_ctl_mtx);
    return n;
}

/* ---- list of strings shared by LISTBOX and COMBOBOX ---- */
#define BEER_LB_ERR (-1)
static int beer_ctl_insert(BeerCtl *c, s32 index, const u16 *text)
{
    if (c->count == c->capacity) {
        const u32 cap = c->capacity ? c->capacity * 2 : 16;
        u16 **items = realloc(c->items, cap * sizeof(*items));
        if (!items) return BEER_LB_ERR;
        c->items = items;
        u64 *data = realloc(c->item_data, cap * sizeof(*data));
        if (!data) return BEER_LB_ERR;
        c->item_data = data;
        u8 *sel = realloc(c->selected, cap);
        if (!sel) return BEER_LB_ERR;
        c->selected = sel;
        u16 *heights = realloc(c->heights, cap * sizeof(*heights));
        if (!heights) return BEER_LB_ERR;
        c->heights = heights;
        c->capacity = cap;
    }
    if (index < 0 || (u32)index > c->count) index = (s32)c->count;
    u16 *copy = beer_ctl_wdup(text ? text : (const u16 *)u"");
    if (!copy) return BEER_LB_ERR;
    memmove(&c->items[index + 1], &c->items[index], (c->count - index) * sizeof(*c->items));
    memmove(&c->item_data[index + 1], &c->item_data[index], (c->count - index) * sizeof(*c->item_data));
    memmove(&c->selected[index + 1], &c->selected[index], c->count - index);
    memmove(&c->heights[index + 1], &c->heights[index], (c->count - index) * sizeof(*c->heights));
    c->items[index] = copy; c->item_data[index] = 0; c->selected[index] = 0; c->heights[index] = 0;
    ++c->count;
    if (c->cursel >= index) ++c->cursel;
    return index;
}
static int beer_ctl_sorted_position(const BeerCtl *c, const u16 *text)
{
    u32 at = 0;
    while (at < c->count && beer_ctl_wcmp_ci(c->items[at], text, 0) <= 0) ++at;
    return (int)at;
}
static int beer_ctl_delete(BeerCtl *c, s32 index)
{
    if (index < 0 || (u32)index >= c->count) return BEER_LB_ERR;
    free(c->items[index]);
    memmove(&c->items[index], &c->items[index + 1], (c->count - index - 1) * sizeof(*c->items));
    memmove(&c->item_data[index], &c->item_data[index + 1], (c->count - index - 1) * sizeof(*c->item_data));
    memmove(&c->selected[index], &c->selected[index + 1], c->count - index - 1);
    memmove(&c->heights[index], &c->heights[index + 1], (c->count - index - 1) * sizeof(*c->heights));
    --c->count;
    if (c->cursel == index) c->cursel = -1; else if (c->cursel > index) --c->cursel;
    return (int)c->count;
}
static int beer_ctl_find(const BeerCtl *c, s32 after, const u16 *text, int exact)
{
    if (!text || !c->count) return BEER_LB_ERR;
    const size_t prefix = exact ? 0 : beer_ctl_wlen(text);
    for (u32 n = 0; n < c->count; ++n) {
        const u32 i = (u32)((after + 1 + (s32)n) % (s32)c->count);
        if (!beer_ctl_wcmp_ci(c->items[i], text, prefix)) return (int)i;
    }
    return BEER_LB_ERR;
}
static s32 beer_ctl_copy_item(const BeerCtl *c, s32 index, u16 *out)
{
    if (index < 0 || (u32)index >= c->count) return BEER_LB_ERR;
    const size_t n = beer_ctl_wlen(c->items[index]);
    if (out) memcpy(out, c->items[index], (n + 1) * sizeof(u16));
    return (s32)n;
}

/* An item's height: measured (owner-draw variable), the fixed height, or the default for the font. */
static s32 beer_ctl_item_h(const BeerCtl *c, s32 index)
{
    if (index >= 0 && (u32)index < c->count && c->heights && c->heights[index]) return c->heights[index];
    return c->item_height > 0 ? c->item_height : 16;
}
/* The item at a client y (from the top item), or -1. */
static s32 beer_ctl_item_at(const BeerCtl *c, s32 y)
{
    s32 top = 1;
    for (s32 i = c->top < 0 ? 0 : c->top; (u32)i < c->count; ++i) {
        const s32 h = beer_ctl_item_h(c, i);
        if (y >= top && y < top + h) return i;
        top += h;
    }
    return -1;
}
/* LISTBOX and COMBOBOX share one list: `base` maps the message numbers (LB_ADDSTRING = 0x180, CB_ADDSTRING = 0x143). */
static u64 beer_ctl_list_message(u64 hw, u32 msg, u64 wp, u64 lp, int combo)
{
    const u64 ERR = (u64)(s64)-1;
    u64 result = 0;
    int handled = 1;
    pthread_mutex_lock(&g_beer_ctl_mtx);
    BeerCtl *c = beer_ctl_get(hw, 1);
    if (!c) { pthread_mutex_unlock(&g_beer_ctl_mtx); return ERR; }
    /* Normalise combo-box messages onto the list-box numbers. */
    u32 m = msg;
    if (combo) {
        switch (msg) {
        case 0x143: m = 0x180; break; case 0x14A: m = 0x181; break; case 0x144: m = 0x182; break;
        case 0x14B: m = 0x184; break; case 0x14E: m = 0x186; break; case 0x147: m = 0x188; break;
        case 0x148: m = 0x189; break; case 0x149: m = 0x18A; break; case 0x146: m = 0x18B; break;
        case 0x14D: m = 0x18C; break; case 0x14C: m = 0x18F; break; case 0x158: m = 0x1A2; break;
        case 0x150: m = 0x199; break; case 0x151: m = 0x19A; break; case 0x15b: m = 0x18E; break;
        case 0x15c: m = 0x197; break; case 0x154: m = 0x1A1; break; case 0x153: m = 0x1A0; break;
        default: m = 0; break;                                    /* other CB_ messages handled below */
        }
    }
    switch (m) {
    case 0x180: {                                                 /* LB_ADDSTRING */
        const u16 *text = (const u16 *)(uintptr_t)lp;
        const int at = (c->style & 0x2) || (combo && (c->style & 0x100)) ? beer_ctl_sorted_position(c, text) : -1;
        result = (u64)(s64)beer_ctl_insert(c, at, text);
        break;
    }
    case 0x181: result = (u64)(s64)beer_ctl_insert(c, (s32)wp, (const u16 *)(uintptr_t)lp); break;   /* INSERTSTRING */
    case 0x182: result = (u64)(s64)beer_ctl_delete(c, (s32)wp); break;                                 /* DELETESTRING */
    case 0x184: beer_ctl_clear_items(c); break;                                                         /* RESETCONTENT */
    case 0x18B: result = c->count; break;                                                               /* GETCOUNT */
    case 0x188: result = (u64)(s64)c->cursel; break;                                                    /* GETCURSEL */
    case 0x186:                                                                                         /* SETCURSEL */
        if ((s32)wp < 0) { c->cursel = -1; result = ERR; }
        else if ((u32)wp >= c->count) result = ERR;
        else { c->cursel = (s32)wp; result = wp; }
        break;
    case 0x189: result = (u64)(s64)beer_ctl_copy_item(c, (s32)wp, (u16 *)(uintptr_t)lp); break;       /* GETTEXT */
    case 0x18A: result = (u64)(s64)beer_ctl_copy_item(c, (s32)wp, NULL); break;                         /* GETTEXTLEN */
    case 0x199: result = (wp < c->count) ? c->item_data[wp] : ERR; break;                               /* GETITEMDATA */
    case 0x19A:                                                                                         /* SETITEMDATA */
        if (wp < c->count) { c->item_data[wp] = lp; result = 1; } else result = ERR;
        break;
    case 0x18F: result = (u64)(s64)beer_ctl_find(c, (s32)wp, (const u16 *)(uintptr_t)lp, 0); break;   /* FINDSTRING */
    case 0x1A2: result = (u64)(s64)beer_ctl_find(c, (s32)wp, (const u16 *)(uintptr_t)lp, 1); break;   /* FINDSTRINGEXACT */
    case 0x18C: {                                                                                       /* SELECTSTRING */
        const int found = beer_ctl_find(c, (s32)wp, (const u16 *)(uintptr_t)lp, 0);
        if (found >= 0) c->cursel = found;
        result = (u64)(s64)found;
        break;
    }
    case 0x18E: result = (u64)(u32)c->top; break;                                                       /* GETTOPINDEX */
    case 0x197: c->top = (s32)wp; break;                                                                /* SETTOPINDEX */
    case 0x187: result = (wp < c->count && c->selected[wp]) ? 1 : (wp < c->count ? 0 : ERR); break;     /* GETSEL */
    case 0x185:                                                                                         /* SETSEL */
        if ((s32)lp < 0) for (u32 i = 0; i < c->count; ++i) c->selected[i] = wp != 0;
        else if ((u32)lp < c->count) c->selected[(u32)lp] = wp != 0; else result = ERR;
        break;
    case 0x190: for (u32 i = 0; i < c->count; ++i) result += c->selected[i]; break;                     /* GETSELCOUNT */
    case 0x19E: c->caret = (s32)wp; break;                                                              /* SETCARETINDEX */
    case 0x19F: result = (u64)(s64)c->caret; break;                                                     /* GETCARETINDEX */
    case 0x1A1:                                                                                         /* GETITEMHEIGHT */
        result = (u64)(u32)((s32)wp >= 0 && wp < c->count && c->heights && c->heights[wp] ? c->heights[wp] :
                            (c->item_height > 0 ? c->item_height : 16));
        break;
    case 0x1A0:                                                                                         /* SETITEMHEIGHT */
        if ((c->style & 0x20) && wp < c->count && c->heights) c->heights[wp] = (u16)(lp & 0xffff);
        else c->item_height = (s32)(lp & 0xffff);
        result = 0;
        break;
    case 0x198: {                                                                                       /* GETITEMRECT */
        s32 *rect = (s32 *)(uintptr_t)lp;
        BeerWindow *lw = beer_window_other(hw);
        if (rect && wp < c->count) {
            s32 y = 1;
            if ((s32)wp >= c->top) for (s32 i = c->top; i < (s32)wp; ++i) y += beer_ctl_item_h(c, i);
            else for (s32 i = (s32)wp; i < c->top; ++i) y -= beer_ctl_item_h(c, i);
            rect[0] = 1; rect[1] = y; rect[2] = lw ? lw->w - 1 - ((lw->style & 0x00200000u) ? 17 : 0) : 100;
            rect[3] = y + beer_ctl_item_h(c, (s32)wp);
            result = 0;
        } else result = ERR;
        break;
    }
    case 0x1A9: {                                                                                       /* ITEMFROMPOINT */
        const s32 y = (s16)((lp >> 16) & 0xffff);
        const s32 index = beer_ctl_item_at(c, y);
        result = index >= 0 ? (u64)(u32)index : ((u64)(c->count ? c->count - 1 : 0) | 0x10000u);
        break;
    }
    case 0x1A8: result = c->capacity; break;                                                            /* INITSTORAGE */
    case 0x1A7: case 0x192: case 0x194: case 0x195: case 0x19B: case 0x19C: case 0x19D: case 0x183:     /* layout/range */
        break;
    default: handled = 0; break;
    }
    if (!handled && combo) {
        handled = 1;
        switch (msg) {
        case 0x14F: case 0x155: break;                                                                  /* SHOWDROPDOWN, SETEXTENDEDUI */
        case 0x157: result = 0; break;                                                                  /* GETDROPPEDSTATE */
        case 0x141: result = 1; break;                                                                  /* LIMITTEXT */
        case 0x140: result = 0; break;                                                                  /* GETEDITSEL */
        case 0x142: result = 1; break;                                                                  /* SETEDITSEL */
        case 0x156: result = 0; break;                                                                  /* GETEXTENDEDUI */
        case 0x15f: case 0x160: result = 0; break;                                                      /* dropped width */
        default: handled = 0; break;
        }
    }
    pthread_mutex_unlock(&g_beer_ctl_mtx);
    if (!handled) return impl_DefWindowProcW(hw, msg, wp, lp);
    return result;
}

/* ---- RichEdit text (EM_STREAMIN): RTF reduced to its plain text ----
 * Paragraphs, line breaks and tabs are kept; formatting (fonts, colours, bold) is dropped, and destination groups
 * that hold no visible text (font / colour / style tables, document info, pictures, \* groups) are skipped. */
/* Windows-1252 bytes 0x80..0x9F (the rest of the code page is Latin-1). */
static const u16 g_cp1252_high[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D,
    0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A,
    0x0153, 0x009D, 0x017E, 0x0178 };
/* A character of a symbol font (Wingdings, Symbol, Webdings: \fcharset2) as the Unicode symbol it shows. */
static u16 beer_rtf_symbol(u8 code)
{
    switch (code) {
    case 0xFC: return 0x2713;           /* check mark */
    case 0xFB: case 0xFD: return 0x2717; /* ballot x */
    case 0xFE: return 0x2611;           /* checked box */
    case 0x6F: case 0xA8: return 0x25A1; /* white square */
    case 0x6E: case 0xA7: return 0x25AA; /* black square */
    case 0x6C: case 0x9F: case 0xB7: return 0x2022;   /* bullets */
    case 0x76: return 0x2756;
    case 0xD8: return 0x27A2;
    case 0xE0: case 0xE8: case 0xF0: return 0x2192;   /* arrows */
    case 0x4A: return 0x263A;           /* smiley */
    case 0x4C: return 0x2639;
    default: return code >= 0x20 && code < 0x7F ? code : 0x2022;
    }
}
static size_t beer_rtf_to_text(const u8 *rtf, size_t n, u16 *out, size_t capacity)
{
    size_t o = 0;
    int depth = 0, skip_depth = -1, uc = 1, pending_skip = 0;
    /* per group: the current font; the font table marks symbol fonts (\fcharset2, or a symbol font's name) */
    int font_at[64] = { 0 };
    u8 symbol_font[256] = { 0 };
    int in_fonttbl = -1, table_font = -1;
    char font_name[64];
    size_t font_name_len = 0;
#define RTF_EMIT(ch) do { if (pending_skip) --pending_skip; else out[o++] = (ch); } while (0)
    for (size_t i = 0; i < n && o + 3 < capacity; ) {
        const u8 c = rtf[i];
        const int font = depth >= 0 && depth < 64 ? font_at[depth] : 0;
        const int symbol = font >= 0 && font < 256 && symbol_font[font];
        if (c == '{') { ++depth; if (depth > 0 && depth < 64) font_at[depth] = font_at[depth - 1]; ++i; continue; }
        if (c == '}') {
            if (in_fonttbl >= 0 && table_font >= 0 && table_font < 256 && font_name_len) {   /* a font table entry */
                font_name[font_name_len] = 0;
                if (strstr(font_name, "Wingdings") || strstr(font_name, "Symbol") || strstr(font_name, "Webdings"))
                    symbol_font[table_font] = 1;
                font_name_len = 0;
            }
            if (depth == in_fonttbl) in_fonttbl = -1;
            if (depth == skip_depth) skip_depth = -1;
            --depth; ++i; continue;
        }
        if (c == '\r' || c == '\n') { ++i; continue; }
        if (c == '\\') {
            if (i + 1 >= n) break;
            const u8 d = rtf[i + 1];
            if (d == '\\' || d == '{' || d == '}') {
                if (skip_depth < 0) RTF_EMIT(d);
                i += 2; continue;
            }
            if (d == '\'') {                                          /* \'hh: a byte of the document code page */
                unsigned v = 0;
                for (size_t k = 0; k < 2 && i + 2 + k < n; ++k) {
                    const u8 h = rtf[i + 2 + k];
                    v = v * 16 + (h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 :
                                  h >= 'A' && h <= 'F' ? h - 'A' + 10 : 0);
                }
                if (skip_depth < 0)
                    RTF_EMIT(symbol ? beer_rtf_symbol((u8)v) : v >= 0x80 && v < 0xA0 ? g_cp1252_high[v - 0x80] : (u16)v);
                i += 4; continue;
            }
            if (d == '*') { if (skip_depth < 0) skip_depth = depth; i += 2; continue; }
            if (d == '~') { if (skip_depth < 0) RTF_EMIT(0xa0); i += 2; continue; }
            if (d == '-' || d == '_') { if (skip_depth < 0 && d == '_') RTF_EMIT('-'); i += 2; continue; }
            /* a control word: letters, an optional signed number, an optional delimiting space */
            size_t j = i + 1;
            char word[32];
            size_t w = 0;
            while (j < n && ((rtf[j] >= 'a' && rtf[j] <= 'z') || (rtf[j] >= 'A' && rtf[j] <= 'Z'))) {
                if (w + 1 < sizeof(word)) word[w++] = (char)rtf[j];
                ++j;
            }
            word[w] = 0;
            int has_number = 0, negative = 0;
            long number = 0;
            if (j < n && rtf[j] == '-') { negative = 1; ++j; }
            while (j < n && rtf[j] >= '0' && rtf[j] <= '9') { number = number * 10 + (rtf[j] - '0'); has_number = 1; ++j; }
            if (negative) number = -number;
            if (j < n && rtf[j] == ' ') ++j;
            i = j;
            if (!w) continue;
            if (!strcmp(word, "fonttbl")) { in_fonttbl = depth; continue; }
            if (in_fonttbl >= 0) {                                    /* inside the font table */
                if (!strcmp(word, "f") && has_number) { table_font = (int)number; font_name_len = 0; }
                else if (!strcmp(word, "fcharset") && number == 2 && table_font >= 0 && table_font < 256)
                    symbol_font[table_font] = 1;
                continue;
            }
            static const char *const destinations[] = { "colortbl", "stylesheet", "info", "pict", "object",
                                                        "header", "footer", "listtable", "listoverridetable",
                                                        "generator", "themedata", "colorschememapping", "latentstyles",
                                                        "datastore", "xmlnstbl", "rsidtbl", NULL };
            for (int k = 0; destinations[k]; ++k)
                if (!strcmp(word, destinations[k]) && skip_depth < 0) skip_depth = depth;
            if (skip_depth >= 0) continue;
            if (!strcmp(word, "f") && has_number) { if (depth >= 0 && depth < 64) font_at[depth] = (int)number; }
            else if (!strcmp(word, "par") || !strcmp(word, "line") || !strcmp(word, "sect") || !strcmp(word, "page")) {
                out[o++] = '\r'; out[o++] = '\n';
            } else if (!strcmp(word, "tab")) out[o++] = '\t';
            else if (!strcmp(word, "emdash")) out[o++] = 0x2014;
            else if (!strcmp(word, "endash")) out[o++] = 0x2013;
            else if (!strcmp(word, "bullet")) out[o++] = 0x2022;
            else if (!strcmp(word, "lquote")) out[o++] = 0x2018;
            else if (!strcmp(word, "rquote")) out[o++] = 0x2019;
            else if (!strcmp(word, "ldblquote")) out[o++] = 0x201c;
            else if (!strcmp(word, "rdblquote")) out[o++] = 0x201d;
            else if (!strcmp(word, "uc") && has_number) uc = (int)number;
            else if (!strcmp(word, "u") && has_number) {                 /* \uN then uc fallback characters */
                long v = number < 0 ? number + 65536 : number;
                if (symbol && v >= 0xF000 && v <= 0xF0FF) v = beer_rtf_symbol((u8)(v & 0xff));   /* symbol PUA */
                out[o++] = (u16)v;
                pending_skip = uc;
            }
            continue;
        }
        if (in_fonttbl >= 0) {                                        /* a font name; ';' ends it */
            if (c != ';' && font_name_len + 1 < sizeof(font_name)) font_name[font_name_len++] = (char)c;
            ++i; continue;
        }
        if (skip_depth < 0) RTF_EMIT(symbol ? beer_rtf_symbol(c) : c >= 0x80 && c < 0xA0 ? g_cp1252_high[c - 0x80] : c);
        ++i;
    }
#undef RTF_EMIT
    while (o && (out[o - 1] == '\n' || out[o - 1] == '\r')) --o;           /* the final \par */
    out[o] = 0;
    return o;
}
/* EM_STREAMIN data for a RichEdit / edit window: format SF_TEXT (1) or SF_RTF (2), SF_UNICODE (0x10) for UTF-16 text;
 * SFF_SELECTION (0x8000) appends. Returns the characters stored. */
static void beer_ctl_redraw(u64 hw);
static u64 beer_ctl_stream_in(u64 hw, const u8 *data, size_t n, u32 format)
{
    const size_t capacity = n + 16;
    u16 *text = malloc(capacity * sizeof(u16));
    if (!text) return 0;
    size_t length = 0;
    if ((format & 3) == 2 || (n >= 5 && !memcmp(data, "{\\rtf", 5))) length = beer_rtf_to_text(data, n, text, capacity);
    else if (format & 0x10) { length = n / 2; memcpy(text, data, length * 2); text[length] = 0; }
    else { for (size_t i = 0; i < n; ++i) text[i] = data[i]; length = n; text[length] = 0; }
    if (format & 0x8000) {                                                  /* replace the selection: append */
        const u64 old = beer_ctl_get_text(hw, NULL, 0);
        u16 *joined = malloc((old + length + 1) * sizeof(u16));
        if (joined) {
            beer_ctl_get_text(hw, joined, old + 1);
            memcpy(joined + old, text, length * sizeof(u16));
            joined[old + length] = 0;
            beer_ctl_set_text(hw, joined);
            free(joined);
        }
    } else beer_ctl_set_text(hw, text);
    if (getenv("BEER_CTL_DIAGNOSTICS")) {
        char preview[161];
        size_t k = 0;
        for (; k < length && k < 160; ++k) preview[k] = text[k] >= 32 && text[k] < 127 ? (char)text[k] : '.';
        preview[k] = 0;
        fprintf(stderr, "[CTL] EM_STREAMIN 0x%llx: %zu bytes, format 0x%x -> %zu chars: %s\n", (unsigned long long)hw, n,
                format, length, preview);
        for (size_t k = 0; k < length; ++k)
            if ((text[k] < 32 && text[k] != 13 && text[k] != 10) || text[k] >= 127)
                fprintf(stderr, "[CTL]   char U+%04X at %zu\n", text[k], k);
    }
    free(text);
    pthread_mutex_lock(&g_beer_ctl_mtx);
    { BeerCtl *c = beer_ctl_get(hw, 1); if (c) c->scroll = 0; }
    pthread_mutex_unlock(&g_beer_ctl_mtx);
    beer_ctl_redraw(hw);
    return length;
}

/* ---- drawing ----
 * Controls of Beer-drawn windows (dialogs) paint themselves on WM_PAINT into their window canvas, which the top-level
 * window composites (beer_other_present). Colours follow the Windows 10 classic theme; the parent's WM_CTLCOLOR*
 * answer supplies the background brush and text colour, as for real controls (Delphi forms set them there). */
static _Atomic(u64) g_mouse_capture_hwnd;
static u64 __attribute__((ms_abi)) impl_GetFocus(void);
static int beer_ctl_focused(u64 hw) { return impl_GetFocus() == hw; }
static void beer_ctl_fill(u64 dc, s32 l, s32 t, s32 r, s32 b, u32 color);
/* A dotted focus rectangle (DrawFocusRect). */
static void beer_ctl_focus_rect(u64 dc, s32 l, s32 t, s32 r, s32 b)
{
    for (s32 x = l; x < r; x += 2) { beer_ctl_fill(dc, x, t, x + 1, t + 1, 0); beer_ctl_fill(dc, x, b - 1, x + 1, b, 0); }
    for (s32 y = t; y < b; y += 2) { beer_ctl_fill(dc, l, y, l + 1, y + 1, 0); beer_ctl_fill(dc, r - 1, y, r, y + 1, 0); }
}
#define BEER_RGB(r, g, b) ((u32)(r) | ((u32)(g) << 8) | ((u32)(b) << 16))
static void beer_ctl_fill(u64 dc, s32 l, s32 t, s32 r, s32 b, u32 color)
{
    if (l >= r || t >= b) return;
    const int32_t rect[4] = { l, t, r, b };
    const u64 brush = beer_CreateSolidBrush(color);
    beer_FillRect(dc, rect, brush);
    beer_DeleteObject(brush);
}
static void beer_ctl_frame(u64 dc, s32 l, s32 t, s32 r, s32 b, u32 color)
{
    beer_ctl_fill(dc, l, t, r, t + 1, color); beer_ctl_fill(dc, l, b - 1, r, b, color);
    beer_ctl_fill(dc, l, t, l + 1, b, color); beer_ctl_fill(dc, r - 1, t, r, b, color);
}
/* Starts painting a control: its canvas DC with the control's font, the parent's colours applied and the background
 * filled (with the parent's WM_CTLCOLOR* brush when it returns one). NULL window when it is not Beer-drawn. */
static BeerWindow *beer_ctl_paint_begin(u64 hw, u32 ctlcolor, u32 fallback_bg, u64 *dc_out, BeerCtl *snapshot,
                                        u16 *text, size_t text_cap)
{
    BeerWindow *w = beer_window_other(hw);
    if (!w || w->w <= 0 || w->h <= 0) return NULL;
    const u64 dc = beer_other_paint_dc(w);
    if (!dc) return NULL;
    w->paint_posted = 0;
    memset(snapshot, 0, sizeof(*snapshot));
    pthread_mutex_lock(&g_beer_ctl_mtx);
    { const BeerCtl *c = beer_ctl_get(hw, 0); if (c) *snapshot = *c; }
    pthread_mutex_unlock(&g_beer_ctl_mtx);
    snapshot->items = NULL; snapshot->item_data = NULL; snapshot->selected = NULL; snapshot->text = NULL;
    if (text && text_cap) { text[0] = 0; beer_ctl_get_text(hw, text, text_cap); }
    snapshot->style = w->style;
    if (snapshot->font) beer_SelectObject(dc, snapshot->font);
    beer_SetTextColor(dc, (w->style & 0x08000000u) ? BEER_RGB(0x6d, 0x6d, 0x6d) : 0);
    beer_SetBkMode(dc, 1);                                                   /* TRANSPARENT */
    u64 brush = w->parent ? call_window_proc(w->parent, ctlcolor, dc, hw) : 0;
    if ((brush >> 32) == 0xffffffffu) brush = (u32)brush;        /* a 32-bit window procedure's result, sign-extended */
    if (getenv("BEER_CTL_DIAGNOSTICS"))
        fprintf(stderr, "[CTL] 0x%llx ctlcolor 0x%x -> brush 0x%llx\n", (unsigned long long)hw, ctlcolor,
                (unsigned long long)brush);
    const int32_t rect[4] = { 0, 0, w->w, w->h };
    if (brush && (brush >> 32) == 0) beer_FillRect(dc, rect, brush);
    else beer_ctl_fill(dc, 0, 0, w->w, w->h, fallback_bg);
    if (w->style & 0x08000000u) beer_SetTextColor(dc, BEER_RGB(0x6d, 0x6d, 0x6d));
    beer_SetBkMode(dc, 1);
    *dc_out = dc;
    return w;
}
static void beer_ctl_paint_end(BeerWindow *w) { beer_other_present(w); }

static void beer_ctl_paint_button(u64 hw)
{
    u64 dc = 0; BeerCtl c; u16 text[512];
    const u32 face = BEER_RGB(0xf0, 0xf0, 0xf0);
    BeerWindow *w = beer_ctl_paint_begin(hw, 0x0135, face, &dc, &c, text, 512);  /* WM_CTLCOLORBTN */
    if (!w) return;
    const u32 kind = w->style & 0xf;
    int32_t rect[4] = { 0, 0, w->w, w->h };
    if (kind == 0xb) { beer_ctl_paint_end(w); return; }                     /* BS_OWNERDRAW: the parent draws */
    if (kind == 7) {                                                         /* group box */
        beer_ctl_frame(dc, 0, 7, w->w, w->h, BEER_RGB(0xdc, 0xdc, 0xdc));
        int32_t label[4] = { 8, 0, w->w - 8, 16 };
        beer_DrawTextW(dc, text, -1, label, 0x0020 | 0x0400);                /* DT_SINGLELINE | DT_CALCRECT */
        beer_ctl_fill(dc, label[0] - 2, 0, label[2] + 2, label[3], face);
        beer_DrawTextW(dc, text, -1, label, 0x0020);
    } else if (kind == 2 || kind == 3 || kind == 5 || kind == 6 || kind == 4 || kind == 9) {
        const int radio = kind == 4 || kind == 9;
        const s32 box = 13, top = (w->h - box) / 2;
        const int left_text = (w->style & 0x0020) != 0;                      /* BS_LEFTTEXT */
        const s32 bx = left_text ? w->w - box : 0;
        beer_ctl_fill(dc, bx, top, bx + box, top + box, c.pressed ? BEER_RGB(0xcc, 0xe4, 0xf7) : BEER_RGB(0xff, 0xff, 0xff));
        beer_ctl_frame(dc, bx, top, bx + box, top + box, BEER_RGB(0x33, 0x33, 0x33));
        if (c.check == 1) {
            if (radio) beer_ctl_fill(dc, bx + 4, top + 4, bx + box - 4, top + box - 4, BEER_RGB(0x33, 0x33, 0x33));
            else for (int i = 0; i < 7; ++i) {                               /* a tick: two strokes */
                const s32 x = bx + 3 + i, y = i < 3 ? top + 6 + i : top + 8 - (i - 2);
                beer_ctl_fill(dc, x, y, x + 1, y + 2, BEER_RGB(0x33, 0x33, 0x33));
            }
        } else if (c.check == 2) beer_ctl_fill(dc, bx + 3, top + 3, bx + box - 3, top + box - 3, BEER_RGB(0x33, 0x33, 0x33));
        int32_t label[4] = { left_text ? 0 : box + 5, 0, left_text ? w->w - box - 5 : w->w, w->h };
        beer_DrawTextW(dc, text, -1, label, (w->style & 0x2000) ? 0x0010 : (0x0020 | 0x0004));  /* MULTILINE: WORDBREAK */
        if (beer_ctl_focused(hw) && text[0]) {
            int32_t extent[4] = { label[0], 0, label[2], w->h };
            beer_DrawTextW(dc, text, -1, extent, 0x0400 | ((w->style & 0x2000) ? 0x0010 : 0x0020));   /* CALCRECT */
            const s32 th = extent[3] - extent[1], ty = (w->style & 0x2000) ? 0 : (w->h - th) / 2;
            beer_ctl_focus_rect(dc, extent[0] - 1, ty, extent[2] + 1 < w->w ? extent[2] + 1 : w->w, ty + th);
        }
    } else {                                                                 /* push button */
        const int def = kind == 1;
        beer_ctl_fill(dc, 0, 0, w->w, w->h, c.pressed ? BEER_RGB(0xcc, 0xe4, 0xf7) : BEER_RGB(0xe1, 0xe1, 0xe1));
        beer_ctl_frame(dc, 0, 0, w->w, w->h, def || c.pressed ? BEER_RGB(0x00, 0x78, 0xd7) : BEER_RGB(0xad, 0xad, 0xad));
        if (def) beer_ctl_frame(dc, 1, 1, w->w - 1, w->h - 1, BEER_RGB(0x00, 0x78, 0xd7));
        beer_DrawTextW(dc, text, -1, rect, 0x0001 | 0x0004 | 0x0020);       /* DT_CENTER | DT_VCENTER | DT_SINGLELINE */
        if (beer_ctl_focused(hw)) beer_ctl_focus_rect(dc, 3, 3, w->w - 3, w->h - 3);
    }
    beer_ctl_paint_end(w);
}
static void beer_ctl_paint_static(u64 hw)
{
    u64 dc = 0; BeerCtl c; u16 text[2048];
    BeerWindow *w = beer_ctl_paint_begin(hw, 0x0138, BEER_RGB(0xf0, 0xf0, 0xf0), &dc, &c, text, 2048);   /* CTLCOLORSTATIC */
    if (!w) return;
    const u32 kind = w->style & 0x1f;
    int32_t rect[4] = { 0, 0, w->w, w->h };
    if (kind == 0x10) beer_ctl_fill(dc, 0, w->h / 2, w->w, w->h / 2 + 1, BEER_RGB(0xa0, 0xa0, 0xa0));       /* ETCHEDHORZ */
    else if (kind >= 0x04 && kind <= 0x09) {                                 /* rectangles and frames */
        const u32 color = kind == 4 || kind == 7 ? 0 : kind == 5 || kind == 8 ? BEER_RGB(0xa0, 0xa0, 0xa0) : BEER_RGB(0xff, 0xff, 0xff);
        if (kind <= 6) beer_ctl_fill(dc, 0, 0, w->w, w->h, color); else beer_ctl_frame(dc, 0, 0, w->w, w->h, color);
    } else if (kind <= 2 || kind == 0xb || kind == 0xc) {                    /* text */
        u32 format = kind == 1 ? 0x0001 : kind == 2 ? 0x0002 : 0;            /* DT_CENTER / DT_RIGHT */
        format |= kind == 0xc || kind == 0xb ? 0x0020 : 0x0010;              /* LEFTNOWORDWRAP/SIMPLE: one line */
        if (w->style & 0x0080) format |= 0x0800;                             /* SS_NOPREFIX */
        if (w->style & 0x0200) format |= 0x0004 | 0x0020;                    /* SS_CENTERIMAGE: vertically centred */
        beer_DrawTextW(dc, text, -1, rect, format);
    }
    beer_ctl_paint_end(w);
}
static void beer_ctl_paint_edit(u64 hw)
{
    u64 dc = 0; BeerCtl c; u16 text[4096];
    BeerWindow *w = beer_ctl_paint_begin(hw, 0x0133, BEER_RGB(0xff, 0xff, 0xff), &dc, &c, text, 4096);   /* CTLCOLOREDIT */
    if (!w) return;
    if (w->style & 0x0800) beer_ctl_fill(dc, 0, 0, w->w, w->h, BEER_RGB(0xf0, 0xf0, 0xf0));             /* ES_READONLY */
    beer_ctl_frame(dc, 0, 0, w->w, w->h, BEER_RGB(0x7a, 0x7a, 0x7a));
    if (w->style & 0x0020) for (size_t i = 0; text[i]; ++i) text[i] = 0x25cf;                            /* ES_PASSWORD */
    const int multi_line = (w->style & 0x0004) != 0;
    const int vscroll = multi_line && (w->style & 0x00200000u);              /* WS_VSCROLL: a scroll bar */
    int32_t rect[4] = { 4, 2, w->w - (vscroll ? 21 : 4), w->h - 2 };
    s32 line_h = 13, total_h = 0, first = 0;
    if (multi_line) {                                                         /* scrolled by whole lines */
        int32_t one[4] = { 0, 0, 100, 0 }, all[4] = { rect[0], 0, rect[2], 0 };
        const u16 probe[2] = { 'X', 0 };
        beer_DrawTextW(dc, probe, 1, one, 0x0400 | 0x0020);
        if (one[3] > 0) line_h = one[3];
        beer_DrawTextW(dc, text, -1, all, 0x0400 | 0x0010 | 0x0800);
        total_h = all[3];
        const s32 visible = (w->h - 4) / line_h, lines = (total_h + line_h - 1) / line_h;
        s32 max_first = lines - visible;
        if (max_first < 0) max_first = 0;
        first = c.scroll < 0 ? 0 : c.scroll > max_first ? max_first : c.scroll;
        if (first != c.scroll) {                                             /* scrolled past the end: clamp */
            pthread_mutex_lock(&g_beer_ctl_mtx);
            { BeerCtl *live = beer_ctl_get(hw, 0); if (live) live->scroll = first; }
            pthread_mutex_unlock(&g_beer_ctl_mtx);
        }
        rect[1] = 2 - first * line_h;
        if (vscroll) {
            const s32 track_x = w->w - 18, track_t = 1, track_b = w->h - 1;
            beer_ctl_fill(dc, track_x, track_t, w->w - 1, track_b, BEER_RGB(0xf0, 0xf0, 0xf0));
            if (lines > visible && lines > 0) {
                const s32 span = track_b - track_t, thumb = span * visible / lines < 12 ? 12 : span * visible / lines;
                const s32 top = track_t + (span - thumb) * first / (max_first ? max_first : 1);
                beer_ctl_fill(dc, track_x + 2, top, w->w - 3, top + thumb, BEER_RGB(0xc2, 0xc2, 0xc2));
            }
        }
    }
    const int multi = multi_line, focused = beer_ctl_focused(hw);
    const u32 format = multi ? 0x0010 | 0x0800 : 0x0020 | 0x0004 | 0x0800 |
                       ((w->style & 3) == 1 ? 0x0001 : (w->style & 3) == 2 ? 0x0002 : 0);
    int32_t extent[4] = { rect[0], rect[1], rect[2], rect[3] };
    beer_DrawTextW(dc, text, -1, extent, format | 0x0400);                  /* DT_CALCRECT: the text's extent */
    const s32 th = extent[3] - extent[1], ty = multi ? rect[1] : (w->h - th) / 2;
    if (focused && c.select_all && text[0]) {                                /* selected: white on blue */
        beer_ctl_fill(dc, extent[0], ty, extent[2] < rect[2] ? extent[2] : rect[2], ty + th, BEER_RGB(0x00, 0x78, 0xd7));
        beer_SetTextColor(dc, BEER_RGB(0xff, 0xff, 0xff));
    }
    beer_DrawTextW(dc, text, -1, rect, format);
    if (focused && !c.select_all && !(w->style & 0x0800) && !multi) {      /* caret after the text */
        const s32 cx = text[0] ? (extent[2] < rect[2] ? extent[2] : rect[2] - 1) : rect[0];
        beer_ctl_fill(dc, cx, ty, cx + 1, ty + (th > 0 ? th : w->h - 4), 0);
    }
    if (multi) {                                                             /* the frame over clipped text rows */
        beer_ctl_fill(dc, 1, 1, w->w - 1, 2, BEER_RGB(0xff, 0xff, 0xff));
        beer_ctl_frame(dc, 0, 0, w->w, w->h, BEER_RGB(0x82, 0x87, 0x90));
    }
    (void)total_h;
    beer_ctl_paint_end(w);
}
static void beer_ctl_owner_draw_list(u64 hw, BeerWindow *w, u64 dc);
static void beer_ctl_paint_list(u64 hw, int combo)
{
    u64 dc = 0; BeerCtl c; u16 text[1024];
    BeerWindow *w = beer_ctl_paint_begin(hw, combo ? 0x0134 : 0x0134, BEER_RGB(0xff, 0xff, 0xff), &dc, &c, NULL, 0);
    if (!w) return;
    const u32 border = BEER_RGB(0x7a, 0x7a, 0x7a), select = BEER_RGB(0x00, 0x78, 0xd7);
    if (combo) {
        const s32 h = w->h;                                                  /* the closed box */
        const int droplist = (w->style & 3) == 3;
        beer_ctl_fill(dc, 0, 0, w->w, h, droplist ? BEER_RGB(0xe5, 0xe5, 0xe5) : BEER_RGB(0xff, 0xff, 0xff));
        beer_ctl_frame(dc, 0, 0, w->w, h, BEER_RGB(0xad, 0xad, 0xad));
        text[0] = 0;
        pthread_mutex_lock(&g_beer_ctl_mtx);
        { const BeerCtl *live = beer_ctl_get(hw, 0); if (live && live->cursel >= 0) beer_ctl_copy_item(live, live->cursel, text); }
        pthread_mutex_unlock(&g_beer_ctl_mtx);
        if (!text[0]) beer_ctl_get_text(hw, text, 1024);
        int32_t rect[4] = { 4, 0, w->w - 18, h };
        if (beer_ctl_focused(hw)) {                                          /* focused: the selection highlighted */
            beer_ctl_fill(dc, 3, 3, w->w - 18, h - 3, BEER_RGB(0x00, 0x78, 0xd7));
            beer_SetTextColor(dc, BEER_RGB(0xff, 0xff, 0xff));
        }
        beer_DrawTextW(dc, text, -1, rect, 0x0020 | 0x0004 | 0x0800 | 0x8000);   /* single, vcenter, noprefix, ellipsis */
        beer_SetTextColor(dc, 0);
        const s32 ax = w->w - 13, ay = h / 2 - 2;                            /* the drop-down arrow */
        for (int i = 0; i < 4; ++i) beer_ctl_fill(dc, ax + i, ay + i, ax + 8 - i, ay + i + 1, BEER_RGB(0x60, 0x60, 0x60));
    } else if (w->style & 0x0030) {                                          /* LBS_OWNERDRAWFIXED / VARIABLE */
        beer_ctl_owner_draw_list(hw, w, dc);
        beer_ctl_frame(dc, 0, 0, w->w, w->h, BEER_RGB(0x82, 0x87, 0x90));
    } else {
        beer_ctl_frame(dc, 0, 0, w->w, w->h, border);
        const s32 item_h = 16;
        pthread_mutex_lock(&g_beer_ctl_mtx);
        const BeerCtl *live = beer_ctl_get(hw, 0);
        const u32 count = live ? live->count : 0;
        const s32 top = live ? live->top : 0;
        pthread_mutex_unlock(&g_beer_ctl_mtx);
        for (s32 i = top, y = 1; i < (s32)count && y < w->h - 1; ++i, y += item_h) {
            int selected = 0;
            text[0] = 0;
            pthread_mutex_lock(&g_beer_ctl_mtx);
            live = beer_ctl_get(hw, 0);
            if (live && (u32)i < live->count) {
                beer_ctl_copy_item(live, i, text);
                selected = live->cursel == i || (live->selected && live->selected[i]);
            }
            pthread_mutex_unlock(&g_beer_ctl_mtx);
            if (selected) beer_ctl_fill(dc, 1, y, w->w - 1, y + item_h, select);
            beer_SetTextColor(dc, selected ? BEER_RGB(0xff, 0xff, 0xff) : 0);
            int32_t rect[4] = { 4, y, w->w - 2, y + item_h };
            beer_DrawTextW(dc, text, -1, rect, 0x0020 | 0x0004 | 0x0800);
        }
    }
    beer_ctl_paint_end(w);
}
static void beer_ctl_paint_progress(u64 hw)
{
    u64 dc = 0; BeerCtl c;
    BeerWindow *w = beer_ctl_paint_begin(hw, 0x0138, BEER_RGB(0xe6, 0xe6, 0xe6), &dc, &c, NULL, 0);
    if (!w) return;
    beer_ctl_fill(dc, 0, 0, w->w, w->h, BEER_RGB(0xe6, 0xe6, 0xe6));
    if (!c.range_set) { c.range_min = 0; c.range_max = 100; }               /* the default range is 0..100 */
    if (c.range_min == 0 && c.range_max == 0) c.range_max = 100;            /* the default range is 0..100 */
    const s64 span = (s64)c.range_max - c.range_min;
    if (span > 0) {
        s64 filled = ((s64)c.position - c.range_min) * (w->w - 2) / span;
        if (filled < 0) filled = 0;
        if (filled > w->w - 2) filled = w->w - 2;
        beer_ctl_fill(dc, 1, 1, 1 + (s32)filled, w->h - 1, BEER_RGB(0x06, 0xb0, 0x25));
    }
    beer_ctl_paint_end(w);
}
/* WM_SETFONT / WM_GETFONT and redrawing after a state change; 1 when the message was handled. */
static int beer_ctl_common(u64 hw, u32 msg, u64 wp, u64 lp, u64 *result)
{
    if (msg == 0x0030) {                                          /* WM_SETFONT */
        pthread_mutex_lock(&g_beer_ctl_mtx);
        { BeerCtl *c = beer_ctl_get(hw, 1); if (c) c->font = wp; }
        pthread_mutex_unlock(&g_beer_ctl_mtx);
        if (lp & 0xffff) { BeerWindow *w = beer_window_other(hw); if (w) beer_other_post_paint(w); }
        *result = 0;
        return 1;
    }
    if (msg == 0x0031) {                                          /* WM_GETFONT */
        pthread_mutex_lock(&g_beer_ctl_mtx);
        { const BeerCtl *c = beer_ctl_get(hw, 0); *result = c ? c->font : 0; }
        pthread_mutex_unlock(&g_beer_ctl_mtx);
        return 1;
    }
    if (msg == 0x0014) { *result = 1; return 1; }                 /* WM_ERASEBKGND: WM_PAINT fills */
    if (msg == 0x0007 || msg == 0x0008) {                        /* WM_SETFOCUS / WM_KILLFOCUS: focus drawing */
        if (msg == 0x0008) {
            pthread_mutex_lock(&g_beer_ctl_mtx);
            { BeerCtl *c = beer_ctl_get(hw, 0); if (c) c->pressed = 0; }
            pthread_mutex_unlock(&g_beer_ctl_mtx);
        }
        BeerWindow *w = beer_window_other(hw);
        if (w) beer_other_post_paint(w);
        return 0;                                                 /* DefWindowProc too */
    }
    return 0;
}
/* Sends a notification code to the control's parent as WM_COMMAND (BN_CLICKED, EN_CHANGE, CBN_SELCHANGE, ...). */
static void beer_ctl_notify(u64 hw, u32 code)
{
    s32 id = 0;
    pthread_mutex_lock(&g_beer_ctl_mtx);
    { const BeerCtl *c = beer_ctl_get(hw, 0); if (c) id = c->id; }
    pthread_mutex_unlock(&g_beer_ctl_mtx);
    BeerWindow *w = beer_window_find(hw);
    if (w && w->parent) call_window_proc(w->parent, 0x0111, (u64)(u16)id | ((u64)(u16)code << 16), hw);
}
static void beer_ctl_redraw(u64 hw) { BeerWindow *w = beer_window_other(hw); if (w) beer_other_post_paint(w); }

static int beer_ctl_common(u64 hw, u32 msg, u64 wp, u64 lp, u64 *result);
static void beer_ctl_paint_list(u64 hw, int combo);
static void beer_ctl_redraw(u64 hw);
static void beer_ctl_notify(u64 hw, u32 code);
static u64 __attribute__((ms_abi)) beer_ctl_listbox_proc(u64 hw, u32 msg, u64 wp, u64 lp)
{
    u64 result = 0;
    if (beer_ctl_common(hw, msg, wp, lp, &result)) return result;
    if (msg == 0x000F) { beer_ctl_paint_list(hw, 0); return 0; }
    if (msg == 0x0087) return 0x0001;                             /* WM_GETDLGCODE: DLGC_WANTARROWS */
    if (msg == 0x020A) {                                          /* wheel: scroll by 3 items */
        pthread_mutex_lock(&g_beer_ctl_mtx);
        BeerCtl *c = beer_ctl_get(hw, 1);
        if (c) { c->top += (s16)(wp >> 16) > 0 ? -3 : 3; if (c->top > (s32)c->count - 1) c->top = (s32)c->count - 1;
                 if (c->top < 0) c->top = 0; }
        pthread_mutex_unlock(&g_beer_ctl_mtx);
        beer_ctl_redraw(hw);
        return 0;
    }
    if (msg == 0x0100 && (wp == 0x26 || wp == 0x28)) {            /* up / down: move the selection, LBN_SELCHANGE */
        int changed = 0;
        pthread_mutex_lock(&g_beer_ctl_mtx);
        BeerCtl *c = beer_ctl_get(hw, 1);
        if (c && c->count) {
            s32 next = c->cursel < 0 ? 0 : c->cursel + (wp == 0x26 ? -1 : 1);
            if (next < 0) next = 0;
            if (next >= (s32)c->count) next = (s32)c->count - 1;
            changed = next != c->cursel;
            c->cursel = next;
        }
        pthread_mutex_unlock(&g_beer_ctl_mtx);
        if (changed) { beer_ctl_redraw(hw); beer_ctl_notify(hw, 1); }
        return 0;
    }
    if (msg == 0x0201) {                                          /* WM_LBUTTONDOWN: select the item, LBN_SELCHANGE */
        const s32 y = (s16)((lp >> 16) & 0xffff);
        s32 index = -1, id = 0;
        pthread_mutex_lock(&g_beer_ctl_mtx);
        BeerCtl *c = beer_ctl_get(hw, 1);
        if (c) { index = beer_ctl_item_at(c, y); if (index >= 0) c->cursel = index; id = c->id; }
        pthread_mutex_unlock(&g_beer_ctl_mtx);
        if (index >= 0) {
            beer_ctl_redraw(hw);
            BeerWindow *w = beer_window_find(hw);
            if (w && w->parent) call_window_proc(w->parent, 0x0111, (u64)(u16)id | (1ull << 16), hw);
        }
        return 0;
    }
    if (msg >= 0x180 && msg <= 0x1AA) {
        const u64 r = beer_ctl_list_message(hw, msg, wp, lp, 0);
        if (msg == 0x0180 || msg == 0x0181 || msg == 0x0182 || msg == 0x0184 || msg == 0x0186) beer_ctl_redraw(hw);
        return r;
    }
    if (msg == 0x0082) beer_ctl_destroy(hw);
    return impl_DefWindowProcW(hw, msg, wp, lp);
}
/* ---- the drop-down list of a combo box: a popup window under it (userdata = the combo box) that captures the
 * mouse; a click on an item selects it (CBN_SELCHANGE), a click elsewhere closes it. ---- */
#define BEER_DROP_ITEM_H 17
static u64 __attribute__((ms_abi)) impl_DestroyWindow(u64 hw);
static void beer_ctl_dropdown_close(u64 combo)
{
    u64 popup = 0;
    pthread_mutex_lock(&g_beer_ctl_mtx);
    { BeerCtl *c = beer_ctl_get(combo, 0); if (c) { popup = c->dropdown; c->dropdown = 0; } }
    pthread_mutex_unlock(&g_beer_ctl_mtx);
    if (!popup) return;
    u64 expected = popup;
    atomic_compare_exchange_strong(&g_mouse_capture_hwnd, &expected, 0);
    impl_DestroyWindow(popup);
    beer_ctl_redraw(combo);
}
static u64 __attribute__((ms_abi)) beer_ctl_dropdown_proc(u64 hw, u32 msg, u64 wp, u64 lp)
{
    BeerWindow *w = beer_window_other(hw);
    const u64 combo = w ? w->userdata : 0;
    if (msg == 0x000F && w && combo) {                                       /* WM_PAINT: the items */
        const u64 dc = beer_other_paint_dc(w);
        w->paint_posted = 0;
        if (!dc) return 0;
        beer_ctl_fill(dc, 0, 0, w->w, w->h, BEER_RGB(0xff, 0xff, 0xff));
        beer_ctl_frame(dc, 0, 0, w->w, w->h, BEER_RGB(0x64, 0x64, 0x64));
        u64 font = 0;
        pthread_mutex_lock(&g_beer_ctl_mtx);
        { const BeerCtl *c = beer_ctl_get(combo, 0); if (c) font = c->font; }
        pthread_mutex_unlock(&g_beer_ctl_mtx);
        if (font) beer_SelectObject(dc, font);
        beer_SetBkMode(dc, 1);
        for (s32 i = 0, y = 1; y < w->h - 1; ++i, y += BEER_DROP_ITEM_H) {
            u16 text[512];
            int selected = 0, have = 0;
            text[0] = 0;
            pthread_mutex_lock(&g_beer_ctl_mtx);
            const BeerCtl *c = beer_ctl_get(combo, 0);
            const s32 index = c ? c->top + i : -1;
            if (c && index < (s32)c->count) { beer_ctl_copy_item(c, index, text); selected = c->caret == index; have = 1; }
            pthread_mutex_unlock(&g_beer_ctl_mtx);
            if (!have) break;
            if (selected) beer_ctl_fill(dc, 1, y, w->w - 1, y + BEER_DROP_ITEM_H, BEER_RGB(0x00, 0x78, 0xd7));
            beer_SetTextColor(dc, selected ? BEER_RGB(0xff, 0xff, 0xff) : 0);
            int32_t rect[4] = { 4, y, w->w - 2, y + BEER_DROP_ITEM_H };
            beer_DrawTextW(dc, text, -1, rect, 0x0020 | 0x0004 | 0x0800);
        }
        beer_other_present(w);
        return 0;
    }
    if (msg == 0x0014) return 1;
    if ((msg == 0x0200 || msg == 0x0201 || msg == 0x0202) && w && combo) {  /* hover / click */
        const s32 x = (s16)(lp & 0xffff), y = (s16)((lp >> 16) & 0xffff);
        const int inside = x >= 0 && y >= 0 && x < w->w && y < w->h;
        s32 index = -1, count = 0, top = 0;
        pthread_mutex_lock(&g_beer_ctl_mtx);
        BeerCtl *c = beer_ctl_get(combo, 0);
        if (c) { count = (s32)c->count; top = c->top; }
        if (c && inside) {
            index = top + (y - 1) / BEER_DROP_ITEM_H;
            if (index >= count) index = -1;
            if (index >= 0 && c->caret != index) { c->caret = index; if (msg == 0x0200) beer_other_post_paint(w); }
        }
        pthread_mutex_unlock(&g_beer_ctl_mtx);
        if (msg == 0x0201 && !inside) { beer_ctl_dropdown_close(combo); return 0; }
        if (msg == 0x0202 && inside && index >= 0) {
            beer_ctl_dropdown_close(combo);
            beer_ctl_list_message(combo, 0x014E, (u64)index, 0, 1);         /* CB_SETCURSEL */
            beer_ctl_redraw(combo);
            beer_ctl_notify(combo, 1);                                        /* CBN_SELCHANGE */
            beer_ctl_notify(combo, 9);                                        /* CBN_SELENDOK */
        }
        return 0;
    }
    return impl_DefWindowProcW(hw, msg, wp, lp);
}
static void beer_ctl_dropdown_open(u64 combo)
{
    BeerWindow *cw = beer_window_other(combo);
    if (!cw) return;
    s32 count = 0, cursel = -1;
    pthread_mutex_lock(&g_beer_ctl_mtx);
    BeerCtl *c = beer_ctl_get(combo, 1);
    if (c) { count = (s32)c->count; cursel = c->cursel; c->caret = cursel; c->top = 0; }
    pthread_mutex_unlock(&g_beer_ctl_mtx);
    if (count <= 0) return;
    s32 limit = 10;
    pthread_mutex_lock(&g_beer_ctl_mtx);
    { const BeerCtl *dc = beer_ctl_get(combo, 0); if (dc && dc->dropped_h > BEER_COMBO_BOX_H + BEER_DROP_ITEM_H)
          limit = (dc->dropped_h - BEER_COMBO_BOX_H) / BEER_DROP_ITEM_H; }
    pthread_mutex_unlock(&g_beer_ctl_mtx);
    if (limit < 1) limit = 1;
    const s32 shown = count < limit ? count : limit;
    s32 sx = 0, sy = 0;
    if (!beer_window_screen_origin(cw, &sx, &sy)) return;
    const s32 box_h = cw->h;
    pthread_mutex_lock(&g_beer_ctl_mtx);
    c = beer_ctl_get(combo, 1);
    if (c && cursel >= shown) c->top = cursel - shown + 1;
    pthread_mutex_unlock(&g_beer_ctl_mtx);
    const u64 popup = create_surfaceless_window(0x00000088u /* TOPMOST | TOOLWINDOW */, 0, 0,
                                                0x80000000u /* WS_POPUP */, sx, sy + box_h, cw->w,
                                                shown * BEER_DROP_ITEM_H + 2, 0, 0, 0, 0,
                                                (u64)(uintptr_t)beer_ctl_dropdown_proc);
    BeerWindow *pw = popup ? beer_window_other(popup) : NULL;
    if (!pw) return;
    pw->userdata = combo;
    pw->owner_tid = cw->owner_tid;
    pw->visible = 1;
    pthread_mutex_lock(&g_beer_ctl_mtx);
    c = beer_ctl_get(combo, 1);
    if (c) c->dropdown = popup;
    pthread_mutex_unlock(&g_beer_ctl_mtx);
    atomic_store(&g_mouse_capture_hwnd, popup);
    beer_other_post_paint(pw);
    beer_ctl_notify(combo, 7);                                                /* CBN_DROPDOWN */
}

static u64 __attribute__((ms_abi)) beer_ctl_combobox_proc(u64 hw, u32 msg, u64 wp, u64 lp)
{
    u64 result = 0;
    if (msg == 0x0081) {                                          /* WM_NCCREATE: the window is the box only */
        BeerWindow *w = beer_window_other(hw);
        pthread_mutex_lock(&g_beer_ctl_mtx);
        BeerCtl *c = beer_ctl_get(hw, 1);
        if (c) c->is_combo = 1;
        if (c && w && (w->style & 3) >= 2) { c->dropped_h = w->h; if (w->h > BEER_COMBO_BOX_H) w->h = BEER_COMBO_BOX_H; }
        pthread_mutex_unlock(&g_beer_ctl_mtx);
    }
    if (beer_ctl_common(hw, msg, wp, lp, &result)) return result;
    if (msg == 0x000F) { beer_ctl_paint_list(hw, 1); return 0; }
    if (msg == 0x0087) return 0x0001;                             /* WM_GETDLGCODE: DLGC_WANTARROWS */
    if (msg == 0x0201) {                                          /* WM_LBUTTONDOWN: open / close the list */
        u64 open = 0;
        pthread_mutex_lock(&g_beer_ctl_mtx);
        { const BeerCtl *c = beer_ctl_get(hw, 0); if (c) open = c->dropdown; }
        pthread_mutex_unlock(&g_beer_ctl_mtx);
        if (open) beer_ctl_dropdown_close(hw); else beer_ctl_dropdown_open(hw);
        return 0;
    }
    if (msg == 0x0008 || msg == 0x0082) beer_ctl_dropdown_close(hw); /* focus lost / destroyed: the list closes */
    if (msg == 0x014F) {                                          /* CB_SHOWDROPDOWN */
        if (wp) beer_ctl_dropdown_open(hw); else beer_ctl_dropdown_close(hw);
        return 1;
    }
    if (msg == 0x0157) {                                          /* CB_GETDROPPEDSTATE */
        u64 open = 0;
        pthread_mutex_lock(&g_beer_ctl_mtx);
        { const BeerCtl *c = beer_ctl_get(hw, 0); if (c) open = c->dropdown; }
        pthread_mutex_unlock(&g_beer_ctl_mtx);
        return open != 0;
    }
    if (msg == 0x0100 || msg == 0x020A) {                         /* arrows / Home / End / wheel: move the selection */
        s32 delta = 0, count = 0, cur = -1;
        pthread_mutex_lock(&g_beer_ctl_mtx);
        { const BeerCtl *c = beer_ctl_get(hw, 0); if (c) { count = (s32)c->count; cur = c->cursel; } }
        pthread_mutex_unlock(&g_beer_ctl_mtx);
        if (msg == 0x020A) delta = (s16)(wp >> 16) > 0 ? -1 : 1;
        else if (wp == 0x26 || wp == 0x25) delta = -1;
        else if (wp == 0x28 || wp == 0x27) delta = 1;
        else if (wp == 0x24) delta = -count;
        else if (wp == 0x23) delta = count;
        if (delta && count > 0) {
            s32 next = cur < 0 ? 0 : cur + delta;
            if (next < 0) next = 0;
            if (next >= count) next = count - 1;
            if (next != cur) {
                beer_ctl_list_message(hw, 0x014E, (u64)next, 0, 1);  /* CB_SETCURSEL */
                beer_ctl_redraw(hw);
                beer_ctl_notify(hw, 1);                               /* CBN_SELCHANGE */
            }
            return 0;
        }
    }
    if (msg >= 0x140 && msg <= 0x162) {
        const u64 r = beer_ctl_list_message(hw, msg, wp, lp, 1);
        if (msg == 0x014E || msg == 0x0143 || msg == 0x014A || msg == 0x0144 || msg == 0x014B) beer_ctl_redraw(hw);
        return r;
    }
    if (msg == 0x0082) beer_ctl_destroy(hw);
    return impl_DefWindowProcW(hw, msg, wp, lp);
}

/* BUTTON: check state; BM_CLICK toggles automatic check boxes and radio buttons. */
static u64 __attribute__((ms_abi)) beer_ctl_button_proc(u64 hw, u32 msg, u64 wp, u64 lp)
{
    u64 result = 0;
    if (beer_ctl_common(hw, msg, wp, lp, &result)) return result;
    switch (msg) {
    case 0x000F: beer_ctl_paint_button(hw); return 0;             /* WM_PAINT */
    case 0x0087: return 0x0001 | 0x0004 | (((beer_window_other(hw) ? beer_window_other(hw)->style : 0) & 0xf) <= 1 ?
                                             0x0010 : 0);           /* WM_GETDLGCODE: WANTARROWS|BUTTON|PUSHBUTTON */
    case 0x0100:                                                  /* WM_KEYDOWN: space presses */
        if (wp == 0x20) {
            pthread_mutex_lock(&g_beer_ctl_mtx);
            { BeerCtl *c = beer_ctl_get(hw, 1); if (c) c->pressed = 1; }
            pthread_mutex_unlock(&g_beer_ctl_mtx);
            beer_ctl_redraw(hw);
            return 0;
        }
        break;
    case 0x0101:                                                  /* WM_KEYUP: space clicks */
        if (wp == 0x20) {
            int was_pressed = 0;
            pthread_mutex_lock(&g_beer_ctl_mtx);
            { BeerCtl *c = beer_ctl_get(hw, 1); if (c) { was_pressed = c->pressed; c->pressed = 0; } }
            pthread_mutex_unlock(&g_beer_ctl_mtx);
            beer_ctl_redraw(hw);
            if (was_pressed) beer_ctl_button_proc(hw, 0x00F5, 0, 0);
            return 0;
        }
        break;
    case 0x0201: case 0x0203: {                                   /* WM_LBUTTONDOWN / DBLCLK: pressed, mouse captured */
        BeerWindow *w = beer_window_other(hw);
        if (!w || (w->style & 0x08000000u)) return 0;
        pthread_mutex_lock(&g_beer_ctl_mtx);
        { BeerCtl *c = beer_ctl_get(hw, 1); if (c) c->pressed = 1; }
        pthread_mutex_unlock(&g_beer_ctl_mtx);
        atomic_store(&g_mouse_capture_hwnd, hw);
        beer_ctl_redraw(hw);
        return 0;
    }
    case 0x0202: {                                                /* WM_LBUTTONUP: a click when released inside */
        int was_pressed = 0;
        pthread_mutex_lock(&g_beer_ctl_mtx);
        { BeerCtl *c = beer_ctl_get(hw, 1); if (c) { was_pressed = c->pressed; c->pressed = 0; } }
        pthread_mutex_unlock(&g_beer_ctl_mtx);
        u64 expected = hw;
        atomic_compare_exchange_strong(&g_mouse_capture_hwnd, &expected, 0);
        beer_ctl_redraw(hw);
        BeerWindow *w = beer_window_other(hw);
        const s32 x = (s16)(lp & 0xffff), y = (s16)((lp >> 16) & 0xffff);
        if (was_pressed && w && x >= 0 && y >= 0 && x < w->w && y < w->h) beer_ctl_button_proc(hw, 0x00F5, 0, 0);
        return 0;
    }
    case 0x00F0:                                                  /* BM_GETCHECK */
        pthread_mutex_lock(&g_beer_ctl_mtx);
        { const BeerCtl *c = beer_ctl_get(hw, 0); result = c ? c->check : 0; }
        pthread_mutex_unlock(&g_beer_ctl_mtx);
        return result;
    case 0x00F1:                                                  /* BM_SETCHECK */
        pthread_mutex_lock(&g_beer_ctl_mtx);
        { BeerCtl *c = beer_ctl_get(hw, 1); if (c) c->check = (u32)wp & 3; }
        pthread_mutex_unlock(&g_beer_ctl_mtx);
        beer_ctl_redraw(hw);
        return 0;
    case 0x00F2:                                                  /* BM_GETSTATE */
        pthread_mutex_lock(&g_beer_ctl_mtx);
        { const BeerCtl *c = beer_ctl_get(hw, 0); result = c ? c->check : 0; }
        pthread_mutex_unlock(&g_beer_ctl_mtx);
        return result;
    case 0x00F3: case 0x00F4: return 0;                           /* BM_SETSTATE, BM_SETSTYLE */
    case 0x00F5: {                                                /* BM_CLICK */
        u32 style = 0; s32 id = 0;
        pthread_mutex_lock(&g_beer_ctl_mtx);
        BeerCtl *c = beer_ctl_get(hw, 1);
        if (c) {
            style = c->style; id = c->id;
            const u32 kind = style & 0xf;
            if (kind == 2 || kind == 3) c->check = c->check ? 0 : 1;           /* check box */
            else if (kind == 6 || kind == 5) c->check = (c->check + 1) % 3;     /* three-state */
            else if (kind == 4 || kind == 9) c->check = 1;                      /* radio button */
        }
        pthread_mutex_unlock(&g_beer_ctl_mtx);
        beer_ctl_redraw(hw);
        BeerWindow *w = beer_window_find(hw);
        const u64 parent = w ? w->parent : 0;
        if (parent) call_window_proc(parent, 0x0111, (u64)(u16)id /* BN_CLICKED = 0 */, hw);
        return 0;
    }
    case 0x0082: beer_ctl_destroy(hw); break;
    default: break;
    }
    return impl_DefWindowProcW(hw, msg, wp, lp);
}

/* PROGRESS bar. */
static u64 __attribute__((ms_abi)) beer_ctl_progress_proc(u64 hw, u32 msg, u64 wp, u64 lp)
{
    {
        u64 handled = 0;
        if (beer_ctl_common(hw, msg, wp, lp, &handled)) return handled;
        if (msg == 0x000F) { beer_ctl_paint_progress(hw); return 0; }
        if (msg >= 0x401 && msg <= 0x406) {
            if (getenv("BEER_CTL_DIAGNOSTICS")) {
                static _Atomic(u32) logs;
                if (msg != 0x402 || atomic_fetch_add(&logs, 1) < 40)       /* every range change, the first positions */
                    fprintf(stderr, "[CTL] progress 0x%llx msg 0x%x wp 0x%llx lp 0x%llx\n", (unsigned long long)hw, msg,
                            (unsigned long long)wp, (unsigned long long)lp);
            }
            const u64 r = beer_ctl_progress_proc(hw, msg | 0x10000u, wp, lp);
            beer_ctl_redraw(hw);
            return r;
        }
        msg &= 0xffffu;
    }
    if (msg < 0x400 || msg > 0x420) {
        if (msg == 0x0082) beer_ctl_destroy(hw);
        return impl_DefWindowProcW(hw, msg, wp, lp);
    }
    u64 result = 0;
    pthread_mutex_lock(&g_beer_ctl_mtx);
    BeerCtl *c = beer_ctl_get(hw, 1);
    if (!c) { pthread_mutex_unlock(&g_beer_ctl_mtx); return 0; }
    if (!c->range_set && msg != 0x401 && msg != 0x406) { c->range_max = 100; c->range_set = 1; }   /* default 0..100 */
    if (!c->step) c->step = 10;                                       /* the default step */
    switch (msg) {
    case 0x401: result = ((u64)(u32)c->range_max << 16) | (u16)c->range_min;               /* PBM_SETRANGE (previous) */
                c->range_min = (s32)(lp & 0xffff); c->range_max = (s32)((lp >> 16) & 0xffff); c->range_set = 1; break;
    case 0x406: c->range_min = (s32)wp; c->range_max = (s32)lp; c->range_set = 1; break;                     /* PBM_SETRANGE32 */
    case 0x407: if (lp) { ((s32 *)(uintptr_t)lp)[0] = wp ? c->range_min : c->range_max; }    /* PBM_GETRANGE */
                result = wp ? c->range_min : c->range_max; break;
    case 0x402: result = (u32)c->position; c->position = (s32)wp; break;                     /* PBM_SETPOS */
    case 0x403: result = (u32)c->position; c->position += (s32)wp; break;                    /* PBM_DELTAPOS */
    case 0x404: result = (u32)c->step; c->step = (s32)wp; break;                             /* PBM_SETSTEP */
    case 0x405: result = (u32)c->position; c->position += c->step;                           /* PBM_STEPIT */
                if (c->position > c->range_max) c->position = c->range_min; break;
    case 0x408: result = (u32)c->position; break;                                            /* PBM_GETPOS */
    default: break;
    }
    pthread_mutex_unlock(&g_beer_ctl_mtx);
    return result;
}

/* STATIC, EDIT and the rest: window text plus the few edit messages programs poll. */
static u64 __attribute__((ms_abi)) beer_ctl_generic_proc_kind(u64 hw, u32 msg, u64 wp, u64 lp, int edit)
{
    u64 handled = 0;
    if (beer_ctl_common(hw, msg, wp, lp, &handled)) return handled;
    if (msg == 0x000F) { if (edit) beer_ctl_paint_edit(hw); else beer_ctl_paint_static(hw); return 0; }
    if (edit && msg == 0x0087) return 0x0080 | 0x0100 | 0x0001;  /* WM_GETDLGCODE: WANTCHARS | HASSETSEL | WANTARROWS */
    if (edit && msg == 0x0449 && lp) {                            /* EM_STREAMIN (64-bit EDITSTREAM) */
        typedef u32 (__attribute__((ms_abi)) *StreamCallback)(u64 cookie, u8 *buffer, s32 bytes, s32 *done);
        const u8 *stream = (const u8 *)(uintptr_t)lp;
        u64 cookie = 0, callback = 0;
        memcpy(&cookie, stream, 8); memcpy(&callback, stream + 16, 8);
        size_t used = 0, cap = 0;
        u8 *data = NULL;
        u32 error = 0;
        for (int guard = 0; callback && guard < 65536; ++guard) {
            if (cap - used < 4096) { u8 *grown = realloc(data, cap + 65536); if (!grown) break; data = grown; cap += 65536; }
            s32 got = 0;
            error = ((StreamCallback)callback)(cookie, data + used, 4096, &got);
            if (error || got <= 0) break;
            used += (size_t)got;
        }
        memcpy((u8 *)(uintptr_t)lp + 8, &error, 4);
        const u64 r = data ? beer_ctl_stream_in(hw, data, used, (u32)wp) : 0;
        free(data);
        return r;
    }
    if (edit && (msg == 0x020A || msg == 0x0115 || (msg == 0x0100 && (wp == 0x21 || wp == 0x22 || wp == 0x26 || wp == 0x28)))) {
        BeerWindow *w = beer_window_other(hw);                    /* wheel / WM_VSCROLL / keys: scroll the lines */
        if (w && (w->style & 0x0004)) {
            s32 delta = 0;
            if (msg == 0x020A) delta = (s16)(wp >> 16) > 0 ? -3 : 3;
            else if (msg == 0x0115) delta = (wp & 0xffff) == 0 ? -1 : (wp & 0xffff) == 1 ? 1 : (wp & 0xffff) == 2 ? -8 :
                                             (wp & 0xffff) == 3 ? 8 : 0;
            else delta = wp == 0x21 ? -8 : wp == 0x22 ? 8 : wp == 0x26 ? -1 : 1;
            pthread_mutex_lock(&g_beer_ctl_mtx);
            { BeerCtl *c = beer_ctl_get(hw, 1); if (c) { c->scroll += delta; if (c->scroll < 0) c->scroll = 0; } }
            pthread_mutex_unlock(&g_beer_ctl_mtx);
            beer_ctl_redraw(hw);
            return 0;
        }
    }
    if (edit && msg == 0x0201) {                                  /* a click on the scroll bar pages */
        BeerWindow *w = beer_window_other(hw);
        const s32 x = (s16)(lp & 0xffff), y = (s16)((lp >> 16) & 0xffff);
        if (w && (w->style & 0x0004) && (w->style & 0x00200000u) && x >= w->w - 18) {
            pthread_mutex_lock(&g_beer_ctl_mtx);
            { BeerCtl *c = beer_ctl_get(hw, 1); if (c) { c->scroll += y < w->h / 2 ? -8 : 8; if (c->scroll < 0) c->scroll = 0; } }
            pthread_mutex_unlock(&g_beer_ctl_mtx);
            beer_ctl_redraw(hw);
            return 0;
        }
    }
    if (edit && msg == 0x00B1) {                                  /* EM_SETSEL: (0, -1) selects all, (-1, ..) none */
        pthread_mutex_lock(&g_beer_ctl_mtx);
        { BeerCtl *c = beer_ctl_get(hw, 1); if (c) c->select_all = (s32)wp == 0 && (s32)lp == -1; }
        pthread_mutex_unlock(&g_beer_ctl_mtx);
        beer_ctl_redraw(hw);
        return 1;
    }
    if (edit && msg == 0x0102) {                                  /* WM_CHAR: typing (caret at the end) */
        BeerWindow *w = beer_window_other(hw);
        if (!w || (w->style & 0x0800) || (w->style & 0x08000000u)) return 0;   /* read-only / disabled */
        const u16 ch = (u16)wp;
        if (ch != 8 && ch < 32 && !(ch == 13 && (w->style & 0x0004))) return 0;
        int replace = 0;
        pthread_mutex_lock(&g_beer_ctl_mtx);
        { BeerCtl *c = beer_ctl_get(hw, 1); if (c) { replace = c->select_all; c->select_all = 0; } }
        pthread_mutex_unlock(&g_beer_ctl_mtx);
        const u64 old = replace ? 0 : beer_ctl_get_text(hw, NULL, 0);
        u16 *joined = malloc((old + 3) * sizeof(u16));
        if (!joined) return 0;
        joined[0] = 0;
        if (old) beer_ctl_get_text(hw, joined, old + 1);
        u64 n = old;
        if (ch == 8) { if (n) --n; }
        else if (ch == 13) { joined[n++] = 13; joined[n++] = 10; }
        else joined[n++] = ch;
        joined[n] = 0;
        beer_ctl_set_text(hw, joined);
        free(joined);
        beer_ctl_redraw(hw);
        beer_ctl_notify(hw, 0x0300);                              /* EN_CHANGE */
        return 0;
    }
    if (edit && msg == 0x0007) {                                  /* WM_SETFOCUS: tabbing in selects the text */
        pthread_mutex_lock(&g_beer_ctl_mtx);
        { BeerCtl *c = beer_ctl_get(hw, 1); if (c) c->select_all = 1; }
        pthread_mutex_unlock(&g_beer_ctl_mtx);
    }
    if (msg == 0x000C) {                                          /* WM_SETTEXT: shown at once */
        const u64 r = impl_DefWindowProcW(hw, msg, wp, lp);
        if (edit) {                                               /* a new text: caret at the end, EN_UPDATE/EN_CHANGE */
            pthread_mutex_lock(&g_beer_ctl_mtx);
            { BeerCtl *c = beer_ctl_get(hw, 1); if (c) c->select_all = 0; }
            pthread_mutex_unlock(&g_beer_ctl_mtx);
        }
        beer_ctl_redraw(hw);
        if (edit) { beer_ctl_notify(hw, 0x0400); beer_ctl_notify(hw, 0x0300); }
        return r;
    }
    switch (msg) {
    case 0x00B0: return 0;                                        /* EM_GETSEL */
    case 0x00B1: return 1;                                        /* EM_SETSEL */
    case 0x00C5: case 0x00CF: case 0x00D3: case 0x00D4: return 1; /* EM_LIMITTEXT, SETREADONLY, SETMARGINS, GETMARGINS */
    case 0x00BA: {                                                /* EM_GETLINECOUNT */
        u16 *text = NULL; u64 n = beer_ctl_get_text(hw, NULL, 0), lines = 1;
        text = malloc((n + 1) * sizeof(u16));
        if (text) { beer_ctl_get_text(hw, text, n + 1); for (u64 i = 0; i < n; ++i) if (text[i] == '\n') ++lines; free(text); }
        return lines;
    }
    case 0x00C2: {                                                /* EM_REPLACESEL: appended (selection is not tracked) */
        const u16 *add = (const u16 *)(uintptr_t)lp;
        const u64 old = beer_ctl_get_text(hw, NULL, 0), extra = beer_ctl_wlen(add);
        u16 *joined = malloc((old + extra + 1) * sizeof(u16));
        if (!joined) return 0;
        beer_ctl_get_text(hw, joined, old + 1);
        if (extra) memcpy(joined + old, add, extra * sizeof(u16));
        joined[old + extra] = 0;
        beer_ctl_set_text(hw, joined);
        free(joined);
        return 0;
    }
    case 0x0082: beer_ctl_destroy(hw); break;
    default: break;
    }
    return impl_DefWindowProcW(hw, msg, wp, lp);
}
static u64 __attribute__((ms_abi)) beer_ctl_generic_proc(u64 hw, u32 msg, u64 wp, u64 lp)
    { return beer_ctl_generic_proc_kind(hw, msg, wp, lp, 0); }
static u64 __attribute__((ms_abi)) beer_ctl_edit_proc(u64 hw, u32 msg, u64 wp, u64 lp)
    { return beer_ctl_generic_proc_kind(hw, msg, wp, lp, 1); }

static u64 __attribute__((ms_abi)) beer_ctl_treeview_proc(u64 hw, u32 msg, u64 wp, u64 lp);
/* The window procedure of a predefined class name (lower-case key); NULL when it is not one. */
static u64 beer_system_control_proc(const char *key)
{
    if (!strcmp(key, "listbox")) return (u64)(uintptr_t)beer_ctl_listbox_proc;
    if (!strcmp(key, "combobox")) return (u64)(uintptr_t)beer_ctl_combobox_proc;
    if (!strcmp(key, "button")) return (u64)(uintptr_t)beer_ctl_button_proc;
    if (!strcmp(key, "msctls_progress32")) return (u64)(uintptr_t)beer_ctl_progress_proc;
    if (!strcmp(key, "edit") || !strncmp(key, "richedit", 8)) return (u64)(uintptr_t)beer_ctl_edit_proc;
    if (!strcmp(key, "systreeview32")) return (u64)(uintptr_t)beer_ctl_treeview_proc;
    return (u64)(uintptr_t)beer_ctl_generic_proc;
}

/* Remembers what a new window was created with (WM_NCCREATE): its text, style and control ID. */
static void beer_ctl_on_create(u64 hw, const WIN_CREATESTRUCT *cs)
{
    if (!cs) return;
    if (cs->lpszName > 0xffffu) beer_ctl_set_text(hw, (const u16 *)(uintptr_t)cs->lpszName);
    pthread_mutex_lock(&g_beer_ctl_mtx);
    BeerCtl *c = beer_ctl_get(hw, 1);
    if (c) { c->style = (u32)cs->style; c->id = (s32)(u32)cs->hMenu; }
    pthread_mutex_unlock(&g_beer_ctl_mtx);
}

static u64 __attribute__((ms_abi)) impl_SetFocus(u64 hw);
#include "win32_treeview.inc"
#include "win32_ownerdraw.inc"
