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
    assert(!beer_nt_section_is_image(section));
    close(fd);
    void *view = NULL;
    size_t size = 0;
    assert(beer_nt_section_map(section, &view, 0, NULL, &size, 1, 0, 0x02) == 0);
    assert(size == 7 && memcmp(view, "payload", 7) == 0);
    assert(beer_nt_section_unmap(view) == 0);
    assert(beer_nt_section_close(section));
}

static void test_image_section_identity(void)
{
    char path[] = "/tmp/beer-image-section-test-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    unlink(path);
    assert(write(fd, "MZ-image", 8) == 8);

    uint64_t section = 0;
    assert(beer_nt_section_create(&section, fd, 0, 0x02, 0x01000000) == 0);
    close(fd);
    assert(beer_nt_section_is_image(section));
    assert(beer_nt_section_close(section));
    assert(!beer_nt_section_is_image(section));
}


static void test_image_section_layout(void)
{
    /* Minimal PE32+: headers 0x200 bytes, one section with 4 bytes of data at RVA 0x1000, SizeOfImage 0x3000. */
    uint8_t file[0x204];
    memset(file, 0, sizeof(file));
    file[0] = 'M'; file[1] = 'Z';
    uint32_t pe = 0x40, v;
    memcpy(file + 0x3c, &pe, 4);
    memcpy(file + pe, "PE\0\0", 4);
    uint16_t h16 = 1; memcpy(file + pe + 6, &h16, 2);
    h16 = 0xf0; memcpy(file + pe + 20, &h16, 2);
    h16 = 0x20b; memcpy(file + pe + 24, &h16, 2);
    v = 0x3000; memcpy(file + pe + 24 + 56, &v, 4);
    v = 0x200; memcpy(file + pe + 24 + 60, &v, 4);
    uint8_t *sec = file + pe + 24 + 0xf0;
    v = 4; memcpy(sec + 8, &v, 4);
    v = 0x1000; memcpy(sec + 12, &v, 4);
    v = 4; memcpy(sec + 16, &v, 4);
    v = 0x200; memcpy(sec + 20, &v, 4);
    memcpy(file + 0x200, "CODE", 4);

    char path[] = "/tmp/beer-image-layout-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    unlink(path);
    assert(write(fd, file, sizeof(file)) == (ssize_t)sizeof(file));
    uint64_t section = 0;
    assert(beer_nt_section_create(&section, fd, 0, 0x02, 0x01000000) == 0);
    close(fd);
    void *view = NULL;
    size_t size = 0;
    assert(beer_nt_section_map(section, &view, 0, NULL, &size, 1, 0, 0x02) == 0);
    assert(size == 0x3000);
    assert(((uint8_t *)view)[0] == 'M' && memcmp((uint8_t *)view + 0x1000, "CODE", 4) == 0);
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
    test_image_section_identity();
    test_image_section_layout();
    test_invalid_inputs();
    beer_nt_section_reset();
    puts("Win32 NT section tests passed");
    return 0;
}
