#include "win32_environment.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_ansi_block(void)
{
    char first[] = "BEER_ONE=1";
    char second[] = "BEER_TWO=two";
    char *environment[] = {first, second, NULL};
    char *block = beer_win32_environment_block_a(environment);

    assert(block);
    assert(strcmp(block, first) == 0);
    char *next = block + strlen(block) + 1;
    assert(strcmp(next, second) == 0);
    next += strlen(next) + 1;
    assert(next[0] == '\0');
    beer_win32_environment_block_free(block);
}

static void test_wide_block_and_non_ascii_fallback(void)
{
    char entry[] = {'X', '=', (char)0xc3, (char)0xa9, 0};
    char *environment[] = {entry, NULL};
    uint16_t *block = beer_win32_environment_block_w(environment);

    assert(block);
    assert(block[0] == 'X' && block[1] == '=');
    assert(block[2] == '?' && block[3] == '?');
    assert(block[4] == 0 && block[5] == 0);
    beer_win32_environment_block_free(block);
}

static void test_empty_block_has_two_terminators(void)
{
    char *environment[] = {NULL};
    char *ansi = beer_win32_environment_block_a(environment);
    uint16_t *wide = beer_win32_environment_block_w(environment);

    assert(ansi && ansi[0] == 0 && ansi[1] == 0);
    assert(wide && wide[0] == 0 && wide[1] == 0);
    beer_win32_environment_block_free(ansi);
    beer_win32_environment_block_free(wide);
}

static void test_case_insensitive_lookup_and_set_delete(void)
{
    char mixed[] = "Beer_Test_Value=present";
    char *environment[] = {mixed, NULL};
    assert(strcmp(beer_win32_environment_get("BEER_TEST_VALUE", environment),
                  "present") == 0);
    assert(beer_win32_environment_get("missing", environment) == NULL);
    assert(beer_win32_environment_get("bad=name", environment) == NULL);

    const char *name = "BEER_ENVIRONMENT_CONTRACT_TEST";
    assert(beer_win32_environment_set(name, "set"));
    assert(getenv(name) && strcmp(getenv(name), "set") == 0);
    assert(beer_win32_environment_set(name, NULL));
    assert(getenv(name) == NULL);
    assert(!beer_win32_environment_set("", "bad"));
    assert(!beer_win32_environment_set("bad=name", "bad"));
}

static void test_expand_environment_strings(void)
{
    char value[] = "Beer_Path=expanded";
    char *environment[] = {value, NULL};
    char output[64];
    const char *source = "pre-%BEER_PATH%-%MISSING%-post";
    const char *expected = "pre-expanded-%MISSING%-post";
    size_t required = beer_win32_environment_expand_a(source, NULL, 0, environment);
    assert(required == strlen(expected) + 1);
    assert(beer_win32_environment_expand_a(source, output, sizeof(output), environment) == required);
    assert(strcmp(output, expected) == 0);
    assert(beer_win32_environment_expand_a(source, output, required - 1, environment) == required);

    static const uint16_t wide_source[] = {'%', 'b', 'e', 'e', 'r', '_', 'p', 'a', 't', 'h', '%', 0};
    uint16_t wide_output[16];
    assert(beer_win32_environment_expand_w(wide_source, wide_output, 16, environment) == 9);
    static const uint16_t wide_expected[] = {'e', 'x', 'p', 'a', 'n', 'd', 'e', 'd', 0};
    assert(memcmp(wide_output, wide_expected, sizeof(wide_expected)) == 0);
}

int main(void)
{
    test_ansi_block();
    test_wide_block_and_non_ascii_fallback();
    test_empty_block_has_two_terminators();
    test_case_insensitive_lookup_and_set_delete();
    test_expand_environment_strings();
    puts("Win32 environment block tests passed");
    return 0;
}
