/* Security-descriptor / token / anonymous-pipe / timer-queue APIs that protection and launcher code (the Razor1911 module among
 * them) calls to decide whether a folder is usable, to run helper programs and to schedule callbacks.  Included from loader.c.
 *
 * Beer has a single user with full access everywhere, so the security calls answer "allowed"; pipes are host pipes wrapped in
 * ordinary file handles; timer-queue timers are host threads that call the guest callback with a proper TEB. */

/* ---- tokens / security descriptors ---- */
static u64 __attribute__((ms_abi)) impl_DuplicateToken(u64 token, u32 level, u64 *out)
{
    (void)level;
    if (!out) { g_last_error = 87; return 0; }
    *out = token + 1;
    return 1;
}
static u64 __attribute__((ms_abi)) impl_DuplicateTokenEx(u64 token, u32 access, void *attributes, u32 level, u32 type, u64 *out)
{
    (void)access; (void)attributes; (void)level; (void)type;
    return impl_DuplicateToken(token, 0, out);
}
/* A self-relative SECURITY_DESCRIPTOR with no owner/group/DACL (everybody allowed). */
static u64 __attribute__((ms_abi)) impl_GetFileSecurityW(const u16 *path, u32 info, u8 *descriptor, u32 length, u32 *needed)
{
    (void)path; (void)info;
    if (needed) *needed = 20;
    if (!descriptor || length < 20) { g_last_error = 122; return 0; }       /* ERROR_INSUFFICIENT_BUFFER */
    memset(descriptor, 0, 20);
    descriptor[0] = 1;                                                       /* Revision */
    *(u16 *)(descriptor + 2) = 0x8000;                                       /* SE_SELF_RELATIVE */
    g_last_error = 0;
    return 1;
}
static u64 __attribute__((ms_abi)) impl_GetFileSecurityA(const char *path, u32 info, u8 *descriptor, u32 length, u32 *needed)
    { return impl_GetFileSecurityW(NULL, info, descriptor, length, needed); (void)path; }
static u64 __attribute__((ms_abi)) impl_AccessCheck(const void *descriptor, u64 token, u32 desired, const void *mapping,
                                                    void *privileges, u32 *privileges_length, u32 *granted, u32 *status)
{
    (void)descriptor; (void)token; (void)mapping; (void)privileges; (void)privileges_length;
    if (granted) *granted = desired;
    if (status) *status = 1;
    g_last_error = 0;
    return 1;
}
/* GENERIC_MAPPING { GenericRead, GenericWrite, GenericExecute, GenericAll } */
static u64 __attribute__((ms_abi)) impl_MapGenericMask(u32 *access, const u32 *mapping)
{
    if (!access || !mapping) return 0;
    u32 value = *access;
    if (value & 0x80000000u) value |= mapping[0];
    if (value & 0x40000000u) value |= mapping[1];
    if (value & 0x20000000u) value |= mapping[2];
    if (value & 0x10000000u) value |= mapping[3];
    *access = value & ~0xF0000000u;
    return 0;
}

/* ---- anonymous pipes ---- */
static u64 __attribute__((ms_abi)) impl_CreatePipe(u64 *read_handle, u64 *write_handle, const void *attributes, u32 size)
{
    (void)attributes; (void)size;
    int fds[2];
    if (!read_handle || !write_handle) { g_last_error = 87; return 0; }
    if (pipe2(fds, O_CLOEXEC) != 0) { g_last_error = 4; return 0; }
    u64 r = fh_alloc(fds[0], "<anonymous pipe>", "");
    u64 w = fh_alloc(fds[1], "<anonymous pipe>", "");
    if (r == INVALID_HANDLE_VALUE64 || w == INVALID_HANDLE_VALUE64) { close(fds[0]); close(fds[1]); return 0; }
    *read_handle = r; *write_handle = w;
    g_last_error = 0;
    return 1;
}
static u64 __attribute__((ms_abi)) impl_SetHandleInformation(u64 handle, u32 mask, u32 flags)
    { (void)handle; (void)mask; (void)flags; return 1; }

/* ---- timer queue ---- */
typedef struct { u64 callback, parameter; u32 due, period, flags; _Atomic(int) cancelled; _Atomic(int) running; } BeerQueueTimer;
static void *beer_queue_timer_thread(void *arg)
{
    BeerQueueTimer *timer = arg;
    void *teb = alloc_teb_for_thread();
    syscall(SYS_arch_prctl, ARCH_SET_GS, (u64)teb);
    typedef void __attribute__((ms_abi)) (*TimerFn)(u64, u8);
    usleep(timer->due * 1000u);
    while (!atomic_load(&timer->cancelled)) {
        ((TimerFn)timer->callback)(timer->parameter, 1);
        if (!timer->period || (timer->flags & 0x8u /* WT_EXECUTEONLYONCE */)) break;
        usleep(timer->period * 1000u);
    }
    atomic_store(&timer->running, 0);
    return NULL;
}
static u64 __attribute__((ms_abi)) impl_CreateTimerQueue(void) { return 0x71710001ull; }
static u64 __attribute__((ms_abi)) impl_CreateTimerQueueTimer(u64 *out, u64 queue, u64 callback, u64 parameter, u32 due, u32 period, u32 flags)
{
    (void)queue;
    if (!out || !callback) { g_last_error = 87; return 0; }
    BeerQueueTimer *timer = calloc(1, sizeof(*timer));
    if (!timer) { g_last_error = 14; return 0; }
    timer->callback = callback; timer->parameter = parameter; timer->due = due; timer->period = period; timer->flags = flags;
    atomic_store(&timer->running, 1);
    pthread_t thread;
    if (pthread_create(&thread, NULL, beer_queue_timer_thread, timer) != 0) { free(timer); g_last_error = 8; return 0; }
    pthread_detach(thread);
    *out = (u64)(uintptr_t)timer;
    g_last_error = 0;
    return 1;
}
static u64 __attribute__((ms_abi)) impl_DeleteTimerQueueTimer(u64 queue, u64 timer_handle, u64 completion)
{
    (void)queue; (void)completion;
    BeerQueueTimer *timer = (BeerQueueTimer *)(uintptr_t)timer_handle;
    if (!timer) { g_last_error = 87; return 0; }
    atomic_store(&timer->cancelled, 1);        /* the (leaked) record stays valid for a thread still inside the callback */
    g_last_error = 0;
    return 1;
}
static u64 __attribute__((ms_abi)) impl_DeleteTimerQueueEx(u64 queue, u64 completion) { (void)queue; (void)completion; return 1; }
