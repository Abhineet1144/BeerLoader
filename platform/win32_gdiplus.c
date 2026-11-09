/* GDI+ flat API subset (gdiplus.dll), drawn into Beer's GDI surfaces.
 *
 * Covers what UI libraries built on GDI+ use to draw their skins (duilib in NetEase's SDK dialogs): images loaded from
 * IStream (PNG, BMP) or created from memory, Graphics on an HDC or an image, scaled alpha-blended DrawImage, solid /
 * linear-gradient / texture brushes, FillEllipse, fonts and DrawString (through Beer's GDI text). Rendering is
 * source-over with nearest-neighbour scaling; the world transform is a translation. Anything else returns
 * NotImplemented instead of a false Ok, so callers take their own fallback.
 *
 * Built with SSE: the Win64 ABI passes GDI+'s REAL (float) parameters in XMM registers, and image loading calls the
 * guest's IStream through its vtable. */
#include "win32_gdiplus.h"
#include "win32_gdi.h"
#include "beer_icon_png.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MS __attribute__((ms_abi))

enum { Ok = 0, GenericError = 1, InvalidParameter = 2, OutOfMemory = 3, NotImplemented = 6, UnknownImageFormat = 13,
       PropertyNotFound = 19 };
#define FMT_32ARGB  0x0026200Au
#define FMT_32PARGB 0x000E200Bu
#define FMT_32RGB   0x00022009u
#define FMT_24RGB   0x00021808u

enum { KIND_IMAGE = 0x47494d31, KIND_GRAPHICS, KIND_BRUSH, KIND_FONT, KIND_FORMAT };

struct GpImage {
    uint32_t kind;
    int width, height, stride;     /* stride in bytes */
    uint32_t format;
    uint8_t *scan0;
    int owned;
    uint8_t *lock_buffer;          /* LockBits conversion buffer */
    int lock_x, lock_y, lock_w, lock_h, lock_write;
    uint32_t lock_format;
};

struct GpGraphics {
    uint32_t kind;
    uint64_t hdc;                  /* target DC, or 0 */
    GpImage *image;                /* target image, or NULL */
    float tx, ty;                  /* world translation */
};

struct GpBrush {
    uint32_t kind;
    int type;                      /* 0 solid, 1 linear gradient, 2 texture */
    uint32_t color1, color2;       /* ARGB */
    float x1, y1, x2, y2;
    GpImage *texture;
};

struct GpFont { uint32_t kind; uint8_t logfont[92]; };                   /* LOGFONTW */
struct GpFormat { uint32_t kind; int align, line_align; uint32_t flags; };

static int is_kind(const void *object, uint32_t kind) { return object && *(const uint32_t *)object == kind; }

/* ------------------------------------------------------------------ pixels */

