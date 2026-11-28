/* ISteamMatchmaking for official-mode games, backed by platform/beer_steam_mms.c, and the Steam
 * client's callback channel for it (Steam_BGetCallback / Steam_FreeLastCallback / Steam_GetAPICallResult,
 * which Valve's steam_api64 pumps from SteamAPI_RunCallbacks, plus Beer's own SteamAPI facade).
 *
 * MSVC Win64 member functions return a CSteamID through a hidden pointer after `this`; CSteamID arguments
 * arrive as 64-bit integers. Included once by loader.c. */

static _Atomic(int) g_steam_mms_started;

#include "steam_networking_identity.h"
#include "beer_steam_networking.h"

#include "win32_steam_networking.inc"

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
MMS_FN mm_create_lobby(u64 self, s32 type, s32 max)
{
    (void)self;
    u64 call = beer_mms_create_lobby(type, max);
    if (getenv("BEER_STEAM_DIAGNOSTICS"))
        fprintf(stderr, "[STEAM-MMS] CreateLobby(type=%d, max=%d) -> call=%llu\n",
                type, max, (unsigned long long)call);
    return call;
}
MMS_FN mm_join_lobby(u64 self, u64 lobby) { (void)self; return beer_mms_join_lobby(lobby); }
MMS_FN mm_leave_lobby(u64 self, u64 lobby) { (void)self; beer_mms_leave_lobby(lobby); return 0; }
/* InviteUserToLobby: with a signed-in session the provider sends it as the Steam client does (matchmaking message and
 * a chat invite with a Join button), off the game thread; the result is whether the invite could be started. */
struct steam_invite_job { u32 appid; u64 lobby, user; };
static void *steam_invite_thread(void *arg)
{
    struct steam_invite_job *job = arg;
    beer_steam_session_invite_lobby(job->appid, job->lobby, job->user);
    free(job);
    return NULL;
}
static int steam_invite_async(u64 lobby, u64 user)
{
    struct steam_invite_job *job = malloc(sizeof(*job));
    pthread_t thread;
    if (!job) return 0;
    *job = (struct steam_invite_job){ g_steam_own_appid, lobby, user };
    if (pthread_create(&thread, NULL, steam_invite_thread, job)) { free(job); return 0; }
    pthread_detach(thread);
    return 1;
}
MMS_FN mm_invite(u64 self, u64 lobby, u64 user)
{
    (void)self;
    if (g_steam_api_official && beer_steam_session_active() && lobby && user) return (u64)steam_invite_async(lobby, user);
    return (u64)beer_mms_invite(lobby, user);
}
MMS_FN mm_member_count(u64 self, u64 lobby) { (void)self; return (u64)(s64)beer_mms_member_count(lobby); }
MMS_FN mm_member_by_index(u64 self, u64 *ret, u64 lobby, s32 index)
    { (void)self; if (ret) *ret = beer_mms_member_by_index(lobby, index); return (u64)(uintptr_t)ret; }
MMS_FN mm_lobby_data(u64 self, u64 lobby, const char *key)
{
    (void)self;
    const char *value = beer_mms_lobby_data(lobby, key);
    if (getenv("BEER_STEAM_DIAGNOSTICS") && key && strcasestr(key, "ping"))
        fprintf(stderr, "[STEAM-PING] lobby %llu key %.128s: %s\n", (unsigned long long)lobby,
                key, *value ? "present" : "missing");
    return (u64)(uintptr_t)value;
}
MMS_FN mm_set_lobby_data(u64 self, u64 lobby, const char *key, const char *value)
    { (void)self; return (u64)beer_mms_set_lobby_data(lobby, key, value); }
