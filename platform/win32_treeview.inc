/* SysTreeView32 (comctl32 tree view) for Beer-drawn windows: items, expand / collapse, selection, scrolling, mouse and
 * keyboard, drawn with lines, +/- buttons and a folder glyph; notifications to the parent through WM_NOTIFY
 * (TVN_SELCHANGING/ED, TVN_ITEMEXPANDING/ED, TVN_GETDISPINFO, TVN_DELETEITEM, NM_CLICK/DBLCLK/RETURN).
 * Inno Setup's "Browse For Folder" form fills one with the drives and folders and expands it on demand.
 *
 * Structures follow the program's bitness: a 32-bit program (WoW32) sends and receives the 32-bit TVITEM,
 * TVINSERTSTRUCT and NMHDR layouts, and the notifications Beer sends it live in its low memory.
 * Included at the end of win32_controls.inc. */

static u32 g_w32_image;                                  /* nonzero: the program is 32-bit (wow32.inc) */
static void *w32_heap_alloc(size_t size, int zero);
static void w32_heap_free(void *block);
static int tv_guest32(void) { return g_w32_image != 0; }
static void *tv_guest_alloc(size_t size)
{
    return tv_guest32() ? w32_heap_alloc(size, 1) : calloc(1, size);
}
static void tv_guest_free(void *p)
{
    if (!p) return;
    if (tv_guest32()) w32_heap_free(p); else free(p);
}

#define TV_MAX_ITEMS 16384
#define TV_ITEM_BASE 0x00A00000u                         /* item handles: small values (also valid 32-bit) */
#define TV_ROOT ((u64)(s64)(s32)0xFFFF0000)
#define TV_FIRST ((u64)(s64)(s32)0xFFFF0001)
#define TV_LAST ((u64)(s64)(s32)0xFFFF0002)
#define TV_SORT ((u64)(s64)(s32)0xFFFF0003)
#define TV_TEXTCALLBACK ((u64)(s64)-1)
typedef struct {
    int used;
    u64 tree;                                            /* owning window */
    u32 parent, first_child, next, prev;                 /* item indexes + 1, 0 = none */
    u16 *text;                                           /* NULL with text_callback */
    int text_callback;
    s32 children;                                        /* cChildren: -1 = I_CHILDRENCALLBACK, -2 = by the list */
    u64 lparam;
    s32 image, selected_image;
    u32 state;                                           /* TVIS_* */
} TvItem;
typedef struct {
    int used;
    u64 hwnd;
    u32 first_root;
    u32 selected;
    s32 scroll;                                          /* first visible row */
    u64 image_list, font;
    s32 indent, item_height;
    int focused;
} TvTree;
static TvItem g_tv_items[TV_MAX_ITEMS];
static TvTree g_tv_trees[64];
static pthread_mutex_t g_tv_mtx = PTHREAD_MUTEX_INITIALIZER;
static u32 g_tv_hint;

static TvTree *tv_tree(u64 hwnd, int create)
{
    for (int i = 0; i < 64; ++i) if (g_tv_trees[i].used && g_tv_trees[i].hwnd == hwnd) return &g_tv_trees[i];
    if (!create) return NULL;
    for (int i = 0; i < 64; ++i)
        if (!g_tv_trees[i].used) {
            memset(&g_tv_trees[i], 0, sizeof(g_tv_trees[i]));
            g_tv_trees[i].used = 1; g_tv_trees[i].hwnd = hwnd; g_tv_trees[i].indent = 19;
            return &g_tv_trees[i];
        }
    return NULL;
}
static TvItem *tv_item(u32 index) { return index && index <= TV_MAX_ITEMS && g_tv_items[index - 1].used ? &g_tv_items[index - 1] : NULL; }
static u32 tv_index(u64 handle)
{
    const u32 h = (u32)handle;
    if (h <= TV_ITEM_BASE || h > TV_ITEM_BASE + TV_MAX_ITEMS) return 0;
    return tv_item(h - TV_ITEM_BASE) ? h - TV_ITEM_BASE : 0;
}
static u64 tv_handle(u32 index) { return index ? (u64)(TV_ITEM_BASE + index) : 0; }
static u32 tv_alloc(void)
{
    for (u32 n = 0; n < TV_MAX_ITEMS; ++n) {
        const u32 i = (g_tv_hint + n) % TV_MAX_ITEMS;
        if (!g_tv_items[i].used) { memset(&g_tv_items[i], 0, sizeof(g_tv_items[i])); g_tv_items[i].used = 1; g_tv_hint = i + 1; return i + 1; }
    }
    return 0;
}
static u32 tv_first_child(TvTree *t, u32 parent) { return parent ? tv_item(parent)->first_child : t->first_root; }

