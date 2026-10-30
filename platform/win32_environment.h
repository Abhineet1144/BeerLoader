#ifndef BEER_WIN32_ENVIRONMENT_H
#define BEER_WIN32_ENVIRONMENT_H

#include <stddef.h>
#include <stdint.h>

/* Build caller-owned Windows environment blocks from a NULL-terminated host
 * environment vector. Each block ends with an additional terminator. */
char *beer_win32_environment_block_a(char *const environment[]);
uint16_t *beer_win32_environment_block_w(char *const environment[]);
void beer_win32_environment_block_free(void *block);

/* Windows environment-variable names are case-insensitive. These helpers use
 * the host process environment as Beer's process environment while preserving
 * Win32 lookup and set/delete semantics. */
const char *beer_win32_environment_get(const char *name, char *const environment[]);
int beer_win32_environment_set(const char *name, const char *value);

/* Expand %NAME% references using Windows' case-insensitive environment lookup.
 * The return value includes the terminating NUL, matching
 * ExpandEnvironmentStringsA/W. Unknown references remain unchanged. */
size_t beer_win32_environment_expand_a(const char *source, char *destination,
                                       size_t capacity, char *const environment[]);
size_t beer_win32_environment_expand_w(const uint16_t *source, uint16_t *destination,
                                       size_t capacity, char *const environment[]);

#endif
