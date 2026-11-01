/* ISteamMatchmaking for official-mode games, backed by platform/beer_steam_mms.c, and the Steam
 * client's callback channel for it (Steam_BGetCallback / Steam_FreeLastCallback / Steam_GetAPICallResult,
 * which Valve's steam_api64 pumps from SteamAPI_RunCallbacks, plus Beer's own SteamAPI facade).
 *
 * MSVC Win64 member functions return a CSteamID through a hidden pointer after `this`; CSteamID arguments
 * arrive as 64-bit integers. Included once by loader.c. */

static _Atomic(int) g_steam_mms_started;

/* Starts matchmaking for the signed-in session and tells Steam the app is running, as the Steam client does
 * when a game initializes Steamworks. */
static int steam_mms_start(void)
{
    if (atomic_load(&g_steam_mms_started)) return beer_mms_ready();
    if (!g_steam_api_official || !steam_official_session()) return 0;
    if (atomic_exchange(&g_steam_mms_started, 1)) return beer_mms_ready();
    static const BeerMmsTransport io = {
        beer_steam_session_mms_request, beer_steam_session_mms_send, beer_steam_session_next_mms_event
    };
    beer_mms_init(&io, g_steam_own_appid, beer_steam_session_steamid(), beer_steam_session_persona());
    beer_steam_session_games_played(g_steam_own_appid, (uint32_t)getpid());
    fprintf(stderr, "[STEAM] matchmaking ready for app %u (running as %llu)\n", g_steam_own_appid,
            (unsigned long long)beer_steam_session_steamid());
    return beer_mms_ready();
}

typedef u64 SteamMs;
#define MMS_FN static u64 __attribute__((ms_abi))

MMS_FN mm_zero(void) { return 0; }
MMS_FN mm_request_lobby_list(u64 self) { (void)self; return beer_mms_request_lobby_list(); }
MMS_FN mm_string_filter(u64 self, const char *key, const char *value, s32 comparison)
    { (void)self; beer_mms_add_filter(key, value, comparison, 0); return 0; }
MMS_FN mm_numerical_filter(u64 self, const char *key, s32 value, s32 comparison)
{
    (void)self;
    char text[16];
    snprintf(text, sizeof(text), "%d", value);
    beer_mms_add_filter(key, text, comparison, 1);
    return 0;
}
MMS_FN mm_near_filter(u64 self, const char *key, s32 value)
{
    (void)self;
    char text[16];
    snprintf(text, sizeof(text), "%d", value);
    beer_mms_add_filter(key, text, 0, 3);
    return 0;
}
MMS_FN mm_slots_filter(u64 self, s32 slots)
{
    (void)self;
    char text[16];
    snprintf(text, sizeof(text), "%d", slots);
    beer_mms_add_filter("", text, 0, 2);
    return 0;
}
MMS_FN mm_distance_filter(u64 self, s32 filter)
{
    (void)self;
    char text[16];
    snprintf(text, sizeof(text), "%d", filter);
    beer_mms_add_filter("", text, 0, 4);
    return 0;
}
MMS_FN mm_result_count(u64 self, s32 count) { (void)self; beer_mms_set_result_count(count); return 0; }
MMS_FN mm_lobby_by_index(u64 self, u64 *ret, s32 index)
    { (void)self; if (ret) *ret = beer_mms_lobby_by_index(index); return (u64)(uintptr_t)ret; }
MMS_FN mm_create_lobby(u64 self, s32 type, s32 max) { (void)self; return beer_mms_create_lobby(type, max); }
MMS_FN mm_join_lobby(u64 self, u64 lobby) { (void)self; return beer_mms_join_lobby(lobby); }
MMS_FN mm_leave_lobby(u64 self, u64 lobby) { (void)self; beer_mms_leave_lobby(lobby); return 0; }
MMS_FN mm_invite(u64 self, u64 lobby, u64 user) { (void)self; return (u64)beer_mms_invite(lobby, user); }
MMS_FN mm_member_count(u64 self, u64 lobby) { (void)self; return (u64)(s64)beer_mms_member_count(lobby); }
MMS_FN mm_member_by_index(u64 self, u64 *ret, u64 lobby, s32 index)
    { (void)self; if (ret) *ret = beer_mms_member_by_index(lobby, index); return (u64)(uintptr_t)ret; }
