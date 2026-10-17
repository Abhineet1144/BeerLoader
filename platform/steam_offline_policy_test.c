#include "steam_offline_policy.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    assert(!beer_steam_local_user_logged_on(0));
    assert(beer_steam_local_user_logged_on(1));
    assert(beer_steam_local_user_id() == UINT64_C(0x0110000100000001));
    assert(BEER_STEAM_REMOTE_STORAGE_FILE_WRITE_SLOT == 0u);
    assert(BEER_STEAM_REMOTE_STORAGE_FILE_READ_SLOT == 1u);
    assert(BEER_STEAM_REMOTE_STORAGE_FILE_WRITE_ASYNC_SLOT == 2u);
    assert(BEER_STEAM_REMOTE_STORAGE_FILE_READ_ASYNC_SLOT == 3u);
    assert(BEER_STEAM_REMOTE_STORAGE_FILE_READ_ASYNC_COMPLETE_SLOT == 4u);
    assert(BEER_STEAM_REMOTE_STORAGE_WRITE_CALLBACK_ID == 1331u);
    assert(BEER_STEAM_REMOTE_STORAGE_READ_CALLBACK_ID == 1332u);
    assert(BEER_STEAM_REMOTE_STORAGE_WRITE_RESULT_SIZE == sizeof(uint32_t));
    assert(BEER_STEAM_REMOTE_STORAGE_READ_RESULT_SIZE == 24u);
    assert(BEER_STEAM_REMOTE_STORAGE_READ_RESULT_OFFSET_OFFSET == 12u);
    assert(BEER_STEAM_REMOTE_STORAGE_READ_RESULT_BYTES_OFFSET == 16u);

    char path[128];
    assert(beer_steam_storage_normalize_path("./win64_save/data00-1.bin",
                                             path, sizeof(path)));
    assert(!strcmp(path, "win64_save/data00-1.bin"));
    assert(beer_steam_storage_normalize_path("win64_save\\data00-1.bin",
                                             path, sizeof(path)));
    assert(!strcmp(path, "win64_save/data00-1.bin"));
    assert(!beer_steam_storage_normalize_path("../data.bin", path, sizeof(path)));
    assert(!beer_steam_storage_normalize_path("save/../../data.bin", path, sizeof(path)));
    assert(!beer_steam_storage_normalize_path("/absolute/data.bin", path, sizeof(path)));
    assert(!beer_steam_storage_normalize_path("C:\\data.bin", path, sizeof(path)));
    assert(!beer_steam_storage_normalize_path("./", path, sizeof(path)));
    puts("Steam offline policy tests passed");
    return 0;
}