static uint32_t image_get(const GpImage *im, int x, int y)          /* non-premultiplied ARGB */
{
    const uint8_t *p = im->scan0 + (size_t)y * (size_t)im->stride;
    if (im->format == FMT_24RGB) { p += (size_t)x * 3; return 0xff000000u | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }
    uint32_t px;
    memcpy(&px, p + (size_t)x * 4, 4);
    if (im->format == FMT_32RGB) return px | 0xff000000u;
    if (im->format == FMT_32PARGB) {
        const uint32_t a = px >> 24;
        if (!a) return 0;
        if (a == 255) return px;
        uint32_t r = ((px >> 16) & 0xff) * 255u / a, g = ((px >> 8) & 0xff) * 255u / a, b = (px & 0xff) * 255u / a;
        return a << 24 | (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
    }
    return px;
}

static void image_put(GpImage *im, int x, int y, uint32_t argb)
{
    uint8_t *p = im->scan0 + (size_t)y * (size_t)im->stride;
    if (im->format == FMT_24RGB) { p += (size_t)x * 3; p[0] = (uint8_t)argb; p[1] = (uint8_t)(argb >> 8); p[2] = (uint8_t)(argb >> 16); return; }
    if (im->format == FMT_32PARGB) {
        const uint32_t a = argb >> 24;
        argb = a << 24 | (((argb >> 16) & 0xff) * a / 255u) << 16 | (((argb >> 8) & 0xff) * a / 255u) << 8 | (argb & 0xff) * a / 255u;
    }
    memcpy(p + (size_t)x * 4, &argb, 4);
}

static uint32_t blend_over(uint32_t dst, uint32_t src)             /* both non-premultiplied ARGB */
{
    const uint32_t sa = src >> 24;
    if (sa == 255) return src;
    if (!sa) return dst;
    const uint32_t da = dst >> 24, inv = 255u - sa;
    const uint32_t oa = sa + da * inv / 255u;
    if (!oa) return 0;
    uint32_t out = oa << 24;
    for (int shift = 0; shift <= 16; shift += 8) {
        const uint32_t s = (src >> shift) & 0xff, d = (dst >> shift) & 0xff;
        out |= ((s * sa + d * da * inv / 255u) / oa) << shift;
    }
    return out;
}

/* A drawing target: a DC surface or an image, addressed top-down. */
typedef struct {
    GpImage *image;
    uint8_t *bits;
    int width, height, stride, bpp, topdown;
} Target;

static int target_open(GpGraphics *g, Target *t)
{
    memset(t, 0, sizeof(*t));
    if (g->image) { t->image = g->image; t->width = g->image->width; t->height = g->image->height; return 1; }
    return g->hdc && beer_gdi_dc_surface(g->hdc, &t->bits, &t->width, &t->height, &t->stride, &t->bpp, &t->topdown) &&
           (t->bpp == 32 || t->bpp == 24);
}

static uint32_t target_get(const Target *t, int x, int y)
{
    if (t->image) return image_get(t->image, x, y);
    const int row = t->topdown ? y : t->height - 1 - y;
    const uint8_t *p = t->bits + (size_t)row * (size_t)t->stride + (size_t)x * (size_t)(t->bpp / 8);
    return (t->bpp == 32 ? (uint32_t)p[3] << 24 : 0xff000000u) | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}

static void target_put(Target *t, int x, int y, uint32_t argb)
{
    if (t->image) { image_put(t->image, x, y, argb); return; }
    const int row = t->topdown ? y : t->height - 1 - y;
    uint8_t *p = t->bits + (size_t)row * (size_t)t->stride + (size_t)x * (size_t)(t->bpp / 8);
    p[0] = (uint8_t)argb; p[1] = (uint8_t)(argb >> 8); p[2] = (uint8_t)(argb >> 16);
    if (t->bpp == 32) p[3] = (uint8_t)(argb >> 24);
}

static void target_blend(Target *t, int x, int y, uint32_t argb)
{
    if (x < 0 || y < 0 || x >= t->width || y >= t->height || !(argb >> 24)) return;
    target_put(t, x, y, (argb >> 24) == 255 ? argb : blend_over(target_get(t, x, y), argb));
}

/* ------------------------------------------------------------------ images */

static GpImage *image_new(int width, int height, uint32_t format)
{
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) return NULL;
    GpImage *im = calloc(1, sizeof(*im));
    if (!im) return NULL;
    im->kind = KIND_IMAGE;
    im->width = width; im->height = height; im->format = format;
    im->stride = format == FMT_24RGB ? ((width * 3 + 3) & ~3) : width * 4;
    im->scan0 = calloc((size_t)im->stride, (size_t)height);
    im->owned = 1;
    if (!im->scan0) { free(im); return NULL; }
    return im;
}

static void image_free(GpImage *im)
{
    if (!im) return;
    if (im->owned) free(im->scan0);
    free(im->lock_buffer);
    im->kind = 0;
    free(im);
}

/* Uncompressed BMP (24/32 bpp, BI_RGB/BI_BITFIELDS). */
static GpImage *decode_bmp(const uint8_t *data, size_t size)
{
    if (size < 54 || data[0] != 'B' || data[1] != 'M') return NULL;
    uint32_t offset, header; int32_t width, height; uint16_t bpp; uint32_t compression;
    memcpy(&offset, data + 10, 4); memcpy(&header, data + 14, 4); memcpy(&width, data + 18, 4);
    memcpy(&height, data + 22, 4); memcpy(&bpp, data + 28, 2); memcpy(&compression, data + 30, 4);
    if ((bpp != 24 && bpp != 32) || (compression != 0 && compression != 3) || width <= 0 || !height) return NULL;
    const int top_down = height < 0, h = top_down ? -height : height;
    const size_t stride = ((size_t)width * (bpp / 8) + 3) & ~(size_t)3;
    if (offset > size || stride * (size_t)h > size - offset) return NULL;
    GpImage *im = image_new(width, h, bpp == 32 ? FMT_32ARGB : FMT_32RGB);
    if (!im) return NULL;
    for (int y = 0; y < h; ++y) {
        const uint8_t *row = data + offset + stride * (size_t)(top_down ? y : h - 1 - y);
        for (int x = 0; x < width; ++x) {
            const uint8_t *p = row + (size_t)x * (bpp / 8);
            const uint32_t a = bpp == 32 ? p[3] : 255;
            image_put(im, x, y, a << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]);
        }
    }
    return im;
}

