/* ---- WoW32: Win32 surface for Delphi / Inno Setup style programs (kernel32, advapi32, oleaut32, user32) ----
 * Included by platform/wow32.inc before its API table. Functions here translate what differs between the 32- and
 * 64-bit ABIs (structures holding pointers or handles, pointer/handle outputs, memory that must stay below 2 GiB);
 * plain-argument APIs are forwarded to the 64-bit implementations through W32_FWD in the table. */

/* Calls a 64-bit implementation with the sign-extended arguments, except argument `out_index`, which becomes a
 * pointer to the 64-bit `out` (handle outputs: the caller narrows the result into the 32-bit slot). */
static u64 w32_call_impl_out(const char *name, const u32 *args, u32 argc, u32 out_index, u64 *out)
{
    const ImplFn impl = find_impl(name);
    if (!impl) { g_last_error = 120; return 0; }
    u64 a[12] = {0};
    for (u32 i = 0; i < argc && i < 12; ++i) a[i] = w32_widen(args[i]);
    a[out_index] = args[out_index] ? (u64)(uintptr_t)out : 0;
    return (u32)((W32Impl12)(void *)impl)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11]);
}

/* VERSIONINFO is fixed-width data. VerQueryValue's returned pointer is not:
 * translate it through a native temporary, never write eight bytes to LPVOID*. */
static u64 w32_VerQueryValueW(const u32 *args)
{
    u64 value = 0;
    u64 result = w32_call_impl_out("VerQueryValueW", args, 4, 2, &value);
    if (!result) return 0;
    if (value > UINT32_MAX) { g_last_error = 50; return 0; }
    if (args[2]) *(u32 *)W32_PTR(2) = (u32)value;
    return result;
}

/* LOGFONTW/LOGPEN have fixed-width fields, but BITMAP and LOGBRUSH contain
 * pointers/ULONG_PTR. Never let the native GetObject overwrite a PE32 buffer. */
static u64 w32_GetObjectW(const u32 *args)
{
    u8 native[96] = {0}, out[92] = {0};
    u64 object = w32_widen(args[0]);
    u32 type = beer_GetObjectType(object);
    s32 size = (s32)args[1];
    int full;
    if (type == 6) full = 92; /* OBJ_FONT */
    else if (type == 1) full = 16; /* OBJ_PEN */
    else if (type == 2) full = 12; /* OBJ_BRUSH */
    else if (type == 7) full = beer_GetObjectW(object, 0, NULL) > 32 ? 84 : 24;
    else return 0;
    if (!args[2]) return full;
    if (size <= 0 || !beer_GetObjectW(object, sizeof(native), native)) return 0;
    if (type == 7) {
        u64 bits;
        memcpy(&bits, native + 24, sizeof(bits));
        if (bits > UINT32_MAX) { g_last_error = 50; return 0; }
        if (size < 24) return 0;
        memcpy(out, native, 20);
        u32 pointer = (u32)bits;
        memcpy(out + 20, &pointer, 4);
        if (full == 84 && size >= 84) memcpy(out + 24, native + 32, 52);
        else full = 24;
    } else memcpy(out, native, (size_t)full);
    int copied = size < full ? size : full;
    memcpy(W32_PTR(2), out, (size_t)copied);
    return copied;
}

static u64 w32_EnumThreadWindows(const u32 *args)
{
    if (!args[1]) { g_last_error = 87; return 0; }
    u32 windows[BEER_MAX_WINDOWS];
    size_t count = 0;
    pthread_mutex_lock(&g_beer_windows_mtx);
    for (size_t i = 0; i < BEER_MAX_WINDOWS; ++i) {
        const BeerWindow *w = &g_beer_windows[i];
        if (w->used && w->owner_tid == args[0] && !(w->style & 0x40000000u) &&
            w->parent != (u64)-3)
            windows[count++] = (u32)w->hwnd;
    }
    pthread_mutex_unlock(&g_beer_windows_mtx);
    for (size_t i = 0; i < count; ++i) {
        /* Do not hold the window-table lock across guest callbacks. */
        pthread_mutex_lock(&g_beer_windows_mtx);
        const BeerWindow *w = beer_window_find(windows[i]);
        const int live = w && w->owner_tid == args[0] && !(w->style & 0x40000000u) &&
                         w->parent != (u64)-3;
        pthread_mutex_unlock(&g_beer_windows_mtx);
        if (!live) continue;
        const u32 argv[] = {windows[i], args[2]};
        if (!w32_call(args[1], 2, argv)) return 0;
    }
    return 1;
}

static u64 w32_CreateDIBSection(const u32 *args)
{
    if (args[3]) *(u32 *)W32_PTR(3) = 0;
    if (args[2] || args[4]) { g_last_error = 50; return 0; }
    void *bits = NULL;
    u64 bitmap = beer_gdi_create_dib_section_low32(W32_PTR(1), &bits);
    if (!bitmap) {
        g_last_error = errno == ENOMEM ? 8 : errno == ENOTSUP ? 50 : 87;
        return 0;
    }
    if (args[3]) *(u32 *)W32_PTR(3) = (u32)(uintptr_t)bits;
    return (u32)bitmap;
}

/* SID bytes have the same layout on both ABIs, but PSID* is a DWORD output
 * and its allocation must remain addressable by the PE32 caller. */
static u64 w32_AllocateAndInitializeSid(const u32 *args)
{
    const u8 count = (u8)args[1];
    if (!args[0] || !args[10] || count > 8) { g_last_error = 87; return 0; }
    u8 *sid = w32_heap_alloc(8u + 4u * count, 1);
    if (!sid) { g_last_error = 8; return 0; }
    sid[0] = 1;
    sid[1] = count;
    memcpy(sid + 2, W32_PTR(0), 6);
    memcpy(sid + 8, args + 2, 4u * count);
    const u32 pointer = (u32)(uintptr_t)sid;
    memcpy(W32_PTR(10), &pointer, sizeof(pointer));
    return 1;
}

static u64 w32_FreeSid(const u32 *args)
{
    if (args[0]) w32_heap_free(W32_PTR(0));
    return 0;
}

/* ---- modules ---- */

/* System DLLs are opaque pseudo handles (no 32-bit images of them exist); GetProcAddress on them hands out thunks. */
static const struct { const char *name; u32 handle; } g_w32_system_modules[] = {
    {"kernel32", 0xFEED0000u}, {"kernelbase", 0xFEE90000u}, {"user32", 0xFEF10000u}, {"ntdll", 0xFEF50000u},
    {"advapi32", 0xFEF90000u}, {"msvcrt", 0xFEFD0000u}, {"shell32", 0xFF010000u}, {"oleaut32", 0xFF050000u},
    {"ole32", 0xFF090000u}, {"comctl32", 0xFF0D0000u}, {"gdi32", 0xFF110000u}, {"version", 0xFF150000u},
    {"shlwapi", 0xFF190000u}, {"comdlg32", 0xFF1D0000u}, {"uxtheme", 0xFF210000u}, {"winmm", 0xFF250000u},
    {"imm32", 0xFF290000u}, {"userenv", 0xFF2D0000u}, {"mpr", 0xFF310000u}, {"netapi32", 0xFF350000u},
    {"dwmapi", 0xFF390000u}, {"setupapi", 0xFF3D0000u}, {"crypt32", 0xFF410000u}, {"wintrust", 0xFF450000u}, {"wininet", 0xFF490000u}, {"msacm32", 0xFF4D0000u},
};

static int w32_is_system_module(u32 handle)
{
    for (size_t i = 0; i < sizeof(g_w32_system_modules) / sizeof(*g_w32_system_modules); ++i)
        if (g_w32_system_modules[i].handle == handle) return 1;
    return 0;
}

/* Base name without directory and ".dll", as ASCII. */
static void w32_module_key(const char *path, char *out, size_t size)
{
    const char *base = path;
    for (const char *p = path; *p; ++p) if (*p == '\\' || *p == '/') base = p + 1;
    snprintf(out, size, "%s", base);
    char *dot = strrchr(out, '.');
    if (dot && !strcasecmp(dot, ".dll")) *dot = 0;
}

static u32 w32_module_by_name(const char *path)
{
    char key[64];
    w32_module_key(path, key, sizeof(key));
    for (size_t i = 0; i < sizeof(g_w32_system_modules) / sizeof(*g_w32_system_modules); ++i)
        if (!strcasecmp(key, g_w32_system_modules[i].name)) {
            w32_sysmod_ensure_image(g_w32_system_modules[i].handle);     /* headers + exports for manual walkers */
            return g_w32_system_modules[i].handle;
        }
    u32 loaded = w32_dll_by_name(path);
    if (loaded) return loaded;
    /* The executable itself, by name. */
    char self[PATH_MAX];
    if (impl_GetModuleFileNameA(0, self, sizeof(self))) {
        char self_key[64];
        w32_module_key(self, self_key, sizeof(self_key));
        if (!strcasecmp(key, self_key)) return g_w32_image;
    }
    return 0;
}

static void w32_narrow(const u16 *wide, char *out, size_t size)
{
    size_t i = 0;
    for (; wide && wide[i] && i + 1 < size; ++i) out[i] = wide[i] < 0x80 ? (char)wide[i] : '?';
    out[i] = 0;
}

static u64 w32_GetModuleHandleW(const u32 *args)
{
    if (!args[0]) return g_w32_image;
    char name[PATH_MAX];
    w32_narrow(W32_PTR(0), name, sizeof(name));
    const u32 handle = w32_module_by_name(name);
    if (!handle) g_last_error = 126;                        /* ERROR_MOD_NOT_FOUND */
    return handle;
}

/* System pseudo modules retain their existing policy; other DLLs are real PE32 images. */
static u64 w32_load_library(const char *name)
{
    const u32 handle = w32_module_by_name(name);
    if (handle && !w32_dll_path(handle)) return handle;
    return w32_dll_load(name);
}
static u64 w32_LoadLibraryA(const u32 *args) { return args[0] ? w32_load_library(W32_PTR(0)) : 0; }
static u64 w32_LoadLibraryExA(const u32 *args)
{
    if (args[1] || args[2]) { g_last_error = 50; return 0; }
    return w32_LoadLibraryA(args);
}
static u64 w32_LoadLibraryW(const u32 *args)
{
    if (!args[0]) return 0;
    char name[PATH_MAX];
    w32_narrow(W32_PTR(0), name, sizeof(name));
    return w32_load_library(name);
}
static u64 w32_LoadLibraryExW(const u32 *args)
{
    if (args[1] || args[2]) { g_last_error = 50; return 0; }
    return w32_LoadLibraryW(args);
}
static u64 w32_FreeLibrary(const u32 *args)
{
    if (w32_is_system_module(args[0])) return 1;
    return w32_dll_free(args[0]);
}

static u64 w32_GetModuleFileNameW(const u32 *args)
{
    const char *dll_path = w32_dll_path(args[0]);
    if (dll_path) {
        /* Canonical host paths are addressable through Beer's Z: drive. */
        if (!args[2]) { g_last_error = 122; return 0; }
        u16 *out = W32_PTR(1);
        for (u32 i = 0; i < args[2]; ++i) {
            char c = i == 0 ? 'Z' : i == 1 ? ':' : dll_path[i - 2];
            out[i] = c == '/' ? '\\' : (u8)c;
            if (!c) return i;
        }
        out[args[2] - 1] = 0;
        g_last_error = 122;
        return args[2];
    }
    if (args[0] && args[0] != g_w32_image && !w32_is_system_module(args[0])) { g_last_error = 126; return 0; }
    /* System pseudo modules report their System32 path, as on Windows; the executable reports its own. */
    if (args[0] && args[0] != g_w32_image) {
        const char *name = "kernel32";
        for (size_t i = 0; i < sizeof(g_w32_system_modules) / sizeof(*g_w32_system_modules); ++i)
            if (g_w32_system_modules[i].handle == args[0]) name = g_w32_system_modules[i].name;
        char path[96];
        snprintf(path, sizeof(path), "C:\\Windows\\SysWOW64\\%s.dll", name);
        u16 *out = W32_PTR(1);
        u32 i = 0;
        for (; path[i] && i + 1 < args[2]; ++i) out[i] = (u8)path[i];
        if (args[2]) out[i] = 0;
        return i;
    }
    return w32_forward("GetModuleFileNameW", (const u32[]){0, args[1], args[2]}, 3);
}

/* ---- system information ---- */

/* SYSTEM_INFO, 36 bytes in a 32-bit process. */
static u64 w32_GetSystemInfo(const u32 *args)
{
    u8 *si = W32_PTR(0);
    if (!si) return 0;
    memset(si, 0, 36);
    const long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    const u32 n = cpus > 0 && cpus <= 32 ? (u32)cpus : 32;
    *(u16 *)(si + 0) = 0;                                   /* PROCESSOR_ARCHITECTURE_INTEL */
    *(u32 *)(si + 4) = 0x1000;                              /* dwPageSize */
    *(u32 *)(si + 8) = 0x00010000;                          /* lpMinimumApplicationAddress */
    *(u32 *)(si + 12) = 0x7ffeffff;                         /* lpMaximumApplicationAddress */
    *(u32 *)(si + 16) = n == 32 ? 0xffffffffu : (1u << n) - 1u;
    *(u32 *)(si + 20) = n;
    *(u32 *)(si + 24) = 586;                                /* PROCESSOR_INTEL_PENTIUM */
    *(u32 *)(si + 28) = 0x10000;                            /* dwAllocationGranularity */
    *(u16 *)(si + 32) = 6;                                  /* wProcessorLevel */
    return 0;
}
static u64 w32_GetNativeSystemInfo(const u32 *args)
{
    w32_GetSystemInfo(args);
    u8 *si = W32_PTR(0);
    if (si) *(u16 *)si = 9;                                 /* PROCESSOR_ARCHITECTURE_AMD64 */
    return 0;
}

