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
    "    xor %eax, %eax\n"
    "    xor %ecx, %ecx\n"
    "    xor %edx, %edx\n"
    "    xor %r8d, %r8d\n"
    "    xor %r9d, %r9d\n"
    "    jmp *%r11\n"
    ".size beer_guest_enter,.-beer_guest_enter\n"
    ".popsection\n"
);