static GpImage *decode_image(const uint8_t *data, size_t size)
{
    int width = 0, height = 0;
    if (beer_icon_is_png(data, size) && beer_icon_png_size(data, size, &width, &height) && width > 0 && height > 0 &&
        width <= 16384 && height <= 16384) {
        GpImage *im = image_new(width, height, FMT_32ARGB);
        if (im && beer_icon_decode_png_argb(data, size, (uint32_t *)im->scan0, (size_t)width * (size_t)height,
                                            &width, &height))
            return im;
        image_free(im);
        fprintf(stderr, "[GDIPLUS] PNG variant not supported (%dx%d)\n", width, height);
        return NULL;
    }
    GpImage *bmp = decode_bmp(data, size);
    if (bmp) return bmp;
    static int logs;
    if (logs++ < 8)
        fprintf(stderr, "[GDIPLUS] unsupported image format (%02x %02x %02x %02x, %zu bytes)\n",
                size > 0 ? data[0] : 0, size > 1 ? data[1] : 0, size > 2 ? data[2] : 0, size > 3 ? data[3] : 0, size);
    return NULL;
}

/* IStream: Read (vtable 3), Seek (5). */
typedef long MS (*StreamRead)(void *, void *, uint32_t, uint32_t *);
typedef long MS (*StreamSeek)(void *, int64_t, uint32_t, uint64_t *);

static uint8_t *read_stream(void *stream, size_t *size)
{
    void **vtable = *(void ***)stream;
    ((StreamSeek)vtable[5])(stream, 0, 0 /* STREAM_SEEK_SET */, NULL);
    size_t capacity = 1 << 16, length = 0;
    uint8_t *data = malloc(capacity);
    while (data) {
        if (capacity - length < (1u << 16)) {
            uint8_t *grown = realloc(data, capacity * 2);
            if (!grown) { free(data); return NULL; }
            data = grown; capacity *= 2;
        }
        uint32_t got = 0;
        const long hr = ((StreamRead)vtable[3])(stream, data + length, 1u << 16, &got);
        length += got;
        if (hr < 0 || !got || length > (256u << 20)) break;
    }
    *size = length;
    return data;
}

MS uint32_t beer_GdipLoadImageFromStream(void *stream, GpImage **image)
{
    if (!stream || !image) return InvalidParameter;
    *image = NULL;
    size_t size = 0;
    uint8_t *data = read_stream(stream, &size);
    if (!data) return OutOfMemory;
    *image = decode_image(data, size);
    free(data);
    return *image ? Ok : UnknownImageFormat;
}

MS uint32_t beer_GdipCreateBitmapFromScan0(int width, int height, int stride, uint32_t format, uint8_t *scan0,
                                           GpImage **bitmap)
{
    if (!bitmap || width <= 0 || height <= 0) return InvalidParameter;
    if (format != FMT_32ARGB && format != FMT_32PARGB && format != FMT_32RGB && format != FMT_24RGB) return NotImplemented;
    if (scan0) {                                                   /* GDI+ uses the caller's memory */
        GpImage *im = calloc(1, sizeof(*im));
        if (!im) return OutOfMemory;
        im->kind = KIND_IMAGE; im->width = width; im->height = height; im->stride = stride; im->format = format;
        im->scan0 = scan0;
        *bitmap = im;
        return Ok;
    }
    *bitmap = image_new(width, height, format);
    return *bitmap ? Ok : OutOfMemory;
}