/* SetFilePointerEx(HANDLE, LARGE_INTEGER distance, PLARGE_INTEGER new_position, DWORD method): the distance is passed
 * by value, as two stack DWORDs (low, high). The generic forwarder widened each DWORD into its own argument, so the
 * high half became the output pointer and every seek past 4 GiB went wrong (FitGirl's 6 GB fg-01.bin then
 * "decompression fails"). The LARGE_INTEGER output has the same layout in both modes. */
static u64 w32_SetFilePointerEx(const u32 *args)
{
    const s64 distance = (s64)(((u64)args[2] << 32) | args[1]);
    return (u32)impl_SetFilePointerEx(w32_widen(args[0]), distance, (s64 *)W32_PTR(3), args[4]);
}

/* Bytes of the 32-bit user range (64 KiB .. 0x7ffeffff) that no host mapping uses. */
static u64 w32_free_address_space(void)
{
    const unsigned long long lo = 0x10000, hi = 0x7fff0000;
    unsigned long long used = 0;
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) return hi - lo;
    char line[512];
    while (fgets(line, sizeof(line), maps)) {
        unsigned long long a, b;
        if (sscanf(line, "%llx-%llx", &a, &b) != 2 || b <= lo || a >= hi) continue;
        if (a < lo) a = lo;
        if (b > hi) b = hi;
        used += b - a;
    }
    fclose(maps);
    return used < hi - lo ? hi - lo - used : 0;
}

/* A 32-bit process that is not large-address-aware sees at most 2 GB - 1 in each MEMORYSTATUS field. */
static u32 w32_clamp_2g(u64 v) { return v > 0x7fffffffu ? 0x7fffffffu : (u32)v; }

/* GlobalMemoryStatus: the 32-bit MEMORYSTATUS is eight DWORDs (32 bytes). Forwarding to the 64-bit implementation
 * wrote 56 bytes of SIZE_T fields and overwrote the caller's frame (unarc.dll returned to 0x7fff, the high half of
 * the 64-bit total-virtual value). */
static u64 w32_GlobalMemoryStatus(const u32 *args)
{
    u8 *ms = W32_PTR(0);
    if (!ms) return 0;
    WIN_MEMORYSTATUSEX info;
    memset(&info, 0, sizeof(info));
    info.length = sizeof(info);
    if (!impl_GlobalMemoryStatusEx(&info)) return 0;
    const u32 out[8] = { 32, info.memory_load, w32_clamp_2g(info.total_physical), w32_clamp_2g(info.available_physical),
                         w32_clamp_2g(info.total_page_file), w32_clamp_2g(info.available_page_file), 0x7ffe0000u,
                         w32_clamp_2g(w32_free_address_space()) };
    memcpy(ms, out, sizeof(out));
    return 0;
}

/* GlobalMemoryStatusEx: same 64-byte layout as on x64, but the virtual figures are the 32-bit user range. */
static u64 w32_GlobalMemoryStatusEx(const u32 *args)
{
    WIN_MEMORYSTATUSEX *ms = W32_PTR(0);
    if (!ms) { g_last_error = 87; return 0; }
    if (!impl_GlobalMemoryStatusEx(ms)) return 0;
    ms->total_virtual = 0x7ffe0000u;
    ms->available_virtual = w32_free_address_space();
    ms->available_extended_virtual = 0;
    return 1;
}

/* ---- synchronization ---- */

/* CRITICAL_SECTION, 24 bytes: DebugInfo, LockCount, RecursionCount, OwningThread, LockSemaphore, SpinCount.
 * LockCount follows Windows (-1 free); LockSemaphore is the futex word (0 free, 1 held, 2 held with waiters). */
typedef struct { u32 debug; s32 lock_count, recursion; u32 owner, word, spin; } W32CritSec;

static u32 w32_tid(void) { return w32_teb()->UniqueThread; }

static u64 w32_InitializeCriticalSection(const u32 *args)
{
    W32CritSec *cs = W32_PTR(0);
    if (!cs) return 0;
    memset(cs, 0, sizeof(*cs));
    cs->lock_count = -1;
    return 1;
}
static u64 w32_InitializeCriticalSectionAndSpinCount(const u32 *args)
{
    w32_InitializeCriticalSection(args);
    W32CritSec *cs = W32_PTR(0);
    if (cs) cs->spin = args[1];
    return 1;
}
/* (cs, spin, flags): the flags (CRITICAL_SECTION_NO_DEBUG_INFO, ...) only affect debug bookkeeping. */
static u64 w32_InitializeCriticalSectionEx(const u32 *args) { return w32_InitializeCriticalSectionAndSpinCount(args); }
/* A 32-bit SLIST_HEADER is 8 bytes (next pointer, depth, sequence), all zero when empty. The MSVC CRT's startup
 * initializes one and later pushes onto it; leaving it unwritten crashed Pratfall's Launcher.exe on a null next. */
static u64 w32_InitializeSListHead(const u32 *args)
{
    u8 *head = W32_PTR(0);
    if (head) memset(head, 0, 8);
    return 0;
}
static u64 w32_DeleteCriticalSection(const u32 *args) { (void)args; return 0; }

