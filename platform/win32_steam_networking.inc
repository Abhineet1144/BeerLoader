/* ISteamNetworkingSockets012 and ISteamNetworkingUtils004 for official-mode games, on platform/beer_steam_networking.c.
 * Vtable slots follow the Steamworks SDK the games are built with (checked against the flat wrappers in a
 * shipped steam_api64.dll: sockets RunCallbacks = 41, utils SetConfigValue = 16).
 *
 * What the Steam client provides and Beer reproduces here:
 * - SteamNetConnectionStatusChangedCallback_t (1221) goes to the function a game configured with
 *   k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged (on the connection, its listen socket, or globally),
 *   invoked from the game's callback pump; without one it is posted as a regular Steam callback.
 * - SteamNetworkingMessage_t objects handed to the game release themselves through Win64 function pointers: the
 *   library's messages are copied into Beer-owned objects and released at once.
 * - Valve's relay network is not available: GetRelayNetworkStatus reports k_ESteamNetworkingAvailability_CannotTry.
 *   Included once by win32_steam_matchmaking.inc. */

typedef void __attribute__((ms_abi)) (*SteamNetStatusFn)(void *);

/* Callback functions configured by the game (config value 201), by handle. */
#define NET_CALLBACK_SLOTS 256
static struct { u32 handle; u64 fn; } g_net_conn_callbacks[NET_CALLBACK_SLOTS], g_net_listen_callbacks[NET_CALLBACK_SLOTS];
static u64 g_net_global_callback;
static u64 g_net_auth_callback;
static pthread_mutex_t g_net_callback_lock = PTHREAD_MUTEX_INITIALIZER;

static void net_callback_set(typeof(g_net_conn_callbacks[0]) *table, u32 handle, u64 fn)
{
    if (!handle) return;
    pthread_mutex_lock(&g_net_callback_lock);
    int slot = -1;
    for (int i = 0; i < NET_CALLBACK_SLOTS; ++i) {
        if (table[i].handle == handle) { slot = i; break; }
        if (slot < 0 && !table[i].handle) slot = i;
    }
    if (slot >= 0) { table[slot].handle = fn ? handle : 0; table[slot].fn = fn; }
    pthread_mutex_unlock(&g_net_callback_lock);
}
static u64 net_callback_get(typeof(g_net_conn_callbacks[0]) *table, u32 handle)
{
    u64 fn = 0;
    pthread_mutex_lock(&g_net_callback_lock);
    for (int i = 0; handle && i < NET_CALLBACK_SLOTS && !fn; ++i) if (table[i].handle == handle) fn = table[i].fn;
    pthread_mutex_unlock(&g_net_callback_lock);
    return fn;
}

/* SteamNetworkingConfigValue_t arrays (16 bytes: value, data type, 8-byte union): callback values hold Win64 function
 * pointers the library must not see; they are taken out (ConnectionStatusChanged is kept for the handle). */
static int net_filter_options(int count, const void *options, u8 *out, int cap, u64 *status_fn)
{
    *status_fn = 0;
    if (count <= 0 || !options) return 0;
    int kept = 0;
    const u8 *in = options;
    for (int i = 0; i < count; ++i, in += 16) {
        s32 value;
        memcpy(&value, in, 4);
        if (value >= BEER_NET_CONFIG_CALLBACK_FIRST && value <= BEER_NET_CONFIG_CALLBACK_LAST) {
            if (value == BEER_NET_CONFIG_CALLBACK_CONNECTION_STATUS) memcpy(status_fn, in + 8, 8);
            continue;
        }
        if (kept < cap) memcpy(out + 16 * kept++, in, 16);
    }
    return kept;
}

/* ---- SteamNetworkingMessage_t (216 bytes: m_pData @0, m_cbSize @8, m_conn @12, m_identityPeer @16,
 * m_nConnUserData @152, m_usecTimeReceived @160, m_nMessageNumber @168, m_pfnFreeData @176, m_pfnRelease @184,
 * m_nChannel @192, m_nFlags @196, m_nUserData @200, m_idxLane @208). The payload follows the object. ---- */