MS uint32_t beer_GdipCloneImage(GpImage *image, GpImage **clone)
{
    if (!is_kind(image, KIND_IMAGE) || !clone) return InvalidParameter;
    GpImage *copy = image_new(image->width, image->height, image->format);
    if (!copy) return OutOfMemory;
    for (int y = 0; y < image->height; ++y)
        memcpy(copy->scan0 + (size_t)y * copy->stride, image->scan0 + (size_t)y * image->stride,
               (size_t)(copy->stride < image->stride ? copy->stride : image->stride));
    *clone = copy;
    return Ok;
}

MS uint32_t beer_GdipDisposeImage(GpImage *image)
{
    if (!is_kind(image, KIND_IMAGE)) return InvalidParameter;
    image_free(image);
    return Ok;
}

MS uint32_t beer_GdipGetImageWidth(GpImage *image, uint32_t *width)
{
    if (!is_kind(image, KIND_IMAGE) || !width) return InvalidParameter;
    *width = (uint32_t)image->width;
    return Ok;
}

MS uint32_t beer_GdipGetImageHeight(GpImage *image, uint32_t *height)
{
    if (!is_kind(image, KIND_IMAGE) || !height) return InvalidParameter;
    *height = (uint32_t)image->height;
    return Ok;
}

/* Single-frame images: one dimension, FrameDimensionPage. Animated GIFs are not decoded. */
MS uint32_t beer_GdipImageGetFrameDimensionsCount(GpImage *image, uint32_t *count)
{
    if (!is_kind(image, KIND_IMAGE) || !count) return InvalidParameter;
    *count = 1;
    return Ok;
}

MS uint32_t beer_GdipImageGetFrameDimensionsList(GpImage *image, uint8_t *ids, uint32_t count)
{
    static const uint8_t page[16] = { 0x86, 0xdc, 0x62, 0x74, 0x80, 0x61, 0x7e, 0x4c,
                                      0x8e, 0x3f, 0xee, 0x73, 0x33, 0xa7, 0xa4, 0x83 };   /* 7462dc86-6180-4c7e-8e3f-ee7333a7a483 */
    if (!is_kind(image, KIND_IMAGE) || !ids || count < 1) return InvalidParameter;
    memcpy(ids, page, 16);
    return Ok;
}

MS uint32_t beer_GdipImageGetFrameCount(GpImage *image, const uint8_t *dimension, uint32_t *count)
{
    (void)dimension;
    if (!is_kind(image, KIND_IMAGE) || !count) return InvalidParameter;
    *count = 1;
    return Ok;
}

MS uint32_t beer_GdipImageSelectActiveFrame(GpImage *image, const uint8_t *dimension, uint32_t index)
{
    (void)dimension;
    if (!is_kind(image, KIND_IMAGE)) return InvalidParameter;
    return index == 0 ? Ok : InvalidParameter;
}

MS uint32_t beer_GdipGetPropertyItemSize(GpImage *image, uint32_t id, uint32_t *size)
    { (void)image; (void)id; if (size) *size = 0; return PropertyNotFound; }
MS uint32_t beer_GdipGetPropertyItem(GpImage *image, uint32_t id, uint32_t size, void *item)
    { (void)image; (void)id; (void)size; (void)item; return PropertyNotFound; }

/* BitmapData: Width, Height, Stride, PixelFormat, Scan0, Reserved. */
struct BitmapData { uint32_t width, height; int32_t stride; uint32_t format; void *scan0; uint64_t reserved; };

