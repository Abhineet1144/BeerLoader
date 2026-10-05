#ifndef BEER_WIN32_INPUT_H
#define BEER_WIN32_INPUT_H

#include <stdint.h>

/* Update the key state associated with a Win32 thread message queue. Windows
 * advances this state as keyboard/mouse messages are removed, independently
 * of the newer physical state returned by GetAsyncKeyState. */
static inline void beer_win32_apply_input_message(uint8_t keys[256],
                                                   uint32_t message,
                                                   uint64_t wparam)
{
    uint32_t vk = 0;
    int down = 0;

    switch (message) {
    case 0x0100: /* WM_KEYDOWN */
    case 0x0104: /* WM_SYSKEYDOWN */
        vk = (uint32_t)wparam;
        down = 1;
        break;
    case 0x0101: /* WM_KEYUP */
    case 0x0105: /* WM_SYSKEYUP */
        vk = (uint32_t)wparam;
        break;
    case 0x0201: vk = 0x01; down = 1; break; /* WM_LBUTTONDOWN / VK_LBUTTON */
    case 0x0202: vk = 0x01; break;           /* WM_LBUTTONUP */
    case 0x0204: vk = 0x02; down = 1; break; /* WM_RBUTTONDOWN / VK_RBUTTON */
    case 0x0205: vk = 0x02; break;           /* WM_RBUTTONUP */
    case 0x0207: vk = 0x04; down = 1; break; /* WM_MBUTTONDOWN / VK_MBUTTON */
    case 0x0208: vk = 0x04; break;           /* WM_MBUTTONUP */
    default: return;
    }

    if (vk < 256)
        keys[vk] = down ? 0x80u : 0u;
}

static inline uint16_t beer_win32_get_key_state(const uint8_t keys[256],
                                                 int virtual_key)
{
    if (virtual_key < 0 || virtual_key >= 256)
        return 0;
    return (keys[virtual_key] & 0x80u) ? 0x8000u : 0u;
}

static inline int beer_win32_message_is_coalescible_motion(uint32_t message)
{
    return message == 0x0200u; /* WM_MOUSEMOVE */
}

/* PostThreadMessage messages are queue-only records and therefore carry no
 * window handle. Keep this policy explicit so they cannot accidentally be
 * dispatched through the process window procedure. */
static inline uint64_t beer_win32_message_hwnd(int is_thread_message,
                                               uint64_t window_handle,
                                               uint32_t message)
{
    return (is_thread_message || message == 0x0012u) ? 0 : window_handle;
}

static inline int beer_win32_message_is_button_transition(uint32_t message)
{
    switch (message) {
    case 0x0201u: case 0x0202u: /* left */
    case 0x0204u: case 0x0205u: /* right */
    case 0x0207u: case 0x0208u: /* middle */
        return 1;
    default:
        return 0;
    }
}

#endif
