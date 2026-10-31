#include "beer_message_dialog.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Xlib is loaded at run time, as in xwayland_backend.c, so the loader neither
 * links libX11 nor needs its headers. Only the 64-bit Xlib ABI pieces used here
 * are declared; offsets are those of <X11/Xlib.h> on LP64. */
typedef struct _XDisplay Display;
typedef unsigned long XID;
typedef XID Window;
typedef XID Atom;
typedef XID Font;
typedef void *GC;
typedef union { int type; long pad[24]; } XEvent;     /* sizeof(XEvent) == 192 */

#define X_KEY_PRESS        2
#define X_BUTTON_PRESS     4
#define X_EXPOSE           12
#define X_CLIENT_MESSAGE   33
#define X_MASK_KEY_PRESS   (1L << 0)
#define X_MASK_BUTTON      (1L << 2)
#define X_MASK_EXPOSURE    (1L << 15)
#define X_MASK_STRUCTURE   (1L << 17)
#define X_ATOM_ATOM        4
#define X_PROP_REPLACE     0

static struct {
    void *library;
    int (*InitThreads)(void);
    Display *(*OpenDisplay)(const char *);
    int (*CloseDisplay)(Display *);
    int (*DefaultScreen)(Display *);
    Window (*RootWindow)(Display *, int);
    int (*DisplayWidth)(Display *, int);
    int (*DisplayHeight)(Display *, int);
    Window (*CreateSimpleWindow)(Display *, Window, int, int, unsigned, unsigned, unsigned,
                                 unsigned long, unsigned long);
    int (*DestroyWindow)(Display *, Window);
    int (*StoreName)(Display *, Window, const char *);
    Atom (*InternAtom)(Display *, const char *, int);
    int (*ChangeProperty)(Display *, Window, Atom, Atom, int, int, const unsigned char *, int);
    int (*SetWMProtocols)(Display *, Window, Atom *, int);
    int (*SetWMNormalHints)(Display *, Window, void *);
    int (*SelectInput)(Display *, Window, long);
    int (*MapRaised)(Display *, Window);
    int (*NextEvent)(Display *, XEvent *);
    GC (*CreateGC)(Display *, Window, unsigned long, void *);
    int (*FreeGC)(Display *, GC);
    int (*SetForeground)(Display *, GC, unsigned long);
    int (*SetFont)(Display *, GC, Font);
    int (*FillRectangle)(Display *, Window, GC, int, int, unsigned, unsigned);
    int (*DrawRectangle)(Display *, Window, GC, int, int, unsigned, unsigned);
    int (*DrawString)(Display *, Window, GC, int, int, const char *, int);
    void *(*LoadQueryFont)(Display *, const char *);
    int (*FreeFont)(Display *, void *);
    int (*TextWidth)(void *, const char *, int);
    unsigned long (*LookupKeysym)(void *, int);
    int (*Flush)(Display *);
} x;
static pthread_mutex_t g_dialog_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_dialog_state;                          /* 0 untried, 1 usable, -1 unavailable */

static int dialog_load(void)
{
    if (g_dialog_state) return g_dialog_state > 0;
    g_dialog_state = -1;
    const char *setting = getenv("BEER_DIALOGS");
    if (setting && !strcmp(setting, "0")) return 0;
    x.library = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
    if (!x.library) return 0;
#define LOAD(field, symbol) if (!(*(void **)&x.field = dlsym(x.library, symbol))) return 0
    LOAD(InitThreads, "XInitThreads");
    LOAD(OpenDisplay, "XOpenDisplay");
    LOAD(CloseDisplay, "XCloseDisplay");
    LOAD(DefaultScreen, "XDefaultScreen");
    LOAD(RootWindow, "XRootWindow");
    LOAD(DisplayWidth, "XDisplayWidth");
    LOAD(DisplayHeight, "XDisplayHeight");
    LOAD(CreateSimpleWindow, "XCreateSimpleWindow");
    LOAD(DestroyWindow, "XDestroyWindow");
    LOAD(StoreName, "XStoreName");
    LOAD(InternAtom, "XInternAtom");
    LOAD(ChangeProperty, "XChangeProperty");
    LOAD(SetWMProtocols, "XSetWMProtocols");
    LOAD(SetWMNormalHints, "XSetWMNormalHints");
    LOAD(SelectInput, "XSelectInput");
    LOAD(MapRaised, "XMapRaised");
    LOAD(NextEvent, "XNextEvent");
    LOAD(CreateGC, "XCreateGC");
    LOAD(FreeGC, "XFreeGC");
    LOAD(SetForeground, "XSetForeground");
    LOAD(SetFont, "XSetFont");
    LOAD(FillRectangle, "XFillRectangle");
    LOAD(DrawRectangle, "XDrawRectangle");
    LOAD(DrawString, "XDrawString");
    LOAD(LoadQueryFont, "XLoadQueryFont");
    LOAD(FreeFont, "XFreeFont");
    LOAD(TextWidth, "XTextWidth");
    LOAD(LookupKeysym, "XLookupKeysym");
    LOAD(Flush, "XFlush");
#undef LOAD
    x.InitThreads();
    g_dialog_state = 1;
    return 1;
}

