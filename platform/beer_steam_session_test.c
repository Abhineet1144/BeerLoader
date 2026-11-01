/* beer_steam_session against a fake provider socket: replies are matched by id, broadcast events
 * in between are skipped, failures are reported as failures. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "beer_steam_session.h"

static int listener;

static void reply_to(int fd, const char *line)
{
    char id[64] = "", op[64] = "";
    const char *p = strstr(line, "\"id\":\"");
    if (p) sscanf(p + 6, "%63[^\"]", id);
    p = strstr(line, "\"op\":\"");
    if (p) sscanf(p + 6, "%63[^\"]", op);
    char out[1024];
    /* A broadcast the client must ignore. */
    dprintf(fd, "{\"event\":\"download\",\"appid\":1,\"state\":\"downloading\",\"id\":\"\"}\n");
    if (!strcmp(op, "game_session") && strstr(line, "\"appid\":\"480\""))
        snprintf(out, sizeof(out), "{\"event\":\"game_session\",\"id\":\"%s\",\"ok\":true,\"appid\":480,"
                 "\"steamid\":\"76561198000000001\",\"account\":\"tester\",\"persona\":\"Tes\\\"ter\",\"owned\":[480,481,1000]}", id);
    else if (!strcmp(op, "game_session"))
        snprintf(out, sizeof(out), "{\"event\":\"game_session\",\"id\":\"%s\",\"ok\":true,\"appid\":999,"
                 "\"steamid\":\"76561198000000001\",\"account\":\"tester\",\"persona\":\"Tes\\\"ter\",\"owned\":[480,481,1000]}", id);
    else if (!strcmp(op, "app_ticket"))
        snprintf(out, sizeof(out), "{\"event\":\"app_ticket\",\"id\":\"%s\",\"ok\":true,\"appid\":480,\"ticket\":\"0a0b0c0d\"}", id);
    else if (!strcmp(op, "encrypted_ticket") && strstr(line, "\"userdata\":\"cafe\""))
        snprintf(out, sizeof(out), "{\"event\":\"encrypted_ticket\",\"id\":\"%s\",\"ok\":true,\"appid\":480,\"ticket\":\"ff00\"}", id);
    else
        snprintf(out, sizeof(out), "{\"event\":\"%s\",\"id\":\"%s\",\"ok\":false,\"error\":\"refused\"}", op, id);
    dprintf(fd, "%s\n", out);
}

static void *serve(void *arg)
{
    (void)arg;
    for (;;) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) return NULL;
        char buf[8192];
        size_t len = 0;
        ssize_t got;
        while ((got = recv(fd, buf + len, sizeof(buf) - 1 - len, 0)) > 0) {
            len += (size_t)got;
            buf[len] = 0;
            char *nl;
            while ((nl = strchr(buf, '\n'))) {
                *nl = 0;
                reply_to(fd, buf);
                len -= (size_t)(nl + 1 - buf);
                memmove(buf, nl + 1, len + 1);
            }
        }
        close(fd);
    }
}

int main(void)
{
    char dir[] = "/tmp/beer-steam-session-XXXXXX";
    assert(mkdtemp(dir));
    setenv("XDG_RUNTIME_DIR", dir, 1);

    char err[256] = "";
    assert(!beer_steam_session_open(480, err, sizeof(err)));       /* no provider yet */
    assert(strstr(err, "not running"));
    assert(!beer_steam_session_active());

    char path[256];
    snprintf(path, sizeof(path), "%s/beer", dir);
    mkdir(path, 0700);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    assert(beer_steam_session_socket_path(addr.sun_path, sizeof(addr.sun_path)));
    listener = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(!bind(listener, (struct sockaddr *)&addr, sizeof(addr)) && !listen(listener, 4));
    pthread_t thread;
    pthread_create(&thread, NULL, serve, NULL);

    assert(beer_steam_session_open(999, err, sizeof(err)));
    assert(beer_steam_session_active());
    assert(!beer_steam_session_owns(999));
    assert(beer_steam_session_open(480, err, sizeof(err)));
    assert(beer_steam_session_active());
    assert(beer_steam_session_steamid() == 76561198000000001ull);
    assert(!strcmp(beer_steam_session_persona(), "Tes\"ter"));
    assert(beer_steam_session_owns(481) && beer_steam_session_owns(1000) && !beer_steam_session_owns(482));

    uint8_t ticket[16];
    assert(beer_steam_session_app_ticket(480, ticket, sizeof(ticket)) == 4 && ticket[0] == 0x0a && ticket[3] == 0x0d);
    assert(beer_steam_session_app_ticket(480, ticket, 2) == 0);     /* does not fit */
    const uint8_t userdata[] = { 0xca, 0xfe };
    assert(beer_steam_session_encrypted_ticket(480, userdata, 2, ticket, sizeof(ticket)) == 2 && ticket[0] == 0xff);
    assert(beer_steam_session_encrypted_ticket(480, NULL, 0, ticket, sizeof(ticket)) == 0);

    unlink(addr.sun_path);
    rmdir(path);
    rmdir(dir);
    puts("steam session client tests passed");
    return 0;
}