static void w32_cs_enter(W32CritSec *cs)
{
    const u32 me = w32_tid();
    if (cs->owner == me) { ++cs->recursion; return; }
    u32 expected = 0;
    if (!__atomic_compare_exchange_n(&cs->word, &expected, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        while (__atomic_exchange_n(&cs->word, 2, __ATOMIC_ACQUIRE) != 0)
            syscall(SYS_futex, &cs->word, FUTEX_WAIT_PRIVATE, 2, NULL, NULL, 0);
    }
    cs->owner = me;
    cs->recursion = 1;
    cs->lock_count = 0;
}
static u64 w32_EnterCriticalSection(const u32 *args) { if (args[0]) w32_cs_enter(W32_PTR(0)); return 0; }
static u64 w32_TryEnterCriticalSection(const u32 *args)
{
    W32CritSec *cs = W32_PTR(0);
    if (!cs) return 0;
    const u32 me = w32_tid();
    if (cs->owner == me) { ++cs->recursion; return 1; }
    u32 expected = 0;
    if (!__atomic_compare_exchange_n(&cs->word, &expected, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 0;
    cs->owner = me;
    cs->recursion = 1;
    cs->lock_count = 0;
    return 1;
}
static u64 w32_LeaveCriticalSection(const u32 *args)
{
    W32CritSec *cs = W32_PTR(0);
    if (!cs || cs->owner != w32_tid()) return 0;
    if (--cs->recursion > 0) return 0;
    cs->owner = 0;
    cs->lock_count = -1;
    if (__atomic_exchange_n(&cs->word, 0, __ATOMIC_RELEASE) == 2)
        syscall(SYS_futex, &cs->word, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
    return 0;
}

static u64 w32_InterlockedCompareExchange(const u32 *args)
{
    u32 expected = args[2];
    __atomic_compare_exchange_n((u32 *)W32_PTR(0), &expected, args[1], 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}
static u64 w32_InterlockedExchange(const u32 *args) { return __atomic_exchange_n((u32 *)W32_PTR(0), args[1], __ATOMIC_SEQ_CST); }
static u64 w32_InterlockedExchangeAdd(const u32 *args) { return __atomic_fetch_add((u32 *)W32_PTR(0), args[1], __ATOMIC_SEQ_CST); }
static u64 w32_InterlockedIncrement(const u32 *args) { return __atomic_add_fetch((u32 *)W32_PTR(0), 1u, __ATOMIC_SEQ_CST); }
static u64 w32_InterlockedDecrement(const u32 *args) { return __atomic_sub_fetch((u32 *)W32_PTR(0), 1u, __ATOMIC_SEQ_CST); }

/* Objects with SECURITY_ATTRIBUTES: 12 bytes in 32-bit code (nLength, lpSecurityDescriptor, bInheritHandle),
 * 24 in 64-bit. Returns the 64-bit copy in `buffer`, or NULL. */
static void *w32_security_attributes(u32 address, u8 buffer[24])
{
    if (!address) return NULL;
    const u32 *sa = (const u32 *)(uintptr_t)address;
    memset(buffer, 0, 24);
    *(u32 *)buffer = 24;
    *(u64 *)(buffer + 8) = sa[1];
    *(u32 *)(buffer + 16) = sa[2];
    return buffer;
}

static u64 w32_call_sa(const char *name, const u32 *args, u32 argc, u32 sa_index)
{
    u32 copy[12];
    memcpy(copy, args, argc * 4);
    u8 sa[24];
    const ImplFn impl = find_impl(name);
    if (!impl) { g_last_error = 120; return 0; }
    u64 a[12] = {0};
    for (u32 i = 0; i < argc; ++i) a[i] = w32_widen(copy[i]);
    a[sa_index] = (u64)(uintptr_t)w32_security_attributes(copy[sa_index], sa);
    return (u32)((W32Impl12)(void *)impl)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11]);
}
static u64 w32_CreateFileW(const u32 *args) { return w32_call_sa("CreateFileW", args, 7, 3); }
static u64 w32_CreateFileA(const u32 *args) { return w32_call_sa("CreateFileA", args, 7, 3); }
static u64 w32_CreateEventW(const u32 *args) { return w32_call_sa("CreateEventW", args, 4, 0); }
static u64 w32_CreateEventA(const u32 *args) { return w32_call_sa("CreateEventA", args, 4, 0); }
static u64 w32_CreateMutexW(const u32 *args) { return w32_call_sa("CreateMutexW", args, 3, 0); }
static u64 w32_CreateDirectoryW(const u32 *args) { return w32_call_sa("CreateDirectoryW", args, 2, 1); }
static u64 w32_CreateDirectoryA(const u32 *args) { return w32_call_sa("CreateDirectoryA", args, 2, 1); }

/* ---- thread-local storage: the 64 slots of the TEB32 (TlsSlots at +0xE10) ---- */

static u64 g_w32_tls_used;
static u32 *w32_tls_slots(void) { return (u32 *)((u8 *)w32_teb() + 0xe10); }
static u64 w32_TlsAlloc(const u32 *args)
{
    (void)args;
    pthread_mutex_lock(&g_w32_lock);
    for (u32 i = 0; i < 64; ++i)
        if (!(g_w32_tls_used & (1ull << i))) {
            g_w32_tls_used |= 1ull << i;
            pthread_mutex_unlock(&g_w32_lock);
            w32_tls_slots()[i] = 0;
            return i;
        }
    pthread_mutex_unlock(&g_w32_lock);
    g_last_error = 8;
    return 0xffffffffu;                                     /* TLS_OUT_OF_INDEXES */
}
static u64 w32_TlsFree(const u32 *args)
{
    if (args[0] >= 64) { g_last_error = 87; return 0; }
    pthread_mutex_lock(&g_w32_lock);
    g_w32_tls_used &= ~(1ull << args[0]);
    pthread_mutex_unlock(&g_w32_lock);
    return 1;
}
static u64 w32_TlsGetValue(const u32 *args)
{
    if (args[0] >= 64) { g_last_error = 87; return 0; }
    g_last_error = 0;
    return w32_tls_slots()[args[0]];
}
static u64 w32_TlsSetValue(const u32 *args)
{
    if (args[0] >= 64) { g_last_error = 87; return 0; }
    w32_tls_slots()[args[0]] = args[1];
    return 1;
}

/* ---- memory ---- */

/* Local/Global memory is fixed memory from the 32-bit heap (LMEM_FIXED/GMEM_FIXED; movable handles are the
 * pointers themselves, which Lock/Unlock pass through). LMEM_ZEROINIT/GMEM_ZEROINIT = 0x40. */
static u64 w32_LocalAlloc(const u32 *args)
{
    void *p = w32_heap_alloc(args[1] ? args[1] : 1, (args[0] & 0x40u) != 0);
    if (!p) g_last_error = 8;
    return (u32)(uintptr_t)p;
}
static u64 w32_LocalReAlloc(const u32 *args)
{
    void *p = w32_heap_realloc(W32_PTR(0), args[1] ? args[1] : 1, (args[2] & 0x40u) != 0);
    if (!p) g_last_error = 8;
    return (u32)(uintptr_t)p;
}
static u64 w32_LocalSize(const u32 *args) { return args[0] ? (u32)w32_heap_size(W32_PTR(0)) : 0; }
static u64 w32_GlobalFree(const u32 *args) { w32_heap_free(W32_PTR(0)); return 0; }
static u64 w32_GlobalLock(const u32 *args) { return args[0]; }
static u64 w32_GlobalUnlock(const u32 *args) { (void)args; return 1; }
static u64 w32_GlobalHandle(const u32 *args) { return args[0]; }

/* VirtualAlloc below 2 GiB. Reservations are recorded so MEM_RELEASE and VirtualQuery know their extent. */
#define W32_VMAX 4096
static struct { u32 base, size; } g_w32_vm[W32_VMAX];
static u32 g_w32_vm_count;

static int w32_page_prot(u32 protect)
{
    switch (protect & 0xff) {
    case 0x01: return PROT_NONE;                                    /* PAGE_NOACCESS */
    case 0x02: return PROT_READ;                                    /* PAGE_READONLY */
    case 0x04: case 0x08: return PROT_READ | PROT_WRITE;            /* PAGE_READWRITE / WRITECOPY */
    case 0x10: return PROT_EXEC | PROT_READ;                        /* PAGE_EXECUTE */
    case 0x20: return PROT_READ | PROT_EXEC;                        /* PAGE_EXECUTE_READ */
    default:   return PROT_READ | PROT_WRITE | PROT_EXEC;           /* EXECUTE_READWRITE / WRITECOPY */
    }
}

static int w32_vm_find(u32 address)
{
    for (u32 i = 0; i < g_w32_vm_count; ++i)
        if (address >= g_w32_vm[i].base && address - g_w32_vm[i].base < g_w32_vm[i].size) return (int)i;
    return -1;
}

static u64 w32_VirtualAlloc(const u32 *args)
{
    const u32 type = args[2];
    const int prot = w32_page_prot(args[3]);
    u32 address = args[0], size = args[1];
    if (!size) { g_last_error = 87; return 0; }
    pthread_mutex_lock(&g_w32_lock);
    const int region = address ? w32_vm_find(address) : -1;
    pthread_mutex_unlock(&g_w32_lock);
    if (region >= 0) {                                       /* commit (or re-protect) inside a reservation */
        const u32 start = address & ~0xfffu, end = (address + size + 0xfffu) & ~0xfffu;
        if (mprotect((void *)(uintptr_t)start, end - start, (type & 0x1000u) ? prot : PROT_NONE) != 0) {
            g_last_error = 487;                              /* ERROR_INVALID_ADDRESS */
            return 0;
        }
        return start;
    }
    size = (size + 0xffffu) & ~0xffffu;
    void *p = w32_map(address & ~0xffffu, size, (type & 0x1000u) ? prot : PROT_NONE);
    if (!p || (address && (u32)(uintptr_t)p != (address & ~0xffffu))) {
        if (p) munmap(p, size);
        g_last_error = address ? 487 : 8;
        return 0;
    }
    pthread_mutex_lock(&g_w32_lock);
    if (g_w32_vm_count < W32_VMAX) {
        g_w32_vm[g_w32_vm_count].base = (u32)(uintptr_t)p;
        g_w32_vm[g_w32_vm_count++].size = size;
    }
    pthread_mutex_unlock(&g_w32_lock);
    return (u32)(uintptr_t)p;
}

static u64 w32_VirtualFree(const u32 *args)
{
    const u32 address = args[0], size = args[1], type = args[2];
    pthread_mutex_lock(&g_w32_lock);
    const int region = w32_vm_find(address);
    if (region < 0) { pthread_mutex_unlock(&g_w32_lock); g_last_error = 487; return 0; }
    if (type & 0x8000u) {                                    /* MEM_RELEASE: the whole reservation */
        munmap((void *)(uintptr_t)g_w32_vm[region].base, g_w32_vm[region].size);
        g_w32_vm[region] = g_w32_vm[--g_w32_vm_count];
        pthread_mutex_unlock(&g_w32_lock);
        return 1;
    }
    pthread_mutex_unlock(&g_w32_lock);
    const u32 start = address & ~0xfffu, end = (address + size + 0xfffu) & ~0xfffu;   /* MEM_DECOMMIT */
    mmap((void *)(uintptr_t)start, end - start, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    return 1;
}

/* MEMORY_BASIC_INFORMATION, 28 bytes in a 32-bit process. */
static u64 w32_VirtualQuery(const u32 *args)
{
    u32 *mbi = W32_PTR(1);
    if (!mbi || args[2] < 28) { g_last_error = 24; return 0; }
    memset(mbi, 0, 28);
    const u32 page = args[0] & ~0xfffu;
    pthread_mutex_lock(&g_w32_lock);
    const int region = w32_vm_find(page);
    const u32 base = region >= 0 ? g_w32_vm[region].base : 0, size = region >= 0 ? g_w32_vm[region].size : 0;
    pthread_mutex_unlock(&g_w32_lock);
    unsigned char probe;
    if (region >= 0) {
        mbi[0] = page; mbi[1] = base; mbi[2] = 0x04;
        mbi[3] = base + size - page;
        mbi[4] = 0x1000; mbi[5] = 0x04; mbi[6] = 0x20000;    /* MEM_COMMIT, PAGE_READWRITE, MEM_PRIVATE */
    } else if (mincore((void *)(uintptr_t)page, 0x1000, &probe) == 0) {
        u32 image_base = 0, image_size = 0;
        if (g_w32_image) {
            const u8 *exe = (const u8 *)(uintptr_t)g_w32_image;
            const W32NtHdrs *nt = (const W32NtHdrs *)(exe + ((const DosHdr *)exe)->lfanew);
            if (page >= g_w32_image && page - g_w32_image < nt->opt.size_of_image) {
                image_base = g_w32_image; image_size = nt->opt.size_of_image;
            }
        }
        if (!image_base) w32_dll_image_range(page, &image_base, &image_size);
        mbi[0] = page; mbi[1] = image_base ? image_base : page; mbi[2] = 0x40;
        mbi[3] = image_base ? image_base + image_size - page : 0x1000;
        mbi[4] = 0x1000; mbi[5] = 0x40;                       /* mapped, committed */
        mbi[6] = image_base ? 0x1000000u : 0x20000u;          /* MEM_IMAGE / MEM_PRIVATE */
    } else {
        mbi[0] = page; mbi[3] = 0x10000; mbi[4] = 0x10000; mbi[5] = 0x01;   /* MEM_FREE */
    }
    return 28;
}

/* ---- files and paths: pointer outputs are 32-bit ---- */

static u64 w32_GetFullPathNameW(const u32 *args)
{
    u64 part = 0;
    const ImplFn impl = find_impl("GetFullPathNameW");
    if (!impl) { g_last_error = 120; return 0; }
    const u64 n = ((u64 (__attribute__((ms_abi)) *)(u64, u64, u64, u64))(void *)impl)(
        args[0], args[1], args[2], args[3] ? (u64)(uintptr_t)&part : 0);
    if (args[3]) *(u32 *)W32_PTR(3) = (u32)part;
    return (u32)n;
}
static u64 w32_GetFullPathNameA(const u32 *args)
{
    u64 part = 0;
    const ImplFn impl = find_impl("GetFullPathNameA");
    if (!impl) { g_last_error = 120; return 0; }
    const u64 n = ((u64 (__attribute__((ms_abi)) *)(u64, u64, u64, u64))(void *)impl)(
        args[0], args[1], args[2], args[3] ? (u64)(uintptr_t)&part : 0);
    if (args[3]) *(u32 *)W32_PTR(3) = (u32)part;
    return (u32)n;
}

/* FormatMessageW with Beer's text; FORMAT_MESSAGE_ALLOCATE_BUFFER allocates from the 32-bit heap. */
static u64 w32_FormatMessageW(const u32 *args)
{
    char text[64];
    snprintf(text, sizeof(text), "error %u", args[2]);
    const size_t length = strlen(text);
    u16 *out;
    if (args[0] & 0x100u) {
        out = w32_heap_alloc((length + 1) * 2, 0);
        if (!out) { g_last_error = 8; return 0; }
        if (args[4]) *(u32 *)W32_PTR(4) = (u32)(uintptr_t)out;
    } else {
        if (!args[4] || args[5] <= length) { g_last_error = 122; return 0; }
        out = W32_PTR(4);
    }
    for (size_t i = 0; i <= length; ++i) out[i] = (u8)text[i];
    return length;
}

/* ---- advapi32: handle outputs ---- */

static u64 w32_RegOpenKeyExW(const u32 *args)
{
    u64 key = 0;
    const u64 status = w32_call_impl_out("RegOpenKeyExW", args, 5, 4, &key);
    if (args[4]) *(u32 *)W32_PTR(4) = (u32)key;
    return (u32)status;
}
static u64 w32_RegCreateKeyExW(const u32 *args)
{
    u64 key = 0;
    const u64 status = w32_call_impl_out("RegCreateKeyExW", args, 9, 7, &key);
    if (args[7]) *(u32 *)W32_PTR(7) = (u32)key;
    return (u32)status;
}
static u64 w32_OpenProcessToken(const u32 *args)
{
    u64 token = 0;
    const u64 ok = w32_call_impl_out("OpenProcessToken", args, 3, 2, &token);
    if (args[2]) *(u32 *)W32_PTR(2) = (u32)token;
    return (u32)ok;
}

/* ---- oleaut32: BSTRs live in the 32-bit heap (length prefix, then the characters) ---- */

static u32 w32_bstr_alloc(const u16 *text, u32 chars)
{
    u8 *block = w32_heap_alloc(4 + (size_t)chars * 2 + 2, 0);
    if (!block) return 0;
    *(u32 *)block = chars * 2;
    u16 *s = (u16 *)(block + 4);
    if (text) memcpy(s, text, (size_t)chars * 2); else memset(s, 0, (size_t)chars * 2);
    s[chars] = 0;
    return (u32)(uintptr_t)s;
}
static u64 w32_SysAllocStringLen(const u32 *args) { return w32_bstr_alloc(W32_PTR(0), args[1]); }
static u64 w32_SysAllocString(const u32 *args)
{
    const u16 *s = W32_PTR(0);
    if (!s) return 0;
    u32 n = 0;
    while (s[n]) ++n;
    return w32_bstr_alloc(s, n);
}
static u64 w32_SysFreeString(const u32 *args) { if (args[0]) w32_heap_free((u8 *)W32_PTR(0) - 4); return 0; }
static u64 w32_SysStringLen(const u32 *args) { return args[0] ? *(const u32 *)((const u8 *)W32_PTR(0) - 4) / 2 : 0; }
static u64 w32_SysReAllocStringLen(const u32 *args)
{
    u32 *slot = W32_PTR(0);
    if (!slot) return 0;
    const u32 fresh = w32_bstr_alloc(W32_PTR(1), args[2]);
    if (!fresh) return 0;
    if (*slot) w32_heap_free((u8 *)(uintptr_t)*slot - 4);
    *slot = fresh;
    return 1;
}

/* VARIANT is 16 bytes in a 32-bit process: vt, three reserved words, then the value. Simple types only; BSTRs are
 * copied and freed; other reference types are copied as their pointer value. */
static u64 w32_VariantInit(const u32 *args) { if (args[0]) memset(W32_PTR(0), 0, 16); return 0; }
static u64 w32_VariantClear(const u32 *args)
{
    u8 *v = W32_PTR(0);
    if (!v) return 0;
    if (*(u16 *)v == 8 && *(u32 *)(v + 8)) w32_heap_free((u8 *)(uintptr_t)*(u32 *)(v + 8) - 4);   /* VT_BSTR */
    memset(v, 0, 16);
    return 0;
}
static u64 w32_VariantCopy(const u32 *args)
{
    u8 *dst = W32_PTR(0);
    const u8 *src = W32_PTR(1);
    if (!dst || !src) return 0x80070057u;                   /* E_INVALIDARG */
    if (dst == src) return 0;
    w32_VariantClear((const u32[]){args[0]});
    memcpy(dst, src, 16);
    if (*(const u16 *)src == 8 && *(const u32 *)(src + 8)) {
        const u16 *s = (const u16 *)(uintptr_t)*(const u32 *)(src + 8);
        *(u32 *)(dst + 8) = w32_bstr_alloc(s, *(const u32 *)((const u8 *)s - 4) / 2);
    }
    return 0;
}
static u64 w32_VariantChangeType(const u32 *args)
{
    fprintf(stderr, "[WOW32] VariantChangeType(vt %u -> %u) is not implemented\n",
            args[1] ? *(const u16 *)W32_PTR(1) : 0, args[3] & 0xffff);
    return 0x80020005u;                                     /* DISP_E_TYPEMISMATCH */
}

/* ---- user32 ---- */

static u64 w32_CharNextW(const u32 *args)
{
    const u16 *s = W32_PTR(0);
    return s && *s ? args[0] + 2 : args[0];
}
static u64 w32_CharUpperBuffW(const u32 *args)
{
    u16 *s = W32_PTR(0);
    for (u32 i = 0; s && i < args[1]; ++i) if (s[i] >= 'a' && s[i] <= 'z') s[i] -= 32;
    return args[1];
}
static u64 w32_CharLowerBuffW(const u32 *args)
{
    u16 *s = W32_PTR(0);
    for (u32 i = 0; s && i < args[1]; ++i) if (s[i] >= 'A' && s[i] <= 'Z') s[i] += 32;
    return args[1];
}
static u64 w32_GetKeyboardType(const u32 *args)
{
    switch (args[0]) {
    case 0: return 4;                                       /* IBM enhanced (101/102-key) */
    case 2: return 12;                                      /* function keys */
    default: return 0;
    }
}
static u64 w32_InitCommonControls(const u32 *args) { (void)args; return 0; }
static u64 w32_MessageBoxW(const u32 *args)
{
    char text[1024], caption[256];
    w32_narrow(W32_PTR(1), text, sizeof(text));
    w32_narrow(W32_PTR(2), caption, sizeof(caption));
    fprintf(stderr, "[WOW32] MessageBoxW \"%s\": %s\n", caption, text);
    return w32_forward("MessageBoxW", args, 4);
}

/* ---- resources with wide names: the ASCII lookup in platform/wow32.inc with the names narrowed ---- */

static u64 w32_FindResourceExA(const u32 *args);
/* A resource name/type argument: an integer ID below 0x10000, else a string; narrowed into `buffer`. */
static uintptr_t w32_res_name(u32 value, char *buffer, size_t size)
{
    if (value < 0x10000) return value;
    w32_narrow((const u16 *)(uintptr_t)value, buffer, size);
    return (uintptr_t)buffer;
}
static u64 w32_FindResourceExW(const u32 *args)
{
    char type[256], name[256];
    return w32_find_resource(args[0], w32_res_name(args[1], type, sizeof(type)),
                             w32_res_name(args[2], name, sizeof(name)), args[3]);
}
static u64 w32_FindResourceW(const u32 *args)         /* (module, name, type) */
{
    char type[256], name[256];
    return w32_find_resource(args[0], w32_res_name(args[2], type, sizeof(type)),
                             w32_res_name(args[1], name, sizeof(name)), 0);
}
/* LoadBitmapW/A: system bitmaps (NULL module, OBM_*) from Beer; a module's own RT_BITMAP resources from the 32-bit
 * image (the 64-bit loader cannot see 32-bit modules' resources). */
static u64 w32_load_bitmap(u32 module, uintptr_t name)
{
    if (!module) return (u32)beer_LoadBitmapW(0, name);
    const u64 entry = w32_find_resource(module, name, 2 /* RT_BITMAP */, 0);
    if (!entry) return 0;
    const u32 *data_entry = (const u32 *)(uintptr_t)entry;
    return (u32)beer_gdi_bitmap_from_dib((const u8 *)(uintptr_t)module + data_entry[0], data_entry[1]);
}
static u64 w32_LoadBitmapW(const u32 *args)
{
    char name[256];
    return w32_load_bitmap(args[0], w32_res_name(args[1], name, sizeof(name)));
}
static u64 w32_LoadBitmapA(const u32 *args) { return w32_load_bitmap(args[0], args[1]); }
/* POINT passed by value: two DWORDs on the 32-bit stack, one register (x low, y high) in 64-bit code. */
static u64 w32_pack_point(const u32 *args, int index) { return (u64)args[index] | ((u64)args[index + 1] << 32); }
static u64 w32_PtInRect(const u32 *args) { return (u32)impl_PtInRect(W32_PTR(0), w32_pack_point(args, 1)); }
static u64 w32_WindowFromPoint(const u32 *args) { return (u32)impl_WindowFromPoint(w32_pack_point(args, 0)); }
static u64 w32_FindResourceA(const u32 *args)
{
    const u32 reordered[4] = { args[0], args[2], args[1], 0 };
    return w32_FindResourceExA(reordered);
}

/* GetDiskFreeSpaceW / ExW on the host file system behind the path (NULL: the current directory). */
static int w32_statvfs_w(u32 path, struct statvfs *out)
{
    char host[PATH_MAX] = ".";
    if (path && !wide_to_host_path((const u16 *)(uintptr_t)path, host, sizeof(host))) return -1;
    return statvfs(host, out);
}
static u64 w32_GetDiskFreeSpaceW(const u32 *args)
{
    struct statvfs st;
    if (w32_statvfs_w(args[0], &st) != 0) { g_last_error = 3; return 0; }
    const u64 cluster = 4096, sector = 512;
    const u64 total = (u64)st.f_blocks * st.f_frsize / cluster, avail = (u64)st.f_bavail * st.f_frsize / cluster;
    if (args[1]) *(u32 *)W32_PTR(1) = (u32)(cluster / sector);
    if (args[2]) *(u32 *)W32_PTR(2) = (u32)sector;
    if (args[3]) *(u32 *)W32_PTR(3) = avail > 0xffffffffu ? 0xffffffffu : (u32)avail;
    if (args[4]) *(u32 *)W32_PTR(4) = total > 0xffffffffu ? 0xffffffffu : (u32)total;
    return 1;
}
static u64 w32_GetDiskFreeSpaceExW(const u32 *args)
{
    struct statvfs st;
    if (w32_statvfs_w(args[0], &st) != 0) { g_last_error = 3; return 0; }
    if (args[1]) *(u64 *)W32_PTR(1) = (u64)st.f_bavail * st.f_frsize;
    if (args[2]) *(u64 *)W32_PTR(2) = (u64)st.f_blocks * st.f_frsize;
    if (args[3]) *(u64 *)W32_PTR(3) = (u64)st.f_bfree * st.f_frsize;
    return 1;
}

/* ---- user32: window procedures across the 32/64-bit boundary ----
 * Beer's window system calls window procedures as 64-bit ms_abi functions. A 32-bit procedure handed to it
 * (RegisterClass, SetWindowLong(GWL_WNDPROC)) is replaced by one of W32_BRIDGES 64-bit bridges that calls the 32-bit
 * code with the message parameters translated; a 64-bit procedure handed back to 32-bit code (GetWindowLong,
 * SetWindowLong's previous value) becomes the original 32-bit address, or a 32-bit thunk for Beer's own procedure. */

#define W32_BRIDGES 256
static u32 g_w32_bridge_proc[W32_BRIDGES];

/* lParam structures of the messages Beer itself sends, copied into 32-bit memory for the call and back after it. */
static u32 w32_wndproc_call(u32 proc, u64 hwnd, u32 message, u64 wparam, u64 lparam)
{
    u32 lp32 = (u32)lparam;
    u8 *copy = NULL;
    const u8 *host = (const u8 *)(uintptr_t)lparam;
    /* A sign-extended 32-bit value (MAKELPARAM(0, 65535) for PBM_SETRANGE, negative coordinates) is not a pointer. */
    const int widened_value = (lparam >> 31) == 0x1ffffffffull;
    if (lparam > 0xffffffffull && !widened_value) {
        switch (message) {
        case 0x0001: case 0x0081: {                         /* WM_CREATE / WM_NCCREATE: CREATESTRUCT */
            const WIN_CREATESTRUCT *cs = (const WIN_CREATESTRUCT *)host;
            u32 *c = w32_heap_alloc(48, 1);
            if (!c) break;
            c[0] = (u32)cs->lpCreateParams; c[1] = (u32)cs->hInstance; c[2] = (u32)cs->hMenu;
            c[3] = (u32)cs->hwndParent; c[4] = (u32)cs->cy; c[5] = (u32)cs->cx; c[6] = (u32)cs->y; c[7] = (u32)cs->x;
            c[8] = (u32)cs->style;
            c[9] = cs->lpszName <= 0xffffffffull ? (u32)cs->lpszName : 0;
            c[10] = cs->lpszClass <= 0xffffffffull ? (u32)cs->lpszClass : 0;
            c[11] = cs->dwExStyle;
            copy = (u8 *)c;
            break;
        }
        case 0x0046: case 0x0047: {                         /* WM_WINDOWPOSCHANGING / CHANGED: WINDOWPOS */
            u32 *c = w32_heap_alloc(28, 1);
            if (!c) break;
            c[0] = (u32)*(const u64 *)host; c[1] = (u32)*(const u64 *)(host + 8);
            memcpy(c + 2, host + 16, 20);
            copy = (u8 *)c;
            break;
        }
        case 0x0024:                                        /* WM_GETMINMAXINFO: five POINTs, same layout */
            if ((copy = w32_heap_alloc(40, 0))) memcpy(copy, host, 40);
            break;
        case 0x0083:                                        /* WM_NCCALCSIZE */
            if (wparam) {                                   /* NCCALCSIZE_PARAMS: three RECTs + PWINDOWPOS */
                if ((copy = w32_heap_alloc(52 + 28, 1))) {
                    memcpy(copy, host, 48);
                    const u8 *pos = (const u8 *)(uintptr_t)*(const u64 *)(host + 48);
                    if (pos) {
                        u32 *p = (u32 *)(copy + 52);
                        p[0] = (u32)*(const u64 *)pos; p[1] = (u32)*(const u64 *)(pos + 8);
                        memcpy(p + 2, pos + 16, 20);
                        *(u32 *)(copy + 48) = (u32)(uintptr_t)p;
                    }
                }
            } else if ((copy = w32_heap_alloc(16, 0))) memcpy(copy, host, 16);
            break;
        case 0x007c: case 0x007d:                           /* WM_STYLECHANGING / CHANGED: STYLESTRUCT */
            if ((copy = w32_heap_alloc(8, 0))) memcpy(copy, host, 8);
            break;
        case 0x000c: {                                      /* WM_SETTEXT: a wide string */
            const u16 *text = (const u16 *)host;
            size_t n = 0;
            while (text[n]) ++n;
            if ((copy = w32_heap_alloc((n + 1) * 2, 0))) memcpy(copy, text, (n + 1) * 2);
            break;
        }
        default: {
            static _Atomic(u32) reported[0x400];
            if (message >= 0x400 || !atomic_exchange(&reported[message], 1))
                fprintf(stderr, "[WOW32] message 0x%04x: lParam %#llx is above 4 GiB and not translated\n", message,
                        (unsigned long long)lparam);
            break;
        }
        }
        lp32 = (u32)(uintptr_t)copy;
    }
    const u32 argv[4] = { (u32)hwnd, message, (u32)wparam, lp32 };
    const u32 result = w32_call(proc, 4, argv);
    if (copy) {
        u8 *out = (u8 *)(uintptr_t)lparam;
        switch (message) {
        case 0x0024: memcpy(out, copy, 40); break;
        case 0x0046: memcpy(out + 16, copy + 8, 20); break;           /* the procedure may adjust x, y, cx, cy, flags */
        case 0x0083: memcpy(out, copy, wparam ? 48 : 16); break;
        case 0x007c: memcpy(out, copy, 8); break;
        default: break;
        }
        w32_heap_free(copy);
    }
    return result;
}

/* Results are sign-extended to 64 bits: LRESULT -1 (WM_CREATE failure) must stay -1.
 * Bridge n forwards to the 32-bit procedure stored in g_w32_bridge_proc[n] (generated: one function per slot). */
#define W32_BRIDGE(n) \
    static u64 __attribute__((ms_abi)) w32_bridge_##n(u64 hwnd, u32 message, u64 wparam, u64 lparam) \
    { return (u64)(s64)(s32)w32_wndproc_call(g_w32_bridge_proc[n], hwnd, message, wparam, lparam); }
W32_BRIDGE(0)
W32_BRIDGE(1)
W32_BRIDGE(2)
W32_BRIDGE(3)
W32_BRIDGE(4)
W32_BRIDGE(5)
W32_BRIDGE(6)
W32_BRIDGE(7)
W32_BRIDGE(8)
W32_BRIDGE(9)
W32_BRIDGE(10)
W32_BRIDGE(11)
W32_BRIDGE(12)
W32_BRIDGE(13)
W32_BRIDGE(14)
W32_BRIDGE(15)
W32_BRIDGE(16)
W32_BRIDGE(17)
W32_BRIDGE(18)
W32_BRIDGE(19)
W32_BRIDGE(20)
W32_BRIDGE(21)
W32_BRIDGE(22)
W32_BRIDGE(23)
W32_BRIDGE(24)
W32_BRIDGE(25)
W32_BRIDGE(26)
W32_BRIDGE(27)
W32_BRIDGE(28)
W32_BRIDGE(29)
W32_BRIDGE(30)
W32_BRIDGE(31)
W32_BRIDGE(32)
W32_BRIDGE(33)
W32_BRIDGE(34)
W32_BRIDGE(35)
W32_BRIDGE(36)
W32_BRIDGE(37)
W32_BRIDGE(38)
W32_BRIDGE(39)
W32_BRIDGE(40)
W32_BRIDGE(41)
W32_BRIDGE(42)
W32_BRIDGE(43)
W32_BRIDGE(44)
W32_BRIDGE(45)
W32_BRIDGE(46)
W32_BRIDGE(47)
W32_BRIDGE(48)
W32_BRIDGE(49)
W32_BRIDGE(50)
W32_BRIDGE(51)
W32_BRIDGE(52)
W32_BRIDGE(53)
W32_BRIDGE(54)
W32_BRIDGE(55)
W32_BRIDGE(56)
W32_BRIDGE(57)
W32_BRIDGE(58)
W32_BRIDGE(59)
W32_BRIDGE(60)
W32_BRIDGE(61)
W32_BRIDGE(62)
W32_BRIDGE(63)
W32_BRIDGE(64)
W32_BRIDGE(65)
W32_BRIDGE(66)
W32_BRIDGE(67)
W32_BRIDGE(68)
W32_BRIDGE(69)
W32_BRIDGE(70)
W32_BRIDGE(71)
W32_BRIDGE(72)
W32_BRIDGE(73)
W32_BRIDGE(74)
W32_BRIDGE(75)
W32_BRIDGE(76)
W32_BRIDGE(77)
W32_BRIDGE(78)
W32_BRIDGE(79)
W32_BRIDGE(80)
W32_BRIDGE(81)
W32_BRIDGE(82)
W32_BRIDGE(83)
W32_BRIDGE(84)
W32_BRIDGE(85)
W32_BRIDGE(86)
W32_BRIDGE(87)
W32_BRIDGE(88)
W32_BRIDGE(89)
W32_BRIDGE(90)
W32_BRIDGE(91)
W32_BRIDGE(92)
W32_BRIDGE(93)
W32_BRIDGE(94)
W32_BRIDGE(95)
W32_BRIDGE(96)
W32_BRIDGE(97)
W32_BRIDGE(98)
W32_BRIDGE(99)
W32_BRIDGE(100)
W32_BRIDGE(101)
W32_BRIDGE(102)
W32_BRIDGE(103)
W32_BRIDGE(104)
W32_BRIDGE(105)
W32_BRIDGE(106)
W32_BRIDGE(107)
W32_BRIDGE(108)
W32_BRIDGE(109)
W32_BRIDGE(110)
W32_BRIDGE(111)
W32_BRIDGE(112)
W32_BRIDGE(113)
W32_BRIDGE(114)
W32_BRIDGE(115)
W32_BRIDGE(116)
W32_BRIDGE(117)
W32_BRIDGE(118)
W32_BRIDGE(119)
W32_BRIDGE(120)
W32_BRIDGE(121)
W32_BRIDGE(122)
W32_BRIDGE(123)
W32_BRIDGE(124)
W32_BRIDGE(125)
W32_BRIDGE(126)
W32_BRIDGE(127)
W32_BRIDGE(128)
W32_BRIDGE(129)
W32_BRIDGE(130)
W32_BRIDGE(131)
W32_BRIDGE(132)
W32_BRIDGE(133)
W32_BRIDGE(134)
W32_BRIDGE(135)
W32_BRIDGE(136)
W32_BRIDGE(137)
W32_BRIDGE(138)
W32_BRIDGE(139)
W32_BRIDGE(140)
W32_BRIDGE(141)
W32_BRIDGE(142)
W32_BRIDGE(143)
W32_BRIDGE(144)
W32_BRIDGE(145)
W32_BRIDGE(146)
W32_BRIDGE(147)
W32_BRIDGE(148)
W32_BRIDGE(149)
W32_BRIDGE(150)
W32_BRIDGE(151)
W32_BRIDGE(152)
W32_BRIDGE(153)
W32_BRIDGE(154)
W32_BRIDGE(155)
W32_BRIDGE(156)
W32_BRIDGE(157)
W32_BRIDGE(158)
W32_BRIDGE(159)
W32_BRIDGE(160)
W32_BRIDGE(161)
W32_BRIDGE(162)
W32_BRIDGE(163)
W32_BRIDGE(164)
W32_BRIDGE(165)
W32_BRIDGE(166)
W32_BRIDGE(167)
W32_BRIDGE(168)
W32_BRIDGE(169)
W32_BRIDGE(170)
W32_BRIDGE(171)
W32_BRIDGE(172)
W32_BRIDGE(173)
W32_BRIDGE(174)
W32_BRIDGE(175)
W32_BRIDGE(176)
W32_BRIDGE(177)
W32_BRIDGE(178)
W32_BRIDGE(179)
W32_BRIDGE(180)
W32_BRIDGE(181)
W32_BRIDGE(182)
W32_BRIDGE(183)
W32_BRIDGE(184)
W32_BRIDGE(185)
W32_BRIDGE(186)
W32_BRIDGE(187)
W32_BRIDGE(188)
W32_BRIDGE(189)
W32_BRIDGE(190)
W32_BRIDGE(191)
W32_BRIDGE(192)
W32_BRIDGE(193)
W32_BRIDGE(194)
W32_BRIDGE(195)
W32_BRIDGE(196)
W32_BRIDGE(197)
W32_BRIDGE(198)
W32_BRIDGE(199)
W32_BRIDGE(200)
W32_BRIDGE(201)
W32_BRIDGE(202)
W32_BRIDGE(203)
W32_BRIDGE(204)
W32_BRIDGE(205)
W32_BRIDGE(206)
W32_BRIDGE(207)
W32_BRIDGE(208)
W32_BRIDGE(209)
W32_BRIDGE(210)
W32_BRIDGE(211)
W32_BRIDGE(212)
W32_BRIDGE(213)
W32_BRIDGE(214)
W32_BRIDGE(215)
W32_BRIDGE(216)
W32_BRIDGE(217)
W32_BRIDGE(218)
W32_BRIDGE(219)
W32_BRIDGE(220)
W32_BRIDGE(221)
W32_BRIDGE(222)
W32_BRIDGE(223)
W32_BRIDGE(224)
W32_BRIDGE(225)
W32_BRIDGE(226)
W32_BRIDGE(227)
W32_BRIDGE(228)
W32_BRIDGE(229)
W32_BRIDGE(230)
W32_BRIDGE(231)
W32_BRIDGE(232)
W32_BRIDGE(233)
W32_BRIDGE(234)
W32_BRIDGE(235)
W32_BRIDGE(236)
W32_BRIDGE(237)
W32_BRIDGE(238)
W32_BRIDGE(239)
W32_BRIDGE(240)
W32_BRIDGE(241)
W32_BRIDGE(242)
W32_BRIDGE(243)
W32_BRIDGE(244)
W32_BRIDGE(245)
W32_BRIDGE(246)
W32_BRIDGE(247)
W32_BRIDGE(248)
W32_BRIDGE(249)
W32_BRIDGE(250)
W32_BRIDGE(251)
W32_BRIDGE(252)
W32_BRIDGE(253)
W32_BRIDGE(254)
W32_BRIDGE(255)
typedef u64 (__attribute__((ms_abi)) *W32Bridge)(u64, u32, u64, u64);
static const W32Bridge g_w32_bridges[W32_BRIDGES] = {
    w32_bridge_0, w32_bridge_1, w32_bridge_2, w32_bridge_3, w32_bridge_4, w32_bridge_5, w32_bridge_6, w32_bridge_7,
    w32_bridge_8, w32_bridge_9, w32_bridge_10, w32_bridge_11, w32_bridge_12, w32_bridge_13, w32_bridge_14, w32_bridge_15,
    w32_bridge_16, w32_bridge_17, w32_bridge_18, w32_bridge_19, w32_bridge_20, w32_bridge_21, w32_bridge_22, w32_bridge_23,
    w32_bridge_24, w32_bridge_25, w32_bridge_26, w32_bridge_27, w32_bridge_28, w32_bridge_29, w32_bridge_30, w32_bridge_31,
    w32_bridge_32, w32_bridge_33, w32_bridge_34, w32_bridge_35, w32_bridge_36, w32_bridge_37, w32_bridge_38, w32_bridge_39,
    w32_bridge_40, w32_bridge_41, w32_bridge_42, w32_bridge_43, w32_bridge_44, w32_bridge_45, w32_bridge_46, w32_bridge_47,
    w32_bridge_48, w32_bridge_49, w32_bridge_50, w32_bridge_51, w32_bridge_52, w32_bridge_53, w32_bridge_54, w32_bridge_55,
    w32_bridge_56, w32_bridge_57, w32_bridge_58, w32_bridge_59, w32_bridge_60, w32_bridge_61, w32_bridge_62, w32_bridge_63,
    w32_bridge_64, w32_bridge_65, w32_bridge_66, w32_bridge_67, w32_bridge_68, w32_bridge_69, w32_bridge_70, w32_bridge_71,
    w32_bridge_72, w32_bridge_73, w32_bridge_74, w32_bridge_75, w32_bridge_76, w32_bridge_77, w32_bridge_78, w32_bridge_79,
    w32_bridge_80, w32_bridge_81, w32_bridge_82, w32_bridge_83, w32_bridge_84, w32_bridge_85, w32_bridge_86, w32_bridge_87,
    w32_bridge_88, w32_bridge_89, w32_bridge_90, w32_bridge_91, w32_bridge_92, w32_bridge_93, w32_bridge_94, w32_bridge_95,
    w32_bridge_96, w32_bridge_97, w32_bridge_98, w32_bridge_99, w32_bridge_100, w32_bridge_101, w32_bridge_102, w32_bridge_103,
    w32_bridge_104, w32_bridge_105, w32_bridge_106, w32_bridge_107, w32_bridge_108, w32_bridge_109, w32_bridge_110, w32_bridge_111,
    w32_bridge_112, w32_bridge_113, w32_bridge_114, w32_bridge_115, w32_bridge_116, w32_bridge_117, w32_bridge_118, w32_bridge_119,
    w32_bridge_120, w32_bridge_121, w32_bridge_122, w32_bridge_123, w32_bridge_124, w32_bridge_125, w32_bridge_126, w32_bridge_127,
    w32_bridge_128, w32_bridge_129, w32_bridge_130, w32_bridge_131, w32_bridge_132, w32_bridge_133, w32_bridge_134, w32_bridge_135,
    w32_bridge_136, w32_bridge_137, w32_bridge_138, w32_bridge_139, w32_bridge_140, w32_bridge_141, w32_bridge_142, w32_bridge_143,
    w32_bridge_144, w32_bridge_145, w32_bridge_146, w32_bridge_147, w32_bridge_148, w32_bridge_149, w32_bridge_150, w32_bridge_151,
    w32_bridge_152, w32_bridge_153, w32_bridge_154, w32_bridge_155, w32_bridge_156, w32_bridge_157, w32_bridge_158, w32_bridge_159,
    w32_bridge_160, w32_bridge_161, w32_bridge_162, w32_bridge_163, w32_bridge_164, w32_bridge_165, w32_bridge_166, w32_bridge_167,
    w32_bridge_168, w32_bridge_169, w32_bridge_170, w32_bridge_171, w32_bridge_172, w32_bridge_173, w32_bridge_174, w32_bridge_175,
    w32_bridge_176, w32_bridge_177, w32_bridge_178, w32_bridge_179, w32_bridge_180, w32_bridge_181, w32_bridge_182, w32_bridge_183,
    w32_bridge_184, w32_bridge_185, w32_bridge_186, w32_bridge_187, w32_bridge_188, w32_bridge_189, w32_bridge_190, w32_bridge_191,
    w32_bridge_192, w32_bridge_193, w32_bridge_194, w32_bridge_195, w32_bridge_196, w32_bridge_197, w32_bridge_198, w32_bridge_199,
    w32_bridge_200, w32_bridge_201, w32_bridge_202, w32_bridge_203, w32_bridge_204, w32_bridge_205, w32_bridge_206, w32_bridge_207,
    w32_bridge_208, w32_bridge_209, w32_bridge_210, w32_bridge_211, w32_bridge_212, w32_bridge_213, w32_bridge_214, w32_bridge_215,
    w32_bridge_216, w32_bridge_217, w32_bridge_218, w32_bridge_219, w32_bridge_220, w32_bridge_221, w32_bridge_222, w32_bridge_223,
    w32_bridge_224, w32_bridge_225, w32_bridge_226, w32_bridge_227, w32_bridge_228, w32_bridge_229, w32_bridge_230, w32_bridge_231,
    w32_bridge_232, w32_bridge_233, w32_bridge_234, w32_bridge_235, w32_bridge_236, w32_bridge_237, w32_bridge_238, w32_bridge_239,
    w32_bridge_240, w32_bridge_241, w32_bridge_242, w32_bridge_243, w32_bridge_244, w32_bridge_245, w32_bridge_246, w32_bridge_247,
    w32_bridge_248, w32_bridge_249, w32_bridge_250, w32_bridge_251, w32_bridge_252, w32_bridge_253, w32_bridge_254, w32_bridge_255,
};

/* The thunk entry behind a 32-bit address, when it is one of this layer's thunks; NULL otherwise. */
static const W32Thunk *w32_thunk_at(u32 address)
{
    const uintptr_t base = (uintptr_t)g_w32_thunk_code;
    if (address < base || address >= base + 12u * g_w32_thunk_count || (address - base) % 12u) return NULL;
    return &g_w32_thunks[(address - base) / 12u];
}

/* A 32-bit thunk for a 64-bit window procedure (four arguments, stdcall). */
static u32 w32_thunk_for_proc64(u64 proc)
{
    for (u32 i = 0; i < g_w32_thunk_count; ++i)
        if (g_w32_thunks[i].impl == (ImplFn)(uintptr_t)proc && !strcmp(g_w32_thunks[i].name, "<wndproc64>"))
            return (u32)(uintptr_t)(g_w32_thunk_code + 12 * i);
    if (g_w32_thunk_count == g_w32_thunk_capacity) return 0;
    W32Thunk *t = &g_w32_thunks[g_w32_thunk_count];
    t->dll = "user32.dll";
    t->name = "<wndproc64>";
    t->fn = w32_unimplemented;
    t->impl = (ImplFn)(uintptr_t)proc;
    t->arg_bytes = 16;
    u8 *code = g_w32_thunk_code + 12 * g_w32_thunk_count;
    code[0] = 0xb8;
    const u32 index = g_w32_thunk_count;
    memcpy(code + 1, &index, 4);
    code[5] = 0xea;
    const u32 target = (u32)(uintptr_t)g_w32_lowstub;
    memcpy(code + 6, &target, 4);
    code[10] = 0x33; code[11] = 0x00;
    return (u32)(uintptr_t)(g_w32_thunk_code + 12 * g_w32_thunk_count++);
}

/* 32-bit procedure -> what Beer stores. */
static u64 w32_proc_to_native(u32 proc)
{
    if (!proc) return 0;
    const W32Thunk *t = w32_thunk_at(proc);
    if (t && t->impl) return (u64)(uintptr_t)t->impl;      /* a 64-bit procedure handed back */
    pthread_mutex_lock(&g_w32_lock);
    int slot = -1;
    for (int i = 0; i < W32_BRIDGES && slot < 0; ++i) if (g_w32_bridge_proc[i] == proc) slot = i;
    for (int i = 0; i < W32_BRIDGES && slot < 0; ++i) if (!g_w32_bridge_proc[i]) { g_w32_bridge_proc[i] = proc; slot = i; }
    pthread_mutex_unlock(&g_w32_lock);
    if (slot < 0) { fprintf(stderr, "[WOW32] out of window procedure bridges\n"); return 0; }
    return (u64)(uintptr_t)g_w32_bridges[slot];
}

/* What Beer stores -> 32-bit procedure. */
static u32 w32_proc_from_native(u64 proc)
{
    if (!proc) return 0;
    for (int i = 0; i < W32_BRIDGES; ++i)
        if ((u64)(uintptr_t)g_w32_bridges[i] == proc) return g_w32_bridge_proc[i];
    return w32_thunk_for_proc64(proc);
}

static u64 w32_SetWindowLongW(const u32 *args)
{
    if ((s32)args[1] != -4) return (u32)impl_SetWindowLongPtrW(w32_widen(args[0]), (s32)args[1], args[2]);
    return w32_proc_from_native(impl_SetWindowLongPtrW(w32_widen(args[0]), -4, w32_proc_to_native(args[2])));
}
static u64 w32_GetWindowLongW(const u32 *args)
{
    const u64 value = impl_GetWindowLongPtrW(w32_widen(args[0]), (s32)args[1]);
    return (s32)args[1] == -4 ? w32_proc_from_native(value) : (u32)value;
}

/* A 32-bit CREATESTRUCT (12 DWORDs) as the native 64-bit structure Beer's own window procedures expect. */
static void w32_createstruct_native(const u32 *c, WIN_CREATESTRUCT *out)
{
    memset(out, 0, sizeof(*out));
    out->lpCreateParams = c[0]; out->hInstance = c[1]; out->hMenu = c[2]; out->hwndParent = w32_widen(c[3]);
    out->cy = (s32)c[4]; out->cx = (s32)c[5]; out->y = (s32)c[6]; out->x = (s32)c[7]; out->style = (s32)c[8];
    out->lpszName = c[9]; out->lpszClass = c[10]; out->dwExStyle = c[11];
}
/* Calls a native 64-bit window procedure for a 32-bit caller, translating lParam structures that differ in size. */
static u64 w32_call_native_proc(u64 (__attribute__((ms_abi)) *proc)(u64, u32, u64, u64), u64 hwnd, u32 message,
                                u64 wparam, u64 lparam)
{
    if ((message == 0x0081 || message == 0x0001) && lparam) {          /* WM_NCCREATE / WM_CREATE */
        WIN_CREATESTRUCT native;
        w32_createstruct_native((const u32 *)(uintptr_t)lparam, &native);
        return (u32)proc(hwnd, message, wparam, (u64)(uintptr_t)&native);
    }
    return (u32)proc(hwnd, message, wparam, lparam);
}
static u64 w32_DefWindowProcW(const u32 *args)
{
    return w32_call_native_proc((void *)impl_DefWindowProcW, w32_widen(args[0]), args[1], w32_widen(args[2]),
                                w32_widen(args[3]));
}
static u64 w32_CallWindowProcW(const u32 *args)
{
    const u32 proc = args[0];
    const u64 hwnd = w32_widen(args[1]), wparam = w32_widen(args[3]), lparam = w32_widen(args[4]);
    if (!proc) return (u32)impl_DefWindowProcW(hwnd, args[2], wparam, lparam);
    const W32Thunk *t = w32_thunk_at(proc);
    if (t && t->impl)
        return w32_call_native_proc((void *)t->impl, hwnd, args[2], wparam, lparam);
    return w32_call(proc, 4, args + 1);
}

/* WNDCLASSW: 40 bytes in 32-bit code; WNDCLASSEXW: 48. */
static u64 w32_register_class(const u32 *c, int ex)
{
    if (!c) { g_last_error = 87; return 0; }
    const u32 *f = ex ? c + 1 : c;                          /* WNDCLASSEX starts with cbSize */
    WIN_WNDCLASSEX wc = { .cbSize = sizeof(wc) };
    wc.style = f[0];
    wc.lpfnWndProc = w32_proc_to_native(f[1]);
    wc.cbClsExtra = (s32)f[2]; wc.cbWndExtra = (s32)f[3];
    wc.hInstance = f[4]; wc.hIcon = w32_widen(f[5]); wc.hCursor = w32_widen(f[6]); wc.hbrBackground = w32_widen(f[7]);
    wc.lpszMenuName = f[8]; wc.lpszClassName = f[9];
    wc.hIconSm = ex ? w32_widen(f[10]) : 0;
    return (u32)impl_RegisterClassExW(&wc);
}
static u64 w32_RegisterClassW(const u32 *args) { return w32_register_class(W32_PTR(0), 0); }
static u64 w32_RegisterClassExW(const u32 *args) { return w32_register_class(W32_PTR(0), 1); }

static u64 w32_GetClassInfoW(const u32 *args)
{
    if (!args[2]) { g_last_error = 87; return 0; }
    WIN_WNDCLASSEX wc;
    if (!impl_GetClassInfoExW(args[0], args[1], &wc)) return 0;
    /* Strings registered through this ABI already reside in low memory. */
    if (wc.lpszMenuName > UINT32_MAX || wc.lpszClassName > UINT32_MAX) {
        g_last_error = 50;
        return 0;
    }
    u32 out[10] = { wc.style, w32_proc_from_native(wc.lpfnWndProc),
        (u32)wc.cbClsExtra, (u32)wc.cbWndExtra, (u32)wc.hInstance,
        (u32)wc.hIcon, (u32)wc.hCursor, (u32)wc.hbrBackground,
        (u32)wc.lpszMenuName, (u32)wc.lpszClassName };
    memcpy(W32_PTR(2), out, sizeof(out));
    return 1;
}

static u64 w32_CharLowerW(const u32 *args)
{
    if (args[0] <= 0xffffu) return (u16)towlower((u16)args[0]);
    u16 *text = W32_PTR(0);
    for (size_t i = 0; text[i]; ++i) text[i] = (u16)towlower(text[i]);
    return args[0];
}

static u64 w32_GetKeyboardLayoutList(const u32 *args)
{
    s32 count = (s32)args[0];
    if (count < 0 || (count && !args[1])) { g_last_error = 87; return 0; }
    if (count) *(u32 *)W32_PTR(1) = (u32)impl_GetKeyboardLayout(0);
    return 1; /* Same single installed layout as GetKeyboardLayout. */
}

static u64 w32_ImmIsIME(const u32 *args)
{
    (void)args;
    /* No IME layouts are installed: the layout registry exposed above contains
     * only the ordinary keyboard returned by GetKeyboardLayout. */
    return 0;
}

/* MSG-based message functions (W): MSG is 28 bytes in 32-bit code. */
static void w32_msg_from_native(const WIN_MSG *msg, u32 *out)
{
    out[0] = (u32)msg->hwnd; out[1] = msg->message; out[2] = (u32)msg->wParam; out[3] = (u32)msg->lParam;
    out[4] = msg->time; out[5] = (u32)msg->pt.x; out[6] = (u32)msg->pt.y;
}
static u64 w32_PeekMessageW(const u32 *args)
{
    WIN_MSG msg;
    memset(&msg, 0, sizeof(msg));
    const u64 result = impl_PeekMessageW(&msg, w32_widen(args[1]), args[2], args[3], args[4]);
    if (result && getenv("BEER_W32_MSG_TRACE"))
        fprintf(stderr, "[W32MSG] peek hwnd=0x%llx msg=0x%x wp=0x%llx lp=0x%llx remove=%u\n",
                (unsigned long long)msg.hwnd, msg.message, (unsigned long long)msg.wParam,
                (unsigned long long)msg.lParam, args[4]);
    if (result && args[0]) w32_msg_from_native(&msg, W32_PTR(0));
    return (u32)result;
}
static u64 w32_GetMessageW(const u32 *args)
{
    WIN_MSG msg;
    memset(&msg, 0, sizeof(msg));
    const u64 result = impl_GetMessageW(&msg, w32_widen(args[1]), args[2], args[3]);
    if ((s32)result != -1 && args[0]) w32_msg_from_native(&msg, W32_PTR(0));
    return (u32)result;
}
static u64 w32_DispatchMessageW(const u32 *args)
{
    WIN_MSG msg;
    w32_msg_to_native(W32_PTR(0), &msg);
    return (u32)impl_DispatchMessageW(&msg);
}

/* CreateProcessW: STARTUPINFOW, PROCESS_INFORMATION and SECURITY_ATTRIBUTES differ between 32 and 64 bits (the
 * STARTUPINFO translation matches w32_CreateProcessA). A 32-bit child runs in its own loader through WoW32 again. */
static u64 w32_CreateProcessW(const u32 *args)
{
    const u8 *si32 = W32_PTR(8);
    u8 si64[104];
    memset(si64, 0, sizeof(si64));
    *(u32 *)si64 = 104;
    if (si32) {
        memcpy(si64 + 32, si32 + 16, 32);                   /* dwX .. dwFlags */
        memcpy(si64 + 64, si32 + 48, 4);                    /* wShowWindow, cbReserved2 */
        for (int i = 0; i < 3; ++i) *(u64 *)(si64 + 80 + 8 * i) = w32_widen(*(const u32 *)(si32 + 56 + 4 * i));
    }
    u8 process_sa[24], thread_sa[24], pi64[24];
    memset(pi64, 0, sizeof(pi64));
    const u64 ok = beer_CreateProcessW(W32_PTR(0), W32_PTR(1), w32_security_attributes(args[2], process_sa),
                                       w32_security_attributes(args[3], thread_sa), args[4], args[5], W32_PTR(6),
                                       W32_PTR(7), si32 ? si64 : NULL, pi64);
    u32 *pi32 = W32_PTR(9);
    if (pi32 && ok) {
        pi32[0] = (u32)*(u64 *)pi64;
        pi32[1] = (u32)*(u64 *)(pi64 + 8);
        pi32[2] = *(u32 *)(pi64 + 16);
        pi32[3] = *(u32 *)(pi64 + 20);
    }
    return (u32)ok;
}

/* Handle arrays are 4-byte HANDLEs in 32-bit code: widened before the wait. */
static u64 w32_wait_handles(const char *name, const u32 *args, u32 argc, u32 count_index, u32 array_index)
{
    const u32 count = args[count_index];
    if (count > 64) { g_last_error = 87; return 0xffffffffu; }    /* MAXIMUM_WAIT_OBJECTS */
    u64 handles[64];
    const u32 *in = (const u32 *)(uintptr_t)args[array_index];
    for (u32 i = 0; i < count; ++i) handles[i] = in ? w32_widen(in[i]) : 0;
    const ImplFn impl = find_impl(name);
    if (!impl) { g_last_error = 120; return 0xffffffffu; }
    u64 a[12] = {0};
    for (u32 i = 0; i < argc; ++i) a[i] = w32_widen(args[i]);
    a[array_index] = (u64)(uintptr_t)handles;
    return (u32)((W32Impl12)(void *)impl)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11]);
}
static u64 w32_WaitForMultipleObjects(const u32 *args) { return w32_wait_handles("WaitForMultipleObjects", args, 4, 0, 1); }
static u64 w32_MsgWaitForMultipleObjects(const u32 *args) { return w32_wait_handles("MsgWaitForMultipleObjects", args, 5, 0, 1); }
static u64 w32_MsgWaitForMultipleObjectsEx(const u32 *args) { return w32_wait_handles("MsgWaitForMultipleObjectsEx", args, 5, 0, 1); }

/* EnumResource{Types,Names,Languages}W: walks the PE32 resource directory and calls the 32-bit callback for each
 * entry; a FALSE return stops the enumeration. Named entries are copied to NUL-terminated heap strings. */
static u32 w32_res_entry_key(const u8 *root, u32 raw, u16 **to_free)
{
    *to_free = NULL;
    if (!(raw & 0x80000000u)) return raw;
    const u16 *name = (const u16 *)(root + (raw & 0x7fffffffu));
    const u16 length = name[0];
    u16 *copy = w32_heap_alloc((size_t)(length + 1) * 2, 1);
    if (!copy) return 0;
    memcpy(copy, name + 1, (size_t)length * 2);
    *to_free = copy;
    return (u32)(uintptr_t)copy;
}
/* level 0 = types, 1 = names (inside `type`), 2 = languages (inside `type` and `name`). */
static u64 w32_enum_resources(u32 module, int level, u32 type, u32 name, u32 proc, u32 lparam)
{
    const u8 *image = (const u8 *)(uintptr_t)(module ? module : g_w32_image);
    const W32NtHdrs *nt = (const W32NtHdrs *)(image + ((const DosHdr *)image)->lfanew);
    if (!proc) { g_last_error = 87; return 0; }
    if (!nt->opt.dirs[2].rva || !nt->opt.dirs[2].size) { g_last_error = 1812; return 0; }
    const u8 *root = image + nt->opt.dirs[2].rva;
    u32 offset = 0;
    char tbuf[256], nbuf[256];
    const uintptr_t keys[2] = { w32_res_name(type, tbuf, sizeof(tbuf)), w32_res_name(name, nbuf, sizeof(nbuf)) };
    for (int l = 0; l < level; ++l) {
        offset = w32_res_child(root, offset, keys[l], 0);
        if (offset == W32_RES_NOT_FOUND || !(offset & 0x80000000u)) {
            g_last_error = l == 0 ? 1813 : 1814;           /* ERROR_RESOURCE_TYPE/NAME_NOT_FOUND */
            return 0;
        }
        offset &= 0x7fffffffu;
    }
    const u16 *dir = (const u16 *)(root + offset);
    const u32 count = (u32)dir[6] + dir[7];
    const u32 *entries = (const u32 *)(root + offset + 16);
    for (u32 i = 0; i < count; ++i) {
        u16 *tmp;
        const u32 key = w32_res_entry_key(root, entries[2 * i], &tmp);
        u32 argv[5]; u32 argc;
        if (level == 0)      { argv[0] = module; argv[1] = key; argv[2] = lparam; argc = 3; }
        else if (level == 1) { argv[0] = module; argv[1] = type; argv[2] = key; argv[3] = lparam; argc = 4; }
        else                 { argv[0] = module; argv[1] = type; argv[2] = name; argv[3] = key & 0xffff;
                               argv[4] = lparam; argc = 5; }
        const u32 keep = w32_call(proc, argc, argv);
        if (tmp) w32_heap_free(tmp);
        if (!keep) break;
    }
    return 1;
}
static u64 w32_EnumResourceTypesW(const u32 *a)     { return w32_enum_resources(a[0], 0, 0, 0, a[1], a[2]); }
static u64 w32_EnumResourceNamesW(const u32 *a)     { return w32_enum_resources(a[0], 1, a[1], 0, a[2], a[3]); }
static u64 w32_EnumResourceLanguagesW(const u32 *a) { return w32_enum_resources(a[0], 2, a[1], a[2], a[3], a[4]); }

/* ---- FindFirstFile handles: the native handle is a 64-bit pointer, so the guest gets a table index ---- */
#define W32_FINDH_MAX  256
#define W32_FINDH_BASE 0x7c000000u
static u64 g_w32_findh[W32_FINDH_MAX];
static pthread_mutex_t g_w32_findh_lock = PTHREAD_MUTEX_INITIALIZER;
typedef u64 (__attribute__((ms_abi)) *W32Impl2)(u64, u64);
static u64 w32_findh_first(const char *name, const u32 *args)
{
    const ImplFn impl = find_impl(name);
    if (!impl) { g_last_error = 120; return 0xffffffffu; }
    const u64 native = ((W32Impl2)(void *)impl)(args[0], args[1]);
    if (native == (u64)-1 || native == 0) return 0xffffffffu;
    pthread_mutex_lock(&g_w32_findh_lock);
    for (u32 i = 0; i < W32_FINDH_MAX; ++i) if (!g_w32_findh[i]) {
        g_w32_findh[i] = native;
        pthread_mutex_unlock(&g_w32_findh_lock);
        return W32_FINDH_BASE + i;
    }
    pthread_mutex_unlock(&g_w32_findh_lock);
    ((W32Impl2)(void *)find_impl("FindClose"))(native, 0);
    g_last_error = 4;                                       /* ERROR_TOO_MANY_OPEN_FILES */
    return 0xffffffffu;
}
static u64 w32_findh_get(u32 handle, int release)
{
    if (handle < W32_FINDH_BASE || handle >= W32_FINDH_BASE + W32_FINDH_MAX) return 0;
    pthread_mutex_lock(&g_w32_findh_lock);
    const u64 native = g_w32_findh[handle - W32_FINDH_BASE];
    if (release) g_w32_findh[handle - W32_FINDH_BASE] = 0;
    pthread_mutex_unlock(&g_w32_findh_lock);
    return native;
}
static u64 w32_FindFirstFileW(const u32 *args) { return w32_findh_first("FindFirstFileW", args); }
static u64 w32_FindFirstFileA(const u32 *args) { return w32_findh_first("FindFirstFileA", args); }
static u64 w32_findh_next(const char *name, const u32 *args)
{
    const u64 native = w32_findh_get(args[0], 0);
    if (!native) { g_last_error = 6; return 0; }            /* ERROR_INVALID_HANDLE */
    return (u32)((W32Impl2)(void *)find_impl(name))(native, args[1]);
}
static u64 w32_FindNextFileW(const u32 *args) { return w32_findh_next("FindNextFileW", args); }
static u64 w32_FindNextFileA(const u32 *args) { return w32_findh_next("FindNextFileA", args); }
static u64 w32_FindClose(const u32 *args)
{
    const u64 native = w32_findh_get(args[0], 1);
    if (!native) { g_last_error = 6; return 0; }
    return (u32)((W32Impl2)(void *)find_impl("FindClose"))(native, 0);
}

/* ---- advapi32 ANSI registry entry points (widened to the wide implementations) ---- */
static u16 *w32_widen_ansi(u32 text, u32 length /* bytes, or 0xffffffff for NUL-terminated */)
{
    if (!text) return NULL;
    const char *s = (const char *)(uintptr_t)text;
    if (length == 0xffffffffu) length = (u32)strlen(s);
    u16 *w = w32_heap_alloc((size_t)(length + 1) * 2, 1);
    if (!w) return NULL;
    for (u32 i = 0; i < length; ++i) w[i] = (unsigned char)s[i];
    return w;
}
static u64 w32_RegOpenKeyA(const u32 *args)                 /* (key, subkey, result) */
{
    u16 *sub = w32_widen_ansi(args[1], 0xffffffffu);
    const u32 call[5] = { args[0], (u32)(uintptr_t)sub, 0, 0xF003Fu, args[2] };
    const u64 r = w32_RegOpenKeyExW(call);
    if (sub) w32_heap_free(sub);
    return r;
}
static u64 w32_RegCreateKeyExA(const u32 *args)
{
    u16 *sub = w32_widen_ansi(args[1], 0xffffffffu), *cls = w32_widen_ansi(args[3], 0xffffffffu);
    u32 call[9];
    memcpy(call, args, sizeof(call));
    call[1] = (u32)(uintptr_t)sub; call[3] = (u32)(uintptr_t)cls;
    const u64 r = w32_RegCreateKeyExW(call);
    if (sub) w32_heap_free(sub);
    if (cls) w32_heap_free(cls);
    return r;
}
static u64 w32_RegSetValueExA(const u32 *args)              /* (key, name, reserved, type, data, size) */
{
    u16 *name = w32_widen_ansi(args[1], 0xffffffffu);
    u32 call[6];
    memcpy(call, args, sizeof(call));
    call[1] = (u32)(uintptr_t)name;
    u16 *data = NULL;
    if ((args[3] == 1 || args[3] == 2 || args[3] == 7) && args[4]) {   /* REG_SZ / EXPAND_SZ / MULTI_SZ */
        data = w32_widen_ansi(args[4], args[5]);
        call[4] = (u32)(uintptr_t)data;
        call[5] = args[5] * 2;
    }
    const u64 r = w32_forward("RegSetValueExW", call, 6);
    if (name) w32_heap_free(name);
    if (data) w32_heap_free(data);
    return r;
}

/* ---- security descriptors/ACLs: 32-bit layouts (SECURITY_DESCRIPTOR is 20 bytes here, 40 natively) ---- */
static u64 w32_InitializeSecurityDescriptor(const u32 *args)
{
    u8 *sd = W32_PTR(0);
    if (!sd || args[1] != 1) { g_last_error = args[0] ? 1306 : 87; return 0; }   /* ERROR_UNKNOWN_REVISION */
    memset(sd, 0, 20);
    sd[0] = 1;
    return 1;
}
static u64 w32_SetSecurityDescriptorDacl(const u32 *args)   /* (sd, present, dacl, defaulted) */
{
    u8 *sd = W32_PTR(0);
    if (!sd || sd[0] != 1) { g_last_error = 1338; return 0; }                    /* ERROR_INVALID_SECURITY_DESCR */
    u16 *control = (u16 *)(sd + 2);
    if (!args[1]) { *control &= (u16)~0x4u; return 1; }
    *control |= 0x4u;
    *control = args[3] ? (u16)(*control | 0x8u) : (u16)(*control & ~0x8u);
    *(u32 *)(sd + 16) = args[2];
    return 1;
}
/* Linux file permissions have no Windows ACL model: the descriptor is validated and accepted, not applied. */
static u64 w32_SetFileSecurityW(const u32 *args)
{
    const u8 *sd = W32_PTR(2);
    if (!args[0] || !sd || sd[0] != 1) { g_last_error = 87; return 0; }
    return 1;
}
static u64 w32_InitializeAcl(const u32 *args)               /* (acl, size, revision) */
{
    u8 *acl = W32_PTR(0);
    if (!acl || args[1] < 8 || args[1] > 0xffffu || args[2] < 2 || args[2] > 4) { g_last_error = 87; return 0; }
    memset(acl, 0, 8);
    acl[0] = (u8)args[2];
    *(u16 *)(acl + 2) = (u16)args[1];
    return 1;
}
static u64 w32_AddAccessAllowedAce(const u32 *args)         /* (acl, revision, mask, sid) */
{
    u8 *acl = W32_PTR(0);
    const u8 *sid = W32_PTR(3);
    if (!acl || !sid || sid[0] != 1 || sid[1] > 15) { g_last_error = 87; return 0; }
    const u32 sid_len = 8 + 4u * sid[1], size = 8 + sid_len;
    u16 *acl_size = (u16 *)(acl + 2), *count = (u16 *)(acl + 4);
    u32 used = 8;
    for (u16 i = 0; i < *count; ++i) used += *(u16 *)(acl + used + 2);
    if (used + size > *acl_size) { g_last_error = 122; return 0; }               /* ERROR_INSUFFICIENT_BUFFER */
    u8 *ace = acl + used;
    ace[0] = 0; ace[1] = 0;                                 /* ACCESS_ALLOWED_ACE_TYPE, no flags */
    *(u16 *)(ace + 2) = (u16)size;
    *(u32 *)(ace + 4) = args[2];
    memcpy(ace + 8, sid, sid_len);
    ++*count;
    return 1;
}
static u64 w32_GetAce(const u32 *args)                      /* (acl, index, ace**) */
{
    u8 *acl = W32_PTR(0);
    if (!acl || !args[2]) { g_last_error = 87; return 0; }
    const u16 count = *(u16 *)(acl + 4);
    if (args[1] >= count) { g_last_error = 87; return 0; }
    u32 at = 8;
    for (u32 i = 0; i < args[1]; ++i) at += *(u16 *)(acl + at + 2);
    *(u32 *)W32_PTR(2) = (u32)(uintptr_t)(acl + at);
    return 1;
}
static u64 w32_LookupAccountSidW(const u32 *args)
{
    (void)args;
    g_last_error = 1332;                                    /* ERROR_NONE_MAPPED: Beer has no account database */
    return 0;
}

/* IsBadWritePtr: true when any byte of the range is not in a writable mapping. */
static u64 w32_IsBadWritePtr(const u32 *args)
{
    const uintptr_t start = args[0], end = start + args[1];
    if (!args[1]) return 0;
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) return 1;
    char line[512];
    uintptr_t covered = start;
    while (covered < end && fgets(line, sizeof(line), maps)) {
        unsigned long lo, hi; char perms[8];
        if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) != 3) continue;
        if (covered >= lo && covered < hi) {
            if (perms[1] != 'w') break;
            covered = hi;
        }
    }
    fclose(maps);
    return covered < end;
}

