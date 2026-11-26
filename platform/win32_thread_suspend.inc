/* ---- Thread suspension: SuspendThread / ResumeThread / GetThreadContext / SetThreadContext on running threads ----
 *
 * Windows suspends a thread only at the user/kernel boundary: a thread running user code stops where it is, a thread
 * inside a system call finishes or blocks there and stops before returning to user mode, so a suspended thread never
 * holds a user-mode lock. GetThreadContext then reports its user-mode registers (for a thread in a system call, the
 * state at the call). Runtimes depend on this: CoreCLR's GC suspends threads running managed code, reads RIP and
 * redirects them to a safe point (SetThreadContext); Mono/Boehm scan suspended threads' registers and stacks.
 * Before this, SuspendThread did nothing to a running thread and GetThreadContext returned no registers, so CoreCLR
 * retried forever (Pratfall froze in an online session).
 *
 * Here "user mode" is guest code and "the kernel" is Beer: SuspendThread signals the thread (BEER_SUSPEND_SIGNAL).
 *   - Interrupted in guest code (or in Beer's lock-free stubs: the dispatch trampoline, the syscall-resume stub), the
 *     handler captures the CONTEXT and parks on a futex until ResumeThread; context changes are written back.
 *   - Interrupted inside a Beer implementation, it captures the guest state at the call (return address, guest RSP,
 *     the nonvolatile registers the trampoline saved) and returns at once; the thread parks when the trampoline is
 *     about to return to the guest (beer_suspend_checkpoint), where a changed RIP/RAX becomes the return address /
 *     return value. A thread blocked in a Beer wait stays blocked.
 * "Beer code" is recognised without locks from a snapshot of the host's executable mappings (refreshed by
 * SuspendThread), plus the syscall selector. */

#define BEER_SUSPEND_SIGNAL (SIGRTMIN + 5)
#define BEER_SUSPEND_SLOTS 1024
enum { SUSPEND_RUNNING = 0, SUSPEND_PARKED = 1, SUSPEND_HOST_PENDING = 2 };

typedef struct {
    u32 win_tid;                 /* 0: free */
    pid_t host_tid;
    int count;                   /* Windows suspend count (atomic) */
    u32 state;                   /* SUSPEND_* (atomic, written by the target) */
    u32 acked;                   /* futex: bumped by the target once `context` holds its state */
    u32 resume;                  /* futex: bumped by ResumeThread when the count reaches 0, and by each new request */
    u32 requests;                /* bumped by SuspendThread for each 0 -> 1 suspension (see suspend_park) */
    int dirty;                   /* SetThreadContext changed `context` */
    long long hitch_start_us;                        /* BEER_HITCH_LOG: when the current suspension began */
    u8 context[0x4d0] __attribute__((aligned(16)));
} BeerSuspendSlot;
static BeerSuspendSlot g_suspend_slots[BEER_SUSPEND_SLOTS];
static int g_suspend_slot_count;
static pthread_mutex_t g_suspend_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread BeerSuspendSlot *t_suspend_slot;
__thread u8 g_suspend_pending;   /* checked by the dispatch trampoline before it returns to the guest */

extern char beer_tramp_outer_ret[], beer_dispatch_trampoline_end[];
extern void beer_win32_syscall_resume(void);

static long beer_futex(u32 *word, int op, u32 value, const struct timespec *timeout)
{
    return syscall(SYS_futex, word, op, value, timeout, NULL, 0);
}

/* ---- host executable ranges (lock-free reads from the signal handler) ---- */
typedef struct { uintptr_t lo, hi; } HostRange;
static HostRange g_host_ranges[2][512];
static int g_host_range_count[2];
static int g_host_range_current = -1;   /* atomic: which buffer the handler reads */
static u64 g_host_range_refreshed_ms;

static int host_path_is_guest_image(const char *path)
{
    const size_t n = strlen(path);
    return n > 4 && (!strcasecmp(path + n - 4, ".exe") || !strcasecmp(path + n - 4, ".dll"));
}

