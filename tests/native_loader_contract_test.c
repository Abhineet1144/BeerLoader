/* Exercise the real guest ABI wrappers and Kernel32/native interoperability,
 * not a copy of the loader's dispatch logic. Unused loader code is discarded. */
#define main beer_loader_unused_main
#include "../loader.c"
#undef main
#include <assert.h>
#include "pe_version_fixture.h"
#include "wow32_dll_contract.inc"

static int native_veh_calls;
static int steam_auth_callback_calls;
static void __attribute__((ms_abi)) steam_auth_test_callback(void *data)
{
    int32_t availability;
    memcpy(&availability, data, 4);
    assert(availability == 100);
    assert(!strcmp((const char *)data + 4, "OK"));
    ++steam_auth_callback_calls;
}

static void steam_auth_callback_contract_test(void)
{
    struct { u8 location[512]; u64 guard; } ping = { .guard = UINT64_C(0xabcdef0123456789) };
    memset(ping.location, 0xa5, sizeof(ping.location));
    assert(net_utils_local_ping(0, ping.location) == -1.0f);
    for (size_t i = 0; i < sizeof(ping.location); ++i) assert(!ping.location[i]);
    assert(ping.guard == UINT64_C(0xabcdef0123456789));
    assert((s64)net_utils_estimate_ping(0, ping.location, ping.location) == -2);
    assert((s64)net_utils_estimate_local_ping(0, ping.location) == -2);
    assert(!net_utils_ping_fresh(0, 30.0f));
    char text[2] = {'x', 'y'};
    net_utils_ping_string(0, ping.location, text, 1);
    assert(!text[0] && text[1] == 'y');
    assert(!net_utils_parse_ping(0, "invalid", ping.location));
    u32 via = UINT32_MAX;
    assert((s64)net_utils_ping_dc(0, 0, &via) == -2 && !via);
    assert((s64)net_utils_ping_pop(0, 0) == -2);
    u64 fn = (u64)(uintptr_t)steam_auth_test_callback;
    assert(!net_utils_set_config(0, 202, 3, 0, 5, &fn));
    assert(!net_utils_set_config(0, 202, 1, 0, 1, &fn));
    assert(net_utils_set_config(0, 202, 1, 0, 5, &fn));
    struct { u64 value, guard; } result = {0, UINT64_C(0xabcdef0123456789)};
    u64 size = 4;
    s32 type = 0;
    assert((s64)net_utils_get_config(0, 202, 1, 0, &type, &result.value, &size) == -3);
    assert(size == 8 && type == 5 && !result.value && result.guard == UINT64_C(0xabcdef0123456789));
    assert(net_utils_get_config(0, 202, 1, 0, &type, &result.value, &size) == 1 && result.value == fn);
    u8 auth[BEER_STEAM_AUTH_SIZE] = {0};
    const s32 current = 100;
    memcpy(auth, &current, 4);
    memcpy(auth + 4, "OK", 3);
    steam_networking_dispatch_auth(auth);
    assert(steam_auth_callback_calls == 1);
    assert(net_utils_set_config(0, 202, 1, 0, 5, NULL));
    u32 before = g_steam_event_count;
    steam_networking_dispatch_auth(auth);
    assert(g_steam_event_count == before + 1);
    assert(g_steam_events[before].id == 1222 && g_steam_events[before].size == sizeof(auth));
    assert(!memcmp(g_steam_events[before].data, auth, sizeof(auth)));
    g_steam_event_count = before;
}
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

static void winsock_ordinal_contract_test(void)
{
    for (u32 ordinal = 1; ordinal <= UINT16_MAX; ++ordinal) {
        ImplFn implementation = winsock_ordinal_impl((u16)ordinal);
        const char *name = winsock_ordinal_name((u16)ordinal);
        assert((implementation != NULL) == (name != NULL));
        if (name) assert(find_impl(name) == implementation);
    }
}

static void wldap_ordinal_contract_test(void)
{
    for (u32 ordinal = 1; ordinal <= UINT16_MAX; ++ordinal) {
        ImplFn implementation = wldap_ordinal_impl((u16)ordinal);
        const char *name = wldap_ordinal_name((u16)ordinal);
        assert((implementation != NULL) == (name != NULL));
        if (name) assert(find_impl(name) == implementation);
    }
    assert(wldap_ordinal_impl(22) == (ImplFn)impl_ldap_err2string);
    assert(wldap_ordinal_impl(26) == (ImplFn)impl_ldap_first_entry);
    assert(wldap_ordinal_impl(27) == (ImplFn)impl_ldap_next_entry);
    assert(wldap_ordinal_impl(30) == (ImplFn)impl_ldap_get_dn);
    assert(wldap_ordinal_impl(32) == (ImplFn)impl_ldap_first_attribute);
    assert(wldap_ordinal_impl(33) == (ImplFn)impl_ldap_next_attribute);
    assert(wldap_ordinal_impl(35) == (ImplFn)impl_ldap_get_values_len);
    assert(wldap_ordinal_impl(41) == (ImplFn)impl_ldap_msgfree);
    assert(wldap_ordinal_impl(46) == (ImplFn)impl_ldap_unbind_s);
    assert(wldap_ordinal_impl(50) == (ImplFn)impl_ldap_result);
    assert(wldap_ordinal_impl(60) == (ImplFn)impl_ldap_simple_bind_s);
    assert(wldap_ordinal_impl(143) == (ImplFn)impl_ldap_init);
    assert(wldap_ordinal_impl(211) == (ImplFn)impl_ldap_set_option);
    assert(wldap_ordinal_impl(79) == (ImplFn)impl_ldap_value_free_len);
    assert(wldap_ordinal_impl(200) == (ImplFn)impl_ldap_memfree);
    assert(wldap_ordinal_impl(301) == (ImplFn)impl_ber_free);
}

static void native_crt_import_ownership_test(void)
{
    /* A DLL's CRT API-set imports must use the loaded UCRT even when Beer has
     * a same-named fallback (setlocale's fallback only supports C). */
    int saved_count = g_guest_dll_count;
    assert(saved_count < MAX_GUEST_DLLS);
    GuestDll saved = g_guest_dlls[saved_count];
    GuestDll *runtime = &g_guest_dlls[g_guest_dll_count++];
    memset(runtime, 0, sizeof(*runtime));
    strcpy(runtime->name, "ucrtbase.dll");
    runtime->image_size = 4096;
    runtime->image = calloc(1, runtime->image_size);
    GuestDll dependent = { .image_size = 4096 };
    dependent.image = calloc(1, dependent.image_size);
    assert(runtime->image && dependent.image);
    GuestDll *images[] = {runtime, &dependent};
    for (size_t i = 0; i < 2; ++i) {
        DosHdr *dos = (DosHdr *)images[i]->image;
        dos->magic = MZ_MAGIC;
        dos->lfanew = sizeof(*dos);
        NtHdrs64 *nt = (NtHdrs64 *)(images[i]->image + dos->lfanew);
        nt->sig = PE_SIG;
        nt->opt.magic = PE32PLUS;
        nt->opt.sz_image = 4096;
    }
    NtHdrs64 *nt = (NtHdrs64 *)(runtime->image + sizeof(DosHdr));
    nt->opt.dirs[0 /* IMAGE_DIRECTORY_ENTRY_EXPORT */].rva = 512;
    nt->opt.dirs[0].size = 128;
    /* IMAGE_EXPORT_DIRECTORY: counts +20/+24, EAT/name/ordinal RVAs +28/32/36. */
    u32 *exports = (u32 *)(runtime->image + 512);
    exports[5] = exports[6] = 1;
    exports[7] = 640;
    exports[8] = 648;
    exports[9] = 656;
    *(u32 *)(runtime->image + 640) = 1024;
    *(u32 *)(runtime->image + 648) = 700;
    strcpy((char *)runtime->image + 700, "setlocale");
    assert(guest_dll_export(runtime, "setlocale") == (u64)(uintptr_t)(runtime->image + 1024));
    nt = (NtHdrs64 *)(dependent.image + sizeof(DosHdr));
    nt->opt.dirs[DIR_IMPORT].rva = 512;
    nt->opt.dirs[DIR_IMPORT].size = 2 * sizeof(ImportDesc);
    ImportDesc *import = (ImportDesc *)(dependent.image + 512);
    import->name_rva = 640;
    import->orig_ilt = 768;
    import->iat_rva = 800;
    strcpy((char *)dependent.image + 640, "api-ms-win-crt-locale-l1-1-0.dll");
    *(u64 *)(dependent.image + 768) = 704;
    strcpy((char *)dependent.image + 706, "setlocale");
    u8 *saved_tramp = g_tramp;
    u64 saved_trampsz = g_trampsz;
    int saved_stubs = g_nstubs, saved_pending = g_pending_dependency_count;
    const char *saved_name = g_snames[saved_stubs];
    u64 saved_target = g_impl_targets[saved_stubs];
    int saved_ready = g_fake_pe_ready;
    g_fake_pe_ready = 0;
    g_tramp = calloc(1, 4096);
    g_trampsz = 4096;
    assert(g_tramp);
    guest_dll_resolve_imports(&dependent);
    assert(*(u64 *)(dependent.image + 800) == (u64)(uintptr_t)(runtime->image + 1024));
    free((void *)g_snames[saved_stubs]);
    g_snames[saved_stubs] = saved_name;
    g_impl_targets[saved_stubs] = saved_target;
    g_nstubs = saved_stubs;
    g_pending_dependency_count = saved_pending;
    free(g_tramp);
    g_tramp = saved_tramp;
    g_trampsz = saved_trampsz;
    g_fake_pe_ready = saved_ready;
    free(runtime->image);
    free(dependent.image);
    *runtime = saved;
    g_guest_dll_count = saved_count;
}

