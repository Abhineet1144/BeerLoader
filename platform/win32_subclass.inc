/* COMCTL32 subclass helpers. Include after the window-long implementations.
 * Window ownership makes this registry thread-local: no lock spans guest code.
 * Nodes are tombstoned while dispatch is active; stack-local continuation frames
 * keep nested messages and repeated DefSubclassProc calls independent. */
typedef struct BeerSubclassNode {
    struct BeerSubclassNode *next;
    u64 proc, id, ref;
    int removed;
} BeerSubclassNode;

typedef struct BeerSubclassWindow {
    struct BeerSubclassWindow *next;
    BeerSubclassNode *head;
    u64 hwnd, original;
    unsigned active;
    int dead;
} BeerSubclassWindow;

typedef struct BeerSubclassFrame {
    struct BeerSubclassFrame *previous;
    BeerSubclassWindow *window;
    BeerSubclassNode *next;
} BeerSubclassFrame;

static __thread BeerSubclassWindow *g_subclass_windows;
static __thread BeerSubclassFrame *g_subclass_frame;

static u64 call_window_callback6(u64 proc, u64 hwnd, u32 msg, u64 wp,
                                 u64 lp, u64 id, u64 ref);
static u64 __attribute__((ms_abi))
beer_subclass_dispatch(u64 hwnd, u32 msg, u64 wp, u64 lp);

static u64 beer_subclass_dispatch_address(void)
{
    return (u64)(uintptr_t)beer_subclass_dispatch;
}

static int beer_subclass_owned(u64 hwnd)
{
    BeerWindow *w = beer_window_find(hwnd);
    if (!w) { g_last_error = 1400; return 0; } /* ERROR_INVALID_WINDOW_HANDLE */
    u32 owner = hwnd == g_native_hwnd && g_window_thread_id
                    ? g_window_thread_id : w->owner_tid;
    if (!owner || owner != (u32)current_windows_thread_id()) {
        g_last_error = 5; /* ERROR_ACCESS_DENIED */
        return 0;
    }
    return 1;
}

static BeerSubclassWindow *beer_subclass_find(u64 hwnd)
{
    for (BeerSubclassWindow *w = g_subclass_windows; w; w = w->next)
        if (w->hwnd == hwnd) return w;
    return NULL;
}

static void beer_subclass_unlink(BeerSubclassWindow *w)
{
    BeerSubclassWindow **link = &g_subclass_windows;
    while (*link && *link != w) link = &(*link)->next;
    if (*link) *link = w->next;
}

/* Only called without live continuation frames for this window. If somebody
 * put another WndProc above ours, retain an empty pass-through chain: that proc
 * can still call its saved dispatcher. Never replace that third-party proc. */
static void beer_subclass_collect(BeerSubclassWindow *w)
{
    if (w->active) return;
    BeerSubclassNode **link = &w->head;
    while (*link) {
        BeerSubclassNode *node = *link;
        if (w->dead || node->removed) {
            *link = node->next;
            free(node);
        } else link = &node->next;
    }
    if (!w->dead && w->head) return;
    if (beer_window_find(w->hwnd) &&
        impl_GetWindowLongPtrW(w->hwnd, -4) == beer_subclass_dispatch_address()) {
        impl_SetWindowLongPtrW(w->hwnd, -4, w->original);
        if (!w->dead && impl_GetWindowLongPtrW(w->hwnd, -4) ==
                            beer_subclass_dispatch_address()) return;
    } else if (!w->dead && beer_window_find(w->hwnd)) return;
    beer_subclass_unlink(w);
    free(w);
}

static u64 beer_subclass_next(BeerSubclassWindow *w, BeerSubclassNode *node,
                              u64 hwnd, u32 msg, u64 wp, u64 lp)
{
    while (node && node->removed) node = node->next;
    BeerSubclassFrame frame = { g_subclass_frame, w, node ? node->next : NULL };
    g_subclass_frame = &frame;
    u64 result;
    if (node)
        result = call_window_callback6(node->proc, hwnd, msg, wp, lp,
                                       node->id, node->ref);
    else if (w->original)
        result = call_window_callback(w->original, hwnd, msg, wp, lp);
    else
        result = impl_DefWindowProcW(hwnd, msg, wp, lp);
    g_subclass_frame = frame.previous;
    return result;
}

