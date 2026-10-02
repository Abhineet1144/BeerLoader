#ifndef BEER_XWAYLAND_BACKEND_H
#define BEER_XWAYLAND_BACKEND_H

#include <stdint.h>

typedef struct {
    int x;
    int y;
    int width;
    int height;
    int visible;
} XwaylandWindowState;

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
int xwayland_window_pump_events(void);
/* Presents tightly packed RGBA8 pixels. The backend converts them to the
 * native XWayland visual without taking ownership of the caller's storage. */
int xwayland_window_present_rgba8(const uint8_t *pixels, int width, int height,
                                  int row_pitch);
void xwayland_window_destroy(void);
int xwayland_window_exists(void);

#endif
