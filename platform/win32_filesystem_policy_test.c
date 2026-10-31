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

    assert(beer_module_is_preloaded_system_dll("kernel32.dll"));
    assert(beer_module_is_preloaded_system_dll("kernelbase.dll"));
    assert(beer_module_is_preloaded_system_dll("user32.dll"));
    assert(beer_module_is_preloaded_system_dll("ntdll.dll"));
    assert(beer_module_is_preloaded_system_dll("advapi32.dll"));
    assert(beer_module_is_preloaded_system_dll("win32u.dll"));
    assert(beer_module_is_preloaded_system_dll("wintrust.dll"));
    assert(!beer_module_is_preloaded_system_dll("steam_api64.dll"));
    assert(!beer_module_is_preloaded_system_dll("game.dll"));
    assert(!beer_module_is_preloaded_system_dll(NULL));
    assert(beer_module_prefers_compat_fallback("steam_api64.dll"));
    assert(beer_module_prefers_compat_fallback("steamclient64.dll"));
    assert(!beer_module_prefers_compat_fallback("game.dll"));
    assert(!beer_module_prefers_compat_fallback(NULL));
    assert(!beer_module_may_execute_guest_image("steamclient64.dll"));
    assert(beer_module_may_execute_guest_image("steam_api64.dll"));
    assert(beer_module_may_execute_guest_image("game.dll"));
    assert(!beer_module_may_execute_guest_image(NULL));
    assert(beer_module_has_compat_file_identity("steamclient64.dll"));
    assert(!beer_module_has_compat_file_identity("steam_api64.dll"));
    assert(!beer_module_has_compat_file_identity("game.dll"));
    assert(!beer_module_has_compat_file_identity(NULL));
    assert(beer_module_path_is_explicit("C:\\Games\\steam_api64.dll"));
    assert(beer_module_path_is_explicit("Z:/Games/steam_api64.dll"));
    assert(beer_module_path_is_explicit(".\\steam_api64.dll"));
    assert(!beer_module_path_is_explicit("steam_api64.dll"));
    assert(!beer_module_path_is_explicit(NULL));
    assert(beer_module_path_is_system_directory("C:\\Windows\\System32\\winmm.dll"));
    assert(beer_module_path_is_system_directory("c:/windows/system32/winmm.dll"));
    assert(beer_module_path_is_system_directory("C:\\WINDOWS\\SysWOW64\\winmm.dll"));
    assert(!beer_module_path_is_system_directory("Z:\\Games\\winmm.dll"));
    assert(!beer_module_path_is_system_directory("winmm.dll"));
    assert(!beer_module_path_is_system_directory(NULL));
    assert(beer_module_may_use_compat_fallback("steam_api64.dll", "steam_api64.dll"));
    assert(beer_module_may_use_compat_fallback("steamclient64.dll", "steamclient64.dll"));
    assert(!beer_module_may_use_compat_fallback("C:\\Games\\steam_api64.dll", "steam_api64.dll"));
    assert(!beer_module_may_use_compat_fallback("Z:/Games/steamclient64.dll", "steamclient64.dll"));
    assert(!beer_module_may_use_compat_fallback("game.dll", "game.dll"));
    /* An explicit path that cannot be mapped must report failure; callers may
     * not substitute a same-basename compatibility module afterward. */
    assert(!beer_module_may_use_compat_fallback("C:\\missing\\steam_api64.dll", "steam_api64.dll"));
    assert(!beer_module_may_load_application_local("kernel32.dll"));
    assert(!beer_module_may_load_application_local("api-ms-win-core-file-l1-1-0.dll"));
    assert(beer_module_may_load_application_local("winmm.dll"));
    assert(beer_module_may_load_application_local("d3d11.dll"));
    assert(beer_module_may_load_application_local("game.dll"));
    assert(!beer_module_may_load_application_local(NULL));
    assert(beer_module_is_windows_runtime_dll("kernel32.dll"));
    assert(beer_module_is_windows_runtime_dll("winmm.dll"));
    assert(beer_module_is_windows_runtime_dll("api-ms-win-core-file-l1-1-0.dll"));
    assert(!beer_module_is_windows_runtime_dll("steam_api64.dll"));
    assert(!beer_module_is_windows_runtime_dll("game.dll"));
    assert(!beer_module_is_windows_runtime_dll(NULL));

    assert(beer_classify_known_folder(saved_games) == BEER_KNOWN_FOLDER_SAVED_GAMES);
    assert(beer_classify_known_folder(local_appdata) == BEER_KNOWN_FOLDER_LOCAL_APPDATA);
    assert(beer_classify_known_folder(unknown) == BEER_KNOWN_FOLDER_UNKNOWN);
    assert(beer_classify_known_folder(NULL) == BEER_KNOWN_FOLDER_UNKNOWN);

    assert(beer_file_open_may_use_native_hook(0x80000000u, 3));
    assert(beer_file_open_may_use_native_hook(0x80100080u, 3));
    assert(beer_file_open_may_use_native_hook(0, 3));
    assert(!beer_file_open_may_use_native_hook(0x40000000u, 3));
    assert(!beer_file_open_may_use_native_hook(0x10000000u, 3));
    assert(!beer_file_open_may_use_native_hook(0x80000000u, 2));

    assert(beer_creation_disposition_flags(1, &flags) && flags == 3);
    assert(beer_creation_disposition_flags(2, &flags) && flags == 5);
    assert(beer_creation_disposition_flags(3, &flags) && flags == 0);
    assert(beer_creation_disposition_flags(4, &flags) && flags == 1);
    assert(beer_creation_disposition_flags(5, &flags) && flags == 4);
    assert(!beer_creation_disposition_flags(0, &flags));

    puts("Win32 filesystem policy tests passed");
    return 0;
}
