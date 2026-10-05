#include "media_foundation.h"

#include <stdint.h>
#include <stdio.h>

int main(void)
{
    void *output = (void *)(uintptr_t)1;
    uint32_t token = 99;

    if (beer_mf_startup(0x20070, 0) != 0) return 1;
    if (beer_mf_create_dxgi_device_manager(&token, &output) !=
            UINT64_C(0x80004001) || output != NULL || token != 0)
        return 2;
    output = (void *)(uintptr_t)1;
    if (beer_mf_create_attributes(&output, 8) != UINT64_C(0x80004001) ||
        output != NULL)
        return 3;
    output = (void *)(uintptr_t)1;
    if (beer_mf_create_media_type(&output) != UINT64_C(0x80004001) ||
        output != NULL)
        return 4;
    output = (void *)(uintptr_t)1;
    if (beer_mf_create_byte_stream_on_stream((void *)(uintptr_t)1, &output) !=
            UINT64_C(0x80004001) || output != NULL)
        return 5;
    output = (void *)(uintptr_t)1;
    if (beer_mf_create_source_reader_from_byte_stream(
            (void *)(uintptr_t)1, NULL, &output) != UINT64_C(0x80004001) ||
        output != NULL)
        return 6;
    puts("Media Foundation compatibility tests passed");
    return 0;
}
