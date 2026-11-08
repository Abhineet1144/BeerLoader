#include "win32_sddl.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t buffer[4096];

static size_t convert(const char *sddl, uint32_t expect)
{
    size_t size = 0;
    const uint32_t rc = beer_sddl_to_descriptor(sddl, 1, buffer, sizeof(buffer), &size);
    if (rc != expect) fprintf(stderr, "'%s': got %u, expected %u\n", sddl, rc, expect);
    assert(rc == expect);
    return size;
}
static uint16_t u16at(size_t o) { uint16_t v; memcpy(&v, buffer + o, 2); return v; }
static uint32_t u32at(size_t o) { uint32_t v; memcpy(&v, buffer + o, 4); return v; }

int main(void)
{
    /* Everyone: generic all. Header 20 + ACL 8 + ACE (8 + SID 12) = 48. */
    assert(convert("D:(A;;GA;;;WD)", 0) == 48);
    assert(buffer[0] == 1 && u16at(2) == (0x8000 | 0x0004));
    assert(u32at(4) == 0 && u32at(8) == 0 && u32at(12) == 0 && u32at(16) == 20);
    assert(buffer[20] == 2 && u16at(22) == 28 && u16at(24) == 1);           /* ACL revision, size, ACE count */
    assert(buffer[28] == 0 && buffer[29] == 0 && u16at(30) == 20);           /* ACCESS_ALLOWED, no flags, size */
    assert(u32at(32) == 0x10000000u);                                       /* GENERIC_ALL */
    assert(buffer[36] == 1 && buffer[37] == 1 && buffer[43] == 1 && u32at(44) == 0);   /* S-1-1-0 */

    /* Owner, group, protected DACL with two ACEs, inheritance flags and file rights; whitespace tolerated. */
    size_t size = convert("O:BA G:SY D:P(A;OICI;FA;;;BA)(D;;0x10;;;S-1-5-21-1-2-3-1001)", 0);
    assert(u16at(2) == (0x8000 | 0x0004 | 0x1000));
    const uint32_t dacl = u32at(16), owner = u32at(4), group = u32at(8);
    assert(dacl == 20 && owner && group && size == group + 12);
    assert(u16at(dacl + 4) == 2);
    assert(buffer[dacl + 8] == 0 && buffer[dacl + 9] == 0x03 && u32at(dacl + 12) == 0x001f01ffu);
    const size_t second = dacl + 8 + u16at(dacl + 10);
    assert(buffer[second] == 1 && u32at(second + 4) == 0x10 && buffer[second + 9] == 5);     /* deny, 5 subs */
    assert(buffer[owner + 1] == 2 && u32at(owner + 8) == 32 && u32at(owner + 12) == 544);
    assert(buffer[group + 1] == 1 && u32at(group + 8) == 18);

    /* NULL DACL: present, no ACL. Mandatory label in a SACL. */
    assert(convert("D:NO_ACCESS_CONTROL", 0) == 20 && u16at(2) == (0x8000 | 0x0004) && u32at(16) == 0);
    convert("S:(ML;;NW;;;LW)", 0);
    assert(u16at(2) == (0x8000 | 0x0010) && u32at(12) == 20 && buffer[28] == 0x11 && u32at(32) == 1);

    /* Wide input. */
    const uint16_t wide[] = { 'D', ':', '(', 'A', ';', ';', 'G', 'R', ';', ';', ';', 'A', 'U', ')', 0 };
    assert(beer_sddl_to_descriptor(wide, 0, buffer, sizeof(buffer), &size) == 0 && size == 48);

    /* Size query and short buffers. */
    assert(beer_sddl_to_descriptor("D:(A;;GA;;;WD)", 1, NULL, 0, &size) == 0 && size == 48);
    assert(beer_sddl_to_descriptor("D:(A;;GA;;;WD)", 1, buffer, 47, &size) == 122);

    /* Malformed input. */
    convert("D:(A;;GA;;;XX)", 87);
    convert("D:(Q;;GA;;;WD)", 87);
    convert("D:(A;;GA;;WD)", 87);
    convert("D:(OA;;GA;bf967aba-0de6-11d0-a285-00aa003049e2;;WD)", 87);
    convert("X:BA", 87);
    convert("O:BAO:SY", 87);
    puts("win32_sddl_test: ok");
    return 0;
}
