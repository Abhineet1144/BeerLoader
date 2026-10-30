#ifndef BEER_WIN32_GDI_H
#define BEER_WIN32_GDI_H

#include <stdint.h>

/* Software GDI. Every bitmap is a CPU surface; the window DC draws into a client-sized surface that
 * is presented through the native window when the guest releases the DC / ends painting. The
 * window DC handle is the loader's FAKE_HDC (0xDC000001). */

#define BEER_GDI_WINDOW_DC 0xDC000001ULL

#define GDI_MS __attribute__((ms_abi))

/* Presents the window surface if anything was drawn since the last flush. */
void beer_gdi_flush_window(void);

/* USER32 */
uint64_t GDI_MS beer_LoadBitmapW(uint64_t instance, uint64_t name);
uint64_t GDI_MS beer_LoadBitmapA(uint64_t instance, uint64_t name);
uint64_t GDI_MS beer_LoadImageW(uint64_t instance, uint64_t name, uint32_t type, int cx, int cy,
                                uint32_t flags);
uint64_t GDI_MS beer_LoadImageA(uint64_t instance, uint64_t name, uint32_t type, int cx, int cy,
                                uint32_t flags);
int GDI_MS beer_FillRect(uint64_t dc, const int32_t *rect, uint64_t brush);
int GDI_MS beer_DrawTextW(uint64_t dc, const uint16_t *text, int count, int32_t *rect, uint32_t format);
int GDI_MS beer_DrawTextA(uint64_t dc, const char *text, int count, int32_t *rect, uint32_t format);
uint64_t GDI_MS beer_GetWindowDC(uint64_t window);

/* Paints the built-in USER32 Static/Button controls into the parent window's
 * software client surface. Returns nonzero only for a recognized class. */
int beer_gdi_paint_standard_control(const char *class_name, const uint16_t *text,
                                    int x, int y, int width, int height);

/* GDI32 */
uint64_t GDI_MS beer_GetStockObject(int index);
uint64_t GDI_MS beer_CreateCompatibleDC(uint64_t dc);
uint64_t GDI_MS beer_CreateCompatibleBitmap(uint64_t dc, int width, int height);
uint64_t GDI_MS beer_CreateBitmap(int width, int height, uint32_t planes, uint32_t bits, const void *data);
uint64_t GDI_MS beer_CreateDIBSection(uint64_t dc, const void *info, uint32_t usage, void **bits,
                                      uint64_t section, uint32_t offset);
int GDI_MS beer_DeleteDC(uint64_t dc);
int GDI_MS beer_DeleteObject(uint64_t object);
uint64_t GDI_MS beer_SelectObject(uint64_t dc, uint64_t object);
int GDI_MS beer_GetObjectW(uint64_t object, int size, void *out);
uint32_t GDI_MS beer_GetObjectType(uint64_t object);
int GDI_MS beer_GetDeviceCaps(uint64_t dc, int index);
uint64_t GDI_MS beer_CreateFontW(int height, int width, int escapement, int orientation, int weight,
                                 uint32_t italic, uint32_t underline, uint32_t strike, uint32_t charset,
                                 uint32_t out_precision, uint32_t clip_precision, uint32_t quality,
                                 uint32_t pitch_family, const uint16_t *face);
uint64_t GDI_MS beer_CreateFontA(int height, int width, int escapement, int orientation, int weight,
                                 uint32_t italic, uint32_t underline, uint32_t strike, uint32_t charset,
                                 uint32_t out_precision, uint32_t clip_precision, uint32_t quality,
                                 uint32_t pitch_family, const char *face);
uint64_t GDI_MS beer_CreateFontIndirectW(const uint8_t *logfont);
uint64_t GDI_MS beer_CreateSolidBrush(uint32_t color);
uint64_t GDI_MS beer_CreatePen(int style, int width, uint32_t color);
int GDI_MS beer_SetBkMode(uint64_t dc, int mode);
uint32_t GDI_MS beer_SetBkColor(uint64_t dc, uint32_t color);
uint32_t GDI_MS beer_SetTextColor(uint64_t dc, uint32_t color);
uint32_t GDI_MS beer_GetBkColor(uint64_t dc);
uint32_t GDI_MS beer_GetTextColor(uint64_t dc);
uint32_t GDI_MS beer_SetTextAlign(uint64_t dc, uint32_t align);
int GDI_MS beer_GetTextExtentPoint32W(uint64_t dc, const uint16_t *text, int count, int32_t *size);
int GDI_MS beer_GetTextExtentPoint32A(uint64_t dc, const char *text, int count, int32_t *size);
int GDI_MS beer_GetTextMetricsW(uint64_t dc, uint8_t *metrics);
int GDI_MS beer_TextOutW(uint64_t dc, int x, int y, const uint16_t *text, int count);
int GDI_MS beer_TextOutA(uint64_t dc, int x, int y, const char *text, int count);
int GDI_MS beer_ExtTextOutW(uint64_t dc, int x, int y, uint32_t options, const int32_t *rect,
                            const uint16_t *text, uint32_t count, const int *dx);
int GDI_MS beer_BitBlt(uint64_t dst, int x, int y, int w, int h, uint64_t src, int sx, int sy, uint32_t rop);
int GDI_MS beer_StretchBlt(uint64_t dst, int x, int y, int w, int h, uint64_t src, int sx, int sy,
                           int sw, int sh, uint32_t rop);
int GDI_MS beer_PatBlt(uint64_t dc, int x, int y, int w, int h, uint32_t rop);
int GDI_MS beer_Rectangle(uint64_t dc, int left, int top, int right, int bottom);
int GDI_MS beer_StretchDIBits(uint64_t dc, int xd, int yd, int wd, int hd, int xs, int ys, int ws, int hs,
                              const void *bits, const void *info, uint32_t usage, uint32_t rop);
int GDI_MS beer_SetDIBitsToDevice(uint64_t dc, int xd, int yd, uint32_t w, uint32_t h, int xs, int ys,
                                  uint32_t start, uint32_t lines, const void *bits, const void *info,
                                  uint32_t usage);
uint32_t GDI_MS beer_SetPixel(uint64_t dc, int x, int y, uint32_t color);
uint32_t GDI_MS beer_GetPixel(uint64_t dc, int x, int y);
int GDI_MS beer_GdiFlush(void);
int GDI_MS beer_SetStretchBltMode(uint64_t dc, int mode);
int GDI_MS beer_SaveDC(uint64_t dc);
int GDI_MS beer_RestoreDC(uint64_t dc, int saved);
int GDI_MS beer_AlphaBlend(uint64_t dst, int x, int y, int w, int h, uint64_t src, int sx, int sy,
                           int sw, int sh, uint32_t blend);
int GDI_MS beer_TransparentBlt(uint64_t dst, int x, int y, int w, int h, uint64_t src, int sx, int sy,
                               int sw, int sh, uint32_t color);

#endif
