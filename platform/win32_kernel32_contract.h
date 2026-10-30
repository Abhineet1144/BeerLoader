#ifndef BEER_WIN32_KERNEL32_CONTRACT_H
#define BEER_WIN32_KERNEL32_CONTRACT_H

#include <stddef.h>
#include <stdint.h>

/*
 * Stable, first-party Kernel32 surface implemented by Beer but not necessarily
 * present in a guest's static import table.  Keep entries grouped by documented
 * Win32 contract and publish an entry only when loader.c has a real implementation.
 *
 * This is deliberately not an inventory copied from Windows, Wine, or another
 * compatibility runtime.  It records Beer's own supported contracts so a PE
 * export walker observes the same module identity as GetProcAddress.
 */
static const char *const beer_kernel32_locale_exports[] = {
    "CompareStringA",
    "CompareStringEx",
    "CompareStringOrdinal",
    "CompareStringW",
    "EnumSystemLocalesEx",
    "EnumSystemLocalesW",
    "GetLocaleInfoA",
    "GetLocaleInfoEx",
    "GetLocaleInfoW",
    "GetUserDefaultLCID",
    "GetUserDefaultLangID",
    "GetUserDefaultLocaleName",
    "GetUserDefaultUILanguage",
    "GetUserPreferredUILanguages",
    "IsValidLocale",
    "IsValidLocaleName",
    "LCMapStringA",
    "LCMapStringEx",
    "LCMapStringW",
    "LocaleNameToLCID",
};

#define BEER_KERNEL32_LOCALE_EXPORT_COUNT \
    (sizeof(beer_kernel32_locale_exports) / sizeof(beer_kernel32_locale_exports[0]))

static const char *const beer_kernel32_environment_exports[] = {
    "ExpandEnvironmentStringsA",
    "ExpandEnvironmentStringsW",
    "GetEnvironmentVariableA",
    "GetEnvironmentVariableW",
    "SetEnvironmentVariableA",
    "SetEnvironmentVariableW",
};

#define BEER_KERNEL32_ENVIRONMENT_EXPORT_COUNT \
    (sizeof(beer_kernel32_environment_exports) / \
     sizeof(beer_kernel32_environment_exports[0]))

static const char *const beer_kernel32_thread_exports[] = {
    "QueueUserWorkItem",
    "SetThreadDescription",
};

#define BEER_KERNEL32_THREAD_EXPORT_COUNT \
    (sizeof(beer_kernel32_thread_exports) / sizeof(beer_kernel32_thread_exports[0]))

static const char *const beer_kernel32_string_exports[] = {
    "lstrcmpA",
    "lstrcmpW",
    "lstrcmpiA",
    "lstrcmpiW",
};

#define BEER_KERNEL32_STRING_EXPORT_COUNT \
    (sizeof(beer_kernel32_string_exports) / sizeof(beer_kernel32_string_exports[0]))

static const char *const beer_kernel32_string_probe_exports[] = {
    "IsBadStringPtrA",
    "IsBadStringPtrW",
};

#define BEER_KERNEL32_STRING_PROBE_EXPORT_COUNT \
    (sizeof(beer_kernel32_string_probe_exports) / \
     sizeof(beer_kernel32_string_probe_exports[0]))

static const char *const beer_kernel32_local_memory_exports[] = {
    "LocalAlloc",
    "LocalFree",
    "LocalLock",
    "LocalReAlloc",
    "LocalSize",
    "LocalUnlock",
};

#define BEER_KERNEL32_LOCAL_MEMORY_EXPORT_COUNT \
    (sizeof(beer_kernel32_local_memory_exports) / \
     sizeof(beer_kernel32_local_memory_exports[0]))

static const char *const beer_kernel32_file_time_exports[] = {
    "CompareFileTime",
    "FileTimeToLocalFileTime",
    "FileTimeToSystemTime",
    "GetFileTime",
    "GetSystemTimeAsFileTime",
    "GetSystemTimePreciseAsFileTime",
    "SystemTimeToFileTime",
};

#define BEER_KERNEL32_FILE_TIME_EXPORT_COUNT \
    (sizeof(beer_kernel32_file_time_exports) / \
     sizeof(beer_kernel32_file_time_exports[0]))

static const char *const beer_kernel32_file_search_exports[] = {
    "FindClose",
    "FindFirstFileA",
    "FindFirstFileExA",
    "FindFirstFileExW",
    "FindFirstFileW",
    "FindNextFileA",
    "FindNextFileW",
};

#define BEER_KERNEL32_FILE_SEARCH_EXPORT_COUNT \
    (sizeof(beer_kernel32_file_search_exports) / \
     sizeof(beer_kernel32_file_search_exports[0]))

static inline int beer_compare_filetime(uint64_t left, uint64_t right)
{
    return left < right ? -1 : left > right ? 1 : 0;
}

#endif
