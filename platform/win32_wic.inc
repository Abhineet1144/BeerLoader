/* Windows Imaging Component (windowscodecs.dll) — the part programs use to load a still image:
 *   IWICImagingFactory::CreateDecoderFromFilename -> IWICBitmapDecoder::GetFrame(0) -> IWICBitmapFrameDecode,
 *   IWICImagingFactory::CreateFormatConverter -> IWICFormatConverter::Initialize(frame, format) -> CopyPixels.
 * Unreal Engine's Windows splash screen loads Content/Splash/Splash.bmp exactly this way and does not check the
 * factory's creation result, so a missing WIC crashed the splash thread (Stellar Blade).
 *
 * Decoders: BMP (1/4/8-bit palette, 16-bit 5-5-5, 24-bit, 32-bit; bottom-up or top-down; BI_RGB/BI_BITFIELDS).
 * Other containers answer WINCODEC_ERR_COMPONENTNOTFOUND, as Windows does without a codec for them.
 * Every frame is decoded to 32-bit BGRA; the converter writes 32bpp BGR/BGRA/PBGRA/RGBA/PRGBA and 24bpp BGR/RGB.
 * Included after platform/win32_wmi.inc (GUID helper, E_NOTIMPL slot). */

#define WIC_E_NOINTERFACE        UINT64_C(0x80004002)
#define WIC_E_POINTER            UINT64_C(0x80004003)
#define WIC_E_INVALIDARG         UINT64_C(0x80070057)
#define WIC_E_OUTOFMEMORY        UINT64_C(0x8007000e)
#define WIC_E_FILENOTFOUND       UINT64_C(0x80070002)
#define WIC_E_NOTINITIALIZED     UINT64_C(0x88982f0c)
#define WIC_E_COMPONENTNOTFOUND  UINT64_C(0x88982f50)
#define WIC_E_BADIMAGE           UINT64_C(0x88982f60)
#define WIC_E_UNSUPPORTEDFORMAT  UINT64_C(0x88982f80)
#define WIC_E_INSUFFICIENTBUFFER UINT64_C(0x88982f8c)

typedef struct { _Atomic(u32) refs; u32 width, height; u8 *bgra; } WicImage;      /* top-down, 4 bytes/pixel */
enum { WIC_FACTORY, WIC_DECODER, WIC_FRAME, WIC_CONVERTER };
typedef struct {
    void **vtbl;
    _Atomic(u32) refs;
    int kind;
    WicImage *image;               /* decoder, frame, initialised converter */
    u8 format[16];                 /* converter output format */
} WicObject;

static void wic_image_release(WicImage *image)
{
    if (image && --image->refs == 0) { free(image->bgra); free(image); }
}

/* {6fddc324-4e03-4bfe-b185-3d77768dc9xx}: the WIC native pixel formats, told apart by the last byte. */
static int wic_native_format(const u8 *g)
{
    static const u8 base[15] = { 0x24,0xc3,0xdd,0x6f, 0x03,0x4e, 0xfe,0x4b, 0xb1,0x85,0x3d,0x77,0x76,0x8d,0xc9 };
    return g && !memcmp(g, base, 15) ? g[15] : -1;
}
static void wic_set_native_format(u8 *g, u8 last)
{
    static const u8 base[15] = { 0x24,0xc3,0xdd,0x6f, 0x03,0x4e, 0xfe,0x4b, 0xb1,0x85,0x3d,0x77,0x76,0x8d,0xc9 };
    memcpy(g, base, 15);
    g[15] = last;
}
/* Output layouts: bytes per pixel, red/blue swapped against BGRA, alpha premultiplied, alpha forced opaque. */
typedef struct { int bytes, rgb, premultiply, opaque; } WicLayout;
static int wic_layout(const u8 *format, WicLayout *layout)
{
    switch (wic_native_format(format)) {
    case 0x0c: *layout = (WicLayout){ 3, 0, 0, 1 }; return 1;              /* 24bppBGR */
    case 0x0d: *layout = (WicLayout){ 3, 1, 0, 1 }; return 1;              /* 24bppRGB */
    case 0x0e: *layout = (WicLayout){ 4, 0, 0, 1 }; return 1;              /* 32bppBGR */
    case 0x0f: *layout = (WicLayout){ 4, 0, 0, 0 }; return 1;              /* 32bppBGRA */
    case 0x10: *layout = (WicLayout){ 4, 0, 1, 0 }; return 1;              /* 32bppPBGRA */
    default: break;
    }
    if (beer_wmi_guid_is(format, 0xf5c7ad2d, 0x6a8d, 0x43dd, 0xa7, 0xa8, 0xa2, 0x99, 0x35, 0x26, 0x1a, 0xe9)) {
        *layout = (WicLayout){ 4, 1, 0, 0 }; return 1;                     /* 32bppRGBA */
    }
    if (beer_wmi_guid_is(format, 0x3cc4a650, 0xa527, 0x4d37, 0xa9, 0x16, 0x31, 0x42, 0xc7, 0xeb, 0xed, 0xba)) {
        *layout = (WicLayout){ 4, 1, 1, 0 }; return 1;                     /* 32bppPRGBA */
    }
    return 0;
}

