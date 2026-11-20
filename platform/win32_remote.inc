/* Remote process operations on Beer children: VirtualAllocEx / VirtualProtectEx / VirtualFreeEx / VirtualQueryEx and
 * CreateRemoteThread[Ex] travel over the child's private control socket (platform/win32_child.c) and run inside the
 * child; ReadProcessMemory / WriteProcessMemory copy directly with process_vm_readv/writev (the caller is the child's
 * parent, which Linux permits). Launchers that inject a DLL into a suspended game (CreateProcess CREATE_SUSPENDED,
 * VirtualAllocEx + WriteProcessMemory of the DLL name, CreateRemoteThread at LoadLibraryA, ResumeThread) use this.
 *
 * Thread start addresses: Beer's system-module export stubs are laid out per process (from that process's imports),
 * so a parent address is never executed as is. The parent names the export (module + name) and the child resolves
 * it with its own GetProcAddress. A raw address is accepted only inside memory the parent allocated in that child
 * with VirtualAllocEx (injected code). Anything else fails with ERROR_INVALID_PARAMETER.
 * Contract: Microsoft Learn VirtualAllocEx, CreateRemoteThreadEx, ReadProcessMemory. */

/* ---- Child side ---- */
typedef struct { u64 base, size; } BeerRemoteRegion;
static BeerRemoteRegion g_remote_regions[64];
static pthread_mutex_t g_remote_region_mtx = PTHREAD_MUTEX_INITIALIZER;

static int beer_remote_region_contains(u64 address)
{
    pthread_mutex_lock(&g_remote_region_mtx);
    int found = 0;
    for (int i = 0; i < 64 && !found; ++i)
        found = g_remote_regions[i].size && address >= g_remote_regions[i].base &&
                address - g_remote_regions[i].base < g_remote_regions[i].size;
    pthread_mutex_unlock(&g_remote_region_mtx);
    return found;
}
static void beer_remote_region_set(u64 base, u64 size)
{
    pthread_mutex_lock(&g_remote_region_mtx);
    int slot = -1;
    for (int i = 0; i < 64; ++i) {
        if (g_remote_regions[i].size && g_remote_regions[i].base == base) { slot = i; break; }
        if (!g_remote_regions[i].size && slot < 0) slot = i;
    }
    if (slot >= 0) g_remote_regions[slot] = (BeerRemoteRegion){ base, size };
    pthread_mutex_unlock(&g_remote_region_mtx);
}

static u32 beer_remote_child_rpc(char op, const uint64_t in[BEER_CHILD_RPC_IN], uint64_t out[BEER_CHILD_RPC_OUT])
{
    g_last_error = 0;
    switch (op) {
    case 'V': {                                                  /* VirtualAlloc(address, size, type, protect) */
        const u64 r = impl_VirtualAlloc((void *)(uintptr_t)in[0], in[1], (u32)in[2], (u32)in[3]);
        out[0] = r;
        if (!r) return g_last_error ? g_last_error : 8;
        beer_remote_region_set(r, in[1]);                       /* a raw remote-thread start may lie here */
        return 0;
    }
    case 'P': {                                                  /* VirtualProtect(address, size, protect) */
        u32 old = 0;
        out[0] = impl_VirtualProtect((void *)(uintptr_t)in[0], in[1], (u32)in[2], &old);
        out[2] = old;
        return out[0] ? 0 : (g_last_error ? g_last_error : 87);
    }
    case 'F':                                                    /* VirtualFree(address, size, type) */
        out[0] = impl_VirtualFree((void *)(uintptr_t)in[0], in[1], (u32)in[2]);
        if (out[0] && ((u32)in[2] & 0x8000u)) beer_remote_region_set(in[0], 0);   /* MEM_RELEASE */
        return out[0] ? 0 : (g_last_error ? g_last_error : 87);
    case 'Q': {                                                  /* VirtualQuery(address) */
        BeerMemoryBasicInformation info;
        memset(&info, 0, sizeof(info));
        out[0] = impl_VirtualQuery((void *)(uintptr_t)in[0], &info, sizeof(info));
        if (!out[0]) return g_last_error ? g_last_error : 87;
        out[0] = info.base_address;
        out[2] = info.allocation_base;
        out[3] = info.region_size;
        out[4] = (u64)info.allocation_protect | (u64)info.state << 32;
        out[5] = (u64)info.protect | (u64)info.type << 32;
        return 0;
    }
    default:
        return 50;
    }
}

