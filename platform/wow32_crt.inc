/* ---- more of msvcrt (cdecl; used by MinGW-built DLLs such as wintb.dll) ---- */
static u64 w32_crt_malloc(const u32 *args)  { return (u32)(uintptr_t)w32_heap_alloc(args[0] ? args[0] : 1, 0); }
static u64 w32_crt_calloc(const u32 *args)
{
    const u64 total = (u64)args[0] * args[1];
    if (total > 0x7fff0000u) return 0;
    return (u32)(uintptr_t)w32_heap_alloc(total ? (size_t)total : 1, 1);
}
static u64 w32_crt_free(const u32 *args)    { if (args[0]) w32_heap_free(W32_PTR(0)); return 0; }
static u64 w32_crt_realloc(const u32 *args)
{
    if (!args[0]) return (u32)(uintptr_t)w32_heap_alloc(args[1] ? args[1] : 1, 0);
    if (!args[1]) { w32_heap_free(W32_PTR(0)); return 0; }
    return (u32)(uintptr_t)w32_heap_realloc(W32_PTR(0), args[1], 0);
}
static u64 w32_crt_memcpy(const u32 *args)  { memcpy(W32_PTR(0), W32_PTR(1), args[2]); return args[0]; }
static u64 w32_crt_memmove(const u32 *args) { memmove(W32_PTR(0), W32_PTR(1), args[2]); return args[0]; }
static u64 w32_crt_memcmp(const u32 *args)  { return (u32)memcmp(W32_PTR(0), W32_PTR(1), args[2]); }
static u64 w32_crt_strncmp(const u32 *args) { return (u32)strncmp(W32_PTR(0), W32_PTR(1), args[2]); }
static u64 w32_crt_stricmp(const u32 *args) { return (u32)strcasecmp(W32_PTR(0), W32_PTR(1)); }
static u64 w32_crt_toupper(const u32 *args) { return (u32)toupper((int)args[0]); }
static u64 w32_crt_tolower(const u32 *args) { return (u32)tolower((int)args[0]); }
static u64 w32_crt_abort(const u32 *args)
{
    (void)args;
    fprintf(stderr, "[WOW32] abort() called by the guest\n");
    impl_ExitProcess(3);
    _exit(3);
}
static u64 w32_crt_errno(const u32 *args)
{
    (void)args;
    static u32 location;
    if (!location) location = (u32)(uintptr_t)w32_heap_alloc(4, 1);
    return location;
}
static u64 w32_crt_fflush(const u32 *args)
{
    FILE *file = args[0] ? w32_host_file(args[0]) : NULL;
    if (args[0] && !file) return (u32)-1;
    return (u32)fflush(file);
}
static u64 w32_crt_vfprintf(const u32 *args)               /* (stream, format, va_list) */
{
    FILE *file = w32_host_file(args[0]);
    if (!file) return (u32)-1;
    char text[8192];
    const size_t length = w32_format(text, sizeof(text), W32_PTR(1), (const u32 *)W32_PTR(2));
    return (u32)fwrite(text, 1, length, file);
}
static u64 w32_crt_sprintf(const u32 *args)
{
    char *out = W32_PTR(0);
    char text[8192];
    const size_t length = w32_format(text, sizeof(text), W32_PTR(1), args + 2);
    memcpy(out, text, length + 1);
    return (u32)length;
}
static u64 w32_crt_snprintf(const u32 *args)               /* (buffer, count, format, ...) */
{
    char *out = W32_PTR(0);
    char text[8192];
    const size_t length = w32_format(text, sizeof(text), W32_PTR(2), args + 3);
    if (args[1]) { const size_t n = length < args[1] - 1 ? length : args[1] - 1; memcpy(out, text, n); out[n] = 0; }
    return (u32)length;
}
static u64 w32_crt_wcslen(const u32 *args)  { return (u32)beer_ctl_wlen(W32_PTR(0)); }
static u64 w32_crt_wcscpy(const u32 *args)
{
    u16 *d = W32_PTR(0); const u16 *s = W32_PTR(1);
    size_t i = 0;
    do d[i] = s[i]; while (s[i++]);
    return args[0];
}
static u64 w32_crt_wcscat(const u32 *args)
{
    u16 *d = W32_PTR(0); const u16 *s = W32_PTR(1);
    d += beer_ctl_wlen(d);
    size_t i = 0;
    do d[i] = s[i]; while (s[i++]);
    return args[0];
}
static u64 w32_crt_wcscmp(const u32 *args)
{
    const u16 *a = W32_PTR(0), *b = W32_PTR(1);
    while (*a && *a == *b) { ++a; ++b; }
    return (u32)((int)*a - (int)*b);
}
static u64 w32_crt_wcsstr(const u32 *args)
{
    const u16 *h = W32_PTR(0), *n = W32_PTR(1);
    const size_t nl = beer_ctl_wlen(n);
    if (!nl) return args[0];
    for (; *h; ++h) {
        size_t i = 0;
        while (i < nl && h[i] == n[i]) ++i;
        if (i == nl) return (u32)(uintptr_t)h;
    }
    return 0;
}
/* __dllonexit(func, &begin, &end): registers an atexit-style function in the DLL's own table; they run at process
 * exit in Windows, which Beer does not emulate for DLLs, so the function is only recorded as accepted. */
