#include "win32_entry.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

uintptr_t beer_test_entry_rsp;
unsigned beer_test_tls_ran;
extern void __attribute__((ms_abi)) beer_test_tls_runner(void);
extern void beer_test_pe_entry(void);

/* An actual ABI boundary test: TLS uses all home slots and an aligned SIMD
 * store, clobbers volatile registers, and returns. The PE entry checks the
 * exact final RSP, TLS ordering, cleared argument registers, and return slot. */
__asm__(
    ".pushsection .text\n"
    ".global beer_test_tls_runner\n"
    "beer_test_tls_runner:\n"
    "    mov %rsp, %rax\n"
    "    and $15, %eax\n"
    "    cmp $8, %eax\n"
    "    jne 9f\n"
    "    movaps %xmm0, 8(%rsp)\n"
    "    movq $0, 24(%rsp)\n"
    "    movq $0, 32(%rsp)\n"
    "    movl $1, beer_test_tls_ran(%rip)\n"
    "    mov $-1, %rax\n"
    "    mov %rax, %rcx\n"
    "    mov %rax, %rdx\n"
    "    mov %rax, %r8\n"
    "    mov %rax, %r9\n"
    "    mov %rax, %r10\n"
    "    mov %rax, %r11\n"
    "    ret\n"
    ".global beer_test_pe_entry\n"
    "beer_test_pe_entry:\n"
    "    cmp beer_test_entry_rsp(%rip), %rsp\n"
    "    jne 9f\n"
    "    cmpq $0x1234, (%rsp)\n"
    "    jne 9f\n"
    "    cmpl $1, beer_test_tls_ran(%rip)\n"
    "    jne 9f\n"
    "    or %rcx, %rax\n"
    "    or %rdx, %rax\n"
    "    or %r8, %rax\n"
    "    or %r9, %rax\n"
    "    jnz 9f\n"
    "    xor %edi, %edi\n"
    "    jmp 8f\n"
    "9:  mov $1, %edi\n"
    "8:  mov $60, %eax\n"
    "    syscall\n"
    "    ud2\n"
    ".popsection\n"
);

int main(void)
{
    const size_t size = 65536;
    for (unsigned i = 0; i < 16; ++i) {
        unsigned char *stack = mmap(NULL, size, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        assert(stack != MAP_FAILED);
        assert(mprotect(stack, 4096, PROT_NONE) == 0);
        beer_test_entry_rsp = (uintptr_t)(stack + size - 256 - i * 16) + 8;
        *(uintptr_t *)beer_test_entry_rsp = 0x1234;
        pid_t child = fork();
        assert(child >= 0);
        if (!child)
            beer_guest_enter(beer_test_entry_rsp, beer_test_tls_runner,
                             (uintptr_t)beer_test_pe_entry);
        int status = 0;
        assert(waitpid(child, &status, 0) == child);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        assert(munmap(stack, size) == 0);
    }
    puts("Win64 initial TLS/entry ABI tests passed");
    return 0;
}