/* ---- BMP ---- */
static u32 wic_le32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u16 wic_le16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }

static u64 wic_decode_bmp(const u8 *data, size_t size, WicImage **out)
{
    if (size < 54 || data[0] != 'B' || data[1] != 'M') return WIC_E_COMPONENTNOTFOUND;
    const u32 pixels_at = wic_le32(data + 10), header = wic_le32(data + 14);
    if (header < 40 || 14 + (size_t)header > size) return WIC_E_BADIMAGE;
    const s32 width = (s32)wic_le32(data + 18), raw_height = (s32)wic_le32(data + 22);
    const u16 bpp = wic_le16(data + 28);
    const u32 compression = wic_le32(data + 30);
    u32 colors = wic_le32(data + 46);
    const int top_down = raw_height < 0;
    const u32 height = (u32)(top_down ? -(s64)raw_height : raw_height);
    if (width <= 0 || height == 0 || width > 32768 || height > 32768) return WIC_E_BADIMAGE;
    if (compression != 0 && compression != 3) return WIC_E_UNSUPPORTEDFORMAT;       /* BI_RGB, BI_BITFIELDS */
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) return WIC_E_UNSUPPORTEDFORMAT;
    const size_t stride = (((size_t)width * bpp + 31) / 32) * 4;
    if (pixels_at > size || stride * height > size - pixels_at) return WIC_E_BADIMAGE;
    const u8 *palette = data + 14 + header;
    if (bpp <= 8) {
        if (!colors || colors > (1u << bpp)) colors = 1u << bpp;
        if ((size_t)(palette - data) + colors * 4 > size) return WIC_E_BADIMAGE;
    }
    /* 32-bit images carry alpha only with BI_BITFIELDS masks that include an alpha channel (or a V4+ header);
     * plain BI_RGB 32-bit pixels are opaque. */
    const int has_alpha = bpp == 32 && compression == 3 && header >= 56 && wic_le32(data + 14 + 52) != 0;
    WicImage *image = calloc(1, sizeof(*image));
    if (!image || !(image->bgra = malloc((size_t)width * height * 4))) { free(image); return WIC_E_OUTOFMEMORY; }
    image->refs = 1; image->width = (u32)width; image->height = height;
    for (u32 y = 0; y < height; ++y) {
        const u8 *row = data + pixels_at + stride * (top_down ? y : height - 1 - y);
        u8 *dst = image->bgra + (size_t)y * width * 4;
        for (s32 x = 0; x < width; ++x, dst += 4) {
            u32 index;
            switch (bpp) {
            case 1: index = (row[x >> 3] >> (7 - (x & 7))) & 1u; break;
            case 4: index = (row[x >> 1] >> ((x & 1) ? 0 : 4)) & 15u; break;
            case 8: index = row[x]; break;
            case 16: {
                const u16 v = wic_le16(row + x * 2);                                  /* 5-5-5 */
                dst[0] = (u8)((v & 31) * 255 / 31); dst[1] = (u8)(((v >> 5) & 31) * 255 / 31);
                dst[2] = (u8)(((v >> 10) & 31) * 255 / 31); dst[3] = 255;
                continue;
            }
            case 24: dst[0] = row[x * 3]; dst[1] = row[x * 3 + 1]; dst[2] = row[x * 3 + 2]; dst[3] = 255; continue;
            default: memcpy(dst, row + x * 4, 4); if (!has_alpha) dst[3] = 255; continue;
            }
            if (index >= colors) index = 0;
            dst[0] = palette[index * 4]; dst[1] = palette[index * 4 + 1]; dst[2] = palette[index * 4 + 2]; dst[3] = 255;
        }
    }
    *out = image;
    return 0;
}

