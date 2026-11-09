/* GDI+ flat API subset (see win32_gdiplus.c). */
#ifndef BEER_WIN32_GDIPLUS_H
#define BEER_WIN32_GDIPLUS_H

#include <stdint.h>

#ifndef MS
#define MS __attribute__((ms_abi))
#define BEER_GDIPLUS_DEFINED_MS
#endif

typedef struct GpImage GpImage;
typedef struct GpGraphics GpGraphics;
typedef struct GpBrush GpBrush;
typedef struct GpFont GpFont;
typedef struct GpFormat GpFormat;
typedef struct BitmapData BitmapData;

MS uint32_t beer_GdipLoadImageFromStream(void *stream, GpImage **image);
MS uint32_t beer_GdipCreateBitmapFromScan0(int width, int height, int stride, uint32_t format, uint8_t *scan0, GpImage **bitmap);
MS uint32_t beer_GdipCloneImage(GpImage *image, GpImage **clone);
MS uint32_t beer_GdipDisposeImage(GpImage *image);
MS uint32_t beer_GdipGetImageWidth(GpImage *image, uint32_t *width);
MS uint32_t beer_GdipGetImageHeight(GpImage *image, uint32_t *height);
MS uint32_t beer_GdipImageGetFrameDimensionsCount(GpImage *image, uint32_t *count);
MS uint32_t beer_GdipImageGetFrameDimensionsList(GpImage *image, uint8_t *ids, uint32_t count);
MS uint32_t beer_GdipImageGetFrameCount(GpImage *image, const uint8_t *dimension, uint32_t *count);
MS uint32_t beer_GdipImageSelectActiveFrame(GpImage *image, const uint8_t *dimension, uint32_t index);
MS uint32_t beer_GdipGetPropertyItemSize(GpImage *image, uint32_t id, uint32_t *size);
MS uint32_t beer_GdipGetPropertyItem(GpImage *image, uint32_t id, uint32_t size, void *item);
MS uint32_t beer_GdipBitmapLockBits(GpImage *image, const int32_t *rect, uint32_t flags, uint32_t format, BitmapData *out);
MS uint32_t beer_GdipBitmapUnlockBits(GpImage *image, BitmapData *data);
MS uint32_t beer_GdipCreateHBITMAPFromBitmap(GpImage *image, uint64_t *hbitmap, uint32_t background);
MS uint32_t beer_GdipCreateFromHDC(uint64_t hdc, GpGraphics **graphics);
MS uint32_t beer_GdipGetImageGraphicsContext(GpImage *image, GpGraphics **graphics);
MS uint32_t beer_GdipDeleteGraphics(GpGraphics *graphics);
MS uint32_t beer_GdipSetInterpolationMode(GpGraphics *g, int mode);
MS uint32_t beer_GdipSetPixelOffsetMode(GpGraphics *g, int mode);
MS uint32_t beer_GdipSetSmoothingMode(GpGraphics *g, int mode);
MS uint32_t beer_GdipSetTextRenderingHint(GpGraphics *g, int hint);
MS uint32_t beer_GdipTranslateWorldTransform(GpGraphics *g, float dx, float dy, int order);
MS uint32_t beer_GdipGraphicsClear(GpGraphics *g, uint32_t color);
MS uint32_t beer_GdipDrawImageRectI(GpGraphics *g, GpImage *im, int x, int y, int w, int h);
MS uint32_t beer_GdipDrawImageRectRectI(GpGraphics *g, GpImage *im, int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh, int unit, void *attributes, void *callback, void *data);
MS uint32_t beer_GdipCreateSolidFill(uint32_t color, GpBrush **brush);
MS uint32_t beer_GdipCreateLineBrushI(const int32_t *p1, const int32_t *p2, uint32_t c1, uint32_t c2, int wrap, GpBrush **brush);
MS uint32_t beer_GdipCreateTexture(GpImage *image, int wrap, GpBrush **brush);
MS uint32_t beer_GdipCloneBrush(GpBrush *brush, GpBrush **clone);
MS uint32_t beer_GdipDeleteBrush(GpBrush *brush);
MS uint32_t beer_GdipFillEllipseI(GpGraphics *g, GpBrush *brush, int x, int y, int w, int h);
MS uint32_t beer_GdipCreateFontFromLogfontA(uint64_t hdc, const uint8_t *logfont, GpFont **font);
MS uint32_t beer_GdipCreateFontFromDC(uint64_t hdc, GpFont **font);
MS uint32_t beer_GdipDeleteFont(GpFont *font);
MS uint32_t beer_GdipCreateStringFormat(int flags, uint16_t language, GpFormat **format);
MS uint32_t beer_GdipDeleteStringFormat(GpFormat *format);
MS uint32_t beer_GdipSetStringFormatAlign(GpFormat *format, int align);
MS uint32_t beer_GdipSetStringFormatLineAlign(GpFormat *format, int align);
MS uint32_t beer_GdipDrawString(GpGraphics *g, const uint16_t *text, int length, GpFont *font, const float *rect, GpFormat *format, GpBrush *brush);

#ifdef BEER_GDIPLUS_DEFINED_MS
#undef MS
#undef BEER_GDIPLUS_DEFINED_MS
#endif
#endif