static u64 w32_crt_dllonexit(const u32 *args) { return args[0]; }

/* ---- entries found in the DLLs a Delphi/Inno installer extracts (BASS, botva2, cls-*, MpzSlimmer) ---- */
static u64 w32_wsprintfA(const u32 *args)                   /* user32 wsprintfA: cdecl, varargs */
{
    char text[1024];
    const size_t length = w32_format(text, sizeof(text), W32_PTR(1), args + 2);
    memcpy(W32_PTR(0), text, length + 1);
    return (u32)length;
}
static u64 w32_IsBadReadPtr(const u32 *args)               /* readable = covered by a mapping with 'r' */
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
        if (covered >= lo && covered < hi) { if (perms[0] != 'r') break; covered = hi; }
    }
    fclose(maps);
    return covered < end;
}
static u64 w32_IsBadCodePtr(const u32 *args)
{
    const u32 probe[2] = { args[0], 1 };
    return w32_IsBadReadPtr(probe);
}
/* GetModuleHandleEx: flags 1 = pin, 2 = unchanged refcount, 4 = the argument is an address inside the module. */
/* GetModuleHandleEx(flags, name_or_address, module*). The name is passed as a host pointer: the W variant narrows it
 * into a host buffer, whose 64-bit address cannot travel through a 32-bit argument slot (it was truncated and the
 * lookup read garbage: the MSVC CRT's exit path in Pratfall's Launcher.exe faulted in w32_module_key). */
static u64 w32_module_handle_ex(u32 flags, u32 address, const char *name, u32 out)
{
    if (!out) { g_last_error = 87; return 0; }
    u32 module = 0;
    if (flags & 4) {                                         /* GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS */
        u32 base, size;
        const u8 *exe = (const u8 *)(uintptr_t)g_w32_image;
        const W32NtHdrs *nt = (const W32NtHdrs *)(exe + ((const DosHdr *)exe)->lfanew);
        if (address >= g_w32_image && address - g_w32_image < nt->opt.size_of_image) module = g_w32_image;
        else if (w32_dll_image_range(address, &base, &size)) module = base;
    } else if (!name) module = g_w32_image;
    else module = w32_module_by_name(name);
    if (!module) { g_last_error = 126; return 0; }
    *(u32 *)(uintptr_t)out = module;
    return 1;
}
static u64 w32_GetModuleHandleExA(const u32 *args)
{
    return w32_module_handle_ex(args[0], args[1], (args[0] & 4) || !args[1] ? NULL : (const char *)W32_PTR(1), args[2]);
}
static u64 w32_GetModuleHandleExW(const u32 *args)
{
    if ((args[0] & 4) || !args[1]) return w32_module_handle_ex(args[0], args[1], NULL, args[2]);
    char name[PATH_MAX];
    w32_narrow(W32_PTR(1), name, sizeof(name));
    return w32_module_handle_ex(args[0], args[1], name, args[2]);
}
static u64 w32_SetWindowTextA(const u32 *args) { return w32_via_wide("SetWindowTextW", args, 2, 2); }
static u64 w32_OpenEventA(const u32 *args) { (void)args; g_last_error = 2; return 0; }
static u64 w32_TrackMouseEvent(const u32 *args) { (void)args; return 1; }   /* no mouse tracking: always succeeds */
static u64 w32_mmsys_nodriver(const u32 *args) { (void)args; return 6; }    /* MMSYSERR_NODRIVER */
static u64 w32_AreFileApisANSI(const u32 *args) { (void)args; return 1; }
static u64 w32_GetDiskFreeSpaceExA(const u32 *args)
{
    u16 *wide = w32_widen_ansi(args[0], 0xffffffffu);
    u32 call[4] = { (u32)(uintptr_t)wide, args[1], args[2], args[3] };
    const u64 r = w32_GetDiskFreeSpaceExW(call);
    if (wide) w32_heap_free(wide);
    return r;
}
/* Affinity is not applied (the host scheduler decides); the previous mask reported is every online CPU. */
static u64 w32_SetThreadAffinityMask(const u32 *args)
{
    (void)args;
    const long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    return cpus >= 32 ? 0xffffffffu : (u32)((1ull << cpus) - 1);
}
static u64 w32_FreeLibraryAndExitThread(const u32 *args)    /* (module, exit code) */
{
    const u32 free_args[1] = { args[0] };
    w32_FreeLibrary(free_args);
    impl_ExitThread(args[1]);
    return 0;
}