/* ---- COM objects ---- */
static void *g_wic_factory_vtbl[28];
static void *g_wic_decoder_vtbl[14];
static void *g_wic_frame_vtbl[11];
static void *g_wic_converter_vtbl[10];

static WicObject *wic_new(int kind, void **vtbl, WicImage *image)
{
    WicObject *o = calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->vtbl = vtbl; o->refs = 1; o->kind = kind; o->image = image;
    if (image) image->refs++;
    return o;
}
static u64 __attribute__((ms_abi)) wic_AddRef(WicObject *o) { return ++o->refs; }
static u64 __attribute__((ms_abi)) wic_Release(WicObject *o)
{
    const u32 left = --o->refs;
    if (!left) { wic_image_release(o->image); free(o); }
    return left;
}
static int wic_iid_is_unknown(const u8 *iid) { return beer_wmi_guid_is(iid, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46); }
static int wic_iid_is_source(const u8 *iid)   /* IWICBitmapSource {00000120-a8f2-4877-ba0a-fd2b6645fb94} */
{
    return beer_wmi_guid_is(iid, 0x00000120, 0xa8f2, 0x4877, 0xba, 0x0a, 0xfd, 0x2b, 0x66, 0x45, 0xfb, 0x94);
}
static u64 __attribute__((ms_abi)) wic_QI(WicObject *o, const u8 *iid, void **out)
{
    if (!out) return WIC_E_POINTER;
    *out = NULL;
    int ok = wic_iid_is_unknown(iid);
    switch (o->kind) {
    case WIC_FACTORY:      /* IWICImagingFactory, IWICImagingFactory2 (whose extra method answers E_NOTIMPL) */
        ok |= beer_wmi_guid_is(iid, 0xec5ec8a9, 0xc395, 0x4314, 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70);
        ok |= beer_wmi_guid_is(iid, 0x7b816b45, 0x1996, 0x4476, 0xb1, 0x32, 0xde, 0x9e, 0x24, 0x7c, 0x8a, 0xf0);
        break;
    case WIC_DECODER:      /* IWICBitmapDecoder */
        ok |= beer_wmi_guid_is(iid, 0x9edde9e7, 0x8dee, 0x47ea, 0x99, 0xdf, 0xe6, 0xfa, 0xf2, 0xed, 0x44, 0xbf);
        break;
    case WIC_FRAME:        /* IWICBitmapFrameDecode */
        ok |= wic_iid_is_source(iid) ||
              beer_wmi_guid_is(iid, 0x3b16811b, 0x6a43, 0x4ec9, 0xa8, 0x13, 0x3d, 0x93, 0x0c, 0x13, 0xb9, 0x40);
        break;
    case WIC_CONVERTER:    /* IWICFormatConverter */
        ok |= wic_iid_is_source(iid) ||
              beer_wmi_guid_is(iid, 0x00000301, 0xa8f2, 0x4877, 0xba, 0x0a, 0xfd, 0x2b, 0x66, 0x45, 0xfb, 0x94);
        break;
    }
    if (!ok) return WIC_E_NOINTERFACE;
    wic_AddRef(o);
    *out = o;
    return 0;
}

