/* ---- WoW32: running 32-bit (PE32, i386) Windows programs ---------------------------------------------------------
 *
 * Included into loader.c (it calls the impl_* Windows implementations directly). The design follows Windows' own
 * WoW64: 32-bit code runs natively in x86 compatibility mode (CS 0x23) inside the 64-bit process, and every Windows
 * API call leaves it for 64-bit mode (CS 0x33), where a thunk reads the stdcall arguments from the 32-bit stack and
 * calls the 64-bit implementation.
 *
 * Address space: everything the 32-bit program can see lives below 4 GiB (images at their preferred bases, stacks,
 *   heaps, TEB/PEB, thunks). Beer's handles already fit in 32 bits.
 * Threads: each thread has a 64-bit TEB (GS base; Beer's own code keeps working) followed by its 32-bit TEB at
 *   TEB64+0x2000, as on Windows. 32-bit code reaches the TEB32 through FS, which holds a per-thread LDT selector
 *   (modify_ldt) whose base is the TEB32. The host's FS base (glibc TLS) is restored with WRFSBASE whenever control
 *   returns to 64-bit code, and FS is reloaded from the selector on the way back.
 * Per-thread switching state lives in the unused tail of the TEB64 (offsets W32_* below, past the 0x1838 bytes the
 *   Windows x64 TEB uses), reached GS-relative from the assembly entry points.
 *
 * Milestone 1 (this file): PE32 executables, relocations, imports bound to thunks, TEB32/PEB32, a 32-bit heap, a
 * first set of kernel32 functions, and fault reporting. Not yet: 32-bit DLL loading, SEH (FS:[0] frames),
 * callbacks into 32-bit code, threads. */

#include <asm/ldt.h>
#include <fnmatch.h>

#define W32_SLOT_HOST_RSP   0x1900   /* host stack pointer to resume on when 32-bit code calls an API */
#define W32_SLOT_HOST_FS    0x1908   /* glibc FS base of this thread */
#define W32_SLOT_FS_SEL     0x1910   /* this thread's 32-bit FS selector (TEB32) */
#define W32_SLOT_ESP        0x1918   /* guest ESP at the API call (return address on top) */
#define W32_SLOT_EBX        0x1920
#define W32_SLOT_ESI        0x1928
#define W32_SLOT_EDI        0x1930
#define W32_SLOT_EBP        0x1938
#define W32_SLOT_CLEANUP    0x1940   /* bytes of arguments the callee pops (stdcall) */
#define W32_SLOT_INDEX      0x1948   /* thunk index of the API being called */

#define W32_USER32_CS 0x23
#define W32_USER_DS   0x2b
#define W32_LIMIT     0x7fff0000ull  /* top of the 32-bit user address space (no /LARGEADDRESSAWARE yet) */

typedef struct {
    u16 magic; u8 major_linker, minor_linker;
    u32 code_size, init_size, uninit_size, entry_rva, code_base, data_base;
    u32 image_base, section_align, file_align;
    u16 os_major, os_minor, image_major, image_minor, subsys_major, subsys_minor;
    u32 win32_version, size_of_image, size_of_headers, checksum;
    u16 subsystem, dll_characteristics;
    u32 stack_reserve, stack_commit, heap_reserve, heap_commit, loader_flags, rva_count;
    DataDir dirs[16];
} W32OptHdr;

typedef struct {
    u32 sig;
    struct { u16 machine, nsections; u32 timestamp, symtab, nsymbols; u16 opthdr_sz, characteristics; } file;
    W32OptHdr opt;
} W32NtHdrs;

typedef u64 (*W32Fn)(const u32 *args);   /* returns EDX:EAX */
typedef struct { const char *name; W32Fn fn; u32 arg_bytes; } W32Api;

typedef struct {
    const char *dll, *name;
    W32Fn fn;
    ImplFn impl;         /* generic forwarding: the 64-bit implementation called with the sign-extended arguments */
    u32 arg_bytes;       /* popped by the callee (stdcall); 0 for cdecl */
} W32Thunk;

static W32Thunk *g_w32_thunks;
static u32 g_w32_thunk_count, g_w32_thunk_capacity;
static u8 *g_w32_thunk_code;            /* 32-bit entry stubs, 12 bytes each, below 4 GiB */
static u8 *g_w32_lowstub;               /* 64-bit code below 4 GiB the 32-bit stubs far-jump to */
static u32 g_w32_return_stub;           /* 32-bit address: returning here ends a host->guest call */
static u32 g_w32_image;                 /* the executable's base */
static u32 g_w32_peb;
static uintptr_t g_w32_next = 0x10000000;   /* bump pointer for anonymous low allocations */
static pthread_mutex_t g_w32_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_w32_next_ldt = 16;

extern char beer_w32_api_entry[], beer_w32_return_entry[];
u64 beer_w32_call32(u32 entry, u32 esp);   /* runs 32-bit code at `entry` on stack `esp` until it returns to the
                                             * return stub; result EDX:EAX */

/* ---- low memory ---- */

/* Maps `size` bytes below 4 GiB at `want` (0 = anywhere). */
static void *w32_map(uintptr_t want, size_t size, int prot)
{
    size = (size + 0xffff) & ~(size_t)0xffff;
    if (want) {
        void *p = mmap((void *)want, size, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p != MAP_FAILED) return p;
    }
    pthread_mutex_lock(&g_w32_lock);
    for (uintptr_t at = g_w32_next; at + size <= W32_LIMIT; at += 0x10000) {
        void *p = mmap((void *)at, size, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p != MAP_FAILED) {
            g_w32_next = at + size;
            pthread_mutex_unlock(&g_w32_lock);
            return p;
        }
    }
    pthread_mutex_unlock(&g_w32_lock);
    return NULL;
}

/* ---- 32-bit heap ----
 * Size classes 16..32768 bytes (powers of two) with free lists; larger blocks get their own mapping. A 16-byte
 * header before each block records its class (or the mapping size), so HeapSize/HeapFree/HeapReAlloc work. */
#define W32_HEAP_CLASSES 12
typedef struct W32Free { u32 next; } W32Free;
static u32 g_w32_free[W32_HEAP_CLASSES];
static u8 *g_w32_arena, *g_w32_arena_end;
static pthread_mutex_t g_w32_heap_lock = PTHREAD_MUTEX_INITIALIZER;

static void *w32_heap_alloc(size_t size, int zero)
{
    if (size > 0x7fff0000u) return NULL;
    int cls = 0;
    while (cls < W32_HEAP_CLASSES && (16u << cls) < size) ++cls;
    u8 *block;
    if (cls == W32_HEAP_CLASSES) {
        const size_t total = (size + 16 + 0xfff) & ~(size_t)0xfff;
        u8 *p = w32_map(0, total, PROT_READ | PROT_WRITE);
        if (!p) return NULL;
        ((u32 *)p)[0] = 0xffffffffu;                 /* large */
        ((u32 *)p)[1] = (u32)total;
        ((u32 *)p)[2] = (u32)size;
        return p + 16;                               /* fresh mapping is zeroed */
    }
    pthread_mutex_lock(&g_w32_heap_lock);
    if (g_w32_free[cls]) {
        block = (u8 *)(uintptr_t)g_w32_free[cls];
        g_w32_free[cls] = ((W32Free *)block)->next;
    } else {
        const size_t need = (16u << cls) + 16;
        if (!g_w32_arena || g_w32_arena + need > g_w32_arena_end) {
            const size_t chunk = 16u << 20;
            g_w32_arena = w32_map(0, chunk, PROT_READ | PROT_WRITE);
            if (!g_w32_arena) { pthread_mutex_unlock(&g_w32_heap_lock); return NULL; }
            g_w32_arena_end = g_w32_arena + chunk;
        }
        u8 *p = g_w32_arena;
        g_w32_arena += need;
        ((u32 *)p)[0] = (u32)cls;
        block = p + 16;
    }
    pthread_mutex_unlock(&g_w32_heap_lock);
    ((u32 *)(block - 16))[2] = (u32)size;
    if (zero) memset(block, 0, size);
    return block;
}

static size_t w32_heap_size(const void *block)
{
    return block ? ((const u32 *)block - 4)[2] : 0;
}

static void w32_heap_free(void *block)
{
    if (!block) return;
    u32 *header = (u32 *)block - 4;
    if (header[0] == 0xffffffffu) { munmap(header, header[1]); return; }
    const u32 cls = header[0];
    if (cls >= W32_HEAP_CLASSES) return;            /* not ours */
    pthread_mutex_lock(&g_w32_heap_lock);
    ((W32Free *)block)->next = g_w32_free[cls];
    g_w32_free[cls] = (u32)(uintptr_t)block;
    pthread_mutex_unlock(&g_w32_heap_lock);
}

static void *w32_heap_realloc(void *block, size_t size, int zero)
{
    if (!block) return w32_heap_alloc(size, zero);
    const size_t old = w32_heap_size(block);
    u32 *header = (u32 *)block - 4;
    if (header[0] < W32_HEAP_CLASSES && size <= (16u << header[0])) {   /* fits in place */
        if (zero && size > old) memset((u8 *)block + old, 0, size - old);
        header[2] = (u32)size;
        return block;
    }
    void *fresh = w32_heap_alloc(size, zero);
    if (!fresh) return NULL;
    memcpy(fresh, block, old < size ? old : size);
    w32_heap_free(block);
    return fresh;
}

/* Copies a host string into the 32-bit heap (strings the program keeps, e.g. GetCommandLineA). */
static u32 w32_strdup(const char *text)
{
    const size_t n = strlen(text) + 1;
    char *copy = w32_heap_alloc(n, 0);
    if (!copy) return 0;
    memcpy(copy, text, n);
    return (u32)(uintptr_t)copy;
}
static u32 w32_wcsdup(const u16 *text)
{
    size_t n = 0;
    while (text[n]) ++n;
    u16 *copy = w32_heap_alloc((n + 1) * 2, 0);
    if (!copy) return 0;
    memcpy(copy, text, (n + 1) * 2);
    return (u32)(uintptr_t)copy;
}

/* ---- threads ---- */

typedef struct {
    u32 ExceptionList, StackBase, StackLimit, SubSystemTib, FiberData, ArbitraryUserPointer, Self;
    u32 EnvironmentPointer, UniqueProcess, UniqueThread, ActiveRpcHandle, ThreadLocalStoragePointer, Peb;
    u32 LastErrorValue;                               /* +0x34 */
} W32Teb;

/* Sets up the calling host thread for 32-bit code: a TEB64 (GS) and TEB32 (FS selector) below 4 GiB, and a guest
 * stack of `stack_size` bytes. Returns the initial guest ESP. */