#define NET_MSG_SIZE 216
static void __attribute__((ms_abi)) net_msg_free_data(u8 *msg) { (void)msg; }    /* payload lives in the object */
static void __attribute__((ms_abi)) net_msg_release(u8 *msg)
{
    if (!msg) return;
    u64 free_data;
    memcpy(&free_data, msg + 176, 8);
    if (free_data && free_data != (u64)(uintptr_t)net_msg_free_data)
        ((void __attribute__((ms_abi)) (*)(u8 *))(uintptr_t)free_data)(msg);   /* the game's own buffer */
    free(msg);
}
static u8 *net_msg_alloc(int payload)
{
    if (payload < 0) payload = 0;
    u8 *msg = calloc(1, NET_MSG_SIZE + (size_t)payload);
    if (!msg) return NULL;
    const u64 data = payload ? (u64)(uintptr_t)(msg + NET_MSG_SIZE) : 0;
    const u64 free_data = (u64)(uintptr_t)net_msg_free_data, release = (u64)(uintptr_t)net_msg_release;
    const s32 size = payload;
    memcpy(msg, &data, 8);
    memcpy(msg + 8, &size, 4);
    memcpy(msg + 176, &free_data, 8);
    memcpy(msg + 184, &release, 8);
    return msg;
}
/* Replaces the library's messages in `messages` with Beer-owned Win64 objects. */
static int net_msgs_to_guest(void **messages, int count)
{
    for (int i = 0; i < count; ++i) {
        u8 *native = messages[i];
        s32 size;
        memcpy(&size, native + 8, 4);
        u8 *copy = net_msg_alloc(size);
        if (copy) {
            u64 data, free_data, release;
            memcpy(&free_data, copy + 176, 8);
            memcpy(&release, copy + 184, 8);
            memcpy(&data, copy, 8);
            memcpy(copy + 8, native + 8, NET_MSG_SIZE - 8);   /* size, connection, identity, numbers, lane... */
            memcpy(copy + 176, &free_data, 8);
            memcpy(copy + 184, &release, 8);
            u64 native_data;
            memcpy(&native_data, native, 8);
            if (size > 0 && native_data) memcpy(copy + NET_MSG_SIZE, (const void *)(uintptr_t)native_data, (size_t)size);
        }
        beer_steam_networking_release_message(native);
        messages[i] = copy;
        if (!copy) {                                          /* out of memory: drop the rest, as a short read */
            for (int k = i + 1; k < count; ++k) beer_steam_networking_release_message(messages[k]);
            return i;
        }
    }
    return count;
}

/* ---- the callback pump: runs on the game's thread from Steam_BGetCallback / SteamAPI_RunCallbacks /
 * ISteamNetworkingSockets::RunCallbacks ---- */
static void steam_callback_post(u32 id, const void *data, u32 size);
static void steam_networking_dispatch_auth(const void *auth)
{
    pthread_mutex_lock(&g_net_callback_lock);
    u64 fn = g_net_auth_callback;
    pthread_mutex_unlock(&g_net_callback_lock);
    if (fn) ((SteamNetStatusFn)(uintptr_t)fn)((void *)auth);
    else steam_callback_post(BEER_STEAM_AUTH_CALLBACK, auth, BEER_STEAM_AUTH_SIZE);
}

static void steam_networking_pump(void)
{
    static __thread int busy;                                /* a callback calling RunCallbacks again */
    if (busy) return;
    busy = 1;
    u8 auth[BEER_STEAM_AUTH_SIZE];
    while (beer_steam_networking_next_auth(auth)) {
        steam_networking_dispatch_auth(auth);
    }
    u8 status[BEER_STEAM_STATUS_SIZE];
    while (beer_steam_networking_next_status(status)) {
        u32 connection, listen;
        s32 state;
        memcpy(&connection, status, 4);
        memcpy(&listen, status + 8 + 144, 4);
        memcpy(&state, status + 8 + 176, 4);
        u64 fn = net_callback_get(g_net_conn_callbacks, connection);
        if (!fn) fn = net_callback_get(g_net_listen_callbacks, listen);
        if (!fn) fn = g_net_global_callback;
        if (fn) ((SteamNetStatusFn)(uintptr_t)fn)(status);
        else steam_callback_post(BEER_STEAM_STATUS_CALLBACK, status, BEER_STEAM_STATUS_SIZE);
        if (state == 0 || state == -3) net_callback_set(g_net_conn_callbacks, connection, 0);   /* None / Dead */
    }
    busy = 0;
}

