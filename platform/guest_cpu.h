#ifndef BEER_GUEST_CPU_H
#define BEER_GUEST_CPU_H

/* Guest-CPU seam: everything the loader needs to know about the CPU that runs the guest's x86-64 code.
 *
 * Native x86-64 host (BEER_GUEST_NATIVE): the guest runs directly on the host CPU, so the host's own
 * signal context IS the guest context and the GS base is set with arch_prctl. Everything here maps to
 * exactly what loader.c did before this header existed.
 *
 * Any other host (Android/ARM64): the guest runs under an emulator backend (FEXCore). The loader's crash
 * and exception code keeps working on an x86-shaped context: the backend fills a beer_guest_ucontext_t from
 * the emulator's register state, calls the existing handler logic, and copies the (possibly modified)
 * context back before resuming. The context type uses glibc's x86-64 layout and REG_* indices so that
 * code such as  uc->uc_mcontext.gregs[REG_RIP]  is unchanged. */

#include "beer_arch.h"
#include "win32_entry.h"

#include <signal.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#if BEER_GUEST_NATIVE

typedef ucontext_t beer_guest_ucontext_t;

#define BEER_ARCH_SET_GS 0x1001
#define BEER_ARCH_GET_GS 0x1004

/* GS base of the calling thread; the loader points it at the Windows TEB. Return 0 on success. */
static inline int beer_guest_get_gs_base(uint64_t *base)
{
    return (int)syscall(SYS_arch_prctl, BEER_ARCH_GET_GS, base);
}

static inline int beer_guest_set_gs_base(uint64_t base)
{
    return (int)syscall(SYS_arch_prctl, BEER_ARCH_SET_GS, base);
}

/* Natively the guest calls host functions directly with the Win64 ABI; nothing to register. */
#define BEER_HOST_FLOAT_FN(fn, mask)

#else

/* glibc x86-64 <sys/ucontext.h> register indices. */
enum {
    REG_R8 = 0, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15,
    REG_RDI, REG_RSI, REG_RBP, REG_RBX, REG_RDX, REG_RAX, REG_RCX, REG_RSP,
    REG_RIP, REG_EFL, REG_CSGSFS, REG_ERR, REG_TRAPNO, REG_OLDMASK, REG_CR2,
    BEER_NGREG
};

struct beer_xmmreg { uint32_t element[4]; };

struct beer_fpstate {
    struct beer_xmmreg _xmm[16];
};

typedef struct {
    struct {
        long long gregs[BEER_NGREG];   /* glibc x86-64: signed 64-bit */
        struct beer_fpstate *fpregs;
    } uc_mcontext;
} beer_guest_ucontext_t;

/* ---- Contract the emulator backend implements (not needed natively) ---- */

/* The guest thread's GS base (the TEB pointer). Return 0 on success. */
int beer_guest_get_gs_base(uint64_t *base);
int beer_guest_set_gs_base(uint64_t base);

/* Host -> guest call: run the guest function at guest address `fn` with Win64 arguments (first four in
 * RCX/RDX/R8/R9, the rest on the guest stack) and return RAX. May nest (guest -> host -> guest). */
uint64_t beer_guest_call(uint64_t fn, const uint64_t *args, unsigned nargs);

/* Like beer_guest_call, for functions with float/double arguments. args[] holds raw bit patterns (a float in the low
 * 32 bits). For each of the first four argument positions whose bit is set in float_mask, the value goes in the XMM
 * register of that position (XMM0-XMM3) instead of the integer register, as the Win64 ABI passes it. */
uint64_t beer_guest_call_f(uint64_t fn, const uint64_t *args, unsigned nargs, unsigned float_mask);

/* beer_guest_enter(stack, tls, entry) (win32_entry.h, included above) starts the guest's entry point on the current
 * thread: run the host function tls() first (it calls the executable's TLS callbacks), then enter `entry` with RSP =
 * stack. Natively it is platform/win32_entry.c; emulated, the backend implements it. Does not return. */