/* IWICBitmapSource (frames and converters). */
static u64 __attribute__((ms_abi)) wic_GetSize(WicObject *o, u32 *width, u32 *height)
{
    if (!width || !height) return WIC_E_INVALIDARG;
    if (!o->image) return WIC_E_NOTINITIALIZED;
    *width = o->image->width; *height = o->image->height;
    return 0;
}
static u64 __attribute__((ms_abi)) wic_GetPixelFormat(WicObject *o, u8 *format)
{
    if (!format) return WIC_E_INVALIDARG;
    if (!o->image) return WIC_E_NOTINITIALIZED;
    if (o->kind == WIC_CONVERTER) memcpy(format, o->format, 16);
    else wic_set_native_format(format, 0x0f);                         /* frames decode to 32bppBGRA */
    return 0;
}
static u64 __attribute__((ms_abi)) wic_GetResolution(WicObject *o, double *x, double *y)
{
    (void)o;
    if (!x || !y) return WIC_E_INVALIDARG;
    *x = 96.0; *y = 96.0;
    return 0;
}
static u64 __attribute__((ms_abi)) wic_CopyPalette(void) { return UINT64_C(0x88982f45); }   /* PALETTEUNAVAILABLE */
static u64 __attribute__((ms_abi)) wic_CopyPixels(WicObject *o, const s32 *rect, u32 stride, u32 size, u8 *buffer)
{
    if (!o->image) return WIC_E_NOTINITIALIZED;
    if (!buffer) return WIC_E_INVALIDARG;
    WicLayout layout = { 4, 0, 0, 0 };
    if (o->kind == WIC_CONVERTER && !wic_layout(o->format, &layout)) return WIC_E_UNSUPPORTEDFORMAT;
    const WicImage *image = o->image;
    s32 x0 = 0, y0 = 0, w = (s32)image->width, h = (s32)image->height;
    if (rect) { x0 = rect[0]; y0 = rect[1]; w = rect[2]; h = rect[3]; }
    if (x0 < 0 || y0 < 0 || w < 0 || h < 0 || (u32)(x0 + w) > image->width || (u32)(y0 + h) > image->height)
        return WIC_E_INVALIDARG;
    if (!w || !h) return 0;
    const u64 row_bytes = (u64)w * (u64)layout.bytes;
    if (stride < row_bytes) return WIC_E_INVALIDARG;
    if ((u64)stride * (u64)(h - 1) + row_bytes > size) return WIC_E_INSUFFICIENTBUFFER;
    for (s32 y = 0; y < h; ++y) {
        const u8 *src = image->bgra + ((size_t)(y0 + y) * image->width + (size_t)x0) * 4;
        u8 *dst = buffer + (size_t)y * stride;
        for (s32 x = 0; x < w; ++x, src += 4, dst += layout.bytes) {
            u8 b = src[0], g = src[1], r = src[2], a = layout.opaque ? 255 : src[3];
            if (layout.premultiply) { b = (u8)((b * a + 127) / 255); g = (u8)((g * a + 127) / 255); r = (u8)((r * a + 127) / 255); }
            dst[0] = layout.rgb ? r : b; dst[1] = g; dst[2] = layout.rgb ? b : r;
            if (layout.bytes == 4) dst[3] = a;
        }
    }
    return 0;
}

/* IWICBitmapDecoder. */
static u64 __attribute__((ms_abi)) wic_decoder_GetContainerFormat(WicObject *o, u8 *format)
{
    (void)o;
    if (!format) return WIC_E_INVALIDARG;
    static const u8 bmp[16] = { 0x7e,0xd8,0xf1,0x0a, 0xfe,0xfc, 0x88,0x41, 0xbd,0xeb,0xa7,0x90,0x64,0x71,0xcb,0xe3 };
    memcpy(format, bmp, 16);                                          /* GUID_ContainerFormatBmp */
    return 0;
}
static u64 __attribute__((ms_abi)) wic_decoder_GetFrameCount(WicObject *o, u32 *count)
{
    (void)o;
    if (!count) return WIC_E_INVALIDARG;
    *count = 1;
    return 0;
}
static u64 __attribute__((ms_abi)) wic_decoder_GetFrame(WicObject *o, u32 index, void **frame)
{
    if (!frame) return WIC_E_INVALIDARG;
    *frame = NULL;
    if (index != 0) return WIC_E_INVALIDARG;
    WicObject *f = wic_new(WIC_FRAME, g_wic_frame_vtbl, o->image);
    if (!f) return WIC_E_OUTOFMEMORY;
    *frame = f;
    return 0;
}

