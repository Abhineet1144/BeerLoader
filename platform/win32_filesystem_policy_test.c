#include "win32_filesystem_policy.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    static const uint8_t saved_games[16] =
        {0xff,0x32,0x5c,0x4c,0x9d,0xbb,0xb0,0x43,0xb5,0xb4,0x2d,0x72,0xe5,0x4e,0xaa,0xa4};
    static const uint8_t local_appdata[16] =
        {0x85,0x27,0xb3,0xf1,0x72,0x6f,0x4a,0x40,0x8f,0x95,0x4e,0x6c,0x3b,0xfa,0x12,0x4a};
    uint8_t unknown[16] = {0};
    int flags = -1;

    assert(beer_classify_known_folder(saved_games) == BEER_KNOWN_FOLDER_SAVED_GAMES);
    assert(beer_classify_known_folder(local_appdata) == BEER_KNOWN_FOLDER_LOCAL_APPDATA);
    assert(beer_classify_known_folder(unknown) == BEER_KNOWN_FOLDER_UNKNOWN);
    assert(beer_classify_known_folder(NULL) == BEER_KNOWN_FOLDER_UNKNOWN);

    assert(beer_creation_disposition_flags(1, &flags) && flags == 3);
    assert(beer_creation_disposition_flags(2, &flags) && flags == 5);
    assert(beer_creation_disposition_flags(3, &flags) && flags == 0);
    assert(beer_creation_disposition_flags(4, &flags) && flags == 1);
    assert(beer_creation_disposition_flags(5, &flags) && flags == 4);
    assert(!beer_creation_disposition_flags(0, &flags));

    puts("Win32 filesystem policy tests passed");
    return 0;
}
