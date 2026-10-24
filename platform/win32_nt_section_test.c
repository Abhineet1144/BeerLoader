#include "win32_nt_section.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void test_anonymous_section_is_shared(void)
{
    uint64_t section = 0;
    assert(beer_nt_section_create(&section, -1, 8192, 0x04, 0x08000000) == 0);
    assert(section != 0);

    void *first = NULL;
    void *second = NULL;
    size_t first_size = 0;
    size_t second_size = 4096;
    assert(beer_nt_section_map(section, &first, 0, NULL, &first_size, 1, 0, 0x04) == 0);
    assert(first && first_size == 8192);
    assert(beer_nt_section_map(section, &second, 0, NULL, &second_size, 1, 0, 0x04) == 0);
    assert(second && second_size == 4096);

    memcpy(first, "shared", 7);
    assert(memcmp(second, "shared", 7) == 0);
    assert(beer_nt_section_unmap(first) == 0);
    assert(beer_nt_section_unmap(second) == 0);
    assert(beer_nt_section_close(section));
    assert(!beer_nt_section_close(section));
}

static void test_file_backed_section(void)
{
    char path[] = "/tmp/beer-section-test-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    unlink(path);
    assert(write(fd, "payload", 7) == 7);

    uint64_t section = 0;
    assert(beer_nt_section_create(&section, fd, 0, 0x02, 0x08000000) == 0);
    close(fd);
    void *view = NULL;
    size_t size = 0;
    assert(beer_nt_section_map(section, &view, 0, NULL, &size, 1, 0, 0x02) == 0);
    assert(size == 7 && memcmp(view, "payload", 7) == 0);
    assert(beer_nt_section_unmap(view) == 0);
    assert(beer_nt_section_close(section));
}

static void test_invalid_inputs(void)
{
    uint64_t section = 123;
    assert(beer_nt_section_create(&section, -1, 0, 0x04, 0) == BEER_STATUS_INVALID_PARAMETER);
    assert(section == 0);
    void *view = NULL;
    size_t size = 4096;
    assert(beer_nt_section_map(0xdead, &view, 0, NULL, &size, 1, 0, 0x04) ==
           BEER_STATUS_INVALID_HANDLE);
}

int main(void)
{
    test_anonymous_section_is_shared();
    test_file_backed_section();
    test_invalid_inputs();
    beer_nt_section_reset();
    puts("Win32 NT section tests passed");
    return 0;
}
