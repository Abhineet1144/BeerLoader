#define _GNU_SOURCE
#include "xwayland_backend.h"
#include "window_icon_policy.h"
#include "../renderer/vulkan/vulkan_presenter.h"

/* Keep the platform backend independent of Vulkan SDK headers. */
int beer_vulkan_device_lost(void);

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct _XDisplay Display;
typedef struct _XVisual Visual;
typedef struct _XGC *GC;
typedef struct _XImage XImage;
typedef unsigned long XID;
typedef XID Window;
typedef XID Atom;
typedef XID Pixmap;
typedef XID Cursor;
typedef int Bool;

typedef union {
    int type;
    long pad[24];
} XEvent;

typedef struct {
    int type;
    unsigned long serial;
    Bool send_event;
    Display *display;
    Window window;
    Atom message_type;
    int format;
    union { char b[20]; short s[10]; long l[5]; } data;
} XClientMessageEvent;

typedef struct {
    int type;
    Display *display;
    unsigned long resourceid;
    unsigned long serial;
    unsigned char error_code;
    unsigned char request_code;
    unsigned char minor_code;
} XErrorEvent;

typedef struct {
    int type;
    unsigned long serial;
    Bool send_event;
    Display *display;
    Window event;
    Window window;
    int x;
    int y;
    int width;
    int height;
    int border_width;
    Window above;
    Bool override_redirect;
} XConfigureEvent;

typedef struct {
    long flags;
    int x, y;
    int width, height;
    int min_width, min_height;
    int max_width, max_height;
    int width_inc, height_inc;
    struct { int x, y; } min_aspect, max_aspect;
    int base_width, base_height;
    int win_gravity;
} XSizeHints;

typedef struct {
    unsigned long flags;
    unsigned long functions;
    unsigned long decorations;
    long input_mode;
    unsigned long status;
} MotifWmHints;

typedef struct {
    int type;
    unsigned long serial;
    Bool send_event;
    Display *display;
    Window window;
    Window root;
    Window subwindow;
    unsigned long time;
    int x;
    int y;
    int x_root;
    int y_root;
    unsigned int state;
    unsigned int keycode;
    Bool same_screen;
} XKeyEvent;

typedef struct {
    int type;
    unsigned long serial;
    Bool send_event;
    Display *display;
    Window window;
    Window root;
    Window subwindow;
    unsigned long time;
    int x;
    int y;
    int x_root;
    int y_root;
    unsigned int state;
    unsigned int button;
    Bool same_screen;
} XButtonEvent;

typedef XKeyEvent XMotionEvent;

#define BEER_DESKTOP_ICON_MAX_DIMENSION 64

typedef struct {
    void *library;
    Display *display;
    Window window;
    Atom wm_delete;
    int x;
    int y;
    int width;
    int height;
    /* Guest-visible client dimensions remain the swap-chain dimensions even
     * when XWayland scales the native fullscreen surface. */
    int client_width;
    int client_height;
    int visible;
    int viewable;          /* set only by MapNotify: XSetInputFocus on an unviewable window is a BadMatch */
    int focus_requested;
    int initialized;
    XImage *present_image;
    uint8_t *present_pixels;
    int present_width;
    int present_height;
    uint64_t source_hash;
    int source_width;
    int source_height;
    int source_row_pitch;
    int source_hash_valid;
    uint32_t *app_icon_pixels;
    int app_icon_width;
    int app_icon_height;
    char app_id[128];
    Cursor hidden_cursor;
    int cursor_visible;
    XwaylandPresenter presenter;
    pthread_mutex_t lock;

    int (*InitThreads)(void);
    Display *(*OpenDisplay)(const char *);
    int (*CloseDisplay)(Display *);
    Window (*DefaultRootWindow)(Display *);
    Window (*CreateSimpleWindow)(Display *, Window, int, int, unsigned int,
                                 unsigned int, unsigned int, unsigned long,
                                 unsigned long);
    int (*DestroyWindow)(Display *, Window);
    Pixmap (*CreateBitmapFromData)(Display *, Window, const char *, unsigned int,
                                   unsigned int);
    Cursor (*CreatePixmapCursor)(Display *, Pixmap, Pixmap, void *, void *,
                                 unsigned int, unsigned int);
    int (*FreePixmap)(Display *, Pixmap);
    int (*FreeCursor)(Display *, Cursor);
    int (*DefineCursor)(Display *, Window, Cursor);
    int (*UndefineCursor)(Display *, Window);
    int (*WarpPointer)(Display *, Window, Window, int, int, unsigned int,
                       unsigned int, int, int);
    int (*MapRaised)(Display *, Window);
    int (*UnmapWindow)(Display *, Window);
    int (*SetInputFocus)(Display *, Window, int, unsigned long);
    int (*RaiseWindow)(Display *, Window);
    int (*MoveResizeWindow)(Display *, Window, int, int, unsigned int, unsigned int);
    int (*StoreName)(Display *, Window, const char *);
    Atom (*InternAtom)(Display *, const char *, Bool);
    int (*ChangeProperty)(Display *, Window, Atom, Atom, int, int,
                          const unsigned char *, int);
    void (*SetWMNormalHints)(Display *, Window, XSizeHints *);
    int (*SendEvent)(Display *, Window, Bool, long, XEvent *);
    int (*SetWMProtocols)(Display *, Window, Atom *, int);
    int (*SelectInput)(Display *, Window, long);
    int (*Pending)(Display *);
    int (*NextEvent)(Display *, XEvent *);
    unsigned long (*LookupKeysym)(XKeyEvent *, int);
    Visual *(*DefaultVisual)(Display *, int);
    int (*DefaultDepth)(Display *, int);
    GC (*DefaultGC)(Display *, int);
    XImage *(*CreateImage)(Display *, Visual *, unsigned int, int, int, char *,
                           unsigned int, unsigned int, int, int);
    int (*PutImage)(Display *, Window, GC, XImage *, int, int, int, int,
                    unsigned int, unsigned int);
    int (*DestroyImage)(XImage *);
    int (*Flush)(Display *);
    int (*Sync)(Display *, Bool);
    int (*(*SetErrorHandler)(int (*)(Display *, XErrorEvent *)))(Display *, XErrorEvent *);
    int (*GrabPointer)(Display *, Window, Bool, unsigned int, int, int, Window,
                       Cursor, unsigned long);
    int (*UngrabPointer)(Display *, unsigned long);
    int pointer_confine_wanted;   /* ClipCursor active */
    int win_decorated;            /* last set_style */
    int focused;
    /* XInput2 raw motion */
    int xi_opcode;
    int xi_active;
    double xi_carry_x, xi_carry_y;
    int (*QueryExtension)(Display *, const char *, int *, int *, int *);
    int (*GetEventData)(Display *, void *);
    void (*FreeEventData)(Display *, void *);
    int (*XIQueryVersion)(Display *, int *, int *);
    int (*XISelectEvents)(Display *, Window, void *, int);
    int win_resizable;
    int fullscreen_applied;       /* _NET_WM_STATE_FULLSCREEN requested by move_resize */
    int cursor_requested_visible; /* what the guest asked for, before the fullscreen rule */
    int pointer_confined;         /* an XGrabPointer is currently held */
} XwaylandBackend;

