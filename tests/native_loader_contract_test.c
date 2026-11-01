/* Exercise the real guest ABI wrappers and Kernel32/native interoperability,
 * not a copy of the loader's dispatch logic. Unused loader code is discarded. */
#define main beer_loader_unused_main
#include "../loader.c"
#undef main
#include <assert.h>

static int native_veh_calls;
typedef struct { WIN_CS *cs; pthread_barrier_t tried; } CriticalSectionContender;
static void *critical_section_contender(void *arg)
{
    CriticalSectionContender *state = arg;
    WIN_CS *cs = state->cs;
    assert(!impl_TryEnterCriticalSection(cs));
    pthread_barrier_wait(&state->tried);
    impl_EnterCriticalSection(cs);
    assert(cs->OwningThread == current_windows_thread_id() && cs->RecursionCount == 1);
    impl_LeaveCriticalSection(cs);
    return NULL;
}
static void critical_section_layout_test(void)
{
    struct { WIN_CS cs; u64 guard; } object = { .guard = 0x123456789abcdef0ull };
    WIN_CS *cs = &object.cs;
    assert(impl_InitializeCriticalSectionAndSpinCount(cs, 250));
    assert(cs->LockCount == -1 && !cs->RecursionCount && !cs->OwningThread);
    assert(!cs->DebugInfo && !cs->LockSemaphore && cs->SpinCount == 250);
    impl_EnterCriticalSection(cs);
    u64 owner;
    memcpy(&owner, (u8 *)cs + 16, 8);
    assert(owner == current_windows_thread_id() && cs->RecursionCount == 1 && cs->LockCount == 0);
    assert(impl_TryEnterCriticalSection(cs));
    assert(cs->RecursionCount == 2 && cs->LockCount == 1);
    impl_LeaveCriticalSection(cs);
    assert(cs->RecursionCount == 1 && cs->OwningThread == owner);
    CriticalSectionContender state = { .cs = cs };
    pthread_barrier_init(&state.tried, NULL, 2);
    pthread_t contender;
    assert(!pthread_create(&contender, NULL, critical_section_contender, &state));
    pthread_barrier_wait(&state.tried);
    assert(object.guard == 0x123456789abcdef0ull);
    impl_LeaveCriticalSection(cs);
    pthread_join(contender, NULL);
    pthread_barrier_destroy(&state.tried);
    assert(!cs->OwningThread && !cs->RecursionCount && cs->LockCount == -1);
    impl_DeleteCriticalSection(cs);
    assert(impl_InitializeCriticalSection(cs));
    assert(impl_TryEnterCriticalSection(cs));
    impl_LeaveCriticalSection(cs);
    impl_DeleteCriticalSection(cs);
}