/* ---- TVITEM in the program's layout ---- */
typedef struct { u32 mask; u64 item; u32 state, state_mask; u64 text; s32 text_max, image, selected_image, children; u64 lparam; } TvItemFields;
static void tv_read_item(const u8 *p, TvItemFields *f)
{
    memset(f, 0, sizeof(*f));
    if (tv_guest32()) {
        const u32 *v = (const u32 *)p;
        f->mask = v[0]; f->item = v[1]; f->state = v[2]; f->state_mask = v[3]; f->text = v[4] == 0xffffffffu ? TV_TEXTCALLBACK : v[4];
        f->text_max = (s32)v[5]; f->image = (s32)v[6]; f->selected_image = (s32)v[7]; f->children = (s32)v[8]; f->lparam = v[9];
    } else {
        memcpy(&f->mask, p, 4); memcpy(&f->item, p + 8, 8); memcpy(&f->state, p + 16, 4); memcpy(&f->state_mask, p + 20, 4);
        memcpy(&f->text, p + 24, 8); memcpy(&f->text_max, p + 32, 4); memcpy(&f->image, p + 36, 4);
        memcpy(&f->selected_image, p + 40, 4); memcpy(&f->children, p + 44, 4); memcpy(&f->lparam, p + 48, 8);
    }
}
static void tv_write_item(u8 *p, const TvItemFields *f)
{
    if (tv_guest32()) {
        u32 *v = (u32 *)p;
        v[0] = f->mask; v[1] = (u32)f->item; v[2] = f->state; v[3] = f->state_mask; v[4] = (u32)f->text;
        v[5] = (u32)f->text_max; v[6] = (u32)f->image; v[7] = (u32)f->selected_image; v[8] = (u32)f->children; v[9] = (u32)f->lparam;
    } else {
        memcpy(p, &f->mask, 4); memcpy(p + 8, &f->item, 8); memcpy(p + 16, &f->state, 4); memcpy(p + 20, &f->state_mask, 4);
        memcpy(p + 24, &f->text, 8); memcpy(p + 32, &f->text_max, 4); memcpy(p + 36, &f->image, 4);
        memcpy(p + 40, &f->selected_image, 4); memcpy(p + 44, &f->children, 4); memcpy(p + 48, &f->lparam, 8);
    }
}
static size_t tv_item_size(void) { return tv_guest32() ? 40 : 56; }
static size_t tv_hdr_size(void) { return tv_guest32() ? 12 : 24; }
static void tv_write_hdr(u8 *p, u64 hwnd, u32 code)
{
    s32 id = 0;
    pthread_mutex_lock(&g_beer_ctl_mtx);
    { const BeerCtl *c = beer_ctl_get(hwnd, 0); if (c) id = c->id; }
    pthread_mutex_unlock(&g_beer_ctl_mtx);
    if (tv_guest32()) { u32 *v = (u32 *)p; v[0] = (u32)hwnd; v[1] = (u32)id; v[2] = code; }
    else { const u64 from = hwnd, idv = (u64)(s64)id; memcpy(p, &from, 8); memcpy(p + 8, &idv, 8); memcpy(p + 16, &code, 4); }
}
static u64 tv_send_notify(u64 hwnd, u8 *nm)
{
    BeerWindow *w = beer_window_find(hwnd);
    if (!w || !w->parent) return 0;
    s32 id = 0;
    pthread_mutex_lock(&g_beer_ctl_mtx);
    { const BeerCtl *c = beer_ctl_get(hwnd, 0); if (c) id = c->id; }
    pthread_mutex_unlock(&g_beer_ctl_mtx);
    return call_window_proc(w->parent, 0x004E, (u64)(s64)id, (u64)(uintptr_t)nm);      /* WM_NOTIFY */
}
/* A notification carrying one item (NMTREEVIEW itemNew, or NMTVDISPINFO / NMTREEVIEW for deletion via itemOld). */
static void tv_fields_of(u32 index, TvItemFields *f)
{
    TvItem *it = tv_item(index);
    memset(f, 0, sizeof(*f));
    f->mask = 0x0001 | 0x0004 | 0x0008 | 0x0010 | 0x0040;     /* TEXT? no: HANDLE | STATE | PARAM | ... */
    f->mask = 0x0010 | 0x0008 | 0x0004;                      /* TVIF_HANDLE | TVIF_STATE | TVIF_PARAM */
    f->item = tv_handle(index);
    if (it) { f->state = it->state; f->state_mask = 0xffff; f->lparam = it->lparam; f->image = it->image;
              f->selected_image = it->selected_image; f->children = it->children; }
}
/* NMTREEVIEW: hdr, action, itemOld, itemNew, ptDrag. Returns the parent's answer. */
static u64 tv_notify_tree(u64 hwnd, s32 code, u32 action, u32 old_index, u32 new_index)
{
    const size_t hdr = tv_hdr_size(), item = tv_item_size(), action_off = hdr, old_off = tv_guest32() ? hdr + 4 : hdr + 8;
    u8 *nm = tv_guest_alloc(old_off + 2 * item + 8);
    if (!nm) return 0;
    tv_write_hdr(nm, hwnd, (u32)code);
    memcpy(nm + action_off, &action, 4);
    TvItemFields f;
    pthread_mutex_lock(&g_tv_mtx);
    tv_fields_of(old_index, &f); tv_write_item(nm + old_off, &f);
    tv_fields_of(new_index, &f); tv_write_item(nm + old_off + item, &f);
    pthread_mutex_unlock(&g_tv_mtx);
    const u64 r = tv_send_notify(hwnd, nm);
    tv_guest_free(nm);
    return r;
}
/* TVN_GETDISPINFOW for the item's text and/or children count. */
static void tv_dispinfo(u64 hwnd, u32 index, u32 mask, u16 *text_out, size_t text_cap, s32 *children_out)
{
    const size_t hdr = tv_hdr_size();
    u8 *nm = tv_guest_alloc(hdr + tv_item_size() + 520 * 2 + 16);
    if (!nm) return;
    u16 *buffer = (u16 *)(nm + hdr + tv_item_size() + 8);
    tv_write_hdr(nm, hwnd, (u32)-452);                                  /* TVN_GETDISPINFOW */
    TvItemFields f;
    pthread_mutex_lock(&g_tv_mtx);
    tv_fields_of(index, &f);
    pthread_mutex_unlock(&g_tv_mtx);
    f.mask = mask | 0x0010 | 0x0004;
    f.text = (u64)(uintptr_t)buffer; f.text_max = 520;
    tv_write_item(nm + hdr, &f);
    tv_send_notify(hwnd, nm);
    TvItemFields r;
    tv_read_item(nm + hdr, &r);
    if (getenv("BEER_CTL_DIAGNOSTICS")) {
        char name[64] = {0};
        const u16 *src = r.text && r.text != TV_TEXTCALLBACK ? (const u16 *)(uintptr_t)r.text : buffer;
        for (int k = 0; src && src[k] && k < 63; ++k) name[k] = src[k] < 128 ? (char)src[k] : '?';
        fprintf(stderr, "[TV] dispinfo item %u mask 0x%x -> mask 0x%x text=0x%llx (buffer %p) '%s' children=%d\n", index,
                mask, r.mask, (unsigned long long)r.text, (void *)buffer, name, r.children);
    }
    if ((mask & 1) && text_out) {
        const u16 *src = r.text && r.text != TV_TEXTCALLBACK ? (const u16 *)(uintptr_t)r.text : buffer;
        size_t k = 0;
        for (; src && src[k] && k + 1 < text_cap; ++k) text_out[k] = src[k];
        text_out[k] = 0;
    }
    if ((mask & 0x40) && children_out) *children_out = r.children;
    if (r.mask & 0x1000) {                                               /* TVIF_DI_SETITEM: remember it */
        pthread_mutex_lock(&g_tv_mtx);
        TvItem *it = tv_item(index);
        if (it && (mask & 0x40)) it->children = r.children;
        if (it && (mask & 1) && text_out) { free(it->text); it->text = beer_ctl_wdup(text_out); it->text_callback = 0; }
        pthread_mutex_unlock(&g_tv_mtx);
    }
    tv_guest_free(nm);
}
static int tv_has_children(u64 hwnd, u32 index)
{
    pthread_mutex_lock(&g_tv_mtx);
    TvItem *it = tv_item(index);
    const int listed = it && it->first_child, children = it ? it->children : 0;
    pthread_mutex_unlock(&g_tv_mtx);
    if (listed) return 1;
    if (children == -1) { s32 c = 0; tv_dispinfo(hwnd, index, 0x40, NULL, 0, &c); return c != 0; }
    return children > 0;
}