/* IWICFormatConverter. */
static u64 __attribute__((ms_abi))
wic_converter_Initialize(WicObject *o, WicObject *source, const u8 *format, u32 dither, void *palette, double alpha_threshold, u32 palette_type)
{
    (void)dither; (void)palette; (void)alpha_threshold; (void)palette_type;
    WicLayout layout;
    if (!source || !format) return WIC_E_INVALIDARG;
    if (!wic_layout(format, &layout)) return WIC_E_UNSUPPORTEDFORMAT;
    /* Sources are Beer's own frames and converters: the vtable identifies them. */
    if (source->vtbl != g_wic_frame_vtbl && source->vtbl != g_wic_converter_vtbl) return WIC_E_INVALIDARG;
    if (!source->image) return WIC_E_NOTINITIALIZED;
    if (o->image) return UINT64_C(0x88982f87);                                       /* WRONGSTATE */
    o->image = source->image;
    o->image->refs++;
    memcpy(o->format, format, 16);
    return 0;
}
static u64 __attribute__((ms_abi)) wic_converter_CanConvert(WicObject *o, const u8 *from, const u8 *to, s32 *can)
{
    (void)o; (void)from;
    WicLayout layout;
    if (!can) return WIC_E_INVALIDARG;
    *can = wic_layout(to, &layout) ? 1 : 0;
    return 0;
}