/* ---- guest threads: a Beer thread whose body creates the TEB32 and runs the 32-bit start routine ---- */
typedef struct { u32 start, param; size_t stack; } W32ThreadStart;
static u64 __attribute__((ms_abi)) w32_thread_body(u64 ctx_pointer)
{
    W32ThreadStart ctx = *(W32ThreadStart *)(uintptr_t)ctx_pointer;
    free((void *)(uintptr_t)ctx_pointer);
    u64 beer_teb;
    __asm__ volatile("mov %%gs:0x30, %0" : "=r"(beer_teb));
    w32_thread_init(ctx.stack);                             /* TEB64+TEB32 below 4 GiB, GS now points at them */
    const u32 code = w32_call(ctx.start, 1, &ctx.param);
    syscall(SYS_arch_prctl, ARCH_SET_GS, beer_teb);         /* the thread wrapper's own cleanup needs its TEB */
    return code;
}
static u64 w32_CreateThread(const u32 *args)   /* (attributes, stack, start, parameter, flags, id*) */
{
    W32ThreadStart *ctx = malloc(sizeof(*ctx));
    if (!ctx) { g_last_error = 8; return 0; }
    ctx->start = args[2]; ctx->param = args[3];
    size_t stack = args[1];
    if (stack < (256u << 10)) stack = 1u << 20;             /* the guest stack lives below 4 GiB: keep it modest */
    ctx->stack = stack;
    u32 id = 0;
    const u64 handle = impl_CreateThread(0, args[1], (u64)(uintptr_t)w32_thread_body, (u64)(uintptr_t)ctx, args[4], &id);
    if (!handle) { free(ctx); return 0; }
    if (args[5]) *(u32 *)W32_PTR(5) = id;
    return (u32)handle;
}