/* XFontStruct (LP64): fid at 8, ascent at 88, descent at 92. */
static Font font_id(void *font) { return *(Font *)((char *)font + 8); }
static int font_ascent(void *font) { return *(int *)((char *)font + 88); }
static int font_descent(void *font) { return *(int *)((char *)font + 92); }

enum { MAX_LINES = 64, MAX_LINE = 256, MARGIN = 20, BUTTON_HEIGHT = 30, BUTTON_GAP = 10 };

typedef struct {
    char lines[MAX_LINES][MAX_LINE];
    int count;
} WrappedText;

/* Split at newlines and wrap words at `width` pixels. */
static void wrap_text(const char *text, void *font, int width, WrappedText *out)
{
    out->count = 0;
    const char *p = text ? text : "";
    while (*p && out->count < MAX_LINES) {
        const char *end = strchr(p, '\n');
        size_t paragraph = end ? (size_t)(end - p) : strlen(p);
        size_t start = 0;
        do {
            size_t take = 0, last_space = 0;
            while (start + take < paragraph && take + 1 < MAX_LINE) {
                if (x.TextWidth(font, p + start, (int)(take + 1)) > width) break;
                if (p[start + take] == ' ') last_space = take;
                ++take;
            }
            if (start + take < paragraph && last_space) take = last_space + 1;
            size_t copy = take;
            while (copy && p[start + copy - 1] == ' ') --copy;
            memcpy(out->lines[out->count], p + start, copy);
            out->lines[out->count][copy] = 0;
            ++out->count;
            start += take ? take : 1;
        } while (start < paragraph && out->count < MAX_LINES);
        if (!paragraph && out->count < MAX_LINES) out->lines[out->count++][0] = 0;
        p = end ? end + 1 : p + paragraph;
    }
    if (!out->count) { out->lines[0][0] = 0; out->count = 1; }
}

typedef struct {
    Display *display;
    Window window;
    GC gc;
    void *font;
    const char *const *labels;
    int count, focus, button_y, width;
    int button_x[8], button_width[8];
} DialogLayout;

static void draw_buttons(const DialogLayout *d)
{
    x.SetForeground(d->display, d->gc, 0xf0f0f0);
    x.FillRectangle(d->display, d->window, d->gc, 0, d->button_y - 2, (unsigned)d->width, BUTTON_HEIGHT + 4);
    for (int i = 0; i < d->count; ++i) {
        x.SetForeground(d->display, d->gc, i == d->focus ? 0xdde8f8 : 0xffffff);
        x.FillRectangle(d->display, d->window, d->gc, d->button_x[i], d->button_y,
                        (unsigned)d->button_width[i], BUTTON_HEIGHT);
        x.SetForeground(d->display, d->gc, i == d->focus ? 0x2060c0 : 0x808080);
        x.DrawRectangle(d->display, d->window, d->gc, d->button_x[i], d->button_y,
                        (unsigned)d->button_width[i] - 1, BUTTON_HEIGHT - 1);
        x.SetForeground(d->display, d->gc, 0x000000);
        int length = (int)strlen(d->labels[i]);
        int label_width = x.TextWidth(d->font, d->labels[i], length);
        x.DrawString(d->display, d->window, d->gc, d->button_x[i] + (d->button_width[i] - label_width) / 2,
                     d->button_y + (BUTTON_HEIGHT + font_ascent(d->font) - font_descent(d->font)) / 2,
                     d->labels[i], length);
    }
    x.Flush(d->display);
}

