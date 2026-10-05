#ifndef BEER_XWAYLAND_BACKEND_H
#define BEER_XWAYLAND_BACKEND_H

#include <stdint.h>

typedef struct {
    int x;
    int y;
    int width;
    int height;
    int client_width;
    int client_height;
    int visible;
} XwaylandWindowState;

typedef enum {
    XWAYLAND_EVENT_NONE = 0,
    XWAYLAND_EVENT_CLOSE,
    XWAYLAND_EVENT_SHOW,
    XWAYLAND_EVENT_HIDE,
    XWAYLAND_EVENT_FOCUS_IN,
    XWAYLAND_EVENT_FOCUS_OUT,
    XWAYLAND_EVENT_CONFIGURE,
    XWAYLAND_EVENT_EXPOSE,
    XWAYLAND_EVENT_KEY_DOWN,
    XWAYLAND_EVENT_KEY_UP,
    XWAYLAND_EVENT_BUTTON_DOWN,
    XWAYLAND_EVENT_BUTTON_UP,
    XWAYLAND_EVENT_POINTER_MOTION
} XwaylandEventType;

typedef struct {
    XwaylandEventType type;
    int x;
    int y;
    int root_x;
    int root_y;
    int width;
    int height;
    unsigned int keycode;
    unsigned long keysym;
    unsigned int button;
    unsigned int state;
} XwaylandEvent;

typedef enum {
    XWAYLAND_PRESENTER_AUTO = 0,
    XWAYLAND_PRESENTER_VULKAN,
    XWAYLAND_PRESENTER_X11
} XwaylandPresenter;

/* Must be selected before the native window is created. AUTO prefers Vulkan
 * and falls back to XPutImage; VULKAN is strict and never silently falls back. */
int xwayland_set_presenter(XwaylandPresenter presenter);
const char *xwayland_presenter_name(XwaylandPresenter presenter);

/* Xlib is loaded at runtime so Beer remains buildable without X11 headers. */
int xwayland_window_create(int x, int y, int width, int height,
                           const char *title, int initially_visible);
int xwayland_window_show(int visible);
/* Requests compositor-managed fullscreen through EWMH. Under Wayland this is
 * handled by XWayland and the active Wayland compositor. */
int xwayland_window_set_fullscreen(int fullscreen);
int xwayland_window_move_resize(int x, int y, int width, int height,
                                int move, int resize);
int xwayland_window_set_title(const char *title);
int xwayland_window_get_state(XwaylandWindowState *state);
/* Returns one native lifecycle event at a time. Input events remain reserved for
 * the DirectInput backend; window activation/configuration belongs to USER32. */
int xwayland_window_poll_event(XwaylandEvent *event);
int xwayland_window_pump_events(void);
/* Presents tightly packed RGBA8 pixels. The backend converts them to the
 * native XWayland visual without taking ownership of the caller's storage. */
int xwayland_window_present_rgba8(const uint8_t *pixels, int width, int height,
                                  int row_pitch);
int xwayland_window_present_resource_rgba8(const void *resource, uint64_t serial,
                                           const uint8_t *pixels, int width,
                                           int height, int row_pitch);
void xwayland_window_destroy(void);
int xwayland_window_exists(void);

#endif
