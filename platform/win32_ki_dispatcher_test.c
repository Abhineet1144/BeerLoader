/* Executes the KiUserExceptionDispatcher stub the way Windows enters it: RSP at a CONTEXT, the
 * EXCEPTION_RECORD 0x4F0 bytes above, no return address. Checks the arguments the dispatcher gets,
 * that a set Wow64PrepareForException runs first with the same arguments, that the entry exposes that
 * variable through "cld; mov rax, [rip+disp32]", and that the stub resumes through the returned entry
 * with (CONTEXT, NULL). */
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "win32_ki_dispatcher.h"

static jmp_buf done;
static uint8_t *expected_context;
static int prepared, dispatched, resumed;
static uint64_t resume_rcx, resume_rdx;

static void __attribute__((ms_abi, noreturn)) resume(uint64_t context, uint64_t record)
{
    resume_rcx = context;
    resume_rdx = record;
    resumed = 1;
    longjmp(done, 1);
}

static void check_arguments(void *record, uint8_t *context)
{
    assert(context == expected_context);
    assert((uint8_t *)record == expected_context + BEER_KI_EXCEPTION_RECORD_OFFSET);
    assert(*(uint32_t *)record == 0xC0000005u);       /* the record the "kernel" placed */
    assert(((uintptr_t)__builtin_frame_address(0) & 15) == 0);
}

static void __attribute__((ms_abi)) prepare(void *record, uint8_t *context)
{
    check_arguments(record, context);
    assert(!dispatched);
    prepared = 1;
}

static uint64_t __attribute__((ms_abi)) dispatch(void *record, uint8_t *context)
{
    check_arguments(record, context);
    dispatched = 1;
    return (uint64_t)(uintptr_t)resume;
}

static void enter(uint8_t *code, uint8_t *entry)
{
    static uint8_t stack[1 << 16] __attribute__((aligned(16)));
    expected_context = stack + sizeof(stack) - 0x1000;   /* room below for the call frames */
    memset(expected_context, 0, 0x4F0 + 0x98);
    *(uint32_t *)(expected_context + BEER_KI_EXCEPTION_RECORD_OFFSET) = 0xC0000005u;
    prepared = dispatched = resumed = 0;
    typedef void (*Enter)(void *rsp, void *target);
    if (!setjmp(done)) ((Enter)(void *)code)(expected_context, entry);
    assert(dispatched && resumed);
    assert(resume_rcx == (uint64_t)(uintptr_t)expected_context);
    assert(resume_rdx == 0);
}

int main(void)
{
    uint8_t *code = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(code != MAP_FAILED);
    /* Entry shim: mov rsp, rdi ; jmp rsi */
    static const uint8_t shim[] = { 0x48, 0x89, 0xFC, 0xFF, 0xE6 };
    memcpy(code, shim, sizeof(shim));
    uint8_t *entry = code + 64, *body = code + 1024;
    uint64_t *slot = (uint64_t *)(code + 2048);
    *slot = 0;
    beer_ki_dispatcher_stub(entry, body, slot, (uint64_t)(uintptr_t)dispatch);

    /* The layout callers parse: cld; mov rax, [rip+disp32] -> the variable. */
    assert(entry[0] == 0xFC && entry[1] == 0x48 && entry[2] == 0x8B && entry[3] == 0x05);
    int32_t disp;
    memcpy(&disp, entry + 4, 4);
    assert(entry + 8 + disp == (uint8_t *)slot);

    enter(code, entry);
    assert(!prepared);

    *slot = (uint64_t)(uintptr_t)prepare;
    enter(code, entry);
    assert(prepared);
    puts("KiUserExceptionDispatcher stub tests passed");
    return 0;
}
