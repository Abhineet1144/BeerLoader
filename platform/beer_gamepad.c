#include "beer_gamepad.h"

#include <pthread.h>
#include <string.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static BeerGamepadState g_sources[BEER_PAD_SOURCES];
static int g_connected[BEER_PAD_SOURCES];
static uint32_t g_packet;

void beer_gamepad_update(int source, int connected, const BeerGamepadState *state)
{
    if (source < 0 || source >= BEER_PAD_SOURCES) return;
    pthread_mutex_lock(&g_lock);
    BeerGamepadState next;
    if (connected && state) next = *state;
    else memset(&next, 0, sizeof(next));
    if (g_connected[source] != (connected != 0) || memcmp(&g_sources[source], &next, sizeof(next))) ++g_packet;
    g_sources[source] = next;
    g_connected[source] = connected != 0;
    pthread_mutex_unlock(&g_lock);
}

static int16_t stronger(int16_t a, int16_t b)
{
    return (a < 0 ? -(int)a : a) >= (b < 0 ? -(int)b : b) ? a : b;
}

int beer_gamepad_read(BeerGamepadState *state, uint32_t *packet)
{
    BeerGamepadState merged;
    memset(&merged, 0, sizeof(merged));
    int connected = 0;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < BEER_PAD_SOURCES; ++i) {
        if (!g_connected[i]) continue;
        const BeerGamepadState *s = &g_sources[i];
        connected = 1;
        merged.buttons |= s->buttons;
        if (s->left_trigger > merged.left_trigger) merged.left_trigger = s->left_trigger;
        if (s->right_trigger > merged.right_trigger) merged.right_trigger = s->right_trigger;
        merged.lx = stronger(merged.lx, s->lx);
        merged.ly = stronger(merged.ly, s->ly);
        merged.rx = stronger(merged.rx, s->rx);
        merged.ry = stronger(merged.ry, s->ry);
    }
    if (packet) *packet = g_packet;
    pthread_mutex_unlock(&g_lock);
    if (state) *state = merged;
    return connected;
}