/* ---- ISteamNetworkingSockets012 ---- */
#define NET_FN static u64 __attribute__((ms_abi))
NET_FN net_listen_ip(u64 self, const void *address, s32 count, const void *options)
{
    (void)self;
    u8 kept[64 * 16]; u64 fn;
    const int n = net_filter_options(count, options, kept, 64, &fn);
    const u32 h = beer_steam_networking_listen_ip(address, n, n ? kept : NULL);
    net_callback_set(g_net_listen_callbacks, h, fn);
    return h;
}
NET_FN net_connect_ip(u64 self, const void *address, s32 count, const void *options)
{
    (void)self;
    u8 kept[64 * 16]; u64 fn;
    const int n = net_filter_options(count, options, kept, 64, &fn);
    const u32 h = beer_steam_networking_connect_ip(address, n, n ? kept : NULL);
    net_callback_set(g_net_conn_callbacks, h, fn);
    return h;
}
NET_FN net_listen_p2p(u64 self, s32 port, s32 count, const void *options)
{
    (void)self;
    u8 kept[64 * 16]; u64 fn;
    const int n = net_filter_options(count, options, kept, 64, &fn);
    const u32 h = beer_steam_networking_listen(port, n, n ? kept : NULL);
    net_callback_set(g_net_listen_callbacks, h, fn);
    return h;
}
NET_FN net_connect_p2p(u64 self, const void *identity, s32 port, s32 count, const void *options)
{
    (void)self;
    u8 kept[64 * 16]; u64 fn;
    const int n = net_filter_options(count, options, kept, 64, &fn);
    const u32 h = beer_steam_networking_connect_p2p(identity, port, n, n ? kept : NULL);
    net_callback_set(g_net_conn_callbacks, h, fn);
    return h;
}
NET_FN net_accept(u64 self, u32 c) { (void)self; return (u64)(u32)beer_steam_networking_accept(c); }
NET_FN net_close(u64 self, u32 c, s32 reason, const char *debug, u8 linger)
    { (void)self; return (u64)beer_steam_networking_close(c, reason, debug, linger); }
NET_FN net_close_listener(u64 self, u32 h)
{
    (void)self;
    net_callback_set(g_net_listen_callbacks, h, 0);
    return (u64)beer_steam_networking_close_listener(h);
}
NET_FN net_set_user_data(u64 self, u32 c, s64 v) { (void)self; return (u64)beer_steam_networking_set_user_data(c, v); }
NET_FN net_get_user_data(u64 self, u32 c) { (void)self; return (u64)beer_steam_networking_get_user_data(c); }
NET_FN net_set_name(u64 self, u32 c, const char *name) { (void)self; beer_steam_networking_set_name(c, name); return 0; }
NET_FN net_get_name(u64 self, u32 c, char *out, s32 cap) { (void)self; return (u64)beer_steam_networking_get_name(c, out, cap); }
NET_FN net_send(u64 self, u32 c, const void *data, u32 size, s32 flags, s64 *number)
    { (void)self; return (u64)(u32)beer_steam_networking_send(c, data, size, flags, (int64_t *)number); }
/* SendMessages: Steam takes ownership of every message, sent or not. Each result is the message number, or the
 * negated EResult. */
NET_FN net_send_messages(u64 self, s32 count, u8 **messages, s64 *results)
{
    (void)self;
    for (s32 i = 0; i < count; ++i) {
        u8 *msg = messages ? messages[i] : NULL;
        if (!msg) { if (results) results[i] = -2; continue; }      /* k_EResultFail */
        u64 data, release;
        s32 size, flags;
        u32 connection;
        memcpy(&data, msg, 8);
        memcpy(&size, msg + 8, 4);
        memcpy(&connection, msg + 12, 4);
        memcpy(&flags, msg + 196, 4);
        int64_t number = 0;
        const int result = beer_steam_networking_send(connection, (const void *)(uintptr_t)data, (u32)size, flags, &number);
        if (results) results[i] = result == 1 ? number : -(s64)result;
        memcpy(&release, msg + 184, 8);
        if (release) ((void __attribute__((ms_abi)) (*)(u8 *))(uintptr_t)release)(msg);
    }
    return 0;
}
NET_FN net_flush(u64 self, u32 c) { (void)self; return (u64)(u32)beer_steam_networking_flush(c); }
NET_FN net_receive(u64 self, u32 c, void **out, s32 count)
{
    (void)self;
    const int n = beer_steam_networking_receive(c, out, count);
    return (u64)(s64)(n > 0 ? net_msgs_to_guest(out, n) : n);
}
NET_FN net_info(u64 self, u32 c, void *info) { (void)self; return (u64)beer_steam_networking_info(c, info); }
NET_FN net_realtime(u64 self, u32 c, void *status, s32 lanes, void *lane_status)
    { (void)self; return (u64)(u32)beer_steam_networking_realtime(c, status, lanes, lane_status); }
