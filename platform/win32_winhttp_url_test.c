#include "win32_winhttp_url.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t widen(const char *text, uint16_t *out)
{
    uint32_t n = 0;
    while (text[n]) { out[n] = (uint8_t)text[n]; ++n; }
    out[n] = 0;
    return n;
}

static int equals(const uint16_t *text, uint32_t length, const char *ascii)
{
    if (strlen(ascii) != length) return 0;
    for (uint32_t i = 0; i < length; ++i)
        if (text[i] != (uint8_t)ascii[i]) return 0;
    return 1;
}

static void request_pointers(BeerUrlComponents *c)
{
    memset(c, 0, sizeof(*c));
    c->struct_size = sizeof(*c);
    c->scheme_length = c->host_name_length = c->user_name_length = c->password_length =
        c->url_path_length = c->extra_info_length = (uint32_t)-1;
}

#define PTR(c, field) ((const uint16_t *)(uintptr_t)(c).field)

int main(void)
{
    assert(sizeof(BeerUrlComponents) == 104);
    uint16_t url[256];
    BeerUrlComponents c;

    /* Pointer mode: every component points into the URL. */
    widen("https://user:pw@api.example.com:8443/v1/dlc?id=7#top", url);
    request_pointers(&c);
    assert(beer_winhttp_crack_url(url, 0, 0, &c) == 0);
    assert(c.n_scheme == BEER_INTERNET_SCHEME_HTTPS && c.port == 8443);
    assert(equals(PTR(c, scheme), c.scheme_length, "https"));
    assert(equals(PTR(c, host_name), c.host_name_length, "api.example.com"));
    assert(equals(PTR(c, user_name), c.user_name_length, "user"));
    assert(equals(PTR(c, password), c.password_length, "pw"));
    assert(equals(PTR(c, url_path), c.url_path_length, "/v1/dlc"));
    assert(equals(PTR(c, extra_info), c.extra_info_length, "?id=7#top"));
    assert(PTR(c, host_name) == url + 16);

    /* Default ports, no path, explicit length shorter than the string. */
    widen("http://example.comXYZ", url);
    request_pointers(&c);
    assert(beer_winhttp_crack_url(url, 18, 0, &c) == 0);
    assert(c.n_scheme == BEER_INTERNET_SCHEME_HTTP && c.port == 80);
    assert(equals(PTR(c, host_name), c.host_name_length, "example.com"));
    assert(c.url_path_length == 0 && c.extra_info_length == 0 && c.user_name_length == 0);

    /* Copy mode with ICU_DECODE | ICU_ESCAPE: NUL-terminated copies; lengths exclude the NUL. */
    widen("HTTPS://Host.Example/a%41b c?q=%7e", url);
    uint16_t host[64], path[64], extra[64], scheme[8];
    memset(&c, 0, sizeof(c));
    c.struct_size = sizeof(c);
    c.scheme = (uintptr_t)scheme;   c.scheme_length = 8;
    c.host_name = (uintptr_t)host;  c.host_name_length = 64;
    c.url_path = (uintptr_t)path;   c.url_path_length = 64;
    c.extra_info = (uintptr_t)extra; c.extra_info_length = 64;
    assert(beer_winhttp_crack_url(url, 0, BEER_ICU_DECODE | BEER_ICU_ESCAPE, &c) == 0);
    assert(c.port == 443 && c.n_scheme == BEER_INTERNET_SCHEME_HTTPS);
    assert(equals(scheme, c.scheme_length, "HTTPS") && scheme[c.scheme_length] == 0);
    assert(equals(host, c.host_name_length, "Host.Example"));
    assert(equals(path, c.url_path_length, "/aAb%20c"));
    assert(equals(extra, c.extra_info_length, "?q=~"));
    assert(c.user_name_length == 0 && c.user_name == 0);       /* not requested: untouched */

    /* A buffer that is too small reports the size needed, including the NUL. */
    widen("http://example.com/path", url);
    memset(&c, 0, sizeof(c));
    c.struct_size = sizeof(c);
    c.host_name = (uintptr_t)host; c.host_name_length = 5;
    assert(beer_winhttp_crack_url(url, 0, 0, &c) == 122);
    assert(c.host_name_length == 12);

    /* IPv6 literal with a port. */
    widen("http://[::1]:8080/x", url);
    request_pointers(&c);
    assert(beer_winhttp_crack_url(url, 0, 0, &c) == 0);
    assert(c.port == 8080 && equals(PTR(c, host_name), c.host_name_length, "[::1]"));
    assert(equals(PTR(c, url_path), c.url_path_length, "/x"));

    /* Errors. */
    request_pointers(&c);
    widen("ftp://example.com/", url);
    assert(beer_winhttp_crack_url(url, 0, 0, &c) == 12006);
    widen("example.com/path", url);
    assert(beer_winhttp_crack_url(url, 0, 0, &c) == 12006);
    widen("http:///path", url);
    assert(beer_winhttp_crack_url(url, 0, 0, &c) == 12005);
    widen("http://host:99999/", url);
    assert(beer_winhttp_crack_url(url, 0, 0, &c) == 12005);
    widen("http://host:8x/", url);
    assert(beer_winhttp_crack_url(url, 0, 0, &c) == 12005);
    c.struct_size = 100;
    widen("http://host/", url);
    assert(beer_winhttp_crack_url(url, 0, 0, &c) == 87);

    puts("winhttp url tests passed");
    return 0;
}
