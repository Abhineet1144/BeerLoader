#ifndef BEER_WIN32_FILESYSTEM_POLICY_H
#define BEER_WIN32_FILESYSTEM_POLICY_H

#include <stdint.h>
#include <string.h>

/* LoadLibrary first returns modules that the process loader has already
 * published. All other names proceed through DLL search/native file opens;
 * compatibility modules are fallbacks and are not implicitly preloaded. */
static inline int beer_module_is_preloaded_system_dll(const char *lower_name)
{
    return lower_name &&
           (!strcmp(lower_name, "kernel32.dll") ||
            !strcmp(lower_name, "kernelbase.dll") ||
            !strcmp(lower_name, "user32.dll") ||
            !strcmp(lower_name, "ntdll.dll") ||
            !strcmp(lower_name, "advapi32.dll") ||
            !strcmp(lower_name, "win32u.dll") ||
            !strcmp(lower_name, "wintrust.dll"));
}

/* Beer supplies process-wide compatibility state for selected runtime APIs.
 * Their fallback is considered only after the native DLL-open path (including
 * guest hooks) declines the module, and before loading an incompatible second
 * implementation from a host-visible file. */
static inline int beer_module_prefers_compat_fallback(const char *lower_name)
{
    return lower_name &&
           (!strcmp(lower_name, "steam_api64.dll") ||
            !strcmp(lower_name, "steamclient64.dll"));
}

/* Some compatibility modules use a backing PE only for module/file identity.
 * They must never execute as guest code: their exports are implemented by Beer
 * and loading a second runtime creates two conflicting process-wide clients. */
static inline int beer_module_may_execute_guest_image(const char *lower_name)
{
    return lower_name && strcmp(lower_name, "steamclient64.dll");
}

/* Identity-backed compatibility modules expose a byte-identical backing PE at
 * their advertised path while Beer owns execution of their exports. An exact
 * path request may return that identity, unlike an ordinary basename facade. */
static inline int beer_module_has_compat_file_identity(const char *lower_name)
{
    return lower_name && !strcmp(lower_name, "steamclient64.dll");
}

/* An explicit DOS/UNC path is an instruction to load that image, not a module
 * basename search. Compatibility fallbacks may satisfy unresolved basename
 * requests, but must never replace a caller-selected application file. */
static inline int beer_module_path_is_explicit(const char *name)
{
    return name && (strchr(name, '\\') || strchr(name, '/') ||
                    (name[0] && name[1] == ':'));
}

/* An explicit Windows system-directory path identifies the runtime DLL even
 * when an application-local proxy with the same basename is already loaded.
 * Windows can therefore keep both module identities distinct. Normalize only
 * ASCII case and slash direction here; actual file lookup remains elsewhere. */
static inline int beer_module_path_is_system_directory(const char *name)
{
    if (!name) return 0;
    char normalized[512];
    size_t length = strlen(name);
    if (length >= sizeof(normalized)) return 0;
    for (size_t i = 0; i <= length; ++i) {
        char c = name[i];
        if (c == '/') c = '\\';
        if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
        normalized[i] = c;
    }
    return !strncmp(normalized, "c:\\windows\\system32\\", 20) ||
           !strncmp(normalized, "c:\\windows\\syswow64\\", 20);
}

/* A compatibility module may satisfy an unresolved basename search, but an
 * explicit path selects a concrete file. Returning a facade for that request
 * would violate Windows loader identity and hide application-local proxies. */
static inline int beer_module_may_use_compat_fallback(const char *name,
                                                       const char *lower_name)
{
    return !beer_module_path_is_explicit(name) &&
           beer_module_prefers_compat_fallback(lower_name);
}

/* Static imports use normal application-directory search unless the module is
 * already a process-owned KnownDLL identity or an API-set contract. Having a
 * Beer implementation for one imported symbol does not suppress a local proxy
 * DLL: Windows loads the module first and only then binds its exports. */
static inline int beer_module_may_load_application_local(const char *lower_name)
{
    return lower_name &&
           !beer_module_is_preloaded_system_dll(lower_name) &&
           strncmp(lower_name, "api-ms-", 7) &&
           strncmp(lower_name, "ext-ms-", 7);
}

/* These names belong to the Windows runtime rather than the application DLL
 * search directory. A guest PE already loaded for one of them remains valid,
 * but an unresolved name falls back to Beer's process runtime instead of a
 * protector's application-private virtual filesystem. */
static inline int beer_module_is_windows_runtime_dll(const char *lower_name)
{
    static const char *const names[] = {
        "kernel32.dll", "kernelbase.dll", "user32.dll", "ntdll.dll",
        "advapi32.dll", "win32u.dll", "gdi32.dll", "shell32.dll",
        "ole32.dll", "oleaut32.dll", "comctl32.dll", "uxtheme.dll",
        "dwmapi.dll", "msimg32.dll", "shlwapi.dll", "shfolder.dll",
        "version.dll", "setupapi.dll", "comdlg32.dll", "imm32.dll",
        "winmm.dll", "hid.dll", "crypt32.dll", "psapi.dll", "ws2_32.dll"
    };
    if (!lower_name) return 0;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (!strcmp(lower_name, names[i])) return 1;
    return !strncmp(lower_name, "api-ms-", 7) ||
           !strncmp(lower_name, "ext-ms-", 7);
}