/* ---- WININET: Beer has no WinINet. Every entry point fails the way an unreachable network does, so a program
 * that downloads optionally (Inno Download Plugin) loads and then reports a failed download. ---- */
#define W32_ERROR_INTERNET_CANNOT_CONNECT 12029
static u64 w32_wininet_fail(const u32 *args)
{
    (void)args;
    g_last_error = W32_ERROR_INTERNET_CANNOT_CONNECT;
    return 0;
}

/* ---- process heaps: one shared 32-bit allocator; HeapCreate hands out distinct handles over it ---- */
static u64 w32_HeapCreate(const u32 *args) { (void)args; return 0x00030000u; }
static u64 w32_HeapDestroy(const u32 *args) { return args[0] != 0; }   /* blocks stay in the shared arena */

/* ---- environment blocks: copied into the 32-bit heap (the native block is a 64-bit pointer) ---- */
static u64 w32_env_block(int wide)
{
    void *native = wide ? (void *)impl_GetEnvironmentStringsW() : (void *)impl_GetEnvironmentStringsA();
    if (!native) return 0;
    size_t size = 0;
    if (wide) { const u16 *p = native; while (p[size] || p[size + 1]) ++size; size = (size + 2) * 2; }
    else { const char *p = native; while (p[size] || p[size + 1]) ++size; size += 2; }
    void *copy = w32_heap_alloc(size, 0);
    if (copy) memcpy(copy, native, size);
    impl_FreeEnvironmentStrings((u64)(uintptr_t)native);
    return (u32)(uintptr_t)copy;
}
static u64 w32_GetEnvironmentStrings(const u32 *args)  { (void)args; return w32_env_block(0); }
static u64 w32_GetEnvironmentStringsW(const u32 *args) { (void)args; return w32_env_block(1); }
static u64 w32_FreeEnvironmentStrings(const u32 *args) { if (args[0]) w32_heap_free(W32_PTR(0)); return 1; }

