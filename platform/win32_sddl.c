#include "win32_sddl.h"

#include <stdlib.h>
#include <string.h>

/* Parsing works on a narrow copy: SDDL is ASCII by definition, so any non-ASCII character is malformed input. */
#define SDDL_MAX 4096
#define MAX_ACES 128
#define MAX_SID 68                       /* revision, count, authority, 15 sub-authorities */

typedef struct { uint8_t bytes[MAX_SID]; size_t size; } Sid;
typedef struct { uint8_t type, flags; uint32_t mask; Sid sid; } Ace;
typedef struct { int present, null_acl; uint16_t control; Ace aces[MAX_ACES]; unsigned count; } Acl;

static int sid_build(Sid *sid, uint64_t authority, const uint32_t *sub, unsigned count)
{
    if (count > 15) return 0;
    sid->bytes[0] = 1;
    sid->bytes[1] = (uint8_t)count;
    for (int i = 0; i < 6; ++i) sid->bytes[2 + i] = (uint8_t)(authority >> (8 * (5 - i)));   /* big-endian */
    memcpy(sid->bytes + 8, sub, 4u * count);
    sid->size = 8u + 4u * count;
    return 1;
}

/* Two-letter aliases that do not depend on a domain. */
static int sid_alias(const char *s, Sid *sid)
{
    static const struct { char name[3]; uint8_t authority; uint8_t count; uint32_t sub[2]; } aliases[] = {
        { "WD", 1, 1, { 0 } },       { "CO", 3, 1, { 0 } },       { "CG", 3, 1, { 1 } },
        { "OW", 3, 1, { 4 } },       { "NU", 5, 1, { 2 } },       { "IU", 5, 1, { 4 } },
        { "SU", 5, 1, { 6 } },       { "AN", 5, 1, { 7 } },       { "ED", 5, 1, { 9 } },
        { "PS", 5, 1, { 10 } },      { "AU", 5, 1, { 11 } },      { "RC", 5, 1, { 12 } },
        { "SY", 5, 1, { 18 } },      { "LS", 5, 1, { 19 } },      { "NS", 5, 1, { 20 } },
        { "BA", 5, 2, { 32, 544 } }, { "BU", 5, 2, { 32, 545 } }, { "BG", 5, 2, { 32, 546 } },
        { "PU", 5, 2, { 32, 547 } }, { "AO", 5, 2, { 32, 548 } }, { "SO", 5, 2, { 32, 549 } },
        { "PO", 5, 2, { 32, 550 } }, { "BO", 5, 2, { 32, 551 } }, { "RE", 5, 2, { 32, 552 } },
        { "RU", 5, 2, { 32, 554 } }, { "RD", 5, 2, { 32, 555 } }, { "NO", 5, 2, { 32, 556 } },
        { "MU", 5, 2, { 32, 558 } }, { "LU", 5, 2, { 32, 559 } }, { "IS", 5, 2, { 32, 568 } },
        { "CY", 5, 2, { 32, 569 } }, { "ER", 5, 2, { 32, 573 } }, { "HA", 5, 2, { 32, 578 } },
        { "AC", 15, 2, { 2, 1 } },
        { "LW", 16, 1, { 4096 } },   { "ME", 16, 1, { 8192 } },   { "MP", 16, 1, { 8448 } },
        { "HI", 16, 1, { 12288 } },  { "SI", 16, 1, { 16384 } },
    };
    for (size_t i = 0; i < sizeof(aliases) / sizeof(*aliases); ++i)
        if (s[0] == aliases[i].name[0] && s[1] == aliases[i].name[1])
            return sid_build(sid, aliases[i].authority, aliases[i].sub, aliases[i].count);
    return 0;
}

/* A SID at *p ("S-1-5-..." or an alias); advances *p past it. */
static int parse_sid(const char **p, Sid *sid)
{
    const char *s = *p;
    if (s[0] == 'S' && s[1] == '-') {
        s += 2;
        char *end;
        const unsigned long revision = strtoul(s, &end, 10);
        if (end == s || revision != 1 || *end != '-') return 0;
        s = end + 1;
        const unsigned long long authority = strtoull(s, &end, 10);
        if (end == s || authority > 0xffffffffffffull) return 0;
        s = end;
        uint32_t sub[15];
        unsigned count = 0;
        while (*s == '-' && count < 15) {
            const unsigned long value = strtoul(s + 1, &end, 10);
            if (end == s + 1 || value > 0xfffffffful) return 0;
            sub[count++] = (uint32_t)value;
            s = end;
        }
        if (*s == '-') return 0;                           /* more than 15 sub-authorities */
        if (!sid_build(sid, authority, sub, count)) return 0;
        *p = s;
        return 1;
    }
    if (!s[0] || !s[1] || !sid_alias(s, sid)) return 0;
    *p = s + 2;
    return 1;
}

