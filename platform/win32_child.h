#ifndef BEER_WIN32_CHILD_H
#define BEER_WIN32_CHILD_H
#include <stdint.h>
#include <spawn.h>
#include <sys/types.h>
/* 0x39 is reserved for remote primary threads; 0x33 is local threads,
 * 0x35/36 processes/snapshots, 0x37 window stations, 0x38 event logs. */
#define BEER_PRIMARY_BASE 0x39000000u
#define BEER_CHILD_ENV "_BEER_CHILD_CONTROL="
typedef struct { int parent_fd, child_fd; char env[64]; } BeerChildLaunch;
int beer_child_prepare(BeerChildLaunch *, posix_spawn_file_actions_t *, int minimum_fd);
void beer_child_discard(BeerChildLaunch *);
/* Takes the parent's fd on success only. Both references must be released:
 * close(thread) and release(object) (the process handle's reference). */
int beer_child_adopt(BeerChildLaunch *, pid_t, int suspended, uint64_t *object, uint64_t *thread);
int beer_child_startup(void);
/* After loader re-exec, before host libraries can spawn helpers. */
int beer_child_capture(void);
void beer_child_primary_exit(uint32_t);
int beer_primary_namespace(uint64_t);
int beer_primary_resume(uint64_t, uint32_t *previous);
int beer_primary_suspend(uint64_t, uint32_t *previous);
int beer_primary_close(uint64_t);
int beer_primary_poll(uint64_t);
int beer_primary_exit_code(uint64_t, uint32_t *);
int beer_primary_terminate(uint64_t, uint32_t);
int beer_child_poll(uint64_t, uint32_t *);
int beer_child_terminate(uint64_t, uint32_t);
void beer_child_release(uint64_t);

/* Remote operations on a child (VirtualAllocEx & co., CreateRemoteThread), served inside the child. */
#define BEER_CHILD_RPC_IN 9                /* CreateRemoteThread: start, parameter, module, 48-byte name */
#define BEER_CHILD_RPC_OUT 6               /* out[0] result, out[1] Win32 error, out[2..5] extra data */
typedef struct {
    /* Memory operations: returns a Win32 error, fills out[0] and out[2..]. */
    uint32_t (*rpc)(char op, const uint64_t in[BEER_CHILD_RPC_IN], uint64_t out[BEER_CHILD_RPC_OUT]);
    /* Starts the guest thread a 'C' request describes; calls beer_child_remote_thread_done when it ends. Returns a
     * Win32 error (0 on success). Called after process initialization only. */
    uint32_t (*start_thread)(const uint64_t in[BEER_CHILD_RPC_IN], uint32_t id);
} BeerChildHooks;
void beer_child_set_hooks(const BeerChildHooks *);
void beer_child_remote_thread_done(uint32_t id, uint32_t code);
int beer_child_pre_entry(void);
void beer_child_set_remote_done(void (*)(uint64_t token, uint32_t id, uint32_t code));
int beer_child_rpc(uint64_t h, char op, const uint64_t in[BEER_CHILD_RPC_IN], uint64_t out[BEER_CHILD_RPC_OUT]);
pid_t beer_child_pid(uint64_t h);
#endif