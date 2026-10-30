#include "steam_offline_policy.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    assert(beer_steam_parse_app_id_text("3357650\n") == 3357650u);
    assert(beer_steam_parse_app_id_text("  2050650 \r\n") == 2050650u);
    assert(beer_steam_parse_app_id_text("0") == 0u);
    assert(beer_steam_parse_app_id_text("3357650x") == 0u);
    assert(beer_steam_parse_app_id_text("99999999999") == 0u);
    assert(beer_steam_parse_app_id_text(NULL) == 0u);
    assert(!beer_steam_local_user_logged_on(0));
    assert(beer_steam_local_user_logged_on(1));
    assert(beer_steam_local_user_id() == UINT64_C(0x0110000100000001));

    char registry_root[256];
    assert(beer_steam_registry_root(0, NULL, registry_root, sizeof(registry_root)));
    assert(!strcmp(registry_root, "C:\\Program Files (x86)\\Steam"));
    assert(beer_steam_registry_root(
        1, "Z:\\mnt\\Games\\PRAGMATA\\PRAGMATA.exe",
        registry_root, sizeof(registry_root)));
    assert(!strcmp(registry_root, "Z:\\mnt\\Games\\PRAGMATA"));
    assert(!beer_steam_registry_root(1, "PRAGMATA.exe",
                                     registry_root, sizeof(registry_root)));
    assert(!beer_steam_registry_root(1, "Z:\\PRAGMATA.exe", registry_root, 2));

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
    assert(beer_steam_call_result_finishes_request(
        BEER_STEAM_REMOTE_STORAGE_WRITE_CALLBACK_ID));
    assert(!beer_steam_call_result_finishes_request(
        BEER_STEAM_REMOTE_STORAGE_READ_CALLBACK_ID));

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
