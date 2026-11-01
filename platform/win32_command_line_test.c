#include "win32_command_line.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
static void wide(uint16_t *out, const char *s)
{
    do { *out++ = (unsigned char)*s; } while (*s++);
}
static void check(const char *command, int expected, const char **values)
{
    uint16_t input[1024], exe[100];
    wide(input, command); wide(exe, "C:\\Program Files\\app.exe");
    int count = -1;
    uint16_t **args = beer_command_line_to_argv(input, exe, &count, malloc);
    assert(args && count == expected);
    for (int i = 0; i < count; ++i) {
        assert((unsigned char *)args[i] >= (unsigned char *)args + count * sizeof(*args));
        size_t j = 0;
        for (; values[i][j]; ++j) assert(args[i][j] == (unsigned char)values[i][j]);
        assert(!args[i][j]);
    }
    free(args);
}
static void *fail_alloc(size_t n) { (void)n; return NULL; }
int main(void)
{
    check("", 1, (const char *[]){"C:\\Program Files\\app.exe"});
    check("app.exe one two", 3, (const char *[]){"app.exe", "one", "two"});
    check("\"C:\\Program Files\\app.exe\" \"two words\" \"\"", 3,
          (const char *[]){"C:\\Program Files\\app.exe", "two words", ""});
    check(" \t app.exe arg \t", 3, (const char *[]){"", "app.exe", "arg"});
    check("\t ", 1, (const char *[]){""});
    check("app ab\"cd ef\"gh", 2, (const char *[]){"app", "abcd efgh"});
    check("app a\\\"b a\\\\\"b c\" a\\\\\\\"b", 4,
          (const char *[]){"app", "a\"b", "a\\b c", "a\\\"b"});
    check("app tail\\\\", 2, (const char *[]){"app", "tail\\\\"});
    check("app \"unfinished word", 2, (const char *[]){"app", "unfinished word"});
    uint16_t input[] = {'p', ' ', 0xd83d, 0xde00, 0};
    int count;
    uint16_t **a = beer_command_line_to_argv(input, input, &count, malloc);
    uint16_t **b = beer_command_line_to_argv(input, input, &count, malloc);
    assert(a && b && a != b && count == 2 && a[1][0] == 0xd83d && a[1][1] == 0xde00);
    a[1][0] = 'X'; assert(b[1][0] == 0xd83d && input[2] == 0xd83d);
    free(a); free(b);
    assert(!beer_command_line_to_argv(NULL, input, &count, malloc));
    assert(!beer_command_line_to_argv(input, input, NULL, malloc));
    assert(!beer_command_line_to_argv(input, input, &count, fail_alloc) && !count);
    puts("Shell32 command-line parsing: PASS");
}