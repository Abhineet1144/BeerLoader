#include "win32_registered_message.h"

#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define BEER_REGISTERED_MESSAGE_FIRST 0xC000u
#define BEER_REGISTERED_MESSAGE_LAST  0xFFFFu
#define BEER_REGISTERED_MESSAGE_MAX_CHARS 255u
#define BEER_ERROR_INVALID_PARAMETER 87u
#define BEER_ERROR_NOT_ENOUGH_MEMORY 8u
#define BEER_ERROR_NOT_ENOUGH_QUOTA 1816u

typedef struct {
    uint16_t *name;
    uint16_t length;
} BeerRegisteredMessage;

static BeerRegisteredMessage g_messages[BEER_REGISTERED_MESSAGE_LAST -
                                         BEER_REGISTERED_MESSAGE_FIRST + 1u];
static uint32_t g_message_count;
static pthread_mutex_t g_message_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint32_t register_utf16(const uint16_t *name, size_t length, uint32_t *error)
{
    if (!name || !length || length > BEER_REGISTERED_MESSAGE_MAX_CHARS) {
        if (error) *error = BEER_ERROR_INVALID_PARAMETER;
        return 0;
    }

    pthread_mutex_lock(&g_message_mutex);
    for (uint32_t i = 0; i < g_message_count; ++i) {
        if (g_messages[i].length == length &&
            memcmp(g_messages[i].name, name, length * sizeof(*name)) == 0) {
            uint32_t result = BEER_REGISTERED_MESSAGE_FIRST + i;
            pthread_mutex_unlock(&g_message_mutex);
            return result;
        }
    }
    if (g_message_count >= sizeof(g_messages) / sizeof(*g_messages)) {
        pthread_mutex_unlock(&g_message_mutex);
        if (error) *error = BEER_ERROR_NOT_ENOUGH_QUOTA;
        return 0;
    }
    uint16_t *copy = malloc((length + 1u) * sizeof(*copy));
    if (!copy) {
        pthread_mutex_unlock(&g_message_mutex);
        if (error) *error = BEER_ERROR_NOT_ENOUGH_MEMORY;
        return 0;
    }
    memcpy(copy, name, length * sizeof(*copy));
    copy[length] = 0;
    uint32_t index = g_message_count++;
    g_messages[index].name = copy;
    g_messages[index].length = (uint16_t)length;
    pthread_mutex_unlock(&g_message_mutex);
    return BEER_REGISTERED_MESSAGE_FIRST + index;
}

uint32_t beer_register_window_message_a(const char *name, uint32_t *error)
{
    if (!name) {
        if (error) *error = BEER_ERROR_INVALID_PARAMETER;
        return 0;
    }
    uint16_t wide[BEER_REGISTERED_MESSAGE_MAX_CHARS + 1u];
    size_t length = 0;
    while (name[length] && length <= BEER_REGISTERED_MESSAGE_MAX_CHARS) {
        wide[length] = (unsigned char)name[length];
        ++length;
    }
    if (length > BEER_REGISTERED_MESSAGE_MAX_CHARS) {
        if (error) *error = BEER_ERROR_INVALID_PARAMETER;
        return 0;
    }
    return register_utf16(wide, length, error);
}

uint32_t beer_register_window_message_w(const uint16_t *name, uint32_t *error)
{
    if (!name) {
        if (error) *error = BEER_ERROR_INVALID_PARAMETER;
        return 0;
    }
    size_t length = 0;
    while (name[length] && length <= BEER_REGISTERED_MESSAGE_MAX_CHARS) ++length;
    if (length > BEER_REGISTERED_MESSAGE_MAX_CHARS) {
        if (error) *error = BEER_ERROR_INVALID_PARAMETER;
        return 0;
    }
    return register_utf16(name, length, error);
}

void beer_registered_message_reset_for_test(void)
{
    pthread_mutex_lock(&g_message_mutex);
    for (uint32_t i = 0; i < g_message_count; ++i) {
        free(g_messages[i].name);
        g_messages[i].name = NULL;
        g_messages[i].length = 0;
    }
    g_message_count = 0;
    pthread_mutex_unlock(&g_message_mutex);
}
