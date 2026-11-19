/* PE32 DLLs are deliberately separate from the native PE32+ module registry.
 * First supported subset: i386, HIGHLOW relocations, system imports with known
 * ABI signatures, named/ordinal direct exports, synchronous DllMain. Static TLS,
 * delay imports, forwarded exports and guest-DLL dependencies fail explicitly.
 * No unknown zero-return import thunks are allowed in newly loaded DLLs. */
#define W32_DLL_MAX 64
typedef struct {
    u8 *image;
    u32 size, entry, refs;
    int state; /* 1 ready, 2 attaching, 3 detaching */
    char path[PATH_MAX];
    DataDir exports;
} W32Dll;
static W32Dll g_w32_dlls[W32_DLL_MAX];
/* Windows serializes loader operations, including DllMain, with a recursive
 * loader lock. Never reuse the allocation mutex: a guest callback can allocate. */
static pthread_once_t g_w32_dll_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_w32_dll_mutex;
static void w32_dll_mutex_init(void)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_w32_dll_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}
static void w32_dll_lock(void)
{
    pthread_once(&g_w32_dll_once, w32_dll_mutex_init);
    pthread_mutex_lock(&g_w32_dll_mutex);
}

static int w32_dll_range(size_t total, u32 at, size_t bytes)
{
    return at <= total && bytes <= total - at;
}
static const char *w32_dll_string(const W32Dll *dll, u32 rva)
{
    if (rva >= dll->size) return NULL;
    const char *text = (const char *)dll->image + rva;
    return memchr(text, 0, dll->size - rva) ? text : NULL;
}
static W32Dll *w32_dll_find(u32 module)
{
    for (size_t i = 0; i < W32_DLL_MAX; ++i)
        if (g_w32_dlls[i].image && (u32)(uintptr_t)g_w32_dlls[i].image == module)
            return &g_w32_dlls[i];
    return NULL;
}
static const char *w32_dll_path(u32 module)
{
    static _Thread_local char path[PATH_MAX];
    w32_dll_lock();
    W32Dll *dll = w32_dll_find(module);
    if (dll) memcpy(path, dll->path, sizeof(path));
    pthread_mutex_unlock(&g_w32_dll_mutex);
    return dll ? path : NULL;
}
static u32 w32_dll_by_name_locked(const char *name)
{
    char key[64];
    w32_module_key(name, key, sizeof(key));
    int qualified = strchr(name, '/') != NULL || strchr(name, '\\') != NULL || strchr(name, ':') != NULL;
    char canonical[PATH_MAX], host[PATH_MAX];
    if (qualified && (!resolve_windows_host_path(name, host, sizeof(host)) || !realpath(host, canonical)))
        return 0;
    for (size_t i = 0; i < W32_DLL_MAX; ++i) {
        W32Dll *dll = &g_w32_dlls[i];
        if (!dll->image) continue;
        if (qualified) {
            if (!strcmp(canonical, dll->path)) return (u32)(uintptr_t)dll->image;
        } else {
            char candidate[64];
            w32_module_key(dll->path, candidate, sizeof(candidate));
            if (!strcasecmp(key, candidate)) return (u32)(uintptr_t)dll->image;
        }
    }
    return 0;
}

static u32 w32_dll_proc_locked(u32 module, uintptr_t name)
{
    W32Dll *dll = w32_dll_find(module);
    if (!dll) { g_last_error = 6; return 0; }
    DataDir dir = dll->exports;
    g_last_error = 127;
    if (dir.size < sizeof(ExportDir) || !w32_dll_range(dll->size, dir.rva, sizeof(ExportDir))) return 0;
    const ExportDir *exp = (const void *)(dll->image + dir.rva);
    if (!w32_dll_range(dll->size, exp->functions_rva, (u64)exp->function_count * 4) ||
        !w32_dll_range(dll->size, exp->names_rva, (u64)exp->name_count * 4) ||
        !w32_dll_range(dll->size, exp->ordinals_rva, (u64)exp->name_count * 2)) return 0;
    u32 index = UINT32_MAX;
    if (name < 0x10000) {
        if (name >= exp->ordinal_base && name - exp->ordinal_base < exp->function_count)
            index = name - exp->ordinal_base;
    } else {
        for (u32 i = 0; i < exp->name_count; ++i) {
            u32 rva; u16 ordinal;
            memcpy(&rva, dll->image + exp->names_rva + 4ull * i, 4);
            const char *text = w32_dll_string(dll, rva);
            if (!text) return 0;
            if (strcmp(text, (const char *)name)) continue;
            memcpy(&ordinal, dll->image + exp->ordinals_rva + 2ull * i, 2);
            index = ordinal;
            break;
        }
    }
    if (index >= exp->function_count) return 0;
    u32 rva;
    memcpy(&rva, dll->image + exp->functions_rva + 4ull * index, 4);
    if (!rva || rva >= dll->size) return 0;
    if (rva >= dir.rva && rva - dir.rva < dir.size) {
        /* Don't hand a forwarder string to the guest as executable code. */
        g_last_error = 50;
        return 0;
    }
    return module + rva;
}

