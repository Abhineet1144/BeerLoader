#include "win32_ui_language.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    uint16_t buffer[16];
    uint32_t error = 0x1234, count = 0, size = 0;
    assert(beer_user_preferred_ui_languages(8, &count, NULL, &size, &error));
    assert(count == 1 && size == 7 && error == 0x1234);
    memset(buffer, 0x55, sizeof(buffer));
    size = 6;
    assert(!beer_user_preferred_ui_languages(8, &count, buffer, &size, &error));
    assert(error == 122 && size == 7 && buffer[0] == 0x5555);
    assert(beer_user_preferred_ui_languages(8, &count, buffer, &size, &error));
    assert(count == 1 && size == 7 && buffer[5] == 0 && buffer[6] == 0);
    assert(buffer[7] == 0x5555);
    assert(!memcmp(buffer, beer_ui_locale_name, sizeof(beer_ui_locale_name)));
    size = 16;
    assert(beer_user_preferred_ui_languages(0, &count, buffer, &size, &error));
    assert(size == 7);
    size = 0;
    assert(beer_user_preferred_ui_languages(4, &count, NULL, &size, &error));
    assert(size == 6);
    assert(beer_user_preferred_ui_languages(4, &count, buffer, &size, &error));
    static const uint16_t expected[] = {'0', '4', '0', '9', 0, 0};
    assert(!memcmp(buffer, expected, sizeof(expected)));
    size = 0;
    assert(!beer_user_preferred_ui_languages(8, &count, buffer, &size, &error));
    assert(error == 122 && size == 7);
    assert(!beer_user_preferred_ui_languages(8, &count, NULL, &size, &error));
    assert(error == 122);
    assert(!beer_user_preferred_ui_languages(12, &count, buffer, &size, &error));
    assert(error == 87);
    assert(!beer_user_preferred_ui_languages(0x100, &count, buffer, &size, &error));
    assert(!beer_user_preferred_ui_languages(8, NULL, buffer, &size, &error));
    assert(!beer_user_preferred_ui_languages(8, &count, buffer, NULL, &error));
    memset(buffer, 0x55, sizeof(buffer));
    assert(!beer_user_default_locale_name(buffer, 5, &error));
    assert(error == 122 && buffer[0] == 0x5555);
    assert(!beer_user_default_locale_name(NULL, 6, &error));
    assert(error == 87);
    assert(!beer_user_default_locale_name(buffer, 0, &error));
    assert(beer_user_default_locale_name(buffer, 6, &error) == 6);
    assert(!memcmp(buffer, beer_ui_locale_name, sizeof(beer_ui_locale_name)));
    assert(buffer[6] == 0x5555);
    puts("Win32 UI-language contract tests passed");
    return 0;
}