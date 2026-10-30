#ifndef BEER_WIN32_SYSCALL_H
#define BEER_WIN32_SYSCALL_H

#include <stddef.h>
#include <stdint.h>

/* Native Windows x64 system-service ABI.
 *
 * On Windows every ntdll Nt and Zw export is a short system-call stub, which
 * loads a service number into EAX and executes SYSCALL. Protected executables
 * read those numbers out of ntdll, or carry their own table for the Windows
 * build reported in the PEB, and execute SYSCALL themselves. Beer emits the
 * same stub shape. For services that exist in the Windows build Beer
 * advertises (10.0.19045, see the PEB), it uses that build's service number.
 * Other Beer-implemented services get numbers from BEER_WIN32_SYSCALL_EXTRA_BASE
 * upward.
 *
 * Linux Syscall User Dispatch separates guest system calls from host ones. Each
 * thread that runs guest code enables dispatch with a per-thread selector byte.
 * The selector is BLOCK while guest code runs and ALLOW in Beer's host code.
 * Beer's import trampoline flips it on every guest-to-host transition. A
 * blocked SYSCALL raises SIGSYS. The handler serves guest calls with Beer's
 * implementation, using R10, RDX, R8 and R9 and the stack arguments at
 * [RSP+0x28]. A trapped host call (host code entered without the trampoline,
 * such as a COM method) switches the selector to ALLOW and re-executes it.
 *
 * The 19045 service numbers are the Windows ABI of that build. They are not
 * taken from Wine, whose numbering differs. */

#define BEER_WIN32_SYSCALL_EXTRA_BASE 0x0800u
#define BEER_WIN32_SYSCALL_CAPACITY   0x1000u
#define BEER_WIN32_SYSCALL_STUB_SIZE  24u

#define BEER_STATUS_INVALID_SYSTEM_SERVICE 0xC000001Cu
#define BEER_STATUS_NOT_IMPLEMENTED        0xC0000002u

typedef uint64_t (__attribute__((ms_abi)) *BeerWin32SyscallFn)(
    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

/* Per-thread dispatch selector (0 = allow, 1 = block) and the guest address a
 * serviced call resumes at. They are host ELF TLS so that the import trampoline
 * and the resume stub can address them through %fs. */
extern __thread uint8_t beer_win32_syscall_selector;
extern __thread uint64_t beer_win32_syscall_resume_rip;

/* Returns the service number for a native routine. Nt<Name> and Zw<Name>
 * share one number, as on Windows. A NULL implementation is valid: the service
 * then answers STATUS_NOT_IMPLEMENTED. Returns -1 when the table is full. */
int beer_win32_syscall_register(const char *name, BeerWin32SyscallFn fn);

/* True for exported Nt or Zw names that are system services on Windows. Other
 * names, such as the Ntdll* window procedures, are ordinary functions. */
int beer_win32_syscall_is_service_name(const char *name);

/* Writes the native x64 system-call stub for a service number:
 *   mov r10, rcx; mov eax, <number>; test byte ptr [7FFE0308h], 1;
 *   jne int2e; syscall; ret; int2e: int 2Eh; ret
 * Returns BEER_WIN32_SYSCALL_STUB_SIZE, or 0 when capacity is too small. */
size_t beer_win32_syscall_write_stub(uint8_t *out, size_t capacity, uint32_t number);

/* Installs the SIGSYS handler and enables dispatch on the calling thread.
 * Returns 1 on success and 0 if the host lacks Syscall User Dispatch. */
int beer_win32_syscall_install(void);

/* Enables dispatch on the calling thread (selector ALLOW). Call it on every
 * thread that will run guest code. It does nothing if install failed. */
void beer_win32_syscall_thread_init(void);

#endif