NET_FN net_detailed(u64 self, u32 c, char *out, s32 cap) { (void)self; return (u64)(s64)beer_steam_networking_detailed(c, out, cap); }
NET_FN net_listen_address(u64 self, u32 h, void *address) { (void)self; return (u64)beer_steam_networking_listen_address(h, address); }
NET_FN net_socket_pair(u64 self, u32 *a, u32 *b, u8 loopback, const void *id1, const void *id2)
    { (void)self; return (u64)beer_steam_networking_socket_pair(a, b, loopback, id1, id2); }
NET_FN net_configure_lanes(u64 self, u32 c, s32 lanes, const int *priorities, const u16 *weights)
    { (void)self; return (u64)(u32)beer_steam_networking_configure_lanes(c, lanes, priorities, weights); }
NET_FN net_init_auth(u64 self) { (void)self; return (u64)(s64)beer_steam_networking_init_auth(); }
NET_FN net_auth_status(u64 self, void *details) { (void)self; return (u64)(s64)beer_steam_networking_auth_status(details); }
NET_FN net_poll_group(u64 self) { (void)self; return beer_steam_networking_poll_group(); }
NET_FN net_destroy_group(u64 self, u32 h) { (void)self; return (u64)beer_steam_networking_destroy_poll_group(h); }
NET_FN net_set_group(u64 self, u32 c, u32 g) { (void)self; return (u64)beer_steam_networking_set_poll_group(c, g); }
NET_FN net_receive_group(u64 self, u32 g, void **out, s32 count)
{
    (void)self;
    const int n = beer_steam_networking_receive_poll_group(g, out, count);
    return (u64)(s64)(n > 0 ? net_msgs_to_guest(out, n) : n);
}
NET_FN net_run_callbacks(u64 self) { (void)self; steam_networking_pump(); return 0; }

static u64 __attribute__((ms_abi)) steam_networking_get_identity(u64 self, void *out)
{
    (void)self;
    u64 id = g_steam_api_official && steam_official_session() ? beer_steam_session_steamid() : 0;
    int ok = beer_steam_networking_identity(out, id);
    if (getenv("BEER_STEAM_DIAGNOSTICS"))
        fprintf(stderr, "[STEAM-NET] GetIdentity -> %d\n", ok);
    return (u64)ok;
}

static const BeerP2PSignaling g_steam_p2p_signaling = {
    beer_steam_session_p2p_send, beer_steam_session_next_p2p_event, beer_steam_session_networking_certificate,
    beer_steam_session_networking_config
};

