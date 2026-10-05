/* Standalone X11 window/input probe used for Beer UI debugging.
 *
 * Beer renders through Vulkan on an XWayland surface, so desktop screenshot
 * tools read back an empty X11 pixmap. Visual inspection therefore relies on
 * the renderer-side frame dump (BEER_FRAME_DUMP_DIR), and this helper supplies
 * the matching deterministic input: it locates the guest window, reports its
 * geometry, and injects absolute pointer motion/clicks through XTest so hover
 * states can be reproduced without manual mouse work.
 *
 * This tool is diagnostics-only and is never linked into the loader. Xlib and
 * XTest are resolved with dlopen so the loader build stays header-free.
 *
 * Build:  gcc -O2 -Wall -Wextra tools/x11_input_probe.c -ldl -o /tmp/beer-x11-probe
 * Usage:  beer-x11-probe find <title-substring>
 *         beer-x11-probe move <x> <y>
 *         beer-x11-probe click <x> <y> [button]
 *         beer-x11-probe key <keysym-name>
 *         beer-x11-probe focus <title-substring>
 *         beer-x11-probe hover <title-substring> <client-x> <client-y>
 *         beer-x11-probe pointer
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct Display Display;
typedef unsigned long XID;
typedef XID Window;
typedef XID Atom;

typedef Display *(*PFN_XOpenDisplay)(const char *);
typedef int (*PFN_XCloseDisplay)(Display *);
typedef Window (*PFN_XDefaultRootWindow)(Display *);
typedef int (*PFN_XQueryTree)(Display *, Window, Window *, Window *,
                              Window **, unsigned int *);
typedef int (*PFN_XFetchName)(Display *, Window, char **);
typedef int (*PFN_XFree)(void *);
typedef int (*PFN_XGetGeometry)(Display *, XID, Window *, int *, int *,
                               unsigned int *, unsigned int *,
                               unsigned int *, unsigned int *);
typedef int (*PFN_XTranslateCoordinates)(Display *, Window, Window, int, int,
                                         int *, int *, Window *);
typedef int (*PFN_XQueryPointer)(Display *, Window, Window *, Window *,
                                 int *, int *, int *, int *, unsigned int *);
typedef int (*PFN_XFlush)(Display *);
typedef int (*PFN_XSync)(Display *, int);
typedef int (*PFN_XTestFakeMotionEvent)(Display *, int, int, int,
                                        unsigned long);
typedef int (*PFN_XTestFakeButtonEvent)(Display *, unsigned int, int,
                                        unsigned long);
typedef int (*PFN_XTestFakeKeyEvent)(Display *, unsigned int, int,
                                     unsigned long);
typedef int (*PFN_XSetInputFocus)(Display *, Window, int, unsigned long);
typedef int (*PFN_XRaiseWindow)(Display *, Window);
typedef int (*PFN_XWarpPointer)(Display *, Window, Window, int, int,
                                unsigned int, unsigned int, int, int);
typedef unsigned char (*PFN_XKeysymToKeycode)(Display *, unsigned long);
typedef unsigned long (*PFN_XStringToKeysym)(const char *);
typedef Atom (*PFN_XInternAtom)(Display *, const char *, int);
typedef int (*PFN_XSendEvent)(Display *, Window, int, long, void *);

static struct {
    void *xlib;
    void *xtst;
    Display *display;
    PFN_XOpenDisplay OpenDisplay;
    PFN_XCloseDisplay CloseDisplay;
    PFN_XDefaultRootWindow DefaultRootWindow;
    PFN_XQueryTree QueryTree;
    PFN_XFetchName FetchName;
    PFN_XFree Free;
    PFN_XGetGeometry GetGeometry;
    PFN_XTranslateCoordinates TranslateCoordinates;
    PFN_XQueryPointer QueryPointer;
    PFN_XFlush Flush;
    PFN_XSync Sync;
    PFN_XTestFakeMotionEvent FakeMotion;
    PFN_XTestFakeButtonEvent FakeButton;
    PFN_XTestFakeKeyEvent FakeKey;
    PFN_XKeysymToKeycode KeysymToKeycode;
    PFN_XStringToKeysym StringToKeysym;
    PFN_XSetInputFocus SetInputFocus;
    PFN_XRaiseWindow RaiseWindow;
    PFN_XWarpPointer WarpPointer;
} g;

#define LOAD(handle, field, name)                                             \
    do {                                                                      \
        *(void **)(&g.field) = dlsym(handle, name);                           \
        if (!g.field) {                                                       \
            fprintf(stderr, "missing symbol %s\n", name);                     \
            return 0;                                                         \
        }                                                                     \
    } while (0)

static int probe_initialize(void)
{
    g.xlib = dlopen("libX11.so.6", RTLD_NOW);
    if (!g.xlib) {
        fprintf(stderr, "libX11.so.6 unavailable: %s\n", dlerror());
        return 0;
    }
    LOAD(g.xlib, OpenDisplay, "XOpenDisplay");
    LOAD(g.xlib, CloseDisplay, "XCloseDisplay");
    LOAD(g.xlib, DefaultRootWindow, "XDefaultRootWindow");
    LOAD(g.xlib, QueryTree, "XQueryTree");
    LOAD(g.xlib, FetchName, "XFetchName");
    LOAD(g.xlib, Free, "XFree");
    LOAD(g.xlib, GetGeometry, "XGetGeometry");
    LOAD(g.xlib, TranslateCoordinates, "XTranslateCoordinates");
    LOAD(g.xlib, QueryPointer, "XQueryPointer");
    LOAD(g.xlib, Flush, "XFlush");
    LOAD(g.xlib, Sync, "XSync");

    *(void **)(&g.KeysymToKeycode) = dlsym(g.xlib, "XKeysymToKeycode");
    *(void **)(&g.StringToKeysym) = dlsym(g.xlib, "XStringToKeysym");
    *(void **)(&g.SetInputFocus) = dlsym(g.xlib, "XSetInputFocus");
    *(void **)(&g.RaiseWindow) = dlsym(g.xlib, "XRaiseWindow");
    *(void **)(&g.WarpPointer) = dlsym(g.xlib, "XWarpPointer");

    g.xtst = dlopen("libXtst.so.6", RTLD_NOW);
    if (g.xtst) {
        *(void **)(&g.FakeMotion) = dlsym(g.xtst, "XTestFakeMotionEvent");
        *(void **)(&g.FakeButton) = dlsym(g.xtst, "XTestFakeButtonEvent");
        *(void **)(&g.FakeKey) = dlsym(g.xtst, "XTestFakeKeyEvent");
    }

    g.display = g.OpenDisplay(NULL);
    if (!g.display) {
        fprintf(stderr, "cannot open DISPLAY\n");
        return 0;
    }
    return 1;
}

/* Returns the deepest window whose name contains needle.
 *
 * The compositor reparents the guest surface, so both the mutter frame and the
 * loader's own window carry the title. XTest input and focus must target the
 * loader window (the deepest match), not the frame: focusing the frame leaves
 * the guest unable to observe keys. */
