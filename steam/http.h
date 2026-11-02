/* Persistent HTTP/1.1 client connection (plain or TLS) for content-server downloads. */
#ifndef BEER_STEAM_HTTP_H
#define BEER_STEAM_HTTP_H

#include <stddef.h>
#include <stdint.h>

#include "net.h"

typedef struct {
    char host[256];
    int https;
    int connected;
    int fd;                      /* plain connections */
    Tls tls;                     /* TLS connections */
    uint8_t *buf;                /* received bytes not yet consumed */
    size_t len, cap;
} HttpConn;

void http_init(HttpConn *c, const char *host, int https);
/* GET path; reconnects once if a kept-alive connection was dropped. *body is malloc'd.
 * Returns the HTTP status, or 0 on a transport failure (err says why). */
int http_get(HttpConn *c, const char *path, uint8_t **body, size_t *len, char *err, size_t errlen);
void http_close(HttpConn *c);

#endif
