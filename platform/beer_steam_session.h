/* Connection from a game to the Beer Steam session provider (BEER_STEAM_API_MODE=official).
 *
 * Identity, ownership and tickets come from the user's signed-in Steam session through the
 * provider's local socket. Nothing is made up here: when the provider cannot answer, callers report
 * Steam's own failure (for example SteamAPI_Init returning false). */
#ifndef BEER_STEAM_SESSION_H
#define BEER_STEAM_SESSION_H

#include <stddef.h>
#include <stdint.h>

/* Opens the session for appid. Returns 1 when the provider confirmed the signed-in account owns it. */
int beer_steam_session_open(uint32_t appid, char *err, size_t errlen);
int beer_steam_session_active(void);
uint64_t beer_steam_session_steamid(void);
const char *beer_steam_session_persona(void);
/* Whether the account owns appid (the app itself, DLC, or anything else in its library). */
int beer_steam_session_owns(uint32_t appid);

/* Steam-signed app ownership ticket. Returns its length, 0 on failure. */
size_t beer_steam_session_app_ticket(uint32_t appid, uint8_t *out, size_t cap);
/* Serialized EncryptedAppTicket from Steam for the given user data. Returns its length, 0 on failure. */
size_t beer_steam_session_encrypted_ticket(uint32_t appid, const void *userdata, size_t userdata_len,
                                           uint8_t *out, size_t cap);

/* Auth session ticket for game servers and peers (ISteamUser::GetAuthSessionTicket). Returns its
 * length and the provider's ticket handle, 0 on failure. */
size_t beer_steam_session_auth_ticket(uint32_t appid, uint8_t *out, size_t cap, uint32_t *handle);
void beer_steam_session_cancel_auth_ticket(uint32_t appid, uint32_t handle);

/* Socket path used by the provider; exposed for tests. */
int beer_steam_session_socket_path(char *out, size_t cap);

#endif
