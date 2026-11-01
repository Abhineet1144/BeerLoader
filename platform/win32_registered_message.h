#ifndef BEER_WIN32_REGISTERED_MESSAGE_H
#define BEER_WIN32_REGISTERED_MESSAGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Windows allocates registered-window-message identifiers from the atom range
 * 0xC000..0xFFFF. A and W registrations share one case-sensitive namespace. */
uint32_t beer_register_window_message_a(const char *name, uint32_t *error);
uint32_t beer_register_window_message_w(const uint16_t *name, uint32_t *error);

/* Test-only reset; call only when no registration is running concurrently. */
void beer_registered_message_reset_for_test(void);

#ifdef __cplusplus
}
#endif

#endif
