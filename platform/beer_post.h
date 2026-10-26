#ifndef BEER_POST_H
#define BEER_POST_H

#include <stddef.h>
#include <stdint.h>

/* Final-image effects and screenshots. All values are fixed point (milli units, 1000 = 1.0) because the
 * loader is built without SSE; the compute shader divides them back. */
typedef struct {
    int32_t brightness, contrast, gamma, saturation, vibrance, sharpen, vignette, grain;
    int32_t temperature, colorblind, cb_strength, letterbox, seed;
} BeerPostParams;

/* Fills *out and returns 1 when any effect differs from neutral. */
int beer_post_params(BeerPostParams *out);

/* Returns 1 (once) when a screenshot was requested; path receives the file to write and clean is set when the
 * overlay should not be part of it. */
int beer_post_take_screenshot(char *path, size_t size, int *clean);

/* Screenshot hotkey (keysym + Shift/Ctrl/Alt mask from the window thread); 1 = consumed. */
int beer_post_hotkey_event(unsigned long keysym, unsigned state, int down);

/* post_get / post_set / screenshot control commands and BEER_POST_JSON. */
void beer_post_handle(const char *cmd, const char *request, char *out, size_t size);
void beer_post_init(const char *exe_path);

#endif
