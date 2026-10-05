#define _GNU_SOURCE
#include "xwayland_backend.h"
#include "../renderer/vulkan/vulkan_presenter.h"

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
    X11_LOAD(LookupKeysym, "XLookupKeysym");
    X11_LOAD(DefaultVisual, "XDefaultVisual");
    X11_LOAD(DefaultDepth, "XDefaultDepth");
    X11_LOAD(DefaultGC, "XDefaultGC");
    X11_LOAD(CreateImage, "XCreateImage");
    X11_LOAD(PutImage, "XPutImage");
    X11_LOAD(DestroyImage, "XDestroyImage");
    X11_LOAD(Flush, "XFlush");
    X11_LOAD(Sync, "XSync");

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
    g_x11.client_width = width;
    g_x11.client_height = height;
    g_x11.StoreName(g_x11.display, g_x11.window,
                    (title && *title) ? title : "Beer Windows Application");
    g_x11.wm_delete = g_x11.InternAtom(g_x11.display, "WM_DELETE_WINDOW", 0);
    if (g_x11.wm_delete)
        g_x11.SetWMProtocols(g_x11.display, g_x11.window, &g_x11.wm_delete, 1);
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
        case 9: out->type = XWAYLAND_EVENT_FOCUS_IN; break;
        case 10: out->type = XWAYLAND_EVENT_FOCUS_OUT; break;
        case 12:
            g_x11.source_hash_valid = 0;
            out->type = XWAYLAND_EVENT_EXPOSE;
            break;
        case 18: g_x11.visible = 0; out->type = XWAYLAND_EVENT_HIDE; break;
        case 19:
            g_x11.visible = 1;
            g_x11.source_hash_valid = 0;
            out->type = XWAYLAND_EVENT_SHOW;
            break;
        case 22: {
            XConfigureEvent *configure = (XConfigureEvent *)&event;
            g_x11.x = configure->x; g_x11.y = configure->y;
            g_x11.width = configure->width; g_x11.height = configure->height;
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
    /* The native fullscreen surface can differ from the guest swap-chain
     * dimensions. Keep USER32 client coordinates in the guest render space so
     * mouse hit testing matches the pixels that are scaled for presentation. */
    g_x11.client_width = width;
    g_x11.client_height = height;
    int output_width = g_x11.width > 0 ? g_x11.width : width;
    int output_height = g_x11.height > 0 ? g_x11.height : height;
    if (!ok || output_width <= 0 || output_height <= 0 ||
        (size_t)output_width > SIZE_MAX / 4u / (size_t)output_height) {
        pthread_mutex_unlock(&g_x11.lock);
        return 0;
    }

    /* Prefer Vulkan for final presentation. The D3D11 compatibility renderer
     * remains CPU-backed; Vulkan owns only the native swapchain upload/present.
     * This gives overlays such as MangoHud a real Vulkan queue to observe. */
    if (g_x11.presenter != XWAYLAND_PRESENTER_X11) {
        if (vulkan_presenter_present_resource_rgba8(
                g_x11.display, g_x11.window, resource, serial, pixels,
                width, height, row_pitch, output_width, output_height)) {
            pthread_mutex_unlock(&g_x11.lock);
            return 1;
        }
        if (g_x11.presenter == XWAYLAND_PRESENTER_VULKAN) {
            fprintf(stderr, "[VULKAN] strict presenter failed; no X11 fallback\n");
            pthread_mutex_unlock(&g_x11.lock);
            return 0;
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

void xwayland_window_destroy(void)
{
    pthread_mutex_lock(&g_x11.lock);
    vulkan_presenter_destroy();
    if (g_x11.display && g_x11.window) {
        g_x11.DestroyWindow(g_x11.display, g_x11.window);
        g_x11.Flush(g_x11.display);
        g_x11.window = 0;
        g_x11.visible = 0;
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