static int parse_rights(const char *s, size_t length, uint32_t *mask)
{
    if (!length) { *mask = 0; return 1; }
    if (s[0] >= '0' && s[0] <= '9') {
        char number[32];
        if (length >= sizeof(number)) return 0;
        memcpy(number, s, length);
        number[length] = 0;
        char *end;
        const unsigned long long value = strtoull(number, &end, 0);
        if (*end || value > 0xffffffffull) return 0;
        *mask = (uint32_t)value;
        return 1;
    }
    static const struct { char name[3]; uint32_t mask; } rights[] = {
        { "GA", 0x10000000u }, { "GR", 0x80000000u }, { "GW", 0x40000000u }, { "GX", 0x20000000u },
        { "RC", 0x00020000u }, { "SD", 0x00010000u }, { "WD", 0x00040000u }, { "WO", 0x00080000u },
        { "RP", 0x00000010u }, { "WP", 0x00000020u }, { "CC", 0x00000001u }, { "DC", 0x00000002u },
        { "LC", 0x00000004u }, { "SW", 0x00000008u }, { "LO", 0x00000080u }, { "DT", 0x00000040u },
        { "CR", 0x00000100u },
        { "FA", 0x001f01ffu }, { "FR", 0x00120089u }, { "FW", 0x00120116u }, { "FX", 0x001200a0u },
        { "KA", 0x000f003fu }, { "KR", 0x00020019u }, { "KW", 0x00020006u }, { "KX", 0x00020019u },
        { "NR", 0x00000002u }, { "NW", 0x00000001u }, { "NX", 0x00000004u },
    };
    uint32_t value = 0;
    if (length % 2) return 0;
    for (size_t at = 0; at < length; at += 2) {
        size_t i = 0;
        for (; i < sizeof(rights) / sizeof(*rights); ++i)
            if (s[at] == rights[i].name[0] && s[at + 1] == rights[i].name[1]) break;
        if (i == sizeof(rights) / sizeof(*rights)) return 0;
        value |= rights[i].mask;
    }
    *mask = value;
    return 1;
}

static int parse_ace_flags(const char *s, size_t length, uint8_t *flags)
{
    static const struct { char name[3]; uint8_t flag; } table[] = {
        { "OI", 0x01 }, { "CI", 0x02 }, { "NP", 0x04 }, { "IO", 0x08 }, { "ID", 0x10 }, { "SA", 0x40 }, { "FA", 0x80 },
    };
    uint8_t value = 0;
    if (length % 2) return 0;
    for (size_t at = 0; at < length; at += 2) {
        size_t i = 0;
        for (; i < sizeof(table) / sizeof(*table); ++i)
            if (s[at] == table[i].name[0] && s[at + 1] == table[i].name[1]) break;
        if (i == sizeof(table) / sizeof(*table)) return 0;
        value |= table[i].flag;
    }
    *flags = value;
    return 1;
}

/* "(type;flags;rights;object;inherited-object;sid)" at *p. */
static int parse_ace(const char **p, Ace *ace)
{
    const char *s = *p;
    if (*s != '(') return 0;
    const char *field[6];
    size_t length[6];
    unsigned n = 0;
    const char *start = ++s;
    for (;; ++s) {
        if (!*s) return 0;
        if (*s == ';' || *s == ')') {
            if (n == 6) return 0;
            field[n] = start;
            length[n] = (size_t)(s - start);
            ++n;
            if (*s == ')') break;
            start = s + 1;
        }
    }
    if (n != 6) return 0;                                   /* conditional/resource ACEs are not supported */
    static const struct { const char *name; uint8_t type; } types[] = {
        { "A", 0x00 }, { "D", 0x01 }, { "AU", 0x02 }, { "AL", 0x03 }, { "ML", 0x11 },
    };
    size_t t = 0;
    for (; t < sizeof(types) / sizeof(*types); ++t)
        if (strlen(types[t].name) == length[0] && !memcmp(types[t].name, field[0], length[0])) break;
    if (t == sizeof(types) / sizeof(*types)) return 0;
    ace->type = types[t].type;
    if (length[3] || length[4]) return 0;                  /* object GUIDs only exist on object ACEs */
    if (!parse_ace_flags(field[1], length[1], &ace->flags) || !parse_rights(field[2], length[2], &ace->mask))
        return 0;
    char sid_text[200];
    if (!length[5] || length[5] >= sizeof(sid_text)) return 0;
    memcpy(sid_text, field[5], length[5]);
    sid_text[length[5]] = 0;
    const char *q = sid_text;
    if (!parse_sid(&q, &ace->sid) || *q) return 0;
    *p = s + 1;
    return 1;
}

/* After "D:" or "S:": flags, then ACEs, up to the next component or the end. */
static int parse_acl(const char **p, Acl *acl, int sacl)
{
    const char *s = *p;
    acl->present = 1;
    while (*s && *s != '(' && !(s[1] == ':' && (s[0] == 'O' || s[0] == 'G' || s[0] == 'D' || s[0] == 'S'))) {
        if (!strncmp(s, "NO_ACCESS_CONTROL", 17)) { acl->null_acl = 1; s += 17; }
        else if (s[0] == 'P') { acl->control |= sacl ? 0x2000 : 0x1000; s += 1; }       /* SE_*_PROTECTED */
        else if (s[0] == 'A' && s[1] == 'I') { acl->control |= sacl ? 0x0800 : 0x0400; s += 2; } /* AUTO_INHERITED */
        else if (s[0] == 'A' && s[1] == 'R') { acl->control |= sacl ? 0x0200 : 0x0100; s += 2; } /* INHERIT_REQ */
        else return 0;
    }
    while (*s == '(') {
        if (acl->count == MAX_ACES || !parse_ace(&s, &acl->aces[acl->count])) return 0;
        ++acl->count;
    }
    if (acl->null_acl && acl->count) return 0;
    *p = s;
    return 1;
}