/* ---- small kernel32 helpers with no native counterpart ---- */
static u64 w32_RtlMoveMemory(const u32 *args) { memmove(W32_PTR(0), W32_PTR(1), args[2]); return 0; }
static u64 w32_RtlZeroMemory(const u32 *args) { memset(W32_PTR(0), 0, args[1]); return 0; }
static u64 w32_RtlFillMemory(const u32 *args) { memset(W32_PTR(0), (int)(args[2] & 0xff), args[1]); return 0; }   /* (dest, length, fill) */
static u64 w32_IsDBCSLeadByte(const u32 *args) { (void)args; return 0; }        /* single-byte code page */
static u64 w32_IsDBCSLeadByteEx(const u32 *args) { (void)args; return 0; }
static u64 w32_HeapOneArgOk(const u32 *args) { return args[0] != 0; }            /* HeapLock/Unlock/Validate: always fine */
static u64 w32_HeapCompact(const u32 *args) { (void)args; return 0; }
static u64 w32_ConsoleCtrlHandler(const u32 *args) { (void)args; return 1; }     /* no console events reach the guest */
static u64 w32_PathPassThroughA(const u32 *args)                                /* GetShortPathNameA/GetLongPathNameA */
{
    const char *in = W32_PTR(0);
    char *out = W32_PTR(1);
    if (!in) { g_last_error = 87; return 0; }
    const u32 need = (u32)strlen(in) + 1;
    if (!out || args[2] < need) return need;
    memcpy(out, in, need);
    return need - 1;
}

/* ---- winmm / msacm32: Beer has no multimedia devices; wave/mixer calls report that there is no driver ---- */
static u64 w32_acm_notpossible(const u32 *args) { (void)args; return 512; }    /* ACMERR_NOTPOSSIBLE */
static u64 w32_timeGetDevCaps(const u32 *args)                                 /* TIMECAPS { min, max } */
{
    u32 *caps = W32_PTR(0);
    if (!caps || args[1] < 8) return 97;                                       /* TIMERR_STRUCT */
    caps[0] = 1; caps[1] = 1000000;
    return 0;
}

/* ---- C runtime pieces a packed MSVC DLL (BASS) imports ----
 * x87 conventions: a double result is returned in ST0, and the _CIxxx helpers / _ftol take their operands in ST0
 * (and ST1). The guest's x87 stack is the same register file the host code runs on, so a handler pops its operands
 * with FSTP and pushes a result with FLD; the thunk return path then leaves ST0 exactly as 32-bit code expects. */
static inline void w32_fpu_pop(double *out) { __asm__ volatile("fstpl %0" : "=m"(*out)); }
static inline void w32_fpu_push(const double *value) { __asm__ volatile("fldl %0" : : "m"(*value)); }
static void w32_math_apply(int op, double a, double b)       /* computes op(a, b) and pushes it onto the x87 stack */
{
    const double in[2] = { a, b };
    double out;
    beer_w32_math(op, in, &out);
    w32_fpu_push(&out);
}
#define W32_ARG_D(args, i) ({ double d_; memcpy(&d_, (args) + 2 * (i), 8); d_; })
#define W32_MATH1(name, op) \
    static u64 w32_crt_##name(const u32 *args) { w32_math_apply(op, W32_ARG_D(args, 0), 0); return 0; } \
    static u64 w32_crt_CI##name(const u32 *args) { (void)args; double x; w32_fpu_pop(&x); w32_math_apply(op, x, 0); return 0; }
