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
int xwayland_window_move_resize(int x, int y, int width, int height,
                                int move, int resize);
int xwayland_window_set_title(const char *title);
int xwayland_window_get_state(XwaylandWindowState *state);
int xwayland_window_pump_events(void);
void xwayland_window_destroy(void);
int xwayland_window_exists(void);

#endif