static void locale_info_contract_test(void)
{
    const u16 english[] = {'e','n','-','U','S',0};
    const u16 japanese[] = {'j','a','-','J','P',0};
    const struct { u32 type, english, japanese; } numbers[] = {
        {0x0001, 0x0409, 0x0411}, /* ILANGUAGE is hexadecimal in text form. */
        {0x0009, 0x0409, 0x0411}, /* IDEFAULTLANGUAGE */
        {0x000b, 437, 932},       /* IDEFAULTCODEPAGE (OEM, not ANSI) */
        {0x1004, 1252, 932},      /* IDEFAULTANSICODEPAGE */
        {0x1008, 10000, 10001},   /* IDEFAULTMACCODEPAGE */
        {0x1009, 1, 1},           /* ICALENDARTYPE */
        {0x0019, 2, 0},           /* ICURRDIGITS */
        {0x0022, 0, 2},           /* ILDATE */
        {0x005b, 244, 122},       /* IGEOID */
    };
    for (size_t i = 0; i < sizeof(numbers) / sizeof(*numbers); ++i) {
        for (int jp = 0; jp < 2; ++jp) {
            u32 expected = jp ? numbers[i].japanese : numbers[i].english;
            u32 type = numbers[i].type | 0x20000000u;
            struct { u32 value, guard; } out = {0xdeadbeef, 0x12345678};
            assert(impl_GetLocaleInfoEx((u64)(uintptr_t)(jp ? japanese : english), type,
                                       (u16 *)&out.value, 2) == 2);
            assert(out.value == expected && out.guard == 0x12345678);
            assert(impl_GetLocaleInfoW(jp ? 0x0411 : 0x0409, type,
                                      (u16 *)&out.value, 2) == 2);
            assert(out.value == expected && out.guard == 0x12345678);
            assert(impl_GetLocaleInfoA(jp ? 0x0411 : 0x0409, type,
                                      (char *)&out.value, 4) == 4);
            assert(out.value == expected && out.guard == 0x12345678);
        }
    }
    u32 value = 0xdeadbeef;
    assert(impl_GetLocaleInfoEx(0, 0xa0001004u, (u16 *)&value, 2) == 2);
    assert(value == 1252); /* NOUSEROVERRIDE must not change the LCTYPE. */
    assert(impl_GetLocaleInfoEx(0, 0x20001004u, NULL, 0) == 2);
    assert(impl_GetLocaleInfoA(0x0409, 0x20001004u, NULL, 0) == 4);
    assert(!impl_GetLocaleInfoEx(0, 0x20001004u, (u16 *)&value, 1));
    assert(g_last_error == 122 && value == 1252);
    assert(!impl_GetLocaleInfoEx(0, 0x2000ffffu, (u16 *)&value, 2));
    assert(g_last_error == 1004 && value == 1252);
    assert(!impl_GetLocaleInfoEx(0, 0x2000000eu, (u16 *)&value, 2));
    assert(g_last_error == 1004 && value == 1252); /* SDECIMAL is not numeric. */
    assert(!impl_GetLocaleInfoEx(0, 0x1004, NULL, 2) && g_last_error == 87);
    assert(!impl_GetLocaleInfoW(0x0409, 0x1004, (u16 *)&value, -1) && g_last_error == 87);
    assert(!impl_GetLocaleInfoA(0x0409, 0x1004, (char *)&value, -1) && g_last_error == 87);
    u16 text[16];
    assert(impl_GetLocaleInfoEx((u64)(uintptr_t)english, 0x1001, text, 16) == 8);
    const char *expected = "English";
    for (size_t i = 0; i <= strlen(expected); ++i) assert(text[i] == (u8)expected[i]);
}

static u64 callback_test_low, callback_test_high, callback_test_import;
static unsigned callback_test_visits;
static u64 __attribute__((ms_abi)) callback_stack_probe(u64 hw, u32 msg, u64 wp, u64 lp,
                                                       u64 id, u64 ref)
{
    u64 local;
    WinTEB *teb;
    __asm__ volatile("mov %%gs:0x30, %0" : "=r"(teb));
    assert((u64)(uintptr_t)&local >= callback_test_low && (u64)(uintptr_t)&local < callback_test_high);
    assert(teb->StackLimit == callback_test_low && teb->StackBase == callback_test_high);
    assert(hw == 0xd0001234 && msg == 0x8123 && wp == 0xfedcba9876543210ull);
    assert(lp == 0x1234567887654321ull && id == 0xabcdef0123456789ull && ref == UINT64_MAX);
    ++callback_test_visits;
    if (callback_test_visits == 1) {
        /* A real import reenters Beer while this callback is still live. */
        u64 result = ((u64 (__attribute__((ms_abi)) *)(void))(uintptr_t)callback_test_import)();
        assert(result == 0x8765432112345678ull);
        assert(teb->StackLimit == callback_test_low && teb->StackBase == callback_test_high);
    }
    return 0x8765432112345678ull;
}
static u64 __attribute__((ms_abi)) callback_host_entry(void)
{
    return call_window_callback6((u64)(uintptr_t)callback_stack_probe, 0xd0001234, 0x8123,
                                 0xfedcba9876543210ull, 0x1234567887654321ull,
                                 0xabcdef0123456789ull, UINT64_MAX);
}
static void window_callback_stack_test(void)
{
    u64 old_gs;
    assert(!syscall(SYS_arch_prctl, ARCH_GET_GS, &old_gs));
    WinTEB teb = {0};
    teb.Self = (u64)(uintptr_t)&teb;
    void *guest = mmap(NULL, HOST_CALL_STACK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void *host = mmap(NULL, HOST_CALL_STACK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(guest != MAP_FAILED && host != MAP_FAILED);
    callback_test_low = (u64)(uintptr_t)guest;
    callback_test_high = callback_test_low + HOST_CALL_STACK_SIZE;
    g_host_call_stack_base = (u64)(uintptr_t)host;
    g_host_call_stack_top = g_host_call_stack_real_top = g_host_call_stack_base + HOST_CALL_STACK_SIZE - 256;
    g_teb_guest_stack_limit = callback_test_low;
    g_teb_guest_stack_base = callback_test_high;
    g_saved_guest_rsp = callback_test_high - 256;
    teb.StackLimit = g_host_call_stack_base;
    teb.StackBase = g_host_call_stack_top + 256;
    assert(!syscall(SYS_arch_prctl, ARCH_SET_GS, &teb));
    int slot = g_nstubs++;
    g_snames[slot] = strdup("callback-test!nested");
    emit_impl_thunk(slot, (u64)(uintptr_t)callback_host_entry);
    callback_test_import = (u64)(uintptr_t)(g_tramp + slot * THUNK_SZ);
    u64 top = g_host_call_stack_top, saved = g_saved_guest_rsp;
    assert(call_window_callback_on_stack((u64)(uintptr_t)callback_host_entry,
                                        0, 0, 0, 0, 0, 0, top - 256) == 0x8765432112345678ull);
    assert(callback_test_visits == 2 && g_host_call_stack_top == top && g_saved_guest_rsp == saved);
    assert(teb.StackLimit == g_host_call_stack_base && teb.StackBase == top + 256);
    assert(!syscall(SYS_arch_prctl, ARCH_SET_GS, old_gs));
    g_host_call_stack_base = g_host_call_stack_top = g_host_call_stack_real_top = 0;
    g_saved_guest_rsp = g_teb_guest_stack_limit = g_teb_guest_stack_base = 0;
    munmap(guest, HOST_CALL_STACK_SIZE);
    munmap(host, HOST_CALL_STACK_SIZE);
}

static unsigned subclass_test_calls;
static u64 __attribute__((ms_abi)) subclass_original(u64 hw, u32 msg, u64 wp, u64 lp)
{
    assert(hw && msg && wp == 3 && lp == 4);
    ++subclass_test_calls;
    return 0xfedcba9876543210ull;
}
static u64 __attribute__((ms_abi)) subclass_probe(u64 hw, u32 msg, u64 wp, u64 lp, u64 id, u64 ref)
{
    assert(id == 0x1234567887654321ull && ref == 0xabcdef0123456789ull);
    ++subclass_test_calls;
    assert(impl_RemoveWindowSubclass(hw, (u64)(uintptr_t)subclass_probe, id));
    return impl_DefSubclassProc(hw, msg, wp, lp);
}
static void subclass_contract_test(void)
{
    BeerWindow *w = beer_window_new();
    assert(w);
    w->owner_tid = (u32)current_windows_thread_id();
    w->proc = (u64)(uintptr_t)subclass_original;
    u64 hw = w->hwnd, ref = 0;
    const u64 proc = (u64)(uintptr_t)subclass_probe, id = 0x1234567887654321ull;
    assert(!impl_SetWindowSubclass(0, proc, id, 0));
    assert(!impl_SetWindowSubclass(hw, 0, id, 0));
    assert(impl_SetWindowSubclass(hw, proc, id, 5));
    assert(impl_SetWindowSubclass(hw, proc, id, 0xabcdef0123456789ull));
    assert(impl_GetWindowSubclass(hw, proc, id, &ref) && ref == 0xabcdef0123456789ull);
    assert(call_window_proc(hw, 0x8001, 3, 4) == 0xfedcba9876543210ull);
    assert(subclass_test_calls == 2 && !impl_GetWindowSubclass(hw, proc, id, &ref));
    assert(w->proc == (u64)(uintptr_t)subclass_original && !g_subclass_windows);
    assert(impl_SetWindowSubclass(hw, proc, id, 0xabcdef0123456789ull));
    assert(call_window_proc(hw, 0x82, 3, 4) == 0xfedcba9876543210ull);
    assert(subclass_test_calls == 4 && !g_subclass_windows);
    beer_window_free(hw);
}

static unsigned notify_calls;
static u32 notify_expected_tid;
static u64 __attribute__((ms_abi)) notify_probe(u64 hw, u32 msg, u64 wp, u64 lp)
{
    assert(hw && msg == 0x8042 && wp == 3 && lp == 4);
    assert(current_windows_thread_id() == notify_expected_tid);
    ++notify_calls;
    return 0; /* Successful notification must still return TRUE. */
}
typedef struct { u64 hwnd; pthread_barrier_t ready, pump; } NotifyWorker;
static void *notify_worker(void *arg)
{
    NotifyWorker *worker = arg;
    BeerWindow *w = beer_window_find(worker->hwnd);
    assert(w);
    w->owner_tid = (u32)current_windows_thread_id();
    notify_expected_tid = w->owner_tid;
    pthread_barrier_wait(&worker->ready);
    pthread_barrier_wait(&worker->pump);
    WIN_MSG msg;
    /* Sent notifications execute despite a nonmatching posted-message filter
     * and do not appear in the posted queue. */
    assert(!impl_PeekMessageW(&msg, 0, 0x9000, 0x9001, 1));
    return NULL;
}
static void notify_message_contract_test(void)
{
    assert(find_impl("SendNotifyMessageW") == (ImplFn)impl_SendNotifyMessageW);
    int exported = 0;
    for (size_t i = 0; i < sizeof(beer_module_exports) / sizeof(*beer_module_exports); ++i)
        if (!strcmp(beer_module_exports[i][0], "user32.dll") &&
            !strcmp(beer_module_exports[i][1], "SendNotifyMessageW")) exported = 1;
    assert(exported);
    BeerWindow *w = beer_window_new();
    assert(w);
    const u64 hw = w->hwnd;
    w->owner_tid = notify_expected_tid = (u32)current_windows_thread_id();
    w->proc = (u64)(uintptr_t)notify_probe;
    assert(impl_SendNotifyMessageW(hw, 0x8042, 3, 4) == 1 && notify_calls == 1);
    assert(!impl_SendNotifyMessageW(0x1234, 0x8042, 3, 4) && g_last_error == 1400);
    assert(!impl_SendNotifyMessageW(hw, 0x000c, 0, 1) && g_last_error == 1159);
    assert(impl_SendNotifyMessageW(0xffff, 0x8042, 3, 4) == 1 && notify_calls == 2);
    w->style = 0x40000000u;
    assert(impl_SendNotifyMessageW(0xffff, 0x8042, 3, 4) == 1 && notify_calls == 2);
    w->style = 0;
    w->parent = (u64)-3;
    assert(impl_SendNotifyMessageW(0xffff, 0x8042, 3, 4) == 1 && notify_calls == 2);
    w->parent = 0;
    NotifyWorker worker = {.hwnd = hw};
    pthread_barrier_init(&worker.ready, NULL, 2);
    pthread_barrier_init(&worker.pump, NULL, 2);
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, notify_worker, &worker));
    pthread_barrier_wait(&worker.ready);
    assert(impl_SendNotifyMessageW(hw, 0x8042, 3, 4) == 1);
    assert(notify_calls == 2); /* No wrong-thread fallback, no pumping required. */
    assert(g_sent_head && g_sent_head->notify);
    pthread_barrier_wait(&worker.pump);
    pthread_join(thread, NULL);
    assert(notify_calls == 3 && !g_sent_head);
    pthread_barrier_destroy(&worker.ready);
    pthread_barrier_destroy(&worker.pump);
    beer_window_free(hw);
}