typedef struct { u64 thread; u32 id; } BeerRemoteWatch;
static void *beer_remote_thread_watch(void *arg)
{
    BeerRemoteWatch watch = *(BeerRemoteWatch *)arg;
    free(arg);
    impl_WaitForSingleObject(watch.thread, 0xFFFFFFFFu);
    u32 code = 0;
    impl_GetExitCodeThread(watch.thread, &code);
    impl_CloseHandle(watch.thread);
    beer_child_remote_thread_done(watch.id, code);
    return NULL;
}

/* in[0] raw start, in[1] parameter, in[2] module base (0: none), in[3..8] export name. */
static u32 beer_remote_child_start_thread(const uint64_t in[BEER_CHILD_RPC_IN], uint32_t id)
{
    char name[49];
    memcpy(name, &in[3], 48);
    name[48] = 0;
    u64 start = 0;
    if (name[0]) {
        if (in[2]) start = impl_GetProcAddress(in[2], name);
        static const u64 fallbacks[] = { HMOD_K32, HMOD_KBASE, HMOD_NTDLL, HMOD_USER32, HMOD_ADVAPI };
        for (size_t i = 0; !start && i < sizeof(fallbacks) / sizeof(*fallbacks); ++i)
            start = impl_GetProcAddress(fallbacks[i], name);
        fprintf(stderr, "[REMOTE] thread %u: %s -> %#llx (parameter %#llx)\n", id, name, (unsigned long long)start,
                (unsigned long long)in[1]);
        if (!start) return 127;                                  /* ERROR_PROC_NOT_FOUND */
    } else {
        if (!beer_remote_region_contains(in[0])) {
            fprintf(stderr, "[REMOTE] thread %u: start %#llx is not in memory the parent allocated here; refused\n", id,
                    (unsigned long long)in[0]);
            return 87;
        }
        start = in[0];
    }
    u32 tid = 0;
    const u64 thread = impl_CreateThread(0, 0, start, in[1], 0, &tid);
    if (!thread) return g_last_error ? g_last_error : 8;
    BeerRemoteWatch *watch = malloc(sizeof(*watch));
    pthread_t watcher;
    if (!watch) { impl_CloseHandle(thread); return 8; }
    *watch = (BeerRemoteWatch){ thread, id };
    if (pthread_create(&watcher, NULL, beer_remote_thread_watch, watch)) { free(watch); impl_CloseHandle(thread); return 8; }
    pthread_detach(watcher);
    return 0;
}

static const BeerChildHooks g_beer_remote_child_hooks = { beer_remote_child_rpc, beer_remote_child_start_thread };

/* ---- Parent side ---- */
/* Remote thread handles: manual-reset events signalled when the child reports the thread finished. */
typedef struct { int used; u64 token; u32 id; u64 event; u32 code; int done; } BeerRemoteThread;
static BeerRemoteThread g_remote_threads[64];
static pthread_mutex_t g_remote_thread_mtx = PTHREAD_MUTEX_INITIALIZER;

static void beer_remote_thread_finished(uint64_t token, uint32_t id, uint32_t code)
{
    u64 event = 0;
    pthread_mutex_lock(&g_remote_thread_mtx);
    for (int i = 0; i < 64; ++i)
        if (g_remote_threads[i].used && g_remote_threads[i].token == token && g_remote_threads[i].id == id) {
            g_remote_threads[i].code = code;
            g_remote_threads[i].done = 1;
            event = g_remote_threads[i].event;
            break;
        }
    pthread_mutex_unlock(&g_remote_thread_mtx);
    if (event) impl_SetEvent(event);
}
/* GetExitCodeThread for a remote thread handle: 1 when `h` is one. */
static int beer_remote_thread_exit_code(u64 h, u32 *code)
{
    int found = 0;
    pthread_mutex_lock(&g_remote_thread_mtx);
    for (int i = 0; i < 64 && !found; ++i)
        if (g_remote_threads[i].used && g_remote_threads[i].event == h) {
            found = 1;
            if (code) *code = g_remote_threads[i].done ? g_remote_threads[i].code : 259;   /* STILL_ACTIVE */
        }
    pthread_mutex_unlock(&g_remote_thread_mtx);
    return found;
}
static void beer_remote_thread_forget(u64 h)
{
    pthread_mutex_lock(&g_remote_thread_mtx);
    for (int i = 0; i < 64; ++i)
        if (g_remote_threads[i].used && g_remote_threads[i].event == h) g_remote_threads[i].used = 0;
    pthread_mutex_unlock(&g_remote_thread_mtx);
}

/* The control token of a process handle naming a Beer child, or 0. A handle from OpenProcess(child pid) names the
 * same child as the one CreateProcess returned (the unsteam launcher reopens its child by id). */
