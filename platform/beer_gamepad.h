#ifndef BEER_GAMEPAD_H
#define BEER_GAMEPAD_H

#include <stdint.h>

/* One virtual Xbox controller (XInput user 0), fed by any number of input sources: the Android on-screen pad, a
 * physical gamepad, ... Sources are merged (buttons ORed, the stronger trigger and stick deflection wins), so they can
 * be used together. The controller reads as connected while at least one source is. */

/* XINPUT_GAMEPAD button bits */
enum {
    BEER_PAD_DPAD_UP = 0x0001, BEER_PAD_DPAD_DOWN = 0x0002, BEER_PAD_DPAD_LEFT = 0x0004, BEER_PAD_DPAD_RIGHT = 0x0008,
    BEER_PAD_START = 0x0010, BEER_PAD_BACK = 0x0020, BEER_PAD_LEFT_THUMB = 0x0040, BEER_PAD_RIGHT_THUMB = 0x0080,
    BEER_PAD_LEFT_SHOULDER = 0x0100, BEER_PAD_RIGHT_SHOULDER = 0x0200,
    BEER_PAD_A = 0x1000, BEER_PAD_B = 0x2000, BEER_PAD_X = 0x4000, BEER_PAD_Y = 0x8000,
};

typedef struct {
    uint16_t buttons;
    uint8_t left_trigger, right_trigger;
    int16_t lx, ly, rx, ry;   /* XInput convention: +y is up */
} BeerGamepadState;

enum { BEER_PAD_SOURCE_ONSCREEN = 0, BEER_PAD_SOURCE_PHYSICAL = 1, BEER_PAD_SOURCES = 4 };

/* A source reports its whole state; `connected` = 0 removes it. */
void beer_gamepad_update(int source, int connected, const BeerGamepadState *state);

/* The merged controller. Returns 1 when connected; *packet changes whenever the state changed (XInput dwPacketNumber). */
int beer_gamepad_read(BeerGamepadState *state, uint32_t *packet);

#endif