/* IWICImagingFactory. */
static void wic_wide_to_utf8(const u16 *in, char *out, size_t capacity)
{
    size_t n = 0;
    for (; *in && n + 4 < capacity; ++in) {
        u32 c = *in;
        if (c >= 0xd800 && c < 0xdc00 && in[1] >= 0xdc00 && in[1] < 0xe000) { c = 0x10000 + ((c - 0xd800) << 10) + (in[1] - 0xdc00); ++in; }
        if (c < 0x80) out[n++] = (char)c;
        else if (c < 0x800) { out[n++] = (char)(0xc0 | c >> 6); out[n++] = (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { out[n++] = (char)(0xe0 | c >> 12); out[n++] = (char)(0x80 | ((c >> 6) & 63)); out[n++] = (char)(0x80 | (c & 63)); }
        else { out[n++] = (char)(0xf0 | c >> 18); out[n++] = (char)(0x80 | ((c >> 12) & 63)); out[n++] = (char)(0x80 | ((c >> 6) & 63)); out[n++] = (char)(0x80 | (c & 63)); }
    }
    out[n] = 0;
}
static u64 __attribute__((ms_abi))
wic_factory_CreateDecoderFromFilename(WicObject *o, const u16 *name, const u8 *vendor, u32 access, u32 options, void **decoder)
{
    (void)o; (void)vendor; (void)access; (void)options;
    if (!name || !decoder) return WIC_E_INVALIDARG;
    *decoder = NULL;
    char guest[4096], host[PATH_MAX];
    wic_wide_to_utf8(name, guest, sizeof(guest));
    if (!resolve_windows_host_path(guest, host, sizeof(host))) return WIC_E_FILENOTFOUND;
    FILE *file = fopen(host, "rb");
    if (!file) return WIC_E_FILENOTFOUND;
    u8 *data = NULL;
    long size = 0;
    if (fseek(file, 0, SEEK_END) == 0 && (size = ftell(file)) > 0 && size < (256L << 20) && fseek(file, 0, SEEK_SET) == 0) {
        data = malloc((size_t)size);
        if (data && fread(data, 1, (size_t)size, file) != (size_t)size) { free(data); data = NULL; }
    }
    fclose(file);
    if (!data) return WIC_E_BADIMAGE;
    WicImage *image = NULL;
    u64 hr = wic_decode_bmp(data, (size_t)size, &image);
    free(data);
    fprintf(stderr, "[WIC] CreateDecoderFromFilename(\"%s\") -> 0x%llx (%ux%u)\n", guest, (unsigned long long)hr,
            image ? image->width : 0, image ? image->height : 0);
    if (hr) return hr;
    WicObject *d = wic_new(WIC_DECODER, g_wic_decoder_vtbl, image);
    wic_image_release(image);                                         /* the decoder took its own reference */
    if (!d) return WIC_E_OUTOFMEMORY;
    *decoder = d;
    return 0;
}
static u64 __attribute__((ms_abi)) wic_factory_CreateFormatConverter(WicObject *o, void **converter)
{
    (void)o;
    if (!converter) return WIC_E_INVALIDARG;
    *converter = wic_new(WIC_CONVERTER, g_wic_converter_vtbl, NULL);
    return *converter ? 0 : WIC_E_OUTOFMEMORY;
}

static void wic_vtables_init(void)
{
    static int ready;
    if (ready) return;
    void **tables[] = { g_wic_factory_vtbl, g_wic_decoder_vtbl, g_wic_frame_vtbl, g_wic_converter_vtbl };
    const size_t sizes[] = { 28, 14, 11, 10 };
    for (size_t t = 0; t < 4; ++t) {
        for (size_t i = 0; i < sizes[t]; ++i) tables[t][i] = (void *)wmi_NotImpl;
        tables[t][0] = (void *)wic_QI; tables[t][1] = (void *)wic_AddRef; tables[t][2] = (void *)wic_Release;
    }
    g_wic_factory_vtbl[3] = (void *)wic_factory_CreateDecoderFromFilename;
    g_wic_factory_vtbl[10] = (void *)wic_factory_CreateFormatConverter;
    g_wic_decoder_vtbl[5] = (void *)wic_decoder_GetContainerFormat;
    g_wic_decoder_vtbl[12] = (void *)wic_decoder_GetFrameCount;
    g_wic_decoder_vtbl[13] = (void *)wic_decoder_GetFrame;
    void **sources[] = { g_wic_frame_vtbl, g_wic_converter_vtbl };
    for (size_t s = 0; s < 2; ++s) {
        sources[s][3] = (void *)wic_GetSize; sources[s][4] = (void *)wic_GetPixelFormat;
        sources[s][5] = (void *)wic_GetResolution; sources[s][6] = (void *)wic_CopyPalette;
        sources[s][7] = (void *)wic_CopyPixels;
    }
    g_wic_converter_vtbl[8] = (void *)wic_converter_Initialize;
    g_wic_converter_vtbl[9] = (void *)wic_converter_CanConvert;
    ready = 1;
}

/* CoCreateInstance hook: CLSID_WICImagingFactory {cacaf262-9370-4615-a13b-9f5539da4c0a} and
 * CLSID_WICImagingFactory2 {317d06e8-5f24-433d-bdf7-79ce68d8abc2}. Returns the HRESULT, or 1 when not WIC. */
static u64 beer_wic_create_instance(const u8 *clsid, const u8 *iid, void **out)
{
    if (!beer_wmi_guid_is(clsid, 0xcacaf262, 0x9370, 0x4615, 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a) &&
        !beer_wmi_guid_is(clsid, 0x317d06e8, 0x5f24, 0x433d, 0xbd, 0xf7, 0x79, 0xce, 0x68, 0xd8, 0xab, 0xc2))
        return 1;
    if (!out) return WIC_E_POINTER;
    wic_vtables_init();
    WicObject *factory = wic_new(WIC_FACTORY, g_wic_factory_vtbl, NULL);
    if (!factory) return WIC_E_OUTOFMEMORY;
    const u64 hr = wic_QI(factory, iid, out);
    wic_Release(factory);
    return hr;
}