/* ---- small kernel32/user32/oleaut32 entries whose native versions do not fit a 32-bit call ---- */
static u64 w32_UnhandledExceptionFilter(const u32 *args)
{
    if (g_w32_unhandled_filter) return w32_call(g_w32_unhandled_filter, 1, args);
    return 1;                                               /* EXCEPTION_EXECUTE_HANDLER */
}
static u64 w32_LocalFileTimeToFileTime(const u32 *args)
{
    if (!args[0] || !args[1]) { g_last_error = 87; return 0; }
    memcpy(W32_PTR(1), W32_PTR(0), 8);                      /* Beer's local time zone is UTC */
    return 1;
}
/* Linux keeps no creation time and Beer's file handles carry no timestamp path: the call succeeds without changing
 * the times (installers set them cosmetically). */
static u64 w32_SetFileTime(const u32 *args) { (void)args; return 1; }
static u64 w32_EnumSystemLocalesA(const u32 *args)         /* (proc, flags): reports the one locale Beer has */
{
    if (!args[0]) { g_last_error = 87; return 0; }
    char *id = w32_heap_alloc(16, 0);
    if (!id) { g_last_error = 8; return 0; }
    snprintf(id, 16, "00000409");
    const u32 argv[1] = { (u32)(uintptr_t)id };
    w32_call(args[0], 1, argv);
    w32_heap_free(id);
    return 1;
}
static u64 w32_EnumSystemLocalesW(const u32 *args)         /* (proc, flags): as the A variant, UTF-16 */
{
    if (!args[0]) { g_last_error = 87; return 0; }
    u16 *id = w32_heap_alloc(18 * sizeof(u16), 0);
    if (!id) { g_last_error = 8; return 0; }
    static const char text[] = "00000409";
    for (size_t i = 0; i < sizeof(text); ++i) id[i] = (u8)text[i];
    const u32 argv[1] = { (u32)(uintptr_t)id };
    w32_call(args[0], 1, argv);
    w32_heap_free(id);
    return 1;
}
static u64 w32_EnumCalendarInfoW(const u32 *args) { (void)args; g_last_error = 120; return 0; }
static u64 w32_SignalObjectAndWait(const u32 *args) { (void)args; g_last_error = 120; return 0xffffffffu; }
static u64 w32_ExitWindowsEx(const u32 *args) { (void)args; g_last_error = 5; return 0; }
static u64 w32_DialogBoxParamW(const u32 *args) { (void)args; g_last_error = 1813; return 0xffffffffu; }
/* SAFEARRAY has a different layout in a 32-bit process: not supported yet. */
static u64 w32_SafeArrayCreate(const u32 *args) { (void)args; g_last_error = 120; return 0; }
static u64 w32_SafeArrayHresult(const u32 *args) { (void)args; return 0x80070057u; }   /* E_INVALIDARG */

