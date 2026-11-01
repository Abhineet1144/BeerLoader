/* Minimal protobuf wire-format encoder and decoder for Steam messages.
 * Only the scalar, length-delimited and fixed types Steam uses are covered. */
#ifndef BEER_STEAM_PB_H
#define BEER_STEAM_PB_H

#include <stddef.h>
#include <stdint.h>

enum { PB_VARINT = 0, PB_FIXED64 = 1, PB_BYTES = 2, PB_FIXED32 = 5 };

typedef struct {
    uint8_t *data;
    size_t len, cap;
    int failed;   /* allocation failed; the buffer content is incomplete */
} PbBuf;

void pb_free(PbBuf *b);
void pb_raw(PbBuf *b, const void *data, size_t len);
void pb_varint(PbBuf *b, uint64_t v);
void pb_u64(PbBuf *b, uint32_t field, uint64_t v);             /* uint32/uint64/enum/bool */
void pb_i32(PbBuf *b, uint32_t field, int32_t v);              /* int32 (sign-extended) */
void pb_fixed64(PbBuf *b, uint32_t field, uint64_t v);
void pb_bytes(PbBuf *b, uint32_t field, const void *data, size_t len);
void pb_string(PbBuf *b, uint32_t field, const char *s);       /* NULL writes nothing */
void pb_msg(PbBuf *b, uint32_t field, const PbBuf *message);

typedef struct {
    const uint8_t *p, *end;
} PbReader;

typedef struct {
    uint32_t field;
    int type;
    uint64_t value;           /* varint, fixed64, or fixed32 bits */
    const uint8_t *data;      /* PB_BYTES payload */
    size_t len;
} PbField;

/* Returns 1 with the next field, 0 at the end, -1 on malformed input. */
int pb_next(PbReader *r, PbField *f);
/* Copies a PB_BYTES field to a new NUL-terminated string (NULL on failure). */
char *pb_strdup(const PbField *f);
/* First occurrence of a field in a message, 1 if found. */
int pb_find(const uint8_t *data, size_t len, uint32_t field, PbField *out);

#endif
