#include "net.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

static void set_err(char *err, size_t errlen, const char *what, const char *detail)
{
    if (err && errlen) snprintf(err, errlen, "%s%s%s", what, detail ? ": " : "", detail ? detail : "");
}

int net_connect_tcp(const char *host, const char *port, int timeout_ms, char *err, size_t errlen)
{
    struct addrinfo hints = {0}, *list = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int status = getaddrinfo(host, port, &hints, &list);
    if (status) { set_err(err, errlen, "resolve failed", gai_strerror(status)); return -1; }
    int fd = -1;
    for (struct addrinfo *a = list; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, a->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        if (errno == EINPROGRESS) {
            struct pollfd p = { fd, POLLOUT, 0 };
            int soerr = 0;
            socklen_t sl = sizeof(soerr);
            if (poll(&p, 1, timeout_ms) == 1 && !getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) && !soerr)
                break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(list);
    if (fd < 0) { set_err(err, errlen, "connect failed", host); return -1; }
    /* The handshake and HTTP exchanges run blocking; ws_connect switches back afterwards. */
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return fd;
}

int tls_connect(Tls *t, const char *host, const char *port, int timeout_ms, char *err, size_t errlen)
{
    memset(t, 0, sizeof(*t));
    t->fd = net_connect_tcp(host, port, timeout_ms, err, errlen);
    if (t->fd < 0) return 0;
    t->ctx = SSL_CTX_new(TLS_client_method());
    if (!t->ctx) goto fail;
    SSL_CTX_set_min_proto_version(t->ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(t->ctx, SSL_VERIFY_PEER, NULL);
    if (!SSL_CTX_set_default_verify_paths(t->ctx)) goto fail;
    SSL_CTX_set_mode(t->ctx, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    t->ssl = SSL_new(t->ctx);
    if (!t->ssl || !SSL_set_tlsext_host_name(t->ssl, host) || !SSL_set1_host(t->ssl, host) ||
        !SSL_set_fd(t->ssl, t->fd))
        goto fail;
    if (SSL_connect(t->ssl) != 1) goto fail;
    return 1;
fail:;
    char detail[256];
    unsigned long code = ERR_get_error();
    if (code) ERR_error_string_n(code, detail, sizeof(detail));
    else snprintf(detail, sizeof(detail), "%s", host);
    set_err(err, errlen, "TLS handshake failed", detail);
    tls_close(t);
    return 0;
}

int tls_write_all(Tls *t, const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len) {
        int n = SSL_write(t->ssl, p, len > 1 << 30 ? 1 << 30 : (int)len);
        if (n > 0) { p += n; len -= (size_t)n; continue; }
        int e = SSL_get_error(t->ssl, n);
        if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) return 0;
        struct pollfd pfd = { t->fd, e == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, 0 };
        if (poll(&pfd, 1, 15000) != 1) return 0;
    }
    return 1;
}

long tls_read(Tls *t, void *buf, size_t len)
{
    int n = SSL_read(t->ssl, buf, len > 1 << 30 ? 1 << 30 : (int)len);
    if (n > 0) return n;
    int e = SSL_get_error(t->ssl, n);
    if (e == SSL_ERROR_ZERO_RETURN) return 0;
    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) return -2;
    return -1;
}

void tls_close(Tls *t)
{
    if (t->ssl) { SSL_shutdown(t->ssl); SSL_free(t->ssl); }
    if (t->ctx) SSL_CTX_free(t->ctx);
    if (t->fd > 0) close(t->fd);
    memset(t, 0, sizeof(*t));
    t->fd = -1;
}

/* Reads the whole HTTP response (the request asks the server to close). */
static char *read_all(Tls *t, size_t *len)
{
    size_t cap = 16384, n = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    for (;;) {
        if (cap - n < 4096) {
            if (cap > (64u << 20)) { free(out); return NULL; }
            char *grown = realloc(out, cap *= 2);
            if (!grown) { free(out); return NULL; }
            out = grown;
        }
        long got = tls_read(t, out + n, cap - n - 1);
        if (got <= 0) break;
        n += (size_t)got;
    }
    out[n] = 0;
    *len = n;
    return out;
}