static Window find_window(Window root, const char *needle, int depth)
{
    Window found = 0;
    Window parent = 0, *children = NULL;
    unsigned int count = 0;

    if (depth <= 6 &&
        g.QueryTree(g.display, root, &parent, &parent, &children, &count) &&
        children) {
        for (unsigned int i = 0; i < count; ++i) {
            Window child = find_window(children[i], needle, depth + 1);
            if (child) found = child;
        }
        g.Free(children);
    }
    if (found) return found;

    char *name = NULL;
    if (g.FetchName(g.display, root, &name) && name) {
        if (strstr(name, needle)) found = root;
        g.Free(name);
    }
    return found;
}

static int report_window(const char *needle)
{
    Window root = g.DefaultRootWindow(g.display);
    Window window = find_window(root, needle, 0);
    if (!window) {
        fprintf(stderr, "no window matching \"%s\"\n", needle);
        return 1;
    }
    Window child_root = 0;
    int x = 0, y = 0;
    unsigned int width = 0, height = 0, border = 0, depth = 0;
    if (!g.GetGeometry(g.display, window, &child_root, &x, &y, &width, &height,
                       &border, &depth)) {
        fprintf(stderr, "XGetGeometry failed\n");
        return 1;
    }
    int root_x = 0, root_y = 0;
    Window ignored = 0;
    g.TranslateCoordinates(g.display, window, root, 0, 0, &root_x, &root_y,
                           &ignored);
    printf("window=0x%lx root-origin=%d,%d size=%ux%u depth=%u\n",
           (unsigned long)window, root_x, root_y, width, height, depth);
    return 0;
}

static int inject_motion(int x, int y)
{
    if (!g.FakeMotion) {
        fprintf(stderr, "XTest unavailable; cannot inject motion\n");
        return 1;
    }
    g.FakeMotion(g.display, -1, x, y, 0);
    g.Sync(g.display, 0);
    printf("motion %d,%d\n", x, y);
    return 0;
}

