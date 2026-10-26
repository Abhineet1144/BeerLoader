#ifndef BEER_OVERLAY_H
#define BEER_OVERLAY_H

#include <stddef.h>
#include <stdint.h>

/* In-game overlay: two CPU-rendered boxes (resource usage, pinned memory values) that the presenter copies
 * onto the swapchain image just before presenting. Layout, hotkey and colours come from the manager. */
typedef struct {
    int x, y, w, h;          /* position and size on the output image, pixels */
    const uint8_t *bgra;     /* w*h*4 bytes, B G R A (straight alpha) */
    uint64_t serial;         /* changes whenever the pixels changed */
} BeerOverlayRect;

/* Returns the number of visible boxes (0 when the overlay is hidden), filling rects[0..n). */
int beer_overlay_frame(int out_w, int out_h, BeerOverlayRect rects[2]);

/* Key event from the window thread. Returns 1 when the event was the overlay hotkey and must not reach the game. */
int beer_overlay_hotkey_event(unsigned long keysym, unsigned state, int down);

/* Call once per presented frame: feeds the frame-time graph. */
void beer_overlay_note_frame(void);

/* overlay_* control commands and the initial BEER_OVERLAY_JSON configuration. */
void beer_overlay_handle(const char *cmd, const char *request, char *out, size_t size);
void beer_overlay_init(void);

#endif