static _Atomic unsigned mm_timer_test_calls;
static u64 mm_timer_test_done;
static void __attribute__((ms_abi)) mm_timer_test_callback(u32 id, u32 message, u64 user, u64 a, u64 b)
{
    assert(beer_win32_syscall_selector == 1);
    beer_win32_syscall_selector = 0; /* Test function is host code, not guest imports. */
    assert(id && !message && user == 0xabcdef0123456789ull && !a && !b);
    WinTEB *teb;
    __asm__ volatile("mov %%gs:0x30, %0" : "=r"(teb));
    u64 local;
    assert(teb && (u64)(uintptr_t)&local >= teb->StackLimit && (u64)(uintptr_t)&local < teb->StackBase);
    atomic_fetch_add(&mm_timer_test_calls, 1);
    assert(!impl_timeKillEvent(id)); /* Synchronous self-cancellation must not deadlock. */
    impl_SetEvent(mm_timer_test_done);
}
static void multimedia_timer_contract_test(void)
{
    /* Prepared workers inspect the loaded executable's TLS directory. */
    static u8 image[sizeof(DosHdr) + sizeof(NtHdrs64)];
    u8 *saved_image = g_img;
    ((DosHdr *)image)->lfanew = sizeof(DosHdr);
    g_img = image;
    assert(!impl_timeSetEvent(0, 0, 1, 0, 0));
    assert(!impl_timeSetEvent(1, 0, 0, 0, 0));
    assert(!impl_timeSetEvent(1, 0, 1, 0, 0x30));
    assert(impl_timeKillEvent(0) == 11);
    mm_timer_test_done = impl_CreateEventW(0, 1, 0, 0);
    assert(mm_timer_test_done);
    u64 id = impl_timeSetEvent(5, 0, (u64)(uintptr_t)mm_timer_test_callback,
                               0xabcdef0123456789ull, 0x101);
    assert(id && impl_WaitForSingleObject(mm_timer_test_done, 2000) == 0);
    /* Self-cancellation is already committed even if the worker has not yet
     * retired the callback's record. No subsequent callback is eligible. */
    assert(impl_timeKillEvent((u32)id) == 11);
    assert(atomic_load(&mm_timer_test_calls) == 1);
    assert(impl_ResetEvent(mm_timer_test_done));
    id = impl_timeSetEvent(5, 0, mm_timer_test_done, 0, 0x111);
    assert(id && impl_WaitForSingleObject(mm_timer_test_done, 2000) == 0);
    assert(!impl_timeKillEvent((u32)id));
    assert(impl_timeKillEvent((u32)id) == 11);
    assert(impl_ResetEvent(mm_timer_test_done));
    assert(impl_WaitForSingleObject(mm_timer_test_done, 25) == 0x102);
    id = impl_timeSetEvent(1000, 0, mm_timer_test_done, 0, 0x10);
    assert(id && !impl_timeKillEvent((u32)id)); /* Cancel before first expiry. */
    assert(impl_WaitForSingleObject(mm_timer_test_done, 25) == 0x102);
    assert(impl_CloseHandle(mm_timer_test_done));
    g_img = saved_image;
}

static void legacy_dns_contract_test(void)
{
        assert(offsetof(WinHostEnt, address_type) == 16 && offsetof(WinHostEnt, address_list) == 24);
        struct sockaddr_in addresses[2] = {{ .sin_family = AF_INET }, { .sin_family = AF_INET }};
        assert(inet_pton(AF_INET, "192.0.2.10", &addresses[0].sin_addr) == 1);
        assert(inet_pton(AF_INET, "192.0.2.11", &addresses[1].sin_addr) == 1);
        struct addrinfo entries[3] = {
                { .ai_family = AF_INET6 },
                { .ai_family = AF_INET, .ai_addrlen = sizeof(addresses[0]),
                    .ai_addr = (struct sockaddr *)&addresses[0], .ai_canonname = "canonical.example.test" },
                { .ai_family = AF_INET, .ai_addrlen = sizeof(addresses[1]),
                    .ai_addr = (struct sockaddr *)&addresses[1] }
        };
        entries[0].ai_next = &entries[1];
        entries[1].ai_next = &entries[2];
        WinHostEnt *host = (WinHostEnt *)(uintptr_t)win_hostent_from_addrinfo("alias.example.test", entries);
        assert(host && host->address_type == 2 && host->address_length == 4 && !host->padding);
        assert(!strcmp((char *)(uintptr_t)host->name, "canonical.example.test"));
        assert(!*(u64 *)(uintptr_t)host->aliases);
        u64 *list = (u64 *)(uintptr_t)host->address_list;
        assert(list[0] && list[1] && !list[2]);
        assert(!memcmp((void *)(uintptr_t)list[0], &addresses[0].sin_addr, 4));
        assert(!memcmp((void *)(uintptr_t)list[1], &addresses[1].sin_addr, 4));
        assert(list[0] != (u64)(uintptr_t)&addresses[0].sin_addr);
        assert(!win_hostent_from_addrinfo("empty.example.test", NULL) && g_wsa_last_error == 11004);
        u8 startup[408];
        assert(!impl_WSAStartup(0x0202, startup));
        host = (WinHostEnt *)(uintptr_t)impl_gethostbyname("LOCALHOST");
        assert(host && host->address_type == 2 && host->address_length == 4);
        assert(impl_gethostbyname("127.0.0.1"));
        assert(!impl_gethostbyname(NULL) && g_wsa_last_error == 10014);
        assert(!impl_WSACleanup());
        free(g_win_hostent_storage);
        g_win_hostent_storage = NULL;
}

