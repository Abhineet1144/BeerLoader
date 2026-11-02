#include "vdf.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_DEPTH 64

static VdfNode *node_new(char *key, char *value)
{
    VdfNode *node = calloc(1, sizeof(*node));
    if (!node) { free(key); free(value); return NULL; }
    node->key = key;
    node->value = value;
    return node;
}

void vdf_free(VdfNode *node)
{
    while (node) {
        VdfNode *next = node->next;
        vdf_free(node->child);
        free(node->key);
        free(node->value);
        free(node);
        node = next;
    }
}

/* ---------------------------------------------------------------- text */

typedef struct {
    const char *p, *end;
} Text;

enum { TOK_END, TOK_STRING, TOK_OPEN, TOK_CLOSE, TOK_ERROR };

static void skip_space(Text *t)
{
    while (t->p < t->end) {
        char c = *t->p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { t->p++; continue; }
        if (c == '/' && t->p + 1 < t->end && t->p[1] == '/') {
            while (t->p < t->end && *t->p != '\n') t->p++;
            continue;
        }
        break;
    }
}

/* Reads one token; for strings *out receives a malloc'd copy. */
static int token(Text *t, char **out)
{
    skip_space(t);
    if (t->p >= t->end || !*t->p) return TOK_END;
    char c = *t->p;
    if (c == '{') { t->p++; return TOK_OPEN; }
    if (c == '}') { t->p++; return TOK_CLOSE; }
    size_t cap = 32, n = 0;
    char *s = malloc(cap);
    if (!s) return TOK_ERROR;
    if (c == '"') {
        t->p++;
        while (t->p < t->end && *t->p != '"') {
            char ch = *t->p++;
            if (ch == '\\' && t->p < t->end) {
                char e = *t->p++;
                ch = e == 'n' ? '\n' : e == 't' ? '\t' : e;
            }
            if (n + 2 > cap) { char *g = realloc(s, cap *= 2); if (!g) { free(s); return TOK_ERROR; } s = g; }
            s[n++] = ch;
        }
        if (t->p >= t->end) { free(s); return TOK_ERROR; }
        t->p++;
    } else {
        while (t->p < t->end && !strchr(" \t\r\n{}\"", *t->p)) {
            if (n + 2 > cap) { char *g = realloc(s, cap *= 2); if (!g) { free(s); return TOK_ERROR; } s = g; }
            s[n++] = *t->p++;
        }
    }
    s[n] = 0;
    *out = s;
    return TOK_STRING;
}

/* Skips a platform conditional such as [$WIN32] after a key or value. */
static void skip_conditional(Text *t)
{
    skip_space(t);
    if (t->p < t->end && *t->p == '[') {
        while (t->p < t->end && *t->p != ']' && *t->p != '\n') t->p++;
        if (t->p < t->end && *t->p == ']') t->p++;
    }
}

static int parse_block(Text *t, VdfNode *parent, int depth, int top)
{
    if (depth > MAX_DEPTH) return 0;
    VdfNode **tail = &parent->child;
    for (;;) {
        char *key = NULL;
        int tok = token(t, &key);
        if (tok == TOK_END) return top;
        if (tok == TOK_CLOSE) return !top;
        if (tok != TOK_STRING) return 0;
        skip_conditional(t);
        char *value = NULL;
        tok = token(t, &value);
        VdfNode *node;
        if (tok == TOK_STRING) {
            skip_conditional(t);
            node = node_new(key, value);
            if (!node) return 0;
        } else if (tok == TOK_OPEN) {
            node = node_new(key, NULL);
            if (!node) return 0;
            *tail = node;
            tail = &node->next;
            if (!parse_block(t, node, depth + 1, 0)) return 0;
            continue;
        } else {
            free(key);
            return 0;
        }
        *tail = node;
        tail = &node->next;
    }
}

VdfNode *vdf_parse_text(const char *text, size_t len)
{
    Text t = { text, text + len };
    VdfNode *root = node_new(NULL, NULL);
    if (root && !parse_block(&t, root, 0, 1)) { vdf_free(root); return NULL; }
    return root;
}

