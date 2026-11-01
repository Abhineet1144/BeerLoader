#include "win32_shell_strings.h"
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

/* Legacy SHLWAPI paths are limited to MAX_PATH, including the terminator.
 * Work in local storage so both documented in-place combinations are safe. */
static int sep(uint16_t c) { return c == '\\' || c == '/'; }
static int path_copy(uint16_t *out, const uint16_t *in)
{
    size_t n = 0;
    if (in) while (in[n]) { if (n == 259) return 0; out[n] = in[n]; ++n; }
    out[n] = 0;
    return 1;
}

uint64_t __attribute__((ms_abi)) beer_PathCanonicalizeW(uint16_t *out, const uint16_t *in)
{
    uint16_t src[260], dst[260];
    size_t i = 0, n = 0, root = 0;
    if (!out) return 0;
    if (!in || !path_copy(src, in)) { out[0] = 0; return 0; }
    if (!src[0]) { out[0] = '\\'; out[1] = 0; return 1; }
    if (src[1] == ':') {
        dst[n++] = src[i++]; dst[n++] = src[i++];
        if (sep(src[i])) { dst[n++] = '\\'; ++i; }
        root = n;
    } else if (sep(src[0])) {
        dst[n++] = '\\'; ++i; root = n;
        if (sep(src[i])) {
            dst[n++] = '\\'; ++i;
            /* UNC server/share are a root, not removable components. */
            for (int component = 0; component < 2; ++component) {
                while (src[i] && !sep(src[i])) dst[n++] = src[i++];
                if (sep(src[i])) { dst[n++] = '\\'; ++i; }
            }
            root = n;
        }
    }
    while (src[i]) {
        while (sep(src[i])) ++i;
        size_t start = i;
        while (src[i] && !sep(src[i])) ++i;
        size_t len = i - start;
        if (!len) break;
        if (len == 1 && src[start] == '.') continue;
        if (len == 2 && src[start] == '.' && src[start + 1] == '.') {
            if (n > root) {
                if (n && dst[n - 1] == '\\') --n;
                while (n > root && dst[n - 1] != '\\') --n;
                if (n > root && dst[n - 1] == '\\') --n;
            }
            continue;
        }
        if (n > root && dst[n - 1] != '\\') dst[n++] = '\\';
        if (n + len >= 260) { out[0] = 0; return 0; }
        memcpy(dst + n, src + start, len * sizeof(*dst)); n += len;
    }
    if (i && sep(src[i - 1]) && n && dst[n - 1] != '\\') {
        if (n == 259) { out[0] = 0; return 0; }
        dst[n++] = '\\';
    }
    if (n == 2 && dst[1] == ':') dst[n++] = '\\';
    dst[n] = 0;
    memcpy(out, dst, (n + 1) * sizeof(*out));
    return 1;
}

uint64_t __attribute__((ms_abi)) beer_PathCombineW(uint16_t *out, const uint16_t *dir, const uint16_t *file)
{
    uint16_t joined[260], tail[260];
    if (!out) return 0;
    if (!dir && !file) { out[0] = 0; return 0; }
    if (!path_copy(joined, dir) || !path_copy(tail, file)) { out[0] = 0; return 0; }
    size_t n = 0, m = 0;
    while (joined[n]) ++n;
    while (tail[m]) ++m;
    if (m && (tail[1] == ':' || (sep(tail[0]) && sep(tail[1])) || !n)) {
        return beer_PathCanonicalizeW(out, tail) ? (uintptr_t)out : 0;
    }
    if (m && sep(tail[0])) {
        /* Root-relative file keeps only the directory's drive or UNC root. */
        if (n >= 2 && joined[1] == ':') n = 2;
        else if (n >= 2 && sep(joined[0]) && sep(joined[1])) {
            n = 2;
            for (int c = 0; c < 2; ++c) {
                while (joined[n] && !sep(joined[n])) ++n;
                if (c == 0 && joined[n]) ++n;
            }
        } else n = 0;
    } else if (m && n && !sep(joined[n - 1]) && joined[n - 1] != ':') {
        if (n == 259) { out[0] = 0; return 0; }
        joined[n++] = '\\';
    }
    if (n + m >= 260) { out[0] = 0; return 0; }
    memcpy(joined + n, tail, (m + 1) * sizeof(*tail));
    return beer_PathCanonicalizeW(out, joined) ? (uintptr_t)out : 0;
}

/* USER32's formatter accepts integers/chars/strings, not floating point.
 * A Windows va_list is an array of 8-byte argument slots, not a SysV va_list.
 * Keep it explicit so callers of wvsprintf and wsprintf share one parser. */
