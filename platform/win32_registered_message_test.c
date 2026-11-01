#include "win32_registered_message.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    static const uint16_t beer_w[] = {'B','e','e','r','.','E','v','e','n','t',0};
    static const uint16_t lower_w[] = {'b','e','e','r','.','e','v','e','n','t',0};
    uint32_t error = 0x12345678u;

    beer_registered_message_reset_for_test();
    uint32_t first = beer_register_window_message_a("Beer.Event", &error);
    assert(first == 0xC000u);
    assert(error == 0x12345678u);
    assert(beer_register_window_message_w(beer_w, &error) == first);
    assert(beer_register_window_message_a("Beer.Event", &error) == first);
    assert(beer_register_window_message_w(lower_w, &error) == 0xC001u);

    assert(beer_register_window_message_a(NULL, &error) == 0);
    assert(error == 87u);
    assert(beer_register_window_message_a("", &error) == 0);
    assert(error == 87u);

    char oversized[257];
    for (size_t i = 0; i < sizeof(oversized) - 1; ++i) oversized[i] = 'x';
    oversized[sizeof(oversized) - 1] = 0;
    assert(beer_register_window_message_a(oversized, &error) == 0);
    assert(error == 87u);

    beer_registered_message_reset_for_test();
    puts("Win32 registered-message contract tests passed");
    return 0;
}