/* ---------------------------------------------------------------- binary */

enum { BIN_BLOCK = 0, BIN_STRING = 1, BIN_INT32 = 2, BIN_FLOAT = 3, BIN_POINTER = 4, BIN_WSTRING = 5,
       BIN_COLOR = 6, BIN_UINT64 = 7, BIN_END = 8, BIN_INT64 = 10, BIN_END_ALT = 11 };

typedef struct {
    const unsigned char *p, *end;
} Bin;

static char *bin_cstring(Bin *b)
{
    const unsigned char *zero = memchr(b->p, 0, (size_t)(b->end - b->p));
    if (!zero) return NULL;
    size_t n = (size_t)(zero - b->p);
    char *s = malloc(n + 1);
    if (!s) return NULL;
    memcpy(s, b->p, n);
    s[n] = 0;
    b->p = zero + 1;
    return s;
}

static int bin_fixed(Bin *b, void *out, size_t n)
{
    if ((size_t)(b->end - b->p) < n) return 0;
    memcpy(out, b->p, n);
    b->p += n;
    return 1;
}

static int parse_binary_block(Bin *b, VdfNode *parent, int depth)
{
    if (depth > MAX_DEPTH) return 0;
    VdfNode **tail = &parent->child;
    while (b->p < b->end) {
        int type = *b->p++;
        if (type == BIN_END || type == BIN_END_ALT) return 1;
        char *key = bin_cstring(b);
        if (!key) return 0;
        char number[32], *value = NULL;
        VdfNode *node;
        switch (type) {
        case BIN_BLOCK:
            node = node_new(key, NULL);
            if (!node) return 0;
            *tail = node;
            tail = &node->next;
            if (!parse_binary_block(b, node, depth + 1)) return 0;
            continue;
        case BIN_STRING:
            value = bin_cstring(b);
            break;
        case BIN_INT32: case BIN_POINTER: case BIN_COLOR: {
            int32_t v;
            if (bin_fixed(b, &v, 4)) { snprintf(number, sizeof(number), "%d", v); value = strdup(number); }
            break;
        }
        case BIN_FLOAT: {
            float v;
            if (bin_fixed(b, &v, 4)) { snprintf(number, sizeof(number), "%g", v); value = strdup(number); }
            break;
        }
        case BIN_UINT64: case BIN_INT64: {
            uint64_t v;
            if (bin_fixed(b, &v, 8)) {
                if (type == BIN_INT64) snprintf(number, sizeof(number), "%lld", (long long)v);
                else snprintf(number, sizeof(number), "%llu", (unsigned long long)v);
                value = strdup(number);
            }
            break;
        }
        default:
            break;                       /* wide strings never appear in Steam metadata */
        }
        if (!value) { free(key); return 0; }
        if (!(node = node_new(key, value))) return 0;
        *tail = node;
        tail = &node->next;
    }
    return depth == 0;
}

VdfNode *vdf_parse_binary(const unsigned char *data, size_t len)
{
    Bin b = { data, data + len };
    VdfNode *root = node_new(NULL, NULL);
    if (root && !parse_binary_block(&b, root, 0)) { vdf_free(root); return NULL; }
    return root;
}

/* ---------------------------------------------------------------- lookup */

VdfNode *vdf_child(const VdfNode *block, const char *key)
{
    for (VdfNode *n = block ? block->child : NULL; n; n = n->next)
        if (n->key && !strcasecmp(n->key, key)) return n;
    return NULL;
}

VdfNode *vdf_path(const VdfNode *block, const char *path)
{
    char part[256];
    const VdfNode *node = block;
    while (node && *path) {
        size_t n = strcspn(path, "/");
        if (n >= sizeof(part)) return NULL;
        memcpy(part, path, n);
        part[n] = 0;
        node = vdf_child(node, part);
        path += n + (path[n] == '/');
    }
    return (VdfNode *)node;
}

const char *vdf_string(const VdfNode *block, const char *path, const char *fallback)
{
    VdfNode *node = vdf_path(block, path);
    return node && node->value ? node->value : fallback;
}