MMS_FN mm_lobby_data_count(u64 self, u64 lobby) { (void)self; return (u64)(s64)beer_mms_lobby_data_count(lobby); }
MMS_FN mm_lobby_data_by_index(u64 self, u64 lobby, s32 index, char *key, s32 key_cap, char *value, s32 value_cap)
{
    (void)self;
    int ok = beer_mms_lobby_data_by_index(lobby, index, key, key_cap, value, value_cap);
    if (ok && getenv("BEER_STEAM_DIAGNOSTICS") && key && key_cap > 0 && strcasestr(key, "ping"))
        fprintf(stderr, "[STEAM-PING] lobby %llu enumerated key %.128s: %s\n", (unsigned long long)lobby,
                key, value && value_cap > 0 && *value ? "present" : "missing");
    return (u64)ok;
}
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
        steam_vtab_unique_slots(vtable, sizeof(vtable) / sizeof(*vtable));   /* XMM6-15 kept */
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

/* ISteamFriends::ActivateGameOverlayInviteDialog(lobby): Steam's overlay is not available, so Beer opens its own
 * friend picker (manager/invite_picker.py, which asks the Steam provider for the friends list and sends the invites).
 * The game is told the overlay opened and closed (GameOverlayActivated_t, callback 331), as with Steam's overlay, so
 * it frees the mouse and pauses input meanwhile. */
static void steam_overlay_activated(int active)
{
    struct { u8 active, user_initiated, pad[2]; u32 appid, overlay_pid; } event = {
        (u8)(active != 0), 1, { 0, 0 }, g_steam_own_appid, (u32)getpid() };
    steam_callback_post(331, &event, sizeof(event));
}
static _Atomic(int) g_steam_invite_picker_open;
static void *steam_invite_picker_thread(void *arg)
{
    const u64 lobby = (u64)(uintptr_t)arg;
    char exe[4096], script[4200], appid[16], lobby_text[32];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    exe[n > 0 ? n : 0] = 0;
    char *slash = strrchr(exe, '/');
    if (slash) *slash = 0;
    snprintf(script, sizeof(script), "%s/manager/invite_picker.py", exe);
    snprintf(appid, sizeof(appid), "%u", g_steam_own_appid);
    snprintf(lobby_text, sizeof(lobby_text), "%llu", (unsigned long long)lobby);
    steam_overlay_activated(1);
    char *argv[] = { "python3", script, "--appid", appid, "--lobby", lobby_text, NULL };
    size_t count = 0;
    while (environ && environ[count]) ++count;
    char **env = calloc(count + 1, sizeof(char *));
    for (size_t i = 0, k = 0; env && i < count; ++i)
        if (strncmp(environ[i], "LD_PRELOAD=", 11)) env[k++] = environ[i];   /* the game's preloads are not for it */
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none, defaults;
    sigemptyset(&none);
    sigfillset(&defaults);
    sigdelset(&defaults, SIGKILL);
    sigdelset(&defaults, SIGSTOP);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    pid_t child = -1;
    if (posix_spawnp(&child, "python3", NULL, &attr, argv, env ? env : environ)) child = -1;
    posix_spawnattr_destroy(&attr);
    free(env);
    int status = 0;
    if (child > 0) while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    fprintf(stderr, "[STEAM] invite picker for lobby %s closed (%s)\n", lobby_text,
            child < 0 ? "could not start" : WIFEXITED(status) && !WEXITSTATUS(status) ? "ok" : "failed");
    steam_overlay_activated(0);
    atomic_store(&g_steam_invite_picker_open, 0);
    return NULL;
}
MMS_FN steam_friends_invite_dialog(u64 self, u64 lobby)
{
    (void)self;
    fprintf(stderr, "[STEAM] ActivateGameOverlayInviteDialog(lobby %llu)\n", (unsigned long long)lobby);
    if (!g_steam_api_official || !beer_steam_session_active() || !lobby) return 0;
    if (atomic_exchange(&g_steam_invite_picker_open, 1)) return 0;          /* already open */
    pthread_t thread;
    if (pthread_create(&thread, NULL, steam_invite_picker_thread, (void *)(uintptr_t)lobby)) {
        atomic_store(&g_steam_invite_picker_open, 0);
        return 0;
    }
    pthread_detach(thread);
    return 0;
}

