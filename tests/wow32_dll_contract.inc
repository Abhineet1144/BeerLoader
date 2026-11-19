/* Real compatibility-mode code, not host ABI replacements for DllMain. */
static void wow32_dll_fixture(u8 file[0x1200])
{
    memset(file, 0, 0x1200);
    DosHdr *dos = (void *)file;
    dos->magic = MZ_MAGIC; dos->lfanew = 0x80;
    W32NtHdrs *nt = (void *)(file + 0x80);
    nt->sig = PE_SIG; nt->file.machine = 0x14c; nt->file.nsections = 1;
    nt->file.opthdr_sz = sizeof(W32OptHdr); nt->file.characteristics = 0x2102;
    nt->opt.magic = 0x10b; nt->opt.image_base = 0x60000000;
    nt->opt.section_align = 0x1000; nt->opt.file_align = 0x200;
    nt->opt.size_of_headers = 0x200; nt->opt.size_of_image = 0x2000;
    nt->opt.entry_rva = 0x1000; nt->opt.rva_count = 16;
    nt->opt.dirs[0] = (DataDir){0x1400, 0x100};
    nt->opt.dirs[1] = (DataDir){0x1200, 40};
    nt->opt.dirs[5] = (DataDir){0x1500, 12};
    SecHdr *section = (void *)((u8 *)&nt->opt + sizeof(nt->opt));
    memcpy(section->name, ".all", 4);
    section->vsz = section->raw_sz = 0x1000;
    section->vrva = 0x1000; section->raw_off = 0x200;
    section->chars = SCN_READ | SCN_WRITE | SCN_EXEC;
    u8 *raw = file + 0x200;
    const u8 entry[] = {
        0x8b,0x44,0x24,0x04,             /* mov eax,[esp+4] (hmodule) */
        0x8b,0x4c,0x24,0x08,             /* mov ecx,[esp+8] (reason) */
        0xff,0x84,0x88,0x00,0x11,0,0,    /* inc [eax+ecx*4+1100] */
        0x8b,0x90,0x20,0x11,0,0,         /* mov edx,[eax+1120] (relocated pointer) */
        0xc7,0x02,0x34,0x12,0,0,         /* mov [edx],1234 */
        0x50,                            /* push eax (preserve module across API) */
        0xff,0x90,0x00,0x13,0,0,         /* call [eax+1300] (GetLastError) */
        0x5a,                            /* pop edx */
        0x89,0x82,0x10,0x11,0,0,         /* mov [edx+1110],eax */
        0xb8,0x01,0,0,0,0xc2,0x0c,0     /* return TRUE, pop three args */
    };
    memcpy(raw, entry, sizeof(entry));
    const u8 proc[] = {0x8b,0x44,0x24,0x04,0x83,0xc0,7,0xc2,4,0};
    memcpy(raw + 0x80, proc, sizeof(proc));
    *(u32 *)(raw + 0x120) = 0x60001124;
    ImportDesc *imports = (void *)(raw + 0x200);
    imports->orig_ilt = 0x1280; imports->name_rva = 0x1260; imports->iat_rva = 0x1300;
    strcpy((char *)raw + 0x260, "kernel32.dll");
    *(u32 *)(raw + 0x280) = 0x12a0;
    strcpy((char *)raw + 0x2a2, "GetLastError");
    ExportDir *exp = (void *)(raw + 0x400);
    exp->ordinal_base = 7; exp->function_count = 2; exp->name_count = 1;
    exp->functions_rva = 0x1440; exp->names_rva = 0x1450; exp->ordinals_rva = 0x1458;
    *(u32 *)(raw + 0x440) = 0x1080;
    *(u32 *)(raw + 0x444) = 0x1468; /* Explicitly unsupported forwarder. */
    *(u32 *)(raw + 0x450) = 0x1460;
    *(u16 *)(raw + 0x458) = 0;
    strcpy((char *)raw + 0x460, "Add7");
    strcpy((char *)raw + 0x468, "kernel32.GetLastError");
    RelocBlock *reloc = (void *)(raw + 0x500);
    reloc->page_rva = 0x1000; reloc->block_sz = 12;
    *(u16 *)(raw + 0x508) = 0x3120; /* pointer at image+1120 */
}

static void wow32_dll_write(const char *path, const u8 *file, size_t size)
{
    int fd = open(path, O_WRONLY | O_TRUNC);
    assert(fd >= 0 && write(fd, file, size) == (ssize_t)size);
    assert(!close(fd));
}

static void *wow32_dll_ref_worker(void *arg)
{
    const char *path = arg;
    for (unsigned i = 0; i < 50; ++i) {
        u32 module = w32_dll_load(path);
        assert(module && w32_dll_by_name(path) == module);
        assert(w32_dll_free(module));
    }
    return NULL;
}