static u64 beer_remote_token(u64 process)
{
    pthread_mutex_lock(&g_proc_mtx);
    BeerProc *p = beer_proc_from_handle(process);
    u64 token = p && p->child ? p->control : 0;
    for (u32 i = 0; p && !token && i < BEER_MAX_PROCS; ++i)
        if (g_procs[i].used && g_procs[i].control && g_procs[i].pid == p->pid && !g_procs[i].exited)
            token = g_procs[i].control;
    pthread_mutex_unlock(&g_proc_mtx);
    return token;
}
/* The host pid of a Beer child named by `process` (directly or reopened by id), or 0. */
static pid_t beer_remote_pid(u64 process)
{
    const u64 token = beer_remote_token(process);
    return token ? beer_child_pid(token) : 0;
}

/* 1 when `process` is a child (the call was made there; *result and g_last_error set), 0 otherwise. */
static int beer_remote_call(u64 process, char op, const u64 in[BEER_CHILD_RPC_IN], u64 out[BEER_CHILD_RPC_OUT])
{
    const u64 token = beer_remote_token(process);
    if (!token) return 0;
    memset(out, 0, sizeof(u64) * BEER_CHILD_RPC_OUT);
    const int error = beer_child_rpc(token, op, in, out);
    g_last_error = (u32)error;
    if (error == 6 || error == 31) g_last_error = 5;             /* the child is gone: ERROR_ACCESS_DENIED */
    if (error) out[0] = 0;
    return 1;
}

static u64 beer_remote_virtual_alloc(u64 process, void *address, u64 size, u32 type, u32 protect, int *handled)
{
    const u64 in[BEER_CHILD_RPC_IN] = { (u64)(uintptr_t)address, size, type, protect };
    u64 out[BEER_CHILD_RPC_OUT];
    *handled = beer_remote_call(process, 'V', in, out);
    if (*handled)
        fprintf(stderr, "[REMOTE] VirtualAllocEx(%#llx, %#llx) -> %#llx error %u\n", (unsigned long long)process,
                (unsigned long long)size, (unsigned long long)out[0], g_last_error);
    return out[0];
}
static u64 beer_remote_virtual_protect(u64 process, void *address, u64 size, u32 protect, u32 *old, int *handled)
{
    const u64 in[BEER_CHILD_RPC_IN] = { (u64)(uintptr_t)address, size, protect };
    u64 out[BEER_CHILD_RPC_OUT];
    *handled = beer_remote_call(process, 'P', in, out);
    if (*handled && out[0] && old) *old = (u32)out[2];
    return out[0];
}
static u64 beer_remote_virtual_free(u64 process, void *address, u64 size, u32 type, int *handled)
{
    const u64 in[BEER_CHILD_RPC_IN] = { (u64)(uintptr_t)address, size, type };
    u64 out[BEER_CHILD_RPC_OUT];
    *handled = beer_remote_call(process, 'F', in, out);
    return out[0];
}
static u64 beer_remote_virtual_query(u64 process, void *address, BeerMemoryBasicInformation *info, u64 size, int *handled)
{
    const u64 in[BEER_CHILD_RPC_IN] = { (u64)(uintptr_t)address };
    u64 out[BEER_CHILD_RPC_OUT];
    *handled = beer_remote_call(process, 'Q', in, out);
    if (!*handled || g_last_error) return 0;
    if (!info || size < sizeof(*info)) { g_last_error = 24; return 0; }   /* ERROR_BAD_LENGTH */
    memset(info, 0, sizeof(*info));
    info->base_address = out[0];
    info->allocation_base = out[2];
    info->region_size = out[3];
    info->allocation_protect = (u32)out[4];
    info->state = (u32)(out[4] >> 32);
    info->protect = (u32)out[5];
    info->type = (u32)(out[5] >> 32);
    return sizeof(*info);
}