/* ---- stack-balanced failures for imports without a 32-bit-safe implementation (see wow32_signatures.inc) ---- */
static u64 w32_unsupported(const u32 *args) { (void)args; g_last_error = 120; return 0; }   /* ERROR_CALL_NOT_IMPLEMENTED */
static u64 w32_unsupported_hr(const u32 *args)   /* HRESULT functions; an out pointer, if any, is cleared by the caller's init */
{
    (void)args;
    return 0x80004001u;                                     /* E_NOTIMPL */
}
static u64 w32_unsupported_net(const u32 *args) { (void)args; return 50; }                /* ERROR_NOT_SUPPORTED */

/* user32/gdi32/ole32 entries with 32-bit layouts that differ from the native ones. */
static u64 w32_BeginPaint(const u32 *args)                 /* PAINTSTRUCT: 64 bytes here, 72 natively */
{
    u8 native[72] = {0};
    const u64 hdc = impl_BeginPaint(w32_widen(args[0]), (void *)native);
    u32 *ps = W32_PTR(1);
    if (ps) {
        ps[0] = (u32)hdc; ps[1] = *(u32 *)(native + 8);
        memcpy(ps + 2, native + 12, 16);                    /* rcPaint */
        ps[6] = *(u32 *)(native + 28); ps[7] = *(u32 *)(native + 32);
        memset(ps + 8, 0, 32);
    }
    return (u32)hdc;
}
static u64 w32_EndPaint(const u32 *args)
{
    u8 native[72] = {0};
    const u32 *ps = W32_PTR(1);
    if (ps) {
        *(u64 *)native = w32_widen(ps[0]); *(u32 *)(native + 8) = ps[1];
        memcpy(native + 12, ps + 2, 16);
    }
    return (u32)impl_EndPaint(w32_widen(args[0]), (void *)native);
}
static u64 w32_PeekMessageA(const u32 *args) { return w32_PeekMessageW(args); }
/* SendMessageW: EM_STREAMIN (a RichEdit's content, Inno Setup's license and information pages) calls the program's
 * 32-bit EDITSTREAMCALLBACK (DWORD_PTR cookie, LPBYTE buffer, LONG bytes, LONG *done) with a guest buffer until it
 * delivers nothing; the 32-bit EDITSTREAM is { cookie, dwError, callback }. Everything else is forwarded. */
