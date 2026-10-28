/* Included into every loader object (see CFLAGS in the Makefile).
 *
 * Beer reads its BEER_* switches with getenv() all over hot paths (Sleep, HeapAlloc, timers, Vulkan entry points).
 * glibc's getenv walks the whole environment with strncmp on every call, which cost RDR2 about a quarter of its CPU
 * time.  BEER_* variables never change after start-up, so their lookups are cached; any other name goes to the real
 * getenv, because the loader itself sets USERPROFILE, APPDATA and friends at run time. */
#ifndef BEER_ENV_H
#define BEER_ENV_H

/* No system includes here: this header is force-included before each file's own _GNU_SOURCE.  The object-like macro
 * turns glibc's own getenv prototype into a matching declaration of beer_getenv. */
char *beer_getenv(const char *name);
#define getenv beer_getenv

#endif
