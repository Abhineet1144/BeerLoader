#include "win32_version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint32_t va, vsize, raw, rawsize; } Section;

static int read_at(FILE *f, uint64_t offset, void *out, size_t size)
{
    return fseeko(f, (off_t)offset, SEEK_SET) == 0 && fread(out, 1, size, f) == size;
}

static int rva_to_offset(const Section *sections, unsigned count, uint32_t rva, uint64_t *offset)
{
    for (unsigned i = 0; i < count; ++i) {
        const uint32_t span = sections[i].vsize > sections[i].rawsize ? sections[i].vsize : sections[i].rawsize;
        if (rva >= sections[i].va && rva < sections[i].va + span) {
            *offset = (uint64_t)sections[i].raw + (rva - sections[i].va);
            return 1;
        }
    }
    return 0;
}

/* Finds the entry with `id` (or the first entry when id == UINT32_MAX) in a resource directory. */
static int resource_entry(FILE *f, uint64_t root, uint32_t directory, uint32_t id, uint32_t *target, int *is_directory)
{
    uint8_t header[16];
    if (!read_at(f, root + directory, header, sizeof(header))) return 0;
    const unsigned named = header[12] | (header[13] << 8), ids = header[14] | (header[15] << 8);
    for (unsigned i = 0; i < named + ids; ++i) {
        uint8_t entry[8];
        if (!read_at(f, root + directory + 16 + (uint64_t)i * 8, entry, sizeof(entry))) return 0;
        uint32_t name, offset;
        memcpy(&name, entry, 4); memcpy(&offset, entry + 4, 4);
        if (id != UINT32_MAX && (i < named || name != id)) continue;
        *is_directory = (offset & 0x80000000u) != 0;
        *target = offset & 0x7fffffffu;
        return 1;
    }
    return 0;
}

int beer_pe_version_resource(const char *host_path, uint8_t **data, uint32_t *size)
{
    FILE *f = fopen(host_path, "rb");
    if (!f) return 0;
    int ok = 0;
    uint8_t dos[64];
    uint32_t pe = 0;
    Section sections[96];
    unsigned count = 0;
    uint8_t head[24 + 240];
    if (!read_at(f, 0, dos, sizeof(dos)) || dos[0] != 'M' || dos[1] != 'Z') goto done;
    memcpy(&pe, dos + 0x3c, 4);
    if (!read_at(f, pe, head, sizeof(head)) || memcmp(head, "PE\0\0", 4)) goto done;
    count = head[6] | (head[7] << 8);
    const unsigned opt_size = head[20] | (head[21] << 8);
    if (count > 96 || opt_size < 112 + 16 * 8) goto done;
    const int pe32plus = (head[24] | (head[25] << 8)) == 0x20b;
    const uint8_t *dirs = head + 24 + (pe32plus ? 112 : 96);
    uint32_t rsrc_rva, rsrc_size;
    memcpy(&rsrc_rva, dirs + 2 * 8, 4); memcpy(&rsrc_size, dirs + 2 * 8 + 4, 4);
    if (!rsrc_rva) goto done;
    for (unsigned i = 0; i < count; ++i) {
        uint8_t s[40];
        if (!read_at(f, (uint64_t)pe + 24 + opt_size + i * 40u, s, sizeof(s))) goto done;
        memcpy(&sections[i].vsize, s + 8, 4); memcpy(&sections[i].va, s + 12, 4);
        memcpy(&sections[i].rawsize, s + 16, 4); memcpy(&sections[i].raw, s + 20, 4);
    }
    uint64_t root;
    if (!rva_to_offset(sections, count, rsrc_rva, &root)) goto done;
    uint32_t dir = 0, next;
    int is_dir;
    if (!resource_entry(f, root, dir, 16 /* RT_VERSION */, &next, &is_dir) || !is_dir) goto done;
    dir = next;
    if (!resource_entry(f, root, dir, UINT32_MAX, &next, &is_dir) || !is_dir) goto done;   /* first name/id (usually 1) */
    dir = next;
    if (!resource_entry(f, root, dir, UINT32_MAX, &next, &is_dir) || is_dir) goto done;    /* first language */
    uint8_t leaf[16];
    if (!read_at(f, root + next, leaf, sizeof(leaf))) goto done;
    uint32_t data_rva, data_size;
    memcpy(&data_rva, leaf, 4); memcpy(&data_size, leaf + 4, 4);
    uint64_t data_offset;
    if (!data_size || data_size > (1u << 20) || !rva_to_offset(sections, count, data_rva, &data_offset)) goto done;
    uint8_t *buffer = malloc(data_size);
    if (!buffer) goto done;
    if (!read_at(f, data_offset, buffer, data_size)) { free(buffer); goto done; }
    *data = buffer;
    *size = data_size;
    ok = 1;
done:
    fclose(f);
    return ok;
}

/* ---- VS_VERSIONINFO tree ---- */

typedef struct {
    const uint8_t *base, *end;
    const uint16_t *key;
    const uint8_t *value;
    uint32_t value_length;       /* as stored (characters for text values) */
    uint32_t value_bytes;
    int is_text;
    const uint8_t *children, *next;
} Node;

static const uint8_t *align4(const uint8_t *p, const uint8_t *origin)
{
    return origin + (((size_t)(p - origin) + 3) & ~(size_t)3);
}

static int node_parse(const uint8_t *p, const uint8_t *limit, const uint8_t *origin, Node *n)
{
    if (limit - p < 6) return 0;
    uint16_t length, value_length, type;
    memcpy(&length, p, 2); memcpy(&value_length, p + 2, 2); memcpy(&type, p + 4, 2);
    if (length < 6 || length > limit - p) return 0;
    n->base = p;
    n->end = p + length;
    n->key = (const uint16_t *)(p + 6);
    const uint8_t *q = p + 6;
    while (q + 2 <= n->end && (q[0] || q[1])) q += 2;
    q += 2;
    q = align4(q, origin);
    n->is_text = type == 1;
    n->value_length = value_length;
    n->value_bytes = n->is_text ? value_length * 2u : value_length;
    n->value = q;
    q += n->value_bytes;
    q = align4(q, origin);
    n->children = q < n->end ? q : n->end;
    n->next = align4(n->end, origin);
    return n->value <= n->end;
}

static int key_equals(const uint16_t *key, const uint16_t *name, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        uint16_t a = key[i], b = name[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return key[length] == 0;
}

int beer_version_query(const uint8_t *block, uint32_t block_size, const uint16_t *sub_block, const void **value, uint32_t *length)
{
    Node node;
    if (!block || !sub_block || !node_parse(block, block + block_size, block, &node)) return 0;
    const uint16_t *s = sub_block;
    while (*s == '\\') ++s;
    if (!*s) {
        *value = node.value;
        *length = node.value_bytes;
        return node.value_bytes != 0;
    }
    while (*s) {
        size_t n = 0;
        while (s[n] && s[n] != '\\') ++n;
        const uint8_t *child = node.children;
        int found = 0;
        Node c;
        while (child < node.end && node_parse(child, node.end, block, &c)) {
            if (key_equals(c.key, s, n)) { found = 1; break; }
            child = c.next;
        }
        if (!found) return 0;
        node = c;
        s += n;
        while (*s == '\\') ++s;
    }
    *value = node.value;
    *length = node.is_text ? node.value_length : node.value_bytes;
    return 1;
}