static u32 w32_thread_init(size_t stack_size)
{
    u8 *tebs = w32_map(0, 0x3000, PROT_READ | PROT_WRITE);
    if (!tebs) die("WoW32: no low memory for the TEBs");
    WinTEB *teb64 = (WinTEB *)tebs;
    W32Teb *teb32 = (W32Teb *)(tebs + 0x2000);
    u8 *stack = w32_map(0, stack_size, PROT_READ | PROT_WRITE);
    if (!stack) die("WoW32: no low memory for the thread stack");

    teb64->Self = (u64)(uintptr_t)teb64;
    teb64->ProcEnvBlk = (u64)(uintptr_t)g_process_peb;
    teb64->UniqueProcess = (u64)(u32)getpid();
    teb64->UniqueThread = (u64)(u32)syscall(SYS_gettid);
    teb64->StackBase = (u64)(uintptr_t)(stack + stack_size);
    teb64->StackLimit = (u64)(uintptr_t)stack;
    teb64->CurrentLocale = 0x409;
    teb64->TlsLinks[0] = teb64->TlsLinks[1] = (u64)(uintptr_t)teb64->TlsLinks;
    g_cached_windows_thread_id = teb64->UniqueThread;

    teb32->ExceptionList = 0xffffffffu;               /* end of the SEH chain */
    teb32->StackBase = (u32)(uintptr_t)(stack + stack_size);
    teb32->StackLimit = (u32)(uintptr_t)stack;
    teb32->Self = (u32)(uintptr_t)teb32;
    teb32->UniqueProcess = (u32)teb64->UniqueProcess;
    teb32->UniqueThread = (u32)teb64->UniqueThread;
    teb32->Peb = g_w32_peb;

    pthread_mutex_lock(&g_w32_lock);
    const int entry = g_w32_next_ldt++;
    pthread_mutex_unlock(&g_w32_lock);
    struct user_desc desc = { .entry_number = (unsigned)entry, .base_addr = (u32)(uintptr_t)teb32, .limit = 0xfff,
                              .seg_32bit = 1, .useable = 1 };
    if (syscall(SYS_modify_ldt, 0x11, &desc, sizeof(desc)) != 0)
        die("WoW32: modify_ldt: %s", strerror(errno));
    *(u64 *)(tebs + W32_SLOT_FS_SEL) = (u64)((entry << 3) | 4 | 3);
    u64 host_fs;
    __asm__ volatile("rdfsbase %0" : "=r"(host_fs));
    *(u64 *)(tebs + W32_SLOT_HOST_FS) = host_fs;
    if (syscall(SYS_arch_prctl, ARCH_SET_GS, (u64)(uintptr_t)teb64) != 0)
        die("WoW32: ARCH_SET_GS: %s", strerror(errno));
    return (u32)(uintptr_t)(stack + stack_size - 16);
}

static W32Teb *w32_teb(void)
{
    u64 teb64;
    __asm__ volatile("mov %%gs:0x30, %0" : "=r"(teb64));
    return (W32Teb *)(uintptr_t)(teb64 + 0x2000);
}

/* ---- the 32 <-> 64-bit transitions ----
 * beer_w32_api_entry: 64-bit code reached by far jump from a thunk (EAX = thunk index, ESP = guest stack with the
 *   return address on top). Saves the guest's callee-saved registers, switches to the host stack and FS base, calls
 *   w32_dispatch, then pops the return address and the stdcall arguments, reloads FS and returns to 32-bit mode.
 * beer_w32_call32(entry, esp): enters 32-bit code from the host; it returns when the 32-bit code returns to the
 *   return stub (which far-jumps to beer_w32_return_entry). Nesting works: the previous host RSP is kept on the
 *   host stack. */
u64 w32_dispatch(u32 index);
__asm__(
    ".text\n"
    ".global beer_w32_api_entry\n"
    "beer_w32_api_entry:\n"
    "    mov %esp, %esp\n"                       /* upper halves are undefined after the mode switch */
    "    mov %eax, %eax\n"
    "    mov %rsp, %gs:0x1918\n"
    "    mov %rbx, %gs:0x1920\n"
    "    mov %rsi, %gs:0x1928\n"
    "    mov %rdi, %gs:0x1930\n"
    "    mov %rbp, %gs:0x1938\n"
    "    mov %gs:0x1900, %rsp\n"
    "    mov %gs:0x1908, %rcx\n"
    "    wrfsbase %rcx\n"
    "    mov %eax, %edi\n"
    "    call w32_dispatch\n"
    "    mov %rax, %rdx\n"
    "    shr $32, %rdx\n"
    "    mov %gs:0x1918, %rcx\n"                 /* guest ESP: return address on top */
    "    mov %ecx, %ecx\n"
    "    mov (%rcx), %r8d\n"
    "    add $4, %ecx\n"
    "    add %gs:0x1940, %ecx\n"                 /* stdcall: the callee pops its arguments */
    "    mov %gs:0x1910, %r9\n"
    "    mov %r9w, %fs\n"                        /* FS base = TEB32 again */
    "    mov $0x2b, %r9d\n"
    "    mov %r9w, %ds\n"
    "    mov %r9w, %es\n"
    "    mov %gs:0x1920, %rbx\n"
    "    mov %gs:0x1928, %rsi\n"
    "    mov %gs:0x1930, %rdi\n"
    "    mov %gs:0x1938, %rbp\n"
    "    push $0x2b\n"
    "    push %rcx\n"
    "    pushfq\n"
    "    push $0x23\n"
    "    push %r8\n"
    "    iretq\n"

    ".global beer_w32_call32\n"
    "beer_w32_call32:\n"                         /* edi = entry, esi = guest esp */
    "    push %rbx\n push %rbp\n push %r12\n push %r13\n push %r14\n push %r15\n"
    "    push %gs:0x1900\n"                      /* nesting: the outer host resume point; RSP is now 16-aligned */
    "    mov %rsp, %gs:0x1900\n"
    "    mov %gs:0x1910, %r9\n"
    "    mov %r9w, %fs\n"
    "    mov $0x2b, %r9d\n"
    "    mov %r9w, %ds\n"
    "    mov %r9w, %es\n"
    "    mov %esi, %ecx\n"
    "    mov %edi, %r8d\n"
    "    push $0x2b\n"
    "    push %rcx\n"
    "    pushfq\n"
    "    push $0x23\n"
    "    push %r8\n"
    "    iretq\n"

    ".global beer_w32_return_entry\n"
    "beer_w32_return_entry:\n"                   /* reached from the 32-bit return stub; EDX:EAX = result */
    "    mov %eax, %eax\n"
    "    shl $32, %rdx\n"
    "    or %rdx, %rax\n"
    "    mov %gs:0x1900, %rsp\n"
    "    mov %gs:0x1908, %rcx\n"
    "    wrfsbase %rcx\n"
    "    pop %gs:0x1900\n"
    "    pop %r15\n pop %r14\n pop %r13\n pop %r12\n pop %rbp\n pop %rbx\n"
    "    ret\n");

/* Calls a 32-bit stdcall/cdecl function from host code (entry points, TLS callbacks, later callbacks). */
static u32 w32_call(u32 fn, u32 argc, const u32 *argv)
{
    u64 teb64;
    __asm__ volatile("mov %%gs:0x30, %0" : "=r"(teb64));
    u32 esp = *(u32 *)(uintptr_t)(teb64 + W32_SLOT_ESP);   /* below the frame of an API call in progress */
    if (!esp) esp = (u32)w32_teb()->StackBase - 16;
    esp = (esp - 64) & ~15u;                               /* leave the caller's frame alone */
    u32 *sp = (u32 *)(uintptr_t)esp;
    for (u32 i = argc; i-- > 0;) *--sp = argv[i];
    *--sp = g_w32_return_stub;
    /* A callback made during an API call: its own API calls reuse the switching slots, so keep the outer call's. */
    u64 saved[9];
    memcpy(saved, (void *)(uintptr_t)(teb64 + W32_SLOT_ESP), sizeof(saved));
    const u32 result = (u32)beer_w32_call32(fn, (u32)(uintptr_t)sp);
    memcpy((void *)(uintptr_t)(teb64 + W32_SLOT_ESP), saved, sizeof(saved));
    return result;
}

/* ---- thunks ---- */

static u64 w32_unimplemented(const u32 *args)
{
    (void)args;
    return 0;
}

/* Calls a 64-bit ms_abi implementation with up to 12 stdcall arguments. Each 32-bit argument is sign-extended: guest
 * addresses stay below 2 GiB, so pointers are unchanged, while handles such as -1 and HKEY_LOCAL_MACHINE keep the
 * 64-bit value the implementation compares against. The result is truncated to EAX. */
typedef u64 (__attribute__((ms_abi)) *W32Impl12)(u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64);
/* A 32-bit argument as the 64-bit implementation expects it. Small negative values (pseudo handles such as
 * GetCurrentProcess() = -1, INVALID_HANDLE_VALUE, STD_*_HANDLE, negative coordinates) and the predefined registry
 * keys (HKEY_* = 0x80000000..) are sign-extended, as WoW64 does; everything else is zero-extended, so pointers (all
 * below 2 GiB) and Beer's handles with the top bit set (windows 0xD000xxxx, DCs 0xDC00xxxx) keep their value. */
static inline u64 w32_widen(u32 v)
{
    if (v >= 0xffff0000u || (v >= 0x80000000u && v <= 0x800000ffu)) return (u64)(s64)(s32)v;
    return v;
}
static u64 w32_call_impl(ImplFn fn, const u32 *args, u32 argc)
{
    u64 a[12] = {0};
    for (u32 i = 0; i < argc && i < 12; ++i) a[i] = w32_widen(args[i]);
    return (u32)((W32Impl12)(void *)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11]);
}

u64 w32_dispatch(u32 index)
{
    u64 teb64;
    __asm__ volatile("mov %%gs:0x30, %0" : "=r"(teb64));
    if (index >= g_w32_thunk_count) {
        const u32 bad_esp = (u32)*(u64 *)(uintptr_t)(teb64 + W32_SLOT_ESP);
        fprintf(stderr, "[WOW32] bad thunk index %u (thunks %u) guest esp=0x%x return=0x%x\n", index,
                g_w32_thunk_count, bad_esp, *(u32 *)(uintptr_t)bad_esp);
        _exit(3);
    }
    const W32Thunk *t = &g_w32_thunks[index];
    const u32 esp = (u32)*(u64 *)(uintptr_t)(teb64 + W32_SLOT_ESP);
    *(u64 *)(uintptr_t)(teb64 + W32_SLOT_CLEANUP) = t->arg_bytes;
    *(u64 *)(uintptr_t)(teb64 + W32_SLOT_INDEX) = index;
    if (t->fn == w32_unimplemented && !t->impl) {
        static _Atomic(u32) reported[4096];
        if (index >= 4096 || !atomic_exchange(&reported[index], 1))
            fprintf(stderr, "[WOW32] called unimplemented %s!%s (returns 0)\n", t->dll, t->name);
    }
    const u32 *args = (const u32 *)(uintptr_t)(esp + 4);
    static int trace = -1;
    if (trace < 0) trace = getenv("BEER_W32_TRACE") != NULL;
    if (trace) {
        fprintf(stderr, "[W32T] %s!%s(0x%x,0x%x,0x%x,0x%x)", t->dll, t->name, args[0], args[1], args[2], args[3]);
        unsigned char page_probe;
        if (args[0] > 0x10000 && args[0] < 0x7f000000 && t->arg_bytes &&
            mincore((void *)(uintptr_t)(args[0] & ~0xfffu), 1, &page_probe) == 0) {
            const char *s = (const char *)(uintptr_t)args[0];
            if (s[0] >= 32 && s[0] < 127 && s[1] >= 32 && s[1] < 127) fprintf(stderr, " \"%.80s\"", s);
        }
        fputc('\n', stderr);
    }
    const u64 result = t->impl ? w32_call_impl(t->impl, args, t->arg_bytes / 4) : t->fn(args);
    w32_teb()->LastErrorValue = g_last_error;
    return result;
}

