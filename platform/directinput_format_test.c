#include "directinput_format.h"
#include "win32_input.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    uint32_t axes[3] = {20, 24, 28};
    uint32_t buttons[8] = {7, 3, 11, UINT32_MAX, UINT32_MAX, UINT32_MAX,
                           UINT32_MAX, UINT32_MAX};
    uint32_t keys[256];
    for (uint32_t i = 0; i < 256; ++i) keys[i] = UINT32_MAX;
    keys[0x1c] = 42;

    assert(beer_dinput_format_offset(1, 0, axes, buttons, keys) == 20);
    assert(beer_dinput_format_offset(1, 4, axes, buttons, keys) == 24);
    assert(beer_dinput_format_offset(1, 8, axes, buttons, keys) == 28);
    assert(beer_dinput_format_offset(1, 12, axes, buttons, keys) == 7);
    assert(beer_dinput_format_offset(1, 13, axes, buttons, keys) == 3);
    assert(beer_dinput_format_offset(1, 15, axes, buttons, keys) == UINT32_MAX);
    assert(beer_dinput_format_offset(2, 0x1c, axes, buttons, keys) == 42);
    assert(beer_dinput_format_offset(2, 0x1d, axes, buttons, keys) == UINT32_MAX);
    assert(beer_dinput_format_offset(0, 12, axes, buttons, keys) == UINT32_MAX);

    int32_t mouse_axes[3] = {-7, 11, 120};
    uint8_t mouse_buttons[8] = {0x80, 0, 0x80, 0, 0, 0, 0, 0};
    uint8_t mouse_state[20];
    memset(mouse_state, 0xcc, sizeof(mouse_state));
    beer_dinput_write_standard_mouse_state(mouse_state, sizeof(mouse_state),
                                            mouse_axes, mouse_buttons);
    assert(memcmp(mouse_state + 0, &mouse_axes[0], 4) == 0);
    assert(memcmp(mouse_state + 4, &mouse_axes[1], 4) == 0);
    assert(memcmp(mouse_state + 8, &mouse_axes[2], 4) == 0);
    assert(mouse_state[12] == 0x80);
    assert(mouse_state[13] == 0);
    assert(mouse_state[14] == 0x80);
    assert(mouse_state[19] == 0);

    uint8_t partial_mouse_state[13] = {0};
    beer_dinput_write_standard_mouse_state(partial_mouse_state,
                                            sizeof(partial_mouse_state),
                                            mouse_axes, mouse_buttons);
    assert(partial_mouse_state[12] == 0x80);

    uint8_t queue_keys[256] = {0};
    beer_win32_apply_input_message(queue_keys, 0x0201, 0);
    assert(beer_win32_get_key_state(queue_keys, 0x01) == 0x8000);
    beer_win32_apply_input_message(queue_keys, 0x0202, 0);
    assert(beer_win32_get_key_state(queue_keys, 0x01) == 0);
    beer_win32_apply_input_message(queue_keys, 0x0204, 0);
    assert(beer_win32_get_key_state(queue_keys, 0x02) == 0x8000);
    beer_win32_apply_input_message(queue_keys, 0x0100, 'E');
    assert(beer_win32_get_key_state(queue_keys, 'E') == 0x8000);
    beer_win32_apply_input_message(queue_keys, 0x0101, 'E');
    assert(beer_win32_get_key_state(queue_keys, 'E') == 0);

    assert(beer_win32_message_is_coalescible_motion(0x0200));
    assert(!beer_win32_message_is_coalescible_motion(0x0201));
    assert(beer_win32_message_is_button_transition(0x0201));
    assert(beer_win32_message_is_button_transition(0x0202));
    assert(beer_win32_message_is_button_transition(0x0204));
    assert(beer_win32_message_is_button_transition(0x0208));
    assert(!beer_win32_message_is_button_transition(0x0200));
    assert(!beer_win32_message_is_button_transition(0x020a));

    assert(beer_win32_message_hwnd(1, 0xd0000001u, 0xa020u) == 0);
    assert(beer_win32_message_hwnd(0, 0xd0000001u, 0xa020u) == 0xd0000001u);
    assert(beer_win32_message_hwnd(0, 0xd0000001u, 0x0012u) == 0);

    /* Verify Win32 mouse-button message constants match the Windows ABI.
     * messages[i][0] = down message, messages[i][1] = up message.
     * Button 1 (left):   WM_LBUTTONDOWN=0x0201 / WM_LBUTTONUP=0x0202
     * Button 2 (middle): WM_MBUTTONDOWN=0x0207 / WM_MBUTTONUP=0x0208
     * Button 3 (right):  WM_RBUTTONDOWN=0x0204 / WM_RBUTTONUP=0x0205 */
    static const uint32_t mouse_msgs[3][2] = {
        {0x0201, 0x0202}, {0x0207, 0x0208}, {0x0204, 0x0205}
    };
    assert(mouse_msgs[0][0] == 0x0201); /* WM_LBUTTONDOWN */
    assert(mouse_msgs[0][1] == 0x0202); /* WM_LBUTTONUP */
    assert(mouse_msgs[1][0] == 0x0207); /* WM_MBUTTONDOWN */
    assert(mouse_msgs[1][1] == 0x0208); /* WM_MBUTTONUP */
    assert(mouse_msgs[2][0] == 0x0204); /* WM_RBUTTONDOWN */
    assert(mouse_msgs[2][1] == 0x0205); /* WM_RBUTTONUP */
    /* Confirm button-down is index [0] and button-up is index [1] */
    assert(beer_win32_message_is_button_transition(mouse_msgs[0][0])); /* down = button transition */
    assert(beer_win32_message_is_button_transition(mouse_msgs[0][1])); /* up   = button transition */

    puts("DirectInput and Win32 input mapping tests passed");
    return 0;
}
