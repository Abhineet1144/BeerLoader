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

/* ---- Win32 file mappings on top of the NT section objects (platform/win32_nt_section.c) ----
 * Named page-file mappings are what two processes use to share memory (a launcher and its game, a crack module and its
 * host); they live in /dev/shm as "beer-map-<name>" so every Beer process that opens the same name sees the same bytes. */
static int beer_mapping_shm(const char *name, s64 size, int create, int *existed)
{
    char path[300] = "/beer-map-";
    size_t n = strlen(path);
    for (size_t i = 0; name[i] && n + 1 < sizeof(path); ++i) path[n++] = (name[i] == '/' || name[i] == '\\') ? '_' : name[i];
    path[n] = 0;
    int fd = shm_open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (existed) *existed = 0;
    if (fd < 0 && errno == EEXIST) { fd = shm_open(path, O_RDWR, 0600); if (existed) *existed = 1; }
    else if (fd < 0 && !create) return -1;
    if (fd < 0 && !create) return -1;
    if (fd >= 0 && create && size > 0) {
        struct stat st;
        if (fstat(fd, &st) == 0 && st.st_size < size && ftruncate(fd, size) != 0) { close(fd); return -1; }
    }
    return fd;
}
static u64 beer_create_file_mapping(u64 file, u32 protect, u32 size_high, u32 size_low, const char *name)
{
    s64 size = (s64)(((u64)size_high << 32) | size_low);
    int fd = -1, owned = 0, existed = 0;
    if (file && file != INVALID_HANDLE_VALUE64) {
        fd = fh_get(file);
        if (fd < 0) { g_last_error = 6; return 0; }
    } else if (name && *name) {
        if (size <= 0) { g_last_error = 87; return 0; }
        fd = beer_mapping_shm(name, size, 1, &existed);
        if (fd < 0) { g_last_error = 5; return 0; }
        owned = 1;
        if (existed) { struct stat st; if (fstat(fd, &st) == 0 && st.st_size > size) size = st.st_size; }
    } else if (size <= 0) { g_last_error = 87; return 0; }
    u64 handle = 0;
    u32 status = beer_nt_section_create(&handle, fd, size, protect & 0xFF, 0x8000000u /* SEC_COMMIT */);
    if (owned) close(fd);
    if (getenv("BEER_NT_SECTION_DIAGNOSTICS"))
        fprintf(stderr, "[MAPPING] CreateFileMapping name=%s file=0x%llx size=%lld protect=0x%x -> status=0x%x handle=0x%llx%s\n",
                name ? name : "(none)", (unsigned long long)file, (long long)size, protect, status, (unsigned long long)handle,
                existed ? " (existing)" : "");
    if (status) { g_last_error = status == BEER_STATUS_MAPPED_FILE_SIZE_ZERO ? 1006u : 8u; return 0; }
    g_last_error = existed ? 183u : 0u;                                    /* ERROR_ALREADY_EXISTS */
    return handle;
}
static void beer_mapping_name(const u16 *wide, char *out, size_t size)
{
    size_t i = 0;
    for (; wide && wide[i] && i + 1 < size; ++i) out[i] = (char)(wide[i] < 0x80 ? wide[i] : '_');
    out[i] = 0;
}
static u64 __attribute__((ms_abi)) impl_CreateFileMappingA(u64 file, void *attributes, u32 protect, u32 size_high, u32 size_low, const char *name)
    { (void)attributes; return beer_create_file_mapping(file, protect, size_high, size_low, name); }
static u64 __attribute__((ms_abi)) impl_CreateFileMappingW(u64 file, void *attributes, u32 protect, u32 size_high, u32 size_low, const u16 *name)
{
    (void)attributes;
    char narrow[260];
    beer_mapping_name(name, narrow, sizeof(narrow));
    return beer_create_file_mapping(file, protect, size_high, size_low, name ? narrow : NULL);
}
static u64 beer_open_file_mapping(const char *name)
{
    int fd = name ? beer_mapping_shm(name, 0, 0, NULL) : -1;
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size <= 0) {
        if (fd >= 0) close(fd);
        if (getenv("BEER_NT_SECTION_DIAGNOSTICS")) fprintf(stderr, "[MAPPING] OpenFileMapping name=%s -> not found\n", name ? name : "(none)");
        g_last_error = 2;
        return 0;
    }
    u64 handle = 0;
    u32 status = beer_nt_section_create(&handle, fd, st.st_size, 0x04, 0x8000000u);
    close(fd);
    if (getenv("BEER_NT_SECTION_DIAGNOSTICS")) fprintf(stderr, "[MAPPING] OpenFileMapping name=%s -> handle=0x%llx\n", name, (unsigned long long)handle);
    if (status) { g_last_error = 8; return 0; }
    g_last_error = 0;
    return handle;
}
static u64 __attribute__((ms_abi)) impl_OpenFileMappingA(u32 access, u32 inherit, const char *name)
    { (void)access; (void)inherit; return beer_open_file_mapping(name); }
static u64 __attribute__((ms_abi)) impl_OpenFileMappingW(u32 access, u32 inherit, const u16 *name)
{
    (void)access; (void)inherit;
    char narrow[260];
    beer_mapping_name(name, narrow, sizeof(narrow));
    return beer_open_file_mapping(name ? narrow : NULL);
}
static u64 __attribute__((ms_abi)) impl_MapViewOfFileEx(u64 mapping, u32 access, u32 offset_high, u32 offset_low, u64 bytes, void *base)
{
    u32 protection = (access & 0x20u) ? 0x40u : ((access & 0x2u) || (access & 0xF001Fu) == 0xF001Fu) ? 0x04u
                   : (access & 0x1u) ? 0x08u /* FILE_MAP_COPY */ : 0x02u;
    s64 offset = (s64)(((u64)offset_high << 32) | offset_low);
    size_t size = (size_t)bytes;
    void *address = base;
    u32 status = beer_nt_section_map(mapping, &address, 0, &offset, &size, 1, 0, protection);
    if (getenv("BEER_NT_SECTION_DIAGNOSTICS"))
        fprintf(stderr, "[MAPPING] MapViewOfFile handle=0x%llx access=0x%x offset=%lld size=%llu -> status=0x%x base=%p\n",
                (unsigned long long)mapping, access, (long long)offset, (unsigned long long)bytes, status, address);
    if (status) { g_last_error = status == BEER_STATUS_INVALID_HANDLE ? 6u : 8u; return 0; }
    g_last_error = 0;
    return (u64)(uintptr_t)address;
}
static u64 __attribute__((ms_abi)) impl_MapViewOfFile(u64 mapping, u32 access, u32 offset_high, u32 offset_low, u64 bytes)
    { return impl_MapViewOfFileEx(mapping, access, offset_high, offset_low, bytes, NULL); }
static u64 __attribute__((ms_abi)) impl_UnmapViewOfFile(void *base)
    { return beer_nt_section_unmap(base) == 0 ? 1 : (g_last_error = 487, 0); }
