#include "beer_icon_png.h"

#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static const uint8_t PNG_SIGNATURE[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

int beer_icon_is_png(const uint8_t *data, size_t size)
{
    return data && size >= 8 && !memcmp(data, PNG_SIGNATURE, 8);
}

int beer_icon_png_size(const uint8_t *data, size_t size, int *width, int *height)
{
    if (!beer_icon_is_png(data, size) || size < 33 || memcmp(data + 12, "IHDR", 4)) return 0;
    uint32_t w = be32(data + 16), h = be32(data + 20);
    if (!w || !h || w > 4096 || h > 4096) return 0;
    *width = (int)w;
    *height = (int)h;
    return 1;
}

static uint8_t paeth(uint8_t a, uint8_t b, uint8_t c)
{
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

int beer_icon_decode_png_argb(const uint8_t *data, size_t size, uint32_t *pixels, size_t capacity,
                              int *out_width, int *out_height)
{
    int width, height;
    if (!pixels || !out_width || !out_height || !beer_icon_png_size(data, size, &width, &height)) return 0;
    const uint8_t depth = data[24], color = data[25], interlace = data[28];
    static const int channels_of[7] = { 1, 0, 3, 1, 2, 0, 4 };
    if (depth != 8 || color > 6 || !channels_of[color] || interlace || data[26] || data[27]) return 0;
    if ((size_t)width * (size_t)height > capacity) return 0;
    const size_t channels = (size_t)channels_of[color], stride = (size_t)width * channels;

    uint8_t palette[256][4];
    int palette_size = 0;
    for (int i = 0; i < 256; i++) { palette[i][0] = palette[i][1] = palette[i][2] = 0; palette[i][3] = 0xff; }
    uint8_t *packed = NULL;
    size_t packed_len = 0;
    for (size_t at = 8; at + 12 <= size;) {
        uint32_t len = be32(data + at);
        const uint8_t *type = data + at + 4, *body = data + at + 8;
        if (len > size - at - 12) break;
        if (!memcmp(type, "PLTE", 4) && len % 3 == 0 && len <= 768) {
            palette_size = (int)(len / 3);
            for (int i = 0; i < palette_size; i++) memcpy(palette[i], body + 3 * i, 3);
        } else if (!memcmp(type, "tRNS", 4) && color == 3) {
            for (uint32_t i = 0; i < len && i < 256; i++) palette[i][3] = body[i];
        } else if (!memcmp(type, "IDAT", 4)) {
            uint8_t *grown = realloc(packed, packed_len + len + 1);
            if (!grown) { free(packed); return 0; }
            packed = grown;
            memcpy(packed + packed_len, body, len);
            packed_len += len;
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        at += 12 + (size_t)len;
    }
    if (!packed || (color == 3 && !palette_size)) { free(packed); return 0; }

    const size_t raw_len = (stride + 1) * (size_t)height;
    uint8_t *raw = malloc(raw_len);
    uLongf got = (uLongf)raw_len;
    int ok = raw && uncompress(raw, &got, packed, (uLong)packed_len) == Z_OK && got == raw_len;
    free(packed);
    if (!ok) { free(raw); return 0; }

    /* Undo the per-row filters in place (bytes-per-pixel is the channel count at 8-bit depth). */
    for (int y = 0; y < height && ok; y++) {
        uint8_t *row = raw + (size_t)y * (stride + 1), filter = row[0];
        uint8_t *cur = row + 1;
        const uint8_t *prev = y ? raw + (size_t)(y - 1) * (stride + 1) + 1 : NULL;
        for (size_t x = 0; x < stride; x++) {
            uint8_t a = x >= channels ? cur[x - channels] : 0, b = prev ? prev[x] : 0;
            uint8_t c = prev && x >= channels ? prev[x - channels] : 0;
            switch (filter) {
            case 0: break;
            case 1: cur[x] = (uint8_t)(cur[x] + a); break;
            case 2: cur[x] = (uint8_t)(cur[x] + b); break;
            case 3: cur[x] = (uint8_t)(cur[x] + ((a + b) >> 1)); break;
            case 4: cur[x] = (uint8_t)(cur[x] + paeth(a, b, c)); break;
            default: ok = 0; break;
            }
        }
    }
    for (int y = 0; y < height && ok; y++) {
        const uint8_t *row = raw + (size_t)y * (stride + 1) + 1;
        for (int x = 0; x < width; x++) {
            const uint8_t *p = row + (size_t)x * channels;
            uint8_t r, gr, b, alpha = 0xff;
            switch (color) {
            case 0: r = gr = b = p[0]; break;
            case 2: r = p[0]; gr = p[1]; b = p[2]; break;
            case 3: r = palette[p[0]][0]; gr = palette[p[0]][1]; b = palette[p[0]][2]; alpha = palette[p[0]][3]; break;
            case 4: r = gr = b = p[0]; alpha = p[1]; break;
            default: r = p[0]; gr = p[1]; b = p[2]; alpha = p[3]; break;
            }
            pixels[(size_t)y * (size_t)width + (size_t)x] =
                (uint32_t)alpha << 24 | (uint32_t)r << 16 | (uint32_t)gr << 8 | b;
        }
    }
    free(raw);
    if (!ok) return 0;
    *out_width = width;
    *out_height = height;
    return 1;
}
