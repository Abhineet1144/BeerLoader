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
    "AppPolicyGetThreadInitializationType",
    "QueueUserWorkItem",
    "SetThreadDescription",
};

#define BEER_KERNEL32_THREAD_EXPORT_COUNT \
    (sizeof(beer_kernel32_thread_exports) / sizeof(beer_kernel32_thread_exports[0]))

static const char *const beer_kernel32_string_exports[] = {
    "lstrcatA",
    "lstrcmpA",
    "lstrcmpW",
    "lstrcmpiA",
    "lstrcmpiW",
    "lstrcpyA",
    "lstrlenA",
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

/* x64 Kernel32 publishes the runtime-function and unwind routines as PE
 * forwarders into ntdll. Run-time code generators look them up through
 * Kernel32 and expect the result inside ntdll, not a free-standing thunk. */
static const char *const beer_kernel32_runtime_function_exports[] = {
    "RtlAddFunctionTable",
    "RtlCaptureContext",
    "RtlCaptureStackBackTrace",
    "RtlDeleteFunctionTable",
    "RtlInstallFunctionTableCallback",
    "RtlLookupFunctionEntry",
    "RtlPcToFileHeader",
    "RtlRestoreContext",
    "RtlUnwind",
    "RtlUnwindEx",
    "RtlVirtualUnwind",
};

#define BEER_KERNEL32_RUNTIME_FUNCTION_EXPORT_COUNT \
    (sizeof(beer_kernel32_runtime_function_exports) / \
     sizeof(beer_kernel32_runtime_function_exports[0]))

/* Documented Kernel32/KernelBase APIs that the MSVC C/C++ runtimes resolve at
 * run time with GetProcAddress (fiber-local storage, synchronization, thread
 * pool, locale-name APIs) instead of importing them. A runtime copy loaded by a
 * protector looks them up in the Kernel32 module and requires the result to lie
 * inside it. Beer publishes the ones it implements from both modules. */
static const char *const beer_kernel32_crt_runtime_exports[] = {
    "AcquireSRWLockExclusive",
    "AddDllDirectory",
    "AreFileApisANSI",
    "CloseThreadpoolTimer",
    "CloseThreadpoolWait",
    "CloseThreadpoolWork",
    "CompareStringEx",
    "CreateEventExW",
    "CreateSemaphoreExW",
    "CreateSemaphoreW",
    "CreateSymbolicLinkW",
    "CreateThreadpoolTimer",
    "CreateThreadpoolWait",
    "CreateThreadpoolWork",
    "EnumSystemLocalesEx",
    "FlsAlloc",
    "FlsFree",
    "FlsGetValue",
    "FlsSetValue",
    "FlushProcessWriteBuffers",
    "FreeLibraryWhenCallbackReturns",
    "GetCurrentPackageId",
    "GetCurrentProcessorNumber",
    "GetDateFormatEx",
    "GetFileInformationByHandleEx",
    "GetLocaleInfoEx",
    "GetSystemTimePreciseAsFileTime",
    "GetTickCount64",
    "GetTimeFormatEx",
    "GetUserDefaultLocaleName",
    "InitOnceExecuteOnce",
    "InitializeConditionVariable",
    "InitializeCriticalSectionEx",
    "InitializeSRWLock",
    "IsValidLocaleName",
    "LCIDToLocaleName",
    "LCMapStringEx",
    "LocaleNameToLCID",
    "ReleaseSRWLockExclusive",
    "SetFileInformationByHandle",
    "SetThreadpoolTimer",
    "SetThreadpoolWait",
    "SleepConditionVariableCS",
    "SleepConditionVariableSRW",
    "SubmitThreadpoolWork",
    "TryAcquireSRWLockExclusive",
    "WaitForThreadpoolTimerCallbacks",
    "WakeAllConditionVariable",
    "WakeConditionVariable",
};

#define BEER_KERNEL32_CRT_RUNTIME_EXPORT_COUNT \
    (sizeof(beer_kernel32_crt_runtime_exports) / \
     sizeof(beer_kernel32_crt_runtime_exports[0]))

/* Console input APIs. Beer gives the process no console; the implementations
 * report that truthfully, and the names are published so import resolvers find
 * them in Kernel32 regardless of which DLLs the process happens to load. */
static const char *const beer_kernel32_console_exports[] = {
    "ReadConsoleA",
    "ReadConsoleW",
};

#define BEER_KERNEL32_CONSOLE_EXPORT_COUNT \
    (sizeof(beer_kernel32_console_exports) / \
     sizeof(beer_kernel32_console_exports[0]))

enum {
    BEER_APP_POLICY_THREAD_INITIALIZATION_NONE = 0,
    BEER_APP_POLICY_THREAD_INITIALIZATION_INITIALIZE_WINRT = 1,
};

static inline long beer_app_policy_get_thread_initialization_type(
    int32_t *initialization_type)
{
    if (!initialization_type) return 87; /* ERROR_INVALID_PARAMETER */
    *initialization_type = BEER_APP_POLICY_THREAD_INITIALIZATION_NONE;
    return 0; /* ERROR_SUCCESS */
}

static inline int beer_compare_filetime(uint64_t left, uint64_t right)
{
    return left < right ? -1 : left > right ? 1 : 0;
}

#endif
