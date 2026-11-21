#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#include "steam_stun_resolver.h"
#include <assert.h>
#include <netinet/in.h>

static void expect(const char *input, const char *expected)
{
    char *out = NULL, error[256];
    assert(beer_steam_stun_resolve(input, &out, error, sizeof(error)));
    assert(out && !strcmp(out, expected) && !error[0]);
    free(out);
}

static void reject(const char *input)
{
    char *out = (char *)(uintptr_t)1, error[256];
    assert(!beer_steam_stun_resolve(input, &out, error, sizeof(error)));
    assert(out == NULL && error[0]);
}

/* Independently parse every result with numeric-only getaddrinfo. Never sends
 * packets and does not depend on a working DNS server or external STUN service. */
static void localhost_test(const char *input, int prefix, unsigned expected_port)
{
    char *out = NULL, error[256];
    assert(beer_steam_stun_resolve(input, &out, error, sizeof(error)));
    assert(out && *out && !strstr(out, "localhost"));
    size_t count = 0;
    char *save = NULL;
    for (char *p = strtok_r(out, ",", &save); p; p = strtok_r(NULL, ",", &save)) {
        ++count;
        assert((strncmp(p, "stun:", 5) == 0) == prefix);
        if (prefix) p += 5;
        char *host = p, *port;
        if (*host == '[') {
            ++host;
            char *end = strchr(host, ']');
            assert(end && end[1] == ':');
            *end = '\0';
            port = end + 2;
        } else {
            port = strrchr(host, ':');
            assert(port);
            *port++ = '\0';
        }
        assert(strtoul(port, NULL, 10) == expected_port);
        struct addrinfo hints, *addresses = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
        assert(!getaddrinfo(host, port, &hints, &addresses));
        for (struct addrinfo *a = addresses; a; a = a->ai_next) {
            if (a->ai_family == AF_INET) {
                const struct sockaddr_in *v4 = (const struct sockaddr_in *)a->ai_addr;
                assert((ntohl(v4->sin_addr.s_addr) >> 24) == 127);
            } else {
                assert(a->ai_family == AF_INET6);
                const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)a->ai_addr;
                assert(IN6_IS_ADDR_LOOPBACK(&v6->sin6_addr));
            }
        }
        freeaddrinfo(addresses);
    }
    assert(count);
    free(out);
}

int main(void)
{
    expect("", "");
    expect(" \t\n", "");
    expect("127.0.0.1:19302", "127.0.0.1:19302");
    expect("192.0.2.17:54321,198.51.100.22:65535", "192.0.2.17:54321,198.51.100.22:65535");
    expect(" stun:127.0.0.1:1 , STUN:[::1]:65535 ", "stun:127.0.0.1:1,stun:[::1]:65535");
    expect("127.0.0.1", "127.0.0.1:3478");
    expect("stun:127.0.0.1", "stun:127.0.0.1:3478");
    expect("[::1]:19302", "[::1]:19302");
    expect("[2001:db8::1234]:12345", "[2001:db8::1234]:12345");
    expect("::1", "[::1]:3478");
    expect("stun:[::1]", "stun:[::1]:3478");
    expect("stun:2001:db8::1", "stun:[2001:db8::1]:3478");
    expect("[::ffff:192.0.2.9]:3478", "[::ffff:192.0.2.9]:3478");
    localhost_test("localhost:19302", 0, 19302);
    localhost_test("STUN:localhost", 1, 3478);
    localhost_test("localhost:12345,localhost:12345", 0, 12345);
    const char *bad[] = {
        NULL, ",", "127.0.0.1:3478,", ",127.0.0.1:3478", "127.0.0.1,,::1",
        "stun:", "stun::3478", "127.0.0.1:", "127.0.0.1:0", "127.0.0.1:65536",
        "127.0.0.1:-1", "127.0.0.1:+1", "127.0.0.1:99999999999999999999",
        "localhost:http", "bad host:3478", "stun://localhost:3478", "[::1",
        "[]:3478", "[::1]garbage", "[::1]:", "[127.0.0.1]:3478",
        "[gggg::1]:3478", "[::1]:3478?transport=udp", "999.999.999.999:3478",
        "127.0.0.1:3478,[gggg::1]:3478", /* no partial success */
        "localhost:3478,bad host:3478", "[::1]:3478, \t", "-invalid:3478"
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) reject(bad[i]);
    char long_label[80];
    memset(long_label, 'a', 64);
    strcpy(long_label + 64, ":3478");
    reject(long_label); /* invalid DNS label rejected without querying DNS */
    char error[8], *out = NULL;
    assert(!beer_steam_stun_resolve("", NULL, error, sizeof(error)));
    assert(error[sizeof(error) - 1] == '\0');
    assert(!beer_steam_stun_resolve("[invalid::]:3478", &out, NULL, 0) && !out);
    assert(beer_steam_stun_resolve("::1", &out, NULL, 0));
    free(out);
    puts("STUN resolver tests passed (offline localhost, numeric IPv4/IPv6, malformed, ownership)");
    return 0;
}