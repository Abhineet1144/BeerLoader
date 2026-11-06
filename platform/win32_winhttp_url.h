#ifndef BEER_WIN32_WINHTTP_URL_H
#define BEER_WIN32_WINHTTP_URL_H
#include <stdint.h>

/* URL_COMPONENTS (x64, 104 bytes) as WinHttpCrackUrl reads and fills it. */
typedef struct {
    uint32_t struct_size;
    uint32_t pad0;
    uint64_t scheme;          uint32_t scheme_length;     int32_t n_scheme;
    uint64_t host_name;       uint32_t host_name_length;  uint16_t port; uint16_t pad1;
    uint64_t user_name;       uint32_t user_name_length;  uint32_t pad2;
    uint64_t password;        uint32_t password_length;   uint32_t pad3;
    uint64_t url_path;        uint32_t url_path_length;   uint32_t pad4;
    uint64_t extra_info;      uint32_t extra_info_length; uint32_t pad5;
} BeerUrlComponents;

#define BEER_ICU_DECODE 0x10000000u
#define BEER_ICU_ESCAPE 0x80000000u
#define BEER_INTERNET_SCHEME_HTTP  1
#define BEER_INTERNET_SCHEME_HTTPS 2

/* WinHttpCrackUrl: splits url (length UTF-16 units, or NUL-terminated when 0) into components.
 * Each component is skipped when its length is 0, returned as a pointer into url when its buffer
 * is NULL, and otherwise copied NUL-terminated into the caller's buffer. Returns 0 on success or
 * the Win32 error code for GetLastError. */
uint32_t beer_winhttp_crack_url(const uint16_t *url, uint32_t length, uint32_t flags,
                                BeerUrlComponents *components);

#endif
