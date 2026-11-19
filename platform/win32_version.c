#include "win32_version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint32_t va, vsize, raw, rawsize; } Section;

static int read_at(FILE *f, uint64_t offset, void *out, size_t size)
{
    return fseeko(f, (off_t)offset, SEEK_SET) == 0 && fread(out, 1, size, f) == size;
}

static int rva_to_offset(const Section *sections, unsigned count, uint32_t rva,
                         uint32_t bytes, uint64_t file_size, uint64_t *offset)
{
    for (unsigned i = 0; i < count; ++i) {
        if (rva < sections[i].va) continue;
        uint32_t delta = rva - sections[i].va;
        if (delta > sections[i].rawsize || bytes > sections[i].rawsize - delta) continue;
        uint64_t at = (uint64_t)sections[i].raw + delta;
        if (at > file_size || bytes > file_size - at) continue;
        *offset = at;
        return 1;
    }
    return 0;
}

/* Finds the entry with `id` (or the first entry when id == UINT32_MAX) in a resource directory. */
static int resource_entry(FILE *f, uint64_t root, uint32_t root_size, uint32_t directory,
                           uint32_t id, uint32_t *target, int *is_directory)
{
    uint8_t header[16];
    if (directory > root_size || sizeof(header) > root_size - directory) return 0;
    if (!read_at(f, root + directory, header, sizeof(header))) return 0;
    const unsigned named = header[12] | (header[13] << 8), ids = header[14] | (header[15] << 8);
    if (named + ids > (root_size - directory - sizeof(header)) / 8) return 0;
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

int beer_system_version_resource(uint8_t **data, uint32_t *size)
{
    static const uint16_t key[] = {
        'V','S','_','V','E','R','S','I','O','N','_','I','N','F','O',0
    };
    enum { fixed_size = 13 * 4 };
    const size_t header_and_key = 6 + sizeof(key);
    const size_t value_offset = (header_and_key + 3u) & ~(size_t)3u;
    const size_t total = value_offset + fixed_size;
    if (!data || !size || total > UINT16_MAX) return 0;

    uint8_t *block = calloc(1, total);
    if (!block) return 0;
    const uint16_t length = (uint16_t)total, value_length = fixed_size, type = 0;
    memcpy(block, &length, 2);
    memcpy(block + 2, &value_length, 2);
    memcpy(block + 4, &type, 2);
    memcpy(block + 6, key, sizeof(key));

    /* VS_FIXEDFILEINFO for the Windows 10 build Beer advertises through its
     * PEB/version APIs. Keep this metadata coherent with those process fields. */
    const uint32_t fixed[13] = {
        0xFEEF04BDu, 0x00010000u,
        0x000A0000u, 0x4A650000u, /* FileVersion 10.0.19045.0 */
        0x000A0000u, 0x4A650000u, /* ProductVersion 10.0.19045.0 */
        0x0000003Fu, 0, 0x00040004u, 0x00000002u, 0, 0, 0
    };
    memcpy(block + value_offset, fixed, sizeof(fixed));
    *data = block;
    *size = (uint32_t)total;
    return 1;
}

int beer_pe_version_resource(const char *host_path, uint8_t **data, uint32_t *size)
{
    if (!host_path || !data || !size) return 0;
    FILE *f = fopen(host_path, "rb");
    if (!f) return 0;
    int ok = 0;
    uint8_t dos[64];
    uint32_t pe = 0;
    Section sections[96];
    unsigned count = 0;
    uint8_t head[24], optional[136];
    if (fseeko(f, 0, SEEK_END)) goto done;
    off_t end = ftello(f);
    if (end < 0) goto done;
    uint64_t file_size = (uint64_t)end;
    if (!read_at(f, 0, dos, sizeof(dos)) || dos[0] != 'M' || dos[1] != 'Z') goto done;
    memcpy(&pe, dos + 0x3c, 4);
    if (!read_at(f, pe, head, sizeof(head)) || memcmp(head, "PE\0\0", 4)) goto done;
    count = head[6] | (head[7] << 8);
    const unsigned opt_size = head[20] | (head[21] << 8);
    if (!count || count > 96 || opt_size < 2 ||
        !read_at(f, (uint64_t)pe + 24, optional, 2)) goto done;
    unsigned magic = optional[0] | (optional[1] << 8);
    unsigned dirs_at = magic == 0x10b ? 96 : magic == 0x20b ? 112 : 0;
    if (!dirs_at || opt_size < dirs_at + 3 * 8 ||
        !read_at(f, (uint64_t)pe + 24, optional, dirs_at + 3 * 8)) goto done;
    uint32_t dir_count;
    memcpy(&dir_count, optional + dirs_at - 4, 4);
    if (dir_count < 3) goto done;
    const uint8_t *dirs = optional + dirs_at;
    uint32_t rsrc_rva, rsrc_size;
    memcpy(&rsrc_rva, dirs + 2 * 8, 4); memcpy(&rsrc_size, dirs + 2 * 8 + 4, 4);
    if (!rsrc_rva || !rsrc_size) goto done;
    for (unsigned i = 0; i < count; ++i) {
        uint8_t s[40];
        if (!read_at(f, (uint64_t)pe + 24 + opt_size + i * 40u, s, sizeof(s))) goto done;
        memcpy(&sections[i].vsize, s + 8, 4); memcpy(&sections[i].va, s + 12, 4);
        memcpy(&sections[i].rawsize, s + 16, 4); memcpy(&sections[i].raw, s + 20, 4);
    }
    uint64_t root;
    if (!rva_to_offset(sections, count, rsrc_rva, rsrc_size, file_size, &root)) goto done;
    uint32_t dir = 0, next;
    int is_dir;
    if (!resource_entry(f, root, rsrc_size, dir, 16 /* RT_VERSION */, &next, &is_dir) || !is_dir) goto done;
    dir = next;
    if (!resource_entry(f, root, rsrc_size, dir, UINT32_MAX, &next, &is_dir) || !is_dir) goto done;   /* first name/id (usually 1) */
    dir = next;
    if (!resource_entry(f, root, rsrc_size, dir, UINT32_MAX, &next, &is_dir) || is_dir) goto done;    /* first language */
    uint8_t leaf[16];
    if (next > rsrc_size || sizeof(leaf) > rsrc_size - next) goto done;
    if (!read_at(f, root + next, leaf, sizeof(leaf))) goto done;
    uint32_t data_rva, data_size;
    memcpy(&data_rva, leaf, 4); memcpy(&data_size, leaf + 4, 4);
    uint64_t data_offset;
    if (!data_size || data_size > (1u << 20) ||
        !rva_to_offset(sections, count, data_rva, data_size, file_size, &data_offset)) goto done;
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
    size_t key_length;
    const uint8_t *value;
    uint32_t value_length;       /* as stored (characters for text values) */
    uint32_t value_bytes;
    int is_text;
    const uint8_t *children, *next;
} Node;

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
    while ((size_t)(n->end - q) >= 2 && (q[0] || q[1])) q += 2;
    if ((size_t)(n->end - q) < 2) return 0;
    n->key_length = (size_t)(q - (p + 6)) / 2;
    q += 2;
    n->is_text = type == 1;
    n->value_length = value_length;
    n->value_bytes = n->is_text ? value_length * 2u : value_length;
    size_t padding = (4u - ((size_t)(q - origin) & 3u)) & 3u;
    /* A leaf without a value need not include trailing alignment bytes. */
    if (padding > (size_t)(n->end - q)) {
        if (n->value_bytes) return 0;
        q = n->end;
    } else q += padding;
    if (n->value_bytes > (size_t)(n->end - q)) return 0;
    n->value = q;
    q += n->value_bytes;
    padding = (4u - ((size_t)(q - origin) & 3u)) & 3u;
    if (padding > (size_t)(n->end - q)) q = n->end;
    else q += padding;
    n->children = q < n->end ? q : n->end;
    padding = (4u - ((size_t)(n->end - origin) & 3u)) & 3u;
    n->next = padding > (size_t)(limit - n->end) ? limit : n->end + padding;
    return 1;
}

static int key_equals(const Node *node, const uint16_t *name, size_t length)
{
    if (length != node->key_length) return 0;
    for (size_t i = 0; i < length; ++i) {
        uint16_t a = node->key[i], b = name[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
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
            if (key_equals(&c, s, n)) { found = 1; break; }
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