static XwaylandBackend g_x11 = {
    .presenter = XWAYLAND_PRESENTER_AUTO,
    .lock = PTHREAD_MUTEX_INITIALIZER
};

const char *xwayland_presenter_name(XwaylandPresenter presenter)
{
    switch (presenter) {
    case XWAYLAND_PRESENTER_AUTO: return "auto";
    case XWAYLAND_PRESENTER_VULKAN: return "vulkan";
    case XWAYLAND_PRESENTER_X11: return "x11";
    default: return "unknown";
    }
}

int xwayland_set_presenter(XwaylandPresenter presenter)
{
    if (presenter < XWAYLAND_PRESENTER_AUTO || presenter > XWAYLAND_PRESENTER_X11)
        return 0;
    pthread_mutex_lock(&g_x11.lock);
    if (g_x11.display || g_x11.window || vulkan_presenter_is_active()) {
        pthread_mutex_unlock(&g_x11.lock);
        return 0;
    }
    g_x11.presenter = presenter;
    pthread_mutex_unlock(&g_x11.lock);
    return 1;
}

static int beer_x_error_handler(Display *display, XErrorEvent *error);
#define X11_LOAD(field, symbol) do { \
    *(void **)(&g_x11.field) = dlsym(g_x11.library, symbol); \
    if (!g_x11.field) { \
        fprintf(stderr, "[XWAYLAND] missing Xlib symbol %s\n", symbol); \
        goto fail; \
    } \
} while (0)

