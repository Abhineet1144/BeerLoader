/* beer_steam_mms against a fake Steam: message bodies, callback layouts, async results and events. */
#include "beer_steam_mms.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const uint64_t SELF = UINT64_C(76561199000000001), FRIEND = UINT64_C(76561199000000002);
static const uint64_t LOBBY = UINT64_C(109775243000000042);

static uint8_t last_body[4096];
static size_t last_len;
static uint32_t last_emsg;
static int set_data_requests;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

/* protobuf helpers for building replies */
static size_t varint(uint8_t *p, uint64_t v) { size_t n = 0; do { p[n] = (uint8_t)(v & 0x7f); v >>= 7; if (v) p[n] |= 0x80; n++; } while (v); return n; }
static size_t f_u(uint8_t *p, uint32_t f, uint64_t v) { size_t n = varint(p, (uint64_t)f << 3); return n + varint(p + n, v); }
static size_t f_64(uint8_t *p, uint32_t f, uint64_t v) { size_t n = varint(p, (uint64_t)f << 3 | 1); memcpy(p + n, &v, 8); return n + 8; }
static size_t f_b(uint8_t *p, uint32_t f, const void *d, size_t len)
{
    size_t n = varint(p, (uint64_t)f << 3 | 2);
    n += varint(p + n, len);
    memcpy(p + n, d, len);
    return n + len;
}
static const uint8_t KV[] = { 0, 0, 1, 'm', 'o', 'd', 'e', 0, 'c', 'o', 'o', 'p', 0, 8, 8 };

static long fake_request(uint32_t appid, uint32_t emsg, const void *body, size_t len, uint8_t *out, size_t cap)
{
    (void)cap;
    assert(appid == 480);
    pthread_mutex_lock(&lock);
    memcpy(last_body, body, len);
    last_len = len;
    last_emsg = emsg;
    if (emsg == 6609) set_data_requests++;
    pthread_mutex_unlock(&lock);
    size_t n = 0;
    switch (emsg) {
    case 6601: n += f_u(out, 1, 480); n += f_64(out + n, 2, LOBBY); n += f_u(out + n, 3, 1); break;
    case 6603: {
        uint8_t member[64];
        size_t m = f_64(member, 1, FRIEND);
        m += f_b(member + m, 2, "friend", 6);
        n += f_u(out, 1, 480); n += f_64(out + n, 2, LOBBY); n += f_u(out + n, 3, 1); n += f_u(out + n, 4, 4);
        n += f_64(out + n, 7, FRIEND); n += f_b(out + n, 8, KV, sizeof(KV)); n += f_b(out + n, 9, member, m);
        break;
    }
    case 6607: {
        uint8_t lobby[64];
        size_t m = f_64(lobby, 1, LOBBY);
        m += f_u(lobby + m, 2, 4);
        m += f_b(lobby + m, 5, KV, sizeof(KV));
        n += f_u(out, 1, 480); n += f_u(out + n, 3, 1); n += f_b(out + n, 4, lobby, m);
        break;
    }
    case 6609: n += f_u(out, 1, 480); n += f_64(out + n, 2, LOBBY); n += f_u(out + n, 3, 1); break;
    default: n += f_u(out, 3, 1); break;
    }
    return (long)n;
}
static int fake_send(uint32_t appid, uint32_t emsg, const void *body, size_t len)
{
    (void)appid;
    pthread_mutex_lock(&lock);
    memcpy(last_body, body, len);
    last_len = len;
    last_emsg = emsg;
    pthread_mutex_unlock(&lock);
    return 1;
}
static uint8_t event_body[256];
static size_t event_len;
static uint32_t event_emsg;
static long fake_event(uint32_t *emsg, uint8_t *out, size_t cap)
{
    if (!event_len || event_len > cap) return -1;
    *emsg = event_emsg;
    memcpy(out, event_body, event_len);
    long n = (long)event_len;
    event_len = 0;
    return n;
}

static void wait_call(uint64_t call)
{
    int failed = 1;
    for (int i = 0; i < 400 && !beer_mms_call_completed(call, &failed); i++) usleep(5000);
    assert(beer_mms_call_completed(call, &failed) && !failed);
}
static void expect_callback(uint32_t id, BeerMmsCallback *cb)
{
    for (int i = 0; i < 400 && !beer_mms_peek_callback(cb); i++) usleep(5000);
    assert(beer_mms_peek_callback(cb));
    if (cb->id != id) fprintf(stderr, "expected callback %u, got %u\n", id, cb->id);
    assert(cb->id == id);
    beer_mms_pop_callback();
}

