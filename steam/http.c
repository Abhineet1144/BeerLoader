#include "http.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define TIMEOUT_MS 30000
#define MAX_BODY (512u << 20)

void http_init(HttpConn *c, const char *host, int https)
{
    memset(c, 0, sizeof(*c));
    snprintf(c->host, sizeof(c->host), "%s", host);
    c->https = https;
    c->fd = -1;
    c->tls.fd = -1;
}

void http_close(HttpConn *c)
{
    if (c->https) tls_close(&c->tls);
    else if (c->fd >= 0) close(c->fd);
    c->fd = -1;
    c->connected = 0;
    c->len = 0;
}

static int open_conn(HttpConn *c, char *err, size_t errlen)
{
    http_close(c);
    if (c->https) {
        if (!tls_connect(&c->tls, c->host, "443", TIMEOUT_MS, err, errlen)) return 0;
    } else {
        c->fd = net_connect_tcp(c->host, "80", TIMEOUT_MS, err, errlen);
        if (c->fd < 0) return 0;
    }
    c->connected = 1;
    return 1;
}

static long raw_read(HttpConn *c, void *buf, size_t n)
{
    if (c->https) {
        long got = tls_read(&c->tls, buf, n);
        return got == -2 ? -1 : got;            /* a blocking socket only "wants read" on timeout */
    }
    ssize_t got = recv(c->fd, buf, n, 0);
    return got < 0 ? -1 : got;
}

static int raw_write(HttpConn *c, const void *data, size_t n)
{
    if (c->https) return tls_write_all(&c->tls, data, n);
    const char *p = data;
    while (n) {
        ssize_t sent = send(c->fd, p, n, MSG_NOSIGNAL);
        if (sent <= 0) { if (sent < 0 && errno == EINTR) continue; return 0; }
        p += sent;
        n -= (size_t)sent;
    }
    return 1;
}

/* Ensures at least need bytes are buffered. */
static int fill(HttpConn *c, size_t need)
{
    while (c->len < need) {
        if (c->cap - c->len < 65536) {
            size_t cap = c->cap ? c->cap * 2 : 131072;
            while (cap - c->len < 65536) cap *= 2;
            uint8_t *grown = realloc(c->buf, cap);
            if (!grown) return 0;
            c->buf = grown;
            c->cap = cap;
        }
        long got = raw_read(c, c->buf + c->len, c->cap - c->len);
        if (got <= 0) return 0;
        c->len += (size_t)got;
    }
    return 1;
}

static void consume(HttpConn *c, size_t n)
{
    memmove(c->buf, c->buf + n, c->len - n);
    c->len -= n;
}

/* Index just past the next CRLF at or after start, reading more as needed; 0 on failure. */
static size_t line_end(HttpConn *c, size_t start)
{
    for (size_t i = start;; i++) {
        if (i + 1 >= c->len && !fill(c, i + 2)) return 0;
        if (c->buf[i] == '\r' && c->buf[i + 1] == '\n') return i + 2;
        if (i > start + 65536) return 0;
    }
}

static int read_response(HttpConn *c, uint8_t **body, size_t *body_len, int *keep_alive)
{
    /* Status line and headers. */
    size_t end = 0, pos = 0;
    for (;;) {
        size_t next = line_end(c, pos);
        if (!next) return 0;
        if (next - pos == 2) { end = next; break; }
        pos = next;
        if (pos > 65536) return 0;
    }
    int status = 0;
    if (end < 12 || memcmp(c->buf, "HTTP/1.", 7) || sscanf((char *)c->buf + 8, " %d", &status) != 1) return 0;
    long long content_length = -1;
    int chunked = 0;
    *keep_alive = c->buf[7] == '1';
    for (size_t i = 0; i < end;) {
        size_t j = i;
        while (j + 1 < end && !(c->buf[j] == '\r' && c->buf[j + 1] == '\n')) j++;
        char line[512];
        size_t n = j - i < sizeof(line) - 1 ? j - i : sizeof(line) - 1;
        memcpy(line, c->buf + i, n);
        line[n] = 0;
        if (!strncasecmp(line, "Content-Length:", 15)) content_length = atoll(line + 15);
        else if (!strncasecmp(line, "Transfer-Encoding:", 18) && strcasestr(line + 18, "chunked")) chunked = 1;
        else if (!strncasecmp(line, "Connection:", 11)) {
            if (strcasestr(line + 11, "close")) *keep_alive = 0;
            else if (strcasestr(line + 11, "keep-alive")) *keep_alive = 1;
        }
        i = j + 2;
    }
    consume(c, end);

    uint8_t *out = NULL;
    size_t out_len = 0;
    if (chunked) {
        for (;;) {
            size_t next = line_end(c, 0);
            if (!next) { free(out); return 0; }
            unsigned long long size = strtoull((char *)c->buf, NULL, 16);
            consume(c, next);
            if (!size) {
                size_t trailer;                     /* trailers end with an empty line */
                while ((trailer = line_end(c, 0)) && trailer != 2) consume(c, trailer);
                if (trailer) consume(c, trailer);
                break;
            }
            if (out_len + size > MAX_BODY || !fill(c, (size_t)size + 2)) { free(out); return 0; }
            uint8_t *grown = realloc(out, out_len + (size_t)size + 1);
            if (!grown) { free(out); return 0; }
            out = grown;
            memcpy(out + out_len, c->buf, (size_t)size);
            out_len += (size_t)size;
            consume(c, (size_t)size + 2);
        }
    } else if (content_length >= 0) {
        if ((unsigned long long)content_length > MAX_BODY || !fill(c, (size_t)content_length)) return 0;
        out = malloc((size_t)content_length + 1);
        if (!out) return 0;
        memcpy(out, c->buf, (size_t)content_length);
        out_len = (size_t)content_length;
        consume(c, out_len);
    } else {
        /* Body runs to the end of the connection. */
        while (fill(c, c->len + 1)) {
            if (c->len > MAX_BODY) return 0;
        }
        out = malloc(c->len + 1);
        if (!out) return 0;
        memcpy(out, c->buf, c->len);
        out_len = c->len;
        c->len = 0;
        *keep_alive = 0;
    }
    if (!out) out = malloc(1);
    if (out) out[out_len] = 0;
    *body = out;
    *body_len = out_len;
    return status;
}

int http_get(HttpConn *c, const char *path, uint8_t **body, size_t *len, char *err, size_t errlen)
{
    *body = NULL;
    *len = 0;
    char request[1024];
    int n = snprintf(request, sizeof(request),
                     "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Valve/Steam HTTP Client 1.0\r\n"
                     "Accept: */*\r\nConnection: keep-alive\r\n\r\n", path, c->host);
    if (n <= 0 || (size_t)n >= sizeof(request)) { snprintf(err, errlen, "request too long"); return 0; }
    for (int attempt = 0; attempt < 2; attempt++) {
        int reused = c->connected;
        if (!c->connected && !open_conn(c, err, errlen)) return 0;
        int keep_alive = 0;
        int status = raw_write(c, request, (size_t)n) ? read_response(c, body, len, &keep_alive) : 0;
        if (status) {
            if (!keep_alive) http_close(c);
            return status;
        }
        http_close(c);
        if (!reused) break;                       /* a fresh connection failed: do not retry */
    }
    snprintf(err, errlen, "connection to %s failed", c->host);
    return 0;
}
