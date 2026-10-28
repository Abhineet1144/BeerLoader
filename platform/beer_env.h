/* Included into every loader object (see CFLAGS in the Makefile).
 *
 * Beer reads its BEER_* switches with getenv() all over hot paths (Sleep, HeapAlloc, timers, Vulkan entry points).
 * glibc's getenv walks the whole environment with strncmp on every call, which cost RDR2 about a quarter of its CPU
 * time.  BEER_* variables never change after start-up, so their lookups are cached: a literal name is cached at its
 * call site (one load on the hot path), anything else goes through beer_getenv's table.  Other names go to the real
 * getenv, because the loader itself sets USERPROFILE, APPDATA and friends at run time. */
#ifndef BEER_ENV_H
#define BEER_ENV_H

/* Every source defines _GNU_SOURCE this same way; it has to be set before <stdlib.h>, which must come before the
 * function-like macro below so glibc's own prototype is not rewritten. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdlib.h>

char *beer_getenv(const char *name);
/* state: 0 = not looked up yet, 1 = cached in *value, 2 = not cacheable (not a BEER_* name) */
char *beer_getenv_site(const char *name, char **value, int *state);

#define getenv(name)                                                                                              \
    (__builtin_constant_p(name)                                                                                   \
         ? __extension__({                                                                                        \
               static char *beer_env_value_;                                                                      \
               static int beer_env_state_;                                                                        \
               __atomic_load_n(&beer_env_state_, __ATOMIC_ACQUIRE) == 1                                           \
                   ? beer_env_value_                                                                              \
                   : beer_getenv_site((name), &beer_env_value_, &beer_env_state_);                                \
           })                                                                                                     \
         : beer_getenv(name))

#endif
