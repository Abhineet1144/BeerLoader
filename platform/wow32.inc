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

u64 w32_dispatch(u32 index)
{
    u64 teb64;
    __asm__ volatile("mov %%gs:0x30, %0" : "=r"(teb64));
    if (index >= g_w32_thunk_count) {
        fprintf(stderr, "[WOW32] bad thunk index %u\n", index);
        _exit(3);
    }
    const W32Thunk *t = &g_w32_thunks[index];
    const u32 esp = (u32)*(u64 *)(uintptr_t)(teb64 + W32_SLOT_ESP);
    *(u64 *)(uintptr_t)(teb64 + W32_SLOT_CLEANUP) = t->arg_bytes;
    *(u64 *)(uintptr_t)(teb64 + W32_SLOT_INDEX) = index;
    if (t->fn == w32_unimplemented) {
        static _Atomic(u32) reported[4096];
        if (index >= 4096 || !atomic_exchange(&reported[index], 1))
            fprintf(stderr, "[WOW32] called unimplemented %s!%s (returns 0)\n", t->dll, t->name);
    }
    const u64 result = t->fn((const u32 *)(uintptr_t)(esp + 4));
    w32_teb()->LastErrorValue = g_last_error;
    return result;
}

static const W32Api *w32_find_api(const char *name);

/* Returns the 32-bit address of a thunk for dll!name, creating it on first use. */
static u32 w32_thunk_for(const char *dll, const char *name)
{
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
    t->fn = api ? api->fn : w32_unimplemented;
    t->arg_bytes = api ? api->arg_bytes : 0;
    if (!api) fprintf(stderr, "[WOW32] import %s!%s is not implemented\n", dll, name);
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
    /* 64-bit: movabs rax, beer_w32_api_entry; jmp rax */
    u8 *p = g_w32_lowstub;
    p[0] = 0x48; p[1] = 0xb8;
    const u64 api_entry = (u64)(uintptr_t)beer_w32_api_entry;
    memcpy(p + 2, &api_entry, 8);
    p[10] = 0xff; p[11] = 0xe0;
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

static const W32Api g_w32_apis[] = {
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