static int initialize_locked(void)
{
    if (g_x11.initialized) return g_x11.display != NULL;
    g_x11.initialized = 1;
    g_x11.library = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
    if (!g_x11.library) {
        fprintf(stderr, "[XWAYLAND] libX11.so.6 unavailable: %s\n", dlerror());
        return 0;
    }

    X11_LOAD(InitThreads, "XInitThreads");
    X11_LOAD(OpenDisplay, "XOpenDisplay");
    X11_LOAD(CloseDisplay, "XCloseDisplay");
    X11_LOAD(DefaultRootWindow, "XDefaultRootWindow");
    X11_LOAD(CreateSimpleWindow, "XCreateSimpleWindow");
    X11_LOAD(DestroyWindow, "XDestroyWindow");
    X11_LOAD(CreateBitmapFromData, "XCreateBitmapFromData");
    X11_LOAD(CreatePixmapCursor, "XCreatePixmapCursor");
    X11_LOAD(FreePixmap, "XFreePixmap");
    X11_LOAD(FreeCursor, "XFreeCursor");
    X11_LOAD(DefineCursor, "XDefineCursor");
    X11_LOAD(UndefineCursor, "XUndefineCursor");
    X11_LOAD(WarpPointer, "XWarpPointer");
    X11_LOAD(MapRaised, "XMapRaised");
    X11_LOAD(UnmapWindow, "XUnmapWindow");
    X11_LOAD(SetInputFocus, "XSetInputFocus");
    X11_LOAD(RaiseWindow, "XRaiseWindow");
    X11_LOAD(MoveResizeWindow, "XMoveResizeWindow");
    X11_LOAD(StoreName, "XStoreName");
    X11_LOAD(InternAtom, "XInternAtom");
    X11_LOAD(ChangeProperty, "XChangeProperty");
    X11_LOAD(SetWMNormalHints, "XSetWMNormalHints");
    X11_LOAD(SendEvent, "XSendEvent");
    X11_LOAD(SetWMProtocols, "XSetWMProtocols");
    X11_LOAD(SelectInput, "XSelectInput");
    X11_LOAD(Pending, "XPending");
    X11_LOAD(NextEvent, "XNextEvent");
    X11_LOAD(LookupKeysym, "XLookupKeysym");
    X11_LOAD(DefaultVisual, "XDefaultVisual");
    X11_LOAD(DefaultDepth, "XDefaultDepth");
    X11_LOAD(DefaultGC, "XDefaultGC");
    X11_LOAD(CreateImage, "XCreateImage");
    X11_LOAD(PutImage, "XPutImage");
    X11_LOAD(DestroyImage, "XDestroyImage");
    X11_LOAD(Flush, "XFlush");
    X11_LOAD(Sync, "XSync");
    X11_LOAD(SetErrorHandler, "XSetErrorHandler");
    X11_LOAD(GrabPointer, "XGrabPointer");
    X11_LOAD(QueryExtension, "XQueryExtension");
    X11_LOAD(GetEventData, "XGetEventData");
    X11_LOAD(FreeEventData, "XFreeEventData");
    X11_LOAD(UngrabPointer, "XUngrabPointer");

    g_x11.InitThreads();
    g_x11.SetErrorHandler(beer_x_error_handler);
    g_x11.display = g_x11.OpenDisplay(NULL);
    if (!g_x11.display) {
        fprintf(stderr, "[XWAYLAND] cannot open DISPLAY=%s\n",
                getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
        goto fail;
    }
    fprintf(stderr, "[XWAYLAND] connected to DISPLAY=%s\n",
            getenv("DISPLAY") ? getenv("DISPLAY") : "(default)");
    return 1;

fail:
    if (g_x11.display) g_x11.CloseDisplay(g_x11.display);
    g_x11.display = NULL;
    if (g_x11.library) dlclose(g_x11.library);
    g_x11.library = NULL;
    return 0;
}

/* XInput2 raw motion on the root window: relative deltas that do not stop at
 * the screen edge (XWayland feeds them from the Wayland relative-pointer). */
static void setup_xi_raw_motion_locked(void)
{
    if (g_x11.xi_active || !g_x11.display || !g_x11.QueryExtension ||
        getenv("BEER_NO_XI2"))
        return;
    void *xi = dlopen("libXi.so.6", RTLD_NOW | RTLD_LOCAL);
    if (!xi) return;
    g_x11.XIQueryVersion = (int (*)(Display *, int *, int *))dlsym(xi, "XIQueryVersion");
    g_x11.XISelectEvents = (int (*)(Display *, Window, void *, int))dlsym(xi, "XISelectEvents");
    if (!g_x11.XIQueryVersion || !g_x11.XISelectEvents || !g_x11.GetEventData ||
        !g_x11.FreeEventData)
        return;
    int opcode = 0, event_base = 0, error_base = 0;
    if (!g_x11.QueryExtension(g_x11.display, "XInputExtension", &opcode,
                              &event_base, &error_base))
        return;
    int major = 2, minor = 0;
    if (g_x11.XIQueryVersion(g_x11.display, &major, &minor) != 0) return;
    struct { int deviceid; int mask_len; unsigned char *mask; } event_mask;
    unsigned char bits[4] = { 0, 0, 0, 0 };
    bits[17 >> 3] |= (unsigned char)(1u << (17 & 7)); /* XI_RawMotion */
    event_mask.deviceid = 1;                          /* XIAllMasterDevices */
    event_mask.mask_len = sizeof(bits);
    event_mask.mask = bits;
    g_x11.XISelectEvents(g_x11.display, g_x11.DefaultRootWindow(g_x11.display),
                         &event_mask, 1);
    g_x11.Flush(g_x11.display);
    g_x11.xi_opcode = opcode;
    g_x11.xi_active = 1;
    fprintf(stderr, "[XWAYLAND] XInput2 %d.%d raw motion enabled\n", major, minor);
}

int xwayland_raw_motion_active(void) { return g_x11.xi_active; }

int xwayland_window_create(int x, int y, int width, int height,
                           const char *title, int initially_visible)
{
    pthread_mutex_lock(&g_x11.lock);
    if (!initialize_locked()) {
        pthread_mutex_unlock(&g_x11.lock);
        return 0;
    }
    if (g_x11.window) {
        pthread_mutex_unlock(&g_x11.lock);
        return 1;
    }

    if (width <= 0) width = 816;
    if (height <= 0) height = 488;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    Window root = g_x11.DefaultRootWindow(g_x11.display);
    g_x11.window = g_x11.CreateSimpleWindow(g_x11.display, root, x, y,
                                             (unsigned)width, (unsigned)height,
                                             0, 0, 0);
    if (!g_x11.window) {
        fprintf(stderr, "[XWAYLAND] XCreateSimpleWindow failed\n");
        pthread_mutex_unlock(&g_x11.lock);
        return 0;
    }

    g_x11.x = x;
    g_x11.y = y;
    g_x11.width = width;
    g_x11.height = height;
    g_x11.client_width = width;
    g_x11.client_height = height;
    g_x11.cursor_visible = 1;
    g_x11.StoreName(g_x11.display, g_x11.window,
                    (title && *title) ? title : "Beer Windows Application");
    if (g_x11.app_id[0]) {
        Atom wm_class = g_x11.InternAtom(g_x11.display, "WM_CLASS", 0);
        Atom string = g_x11.InternAtom(g_x11.display, "STRING", 0);
        Atom utf8 = g_x11.InternAtom(g_x11.display, "UTF8_STRING", 0);
        Atom gtk_application_id =
            g_x11.InternAtom(g_x11.display, "_GTK_APPLICATION_ID", 0);
        size_t length = strlen(g_x11.app_id);
        char class_value[sizeof(g_x11.app_id) * 2u + 2u];
        char desktop_id[sizeof(g_x11.app_id) + 6u];
        memcpy(class_value, g_x11.app_id, length + 1u);
        memcpy(class_value + length + 1u, g_x11.app_id, length + 1u);
        snprintf(desktop_id, sizeof(desktop_id), "beer-%s", g_x11.app_id);
        if (wm_class && string)
            g_x11.ChangeProperty(g_x11.display, g_x11.window,
                                 wm_class, string, 8, 0,
                                 (const unsigned char *)class_value,
                                 (int)(length * 2u + 2u));
        if (gtk_application_id && utf8)
            g_x11.ChangeProperty(g_x11.display, g_x11.window,
                                 gtk_application_id, utf8, 8, 0,
                                 (const unsigned char *)desktop_id,
                                 (int)strlen(desktop_id));
    }
    {
        Atom net_wm_pid = g_x11.InternAtom(g_x11.display, "_NET_WM_PID", 0);
        Atom cardinal = g_x11.InternAtom(g_x11.display, "CARDINAL", 0);
        unsigned long process_id = (unsigned long)(uint32_t)getpid();
        if (net_wm_pid && cardinal)
            g_x11.ChangeProperty(g_x11.display, g_x11.window,
                                 net_wm_pid, cardinal, 32, 0,
                                 (const unsigned char *)&process_id, 1);
    }
    g_x11.wm_delete = g_x11.InternAtom(g_x11.display, "WM_DELETE_WINDOW", 0);
    if (g_x11.wm_delete)
        g_x11.SetWMProtocols(g_x11.display, g_x11.window, &g_x11.wm_delete, 1);
    if (g_x11.app_icon_pixels && g_x11.app_icon_width > 0 &&
        g_x11.app_icon_height > 0) {
        Atom net_wm_icon = g_x11.InternAtom(g_x11.display, "_NET_WM_ICON", 0);
        Atom cardinal = g_x11.InternAtom(g_x11.display, "CARDINAL", 0);
        size_t count = (size_t)g_x11.app_icon_width *
                       (size_t)g_x11.app_icon_height;
        unsigned long *property = malloc((count + 2u) * sizeof(*property));
        if (property && net_wm_icon && cardinal) {
            property[0] = (unsigned long)(uint32_t)g_x11.app_icon_width;
            property[1] = (unsigned long)(uint32_t)g_x11.app_icon_height;
            for (size_t i = 0; i < count; ++i)
                property[i + 2u] = (unsigned long)g_x11.app_icon_pixels[i];
            g_x11.ChangeProperty(g_x11.display, g_x11.window,
                                 net_wm_icon, cardinal, 32, 0,
                                 (const unsigned char *)property,
                                 (int)(count + 2u));
        }
        free(property);
    }
    /* USER32 needs lifecycle, focus, configure and paint notifications. Keyboard
     * and pointer events remain owned by the DirectInput/input compatibility path. */
    g_x11.SelectInput(g_x11.display, g_x11.window,
                      (1L << 17) | /* StructureNotifyMask */
                      (1L << 21) | /* FocusChangeMask */
                      (1L << 15) | /* ExposureMask */
                      (1L << 0)  | /* KeyPressMask */
                      (1L << 1)  | /* KeyReleaseMask */
                      (1L << 2)  | /* ButtonPressMask */
                      (1L << 3)  | /* ButtonReleaseMask */
                      (1L << 6));  /* PointerMotionMask */
    if (initially_visible) {
        g_x11.MapRaised(g_x11.display, g_x11.window);
        g_x11.visible = 1;
    }
    g_x11.Flush(g_x11.display);
    if (initially_visible) {
        /* The map is asynchronous. Defer native focus until MapNotify so
         * XSetInputFocus never targets a still-unviewable XWayland window. */
        g_x11.focus_requested = 1;
        g_x11.RaiseWindow(g_x11.display, g_x11.window);
        g_x11.Flush(g_x11.display);
    }
    setup_xi_raw_motion_locked();
    fprintf(stderr, "[XWAYLAND] native window 0x%lx created (%dx%d)\n",
            g_x11.window, width, height);
    pthread_mutex_unlock(&g_x11.lock);
    return 1;
}

int xwayland_window_show(int visible)
{
    pthread_mutex_lock(&g_x11.lock);
    if (!g_x11.display || !g_x11.window) {
        pthread_mutex_unlock(&g_x11.lock);
        return 0;
    }
    if (visible) g_x11.MapRaised(g_x11.display, g_x11.window);
    else g_x11.UnmapWindow(g_x11.display, g_x11.window);
    g_x11.visible = !!visible;
    g_x11.Flush(g_x11.display);
    pthread_mutex_unlock(&g_x11.lock);
    return 1;
}

int xwayland_window_set_style(int decorated, int resizable)
{
    pthread_mutex_lock(&g_x11.lock);
    if (!g_x11.display || !g_x11.window) {
        pthread_mutex_unlock(&g_x11.lock);
        return 0;
    }

    Atom motif = g_x11.InternAtom(g_x11.display, "_MOTIF_WM_HINTS", 0);
    if (motif) {
        MotifWmHints hints;
        memset(&hints, 0, sizeof(hints));
        hints.flags = 1u << 1; /* MWM_HINTS_DECORATIONS */
        hints.decorations = decorated ? 1u : 0u;
        g_x11.ChangeProperty(g_x11.display, g_x11.window, motif, motif,
                             32, 0, (const unsigned char *)&hints,
                             (int)(sizeof(hints) / sizeof(long)));
    }

    g_x11.win_decorated = decorated;
    g_x11.win_resizable = resizable;
    XSizeHints size_hints;
    memset(&size_hints, 0, sizeof(size_hints));
    if (!resizable) {
        size_hints.flags = (1L << 4) | (1L << 5); /* PMinSize | PMaxSize */
        size_hints.min_width = size_hints.max_width = g_x11.width;
        size_hints.min_height = size_hints.max_height = g_x11.height;
    }
    g_x11.SetWMNormalHints(g_x11.display, g_x11.window, &size_hints);
    g_x11.Flush(g_x11.display);
    fprintf(stderr, "[XWAYLAND] window frame decorated=%d resizable=%d\n",
            !!decorated, !!resizable);
    pthread_mutex_unlock(&g_x11.lock);
    return 1;
}

int xwayland_window_set_fullscreen(int fullscreen)
{
    pthread_mutex_lock(&g_x11.lock);
    if (!g_x11.display || !g_x11.window) {
        pthread_mutex_unlock(&g_x11.lock);
        return 0;
    }

    Atom wm_state = g_x11.InternAtom(g_x11.display, "_NET_WM_STATE", 0);
    Atom wm_fullscreen = g_x11.InternAtom(
        g_x11.display, "_NET_WM_STATE_FULLSCREEN", 0);
    if (!wm_state || !wm_fullscreen) {
        pthread_mutex_unlock(&g_x11.lock);
        return 0;
    }

    XEvent event;
    memset(&event, 0, sizeof(event));
    XClientMessageEvent *client = (XClientMessageEvent *)&event;
    client->type = 33; /* ClientMessage */
    client->display = g_x11.display;
    client->window = g_x11.window;
    client->message_type = wm_state;
    client->format = 32;
    client->data.l[0] = fullscreen ? 1 : 0; /* _NET_WM_STATE_ADD/REMOVE */
    client->data.l[1] = (long)wm_fullscreen;
    client->data.l[3] = 1; /* normal application source */

    Window root = g_x11.DefaultRootWindow(g_x11.display);
    long mask = (1L << 20) | (1L << 19); /* SubstructureRedirect/Notify */
    int ok = g_x11.SendEvent(g_x11.display, root, 0, mask, &event) != 0;
    if (ok) g_x11.Flush(g_x11.display);
    pthread_mutex_unlock(&g_x11.lock);
    return ok;
}

static void screen_size_locked(int *out_w, int *out_h);
static int apply_cursor_locked(void);

int xwayland_window_move_resize(int x, int y, int width, int height,
                                int move, int resize)
{
    pthread_mutex_lock(&g_x11.lock);
    if (!g_x11.display || !g_x11.window) {
        pthread_mutex_unlock(&g_x11.lock);
        return 0;
    }
    if (!move) { x = g_x11.x; y = g_x11.y; }
    if (!resize) { width = g_x11.width; height = g_x11.height; }
    if (width <= 0 || height <= 0) {
        pthread_mutex_unlock(&g_x11.lock);
        return 0;
    }
    if (resize && !g_x11.win_resizable) {
        /* A fixed-size window pins min == max to the old size; the window
         * manager would ignore the new size, leaving a borderless "fullscreen"
         * window stuck at its initial 1080p extent. Re-pin to the new size. */
        XSizeHints size_hints;
        memset(&size_hints, 0, sizeof(size_hints));
        size_hints.flags = (1L << 4) | (1L << 5); /* PMinSize | PMaxSize */
        size_hints.min_width = size_hints.max_width = width;
        size_hints.min_height = size_hints.max_height = height;
        g_x11.SetWMNormalHints(g_x11.display, g_x11.window, &size_hints);
    }
    g_x11.MoveResizeWindow(g_x11.display, g_x11.window, x, y,
                           (unsigned)width, (unsigned)height);
    g_x11.x = x; g_x11.y = y; g_x11.width = width; g_x11.height = height;
    if (resize) {
        /* An undecorated window covering the whole desktop is a borderless
         * fullscreen window: ask the compositor for fullscreen so panels and
         * decorations do not constrain it. */
        int sw = 0, sh = 0;
        screen_size_locked(&sw, &sh);
        const int covers = !g_x11.win_decorated && sw > 0 && sh > 0 &&
                           width >= sw && height >= sh;
        if (covers != g_x11.fullscreen_applied) {
            Atom wm_state = g_x11.InternAtom(g_x11.display, "_NET_WM_STATE", 0);
            Atom wm_fs = g_x11.InternAtom(g_x11.display, "_NET_WM_STATE_FULLSCREEN", 0);
            if (wm_state && wm_fs) {
                XEvent event;
                memset(&event, 0, sizeof(event));
                XClientMessageEvent *client = (XClientMessageEvent *)&event;
                client->type = 33;
                client->display = g_x11.display;
                client->window = g_x11.window;
                client->message_type = wm_state;
                client->format = 32;
                client->data.l[0] = covers ? 1 : 0;
                client->data.l[1] = (long)wm_fs;
                client->data.l[3] = 1;
                Window root = g_x11.DefaultRootWindow(g_x11.display);
                g_x11.SendEvent(g_x11.display, root, 0,
                                (1L << 20) | (1L << 19), &event);
                g_x11.fullscreen_applied = covers;
                apply_cursor_locked();
                fprintf(stderr, "[XWAYLAND] borderless %dx%d covers desktop %dx%d -> fullscreen=%d\n",
                        width, height, sw, sh, covers);
            }
        }
    }
    if (resize) {
        g_x11.client_width = width;
        g_x11.client_height = height;
    }
    g_x11.Flush(g_x11.display);
    pthread_mutex_unlock(&g_x11.lock);
    return 1;
}

int xwayland_window_set_title(const char *title)
{
    pthread_mutex_lock(&g_x11.lock);
    int ok = g_x11.display && g_x11.window && title;
    if (ok) {
        g_x11.StoreName(g_x11.display, g_x11.window, title);
        g_x11.Flush(g_x11.display);
    }
    pthread_mutex_unlock(&g_x11.lock);
    return ok;
}

int xwayland_window_set_app_id(const char *app_id)
{
    if (!app_id || !*app_id) return 0;
    size_t length = strnlen(app_id, sizeof(g_x11.app_id));
    if (!length || length >= sizeof(g_x11.app_id)) return 0;
    pthread_mutex_lock(&g_x11.lock);
    memcpy(g_x11.app_id, app_id, length + 1u);
    pthread_mutex_unlock(&g_x11.lock);
    return 1;
}

int xwayland_window_set_icon_argb(const uint32_t *pixels, int width, int height)
{
    if (!pixels || width <= 0 || height <= 0) return 0;

    int published_width = width;
    int published_height = height;
    if (published_width > BEER_DESKTOP_ICON_MAX_DIMENSION ||
        published_height > BEER_DESKTOP_ICON_MAX_DIMENSION) {
        double scale_x = (double)BEER_DESKTOP_ICON_MAX_DIMENSION / published_width;
        double scale_y = (double)BEER_DESKTOP_ICON_MAX_DIMENSION / published_height;
        double scale = scale_x < scale_y ? scale_x : scale_y;
        published_width = (int)(published_width * scale);
        published_height = (int)(published_height * scale);
        if (published_width < 1) published_width = 1;
        if (published_height < 1) published_height = 1;
    }
    if ((size_t)published_width > SIZE_MAX / sizeof(uint32_t) /
                                      (size_t)published_height)
        return 0;

    size_t pixel_count = (size_t)published_width * (size_t)published_height;
    uint32_t *cached = malloc(pixel_count * sizeof(*cached));
    if (!cached) return 0;
    if (!beer_icon_scale_argb(pixels, width, height, cached,
                              published_width, published_height)) {
        free(cached);
        return 0;
    }

    pthread_mutex_lock(&g_x11.lock);
    free(g_x11.app_icon_pixels);
    g_x11.app_icon_pixels = cached;
    g_x11.app_icon_width = published_width;
    g_x11.app_icon_height = published_height;
    int ok = 1;
    if (g_x11.display && g_x11.window) {
        Atom net_wm_icon = g_x11.InternAtom(g_x11.display, "_NET_WM_ICON", 0);
        Atom cardinal = g_x11.InternAtom(g_x11.display, "CARDINAL", 0);
        ok = net_wm_icon && cardinal;
        if (ok) {
            /* Xlib represents format-32 property elements as host longs even
             * on LP64. Passing the uint32_t cache directly makes XChangeProperty
             * stride through pairs of pixels and can replace a valid pre-map
             * icon with malformed/empty data after WM_SETICON. */
            unsigned long *property =
                malloc((pixel_count + 2u) * sizeof(*property));
            ok = property != NULL;
            if (ok) {
                property[0] = (unsigned long)(uint32_t)published_width;
                property[1] = (unsigned long)(uint32_t)published_height;
                for (size_t i = 0; i < pixel_count; ++i)
                    property[i + 2u] = (unsigned long)cached[i];
                g_x11.ChangeProperty(g_x11.display, g_x11.window,
                                     net_wm_icon, cardinal, 32, 0,
                                     (const unsigned char *)property,
                                     (int)(pixel_count + 2u));
                g_x11.Flush(g_x11.display);
                free(property);
            }
        }
    }
    pthread_mutex_unlock(&g_x11.lock);
    return ok;
}

/* Xlib's default error handler terminates the process. Focus/raise requests race with the compositor
 * (a window can become unviewable between request and delivery) and are best effort, so report and
 * continue instead of killing the guest. */
static int beer_x_error_handler(Display *display, XErrorEvent *error)
{
    (void)display;
    fprintf(stderr, "[XWAYLAND] ignored X error: code=%d request=%d minor=%d resource=0x%lx\n",
            error->error_code, error->request_code, error->minor_code, error->resourceid);
    return 0;
}

static int request_focus_locked(void)
{
    if (!g_x11.display || !g_x11.window) return 0;

    /* XWayland is managed by the Wayland compositor. XSetInputFocus alone is
     * commonly ignored (or targets a mapped-but-inactive X11 surface), while
     * USER32 has already told the guest it owns focus. Ask the window manager
     * to activate the surface through EWMH first, then synchronize X focus. */
    Atom active = g_x11.InternAtom(g_x11.display, "_NET_ACTIVE_WINDOW", 0);
    Window root = g_x11.DefaultRootWindow(g_x11.display);
    if (active && root) {
        XClientMessageEvent message;
        memset(&message, 0, sizeof(message));
        message.type = 33; /* ClientMessage */
        message.display = g_x11.display;
        message.window = g_x11.window;
        message.message_type = active;
        message.format = 32;
        message.data.l[0] = 1; /* application-originated activation */
        message.data.l[1] = 0; /* CurrentTime */
        g_x11.SendEvent(g_x11.display, root, 0,
                        (1L << 19) | (1L << 20), (XEvent *)&message);
    }
    g_x11.RaiseWindow(g_x11.display, g_x11.window);
    int ok = g_x11.SetInputFocus(g_x11.display, g_x11.window, 1, 0) != 0;
    g_x11.Flush(g_x11.display);
    return ok;
}

int xwayland_window_request_focus(void)
{
    pthread_mutex_lock(&g_x11.lock);
    int ok = g_x11.display && g_x11.window;
    if (ok) {
        g_x11.focus_requested = 1;
        /* XMapWindow is asynchronous. Apply focus immediately only after a
         * MapNotify has made the window viewable; otherwise poll_event will
         * fulfill this request when that notification arrives. */
        if (g_x11.viewable && request_focus_locked())
            g_x11.focus_requested = 0;
    }
    pthread_mutex_unlock(&g_x11.lock);
    return ok;
}

static void apply_pointer_confine_locked(void);

/* Applies the effective host cursor state: the guest's request, except that a
 * fullscreen (desktop-covering) window never shows the host cursor because the
 * games that run this way draw their own. BEER_HOST_CURSOR=1 disables that
 * rule. Called with the lock held. */
static int apply_cursor_locked(void)
{
    int visible = g_x11.cursor_requested_visible;
    if (g_x11.fullscreen_applied && !getenv("BEER_HOST_CURSOR")) visible = 0;
    int ok = g_x11.display && g_x11.window;
    if (ok && !!visible != g_x11.cursor_visible) {
        if (visible) {
            ok = g_x11.UndefineCursor(g_x11.display, g_x11.window) != 0;
        } else {
            if (!g_x11.hidden_cursor) {
                static const char empty[1] = {0};
                struct {
                    unsigned long pixel;
                    unsigned short red, green, blue;
                    char flags, pad;
                } color;
                memset(&color, 0, sizeof(color));
                Pixmap bitmap = g_x11.CreateBitmapFromData(
                    g_x11.display, g_x11.window, empty, 1, 1);
                if (bitmap) {
                    g_x11.hidden_cursor = g_x11.CreatePixmapCursor(
                        g_x11.display, bitmap, bitmap, &color, &color, 0, 0);
                    g_x11.FreePixmap(g_x11.display, bitmap);
                }
            }
            ok = g_x11.hidden_cursor &&
                 g_x11.DefineCursor(g_x11.display, g_x11.window,
                                    g_x11.hidden_cursor) != 0;
        }
        if (ok) {
            g_x11.cursor_visible = !!visible;
            apply_pointer_confine_locked();
            g_x11.Flush(g_x11.display);
        }
    }
    return ok;
}

int xwayland_window_set_cursor_visible(int visible)
{
    pthread_mutex_lock(&g_x11.lock);
    g_x11.cursor_requested_visible = !!visible;
    const int ok = apply_cursor_locked();
    pthread_mutex_unlock(&g_x11.lock);
    return ok;
}

int xwayland_window_set_cursor_position(int root_x, int root_y)
{
    pthread_mutex_lock(&g_x11.lock);
    int ok = g_x11.display && g_x11.window;
    if (ok) {
        Window root = g_x11.DefaultRootWindow(g_x11.display);
        ok = root && g_x11.WarpPointer(g_x11.display, 0, root, 0, 0, 0, 0,
                                      root_x, root_y) != 0;
        if (ok) g_x11.Flush(g_x11.display);
    }
    pthread_mutex_unlock(&g_x11.lock);
    return ok;
}

/* Grab the pointer and confine it to the window (ClipCursor). XWayland maps a
 * confining grab to a Wayland confined/locked pointer, and with a hidden cursor
 * it emulates pointer lock for XWarpPointer-based mouse look. Called with the
 * lock held. */
static void apply_pointer_confine_locked(void)
{
    if (!g_x11.display || !g_x11.window || !g_x11.GrabPointer) return;
    /* A hidden cursor means the game draws its own and reads relative motion,
     * so the host pointer must stay inside the window just like under
     * ClipCursor; otherwise it escapes to the desktop at the window edge. */
    const int want = g_x11.pointer_confine_wanted || !g_x11.cursor_visible;
    if (want && !g_x11.pointer_confined && g_x11.visible) {
        const unsigned int mask = (1u << 2) | (1u << 3) | (1u << 6) | (1u << 13); /* press, release, motion, ButtonMotion */
        const int result = g_x11.GrabPointer(g_x11.display, g_x11.window, 1,
                                             mask, 1, 1, g_x11.window, 0, 0);
        g_x11.pointer_confined = result == 0;
        g_x11.Flush(g_x11.display);
        if (getenv("BEER_INPUT_DIAGNOSTICS"))
            fprintf(stderr, "[INPUT] XGrabPointer(confine) -> %d\n", result);
    } else if ((!want || !g_x11.visible) && g_x11.pointer_confined) {
        g_x11.UngrabPointer(g_x11.display, 0);
        g_x11.pointer_confined = 0;
        g_x11.Flush(g_x11.display);
        if (getenv("BEER_INPUT_DIAGNOSTICS"))
            fprintf(stderr, "[INPUT] XUngrabPointer\n");
    }
}

int xwayland_window_set_pointer_confine(int enable)
{
    pthread_mutex_lock(&g_x11.lock);
    const int changed = g_x11.pointer_confine_wanted != (enable != 0);
    g_x11.pointer_confine_wanted = enable != 0;
    if (changed || (enable && !g_x11.pointer_confined))
        apply_pointer_confine_locked();
    const int ok = g_x11.display && g_x11.window;
    pthread_mutex_unlock(&g_x11.lock);
    return ok;
}

int xwayland_window_set_client_rect(int client_x, int client_y,
                                    int client_width, int client_height)
{
    pthread_mutex_lock(&g_x11.lock);
    int ok = g_x11.display && g_x11.window;
    if (ok && client_x == 0 && client_y == 0 &&
        client_width > 0 && client_height > 0) {
        /* X11 drawable and input coordinates are already client-relative.
         * Non-client metrics affect only USER32's outer rectangle; they never
         * define a sub-rectangle inside a swapchain image. */
        g_x11.client_width = client_width;
        g_x11.client_height = client_height;
    }
    pthread_mutex_unlock(&g_x11.lock);
    return ok;
}

int xwayland_window_get_state(XwaylandWindowState *state)
{
    if (!state) return 0;
    pthread_mutex_lock(&g_x11.lock);
    int ok = g_x11.display && g_x11.window;
    if (ok) {
        state->x = g_x11.x; state->y = g_x11.y;
        state->width = g_x11.width; state->height = g_x11.height;
        state->client_width = g_x11.client_width > 0
            ? g_x11.client_width : g_x11.width;
        state->client_height = g_x11.client_height > 0
            ? g_x11.client_height : g_x11.height;
        state->visible = g_x11.visible;
    }
    pthread_mutex_unlock(&g_x11.lock);
    return ok;
}

static void screen_size_locked(int *out_w, int *out_h)
{
    int w = 0, h = 0;
    if (g_x11.display && g_x11.library) {
        typedef int (*XDisplayDim)(Display *, int);
        XDisplayDim get_w = (XDisplayDim)dlsym(g_x11.library, "XDisplayWidth");
        XDisplayDim get_h = (XDisplayDim)dlsym(g_x11.library, "XDisplayHeight");
        if (get_w && get_h) { w = get_w(g_x11.display, 0); h = get_h(g_x11.display, 0); }
    }
    *out_w = w; *out_h = h;
}

void xwayland_get_screen_size(int *out_width, int *out_height)
{
    int w = 1920, h = 1080;
    pthread_mutex_lock(&g_x11.lock);
    /* DXGI and USER32 query the desktop before any window exists; connect now
     * (the window creation path reuses this connection). */
    if (!g_x11.initialized) initialize_locked();
    if (g_x11.display) {
        /* XDefaultScreen returns the index of the default screen.
         * WidthOfScreen / HeightOfScreen are macros over the Screen struct,
         * so we read directly through the display structure offset — but the
         * portable path is XDisplayWidth / XDisplayHeight which are loaded
         * from libX11 if available.  We use them here via a local dlsym so we
         * do not need a new field in XwaylandBackend. */
        typedef int (*XDisplayDim)(Display *, int);
        XDisplayDim get_w = (XDisplayDim)dlsym(g_x11.library, "XDisplayWidth");
        XDisplayDim get_h = (XDisplayDim)dlsym(g_x11.library, "XDisplayHeight");
        if (get_w && get_h) {
            /* screen index 0 is the default screen */
            int sw = get_w(g_x11.display, 0);
            int sh = get_h(g_x11.display, 0);
            if (sw > 0) w = sw;
            if (sh > 0) h = sh;
        }
    }
    pthread_mutex_unlock(&g_x11.lock);
    if (out_width)  *out_width  = w;
    if (out_height) *out_height = h;
}

/* Current refresh rate of the default screen in Hz (XRandR), 60 if unknown. */
int xwayland_get_screen_refresh(void)
{
    int hz = 60;
    pthread_mutex_lock(&g_x11.lock);
    if (!g_x11.initialized) initialize_locked();
    if (g_x11.display) {
        void *xrr = dlopen("libXrandr.so.2", RTLD_NOW | RTLD_LOCAL);
        if (xrr) {
            typedef void *(*GetInfo)(Display *, Window);
            typedef short (*CurRate)(void *);
            typedef void (*Free)(void *);
            GetInfo get_info = (GetInfo)dlsym(xrr, "XRRGetScreenInfo");
            CurRate cur_rate = (CurRate)dlsym(xrr, "XRRConfigCurrentRate");
            Free free_info = (Free)dlsym(xrr, "XRRFreeScreenConfigInfo");
            if (get_info && cur_rate && free_info) {
                void *info = get_info(g_x11.display,
                                      g_x11.DefaultRootWindow(g_x11.display));
                if (info) {
                    short rate = cur_rate(info);
                    if (rate >= 24 && rate <= 1000) hz = rate;
                    free_info(info);
                }
            }
            dlclose(xrr);
        }
    }
    pthread_mutex_unlock(&g_x11.lock);
    return hz;
}

int xwayland_window_poll_event(XwaylandEvent *out)
{
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&g_x11.lock);
    if (!g_x11.display || !g_x11.window || g_x11.Pending(g_x11.display) <= 0) {
        pthread_mutex_unlock(&g_x11.lock);
        return 0;
    }
    XEvent event;
    memset(&event, 0, sizeof(event));
    g_x11.NextEvent(g_x11.display, &event);
    switch (event.type) {
        case 2:
        case 3: {
            XKeyEvent *key = (XKeyEvent *)&event;
            out->type = event.type == 2 ? XWAYLAND_EVENT_KEY_DOWN : XWAYLAND_EVENT_KEY_UP;
            out->x = key->x; out->y = key->y;
            out->root_x = key->x_root; out->root_y = key->y_root;
            out->keycode = key->keycode;
            out->keysym = g_x11.LookupKeysym(key, 0);
            out->state = key->state;
            break;
        }
        case 4:
        case 5: {
            XButtonEvent *button = (XButtonEvent *)&event;
            out->type = event.type == 4 ? XWAYLAND_EVENT_BUTTON_DOWN : XWAYLAND_EVENT_BUTTON_UP;
            out->x = button->x; out->y = button->y;
            out->root_x = button->x_root; out->root_y = button->y_root;
            out->button = button->button;
            out->state = button->state;
            break;
        }
        case 6: {
            XMotionEvent *motion = (XMotionEvent *)&event;
            out->type = XWAYLAND_EVENT_POINTER_MOTION;
            out->x = motion->x; out->y = motion->y;
            out->root_x = motion->x_root; out->root_y = motion->y_root;
            out->state = motion->state;
            break;
        }
        case 35: { /* GenericEvent: XInput2 raw motion */
            struct {
                int type; unsigned long serial; int send_event; Display *display;
                int extension; int evtype; unsigned long time; int deviceid;
                int sourceid; int detail; int flags;
                struct { int mask_len; unsigned char *mask; double *values; } valuators;
                double *raw_values;
            } *xi;
            XEvent *ev = &event;
            struct { int type; unsigned long serial; int send_event; Display *display;
                     int extension; int evtype; unsigned int cookie; void *data; } *cookie =
                (void *)ev;
            if (g_x11.xi_active && cookie->extension == g_x11.xi_opcode &&
                g_x11.GetEventData(g_x11.display, cookie) && cookie->data) {
                xi = cookie->data;
                if (xi->evtype == 17 && xi->valuators.mask && xi->raw_values) {
                    double dx = 0, dy = 0;
                    int index = 0;
                    for (int bit = 0; bit < xi->valuators.mask_len * 8 && bit < 2; ++bit) {
                        if (!(xi->valuators.mask[bit >> 3] & (1u << (bit & 7)))) continue;
                        if (bit == 0) dx = xi->raw_values[index];
                        else dy = xi->raw_values[index];
                        ++index;
                    }
                    g_x11.xi_carry_x += dx;
                    g_x11.xi_carry_y += dy;
                    const int ix = (int)g_x11.xi_carry_x, iy = (int)g_x11.xi_carry_y;
                    g_x11.xi_carry_x -= ix;
                    g_x11.xi_carry_y -= iy;
                    if (ix || iy) {
                        out->type = XWAYLAND_EVENT_RAW_MOTION;
                        out->x = ix;
                        out->y = iy;
                    }
                }
                g_x11.FreeEventData(g_x11.display, cookie);
            }
            break;
        }
        case 9:
            g_x11.focused = 1;
            out->type = XWAYLAND_EVENT_FOCUS_IN;
            apply_pointer_confine_locked();   /* re-confine after refocus */
            break;
        case 10:
            g_x11.focused = 0;
            out->type = XWAYLAND_EVENT_FOCUS_OUT;
            if (g_x11.pointer_confined) {      /* never keep a grab while unfocused */
                g_x11.UngrabPointer(g_x11.display, 0);
                g_x11.pointer_confined = 0;
            }
            break;
        case 12:
            g_x11.source_hash_valid = 0;
            out->type = XWAYLAND_EVENT_EXPOSE;
            break;
        case 18: g_x11.visible = 0; g_x11.viewable = 0; out->type = XWAYLAND_EVENT_HIDE; break;
        case 19:
            g_x11.visible = 1;
            g_x11.viewable = 1;
            g_x11.source_hash_valid = 0;
            if (g_x11.focus_requested && request_focus_locked())
                g_x11.focus_requested = 0;
            apply_pointer_confine_locked();
            out->type = XWAYLAND_EVENT_SHOW;
            break;
        case 22: {
            XConfigureEvent *configure = (XConfigureEvent *)&event;
            g_x11.x = configure->x; g_x11.y = configure->y;
            g_x11.width = configure->width; g_x11.height = configure->height;
            /* An X11 ConfigureNotify changes the drawable's client extent.
             * Reflect that same extent through GetClientRect, WM_SIZE, and
             * pointer mapping. Keeping the pre-fullscreen requested size here
             * made USER32 report 2560x1440 while the drawable and WM_SIZE were
             * 1920x1080, scaling absolute pointer positions by 4/3. */
            g_x11.client_width = configure->width;
            g_x11.client_height = configure->height;
            out->type = XWAYLAND_EVENT_CONFIGURE;
            out->x = configure->x; out->y = configure->y;
            out->width = configure->width; out->height = configure->height;
            break;
        }
        case 33: {
            XClientMessageEvent *client = (XClientMessageEvent *)&event;
            if ((Atom)client->data.l[0] == g_x11.wm_delete)
                out->type = XWAYLAND_EVENT_CLOSE;
            break;
        }
        default: break;
    }
    pthread_mutex_unlock(&g_x11.lock);
    return out->type != XWAYLAND_EVENT_NONE;
}