static int inject_click(int x, int y, unsigned int button)
{
    if (!g.FakeMotion || !g.FakeButton) {
        fprintf(stderr, "XTest unavailable; cannot inject click\n");
        return 1;
    }
    g.FakeMotion(g.display, -1, x, y, 0);
    g.Sync(g.display, 0);
    g.FakeButton(g.display, button, 1, 0);
    g.Sync(g.display, 0);
    g.FakeButton(g.display, button, 0, 0);
    g.Sync(g.display, 0);
    printf("click %d,%d button=%u\n", x, y, button);
    return 0;
}

/* Raises the guest window and gives it the X11 input focus.
 *
 * XTest events are delivered to the focused window, so without this the loader
 * never observes injected keys while another application owns the focus. */
static int focus_window(const char *needle)
{
    if (!g.SetInputFocus || !g.RaiseWindow) {
        fprintf(stderr, "focus entry points unavailable\n");
        return 1;
    }
    Window root = g.DefaultRootWindow(g.display);
    Window window = find_window(root, needle, 0);
    if (!window) {
        fprintf(stderr, "no window matching \"%s\"\n", needle);
        return 1;
    }
    g.RaiseWindow(g.display, window);
    g.SetInputFocus(g.display, window, 2 /* RevertToParent */, 0 /* CurrentTime */);
    g.Sync(g.display, 0);
    printf("focus window=0x%lx\n", (unsigned long)window);
    return 0;
}

/* Activates the guest window through EWMH.
 *
 * XSetInputFocus alone is insufficient under mutter: the compositor tracks its
 * own active window and restores focus immediately, so injected keys are
 * delivered elsewhere. _NET_ACTIVE_WINDOW asks the window manager to perform a
 * real activation, which also raises the frame and updates the focus stack. */
static int activate_window(const char *needle)
{
    if (!g.InternAtom || !g.SendEvent) {
        fprintf(stderr, "EWMH entry points unavailable\n");
        return 1;
    }
    Window root = g.DefaultRootWindow(g.display);
    Window window = find_window(root, needle, 0);
    if (!window) {
        fprintf(stderr, "no window matching \"%s\"\n", needle);
        return 1;
    }

    Atom active = g.InternAtom(g.display, "_NET_ACTIVE_WINDOW", 0);
    /* ClientMessage, 32-bit format. data.l[0] = 2 marks a pager/tool request,
     * which window managers honour without a user-interaction timestamp. */
    long event[24] = {0};
    event[0] = 33;              /* type = ClientMessage */
    event[1] = 32;              /* serial (unused) */
    event[2] = 0;               /* send_event */
    event[3] = (long)(intptr_t)g.display;
    event[4] = (long)window;
    event[5] = (long)active;
    event[6] = 32;              /* format */
    event[7] = 2;               /* source: pager */
    event[8] = 0;               /* timestamp: CurrentTime */
    event[9] = 0;               /* requestor */

    /* SubstructureNotifyMask | SubstructureRedirectMask */
    g.SendEvent(g.display, root, 0, (1L << 19) | (1L << 20), (XEvent *)event);
    g.Sync(g.display, 0);
    if (g.RaiseWindow) g.RaiseWindow(g.display, window);
    if (g.SetInputFocus) g.SetInputFocus(g.display, window, 2, 0);
    g.Sync(g.display, 0);
    printf("activate window=0x%lx\n", (unsigned long)window);
    return 0;
}

/* Warps the pointer to a window-relative position.
 *
 * XTest absolute motion targets the root window, which is wrong once the guest
 * surface is offset or scaled. Hover reproduction needs client coordinates. */
static int hover_window(const char *needle, int x, int y)
{
    if (!g.WarpPointer) {
        fprintf(stderr, "XWarpPointer unavailable\n");
        return 1;
    }
    Window root = g.DefaultRootWindow(g.display);
    Window window = find_window(root, needle, 0);
    if (!window) {
        fprintf(stderr, "no window matching \"%s\"\n", needle);
        return 1;
    }
    g.WarpPointer(g.display, 0, window, 0, 0, 0, 0, x, y);
    g.Sync(g.display, 0);
    printf("hover window=0x%lx client=%d,%d\n", (unsigned long)window, x, y);
    return 0;
}

/* Presses and releases a key, holding it for hold_ms.
 *
 * A zero-length press is unreliable against a game that samples device state
 * once per frame: the press and release can both land between two polls. The
 * default hold spans several frames at 60 Hz. */