/* Run the guest function `fn` with the nonvolatile registers (RBX, RBP, RSI, RDI, R12-R15) and the stack of `regs`
 * (the callee's frame is placed below regs' RSP, 16-byte aligned) and Win64 integer arguments; return RAX. Used for
 * STATUS_UNWIND_CONSOLIDATE, where the C++ runtime's catch-block runner needs the unwound frame's registers. */
uint64_t beer_guest_call_in_context(const beer_guest_ucontext_t *regs, uint64_t fn, const uint64_t *args, unsigned nargs);

/* The guest stack the calling thread's guest code runs on (allocated on first use). The loader publishes it as the
 * TEB's StackBase/StackLimit: guest stack probes (__chkstk) and unwinding read them. Returns 0 on success. */
int beer_guest_thread_stack(uint64_t *low, uint64_t *high);

/* Registers of the guest at the moment it called the import now being handled, as the caller saw them:
 * RIP = return address, RSP = caller's stack pointer after the call returns. */
void beer_guest_caller_regs(beer_guest_ucontext_t *out);

/* Load `regs` into the guest and continue there. Does not return (RtlRestoreContext, SEH unwind). */
void beer_guest_resume(const beer_guest_ucontext_t *regs) __attribute__((noreturn));

/* Where the loader's crash/exception logic is called for a guest fault. The backend rebuilds a
 * beer_guest_ucontext_t from the emulator state, calls this, and applies the (possibly modified) context
 * before resuming the guest. Replaces installing sigaction handlers for guest faults. */
typedef void (*beer_guest_fault_fn)(int sig, siginfo_t *si, void *guest_ucontext);
void beer_guest_set_fault_handler(beer_guest_fault_fn fn);

/* Implemented by the loader, called by the backend for every guest `syscall` import thunk
 * (`mov r10,rcx ; mov eax,<id> ; syscall ; ret`). args[0..3] = RCX(R10)/RDX/R8/R9, args[4..15] = the stack
 * arguments at guest_rsp+0x28. Returns the value for RAX. */
uint64_t beer_guest_import_dispatch(uint64_t id, const uint64_t args[16], const uint64_t xmm[4], uint64_t guest_rsp);

/* Implemented by the loader: call the host function `fn` for a guest call to a host address (a COM vtable entry),
 * with the Win64 arguments the guest passed. args[0..3] are RCX/RDX/R8/R9 and args[4..15] the stack arguments;
 * xmm[0..3] are the low 64 bits of XMM0-XMM3, where Win64 passes float arguments by position. Host functions that take
 * floats are registered with BEER_HOST_FLOAT_FN so each float reaches the callee's floating-point register. */
uint64_t beer_guest_host_call(uint64_t fn, const uint64_t args[16], const uint64_t xmm[4]);

/* Declares that the host function `fn` takes float arguments at the argument positions set in `mask` (bit i = the
 * (i+1)th argument, positions 0-3 only). Emulated hosts need it because Win64 and AAPCS64 place floats differently. */
void beer_host_float_signature(void *fn, unsigned mask);
#define BEER_HOST_FLOAT_FN(fn, mask) \
    static void __attribute__((constructor)) beer_float_signature_##fn(void) { beer_host_float_signature((void *)(fn), (mask)); }

#endif

/* ---- Calling guest code from the host ----
 *
 * BEER_GUEST_CALL(fn, args...) calls a guest function pointer `fn` (a typed Win64 function pointer) with integer
 * and pointer arguments, and yields its result converted to the pointer's return type. Natively it is the direct
 * call it always was. Emulated, a guest address cannot be called as host code, so it goes through
 * beer_guest_call(), which runs the guest function in the emulator (Win64: first four arguments in registers,
 * the rest on the guest stack). Float/double arguments are rejected at compile time (an error at the call
 * site): the emulated path passes integers only, so those call sites need explicit handling. */
#if BEER_GUEST_NATIVE
#define BEER_GUEST_CALL(fn, ...) ((fn)(__VA_ARGS__))
#else
/* A float/double argument must not be silently converted to an integer (it would also travel in the wrong register
 * class). Selecting that case calls a function marked __attribute__((error)), which the compiler reports at the
 * offending call site. */
