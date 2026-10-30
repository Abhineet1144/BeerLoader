#define _GNU_SOURCE
#include "win32_syscall.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static uint64_t seen[6];

static uint64_t __attribute__((ms_abi))
service_six(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f,
            uint64_t g, uint64_t h, uint64_t i, uint64_t j, uint64_t k, uint64_t l)
{
    (void)g; (void)h; (void)i; (void)j; (void)k; (void)l;
    seen[0] = a; seen[1] = b; seen[2] = c; seen[3] = d; seen[4] = e; seen[5] = f;
    return 0x12345678u;
}

typedef uint64_t (__attribute__((ms_abi)) *SixArgs)(uint64_t, uint64_t, uint64_t, uint64_t,
                                                     uint64_t, uint64_t);

/* Guest code runs with guest system calls blocked, as after an import-trampoline return. */
static uint64_t call_as_guest(void *code, uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                              uint64_t e, uint64_t f)
{
    beer_win32_syscall_selector = 1;
    uint64_t status = ((SixArgs)code)(a, b, c, d, e, f);
    /* The resume stub leaves guest calls blocked; this host code's next system
     * call exercises the trapped-host path, which re-allows and re-executes it. */
    assert(beer_win32_syscall_selector == 1);
    return status;
}

int main(void)
{
    assert(beer_win32_syscall_is_service_name("NtQueryInformationProcess"));
    assert(beer_win32_syscall_is_service_name("ZwClose"));
    assert(!beer_win32_syscall_is_service_name("NtdllDefWindowProc_A"));
    assert(!beer_win32_syscall_is_service_name("NtGetTickCount"));
    assert(!beer_win32_syscall_is_service_name("RtlGetVersion"));

    /* Windows 10 19045 numbers for known services, Beer numbers for others. */
    assert(beer_win32_syscall_register("ZwClose", NULL) == 0x0F);
    assert(beer_win32_syscall_register("NtReadVirtualMemory", NULL) == 0x3F);
    assert(beer_win32_syscall_register("NtProtectVirtualMemory", NULL) == 0x50);
    int nt = beer_win32_syscall_register("NtBeerTestService", service_six);
    int zw = beer_win32_syscall_register("ZwBeerTestService", NULL);
    int missing = beer_win32_syscall_register("NtBeerMissingService", NULL);
    assert(nt == (int)BEER_WIN32_SYSCALL_EXTRA_BASE && zw == nt && missing == nt + 1);

    uint8_t bytes[BEER_WIN32_SYSCALL_STUB_SIZE];
    assert(beer_win32_syscall_write_stub(bytes, sizeof(bytes) - 1, 1) == 0);
    assert(beer_win32_syscall_write_stub(bytes, sizeof(bytes), (uint32_t)nt) == sizeof(bytes));
    static const uint8_t prefix[] = { 0x4C, 0x8B, 0xD1, 0xB8 };
    uint32_t encoded;
    memcpy(&encoded, bytes + 4, sizeof(encoded));
    assert(!memcmp(bytes, prefix, sizeof(prefix)) && encoded == (uint32_t)nt);
    assert(bytes[18] == 0x0F && bytes[19] == 0x05 && bytes[20] == 0xC3);

    /* The stub's test reads KUSER_SHARED_DATA.SystemCall at 0x7ffe0308. */
    void *shared = mmap((void *)0x7ffe0000, 0x1000, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(shared == (void *)0x7ffe0000);
    uint8_t *code = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(code != MAP_FAILED);
    beer_win32_syscall_write_stub(code, 0x100, (uint32_t)nt);
    beer_win32_syscall_write_stub(code + 0x40, 0x100, (uint32_t)missing);
    beer_win32_syscall_write_stub(code + 0x80, 0x100, BEER_WIN32_SYSCALL_CAPACITY - 1);
    /* A guest syscall with a number Linux also uses (39 = getpid) must not reach Linux. */
    beer_win32_syscall_write_stub(code + 0xC0, 0x100, 39);

    assert(beer_win32_syscall_install());
    uint64_t status = call_as_guest(code, 0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666);
    assert(status == 0x12345678u);
    assert(seen[0] == 0x1111 && seen[1] == 0x2222 && seen[2] == 0x3333 &&
           seen[3] == 0x4444 && seen[4] == 0x5555 && seen[5] == 0x6666);
    assert(call_as_guest(code + 0x40, 0, 0, 0, 0, 0, 0) == BEER_STATUS_NOT_IMPLEMENTED);
    assert(call_as_guest(code + 0x80, 0, 0, 0, 0, 0, 0) == BEER_STATUS_INVALID_SYSTEM_SERVICE);
    assert(call_as_guest(code + 0xC0, 0, 0, 0, 0, 0, 0) == BEER_STATUS_INVALID_SYSTEM_SERVICE);

    /* Host system calls made while blocked are re-executed after re-allowing. */
    beer_win32_syscall_selector = 1;
    assert(getpid() > 0);
    assert(beer_win32_syscall_selector == 0);
    puts("win32 syscall tests passed");
    return 0;
}