static const W32Api *w32_find_api(const char *name);
static u32 w32_data_import(const char *name);   /* address of an exported variable (msvcrt's _iob), 0 if not one */

/* Returns the 32-bit address of a thunk for dll!name, creating it on first use. */
static u32 w32_thunk_for(const char *dll, const char *name)
{
    const u32 data = w32_data_import(name);
    if (data) return data;
    for (u32 i = 0; i < g_w32_thunk_count; ++i)
        if (!strcasecmp(g_w32_thunks[i].name, name) && !strcasecmp(g_w32_thunks[i].dll, dll))
            return (u32)(uintptr_t)(g_w32_thunk_code + 12 * i);
    if (g_w32_thunk_count == g_w32_thunk_capacity) {
        fprintf(stderr, "[WOW32] thunk table full\n");
        return 0;
    }
    const W32Api *api = w32_find_api(name);
    W32Thunk *t = &g_w32_thunks[g_w32_thunk_count];
    t->dll = strdup(dll);
    t->name = strdup(name);
    t->fn = api && api->fn ? api->fn : w32_unimplemented;
    t->impl = api && !api->fn ? find_impl(api->name) : NULL;
    t->arg_bytes = api ? api->arg_bytes : 0;
    if (!api || (!api->fn && !t->impl)) fprintf(stderr, "[WOW32] import %s!%s is not implemented\n", dll, name);
    u8 *code = g_w32_thunk_code + 12 * g_w32_thunk_count;
    code[0] = 0xb8;                                        /* mov eax, index */
    const u32 index = g_w32_thunk_count;
    memcpy(code + 1, &index, 4);
    code[5] = 0xea;                                        /* ljmp 0x33:lowstub */
    const u32 target = (u32)(uintptr_t)g_w32_lowstub;
    memcpy(code + 6, &target, 4);
    code[10] = 0x33; code[11] = 0x00;
    return (u32)(uintptr_t)(g_w32_thunk_code + 12 * g_w32_thunk_count++);
}

static void w32_thunks_init(void)
{
    g_w32_thunk_capacity = 8192;
    g_w32_thunks = calloc(g_w32_thunk_capacity, sizeof(*g_w32_thunks));
    u8 *code = w32_map(0x7f000000, 0x30000, PROT_READ | PROT_WRITE | PROT_EXEC);
    if (!g_w32_thunks || !code) die("WoW32: cannot allocate thunks");
    g_w32_thunk_code = code;
    g_w32_lowstub = code + 0x20000;
    /* 64-bit: movabs r11, beer_w32_api_entry; jmp r11 (EAX holds the thunk index and must survive) */
    u8 *p = g_w32_lowstub;
    p[0] = 0x49; p[1] = 0xbb;
    const u64 api_entry = (u64)(uintptr_t)beer_w32_api_entry;
    memcpy(p + 2, &api_entry, 8);
    p[10] = 0x41; p[11] = 0xff; p[12] = 0xe3;
    /* 64-bit: movabs rcx, beer_w32_return_entry; jmp rcx (EAX/EDX hold the result) */
    u8 *r64 = g_w32_lowstub + 0x40;
    r64[0] = 0x48; r64[1] = 0xb9;
    const u64 ret_entry = (u64)(uintptr_t)beer_w32_return_entry;
    memcpy(r64 + 2, &ret_entry, 8);
    r64[10] = 0xff; r64[11] = 0xe1;
    /* 32-bit return stub: ljmp 0x33:r64 */
    u8 *r32 = g_w32_lowstub + 0x80;
    r32[0] = 0xea;
    const u32 r64_address = (u32)(uintptr_t)r64;
    memcpy(r32 + 1, &r64_address, 4);
    r32[5] = 0x33; r32[6] = 0x00;
    g_w32_return_stub = (u32)(uintptr_t)r32;
}

/* ---- API implementations (32-bit argument layout) ----
 * Each reads its stdcall arguments from the guest stack; pointers zero-extend, handles fit in 32 bits. */

#define W32_ARG(i) ((u64)args[i])
#define W32_PTR(i) ((void *)(uintptr_t)args[i])

static u64 w32_GetStdHandle(const u32 *args) { return (u32)impl_GetStdHandle(args[0]); }
static u64 w32_WriteFile(const u32 *args)
{
    return impl_WriteFile(W32_ARG(0), W32_PTR(1), args[2], W32_PTR(3), W32_ARG(4));
}
static u64 w32_ReadFile(const u32 *args)
{
    return impl_ReadFile(W32_ARG(0), W32_PTR(1), args[2], W32_PTR(3), W32_PTR(4));
}
static u64 w32_ExitProcess(const u32 *args) { impl_ExitProcess(args[0]); return 0; }
static u64 w32_TerminateProcess(const u32 *args) { return impl_TerminateProcess(W32_ARG(0), args[1]); }
static u64 w32_GetLastError(const u32 *args) { (void)args; return g_last_error; }
static u64 w32_SetLastError(const u32 *args) { g_last_error = args[0]; return 0; }
static u64 w32_CloseHandle(const u32 *args) { return impl_CloseHandle(W32_ARG(0)); }
static u64 w32_Sleep(const u32 *args) { impl_Sleep(args[0]); return 0; }
static u64 w32_GetCurrentProcessId(const u32 *args) { (void)args; return (u32)impl_GetCurrentProcessId(); }
static u64 w32_GetCurrentThreadId(const u32 *args) { (void)args; return (u32)impl_GetCurrentThreadId(); }
static u64 w32_GetCurrentProcess(const u32 *args) { (void)args; return 0xffffffffu; }
static u64 w32_IsDebuggerPresent(const u32 *args) { (void)args; return 0; }
static u64 w32_IsProcessorFeaturePresent(const u32 *args) { return impl_IsProcessorFeaturePresent(args[0]); }
static u64 w32_QueryPerformanceCounter(const u32 *args) { return impl_QueryPerformanceCounter(W32_PTR(0)); }
static u64 w32_QueryPerformanceFrequency(const u32 *args) { return impl_QueryPerformanceFrequency(W32_PTR(0)); }
static u64 w32_GetSystemTimeAsFileTime(const u32 *args) { impl_GetSystemTimeAsFileTime(W32_PTR(0)); return 0; }
static u64 w32_GetTickCount(const u32 *args) { (void)args; return (u32)impl_GetTickCount(); }
static u64 w32_GetProcessHeap(const u32 *args) { (void)args; return 0x00030000u; }   /* the one 32-bit heap */
static u64 w32_HeapAlloc(const u32 *args)
{
    void *p = w32_heap_alloc(args[2] ? args[2] : 1, (args[1] & 0x8) != 0);
    if (!p) g_last_error = 8;
    return (u32)(uintptr_t)p;
}
static u64 w32_HeapFree(const u32 *args) { w32_heap_free(W32_PTR(2)); return 1; }
static u64 w32_HeapReAlloc(const u32 *args)
{
    void *p = w32_heap_realloc(W32_PTR(2), args[3] ? args[3] : 1, (args[1] & 0x8) != 0);
    if (!p) g_last_error = 8;
    return (u32)(uintptr_t)p;
}
static u64 w32_HeapSize(const u32 *args) { return args[2] ? (u32)w32_heap_size(W32_PTR(2)) : 0xffffffffu; }
static u64 w32_MessageBoxA(const u32 *args)
{
    return beer_MessageBoxA(W32_ARG(0), W32_PTR(1), W32_PTR(2), args[3]);
}
static u64 w32_GetCommandLineA(const u32 *args)
{
    (void)args;
    static u32 cached;
    if (!cached) cached = w32_strdup((const char *)(uintptr_t)impl_GetCommandLineA());
    return cached;
}
static u64 w32_GetCommandLineW(const u32 *args)
{
    (void)args;
    static u32 cached;
    if (!cached) cached = w32_wcsdup((const u16 *)(uintptr_t)impl_GetCommandLineW());
    return cached;
}

/* ---- kernel32: process, module and resource APIs ---- */

#define W32_SEXT(i) w32_widen(args[i])

static u64 w32_forward(const char *name, const u32 *args, u32 argc)
{
    const ImplFn impl = find_impl(name);
    if (!impl) { g_last_error = 120; return 0; }            /* ERROR_CALL_NOT_IMPLEMENTED */
    return w32_call_impl(impl, args, argc);
}

/* STARTUPINFOA: 68 bytes in a 32-bit process. */
static u64 w32_GetStartupInfoA(const u32 *args)
{
    u8 *si = W32_PTR(0);
    if (!si) return 0;
    memset(si, 0, 68);
    *(u32 *)(si + 0) = 68;
    *(u16 *)(si + 48) = 1;                                  /* wShowWindow = SW_NORMAL */
    *(u32 *)(si + 56) = (u32)impl_GetStdHandle((u32)-10);
    *(u32 *)(si + 60) = (u32)impl_GetStdHandle((u32)-11);
    *(u32 *)(si + 64) = (u32)impl_GetStdHandle((u32)-12);
    return 0;
}

/* CreateProcessA: the STARTUPINFOA and PROCESS_INFORMATION layouts differ between 32 and 64 bits. */
static u64 w32_CreateProcessA(const u32 *args)
{
    const u8 *si32 = W32_PTR(8);
    u8 si64[104];
    memset(si64, 0, sizeof(si64));
    *(u32 *)si64 = 104;
    if (si32) {
        memcpy(si64 + 32, si32 + 16, 32);                   /* dwX .. dwFlags */
        memcpy(si64 + 64, si32 + 48, 4);                    /* wShowWindow, cbReserved2 */
        for (int i = 0; i < 3; ++i) *(u64 *)(si64 + 80 + 8 * i) = (u64)(s64)*(const s32 *)(si32 + 56 + 4 * i);
    }
    u8 pi64[24];
    memset(pi64, 0, sizeof(pi64));
    const u64 ok = beer_CreateProcessA(W32_PTR(0), W32_PTR(1), W32_PTR(2), W32_PTR(3), args[4], args[5], W32_PTR(6),
                                       W32_PTR(7), si32 ? si64 : NULL, pi64);
    u32 *pi32 = W32_PTR(9);
    if (pi32) {
        pi32[0] = (u32)*(u64 *)pi64;
        pi32[1] = (u32)*(u64 *)(pi64 + 8);
        pi32[2] = *(u32 *)(pi64 + 16);
        pi32[3] = *(u32 *)(pi64 + 20);
    }
    return (u32)ok;
}

