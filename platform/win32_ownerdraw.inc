/* Owner-drawn list boxes (LBS_OWNERDRAWFIXED / LBS_OWNERDRAWVARIABLE): the parent measures (WM_MEASUREITEM) and draws
 * (WM_DRAWITEM) each item into the list's canvas, as on Windows. Inno Setup's component and task lists
 * (TNewCheckListBox) are such lists. MEASUREITEMSTRUCT / DRAWITEMSTRUCT follow the program's bitness (tv_guest32).
 * Included at the end of win32_controls.inc. */

/* WM_MEASUREITEM for one item: its height (0 when the parent did not answer). */
static s32 od_measure(u64 hw, BeerWindow *w, s32 id, s32 index, u64 data, s32 default_h)
{
    u8 *m = tv_guest_alloc(32);
    if (!m) return 0;
    const u32 v[5] = { 2 /* ODT_LISTBOX */, (u32)id, (u32)index, (u32)(w->w > 0 ? w->w : 0), (u32)default_h };
    memcpy(m, v, 20);
    if (tv_guest32()) { const u32 d = (u32)data; memcpy(m + 20, &d, 4); }
    else memcpy(m + 24, &data, 8);
    call_window_proc(w->parent, 0x002C, (u64)(s64)id, (u64)(uintptr_t)m);       /* WM_MEASUREITEM */
    s32 h = 0;
    memcpy(&h, m + 16, 4);
    tv_guest_free(m);
    if (getenv("BEER_CTL_DIAGNOSTICS"))
        fprintf(stderr, "[OD] measure 0x%llx id 0x%x parent 0x%llx item %d (default %d) -> %d\n", (unsigned long long)hw,
                (u32)id, (unsigned long long)w->parent, index, default_h, h);
    return h;
}
/* WM_DRAWITEM for one item into dc at rect (ODA_DRAWENTIRE). */
static void od_draw(u64 hw, BeerWindow *w, s32 id, s32 index, u32 state, u64 dc, const s32 rect[4], u64 data)
{
    u8 *d = tv_guest_alloc(64);
    if (!d) return;
    const u32 head[5] = { 2 /* ODT_LISTBOX */, (u32)id, (u32)index, 1 /* ODA_DRAWENTIRE */, state };
    memcpy(d, head, 20);
    if (tv_guest32()) {
        const u32 tail[2] = { (u32)hw, (u32)dc };
        memcpy(d + 20, tail, 8);
        memcpy(d + 28, rect, 16);
        const u32 d32 = (u32)data;
        memcpy(d + 44, &d32, 4);
    } else {
        memcpy(d + 24, &hw, 8);
        memcpy(d + 32, &dc, 8);
        memcpy(d + 40, rect, 16);
        memcpy(d + 56, &data, 8);
    }
    call_window_proc(w->parent, 0x002B, (u64)(s64)id, (u64)(uintptr_t)d);       /* WM_DRAWITEM */
    tv_guest_free(d);
}
static void beer_ctl_owner_draw_list(u64 hw, BeerWindow *w, u64 dc)
{
    if (!w->parent) return;
    const int variable = (w->style & 0x0020) != 0, focused = beer_ctl_focused(hw);
    s32 id = 0, count = 0, top = 0, fixed = 0;
    pthread_mutex_lock(&g_beer_ctl_mtx);
    BeerCtl *c = beer_ctl_get(hw, 1);
    if (c) { id = c->id; count = (s32)c->count; top = c->top < 0 ? 0 : c->top; fixed = c->item_height; }
    pthread_mutex_unlock(&g_beer_ctl_mtx);
    if (!c) return;
    if (fixed <= 0) {                                               /* the fixed height: measured once (item 0) */
        fixed = od_measure(hw, w, id, 0, 0, 16);
        if (fixed <= 0) fixed = 16;
        pthread_mutex_lock(&g_beer_ctl_mtx);
        if ((c = beer_ctl_get(hw, 0))) c->item_height = fixed;
        pthread_mutex_unlock(&g_beer_ctl_mtx);
    }
    s32 total_h = 0;                                               /* measure the variable heights not known yet */
    for (s32 i = 0; i < count; ++i) {
        u64 data = 0;
        s32 h = 0;
        pthread_mutex_lock(&g_beer_ctl_mtx);
        c = beer_ctl_get(hw, 0);
        if (c && (u32)i < c->count) { data = c->item_data[i]; h = variable && c->heights ? c->heights[i] : fixed; }
        pthread_mutex_unlock(&g_beer_ctl_mtx);
        if (variable && !h) {
            h = od_measure(hw, w, id, i, data, fixed);
            if (h <= 0) h = fixed;
            pthread_mutex_lock(&g_beer_ctl_mtx);
            c = beer_ctl_get(hw, 0);
            if (c && (u32)i < c->count && c->heights) c->heights[i] = (u16)h;
            pthread_mutex_unlock(&g_beer_ctl_mtx);
        }
        total_h += h;
    }
    const int scrollbar = total_h > w->h - 2;
    const s32 right = w->w - 1 - (scrollbar ? 17 : 0);
    s32 y = 1;
    for (s32 i = top; i < count && y < w->h - 1; ++i) {
        u64 data = 0;
        u32 state = 0;
        s32 h = fixed;
        pthread_mutex_lock(&g_beer_ctl_mtx);
        c = beer_ctl_get(hw, 0);
        if (c && (u32)i < c->count) {
            data = c->item_data[i];
            h = beer_ctl_item_h(c, i);
            if (c->cursel == i || (c->selected && c->selected[i])) state |= 0x0001;    /* ODS_SELECTED */
            if (focused && (c->caret == i || (c->caret < 0 && c->cursel == i))) state |= 0x0010;   /* ODS_FOCUS */
        }
        pthread_mutex_unlock(&g_beer_ctl_mtx);
        if (w->style & 0x08000000u) state |= 0x0004;                                    /* ODS_DISABLED */
        const s32 rect[4] = { 1, y, right, y + h };
        beer_SaveDC(dc);
        beer_IntersectClipRect(dc, 1, 1, right, w->h - 1);
        od_draw(hw, w, id, i, state, dc, rect, data);
        beer_RestoreDC(dc, -1);
        y += h;
    }
    if (scrollbar) {                                                /* a vertical scroll bar */
        s32 visible_h = 0, above = 0;
        pthread_mutex_lock(&g_beer_ctl_mtx);
        c = beer_ctl_get(hw, 0);
        for (s32 i = 0; c && i < count; ++i) { if (i < top) above += beer_ctl_item_h(c, i); }
        pthread_mutex_unlock(&g_beer_ctl_mtx);
        visible_h = w->h - 2;
        const s32 x0 = w->w - 18, span = w->h - 2;
        beer_ctl_fill(dc, x0, 1, w->w - 1, w->h - 1, BEER_RGB(0xf0, 0xf0, 0xf0));
        s32 thumb = span * visible_h / (total_h ? total_h : 1);
        if (thumb < 12) thumb = 12;
        const s32 range = total_h - visible_h > 0 ? total_h - visible_h : 1;
        s32 top_px = 1 + (span - thumb) * (above < range ? above : range) / range;
        beer_ctl_fill(dc, x0 + 2, top_px, w->w - 3, top_px + thumb, BEER_RGB(0xc2, 0xc2, 0xc2));
    }
}