/* ---- accepting a friend's invite ----
 * An invite that reached the account (beer_steam_session_next_invite) is posted as LobbyInvite_t (503: inviter,
 * lobby, game ID) and offered in a small window (manager/invite_prompt.py), standing in for Steam's notification and
 * its Join button. Join posts GameLobbyJoinRequested_t (333: lobby, friend), what Steam sends when the invite is
 * accepted while the game runs; the game then joins the lobby itself. */
struct steam_invite_offer { u64 from, lobby; char name[96]; };
static _Atomic(int) g_steam_invite_prompt_open;
static void *steam_invite_prompt_thread(void *arg)
{
    struct steam_invite_offer *offer = arg;
    char exe[4096], script[4200], from_text[32];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    exe[n > 0 ? n : 0] = 0;
    char *slash = strrchr(exe, '/');
    if (slash) *slash = 0;
    snprintf(script, sizeof(script), "%s/manager/invite_prompt.py", exe);
    snprintf(from_text, sizeof(from_text), "%llu", (unsigned long long)offer->from);
    char *argv[] = { "python3", script, "--name", offer->name[0] ? offer->name : from_text, NULL };
    size_t count = 0;
    while (environ && environ[count]) ++count;
    char **env = calloc(count + 1, sizeof(char *));
    for (size_t i = 0, k = 0; env && i < count; ++i)
        if (strncmp(environ[i], "LD_PRELOAD=", 11)) env[k++] = environ[i];
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none, defaults;
    sigemptyset(&none);
    sigfillset(&defaults);
    sigdelset(&defaults, SIGKILL);
    sigdelset(&defaults, SIGSTOP);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    pid_t child = -1;
    if (posix_spawnp(&child, "python3", NULL, &attr, argv, env ? env : environ)) child = -1;
    posix_spawnattr_destroy(&attr);
    free(env);
    int status = 1 << 8;
    if (child > 0) while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    const int join = child > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    fprintf(stderr, "[STEAM] invite to lobby %llu from %s: %s\n", (unsigned long long)offer->lobby, from_text,
            join ? "accepted" : "not accepted");
    if (join) {
        const u64 request[2] = { offer->lobby, offer->from };          /* GameLobbyJoinRequested_t */
        steam_callback_post(333, request, sizeof(request));
    }
    free(offer);
    atomic_store(&g_steam_invite_prompt_open, 0);
    return NULL;
}
static void steam_invites_poll(void)
{
    if (!g_steam_api_official || !beer_steam_session_active()) return;
    static _Atomic(u64) last_ms;
    const u64 now = beer_time_monotonic_ns_for(BEER_CLOCK_TICK) / 1000000u;
    if (now - atomic_load(&last_ms) < 250) return;
    atomic_store(&last_ms, now);
    u64 from = 0, lobby = 0;
    u32 appid = 0;
    char name[96];
    while (beer_steam_session_next_invite(&from, &appid, &lobby, name, sizeof(name))) {
        if (appid != g_steam_own_appid) continue;
        fprintf(stderr, "[STEAM] lobby invite from %llu (%s) to lobby %llu\n", (unsigned long long)from, name,
                (unsigned long long)lobby);
        const u64 invite[3] = { from, lobby, appid };                     /* LobbyInvite_t */
        steam_callback_post(503, invite, sizeof(invite));
        if (atomic_exchange(&g_steam_invite_prompt_open, 1)) continue;   /* one prompt at a time */
        struct steam_invite_offer *offer = calloc(1, sizeof(*offer));
        pthread_t thread;
        if (!offer) { atomic_store(&g_steam_invite_prompt_open, 0); continue; }
        offer->from = from;
        offer->lobby = lobby;
        snprintf(offer->name, sizeof(offer->name), "%s", name);
        if (pthread_create(&thread, NULL, steam_invite_prompt_thread, offer)) {
            free(offer);
            atomic_store(&g_steam_invite_prompt_open, 0);
        } else {
            pthread_detach(thread);
        }
    }
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
    if (getenv("BEER_STEAM_DIAGNOSTICS"))
        fprintf(stderr, "[STEAM-MMS] callback id=%d size=%d\n", id, size);
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
    steam_invites_poll();
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