W32_MATH1(floor, W32_MATH_FLOOR) W32_MATH1(ceil, W32_MATH_CEIL) W32_MATH1(sqrt, W32_MATH_SQRT)
W32_MATH1(fabs, W32_MATH_FABS) W32_MATH1(sin, W32_MATH_SIN) W32_MATH1(cos, W32_MATH_COS) W32_MATH1(tan, W32_MATH_TAN)
W32_MATH1(atan, W32_MATH_ATAN) W32_MATH1(exp, W32_MATH_EXP) W32_MATH1(log, W32_MATH_LOG) W32_MATH1(log10, W32_MATH_LOG10)
static u64 w32_crt_pow(const u32 *args)   { w32_math_apply(W32_MATH_POW, W32_ARG_D(args, 0), W32_ARG_D(args, 1)); return 0; }
static u64 w32_crt_CIpow(const u32 *args) { (void)args; double e, b; w32_fpu_pop(&e); w32_fpu_pop(&b); w32_math_apply(W32_MATH_POW, b, e); return 0; }
static u64 w32_crt_atan2(const u32 *args) { w32_math_apply(W32_MATH_ATAN2, W32_ARG_D(args, 0), W32_ARG_D(args, 1)); return 0; }
static u64 w32_crt_CIatan2(const u32 *args) { (void)args; double x, y; w32_fpu_pop(&x); w32_fpu_pop(&y); w32_math_apply(W32_MATH_ATAN2, y, x); return 0; }
static u64 w32_crt_fmod(const u32 *args)  { w32_math_apply(W32_MATH_FMOD, W32_ARG_D(args, 0), W32_ARG_D(args, 1)); return 0; }
static u64 w32_crt_ldexp(const u32 *args) { w32_math_apply(W32_MATH_LDEXP, W32_ARG_D(args, 0), (double)(int)args[2]); return 0; }
static u64 w32_crt_ftol(const u32 *args)  { (void)args; double x; w32_fpu_pop(&x); return (u64)(int64_t)x; }    /* truncates toward zero */
static u64 w32_crt_strdup(const u32 *args)
{
    const char *s = W32_PTR(0);
    char *copy = w32_heap_alloc(strlen(s) + 1, 0);
    if (copy) strcpy(copy, s);
    return (u32)(uintptr_t)copy;
}
static u64 w32_crt_wcsdup(const u32 *args)
{
    const u16 *s = W32_PTR(0);
    const size_t n = beer_ctl_wlen(s);
    u16 *copy = w32_heap_alloc((n + 1) * 2, 0);
    if (copy) memcpy(copy, s, (n + 1) * 2);
    return (u32)(uintptr_t)copy;
}
static u64 w32_crt_strtoul(const u32 *args)                /* (string, end**, base) */
{
    char *end = NULL;
    const unsigned long v = strtoul(W32_PTR(0), &end, (int)args[2]);
    if (args[1]) *(u32 *)W32_PTR(1) = (u32)(uintptr_t)end;
    return (u32)v;
}
static u64 w32_crt_strnicmp(const u32 *args) { return (u32)strncasecmp(W32_PTR(0), W32_PTR(1), args[2]); }
static u64 w32_crt_memchr(const u32 *args)  { return (u32)(uintptr_t)memchr(W32_PTR(0), (int)args[1], args[2]); }
/* qsort with a guest comparison function (cdecl: (const void *, const void *)). */
typedef struct { u32 compare; } W32SortContext;
static int w32_crt_qsort_compare(const void *a, const void *b, void *context)
{
    const W32SortContext *ctx = context;
    const u32 argv[2] = { (u32)(uintptr_t)a, (u32)(uintptr_t)b };
    return (int)w32_call(ctx->compare, 2, argv);
}
static u64 w32_crt_qsort(const u32 *args)                  /* (base, count, size, compare) */
{
    W32SortContext ctx = { args[3] };
    qsort_r(W32_PTR(0), args[1], args[2], w32_crt_qsort_compare, &ctx);
    return 0;
}
static u64 w32_crt_initterm(const u32 *args)               /* (begin, end): runs the function pointers in between */
{
    for (const u32 *p = W32_PTR(0); p < (const u32 *)W32_PTR(1); ++p)
        if (*p) w32_call(*p, 0, NULL);
    return 0;
}
static u64 w32_crt_new(const u32 *args)    { return (u32)(uintptr_t)w32_heap_alloc(args[0] ? args[0] : 1, 0); }   /* operator new */
static u64 w32_crt_delete(const u32 *args) { if (args[0]) w32_heap_free(W32_PTR(0)); return 0; }                    /* operator delete */
static u64 w32_wininet_connected(const u32 *args) { (void)args; return 0; }                                        /* InternetGetConnectedState */

/* LoadStringW from the module's own RT_STRING resources (the shared implementation only knows the 64-bit image). */
static u64 w32_LoadStringW(const u32 *args)                 /* (module, id, buffer, size) */
{
    u16 *buffer = W32_PTR(2);
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
    if (!args[3]) {                                          /* size 0: the buffer receives a pointer to the string */
        if (buffer) *(u32 *)buffer = (u32)(uintptr_t)(block + at + 1);
        return length;
    }
    if (!buffer) { g_last_error = 87; return 0; }
    if (length > args[3] - 1) length = args[3] - 1;
    memcpy(buffer, block + at + 1, (size_t)length * 2);
    buffer[length] = 0;
    if (getenv("BEER_W32_TRACE")) {
        char text[256];
        w32_narrow(buffer, text, sizeof(text));
        fprintf(stderr, "[WOW32] LoadStringW(%u) = \"%s\"\n", args[1], text);
    }
    return length;
}
/* Beer does not redirect file-system paths per bitness, so disabling redirection is trivially successful. */
static u64 w32_Wow64DisableFsRedirection(const u32 *args) { if (args[0]) *(u32 *)W32_PTR(0) = 0; return 1; }
static u64 w32_one(const u32 *args) { (void)args; return 1; }
/* The WoW64 system directory (the prefix has drive_c/Windows/SysWOW64). */
static u64 w32_wow64_dir(const u32 *args, int wide)         /* (buffer, size) */
{
    static const char path[] = "C:\\Windows\\SysWOW64";
    const u32 need = (u32)sizeof(path);
    if (!args[0] || args[1] < need) return need;
    for (u32 i = 0; i < need; ++i) {
        if (wide) ((u16 *)W32_PTR(0))[i] = (unsigned char)path[i]; else ((char *)W32_PTR(0))[i] = path[i];
    }
    return need - 1;
}
static u64 w32_GetSystemWow64DirectoryA(const u32 *args) { return w32_wow64_dir(args, 0); }
static u64 w32_GetSystemWow64DirectoryW2(const u32 *args) { return w32_wow64_dir(args, 1); }
/* GetModuleFileNameA through the wide implementation (DLL modules, system modules and the executable all answer there). */
static u64 w32_GetModuleFileNameA(const u32 *args)          /* (module, buffer, size) */
{
    if (!args[1] || !args[2]) { g_last_error = 122; return 0; }
    u16 *wide = w32_heap_alloc((size_t)args[2] * 2 + 2, 1);
    if (!wide) { g_last_error = 8; return 0; }
    const u32 call[3] = { args[0], (u32)(uintptr_t)wide, args[2] };
    const u32 n = (u32)w32_GetModuleFileNameW(call);
    char *out = W32_PTR(1);
    const u32 count = n < args[2] ? n : args[2] - 1;
    for (u32 i = 0; i < count; ++i) out[i] = wide[i] < 0x100 ? (char)wide[i] : '?';
    out[count] = 0;
    w32_heap_free(wide);
    return n;
}

