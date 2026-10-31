#include "win32_kernel32_contract.h"
#include "win32_string_ordinal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int contains(const char *name)
{
    for (size_t i = 0; i < BEER_KERNEL32_LOCALE_EXPORT_COUNT; ++i)
        if (!strcmp(beer_kernel32_locale_exports[i], name)) return 1;
    return 0;
}

int main(void)
{
    assert(BEER_KERNEL32_LOCALE_EXPORT_COUNT == 20);
    assert(contains("GetUserPreferredUILanguages"));
    assert(contains("CompareStringEx"));
    assert(contains("CompareStringOrdinal"));
    assert(contains("GetLocaleInfoEx"));
    assert(contains("LCMapStringEx"));
    assert(contains("LocaleNameToLCID"));
    assert(BEER_KERNEL32_ENVIRONMENT_EXPORT_COUNT == 6);
    assert(!strcmp(beer_kernel32_environment_exports[0], "ExpandEnvironmentStringsA"));
    assert(!strcmp(beer_kernel32_environment_exports[2], "GetEnvironmentVariableA"));
    assert(!strcmp(beer_kernel32_environment_exports[5], "SetEnvironmentVariableW"));
    assert(BEER_KERNEL32_THREAD_EXPORT_COUNT == 3);
    assert(!strcmp(beer_kernel32_thread_exports[0], "AppPolicyGetThreadInitializationType"));
    assert(!strcmp(beer_kernel32_thread_exports[1], "QueueUserWorkItem"));
    assert(!strcmp(beer_kernel32_thread_exports[2], "SetThreadDescription"));
    assert(BEER_KERNEL32_STRING_EXPORT_COUNT == 7);
    assert(!strcmp(beer_kernel32_string_exports[0], "lstrcatA"));
    assert(!strcmp(beer_kernel32_string_exports[1], "lstrcmpA"));
    assert(!strcmp(beer_kernel32_string_exports[4], "lstrcmpiW"));
    assert(!strcmp(beer_kernel32_string_exports[5], "lstrcpyA"));
    assert(!strcmp(beer_kernel32_string_exports[6], "lstrlenA"));
    assert(BEER_KERNEL32_STRING_PROBE_EXPORT_COUNT == 2);
    assert(!strcmp(beer_kernel32_string_probe_exports[0], "IsBadStringPtrA"));
    assert(!strcmp(beer_kernel32_string_probe_exports[1], "IsBadStringPtrW"));
    assert(BEER_KERNEL32_LOCAL_MEMORY_EXPORT_COUNT == 6);
    assert(!strcmp(beer_kernel32_local_memory_exports[0], "LocalAlloc"));
    assert(!strcmp(beer_kernel32_local_memory_exports[3], "LocalReAlloc"));
    assert(!strcmp(beer_kernel32_local_memory_exports[5], "LocalUnlock"));
    assert(BEER_KERNEL32_FILE_TIME_EXPORT_COUNT == 7);
    assert(!strcmp(beer_kernel32_file_time_exports[0], "CompareFileTime"));
    assert(!strcmp(beer_kernel32_file_time_exports[3], "GetFileTime"));
    assert(!strcmp(beer_kernel32_file_time_exports[6], "SystemTimeToFileTime"));
    assert(BEER_KERNEL32_FILE_SEARCH_EXPORT_COUNT == 7);
    assert(!strcmp(beer_kernel32_file_search_exports[0], "FindClose"));
    assert(!strcmp(beer_kernel32_file_search_exports[4], "FindFirstFileW"));
    assert(!strcmp(beer_kernel32_file_search_exports[6], "FindNextFileW"));
    assert(BEER_KERNEL32_RUNTIME_FUNCTION_EXPORT_COUNT == 11);
    assert(!strcmp(beer_kernel32_runtime_function_exports[0], "RtlAddFunctionTable"));
    assert(!strcmp(beer_kernel32_runtime_function_exports[3], "RtlDeleteFunctionTable"));
    assert(!strcmp(beer_kernel32_runtime_function_exports[10], "RtlVirtualUnwind"));
    assert(BEER_KERNEL32_CRT_RUNTIME_EXPORT_COUNT == 49);
    assert(BEER_KERNEL32_CONSOLE_EXPORT_COUNT == 2);
    assert(!strcmp(beer_kernel32_console_exports[1], "ReadConsoleW"));
    for (size_t i = 1; i < BEER_KERNEL32_CRT_RUNTIME_EXPORT_COUNT; ++i)
        assert(strcmp(beer_kernel32_crt_runtime_exports[i - 1],
                      beer_kernel32_crt_runtime_exports[i]) < 0);
    for (size_t i = 1; i < BEER_KERNEL32_RUNTIME_FUNCTION_EXPORT_COUNT; ++i)
        assert(strcmp(beer_kernel32_runtime_function_exports[i - 1],
                      beer_kernel32_runtime_function_exports[i]) < 0);
    int32_t initialization_type = -1;
    assert(beer_app_policy_get_thread_initialization_type(&initialization_type) == 0);
    assert(initialization_type == BEER_APP_POLICY_THREAD_INITIALIZATION_NONE);
    assert(beer_app_policy_get_thread_initialization_type(NULL) == 87);
    assert(beer_compare_filetime(99, 100) == -1);
    assert(beer_compare_filetime(100, 100) == 0);
    assert(beer_compare_filetime(101, 100) == 1);

    for (size_t i = 0; i < BEER_KERNEL32_LOCALE_EXPORT_COUNT; ++i) {
        assert(beer_kernel32_locale_exports[i][0]);
        for (size_t j = i + 1; j < BEER_KERNEL32_LOCALE_EXPORT_COUNT; ++j)
            assert(strcmp(beer_kernel32_locale_exports[i],
                          beer_kernel32_locale_exports[j]));
    }

    static const uint16_t lower[] = {'b', 'e', 'e', 'r', 0};
    static const uint16_t upper[] = {'B', 'E', 'E', 'R', 0};
    static const uint16_t longer[] = {'b', 'e', 'e', 'r', '2', 0};
    assert(beer_compare_string_ordinal(lower, -1, upper, -1, 1) == BEER_CSTR_EQUAL);
    assert(beer_compare_string_ordinal(lower, -1, upper, -1, 0) == BEER_CSTR_GREATER_THAN);
    assert(beer_compare_string_ordinal(lower, 4, longer, 5, 0) == BEER_CSTR_LESS_THAN);
    assert(beer_compare_string_ordinal(NULL, 0, upper, -1, 0) == 0);
    assert(beer_compare_string_ordinal(lower, -2, upper, -1, 0) == 0);

    puts("win32 Kernel32 contract tests passed");
    return 0;
}