MMS_FN mm_lobby_data(u64 self, u64 lobby, const char *key)
    { (void)self; return (u64)(uintptr_t)beer_mms_lobby_data(lobby, key); }
MMS_FN mm_set_lobby_data(u64 self, u64 lobby, const char *key, const char *value)
    { (void)self; return (u64)beer_mms_set_lobby_data(lobby, key, value); }
MMS_FN mm_lobby_data_count(u64 self, u64 lobby) { (void)self; return (u64)(s64)beer_mms_lobby_data_count(lobby); }
MMS_FN mm_lobby_data_by_index(u64 self, u64 lobby, s32 index, char *key, s32 key_cap, char *value, s32 value_cap)
    { (void)self; return (u64)beer_mms_lobby_data_by_index(lobby, index, key, key_cap, value, value_cap); }
MMS_FN mm_delete_lobby_data(u64 self, u64 lobby, const char *key)
    { (void)self; return (u64)beer_mms_delete_lobby_data(lobby, key); }
MMS_FN mm_member_data(u64 self, u64 lobby, u64 user, const char *key)
    { (void)self; return (u64)(uintptr_t)beer_mms_member_data(lobby, user, key); }
MMS_FN mm_set_member_data(u64 self, u64 lobby, const char *key, const char *value)
    { (void)self; beer_mms_set_member_data(lobby, key, value); return 0; }
MMS_FN mm_send_chat(u64 self, u64 lobby, const void *data, s32 len)
    { (void)self; return (u64)beer_mms_send_chat(lobby, data, len); }
MMS_FN mm_chat_entry(u64 self, u64 lobby, s32 chat_id, u64 *user, void *data, s32 cap, s32 *type)
{
    (void)self;
    int entry_type = 0;
    int len = beer_mms_chat_entry(lobby, chat_id, user, data, cap, &entry_type);
    if (type) *type = entry_type;
    return (u64)(s64)len;
}
MMS_FN mm_request_lobby_data(u64 self, u64 lobby) { (void)self; return (u64)beer_mms_request_lobby_data(lobby); }
MMS_FN mm_set_game_server(u64 self, u64 lobby, u32 ip, u16 port, u64 server)
    { (void)self; beer_mms_set_game_server(lobby, ip, port, server); return 0; }
MMS_FN mm_game_server(u64 self, u64 lobby, u32 *ip, u16 *port, u64 *server)
    { (void)self; return (u64)beer_mms_game_server(lobby, ip, port, server); }
MMS_FN mm_set_member_limit(u64 self, u64 lobby, s32 max) { (void)self; return (u64)beer_mms_set_member_limit(lobby, max); }
MMS_FN mm_member_limit(u64 self, u64 lobby) { (void)self; return (u64)(s64)beer_mms_member_limit(lobby); }
MMS_FN mm_set_type(u64 self, u64 lobby, s32 type) { (void)self; return (u64)beer_mms_set_type(lobby, type); }
MMS_FN mm_set_joinable(u64 self, u64 lobby, u32 joinable)
    { (void)self; return (u64)beer_mms_set_joinable(lobby, (joinable & 0xff) != 0); }
MMS_FN mm_owner(u64 self, u64 *ret, u64 lobby)
    { (void)self; if (ret) *ret = beer_mms_owner(lobby); return (u64)(uintptr_t)ret; }
MMS_FN mm_set_owner(u64 self, u64 lobby, u64 owner) { (void)self; return (u64)beer_mms_set_owner(lobby, owner); }