MS uint32_t beer_GdipBitmapLockBits(GpImage *image, const int32_t *rect, uint32_t flags, uint32_t format, BitmapData *out)
{
    if (!is_kind(image, KIND_IMAGE) || !out) return InvalidParameter;
    int x = 0, y = 0, w = image->width, h = image->height;
    if (rect) { x = rect[0]; y = rect[1]; w = rect[2]; h = rect[3]; }
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > image->width || y + h > image->height) return InvalidParameter;
    if (format != FMT_32ARGB && format != FMT_32PARGB && format != FMT_32RGB && format != FMT_24RGB) return NotImplemented;
    if (format == image->format) {
        const int bytes = format == FMT_24RGB ? 3 : 4;
        out->scan0 = image->scan0 + (size_t)y * image->stride + (size_t)x * bytes;
        out->stride = image->stride;
    } else {                                                       /* converted copy, written back on unlock */
        GpImage view = { KIND_IMAGE, w, h, format == FMT_24RGB ? ((w * 3 + 3) & ~3) : w * 4, format, NULL, 0, NULL,
                         0, 0, 0, 0, 0, 0 };
        free(image->lock_buffer);
        image->lock_buffer = calloc((size_t)view.stride, (size_t)h);
        if (!image->lock_buffer) return OutOfMemory;
        view.scan0 = image->lock_buffer;
        if (flags & 1u)                                            /* ImageLockModeRead */
            for (int row = 0; row < h; ++row)
                for (int col = 0; col < w; ++col) image_put(&view, col, row, image_get(image, x + col, y + row));
        image->lock_x = x; image->lock_y = y; image->lock_w = w; image->lock_h = h;
        image->lock_write = (flags & 2u) != 0; image->lock_format = format;
        out->scan0 = image->lock_buffer;
        out->stride = view.stride;
    }
    out->width = (uint32_t)w; out->height = (uint32_t)h; out->format = format; out->reserved = 0;
    return Ok;
}

MS uint32_t beer_GdipBitmapUnlockBits(GpImage *image, BitmapData *data)
{
    (void)data;
    if (!is_kind(image, KIND_IMAGE)) return InvalidParameter;
    if (image->lock_buffer) {
        if (image->lock_write) {
            GpImage view = { KIND_IMAGE, image->lock_w, image->lock_h,
                             image->lock_format == FMT_24RGB ? ((image->lock_w * 3 + 3) & ~3) : image->lock_w * 4,
                             image->lock_format, image->lock_buffer, 0, NULL, 0, 0, 0, 0, 0, 0 };
            for (int row = 0; row < image->lock_h; ++row)
                for (int col = 0; col < image->lock_w; ++col)
                    image_put(image, image->lock_x + col, image->lock_y + row, image_get(&view, col, row));
        }
        free(image->lock_buffer);
        image->lock_buffer = NULL;
    }
    return Ok;
}

MS uint32_t beer_GdipCreateHBITMAPFromBitmap(GpImage *image, uint64_t *hbitmap, uint32_t background)
{
    if (!is_kind(image, KIND_IMAGE) || !hbitmap) return InvalidParameter;
    struct { uint32_t size; int32_t width, height; uint16_t planes, bpp; uint32_t compression, image_size;
             int32_t xppm, yppm; uint32_t used, important; } info = { 40, image->width, -image->height, 1, 32, 0,
                                                                      0, 0, 0, 0, 0 };
    void *bits = NULL;
    *hbitmap = beer_CreateDIBSection(0, &info, 0, &bits, 0, 0);
    if (!*hbitmap || !bits) return GenericError;
    uint32_t *out = bits;
    for (int y = 0; y < image->height; ++y)
        for (int x = 0; x < image->width; ++x)
            out[(size_t)y * image->width + x] = blend_over(background, image_get(image, x, y));
    return Ok;
}

/* ------------------------------------------------------------------ graphics */

static GpGraphics *graphics_new(uint64_t hdc, GpImage *image)
{
    GpGraphics *g = calloc(1, sizeof(*g));
    if (g) { g->kind = KIND_GRAPHICS; g->hdc = hdc; g->image = image; }
    return g;
}

MS uint32_t beer_GdipCreateFromHDC(uint64_t hdc, GpGraphics **graphics)
{
    if (!hdc || !graphics) return InvalidParameter;
    *graphics = graphics_new(hdc, NULL);
    return *graphics ? Ok : OutOfMemory;
}

MS uint32_t beer_GdipGetImageGraphicsContext(GpImage *image, GpGraphics **graphics)
{
    if (!is_kind(image, KIND_IMAGE) || !graphics) return InvalidParameter;
    *graphics = graphics_new(0, image);
    return *graphics ? Ok : OutOfMemory;
}

MS uint32_t beer_GdipDeleteGraphics(GpGraphics *graphics)
{
    if (!is_kind(graphics, KIND_GRAPHICS)) return InvalidParameter;
    graphics->kind = 0;
    free(graphics);
    return Ok;
}

