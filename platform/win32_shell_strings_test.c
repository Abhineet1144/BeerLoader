#include "win32_shell_strings.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#define W(s) ((const uint16_t *)u##s)
static void eq(const uint16_t *a, const uint16_t *b)
{
    while (*a || *b) assert(*a++ == *b++);
}
int main(void)
{
    uint16_t out[1024];
    assert(beer_PathCanonicalizeW(out, W("Z:\\games\\.\\SB\\..\\SB.exe")));
    eq(out, W("Z:\\games\\SB.exe"));
    assert(beer_PathCanonicalizeW(out, W("C:\\..\\..\\file")));
    eq(out, W("C:\\file"));
    assert(beer_PathCanonicalizeW(out, W("\\\\server\\share\\..\\file")));
    eq(out, W("\\\\server\\share\\file"));
    assert(beer_PathCanonicalizeW(out, W("C:\\dir\\"))); eq(out, W("C:\\dir\\"));
    assert(beer_PathCanonicalizeW(out, W(""))); eq(out, W("\\"));
    assert(beer_PathCombineW(out, W("C:\\games"), W("SB\\x.exe")) == (uintptr_t)out);
    eq(out, W("C:\\games\\SB\\x.exe"));
    assert(beer_PathCombineW(out, out, W("..\\y.exe"))); eq(out, W("C:\\games\\SB\\y.exe"));
    assert(beer_PathCombineW(out, W("C:\\games"), W("\\root.exe"))); eq(out, W("C:\\root.exe"));
    assert(beer_PathCombineW(out, W("C:\\games"), W("D:\\x.exe"))); eq(out, W("D:\\x.exe"));
    assert(beer_PathCombineW(out, NULL, W("relative.exe"))); eq(out, W("relative.exe"));
    assert(!beer_PathCombineW(out, NULL, NULL));
    uint16_t long_path[261];
    for (int i = 0; i < 260; ++i) long_path[i] = 'a';
    long_path[260] = 0;
    assert(!beer_PathCanonicalizeW(out, long_path) && !out[0]);
    assert(!beer_PathCanonicalizeW(out, NULL) && !out[0]);
    assert(beer_wsprintfW(out, W("%s\\%s %d %u %08X %I64d %%"), W("C:\\games"), W("SB.exe"), -17,
                         42u, 0xabcdu, (int64_t)INT64_MIN) == 54);
    eq(out, W("C:\\games\\SB.exe -17 42 0000ABCD -9223372036854775808 %"));
    beer_wsprintfW(out, W("%s:%s:%s:%s:%s"), W("a"), W("b"), W("c"), W("d"), W("e"));
    eq(out, W("a:b:c:d:e")); /* Arguments beyond the register slots. */
    beer_wsprintfW(out, W("%-5.2s:%hs:%c:%#06x"), W("abcd"), "ascii", 'Z', 0x12);
    eq(out, W("ab   :ascii:Z:0x0012"));
    uint64_t args[] = {(uintptr_t)W("wide"), 123};
    beer_wvsprintfW(out, W("%s %d"), args); eq(out, W("wide 123"));
    puts("shell path and Win64 wide-format contracts: PASS");
    return 0;
}