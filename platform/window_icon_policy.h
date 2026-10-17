#ifndef BEER_WINDOW_ICON_POLICY_H
#define BEER_WINDOW_ICON_POLICY_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Produces a desktop-file/icon-safe identifier without embedding policy for
 * any particular executable. Keep the mapping deterministic so WM_CLASS and
 * StartupWMClass can be associated across launches. */
static inline int beer_icon_sanitize_app_id(const char *source,
                                            char *target, size_t capacity)
{
    if (!source || !target || capacity < 2) return 0;
    size_t written = 0;
    for (size_t i = 0; source[i]; ++i) {
        unsigned char value = (unsigned char)source[i];
        char output = ((value >= 'a' && value <= 'z') ||
                       (value >= 'A' && value <= 'Z') ||
                       (value >= '0' && value <= '9') ||
                       value == '-' || value == '_')
            ? (char)value : '-';
        if (written && output == '-' && target[written - 1] == '-') continue;
        if (written + 1 >= capacity) return 0;
        target[written++] = output;
    }
    while (written && target[written - 1] == '-') --written;
    if (!written) return 0;
    target[written] = 0;
    return 1;
}

static inline uint16_t beer_icon_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static inline uint32_t beer_icon_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Decodes the uncompressed DIB payload stored by a standard PE RT_ICON.
 * Windows icon DIBs contain the XOR bitmap followed by a 1-bit AND mask, and
 * report twice the visible height in BITMAPINFOHEADER. Output uses EWMH's
 * 0xAARRGGBB pixel representation. */
static inline int beer_icon_scale_argb(const uint32_t *source,
                                       int source_width, int source_height,
                                       uint32_t *target,
                                       int target_width, int target_height)
{
    if (!source || !target || source_width <= 0 || source_height <= 0 ||
        target_width <= 0 || target_height <= 0)
        return 0;
    for (int y = 0; y < target_height; ++y) {
        int source_y = (int)((int64_t)y * source_height / target_height);
        for (int x = 0; x < target_width; ++x) {
            int source_x = (int)((int64_t)x * source_width / target_width);
            target[(size_t)y * (size_t)target_width + (size_t)x] =
                source[(size_t)source_y * (size_t)source_width + (size_t)source_x];
        }
    }
    return 1;
}

static inline int beer_icon_decode_dib_argb(const uint8_t *data, size_t size,
                                            uint32_t *pixels,
                                            int *out_width, int *out_height)
{
    if (!data || size < 40 || !pixels || !out_width || !out_height) return 0;
    uint32_t header_size = beer_icon_u32(data);
    int32_t width = (int32_t)beer_icon_u32(data + 4);
    int32_t stored_height = (int32_t)beer_icon_u32(data + 8);
    uint16_t planes = beer_icon_u16(data + 12);
    uint16_t bits = beer_icon_u16(data + 14);
    uint32_t compression = beer_icon_u32(data + 16);
    if (header_size < 40 || header_size > size || width <= 0 ||
        stored_height == 0 || planes != 1 || (bits != 24 && bits != 32) ||
        compression != 0)
        return 0;

    int top_down = stored_height < 0;
    uint32_t total_height = (uint32_t)(top_down ? -stored_height : stored_height);
    if ((total_height & 1u) || total_height < 2u) return 0;
    uint32_t height = total_height / 2u;
    uint32_t row_bytes = ((uint32_t)width * bits + 31u) / 32u * 4u;
    uint32_t mask_row_bytes = ((uint32_t)width + 31u) / 32u * 4u;
    size_t xor_bytes = (size_t)row_bytes * height;
    size_t mask_bytes = (size_t)mask_row_bytes * height;
    if (header_size > size || xor_bytes > size - header_size ||
        mask_bytes > size - header_size - xor_bytes)
        return 0;

    const uint8_t *xor_bitmap = data + header_size;
    const uint8_t *and_mask = xor_bitmap + xor_bytes;
    int any_alpha = 0;
    if (bits == 32) {
        for (size_t i = 3; i < xor_bytes; i += 4)
            any_alpha |= xor_bitmap[i] != 0;
    }

    for (uint32_t y = 0; y < height; ++y) {
        uint32_t source_y = top_down ? y : height - 1u - y;
        const uint8_t *source = xor_bitmap + (size_t)source_y * row_bytes;
        const uint8_t *mask = and_mask + (size_t)source_y * mask_row_bytes;
        for (uint32_t x = 0; x < (uint32_t)width; ++x) {
            uint8_t blue = source[(size_t)x * (bits / 8u) + 0u];
            uint8_t green = source[(size_t)x * (bits / 8u) + 1u];
            uint8_t red = source[(size_t)x * (bits / 8u) + 2u];
            uint8_t alpha = bits == 32 && any_alpha
                ? source[(size_t)x * 4u + 3u] : 0xffu;
            if (mask[x / 8u] & (uint8_t)(0x80u >> (x & 7u))) alpha = 0;
            pixels[(size_t)y * (uint32_t)width + x] =
                ((uint32_t)alpha << 24) | ((uint32_t)red << 16) |
                ((uint32_t)green << 8) | blue;
        }
    }
    *out_width = width;
    *out_height = (int)height;
    return 1;
}

#endif
