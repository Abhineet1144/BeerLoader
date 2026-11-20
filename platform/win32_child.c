#define _GNU_SOURCE
#include "win32_child.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <time.h>

/* One reaper per owned child, independent of either external handle's lifetime.
 * No SIGSTOP: only the initial guest thread is gated, not loader workers. */
/* The control socket carries, parent to child: 'R' (one byte, release the primary thread) and remote-operation
 * requests (op byte + BEER_CHILD_RPC_IN u64s); child to parent: 'A' (startup acknowledged), 'T' + u32 (primary
 * thread exit code), 'r' + BEER_CHILD_RPC_OUT u64s (the reply to the one outstanding request) and 'D' + u32 id +
 * u32 code (a remote thread finished). A listener thread in the parent owns every read after the acknowledgment. */
typedef struct {
    int used, thread_open, process_open, fd, done, forced, primary_done;
    int listening, reply_ready, rpc_busy;
    pthread_t listener;
    pid_t pid;
    uint32_t token, count, code, forced_code, primary_code;
    uint64_t reply[BEER_CHILD_RPC_OUT];
} Child;
static Child children[128];
static uint32_t serial;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t reply_cv = PTHREAD_COND_INITIALIZER;
static int startup_fd = -1;
static int pending_fd = -1;
static _Thread_local int startup_owner;
static pthread_mutex_t send_lock = PTHREAD_MUTEX_INITIALIZER;
static void (*remote_done_hook)(uint64_t token, uint32_t id, uint32_t code);
static const BeerChildHooks *child_hooks;

