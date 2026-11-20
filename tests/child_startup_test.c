#define _GNU_SOURCE
#include "win32_child.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static char self[4096];
static uint64_t start(int suspended, int *marker, const char *mode, pid_t *pid)
{
    int pipefd[2];
    assert(!pipe2(pipefd, O_CLOEXEC));
    posix_spawn_file_actions_t a;
    assert(!posix_spawn_file_actions_init(&a));
    assert(!posix_spawn_file_actions_adddup2(&a, pipefd[1], 100));
    BeerChildLaunch l;
    assert(beer_child_prepare(&l, &a, 200));
    assert(fcntl(l.parent_fd, F_GETFD) & FD_CLOEXEC);
    assert(fcntl(l.child_fd, F_GETFD) & FD_CLOEXEC);
    char *args[] = {self, (char *)mode, NULL};
    char *env[] = {l.env, NULL};
    assert(!posix_spawn(pid, self, &a, NULL, args, env));
    posix_spawn_file_actions_destroy(&a);
    close(pipefd[1]);
    uint64_t object = 0, thread = 0;
    int adopted = beer_child_adopt(&l, *pid, suspended, &object, &thread);
    if (!strcmp(mode, "fail")) {
        assert(!adopted);
        beer_child_discard(&l);
        assert(waitpid(*pid, NULL, 0) == *pid);
        close(pipefd[0]);
        return 0;
    }
    assert(adopted == 1 && object == thread && beer_primary_namespace(thread));
    beer_child_discard(&l);
    *marker = pipefd[0];
    return thread;
}
static void wait_done(uint64_t h, int process)
{
    for (unsigned i = 0; i < 5000; ++i) {
        if ((process ? beer_child_poll(h, NULL) : beer_primary_poll(h)) == 1) return;
        (void)poll(NULL, 0, 1);
    }
    assert(!"completion timeout");
}
static void release(uint64_t h, int marker)
{
    assert(beer_primary_close(h));
    assert(!beer_primary_close(h));
    uint32_t previous;
    assert(beer_primary_resume(h, &previous) == 6);
    beer_child_release(h);
    close(marker);
}
int main(int argc, char **argv)
{
    alarm(25);
    if (argc > 1) {
        if (!strcmp(argv[1], "fd-check")) {
            assert(argc == 3 && fcntl(atoi(argv[2]), F_GETFD) == -1 && errno == EBADF);
            assert(!getenv("_BEER_CHILD_CONTROL"));
            return 0;
        }
        if (!strcmp(argv[1], "fail")) return 7;
        const char *control = getenv("_BEER_CHILD_CONTROL");
        assert(control);
        char descriptor[32]; snprintf(descriptor, sizeof(descriptor), "%s", control);
        assert(beer_child_capture());
        assert(fcntl(atoi(descriptor), F_GETFD) & FD_CLOEXEC);
        char *helper_args[] = {argv[0], "fd-check", descriptor, NULL};
        char *helper_env[] = {NULL};
        pid_t helper;
        assert(!posix_spawn(&helper, argv[0], NULL, NULL, helper_args, helper_env));
        int status;
        assert(waitpid(helper, &status, 0) == helper && WIFEXITED(status) && !WEXITSTATUS(status));
        if (!beer_child_startup()) return 9;
        assert(!getenv("_BEER_CHILD_CONTROL"));
        assert(write(100, "G", 1) == 1); /* stands for first guest TLS/callback */
        if (!strcmp(argv[1], "thread-exit")) {
            beer_child_primary_exit(0x12345678);
            (void)poll(NULL, 0, 200);
            return 23;
        }
        (void)poll(NULL, 0, 200);
        return 23;
    }
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    assert(n > 0); self[n] = 0;
    setenv("_BEER_CHILD_CONTROL", "invalid", 1);
    assert(!beer_child_startup());
    unsetenv("_BEER_CHILD_CONTROL");
    assert(beer_child_startup());
    uint32_t previous = 999, code;
    assert(beer_primary_resume(0x33000001, &previous) == 6 && previous == 999);
    assert(beer_primary_resume(1, NULL) == 87);
    int marker = -1;
    pid_t pid;
    uint64_t h = start(1, &marker, "child", &pid);
    struct pollfd p = {.fd = marker, .events = POLLIN};
    assert(poll(&p, 1, 100) == 0 && beer_primary_poll(h) == 0 && beer_child_poll(h, NULL) == 0);
    assert(beer_primary_exit_code(h, &code) && code == 259);
    assert(!beer_primary_suspend(h, &previous) && previous == 1);
    assert(!beer_primary_resume(h, &previous) && previous == 2);
    assert(poll(&p, 1, 100) == 0);
    assert(!beer_primary_resume(h, &previous) && previous == 1);
    assert(!beer_primary_resume(h, &previous) && previous == 0);
    assert(beer_primary_suspend(h, &previous) == 50);
    assert(beer_primary_terminate(h, 99) == 50);
    assert(poll(&p, 1, 1000) == 1);
    char b; assert(read(marker, &b, 1) == 1 && b == 'G');
    wait_done(h, 1);
    assert(beer_primary_exit_code(h, &code) && code == 23);
    release(h, marker);
    uint64_t stale = h;
    h = start(1, &marker, "child", &pid);
    assert(h != stale && beer_primary_resume(stale, &previous) == 6);
    assert(!beer_primary_terminate(h, 0xf1234567));
    wait_done(h, 1);
    assert(beer_primary_exit_code(h, &code) && code == 0xf1234567);
    assert(read(marker, &b, 1) == 0);
    release(h, marker);
    h = start(0, &marker, "thread-exit", &pid);
    wait_done(h, 0);
    assert(beer_primary_exit_code(h, &code) && code == 0x12345678);
    assert(beer_child_poll(h, NULL) == 0); /* primary exit != process exit */
    wait_done(h, 1);
    release(h, marker);
    h = start(1, &marker, "child", &pid);
    assert(beer_primary_close(h)); /* close must not resume or kill */
    assert(beer_primary_poll(h) == -1 && beer_child_poll(h, NULL) == 0);
    assert(!beer_child_terminate(h, 44));
    wait_done(h, 1);
    beer_child_release(h); close(marker);
    assert(!start(1, &marker, "fail", &pid));
    puts("child startup/lifecycle: PASS");
    return 0;
}