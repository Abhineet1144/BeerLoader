#ifndef BEER_WIN32_PROCESS_PARAMETERS_H
#define BEER_WIN32_PROCESS_PARAMETERS_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    uint16_t Length;
    uint16_t MaximumLength;
    uint32_t Padding;
    uint64_t Buffer;
} BeerProcessUnicodeString;

/* AMD64 RTL_USER_PROCESS_PARAMETERS prefix. Absolute string/environment
 * pointers require RTL_USER_PROC_PARAMS_NORMALIZED in Flags. */
typedef struct {
    uint32_t MaximumLength;                  /* +0x000 */
    uint32_t Length;                         /* +0x004 */
    uint32_t Flags;                          /* +0x008 */
    uint32_t DebugFlags;                     /* +0x00c */
    uint64_t ConsoleHandle;                  /* +0x010 */
    uint32_t ConsoleFlags;                   /* +0x018 */
    uint32_t Padding;
    uint64_t StandardInput;                  /* +0x020 */
    uint64_t StandardOutput;                 /* +0x028 */
    uint64_t StandardError;                  /* +0x030 */
    BeerProcessUnicodeString CurrentDirectoryPath; /* +0x038 */
    uint64_t CurrentDirectoryHandle;         /* +0x048 */
    BeerProcessUnicodeString DllPath;        /* +0x050 */
    BeerProcessUnicodeString ImagePathName;  /* +0x060 */
    BeerProcessUnicodeString CommandLine;    /* +0x070 */
    uint64_t Environment;                    /* +0x080 */
    uint8_t Reserved[0x100 - 0x88];
} BeerProcessParameters;

_Static_assert(sizeof(BeerProcessUnicodeString) == 16, "UNICODE_STRING size");
_Static_assert(offsetof(BeerProcessParameters, Length) == 4, "parameters length");
_Static_assert(offsetof(BeerProcessParameters, Flags) == 8, "parameters flags");
_Static_assert(offsetof(BeerProcessParameters, StandardInput) == 0x20, "stdin offset");
_Static_assert(offsetof(BeerProcessParameters, CurrentDirectoryPath) == 0x38, "cwd offset");
_Static_assert(offsetof(BeerProcessParameters, ImagePathName) == 0x60, "image path offset");
_Static_assert(offsetof(BeerProcessParameters, CommandLine) == 0x70, "command line offset");
_Static_assert(offsetof(BeerProcessParameters, Environment) == 0x80, "environment offset");

static inline int beer_windows_path_from_host(const char *source,
    char *destination, size_t destination_size)
{
    if (!source || !destination || !destination_size) return 0;
    size_t source_length = strlen(source);
    int has_drive = source_length >= 2 &&
                    ((source[0] >= 'A' && source[0] <= 'Z') ||
                     (source[0] >= 'a' && source[0] <= 'z')) &&
                    source[1] == ':';
    size_t prefix = source[0] == '/' && !has_drive ? 2u : 0u;
    if (prefix + source_length + 1 > destination_size) return 0;
    if (prefix) {
        destination[0] = 'Z';
        destination[1] = ':';
    }
    for (size_t index = 0; index <= source_length; ++index)
        destination[prefix + index] = source[index] == '/' ? '\\' : source[index];
    return 1;
}

static inline void beer_process_parameters_initialize(BeerProcessParameters *params,
    uint64_t standard_input, uint64_t standard_output, uint64_t standard_error)
{
    memset(params, 0, sizeof(*params));
    params->MaximumLength = sizeof(*params);
    params->Length = sizeof(*params);
    params->Flags = 1; /* RTL_USER_PROC_PARAMS_NORMALIZED */
    params->StandardInput = standard_input;
    params->StandardOutput = standard_output;
    params->StandardError = standard_error;
}

#endif