/* Rendering hints: Beer renders one way; accept them so callers carry on. */
MS uint32_t beer_GdipSetInterpolationMode(GpGraphics *g, int mode) { (void)mode; return is_kind(g, KIND_GRAPHICS) ? Ok : InvalidParameter; }
MS uint32_t beer_GdipSetPixelOffsetMode(GpGraphics *g, int mode) { (void)mode; return is_kind(g, KIND_GRAPHICS) ? Ok : InvalidParameter; }
MS uint32_t beer_GdipSetSmoothingMode(GpGraphics *g, int mode) { (void)mode; return is_kind(g, KIND_GRAPHICS) ? Ok : InvalidParameter; }
MS uint32_t beer_GdipSetTextRenderingHint(GpGraphics *g, int hint) { (void)hint; return is_kind(g, KIND_GRAPHICS) ? Ok : InvalidParameter; }

MS uint32_t beer_GdipTranslateWorldTransform(GpGraphics *g, float dx, float dy, int order)
{
    (void)order;                                                   /* translations commute */
    if (!is_kind(g, KIND_GRAPHICS)) return InvalidParameter;
    g->tx += dx; g->ty += dy;
    return Ok;
}

MS uint32_t beer_GdipGraphicsClear(GpGraphics *g, uint32_t color)
{
    if (!is_kind(g, KIND_GRAPHICS)) return InvalidParameter;
    Target t;
    if (!target_open(g, &t)) return GenericError;
    for (int y = 0; y < t.height; ++y)
        for (int x = 0; x < t.width; ++x) target_put(&t, x, y, color);
    return Ok;
}

static uint32_t draw_image(GpGraphics *g, GpImage *im, int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh)
{
    if (!is_kind(g, KIND_GRAPHICS) || !is_kind(im, KIND_IMAGE)) return InvalidParameter;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return Ok;
    Target t;
    if (!target_open(g, &t)) return GenericError;
    dx += (int)g->tx; dy += (int)g->ty;
    for (int y = 0; y < dh; ++y) {
        const int ty = dy + y;
        if (ty < 0 || ty >= t.height) continue;
        const int iy = sy + (int)((int64_t)y * sh / dh);
        if (iy < 0 || iy >= im->height) continue;
        for (int x = 0; x < dw; ++x) {
            const int tx = dx + x;
            if (tx < 0 || tx >= t.width) continue;
            const int ix = sx + (int)((int64_t)x * sw / dw);
            if (ix < 0 || ix >= im->width) continue;
            target_blend(&t, tx, ty, image_get(im, ix, iy));
        }
    }
    return Ok;
}

MS uint32_t beer_GdipDrawImageRectI(GpGraphics *g, GpImage *im, int x, int y, int w, int h)
{
    return is_kind(im, KIND_IMAGE) ? draw_image(g, im, x, y, w, h, 0, 0, im->width, im->height) : InvalidParameter;
}

MS uint32_t beer_GdipDrawImageRectRectI(GpGraphics *g, GpImage *im, int dx, int dy, int dw, int dh, int sx, int sy,
                                        int sw, int sh, int unit, void *attributes, void *callback, void *data)
{
    (void)unit; (void)attributes; (void)callback; (void)data;     /* UnitPixel; image attributes not applied */
    return draw_image(g, im, dx, dy, dw, dh, sx, sy, sw, sh);
}

/* ------------------------------------------------------------------ brushes */

static GpBrush *brush_new(int type)
{
    GpBrush *b = calloc(1, sizeof(*b));
    if (b) { b->kind = KIND_BRUSH; b->type = type; }
    return b;
}

MS uint32_t beer_GdipCreateSolidFill(uint32_t color, GpBrush **brush)
{
    if (!brush) return InvalidParameter;
    *brush = brush_new(0);
    if (!*brush) return OutOfMemory;
    (*brush)->color1 = color;
    return Ok;
}

MS uint32_t beer_GdipCreateLineBrushI(const int32_t *p1, const int32_t *p2, uint32_t c1, uint32_t c2, int wrap, GpBrush **brush)
{
    (void)wrap;
    if (!p1 || !p2 || !brush) return InvalidParameter;
    *brush = brush_new(1);
    if (!*brush) return OutOfMemory;
    (*brush)->color1 = c1; (*brush)->color2 = c2;
    (*brush)->x1 = (float)p1[0]; (*brush)->y1 = (float)p1[1]; (*brush)->x2 = (float)p2[0]; (*brush)->y2 = (float)p2[1];
    return Ok;
}

