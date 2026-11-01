/* ntdll!KiUserExceptionDispatcher entry stub (x86-64 machine code, emitted into Beer's ntdll).
 *
 * Windows enters it by jumping to it with RSP at the CONTEXT record and the EXCEPTION_RECORD 0x4F0
 * bytes above, without a return address. The stub calls dispatch(record, context), which returns the
 * address to resume through (RtlRestoreContext), and calls it with (context, NULL); it does not
 * return, but a call gives it the stack alignment of a normal function entry. The CONTEXT's
 * leading home area (P1Home..P6Home) serves as the callee's shadow space, as on Windows. The first
 * instructions are position independent, so hooking libraries can relocate the prologue. */
#ifndef BEER_WIN32_KI_DISPATCHER_H
#define BEER_WIN32_KI_DISPATCHER_H

#include <stdint.h>
#include <string.h>

#define BEER_KI_DISPATCHER_STUB_SIZE 31u
#define BEER_KI_EXCEPTION_RECORD_OFFSET 0x4F0u

static inline void beer_ki_dispatcher_stub(uint8_t *out, uint64_t dispatch)
{
    static const uint8_t head[] = {
        0xFC,                                           /* cld */
        0x48, 0x8D, 0x8C, 0x24, 0xF0, 0x04, 0x00, 0x00, /* lea rcx, [rsp+0x4F0]  ; EXCEPTION_RECORD */
        0x48, 0x89, 0xE2,                               /* mov rdx, rsp          ; CONTEXT */
        0x48, 0xB8,                                     /* mov rax, imm64        ; dispatch */
    };
    static const uint8_t tail[] = {
        0xFF, 0xD0,                                     /* call rax              ; -> resume entry */
        0x48, 0x89, 0xE1,                               /* mov rcx, rsp          ; CONTEXT */
        0x31, 0xD2,                                     /* xor edx, edx          ; no record */
        0xFF, 0xD0,                                     /* call rax              ; never returns; a call keeps
                                                         * the entry alignment the ABI expects */
    };
    _Static_assert(sizeof(head) + 8 + sizeof(tail) == BEER_KI_DISPATCHER_STUB_SIZE, "stub size");
    memcpy(out, head, sizeof(head));
    memcpy(out + sizeof(head), &dispatch, 8);
    memcpy(out + sizeof(head) + 8, tail, sizeof(tail));
}

#endif
