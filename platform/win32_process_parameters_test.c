#include "win32_process_parameters.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    BeerProcessParameters params;
    memset(&params, 0xaa, sizeof(params));
    beer_process_parameters_initialize(&params, (uint64_t)-10,
                                       (uint64_t)-11, (uint64_t)-12);
    const unsigned char *bytes = (const unsigned char *)&params;
    uint32_t word;
    memcpy(&word, bytes, 4);
    assert(word == sizeof(params));
    memcpy(&word, bytes + 4, 4);
    assert(word == sizeof(params));
    memcpy(&word, bytes + 8, 4);
    assert(word == 1);
    uint64_t handle;
    memcpy(&handle, bytes + 0x20, 8);
    assert(handle == (uint64_t)-10);
    memcpy(&handle, bytes + 0x28, 8);
    assert(handle == (uint64_t)-11);
    memcpy(&handle, bytes + 0x30, 8);
    assert(handle == (uint64_t)-12);
    assert(!params.Environment && !params.CommandLine.Buffer);

    char windows_path[128];
    assert(beer_windows_path_from_host("/home/player/game/ucrtbase.dll",
                                       windows_path, sizeof(windows_path)));
    assert(!strcmp(windows_path, "Z:\\home\\player\\game\\ucrtbase.dll"));
    assert(beer_windows_path_from_host("C:/Windows/System32/ntdll.dll",
                                       windows_path, sizeof(windows_path)));
    assert(!strcmp(windows_path, "C:\\Windows\\System32\\ntdll.dll"));
    assert(!beer_windows_path_from_host("/path/too/long", windows_path, 4));

    uint16_t command[] = {'b', 'e', 'e', 'r', 0};
    params.CommandLine.Length = 8;
    params.CommandLine.MaximumLength = 10;
    params.CommandLine.Buffer = (uintptr_t)command;
    uint64_t pointer;
    memcpy(&pointer, bytes + 0x78, 8);
    assert(pointer == (uintptr_t)command);
    assert(*(const uint16_t *)(uintptr_t)pointer == 'b');
    for (size_t i = 0; i < sizeof(params.Reserved); ++i) assert(!params.Reserved[i]);
    puts("Win64 process-parameter layout and normalization tests passed");
    return 0;
}