static void late_loader_list_test(void)
{
    WinPEB peb = {0};
    WinPebLdrData ldr = { .Length = sizeof(ldr), .Initialized = 1 };
    win_list_init(&ldr.InLoadOrderModuleList);
    win_list_init(&ldr.InMemoryOrderModuleList);
    win_list_init(&ldr.InInitializationOrderModuleList);
    peb.Ldr = (u64)(uintptr_t)&ldr;
    WinPEB *saved_peb = g_process_peb;
    g_process_peb = &peb;
    GuestDll dll = { .image_size = 4096 };
    strcpy(dll.name, "loader-fixture.dll");
    strcpy(dll.path, "C:\\fixtures\\loader-fixture.dll");
    dll.image = mmap(NULL, dll.image_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(dll.image != MAP_FAILED);
    DosHdr *dos = (DosHdr *)dll.image;
    dos->magic = MZ_MAGIC;
    dos->lfanew = sizeof(*dos);
    NtHdrs64 *nt = (NtHdrs64 *)(dll.image + dos->lfanew);
    nt->sig = PE_SIG;
    nt->opt.magic = PE32PLUS;
    nt->file.timestamp = 12345;
    assert(guest_dll_finish(&dll) == &dll);
    /* Finishing, not DllMain, publishes the image: callbacks can find it. */
    assert(!dll.entry_called);
    WinLdrDataTableEntry *entry = (WinLdrDataTableEntry *)(uintptr_t)ldr.InLoadOrderModuleList.Flink;
    assert((void *)entry != (void *)&ldr.InLoadOrderModuleList);
    assert(entry->DllBase == (u64)(uintptr_t)dll.image);
    assert(!entry->EntryPoint && entry->SizeOfImage == 4096 && entry->TimeDateStamp == 12345);
    assert(entry->BaseDllName.Length == strlen(dll.name) * 2);
    assert(entry->FullDllName.Length == strlen(dll.path) * 2);
    const u16 *name = (const u16 *)(uintptr_t)entry->BaseDllName.Buffer;
    for (size_t i = 0; i <= strlen(dll.name); ++i) assert(name[i] == (u8)dll.name[i]);
    assert(ldr.InMemoryOrderModuleList.Flink == (u64)(uintptr_t)&entry->InMemoryOrderLinks);
    assert(ldr.InInitializationOrderModuleList.Flink == (u64)(uintptr_t)&entry->InInitializationOrderLinks);
    assert(entry->InLoadOrderLinks.Flink == (u64)(uintptr_t)&ldr.InLoadOrderModuleList);
    assert(entry->InLoadOrderLinks.Blink == (u64)(uintptr_t)&ldr.InLoadOrderModuleList);
    assert(guest_dll_finish(&dll) == &dll);
    assert(ldr.InLoadOrderModuleList.Flink == (u64)(uintptr_t)entry);
    assert(ldr.InLoadOrderModuleList.Blink == (u64)(uintptr_t)entry);
    /* Static imports use exactly the same facade identity as LoadLibrary. */
    nt->opt.dirs[DIR_IMPORT].rva = 512;
    nt->opt.dirs[DIR_IMPORT].size = 2 * sizeof(ImportDesc);
    ImportDesc *import = (ImportDesc *)(dll.image + 512);
    import->name_rva = 768;
    strcpy((char *)dll.image + 768, "SHELL32.dll");
    publish_static_import_identities(dll.image);
    u64 shell = dll_name_to_handle("shell32.dll", 0);
    assert(shell && *(const u16 *)(uintptr_t)shell == MZ_MAGIC);
    WinLdrDataTableEntry *facade = (WinLdrDataTableEntry *)(uintptr_t)entry->InLoadOrderLinks.Flink;
    assert(facade->DllBase == shell);
    assert(facade->InLoadOrderLinks.Flink == (u64)(uintptr_t)&ldr.InLoadOrderModuleList);
    assert(ldr.InMemoryOrderModuleList.Blink == (u64)(uintptr_t)&facade->InMemoryOrderLinks);
    assert(ldr.InInitializationOrderModuleList.Blink == (u64)(uintptr_t)&facade->InInitializationOrderLinks);
    publish_static_import_identities(dll.image);
    assert(entry->InLoadOrderLinks.Flink == (u64)(uintptr_t)facade);
    assert(facade->InLoadOrderLinks.Flink == (u64)(uintptr_t)&ldr.InLoadOrderModuleList);
    free((void *)(uintptr_t)facade->BaseDllName.Buffer);
    free((void *)(uintptr_t)facade->FullDllName.Buffer);
    free(facade);
    free((void *)(uintptr_t)entry->BaseDllName.Buffer);
    free((void *)(uintptr_t)entry->FullDllName.Buffer);
    free(entry);
    munmap(dll.image, dll.image_size);
    g_process_peb = saved_peb;
}

static int32_t __attribute__((ms_abi)) native_veh_probe(void *p)
{
    assert(p == &native_veh_calls);
    ++native_veh_calls;
    return BEER_EXCEPTION_CONTINUE_EXECUTION;
}

static void guest_module_filename_test(void)
{
    int saved_count = g_guest_dll_count;
    const char *saved_exe = g_exe_path;
    assert(saved_count < MAX_GUEST_DLLS);
    GuestDll *dll = &g_guest_dlls[g_guest_dll_count++];
    GuestDll saved = *dll;
    char image;
    memset(dll, 0, sizeof(*dll));
    dll->image = (u8 *)&image;
    strcpy(dll->name, "filename-fixture.dll");
    strcpy(dll->path, "/tmp/Module Directory/filename-fixture.dll");
    g_exe_path = "Z:\\tmp\\Application\\fixture.exe";
    const char *expected = "Z:\\tmp\\Module Directory\\filename-fixture.dll";
    char ansi[PATH_MAX + 3];
    u16 wide[PATH_MAX + 3];
    u64 handle = (u64)(uintptr_t)dll->image;
    assert(impl_GetModuleFileNameA(handle, ansi, sizeof(ansi)) == strlen(expected));
    assert(!strcmp(ansi, expected));
    assert(impl_GetModuleFileNameW(handle, wide, sizeof(wide) / sizeof(*wide)) == strlen(expected));
    for (size_t i = 0; i <= strlen(expected); ++i) assert(wide[i] == (u8)expected[i]);
    strcpy(dll->path, "C:\\fixtures\\filename-fixture.dll");
    assert(!strcmp(module_file_name(handle), dll->path));
    assert(!strcmp(module_file_name(0), g_exe_path));
    *dll = saved;
    g_guest_dll_count = saved_count;
    g_exe_path = saved_exe;
}

int main(void)
{
    guest_module_filename_test();
    critical_section_layout_test();
    u64 event = impl_CreateEventW(0, 1, 0, 0);
    assert(event && event == (u64)(int64_t)(int32_t)event);
    assert(impl_SetEvent(event));
    assert(impl_WaitForSingleObject(event, 0) == 0);
    assert(event_id_from_handle(event | (1ull << 32)) == -1);
    assert(impl_CloseHandle(event));
    int file_fd = open("/dev/null", O_RDWR);
    assert(file_fd >= 0);
    u64 file_handle = fh_alloc(file_fd, "fixture", "/dev/null");
    assert(file_handle != UINT64_MAX && file_handle == (u64)(int64_t)(int32_t)file_handle);
    assert(fh_get(file_handle) == file_fd && fh_close(file_handle));
    u8 process_info[24];
    memset(process_info, 0xa5, sizeof(process_info));
    assert(!beer_CreateProcessA("fixture.exe", NULL, NULL, NULL, 1, 0, NULL, NULL, NULL, process_info));
    assert(g_last_error == 50);
    for (size_t i = 0; i < sizeof(process_info); ++i) assert(!process_info[i]);
    const u16 process_name[] = {'f', 'i', 'x', 't', 'u', 'r', 'e', 0};
    memset(process_info, 0xa5, sizeof(process_info));
    assert(!beer_CreateProcessW(process_name, NULL, NULL, NULL, 1, 0, NULL, NULL, NULL, process_info));
    assert(g_last_error == 50);
    for (size_t i = 0; i < sizeof(process_info); ++i) assert(!process_info[i]);
    const u16 command[] = {'p', ' ', '"', 'a', ' ', 'b', '"', 0};
    int command_argc = -1;
    u16 **command_args = (u16 **)(uintptr_t)impl_CommandLineToArgvW(command, &command_argc);
    u16 **other_args = (u16 **)(uintptr_t)impl_CommandLineToArgvW(command, &command_argc);
    assert(command_args && other_args && command_args != other_args && command_argc == 2);
    assert(command_args[0][0] == 'p' && !command_args[0][1]);
    assert(command_args[1][0] == 'a' && command_args[1][1] == ' ' && command_args[1][2] == 'b');
    assert(impl_LocalSize(command_args) >= 2 * sizeof(*command_args) + 12);
    assert(!impl_LocalFree(command_args) && !impl_LocalFree(other_args));
    assert(command[0] == 'p');
    assert(!impl_CommandLineToArgvW(command, NULL) && g_last_error == 87);
    assert(find_impl("RtlAddVectoredExceptionHandler") == (ImplFn)impl_AddVectoredExceptionHandler);
    assert(find_impl("RtlRemoveVectoredExceptionHandler") == (ImplFn)impl_RemoveVectoredExceptionHandler);
    typedef u64 (__attribute__((ms_abi)) *NativeVehAdd)(u32, BeerVectoredExceptionHandler);
    typedef u64 (__attribute__((ms_abi)) *NativeVehRemove)(u64);
    NativeVehAdd native_add = (NativeVehAdd)find_impl("RtlAddVectoredExceptionHandler");
    NativeVehRemove native_remove = (NativeVehRemove)find_impl("RtlRemoveVectoredExceptionHandler");
    beer_veh_reset();
    assert(!native_add(1, NULL));
    u64 native_handle = native_add(1, native_veh_probe);
    assert(native_handle && beer_veh_dispatch(&native_veh_calls));
    assert(native_veh_calls == 1);
    assert(impl_RemoveVectoredExceptionHandler(native_handle));
    assert(!native_remove(native_handle));
    native_handle = impl_AddVectoredExceptionHandler(0, native_veh_probe);
    assert(native_handle && !native_remove(native_handle + 1));
    assert(beer_veh_dispatch(&native_veh_calls) && native_veh_calls == 2);
    assert(native_remove(native_handle));
    assert(!beer_veh_dispatch(&native_veh_calls));
    char instruction_bytes[16] = {0};
    assert(!impl_NtFlushInstructionCache(UINT64_MAX, instruction_bytes, sizeof(instruction_bytes)));
    assert(!impl_NtFlushInstructionCache(UINT64_MAX, NULL, 0));
    assert(impl_NtFlushInstructionCache(1, NULL, 0) == 0xC0000008u);
    assert(impl_NtFlushInstructionCache(UINT64_MAX, (void *)(UINTPTR_MAX - 4), 8) == 0xC000000Du);
    assert(impl_NtIsProcessInJob(UINT64_MAX, 0) == 0x123u);
    assert(impl_NtIsProcessInJob(UINT64_MAX, 1) == 0xC0000008u);
    assert(impl_NtIsProcessInJob(1, 0) == 0xC0000008u);
    assert(find_impl("RtlCreateHeap") == (ImplFn)impl_RtlCreateHeap);
    assert(find_impl("RtlFreeHeap") == (ImplFn)impl_RtlFreeHeap);
    assert(find_impl("NtAllocateVirtualMemory") == (ImplFn)impl_NtAllocateVirtualMemory);
    assert(find_impl("ZwAllocateVirtualMemory") == (ImplFn)impl_NtAllocateVirtualMemory);
    assert(find_impl("NtFreeVirtualMemory") == (ImplFn)impl_NtFreeVirtualMemory);
    assert(beer_win32_syscall_register("NtAllocateVirtualMemory", (BeerWin32SyscallFn)impl_NtAllocateVirtualMemory) == 0x18);
    assert(beer_win32_syscall_register("ZwAllocateVirtualMemory", (BeerWin32SyscallFn)impl_NtAllocateVirtualMemory) == 0x18);
    void *mapping = NULL;
    u64 region = 4097;
    assert(!impl_NtAllocateVirtualMemory(UINT64_MAX, &mapping, 0, &region,
                                        BEER_MEM_RESERVE | BEER_MEM_COMMIT, BEER_PAGE_READWRITE));
    assert(mapping && !((uintptr_t)mapping & 65535) && region == 8192);
    for (u64 i = 0; i < region; ++i) assert(!((u8 *)mapping)[i]);
    void *saved = mapping;
    u64 saved_size = region;
    assert(impl_NtAllocateVirtualMemory(1, &mapping, 0, &region, BEER_MEM_COMMIT,
                                       BEER_PAGE_READWRITE) == 0xC0000008u);
    assert(mapping == saved && region == saved_size);
    assert(impl_NtAllocateVirtualMemory(UINT64_MAX, &mapping, 1, &region,
                                       BEER_MEM_COMMIT, BEER_PAGE_READWRITE) == 0xC00000BBu);
    region = UINT64_MAX;
    mapping = NULL;
    assert(impl_NtAllocateVirtualMemory(UINT64_MAX, &mapping, 0, &region,
                                       BEER_MEM_COMMIT, BEER_PAGE_READWRITE) == 0xC0000017u);
    assert(!mapping && region == UINT64_MAX);
    mapping = (u8 *)saved + 1;
    region = 1;
    assert(!impl_NtFreeVirtualMemory(UINT64_MAX, &mapping, &region, BEER_MEM_DECOMMIT));
    assert(mapping == saved && region == 4096);
    assert(!impl_NtAllocateVirtualMemory(UINT64_MAX, &mapping, 0, &region,
                                        BEER_MEM_COMMIT, BEER_PAGE_READWRITE));
    assert(!*(u8 *)mapping);
    assert(impl_NtFreeVirtualMemory(UINT64_MAX, &mapping, &region, BEER_MEM_RELEASE) == 0xC000000Du);
    region = 0;
    assert(!impl_NtFreeVirtualMemory(UINT64_MAX, &mapping, &region, BEER_MEM_RELEASE));
    assert(!mapping && !region);
    mapping = &saved;
    region = 1;
    assert(impl_NtFreeVirtualMemory(UINT64_MAX, &mapping, &region, BEER_MEM_DECOMMIT) == 0xC00000A0u);
    assert(mapping == &saved && region == 1);
    u64 heap = impl_RtlCreateHeap(2, NULL, 0x4000, 0, NULL, NULL);
    u64 other = impl_RtlCreateHeap(2, NULL, 0, 0, NULL, NULL);
    assert(heap && other && heap != other);
    assert(!impl_RtlCreateHeap(2, (void *)1, 0, 0, NULL, NULL));
    assert(!impl_RtlCreateHeap(2, NULL, 0, 0, (void *)1, NULL));
    assert(!impl_RtlCreateHeap(2, NULL, 0, 0, NULL, (void *)1));
    void *p = (void *)(uintptr_t)impl_RtlAllocateHeap(heap, 8, 32);
    assert(p && !((uintptr_t)p & 15));
    for (unsigned i = 0; i < 32; ++i) assert(!((u8 *)p)[i]);
    assert(impl_HeapSize(heap, 0, p) == 32);
    assert(!impl_RtlFreeHeap(other, 0, p));
    memset(p, 0x42, 32);
    p = (void *)(uintptr_t)impl_HeapReAlloc(heap, 8, p, 64);
    assert(p && impl_RtlSizeHeap(heap, 0, p) == 64);
    for (unsigned i = 0; i < 64; ++i) assert(((u8 *)p)[i] == (i < 32 ? 0x42 : 0));
    assert(!impl_RtlReAllocateHeap(heap, 0, p, UINT64_MAX));
    assert(impl_HeapFree(heap, 0, p));
    assert(!impl_RtlFreeHeap(heap, 0, p));
    p = (void *)(uintptr_t)impl_HeapAlloc(heap, 0, 24);
    assert(p && impl_RtlFreeHeap(heap, 0, p));
    assert(!impl_RtlAllocateHeap(1, 0, 24));
    assert(impl_RtlSizeHeap(1, 0, (void *)1) == UINT64_MAX);
    assert(impl_RtlDestroyHeap(1) == 1);
    assert(!impl_RtlDestroyHeap(heap));
    assert(!impl_RtlDestroyHeap(other));
    u64 process = impl_GetProcessHeap();
    p = (void *)(uintptr_t)impl_HeapAlloc(process, 0, 17);
    assert(p && impl_RtlSizeHeap(process, 0, p) == 17);
    assert(impl_RtlFreeHeap(process, 0, p));
    assert(!impl_RtlFreeHeap(process, 0, p));
    assert(!impl_RtlAllocateHeap(process, 0, UINT64_MAX));
    assert(impl_RtlDestroyHeap(process) == process);
    assert(beer_static_tls_slot((size_t)(&g_guest_dlls[0] - g_guest_dlls), TLS_SLOTS) == 1);
    assert(beer_static_tls_slot((size_t)(&g_guest_dlls[31] - g_guest_dlls), TLS_SLOTS) == 32);
    /* Build the actual synthetic PE images and walk their real export table.
     * Names must remain PE-sorted while addresses follow service ID order. */
    g_tramp = mmap(NULL, (size_t)MAX_STUBS * THUNK_SZ, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(g_tramp != MAP_FAILED);
    /* The real loader constructor already maps these headers before main. */
    for (size_t i = 0; i < sizeof(g_fake_pe_modules) / sizeof(*g_fake_pe_modules); ++i)
        assert(*(const u16 *)(uintptr_t)g_fake_pe_modules[i].base == MZ_MAGIC);
    /* A test-only unknown import must reserve its service slot before layout. */
    g_snames[g_nstubs++] = strdup("ntdll.dll!NtBeerUnsupportedTest");
    g_snames[g_nstubs++] = strdup("shell32.dll!CommandLineToArgvW");
    fake_pe_exports_build();
    assert(g_fake_pe_ready);
    late_loader_list_test();
    int before_missing_lookup = g_nstubs;
    assert(!fake_pe_export_address(HMOD_K32, "InitializeProcThreadAttributeList"));
    assert(!impl_GetProcAddress(HMOD_K32, "InitializeProcThreadAttributeList") && g_last_error == 127);
    assert(!impl_GetProcAddress(HMOD_K32, "UpdateProcThreadAttribute") && g_last_error == 127);
    assert(!impl_GetProcAddress(HMOD_USER32, "BeerAbsentExportTest") && g_last_error == 127);
    assert(g_nstubs == before_missing_lookup);
    FakePeModule *native = fake_pe_module_from_base(HMOD_NTDLL);
    assert(fake_pe_export_address(HMOD_NTDLL, "RtlAddVectoredExceptionHandler"));
    assert(fake_pe_export_address(HMOD_NTDLL, "RtlRemoveVectoredExceptionHandler"));
    for (size_t i = 1; i < native->export_count; ++i)
        assert(strcmp(native->exports[i - 1], native->exports[i]) < 0);
    size_t count = beer_win32_syscall_count();
    for (size_t i = 0; i < count; ++i) {
        u64 nt = fake_pe_export_address(HMOD_NTDLL, beer_win32_syscall_name((u32)i, 0));
        u64 zw = fake_pe_export_address(HMOD_NTDLL, beer_win32_syscall_name((u32)i, 1));
        assert(nt == HMOD_NTDLL + FAKE_PE_TEXT_RVA + i * FAKE_PE_STUB_SLOT);
        assert(zw == nt);
        const u8 *stub = (const u8 *)(uintptr_t)nt;
        u32 encoded;
        memcpy(&encoded, stub + 4, sizeof(encoded));
        assert(stub[0] == 0x4c && stub[1] == 0x8b && stub[2] == 0xd1 && stub[3] == 0xb8);
        assert(encoded == i);
    }
    assert(!beer_win32_syscall_supported(1));
    u64 missing = fake_pe_export_address(HMOD_NTDLL, "NtWorkerFactoryWorkerReady");
    assert(((u64 (__attribute__((ms_abi)) *)(void))(uintptr_t)missing)() == BEER_STATUS_NOT_IMPLEMENTED);
    assert(!fake_pe_export_address(HMOD_NTDLL, "NtNotAnExport"));
    u64 imported = fake_pe_export_address(HMOD_NTDLL, "NtBeerUnsupportedTest");
    assert(imported && ((u64 (__attribute__((ms_abi)) *)(void))(uintptr_t)imported)()
        == BEER_STATUS_NOT_IMPLEMENTED);
    assert(fake_pe_export_address(HMOD_NTDLL, "RtlCreateHeap") >=
        HMOD_NTDLL + FAKE_PE_TEXT_RVA + count * FAKE_PE_STUB_SLOT);
    puts("native loader ABI/interoperability: PASS");
    return 0;
}