/* FormatMessageA with Beer's message text; FORMAT_MESSAGE_ALLOCATE_BUFFER allocates from the 32-bit heap. */
static u64 w32_FormatMessageA(const u32 *args)
{
    char text[64];
    snprintf(text, sizeof(text), "error %u", args[2]);
    const size_t length = strlen(text);
    if (args[0] & 0x100u) {                                 /* FORMAT_MESSAGE_ALLOCATE_BUFFER */
        char *block = w32_heap_alloc(length + 1, 0);
        if (!block) { g_last_error = 8; return 0; }
        memcpy(block, text, length + 1);
        if (args[4]) *(u32 *)W32_PTR(4) = (u32)(uintptr_t)block;
        return length;
    }
    if (!args[4] || !args[5]) { g_last_error = 122; return 0; }   /* ERROR_INSUFFICIENT_BUFFER */
    snprintf(W32_PTR(4), args[5], "%s", text);
    return strlen(W32_PTR(4));
}

static u64 w32_LocalFree(const u32 *args) { w32_heap_free(W32_PTR(0)); return 0; }

/* System modules are opaque handles here: the 64-bit module table builds trampolines for the 64-bit image, which a
 * 32-bit process does not have. GetProcAddress on them resolves only what this layer provides. */
static u32 w32_module_by_name(const char *path);
static u64 w32_GetModuleHandleA(const u32 *args)
{
    if (!args[0]) return g_w32_image;
    const u32 handle = w32_module_by_name(W32_PTR(0));
    if (!handle) g_last_error = 126;                        /* ERROR_MOD_NOT_FOUND */
    return handle;
}

/* A GetProcAddress result is a thunk, so only APIs this layer provides can be returned. */
static u64 w32_GetProcAddress(const u32 *args)
{
    const char *name = W32_PTR(1);
    if (args[1] < 0x10000 || !w32_find_api(name)) { g_last_error = 127; return 0; }   /* ERROR_PROC_NOT_FOUND */
    return w32_thunk_for("kernel32.dll", name);
}

static u32 g_w32_unhandled_filter;
static u64 w32_SetUnhandledExceptionFilter(const u32 *args)
{
    const u32 previous = g_w32_unhandled_filter;
    g_w32_unhandled_filter = args[0];
    return previous;
}

static u64 w32_IsWow64Process(const u32 *args)
{
    if (args[1]) *(u32 *)W32_PTR(1) = 1;                    /* a 32-bit process on a 64-bit system */
    return 1;
}

/* Resources of the 32-bit image. HRSRC is the IMAGE_RESOURCE_DATA_ENTRY, HGLOBAL the data itself. */
#define W32_RES_NOT_FOUND 0xffffffffu
static u32 w32_res_child(const u8 *root, u32 dir_offset, u32 key, int language_level)
{
    const u8 *dir = root + dir_offset;
    const u32 named = *(const u16 *)(dir + 12), ids = *(const u16 *)(dir + 14);
    const u32 *entries = (const u32 *)(dir + 16);
    if (language_level) {
        u32 first = W32_RES_NOT_FOUND, neutral = W32_RES_NOT_FOUND;
        for (u32 i = named; i < named + ids; ++i) {
            if (first == W32_RES_NOT_FOUND) first = entries[2 * i + 1];
            if (entries[2 * i] == key) return entries[2 * i + 1];
            if (entries[2 * i] == 0) neutral = entries[2 * i + 1];
        }
        return neutral != W32_RES_NOT_FOUND ? neutral : first;
    }
    if (key >= 0x10000 && ((const char *)(uintptr_t)key)[0] != '#') {
        const char *wanted = (const char *)(uintptr_t)key;
        for (u32 i = 0; i < named; ++i) {
            const u16 *name = (const u16 *)(root + (entries[2 * i] & 0x7fffffffu));
            const u16 length = name[0];
            u16 k = 0;
            while (k < length && wanted[k] && toupper((unsigned char)wanted[k]) == toupper(name[1 + k])) ++k;
            if (k == length && !wanted[k]) return entries[2 * i + 1];
        }
        return W32_RES_NOT_FOUND;
    }
    const u32 id = key >= 0x10000 ? (u32)atoi((const char *)(uintptr_t)key + 1) : key;
    for (u32 i = named; i < named + ids; ++i)
        if (entries[2 * i] == id) return entries[2 * i + 1];
    return W32_RES_NOT_FOUND;
}

static u64 w32_FindResourceExA(const u32 *args)
{
    const u8 *image = (const u8 *)(uintptr_t)(args[0] ? args[0] : g_w32_image);
    const W32NtHdrs *nt = (const W32NtHdrs *)(image + ((const DosHdr *)image)->lfanew);
    if (!nt->opt.dirs[2].rva || !nt->opt.dirs[2].size) { g_last_error = 1812; return 0; }   /* ERROR_RESOURCE_DATA_NOT_FOUND */
    const u8 *root = image + nt->opt.dirs[2].rva;
    const u32 keys[2] = { args[1], args[2] };
    u32 offset = 0;
    for (int level = 0; level < 2; ++level) {
        offset = w32_res_child(root, offset, keys[level], 0);
        if (offset == W32_RES_NOT_FOUND || !(offset & 0x80000000u)) { g_last_error = 1814; return 0; }
        offset &= 0x7fffffffu;
    }
    offset = w32_res_child(root, offset, args[3], 1);
    if (offset == W32_RES_NOT_FOUND || (offset & 0x80000000u)) { g_last_error = 1814; return 0; }   /* ERROR_RESOURCE_NAME_NOT_FOUND */
    return (u32)(uintptr_t)(root + offset);
}
static u64 w32_LoadResource(const u32 *args)
{
    if (!args[1]) { g_last_error = 87; return 0; }
    const u8 *image = (const u8 *)(uintptr_t)(args[0] ? args[0] : g_w32_image);
    return (u32)(uintptr_t)(image + ((const u32 *)W32_PTR(1))[0]);
}
static u64 w32_LockResource(const u32 *args) { return args[0]; }
static u64 w32_SizeofResource(const u32 *args)
{
    if (!args[1]) { g_last_error = 87; return 0; }
    return ((const u32 *)W32_PTR(1))[1];
}

/* ---- advapi32 ---- */

static u64 w32_RegOpenKeyExA(const u32 *args)
{
    u64 key = 0;
    const u64 status = impl_RegOpenKeyExA(W32_SEXT(0), W32_PTR(1), args[2], args[3], &key);
    if (args[4]) *(u32 *)W32_PTR(4) = (u32)key;
    return (u32)status;
}

/* ---- user32 ---- */

/* MSG is 28 bytes in a 32-bit process, 48 bytes in a 64-bit one. */
static void w32_msg_to_native(const u32 *msg32, WIN_MSG *msg)
{
    memset(msg, 0, sizeof(*msg));
    msg->hwnd = w32_widen(msg32[0]);
    msg->message = msg32[1];
    msg->wParam = w32_widen(msg32[2]);
    msg->lParam = w32_widen(msg32[3]);
    msg->time = msg32[4];
    msg->pt.x = (s32)msg32[5];
    msg->pt.y = (s32)msg32[6];
}
static u64 w32_GetMessageA(const u32 *args)
{
    WIN_MSG msg;
    memset(&msg, 0, sizeof(msg));
    const u64 result = impl_GetMessageW(&msg, W32_SEXT(1), args[2], args[3]);
    u32 *out = W32_PTR(0);
    if (out && (s32)result != -1) {
        out[0] = (u32)msg.hwnd; out[1] = msg.message; out[2] = (u32)msg.wParam; out[3] = (u32)msg.lParam;
        out[4] = msg.time; out[5] = (u32)msg.pt.x; out[6] = (u32)msg.pt.y;
    }
    return (u32)result;
}
static u64 w32_DispatchMessageA(const u32 *args)
{
    WIN_MSG msg;
    w32_msg_to_native(W32_PTR(0), &msg);
    return (u32)impl_DispatchMessageW(&msg);
}
static u64 w32_TranslateMessage(const u32 *args)
{
    WIN_MSG msg;
    w32_msg_to_native(W32_PTR(0), &msg);
    return (u32)impl_TranslateMessage(&msg);
}
/* A 32-bit timer callback cannot be invoked by the 64-bit timer machinery; WM_TIMER messages still work. */
static u64 w32_SetTimer(const u32 *args)
{
    if (args[3]) { g_last_error = 87; return 0; }
    return w32_forward("SetTimer", args, 4);
}
/* Bitmaps in the 32-bit image's resources are not loaded; images from files are. */
static u64 w32_LoadImageA(const u32 *args)
{
    if (!(args[5] & 0x10u)) { g_last_error = 1814; return 0; }     /* LR_LOADFROMFILE */
    return w32_forward("LoadImageA", args, 6);
}
static u64 w32_FindWindowExA(const u32 *args) { (void)args; return 0; }   /* Beer has no other process's windows */
static u64 w32_EnumWindows(const u32 *args) { (void)args; return 1; }     /* ...so there is nothing to enumerate */

#include "wow32_win32.inc"

/* ---- msvcrt: a 32-bit C runtime on top of the host libc ----
 * FILE is msvcrt's 32-byte struct; the guest only passes the pointer around (and takes &_iob[n] for the standard
 * streams), so each slot of a pool in low memory stands for one host FILE. */
#define W32_FILE_SLOTS 64
static u8 *g_w32_file_pool;
static FILE *g_w32_file_host[W32_FILE_SLOTS];
static pthread_mutex_t g_w32_file_lock = PTHREAD_MUTEX_INITIALIZER;

static u8 *w32_file_pool(void)
{
    pthread_mutex_lock(&g_w32_file_lock);
    if (!g_w32_file_pool) {
        g_w32_file_pool = w32_map(0, W32_FILE_SLOTS * 32, PROT_READ | PROT_WRITE);
        if (!g_w32_file_pool) die("WoW32: no low memory for the CRT FILE table");
        g_w32_file_host[0] = stdin; g_w32_file_host[1] = stdout; g_w32_file_host[2] = stderr;
        for (int i = 0; i < 3; ++i) *(s32 *)(g_w32_file_pool + 32 * i + 16) = i;
    }
    pthread_mutex_unlock(&g_w32_file_lock);
    return g_w32_file_pool;
}
static u32 w32_data_import(const char *name)
{
    if (!strcmp(name, "_iob") || !strcmp(name, "__imp___iob")) return (u32)(uintptr_t)w32_file_pool();
    return 0;
}
static FILE *w32_host_file(u32 guest)
{
    const u8 *pool = w32_file_pool();
    const uintptr_t offset = (uintptr_t)guest - (uintptr_t)pool;
    if ((uintptr_t)guest < (uintptr_t)pool || offset >= W32_FILE_SLOTS * 32 || offset % 32) return NULL;
    return g_w32_file_host[offset / 32];
}