static u64 __attribute__((ms_abi))
beer_subclass_dispatch(u64 hwnd, u32 msg, u64 wp, u64 lp)
{
    if (!beer_subclass_owned(hwnd)) return 0;
    BeerSubclassWindow *w = beer_subclass_find(hwnd);
    if (!w) return impl_DefWindowProcW(hwnd, msg, wp, lp);
    ++w->active;
    u64 result = beer_subclass_next(w, w->head, hwnd, msg, wp, lp);
    if (msg == 0x0082) { /* WM_NCDESTROY, including swallowed messages */
        w->dead = 1;
        beer_subclass_unlink(w);
        for (BeerSubclassNode *node = w->head; node; node = node->next)
            node->removed = 1;
    }
    --w->active;
    beer_subclass_collect(w);
    return result;
}

static u64 __attribute__((ms_abi))
impl_SetWindowSubclass(u64 hwnd, u64 proc, u64 id, u64 ref)
{
    if (!beer_subclass_owned(hwnd)) return 0;
    if (!proc) { g_last_error = 87; return 0; }
    /* A nested WM_NCDESTROY may already have unlinked the live frames. */
    for (BeerSubclassFrame *f = g_subclass_frame; f; f = f->previous)
        if (f->window->hwnd == hwnd && f->window->dead) return 0;
    BeerSubclassWindow *w = beer_subclass_find(hwnd);
    if (w) {
        for (BeerSubclassNode *node = w->head; node; node = node->next)
            if (!node->removed && node->proc == proc && node->id == id) {
                node->ref = ref;
                return 1;
            }
    }
    BeerSubclassNode *node = calloc(1, sizeof(*node));
    if (!node) { g_last_error = 8; return 0; }
    if (!w) {
        w = calloc(1, sizeof(*w));
        if (!w) { free(node); g_last_error = 8; return 0; }
        w->hwnd = hwnd;
        w->original = impl_GetWindowLongPtrW(hwnd, -4);
        /* A stale saved dispatcher cannot be its own original procedure. */
        if (w->original == beer_subclass_dispatch_address()) {
            free(w); free(node); return 0;
        }
        impl_SetWindowLongPtrW(hwnd, -4, beer_subclass_dispatch_address());
        if (impl_GetWindowLongPtrW(hwnd, -4) != beer_subclass_dispatch_address()) {
            free(w); free(node); return 0;
        }
        w->next = g_subclass_windows;
        g_subclass_windows = w;
    }
    node->proc = proc;
    node->id = id;
    node->ref = ref;
    node->next = w->head;
    w->head = node;
    return 1;
}

static u64 __attribute__((ms_abi))
impl_GetWindowSubclass(u64 hwnd, u64 proc, u64 id, u64 *ref)
{
    if (!beer_subclass_owned(hwnd)) return 0;
    if (!proc || !ref) { g_last_error = 87; return 0; }
    BeerSubclassWindow *w = beer_subclass_find(hwnd);
    for (BeerSubclassNode *node = w ? w->head : NULL; node; node = node->next)
        if (!node->removed && node->proc == proc && node->id == id) {
            *ref = node->ref;
            return 1;
        }
    return 0;
}

static u64 __attribute__((ms_abi))
impl_RemoveWindowSubclass(u64 hwnd, u64 proc, u64 id)
{
    if (!beer_subclass_owned(hwnd)) return 0;
    if (!proc) { g_last_error = 87; return 0; }
    BeerSubclassWindow *w = beer_subclass_find(hwnd);
    for (BeerSubclassNode *node = w ? w->head : NULL; node; node = node->next)
        if (!node->removed && node->proc == proc && node->id == id) {
            node->removed = 1;
            beer_subclass_collect(w);
            return 1;
        }
    return 0;
}

static u64 __attribute__((ms_abi))
impl_DefSubclassProc(u64 hwnd, u32 msg, u64 wp, u64 lp)
{
    if (!beer_subclass_owned(hwnd)) return 0;
    for (BeerSubclassFrame *f = g_subclass_frame; f; f = f->previous)
        if (f->window->hwnd == hwnd)
            return beer_subclass_next(f->window, f->next, hwnd, msg, wp, lp);
    /* Without an active subclass invocation there is no chain to continue. */
    return impl_DefWindowProcW(hwnd, msg, wp, lp);
}