static void wow32_dib_contract_test(void)
{
    u32 enum_args[] = {UINT32_MAX, 0, 0};
    assert(!w32_EnumThreadWindows(enum_args) && g_last_error == 87);
    enum_args[1] = 0x1234;
    assert(w32_EnumThreadWindows(enum_args)); /* No windows belonging to this thread. */
    struct {
        u32 info[13], bits, guard;
        u32 object[21], object_guard;
    } *b = w32_heap_alloc(sizeof(*b), 1);
    assert(b);
    b->guard = b->object_guard = 0xa1b2c3d4;
    b->info[0] = 40; b->info[1] = 3;
    u32 args[6] = {0, (u32)(uintptr_t)b->info, 0, (u32)(uintptr_t)&b->bits, 0, 0};
    u64 dc = beer_CreateCompatibleDC(0);
    assert(dc);
    for (unsigned mode = 0; mode < 4; ++mode) {
        int topdown = mode & 1, bpp = mode & 2 ? 24 : 32;
        b->info[2] = topdown ? (u32)-2 : 2;
        b->info[3] = 1u | ((u32)bpp << 16);
        u64 bitmap = w32_CreateDIBSection(args);
        assert(bitmap && b->bits && b->guard == 0xa1b2c3d4);
        const size_t stride = 12;
        u8 *pixels = (void *)(uintptr_t)b->bits;
        for (size_t i = 0; i < 24; ++i) assert(!pixels[i]);
        u64 previous = beer_SelectObject(dc, bitmap);
        u32 colors[2] = {0xa1b2c3d4, 0x12345678};
        assert(!beer_GetDIBColorTable(dc, 0, 2, colors));
        assert(colors[0] == 0xa1b2c3d4 && colors[1] == 0x12345678);
        size_t first_row = topdown ? 0 : stride;
        pixels[first_row] = 0x33;
        pixels[first_row + 1] = 0x22;
        pixels[first_row + 2] = 0x11;
        assert(beer_GetPixel(dc, 0, 0) == 0x332211);
        assert(beer_SetPixel(dc, 1, 0, 0x665544) == 0x665544);
        size_t second_pixel = first_row + bpp / 8;
        assert(pixels[second_pixel] == 0x66 && pixels[second_pixel + 1] == 0x55 &&
               pixels[second_pixel + 2] == 0x44);
        u32 object_args[] = {(u32)bitmap, 84, (u32)(uintptr_t)b->object};
        assert(w32_GetObjectW(object_args) == 84);
        assert(b->object[5] == b->bits && b->object[6] == 40);
        assert(b->object[7] == 3 && b->object[8] == b->info[2]);
        assert(b->object_guard == 0xa1b2c3d4);
        beer_SelectObject(dc, previous);
        assert(beer_DeleteObject(bitmap));
        unsigned char resident;
        errno = 0;
        assert(mincore(pixels, 4096, &resident) == -1 && errno == ENOMEM);
    }
    b->info[2] = 0x80000000u;
    b->bits = 123;
    assert(!w32_CreateDIBSection(args) && !b->bits && b->guard == 0xa1b2c3d4);
    b->info[2] = 2;
    args[4] = 1; /* Do not silently ignore section-backed requests. */
    assert(!w32_CreateDIBSection(args) && g_last_error == 50 && !b->bits);
    args[4] = 0;
    args[5] = 123; /* Ignored for anonymous DIBs. */
    u64 offset_bitmap = w32_CreateDIBSection(args);
    assert(offset_bitmap && beer_DeleteObject(offset_bitmap));
    args[5] = 0;
    b->info[4] = 3;
    assert(!w32_CreateDIBSection(args) && g_last_error == 50);
    b->info[10] = 0xff0000; b->info[11] = 0xff00; b->info[12] = 0xff;
    u64 bitmap = w32_CreateDIBSection(args);
    assert(bitmap && beer_DeleteObject(bitmap));
    assert(beer_DeleteDC(dc));
    /* Native allocator path still creates/deletes a normal live DIB. */
    void *native_bits = NULL;
    b->info[4] = 0;
    bitmap = beer_CreateDIBSection(0, b->info, 0, &native_bits, 0, 0);
    assert(bitmap && native_bits && beer_DeleteObject(bitmap));
    w32_heap_free(b);
}

static void wow32_sid_contract_test(void)
{
    u32 *b = w32_heap_alloc(32, 1);
    assert(b);
    const u8 authority[6] = {0, 0, 0, 0, 0, 5};
    memcpy(b, authority, 6);
    b[5] = 0xa1b2c3d4;
    u32 args[11] = {(u32)(uintptr_t)b, 2, 32, 544, 0, 0, 0, 0, 0, 0,
                    (u32)(uintptr_t)(b + 4)};
    assert(w32_AllocateAndInitializeSid(args));
    assert(b[4] && b[5] == 0xa1b2c3d4);
    const u8 *sid = (void *)(uintptr_t)b[4];
    assert(sid[0] == 1 && sid[1] == 2 && !memcmp(sid + 2, authority, 6));
    u32 sub[2];
    memcpy(sub, sid + 8, sizeof(sub));
    assert(sub[0] == 32 && sub[1] == 544);
    u32 member_args[] = {0, b[4], (u32)(uintptr_t)(b + 6)};
    b[7] = 0xa1b2c3d4;
    int saved_token = g_process_elevated;
    for (int token = 0; token <= 1; ++token) {
        g_process_elevated = token;
        assert(w32_call_impl(find_impl("CheckTokenMembership"), member_args, 3) == 1);
        assert(b[6] == (u32)token && b[7] == 0xa1b2c3d4);
    }
    g_process_elevated = saved_token;
    u32 free_args[] = {b[4]};
    assert(!w32_FreeSid(free_args));
    b[4] = 0x12345678;
    args[1] = 9;
    assert(!w32_AllocateAndInitializeSid(args) && g_last_error == 87);
    assert(b[4] == 0x12345678 && b[5] == 0xa1b2c3d4);
    args[1] = 0;
    assert(w32_AllocateAndInitializeSid(args));
    sid = (void *)(uintptr_t)b[4];
    assert(sid[0] == 1 && !sid[1]);
    free_args[0] = b[4];
    assert(!w32_FreeSid(free_args));
    args[10] = 0;
    assert(!w32_AllocateAndInitializeSid(args) && g_last_error == 87);
    w32_heap_free(b);
}

static void wow32_static_tls_contract_test(void)
{
    u8 *image = w32_heap_alloc(512, 1);
    assert(image);
    W32Teb teb = {0};
    DataDir dir = {.rva = 32, .size = sizeof(W32TlsDirectory)};
    W32TlsDirectory *tls = (void *)(image + dir.rva);
    tls->start = (u32)(uintptr_t)(image + 128);
    tls->end = tls->start + 4;
    tls->index = (u32)(uintptr_t)(image + 64);
    tls->zero_fill = 28;
    tls->characteristics = 7u << 20; /* 64-byte alignment */
    memcpy(image + 128, "TLS!", 4);
    *(u32 *)(image + 64) = 0xdeadbeef;
    assert(w32_static_tls_init(&teb, image, 512, &dir));
    assert(*(u32 *)(image + 64) == 0);
    u32 *vector = (void *)(uintptr_t)teb.ThreadLocalStoragePointer;
    u8 *block = (void *)(uintptr_t)vector[0];
    assert(vector[0] && !(vector[0] & 63) && !memcmp(block, "TLS!", 4));
    for (size_t i = 4; i < 32; ++i) assert(!block[i]);
    block[0] = 'x';
    W32Teb second = {0};
    assert(w32_static_tls_init(&second, image, 512, &dir));
    u32 *second_vector = (void *)(uintptr_t)second.ThreadLocalStoragePointer;
    assert(second_vector[0] != vector[0]);
    assert(!memcmp((const void *)(uintptr_t)second_vector[0], "TLS!", 4));
    w32_heap_free(vector);                              /* the vector is a 32-bit heap block (DLL slots follow slot 0) */
    w32_heap_free(second_vector);

    teb.ThreadLocalStoragePointer = 0;
    tls->end = tls->start - 1;
    assert(!w32_static_tls_init(&teb, image, 512, &dir));
    tls->end = tls->start + 4;
    tls->index = (u32)(uintptr_t)(image + 510);
    assert(!w32_static_tls_init(&teb, image, 512, &dir));
    tls->index = (u32)(uintptr_t)(image + 64);
    tls->characteristics = 15u << 20;
    assert(!w32_static_tls_init(&teb, image, 512, &dir));
    tls->characteristics = 0;
    tls->zero_fill = UINT32_MAX;
    assert(!w32_static_tls_init(&teb, image, 512, &dir));
    dir.rva = 510;
    assert(!w32_static_tls_init(&teb, image, 512, &dir));
    dir.size = 0;
    assert(w32_static_tls_init(&teb, image, 512, &dir));
    assert(!teb.ThreadLocalStoragePointer);
    w32_heap_free(image);
}

/* Standard controls keep their state as data: list boxes, check state, progress bars and window text. */
static void wow32_controls_contract_test(void)
{
    const u64 list = 0xD0000F01u, button = 0xD0000F02u, bar = 0xD0000F03u, sorted = 0xD0000F04u;
    const u16 a[] = u"alpha", b[] = u"Beta", c[] = u"gamma", prefix[] = u"be";
    u16 text[16];
    assert(beer_ctl_listbox_proc(list, 0x180, 0, (u64)(uintptr_t)a) == 0);      /* LB_ADDSTRING */
    assert(beer_ctl_listbox_proc(list, 0x180, 0, (u64)(uintptr_t)b) == 1);
    assert(beer_ctl_listbox_proc(list, 0x181, 1, (u64)(uintptr_t)c) == 1);      /* LB_INSERTSTRING before "Beta" */
    assert(beer_ctl_listbox_proc(list, 0x18B, 0, 0) == 3);                      /* LB_GETCOUNT */
    assert(beer_ctl_listbox_proc(list, 0x189, 1, (u64)(uintptr_t)text) == 5);   /* LB_GETTEXT = "gamma" */
    assert(!memcmp(text, c, sizeof(c)));
    assert(beer_ctl_listbox_proc(list, 0x18A, 2, 0) == 4);                      /* LB_GETTEXTLEN("Beta") */
    assert(beer_ctl_listbox_proc(list, 0x18F, (u64)(s64)-1, (u64)(uintptr_t)prefix) == 2);   /* FINDSTRING is case-blind */
    assert(beer_ctl_listbox_proc(list, 0x1A2, (u64)(s64)-1, (u64)(uintptr_t)prefix) == (u64)(s64)-1);  /* exact: none */
    assert(beer_ctl_listbox_proc(list, 0x188, 0, 0) == (u64)(s64)-1);           /* no selection yet */
    assert(beer_ctl_listbox_proc(list, 0x186, 2, 0) == 2 && beer_ctl_listbox_proc(list, 0x188, 0, 0) == 2);
    assert(beer_ctl_listbox_proc(list, 0x19A, 2, 0x1234) == 1);                 /* LB_SETITEMDATA */
    assert(beer_ctl_listbox_proc(list, 0x199, 2, 0) == 0x1234);
    assert(beer_ctl_listbox_proc(list, 0x182, 0, 0) == 2);                      /* LB_DELETESTRING leaves 2 */
    assert(beer_ctl_listbox_proc(list, 0x188, 0, 0) == 1);                      /* the selection moved with its item */
    assert(beer_ctl_listbox_proc(list, 0x189, 7, (u64)(uintptr_t)text) == (u64)(s64)-1);
    assert(beer_ctl_listbox_proc(list, 0x184, 0, 0) == 0 && beer_ctl_listbox_proc(list, 0x18B, 0, 0) == 0);
    beer_ctl_destroy(list);
    assert(beer_ctl_listbox_proc(list, 0x18B, 0, 0) == 0);                      /* a destroyed window starts empty */
    beer_ctl_destroy(list);

    WIN_CREATESTRUCT sort_style = { .style = 0x2 /* LBS_SORT */ };
    beer_ctl_on_create(sorted, &sort_style);
    assert(beer_ctl_listbox_proc(sorted, 0x180, 0, (u64)(uintptr_t)c) == 0);
    assert(beer_ctl_listbox_proc(sorted, 0x180, 0, (u64)(uintptr_t)a) == 0);    /* sorts before "gamma" */
    assert(beer_ctl_listbox_proc(sorted, 0x180, 0, (u64)(uintptr_t)b) == 1);    /* between alpha and gamma */
    beer_ctl_destroy(sorted);

    assert(beer_ctl_button_proc(button, 0xF0, 0, 0) == 0);                      /* BM_GETCHECK */
    beer_ctl_button_proc(button, 0xF1, 1, 0);                                   /* BM_SETCHECK */
    assert(beer_ctl_button_proc(button, 0xF0, 0, 0) == 1);
    beer_ctl_destroy(button);

    beer_ctl_progress_proc(bar, 0x406, 10, 50);                                 /* PBM_SETRANGE32 */
    assert(beer_ctl_progress_proc(bar, 0x402, 30, 0) == 0);                     /* PBM_SETPOS returns the old one */
    assert(beer_ctl_progress_proc(bar, 0x408, 0, 0) == 30);
    beer_ctl_progress_proc(bar, 0x404, 5, 0);
    beer_ctl_progress_proc(bar, 0x405, 0, 0);                                   /* PBM_STEPIT */
    assert(beer_ctl_progress_proc(bar, 0x408, 0, 0) == 35);
    beer_ctl_destroy(bar);

    const u16 title[] = u"Setup - Test";
    beer_ctl_set_text(bar, title);
    assert(beer_ctl_get_text(bar, NULL, 0) == 12);
    assert(beer_ctl_get_text(bar, text, 6) == 5 && text[5] == 0 && !memcmp(text, title, 10));   /* truncated, terminated */
    beer_ctl_destroy(bar);
    assert(beer_ctl_get_text(bar, NULL, 0) == 0);
}

