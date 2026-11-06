#ifndef BEER_WIN32_FULL_PATH_H
#define BEER_WIN32_FULL_PATH_H
#include <stddef.h>

/* GetFullPathName's path resolution, on narrow (ASCII/UTF-8) Windows paths:
 *   "rel\x"      -> relative to the current directory
 *   "\x"         -> on the current directory's drive
 *   "C:rel"      -> relative to drive C's current directory (the current directory when it is on C:,
 *                   the drive root otherwise); "C:" and "C:." name that directory itself
 *   "C:\x"       -> absolute
 *   "\\server\share\x" -> UNC; ".." stops at the share
 *   "\\?\..." and "\\.\..." -> returned unchanged
 * '/' counts as a separator. Repeated separators and "." / ".." components are collapsed, and a trailing
 * separator in the input is kept, as Windows does. current_directory is a full Windows path such as
 * "Z:\games\foo" (no trailing separator needed). Returns the length written (excluding the terminator), or 0
 * when the result does not fit in cap or the input is empty. */
size_t beer_win32_full_path(const char *path, const char *current_directory, char *out, size_t cap);

#endif
