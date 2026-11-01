#ifndef BEER_STEAM_INTERFACE_CONTRACT_H
#define BEER_STEAM_INTERFACE_CONTRACT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    BEER_STEAM_UTILS_SLOT_IS_API_CALL_COMPLETED = 11,
    BEER_STEAM_UTILS_SLOT_GET_API_CALL_FAILURE_REASON = 12,
    BEER_STEAM_UTILS_SLOT_GET_API_CALL_RESULT = 13,
    BEER_STEAM_UTILS_SLOT_RUN_FRAME = 14,
    BEER_STEAM_UTILS_SLOT_GET_STEAM_UI_LANGUAGE = 23,
};

static inline int beer_steam_is_offline_input_interface(const char *lower_name)
{
    return lower_name &&
           (strstr(lower_name, "steaminput") != NULL ||
            strstr(lower_name, "steamcontroller") != NULL);
}

/* The legacy steamclient C exports describe the pipe/user connection created
 * through ISteamClient. A local compatibility session is connected after
 * ConnectToGlobalUser/CreateLocalUser succeeds; official mode additionally
 * requires the signed-in provider session to remain active. */
static inline int beer_steam_legacy_session_connected(
    int official_mode, int official_session_active,
    int client_connected, uint32_t user, uint32_t pipe)
{
    return user != 0 && pipe != 0 && client_connected &&
           (!official_mode || official_session_active);
}

static inline void beer_steam_utils_install_async_slots(
    uint64_t *vtable, size_t count,
    uint64_t is_completed, uint64_t failure_reason,
    uint64_t get_result, uint64_t run_frame)
{
    if (!vtable || count <= BEER_STEAM_UTILS_SLOT_RUN_FRAME) return;
    vtable[BEER_STEAM_UTILS_SLOT_IS_API_CALL_COMPLETED] = is_completed;
    vtable[BEER_STEAM_UTILS_SLOT_GET_API_CALL_FAILURE_REASON] = failure_reason;
    vtable[BEER_STEAM_UTILS_SLOT_GET_API_CALL_RESULT] = get_result;
    vtable[BEER_STEAM_UTILS_SLOT_RUN_FRAME] = run_frame;
}

#endif
