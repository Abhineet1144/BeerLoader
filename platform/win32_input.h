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
    case 0x020b: case 0x020c: {              /* WM_XBUTTONDOWN / WM_XBUTTONUP */
        /* HIWORD(wParam) selects XBUTTON1 (1) or XBUTTON2 (2). */
        const uint32_t which = (uint32_t)(wparam >> 16) & 0xffffu;
        if (which != 1 && which != 2) return;
        vk = 0x04u + which;                  /* VK_XBUTTON1 0x05 / VK_XBUTTON2 0x06 */
        down = message == 0x020b;
        break;
    }
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

static inline int beer_dinput_can_acquire(int focused,
                                          uint32_t cooperative_flags)
{
    /* DISCL_FOREGROUND devices are unavailable while their window is not the
     * foreground window. DISCL_BACKGROUND devices remain available. */
    const uint32_t DISCL_BACKGROUND = 0x00000008u;
    return focused || (cooperative_flags & DISCL_BACKGROUND) != 0;
}

/* TranslateMessage posts WM_CHAR only for key-down messages. This deliberately
 * covers the layout-independent control keys and the common US-ASCII range;
 * WM_KEYDOWN remains available to applications that use physical controls. */
static inline uint32_t beer_win32_key_message_character(uint32_t message,
                                                         uint32_t virtual_key,
                                                         int shift,
                                                         int caps_lock)
{
    if (message != 0x0100u && message != 0x0104u)
        return 0;
    switch (virtual_key) {
    case 0x08u: return '\b';
    case 0x09u: return '\t';
    case 0x0du: return '\r';
    case 0x1bu: return 0x1bu;
    case 0x20u: return ' ';
    default: break;
    }
    if (virtual_key >= 'A' && virtual_key <= 'Z') {
        int uppercase = !!shift ^ !!caps_lock;
        return uppercase ? virtual_key : virtual_key + ('a' - 'A');
    }
    if (virtual_key >= '0' && virtual_key <= '9' && !shift)
        return virtual_key;
    return 0;
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

static inline void beer_win32_pointer_motion_delta(int position_valid,
                                                    int32_t previous_x,
                                                    int32_t previous_y,
                                                    int32_t current_x,
                                                    int32_t current_y,
                                                    int32_t *delta_x,
                                                    int32_t *delta_y)
{
    if (!delta_x || !delta_y)
        return;
    /* The first absolute event establishes the pointer origin. Treating it as
     * relative motion from zero permanently offsets software cursors. */
    *delta_x = position_valid ? current_x - previous_x : 0;
    *delta_y = position_valid ? current_y - previous_y : 0;
}

static inline int beer_win32_message_is_button_transition(uint32_t message)
{
    switch (message) {
    case 0x0201u: case 0x0202u: /* left */
    case 0x0204u: case 0x0205u: /* right */
    case 0x0207u: case 0x0208u: /* middle */
    case 0x020bu: case 0x020cu: /* X buttons */
        return 1;
    default:
        return 0;
    }
}

typedef struct {
    uint32_t type;
    uint32_t size;
    uint64_t device;
    uint64_t parameter;
} BeerRawInputHeader;

typedef struct {
    uint16_t make_code;
    uint16_t flags;
    uint16_t reserved;
    uint16_t virtual_key;
    uint32_t message;
    uint32_t extra_information;
} BeerRawKeyboard;

typedef struct {
    uint16_t flags;
    uint16_t padding;
    /* RAWMOUSE's ulButtons union is ULONG-aligned. The button halves begin at
     * offsets 4 and 6, not immediately after usFlags. */
    uint16_t button_flags;
    uint16_t button_data;
    uint32_t raw_buttons;
    int32_t last_x;
    int32_t last_y;
    uint32_t extra_information;
} BeerRawMouse;

typedef struct {
    BeerRawInputHeader header;
    union {
        BeerRawMouse mouse;
        BeerRawKeyboard keyboard;
    } data;
} BeerRawInput;

static inline BeerRawInput beer_raw_keyboard_input(uint32_t virtual_key,
                                                    uint32_t scan_code,
                                                    int down)
{
    BeerRawInput input = {0};
    input.header.type = 1; /* RIM_TYPEKEYBOARD */
    input.header.size = sizeof(BeerRawInputHeader) + sizeof(BeerRawKeyboard);
    input.header.device = 2;
    input.data.keyboard.make_code = (uint16_t)scan_code;
    input.data.keyboard.flags = down ? 0 : 1; /* RI_KEY_BREAK; RI_KEY_E0 (2) is set by the caller */
    input.data.keyboard.virtual_key = (uint16_t)virtual_key;
    input.data.keyboard.message = down ? 0x0100u : 0x0101u;
    return input;
}

static inline BeerRawInput beer_raw_mouse_input(int32_t dx, int32_t dy,
                                                 uint16_t button_flags,
                                                 uint16_t button_data)
{
    BeerRawInput input = {0};
    input.header.type = 0; /* RIM_TYPEMOUSE */
    input.header.size = sizeof(BeerRawInputHeader) + sizeof(BeerRawMouse);
    input.header.device = 1;
    input.data.mouse.button_flags = button_flags;
    input.data.mouse.button_data = button_data;
    input.data.mouse.last_x = dx;
    input.data.mouse.last_y = dy;
    return input;
}

#endif
