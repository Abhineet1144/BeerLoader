#include "window_style_policy.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    const uint32_t overlapped = UINT32_C(0x00cf0000);
    assert(beer_window_style_is_resizable(overlapped));
    assert(beer_window_style_is_decorated(overlapped));
    assert(!beer_window_request_is_fullscreen(overlapped, 0, 0,
                                               1920, 1080, 1920, 1080));

    const uint32_t fixed_caption = UINT32_C(0x00c80000);
    assert(!beer_window_style_is_resizable(fixed_caption));
    assert(beer_window_style_is_decorated(fixed_caption));
    assert(beer_window_request_is_fullscreen(fixed_caption, 0, 0,
                                              1936, 1118, 1920, 1080));
    assert(!beer_window_request_is_fullscreen(fixed_caption, 100, 100,
                                               1280, 720, 1920, 1080));

    const uint32_t borderless = BEER_WS_POPUP;
    assert(!beer_window_style_is_resizable(borderless));
    assert(!beer_window_style_is_decorated(borderless));
    assert(beer_window_request_is_fullscreen(borderless, 0, 0,
                                              1920, 1080, 1920, 1080));
    assert(!beer_window_request_is_fullscreen(borderless, 40, 40,
                                               1280, 720, 1920, 1080));

    int client_x = -1, client_y = -1, client_width = 0, client_height = 0;
    assert(beer_window_client_from_outer(1936, 1118, 1, &client_x, &client_y,
                                         &client_width, &client_height));
    assert(client_x == 0 && client_y == 0);
    assert(client_width == 1920 && client_height == 1080);
    assert(beer_window_client_from_outer(1920, 1080, 0, &client_x, &client_y,
                                         &client_width, &client_height));
    assert(client_x == 0 && client_y == 0);
    assert(client_width == 1920 && client_height == 1080);

    int outer_width = 0, outer_height = 0;
    assert(beer_window_outer_from_client(1920, 1080, 1,
                                         &outer_width, &outer_height));
    assert(outer_width == 1936 && outer_height == 1118);
    assert(beer_window_outer_from_client(1920, 1080, 0,
                                         &outer_width, &outer_height));
    assert(outer_width == 1920 && outer_height == 1080);

    int mapped_x = -1, mapped_y = -1;
    assert(beer_window_map_client_point(960, 540, 1920, 1080,
                                        1920, 1080, &mapped_x, &mapped_y));
    assert(mapped_x == 960 && mapped_y == 540);
    assert(beer_window_map_client_point(952, 521, 1904, 1042,
                                        1920, 1080, &mapped_x, &mapped_y));
    assert(mapped_x == 960 && mapped_y == 540);
    /* A compositor fullscreen resize must update the Win32 client extent to
     * the configured drawable. Feeding an old 2560x1440 client size into a
     * 1920x1080 drawable reproduces the observed 4/3 cursor drift. */
    assert(beer_window_map_client_point(1440, 810, 1920, 1080,
                                        1920, 1080, &mapped_x, &mapped_y));
    assert(mapped_x == 1440 && mapped_y == 810);
    assert(!beer_window_map_client_point(1, 1, 0, 1080,
                                         1920, 1080, &mapped_x, &mapped_y));

    puts("Window style policy tests passed");
    return 0;
}
