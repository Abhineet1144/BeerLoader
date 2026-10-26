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
    XWAYLAND_EVENT_POINTER_MOTION,
    /* Unaccelerated relative motion (XInput2 raw); x/y carry the whole-pixel
     * delta. Keeps arriving when the absolute pointer is clamped at a screen
     * or window edge, which mouse look relies on. */
    XWAYLAND_EVENT_RAW_MOTION
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
/* Requests native keyboard focus for the guest's top-level XWayland window. */
int xwayland_window_request_focus(void);
int xwayland_window_focus_lost_by_user(void);
/* Applies the guest Win32 frame policy to the native XWayland window. */
int xwayland_window_set_style(int decorated, int resizable);
/* Requests compositor-managed fullscreen through EWMH. Under Wayland this is
 * handled by XWayland and the active Wayland compositor. */
int xwayland_window_set_fullscreen(int fullscreen);
int xwayland_window_move_resize(int x, int y, int width, int height,
                                int move, int resize);
/* Sets the guest-visible client extent. Native X11 window dimensions already
 * describe a client surface, so the origin must normally remain (0,0). */
int xwayland_window_set_client_rect(int client_x, int client_y,
                                    int client_width, int client_height);
int xwayland_window_set_title(const char *title);
/* Sets the generic desktop identity used for WM_CLASS grouping. The value is
 * cached before native-window creation and contains no title-specific policy. */
int xwayland_window_set_app_id(const char *app_id);
/* Publishes an ARGB32 application icon through EWMH for taskbars and switchers.
 * Pixels use Win32's 0xAARRGGBB representation. The backend caches a bounded
 * copy so it can install the property before mapping the native window. */
int xwayland_window_set_icon_argb(const uint32_t *pixels, int width, int height);
/* Applies Win32 ShowCursor visibility to the native XWayland window. */
int xwayland_window_set_cursor_visible(int visible);
/* Moves the native pointer to an absolute desktop coordinate. */
int xwayland_window_set_cursor_position(int root_x, int root_y);
/* ClipCursor: confine (1) or release (0) the pointer to the window. */
int xwayland_window_set_pointer_confine(int enable);
/* Nonzero once XInput2 raw motion is delivering relative deltas. */
int xwayland_raw_motion_active(void);
int xwayland_window_get_state(XwaylandWindowState *state);
/* Returns the physical screen dimensions in pixels via the X11 display
 * connection.  Falls back to 1920x1080 when the display is unavailable. */
void xwayland_get_screen_size(int *out_width, int *out_height);
int xwayland_get_screen_refresh(void);
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
/* Presents a Vulkan image (handle as uint64_t) directly through the Vulkan
 * presenter; see vulkan_presenter_present_image. No X11 fallback. */
int xwayland_window_present_image(uint64_t image, uint32_t layout, int width,
                                  int height, int direct_blit);
/* Native handles of the guest window (Display* and X11 Window) for guest-side Vulkan WSI; 0 when no window. */
int xwayland_window_native(void **display, unsigned long *window);
void xwayland_window_destroy(void);
int xwayland_window_exists(void);

#endif
