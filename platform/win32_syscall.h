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
 * The already-defined contiguous 19045 prefix retains those IDs. Other
 * Beer-implemented services get local numbers immediately after that prefix,
 * from BEER_WIN32_SYSCALL_EXTRA_BASE upward; these are not claimed as Windows
 * IDs outside the defined prefix. The native image's physical service-stub
 * order matches this complete local table, including unsupported catalog slots.
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

#define BEER_WIN32_SYSCALL_EXTRA_BASE 0x01D9u
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

/* The existing first-party service catalog occupies every slot below EXTRA_BASE.
 * Unimplemented catalog operations return STATUS_NOT_IMPLEMENTED. Implemented
 * extensions follow contiguously, so native stub address rank and encoded ID
 * can agree without importing another runtime's export inventory.
 * Names remain stable for the process lifetime. Initialization/registration
 * occurs before guest execution; readers must not race registration. */
size_t beer_win32_syscall_count(void);
const char *beer_win32_syscall_name(uint32_t number, int zw_alias);
int beer_win32_syscall_supported(uint32_t number);

/* True for exported Nt or Zw names that are system services on Windows. Other
 * names, such as the Ntdll* window procedures, are ordinary functions. */
int beer_win32_syscall_is_service_name(const char *name);

/* Writes the native x64 system-call stub for a service number:
 *   mov r10, rcx; mov eax, <number>; test byte ptr [7FFE0308h], 1;
 *   jne int2e; syscall; ret; int2e: int 2Eh; ret
 * Returns BEER_WIN32_SYSCALL_STUB_SIZE, or 0 when capacity is too small. */
size_t beer_win32_syscall_write_stub(uint8_t *out, size_t capacity, uint32_t number);

/* NtContinue does not return to its caller: it replaces the thread's registers with a CONTEXT.
 * For a guest SYSCALL the loader supplies the conversion, which writes the CONTEXT
 * into the signal frame (a ucontext_t) and returns 0, or returns a failure NTSTATUS
 * and leaves the frame unchanged. */
typedef uint32_t (*BeerWin32ContinueFn)(void *ucontext, uint64_t context, uint64_t test_alert);
void beer_win32_syscall_set_continue(BeerWin32ContinueFn fn);
/* ProcessInstrumentationCallback: when nonzero, every serviced guest system call returns to this address with
 * R10 = the real return address, as the Windows kernel does. Zero removes it. */
void beer_win32_syscall_set_instrumentation_callback(uint64_t callback);

/* Installs the SIGSYS handler and enables dispatch on the calling thread.
 * Returns 1 on success and 0 if the host lacks Syscall User Dispatch. */
int beer_win32_syscall_install(void);

/* True once install succeeded: guest SYSCALL instructions are then trapped and served. Native stubs that
 * execute SYSCALL themselves are only emitted then; otherwise exports jump to Beer directly. */
int beer_win32_syscall_active(void);
/* Re-reads which executable mappings are host code. Call it once the guest images are mapped: a trapped call from
 * an address the table does not know re-reads /proc/self/maps, which is slow enough for protection code that
 * times its system calls to notice. */
void beer_win32_syscall_refresh_ranges(void);

/* Enables dispatch on the calling thread (selector ALLOW). Call it on every
 * thread that will run guest code. It does nothing if install failed. */
void beer_win32_syscall_thread_init(void);

#endif