static u64 w32_fopen(const u32 *args)
{
    char host[PATH_MAX];
    if (!resolve_windows_host_path(W32_PTR(0), host, sizeof(host))) { errno = ENOENT; return 0; }
    FILE *file = fopen(host, W32_PTR(1));
    if (!file) return 0;
    u8 *pool = w32_file_pool();
    pthread_mutex_lock(&g_w32_file_lock);
    for (int i = 3; i < W32_FILE_SLOTS; ++i) {
        if (g_w32_file_host[i]) continue;
        g_w32_file_host[i] = file;
        memset(pool + 32 * i, 0, 32);
        *(s32 *)(pool + 32 * i + 16) = fileno(file);
        pthread_mutex_unlock(&g_w32_file_lock);
        return (u32)(uintptr_t)(pool + 32 * i);
    }
    pthread_mutex_unlock(&g_w32_file_lock);
    fclose(file);
    return 0;
}
static u64 w32_fclose(const u32 *args)
{
    FILE *file = w32_host_file(args[0]);
    if (!file) return (u32)-1;
    const uintptr_t slot = ((uintptr_t)args[0] - (uintptr_t)g_w32_file_pool) / 32;
    if (slot < 3) return fflush(file) ? (u32)-1 : 0;
    pthread_mutex_lock(&g_w32_file_lock);
    g_w32_file_host[slot] = NULL;
    pthread_mutex_unlock(&g_w32_file_lock);
    return (u32)fclose(file);
}
static u64 w32_fwrite(const u32 *args)
{
    FILE *file = w32_host_file(args[3]);
    return file ? (u32)fwrite(W32_PTR(0), args[1], args[2], file) : 0;
}

/* printf-style formatting where the arguments are 32-bit stack slots (int, pointer: one slot; 64-bit integer: two).
 * Floating-point conversions are not supported (the loader is built without SSE): their two slots are skipped. */
static size_t w32_format(char *out, size_t capacity, const char *format, const u32 *va)
{
    size_t length = 0;
#define W32_EMIT(...) do { \
        const int n_ = snprintf(out + length, capacity - length, __VA_ARGS__); \
        if (n_ > 0) length += (size_t)n_; \
        if (length >= capacity) length = capacity - 1; \
    } while (0)
    for (; *format && length + 1 < capacity; ++format) {
        if (*format != '%') { out[length++] = *format; out[length] = 0; continue; }
        if (format[1] == '%') { out[length++] = '%'; out[length] = 0; ++format; continue; }
        char spec[64];
        size_t n = 0;
        spec[n++] = *format++;
        while (*format && strchr("-+ #0", *format) && n < 16) spec[n++] = *format++;
        if (*format == '*') { n += (size_t)snprintf(spec + n, sizeof(spec) - n, "%d", (int)*va++); ++format; }
        else while (isdigit((unsigned char)*format) && n < 24) spec[n++] = *format++;
        if (*format == '.') {
            spec[n++] = *format++;
            if (*format == '*') { n += (size_t)snprintf(spec + n, sizeof(spec) - n, "%d", (int)*va++); ++format; }
            else while (isdigit((unsigned char)*format) && n < 40) spec[n++] = *format++;
        }
        int wide = 0, shorts = 0;
        if (format[0] == 'I' && format[1] == '6' && format[2] == '4') { wide = 1; format += 3; }
        else if (format[0] == 'I' && format[1] == '3' && format[2] == '2') format += 3;
        else if (format[0] == 'l' && format[1] == 'l') { wide = 1; format += 2; }
        else for (; *format == 'h' || *format == 'l' || *format == 'z' || *format == 'j' || *format == 't' ||
                    *format == 'L'; ++format)
                 if (*format == 'h') ++shorts;
        const char conversion = *format;
        if (!conversion) break;
        switch (conversion) {
        case 'd': case 'i': {
            long long v = wide ? (long long)(((u64)va[1] << 32) | va[0]) : (long long)(s32)va[0];
            va += wide ? 2 : 1;
            if (shorts == 1) v = (short)v; else if (shorts >= 2) v = (signed char)v;
            spec[n++] = 'l'; spec[n++] = 'l'; spec[n++] = 'd'; spec[n] = 0;
            W32_EMIT(spec, v);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            unsigned long long v = wide ? (((u64)va[1] << 32) | va[0]) : (unsigned long long)va[0];
            va += wide ? 2 : 1;
            if (shorts == 1) v = (unsigned short)v; else if (shorts >= 2) v = (unsigned char)v;
            spec[n++] = 'l'; spec[n++] = 'l'; spec[n++] = conversion; spec[n] = 0;
            W32_EMIT(spec, v);
            break;
        }
        case 'c':
            spec[n++] = 'c'; spec[n] = 0;
            W32_EMIT(spec, (int)*va++);
            break;
        case 's': {
            const char *text = (const char *)(uintptr_t)*va++;
            spec[n++] = 's'; spec[n] = 0;
            W32_EMIT(spec, text ? text : "(null)");
            break;
        }
        case 'p':
            W32_EMIT("%08X", *va++);
            break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            va += 2;
            break;
        case 'n':
            ++va;
            break;
        default:
            W32_EMIT("%c", conversion);
            break;
        }
    }
#undef W32_EMIT
    out[length] = 0;
    return length;
}

static u64 w32_fprintf(const u32 *args)
{
    FILE *file = w32_host_file(args[0]);
    if (!file) return (u32)-1;
    char text[8192];
    const size_t length = w32_format(text, sizeof(text), W32_PTR(1), args + 2);
    return (u32)fwrite(text, 1, length, file);
}
static u64 w32_printf(const u32 *args)
{
    char text[8192];
    const size_t length = w32_format(text, sizeof(text), W32_PTR(0), args + 1);
    return (u32)fwrite(text, 1, length, stdout);
}
static u64 w32_puts(const u32 *args) { return puts(W32_PTR(0)) < 0 ? (u32)-1 : 1; }

/* Process start-up: argv/envp for __getmainargs. */
static u32 *g_w32_environ_cell, *g_w32_fmode_cell;      /* the variables live in 32-bit memory: the guest gets their address */
static u32 *w32_cell(u32 **cell)
{
    pthread_mutex_lock(&g_w32_heap_lock);
    if (!*cell) *cell = w32_map(0, 0x10000, PROT_READ | PROT_WRITE);
    pthread_mutex_unlock(&g_w32_heap_lock);
    return *cell;
}
static u32 *w32_build_vector(char **items, u32 count)
{
    u32 *vector = w32_heap_alloc(4 * (count + 1), 1);
    if (!vector) return NULL;
    for (u32 i = 0; i < count; ++i) vector[i] = w32_strdup(items[i]);
    return vector;
}
static u64 w32_getmainargs(const u32 *args)
{
    /* Split the command line the way the C runtime does: blanks separate arguments, double quotes group. */
    const char *p = (const char *)(uintptr_t)w32_GetCommandLineA(NULL);
    char *items[256];
    u32 count = 0;
    while (*p && count < 255) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        char *item = malloc(strlen(p) + 1);
        size_t n = 0;
        int quoted = 0;
        for (; *p && (quoted || (*p != ' ' && *p != '\t')); ++p) {
            if (*p == '"') quoted = !quoted;
            else item[n++] = *p;
        }
        item[n] = 0;
        items[count++] = item;
    }
    u32 *argv = w32_build_vector(items, count);
    for (u32 i = 0; i < count; ++i) free(items[i]);
    u32 env_count = 0;
    while (environ && environ[env_count] && env_count < 4096) ++env_count;
    u32 *envp = w32_build_vector(environ, env_count);
    if (!argv || !envp) return (u32)-1;
    *w32_cell(&g_w32_environ_cell) = (u32)(uintptr_t)envp;
    if (args[0]) *(u32 *)W32_PTR(0) = count;
    if (args[1]) *(u32 *)W32_PTR(1) = (u32)(uintptr_t)argv;
    if (args[2]) *(u32 *)W32_PTR(2) = (u32)(uintptr_t)envp;
    return 0;
}
static u64 w32_p_environ(const u32 *args) { (void)args; return (u32)(uintptr_t)w32_cell(&g_w32_environ_cell); }
static u64 w32_p_fmode(const u32 *args) { (void)args; return (u32)(uintptr_t)w32_cell(&g_w32_fmode_cell); }
static u64 w32_set_app_type(const u32 *args) { (void)args; return 0; }

#define W32_ATEXIT_MAX 64
static u32 g_w32_atexit[W32_ATEXIT_MAX];
static u32 g_w32_atexit_count;
static u64 w32_atexit(const u32 *args)
{
    if (g_w32_atexit_count >= W32_ATEXIT_MAX) return (u32)-1;
    g_w32_atexit[g_w32_atexit_count++] = args[0];
    return 0;
}
static u64 w32_onexit(const u32 *args) { return w32_atexit(args) ? 0 : args[0]; }
static u64 w32_cexit(const u32 *args)
{
    (void)args;
    while (g_w32_atexit_count) w32_call(g_w32_atexit[--g_w32_atexit_count], 0, NULL);
    return 0;
}
static u32 g_w32_signal[32];
static u64 w32_signal(const u32 *args)
{
    if (args[0] >= 32) { errno = EINVAL; return (u32)-1; }
    const u32 previous = g_w32_signal[args[0]];
    g_w32_signal[args[0]] = args[1];                        /* recorded only: Beer raises no CRT signals */
    return previous;
}