/* GUID bytes use the in-memory Win32 layout: Data1/Data2/Data3 are little
 * endian, while Data4 is stored in declaration order. */
static inline int beer_guid_equal(const void *value, const uint8_t expected[16])
{
    return value && memcmp(value, expected, 16) == 0;
}

typedef enum BeerKnownFolder {
    BEER_KNOWN_FOLDER_UNKNOWN = 0,
    BEER_KNOWN_FOLDER_PROFILE,
    BEER_KNOWN_FOLDER_DOCUMENTS,
    BEER_KNOWN_FOLDER_SAVED_GAMES,
    BEER_KNOWN_FOLDER_ROAMING_APPDATA,
    BEER_KNOWN_FOLDER_LOCAL_APPDATA,
    BEER_KNOWN_FOLDER_PROGRAM_DATA
} BeerKnownFolder;

static inline BeerKnownFolder beer_classify_known_folder(const void *rfid)
{
    static const uint8_t profile[16] =
        {0x8f,0x85,0x6c,0x5e,0x20,0xe1,0x41,0x42,0xa7,0xea,0x3f,0xd6,0xd0,0xff,0x5f,0x20};
    static const uint8_t documents[16] =
        {0xd0,0x9a,0xd3,0xfd,0x8f,0x23,0xaf,0x46,0xad,0xb4,0x6c,0x85,0x48,0x03,0x69,0xc7};
    static const uint8_t saved_games[16] =
        {0xff,0x32,0x5c,0x4c,0x9d,0xbb,0xb0,0x43,0xb5,0xb4,0x2d,0x72,0xe5,0x4e,0xaa,0xa4};
    static const uint8_t roaming[16] =
        {0xdb,0x85,0xb6,0x3e,0x6d,0x65,0x48,0x4c,0xba,0xc1,0x3f,0x80,0x03,0x4d,0x20,0x7e};
    static const uint8_t local[16] =
        {0x85,0x27,0xb3,0xf1,0x72,0x6f,0x4a,0x40,0x8f,0x95,0x4e,0x6c,0x3b,0xfa,0x12,0x4a};
    static const uint8_t program_data[16] =
        {0x82,0x5d,0xab,0x62,0xc1,0xfd,0xdc,0x4d,0xa9,0xdd,0x07,0x0d,0x1d,0x49,0x5d,0x97};

    if (beer_guid_equal(rfid, profile)) return BEER_KNOWN_FOLDER_PROFILE;
    if (beer_guid_equal(rfid, documents)) return BEER_KNOWN_FOLDER_DOCUMENTS;
    if (beer_guid_equal(rfid, saved_games)) return BEER_KNOWN_FOLDER_SAVED_GAMES;
    if (beer_guid_equal(rfid, roaming)) return BEER_KNOWN_FOLDER_ROAMING_APPDATA;
    if (beer_guid_equal(rfid, local)) return BEER_KNOWN_FOLDER_LOCAL_APPDATA;
    if (beer_guid_equal(rfid, program_data)) return BEER_KNOWN_FOLDER_PROGRAM_DATA;
    return BEER_KNOWN_FOLDER_UNKNOWN;
}

/* A Win32 read-only OPEN_EXISTING request may be satisfied through an
 * installed native NT file hook before Beer falls back to the host filesystem.
 * Mutating opens remain on the ordinary path until virtual write-through has
 * a complete handle-lifetime implementation. */
static inline int beer_file_open_may_use_native_hook(uint32_t access,
                                                      uint32_t creation)
{
    const uint32_t GENERIC_WRITE = 0x40000000u;
    const uint32_t GENERIC_ALL = 0x10000000u;
    return creation == 3u && !(access & (GENERIC_WRITE | GENERIC_ALL));
}

static inline int beer_creation_disposition_flags(uint32_t creation,
                                                   int *add_flags)
{
    enum {
        BEER_O_CREAT = 1 << 0,
        BEER_O_EXCL = 1 << 1,
        BEER_O_TRUNC = 1 << 2
    };
    if (!add_flags) return 0;
    switch (creation) {
    case 1: *add_flags = BEER_O_CREAT | BEER_O_EXCL; return 1; /* CREATE_NEW */
    case 2: *add_flags = BEER_O_CREAT | BEER_O_TRUNC; return 1; /* CREATE_ALWAYS */
    case 3: *add_flags = 0; return 1;                            /* OPEN_EXISTING */
    case 4: *add_flags = BEER_O_CREAT; return 1;                 /* OPEN_ALWAYS */
    case 5: *add_flags = BEER_O_TRUNC; return 1;                 /* TRUNCATE_EXISTING */
    default: *add_flags = 0; return 0;
    }
}

#endif