static void wow32_ui_contract_test(void)
{
    const W32Api expected[] = {
        {"GetDC", NULL, 4}, {"GetWindowDC", NULL, 4}, {"ReleaseDC", NULL, 8},
        {"GetDeviceCaps", NULL, 8}, {"CreatePalette", NULL, 4},
        {"GetStockObject", NULL, 4}, {"LoadIconW", NULL, 8}, {"LoadCursorW", NULL, 8},
        {"MulDiv", NULL, 12}, {"SystemParametersInfoW", NULL, 16},
        {"CreateFontIndirectW", NULL, 4}, {"GetKeyboardLayout", NULL, 4},
        {"SetClassLongW", NULL, 12}, {"GetSystemMenu", NULL, 8}, {"DeleteMenu", NULL, 12},
        {"OleInitialize", NULL, 4}, {"OleUninitialize", NULL, 0}, {"CoInitialize", NULL, 4},
        {"GetObjectW", w32_GetObjectW, 12}, {"GetClassInfoW", w32_GetClassInfoW, 12},
        {"CreateDIBSection", w32_CreateDIBSection, 24},
        {"CreateCompatibleDC", NULL, 4}, {"SelectObject", NULL, 8},
        {"GetActiveWindow", NULL, 0}, {"SetActiveWindow", NULL, 4},
        {"IsWindowVisible", NULL, 4},
        {"IsWindow", NULL, 4},
        {"IsWindowEnabled", NULL, 4}, {"EnableWindow", NULL, 8},
        {"EnumThreadWindows", w32_EnumThreadWindows, 12},
        {"DeleteDC", NULL, 4}, {"DeleteObject", NULL, 4}, {"GetDIBColorTable", NULL, 16},
        {"CharLowerW", w32_CharLowerW, 4}, {"GetKeyboardLayoutList", w32_GetKeyboardLayoutList, 8},
        {"ImmIsIME", w32_ImmIsIME, 4}, {"SetWindowTextW", NULL, 8},
        {"RegisterWindowMessageW", NULL, 4}, {"GlobalAddAtomW", NULL, 4},
        {"RegisterClipboardFormatW", NULL, 4},
        {"SendNotifyMessageW", NULL, 16}, {"SendNotifyMessageA", NULL, 16},
        {"AllocateAndInitializeSid", w32_AllocateAndInitializeSid, 44}, {"FreeSid", w32_FreeSid, 4},
        {"CheckTokenMembership", NULL, 12}
    };
    for (size_t i = 0; i < sizeof(expected) / sizeof(*expected); ++i) {
        const W32Api *api = w32_find_api(expected[i].name);
        assert(api && api->arg_bytes == expected[i].arg_bytes && api->fn == expected[i].fn);
        if (!api->fn) assert(find_impl(api->name));
    }

    /* No thunk initialization, GS/TEB access, or entry into guest code. All
     * pointer-valued PE32 arguments and embedded pointers live below 2 GiB. */
    struct Wow32UiBuffers {
        u32 logfont[23];
        u32 font[24];               /* LOGFONTW: 92 bytes, then canary. */
        u32 brush[4];               /* LOGBRUSH32: 12 bytes, then canary. */
        u32 bitmap[7];              /* BITMAP32: 24 bytes, then canary. */
        u32 layouts[2];             /* HKL32: 4 bytes, then canary. */
        u32 parameter[2];
        u32 window_class[10];
        u32 class_info[11];         /* WNDCLASS32: 40 bytes, then canary. */
        u16 class_name[40];
        u16 text[8];
    } *b = w32_heap_alloc(sizeof(*b), 1);
    assert(b && (uintptr_t)b > 0xffffu && (uintptr_t)b + sizeof(*b) <= 0x80000000ull);
    const u32 canary = 0xa1b2c3d4u;
    b->font[23] = b->brush[3] = b->bitmap[6] = b->layouts[1] = canary;
    b->class_info[10] = b->parameter[1] = canary;

    u32 args[4] = {0};
    u64 dc = w32_call_impl(find_impl("GetDC"), args, 1);
    assert(dc && dc <= UINT32_MAX);
    args[0] = (u32)dc;
    args[1] = 88; /* LOGPIXELSX */
    assert(w32_call_impl(find_impl("GetDeviceCaps"), args, 2) == 96);
    args[1] = 90; /* LOGPIXELSY */
    assert(w32_call_impl(find_impl("GetDeviceCaps"), args, 2) == 96);
    args[0] = 0;
    args[1] = (u32)dc;
    assert(w32_call_impl(find_impl("ReleaseDC"), args, 2) == 1);
    args[0] = 0;
    dc = w32_call_impl(find_impl("GetWindowDC"), args, 1);
    assert(dc && dc <= UINT32_MAX);
    args[0] = (u32)dc;
    args[1] = 88;
    assert(w32_call_impl(find_impl("GetDeviceCaps"), args, 2) == 96);
    args[0] = 0;
    args[1] = (u32)dc;
    assert(w32_call_impl(find_impl("ReleaseDC"), args, 2) == 1);

    const struct { s32 number, numerator, denominator, result; } muldiv[] = {
        {21, 10, 3, 70}, {-21, 10, 1, -210}, {21, -10, 1, -210},
        {21, 10, -1, -210}, {-21, -10, 1, 210}
    };
    for (size_t i = 0; i < sizeof(muldiv) / sizeof(*muldiv); ++i) {
        args[0] = (u32)muldiv[i].number;
        args[1] = (u32)muldiv[i].numerator;
        args[2] = (u32)muldiv[i].denominator;
        assert(w32_call_impl(find_impl("MulDiv"), args, 3) == (u32)muldiv[i].result);
    }
    args[0] = 0x000e; /* SPI_GETKEYBOARDSPEED: getter only. */
    args[1] = 0;
    args[2] = (u32)(uintptr_t)b->parameter;
    args[3] = 0;
    assert(w32_call_impl(find_impl("SystemParametersInfoW"), args, 4) == 1);
    assert(b->parameter[0] == 31 && b->parameter[1] == canary);

    b->logfont[0] = (u32)-19;
    b->logfont[4] = 700;
    ((u8 *)b->logfont)[20] = 1; /* italic */
    ((u8 *)b->logfont)[23] = 1; /* DEFAULT_CHARSET */
    const u16 face[] = {'A', 'r', 'i', 'a', 'l', 0};
    memcpy((u8 *)b->logfont + 28, face, sizeof(face));
    u64 font = beer_CreateFontIndirectW((const u8 *)b->logfont);
    assert(font && font <= UINT32_MAX);
    args[0] = (u32)(uintptr_t)b->logfont;
    u64 forwarded_font = w32_call_impl(find_impl("CreateFontIndirectW"), args, 1);
    assert(forwarded_font && forwarded_font <= UINT32_MAX);
    u32 native_font[23];
    assert(beer_GetObjectW(font, sizeof(native_font), native_font) == 92);
    assert((s32)native_font[0] == -19 && native_font[4] == 700);
    u64 fonts[] = {font, forwarded_font};
    for (size_t i = 0; i < sizeof(fonts) / sizeof(*fonts); ++i) {
        args[0] = (u32)fonts[i];
        args[1] = args[2] = 0;
        assert(w32_GetObjectW(args) == 92); /* Null output size query. */
        args[1] = 92;
        args[2] = (u32)(uintptr_t)b->font;
        assert(w32_GetObjectW(args) == 92);
        assert(!memcmp(b->font, native_font, 92) && b->font[23] == canary);
    }
    args[0] = 0; /* WHITE_BRUSH */
    u64 brush = beer_GetStockObject(0);
    assert(brush && brush <= UINT32_MAX);
    assert(w32_call_impl(find_impl("GetStockObject"), args, 1) == brush);
    args[0] = (u32)brush;
    args[1] = args[2] = 0;
    assert(w32_GetObjectW(args) == 12);
    args[1] = 12;
    args[2] = (u32)(uintptr_t)b->brush;
    assert(w32_GetObjectW(args) == 12);
    assert(b->brush[0] == 0 && b->brush[1] == 0xffffff && b->brush[2] == 0);
    assert(b->brush[3] == canary);

    u64 bitmap = beer_CreateCompatibleBitmap(0, 3, 2);
    assert(bitmap && bitmap <= UINT32_MAX);
    args[0] = (u32)bitmap;
    args[1] = args[2] = 0;
    assert(w32_GetObjectW(args) == 24);
    memset(b->bitmap, 0xa5, 24);
    args[1] = 23; /* A partial BITMAP must not be written. */
    args[2] = (u32)(uintptr_t)b->bitmap;
    assert(!w32_GetObjectW(args));
    for (size_t i = 0; i < 6; ++i) assert(b->bitmap[i] == 0xa5a5a5a5u);
    assert(b->bitmap[6] == canary);
    args[1] = 24;
    assert(w32_GetObjectW(args) == 24);
    assert(b->bitmap[0] == 0 && b->bitmap[1] == 3 && b->bitmap[2] == 2);
    assert(b->bitmap[3] == 12 && b->bitmap[4] == (32u << 16 | 1u));
    assert(b->bitmap[5] == 0 && b->bitmap[6] == canary);

    args[0] = 0;
    u64 layout = w32_call_impl(find_impl("GetKeyboardLayout"), args, 1);
    assert(layout);
    args[1] = 0;
    assert(w32_GetKeyboardLayoutList(args) == 1);
    args[0] = 1;
    args[1] = (u32)(uintptr_t)b->layouts;
    assert(w32_GetKeyboardLayoutList(args) == 1);
    assert(b->layouts[0] == layout && b->layouts[1] == canary);
    u32 ime_args[] = {(u32)layout};
    assert(!w32_ImmIsIME(ime_args));
    args[0] = (u32)-1;
    assert(!w32_GetKeyboardLayoutList(args) && g_last_error == 87);
    assert(b->layouts[1] == canary);
    args[0] = 1;
    args[1] = 0;
    assert(!w32_GetKeyboardLayoutList(args) && g_last_error == 87);

    args[0] = 'Q';
    assert(w32_CharLowerW(args) == 'q');
    args[0] = '7';
    assert(w32_CharLowerW(args) == '7');
    const u16 mixed[] = {'A', 'b', 'Z', '9', '!', 0, 0x55aa, 0xaa55};
    memcpy(b->text, mixed, sizeof(mixed));
    args[0] = (u32)(uintptr_t)b->text;
    assert(w32_CharLowerW(args) == args[0]);
    assert(b->text[0] == 'a' && b->text[1] == 'b' && b->text[2] == 'z');
    assert(b->text[3] == '9' && b->text[4] == '!' && !b->text[5]);
    assert(b->text[6] == 0x55aa && b->text[7] == 0xaa55);

    /* Registration builds a host bridge for this opaque guest address, but
     * neither registration nor GetClassInfo executes the callback. Restore
     * only the class slot and registration globals touched by this fixture. */
    const char class_name[] = "BeerWow32UiContractFixture";
    for (size_t i = 0; i < sizeof(class_name); ++i) b->class_name[i] = (u8)class_name[i];
    int saved_count = g_window_class_count;
    assert(saved_count < MAX_WINDOW_CLASSES);
    __typeof__(g_window_classes[0]) saved_slot = g_window_classes[saved_count];
    u64 saved_proc = g_window_proc, saved_last_proc = g_last_class_proc;
    u64 saved_instance = g_window_instance, saved_icon = g_window_icon;
    u64 saved_small_icon = g_window_small_icon;
    u32 saved_registerclass = g_api_registerclass;
    const u32 fields[] = {3, 0x123450, 8, 12, 0x400000, 0x20000001,
        0x20000002, (u32)brush, 101, (u32)(uintptr_t)b->class_name};
    memcpy(b->window_class, fields, sizeof(fields));
    assert(w32_register_class(b->window_class, 0));
    assert(g_window_class_count == saved_count + 1);
    assert(g_window_classes[saved_count].info.lpfnWndProc != fields[1]);
    assert(w32_proc_from_native(g_window_classes[saved_count].info.lpfnWndProc) == fields[1]);
    args[0] = fields[4];
    args[1] = fields[9];
    args[2] = (u32)(uintptr_t)b->class_info;
    assert(w32_GetClassInfoW(args) == 1);
    assert(!memcmp(b->class_info, fields, 40) && b->class_info[10] == canary);
    args[2] = 0;
    assert(!w32_GetClassInfoW(args) && g_last_error == 87);
    pthread_mutex_lock(&g_window_class_lock);
    g_window_classes[saved_count] = saved_slot;
    g_window_class_count = saved_count;
    pthread_mutex_unlock(&g_window_class_lock);
    g_window_proc = saved_proc;
    g_last_class_proc = saved_last_proc;
    g_window_instance = saved_instance;
    g_window_icon = saved_icon;
    g_window_small_icon = saved_small_icon;
    g_api_registerclass = saved_registerclass;
    assert(beer_DeleteObject(bitmap));
    assert(beer_DeleteObject(forwarded_font));
    assert(beer_DeleteObject(font));
    w32_heap_free(b);
}