int xwayland_window_pump_events(void)
{
    int close_requested = 0;
    XwaylandEvent event;
    while (xwayland_window_poll_event(&event))
        close_requested |= event.type == XWAYLAND_EVENT_CLOSE;
    return close_requested;
}

static int present_rgba8_internal(const void *resource, uint64_t serial,
                                  const uint8_t *pixels, int width, int height,
                                  int row_pitch)
{
    if (!pixels || width <= 0 || height <= 0 || row_pitch < width * 4)
        return 0;

    pthread_mutex_lock(&g_x11.lock);
    int ok = g_x11.display && g_x11.window;
    /* A swapchain image is already client content. Win32 non-client metrics
     * affect only the outer window rectangle and must never crop source pixels.
     * Scale the complete image to the native X11 client drawable. */
    int output_width = g_x11.width > 0 ? g_x11.width : width;
    int output_height = g_x11.height > 0 ? g_x11.height : height;
    if (!ok || output_width <= 0 || output_height <= 0 ||
        (size_t)output_width > SIZE_MAX / 4u / (size_t)output_height) {
        pthread_mutex_unlock(&g_x11.lock);
        return 0;
    }

    /* Prefer Vulkan for final presentation. The D3D11 compatibility renderer
     * remains CPU-backed; Vulkan owns only the native swapchain upload/present.
     * This gives overlays such as MangoHud a real Vulkan queue to observe.
     *
     * The CPU-X11 correctness path is intentionally stronger than Vulkan's
     * staging option: it bypasses the Vulkan upload image, blit, swapchain, and
     * compositor synchronization entirely. This makes it possible to prove
     * whether a visible frame differs from the synchronized D3D12 pixels. */
    const int force_cpu_x11 = getenv("BEER_PRESENTER_CPU_X11") != NULL;
    if (!force_cpu_x11 && g_x11.presenter != XWAYLAND_PRESENTER_X11) {
        if (vulkan_presenter_present_resource_rgba8(
                g_x11.display, g_x11.window, resource, serial, pixels,
                width, height, row_pitch, output_width, output_height)) {
            pthread_mutex_unlock(&g_x11.lock);
            return 1;
        }
        if (g_x11.presenter == XWAYLAND_PRESENTER_VULKAN) {
            static int strict_failure_announced;
            if (!strict_failure_announced) {
                fprintf(stderr,
                        "[PRESENTER] strict Vulkan presentation failed%s; "
                        "use --presenter=auto for X11 fallback\n",
                        beer_vulkan_device_lost() ? " after device loss" : "");
                strict_failure_announced = 1;
            }
            pthread_mutex_unlock(&g_x11.lock);
            return 0;
        }
    }
    if (force_cpu_x11) {
        static int announced;
        if (!announced) {
            fprintf(stderr,
                    "[PRESENTER] source=synchronized-d3d12 path=cpu-x11\n");
            announced = 1;
        }
    }

    /* Avoid re-scaling and uploading byte-identical frames. Present still
     * succeeds and guest timing remains unchanged; XWayland retains the last
     * image until a new frame or an expose event requires repainting. */
    uint64_t source_hash = 1469598103934665603ULL;
    for (int y = 0; y < height; ++y) {
        const uint8_t *row = pixels + (size_t)y * (size_t)row_pitch;
        for (int x = 0; x < width * 4; ++x) {
            source_hash ^= row[x];
            source_hash *= 1099511628211ULL;
        }
    }
    if (g_x11.source_hash_valid && g_x11.source_hash == source_hash &&
        g_x11.source_width == width && g_x11.source_height == height &&
        g_x11.source_row_pitch == row_pitch &&
        g_x11.present_width == output_width &&
        g_x11.present_height == output_height) {
        pthread_mutex_unlock(&g_x11.lock);
        return 1;
    }

    size_t size = (size_t)output_width * (size_t)output_height * 4;
    if (!g_x11.present_image || g_x11.present_width != output_width ||
        g_x11.present_height != output_height) {
        if (g_x11.present_image) {
            g_x11.DestroyImage(g_x11.present_image);
            g_x11.present_image = NULL;
            g_x11.present_pixels = NULL;
        }
        uint8_t *storage = malloc(size);
        if (!storage) {
            pthread_mutex_unlock(&g_x11.lock);
            return 0;
        }
        int screen = 0;
        g_x11.present_image = g_x11.CreateImage(
            g_x11.display, g_x11.DefaultVisual(g_x11.display, screen),
            (unsigned)g_x11.DefaultDepth(g_x11.display, screen), 2, 0,
            (char *)storage, (unsigned)output_width, (unsigned)output_height,
            32, output_width * 4);
        if (!g_x11.present_image) {
            free(storage);
            pthread_mutex_unlock(&g_x11.lock);
            return 0;
        }
        g_x11.present_pixels = storage;
        g_x11.present_width = output_width;
        g_x11.present_height = output_height;
    }

    /* Scale and convert in one pass. Precompute horizontal source offsets once
     * per output size; division in the inner 3.7-million-pixel loop was a major
     * avoidable cost on every Present. */
    static int *source_x_offsets;
    static int source_x_width;
    if (source_x_width != output_width) {
        int *offsets = realloc(source_x_offsets, (size_t)output_width * sizeof(*offsets));
        if (!offsets) {
            pthread_mutex_unlock(&g_x11.lock);
            return 0;
        }
        source_x_offsets = offsets;
        source_x_width = output_width;
        for (int x = 0; x < output_width; ++x)
            source_x_offsets[x] = (int)((uint64_t)(unsigned)x * (unsigned)width /
                                        (unsigned)output_width) * 4;
    }
    for (int y = 0; y < output_height; ++y) {
        int source_y = (int)((uint64_t)(unsigned)y * (unsigned)height /
                             (unsigned)output_height);
        const uint8_t *source_row = pixels + (size_t)source_y * (size_t)row_pitch;
        uint8_t *destination_row = g_x11.present_pixels +
            (size_t)y * (size_t)output_width * 4;
        for (int x = 0; x < output_width; ++x) {
            const uint8_t *src = source_row + source_x_offsets[x];
            uint8_t *dst = destination_row + (size_t)x * 4;
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = 0xff;
        }
    }

    int screen = 0;
    ok = g_x11.PutImage(g_x11.display, g_x11.window,
                        g_x11.DefaultGC(g_x11.display, screen),
                        g_x11.present_image, 0, 0, 0, 0,
                        (unsigned)output_width, (unsigned)output_height) == 0;
    /* Flush submits the image without a round trip. Presentation diagnostics
     * describe the submitted frame; callers needing a screenshot can opt into
     * the back-buffer capture path without stalling every guest Present. */
    if (ok) {
        g_x11.Flush(g_x11.display);
        g_x11.source_hash = source_hash;
        g_x11.source_width = width;
        g_x11.source_height = height;
        g_x11.source_row_pitch = row_pitch;
        g_x11.source_hash_valid = 1;
    }
    pthread_mutex_unlock(&g_x11.lock);
    return ok;
}