extern uint64_t beer_gcall_float_argument_unsupported(void)
    __attribute__((error("BEER_GUEST_CALL: float/double arguments are not supported; call this guest function explicitly")));
#define BEER_U64_ARG(x) _Generic((x), \
    float: beer_gcall_float_argument_unsupported(), \
    double: beer_gcall_float_argument_unsupported(), \
    long double: beer_gcall_float_argument_unsupported(), \
    default: ((uint64_t)(uintptr_t)(x)))
#define BEER_ARGC_(_0, _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, N, ...) N
#define BEER_ARGC(...) BEER_ARGC_(0, ##__VA_ARGS__, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)
#define BEER_CAT_(a, b) a##b
#define BEER_CAT(a, b) BEER_CAT_(a, b)
#define BEER_MAP_0(f)
#define BEER_MAP_1(f, a0) f(a0)
#define BEER_MAP_2(f, a0, a1) f(a0), f(a1)
#define BEER_MAP_3(f, a0, a1, a2) f(a0), f(a1), f(a2)
#define BEER_MAP_4(f, a0, a1, a2, a3) f(a0), f(a1), f(a2), f(a3)
#define BEER_MAP_5(f, a0, a1, a2, a3, a4) f(a0), f(a1), f(a2), f(a3), f(a4)
#define BEER_MAP_6(f, a0, a1, a2, a3, a4, a5) f(a0), f(a1), f(a2), f(a3), f(a4), f(a5)
#define BEER_MAP_7(f, a0, a1, a2, a3, a4, a5, a6) f(a0), f(a1), f(a2), f(a3), f(a4), f(a5), f(a6)
#define BEER_MAP_8(f, a0, a1, a2, a3, a4, a5, a6, a7) f(a0), f(a1), f(a2), f(a3), f(a4), f(a5), f(a6), f(a7)
#define BEER_MAP_9(f, a0, a1, a2, a3, a4, a5, a6, a7, a8) f(a0), f(a1), f(a2), f(a3), f(a4), f(a5), f(a6), f(a7), f(a8)
#define BEER_MAP_10(f, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9) f(a0), f(a1), f(a2), f(a3), f(a4), f(a5), f(a6), f(a7), f(a8), f(a9)
#define BEER_MAP_11(f, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10) f(a0), f(a1), f(a2), f(a3), f(a4), f(a5), f(a6), f(a7), f(a8), f(a9), f(a10)
#define BEER_MAP_12(f, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11) f(a0), f(a1), f(a2), f(a3), f(a4), f(a5), f(a6), f(a7), f(a8), f(a9), f(a10), f(a11)
#define BEER_MAP_13(f, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12) f(a0), f(a1), f(a2), f(a3), f(a4), f(a5), f(a6), f(a7), f(a8), f(a9), f(a10), f(a11), f(a12)
#define BEER_MAP_14(f, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13) f(a0), f(a1), f(a2), f(a3), f(a4), f(a5), f(a6), f(a7), f(a8), f(a9), f(a10), f(a11), f(a12), f(a13)
#define BEER_MAP_15(f, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14) f(a0), f(a1), f(a2), f(a3), f(a4), f(a5), f(a6), f(a7), f(a8), f(a9), f(a10), f(a11), f(a12), f(a13), f(a14)
#define BEER_MAP_16(f, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15) f(a0), f(a1), f(a2), f(a3), f(a4), f(a5), f(a6), f(a7), f(a8), f(a9), f(a10), f(a11), f(a12), f(a13), f(a14), f(a15)
#define BEER_MAP(f, ...) BEER_CAT(BEER_MAP_, BEER_ARGC(__VA_ARGS__))(f, ##__VA_ARGS__)
#define BEER_GUEST_CALL(fn, ...) \
    ((__typeof__((fn)(__VA_ARGS__)))beer_guest_call((uint64_t)(uintptr_t)(fn), \
        (const uint64_t[]){0, BEER_MAP(BEER_U64_ARG, ##__VA_ARGS__)} + 1, (unsigned)BEER_ARGC(__VA_ARGS__)))
#endif

#endif