static int send_all(int fd, const void *data, size_t size)
{
    size_t sent = 0;
    while (sent < size) {
        ssize_t n = send(fd, (const char *)data + sent, size - sent, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        sent += (size_t)n;
    }
    return 1;
}
static int recv_all(int fd, void *data, size_t size)
{
    size_t got = 0;
    while (got < size) {
        ssize_t n = recv(fd, (char *)data + got, size - got, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        got += (size_t)n;
    }
    return 1;
}
static int send_packet(int fd, const void *data, size_t size)
{
    pthread_mutex_lock(&send_lock);
    const int ok = send_all(fd, data, size);
    pthread_mutex_unlock(&send_lock);
    return ok;
}
void beer_child_primary_exit(uint32_t code)
{
    if (!startup_owner || startup_fd < 0) return;
    unsigned char packet[5] = {'T'};
    memcpy(packet + 1, &code, 4);
    send_packet(startup_fd, packet, sizeof(packet));      /* the socket stays open for remote operations */
}
/* The listener records primary exits as they arrive. */
static void primary_refresh(Child *c) { (void)c; }
static Child *lookup(uint64_t h, int thread)
{
    for (unsigned i = 0; i < 128; ++i)
        if (children[i].used && children[i].token == h && (!thread || children[i].thread_open))
            return &children[i];
    return NULL;
}
int beer_primary_namespace(uint64_t h)
{ return !(h >> 32) && (h & 0xff000000u) == BEER_PRIMARY_BASE; }
static void collect(Child *c)
{ if (c->done && !c->thread_open && !c->process_open) c->used = 0; }
static void *listen_child(void *arg)
{
    Child *c = arg;
    const int fd = c->fd;
    unsigned char op;
    while (recv_all(fd, &op, 1)) {
        if (op == 'T') {
            uint32_t code;
            if (!recv_all(fd, &code, 4)) break;
            pthread_mutex_lock(&lock);
            if (!c->primary_done) { c->primary_code = code; c->primary_done = 1; }
            pthread_mutex_unlock(&lock);
        } else if (op == 'r') {
            uint64_t reply[BEER_CHILD_RPC_OUT];
            if (!recv_all(fd, reply, sizeof(reply))) break;
            pthread_mutex_lock(&lock);
            memcpy(c->reply, reply, sizeof(reply));
            c->reply_ready = 1;
            pthread_cond_broadcast(&reply_cv);
            pthread_mutex_unlock(&lock);
        } else if (op == 'D') {
            uint32_t done[2];
            if (!recv_all(fd, done, sizeof(done))) break;
            if (remote_done_hook) remote_done_hook(c->token, done[0], done[1]);
        } else break;                                          /* a protocol error ends the channel */
    }
    pthread_mutex_lock(&lock);
    c->listening = 0;
    pthread_cond_broadcast(&reply_cv);                        /* a pending request fails */
    pthread_mutex_unlock(&lock);
    return NULL;
}
static void *reap(void *arg)
{
    Child *c = arg;
    int status = 0;
    pid_t r;
    do { r = waitpid(c->pid, &status, 0); } while (r < 0 && errno == EINTR);
    pthread_join(c->listener, NULL);                          /* EOF: the child's end died with it */
    pthread_mutex_lock(&lock);
    primary_refresh(c);
    c->code = c->forced ? c->forced_code :
        (r > 0 && WIFEXITED(status) ? (uint32_t)WEXITSTATUS(status) : 0xc0000005u);
    c->done = 1;
    if (!c->primary_done) { c->primary_done = 1; c->primary_code = c->code; }
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
    collect(c);
    pthread_mutex_unlock(&lock);
    return NULL;
}
int beer_child_prepare(BeerChildLaunch *l, posix_spawn_file_actions_t *a, int minimum_fd)
{
    int pair[2];
    *l = (BeerChildLaunch){ .parent_fd = -1, .child_fd = -1 };
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair)) return 0;
    l->parent_fd = fcntl(pair[0], F_DUPFD_CLOEXEC, minimum_fd);
    close(pair[0]);
    l->child_fd = fcntl(pair[1], F_DUPFD_CLOEXEC, minimum_fd);
    close(pair[1]);
    if (l->parent_fd < 0 || l->child_fd < 0) { beer_child_discard(l); return 0; }
    /* dup2(fd, fd) in spawn deliberately removes CLOEXEC in the child only.
     * The parent's descriptors remain CLOEXEC throughout, including failures. */
    int rc = posix_spawn_file_actions_adddup2(a, l->child_fd, l->child_fd);
    if (!rc) rc = posix_spawn_file_actions_addclose(a, l->parent_fd);
    if (rc) { beer_child_discard(l); return 0; }
    snprintf(l->env, sizeof(l->env), BEER_CHILD_ENV "%d", l->child_fd);
    return 1;
}
void beer_child_discard(BeerChildLaunch *l)
{
    if (l->parent_fd >= 0) close(l->parent_fd);
    if (l->child_fd >= 0) close(l->child_fd);
    l->parent_fd = l->child_fd = -1;
}
static int send_byte(int fd, char b)
{
    ssize_t n;
    do { n = send(fd, &b, 1, MSG_NOSIGNAL); } while (n < 0 && errno == EINTR);
    return n == 1;
}
int beer_child_capture(void)
{
    const char *value = getenv("_BEER_CHILD_CONTROL");
    if (!value) return 1;
    char *end;
    errno = 0;
    long parsed = strtol(value, &end, 10);
    if (errno || !*value || *end || parsed < 3 || parsed > INT_MAX) return 0;
    int fd = (int)parsed, type = 0;
    socklen_t size = sizeof(type);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size) || type != SOCK_STREAM) return 0;
    unsetenv("_BEER_CHILD_CONTROL");
    if (fcntl(fd, F_SETFD, FD_CLOEXEC)) { close(fd); return 0; }
    pending_fd = fd;
    return 1;
}
/* ---- Child side: the primary thread's gate and the remote-operation server ---- */
typedef struct { uint64_t in[BEER_CHILD_RPC_IN]; uint32_t id; } QueuedThread;
static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static int released, init_requested, initialized, channel_lost, queued_count, running_remote;
static uint32_t next_remote_id;
static QueuedThread queued[16];

void beer_child_set_hooks(const BeerChildHooks *hooks) { child_hooks = hooks; }
void beer_child_set_remote_done(void (*hook)(uint64_t, uint32_t, uint32_t)) { remote_done_hook = hook; }