/* ---- the visible rows: expanded items in order ---- */
static u32 tv_next_visible(TvTree *t, u32 index)
{
    TvItem *it = tv_item(index);
    if (!it) return 0;
    if ((it->state & 0x20) && it->first_child) return it->first_child;          /* TVIS_EXPANDED */
    for (u32 cur = index; cur; cur = tv_item(cur)->parent)
        if (tv_item(cur)->next) return tv_item(cur)->next;
    (void)t;
    return 0;
}
static u32 tv_prev_visible(TvTree *t, u32 index)
{
    TvItem *it = tv_item(index);
    if (!it) return 0;
    if (!it->prev) return it->parent;
    u32 cur = it->prev;
    for (;;) {
        TvItem *c = tv_item(cur);
        if (!(c->state & 0x20) || !c->first_child) return cur;
        u32 last = c->first_child;
        while (tv_item(last)->next) last = tv_item(last)->next;
        cur = last;
    }
    (void)t;
}
static int tv_depth(u32 index) { int d = 0; for (TvItem *it = tv_item(index); it && it->parent; it = tv_item(it->parent)) ++d; return d; }
static s32 tv_row_of(TvTree *t, u32 index)
{
    s32 row = 0;
    for (u32 cur = t->first_root; cur; cur = tv_next_visible(t, cur), ++row) if (cur == index) return row;
    return -1;
}
static u32 tv_item_at_row(TvTree *t, s32 row)
{
    u32 cur = t->first_root;
    for (s32 r = 0; cur && r < row; ++r) cur = tv_next_visible(t, cur);
    return row >= 0 ? cur : 0;
}
static s32 tv_visible_rows(TvTree *t) { s32 n = 0; for (u32 cur = t->first_root; cur; cur = tv_next_visible(t, cur)) ++n; return n; }

