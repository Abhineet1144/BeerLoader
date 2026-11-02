/* TLS client (OpenSSL, peer and hostname verified), HTTPS GET and WebSocket framing. */
#ifndef BEER_STEAM_NET_H
#define BEER_STEAM_NET_H

#include <stddef.h>
#include <stdint.h>

typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;

typedef struct {
    int fd;
    SSL_CTX *ctx;
    SSL *ssl;
} Tls;

/* Blocking TCP connect with send/receive timeouts; returns the socket or -1. */
int net_connect_tcp(const char *host, const char *port, int timeout_ms, char *err, size_t errlen);
/* Blocking connect and handshake. On failure returns 0 and describes why in err. */
int tls_connect(Tls *t, const char *host, const char *port, int timeout_ms, char *err, size_t errlen);
int tls_write_all(Tls *t, const void *data, size_t len);
/* Returns bytes read, 0 when the peer closed, -1 on error, -2 when non-blocking and nothing is ready. */
long tls_read(Tls *t, void *buf, size_t len);
void tls_close(Tls *t);

/* HTTPS GET; *body is malloc'd and NUL-terminated. Returns the HTTP status, or 0 on failure. */
int https_get(const char *host, const char *path, char **body, size_t *len, char *err, size_t errlen);

typedef struct {
    Tls tls;
    uint8_t *rx;          /* bytes received but not yet parsed into frames */
    size_t rx_len, rx_cap;
    uint8_t *msg;         /* message being reassembled from fragments */
    size_t msg_len, msg_cap;
    int msg_opcode;
    int msg_done;         /* msg was returned; start a new message on the next call */
    int closed;
} Ws;

enum { WS_TEXT = 1, WS_BINARY = 2, WS_CLOSE = 8, WS_PING = 9, WS_PONG = 10 };

/* endpoint is "host" or "host:port" (default 443). The socket is non-blocking afterwards. */
int ws_connect(Ws *w, const char *endpoint, const char *path, char *err, size_t errlen);
int ws_send(Ws *w, int opcode, const void *data, size_t len);
/* Returns 1 when a complete data message is in data and len (valid until the next call),
 * 0 when more bytes are needed (poll tls.fd for input), -1 when closed or failed.
 * Ping, pong and close frames are handled internally. */
int ws_poll_message(Ws *w, const uint8_t **data, size_t *len);
void ws_close(Ws *w);

#endif
