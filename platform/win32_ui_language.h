#ifndef BEER_WIN32_UI_LANGUAGE_H
#define BEER_WIN32_UI_LANGUAGE_H

#include <stdint.h>
#include <stddef.h>

/* Beer's current declared UI locale, shared by the default-name and MUI
 * queries. This is the existing compatibility locale, not host locale data
 * or a language list extracted from a reference runtime. */
static const uint16_t beer_ui_locale_name[] = {'e', 'n', '-', 'U', 'S', 0};

static inline int beer_user_default_locale_name(uint16_t *buffer, int capacity,
                                                uint32_t *error)
{
    const int needed = sizeof(beer_ui_locale_name) / sizeof(beer_ui_locale_name[0]);
    if (!buffer || capacity <= 0) { *error = 87; return 0; }
    if (capacity < needed) { *error = 122; return 0; }
    for (int i = 0; i < needed; ++i) buffer[i] = beer_ui_locale_name[i];
    return needed;
}

static inline int beer_user_preferred_ui_languages(uint32_t flags,
    uint32_t *count, uint16_t *buffer, uint32_t *capacity, uint32_t *error)
{
    /* MUI_LANGUAGE_ID and MUI_LANGUAGE_NAME are mutually exclusive.
     * Flags 0 selects the documented default, MUI_LANGUAGE_NAME. */
    static const uint16_t id[] = {'0', '4', '0', '9', 0, 0};
    if (!count || !capacity || (flags & ~UINT32_C(0x0c)) || flags == 0x0c) {
        *error = 87;
        return 0;
    }
    uint32_t available = *capacity;
    const uint16_t *source = flags == 4 ? id : beer_ui_locale_name;
    uint32_t needed = flags == 4 ? 6 : 7;
    *capacity = needed;
    *count = 1;
    if (!buffer && !available) return 1;
    if (!buffer || available < needed) { *error = 122; return 0; }
    for (uint32_t i = 0; i + 1 < needed; ++i) buffer[i] = source[i];
    buffer[needed - 1] = 0;
    return 1;
}

#endif