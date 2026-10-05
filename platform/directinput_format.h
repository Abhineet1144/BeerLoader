#ifndef BEER_DIRECTINPUT_FORMAT_H
#define BEER_DIRECTINPUT_FORMAT_H

#include <stdint.h>
#include <string.h>

/* Write the standard DIMOUSESTATE/DIMOUSESTATE2 prefix used before the later
 * custom-format experiment. Partial caller buffers are supported defensively. */
static inline void beer_dinput_write_standard_mouse_state(
    void *state, uint32_t size, const int32_t axes[3],
    const uint8_t buttons[8])
{
    uint8_t *bytes = state;
    if (!bytes || !size) return;
    if (size >= 4) memcpy(bytes + 0, &axes[0], 4);
    if (size >= 8) memcpy(bytes + 4, &axes[1], 4);
    if (size >= 12) memcpy(bytes + 8, &axes[2], 4);
    uint32_t button_count = size > 12 ? size - 12 : 0;
    if (button_count > 8) button_count = 8;
    if (button_count) memcpy(bytes + 12, buttons, button_count);
}

/* Translate the canonical offsets used by Beer's host-event ring into the
 * offsets selected by a DirectInput device's current DIDATAFORMAT. */
static inline uint32_t beer_dinput_format_offset(
    int kind, uint32_t canonical_offset,
    const uint32_t axis_offsets[3], const uint32_t button_offsets[8],
    const uint32_t key_offsets[256])
{
    if (kind == 2)
        return canonical_offset < 256 ? key_offsets[canonical_offset] : UINT32_MAX;
    if (kind != 1)
        return UINT32_MAX;

    if (canonical_offset == 0) return axis_offsets[0];
    if (canonical_offset == 4) return axis_offsets[1];
    if (canonical_offset == 8) return axis_offsets[2];
    if (canonical_offset >= 12 && canonical_offset < 20)
        return button_offsets[canonical_offset - 12];
    return UINT32_MAX;
}

#endif
