#ifndef BEER_WIN32_STRING_ORDINAL_H
#define BEER_WIN32_STRING_ORDINAL_H

#include <stddef.h>
#include <stdint.h>

/* CompareStringOrdinal result values from WinNLS.h. */
enum {
    BEER_CSTR_LESS_THAN = 1,
    BEER_CSTR_EQUAL = 2,
    BEER_CSTR_GREATER_THAN = 3,
};

static inline uint16_t beer_ordinal_fold_ascii(uint16_t character)
{
    return character >= 'a' && character <= 'z'
        ? (uint16_t)(character - ('a' - 'A')) : character;
}

/* Locale-independent UTF-16 ordinal comparison. Windows' ordinal comparison
 * operates on code units rather than applying locale collation. Beer currently
 * folds the invariant ASCII range used by Win32 identifiers and paths; all
 * other UTF-16 code units retain their ordinal value. */
static inline int beer_compare_string_ordinal(const uint16_t *left, int left_count,
                                               const uint16_t *right, int right_count,
                                               int ignore_case)
{
    if (!left || !right || left_count < -1 || right_count < -1) return 0;
    if (left_count < 0) {
        left_count = 0;
        while (left[left_count]) ++left_count;
    }
    if (right_count < 0) {
        right_count = 0;
        while (right[right_count]) ++right_count;
    }

    int common = left_count < right_count ? left_count : right_count;
    for (int index = 0; index < common; ++index) {
        uint16_t a = left[index], b = right[index];
        if (ignore_case) {
            a = beer_ordinal_fold_ascii(a);
            b = beer_ordinal_fold_ascii(b);
        }
        if (a != b) return a < b ? BEER_CSTR_LESS_THAN : BEER_CSTR_GREATER_THAN;
    }
    if (left_count == right_count) return BEER_CSTR_EQUAL;
    return left_count < right_count ? BEER_CSTR_LESS_THAN : BEER_CSTR_GREATER_THAN;
}

#endif