/* Decodes a chunked body in place; returns its length, or -1 when malformed. */
static long dechunk(char *body, size_t len)
{
    char *in = body, *end = body + len, *out = body;
    for (;;) {
        char *line_end = memchr(in, '\n', (size_t)(end - in));
        if (!line_end) return -1;
        unsigned long size = strtoul(in, NULL, 16);
        in = line_end + 1;
        if (!size) break;
        if (size > (size_t)(end - in)) return -1;
        memmove(out, in, size);
        out += size;
        in += size;
        if (in < end && *in == '\r') in++;
        if (in < end && *in == '\n') in++;
    }
    *out = 0;
    return out - body;
}

int https_get(const char *host, const char *path, char **body, size_t *len, char *err, size_t errlen)
{
    *body = NULL;
    *len = 0;
    Tls t;
    if (!tls_connect(&t, host, "443", 15000, err, errlen)) return 0;
    char request[2048];
    int n = snprintf(request, sizeof(request),
                     "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Beer\r\nAccept: */*\r\nConnection: close\r\n\r\n",
                     path, host);
    size_t total = 0;
    char *response = n > 0 && (size_t)n < sizeof(request) && tls_write_all(&t, request, (size_t)n)
                   ? read_all(&t, &total) : NULL;
    tls_close(&t);
    if (!response) { set_err(err, errlen, "HTTP request failed", host); return 0; }
    int status = 0;
    char *headers_end = strstr(response, "\r\n\r\n");
    if (strncmp(response, "HTTP/1.", 7) || !headers_end || sscanf(response + 8, " %d", &status) != 1) {
        free(response);
        set_err(err, errlen, "malformed HTTP response", host);
        return 0;
    }
    *headers_end = 0;
    int chunked = 0;
    for (char *line = strstr(response, "\r\n"); line; line = strstr(line + 2, "\r\n"))
        if (!strncasecmp(line + 2, "Transfer-Encoding:", 18) && strstr(line + 20, "chunked")) chunked = 1;
    char *content = headers_end + 4;
    size_t content_len = total - (size_t)(content - response);
    if (chunked) {
        long decoded = dechunk(content, content_len);
        if (decoded < 0) { free(response); set_err(err, errlen, "malformed chunked body", host); return 0; }
        content_len = (size_t)decoded;
    }
    memmove(response, content, content_len);
    response[content_len] = 0;
    *body = response;
    *len = content_len;
    return status;
}

/* ---------------------------------------------------------------- WebSocket (RFC 6455) */

static int append(uint8_t **buf, size_t *len, size_t *cap, const void *data, size_t n)
{
    if (*len + n > *cap) {
        size_t c = *cap ? *cap : 4096;
        while (c < *len + n) c *= 2;
        uint8_t *grown = realloc(*buf, c);
        if (!grown) return 0;
        *buf = grown;
        *cap = c;
    }
    memcpy(*buf + *len, data, n);
    *len += n;
    return 1;
}

int ws_connect(Ws *w, const char *endpoint, const char *path, char *err, size_t errlen)
{
    memset(w, 0, sizeof(*w));
    char host[256], port[16] = "443";
    snprintf(host, sizeof(host), "%s", endpoint);
    char *colon = strrchr(host, ':');
    if (colon) { snprintf(port, sizeof(port), "%s", colon + 1); *colon = 0; }
    if (!tls_connect(&w->tls, host, port, 15000, err, errlen)) return 0;

    uint8_t nonce[16];
    char key[32];
    RAND_bytes(nonce, sizeof(nonce));
    EVP_EncodeBlock((unsigned char *)key, nonce, sizeof(nonce));
    char request[1024];
    int n = snprintf(request, sizeof(request),
                     "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n", path, host, key);
    if (n <= 0 || (size_t)n >= sizeof(request) || !tls_write_all(&w->tls, request, (size_t)n)) {
        set_err(err, errlen, "WebSocket request failed", endpoint);
        ws_close(w);
        return 0;
    }
    /* Read the upgrade response; any bytes after the headers already belong to frames. */
    char head[8192];
    size_t got = 0;
    char *end = NULL;
    while (!end && got < sizeof(head) - 1) {
        long r = tls_read(&w->tls, head + got, sizeof(head) - 1 - got);
        if (r <= 0) break;
        got += (size_t)r;
        head[got] = 0;
        end = strstr(head, "\r\n\r\n");
    }
    if (!end || strncmp(head, "HTTP/1.1 101", 12)) {
        set_err(err, errlen, "WebSocket upgrade refused", endpoint);
        ws_close(w);
        return 0;
    }
    size_t header_len = (size_t)(end + 4 - head);
    if (got > header_len && !append(&w->rx, &w->rx_len, &w->rx_cap, head + header_len, got - header_len)) {
        ws_close(w);
        return 0;
    }
    fcntl(w->tls.fd, F_SETFL, fcntl(w->tls.fd, F_GETFL) | O_NONBLOCK);
    return 1;
}

