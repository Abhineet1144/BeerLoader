#ifndef BEER_WIN32_KEYBOARD_TEXT_H
#define BEER_WIN32_KEYBOARD_TEXT_H

#include <stdint.h>

/* Translate one key with Beer's advertised non-IME US keyboard layout. */
static inline uint32_t beer_win32_us_key_character(uint32_t virtual_key,
                                                    int shift,
                                                    int caps_lock)
{
    switch (virtual_key) {
    case 0x08u: return '\b';
    case 0x09u: return '\t';
    case 0x0du: return '\r';
    case 0x1bu: return 0x1bu;
    case 0x20u: return ' ';
    default: break;
    }
    if (virtual_key >= 'A' && virtual_key <= 'Z') {
        const int uppercase = !!shift ^ !!caps_lock;
        return uppercase ? virtual_key : virtual_key + ('a' - 'A');
    }

    static const uint8_t keys[] = {
        '0','1','2','3','4','5','6','7','8','9',
        0xba,0xbb,0xbc,0xbd,0xbe,0xbf,0xc0,0xdb,0xdc,0xdd,0xde
    };
    static const uint8_t unshifted[] = {
        '0','1','2','3','4','5','6','7','8','9',
        ';','=',',','-','.','/','`','[','\\',']','\''
    };
    static const uint8_t shifted[] = {
        ')','!','@','#','$','%','^','&','*','(',
        ':','+','<','_','>','?','~','{','|','}','"'
    };
    for (uint32_t i = 0; i < sizeof(keys); ++i)
        if (virtual_key == keys[i])
            return shift ? shifted[i] : unshifted[i];
    return 0;
}

static inline int beer_win32_to_unicode_us(uint32_t virtual_key,
                                           const uint8_t keyboard_state[256],
                                           uint16_t *buffer,
                                           int buffer_length)
{
    if (!keyboard_state || !buffer || buffer_length <= 0)
        return 0;
    const int shift = (keyboard_state[0x10] & 0x80u) != 0 ||
                      (keyboard_state[0xa0] & 0x80u) != 0 ||
                      (keyboard_state[0xa1] & 0x80u) != 0;
    const int caps_lock = (keyboard_state[0x14] & 1u) != 0;
    const uint32_t character = beer_win32_us_key_character(
        virtual_key, shift, caps_lock);
    if (!character) return 0;
    buffer[0] = (uint16_t)character;
    return 1;
}

#endif
