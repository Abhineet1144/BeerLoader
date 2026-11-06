#include "win32_winhttp_url.h"

#include <stddef.h>
#include <string.h>

#define ERROR_INVALID_PARAMETER 87u
#define ERROR_INSUFFICIENT_BUFFER 122u
#define ERROR_WINHTTP_INVALID_URL 12005u
#define ERROR_WINHTTP_UNRECOGNIZED_SCHEME 12006u

static int hex_value(uint16_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int needs_escape(uint16_t c)
{
    return c <= 0x20 || c >= 0x7f;
}

/* Length of the component after the transformations the flags request on a copy. */
static uint32_t transformed(const uint16_t *text, uint32_t length, uint32_t flags, int escapable,
                            uint16_t *out)
{
    static const char digits[] = "0123456789ABCDEF";
    uint32_t n = 0;
    for (uint32_t i = 0; i < length; ++i) {
        uint16_t c = text[i];
        if ((flags & BEER_ICU_DECODE) && c == '%' && i + 2 < length &&
            hex_value(text[i + 1]) >= 0 && hex_value(text[i + 2]) >= 0) {
            if (out) out[n] = (uint16_t)(hex_value(text[i + 1]) * 16 + hex_value(text[i + 2]));
            ++n;
            i += 2;
        } else if ((flags & BEER_ICU_ESCAPE) && escapable && needs_escape(c) && c <= 0xff) {
            if (out) {
                out[n] = '%';
                out[n + 1] = (uint16_t)digits[c >> 4];
                out[n + 2] = (uint16_t)digits[c & 15];
            }
            n += 3;
        } else {
            if (out) out[n] = c;
            ++n;
        }
    }
    return n;
}

static uint32_t store(const uint16_t *text, uint32_t length, uint32_t flags, int escapable,
                      uint64_t *buffer, uint32_t *buffer_length)
{
    if (!*buffer_length) return 0;                              /* component not requested */
    if (!*buffer) {                                             /* pointer into the caller's URL */
        *buffer = length ? (uint64_t)(uintptr_t)text : 0;
        *buffer_length = length;
        return 0;
    }
    uint32_t needed = transformed(text, length, flags, escapable, NULL);
    if (*buffer_length < needed + 1) {
        *buffer_length = needed + 1;
        return ERROR_INSUFFICIENT_BUFFER;
    }
    uint16_t *out = (uint16_t *)(uintptr_t)*buffer;
    transformed(text, length, flags, escapable, out);
    out[needed] = 0;
    *buffer_length = needed;
    return 0;
}

static int matches(const uint16_t *text, uint32_t length, const char *ascii)
{
    size_t n = strlen(ascii);
    if (length != n) return 0;
    for (size_t i = 0; i < n; ++i) {
        uint16_t c = text[i];
        if (c >= 'A' && c <= 'Z') c = (uint16_t)(c - 'A' + 'a');
        if (c != (uint8_t)ascii[i]) return 0;
    }
    return 1;
}

uint32_t beer_winhttp_crack_url(const uint16_t *url, uint32_t length, uint32_t flags,
                                BeerUrlComponents *components)
{
    if (!url || !components || components->struct_size != sizeof(BeerUrlComponents))
        return ERROR_INVALID_PARAMETER;
    if (!length)
        while (url[length]) ++length;

    /* scheme "://" */
    uint32_t colon = 0;
    while (colon < length && url[colon] != ':') ++colon;
    if (!colon || colon + 2 >= length || url[colon + 1] != '/' || url[colon + 2] != '/')
        return ERROR_WINHTTP_UNRECOGNIZED_SCHEME;
    int scheme;
    uint16_t default_port;
    if (matches(url, colon, "http")) { scheme = BEER_INTERNET_SCHEME_HTTP; default_port = 80; }
    else if (matches(url, colon, "https")) { scheme = BEER_INTERNET_SCHEME_HTTPS; default_port = 443; }
    else return ERROR_WINHTTP_UNRECOGNIZED_SCHEME;

    /* authority: [user[:password]@]host[:port] up to the path, query or fragment */
    uint32_t start = colon + 3, end = start;
    while (end < length && url[end] != '/' && url[end] != '?' && url[end] != '#') ++end;
    uint32_t at = end;
    for (uint32_t i = start; i < end; ++i)
        if (url[i] == '@') at = i;
    uint32_t user = start, user_length = 0, password = start, password_length = 0, host = start;
    if (at != end) {
        uint32_t split = user;
        while (split < at && url[split] != ':') ++split;
        user_length = split - user;
        if (split < at) { password = split + 1; password_length = at - password; }
        host = at + 1;
    }
    uint32_t host_end = end, port_start = end;
    if (host < end && url[host] == '[') {                        /* IPv6 literal */
        uint32_t close = host;
        while (close < end && url[close] != ']') ++close;
        if (close == end) return ERROR_WINHTTP_INVALID_URL;
        host_end = close + 1;
        if (host_end < end) {
            if (url[host_end] != ':') return ERROR_WINHTTP_INVALID_URL;
            port_start = host_end + 1;
        }
    } else {
        for (uint32_t i = host; i < end; ++i)
            if (url[i] == ':') { host_end = i; port_start = i + 1; break; }
    }
    if (host_end == host) return ERROR_WINHTTP_INVALID_URL;
    uint32_t port = default_port;
    if (port_start < end || (port_start == end && host_end != end)) {
        if (port_start == end) return ERROR_WINHTTP_INVALID_URL;
        port = 0;
        for (uint32_t i = port_start; i < end; ++i) {
            if (url[i] < '0' || url[i] > '9') return ERROR_WINHTTP_INVALID_URL;
            port = port * 10 + (uint32_t)(url[i] - '0');
            if (port > 65535) return ERROR_WINHTTP_INVALID_URL;
        }
    }

    /* path up to the query or fragment; the rest is extra info */
    uint32_t path = end, extra = end;
    while (extra < length && url[extra] != '?' && url[extra] != '#') ++extra;

    uint32_t error;
    if ((error = store(url, colon, flags, 0, &components->scheme, &components->scheme_length)) ||
        (error = store(url + host, host_end - host, flags, 0,
                       &components->host_name, &components->host_name_length)) ||
        (error = store(url + user, user_length, flags, 0,
                       &components->user_name, &components->user_name_length)) ||
        (error = store(url + password, password_length, flags, 0,
                       &components->password, &components->password_length)) ||
        (error = store(url + path, extra - path, flags, 1,
                       &components->url_path, &components->url_path_length)) ||
        (error = store(url + extra, length - extra, flags, 1,
                       &components->extra_info, &components->extra_info_length)))
        return error;
    components->n_scheme = scheme;
    components->port = (uint16_t)port;
    return 0;
}
