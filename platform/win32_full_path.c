#include "win32_full_path.h"

#include <string.h>

static int is_sep(char c) { return c == '\\' || c == '/'; }
static int is_drive(const char *p)
{
    return ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':';
}
static char upper(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c; }

/* Splits an absolute path into its root ("C:" or "\\server\share") and the rest. Returns 0 when the path has
 * no root of its own. */
static int split_root(const char *path, char *root, size_t root_cap, const char **rest)
{
    if (is_drive(path)) {
        if (root_cap < 3) return 0;
        root[0] = path[0]; root[1] = ':'; root[2] = 0;
        *rest = path + 2;
        return 1;
    }
    if (is_sep(path[0]) && is_sep(path[1])) {
        const char *p = path + 2;
        size_t n = 0;
        if (root_cap < 3) return 0;
        root[n++] = '\\'; root[n++] = '\\';
        for (int part = 0; part < 2 && *p; ++part) {
            if (part) { if (n + 1 >= root_cap) return 0; root[n++] = '\\'; }
            while (*p && !is_sep(*p)) { if (n + 1 >= root_cap) return 0; root[n++] = *p++; }
            while (part == 0 && is_sep(*p)) ++p;
        }
        root[n] = 0;
        *rest = p;
        return 1;
    }
    return 0;
}

static int append(char *buffer, size_t cap, size_t *used, const char *text, size_t length)
{
    if (*used + length + 1 > cap) return 0;
    memcpy(buffer + *used, text, length);
    *used += length;
    buffer[*used] = 0;
    return 1;
}

size_t beer_win32_full_path(const char *path, const char *current_directory, char *out, size_t cap)
{
    if (!path || !*path || !out || !cap) return 0;
    const char *cwd = current_directory ? current_directory : "";

    /* Verbatim and device paths are not normalized. */
    if (is_sep(path[0]) && is_sep(path[1]) && (path[2] == '?' || path[2] == '.') && is_sep(path[3])) {
        size_t length = strlen(path);
        if (length + 1 > cap) return 0;
        memcpy(out, path, length + 1);
        return length;
    }

    char root[512] = "", rest[8192] = "";
    size_t rest_used = 0;
    const char *tail = NULL, *cwd_tail = "";
    char cwd_root[512] = "";
    if (!split_root(cwd, cwd_root, sizeof(cwd_root), &cwd_tail)) cwd_tail = cwd;

    if (is_sep(path[0]) && is_sep(path[1])) {                       /* \\server\share\x */
        if (!split_root(path, root, sizeof(root), &tail)) return 0;
        if (!append(rest, sizeof(rest), &rest_used, tail, strlen(tail))) return 0;
    } else if (is_drive(path) && is_sep(path[2])) {                 /* C:\x */
        root[0] = path[0]; root[1] = ':'; root[2] = 0;
        if (!append(rest, sizeof(rest), &rest_used, path + 2, strlen(path + 2))) return 0;
    } else if (is_drive(path)) {                                    /* C:rel */
        root[0] = path[0]; root[1] = ':'; root[2] = 0;
        if (is_drive(cwd_root) && upper(cwd_root[0]) == upper(path[0])) {
            root[0] = cwd_root[0];
            if (!append(rest, sizeof(rest), &rest_used, cwd_tail, strlen(cwd_tail))) return 0;
        }
        if (!append(rest, sizeof(rest), &rest_used, "\\", 1) ||
            !append(rest, sizeof(rest), &rest_used, path + 2, strlen(path + 2))) return 0;
    } else if (is_sep(path[0])) {                                   /* \x: on the current drive */
        memcpy(root, cwd_root, sizeof(root));
        if (!append(rest, sizeof(rest), &rest_used, path, strlen(path))) return 0;
    } else {                                                        /* rel */
        memcpy(root, cwd_root, sizeof(root));
        if (!append(rest, sizeof(rest), &rest_used, cwd_tail, strlen(cwd_tail)) ||
            !append(rest, sizeof(rest), &rest_used, "\\", 1) ||
            !append(rest, sizeof(rest), &rest_used, path, strlen(path))) return 0;
    }

    /* Collapse separators, "." and ".." (never above the root). */
    size_t starts[512], count = 0, n = 0;
    if (!append(out, cap, &n, root, strlen(root))) return 0;
    const size_t base = n;
    for (size_t i = 0; i < rest_used;) {
        while (i < rest_used && is_sep(rest[i])) ++i;
        const size_t start = i;
        while (i < rest_used && !is_sep(rest[i])) ++i;
        const size_t length = i - start;
        if (!length || (length == 1 && rest[start] == '.')) continue;
        if (length == 2 && rest[start] == '.' && rest[start + 1] == '.') {
            if (count) n = starts[--count];
            out[n] = 0;
            continue;
        }
        if (count < sizeof(starts) / sizeof(*starts)) starts[count++] = n;
        if (!append(out, cap, &n, "\\", 1) || !append(out, cap, &n, rest + start, length)) return 0;
    }
    const size_t path_length = strlen(path);
    const int trailing = is_sep(path[path_length - 1]);
    if (n == base) {
        /* A drive root keeps its separator ("C:\"); a bare share stays "\\server\share" unless one was given. */
        if ((is_drive(root) || trailing) && !append(out, cap, &n, "\\", 1)) return 0;
    } else if (trailing && !append(out, cap, &n, "\\", 1)) {
        return 0;
    }
    return n;
}
