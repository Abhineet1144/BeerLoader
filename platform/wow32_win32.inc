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
    {"dwmapi", 0xFF390000u}, {"setupapi", 0xFF3D0000u}, {"crypt32", 0xFF410000u}, {"wintrust", 0xFF450000u},
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
        if (!strcasecmp(key, g_w32_system_modules[i].name)) return g_w32_system_modules[i].handle;
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

/* Only system DLLs load: loading 32-bit DLL images is not implemented yet (reported, ERROR_MOD_NOT_FOUND). */
static u64 w32_load_library(const char *name)
{
    const u32 handle = w32_module_by_name(name);
    if (handle) return handle;
    fprintf(stderr, "[WOW32] LoadLibrary(\"%s\"): 32-bit DLL images are not supported yet\n", name);
    g_last_error = 126;
    return 0;
}
static u64 w32_LoadLibraryA(const u32 *args) { return args[0] ? w32_load_library(W32_PTR(0)) : 0; }
static u64 w32_LoadLibraryExA(const u32 *args) { return args[0] ? w32_load_library(W32_PTR(0)) : 0; }
static u64 w32_LoadLibraryW(const u32 *args)
{
    if (!args[0]) return 0;
    char name[PATH_MAX];
    w32_narrow(W32_PTR(0), name, sizeof(name));
    return w32_load_library(name);
}
static u64 w32_LoadLibraryExW(const u32 *args) { return w32_LoadLibraryW(args); }
static u64 w32_FreeLibrary(const u32 *args) { (void)args; return 1; }

static u64 w32_GetModuleFileNameW(const u32 *args)
{
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
        mbi[0] = page; mbi[1] = page; mbi[2] = 0x40;
        mbi[3] = 0x1000; mbi[4] = 0x1000; mbi[5] = 0x40;     /* mapped: image or Beer's own, committed */
        mbi[6] = page >= g_w32_image && page < g_w32_image + 0x10000000u ? 0x1000000u : 0x20000u;
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
static u32 w32_res_name(u32 value, char *buffer, size_t size)
{
    if (value < 0x10000) return value;
    w32_narrow((const u16 *)(uintptr_t)value, buffer, size);
    return (u32)(uintptr_t)buffer;
}
static u64 w32_FindResourceExW(const u32 *args)
{
    char type[256], name[256];
    const u32 narrowed[4] = { args[0], w32_res_name(args[1], type, sizeof(type)), w32_res_name(args[2], name, sizeof(name)),
                              args[3] };
    return w32_FindResourceExA(narrowed);
}
static u64 w32_FindResourceW(const u32 *args)         /* (module, name, type) */
{
    char type[256], name[256];
    const u32 narrowed[4] = { args[0], w32_res_name(args[2], type, sizeof(type)), w32_res_name(args[1], name, sizeof(name)),
                              0 };
    return w32_FindResourceExA(narrowed);
}
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

#define W32_BRIDGES 64
static u32 g_w32_bridge_proc[W32_BRIDGES];

/* lParam structures of the messages Beer itself sends, copied into 32-bit memory for the call and back after it. */
static u32 w32_wndproc_call(u32 proc, u64 hwnd, u32 message, u64 wparam, u64 lparam)
{
    u32 lp32 = (u32)lparam;
    u8 *copy = NULL;
    const u8 *host = (const u8 *)(uintptr_t)lparam;
    if (lparam > 0xffffffffull) {
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

/* Results are sign-extended to 64 bits: LRESULT -1 (WM_CREATE failure) must stay -1. */
#define W32_BRIDGE(n) \
    static u64 __attribute__((ms_abi)) w32_bridge_##n(u64 hwnd, u32 message, u64 wparam, u64 lparam) \
    { return (u64)(s64)(s32)w32_wndproc_call(g_w32_bridge_proc[n], hwnd, message, wparam, lparam); }
#define W32_BRIDGE8(n) W32_BRIDGE(n##0) W32_BRIDGE(n##1) W32_BRIDGE(n##2) W32_BRIDGE(n##3) \
                       W32_BRIDGE(n##4) W32_BRIDGE(n##5) W32_BRIDGE(n##6) W32_BRIDGE(n##7)
W32_BRIDGE8(0) W32_BRIDGE8(1) W32_BRIDGE8(2) W32_BRIDGE8(3) W32_BRIDGE8(4) W32_BRIDGE8(5) W32_BRIDGE8(6) W32_BRIDGE8(7)
#define W32_BRIDGE_REF8(n) w32_bridge_##n##0, w32_bridge_##n##1, w32_bridge_##n##2, w32_bridge_##n##3, \
                           w32_bridge_##n##4, w32_bridge_##n##5, w32_bridge_##n##6, w32_bridge_##n##7
typedef u64 (__attribute__((ms_abi)) *W32Bridge)(u64, u32, u64, u64);
static const W32Bridge g_w32_bridges[W32_BRIDGES] = {
    W32_BRIDGE_REF8(0), W32_BRIDGE_REF8(1), W32_BRIDGE_REF8(2), W32_BRIDGE_REF8(3),
    W32_BRIDGE_REF8(4), W32_BRIDGE_REF8(5), W32_BRIDGE_REF8(6), W32_BRIDGE_REF8(7),
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

static u64 w32_CallWindowProcW(const u32 *args)
{
    const u32 proc = args[0];
    const u64 hwnd = w32_widen(args[1]), wparam = w32_widen(args[3]), lparam = w32_widen(args[4]);
    if (!proc) return (u32)impl_DefWindowProcW(hwnd, args[2], wparam, lparam);
    const W32Thunk *t = w32_thunk_at(proc);
    if (t && t->impl)
        return (u32)((u64 (__attribute__((ms_abi)) *)(u64, u32, u64, u64))(void *)t->impl)(hwnd, args[2], wparam, lparam);
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