static int wide_format(uint16_t *out, const uint16_t *fmt, const uint64_t *args)
{
    size_t n = 0;
    if (!out || !fmt) return 0;
#define PUT(c) do { if (n == 1023) goto full; out[n++] = (uint16_t)(c); } while (0)
    while (*fmt) {
        if (*fmt != '%') { PUT(*fmt++); continue; }
        ++fmt;
        if (*fmt == '%') { PUT(*fmt++); continue; }
        int left = 0, zero = 0, alternate = 0, width = 0, precision = -1, length = 0;
        while (*fmt == '-' || *fmt == '0' || *fmt == '#') {
            if (*fmt == '-') left = 1;
            if (*fmt == '0') zero = 1;
            if (*fmt == '#') alternate = 1;
            ++fmt;
        }
        while (*fmt >= '0' && *fmt <= '9') { width = width < 1024 ? width * 10 + *fmt - '0' : 1024; ++fmt; }
        if (*fmt == '.') {
            ++fmt; precision = 0;
            while (*fmt >= '0' && *fmt <= '9') { precision = precision < 1024 ? precision * 10 + *fmt - '0' : 1024; ++fmt; }
        }
        if (*fmt == 'h') { length = -1; ++fmt; }
        else if (*fmt == 'l' || *fmt == 'w') { length = 1; ++fmt; }
        else if (*fmt == 'I') {
            ++fmt; length = 2;
            if (fmt[0] == '6' && fmt[1] == '4') fmt += 2;
            else if (fmt[0] == '3' && fmt[1] == '2') { fmt += 2; length = 0; }
        }
        uint16_t spec = *fmt;
        if (!spec) break;
        ++fmt;
        if (spec != 's' && spec != 'S' && spec != 'c' && spec != 'C' &&
            spec != 'd' && spec != 'i' && spec != 'u' && spec != 'x' && spec != 'X' && spec != 'p') {
            PUT(spec); continue;
        }
        uint64_t value = *args++;
        uint16_t text[1024]; size_t count = 0, prefix = 0;
        if (spec == 's' || spec == 'S') {
            int narrow = length == -1 || (spec == 'S' && length != 1);
            if (value) {
                const uint16_t *w = (const uint16_t *)(uintptr_t)value;
                const unsigned char *a = (const unsigned char *)(uintptr_t)value;
                while (count < 1023 && (precision < 0 || count < (size_t)precision)) {
                    uint16_t c = narrow ? a[count] : w[count];
                    if (!c) break;
                    text[count++] = c;
                }
            }
        } else if (spec == 'c' || spec == 'C') {
            text[count++] = (length == -1 || (spec == 'C' && length != 1)) ? (uint8_t)value : (uint16_t)value;
        } else {
            char digits[32];
            int negative = 0, base = spec == 'x' || spec == 'X' || spec == 'p' ? 16 : 10;
            if (length != 2 && spec != 'p') value = (uint32_t)value;
            if (spec == 'd' || spec == 'i') {
                int64_t signed_value = length == 2 ? (int64_t)value : (int32_t)value;
                if (signed_value < 0) { negative = 1; value = 0 - (uint64_t)signed_value; }
            }
            size_t d = 0;
            do { digits[d++] = "0123456789abcdef"[value % base]; value /= base; } while (value);
            if (negative) text[count++] = '-';
            if (alternate && base == 16 && !(d == 1 && digits[0] == '0')) { text[count++] = '0'; text[count++] = spec == 'X' ? 'X' : 'x'; }
            prefix = count;
            if (spec == 'p') precision = 16;
            while (precision > (int)d && count < 1000) { text[count++] = '0'; --precision; }
            while (d) { char c = digits[--d]; if ((spec == 'X' || spec == 'p') && c >= 'a') c -= 32; text[count++] = c; }
        }
        if (!left && zero && precision < 0 && spec != 's' && spec != 'S') {
            for (size_t k = 0; k < prefix; ++k) PUT(text[k]);
            while (width > (int)count) { PUT('0'); --width; }
            for (size_t k = prefix; k < count; ++k) PUT(text[k]);
        } else {
            if (!left) while (width > (int)count) { PUT(' '); --width; }
            for (size_t k = 0; k < count; ++k) PUT(text[k]);
            if (left) while (width > (int)count) { PUT(' '); --width; }
        }
    }
full:
    out[n] = 0;
    return (int)n;
#undef PUT
}

int __attribute__((ms_abi)) beer_wvsprintfW(uint16_t *out, const uint16_t *fmt, const uint64_t *args)
{
    return wide_format(out, fmt, args);
}

int __attribute__((ms_abi)) beer_wsprintfW(uint16_t *out, const uint16_t *fmt, ...)
{
    __builtin_ms_va_list args;
    __builtin_ms_va_start(args, fmt);
    int result = wide_format(out, fmt, (const uint64_t *)args);
    __builtin_ms_va_end(args);
    return result;
}