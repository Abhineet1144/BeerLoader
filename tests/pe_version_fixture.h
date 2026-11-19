#ifndef BEER_TEST_PE_VERSION_FIXTURE_H
#define BEER_TEST_PE_VERSION_FIXTURE_H

#include <assert.h>
#include <stdint.h>
#include <string.h>

static void version_fixture_u16(uint8_t *p, uint16_t value) { memcpy(p, &value, 2); }
static void version_fixture_u32(uint8_t *p, uint32_t value) { memcpy(p, &value, 4); }

/* Independent PE fixture: one .rsrc section and a binary VS_FIXEDFILEINFO,
 * with file/product version 2.3.4.5, not Beer's synthetic system version. */
static void version_fixture_build(uint8_t image[1024], int pe32plus)
{
    memset(image, 0, 1024);
    version_fixture_u16(image, 0x5a4d);
    version_fixture_u32(image + 0x3c, 0x80);
    version_fixture_u32(image + 0x80, 0x4550);
    version_fixture_u16(image + 0x84, pe32plus ? 0x8664 : 0x14c);
    version_fixture_u16(image + 0x86, 1);
    size_t opt = 0x98, optional_size = pe32plus ? 240 : 224;
    size_t dirs = pe32plus ? 112 : 96;
    version_fixture_u16(image + 0x94, optional_size);
    version_fixture_u16(image + opt, pe32plus ? 0x20b : 0x10b);
    version_fixture_u32(image + opt + dirs - 4, 16);
    version_fixture_u32(image + opt + dirs + 16, 0x1000);
    version_fixture_u32(image + opt + dirs + 20, 512);
    uint8_t *section = image + opt + optional_size;
    memcpy(section, ".rsrc", 5);
    version_fixture_u32(section + 8, 512);
    version_fixture_u32(section + 12, 0x1000);
    version_fixture_u32(section + 16, 512);
    version_fixture_u32(section + 20, 512);
    uint8_t *root = image + 512;
    version_fixture_u16(root + 14, 1);
    version_fixture_u32(root + 16, 16); /* RT_VERSION */
    version_fixture_u32(root + 20, 0x80000020);
    version_fixture_u16(root + 32 + 14, 1);
    version_fixture_u32(root + 48, 1);
    version_fixture_u32(root + 52, 0x80000040);
    version_fixture_u16(root + 64 + 14, 1);
    version_fixture_u32(root + 80, 1033);
    version_fixture_u32(root + 84, 96);
    version_fixture_u32(root + 96, 0x1080);
    version_fixture_u32(root + 100, 92);
    uint8_t *block = root + 128;
    version_fixture_u16(block, 92);
    version_fixture_u16(block + 2, 52);
    static const char key[] = "VS_VERSION_INFO";
    for (size_t i = 0; i < sizeof(key); ++i) version_fixture_u16(block + 6 + 2 * i, key[i]);
    const uint32_t fixed[13] = {0xfeef04bd, 0x10000, 0x20003, 0x40005, 0x20003, 0x40005,
                                0x3f, 0, 0x40004, 2, 0, 0, 0};
    memcpy(block + 40, fixed, sizeof(fixed));
}

#endif