static u64 steam_networking_sockets_interface(void)
{
    static u64 vtable[64];
    static u64 object;
    if (!object) {
        for (size_t i = 0; i < sizeof(vtable) / sizeof(*vtable); ++i)
            vtable[i] = (u64)(uintptr_t)impl_steam_offline_noop;
        /* GetIdentity reports the session identity whether or not a transport is installed. */
        vtable[21] = (u64)(uintptr_t)steam_networking_get_identity;
        if (g_steam_api_official && steam_official_session() &&
            beer_steam_networking_start(beer_steam_session_steamid())) {
            beer_steam_networking_set_signaling(g_steam_own_appid, &g_steam_p2p_signaling);
            static const struct { int slot; void *fn; } slots[] = {
                { 0, net_listen_ip }, { 1, net_connect_ip }, { 2, net_listen_p2p }, { 3, net_connect_p2p },
                { 4, net_accept }, { 5, net_close }, { 6, net_close_listener }, { 7, net_set_user_data },
                { 8, net_get_user_data }, { 9, net_set_name }, { 10, net_get_name }, { 11, net_send },
                { 12, net_send_messages }, { 13, net_flush }, { 14, net_receive }, { 15, net_info },
                { 16, net_realtime }, { 17, net_detailed }, { 18, net_listen_address }, { 19, net_socket_pair },
                { 20, net_configure_lanes }, { 22, net_init_auth }, { 23, net_auth_status }, { 24, net_poll_group },
                { 25, net_destroy_group }, { 26, net_set_group }, { 27, net_receive_group },
                { 41, net_run_callbacks },
            };
            for (size_t i = 0; i < sizeof(slots) / sizeof(*slots); ++i) vtable[slots[i].slot] = (u64)(uintptr_t)slots[i].fn;
        }
        object = (u64)(uintptr_t)vtable;
    }
    return (u64)(uintptr_t)&object;
}

/* ---- ISteamNetworkingUtils004 ---- */
NET_FN net_utils_allocate(u64 self, s32 size) { (void)self; return (u64)(uintptr_t)net_msg_alloc(size); }
/* SteamRelayNetworkStatus_t: m_eAvail, m_bPingMeasurementInProgress, m_eAvailNetworkConfig, m_eAvailAnyRelay,
 * m_debugMsg[256]. Valve's relays are not reachable from Beer. */
NET_FN net_utils_relay_status(u64 self, u8 *details)
{
    (void)self;
    const s32 cannot_try = -102;
    if (details) {
        memset(details, 0, 16 + 256);
        memcpy(details, &cannot_try, 4);
        memcpy(details + 8, &cannot_try, 4);
        memcpy(details + 12, &cannot_try, 4);
        snprintf((char *)details + 16, 256, "Steam Datagram Relay is not available; peer-to-peer uses direct ICE");
    }
    return (u64)(s64)cannot_try;
}
NET_FN net_utils_timestamp(u64 self) { (void)self; return (u64)beer_steam_networking_timestamp(); }
/* Ping locations require actual relay measurements. Unknown is not zero milliseconds.
 * GetLocalPingLocation returns FLOAT in XMM0, unlike the integer no-op table entries. */