int main(void)
{
    const BeerMmsTransport io = { fake_request, fake_send, fake_event };
    assert(!beer_mms_ready() && beer_mms_request_lobby_list() == 0);
    beer_mms_init(&io, 480, SELF, "me");
    assert(beer_mms_ready());

    /* CreateLobby: LobbyEnter_t callback, then SteamAPICallCompleted_t; LobbyCreated_t {eresult, lobby}. */
    uint64_t call = beer_mms_create_lobby(1, 4);
    assert(call && beer_mms_owns_call(call));
    wait_call(call);
    BeerMmsCallback cb;
    expect_callback(504, &cb);
    uint64_t v;
    memcpy(&v, cb.data, 8);
    assert(cb.size == 24 && v == LOBBY && cb.data[16] == 1);
    expect_callback(703, &cb);
    memcpy(&v, cb.data, 8);
    assert(cb.size == 16 && v == call && cb.data[8] == 0x01 && cb.data[9] == 0x02 && cb.data[12] == 16);
    uint8_t created[16];
    int failed = 1;
    assert(!beer_mms_call_result(call, created, 16, 504, &failed) && failed);   /* wrong callback id */
    assert(beer_mms_call_result(call, created, 16, 513, &failed) && !failed);
    memcpy(&v, created + 8, 8);
    assert(created[0] == 1 && v == LOBBY);
    assert(!beer_mms_call_result(call, created, 16, 513, &failed));            /* consumed */
    assert(beer_mms_owner(LOBBY) == SELF && beer_mms_member_count(LOBBY) == 1);
    assert(beer_mms_member_by_index(LOBBY, 0) == SELF && beer_mms_member_limit(LOBBY) == 4);

    /* SetLobbyData: owner-only, sent as the full metadata (KeyValues) with routing fields. */
    assert(beer_mms_set_lobby_data(LOBBY, "mode", "coop"));
    assert(beer_mms_set_lobby_data(LOBBY, "map", "lava"));
    expect_callback(505, &cb);
    pthread_mutex_lock(&lock);
    assert(last_emsg == 6609 && set_data_requests >= 1 && set_data_requests <= 2);
    assert(memmem(last_body, last_len, "\x01mode\0coop\0", 11) && memmem(last_body, last_len, "\x01map\0lava\0", 10));
    pthread_mutex_unlock(&lock);
    assert(!strcmp(beer_mms_lobby_data(LOBBY, "map"), "lava") && !strcmp(beer_mms_lobby_data(LOBBY, "x"), ""));
    char key[16], value[16];
    assert(beer_mms_lobby_data_count(LOBBY) == 2 && beer_mms_lobby_data_by_index(LOBBY, 1, key, 16, value, 16));
    assert(!strcmp(key, "map") && !strcmp(value, "lava"));
    while (beer_mms_peek_callback(&cb)) beer_mms_pop_callback();

    /* A member joins (6619): LobbyChatUpdate_t Entered with their persona remembered. */
    size_t n = 0;
    n += f_u(event_body, 1, 480); n += f_64(event_body + n, 2, LOBBY); n += f_64(event_body + n, 3, FRIEND);
    n += f_b(event_body + n, 4, "friend", 6);
    event_emsg = 6619; event_len = n;
    beer_mms_pump();
    expect_callback(506, &cb);
    uint32_t state;
    memcpy(&v, cb.data + 8, 8);
    memcpy(&state, cb.data + 24, 4);
    assert(cb.size == 32 && v == FRIEND && state == 1 && beer_mms_member_count(LOBBY) == 2);
    assert(!strcmp(beer_mms_persona(FRIEND), "friend"));

    /* Chat (6614): LobbyChatMsg_t and GetLobbyChatEntry. */
    n = 0;
    n += f_u(event_body, 1, 480); n += f_64(event_body + n, 2, LOBBY); n += f_64(event_body + n, 3, FRIEND);
    n += f_b(event_body + n, 4, "hello", 5);
    event_emsg = 6614; event_len = n;
    beer_mms_pump();
    expect_callback(507, &cb);
    uint32_t chat_id;
    memcpy(&chat_id, cb.data + 20, 4);
    uint64_t sender = 0;
    int type = 0;
    char text[16] = "";
    assert(cb.data[16] == 1 && beer_mms_chat_entry(LOBBY, (int)chat_id, &sender, text, 16, &type) == 5);
    assert(sender == FRIEND && type == 1 && !memcmp(text, "hello", 5));

    /* The member leaves (6620). */
    n = 0;
    n += f_u(event_body, 1, 480); n += f_64(event_body + n, 2, LOBBY); n += f_64(event_body + n, 3, FRIEND);
    event_emsg = 6620; event_len = n;
    beer_mms_pump();
    expect_callback(506, &cb);
    memcpy(&state, cb.data + 24, 4);
    assert(state == 2 && beer_mms_member_count(LOBBY) == 1);

    /* Chat send and leave. */
    assert(beer_mms_send_chat(LOBBY, "hi", 2));
    pthread_mutex_lock(&lock);
    assert(last_emsg == 6613 && memmem(last_body, last_len, "hi", 2));
    pthread_mutex_unlock(&lock);
    beer_mms_leave_lobby(LOBBY);
    assert(beer_mms_member_count(LOBBY) == 0);

    /* Lobby list with a filter: LobbyMatchList_t, then GetLobbyByIndex and its data. */
    beer_mms_add_filter("mode", "coop", 0, 0);
    beer_mms_set_result_count(10);
    call = beer_mms_request_lobby_list();
    wait_call(call);
    pthread_mutex_lock(&lock);
    assert(last_emsg == 6607 && memmem(last_body, last_len, "mode", 4) && memmem(last_body, last_len, "coop", 4));
    pthread_mutex_unlock(&lock);
    uint32_t count = 0;
    assert(beer_mms_call_result(call, &count, 4, 510, &failed) && count == 1);
    assert(beer_mms_lobby_by_index(0) == LOBBY && beer_mms_lobby_by_index(1) == 0);
    assert(!strcmp(beer_mms_lobby_data(LOBBY, "mode"), "coop"));

    /* Join: members and owner from the reply; LobbyEnter_t with Success. */
    while (beer_mms_peek_callback(&cb)) beer_mms_pop_callback();
    call = beer_mms_join_lobby(LOBBY);
    wait_call(call);
    expect_callback(504, &cb);
    assert(cb.data[16] == 1);
    assert(beer_mms_owner(LOBBY) == FRIEND && beer_mms_member_count(LOBBY) == 2);
    assert(!beer_mms_set_lobby_data(LOBBY, "mode", "x"));                    /* not the owner */
    beer_mms_set_member_data(LOBBY, "ready", "1");
    assert(!strcmp(beer_mms_member_data(LOBBY, SELF, "ready"), "1"));
    puts("Steam matchmaking tests passed");
    return 0;
}