/* ISteamMatchmaking009 (Steamworks SDK 1.38 and later) in vtable order. */
static u64 steam_matchmaking_interface(void)
{
    static u64 vtable[38];
    static u64 object;
    if (!object) {
        for (size_t i = 0; i < sizeof(vtable) / sizeof(*vtable); ++i) vtable[i] = (u64)(uintptr_t)mm_zero;
        /* 0..3 favorites: none (GetFavoriteGameCount 0, GetFavoriteGame/Remove false, Add 0) */
        vtable[4] = (u64)(uintptr_t)mm_request_lobby_list;
        vtable[5] = (u64)(uintptr_t)mm_string_filter;
        vtable[6] = (u64)(uintptr_t)mm_numerical_filter;
        vtable[7] = (u64)(uintptr_t)mm_near_filter;
        vtable[8] = (u64)(uintptr_t)mm_slots_filter;
        vtable[9] = (u64)(uintptr_t)mm_distance_filter;
        vtable[10] = (u64)(uintptr_t)mm_result_count;
        /* 11 AddRequestLobbyListCompatibleMembersFilter: no effect */
        vtable[12] = (u64)(uintptr_t)mm_lobby_by_index;
        vtable[13] = (u64)(uintptr_t)mm_create_lobby;
        vtable[14] = (u64)(uintptr_t)mm_join_lobby;
        vtable[15] = (u64)(uintptr_t)mm_leave_lobby;
        vtable[16] = (u64)(uintptr_t)mm_invite;
        vtable[17] = (u64)(uintptr_t)mm_member_count;
        vtable[18] = (u64)(uintptr_t)mm_member_by_index;
        vtable[19] = (u64)(uintptr_t)mm_lobby_data;
        vtable[20] = (u64)(uintptr_t)mm_set_lobby_data;
        vtable[21] = (u64)(uintptr_t)mm_lobby_data_count;
        vtable[22] = (u64)(uintptr_t)mm_lobby_data_by_index;
        vtable[23] = (u64)(uintptr_t)mm_delete_lobby_data;
        vtable[24] = (u64)(uintptr_t)mm_member_data;
        vtable[25] = (u64)(uintptr_t)mm_set_member_data;
        vtable[26] = (u64)(uintptr_t)mm_send_chat;
        vtable[27] = (u64)(uintptr_t)mm_chat_entry;
        vtable[28] = (u64)(uintptr_t)mm_request_lobby_data;
        vtable[29] = (u64)(uintptr_t)mm_set_game_server;
        vtable[30] = (u64)(uintptr_t)mm_game_server;
        vtable[31] = (u64)(uintptr_t)mm_set_member_limit;
        vtable[32] = (u64)(uintptr_t)mm_member_limit;
        vtable[33] = (u64)(uintptr_t)mm_set_type;
        vtable[34] = (u64)(uintptr_t)mm_set_joinable;
        vtable[35] = (u64)(uintptr_t)mm_owner;
        vtable[36] = (u64)(uintptr_t)mm_set_owner;
        /* 37 SetLinkedLobby: false */
        object = (u64)(uintptr_t)vtable;
    }
    return (u64)(uintptr_t)&object;
}

/* ISteamFriends::GetFriendPersonaName: names of users met in lobbies; "[unknown]" otherwise, as Steam. */
MMS_FN steam_friends_friend_persona_name(u64 self, u64 user)
{
    (void)self;
    if (g_steam_api_official && user == beer_steam_session_steamid()) return (u64)(uintptr_t)beer_steam_session_persona();
    const char *name = beer_mms_persona(user);
    return (u64)(uintptr_t)(name ? name : "[unknown]");
}

/* ---- callback channel used by Valve's steam_api64 (CallbackMsg_t: user, id, param, size) ---- */
static __thread u8 t_steam_callback_param[64];