static void wow32_dll_child_test(void)
{
    g_w32_image = 0;
    w32_thunks_init();
    w32_thread_init(1u << 20);
    u8 *file = malloc(0x1200);
    assert(file);
    char path[] = "/tmp/beer-wow32-dll-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0); close(fd);
    /* Occupy preferred base so HIGHLOW relocation is actually exercised. */
    void *blocked = mmap((void *)0x60000000, 0x10000, PROT_NONE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(blocked != MAP_FAILED);
    wow32_dll_fixture(file);
    wow32_dll_write(path, file, 0x1200);
    g_last_error = 123;
    u32 module = w32_dll_load(path);
    assert(module && module != 0x60000000);
    u32 *state = (void *)(uintptr_t)(module + 0x1100);
    assert(state[0] == 0 && state[1] == 1); /* process attach exactly once */
    assert(state[4] == 123 && state[9] == 0x1234); /* API roundtrip + relocated write */
    assert(*(u32 *)(uintptr_t)(module + 0x1120) == module + 0x1124);
    assert(w32_dll_load(path) == module && state[1] == 1);
    assert(w32_dll_by_name(path) == module);
    u32 proc = w32_dll_proc(module, (uintptr_t)"Add7");
    assert(proc == module + 0x1080 && w32_dll_proc(module, 7) == proc);
    assert(!w32_dll_proc(module, 8) && g_last_error == 50);
    assert(!w32_dll_proc(module, (uintptr_t)"Missing") && g_last_error == 127);
    u32 *guest = w32_heap_alloc(256, 1);
    assert(guest);
    strcpy((char *)guest, "Add7");
    u32 lookup[] = {module, (u32)(uintptr_t)guest};
    assert(w32_GetProcAddress(lookup) == proc);
    u32 path_args[] = {module, (u32)(uintptr_t)guest, 120};
    assert(w32_GetModuleFileNameW(path_args) == strlen(path) + 2);
    assert(((u16 *)guest)[0] == 'Z' && ((u16 *)guest)[1] == ':' && ((u16 *)guest)[2] == '\\');
    const u32 value[] = {35};
    assert(w32_call(proc, 1, value) == 42);
    assert(w32_dll_free(module) && state[1] == 1);
    /* Reject reentrant unload while attaching, without releasing its mapping. */
    W32Dll *dll = w32_dll_find(module);
    dll->state = 2;
    assert(!w32_dll_free(module) && g_last_error == 1114);
    assert(!w32_dll_load(path) && g_last_error == 1114);
    dll->state = 1;
    *(u32 *)(uintptr_t)(module + 0x1120) = (u32)(uintptr_t)(guest + 32);
    guest[32] = 0;
    assert(w32_dll_free(module));
    assert(guest[32] == 0x1234); /* Real process-detach code ran before unmapping. */
    unsigned char resident;
    assert(mincore((void *)(uintptr_t)module, 4096, &resident) == -1 && errno == ENOMEM);
    assert(!w32_dll_path(module) && !w32_dll_free(module) && g_last_error == 6);
    /* No entry call on malformed data or imports with unknown ABI. */
    for (int kind = 0; kind < 8; ++kind) {
        wow32_dll_fixture(file);
        W32NtHdrs *nt = (void *)(file + 0x80);
        if (kind == 0) nt->opt.dirs[9] = (DataDir){0x1600, 24};
        if (kind == 1) nt->opt.dirs[13] = (DataDir){0x1600, 32};
        if (kind == 2) nt->opt.dirs[5].size = 0;
        if (kind == 3) strcpy((char *)file + 0x4a2, "BeerUnknownImport");
        if (kind == 4) ((RelocBlock *)(file + 0x700))->block_sz = 14;
        if (kind == 5) ((SecHdr *)((u8 *)&nt->opt + sizeof(nt->opt)))->raw_sz = 0x2000;
        if (kind == 6) nt->opt.entry_rva = 0x19000;
        if (kind == 7) *(u32 *)(file + 0x480) = 0x80000007; /* ordinal import unsupported */
        wow32_dll_write(path, file, 0x1200);
        assert(!w32_dll_load(path));
        assert(g_last_error == (kind < 2 ? 50 : kind == 3 || kind == 7 ? 127 : 193));
        assert(!w32_dll_by_name(path));
    }
    /* A real DllMain rejection is ERROR_DLL_INIT_FAILED, not successful load. */
    wow32_dll_fixture(file);
    const u8 reject[] = {0x31,0xc0,0xc2,0x0c,0};
    memcpy(file + 0x200, reject, sizeof(reject));
    wow32_dll_write(path, file, 0x1200);
    assert(!w32_dll_load(path) && g_last_error == 1114 && !w32_dll_by_name(path));
    /* No DllMain: final image page flags must come from the section, not RWX. */
    wow32_dll_fixture(file);
    W32NtHdrs *nt = (void *)(file + 0x80);
    nt->opt.entry_rva = 0;
    ((SecHdr *)((u8 *)&nt->opt + sizeof(nt->opt)))->chars = SCN_READ | SCN_EXEC;
    wow32_dll_write(path, file, 0x1200);
    module = w32_dll_load(path);
    assert(module);
    pthread_t workers[4];
    for (size_t i = 0; i < 4; ++i) assert(!pthread_create(&workers[i], NULL, wow32_dll_ref_worker, path));
    for (size_t i = 0; i < 4; ++i) assert(!pthread_join(workers[i], NULL));
    assert(w32_dll_find(module)->refs == 1);
    FILE *maps = fopen("/proc/self/maps", "r");
    assert(maps);
    char line[512], flags[5];
    unsigned long low, high;
    int checked = 0;
    while (fgets(line, sizeof(line), maps)) {
        if (sscanf(line, "%lx-%lx %4s", &low, &high, flags) != 3) continue;
        if (module + 0x1000 < low || module + 0x1000 >= high) continue;
        assert(flags[0] == 'r' && flags[1] == '-' && flags[2] == 'x');
        checked = 1;
    }
    fclose(maps);
    assert(checked && w32_dll_free(module));
    free(file); w32_heap_free(guest);
    assert(!unlink(path));
    munmap(blocked, 0x10000);
}

static void wow32_dll_contract_test(void)
{
    /* Isolate TEB/GS/LDT setup and faults from native ABI tests. */
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) { wow32_dll_child_test(); _exit(0); }
    int status;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}