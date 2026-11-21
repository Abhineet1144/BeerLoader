#ifndef BEER_STEAM_STUN_RESOLVER_H
#define BEER_STEAM_STUN_RESOLVER_H

/* Header-only so the adapter needs no new build target. Requires POSIX.1-2001
 * declarations (e.g. compile with _GNU_SOURCE or _POSIX_C_SOURCE=200112L).
 *
 * Call BEFORE GameNetworkingSockets_Init and outside any networking/GNS lock.
 * getaddrinfo may block on the system resolver; this moves that work out of ICE
 * initialization, it does not impose a DNS timeout or verify STUN reachability.
 * Success: *numeric is malloc-owned (including an explicit empty list), free it
 * after SetConfigValue copies it. Failure: *numeric is NULL, error explains why;
 * never fall back to passing the original hostname list to GNS. All-or-nothing:
 * a bad member fails the whole list, rather than silently dropping a server.
 *
 * Accepts comma-separated host[:port], [IPv6][:port], bare IPv6 (no port), and
 * optional case-insensitive stun: prefixes. Missing port defaults to 3478.
 * Surrounding whitespace is ignored; empty members in a nonempty list fail.
 * All returned endpoints have explicit ports, IPv6 brackets, and numeric hosts.
 */
#include <ctype.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>

static inline int beer_stun_append(char **list, size_t *length, const char *endpoint)
{
    const size_t n = strlen(endpoint), separator = *length != 0;
    if (*length > SIZE_MAX - separator - 1 || n > SIZE_MAX - *length - separator - 1)
        return 0;
    char *grown = realloc(*list, *length + separator + n + 1);
    if (!grown) return 0;
    *list = grown;
    if (separator) grown[(*length)++] = ',';
    memcpy(grown + *length, endpoint, n + 1);
    *length += n;
    return 1;
}

/* Validate DNS names before allowing a non-numeric getaddrinfo call. In
 * particular, malformed IPv6 and dotted numeric addresses must not trigger DNS. */
static inline int beer_stun_hostname_valid(const char *host)
{
    size_t label = 0, n = strlen(host);
    int has_nondigit = 0;
    if (!n || n > 253) return 0;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = (unsigned char)host[i];
        if (c == '.') {
            if (!label || host[i - 1] == '-') return 0;
            label = 0;
        } else {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-')) return 0;
            if ((!label && c == '-') || ++label > 63) return 0;
            if (c < '0' || c > '9') has_nondigit = 1;
        }
    }
    return has_nondigit && host[n - 1] != '-'; /* trailing DNS root dot is allowed */
}

static inline int beer_steam_stun_resolve(const char *configured, char **numeric,
                                         char *error, size_t error_size)
{
    char *copy = NULL, *list = NULL;
    size_t length = 0, member = 0;
    struct addrinfo *addresses = NULL;
    const char *failure = "out of memory";
    if (error && error_size) error[0] = '\0';
    if (!numeric) {
        if (error && error_size) snprintf(error, error_size, "STUN output pointer is NULL");
        return 0;
    }
    *numeric = NULL;
    if (!configured) { failure = "configuration is NULL"; goto fail; }
    copy = malloc(strlen(configured) + 1);
    list = malloc(1);
    if (!copy || !list) goto fail;
    strcpy(copy, configured);
    list[0] = '\0';
    char *cursor = copy;
    while (isspace((unsigned char)*cursor)) ++cursor;
    if (!*cursor) goto success;
    for (;;) {
        ++member;
        char *next = strchr(cursor, ',');
        if (next) *next++ = '\0';
        while (isspace((unsigned char)*cursor)) ++cursor;
        char *end = cursor + strlen(cursor);
        while (end > cursor && isspace((unsigned char)end[-1])) *--end = '\0';
        const int prefixed = strncasecmp(cursor, "stun:", 5) == 0;
        char *host = cursor + (prefixed ? 5 : 0), *port = NULL;
        int ipv6_literal = 0;
        failure = "malformed endpoint";
        if (!*host) goto fail;
        if (*host == '[') {
            ++host;
            char *bracket = strchr(host, ']');
            if (!bracket || bracket == host || !strchr(host, ':')) goto fail;
            if (bracket[1]) {
                if (bracket[1] != ':') goto fail;
                port = bracket + 2;
            }
            *bracket = '\0';
            ipv6_literal = 1;
        } else {
            char *colon = strchr(host, ':');
            if (colon && strchr(colon + 1, ':')) ipv6_literal = 1;
            else if (colon) { *colon = '\0'; port = colon + 1; }
        }
        if (!*host) goto fail;
        unsigned port_number = 3478;
        if (port) {
            failure = "port must be an integer in 1..65535";
            if (!*port) goto fail;
            port_number = 0;
            for (const char *p = port; *p; ++p) {
                if (*p < '0' || *p > '9') goto fail;
                port_number = port_number * 10 + (unsigned)(*p - '0');
                if (port_number > 65535) goto fail;
            }
            if (!port_number) goto fail;
        }
        char service[6];
        snprintf(service, sizeof(service), "%u", port_number);
        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = ipv6_literal ? AF_INET6 : AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;
        hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
        int rc = getaddrinfo(host, service, &hints, &addresses);
        if (rc == EAI_NONAME && !ipv6_literal && beer_stun_hostname_valid(host)) {
            hints.ai_flags = AI_NUMERICSERV;
            rc = getaddrinfo(host, service, &hints, &addresses);
        }
        if (rc) {
            if (error && error_size)
                snprintf(error, error_size, "STUN member %zu ('%s'): getaddrinfo: %s",
                         member, host, gai_strerror(rc));
            goto cleanup;
        }
        size_t before = length;
        for (struct addrinfo *a = addresses; a; a = a->ai_next) {
            if (a->ai_family != AF_INET && a->ai_family != AF_INET6) continue;
            char address[1025], endpoint[1050];
            rc = getnameinfo(a->ai_addr, a->ai_addrlen, address, sizeof(address),
                             NULL, 0, NI_NUMERICHOST);
            if (rc) {
                failure = gai_strerror(rc);
                goto fail;
            }
            snprintf(endpoint, sizeof(endpoint), a->ai_family == AF_INET6 ? "%s[%s]:%u" : "%s%s:%u",
                     prefixed ? "stun:" : "", address, port_number);
            if (!beer_stun_append(&list, &length, endpoint)) { failure = "out of memory"; goto fail; }
        }
        freeaddrinfo(addresses);
        addresses = NULL;
        if (length == before) { failure = "no IPv4 or IPv6 addresses returned"; goto fail; }
        if (!next) break;
        cursor = next;
    }
success:
    free(copy);
    *numeric = list;
    return 1;
fail:
    if (error && error_size) snprintf(error, error_size, "STUN member %zu: %s", member, failure);
cleanup:
    if (addresses) freeaddrinfo(addresses);
    free(copy);
    free(list);
    return 0;
}

#endif