static int inject_key(const char *keysym_name, unsigned int hold_ms)
{
    if (!g.FakeKey || !g.KeysymToKeycode || !g.StringToKeysym) {
        fprintf(stderr, "XTest keyboard injection unavailable\n");
        return 1;
    }
    unsigned long keysym = g.StringToKeysym(keysym_name);
    if (!keysym) {
        fprintf(stderr, "unknown keysym \"%s\"\n", keysym_name);
        return 1;
    }
    unsigned int keycode = g.KeysymToKeycode(g.display, keysym);
    if (!keycode) {
        fprintf(stderr, "keysym \"%s\" is not mapped\n", keysym_name);
        return 1;
    }
    g.FakeKey(g.display, keycode, 1, 0);
    g.Sync(g.display, 0);
    struct timespec hold = {
        .tv_sec = (time_t)(hold_ms / 1000u),
        .tv_nsec = (long)(hold_ms % 1000u) * 1000000L
    };
    nanosleep(&hold, NULL);
    g.FakeKey(g.display, keycode, 0, 0);
    g.Sync(g.display, 0);
    printf("key %s keycode=%u hold=%ums\n", keysym_name, keycode, hold_ms);
    return 0;
}

/* Dumps the window tree with names and geometry.
 *
 * Needed because the loader's own window id (reported by the XWayland backend)
 * may differ from the first title match: compositors and XWayland insert
 * frame/parent windows that also carry the application title. */
static void dump_tree(Window window, int depth)
{
    Window parent = 0, *children = NULL;
    unsigned int count = 0;
    char *name = NULL;
    Window child_root = 0;
    int x = 0, y = 0;
    unsigned int width = 0, height = 0, border = 0, bit_depth = 0;

    if (g.GetGeometry(g.display, window, &child_root, &x, &y, &width, &height,
                      &border, &bit_depth)) {
        g.FetchName(g.display, window, &name);
        printf("%*swindow=0x%-9lx %4ux%-4u at %4d,%-4d depth=%2u name=%s\n",
               depth * 2, "", (unsigned long)window, width, height, x, y,
               bit_depth, name ? name : "(none)");
        if (name) g.Free(name);
    }
    if (depth > 4) return;
    if (!g.QueryTree(g.display, window, &parent, &parent, &children, &count) ||
        !children)
        return;
    for (unsigned int i = 0; i < count; ++i) dump_tree(children[i], depth + 1);
    g.Free(children);
}

static int report_tree(void)
{
    dump_tree(g.DefaultRootWindow(g.display), 0);
    return 0;
}

static int report_pointer(void)
{
    Window root = g.DefaultRootWindow(g.display);
    Window returned_root = 0, child = 0;
    int root_x = 0, root_y = 0, win_x = 0, win_y = 0;
    unsigned int mask = 0;
    if (!g.QueryPointer(g.display, root, &returned_root, &child, &root_x,
                        &root_y, &win_x, &win_y, &mask)) {
        fprintf(stderr, "XQueryPointer failed\n");
        return 1;
    }
    printf("pointer root=%d,%d child=0x%lx mask=0x%x\n", root_x, root_y,
           (unsigned long)child, mask);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
                "usage: %s find <title> | move <x> <y> | "
                "click <x> <y> [button] | key <keysym> | "
                "focus <title> | activate <title> | "
                "hover <title> <x> <y> | tree | pointer\n",
                argv[0]);
        return 2;
    }
    if (!probe_initialize()) return 1;

    int status = 2;
    if (!strcmp(argv[1], "find") && argc >= 3) {
        status = report_window(argv[2]);
    } else if (!strcmp(argv[1], "move") && argc >= 4) {
        status = inject_motion(atoi(argv[2]), atoi(argv[3]));
    } else if (!strcmp(argv[1], "click") && argc >= 4) {
        unsigned int button = argc >= 5 ? (unsigned int)atoi(argv[4]) : 1u;
        status = inject_click(atoi(argv[2]), atoi(argv[3]), button);
    } else if (!strcmp(argv[1], "key") && argc >= 3) {
        unsigned int hold = argc >= 4 ? (unsigned int)atoi(argv[3]) : 120u;
        status = inject_key(argv[2], hold);
    } else if (!strcmp(argv[1], "focus") && argc >= 3) {
        status = focus_window(argv[2]);
    } else if (!strcmp(argv[1], "activate") && argc >= 3) {
        status = activate_window(argv[2]);
    } else if (!strcmp(argv[1], "hover") && argc >= 5) {
        status = hover_window(argv[2], atoi(argv[3]), atoi(argv[4]));
    } else if (!strcmp(argv[1], "tree")) {
        status = report_tree();
    } else if (!strcmp(argv[1], "pointer")) {
        status = report_pointer();
    } else {
        fprintf(stderr, "unknown command\n");
    }

    g.Flush(g.display);
    g.CloseDisplay(g.display);
    return status;
}