static u64 w32_memset(const u32 *args) { memset(W32_PTR(0), (int)args[1], args[2]); return args[0]; }
static u64 w32_strlen(const u32 *args) { return (u32)strlen(W32_PTR(0)); }
static u64 w32_strcmp(const u32 *args) { return (u32)strcmp(W32_PTR(0), W32_PTR(1)); }
static u64 w32_strcpy(const u32 *args) { strcpy(W32_PTR(0), W32_PTR(1)); return args[0]; }
static u64 w32_strcat(const u32 *args) { strcat(W32_PTR(0), W32_PTR(1)); return args[0]; }
static u64 w32_strncpy(const u32 *args) { strncpy(W32_PTR(0), W32_PTR(1), args[2]); return args[0]; }
static u64 w32_strncat(const u32 *args) { strncat(W32_PTR(0), W32_PTR(1), args[2]); return args[0]; }
static u64 w32_strchr(const u32 *args) { return (u32)(uintptr_t)strchr(W32_PTR(0), (int)args[1]); }
static u64 w32_strrchr(const u32 *args) { return (u32)(uintptr_t)strrchr(W32_PTR(0), (int)args[1]); }
static u64 w32_strstr(const u32 *args) { return (u32)(uintptr_t)strstr(W32_PTR(0), W32_PTR(1)); }
static u64 w32_strpbrk(const u32 *args) { return (u32)(uintptr_t)strpbrk(W32_PTR(0), W32_PTR(1)); }
static u64 w32_strtok(const u32 *args) { return (u32)(uintptr_t)strtok(W32_PTR(0), W32_PTR(1)); }
static u64 w32_atoi(const u32 *args) { return (u32)atoi(W32_PTR(0)); }
static u64 w32_itoa(const u32 *args)
{
    char *buffer = W32_PTR(1);
    const int radix = (int)args[2];
    if (radix < 2 || radix > 36) { buffer[0] = 0; return args[1]; }
    char digits[40];
    size_t n = 0;
    const int negative = radix == 10 && (s32)args[0] < 0;
    u32 value = negative ? (u32)(-(s64)(s32)args[0]) : args[0];
    do { digits[n++] = "0123456789abcdefghijklmnopqrstuvwxyz"[value % (u32)radix]; value /= (u32)radix; } while (value);
    size_t at = 0;
    if (negative) buffer[at++] = '-';
    while (n) buffer[at++] = digits[--n];
    buffer[at] = 0;
    return args[1];
}

/* Low-level I/O: CRT descriptors are host descriptors. */
static u64 w32_open(const u32 *args)
{
    char host[PATH_MAX];
    if (!resolve_windows_host_path(W32_PTR(0), host, sizeof(host))) { errno = ENOENT; return (u32)-1; }
    const u32 mode = args[1];
    int flags = (int)(mode & 3u);
    if (mode & 0x08u) flags |= O_APPEND;
    if (mode & 0x100u) flags |= O_CREAT;
    if (mode & 0x200u) flags |= O_TRUNC;
    if (mode & 0x400u) flags |= O_EXCL;
    return (u32)open(host, flags | O_CLOEXEC, 0666);
}
static u64 w32_close(const u32 *args) { return (u32)close((int)args[0]); }
static u64 w32_read(const u32 *args) { return (u32)read((int)args[0], W32_PTR(1), args[2]); }
static u64 w32_setmode(const u32 *args) { (void)args; return 0x8000; }   /* _O_BINARY: text mode is not translated */

static u64 w32_chdir(const u32 *args) { return impl_SetCurrentDirectoryA(W32_PTR(0)) ? 0 : (u32)-1; }

static u64 w32_stat(const u32 *args)
{
    char host[PATH_MAX];
    struct stat st;
    if (!resolve_windows_host_path(W32_PTR(0), host, sizeof(host)) || stat(host, &st) != 0) {
        errno = ENOENT;
        return (u32)-1;
    }
    u8 *out = W32_PTR(1);
    memset(out, 0, 36);                                     /* struct _stat of the 32-bit msvcrt */
    u16 mode = S_ISDIR(st.st_mode) ? 0x4000 : 0x8000;
    u16 permissions = 0;
    if (st.st_mode & S_IRUSR) permissions |= 0x100;
    if (st.st_mode & S_IWUSR) permissions |= 0x80;
    if (S_ISDIR(st.st_mode) || (st.st_mode & S_IXUSR)) permissions |= 0x40;
    mode |= permissions | (u16)((permissions & 0x1c0) >> 3) | (u16)((permissions & 0x1c0) >> 6);
    *(u32 *)(out + 0) = (u32)st.st_dev;
    *(u16 *)(out + 4) = (u16)st.st_ino;
    *(u16 *)(out + 6) = mode;
    *(int16_t *)(out + 8) = 1;
    *(u32 *)(out + 16) = (u32)st.st_dev;
    *(s32 *)(out + 20) = st.st_size > 0x7fffffff ? 0x7fffffff : (s32)st.st_size;
    *(s32 *)(out + 24) = (s32)st.st_atime;
    *(s32 *)(out + 28) = (s32)st.st_mtime;
    *(s32 *)(out + 32) = (s32)st.st_ctime;
    return 0;
}

/* _findfirst/_findnext/_findclose: struct _finddata_t is 280 bytes (attrib, 3 times, size, name[260]). */
#define W32_FIND_MAX 16
static struct { DIR *dir; char directory[PATH_MAX]; char pattern[260]; } g_w32_find[W32_FIND_MAX];
static pthread_mutex_t g_w32_find_lock = PTHREAD_MUTEX_INITIALIZER;

static int w32_find_next_entry(int slot, u8 *out)
{
    struct dirent *entry;
    while ((entry = readdir(g_w32_find[slot].dir)) != NULL) {
        if (fnmatch(g_w32_find[slot].pattern, entry->d_name, FNM_CASEFOLD)) continue;
        char path[PATH_MAX + 300];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", g_w32_find[slot].directory, entry->d_name);
        if (stat(path, &st) != 0) continue;
        memset(out, 0, 280);
        *(u32 *)(out + 0) = S_ISDIR(st.st_mode) ? 0x10u : 0x20u;
        if (!(st.st_mode & S_IWUSR)) *(u32 *)(out + 0) |= 0x1u;
        *(s32 *)(out + 4) = (s32)st.st_ctime;
        *(s32 *)(out + 8) = (s32)st.st_atime;
        *(s32 *)(out + 12) = (s32)st.st_mtime;
        *(u32 *)(out + 16) = st.st_size > 0xffffffffll ? 0xffffffffu : (u32)st.st_size;
        snprintf((char *)out + 20, 260, "%s", entry->d_name);
        return 1;
    }
    return 0;
}
static u64 w32_findfirst(const u32 *args)
{
    char spec[PATH_MAX], host[PATH_MAX];
    snprintf(spec, sizeof(spec), "%s", (const char *)W32_PTR(0));
    char *separator = NULL;
    for (char *c = spec; *c; ++c) if (*c == '\\' || *c == '/') separator = c;
    char pattern[260];
    if (separator) {
        snprintf(pattern, sizeof(pattern), "%s", separator + 1);
        *separator = 0;
        if (!spec[0]) snprintf(spec, sizeof(spec), "/");                     /* "\\*.txt": the root */
        else if (spec[1] == ':' && !spec[2]) snprintf(spec + 2, sizeof(spec) - 2, "\\");   /* "C:\\*" */
    } else {
        snprintf(pattern, sizeof(pattern), "%s", spec);
        snprintf(spec, sizeof(spec), ".");
    }
    if (!strcmp(pattern, "*.*")) snprintf(pattern, sizeof(pattern), "*");
    if (!strcmp(spec, ".")) snprintf(host, sizeof(host), ".");
    else if (!resolve_windows_host_path(spec, host, sizeof(host))) { errno = ENOENT; return (u32)-1; }
    DIR *dir = opendir(host);
    if (!dir) { errno = ENOENT; return (u32)-1; }
    pthread_mutex_lock(&g_w32_find_lock);
    int slot = -1;
    for (int i = 0; i < W32_FIND_MAX && slot < 0; ++i) if (!g_w32_find[i].dir) slot = i;
    if (slot >= 0) {
        g_w32_find[slot].dir = dir;
        snprintf(g_w32_find[slot].directory, sizeof(g_w32_find[slot].directory), "%s", host);
        snprintf(g_w32_find[slot].pattern, sizeof(g_w32_find[slot].pattern), "%s", pattern);
    }
    pthread_mutex_unlock(&g_w32_find_lock);
    if (slot < 0) { closedir(dir); errno = EMFILE; return (u32)-1; }
    if (!w32_find_next_entry(slot, W32_PTR(1))) {
        closedir(dir);
        g_w32_find[slot].dir = NULL;
        errno = ENOENT;
        return (u32)-1;
    }
    return (u32)(slot + 1);
}
static u64 w32_findnext(const u32 *args)
{
    const int slot = (int)args[0] - 1;
    if (slot < 0 || slot >= W32_FIND_MAX || !g_w32_find[slot].dir) { errno = EINVAL; return (u32)-1; }
    if (w32_find_next_entry(slot, W32_PTR(1))) return 0;
    errno = ENOENT;
    return (u32)-1;
}
static u64 w32_findclose(const u32 *args)
{
    const int slot = (int)args[0] - 1;
    if (slot < 0 || slot >= W32_FIND_MAX || !g_w32_find[slot].dir) { errno = EINVAL; return (u32)-1; }
    pthread_mutex_lock(&g_w32_find_lock);
    closedir(g_w32_find[slot].dir);
    g_w32_find[slot].dir = NULL;
    pthread_mutex_unlock(&g_w32_find_lock);
    return 0;
}

/* Table: a NULL function forwards to the 64-bit implementation of the same name (arguments in bytes). */
#define W32_FWD(name, bytes) {name, NULL, bytes}