static void tv_unlink(TvTree *t, u32 index)
{
    TvItem *it = tv_item(index);
    if (it->prev) tv_item(it->prev)->next = it->next;
    else if (it->parent) tv_item(it->parent)->first_child = it->next;
    else t->first_root = it->next;
    if (it->next) tv_item(it->next)->prev = it->prev;
    it->next = it->prev = 0;
}
/* Deletes an item and its descendants (TVN_DELETEITEM for each, children first). Called without g_tv_mtx. */
static void tv_delete(u64 hwnd, u32 index)
{
    pthread_mutex_lock(&g_tv_mtx);
    TvItem *it = tv_item(index);
    u32 child = it ? it->first_child : 0;
    pthread_mutex_unlock(&g_tv_mtx);
    while (child) {
        pthread_mutex_lock(&g_tv_mtx);
        const u32 next = tv_item(child) ? tv_item(child)->next : 0;
        pthread_mutex_unlock(&g_tv_mtx);
        tv_delete(hwnd, child);
        child = next;
    }
    tv_notify_tree(hwnd, -458, 0, index, 0);                                  /* TVN_DELETEITEMW (itemOld) */
    pthread_mutex_lock(&g_tv_mtx);
    TvTree *t = tv_tree(hwnd, 0);
    it = tv_item(index);
    if (t && it) {
        tv_unlink(t, index);
        if (t->selected == index) t->selected = 0;
        free(it->text);
        memset(it, 0, sizeof(*it));
    }
    pthread_mutex_unlock(&g_tv_mtx);
}
static int tv_compare(const u16 *a, const u16 *b) { return beer_ctl_wcmp_ci(a ? a : (const u16 *)u"", b ? b : (const u16 *)u"", 0); }
static u64 tv_insert(u64 hwnd, const u8 *insert)
{
    u64 parent_handle, after;
    TvItemFields f;
    if (tv_guest32()) {
        const u32 *v = (const u32 *)insert;
        parent_handle = (u64)(s64)(s32)v[0]; after = (u64)(s64)(s32)v[1];
        tv_read_item(insert + 8, &f);
    } else {
        memcpy(&parent_handle, insert, 8); memcpy(&after, insert + 8, 8);
        tv_read_item(insert + 16, &f);
    }
    if (getenv("BEER_CTL_DIAGNOSTICS")) {
        char name[64] = {0};
        const u16 *src = f.text && f.text != TV_TEXTCALLBACK ? (const u16 *)(uintptr_t)f.text : NULL;
        for (int k = 0; src && src[k] && k < 63; ++k) name[k] = src[k] < 128 ? (char)src[k] : '?';
        fprintf(stderr, "[TV] insert parent=0x%llx after=0x%llx mask=0x%x text=0x%llx '%s' children=%d\n",
                (unsigned long long)parent_handle, (unsigned long long)after, f.mask, (unsigned long long)f.text, name,
                f.children);
    }
    pthread_mutex_lock(&g_tv_mtx);
    TvTree *t = tv_tree(hwnd, 1);
    const u32 parent = parent_handle == TV_ROOT || !parent_handle ? 0 : tv_index(parent_handle);
    const u32 index = t && (parent || parent_handle == TV_ROOT || !parent_handle) ? tv_alloc() : 0;
    if (!index) { pthread_mutex_unlock(&g_tv_mtx); return 0; }
    TvItem *it = tv_item(index);
    it->tree = hwnd; it->parent = parent;
    if (f.mask & 0x0001) {                                                      /* TVIF_TEXT */
        if (f.text == TV_TEXTCALLBACK) it->text_callback = 1;
        else it->text = beer_ctl_wdup((const u16 *)(uintptr_t)f.text);
    }
    it->children = (f.mask & 0x0040) ? f.children : -2;                         /* TVIF_CHILDREN */
    if (f.mask & 0x0004) it->lparam = f.lparam;
    if (f.mask & 0x0002) it->image = f.image;
    if (f.mask & 0x0020) it->selected_image = f.selected_image;
    if (f.mask & 0x0008) it->state = f.state & f.state_mask;
    /* place it among its siblings */
    u32 first = tv_first_child(t, parent), prev = 0;
    if (after == TV_FIRST) prev = 0;
    else if (after == TV_SORT) {
        for (u32 cur = first; cur && tv_compare(tv_item(cur)->text, it->text) <= 0; cur = tv_item(cur)->next) prev = cur;
    } else if (after != TV_LAST && tv_index(after)) prev = tv_index(after);
    else for (u32 cur = first; cur; cur = tv_item(cur)->next) prev = cur;        /* TVI_LAST */
    if (prev && prev != index) {
        it->prev = prev; it->next = tv_item(prev)->next;
        if (it->next) tv_item(it->next)->prev = index;
        tv_item(prev)->next = index;
    } else {
        it->next = first;
        if (first) tv_item(first)->prev = index;
        if (parent) tv_item(parent)->first_child = index; else t->first_root = index;
    }
    pthread_mutex_unlock(&g_tv_mtx);
    beer_ctl_redraw(hwnd);
    return tv_handle(index);
}
/* Expands or collapses (TVE_COLLAPSE 1, TVE_EXPAND 2, TVE_TOGGLE 3), with TVN_ITEMEXPANDING (vetoable) / ED. */
static int tv_expand(u64 hwnd, u32 index, u32 how)
{
    pthread_mutex_lock(&g_tv_mtx);
    TvItem *it = tv_item(index);
    const int expanded = it && (it->state & 0x20);
    pthread_mutex_unlock(&g_tv_mtx);
    if (!it) return 0;
    u32 action = how & 3;
    if (action == 3) action = expanded ? 1 : 2;
    if ((action == 2) == expanded && !(how & 0x8000)) return 1;                 /* already so */
    if (tv_notify_tree(hwnd, -454, action, 0, index)) return 0;                 /* TVN_ITEMEXPANDINGW: vetoed */
    pthread_mutex_lock(&g_tv_mtx);
    it = tv_item(index);
    if (it) {
        if (action == 2) it->state |= 0x20 | 0x40; else it->state &= ~0x20u;    /* EXPANDED, EXPANDEDONCE */
        TvTree *t = tv_tree(hwnd, 0);
        if (action == 1 && t && t->selected) {                                  /* a collapsed branch: select its root */
            for (u32 cur = tv_item(t->selected) ? tv_item(t->selected)->parent : 0; cur; cur = tv_item(cur)->parent)
                if (cur == index) { t->selected = index; break; }
        }
    }
    pthread_mutex_unlock(&g_tv_mtx);
    if (how & 0x8000) {                                                         /* TVE_COLLAPSERESET: drop children */
        u32 child;
        do {
            pthread_mutex_lock(&g_tv_mtx);
            child = tv_item(index) ? tv_item(index)->first_child : 0;
            pthread_mutex_unlock(&g_tv_mtx);
            if (child) tv_delete(hwnd, child);
        } while (child);
        pthread_mutex_lock(&g_tv_mtx);
        if (tv_item(index)) tv_item(index)->state &= ~0x40u;
        pthread_mutex_unlock(&g_tv_mtx);
    }
    tv_notify_tree(hwnd, -455, action, 0, index);                               /* TVN_ITEMEXPANDEDW */
    beer_ctl_redraw(hwnd);
    return 1;
}
static void tv_ensure_visible(u64 hwnd, u32 index);
/* Selects an item (TVN_SELCHANGINGW may veto; TVN_SELCHANGEDW after). action: TVC_UNKNOWN 0, BYMOUSE 1, BYKEYBOARD 2. */
static int tv_select(u64 hwnd, u32 index, u32 action)
{
    pthread_mutex_lock(&g_tv_mtx);
    TvTree *t = tv_tree(hwnd, 1);
    const u32 old = t ? t->selected : 0;
    pthread_mutex_unlock(&g_tv_mtx);
    if (!t || old == index) { if (index) tv_ensure_visible(hwnd, index); return 1; }
    if (tv_notify_tree(hwnd, -450, action, old, index)) return 0;               /* TVN_SELCHANGINGW */
    pthread_mutex_lock(&g_tv_mtx);
    if (tv_item(old)) tv_item(old)->state &= ~0x2u;                             /* TVIS_SELECTED */
    if (tv_item(index)) tv_item(index)->state |= 0x2;
    t->selected = index;
    pthread_mutex_unlock(&g_tv_mtx);
    if (index) tv_ensure_visible(hwnd, index);
    tv_notify_tree(hwnd, -451, action, old, index);                             /* TVN_SELCHANGEDW */
    beer_ctl_redraw(hwnd);
    return 1;
}
static s32 tv_row_height(TvTree *t) { return t->item_height > 0 ? t->item_height : 17; }
static s32 tv_page_rows(u64 hwnd, TvTree *t)
{
    BeerWindow *w = beer_window_other(hwnd);
    const s32 rows = w ? (w->h - 4) / tv_row_height(t) : 10;
    return rows > 0 ? rows : 1;
}
static void tv_ensure_visible(u64 hwnd, u32 index)
{
    pthread_mutex_lock(&g_tv_mtx);
    TvTree *t = tv_tree(hwnd, 0);
    if (t) {
        for (u32 cur = tv_item(index) ? tv_item(index)->parent : 0; cur; cur = tv_item(cur)->parent)
            tv_item(cur)->state |= 0x20;                                        /* its ancestors are expanded */
        const s32 row = tv_row_of(t, index), page = tv_page_rows(hwnd, t);
        if (row >= 0 && row < t->scroll) t->scroll = row;
        else if (row >= t->scroll + page) t->scroll = row - page + 1;
    }
    pthread_mutex_unlock(&g_tv_mtx);
}