static int steam_client_next_callback(void *message)
{
    if (!message || !atomic_load(&g_steam_mms_started)) return 0;
    beer_mms_pump();
    BeerMmsCallback cb;
    if (!beer_mms_peek_callback(&cb)) return 0;
    memcpy(t_steam_callback_param, cb.data, cb.size);
    u8 *msg = message;
    const s32 user = 1, id = (s32)cb.id, size = (s32)cb.size;
    const u64 param = (u64)(uintptr_t)t_steam_callback_param;
    memcpy(msg, &user, 4);                                 /* m_hSteamUser */
    memcpy(msg + 4, &id, 4);                               /* m_iCallback */
    memcpy(msg + 8, &param, 8);                            /* m_pubParam */
    memcpy(msg + 16, &size, 4);                            /* m_cubParam */
    return 1;
}

/* ---- Beer's SteamAPI facade: broadcast callbacks and registered call results ---- */
#define STEAM_MMS_RESULTS 64
static struct { u64 callback, call; } g_steam_mms_results[STEAM_MMS_RESULTS];
static pthread_mutex_t g_steam_mms_result_lock = PTHREAD_MUTEX_INITIALIZER;
static void steam_callback_post(u32 id, const void *data, u32 size);

static int steam_mms_register_call_result(u64 callback, u64 call)
{
    if (!beer_mms_owns_call(call)) return 0;
    pthread_mutex_lock(&g_steam_mms_result_lock);
    for (int i = 0; i < STEAM_MMS_RESULTS; ++i)
        if (!g_steam_mms_results[i].call) { g_steam_mms_results[i].callback = callback; g_steam_mms_results[i].call = call; break; }
    pthread_mutex_unlock(&g_steam_mms_result_lock);
    return 1;
}

static int steam_mms_unregister_call_result(u64 call)
{
    if (!beer_mms_owns_call(call)) return 0;
    pthread_mutex_lock(&g_steam_mms_result_lock);
    for (int i = 0; i < STEAM_MMS_RESULTS; ++i)
        if (g_steam_mms_results[i].call == call) g_steam_mms_results[i].call = 0;
    pthread_mutex_unlock(&g_steam_mms_result_lock);
    return 1;
}

/* From SteamAPI_RunCallbacks: queue broadcast callbacks for CCallback listeners and run CCallResults. */
static void steam_mms_facade_pump(void)
{
    if (!atomic_load(&g_steam_mms_started)) return;
    beer_mms_pump();
    BeerMmsCallback cb;
    while (beer_mms_peek_callback(&cb)) {
        beer_mms_pop_callback();
        if (cb.id != BEER_CB_API_CALL_COMPLETED) steam_callback_post(cb.id, cb.data, cb.size);
    }
    for (int i = 0; i < STEAM_MMS_RESULTS; ++i) {
        pthread_mutex_lock(&g_steam_mms_result_lock);
        u64 callback = g_steam_mms_results[i].callback, call = g_steam_mms_results[i].call;
        pthread_mutex_unlock(&g_steam_mms_result_lock);
        int failed = 0;
        u32 id = 0, size = 0;
        if (!call || !beer_mms_call_completed(call, &failed) || !beer_mms_call_info(call, &id, &size)) continue;
        u8 result[64];
        beer_mms_call_result(call, result, size, (s32)id, &failed);
        pthread_mutex_lock(&g_steam_mms_result_lock);
        g_steam_mms_results[i].call = 0;
        pthread_mutex_unlock(&g_steam_mms_result_lock);
        u64 *object = (u64 *)(uintptr_t)callback;
        u64 *vtable = object ? (u64 *)(uintptr_t)object[0] : NULL;
        if (!vtable || !vtable[1]) continue;
        typedef void __attribute__((ms_abi)) (*RunCallResult)(u64, void *, u32, u64);
        ((RunCallResult)(uintptr_t)vtable[1])(callback, result, (u32)failed, call);   /* Run(param, bIOFailure, call) */
    }
}

#undef MMS_FN
