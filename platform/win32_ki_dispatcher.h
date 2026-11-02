/* ntdll!KiUserExceptionDispatcher stub (x86-64 machine code, emitted into Beer's ntdll).
 *
 * Windows enters it by jumping to it with RSP at the CONTEXT record and the EXCEPTION_RECORD 0x4F0
 * bytes above, without a return address. Like Windows' x64 ntdll, the entry first loads ntdll's
 * Wow64PrepareForException pointer RIP-relatively and, when it is set, calls it with
 * (record, context) before dispatching; code that installs an early exception callback locates that
 * variable through the entry's leading "cld; mov rax, [rip+disp32]".
 *
 * The body calls dispatch(record, context), which returns the address to resume through
 * (RtlRestoreContext), and calls it with (context, NULL); it does not return, but a call gives it
 * the stack alignment of a normal function entry. The CONTEXT's leading home area (P1Home..P6Home)
 * serves as the callees' shadow space, as on Windows. The entry is position independent apart from
 * RIP-relative operands, so hooking libraries can relocate its prologue. */
#ifndef BEER_WIN32_KI_DISPATCHER_H
#define BEER_WIN32_KI_DISPATCHER_H

#include <stdint.h>
#include <string.h>

#define BEER_KI_DISPATCHER_ENTRY_SIZE 31u
#define BEER_KI_DISPATCHER_BODY_SIZE 31u
#define BEER_KI_EXCEPTION_RECORD_OFFSET 0x4F0u

static inline void beer_ki_put_rel32(uint8_t *field, const void *target, const uint8_t *next)
{
    int32_t rel = (int32_t)((intptr_t)target - (intptr_t)next);
    memcpy(field, &rel, 4);
}

/* entry, body and *prepare must lie within +-2 GiB of each other (one image). */
static inline void beer_ki_dispatcher_stub(uint8_t *entry, uint8_t *body, const uint64_t *prepare,
                                           uint64_t dispatch)
{
    static const uint8_t prologue[] = {
        0xFC,                                           /* cld */
        0x48, 0x8B, 0x05, 0, 0, 0, 0,                   /* mov rax, [rip+Wow64PrepareForException] */
        0x48, 0x85, 0xC0,                               /* test rax, rax */
        0x74, 0x0D,                                     /* jz body */
        0x48, 0x8D, 0x8C, 0x24, 0xF0, 0x04, 0x00, 0x00, /* lea rcx, [rsp+0x4F0]  ; EXCEPTION_RECORD */
        0x48, 0x89, 0xE2,                               /* mov rdx, rsp          ; CONTEXT */
        0xFF, 0xD0,                                     /* call rax */
        0xE9, 0, 0, 0, 0,                               /* jmp body */
    };
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
    _Static_assert(sizeof(prologue) == BEER_KI_DISPATCHER_ENTRY_SIZE, "entry size");
    _Static_assert(sizeof(head) + 8 + sizeof(tail) == BEER_KI_DISPATCHER_BODY_SIZE, "body size");
    memcpy(entry, prologue, sizeof(prologue));
    beer_ki_put_rel32(entry + 4, prepare, entry + 8);
    beer_ki_put_rel32(entry + 27, body, entry + 31);
    memcpy(body, head, sizeof(head));
    memcpy(body + sizeof(head), &dispatch, 8);
    memcpy(body + sizeof(head) + 8, tail, sizeof(tail));
}

#endif
