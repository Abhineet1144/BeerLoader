/* WINMM multimedia timers. One prepared Windows worker serializes callbacks;
 * the registry lock is never held across guest code. All deadlines use the
 * monotonic clock, and synchronous cancellation drains an in-flight callback. */
typedef struct BeerMmTimer {
    struct BeerMmTimer *next;
    u64 callback, user, due_ns;
    u32 id, delay, flags;
    int running, cancelled;
} BeerMmTimer;

static pthread_mutex_t g_mm_timer_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_mm_timer_cv;
static pthread_once_t g_mm_timer_once = PTHREAD_ONCE_INIT;
static BeerMmTimer *g_mm_timers;
static u32 g_mm_timer_id;
static int g_mm_timer_worker_started;
static __thread BeerMmTimer *g_mm_timer_current;

static __attribute__((noinline)) void mm_timer_call(u64 proc, u32 id, u64 user)
{
    register u64 r8 __asm__("r8") = user;
    register u64 r9 __asm__("r9") = 0;
    u64 first = id, second = 0;
    __asm__ volatile(
        "mov %%rsp, %%rbx\n"
        "and $-16, %%rsp\n"
        "sub $0x30, %%rsp\n"
        "movq $0, 0x20(%%rsp)\n"
        "movb $1, %%fs:beer_win32_syscall_selector@tpoff\n"
        "call *%[proc]\n"
        "movb $0, %%fs:beer_win32_syscall_selector@tpoff\n"
        "mov %%rbx, %%rsp\n"
        : "+c"(first), "+d"(second), "+r"(r8), "+r"(r9)
        : [proc] "r"(proc)
        : "rax", "rbx", "r10", "r11", "memory", "cc");
}

static void mm_timer_initialize(void)
{
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&g_mm_timer_cv, &attr);
    pthread_condattr_destroy(&attr);
}

static u64 mm_timer_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

static BeerMmTimer **mm_timer_link(u32 id)
{
    BeerMmTimer **link = &g_mm_timers;
    while (*link && (*link)->id != id) link = &(*link)->next;
    return link;
}

static void mm_timer_remove(BeerMmTimer *timer)
{
    BeerMmTimer **link = mm_timer_link(timer->id);
    if (*link == timer) *link = timer->next;
    free(timer);
    pthread_cond_broadcast(&g_mm_timer_cv);
}

static void *mm_timer_worker(void *unused)
{
    (void)unused;
    pthread_mutex_lock(&g_mm_timer_lock);
    for (;;) {
        BeerMmTimer *timer = NULL;
        for (BeerMmTimer *t = g_mm_timers; t; t = t->next)
            if (!timer || t->due_ns < timer->due_ns) timer = t;
        if (!timer) {
            pthread_cond_wait(&g_mm_timer_cv, &g_mm_timer_lock);
            continue;
        }
        u64 now = mm_timer_now();
        if (timer->due_ns > now) {
            struct timespec due = { (time_t)(timer->due_ns / 1000000000ull),
                                    (long)(timer->due_ns % 1000000000ull) };
            pthread_cond_timedwait(&g_mm_timer_cv, &g_mm_timer_lock, &due);
            continue; /* A wake can change which timer expires first. */
        }
        timer->running = 1;
        pthread_mutex_unlock(&g_mm_timer_lock);
        g_mm_timer_current = timer;
        if (timer->flags & 0x10u) impl_SetEvent(timer->callback);
        else if (timer->flags & 0x20u) impl_PulseEvent(timer->callback);
        else {
            mm_timer_call(timer->callback, timer->id, timer->user);
        }
        g_mm_timer_current = NULL;
        pthread_mutex_lock(&g_mm_timer_lock);
        timer->running = 0;
        if (timer->cancelled || !(timer->flags & 1u)) mm_timer_remove(timer);
        else {
            u64 period = (u64)timer->delay * 1000000ull;
            timer->due_ns += period;
            now = mm_timer_now();
            if (timer->due_ns <= now) timer->due_ns = now + period;
            pthread_cond_broadcast(&g_mm_timer_cv);
        }
    }
    return NULL;
}

static u64 __attribute__((ms_abi))
impl_timeSetEvent(u32 delay, u32 resolution, u64 callback, u64 user, u32 flags)
{
    (void)resolution; /* Requested accuracy, not a separate timer period. */
    if (!delay || delay > 1000000u || !callback || (flags & ~0x131u) ||
        (flags & 0x30u) == 0x30u) return 0;
    BeerMmTimer *timer = calloc(1, sizeof(*timer));
    if (!timer) return 0;
    timer->callback = callback;
    timer->user = user;
    timer->delay = delay;
    timer->flags = flags;
    timer->due_ns = mm_timer_now() + (u64)delay * 1000000ull;
    pthread_once(&g_mm_timer_once, mm_timer_initialize);
    pthread_mutex_lock(&g_mm_timer_lock);
    if (!g_mm_timer_worker_started) {
        if (!beer_mf_start_guest_worker(mm_timer_worker, NULL)) {
            pthread_mutex_unlock(&g_mm_timer_lock);
            free(timer);
            return 0;
        }
        g_mm_timer_worker_started = 1;
    }
    do { ++g_mm_timer_id; } while (!g_mm_timer_id || *mm_timer_link(g_mm_timer_id));
    timer->id = g_mm_timer_id;
    timer->next = g_mm_timers;
    g_mm_timers = timer;
    u32 id = timer->id;
    pthread_cond_broadcast(&g_mm_timer_cv);
    pthread_mutex_unlock(&g_mm_timer_lock);
    return id;
}

static u64 __attribute__((ms_abi)) impl_timeKillEvent(u32 id)
{
    pthread_once(&g_mm_timer_once, mm_timer_initialize);
    pthread_mutex_lock(&g_mm_timer_lock);
    BeerMmTimer *timer = *mm_timer_link(id);
    if (!timer || timer->cancelled) {
        pthread_mutex_unlock(&g_mm_timer_lock);
        return 11; /* MMSYSERR_INVALPARAM */
    }
    timer->cancelled = 1;
    int synchronous = (timer->flags & 0x100u) && g_mm_timer_current != timer;
    if (!timer->running) mm_timer_remove(timer);
    else if (synchronous)
        while (*mm_timer_link(id)) pthread_cond_wait(&g_mm_timer_cv, &g_mm_timer_lock);
    pthread_cond_broadcast(&g_mm_timer_cv);
    pthread_mutex_unlock(&g_mm_timer_lock);
    return 0;
}