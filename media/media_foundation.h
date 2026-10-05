#ifndef BEER_MEDIA_FOUNDATION_H
#define BEER_MEDIA_FOUNDATION_H

#include <stdint.h>

uint64_t __attribute__((ms_abi)) beer_mf_startup(uint32_t version, uint32_t flags);
uint64_t __attribute__((ms_abi)) beer_mf_shutdown(void);
uint64_t __attribute__((ms_abi)) beer_mf_create_dxgi_device_manager(
    uint32_t *reset_token, void **manager);
uint64_t __attribute__((ms_abi)) beer_mf_create_attributes(
    void **attributes, uint32_t initial_size);
uint64_t __attribute__((ms_abi)) beer_mf_create_media_type(void **media_type);
uint64_t __attribute__((ms_abi)) beer_mf_create_byte_stream_on_stream(
    void *stream, void **byte_stream);
uint64_t __attribute__((ms_abi)) beer_mf_create_source_reader_from_byte_stream(
    void *byte_stream, void *attributes, void **reader);

#endif
