/* Valve KeyValues ("VDF") in its text form (appinfo) and binary form (package info).
 * Both parse into the same tree; binary numbers become decimal strings. */
#ifndef BEER_STEAM_VDF_H
#define BEER_STEAM_VDF_H

#include <stddef.h>

typedef struct VdfNode {
    char *key;
    char *value;                 /* NULL for a block */
    struct VdfNode *child;       /* first child of a block */
    struct VdfNode *next;        /* next sibling */
} VdfNode;

/* Return the root block (its children are the top-level entries), or NULL when malformed. */
VdfNode *vdf_parse_text(const char *text, size_t len);
VdfNode *vdf_parse_binary(const unsigned char *data, size_t len);
void vdf_free(VdfNode *node);

/* Child by key, case-insensitively, as Valve's own parser treats keys. */
VdfNode *vdf_child(const VdfNode *block, const char *key);
/* Follows a '/'-separated path from a block; NULL when any step is missing. */
VdfNode *vdf_path(const VdfNode *block, const char *path);
/* String at a path, or fallback when missing or not a value. */
const char *vdf_string(const VdfNode *block, const char *path, const char *fallback);

#endif
