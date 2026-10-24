#include "win32_environment.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
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

int main(void)
{
    test_ansi_block();
    test_wide_block_and_non_ascii_fallback();
    test_empty_block_has_two_terminators();
    puts("Win32 environment block tests passed");
    return 0;
}