static size_t acl_size(const Acl *acl)
{
    size_t size = 8;
    for (unsigned i = 0; i < acl->count; ++i) size += 8 + acl->aces[i].sid.size;
    return size;
}

static uint8_t *write_acl(uint8_t *at, const Acl *acl)
{
    const size_t size = acl_size(acl);
    at[0] = 2;                                              /* ACL_REVISION */
    at[1] = 0;
    at[2] = (uint8_t)size; at[3] = (uint8_t)(size >> 8);
    at[4] = (uint8_t)acl->count; at[5] = (uint8_t)(acl->count >> 8);
    at[6] = at[7] = 0;
    uint8_t *ace = at + 8;
    for (unsigned i = 0; i < acl->count; ++i) {
        const Ace *a = &acl->aces[i];
        const size_t ace_size = 8 + a->sid.size;
        ace[0] = a->type;
        ace[1] = a->flags;
        ace[2] = (uint8_t)ace_size; ace[3] = (uint8_t)(ace_size >> 8);
        memcpy(ace + 4, &a->mask, 4);
        memcpy(ace + 8, a->sid.bytes, a->sid.size);
        ace += ace_size;
    }
    return ace;
}

uint32_t beer_sddl_to_descriptor(const void *sddl, int narrow, uint8_t *out, size_t capacity, size_t *size)
{
    if (!sddl || !size) return 87;
    static __thread char text[SDDL_MAX];
    size_t n = 0;                                          /* whitespace between tokens is ignored, as on Windows */
    for (size_t i = 0;; ++i) {
        const unsigned c = narrow ? ((const unsigned char *)sddl)[i] : ((const uint16_t *)sddl)[i];
        if (c > 0x7f) return 87;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (n == SDDL_MAX) return 87;
        text[n++] = (char)c;
        if (!c) break;
    }
    Sid owner = { { 0 }, 0 }, group = { { 0 }, 0 };
    static __thread Acl dacl, sacl;
    memset(&dacl, 0, sizeof(dacl));
    memset(&sacl, 0, sizeof(sacl));
    int have_owner = 0, have_group = 0;
    const char *p = text;
    while (*p) {
        if (p[1] != ':') return 87;
        const char kind = p[0];
        p += 2;
        if (kind == 'O' && !have_owner) { if (!parse_sid(&p, &owner)) return 87; have_owner = 1; }
        else if (kind == 'G' && !have_group) { if (!parse_sid(&p, &group)) return 87; have_group = 1; }
        else if (kind == 'D' && !dacl.present) { if (!parse_acl(&p, &dacl, 0)) return 87; }
        else if (kind == 'S' && !sacl.present) { if (!parse_acl(&p, &sacl, 1)) return 87; }
        else return 87;
    }

    /* Layout: header, SACL, DACL, owner, group (all 4-byte aligned: SID and ACE sizes are multiples of 4). */
    size_t total = 20;
    const size_t sacl_offset = sacl.present && !sacl.null_acl ? total : 0;
    if (sacl_offset) total += acl_size(&sacl);
    const size_t dacl_offset = dacl.present && !dacl.null_acl ? total : 0;
    if (dacl_offset) total += acl_size(&dacl);
    const size_t owner_offset = have_owner ? total : 0;
    total += owner.size;
    const size_t group_offset = have_group ? total : 0;
    total += group.size;
    *size = total;
    if (!out) return 0;
    if (capacity < total) return 122;

    uint16_t control = 0x8000 | dacl.control | sacl.control;          /* SE_SELF_RELATIVE */
    if (dacl.present) control |= 0x0004;                               /* SE_DACL_PRESENT (NULL DACL: offset 0) */
    if (sacl.present) control |= 0x0010;                               /* SE_SACL_PRESENT */
    memset(out, 0, total);
    out[0] = 1;                                                        /* SECURITY_DESCRIPTOR_REVISION */
    memcpy(out + 2, &control, 2);
    const uint32_t offsets[4] = { (uint32_t)owner_offset, (uint32_t)group_offset, (uint32_t)sacl_offset,
                                  (uint32_t)dacl_offset };
    memcpy(out + 4, offsets, sizeof(offsets));
    if (sacl_offset) write_acl(out + sacl_offset, &sacl);
    if (dacl_offset) write_acl(out + dacl_offset, &dacl);
    if (owner_offset) memcpy(out + owner_offset, owner.bytes, owner.size);
    if (group_offset) memcpy(out + group_offset, group.bytes, group.size);
    return 0;
}
