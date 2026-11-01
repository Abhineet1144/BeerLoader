/* Executes the KiUserExceptionDispatcher stub the way Windows enters it: RSP at a CONTEXT, the
 * EXCEPTION_RECORD 0x4F0 bytes above, no return address. Checks the arguments the dispatcher gets
 * and that the stub resumes through the returned entry with (CONTEXT, NULL). */
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "win32_ki_dispatcher.h"

static jmp_buf done;
static uint8_t *expected_context;
static int dispatched, resumed;
static uint64_t resume_rcx, resume_rdx;

static void __attribute__((ms_abi, noreturn)) resume(uint64_t context, uint64_t record)
{
    resume_rcx = context;
    resume_rdx = record;
    resumed = 1;
    longjmp(done, 1);
}

static uint64_t __attribute__((ms_abi)) dispatch(void *record, uint8_t *context)
{
    assert(context == expected_context);
    assert((uint8_t *)record == expected_context + BEER_KI_EXCEPTION_RECORD_OFFSET);
    assert(*(uint32_t *)record == 0xC0000005u);       /* the record the "kernel" placed */
    dispatched = 1;
    return (uint64_t)(uintptr_t)resume;
}

int main(void)
{
    uint8_t *code = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(code != MAP_FAILED);
    /* Entry shim: mov rsp, rdi ; jmp rsi */
    static const uint8_t shim[] = { 0x48, 0x89, 0xFC, 0xFF, 0xE6 };
    memcpy(code, shim, sizeof(shim));
    beer_ki_dispatcher_stub(code + 64, (uint64_t)(uintptr_t)dispatch);
    assert(code[64] == 0xFC);                           /* cld first, as on Windows */

    static uint8_t stack[1 << 16] __attribute__((aligned(16)));
    expected_context = stack + sizeof(stack) - 0x1000;   /* room below for the call frames */
    memset(expected_context, 0, 0x4F0 + 0x98);
    *(uint32_t *)(expected_context + BEER_KI_EXCEPTION_RECORD_OFFSET) = 0xC0000005u;
    typedef void (*Enter)(void *rsp, void *target);
    if (!setjmp(done)) ((Enter)(void *)code)(expected_context, code + 64);
    assert(dispatched && resumed);
    assert(resume_rcx == (uint64_t)(uintptr_t)expected_context);
    assert(resume_rdx == 0);
    puts("KiUserExceptionDispatcher stub tests passed");
    return 0;
}
