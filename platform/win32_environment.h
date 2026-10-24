#ifndef BEER_WIN32_ENVIRONMENT_H
#define BEER_WIN32_ENVIRONMENT_H

#include <stdint.h>

/* Build caller-owned Windows environment blocks from a NULL-terminated host
 * environment vector. Each block ends with an additional terminator. */
char *beer_win32_environment_block_a(char *const environment[]);
uint16_t *beer_win32_environment_block_w(char *const environment[]);
void beer_win32_environment_block_free(void *block);

#endif