/* ---- msvcrt stdio / low-level I/O / time for MinGW and MSVC DLLs such as unarc.dll (cdecl) ---- */
static u64 w32_crt_atof(const u32 *args) { double d; beer_w32_strtod(W32_PTR(0), &d, NULL); w32_fpu_push(&d); return 0; }
static u64 w32_crt_clock(const u32 *args) { (void)args; return (u32)(clock() * 1000 / CLOCKS_PER_SEC); }
static u64 w32_crt_exit(const u32 *args) { impl_ExitProcess(args[0]); _exit((int)args[0]); }
static u64 w32_crt_assert(const u32 *args)                  /* _assert(expression, file, line) */
{
    fprintf(stderr, "[WOW32] assertion failed: %s (%s:%u)\n", (const char *)W32_PTR(0), (const char *)W32_PTR(1), args[2]);
    impl_ExitProcess(3);
    _exit(3);
}
static u64 w32_crt_clearerr(const u32 *args) { FILE *f = w32_host_file(args[0]); if (f) clearerr(f); return 0; }
static u64 w32_crt_fputc(const u32 *args)  { FILE *f = w32_host_file(args[1]); return f ? (u32)fputc((int)args[0], f) : (u32)-1; }
static u64 w32_crt_flsbuf(const u32 *args) { return w32_crt_fputc(args); }
static u64 w32_crt_fread(const u32 *args)                   /* (buffer, size, count, stream) */
{
    FILE *f = w32_host_file(args[3]);
    return f ? (u32)fread(W32_PTR(0), args[1], args[2], f) : 0;
}
static u64 w32_crt_fseek(const u32 *args)                   /* (stream, offset, origin) */
{
    FILE *f = w32_host_file(args[0]);
    return f ? (u32)fseek(f, (s32)args[1], (int)args[2]) : (u32)-1;
}
static u64 w32_crt_ftell(const u32 *args)  { FILE *f = w32_host_file(args[0]); return f ? (u32)ftell(f) : (u32)-1; }
static u64 w32_crt_rewind(const u32 *args) { FILE *f = w32_host_file(args[0]); if (f) rewind(f); return 0; }
static u64 w32_crt_fdopen(const u32 *args)                  /* (fd, mode): a pool slot over the host fd */
{
    FILE *file = fdopen(w32_fd(args[0]), W32_PTR(1));
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
static u64 w32_crt_remove(const u32 *args)
{
    char host[PATH_MAX];
    if (!resolve_windows_host_path(W32_PTR(0), host, sizeof(host))) { errno = ENOENT; return (u32)-1; }
    return (u32)remove(host);
}
static int w32_wide_host_path(const u16 *wide, char *host, size_t size)
{
    char narrow[PATH_MAX];
    w32_narrow(wide, narrow, sizeof(narrow));
    return resolve_windows_host_path(narrow, host, size);
}
static u64 w32_crt_wremove(const u32 *args) { char h[PATH_MAX]; if (!w32_wide_host_path(W32_PTR(0), h, sizeof(h))) return (u32)-1; return (u32)remove(h); }
static u64 w32_crt_wrmdir(const u32 *args)  { char h[PATH_MAX]; if (!w32_wide_host_path(W32_PTR(0), h, sizeof(h))) return (u32)-1; return (u32)rmdir(h); }
static u64 w32_crt_wmkdir(const u32 *args)  { char h[PATH_MAX]; if (!w32_wide_host_path(W32_PTR(0), h, sizeof(h))) return (u32)-1; return (u32)mkdir(h, 0777); }
static u64 w32_crt_wrename(const u32 *args)
{
    char from[PATH_MAX], to[PATH_MAX];
    if (!w32_wide_host_path(W32_PTR(0), from, sizeof(from)) || !w32_wide_host_path(W32_PTR(1), to, sizeof(to))) return (u32)-1;
    return (u32)rename(from, to);
}
static u64 w32_crt_access(const u32 *args)
{
    char host[PATH_MAX];
    if (!resolve_windows_host_path(W32_PTR(0), host, sizeof(host))) { errno = ENOENT; return (u32)-1; }
    return (u32)access(host, (args[1] & 2 ? W_OK : 0) | (args[1] & 4 ? R_OK : 0) | (args[1] ? 0 : F_OK));
}
static u64 w32_crt_waccess(const u32 *args)
{
    char host[PATH_MAX];
    if (!w32_wide_host_path(W32_PTR(0), host, sizeof(host))) { errno = ENOENT; return (u32)-1; }
    return (u32)access(host, (args[1] & 2 ? W_OK : 0) | (args[1] & 4 ? R_OK : 0) | (args[1] ? 0 : F_OK));
}
static u64 w32_crt_wopen(const u32 *args)                   /* (path, flags, mode) */
{
    char host[PATH_MAX];
    if (!w32_wide_host_path(W32_PTR(0), host, sizeof(host))) { errno = ENOENT; return (u32)-1; }
    const u32 mode = args[1];
    int flags = (int)(mode & 3u);
    if (mode & 0x08u) flags |= O_APPEND;
    if (mode & 0x100u) flags |= O_CREAT;
    if (mode & 0x200u) flags |= O_TRUNC;
    if (mode & 0x400u) flags |= O_EXCL;
    return (u32)open(host, flags | O_CLOEXEC, 0666);
}
/* _write writes everything (a blocking pipe takes the whole buffer, as WriteFile does); a host write can stop early
 * when a signal arrives, and unarc treats a short count as a broken pipe. */
static u64 w32_crt_write(const u32 *args)
{
    const int fd = w32_fd(args[0]);
    const u8 *data = W32_PTR(1);
    u32 done = 0;
    while (done < args[2]) {
        const ssize_t n = write(fd, data + done, args[2] - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return done ? done : (u32)-1;
        done += (u32)n;
    }
    return done;
}
static u64 w32_crt_dup(const u32 *args)
{
    const int fd = w32_fd(args[0]);
    return fd < 0 ? (u32)-1 : (u32)fcntl(fd, F_DUPFD_CLOEXEC, 3);
}
static u64 w32_crt_dup2(const u32 *args)                    /* MSVCRT: 0 on success */
{
    const int from = w32_fd(args[0]);
    const int to = (int)args[1];
    if (from < 0 || to < 0) { errno = EBADF; return (u32)-1; }
    if (to <= 2) {
        const int copy = fcntl(from, F_DUPFD_CLOEXEC, 3);
        if (copy < 0) return (u32)-1;
        pthread_mutex_lock(&g_w32_std_lock);
        const int old = g_w32_std[to];
        g_w32_std[to] = copy;
        pthread_mutex_unlock(&g_w32_std_lock);
        if (old >= 0) close(old);
        return 0;
    }
    return dup3(from, to, O_CLOEXEC) < 0 ? (u32)-1 : 0;   /* children get std streams explicitly */
}
static u64 w32_crt_pipe(const u32 *args)                    /* (int fds[2], size, textmode) */
{
    int fds[2];
    if (pipe2(fds, O_CLOEXEC)) return (u32)-1;           /* never leaked into a child: it would hold its own pipe open */
    ((s32 *)W32_PTR(0))[0] = fds[0]; ((s32 *)W32_PTR(0))[1] = fds[1];
    return 0;
}
static u64 w32_crt_filelengthi64(const u32 *args)
{
    struct stat st;
    return fstat(w32_fd(args[0]), &st) ? (u64)(s64)-1 : (u64)st.st_size;
}
static u64 w32_crt_lseeki64(const u32 *args)                /* (fd, int64 offset, whence) -> int64 in EDX:EAX */
{
    const s64 offset = (s64)(((u64)args[2] << 32) | args[1]);
    return (u64)lseek(w32_fd(args[0]), offset, (int)args[3]);
}
/* A Win32 HANDLE for a CRT descriptor: the handle table is separate from host descriptors, so none is available. */
static u64 w32_crt_get_osfhandle(const u32 *args) { (void)args; errno = EBADF; return (u32)-1; }
static u64 w32_crt_raise(const u32 *args) { (void)args; return 0; }
static u64 w32_crt_strerror(const u32 *args) { static u32 buffer; if (!buffer) buffer = (u32)(uintptr_t)w32_heap_alloc(128, 1); snprintf((char *)(uintptr_t)buffer, 128, "%s", strerror((int)args[0])); return buffer; }
static u64 w32_crt_vsnprintf(const u32 *args)               /* (buffer, count, format, va_list) */
{
    char text[8192];
    const size_t length = w32_format(text, sizeof(text), W32_PTR(2), (const u32 *)W32_PTR(3));
    char *out = W32_PTR(0);
    if (args[1]) { const size_t n = length < args[1] - 1 ? length : args[1] - 1; memcpy(out, text, n); out[n] = 0; }
    return (u32)length;
}
static u64 w32_crt_lrotr(const u32 *args) { const u32 n = args[1] & 31; return n ? (args[0] >> n) | (args[0] << (32 - n)) : args[0]; }
static u64 w32_crt_wcschr(const u32 *args)
{
    for (const u16 *s = W32_PTR(0);; ++s) { if (*s == (u16)args[1]) return (u32)(uintptr_t)s; if (!*s) return 0; }
}
static u64 w32_crt_wcsrchr(const u32 *args)
{
    const u16 *last = NULL;
    for (const u16 *s = W32_PTR(0);; ++s) { if (*s == (u16)args[1]) last = s; if (!*s) break; }
    return (u32)(uintptr_t)last;
}
static u64 w32_crt_wcslwr(const u32 *args)
{
    for (u16 *s = W32_PTR(0); *s; ++s) if (*s >= 'A' && *s <= 'Z') *s += 32;
    return args[0];
}
/* struct tm for 32-bit code is nine ints; one static record, as the CRT's gmtime/localtime return. */
static u32 w32_tm_fill(const struct tm *t)
{
    static u32 record;
    if (!record) record = (u32)(uintptr_t)w32_heap_alloc(36, 1);
    s32 *r = (s32 *)(uintptr_t)record;
    r[0] = t->tm_sec; r[1] = t->tm_min; r[2] = t->tm_hour; r[3] = t->tm_mday; r[4] = t->tm_mon;
    r[5] = t->tm_year; r[6] = t->tm_wday; r[7] = t->tm_yday; r[8] = t->tm_isdst;
    return record;
}
static u64 w32_crt_gmtime(const u32 *args)    { const time_t t = (s32)((const u32 *)W32_PTR(0))[0]; struct tm r; if (!gmtime_r(&t, &r)) return 0; return w32_tm_fill(&r); }
static u64 w32_crt_localtime(const u32 *args) { const time_t t = (s32)((const u32 *)W32_PTR(0))[0]; struct tm r; if (!localtime_r(&t, &r)) return 0; return w32_tm_fill(&r); }
static u64 w32_crt_strftime(const u32 *args)                /* (buffer, size, format, struct tm*) */
{
    const s32 *r = W32_PTR(3);
    struct tm t = {0};
    t.tm_sec = r[0]; t.tm_min = r[1]; t.tm_hour = r[2]; t.tm_mday = r[3]; t.tm_mon = r[4]; t.tm_year = r[5];
    t.tm_wday = r[6]; t.tm_yday = r[7]; t.tm_isdst = r[8];
    return (u32)strftime(W32_PTR(0), args[1], W32_PTR(2), &t);
}
/* sscanf for the conversions programs use on config text: %d %i %u %x %s %c %% (plus an optional width and %n). */
static u64 w32_crt_sscanf(const u32 *args)
{
    const char *in = W32_PTR(0), *fmt = W32_PTR(1);
    const u32 *out = args + 2;
    int assigned = 0;
    for (; *fmt; ++fmt) {
        if (isspace((unsigned char)*fmt)) { while (isspace((unsigned char)*in)) ++in; continue; }
        if (*fmt != '%') { if (*in != *fmt) break; ++in; continue; }
        ++fmt;
        int width = 0, skip = 0;
        if (*fmt == '*') { skip = 1; ++fmt; }
        while (isdigit((unsigned char)*fmt)) width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l' || *fmt == 'h') ++fmt;
        const char c = *fmt;
        if (c == '%') { if (*in != '%') break; ++in; continue; }
        if (c == 'n') { if (!skip) *(s32 *)(uintptr_t)*out++ = (s32)0; continue; }
        if (c != 'c') while (isspace((unsigned char)*in)) ++in;
        if (!*in) break;
        if (c == 'd' || c == 'i' || c == 'u' || c == 'x' || c == 'X') {
            char *end;
            const long v = c == 'u' ? (long)strtoul(in, &end, 10) : strtol(in, &end, c == 'x' || c == 'X' ? 16 : (c == 'i' ? 0 : 10));
            if (end == in) break;
            in = end;
            if (!skip) *(s32 *)(uintptr_t)*out++ = (s32)v;
            if (!skip) ++assigned;
        } else if (c == 's') {
            char *dst = skip ? NULL : (char *)(uintptr_t)*out++;
            size_t n = 0;
            while (*in && !isspace((unsigned char)*in) && (!width || (int)n < width)) { if (dst) dst[n] = *in; ++in; ++n; }
            if (dst) { dst[n] = 0; ++assigned; }
        } else if (c == 'c') {
            if (!skip) { *(char *)(uintptr_t)*out++ = *in; ++assigned; }
            ++in;
        } else break;
    }
    return (u32)assigned;
}
/* _beginthreadex(security, stack, start, arg, flags, id*): the start routine is stdcall like CreateThread's. */
static u64 w32_crt_beginthreadex(const u32 *args)
{
    const u32 call[6] = { args[0], args[1], args[2], args[3], args[4], args[5] };
    return w32_CreateThread(call);
}
/* ctype tables: _pctype is a pointer to 256 classification words, __mb_cur_max a one-byte-code-page flag. */
static u64 w32_crt_isctype(const u32 *args)                 /* (char, mask) with the CRT's _ALPHA/_DIGIT/... bits */
{
    const int c = (int)args[0];
    u32 have = 0;
    if (c >= 0 && c < 256) {
        if (isupper(c)) have |= 0x1; if (islower(c)) have |= 0x2; if (isdigit(c)) have |= 0x4;
        if (isspace(c)) have |= 0x8; if (ispunct(c)) have |= 0x10; if (iscntrl(c)) have |= 0x20;
        if (c == ' ') have |= 0x40; if (isxdigit(c)) have |= 0x80; if (isalpha(c)) have |= 0x100;
    }
    return (have & args[1]) != 0;
}
static u32 w32_crt_pctype_table(void)
{
    static u32 table;
    if (!table) {
        u16 *t = w32_heap_alloc(257 * 2, 1);
        for (int c = 0; c < 256; ++c) {
            u16 v = 0;
            if (isupper(c)) v |= 0x1; if (islower(c)) v |= 0x2; if (isdigit(c)) v |= 0x4; if (isspace(c)) v |= 0x8;
            if (ispunct(c)) v |= 0x10; if (iscntrl(c)) v |= 0x20; if (c == ' ') v |= 0x40; if (isxdigit(c)) v |= 0x80;
            if (isalpha(c)) v |= 0x100;
            t[c + 1] = v;
        }
        table = (u32)(uintptr_t)(t + 1);
    }
    return table;
}
/* Atoms (kernel32): a process-wide table of names. */
static char *g_w32_atoms[256];
static u64 w32_atom_add(const char *name, int add)
{
    if (!name) { g_last_error = 87; return 0; }
    pthread_mutex_lock(&g_w32_lock);
    for (int i = 0; i < 256; ++i) if (g_w32_atoms[i] && !strcasecmp(g_w32_atoms[i], name)) { pthread_mutex_unlock(&g_w32_lock); return 0xC000u + i; }
    if (add) for (int i = 0; i < 256; ++i) if (!g_w32_atoms[i]) { g_w32_atoms[i] = strdup(name); pthread_mutex_unlock(&g_w32_lock); return 0xC000u + i; }
    pthread_mutex_unlock(&g_w32_lock);
    g_last_error = add ? 8 : 2;
    return 0;
}
static u64 w32_AddAtomA(const u32 *args)  { return w32_atom_add(W32_PTR(0), 1); }
static u64 w32_FindAtomA(const u32 *args) { return w32_atom_add(W32_PTR(0), 0); }
static u64 w32_GetAtomNameA(const u32 *args)                /* (atom, buffer, size) */
{
    const u32 i = args[0] - 0xC000u;
    if (i >= 256 || !g_w32_atoms[i]) { g_last_error = 6; return 0; }
    const u32 n = (u32)strlen(g_w32_atoms[i]);
    if (!args[1] || !args[2]) return 0;
    const u32 count = n < args[2] - 1 ? n : args[2] - 1;
    memcpy(W32_PTR(1), g_w32_atoms[i], count);
    ((char *)W32_PTR(1))[count] = 0;
    return count;
}
static u64 w32_ConsoleTitle(const u32 *args) { (void)args; return 0; }        /* no console: empty title */
static u64 w32_nullptr(const u32 *args) { (void)args; return 0; }

/* ---- setjmp / longjmp with msvcrt's _JUMP_BUFFER (Ebp, Ebx, Edi, Esi, Esp, Eip, ...) ----
 * The thunk return path pops the return address from the guest ESP slot and reloads the saved callee-saved registers,
 * so longjmp only has to rewrite those slots: it plants the saved Eip under the saved Esp and "returns" through it. */
static u64 w32_crt_setjmp3(const u32 *args)
{
    u64 teb64;
    __asm__ volatile("mov %%gs:0x30, %0" : "=r"(teb64));
    u32 *buf = W32_PTR(0);
    const u32 esp = (u32)*(u64 *)(uintptr_t)(teb64 + W32_SLOT_ESP);        /* points at the return address */
    memset(buf, 0, 64);
    buf[0] = (u32)*(u64 *)(uintptr_t)(teb64 + W32_SLOT_EBP);
    buf[1] = (u32)*(u64 *)(uintptr_t)(teb64 + W32_SLOT_EBX);
    buf[2] = (u32)*(u64 *)(uintptr_t)(teb64 + W32_SLOT_EDI);
    buf[3] = (u32)*(u64 *)(uintptr_t)(teb64 + W32_SLOT_ESI);
    buf[4] = esp + 4;                                                      /* ESP once setjmp has returned */
    buf[5] = *(const u32 *)(uintptr_t)esp;                                 /* where setjmp returns to */
    return 0;
}
static u64 w32_crt_longjmp(const u32 *args)
{
    u64 teb64;
    __asm__ volatile("mov %%gs:0x30, %0" : "=r"(teb64));
    const u32 *buf = W32_PTR(0);
    const u32 value = args[1] ? args[1] : 1;
    *(u64 *)(uintptr_t)(teb64 + W32_SLOT_EBP) = buf[0];
    *(u64 *)(uintptr_t)(teb64 + W32_SLOT_EBX) = buf[1];
    *(u64 *)(uintptr_t)(teb64 + W32_SLOT_EDI) = buf[2];
    *(u64 *)(uintptr_t)(teb64 + W32_SLOT_ESI) = buf[3];
    *(u32 *)(uintptr_t)(buf[4] - 4) = buf[5];                              /* return address under the saved ESP */
    *(u64 *)(uintptr_t)(teb64 + W32_SLOT_ESP) = buf[4] - 4;
    *(u64 *)(uintptr_t)(teb64 + W32_SLOT_CLEANUP) = 0;                     /* cdecl: nothing to pop */
    return value;
}
