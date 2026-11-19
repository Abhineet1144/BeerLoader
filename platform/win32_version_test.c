#include "win32_version.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "../tests/pe_version_fixture.h"

static int extract_fixture(const uint8_t *image, size_t length, uint8_t **data, uint32_t *size)
{
    char path[] = "/tmp/beer-version-fixture-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    FILE *f = fdopen(fd, "wb");
    assert(f && fwrite(image, 1, length, f) == length);
    assert(!fclose(f));
    int ok = beer_pe_version_resource(path, data, size);
    assert(!unlink(path));
    return ok;
}

static void pe_resource_test(void)
{
    for (int bitness = 0; bitness < 2; ++bitness) {
        uint8_t image[1024], *block = NULL;
        uint32_t size = 0;
        version_fixture_build(image, bitness);
        assert(extract_fixture(image, sizeof(image), &block, &size));
        assert(size == 92 && !memcmp(block, image + 640, size));
        const uint16_t root[] = {'\\', 0};
        const void *value = NULL;
        uint32_t length = 0;
        assert(beer_version_query(block, size, root, &value, &length));
        const uint32_t *fixed = value;
        assert(length == 52 && fixed[0] == 0xfeef04bd && fixed[2] == 0x20003 && fixed[3] == 0x40005);
        free(block);
        /* Missing resource type; truncated raw data; invalid header magic. */
        version_fixture_u32(image + 528, 10);
        assert(!extract_fixture(image, sizeof(image), &block, &size));
        version_fixture_build(image, bitness);
        assert(!extract_fixture(image, 700, &block, &size));
        version_fixture_u16(image + 0x98, 0x107);
        assert(!extract_fixture(image, sizeof(image), &block, &size));
        version_fixture_build(image, bitness);
        version_fixture_u32(image + 532, 0x800001ff); /* directory straddles resource limit */
        assert(!extract_fixture(image, sizeof(image), &block, &size));
        version_fixture_build(image, bitness);
        version_fixture_u16(image + 526, 0xffff); /* too many root entries */
        assert(!extract_fixture(image, sizeof(image), &block, &size));
        version_fixture_build(image, bitness);
        version_fixture_u32(image + 596, 510); /* leaf straddles resource limit */
        assert(!extract_fixture(image, sizeof(image), &block, &size));
        version_fixture_build(image, bitness);
        version_fixture_u32(image + 608, 0x11f0); /* value outside raw-backed range */
        assert(!extract_fixture(image, sizeof(image), &block, &size));
        version_fixture_build(image, bitness);
        version_fixture_u32(image + 0x98 + (bitness ? 112 : 96) - 4, 2);
        assert(!extract_fixture(image, sizeof(image), &block, &size));
    }
}

static void malformed_query_test(void)
{
    uint8_t image[1024];
    version_fixture_build(image, 0);
    uint8_t *block = image + 640;
    const uint16_t root[] = {'\\', 0};
    const void *value = NULL;
    uint32_t length = 0;
    version_fixture_u16(block + 2, 53); /* one byte beyond actual binary value */
    assert(!beer_version_query(block, 92, root, &value, &length));
    version_fixture_u16(block + 2, 27);
    version_fixture_u16(block + 4, 1); /* UTF-16 value length also checked in bytes */
    assert(!beer_version_query(block, 92, root, &value, &length));
    version_fixture_build(image, 0);
    version_fixture_u16(block, 38);
    memset(block + 6, 'A', 32); /* no terminated key in this node */
    assert(!beer_version_query(block, 38, root, &value, &length));
    version_fixture_build(image, 0);
    /* A short child key must not be read past its node when a longer path is queried. */
    version_fixture_u16(block, 100);
    version_fixture_u16(block + 92, 8);
    const uint16_t missing[] = {'\\', 'A', 'B', 'C', 'D', 'E', 0};
    assert(!beer_version_query(block, 100, missing, &value, &length));
    assert(!beer_version_query(block, 5, root, &value, &length));
}

int main(void)
{
    pe_resource_test();
    malformed_query_test();
    uint8_t *block = NULL;
    uint32_t size = 0;
    if (!beer_system_version_resource(&block, &size) || !block || size < 64) {
        fprintf(stderr, "failed to build system version resource\n");
        return 1;
    }

    static const uint16_t root[] = {'\\', 0};
    const void *value = NULL;
    uint32_t length = 0;
    if (!beer_version_query(block, size, root, &value, &length) ||
        !value || length != 13 * sizeof(uint32_t)) {
        fprintf(stderr, "failed to query VS_FIXEDFILEINFO\n");
        free(block);
        return 1;
    }

    const uint32_t *fixed = value;
    const int ok = fixed[0] == 0xFEEF04BDu && fixed[1] == 0x00010000u &&
                   fixed[2] == 0x000A0000u && fixed[3] == 0x4A650000u &&
                   fixed[4] == 0x000A0000u && fixed[5] == 0x4A650000u;
    free(block);
    if (!ok) {
        fprintf(stderr, "unexpected synthetic Windows version metadata\n");
        return 1;
    }
    puts("win32 version contract test passed");
    return 0;
}