/* ---- drawing ---- */
static void tv_paint(u64 hw)
{
    u64 dc = 0; BeerCtl c;
    BeerWindow *w = beer_ctl_paint_begin(hw, 0x0138, BEER_RGB(0xff, 0xff, 0xff), &dc, &c, NULL, 0);
    if (!w) return;
    beer_ctl_fill(dc, 0, 0, w->w, w->h, BEER_RGB(0xff, 0xff, 0xff));
    pthread_mutex_lock(&g_tv_mtx);
    TvTree *t = tv_tree(hw, 1);
    if (!t) { pthread_mutex_unlock(&g_tv_mtx); beer_ctl_paint_end(w); return; }
    if (t->font) beer_SelectObject(dc, t->font); else if (c.font) beer_SelectObject(dc, c.font);
    if (getenv("BEER_CTL_DIAGNOSTICS")) {
        static int logged;
        if (logged++ < 3) {
            int32_t probe[4] = { 0, 0, 500, 0 };
            const u16 sample[4] = { 'A', 'b', 'c', 0 };
            beer_DrawTextW(dc, sample, -1, probe, 0x0400 | 0x0020);
            fprintf(stderr, "[TV] paint font t=0x%llx ctl=0x%llx: 'Abc' -> %dx%d\n", (unsigned long long)t->font,
                    (unsigned long long)c.font, probe[2], probe[3]);
        }
    }
    const s32 row_h = tv_row_height(t), indent = t->indent > 0 ? t->indent : 19, page = tv_page_rows(hw, t);
    const s32 total = tv_visible_rows(t);
    if (t->scroll > total - page) t->scroll = total - page;
    if (t->scroll < 0) t->scroll = 0;
    const int has_scrollbar = total > page;
    const s32 right = w->w - (has_scrollbar ? 19 : 2);
    const int buttons = (w->style & 0x0001) != 0, lines = (w->style & 0x0002) != 0, at_root = (w->style & 0x0004) != 0;
    const int focused = beer_ctl_focused(hw);
    u32 rows[256];
    s32 count = 0;
    for (u32 cur = tv_item_at_row(t, t->scroll); cur && count < page + 1 && count < 256; cur = tv_next_visible(t, cur))
        rows[count++] = cur;
    const u32 selected = t->selected;
    pthread_mutex_unlock(&g_tv_mtx);
    beer_SetBkMode(dc, 1);
    for (s32 r = 0; r < count; ++r) {
        const u32 index = rows[r];
        pthread_mutex_lock(&g_tv_mtx);
        TvItem *it = tv_item(index);
        if (!it) { pthread_mutex_unlock(&g_tv_mtx); continue; }
        const int depth = tv_depth(index), expanded = (it->state & 0x20) != 0, callback = it->text_callback;
        u16 text[520];
        size_t k = 0;
        for (; it->text && it->text[k] && k + 1 < 520; ++k) text[k] = it->text[k];
        text[k] = 0;
        const u32 next_sibling = it->next, parent = it->parent;
        pthread_mutex_unlock(&g_tv_mtx);
        if (callback) tv_dispinfo(hw, index, 0x0001, text, 520, NULL);
        const int children = tv_has_children(hw, index);
        const s32 y = 2 + r * row_h, level = depth + (at_root ? 1 : 0);
        const s32 x_box = 2 + (level - 1) * indent + indent / 2, y_mid = y + row_h / 2;
        if (lines && level > 0) {                                               /* dotted connector lines */
            for (s32 x = x_box; x < x_box + indent / 2 + 2; x += 2) beer_ctl_fill(dc, x, y_mid, x + 1, y_mid + 1, BEER_RGB(0xa0, 0xa0, 0xa0));
            for (s32 yy = y; yy < (next_sibling ? y + row_h : y_mid); yy += 2) beer_ctl_fill(dc, x_box, yy, x_box + 1, yy + 1, BEER_RGB(0xa0, 0xa0, 0xa0));
            /* ancestors that continue below draw their vertical line through this row */
            s32 lv = level - 1;
            for (u32 anc = parent; anc && lv > 0; --lv) {
                pthread_mutex_lock(&g_tv_mtx);
                const int continues = tv_item(anc) && tv_item(anc)->next != 0;
                const u32 up = tv_item(anc) ? tv_item(anc)->parent : 0;
                pthread_mutex_unlock(&g_tv_mtx);
                const s32 ax = 2 + (lv - 1) * indent + indent / 2;
                if (continues) for (s32 yy = y; yy < y + row_h; yy += 2) beer_ctl_fill(dc, ax, yy, ax + 1, yy + 1, BEER_RGB(0xa0, 0xa0, 0xa0));
                anc = up;
            }
        }
        if (buttons && children && level > 0) {                                  /* the expand button */
            const s32 bx = x_box - 4, by = y_mid - 4;
            beer_ctl_fill(dc, bx, by, bx + 9, by + 9, BEER_RGB(0xff, 0xff, 0xff));
            beer_ctl_frame(dc, bx, by, bx + 9, by + 9, BEER_RGB(0x91, 0x91, 0x91));
            beer_ctl_fill(dc, bx + 2, by + 4, bx + 7, by + 5, BEER_RGB(0x33, 0x33, 0x33));
            if (!expanded) beer_ctl_fill(dc, bx + 4, by + 2, bx + 5, by + 7, BEER_RGB(0x33, 0x33, 0x33));
        }
        const s32 icon_x = 2 + level * indent;
        /* a folder glyph */
        beer_ctl_fill(dc, icon_x + 1, y_mid - 5, icon_x + 7, y_mid - 3, BEER_RGB(0xe8, 0xb6, 0x3c));
        beer_ctl_fill(dc, icon_x + 1, y_mid - 3, icon_x + 15, y_mid + 6, BEER_RGB(0xf8, 0xd7, 0x75));
        beer_ctl_frame(dc, icon_x + 1, y_mid - 3, icon_x + 15, y_mid + 6, BEER_RGB(0xd0, 0x9e, 0x2c));
        int32_t extent[4] = { icon_x + 20, y, right, y + row_h };
        beer_DrawTextW(dc, text, -1, extent, 0x0400 | 0x0020 | 0x0800);
        const s32 text_r = extent[2] + 2 < right ? extent[2] + 2 : right;
        if (index == selected && (focused || (w->style & 0x0020))) {               /* TVS_SHOWSELALWAYS */
            beer_ctl_fill(dc, icon_x + 18, y, text_r, y + row_h, focused ? BEER_RGB(0xcc, 0xe8, 0xff) : BEER_RGB(0xd9, 0xd9, 0xd9));
            if (focused) beer_ctl_frame(dc, icon_x + 18, y, text_r, y + row_h, BEER_RGB(0x99, 0xd1, 0xff));
        }
        beer_SetTextColor(dc, 0);
        int32_t rect[4] = { icon_x + 20, y, right, y + row_h };
        beer_DrawTextW(dc, text, -1, rect, 0x0020 | 0x0004 | 0x0800 | 0x8000);
    }
    if (has_scrollbar) {                                                          /* a vertical scroll bar */
        const s32 x0 = w->w - 18, span = w->h - 2;
        beer_ctl_fill(dc, x0, 1, w->w - 1, w->h - 1, BEER_RGB(0xf0, 0xf0, 0xf0));
        s32 thumb = span * page / (total ? total : 1);
        if (thumb < 12) thumb = 12;
        const s32 top = 1 + (span - thumb) * t->scroll / (total - page > 0 ? total - page : 1);
        beer_ctl_fill(dc, x0 + 2, top, w->w - 3, top + thumb, BEER_RGB(0xc2, 0xc2, 0xc2));
    }
    beer_ctl_frame(dc, 0, 0, w->w, w->h, BEER_RGB(0x82, 0x87, 0x90));
    beer_ctl_paint_end(w);
}