int ws_send(Ws *w, int opcode, const void *data, size_t len)
{
    uint8_t header[14];
    size_t n = 0;
    header[n++] = (uint8_t)(0x80 | opcode);
    if (len < 126) header[n++] = (uint8_t)(0x80 | len);
    else if (len <= 0xffff) { header[n++] = 0x80 | 126; header[n++] = (uint8_t)(len >> 8); header[n++] = (uint8_t)len; }
    else {
        header[n++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) header[n++] = (uint8_t)((uint64_t)len >> (8 * i));
    }
    uint8_t mask[4];
    RAND_bytes(mask, 4);
    memcpy(header + n, mask, 4);
    n += 4;
    uint8_t *frame = malloc(n + len);
    if (!frame) return 0;
    memcpy(frame, header, n);
    const uint8_t *src = data;
    for (size_t i = 0; i < len; i++) frame[n + i] = src[i] ^ mask[i & 3];
    int ok = tls_write_all(&w->tls, frame, n + len);
    free(frame);
    return ok;
}

/* Parses one frame from rx. Returns 1 if a frame was consumed, 0 if incomplete, -1 if invalid. */
static int take_frame(Ws *w, int *fin, int *opcode, const uint8_t **payload, size_t *len, size_t *consumed)
{
    if (w->rx_len < 2) return 0;
    const uint8_t *p = w->rx;
    *fin = p[0] >> 7;
    *opcode = p[0] & 15;
    int masked = p[1] >> 7;
    uint64_t size = p[1] & 127;
    size_t n = 2;
    if (size == 126) {
        if (w->rx_len < 4) return 0;
        size = (uint64_t)p[2] << 8 | p[3];
        n = 4;
    } else if (size == 127) {
        if (w->rx_len < 10) return 0;
        size = 0;
        for (int i = 0; i < 8; i++) size = size << 8 | p[2 + i];
        n = 10;
    }
    if (masked || size > (256u << 20)) return -1;   /* servers never mask */
    if (w->rx_len - n < size) return 0;
    *payload = p + n;
    *len = (size_t)size;
    *consumed = n + (size_t)size;
    return 1;
}

int ws_poll_message(Ws *w, const uint8_t **data, size_t *len)
{
    if (w->closed) return -1;
    if (w->msg_done) { w->msg_len = 0; w->msg_done = 0; }
    for (;;) {
        int fin, opcode;
        const uint8_t *payload;
        size_t size, consumed;
        int status = take_frame(w, &fin, &opcode, &payload, &size, &consumed);
        if (status < 0) { w->closed = 1; return -1; }
        if (status == 0) {
            uint8_t buf[16384];
            long got = tls_read(&w->tls, buf, sizeof(buf));
            if (got == -2) return 0;
            if (got <= 0 || !append(&w->rx, &w->rx_len, &w->rx_cap, buf, (size_t)got)) {
                w->closed = 1;
                return -1;
            }
            continue;
        }
        int result = 0;
        if (opcode == WS_PING) {
            ws_send(w, WS_PONG, payload, size);
        } else if (opcode == WS_CLOSE) {
            ws_send(w, WS_CLOSE, payload, size < 2 ? size : 2);
            w->closed = 1;
            result = -1;
        } else if (opcode == WS_PONG) {
        } else {
            if (opcode) w->msg_opcode = opcode;
            if (!append(&w->msg, &w->msg_len, &w->msg_cap, payload, size)) { w->closed = 1; return -1; }
            if (fin) result = 1;
        }
        memmove(w->rx, w->rx + consumed, w->rx_len - consumed);
        w->rx_len -= consumed;
        if (result) {
            w->msg_done = 1;
            *data = w->msg;
            *len = w->msg_len;
            return result;
        }
    }
}

void ws_close(Ws *w)
{
    if (w->tls.ssl && !w->closed) ws_send(w, WS_CLOSE, "\x03\xe8", 2);
    tls_close(&w->tls);
    free(w->rx);
    free(w->msg);
    memset(w, 0, sizeof(*w));
    w->tls.fd = -1;
}