/* Executable mappings of the host: the loader, shared libraries, the vDSO. Guest PE images and anonymous code (JIT,
 * Beer's generated thunks) are guest. Called by suspending threads only (not from a signal handler). */
static void beer_host_ranges_refresh(int force)
{
    const u64 now = beer_wait_now_ms();
    if (!force && __atomic_load_n(&g_host_range_current, __ATOMIC_ACQUIRE) >= 0 &&
        now - __atomic_load_n(&g_host_range_refreshed_ms, __ATOMIC_RELAXED) < 500)
        return;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    const int current = __atomic_load_n(&g_host_range_current, __ATOMIC_ACQUIRE);
    const int next = current == 0 ? 1 : 0;
    int count = 0;
    FILE *maps = fopen("/proc/self/maps", "r");
    char line[1024];
    while (maps && fgets(line, sizeof(line), maps) && count < 512) {
        unsigned long lo, hi;
        char perms[8] = {0};
        int path_at = 0;
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %n", &lo, &hi, perms, &path_at) < 3 || perms[2] != 'x') continue;
        char *path = path_at ? line + path_at : line + strlen(line);
        path[strcspn(path, "\n")] = 0;
        const int host = (path[0] == '/' && !host_path_is_guest_image(path)) || !strcmp(path, "[vdso]");
        if (host) g_host_ranges[next][count++] = (HostRange){ lo, hi };
    }
    if (maps) fclose(maps);
    g_host_range_count[next] = count;
    __atomic_store_n(&g_host_range_current, next, __ATOMIC_RELEASE);
    __atomic_store_n(&g_host_range_refreshed_ms, now, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&lock);
}

static int host_range_contains(uintptr_t ip)
{
    const int current = __atomic_load_n(&g_host_range_current, __ATOMIC_ACQUIRE);
    if (current < 0) return 0;
    for (int i = 0; i < g_host_range_count[current]; ++i)
        if (ip >= g_host_ranges[current][i].lo && ip < g_host_ranges[current][i].hi) return 1;
    return 0;
}