static void net_ping_diagnostic(unsigned slot, const char *method)
{
    if (!getenv("BEER_STEAM_DIAGNOSTICS") || slot >= 32) return;
    static _Atomic(u32) seen;
    u32 bit = 1u << slot;
    if (!(atomic_fetch_or(&seen, bit) & bit))
        fprintf(stderr, "[STEAM-PING] %s called; relay measurement unavailable\n", method);
}
static float __attribute__((ms_abi)) net_utils_local_ping(u64 self, void *location)
{ (void)self; net_ping_diagnostic(2, "GetLocalPingLocation"); if (location) memset(location, 0, 512); return -1.0f; }
NET_FN net_utils_estimate_ping(u64 self, const void *a, const void *b)
{ (void)self; (void)a; (void)b; net_ping_diagnostic(3, "EstimatePingTimeBetweenTwoLocations"); return (u64)(s64)-2; }
NET_FN net_utils_estimate_local_ping(u64 self, const void *location)
{ (void)self; (void)location; net_ping_diagnostic(4, "EstimatePingTimeFromLocalHost"); return (u64)(s64)-2; }
NET_FN net_utils_ping_string(u64 self, const void *location, char *out, s32 cap)
{ (void)self; (void)location; net_ping_diagnostic(5, "ConvertPingLocationToString"); if (out && cap > 0) out[0] = 0; return 0; }
NET_FN net_utils_parse_ping(u64 self, const char *text, void *location)
{ (void)self; (void)text; (void)location; net_ping_diagnostic(6, "ParsePingLocationString"); return 0; }
static u64 __attribute__((ms_abi)) net_utils_ping_fresh(u64 self, float age)
{ (void)self; (void)age; net_ping_diagnostic(7, "CheckPingDataUpToDate"); return 0; }
NET_FN net_utils_ping_dc(u64 self, u32 pop, u32 *via)
{ (void)self; (void)pop; net_ping_diagnostic(8, "GetPingToDataCenter"); if (via) *via = 0; return (u64)(s64)-2; }
NET_FN net_utils_ping_pop(u64 self, u32 pop)
{ (void)self; (void)pop; net_ping_diagnostic(9, "GetDirectPingToPOP"); return (u64)(s64)-2; }
NET_FN net_utils_set_debug(u64 self, s32 level, u64 fn) { (void)self; (void)level; (void)fn; return 0; }
NET_FN net_utils_set_config(u64 self, s32 value, s32 scope, s64 object, s32 type, const void *argument)
{
    (void)self;
    if (value >= BEER_NET_CONFIG_CALLBACK_FIRST && value <= BEER_NET_CONFIG_CALLBACK_LAST) {
        if (value == 202 /* AuthStatusChanged */) {
            if ((scope != 1 && scope != 2) || type != 5 /* Ptr */) return 0;
            u64 fn = 0;
            if (argument) memcpy(&fn, argument, 8);
            pthread_mutex_lock(&g_net_callback_lock);
            g_net_auth_callback = fn;
            pthread_mutex_unlock(&g_net_callback_lock);
            return 1;
        }
        if (value != BEER_NET_CONFIG_CALLBACK_CONNECTION_STATUS) return 1;     /* accepted; never invoked */
        u64 fn = 0;
        if (argument) memcpy(&fn, argument, 8);
        /* Scopes: 1 global, 2 socket interface, 3 listen socket, 4 connection. */
        if (scope == 1 || scope == 2) g_net_global_callback = fn;
        else if (scope == 3) net_callback_set(g_net_listen_callbacks, (u32)object, fn);
        else if (scope == 4) net_callback_set(g_net_conn_callbacks, (u32)object, fn);
        else return 0;
        return 1;
    }
    return (u64)beer_steam_networking_set_config(value, scope, (intptr_t)object, type, argument);
}
NET_FN net_utils_get_config(u64 self, s32 value, s32 scope, s64 object, s32 *type, void *result, u64 *size)
{
    (void)self;
    if (value == 202) {
        if (scope != 1 && scope != 2) return (u64)(s64)-2;
        if (!size) return (u64)(s64)-3;
        u64 capacity = *size;
        *size = 8;
        if (type) *type = 5;
        if (!result || capacity < 8) return (u64)(s64)-3;
        pthread_mutex_lock(&g_net_callback_lock);
        memcpy(result, &g_net_auth_callback, 8);
        pthread_mutex_unlock(&g_net_callback_lock);
        return 1;
    }
    return (u64)(s64)beer_steam_networking_get_config(value, scope, (intptr_t)object, type, result, (size_t *)size);
}

static u64 steam_networking_utils_interface(void)
{
    static u64 vtable[64];
    static u64 object;
    if (!object) {
        for (size_t i = 0; i < sizeof(vtable) / sizeof(*vtable); ++i)
            vtable[i] = (u64)(uintptr_t)impl_steam_offline_noop;
        vtable[0] = (u64)(uintptr_t)net_utils_allocate;
        vtable[1] = (u64)(uintptr_t)net_utils_relay_status;
        vtable[2] = (u64)(uintptr_t)net_utils_local_ping;
        vtable[3] = (u64)(uintptr_t)net_utils_estimate_ping;
        vtable[4] = (u64)(uintptr_t)net_utils_estimate_local_ping;
        vtable[5] = (u64)(uintptr_t)net_utils_ping_string;
        vtable[6] = (u64)(uintptr_t)net_utils_parse_ping;
        vtable[7] = (u64)(uintptr_t)net_utils_ping_fresh;
        vtable[8] = (u64)(uintptr_t)net_utils_ping_dc;
        vtable[9] = (u64)(uintptr_t)net_utils_ping_pop;
        vtable[12] = (u64)(uintptr_t)net_utils_timestamp;
        vtable[13] = (u64)(uintptr_t)net_utils_set_debug;
        if (g_steam_api_official && steam_official_session() && beer_steam_networking_start(beer_steam_session_steamid())) {
            vtable[16] = (u64)(uintptr_t)net_utils_set_config;
            vtable[17] = (u64)(uintptr_t)net_utils_get_config;
        }
        object = (u64)(uintptr_t)vtable;
    }
    return (u64)(uintptr_t)&object;
}
