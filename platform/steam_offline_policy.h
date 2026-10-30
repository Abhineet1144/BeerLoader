#ifndef BEER_STEAM_OFFLINE_POLICY_H
#define BEER_STEAM_OFFLINE_POLICY_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* A successfully initialized local Steam runtime has an authenticated local
 * user even when network services are unavailable. CSteamID carries an
 * eight-byte payload; the Steamworks C++ interface constructs its non-trivial
 * return value through the Win64 hidden-result-pointer ABI. */
#define BEER_STEAM_LOCAL_USER_ID UINT64_C(0x0110000100000001)

/* ISteamRemoteStorage014 C++ vtable positions. Keep these explicit so adding
 * later methods cannot accidentally treat the interface as append-only. */
#define BEER_STEAM_REMOTE_STORAGE_FILE_WRITE_SLOT                0u
#define BEER_STEAM_REMOTE_STORAGE_FILE_READ_SLOT                 1u
#define BEER_STEAM_REMOTE_STORAGE_FILE_WRITE_ASYNC_SLOT          2u
#define BEER_STEAM_REMOTE_STORAGE_FILE_READ_ASYNC_SLOT           3u
#define BEER_STEAM_REMOTE_STORAGE_FILE_READ_ASYNC_COMPLETE_SLOT  4u
#define BEER_STEAM_REMOTE_STORAGE_WRITE_CALLBACK_ID              1331u
#define BEER_STEAM_REMOTE_STORAGE_READ_CALLBACK_ID               1332u
#define BEER_STEAM_REMOTE_STORAGE_WRITE_RESULT_SIZE              4u
/* Steam callback structs use 8-byte packing. The logical fields occupy 20
 * bytes and the structure has four bytes of tail padding. */
#define BEER_STEAM_REMOTE_STORAGE_READ_RESULT_SIZE               24u
#define BEER_STEAM_REMOTE_STORAGE_READ_RESULT_OFFSET_OFFSET      12u
#define BEER_STEAM_REMOTE_STORAGE_READ_RESULT_BYTES_OFFSET       16u

/* FileReadAsync has two completion phases. GetAPICallResult consumes the
 * callback payload, but the request's path/range must remain alive until
 * FileReadAsyncComplete copies the bytes into the caller's buffer. Other
 * asynchronous results are complete once their callback payload is read. */
static inline int beer_steam_call_result_finishes_request(uint32_t callback_id)
{
    return callback_id != BEER_STEAM_REMOTE_STORAGE_READ_CALLBACK_ID;
}

static inline int beer_steam_storage_normalize_path(const char *name,
                                                     char *path,
                                                     size_t capacity)
{
    if (!name || !path || capacity < 2 || name[0] == '/' || name[0] == '\\' ||
        (name[0] && name[1] == ':'))
        return 0;

    size_t output = 0;
    const char *component = name;
    while (*component) {
        while (*component == '/' || *component == '\\') component++;
        if (!*component) break;
        const char *end = component;
        while (*end && *end != '/' && *end != '\\') end++;
        size_t length = (size_t)(end - component);
        if (length == 1 && component[0] == '.') {
            component = end;
            continue;
        }
        if (length == 2 && component[0] == '.' && component[1] == '.') return 0;
        if (!length || output + (output ? 1u : 0u) + length >= capacity) return 0;
        if (output) path[output++] = '/';
        for (size_t i = 0; i < length; ++i) path[output++] = component[i];
        component = end;
    }
    if (!output) return 0;
    path[output] = '\0';
    return 1;
}

static inline uint32_t beer_steam_parse_app_id_text(const char *text)
{
    if (!text) return 0;
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') ++text;
    uint64_t value = 0;
    const char *start = text;
    while (*text >= '0' && *text <= '9') {
        value = value * 10u + (uint32_t)(*text++ - '0');
        if (value > UINT32_MAX) return 0;
    }
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') ++text;
    return text != start && !*text && value ? (uint32_t)value : 0;
}

static inline uint64_t beer_steam_local_user_logged_on(int runtime_initialized)
{
    return runtime_initialized ? 1u : 0u;
}

static inline uint64_t beer_steam_local_user_id(void)
{
    return BEER_STEAM_LOCAL_USER_ID;
}

/* Steam's application-local wrappers compare the registry installation root
 * with their own module directory before selecting an adjacent private
 * backend. In explicit guest-module mode, expose that application directory;
 * compatibility-facade mode retains Beer's synthetic client installation.
 * This is module policy, not executable-name or app-ID detection. */
static inline int beer_steam_registry_root(int guest_module_mode,
                                           const char *windows_executable,
                                           char *root,
                                           size_t capacity)
{
    static const char compatibility_root[] = "C:\\Program Files (x86)\\Steam";
    if (!root || !capacity) return 0;
    const char *source = compatibility_root;
    size_t length = sizeof(compatibility_root) - 1u;
    if (guest_module_mode) {
        if (!windows_executable || !*windows_executable) return 0;
        const char *slash = NULL;
        for (const char *cursor = windows_executable; *cursor; ++cursor)
            if (*cursor == '\\' || *cursor == '/') slash = cursor;
        if (!slash || slash == windows_executable) return 0;
        source = windows_executable;
        length = (size_t)(slash - windows_executable);
    }
    if (length + 1u > capacity) return 0;
    memcpy(root, source, length);
    root[length] = '\0';
    return 1;
}

#endif