/* Called by the loader's remote-thread runner when one finishes. */
void beer_child_remote_thread_done(uint32_t id, uint32_t code)
{
    pthread_mutex_lock(&gate_lock);
    if (running_remote) --running_remote;
    pthread_cond_broadcast(&gate_cv);
    pthread_mutex_unlock(&gate_lock);
    if (startup_fd < 0) return;
    unsigned char packet[9] = {'D'};
    memcpy(packet + 1, &id, 4);
    memcpy(packet + 5, &code, 4);
    send_packet(startup_fd, packet, sizeof(packet));
}

static void *serve_parent(void *arg)
{
    const int fd = (int)(intptr_t)arg;
    unsigned char op;
    while (recv_all(fd, &op, 1)) {
        if (op == 'R') {
            pthread_mutex_lock(&gate_lock);
            released = 1;
            pthread_cond_broadcast(&gate_cv);
            pthread_mutex_unlock(&gate_lock);
            continue;
        }
        uint64_t in[BEER_CHILD_RPC_IN], out[BEER_CHILD_RPC_OUT];
        if (!recv_all(fd, in, sizeof(in))) break;
        memset(out, 0, sizeof(out));
        if (op == 'C') {
            /* CreateRemoteThread: in = start (already resolved for this process), parameter, suspended flag.
             * Before process initialization the thread is queued: like the first thread of a suspended Windows
             * process, it makes the process initialize, then runs before the primary thread may enter. */
            pthread_mutex_lock(&gate_lock);
            const uint32_t id = ++next_remote_id;
            if (!initialized) {
                if (queued_count < 16) {
                    memcpy(queued[queued_count].in, in, sizeof(queued[0].in));
                    queued[queued_count++].id = id;
                    init_requested = 1;
                    pthread_cond_broadcast(&gate_cv);
                    out[0] = id;
                } else out[1] = 8;
                pthread_mutex_unlock(&gate_lock);
            } else {
                ++running_remote;
                pthread_mutex_unlock(&gate_lock);
                out[0] = id;
                const uint32_t error = child_hooks && child_hooks->start_thread ? child_hooks->start_thread(in, id) : 50;
                if (error) {
                    pthread_mutex_lock(&gate_lock);
                    if (running_remote) --running_remote;
                    pthread_mutex_unlock(&gate_lock);
                    out[0] = 0; out[1] = error;
                }
            }
        } else if (child_hooks && child_hooks->rpc) {
            out[1] = child_hooks->rpc((char)op, in, out);
        } else out[1] = 50;
        unsigned char packet[1 + sizeof(out)];
        packet[0] = 'r';
        memcpy(packet + 1, out, sizeof(out));
        if (!send_packet(fd, packet, sizeof(packet))) break;
    }
    pthread_mutex_lock(&gate_lock);
    channel_lost = 1;                                          /* the parent is gone: startup must fail */
    pthread_cond_broadcast(&gate_cv);
    pthread_mutex_unlock(&gate_lock);
    return NULL;
}

int beer_child_startup(void)
{
    if (!beer_child_capture()) return 0;
    if (pending_fd < 0) return 1;
    int fd = pending_fd;
    pending_fd = -1;
    if (!send_byte(fd, 'A')) { close(fd); return 0; }
    startup_fd = fd;
    startup_owner = 1;
    pthread_t server;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) || pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) ||
        pthread_create(&server, &attr, serve_parent, (void *)(intptr_t)fd)) {
        close(fd); startup_fd = -1; return 0;
    }
    pthread_attr_destroy(&attr);
    pthread_mutex_lock(&gate_lock);
    while (!released && !init_requested && !channel_lost) pthread_cond_wait(&gate_cv, &gate_lock);
    const int ok = released || init_requested;
    pthread_mutex_unlock(&gate_lock);
    return ok; /* CLOEXEC; never carried to a host exec or unrelated child */
}

/* After process initialization, immediately before the executable's TLS callbacks and entry point: runs the remote
 * threads queued while the process was suspended and waits for them, then waits for ResumeThread. */
