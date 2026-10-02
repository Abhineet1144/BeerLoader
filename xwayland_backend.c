#define _GNU_SOURCE
#include "xwayland_backend.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct _XDisplay Display;
typedef struct _XVisual Visual;
typedef struct _XGC *GC;
typedef struct _XImage XImage;
typedef unsigned long XID;
typedef XID Window;
typedef XID Atom;
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
    void *library;
    Display *display;
    Window window;
    Atom wm_delete;
    int x;
    int y;
    int width;
    int height;
    int visible;
    int initialized;
    pthread_mutex_t lock;

    int (*InitThreads)(void);
    Display *(*OpenDisplay)(const char *);
    int (*CloseDisplay)(Display *);
    Window (*DefaultRootWindow)(Display *);
    Window (*CreateSimpleWindow)(Display *, Window, int, int, unsigned int,
                                 unsigned int, unsigned int, unsigned long,
                                 unsigned long);
    int (*DestroyWindow)(Display *, Window);
    int (*MapRaised)(Display *, Window);
    int (*UnmapWindow)(Display *, Window);
    int (*MoveResizeWindow)(Display *, Window, int, int, unsigned int, unsigned int);
    int (*StoreName)(Display *, Window, const char *);
    Atom (*InternAtom)(Display *, const char *, Bool);
    int (*SendEvent)(Display *, Window, Bool, long, XEvent *);
    int (*SetWMProtocols)(Display *, Window, Atom *, int);
    int (*SelectInput)(Display *, Window, long);
    int (*Pending)(Display *);
    int (*NextEvent)(Display *, XEvent *);
    Visual *(*DefaultVisual)(Display *, int);
    int (*DefaultDepth)(Display *, int);
    GC (*DefaultGC)(Display *, int);
    XImage *(*CreateImage)(Display *, Visual *, unsigned int, int, int, char *,
                           unsigned int, unsigned int, int, int);
    int (*PutImage)(Display *, Window, GC, XImage *, int, int, int, int,
                    unsigned int, unsigned int);
    int (*DestroyImage)(XImage *);
    int (*Flush)(Display *);
} XwaylandBackend;

static XwaylandBackend g_x11 = { .lock = PTHREAD_MUTEX_INITIALIZER };

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
    X11_LOAD(MapRaised, "XMapRaised");
    X11_LOAD(UnmapWindow, "XUnmapWindow");
    X11_LOAD(MoveResizeWindow, "XMoveResizeWindow");
    X11_LOAD(StoreName, "XStoreName");
    X11_LOAD(InternAtom, "XInternAtom");
    X11_LOAD(SendEvent, "XSendEvent");
    X11_LOAD(SetWMProtocols, "XSetWMProtocols");
    X11_LOAD(SelectInput, "XSelectInput");
    X11_LOAD(Pending, "XPending");
    X11_LOAD(NextEvent, "XNextEvent");
    X11_LOAD(DefaultVisual, "XDefaultVisual");
    X11_LOAD(DefaultDepth, "XDefaultDepth");
    X11_LOAD(DefaultGC, "XDefaultGC");
    X11_LOAD(CreateImage, "XCreateImage");
    X11_LOAD(PutImage, "XPutImage");
    X11_LOAD(DestroyImage, "XDestroyImage");
    X11_LOAD(Flush, "XFlush");

    g_x11.InitThreads();
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
    g_x11.StoreName(g_x11.display, g_x11.window,
                    (title && *title) ? title : "Beer Windows Application");
    g_x11.wm_delete = g_x11.InternAtom(g_x11.display, "WM_DELETE_WINDOW", 0);
    if (g_x11.wm_delete)
        g_x11.SetWMProtocols(g_x11.display, g_x11.window, &g_x11.wm_delete, 1);
    /* StructureNotifyMask: enough for lifecycle notifications without consuming input. */
    g_x11.SelectInput(g_x11.display, g_x11.window, 1L << 17);
    if (initially_visible) {
        g_x11.MapRaised(g_x11.display, g_x11.window);
        g_x11.visible = 1;
    }
    g_x11.Flush(g_x11.display);
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
    g_x11.MoveResizeWindow(g_x11.display, g_x11.window, x, y,
                           (unsigned)width, (unsigned)height);
    g_x11.x = x; g_x11.y = y; g_x11.width = width; g_x11.height = height;
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

int xwayland_window_get_state(XwaylandWindowState *state)
{
    if (!state) return 0;
    pthread_mutex_lock(&g_x11.lock);
    int ok = g_x11.display && g_x11.window;
    if (ok) {
        state->x = g_x11.x; state->y = g_x11.y;
        state->width = g_x11.width; state->height = g_x11.height;
        state->visible = g_x11.visible;
    }
    pthread_mutex_unlock(&g_x11.lock);
    return ok;
}

int xwayland_window_pump_events(void)
{
    int close_requested = 0;
    pthread_mutex_lock(&g_x11.lock);
    if (g_x11.display && g_x11.window) {
        while (g_x11.Pending(g_x11.display) > 0) {
            XEvent event;
            memset(&event, 0, sizeof(event));
            g_x11.NextEvent(g_x11.display, &event);
            if (event.type == 33) { /* ClientMessage */
                XClientMessageEvent *client = (XClientMessageEvent *)&event;
                if ((Atom)client->data.l[0] == g_x11.wm_delete)
                    close_requested = 1;
            }
        }
    }
    pthread_mutex_unlock(&g_x11.lock);
    return close_requested;
}

int xwayland_window_present_rgba8(const uint8_t *pixels, int width, int height,
                                  int row_pitch)
{
    if (!pixels || width <= 0 || height <= 0 || row_pitch < width * 4)
        return 0;

    size_t size = (size_t)width * (size_t)height * 4;
    uint8_t *native = malloc(size);
    if (!native) return 0;

    /* The default 24-bit TrueColor XWayland visual stores pixels as B,G,R,x on
     * little-endian hosts. Keep alpha opaque because the top-level window has
     * no alpha channel. */
    for (int y = 0; y < height; ++y) {
        const uint8_t *src = pixels + (size_t)y * (size_t)row_pitch;
        uint8_t *dst = native + (size_t)y * (size_t)width * 4;
        for (int x = 0; x < width; ++x) {
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = 0xff;
        }
    }

    pthread_mutex_lock(&g_x11.lock);
    int ok = g_x11.display && g_x11.window;
    XImage *image = NULL;
    if (ok) {
        int screen = 0;
        image = g_x11.CreateImage(g_x11.display,
                                  g_x11.DefaultVisual(g_x11.display, screen),
                                  (unsigned)g_x11.DefaultDepth(g_x11.display, screen),
                                  2, 0, (char *)native, (unsigned)width,
                                  (unsigned)height, 32, width * 4);
        ok = image && g_x11.PutImage(g_x11.display, g_x11.window,
                                     g_x11.DefaultGC(g_x11.display, screen),
                                     image, 0, 0, 0, 0,
                                     (unsigned)width, (unsigned)height) == 0;
        if (ok) g_x11.Flush(g_x11.display);
    }
    if (image) {
        /* XDestroyImage owns and frees image->data. */
        g_x11.DestroyImage(image);
        native = NULL;
    }
    pthread_mutex_unlock(&g_x11.lock);
    free(native);
    return ok;
}

void xwayland_window_destroy(void)
{
    pthread_mutex_lock(&g_x11.lock);
    if (g_x11.display && g_x11.window) {
        g_x11.DestroyWindow(g_x11.display, g_x11.window);
        g_x11.Flush(g_x11.display);
        g_x11.window = 0;
        g_x11.visible = 0;
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
