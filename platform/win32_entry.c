#include "win32_entry.h"

/* Keep the stack switch outside inline C: a CALL hidden in inline asm cannot
 * express GCC's frame/alignment and register-lifetime requirements. SysV input
 * registers are RDI=stack, RSI=TLS runner, RDX=PE entry. */
__asm__(
    ".pushsection .text\n"
    ".global beer_guest_enter\n"
    ".type beer_guest_enter,@function\n"
    "beer_guest_enter:\n"
    "    mov %rdi, %rsp\n"
    /* Entry RSP is 8 mod 16. Reserve 32 bytes of Win64 home space plus
     * 8 bytes for the entry address, making pre-CALL RSP 0 mod 16. */
    "    sub $0x28, %rsp\n"
    "    mov %rdx, 0x20(%rsp)\n"
    "    call *%rsi\n"
    "    mov 0x20(%rsp), %r11\n"
    "    add $0x28, %rsp\n"
    /* As on Windows, the initial thread enters the image with RCX = PEB
     * (BaseThreadInitThunk passes RtlUserThreadStart's argument) and the
     * nonvolatile registers of the zeroed initial context (RDX = entry and R8 = PEB, see below). Carrying Beer's
     * host values in would put host addresses into the guest: RBP held the
     * Linux main() frame (0x7fff...), and RE9's protection, which saves and
     * mixes the caller's registers, then failed its integrity check. */
    /* kernel32!BaseThreadInitThunk(reserved, StartAddress, Parameter) calls the entry with the parameter in RCX and
     * leaves StartAddress in RDX and the parameter in R8 (Wine's entry state is the same). Onimusha's protection
     * mixes them into its control flow; with RDX and R8 zeroed it jumped into the middle of an instruction. */
    "    mov %gs:0x60, %rcx\n"
    "    xor %eax, %eax\n"
    "    mov %r11, %rdx\n"
    "    mov %rcx, %r8\n"
    "    xor %r9d, %r9d\n"
    "    xor %ebx, %ebx\n"
    "    xor %ebp, %ebp\n"
    "    xor %esi, %esi\n"
    "    xor %edi, %edi\n"
    "    xor %r12d, %r12d\n"
    "    xor %r13d, %r13d\n"
    "    xor %r14d, %r14d\n"
    "    xor %r15d, %r15d\n"
    "    jmp *%r11\n"
    ".size beer_guest_enter,.-beer_guest_enter\n"
    ".popsection\n"
);