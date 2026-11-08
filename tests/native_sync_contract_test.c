#define main beer_loader_unused_main
#include "../loader.c"
#undef main
#include <assert.h>

static _Atomic u32 thread_calls;
static u64 __attribute__((ms_abi)) thread_probe(u64 argument)
{
    assert(argument == 0x12345678);
    atomic_fetch_add(&thread_calls, 1);
    return 42;
}

int main(void)
{
    /* The established worker backend initializes TLS from the process PE. */
    static u8 image[4096];
    g_img = image;
    ((DosHdr *)image)->magic = MZ_MAGIC;
    ((DosHdr *)image)->lfanew = sizeof(DosHdr);
    NtHdrs64 *nt = (NtHdrs64 *)(image + sizeof(DosHdr));
    nt->sig = PE_SIG;
    nt->opt.magic = PE32PLUS;
    for (u32 type = 0; type < 2; ++type) {
        u64 event = 0;
        BeerSyncObjectAttributes attributes = { .Length = 48 };
        assert(!impl_NtCreateEvent(&event, 0x1f0003, &attributes, type, 0));
        assert(event && impl_WaitForSingleObject(event, 0) == 0x102);
        s32 previous = -1;
        assert(!impl_NtSetEvent(event, &previous) && previous == 0);
        assert(!impl_NtSetEvent(event, &previous) && previous == 1);
        assert(impl_WaitForSingleObject(event, 0) == 0);
        assert(impl_WaitForSingleObject(event, 0) == (type ? 0x102 : 0));
        assert(impl_SetEvent(event));
        assert(!impl_NtResetEvent(event, &previous) && previous == 1);
        assert(!impl_NtResetEvent(event, &previous) && previous == 0);
        assert(impl_WaitForSingleObject(event, 0) == 0x102);
        assert(impl_SetEvent(event) && !impl_NtClearEvent(event));
        assert(impl_WaitForSingleObject(event, 0) == 0x102);
        assert(beer_NtClose(event) == 0);
        previous = -1;
        assert(impl_NtSetEvent(event, &previous) == NT_STATUS_INVALID_HANDLE && previous == -1);
        assert(impl_NtResetEvent(event, &previous) == NT_STATUS_INVALID_HANDLE && previous == -1);
        assert(!impl_NtCreateEvent(&event, 0x1f0003, NULL, type, 1));
        assert(impl_WaitForSingleObject(event, 0) == 0);
        assert(impl_CloseHandle(event));
    }
    u64 output = 0x1234;
    BeerSyncObjectAttributes attributes = { .Length = 48, .ObjectName = 1 };
    assert(impl_NtCreateEvent(&output, 0x1f0003, &attributes, 0, 0) == BEER_STATUS_NOT_IMPLEMENTED);
    attributes.ObjectName = 0;
    attributes.Length = 47;
    assert(impl_NtCreateEvent(&output, 0x1f0003, &attributes, 0, 0) == NT_STATUS_INVALID_PARAMETER);
    assert(impl_NtCreateEvent(&output, 0x1f0003, NULL, 2, 0) == 0xC00000F2u);
    assert(impl_NtCreateEvent(&output, 1, NULL, 0, 0) == BEER_STATUS_NOT_IMPLEMENTED);
    assert(output == 0x1234);
    assert(impl_NtCreateEvent(NULL, 0x1f0003, NULL, 0, 0) == NT_STATUS_ACCESS_VIOLATION);
    assert(find_impl("ZwCreateEvent") == (ImplFn)impl_NtCreateEvent);
    assert(find_impl("NtSetEvent") == (ImplFn)impl_NtSetEvent);
    assert(beer_win32_syscall_register("NtCreateEvent", (BeerWin32SyscallFn)impl_NtCreateEvent) == 0x48);
    assert(beer_win32_syscall_register("NtSetEvent", (BeerWin32SyscallFn)impl_NtSetEvent) == 0xe);
    assert(beer_win32_syscall_supported(0x48) && beer_win32_syscall_supported(0xe));
    assert(beer_win32_syscall_register("NtCreateThreadEx", (BeerWin32SyscallFn)impl_NtCreateThreadEx) == 0xc2);
    u64 thread = 0;
    assert(!impl_NtCreateThreadEx(&thread, 0x1fffff, NULL, UINT64_MAX,
                                 (u64)(uintptr_t)thread_probe, 0x12345678, 1, 0, 0, 0, NULL));
    assert(thread && !atomic_load(&thread_calls));
    assert(impl_WaitForSingleObject(thread, 0) == 0x102);
    assert(impl_ResumeThread(thread) == 1);
    assert(impl_WaitForSingleObject(thread, 5000) == 0);
    u32 exit_code = 0;
    assert(impl_GetExitCodeThread(thread, &exit_code) && exit_code == 42);
    assert(atomic_load(&thread_calls) == 1);
    assert(impl_CloseHandle(thread));
    thread = 0x1234;
    assert(impl_NtCreateThreadEx(&thread, 0x1fffff, NULL, 1,
                                (u64)(uintptr_t)thread_probe, 0, 0, 0, 0, 0, NULL) == NT_STATUS_INVALID_HANDLE);
    assert(impl_NtCreateThreadEx(&thread, 0x1fffff, NULL, UINT64_MAX,
                                (u64)(uintptr_t)thread_probe, 0, 2, 0, 0, 0, NULL) == BEER_STATUS_NOT_IMPLEMENTED);
    assert(thread == 0x1234);
    assert(find_impl("ZwCreateThreadEx") == (ImplFn)impl_NtCreateThreadEx);
    puts("native event / Kernel32 interoperability: PASS");
    return 0;
}