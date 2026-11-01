/* Steam matchmaking (ISteamMatchmaking) on top of the signed-in session's MMS relay.
 *
 * What the Steam client does for a running game: lobby requests become CMsgClientMMS* messages on its
 * connection to Steam (routed to the app), replies complete async API calls, and Steam's unsolicited
 * lobby messages (data, members joining or leaving, chat, game server) become Steamworks callbacks.
 * Lobby state is cached so the getters return stable strings, as the Steam client's do.
 *
 * Portable C: no loader, guest ABI or x86 dependency. The transport is injected so the module can be
 * tested without a provider. Callback structures use the Win64 Steamworks layout (8-byte packing). */
#ifndef BEER_STEAM_MMS_H
#define BEER_STEAM_MMS_H

#include <stddef.h>
#include <stdint.h>

/* Transport: a request returns the reply body length (or -1), a send has no reply, next_event returns the
 * next unsolicited MMS message (-1 when none). */
typedef struct {
    long (*request)(uint32_t appid, uint32_t emsg, const void *body, size_t len, uint8_t *out, size_t cap);
    int (*send)(uint32_t appid, uint32_t emsg, const void *body, size_t len);
    long (*next_event)(uint32_t *emsg, uint8_t *out, size_t cap);
} BeerMmsTransport;

/* Steamworks callback IDs used here. */
enum {
    BEER_CB_LOBBY_INVITE = 503,
    BEER_CB_LOBBY_ENTER = 504,
    BEER_CB_LOBBY_DATA_UPDATE = 505,
    BEER_CB_LOBBY_CHAT_UPDATE = 506,
    BEER_CB_LOBBY_CHAT_MSG = 507,
    BEER_CB_LOBBY_GAME_CREATED = 509,
    BEER_CB_LOBBY_MATCH_LIST = 510,
    BEER_CB_LOBBY_CREATED = 513,
    BEER_CB_API_CALL_COMPLETED = 703,
};

typedef struct {
    uint32_t id, size;
    uint8_t data[48];
} BeerMmsCallback;

/* Session setup: the app the game runs as, the user's SteamID and persona. Idempotent. */
void beer_mms_init(const BeerMmsTransport *transport, uint32_t appid, uint64_t self, const char *persona);
int beer_mms_ready(void);

/* Delivers what Steam sent since the last call (as callbacks); call from the game's callback pump. */
void beer_mms_pump(void);
/* Next queued callback, 1 if one was copied; beer_mms_pop_callback removes it. */
int beer_mms_peek_callback(BeerMmsCallback *out);
void beer_mms_pop_callback(void);
/* Async call results: 1 when the call has completed (*failed set on I/O failure). get copies the result
 * when id and size match, then forgets the call. */
int beer_mms_call_completed(uint64_t call, int *failed);
int beer_mms_call_result(uint64_t call, void *out, uint32_t size, int32_t expected_id, int *failed);
/* Callback ID and result size of a pending or completed call, 1 if the call is known. */
int beer_mms_call_info(uint64_t call, uint32_t *id, uint32_t *size);
/* Whether a call handle belongs to this module. */
int beer_mms_owns_call(uint64_t call);

/* ISteamMatchmaking. Async operations return a SteamAPICall_t (0 = k_uAPICallInvalid). */
void beer_mms_add_filter(const char *key, const char *value, int comparison, int filter_type);
void beer_mms_set_result_count(int count);
uint64_t beer_mms_request_lobby_list(void);
uint64_t beer_mms_lobby_by_index(int index);
uint64_t beer_mms_create_lobby(int type, int max_members);
uint64_t beer_mms_join_lobby(uint64_t lobby);
void beer_mms_leave_lobby(uint64_t lobby);
int beer_mms_invite(uint64_t lobby, uint64_t user);
int beer_mms_member_count(uint64_t lobby);
uint64_t beer_mms_member_by_index(uint64_t lobby, int index);
const char *beer_mms_lobby_data(uint64_t lobby, const char *key);
int beer_mms_set_lobby_data(uint64_t lobby, const char *key, const char *value);
int beer_mms_lobby_data_count(uint64_t lobby);
int beer_mms_lobby_data_by_index(uint64_t lobby, int index, char *key, int key_cap, char *value, int value_cap);
int beer_mms_delete_lobby_data(uint64_t lobby, const char *key);
const char *beer_mms_member_data(uint64_t lobby, uint64_t user, const char *key);
void beer_mms_set_member_data(uint64_t lobby, const char *key, const char *value);
int beer_mms_send_chat(uint64_t lobby, const void *data, int len);
int beer_mms_chat_entry(uint64_t lobby, int chat_id, uint64_t *user, void *data, int cap, int *type);
int beer_mms_request_lobby_data(uint64_t lobby);
void beer_mms_set_game_server(uint64_t lobby, uint32_t ip, uint16_t port, uint64_t server);
int beer_mms_game_server(uint64_t lobby, uint32_t *ip, uint16_t *port, uint64_t *server);
int beer_mms_set_member_limit(uint64_t lobby, int max_members);
int beer_mms_member_limit(uint64_t lobby);
int beer_mms_set_type(uint64_t lobby, int type);
int beer_mms_set_joinable(uint64_t lobby, int joinable);
uint64_t beer_mms_owner(uint64_t lobby);
int beer_mms_set_owner(uint64_t lobby, uint64_t owner);

/* Persona name of a user seen in a lobby (members carry their names), NULL when unknown. */
const char *beer_mms_persona(uint64_t user);

#endif