static void wow32_version_contract_test(void)
{
    u8 *memory = w32_heap_alloc(4096, 1);
    assert(memory);
    char *path = (void *)memory;
    strcpy(path, "/tmp/beer-version-abi-XXXXXX");
    int fd = mkstemp(path);
    assert(fd >= 0);
    u8 fixture[1024];
    version_fixture_build(fixture, 0);
    assert(write(fd, fixture, sizeof(fixture)) == sizeof(fixture));
    close(fd);
    u16 *wide_path = (void *)(memory + 256);
    for (size_t i = 0; i <= strlen(path); ++i) wide_path[i] = (u8)path[i];
    u32 *output = (void *)(memory + 512);
    output[0] = 0x12345678; output[1] = 0xaabbccdd;
    u32 size_args[] = {(u32)(uintptr_t)wide_path, (u32)(uintptr_t)output};
    assert(w32_call_impl(find_impl("GetFileVersionInfoSizeW"), size_args, 2) == 92);
    assert(!output[0] && output[1] == 0xaabbccdd);
    size_args[0] = (u32)(uintptr_t)path;
    assert(w32_call_impl(find_impl("GetFileVersionInfoSizeA"), size_args, 2) == 92);
    u8 *block = memory + 1024;
    u32 info_args[] = {(u32)(uintptr_t)wide_path, 0, 92, (u32)(uintptr_t)block};
    assert(w32_call_impl(find_impl("GetFileVersionInfoW"), info_args, 4));
    assert(!memcmp(block, fixture + 640, 92));
    info_args[0] = (u32)(uintptr_t)path;
    assert(w32_call_impl(find_impl("GetFileVersionInfoA"), info_args, 4));
    const u16 root[] = {'\\', 0};
    u16 *query = (void *)(memory + 768);
    memcpy(query, root, sizeof(root));
    u32 query_args[] = {(u32)(uintptr_t)block, (u32)(uintptr_t)query,
                         (u32)(uintptr_t)output, (u32)(uintptr_t)(output + 2)};
    output[0] = 0; output[1] = 0xaabbccdd;
    output[2] = 0; output[3] = 0x99887766;
    assert(w32_VerQueryValueW(query_args));
    assert(output[0] == (u32)(uintptr_t)(block + 40) && output[1] == 0xaabbccdd);
    assert(output[2] == 52 && output[3] == 0x99887766);
    assert(*(u32 *)(uintptr_t)output[0] == 0xfeef04bd);
    query_args[0] = 0;
    assert(!w32_VerQueryValueW(query_args) && g_last_error == 87);
    assert(output[1] == 0xaabbccdd && output[3] == 0x99887766);
    const char *names[] = {"GetFileVersionInfoSizeW", "GetFileVersionInfoSizeA",
                           "GetFileVersionInfoW", "GetFileVersionInfoA", "VerQueryValueW"};
    const u32 bytes[] = {8, 8, 16, 16, 16};
    for (size_t n = 0; n < sizeof(names) / sizeof(*names); ++n) {
        int found = 0;
        for (size_t i = 0; i < sizeof(g_w32_apis) / sizeof(g_w32_apis[0]); ++i)
            if (!strcmp(g_w32_apis[i].name, names[n])) {
                assert(g_w32_apis[i].arg_bytes == bytes[n]);
                found = 1;
            }
        assert(found);
    }
    assert(!unlink(path));
    w32_heap_free(memory);
}

static void wow32_named_resource_contract_test(void)
{
    u8 *image = w32_heap_alloc(4096, 1);
    assert(image);
    ((DosHdr *)image)->lfanew = 0x80;
    W32NtHdrs *nt = (void *)(image + 0x80);
    nt->opt.dirs[2] = (DataDir){0x400, 0x400};
    u8 *root = image + 0x400;
    ((ResourceDir *)root)->id_count = 1;
    *(ResourceEntry *)(root + 16) = (ResourceEntry){10, 0x80000020};
    ((ResourceDir *)(root + 32))->named_count = 1;
    *(ResourceEntry *)(root + 48) = (ResourceEntry){0x80000080, 0x80000040};
    ((ResourceDir *)(root + 64))->id_count = 1;
    *(ResourceEntry *)(root + 80) = (ResourceEntry){1033, 96};
    *(ResourceData *)(root + 96) = (ResourceData){0x600, 4, 0, 0};
    const u16 resource_name[] = {4, 'T', 'E', 'S', 'T'};
    memcpy(root + 128, resource_name, sizeof(resource_name));
    u16 *guest_name = (void *)(image + 0x900);
    const u16 name[] = {'t', 'e', 's', 't', 0};
    memcpy(guest_name, name, sizeof(name));
    u32 module = (u32)(uintptr_t)image, guest = (u32)(uintptr_t)guest_name;
    u32 args[] = {module, guest, 10};
    assert(w32_FindResourceW(args) == module + 0x460);
    u32 ex[] = {module, 10, guest, 1033};
    assert(w32_FindResourceExW(ex) == module + 0x460);
    char host_name[] = "TEST";
    assert((uintptr_t)host_name > UINT32_MAX);
    assert(w32_find_resource(module, 10, (uintptr_t)host_name, 1033) == module + 0x460);
    u32 free_args[] = {module + 0x600};
    assert(!w32_call_impl(find_impl("FreeResource"), free_args, 1));
    int found = 0;
    for (size_t i = 0; i < sizeof(g_w32_apis) / sizeof(g_w32_apis[0]); ++i)
        if (!strcmp(g_w32_apis[i].name, "FreeResource")) {
            assert(g_w32_apis[i].arg_bytes == 4);
            found = 1;
        }
    assert(found);
    w32_heap_free(image);
}