/* ---- registry ---- */
static void beer_suspend_register_thread(u32 win_tid)
{
    if (!win_tid) return;
    pthread_mutex_lock(&g_suspend_registry_lock);
    BeerSuspendSlot *slot = NULL;
    for (int i = 0; i < g_suspend_slot_count && !slot; ++i)
        if (g_suspend_slots[i].win_tid == win_tid) slot = &g_suspend_slots[i];
    for (int i = 0; i < g_suspend_slot_count && !slot; ++i)
        if (!g_suspend_slots[i].win_tid) slot = &g_suspend_slots[i];
    if (!slot && g_suspend_slot_count < BEER_SUSPEND_SLOTS) slot = &g_suspend_slots[g_suspend_slot_count++];
    if (slot) {
        memset(slot, 0, sizeof(*slot));
        slot->host_tid = (pid_t)syscall(SYS_gettid);
        __atomic_store_n(&slot->win_tid, win_tid, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_suspend_registry_lock);
    t_suspend_slot = slot;
}

static void beer_suspend_unregister_thread(void)
{
    BeerSuspendSlot *slot = t_suspend_slot;
    if (!slot) return;
    __atomic_store_n(&slot->win_tid, 0, __ATOMIC_RELEASE);
    t_suspend_slot = NULL;
}

static BeerSuspendSlot *suspend_slot_for_tid(u32 win_tid)
{
    if (!win_tid) return NULL;
    for (int i = 0; i < __atomic_load_n(&g_suspend_slot_count, __ATOMIC_ACQUIRE); ++i)
        if (__atomic_load_n(&g_suspend_slots[i].win_tid, __ATOMIC_ACQUIRE) == win_tid) return &g_suspend_slots[i];
    return NULL;
}

/* ---- context capture ---- */
static void suspend_context_from_signal(u8 *context, const ucontext_t *uc)
{
    beer_context_from_ucontext(context, uc);
    *(u32 *)(context + 0x30) = 0x0010001fu;            /* CONTEXT_AMD64 | CONTROL | INTEGER | SEGMENTS | FLOATING_POINT | DEBUG */
    *(u16 *)(context + 0x38) = 0x33;                   /* SegCs */
    *(u16 *)(context + 0x3a) = *(u16 *)(context + 0x3c) = *(u16 *)(context + 0x40) = *(u16 *)(context + 0x42) = 0x2b;
    *(u16 *)(context + 0x3e) = 0x53;                   /* SegFs */
    if (uc->uc_mcontext.fpregs) {
        memcpy(context + 0x100, uc->uc_mcontext.fpregs, 512);   /* FltSave: FXSAVE image */
        *(u32 *)(context + 0x34) = uc->uc_mcontext.fpregs->mxcsr;
    }
}

/* The guest's state at its outermost call into Beer, from the trampoline frame (see beer_dispatch_trampoline):
 * RIP = the return address at the saved guest RSP, RSP just above it, RBX..R15 and XMM6-15 as the guest left them.
 * Volatile registers are not preserved across a call on Windows and read as 0. 0 when no such frame is active. */
static int suspend_context_from_trampoline(u8 *context)
{
    const u64 guest_rsp = g_saved_guest_rsp;
    if (!guest_rsp) return 0;
    const u64 tops[2] = { g_host_call_stack_top, g_host_call_stack_real_top };
    const u8 *frame = NULL;
    for (int i = 0; i < 2 && !frame; ++i) {
        const u64 candidate = (tops[i] & ~(u64)15) - 0x170;
        if (tops[i] && candidate - 8 >= g_host_call_stack_base &&
            *(const u64 *)(uintptr_t)(candidate - 8) == (u64)(uintptr_t)beer_tramp_outer_ret)
            frame = (const u8 *)(uintptr_t)candidate;
    }
    if (!frame) return 0;
    memset(context, 0, 0x4d0);
    *(u32 *)(context + 0x30) = 0x0010001fu;
    *(u16 *)(context + 0x38) = 0x33;
    *(u16 *)(context + 0x3a) = *(u16 *)(context + 0x3c) = *(u16 *)(context + 0x40) = *(u16 *)(context + 0x42) = 0x2b;
    *(u16 *)(context + 0x3e) = 0x53;
    *(u32 *)(context + 0x44) = 0x202;                  /* EFlags: IF */
    *(u32 *)(context + 0x34) = 0x1f80;                 /* MxCsr default */
    *(u32 *)(context + 0x100 + 24) = 0x1f80;           /* FltSave.MxCsr */
    /* Inside the called function, as Windows reports a thread in a system call: RIP in the callee (Beer's dispatch
     * trampoline, never managed code), RSP at the return address. Reporting the caller's return address as RIP told
     * CoreCLR the thread sat at a managed instruction (a [SuppressGCTransition] P/Invoke's call site), so it
     * redirected the thread mid-call with the volatile registers this context cannot know. */
    *(u64 *)(context + 0x98) = guest_rsp;
    *(u64 *)(context + 0xf8) = (u64)(uintptr_t)beer_dispatch_trampoline;
    *(u64 *)(context + 0x90) = *(const u64 *)(frame + 0x120);        /* Rbx */
    *(u64 *)(context + 0xa0) = *(const u64 *)(frame + 0x128);        /* Rbp */
    *(u64 *)(context + 0xa8) = *(const u64 *)(frame + 0x130);        /* Rsi */
    *(u64 *)(context + 0xb0) = *(const u64 *)(frame + 0x138);        /* Rdi */
    for (int r = 0; r < 4; ++r)                                       /* R12-R15 */
        *(u64 *)(context + 0xd8 + r * 8) = *(const u64 *)(frame + 0x140 + r * 8);
    for (int x = 6; x < 16; ++x)                                      /* XMM6-15 into FltSave.XmmRegisters */
        memcpy(context + 0x1a0 + x * 16, frame + 0x20 + x * 16, 16);
    return 1;
}

static void suspend_wait_resumed(BeerSuspendSlot *slot)
{
    for (;;) {
        const u32 ticket = __atomic_load_n(&slot->resume, __ATOMIC_ACQUIRE);
        if (__atomic_load_n(&slot->count, __ATOMIC_ACQUIRE) <= 0) return;
        beer_futex(&slot->resume, FUTEX_WAIT_PRIVATE, ticket, NULL);
    }
}

static void suspend_ack(BeerSuspendSlot *slot, u32 state)
{
    __atomic_store_n(&slot->state, state, __ATOMIC_RELEASE);
    __atomic_add_fetch(&slot->acked, 1, __ATOMIC_ACQ_REL);
    beer_futex(&slot->acked, FUTEX_WAKE_PRIVATE, 0x7fffffff, NULL);
}

/* Parked in the signal handler until the count drops to 0. A thread resumed and suspended again before it got to run
 * is still in here, and the new suspension's signal stays blocked until the handler returns: it is acknowledged from
 * this loop (`seen`: the request count when the handler was entered). Without that the second SuspendThread waited
 * for an answer that could not come and failed, which CoreCLR treats as fatal ("SuspendThread failed with error 5",
 * Pratfall during garbage collections). */
static void suspend_park(BeerSuspendSlot *slot, u32 seen)
{
    for (;;) {
        const u32 ticket = __atomic_load_n(&slot->resume, __ATOMIC_ACQUIRE);
        if (__atomic_load_n(&slot->count, __ATOMIC_ACQUIRE) <= 0) return;
        const u32 requests = __atomic_load_n(&slot->requests, __ATOMIC_ACQUIRE);
        if (requests != seen) {
            seen = requests;
            suspend_ack(slot, SUSPEND_PARKED);
            continue;
        }
        beer_futex(&slot->resume, FUTEX_WAIT_PRIVATE, ticket, NULL);
    }
}

static void beer_suspend_signal(int sig, siginfo_t *info, void *context)
{
    (void)sig; (void)info;
    const int saved_errno = errno;
    const u8 selector = beer_win32_syscall_selector;
    beer_win32_syscall_selector = 0;                   /* the futex below is a host system call */
    ucontext_t *uc = context;
    greg_t *regs = uc->uc_mcontext.gregs;
    BeerSuspendSlot *slot = t_suspend_slot;
    const u32 requests_seen = slot ? __atomic_load_n(&slot->requests, __ATOMIC_ACQUIRE) : 0;
    /* Any state but parked takes a new snapshot: a thread resumed while still inside Beer stays HOST_PENDING until it
     * reaches the trampoline checkpoint, and a later suspension must still be answered (an unanswered SuspendThread
     * fails, which CoreCLR treats as fatal). */
    if (slot && __atomic_load_n(&slot->count, __ATOMIC_ACQUIRE) > 0 &&
        __atomic_load_n(&slot->state, __ATOMIC_ACQUIRE) != SUSPEND_PARKED) {
        const uintptr_t ip = (uintptr_t)regs[REG_RIP];
        const int lock_free_stub =
            (ip >= (uintptr_t)beer_dispatch_trampoline && ip < (uintptr_t)beer_dispatch_trampoline_end) ||
            (ip >= (uintptr_t)beer_win32_syscall_resume && ip < (uintptr_t)beer_win32_syscall_resume + 16);
        if (!lock_free_stub && host_range_contains(ip)) {
            /* Inside a Beer implementation: report the state at the call, park on the way back to the guest. */
            if (!suspend_context_from_trampoline(slot->context)) suspend_context_from_signal(slot->context, uc);
            slot->dirty = 0;
            g_suspend_pending = 1;
            suspend_ack(slot, SUSPEND_HOST_PENDING);
        } else {
            suspend_context_from_signal(slot->context, uc);
            slot->dirty = 0;
            suspend_ack(slot, SUSPEND_PARKED);
            suspend_park(slot, requests_seen);
            if (slot->dirty) {                         /* SetThreadContext while parked */
                const u32 flags = *(const u32 *)(slot->context + 0x30);
                const u64 eflags = (u64)regs[REG_EFL];
                beer_context_to_ucontext(uc, slot->context);
                if (!(flags & 0x1u)) {                 /* CONTEXT_CONTROL not supplied: keep the interrupted ones */
                    regs[REG_EFL] = (greg_t)eflags;
                }
                if ((flags & 0x8u) && uc->uc_mcontext.fpregs) memcpy(uc->uc_mcontext.fpregs, slot->context + 0x100, 416);
            }
            __atomic_store_n(&slot->state, SUSPEND_RUNNING, __ATOMIC_RELEASE);
        }
    }
    if (selector) {                                    /* back to code that ran with guest system calls blocked */
        const uintptr_t ip = (uintptr_t)regs[REG_RIP];
        if (ip >= (uintptr_t)beer_win32_syscall_resume && ip < (uintptr_t)beer_win32_syscall_resume + 16) {
            regs[REG_RIP] = (greg_t)(uintptr_t)beer_win32_syscall_resume;   /* its target is still in resume_rip */
        } else {
            beer_win32_syscall_resume_rip = ip;
            regs[REG_RIP] = (greg_t)(uintptr_t)beer_win32_syscall_resume;
        }
    }
    errno = saved_errno;
}

/* Called by the dispatch trampoline before it returns to the guest while a suspension is pending (rax_slot: the
 * implementation's return value). Parks until resumed; a CONTEXT changed meanwhile moves the return address (Rip)
 * and the return value (Rax). nested: an inner call returning to guest code that runs inside a Beer call, whose
 * state belongs to the outer frame: parks only. */
void beer_suspend_checkpoint(u64 *rax_slot, int nested)
{
    g_suspend_pending = 0;
    BeerSuspendSlot *slot = t_suspend_slot;
    if (!slot) return;
    suspend_wait_resumed(slot);
    if (slot->dirty && !nested && g_saved_guest_rsp) {
        const u8 *context = slot->context;
        const u32 flags = *(const u32 *)(context + 0x30);
        /* A new RIP is where the call returns to (the context described the thread inside the call); the rest
         * of a thread inside Beer cannot be rewritten. */
        const u64 rip = *(const u64 *)(context + 0xf8);
        if ((flags & 0x1u) && rip != (u64)(uintptr_t)beer_dispatch_trampoline)
            *(u64 *)(uintptr_t)g_saved_guest_rsp = rip;
        (void)rax_slot;   /* the return value stays the implementation's: the captured context never knew it */
    }
    slot->dirty = 0;
    __atomic_store_n(&slot->state, SUSPEND_RUNNING, __ATOMIC_RELEASE);
}

static void beer_suspend_install(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = beer_suspend_signal;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    sigemptyset(&action.sa_mask);
    sigaction(BEER_SUSPEND_SIGNAL, &action, NULL);
    beer_host_ranges_refresh(1);
}

/* BEER_SUSPEND_DIAGNOSTICS=1: each suspension (where the thread was caught) and each SetThreadContext. */
static int suspend_diagnostics(void)
{
    static int on = -1;
    if (on < 0) on = getenv("BEER_SUSPEND_DIAGNOSTICS") != NULL;
    return on;
}

/* ---- the API surface ---- */
static u32 suspend_handle_tid(u64 handle)
{
    if (handle == (u64)-2) return (u32)current_windows_thread_id();
    const int id = thread_id_from_handle(handle);
    return id > 0 ? g_threads[id].tid : 0;
}

/* Waits until the target captured its state (acked moved past `before`); 0 if it exited or did not answer. */
static int suspend_wait_ack(BeerSuspendSlot *slot, u32 before)
{
    for (int i = 0; i < 2000; ++i) {                  /* up to ~10 s */
        if (__atomic_load_n(&slot->acked, __ATOMIC_ACQUIRE) != before) return 1;
        const struct timespec slice = { 0, 5 * 1000 * 1000 };
        beer_futex(&slot->acked, FUTEX_WAIT_PRIVATE, before, &slice);
        if (!__atomic_load_n(&slot->win_tid, __ATOMIC_ACQUIRE)) return 0;
    }
    return __atomic_load_n(&slot->acked, __ATOMIC_ACQUIRE) != before;
}

/* SuspendThread on a running thread. Returns the previous count, or (u64)-1 when the thread cannot be suspended;
 * *handled = 0 when the handle is not a registered running thread (the caller keeps its own handling). */
/* BEER_THREAD_SUSPEND=0: the old behaviour (running threads are not suspended), for comparisons. */
static int suspend_enabled(void)
{
    static int on = -1;
    if (on < 0) { const char *v = getenv("BEER_THREAD_SUSPEND"); on = !(v && !strcmp(v, "0")); }
    return on;
}

static u64 beer_suspend_running(u64 handle, int *handled)
{
    *handled = 0;
    if (!suspend_enabled()) return 0;
    BeerSuspendSlot *slot = suspend_slot_for_tid(suspend_handle_tid(handle));
    if (!slot) return 0;
    *handled = 1;
    const int previous = __atomic_fetch_add(&slot->count, 1, __ATOMIC_ACQ_REL);
    if (previous == 0 && beer_hitch_enabled()) slot->hitch_start_us = beer_hitch_now_us();
    if (suspend_diagnostics())
        fprintf(stderr, "[SUSPEND] SuspendThread handle 0x%llx tid %u count %d -> %d\n", (unsigned long long)handle,
                slot->win_tid, previous, previous + 1);
    if (previous > 0) return (u64)previous;
    if (slot == t_suspend_slot) {                      /* itself: parks when this call returns to the guest */
        if (!suspend_context_from_trampoline(slot->context)) memset(slot->context, 0, sizeof(slot->context));
        slot->dirty = 0;
        g_suspend_pending = 1;
        suspend_ack(slot, SUSPEND_HOST_PENDING);
        return 0;
    }
    beer_host_ranges_refresh(0);
    const u32 before = __atomic_load_n(&slot->acked, __ATOMIC_ACQUIRE);
    __atomic_add_fetch(&slot->requests, 1, __ATOMIC_ACQ_REL);          /* answered by a thread still parked */
    __atomic_add_fetch(&slot->resume, 1, __ATOMIC_ACQ_REL);
    beer_futex(&slot->resume, FUTEX_WAKE_PRIVATE, 0x7fffffff, NULL);
    if (syscall(SYS_tgkill, getpid(), slot->host_tid, BEER_SUSPEND_SIGNAL) != 0 || !suspend_wait_ack(slot, before)) {
        __atomic_sub_fetch(&slot->count, 1, __ATOMIC_ACQ_REL);
        g_last_error = 5;                              /* ERROR_ACCESS_DENIED: exited or not answering */
        if (suspend_diagnostics()) fprintf(stderr, "[SUSPEND] tid %u: no answer\n", slot->win_tid);
        return (u64)(u32)-1;
    }
    if (suspend_diagnostics())
        fprintf(stderr, "[SUSPEND] tid %u %s rip 0x%llx rsp 0x%llx\n", slot->win_tid,
                __atomic_load_n(&slot->state, __ATOMIC_ACQUIRE) == SUSPEND_PARKED ? "parked in guest" : "inside Beer",
                (unsigned long long)*(const u64 *)(slot->context + 0xf8),
                (unsigned long long)*(const u64 *)(slot->context + 0x98));
    return 0;
}

static u64 beer_resume_running(u64 handle, int *handled)
{
    *handled = 0;
    if (!suspend_enabled()) return 0;
    BeerSuspendSlot *slot = suspend_slot_for_tid(suspend_handle_tid(handle));
    if (!slot) return 0;
    *handled = 1;
    int previous = __atomic_load_n(&slot->count, __ATOMIC_ACQUIRE);
    while (previous > 0 &&
           !__atomic_compare_exchange_n(&slot->count, &previous, previous - 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    }
    if (suspend_diagnostics())
        fprintf(stderr, "[SUSPEND] ResumeThread handle 0x%llx tid %u count %d -> %d\n", (unsigned long long)handle,
                slot->win_tid, previous, previous > 0 ? previous - 1 : 0);
    if (previous == 1 && beer_hitch_enabled() && slot->hitch_start_us > 0) {
        const long long held = beer_hitch_now_us() - slot->hitch_start_us;
        if (held >= 1000) beer_hitch_note("tid %u suspended %lld.%01lld ms", slot->win_tid, held / 1000, held % 1000 / 100);
    }
    if (previous == 1) {
        __atomic_add_fetch(&slot->resume, 1, __ATOMIC_ACQ_REL);
        beer_futex(&slot->resume, FUTEX_WAKE_PRIVATE, 0x7fffffff, NULL);
    }
    return (u64)(previous > 0 ? previous : 0);
}

/* GetThreadContext / SetThreadContext of a suspended thread: its captured state. 0 when the thread is not one. */
static int beer_suspended_context(u64 handle, u8 *context, int set)
{
    BeerSuspendSlot *slot = suspend_slot_for_tid(suspend_handle_tid(handle));
    if (!slot || slot == t_suspend_slot || __atomic_load_n(&slot->count, __ATOMIC_ACQUIRE) <= 0 ||
        __atomic_load_n(&slot->state, __ATOMIC_ACQUIRE) == SUSPEND_RUNNING)
        return 0;
    const u32 flags = *(const u32 *)(context + 0x30);
    if (!set) {
        /* The requested parts: CONTROL (Rsp/Rip/EFlags/SegCs/SegSs), INTEGER, SEGMENTS, FLOATING_POINT; debug
         * registers read as clear (Beer arms none). */
        if (flags & 0x1u) {
            memcpy(context + 0x98, slot->context + 0x98, 8);
            memcpy(context + 0xf8, slot->context + 0xf8, 8);
            memcpy(context + 0x44, slot->context + 0x44, 4);
            memcpy(context + 0x38, slot->context + 0x38, 2);
            memcpy(context + 0x42, slot->context + 0x42, 2);
        }
        if (flags & 0x2u) {
            memcpy(context + 0x78, slot->context + 0x78, 0x98 - 0x78);   /* Rax..Rbx */
            memcpy(context + 0xa0, slot->context + 0xa0, 0xf8 - 0xa0);   /* Rbp..R15 */
        }
        if (flags & 0x4u) memcpy(context + 0x3a, slot->context + 0x3a, 8);
        if (flags & 0x8u) {
            memcpy(context + 0x100, slot->context + 0x100, 512);
            memcpy(context + 0x34, slot->context + 0x34, 4);
        }
        if ((flags & 0x100010u) == 0x100010u) memset(context + 0x48, 0, 0x30);
        return 1;
    }
    if (suspend_diagnostics())
        fprintf(stderr, "[SUSPEND] SetThreadContext tid %u flags 0x%x rip 0x%llx -> 0x%llx rsp 0x%llx -> 0x%llx (%s)\n",
                slot->win_tid, flags, (unsigned long long)*(const u64 *)(slot->context + 0xf8),
                (unsigned long long)*(const u64 *)(context + 0xf8),
                (unsigned long long)*(const u64 *)(slot->context + 0x98),
                (unsigned long long)*(const u64 *)(context + 0x98),
                __atomic_load_n(&slot->state, __ATOMIC_ACQUIRE) == SUSPEND_PARKED ? "parked in guest" : "inside Beer");
    if (flags & 0x1u) {
        memcpy(slot->context + 0x98, context + 0x98, 8);
        memcpy(slot->context + 0xf8, context + 0xf8, 8);
        memcpy(slot->context + 0x44, context + 0x44, 4);
    }
    if (flags & 0x2u) {
        memcpy(slot->context + 0x78, context + 0x78, 0x98 - 0x78);
        memcpy(slot->context + 0xa0, context + 0xa0, 0xf8 - 0xa0);
    }
    if (flags & 0x8u) {
        memcpy(slot->context + 0x100, context + 0x100, 416);
        memcpy(slot->context + 0x34, context + 0x34, 4);
    }
    *(u32 *)(slot->context + 0x30) = (*(u32 *)(slot->context + 0x30) & ~0xfu) | (flags & 0xbu) | 0x100000u;
    slot->dirty = 1;
    return 1;
}
