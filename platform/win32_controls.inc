/* Standard window controls as data: window text, LISTBOX / COMBOBOX item lists, BUTTON check state, PROGRESS range.
 *
 * Beer draws no controls, but programs that build their dialogs from them (Inno Setup's Delphi VCL wizard, MFC and
 * Win32 dialogs) read the state back: a list box must answer LB_GETCOUNT with the number of strings added, a window
 * must answer WM_GETTEXT with the text it was given. This file keeps that state per window, behind the same window
 * procedures Windows' own classes have, so the program sees a coherent control even though nothing is painted.
 * Included into loader.c ahead of impl_DefWindowProcW (which uses the window text helpers). */

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
    free(c->items); free(c->item_data); free(c->selected);
    c->items = NULL; c->item_data = NULL; c->selected = NULL; c->count = c->capacity = 0;
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
        c->capacity = cap;
    }
    if (index < 0 || (u32)index > c->count) index = (s32)c->count;
    u16 *copy = beer_ctl_wdup(text ? text : (const u16 *)u"");
    if (!copy) return BEER_LB_ERR;
    memmove(&c->items[index + 1], &c->items[index], (c->count - index) * sizeof(*c->items));
    memmove(&c->item_data[index + 1], &c->item_data[index], (c->count - index) * sizeof(*c->item_data));
    memmove(&c->selected[index + 1], &c->selected[index], c->count - index);
    c->items[index] = copy; c->item_data[index] = 0; c->selected[index] = 0;
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
    case 0x1A1: result = 16; break;                                                                     /* GETITEMHEIGHT */
    case 0x1A0: result = 0; break;                                                                      /* SETITEMHEIGHT */
    case 0x198: {                                                                                       /* GETITEMRECT */
        s32 *rect = (s32 *)(uintptr_t)lp;
        if (rect && wp < c->count) { rect[0] = 0; rect[1] = (s32)wp * 16; rect[2] = 100; rect[3] = (s32)wp * 16 + 16; result = 0; }
        else result = ERR;
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

static u64 __attribute__((ms_abi)) beer_ctl_listbox_proc(u64 hw, u32 msg, u64 wp, u64 lp)
{
    if (msg >= 0x180 && msg <= 0x1AA) return beer_ctl_list_message(hw, msg, wp, lp, 0);
    if (msg == 0x0082) beer_ctl_destroy(hw);
    return impl_DefWindowProcW(hw, msg, wp, lp);
}
static u64 __attribute__((ms_abi)) beer_ctl_combobox_proc(u64 hw, u32 msg, u64 wp, u64 lp)
{
    if (msg >= 0x140 && msg <= 0x162) return beer_ctl_list_message(hw, msg, wp, lp, 1);
    if (msg == 0x0082) beer_ctl_destroy(hw);
    return impl_DefWindowProcW(hw, msg, wp, lp);
}

/* BUTTON: check state; BM_CLICK toggles automatic check boxes and radio buttons. */
static u64 __attribute__((ms_abi)) beer_ctl_button_proc(u64 hw, u32 msg, u64 wp, u64 lp)
{
    u64 result = 0;
    switch (msg) {
    case 0x00F0:                                                  /* BM_GETCHECK */
        pthread_mutex_lock(&g_beer_ctl_mtx);
        { const BeerCtl *c = beer_ctl_get(hw, 0); result = c ? c->check : 0; }
        pthread_mutex_unlock(&g_beer_ctl_mtx);
        return result;
    case 0x00F1:                                                  /* BM_SETCHECK */
        pthread_mutex_lock(&g_beer_ctl_mtx);
        { BeerCtl *c = beer_ctl_get(hw, 1); if (c) c->check = (u32)wp & 3; }
        pthread_mutex_unlock(&g_beer_ctl_mtx);
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
    if (msg < 0x400 || msg > 0x420) {
        if (msg == 0x0082) beer_ctl_destroy(hw);
        return impl_DefWindowProcW(hw, msg, wp, lp);
    }
    u64 result = 0;
    pthread_mutex_lock(&g_beer_ctl_mtx);
    BeerCtl *c = beer_ctl_get(hw, 1);
    if (!c) { pthread_mutex_unlock(&g_beer_ctl_mtx); return 0; }
    switch (msg) {
    case 0x401: result = ((u64)(u32)c->range_max << 16) | (u16)c->range_min;               /* PBM_SETRANGE (previous) */
                c->range_min = (s32)(lp & 0xffff); c->range_max = (s32)((lp >> 16) & 0xffff); break;
    case 0x406: c->range_min = (s32)wp; c->range_max = (s32)lp; break;                      /* PBM_SETRANGE32 */
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
static u64 __attribute__((ms_abi)) beer_ctl_generic_proc(u64 hw, u32 msg, u64 wp, u64 lp)
{
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

/* The window procedure of a predefined class name (lower-case key); NULL when it is not one. */
static u64 beer_system_control_proc(const char *key)
{
    if (!strcmp(key, "listbox")) return (u64)(uintptr_t)beer_ctl_listbox_proc;
    if (!strcmp(key, "combobox")) return (u64)(uintptr_t)beer_ctl_combobox_proc;
    if (!strcmp(key, "button")) return (u64)(uintptr_t)beer_ctl_button_proc;
    if (!strcmp(key, "msctls_progress32")) return (u64)(uintptr_t)beer_ctl_progress_proc;
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