MS uint32_t beer_GdipCreateTexture(GpImage *image, int wrap, GpBrush **brush)
{
    (void)wrap;
    if (!is_kind(image, KIND_IMAGE) || !brush) return InvalidParameter;
    GpImage *copy = NULL;
    const uint32_t status = beer_GdipCloneImage(image, &copy);
    if (status != Ok) return status;
    *brush = brush_new(2);
    if (!*brush) { image_free(copy); return OutOfMemory; }
    (*brush)->texture = copy;
    return Ok;
}

MS uint32_t beer_GdipCloneBrush(GpBrush *brush, GpBrush **clone)
{
    if (!is_kind(brush, KIND_BRUSH) || !clone) return InvalidParameter;
    *clone = brush_new(brush->type);
    if (!*clone) return OutOfMemory;
    GpImage *texture = NULL;
    if (brush->texture && beer_GdipCloneImage(brush->texture, &texture) != Ok) { free(*clone); return OutOfMemory; }
    **clone = *brush;
    (*clone)->texture = texture;
    return Ok;
}

MS uint32_t beer_GdipDeleteBrush(GpBrush *brush)
{
    if (!is_kind(brush, KIND_BRUSH)) return InvalidParameter;
    image_free(brush->texture);
    brush->kind = 0;
    free(brush);
    return Ok;
}

static uint32_t lerp_color(uint32_t a, uint32_t b, float t)
{
    if (t <= 0.f) return a;
    if (t >= 1.f) return b;
    uint32_t out = 0;
    for (int shift = 0; shift <= 24; shift += 8) {
        const float ca = (float)((a >> shift) & 0xff), cb = (float)((b >> shift) & 0xff);
        out |= (uint32_t)(ca + (cb - ca) * t + 0.5f) << shift;
    }
    return out;
}

static uint32_t brush_color(const GpBrush *b, int x, int y)
{
    if (b->type == 1) {
        const float vx = b->x2 - b->x1, vy = b->y2 - b->y1, len2 = vx * vx + vy * vy;
        const float t = len2 > 0.f ? (((float)x - b->x1) * vx + ((float)y - b->y1) * vy) / len2 : 0.f;
        return lerp_color(b->color1, b->color2, t);
    }
    if (b->type == 2 && b->texture) {
        const int w = b->texture->width, h = b->texture->height;
        return image_get(b->texture, ((x % w) + w) % w, ((y % h) + h) % h);
    }
    return b->color1;
}

MS uint32_t beer_GdipFillEllipseI(GpGraphics *g, GpBrush *brush, int x, int y, int w, int h)
{
    if (!is_kind(g, KIND_GRAPHICS) || !is_kind(brush, KIND_BRUSH)) return InvalidParameter;
    if (w <= 0 || h <= 0) return Ok;
    Target t;
    if (!target_open(g, &t)) return GenericError;
    x += (int)g->tx; y += (int)g->ty;
    const float rx = (float)w / 2.f, ry = (float)h / 2.f, cx = (float)x + rx, cy = (float)y + ry;
    for (int py = y; py < y + h; ++py)
        for (int px = x; px < x + w; ++px) {
            const float nx = ((float)px + 0.5f - cx) / rx, ny = ((float)py + 0.5f - cy) / ry;
            if (nx * nx + ny * ny <= 1.f) target_blend(&t, px, py, brush_color(brush, px - (int)g->tx, py - (int)g->ty));
        }
    return Ok;
}

/* ------------------------------------------------------------------ text */

static GpFont *font_new(void)
{
    GpFont *f = calloc(1, sizeof(*f));
    if (f) f->kind = KIND_FONT;
    return f;
}

MS uint32_t beer_GdipCreateFontFromLogfontA(uint64_t hdc, const uint8_t *logfont, GpFont **font)
{
    (void)hdc;
    if (!logfont || !font) return InvalidParameter;
    *font = font_new();
    if (!*font) return OutOfMemory;
    memcpy((*font)->logfont, logfont, 28);                         /* the numeric fields */
    for (int i = 0; i < 31 && logfont[28 + i]; ++i) {              /* LF_FACESIZE: ANSI face name to UTF-16 */
        (*font)->logfont[28 + i * 2] = logfont[28 + i];
        (*font)->logfont[29 + i * 2] = 0;
    }
    return Ok;
}

