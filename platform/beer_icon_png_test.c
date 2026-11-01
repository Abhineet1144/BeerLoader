/* PNG icon decoding: colour types, row filters, palette transparency, and rejected variants. */
#include "beer_icon_png.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>

static size_t put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; return 4; }
static size_t chunk(uint8_t *out, const char *type, const uint8_t *data, uint32_t len)
{
    size_t n = put32(out, len);
    memcpy(out + n, type, 4);
    memcpy(out + n + 4, data, len);
    n += 4 + len;
    n += put32(out + n, (uint32_t)crc32(crc32(0, (const Bytef *)type, 4), data, len));
    return n;
}
/* rows: already filtered scanlines (filter byte first). */
static size_t make_png(uint8_t *out, uint32_t w, uint32_t h, uint8_t depth, uint8_t color, uint8_t interlace,
                       const uint8_t *rows, size_t rows_len, const uint8_t *plte, uint32_t plte_len,
                       const uint8_t *trns, uint32_t trns_len)
{
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    uint8_t ihdr[13];
    put32(ihdr, w);
    put32(ihdr + 4, h);
    ihdr[8] = depth; ihdr[9] = color; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = interlace;
    size_t n = 0;
    memcpy(out, sig, 8);
    n = 8;
    n += chunk(out + n, "IHDR", ihdr, 13);
    if (plte) n += chunk(out + n, "PLTE", plte, plte_len);
    if (trns) n += chunk(out + n, "tRNS", trns, trns_len);
    uint8_t packed[1024];
    uLongf packed_len = sizeof(packed);
    assert(compress(packed, &packed_len, rows, (uLong)rows_len) == Z_OK);
    n += chunk(out + n, "IDAT", packed, (uint32_t)packed_len);
    n += chunk(out + n, "IEND", (const uint8_t *)"", 0);
    return n;
}

int main(void)
{
    uint8_t png[2048];
    uint32_t px[16];
    int w, h;

    /* RGBA 2x2: row 0 Sub-filtered, row 1 Up-filtered. */
    const uint8_t rgba_rows[] = {
        1, 10, 20, 30, 255, 5, 5, 5, 0,                      /* (10,20,30,255) then (15,25,35,255) */
        2, 1, 1, 1, 0, 1, 1, 1, 0,                           /* each pixel = above + 1 in RGB */
    };
    size_t n = make_png(png, 2, 2, 8, 6, 0, rgba_rows, sizeof(rgba_rows), NULL, 0, NULL, 0);
    assert(beer_icon_is_png(png, n) && beer_icon_png_size(png, n, &w, &h) && w == 2 && h == 2);
    assert(beer_icon_decode_png_argb(png, n, px, 16, &w, &h) && w == 2 && h == 2);
    assert(px[0] == 0xff0a141eu && px[1] == 0xff0f1923u && px[2] == 0xff0b151fu && px[3] == 0xff101a24u);

    /* Palette with tRNS; Paeth filter on the second row (above == left == 0 delta keeps the index). */
    const uint8_t plte[] = { 255, 0, 0, 0, 0, 255 };
    const uint8_t trns[] = { 128 };
    const uint8_t pal_rows[] = { 0, 0, 1, 4, 0, 0 };
    n = make_png(png, 2, 2, 8, 3, 0, pal_rows, sizeof(pal_rows), plte, sizeof(plte), trns, sizeof(trns));
    assert(beer_icon_decode_png_argb(png, n, px, 16, &w, &h));
    assert(px[0] == 0x80ff0000u && px[1] == 0xff0000ffu && px[2] == 0x80ff0000u && px[3] == 0xff0000ffu);

    /* Grey 1x1 RGB-free path, capacity too small, interlaced and 16-bit images are rejected. */
    const uint8_t grey_rows[] = { 0, 200 };
    n = make_png(png, 1, 1, 8, 0, 0, grey_rows, sizeof(grey_rows), NULL, 0, NULL, 0);
    assert(beer_icon_decode_png_argb(png, n, px, 16, &w, &h) && px[0] == 0xffc8c8c8u);
    assert(!beer_icon_decode_png_argb(png, n, px, 0, &w, &h));
    n = make_png(png, 1, 1, 8, 0, 1, grey_rows, sizeof(grey_rows), NULL, 0, NULL, 0);
    assert(!beer_icon_decode_png_argb(png, n, px, 16, &w, &h));
    const uint8_t deep_rows[] = { 0, 1, 2 };
    n = make_png(png, 1, 1, 16, 0, 0, deep_rows, sizeof(deep_rows), NULL, 0, NULL, 0);
    assert(!beer_icon_decode_png_argb(png, n, px, 16, &w, &h));
    assert(!beer_icon_is_png((const uint8_t *)"\x28\0\0\0", 4));
    puts("PNG icon decoding tests passed");
    return 0;
}