static const W32Api g_w32_apis[] = {
    /* kernel32 (platform/wow32_win32.inc) */
    {"GetModuleHandleW", w32_GetModuleHandleW, 4},
    {"WaitForMultipleObjects", w32_WaitForMultipleObjects, 16},
    {"MsgWaitForMultipleObjects", w32_MsgWaitForMultipleObjects, 20},
    {"MsgWaitForMultipleObjectsEx", w32_MsgWaitForMultipleObjectsEx, 20},
    {"CreateProcessW", w32_CreateProcessW, 40},
    {"SetWindowLongW", w32_SetWindowLongW, 12}, {"GetWindowLongW", w32_GetWindowLongW, 8},
    {"SetWindowLongA", w32_SetWindowLongW, 12}, {"GetWindowLongA", w32_GetWindowLongW, 8},
    {"CallWindowProcW", w32_CallWindowProcW, 20}, {"CallWindowProcA", w32_CallWindowProcW, 20},
    {"RegisterClassW", w32_RegisterClassW, 4}, {"RegisterClassExW", w32_RegisterClassExW, 4},
    {"PeekMessageW", w32_PeekMessageW, 20}, {"GetMessageW", w32_GetMessageW, 16},
    {"DispatchMessageW", w32_DispatchMessageW, 4}, {"TranslateMessage", w32_TranslateMessage, 4},
    W32_FWD("CreateWindowExW", 48), W32_FWD("DefWindowProcW", 16), W32_FWD("SendMessageW", 16),
    W32_FWD("PostMessageW", 16), W32_FWD("PostQuitMessage", 4), W32_FWD("ShowWindow", 8),
    {"FindResourceW", w32_FindResourceW, 12}, {"FindResourceExW", w32_FindResourceExW, 16},
    {"FindResourceA", w32_FindResourceA, 12},
    {"GetDiskFreeSpaceW", w32_GetDiskFreeSpaceW, 20}, {"GetDiskFreeSpaceExW", w32_GetDiskFreeSpaceExW, 16},
    {"LoadLibraryA", w32_LoadLibraryA, 4}, {"LoadLibraryW", w32_LoadLibraryW, 4},
    {"LoadLibraryExA", w32_LoadLibraryExA, 12}, {"LoadLibraryExW", w32_LoadLibraryExW, 12},
    {"FreeLibrary", w32_FreeLibrary, 4},
    {"GetModuleFileNameW", w32_GetModuleFileNameW, 12},
    {"GetSystemInfo", w32_GetSystemInfo, 4}, {"GetNativeSystemInfo", w32_GetNativeSystemInfo, 4},
    {"InitializeCriticalSection", w32_InitializeCriticalSection, 4},
    {"InitializeCriticalSectionAndSpinCount", w32_InitializeCriticalSectionAndSpinCount, 8},
    {"DeleteCriticalSection", w32_DeleteCriticalSection, 4},
    {"EnterCriticalSection", w32_EnterCriticalSection, 4},
    {"TryEnterCriticalSection", w32_TryEnterCriticalSection, 4},
    {"LeaveCriticalSection", w32_LeaveCriticalSection, 4},
    {"InterlockedCompareExchange", w32_InterlockedCompareExchange, 12},
    {"InterlockedExchange", w32_InterlockedExchange, 8},
    {"InterlockedExchangeAdd", w32_InterlockedExchangeAdd, 8},
    {"InterlockedIncrement", w32_InterlockedIncrement, 4},
    {"InterlockedDecrement", w32_InterlockedDecrement, 4},
    {"CreateFileW", w32_CreateFileW, 28}, {"CreateFileA", w32_CreateFileA, 28},
    {"CreateEventW", w32_CreateEventW, 16}, {"CreateEventA", w32_CreateEventA, 16},
    {"CreateMutexW", w32_CreateMutexW, 12},
    {"CreateDirectoryW", w32_CreateDirectoryW, 8}, {"CreateDirectoryA", w32_CreateDirectoryA, 8},
    {"TlsAlloc", w32_TlsAlloc, 0}, {"TlsFree", w32_TlsFree, 4},
    {"TlsGetValue", w32_TlsGetValue, 4}, {"TlsSetValue", w32_TlsSetValue, 8},
    {"LocalAlloc", w32_LocalAlloc, 8}, {"LocalReAlloc", w32_LocalReAlloc, 12}, {"LocalSize", w32_LocalSize, 4},
    {"GlobalAlloc", w32_LocalAlloc, 8}, {"GlobalReAlloc", w32_LocalReAlloc, 12}, {"GlobalSize", w32_LocalSize, 4},
    {"GlobalFree", w32_GlobalFree, 4}, {"GlobalLock", w32_GlobalLock, 4}, {"GlobalUnlock", w32_GlobalUnlock, 4},
    {"GlobalHandle", w32_GlobalHandle, 4},
    {"VirtualAlloc", w32_VirtualAlloc, 16}, {"VirtualFree", w32_VirtualFree, 12},
    {"VirtualQuery", w32_VirtualQuery, 12},
    {"GetFullPathNameW", w32_GetFullPathNameW, 16}, {"GetFullPathNameA", w32_GetFullPathNameA, 16},
    {"FormatMessageW", w32_FormatMessageW, 28},
    W32_FWD("CompareStringW", 24), W32_FWD("CompareStringA", 24),
    W32_FWD("DeleteFileW", 4), W32_FWD("DeleteFileA", 4),
    W32_FWD("FindFirstFileW", 8), W32_FWD("FindNextFileW", 8), W32_FWD("FindClose", 4),
    W32_FWD("FindFirstFileA", 8), W32_FWD("FindNextFileA", 8),
    W32_FWD("GetACP", 0), W32_FWD("GetOEMCP", 0), W32_FWD("GetCPInfo", 8),
    W32_FWD("GetDateFormatW", 24), W32_FWD("GetTimeFormatW", 24),
    W32_FWD("GetEnvironmentVariableW", 12), W32_FWD("SetEnvironmentVariableW", 8),
    W32_FWD("GetFileAttributesW", 4), W32_FWD("GetFileAttributesA", 4), W32_FWD("SetFileAttributesW", 8),
    W32_FWD("GetFileSize", 8), W32_FWD("GetFileType", 4),
    W32_FWD("GetLocaleInfoW", 16), W32_FWD("GetLocaleInfoA", 16), W32_FWD("GetLocalTime", 4),
    W32_FWD("GetSystemTime", 4), W32_FWD("GetThreadLocale", 0), W32_FWD("GetUserDefaultLangID", 0),
    W32_FWD("GetUserDefaultLCID", 0), W32_FWD("GetSystemDefaultLangID", 0), W32_FWD("GetUserDefaultUILanguage", 0),
    W32_FWD("GetVersion", 0), W32_FWD("GetVersionExW", 4), W32_FWD("GetVersionExA", 4),
    W32_FWD("GetWindowsDirectoryW", 8), W32_FWD("GetSystemDirectoryW", 8), W32_FWD("GetTempPathW", 8),
    W32_FWD("GetTempFileNameW", 16), W32_FWD("GetCurrentDirectoryW", 8), W32_FWD("SetCurrentDirectoryW", 4),
    W32_FWD("lstrcpynW", 12), W32_FWD("lstrlenW", 4), W32_FWD("lstrlenA", 4), W32_FWD("lstrcpyW", 8),
    W32_FWD("lstrcmpiW", 8), W32_FWD("lstrcmpW", 8),
    W32_FWD("MultiByteToWideChar", 24), W32_FWD("WideCharToMultiByte", 32),
    W32_FWD("RemoveDirectoryW", 4), W32_FWD("MoveFileW", 8), W32_FWD("MoveFileExW", 12), W32_FWD("CopyFileW", 12),
    W32_FWD("SetEvent", 4), W32_FWD("ResetEvent", 4), W32_FWD("SignalObjectAndWait", 16),
W32_FWD("ReleaseMutex", 4),
    W32_FWD("SetEndOfFile", 4), W32_FWD("SetFilePointer", 16), W32_FWD("FlushFileBuffers", 4),
    W32_FWD("SetErrorMode", 4), W32_FWD("VirtualProtect", 16), W32_FWD("EnumCalendarInfoW", 16),
    W32_FWD("GetStringTypeW", 16), W32_FWD("LCMapStringW", 24), W32_FWD("GetUserDefaultLocaleName", 8),
    W32_FWD("GetTickCount64", 0), W32_FWD("FileTimeToLocalFileTime", 8), W32_FWD("FileTimeToSystemTime", 8),
    W32_FWD("SystemTimeToFileTime", 8), W32_FWD("FileTimeToDosDateTime", 12), W32_FWD("SetFileTime", 16),
    W32_FWD("GetFileTime", 16), W32_FWD("DuplicateHandle", 28), W32_FWD("GetExitCodeThread", 8),
    /* advapi32 */
    {"RegOpenKeyExW", w32_RegOpenKeyExW, 20}, {"RegCreateKeyExW", w32_RegCreateKeyExW, 36},
    {"OpenProcessToken", w32_OpenProcessToken, 12},
    W32_FWD("RegQueryValueExW", 24), W32_FWD("RegSetValueExW", 24), W32_FWD("RegDeleteValueW", 8),
    W32_FWD("LookupPrivilegeValueW", 12), W32_FWD("AdjustTokenPrivileges", 24),
    /* oleaut32 */
    {"SysAllocString", w32_SysAllocString, 4}, {"SysAllocStringLen", w32_SysAllocStringLen, 8},
    {"SysFreeString", w32_SysFreeString, 4}, {"SysStringLen", w32_SysStringLen, 4},
    {"SysReAllocStringLen", w32_SysReAllocStringLen, 12},
    {"VariantInit", w32_VariantInit, 4}, {"VariantClear", w32_VariantClear, 4},
    {"VariantCopy", w32_VariantCopy, 8}, {"VariantChangeType", w32_VariantChangeType, 16},
    /* user32 / comctl32 */
    {"CharNextW", w32_CharNextW, 4}, {"CharUpperBuffW", w32_CharUpperBuffW, 8},
    {"CharLowerBuffW", w32_CharLowerBuffW, 8}, {"GetKeyboardType", w32_GetKeyboardType, 4},
    {"InitCommonControls", w32_InitCommonControls, 0}, {"MessageBoxW", w32_MessageBoxW, 16},
    W32_FWD("LoadStringW", 16), W32_FWD("DestroyWindow", 4), W32_FWD("ExitWindowsEx", 8),
    /* kernel32 */
    {"GetStartupInfoA", w32_GetStartupInfoA, 4},
    {"CreateProcessA", w32_CreateProcessA, 40},
    {"FormatMessageA", w32_FormatMessageA, 28},
    {"LocalFree", w32_LocalFree, 4},
    {"GetModuleHandleA", w32_GetModuleHandleA, 4},
    {"GetProcAddress", w32_GetProcAddress, 8},
    {"SetUnhandledExceptionFilter", w32_SetUnhandledExceptionFilter, 4},
    {"IsWow64Process", w32_IsWow64Process, 8},
    {"FindResourceExA", w32_FindResourceExA, 16},
    {"LoadResource", w32_LoadResource, 8},
    {"LockResource", w32_LockResource, 4},
    {"SizeofResource", w32_SizeofResource, 8},
    W32_FWD("CreateMutexA", 12),
    W32_FWD("GetCurrentDirectoryA", 8),
    W32_FWD("GetEnvironmentVariableA", 12),
    W32_FWD("SetEnvironmentVariableA", 8),
    W32_FWD("GetExitCodeProcess", 8),
    W32_FWD("GetModuleFileNameA", 12),
    W32_FWD("GlobalMemoryStatusEx", 4),
    W32_FWD("WaitForSingleObject", 8),
    /* advapi32 */
    {"RegOpenKeyExA", w32_RegOpenKeyExA, 20},
    W32_FWD("RegCloseKey", 4),
    W32_FWD("RegEnumKeyExA", 32),
    W32_FWD("RegQueryValueExA", 24),
    /* shell32 / user32 */
    W32_FWD("ShellExecuteA", 24),
    W32_FWD("CreateWindowExA", 48),
    {"DispatchMessageA", w32_DispatchMessageA, 4},
    {"EnumWindows", w32_EnumWindows, 8},
    {"FindWindowExA", w32_FindWindowExA, 16},
    {"GetMessageA", w32_GetMessageA, 16},
    W32_FWD("GetSystemMetrics", 4),
    W32_FWD("GetWindowRect", 8),
    W32_FWD("GetWindowTextA", 12),
    W32_FWD("GetWindowThreadProcessId", 8),
    W32_FWD("KillTimer", 8),
    {"LoadImageA", w32_LoadImageA, 24},
    W32_FWD("SendMessageA", 16),
    W32_FWD("SetForegroundWindow", 4),
    {"SetTimer", w32_SetTimer, 16},
    W32_FWD("SetWindowPos", 28),
    W32_FWD("ShowWindow", 8),
    W32_FWD("UpdateWindow", 4),
    /* msvcrt (cdecl: the caller pops the arguments) */
    {"__getmainargs", w32_getmainargs, 0},
    {"__p__environ", w32_p_environ, 0},
    {"__p__fmode", w32_p_fmode, 0},
    {"__set_app_type", w32_set_app_type, 0},
    {"_cexit", w32_cexit, 0},
    {"atexit", w32_atexit, 0},
    {"_onexit", w32_onexit, 0},
    {"signal", w32_signal, 0},
    {"_chdir", w32_chdir, 0},
    {"_open", w32_open, 0},
    {"_close", w32_close, 0},
    {"_read", w32_read, 0},
    {"_setmode", w32_setmode, 0},
    {"_stat", w32_stat, 0},
    {"_findfirst", w32_findfirst, 0},
    {"_findnext", w32_findnext, 0},
    {"_findclose", w32_findclose, 0},
    {"_itoa", w32_itoa, 0},
    {"atoi", w32_atoi, 0},
    {"fopen", w32_fopen, 0},
    {"fclose", w32_fclose, 0},
    {"fwrite", w32_fwrite, 0},
    {"fprintf", w32_fprintf, 0},
    {"printf", w32_printf, 0},
    {"puts", w32_puts, 0},
    {"memset", w32_memset, 0},
    {"strlen", w32_strlen, 0},
    {"strcmp", w32_strcmp, 0},
    {"strcpy", w32_strcpy, 0},
    {"strcat", w32_strcat, 0},
    {"strncpy", w32_strncpy, 0},
    {"strncat", w32_strncat, 0},
    {"strchr", w32_strchr, 0},
    {"strrchr", w32_strrchr, 0},
    {"strstr", w32_strstr, 0},
    {"strpbrk", w32_strpbrk, 0},
    {"strtok", w32_strtok, 0},
    /* previously provided */
    {"GetStdHandle", w32_GetStdHandle, 4},
    {"WriteFile", w32_WriteFile, 20},
    {"ReadFile", w32_ReadFile, 20},
    {"ExitProcess", w32_ExitProcess, 4},
    {"TerminateProcess", w32_TerminateProcess, 8},
    {"GetLastError", w32_GetLastError, 0},
    {"SetLastError", w32_SetLastError, 4},
    {"CloseHandle", w32_CloseHandle, 4},
    {"Sleep", w32_Sleep, 4},
    {"GetCurrentProcessId", w32_GetCurrentProcessId, 0},
    {"GetCurrentThreadId", w32_GetCurrentThreadId, 0},
    {"GetCurrentProcess", w32_GetCurrentProcess, 0},
    {"IsDebuggerPresent", w32_IsDebuggerPresent, 0},
    {"IsProcessorFeaturePresent", w32_IsProcessorFeaturePresent, 4},
    {"QueryPerformanceCounter", w32_QueryPerformanceCounter, 4},
    {"QueryPerformanceFrequency", w32_QueryPerformanceFrequency, 4},
    {"GetSystemTimeAsFileTime", w32_GetSystemTimeAsFileTime, 4},
    {"GetTickCount", w32_GetTickCount, 0},
    {"GetProcessHeap", w32_GetProcessHeap, 0},
    {"HeapAlloc", w32_HeapAlloc, 12},
    {"HeapFree", w32_HeapFree, 12},
    {"HeapReAlloc", w32_HeapReAlloc, 16},
    {"HeapSize", w32_HeapSize, 12},
    {"MessageBoxA", w32_MessageBoxA, 16},
    {"GetCommandLineA", w32_GetCommandLineA, 0},
    {"GetCommandLineW", w32_GetCommandLineW, 0},
};

