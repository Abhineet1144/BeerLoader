/* Protobuf wire-format round trips, checked against encodings produced by protoc. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "pb.h"

static void expect(const PbBuf *b, const char *hex)
{
    char got[512] = "";
    for (size_t i = 0; i < b->len; i++) sprintf(got + 2 * i, "%02x", b->data[i]);
    if (strcmp(got, hex)) {
        fprintf(stderr, "encoded %s, expected %s\n", got, hex);
        assert(0);
    }
}

int main(void)
{
    PbBuf b = {0};
    pb_u64(&b, 1, 150);                       /* the protobuf documentation's example */
    expect(&b, "089601");
    pb_free(&b);

    pb_i32(&b, 7, -203);                      /* negative int32 is a 10-byte varint */
    expect(&b, "38b5feffffffffffffff01");
    pb_free(&b);

    pb_fixed64(&b, 1, 0x0110000100000000ull);
    pb_string(&b, 12, "Authentication.PollAuthSessionStatus#1");
    expect(&b, "090000000001001001622641757468656e7469636174696f6e2e506f6c6c4175746853657373696f6e5374617475732331");
    pb_free(&b);

    /* Nested message and decoding. */
    PbBuf inner = {0}, outer = {0};
    pb_string(&inner, 1, "Beer");
    pb_u64(&inner, 2, 1);
    pb_msg(&outer, 3, &inner);
    pb_u64(&outer, 99, 1ull << 40);
    PbReader r = { outer.data, outer.data + outer.len };
    PbField f;
    assert(pb_next(&r, &f) == 1 && f.field == 3 && f.type == PB_BYTES && f.len == inner.len);
    PbField name;
    assert(pb_find(f.data, f.len, 1, &name) && name.len == 4 && !memcmp(name.data, "Beer", 4));
    assert(pb_next(&r, &f) == 1 && f.field == 99 && f.value == 1ull << 40);
    assert(pb_next(&r, &f) == 0);
    pb_free(&inner);
    pb_free(&outer);

    /* fixed32 (a float interval of 5.0 s) and malformed input. */
    const uint8_t interval[] = { 0x25, 0x00, 0x00, 0xa0, 0x40 };
    assert(pb_find(interval, sizeof(interval), 4, &f) && f.type == PB_FIXED32 && f.value == 0x40a00000u);
    const uint8_t truncated[] = { 0x12, 0x05, 'a', 'b' };
    r = (PbReader){ truncated, truncated + sizeof(truncated) };
    assert(pb_next(&r, &f) == -1);
    const uint8_t bad_type[] = { 0x0b };
    r = (PbReader){ bad_type, bad_type + 1 };
    assert(pb_next(&r, &f) == -1);

    puts("steam protobuf tests passed");
    return 0;
}