/* ---- the window procedure ---- */
static u64 __attribute__((ms_abi)) beer_ctl_treeview_proc(u64 hw, u32 msg, u64 wp, u64 lp)
{
    u64 handled = 0;
    if (msg == 0x0030) {                                                         /* WM_SETFONT */
        pthread_mutex_lock(&g_tv_mtx);
        TvTree *t = tv_tree(hw, 1);
        if (t) t->font = wp;
        pthread_mutex_unlock(&g_tv_mtx);
    }
    if (beer_ctl_common(hw, msg, wp, lp, &handled)) return handled;
    switch (msg) {
    case 0x000F: tv_paint(hw); return 0;
    case 0x0087: return 0x0001 | 0x0004;                                         /* WM_GETDLGCODE: arrows, chars */
    case 0x0082: {                                                               /* WM_NCDESTROY: free the items */
        pthread_mutex_lock(&g_tv_mtx);
        for (u32 i = 0; i < TV_MAX_ITEMS; ++i)
            if (g_tv_items[i].used && g_tv_items[i].tree == hw) { free(g_tv_items[i].text); memset(&g_tv_items[i], 0, sizeof(g_tv_items[i])); }
        TvTree *t = tv_tree(hw, 0);
        if (t) t->used = 0;
        pthread_mutex_unlock(&g_tv_mtx);
        beer_ctl_destroy(hw);
        return impl_DefWindowProcW(hw, msg, wp, lp);
    }
    case 0x1132: case 0x1100: return lp ? tv_insert(hw, (const u8 *)(uintptr_t)lp) : 0;      /* TVM_INSERTITEMW / A */
    case 0x1101: {                                                               /* TVM_DELETEITEM */
        if (lp == TV_ROOT || (u32)lp == 0xFFFF0000u || !lp) {
            u32 root;
            do {
                pthread_mutex_lock(&g_tv_mtx);
                TvTree *t = tv_tree(hw, 0);
                root = t ? t->first_root : 0;
                pthread_mutex_unlock(&g_tv_mtx);
                if (root) tv_delete(hw, root);
            } while (root);
        } else if (tv_index(lp)) tv_delete(hw, tv_index(lp));
        else return 0;
        beer_ctl_redraw(hw);
        return 1;
    }
    case 0x1102: return tv_index(lp) ? tv_expand(hw, tv_index(lp), (u32)wp) : 0;          /* TVM_EXPAND */
    case 0x1104: {                                                               /* TVM_GETITEMRECT */
        if (!lp) return 0;
        u32 item_value = 0;
        memcpy(&item_value, (const void *)(uintptr_t)lp, 4);
        u64 item = tv_guest32() ? item_value : *(const u64 *)(uintptr_t)lp;
        pthread_mutex_lock(&g_tv_mtx);
        TvTree *t = tv_tree(hw, 0);
        const u32 index = tv_index(item);
        const s32 row = t && index ? tv_row_of(t, index) : -1;
        const s32 row_h = t ? tv_row_height(t) : 17, depth = index ? tv_depth(index) : 0, indent = t ? t->indent : 19;
        const s32 scroll = t ? t->scroll : 0;
        pthread_mutex_unlock(&g_tv_mtx);
        BeerWindow *w = beer_window_other(hw);
        if (row < 0 || !w) return 0;
        s32 *rect = (s32 *)(uintptr_t)lp;
        rect[1] = 2 + (row - scroll) * row_h; rect[3] = rect[1] + row_h;
        rect[0] = wp ? 2 + (depth + 1) * indent + 18 : 0; rect[2] = wp ? w->w - 2 : w->w;
        return 1;
    }
    case 0x1105: {                                                               /* TVM_GETCOUNT */
        u64 n = 0;
        pthread_mutex_lock(&g_tv_mtx);
        for (u32 i = 0; i < TV_MAX_ITEMS; ++i) if (g_tv_items[i].used && g_tv_items[i].tree == hw) ++n;
        pthread_mutex_unlock(&g_tv_mtx);
        return n;
    }
    case 0x1106: { pthread_mutex_lock(&g_tv_mtx); TvTree *t = tv_tree(hw, 1); const u64 r = t ? (u64)t->indent : 19;
                   pthread_mutex_unlock(&g_tv_mtx); return r; }                  /* TVM_GETINDENT */
    case 0x1107: { pthread_mutex_lock(&g_tv_mtx); TvTree *t = tv_tree(hw, 1); if (t) t->indent = wp < 12 ? 12 : (s32)wp;
                   pthread_mutex_unlock(&g_tv_mtx); beer_ctl_redraw(hw); return 1; }   /* TVM_SETINDENT */
    case 0x1108: { pthread_mutex_lock(&g_tv_mtx); TvTree *t = tv_tree(hw, 1); const u64 r = t ? t->image_list : 0;
                   pthread_mutex_unlock(&g_tv_mtx); return r; }                  /* TVM_GETIMAGELIST */
    case 0x1109: { pthread_mutex_lock(&g_tv_mtx); TvTree *t = tv_tree(hw, 1); const u64 r = t ? t->image_list : 0;
                   if (t && wp == 0) t->image_list = lp; pthread_mutex_unlock(&g_tv_mtx); return r; }   /* TVM_SETIMAGELIST */
    case 0x110A: {                                                               /* TVM_GETNEXTITEM */
        pthread_mutex_lock(&g_tv_mtx);
        TvTree *t = tv_tree(hw, 1);
        const u32 index = tv_index(lp);
        TvItem *it = tv_item(index);
        u32 r = 0;
        switch ((u32)wp & 0xff) {
        case 0: r = t ? t->first_root : 0; break;                                  /* TVGN_ROOT */
        case 1: r = it ? it->next : 0; break;                                      /* NEXT */
        case 2: r = it ? it->prev : 0; break;                                      /* PREVIOUS */
        case 3: r = it ? it->parent : 0; break;                                    /* PARENT */
        case 4: r = it ? it->first_child : (!lp && t ? t->first_root : 0); break;  /* CHILD */
        case 5: r = t ? tv_item_at_row(t, t->scroll) : 0; break;                    /* FIRSTVISIBLE */
        case 6: r = it ? tv_next_visible(t, index) : 0; break;                     /* NEXTVISIBLE */
        case 7: r = it ? tv_prev_visible(t, index) : 0; break;                     /* PREVIOUSVISIBLE */
        case 8: r = 0; break;                                                      /* DROPHILITE */
        case 9: r = t ? t->selected : 0; break;                                    /* CARET */
        case 10: { u32 last = 0; for (u32 cur = t ? t->first_root : 0; cur; cur = tv_next_visible(t, cur)) last = cur; r = last; break; }
        default: break;
        }
        pthread_mutex_unlock(&g_tv_mtx);
        return tv_handle(r);
    }
    case 0x110B: {                                                               /* TVM_SELECTITEM */
        const u32 index = tv_index(lp);
        if ((wp & 0xff) == 9) return (u64)tv_select(hw, index, 0);                 /* TVGN_CARET */
        if ((wp & 0xff) == 5 && index) {                                           /* TVGN_FIRSTVISIBLE */
            pthread_mutex_lock(&g_tv_mtx);
            TvTree *t = tv_tree(hw, 0);
            if (t) { const s32 row = tv_row_of(t, index); if (row >= 0) t->scroll = row; }
            pthread_mutex_unlock(&g_tv_mtx);
            beer_ctl_redraw(hw);
            return 1;
        }
        return 1;
    }
    case 0x113E: case 0x110C: case 0x113F: case 0x110D: {                         /* TVM_GETITEMW/A, TVM_SETITEMW/A */
        if (!lp) return 0;
        TvItemFields f;
        tv_read_item((const u8 *)(uintptr_t)lp, &f);
        const u32 index = tv_index(f.item);
        if (!index) return 0;
        const int set = msg == 0x113F || msg == 0x110D;
        if (set) {
            pthread_mutex_lock(&g_tv_mtx);
            TvItem *it = tv_item(index);
            if (f.mask & 0x0001) {
                free(it->text); it->text = NULL;
                it->text_callback = f.text == TV_TEXTCALLBACK;
                if (!it->text_callback) it->text = beer_ctl_wdup((const u16 *)(uintptr_t)f.text);
            }
            if (f.mask & 0x0040) it->children = f.children;
            if (f.mask & 0x0004) it->lparam = f.lparam;
            if (f.mask & 0x0002) it->image = f.image;
            if (f.mask & 0x0020) it->selected_image = f.selected_image;
            if (f.mask & 0x0008) it->state = (it->state & ~f.state_mask) | (f.state & f.state_mask);
            pthread_mutex_unlock(&g_tv_mtx);
            beer_ctl_redraw(hw);
            return 1;
        }
        pthread_mutex_lock(&g_tv_mtx);
        TvItem *it = tv_item(index);
        const int callback = it->text_callback;
        if (f.mask & 0x0004) f.lparam = it->lparam;
        if (f.mask & 0x0008) f.state = it->state & (f.state_mask ? f.state_mask : 0xffff);
        if (f.mask & 0x0002) f.image = it->image;
        if (f.mask & 0x0020) f.selected_image = it->selected_image;
        if (f.mask & 0x0040) f.children = it->children == -2 ? (it->first_child != 0) : it->children;
        if ((f.mask & 0x0001) && f.text && f.text != TV_TEXTCALLBACK && f.text_max > 0 && !callback) {
            u16 *out = (u16 *)(uintptr_t)f.text;
            s32 k = 0;
            for (; it->text && it->text[k] && k + 1 < f.text_max; ++k) out[k] = it->text[k];
            out[k] = 0;
        }
        pthread_mutex_unlock(&g_tv_mtx);
        if ((f.mask & 0x0001) && callback && f.text && f.text != TV_TEXTCALLBACK && f.text_max > 0)
            tv_dispinfo(hw, index, 0x0001, (u16 *)(uintptr_t)f.text, (size_t)f.text_max, NULL);
        tv_write_item((u8 *)(uintptr_t)lp, &f);
        return 1;
    }
    case 0x1110: { pthread_mutex_lock(&g_tv_mtx); TvTree *t = tv_tree(hw, 1); const u64 r = t ? (u64)tv_page_rows(hw, t) : 0;
                   pthread_mutex_unlock(&g_tv_mtx); return r; }                  /* TVM_GETVISIBLECOUNT */
    case 0x1111: {                                                               /* TVM_HITTEST: { POINT, flags, hItem } */
        if (!lp) return 0;
        s32 *info = (s32 *)(uintptr_t)lp;
        pthread_mutex_lock(&g_tv_mtx);
        TvTree *t = tv_tree(hw, 1);
        const s32 row = t ? (info[1] - 2) / tv_row_height(t) + t->scroll : -1;
        const u32 index = info[1] >= 2 && t ? tv_item_at_row(t, row) : 0;
        pthread_mutex_unlock(&g_tv_mtx);
        info[2] = index ? 0x0004 : 0x0001;                                         /* TVHT_ONITEMLABEL / NOWHERE */
        if (tv_guest32()) info[3] = (s32)tv_handle(index); else { const u64 h = tv_handle(index); memcpy(info + 4, &h, 8); }
        return tv_handle(index);
    }
    case 0x1113: case 0x1115: {                                                  /* TVM_SORTCHILDREN(CB): by text */
        pthread_mutex_lock(&g_tv_mtx);
        TvTree *t = tv_tree(hw, 1);
        const u32 parent = tv_index(lp);
        if (t) {
            u32 sorted = 0;                                                          /* insertion sort of the list */
            for (u32 cur = tv_first_child(t, parent); cur; ) {
                const u32 next = tv_item(cur)->next;
                if (!sorted || tv_compare(tv_item(cur)->text, tv_item(sorted)->text) < 0) {
                    tv_item(cur)->next = sorted; tv_item(cur)->prev = 0; if (sorted) tv_item(sorted)->prev = cur; sorted = cur;
                } else {
                    u32 at = sorted;
                    while (tv_item(at)->next && tv_compare(tv_item(tv_item(at)->next)->text, tv_item(cur)->text) <= 0)
                        at = tv_item(at)->next;
                    tv_item(cur)->next = tv_item(at)->next; tv_item(cur)->prev = at;
                    if (tv_item(at)->next) tv_item(tv_item(at)->next)->prev = cur;
                    tv_item(at)->next = cur;
                }
                cur = next;
            }
            if (parent) tv_item(parent)->first_child = sorted; else t->first_root = sorted;
        }
        pthread_mutex_unlock(&g_tv_mtx);
        beer_ctl_redraw(hw);
        return 1;
    }
    case 0x1114: if (tv_index(lp)) { tv_ensure_visible(hw, tv_index(lp)); beer_ctl_redraw(hw); } return 1;   /* ENSUREVISIBLE */
    case 0x111B: { pthread_mutex_lock(&g_tv_mtx); TvTree *t = tv_tree(hw, 1); const u64 r = t ? (u64)tv_row_height(t) : 17;
                   if (t && (s32)wp > 0) t->item_height = (s32)wp; pthread_mutex_unlock(&g_tv_mtx); beer_ctl_redraw(hw); return r; }
    case 0x111C: { pthread_mutex_lock(&g_tv_mtx); TvTree *t = tv_tree(hw, 1); const u64 r = t ? (u64)tv_row_height(t) : 17;
                   pthread_mutex_unlock(&g_tv_mtx); return r; }                  /* TVM_GETITEMHEIGHT */
    case 0x1127: {                                                               /* TVM_GETITEMSTATE */
        pthread_mutex_lock(&g_tv_mtx);
        TvItem *it = tv_item(tv_index(wp));
        const u64 r = it ? (it->state & (u32)lp) : 0;
        pthread_mutex_unlock(&g_tv_mtx);
        return r;
    }
    case 0x111D: case 0x111E: case 0x1120: case 0x1121: case 0x1128: case 0x1129: return (u64)(s64)-1;   /* colours */
    case 0x0201: case 0x0203: {                                                  /* WM_LBUTTONDOWN / DBLCLK */
        impl_SetFocus(hw);
        BeerWindow *w = beer_window_other(hw);
        const s32 x = (s16)(lp & 0xffff), y = (s16)((lp >> 16) & 0xffff);
        pthread_mutex_lock(&g_tv_mtx);
        TvTree *t = tv_tree(hw, 1);
        const s32 total = t ? tv_visible_rows(t) : 0, page = t ? tv_page_rows(hw, t) : 1;
        if (w && t && total > page && x >= w->w - 18) {                            /* the scroll bar: page */
            t->scroll += y < w->h / 2 ? -page : page;
            pthread_mutex_unlock(&g_tv_mtx);
            beer_ctl_redraw(hw);
            return 0;
        }
        const u32 index = t && y >= 2 ? tv_item_at_row(t, (y - 2) / tv_row_height(t) + t->scroll) : 0;
        const s32 level = index ? tv_depth(index) + ((w && (w->style & 0x0004)) ? 1 : 0) : 0, indent = t ? t->indent : 19;
        pthread_mutex_unlock(&g_tv_mtx);
        if (!index) return 0;
        const s32 x_box = 2 + (level - 1) * indent + indent / 2;
        if ((msg == 0x0201 && level > 0 && x >= x_box - 6 && x <= x_box + 6) || msg == 0x0203) {
            tv_expand(hw, index, 3);                                                 /* the button, or a double click */
            if (msg == 0x0203) {
                u8 nm[24] = { 0 };
                u8 *g = tv_guest_alloc(24);
                if (g) { tv_write_hdr(g, hw, (u32)-3); tv_send_notify(hw, g); tv_guest_free(g); }   /* NM_DBLCLK */
                (void)nm;
            }
            if (msg == 0x0201) return 0;
        }
        tv_select(hw, index, 1);                                                     /* TVC_BYMOUSE */
        u8 *g = tv_guest_alloc(24);
        if (g) { tv_write_hdr(g, hw, (u32)-2); tv_send_notify(hw, g); tv_guest_free(g); }       /* NM_CLICK */
        return 0;
    }
    case 0x020A: {                                                               /* WM_MOUSEWHEEL */
        pthread_mutex_lock(&g_tv_mtx);
        TvTree *t = tv_tree(hw, 1);
        if (t) t->scroll += (s16)(wp >> 16) > 0 ? -3 : 3;
        if (t && t->scroll < 0) t->scroll = 0;
        pthread_mutex_unlock(&g_tv_mtx);
        beer_ctl_redraw(hw);
        return 0;
    }
    case 0x0100: {                                                               /* WM_KEYDOWN */
        pthread_mutex_lock(&g_tv_mtx);
        TvTree *t = tv_tree(hw, 1);
        const u32 sel = t ? t->selected : 0;
        u32 target = 0;
        int expand = 0;
        const s32 page = t ? tv_page_rows(hw, t) : 1;
        TvItem *it = tv_item(sel);
        switch (wp) {
        case 0x26: target = sel ? tv_prev_visible(t, sel) : (t ? t->first_root : 0); break;            /* up */
        case 0x28: target = sel ? tv_next_visible(t, sel) : (t ? t->first_root : 0); break;            /* down */
        case 0x24: target = t ? t->first_root : 0; break;                                               /* home */
        case 0x23: { for (u32 cur = t ? t->first_root : 0; cur; cur = tv_next_visible(t, cur)) target = cur; break; }
        case 0x21: { target = sel; for (s32 i = 0; i < page && target && tv_prev_visible(t, target); ++i) target = tv_prev_visible(t, target); break; }
        case 0x22: { target = sel; for (s32 i = 0; i < page && target && tv_next_visible(t, target); ++i) target = tv_next_visible(t, target); break; }
        case 0x25: if (it && (it->state & 0x20)) expand = 1; else target = it ? it->parent : 0; break;  /* left */
        case 0x27: if (it && (it->state & 0x20)) target = it->first_child; else expand = 2; break;      /* right */
        case 0x6B: expand = 2; break;                                                                    /* + */
        case 0x6D: expand = 1; break;                                                                    /* - */
        case 0x0D: target = 0; break;
        default: break;
        }
        pthread_mutex_unlock(&g_tv_mtx);
        if (wp == 0x0D) {                                                         /* NM_RETURN */
            u8 *g = tv_guest_alloc(24);
            if (g) { tv_write_hdr(g, hw, (u32)-4); tv_send_notify(hw, g); tv_guest_free(g); }
            return 0;
        }
        if (expand && sel) tv_expand(hw, sel, (u32)expand);
        if (target) tv_select(hw, target, 2);                                       /* TVC_BYKEYBOARD */
        return 0;
    }
    case 0x0007: case 0x0008: {                                                  /* focus: NM_SETFOCUS / NM_KILLFOCUS */
        u8 *g = tv_guest_alloc(24);
        if (g) { tv_write_hdr(g, hw, msg == 0x0007 ? (u32)-7 : (u32)-8); tv_send_notify(hw, g); tv_guest_free(g); }
        return impl_DefWindowProcW(hw, msg, wp, lp);
    }
    default: break;
    }
    if (msg >= 0x1100 && msg < 0x1200) return 0;                                 /* other TVM_ messages */
    return impl_DefWindowProcW(hw, msg, wp, lp);
}