static u64 beer_ctl_stream_in(u64 hw, const u8 *data, size_t n, u32 format);
static u64 w32_SendMessageW(const u32 *args)
{
    if (args[1] == 0x0449 && args[3]) {
        u32 *stream = W32_PTR(3);
        u8 *chunk = w32_heap_alloc(4096 + 8, 1);
        size_t used = 0, cap = 0;
        u8 *data = NULL;
        u32 error = 0;
        for (int guard = 0; chunk && stream[2] && guard < 65536; ++guard) {
            u32 *done = (u32 *)(chunk + 4096);
            *done = 0;
            const u32 call[4] = { stream[0], (u32)(uintptr_t)chunk, 4096, (u32)(uintptr_t)done };
            error = w32_call(stream[2], 4, call);
            if (error || (s32)*done <= 0) break;
            if (cap - used < *done) { u8 *grown = realloc(data, cap + 65536); if (!grown) break; data = grown; cap += 65536; }
            memcpy(data + used, chunk, *done);
            used += *done;
        }
        stream[1] = error;
        if (chunk) w32_heap_free(chunk);
        const u64 r = data ? beer_ctl_stream_in(w32_widen(args[0]), data, used, args[2]) : 0;
        free(data);
        return (u32)r;
    }
    return w32_forward("SendMessageW", args, 4);
}
static u64 w32_SendMessageTimeoutW(const u32 *args)        /* (hwnd, msg, wp, lp, flags, timeout, DWORD_PTR*) */
{
    const u64 r = w32_forward("SendMessageW", args, 4);
    if (args[6]) *(u32 *)W32_PTR(6) = (u32)r;
    return 1;
}
static u64 w32_WaitForMultipleObjectsEx(const u32 *args) { return w32_wait_handles("WaitForMultipleObjectsEx", args, 5, 0, 1); }
static u64 w32_CoTaskMemAlloc(const u32 *args) { return (u32)(uintptr_t)w32_heap_alloc(args[0] ? args[0] : 1, 0); }
static u64 w32_IsEqualGUID(const u32 *args) { return args[0] && args[1] && !memcmp(W32_PTR(0), W32_PTR(1), 16); }
static u64 w32_EqualSid(const u32 *args)
{
    const u8 *a = W32_PTR(0), *b = W32_PTR(1);
    if (!a || !b || a[1] > 15 || a[1] != b[1]) return 0;
    return !memcmp(a, b, 8 + 4u * a[1]);
}
static u64 w32_IsWindowUnicode(const u32 *args) { (void)args; return 1; }
static u64 w32_GetDlgCtrlID(const u32 *args) { return (u32)impl_GetDlgCtrlID(w32_widen(args[0])); }

/* ---- file mappings: the guest can only address the low 4 GiB, so views are placed there explicitly ---- */
static u64 w32_CreateFileMappingA(const u32 *args) { return w32_call_sa("CreateFileMappingA", args, 6, 1); }
static u64 w32_CreateFileMappingW(const u32 *args) { return w32_call_sa("CreateFileMappingW", args, 6, 1); }
static u64 w32_MapViewOfFileEx(const u32 *args)   /* (mapping, access, offset_high, offset_low, bytes, base) */
{
    const u32 access = args[1];
    const u32 protection = (access & 0x20u) ? 0x40u : ((access & 0x2u) || (access & 0xF001Fu) == 0xF001Fu) ? 0x04u
                         : (access & 0x1u) ? 0x08u : 0x02u;
    int64_t offset = (int64_t)(((u64)args[2] << 32) | args[3]);
    uintptr_t candidate = args[5] ? args[5] : 0;
    uint32_t status = 0;
    void *address = NULL;
    size_t size = 0;
    for (int attempt = 0; attempt < 64; ++attempt) {
        if (!args[5]) {   /* the highest free gap (a view of the whole section is sized by the section; 16 MiB guess) */
            const uintptr_t span = args[4] ? ((uintptr_t)args[4] + 0xffffu) & ~(uintptr_t)0xffffu : (0x1000000u << (attempt / 8));
            pthread_mutex_lock(&g_w32_lock);
            candidate = w32_find_gap(span, 0x20000000, 0x7e000000);
            if (!candidate) candidate = w32_find_gap(span, 0x10000, 0x20000000);
            pthread_mutex_unlock(&g_w32_lock);
            if (!candidate) break;
        }
        address = (void *)candidate;
        size = args[4];
        status = beer_nt_section_map(w32_widen(args[0]), &address, 0, &offset, &size, 1, 0, protection);
        if (status == 0 || args[5] || status == BEER_STATUS_INVALID_HANDLE) break;
    }
    if (status) { g_last_error = status == BEER_STATUS_INVALID_HANDLE ? 6u : 8u; return 0; }
    if ((uintptr_t)address + size > W32_LIMIT) {            /* does not fit below 2 GiB: undo */
        beer_nt_section_unmap(address);
        g_last_error = 8;
        return 0;
    }
    pthread_mutex_lock(&g_w32_lock);
    if (!args[5] && (uintptr_t)address < g_w32_top) g_w32_top = (uintptr_t)address;
    pthread_mutex_unlock(&g_w32_lock);
    g_last_error = 0;
    return (u32)(uintptr_t)address;
}
static u64 w32_MapViewOfFile(const u32 *args)
{
    const u32 full[6] = { args[0], args[1], args[2], args[3], args[4], 0 };
    return w32_MapViewOfFileEx(full);
}

static u64 w32_CharNextA(const u32 *args)
{
    const unsigned char *s = W32_PTR(0);
    return s && *s ? args[0] + 1 : args[0];                 /* single-byte code page */
}
static u64 w32_GetDiskFreeSpaceA(const u32 *args)
{
    u16 *wide = w32_widen_ansi(args[0], 0xffffffffu);
    u32 call[5];
    memcpy(call, args, sizeof(call));
    call[0] = (u32)(uintptr_t)wide;
    const u64 r = w32_GetDiskFreeSpaceW(call);
    if (wide) w32_heap_free(wide);
    return r;
}
static u64 w32_VariantChangeTypeEx(const u32 *args)
{
    fprintf(stderr, "[WOW32] VariantChangeTypeEx(vt %u -> %u) is not implemented\n",
            args[1] ? *(const u16 *)W32_PTR(1) : 0, args[4] & 0xffff);
    return 0x80020005u;                                     /* DISP_E_TYPEMISMATCH */
}
static u64 w32_lstrcpynA(const u32 *args)                   /* (dest, source, max) */
{
    char *dst = W32_PTR(0);
    const char *src = W32_PTR(1);
    if (!dst || !src || (s32)args[2] <= 0) return 0;
    s32 i = 0;
    for (; i < (s32)args[2] - 1 && src[i]; ++i) dst[i] = src[i];
    dst[i] = 0;
    return args[0];
}
static u64 w32_LoadStringA(const u32 *args)                 /* (module, id, buffer, size) */
{
    char *buffer = W32_PTR(2);
    if (!buffer || !args[3]) { g_last_error = 87; return 0; }
    const u32 record = (u32)w32_find_resource(args[0], 6 /* RT_STRING */, args[1] / 16 + 1, 0);
    if (!record) return 0;
    const u8 *image = (const u8 *)(uintptr_t)(args[0] ? args[0] : g_w32_image);
    const u16 *block = (const u16 *)(image + ((const u32 *)(uintptr_t)record)[0]);
    const u32 block_chars = ((const u32 *)(uintptr_t)record)[1] / 2;
    u32 at = 0;
    for (u32 i = 0; i < args[1] % 16; ++i) {
        if (at >= block_chars) { g_last_error = 1814; return 0; }
        at += 1 + block[at];
    }
    if (at >= block_chars) { g_last_error = 1814; return 0; }
    u32 length = block[at];
    if (at + 1 + length > block_chars) length = block_chars - at - 1;
    if (!length) { g_last_error = 1814; buffer[0] = 0; return 0; }
    if (length > args[3] - 1) length = args[3] - 1;
    for (u32 i = 0; i < length; ++i) buffer[i] = block[at + 1 + i] < 0x100 ? (char)block[at + 1 + i] : '?';
    buffer[length] = 0;
    return length;
}

/* ANSI entry points over their wide siblings: string arguments (bit mask) are widened into the 32-bit heap. */
static u64 w32_via_wide(const char *name, const u32 *args, u32 argc, u32 string_mask)
{
    u32 call[8];
    u16 *owned[8] = {0};
    memcpy(call, args, argc * 4);
    for (u32 i = 0; i < argc; ++i)
        if (string_mask & (1u << i)) { owned[i] = w32_widen_ansi(args[i], 0xffffffffu); call[i] = (u32)(uintptr_t)owned[i]; }
    const u64 r = w32_forward(name, call, argc);
    for (u32 i = 0; i < argc; ++i) if (owned[i]) w32_heap_free(owned[i]);
    return r;
}
static u64 w32_MoveFileA(const u32 *args)        { return w32_via_wide("MoveFileW", args, 2, 3); }
static u64 w32_MoveFileExA(const u32 *args)      { return w32_via_wide("MoveFileExW", args, 3, 3); }
static u64 w32_RemoveDirectoryA(const u32 *args) { return w32_via_wide("RemoveDirectoryW", args, 1, 1); }
static u64 w32_DeleteFileA(const u32 *args) { return w32_via_wide("DeleteFileW", args, 1, 1); }
static u64 w32_RegDeleteNotSupported(const u32 *args) { (void)args; return 120; }   /* Beer's registry has no delete yet */
/* GetTimeFormat: the system default time format ("HH:mm:ss"); custom picture strings are not interpreted. */
static u64 w32_time_format(const u32 *args, int wide)       /* (locale, flags, SYSTEMTIME*, format, buffer, size) */
{
    const u16 *t = W32_PTR(2);
    u16 h, m, s;
    if (t) { h = t[4]; m = t[5]; s = t[6]; }
    else { const time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm); h = tm.tm_hour; m = tm.tm_min; s = tm.tm_sec; }
    char text[16];
    snprintf(text, sizeof(text), "%02u:%02u:%02u", h % 100, m % 100, s % 100);
    const u32 need = (u32)strlen(text) + 1;
    if (!args[5]) return need;
    if (args[5] < need) { g_last_error = 122; return 0; }
    for (u32 i = 0; i < need; ++i) {
        if (wide) ((u16 *)W32_PTR(4))[i] = (unsigned char)text[i]; else ((char *)W32_PTR(4))[i] = text[i];
    }
    return need;
}
static u64 w32_GetTimeFormatA(const u32 *args) { return w32_time_format(args, 0); }
static u64 w32_GetTimeFormatW(const u32 *args) { return w32_time_format(args, 1); }

/* ---- UxTheme / IMM / flat scroll bars / pointer encoding ----
 * Beer draws no visual styles: no theme is active, so theme handles are NULL and every query fails the way an
 * unthemed Windows session's does. (VCL calls IsThemeActive through a pointer it resolved at start-up.) */
static u64 w32_zero(const u32 *args) { (void)args; return 0; }
static u64 w32_E_FAIL(const u32 *args) { (void)args; return 0x80004005u; }
static u64 w32_identity(const u32 *args) { return args[0]; }       /* Encode/DecodePointer: no encoding */
static u64 w32_FlsAlloc(const u32 *args) { (void)args; return w32_TlsAlloc(args); }
static u64 w32_FlsFree(const u32 *args) { return w32_TlsFree(args); }
static u64 w32_FlsGetValue(const u32 *args) { return w32_TlsGetValue(args); }
static u64 w32_FlsSetValue(const u32 *args) { return w32_TlsSetValue(args); }
static u64 w32_GetSystemWow64DirectoryW(const u32 *args) { (void)args; g_last_error = 120; return 0; }
static u64 w32_VirtualQueryEx(const u32 *args)             /* (process, address, buffer, length): own process only */
{
    if (args[0] != 0xffffffffu) { g_last_error = 5; return 0; }
    return w32_VirtualQuery(args + 1);
}
