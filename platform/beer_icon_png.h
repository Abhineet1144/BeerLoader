/* PNG-compressed icon images (Windows stores large icons, usually 256x256, as PNG inside RT_ICON). */
#ifndef BEER_ICON_PNG_H
#define BEER_ICON_PNG_H

#include <stddef.h>
#include <stdint.h>

/* 1 if data starts with the PNG signature. */
int beer_icon_is_png(const uint8_t *data, size_t size);
/* Image size from the IHDR chunk, 1 on success. */
int beer_icon_png_size(const uint8_t *data, size_t size, int *width, int *height);
/* Decodes 8-bit non-interlaced PNG (grey, grey+alpha, RGB, RGBA, palette with tRNS) into ARGB pixels
 * (capacity in pixels). Returns 1 on success; other variants fail cleanly. */
int beer_icon_decode_png_argb(const uint8_t *data, size_t size, uint32_t *pixels, size_t capacity,
                              int *width, int *height);

#endif
