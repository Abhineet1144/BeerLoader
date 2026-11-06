#include "win32_full_path.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *full(const char *path, const char *cwd)
{
    static char out[1024];
    if (!beer_win32_full_path(path, cwd, out, sizeof(out))) return "<fail>";
    return out;
}

#define EXPECT(path, cwd, want) do { \
    const char *got = full(path, cwd); \
    if (strcmp(got, want)) { fprintf(stderr, "full(\"%s\", \"%s\") = \"%s\", want \"%s\"\n", path, cwd, got, want); return 1; } \
} while (0)

int main(void)
{
    const char *game = "Z:\\games\\Onimusha - Way of the Sword";

    /* Drive-relative paths resolve against that drive's current directory. */
    EXPECT("Z:.", game, "Z:\\games\\Onimusha - Way of the Sword");
    EXPECT("Z:", game, "Z:\\games\\Onimusha - Way of the Sword");
    EXPECT("z:dlc\\a.pak", game, "Z:\\games\\Onimusha - Way of the Sword\\dlc\\a.pak");
    EXPECT("Z:..", game, "Z:\\games");
    EXPECT("C:foo", game, "C:\\foo");                         /* another drive: its root */
    EXPECT("C:", game, "C:\\");

    /* Relative and rooted paths. */
    EXPECT(".", game, "Z:\\games\\Onimusha - Way of the Sword");
    EXPECT("./win64_save", game, "Z:\\games\\Onimusha - Way of the Sword\\win64_save");
    EXPECT("..\\..\\..", game, "Z:\\");
    EXPECT("\\Windows\\System32", game, "Z:\\Windows\\System32");
    EXPECT("/tmp/x", game, "Z:\\tmp\\x");

    /* Absolute paths, separators, dots, trailing separators. */
    EXPECT("C:\\a\\.\\b\\..\\c", game, "C:\\a\\c");
    EXPECT("C:/a//b/", game, "C:\\a\\b\\");                    /* trailing separator kept */
    EXPECT("C:\\a\\b\\.", game, "C:\\a\\b");
    EXPECT("C:\\", game, "C:\\");
    EXPECT("C:\\..", game, "C:\\");
    EXPECT(".\\", game, "Z:\\games\\Onimusha - Way of the Sword\\");

    /* UNC: ".." stops at the share. */
    EXPECT("\\\\server\\share\\a\\..\\..\\b", game, "\\\\server\\share\\b");
    EXPECT("\\\\server\\share", game, "\\\\server\\share");

    /* Verbatim and device paths are returned unchanged. */
    EXPECT("\\\\?\\C:\\a\\..\\b", game, "\\\\?\\C:\\a\\..\\b");
    EXPECT("\\\\.\\PhysicalDrive0", game, "\\\\.\\PhysicalDrive0");

    /* A UNC current directory. */
    EXPECT("x\\..\\y", "\\\\srv\\sh\\dir", "\\\\srv\\sh\\dir\\y");

    /* Too small a buffer fails instead of truncating. */
    char tiny[4];
    assert(beer_win32_full_path("C:\\abc", game, tiny, sizeof(tiny)) == 0);
    assert(beer_win32_full_path("", game, tiny, sizeof(tiny)) == 0);

    puts("full path tests passed");
    return 0;
}