static int w32_dll_relocate(W32Dll *dll, DataDir dir, u32 delta)
{
    if (!delta) return 1;
    if (!dir.size) return 0;
    size_t cursor = 0;
    while (cursor < dir.size) {
        if (dir.size - cursor < sizeof(RelocBlock)) return 0;
        RelocBlock block;
        memcpy(&block, dll->image + dir.rva + cursor, sizeof(block));
        if (block.block_sz < sizeof(block) || block.block_sz > dir.size - cursor || (block.block_sz & 1)) return 0;
        for (size_t i = sizeof(block); i < block.block_sz; i += 2) {
            u16 fixup;
            memcpy(&fixup, dll->image + dir.rva + cursor + i, 2);
            u32 type = fixup >> 12;
            if (!type) continue;
            if (type != 3 || (u64)block.page_rva + (fixup & 0xfff) + 4 > dll->size) return 0;
            u32 value;
            u8 *at = dll->image + block.page_rva + (fixup & 0xfff);
            memcpy(&value, at, 4); value += delta; memcpy(at, &value, 4);
        }
        cursor += block.block_sz;
    }
    return 1;
}

static int w32_dll_bind(W32Dll *dll, DataDir dir)
{
    if (!dir.size) return 1;
    for (size_t cursor = 0; cursor + sizeof(ImportDesc) <= dir.size; cursor += sizeof(ImportDesc)) {
        ImportDesc desc;
        memcpy(&desc, dll->image + dir.rva + cursor, sizeof(desc));
        if (!desc.orig_ilt && !desc.timestamp && !desc.fwd && !desc.name_rva && !desc.iat_rva) return 1;
        const char *name = w32_dll_string(dll, desc.name_rva);
        if (!name) return 0;
        u32 module = w32_module_by_name(name);
        if (!w32_is_system_module(module)) {
            fprintf(stderr, "[WOW32 DLL] dependency %s is not a supported system module\n", name);
            g_last_error = 126;
            return 0;
        }
        u32 lookup = desc.orig_ilt ? desc.orig_ilt : desc.iat_rva;
        if (!lookup || !desc.iat_rva) return 0;
        for (size_t index = 0;; ++index) {
            size_t at = (size_t)lookup + index * 4, out = (size_t)desc.iat_rva + index * 4;
            if (at > dll->size || dll->size - at < 4 || out > dll->size || dll->size - out < 4) return 0;
            u32 symbol;
            memcpy(&symbol, dll->image + at, 4);
            if (!symbol) break;
            const char *proc = NULL;
            if (!(symbol & 0x80000000u) && w32_dll_range(dll->size, symbol, 3))
                proc = w32_dll_string(dll, symbol + 2);
            const u32 data_address = proc ? w32_data_import(proc) : 0;      /* exported variables (_iob) */
            if (data_address) { memcpy(dll->image + out, &data_address, 4); continue; }
            const W32Api *api = proc ? w32_find_api(proc) : NULL;
            if (!api || (!api->fn && !find_impl(proc))) {
                if (symbol & 0x80000000u)
                    fprintf(stderr, "[WOW32 DLL] %s: unsupported ordinal import %s!#%u\n",
                            dll->path, name, symbol & 0xffff);
                else fprintf(stderr, "[WOW32 DLL] %s: unresolved import %s!%s\n",
                             dll->path, name, proc ? proc : "<invalid name>");
                g_last_error = 127;
                return 0;
            }
            u32 address = w32_thunk_for(name, proc);
            memcpy(dll->image + out, &address, 4);
        }
    }
    return 0; /* Missing terminated import descriptor list. */
}