int beer_child_pre_entry(void)
{
    if (startup_fd < 0) return 1;
    pthread_mutex_lock(&gate_lock);
    initialized = 1;
    QueuedThread run[16];
    const int count = queued_count;
    memcpy(run, queued, sizeof(run));
    queued_count = 0;
    running_remote += count;
    pthread_mutex_unlock(&gate_lock);
    for (int i = 0; i < count; ++i)
        if (!child_hooks || !child_hooks->start_thread || child_hooks->start_thread(run[i].in, run[i].id))
            beer_child_remote_thread_done(run[i].id, 0xc0000139u);   /* STATUS_ENTRYPOINT_NOT_FOUND */
    pthread_mutex_lock(&gate_lock);
    /* A queued thread (LoadLibrary of an injected DLL) finishes before the executable starts, as the loader lock
     * orders it on Windows; one that keeps running stops holding back the entry after 10 seconds. */
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 10;
    while (running_remote && !channel_lost)
        if (pthread_cond_timedwait(&gate_cv, &gate_lock, &deadline) == ETIMEDOUT) break;
    while (!released && !channel_lost) pthread_cond_wait(&gate_cv, &gate_lock);
    const int ok = released;
    pthread_mutex_unlock(&gate_lock);
    return ok;
}

/* Parent side: one request to the child's server, answered while the child lives. Returns a Win32 error. */
int beer_child_rpc(uint64_t h, char op, const uint64_t in[BEER_CHILD_RPC_IN], uint64_t out[BEER_CHILD_RPC_OUT])
{
    pthread_mutex_lock(&lock);
    Child *c = lookup(h, 0);
    while (c && c->rpc_busy && c->listening) pthread_cond_wait(&reply_cv, &lock);
    if (!c || c->done || !c->listening || c->fd < 0) { pthread_mutex_unlock(&lock); return 6; }
    c->rpc_busy = 1;
    c->reply_ready = 0;
    unsigned char packet[1 + 8 * BEER_CHILD_RPC_IN];
    packet[0] = (unsigned char)op;
    memcpy(packet + 1, in, 8 * BEER_CHILD_RPC_IN);
    int error = send_packet(c->fd, packet, sizeof(packet)) ? 0 : 31;
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 30;
    while (!error && !c->reply_ready && c->listening)
        if (pthread_cond_timedwait(&reply_cv, &lock, &deadline) == ETIMEDOUT) error = 1460;   /* ERROR_TIMEOUT */
    if (!error && !c->reply_ready) error = 31;
    if (!error) {
        memcpy(out, c->reply, sizeof(c->reply));
        error = (int)out[1];
    }
    c->rpc_busy = 0;
    pthread_cond_broadcast(&reply_cv);
    pthread_mutex_unlock(&lock);
    return error;
}
pid_t beer_child_pid(uint64_t h)
{
    pthread_mutex_lock(&lock);
    Child *c = lookup(h, 0);
    const pid_t pid = c && !c->done ? c->pid : 0;
    pthread_mutex_unlock(&lock);
    return pid;
}
int beer_child_adopt(BeerChildLaunch *l, pid_t pid, int suspended, uint64_t *object, uint64_t *thread)
{
    if (l->child_fd >= 0) close(l->child_fd);
    l->child_fd = -1;
    struct pollfd p = { .fd = l->parent_fd, .events = POLLIN };
    int r;
    do { r = poll(&p, 1, 30000); } while (r < 0 && errno == EINTR);
    char ack = 0;
    if (r <= 0 || recv(l->parent_fd, &ack, 1, MSG_DONTWAIT) != 1 || ack != 'A') return 0;
    pthread_mutex_lock(&lock);
    Child *c = NULL;
    for (unsigned i = 0; i < 128; ++i) if (!children[i].used) { c = &children[i]; break; }
    /* Never recycle a numeric handle: stale handles must not alias new children. */
    if (!c || serial == 0xffffffu) { pthread_mutex_unlock(&lock); return 0; }
    *c = (Child){ .used = 1, .thread_open = 1, .process_open = 1, .pid = pid,
        .fd = l->parent_fd, .token = BEER_PRIMARY_BASE | ++serial, .count = 1 };
    pthread_attr_t attr;
    pthread_t worker;
    if (pthread_create(&c->listener, NULL, listen_child, c)) {
        c->used = 0; l->parent_fd = c->fd; pthread_mutex_unlock(&lock); return 0;
    }
    c->listening = 1;
    int rc = pthread_attr_init(&attr);
    if (!rc) {
        rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (!rc) rc = pthread_create(&worker, &attr, reap, c);
        pthread_attr_destroy(&attr);
    }
    if (rc) {
        /* No reaper: stop the listener (it holds no lock while reading) before the caller kills the child. */
        shutdown(c->fd, SHUT_RDWR);
        pthread_mutex_unlock(&lock);
        pthread_join(c->listener, NULL);
        pthread_mutex_lock(&lock);
        c->used = 0; l->parent_fd = c->fd; pthread_mutex_unlock(&lock); return 0;
    }
    /* The reaper exists before release. If release fails the caller kills and
     * this reaper owns waitpid (avoid a second racing wait in rollback). */
    if (!suspended) {
        if (!send_byte(c->fd, 'R')) {
            kill(pid, SIGKILL);
            c->thread_open = c->process_open = 0;
            l->parent_fd = -1;
            pthread_mutex_unlock(&lock);
            return -1;
        }
        c->count = 0;
    }
    l->parent_fd = -1;
    *object = *thread = c->token;
    pthread_mutex_unlock(&lock);
    return 1;
}
int beer_primary_resume(uint64_t h, uint32_t *previous)
{
    if (!previous) return 87;
    pthread_mutex_lock(&lock);
    Child *c = lookup(h, 1);
    if (c) primary_refresh(c);
    int error = !c ? 6 : c->primary_done ? 31 : 0;
    if (!error && c->count == 1 && !send_byte(c->fd, 'R')) error = 31;
    if (!error) {
        *previous = c->count;
        if (c->count) --c->count;
    }
    pthread_mutex_unlock(&lock);
    return error;
}
int beer_primary_suspend(uint64_t h, uint32_t *previous)
{
    if (!previous) return 87;
    pthread_mutex_lock(&lock);
    Child *c = lookup(h, 1);
    int error = !c ? 6 : (c->done || !c->count) ? 50 : c->count == 127 ? 156 : 0;
    if (!error) { *previous = c->count; ++c->count; }
    pthread_mutex_unlock(&lock);
    return error;
}
int beer_primary_close(uint64_t h)
{
    pthread_mutex_lock(&lock);
    Child *c = lookup(h, 1);
    int ok = c != NULL;
    if (c) { c->thread_open = 0; collect(c); }
    pthread_mutex_unlock(&lock);
    return ok;
}
static int query(uint64_t h, uint32_t *code, int thread)
{
    pthread_mutex_lock(&lock);
    Child *c = lookup(h, thread);
    if (c) primary_refresh(c);
    int state = c ? (thread ? c->primary_done : c->done) : -1;
    if (c && code) *code = state ? (thread ? c->primary_code : c->code) : 259;
    pthread_mutex_unlock(&lock);
    return state;
}
int beer_primary_poll(uint64_t h) { return query(h, NULL, 1); }
int beer_child_poll(uint64_t h, uint32_t *code) { return query(h, code, 0); }
int beer_primary_exit_code(uint64_t h, uint32_t *code)
{ return code && query(h, code, 1) >= 0; }
static int terminate(uint64_t h, uint32_t code, int thread)
{
    pthread_mutex_lock(&lock);
    Child *c = lookup(h, thread);
    /* Killing a running main host thread would kill other guest threads too.
     * Only the not-yet-started primary thread can be terminated faithfully. */
    if (c) primary_refresh(c);
    int error = !c ? 6 : thread && !c->primary_done && !c->count ? 50 : 0;
    if (!error && !c->done && !(thread && c->primary_done)) {
        if (kill(c->pid, SIGKILL)) error = 31;
        else { c->forced = 1; c->forced_code = code; }
    }
    pthread_mutex_unlock(&lock);
    return error;
}
int beer_primary_terminate(uint64_t h, uint32_t code) { return terminate(h, code, 1); }
int beer_child_terminate(uint64_t h, uint32_t code) { return terminate(h, code, 0); }
void beer_child_release(uint64_t h)
{
    pthread_mutex_lock(&lock);
    Child *c = lookup(h, 0);
    if (c) { c->process_open = 0; collect(c); }
    pthread_mutex_unlock(&lock);
}