static void process_security_contract_test(void)
{
    static const char xml[] =
        "<?xml version='1.0'?><assembly xmlns='urn:schemas-microsoft-com:asm.v1'>"
        "<!-- level='asInvoker' -->"
        "<trustInfo xmlns='urn:schemas-microsoft-com:asm.v3'><security><requestedPrivileges>"
        "<requestedExecutionLevel uiAccess='false' level = 'requireAdministrator'/>"
        "</requestedPrivileges></security></trustInfo></assembly>";
    assert(beer_manifest_xml_level((const u8 *)xml, strlen(xml)) == 3);
    const char *comment = "<assembly><!-- <requestedExecutionLevel level='requireAdministrator'/> --></assembly>";
    assert(beer_manifest_xml_level((const u8 *)comment, strlen(comment)) == 0);
    const char *bad = "<assembly><trustInfo>";
    assert(beer_manifest_xml_level((const u8 *)bad, strlen(bad)) == -1);
    const char *dtd = "<!DOCTYPE assembly [<!ENTITY x 'test'>]><assembly/>";
    assert(beer_manifest_xml_level((const u8 *)dtd, strlen(dtd)) == -1);
    /* A UTF-16LE BOM and XML declaration are handled by the real XML parser. */
    u16 utf16[512] = {0xfeff};
    for (size_t i = 0; i < strlen(xml); ++i) utf16[i + 1] = (u8)xml[i];
    /* The declaration has no encoding attribute, so BOM selects UTF-16. */
    assert(beer_manifest_xml_level((const u8 *)utf16, (strlen(xml) + 1) * 2) == 3);
    int saved_token = g_process_elevated;
    const char *prior = getenv("_BEER_PRIMARY_TOKEN");
    char *saved_env = prior ? strdup(prior) : NULL;
    for (int bitness = 0; bitness < 2; ++bitness) {
        u8 image[4096] = {0};
        *(u16 *)image = MZ_MAGIC;
        *(u32 *)(image + 0x3c) = 0x80;
        *(u32 *)(image + 0x80) = PE_SIG;
        *(u16 *)(image + 0x86) = 1;
        size_t opt = 0x98, dirs = bitness ? 112 : 96;
        *(u16 *)(image + 0x94) = dirs + 128;
        *(u16 *)(image + opt) = bitness ? PE32PLUS : 0x10b;
        *(u32 *)(image + opt + dirs - 4) = 16;
        DataDir *resources = (void *)(image + opt + dirs + 16);
        *resources = (DataDir){0x400, 0x800};
        u8 *root = image + 0x400;
        ((ResourceDir *)root)->id_count = 1;
        *(ResourceEntry *)(root + 16) = (ResourceEntry){24, 0x80000020};
        ((ResourceDir *)(root + 32))->id_count = 1;
        *(ResourceEntry *)(root + 48) = (ResourceEntry){1, 0x80000040};
        ((ResourceDir *)(root + 64))->id_count = 1;
        *(ResourceEntry *)(root + 80) = (ResourceEntry){1033, 96};
        *(ResourceData *)(root + 96) = (ResourceData){0x480, sizeof(xml) - 1, 0, 0};
        memcpy(root + 128, xml, sizeof(xml));
        SecHdr *section = (void *)(image + opt + dirs + 128);
        section->vrva = section->raw_off = 0x400;
        section->raw_sz = 0x800;
        assert(beer_security_image_level(image, sizeof(image), 1) == 3);
        assert(beer_security_image_level(image, sizeof(image), 0) == 3);
        unsetenv("_BEER_PRIMARY_TOKEN");
        assert(beer_security_init(image, sizeof(image)) && beer_process_elevated() == 1);
        setenv("_BEER_PRIMARY_TOKEN", "0", 1);
        assert(!beer_security_init(image, sizeof(image)) && g_last_error == 740);
        assert(!getenv("_BEER_PRIMARY_TOKEN"));
        setenv("_BEER_PRIMARY_TOKEN", "1", 1);
        assert(beer_security_init(image, sizeof(image)) && beer_process_elevated() == 1);
        /* Remove the manifest: explicit false must override stale true state;
         * explicit true must survive an asInvoker/no-manifest child. */
        resources->rva = resources->size = 0;
        for (int token = 0; token <= 1; ++token) {
            setenv("_BEER_PRIMARY_TOKEN", token ? "1" : "0", 1);
            assert(beer_security_init(image, sizeof(image)) && beer_process_elevated() == token);
            setenv("_BEER_PRIMARY_TOKEN", "stale", 1);
            /* A stale image name (this process's own) never reaches a child; the one given for the child does, once. */
            setenv("_BEER_IMAGE_NAME", "stale.exe", 1);
            char **env = beer_inherit_environment(NULL, token ? "_BEER_IMAGE_NAME=CLS-srep_x64.exe" : NULL);
            assert(env);
            int seen = 0, images = 0;
            for (size_t i = 0; env[i]; ++i) {
                if (!strncmp(env[i], "_BEER_IMAGE_NAME=", 17)) {
                    assert(token && !strcmp(env[i], "_BEER_IMAGE_NAME=CLS-srep_x64.exe"));
                    ++images;
                }
                if (strncmp(env[i], "_BEER_PRIMARY_TOKEN=", 20)) continue;
                assert(!strcmp(env[i], token ? "_BEER_PRIMARY_TOKEN=1" : "_BEER_PRIMARY_TOKEN=0"));
                ++seen;
            }
            assert(seen == 1);
            assert(images == (token ? 1 : 0));
            unsetenv("_BEER_IMAGE_NAME");
            free(env);
        }
        unsetenv("_BEER_PRIMARY_TOKEN");
        assert(beer_security_init(image, sizeof(image)) && beer_process_elevated() == 0);
        setenv("_BEER_PRIMARY_TOKEN", "invalid", 1);
        assert(!beer_security_init(image, sizeof(image)) && g_last_error == 13);
        resources->rva = sizeof(image) - 1;
        resources->size = 128;
        assert(beer_security_image_level(image, sizeof(image), 1) == -1);
    }
    g_process_elevated = saved_token;
    if (saved_env) { setenv("_BEER_PRIMARY_TOKEN", saved_env, 1); free(saved_env); }
    else unsetenv("_BEER_PRIMARY_TOKEN");
}

/* Asynchronous OVERLAPPED reads (FILE_FLAG_OVERLAPPED regular files): pending or inline completion, data,
 * GetOverlappedResult waiting, end of file, completion-port packets and CancelIoEx. The file is dropped from the
 * page cache first so the reads take the asynchronous path. */
static void evict_file(const char *host)
{
    int fd = open(host, O_RDONLY);
    assert(fd >= 0);
    fdatasync(fd);
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    close(fd);
}
static void async_file_read_contract_test(void)
{
    char host[] = "/tmp/beer-async-read-XXXXXX";
    int fd = mkstemp(host);
    assert(fd >= 0);
    enum { SIZE = 1 << 20, CHUNK = 65536 };
    u8 *data = malloc(SIZE);
    assert(data);
    for (u32 i = 0; i < SIZE; ++i) data[i] = (u8)(i * 7 + (i >> 12));
    assert(write(fd, data, SIZE) == SIZE);
    close(fd);
    char guest[PATH_MAX];
    snprintf(guest, sizeof(guest), "Z:%s", host);
    for (char *c = guest; *c; ++c) if (*c == '/') *c = '\\';
    const u64 file = impl_CreateFileA(guest, 0x80000000u, 1, 0, 3, 0x40000080u, 0);   /* FILE_FLAG_OVERLAPPED */
    assert(file != INVALID_HANDLE_VALUE64 && fh_is_async(file));
    u8 *buffer = malloc(CHUNK);
    assert(buffer);

    /* Event-signalled read, waited for with GetOverlappedResult. */
    evict_file(host);
    WIN_OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.Offset = 7 * 4096;
    ov.hEvent = event_alloc(1, 1);                       /* initially set: ReadFile must reset it */
    u32 got = 0;
    const u64 ok = impl_ReadFile(file, buffer, CHUNK, NULL, &ov);
    assert(ok || g_last_error == 997);
    assert(impl_GetOverlappedResult(file, &ov, &got, 1) && got == CHUNK);
    assert(!memcmp(buffer, data + 7 * 4096, CHUNK));
    assert(impl_WaitForSingleObject(ov.hEvent, 0) == 0);
    assert(g_async_workers > 0 || ok);

    /* Past the end: no data, end-of-file. */
    memset(&ov, 0, sizeof(ov));
    ov.Offset = SIZE;
    ov.hEvent = event_alloc(1, 0);
    const u64 eof = impl_ReadFile(file, buffer, CHUNK, NULL, &ov);
    got = 1;
    const u64 eof_result = impl_GetOverlappedResult(file, &ov, &got, 1);
    assert(got == 0);
    assert(eof_result || g_last_error == 38);
    (void)eof;

    /* Completion port: the packet carries the key, the OVERLAPPED and the byte count. */
    const u64 port = impl_CreateIoCompletionPort(file, 0, 77, 0);
    assert(port);
    evict_file(host);
    memset(&ov, 0, sizeof(ov));
    ov.Offset = 3 * CHUNK;
    memset(buffer, 0, CHUNK);
    const u64 queued = impl_ReadFile(file, buffer, CHUNK, NULL, &ov);
    assert(queued || g_last_error == 997);
    u32 bytes = 0;
    u64 key = 0, overlapped = 0;
    assert(impl_GetQueuedCompletionStatus(port, &bytes, &key, &overlapped, 5000));
    assert(key == 77 && overlapped == (u64)(uintptr_t)&ov && bytes == CHUNK);
    assert(ov.Internal == 0 && ov.InternalHigh == CHUNK && !memcmp(buffer, data + 3 * CHUNK, CHUNK));

    /* Nothing is queued any more: CancelIoEx finds nothing. */
    assert(!impl_CancelIoEx(file, &ov) && g_last_error == 1168);
    assert(impl_CloseHandle(file));
    impl_CloseHandle(port);
    unlink(host);
    free(buffer);
    free(data);
    puts("asynchronous OVERLAPPED file reads: PASS");
}

