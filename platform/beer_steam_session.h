/* Connection from a game to the Beer Steam session provider (BEER_STEAM_API_MODE=official).
 *
 * Identity, ownership and tickets come from the user's signed-in Steam session through the
 * provider's local socket. Nothing is made up here: when the provider cannot answer, callers report
 * Steam's own failure (for example SteamAPI_Init returning false). */
#ifndef BEER_STEAM_SESSION_H
#define BEER_STEAM_SESSION_H

#include <stddef.h>
#include <stdint.h>

/* Opens the signed-in session for appid. This exposes identity/persona even when
 * the app is not owned; ownership and ticket APIs remain independently gated. */
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
/* Web API auth ticket (ISteamUser::GetAuthTicketForWebApi) for the given identity: registered with Steam so the
 * web service's AuthenticateUserTicket(identity) accepts it. Same return convention; cancel it like the above. */
size_t beer_steam_session_web_api_ticket(uint32_t appid, const char *identity, uint8_t *out, size_t cap,
                                         uint32_t *handle);

/* Matchmaking (MMS) relay through the signed-in session, as the Steam client sends it for the running app:
 * a request returns the reply body length (or -1), a send has no reply. games_played tells Steam the app is
 * running (cleared when this process disconnects). next_mms_event returns the next unsolicited
 * matchmaking message from Steam (lobby data, members, chat), -1 when none is waiting. */
long beer_steam_session_mms_request(uint32_t appid, uint32_t emsg, const void *body, size_t len,
                                    uint8_t *out, size_t cap);
int beer_steam_session_mms_send(uint32_t appid, uint32_t emsg, const void *body, size_t len);
int beer_steam_session_games_played(uint32_t appid, uint32_t pid);
long beer_steam_session_next_mms_event(uint32_t *emsg, uint8_t *out, size_t cap);

/* Steam networking signaling (CMsgClientP2PConnectionInfo, EMsg 5434) through the signed-in session: p2p_send
 * delivers one rendezvous blob to `dest` for the app; next_p2p_event returns the next blob Steam delivered for this
 * account (its length, or -1 when none), or with *failed set a delivery failure (4-byte close reason in out). */
int beer_steam_session_p2p_send(uint32_t appid, uint64_t dest, const void *body, size_t len);
/* Steam-issued networking certificate for this instance's public key. Private key stays in GNS.
 * Returns the signed certificate blob length, or -1 with a provider error. */
long beer_steam_session_networking_certificate(uint32_t appid, const uint8_t key[32],
                                               uint8_t *out, size_t cap, char *err, size_t errlen);
/* HTTPS-fetched Valve config, including signed CA chains and key revocations; no caller-selected URL. */
long beer_steam_session_networking_config(uint32_t appid, uint8_t *out, size_t cap, char *err, size_t errlen);
long beer_steam_session_next_p2p_event(uint64_t *src, uint64_t *dest, uint32_t *appid, int *failed,
                                       uint8_t *out, size_t cap);

/* ISteamMatchmaking::InviteUserToLobby as the Steam client does it: the matchmaking server is told and the friend
 * gets a Steam chat invite with a Join button. Returns 1 when Steam accepted the invite message. */
int beer_steam_session_invite_lobby(uint32_t appid, uint64_t lobby, uint64_t friend_id);

/* A friend's lobby invite that reached the signed-in account (provider event lobby_invite), oldest first: 1 with its
 * sender, app and lobby (name: the sender's persona name, may be empty), 0 when none is waiting. Never blocks. */
int beer_steam_session_next_invite(uint64_t *from, uint32_t *appid, uint64_t *lobby, char *name, size_t name_cap);

/* Socket path used by the provider; exposed for tests. */
int beer_steam_session_socket_path(char *out, size_t cap);

#endif
