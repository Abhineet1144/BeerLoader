#ifndef BEER_WINDOW_STYLE_POLICY_H
#define BEER_WINDOW_STYLE_POLICY_H

#include <stdint.h>

#define BEER_WS_POPUP       UINT32_C(0x80000000)
#define BEER_WS_CAPTION     UINT32_C(0x00c00000)
#define BEER_WS_THICKFRAME  UINT32_C(0x00040000)

/* Beer's current USER32 non-client contract. Keep AdjustWindowRect, client
 * reporting, and presentation cropping derived from the same values. */
#define BEER_WINDOW_FRAME_LEFT    8
#define BEER_WINDOW_FRAME_TOP    30
#define BEER_WINDOW_FRAME_RIGHT   8
#define BEER_WINDOW_FRAME_BOTTOM  8

static inline int beer_window_client_from_outer(int outer_width,
                                                 int outer_height,
                                                 int decorated,
                                                 int *client_x,
                                                 int *client_y,
                                                 int *client_width,
                                                 int *client_height)
{
    if (outer_width <= 0 || outer_height <= 0 || !client_x || !client_y ||
        !client_width || !client_height)
        return 0;

    *client_x = 0;
    *client_y = 0;
    *client_width = outer_width - (decorated ?
        BEER_WINDOW_FRAME_LEFT + BEER_WINDOW_FRAME_RIGHT : 0);
    *client_height = outer_height - (decorated ?
        BEER_WINDOW_FRAME_TOP + BEER_WINDOW_FRAME_BOTTOM : 0);
    return *client_width > 0 && *client_height > 0;
}

static inline int beer_window_outer_from_client(int client_width,
                                                 int client_height,
                                                 int decorated,
                                                 int *outer_width,
                                                 int *outer_height)
{
    if (client_width <= 0 || client_height <= 0 ||
        !outer_width || !outer_height)
        return 0;

    *outer_width = client_width + (decorated ?
        BEER_WINDOW_FRAME_LEFT + BEER_WINDOW_FRAME_RIGHT : 0);
    *outer_height = client_height + (decorated ?
        BEER_WINDOW_FRAME_TOP + BEER_WINDOW_FRAME_BOTTOM : 0);
    return 1;
}

static inline int beer_window_map_client_point(int native_x, int native_y,
                                                int native_width,
                                                int native_height,
                                                int client_width,
                                                int client_height,
                                                int *client_x,
                                                int *client_y)
{
    if (!client_x || !client_y || native_width <= 0 || native_height <= 0 ||
        client_width <= 0 || client_height <= 0)
        return 0;

    /* X11 pointer coordinates and drawable dimensions are client-relative.
     * Usually both extents are identical and this is an identity mapping. If
     * the compositor is between ConfigureNotify events, scale exactly once to
     * the guest client extent; never apply synthetic non-client offsets. */
    *client_x = (int)((int64_t)native_x * client_width / native_width);
    *client_y = (int)((int64_t)native_y * client_height / native_height);
    return 1;
}

static inline int beer_window_style_is_resizable(uint32_t style)
{
    return (style & BEER_WS_THICKFRAME) != 0;
}

static inline int beer_window_style_is_decorated(uint32_t style)
{
    return (style & BEER_WS_CAPTION) != 0 &&
           (style & BEER_WS_POPUP) == 0;
}

static inline int beer_window_request_is_fullscreen(uint32_t style,
                                                    int x, int y,
                                                    int width, int height,
                                                    int screen_width,
                                                    int screen_height)
{
    if (screen_width <= 0 || screen_height <= 0 ||
        x < -1 || y < -1 || x > 1 || y > 1)
        return 0;

    /* Borderless games commonly request the monitor dimensions directly. */
    if ((style & BEER_WS_POPUP) != 0 &&
        (style & BEER_WS_CAPTION) == 0 &&
        width >= screen_width - 1 && height >= screen_height - 1)
        return 1;

    /* Some fixed-mode games call AdjustWindowRect for a monitor-sized client
     * before creating their top-level window. Beer's non-client contract adds
     * 16x38 pixels, so the resulting request is slightly larger than the
     * monitor. Recognize only fixed, decorated windows near the monitor size;
     * ordinary resizable desktop windows must remain windowed. */
    return beer_window_style_is_decorated(style) &&
           !beer_window_style_is_resizable(style) &&
           width >= screen_width && width <= screen_width + 64 &&
           height >= screen_height && height <= screen_height + 64;
}

#endif
