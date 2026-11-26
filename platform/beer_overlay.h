#ifndef BEER_OVERLAY_H
#define BEER_OVERLAY_H

#include <stddef.h>
#include <stdint.h>

/* In-game overlay: two CPU-rendered boxes (resource usage, pinned memory values) that the presenter copies
 * onto the swapchain image just before presenting. Layout, hotkey and colours come from the manager. */
typedef struct {
    int x, y, w, h;          /* position and size on the output image, pixels */
    const uint8_t *bgra;     /* w*h*4 bytes, B G R A (straight alpha) */
    uint64_t serial;         /* changes whenever the pixels changed (per slot) */
    int slot;                /* upload region 0..BEER_OVERLAY_SLOTS-1, fixed per box */
} BeerOverlayRect;

/* Slots: 0 resource panel, 1 pinned values, 2 alert banner, 3.. mod boxes (overlay_box_set). */
#define BEER_OVERLAY_SLOTS 12
#define BEER_OVERLAY_MOD_BOXES (BEER_OVERLAY_SLOTS - 3)
#define BEER_OVERLAY_REGION_BYTES (2u * 1024u * 1024u)   /* largest box bitmap per slot */

/* Returns the number of visible boxes, filling rects[0..n). */
int beer_overlay_frame(int out_w, int out_h, BeerOverlayRect rects[BEER_OVERLAY_SLOTS]);
/* BEER_HITCH_LOG=1: timestamped "[HITCH]" lines for frame-to-frame spikes (checked once per presented frame) and for
 * events other code reports (key presses, thread suspensions), to line a visible hitch up with its cause. */
int beer_hitch_enabled(void);
long long beer_hitch_now_us(void);
void beer_hitch_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* 1 while the overlay is shown with opacity below 100%: the presenter must blend it (post pass), not copy it. */
int beer_overlay_translucent(void);

/* Centred alert banner shown even while the overlay is hidden ("" removes it). Lines split by \n. */
void beer_overlay_set_alert(const char *text);

/* Key event from the window thread. Returns 1 when the event was the overlay hotkey and must not reach the game. */
int beer_overlay_hotkey_event(unsigned long keysym, unsigned state, int down);

/* Call once per presented frame: feeds the frame-time graph. */
void beer_overlay_note_frame(void);

/* overlay_* control commands and the initial BEER_OVERLAY_JSON configuration. */
void beer_overlay_handle(const char *cmd, const char *request, char *out, size_t size);
void beer_overlay_init(void);

#endif
