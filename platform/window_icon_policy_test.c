#include "window_icon_policy.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    char app_id[64];
    assert(beer_icon_sanitize_app_id("Example Game.exe", app_id,
                                     sizeof(app_id)));
    assert(strcmp(app_id, "Example-Game-exe") == 0);
    assert(!beer_icon_sanitize_app_id("---", app_id, sizeof(app_id)));

    /* 2x2, 32-bpp, bottom-up XOR bitmap plus one 32-bit AND-mask row per
     * visible row. BITMAPINFOHEADER reports doubled icon height (4). */
    uint8_t dib[40 + 16 + 8] = {0};
    dib[0] = 40;
    dib[4] = 2;
    dib[8] = 4;
    dib[12] = 1;
    dib[14] = 32;
    uint8_t *xor_bitmap = dib + 40;
    /* Bottom source row: blue, white. */
    xor_bitmap[0] = 0xff; xor_bitmap[3] = 0xff;
    xor_bitmap[4] = 0xff; xor_bitmap[5] = 0xff;
    xor_bitmap[6] = 0xff; xor_bitmap[7] = 0xff;
    /* Top source row: red, green. */
    xor_bitmap[8 + 2] = 0xff; xor_bitmap[8 + 3] = 0xff;
    xor_bitmap[12 + 1] = 0xff; xor_bitmap[12 + 3] = 0xff;
    uint32_t pixels[4] = {0};
    int width = 0, height = 0;
    assert(beer_icon_decode_dib_argb(dib, sizeof(dib), pixels,
                                     &width, &height));
    assert(width == 2 && height == 2);
    assert(pixels[0] == UINT32_C(0xffff0000));
    assert(pixels[1] == UINT32_C(0xff00ff00));
    assert(pixels[2] == UINT32_C(0xff0000ff));
    assert(pixels[3] == UINT32_C(0xffffffff));

    /* With every alpha byte zero, Windows uses the AND mask for transparency. */
    for (size_t i = 3; i < 16; i += 4) xor_bitmap[i] = 0;
    dib[40 + 16 + 4] = 0x80; /* mask top-left after bottom-up row inversion */
    assert(beer_icon_decode_dib_argb(dib, sizeof(dib), pixels,
                                     &width, &height));
    assert((pixels[0] >> 24) == 0);
    assert((pixels[1] >> 24) == 0xff);

    /* Desktop icon publication bounds large PE icons while preserving the
     * complete image rather than truncating its X11 property payload. */
    const uint32_t source[4] = {
        UINT32_C(0xffff0000), UINT32_C(0xff00ff00),
        UINT32_C(0xff0000ff), UINT32_C(0xffffffff)
    };
    uint32_t scaled[16] = {0};
    assert(beer_icon_scale_argb(source, 2, 2, scaled, 4, 4));
    assert(scaled[0] == source[0]);
    assert(scaled[3] == source[1]);
    assert(scaled[12] == source[2]);
    assert(scaled[15] == source[3]);

    puts("Window icon policy tests passed");
    return 0;
}