int beer_message_dialog(const char *title, const char *text, const int *ids,
                        const char *const *labels, int count, int default_index,
                        int cancel_index)
{
    if (count <= 0 || count > 8) return -1;
    pthread_mutex_lock(&g_dialog_lock);
    if (!dialog_load()) { pthread_mutex_unlock(&g_dialog_lock); return -1; }
    Display *display = x.OpenDisplay(NULL);
    if (!display) { pthread_mutex_unlock(&g_dialog_lock); return -1; }

    int screen = x.DefaultScreen(display);
    void *font = x.LoadQueryFont(display, "-misc-fixed-medium-r-normal--15-*-*-*-*-*-iso8859-1");
    if (!font) font = x.LoadQueryFont(display, "fixed");
    if (!font) { x.CloseDisplay(display); pthread_mutex_unlock(&g_dialog_lock); return -1; }
    int line_height = font_ascent(font) + font_descent(font) + 2;

    static WrappedText wrapped;
    int text_width = 560;
    wrap_text(text, font, text_width, &wrapped);
    int widest = 0;
    for (int i = 0; i < wrapped.count; ++i) {
        int w = x.TextWidth(font, wrapped.lines[i], (int)strlen(wrapped.lines[i]));
        if (w > widest) widest = w;
    }
    int button_width[8], buttons_total = 0;
    for (int i = 0; i < count; ++i) {
        button_width[i] = x.TextWidth(font, labels[i], (int)strlen(labels[i])) + 32;
        if (button_width[i] < 88) button_width[i] = 88;
        buttons_total += button_width[i] + (i ? BUTTON_GAP : 0);
    }
    int width = (widest > buttons_total ? widest : buttons_total) + 2 * MARGIN;
    if (width < 320) width = 320;
    int height = MARGIN + wrapped.count * line_height + MARGIN + BUTTON_HEIGHT + MARGIN;
    int screen_w = x.DisplayWidth(display, screen), screen_h = x.DisplayHeight(display, screen);

    Window window = x.CreateSimpleWindow(display, x.RootWindow(display, screen),
                                         (screen_w - width) / 2, (screen_h - height) / 2,
                                         (unsigned)width, (unsigned)height, 1, 0x808080, 0xf0f0f0);
    x.StoreName(display, window, title && *title ? title : "Error");
    Atom type = x.InternAtom(display, "_NET_WM_WINDOW_TYPE", 0);
    Atom dialog = x.InternAtom(display, "_NET_WM_WINDOW_TYPE_DIALOG", 0);
    x.ChangeProperty(display, window, type, X_ATOM_ATOM, 32, X_PROP_REPLACE, (unsigned char *)&dialog, 1);
    Atom state = x.InternAtom(display, "_NET_WM_STATE", 0);
    Atom above = x.InternAtom(display, "_NET_WM_STATE_ABOVE", 0);
    x.ChangeProperty(display, window, state, X_ATOM_ATOM, 32, X_PROP_REPLACE, (unsigned char *)&above, 1);
    Atom delete_window = x.InternAtom(display, "WM_DELETE_WINDOW", 0);
    x.SetWMProtocols(display, window, &delete_window, 1);
    long hints[16] = {0};                           /* XSizeHints: fixed size */
    hints[0] = (1L << 4) | (1L << 5);               /* PMinSize | PMaxSize */
    ((int *)hints)[6] = width; ((int *)hints)[7] = height;
    ((int *)hints)[8] = width; ((int *)hints)[9] = height;
    x.SetWMNormalHints(display, window, hints);
    x.SelectInput(display, window, X_MASK_EXPOSURE | X_MASK_KEY_PRESS | X_MASK_BUTTON | X_MASK_STRUCTURE);
    GC gc = x.CreateGC(display, window, 0, NULL);
    x.SetFont(display, gc, font_id(font));
    x.MapRaised(display, window);
    x.Flush(display);

    DialogLayout layout = { display, window, gc, font, labels, count,
                            default_index >= 0 && default_index < count ? default_index : 0,
                            height - MARGIN - BUTTON_HEIGHT, width, {0}, {0} };
    {
        int bx = width - MARGIN - buttons_total;
        for (int i = 0; i < count; ++i) {
            layout.button_x[i] = bx;
            layout.button_width[i] = button_width[i];
            bx += button_width[i] + BUTTON_GAP;
        }
    }
    int chosen = -1;
    while (chosen < 0) {
        XEvent event;
        x.NextEvent(display, &event);
        const char *e = (const char *)event.pad;
        if (event.type == X_EXPOSE) {
            x.SetForeground(display, gc, 0x000000);
            for (int i = 0; i < wrapped.count; ++i)
                x.DrawString(display, window, gc, MARGIN, MARGIN + font_ascent(font) + i * line_height,
                             wrapped.lines[i], (int)strlen(wrapped.lines[i]));
            draw_buttons(&layout);
        } else if (event.type == X_BUTTON_PRESS) {           /* XButtonEvent x@64 y@68 */
            int px = *(const int *)(e + 64), py = *(const int *)(e + 68);
            for (int i = 0; i < count; ++i)
                if (px >= layout.button_x[i] && px < layout.button_x[i] + layout.button_width[i] &&
                    py >= layout.button_y && py < layout.button_y + BUTTON_HEIGHT)
                    chosen = i;
        } else if (event.type == X_KEY_PRESS) {
            unsigned long key = x.LookupKeysym(event.pad, 0);
            if (key == 0xff0d || key == 0xff8d || key == 0x20) chosen = layout.focus;  /* Return, KP_Enter, space */
            else if (key == 0xff1b) chosen = cancel_index >= 0 ? cancel_index : layout.focus;   /* Escape */
            else if (key == 0xff09 || key == 0xff53 || key == 0xff51) {                /* Tab, Right, Left */
                layout.focus = (layout.focus + (key == 0xff51 ? count - 1 : 1)) % count;
                draw_buttons(&layout);
            }
        } else if (event.type == X_CLIENT_MESSAGE &&         /* XClientMessageEvent data.l[0]@56 */
                   *(const Atom *)(e + 56) == delete_window) {
            chosen = cancel_index >= 0 ? cancel_index : layout.focus;
        }
    }
    x.FreeGC(display, gc);
    x.FreeFont(display, font);
    x.DestroyWindow(display, window);
    x.CloseDisplay(display);
    pthread_mutex_unlock(&g_dialog_lock);
    return ids[chosen];
}