MS uint32_t beer_GdipCreateFontFromDC(uint64_t hdc, GpFont **font)
{
    (void)hdc;
    if (!font) return InvalidParameter;
    *font = font_new();
    if (!*font) return OutOfMemory;
    const int32_t height = -12;                                    /* the DC's font is not tracked: a default UI size */
    memcpy((*font)->logfont, &height, 4);
    return Ok;
}

MS uint32_t beer_GdipDeleteFont(GpFont *font)
{
    if (!is_kind(font, KIND_FONT)) return InvalidParameter;
    font->kind = 0;
    free(font);
    return Ok;
}

MS uint32_t beer_GdipCreateStringFormat(int flags, uint16_t language, GpFormat **format)
{
    (void)language;
    if (!format) return InvalidParameter;
    *format = calloc(1, sizeof(**format));
    if (!*format) return OutOfMemory;
    (*format)->kind = KIND_FORMAT;
    (*format)->flags = (uint32_t)flags;
    return Ok;
}

MS uint32_t beer_GdipDeleteStringFormat(GpFormat *format)
{
    if (!is_kind(format, KIND_FORMAT)) return InvalidParameter;
    format->kind = 0;
    free(format);
    return Ok;
}

MS uint32_t beer_GdipSetStringFormatAlign(GpFormat *format, int align)
{
    if (!is_kind(format, KIND_FORMAT)) return InvalidParameter;
    format->align = align;
    return Ok;
}

MS uint32_t beer_GdipSetStringFormatLineAlign(GpFormat *format, int align)
{
    if (!is_kind(format, KIND_FORMAT)) return InvalidParameter;
    format->line_align = align;
    return Ok;
}

/* Text on an HDC through Beer's GDI DrawTextW, in the brush's colour; text on images is not rendered. */
MS uint32_t beer_GdipDrawString(GpGraphics *g, const uint16_t *text, int length, GpFont *font, const float *rect,
                                GpFormat *format, GpBrush *brush)
{
    if (!is_kind(g, KIND_GRAPHICS) || !text || !rect || !is_kind(brush, KIND_BRUSH)) return InvalidParameter;
    if (!g->hdc) return Ok;
    int32_t box[4] = { (int32_t)(rect[0] + g->tx), (int32_t)(rect[1] + g->ty),
                       (int32_t)(rect[0] + rect[2] + g->tx), (int32_t)(rect[1] + rect[3] + g->ty) };
    uint32_t flags = 0x0010;                                       /* DT_WORDBREAK */
    if (format && is_kind(format, KIND_FORMAT)) {
        if (format->align == 1) flags |= 0x0001;                   /* DT_CENTER */
        else if (format->align == 2) flags |= 0x0002;              /* DT_RIGHT */
        if (format->line_align == 1) flags = (flags & ~0x0010u) | 0x0004 | 0x0020;   /* DT_VCENTER | DT_SINGLELINE */
        else if (format->line_align == 2) flags = (flags & ~0x0010u) | 0x0008 | 0x0020;   /* DT_BOTTOM */
        if (format->flags & 0x1000) flags |= 0x0100;               /* StringFormatFlagsNoWrap -> DT_NOCLIP-ish */
    }
    const uint64_t hfont = is_kind(font, KIND_FONT) ? beer_CreateFontIndirectW(font->logfont) : 0;
    const uint64_t previous = hfont ? beer_SelectObject(g->hdc, hfont) : 0;
    const uint32_t argb = brush_color(brush, box[0], box[1]);
    const uint32_t old_color = beer_SetTextColor(g->hdc, (argb >> 16 & 0xff) | (argb & 0xff00) | (argb & 0xff) << 16);
    const int old_mode = beer_SetBkMode(g->hdc, 1 /* TRANSPARENT */);
    beer_DrawTextW(g->hdc, text, length, box, flags);
    beer_SetBkMode(g->hdc, old_mode);
    beer_SetTextColor(g->hdc, old_color);
    if (hfont) { beer_SelectObject(g->hdc, previous); beer_DeleteObject(hfont); }
    return Ok;
}