/* The export a parent address names: a Beer system-module export or a GetProcAddress thunk. */
static int beer_remote_export_name(u64 address, u64 *module, char *name, size_t size)
{
    for (size_t m = 0; m < sizeof(g_fake_pe_modules) / sizeof(*g_fake_pe_modules); ++m) {
        const u64 base = g_fake_pe_modules[m].base;
        if (address < base || address - base >= FAKE_PE_SIZE) continue;
        const u8 *image = (const u8 *)(uintptr_t)base;
        const u8 *opt = image + *(const u32 *)(image + 0x3c) + 24;
        const u32 export_rva = *(const u32 *)(opt + 112);
        if (!export_rva) return 0;
        const u8 *directory = image + export_rva;
        const u32 function_count = *(const u32 *)(directory + 20), name_count = *(const u32 *)(directory + 24);
        const u32 *functions = (const u32 *)(image + *(const u32 *)(directory + 28));
        const u32 *names = (const u32 *)(image + *(const u32 *)(directory + 32));
        const u16 *ordinals = (const u16 *)(image + *(const u32 *)(directory + 36));
        for (u32 n = 0; n < name_count; ++n)
            if (ordinals[n] < function_count && functions[ordinals[n]] == (u32)(address - base)) {
                snprintf(name, size, "%s", (const char *)image + names[n]);
                *module = base;
                return 1;
            }
        return 0;
    }
    if (g_tramp && address >= (u64)(uintptr_t)g_tramp && address < (u64)(uintptr_t)g_tramp + (u64)g_nstubs * THUNK_SZ &&
        !((address - (u64)(uintptr_t)g_tramp) % THUNK_SZ)) {
        const char *label = g_snames[(address - (u64)(uintptr_t)g_tramp) / THUNK_SZ];
        if (!label) return 0;
        const char *bang = strchr(label, '!');
        snprintf(name, size, "%s", bang ? bang + 1 : label);
        *module = 0;
        return name[0] != 0;
    }
    return 0;
}

static u64 beer_remote_create_thread(u64 process, u64 start, u64 param, u32 flags, u32 *tid, int *handled)
{
    *handled = 0;
    const u64 token = beer_remote_token(process);
    if (!token) return 0;
    *handled = 1;
    if (flags & 4u) { g_last_error = 50; return 0; }            /* CREATE_SUSPENDED remote threads: not provided */
    u64 in[BEER_CHILD_RPC_IN] = { start, param };
    char name[48];
    u64 module = 0;
    memset(name, 0, sizeof(name));
    if (beer_remote_export_name(start, &module, name, sizeof(name))) {
        in[2] = module;
        memcpy(&in[3], name, 48);
    }
    u64 event = event_alloc(1, 0);
    if (!event) { g_last_error = 8; return 0; }
    u64 out[BEER_CHILD_RPC_OUT];
    /* Register the handle before the request: the completion can arrive before the reply is read. */
    int slot = -1;
    pthread_mutex_lock(&g_remote_thread_mtx);
    for (int i = 0; i < 64 && slot < 0; ++i) if (!g_remote_threads[i].used) slot = i;
    if (slot >= 0) g_remote_threads[slot] = (BeerRemoteThread){ .used = 1, .token = token, .id = 0, .event = event };
    pthread_mutex_unlock(&g_remote_thread_mtx);
    if (slot < 0) { impl_CloseHandle(event); g_last_error = 8; return 0; }
    beer_child_set_remote_done(beer_remote_thread_finished);
    const int error = beer_child_rpc(token, 'C', in, out);
    fprintf(stderr, "[REMOTE] CreateRemoteThread(%#llx, start %#llx = %s, parameter %#llx) -> id %llu error %d\n",
            (unsigned long long)process, (unsigned long long)start, name[0] ? name : "(raw)",
            (unsigned long long)param, (unsigned long long)out[0], error);
    if (error) {
        beer_remote_thread_forget(event);
        impl_CloseHandle(event);
        g_last_error = error == 6 || error == 31 ? 5 : (u32)error;
        return 0;
    }
    pthread_mutex_lock(&g_remote_thread_mtx);
    g_remote_threads[slot].id = (u32)out[0];
    pthread_mutex_unlock(&g_remote_thread_mtx);
    if (tid) *tid = (u32)out[0];
    g_last_error = 0;
    return event;
}

static u64 beer_remote_memory_copy(u64 process, void *remote, void *local, u64 size, u64 *done, int write, int *handled)
{
    *handled = 0;
    const pid_t pid = beer_remote_pid(process);
    if (!pid) return 0;
    *handled = 1;
    if (done) *done = 0;
    if (!size) return 1;
    struct iovec local_vector = { local, (size_t)size }, remote_vector = { remote, (size_t)size };
    ssize_t copied = write ? process_vm_writev(pid, &local_vector, 1, &remote_vector, 1, 0)
                           : process_vm_readv(pid, &local_vector, 1, &remote_vector, 1, 0);
    if (copied < 0) {
        if (errno == EPERM) { g_last_error = 5; return 0; }
        copied = 0;
    }
    if (done) *done = (u64)copied;
    if ((u64)copied != size) { g_last_error = 299; return 0; }  /* ERROR_PARTIAL_COPY */
    return 1;
}