int main(void)
{
    async_file_read_contract_test();
    steam_auth_callback_contract_test();
    typedef u64 (__attribute__((ms_abi)) *SetConsoleAttributes)(u64, u16);
    SetConsoleAttributes set_attributes = (SetConsoleAttributes)find_impl("SetConsoleTextAttribute");
    assert(set_attributes);
    int console_export_owners = 0;
    for (size_t i = 0; i < BEER_MODULE_EXPORT_COUNT; ++i)
        if (!strcmp(beer_module_exports[i][1], "SetConsoleTextAttribute")) {
            if (!strcmp(beer_module_exports[i][0], "kernel32.dll")) console_export_owners |= 1;
            if (!strcmp(beer_module_exports[i][0], "kernelbase.dll")) console_export_owners |= 2;
        }
    assert(console_export_owners == 3);
    const u64 console_handles[] = {0, UINT64_MAX, (u64)(s64)-11, (u64)(s64)-12};
    for (size_t i = 0; i < sizeof(console_handles) / sizeof(*console_handles); ++i) {
        g_last_error = 0;
        assert(!set_attributes(console_handles[i], 14));
        assert(g_last_error == 6);
    }
    multimedia_timer_contract_test();
        legacy_dns_contract_test();
    subclass_contract_test();
    notify_message_contract_test();
    native_crt_import_ownership_test();
    locale_info_contract_test();
    winsock_ordinal_contract_test();
    wldap_ordinal_contract_test();
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
    /* Inheriting redirected standard handles (STARTUPINFO.dwFlags = STARTF_USESTDHANDLES) is accepted now (handles
     * travel to the child); a missing executable still fails cleanly and leaves PROCESS_INFORMATION zeroed. */
    u8 startup[104] = {0};
    *(u32 *)startup = sizeof(startup);
    *(u32 *)(startup + 60) = 0x100u;
    memset(process_info, 0xa5, sizeof(process_info));
    assert(!beer_CreateProcessA("fixture.exe", NULL, NULL, NULL, 1, 0, NULL, NULL, startup, process_info));
    assert(g_last_error != 0 && g_last_error != 50);
    for (size_t i = 0; i < sizeof(process_info); ++i) assert(!process_info[i]);
    const u16 process_name[] = {'f', 'i', 'x', 't', 'u', 'r', 'e', 0};
    memset(process_info, 0xa5, sizeof(process_info));
    assert(!beer_CreateProcessW(process_name, NULL, NULL, NULL, 1, 0, NULL, NULL, startup, process_info));
    assert(g_last_error != 0 && g_last_error != 50);
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
    /* Legacy Winsock ordinal identity must stay consistent across direct import
     * binding, GetProcAddress/Ldr lookup, and the synthetic PE export table. */
    assert(find_impl("ldap_next_entry") == (ImplFn)impl_ldap_next_entry);
    assert(wldap_ordinal_impl(27) == (ImplFn)impl_ldap_next_entry);
    assert(!strcmp(wldap_ordinal_name(27), "ldap_next_entry"));
    assert(wldap_ordinal_impl(46) == (ImplFn)impl_ldap_unbind_s);
    assert(!strcmp(wldap_ordinal_name(46), "ldap_unbind_s"));
    assert(wldap_ordinal_impl(200) == (ImplFn)impl_ldap_memfree);
    assert(!strcmp(wldap_ordinal_name(200), "ldap_memfree"));
    assert(wldap_ordinal_impl(301) == (ImplFn)impl_ber_free);
    assert(!strcmp(wldap_ordinal_name(301), "ber_free"));
    assert(wldap_ordinal_impl(26) == (ImplFn)impl_ldap_first_entry);
    assert(!strcmp(wldap_ordinal_name(26), "ldap_first_entry"));
    assert(!wldap_ordinal_impl(25) && !wldap_ordinal_name(25));
    assert(!impl_ldap_next_entry(0, 0));
    assert(impl_ldap_unbind_s(0) == 89);
    u16 station_name[16] = {0};
    u32 station_bytes = 0;
    assert(!impl_GetUserObjectInformationW(impl_GetProcessWindowStation(), 2,
                                           station_name, 0, &station_bytes));
    assert(g_last_error == 122 && station_bytes == 16);
    assert(impl_GetUserObjectInformationW(impl_GetProcessWindowStation(), 2,
                                          station_name, sizeof(station_name),
                                          &station_bytes));
    assert(station_name[0] == 'W' && station_name[6] == '0' && !station_name[7]);
    assert(winsock_ordinal_impl(8) == (ImplFn)impl_htonl);
    assert(winsock_ordinal_impl(10) == (ImplFn)impl_ioctlsocket);
    assert(winsock_ordinal_impl(14) == (ImplFn)impl_ntohl);
    assert(winsock_ordinal_impl(15) == (ImplFn)impl_ntohs);
    assert(winsock_ordinal_impl(18) == (ImplFn)impl_select);
    assert(winsock_ordinal_impl(19) == (ImplFn)impl_send);
    assert(winsock_ordinal_impl(22) == (ImplFn)impl_shutdown);
    assert(winsock_ordinal_impl(51) == (ImplFn)impl_gethostbyaddr);
    assert(winsock_ordinal_impl(52) == (ImplFn)impl_gethostbyname);
    assert(winsock_ordinal_impl(55) == (ImplFn)impl_getservbyname);
    assert(winsock_ordinal_impl(151) == (ImplFn)impl_WSAFDIsSet);
    assert(!strcmp(winsock_ordinal_name(8), "htonl"));
    assert(!strcmp(winsock_ordinal_name(10), "ioctlsocket"));
    assert(!strcmp(winsock_ordinal_name(14), "ntohl"));
    assert(!strcmp(winsock_ordinal_name(15), "ntohs"));
    assert(!strcmp(winsock_ordinal_name(18), "select"));
    assert(!strcmp(winsock_ordinal_name(19), "send"));
    assert(!strcmp(winsock_ordinal_name(22), "shutdown"));
    assert(!strcmp(winsock_ordinal_name(51), "gethostbyaddr"));
    assert(!strcmp(winsock_ordinal_name(52), "gethostbyname"));
    assert(!strcmp(winsock_ordinal_name(55), "getservbyname"));
    assert(!strcmp(winsock_ordinal_name(151), "__WSAFDIsSet"));
    assert(find_impl("select") == (ImplFn)impl_select);
    assert(find_impl("send") == (ImplFn)impl_send);
    assert(find_impl("getservbyname") == (ImplFn)impl_getservbyname);
    assert(find_impl("getaddrinfo") == (ImplFn)impl_getaddrinfo);
    assert(find_impl("freeaddrinfo") == (ImplFn)impl_freeaddrinfo);
    assert(find_impl("__WSAFDIsSet") == (ImplFn)impl_WSAFDIsSet);
    assert(find_impl("WSAIoctl") == (ImplFn)impl_WSAIoctl);
    for (u32 ordinal = 1; ordinal <= UINT16_MAX; ++ordinal) {
        ImplFn implementation = winsock_ordinal_impl((u16)ordinal);
        const char *name = winsock_ordinal_name((u16)ordinal);
        assert((implementation != NULL) == (name != NULL));
        if (name) assert(find_impl(name) == implementation);
    }
    /* The sparse table needs 604 bytes. The old fixed 116-slot allocation had
     * only 464 bytes and ordinal 151 consequently overwrote AddressOfNames. */
    assert(151u * sizeof(u32) == 604u);
    assert(151u * sizeof(u32) > 0x200u);
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
    {
        uint32_t next = 0;
        assert(beer_static_tls_next_slot(&next, TLS_SLOTS) == 1);
        assert(beer_static_tls_next_slot(&next, TLS_SLOTS) == 2);
    }
    /* Build the actual synthetic PE images and walk their real export table.
     * Names must remain PE-sorted while addresses follow service ID order. */
    g_tramp = mmap(NULL, (size_t)MAX_STUBS * THUNK_SZ, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(g_tramp != MAP_FAILED);
    window_callback_stack_test();
    /* The real loader constructor already maps these headers before main. */
    for (size_t i = 0; i < sizeof(g_fake_pe_modules) / sizeof(*g_fake_pe_modules); ++i)
        assert(*(const u16 *)(uintptr_t)g_fake_pe_modules[i].base == MZ_MAGIC);
    /* A test-only unknown import must reserve its service slot before layout. */
    g_snames[g_nstubs++] = strdup("ntdll.dll!NtBeerUnsupportedTest");
    g_snames[g_nstubs++] = strdup("shell32.dll!CommandLineToArgvW");
    fake_pe_exports_build();
    assert(g_fake_pe_ready);
    u64 comctl = dll_name_to_handle("comctl32.dll", 1);
    assert(comctl);
    const char *subclass_exports[] = { "SetWindowSubclass", "GetWindowSubclass",
                                       "RemoveWindowSubclass", "DefSubclassProc" };
    for (size_t i = 0; i < 4; ++i) {
        u64 named = impl_GetProcAddress(comctl, subclass_exports[i]);
        assert(named && named == impl_GetProcAddress(comctl, (const char *)(uintptr_t)(410 + i)));
        assert(find_impl(subclass_exports[i]));
    }
    late_loader_list_test();
    int before_missing_lookup = g_nstubs;
    assert(!fake_pe_export_address(HMOD_K32, "InitializeProcThreadAttributeList"));
    assert(!impl_GetProcAddress(HMOD_K32, "InitializeProcThreadAttributeList") && g_last_error == 127);
    assert(!impl_GetProcAddress(HMOD_K32, "UpdateProcThreadAttribute") && g_last_error == 127);
    assert(!impl_GetProcAddress(HMOD_USER32, "BeerAbsentExportTest") && g_last_error == 127);
    assert(fake_pe_export_address(HMOD_USER32, "MessageBoxA"));
    assert(fake_pe_export_address(HMOD_USER32, "MessageBoxW"));
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
    wow32_ui_contract_test();
    wow32_static_tls_contract_test();
    wow32_controls_contract_test();
    wow32_sid_contract_test();
    wow32_dib_contract_test();
    process_security_contract_test();
    wow32_named_resource_contract_test();
    wow32_version_contract_test();
    wow32_dll_contract_test();
    puts("native loader ABI/interoperability: PASS");
    return 0;
}