static const W32Api *w32_find_api(const char *name)
{
    for (size_t i = 0; i < sizeof(g_w32_apis) / sizeof(*g_w32_apis); ++i)
        if (!strcmp(g_w32_apis[i].name, name)) return &g_w32_apis[i];
    return NULL;
}

/* ---- PE32 image loading ---- */

static u8 *w32_image_load(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) die("WoW32: cannot open %s", path);
    struct stat st;
    if (fstat(fd, &st) != 0) die("WoW32: fstat");
    u8 *file = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (file == MAP_FAILED) die("WoW32: mmap file");
    const W32NtHdrs *nt = (const W32NtHdrs *)(file + ((const DosHdr *)file)->lfanew);
    if (nt->sig != PE_SIG || nt->opt.magic != 0x10b || nt->file.machine != 0x14c)
        die("WoW32: %s is not an i386 PE32 image", path);
    const u32 size = nt->opt.size_of_image;
    u8 *image = w32_map(nt->opt.image_base, size, PROT_READ | PROT_WRITE | PROT_EXEC);
    if (!image) die("WoW32: no low memory for the image");
    memcpy(image, file, nt->opt.size_of_headers);
    const SecHdr *secs = (const SecHdr *)((const u8 *)&nt->opt + nt->file.opthdr_sz);
    for (u16 i = 0; i < nt->file.nsections; ++i) {
        const SecHdr *s = &secs[i];
        if (s->raw_sz && s->raw_off) memcpy(image + s->vrva, file + s->raw_off, s->raw_sz < s->vsz ? s->raw_sz : s->vsz);
    }
    munmap(file, (size_t)st.st_size);
    const W32NtHdrs *mnt = (const W32NtHdrs *)(image + ((const DosHdr *)image)->lfanew);
    const u32 delta = (u32)(uintptr_t)image - mnt->opt.image_base;
    fprintf(stderr, "[WOW32] %s mapped at 0x%x (preferred 0x%x, size 0x%x)\n", path, (u32)(uintptr_t)image,
            mnt->opt.image_base, size);
    if (delta) {                                           /* IMAGE_REL_BASED_HIGHLOW */
        const DataDir *rd = &mnt->opt.dirs[DIR_RELOC];
        if (!rd->size) die("WoW32: image could not load at its base and has no relocations");
        for (u8 *p = image + rd->rva, *end = p + rd->size; p < end;) {
            const RelocBlock *blk = (const RelocBlock *)p;
            if (blk->block_sz < 8) break;
            const u16 *ents = (const u16 *)(blk + 1);
            for (u32 j = 0; j < (blk->block_sz - 8) / 2; ++j)
                if ((ents[j] >> 12) == 3) *(u32 *)(image + blk->page_rva + (ents[j] & 0xfff)) += delta;
            p += blk->block_sz;
        }
    }
    /* Imports: every entry is bound to a thunk. */
    const DataDir *id = &mnt->opt.dirs[1];
    for (const u32 *desc = id->size ? (const u32 *)(image + id->rva) : NULL; desc && desc[3]; desc += 5) {
        const char *dll = (const char *)image + desc[3];
        u32 *iat = (u32 *)(image + desc[4]);
        const u32 *names = (const u32 *)(image + (desc[0] ? desc[0] : desc[4]));
        for (u32 k = 0; names[k]; ++k) {
            char ordinal_name[24];
            const char *name;
            if (names[k] & 0x80000000u) {
                snprintf(ordinal_name, sizeof(ordinal_name), "#%u", names[k] & 0xffffu);
                name = ordinal_name;
            } else {
                name = (const char *)image + names[k] + 2;
            }
            iat[k] = w32_thunk_for(dll, name);
        }
    }
    return image;
}

/* ---- process start ---- */

static void w32_fault(int sig, siginfo_t *info, void *uctx)
{
    /* FS may hold the TEB32 base: restore glibc's before touching libc. */
    u64 teb64 = 0;
    __asm__ volatile("rdgsbase %0" : "=r"(teb64));
    if (teb64) {
        const u64 host_fs = *(u64 *)(uintptr_t)(teb64 + W32_SLOT_HOST_FS);
        __asm__ volatile("wrfsbase %0" : : "r"(host_fs));
    }
    const ucontext_t *uc = uctx;
    fprintf(stderr, "[WOW32] fatal signal %d at eip=0x%llx cs=0x%llx fault address %p (eax=%llx ebx=%llx ecx=%llx "
                    "edx=%llx esi=%llx edi=%llx ebp=%llx esp=%llx)\n",
            sig, (unsigned long long)uc->uc_mcontext.gregs[REG_RIP],
            (unsigned long long)(uc->uc_mcontext.gregs[REG_CSGSFS] & 0xffff), info->si_addr,
            (unsigned long long)uc->uc_mcontext.gregs[REG_RAX], (unsigned long long)uc->uc_mcontext.gregs[REG_RBX],
            (unsigned long long)uc->uc_mcontext.gregs[REG_RCX], (unsigned long long)uc->uc_mcontext.gregs[REG_RDX],
            (unsigned long long)uc->uc_mcontext.gregs[REG_RSI], (unsigned long long)uc->uc_mcontext.gregs[REG_RDI],
            (unsigned long long)uc->uc_mcontext.gregs[REG_RBP], (unsigned long long)uc->uc_mcontext.gregs[REG_RSP]);
    _exit(128 + sig);
}

/* Runs a PE32 executable; does not return. */
static void __attribute__((noreturn)) beer_wow32_main(const char *path)
{
    fprintf(stderr, "[WOW32] 32-bit executable: running it in compatibility mode\n");
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = w32_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL);
    w32_thunks_init();
    /* PEB32: the fields CRT start-up reads. */
    u8 *peb = w32_map(0x7ffd0000, 0x10000, PROT_READ | PROT_WRITE);
    if (!peb) peb = w32_map(0, 0x10000, PROT_READ | PROT_WRITE);
    g_w32_peb = (u32)(uintptr_t)peb;
    u8 *image = w32_image_load(path);
    g_w32_image = (u32)(uintptr_t)image;
    *(u32 *)(peb + 0x08) = g_w32_image;                    /* ImageBaseAddress */
    *(u32 *)(peb + 0x18) = 0x00030000u;                    /* ProcessHeap */
    *(u32 *)(peb + 0x64) = (u32)sysconf(_SC_NPROCESSORS_ONLN);
    *(u32 *)(peb + 0xa4) = 10;                             /* OSMajorVersion */
    *(u32 *)(peb + 0xa8) = 0;
    *(u16 *)(peb + 0xac) = 19045;                          /* OSBuildNumber */
    *(u32 *)(peb + 0xb0) = 2;                              /* OSPlatformId */
    const W32NtHdrs *nt = (const W32NtHdrs *)(image + ((const DosHdr *)image)->lfanew);
    size_t stack = nt->opt.stack_reserve ? nt->opt.stack_reserve : (1u << 20);
    if (stack < (1u << 20)) stack = 1u << 20;
    const u32 esp = w32_thread_init(stack);
    u64 teb64;
    __asm__ volatile("mov %%gs:0x30, %0" : "=r"(teb64));
    *(u64 *)(uintptr_t)(teb64 + W32_SLOT_ESP) = esp;
    fprintf(stderr, "[WOW32] entry 0x%x, stack top 0x%x\n", g_w32_image + nt->opt.entry_rva, esp);
    const u32 code = w32_call(g_w32_image + nt->opt.entry_rva, 0, NULL);
    impl_ExitProcess(code);
    _exit((int)code);
}
