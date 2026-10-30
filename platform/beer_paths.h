#ifndef BEER_PATHS_H
#define BEER_PATHS_H

/* Where Beer puts scratch files (decoded media, sockets, anonymous sections).
 *
 * Desktop Linux keeps using /tmp exactly as before. Android has no /tmp: an app or Termux session provides a writable
 * temp directory through TMPDIR, so honour that there. BEER_TMPDIR overrides on any platform. */

#include <stdlib.h>

static inline const char *beer_tmp_dir(void)
{
    const char *dir = getenv("BEER_TMPDIR");
    if (dir && *dir) return dir;
#if defined(__ANDROID__)
    dir = getenv("TMPDIR");
    if (dir && *dir) return dir;
#endif
    return "/tmp";
}

#endif