int xwayland_window_present_rgba8(const uint8_t *pixels, int width, int height,
                                  int row_pitch)
{
    return present_rgba8_internal(NULL, 0, pixels, width, height, row_pitch);
}

int xwayland_window_present_resource_rgba8(const void *resource, uint64_t serial,
                                           const uint8_t *pixels, int width,
                                           int height, int row_pitch)
{
    return present_rgba8_internal(resource, serial, pixels,
                                  width, height, row_pitch);
}

int xwayland_window_present_image(uint64_t image, uint32_t layout, int width,
                                  int height, int direct_blit)
{
    if (width <= 0 || height <= 0) return 0;
    pthread_mutex_lock(&g_x11.lock);
    int ok = g_x11.display && g_x11.window && g_x11.presenter != XWAYLAND_PRESENTER_X11;
    int output_width = g_x11.width > 0 ? g_x11.width : width;
    int output_height = g_x11.height > 0 ? g_x11.height : height;
    int result = ok && vulkan_presenter_present_image(
        g_x11.display, g_x11.window, image, layout, width, height,
        direct_blit, output_width, output_height);
    pthread_mutex_unlock(&g_x11.lock);
    return result;
}

void xwayland_window_destroy(void)
{
    pthread_mutex_lock(&g_x11.lock);
    vulkan_presenter_destroy();
    if (g_x11.display && g_x11.window) {
        if (g_x11.hidden_cursor) {
            g_x11.FreeCursor(g_x11.display, g_x11.hidden_cursor);
            g_x11.hidden_cursor = 0;
        }
        g_x11.DestroyWindow(g_x11.display, g_x11.window);
        g_x11.Flush(g_x11.display);
        g_x11.window = 0;
        g_x11.visible = 0;
        g_x11.cursor_visible = 1;
    }
    if (g_x11.present_image) {
        g_x11.DestroyImage(g_x11.present_image);
        g_x11.present_image = NULL;
        g_x11.present_pixels = NULL;
        g_x11.present_width = 0;
        g_x11.present_height = 0;
        g_x11.source_hash_valid = 0;
    }
    pthread_mutex_unlock(&g_x11.lock);
}

int xwayland_window_exists(void)
{
    pthread_mutex_lock(&g_x11.lock);
    int exists = g_x11.display && g_x11.window;
    pthread_mutex_unlock(&g_x11.lock);
    return exists;
}