int beer_message_box_dialog(const char *title, const char *text, unsigned type)
{
    enum { IDOK = 1, IDCANCEL = 2, IDABORT = 3, IDRETRY = 4, IDIGNORE = 5, IDYES = 6, IDNO = 7,
           IDTRYAGAIN = 10, IDCONTINUE = 11 };
    static const struct { int count, cancel; int ids[3]; const char *labels[3]; } sets[] = {
        { 1, 0, { IDOK }, { "OK" } },
        { 2, 1, { IDOK, IDCANCEL }, { "OK", "Cancel" } },
        { 3, -1, { IDABORT, IDRETRY, IDIGNORE }, { "Abort", "Retry", "Ignore" } },
        { 3, 2, { IDYES, IDNO, IDCANCEL }, { "Yes", "No", "Cancel" } },
        { 2, -1, { IDYES, IDNO }, { "Yes", "No" } },
        { 2, 1, { IDRETRY, IDCANCEL }, { "Retry", "Cancel" } },
        { 3, 0, { IDCANCEL, IDTRYAGAIN, IDCONTINUE }, { "Cancel", "Try Again", "Continue" } },
    };
    unsigned kind = type & 0xf;
    if (kind >= sizeof(sets) / sizeof(*sets)) kind = 0;
    int default_index = (int)((type >> 8) & 0xf);   /* MB_DEFBUTTON1..4 = 0x000, 0x100, 0x200, 0x300 */
    if (default_index >= sets[kind].count) default_index = 0;
    return beer_message_dialog(title, text, sets[kind].ids, sets[kind].labels, sets[kind].count,
                               default_index, sets[kind].cancel);
}
