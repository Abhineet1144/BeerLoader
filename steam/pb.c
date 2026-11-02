#include "pb.h"

#include <stdlib.h>
#include <string.h>

void pb_free(PbBuf *b)
{
    free(b->data);
    memset(b, 0, sizeof(*b));
}

void pb_raw(PbBuf *b, const void *data, size_t len)
{
    if (b->failed || !len) return;
    if (b->len + len > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        while (cap < b->len + len) cap *= 2;
        uint8_t *grown = realloc(b->data, cap);
        if (!grown) { b->failed = 1; return; }
        b->data = grown;
        b->cap = cap;
    }
    memcpy(b->data + b->len, data, len);
    b->len += len;
}

void pb_varint(PbBuf *b, uint64_t v)
{
    uint8_t out[10];
    size_t n = 0;
    do {
        out[n] = v & 0x7f;
        v >>= 7;
        if (v) out[n] |= 0x80;
        n++;
    } while (v);
    pb_raw(b, out, n);
}

static void key(PbBuf *b, uint32_t field, int type) { pb_varint(b, ((uint64_t)field << 3) | (uint64_t)type); }

void pb_u64(PbBuf *b, uint32_t field, uint64_t v) { key(b, field, PB_VARINT); pb_varint(b, v); }
void pb_i32(PbBuf *b, uint32_t field, int32_t v) { key(b, field, PB_VARINT); pb_varint(b, (uint64_t)(int64_t)v); }

void pb_fixed64(PbBuf *b, uint32_t field, uint64_t v)
{
    uint8_t out[8];
    for (int i = 0; i < 8; i++) out[i] = (uint8_t)(v >> (8 * i));
    key(b, field, PB_FIXED64);
    pb_raw(b, out, 8);
}

void pb_bytes(PbBuf *b, uint32_t field, const void *data, size_t len)
{
    key(b, field, PB_BYTES);
    pb_varint(b, len);
    pb_raw(b, data, len);
}

void pb_string(PbBuf *b, uint32_t field, const char *s)
{
    if (s) pb_bytes(b, field, s, strlen(s));
}

void pb_msg(PbBuf *b, uint32_t field, const PbBuf *message)
{
    if (message->failed) b->failed = 1;
    pb_bytes(b, field, message->data, message->len);
}

static int read_varint(PbReader *r, uint64_t *v)
{
    uint64_t out = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (r->p >= r->end) return 0;
        uint8_t byte = *r->p++;
        out |= (uint64_t)(byte & 0x7f) << shift;
        if (!(byte & 0x80)) { *v = out; return 1; }
    }
    return 0;
}

int pb_next(PbReader *r, PbField *f)
{
    if (r->p >= r->end) return 0;
    uint64_t k;
    if (!read_varint(r, &k) || !(k >> 3) || (k >> 3) > UINT32_MAX) return -1;
    memset(f, 0, sizeof(*f));
    f->field = (uint32_t)(k >> 3);
    f->type = (int)(k & 7);
    switch (f->type) {
    case PB_VARINT:
        return read_varint(r, &f->value) ? 1 : -1;
    case PB_FIXED64:
    case PB_FIXED32: {
        size_t n = f->type == PB_FIXED64 ? 8 : 4;
        if ((size_t)(r->end - r->p) < n) return -1;
        for (size_t i = 0; i < n; i++) f->value |= (uint64_t)r->p[i] << (8 * i);
        r->p += n;
        return 1;
    }
    case PB_BYTES: {
        uint64_t len;
        if (!read_varint(r, &len) || len > (uint64_t)(r->end - r->p)) return -1;
        f->data = r->p;
        f->len = (size_t)len;
        r->p += len;
        return 1;
    }
    default:
        return -1;
    }
}

char *pb_strdup(const PbField *f)
{
    if (f->type != PB_BYTES) return NULL;
    char *s = malloc(f->len + 1);
    if (!s) return NULL;
    memcpy(s, f->data, f->len);
    s[f->len] = 0;
    return s;
}

int pb_find(const uint8_t *data, size_t len, uint32_t field, PbField *out)
{
    PbReader r = { data, data + len };
    PbField f;
    int status;
    while ((status = pb_next(&r, &f)) == 1)
        if (f.field == field) { *out = f; return 1; }
    return 0;
}
