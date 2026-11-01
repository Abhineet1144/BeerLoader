#ifndef BEER_WIN32_COMMAND_LINE_H
#define BEER_WIN32_COMMAND_LINE_H
#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <string.h>

/* Portable Shell32 parsing. Pointers and strings share one owned allocation. */
static int beer_command_space(uint16_t c) { return c == ' ' || c == '\t'; }
static size_t beer_command_parse(const uint16_t *s, uint16_t **args, uint16_t *text)
{
    size_t count = 0, out = 0;
#define BEER_COMMAND_PUT(c) do { if (text) text[out] = (c); ++out; } while (0)
    /* The executable token has special quoting: backslashes are literal. */
    if (args) args[count] = text + out;
    ++count;
    if (*s == '"') {
        ++s;
        while (*s && *s != '"') { BEER_COMMAND_PUT(*s); ++s; }
        if (*s) ++s;
    } else {
        while (*s && !beer_command_space(*s)) { BEER_COMMAND_PUT(*s); ++s; }
    }
    BEER_COMMAND_PUT(0);
    while (beer_command_space(*s)) ++s;
    while (*s) {
        int quoted = 0;
        if (args) args[count] = text + out;
        ++count;
        while (*s && (quoted || !beer_command_space(*s))) {
            size_t slashes = 0;
            while (*s == '\\') { ++slashes; ++s; }
            if (*s == '"') {
                for (size_t i = 0; i < slashes / 2; ++i) BEER_COMMAND_PUT('\\');
                if (slashes & 1) { BEER_COMMAND_PUT('"'); ++s; }
                else {
                    ++s;
                    if (quoted && *s == '"') { BEER_COMMAND_PUT('"'); ++s; }
                    else quoted = !quoted;
                }
            } else {
                for (size_t i = 0; i < slashes; ++i) BEER_COMMAND_PUT('\\');
                if (!*s || (!quoted && beer_command_space(*s))) break;
                BEER_COMMAND_PUT(*s); ++s;
            }
        }
        BEER_COMMAND_PUT(0);
        while (beer_command_space(*s)) ++s;
    }
#undef BEER_COMMAND_PUT
    return count;
}
static uint16_t **beer_command_line_to_argv(const uint16_t *command,
        const uint16_t *exe_path, int *argc, void *(*allocate)(size_t))
{
    if (!command || !argc || !allocate) return NULL;
    *argc = 0;
    const uint16_t *source = *command ? command : exe_path;
    if (!source) return NULL;
    size_t length = 0;
    while (source[length]) ++length;
    if (length > (SIZE_MAX / sizeof(uint16_t)) - 2) return NULL;
    size_t count = *command ? beer_command_parse(source, NULL, NULL) : 1;
    if (count > INT_MAX || count > SIZE_MAX / sizeof(uint16_t *)) return NULL;
    size_t pointers = count * sizeof(uint16_t *);
    size_t chars = (length + 2) * sizeof(uint16_t);
    if (pointers > SIZE_MAX - chars) return NULL;
    uint16_t **args = allocate(pointers + chars);
    if (!args) return NULL;
    uint16_t *text = (uint16_t *)((unsigned char *)args + pointers);
    if (*command) beer_command_parse(source, args, text);
    else { args[0] = text; memcpy(text, source, (length + 1) * sizeof(uint16_t)); }
    *argc = (int)count;
    return args;
}
#endif