/*
 * loader.c — Incremental Windows PE64 loader for Linux
 *
 * Strategy: parse the PE, stub *every* imported function (logs call + returns 0),
 * set up a minimal TEB/PEB, point GS at it, then jump to the entry point.
 * Watch the [STUB] lines to see what the exe calls, implement them one by one.
 *
 * Build:  make
 * Run:    ./loader [path/to/exe]
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <dlfcn.h>
#include <unistd.h>

/* arch_prctl – set GS base so gs:[0x30] / gs:[0x60] work as TEB/PEB */
#ifndef ARCH_SET_GS
# define ARCH_SET_GS 0x1001
#endif
#ifndef ARCH_GET_GS
# define ARCH_GET_GS 0x1004
#endif
/* Added in Linux 4.17 – graceful fallback if absent */
#ifndef MAP_FIXED_NOREPLACE
# define MAP_FIXED_NOREPLACE 0x100000
#endif

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t  s32;

/* ── PE structures ──────────────────────────────────────────────── */
#pragma pack(push, 1)

typedef struct { u16 magic; u8 pad[58]; u32 lfanew;          } DosHdr;
typedef struct { u32 rva; u32 size;                           } DataDir;
typedef struct { u32 page_rva; u32 block_sz;                  } RelocBlock;

typedef struct {
    u16 machine, nsections;
    u32 timestamp, symptr, nsymbols;
    u16 opthdr_sz, chars;
} FileHdr;

typedef struct {
    u16 magic; u8 link_maj, link_min;
    u32 sz_code, sz_initdata, sz_uninit, entry_rva, base_of_code;
    u64 imagebase;
    u32 sec_align, file_align;
    u16 os_maj, os_min, img_maj, img_min, sub_maj, sub_min;
    u32 win32ver, sz_image, sz_headers, checksum;
    u16 subsystem, dll_chars;
    u64 stack_reserve, stack_commit, heap_reserve, heap_commit;
    u32 loader_flags, nrva_sizes;
    DataDir dirs[16];
} OptHdr64;

typedef struct { u32 sig; FileHdr file; OptHdr64 opt; } NtHdrs64;

typedef struct {
    char name[8];
    u32 vsz, vrva, raw_sz, raw_off, reloc_off, lineno_off;
    u16 nrelocs, nlinenos;
    u32 chars;
} SecHdr;

typedef struct { u32 orig_ilt, timestamp, fwd, name_rva, iat_rva; } ImportDesc;

#pragma pack(pop)

#define MZ_MAGIC 0x5A4D
#define PE_SIG   0x00004550
#define PE32PLUS 0x020B

#define SCN_EXEC  0x20000000
#define SCN_READ  0x40000000
#define SCN_WRITE 0x80000000

#define DIR_IMPORT 1
#define DIR_RELOC  5

/* ── Globals ────────────────────────────────────────────────────── */
static u8    *g_img     = NULL;   /* image base in our VA space    */
static u64    g_delta   = 0;      /* relocation delta              */

/* Forward declarations: these are fully declared later (g_tramp/g_trampsz
 * in the stub infrastructure section, image_addr_is_exec near the PE
 * loader), but the crash-recovery helper functions defined just below
 * need them earlier in translation-unit order. */
static u8         *g_tramp;
static size_t      g_trampsz;
static int image_addr_is_exec(u64 addr);

/* Crash-recovery loop tracking for fallback redirect gating. */
static u64 g_fb_total = 0;
static u64 g_fb_last_bucket = ~0ULL;
static u32 g_fb_bucket_hits = 0;
static u64 g_lowrip_last_rsp_page = ~0ULL;
static u32 g_lowrip_rsp_hits = 0;
static u64 g_hotspot_hits = 0;
static u32 g_posttxn_escapes = 0;
static u64 g_exec_last_rip = ~0ULL;
static u32 g_exec_rip_hits = 0;
static u64 g_exec_last_bucket = ~0ULL;
static u32 g_exec_bucket_hits = 0;
static u32 g_exec_stack_redirects = 0;
static u64 g_null_cti_hot_hits = 0;
static u64 g_repeated_null_write_rip = 0;
static u32 g_repeated_null_write_hits = 0;
static u64 g_stack_exec_rip = 0;
static u64 g_stack_exec_rsp = 0;
static u32 g_stack_exec_hits = 0;
static u64 g_stack_loop_last_rip = 0;
static u32 g_stack_loop_hits = 0;
static u64 g_stack_recovery_rip = 0;
static u64 g_stack_recovery_rsp = 0;
static u32 g_stack_recovery_hits = 0;
static u64 g_stale_guest_stack_rip = 0;
static u32 g_stale_guest_stack_hits = 0;
static u64 g_bad_stack_operand = 0;
static u32 g_bad_stack_operand_hits = 0;

/* Cross-site dead-cycle detector: the various named recovery gates each
 * reset their own "consecutive same bucket" counters whenever the fault
 * RIP moves to a different page, so a handler that bounces between a
 * fixed small set of fallback RVAs (e.g. A -> B -> C -> D -> A ...) never
 * trips any single gate's threshold — every hop looks like a fresh
 * bucket with hit count 1, yet the process never makes forward progress.
 * Track the last faulting RIPs globally and detect short repeating
 * cycles (period 2..CYCLE_HIST_PERIOD_MAX) so we can abort with a
 * diagnostic instead of spinning silently forever.
 */
#define CYCLE_HIST_LEN 64
#define CYCLE_HIST_PERIOD_MAX 8
#define CYCLE_HIST_MIN_REPEATS 4
static u64 g_cycle_hist[CYCLE_HIST_LEN];
static u32 g_cycle_hist_n = 0;   /* number of entries recorded, saturating */
static u64 g_cycle_total_calls = 0;

/* Persistent ret-stub cycle breaker: track total resets to baseline, give up after 2 */
static u32 g_ret_stub_baseline_resets = 0;

/* Middle-init fault limit: give up if too many faults in the init range */
static u32 g_mid_init_fault_count = 0;
static const u32 MID_INIT_FAULT_LIMIT = 50;  /* Abort if we see >50 faults without escaping the range */

/* Progress tracking: APIs called on the path to window creation */
static u32 g_api_registerclass = 0;
static u32 g_api_createwindow = 0;
static u32 g_api_createfactory = 0;
static u32 g_api_createdevice = 0;
static u32 g_api_first_api_call = 0;

/* RSP observed just before handing off to the PE entry point — the
 * shallowest legitimate stack depth for the whole run. Any "recovered"
 * RSP produced by speculative stack-popping that ends up ABOVE this
 * (closer to StackBase, i.e. shallower than the outermost frame ever
 * was) has walked past all real content into never-written stack
 * memory and cannot contain a valid return address. */
static u64 g_entry_rsp = 0;
static u64 g_entry_rsp_limit = 0;
static u64 g_guest_stack_low = 0;
static u64 g_guest_stack_high = 0;

/* Diagnostics: track which path beer_dispatch_trampoline takes */
__thread int g_trampoline_path = 0;  /* 0=outer, 1=nested */
/* Address of the seeded caller-frame return slot at g_entry_rsp+0x5c8
 * (see main()'s guest_ret_slot2), pre-populated with a real, validated
 * in-image continuation address (guest_initial_ret). Every crash-handler
 * path that "resets to the entry baseline" immediately jumps RIP to the
 * `xor eax,eax; ret` stub at RVA 0x235a694, which *pops* whatever is at
 * the current %rsp and jumps there. Resetting %rsp to g_entry_rsp itself
 * pointed at never-written (zero) stack memory, so that `ret` faithfully
 * popped 0 and jumped to address 0 -- an infinite, guaranteed-to-repeat
 * bug, not a recovered fault. Resetting to this slot instead means the
 * `ret` pops a real, pre-validated return address. */
static u64 g_entry_ret_slot = 0;

static int is_guest_stack_rsp(u64 rsp)
{
    if (!g_guest_stack_low || !g_guest_stack_high)
        return 1; /* no guest stack window recorded yet: permissive until setup */
    return rsp > g_guest_stack_low + 0x200 && rsp < g_guest_stack_high - 0x200;
}

static void clamp_rsp_to_entry_baseline(ucontext_t *uc, u64 rsp)
{
    if (g_entry_rsp_limit && rsp > g_entry_rsp_limit)
        uc->uc_mcontext.gregs[REG_RSP] = (greg_t)g_entry_rsp;
}

static u64 guest_resume_rip(void)
{
    u64 ret = (u64)g_img ? ((u64)g_img + 0x235a120) : 0;
    if (!g_img || !image_addr_is_exec(ret))
        ret = (u64)g_img + 0x235a694;
    return ret;
}

static void seed_guest_entry_frame(u64 ret_target)
{
    if (!g_entry_rsp) return;
    if (!ret_target)
        ret_target = guest_resume_rip();

    /* The helper at 0x235a636..0x235a654 does:
     *     mov rbx, [rsp+0x5d0]
     *     add rsp, 0x5c0
     *     pop rbp
     *     ret
     * so the valid caller frame is anchored at the current %rsp and the saved
     * return address lives at [rsp+0x5c8]. We seed the frame at that real
     * caller-base, not at a synthetic offset that would make the next RET pop a
     * stale value. */
    for (u64 p = g_entry_rsp; p + 8 <= g_entry_rsp + 0x1000; p += 8)
        *(u64 *)p = 0;

    u64 rbp_slot = g_entry_rsp + 0x5c0;
    u64 ret_slot = g_entry_rsp + 0x5c8;
    u64 frame_ptr_slot = g_entry_rsp + 0x5d0;
    *(u64 *)rbp_slot = rbp_slot;
    *(u64 *)ret_slot = ret_target;
    *(u64 *)frame_ptr_slot = ret_slot;
    if (g_entry_ret_slot)
        *(u64 *)g_entry_ret_slot = ret_target;
}

static void reset_rsp_to_entry_baseline(ucontext_t *uc)
{
    /* Restore the real caller-frame base that the helper expects. If we reset to
     * an offset beneath the frame or to the return-slot itself, the next RET
     * pops from the wrong address and the guest never makes forward progress. */
    if (g_entry_rsp) {
        u64 ret_target = guest_resume_rip();
        seed_guest_entry_frame(ret_target);
        uc->uc_mcontext.gregs[REG_RSP] = (greg_t)g_entry_rsp;
        uc->uc_mcontext.gregs[REG_RBP] = (greg_t)(g_entry_rsp + 0x5c0);
    } else {
        fprintf(stderr, "[WARN] reset_rsp_to_entry_baseline called but g_entry_rsp=0!\n");
    }
}

/* Guard against resuming guest execution with %rsp pointing somewhere that
 * is definitely not the guest stack (e.g. still parked inside
 * g_host_call_stack after a wild jump/`ret` chased a stale value off of the
 * host-only call stack). is_guest_stack_rsp() only checks the *window*
 * (low..high), not direction relative to g_entry_rsp, so this catches the
 * case the older "rsp >= g_entry_rsp" checks missed: rsp landing in a
 * completely different memory region (numerically far below the guest
 * stack, e.g. the PIE-mapped loader .bss) rather than merely being too
 * shallow within the guest stack itself. Left untouched otherwise, since
 * blindly resetting a merely-deep-but-valid guest rsp would discard live
 * frames unnecessarily. */
static void ensure_valid_guest_rsp(ucontext_t *uc)
{
    u64 rsp = (u64)uc->uc_mcontext.gregs[REG_RSP];
    if (!is_guest_stack_rsp(rsp))
        reset_rsp_to_entry_baseline(uc);
}

static int is_host_stack_rip(u64 rip)
{
    return rip >= 0x7ff000000000ULL && rip < 0x800000000000ULL;
}

static int is_poisoned_stack_addr(u64 addr)
{
    /* Uninitialized stack-memory poison from Win64 CRT/SEH code manifests as a
     * high address whose low 8 bytes are the 0xCCCCCCCCCCCCCCCC sentinel pattern.
     * Treat this as a stale stack pointer, not a valid guest memory access. */
    return (addr & 0xFFFFFFFFFFFFFF00ULL) == 0xFFFFFFFFCCCCCC00ULL;
}

static int is_in_image_exec_range(u64 addr)
{
    return g_img && addr >= (u64)g_img && addr < (u64)g_img + 0x42d2000 && image_addr_is_exec(addr);
}

static int is_in_trampoline_range(u64 addr)
{
    return g_tramp && addr >= (u64)g_tramp && addr < (u64)(g_tramp + g_trampsz);
}

static int is_known_guest_loop_island(u64 addr)
{
    if (!g_img) return 0;
    u64 base = (u64)g_img;
    struct {
        u64 start, end;
    } const bad[] = {
        { 0x235a636ULL, 0x235a6c5ULL },
        { 0x239c620ULL, 0x239c650ULL },
        { 0x23ab4e0ULL, 0x23ab510ULL },
        { 0x23ab800ULL, 0x23ab850ULL },  /* Loop with RSI=π fault at 0x23ab81f */
        { 0x23adf00ULL, 0x23ae04fULL },
        { 0x23ae170ULL, 0x23ae1d0ULL },
        { 0x23b7000ULL, 0x23b70c0ULL },
        { 0x23b7a20ULL, 0x23b7ac0ULL },
        { 0x23bb640ULL, 0x23bb6d0ULL },
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        u64 s = base + bad[i].start;
        u64 e = base + bad[i].end;
        if (addr >= s && addr <= e)
            return 1;
    }
    return 0;
}

static int is_valid_resume_target(u64 addr)
{
    if (!addr) return 0;
    if (addr >= 0x7ff000000000ULL && addr < 0x800000000000ULL)
        return 0; /* host stack is never a guest continuation */
    if (is_in_image_exec_range(addr))
        return !is_known_guest_loop_island(addr);
    return is_in_trampoline_range(addr);
}

static int try_single_slot_stack_resume(u64 rsp, u64 *ret_out)
{
    if (!ret_out || !rsp) return 0;
    if (!is_guest_stack_rsp(rsp)) return 0;
    u64 *sp = (u64 *)rsp;
    if (!sp) return 0;
    u64 cand = sp[0];
    if (!is_valid_resume_target(cand)) return 0;
    if (g_entry_rsp_limit && rsp + 8 > g_entry_rsp_limit) return 0;
    *ret_out = cand;
    return 1;
}

static void record_host_stack_fault(u64 rip, u64 rsp, u64 faultaddr)
{
    if (!is_host_stack_rip(rip) || faultaddr != rip)
        return;
    if (g_stack_exec_rip == rip && g_stack_exec_rsp == rsp) {
        g_stack_exec_hits++;
    } else {
        g_stack_exec_rip = rip;
        g_stack_exec_rsp = rsp;
        g_stack_exec_hits = 1;
    }
    if (g_stack_exec_hits >= 3) {
        fprintf(stderr,
                "[FATAL] Repeated host-stack execute-fault loop at RIP=0x%lx RSP=0x%lx; dead function-pointer loop; aborting.\n",
                rip, rsp);
        fflush(stderr);
        _exit(2);
    }
}

static int is_probably_stack_value(u64 v)
{
    if (!v) return 0;
    if (v >= 0x7ff000000000ULL && v < 0x800000000000ULL)
        return 1;
    if (g_img && v >= (u64)g_img && v < (u64)g_img + 0x42d2000)
        return image_addr_is_exec(v);
    return 0;
}

static int probe_stack_resume_window(u64 rsp, u64 *ret_out)
{
    if (!ret_out || !rsp) return 0;
    if (!is_guest_stack_rsp(rsp)) return 0;
    u64 *sp = (u64 *)rsp;
    if (!sp) return 0;
    for (int i = 0; i < 8; i++) {
        u64 cand = sp[i];
        if (!is_valid_resume_target(cand)) continue;
        if (g_entry_rsp_limit && rsp + ((u64)i + 1) * 8 > g_entry_rsp_limit)
            continue;
        *ret_out = cand;
        return 1;
    }
    return 0;
}

static void force_safe_fail_return(ucontext_t *uc, u64 rip)
{
    u64 target = guest_resume_rip();
    uc->uc_mcontext.gregs[REG_RAX] = 0;

    /* Prefer a direct resume to the real startup continuation instead of
     * ret'ing through the fail stub. The fail stub does a stack pop and can
     * re-enter stale stack memory; a direct jump keeps the guest on a known
     * in-image path while preserving the recovered synthetic caller frame. */
    reset_rsp_to_entry_baseline(uc);
    uc->uc_mcontext.gregs[REG_RIP] = (greg_t)target;
    fprintf(stderr,
            "[SAFE] Forcing guest resume after loop at RIP=0x%lx via RVA 0x%lx (RSP reset to seeded return slot 0x%lx)\n",
            rip, (unsigned long)target, (unsigned long)g_entry_ret_slot);
}

static int is_repeating_host_stack_loop(u64 rip, u64 rsp, u64 faultaddr)
{
    if (!is_host_stack_rip(rip) || faultaddr != rip)
        return 0;
    if (g_stack_exec_rip == rip && g_stack_exec_rsp == rsp)
        return g_stack_exec_hits >= 2;
    return 0;
}

static void record_bad_stack_operand(u64 value)
{
    if (!value) return;
    if (g_bad_stack_operand == value)
        g_bad_stack_operand_hits++;
    else {
        g_bad_stack_operand = value;
        g_bad_stack_operand_hits = 1;
    }
}

static int is_bad_stack_operand_repeat(u64 value)
{
    return value && g_bad_stack_operand == value && g_bad_stack_operand_hits >= 2;
}

/* Returns nonzero and fills *out_period and *out_repeats if the most recent
 * entries in g_cycle_hist consist of a period-P repeating pattern occurring
 * at least CYCLE_HIST_MIN_REPEATS times back-to-back. */
static int detect_stuck_cycle(int *out_period, int *out_repeats)
{
    if (g_cycle_hist_n < (u32)(CYCLE_HIST_MIN_REPEATS * 2))
        return 0;

    u32 n = g_cycle_hist_n < CYCLE_HIST_LEN ? g_cycle_hist_n : CYCLE_HIST_LEN;

    for (int period = 2; period <= CYCLE_HIST_PERIOD_MAX; period++) {
        u32 need = (u32)(period * CYCLE_HIST_MIN_REPEATS);
        if (need > n) continue;

        int matches = 1;
        for (u32 i = 0; i < need - (u32)period && matches; i++) {
            u32 a = (g_cycle_hist_n - 1 - i) % CYCLE_HIST_LEN;
            u32 b = (g_cycle_hist_n - 1 - i - (u32)period) % CYCLE_HIST_LEN;
            if (g_cycle_hist[a] != g_cycle_hist[b])
                matches = 0;
        }
        if (matches) {
            if (out_period) *out_period = period;
            if (out_repeats) *out_repeats = (int)(need / (u32)period);
            return 1;
        }
    }
    return 0;
}

static void cycle_hist_push(u64 rip)
{
    g_cycle_hist[g_cycle_hist_n % CYCLE_HIST_LEN] = rip;
    g_cycle_hist_n++;
    g_cycle_total_calls++;
}

/* ── Stub infrastructure ────────────────────────────────────────── */
#define MAX_STUBS 8192
#define THUNK_SZ  24   /* bytes per trampoline thunk                */

static const char *g_snames[MAX_STUBS];
static int         g_nstubs  = 0;
static u8         *g_tramp   = NULL;
static size_t      g_trampsz = 0;

/*
 * ------------------------------------------------------------------
 * Dedicated host call stack for guest -> impl_* transitions.
 *
 * main() switches the *real* %rsp to a synthetic guest stack before
 * jumping into the PE entry point, so from that moment on every guest
 * CALL (and therefore every jmp-thunk into our own C implementations)
 * executes with %rsp still pointing into that same guest-owned mmap.
 * Deep host call chains (e.g. impl_InitializeCriticalSectionAndSpinCount
 * -> cs_get_or_create_mutex -> malloc -> glibc's tcache internals) then
 * run with their C stack frames physically inside guest memory instead
 * of a stack dedicated to host execution.
 *
 * beer_dispatch_trampoline() (defined below in raw asm so it can safely
 * juggle %rsp without disturbing the incoming Windows x64 argument
 * registers rcx/rdx/r8/r9) saves the guest's %rsp, switches to a
 * dedicated host-only stack, calls the real impl_* function, then
 * restores the guest %rsp before returning control to the guest via the
 * original return address the guest itself pushed. This keeps host C
 * execution off the guest stack entirely.
 *
 * NOTE: g_host_call_stack/g_saved_guest_rsp are process-global, not
 * per-thread, so this is only safe while a single guest thread is
 * calling into impl_* functions at a time. This matches the current
 * bring-up state (crash reproduces long before the guest calls
 * CreateThread) but MUST become thread-local before relying on this
 * once multiple guest threads are active concurrently.
 * ------------------------------------------------------------------ */
#define HOST_CALL_STACK_SIZE (1 << 20) /* 1 MiB */
#define HOST_CALL_STACK_SIZE_STR "1048576" /* must match HOST_CALL_STACK_SIZE */
static u8  g_host_call_stack[HOST_CALL_STACK_SIZE] __attribute__((aligned(16)));
_Static_assert(HOST_CALL_STACK_SIZE == 1048576,
               "HOST_CALL_STACK_SIZE_STR must match HOST_CALL_STACK_SIZE");
/* Per-thread host call stack state (accessed from the trampoline asm via
 * %fs:sym@tpoff). Every thread that runs guest code must call
 * host_stack_init_thread() first. The main thread uses g_host_call_stack. */
__thread u64 g_host_call_stack_base;
__thread u64 g_host_call_stack_top;
__thread u64 g_saved_guest_rsp;
static u64 g_impl_targets[MAX_STUBS];

/* ── Minimal Windows TEB / PEB ──────────────────────────────────── */
/*
 * Windows x64 uses gs:[offset] to access TEB fields.
 * The most important offsets:
 *   gs:[0x00]  ExceptionList
 *   gs:[0x08]  StackBase
 *   gs:[0x10]  StackLimit
 *   gs:[0x30]  Self (pointer to TEB itself)
 *   gs:[0x60]  PEB pointer
 *
 * We set the GS base MSR to point at our TEB via arch_prctl(ARCH_SET_GS).
 */
typedef struct {
    u64 ExceptionList;          /* +0x000 */
    u64 StackBase;              /* +0x008 */
    u64 StackLimit;             /* +0x010 */
    u8  _pad0[0x30 - 0x18];
    u64 Self;                   /* +0x030 */
    u8  _pad1[0x60 - 0x38];
    u64 ProcEnvBlk;             /* +0x060 */
    u8  _pad2[0xA0 - 0x68];
    u64 SavedGuestRsp;          /* +0x0A0 for beer_dispatch_trampoline */
    u64 HostCallStackBase;      /* +0x0A8 for beer_dispatch_trampoline */
    u64 HostCallStackTop;       /* +0x0B0 for beer_dispatch_trampoline */
    u8  _pad3[0x1000 - 0xB8];
} WinTEB;

/* Forward declaration of WinTEB */
WinTEB;

static void host_stack_init_thread(void)
{
    if (g_host_call_stack_base) return;
    void *m = mmap(NULL, HOST_CALL_STACK_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) { perror("mmap(host_call_stack)"); _exit(2); }
    g_host_call_stack_base = (u64)m;
    g_host_call_stack_top = ((u64)m + HOST_CALL_STACK_SIZE - 256) & ~0xFULL;
    
    /* Also update the TEB fields for beer_dispatch_trampoline to use */
    u64 gs_base = 0;
    if (syscall(SYS_arch_prctl, ARCH_GET_GS, &gs_base) == 0 && gs_base) {
        WinTEB *teb = (WinTEB *)gs_base;
        teb->HostCallStackBase = g_host_call_stack_base;
        teb->HostCallStackTop = g_host_call_stack_top;
    }
}

/*
 * Reentrancy note: an impl_* function can itself call back into guest
 * code (e.g. impl_EnumSystemLocalesW invoking the guest's locale-enum
 * callback), and that guest code can make further Windows API calls,
 * re-entering this very trampoline before the outer call has returned.
 *
 * The original version unconditionally saved %rsp into a single global
 * (g_saved_guest_rsp) and reset %rsp to a FIXED g_host_call_stack_top on
 * every entry. On a nested/reentrant call this (a) stomped the outer
 * call's still-live host stack frame by resetting %rsp back to the same
 * fixed top address instead of continuing to grow downward from where
 * the outer call currently was, and (b) clobbered g_saved_guest_rsp with
 * the inner call's guest %rsp, so the OUTER call's trailing restore used
 * the wrong (inner) value once it eventually returned. Together these
 * corrupted the real guest stack pointer, producing a deterministic
 * crash loop.
 *
 * Fix: only switch onto the dedicated host stack (and only save/restore
 * g_saved_guest_rsp) on the OUTERMOST entry — detected by checking
 * whether %rsp already lies inside g_host_call_stack. A nested entry
 * just issues the call in place, so ordinary CALL/RET semantics grow
 * the real hardware stack downward from wherever the outer call left
 * it, instead of resetting to a fixed address.
 */

/* Sync SavedGuestRsp from TEB to __thread variable for C code */
static inline void sync_saved_guest_rsp_from_teb(void) {
    u64 gs_base = 0;
    if (syscall(SYS_arch_prctl, ARCH_GET_GS, &gs_base) == 0 && gs_base) {
        WinTEB *teb = (WinTEB *)gs_base;
        g_saved_guest_rsp = teb->SavedGuestRsp;
    }
}

/* Helper for trampoline return path */
static u64 __attribute__((ms_abi)) get_saved_guest_rsp_from_teb(void) {
    u64 gs_base = 0;
    if (syscall(SYS_arch_prctl, ARCH_GET_GS, &gs_base) == 0 && gs_base) {
        WinTEB *teb = (WinTEB *)gs_base;
        u64 val = teb->SavedGuestRsp;
        static int call_count = 0;
        if (call_count++ < 5) {  /* Log first 5 calls only */
            fprintf(stderr, "[TEB_READ] SavedGuestRsp=%p (from TEB @ %p)\n", (void*)val, (void*)gs_base);
        }
        return val;
    }
    fprintf(stderr, "[TEB_READ] ERROR: Could not get GS base!\n");
    return 0;
}

__asm__(
    ".text\n"
    ".global beer_dispatch_trampoline\n"
    "beer_dispatch_trampoline:\n"
    "    mov %rsp, %rax\n"
    "    sub %gs:0xA8, %rax\n"              /* Subtract HostCallStackBase from TEB @ 0xA8 */
    "    cmp $" HOST_CALL_STACK_SIZE_STR ", %rax\n"
    "    jb 1f\n"                           /* (rsp - base) < SIZE => already on it */
    "    mov %rsp, %gs:0xA0\n"              /* Save to TEB SavedGuestRsp @ 0xA0 */
    "    mov %gs:0xB0, %rsp\n"              /* Switch to HostCallStackTop from TEB @ 0xB0 */
    "    lea g_impl_targets(%rip), %r11\n"
    "    mov (%r11,%r10,8), %rax\n"
    /* Windows x64 ABI requires the CALLER to reserve 32 bytes of "shadow
     * space" directly below the return address for the callee's own use
     * (ms_abi callees routinely spill their register args there, e.g.
     * plain "mov %ecx, 0x10(%rbp)"-style prologue code). g_host_call_stack_top
     * is already 16-byte aligned, and 0x20 is a multiple of 16, so this
     * preserves the required pre-call alignment while carving out the
     * mandatory gap. Without it, the callee's shadow-space writes land
     * directly on whatever is sitting above our just-pushed return
     * address on the shared host call stack. */
    /* Stack-passed args (5th and later) live at guest_rsp+0x28...; the
     * callee expects them at its own rsp+0x28. Forward a fixed window of 12
     * qwords (over-copying is harmless). rax/r11 are scratch; the target is
     * parked in r10 (index already consumed). */
    "    mov %rax, %r10\n"
    "    mov %gs:0xA0, %r11\n"             /* Changed: load from TEB SavedGuestRsp */
    "    sub $0x80, %rsp\n"
    ".irp off,0,8,16,24,32,40,48,56,64,72,80,88\n"
    "    mov 0x28+\\off(%r11), %rax\n"
    "    mov %rax, 0x20+\\off(%rsp)\n"
    ".endr\n"
    "    call *%r10\n"
    "    add $0x80, %rsp\n"
    "    lea get_saved_guest_rsp_from_teb(%rip), %r11\n"  /* Call helper */
    "    call *%r11\n"
    "    mov %rax, %rsp\n"                  /* Restore guest RSP */
    "    pop %rax\n"                        /* Pop return address from guest stack */
    "    jmp *%rax\n"                       /* Jump to return address */
    "1:\n"                                /* nested: keep using the current (already-switched) rsp */
    "    lea g_impl_targets(%rip), %r11\n"
    "    mov (%r11,%r10,8), %rax\n"
    /* Same shadow-space requirement applies here, plus the incoming %rsp
     * (inherited from whatever nested guest/host call chain got us here)
     * is not guaranteed to be 16-byte aligned before this call. Save the
     * exact incoming %rsp in %r10 (free now that the impl target has
     * already been fetched via it), align down and reserve shadow space
     * for the call, then restore %rsp to the EXACT saved value afterward
     * so the final `ret` still pops the real return address that was
     * already sitting there untouched -- we only ever write into the
     * newly carved-out region strictly below it. */
    "    mov %rax, %r10\n"
    "    mov %rsp, %r11\n"
    "    and $-16, %rsp\n"
    "    sub $0x90, %rsp\n"
    "    mov %r11, 0x80(%rsp)\n"            /* exact incoming rsp, above the arg window */
    ".irp off,0,8,16,24,32,40,48,56,64,72,80,88\n"
    "    mov 0x28+\\off(%r11), %rax\n"
    "    mov %rax, 0x20+\\off(%rsp)\n"
    ".endr\n"
    "    call *%r10\n"
    "    mov 0x80(%rsp), %rsp\n"
    "    ret\n"
);
extern void beer_dispatch_trampoline(void);

/*
 * stub_dispatch – called by every trampoline.
 * __attribute__((ms_abi)) means GCC uses Windows x64 calling convention:
 *   idx → rcx (first param), a1 → rdx, a2 → r8, a3 → r9.
 * The thunk sets ecx = index and jmps here, so idx is our stub index.
 */
static u64 __attribute__((ms_abi))
stub_dispatch(u64 idx, u64 a1, u64 a2, u64 a3)
{
    const char *name = (idx < (u64)g_nstubs) ? g_snames[idx] : "??";
    fprintf(stderr, "[STUB] %-60s  (0x%lx, 0x%lx, 0x%lx) -> 0\n",
            name, a1, a2, a3);
    return 0;
}

/*
 * Emit a 24-byte thunk at g_tramp[idx * THUNK_SZ]:
 *
 *   B9 ii ii ii ii       mov ecx, <idx>          (5 bytes)
 *   48 B8 hh*8           mov rax, stub_dispatch  (10 bytes)
 *   FF E0                jmp rax                 (2 bytes)
 *   90 * 7               nop padding             (7 bytes)
 *
 * JMP (not CALL): stub_dispatch returns directly to the Windows caller.
 * ecx = rcx = first Windows x64 argument → idx arrives as parameter 0.
 */
static void emit_thunk(int idx)
{
    u8  *p  = g_tramp + idx * THUNK_SZ;
    u64  fn = (u64)stub_dispatch;

    p[0] = 0xB9;
    memcpy(p + 1, &idx, 4);          /* mov ecx, idx  */

    p[5] = 0x48; p[6] = 0xB8;
    memcpy(p + 7, &fn, 8);           /* mov rax, stub_dispatch */

    p[15] = 0xFF; p[16] = 0xE0;      /* jmp rax */
    memset(p + 17, 0x90, 7);         /* nop pad */
}

/*
 * emit_impl_thunk – thunk that dispatches to a real implementation via
 * beer_dispatch_trampoline(), which switches onto a dedicated host call
 * stack before invoking it (see the big comment above g_host_call_stack).
 * Does NOT overwrite rcx/rdx/r8/r9, so all original Windows ABI arguments
 * arrive at the real impl_* function intact; only the scratch register
 * r10 is used to carry the stub index.
 *
 *   41 BA ii ii ii ii               mov r10d, <idx>          (6)
 *   48 B8 pp pp pp pp pp pp pp pp   mov rax, beer_dispatch_trampoline (10)
 *   FF E0                           jmp rax                  (2)
 *   90 * 6                          nop pad                  (6)
 */
static void emit_impl_thunk(int idx, u64 fn_ptr)
{
    u8 *p = g_tramp + idx * THUNK_SZ;
    g_impl_targets[idx] = fn_ptr;

    u32 idx32 = (u32)idx;
    u64 tramp = (u64)beer_dispatch_trampoline;

    p[0] = 0x41; p[1] = 0xBA;
    memcpy(p + 2, &idx32, 4);        /* mov r10d, idx */

    p[6] = 0x48; p[7] = 0xB8;
    memcpy(p + 8, &tramp, 8);        /* mov rax, beer_dispatch_trampoline */

    p[16] = 0xFF; p[17] = 0xE0;      /* jmp rax (back to original JMP) */
    memset(p + 18, 0x90, 6);         /* nop pad */
}

/* ── Real implementations (Windows x64 ABI) ────────────────────── */
#include <time.h>
#include <malloc.h>

/* ------------------------------------------------------------------
 * Debug canary allocator (BEER_HEAP_CANARY_DEBUG)
 *
 * Wraps every malloc/calloc/realloc/free call site from this point to
 * the end of the file with an 8-byte trailing canary plus a full-table
 * liveness scan on every single allocator call. Used to hunt a glibc
 * "malloc(): unaligned tcache chunk detected" abort that surfaces deep
 * inside an unrelated later malloc() call with no indication of which
 * buffer/writer actually caused the corruption: this wrapper reports
 * the exact allocation (tag + size + address) whose canary got
 * clobbered, as soon as ANY wrapped allocator call notices it -- much
 * closer in time to the real out-of-bounds write than glibc's own
 * internal tcache-reuse-time detection.
 *
 * Set BEER_HEAP_CANARY_DEBUG to 0 to disable this instrumentation.
 * ------------------------------------------------------------------ */
/* Heap canary instrumentation is useful only for isolated allocator debugging.
 * It wraps malloc/calloc/realloc/free and recursively calls the host allocator,
 * which is not safe once the guest bootstrap path starts using the same
 * functions internally. Disable it for real runtime runs so we don't mask the
 * guest bug with host-heap corruption diagnostics. */
#define BEER_HEAP_CANARY_DEBUG 0
#if BEER_HEAP_CANARY_DEBUG

#define DBG_CANARY_MAGIC 0xC0FFEEC0FFEEBAADULL

typedef struct DbgAllocEntry {
    void   *ptr;
    size_t  size;
    const char *tag;
    struct DbgAllocEntry *next;
} DbgAllocEntry;

#define DBG_ALLOC_BUCKETS 8192
static DbgAllocEntry *g_dbg_alloc_table[DBG_ALLOC_BUCKETS];
static pthread_mutex_t g_dbg_alloc_mutex = PTHREAD_MUTEX_INITIALIZER;

static size_t dbg_alloc_hash(const void *p)
{
    u64 v = (u64)(uintptr_t)p;
    v ^= v >> 33; v *= 0xff51afd7ed558ccdULL; v ^= v >> 33;
    return (size_t)(v % DBG_ALLOC_BUCKETS);
}

static u64 dbg_read_canary(void *p, size_t size)
{
    u64 c;
    memcpy(&c, (u8 *)p + size, sizeof(c));
    return c;
}

/* Must be called with g_dbg_alloc_mutex held. */
static void dbg_scan_all_locked(const char *ctx)
{
    for (size_t b = 0; b < DBG_ALLOC_BUCKETS; b++) {
        for (DbgAllocEntry *e = g_dbg_alloc_table[b]; e; e = e->next) {
            u64 c = dbg_read_canary(e->ptr, e->size);
            if (c != DBG_CANARY_MAGIC) {
                fprintf(stderr,
                    "[HEAPDBG] CORRUPTION DETECTED during %s: buffer tag=%s "
                    "ptr=%p size=%zu canary=0x%016lx (expected 0x%016lx)\n",
                    ctx, e->tag, e->ptr, e->size,
                    (unsigned long)c, (unsigned long)DBG_CANARY_MAGIC);
                fflush(stderr);
                abort();
            }
        }
    }
}

static void *dbg_malloc(size_t size, const char *tag)
{
    void *p = malloc(size + sizeof(u64));
    if (!p) return NULL;
    u64 magic = DBG_CANARY_MAGIC;
    memcpy((u8 *)p + size, &magic, sizeof(magic));

    DbgAllocEntry *e = malloc(sizeof(DbgAllocEntry));
    pthread_mutex_lock(&g_dbg_alloc_mutex);
    dbg_scan_all_locked(tag);
    if (e) {
        e->ptr = p; e->size = size; e->tag = tag;
        size_t b = dbg_alloc_hash(p);
        e->next = g_dbg_alloc_table[b];
        g_dbg_alloc_table[b] = e;
    }
    pthread_mutex_unlock(&g_dbg_alloc_mutex);
    return p;
}

static void *dbg_calloc(size_t n, size_t size, const char *tag)
{
    size_t total = n * size; /* only ever called with small, fixed sizes here */
    void *p = dbg_malloc(total, tag);
    if (p) memset(p, 0, total);
    return p;
}

static void *dbg_realloc(void *ptr, size_t size, const char *tag)
{
    if (!ptr) return dbg_malloc(size, tag);

    pthread_mutex_lock(&g_dbg_alloc_mutex);
    dbg_scan_all_locked(tag);
    size_t b = dbg_alloc_hash(ptr);
    DbgAllocEntry **pp = &g_dbg_alloc_table[b];
    DbgAllocEntry *found = NULL;
    while (*pp) {
        if ((*pp)->ptr == ptr) { found = *pp; *pp = found->next; break; }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_dbg_alloc_mutex);

    if (!found) {
        fprintf(stderr, "[HEAPDBG] realloc(%p) untracked pointer (tag=%s)\n", ptr, tag);
        return realloc(ptr, size);
    }

    void *newp = malloc(size + sizeof(u64));
    if (!newp) { free(found); return NULL; }
    size_t copy = found->size < size ? found->size : size;
    memcpy(newp, ptr, copy);
    u64 magic = DBG_CANARY_MAGIC;
    memcpy((u8 *)newp + size, &magic, sizeof(magic));
    free(ptr);
    found->ptr = newp; found->size = size; found->tag = tag;

    pthread_mutex_lock(&g_dbg_alloc_mutex);
    size_t nb = dbg_alloc_hash(newp);
    found->next = g_dbg_alloc_table[nb];
    g_dbg_alloc_table[nb] = found;
    pthread_mutex_unlock(&g_dbg_alloc_mutex);
    return newp;
}

static void dbg_free(void *ptr, const char *tag)
{
    if (!ptr) return;
    pthread_mutex_lock(&g_dbg_alloc_mutex);
    dbg_scan_all_locked(tag);
    size_t b = dbg_alloc_hash(ptr);
    DbgAllocEntry **pp = &g_dbg_alloc_table[b];
    DbgAllocEntry *found = NULL;
    while (*pp) {
        if ((*pp)->ptr == ptr) { found = *pp; *pp = found->next; break; }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_dbg_alloc_mutex);
    if (!found) {
        fprintf(stderr, "[HEAPDBG] free(%p) untracked pointer (tag=%s)\n", ptr, tag);
        free(ptr);
        return;
    }
    u64 c = dbg_read_canary(found->ptr, found->size);
    if (c != DBG_CANARY_MAGIC)
        fprintf(stderr, "[HEAPDBG] free(%p tag=%s size=%zu): canary already corrupted at free time!\n",
                ptr, found->tag, found->size);
    free(found->ptr);
    free(found);
}

#define malloc(sz)        dbg_malloc((sz), __func__)
#define calloc(n, sz)     dbg_calloc((n), (sz), __func__)
#define realloc(p, sz)    dbg_realloc((p), (sz), __func__)
#define free(p)           dbg_free((p), __func__)

#endif /* BEER_HEAP_CANARY_DEBUG */

/* ---- CRITICAL_SECTION (real pthread_mutex_t for thread safety) ----
 *
 * Windows CRITICAL_SECTION is 40 bytes on x64.  We overlay it with our
 * own struct that fits within those 40 bytes and hides a recursive
 * pthread mutex so EnterCriticalSection actually blocks.
 *
 * Layout we use (must stay ≤ 40 bytes):
 *   [0]  u64  magic         — sentinel so we know it was initialised by us
 *   [8]  pthread_mutex_t *  — heap-allocated recursive mutex (8 bytes ptr)
 *   [16] s32  RecursionCount
 *   [20] s32  LockCount
 *   [24] u64  OwningThread
 *   [32] u64  SpinCount
 * Total = 40 bytes exactly.
 *
 * We heap-allocate the mutex so we don't have to worry about the size
 * of pthread_mutex_t varying across platforms.
 */
#define WIN_CS_MAGIC 0xC5C5C5C5C5C5C5C5ULL

typedef struct {
    u64              Magic;
    pthread_mutex_t *Mutex;        /* heap-allocated recursive mutex */
    s32              RecursionCount;
    s32              LockCount;
    u64              OwningThread;
    u64              SpinCount;
} WIN_CS;

static pthread_mutex_t g_fallback_cs_mutex = PTHREAD_MUTEX_INITIALIZER;

static void *host_alloc(size_t size)
{
    size = (size + 15U) & ~((size_t)15U);
    size_t map_size = ((size + 4095U) & ~((size_t)4095U));
    void *p = mmap(NULL, map_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return NULL;
    return p;
}

static void host_free(void *p, size_t size)
{
    if (!p) return;
    size_t map_size = ((size + 4095U) & ~((size_t)4095U));
    munmap(p, map_size);
}

static int cs_ptr_is_valid(const WIN_CS *cs)
{
    if (!cs) return 0;
    u64 p = (u64)(uintptr_t)cs;
    /* Windows CRITICAL_SECTION objects are often hosted in the guest image's
     * .data/.bss and are perfectly valid pointers to initialize. Reject only
     * impossible addresses (NULL / sentinel / obviously host-ABI ranges) while
     * allowing the guest's own image-backed CRITICAL_SECTION objects to be
     * initialized faithfully. */
    return p != WIN_CS_MAGIC && p >= 0x10000 && p <= 0x00007fffffffffffULL;
}

static pthread_mutex_t *cs_get_or_create_mutex(WIN_CS *cs)
{
    if (!cs_ptr_is_valid(cs))
        return &g_fallback_cs_mutex;

    /* Double-checked init using __sync builtins to be safe against races
     * on InitializeCriticalSection itself being called concurrently. */
    if (__sync_val_compare_and_swap(&cs->Magic, 0, WIN_CS_MAGIC) == 0) {
        /* We won the race — allocate and install the mutex. Never route this
         * through glibc's malloc/calloc path: the guest image can hand us a
         * synthetic CRITICAL_SECTION pointer that is valid-looking but still
         * points into guest memory, and a host malloc in that case can trip the
         * black-box tcache checks we are trying to avoid. Use a private mmap so
         * the mutex lives in a known-aligned host allocation with no libc heap
         * metadata to corrupt. */
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        void *m_raw = mmap(NULL, sizeof(pthread_mutex_t), PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m_raw == MAP_FAILED) {
            pthread_mutexattr_destroy(&attr);
            return &g_fallback_cs_mutex;
        }
        pthread_mutex_t *m = (pthread_mutex_t *)m_raw;
        memset(m, 0, sizeof(*m));
        pthread_mutex_init(m, &attr);
        pthread_mutexattr_destroy(&attr);
        __sync_synchronize();
        cs->Mutex = m;
    } else {
        /* Another thread may still be writing cs->Mutex — spin briefly */
        while (!__sync_val_compare_and_swap(&cs->Mutex,
                                            (pthread_mutex_t *)NULL,
                                            (pthread_mutex_t *)NULL))
            ; /* busy-wait; in practice 0 iterations */
    }
    return cs->Mutex;
}

static u64 __attribute__((ms_abi))
impl_InitializeCriticalSectionAndSpinCount(WIN_CS *cs, u32 spin)
{
    if (!cs) return 0;
    if (!cs_ptr_is_valid(cs)) {
        fprintf(stderr,
                "[IMPL] InitializeCriticalSection* invalid ptr 0x%lx -> ignored\n",
                (u64)(uintptr_t)cs);
        return 1; /* TRUE */
    }
    /* Zero everything first, then set magic to trigger cs_get_or_create */
    memset(cs, 0, sizeof(*cs));
    cs->LockCount  = -1;
    cs->SpinCount  = spin;
    cs_get_or_create_mutex(cs);
    return 1; /* TRUE */
}

static u64 __attribute__((ms_abi))
impl_InitializeCriticalSection(WIN_CS *cs)
{
    return impl_InitializeCriticalSectionAndSpinCount(cs, 0);
}

static u64 __attribute__((ms_abi))
impl_DeleteCriticalSection(WIN_CS *cs)
{
    if (!cs_ptr_is_valid(cs)) return 0;
    if (cs->Magic == WIN_CS_MAGIC && cs->Mutex) {
        pthread_mutex_destroy(cs->Mutex);
        host_free(cs->Mutex, sizeof(pthread_mutex_t)); /* allocated via mmap, not malloc */
        cs->Mutex = NULL;
        cs->Magic = 0;
    }
    return 0;
}

static u64 __attribute__((ms_abi))
impl_EnterCriticalSection(WIN_CS *cs)
{
    if (!cs) return 0;
    int valid = cs_ptr_is_valid(cs);
    pthread_mutex_t *m = cs_get_or_create_mutex(cs);
    pthread_mutex_lock(m);
    if (valid) {
        cs->RecursionCount++;
        cs->LockCount++;
        cs->OwningThread = (u64)syscall(SYS_gettid);
    }
    return 0;
}

static u64 __attribute__((ms_abi))
impl_LeaveCriticalSection(WIN_CS *cs)
{
    if (!cs) return 0;
    int valid = cs_ptr_is_valid(cs);
    pthread_mutex_t *m = cs_get_or_create_mutex(cs);
    if (valid && cs->RecursionCount > 0) {
        cs->RecursionCount--;
        cs->LockCount--;
        if (cs->RecursionCount == 0)
            cs->OwningThread = 0;
    }
    pthread_mutex_unlock(m);
    return 0;
}

static u64 __attribute__((ms_abi))
impl_TryEnterCriticalSection(WIN_CS *cs)
{
    if (!cs) return 0;
    int valid = cs_ptr_is_valid(cs);
    pthread_mutex_t *m = cs_get_or_create_mutex(cs);
    if (pthread_mutex_trylock(m) == 0) {
        if (valid) {
            cs->RecursionCount++;
            cs->LockCount++;
            cs->OwningThread = (u64)syscall(SYS_gettid);
        }
        return 1; /* TRUE */
    }
    return 0; /* FALSE */
}

/* ---- Exception table lookup ---- */
typedef struct { u32 Begin; u32 End; u32 UnwindData; } RUNTIME_FUNC;

static u64 __attribute__((ms_abi))
impl_RtlLookupFunctionEntry(u64 ControlPc, u64 *ImageBase, void *History)
{
    (void)History;
    NtHdrs64 *nt  = (NtHdrs64 *)(g_img + ((DosHdr *)g_img)->lfanew);
    u64 base = (u64)g_img;
    u64 top  = base + nt->opt.sz_image;

    if (ControlPc < base || ControlPc >= top) return 0;

    u32 rva = (u32)(ControlPc - base);

    DataDir *pd = &nt->opt.dirs[3]; /* IMAGE_DIRECTORY_ENTRY_EXCEPTION */
    if (!pd->size) return 0;

    RUNTIME_FUNC *funcs = (RUNTIME_FUNC *)(g_img + pd->rva);
    int lo = 0, hi = (int)(pd->size / sizeof(RUNTIME_FUNC)) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if      (rva < funcs[mid].Begin) hi = mid - 1;
        else if (rva >= funcs[mid].End)  lo = mid + 1;
        else {
            if (ImageBase) *ImageBase = base;
            return (u64)&funcs[mid];
        }
    }
    return 0;
}

/* RtlVirtualUnwind – stub returning 0 is enough for first run */
static u64 __attribute__((ms_abi))
impl_RtlVirtualUnwind(u32 type, u64 base, u64 pc, u64 func,
                       u64 ctx, u64 *data, u64 *frame, u64 ctxptrs)
{
    (void)type; (void)base; (void)pc; (void)func;
    (void)ctx;  (void)data; (void)frame; (void)ctxptrs;
    return 0;
}

/* ---- Module handles ---- */
static const char *g_exe_path = NULL;

/* Fake handles for Windows system DLLs we emulate */
#define HMOD_EXE     ((u64)g_img)
#define HMOD_K32     ((u64)0xFEED0001)
#define HMOD_USER32  ((u64)0xFEED0002)
#define HMOD_NTDLL   ((u64)0xFEED0003)
#define HMOD_ADVAPI  ((u64)0xFEED0004)

static int wcscmp_ascii(const u16 *w, const char *s) {
    while (*s && *w == (u8)*s) { w++; s++; }
    char lw = (char)((*w) & 0x7f);
    if (lw >= 'A' && lw <= 'Z') lw += 32;
    char ls = *s;
    if (ls >= 'A' && ls <= 'Z') ls += 32;
    return (int)lw - (int)ls;
}

/* Simple DLL handle cache for fake/unimplemented DLLs */
static struct {
    char name[64];
    u64 handle;
} g_fake_dll_cache[32];
static int g_fake_dll_count = 0;

static u64 dll_name_to_handle(const char *lname) {
    if (!strcmp(lname,"kernel32.dll")||!strcmp(lname,"kernelbase.dll")) return HMOD_K32;
    if (!strcmp(lname,"user32.dll"))   return HMOD_USER32;
    if (!strcmp(lname,"ntdll.dll"))    return HMOD_NTDLL;
    if (!strcmp(lname,"advapi32.dll")) return HMOD_ADVAPI;
    
    /* For unknown DLLs, generate or return a cached fake handle */
    for (int i=0; i<g_fake_dll_count; i++) {
        if (!strcmp(g_fake_dll_cache[i].name, lname))
            return g_fake_dll_cache[i].handle;
    }
    
    /* Create a new fake handle: use 0x140000000 + (index * 0x100000) */
    if (g_fake_dll_count < 32) {
        u64 fake_h = 0x140000000ULL + ((u64)g_fake_dll_count << 20);
        strncpy(g_fake_dll_cache[g_fake_dll_count].name, lname, 63);
        g_fake_dll_cache[g_fake_dll_count].handle = fake_h;
        g_fake_dll_count++;
        return fake_h;
    }
    return 0;
}

static u64 __attribute__((ms_abi))
impl_GetModuleHandleA(const char *name)
{
    if (!name) return HMOD_EXE;
    char lname[64]; int i;
    for (i=0; i<63 && name[i]; i++) { char c=name[i]; if(c>='A'&&c<='Z')c+=32; lname[i]=c; }
    lname[i]=0;
    u64 h = dll_name_to_handle(lname);
    if (!h) fprintf(stderr, "[IMPL] GetModuleHandleA(\"%s\") -> NULL\n", name);
    return h;
}

static u64 __attribute__((ms_abi))
impl_GetModuleHandleW(const u16 *name)
{
    if (!name) return HMOD_EXE;
    char lname[64]; int i;
    for (i=0; i<63 && name[i]; i++) { char c=(char)(name[i]&0x7f); if(c>='A'&&c<='Z')c+=32; lname[i]=c; }
    lname[i]=0;
    u64 h = dll_name_to_handle(lname);
    if (!h) fprintf(stderr, "[IMPL] GetModuleHandleW(\"%s\") -> NULL\n", lname);
    return h;
}

static u64 __attribute__((ms_abi))
impl_GetModuleFileNameA(u64 hmod, char *buf, u32 sz)
{
    (void)hmod;
    if (!buf || !sz) return 0;
    strncpy(buf, g_exe_path ? g_exe_path : "", sz - 1);
    buf[sz-1] = 0;
    return (u64)strlen(buf);
}

/* Forward declarations needed by GetProcAddress */
typedef u64 (*ImplFn)(void);
static ImplFn find_impl(const char *name);  /* defined later */

/* GetProcAddress – look up in our stub name table, then impl table */
static u64 __attribute__((ms_abi))
impl_GetProcAddress(u64 hmod, const char *procname)
{
    (void)hmod;
    if (!procname) return 0;
    /* Search existing stubs (IAT entries) */
    for (int i = 0; i < g_nstubs; i++) {
        const char *bang = strchr(g_snames[i], '!');
        const char *fn   = bang ? bang + 1 : g_snames[i];
        if (strcmp(fn, procname) == 0)
            return (u64)(g_tramp + i * THUNK_SZ);
    }
    /* Search real implementations (not in IAT but we can implement) */
    ImplFn real = find_impl(procname);
    if (real) {
        if (g_nstubs < MAX_STUBS) {
            g_snames[g_nstubs] = strdup(procname);
            emit_impl_thunk(g_nstubs, (u64)real);
            u64 addr = (u64)(g_tramp + g_nstubs * THUNK_SZ);
            g_nstubs++;
            return addr;
        }
    }

    /* Do not return NULL for unresolved dynamic lookups.
     * Some engine paths assume a callable pointer and crash on NULL. */
    if (g_nstubs < MAX_STUBS) {
        size_t n = strlen(procname);
        char *dyn = malloc(n + 5); /* "dyn!" + name + NUL */
        if (dyn) {
            memcpy(dyn, "dyn!", 4);
            memcpy(dyn + 4, procname, n + 1);
            g_snames[g_nstubs] = dyn;
            emit_thunk(g_nstubs);
            u64 addr = (u64)(g_tramp + g_nstubs * THUNK_SZ);
            g_nstubs++;
            fprintf(stderr, "[IMPL] GetProcAddress(\"%s\") -> dynamic stub\n", procname);
            return addr;
        }
    }

    fprintf(stderr, "[IMPL] GetProcAddress(\"%s\") -> NULL (stub table full/OOM)\n", procname);
    return 0;
}

/* ---- Heap / memory ---- */
static u64 __attribute__((ms_abi))
impl_GetProcessHeap(void) { return 0x1; /* non-null fake handle */ }

static u64 __attribute__((ms_abi))
impl_HeapCreate(u32 opts, u64 initSz, u64 maxSz)
    { (void)opts; (void)initSz; (void)maxSz; return 0x2; }

/*
 * Guard-page heap allocator for HeapAlloc/HeapReAlloc/HeapFree/HeapSize.
 *
 * The engine's HeapAlloc'd buffers are written to directly by raw guest x86
 * code that we never instrument, so a guest-side out-of-bounds write can
 * silently corrupt glibc's malloc metadata (observed: "malloc(): unaligned
 * tcache chunk detected" aborting deep inside an unrelated later malloc()
 * call, with no indication of which buffer/writer was actually at fault).
 *
 * Each HeapAlloc request gets its own private mmap: the user buffer is
 * flush-aligned to end exactly at a following PROT_NONE guard page, so ANY
 * write even 1 byte past the requested size raises an immediate SIGSEGV at
 * the exact faulting guest RIP, caught by our existing on_crash handler
 * (with BEER_DEBUG_CRASH=1 diagnostics) instead of silently corrupting host
 * heap state that only surfaces much later somewhere unrelated.
 */
#define HEAP_GUARD_PAGE 4096u

typedef struct HeapGuardEntry {
    void *user_ptr;
    void *base;
    size_t total_size;
    size_t user_size;
    struct HeapGuardEntry *next;
} HeapGuardEntry;

#define HEAP_GUARD_BUCKETS 4096
static HeapGuardEntry *g_heap_guard_table[HEAP_GUARD_BUCKETS];
static pthread_mutex_t g_heap_guard_mutex = PTHREAD_MUTEX_INITIALIZER;

static size_t heap_guard_hash(void *p)
{
    u64 v = (u64)(uintptr_t)p;
    v ^= v >> 33; v *= 0xff51afd7ed558ccdULL; v ^= v >> 33;
    return (size_t)(v % HEAP_GUARD_BUCKETS);
}

static HeapGuardEntry *heap_guard_find_locked(void *user_ptr)
{
    size_t b = heap_guard_hash(user_ptr);
    for (HeapGuardEntry *e = g_heap_guard_table[b]; e; e = e->next)
        if (e->user_ptr == user_ptr) return e;
    return NULL;
}

static void heap_guard_insert(void *user_ptr, void *base, size_t total_size, size_t user_size)
{
    void *raw = mmap(NULL, sizeof(HeapGuardEntry), PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) return;
    HeapGuardEntry *e = (HeapGuardEntry *)raw;
    memset(e, 0, sizeof(*e));
    e->user_ptr = user_ptr;
    e->base = base;
    e->total_size = total_size;
    e->user_size = user_size;
    pthread_mutex_lock(&g_heap_guard_mutex);
    size_t b = heap_guard_hash(user_ptr);
    e->next = g_heap_guard_table[b];
    g_heap_guard_table[b] = e;
    pthread_mutex_unlock(&g_heap_guard_mutex);
}

static int heap_guard_remove(void *user_ptr, void **out_base, size_t *out_total, size_t *out_user_size)
{
    pthread_mutex_lock(&g_heap_guard_mutex);
    size_t b = heap_guard_hash(user_ptr);
    HeapGuardEntry **pp = &g_heap_guard_table[b];
    while (*pp) {
        if ((*pp)->user_ptr == user_ptr) {
            HeapGuardEntry *e = *pp;
            if (out_base) *out_base = e->base;
            if (out_total) *out_total = e->total_size;
            if (out_user_size) *out_user_size = e->user_size;
            *pp = e->next;
            pthread_mutex_unlock(&g_heap_guard_mutex);
            munmap(e, sizeof(HeapGuardEntry));
            return 1;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_heap_guard_mutex);
    return 0;
}

static void *heap_guard_alloc(size_t size, int zero)
{
    size_t want = size ? size : 1;
    size_t alloc_pages = (want + HEAP_GUARD_PAGE - 1) / HEAP_GUARD_PAGE;
    size_t total = (alloc_pages + 1) * HEAP_GUARD_PAGE; /* +1 trailing guard page */

    void *base = mmap(NULL, total, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return NULL;
    if (mprotect((u8 *)base + alloc_pages * HEAP_GUARD_PAGE, HEAP_GUARD_PAGE, PROT_NONE) != 0)
        fprintf(stderr, "[HEAPGUARD] mprotect guard page failed: %s\n", strerror(errno));

    uintptr_t end = (uintptr_t)base + alloc_pages * HEAP_GUARD_PAGE;
    uintptr_t start = end - want;
    /* The host allocator requires 16-byte-aligned user pointers; the previous
     * formula could put the payload at an unaligned offset relative to the
     * guard-page region and trigger libc's "malloc(): unaligned tcache chunk"
     * check when a later host allocation or free path touches that buffer. Keep
     * the payload inside the mapped region, but floor the start to a 16-byte
     * boundary so the guest-visible heap behaves like a real Windows heap.
     */
    start &= ~((uintptr_t)15);
    void *user_ptr = (void *)start;
    if (zero) memset(user_ptr, 0, want);
    heap_guard_insert(user_ptr, base, total, want);
    return user_ptr;
}

static u64 __attribute__((ms_abi))
impl_HeapAlloc(u64 heap, u32 flags, u64 size)
{
    (void)heap;
    if (size > (1ULL << 31)) {
        /* Corrupted size values show up during fault-recovery paths. Give a
         * small guarded buffer instead of returning NULL and collapsing flow. */
        fprintf(stderr, "[IMPL] HeapAlloc huge size=%lu, fallback 4K\n", (unsigned long)size);
        return (u64)heap_guard_alloc(4096, 1);
    }
    int zero_mem = (flags & 0x8) != 0; /* HEAP_ZERO_MEMORY */
    void *p = heap_guard_alloc((size_t)size, zero_mem);
    if (!p) fprintf(stderr, "[IMPL] HeapAlloc(%lu) OOM/mmap-failed\n", (unsigned long)size);
    return (u64)p;
}

static u64 __attribute__((ms_abi))
impl_HeapReAlloc(u64 heap, u32 flags, void *ptr, u64 size)
{
    (void)heap;
    if (!ptr)
        return impl_HeapAlloc(heap, flags, size);
    if (size > (1ULL << 31)) {
        fprintf(stderr, "[IMPL] HeapReAlloc huge size=%lu, keeping ptr\n", (unsigned long)size);
        return (u64)ptr;
    }

    pthread_mutex_lock(&g_heap_guard_mutex);
    HeapGuardEntry *e = heap_guard_find_locked(ptr);
    void *old_base = e ? e->base : NULL;
    size_t old_total = e ? e->total_size : 0;
    size_t old_user_size = e ? e->user_size : 0;
    pthread_mutex_unlock(&g_heap_guard_mutex);

    if (!e) {
        fprintf(stderr, "[IMPL] HeapReAlloc(%p) untracked pointer, falling back to realloc()\n", ptr);
        return (u64)realloc(ptr, (size_t)size);
    }

    int zero_mem = (flags & 0x8) != 0;
    void *newp = heap_guard_alloc((size_t)size, 0);
    if (!newp) {
        fprintf(stderr, "[IMPL] HeapReAlloc(%lu) OOM/mmap-failed\n", (unsigned long)size);
        return 0;
    }
    size_t copy = old_user_size < (size_t)size ? old_user_size : (size_t)size;
    memcpy(newp, ptr, copy);
    if (zero_mem && (size_t)size > old_user_size)
        memset((u8 *)newp + old_user_size, 0, (size_t)size - old_user_size);

    heap_guard_remove(ptr, NULL, NULL, NULL);
    munmap(old_base, old_total);
    return (u64)newp;
}

static u64 __attribute__((ms_abi))
impl_HeapFree(u64 heap, u32 flags, void *ptr)
{
    (void)heap; (void)flags;
    if (!ptr) return 1;
    void *base = NULL;
    size_t total = 0;
    if (heap_guard_remove(ptr, &base, &total, NULL))
        munmap(base, total);
    /* Untracked pointers: leave as a no-op, matching prior behavior, since
     * we don't know their true origin/size. */
    return 1;
}

static u64 __attribute__((ms_abi))
impl_HeapSize(u64 heap, u32 flags, void *ptr)
{
    (void)heap; (void)flags;
    pthread_mutex_lock(&g_heap_guard_mutex);
    HeapGuardEntry *e = heap_guard_find_locked(ptr);
    size_t sz = e ? e->user_size : 0;
    pthread_mutex_unlock(&g_heap_guard_mutex);
    return (u64)sz;
}

/* VirtualAlloc */
static u64 __attribute__((ms_abi))
impl_VirtualAlloc(void *addr, u64 size, u32 type, u32 protect)
{
    (void)type;
    int prot = 0;
    /* PAGE_EXECUTE_* */
    if (protect & 0xF0) prot |= PROT_EXEC;
    /* PAGE_READWRITE / PAGE_WRITECOPY */
    if (protect & 0x0C) prot |= PROT_READ | PROT_WRITE;
    /* PAGE_READONLY */
    if (protect & 0x02) prot |= PROT_READ;
    if (!prot) prot = PROT_READ | PROT_WRITE;

    void *p = mmap(addr, (size_t)size, prot,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (p == MAP_FAILED) ? 0 : (u64)p;
}

static u64 __attribute__((ms_abi))
impl_VirtualFree(void *addr, u64 size, u32 type)
{
    (void)type;
    if (size == 0) size = 4096;
    munmap(addr, (size_t)size);
    return 1;
}

static u64 __attribute__((ms_abi))
impl_VirtualQuery(void *addr, u64 *buf, u64 sz)
    { (void)addr; (void)buf; (void)sz; return 0; }

static u64 __attribute__((ms_abi))
impl_VirtualProtect(void *addr, u64 sz, u32 new_prot, u32 *old_prot)
{
    if (old_prot) *old_prot = 0;
    int prot = 0;
    if (new_prot & 0xF0) prot |= PROT_EXEC;
    if (new_prot & 0x0C) prot |= PROT_READ | PROT_WRITE;
    if (new_prot & 0x02) prot |= PROT_READ;
    if (!prot) prot = PROT_READ | PROT_WRITE;
    return mprotect(addr, (size_t)sz, prot) == 0 ? 1 : 0;
}

/* ---- Process / thread info ---- */
static u64 __attribute__((ms_abi))
impl_GetCurrentProcessId(void) { return (u64)getpid(); }

static u64 __attribute__((ms_abi))
impl_GetCurrentThreadId(void)  { return (u64)syscall(SYS_gettid); }

/* Windows FILETIME = 100-nanosecond intervals since 1601-01-01 */
#define EPOCH_DIFF 116444736000000000ULL
static u64 __attribute__((ms_abi))
impl_GetSystemTimeAsFileTime(u64 *ft)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    u64 val = (u64)ts.tv_sec * 10000000ULL + ts.tv_nsec / 100 + EPOCH_DIFF;
    if (ft) *ft = val;
    return 0;
}

static u64 __attribute__((ms_abi))
impl_QueryPerformanceCounter(u64 *out)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    if (out) *out = (u64)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    return 1;
}

static u64 __attribute__((ms_abi))
impl_QueryPerformanceFrequency(u64 *out)
    { if (out) *out = 1000000000ULL; return 1; }

/* ---- Error ---- */
static __thread u32 g_last_error = 0;
static u64 __attribute__((ms_abi)) impl_GetLastError(void)  { return g_last_error; }
static u64 __attribute__((ms_abi)) impl_SetLastError(u32 e) { g_last_error = e; return 0; }

/* ---- Misc ---- */
static u64 __attribute__((ms_abi)) impl_IsDebuggerPresent(void)    { return 0; }
static u64 __attribute__((ms_abi)) impl_IsProcessorFeaturePresent(u32 f) {
    /* PF_FLOATING_POINT_EMULATED=0 → false (we have real FP) */
    (void)f; return 0;
}
/* Diagnostic helper: scan the current (guest) stack for return-address-shaped
 * values that fall inside the mapped guest image, to reconstruct an
 * approximate call chain when a stub is invoked directly from guest code. */
static void dbg_dump_guest_callchain(const char *tag) {
    sync_saved_guest_rsp_from_teb();
    extern __thread u64 g_saved_guest_rsp;
    u64 rsp = g_saved_guest_rsp;
    fprintf(stderr, "[CHAIN:%s] rsp=0x%lx\n", tag, rsp);
    u64 *sp = (u64 *)rsp;
    int printed = 0;
    for (int i = 0; i < 200 && printed < 24; i++) {
        u64 v = sp[i];
        if (v >= (u64)g_img && v < (u64)g_img + 0x42d2000) {
            fprintf(stderr, "[CHAIN:%s]   [%3d] img+0x%lx\n", tag, i, v - (u64)g_img);
            printed++;
        }
    }
}
static u64 __attribute__((ms_abi)) impl_SetUnhandledExceptionFilter(u64 f) { (void)f; return 0; }
static u64 __attribute__((ms_abi)) impl_UnhandledExceptionFilter(u64 ep) {
    u32 code = 0;
    fprintf(stderr, "[WARN] UnhandledExceptionFilter");
    if (ep) {
        /* EXCEPTION_POINTERS = { EXCEPTION_RECORD*, CONTEXT* } */
        u64 *ptrs = (u64*)ep;
        if (ptrs[0]) {
            code = *(u32*)ptrs[0];
            fprintf(stderr, ": code=0x%08x", code);
            /* 0xE06D7363 = C++ exception, 0xC0000005 = access violation */
        }
    }
    fputs("\n", stderr);
    fflush(stderr);

    /* STATUS_FATAL_APP_EXIT (0x40000015) is used by the guest CRT as a
     * process-termination sentinel during very-early bootstrap. In real Windows,
     * this is not a recoverable control-flow continuation: the filter should
     * return EXCEPTION_CONTINUE_SEARCH (0) so the system can proceed to the
     * normal process-exit path. Our hosted runner intentionally ignores
     * `ExitProcess`/`CorExitProcess`, but returning a handled value here causes the
     * bootstrap code to keep re-entering the same fatal-exit path in a loop.
     *
     * Keep the shim permissive for ordinary faults, but do not claim ownership of
     * a fatal app-exit sentinel. */
    if (code == 0x40000015u) {
        fprintf(stderr,
                "[WARN] Fatal app-exit sentinel seen; continuing search instead of handling it in-place\n");
        dbg_dump_guest_callchain("UEF-fatalexit");
        fflush(stderr);
        return 0; /* EXCEPTION_CONTINUE_SEARCH */
    }

    /* For ordinary non-fatal guest faults, keep the shim permissive and allow the
     * guest to resume instead of terminating the loader outright. */
    return 0xFFFFFFFFu;
}

/* ---- RtlCaptureContext — naked asm to capture real register state ---- */
/*
 * File-scope asm: no C prologue. At entry (via JMP from impl thunk):
 *   rcx = CONTEXT*   (first Windows arg, intact)
 *   rax = our fn ptr (overwritten by impl thunk — can't help it)
 *   [rsp] = return address = caller's RIP
 *   rsp   = caller's RSP - 8 (call pushed the return addr)
 * We store all GP regs + RIP + RSP into the Windows CONTEXT struct.
 */
asm(
".global impl_RtlCaptureContext\n"
".type   impl_RtlCaptureContext, @function\n"
"impl_RtlCaptureContext:\n"
".intel_syntax noprefix\n"
"mov qword ptr [rcx + 0x078], rax\n"  /* Rax  (= fn ptr, wrong but harmless) */
"mov rax, rcx\n"                       /* save ctx* in rax */
"mov qword ptr [rax + 0x080], rcx\n"  /* Rcx  (= ctx ptr itself, not original) */
"mov qword ptr [rax + 0x088], rdx\n"  /* Rdx */
"mov qword ptr [rax + 0x090], rbx\n"  /* Rbx */
"lea rcx, [rsp + 8]\n"
"mov qword ptr [rax + 0x098], rcx\n"  /* Rsp = caller rsp */
"mov qword ptr [rax + 0x0a0], rbp\n"  /* Rbp */
"mov qword ptr [rax + 0x0a8], rsi\n"  /* Rsi */
"mov qword ptr [rax + 0x0b0], rdi\n"  /* Rdi */
"mov qword ptr [rax + 0x0b8], r8\n"   /* R8 */
"mov qword ptr [rax + 0x0c0], r9\n"   /* R9 */
"mov qword ptr [rax + 0x0c8], r10\n"  /* R10 */
"mov qword ptr [rax + 0x0d0], r11\n"  /* R11 */
"mov qword ptr [rax + 0x0d8], r12\n"  /* R12 */
"mov qword ptr [rax + 0x0e0], r13\n"  /* R13 */
"mov qword ptr [rax + 0x0e8], r14\n"  /* R14 */
"mov qword ptr [rax + 0x0f0], r15\n"  /* R15 */
"mov rcx, [rsp]\n"
"mov qword ptr [rax + 0x0f8], rcx\n"  /* Rip = return address (caller's RIP) */
"mov dword ptr [rax + 0x030], 0x10007f\n" /* ContextFlags = CONTEXT_ALL (offset 0x30 in AMD64 CONTEXT) */
"xor eax, eax\n"
"ret\n"
".att_syntax prefix\n"
);
extern void impl_RtlCaptureContext(void); /* declaration for the table */

/* ── TLS — real per-thread storage via pthread keys ─────────────────
 * Windows TLS must be per-thread. We back each slot with a pthread key.
 */
#define WIN_TLS_MAX       1088
#define TLS_OUT_OF_INDEXES 0xFFFFFFFF

static pthread_key_t g_tls_pkeys[WIN_TLS_MAX];
static u8  g_tls_pkey_ok[WIN_TLS_MAX];
static u8  g_tls_used[WIN_TLS_MAX];
static pthread_mutex_t g_tls_mtx = PTHREAD_MUTEX_INITIALIZER;

static u64 __attribute__((ms_abi)) impl_TlsAlloc(void) {
    pthread_mutex_lock(&g_tls_mtx);
    for (int i = 0; i < WIN_TLS_MAX; i++) {
        if (!g_tls_used[i]) {
            g_tls_used[i] = 1;
            if (!g_tls_pkey_ok[i]) {
                pthread_key_create(&g_tls_pkeys[i], NULL);
                g_tls_pkey_ok[i] = 1;
            }
            pthread_mutex_unlock(&g_tls_mtx);
            return (u64)i;
        }
    }
    pthread_mutex_unlock(&g_tls_mtx);
    return TLS_OUT_OF_INDEXES;
}
static u64 __attribute__((ms_abi)) impl_TlsSetValue(u32 idx, u64 val) {
    if (idx >= WIN_TLS_MAX || !g_tls_used[idx]) return 0;
    pthread_setspecific(g_tls_pkeys[idx], (void *)val);
    return 1;
}
static u64 __attribute__((ms_abi)) impl_TlsGetValue(u32 idx) {
    if (idx >= WIN_TLS_MAX || !g_tls_used[idx]) return 0;
    return (u64)pthread_getspecific(g_tls_pkeys[idx]);
}
static u64 __attribute__((ms_abi)) impl_TlsFree(u32 idx) {
    if (idx >= WIN_TLS_MAX) return 0;
    pthread_mutex_lock(&g_tls_mtx);
    g_tls_used[idx] = 0;
    pthread_mutex_unlock(&g_tls_mtx);
    return 1;
}

/* ---- String / locale ---- */
static u64 __attribute__((ms_abi))
impl_MultiByteToWideChar(u32 cp, u32 flags, const char *src, int srclen,
                          u16 *dst, int dstlen)
{
    (void)cp; (void)flags;
    if (srclen < 0) srclen = (int)strlen(src) + 1;
    if (!dst || !dstlen) return srclen;
    int n = srclen < dstlen ? srclen : dstlen;
    for (int i = 0; i < n; i++) dst[i] = (u8)src[i];
    return n;
}

static u64 __attribute__((ms_abi))
impl_WideCharToMultiByte(u32 cp, u32 flags, const u16 *src, int srclen,
                          char *dst, int dstlen, const char *def, int *used)
{
    (void)cp; (void)flags; (void)def; (void)used;
    if (srclen < 0) { int i=0; while(src[i]) i++; srclen=i+1; }
    if (!dst || !dstlen) return srclen;
    int n = srclen < dstlen ? srclen : dstlen;
    for (int i = 0; i < n; i++) dst[i] = (char)(src[i] & 0x7F);
    return n;
}

/* ---- GetVersion / GetVersionEx ---- */
static u64 __attribute__((ms_abi)) impl_GetVersion(void) {
    /* Return Windows 10.0 (major=10, minor=0, build=19041) */
    return (u64)0x0000000A | ((u64)0x00000000 << 8) | ((u64)19041 << 16);
}

/* ---- LoadLibrary / FreeLibrary ---- */
static u64 __attribute__((ms_abi))
impl_LoadLibraryA(const char *name)
{
    if (!name) { g_last_error = 126; return 0; }
    char lname[64]; int i;
    for (i=0; i<63 && name[i]; i++) {
        char c=name[i]; if(c>='A'&&c<='Z')c+=32; lname[i]=c;
    }
    lname[i]=0;
    u64 h = dll_name_to_handle(lname);
    if (h) {
        if (lname[0] != 'k' && lname[0] != 'u' && lname[0] != 'n' && lname[0] != 'a') {
            fprintf(stderr, "[IMPL] LoadLibraryA(\"%s\") -> 0x%lx (fake)\n", name, h);
        }
        return h;
    }
    g_last_error = 126; return 0;
}
static u64 __attribute__((ms_abi))
impl_LoadLibraryW(const u16 *name)
{
    if (!name) { g_last_error = 126; return 0; }
    char lname[64]; int i;
    for (i=0; i<63 && name[i]; i++) {
        char c=(char)(name[i]&0x7f); if(c>='A'&&c<='Z')c+=32; lname[i]=c;
    }
    lname[i]=0;
    u64 h = dll_name_to_handle(lname);
    if (h) {
        if (lname[0] != 'k' && lname[0] != 'u' && lname[0] != 'n' && lname[0] != 'a') {
            fprintf(stderr, "[IMPL] LoadLibraryW(%S) -> 0x%lx (fake)\n", name, h);
        }
        return h;
    }
    g_last_error = 126; return 0;
}
static u64 __attribute__((ms_abi))
impl_LoadLibraryExA(const char *name, u64 h, u32 flags)
    { (void)h; (void)flags; return impl_LoadLibraryA(name); }
static u64 __attribute__((ms_abi))
impl_LoadLibraryExW(const u16 *name, u64 h, u32 flags)
    { (void)h; (void)flags; return impl_LoadLibraryW(name); }
static u64 __attribute__((ms_abi)) impl_FreeLibrary(u64 h) { (void)h; return 1; }

/* ---- Process affinity / CPU info ---- */
static u64 __attribute__((ms_abi))
impl_GetProcessAffinityMask(u64 h, u64 *proc_mask, u64 *sys_mask) {
    (void)h;
    long ncpus = sysconf(_SC_NPROCESSORS_ONLN);
    u64 mask = (ncpus >= 64) ? ~(u64)0 : ((u64)1 << ncpus) - 1;
    if (proc_mask) *proc_mask = mask;
    if (sys_mask)  *sys_mask  = mask;
    return 1;
}
static u64 __attribute__((ms_abi)) impl_SetProcessAffinityMask(u64 h, u64 m)
    { (void)h;(void)m; return 1; }

/* ---- WINMM timing ---- */
static u64 __attribute__((ms_abi)) impl_timeGetTime(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
static u64 __attribute__((ms_abi)) impl_timeBeginPeriod(u32 p) { (void)p; return 0; }
static u64 __attribute__((ms_abi)) impl_timeEndPeriod(u32 p)   { (void)p; return 0; }

/* ---- Security descriptors ---- */
static u64 __attribute__((ms_abi))
impl_InitializeSecurityDescriptor(u64 sd, u32 rev) { (void)rev; if(sd) memset((void*)sd,0,20); return 1; }
static u64 __attribute__((ms_abi))
impl_SetSecurityDescriptorDacl(u64 sd, u32 p, u64 acl, u32 def)
    { (void)sd;(void)p;(void)acl;(void)def; return 1; }

/* ── Steam API stubs ─────────────────────────────────────────────── */
static u64 __attribute__((ms_abi)) impl_SteamAPI_Init(void) {
    fprintf(stderr, "[STEAM] SteamAPI_Init() -> TRUE (faked)\n");
    return 1;
}
static u64 __attribute__((ms_abi))
impl_SteamAPI_RestartAppIfNecessary(u32 appid) { (void)appid; return 0; }
static u64 __attribute__((ms_abi)) impl_SteamAPI_Shutdown(void)     { return 0; }
static u64 __attribute__((ms_abi)) impl_SteamAPI_RunCallbacks(void) { return 0; }
static u64 __attribute__((ms_abi)) impl_SteamAPI_IsSteamRunning(void) { return 0; }
static u64 __attribute__((ms_abi))
impl_SteamInternal_CreateInterface(const char *ver)
    { fprintf(stderr,"[STEAM] CreateInterface(%s)\n",ver?ver:"?"); return 0; }
static u64 __attribute__((ms_abi))
impl_SteamAPI_RegisterCallback(u64 cb, u32 id)   { (void)cb;(void)id; return 0; }
static u64 __attribute__((ms_abi))
impl_SteamAPI_UnregisterCallback(u64 cb)          { (void)cb; return 0; }

/*
 * Fake Steam interface objects.
 * Steam interfaces are C++ objects: the first field is a pointer to a vtable
 * (array of function pointers). We create one generic vtable where:
 *   - Most methods return 1 (true / "success")
 *   - Methods 4 and 5 of ISteamApps return "english"
 * All Steam interface accessors (SteamApps, SteamUtils, …) return
 * the same fake object, since we only care about not crashing.
 */
#define STEAM_VTAB_SZ 256

static u64 __attribute__((ms_abi))
steam_vfn_true(u64 a, u64 b, u64 c, u64 d) { (void)a;(void)b;(void)c;(void)d; return 1; }
static u64 __attribute__((ms_abi))
steam_vfn_english(u64 a, u64 b, u64 c, u64 d)
    { (void)a;(void)b;(void)c;(void)d; return (u64)"english"; }

static u64  g_steam_vtab[STEAM_VTAB_SZ];
static u64 *g_steam_obj  = NULL;   /* fake interface: &vtable_ptr */

static void init_steam_fake(void) {
    for (int i = 0; i < STEAM_VTAB_SZ; i++)
        g_steam_vtab[i] = (u64)steam_vfn_true;
    /* ISteamApps vtable[4]=GetCurrentGameLanguage, [5]=GetAvailableGameLanguages */
    g_steam_vtab[4] = (u64)steam_vfn_english;
    g_steam_vtab[5] = (u64)steam_vfn_english;

    g_steam_obj = mmap(NULL, 4096, PROT_READ|PROT_WRITE,
                       MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (g_steam_obj != MAP_FAILED)
        g_steam_obj[0] = (u64)g_steam_vtab;
    else
        g_steam_obj = NULL;
}

/* All Steam interface accessors return the same fake object */
static u64 __attribute__((ms_abi)) impl_SteamApps(void)     { return (u64)g_steam_obj; }
static u64 __attribute__((ms_abi)) impl_SteamClient(void)   { return (u64)g_steam_obj; }
static u64 __attribute__((ms_abi)) impl_SteamUtils(void)    { return (u64)g_steam_obj; }
static u64 __attribute__((ms_abi)) impl_SteamUser(void)     { return (u64)g_steam_obj; }
static u64 __attribute__((ms_abi)) impl_SteamFriends(void)  { return (u64)g_steam_obj; }
static u64 __attribute__((ms_abi)) impl_SteamUserStats(void){ return (u64)g_steam_obj; }
static u64 __attribute__((ms_abi)) impl_SteamUGC(void)      { return (u64)g_steam_obj; }
static u64 __attribute__((ms_abi)) impl_SteamRemoteStorage(void) { return (u64)g_steam_obj; }

/* ── Engine internal allocator (vtable[10] on fake COM objects) ─────
 * The game's engine calls through this vtable slot expecting a newly
 * allocated object pointer. We satisfy the call and pre-set offset 0x10
 * to 1 so the "transaction" assertion (cmp [ptr+0x10], 0) passes.
 */
/* Forward-declare g_d3d11_vtab so engine_alloc_vtab10 can reference it */
static u64 g_d3d11_vtab[256];

static u64 __attribute__((ms_abi))
engine_alloc_vtab10(u64 *thisobj, u64 size, u64 alignment)
{
    (void)thisobj; (void)alignment;
    if (size < 128) size = 128;
    u8 *p = calloc(1, (size_t)size);
    if (!p) return 0;
    *(u64*)p = (u64)g_d3d11_vtab;   /* vtable ptr */
    *(u32*)(p + 0x10) = 1;           /* refcount/status: non-zero passes assertion */
    return (u64)p;
}

/* ── Fake COM/DXGI interface stubs ───────────────────────────────────
 * Pattern: object = { vtable_ptr, ... }
 * vtable = { fn0, fn1, ... }
 * Most methods return E_FAIL (0x80004005) or appropriate HRESULT errors.
 */
#define S_OK         0x00000000L
#define E_NOINTERFACE 0x80004002L
#define E_FAIL       0x80004005L
#define DXGI_ERROR_NOT_FOUND 0x887A0002L

static u64 __attribute__((ms_abi))
dxgi_AddRef(u64 *obj) { (void)obj; return 1; }
static u64 __attribute__((ms_abi))
dxgi_Release(u64 *obj) { (void)obj; return 0; }
/* QueryInterface: return the same object for any IID (fake COM) */
static u64 __attribute__((ms_abi))
dxgi_QueryInterface(u64 *obj, u64 riid, void **pp) {
    (void)riid;
    if (pp) *pp = obj; /* return 'this' for any IID */
    return S_OK;
}
static u64 __attribute__((ms_abi))
dxgi_EnumAdapters(u64 *obj, u32 idx, u64 *pp)
    { (void)obj;(void)idx; if(pp)*pp=0; return (u64)DXGI_ERROR_NOT_FOUND; }
static u64 __attribute__((ms_abi))
dxgi_generic_fail(u64 a, u64 b, u64 c, u64 d)
    { (void)a;(void)b;(void)c;(void)d; return (u64)E_FAIL; }

#define DXGI_VTAB_SZ 128
static u64  g_dxgi_vtab[DXGI_VTAB_SZ];
static u64 *g_dxgi_factory = NULL;

/* Forward declarations for DXGI stub functions */
static u64 __attribute__((ms_abi)) dxgi_ok(u64, u64, u64, u64);
static u64 __attribute__((ms_abi)) dxgi_log_ok(u64, u64, u64, u64);
static u64 __attribute__((ms_abi)) dxgi_AddRef(u64 *);
/* Forward declaration for D3D11 vtable (defined in init_d3d11_fake) */
/* g_d3d11_vtab is forward-declared above */
static u64 __attribute__((ms_abi)) dxgi_Release(u64 *);
static u64 __attribute__((ms_abi)) dxgi_QueryInterface(u64 *, u64, void **);
static u64 __attribute__((ms_abi)) dxgi_EnumAdapters(u64 *, u32, u64 *);
static u64 __attribute__((ms_abi)) dxgi_EnumAdapters_with_fake(u64 *, u32, u64 **);
static u64 __attribute__((ms_abi)) dxgi_CreateSwapChain(u64 *, u64 *, u64 *, u64 **);
static u64 __attribute__((ms_abi)) dxgi_Present(u64 *, u32, u32);
static u64 __attribute__((ms_abi)) dxgi_GetBuffer(u64 *, u32, u64, void **);
static u64 __attribute__((ms_abi)) dxgi_ResizeBuffers(u64 *, u32, u32, u32, u32, u32);

static void init_dxgi_fake(void) {
    /* Slots 0-2: IUnknown */
    g_dxgi_vtab[0] = (u64)dxgi_QueryInterface;
    g_dxgi_vtab[1] = (u64)dxgi_AddRef;
    g_dxgi_vtab[2] = (u64)dxgi_Release;
    /* Slots 3+ : fill with logging ok */
    for (int i = 3; i < DXGI_VTAB_SZ; i++) g_dxgi_vtab[i] = (u64)dxgi_log_ok;
    /* IDXGIObject (3-6 privdata methods) */
    /* IDXGIFactory */
    g_dxgi_vtab[7] = (u64)dxgi_EnumAdapters;    /* EnumAdapters */
    g_dxgi_vtab[12]= (u64)dxgi_EnumAdapters;    /* EnumAdapters1 */

    g_dxgi_factory = mmap(NULL, 256, PROT_READ|PROT_WRITE,
                          MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (g_dxgi_factory != MAP_FAILED)
        g_dxgi_factory[0] = (u64)g_dxgi_vtab;
    else
        g_dxgi_factory = NULL;
}

/* CreateDXGIFactory(REFIID riid, void **ppFactory) → S_OK */
static u64 __attribute__((ms_abi))
impl_CreateDXGIFactory(u64 riid, u64 **ppFactory)
{
    sync_saved_guest_rsp_from_teb();
    (void)riid;
    if (!g_api_createfactory) { g_api_createfactory=1; fprintf(stderr, "[PROGRESS] CreateDXGIFactory called - graphics factory created\n"); }
    if (ppFactory) *ppFactory = g_dxgi_factory;
    return S_OK;
}
static u64 __attribute__((ms_abi))
impl_CreateDXGIFactory1(u64 riid, u64 **ppFactory)
    { return impl_CreateDXGIFactory(riid, ppFactory); }
static u64 __attribute__((ms_abi))
impl_CreateDXGIFactory2(u32 flags, u64 riid, u64 **ppFactory)
    { (void)flags; return impl_CreateDXGIFactory(riid, ppFactory); }

/* ── WS2_32 (Winsock 2) ──────────────────────────────────────────── */
static u64 __attribute__((ms_abi))
impl_WSAStartup(u32 ver, u8 *wsadata) {
    (void)ver; if (wsadata) memset(wsadata, 0, 408); return 0;
}
static u64 __attribute__((ms_abi)) impl_WSACleanup(void)    { return 0; }
static u64 __attribute__((ms_abi)) impl_WSAGetLastError(void) { return 10057; }
static u64 __attribute__((ms_abi))
impl_WSASetLastError(u32 e) { (void)e; return 0; }

/* ---- NtQuerySystemInformation ---- */
static u64 __attribute__((ms_abi))
impl_NtQuerySystemInformation(u32 cls, void *buf, u32 sz, u32 *retsz) {
    (void)cls; if(buf) memset(buf,0,sz); if(retsz) *retsz=sz; return 0;
}

/* ---- Time ---- */
static u64 __attribute__((ms_abi))
impl_GetLocalTime(u64 *systime) {
    /* SYSTEMTIME: {year,month,dayofweek,day,hour,min,sec,ms} each u16 */
    if (!systime) return 0;
    time_t t = time(NULL); struct tm *l = localtime(&t);
    u16 *st = (u16*)systime;
    st[0]=(u16)(l->tm_year+1900); st[1]=(u16)(l->tm_mon+1);
    st[2]=(u16)l->tm_wday; st[3]=(u16)l->tm_mday;
    st[4]=(u16)l->tm_hour; st[5]=(u16)l->tm_min;
    st[6]=(u16)l->tm_sec;  st[7]=0;
    return 0;
}
static u64 __attribute__((ms_abi))
impl_GetSystemTime(u64 *st) { return impl_GetLocalTime(st); }
static u64 __attribute__((ms_abi))
impl_SystemTimeToFileTime(const u16 *systime, u64 *filetime) {
    if (!systime || !filetime) return 0;
    /* Approximate: use current time */
    impl_GetSystemTimeAsFileTime(filetime);
    return 1;
}

/* ---- Drive / file system ---- */
static u64 __attribute__((ms_abi))
impl_GetDriveTypeW(const u16 *path) { (void)path; return 3; /* DRIVE_FIXED */ }
static u64 __attribute__((ms_abi))
impl_GetDriveTypeA(const char *path) { (void)path; return 3; }

/* ---- User info ---- */
static u64 __attribute__((ms_abi))
impl_GetUserNameW(u16 *buf, u32 *sz) {
    static u16 name[] = {'P','l','a','y','e','r',0};
    u32 need = 7;
    if (!buf || !sz || *sz < need) { if(sz)*sz=need; return 0; }
    int i; for(i=0;i<6;i++) buf[i]=name[i]; buf[i]=0;
    *sz = need; return 1;
}
static u64 __attribute__((ms_abi))
impl_GetUserNameA(char *buf, u32 *sz) {
    if (!buf || !sz || *sz < 7) { if(sz)*sz=7; return 0; }
    strcpy(buf,"Player"); *sz=7; return 1;
}

/* ---- SHGetFolderPathW ---- */
static u64 __attribute__((ms_abi))
impl_SHGetFolderPathW(u64 hwnd, s32 csidl, u64 tok, u32 flags, u16 *buf)
{
    (void)hwnd;(void)tok;(void)flags;
    if (!buf) return (u64)-1;
    /* Return /tmp as the "folder" for all CSIDL_* */
    const char *path = "/tmp";
    int i; for(i=0;path[i]&&i<255;i++) buf[i]=(u8)path[i]; buf[i]=0;
    return 0; /* S_OK */
}
static u64 __attribute__((ms_abi))
impl_SHGetFolderPathA(u64 hwnd, s32 csidl, u64 tok, u32 flags, char *buf)
{
    (void)hwnd;(void)tok;(void)flags;(void)csidl;
    if (buf) strcpy(buf, "/tmp");
    return 0;
}
static u64 __attribute__((ms_abi))
impl_SHGetKnownFolderPath(u64 rfid, u32 flags, u64 tok, u64 *out) {
    (void)rfid;(void)flags;(void)tok;
    /* Return a CoTaskMem-allocated wstring "/tmp" */
    u16 *p = malloc(8 * sizeof(u16));
    if (!p) return (u64)-1;
    const char *s = "/tmp"; int i;
    for(i=0;s[i];i++) p[i]=(u8)s[i]; p[i]=0;
    if (out) *out = (u64)p;
    return 0;
}
static u64 __attribute__((ms_abi))
impl_CoTaskMemFree(void *p) { (void)p; return 0; }

/* ---- COM CoInitialize ---- */
static u64 __attribute__((ms_abi))
impl_CoInitialize(u64 reserved) { (void)reserved; return 0; /* S_OK */ }
static u64 __attribute__((ms_abi))
impl_CoInitializeEx(u64 reserved, u32 model) { (void)reserved;(void)model; return 0; }
static u64 __attribute__((ms_abi))
impl_CoUninitialize(void) { return 0; }
static u64 __attribute__((ms_abi))
impl_CoCreateInstance(u64 rclsid, u64 outer, u32 ctx, u64 riid, void **ppv) {
    (void)rclsid;(void)outer;(void)ctx;(void)riid;
    if (ppv) *ppv = NULL;
    return (u64)E_FAIL;
}

/* ── Fake D3D11 / DXGI adapter + device ──────────────────────────────
 * We provide a software-like fake that just returns S_OK for device
 * creation but E_FAIL for actual resource/rendering operations.
 * The game will be able to initialize but won't render.
 */
#define D3D11_SDK_VERSION 7
#define D3D_FEATURE_LEVEL_11_0 0xb000

#define D3D11_VTAB_SZ 256
/* g_d3d11_vtab is forward-declared above */
static u64 *g_d3d11_device   = NULL;
static u64 *g_d3d11_context  = NULL;
static u64 *g_dxgi_swapchain = NULL;

static u64 __attribute__((ms_abi))
d3d11_fail(u64 a, u64 b, u64 c, u64 d)
    { (void)a;(void)b;(void)c;(void)d; return (u64)E_FAIL; }
/* DXGI/D3D11 generic success stub - most COM methods return S_OK */
static u64 __attribute__((ms_abi))
dxgi_ok(u64 a, u64 b, u64 c, u64 d)
    { (void)a;(void)b;(void)c;(void)d; return S_OK; }

/* Logging wrapper to understand which vtable slot fires */
static u64 __attribute__((ms_abi))
dxgi_log_ok(u64 a, u64 b, u64 c, u64 d) {
    /* Try to identify which vtable method this is by looking at the caller return address */
    u64 caller_rip = 0;
    if (g_saved_guest_rsp) {
        caller_rip = *(u64*)(g_saved_guest_rsp); /* First stack slot has return address */
    }
    fprintf(stderr, "[DXGI_vtab_CALLED] ok(0x%lx, 0x%lx, 0x%lx) g_saved_guest_rsp=0x%lx caller_rip=0x%lx\n", 
            a, b, c, g_saved_guest_rsp, caller_rip);
    fflush(stderr);
    (void)d; return S_OK;
}

static void init_d3d11_fake(void) {
    /* All methods default to E_FAIL */
    for (int i = 0; i < D3D11_VTAB_SZ; i++) g_d3d11_vtab[i] = (u64)d3d11_fail;
    /* COM IUnknown (vtable[0..2]) */
    g_d3d11_vtab[0] = (u64)dxgi_QueryInterface;
    g_d3d11_vtab[1] = (u64)dxgi_AddRef;
    g_d3d11_vtab[2] = (u64)dxgi_Release;

    /* Allocate three separate 256-byte objects sharing the same vtable */
    g_d3d11_device  = mmap(NULL,256,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    g_d3d11_context = mmap(NULL,256,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    g_dxgi_swapchain= mmap(NULL,256,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (g_d3d11_device  != MAP_FAILED) g_d3d11_device[0]  = (u64)g_d3d11_vtab;
    if (g_d3d11_context != MAP_FAILED) g_d3d11_context[0] = (u64)g_d3d11_vtab;
    if (g_dxgi_swapchain!= MAP_FAILED) g_dxgi_swapchain[0]= (u64)g_d3d11_vtab;
}

/*
 * D3D11CreateDevice(pAdapter, DriverType, Software, Flags,
 *                  pFeatureLevels, FeatureLevels, SDKVersion,
 *                  ppDevice, pFeatureLevel, ppImmediateContext)
 * Windows x64: first 4 in rcx,rdx,r8,r9; rest on stack (with shadow).
 * Parameters past r9 are on stack at [rsp+32],[rsp+40],... at call site.
 * In our ms_abi function the stack params come through normally.
 */
static u64 __attribute__((ms_abi))
impl_D3D11CreateDevice(u64 adapter, u32 driver_type, u64 software,
                        u32 flags, const u32 *feature_levels, u32 n_levels,
                        u32 sdk_ver, u64 **ppDevice, u32 *pFeatureLevel,
                        u64 **ppContext)
{
    (void)adapter;(void)driver_type;(void)software;(void)flags;
    (void)feature_levels;(void)n_levels;(void)sdk_ver;
    if (!g_api_createdevice) { g_api_createdevice=1; fprintf(stderr, "[PROGRESS] D3D11CreateDevice called - graphics device created\n"); }
    fprintf(stderr, "[D3D11] CreateDevice -> fake device\n");
    if (ppDevice)      *ppDevice  = g_d3d11_device;
    if (pFeatureLevel) *pFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    if (ppContext)     *ppContext = g_d3d11_context;
    return S_OK;
}

static u64 __attribute__((ms_abi))
impl_D3D11CreateDeviceAndSwapChain(u64 adapter, u32 dtype, u64 sw, u32 flags,
    const u32 *fls, u32 nfl, u32 sdk, u64 *swdesc, u64 **ppSwap,
    u64 **ppDevice, u32 *pFL, u64 **ppCtx)
{
    (void)adapter;(void)dtype;(void)sw;(void)flags;
    (void)fls;(void)nfl;(void)sdk;(void)swdesc;
    fprintf(stderr, "[D3D11] CreateDeviceAndSwapChain -> fake\n");
    if (ppDevice) *ppDevice = g_d3d11_device;
    if (pFL)      *pFL      = D3D_FEATURE_LEVEL_11_0;
    if (ppCtx)    *ppCtx    = g_d3d11_context;
    if (ppSwap)   *ppSwap   = g_dxgi_swapchain;
    return S_OK;
}

/* IDXGIFactory::CreateSwapChain (vtable[10]) — return our fake swap chain */
static u64 __attribute__((ms_abi))
dxgi_CreateSwapChain(u64 *obj, u64 *device, u64 *desc, u64 **ppSwap)
{
    (void)obj;(void)device;(void)desc;
    if (ppSwap) *ppSwap = g_dxgi_swapchain;
    return S_OK;
}

/* IDXGISwapChain::Present — no-op, return S_OK */
static u64 __attribute__((ms_abi))
dxgi_Present(u64 *obj, u32 sync, u32 flags)
    { (void)obj;(void)sync;(void)flags; return S_OK; }

/* IDXGISwapChain::GetBuffer */
static u64 __attribute__((ms_abi))
dxgi_GetBuffer(u64 *obj, u32 buf, u64 riid, void **pp)
    { (void)obj;(void)buf;(void)riid; if(pp)*pp=g_d3d11_device; return S_OK; }

/* IDXGISwapChain::ResizeBuffers */
static u64 __attribute__((ms_abi))
dxgi_ResizeBuffers(u64 *obj, u32 n, u32 w, u32 h, u32 fmt, u32 flags)
    { (void)obj;(void)n;(void)w;(void)h;(void)fmt;(void)flags; return S_OK; }

/* IDXGIAdapter stubs for EnumAdapters returning one fake adapter */
#define DXGI_VTAB2_SZ 64
static u64  g_dxgi_vtab2[DXGI_VTAB2_SZ];   /* adapter vtable */
static u64 *g_dxgi_adapter = NULL;

static u64 __attribute__((ms_abi))
dxgi_adapter_GetDesc(u64 *obj, u8 *desc)
{
    (void)obj;
    /* DXGI_ADAPTER_DESC: 128 wchars Description + other fields (total ~128+48=176 bytes) */
    if (desc) {
        memset(desc, 0, 176);
        /* Description: "Fake GPU\0" as wchar */
        const char *s = "Fake GPU"; int i;
        u16 *wd = (u16*)desc;
        for(i=0;s[i];i++) wd[i]=(u8)s[i]; wd[i]=0;
        /* DedicatedVideoMemory at offset 128+16 = 144: 1 GB */
        *(u64*)(desc+144) = 1024ULL*1024*1024;
    }
    return S_OK;
}

static void init_dxgi_adapter_fake(void) {
    for (int i=0;i<DXGI_VTAB2_SZ;i++) g_dxgi_vtab2[i]=(u64)dxgi_generic_fail;
    g_dxgi_vtab2[0]=(u64)dxgi_QueryInterface;
    g_dxgi_vtab2[1]=(u64)dxgi_AddRef;
    g_dxgi_vtab2[2]=(u64)dxgi_Release;
    g_dxgi_vtab2[8]=(u64)dxgi_adapter_GetDesc; /* IDXGIAdapter::GetDesc vtable[8] */

    g_dxgi_adapter=mmap(NULL,256,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(g_dxgi_adapter!=MAP_FAILED) g_dxgi_adapter[0]=(u64)g_dxgi_vtab2;
}

/* Override EnumAdapters to return our fake adapter */
static u64 __attribute__((ms_abi))
dxgi_EnumAdapters_with_fake(u64 *obj, u32 idx, u64 **pp) {
    (void)obj;
    if (idx == 0) {
        if (pp) *pp = g_dxgi_adapter;
        return S_OK; /* one adapter at index 0 */
    }
    if (pp) *pp = NULL;
    return (u64)DXGI_ERROR_NOT_FOUND;
}

/* ---- GetLogicalProcessorInformation ---- */
/* SYSTEM_LOGICAL_PROCESSOR_INFORMATION (32 bytes on x64) */
typedef struct {
    u64 ProcessorMask;
    u32 Relationship; /* 0=Core, 1=NUMA, 2=Cache, 3=Package, 4=Group */
    union {
        struct { u8 Flags; u8 _r[3]; } Core;
        struct { u32 NodeNumber; } Numa;
        u64 Reserved[2];
    };
} SLPI;

static u64 __attribute__((ms_abi))
impl_GetLogicalProcessorInformation(SLPI *buf, u32 *retlen)
{
    long ncpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpus < 1) ncpus = 4;
    if (ncpus > 64) ncpus = 64;

    u32 needed = (u32)(ncpus + 1) * sizeof(SLPI); /* ncpus Core entries + 1 Package */

    if (!buf || !retlen || *retlen < needed) {
        if (retlen) *retlen = needed;
        g_last_error = 122; /* ERROR_INSUFFICIENT_BUFFER */
        return 0; /* FALSE */
    }

    memset(buf, 0, needed);
    for (long i = 0; i < ncpus; i++) {
        buf[i].ProcessorMask = (u64)1 << i;
        buf[i].Relationship  = 0;        /* RelationProcessorCore */
        buf[i].Core.Flags    = 1;        /* HT enabled (hyper-threading) */
    }
    buf[ncpus].ProcessorMask = ((u64)1 << ncpus) - 1;
    buf[ncpus].Relationship  = 3; /* RelationProcessorPackage */

    *retlen = needed;
    return 1; /* TRUE */
}

/* ---- Fiber LS — same as TLS for our single-fiber model ---- */
static u64 __attribute__((ms_abi)) impl_FlsAlloc(u64 cb)
    { (void)cb; return impl_TlsAlloc(); }
static u64 __attribute__((ms_abi)) impl_FlsSetValue(u32 i, u64 v)
    { return impl_TlsSetValue(i, v); }
static u64 __attribute__((ms_abi)) impl_FlsGetValue(u32 i)
    { return impl_TlsGetValue(i); }
static u64 __attribute__((ms_abi)) impl_FlsFree(u32 i)
    { return impl_TlsFree(i); }

/* ---- Misc ---- */
static u64 __attribute__((ms_abi)) impl_GetSystemInfo(u64 *si) {
    if (!si) return 0;
    memset(si, 0, 36); /* SYSTEM_INFO is 36 bytes */
    u8 *b = (u8 *)si;
    *(u16*)(b + 0) = 9;    /* PROCESSOR_ARCHITECTURE_AMD64 */
    *(u32*)(b + 4) = 4096; /* dwPageSize */
    *(u32*)(b + 20) = 4;   /* dwNumberOfProcessors */
    *(u32*)(b + 32) = 15;  /* dwProcessorLevel */
    return 0;
}

static u64 __attribute__((ms_abi)) impl_GetCurrentProcess(void) { return (u64)-1; }

/* ---- Standard handles ---- */
static u64 __attribute__((ms_abi))
impl_GetStdHandle(u32 n) {
    /* STD_INPUT=-10, STD_OUTPUT=-11, STD_ERROR=-12 */
    switch ((int)n) {
        case -10: return (u64)(int64_t)-10;
        case -11: return (u64)(int64_t)-11;
        case -12: return (u64)(int64_t)-12;
    }
    return (u64)(int64_t)-1; /* INVALID_HANDLE_VALUE */
}

/* ---- Code pages / locale ---- */
static u64 __attribute__((ms_abi)) impl_GetACP(void)    { return 1252; }
static u64 __attribute__((ms_abi)) impl_GetOEMCP(void)  { return 437; }
static u64 __attribute__((ms_abi)) impl_GetCPInfo(u32 cp, u64 *info)
    { (void)cp; if(info){memset(info,0,12);*(u32*)info=1;}return 1; }
static u64 __attribute__((ms_abi)) impl_IsValidCodePage(u32 cp)
    { return (cp==1252||cp==65001||cp==437)?1:0; }

/* ---- Module file names ---- */
static u64 __attribute__((ms_abi))
impl_GetModuleFileNameW(u64 hmod, u16 *buf, u32 size) {
    (void)hmod;
    if (!buf || !size) return 0;
    const char *p = g_exe_path ? g_exe_path : "sekiro.exe";
    u32 i;
    for (i = 0; i < size - 1 && p[i]; i++) buf[i] = (u8)p[i];
    buf[i] = 0;
    return i;
}

/* ---- SLIST (single-linked list, used by heap internals) ---- */
static u64 __attribute__((ms_abi)) impl_InitializeSListHead(u64 *head)
    { if (head) head[0] = 0; return 0; }
static u64 __attribute__((ms_abi)) impl_InterlockedFlushSList(u64 *head)
    { if (head) { u64 r = head[0]; head[0] = 0; return r; } return 0; }

/* ---- Interlocked ops ---- */
static u64 __attribute__((ms_abi)) impl_InterlockedIncrement(volatile u32 *p)
    { return (u64)__sync_add_and_fetch(p, 1); }
static u64 __attribute__((ms_abi)) impl_InterlockedDecrement(volatile u32 *p)
    { return (u64)__sync_sub_and_fetch(p, 1); }
static u64 __attribute__((ms_abi)) impl_InterlockedExchange(volatile u32 *p, u32 v)
    { return (u64)__sync_lock_test_and_set(p, v); }
static u64 __attribute__((ms_abi)) impl_InterlockedCompareExchange(volatile u32 *p, u32 exch, u32 cmp)
    { return (u64)__sync_val_compare_and_swap(p, cmp, exch); }
static u64 __attribute__((ms_abi)) impl_InterlockedCompareExchange64(volatile u64 *p, u64 exch, u64 cmp)
    { return __sync_val_compare_and_swap(p, cmp, exch); }

/* ---- Synchronization events/semaphores ---- */
#define MAX_EVENTS 512
#define MAX_SEMS   512

typedef struct {
    int used;
    int manual_reset;
    int signaled;
    pthread_mutex_t mtx;
    pthread_cond_t  cv;
} WinEvent;

static WinEvent g_events[MAX_EVENTS];
static int g_event_cnt = 1;

static u32 g_sem_val[MAX_SEMS];
static u32 g_sem_max[MAX_SEMS];
static pthread_mutex_t g_sem_mtx[MAX_SEMS];
static pthread_cond_t  g_sem_cv[MAX_SEMS];
static int g_sem_cnt = 1;

static int event_id_from_handle(u64 h)
{
    u32 hv = (u32)h;
    if ((hv & 0xF0000000u) != 0xE0000000u) return -1;
    int id = (int)(hv - 0xE0000000u);
    if (id <= 0 || id >= g_event_cnt) return -1;
    if (!g_events[id].used) return -1;
    return id;
}

static int sem_id_from_handle(u64 h)
{
    u32 hv = (u32)h;
    if ((hv & 0xF0000000u) != 0xC0000000u) return -1;
    int id = (int)(hv - 0xC0000000u);
    if (id <= 0 || id >= g_sem_cnt) return -1;
    return id;
}

static int calc_abs_deadline(struct timespec *ts, u32 ms)
{
    if (clock_gettime(CLOCK_REALTIME, ts) != 0) return -1;
    ts->tv_sec  += (time_t)(ms / 1000);
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
    return 0;
}

static u64 event_alloc(int manual, int initial)
{
    if (g_event_cnt >= MAX_EVENTS) return 0;
    int id = g_event_cnt++;
    WinEvent *ev = &g_events[id];
    memset(ev, 0, sizeof(*ev));
    ev->used = 1;
    ev->manual_reset = manual ? 1 : 0;
    ev->signaled = initial ? 1 : 0;
    pthread_mutex_init(&ev->mtx, NULL);
    pthread_cond_init(&ev->cv, NULL);
    return (u64)(0xE0000000u + (u32)id);
}

static u64 sem_alloc(u32 init, u32 max)
{
    if (g_sem_cnt >= MAX_SEMS) return 0;
    int id = g_sem_cnt++;
    g_sem_val[id] = init;
    g_sem_max[id] = max ? max : 0x7FFFFFFFu;
    pthread_mutex_init(&g_sem_mtx[id], NULL);
    pthread_cond_init(&g_sem_cv[id], NULL);
    return (u64)(0xC0000000u + (u32)id);
}

/* Thread-handle wait helper (implemented in thread section below).
 * Returns 0/0x102/0xFFFFFFFF for known thread handles, or (u64)-2 if unknown. */
static u64 thread_wait_handle(u64 h, u32 ms);

static u64 __attribute__((ms_abi))
impl_CreateEventA(u64 sa, u32 manual, u32 initial, const char *name)
    { (void)sa;(void)name; return event_alloc((int)manual, (int)initial); }
static u64 __attribute__((ms_abi))
impl_CreateEventW(u64 sa, u32 manual, u32 initial, u64 name)
    { (void)sa;(void)name; return event_alloc((int)manual, (int)initial); }

static u64 __attribute__((ms_abi))
impl_SetEvent(u64 h)
{
    int eid = event_id_from_handle(h);
    if (eid < 0) return 0;
    WinEvent *ev = &g_events[eid];
    pthread_mutex_lock(&ev->mtx);
    ev->signaled = 1;
    if (ev->manual_reset) pthread_cond_broadcast(&ev->cv);
    else pthread_cond_signal(&ev->cv);
    pthread_mutex_unlock(&ev->mtx);
    return 1;
}

static u64 __attribute__((ms_abi))
impl_ResetEvent(u64 h)
{
    int eid = event_id_from_handle(h);
    if (eid < 0) return 0;
    WinEvent *ev = &g_events[eid];
    pthread_mutex_lock(&ev->mtx);
    ev->signaled = 0;
    pthread_mutex_unlock(&ev->mtx);
    return 1;
}

static u64 __attribute__((ms_abi))
impl_WaitForSingleObject(u64 h, u32 ms)
{
    /* WAIT_OBJECT_0=0, WAIT_TIMEOUT=0x102, WAIT_FAILED=0xFFFFFFFF */
    u64 trc = thread_wait_handle(h, ms);
    if (trc != (u64)-2) return trc;

    int eid = event_id_from_handle(h);
    if (eid >= 0) {
        WinEvent *ev = &g_events[eid];
        int rc = 0;
        pthread_mutex_lock(&ev->mtx);
        if (ms == 0xFFFFFFFFu) {
            if (!ev->signaled)
                fprintf(stderr, "[WAIT] WaitForSingleObject(event%d) blocking forever...\n", eid);
            while (!ev->signaled) pthread_cond_wait(&ev->cv, &ev->mtx);
        } else if (ms == 0) {
            if (!ev->signaled) rc = ETIMEDOUT;
        } else {
            struct timespec ts;
            if (calc_abs_deadline(&ts, ms) != 0) rc = ETIMEDOUT;
            while (!rc && !ev->signaled)
                rc = pthread_cond_timedwait(&ev->cv, &ev->mtx, &ts);
        }
        if (!rc && ev->signaled && !ev->manual_reset) ev->signaled = 0;
        pthread_mutex_unlock(&ev->mtx);
        return rc == ETIMEDOUT ? 0x102u : (rc ? 0xFFFFFFFFu : 0u);
    }

    int sid = sem_id_from_handle(h);
    if (sid >= 0) {
        int rc = 0;
        pthread_mutex_lock(&g_sem_mtx[sid]);
        if (ms == 0xFFFFFFFFu) {
            while (g_sem_val[sid] == 0) pthread_cond_wait(&g_sem_cv[sid], &g_sem_mtx[sid]);
        } else if (ms == 0) {
            if (g_sem_val[sid] == 0) rc = ETIMEDOUT;
        } else {
            struct timespec ts;
            if (calc_abs_deadline(&ts, ms) != 0) rc = ETIMEDOUT;
            while (!rc && g_sem_val[sid] == 0)
                rc = pthread_cond_timedwait(&g_sem_cv[sid], &g_sem_mtx[sid], &ts);
        }
        if (!rc && g_sem_val[sid] > 0) g_sem_val[sid]--;
        pthread_mutex_unlock(&g_sem_mtx[sid]);
        return rc == ETIMEDOUT ? 0x102u : (rc ? 0xFFFFFFFFu : 0u);
    }

    /* Unknown handle type: treat as already signaled. */
    return 0;
}

static u64 __attribute__((ms_abi))
impl_WaitForMultipleObjects(u32 n, u64 *handles, u32 all, u32 ms)
{
    if (!handles || n == 0) return 0xFFFFFFFFu;
    if (all) {
        for (u32 i = 0; i < n; i++) {
            u64 rc = impl_WaitForSingleObject(handles[i], ms);
            if (rc != 0) return rc;
        }
        return 0;
    }

    static volatile int wfmo_logged = 0;
    u32 waited = 0;
    for (;;) {
        for (u32 i = 0; i < n; i++) {
            u64 rc = impl_WaitForSingleObject(handles[i], 0);
            if (rc == 0) return i;
        }
        if (ms != 0xFFFFFFFFu && waited >= ms) return 0x102u;
        struct timespec ts = {0, 1000000L};
        nanosleep(&ts, NULL);
        if (ms != 0xFFFFFFFFu) waited++;
        else if (!wfmo_logged) {
            wfmo_logged = 1;
            fprintf(stderr, "[WAIT] WaitForMultipleObjects(n=%u, all=%u) spinning forever\n", n, all);
        }
    }
}

/* ---- CreateMutex ---- */
static u64 __attribute__((ms_abi))
impl_CreateMutexA(u64 sa, u32 own, const char *name)
    { (void)sa;(void)own;(void)name; return 0xA0000001; }
static u64 __attribute__((ms_abi))
impl_CreateMutexW(u64 sa, u32 own, u64 name)
    { (void)sa;(void)own;(void)name; return 0xA0000001; }
static u64 __attribute__((ms_abi)) impl_ReleaseMutex(u64 h) { (void)h; return 1; }
static u64 __attribute__((ms_abi)) impl_OpenMutexA(u32 a, u32 b, const char *n)
    { (void)a;(void)b;(void)n; return 0; }

/* ---- Sleep / Timing ---- */
static u64 __attribute__((ms_abi)) impl_Sleep(u32 ms) {
    static volatile u32 sleep_count = 0;
    u32 cnt = __sync_add_and_fetch(&sleep_count, 1);
    if (cnt == 1 || (cnt % 500) == 0)
        fprintf(stderr, "[SLEEP] Sleep(%u) call #%u\n", ms, cnt);
    if (ms) { struct timespec ts = {ms/1000, (ms%1000)*1000000L}; nanosleep(&ts,NULL); }
    return 0;
}
static u64 __attribute__((ms_abi)) impl_GetTickCount(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
static u64 __attribute__((ms_abi)) impl_GetTickCount64(void) { return impl_GetTickCount(); }

/* ---- File type / handles ---- */
static u64 __attribute__((ms_abi)) impl_GetFileType(u64 handle) {
    /* FILE_TYPE_CHAR = 2 for console, 0 = unknown */
    if ((int64_t)handle == -10 || (int64_t)handle == -11 || (int64_t)handle == -12)
        return 2;
    return 0;
}

static u64 __attribute__((ms_abi))
impl_SetStdHandle(u32 n, u64 h) { (void)n; (void)h; return 1; }

/* ---- Locale / string type functions ---- */
static u64 __attribute__((ms_abi))
impl_GetStringTypeW(u32 type, const u16 *src, int len, u16 *dst)
{
    (void)type;
    if (!src || !dst) return 0;
    if (len < 0) { int i=0; while(src[i]) i++; len=i; }
    for (int i = 0; i < len; i++) {
        u16 c = src[i], t = 0;
        if ((c>='A'&&c<='Z')) t = 0x0201;    /* UPPER | ALPHA */
        else if ((c>='a'&&c<='z')) t = 0x0101; /* LOWER | ALPHA */
        else if ((c>='0'&&c<='9')) t = 0x0004; /* DIGIT */
        else if (c==' '||c=='\t'||c=='\n'||c=='\r') t = 0x0048; /* SPACE|BLANK */
        else if (c < 0x20 || c == 0x7f) t = 0x0020; /* CNTRL */
        else t = 0x0010; /* PUNCT */
        dst[i] = t;
    }
    return 1;
}

static u64 __attribute__((ms_abi))
impl_GetStringTypeA(u32 locale, u32 type, const char *src, int len, u16 *dst)
{
    (void)locale;
    if (!src || !dst) return 0;
    if (len < 0) len = (int)strlen(src);
    for (int i = 0; i < len; i++) {
        u16 tmp = (u8)src[i];
        impl_GetStringTypeW(type, &tmp, 1, &dst[i]);
    }
    return 1;
}

static u64 __attribute__((ms_abi))
impl_LCMapStringW(u32 locale, u32 flags, const u16 *src, int srclen,
                   u16 *dst, int dstlen)
{
    (void)locale;
    if (srclen < 0) { int i=0; while(src[i]) i++; srclen=i+1; }
    if (!dst || !dstlen) return srclen;
    int n = srclen < dstlen ? srclen : dstlen;
    for (int i = 0; i < n; i++) {
        u16 c = src[i];
        if (flags & 0x100) { if (c>='A'&&c<='Z') c+=32; }      /* LCMAP_LOWERCASE */
        else if (flags & 0x200) { if (c>='a'&&c<='z') c-=32; } /* LCMAP_UPPERCASE */
        dst[i] = c;
    }
    return n;
}

static u64 __attribute__((ms_abi))
impl_LCMapStringA(u32 l, u32 f, const char *s, int sl, char *d, int dl)
    { (void)l;(void)f;(void)s;(void)sl;(void)d;(void)dl; return 0; }

/* GetLocaleInfoW: return minimal info */
static u64 __attribute__((ms_abi))
impl_GetLocaleInfoW(u32 locale, u32 type, u16 *buf, int size) {
    (void)locale; (void)type;
    if (buf && size > 0) { buf[0] = 0; }
    return 1;
}

static u64 __attribute__((ms_abi)) impl_GetUserDefaultLCID(void)   { return 0x0409; /* en-US */ }
static u64 __attribute__((ms_abi)) impl_GetSystemDefaultLCID(void) { return 0x0409; }
static u64 __attribute__((ms_abi)) impl_GetUserDefaultUILanguage(void) { return 0x0409; }

/* ---- FormatMessage (minimal) ---- */
static u64 __attribute__((ms_abi))
impl_FormatMessageA(u32 flags, u64 src, u32 msgid, u32 langid,
                     char *buf, u32 size, u64 args)
{
    (void)flags;(void)src;(void)msgid;(void)langid;(void)args;
    if (buf && size) { snprintf(buf, size, "error %u", msgid); return (u64)strlen(buf); }
    return 0;
}
static u64 __attribute__((ms_abi))
impl_FormatMessageW(u32 flags, u64 src, u32 msgid, u32 langid,
                     u16 *buf, u32 size, u64 args)
{
    (void)flags;(void)src;(void)langid;(void)args;
    if (buf && size) { buf[0]='?'; buf[1]=0; return 1; }
    return 0;
}

/* ---- RegisterClassEx / CreateWindow ---- */
#define FAKE_HWND  ((u64)0xD0000001u)
#define FAKE_HDC   ((u64)0xDC000001u)

static volatile int g_win_quit = 0;
static u32 g_win_quit_code = 0;

static u64 __attribute__((ms_abi))
impl_RegisterClassExA(u64 wndclass) { (void)wndclass; if (!g_api_registerclass) { g_api_registerclass=1; fprintf(stderr, "[PROGRESS] RegisterClassExA called - window registration started\n"); } return 1; }
static u64 __attribute__((ms_abi))
impl_RegisterClassExW(u64 wndclass) { (void)wndclass; if (!g_api_registerclass) { g_api_registerclass=1; fprintf(stderr, "[PROGRESS] RegisterClassExW called - window registration started\n"); } return 1; }
static u64 __attribute__((ms_abi))
impl_GetDesktopWindow(void) { return FAKE_HWND; }
static u64 __attribute__((ms_abi))
impl_GetForegroundWindow(void) { return FAKE_HWND; }
static u64 __attribute__((ms_abi))
impl_ShowWindow(u64 hw, u32 cmd) {
    fprintf(stderr, "[WIN] ShowWindow(hwnd=0x%lx, cmd=%u)\n", hw, cmd);
    return 1;
}
static u64 __attribute__((ms_abi))
impl_UpdateWindow(u64 hw) { (void)hw; return 1; }

/* ---- HMODULE GetModuleHandleExA/W ---- */
static u64 __attribute__((ms_abi))
impl_GetModuleHandleExW(u32 flags, u64 name, u64 *out)
    { (void)flags;(void)name; if(out)*out=(u64)g_img; return 1; }
static u64 __attribute__((ms_abi))
impl_GetModuleHandleExA(u32 flags, u64 name, u64 *out)
    { (void)flags;(void)name; if(out)*out=(u64)g_img; return 1; }

/* ---- Condition variables (CONDITION_VARIABLE = pointer-sized opaque) ---- */
static u64 __attribute__((ms_abi)) impl_InitializeConditionVariable(u64 *cv)
    { if(cv) *cv = 0; return 0; }
static u64 __attribute__((ms_abi)) impl_WakeConditionVariable(u64 *cv)
    { (void)cv; return 0; }
static u64 __attribute__((ms_abi)) impl_WakeAllConditionVariable(u64 *cv)
    { (void)cv; return 0; }
static u64 __attribute__((ms_abi))
impl_SleepConditionVariableCS(u64 *cv, WIN_CS *cs, u32 ms)
{
    (void)cv;
    impl_LeaveCriticalSection(cs);
    if (ms && ms != 0xFFFFFFFF) {
        struct timespec ts = { ms/1000, (long)(ms%1000)*1000000L };
        nanosleep(&ts, NULL);
    }
    impl_EnterCriticalSection(cs);
    return 1;
}
static u64 __attribute__((ms_abi))
impl_SleepConditionVariableSRW(u64 *cv, u64 *lock, u32 ms, u32 flags)
{
    (void)cv;(void)lock;(void)flags;
    if (ms && ms != 0xFFFFFFFF) {
        struct timespec ts = { ms/1000, (long)(ms%1000)*1000000L };
        nanosleep(&ts, NULL);
    }
    return 1;
}

/* ---- SRWLOCK ---- */
static u64 __attribute__((ms_abi)) impl_InitializeSRWLock(u64 *l)
    { if(l) *l=0; return 0; }
static u64 __attribute__((ms_abi)) impl_AcquireSRWLockExclusive(u64 *l)
    { if(l) *l=1; return 0; }
static u64 __attribute__((ms_abi)) impl_TryAcquireSRWLockExclusive(u64 *l)
    { if(l) *l=1; return 1; }
static u64 __attribute__((ms_abi)) impl_ReleaseSRWLockExclusive(u64 *l)
    { if(l) *l=0; return 0; }
static u64 __attribute__((ms_abi)) impl_AcquireSRWLockShared(u64 *l)
    { (void)l; return 0; }
static u64 __attribute__((ms_abi)) impl_TryAcquireSRWLockShared(u64 *l)
    { (void)l; return 1; }
static u64 __attribute__((ms_abi)) impl_ReleaseSRWLockShared(u64 *l)
    { (void)l; return 0; }

/* ---- InitializeCriticalSectionEx / InitOnceExecuteOnce ---- */
static u64 __attribute__((ms_abi))
impl_InitializeCriticalSectionEx(WIN_CS *cs, u32 spin, u32 flags)
    { (void)flags; return impl_InitializeCriticalSectionAndSpinCount(cs, spin); }

static u64 __attribute__((ms_abi))
impl_InitOnceExecuteOnce(u64 *once, u64 fn, u64 param, u64 *ctx)
{
    if (!once) return 0;
    if (*once == 0) {
        *once = 1;
        if (fn) {
            typedef u64 __attribute__((ms_abi)) (*InitFn)(u64*, u64, u64*);
            ((InitFn)fn)(once, param, ctx);
        }
    }
    return 1;
}

/* ---- Threads (real pthread implementation) ---- */
#define MAX_THREADS 512
#define THREAD_HANDLE_BASE 0xB2000000u

typedef struct {
    int used;
    int started;
    int suspended;
    int done;
    u32 tid;
    pthread_t pt;
    u64 fn;
    u64 arg;
    pthread_mutex_t mtx;
    pthread_cond_t  cv;
} WinThread;

static WinThread g_threads[MAX_THREADS];
static int g_thread_cnt = 1;

/* Forward declaration — alloc_teb_for_thread() is defined later near TEB setup.
 * It returns WinTEB* cast to void*. */
static void *alloc_teb_for_thread(void);

static int thread_id_from_handle(u64 h)
{
    u32 hv = (u32)h;
    if ((hv & 0xFF000000u) != THREAD_HANDLE_BASE) return -1;
    int id = (int)(hv - THREAD_HANDLE_BASE);
    if (id <= 0 || id >= g_thread_cnt) return -1;
    if (!g_threads[id].used) return -1;
    return id;
}

static void thread_done_cleanup(void *p)
{
    int id = (int)(intptr_t)p;
    if (id <= 0 || id >= g_thread_cnt) return;
    WinThread *t = &g_threads[id];
    pthread_mutex_lock(&t->mtx);
    t->done = 1;
    pthread_cond_broadcast(&t->cv);
    pthread_mutex_unlock(&t->mtx);
}

static void *win_thread_trampoline(void *p) {
    int id = (int)(intptr_t)p;
    if (id <= 0 || id >= g_thread_cnt) return NULL;
    WinThread *t = &g_threads[id];

    /* Give this thread its own private TEB + TLS array so implicit TLS
     * (__declspec(thread) via gs:[0x58][index]) is per-thread, not shared. */
    void *teb = alloc_teb_for_thread();
    int gs_rc = syscall(SYS_arch_prctl, ARCH_SET_GS, (u64)teb);
    if (gs_rc != 0) fprintf(stderr, "[GS] CreateThread: arch_prctl failed: %d\n", gs_rc);

    pthread_mutex_lock(&t->mtx);
    t->tid = (u32)syscall(SYS_gettid);
    pthread_cond_broadcast(&t->cv);
    pthread_mutex_unlock(&t->mtx);

    /* Ensure done is signalled even if thread exits via pthread_exit (ExitThread) */
    pthread_cleanup_push(thread_done_cleanup, (void *)(intptr_t)id);

    typedef u64 __attribute__((ms_abi)) (*WinFn)(u64);
    ((WinFn)t->fn)(t->arg);

    pthread_cleanup_pop(1);  /* calls thread_done_cleanup */
    return NULL;
}

static int thread_start_locked(int id)
{
    WinThread *t = &g_threads[id];
    if (t->started || !t->used) return 0;
    if (pthread_create(&t->pt, NULL, win_thread_trampoline, (void *)(intptr_t)id) != 0)
        return -1;
    t->started = 1;
    t->suspended = 0;
    return 0;
}

static u64 thread_wait_handle(u64 h, u32 ms)
{
    int id = thread_id_from_handle(h);
    if (id < 0) return (u64)-2;

    WinThread *t = &g_threads[id];
    int rc = 0;
    pthread_mutex_lock(&t->mtx);

    if (!t->started && t->suspended) {
        if (ms == 0) {
            pthread_mutex_unlock(&t->mtx);
            return 0x102u;
        }
        if (ms == 0xFFFFFFFFu) {
            while (!t->started && t->used) pthread_cond_wait(&t->cv, &t->mtx);
        } else {
            struct timespec ts;
            if (calc_abs_deadline(&ts, ms) != 0) rc = ETIMEDOUT;
            while (!rc && !t->started && t->used)
                rc = pthread_cond_timedwait(&t->cv, &t->mtx, &ts);
        }
    }

    if (!rc && t->started && !t->done) {
        if (ms == 0xFFFFFFFFu) {
            while (!t->done && t->used) pthread_cond_wait(&t->cv, &t->mtx);
        } else if (ms == 0) {
            rc = ETIMEDOUT;
        } else {
            struct timespec ts;
            if (calc_abs_deadline(&ts, ms) != 0) rc = ETIMEDOUT;
            while (!rc && !t->done && t->used)
                rc = pthread_cond_timedwait(&t->cv, &t->mtx, &ts);
        }
    }

    pthread_mutex_unlock(&t->mtx);
    return rc == ETIMEDOUT ? 0x102u : (rc ? 0xFFFFFFFFu : 0u);
}

static u64 __attribute__((ms_abi))
impl_CreateThread(u64 attr, u64 stack, u64 fn, u64 arg, u32 flags, u32 *tid)
{
    (void)attr;(void)stack;
    if (g_thread_cnt >= MAX_THREADS) return 0;
    int id = g_thread_cnt++;
    WinThread *t = &g_threads[id];
    memset(t, 0, sizeof(*t));
    t->used = 1;
    t->fn = fn;
    t->arg = arg;
    t->suspended = (flags & 0x4) ? 1 : 0; /* CREATE_SUSPENDED */
    pthread_mutex_init(&t->mtx, NULL);
    pthread_cond_init(&t->cv, NULL);

    if (!t->suspended && thread_start_locked(id) != 0) {
        pthread_mutex_destroy(&t->mtx);
        pthread_cond_destroy(&t->cv);
        t->used = 0;
        return 0;
    }

    if (tid) *tid = t->tid ? t->tid : (u32)id;
    return (u64)(THREAD_HANDLE_BASE + (u32)id);
}
static u64 __attribute__((ms_abi)) impl_GetCurrentThread(void)            { return (u64)-2; }
static u64 __attribute__((ms_abi))
impl_SuspendThread(u64 h)
{
    int id = thread_id_from_handle(h);
    if (id < 0) return (u64)-1;
    WinThread *t = &g_threads[id];
    pthread_mutex_lock(&t->mtx);
    u64 prev = t->suspended ? 1 : 0;
    if (!t->started) t->suspended = 1;
    pthread_mutex_unlock(&t->mtx);
    return prev;
}
static u64 __attribute__((ms_abi))
impl_ResumeThread(u64 h)
{
    int id = thread_id_from_handle(h);
    if (id < 0) return (u64)-1;
    WinThread *t = &g_threads[id];
    pthread_mutex_lock(&t->mtx);
    u64 prev = t->suspended ? 1 : 0;
    if (t->suspended && !t->started) {
        if (thread_start_locked(id) != 0) {
            pthread_mutex_unlock(&t->mtx);
            return (u64)-1;
        }
        pthread_cond_broadcast(&t->cv);
    }
    pthread_mutex_unlock(&t->mtx);
    return prev;
}
static u64 __attribute__((ms_abi)) impl_SetThreadPriority(u64 h, u32 p)   { (void)h;(void)p; return 1; }

/* ---- Thread exit (must not return) ---- */
static u64 __attribute__((ms_abi)) impl_ExitThread(u32 code) {
    pthread_exit((void*)(uintptr_t)code);
}
static u64 __attribute__((ms_abi))
impl_FreeLibraryAndExitThread(u64 hmod, u32 code)
    { (void)hmod; pthread_exit((void*)(uintptr_t)code); }
static u64 __attribute__((ms_abi))
impl_TerminateThread(u64 h, u32 code)
{
    (void)code;
    int id = thread_id_from_handle(h);
    if (id < 0) return 0;
    WinThread *t = &g_threads[id];
    pthread_mutex_lock(&t->mtx);
    if (t->started && !t->done) {
        pthread_cancel(t->pt);
        t->done = 1;
        pthread_cond_broadcast(&t->cv);
    }
    pthread_mutex_unlock(&t->mtx);
    return 1;
}

/* OpenThread — return a fake handle */
static u64 __attribute__((ms_abi))
impl_OpenThread(u32 access, u32 inherit, u32 tid)
{
    (void)access;(void)inherit;
    for (int i = 1; i < g_thread_cnt; i++) {
        if (g_threads[i].used && g_threads[i].tid == tid)
            return (u64)(THREAD_HANDLE_BASE + (u32)i);
    }
    return 0;
}
static u64 __attribute__((ms_abi)) impl_GetCurrentProcessorNumber(void)   { return 0; }
static u64 __attribute__((ms_abi)) impl_FlushProcessWriteBuffers(void)    { return 0; }

static u64 __attribute__((ms_abi))
impl_CreateSemaphoreW(u64 sa, u32 init, u32 max, u64 name) { (void)sa;(void)name; return sem_alloc(init,max); }
static u64 __attribute__((ms_abi))
impl_CreateSemaphoreA(u64 sa, u32 init, u32 max, u64 name) { (void)sa;(void)name; return sem_alloc(init,max); }
static u64 __attribute__((ms_abi))
impl_OpenSemaphoreW(u32 a, u32 b, u64 n) { (void)a;(void)b;(void)n; return sem_alloc(0,0x7FFFFFFF); }

/* ---- Debug output (print to stderr) ---- */
static u64 __attribute__((ms_abi))
impl_OutputDebugStringA(const char *msg)
    { if(msg) fprintf(stderr, "[DBG] %s\n", msg); return 0; }
static u64 __attribute__((ms_abi))
impl_OutputDebugStringW(const u16 *msg) {
    if (!msg) return 0;
    char buf[256]; int i;
    for (i=0; i<255 && msg[i]; i++) buf[i]=(char)(msg[i]&0x7f);
    buf[i]=0;
    fprintf(stderr, "[DBG] %s\n", buf);
    return 0;
}

/* ---- File operations (minimal) ---- */
#define INVALID_HANDLE_VALUE64 ((u64)(int64_t)-1)

static u64 __attribute__((ms_abi))
impl_GetTempPathW(u32 sz, u16 *buf) {
    static const char *tmp = "/tmp/";
    if (!buf || !sz) return 5;
    int i; for(i=0;i<(int)sz-1&&tmp[i];i++) buf[i]=(u8)tmp[i]; buf[i]=0;
    return (u64)i;
}
static u64 __attribute__((ms_abi))
impl_GetTempPathA(u32 sz, char *buf) {
    if (!buf || !sz) return 5;
    strncpy(buf,"/tmp/",sz-1); buf[sz-1]=0; return 5;
}

static u64 __attribute__((ms_abi))
impl_GetFullPathNameW(const u16 *path, u32 sz, u16 *buf, u16 **part) {
    if (!buf || !sz || !path) return 0;
    int i; for(i=0;i<(int)sz-1&&path[i];i++) buf[i]=path[i]; buf[i]=0;
    if (part) *part = buf;
    return (u64)i;
}
static u64 __attribute__((ms_abi))
impl_GetFullPathNameA(const char *path, u32 sz, char *buf, char **part) {
    if (!buf || !sz || !path) return 0;
    strncpy(buf,path,sz-1); buf[sz-1]=0;
    if (part) *part = buf;
    return (u64)strlen(buf);
}

/* Fake file handle pool */
#define MAX_FHANDLES 256
static int g_fh_fd[MAX_FHANDLES];
static int g_fh_cnt = 1;

static u64 fh_alloc(int fd) {
    if (g_fh_cnt >= MAX_FHANDLES) return INVALID_HANDLE_VALUE64;
    int id = g_fh_cnt++;
    g_fh_fd[id] = fd;
    return (u64)(0xF0000000u + (u32)id);
}
static int fh_get(u64 h) {
    u32 id = (u32)(h - 0xF0000000u);
    if (id < (u32)g_fh_cnt) return g_fh_fd[id];
    return -1;
}

static u64 __attribute__((ms_abi))
impl_CreateFileA(const char *name, u32 access, u32 share, u64 sa,
                  u32 creation, u32 attrs, u64 tmpl)
{
    (void)sa;(void)attrs;(void)tmpl;(void)share;
    if (!name) return INVALID_HANDLE_VALUE64;
    int flags = 0;
    int mode  = 0644;
    if ((access & 0xC0000000) == 0xC0000000) flags = O_RDWR;
    else if (access & 0x80000000)            flags = O_RDONLY;
    else if (access & 0x40000000)            flags = O_WRONLY;
    else                                     flags = O_RDONLY;
    if (creation == 2)       flags |= O_CREAT|O_TRUNC;  /* CREATE_ALWAYS */
    else if (creation == 1)  flags |= O_CREAT|O_EXCL;   /* CREATE_NEW */
    else if (creation == 4)  flags |= O_CREAT;          /* OPEN_ALWAYS */
    /* creation==3 = OPEN_EXISTING: no extra flags */
    int fd = open(name, flags, mode);
    if (fd < 0) { g_last_error = 2; return INVALID_HANDLE_VALUE64; }
    return fh_alloc(fd);
}
static u64 __attribute__((ms_abi))
impl_CreateFileW(const u16 *name, u32 acc, u32 share, u64 sa, u32 cr, u32 att, u64 tmpl)
{
    char buf[512]; int i;
    for(i=0;i<511&&name&&name[i];i++) buf[i]=(char)(name[i]&0x7f);
    buf[i]=0;
    return impl_CreateFileA(buf, acc, share, sa, cr, att, tmpl);
}
static u64 __attribute__((ms_abi))
impl_CloseHandle(u64 h)
{
    int tid = thread_id_from_handle(h);
    if (tid >= 0) {
        WinThread *t = &g_threads[tid];
        pthread_mutex_lock(&t->mtx);
        if (t->started && !t->done) pthread_detach(t->pt);
        t->used = 0;
        pthread_mutex_unlock(&t->mtx);
        pthread_mutex_destroy(&t->mtx);
        pthread_cond_destroy(&t->cv);
        return 1;
    }

    int eid = event_id_from_handle(h);
    if (eid >= 0) {
        WinEvent *ev = &g_events[eid];
        pthread_mutex_destroy(&ev->mtx);
        pthread_cond_destroy(&ev->cv);
        ev->used = 0;
        return 1;
    }

    int sid = sem_id_from_handle(h);
    if (sid >= 0) {
        pthread_mutex_destroy(&g_sem_mtx[sid]);
        pthread_cond_destroy(&g_sem_cv[sid]);
        g_sem_val[sid] = 0;
        g_sem_max[sid] = 0;
        return 1;
    }

    int fd = fh_get(h);
    if (fd >= 0) close(fd);
    return 1;
}
static u64 __attribute__((ms_abi))
impl_ReadFile(u64 h, void *buf, u32 n, u32 *done, u64 ov)
{
    (void)ov;
    int fd = fh_get(h);
    if (fd < 0) { if(done)*done=0; return 0; }
    ssize_t r = read(fd, buf, n);
    if (done) *done = (u32)(r<0?0:r);
    return r >= 0 ? 1 : 0;
}
static u64 __attribute__((ms_abi))
impl_GetFileSize(u64 h, u32 *high) {
    int fd = fh_get(h);
    if (fd < 0) return INVALID_HANDLE_VALUE64;
    struct stat st; fstat(fd, &st);
    if (high) *high = (u32)(st.st_size >> 32);
    return (u64)(u32)st.st_size;
}
static u64 __attribute__((ms_abi))
impl_SetFilePointer(u64 h, s32 lo, s32 *hi, u32 method) {
    int fd = fh_get(h); if(fd<0) return INVALID_HANDLE_VALUE64;
    int whence = (method==0)?SEEK_SET:(method==1)?SEEK_CUR:SEEK_END;
    off_t dist = lo;
    if (hi) dist |= ((off_t)(u32)*hi << 32);
    off_t r = lseek(fd, dist, whence);
    if (hi) *hi = (s32)(r>>32);
    return (u64)(u32)r;
}

static u64 __attribute__((ms_abi))
impl_GetFileAttributesA(const char *path) {
    struct stat st;
    if (stat(path, &st) < 0) { g_last_error = 2; return 0xFFFFFFFF; }
    u32 attr = 0x20; /* FILE_ATTRIBUTE_NORMAL */
    if (S_ISDIR(st.st_mode)) attr = 0x10; /* FILE_ATTRIBUTE_DIRECTORY */
    return attr;
}
static u64 __attribute__((ms_abi))
impl_GetFileAttributesW(const u16 *path) {
    char buf[512]; int i;
    for(i=0;i<511&&path&&path[i];i++) buf[i]=(char)(path[i]&0x7f); buf[i]=0;
    return impl_GetFileAttributesA(buf);
}
static u64 __attribute__((ms_abi))
impl_SetFileAttributesA(const char *p, u32 a) { (void)p;(void)a; return 1; }
static u64 __attribute__((ms_abi))
impl_SetFileAttributesW(const u16 *p, u32 a) { (void)p;(void)a; return 1; }

/* SetThreadAffinityMask */
static u64 __attribute__((ms_abi))
impl_SetThreadAffinityMask(u64 h, u64 mask) { (void)h;(void)mask; return mask; }

/* LocalAlloc / LocalFree / GlobalAlloc / GlobalFree */
static u64 __attribute__((ms_abi)) impl_LocalAlloc(u32 f, u64 sz)
    { (void)f; return (u64)malloc((size_t)sz); }
static u64 __attribute__((ms_abi)) impl_LocalFree(void *p)
    { (void)p; return 0; }
static u64 __attribute__((ms_abi)) impl_GlobalAlloc(u32 f, u64 sz)
    { (void)f; return (u64)malloc((size_t)sz); }
static u64 __attribute__((ms_abi)) impl_GlobalFree(void *p)
    { (void)p; return 0; }

/* SetErrorMode */
static u32 g_error_mode = 0;
static u64 __attribute__((ms_abi)) impl_SetErrorMode(u32 mode)
    { u32 old=g_error_mode; g_error_mode=mode; return old; }

/* ── USER32 window system ─────────────────────────────────────────── */

static u64 __attribute__((ms_abi))
impl_GetWindowRect(u64 hwnd, u64 *rect) {
    (void)hwnd;
    if (rect) { rect[0]=0; rect[1]=0; rect[2]=1920; rect[3]=1080; }
    return 1;
}
static u64 __attribute__((ms_abi))
impl_GetClientRect(u64 hwnd, u64 *rect) { return impl_GetWindowRect(hwnd, rect); }
static u64 __attribute__((ms_abi))
impl_LoadIconW(u64 hmod, u64 name) { (void)hmod;(void)name; return 0x10000001; }
static u64 __attribute__((ms_abi))
impl_LoadIconA(u64 hmod, u64 name) { (void)hmod;(void)name; return 0x10000001; }
static u64 __attribute__((ms_abi))
impl_LoadCursorW(u64 hmod, u64 name) { (void)hmod;(void)name; return 0x20000001; }
static u64 __attribute__((ms_abi))
impl_LoadCursorA(u64 hmod, u64 name) { (void)hmod;(void)name; return 0x20000001; }
static u64 __attribute__((ms_abi))
impl_AdjustWindowRect(u64 *rect, u32 style, u32 menu) {
    (void)style;(void)menu;
    if (rect) { rect[0]-=8; rect[1]-=30; rect[2]+=8; rect[3]+=8; }
    return 1;
}
static u64 __attribute__((ms_abi))
impl_AdjustWindowRectEx(u64 *rect, u32 style, u32 menu, u32 exstyle)
    { (void)exstyle; return impl_AdjustWindowRect(rect,style,menu); }
static u64 __attribute__((ms_abi))
impl_CreateWindowExW(u32 exstyle, u64 classname, u64 title, u32 style,
                      s32 x, s32 y, s32 w, s32 h, u64 parent, u64 menu, u64 inst, u64 param)
{
    (void)exstyle;(void)classname;(void)title;(void)style;
    (void)x;(void)y;(void)w;(void)h;(void)parent;(void)menu;(void)inst;(void)param;
    if (!g_api_createwindow) { g_api_createwindow=1; fprintf(stderr, "[PROGRESS] CreateWindowExW called - window created\n"); }
    fprintf(stderr, "[WIN] CreateWindowExW(%dx%d) -> HWND\n", w, h);
    return FAKE_HWND;
}
static u64 __attribute__((ms_abi))
impl_CreateWindowExA(u32 exstyle, u64 classname, u64 title, u32 style,
                      s32 x, s32 y, s32 w, s32 h, u64 parent, u64 menu, u64 inst, u64 param)
{
    (void)exstyle;(void)classname;(void)title;(void)style;
    (void)x;(void)y;(void)w;(void)h;(void)parent;(void)menu;(void)inst;(void)param;
    if (!g_api_createwindow) { g_api_createwindow=1; fprintf(stderr, "[PROGRESS] CreateWindowExA called - window created\n"); }
    fprintf(stderr, "[WIN] CreateWindowExA(%dx%d) -> HWND\n", w, h);
    return FAKE_HWND;
}
static u64 __attribute__((ms_abi)) impl_DestroyWindow(u64 hw) { (void)hw; return 1; }
static u64 __attribute__((ms_abi)) impl_IsWindow(u64 hw) { return hw==FAKE_HWND?1:0; }
static u64 __attribute__((ms_abi))
impl_SetWindowTextW(u64 hw, u64 txt) { (void)hw;(void)txt; return 1; }
static u64 __attribute__((ms_abi))
impl_SetWindowTextA(u64 hw, u64 txt) { (void)hw;(void)txt; return 1; }
static u64 __attribute__((ms_abi))
impl_MoveWindow(u64 hw, s32 x, s32 y, s32 w, s32 h, u32 rep)
    { (void)hw;(void)x;(void)y;(void)w;(void)h;(void)rep; return 1; }
static u64 __attribute__((ms_abi))
impl_SetWindowPos(u64 hw, u64 ins, s32 x, s32 y, s32 w, s32 h, u32 flags)
    { (void)hw;(void)ins;(void)x;(void)y;(void)w;(void)h;(void)flags; return 1; }
static u64 __attribute__((ms_abi))
impl_GetDC(u64 hw) { (void)hw; return FAKE_HDC; }
static u64 __attribute__((ms_abi))
impl_ReleaseDC(u64 hw, u64 dc) { (void)hw;(void)dc; return 1; }
static u64 __attribute__((ms_abi))
impl_SetCursor(u64 hc) { (void)hc; return 0; }
static u64 __attribute__((ms_abi))
impl_ShowCursor(s32 show) { (void)show; return 1; }
static u64 __attribute__((ms_abi))
impl_SetCapture(u64 hw) { (void)hw; return 0; }
static u64 __attribute__((ms_abi))
impl_ReleaseCapture(void) { return 1; }
static u64 __attribute__((ms_abi))
impl_SetFocus(u64 hw) { (void)hw; return FAKE_HWND; }
static u64 __attribute__((ms_abi))
impl_GetFocus(void) { return FAKE_HWND; }
static u64 __attribute__((ms_abi))
impl_GetActiveWindow(void) { return FAKE_HWND; }

/* Message loop */
static u64 __attribute__((ms_abi))
impl_PeekMessageW(u64 *msg, u64 hw, u32 min, u32 max, u32 remove) {
    (void)hw;(void)min;(void)max;(void)remove;
    if (msg) memset(msg, 0, 5*8); /* MSG struct */
    if (g_win_quit) {
        if (msg) {
            msg[0] = FAKE_HWND;  /* hwnd */
            msg[1] = 0x0012;     /* WM_QUIT */
            msg[2] = g_win_quit_code;
        }
        return 1;
    }
    return 0; /* no messages */
}
static u64 __attribute__((ms_abi))
impl_GetMessageW(u64 *msg, u64 hw, u32 min, u32 max) {
    (void)hw;(void)min;(void)max;
    if (msg) memset(msg, 0, 5*8);
    if (g_win_quit) {
        if (msg) {
            msg[0] = FAKE_HWND;  /* hwnd */
            msg[1] = 0x0012;     /* WM_QUIT */
            msg[2] = g_win_quit_code;
        }
        return 0;
    }

    /* Keep message loops alive instead of returning WM_QUIT immediately. */
    if (msg) {
        msg[0] = FAKE_HWND; /* hwnd */
        msg[1] = 0x0000;    /* WM_NULL */
    }
    struct timespec ts = {0, 1000000L};
    nanosleep(&ts, NULL);
    return 1;
}
static u64 __attribute__((ms_abi))
impl_TranslateMessage(u64 *msg) { (void)msg; return 0; }
static u64 __attribute__((ms_abi))
impl_DispatchMessageW(u64 *msg) { (void)msg; return 0; }
static u64 __attribute__((ms_abi))
impl_PostQuitMessage(u32 code) { g_win_quit = 1; g_win_quit_code = code; return 0; }
static u64 __attribute__((ms_abi))
impl_SendMessageW(u64 hw, u32 msg, u64 wp, u64 lp)
    { (void)hw;(void)msg;(void)wp;(void)lp; return 0; }
static u64 __attribute__((ms_abi))
impl_PostMessageW(u64 hw, u32 msg, u64 wp, u64 lp)
    { (void)hw;(void)msg;(void)wp;(void)lp; return 1; }
static u64 __attribute__((ms_abi))
impl_DefWindowProcW(u64 hw, u32 msg, u64 wp, u64 lp)
    { (void)hw;(void)msg;(void)wp;(void)lp; return 0; }
static u64 __attribute__((ms_abi))
impl_CallWindowProcW(u64 proc, u64 hw, u32 msg, u64 wp, u64 lp)
    { (void)proc;(void)hw;(void)msg;(void)wp;(void)lp; return 0; }
static u64 __attribute__((ms_abi))
impl_SetWindowLongPtrW(u64 hw, s32 idx, u64 new)
    { (void)hw;(void)idx;(void)new; return 0; }
static u64 __attribute__((ms_abi))
impl_GetWindowLongPtrW(u64 hw, s32 idx) { (void)hw;(void)idx; return 0; }
static u64 __attribute__((ms_abi))
impl_SetWindowLongW(u64 hw, s32 idx, s32 new)
    { (void)hw;(void)idx;(void)new; return 0; }
static u64 __attribute__((ms_abi))
impl_GetWindowLongW(u64 hw, s32 idx) { (void)hw;(void)idx; return 0; }
static u64 __attribute__((ms_abi))
impl_SetForegroundWindow(u64 hw) { (void)hw; return 1; }
static u64 __attribute__((ms_abi))
impl_SetWindowsHookExW(s32 type, u64 fn, u64 mod, u32 tid)
    { (void)type;(void)fn;(void)mod;(void)tid; return 0xD0010002u; }
static u64 __attribute__((ms_abi))
impl_UnhookWindowsHookEx(u64 hk) { (void)hk; return 1; }
static u64 __attribute__((ms_abi))
impl_CallNextHookEx(u64 hk, s32 code, u64 wp, u64 lp)
    { (void)hk;(void)code;(void)wp;(void)lp; return 0; }
static u64 __attribute__((ms_abi))
impl_GetSystemMetrics(u32 n) {
    switch(n) {
        case 0:  return 1920; /* SM_CXSCREEN */
        case 1:  return 1080; /* SM_CYSCREEN */
        case 78: return 1920; /* SM_CXVIRTUALSCREEN */
        case 79: return 1080; /* SM_CYVIRTUALSCREEN */
        case 11: case 12: return 16; /* SM_CXICON, SM_CYICON */
    }
    return 0;
}
static u64 __attribute__((ms_abi))
impl_MonitorFromWindow(u64 hw, u32 flags) { (void)hw;(void)flags; return 0xD0000002u; }
static u64 __attribute__((ms_abi))
impl_GetMonitorInfoW(u64 hm, u64 *mi) {
    (void)hm;
    if (mi) {
        u8 *b = (u8*)mi;
        *(u32*)b = 40; /* cbSize */
        /* rcMonitor: {0,0,1920,1080} */
        *(s32*)(b+ 4)=0; *(s32*)(b+ 8)=0; *(s32*)(b+12)=1920; *(s32*)(b+16)=1080;
        /* rcWork: same */
        *(s32*)(b+20)=0; *(s32*)(b+24)=0; *(s32*)(b+28)=1920; *(s32*)(b+32)=1080;
        *(u32*)(b+36)=1; /* dwFlags = MONITORINFOF_PRIMARY */
    }
    return 1;
}

/* ---- Pointer encoding (security feature — XOR with secret) ---- */
static const u64 g_ptr_secret = 0xA57C3F91D2E84B60ULL;
static u64 __attribute__((ms_abi)) impl_EncodePointer(u64 p)       { return p ^ g_ptr_secret; }
static u64 __attribute__((ms_abi)) impl_DecodePointer(u64 p)       { return p ^ g_ptr_secret; }
static u64 __attribute__((ms_abi)) impl_EncodeSystemPointer(u64 p) { return p ^ g_ptr_secret; }
static u64 __attribute__((ms_abi)) impl_DecodeSystemPointer(u64 p) { return p ^ g_ptr_secret; }

/* ---- Thread / process times ---- */
static u64 __attribute__((ms_abi))
impl_GetThreadTimes(u64 h, u64 *create, u64 *exit, u64 *kernel, u64 *user)
{
    (void)h;
    u64 now = 0;
    impl_GetSystemTimeAsFileTime(&now);
    if (create) *create = now;
    if (exit)   *exit   = 0;
    if (kernel) *kernel = 0;
    if (user)   *user   = 0;
    return 1;
}

static u64 __attribute__((ms_abi))
impl_GetSystemTimePreciseAsFileTime(u64 *ft) { return impl_GetSystemTimeAsFileTime(ft); }

static u64 __attribute__((ms_abi))
impl_GetProcessTimes(u64 h, u64 *create, u64 *exit, u64 *kernel, u64 *user)
    { return impl_GetThreadTimes(h, create, exit, kernel, user); }
static u64 __attribute__((ms_abi))
impl_CreateSemaphoreExW(u64 a, u32 b, u32 c, u64 d, u32 e, u32 f)
    { (void)a;(void)d;(void)e;(void)f; return sem_alloc(b, c); }
static u64 __attribute__((ms_abi))
impl_ReleaseSemaphore(u64 h, u32 n, u32 *prev)
{
    int sid = sem_id_from_handle(h);
    if (sid < 0) return 0;
    pthread_mutex_lock(&g_sem_mtx[sid]);
    if (prev) *prev = g_sem_val[sid];
    u64 nv = (u64)g_sem_val[sid] + n;
    if (nv > g_sem_max[sid]) nv = g_sem_max[sid];
    g_sem_val[sid] = (u32)nv;
    if (n) pthread_cond_broadcast(&g_sem_cv[sid]);
    pthread_mutex_unlock(&g_sem_mtx[sid]);
    return 1;
}

/* ---- Exception raise ---- */
static u64 __attribute__((ms_abi))
impl_RaiseException(u32 code, u32 flags, u32 nargs, const u64 *args)
{
    fputs("[TRACE] impl_RaiseException called\n", stderr);
    fprintf(stderr, "[IMPL] RaiseException(code=0x%08x flags=0x%x nargs=%u)\n",
            code, flags, nargs);
    for (u32 i = 0; i < nargs && i < 8 && args; i++)
        fprintf(stderr, "         args[%u]=0x%lx\n", i, args[i]);
    fflush(stderr);
    if (flags & 1) { /* EXCEPTION_NONCONTINUABLE */
        exit(1);
    }
    return 0; /* continuable — caller handles it */
}

/* ---- RtlPcToFileHeader ---- */
static u64 __attribute__((ms_abi))
impl_RtlPcToFileHeader(u64 pc, u64 **base_out)
{
    NtHdrs64 *nt = (NtHdrs64 *)(g_img + ((DosHdr *)g_img)->lfanew);
    u64 base = (u64)g_img;
    if (pc >= base && pc < base + nt->opt.sz_image) {
        if (base_out) *base_out = (u64 *)base;
        return base;
    }
    if (base_out) *base_out = NULL;
    return 0;
}

/* ---- Locale callbacks ---- */
static u64 __attribute__((ms_abi))
impl_EnumSystemLocalesW(u64 callback, u32 flags)
{
    (void)flags;
    if (callback) {
        static u16 loc[] = {'0','4','0','9',0}; /* en-US */
        typedef u64 __attribute__((ms_abi)) (*Cb)(u16 *);
        ((Cb)callback)(loc);
    }
    return 1;
}
static u64 __attribute__((ms_abi))
impl_IsValidLocale(u32 lcid, u32 flags) { (void)flags; return lcid <= 0xFFFF ? 1 : 0; }
static u64 __attribute__((ms_abi))
impl_GetLocaleInfoA(u32 l, u32 t, char *buf, int sz)
    { (void)l;(void)t; if(buf&&sz){buf[0]='0';buf[1]=0;} return 1; }
static u64 __attribute__((ms_abi))
impl_GetUserDefaultLocaleName(u16 *buf, int sz)
{
    static u16 name[] = {'e','n','-','U','S',0};
    if (buf && sz > 5) { int i=0; while(name[i]) { buf[i]=name[i]; i++; } buf[i]=0; }
    return 5;
}
static u64 __attribute__((ms_abi))
impl_CompareStringW(u32 l, u32 f, const u16 *s1, int n1, const u16 *s2, int n2)
{
    (void)l;(void)f;
    if (n1<0) { int i=0; while(s1[i]) i++; n1=i; }
    if (n2<0) { int i=0; while(s2[i]) i++; n2=i; }
    int n = n1<n2?n1:n2;
    for (int i=0;i<n;i++) if(s1[i]!=s2[i]) return s1[i]<s2[i]?1:3;
    return n1==n2?2:(n1<n2?1:3);
}
static u64 __attribute__((ms_abi))
impl_CompareStringA(u32 l, u32 f, const char *s1, int n1, const char *s2, int n2)
{
    (void)l;(void)f;
    if (n1<0) n1=(int)strlen(s1);
    if (n2<0) n2=(int)strlen(s2);
    int n = n1<n2?n1:n2, r=strncmp(s1,s2,n);
    if (r<0) return 1; if (r>0) return 3;
    return n1==n2?2:(n1<n2?1:3);
}

static u64 __attribute__((ms_abi))
impl_GetEnvironmentStringsW(void) { return 0; }
static u64 __attribute__((ms_abi))
impl_FreeEnvironmentStringsW(u64 p) { (void)p; return 1; }
static u64 __attribute__((ms_abi))
impl_GetEnvironmentVariableW(u64 n, u64 b, u32 sz) { (void)n;(void)b;(void)sz; return 0; }
static u64 __attribute__((ms_abi))
impl_GetCommandLineA(void) { return (u64)"sekiro.exe"; }
static u64 __attribute__((ms_abi))
impl_GetCommandLineW(void) {
    static u16 wcmd[] = {'s','e','k','i','r','o','.','e','x','e',0};
    return (u64)wcmd;
}

static u64 __attribute__((ms_abi)) impl_ExitProcess(u32 code) {
    /* ExitProcess never returns on real Windows; a stubbed no-op return leaves
     * the guest resuming into whatever garbage RIP/RSP followed the call,
     * which manifests as an infinite masked crash-recovery loop instead of a
     * clean exit. Actually terminate so the real cause of any fatal-exit path
     * is visible instead of hidden behind that loop. (Re-confirmed live this
     * session: making this a no-op reproduces exactly the same masked
     * SteamAPI_Init()-repeats-forever loop documented previously, now caught
     * safely by the cycle detector instead of hanging -- but it's still just
     * masking, not fixing, the real early fatal-exit bug.) */
    fprintf(stderr, "[IMPL] ExitProcess(%u) -> terminating loader\n", code);
    fflush(stderr);
    _exit((int)code);
}
static u64 __attribute__((ms_abi)) impl_TerminateProcess(u64 h, u32 code)
    { (void)h; fprintf(stderr, "[IMPL] TerminateProcess(%u) -> terminating loader\n", code); fflush(stderr); _exit((int)code); }
static u64 __attribute__((ms_abi)) impl_CorExitProcess(u32 code)
    { fprintf(stderr, "[IMPL] CorExitProcess(%u) -> terminating loader\n", code); dbg_dump_guest_callchain("CorExitProcess"); fflush(stderr); _exit((int)code); }

/* ---- STARTUPINFOW ---- */
static u64 __attribute__((ms_abi))
impl_GetStartupInfoW(u8 *si)
{
    if (!si) return 0;
    memset(si, 0, 104); /* sizeof STARTUPINFOW */
    *(u32*)(si +  0) = 104; /* cb */
    *(u16*)(si + 64) = 1;   /* wShowWindow = SW_NORMAL */
    /* Standard handles: Windows pseudo-handles -10/-11/-12 */
    *(u64*)(si + 80) = (u64)(int64_t)-10;
    *(u64*)(si + 88) = (u64)(int64_t)-11;
    *(u64*)(si + 96) = (u64)(int64_t)-12;
    return 0;
}
static u64 __attribute__((ms_abi))
impl_GetStartupInfoA(u8 *si) { return impl_GetStartupInfoW(si); }

static u64 __attribute__((ms_abi))
impl_WriteFile(u64 h, const void *buf, u32 n, u32 *written, u64 ov)
{
    (void)ov;
    /* STD_OUTPUT_HANDLE=-11 (0xFFFFFFF5), STD_ERROR_HANDLE=-12 (0xFFFFFFF4) */
    int fd = -1;
    if      (h == (u64)(int64_t)-11 || h == (u64)(int64_t)-0xB) fd = STDOUT_FILENO;
    else if (h == (u64)(int64_t)-12 || h == (u64)(int64_t)-0xC) fd = STDERR_FILENO;
    else { fd = fh_get(h); }
    if (fd < 0) { if (written) *written = 0; return 0; }
    ssize_t r = write(fd, buf, n);
    if (written) *written = (u32)(r < 0 ? 0 : r);
    return r >= 0 ? 1 : 0;
}

/* ── Windows Thread Pool API (Vista+) ─────────────────────────────
 * Queried via GetProcAddress by the engine's runtime library to decide
 * which job-dispatch path to take.  Returning NULL causes the engine to
 * use an incompatible fallback that triggers the Dantelion2
 * "Transaction failed" assertion.  We back PTP_WORK with real pthreads;
 * PTP_TIMER and PTP_WAIT use simplified one-shot stubs.
 */

/* ---- PTP_WORK ---- */
typedef struct WinTPWork {
    u64             cb;       /* ms_abi PTP_WORK_CALLBACK          */
    u64             ctx;      /* void*  user context               */
    volatile int    pending;  /* in-flight submission count        */
    pthread_mutex_t mtx;
    pthread_cond_t  cv;
} WinTPWork;

static void *tp_work_thread(void *arg)
{
    WinTPWork *w = (WinTPWork *)arg;
    void *teb = alloc_teb_for_thread();
    if (teb) {
        int gs_rc = syscall(SYS_arch_prctl, ARCH_SET_GS, (u64)teb);
        if (gs_rc != 0) fprintf(stderr, "[GS] TPWork: arch_prctl failed: %d\n", gs_rc);
    }
    typedef void __attribute__((ms_abi)) (*WorkCB)(u64,u64,u64);
    ((WorkCB)w->cb)(0 /*instance*/, w->ctx, (u64)w);
    pthread_mutex_lock(&w->mtx);
    if (--w->pending == 0) pthread_cond_broadcast(&w->cv);
    pthread_mutex_unlock(&w->mtx);
    return NULL;
}

static u64 __attribute__((ms_abi))
impl_CreateThreadpoolWork(u64 cb, u64 ctx, u64 env)
{
    (void)env;
    WinTPWork *w = calloc(1, sizeof *w);
    if (!w) return 0;
    w->cb = cb; w->ctx = ctx;
    pthread_mutex_init(&w->mtx, NULL);
    pthread_cond_init(&w->cv, NULL);
    return (u64)w;
}

static u64 __attribute__((ms_abi))
impl_SubmitThreadpoolWork(WinTPWork *w)
{
    if (!w) return 0;
    pthread_mutex_lock(&w->mtx); w->pending++; pthread_mutex_unlock(&w->mtx);
    pthread_t t; pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, tp_work_thread, w) != 0) {
        pthread_mutex_lock(&w->mtx); --w->pending; pthread_mutex_unlock(&w->mtx);
    }
    pthread_attr_destroy(&a);
    return 0;
}

static u64 __attribute__((ms_abi))
impl_WaitForThreadpoolWorkCallbacks(WinTPWork *w, u32 cancel)
{
    if (!w) return 0;
    (void)cancel;
    pthread_mutex_lock(&w->mtx);
    while (w->pending > 0) pthread_cond_wait(&w->cv, &w->mtx);
    pthread_mutex_unlock(&w->mtx);
    return 0;
}

static u64 __attribute__((ms_abi))
impl_CloseThreadpoolWork(WinTPWork *w)
{
    if (!w) return 0;
    impl_WaitForThreadpoolWorkCallbacks(w, 0);
    pthread_mutex_destroy(&w->mtx);
    pthread_cond_destroy(&w->cv);
    free(w); return 0;
}

/* ---- PTP_TIMER ---- */
typedef struct WinTPTimer {
    u64             cb;
    u64             ctx;
    volatile int    active;
    volatile int    cancel;
    u64             due_100ns;  /* initial delay, 100-ns units */
    u32             period_ms;
    pthread_t       thr;
    pthread_mutex_t mtx;
    pthread_cond_t  cv;
} WinTPTimer;

static void *tp_timer_thread(void *arg)
{
    WinTPTimer *t = (WinTPTimer *)arg;
    void *teb = alloc_teb_for_thread();
    if (teb) {
        int gs_rc = syscall(SYS_arch_prctl, ARCH_SET_GS, (u64)teb);
        if (gs_rc != 0) fprintf(stderr, "[GS] TPTimer: arch_prctl failed: %d\n", gs_rc);
    }
    typedef void __attribute__((ms_abi)) (*TimerCB)(u64,u64,u64);
    pthread_mutex_lock(&t->mtx);
    while (!t->cancel) {
        u64 wait_us = t->due_100ns / 10;
        if (wait_us < 1000) wait_us = 1000;
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += (time_t)(wait_us / 1000000);
        ts.tv_nsec += (long)((wait_us % 1000000) * 1000);
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        int r = pthread_cond_timedwait(&t->cv, &t->mtx, &ts);
        if (t->cancel) break;
        if (r == ETIMEDOUT) {
            pthread_mutex_unlock(&t->mtx);
            ((TimerCB)t->cb)(0, t->ctx, (u64)t);
            pthread_mutex_lock(&t->mtx);
            if (t->period_ms == 0) break;
            t->due_100ns = (u64)t->period_ms * 10000ULL;
        }
    }
    t->active = 0;
    pthread_mutex_unlock(&t->mtx);
    return NULL;
}

static u64 __attribute__((ms_abi))
impl_CreateThreadpoolTimer(u64 cb, u64 ctx, u64 env)
{
    (void)env;
    WinTPTimer *t = calloc(1, sizeof *t);
    if (!t) return 0;
    t->cb = cb; t->ctx = ctx;
    pthread_mutex_init(&t->mtx, NULL);
    pthread_cond_init(&t->cv, NULL);
    return (u64)t;
}

static u64 __attribute__((ms_abi))
impl_SetThreadpoolTimer(WinTPTimer *t, int64_t *due, u32 period_ms, u32 window_ms)
{
    if (!t) return 0;
    (void)window_ms;
    pthread_mutex_lock(&t->mtx);
    if (!due) {
        t->cancel = 1; pthread_cond_broadcast(&t->cv);
    } else {
        int64_t dt = *due;
        if (dt <= 0) t->due_100ns = (u64)(-dt);
        else { u64 now=0; impl_GetSystemTimeAsFileTime(&now);
               t->due_100ns = ((u64)dt > now) ? (u64)dt - now : 0; }
        t->period_ms = period_ms; t->cancel = 0;
        if (!t->active) {
            t->active = 1;
            pthread_create(&t->thr, NULL, tp_timer_thread, t);
            pthread_detach(t->thr);
        } else { pthread_cond_broadcast(&t->cv); }
    }
    pthread_mutex_unlock(&t->mtx);
    return 0;
}

static u64 __attribute__((ms_abi))
impl_IsThreadpoolTimerSet(WinTPTimer *t)
    { return (t && t->active && !t->cancel) ? 1 : 0; }

static u64 __attribute__((ms_abi))
impl_WaitForThreadpoolTimerCallbacks(WinTPTimer *t, u32 cancel)
{
    if (!t) return 0;
    if (cancel) { pthread_mutex_lock(&t->mtx); t->cancel=1;
                  pthread_cond_broadcast(&t->cv); pthread_mutex_unlock(&t->mtx); }
    struct timespec ts = {0, 1000000};
    while (t->active) nanosleep(&ts, NULL);
    return 0;
}

static u64 __attribute__((ms_abi))
impl_CloseThreadpoolTimer(WinTPTimer *t)
{
    if (!t) return 0;
    impl_WaitForThreadpoolTimerCallbacks(t, 1);
    pthread_mutex_destroy(&t->mtx); pthread_cond_destroy(&t->cv);
    free(t); return 0;
}

/* ---- PTP_WAIT (one-shot: fires callback immediately when handle set) ---- */
typedef struct WinTPWait {
    u64             cb;
    u64             ctx;
    u64             handle;
    volatile int    active;
    volatile int    cancel;
    pthread_t       thr;
    pthread_mutex_t mtx;
    pthread_cond_t  cv;
} WinTPWait;

static void *tp_wait_thread(void *arg)
{
    WinTPWait *w = (WinTPWait *)arg;
    void *teb = alloc_teb_for_thread();
    if (teb) {
        int gs_rc = syscall(SYS_arch_prctl, ARCH_SET_GS, (u64)teb);
        if (gs_rc != 0) fprintf(stderr, "[GS] TPWait: arch_prctl failed: %d\n", gs_rc);
    }
    typedef void __attribute__((ms_abi)) (*WaitCB)(u64,u64,u64,u32);
    pthread_mutex_lock(&w->mtx);
    if (!w->cancel)
        ((WaitCB)w->cb)(0, w->ctx, (u64)w, 0 /*WAIT_OBJECT_0*/);
    w->active = 0;
    pthread_mutex_unlock(&w->mtx);
    return NULL;
}

static u64 __attribute__((ms_abi))
impl_CreateThreadpoolWait(u64 cb, u64 ctx, u64 env)
{
    (void)env;
    WinTPWait *w = calloc(1, sizeof *w);
    if (!w) return 0;
    w->cb = cb; w->ctx = ctx;
    pthread_mutex_init(&w->mtx, NULL);
    pthread_cond_init(&w->cv, NULL);
    return (u64)w;
}

static u64 __attribute__((ms_abi))
impl_SetThreadpoolWait(WinTPWait *w, u64 handle, u64 *timeout)
{
    if (!w) return 0;
    (void)timeout;
    pthread_mutex_lock(&w->mtx);
    if (!handle) {
        w->cancel = 1; pthread_cond_broadcast(&w->cv);
    } else {
        w->handle = handle; w->cancel = 0;
        if (!w->active) {
            w->active = 1;
            pthread_create(&w->thr, NULL, tp_wait_thread, w);
            pthread_detach(w->thr);
        } else { pthread_cond_broadcast(&w->cv); }
    }
    pthread_mutex_unlock(&w->mtx);
    return 0;
}

static u64 __attribute__((ms_abi))
impl_WaitForThreadpoolWaitCallbacks(WinTPWait *w, u32 cancel)
{
    if (!w) return 0;
    if (cancel) { pthread_mutex_lock(&w->mtx); w->cancel=1;
                  pthread_cond_broadcast(&w->cv); pthread_mutex_unlock(&w->mtx); }
    struct timespec ts = {0, 1000000};
    while (w->active) nanosleep(&ts, NULL);
    return 0;
}

static u64 __attribute__((ms_abi))
impl_CloseThreadpoolWait(WinTPWait *w)
{
    if (!w) return 0;
    impl_WaitForThreadpoolWaitCallbacks(w, 1);
    pthread_mutex_destroy(&w->mtx); pthread_cond_destroy(&w->cv);
    free(w); return 0;
}

static u64 __attribute__((ms_abi))
impl_FreeLibraryWhenCallbackReturns(u64 pci, u64 hmod)
    { (void)pci; (void)hmod; return 0; }

/* ---- CreateEventExW ---- */
static u64 __attribute__((ms_abi))
impl_CreateEventExW(u64 sa, u64 name, u32 flags, u32 access)
    { (void)sa;(void)name;(void)flags;(void)access; return 0xE0000001; }

/* ---- GetCurrentPackageId: not running in an AppX package ---- */
#define APPMODEL_ERROR_NO_PACKAGE 15700u
static u64 __attribute__((ms_abi))
impl_GetCurrentPackageId(u32 *buflen, u8 *buf)
    { (void)buf; if(buflen)*buflen=0; return APPMODEL_ERROR_NO_PACKAGE; }

/* ---- RtlCaptureStackBackTrace ---- */
static u64 __attribute__((ms_abi))
impl_RtlCaptureStackBackTrace(u32 skip, u32 count, void **frames, u32 *hash)
    { (void)skip;(void)count;(void)frames; if(hash)*hash=0; return 0; }

/* ---- CreateSymbolicLinkW ---- */
static u64 __attribute__((ms_abi))
impl_CreateSymbolicLinkW(u64 link, u64 target, u32 flags)
    { (void)link;(void)target;(void)flags; g_last_error=1; return 0; }

/* ---- GetFileInformationByHandleEx / SetFileInformationByHandle ---- */
static u64 __attribute__((ms_abi))
impl_GetFileInformationByHandleEx(u64 handle, u32 cls, void *info, u32 sz)
{
    int fd = fh_get(handle);
    if (fd < 0) { g_last_error = 6; return 0; }
    if (info) memset(info, 0, sz);
    return 1;
}
static u64 __attribute__((ms_abi))
impl_SetFileInformationByHandle(u64 handle, u32 cls, void *info, u32 sz)
    { (void)handle;(void)cls;(void)info;(void)sz; return 1; }

/* ---- CompareStringEx / GetLocaleInfoEx / LCMapStringEx ---- */
static u64 __attribute__((ms_abi))
impl_CompareStringEx(u64 locale, u32 flags, const u16 *s1, int n1,
                      const u16 *s2, int n2, u64 vers, u64 res, u64 lp)
    { (void)locale;(void)vers;(void)res;(void)lp;
      return impl_CompareStringW(0, flags, s1, n1, s2, n2); }

static u64 __attribute__((ms_abi))
impl_GetLocaleInfoEx(u64 locale, u32 lctype, u16 *data, int cchdata)
    { (void)locale; return impl_GetLocaleInfoW(0x0409, lctype, data, cchdata); }

static u64 __attribute__((ms_abi))
impl_LCMapStringEx(u64 locale, u32 flags, const u16 *src, int srclen,
                    u16 *dst, int dstlen, u64 vers, u64 res, u64 sh)
    { (void)locale;(void)vers;(void)res;(void)sh;
      return impl_LCMapStringW(0, flags, src, srclen, dst, dstlen); }

/* ---- GetCurrentDirectory / SetCurrentDirectory ---- */
static u64 __attribute__((ms_abi))
impl_GetCurrentDirectoryW(u32 nBuf, u16 *buf)
{
    char cwd[512];
    if (!getcwd(cwd, sizeof cwd)) return 0;
    int len = (int)strlen(cwd);
    if (!buf || !nBuf) return (u64)len + 1;
    if ((u32)len + 1 > nBuf) { g_last_error = 122; return (u64)len + 1; }
    for (int i = 0; i <= len; i++) buf[i] = (u8)cwd[i];
    return (u64)len;
}
static u64 __attribute__((ms_abi))
impl_GetCurrentDirectoryA(u32 nBuf, char *buf)
{
    char cwd[512];
    if (!getcwd(cwd, sizeof cwd)) return 0;
    int len = (int)strlen(cwd);
    if (!buf || !nBuf) return (u64)len + 1;
    if ((u32)len + 1 > nBuf) { g_last_error = 122; return (u64)len + 1; }
    strncpy(buf, cwd, nBuf - 1); buf[nBuf - 1] = 0;
    return (u64)len;
}
static u64 __attribute__((ms_abi))
impl_SetCurrentDirectoryW(const u16 *path)
{
    if (!path) return 0;
    char buf[512]; int i;
    for (i = 0; i < 511 && path[i]; i++) buf[i] = (char)(path[i] & 0x7f);
    buf[i] = 0;
    return chdir(buf) == 0 ? 1 : 0;
}
static u64 __attribute__((ms_abi))
impl_SetCurrentDirectoryA(const char *path)
    { return path && chdir(path) == 0 ? 1 : 0; }

/* ---- FileTimeToSystemTime ---- */
static u64 __attribute__((ms_abi))
impl_FileTimeToSystemTime(const u64 *ft, u64 *st)
{
    if (!ft || !st) return 0;
    u64 ft_val = *ft;
    time_t unix_sec = (time_t)(ft_val / 10000000ULL > 11644473600ULL
                                ? ft_val / 10000000ULL - 11644473600ULL : 0);
    struct tm *tm = gmtime(&unix_sec);
    if (!tm) return 0;
    u16 *s = (u16 *)st;
    s[0] = (u16)(tm->tm_year + 1900); s[1] = (u16)(tm->tm_mon + 1);
    s[2] = (u16)tm->tm_wday;          s[3] = (u16)tm->tm_mday;
    s[4] = (u16)tm->tm_hour;          s[5] = (u16)tm->tm_min;
    s[6] = (u16)tm->tm_sec;           s[7] = (u16)((ft_val % 10000000ULL) / 10000);
    return 1;
}

/* ---- GetSystemDefaultLangID ---- */
static u64 __attribute__((ms_abi))
impl_GetSystemDefaultLangID(void) { return 0x0409; /* en-US */ }

/* ---- CommandLineToArgvW ---- */
static u64 __attribute__((ms_abi))
impl_CommandLineToArgvW(const u16 *cmdline, int *argc)
{
    (void)cmdline;
    if (argc) *argc = 1;
    static u16  fakecmd[] = {'s','e','k','i','r','o','.','e','x','e',0};
    static u16 *fakev[2]  = {fakecmd, NULL};
    return (u64)fakev;
}

/* ---- ImmDisableIME ---- */
static u64 __attribute__((ms_abi))
impl_ImmDisableIME(u32 tid) { (void)tid; return 1; }

/* ---- Impl dispatch table ---- */
typedef struct { const char *name; ImplFn fn; } ImplEntry;

static ImplEntry g_impls[] = {
    /* Critical sections */
    {"InitializeCriticalSectionAndSpinCount", (ImplFn)impl_InitializeCriticalSectionAndSpinCount},
    {"InitializeCriticalSection",             (ImplFn)impl_InitializeCriticalSection},
    {"DeleteCriticalSection",                 (ImplFn)impl_DeleteCriticalSection},
    {"EnterCriticalSection",                  (ImplFn)impl_EnterCriticalSection},
    {"LeaveCriticalSection",                  (ImplFn)impl_LeaveCriticalSection},
    {"TryEnterCriticalSection",               (ImplFn)impl_TryEnterCriticalSection},
    /* Exception table */
    {"RtlLookupFunctionEntry",                (ImplFn)impl_RtlLookupFunctionEntry},
    {"RtlVirtualUnwind",                      (ImplFn)impl_RtlVirtualUnwind},
    {"RtlCaptureContext",                     (ImplFn)impl_RtlCaptureContext},
    /* Modules */
    {"GetModuleHandleA",                      (ImplFn)impl_GetModuleHandleA},
    {"GetModuleHandleW",                      (ImplFn)impl_GetModuleHandleW},
    {"GetModuleFileNameA",                    (ImplFn)impl_GetModuleFileNameA},
    {"GetProcAddress",                        (ImplFn)impl_GetProcAddress},
    /* Heap */
    {"GetProcessHeap",                        (ImplFn)impl_GetProcessHeap},
    {"HeapCreate",                            (ImplFn)impl_HeapCreate},
    {"HeapAlloc",                             (ImplFn)impl_HeapAlloc},
    {"HeapReAlloc",                           (ImplFn)impl_HeapReAlloc},
    {"HeapFree",                              (ImplFn)impl_HeapFree},
    {"HeapSize",                              (ImplFn)impl_HeapSize},
    /* Virtual memory */
    {"VirtualAlloc",                          (ImplFn)impl_VirtualAlloc},
    {"VirtualFree",                           (ImplFn)impl_VirtualFree},
    {"VirtualQuery",                          (ImplFn)impl_VirtualQuery},
    {"VirtualProtect",                        (ImplFn)impl_VirtualProtect},
    /* Process / thread */
    {"GetCurrentProcessId",                   (ImplFn)impl_GetCurrentProcessId},
    {"GetCurrentThreadId",                    (ImplFn)impl_GetCurrentThreadId},
    {"GetSystemTimeAsFileTime",               (ImplFn)impl_GetSystemTimeAsFileTime},
    {"QueryPerformanceCounter",               (ImplFn)impl_QueryPerformanceCounter},
    {"QueryPerformanceFrequency",             (ImplFn)impl_QueryPerformanceFrequency},
    /* Error */
    {"GetLastError",                          (ImplFn)impl_GetLastError},
    {"SetLastError",                          (ImplFn)impl_SetLastError},
    /* Misc */
    {"IsDebuggerPresent",                     (ImplFn)impl_IsDebuggerPresent},
    {"IsProcessorFeaturePresent",             (ImplFn)impl_IsProcessorFeaturePresent},
    {"SetUnhandledExceptionFilter",           (ImplFn)impl_SetUnhandledExceptionFilter},
    {"UnhandledExceptionFilter",              (ImplFn)impl_UnhandledExceptionFilter},
    /* TLS */
    {"TlsAlloc",                              (ImplFn)impl_TlsAlloc},
    {"TlsSetValue",                           (ImplFn)impl_TlsSetValue},
    {"TlsGetValue",                           (ImplFn)impl_TlsGetValue},
    {"TlsFree",                               (ImplFn)impl_TlsFree},
    /* Fiber LS */
    {"FlsAlloc",                              (ImplFn)impl_FlsAlloc},
    {"FlsSetValue",                           (ImplFn)impl_FlsSetValue},
    {"FlsGetValue",                           (ImplFn)impl_FlsGetValue},
    {"FlsFree",                               (ImplFn)impl_FlsFree},
    /* Load / free libraries */
    {"LoadLibraryA",                          (ImplFn)impl_LoadLibraryA},
    {"LoadLibraryW",                          (ImplFn)impl_LoadLibraryW},
    {"LoadLibraryExA",                        (ImplFn)impl_LoadLibraryExA},
    {"LoadLibraryExW",                        (ImplFn)impl_LoadLibraryExW},
    {"FreeLibrary",                           (ImplFn)impl_FreeLibrary},
    /* Process affinity */
    {"GetProcessAffinityMask",                (ImplFn)impl_GetProcessAffinityMask},
    {"SetProcessAffinityMask",                (ImplFn)impl_SetProcessAffinityMask},
    /* WINMM */
    {"timeGetTime",                           (ImplFn)impl_timeGetTime},
    {"timeBeginPeriod",                       (ImplFn)impl_timeBeginPeriod},
    {"timeEndPeriod",                         (ImplFn)impl_timeEndPeriod},
    /* Security */
    {"InitializeSecurityDescriptor",          (ImplFn)impl_InitializeSecurityDescriptor},
    {"SetSecurityDescriptorDacl",             (ImplFn)impl_SetSecurityDescriptorDacl},
    /* Steam */
    {"SteamAPI_Init",                         (ImplFn)impl_SteamAPI_Init},
    {"SteamAPI_RestartAppIfNecessary",        (ImplFn)impl_SteamAPI_RestartAppIfNecessary},
    {"SteamAPI_Shutdown",                     (ImplFn)impl_SteamAPI_Shutdown},
    {"SteamAPI_RunCallbacks",                 (ImplFn)impl_SteamAPI_RunCallbacks},
    {"SteamAPI_IsSteamRunning",               (ImplFn)impl_SteamAPI_IsSteamRunning},
    {"SteamInternal_CreateInterface",         (ImplFn)impl_SteamInternal_CreateInterface},
    {"SteamAPI_RegisterCallback",             (ImplFn)impl_SteamAPI_RegisterCallback},
    {"SteamAPI_UnregisterCallback",           (ImplFn)impl_SteamAPI_UnregisterCallback},
    /* Steam interface objects */
    {"SteamApps",                             (ImplFn)impl_SteamApps},
    {"SteamClient",                           (ImplFn)impl_SteamClient},
    {"SteamUtils",                            (ImplFn)impl_SteamUtils},
    {"SteamUser",                             (ImplFn)impl_SteamUser},
    {"SteamFriends",                          (ImplFn)impl_SteamFriends},
    {"SteamUserStats",                        (ImplFn)impl_SteamUserStats},
    {"SteamUGC",                              (ImplFn)impl_SteamUGC},
    {"SteamRemoteStorage",                    (ImplFn)impl_SteamRemoteStorage},
    /* DXGI */
    {"CreateDXGIFactory",                     (ImplFn)impl_CreateDXGIFactory},
    {"CreateDXGIFactory1",                    (ImplFn)impl_CreateDXGIFactory1},
    {"CreateDXGIFactory2",                    (ImplFn)impl_CreateDXGIFactory2},
    /* WS2_32 */
    {"WSAStartup",                            (ImplFn)impl_WSAStartup},
    {"WSACleanup",                            (ImplFn)impl_WSACleanup},
    {"WSAGetLastError",                       (ImplFn)impl_WSAGetLastError},
    {"WSASetLastError",                       (ImplFn)impl_WSASetLastError},
    /* NTDLL */
    {"NtQuerySystemInformation",              (ImplFn)impl_NtQuerySystemInformation},
    {"GetLogicalProcessorInformation",        (ImplFn)impl_GetLogicalProcessorInformation},
    /* Time */
    {"GetLocalTime",                          (ImplFn)impl_GetLocalTime},
    {"GetSystemTime",                         (ImplFn)impl_GetSystemTime},
    {"SystemTimeToFileTime",                  (ImplFn)impl_SystemTimeToFileTime},
    /* Drive */
    {"GetDriveTypeW",                         (ImplFn)impl_GetDriveTypeW},
    {"GetDriveTypeA",                         (ImplFn)impl_GetDriveTypeA},
    /* User info */
    {"GetUserNameW",                          (ImplFn)impl_GetUserNameW},
    {"GetUserNameA",                          (ImplFn)impl_GetUserNameA},
    /* Shell */
    {"SHGetFolderPathW",                      (ImplFn)impl_SHGetFolderPathW},
    {"SHGetFolderPathA",                      (ImplFn)impl_SHGetFolderPathA},
    {"SHGetKnownFolderPath",                  (ImplFn)impl_SHGetKnownFolderPath},
    {"CoTaskMemFree",                         (ImplFn)impl_CoTaskMemFree},
    /* COM */
    {"CoInitialize",                          (ImplFn)impl_CoInitialize},
    {"CoInitializeEx",                        (ImplFn)impl_CoInitializeEx},
    {"CoUninitialize",                        (ImplFn)impl_CoUninitialize},
    {"CoCreateInstance",                      (ImplFn)impl_CoCreateInstance},
    /* D3D11 */
    {"D3D11CreateDevice",                     (ImplFn)impl_D3D11CreateDevice},
    {"D3D11CreateDeviceAndSwapChain",         (ImplFn)impl_D3D11CreateDeviceAndSwapChain},
    /* String / locale */
    {"MultiByteToWideChar",                   (ImplFn)impl_MultiByteToWideChar},
    {"WideCharToMultiByte",                   (ImplFn)impl_WideCharToMultiByte},
    /* Process */
    {"GetCurrentProcess",                     (ImplFn)impl_GetCurrentProcess},
    {"ExitProcess",                           (ImplFn)impl_ExitProcess},
    {"CorExitProcess",                        (ImplFn)impl_CorExitProcess},
    {"TerminateProcess",                      (ImplFn)impl_TerminateProcess},
    {"GetSystemInfo",                         (ImplFn)impl_GetSystemInfo},
    {"GetVersion",                            (ImplFn)impl_GetVersion},
    {"GetCommandLineA",                       (ImplFn)impl_GetCommandLineA},
    {"GetCommandLineW",                       (ImplFn)impl_GetCommandLineW},
    {"GetStartupInfoW",                       (ImplFn)impl_GetStartupInfoW},
    {"GetStartupInfoA",                       (ImplFn)impl_GetStartupInfoA},
    {"GetEnvironmentStringsW",                (ImplFn)impl_GetEnvironmentStringsW},
    {"FreeEnvironmentStringsW",               (ImplFn)impl_FreeEnvironmentStringsW},
    {"GetEnvironmentVariableW",               (ImplFn)impl_GetEnvironmentVariableW},
    /* File I/O */
    {"WriteFile",                             (ImplFn)impl_WriteFile},
    {"GetStdHandle",                          (ImplFn)impl_GetStdHandle},
    /* Code pages */
    {"GetACP",                                (ImplFn)impl_GetACP},
    {"GetOEMCP",                              (ImplFn)impl_GetOEMCP},
    {"GetCPInfo",                             (ImplFn)impl_GetCPInfo},
    {"IsValidCodePage",                       (ImplFn)impl_IsValidCodePage},
    /* Module paths */
    {"GetModuleFileNameW",                    (ImplFn)impl_GetModuleFileNameW},
    /* SLIST */
    {"InitializeSListHead",                   (ImplFn)impl_InitializeSListHead},
    {"InterlockedFlushSList",                 (ImplFn)impl_InterlockedFlushSList},
    /* Interlocked */
    {"InterlockedIncrement",                  (ImplFn)impl_InterlockedIncrement},
    {"InterlockedDecrement",                  (ImplFn)impl_InterlockedDecrement},
    {"InterlockedExchange",                   (ImplFn)impl_InterlockedExchange},
    {"InterlockedCompareExchange",            (ImplFn)impl_InterlockedCompareExchange},
    {"InterlockedCompareExchange64",          (ImplFn)impl_InterlockedCompareExchange64},
    /* Events */
    {"CreateEventA",                          (ImplFn)impl_CreateEventA},
    {"CreateEventW",                          (ImplFn)impl_CreateEventW},
    {"WaitForSingleObject",                   (ImplFn)impl_WaitForSingleObject},
    {"WaitForMultipleObjects",                (ImplFn)impl_WaitForMultipleObjects},
    {"SetEvent",                              (ImplFn)impl_SetEvent},
    {"ResetEvent",                            (ImplFn)impl_ResetEvent},
    /* Mutex */
    {"CreateMutexA",                          (ImplFn)impl_CreateMutexA},
    {"CreateMutexW",                          (ImplFn)impl_CreateMutexW},
    {"ReleaseMutex",                          (ImplFn)impl_ReleaseMutex},
    {"OpenMutexA",                            (ImplFn)impl_OpenMutexA},
    /* Sleep / timing */
    {"Sleep",                                 (ImplFn)impl_Sleep},
    {"GetTickCount",                          (ImplFn)impl_GetTickCount},
    {"GetTickCount64",                        (ImplFn)impl_GetTickCount64},
    {"GetFileType",                           (ImplFn)impl_GetFileType},
    {"SetStdHandle",                          (ImplFn)impl_SetStdHandle},
    /* Locale / string */
    {"GetStringTypeW",                        (ImplFn)impl_GetStringTypeW},
    {"GetStringTypeA",                        (ImplFn)impl_GetStringTypeA},
    {"LCMapStringW",                          (ImplFn)impl_LCMapStringW},
    {"LCMapStringA",                          (ImplFn)impl_LCMapStringA},
    {"GetLocaleInfoW",                        (ImplFn)impl_GetLocaleInfoW},
    {"GetUserDefaultLCID",                    (ImplFn)impl_GetUserDefaultLCID},
    {"GetSystemDefaultLCID",                  (ImplFn)impl_GetSystemDefaultLCID},
    {"GetUserDefaultUILanguage",              (ImplFn)impl_GetUserDefaultUILanguage},
    /* Format */
    {"FormatMessageA",                        (ImplFn)impl_FormatMessageA},
    {"FormatMessageW",                        (ImplFn)impl_FormatMessageW},
    /* Window placeholders */
    {"RegisterClassExA",                      (ImplFn)impl_RegisterClassExA},
    {"RegisterClassExW",                      (ImplFn)impl_RegisterClassExW},
    {"GetSystemMetrics",                      (ImplFn)impl_GetSystemMetrics},
    {"GetDesktopWindow",                      (ImplFn)impl_GetDesktopWindow},
    {"ShowWindow",                            (ImplFn)impl_ShowWindow},
    {"UpdateWindow",                          (ImplFn)impl_UpdateWindow},
    {"GetModuleHandleExW",                    (ImplFn)impl_GetModuleHandleExW},
    {"GetModuleHandleExA",                    (ImplFn)impl_GetModuleHandleExA},
    /* Condition variables */
    {"InitializeConditionVariable",           (ImplFn)impl_InitializeConditionVariable},
    {"WakeConditionVariable",                 (ImplFn)impl_WakeConditionVariable},
    {"WakeAllConditionVariable",              (ImplFn)impl_WakeAllConditionVariable},
    {"SleepConditionVariableCS",              (ImplFn)impl_SleepConditionVariableCS},
    {"SleepConditionVariableSRW",             (ImplFn)impl_SleepConditionVariableSRW},
    /* SRWLock */
    {"InitializeSRWLock",                     (ImplFn)impl_InitializeSRWLock},
    {"AcquireSRWLockExclusive",               (ImplFn)impl_AcquireSRWLockExclusive},
    {"TryAcquireSRWLockExclusive",            (ImplFn)impl_TryAcquireSRWLockExclusive},
    {"ReleaseSRWLockExclusive",               (ImplFn)impl_ReleaseSRWLockExclusive},
    {"AcquireSRWLockShared",                  (ImplFn)impl_AcquireSRWLockShared},
    {"TryAcquireSRWLockShared",               (ImplFn)impl_TryAcquireSRWLockShared},
    {"ReleaseSRWLockShared",                  (ImplFn)impl_ReleaseSRWLockShared},
    /* Extended CS / one-time init */
    {"InitializeCriticalSectionEx",           (ImplFn)impl_InitializeCriticalSectionEx},
    {"InitOnceExecuteOnce",                   (ImplFn)impl_InitOnceExecuteOnce},
    /* Threads */
    {"CreateThread",                          (ImplFn)impl_CreateThread},
    {"GetCurrentThread",                      (ImplFn)impl_GetCurrentThread},
    {"SuspendThread",                         (ImplFn)impl_SuspendThread},
    {"ResumeThread",                          (ImplFn)impl_ResumeThread},
    {"SetThreadPriority",                     (ImplFn)impl_SetThreadPriority},
    {"ExitThread",                            (ImplFn)impl_ExitThread},
    {"FreeLibraryAndExitThread",              (ImplFn)impl_FreeLibraryAndExitThread},
    {"TerminateThread",                       (ImplFn)impl_TerminateThread},
    {"OpenThread",                            (ImplFn)impl_OpenThread},
    /* Semaphores */
    {"CreateSemaphoreW",                      (ImplFn)impl_CreateSemaphoreW},
    {"CreateSemaphoreA",                      (ImplFn)impl_CreateSemaphoreA},
    {"OpenSemaphoreW",                        (ImplFn)impl_OpenSemaphoreW},
    /* Debug */
    {"OutputDebugStringA",                    (ImplFn)impl_OutputDebugStringA},
    {"OutputDebugStringW",                    (ImplFn)impl_OutputDebugStringW},
    /* File system */
    {"GetTempPathW",                          (ImplFn)impl_GetTempPathW},
    {"GetTempPathA",                          (ImplFn)impl_GetTempPathA},
    {"GetFullPathNameW",                      (ImplFn)impl_GetFullPathNameW},
    {"GetFullPathNameA",                      (ImplFn)impl_GetFullPathNameA},
    {"CreateFileA",                           (ImplFn)impl_CreateFileA},
    {"CreateFileW",                           (ImplFn)impl_CreateFileW},
    {"CloseHandle",                           (ImplFn)impl_CloseHandle},
    {"ReadFile",                              (ImplFn)impl_ReadFile},
    {"WriteFile",                             (ImplFn)impl_WriteFile},
    {"GetFileSize",                           (ImplFn)impl_GetFileSize},
    {"SetFilePointer",                        (ImplFn)impl_SetFilePointer},
    {"GetFileAttributesA",                    (ImplFn)impl_GetFileAttributesA},
    {"GetFileAttributesW",                    (ImplFn)impl_GetFileAttributesW},
    {"SetFileAttributesA",                    (ImplFn)impl_SetFileAttributesA},
    {"SetFileAttributesW",                    (ImplFn)impl_SetFileAttributesW},
    /* Thread affinity */
    {"SetThreadAffinityMask",                 (ImplFn)impl_SetThreadAffinityMask},
    /* Memory */
    {"LocalAlloc",                            (ImplFn)impl_LocalAlloc},
    {"LocalFree",                             (ImplFn)impl_LocalFree},
    {"GlobalAlloc",                           (ImplFn)impl_GlobalAlloc},
    {"GlobalFree",                            (ImplFn)impl_GlobalFree},
    /* Error mode */
    {"SetErrorMode",                          (ImplFn)impl_SetErrorMode},
    /* USER32 window system */
    {"GetWindowRect",                         (ImplFn)impl_GetWindowRect},
    {"GetClientRect",                         (ImplFn)impl_GetClientRect},
    {"LoadIconW",                             (ImplFn)impl_LoadIconW},
    {"LoadIconA",                             (ImplFn)impl_LoadIconA},
    {"LoadCursorW",                           (ImplFn)impl_LoadCursorW},
    {"LoadCursorA",                           (ImplFn)impl_LoadCursorA},
    {"AdjustWindowRect",                      (ImplFn)impl_AdjustWindowRect},
    {"AdjustWindowRectEx",                    (ImplFn)impl_AdjustWindowRectEx},
    {"CreateWindowExW",                       (ImplFn)impl_CreateWindowExW},
    {"CreateWindowExA",                       (ImplFn)impl_CreateWindowExA},
    {"DestroyWindow",                         (ImplFn)impl_DestroyWindow},
    {"IsWindow",                              (ImplFn)impl_IsWindow},
    {"SetWindowTextW",                        (ImplFn)impl_SetWindowTextW},
    {"SetWindowTextA",                        (ImplFn)impl_SetWindowTextA},
    {"MoveWindow",                            (ImplFn)impl_MoveWindow},
    {"SetWindowPos",                          (ImplFn)impl_SetWindowPos},
    {"GetDC",                                 (ImplFn)impl_GetDC},
    {"ReleaseDC",                             (ImplFn)impl_ReleaseDC},
    {"SetCursor",                             (ImplFn)impl_SetCursor},
    {"ShowCursor",                            (ImplFn)impl_ShowCursor},
    {"SetCapture",                            (ImplFn)impl_SetCapture},
    {"ReleaseCapture",                        (ImplFn)impl_ReleaseCapture},
    {"SetFocus",                              (ImplFn)impl_SetFocus},
    {"GetFocus",                              (ImplFn)impl_GetFocus},
    {"GetActiveWindow",                       (ImplFn)impl_GetActiveWindow},
    {"PeekMessageW",                          (ImplFn)impl_PeekMessageW},
    {"GetMessageW",                           (ImplFn)impl_GetMessageW},
    {"TranslateMessage",                      (ImplFn)impl_TranslateMessage},
    {"DispatchMessageW",                      (ImplFn)impl_DispatchMessageW},
    {"PostQuitMessage",                       (ImplFn)impl_PostQuitMessage},
    {"SendMessageW",                          (ImplFn)impl_SendMessageW},
    {"PostMessageW",                          (ImplFn)impl_PostMessageW},
    {"DefWindowProcW",                        (ImplFn)impl_DefWindowProcW},
    {"CallWindowProcW",                       (ImplFn)impl_CallWindowProcW},
    {"SetWindowLongPtrW",                     (ImplFn)impl_SetWindowLongPtrW},
    {"GetWindowLongPtrW",                     (ImplFn)impl_GetWindowLongPtrW},
    {"SetWindowLongW",                        (ImplFn)impl_SetWindowLongW},
    {"GetWindowLongW",                        (ImplFn)impl_GetWindowLongW},
    {"SetForegroundWindow",                   (ImplFn)impl_SetForegroundWindow},
    {"SetWindowsHookExW",                     (ImplFn)impl_SetWindowsHookExW},
    {"UnhookWindowsHookEx",                   (ImplFn)impl_UnhookWindowsHookEx},
    {"CallNextHookEx",                        (ImplFn)impl_CallNextHookEx},
    {"GetSystemMetrics",                      (ImplFn)impl_GetSystemMetrics},
    {"MonitorFromWindow",                     (ImplFn)impl_MonitorFromWindow},
    {"GetMonitorInfoW",                       (ImplFn)impl_GetMonitorInfoW},
    /* Misc sync */
    {"GetCurrentProcessorNumber",             (ImplFn)impl_GetCurrentProcessorNumber},
    {"FlushProcessWriteBuffers",              (ImplFn)impl_FlushProcessWriteBuffers},
    /* Pointer encoding */
    {"EncodePointer",                         (ImplFn)impl_EncodePointer},
    {"DecodePointer",                         (ImplFn)impl_DecodePointer},
    {"EncodeSystemPointer",                   (ImplFn)impl_EncodeSystemPointer},
    {"DecodeSystemPointer",                   (ImplFn)impl_DecodeSystemPointer},
    /* Thread/process times */
    {"GetThreadTimes",                        (ImplFn)impl_GetThreadTimes},
    {"GetProcessTimes",                       (ImplFn)impl_GetProcessTimes},
    {"GetSystemTimePreciseAsFileTime",        (ImplFn)impl_GetSystemTimePreciseAsFileTime},
    {"CreateSemaphoreExW",                    (ImplFn)impl_CreateSemaphoreExW},
    {"ReleaseSemaphore",                      (ImplFn)impl_ReleaseSemaphore},
    /* Exceptions */
    {"RaiseException",                        (ImplFn)impl_RaiseException},
    {"RtlPcToFileHeader",                     (ImplFn)impl_RtlPcToFileHeader},
    /* Locale */
    {"EnumSystemLocalesW",                    (ImplFn)impl_EnumSystemLocalesW},
    {"IsValidLocale",                         (ImplFn)impl_IsValidLocale},
    {"GetLocaleInfoA",                        (ImplFn)impl_GetLocaleInfoA},
    {"GetUserDefaultLocaleName",              (ImplFn)impl_GetUserDefaultLocaleName},
    {"CompareStringW",                        (ImplFn)impl_CompareStringW},
    {"CompareStringA",                        (ImplFn)impl_CompareStringA},
    /* Thread Pool API (Vista+) — required by Dantelion2 job system */
    {"CreateThreadpoolWork",                  (ImplFn)impl_CreateThreadpoolWork},
    {"SubmitThreadpoolWork",                  (ImplFn)impl_SubmitThreadpoolWork},
    {"WaitForThreadpoolWorkCallbacks",         (ImplFn)impl_WaitForThreadpoolWorkCallbacks},
    {"CloseThreadpoolWork",                   (ImplFn)impl_CloseThreadpoolWork},
    {"CreateThreadpoolTimer",                 (ImplFn)impl_CreateThreadpoolTimer},
    {"SetThreadpoolTimer",                    (ImplFn)impl_SetThreadpoolTimer},
    {"IsThreadpoolTimerSet",                  (ImplFn)impl_IsThreadpoolTimerSet},
    {"WaitForThreadpoolTimerCallbacks",        (ImplFn)impl_WaitForThreadpoolTimerCallbacks},
    {"CloseThreadpoolTimer",                  (ImplFn)impl_CloseThreadpoolTimer},
    {"CreateThreadpoolWait",                  (ImplFn)impl_CreateThreadpoolWait},
    {"SetThreadpoolWait",                     (ImplFn)impl_SetThreadpoolWait},
    {"WaitForThreadpoolWaitCallbacks",         (ImplFn)impl_WaitForThreadpoolWaitCallbacks},
    {"CloseThreadpoolWait",                   (ImplFn)impl_CloseThreadpoolWait},
    {"FreeLibraryWhenCallbackReturns",        (ImplFn)impl_FreeLibraryWhenCallbackReturns},
    /* Events (extended) */
    {"CreateEventExW",                        (ImplFn)impl_CreateEventExW},
    /* Package identity */
    {"GetCurrentPackageId",                   (ImplFn)impl_GetCurrentPackageId},
    /* Stack trace */
    {"RtlCaptureStackBackTrace",              (ImplFn)impl_RtlCaptureStackBackTrace},
    /* Symbolic links */
    {"CreateSymbolicLinkW",                   (ImplFn)impl_CreateSymbolicLinkW},
    /* File info by handle */
    {"GetFileInformationByHandleEx",          (ImplFn)impl_GetFileInformationByHandleEx},
    {"SetFileInformationByHandle",            (ImplFn)impl_SetFileInformationByHandle},
    /* Extended locale functions */
    {"CompareStringEx",                       (ImplFn)impl_CompareStringEx},
    {"GetLocaleInfoEx",                       (ImplFn)impl_GetLocaleInfoEx},
    {"LCMapStringEx",                         (ImplFn)impl_LCMapStringEx},
    /* Directory */
    {"GetCurrentDirectoryW",                  (ImplFn)impl_GetCurrentDirectoryW},
    {"GetCurrentDirectoryA",                  (ImplFn)impl_GetCurrentDirectoryA},
    {"SetCurrentDirectoryW",                  (ImplFn)impl_SetCurrentDirectoryW},
    {"SetCurrentDirectoryA",                  (ImplFn)impl_SetCurrentDirectoryA},
    /* Time conversion */
    {"FileTimeToSystemTime",                  (ImplFn)impl_FileTimeToSystemTime},
    /* Locale */
    {"GetSystemDefaultLangID",                (ImplFn)impl_GetSystemDefaultLangID},
    /* CommandLine */
    {"CommandLineToArgvW",                    (ImplFn)impl_CommandLineToArgvW},
    /* IME */
    {"ImmDisableIME",                         (ImplFn)impl_ImmDisableIME},
    {NULL, NULL}
};

static ImplFn find_impl(const char *fn_name)
{
    for (int i = 0; g_impls[i].name; i++)
        if (strcmp(g_impls[i].name, fn_name) == 0)
            return g_impls[i].fn;
    return NULL;
}

/* ── Utilities ──────────────────────────────────────────────────── */
static __attribute__((noreturn, format(printf, 1, 2)))
void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static inline void *rva_ptr(u32 off) { return (void *)(g_img + off); }

/* ── PE loader ──────────────────────────────────────────────────── */
static void pe_load(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); die("Cannot open executable"); }

    struct stat st;
    if (fstat(fd, &st) < 0) die("fstat: %s", strerror(errno));

    u8 *file = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (file == MAP_FAILED) die("mmap(file): %s", strerror(errno));

    /* ── validate ── */
    DosHdr   *dos = (DosHdr *)file;
    if (dos->magic != MZ_MAGIC) die("Not a PE file (bad MZ)");

    NtHdrs64 *nt  = (NtHdrs64 *)(file + dos->lfanew);
    if (nt->sig           != PE_SIG)    die("Bad PE signature");
    if (nt->opt.magic     != PE32PLUS)  die("Not PE32+ (64-bit); magic=0x%04x", nt->opt.magic);

    u64 preferred = nt->opt.imagebase;
    u32 imgsz     = nt->opt.sz_image;

    printf("[PE]  %s\n", path);
    printf("[PE]  ImageBase=0x%lx  SzImage=0x%x  EntryRVA=+0x%x  Sections=%u\n",
           preferred, imgsz, nt->opt.entry_rva, nt->file.nsections);

    /* ── map image memory ── */
    g_img = mmap((void *)preferred, imgsz,
                 PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                 -1, 0);
    if (g_img == MAP_FAILED) {
        /* preferred address busy – try anywhere */
        g_img = mmap(NULL, imgsz, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (g_img == MAP_FAILED) die("mmap(image): %s", strerror(errno));
        printf("[PE]  Preferred base busy; loaded at 0x%lx (need relocs)\n", (u64)g_img);
    } else {
        printf("[PE]  Mapped at preferred base 0x%lx\n", (u64)g_img);
    }
    g_delta = (u64)g_img - preferred;

    /* ── copy headers ── */
    memcpy(g_img, file, nt->opt.sz_headers);

    /* ── copy sections ── */
    SecHdr *secs = (SecHdr *)((u8 *)&nt->opt + nt->file.opthdr_sz);
    for (u16 i = 0; i < nt->file.nsections; i++) {
        SecHdr *s = &secs[i];
        printf("[SEC] %-8.8s  +0x%06x  vsz=0x%05x  raw=0x%05x  chars=0x%08x\n",
               s->name, s->vrva, s->vsz, s->raw_sz, s->chars);
        if (s->raw_sz > 0 && s->raw_off > 0) {
            u32 copy = (s->raw_sz < s->vsz) ? s->raw_sz : s->vsz;
            memcpy(g_img + s->vrva, file + s->raw_off, copy);
        }
        /* remainder is already zero (anonymous mmap) */
    }

    munmap(file, (size_t)st.st_size);

    /* ── base relocations (if loaded at non-preferred address) ── */
    if (g_delta) {
        /* re-derive nt from g_img since file is unmapped */
        NtHdrs64 *nt2 = (NtHdrs64 *)(g_img + ((DosHdr *)g_img)->lfanew);
        DataDir  *rd  = &nt2->opt.dirs[DIR_RELOC];
        if (rd->size) {
            u8  *p = g_img + rd->rva;
            u8  *end = p + rd->size;
            u32  npatched = 0;
            while (p < end) {
                RelocBlock *blk = (RelocBlock *)p;
                if (blk->block_sz < 8) break;
                u16 *ents = (u16 *)(blk + 1);
                int  n    = (int)((blk->block_sz - 8) / 2);
                for (int j = 0; j < n; j++) {
                    if ((ents[j] >> 12) == 10) { /* IMAGE_REL_BASED_DIR64 */
                        u64 *addr = (u64 *)(g_img + blk->page_rva + (ents[j] & 0xFFF));
                        *addr += g_delta;
                        npatched++;
                    }
                }
                p += blk->block_sz;
            }
            printf("[PE]  %u reloc entries patched (delta=+0x%lx)\n", npatched, g_delta);
        } else {
            fprintf(stderr, "[WARN] No reloc table — image may malfunction at non-preferred base\n");
        }
    }
}

/* ── Import resolution ──────────────────────────────────────────── */
static void pe_imports(void)
{
    NtHdrs64  *nt  = (NtHdrs64 *)(g_img + ((DosHdr *)g_img)->lfanew);
    DataDir   *dir = &nt->opt.dirs[DIR_IMPORT];
    if (!dir->size) { puts("[IMP] No import table."); return; }

    /* count total imports to size the trampoline page */
    int total = 0;
    for (ImportDesc *d = rva_ptr(dir->rva); d->name_rva; d++) {
        u64 *ilt = rva_ptr(d->orig_ilt ? d->orig_ilt : d->iat_rva);
        while (*ilt++) total++;
    }
    printf("[IMP] %d imports total\n", total);

    g_trampsz = (((size_t)(total + 64) * THUNK_SZ) + 0xFFFu) & ~(size_t)0xFFF;
    g_tramp = mmap(NULL, g_trampsz,
                   PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_tramp == MAP_FAILED) die("mmap(trampoline): %s", strerror(errno));

    for (ImportDesc *d = rva_ptr(dir->rva); d->name_rva; d++) {
        const char *dll = rva_ptr(d->name_rva);
        if (!dll || !dll[0]) {
            fprintf(stderr, "[IMP] Warning: empty/null DLL name at desc offset 0x%lx, "
                    "name_rva=0x%x, iat_rva=0x%x\n",
                    (u64)((u8*)d - g_img), d->name_rva, d->iat_rva);
            break;
        }
        u64 *ilt = rva_ptr(d->orig_ilt ? d->orig_ilt : d->iat_rva);
        u64 *iat = rva_ptr(d->iat_rva);
        printf("[DLL] %s\n", dll);
        fflush(stdout);

        for (int i = 0; ilt[i]; i++) {
            if (g_nstubs >= MAX_STUBS) die("Import count exceeds MAX_STUBS (%d)", MAX_STUBS);

            char buf[288];
            const char *fn_only = NULL;
            if (ilt[i] & (1ULL << 63)) {
                /* import by ordinal */
                snprintf(buf, sizeof(buf), "%s!#%u", dll, (u16)(ilt[i] & 0xFFFF));
                fn_only = buf; /* ordinals can't match by name */
            } else {
                /* import by name – IMAGE_IMPORT_BY_NAME: 2-byte hint then ASCII */
                fn_only = (const char *)rva_ptr((u32)ilt[i]) + 2;
                snprintf(buf, sizeof(buf), "%s!%s", dll, fn_only);
            }

            /* Use real implementation if available, else logging stub */
            ImplFn real = fn_only ? find_impl(fn_only) : NULL;
            if (real) {
                g_snames[g_nstubs] = strdup(buf);
                emit_impl_thunk(g_nstubs, (u64)real);
                iat[i] = (u64)(g_tramp + g_nstubs * THUNK_SZ);
                printf("  [REAL] %s\n", buf);
            } else {
                g_snames[g_nstubs] = strdup(buf);
                emit_thunk(g_nstubs);
                iat[i] = (u64)(g_tramp + g_nstubs * THUNK_SZ);
            }
            g_nstubs++;
        }
    }
    int nreal = 0;
    for (int i = 0; i < g_nstubs; i++) {
        const char *bang = strchr(g_snames[i], '!');
        const char *fn = bang ? bang + 1 : g_snames[i];
        if (find_impl(fn)) nreal++;
    }
    printf("[IMP] %d stubs total  (%d real implementations, %d stubs -> 0)\n",
           g_nstubs, nreal, g_nstubs - nreal);
}

typedef struct {
    u8  flags[4];               /* +0x000 */
    u8  _pad0[4];
    u64 Mutant;                 /* +0x008 */
    u64 ImageBaseAddress;       /* +0x010 */
    u64 Ldr;                    /* +0x018  (NULL – will cause crashes if walked) */
    u8  _pad1[0x1000 - 0x20];
} WinPEB;

/* ── IMAGE_TLS_DIRECTORY64 (shared type used by both helpers) ───── */
typedef struct {
    u64  StartAddr;       /* VA of raw tls data start  */
    u64  EndAddr;         /* VA of raw tls data end    */
    u64 *AddrOfIndex;     /* VA of DWORD tls_index var */
    u64  AddrOfCallbacks; /* VA of callback array      */
    u32  SizeOfZeroFill;
    u32  Characteristics;
} TLS_DIR64;

#define TLS_SLOTS 128

/*
 * alloc_teb_for_thread — allocate a private WinTEB with its own TLS array
 * and implicit-TLS data block for any thread (main or worker).
 *
 * Does NOT set up StackBase/StackLimit/ProcEnvBlk — callers that need
 * those (i.e. setup_teb_peb for the main thread) fill them in afterwards.
 * Does NOT call arch_prctl — caller does that.
 */
static void *alloc_teb_for_thread(void)
{
    /* Allocate per-thread host call stack first, capturing bounds locally */
    void *host_stack = mmap(NULL, HOST_CALL_STACK_SIZE, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (host_stack == MAP_FAILED) { perror("mmap(host_call_stack)"); _exit(2); }
    u64 host_stack_base = (u64)host_stack;
    u64 host_stack_top = ((u64)host_stack + HOST_CALL_STACK_SIZE - 256) & ~0xFULL;

    WinTEB *teb = mmap(NULL, sizeof(WinTEB), PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (teb == MAP_FAILED) die("mmap(worker TEB): %s", strerror(errno));
    memset(teb, 0, sizeof(*teb));
    teb->Self = (u64)teb;

    /* Populate the host call stack TEB fields with THIS thread's stack bounds.
     * This must happen BEFORE we populate the global variables, to avoid
     * race conditions when multiple threads are created concurrently. */
    teb->HostCallStackBase = host_stack_base;
    teb->HostCallStackTop = host_stack_top;

    /* Also update the globals for backward compatibility.
     * Note: This is racy if multiple threads call concurrently, but that's
     * okay since each thread has its own TEB fields now. */
    g_host_call_stack_base = host_stack_base;
    g_host_call_stack_top = host_stack_top;

    /* Per-thread TLS slot array (gs:[0x58]) */
    u64 *tls_array = mmap(NULL, (size_t)TLS_SLOTS * sizeof(u64),
                          PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tls_array == MAP_FAILED) die("mmap(worker tls_array): %s", strerror(errno));
    memset(tls_array, 0, (size_t)TLS_SLOTS * sizeof(u64));

    /* Copy the module's implicit TLS initialiser data for this thread */
    NtHdrs64 *nt = (NtHdrs64 *)(g_img + ((DosHdr *)g_img)->lfanew);
    DataDir  *tls_dir = &nt->opt.dirs[9]; /* IMAGE_DIRECTORY_ENTRY_TLS */
    if (tls_dir->size) {
        TLS_DIR64 *td = (TLS_DIR64 *)(g_img + tls_dir->rva);
        u64 raw_sz   = td->EndAddr - td->StartAddr;
        u64 total_sz = raw_sz + td->SizeOfZeroFill;
        /* tls_index was fixed to 0 by setup_teb_peb; use AddrOfIndex if live */
        u64 tls_index = (td->AddrOfIndex && *td->AddrOfIndex < (u64)TLS_SLOTS)
                        ? *td->AddrOfIndex : 0;
        if (total_sz > 0 && tls_index < (u64)TLS_SLOTS) {
            u8 *tls_data = mmap(NULL, total_sz, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (tls_data != MAP_FAILED) {
                memset(tls_data, 0, total_sz);
                if (raw_sz > 0)
                    memcpy(tls_data, (void *)td->StartAddr, (size_t)raw_sz);
                tls_array[tls_index] = (u64)tls_data;
            }
        }
    }

    /* Write the TLS array pointer into TEB at offset 0x58
     * (ThreadLocalStoragePointer) */
    *(u64 *)((u8 *)teb + 0x58) = (u64)tls_array;

    /*
     * Fill StackBase/StackLimit with the bounds of the REAL native stack
     * this thread is actually executing on (queried via pthread), not a
     * synthetic region we never switch RSP onto. Previously this was left
     * unset here (all zero) for worker/threadpool threads, and the main
     * thread's caller (setup_teb_peb) pointed it at an mmap'd 8 MiB region
     * that the main thread's RSP never actually ran on. Either way, any
     * guest code reading gs:[0x08]/gs:[0x10] for stack-bounds checks
     * (stack probes, exception unwinding, fiber setup, etc.) got numbers
     * describing a stack that didn't match the live RSP range at all —
     * a very plausible source of the sporadic worker-thread null derefs
     * seen throughout this project. Fix: always describe the real stack.
     */
    {
        pthread_attr_t attr;
        void  *stackaddr = NULL;
        size_t stacksize = 0;
        if (pthread_getattr_np(pthread_self(), &attr) == 0) {
            if (pthread_attr_getstack(&attr, &stackaddr, &stacksize) != 0) {
                stackaddr = NULL;
                stacksize = 0;
            }
            pthread_attr_destroy(&attr);
        }
        if (stackaddr && stacksize) {
            teb->StackBase  = (u64)stackaddr + (u64)stacksize; /* high addr */
            teb->StackLimit = (u64)stackaddr;                  /* low addr  */
        }
    }
    return (void *)teb;
}

static void set_teb_stack_bounds(u64 stack_low, u64 stack_high)
{
    u64 gs_base = 0;
    if (syscall(SYS_arch_prctl, ARCH_GET_GS, &gs_base) != 0 || !gs_base)
        return;
    WinTEB *teb = (WinTEB *)gs_base;
    teb->StackLimit = stack_low;
    teb->StackBase  = stack_high;
}

static void setup_teb_peb(void)
{
    WinTEB *teb = (WinTEB *)alloc_teb_for_thread();

    WinPEB *peb = mmap(NULL, sizeof(WinPEB), PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (peb == MAP_FAILED) die("mmap(PEB): %s", strerror(errno));
    memset(peb, 0, sizeof(*peb));

    /* StackBase/StackLimit are already filled in by alloc_teb_for_thread()
     * with this (main) thread's real stack bounds — do NOT override them
     * with a synthetic mmap'd stack we never switch RSP onto. */
    teb->ProcEnvBlk = (u64)peb;

    peb->ImageBaseAddress = (u64)g_img;

    /* Zero the TLS index variable in the module (shared; done once here) */
    NtHdrs64 *nt2    = (NtHdrs64 *)(g_img + ((DosHdr *)g_img)->lfanew);
    DataDir  *tls_dir = &nt2->opt.dirs[9];
    if (tls_dir->size) {
        TLS_DIR64 *td = (TLS_DIR64 *)(g_img + tls_dir->rva);
        u64 raw_sz    = td->EndAddr - td->StartAddr;
        if (td->AddrOfIndex) *td->AddrOfIndex = 0;
        printf("[TLS] tls_dir RVA=0x%x raw=%lu zero=%u idx=0 (fixed)\n",
               tls_dir->rva, (unsigned long)raw_sz, td->SizeOfZeroFill);
    }

    u64 *tls_array = *(u64 **)((u8 *)teb + 0x58);

    if (syscall(SYS_arch_prctl, ARCH_SET_GS, (u64)teb) != 0) {
        perror("arch_prctl(ARCH_SET_GS)");
        die("Cannot set GS register (need Linux >= 3.1)");
    }

    printf("[TEB] teb=0x%lx  peb=0x%lx  stack=0x%lx..0x%lx  tls_arr=0x%lx\n",
           (u64)teb, (u64)peb, (u64)teb->StackLimit, (u64)teb->StackBase, (u64)tls_array);
}

/* ── Section permissions ────────────────────────────────────────── */
static void apply_section_perms(void)
{
    NtHdrs64 *nt   = (NtHdrs64 *)(g_img + ((DosHdr *)g_img)->lfanew);
    SecHdr   *secs = (SecHdr *)((u8 *)&nt->opt + nt->file.opthdr_sz);
    for (u16 i = 0; i < nt->file.nsections; i++) {
        SecHdr *s = &secs[i];
        if (!s->vsz) continue;
        int prot = 0;
        if (s->chars & SCN_READ)  prot |= PROT_READ;
        if (s->chars & SCN_WRITE) prot |= PROT_WRITE;
        if (s->chars & SCN_EXEC)  prot |= PROT_EXEC;
        size_t sz = ((size_t)s->vsz + 0xFFF) & ~(size_t)0xFFF;
        if (mprotect(g_img + s->vrva, sz, prot) < 0)
            fprintf(stderr, "[WARN] mprotect(%.8s): %s\n", s->name, strerror(errno));
    }
}

static int image_addr_is_exec(u64 addr)
{
    if (!g_img) return 0;
    NtHdrs64 *nt   = (NtHdrs64 *)(g_img + ((DosHdr *)g_img)->lfanew);
    SecHdr   *secs = (SecHdr *)((u8 *)&nt->opt + nt->file.opthdr_sz);
    u64 off = addr - (u64)g_img;
    for (u16 i = 0; i < nt->file.nsections; i++) {
        SecHdr *s = &secs[i];
        u64 start = s->vrva;
        u64 end   = s->vrva + s->vsz;
        if (off >= start && off < end)
            return (s->chars & SCN_EXEC) ? 1 : 0;
    }
    return 0;
}

/* ---- Real PE unwind-info based single-frame unwind ----
 * Crash recovery used to "scan the next N stack slots looking for
 * something that looks like a valid return address" -- unsound, since it
 * can walk RSP past the real call chain into never-written memory (see
 * repo memory notes). The .pdata/UNWIND_INFO tables in the PE already
 * describe exactly how many bytes each function's prologue allocates
 * (pushed nonvolatile registers + stack frame size), so we can compute
 * the REAL return-address slot instead of guessing.
 *
 * This assumes the crash occurs after the function's prologue has fully
 * executed (true for the overwhelming majority of runtime crashes, which
 * happen deep in a function body rather than in its first few
 * instructions) -- we don't track the precise prologue byte offset.
 */
static int pe_unwind_frame_size(u64 rip, u64 *out_frame_bytes)
{
    if (!g_img) return 0;
    NtHdrs64 *nt = (NtHdrs64 *)(g_img + ((DosHdr *)g_img)->lfanew);
    u64 base = (u64)g_img;
    u64 top = base + nt->opt.sz_image;
    if (rip < base || rip >= top) return 0;

    DataDir *pd = &nt->opt.dirs[3]; /* IMAGE_DIRECTORY_ENTRY_EXCEPTION */
    if (!pd->size) return 0;
    RUNTIME_FUNC *funcs = (RUNTIME_FUNC *)(g_img + pd->rva);
    int nfuncs = (int)(pd->size / sizeof(RUNTIME_FUNC));

    u32 rva = (u32)(rip - base);
    int lo = 0, hi = nfuncs - 1, mid_idx = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (rva < funcs[mid].Begin) hi = mid - 1;
        else if (rva >= funcs[mid].End) lo = mid + 1;
        else { mid_idx = mid; break; }
    }
    if (mid_idx < 0) {
        /* No RUNTIME_FUNCTION entry: a true leaf function that neither
         * allocates stack space nor saves registers. [rsp] is already
         * the return address. */
        if (out_frame_bytes) *out_frame_bytes = 0;
        return 1;
    }

    RUNTIME_FUNC *rf = &funcs[mid_idx];
    u64 total_bytes = 0;
    int chain_guard = 0;
    while (rf && rf->UnwindData) {
        if (chain_guard++ > 8) return 0; /* pathological chain, don't trust it */
        u8 *ui = (u8 *)(g_img + rf->UnwindData);
        u8 flags = (u8)(ui[0] >> 3);
        u8 count_of_codes = ui[2];
        u16 *codes = (u16 *)(ui + 4);
        int i = 0;
        while (i < count_of_codes) {
            u8 op     = (u8)((codes[i] >> 8)  & 0xF);
            u8 opinfo = (u8)((codes[i] >> 12) & 0xF);
            switch (op) {
            case 0: total_bytes += 8; i += 1; break;                    /* UWOP_PUSH_NONVOL */
            case 1:                                                     /* UWOP_ALLOC_LARGE */
                if (opinfo == 0) { total_bytes += (u64)codes[i + 1] * 8; i += 2; }
                else { total_bytes += ((u64)codes[i + 1] | ((u64)codes[i + 2] << 16)); i += 3; }
                break;
            case 2: total_bytes += (u64)opinfo * 8 + 8; i += 1; break;  /* UWOP_ALLOC_SMALL */
            case 3: i += 1; break;                                      /* UWOP_SET_FPREG */
            case 4: i += 2; break;                                      /* UWOP_SAVE_NONVOL */
            case 5: i += 3; break;                                      /* UWOP_SAVE_NONVOL_FAR */
            case 8: i += 2; break;                                      /* UWOP_SAVE_XMM128 */
            case 9: i += 3; break;                                      /* UWOP_SAVE_XMM128_FAR */
            case 10: i += 1; break;                                     /* UWOP_PUSH_MACHFRAME */
            default: return 0; /* unrecognized opcode: don't trust accounting */
            }
        }
        if (flags & 0x4 /* UNW_FLAG_CHAININFO */) {
            int padded = (count_of_codes + 1) & ~1;
            rf = (RUNTIME_FUNC *)(ui + 4 + (size_t)padded * 2);
        } else {
            rf = NULL;
        }
    }

    if (out_frame_bytes) *out_frame_bytes = total_bytes;
    return 1;
}

/* Attempt to recover from a crash by computing the REAL return address of
 * the crashing function via .pdata/UNWIND_INFO (not by guessing), and
 * emulating a single `ret` to it. Returns 1 and fills *out_ret/*out_new_rsp
 * on success (validated target + within the g_entry_rsp budget), else 0
 * (caller should fall back to other recovery strategies). */
static int try_real_unwind_return(u64 rip, u64 rsp, u64 *out_ret, u64 *out_new_rsp)
{
    u64 frame_bytes;
    if (!pe_unwind_frame_size(rip, &frame_bytes)) return 0;
    if (!is_guest_stack_rsp(rsp)) return 0;
    if (frame_bytes > 0x100000) return 0; /* sanity cap: a >1MB frame is bogus data */
    u64 ret_slot = rsp + frame_bytes;
    if (!is_guest_stack_rsp(ret_slot) || !is_guest_stack_rsp(ret_slot + 8)) return 0;
    if (g_entry_rsp_limit && ret_slot + 8 > g_entry_rsp_limit) return 0;
    u64 ret = *(u64 *)ret_slot;
    if (!is_valid_resume_target(ret)) return 0;
    if (out_ret) *out_ret = ret;
    if (out_new_rsp) *out_new_rsp = ret_slot + 8;
    return 1;
}

/* The early startup helper sequence around 0x235a636..0x235a6c5 expects a
 * Windows x64 caller frame whose saved return address is at [rsp+0x5c8] and
 * whose linked-frame pointer is at [rsp+0x5d0]. Reproduce that exact layout
 * instead of falling back to the safe stub immediately. */
static int try_startup_helper_frame_resume(u64 rip, u64 rsp, u64 *out_ret, u64 *out_new_rsp)
{
    if (!g_img) return 0;
    if (rip < (u64)g_img + 0x235a636 || rip > (u64)g_img + 0x235a6c5) return 0;
    if (!is_guest_stack_rsp(rsp)) return 0;

    u64 ret_slot = rsp + 0x5c8;
    u64 frame_ptr_slot = rsp + 0x5d0;
    u64 next_rsp = ret_slot + 8;
    if (!is_guest_stack_rsp(ret_slot) || !is_guest_stack_rsp(frame_ptr_slot) ||
        !is_guest_stack_rsp(next_rsp))
        return 0;
    if (g_entry_rsp_limit && next_rsp > g_entry_rsp_limit) return 0;

    u64 ret = *(u64 *)ret_slot;
    u64 linked = *(u64 *)frame_ptr_slot;
    if (linked != ret_slot) return 0;
    if (!is_valid_resume_target(ret)) return 0;

    if (out_ret) *out_ret = ret;
    if (out_new_rsp) *out_new_rsp = next_rsp;
    return 1;
}

/* Patch all Dantelion2 transaction-failed assertion branches:
 *   83 7b 10 00 74 43    cmp [rbx+0x10],0 ; je +0x43
 * We NOP-out the 2-byte JE to avoid panicking on transient init state.
 */
static int patch_transaction_assertions(void)
{
    if (!g_img) return 0;

    NtHdrs64 *nt   = (NtHdrs64 *)(g_img + ((DosHdr *)g_img)->lfanew);
    SecHdr   *secs = (SecHdr *)((u8 *)&nt->opt + nt->file.opthdr_sz);
    int patched = 0;

    for (u16 i = 0; i < nt->file.nsections; i++) {
        SecHdr *s = &secs[i];
        if (!(s->chars & SCN_EXEC) || s->vsz < 6) continue;
        u8 *p = g_img + s->vrva;
        u8 *e = p + s->vsz - 5;
        for (; p < e; p++) {
            if (p[0] == 0x83 && p[1] == 0x7b && p[2] == 0x10 &&
                p[3] == 0x00 && p[4] == 0x74 && p[5] == 0x43) {
                p[4] = 0x90;
                p[5] = 0x90;
                patched++;
            }
        }
    }
    return patched;
}

/* Install tiny failure stubs for known bad indirect-call targets that the
 * game reads from writable data pages as code pointers.
 * Stub bytes: xor eax,eax ; ret
 */
static void patch_known_bad_targets(void)
{
    if (!g_img) return;

    const int aggressive = getenv("BEER_ENABLE_HARD_PATCHES") != NULL;

    /* Known crashing target hit by worker threads: img+0x3dd7570 */
    {
        u64 target = (u64)g_img + 0x3dd7570;
        u64 page   = target & ~(u64)0xFFF;
        if (mprotect((void *)page, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
            u8 *p = (u8 *)target;
            p[0] = 0x31; p[1] = 0xC0; /* xor eax,eax */
            p[2] = 0xC3;              /* ret */
            fprintf(stderr, "[PATCH] Installed bad-target stub at RVA 0x3dd7570\n");
        } else {
            fprintf(stderr, "[WARN] Failed to patch known bad target RVA 0x3dd7570: %s\n",
                    strerror(errno));
        }
    }

    /* Null-write helper at RVA 0x2359f48: a guard path tries to clear a field
     * through a possibly-null object pointer. Make that write a harmless no-op
     * so the guard comparison can still decide whether to continue instead of
     * crashing with RIP=0 / faultaddr=0 on the first startup pass.
     */
    {
        u64 target = (u64)g_img + 0x2359f48;
        u64 page   = target & ~(u64)0xFFF;
        if (mprotect((void *)page, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
            u8 *p = (u8 *)target;
            p[0] = 0x90; p[1] = 0x90; p[2] = 0x90;
            fprintf(stderr, "[PATCH] Null-write guard at RVA 0x2359f48 converted to a no-op\n");
        } else {
            fprintf(stderr, "[WARN] Failed to patch null-write guard at RVA 0x2359f48: %s\n",
                    strerror(errno));
        }
    }

    /* Sekiro game-init bad-return at RVA 0x237ce59: a function epilogue tries to
     * return via a NULL stack slot (NULL return address). This creates an infinite
     * loop as the fault handler can't break out. Patch the `ret` to jump to a safe
     * stub (xor eax,eax; ret) that allows the function chain to complete. */
    {
        u64 target = (u64)g_img + 0x237ce59;
        u64 page   = target & ~(u64)0xFFF;
        if (mprotect((void *)page, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
            u8 *p = (u8 *)target;
            p[0] = 0x31; p[1] = 0xC0; /* xor eax,eax */
            p[2] = 0xC3;              /* ret */
            fprintf(stderr, "[PATCH] Sekiro bad-return stub at RVA 0x237ce59 patched to safe return\n");
        } else {
            fprintf(stderr, "[WARN] Failed to patch Sekiro bad-return at RVA 0x237ce59: %s\n",
                    strerror(errno));
        }
    }

    if (aggressive) {
        /* Entry bootstrap helper ... */
        {
            u64 target = (u64)g_img + 0x235a82c;
            u64 tpage  = target & ~(u64)0xFFF;
            if (mprotect((void *)tpage, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                u8 *p = (u8 *)target;
                p[0] = 0x31; p[1] = 0xC0; /* xor eax,eax */
                p[2] = 0xC3;              /* ret */
                fprintf(stderr, "[PATCH] Stubbed early security-cookie init at RVA 0x235a82c\n");
            } else {
                fprintf(stderr, "[WARN] Failed to patch early security-cookie init RVA 0x235a82c: %s\n",
                        strerror(errno));
            }
        }

        /* Worker-thread cleanup routine ... */
        {
            u8 *jcc = g_img + 0x1a6652e;
            u8 *c1  = g_img + 0x1a6654f;
            u8 *c2  = g_img + 0x1a66559;
            u64 tpage = ((u64)jcc) & ~(u64)0xFFF;
            if (mprotect((void *)tpage, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                if (c1[0] == 0xff && c1[1] == 0x10) {
                    c1[0] = 0x90; c1[1] = 0x90;
                }
                if (c2[0] == 0xff && c2[1] == 0x10) {
                    c2[0] = 0x90; c2[1] = 0x90;
                }
            } else {
                fprintf(stderr, "[WARN] Failed to patch worker vcall block: %s\n", strerror(errno));
            }
            fprintf(stderr, "[PATCH] Hardened worker vcall block at RVA 0x1a6652e\n");
        }
    } else {
        fprintf(stderr,
                "[PATCH] Leaving early startup helper blocks untouched by default. "
                "Set BEER_ENABLE_HARD_PATCHES=1 to opt into these.\n");
    }

    if (aggressive) {
        /* Lock/transaction helper hot loop ... */
        {
            u64 target = (u64)g_img + 0x23ae079;
            u64 tpage  = target & ~(u64)0xFFF;
            if (mprotect((void *)tpage, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                u8 *p = (u8 *)target;
                p[0] = 0x31; p[1] = 0xC0; /* xor eax,eax */
                p[2] = 0xC3;              /* ret */
                p[3] = 0x90; p[4] = 0x90;
                fprintf(stderr, "[PATCH] Stubbed lock/txn helper at RVA 0x23ae079\n");
            } else {
                fprintf(stderr, "[WARN] Failed to patch lock/txn helper RVA 0x23ae079: %s\n",
                        strerror(errno));
            }
        }
    } else {
        fprintf(stderr,
                "[PATCH] Leaving transaction-helper null-check islands untouched by default. "
                "Set BEER_ENABLE_HARD_PATCHES=1 to opt into these.\n");
    }

    if (!aggressive) {
        fprintf(stderr,
                "[PATCH] Skipping startup short-circuit patches by default. "
                "Set BEER_ENABLE_HARD_PATCHES=1 to opt into them.\n");
    } else {
        /* Startup short-circuit blocks intentionally route through a harmless return. */
        {
            u64 target = (u64)g_img + 0x195860;
            u64 tpage  = target & ~(u64)0xFFF;
            if (mprotect((void *)tpage, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                u8 *p = (u8 *)target;
                p[0] = 0xB8; p[1] = 0x01; p[2] = 0x00; p[3] = 0x00; p[4] = 0x00; p[5] = 0xC3;
                fprintf(stderr, "[PATCH] Forced CRT startup gate at RVA 0x195860 to succeed\n");
            } else {
                fprintf(stderr, "[WARN] Failed to patch CRT startup gate RVA 0x195860: %s\n",
                        strerror(errno));
            }
        }

        {
            u64 target = (u64)g_img + 0x237d025;
            u64 safe   = (u64)g_img + 0x235a694;
            u64 tpage  = target & ~(u64)0xFFF;
            if (mprotect((void *)tpage, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                u8 *p = (u8 *)target;
                p[0] = 0x48; p[1] = 0xB8;
                memcpy(p + 2, &safe, 8);
                p[10] = 0xEB; p[11] = 0xF6;
                fprintf(stderr, "[PATCH] Redirected null lookup at RVA 0x237ce25 -> 0x235a694\n");
            } else {
                fprintf(stderr, "[WARN] Failed to patch null lookup RVA 0x237ce25: %s\n",
                        strerror(errno));
            }
        }

        {
            u64 safe = (u64)g_img + 0x235a694;
            static const u32 slots[] = { 0x2929700, 0x2929710, 0x2929750, 0x2929788 };
            u64 tpage = ((u64)g_img + slots[0]) & ~(u64)0xFFF;
            if (mprotect((void *)tpage, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                for (u32 i = 0; i < (u32)(sizeof(slots) / sizeof(slots[0])); i++) {
                    u64 *slot = (u64 *)((u8 *)g_img + slots[i]);
                    *slot = safe;
                    fprintf(stderr, "[PATCH] Seeded fallback slot at RVA 0x%x -> 0x235a694\n",
                            slots[i]);
                }
            } else {
                fprintf(stderr, "[WARN] Failed to seed fallback slots: %s\n", strerror(errno));
            }
        }

        {
            struct { u32 rva; const char *name; } helpers[] = {
                { 0x23ab848, "compare helper" },
                { 0x23ab7d0, "compare helper" },
            };
            for (u32 i = 0; i < (u32)(sizeof(helpers) / sizeof(helpers[0])); i++) {
                u64 target = (u64)g_img + helpers[i].rva;
                u64 tpage  = target & ~(u64)0xFFF;
                if (mprotect((void *)tpage, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                    u8 *p = (u8 *)target;
                    p[0] = 0x31; p[1] = 0xC0; p[2] = 0xC3;
                    fprintf(stderr, "[PATCH] Stubbed %s at RVA 0x%x\n",
                            helpers[i].name, helpers[i].rva);
                } else {
                    fprintf(stderr, "[WARN] Failed to patch %s RVA 0x%x: %s\n",
                            helpers[i].name, helpers[i].rva, strerror(errno));
                }
            }
        }

        {
            u64 target = (u64)g_img + 0x23b78e1;
            u64 tpage  = target & ~(u64)0xFFF;
            if (mprotect((void *)tpage, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                u8 *p = (u8 *)target;
                p[0] = 0x31; p[1] = 0xC0; p[2] = 0xC3; p[3] = 0x90; p[4] = 0x90;
                fprintf(stderr, "[PATCH] Stubbed lock/txn helper at RVA 0x23b78e1\n");
            } else {
                fprintf(stderr, "[WARN] Failed to patch lock/txn helper RVA 0x23b78e1: %s\n",
                        strerror(errno));
            }
        }

        /* Hot return sites repeatedly reached during low-RIP recovery. */
        {
            u64 target = (u64)g_img + 0x23b7a24;
            u64 tpage  = target & ~(u64)0xFFF;
            if (mprotect((void *)tpage, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                u8 *p = (u8 *)target;
                p[0] = 0x31; p[1] = 0xC0; p[2] = 0xC3; p[3] = 0x90; p[4] = 0x90;
                fprintf(stderr, "[PATCH] Stubbed hot return site at RVA 0x23b7a24\n");
            } else {
                fprintf(stderr, "[WARN] Failed to patch hot return site RVA 0x23b7a24: %s\n",
                        strerror(errno));
            }
        }

        {
            u64 target = (u64)g_img + 0x239de43;
            u64 tpage  = target & ~(u64)0xFFF;
            if (mprotect((void *)tpage, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                u8 *p = (u8 *)target;
                p[0] = 0x31; p[1] = 0xC0; p[2] = 0xC3; p[3] = 0x90; p[4] = 0x90;
                fprintf(stderr, "[PATCH] Stubbed hot return site at RVA 0x239de43\n");
            } else {
                fprintf(stderr, "[WARN] Failed to patch hot return site RVA 0x239de43: %s\n",
                        strerror(errno));
            }
        }

        {
            u64 target = (u64)g_img + 0x239de38;
            u64 tpage  = target & ~(u64)0xFFF;
            if (mprotect((void *)tpage, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                u8 *p = (u8 *)target;
                p[0] = 0x31; p[1] = 0xC0; p[2] = 0xC3; p[3] = 0x90; p[4] = 0x90;
                fprintf(stderr, "[PATCH] Stubbed hot return site at RVA 0x239de38\n");
            } else {
                fprintf(stderr, "[WARN] Failed to patch hot return site RVA 0x239de38: %s\n",
                        strerror(errno));
            }
        }

        {
            u64 target = (u64)g_img + 0x23979f9;
            u64 tpage  = target & ~(u64)0xFFF;
            if (mprotect((void *)tpage, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                u8 *p = (u8 *)target;
                p[0] = 0x31; p[1] = 0xC0; p[2] = 0xC3; p[3] = 0x90; p[4] = 0x90;
                fprintf(stderr, "[PATCH] Stubbed hot return site at RVA 0x23979f9\n");
            } else {
                fprintf(stderr, "[WARN] Failed to patch hot return site RVA 0x23979f9: %s\n",
                        strerror(errno));
            }
        }

        {
            u64 target = (u64)g_img + 0x239ea2e;
            u64 tpage  = target & ~(u64)0xFFF;
            if (mprotect((void *)tpage, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                u8 *p = (u8 *)target;
                p[0] = 0x31; p[1] = 0xC0; p[2] = 0xC3; p[3] = 0x90; p[4] = 0x90;
                fprintf(stderr, "[PATCH] Stubbed crash site at RVA 0x239ea2e\n");
            } else {
                fprintf(stderr, "[WARN] Failed to patch crash site RVA 0x239ea2e: %s\n",
                        strerror(errno));
            }
        }

        {
            u64 target = (u64)g_img + 0x23a6d71;
            u64 tpage  = target & ~(u64)0xFFF;
            if (mprotect((void *)tpage, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                u8 *p = (u8 *)target;
                p[0] = 0x31; p[1] = 0xC0; p[2] = 0xC3; p[3] = 0x90; p[4] = 0x90;
                fprintf(stderr, "[PATCH] Stubbed loop site at RVA 0x23a6d71\n");
            } else {
                fprintf(stderr, "[WARN] Failed to patch loop site RVA 0x23a6d71: %s\n",
                        strerror(errno));
            }
        }

        {
            u64 target = (u64)g_img + 0x23b79d9;
            u64 tpage  = target & ~(u64)0xFFF;
            if (mprotect((void *)tpage, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                u8 *p = (u8 *)target;
                p[0] = 0x31; p[1] = 0xC0; p[2] = 0xC3; p[3] = 0x90; p[4] = 0x90;
                fprintf(stderr, "[PATCH] Stubbed loop site at RVA 0x23b79d9\n");
            } else {
                fprintf(stderr, "[WARN] Failed to patch loop site RVA 0x23b79d9: %s\n",
                        strerror(errno));
            }
        }

        {
            u64 target = (u64)g_img + 0x23b6275;
            u64 tpage  = target & ~(u64)0xFFF;
            if (mprotect((void *)tpage, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                u8 *p = (u8 *)target;
                p[0] = 0x31; p[1] = 0xC0; p[2] = 0xC3; p[3] = 0x90; p[4] = 0x90;
                fprintf(stderr, "[PATCH] Stubbed crash site at RVA 0x23b6275\n");
            } else {
                fprintf(stderr, "[WARN] Failed to patch crash site RVA 0x23b6275: %s\n",
                        strerror(errno));
            }
        }

        {
            u64 target = (u64)g_img + 0x239c625;
            u64 tpage  = target & ~(u64)0xFFF;
            if (mprotect((void *)tpage, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                u8 *p = (u8 *)target;
                p[0] = 0x31; p[1] = 0xC0; p[2] = 0xC3; p[3] = 0x90; p[4] = 0x90;
                fprintf(stderr, "[PATCH] Stubbed hot return site at RVA 0x239c625\n");
            } else {
                fprintf(stderr, "[WARN] Failed to patch hot return site RVA 0x239c625: %s\n",
                        strerror(errno));
            }
        }

        {
            u64 target = (u64)g_img + 0x239c6a8;
            u64 tpage  = target & ~(u64)0xFFF;
            if (mprotect((void *)tpage, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                u8 *p = (u8 *)target;
                p[0] = 0x31; p[1] = 0xC0; p[2] = 0xC3; p[3] = 0x90; p[4] = 0x90;
                fprintf(stderr, "[PATCH] Stubbed hot return site at RVA 0x239c6a8\n");
            } else {
                fprintf(stderr, "[WARN] Failed to patch hot return site RVA 0x239c6a8: %s\n",
                        strerror(errno));
            }
        }

        {
            u64 target = (u64)g_img + 0x23a7dec;
            u64 tpage  = target & ~(u64)0xFFF;
            if (mprotect((void *)tpage, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                u8 *p = (u8 *)target;
                p[0] = 0x31; p[1] = 0xC0; p[2] = 0xC3; p[3] = 0x90; p[4] = 0x90;
                fprintf(stderr, "[PATCH] Stubbed hot return site at RVA 0x23a7dec\n");
            } else {
                fprintf(stderr, "[WARN] Failed to patch hot return site RVA 0x23a7dec: %s\n",
                        strerror(errno));
            }
        }

        {
            u64 target = (u64)g_img + 0x235a6a4;
            u64 tpage  = target & ~(u64)0xFFF;
            u64 cont   = (u64)g_img + 0x235a694;
            if (mprotect((void *)tpage, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                u8 *p = (u8 *)target;
                p[0] = 0x48; p[1] = 0xB8;
                memcpy(p + 2, &cont, 8);
                p[10] = 0xFF; p[11] = 0xE0;
                fprintf(stderr, "[PATCH] Redirected RVA 0x235a6a4 -> 0x235a694\n");
            } else {
                fprintf(stderr, "[WARN] Failed to redirect RVA 0x235a6a4: %s\n",
                        strerror(errno));
            }
        }

        {
            u64 target = (u64)g_img + 0x2e918a0;
            u64 tpage  = target & ~(u64)0xFFF;
            u64 cont   = (u64)g_img + 0x235a694;
            if (mprotect((void *)tpage, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                u8 *p = (u8 *)target;
                p[0] = 0x65; p[1] = 0x48; p[2] = 0x8B; p[3] = 0x24; p[4] = 0x25;
                p[5] = 0x08; p[6] = 0x00; p[7] = 0x00; p[8] = 0x00;
                p[9] = 0x48; p[10] = 0x81; p[11] = 0xEC;
                p[12] = 0x00; p[13] = 0x02; p[14] = 0x00; p[15] = 0x00;
                p[16] = 0x48; p[17] = 0xB8;
                memcpy(p + 18, &cont, 8);
                p[26] = 0xFF; p[27] = 0xE0;
                fprintf(stderr, "[PATCH] Redirected crash page RVA 0x2e918a0 via stack-fix trampoline -> 0x235a694\n");
            } else {
                fprintf(stderr, "[WARN] Failed to redirect crash page RVA 0x2e918a0: %s\n",
                        strerror(errno));
            }
        }

        {
            static const u32 extra_rvas[] = {
                0x237a32f, 0x239c70c, 0x239ea1a, 0x19edfeb, 0x19eb477, 0x19eb496,
                0x19ee000, 0x19ee013, 0x19ee026, 0x19ee039, 0x19ee04c, 0x19e27f8,
                0x19e280a, 0x19e2816, 0x19e2821, 0x19e2830, 0x19ee0d0, 0x19ee0e0,
                0x19ee0e8, 0x19ee0f2, 0x19ee5e2, 0x19fbf90, 0x19fbfb5, 0x19fbfc3,
                0x19fbfde, 0x19fbfe4, 0x19fc004, 0x19fc013, 0x19fc01a, 0x19fc03c,
                0x19fc043, 0x19fc050, 0x233522d, 0x23351e0, 0x23351ea, 0x23351be,
                0x23351c9, 0x23353d4, 0x23355aa, 0x233b69a, 0x233b6e6, 0x233b823,
                0x233b86a, 0x2352673, 0x23526fd, 0x23a7667, 0x23a6d4c, 0x23a6d73,
                0x23a6d7a, 0x23a6e8f, 0x23a6e96, 0x23adf24, 0x23adf7e, 0x23ae05d,
                0x23ae07b, 0x23ae082, 0x23ae089, 0x23ae1b7, 0x23ae182, 0x23ae18e,
                0x23ab4eb, 0x23ab4f2, 0x23ae1b9, 0x23ae1c0, 0x23ae1d8, 0x23ae1e8,
                0x23ab08e, 0x23ab80c, 0x23ab862, 0x23ab873, 0x23ab875, 0x23ab085,
                0x23ab2e1, 0x23b164a, 0x23b159a, 0x23b57ef, 0x23b7019, 0x23b703f,
                0x23b7066, 0x23b706b, 0x23b7644, 0x23b76e4, 0x23b7987, 0x23b7990,
                0x23b7a24, 0x23b7a26, 0x23b7a28, 0x23b7a2a, 0x23b7a2f, 0x23b7a31,
                0x23b7a33, 0x23b7a38, 0x23b7a3a, 0x23b7a4f, 0x23b7a65, 0x23b7a75,
                0x23b7a97, 0x23b7a9e, 0x23b7aa5, 0x23b7ab0, 0x23b7ab7, 0x23bb64f,
                0x23bb65a, 0x23bb667, 0x23bb674, 0x23bb681, 0x23ba892, 0x23ba89c,
                0x23ba8a3, 0x23ba8b0, 0x23ba8bf, 0x23ae0dd, 0x30c1a48, 0x19dcfda,
                0x35a142, 0x35a500, 0x1a3bc36, 0x1a0559e, 0x1a055b3, 0x1a055ba,
                0x1a055c8, 0x1a055d1, 0x19dca02, 0x2359b88, 0x2dc5820, 0x2dc5948,
                0x2dc5960, 0x3ec7d60, 0x3ec7d68, 0x239c627, 0x3f08960, 0x3f08bc0,
                0x3f08bc8, 0x3f31e30, 0x3f332a0, 0x3f332f0, 0x39ea11, 0x39ea1a,
                0x237cee4, 0x237cef3, 0x237cf00, 0x237cf10, 0x237cf2e, 0x237cff0, 0x237d01d, 0x1130b2e,
                0x1130b36, 0x1130b3d, 0x1130b44, 0x1130b4c, 0x30b5a90, 0x30b5ad0,
                0x30b5af8, 0x30b5b08, 0x30b5bb2, 0x30bea90, 0x30beaa8, 0x30beaaf,
                0x2945090, 0x29450e0, 0x011ae01, 0x011ae80, 0x011ae95,
            };
            for (u32 i = 0; i < (u32)(sizeof(extra_rvas) / sizeof(extra_rvas[0])); i++) {
                u64 target = (u64)g_img + extra_rvas[i];
                u64 tpage  = target & ~(u64)0xFFF;
                if (mprotect((void *)tpage, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                    u8 *p = (u8 *)target;
                    p[0] = 0x31; p[1] = 0xC0; p[2] = 0xC3; p[3] = 0x90; p[4] = 0x90;
                    fprintf(stderr, "[PATCH] Stubbed extra site at RVA 0x%x\n", extra_rvas[i]);
                } else {
                    fprintf(stderr, "[WARN] Failed to patch extra site RVA 0x%x: %s\n",
                            extra_rvas[i], strerror(errno));
                }
            }
        }

        {
            u64 island = (u64)g_img + 0x3f33280;
            u64 tpage  = island & ~(u64)0xFFF;
            if (mprotect((void *)tpage, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                memset((void *)island, 0xC3, 0x80);
                fprintf(stderr, "[PATCH] Ret-sled at RVA 0x3f33280..0x3f332ff\n");
            } else {
                fprintf(stderr, "[WARN] Failed ret-sled at RVA 0x3f33280: %s\n", strerror(errno));
            }
        }

        {
            u64 island = (u64)g_img + 0x23ae050;
            u64 tpage  = island & ~(u64)0xFFF;
            if (mprotect((void *)tpage, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                memset((void *)island, 0xC3, 0x60);
                fprintf(stderr, "[PATCH] Ret-sled at RVA 0x23ae050..0x23ae0af\n");
            } else {
                fprintf(stderr, "[WARN] Failed ret-sled at RVA 0x23ae050: %s\n", strerror(errno));
            }
        }

        {
            u64 island = (u64)g_img + 0x23ae170;
            u64 tpage  = island & ~(u64)0xFFF;
            if (mprotect((void *)tpage, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                memset((void *)island, 0xC3, 0x70);
                fprintf(stderr, "[PATCH] Ret-sled at RVA 0x23ae170..0x23ae1df\n");
            } else {
                fprintf(stderr, "[WARN] Failed ret-sled at RVA 0x23ae170: %s\n", strerror(errno));
            }
        }

        {
            u64 island = (u64)g_img + 0x23b7a20;
            u64 tpage  = island & ~(u64)0xFFF;
            if (mprotect((void *)tpage, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                memset((void *)island, 0xC3, 0xB0);
                fprintf(stderr, "[PATCH] Ret-sled at RVA 0x23b7a20..0x23b7acf\n");
            } else {
                fprintf(stderr, "[WARN] Failed ret-sled at RVA 0x23b7a20: %s\n", strerror(errno));
            }
        }

        {
            u64 island = (u64)g_img + 0x23b7000;
            u64 tpage  = island & ~(u64)0xFFF;
            if (mprotect((void *)tpage, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                memset((void *)island, 0xC3, 0xD0);
                fprintf(stderr, "[PATCH] Ret-sled at RVA 0x23b7000..0x23b70cf\n");
            } else {
                fprintf(stderr, "[WARN] Failed ret-sled at RVA 0x23b7000: %s\n", strerror(errno));
            }
        }

        {
            u64 island = (u64)g_img + 0x239c620;
            u64 tpage  = island & ~(u64)0xFFF;
            if (mprotect((void *)tpage, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                memset((void *)island, 0xC3, 0x40);
                fprintf(stderr, "[PATCH] Ret-sled at RVA 0x239c620..0x239c65f\n");
            } else {
                fprintf(stderr, "[WARN] Failed ret-sled at RVA 0x239c620: %s\n", strerror(errno));
            }
        }

        {
            u64 island = (u64)g_img + 0x23bb640;
            u64 tpage  = island & ~(u64)0xFFF;
            if (mprotect((void *)tpage, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                memset((void *)island, 0xC3, 0xA0);
                fprintf(stderr, "[PATCH] Ret-sled at RVA 0x23bb640..0x23bb6df\n");
            } else {
                fprintf(stderr, "[WARN] Failed ret-sled at RVA 0x23bb640: %s\n", strerror(errno));
            }
        }

        {
            u64 island1 = (u64)g_img + 0x30b5a80;
            u64 island2 = (u64)g_img + 0x30bea80;
            u64 tpage1 = island1 & ~(u64)0xFFF;
            u64 tpage2 = island2 & ~(u64)0xFFF;
            if (mprotect((void *)tpage1, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                memset((void *)island1, 0xC3, 0x180);
                fprintf(stderr, "[PATCH] Ret-sled at RVA 0x30b5a80..0x30b5bff\n");
            }
            if (mprotect((void *)tpage2, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                memset((void *)island2, 0xC3, 0x80);
                fprintf(stderr, "[PATCH] Ret-sled at RVA 0x30bea80..0x30beaff\n");
            }
        }
    }
}

/* ── Crash handler ──────────────────────────────────────────────── */
static u64 crash_pick_fallback_rip(u64 rip, const char *tag)
{
    /* Default recovery should not keep bouncing back into the hot 0x23b7a24
     * panic island. Use the real in-image startup continuation instead of the
     * fail-return stub when we are trying to recover: the fail stub pops from
     * the live stack and can immediately re-enter stale stack memory. */
    const int aggressive = getenv("BEER_ENABLE_HARD_PATCHES") != NULL;
    u64 safe_resume = guest_resume_rip();
    u64 primary = aggressive ? ((u64)g_img + 0x23b7a24) : safe_resume;
    u64 alt1    = aggressive ? ((u64)g_img + 0x23ae05d) : safe_resume;
    u64 alt2    = aggressive ? ((u64)g_img + 0x239c625) : safe_resume;
    u64 alt3    = safe_resume;

    u64 bucket = rip & ~0xFFFULL;
    g_fb_total++;
    if (bucket == g_fb_last_bucket)
        g_fb_bucket_hits++;
    else {
        g_fb_last_bucket = bucket;
        g_fb_bucket_hits = 1;
    }

    if (tag && strcmp(tag, "null-txn") == 0 && g_fb_bucket_hits > 6) {
        fprintf(stderr,
                "[GATE] %s: hard cap at bucket 0x%lx (%u), alt3\n",
                tag, bucket, g_fb_bucket_hits);
        return alt3;
    }

    if (tag && strcmp(tag, "null-txn") == 0) {
        if (g_fb_bucket_hits > 2 || g_fb_total > 120) {
            fprintf(stderr,
                    "[GATE] %s: early escape at bucket 0x%lx (%u), alt2\n",
                    tag, bucket, g_fb_bucket_hits);
            return alt2;
        }
        return primary;
    }

    if (tag && strcmp(tag, "null-hot") == 0 && g_fb_bucket_hits > 8) {
        fprintf(stderr,
                "[GATE] %s: hard cap at bucket 0x%lx (%u), alt2\n",
                tag, bucket, g_fb_bucket_hits);
        return alt2;
    }

    if (tag && strcmp(tag, "null-1130b") == 0 && g_fb_bucket_hits > 4) {
        fprintf(stderr,
                "[GATE] %s: hard cap at bucket 0x%lx (%u), alt3\n",
                tag, bucket, g_fb_bucket_hits);
        return alt3;
    }

    if (tag && strcmp(tag, "null-posttxn") == 0 && g_fb_bucket_hits > 3) {
        if (g_fb_bucket_hits > 9) {
            g_posttxn_escapes++;
            if (g_posttxn_escapes & 1) {
                fprintf(stderr,
                        "[GATE] %s: deep escape at bucket 0x%lx (%u), alt2\n",
                        tag, bucket, g_fb_bucket_hits);
                return alt2;
            }
            fprintf(stderr,
                    "[GATE] %s: deep escape at bucket 0x%lx (%u), primary\n",
                    tag, bucket, g_fb_bucket_hits);
            return primary;
        }
        fprintf(stderr,
                "[GATE] %s: escape at bucket 0x%lx (%u), primary\n",
                tag, bucket, g_fb_bucket_hits);
        return primary;
    }

    if (tag && strcmp(tag, "null-dispatch") == 0 && g_fb_bucket_hits > 3) {
        fprintf(stderr,
                "[GATE] %s: escape at bucket 0x%lx (%u), primary\n",
                tag, bucket, g_fb_bucket_hits);
        return primary;
    }

    if (tag && strcmp(tag, "null-237ce") == 0 && g_fb_bucket_hits > 4) {
        fprintf(stderr,
                "[GATE] %s: hard cap at bucket 0x%lx (%u), alt3\n",
                tag, bucket, g_fb_bucket_hits);
        return alt3;
    }

    if (tag && strcmp(tag, "low-rip") == 0 && g_fb_bucket_hits > 6) {
        fprintf(stderr,
                "[GATE] %s: low-rip loop at bucket 0x%lx (%u), alt3\n",
                tag, bucket, g_fb_bucket_hits);
        return alt3;
    }

    if (tag && strcmp(tag, "low-rva") == 0 && g_fb_bucket_hits > 3) {
        fprintf(stderr,
                "[GATE] %s: low-rva loop at bucket 0x%lx (%u), alt3\n",
                tag, bucket, g_fb_bucket_hits);
        return alt3;
    }

    if (tag && strcmp(tag, "sigbus") == 0 && g_fb_bucket_hits > 3) {
        fprintf(stderr,
                "[GATE] %s: repeat bus at bucket 0x%lx (%u), alt3\n",
                tag, bucket, g_fb_bucket_hits);
        return alt3;
    }

    if (tag && strcmp(tag, "exec-bad") == 0 && g_fb_bucket_hits > 16) {
        fprintf(stderr,
                "[GATE] %s: persistent execute-fault bucket 0x%lx (%u), alt3\n",
                tag, bucket, g_fb_bucket_hits);
        return alt3;
    }

    if (tag && strcmp(tag, "exec-bad") == 0 && g_fb_bucket_hits > 3) {
        fprintf(stderr,
                "[GATE] %s: repeat execute-fault at bucket 0x%lx (%u), alt3\n",
                tag, bucket, g_fb_bucket_hits);
        return alt3;
    }

    if (tag && strcmp(tag, "host-bad") == 0 && g_fb_bucket_hits > 12) {
        fprintf(stderr,
                "[GATE] %s: persistent host-bad bucket 0x%lx (%u), primary\n",
                tag, bucket, g_fb_bucket_hits);
        return primary;
    }

    if (tag && strcmp(tag, "host-mid") == 0 && g_fb_bucket_hits > 8) {
        fprintf(stderr,
                "[GATE] %s: persistent mid-host bucket 0x%lx (%u), primary\n",
                tag, bucket, g_fb_bucket_hits);
        return primary;
    }

    if (tag && strcmp(tag, "rip-noncanon") == 0 && g_fb_bucket_hits > 3) {
        fprintf(stderr,
                "[GATE] %s: non-canonical rip bucket 0x%lx (%u), primary\n",
                tag, bucket, g_fb_bucket_hits);
        return primary;
    }

    if (tag && strcmp(tag, "host-nearnull") == 0 && g_fb_bucket_hits > 3) {
        fprintf(stderr,
                "[GATE] %s: repeat host near-null at bucket 0x%lx (%u), alt3\n",
                tag, bucket, g_fb_bucket_hits);
        return alt3;
    }

    if (tag && strcmp(tag, "null-alt2") == 0) {
        fprintf(stderr,
                "[GATE] %s: escape at bucket 0x%lx (%u), primary\n",
                tag, bucket, g_fb_bucket_hits);
        return primary;
    }

    if (tag && strcmp(tag, "ret-stub-235a694") == 0) {
        if (g_fb_bucket_hits > 3 || g_fb_total > 80) {
            fprintf(stderr,
                    "[GATE] %s: escape at bucket 0x%lx (%u), alt1\n",
                    tag, bucket, g_fb_bucket_hits);
            return alt1;
        }
        fprintf(stderr,
                "[GATE] %s: early escape at bucket 0x%lx (%u), primary\n",
                tag, bucket, g_fb_bucket_hits);
        return primary;
    }

    if (g_fb_bucket_hits > 24) {
        fprintf(stderr,
                "[GATE] %s: heavy loop at bucket 0x%lx (%u), alt2\n",
                tag, bucket, g_fb_bucket_hits);
        return alt2;
    }

    if (g_fb_bucket_hits > 10 || g_fb_total > 80) {
        fprintf(stderr,
                "[GATE] %s: loop at bucket 0x%lx (%u), total=%lu, alt3\n",
                tag, bucket, g_fb_bucket_hits, (unsigned long)g_fb_total);
        return alt3;
    }

    return primary;
}

/* Report progress toward window creation based on which APIs have been called */
static void report_window_progress(void)
{
    int progress = 0;
    fprintf(stderr, "\n[PROGRESS SUMMARY]\n");
    if (g_api_registerclass) { fprintf(stderr, "  [✓] RegisterClass\n"); progress += 25; }
    if (g_api_createwindow) { fprintf(stderr, "  [✓] CreateWindow\n"); progress += 25; }
    if (g_api_createfactory) { fprintf(stderr, "  [✓] CreateDXGIFactory\n"); progress += 25; }
    if (g_api_createdevice) { fprintf(stderr, "  [✓] D3D11CreateDevice\n"); progress += 25; }
    fprintf(stderr, "  Percentage to get window: %d%% (%d/4 major milestones)\n", progress, 
            (g_api_registerclass ? 1 : 0) + (g_api_createwindow ? 1 : 0) + 
            (g_api_createfactory ? 1 : 0) + (g_api_createdevice ? 1 : 0));
    fprintf(stderr, "\n");
    fflush(stderr);
}

static void on_crash_impl(int sig, siginfo_t *si, void *uctx)
{
    ucontext_t *uc = (ucontext_t *)uctx;
    u64 rip = (u64)uc->uc_mcontext.gregs[REG_RIP];
    u64 faultaddr = (u64)si->si_addr;
    u64 rsp0 = (u64)uc->uc_mcontext.gregs[REG_RSP];
    u64 rcx0 = (u64)uc->uc_mcontext.gregs[REG_RCX];

    /* Record + check for a dead recovery cycle FIRST, before any early-return
     * guard below gets a chance to bypass it. Every branch in this function
     * (including the "Invalid guest RSP" reset just below) must be covered by
     * this check -- a branch that resets state to the exact same values every
     * time and returns early, without ever reaching this check, is an
     * unbounded infinite loop instead of a bounded, diagnosed failure. This
     * was previously placed after several early-return blocks and missed the
     * "Invalid guest RSP" reset loop entirely (observed: 3M+ identical
     * resets/sec, no [FATAL] cycle message, process only stopped by an
     * external timeout). */
    cycle_hist_push(rip);
    {
        int period = 0, repeats = 0;
        if (detect_stuck_cycle(&period, &repeats)) {
            fprintf(stderr,
                    "[FATAL] Dead recovery cycle detected: period=%d repeats=%d "
                    "after %lu total crash-handler invocations. Cycle RVAs:\n",
                    period, repeats, (unsigned long)g_cycle_total_calls);
            for (int i = 0; i < period; i++) {
                u64 r = g_cycle_hist[(g_cycle_hist_n - 1 - i) % CYCLE_HIST_LEN];
                u64 rva = (g_img && r >= (u64)g_img) ? r - (u64)g_img : r;
                fprintf(stderr, "  [%d] RIP=0x%lx (RVA 0x%lx)\n", i, r, rva);
            }
            u64 rsp = (u64)uc->uc_mcontext.gregs[REG_RSP];
            fprintf(stderr, "[FATAL] RSP=0x%lx RCX=0x%lx faultaddr=0x%lx entry_rsp=0x%lx %s\n",
                    rsp, rcx0, faultaddr, g_entry_rsp,
                    (g_entry_rsp_limit && rsp > g_entry_rsp_limit) ?
                        "(RSP is ABOVE entry baseline -- walked past all real stack content)" : "");
            u64 *sp = (u64 *)rsp;
            if (rip < 0x1000 && sp) {
                fprintf(stderr, "[FATAL] Top of stack (likely return addrs of the null-call site):\n");
                for (int i = 0; i < 12; i++) {
                    u64 v = sp[i];
                    u64 rva = (g_img && v >= (u64)g_img) ? v - (u64)g_img : v;
                    fprintf(stderr, "  sp[%2d] = 0x%lx (RVA 0x%lx)%s\n", i, v, rva,
                            (g_img && v >= (u64)g_img && v < (u64)g_img + 0x42d2000 &&
                             image_addr_is_exec(v)) ? "  [exec]" : "");
                }
            }
            fprintf(stderr,
                    "[FATAL] Aborting instead of spinning forever; these RVAs "
                    "need a real fix (not another fallback redirect).\n");
            report_window_progress();
            fflush(stderr);
            _exit(2);
        }
    }

    /* EMERGENCY: If RSP is 0, reset to entry baseline immediately.
     * This can happen if beer_dispatch_trampoline's return path corrupted RSP.
     * Without this, RSP=0 leads to infinite crashes and cycle detection firing. */
    if (sig == SIGSEGV && rsp0 == 0 && g_entry_rsp) {
        fprintf(stderr, "[WARN] RSP=0 detected at RIP=0x%lx; resetting to entry baseline 0x%lx\n",
                rip, g_entry_rsp);
        reset_rsp_to_entry_baseline(uc);
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)guest_resume_rip();
        return;
    }

    /* Handle early-startup faults (CRT initialization region RVA 0x2600-0x27ff) using real-unwind.
     * These occur during very early initialization and are usually recoverable
     * via proper function frame unwinding rather than instruction patching. */
    if (sig == SIGSEGV && rip >= (u64)g_img + 0x2600 && rip <= (u64)g_img + 0x27ff &&
        g_guest_stack_low && g_guest_stack_high && rsp0 >= g_guest_stack_low && rsp0 <= g_guest_stack_high) {
        u64 ret_addr = 0, new_rsp = 0;
        static int early_fault_count = 0;
        if (early_fault_count < 10) {
            fprintf(stderr, "[SKIP] Early CRT fault at RIP=0x%lx (RVA 0x%lx)\n", 
                    rip, rip - (u64)g_img);
            early_fault_count++;
        }
        /* Try to use real unwind to find a safe return point */
        if (try_real_unwind_return(rip, rsp0, &ret_addr, &new_rsp)) {
            /* Sanity check: if real-unwind returns to a ret-stub location, avoid it */
            if (ret_addr >= (u64)g_img + 0x235a690 && ret_addr <= (u64)g_img + 0x235a6a0) {
                /* For early CRT, just skip 1 byte and retry instead */
                uc->uc_mcontext.gregs[REG_RIP] = (greg_t)(rip + 1);
                return;
            }
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)ret_addr;
            uc->uc_mcontext.gregs[REG_RSP] = (greg_t)new_rsp;
            return;
        }
        /* If real unwind fails, skip 1 byte and retry */
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)(rip + 1);
        return;
    }

    /* Handle stack-underflow in middle initialization (e.g., RVA 0x99ce-0x9a63 region) */
    if (sig == SIGSEGV && rip >= (u64)g_img + 0x9900 && rip <= (u64)g_img + 0x9b00 &&
        g_guest_stack_low && g_guest_stack_high && rsp0 >= g_guest_stack_low && rsp0 <= g_guest_stack_high) {
        static int mid_fault_count = 0;
        
        /* Increment global limit counter */
        g_mid_init_fault_count++;
        if (g_mid_init_fault_count > MID_INIT_FAULT_LIMIT) {
            fprintf(stderr, "[FATAL] Middle-init SIGSEGV limit exceeded (%d faults), exiting to avoid infinite loop\n",
                    g_mid_init_fault_count);
            report_window_progress();
            exit(98);
        }
        
        if (mid_fault_count < 10) {
            fprintf(stderr, "[SKIP] Middle-init SIGSEGV at RIP=0x%lx (RVA 0x%lx), skipping 2 bytes [fault %d]\n", 
                    rip, rip - (u64)g_img, g_mid_init_fault_count);
            mid_fault_count++;
        }
        /* For middle-init faults, skip 2 bytes to get past problematic instruction sequences */
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)(rip + 2);
        return;
    }

    /* Handle SIGILL in middle initialization range (often follows SIGSEGV at 0x99cf) */
    if (sig == SIGILL && rip >= (u64)g_img + 0x9900 && rip <= (u64)g_img + 0x9b00) {
        static int mid_sigill_count = 0;
        
        /* Increment global limit counter */
        g_mid_init_fault_count++;
        if (g_mid_init_fault_count > MID_INIT_FAULT_LIMIT) {
            fprintf(stderr, "[FATAL] Middle-init SIGILL limit exceeded (%d faults), exiting to avoid infinite loop\n",
                    g_mid_init_fault_count);
            report_window_progress();
            exit(98);
        }
        
        if (mid_sigill_count < 10) {
            fprintf(stderr, "[SKIP] Middle-init SIGILL at RIP=0x%lx (RVA 0x%lx), skipping 2 bytes [fault %d]\n",
                    rip, rip - (u64)g_img, g_mid_init_fault_count);
            mid_sigill_count++;
        }
        /* Skip 2 bytes and continue - don't use crash_pick_fallback_rip to avoid ret-stub redirect */
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)(rip + 2);
        return;
    }

    /* Cycle detection for 0x237cde0-0x23b1400: Game-init and graphics-setup region.
     * This wide range contains multiple initialization sequences with complex control flow.
     * After multiple attempts to fix register state and return addresses, the code still loops.
     * When stuck in the tight 0x237ce59-0x237ce5b loop with NULL return addresses, jump past
     * the entire problematic region instead of trying internal recovery. */
    if (sig == SIGSEGV && rip >= (u64)g_img + 0x237cde0 && rip <= (u64)g_img + 0x23b1400) {
        static u32 sekiro_cycle_faults = 0;
        static u32 ce5x_attempts = 0;  /* Count attempts at the tight 0x237ce59-0x237ce5b loop */
        
        sekiro_cycle_faults++;
        
        /* Detect if we're looping at 0x237ce59-0x237ce5b with NULL fault addresses */
        if (rip >= (u64)g_img + 0x237ce59 && rip <= (u64)g_img + 0x237ce5b && faultaddr == 0) {
            ce5x_attempts++;
            if (ce5x_attempts >= 3) {
                /* We're definitely stuck in the bad-return loop. Jump past the entire region. */
                fprintf(stderr, "[SKIP] Sekiro tight loop at 0x237ce59-0x237ce5b [attempt %d] - jumping past region to 0x237d140\n",
                        ce5x_attempts);
                uc->uc_mcontext.gregs[REG_RAX] = 0;
                uc->uc_mcontext.gregs[REG_RCX] = (greg_t)g_img;
                uc->uc_mcontext.gregs[REG_RIP] = (greg_t)((u64)g_img + 0x237d140);
                return;
            }
        } else {
            ce5x_attempts = 0;  /* Reset counter if we leave the tight loop region */
        }
        
        if (sekiro_cycle_faults > 20) {
            fprintf(stderr, "[FATAL] Sekiro game-init region unbreakable cycle detected (%d faults), exiting to avoid spin\n",
                    sekiro_cycle_faults);
            fprintf(stderr, "        Last fault at RIP=0x%lx RCX=0x%lx RDX=0x%lx R8=0x%lx R14=0x%lx RSP=0x%lx\n",
                    rip, uc->uc_mcontext.gregs[REG_RCX], uc->uc_mcontext.gregs[REG_RDX],
                    uc->uc_mcontext.gregs[REG_R8], uc->uc_mcontext.gregs[REG_R14],
                    uc->uc_mcontext.gregs[REG_RSP]);
            report_window_progress();
            exit(97);
        }
        
        fprintf(stderr, "[SKIP] Sekiro game-init region RIP=0x%lx (+0x%lx), faultaddr=0x%lx [fault %d/20] R14=0x%lx\n",
                rip, rip - (u64)g_img, faultaddr, sekiro_cycle_faults,
                uc->uc_mcontext.gregs[REG_R14]);
        /* Initialize key registers to prevent address overflows and invalid accesses.
         * R14 is constantly corrupted by game code, causing address calculations to overflow to 0.
         * RCX and R14 = 0 makes memory accesses use RCX as base address. */
        uc->uc_mcontext.gregs[REG_RAX] = 0;
        uc->uc_mcontext.gregs[REG_RCX] = (greg_t)g_img;  /* Ensure image base */
        uc->uc_mcontext.gregs[REG_R14] = 0;              /* Clear index to prevent overflow */
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)crash_pick_fallback_rip(rip, "game-init-escape");
        return;
    }

    if (g_guest_stack_low && g_guest_stack_high &&
        rsp0 && (rsp0 < g_guest_stack_low || rsp0 > g_guest_stack_high)) {
        fprintf(stderr,
                "[SKIP] Invalid guest RSP=0x%lx outside guest stack window 0x%lx..0x%lx; resetting to the seeded baseline. "
                "faulting RIP=0x%lx (RVA 0x%lx) faultaddr=0x%lx rcx=0x%lx\n",
                rsp0, g_guest_stack_low, g_guest_stack_high, rip,
                (g_img && rip >= (u64)g_img) ? rip - (u64)g_img : rip, faultaddr, rcx0);
        reset_rsp_to_entry_baseline(uc);
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)guest_resume_rip();
        return;
    }

    /* If the current RSP is already outside the guest stack but the fault is on a
     * real in-image code address, keep the guest on its own stack frame instead of
     * delivering a new host-side loop through the fallback entry path. */
    if (sig == SIGSEGV && g_img && rip >= (u64)g_img && rip < (u64)g_img + 0x42d2000 &&
        rsp0 && (rsp0 < g_guest_stack_low || rsp0 > g_guest_stack_high)) {
        u64 rsp = g_entry_rsp ? g_entry_rsp : rsp0;
        u64 ret = 0, new_rsp = 0;
        if (try_real_unwind_return(rip, rsp, &ret, &new_rsp)) {
            fprintf(stderr,
                    "[SKIP] Guest-stack reset recovery RIP=0x%lx (+0x%lx) -> 0x%lx (frame=0x%lx)\n",
                    rip, rip - (u64)g_img, ret, new_rsp - rsp - 8);
            uc->uc_mcontext.gregs[REG_RSP] = (greg_t)new_rsp;
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)ret;
            return;
        }
    }

    if (getenv("BEER_DEBUG_CRASH")) {
        u64 rsp0 = (u64)uc->uc_mcontext.gregs[REG_RSP];
        u64 *sp0 = (u64 *)rsp0;
        Dl_info dli;
        fprintf(stderr,
                "[DBG] sig=%d rip=0x%lx fault=0x%lx rax=0x%lx rbx=0x%lx rcx=0x%lx rdx=0x%lx "
                "rsi=0x%lx rdi=0x%lx r8=0x%lx r9=0x%lx rsp=0x%lx\n",
                sig, rip, faultaddr,
                (u64)uc->uc_mcontext.gregs[REG_RAX], (u64)uc->uc_mcontext.gregs[REG_RBX],
                rcx0, (u64)uc->uc_mcontext.gregs[REG_RDX],
                (u64)uc->uc_mcontext.gregs[REG_RSI], (u64)uc->uc_mcontext.gregs[REG_RDI],
                (u64)uc->uc_mcontext.gregs[REG_R8], (u64)uc->uc_mcontext.gregs[REG_R9],
                rsp0);
        if (dladdr((void *)rip, &dli) && (dli.dli_fname || dli.dli_sname)) {
            fprintf(stderr, "[DBG]   rip -> %s (%s) fbase=0x%lx fileoff=0x%lx\n",
                    dli.dli_sname ? dli.dli_sname : "?",
                    dli.dli_fname ? dli.dli_fname : "?",
                    (u64)dli.dli_fbase, rip - (u64)dli.dli_fbase);
        }
        if (sp0) {
            for (int k = 0; k < 8; k++) {
                fprintf(stderr, "[DBG]   sp[%d]=0x%lx", k, sp0[k]);
                if (dladdr((void *)sp0[k], &dli) && (dli.dli_fname || dli.dli_sname))
                    fprintf(stderr, "  -> %s (%s) fbase=0x%lx fileoff=0x%lx",
                            dli.dli_sname ? dli.dli_sname : "?",
                            dli.dli_fname ? dli.dli_fname : "?",
                            (u64)dli.dli_fbase, sp0[k] - (u64)dli.dli_fbase);
                fprintf(stderr, "\n");
            }
        }
    }

    /* Primary recovery strategy: compute the REAL return address via
     * .pdata/UNWIND_INFO instead of guessing. Applies to any in-image
     * SIGSEGV where we haven't already special-cased the exact RVA. */
    if (sig == SIGSEGV && g_img && rip >= (u64)g_img && rip < (u64)g_img + 0x42d2000) {
        u64 rsp = (u64)uc->uc_mcontext.gregs[REG_RSP];
        u64 ret = 0, new_rsp = 0;
        if (try_real_unwind_return(rip, rsp, &ret, &new_rsp)) {
            fprintf(stderr,
                    "[SKIP] Real-unwind ret-emu RIP=0x%lx (+0x%lx) -> 0x%lx (frame=0x%lx)\n",
                    rip, rip - (u64)g_img, ret, new_rsp - rsp - 8);
            uc->uc_mcontext.gregs[REG_RAX] = 0;
            uc->uc_mcontext.gregs[REG_RSP] = (greg_t)new_rsp;
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)ret;
            return;
        }
    }

    /* The startup helper window around 0x235a636..0x235a6c5 is a real
     * call/ret epilogue, not a generic crash site. Prefer a validated unwind
     * continuation here instead of immediately bouncing into the safe fail-return
     * stub at 0x235a694. This keeps the loader on the real boot path while still
     * preventing speculative stack walk loops. */
    if (sig == SIGSEGV && g_img &&
        rip >= (u64)g_img + 0x235a636 && rip <= (u64)g_img + 0x235a6c5) {
        u64 rsp = (u64)uc->uc_mcontext.gregs[REG_RSP];
        u64 ret = 0, new_rsp = 0;
        if (try_startup_helper_frame_resume(rip, rsp, &ret, &new_rsp)) {
            fprintf(stderr,
                    "[SKIP] Startup-helper caller frame RIP=0x%lx (+0x%lx) -> 0x%lx (frame=0x%lx)\n",
                    rip, rip - (u64)g_img, ret, new_rsp - rsp - 8);
            uc->uc_mcontext.gregs[REG_RAX] = 0;
            uc->uc_mcontext.gregs[REG_RSP] = (greg_t)new_rsp;
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)ret;
            return;
        }
        if (try_real_unwind_return(rip, rsp, &ret, &new_rsp)) {
            fprintf(stderr,
                    "[SKIP] Startup-helper unwind RIP=0x%lx (+0x%lx) -> 0x%lx (frame=0x%lx)\n",
                    rip, rip - (u64)g_img, ret, new_rsp - rsp - 8);
            uc->uc_mcontext.gregs[REG_RAX] = 0;
            uc->uc_mcontext.gregs[REG_RSP] = (greg_t)new_rsp;
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)ret;
            return;
        }
    }

    if (g_img && rip >= (u64)g_img + 0x2e918a0 && rip <= (u64)g_img + 0x2e918c0) {
        u64 gs_base = 0;
        if (syscall(SYS_arch_prctl, 0x1004 /*ARCH_GET_GS*/, &gs_base) == 0 && gs_base) {
            u64 stack_base = *(u64 *)(uintptr_t)(gs_base + 0x08);
            if (stack_base) {
                fprintf(stderr,
                        "[SKIP] Synthetic crash page RIP=0x%lx (+0x%lx), resetting rsp from gs and redirecting\n",
                        rip, rip - (u64)g_img);
                uc->uc_mcontext.gregs[REG_RAX] = 0;
                uc->uc_mcontext.gregs[REG_RSP] = (greg_t)(stack_base - 0x200);
                uc->uc_mcontext.gregs[REG_RIP] = (greg_t)guest_resume_rip();
                return;
            }
        }
    }

    /* SIGTRAP from int3: likely an "unreachable" guard or DL_PANIC debugger invoke.
     * Skip the int3 instruction itself and restore the guest stack baseline so the
     * resumed path does not continue on the signal-handler's altstack. */
    if (sig == SIGTRAP) {
        reset_rsp_to_entry_baseline(uc);
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)(rip + 1);
        return;
    }

    /* Some execution paths end up in malformed/poisoned helper islands and
     * trigger SIGILL. Treat as a failing helper and continue on known path.
     */
    if (sig == SIGILL && g_img &&
        rip >= (u64)g_img && rip < (u64)g_img + 0x42d2000) {
        fprintf(stderr, "[SKIP] SIGILL at RIP=0x%lx, redirecting\n", rip);
        uc->uc_mcontext.gregs[REG_RAX] = 0;
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)crash_pick_fallback_rip(rip, "sigill");
        return;
    }

    /* Treat SIGBUS the same as an invalid indirect target: pick a known
     * fail-fast continuation so we don't terminate the process.
     */
    if (sig == SIGBUS && g_img) {
        fprintf(stderr, "[SKIP] SIGBUS at RIP=0x%lx, redirecting\n", rip);
        uc->uc_mcontext.gregs[REG_RAX] = 0;
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)crash_pick_fallback_rip(rip, "sigbus");
        return;
    }

    /* Emergency path for null/near-null control transfers: avoid touching
     * stack memory here (it may be invalid) and jump to a known fail-fast
     * continuation site in-image.
     */
    if (sig == SIGSEGV && rip < 0x10 && g_img) {
        u64 rsp = (u64)uc->uc_mcontext.gregs[REG_RSP];
        u64 rsp_page = rsp & ~0xFFFULL;
        if (rsp_page == g_lowrip_last_rsp_page)
            g_lowrip_rsp_hits++;
        else {
            g_lowrip_last_rsp_page = rsp_page;
            g_lowrip_rsp_hits = 1;
        }
        
        if (g_lowrip_rsp_hits == 1) {
            fprintf(stderr, "[DBG] First low-RIP=0x%lx fault: rsp=0x%lx faultaddr=0x%lx rcx=0x%lx rax=0x%lx rdx=0x%lx\n",
                    rip, rsp, faultaddr, 
                    (u64)uc->uc_mcontext.gregs[REG_RCX],
                    (u64)uc->uc_mcontext.gregs[REG_RAX],
                    (u64)uc->uc_mcontext.gregs[REG_RDX]);
        }

        if (g_lowrip_rsp_hits >= 6) {
            u64 *sp = (u64 *)rsp;
            if (sp) {
                u64 cand = sp[0];
                int ok = 0;
                if (cand >= (u64)g_img && cand < (u64)g_img + 0x42d2000)
                    ok = image_addr_is_exec(cand);
                else if (g_tramp && cand >= (u64)g_tramp && cand < (u64)(g_tramp + g_trampsz))
                    ok = 1;
                /* Only ever emulate a single real `ret` (validated target,
                 * one slot). Never scan deeper into the stack guessing
                 * where a plausible return address might be -- that walks
                 * past real frame content into never-written memory. */
                if (ok && (!g_entry_rsp_limit || rsp + 8 <= g_entry_rsp_limit)) {
                    fprintf(stderr,
                            "[SKIP] Low-RIP circuit-breaker RIP=0x%lx -> 0x%lx (ret-emu)\n",
                            rip, cand);
                    uc->uc_mcontext.gregs[REG_RAX] = 0;
                    uc->uc_mcontext.gregs[REG_RSP] = (greg_t)(rsp + 8);
                    uc->uc_mcontext.gregs[REG_RIP] = (greg_t)cand;
                    return;
                }
            }
        }

        {
            u64 target = crash_pick_fallback_rip(rip, "low-rip");
            fprintf(stderr, "[SKIP] Emergency low RIP=0x%lx redirect -> 0x%lx\n", rip, target);
            uc->uc_mcontext.gregs[REG_RAX] = 0;
            reset_rsp_to_entry_baseline(uc);
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)target;
            return;
        }
    }

    /* Any execution target inside the guest stack window but outside the image
     * or the trampoline table is a stale function pointer / return slot, not a
     * valid guest continuation. Re-entering it produces the same dead loop
     * pattern we are seeing in the real startup path (stack-backed RIPs, then
     * RIP=0 once the stack contents are all zeros). Fail fast instead of
     * chasing another synthetic return target. */
    if (sig == SIGSEGV && g_guest_stack_low && g_guest_stack_high &&
        rip >= g_guest_stack_low && rip < g_guest_stack_high &&
        !is_in_image_exec_range(rip) && !is_in_trampoline_range(rip)) {
        if (g_stale_guest_stack_rip == rip) {
            g_stale_guest_stack_hits++;
        } else {
            g_stale_guest_stack_rip = rip;
            g_stale_guest_stack_hits = 1;
        }
        if (g_stale_guest_stack_hits >= 3) {
            fprintf(stderr,
                    "[FATAL] Repeated stale guest-stack RIP loop at 0x%lx inside guest stack window 0x%lx..0x%lx; refusing more recovery and aborting.\n",
                    rip, g_guest_stack_low, g_guest_stack_high);
            fflush(stderr);
            _exit(2);
        }
        fprintf(stderr,
                "[SKIP] Stale guest-stack RIP=0x%lx inside guest stack window 0x%lx..0x%lx; refusing resume and resetting to the seeded return slot.\n",
                rip, g_guest_stack_low, g_guest_stack_high);
        force_safe_fail_return(uc, rip);
        return;
    }

    if (sig == SIGSEGV && g_img && faultaddr == rip && is_host_stack_rip(rip)) {
        u64 rsp = (u64)uc->uc_mcontext.gregs[REG_RSP];
        u64 ret = 0;
        record_host_stack_fault(rip, rsp, faultaddr);
        if (is_repeating_host_stack_loop(rip, rsp, faultaddr)) {
            fprintf(stderr,
                    "[SKIP] Host-stack loop repeat detected at RIP=0x%lx RSP=0x%lx; taking fail-fast path.\n",
                    rip, rsp);
            force_safe_fail_return(uc, rip);
            return;
        }
        if (try_single_slot_stack_resume(rsp, &ret)) {
            fprintf(stderr,
                    "[SKIP] Host-stack fault at RIP=0x%lx -> validated stack ret 0x%lx\n",
                    rip, ret);
            uc->uc_mcontext.gregs[REG_RAX] = 0;
            uc->uc_mcontext.gregs[REG_RSP] = (greg_t)(rsp + 8);
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)ret;
            return;
        }
        if (probe_stack_resume_window(rsp, &ret)) {
            fprintf(stderr,
                    "[SKIP] Host-stack fault at RIP=0x%lx -> shallow stack window candidate 0x%lx\n",
                    rip, ret);
            uc->uc_mcontext.gregs[REG_RAX] = 0;
            uc->uc_mcontext.gregs[REG_RSP] = (greg_t)(rsp + 8);
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)ret;
            return;
        }
        if (g_entry_rsp_limit && rsp >= g_entry_rsp_limit) {
            fprintf(stderr,
                    "[SKIP] Host-stack fault left RSP at/above entry baseline 0x%lx; resetting to baseline.\n",
                    g_entry_rsp);
            uc->uc_mcontext.gregs[REG_RAX] = 0;
            reset_rsp_to_entry_baseline(uc);
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)guest_resume_rip();
            return;
        }
        if (rip >= (u64)g_img + 0x11e41e0 && rip <= (u64)g_img + 0x11e41e8) {
            u64 *sp_diag = (u64 *)rsp;
            u64 caller = (sp_diag && rsp) ? sp_diag[0] : 0;
            fprintf(stderr,
                    "[SKIP] Null-store sentinel host-stack fault at RIP=0x%lx; caller-return(sp[0])=0x%lx (+0x%lx); forcing safe return instead of chasing stale host stack contents.\n",
                    rip, caller,
                    (g_img && caller >= (u64)g_img && caller < (u64)g_img + 0x42d2000) ? caller - (u64)g_img : 0);
            force_safe_fail_return(uc, rip);
            return;
        }
        fprintf(stderr,
                "[SKIP] Host-stack fault RIP=0x%lx under circuit-breaker; forcing safe fail-return.\n",
                rip);
        force_safe_fail_return(uc, rip);
        return;
    }

    if (sig == SIGSEGV && g_img && faultaddr != 0 && faultaddr < 0x1000 &&
        !(rip >= (u64)g_img && rip < (u64)g_img + 0x42d2000) &&
        !(g_tramp && rip >= (u64)g_tramp && rip < (u64)(g_tramp + g_trampsz))) {
        u64 rsp = (u64)uc->uc_mcontext.gregs[REG_RSP];
        if (rsp) {
            u64 *sp = (u64 *)rsp;
            if (sp) {
                u64 value = sp[0];
                record_bad_stack_operand(value);
                if (is_bad_stack_operand_repeat(value)) {
                    fprintf(stderr,
                            "[SKIP] Repeated near-null stack operand 0x%lx on RIP=0x%lx; fail-fast reset.\n",
                            value, rip);
                    force_safe_fail_return(uc, rip);
                    return;
                }
            }
        }
    }

    if (sig == SIGSEGV && g_img && faultaddr == 0 &&
        rip >= (u64)g_img + 0x11e41e0 && rip <= (u64)g_img + 0x11e41e8) {
        u64 rsp_diag = (u64)uc->uc_mcontext.gregs[REG_RSP];
        u64 *sp_diag = (u64 *)rsp_diag;
        u64 caller = sp_diag ? sp_diag[0] : 0;
        fprintf(stderr,
                "[SKIP] Dead helper null-store at RIP=0x%lx (+0x%lx), fault=0x%lx; caller-return(sp[0])=0x%lx (+0x%lx) rcx=0x%lx rdx=0x%lx r8=0x%lx r9=0x%lx; resetting to the safe bootstrap continuation instead of returning through stale stack data.\n",
                rip, rip - (u64)g_img, faultaddr, caller,
                (g_img && caller >= (u64)g_img && caller < (u64)g_img + 0x42d2000) ? caller - (u64)g_img : 0,
                (u64)uc->uc_mcontext.gregs[REG_RCX],
                (u64)uc->uc_mcontext.gregs[REG_RDX],
                (u64)uc->uc_mcontext.gregs[REG_R8],
                (u64)uc->uc_mcontext.gregs[REG_R9]);
        uc->uc_mcontext.gregs[REG_RAX] = 0;
        reset_rsp_to_entry_baseline(uc);
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)guest_resume_rip();
        return;
    }

    if (sig == SIGSEGV && g_img &&
        rip >= (u64)g_img + 0x23ab4e0 && rip <= (u64)g_img + 0x23ab510) {
        fprintf(stderr,
                "[SKIP] Dispatch block (any fault) at RIP=0x%lx (+0x%lx), redirecting\n",
                rip, rip - (u64)g_img);
        uc->uc_mcontext.gregs[REG_RAX] = 0;
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)crash_pick_fallback_rip(rip, "null-dispatch");
        return;
    }

    if (sig == SIGSEGV && g_img &&
        rip >= (u64)g_img + 0x239c620 && rip <= (u64)g_img + 0x239c650) {
        fprintf(stderr,
                "[SKIP] Alt2 trap block at RIP=0x%lx (+0x%lx), redirecting\n",
                rip, rip - (u64)g_img);
        uc->uc_mcontext.gregs[REG_RAX] = 0;
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)crash_pick_fallback_rip(rip, "null-alt2");
        return;
    }

    if (sig == SIGSEGV && g_img &&
        rip >= (u64)g_img + 0x23ba880 && rip <= (u64)g_img + 0x23ba8d0) {
        fprintf(stderr,
                "[SKIP] 23ba8 trap block at RIP=0x%lx (+0x%lx), redirecting\n",
                rip, rip - (u64)g_img);
        uc->uc_mcontext.gregs[REG_RAX] = 0;
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)crash_pick_fallback_rip(rip, "null-23ba8");
        return;
    }

    if (sig == SIGSEGV && g_img &&
        rip >= (u64)g_img + 0x1a05590 && rip <= (u64)g_img + 0x1a05620) {
        fprintf(stderr,
                "[SKIP] 1a055 trap block at RIP=0x%lx (+0x%lx), redirecting\n",
                rip, rip - (u64)g_img);
        uc->uc_mcontext.gregs[REG_RAX] = 0;
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)crash_pick_fallback_rip(rip, "null-1a055");
        return;
    }

    if (sig == SIGSEGV && g_img &&
        rip >= (u64)g_img + 0x19e27e0 && rip <= (u64)g_img + 0x19e2860) {
        fprintf(stderr,
                "[SKIP] 19e28 trap block at RIP=0x%lx (+0x%lx), redirecting\n",
                rip, rip - (u64)g_img);
        uc->uc_mcontext.gregs[REG_RAX] = 0;
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)crash_pick_fallback_rip(rip, "null-19e28");
        return;
    }

    if (sig == SIGSEGV && g_img &&
        rip >= (u64)g_img + 0x23ae170 && rip <= (u64)g_img + 0x23ae1d0) {
        u64 rsp = (u64)uc->uc_mcontext.gregs[REG_RSP];
        u64 *sp = (u64 *)rsp;
        if (sp) {
            u64 cand = sp[0];
            int ok = 0;
            if (cand >= (u64)g_img && cand < (u64)g_img + 0x42d2000) {
                if (image_addr_is_exec(cand) &&
                    !(cand >= (u64)g_img + 0x23ae170 && cand <= (u64)g_img + 0x23ae1d0))
                    ok = 1;
            } else if (g_tramp && cand >= (u64)g_tramp && cand < (u64)(g_tramp + g_trampsz)) {
                ok = 1;
            }
            if (ok && (!g_entry_rsp_limit || rsp + 8 <= g_entry_rsp_limit)) {
                fprintf(stderr,
                        "[SKIP] 23ae1 trap block at RIP=0x%lx (+0x%lx), ret-emu -> 0x%lx\n",
                        rip, rip - (u64)g_img, cand);
                uc->uc_mcontext.gregs[REG_RAX] = 0;
                uc->uc_mcontext.gregs[REG_RSP] = (greg_t)(rsp + 8);
                uc->uc_mcontext.gregs[REG_RIP] = (greg_t)cand;
                return;
            }
        }
        fprintf(stderr,
                "[SKIP] 23ae1 trap block at RIP=0x%lx (+0x%lx), redirecting\n",
                rip, rip - (u64)g_img);
        uc->uc_mcontext.gregs[REG_RAX] = 0;
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)crash_pick_fallback_rip(rip, "null-posttxn");
        return;
    }

    if (sig == SIGSEGV && g_img &&
        rip >= (u64)g_img + 0x23b7000 && rip <= (u64)g_img + 0x23b70c0) {
        u64 rsp = (u64)uc->uc_mcontext.gregs[REG_RSP];
        u64 *sp = (u64 *)rsp;
        if (sp) {
            u64 ret = sp[0];
            if (ret > 0x1000 && (!g_entry_rsp_limit || rsp + 8 <= g_entry_rsp_limit)) {
                fprintf(stderr,
                        "[SKIP] 23b70 trap block at RIP=0x%lx (+0x%lx), ret-emu -> 0x%lx\n",
                        rip, rip - (u64)g_img, ret);
                uc->uc_mcontext.gregs[REG_RAX] = 0;
                uc->uc_mcontext.gregs[REG_RSP] = (greg_t)(rsp + 8);
                uc->uc_mcontext.gregs[REG_RIP] = (greg_t)ret;
                return;
            }
        }
        fprintf(stderr,
                "[SKIP] 23b70 trap block at RIP=0x%lx (+0x%lx), redirecting\n",
                rip, rip - (u64)g_img);
        uc->uc_mcontext.gregs[REG_RAX] = 0;
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)crash_pick_fallback_rip(rip, "null-23b70");
        return;
    }

    if (sig == SIGSEGV && g_img &&
        rip >= (u64)g_img + 0x23bb640 && rip <= (u64)g_img + 0x23bb6d0) {
        fprintf(stderr,
                "[SKIP] 23bb6 trap block at RIP=0x%lx (+0x%lx), redirecting\n",
                rip, rip - (u64)g_img);
        uc->uc_mcontext.gregs[REG_RAX] = 0;
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)crash_pick_fallback_rip(rip, "null-23bb6");
        return;
    }

    if (sig == SIGSEGV && g_img &&
        rip >= (u64)g_img + 0x23adf00 && rip <= (u64)g_img + 0x23ae04f) {
        fprintf(stderr,
                "[SKIP] 23adf/23ae0 front block at RIP=0x%lx (+0x%lx), redirecting\n",
                rip, rip - (u64)g_img);
        uc->uc_mcontext.gregs[REG_RAX] = 0;
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)crash_pick_fallback_rip(rip, "null-txn");
        return;
    }

    if (sig == SIGSEGV && g_img &&
        rip >= (u64)g_img + 0x1a66000 && rip <= (u64)g_img + 0x1a66013) {
        u64 rsp = (u64)uc->uc_mcontext.gregs[REG_RSP];
        u64 *sp = (u64 *)rsp;
        if (sp) {
            u64 ret = sp[0];
            int ok = 0;
            if (ret >= (u64)g_img && ret < (u64)g_img + 0x42d2000)
                ok = image_addr_is_exec(ret);
            else if (g_tramp && ret >= (u64)g_tramp && ret < (u64)(g_tramp + g_trampsz))
                ok = 1;
            if (ok && (!g_entry_rsp_limit || rsp + 8 <= g_entry_rsp_limit)) {
                fprintf(stderr,
                        "[SKIP] 1a6600x helper fault at RIP=0x%lx (+0x%lx), ret-emu -> 0x%lx\n",
                        rip, rip - (u64)g_img, ret);
                uc->uc_mcontext.gregs[REG_RAX] = 0;
                uc->uc_mcontext.gregs[REG_RSP] = (greg_t)(rsp + 8);
                uc->uc_mcontext.gregs[REG_RIP] = (greg_t)ret;
                return;
            }
        }
        fprintf(stderr,
                "[SKIP] 1a6600x helper fault at RIP=0x%lx (+0x%lx), redirecting\n",
                rip, rip - (u64)g_img);
        uc->uc_mcontext.gregs[REG_RAX] = 0;
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)guest_resume_rip();
        return;
    }

    if (sig == SIGSEGV && g_img &&
        rip >= (u64)g_img + 0x2397000 && rip <= (u64)g_img + 0x2397030) {
        u64 rsp = (u64)uc->uc_mcontext.gregs[REG_RSP];
        u64 *sp = (u64 *)rsp;
        if (sp) {
            u64 ret = sp[0];
            int ok = 0;
            if (ret >= (u64)g_img && ret < (u64)g_img + 0x42d2000)
                ok = image_addr_is_exec(ret);
            else if (g_tramp && ret >= (u64)g_tramp && ret < (u64)(g_tramp + g_trampsz))
                ok = 1;
            if (ok && (!g_entry_rsp_limit || rsp + 8 <= g_entry_rsp_limit)) {
                fprintf(stderr,
                        "[SKIP] 239700x helper fault at RIP=0x%lx (+0x%lx), ret-emu -> 0x%lx\n",
                        rip, rip - (u64)g_img, ret);
                uc->uc_mcontext.gregs[REG_RAX] = 0;
                uc->uc_mcontext.gregs[REG_RSP] = (greg_t)(rsp + 8);
                uc->uc_mcontext.gregs[REG_RIP] = (greg_t)ret;
                return;
            }
        }
        fprintf(stderr,
                "[SKIP] 239700x helper fault at RIP=0x%lx (+0x%lx), redirecting\n",
                rip, rip - (u64)g_img);
        uc->uc_mcontext.gregs[REG_RAX] = 0;
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)crash_pick_fallback_rip(rip, "null-23970");
        return;
    }

    if (sig == SIGSEGV && g_img &&
        rip >= (u64)g_img + 0x239e000 && rip <= (u64)g_img + 0x239e030) {
        u64 rsp = (u64)uc->uc_mcontext.gregs[REG_RSP];
        u64 *sp = (u64 *)rsp;
        if (sp) {
            u64 ret = sp[0];
            int ok = 0;
            if (ret >= (u64)g_img && ret < (u64)g_img + 0x42d2000)
                ok = image_addr_is_exec(ret);
            else if (g_tramp && ret >= (u64)g_tramp && ret < (u64)(g_tramp + g_trampsz))
                ok = 1;
            if (ok && (!g_entry_rsp_limit || rsp + 8 <= g_entry_rsp_limit)) {
                fprintf(stderr,
                        "[SKIP] 239e00x helper fault at RIP=0x%lx (+0x%lx), ret-emu -> 0x%lx\n",
                        rip, rip - (u64)g_img, ret);
                uc->uc_mcontext.gregs[REG_RAX] = 0;
                uc->uc_mcontext.gregs[REG_RSP] = (greg_t)(rsp + 8);
                uc->uc_mcontext.gregs[REG_RIP] = (greg_t)ret;
                return;
            }
        }
        fprintf(stderr,
                "[SKIP] 239e00x helper fault at RIP=0x%lx (+0x%lx), redirecting\n",
                rip, rip - (u64)g_img);
        uc->uc_mcontext.gregs[REG_RAX] = 0;
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)crash_pick_fallback_rip(rip, "null-239e0");
        return;
    }

    if (sig == SIGSEGV && g_img &&
        ((rip >= (u64)g_img + 0x235a660 && rip <= (u64)g_img + 0x235a693) ||
         (rip >= (u64)g_img + 0x235a698 && rip <= (u64)g_img + 0x235a6c5))) {
        u64 rsp = (u64)uc->uc_mcontext.gregs[REG_RSP];
        u64 *sp = (u64 *)rsp;
        if (sp) {
            u64 ret = sp[0];
            int ok = 0;
            if (ret >= (u64)g_img && ret < (u64)g_img + 0x42d2000)
                ok = image_addr_is_exec(ret);
            else if (g_tramp && ret >= (u64)g_tramp && ret < (u64)(g_tramp + g_trampsz))
                ok = 1;
            if (ok && (!g_entry_rsp_limit || rsp + 8 <= g_entry_rsp_limit)) {
                fprintf(stderr,
                        "[SKIP] 235a66x helper fault at RIP=0x%lx (+0x%lx), ret-emu -> 0x%lx\n",
                        rip, rip - (u64)g_img, ret);
                uc->uc_mcontext.gregs[REG_RAX] = 0;
                uc->uc_mcontext.gregs[REG_RSP] = (greg_t)(rsp + 8);
                uc->uc_mcontext.gregs[REG_RIP] = (greg_t)ret;
                return;
            }
        }
        fprintf(stderr,
                "[SKIP] 235a66x helper fault at RIP=0x%lx (+0x%lx), redirecting\n",
                rip, rip - (u64)g_img);
        uc->uc_mcontext.gregs[REG_RAX] = 0;
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)guest_resume_rip();
        return;
    }

    if (sig == SIGSEGV && g_img &&
        rip >= (u64)g_img + 0x235a694 && rip <= (u64)g_img + 0x235a697) {
        u64 rsp = (u64)uc->uc_mcontext.gregs[REG_RSP];
        u64 *sp = (u64 *)rsp;
        static int ret_stub_local_attempts = 0;
        
        /* Increment local attempt counter within this fault sequence */
        ret_stub_local_attempts++;
        
        /* Limit retry attempts at this ret-stub to avoid infinite cycles.
         * If we've already tried once without success, reset to baseline.
         * But if we've already reset twice, give up entirely. */
        if (ret_stub_local_attempts >= 2) {
            if (g_ret_stub_baseline_resets >= 2) {
                fprintf(stderr,
                        "[FATAL] 235a694 ret-stub: Already reset %d times, cannot escape cycle. Exiting.\n",
                        g_ret_stub_baseline_resets);
                report_window_progress();
                exit(99);  /* Exit with special code to distinguish from normal exit */
            }
            fprintf(stderr,
                    "[SKIP] 235a694 ret-stub: Local attempt %d - resetting to entry baseline (reset #%d)\n",
                    ret_stub_local_attempts, g_ret_stub_baseline_resets + 1);
            g_ret_stub_baseline_resets++;
            ret_stub_local_attempts = 0;
            reset_rsp_to_entry_baseline(uc);
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)guest_resume_rip();
            return;
        }
        
        if (sp) {
            u64 ret = sp[0];
            int ok = 0;
            if (ret >= (u64)g_img && ret < (u64)g_img + 0x42d2000)
                ok = image_addr_is_exec(ret);
            else if (g_tramp && ret >= (u64)g_tramp && ret < (u64)(g_tramp + g_trampsz))
                ok = 1;
            if (ok && !(ret >= (u64)g_img + 0x235a636 && ret <= (u64)g_img + 0x235a6c5) &&
                (!g_entry_rsp_limit || rsp + 8 <= g_entry_rsp_limit)) {
                fprintf(stderr,
                        "[SKIP] 235a694 ret-stub fault at RIP=0x%lx (+0x%lx), ret-emu -> 0x%lx\n",
                        rip, rip - (u64)g_img, ret);
                uc->uc_mcontext.gregs[REG_RAX] = 0;
                uc->uc_mcontext.gregs[REG_RSP] = (greg_t)(rsp + 8);
                uc->uc_mcontext.gregs[REG_RIP] = (greg_t)ret;
                ret_stub_local_attempts = 0;
                g_ret_stub_baseline_resets = 0;  /* Reset global counter on successful recovery */
                return;
            }
        }
        fprintf(stderr,
                "[SKIP] 235a694 ret-stub fault at RIP=0x%lx (+0x%lx), attempting fallback\n",
                rip, rip - (u64)g_img);
        uc->uc_mcontext.gregs[REG_RAX] = 0;
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)crash_pick_fallback_rip(rip, "ret-stub-235a694");
        return;
    }

    if (sig == SIGSEGV && g_img &&
        rip >= (u64)g_img + 0x235a650 && rip <= (u64)g_img + 0x235a65f) {
        u64 rsp = (u64)uc->uc_mcontext.gregs[REG_RSP];
        u64 *sp = (u64 *)rsp;
        if (sp) {
            u64 ret = sp[0];
            int ok = 0;
            if (ret >= (u64)g_img && ret < (u64)g_img + 0x42d2000)
                ok = image_addr_is_exec(ret);
            else if (g_tramp && ret >= (u64)g_tramp && ret < (u64)(g_tramp + g_trampsz))
                ok = 1;
            if (ok && !(ret >= (u64)g_img + 0x235a636 && ret <= (u64)g_img + 0x235a6c5) &&
                (!g_entry_rsp_limit || rsp + 8 <= g_entry_rsp_limit)) {
                fprintf(stderr,
                        "[SKIP] 235a65x epilogue fault at RIP=0x%lx (+0x%lx), ret-emu -> 0x%lx\n",
                        rip, rip - (u64)g_img, ret);
                uc->uc_mcontext.gregs[REG_RAX] = 0;
                uc->uc_mcontext.gregs[REG_RSP] = (greg_t)(rsp + 8);
                uc->uc_mcontext.gregs[REG_RIP] = (greg_t)ret;
                return;
            }
        }
        fprintf(stderr,
                "[SKIP] 235a65x epilogue fault at RIP=0x%lx (+0x%lx), redirecting\n",
                rip, rip - (u64)g_img);
        uc->uc_mcontext.gregs[REG_RAX] = 0;
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)guest_resume_rip();
        return;
    }

    if (sig == SIGSEGV && g_img &&
        rip >= (u64)g_img + 0x235a636 && rip <= (u64)g_img + 0x235a654) {
        u64 rsp = (u64)uc->uc_mcontext.gregs[REG_RSP];
        u64 *frame = (u64 *)rsp;
        if (frame) {
            u64 ret = frame[0x5c8 / 8];
            int ok = 0;
            if (ret >= (u64)g_img && ret < (u64)g_img + 0x42d2000)
                ok = image_addr_is_exec(ret);
            else if (g_tramp && ret >= (u64)g_tramp && ret < (u64)(g_tramp + g_trampsz))
                ok = 1;
            if (ok && (!g_entry_rsp_limit || rsp + 0x5d0 <= g_entry_rsp_limit)) {
                fprintf(stderr,
                        "[SKIP] 235a63x epilogue fault at RIP=0x%lx (+0x%lx), epilogue-emu -> 0x%lx\n",
                        rip, rip - (u64)g_img, ret);
                uc->uc_mcontext.gregs[REG_RAX] = 0;
                uc->uc_mcontext.gregs[REG_RSP] = (greg_t)(rsp + 0x5d0);
                uc->uc_mcontext.gregs[REG_RIP] = (greg_t)ret;
                return;
            }
        }
        fprintf(stderr,
                "[SKIP] 235a63x epilogue fault at RIP=0x%lx (+0x%lx), redirecting\n",
                rip, rip - (u64)g_img);
        uc->uc_mcontext.gregs[REG_RAX] = 0;
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)guest_resume_rip();
        return;
    }

    if (sig == SIGSEGV && rip == 0x155555554ULL && g_img) {
        u64 rsp = (u64)uc->uc_mcontext.gregs[REG_RSP];
        fprintf(stderr,
                "[SKIP] Sentinel execute-fault RIP=0x%lx, redirecting to guest resume\n",
                rip);
        uc->uc_mcontext.gregs[REG_RAX] = 0;
        if (!g_entry_rsp_limit || rsp + 8 <= g_entry_rsp_limit)
            uc->uc_mcontext.gregs[REG_RSP] = (greg_t)(rsp + 8);
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)guest_resume_rip();
        return;
    }

    /* Execute-fault sentinels: RIP itself is the faulting address and points
     * outside known executable ranges. Route through fallback immediately.
     */
    if (sig == SIGSEGV && g_img && faultaddr == rip &&
        !(rip >= (u64)g_img && rip < (u64)g_img + 0x42d2000) &&
        !(g_tramp && rip >= (u64)g_tramp && rip < (u64)(g_tramp + g_trampsz))) {
        u64 rsp = (u64)uc->uc_mcontext.gregs[REG_RSP];
        u64 *sp = (u64 *)rsp;
        u64 stack_target = guest_resume_rip();

        if (rip == g_exec_last_rip)
            g_exec_rip_hits++;
        else {
            g_exec_last_rip = rip;
            g_exec_rip_hits = 1;
        }

        {
            u64 ebucket = rip & ~0xFFFULL;
            if (ebucket == g_exec_last_bucket)
                g_exec_bucket_hits++;
            else {
                g_exec_last_bucket = ebucket;
                g_exec_bucket_hits = 1;
            }
        }

        if (rip >= 0x7ff000000000ULL && rip < 0x800000000000ULL &&
            faultaddr == rip && rsp) {
            if (g_stack_exec_rip == rip && g_stack_exec_rsp == rsp)
                g_stack_exec_hits++;
            else {
                g_stack_exec_rip = rip;
                g_stack_exec_rsp = rsp;
                g_stack_exec_hits = 1;
            }
            if (g_stack_exec_hits >= 2) {
                fprintf(stderr,
                        "[SKIP] Repeated host-stack execute-fault loop at RIP=0x%lx RSP=0x%lx; forcing safe fail-return before another redirect.\n",
                        rip, rsp);
                force_safe_fail_return(uc, rip);
                return;
            }
        } else {
            g_stack_exec_rip = 0;
            g_stack_exec_rsp = 0;
            g_stack_exec_hits = 0;
        }

        if (g_entry_rsp_limit && rsp > g_entry_rsp_limit) {
            fprintf(stderr,
                   "[SAFE] Host-stack execute-fault while RSP=0x%lx is above entry baseline 0x%lx; refusing guest resume and resetting to the seeded return slot instead of re-entering the host stack.\n",
                   rsp, g_entry_rsp);
            uc->uc_mcontext.gregs[REG_RAX] = 0;
            reset_rsp_to_entry_baseline(uc);
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)guest_resume_rip();
            return;
        }

        if (sp) {
            /* This single-slot ret-emu check used to be gated to
             * rip in [0x7ff0..0000, 0x8000..0000) (the classic Linux user
             * stack address range), which meant an execute-fault where RIP
             * is some other obviously-bogus sentinel (observed: an
             * INVALID_HANDLE_VALUE-style 0xffffffffffffffff used as a
             * function pointer) never even got a chance at this real,
             * validated single-slot recovery and fell straight through to
             * the generic crash_pick_fallback_rip() redirect, discarding a
             * perfectly good return address and destabilizing RSP for
             * later frames. The ret0 validation itself (in-image-exec or
             * in-trampoline, and bounded by g_entry_rsp) is what makes this
             * safe, not the RIP range, so apply it unconditionally here. */
            u64 ret0 = sp[0];
            if (rip == 0x40490fdb) {
                /* Log register values at π-fault time */
                u64 rax = (u64)uc->uc_mcontext.gregs[REG_RAX];
                u64 rbx = (u64)uc->uc_mcontext.gregs[REG_RBX];
                u64 rcx = (u64)uc->uc_mcontext.gregs[REG_RCX];
                u64 rdx = (u64)uc->uc_mcontext.gregs[REG_RDX];
                u64 rsi = (u64)uc->uc_mcontext.gregs[REG_RSI];
                u64 rdi = (u64)uc->uc_mcontext.gregs[REG_RDI];
                u64 rbp = (u64)uc->uc_mcontext.gregs[REG_RBP];
                
                fprintf(stderr,
                       "[DBG] π-fault case: RIP=0x%lx RSP=0x%lx\n"
                       "[DBG]   sp[0]=0x%lx (+0x%lx) sp[1]=0x%lx (+0x%lx) sp[2]=0x%lx (+0x%lx) sp[3]=0x%lx\n"
                       "[DBG]   Registers: RAX=0x%lx RBX=0x%lx RCX=0x%lx RDX=0x%lx RSI=0x%lx RDI=0x%lx RBP=0x%lx\n",
                       rip, rsp,
                       sp[0], sp[0] >= (u64)g_img ? sp[0] - (u64)g_img : 0,
                       sp[1], sp[1] >= (u64)g_img ? sp[1] - (u64)g_img : 0,
                       sp[2], sp[2] >= (u64)g_img ? sp[2] - (u64)g_img : 0,
                       sp[3],
                       rax, rbx, rcx, rdx, rsi, rdi, rbp);
                fprintf(stderr,
                       "[DBG]   ret0-validation: in_image_exec=%d, in_tramp=%d, entry_rsp_limit_ok=%d\n"
                       "[DBG]   is_known_guest_loop_island(ret0)=%d\n",
                       (int)(ret0 >= (u64)g_img && ret0 < (u64)g_img + 0x42d2000 && image_addr_is_exec(ret0)),
                       (int)(g_tramp && ret0 >= (u64)g_tramp && ret0 < (u64)(g_tramp + g_trampsz)),
                       (int)(!g_entry_rsp_limit || rsp + 8 <= g_entry_rsp_limit),
                       (int)is_known_guest_loop_island(ret0));
                /* Also log what try_real_unwind_return would give us for this same RIP */
                u64 unwind_ret = 0, unwind_rsp = 0;
                if (try_real_unwind_return(rip, rsp, &unwind_ret, &unwind_rsp)) {
                    fprintf(stderr,
                           "[DBG]   try_real_unwind_return: ret=0x%lx (+0x%lx), new_rsp=0x%lx (frame=0x%lx)\n",
                           unwind_ret, unwind_ret - (u64)g_img, unwind_rsp, unwind_rsp - rsp - 8);
                } else {
                    fprintf(stderr, "[DBG]   try_real_unwind_return: FAILED\n");
                }
            }
            if (((ret0 >= (u64)g_img && ret0 < (u64)g_img + 0x42d2000 && image_addr_is_exec(ret0)) ||
                 (g_tramp && ret0 >= (u64)g_tramp && ret0 < (u64)(g_tramp + g_trampsz))) &&
                !(ret0 >= (u64)g_img + 0x23b7a20 && ret0 <= (u64)g_img + 0x23b7ac0) &&
                (!g_entry_rsp_limit || rsp + 8 <= g_entry_rsp_limit)) {
                fprintf(stderr,
                       "[SKIP] Execute-fault RIP=0x%lx -> ret-emu 0x%lx\n",
                       rip, ret0);
                uc->uc_mcontext.gregs[REG_RAX] = 0;
                uc->uc_mcontext.gregs[REG_RSP] = (greg_t)(rsp + 8);
                uc->uc_mcontext.gregs[REG_RIP] = (greg_t)ret0;
                return;
            }
        }
        /* Do NOT scan further into the stack guessing at a continuation
         * point (that's how RSP walked into never-written memory before).
         * If the single-slot ret-emu above didn't validate, only ever
         * redirect RIP -- never touch RSP speculatively. */
        if (g_exec_bucket_hits > 6 && rip >= 0x7ff000000000ULL && rip < 0x800000000000ULL) {
            if (g_exec_bucket_hits <= 16 || (g_exec_bucket_hits % 4096) == 0) {
                fprintf(stderr,
                        "[SKIP] Execute-fault RIP=0x%lx persistent stack page (hit=%u), forcing 0x%lx (RIP-only)\n",
                        rip, g_exec_bucket_hits, stack_target);
            }
            if (g_exec_bucket_hits > 8 || g_cycle_total_calls > 60) {
                fprintf(stderr,
                        "[FATAL] Persistent host-stack execute-fault loop at RIP=0x%lx, bucket=0x%lx, hits=%u, total=%lu. Aborting to avoid infinite dead-stack recursion.\n",
                        rip, rip & ~0xFFFULL, g_exec_bucket_hits, (unsigned long)g_cycle_total_calls);
                fflush(stderr);
                _exit(2);
            }
            uc->uc_mcontext.gregs[REG_RAX] = 0;
            reset_rsp_to_entry_baseline(uc);
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)stack_target;
            return;
        }

        if (rip >= 0x7ff000000000ULL && rip < 0x800000000000ULL) {
            g_exec_stack_redirects++;
            if (g_exec_stack_redirects <= 16 || (g_exec_stack_redirects % 4096) == 0) {
                fprintf(stderr,
                        "[SKIP] Execute-fault RIP=0x%lx stack-fallback -> 0x%lx (n=%u, RIP-only)\n",
                        rip, stack_target, g_exec_stack_redirects);
            }
            if (g_exec_stack_redirects > 8 || g_cycle_total_calls > 60) {
                fprintf(stderr,
                        "[FATAL] Host-stack execute-fault redirect loop at RIP=0x%lx, redirects=%u, total=%lu. Aborting to avoid dead-stack recursion.\n",
                        rip, g_exec_stack_redirects, (unsigned long)g_cycle_total_calls);
                fflush(stderr);
                _exit(2);
            }
            uc->uc_mcontext.gregs[REG_RAX] = 0;
            reset_rsp_to_entry_baseline(uc);
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)stack_target;
            return;
        }

        fprintf(stderr,
                "[SKIP] Execute-fault RIP=0x%lx (fault=0x%lx), redirecting\n",
                rip, faultaddr);
        uc->uc_mcontext.gregs[REG_RAX] = 0;
        /* RIP was itself invalid (non-exec / outside known ranges) -- the
         * fallback target below is trusted in-image code, but if %rsp has
         * also drifted off of the guest stack (e.g. still parked inside
         * g_host_call_stack from whatever wild call/ret got us here) that
         * code's own prologue/epilogue will read/write through a bogus
         * stack pointer and corrupt host memory instead of failing safely.
         * Make sure we resume with a sane guest %rsp. */
        ensure_valid_guest_rsp(uc);
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)crash_pick_fallback_rip(rip, "exec-bad");
        return;
    }

    if (sig == SIGSEGV && g_img &&
        rip >= (u64)g_img + 0xbb7e0 && rip <= (u64)g_img + 0xbb7f0) {
        /* Locale-category TLS-cache init helper (img+0xbb750) dereferences the
         * return value of img+0xf52af0 (a TLS-indexed per-thread locale-state
         * accessor) via `movups xmm0,[rax]`. Depending on ASLR-driven heap/stack
         * layout, a bad/uninitialized rax here reads either the classic debug-
         * heap 0xCC poison fill (already caught by is_poisoned_stack_addr below)
         * or an ordinary-looking wild pointer left over from unrelated memory --
         * both stem from the same root cause (TLS-array slot for this module
         * not populated the way the real CRT expects), so recover the same way
         * regardless of which garbage pattern rax happened to hold. */
        fprintf(stderr,
                "[SKIP] Locale-cache TLS deref fault at RIP=0x%lx (+0x%lx) faultaddr=0x%lx rax=0x%lx; resetting guest RSP to seeded return slot\n",
                rip, rip - (u64)g_img, faultaddr, (u64)uc->uc_mcontext.gregs[REG_RAX]);
        uc->uc_mcontext.gregs[REG_RAX] = 0;
        reset_rsp_to_entry_baseline(uc);
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)guest_resume_rip();
        return;
    }

    if (sig == SIGSEGV && g_img && is_poisoned_stack_addr(faultaddr)) {
        fprintf(stderr,
                "[SKIP] Poisoned stack write at faultaddr=0x%lx RIP=0x%lx, resetting guest RSP to seeded return slot\n",
                faultaddr, rip);
        uc->uc_mcontext.gregs[REG_RAX] = 0;
        reset_rsp_to_entry_baseline(uc);
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)guest_resume_rip();
        return;
    }

    if (sig == SIGSEGV && g_img && faultaddr != 0 && faultaddr < 0x1000 &&
        !(rip >= (u64)g_img && rip < (u64)g_img + 0x42d2000) &&
        !(g_tramp && rip >= (u64)g_tramp && rip < (u64)(g_tramp + g_trampsz))) {
        fprintf(stderr,
                "[SKIP] Host near-null RIP=0x%lx (fault=0x%lx), redirecting\n",
                rip, faultaddr);
        uc->uc_mcontext.gregs[REG_RAX] = 0;
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)crash_pick_fallback_rip(rip, "host-nearnull");
        return;
    }

    if (sig == SIGSEGV && g_img &&
        rip >= 0x0000800000000000ULL && rip < 0xFFFF800000000000ULL &&
        !(g_tramp && rip >= (u64)g_tramp && rip < (u64)(g_tramp + g_trampsz))) {
        fprintf(stderr,
                "[SKIP] Non-canonical RIP=0x%lx, redirecting\n",
                rip);
        uc->uc_mcontext.gregs[REG_RAX] = 0;
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)crash_pick_fallback_rip(rip, "rip-noncanon");
        return;
    }

    if (sig == SIGSEGV && g_img &&
        !(rip >= (u64)g_img && rip < (u64)g_img + 0x42d2000) &&
        !(g_tramp && rip >= (u64)g_tramp && rip < (u64)(g_tramp + g_trampsz)) &&
        rip >= 0x100000ULL && rip < 0x700000000000ULL) {
        fprintf(stderr,
                "[SKIP] Mid-host bad target RIP=0x%lx, redirecting\n",
                rip);
        uc->uc_mcontext.gregs[REG_RAX] = 0;
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)crash_pick_fallback_rip(rip, "host-mid");
        return;
    }

    if (sig == SIGSEGV && g_img && faultaddr >= 0xFFFFFFFFFFFFF000ULL &&
        !(rip >= (u64)g_img && rip < (u64)g_img + 0x42d2000) &&
        !(g_tramp && rip >= (u64)g_tramp && rip < (u64)(g_tramp + g_trampsz))) {
        fprintf(stderr,
                "[SKIP] Host signed near-null RIP=0x%lx (fault=0x%lx), redirecting\n",
                rip, faultaddr);
        uc->uc_mcontext.gregs[REG_RAX] = 0;
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)crash_pick_fallback_rip(rip, "host-nearnull");
        return;
    }

    /* Recurring loop cluster around 0x35a500 and null-write CTI at 0x2359b97.
     * Jump to a stable continuation instead of re-entering the loop.
     */
    if (sig == SIGSEGV && g_img &&
        ((rip >= (u64)g_img + 0x35a140 && rip <= (u64)g_img + 0x35a540) ||
         rip == (u64)g_img + 0x2359b97)) {
        fprintf(stderr,
                "[SKIP] Loop-cluster RIP=0x%lx (+0x%lx) -> guest resume\n",
                rip, rip - (u64)g_img);
        uc->uc_mcontext.gregs[REG_RAX] = 0;
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)guest_resume_rip();
        return;
    }

    /* Null indirect call/jump target. Treat as a failed function call and
     * return 0 to caller so startup can continue.
     */
    if (sig == SIGSEGV && rip < 0x1000) {
        u64 rsp = (u64)uc->uc_mcontext.gregs[REG_RSP];
        u64 *sp = (u64 *)rsp;
        if (sp) {
            u64 cand = sp[0];
            int ok = 0;
            if (g_img && cand >= (u64)g_img && cand < (u64)g_img + 0x42d2000)
                ok = image_addr_is_exec(cand);
            else if (g_tramp && cand >= (u64)g_tramp && cand < (u64)(g_tramp + g_trampsz))
                ok = 1;
            /* Single-slot ret-emulation only -- do not scan deeper into the
             * stack guessing at a continuation point. */
            if (ok && (!g_entry_rsp_limit || rsp + 8 <= g_entry_rsp_limit)) {
                fprintf(stderr,
                    "[SKIP] Low call target RIP=0x%lx, ret-emu -> 0x%lx\n",
                    rip, cand);
                uc->uc_mcontext.gregs[REG_RAX] = 0;
                uc->uc_mcontext.gregs[REG_RSP] = (greg_t)(rsp + 8);
                uc->uc_mcontext.gregs[REG_RIP] = (greg_t)cand;
                return;
            }
        }
        fprintf(stderr, "[SKIP] Low call target RIP=0x%lx, redirecting\n", rip);
        uc->uc_mcontext.gregs[REG_RAX] = 0;
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)crash_pick_fallback_rip(rip, "low-deep");
        return;
    }

    /* Known bad branch path can jump to a stack page with sentinel RCX.
     * Redirect to a stable in-image continuation point. */
    if (sig == SIGSEGV && g_img && rcx0 == 0x20002000200020ULL &&
        rip >= 0x700000000000ULL && rip < 0x800000000000ULL) {
        fprintf(stderr, "[SKIP] Stack-jump RIP=0x%lx with sentinel RCX, redirecting\n", rip);
        uc->uc_mcontext.gregs[REG_RAX] = 0;
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)((u64)g_img + 0x23ae05d);
        return;
    }

    /* Generic bad indirect target: RIP points outside image/tramp into a
     * non-executable-looking area (often stack). Try to recover by checking a
     * few stack slots for a valid guest return address before we redirect to a
     * safe fallback. This is intentionally bounded: a small scan of the top of
     * the current frame is still consistent with real Windows call/ret semantics,
     * but never walks the full stack or guesses deep into memory. */
    if (sig == SIGSEGV && g_img &&
        !(rip >= (u64)g_img && rip < (u64)g_img + 0x42d2000) &&
        !(g_tramp && rip >= (u64)g_tramp && rip < (u64)(g_tramp + g_trampsz))) {
        if (rip >= 0x700000000000ULL && rip < 0x800000000000ULL) {
            u64 rsp = (u64)uc->uc_mcontext.gregs[REG_RSP];
            u64 *sp = (u64 *)rsp;
            if (sp) {
                for (int i = 0; i < 8; i++) {
                    u64 cand = sp[i];
                    if (!cand) continue;
                    if (cand >= (u64)g_img && cand < (u64)g_img + 0x42d2000 &&
                        image_addr_is_exec(cand) &&
                        !(cand >= (u64)g_img + 0x235a636 && cand <= (u64)g_img + 0x235a6c5)) {
                        fprintf(stderr,
                                "[SKIP] Host-space bad target RIP=0x%lx -> guest stack resume 0x%lx (slot=%d)\n",
                                rip, cand, i);
                        uc->uc_mcontext.gregs[REG_RAX] = 0;
                        uc->uc_mcontext.gregs[REG_RSP] = (greg_t)(rsp + (i + 1) * 8);
                        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)cand;
                        return;
                    }
                }
            }
            if ((rip & 0xFFFFULL) == 0x6429ULL) {
                fprintf(stderr,
                        "[SKIP] Host-space signature RIP=0x%lx -> RVA 0x35a142\n",
                        rip);
                uc->uc_mcontext.gregs[REG_RAX] = 0;
                uc->uc_mcontext.gregs[REG_RIP] = (greg_t)((u64)g_img + 0x35a142);
                return;
            }
            if ((rip & 0xFFFFULL) == 0x596dULL) {
                fprintf(stderr,
                        "[SKIP] Host-space signature RIP=0x%lx -> RVA 0x235a694\n",
                        rip);
                uc->uc_mcontext.gregs[REG_RAX] = 0;
                    uc->uc_mcontext.gregs[REG_RIP] = (greg_t)((u64)g_img + 0x235a694);
                return;
            }
            if ((rip & 0xFFFFFFFFULL) == 0x00000008ULL) {
                fprintf(stderr,
                        "[SKIP] Host-space signature RIP=0x%lx -> RVA 0x235a694\n",
                        rip);
                uc->uc_mcontext.gregs[REG_RAX] = 0;
                    uc->uc_mcontext.gregs[REG_RIP] = (greg_t)((u64)g_img + 0x235a694);
                return;
            }
            fprintf(stderr,
                    "[SKIP] Host-space bad target RIP=0x%lx, redirecting\n",
                    rip);
            uc->uc_mcontext.gregs[REG_RAX] = 0;
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)crash_pick_fallback_rip(rip, "host-bad");
            return;
        }
        u64 rsp = (u64)uc->uc_mcontext.gregs[REG_RSP];
        u64 *sp = (u64 *)rsp;
        if (sp) {
            u64 cand = sp[0];
            int ok = 0;
            if (g_img && cand >= (u64)g_img && cand < (u64)g_img + 0x42d2000)
                ok = image_addr_is_exec(cand);
            else if (g_tramp && cand >= (u64)g_tramp && cand < (u64)(g_tramp + g_trampsz))
                ok = 1;
            /* Single-slot ret-emulation only -- do not scan deeper into the
             * stack guessing at a continuation point. */
            if (ok && (!g_entry_rsp_limit || rsp + 8 <= g_entry_rsp_limit)) {
                fprintf(stderr,
                        "[SKIP] Bad target RIP=0x%lx, ret-emu -> 0x%lx\n",
                        rip, cand);
                uc->uc_mcontext.gregs[REG_RAX] = 0;
                uc->uc_mcontext.gregs[REG_RSP] = (greg_t)(rsp + 8);
                uc->uc_mcontext.gregs[REG_RIP] = (greg_t)cand;
                return;
            }
            fprintf(stderr, "[SKIP] Bad target RIP=0x%lx, redirecting\n", rip);
            uc->uc_mcontext.gregs[REG_RAX] = 0;
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)crash_pick_fallback_rip(rip, "bad-deep");
            return;
        }
    }

    /* Known hot crash loops in the engine lock/transaction path.
     * When RCX is a sentinel marker instead of a valid object pointer,
     * short-circuit this helper by emulating a failing return.
     */
    if (sig == SIGSEGV && g_img && rcx0 == 0x10001000100010ULL &&
        ((rip >= (u64)g_img + 0x23b7800 && rip <= (u64)g_img + 0x23b79c0) ||
         (rip >= (u64)g_img + 0x23ae060 && rip <= (u64)g_img + 0x23ae0a0))) {
        u64 rsp = (u64)uc->uc_mcontext.gregs[REG_RSP];
        u64 *sp = (u64 *)rsp;
        if (sp) {
            u64 ret = sp[0];
            u64 fallback = (u64)g_img + 0x23ae05d;
            fprintf(stderr,
                    "[SKIP] Bad lock/txn object at RIP=0x%lx (+0x%lx), forcing fail path\n",
                    rip, rip - (u64)g_img);
            uc->uc_mcontext.gregs[REG_RAX] = 0;
            uc->uc_mcontext.gregs[REG_RCX] = 0;
            if (ret >= (u64)g_img && ret < (u64)g_img + 0x42d2000 &&
                (!g_entry_rsp_limit || rsp + 8 <= g_entry_rsp_limit)) {
                uc->uc_mcontext.gregs[REG_RSP] = (greg_t)(rsp + 8);
                uc->uc_mcontext.gregs[REG_RIP] = (greg_t)ret;
            } else {
                uc->uc_mcontext.gregs[REG_RIP] = (greg_t)fallback;
            }
            return;
        }
    }

    /* SIGSEGV with faultAddr == 0 inside our image = intentional crash (DL_PANIC dump).
     * The instruction is: c7 04 25 00 00 00 00 xx xx xx xx (11 bytes, mov [0], imm32).
     * Skip past it so we can observe what happens after DL_PANIC. */
    if (sig == SIGSEGV && faultaddr == 0 &&
        g_img && rip >= (u64)g_img && rip < (u64)g_img + 0x42d2000) {
        if (rip >= (u64)g_img + 0x2359f40 && rip <= (u64)g_img + 0x2359f60) {
            fprintf(stderr,
                    "[SKIP] Null-write guard helper RIP=0x%lx (+0x%lx) treated as a harmless nil-clear; resuming after the guard.\n",
                    rip, rip - (u64)g_img);
            uc->uc_mcontext.gregs[REG_RAX] = 0;
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)(rip + 0x14);
            return;
        }

        if (g_repeated_null_write_rip == rip) {
            g_repeated_null_write_hits++;
        } else {
            g_repeated_null_write_rip = rip;
            g_repeated_null_write_hits = 1;
        }
        if (g_repeated_null_write_hits > 3) {
            fprintf(stderr,
                    "[SKIP] Repeated null-write hotspot RIP=0x%lx (+0x%lx), hit=%u -> fail-fast continuation\n",
                    rip, rip - (u64)g_img, g_repeated_null_write_hits);
            uc->uc_mcontext.gregs[REG_RAX] = 0;
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)((u64)g_img + 0x235a694);
            return;
        }

        if (rip >= (u64)g_img + 0x23b7a20 && rip <= (u64)g_img + 0x23b7ac0) {
            g_hotspot_hits++;
            u64 rsp = (u64)uc->uc_mcontext.gregs[REG_RSP];
            u64 target = (u64)g_img + 0x235a694;
            int popped = 0;
            u64 *sp = (u64 *)rsp;
            /* Single-slot ret-emulation only -- no multi-slot stack scan. */
            if (sp) {
                u64 cand = sp[0];
                int ok = 0;
                if (cand >= (u64)g_img && cand < (u64)g_img + 0x42d2000) {
                    if (image_addr_is_exec(cand) &&
                        !(cand >= (u64)g_img + 0x23b7a20 && cand <= (u64)g_img + 0x23b7ac0))
                        ok = 1;
                } else if (g_tramp && cand >= (u64)g_tramp && cand < (u64)(g_tramp + g_trampsz)) {
                    ok = 1;
                }
                if (ok && (!g_entry_rsp_limit || rsp + 8 <= g_entry_rsp_limit)) {
                    target = cand;
                    popped = 1;
                }
            }
            if (popped) {
                uc->uc_mcontext.gregs[REG_RSP] = (greg_t)(rsp + 8);
            } else if (g_hotspot_hits > 12) {
                target = crash_pick_fallback_rip(rip, "null-hot");
                if (target >= (u64)g_img + 0x23b7a20 && target <= (u64)g_img + 0x23b7ac0)
                    target = (u64)g_img + 0x235a694;
            }
            if (g_hotspot_hits <= 16 || (g_hotspot_hits % 4096) == 0) {
                fprintf(stderr,
                    "[SKIP] Null-write hot island RIP=0x%lx (+0x%lx), hit=%lu -> 0x%lx (popped=%d)\n",
                    rip, rip - (u64)g_img, (unsigned long)g_hotspot_hits, target, popped);
            }
            uc->uc_mcontext.gregs[REG_RAX] = 0;
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)target;
            return;
        }
        if (rip >= (u64)g_img + 0x23ae050 && rip <= (u64)g_img + 0x23ae090) {
            fprintf(stderr,
                    "[SKIP] Null-write txn block at RIP=0x%lx (+0x%lx), redirecting\n",
                    rip, rip - (u64)g_img);
            uc->uc_mcontext.gregs[REG_RAX] = 0;
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)crash_pick_fallback_rip(rip, "null-txn");
            return;
        }
        if (rip >= (u64)g_img + 0x23ae1b0 && rip <= (u64)g_img + 0x23ae1d0) {
            fprintf(stderr,
                "[SKIP] Null-write post-txn block at RIP=0x%lx (+0x%lx), redirecting\n",
                rip, rip - (u64)g_img);
            uc->uc_mcontext.gregs[REG_RAX] = 0;
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)crash_pick_fallback_rip(rip, "null-posttxn");
            return;
        }
        if (rip >= (u64)g_img + 0x23aed40 && rip <= (u64)g_img + 0x23aee80) {
            fprintf(stderr,
                "[SKIP] Null-write mid-txn block at RIP=0x%lx (+0x%lx), redirecting\n",
                rip, rip - (u64)g_img);
            uc->uc_mcontext.gregs[REG_RAX] = 0;
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)crash_pick_fallback_rip(rip, "null-posttxn");
            return;
        }
        if (rip >= (u64)g_img + 0x19fbf80 && rip <= (u64)g_img + 0x19fc080) {
            fprintf(stderr,
                "[SKIP] Null-write late block at RIP=0x%lx (+0x%lx), redirecting\n",
                rip, rip - (u64)g_img);
            uc->uc_mcontext.gregs[REG_RAX] = 0;
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)crash_pick_fallback_rip(rip, "null-late");
            return;
        }
        if (rip >= (u64)g_img + 0x23ab4e0 && rip <= (u64)g_img + 0x23ab510) {
            fprintf(stderr,
                "[SKIP] Null-write dispatch block at RIP=0x%lx (+0x%lx), redirecting\n",
                rip, rip - (u64)g_img);
            uc->uc_mcontext.gregs[REG_RAX] = 0;
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)crash_pick_fallback_rip(rip, "null-dispatch");
            return;
        }
        if (rip >= (u64)g_img + 0x1130b20 && rip <= (u64)g_img + 0x1130b70) {
            fprintf(stderr,
                "[SKIP] Null-write 1130b block at RIP=0x%lx (+0x%lx), redirecting\n",
                rip, rip - (u64)g_img);
            uc->uc_mcontext.gregs[REG_RAX] = 0;
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)crash_pick_fallback_rip(rip, "null-1130b");
            return;
        }
        if (rip >= (u64)g_img + 0x23978b8 && rip <= (u64)g_img + 0x23978c8) {
            g_null_cti_hot_hits++;
            u64 target = (g_null_cti_hot_hits <= 8)
                      ? ((u64)g_img + 0x235a694)
                       : crash_pick_fallback_rip(rip, "null-cti");
            if (target >= (u64)g_img + 0x23978b8 && target <= (u64)g_img + 0x23978c8)
                  target = (u64)g_img + 0x235a694;
            if (g_null_cti_hot_hits <= 16 || (g_null_cti_hot_hits % 4096) == 0) {
                fprintf(stderr,
                    "[SKIP] Null-write CTI hotspot RIP=0x%lx (+0x%lx), hit=%lu -> 0x%lx\n",
                    rip, rip - (u64)g_img, (unsigned long)g_null_cti_hot_hits, target);
            }
            uc->uc_mcontext.gregs[REG_RAX] = 0;
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)target;
            return;
        }
        u8 *instr = (u8 *)rip;
        /* Detect: c7 04 25 00 00 00 00 = 7 bytes prefix + 4 byte imm = 11 total */
        int skip = 7; /* default */
        if (instr[0]==0x0f && instr[1]==0x29 && instr[2]==0x04 && instr[3]==0x24) {
            if (uc->uc_mcontext.fpregs) {
                memcpy((void *)rsp0,
                       uc->uc_mcontext.fpregs->_xmm[0].element,
                       sizeof(uc->uc_mcontext.fpregs->_xmm[0].element));
            }
            skip = 4;
        } else if (instr[0]==0xc7 && instr[1]==0x04 && instr[2]==0x25 &&
            instr[3]==0 && instr[4]==0 && instr[5]==0 && instr[6]==0)
            skip = 11;
        /* lock add dword ptr [rcx+0x10], r9d: f0 44 01 49 10 */
        else if (instr[0]==0xf0 && instr[1]==0x44 && instr[2]==0x01 && instr[3]==0x49 && instr[4]==0x10)
            skip = 5;
        /* lock xadd dword ptr [rcx+disp32], eax: f0 0f c1 81 xx xx xx xx */
        else if (instr[0]==0xf0 && instr[1]==0x0f && instr[2]==0xc1 && instr[3]==0x81)
            skip = 8;

        /* If we fault on an indirect control transfer sequence, advancing
         * by a few bytes tends to land mid-stream and re-crash. Force a
         * fail-fast continuation instead.
         */
        if ((instr[0]==0x48 && instr[1]==0xff && instr[2]==0xe0) ||
            (instr[0]==0xff && instr[1]==0xd7) ||
            (instr[0]==0xff && instr[1]==0x15)) {
            fprintf(stderr,
                    "[SKIP] Null-write CTI at RIP=0x%lx (+0x%lx), redirecting\n",
                    rip, rip - (u64)g_img);
            uc->uc_mcontext.gregs[REG_RAX] = 0;
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)crash_pick_fallback_rip(rip, "null-cti");
            return;
        }
        u64 gs_base = 0;
        syscall(SYS_arch_prctl, 0x1004 /*ARCH_GET_GS*/, &gs_base);
        fprintf(stderr, "[SKIP] Null-write at RIP=0x%lx (+0x%lx), advancing %d bytes, gs_base=0x%lx\n",
                rip, rip - (u64)g_img, skip, gs_base);
        /* Print call stack for diagnosis */
        u64 *sp = (u64 *)(u64)uc->uc_mcontext.gregs[REG_RSP];
        fprintf(stderr, "[PANIC_STACK] RSP=0x%lx  RAX=0x%lx  RBX=0x%lx  RCX=0x%lx  RDX=0x%lx\n",
                (u64)sp,
                (u64)uc->uc_mcontext.gregs[REG_RAX],
                (u64)uc->uc_mcontext.gregs[REG_RBX],
                (u64)uc->uc_mcontext.gregs[REG_RCX],
                (u64)uc->uc_mcontext.gregs[REG_RDX]);
        fprintf(stderr, "[PANIC_STACK] R8=0x%lx  R9=0x%lx  R10=0x%lx  R11=0x%lx\n",
                (u64)uc->uc_mcontext.gregs[REG_R8],
                (u64)uc->uc_mcontext.gregs[REG_R9],
                (u64)uc->uc_mcontext.gregs[REG_R10],
                (u64)uc->uc_mcontext.gregs[REG_R11]);
        /* What does the instruction actually write to? */
        u8 *instr_ptr = (u8 *)rip;
        fprintf(stderr, "[PANIC_STACK] instr bytes: %02x %02x %02x %02x %02x\n",
                instr_ptr[0], instr_ptr[1], instr_ptr[2], instr_ptr[3], instr_ptr[4]);
        for (int _i = 0; _i < 32; _i++) {
            u64 ret = sp[_i];
            if (ret >= (u64)g_img && ret < (u64)g_img + 0x42d2000)
                fprintf(stderr, "[PANIC_STACK]   [%2d] img+0x%lx\n", _i, ret - (u64)g_img);
        }
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)(rip + skip);
        return;
    }

    /*
     * A pair of helper islands in the early bootstrap path do a null-check on
     * a pointer field (e.g. "cmp dword ptr [rcx+0x10], 0" / "cmp dword ptr
     * [rbx+0x10], 0") but still fault if the object pointer is NULL. That is
     * a genuine missing-object condition, not a dead jump: skip to the safe
     * continuation after the check and treat the helper as a failed lookup.
     */
    if (sig == SIGSEGV && faultaddr != 0 && faultaddr < 0x1000 && g_img &&
        ((rip >= (u64)g_img + 0x23ae050 && rip <= (u64)g_img + 0x23ae0a2) ||
         (rip >= (u64)g_img + 0x23b7a20 && rip <= (u64)g_img + 0x23b7a45))) {
        u64 resume = (rip >= (u64)g_img + 0x23ae050 && rip <= (u64)g_img + 0x23ae0a2)
                   ? ((u64)g_img + 0x23ae084)
                   : ((u64)g_img + 0x23b7a3e);
        fprintf(stderr,
                "[SKIP] Null-check helper at RIP=0x%lx (+0x%lx, fault=0x%lx), resuming at 0x%lx\n",
                rip, rip - (u64)g_img, faultaddr, resume);
        uc->uc_mcontext.gregs[REG_RAX] = 0;
        uc->uc_mcontext.gregs[REG_RCX] = 0;
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)resume;
        return;
    }

    /*
     * SIGSEGV with a small non-zero faultAddr (e.g. 0x8) inside our image is
     * an intrusive-linked-list "attach" bug in the game's own code: it
     * null-checks the node pointer (rax) but then computes rcx = rax+8 and
     * null-checks *that* instead of rax, so a NULL node still falls through
     * to `mov [rcx], rdi` and writes to address 0x8.
     * Instruction is: 48 89 39 (REX.W mov [rcx], rdi — 3 bytes, ModRM=0x39
     * means mod=00,reg=111(rdi),rm=001(rcx), no SIB/disp). Skip over it.
     */
    if (sig == SIGSEGV && faultaddr != 0 && faultaddr < 0x1000 &&
        g_img && rip >= (u64)g_img && rip < (u64)g_img + 0x42d2000) {
        u8 *instr = (u8 *)rip;

        if (rip >= (u64)g_img + 0x237cec0 && rip <= (u64)g_img + 0x237d010) {
            fprintf(stderr,
                    "[SKIP] Near-null 237ce-237d0 block (fault=0x%lx) at RIP=0x%lx (+0x%lx), redirecting\n",
                    faultaddr, rip, rip - (u64)g_img);
            uc->uc_mcontext.gregs[REG_RAX] = 0;
            uc->uc_mcontext.gregs[REG_RCX] = 0;
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)crash_pick_fallback_rip(rip, "null-237ce");
            return;
        }

        if (rip >= (u64)g_img + 0x1130b20 && rip <= (u64)g_img + 0x1130b70) {
            fprintf(stderr,
                    "[SKIP] Near-null 1130b block (fault=0x%lx) at RIP=0x%lx (+0x%lx), redirecting\n",
                    faultaddr, rip, rip - (u64)g_img);
            uc->uc_mcontext.gregs[REG_RAX] = 0;
            uc->uc_mcontext.gregs[REG_RCX] = 0;
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)crash_pick_fallback_rip(rip, "null-1130b");
            return;
        }

        /* Known bad path: null destination object in text formatter helper.
         * RVA 0x165d91 instruction is `mov [rbx], r8`. When rbx is near-null,
         * jump to function epilogue so caller receives a null/partial result
         * instead of terminating process startup. */
        if (rip == (u64)g_img + 0x165d91) {
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)((u64)g_img + 0x165dca);
            uc->uc_mcontext.gregs[REG_RAX] = (greg_t)uc->uc_mcontext.gregs[REG_RBX];
            fprintf(stderr, "[SKIP] Null dst at RVA 0x165d91 (fault=0x%lx), "
                    "jumping to epilogue\n", faultaddr);
            return;
        }

        if (instr[0]==0x48 && instr[1]==0x89 && (instr[2] & 0xC0) == 0x00) {
            fprintf(stderr, "[SKIP] Near-null store (fault=0x%lx) at RIP=0x%lx "
                    "(+0x%lx), advancing 3 bytes\n",
                    faultaddr, rip, rip - (u64)g_img);
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)(rip + 3);
            return;
        }
    }

    /* Worker-thread cleanup helper at 0x1a664e0 still faults after the
     * virtual-call sites are neutralized. Treat faults in the hot 0x1a66548..
     * 0x1a66560 window as a failed helper call and return to the caller. */
    if (sig == SIGSEGV && g_img &&
        rip >= (u64)g_img + 0x1a66548 && rip <= (u64)g_img + 0x1a66560) {
        u64 rsp = (u64)uc->uc_mcontext.gregs[REG_RSP];
        u64 *sp = (u64 *)rsp;
        if (sp && sp[0] && (!g_entry_rsp_limit || rsp + 8 <= g_entry_rsp_limit)) {
            uc->uc_mcontext.gregs[REG_RAX] = 0;
            uc->uc_mcontext.gregs[REG_RSP] = (greg_t)(rsp + 8);
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)sp[0];
            fprintf(stderr,
                    "[SKIP] Worker cleanup block fault at RVA 0x%lx, returning to 0x%lx\n",
                    rip - (u64)g_img, sp[0]);
            return;
        }
        uc->uc_mcontext.gregs[REG_RAX] = 0;
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)((u64)g_img + 0x235a694);
        fprintf(stderr,
                "[SKIP] Worker cleanup block fault at RVA 0x%lx, redirecting\n",
                rip - (u64)g_img);
        return;
    }

    /* Bad indirect call/jump into non-executable .data/.rdata inside image.
     * Install a tiny executable stub at that address:
     *   xor eax,eax ; ret
     * Then resume at RIP so the call returns failure cleanly for all threads. */
    if (sig == SIGSEGV && g_img && rip >= (u64)g_img && rip < (u64)g_img + 0x42d2000 &&
        !image_addr_is_exec(rip)) {
        u64 rva = rip - (u64)g_img;
        if ((rva >= 0x3f08bc0 && rva <= 0x3f08bff) ||
            (rva >= 0x3ec7d60 && rva <= 0x3ec7d8f)) {
            fprintf(stderr,
                    "[SKIP] Guarded dynamic patch RVA 0x%lx, redirecting\n",
                    rva);
            uc->uc_mcontext.gregs[REG_RAX] = 0;
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)((u64)g_img + 0x23ab4e6);
            return;
        }
        if (rva < 0x10000) {
            fprintf(stderr, "[SKIP] Refusing patch at low RVA 0x%lx, redirecting\n", rva);
            uc->uc_mcontext.gregs[REG_RAX] = 0;
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)crash_pick_fallback_rip(rip, "low-rva");
            return;
        }
        u64 page = rip & ~(u64)0xFFF;
        if (mprotect((void *)page, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
            u8 *p = (u8 *)rip;
            p[0] = 0x31; p[1] = 0xC0; /* xor eax,eax */
            p[2] = 0xC3;              /* ret */
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)rip;
            fprintf(stderr, "[SKIP] Patched bad indirect target RIP=0x%lx (+0x%lx) "
                    "with 'xor eax,eax; ret'\n", rip, rip - (u64)g_img);
            return;
        }
    }

    u64 rsp = rsp0;
    u64 rax = (u64)uc->uc_mcontext.gregs[REG_RAX];
    u64 rbx = (u64)uc->uc_mcontext.gregs[REG_RBX];
    u64 rcx = (u64)uc->uc_mcontext.gregs[REG_RCX];
    u64 rdx = (u64)uc->uc_mcontext.gregs[REG_RDX];
    u64 r8  = (u64)uc->uc_mcontext.gregs[REG_R8];
    u64 r9  = (u64)uc->uc_mcontext.gregs[REG_R9];

    const char *signame =
        sig == SIGSEGV ? "SIGSEGV" :
        sig == SIGBUS  ? "SIGBUS"  :
        sig == SIGILL  ? "SIGILL"  :
        sig == SIGFPE  ? "SIGFPE"  : "?";

    fprintf(stderr,
        "\n──────────────── CRASH ────────────────\n"
        "  Signal   : %s (%d)\n"
        "  RIP      : 0x%lx",
        signame, sig, rip);

    if (g_img && rip >= (u64)g_img)
        fprintf(stderr, "  (+0x%lx from image base)", rip - (u64)g_img);
    fprintf(stderr,
        "\n"
        "  RSP      : 0x%lx\n"
        "  RAX      : 0x%lx   RBX : 0x%lx\n"
        "  RCX      : 0x%lx   RDX : 0x%lx\n"
        "  R8       : 0x%lx   R9  : 0x%lx\n"
        "  FaultAddr: 0x%lx\n",
        rsp, rax, rbx, rcx, rdx, r8, r9, (u64)si->si_addr);

    if (g_tramp && rip >= (u64)g_tramp && rip < (u64)(g_tramp + g_trampsz)) {
        int idx = (int)((rip - (u64)g_tramp) / THUNK_SZ);
        if (idx >= 0 && idx < g_nstubs)
            fprintf(stderr, "  InStub   : [%d] %s\n", idx, g_snames[idx]);
    }

    /* Print which library the crash RIP belongs to */
    {
        FILE *maps = fopen("/proc/self/maps", "r");
        if (maps) {
            char line[256];
            while (fgets(line, sizeof(line), maps)) {
                unsigned long start, end;
                if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
                    if (rip >= start && rip < end) {
                        fprintf(stderr, "  Library  : %s", line);
                        break;
                    }
                }
            }
            fclose(maps);
        }
    }

    fputs("────────────────────────────────────────\n", stderr);
    _exit(1);
}

/*
 * on_crash — thin wrapper around on_crash_impl().
 *
 * on_crash_impl() has ~30 separate resume points that each speculatively
 * pop the faulting RSP forward by some number of stack slots while
 * scanning for something that looks like a valid return address. We
 * confirmed empirically (2026-09-22) that this can walk RSP past the
 * outermost frame of the whole run (RSP ending up numerically ABOVE
 * g_entry_rsp, the value captured right before the PE entry point was
 * called) into stack memory that was never legitimately part of any
 * call chain — reading back as all-zero and producing an unrecoverable
 * null-pointer-call loop that used to take ~40+ crashes for the cycle
 * detector to notice.
 *
 * Rather than editing every one of those resume points individually,
 * validate the single shared exit condition here: after on_crash_impl()
 * decides how to resume, if RSP now exceeds the entry baseline, the
 * decision was invalid — stop immediately with a diagnostic instead of
 * letting it cycle further.
 */
static void on_crash(int sig, siginfo_t *si, void *uctx)
{
    on_crash_impl(sig, si, uctx);

    ucontext_t *uc  = (ucontext_t *)uctx;
    u64         rsp = (u64)uc->uc_mcontext.gregs[REG_RSP];
    if (g_entry_rsp_limit && rsp > g_entry_rsp_limit) {
        /* The seeded synthetic caller frame is intentionally *above* the
         * initial entry baseline after a valid `ret`; only values beyond the
         * bounded frame window are invalid. Keep the synthetic frame legal
         * while still rejecting true stack desert walks. */
        u64 safe_rsp = g_entry_ret_slot ? g_entry_ret_slot : g_entry_rsp;
        fprintf(stderr,
                "[GATE] Recovery tried to set RSP=0x%lx above the valid guest frame limit 0x%lx "
                "(+0x%lx). Clamping to the seeded return slot and redirecting to a known "
                "safe continuation instead of wandering into dead stack memory.\n",
                rsp, g_entry_rsp_limit, rsp - g_entry_rsp_limit);
        fflush(stderr);
        uc->uc_mcontext.gregs[REG_RSP] = (greg_t)safe_rsp;
        if (g_img) {
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)((u64)g_img + 0x235a694);
        }
        return;
    }
}

static void __attribute__((noreturn)) guest_exit_stub(void)
{
    _exit(0);
}

/* ── main ───────────────────────────────────────────────────────── */
int main(int argc, char **argv)
{
    const char *exe = (argc > 1) ? argv[1]
        : "/run/media/abhineet/56A4064AA4062CD5/Games/Sekiro - Shadows Die Twice/sekiro.exe";
    g_exe_path = exe;

    /* Install a dedicated alternate signal stack. When the guest runs on a
     * synthetic RSP, the crash handler must not consume the guest stack frame:
     * glibc's stack canary checks will otherwise trip on the fake stack and
     * abort with "stack smashing detected" before the guest crash logic runs. */
    void *altstack = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (altstack == MAP_FAILED)
        die("mmap(sigaltstack): %s", strerror(errno));
    stack_t ss = {0};
    ss.ss_sp = altstack;
    ss.ss_size = 1 << 20;
    ss.ss_flags = 0;
    if (sigaltstack(&ss, NULL) != 0)
        perror("sigaltstack");

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_crash;
    sa.sa_flags     = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
    sigaction(SIGILL,  &sa, NULL);
    sigaction(SIGFPE,  &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL);  /* int3 / abort() in Windows CRT */

    printf("=== Beer Loader ===\n");
    pe_load(exe);
    pe_imports();
    setup_teb_peb();

    {
        int n = patch_transaction_assertions();
        printf("[PATCH] Disabled %d Transaction-failed assertion branches\n", n);
    }

    apply_section_perms();
    patch_known_bad_targets();
    init_steam_fake();
    init_dxgi_fake();
    init_d3d11_fake();
    init_dxgi_adapter_fake();
    /* Patch dxgi factory vtable with real adapter + swapchain */
    g_dxgi_vtab[7]  = (u64)dxgi_EnumAdapters_with_fake;  /* EnumAdapters */
    g_dxgi_vtab[10] = (u64)dxgi_CreateSwapChain;          /* CreateSwapChain */
    g_dxgi_vtab[12] = (u64)dxgi_EnumAdapters_with_fake;   /* EnumAdapters1 */
    g_dxgi_vtab[8]  = (u64)dxgi_Present;       /* IDXGISwapChain::Present */
    g_dxgi_vtab[9]  = (u64)dxgi_GetBuffer;     /* IDXGISwapChain::GetBuffer */
    g_dxgi_vtab[13] = (u64)dxgi_ResizeBuffers; /* IDXGISwapChain::ResizeBuffers */
    /* vtable[10] also acts as engine allocator – set after CreateSwapChain */
    /* For objects that are NOT a factory, vtable[10] returns a real allocation */
    g_d3d11_vtab[10] = (u64)engine_alloc_vtab10;

    NtHdrs64 *nt = (NtHdrs64 *)(g_img + ((DosHdr *)g_img)->lfanew);
    u64 entry    = (u64)g_img + nt->opt.entry_rva;

    /* Note: The TEB host stack fields were already populated by alloc_teb_for_thread()
     * Do NOT override them with the static buffer - that breaks the TEB setup!
     * The TEB already has the correct mmap'd stack bounds. */
    
    /* Instead, just update globals from the TEB for backward compatibility */
    u64 gs_base = 0;
    if (syscall(SYS_arch_prctl, ARCH_GET_GS, &gs_base) == 0 && gs_base) {
        WinTEB *teb_main = (WinTEB *)gs_base;
        g_host_call_stack_base = teb_main->HostCallStackBase;
        g_host_call_stack_top = teb_main->HostCallStackTop;
        fprintf(stderr, "[SETUP] Main thread TEB @ %p: HostCallStackBase=%p, HostCallStackTop=%p\n",
                (void *)gs_base, (void *)teb_main->HostCallStackBase, (void *)teb_main->HostCallStackTop);
    } else {
        fprintf(stderr, "[SETUP] WARNING: Could not get GS base!\n");
    }

    printf("\n[RUN] Handing off to entry point 0x%lx\n", entry);
    fflush(stdout);
    fflush(stderr);

    /*
     * Call via ms_abi function pointer so GCC generates a proper Windows x64
     * call sequence (correct register saves, shadow space, etc.).
     */
    typedef void __attribute__((ms_abi)) (*WinEntry)(void);

    /* Keep the synthetic startup stack well inside the mapped region. The real
     * helper we are emulating takes a normal Windows x64 caller frame, where the
     * current %rsp is the frame anchor and the saved return address is at
     * [rsp+0x5c8]. We therefore seed the frame relative to that anchor and hand
     * execution off with %rsp pointing at the anchor itself, not at a synthetic
     * value 0x5c8 bytes lower than the true frame base. */
    size_t guest_stack_size = 16 * 1024 * 1024;  /* 16 MB for deep recursion */
    void *guest_stack = mmap(NULL, guest_stack_size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (guest_stack == MAP_FAILED)
        die("mmap(guest_stack): %s", strerror(errno));

    u64 guest_stack_low  = (u64)guest_stack;
    u64 guest_stack_high = (u64)guest_stack + guest_stack_size;
    
    /* Map a guard page below the stack for underflow protection */
    void *guard_page = mmap((void *)(guest_stack_low - 0x1000), 0x1000,
                            PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (guard_page != MAP_FAILED) {
        guest_stack_low -= 0x1000;  /* Extend stack low boundary to include guard page */
    }
    
    /* Reserve 2MB at top for guest runtime, allowing deep recursion */
    u64 guest_rsp = (guest_stack_high - 0x200000) & ~0xF;
    /* Entry RSP must be 8 mod 16, as after a real `call` on Windows x64. */
    u64 guest_frame_base = guest_rsp - 0x600;
    /* Entry RSP must be 8 mod 16 (as after a real `call`). BEER_ALIGN_OLD=1
     * restores the legacy 0 mod 16 entry for A/B comparison. */
    if (!getenv("BEER_ALIGN_OLD"))
        guest_frame_base = ((guest_rsp - 0x600) & ~0xFULL) + 8;
    u64 guest_ret_slot = guest_frame_base + 0x5c8;
    u64 guest_rbp_slot = guest_frame_base + 0x5c0;
    u64 guest_frame_ptr_slot = guest_frame_base + 0x5d0;
    u64 guest_initial_ret = g_img ? ((u64)g_img + 0x235a120) : (u64)guest_exit_stub;
    if (!g_img || !image_addr_is_exec(guest_initial_ret))
        guest_initial_ret = (u64)guest_exit_stub;
    *(u64 *)guest_rbp_slot = guest_rbp_slot;
    *(u64 *)guest_ret_slot = guest_initial_ret;
    *(u64 *)guest_frame_ptr_slot = guest_ret_slot;
    g_entry_rsp = guest_frame_base;
    g_entry_rsp_limit = guest_frame_base + 0x1000;
    g_entry_ret_slot = guest_ret_slot;
    seed_guest_entry_frame(guest_initial_ret);
    g_guest_stack_low = guest_stack_low;
    g_guest_stack_high = guest_stack_high;
    set_teb_stack_bounds(guest_stack_low, guest_stack_high);

    printf("[STACK] guest stack=0x%lx..0x%lx ret=0x%lx entry_rsp=0x%lx ret_slot=0x%lx\n",
           guest_stack_low, guest_stack_high, *(u64 *)guest_ret_slot,
           g_entry_rsp, g_entry_ret_slot);
    fflush(stdout);

    fprintf(stderr, "[ENTRY] Entry point at %p, g_entry_rsp=%p, g_host_call_stack: %p..%p\n",
            (void*)entry, (void*)g_entry_rsp, (void*)g_host_call_stack_base, (void*)g_host_call_stack_top);

    /* SavedGuestRsp will be set by beer_dispatch_trampoline on first outer call.
     * Don't set it here - let the trampoline handle it. */
    fprintf(stderr, "[SETUP] Entry frame at 0x%lx..0x%lx, first API will set SavedGuestRsp\n",
            g_entry_rsp, g_entry_rsp + 0x1000);
    
    /* Verify TEB is clean at entry */
    u64 gs_base_check = 0;
    if (syscall(SYS_arch_prctl, ARCH_GET_GS, &gs_base_check) == 0 && gs_base_check) {
        WinTEB *teb_check = (WinTEB *)gs_base_check;
        fprintf(stderr, "[SETUP] TEB SavedGuestRsp at entry: 0x%lx (should be 0)\n", 
                teb_check->SavedGuestRsp);
    }
    
    fprintf(stderr, "[ENTRY] Before handoff: entry=%p g_entry_rsp=%p\n", (void*)entry, (void*)g_entry_rsp);
    fflush(stderr);
    
    asm volatile (
        "xor %%rax, %%rax\n\t"
        "xor %%rcx, %%rcx\n\t"
        "xor %%rdx, %%rdx\n\t"
        "xor %%r8, %%r8\n\t"
        "xor %%r9, %%r9\n\t"
        "mov %0, %%rsp\n\t"
        ".globl _guest_entry_point\n\t"
        "_guest_entry_point:\n\t"
        "jmp *%1\n\t"
        :
        : "r"(g_entry_rsp), "r"(entry)
        : "memory", "rax", "rcx", "rdx", "r8", "r9", "rsp"
    );

    __builtin_unreachable();
}