/* TLS callbacks: a NUL-terminated array of 32-bit function addresses, called like DllMain. */
static void w32_dll_tls_callbacks(const u8 *image, size_t size, u32 array, u32 module, u32 reason)
{
    if (!array) return;
    const uintptr_t base = (uintptr_t)image;
    for (uintptr_t cursor = array; cursor >= base && cursor + 4 <= base + size; cursor += 4) {
        const u32 callback = *(const u32 *)cursor;
        if (!callback) break;
        const u32 argv[3] = { module, reason, 0 };
        w32_call(callback, 3, argv);
    }
}

static u32 w32_dll_load_locked(const char *name)
{
    u32 caller_error = g_last_error;
    u32 existing = w32_dll_by_name(name);
    if (existing) {
        W32Dll *dll = w32_dll_find(existing);
        /* Module lookup/exports are available during attach, but this initial
         * subset rejects recursive self-load instead of publishing a reference
         * that would dangle if the outer attach subsequently failed. */
        if (dll->state != 1 || dll->refs == UINT32_MAX) { g_last_error = 1114; return 0; }
        ++dll->refs;
        return existing;
    }
    W32Dll *slot = NULL;
    for (size_t i = 0; i < W32_DLL_MAX; ++i)
        if (!g_w32_dlls[i].image) { slot = &g_w32_dlls[i]; break; }
    if (!slot) { g_last_error = 8; return 0; }
    char host[PATH_MAX], canonical[PATH_MAX];
    if (!resolve_windows_host_path(name, host, sizeof(host)) || !realpath(host, canonical)) {
        fprintf(stderr, "[WOW32 DLL] cannot find %s\n", name);
        g_last_error = 126; return 0;
    }
    int fd = open(canonical, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { g_last_error = 126; return 0; }
    struct stat st;
    if (fstat(fd, &st) || st.st_size < 64 || (u64)st.st_size > SIZE_MAX) { close(fd); g_last_error = 193; return 0; }
    size_t file_size = st.st_size;
    u8 *file = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (file == MAP_FAILED) { g_last_error = 8; return 0; }
    W32Dll dll = {0};
    u8 *protection = NULL;
    u32 error = 193;
    DosHdr dos; memcpy(&dos, file, sizeof(dos));
    if (dos.magic != MZ_MAGIC || !w32_dll_range(file_size, dos.lfanew, sizeof(W32NtHdrs))) goto fail;
    W32NtHdrs nt; memcpy(&nt, file + dos.lfanew, sizeof(nt));
    if (nt.sig != PE_SIG || nt.file.machine != 0x14c || nt.opt.magic != 0x10b ||
        !(nt.file.characteristics & 0x2000) || nt.file.opthdr_sz < sizeof(W32OptHdr) ||
        !nt.file.nsections || nt.file.nsections > 96 || nt.opt.rva_count > 16 ||
        nt.opt.section_align < 4096 || (nt.opt.section_align & (nt.opt.section_align - 1)) ||
        !nt.opt.size_of_image || nt.opt.size_of_image > (256u << 20) ||
        (nt.opt.size_of_image & (nt.opt.section_align - 1)) ||
        !nt.opt.size_of_headers || nt.opt.size_of_headers > file_size ||
        nt.opt.size_of_headers > nt.opt.size_of_image) goto fail;
    size_t table = (size_t)dos.lfanew + 24 + nt.file.opthdr_sz;
    if (table > nt.opt.size_of_headers || (u64)nt.file.nsections * sizeof(SecHdr) > nt.opt.size_of_headers - table) goto fail;
    for (u32 i = 0; i < nt.opt.rva_count; ++i) {
        /* Security directory is a raw file offset, not a loaded RVA. */
        if (i != 4 && nt.opt.dirs[i].size && !w32_dll_range(nt.opt.size_of_image, nt.opt.dirs[i].rva, nt.opt.dirs[i].size)) goto fail;
    }
    if (nt.opt.rva_count > 13 && nt.opt.dirs[13].size) { error = 50; goto fail; }   /* delay imports */
    dll.size = nt.opt.size_of_image;
    dll.entry = nt.opt.entry_rva;
    dll.exports = nt.opt.rva_count ? nt.opt.dirs[0] : (DataDir){0};
    snprintf(dll.path, sizeof(dll.path), "%s", canonical);
    if (nt.opt.image_base && ((nt.opt.image_base & 0xffff) || (u64)nt.opt.image_base + dll.size > W32_LIMIT)) goto fail;
    dll.image = w32_map(nt.opt.image_base, dll.size, PROT_READ | PROT_WRITE);
    if (!dll.image) { error = 8; goto fail; }
    size_t pages = ((size_t)dll.size + 4095) / 4096;
    protection = malloc(pages);
    if (!protection) { error = 8; goto fail; }
    /* Keep image metadata/gaps readable for bounded export/resource lookup. */
    memset(protection, PROT_READ, pages);
    memcpy(dll.image, file, nt.opt.size_of_headers);
    int executable_entry = dll.entry == 0;
    u64 previous_end = nt.opt.size_of_headers;
    for (u16 i = 0; i < nt.file.nsections; ++i) {
        SecHdr section; memcpy(&section, file + table + i * sizeof(section), sizeof(section));
        u32 span = section.vsz > section.raw_sz ? section.vsz : section.raw_sz;
        if (!w32_dll_range(file_size, section.raw_off, section.raw_sz) ||
            !w32_dll_range(dll.size, section.vrva, span) || section.vrva < previous_end ||
            (section.vrva & (nt.opt.section_align - 1))) goto fail;
        previous_end = (u64)section.vrva + span;
        if (section.raw_sz) memcpy(dll.image + section.vrva, file + section.raw_off, section.raw_sz);
        int prot = PROT_READ | ((section.chars & SCN_WRITE) ? PROT_WRITE : 0) |
                                ((section.chars & SCN_EXEC) ? PROT_EXEC : 0);
        if (span) for (size_t p = section.vrva / 4096; p <= ((u64)section.vrva + span - 1) / 4096; ++p)
            protection[p] |= prot;
        if (dll.entry >= section.vrva && dll.entry - section.vrva < span && (prot & PROT_EXEC)) executable_entry = 1;
    }
    if (!executable_entry || !w32_dll_relocate(&dll, nt.opt.rva_count > 5 ? nt.opt.dirs[5] : (DataDir){0},
                                               (u32)(uintptr_t)dll.image - nt.opt.image_base)) goto fail;
    g_last_error = 193;
    if (!w32_dll_bind(&dll, nt.opt.rva_count > 1 ? nt.opt.dirs[1] : (DataDir){0})) {
        error = g_last_error; goto fail;
    }
    /* Static TLS: slot, index variable and the loading thread's block, while the image is still writable. */
    u32 tls_callbacks = 0, tls_slot = 0;
    if (nt.opt.rva_count > 9 && nt.opt.dirs[9].size &&
        !w32_tls_register_dll(dll.image, dll.size, &nt.opt.dirs[9], &tls_slot, &tls_callbacks)) { error = 50; goto fail; }
    for (size_t p = 0; p < pages; ++p)
        if (mprotect(dll.image + 4096 * p, 4096, protection[p])) { error = 8; goto fail; }
    free(protection); protection = NULL;
    munmap(file, file_size); file = NULL;
    dll.refs = 1;
    dll.state = 2;
    *slot = dll; /* DllMain can find itself, resolve exports and nest imports. */
    g_last_error = caller_error;
    const u32 args[] = {(u32)(uintptr_t)dll.image, 1, 0};
    w32_dll_tls_callbacks(dll.image, dll.size, tls_callbacks, args[0], 1);          /* DLL_PROCESS_ATTACH */
    if (dll.entry && !w32_call((u32)(uintptr_t)dll.image + dll.entry, 3, args)) {
        /* A rejected attach is followed by process detach, like LoadLibrary. */
        slot->state = 3;
        const u32 detach[] = {args[0], 0, 0};
        w32_call(args[0] + dll.entry, 3, detach);
        memset(slot, 0, sizeof(*slot));
        munmap(dll.image, ((size_t)dll.size + 0xffff) & ~(size_t)0xffff);
        g_last_error = 1114;
        return 0;
    }
    slot->state = 1;
    fprintf(stderr, "[WOW32 DLL] attached %s at 0x%x\n", dll.path, args[0]);
    return args[0];
fail:
    free(protection);
    if (dll.image) munmap(dll.image, ((size_t)dll.size + 0xffff) & ~(size_t)0xffff);
    if (file) munmap(file, file_size);
    g_last_error = error;
    fprintf(stderr, "[WOW32 DLL] cannot load %s (Windows error %u)\n", canonical, error);
    return 0;
}

static int w32_dll_free_locked(u32 module)
{
    W32Dll *dll = w32_dll_find(module);
    if (!dll) { g_last_error = 6; return 0; }
    if (dll->state != 1) { g_last_error = 1114; return 0; }
    if (--dll->refs) return 1;
    dll->state = 3;
    if (dll->entry) {
        const u32 args[] = {module, 0, 0};
        w32_call(module + dll->entry, 3, args);
    }
    munmap(dll->image, ((size_t)dll->size + 0xffff) & ~(size_t)0xffff);
    memset(dll, 0, sizeof(*dll));
    return 1;
}

static u32 w32_dll_by_name(const char *name)
{
    w32_dll_lock();
    u32 result = w32_dll_by_name_locked(name);
    pthread_mutex_unlock(&g_w32_dll_mutex);
    return result;
}
static u32 w32_dll_proc(u32 module, uintptr_t name)
{
    w32_dll_lock();
    u32 result = w32_dll_proc_locked(module, name);
    pthread_mutex_unlock(&g_w32_dll_mutex);
    return result;
}
static u32 w32_dll_load(const char *name)
{
    w32_dll_lock();
    u32 result = w32_dll_load_locked(name);
    pthread_mutex_unlock(&g_w32_dll_mutex);
    return result;
}
static int w32_dll_free(u32 module)
{
    w32_dll_lock();
    int result = w32_dll_free_locked(module);
    pthread_mutex_unlock(&g_w32_dll_mutex);
    return result;
}
/* The mapped PE32 DLL containing `address` (base and size), for VirtualQuery's AllocationBase. */
static int w32_dll_image_range(u32 address, u32 *base, u32 *size)
{
    int found = 0;
    w32_dll_lock();
    for (size_t i = 0; i < W32_DLL_MAX && !found; ++i) {
        const W32Dll *dll = &g_w32_dlls[i];
        const u32 start = (u32)(uintptr_t)dll->image;
        if (dll->image && address >= start && address - start < dll->size) { *base = start; *size = dll->size; found = 1; }
    }
    pthread_mutex_unlock(&g_w32_dll_mutex);
    return found;
}

/* ---- export tables of the system pseudo-modules ----
 * Packers and anti-hook code (BASS.dll's loader stub) read a module's PE headers and export directory straight from
 * memory instead of calling GetProcAddress. Each system module therefore gets a small in-memory PE image at its
 * pseudo handle: headers plus an export table listing every function this layer implements, each pointing at that
 * function's thunk (RVA = thunk - handle, modulo 2^32). Every module exports the same set; GetProcAddress keeps
 * answering from the same tables. */
static int w32_name_compare(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }
static void w32_sysmod_ensure_image(u32 handle)
{
    static u32 built[64];
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    for (size_t i = 0; i < sizeof(built) / sizeof(*built); ++i)
        if (built[i] == handle) { pthread_mutex_unlock(&lock); return; }
    size_t free_slot = 0;
    while (free_slot < sizeof(built) / sizeof(*built) && built[free_slot]) ++free_slot;
    u8 *image = mmap((void *)(uintptr_t)handle, 0x10000, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (image == MAP_FAILED || (u32)(uintptr_t)image != handle || free_slot == sizeof(built) / sizeof(*built)) {
        if (image != MAP_FAILED) munmap(image, 0x10000);
        pthread_mutex_unlock(&lock);
        return;
    }
    built[free_slot] = handle;
    const size_t total = sizeof(g_w32_apis) / sizeof(*g_w32_apis);
    const char **names = malloc(total * sizeof(*names));
    size_t count = 0;
    for (size_t i = 0; i < total; ++i) {
        const char *name = g_w32_apis[i].name;
        int duplicate = 0;
        for (size_t j = 0; j < count && !duplicate; ++j) duplicate = !strcmp(names[j], name);
        if (!duplicate && name[0] != '#') names[count++] = name;
    }
    qsort(names, count, sizeof(*names), w32_name_compare);
    /* layout inside the section (rva 0x1000): directory, function RVAs, name RVAs, ordinals, module name, strings */
    const u32 base_rva = 0x1000;
    const u32 dir_size = 40, funcs = base_rva + dir_size, name_table = funcs + 4 * (u32)count,
              ordinals = name_table + 4 * (u32)count, module_name = ordinals + 2 * (u32)count,
              strings = module_name + 16;
    u32 cursor = strings;
    const u32 limit = 0x10000;
    while (count && cursor + 1 > limit) --count;                 /* (defensive) */
    /* names that would not fit are dropped from the tail */
    u32 used = strings;
    size_t fit = 0;
    for (; fit < count; ++fit) {
        const u32 need = (u32)strlen(names[fit]) + 1;
        if (used + need > limit) break;
        used += need;
    }
    count = fit;
    memcpy(image, "MZ", 2);
    *(u32 *)(image + 0x3c) = 0x80;
    u8 *nt = image + 0x80;
    memcpy(nt, "PE\0\0", 4);
    *(u16 *)(nt + 4) = 0x14c; *(u16 *)(nt + 6) = 1;               /* i386, one section */
    *(u16 *)(nt + 20) = 0xe0; *(u16 *)(nt + 22) = 0x2102;         /* optional header size, DLL */
    u8 *opt = nt + 24;
    *(u16 *)opt = 0x10b;
    *(u32 *)(opt + 28) = handle;                                  /* ImageBase */
    *(u32 *)(opt + 32) = 0x1000; *(u32 *)(opt + 36) = 0x200;      /* alignments */
    *(u16 *)(opt + 40) = 5; *(u16 *)(opt + 48) = 5;               /* OS / subsystem versions */
    *(u32 *)(opt + 56) = 0x10000; *(u32 *)(opt + 60) = 0x400;     /* SizeOfImage, SizeOfHeaders */
    *(u16 *)(opt + 68) = 3;                                       /* console subsystem */
    *(u32 *)(opt + 92) = 16;                                      /* data directory count */
    *(u32 *)(opt + 96) = base_rva; *(u32 *)(opt + 100) = used - base_rva;   /* export directory */
    u8 *section = opt + 0xe0;
    memcpy(section, ".edata", 6);
    *(u32 *)(section + 8) = used - base_rva; *(u32 *)(section + 12) = base_rva;
    *(u32 *)(section + 16) = 0x200; *(u32 *)(section + 20) = 0x400;
    *(u32 *)(section + 36) = 0x40000040;                          /* initialized data, readable */
    u8 *sec = image + base_rva;
    *(u32 *)(sec + 12) = module_name;                             /* Name */
    *(u32 *)(sec + 16) = 1;                                       /* Base */
    *(u32 *)(sec + 20) = (u32)count; *(u32 *)(sec + 24) = (u32)count;
    *(u32 *)(sec + 28) = funcs; *(u32 *)(sec + 32) = name_table; *(u32 *)(sec + 36) = ordinals;
    memcpy(image + module_name, "system.dll", 11);
    u32 at = strings;
    for (size_t i = 0; i < count; ++i) {
        const u32 thunk = w32_thunk_for("kernel32.dll", names[i]);
        *(u32 *)(image + funcs + 4 * i) = thunk - handle;
        *(u32 *)(image + name_table + 4 * i) = at;
        *(u16 *)(image + ordinals + 2 * i) = (u16)i;
        const size_t length = strlen(names[i]) + 1;
        memcpy(image + at, names[i], length);
        at += (u32)length;
    }
    (void)cursor;
    free(names);
    mprotect(image, 0x10000, PROT_READ);
    pthread_mutex_unlock(&lock);
}
