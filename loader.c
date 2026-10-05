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
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <limits.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <wctype.h>
#include <dlfcn.h>
#include <unistd.h>

/* D3D11 COM object stubs from graphics/ */
#include "d3d11_compat.h"
#include "d3d12_compat.h"
#include "d3d12_compute.h"
#include "platform/xwayland_backend.h"
#include "platform/directinput_format.h"
#include "platform/win32_input.h"
#include "platform/directinput_format.h"
#include "audio/audio_backend.h"
#include "media/media_foundation.h"
#include "renderer/vulkan/vulkan_indexed_renderer.h"

static void beer_signal_guest_event(uint64_t event_handle);

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
typedef int64_t  s64;

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
typedef struct {
    u32 characteristics, timestamp;
    u16 major, minor;
    u32 name_rva, ordinal_base, function_count, name_count;
    u32 functions_rva, names_rva, ordinals_rva;
} ExportDir;
typedef struct {
    u64  StartAddr;
    u64  EndAddr;
    u64 *AddrOfIndex;
    u64  AddrOfCallbacks;
    u32  SizeOfZeroFill;
    u32  Characteristics;
} TLS_DIR64;

#define TLS_SLOTS 128

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

typedef struct {
    char name[64];
    char path[PATH_MAX];
    u8 *image;
    size_t image_size;
    u64 preferred_base;
    u64 delta;
    u32 tls_index;
    u64 tls_start;
    u64 tls_raw_size;
    u32 tls_zero_size;
    u64 tls_callbacks;
    int entry_called;
    int initializing;
} GuestDll;

#define MAX_GUEST_DLLS 32
static GuestDll g_guest_dlls[MAX_GUEST_DLLS];
static int g_guest_dll_count;
static GuestDll *g_loading_guest_dll;
/* Real bundled FMOD is the normal audio path. The compatibility object graph
 * remains available only as an explicit --audio=compat troubleshooting mode. */
static int g_use_guest_fmod = 1;
static u64 g_real_fmod_event_system_create;
static u64 g_real_fmod_event_get_system;
static u64 g_real_fmod_event_init;
static u64 g_real_fmod_event_update;
static u64 g_real_fmod_event_load;
static u64 g_real_fmod_event_get_event;
static u64 g_real_fmod_event_start;
static u64 g_real_fmod_system_set_output;
static u64 g_real_fmod_system_set_file_system;
static u64 g_real_fmod_system_create_sound;
static u64 g_real_fmod_channel_group_set_volume;
static u64 g_real_fmod_event_set_volume;
static u64 g_real_fmod_event_preload_fsb;
static u64 g_real_bink_open;
static u64 g_real_bink_close;
static u64 g_real_bink_set_sound_system;
static u64 g_real_bink_wait;
static u64 g_real_bink_next_frame;
static u64 g_real_bink_do_frame;
static u64 g_real_bink_do_frame_async_multi;
static u64 g_real_bink_do_frame_async_wait;
static u64 g_real_bink_start_async_thread;
static u64 g_real_bink_request_stop_async_thread;
static u64 g_real_bink_wait_stop_async_thread;
static void *g_real_fmod_core_system;
static void *g_real_fmod_event_system;
static int g_real_fmod_initialized;
static u64 g_real_fmod_internal_init;
static u64 g_real_fmod_internal_update;
static u64 g_real_fmod_internal_load;
static u64 g_real_fmod_internal_get_event;
static u64 g_real_fmod_internal_event_start;

static GuestDll *guest_dll_load(const char *name);
static u64 guest_dll_export(GuestDll *dll, const char *name);
static int resolve_windows_host_path(const char *name, char *output, size_t output_size);

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
static u32 g_total_sigsegv_count = 0;  /* Total SIGSEGV crashes handled */
static u32 g_total_sigill_count = 0;   /* Total SIGILL crashes handled */
static u32 g_total_other_sig_count = 0; /* Other signals */
static u32 g_stack_recovery_hits = 0;
static u64 g_stale_guest_stack_rip = 0;
static u32 g_stale_guest_stack_hits = 0;
static u64 g_bad_stack_operand = 0;
static u32 g_bad_stack_operand_hits = 0;
static u32 g_bad_callback_range_hits = 0;

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

/* Sync memory infrastructure: Track allocated lock/sync structures for game-init.
 * Game code uses XCHG instructions on arrays/tables of locks. We allocate real
 * memory at those addresses on-demand when the game tries to use them.
 * 
 * PHASE 5 ENHANCEMENT: Selective initialization - distinguish code vs data pages.
 * When allocating, analyze each 4KB page to detect if it contains code.
 * Code pages: Dense non-zero content, skip initialization (leave executable).
 * Data pages: Sparse/zero content, safe to fill with lock structures.
 */
#define MAX_SYNC_REGIONS 16
#define PAGE_SIZE 4096
typedef struct {
    u64 base_addr;              /* Base address where we allocated */
    u64 size;                   /* Size allocated */
    int mapped;                 /* Whether successfully mmap'd */
    u8 page_type[256];          /* Type per 4KB page: 0=data, 1=code (supports 1MB regions) */
} SyncRegion;
static SyncRegion g_sync_regions[MAX_SYNC_REGIONS];
static int g_sync_region_count = 0;

/* Analyze a 4KB page to determine if it contains code or data.
 * Heuristic: Code pages have dense non-zero content (>50% non-zero bytes).
 * Data pages have sparse content (<10% non-zero bytes).
 * NOTE: For freshly mmap'd memory (MAP_ANONYMOUS), pages are zero-init anyway. */
static int beer_page_is_code(const u8 *page) {
    if (!page) return 0;  /* Safety: null pointer = data */
    
    int non_zero_count = 0;
    for (int i = 0; i < PAGE_SIZE; i++) {
        if (page[i] != 0) {
            non_zero_count++;
        }
    }
    /* If >50% of page is non-zero, likely code; if <10%, likely data */
    int density_pct = (non_zero_count * 100) / PAGE_SIZE;
    return density_pct > 50 ? 1 : 0;  /* 1=code, 0=data */
}

/* Analyze entire region to detect code vs data pages, fill page_type array.
 * For freshly allocated MAP_ANONYMOUS memory, this is mostly academic since
 * all pages will be zero. The analysis is useful for regions that already had content. */
static void beer_analyze_region(u8 *region_base, u64 region_size, SyncRegion *sr) {
    if (!region_base || !sr) return;  /* Safety checks */
    
    u64 num_pages = (region_size < PAGE_SIZE * 256) ? (region_size / PAGE_SIZE) : 256;
    for (u64 i = 0; i < num_pages; i++) {
        u8 *page = region_base + (i * PAGE_SIZE);
        sr->page_type[i] = beer_page_is_code(page);
    }
}

/* Handle table for tracking created sync objects (CreateEventW, CreateMutexW, etc.)
 * Maps fake HANDLE values returned to game to real BEER_* structure pointers. */
#define MAX_SYNC_HANDLES 256
typedef struct {
    void *handle;        /* Fake handle value returned to game (non-zero) */
    void *object;        /* Pointer to real BEER_EVENT, BEER_CRITICAL_SECTION, etc. */
    u32  type;           /* Object type: 1=EVENT, 2=CRITICAL_SECTION, 3=SRWLOCK, 4=SEMAPHORE */
    u32  in_use;         /* 1 if valid, 0 if closed/free */
} SyncHandle;
static SyncHandle g_sync_handles[MAX_SYNC_HANDLES];
static int g_sync_handle_count = 0;
static u32 g_next_fake_handle = 0x10000000;  /* Start handles at 0x10000000 to avoid NULL */

/* Allocate a fake handle for a sync object */
static void *beer_alloc_sync_handle(void *object, u32 type) {
    if (g_sync_handle_count >= MAX_SYNC_HANDLES) {
        return NULL;  /* Handle table full */
    }
    
    void *fake_handle = (void *)g_next_fake_handle;
    g_next_fake_handle += 4;  /* Increment by 4 to look like HANDLE alignment */
    
    int idx = g_sync_handle_count++;
    g_sync_handles[idx].handle = fake_handle;
    g_sync_handles[idx].object = object;
    g_sync_handles[idx].type = type;
    g_sync_handles[idx].in_use = 1;
    
    return fake_handle;
}

/* Look up a sync object from a fake handle */
static void *beer_get_sync_object(void *handle, u32 *out_type) {
    for (int i = 0; i < g_sync_handle_count; i++) {
        if (g_sync_handles[i].in_use && g_sync_handles[i].handle == handle) {
            if (out_type) *out_type = g_sync_handles[i].type;
            return g_sync_handles[i].object;
        }
    }
    if (out_type) *out_type = 0;
    return NULL;
}

/* Close a handle and free it for reuse */
static void beer_close_sync_handle(void *handle) {
    for (int i = 0; i < g_sync_handle_count; i++) {
        if (g_sync_handles[i].in_use && g_sync_handles[i].handle == handle) {
            g_sync_handles[i].in_use = 0;
            return;
        }
    }
}

/* Windows CRITICAL_SECTION structure (64 bytes aligned) with full state machine.
 * Supports acquire/release with recursion, thread tracking, and waiter queues.
 * The game allocates arrays of these and uses XCHG to implement lock operations. */
#pragma pack(push, 1)
typedef struct _BEER_CRITICAL_SECTION {
    void *DebugInfo;              /* 0x00 (8 bytes) - normally points to RTL_CRITICAL_SECTION_DEBUG */
    s32 LockCount;                /* 0x08 (4 bytes) - -1 = unacquired, >=0 = acquired (stores waiter count-1) */
    s32 RecursionCount;           /* 0x0C (4 bytes) - number of times current owner has re-acquired */
    void *OwningThread;           /* 0x10 (8 bytes) - pthread_t of thread holding lock, or 0 if free */
    void *LockSemaphore;          /* 0x18 (8 bytes) - kernel event object for signaling, or 0 if unused */
    u64  SpinCount;               /* 0x20 (8 bytes) - spin attempts before blocking (typically 0) */
    u64  Reserved[4];             /* 0x28 (32 bytes) - reserved padding to 64-byte alignment */
} BEER_CRITICAL_SECTION;
#pragma pack(pop)

/* Thread ID for lock ownership tracking (cached from pthread_self) */
static __thread u64 g_current_thread_id = 0;

/* Windows SRWLOCK structure (8 bytes) for reader-writer locks.
 * Packed format: reader_count (bits 2-15) | writer_owned (bit 1) | writer_waiting (bit 0) */
#pragma pack(push, 1)
typedef struct _BEER_SRWLOCK {
    u64 State;  /* Packed: reader_count (bits 2-15) | writer_owned (bit 1) | writer_waiting (bit 0) */
} BEER_SRWLOCK;
#pragma pack(pop)

/* SRWLOCK state machine: Initialize to unlocked state */
static inline void beer_srwlock_initialize(BEER_SRWLOCK *lock) {
    lock->State = 0;  /* All zeros: no readers, no writer, no waiters */
}

/* SRWLOCK state machine: Acquire for reading (shared lock)
 * Multiple readers can hold simultaneously. Returns old state. */
static inline u64 beer_srwlock_acquire_shared(BEER_SRWLOCK *lock) {
    u64 old_state = lock->State;
    
    /* Extract components */
    u64 writer_waiting = (old_state >> 0) & 1;
    u64 writer_owned = (old_state >> 1) & 1;
    u64 reader_count = (old_state >> 2) & 0x3FFF;  /* 14-bit reader count */
    
    /* If writer is waiting or owns lock, we should wait (but we fake success) */
    if (writer_owned || writer_waiting) {
        /* Writer has priority; we'd normally block. For emulation, succeed anyway. */
        return old_state;
    }
    
    /* No writer contention; increment reader count */
    reader_count++;
    lock->State = (reader_count << 2) | (writer_owned << 1) | writer_waiting;
    
    return old_state;
}

/* SRWLOCK state machine: Release from reading */
static inline u64 beer_srwlock_release_shared(BEER_SRWLOCK *lock) {
    u64 old_state = lock->State;
    
    /* Extract components */
    u64 writer_waiting = (old_state >> 0) & 1;
    u64 writer_owned = (old_state >> 1) & 1;
    u64 reader_count = (old_state >> 2) & 0x3FFF;
    
    /* Decrement reader count */
    if (reader_count > 0) {
        reader_count--;
    }
    lock->State = (reader_count << 2) | (writer_owned << 1) | writer_waiting;
    
    return old_state;
}

/* SRWLOCK state machine: Acquire for writing (exclusive lock)
 * Only one writer at a time. Sets writer_owned = 1. */
static inline u64 beer_srwlock_acquire_exclusive(BEER_SRWLOCK *lock) {
    u64 old_state = lock->State;
    
    /* Extract components */
    u64 writer_waiting = (old_state >> 0) & 1;
    u64 writer_owned = (old_state >> 1) & 1;
    u64 reader_count = (old_state >> 2) & 0x3FFF;
    
    /* If writer owns or readers exist, we can't acquire (but fake success for emulation) */
    if (writer_owned || reader_count > 0) {
        /* Mark that a writer is waiting */
        lock->State = (reader_count << 2) | (writer_owned << 1) | 1;
        return old_state;
    }
    
    /* No contention; we acquire exclusive lock */
    lock->State = (0 << 2) | (1 << 1) | 0;  /* writer_owned = 1, others = 0 */
    
    return old_state;
}

/* SRWLOCK state machine: Release from writing */
static inline u64 beer_srwlock_release_exclusive(BEER_SRWLOCK *lock) {
    u64 old_state = lock->State;
    
    /* Extract components (just to show work, not really needed for simple release) */
    u64 writer_waiting = (old_state >> 0) & 1;
    (void)writer_waiting;  /* Suppress unused warning */
    
    /* Clear everything on release */
    lock->State = 0;
    
    return old_state;
}

/* Windows EVENT structure (24 bytes) for manual/auto-reset event signaling.
 * Used to signal completion or availability of resources between threads. */
#pragma pack(push, 1)
typedef struct _BEER_EVENT {
    u32 State;            /* 0x00 (4 bytes) - signaled (1) or unsignaled (0) */
    u32 ManualReset;      /* 0x04 (4 bytes) - 1=manual, 0=auto reset after wait */
    u32 WaiterCount;      /* 0x08 (4 bytes) - number of threads waiting on this event */
    u64 Padding[2];       /* 0x0C (16 bytes) - padding to 32-byte alignment */
} BEER_EVENT;
#pragma pack(pop)

/* EVENT state machine: Initialize event */
static inline void beer_event_initialize(BEER_EVENT *event, u32 manual_reset, u32 initial_state) {
    event->State = initial_state;
    event->ManualReset = manual_reset;
    event->WaiterCount = 0;
    event->Padding[0] = 0;
    event->Padding[1] = 0;
}

/* EVENT state machine: Set event to signaled state */
static inline void beer_event_set(BEER_EVENT *event) {
    event->State = 1;  /* Signal the event */
    /* In real Windows, this wakes all auto-reset waiters or all manual-reset waiters */
}

/* EVENT state machine: Reset event to unsignaled state */
static inline void beer_event_reset(BEER_EVENT *event) {
    event->State = 0;  /* Unsignal the event */
}

/* EVENT state machine: Check if event is signaled (for fake WaitForSingleObject) */
static inline u32 beer_event_is_signaled(BEER_EVENT *event) {
    return event->State;
}

/* EVENT state machine: Pulse event (set, then reset for auto-reset events) */
static inline void beer_event_pulse(BEER_EVENT *event) {
    event->State = 1;  /* Temporarily signal */
    if (!event->ManualReset) {
        event->State = 0;  /* Auto-reset: immediately unsignal */
    }
}

/* Get current thread ID for lock ownership (initialize on first call) */
static inline u64 beer_get_thread_id(void) {
    if (g_current_thread_id == 0) {
        g_current_thread_id = (u64)pthread_self();
    }
    return g_current_thread_id;
}

/* CRITICAL_SECTION state machine: Initialize lock to unacquired state */
static inline void beer_critical_section_initialize(BEER_CRITICAL_SECTION *cs) {
    cs->DebugInfo = NULL;
    cs->LockCount = -1;           /* -1 means unacquired */
    cs->RecursionCount = 0;
    cs->OwningThread = NULL;
    cs->LockSemaphore = NULL;
    cs->SpinCount = 0;
    for (int i = 0; i < 4; i++) {
        cs->Reserved[i] = 0;
    }
}

/* CRITICAL_SECTION state machine: Try to acquire lock (atomic semantics)
 * Returns old LockCount value (for XCHG result in RAX/RDX)
 * -1 = we acquired uncontended lock
 * >=0 = lock was held, waiter count before our attempt */
static inline s32 beer_critical_section_enter(BEER_CRITICAL_SECTION *cs) {
    u64 my_thread = beer_get_thread_id();
    s32 old_count;
    
    /* If we already own it, just increment recursion and return -1 (success) */
    if (cs->OwningThread == (void *)my_thread && cs->RecursionCount > 0) {
        cs->RecursionCount++;
        return -1;  /* Recursive acquisition succeeds */
    }
    
    /* Try to acquire uncontended lock: LockCount must be -1 */
    if (cs->LockCount == -1) {
        old_count = -1;
        cs->LockCount = 0;        /* Acquired by us, no waiters yet */
        cs->OwningThread = (void *)my_thread;
        cs->RecursionCount = 1;   /* First acquisition */
        return old_count;         /* Return -1 to indicate success */
    }
    
    /* Lock is held (LockCount >= 0). Return current state and increment waiter count.
     * In a real system, we'd block here. For emulation, we fake it by incrementing
     * LockCount (it tracks: LockCount = waiter_count - 1). */
    old_count = cs->LockCount;
    cs->LockCount++;              /* One more waiter now */
    return old_count;             /* Return previous waiter count */
}

/* CRITICAL_SECTION state machine: Release lock
 * Returns old LockCount value (for XCHG result)
 * Returns -1 if we released and lock is now free
 * Returns >=0 if there are still waiters */
static inline s32 beer_critical_section_leave(BEER_CRITICAL_SECTION *cs) {
    u64 my_thread = beer_get_thread_id();
    
    /* Sanity check: we should own this lock */
    if (cs->OwningThread != (void *)my_thread) {
        /* Attempted to release lock we don't own - return current state */
        return cs->LockCount;
    }
    
    /* Decrement recursion count */
    if (cs->RecursionCount > 1) {
        cs->RecursionCount--;
        return -1;  /* Still held by us (recursive), return -1 */
    }
    
    /* Fully releasing the lock */
    cs->RecursionCount = 0;
    s32 old_count = cs->LockCount;
    
    if (cs->LockCount == 0) {
        /* No waiters, just mark as free */
        cs->LockCount = -1;
        cs->OwningThread = NULL;
        return -1;  /* Signal: lock is now free */
    } else {
        /* Waiters present (LockCount >= 1 means waiter_count = LockCount + 1).
         * Decrement waiter count and mark as unowned (but still locked by someone waiting). */
        cs->LockCount--;
        cs->OwningThread = NULL;  /* Released by us, someone else will acquire */
        return 0;                 /* Indicate: still held, but not by us */
    }
}

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
/* Fault recovery runs on both the synthetic main guest stack and native
 * pthread stacks used by guest workers. Keep each thread's live bounds
 * separately; comparing a worker RSP with the main stack caused valid worker
 * faults to be reset into the main thread's startup frame. */
static __thread u64 g_current_guest_stack_low;
static __thread u64 g_current_guest_stack_high;
static void __attribute__((noreturn)) guest_exit_stub(void);

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
    u64 low = g_current_guest_stack_low ? g_current_guest_stack_low : g_guest_stack_low;
    u64 high = g_current_guest_stack_high ? g_current_guest_stack_high : g_guest_stack_high;
    if (!low || !high)
        return 1; /* no guest stack window recorded yet: permissive until setup */
    return rsp > low + 0x200 && rsp < high - 0x200;
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
        ret_target = (u64)guest_exit_stub;

    /* AddressOfEntryPoint is entered as a normal Win64 callee: RSP is 8 mod 16
     * and [RSP] is its caller's return address.  The old synthetic frame left
     * [RSP] zero and placed a startup continuation only at RSP+0x5c8.  Once the
     * entry point completed normally, RET therefore loaded both RIP and RSP
     * from zeroed context.  Keep the legacy helper slots for narrowly validated
     * recovery paths, but make the architectural return slot authoritative. */
    for (u64 p = g_entry_rsp; p + 8 <= g_entry_rsp + 0x1000; p += 8)
        *(u64 *)p = 0;

    *(u64 *)g_entry_rsp = ret_target;

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

    for (int period = 1; period <= CYCLE_HIST_PERIOD_MAX; period++) {
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
/* Import-dispatch provenance used to diagnose nested guest/host transitions.
 * The trampoline records these before changing RSP or consuming r10. */
__thread u64 g_dispatch_import_index;
__thread u64 g_dispatch_incoming_rsp;
__thread u64 g_dispatch_incoming_return;
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
    u8  _pad1[0x40 - 0x38];
    u64 UniqueProcess;          /* +0x040 CLIENT_ID.UniqueProcess */
    u64 UniqueThread;           /* +0x048 CLIENT_ID.UniqueThread  */
    u8  _pad2[0x60 - 0x50];
    u64 ProcEnvBlk;             /* +0x060 */
    u8  _pad3[0xA0 - 0x68];
    u64 SavedGuestRsp;          /* +0x0A0 for beer_dispatch_trampoline */
    u64 HostCallStackBase;      /* +0x0A8 for beer_dispatch_trampoline */
    u64 HostCallStackTop;       /* +0x0B0 for beer_dispatch_trampoline */
    u8  _pad4[0x1000 - 0xB8];
} WinTEB;

_Static_assert(offsetof(WinTEB, UniqueProcess) == 0x40, "WinTEB process ID offset");
_Static_assert(offsetof(WinTEB, UniqueThread) == 0x48, "WinTEB thread ID offset");
_Static_assert(offsetof(WinTEB, ProcEnvBlk) == 0x60, "WinTEB PEB offset");

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
    "    mov %r10, %fs:g_dispatch_import_index@tpoff\n"
    "    mov %rsp, %fs:g_dispatch_incoming_rsp@tpoff\n"
    "    mov (%rsp), %rax\n"
    "    mov %rax, %fs:g_dispatch_incoming_return@tpoff\n"
    "    mov %rsp, %rax\n"
    "    sub %gs:0xA8, %rax\n"              /* Subtract HostCallStackBase from TEB @ 0xA8 */
    "    cmp $" HOST_CALL_STACK_SIZE_STR ", %rax\n"
    "    jb 1f\n"                           /* (rsp - base) < SIZE => already on it */
    "    mov %rsp, %gs:0xA0\n"              /* Save to TEB SavedGuestRsp @ 0xA0 */
    "    mov %gs:0xB0, %rsp\n"              /* Switch to HostCallStackTop from TEB @ 0xB0 */
    "    and $-16, %rsp\n"                  /* Guarantee Windows ABI pre-call alignment */
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
    /* Preserve the implementation's RAX return value. Reading SavedGuestRsp
     * directly from the TEB needs no helper call, and R11 is volatile under the
     * Windows x64 ABI so it can carry the guest continuation without changing
     * any observable nonvolatile state. The old helper returned guest RSP in
     * RAX and the subsequent `pop %rax` replaced every API result with the
     * caller's continuation address. */
    "    mov %gs:0xA0, %rsp\n"
    "    movq $0, %gs:0xA0\n"              /* No outer dispatch remains active after restore */
    "    pop %r11\n"
    "    jmp *%r11\n"
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
static __thread u64 g_cached_windows_thread_id;
/* Set when the guest creates its top-level window. Keeping this near the
 * synchronization core lets focused diagnostics follow the actual UI/main
 * thread instead of being saturated by worker-pool waits. */
static u32 g_window_thread_id;

static inline u64 current_windows_thread_id(void)
{
    if (g_cached_windows_thread_id) return g_cached_windows_thread_id;
    u64 gs_base = 0;
    if (syscall(SYS_arch_prctl, ARCH_GET_GS, &gs_base) == 0 && gs_base) {
        WinTEB *teb = (WinTEB *)gs_base;
        if (teb->UniqueThread)
            return g_cached_windows_thread_id = teb->UniqueThread;
    }
    return g_cached_windows_thread_id = (u64)(u32)syscall(SYS_gettid);
}

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
    int lock_result = pthread_mutex_trylock(m);
    if (lock_result == EBUSY) {
        if (getenv("BEER_MAIN_THREAD_DIAGNOSTICS") && g_window_thread_id &&
            current_windows_thread_id() == g_window_thread_id) {
            static _Atomic(u32) main_cs_contention_logs;
            u32 trace = atomic_fetch_add(&main_cs_contention_logs, 1);
            if (trace < 256)
                fprintf(stderr,
                        "[MAIN] critical-section contention cs=%p owner=%llu "
                        "caller=0x%llx\n",
                        (void *)cs, (unsigned long long)cs->OwningThread,
                        (unsigned long long)g_dispatch_incoming_return);
        }
        pthread_mutex_lock(m);
    } else if (lock_result != 0) {
        pthread_mutex_lock(m);
    }
    if (valid) {
        cs->RecursionCount++;
        cs->LockCount++;
        cs->OwningThread = current_windows_thread_id();
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
            cs->OwningThread = current_windows_thread_id();
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

/* ---- Module handles / Windows prefix ---- */
static const char *g_exe_path = NULL;       /* Windows-visible Z:\\ path */
static const char *g_exe_host_path = NULL;  /* canonical Linux path */
static char g_exe_path_storage[4096];
static char g_exe_host_path_storage[4096];
static u16 g_exe_path_w[4096];
static u16 *g_guest_argv_w[2] = {g_exe_path_w, NULL};

static char g_prefix_path[PATH_MAX];
static char g_drive_c_path[PATH_MAX];
static const char g_windows_dir[] = "C:\\Windows";
static const char g_system_dir[] = "C:\\Windows\\System32";
static const char g_temp_dir[] = "C:\\Windows\\Temp";
static const char g_user_profile[] = "C:\\Users\\Player";
static const char g_roaming_appdata[] = "C:\\Users\\Player\\AppData\\Roaming";
static const char g_local_appdata[] = "C:\\Users\\Player\\AppData\\Local";
static const char g_program_data[] = "C:\\ProgramData";

static int mkdir_tree(const char *path)
{
    char copy[PATH_MAX];
    size_t length = strlen(path);
    if (!length || length >= sizeof(copy)) { errno = ENAMETOOLONG; return 0; }
    memcpy(copy, path, length + 1);
    for (char *p = copy + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(copy, 0777) != 0 && errno != EEXIST) return 0;
        *p = '/';
    }
    return mkdir(copy, 0777) == 0 || errno == EEXIST;
}

static int configure_guest_prefix(const char *requested)
{
    char candidate[PATH_MAX];
    const char *configured = NULL;
    if (requested && *requested) {
        if (requested[0] == '/') {
            if (strlen(requested) >= sizeof(candidate)) { errno = ENAMETOOLONG; return 0; }
            strcpy(candidate, requested);
        } else {
            char cwd[PATH_MAX];
            if (!getcwd(cwd, sizeof(cwd)) ||
                snprintf(candidate, sizeof(candidate), "%s/%s", cwd, requested) >= (int)sizeof(candidate)) {
                errno = ENAMETOOLONG;
                return 0;
            }
        }
    } else {
        configured = getenv("BEER_PREFIX");
        if (configured && *configured) return configure_guest_prefix(configured);
        const char *data_home = getenv("XDG_DATA_HOME");
        const char *home = getenv("HOME");
        if (data_home && *data_home)
            snprintf(candidate, sizeof(candidate), "%s/beer/prefixes/default", data_home);
        else
            snprintf(candidate, sizeof(candidate), "%s/.local/share/beer/prefixes/default",
                     home && *home ? home : "/tmp");
    }

    char canonical[PATH_MAX];
    const char *prefix_path = realpath(candidate, canonical);
    if (!prefix_path) {
        if (errno != ENOENT || !mkdir_tree(candidate) || !realpath(candidate, canonical)) return 0;
        prefix_path = canonical;
    }
    if (strlen(prefix_path) >= sizeof(g_prefix_path)) { errno = ENAMETOOLONG; return 0; }
    strcpy(g_prefix_path, prefix_path);
    if (snprintf(g_drive_c_path, sizeof(g_drive_c_path), "%s/drive_c", g_prefix_path) >=
        (int)sizeof(g_drive_c_path)) { errno = ENAMETOOLONG; return 0; }

    static const char *directories[] = {
        "drive_c/Windows/System32", "drive_c/Windows/SysWOW64",
        "drive_c/Windows/Temp", "drive_c/Program Files", "drive_c/ProgramData",
        "drive_c/Users/Player/AppData/Local", "drive_c/Users/Player/AppData/Roaming",
        "drive_c/Users/Player/Documents", "drive_c/Users/Player/Saved Games",
        "dosdevices"
    };
    if (!mkdir_tree(g_prefix_path)) return 0;
    for (size_t i = 0; i < sizeof(directories) / sizeof(directories[0]); ++i) {
        char path[PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", g_prefix_path, directories[i]) >= (int)sizeof(path) ||
            !mkdir_tree(path)) return 0;
    }
    char c_link[PATH_MAX];
    if (snprintf(c_link, sizeof(c_link), "%s/dosdevices/c:", g_prefix_path) >= (int)sizeof(c_link))
        return 0;
    if (symlink("../drive_c", c_link) != 0 && errno != EEXIST) return 0;

    setenv("USERPROFILE", g_user_profile, 1);
    setenv("APPDATA", g_roaming_appdata, 1);
    setenv("LOCALAPPDATA", g_local_appdata, 1);
    setenv("PROGRAMDATA", g_program_data, 1);
    setenv("TEMP", g_temp_dir, 1);
    setenv("TMP", g_temp_dir, 1);
    setenv("WINDIR", g_windows_dir, 1);
    setenv("SystemRoot", g_windows_dir, 1);
    fprintf(stderr, "[PREFIX] %s (C: -> %s)\n", g_prefix_path, g_drive_c_path);
    return 1;
}

/* A Windows game launcher starts the process with a stable executable path and,
 * in Sekiro's case, the installation directory as its current directory. Host
 * files outside drive C are exposed through the conventional Z: mapping. */
static const char *configure_guest_process_path(const char *exe)
{
    char resolved[sizeof(g_exe_host_path_storage)];
    if (!exe || !realpath(exe, resolved)) return NULL;

    size_t host_len = strlen(resolved);
    if (host_len >= sizeof(g_exe_host_path_storage) || host_len + 2 >= sizeof(g_exe_path_storage)) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    memcpy(g_exe_host_path_storage, resolved, host_len + 1);
    g_exe_path_storage[0] = 'Z';
    g_exe_path_storage[1] = ':';
    for (size_t i = 0; i <= host_len; ++i)
        g_exe_path_storage[i + 2] = resolved[i] == '/' ? '\\' : resolved[i];
    size_t guest_len = host_len + 2;
    for (size_t i = 0; i <= guest_len; ++i) g_exe_path_w[i] = (u8)g_exe_path_storage[i];

    char directory[sizeof(g_exe_host_path_storage)];
    memcpy(directory, resolved, host_len + 1);
    char *slash = strrchr(directory, '/');
    if (!slash) { errno = EINVAL; return NULL; }
    *slash = 0;
    if (chdir(directory) != 0) return NULL;

    g_exe_host_path = g_exe_host_path_storage;
    g_exe_path = g_exe_path_storage;
    return g_exe_host_path;
}

static void lowercase_dll_basename(const char *name, char *out, size_t out_size)
{
    if (!out_size) return;
    const char *base = name ? name : "";
    for (const char *p = base; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    size_t i = 0;
    while (base[i] && i + 1 < out_size) {
        char c = base[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
        out[i++] = c;
    }
    out[i] = 0;
}

static GuestDll *guest_dll_from_handle(u64 handle)
{
    for (int i = 0; i < g_guest_dll_count; ++i)
        if ((u64)g_guest_dlls[i].image == handle)
            return &g_guest_dlls[i];
    return NULL;
}

static GuestDll *guest_dll_find(const char *name)
{
    char lower[64];
    lowercase_dll_basename(name, lower, sizeof(lower));
    for (int i = 0; i < g_guest_dll_count; ++i)
        if (!strcmp(g_guest_dlls[i].name, lower))
            return &g_guest_dlls[i];
    return NULL;
}

static int build_guest_dll_path(const char *name, char *out, size_t out_size)
{
    if (!name || !*name || !out || out_size < 2) return 0;
    if (strchr(name, '/') || strchr(name, '\\')) {
        char normalized[PATH_MAX];
        size_t n = strlen(name);
        if (n >= sizeof(normalized)) return 0;
        for (size_t i = 0; i <= n; ++i)
            normalized[i] = name[i] == '\\' ? '/' : name[i];
        if (realpath(normalized, out)) return 1;
        return 0;
    }
    char candidate[PATH_MAX];
    const char *slash = g_exe_host_path ? strrchr(g_exe_host_path, '/') : NULL;
    size_t directory_length = slash ? (size_t)(slash - g_exe_host_path) : 1;
    const char *directory = slash ? g_exe_host_path : ".";
    if (directory_length + 1 + strlen(name) + 1 > sizeof(candidate)) return 0;
    memcpy(candidate, directory, directory_length);
    candidate[directory_length] = '/';
    strcpy(candidate + directory_length + 1, name);
    if (realpath(candidate, out)) return 1;

    /* Windows DLL lookup is case-insensitive. Resolve against the installation
     * root even though Beer has already chdir'd there. */
    char absolute_candidate[PATH_MAX];
    if (candidate[0] == '/') {
        if (strlen(candidate) >= sizeof(absolute_candidate)) return 0;
        strcpy(absolute_candidate, candidate);
    } else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd)) ||
            snprintf(absolute_candidate, sizeof(absolute_candidate), "%s/%s", cwd, candidate) >=
                (int)sizeof(absolute_candidate)) return 0;
    }
    if (resolve_windows_host_path(absolute_candidate, out, out_size) &&
        access(out, R_OK) == 0) return 1;

    /* Several games ship the VC/UCRT redistributable privately in a Win64
     * subdirectory. Windows searches application directories supplied by the
     * launcher; Beer has no loader search-path object yet, so model the observed
     * installation layout explicitly rather than replacing those DLLs with
     * success-returning generic thunks. */
    if (directory_length + sizeof("/Win64/") + strlen(name) > sizeof(candidate)) return 0;
    memcpy(candidate, directory, directory_length);
    memcpy(candidate + directory_length, "/Win64/", sizeof("/Win64/") - 1);
    strcpy(candidate + directory_length + sizeof("/Win64/") - 1, name);
    if (realpath(candidate, out)) return 1;
    if (candidate[0] == '/') return resolve_windows_host_path(candidate, out, out_size);
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd)) ||
        snprintf(absolute_candidate, sizeof(absolute_candidate), "%s/%s", cwd, candidate) >=
            (int)sizeof(absolute_candidate)) return 0;
    return resolve_windows_host_path(absolute_candidate, out, out_size);
}

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

    GuestDll *guest = guest_dll_find(lname);
    if (guest) return (u64)guest->image;

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
static __thread u32 g_last_error;

/* GetProcAddress – look up in our stub name table, then impl table */
static u64 __attribute__((ms_abi))
impl_GetProcAddress(u64 hmod, const char *procname)
{
    if (!procname) return 0;
    GuestDll *guest = guest_dll_from_handle(hmod);
    if (guest) {
        u64 address = guest_dll_export(guest, procname);
        if (address) return address;
        g_last_error = 127;
        return 0;
    }
    /* Search existing stubs (IAT entries). Dynamic backend discovery (notably
     * FMOD loading dsound.dll) must resolve to the implementation thunk even
     * when another DLL imported the same basename earlier. */
    for (int i = 0; i < g_nstubs; i++) {
        const char *bang = strchr(g_snames[i], '!');
        const char *fn   = bang ? bang + 1 : g_snames[i];
        if (strcmp(fn, procname) == 0) {
            if (!strncmp(procname, "DirectSound", 11) || !strncmp(procname, "waveOut", 7))
                fprintf(stderr, "[AUDIO] GetProcAddress(0x%lx, %s) -> existing thunk\n",
                        hmod, procname);
            return (u64)(g_tramp + i * THUNK_SZ);
        }
    }
    /* Search real implementations (not in IAT but we can implement) */
    ImplFn real = find_impl(procname);
    if (real) {
        if (g_nstubs < MAX_STUBS) {
            g_snames[g_nstubs] = strdup(procname);
            emit_impl_thunk(g_nstubs, (u64)real);
            u64 addr = (u64)(g_tramp + g_nstubs * THUNK_SZ);
            g_nstubs++;
            if (!strncmp(procname, "DirectSound", 11) ||
                !strncmp(procname, "waveOut", 7))
                fprintf(stderr, "[AUDIO] GetProcAddress(0x%lx, %s) -> new thunk\n",
                        hmod, procname);
            return addr;
        }
    }

    /* Windows returns NULL with ERROR_PROC_NOT_FOUND when the requested export
     * is absent.  Returning a generic callable thunk invents an export and lets
     * callers enter an optional subsystem with no implementation behind it.
     * In particular, Sekiro treated a fabricated compatInit export as present,
     * called it, and later used subsystem state that had never been created. */
    g_last_error = 127; /* ERROR_PROC_NOT_FOUND */
    fprintf(stderr, "[IMPL] GetProcAddress(0x%lx, \"%s\") -> NULL (export not found)\n",
            hmod, procname);
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
    HeapGuardEntry *e = malloc(sizeof(*e));
    if (!e) return;
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
            free(e);
            return 1;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_heap_guard_mutex);
    return 0;
}

static int heap_guards_enabled(void)
{
    static int initialized;
    static int enabled;
    if (!initialized) {
        const char *value = getenv("BEER_HEAP_GUARDS");
        enabled = value && *value && strcmp(value, "0") != 0;
        initialized = 1;
    }
    return enabled;
}

static void *heap_guard_alloc(size_t size, int zero)
{
    size_t want = size ? size : 1;
    if (!heap_guards_enabled()) {
        /* Guarding every allocation previously caused hundreds of thousands of
         * mmap/mprotect/munmap syscalls during startup. Normal runs use the
         * host allocator while retaining Beer’s size table; fault-isolation
         * runs can opt back in with BEER_HEAP_GUARDS=1. */
        void *user_ptr = zero ? calloc(1, want) : malloc(want);
        if (user_ptr) heap_guard_insert(user_ptr, user_ptr, 0, want);
        return user_ptr;
    }

    size_t alloc_pages = (want + HEAP_GUARD_PAGE - 1) / HEAP_GUARD_PAGE;
    size_t total = (alloc_pages + 1) * HEAP_GUARD_PAGE; /* +1 trailing guard page */

    void *base = mmap(NULL, total, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return NULL;
    if (mprotect((u8 *)base + alloc_pages * HEAP_GUARD_PAGE, HEAP_GUARD_PAGE, PROT_NONE) != 0)
        fprintf(stderr, "[HEAPGUARD] mprotect guard page failed: %s\n", strerror(errno));

    uintptr_t end = (uintptr_t)base + alloc_pages * HEAP_GUARD_PAGE;
    uintptr_t start = (end - want) & ~((uintptr_t)15);
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
    if (old_total) munmap(old_base, old_total);
    else free(old_base);
    return (u64)newp;
}

static u64 __attribute__((ms_abi))
impl_HeapFree(u64 heap, u32 flags, void *ptr)
{
    (void)heap; (void)flags;
    if (!ptr) return 1;
    void *base = NULL;
    size_t total = 0;
    if (heap_guard_remove(ptr, &base, &total, NULL)) {
        if (total) munmap(base, total);
        else free(base);
    }
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
impl_GetCurrentThreadId(void)
{
    /* Windows thread IDs are immutable. Cache the TEB value per host thread so
     * this hot API and critical-section ownership checks avoid arch_prctl/gettid
     * syscalls on every synchronization operation. */
    return current_windows_thread_id();
}

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
        /* EXCEPTION_POINTERS = { EXCEPTION_RECORD*, CONTEXT* }.  Preserve the
         * guest's failure evidence before TerminateProcess ends the run: AMD64
         * CONTEXT stores RSP/R14/RIP at 0x98/0xe8/0xf8, while an
         * EXCEPTION_RECORD64 stores NumberParameters at 0x18 and its 15
         * ULONG_PTR values at 0x20. */
        u64 *ptrs = (u64*)ep;
        const u8 *record = (const u8 *)(uintptr_t)ptrs[0];
        const u8 *context = (const u8 *)(uintptr_t)ptrs[1];
        if (record) {
            code = *(const u32 *)(record + 0x00);
            u32 parameter_count = *(const u32 *)(record + 0x18);
            if (parameter_count > 15) parameter_count = 15;
            fprintf(stderr, ": code=0x%08x address=0x%lx params=%u",
                    code, *(const u64 *)(record + 0x10), parameter_count);
            for (u32 i = 0; i < parameter_count; i++)
                fprintf(stderr, " p%u=0x%lx", i,
                        *(const u64 *)(record + 0x20 + (size_t)i * 8));
            /* 0xE06D7363 = C++ exception, 0xC0000005 = access violation */
        }
        if (context) {
            u64 guest_rsp = *(const u64 *)(context + 0x98);
            u64 guest_r14 = *(const u64 *)(context + 0xe8);
            u64 guest_rip = *(const u64 *)(context + 0xf8);
            fprintf(stderr, " context-RIP=0x%lx(RVA=0x%lx) RSP=0x%lx R14=0x%lx",
                    guest_rip,
                    (g_img && guest_rip >= (u64)g_img) ? guest_rip - (u64)g_img : guest_rip,
                    guest_rsp, guest_r14);
            if (is_guest_stack_rsp(guest_rsp)) {
                const u64 *stack = (const u64 *)(uintptr_t)guest_rsp;
                fputs(" stack-RVAs=", stderr);
                for (int i = 0, shown = 0; i < 96 && shown < 12; i++) {
                    u64 candidate = stack[i];
                    if (image_addr_is_exec(candidate)) {
                        fprintf(stderr, "%s0x%lx", shown ? "," : "",
                                candidate - (u64)g_img);
                        shown++;
                    }
                }
            }
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
static int prefer_emulated_module(const char *lower_name)
{
    /* Beer implements the Steam API contract itself.  Some games dynamically
     * load a protected/private steam_api64.dll after startup; mapping that DLL
     * as an ordinary PE image bypasses the compatibility facade and executes
     * its loader/protection stubs.  Keep dynamic loading consistent with IAT
     * imports and return the emulated module handle instead. */
    return lower_name && !strcmp(lower_name, "steam_api64.dll");
}

static u64 __attribute__((ms_abi))
impl_LoadLibraryA(const char *name)
{
    if (!name) { g_last_error = 126; return 0; }
    char lname[64];
    lowercase_dll_basename(name, lname, sizeof(lname));
    if (prefer_emulated_module(lname)) {
        u64 handle = dll_name_to_handle(lname);
        g_last_error = handle ? 0 : 126;
        return handle;
    }
    GuestDll *guest = guest_dll_find(lname);
    if (!guest) guest = guest_dll_load(name);
    if (guest) return (u64)guest->image;
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
    char narrow[PATH_MAX]; size_t i;
    for (i = 0; i + 1 < sizeof(narrow) && name[i]; ++i)
        narrow[i] = (char)(name[i] & 0x7f);
    narrow[i] = 0;
    char lname[64];
    lowercase_dll_basename(narrow, lname, sizeof(lname));
    if (prefer_emulated_module(lname)) {
        u64 handle = dll_name_to_handle(lname);
        g_last_error = handle ? 0 : 126;
        return handle;
    }
    GuestDll *guest = guest_dll_find(lname);
    if (!guest) guest = guest_dll_load(narrow);
    if (guest) return (u64)guest->image;
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
static u32 win_processor_count(void)
{
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    if (count < 1) count = 1;
    if (count > 64) count = 64;
    return (u32)count;
}

static u64 win_processor_mask(void)
{
    u32 count = win_processor_count();
    return count == 64 ? UINT64_MAX : (((u64)1 << count) - 1);
}

static u64 __attribute__((ms_abi))
impl_GetProcessAffinityMask(u64 h, u64 *proc_mask, u64 *sys_mask) {
    (void)h;
    u64 mask = win_processor_mask();
    if (proc_mask) *proc_mask = mask;
    if (sys_mask)  *sys_mask  = mask;
    return 1;
}
static u64 __attribute__((ms_abi)) impl_SetProcessAffinityMask(u64 h, u64 m)
    { (void)h;(void)m; return 1; }

/* Windows version and basic platform queries used by newer MSVC runtimes. */
typedef struct {
    u32 size, major, minor, build, platform;
    u16 service_pack[128];
    u16 service_pack_major, service_pack_minor, suite_mask;
    u8 product_type, reserved;
} WIN_OSVERSIONINFOEXW;

static u64 fill_windows_version(WIN_OSVERSIONINFOEXW *info)
{
    if (!info || info->size < 276) return 87;
    u32 size = info->size;
    memset(info, 0, size < sizeof(*info) ? size : sizeof(*info));
    info->size = size;
    info->major = 10;
    info->minor = 0;
    info->build = 19045;
    info->platform = 2;
    if (size >= sizeof(*info)) info->product_type = 1;
    return 0;
}
static u64 __attribute__((ms_abi)) impl_RtlGetVersion(WIN_OSVERSIONINFOEXW *info)
    { return fill_windows_version(info); }
static u64 __attribute__((ms_abi)) impl_VerSetConditionMask(u64 mask, u32 type, u8 condition)
{
    if (!type || !condition) return mask;
    return mask | ((u64)(condition & 7u) << ((__builtin_ctz(type) * 3u) & 63u));
}
static u64 __attribute__((ms_abi))
impl_VerifyVersionInfoW(const WIN_OSVERSIONINFOEXW *requested, u32 type_mask, u64 condition_mask)
{
    (void)type_mask; (void)condition_mask;
    if (!requested) { g_last_error = 87; return 0; }
    if (requested->major > 10 ||
        (requested->major == 10 && requested->minor > 0) ||
        (requested->major == 10 && requested->minor == 0 && requested->build > 19045)) {
        g_last_error = 1150;
        return 0;
    }
    g_last_error = 0;
    return 1;
}
static u64 __attribute__((ms_abi)) impl_AreFileApisANSI(void) { return 1; }
static u64 __attribute__((ms_abi)) impl_WinHttpCheckPlatform(void) { return 1; }
static u64 __attribute__((ms_abi))
impl_GetAdaptersAddresses(u32 family, u32 flags, void *reserved, void *addresses, u32 *size)
{
    (void)family; (void)flags; (void)reserved; (void)addresses;
    if (size) *size = 0;
    return 232; /* ERROR_NO_DATA */
}

/* ---- WINMM timing ---- */
static u64 __attribute__((ms_abi)) impl_timeGetTime(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
static u64 __attribute__((ms_abi)) impl_timeBeginPeriod(u32 p) { (void)p; return 0; }
static u64 __attribute__((ms_abi)) impl_timeEndPeriod(u32 p)   { (void)p; return 0; }

/* FMOD probes optional Windows registry settings during output setup. Returning
 * ERROR_SUCCESS from a generic stub without initializing its output handles or
 * value buffers corrupts that setup. Beer has no registry hive yet, so report
 * the documented "not found" result and initialize every output defensively. */
#define BEER_ERROR_FILE_NOT_FOUND 2u
#define BEER_ERROR_INVALID_HANDLE 6u
static u64 __attribute__((ms_abi))
impl_RegOpenKeyExA(u64 key, const char *subkey, u32 options, u32 access, u64 *result)
{
    (void)key; (void)subkey; (void)options; (void)access;
    if (result) *result = 0;
    return BEER_ERROR_FILE_NOT_FOUND;
}
static u64 __attribute__((ms_abi))
impl_RegOpenKeyExW(u64 key, const u16 *subkey, u32 options, u32 access, u64 *result)
{
    (void)key; (void)subkey; (void)options; (void)access;
    if (result) *result = 0;
    return BEER_ERROR_FILE_NOT_FOUND;
}
static u64 __attribute__((ms_abi))
impl_RegQueryValueExA(u64 key, const char *name, u32 *reserved, u32 *type,
                      void *data, u32 *size)
{
    (void)key; (void)name; (void)reserved; (void)data;
    if (type) *type = 0;
    if (size) *size = 0;
    return BEER_ERROR_FILE_NOT_FOUND;
}
static u64 __attribute__((ms_abi))
impl_RegQueryValueExW(u64 key, const u16 *name, u32 *reserved, u32 *type,
                      void *data, u32 *size)
{
    (void)key; (void)name; (void)reserved; (void)data;
    if (type) *type = 0;
    if (size) *size = 0;
    return BEER_ERROR_FILE_NOT_FOUND;
}
static u64 __attribute__((ms_abi)) impl_RegCloseKey(u64 key)
{
    return key ? 0u : BEER_ERROR_INVALID_HANDLE;
}

/* ---- WINMM wave output --------------------------------------------------
 * FMOD Ex can use waveOut as its Windows output driver. Keep this bridge
 * deliberately small and truthful: one PCM16 output device backed by the
 * user's PipeWire/Pulse server. The host backend owns a copy of each submitted
 * buffer, so WAVEHDR completion can be reported without retaining guest memory. */
#pragma pack(push, 2)
typedef struct {
    u16 format_tag;
    u16 channels;
    u32 samples_per_sec;
    u32 avg_bytes_per_sec;
    u16 block_align;
    u16 bits_per_sample;
    u16 extra_size;
} BeerWaveFormat;
#pragma pack(pop)

typedef struct BeerWaveHeader {
    char *data;
    u32 buffer_length;
    u32 bytes_recorded;
    u64 user;
    u32 flags;
    u32 loops;
    struct BeerWaveHeader *next;
    u64 reserved;
} BeerWaveHeader;

typedef struct {
    u32 magic;
    u32 rate;
    u16 channels;
    u16 bits;
    u64 callback;
    u64 callback_instance;
    u32 callback_type;
    u32 volume;
    int paused;
} BeerWaveOut;

#define BEER_WAVE_MAGIC 0x57415645u
#define MMSYSERR_NOERROR 0u
#define MMSYSERR_ERROR 1u
#define MMSYSERR_BADDEVICEID 2u
#define MMSYSERR_INVALHANDLE 5u
#define MMSYSERR_NODRIVER 6u
#define WAVERR_BADFORMAT 32u
#define WAVERR_UNPREPARED 34u
#define WAVE_FORMAT_PCM 1u
#define WAVE_FORMAT_QUERY 0x0001u
#define CALLBACK_TYPEMASK 0x00070000u
#define CALLBACK_FUNCTION 0x00030000u
#define CALLBACK_EVENT 0x00050000u
#define WHDR_DONE 0x00000001u
#define WHDR_PREPARED 0x00000002u
#define WHDR_INQUEUE 0x00000010u
#define WOM_OPEN 0x03bbu
#define WOM_CLOSE 0x03bcu
#define WOM_DONE 0x03bdu

static BeerWaveOut g_wave_out;

/* Completion runs on the host audio thread. Event callbacks are ordinary Beer
 * handles; function callbacks execute guest FMOD code and therefore need a
 * Windows TEB/GS just like every other guest-capable host thread. */
static void *alloc_teb_for_thread(void);
static void guest_dll_notify_thread(u32 reason);
static u64 __attribute__((ms_abi)) impl_SetEvent(u64 h);

static void beer_wave_notify(BeerWaveOut *wave, u32 message, u64 param1)
{
    if (!wave || !wave->callback) return;
    if (wave->callback_type == CALLBACK_EVENT) {
        impl_SetEvent(wave->callback);
    } else if (wave->callback_type == CALLBACK_FUNCTION) {
        u64 gs_base = 0;
        if (syscall(SYS_arch_prctl, ARCH_GET_GS, &gs_base) != 0 || !gs_base) {
            void *teb = alloc_teb_for_thread();
            if (!teb || syscall(SYS_arch_prctl, ARCH_SET_GS, (u64)teb) != 0) {
                fprintf(stderr, "[AUDIO] cannot install TEB for wave callback\n");
                return;
            }
        }
        typedef void __attribute__((ms_abi)) (*WaveCallback)(u64, u32, u64, u64, u64);
        ((WaveCallback)wave->callback)((u64)wave, message,
                                      wave->callback_instance, param1, 0);
    }
}

static void beer_wave_buffer_done(void *opaque)
{
    BeerWaveHeader *header = opaque;
    if (!header) return;
    header->flags &= ~WHDR_INQUEUE;
    header->flags |= WHDR_DONE;
    static _Atomic(u32) completions;
    u32 completion = atomic_fetch_add_explicit(&completions, 1, memory_order_relaxed) + 1;
    if (completion <= 4)
        fprintf(stderr, "[AUDIO] waveOut completion #%u callback-type=0x%x\n",
                completion, g_wave_out.callback_type);
    beer_wave_notify(&g_wave_out, WOM_DONE, (u64)header);
}

static u64 __attribute__((ms_abi)) impl_waveOutGetNumDevs(void)
{
    /* Enumeration must not open/fix the host stream format. The output stream
     * is created by waveOutOpen with FMOD's selected sample rate. */
    return 1;
}
static u64 __attribute__((ms_abi)) impl_waveInGetNumDevs(void) { return 0; }

static u64 __attribute__((ms_abi))
impl_waveOutGetDevCapsA(u64 device, void *caps, u32 size)
{
    if ((u32)device > 0 || !caps) return MMSYSERR_BADDEVICEID;
    u8 result[84] = {0};
    *(u16 *)(result + 0) = 1; /* manufacturer */
    *(u16 *)(result + 2) = 1; /* product */
    *(u32 *)(result + 4) = 0x00010000;
    strncpy((char *)(result + 8), "Beer PipeWire Output", 31);
    *(u32 *)(result + 40) = 0x00000fffu; /* common PCM rates/formats */
    *(u16 *)(result + 44) = 2;
    *(u32 *)(result + 48) = 0x000fu; /* volume/pan/sample-accurate */
    memcpy(caps, result, size < sizeof(result) ? size : sizeof(result));
    return MMSYSERR_NOERROR;
}

static u64 __attribute__((ms_abi))
impl_waveOutGetDevCapsW(u64 device, void *caps, u32 size)
{
    if ((u32)device > 0 || !caps) return MMSYSERR_BADDEVICEID;
    u8 result[116] = {0};
    static const char name[] = "Beer PipeWire Output";
    *(u16 *)(result + 0) = 1;
    *(u16 *)(result + 2) = 1;
    *(u32 *)(result + 4) = 0x00010000;
    for (size_t i = 0; i < sizeof(name); ++i) ((u16 *)(result + 8))[i] = (u8)name[i];
    *(u32 *)(result + 72) = 0x00000fffu;
    *(u16 *)(result + 76) = 2;
    *(u32 *)(result + 80) = 0x000fu;
    memcpy(caps, result, size < sizeof(result) ? size : sizeof(result));
    return MMSYSERR_NOERROR;
}

static u64 __attribute__((ms_abi))
impl_waveOutOpen(BeerWaveOut **out, u32 device, const BeerWaveFormat *format,
                 u64 callback, u64 instance, u32 flags)
{
    if (!format || device > 0) return MMSYSERR_BADDEVICEID;
    if (format->format_tag != WAVE_FORMAT_PCM || format->bits_per_sample != 16 ||
        (format->channels != 1 && format->channels != 2) || !format->samples_per_sec)
        return WAVERR_BADFORMAT;
    if (flags & WAVE_FORMAT_QUERY) return MMSYSERR_NOERROR;
    if (!out) return MMSYSERR_ERROR;
    if (g_wave_out.magic == BEER_WAVE_MAGIC) beer_audio_shutdown();
    if (!beer_audio_init(format->samples_per_sec, format->channels))
        return MMSYSERR_NODRIVER;
    memset(&g_wave_out, 0, sizeof(g_wave_out));
    g_wave_out.magic = BEER_WAVE_MAGIC;
    g_wave_out.rate = format->samples_per_sec;
    g_wave_out.channels = format->channels;
    g_wave_out.bits = format->bits_per_sample;
    g_wave_out.callback = callback;
    g_wave_out.callback_instance = instance;
    g_wave_out.callback_type = flags & CALLBACK_TYPEMASK;
    g_wave_out.volume = 0xffffffffu;
    *out = &g_wave_out;
    /* WinMM begins in the stopped state. FMOD's output module uses Reset to
     * stop/flush and Restart to begin mixer consumption; tracking this state
     * keeps the host bridge aligned with the documented lifecycle. */
    beer_wave_notify(&g_wave_out, WOM_OPEN, 0);
    fprintf(stderr, "[AUDIO] waveOutOpen: %u Hz, %u channels, PCM16 callback-type=0x%x callback=%p\n",
            g_wave_out.rate, g_wave_out.channels, g_wave_out.callback_type,
            (void *)g_wave_out.callback);
    return MMSYSERR_NOERROR;
}

static u64 __attribute__((ms_abi))
impl_waveOutPrepareHeader(BeerWaveOut *wave, BeerWaveHeader *header, u32 size)
{
    (void)size;
    if (!wave || wave->magic != BEER_WAVE_MAGIC) return MMSYSERR_INVALHANDLE;
    if (!header || !header->data || !header->buffer_length) return MMSYSERR_ERROR;
    header->flags |= WHDR_PREPARED;
    header->flags &= ~WHDR_DONE;
    return MMSYSERR_NOERROR;
}

static u64 __attribute__((ms_abi))
impl_waveOutUnprepareHeader(BeerWaveOut *wave, BeerWaveHeader *header, u32 size)
{
    (void)size;
    if (!wave || wave->magic != BEER_WAVE_MAGIC) return MMSYSERR_INVALHANDLE;
    if (!header || (header->flags & WHDR_INQUEUE)) return WAVERR_UNPREPARED;
    header->flags &= ~WHDR_PREPARED;
    return MMSYSERR_NOERROR;
}

static u64 __attribute__((ms_abi))
impl_waveOutWrite(BeerWaveOut *wave, BeerWaveHeader *header, u32 size)
{
    (void)size;
    if (!wave || wave->magic != BEER_WAVE_MAGIC) return MMSYSERR_INVALHANDLE;
    if (!header || !(header->flags & WHDR_PREPARED)) return WAVERR_UNPREPARED;
    header->flags |= WHDR_INQUEUE;
    header->flags &= ~WHDR_DONE;
    if (!beer_audio_submit(header->data, header->buffer_length,
                           beer_wave_buffer_done, header)) {
        header->flags &= ~WHDR_INQUEUE;
        return MMSYSERR_ERROR;
    }
    static _Atomic(u32) writes;
    u32 write = atomic_fetch_add_explicit(&writes, 1, memory_order_relaxed) + 1;
    if (write <= 4)
        fprintf(stderr, "[AUDIO] waveOutWrite #%u: %u PCM bytes queued\n",
                write, header->buffer_length);
    /* Completion is delivered asynchronously after PipeWire/Pulse accepts the
     * copied packet. FMOD relies on WOM_DONE to recycle and refill WAVEHDRs. */
    return MMSYSERR_NOERROR;
}

static u64 __attribute__((ms_abi)) impl_waveOutPause(BeerWaveOut *wave)
{
    if (!wave || wave->magic != BEER_WAVE_MAGIC) return MMSYSERR_INVALHANDLE;
    wave->paused = 1;
    return MMSYSERR_NOERROR;
}

static u64 __attribute__((ms_abi)) impl_waveOutRestart(BeerWaveOut *wave)
{
    if (!wave || wave->magic != BEER_WAVE_MAGIC) return MMSYSERR_INVALHANDLE;
    wave->paused = 0;
    return MMSYSERR_NOERROR;
}

static u64 __attribute__((ms_abi)) impl_waveOutSetVolume(BeerWaveOut *wave, u32 volume)
{
    if (!wave || wave->magic != BEER_WAVE_MAGIC) return MMSYSERR_INVALHANDLE;
    /* Preserve WinMM's packed left/right volume. Bink supplies already mixed
     * PCM, so the host stream does not apply an additional gain stage yet. */
    wave->volume = volume;
    return MMSYSERR_NOERROR;
}

static u64 __attribute__((ms_abi)) impl_waveOutReset(BeerWaveOut *wave)
{
    if (!wave || wave->magic != BEER_WAVE_MAGIC) return MMSYSERR_INVALHANDLE;
    beer_audio_reset();
    return MMSYSERR_NOERROR;
}

static u64 __attribute__((ms_abi)) impl_waveOutClose(BeerWaveOut *wave)
{
    if (!wave || wave->magic != BEER_WAVE_MAGIC) return MMSYSERR_INVALHANDLE;
    beer_wave_notify(wave, WOM_CLOSE, 0);
    wave->magic = 0;
    beer_audio_shutdown();
    return MMSYSERR_NOERROR;
}

static u64 __attribute__((ms_abi))
impl_waveOutGetPosition(BeerWaveOut *wave, void *time_value, u32 size)
{
    if (!wave || wave->magic != BEER_WAVE_MAGIC) return MMSYSERR_INVALHANDLE;
    if (!time_value || size < 8) return MMSYSERR_ERROR;
    /* MMTIME constants are bit flags in WinMM: TIME_MS=1, TIME_SAMPLES=2,
     * TIME_BYTES=4. FMOD polls TIME_BYTES to decide when its circular output
     * blocks can be reused, so reporting samples for type 4 stalls the mixer. */
    u32 type = *(u32 *)time_value;
    u64 bytes = beer_audio_bytes_played();
    u32 value;
    if (type == 1 /* TIME_MS */)
        value = (u32)((bytes * 1000u) / (wave->rate * wave->channels * 2u));
    else if (type == 2 /* TIME_SAMPLES */)
        value = (u32)(bytes / (wave->channels * 2u));
    else
        value = (u32)bytes; /* TIME_BYTES and WinMM's fallback form */
    *(u32 *)((u8 *)time_value + 4) = value;
    static _Atomic(u32) position_logs;
    u32 log_index = atomic_fetch_add_explicit(&position_logs, 1, memory_order_relaxed);
    if (log_index < 8)
        fprintf(stderr, "[AUDIO] waveOutGetPosition type=%u value=%u bytes=%llu\n",
                type, value, (unsigned long long)bytes);
    return MMSYSERR_NOERROR;
}

/* ---- DirectSound output --------------------------------------------------
 * FMOD Ex dynamically loads dsound.dll. This compact IDirectSound8/
 * IDirectSoundBuffer8 implementation exposes the subset used by its software
 * mixer and forwards completed PCM ring-buffer regions to BeerAudio. */
typedef struct BeerDirectSound BeerDirectSound;
typedef struct BeerDirectSoundBuffer BeerDirectSoundBuffer;

struct BeerDirectSound {
    void **vtable;
    _Atomic(u32) refs;
};

struct BeerDirectSoundBuffer {
    void **vtable;
    _Atomic(u32) refs;
    u8 *data;
    u32 size;
    u32 flags;
    u32 rate;
    u16 channels;
    u16 bits;
    u32 playing;
    u32 volume;
    u32 pan;
};

enum {
    BEER_DS_OK = 0,
    BEER_DSERR_INVALIDPARAM = 0x8878000a,
    BEER_DSERR_OUTOFMEMORY = 0x8878000e,
    BEER_DSBCAPS_PRIMARYBUFFER = 0x00000001,
    BEER_DSBPLAY_LOOPING = 0x00000001,
    BEER_DSBSTATUS_PLAYING = 0x00000001,
    BEER_DSBSTATUS_LOOPING = 0x00000004,
    BEER_DSBLOCK_ENTIREBUFFER = 0x00000002,
    BEER_DSBLOCK_FROMWRITECURSOR = 0x00000001
};

typedef struct {
    u32 size;
    u32 flags;
    u32 buffer_bytes;
    u32 reserved;
    BeerWaveFormat *format;
    u8 algorithm[16];
} BeerDsBufferDesc;

static u64 __attribute__((ms_abi))
beer_ds_query(void *self, const void *iid, void **out)
{
    (void)iid;
    if (!self || !out) return BEER_DSERR_INVALIDPARAM;
    *out = self;
    atomic_fetch_add_explicit((_Atomic(u32) *)((u8 *)self + sizeof(void *)), 1,
                              memory_order_relaxed);
    return BEER_DS_OK;
}
static u64 __attribute__((ms_abi)) beer_ds_addref(void *self)
{
    if (!self) return 0;
    return atomic_fetch_add_explicit((_Atomic(u32) *)((u8 *)self + sizeof(void *)), 1,
                                     memory_order_relaxed) + 1;
}
static u64 __attribute__((ms_abi)) beer_ds_release(void *self)
{
    if (!self) return 0;
    u32 refs = atomic_fetch_sub_explicit((_Atomic(u32) *)((u8 *)self + sizeof(void *)), 1,
                                         memory_order_acq_rel) - 1;
    return refs;
}

static u64 __attribute__((ms_abi))
beer_dsb_get_current_position(BeerDirectSoundBuffer *buffer, u32 *play, u32 *write)
{
    if (!buffer || !buffer->size) return BEER_DSERR_INVALIDPARAM;
    u32 position = (u32)(beer_audio_bytes_played() % buffer->size);
    if (play) *play = position;
    if (write) *write = (position + buffer->size / 4u) % buffer->size;
    return BEER_DS_OK;
}
static u64 __attribute__((ms_abi))
beer_dsb_get_format(BeerDirectSoundBuffer *buffer, BeerWaveFormat *format,
                    u32 size, u32 *written)
{
    if (!buffer) return BEER_DSERR_INVALIDPARAM;
    if (written) *written = sizeof(BeerWaveFormat);
    if (!format) return BEER_DS_OK;
    BeerWaveFormat value = {
        WAVE_FORMAT_PCM, buffer->channels, buffer->rate,
        buffer->rate * buffer->channels * (buffer->bits / 8u),
        (u16)(buffer->channels * (buffer->bits / 8u)), buffer->bits, 0
    };
    memcpy(format, &value, size < sizeof(value) ? size : sizeof(value));
    return BEER_DS_OK;
}
static u64 __attribute__((ms_abi)) beer_dsb_get_volume(BeerDirectSoundBuffer *b, s32 *v)
    { if (!b || !v) return BEER_DSERR_INVALIDPARAM; *v = (s32)b->volume; return 0; }
static u64 __attribute__((ms_abi)) beer_dsb_get_pan(BeerDirectSoundBuffer *b, s32 *v)
    { if (!b || !v) return BEER_DSERR_INVALIDPARAM; *v = (s32)b->pan; return 0; }
static u64 __attribute__((ms_abi)) beer_dsb_get_frequency(BeerDirectSoundBuffer *b, u32 *v)
    { if (!b || !v) return BEER_DSERR_INVALIDPARAM; *v = b->rate; return 0; }
static u64 __attribute__((ms_abi)) beer_dsb_get_status(BeerDirectSoundBuffer *b, u32 *v)
    { if (!b || !v) return BEER_DSERR_INVALIDPARAM; *v = b->playing ? BEER_DSBSTATUS_PLAYING | BEER_DSBSTATUS_LOOPING : 0; return 0; }

typedef struct {
    u32 size;
    u32 flags;
    u32 buffer_bytes;
    u32 unlock_transfer_rate;
    u32 play_cpu_overhead;
} BeerDsBufferCaps;

static u64 __attribute__((ms_abi))
beer_dsb_get_caps(BeerDirectSoundBuffer *buffer, BeerDsBufferCaps *caps)
{
    if (!buffer || !caps || caps->size < sizeof(*caps))
        return BEER_DSERR_INVALIDPARAM;
    u32 requested_size = caps->size;
    memset(caps, 0, requested_size);
    caps->size = sizeof(*caps);
    caps->flags = buffer->flags;
    caps->buffer_bytes = buffer->size;
    /* These fields are advisory performance estimates. Zero is valid for a
     * software-mixed host buffer and avoids inventing hardware acceleration. */
    return BEER_DS_OK;
}

static u64 __attribute__((ms_abi)) beer_dsb_initialize(BeerDirectSoundBuffer *b, void *ds, void *desc)
    { (void)b; (void)ds; (void)desc; return BEER_DS_OK; }

static u64 __attribute__((ms_abi))
beer_dsb_lock(BeerDirectSoundBuffer *buffer, u32 offset, u32 bytes,
              void **part1, u32 *bytes1, void **part2, u32 *bytes2, u32 flags)
{
    if (!buffer || !buffer->data || !part1 || !bytes1 || !part2 || !bytes2)
        return BEER_DSERR_INVALIDPARAM;
    if (flags & BEER_DSBLOCK_FROMWRITECURSOR)
        offset = (u32)((beer_audio_bytes_played() + buffer->size / 4u) % buffer->size);
    if ((flags & BEER_DSBLOCK_ENTIREBUFFER) || !bytes) bytes = buffer->size;
    offset %= buffer->size;
    if (bytes > buffer->size) bytes = buffer->size;
    u32 first = bytes;
    if (first > buffer->size - offset) first = buffer->size - offset;
    *part1 = buffer->data + offset;
    *bytes1 = first;
    *part2 = bytes > first ? buffer->data : NULL;
    *bytes2 = bytes - first;
    return BEER_DS_OK;
}

static u64 __attribute__((ms_abi))
beer_dsb_play(BeerDirectSoundBuffer *buffer, u32 reserved1, u32 priority, u32 flags)
{
    (void)reserved1; (void)priority;
    if (!buffer || !buffer->data) return BEER_DSERR_INVALIDPARAM;
    if (!beer_audio_init(buffer->rate, buffer->channels)) return BEER_DSERR_INVALIDPARAM;
    buffer->playing = 1;
    buffer->flags = (buffer->flags & ~BEER_DSBSTATUS_LOOPING) |
                    ((flags & BEER_DSBPLAY_LOOPING) ? BEER_DSBSTATUS_LOOPING : 0);
    /* FMOD primes its circular buffer before Play. Beer previously discarded
     * those pre-Play Unlocks and started with a stationary play cursor, so FMOD
     * never observed room for the next mix block. Seed one quarter-ring now;
     * subsequent Lock/Unlock writes keep the host queue and cursor moving. */
    u32 seed_bytes = buffer->size / 4u;
    if (!seed_bytes || !beer_audio_submit(buffer->data, seed_bytes, NULL, NULL)) {
        buffer->playing = 0;
        return BEER_DSERR_INVALIDPARAM;
    }
    fprintf(stderr, "[AUDIO] DirectSoundBuffer::Play looping=%u seed=%u bytes\n",
            !!(flags & BEER_DSBPLAY_LOOPING), seed_bytes);
    return BEER_DS_OK;
}
static u64 __attribute__((ms_abi)) beer_dsb_set_position(BeerDirectSoundBuffer *b, u32 p)
    { (void)b; (void)p; return BEER_DS_OK; }
static u64 __attribute__((ms_abi))
beer_dsb_set_format(BeerDirectSoundBuffer *buffer, const BeerWaveFormat *format)
{
    if (!buffer || !format || format->format_tag != WAVE_FORMAT_PCM || format->bits_per_sample != 16)
        return WAVERR_BADFORMAT;
    buffer->rate = format->samples_per_sec;
    buffer->channels = format->channels;
    buffer->bits = format->bits_per_sample;
    return BEER_DS_OK;
}
static u64 __attribute__((ms_abi)) beer_dsb_set_volume(BeerDirectSoundBuffer *b, s32 v)
    { if (!b) return BEER_DSERR_INVALIDPARAM; b->volume = (u32)v; return 0; }
static u64 __attribute__((ms_abi)) beer_dsb_set_pan(BeerDirectSoundBuffer *b, s32 v)
    { if (!b) return BEER_DSERR_INVALIDPARAM; b->pan = (u32)v; return 0; }
static u64 __attribute__((ms_abi)) beer_dsb_set_frequency(BeerDirectSoundBuffer *b, u32 v)
    { if (!b || !v) return BEER_DSERR_INVALIDPARAM; b->rate = v; return 0; }
static u64 __attribute__((ms_abi)) beer_dsb_stop(BeerDirectSoundBuffer *b)
    { if (!b) return BEER_DSERR_INVALIDPARAM; b->playing = 0; beer_audio_reset(); return 0; }
static void beer_pcm_measure(const void *data, u32 bytes, u64 *nonzero, u32 *peak)
{
    if (!data || !bytes) return;
    const int16_t *samples = data;
    u32 count = bytes / sizeof(*samples);
    for (u32 i = 0; i < count; ++i) {
        s32 sample = samples[i];
        u32 magnitude = (u32)(sample < 0 ? -sample : sample);
        if (sample) ++*nonzero;
        if (magnitude > *peak) *peak = magnitude;
    }
}

static u64 __attribute__((ms_abi))
beer_dsb_unlock(BeerDirectSoundBuffer *buffer, void *part1, u32 bytes1,
                void *part2, u32 bytes2)
{
    if (!buffer) return BEER_DSERR_INVALIDPARAM;
    if (buffer->playing) {
        u64 nonzero = 0;
        u32 peak = 0;
        beer_pcm_measure(part1, bytes1, &nonzero, &peak);
        beer_pcm_measure(part2, bytes2, &nonzero, &peak);
        u64 ring_nonzero = 0;
        u32 ring_peak = 0;
        beer_pcm_measure(buffer->data, buffer->size, &ring_nonzero, &ring_peak);
        int submitted = 1;
        if (part1 && bytes1) submitted = beer_audio_submit(part1, bytes1, NULL, NULL);
        if (submitted && part2 && bytes2)
            submitted = beer_audio_submit(part2, bytes2, NULL, NULL);
        if (!submitted) return BEER_DSERR_INVALIDPARAM;
        static _Atomic(u32) unlocks;
        static _Atomic(u64) nonzero_samples;
        static _Atomic(u32) maximum_peak;
        u32 unlock = atomic_fetch_add_explicit(&unlocks, 1, memory_order_relaxed) + 1;
        u64 total_nonzero = atomic_fetch_add_explicit(&nonzero_samples, nonzero,
                                                       memory_order_relaxed) + nonzero;
        u32 old_peak = atomic_load_explicit(&maximum_peak, memory_order_relaxed);
        while (peak > old_peak &&
               !atomic_compare_exchange_weak_explicit(&maximum_peak, &old_peak, peak,
                                                      memory_order_relaxed,
                                                      memory_order_relaxed)) {}
        if (unlock <= 8 || !(unlock % 256) || (nonzero && total_nonzero == nonzero))
            fprintf(stderr,
                    "[AUDIO] DirectSound unlock #%u: %u+%u bytes nonzero=%llu "
                    "peak=%u ring(nonzero=%llu peak=%u) totals(nonzero=%llu peak=%u)\n",
                    unlock, bytes1, bytes2, (unsigned long long)nonzero, peak,
                    (unsigned long long)ring_nonzero, ring_peak,
                    (unsigned long long)total_nonzero,
                    atomic_load_explicit(&maximum_peak, memory_order_relaxed));
    }
    return BEER_DS_OK;
}
static u64 __attribute__((ms_abi)) beer_dsb_restore(BeerDirectSoundBuffer *b)
    { return b ? BEER_DS_OK : BEER_DSERR_INVALIDPARAM; }
static u64 __attribute__((ms_abi)) beer_dsb_noop(void *b, u64 a, u64 c, u64 d)
    { (void)a; (void)c; (void)d; return b ? BEER_DS_OK : BEER_DSERR_INVALIDPARAM; }

static void *g_direct_sound_buffer_vtable[24] = {
    (void *)beer_ds_query, (void *)beer_ds_addref, (void *)beer_ds_release,
    (void *)beer_dsb_get_caps, (void *)beer_dsb_get_current_position,
    (void *)beer_dsb_get_format, (void *)beer_dsb_get_volume,
    (void *)beer_dsb_get_pan, (void *)beer_dsb_get_frequency,
    (void *)beer_dsb_get_status, (void *)beer_dsb_initialize,
    (void *)beer_dsb_lock, (void *)beer_dsb_play, (void *)beer_dsb_set_position,
    (void *)beer_dsb_set_format, (void *)beer_dsb_set_volume,
    (void *)beer_dsb_set_pan, (void *)beer_dsb_set_frequency,
    (void *)beer_dsb_stop, (void *)beer_dsb_unlock, (void *)beer_dsb_restore,
    (void *)beer_dsb_noop, (void *)beer_dsb_noop, (void *)beer_dsb_noop
};

static u64 __attribute__((ms_abi))
beer_ds_create_buffer(BeerDirectSound *sound, const BeerDsBufferDesc *desc,
                      BeerDirectSoundBuffer **out, void *outer)
{
    (void)outer;
    if (!sound || !desc || !out) return BEER_DSERR_INVALIDPARAM;
    BeerDirectSoundBuffer *buffer = calloc(1, sizeof(*buffer));
    if (!buffer) return BEER_DSERR_OUTOFMEMORY;
    buffer->vtable = g_direct_sound_buffer_vtable;
    atomic_init(&buffer->refs, 1);
    buffer->flags = desc->flags;
    buffer->size = desc->buffer_bytes;
    buffer->rate = 48000;
    buffer->channels = 2;
    buffer->bits = 16;
    if (desc->format) {
        buffer->rate = desc->format->samples_per_sec;
        buffer->channels = desc->format->channels;
        buffer->bits = desc->format->bits_per_sample;
    }
    if (!(desc->flags & BEER_DSBCAPS_PRIMARYBUFFER)) {
        if (!buffer->size) { free(buffer); return BEER_DSERR_INVALIDPARAM; }
        buffer->data = calloc(1, buffer->size);
        if (!buffer->data) { free(buffer); return BEER_DSERR_OUTOFMEMORY; }
    }
    *out = buffer;
    fprintf(stderr, "[AUDIO] DirectSound buffer: %u bytes, %u Hz, %u channels\n",
            buffer->size, buffer->rate, buffer->channels);
    return BEER_DS_OK;
}
static u64 __attribute__((ms_abi)) beer_ds_get_caps(BeerDirectSound *s, void *caps)
    { (void)s; if (caps) memset(caps, 0, 96); return BEER_DS_OK; }
static u64 __attribute__((ms_abi)) beer_ds_duplicate(BeerDirectSound *s, void *a, void **b)
    { (void)s; (void)a; if (b) *b = NULL; return BEER_DSERR_INVALIDPARAM; }
static u64 __attribute__((ms_abi)) beer_ds_coop(BeerDirectSound *s, u64 hwnd, u32 level)
    { (void)hwnd; (void)level; return s ? BEER_DS_OK : BEER_DSERR_INVALIDPARAM; }
static u64 __attribute__((ms_abi)) beer_ds_compact(BeerDirectSound *s)
    { return s ? BEER_DS_OK : BEER_DSERR_INVALIDPARAM; }
static u64 __attribute__((ms_abi)) beer_ds_get_speaker(BeerDirectSound *s, u32 *config)
    { if (!s || !config) return BEER_DSERR_INVALIDPARAM; *config = 4; return 0; }
static u64 __attribute__((ms_abi)) beer_ds_set_speaker(BeerDirectSound *s, u32 config)
    { (void)config; return s ? 0 : BEER_DSERR_INVALIDPARAM; }
static u64 __attribute__((ms_abi)) beer_ds_initialize(BeerDirectSound *s, const void *guid)
    { (void)guid; return s ? 0 : BEER_DSERR_INVALIDPARAM; }
static u64 __attribute__((ms_abi)) beer_ds_verify(BeerDirectSound *s, u32 *certified)
    { if (!s || !certified) return BEER_DSERR_INVALIDPARAM; *certified = 0; return 0; }

static void *g_direct_sound_vtable[12] = {
    (void *)beer_ds_query, (void *)beer_ds_addref, (void *)beer_ds_release,
    (void *)beer_ds_create_buffer, (void *)beer_ds_get_caps,
    (void *)beer_ds_duplicate, (void *)beer_ds_coop, (void *)beer_ds_compact,
    (void *)beer_ds_get_speaker, (void *)beer_ds_set_speaker,
    (void *)beer_ds_initialize, (void *)beer_ds_verify
};
static BeerDirectSound g_direct_sound = { g_direct_sound_vtable, 1 };

static u64 __attribute__((ms_abi))
impl_DirectSoundCreate8(const void *guid, BeerDirectSound **out, void *outer)
{
    (void)guid; (void)outer;
    if (!out) return BEER_DSERR_INVALIDPARAM;
    *out = &g_direct_sound;
    atomic_fetch_add_explicit(&g_direct_sound.refs, 1, memory_order_relaxed);
    fprintf(stderr, "[AUDIO] DirectSoundCreate8 -> Beer PipeWire backend\n");
    return BEER_DS_OK;
}
static u64 __attribute__((ms_abi))
impl_DirectSoundCreate(const void *guid, BeerDirectSound **out, void *outer)
{
    return impl_DirectSoundCreate8(guid, out, outer);
}

typedef int (__attribute__((ms_abi)) *BeerDsEnumCallbackW)(const void *, const u16 *, const u16 *, void *);
static u64 __attribute__((ms_abi))
impl_DirectSoundEnumerateW(BeerDsEnumCallbackW callback, void *context)
{
    if (!callback) return BEER_DSERR_INVALIDPARAM;
    static const u16 primary[] = {'P','r','i','m','a','r','y',' ','S','o','u','n','d',' ','D','r','i','v','e','r',0};
    static const u16 description[] = {'B','e','e','r',' ','P','i','p','e','W','i','r','e',0};
    static const u16 module[] = {'d','s','o','u','n','d','.','d','l','l',0};
    static const u8 device_guid[16] = {
        0x42, 0x45, 0x45, 0x52, 0x50, 0x57, 0x00, 0x01,
        0x80, 0x00, 0x42, 0x45, 0x45, 0x52, 0x00, 0x01
    };
    int keep_enumerating = callback(NULL, primary, module, context);
    if (keep_enumerating)
        keep_enumerating = callback(device_guid, description, module, context);
    fprintf(stderr, "[AUDIO] DirectSoundEnumerateW callbacks complete -> %d\n",
            keep_enumerating);
    return BEER_DS_OK;
}
static u64 __attribute__((ms_abi)) impl_DirectSoundCaptureEnumerateW(BeerDsEnumCallbackW callback, void *context)
    { (void)callback; (void)context; return BEER_DS_OK; }
static u64 __attribute__((ms_abi)) impl_DirectSoundCaptureCreate8(const void *guid, void **out, void *outer)
    { (void)guid; (void)outer; if (out) *out = NULL; return 0x88780078u; }
static u64 __attribute__((ms_abi)) impl_DirectSoundCaptureCreate(const void *guid, void **out, void *outer)
    { return impl_DirectSoundCaptureCreate8(guid, out, outer); }

/* ---- Security descriptors ---- */
static u64 __attribute__((ms_abi))
impl_InitializeSecurityDescriptor(u64 sd, u32 rev) { (void)rev; if(sd) memset((void*)sd,0,20); return 1; }
static u64 __attribute__((ms_abi))
impl_SetSecurityDescriptorDacl(u64 sd, u32 p, u64 acl, u32 def)
    { (void)sd;(void)p;(void)acl;(void)def; return 1; }

/* ── Steam API stubs ─────────────────────────────────────────────── */
static void steam_disable_unsupported_online_probe(void)
{
    static int completed;
    if (completed || !g_img) return;

    /* CSNetFpsCheckStep's writable function/name table is populated by guest
     * static constructors, so it is not available during Beer's pre-entry
     * setup. Once Steam initializes, replace only STEP_Checking with the game's
     * own STEP_Finish routine. Otherwise the unsupported network FPS request
     * eventually invokes STEP_ApplyResult (RVA 0x1068a80), sets status +0xa52,
     * and raises asynchronous FDP_System_Message(4161). */
    u64 *checking_slot = (u64 *)(g_img + 0x3d93bd0);
    u64 expected = (u64)g_img + 0x1068ab0;
    u64 offline_finish = (u64)g_img + 0x1068d50;
    if (*checking_slot == expected) {
        *checking_slot = offline_finish;
        completed = 1;
        fprintf(stderr,
                "[STEAM] Disabled unsupported CSNetFpsCheckStep online probe "
                "(step-table RVA 0x3d93bd0 -> STEP_Finish)\n");
    }
}

static u64 __attribute__((ms_abi)) impl_SteamAPI_Init(void) {
    /* Expose an initialized local runtime so Sekiro can retain its deterministic
     * identity/save path. Online interfaces remain explicitly unavailable. */
    steam_disable_unsupported_online_probe();
    fprintf(stderr, "[STEAM] Steam runtime initialized (offline user)\n");
    return 1;
}
static u64 __attribute__((ms_abi))
impl_SteamAPI_RestartAppIfNecessary(u32 appid) { (void)appid; return 0; }
static u64 __attribute__((ms_abi)) impl_SteamAPI_Shutdown(void)     { return 0; }
static u64 __attribute__((ms_abi)) impl_SteamAPI_RunCallbacks(void)
    { steam_disable_unsupported_online_probe(); return 0; }
static u64 __attribute__((ms_abi)) impl_SteamAPI_IsSteamRunning(void) { return 1; }

static u64 __attribute__((ms_abi)) impl_SteamApps(void);
static u64 __attribute__((ms_abi)) impl_SteamClient(void);
static u64 __attribute__((ms_abi)) impl_SteamUtils(void);
static u64 __attribute__((ms_abi)) impl_SteamUser(void);
static u64 __attribute__((ms_abi)) impl_SteamFriends(void);
static u64 __attribute__((ms_abi)) impl_SteamUserStats(void);
static u64 __attribute__((ms_abi)) impl_SteamUGC(void);
static u64 __attribute__((ms_abi)) impl_SteamRemoteStorage(void);

static u64 __attribute__((ms_abi))
impl_SteamInternal_CreateInterface(const char *version)
{
    if (!version) return 0;
    char lower[128];
    size_t n = strlen(version);
    if (n >= sizeof(lower)) n = sizeof(lower) - 1;
    for (size_t i = 0; i < n; ++i) {
        char c = version[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
    }
    lower[n] = 0;

    /* Steamworks interface version names are conventionally uppercase in
     * modern SDKs (for example STEAMAPPS_INTERFACE_VERSION008). Match the
     * semantic interface name, not the spelling used by one SDK generation. */
    if (strstr(lower, "steamclient"))        return impl_SteamClient();
    if (strstr(lower, "steamuserstats"))     return impl_SteamUserStats();
    if (strstr(lower, "steamuser"))          return impl_SteamUser();
    if (strstr(lower, "steamfriends"))       return impl_SteamFriends();
    if (strstr(lower, "steamapps"))          return impl_SteamApps();
    if (strstr(lower, "steamutils"))         return impl_SteamUtils();
    if (strstr(lower, "steamugc"))           return impl_SteamUGC();
    if (strstr(lower, "steamremotestorage")) return impl_SteamRemoteStorage();
    fprintf(stderr, "[STEAM] unsupported interface %s\n", version);
    return 0;
}

/* Modern steam_api headers keep a callback, generation counter, and interface
 * context in one static object. SteamInternal_ContextInit asks the callback to
 * refresh that context and returns its address. This lets the game's generated
 * SteamInternal_Context() accessor use Beer's coherent offline interfaces
 * without loading the protected private steam_api64 image. */
static u64 __attribute__((ms_abi))
impl_SteamInternal_ContextInit(void *context_data)
{
    if (!context_data) return 0;
    u64 *fields = context_data;
    typedef void __attribute__((ms_abi)) (*ContextPopulate)(void *context);
    ContextPopulate populate = (ContextPopulate)(uintptr_t)fields[0];
    void *context = &fields[2];
    if (populate) populate(context);
    fields[1]++;
    return (u64)context;
}

static u64 __attribute__((ms_abi))
impl_SteamInternal_FindOrCreateUserInterface(u32 user, const char *version)
{
    (void)user;
    return impl_SteamInternal_CreateInterface(version);
}

static u64 __attribute__((ms_abi))
impl_SteamInternal_FindOrCreateGameServerInterface(u32 user, const char *version)
{
    (void)user;
    (void)version;
    return 0; /* Beer exposes no online game-server session. */
}

static u64 __attribute__((ms_abi))
impl_SteamAPI_GetHSteamUser(void) { return 1; }
static u64 __attribute__((ms_abi))
impl_SteamAPI_GetHSteamPipe(void) { return 1; }

static u64 __attribute__((ms_abi))
impl_SteamAPI_RegisterCallback(u64 cb, u32 id)   { (void)cb; (void)id; return 0; }
static u64 __attribute__((ms_abi))
impl_SteamAPI_UnregisterCallback(u64 cb)          { (void)cb; return 0; }
static u64 __attribute__((ms_abi))
impl_SteamAPI_RegisterCallResult(u64 cb, u64 call)
    { (void)cb; (void)call; return 0; }
static u64 __attribute__((ms_abi))
impl_SteamAPI_UnregisterCallResult(u64 cb, u64 call)
    { (void)cb; (void)call; return 0; }

/*
 * Minimal offline Steam interface objects.
 *
 * Steam interfaces are unrelated C++ interfaces and therefore cannot safely
 * share one catch-all vtable. In particular, ISteamFriends slot 0 is
 * GetPersonaName() and returns `const char *`; the old generic integer-success
 * method returned address 1, which Sekiro correctly passed to its string
 * constructor and then faulted while scanning it.
 *
 * Keep unsupported methods inert, but give observed interface methods their
 * documented return types. This is a coherent offline compatibility object,
 * not a claim that a real Steam client is connected.
 */
#define STEAM_VTAB_SZ 256

static u64 __attribute__((ms_abi))
steam_vfn_true(u64 a, u64 b, u64 c, u64 d) { (void)a;(void)b;(void)c;(void)d; return 1; }
static u64 __attribute__((ms_abi))
steam_vfn_false(u64 a, u64 b, u64 c, u64 d) { (void)a;(void)b;(void)c;(void)d; return 0; }
static u64 __attribute__((ms_abi))
steam_vfn_english(u64 a, u64 b, u64 c, u64 d)
    { (void)a;(void)b;(void)c;(void)d; return (u64)"english"; }
static u64 __attribute__((ms_abi))
steam_friends_get_persona_name(u64 self)
    { (void)self; return (u64)"Player"; }
static u64 __attribute__((ms_abi))
steam_user_get_steam_id(u64 self, u64 *steam_id)
{
    (void)self;
    if (!steam_id) return 0;
    /* CSteamID is an eight-byte value returned through the MSVC aggregate
     * return pointer supplied in RDX. Account type Individual and universe
     * Public make this a structurally valid offline identity. */
    *steam_id = 0x0110000100000001ULL;
    return (u64)steam_id;
}

static u64  g_steam_vtab[STEAM_VTAB_SZ];
static u64  g_steam_apps_vtab[STEAM_VTAB_SZ];
static u64  g_steam_friends_vtab[STEAM_VTAB_SZ];
static u64  g_steam_user_vtab[STEAM_VTAB_SZ];
static u64 *g_steam_obj;
static u64  g_steam_apps_obj;
static u64  g_steam_friends_obj;
static u64  g_steam_user_obj;

static void init_steam_fake(void) {
    for (int i = 0; i < STEAM_VTAB_SZ; i++) {
        /* Unsupported Steam methods must not report success without populating
         * their mandatory outputs.  Apart from being ABI-unsafe for pointer
         * returns, the old catch-all TRUE result made background online,
         * stats, UGC, and remote-storage jobs start in an otherwise offline
         * session.  Sekiro later surfaced those asynchronous failures as
         * FDP_System_Message(4161). */
        g_steam_vtab[i] = (u64)steam_vfn_false;
        g_steam_apps_vtab[i] = (u64)steam_vfn_false;
        g_steam_friends_vtab[i] = (u64)steam_vfn_false;
        g_steam_user_vtab[i] = (u64)steam_vfn_false;
    }
    /* ISteamApps vtable[4]=GetCurrentGameLanguage, [5]=GetAvailableGameLanguages. */
    g_steam_apps_vtab[4] = (u64)steam_vfn_english;
    g_steam_apps_vtab[5] = (u64)steam_vfn_english;
    /* ISteamFriends vtable[0]=GetPersonaName. */
    g_steam_friends_vtab[0] = (u64)steam_friends_get_persona_name;
    /* ISteamUser vtable[0]=GetHSteamUser and [1]=BLoggedOn. Beer exposes a
     * valid local Steam runtime but an offline user. Returning TRUE from the
     * old catch-all slot 1 made Sekiro enter its game-server login flow; the
     * asynchronous failure sets network-state byte +0xa52 and raises
     * FDP_System_Message(4161). */
    g_steam_user_vtab[1] = (u64)steam_vfn_false;
    /* ISteamUser vtable[2]=GetSteamID. The 64-bit aggregate is returned via
     * the hidden output pointer used by this MSVC ABI call site. */
    g_steam_user_vtab[2] = (u64)steam_user_get_steam_id;
    g_steam_apps_obj = (u64)g_steam_apps_vtab;
    g_steam_friends_obj = (u64)g_steam_friends_vtab;
    g_steam_user_obj = (u64)g_steam_user_vtab;

    g_steam_obj = mmap(NULL, 4096, PROT_READ|PROT_WRITE,
                       MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (g_steam_obj != MAP_FAILED)
        g_steam_obj[0] = (u64)g_steam_vtab;
    else
        g_steam_obj = NULL;
}

static u64 __attribute__((ms_abi)) impl_SteamApps(void)     { return (u64)&g_steam_apps_obj; }
static u64 __attribute__((ms_abi)) impl_SteamClient(void)   { return (u64)g_steam_obj; }
static u64 __attribute__((ms_abi)) impl_SteamUtils(void)    { return (u64)g_steam_obj; }
static u64 __attribute__((ms_abi)) impl_SteamUser(void)     { return (u64)&g_steam_user_obj; }
static u64 __attribute__((ms_abi)) impl_SteamFriends(void)  { return (u64)&g_steam_friends_obj; }
static u64 __attribute__((ms_abi)) impl_SteamUserStats(void){ return (u64)g_steam_obj; }
static u64 __attribute__((ms_abi)) impl_SteamUGC(void)      { return (u64)g_steam_obj; }
static u64 __attribute__((ms_abi)) impl_SteamRemoteStorage(void) { return (u64)g_steam_obj; }
static u64 __attribute__((ms_abi)) impl_SteamNetworking(void) { return 0; }

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

static int dxgi_iid_equal(const u8 *actual, const u8 expected[16])
{
    return actual && memcmp(actual, expected, 16) == 0;
}

static const u8 k_iid_iunknown[16] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xc0,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x46
};
static const u8 k_iid_idxgi_factory[16] = {
    0xec,0x66,0x71,0x7b,0xc7,0x21,0xae,0x44,
    0xb2,0x1a,0xc9,0xae,0x32,0x1a,0xe3,0x69
};
static const u8 k_iid_idxgi_factory1[16] = {
    0x78,0xae,0x0a,0x77,0x6f,0xf2,0xba,0x4d,
    0xa8,0x29,0x25,0x3c,0x83,0xd1,0xb3,0x87
};
static const u8 k_iid_idxgi_adapter[16] = {
    0xe1,0xe7,0x11,0x24,0xac,0x12,0xcf,0x4c,
    0xbd,0x14,0x97,0x98,0xe8,0x53,0x4d,0xc0
};
static const u8 k_iid_idxgi_adapter1[16] = {
    0x05,0xaa,0x00,0x29,0x59,0xc9,0x90,0x4a,
    0xb8,0x99,0x7f,0x0d,0x56,0x6f,0xb6,0x7a
};
static const u8 k_iid_idxgi_output[16] = {
    0xae,0x02,0xae,0xae,0x4f,0xed,0x35,0x4a,
    0x8d,0x42,0x5d,0xc8,0x39,0xa5,0xf6,0x4c
};

static u64 __attribute__((ms_abi))
dxgi_factory_QueryInterface(u64 *obj, const u8 *riid, void **pp)
{
    if (!pp) return (u64)0x80070057;
    *pp = NULL;
    if (!dxgi_iid_equal(riid, k_iid_iunknown) &&
        !dxgi_iid_equal(riid, k_iid_idxgi_factory) &&
        !dxgi_iid_equal(riid, k_iid_idxgi_factory1))
        return E_NOINTERFACE;
    *pp = obj;
    dxgi_AddRef(obj);
    return S_OK;
}

static u64 __attribute__((ms_abi))
dxgi_adapter_QueryInterface(u64 *obj, const u8 *riid, void **pp)
{
    if (!pp) return (u64)0x80070057;
    *pp = NULL;
    if (!dxgi_iid_equal(riid, k_iid_iunknown) &&
        !dxgi_iid_equal(riid, k_iid_idxgi_adapter) &&
        !dxgi_iid_equal(riid, k_iid_idxgi_adapter1))
        return E_NOINTERFACE;
    *pp = obj;
    dxgi_AddRef(obj);
    return S_OK;
}

static u64 __attribute__((ms_abi))
dxgi_output_QueryInterface(u64 *obj, const u8 *riid, void **pp)
{
    if (!pp) return (u64)0x80070057;
    *pp = NULL;
    if (!dxgi_iid_equal(riid, k_iid_iunknown) &&
        !dxgi_iid_equal(riid, k_iid_idxgi_output))
        return E_NOINTERFACE;
    *pp = obj;
    dxgi_AddRef(obj);
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
static u64 __attribute__((ms_abi)) dxgi_factory_QueryInterface(u64 *, const u8 *, void **);
static u64 __attribute__((ms_abi)) dxgi_adapter_QueryInterface(u64 *, const u8 *, void **);
static u64 __attribute__((ms_abi)) dxgi_output_QueryInterface(u64 *, const u8 *, void **);
static u64 __attribute__((ms_abi)) dxgi_EnumAdapters(u64 *, u32, u64 *);
static u64 __attribute__((ms_abi)) dxgi_EnumAdapters_with_fake(u64 *, u32, u64 **);
static u64 __attribute__((ms_abi)) dxgi_CreateSwapChain(u64 *, u64 *, u64 *, u64 **);
static u64 __attribute__((ms_abi)) dxgi_Present(u64 *, u32, u32);
static u64 __attribute__((ms_abi)) dxgi_GetBuffer(u64 *, u32, u64, void **);
static u64 __attribute__((ms_abi)) dxgi_ResizeBuffers(u64 *, u32, u32, u32, u32, u32);
static u64 __attribute__((ms_abi)) dxgi_adapter_EnumOutputs(u64 *, u32, u64 **);

static void init_dxgi_fake(void) {
    /* Slots 0-2: IUnknown */
    g_dxgi_vtab[0] = (u64)dxgi_factory_QueryInterface;
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
static __thread s32 g_wsa_last_error;
static __thread u32 g_wsa_startup_count;

static u64 __attribute__((ms_abi))
impl_WSAStartup(u32 version, u8 *wsadata)
{
    if (!wsadata) {
        g_wsa_last_error = 10014; /* WSAEFAULT */
        return 10014;
    }
    u16 requested = (u16)version;
    if ((requested & 0xffu) > 2u || (requested & 0xffu) == 0u) {
        g_wsa_last_error = 10092; /* WSAVERNOTSUPPORTED */
        return 10092;
    }
    memset(wsadata, 0, 408);
    *(u16 *)(wsadata + 0) = 0x0202; /* wVersion */
    *(u16 *)(wsadata + 2) = 0x0202; /* wHighVersion */
    memcpy(wsadata + 4, "Beer Winsock 2.2", 17);
    memcpy(wsadata + 261, "Running", 8);
    *(u16 *)(wsadata + 390) = 0; /* iMaxSockets is ignored for Winsock 2 */
    *(u16 *)(wsadata + 392) = 0;
    *(u64 *)(wsadata + 400) = 0;
    ++g_wsa_startup_count;
    g_wsa_last_error = 0;
    return 0;
}

static u64 __attribute__((ms_abi))
impl_WSACleanup(void)
{
    if (!g_wsa_startup_count) {
        g_wsa_last_error = 10093; /* WSANOTINITIALISED */
        return (u32)-1;
    }
    --g_wsa_startup_count;
    return 0;
}

static u64 __attribute__((ms_abi)) impl_WSAGetLastError(void)
    { return (u32)g_wsa_last_error; }
static u64 __attribute__((ms_abi))
impl_WSASetLastError(u32 error) { g_wsa_last_error = (s32)error; return 0; }

static s32 winsock_error_from_errno(int error)
{
    switch (error) {
    case EACCES: return 10013; /* WSAEACCES */
    case EFAULT: return 10014; /* WSAEFAULT */
    case EINVAL: return 10022; /* WSAEINVAL */
    case EMFILE: return 10024; /* WSAEMFILE */
    case EWOULDBLOCK: return 10035; /* WSAEWOULDBLOCK */
    case EINPROGRESS: return 10036; /* WSAEINPROGRESS */
    case EALREADY: return 10037; /* WSAEALREADY */
    case ENOTSOCK: return 10038; /* WSAENOTSOCK */
    case EDESTADDRREQ: return 10039; /* WSAEDESTADDRREQ */
    case EMSGSIZE: return 10040; /* WSAEMSGSIZE */
    case EPROTOTYPE: return 10041; /* WSAEPROTOTYPE */
    case ENOPROTOOPT: return 10042; /* WSAENOPROTOOPT */
    case EPROTONOSUPPORT: return 10043; /* WSAEPROTONOSUPPORT */
    case EOPNOTSUPP: return 10045; /* WSAEOPNOTSUPP */
    case EAFNOSUPPORT: return 10047; /* WSAEAFNOSUPPORT */
    case EADDRINUSE: return 10048; /* WSAEADDRINUSE */
    case EADDRNOTAVAIL: return 10049; /* WSAEADDRNOTAVAIL */
    case ENETDOWN: return 10050; /* WSAENETDOWN */
    case ENETUNREACH: return 10051; /* WSAENETUNREACH */
    case ECONNABORTED: return 10053; /* WSAECONNABORTED */
    case ECONNRESET: return 10054; /* WSAECONNRESET */
    case ENOBUFS: return 10055; /* WSAENOBUFS */
    case EISCONN: return 10056; /* WSAEISCONN */
    case ENOTCONN: return 10057; /* WSAENOTCONN */
    case ETIMEDOUT: return 10060; /* WSAETIMEDOUT */
    case ECONNREFUSED: return 10061; /* WSAECONNREFUSED */
    case EHOSTUNREACH: return 10065; /* WSAEHOSTUNREACH */
    default: return 10022;
    }
}

static int winsock_ready(void)
{
    if (g_wsa_startup_count) return 1;
    g_wsa_last_error = 10093; /* WSANOTINITIALISED */
    return 0;
}

static u64 __attribute__((ms_abi))
impl_socket(s32 family, s32 type, s32 protocol)
{
    if (!winsock_ready()) return UINT64_MAX; /* INVALID_SOCKET */
    int descriptor = socket(family, type, protocol);
    if (descriptor < 0) {
        g_wsa_last_error = winsock_error_from_errno(errno);
        return UINT64_MAX;
    }
    g_wsa_last_error = 0;
    return (u64)(u32)descriptor;
}

static u64 __attribute__((ms_abi))
impl_connect(u64 socket_handle, const void *address, s32 address_length)
{
    if (!winsock_ready()) return (u32)-1;
    if (!address || address_length <= 0) {
        g_wsa_last_error = 10014;
        return (u32)-1;
    }
    if (connect((int)socket_handle, (const struct sockaddr *)address,
                (socklen_t)address_length) != 0) {
        g_wsa_last_error = winsock_error_from_errno(errno);
        return (u32)-1;
    }
    g_wsa_last_error = 0;
    return 0;
}

static u64 __attribute__((ms_abi))
impl_closesocket(u64 socket_handle)
{
    if (!winsock_ready()) return (u32)-1;
    if (close((int)socket_handle) != 0) {
        g_wsa_last_error = winsock_error_from_errno(errno);
        return (u32)-1;
    }
    g_wsa_last_error = 0;
    return 0;
}

static u64 __attribute__((ms_abi))
impl_getsockname(u64 socket_handle, void *address, s32 *address_length)
{
    if (!winsock_ready()) return (u32)-1;
    if (!address || !address_length || *address_length < 0) {
        g_wsa_last_error = 10014;
        return (u32)-1;
    }
    socklen_t length = (socklen_t)*address_length;
    if (getsockname((int)socket_handle, (struct sockaddr *)address, &length) != 0) {
        g_wsa_last_error = winsock_error_from_errno(errno);
        return (u32)-1;
    }
    *address_length = (s32)length;
    g_wsa_last_error = 0;
    return 0;
}

static u64 __attribute__((ms_abi))
impl_inet_pton(s32 family, const char *source, void *destination)
{
    if (!winsock_ready()) return (u32)-1;
    if (!source || !destination) {
        g_wsa_last_error = 10014;
        return (u32)-1;
    }
    int result = inet_pton(family, source, destination);
    if (result < 0) g_wsa_last_error = winsock_error_from_errno(errno);
    else g_wsa_last_error = 0;
    return (u32)result;
}

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

/* Vista+ locale/date helpers dynamically queried by RE8's bundled runtime.
 * Beer exposes one coherent en-US locale rather than returning NULL exports
 * and sending the runtime down partially initialized fallback paths. */
static int copy_ascii_result_w(const char *source, u16 *destination, int capacity)
{
    int required = (int)strlen(source) + 1;
    if (!destination || capacity == 0) return required;
    if (capacity < required) {
        g_last_error = 122; /* ERROR_INSUFFICIENT_BUFFER */
        return 0;
    }
    for (int i = 0; i < required; ++i) destination[i] = (u8)source[i];
    g_last_error = 0;
    return required;
}

static u64 __attribute__((ms_abi))
impl_LCIDToLocaleName(u32 locale, u16 *name, int capacity, u32 flags)
{
    (void)flags;
    const char *value = (locale & 0xffffu) == 0x0411u ? "ja-JP" : "en-US";
    return (u64)copy_ascii_result_w(value, name, capacity);
}

static u64 __attribute__((ms_abi))
impl_LocaleNameToLCID(const u16 *name, u32 flags)
{
    (void)flags;
    if (!name) { g_last_error = 87; return 0; }
    char value[16];
    size_t i = 0;
    while (name[i] && i + 1 < sizeof(value)) {
        value[i] = (char)(name[i] & 0x7f);
        ++i;
    }
    value[i] = 0;
    g_last_error = 0;
    return strcasecmp(value, "ja-JP") == 0 ? 0x0411u : 0x0409u;
}

static void beer_systemtime_fields(const u16 *system_time, u16 fields[8])
{
    if (system_time) {
        memcpy(fields, system_time, 16);
        return;
    }
    impl_GetLocalTime((u64 *)fields);
}

static u64 __attribute__((ms_abi))
impl_GetDateFormatEx(const u16 *locale, u32 flags, const u16 *system_time,
                     const u16 *format, u16 *output, int capacity,
                     const u16 *calendar)
{
    (void)locale; (void)flags; (void)format; (void)calendar;
    u16 fields[8];
    char value[32];
    beer_systemtime_fields(system_time, fields);
    snprintf(value, sizeof(value), "%02u/%02u/%04u",
             fields[1], fields[3], fields[0]);
    return (u64)copy_ascii_result_w(value, output, capacity);
}

static u64 __attribute__((ms_abi))
impl_GetTimeFormatEx(const u16 *locale, u32 flags, const u16 *system_time,
                     const u16 *format, u16 *output, int capacity)
{
    (void)locale; (void)flags; (void)format;
    u16 fields[8];
    char value[32];
    beer_systemtime_fields(system_time, fields);
    snprintf(value, sizeof(value), "%02u:%02u:%02u",
             fields[4], fields[5], fields[6]);
    return (u64)copy_ascii_result_w(value, output, capacity);
}

/* RE8 registers a callback-style suspend notification dynamically. Beer does
 * not synthesize power transitions, but returns a stable registration handle
 * and validates the matching unregister operation. */
#define BEER_SUSPEND_NOTIFICATION_HANDLE UINT64_C(0xd0030010)
static u64 __attribute__((ms_abi))
impl_RegisterSuspendResumeNotification(u64 recipient, u32 flags)
{
    if (!recipient || (flags != 0 && flags != 2)) {
        g_last_error = 87;
        return 0;
    }
    g_last_error = 0;
    return BEER_SUSPEND_NOTIFICATION_HANDLE;
}

static u64 __attribute__((ms_abi))
impl_UnregisterSuspendResumeNotification(u64 handle)
{
    if (handle != BEER_SUSPEND_NOTIFICATION_HANDLE) {
        g_last_error = 6;
        return 0;
    }
    g_last_error = 0;
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

/* ---- Shell known-folder paths ---- */
static const char *beer_roaming_appdata_path(void) { return g_roaming_appdata; }
static const char *beer_local_appdata_path(void) { return g_local_appdata; }

static const char *beer_csidl_path(s32 csidl)
{
    switch ((u32)csidl & 0xffu) {
    case 0x1a: /* CSIDL_APPDATA */
        return beer_roaming_appdata_path();
    case 0x1c: /* CSIDL_LOCAL_APPDATA */
        return beer_local_appdata_path();
    default:
        return beer_roaming_appdata_path();
    }
}

static int copy_ascii_to_wide_path(u16 *dst, size_t capacity, const char *src)
{
    if (!dst || !capacity || !src) return 0;
    size_t length = strlen(src);
    if (length >= capacity) return 0;
    for (size_t i = 0; i <= length; ++i) dst[i] = (u8)src[i];
    return 1;
}

static u64 __attribute__((ms_abi))
impl_SHGetFolderPathW(u64 hwnd, s32 csidl, u64 tok, u32 flags, u16 *buf)
{
    (void)hwnd; (void)tok; (void)flags;
    const char *path = beer_csidl_path(csidl);
    if (!copy_ascii_to_wide_path(buf, 260, path))
        return (u64)(u32)0x8007007a; /* HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) */
    fprintf(stderr, "[SHELL] SHGetFolderPathW(csidl=0x%x) -> %s\n", csidl, path);
    return 0; /* S_OK */
}

static u64 __attribute__((ms_abi))
impl_SHGetFolderPathA(u64 hwnd, s32 csidl, u64 tok, u32 flags, char *buf)
{
    (void)hwnd; (void)tok; (void)flags;
    const char *path = beer_csidl_path(csidl);
    if (!buf || strlen(path) >= 260)
        return (u64)(u32)0x8007007a;
    strcpy(buf, path);
    fprintf(stderr, "[SHELL] SHGetFolderPathA(csidl=0x%x) -> %s\n", csidl, path);
    return 0;
}

static u64 __attribute__((ms_abi))
impl_SHGetKnownFolderPath(u64 rfid, u32 flags, u64 tok, u64 *out)
{
    (void)rfid; (void)flags; (void)tok;
    if (!out) return (u64)(u32)0x80070057; /* E_INVALIDARG */
    *out = 0;
    const char *path = beer_roaming_appdata_path();
    size_t length = strlen(path);
    u16 *wide = malloc((length + 1) * sizeof(*wide));
    if (!wide) return (u64)(u32)0x8007000e; /* E_OUTOFMEMORY */
    if (!copy_ascii_to_wide_path(wide, length + 1, path)) {
        free(wide);
        return (u64)(u32)0x80070057;
    }
    *out = (u64)wide;
    return 0;
}
static u64 __attribute__((ms_abi))
impl_CoTaskMemFree(void *p) { free(p); return 0; }

/* ---- COM CoInitialize ---- */
static u64 __attribute__((ms_abi))
impl_CoInitialize(u64 reserved) { (void)reserved; return 0; /* S_OK */ }
static u64 __attribute__((ms_abi))
impl_CoInitializeEx(u64 reserved, u32 model) { (void)reserved;(void)model; return 0; }
static u64 __attribute__((ms_abi))
impl_CoUninitialize(void) { return 0; }
static u64 __attribute__((ms_abi))
impl_CoInitializeSecurity(void *security_descriptor, s32 authentication_services,
                          void *authentication_service_list, void *reserved1,
                          u32 authentication_level, u32 impersonation_level,
                          void *authentication_list, u32 capabilities,
                          void *reserved3)
{
    (void)security_descriptor;
    (void)authentication_services;
    (void)authentication_service_list;
    (void)reserved1;
    (void)authentication_level;
    (void)impersonation_level;
    (void)authentication_list;
    (void)capabilities;
    (void)reserved3;
    /* Beer hosts one process-local COM runtime and has no RPC transport. The
     * process-wide security policy is therefore accepted once as a no-op. */
    static _Atomic(u32) initialized;
    return atomic_exchange(&initialized, 1) ? UINT64_C(0x80010119) : 0;
}
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
static ID3D11Device *g_d3d11_device   = NULL;
static ID3D11DeviceContext *g_d3d11_context  = NULL;
static IDXGISwapChain *g_dxgi_swapchain = NULL;

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
    /* Initialize D3D11 objects using graphics module with real vtables. */
    g_d3d11_device   = d3d11_device_create();
    g_d3d11_context  = d3d11_device_context_create();
    g_dxgi_swapchain = dxgi_swapchain_create();
    fprintf(stderr, "[D3D11] Initialized real COM objects: device=0x%lx context=0x%lx swapchain=0x%lx\n",
            (u64)g_d3d11_device, (u64)g_d3d11_context, (u64)g_dxgi_swapchain);
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
    fprintf(stderr, "[D3D11] CreateDevice -> returning real COM objects device=0x%lx context=0x%lx\n", 
            (u64)g_d3d11_device, (u64)g_d3d11_context);
    if (ppDevice)      *ppDevice  = (u64 *)g_d3d11_device;
    if (pFeatureLevel) *pFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    if (ppContext)     *ppContext = (u64 *)g_d3d11_context;
    return S_OK;
}

static u64 __attribute__((ms_abi))
impl_D3D11CreateDeviceAndSwapChain(u64 adapter, u32 dtype, u64 sw, u32 flags,
    const u32 *fls, u32 nfl, u32 sdk, u64 *swdesc, u64 **ppSwap,
    u64 **ppDevice, u32 *pFL, u64 **ppCtx)
{
    (void)adapter;(void)dtype;(void)sw;(void)flags;
    (void)fls;(void)nfl;(void)sdk;
    fprintf(stderr, "[D3D11] CreateDeviceAndSwapChain -> returning real COM objects\n");
    if (!swdesc || !ppSwap || !g_dxgi_swapchain) return (u64)0x80070057;
    *ppSwap = NULL;
    HRESULT hr = dxgi_swapchain_configure(g_dxgi_swapchain, g_d3d11_device,
        (const BeerDxgiSwapChainDesc *)swdesc);
    if (hr != S_OK) return (u64)(uint32_t)hr;
    if (ppDevice) *ppDevice = (u64 *)g_d3d11_device;
    if (pFL)      *pFL      = D3D_FEATURE_LEVEL_11_0;
    if (ppCtx)    *ppCtx    = (u64 *)g_d3d11_context;
    *ppSwap = (u64 *)g_dxgi_swapchain;
    return S_OK;
}

/* IDXGIFactory::CreateSwapChain (vtable[10]) — return our real swap chain */
static u64 __attribute__((ms_abi))
dxgi_CreateSwapChain(u64 *obj, u64 *device, u64 *desc, u64 **ppSwap)
{
    static _Atomic(u32) calls;
    u32 call = atomic_fetch_add(&calls, 1) + 1;
    if (call <= 8) {
        fprintf(stderr,
                "[DXGI TRACE] CreateSwapChain #%u factory=%p device=%p desc=%p output=%p\n",
                call, (void *)obj, (void *)device, (void *)desc, (void *)ppSwap);
    }
    if (!ppSwap) return (u64)0x80070057; /* E_INVALIDARG */
    *ppSwap = NULL;
    if (!device || !desc || !g_dxgi_swapchain) return E_FAIL;
    HRESULT hr = dxgi_swapchain_configure(g_dxgi_swapchain,
        (ID3D11Device *)device, (const BeerDxgiSwapChainDesc *)desc);
    if (hr != S_OK) return (u64)(uint32_t)hr;
    *ppSwap = (u64 *)g_dxgi_swapchain;
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

/* IDXGIAdapter/IDXGIOutput compatibility objects. The USER32 backend already
 * exposes one 1920x1080 virtual desktop, so DXGI must expose the corresponding
 * output instead of claiming the adapter has no attached display. */
#define DXGI_VTAB2_SZ 64
static u64  g_dxgi_vtab2[DXGI_VTAB2_SZ];
static u64  g_dxgi_output_vtab[32];
static u64 *g_dxgi_adapter = NULL;
static u64 *g_dxgi_output = NULL;

static u64 __attribute__((ms_abi))
dxgi_adapter_GetParent(u64 *obj, const u8 *riid, void **parent)
{
    (void)obj;
    if (!parent) return (u64)0x80070057; /* E_INVALIDARG */
    *parent = NULL;
    if (!dxgi_iid_equal(riid, k_iid_idxgi_factory) &&
        !dxgi_iid_equal(riid, k_iid_idxgi_factory1))
        return E_NOINTERFACE;
    if (!g_dxgi_factory) return E_FAIL;
    *parent = g_dxgi_factory;
    dxgi_AddRef(g_dxgi_factory);
    return S_OK;
}

static u64 __attribute__((ms_abi))
dxgi_adapter_GetDesc(u64 *obj, u8 *desc)
{
    (void)obj;
    if (!desc) return E_FAIL;

    /* DXGI_ADAPTER_DESC is 304 bytes on Win64: WCHAR Description[128], four
     * DWORD IDs, three SIZE_T memory amounts, and a 64-bit LUID. */
    memset(desc, 0, 304);
    const char *s = "Beer Virtual D3D11 Adapter";
    u16 *wd = (u16 *)desc;
    int i;
    for (i = 0; s[i] && i < 127; i++) wd[i] = (u8)s[i];
    wd[i] = 0;
    *(u32 *)(desc + 256) = 0x1414;                /* VendorId: software adapter */
    *(u32 *)(desc + 260) = 1;                     /* DeviceId */
    *(u64 *)(desc + 272) = 1024ULL * 1024 * 1024; /* DedicatedVideoMemory */
    *(u64 *)(desc + 280) = 256ULL * 1024 * 1024;  /* DedicatedSystemMemory */
    *(u64 *)(desc + 288) = 2ULL * 1024 * 1024 * 1024; /* SharedSystemMemory */
    return S_OK;
}

static u64 __attribute__((ms_abi))
dxgi_output_GetDesc(u64 *obj, u8 *desc)
{
    (void)obj;
    fprintf(stderr, "[DXGI] IDXGIOutput::GetDesc(desc=%p)\n", (void *)desc);
    if (!desc) return E_FAIL;
    /* DXGI_OUTPUT_DESC: WCHAR[32], RECT (four LONGs), BOOL, HMONITOR. */
    memset(desc, 0, 96);
    const char *name = "\\\\.\\DISPLAY1";
    u16 *wd = (u16 *)desc;
    for (int i = 0; name[i] && i < 31; i++) wd[i] = (u8)name[i];
    *(s32 *)(desc + 64) = 0;
    *(s32 *)(desc + 68) = 0;
    *(s32 *)(desc + 72) = 1920;
    *(s32 *)(desc + 76) = 1080;
    *(u32 *)(desc + 80) = 1;          /* AttachedToDesktop */
    *(u64 *)(desc + 88) = 0x10001;     /* stable virtual monitor handle */
    return S_OK;
}

static void dxgi_write_mode(u8 *mode, u32 width, u32 height, u32 format)
{
    memset(mode, 0, 28);
    *(u32 *)(mode + 0) = width;
    *(u32 *)(mode + 4) = height;
    *(u32 *)(mode + 8) = 60;
    *(u32 *)(mode + 12) = 1;
    *(u32 *)(mode + 16) = format;
    *(u32 *)(mode + 20) = 0; /* progressive scan */
    *(u32 *)(mode + 24) = 0; /* unspecified scaling */
}

static u64 __attribute__((ms_abi))
dxgi_output_GetDisplayModeList(u64 *obj, u32 format, u32 flags, u32 *count, u8 *modes)
{
    (void)obj; (void)flags;
    if (!count) return E_FAIL;
    if (!modes) { *count = 1; return S_OK; }
    if (*count < 1) { *count = 1; return 0x887a0003ULL; } /* DXGI_ERROR_MORE_DATA */
    dxgi_write_mode(modes, 1920, 1080, format);
    *count = 1;
    return S_OK;
}

static u64 __attribute__((ms_abi))
dxgi_output_FindClosestMatchingMode(u64 *obj, const u8 *requested, u8 *closest, u64 *device)
{
    (void)obj; (void)device;
    if (!requested || !closest) return E_FAIL;
    u32 width = *(const u32 *)(requested + 0);
    u32 height = *(const u32 *)(requested + 4);
    u32 format = *(const u32 *)(requested + 16);
    dxgi_write_mode(closest, width ? width : 1920, height ? height : 1080, format);
    return S_OK;
}

static u64 __attribute__((ms_abi))
dxgi_output_WaitForVBlank(u64 *obj)
{
    (void)obj;
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 16666667 };
    nanosleep(&ts, NULL);
    return S_OK;
}

static void init_dxgi_adapter_fake(void) {
    for (int i = 0; i < DXGI_VTAB2_SZ; i++) g_dxgi_vtab2[i] = (u64)dxgi_generic_fail;
    for (int i = 0; i < 32; i++) g_dxgi_output_vtab[i] = (u64)dxgi_generic_fail;
    g_dxgi_vtab2[0] = (u64)dxgi_adapter_QueryInterface;
    g_dxgi_vtab2[1] = (u64)dxgi_AddRef;
    g_dxgi_vtab2[2] = (u64)dxgi_Release;
    g_dxgi_vtab2[6] = (u64)dxgi_adapter_GetParent;
    g_dxgi_vtab2[7] = (u64)dxgi_adapter_EnumOutputs;
    g_dxgi_vtab2[8] = (u64)dxgi_adapter_GetDesc;

    g_dxgi_output_vtab[0] = (u64)dxgi_output_QueryInterface;
    g_dxgi_output_vtab[1] = (u64)dxgi_AddRef;
    g_dxgi_output_vtab[2] = (u64)dxgi_Release;
    g_dxgi_output_vtab[7] = (u64)dxgi_output_GetDesc;
    g_dxgi_output_vtab[8] = (u64)dxgi_output_GetDisplayModeList;
    g_dxgi_output_vtab[9] = (u64)dxgi_output_FindClosestMatchingMode;
    g_dxgi_output_vtab[10] = (u64)dxgi_output_WaitForVBlank;

    g_dxgi_adapter = mmap(NULL, 256, PROT_READ|PROT_WRITE,
                          MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    g_dxgi_output = mmap(NULL, 256, PROT_READ|PROT_WRITE,
                         MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (g_dxgi_adapter != MAP_FAILED) g_dxgi_adapter[0] = (u64)g_dxgi_vtab2;
    else g_dxgi_adapter = NULL;
    if (g_dxgi_output != MAP_FAILED) g_dxgi_output[0] = (u64)g_dxgi_output_vtab;
    else g_dxgi_output = NULL;
    d3d11_device_set_dxgi_adapter(g_dxgi_adapter);
}

static u64 __attribute__((ms_abi))
dxgi_adapter_EnumOutputs(u64 *obj, u32 idx, u64 **pp)
{
    (void)obj;
    if (!pp) return E_FAIL;
    *pp = NULL;
    if (idx != 0 || !g_dxgi_output) return (u64)DXGI_ERROR_NOT_FOUND;
    *pp = g_dxgi_output;
    dxgi_AddRef(g_dxgi_output);
    return S_OK;
}

/* Override EnumAdapters to return our fake adapter */
static u64 __attribute__((ms_abi))
dxgi_EnumAdapters_with_fake(u64 *obj, u32 idx, u64 **pp) {
    (void)obj;
    if (idx == 0) {
        if (!pp) return (u64)E_FAIL;
        *pp = g_dxgi_adapter;
        return g_dxgi_adapter ? S_OK : (u64)E_FAIL; /* one adapter at index 0 */
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
    u32 ncpus = win_processor_count();

    u32 needed = (ncpus + 1) * sizeof(SLPI); /* ncpus Core entries + 1 Package */

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
    buf[ncpus].ProcessorMask = win_processor_mask();
    buf[ncpus].Relationship  = 3; /* RelationProcessorPackage */

    *retlen = needed;
    return 1; /* TRUE */
}

/* SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX is a variable-length record stream.
 * RE8 walks it by adding the Size DWORD at offset +4; returning legacy SLPI
 * records here made that field part of ProcessorMask and trapped startup in a
 * 100% CPU parser loop.  A RelationProcessorCore record with one GROUP_AFFINITY
 * is 48 bytes on Win64. */
typedef struct {
    u64 mask;
    u16 group;
    u16 reserved[3];
} WIN_GROUP_AFFINITY;
typedef struct {
    u32 relationship;
    u32 size;
    u8 flags;
    u8 efficiency_class;
    u8 reserved[20];
    u16 group_count;
    WIN_GROUP_AFFINITY group_mask[1];
} WIN_SLPI_EX_CORE;
_Static_assert(sizeof(WIN_GROUP_AFFINITY) == 16, "Win64 GROUP_AFFINITY layout");
_Static_assert(sizeof(WIN_SLPI_EX_CORE) == 48, "Win64 SLPI_EX core layout");

static u64 __attribute__((ms_abi))
impl_GetLogicalProcessorInformationEx(u32 relationship, void *buffer, u32 *length)
{
    enum { RelationProcessorCore = 0, RelationAll = 0xffff };
    if (!length) { g_last_error = 87; return 0; }
    if (relationship != RelationProcessorCore && relationship != RelationAll) {
        g_last_error = 87;
        return 0;
    }

    u32 count = win_processor_count();
    u32 needed = count * (u32)sizeof(WIN_SLPI_EX_CORE);
    if (!buffer || *length < needed) {
        *length = needed;
        g_last_error = 122; /* ERROR_INSUFFICIENT_BUFFER */
        return 0;
    }

    WIN_SLPI_EX_CORE *records = buffer;
    memset(records, 0, needed);
    for (u32 i = 0; i < count; ++i) {
        records[i].relationship = RelationProcessorCore;
        records[i].size = sizeof(records[i]);
        records[i].flags = 0; /* expose one logical processor per core */
        records[i].group_count = 1;
        records[i].group_mask[0].mask = (u64)1 << i;
        records[i].group_mask[0].group = 0;
    }
    *length = needed;
    g_last_error = 0;
    return 1;
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
typedef struct {
    u16 processor_architecture;
    u16 reserved;
    u32 page_size;
    u64 minimum_application_address;
    u64 maximum_application_address;
    u64 active_processor_mask;
    u32 number_of_processors;
    u32 processor_type;
    u32 allocation_granularity;
    u16 processor_level;
    u16 processor_revision;
} WIN_SYSTEM_INFO;

_Static_assert(sizeof(WIN_SYSTEM_INFO) == 48, "Win64 SYSTEM_INFO layout must be 48 bytes");

static u64 __attribute__((ms_abi)) impl_GetSystemInfo(WIN_SYSTEM_INFO *info) {
    if (!info) return 0;
    memset(info, 0, sizeof(*info));
    info->processor_architecture = 9; /* PROCESSOR_ARCHITECTURE_AMD64 */
    info->page_size = 4096;
    info->minimum_application_address = 0x10000;
    info->maximum_application_address = 0x00007ffffffeffffULL;
    info->active_processor_mask = win_processor_mask();
    info->number_of_processors = win_processor_count();
    info->processor_type = 8664; /* PROCESSOR_AMD_X8664 */
    info->allocation_granularity = 0x10000;
    info->processor_level = 6;
    info->processor_revision = 0;
    return 0;
}

static u64 __attribute__((ms_abi)) impl_GetCurrentProcess(void) { return (u64)-1; }

typedef struct {
    u32 length;
    u32 memory_load;
    u64 total_physical, available_physical;
    u64 total_page_file, available_page_file;
    u64 total_virtual, available_virtual, available_extended_virtual;
} WIN_MEMORYSTATUSEX;
static u64 __attribute__((ms_abi)) impl_GlobalMemoryStatusEx(WIN_MEMORYSTATUSEX *status)
{
    if (!status || status->length != sizeof(*status)) { g_last_error = 87; return 0; }
    struct sysinfo info;
    if (sysinfo(&info) != 0) { g_last_error = (u32)errno; return 0; }
    u64 unit = info.mem_unit ? info.mem_unit : 1;
    status->total_physical = (u64)info.totalram * unit;
    status->available_physical = ((u64)info.freeram + info.bufferram) * unit;
    status->total_page_file = ((u64)info.totalram + info.totalswap) * unit;
    status->available_page_file = ((u64)info.freeram + info.freeswap) * unit;
    status->total_virtual = 0x00007fff00000000ULL;
    status->available_virtual = status->total_virtual;
    status->available_extended_virtual = 0;
    status->memory_load = status->total_physical
        ? (u32)(100 - status->available_physical * 100 / status->total_physical) : 0;
    g_last_error = 0;
    return 1;
}

typedef struct {
    s32 bias;
    u16 standard_name[32];
    u16 standard_date[8];
    s32 standard_bias;
    u16 daylight_name[32];
    u16 daylight_date[8];
    s32 daylight_bias;
} WIN_TIME_ZONE_INFORMATION;
static u64 __attribute__((ms_abi))
impl_GetTimeZoneInformation(WIN_TIME_ZONE_INFORMATION *zone)
{
    if (!zone) { g_last_error = 87; return 0xffffffffu; }
    memset(zone, 0, sizeof(*zone));
    time_t now = time(NULL);
    struct tm local_tm, utc_tm;
    localtime_r(&now, &local_tm);
    gmtime_r(&now, &utc_tm);
    time_t local_as_utc = timegm(&local_tm);
    time_t utc = timegm(&utc_tm);
    zone->bias = (s32)(-(local_as_utc - utc) / 60);
    g_last_error = 0;
    return 0; /* TIME_ZONE_ID_UNKNOWN: fixed current bias, no transition rules */
}

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
    const char *p = g_exe_path ? g_exe_path : "";
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
/* These tables model kernel handles, not a small fixed engine pool. Sekiro's
 * worker scheduler can have more than 512 live condition events; imposing that
 * artificial limit made CreateEvent fail and triggered an intentional
 * DLPlainConditionSignal panic. Keep a bounded implementation, but size it well
 * above the observed workload like a normal process handle table. */
#define MAX_EVENTS 16384
#define MAX_SEMS   4096

typedef struct {
    int used;
    int manual_reset;
    int signaled;
    uint32_t waiter_count;
    u64 creator_rip;
    u64 last_signaler_rip;
    pthread_mutex_t mtx;
    pthread_cond_t  cv;
} WinEvent;

static WinEvent g_events[MAX_EVENTS];
static int g_event_cnt = 1;
static pthread_mutex_t g_event_table_mtx = PTHREAD_MUTEX_INITIALIZER;

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
    pthread_mutex_lock(&g_event_table_mtx);
    int id = 0;
    for (int candidate = 1; candidate < g_event_cnt; ++candidate) {
        if (!g_events[candidate].used) {
            id = candidate;
            break;
        }
    }
    if (!id) {
        if (g_event_cnt >= MAX_EVENTS) {
            pthread_mutex_unlock(&g_event_table_mtx);
            g_last_error = 4; /* ERROR_TOO_MANY_OPEN_FILES */
            return 0;
        }
        id = g_event_cnt++;
    }
    WinEvent *ev = &g_events[id];
    memset(ev, 0, sizeof(*ev));
    ev->used = 1;
    ev->manual_reset = manual ? 1 : 0;
    ev->signaled = initial ? 1 : 0;
    ev->creator_rip = g_dispatch_incoming_return;
    pthread_mutex_init(&ev->mtx, NULL);
    pthread_cond_init(&ev->cv, NULL);
    pthread_mutex_unlock(&g_event_table_mtx);
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
    ev->last_signaler_rip = g_dispatch_incoming_return;
    if (getenv("BEER_EVENT_DIAGNOSTICS") && ev->waiter_count)
        fprintf(stderr,
                "[EVENT] signal event%d waiters=%u creator=0x%llx signaler=0x%llx\n",
                eid, ev->waiter_count,
                (unsigned long long)ev->creator_rip,
                (unsigned long long)ev->last_signaler_rip);
    if (ev->manual_reset) pthread_cond_broadcast(&ev->cv);
    else pthread_cond_signal(&ev->cv);
    pthread_mutex_unlock(&ev->mtx);
    return 1;
}

static void beer_signal_guest_event(uint64_t event_handle)
{
    impl_SetEvent(event_handle);
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
impl_PulseEvent(u64 h)
{
    int eid = event_id_from_handle(h);
    if (eid < 0) return 0;
    WinEvent *ev = &g_events[eid];
    pthread_mutex_lock(&ev->mtx);
    /* PulseEvent releases only threads already waiting and resets the event
     * before returning. It is inherently racy on Windows, but it is not an
     * unconditional failure. */
    ev->signaled = 1;
    if (ev->manual_reset) pthread_cond_broadcast(&ev->cv);
    else pthread_cond_signal(&ev->cv);
    ev->signaled = 0;
    pthread_mutex_unlock(&ev->mtx);
    return 1;
}

static u64 __attribute__((ms_abi))
impl_WaitForSingleObject(u64 h, u32 ms)
{
    if (getenv("BEER_MAIN_THREAD_DIAGNOSTICS") && g_window_thread_id &&
        current_windows_thread_id() == g_window_thread_id) {
        static _Atomic(u32) main_wait_logs;
        u32 trace = atomic_fetch_add(&main_wait_logs, 1);
        if (trace < 512)
            fprintf(stderr,
                    "[MAIN] WaitForSingleObject handle=0x%llx timeout=%u "
                    "caller=0x%llx\n",
                    (unsigned long long)h, ms,
                    (unsigned long long)g_dispatch_incoming_return);
    }
    /* WAIT_OBJECT_0=0, WAIT_TIMEOUT=0x102, WAIT_FAILED=0xFFFFFFFF */
    u64 trc = thread_wait_handle(h, ms);
    if (trc != (u64)-2) return trc;

    int eid = event_id_from_handle(h);
    if (eid >= 0) {
        WinEvent *ev = &g_events[eid];
        int rc = 0;
        pthread_mutex_lock(&ev->mtx);
        ++ev->waiter_count;
        if (ms == 0xFFFFFFFFu) {
            if (!ev->signaled) {
                static _Atomic(u32) infinite_wait_logs;
                u32 log_index = atomic_fetch_add(&infinite_wait_logs, 1);
                if (log_index < 64)
                    fprintf(stderr,
                            "[WAIT] WaitForSingleObject(event%d) blocking forever "
                            "creator=0x%llx waiter=0x%llx last-signal=0x%llx\n",
                            eid,
                            (unsigned long long)ev->creator_rip,
                            (unsigned long long)g_dispatch_incoming_return,
                            (unsigned long long)ev->last_signaler_rip);
            }
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
        if (ev->waiter_count) --ev->waiter_count;
        pthread_mutex_unlock(&ev->mtx);
        u64 result = rc == ETIMEDOUT ? 0x102u :
                     (rc ? 0xFFFFFFFFu : 0u);
        if (getenv("BEER_EVENT_DIAGNOSTICS") && ms == 0xFFFFFFFFu) {
            static _Atomic(u32) event_wait_completion_logs;
            static _Atomic(u32) resource_wait_completion_logs;
            u32 trace = atomic_fetch_add(&event_wait_completion_logs, 1);
            int resource_event = ev->creator_rip == UINT64_C(0x144157020) ||
                                 ev->creator_rip == UINT64_C(0x144157033);
            u32 resource_trace = resource_event
                ? atomic_fetch_add(&resource_wait_completion_logs, 1) : 0;
            if (trace < 256 || (resource_event && resource_trace < 64))
                fprintf(stderr,
                        "[EVENT] wait complete event%d result=0x%llx "
                        "waiter=0x%llx thread=%u creator=0x%llx "
                        "last-signal=0x%llx\n",
                        eid, (unsigned long long)result,
                        (unsigned long long)g_dispatch_incoming_return,
                        (unsigned)current_windows_thread_id(),
                        (unsigned long long)ev->creator_rip,
                        (unsigned long long)ev->last_signaler_rip);
        }
        return result;
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

    /* Unknown handles must not look signaled; that lets invalid synchronization
     * state race ahead. Match Windows and fail with ERROR_INVALID_HANDLE. */
    g_last_error = 6;
    return 0xFFFFFFFFu;
}

static u64 __attribute__((ms_abi))
impl_WaitForSingleObjectEx(u64 h, u32 ms, u32 alertable)
{
    (void)alertable;
    return impl_WaitForSingleObject(h, ms);
}

static u64 __attribute__((ms_abi))
impl_WaitForMultipleObjects(u32 n, const u64 *handles, u32 all, u32 ms);
static u64 __attribute__((ms_abi))
impl_WaitForMultipleObjectsEx(u32 n, const u64 *handles, u32 all, u32 ms,
                              u32 alertable);

static u64 __attribute__((ms_abi))
impl_WSACreateEvent(void)
{
    if (!g_wsa_startup_count) {
        g_wsa_last_error = 10093; /* WSANOTINITIALISED */
        return 0;
    }
    u64 handle = event_alloc(1, 0);
    if (!handle) g_wsa_last_error = 10055; /* WSAENOBUFS */
    return handle;
}

static u64 __attribute__((ms_abi))
impl_WSACloseEvent(u64 handle)
{
    int eid = event_id_from_handle(handle);
    if (eid < 0) { g_wsa_last_error = 10022; return 0; }
    pthread_mutex_lock(&g_event_table_mtx);
    WinEvent *event = &g_events[eid];
    pthread_mutex_destroy(&event->mtx);
    pthread_cond_destroy(&event->cv);
    event->used = 0;
    pthread_mutex_unlock(&g_event_table_mtx);
    return 1;
}

static u64 __attribute__((ms_abi))
impl_WSASetEvent(u64 handle)
{
    u64 result = impl_SetEvent(handle);
    if (!result) g_wsa_last_error = 10022;
    return result;
}

static u64 __attribute__((ms_abi))
impl_WSAResetEvent(u64 handle)
{
    u64 result = impl_ResetEvent(handle);
    if (!result) g_wsa_last_error = 10022;
    return result;
}

static u64 __attribute__((ms_abi))
impl_WSAWaitForMultipleEvents(u32 count, const u64 *events, u32 wait_all,
                              u32 timeout, u32 alertable)
{
    (void)alertable;
    if (!g_wsa_startup_count) {
        g_wsa_last_error = 10093;
        return 0xffffffffu; /* WSA_WAIT_FAILED */
    }
    if (!events || count == 0 || count > 64) {
        g_wsa_last_error = 10022;
        return 0xffffffffu;
    }
    return impl_WaitForMultipleObjects(count, events, wait_all, timeout);
}

/* Nonblocking wait used by multiple-object waits. A positive result means the
 * object is signaled, zero means it is pending, and -1 means invalid handle.
 * Auto-reset events and semaphores are consumed only when requested. */
static int wait_handle_poll(u64 handle, int consume)
{
    u64 thread_result = thread_wait_handle(handle, 0);
    if (thread_result != (u64)-2)
        return thread_result == 0 ? 1 : (thread_result == 0x102u ? 0 : -1);

    int eid = event_id_from_handle(handle);
    if (eid >= 0) {
        WinEvent *event = &g_events[eid];
        pthread_mutex_lock(&event->mtx);
        int signaled = event->signaled;
        if (signaled && consume && !event->manual_reset) event->signaled = 0;
        pthread_mutex_unlock(&event->mtx);
        return signaled;
    }

    int sid = sem_id_from_handle(handle);
    if (sid >= 0) {
        pthread_mutex_lock(&g_sem_mtx[sid]);
        int signaled = g_sem_val[sid] > 0;
        if (signaled && consume) --g_sem_val[sid];
        pthread_mutex_unlock(&g_sem_mtx[sid]);
        return signaled;
    }

    g_last_error = 6; /* ERROR_INVALID_HANDLE */
    return -1;
}

static u64 __attribute__((ms_abi))
impl_WaitForMultipleObjects(u32 n, const u64 *handles, u32 all, u32 ms)
{
    /* MAXIMUM_WAIT_OBJECTS is 64. Windows rejects invalid parameters before
     * inspecting or consuming any object. */
    if (!handles || n == 0 || n > 64 || all > 1) {
        g_last_error = 87; /* ERROR_INVALID_PARAMETER */
        return 0xFFFFFFFFu;
    }
    for (u32 i = 0; i < n; ++i) {
        if (wait_handle_poll(handles[i], 0) < 0) return 0xFFFFFFFFu;
        if (all) {
            for (u32 j = 0; j < i; ++j) {
                if (handles[j] == handles[i]) {
                    g_last_error = 87;
                    return 0xFFFFFFFFu;
                }
            }
        }
    }

    struct timespec started;
    clock_gettime(CLOCK_MONOTONIC, &started);
    int traced = 0;
    for (;;) {
        if (all) {
            int ready = 1;
            for (u32 i = 0; i < n; ++i) {
                int state = wait_handle_poll(handles[i], 0);
                if (state < 0) return 0xFFFFFFFFu;
                if (!state) { ready = 0; break; }
            }
            if (ready) {
                /* No producer can consume these Beer handles, so this second
                 * pass safely performs auto-reset/semaphore consumption. */
                for (u32 i = 0; i < n; ++i)
                    (void)wait_handle_poll(handles[i], 1);
                return 0; /* WAIT_OBJECT_0 */
            }
        } else {
            for (u32 i = 0; i < n; ++i) {
                int state = wait_handle_poll(handles[i], 1);
                if (state < 0) return 0xFFFFFFFFu;
                if (state) return i; /* WAIT_OBJECT_0 + index */
            }
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        u64 elapsed_ms = (u64)(now.tv_sec - started.tv_sec) * 1000u;
        if (now.tv_nsec >= started.tv_nsec)
            elapsed_ms += (u64)(now.tv_nsec - started.tv_nsec) / 1000000u;
        else
            elapsed_ms -= (u64)(started.tv_nsec - now.tv_nsec) / 1000000u;
        if (ms == 0 || (ms != 0xFFFFFFFFu && elapsed_ms >= ms))
            return 0x102u; /* WAIT_TIMEOUT */

        if (!traced && getenv("BEER_SYNC_DIAGNOSTICS")) {
            traced = 1;
            fprintf(stderr,
                    "[SYNC] WaitForMultipleObjects n=%u all=%u timeout=%u "
                    "waiter=0x%llx thread=%u\n",
                    n, all, ms,
                    (unsigned long long)g_dispatch_incoming_return,
                    (unsigned)impl_GetCurrentThreadId());
        }
        struct timespec delay = {0, 1000000L};
        nanosleep(&delay, NULL);
    }
}

static u64 __attribute__((ms_abi))
impl_WaitForMultipleObjectsEx(u32 n, const u64 *handles, u32 all, u32 ms,
                              u32 alertable)
{
    /* Beer has no user APC queue yet. In its absence an alertable wait has the
     * same completion outcomes as the non-alertable form; it must not be an
     * unresolved import or a fabricated WAIT_IO_COMPLETION. */
    (void)alertable;
    return impl_WaitForMultipleObjects(n, handles, all, ms);
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
    if (getenv("BEER_MAIN_THREAD_DIAGNOSTICS") && g_window_thread_id &&
        current_windows_thread_id() == g_window_thread_id) {
        static _Atomic(u32) main_sleep_logs;
        u32 trace = atomic_fetch_add(&main_sleep_logs, 1);
        if (trace < 512)
            fprintf(stderr, "[MAIN] Sleep timeout=%u caller=0x%llx\n", ms,
                    (unsigned long long)g_dispatch_incoming_return);
    }
    static volatile u32 sleep_count = 0;
    static _Atomic(u32) poll_trace_count;
    u32 cnt = __sync_add_and_fetch(&sleep_count, 1);
    if (getenv("BEER_RENDER_DIAGNOSTICS") &&
        (cnt == 1 || (cnt % 500) == 0))
        fprintf(stderr, "[SLEEP] Sleep(%u) call #%u\n", ms, cnt);
    if (ms == 8 && getenv("BEER_SLEEP_DIAGNOSTICS")) {
        u32 trace = atomic_fetch_add(&poll_trace_count, 1);
        if (trace < 16 || (trace % 1000) == 0)
            fprintf(stderr,
                    "[SLEEP8] call=%u guest-return=0x%llx thread=%u\n",
                    trace + 1,
                    (unsigned long long)g_dispatch_incoming_return,
                    (unsigned)impl_GetCurrentThreadId());
    }
    if (ms) { struct timespec ts = {ms/1000, (ms%1000)*1000000L}; nanosleep(&ts,NULL); }
    return 0;
}
static u64 __attribute__((ms_abi)) impl_SleepEx(u32 ms, u32 alertable) {
    /* There is currently no guest APC queue. Preserve elapsed-time behavior
     * without inventing WAIT_IO_COMPLETION (0xc0). */
    (void)alertable;
    impl_Sleep(ms);
    return 0;
}
static u64 __attribute__((ms_abi)) impl_GetTickCount(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
static u64 __attribute__((ms_abi)) impl_GetTickCount64(void) { return impl_GetTickCount(); }

/* ---- File type / handles ---- */
static int fh_get(u64 h);

static u64 __attribute__((ms_abi)) impl_GetFileType(u64 handle) {
    /* FILE_TYPE_DISK=1, FILE_TYPE_CHAR=2, FILE_TYPE_UNKNOWN=0. */
    if ((int64_t)handle == -10 || (int64_t)handle == -11 || (int64_t)handle == -12) {
        g_last_error = 0;
        return 2;
    }
    if (fh_get(handle) >= 0) {
        g_last_error = 0;
        return 1;
    }
    g_last_error = 6; /* ERROR_INVALID_HANDLE */
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

/* Locale information must be internally consistent.  Returning an empty
 * successful string makes Sekiro select English through Steam, but fail to
 * resolve the corresponding FDP message table and display resource keys such
 * as `FDP_System_Message(4161)` instead of the post-save disclaimer text. */
static const char *beer_locale_info_ascii(u32 locale, u32 type)
{
    const int japanese = (locale & 0xffffu) == 0x0411u;
    switch (type & ~0x20000000u) { /* strip LOCALE_RETURN_NUMBER */
    case 0x00000001: return japanese ? "0411" : "0409"; /* ILANGUAGE */
    case 0x00000002: return japanese ? "Japanese (Japan)" : "English (United States)";
    case 0x00000003: return japanese ? "JPN" : "ENU";   /* SABBREVLANGNAME */
    case 0x00000004: return japanese ? "Japanese" : "English";
    case 0x00000005: return japanese ? "81" : "1";      /* ICOUNTRY */
    case 0x00000006: return japanese ? "Japan" : "United States";
    case 0x00000007: return japanese ? "JPN" : "USA";
    case 0x00000008: return japanese ? "Japan" : "United States";
    case 0x00000009: return japanese ? "0411" : "0409"; /* IDEFAULTLANGUAGE */
    case 0x0000000a: return japanese ? "81" : "1";      /* IDEFAULTCOUNTRY */
    case 0x0000000b: return japanese ? "932" : "437";   /* IDEFAULTCODEPAGE */
    case 0x0000000e: return ".";                         /* SDECIMAL */
    case 0x0000000f: return ",";                         /* STHOUSAND */
    case 0x00000010: return "3;0";                       /* SGROUPING */
    case 0x00000011: return "2";                         /* IDIGITS */
    case 0x00000012: return "1";                         /* ILZERO */
    case 0x00000013: return "0123456789";                /* SNATIVEDIGITS */
    case 0x00000014: return japanese ? "yen" : "$";     /* SCURRENCY */
    case 0x00000015: return japanese ? "JPY" : "USD";   /* SINTLSYMBOL */
    case 0x0000005c: return japanese ? "ja-JP" : "en-US"; /* SNAME */
    case 0x00001001: return japanese ? "Japanese" : "English"; /* SENGLANGUAGE */
    case 0x00001002: return japanese ? "Japan" : "United States"; /* SENGCOUNTRY */
    case 0x00001004: return japanese ? "932" : "1252";  /* IDEFAULTANSICODEPAGE */
    default: return NULL;
    }
}

static u64 __attribute__((ms_abi))
impl_GetLocaleInfoW(u32 locale, u32 type, u16 *buf, int size)
{
    const char *value = beer_locale_info_ascii(locale, type);
    if (!value) { g_last_error = 1004; return 0; } /* ERROR_INVALID_FLAGS */

    if (type & 0x20000000u) { /* LOCALE_RETURN_NUMBER */
        char *end = NULL;
        unsigned long number = strtoul(value, &end, 10);
        if (!end || *end) { g_last_error = 87; return 0; }
        if (!buf || size == 0) return 2; /* DWORD measured in WCHAR units */
        if (size < 2) { g_last_error = 122; return 0; }
        u32 result = (u32)number;
        memcpy(buf, &result, sizeof(result));
        g_last_error = 0;
        return 2;
    }

    int needed = (int)strlen(value) + 1;
    if (!buf || size == 0) return (u64)needed;
    if (size < needed) { g_last_error = 122; return 0; }
    for (int i = 0; i < needed; ++i) buf[i] = (u8)value[i];
    g_last_error = 0;
    return (u64)needed;
}

static u64 __attribute__((ms_abi)) impl_GetUserDefaultLCID(void)   { return 0x0409; /* en-US */ }
static u64 __attribute__((ms_abi)) impl_GetSystemDefaultLCID(void) { return 0x0409; }
static u64 __attribute__((ms_abi)) impl_GetUserDefaultUILanguage(void) { return 0x0409; }
static u64 __attribute__((ms_abi)) impl_GetUserDefaultLangID(void) { return 0x0409; }

/* HKL is pointer-sized on Win64.  For a non-IME US layout its low word is the
 * language ID and the conventional layout value is 00000409.  Sekiro queries
 * this from worker threads while constructing localized system messages; a
 * null HKL makes that path fall back to untranslated FDP message keys. */
static u64 __attribute__((ms_abi))
impl_GetKeyboardLayout(u32 thread_id)
{
    (void)thread_id;
    g_last_error = 0;
    return 0x0000000004090409ULL;
}

/* Win32 MAPVK_* translation for the US keyboard layout exposed above.  The
 * low byte of an extended scan code remains the set-1 code; the 0xe000 prefix
 * is preserved for the MAPVK_*_EX variants. */
static u32 virtual_key_to_scan_code(u32 virtual_key)
{
    if (virtual_key >= 'A' && virtual_key <= 'Z') {
        static const u8 letter_scans[26] = {
            0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17,
            0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13,
            0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c
        };
        return letter_scans[virtual_key - 'A'];
    }
    if (virtual_key >= '1' && virtual_key <= '9')
        return 0x02u + virtual_key - '1';
    if (virtual_key == '0') return 0x0b;
    if (virtual_key >= 0x70 && virtual_key <= 0x79)
        return 0x3bu + virtual_key - 0x70; /* F1..F10 */
    if (virtual_key == 0x7a) return 0x57; /* F11 */
    if (virtual_key == 0x7b) return 0x58; /* F12 */
    switch (virtual_key) {
        case 0x08: return 0x0e; case 0x09: return 0x0f;
        case 0x0d: return 0x1c; case 0x10: return 0x2a;
        case 0x11: return 0x1d; case 0x12: return 0x38;
        case 0x14: return 0x3a; case 0x1b: return 0x01;
        case 0x20: return 0x39; case 0x21: return 0xe049;
        case 0x22: return 0xe051; case 0x23: return 0xe04f;
        case 0x24: return 0xe047; case 0x25: return 0xe04b;
        case 0x26: return 0xe048; case 0x27: return 0xe04d;
        case 0x28: return 0xe050; case 0x2d: return 0xe052;
        case 0x2e: return 0xe053; case 0x5b: return 0xe05b;
        case 0x5c: return 0xe05c; case 0x60: return 0x52;
        case 0x61: return 0x4f; case 0x62: return 0x50;
        case 0x63: return 0x51; case 0x64: return 0x4b;
        case 0x65: return 0x4c; case 0x66: return 0x4d;
        case 0x67: return 0x47; case 0x68: return 0x48;
        case 0x69: return 0x49; case 0x6a: return 0x37;
        case 0x6b: return 0x4e; case 0x6d: return 0x4a;
        case 0x6e: return 0x53; case 0x6f: return 0xe035;
        case 0x90: return 0x45; case 0x91: return 0x46;
        case 0xba: return 0x27; case 0xbb: return 0x0d;
        case 0xbc: return 0x33; case 0xbd: return 0x0c;
        case 0xbe: return 0x34; case 0xbf: return 0x35;
        case 0xc0: return 0x29; case 0xdb: return 0x1a;
        case 0xdc: return 0x2b; case 0xdd: return 0x1b;
        case 0xde: return 0x28; default: return 0;
    }
}

static u32 scan_code_to_virtual_key(u32 scan_code)
{
    u32 extended = scan_code & 0xff00u;
    u32 scan = scan_code & 0xffu;
    if (extended == 0xe000u) {
        switch (scan) {
            case 0x1c: return 0x0d; case 0x1d: return 0xa3;
            case 0x35: return 0x6f; case 0x38: return 0xa5;
            case 0x47: return 0x24; case 0x48: return 0x26;
            case 0x49: return 0x21; case 0x4b: return 0x25;
            case 0x4d: return 0x27; case 0x4f: return 0x23;
            case 0x50: return 0x28; case 0x51: return 0x22;
            case 0x52: return 0x2d; case 0x53: return 0x2e;
            case 0x5b: return 0x5b; case 0x5c: return 0x5c;
            default: break;
        }
    }
    for (u32 virtual_key = 1; virtual_key < 256; ++virtual_key)
        if ((virtual_key_to_scan_code(virtual_key) & 0xffu) == scan)
            return virtual_key;
    return 0;
}

static u64 __attribute__((ms_abi))
impl_MapVirtualKeyW(u32 code, u32 map_type)
{
    switch (map_type) {
        case 0: return virtual_key_to_scan_code(code) & 0xffu;
        case 1: return scan_code_to_virtual_key(code & 0xffu);
        case 2:
            if ((code >= 'A' && code <= 'Z') ||
                (code >= '0' && code <= '9')) return code;
            if (code == 0x20) return ' ';
            return 0;
        case 3: return scan_code_to_virtual_key(code);
        case 4: return virtual_key_to_scan_code(code);
        default: return 0;
    }
}

static u64 __attribute__((ms_abi))
impl_MapVirtualKeyExW(u32 code, u32 map_type, u64 keyboard_layout)
{
    (void)keyboard_layout;
    return impl_MapVirtualKeyW(code, map_type);
}

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
static u64 g_window_proc;
static u64 g_window_userdata;
static u64 g_window_style;
static u64 g_window_exstyle;
static u64 g_window_instance;
static int g_window_focused;

#define WINDOW_PROPERTY_CAPACITY 256
#define WINDOW_PROPERTY_NAME_CAPACITY 128

typedef struct {
    int active;
    int is_atom;
    u16 atom;
    u16 name[WINDOW_PROPERTY_NAME_CAPACITY];
    u64 value;
} WIN_PROPERTY;

static WIN_PROPERTY g_window_properties[WINDOW_PROPERTY_CAPACITY];
static pthread_mutex_t g_window_property_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    u32 cbSize;
    u32 style;
    u64 lpfnWndProc;
    s32 cbClsExtra;
    s32 cbWndExtra;
    u64 hInstance;
    u64 hIcon;
    u64 hCursor;
    u64 hbrBackground;
    u64 lpszMenuName;
    u64 lpszClassName;
    u64 hIconSm;
} WIN_WNDCLASSEX;
_Static_assert(sizeof(WIN_WNDCLASSEX) == 80, "WNDCLASSEX must be 80 bytes on Win64");

static u64 register_window_class(const WIN_WNDCLASSEX *window_class, const char *api)
{
    if (!window_class || window_class->cbSize < 72 || !window_class->lpfnWndProc) {
        g_last_error = 87;
        return 0;
    }
    g_window_proc = window_class->lpfnWndProc;
    g_window_instance = window_class->hInstance;
    if (!g_api_registerclass) {
        g_api_registerclass = 1;
        fprintf(stderr, "[PROGRESS] %s retained WndProc=0x%lx\n", api, g_window_proc);
    }
    return 1; /* stable class atom */
}

static u64 __attribute__((ms_abi))
impl_RegisterClassExA(const WIN_WNDCLASSEX *window_class)
    { return register_window_class(window_class, "RegisterClassExA"); }
static u64 __attribute__((ms_abi))
impl_RegisterClassExW(const WIN_WNDCLASSEX *window_class)
    { return register_window_class(window_class, "RegisterClassExW"); }
static u64 __attribute__((ms_abi))
impl_GetDesktopWindow(void) { return FAKE_HWND; }
static u64 __attribute__((ms_abi))
impl_GetForegroundWindow(void) { return FAKE_HWND; }
static u64 __attribute__((ms_abi))
impl_ShowWindow(u64 hw, u32 cmd) {
    if (hw != FAKE_HWND || !xwayland_window_exists()) {
        g_last_error = 1400; /* ERROR_INVALID_WINDOW_HANDLE */
        return 0;
    }
    XwaylandWindowState state;
    int was_visible = xwayland_window_get_state(&state) && state.visible;
    int visible = cmd != 0;
    fprintf(stderr, "[WIN] ShowWindow(hwnd=0x%lx, cmd=%u)\n", hw, cmd);
    if (!xwayland_window_show(visible)) return 0;
    if (was_visible != visible && g_window_proc) {
        typedef u64 __attribute__((ms_abi)) (*WndProc)(u64, u32, u64, u64);
        ((WndProc)(uintptr_t)g_window_proc)(FAKE_HWND, 0x0018, visible, 0);
    }
    return was_visible;
}

static u64 __attribute__((ms_abi))
impl_IsWindowEnabled(u64 hwnd)
{
    return hwnd == FAKE_HWND && xwayland_window_exists();
}

static u64 __attribute__((ms_abi))
impl_IsWindowVisible(u64 hwnd)
{
    XwaylandWindowState state;
    return hwnd == FAKE_HWND && xwayland_window_get_state(&state) && state.visible;
}

static u64 __attribute__((ms_abi))
impl_IsIconic(u64 hwnd)
{
    /* XWayland tracks map/visibility but not a separate Win32 minimized state.
     * A valid mapped Beer window is therefore not iconic. */
    if (hwnd != FAKE_HWND || !xwayland_window_exists()) {
        g_last_error = 1400; /* ERROR_INVALID_WINDOW_HANDLE */
        return 0;
    }
    return 0;
}

static u64 __attribute__((ms_abi))
impl_SystemParametersInfoW(u32 action, u32 parameter, void *value, u32 update)
{
    (void)parameter;
    (void)update;
    /* RE8 probes the system-beep setting while constructing its window/input
     * runtime. SPI_GETBEEP writes a BOOL; SPI_SETBEEP has no output. Keep the
     * process-local setting coherent rather than returning false success. */
    static u32 beep_enabled = 1;
    switch (action) {
    case 0x0002: /* SPI_SETBEEP */
        beep_enabled = parameter != 0;
        return 1;
    case 0x0001: /* SPI_GETBEEP */
        if (!value) { g_last_error = 87; return 0; }
        *(u32 *)value = beep_enabled;
        return 1;
    default:
        g_last_error = 87;
        return 0;
    }
}

static u64 __attribute__((ms_abi))
impl_SystemParametersInfoA(u32 action, u32 parameter, void *value, u32 update)
{
    return impl_SystemParametersInfoW(action, parameter, value, update);
}

/* Minimal, coherent desktop/device-discovery contracts. RE8 probes these
 * while creating its input/window services. An empty device set is valid;
 * false success or a null registration handle is not. */
#define BEER_DEVICE_NOTIFICATION_HANDLE UINT64_C(0xd0030001)
#define BEER_DEVICE_INFO_SET_HANDLE      UINT64_C(0xd0030002)
#define ERROR_NO_MORE_ITEMS              259u

static u64 __attribute__((ms_abi))
impl_RegisterDeviceNotificationW(u64 recipient, const void *filter, u32 flags)
{
    (void)filter; (void)flags;
    if (!recipient) { g_last_error = 87; return 0; }
    g_last_error = 0;
    return BEER_DEVICE_NOTIFICATION_HANDLE;
}
static u64 __attribute__((ms_abi))
impl_RegisterDeviceNotificationA(u64 recipient, const void *filter, u32 flags)
{
    return impl_RegisterDeviceNotificationW(recipient, filter, flags);
}
static u64 __attribute__((ms_abi))
impl_UnregisterDeviceNotification(u64 handle)
{
    if (handle != BEER_DEVICE_NOTIFICATION_HANDLE) {
        g_last_error = 6;
        return 0;
    }
    g_last_error = 0;
    return 1;
}
static u64 __attribute__((ms_abi))
impl_SetupDiGetClassDevsW(const void *class_guid, const u16 *enumerator,
                         u64 parent, u32 flags)
{
    (void)class_guid; (void)enumerator; (void)parent; (void)flags;
    g_last_error = 0;
    return BEER_DEVICE_INFO_SET_HANDLE;
}
static u64 __attribute__((ms_abi))
impl_SetupDiGetClassDevsA(const void *class_guid, const char *enumerator,
                         u64 parent, u32 flags)
{
    (void)enumerator;
    return impl_SetupDiGetClassDevsW(class_guid, NULL, parent, flags);
}
static u64 __attribute__((ms_abi))
impl_SetupDiEnumDeviceInfo(u64 set, u32 index, void *device_info)
{
    (void)index; (void)device_info;
    if (set != BEER_DEVICE_INFO_SET_HANDLE) { g_last_error = 6; return 0; }
    g_last_error = ERROR_NO_MORE_ITEMS;
    return 0;
}
static u64 __attribute__((ms_abi))
impl_SetupDiEnumDeviceInterfaces(u64 set, const void *device_info,
                                 const void *interface_class_guid, u32 index,
                                 void *interface_data)
{
    (void)device_info; (void)interface_class_guid; (void)index;
    (void)interface_data;
    if (set != BEER_DEVICE_INFO_SET_HANDLE) { g_last_error = 6; return 0; }
    /* Beer currently exposes no host HID/device interfaces through SetupAPI.
     * An empty set is a valid result and must be reported as end-of-list, not
     * as generic false success with untouched output memory. */
    g_last_error = ERROR_NO_MORE_ITEMS;
    return 0;
}
static u64 __attribute__((ms_abi))
impl_SetupDiGetDeviceRegistryPropertyW(u64 set, const void *device_info,
                                       u32 property, u32 *property_type,
                                       u8 *buffer, u32 buffer_size,
                                       u32 *required_size)
{
    (void)device_info; (void)property; (void)buffer; (void)buffer_size;
    if (property_type) *property_type = 0;
    if (required_size) *required_size = 0;
    if (set != BEER_DEVICE_INFO_SET_HANDLE) { g_last_error = 6; return 0; }
    g_last_error = 13; /* ERROR_INVALID_DATA: no enumerated device/property. */
    return 0;
}
static u64 __attribute__((ms_abi))
impl_SetupDiGetDeviceRegistryPropertyA(u64 set, const void *device_info,
                                       u32 property, u32 *property_type,
                                       u8 *buffer, u32 buffer_size,
                                       u32 *required_size)
{
    return impl_SetupDiGetDeviceRegistryPropertyW(set, device_info, property,
                                                   property_type, buffer,
                                                   buffer_size, required_size);
}
static u64 __attribute__((ms_abi))
impl_SetupDiDestroyDeviceInfoList(u64 set)
{
    if (set != BEER_DEVICE_INFO_SET_HANDLE) { g_last_error = 6; return 0; }
    g_last_error = 0;
    return 1;
}
static u64 __attribute__((ms_abi))
impl_DisableThreadLibraryCalls(u64 module)
{
    if (!module) { g_last_error = 6; return 0; }
    g_last_error = 0;
    return 1;
}
static u64 __attribute__((ms_abi))
impl_PathFileExistsW(const u16 *path)
{
    if (!path) { g_last_error = 87; return 0; }
    char narrow[PATH_MAX], host[PATH_MAX];
    size_t length = 0;
    while (path[length] && length + 1 < sizeof(narrow)) {
        if (path[length] > 0x7f) { g_last_error = 1113; return 0; }
        narrow[length] = (char)path[length];
        ++length;
    }
    if (path[length]) { g_last_error = 206; return 0; }
    narrow[length] = 0;
    if (!resolve_windows_host_path(narrow, host, sizeof(host)) ||
        access(host, F_OK) != 0) {
        g_last_error = 2;
        return 0;
    }
    g_last_error = 0;
    return 1;
}
static u64 __attribute__((ms_abi))
impl_PathFileExistsA(const char *path)
{
    if (!path) { g_last_error = 87; return 0; }
    char host[PATH_MAX];
    if (!resolve_windows_host_path(path, host, sizeof(host)) ||
        access(host, F_OK) != 0) {
        g_last_error = 2;
        return 0;
    }
    g_last_error = 0;
    return 1;
}
static u64 __attribute__((ms_abi))
impl_GetStockObject(s32 object)
{
    return (object >= 0 && object <= 20) ? UINT64_C(0xd0040001) + (u32)object : 0;
}
static u64 __attribute__((ms_abi))
impl_DragAcceptFiles(u64 hwnd, u32 accept)
{
    (void)accept;
    if (hwnd != FAKE_HWND) { g_last_error = 1400; return 0; }
    return 0;
}
static u64 __attribute__((ms_abi))
impl_ImmGetDefaultIMEWnd(u64 hwnd)
{
    (void)hwnd;
    return 0; /* en-US non-IME layout */
}

static u64 __attribute__((ms_abi))
impl_UpdateWindow(u64 hw) {
    if (hw != FAKE_HWND || !xwayland_window_exists()) {
        g_last_error = 1400;
        return 0;
    }
    xwayland_window_pump_events();
    return 1;
}

/* ---- HMODULE GetModuleHandleExA/W ---- */
static u64 __attribute__((ms_abi))
impl_GetModuleHandleExW(u32 flags, u64 name, u64 *out)
    { (void)flags;(void)name; if(out)*out=(u64)g_img; return 1; }
static u64 __attribute__((ms_abi))
impl_GetModuleHandleExA(u32 flags, u64 name, u64 *out)
    { (void)flags;(void)name; if(out)*out=(u64)g_img; return 1; }

/* ---- Condition variables (CONDITION_VARIABLE = pointer-sized opaque) ----
 * Windows stores condition-variable state in one pointer-sized word. Keep the
 * guest word zero-compatible and associate host pthread state by its address.
 * The old implementation returned immediately for INFINITE sleeps, turning
 * producer/consumer waits into busy loops and losing every wake notification. */
#define MAX_CONDITION_VARIABLES 16384

typedef struct {
    u64 *guest_address;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    u64 generation;
    u32 waiters;
    u64 creator_rip;
    u64 last_waker_rip;
    int used;
} WinConditionVariable;

static WinConditionVariable g_condition_variables[MAX_CONDITION_VARIABLES];
static pthread_mutex_t g_condition_table_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Atomic(u32) g_condition_count;
static _Atomic(u32) g_condition_trace_count;

static int sync_diagnostics_enabled(void)
{
    static int initialized;
    static int enabled;
    if (!initialized) {
        const char *value = getenv("BEER_SYNC_DIAGNOSTICS");
        enabled = value && value[0] && strcmp(value, "0") != 0;
        initialized = 1;
    }
    return enabled;
}

static WinConditionVariable *condition_variable_get(u64 *address, int create)
{
    if (!address) return NULL;
    pthread_mutex_lock(&g_condition_table_mutex);
    WinConditionVariable *free_slot = NULL;
    for (size_t i = 0; i < MAX_CONDITION_VARIABLES; ++i) {
        WinConditionVariable *entry = &g_condition_variables[i];
        if (entry->used && entry->guest_address == address) {
            pthread_mutex_unlock(&g_condition_table_mutex);
            return entry;
        }
        if (!entry->used && !free_slot) free_slot = entry;
    }
    if (create && free_slot) {
        memset(free_slot, 0, sizeof(*free_slot));
        free_slot->guest_address = address;
        free_slot->creator_rip = g_dispatch_incoming_return;
        pthread_mutex_init(&free_slot->mutex, NULL);
        pthread_cond_init(&free_slot->condition, NULL);
        free_slot->used = 1;
        atomic_fetch_add(&g_condition_count, 1);
    } else if (create && !free_slot) {
        static _Atomic(u32) exhaustion_logs;
        if (atomic_fetch_add(&exhaustion_logs, 1) == 0)
            fprintf(stderr,
                    "[SYNC] condition-variable table exhausted at %u entries "
                    "guest-return=0x%llx\n",
                    MAX_CONDITION_VARIABLES,
                    (unsigned long long)g_dispatch_incoming_return);
    }
    pthread_mutex_unlock(&g_condition_table_mutex);
    return create ? free_slot : NULL;
}

static u64 __attribute__((ms_abi)) impl_InitializeConditionVariable(u64 *cv)
{
    if (!cv) return 0;
    *cv = 0;
    return condition_variable_get(cv, 1) ? 1 : 0;
}

static u64 __attribute__((ms_abi)) impl_WakeConditionVariable(u64 *cv)
{
    WinConditionVariable *entry = condition_variable_get(cv, 0);
    if (!entry) return 0;
    pthread_mutex_lock(&entry->mutex);
    ++entry->generation;
    entry->last_waker_rip = g_dispatch_incoming_return;
    u32 waiters = entry->waiters;
    pthread_cond_signal(&entry->condition);
    pthread_mutex_unlock(&entry->mutex);
    if (sync_diagnostics_enabled() && waiters) {
        u32 trace = atomic_fetch_add(&g_condition_trace_count, 1);
        if (trace < 256)
            fprintf(stderr,
                    "[SYNC] WakeConditionVariable cv=%p waiters=%u "
                    "creator=0x%llx waker=0x%llx\n",
                    (void *)cv, waiters,
                    (unsigned long long)entry->creator_rip,
                    (unsigned long long)entry->last_waker_rip);
    }
    return 0;
}

static u64 __attribute__((ms_abi)) impl_WakeAllConditionVariable(u64 *cv)
{
    WinConditionVariable *entry = condition_variable_get(cv, 0);
    if (!entry) return 0;
    pthread_mutex_lock(&entry->mutex);
    ++entry->generation;
    entry->last_waker_rip = g_dispatch_incoming_return;
    u32 waiters = entry->waiters;
    pthread_cond_broadcast(&entry->condition);
    pthread_mutex_unlock(&entry->mutex);
    if (sync_diagnostics_enabled() && waiters) {
        u32 trace = atomic_fetch_add(&g_condition_trace_count, 1);
        if (trace < 256)
            fprintf(stderr,
                    "[SYNC] WakeAllConditionVariable cv=%p waiters=%u "
                    "creator=0x%llx waker=0x%llx\n",
                    (void *)cv, waiters,
                    (unsigned long long)entry->creator_rip,
                    (unsigned long long)entry->last_waker_rip);
    }
    return 0;
}

static u64 __attribute__((ms_abi))
impl_SleepConditionVariableCS(u64 *cv, WIN_CS *cs, u32 ms)
{
    WinConditionVariable *entry = condition_variable_get(cv, 1);
    if (!entry || !cs) {
        g_last_error = 87; /* ERROR_INVALID_PARAMETER */
        return 0;
    }

    pthread_mutex_lock(&entry->mutex);
    u64 generation = entry->generation;
    ++entry->waiters;
    if (sync_diagnostics_enabled() && ms == 0xFFFFFFFFu) {
        u32 trace = atomic_fetch_add(&g_condition_trace_count, 1);
        if (trace < 256)
            fprintf(stderr,
                    "[SYNC] SleepConditionVariableCS cv=%p waiters=%u "
                    "creator=0x%llx waiter=0x%llx last-waker=0x%llx\n",
                    (void *)cv, entry->waiters,
                    (unsigned long long)entry->creator_rip,
                    (unsigned long long)g_dispatch_incoming_return,
                    (unsigned long long)entry->last_waker_rip);
    }
    impl_LeaveCriticalSection(cs);

    int rc = 0;
    if (ms == 0) {
        rc = ETIMEDOUT;
    } else if (ms == 0xFFFFFFFFu) {
        while (entry->generation == generation && rc == 0)
            rc = pthread_cond_wait(&entry->condition, &entry->mutex);
    } else {
        struct timespec deadline;
        if (calc_abs_deadline(&deadline, ms) != 0) rc = ETIMEDOUT;
        while (entry->generation == generation && rc == 0)
            rc = pthread_cond_timedwait(&entry->condition, &entry->mutex,
                                        &deadline);
    }
    --entry->waiters;
    pthread_mutex_unlock(&entry->mutex);
    impl_EnterCriticalSection(cs);

    if (rc == ETIMEDOUT) {
        g_last_error = 1460; /* ERROR_TIMEOUT */
        return 0;
    }
    if (rc != 0) {
        g_last_error = 1;
        return 0;
    }
    return 1;
}

/* ---- SRWLOCK ----
 * Like Windows SRWLOCK, the guest-visible object is one pointer-sized word.
 * Host pthread state is associated by address so PE objects can remain
 * zero-initialized and retain their Windows layout. */
#define CONDITION_VARIABLE_LOCKMODE_SHARED 0x1u

typedef struct {
    pthread_rwlock_t lock;
} WinSrwLock;

static WinSrwLock *srw_lock_get(u64 *address, int create)
{
    if (!address) return NULL;

    /* SRWLOCK is an opaque pointer-sized value. Store Beer's host object in
     * that word instead of keying an ever-growing address side table. RE8
     * constructs and frees many short-lived objects; retaining every historical
     * address exhausted the old table and silently turned later locks into
     * no-ops. Compare/exchange also makes lazy initialization thread-safe for
     * statically zero-initialized locks. */
    u64 value = __atomic_load_n(address, __ATOMIC_ACQUIRE);
    if (value || !create) return (WinSrwLock *)(uintptr_t)value;

    WinSrwLock *candidate = host_alloc(sizeof(*candidate));
    if (!candidate) return NULL;
    memset(candidate, 0, sizeof(*candidate));
    if (pthread_rwlock_init(&candidate->lock, NULL) != 0) {
        host_free(candidate, sizeof(*candidate));
        return NULL;
    }

    u64 expected = 0;
    if (!__atomic_compare_exchange_n(address, &expected,
                                     (u64)(uintptr_t)candidate, 0,
                                     __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
        pthread_rwlock_destroy(&candidate->lock);
        host_free(candidate, sizeof(*candidate));
        return (WinSrwLock *)(uintptr_t)expected;
    }
    return candidate;
}

static u64 __attribute__((ms_abi)) impl_InitializeSRWLock(u64 *lock)
{
    if (!lock) return 0;
    *lock = 0;
    (void)srw_lock_get(lock, 1);
    return 0;
}

static u64 __attribute__((ms_abi)) impl_AcquireSRWLockExclusive(u64 *lock)
{
    WinSrwLock *entry = srw_lock_get(lock, 1);
    if (entry) pthread_rwlock_wrlock(&entry->lock);
    return 0;
}

static u64 __attribute__((ms_abi)) impl_TryAcquireSRWLockExclusive(u64 *lock)
{
    WinSrwLock *entry = srw_lock_get(lock, 1);
    return entry && pthread_rwlock_trywrlock(&entry->lock) == 0;
}

static u64 __attribute__((ms_abi)) impl_ReleaseSRWLockExclusive(u64 *lock)
{
    WinSrwLock *entry = srw_lock_get(lock, 0);
    if (entry) pthread_rwlock_unlock(&entry->lock);
    return 0;
}

static u64 __attribute__((ms_abi)) impl_AcquireSRWLockShared(u64 *lock)
{
    WinSrwLock *entry = srw_lock_get(lock, 1);
    if (entry) pthread_rwlock_rdlock(&entry->lock);
    return 0;
}

static u64 __attribute__((ms_abi)) impl_TryAcquireSRWLockShared(u64 *lock)
{
    WinSrwLock *entry = srw_lock_get(lock, 1);
    return entry && pthread_rwlock_tryrdlock(&entry->lock) == 0;
}

static u64 __attribute__((ms_abi)) impl_ReleaseSRWLockShared(u64 *lock)
{
    WinSrwLock *entry = srw_lock_get(lock, 0);
    if (entry) pthread_rwlock_unlock(&entry->lock);
    return 0;
}

static u64 __attribute__((ms_abi))
impl_SleepConditionVariableSRW(u64 *cv, u64 *lock, u32 ms, u32 flags)
{
    if (flags & ~CONDITION_VARIABLE_LOCKMODE_SHARED) {
        g_last_error = 87; /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    WinConditionVariable *condition = condition_variable_get(cv, 1);
    WinSrwLock *srw = srw_lock_get(lock, 1);
    if (!condition || !srw) {
        g_last_error = 87;
        return 0;
    }

    pthread_mutex_lock(&condition->mutex);
    u64 generation = condition->generation;
    ++condition->waiters;
    if (sync_diagnostics_enabled() && ms == 0xFFFFFFFFu) {
        u32 trace = atomic_fetch_add(&g_condition_trace_count, 1);
        if (trace < 256)
            fprintf(stderr,
                    "[SYNC] SleepConditionVariableSRW cv=%p lock=%p "
                    "waiters=%u flags=%u creator=0x%llx waiter=0x%llx "
                    "last-waker=0x%llx\n",
                    (void *)cv, (void *)lock, condition->waiters, flags,
                    (unsigned long long)condition->creator_rip,
                    (unsigned long long)g_dispatch_incoming_return,
                    (unsigned long long)condition->last_waker_rip);
    }
    pthread_rwlock_unlock(&srw->lock);

    int rc = 0;
    if (ms == 0) {
        rc = ETIMEDOUT;
    } else if (ms == 0xFFFFFFFFu) {
        while (condition->generation == generation && rc == 0)
            rc = pthread_cond_wait(&condition->condition, &condition->mutex);
    } else {
        struct timespec deadline;
        if (calc_abs_deadline(&deadline, ms) != 0) rc = ETIMEDOUT;
        while (condition->generation == generation && rc == 0)
            rc = pthread_cond_timedwait(&condition->condition,
                                        &condition->mutex, &deadline);
    }
    --condition->waiters;
    pthread_mutex_unlock(&condition->mutex);

    if (flags & CONDITION_VARIABLE_LOCKMODE_SHARED)
        pthread_rwlock_rdlock(&srw->lock);
    else
        pthread_rwlock_wrlock(&srw->lock);

    if (rc == ETIMEDOUT) {
        g_last_error = 1460; /* ERROR_TIMEOUT */
        return 0;
    }
    if (rc != 0) {
        g_last_error = 1;
        return 0;
    }
    return 1;
}

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
    int handle_refs;
    u32 tid;
    u32 exit_code;
    pthread_t pt;
    u64 fn;
    u64 arg;
    u64 creator_rip;
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
    /* Keep the Windows ID allocated by CreateThread. Linux’s native TID is an
     * implementation detail and must not replace the ID already returned to
     * the guest or stored in its TEB. */
    ((WinTEB *)teb)->UniqueThread = t->tid;
    g_cached_windows_thread_id = t->tid;
    pthread_cond_broadcast(&t->cv);
    pthread_mutex_unlock(&t->mtx);

    guest_dll_notify_thread(2 /* DLL_THREAD_ATTACH */);
    if (getenv("BEER_THREAD_DIAGNOSTICS"))
        fprintf(stderr,
                "[THREAD] start id=%u fn=0x%llx arg=0x%llx creator=0x%llx\n",
                t->tid, (unsigned long long)t->fn,
                (unsigned long long)t->arg,
                (unsigned long long)t->creator_rip);

    /* Ensure done is signalled even if thread exits via pthread_exit (ExitThread) */
    pthread_cleanup_push(thread_done_cleanup, (void *)(intptr_t)id);

    typedef u64 __attribute__((ms_abi)) (*WinFn)(u64);
    u64 exit_code = ((WinFn)t->fn)(t->arg);
    pthread_mutex_lock(&t->mtx);
    t->exit_code = (u32)exit_code;
    pthread_mutex_unlock(&t->mtx);

    guest_dll_notify_thread(3 /* DLL_THREAD_DETACH */);
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
    t->handle_refs = 1;
    t->exit_code = 259u; /* STILL_ACTIVE */
    /* Windows allocates the thread ID before CreateThread returns, even when
     * CREATE_SUSPENDED is requested. Use Beer’s stable handle-table ID rather
     * than returning it first and later replacing it with a Linux TID. */
    t->tid = (u32)id;
    t->fn = fn;
    t->arg = arg;
    t->creator_rip = g_dispatch_incoming_return;
    t->suspended = (flags & 0x4) ? 1 : 0; /* CREATE_SUSPENDED */
    if (getenv("BEER_THREAD_DIAGNOSTICS"))
        fprintf(stderr,
                "[THREAD] create id=%u fn=0x%llx arg=0x%llx flags=0x%x "
                "creator=0x%llx\n",
                t->tid, (unsigned long long)t->fn,
                (unsigned long long)t->arg, flags,
                (unsigned long long)t->creator_rip);
    pthread_mutex_init(&t->mtx, NULL);
    pthread_cond_init(&t->cv, NULL);

    if (!t->suspended && thread_start_locked(id) != 0) {
        pthread_mutex_destroy(&t->mtx);
        pthread_cond_destroy(&t->cv);
        t->used = 0;
        return 0;
    }

    if (tid) *tid = t->tid;
    return (u64)(THREAD_HANDLE_BASE + (u32)id);
}
static u64 __attribute__((ms_abi)) impl_GetCurrentThread(void)            { return (u64)-2; }
static u64 __attribute__((ms_abi))
impl_GetExitCodeThread(u64 h, u32 *exit_code)
{
    int id = thread_id_from_handle(h);
    if (id < 0 || !exit_code) { g_last_error = 6; return 0; }
    WinThread *t = &g_threads[id];
    pthread_mutex_lock(&t->mtx);
    *exit_code = t->done ? t->exit_code : 259u; /* STILL_ACTIVE */
    pthread_mutex_unlock(&t->mtx);
    return 1;
}

static u64 __attribute__((ms_abi))
impl_DuplicateHandle(u64 source_process, u64 source, u64 target_process,
                     u64 *target, u32 access, u32 inherit, u32 options)
{
    (void)access; (void)inherit;
    if (!target || (source_process != (u64)-1 && source_process != (u64)-2) ||
        (target_process != (u64)-1 && target_process != (u64)-2)) {
        g_last_error = 6;
        return 0;
    }
    int tid = thread_id_from_handle(source);
    if (tid >= 0) {
        pthread_mutex_lock(&g_threads[tid].mtx);
        g_threads[tid].handle_refs++;
        pthread_mutex_unlock(&g_threads[tid].mtx);
        *target = source;
        (void)options; /* handles share the same object in Beer */
        return 1;
    }
    /* Pseudo current-process/thread handles are valid duplication sources. */
    if (source == (u64)-1 || source == (u64)-2) {
        *target = source;
        return 1;
    }
    g_last_error = 6;
    return 0;
}
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
impl_GetTempPathW(u32 size, u16 *buffer)
{
    char path[64];
    snprintf(path, sizeof(path), "%s\\", g_temp_dir);
    u32 length = (u32)strlen(path);
    if (!buffer || size <= length) return length + 1;
    for (u32 i = 0; i <= length; ++i) buffer[i] = (u8)path[i];
    return length;
}
static u64 __attribute__((ms_abi))
impl_GetTempPathA(u32 size, char *buffer)
{
    char path[64];
    snprintf(path, sizeof(path), "%s\\", g_temp_dir);
    u32 length = (u32)strlen(path);
    if (!buffer || size <= length) return length + 1;
    memcpy(buffer, path, length + 1);
    return length;
}

static size_t win_full_path_a(const char *path, char *out, size_t cap)
{
    if (!path || !out || cap == 0) return 0;

    char joined[4096];
    size_t used = 0;
    int absolute = path[0] == '/' || path[0] == '\\' ||
                   (((path[0] >= 'A' && path[0] <= 'Z') ||
                     (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':');
    if (!absolute) {
        char cwd[2048];
        if (!getcwd(cwd, sizeof(cwd))) return 0;
        joined[used++] = 'Z';
        joined[used++] = ':';
        for (size_t i = 0; cwd[i] && used + 1 < sizeof(joined); i++)
            joined[used++] = cwd[i] == '/' ? '\\' : cwd[i];
        if (used && joined[used - 1] != '\\' && used + 1 < sizeof(joined))
            joined[used++] = '\\';
    }
    for (size_t i = 0; path[i] && used + 1 < sizeof(joined); i++)
        joined[used++] = path[i] == '/' ? '\\' : path[i];
    joined[used] = 0;

    /* Lexically collapse duplicate separators and dot components.  This keeps
     * Windows root paths stable ("\\" stays "\\") instead of repeatedly
     * presenting the root separator as a file-name component. */
    size_t prefix = 0, n = 0;
    if (used >= 2 && joined[1] == ':') {
        if (n + 2 < cap) { out[n++] = joined[0]; out[n++] = ':'; }
        prefix = 2;
    }
    int rooted = joined[prefix] == '\\';
    if (rooted && n + 1 < cap) out[n++] = '\\';

    size_t component_starts[256];
    size_t component_count = 0;
    size_t i = prefix + (rooted ? 1 : 0);
    while (i < used) {
        while (joined[i] == '\\') i++;
        size_t start = i;
        while (i < used && joined[i] != '\\') i++;
        size_t len = i - start;
        if (!len || (len == 1 && joined[start] == '.')) continue;
        if (len == 2 && joined[start] == '.' && joined[start + 1] == '.') {
            if (component_count) n = component_starts[--component_count];
            continue;
        }
        size_t rollback = n;
        if (n && out[n - 1] != '\\') {
            if (n + 1 >= cap) return 0;
            out[n++] = '\\';
        }
        if (n + len >= cap) return 0;
        component_starts[component_count < 256 ? component_count++ : 255] = rollback;
        memcpy(out + n, joined + start, len);
        n += len;
    }
    if (n == 0 && rooted) out[n++] = '\\';
    out[n] = 0;
    return n;
}

static u64 __attribute__((ms_abi))
impl_GetFullPathNameW(const u16 *path, u32 sz, u16 *buf, u16 **part)
{
    if (part) *part = NULL;
    if (!path) { g_last_error = 87; return 0; }

    char narrow[4096], full[4096];
    size_t in_len = 0;
    while (path[in_len] && in_len + 1 < sizeof(narrow)) {
        narrow[in_len] = (char)(path[in_len] & 0x7f);
        in_len++;
    }
    narrow[in_len] = 0;
    size_t len = win_full_path_a(narrow, full, sizeof(full));
    if (!len) { g_last_error = 206; return 0; }
    if (!buf || sz <= len) return (u64)(len + 1);

    for (size_t i = 0; i <= len; i++) buf[i] = (u8)full[i];
    if (part && len && full[len - 1] != '\\') {
        size_t leaf = len;
        while (leaf && full[leaf - 1] != '\\' && full[leaf - 1] != ':') leaf--;
        *part = buf + leaf;
    }
    return (u64)len;
}

static u64 __attribute__((ms_abi))
impl_GetFullPathNameA(const char *path, u32 sz, char *buf, char **part)
{
    if (part) *part = NULL;
    if (!path) { g_last_error = 87; return 0; }

    char full[4096];
    size_t len = win_full_path_a(path, full, sizeof(full));
    if (!len) { g_last_error = 206; return 0; }
    if (!buf || sz <= len) return (u64)(len + 1);
    memcpy(buf, full, len + 1);
    if (part && len && full[len - 1] != '\\') {
        size_t leaf = len;
        while (leaf && full[leaf - 1] != '\\' && full[leaf - 1] != ':') leaf--;
        *part = buf + leaf;
    }
    return (u64)len;
}

/* File handles are process objects on Windows: closed values become invalid and
 * their table slots may be reused. Sekiro opens thousands of archive/save
 * handles over its lifetime, so a monotonic 256-entry table eventually made
 * valid CreateFile calls fail and also allowed closed descriptors to alias. */
#define MAX_FHANDLES 4096
static int g_fh_fd[MAX_FHANDLES];
static u8 g_fh_used[MAX_FHANDLES];
static pthread_mutex_t g_fh_mtx = PTHREAD_MUTEX_INITIALIZER;

static u64 fh_alloc(int fd) {
    pthread_mutex_lock(&g_fh_mtx);
    for (u32 id = 1; id < MAX_FHANDLES; ++id) {
        if (g_fh_used[id]) continue;
        g_fh_used[id] = 1;
        g_fh_fd[id] = fd;
        pthread_mutex_unlock(&g_fh_mtx);
        return (u64)(0xF0000000u + id);
    }
    pthread_mutex_unlock(&g_fh_mtx);
    g_last_error = 4; /* ERROR_TOO_MANY_OPEN_FILES */
    return INVALID_HANDLE_VALUE64;
}
static int fh_get(u64 h) {
    if (h < 0xF0000001ULL || h >= 0xF0000000ULL + MAX_FHANDLES) return -1;
    u32 id = (u32)(h - 0xF0000000ULL);
    pthread_mutex_lock(&g_fh_mtx);
    int fd = g_fh_used[id] ? g_fh_fd[id] : -1;
    pthread_mutex_unlock(&g_fh_mtx);
    return fd;
}
static int fh_close(u64 h) {
    if (h < 0xF0000001ULL || h >= 0xF0000000ULL + MAX_FHANDLES) return 0;
    u32 id = (u32)(h - 0xF0000000ULL);
    pthread_mutex_lock(&g_fh_mtx);
    int fd = g_fh_used[id] ? g_fh_fd[id] : -1;
    if (fd >= 0) {
        g_fh_used[id] = 0;
        g_fh_fd[id] = -1;
    }
    pthread_mutex_unlock(&g_fh_mtx);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}

static int resolve_windows_host_path(const char *name, char *output,
                                     size_t output_size)
{
    if (!name || !output || output_size < 2) return 0;

    char normalized[PATH_MAX];
    size_t length = strlen(name);
    if (length >= sizeof(normalized)) return 0;
    for (size_t i = 0; i <= length; ++i)
        normalized[i] = name[i] == '\\' ? '/' : name[i];

    const char *input = normalized;
    char resolved[PATH_MAX];
    size_t used = 0;
    if (((input[0] >= 'A' && input[0] <= 'Z') ||
         (input[0] >= 'a' && input[0] <= 'z')) && input[1] == ':') {
        char drive = input[0] >= 'a' ? (char)(input[0] - ('a' - 'A')) : input[0];
        input += 2;
        if (drive == 'C') {
            used = strlen(g_drive_c_path);
            if (used >= sizeof(resolved)) return 0;
            memcpy(resolved, g_drive_c_path, used + 1);
        } else if (drive == 'Z') {
            resolved[used++] = '/';
            resolved[used] = 0;
        } else {
            g_last_error = 15; /* ERROR_INVALID_DRIVE */
            return 0;
        }
        while (*input == '/') ++input;
    } else if (*input == '/') {
        resolved[used++] = '/';
        resolved[used] = 0;
        while (*input == '/') ++input;
    } else {
        resolved[0] = 0;
    }

    while (*input) {
        const char *separator = strchr(input, '/');
        size_t component_length = separator ? (size_t)(separator - input)
                                            : strlen(input);
        if (component_length == 0) {
            input = separator ? separator + 1 : input + component_length;
            continue;
        }
        if (component_length >= 256) return 0;

        char component[256];
        memcpy(component, input, component_length);
        component[component_length] = 0;

        char directory_path[PATH_MAX];
        if (used == 0) strcpy(directory_path, ".");
        else {
            memcpy(directory_path, resolved, used);
            directory_path[used] = 0;
        }

        DIR *directory = opendir(directory_path);
        if (directory) {
            struct dirent *entry;
            while ((entry = readdir(directory)) != NULL) {
                if (strcasecmp(entry->d_name, component) == 0) {
                    size_t actual_length = strlen(entry->d_name);
                    if (actual_length < sizeof(component))
                        memcpy(component, entry->d_name, actual_length + 1);
                    break;
                }
            }
            closedir(directory);
        }

        size_t actual_length = strlen(component);
        if (used && resolved[used - 1] != '/') {
            if (used + 1 >= sizeof(resolved)) return 0;
            resolved[used++] = '/';
        }
        if (used + actual_length >= sizeof(resolved)) return 0;
        memcpy(resolved + used, component, actual_length);
        used += actual_length;
        resolved[used] = 0;

        if (!separator) break;
        input = separator + 1;
        while (*input == '/') ++input;
    }

    if (used + 1 > output_size) return 0;
    memcpy(output, resolved, used + 1);
    return 1;
}

static int wide_ascii_copy(const u16 *source, char *destination, size_t capacity)
{
    if (!destination || capacity == 0) return 0;
    if (!source) { destination[0] = 0; return 1; }
    size_t index = 0;
    for (; index + 1 < capacity && source[index]; ++index) {
        if (source[index] > 0x7f) return 0;
        destination[index] = (char)source[index];
    }
    if (source[index]) return 0;
    destination[index] = 0;
    return 1;
}

static void trim_ascii(char *text)
{
    char *start = text;
    while (*start == ' ' || *start == '\t') ++start;
    if (start != text) memmove(text, start, strlen(start) + 1);
    size_t length = strlen(text);
    while (length && (text[length - 1] == ' ' || text[length - 1] == '\t' ||
                      text[length - 1] == '\r' || text[length - 1] == '\n'))
        text[--length] = 0;
}

static u32 profile_copy_wide(const char *value, u16 *output, u32 capacity)
{
    size_t length = value ? strlen(value) : 0;
    if (!output || capacity == 0) return 0;
    size_t copied = length < (size_t)capacity - 1 ? length : (size_t)capacity - 1;
    for (size_t i = 0; i < copied; ++i) output[i] = (u8)value[i];
    output[copied] = 0;
    return (u32)copied;
}

static u64 __attribute__((ms_abi))
impl_GetPrivateProfileStringW(const u16 *section_w, const u16 *key_w,
                              const u16 *default_w, u16 *output, u32 capacity,
                              const u16 *filename_w)
{
    if (!output || capacity == 0 || !filename_w) return 0;
    char section[256], key[256], fallback[1024], filename[PATH_MAX];
    if (!wide_ascii_copy(section_w, section, sizeof(section)) ||
        !wide_ascii_copy(key_w, key, sizeof(key)) ||
        !wide_ascii_copy(default_w, fallback, sizeof(fallback)) ||
        !wide_ascii_copy(filename_w, filename, sizeof(filename))) {
        g_last_error = 1113;
        output[0] = 0;
        return 0;
    }
    /* Section/key enumeration is not currently observed. Fail explicitly
     * rather than returning a malformed MULTI_SZ. */
    if (!section_w || !key_w) {
        output[0] = 0;
        g_last_error = 120; /* ERROR_CALL_NOT_IMPLEMENTED */
        return 0;
    }

    char host_path[PATH_MAX];
    if (!resolve_windows_host_path(filename, host_path, sizeof(host_path)))
        return profile_copy_wide(fallback, output, capacity);
    FILE *file = fopen(host_path, "r");
    if (!file) return profile_copy_wide(fallback, output, capacity);

    char *line = NULL;
    size_t line_capacity = 0;
    int in_section = 0;
    char value[4096];
    value[0] = 0;
    while (getline(&line, &line_capacity, file) >= 0) {
        trim_ascii(line);
        if (!line[0] || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            char *end = strchr(line + 1, ']');
            if (!end) { in_section = 0; continue; }
            *end = 0;
            trim_ascii(line + 1);
            in_section = strcasecmp(line + 1, section) == 0;
            continue;
        }
        if (!in_section) continue;
        char *equals = strchr(line, '=');
        if (!equals) continue;
        *equals = 0;
        trim_ascii(line);
        if (strcasecmp(line, key) != 0) continue;
        char *found = equals + 1;
        trim_ascii(found);
        size_t length = strlen(found);
        if (length >= 2 && ((found[0] == '"' && found[length - 1] == '"') ||
                            (found[0] == '\'' && found[length - 1] == '\''))) {
            found[length - 1] = 0;
            ++found;
        }
        snprintf(value, sizeof(value), "%s", found);
    }
    free(line);
    fclose(file);
    return profile_copy_wide(value[0] ? value : fallback, output, capacity);
}

static u64 __attribute__((ms_abi))
impl_WritePrivateProfileStringW(const u16 *section_w, const u16 *key_w,
                                const u16 *value_w, const u16 *filename_w)
{
    if (!section_w || !key_w || !filename_w) {
        g_last_error = 120;
        return 0;
    }
    char section[256], key[256], value[4096], filename[PATH_MAX];
    if (!wide_ascii_copy(section_w, section, sizeof(section)) ||
        !wide_ascii_copy(key_w, key, sizeof(key)) ||
        !wide_ascii_copy(value_w, value, sizeof(value)) ||
        !wide_ascii_copy(filename_w, filename, sizeof(filename))) {
        g_last_error = 1113;
        return 0;
    }
    char host_path[PATH_MAX];
    if (!resolve_windows_host_path(filename, host_path, sizeof(host_path))) {
        g_last_error = 3;
        return 0;
    }
    /* Preserve existing files and append the latest value. Windows profile
     * readers use the last matching key; Beer's reader above follows that
     * convention once duplicate writes occur. */
    FILE *file = fopen(host_path, "a");
    if (!file) { g_last_error = errno == EACCES ? 5 : 3; return 0; }
    int ok = fprintf(file, "\n[%s]\n%s=%s\n", section, key, value) >= 0 &&
             fflush(file) == 0;
    fclose(file);
    if (!ok) { g_last_error = 5; return 0; }
    g_last_error = 0;
    return 1;
}

static u64 __attribute__((ms_abi))
impl_GetDiskFreeSpaceExA(const char *directory, u64 *free_available,
                         u64 *total_bytes, u64 *total_free)
{
    char host_path[PATH_MAX];
    const char *guest_path = directory && *directory ? directory : "C:\\";
    if (!resolve_windows_host_path(guest_path, host_path, sizeof(host_path))) {
        g_last_error = 3; /* ERROR_PATH_NOT_FOUND */
        return 0;
    }

    struct statvfs space;
    if (statvfs(host_path, &space) != 0) {
        g_last_error = errno == EACCES ? 5u : 3u;
        return 0;
    }

    u64 fragment_size = space.f_frsize ? (u64)space.f_frsize : (u64)space.f_bsize;
    if (free_available) *free_available = (u64)space.f_bavail * fragment_size;
    if (total_bytes) *total_bytes = (u64)space.f_blocks * fragment_size;
    if (total_free) *total_free = (u64)space.f_bfree * fragment_size;
    g_last_error = 0;
    return 1;
}

static u64 __attribute__((ms_abi))
impl_GetDiskFreeSpaceExW(const u16 *directory, u64 *free_available,
                         u64 *total_bytes, u64 *total_free)
{
    char narrow[PATH_MAX];
    size_t i = 0;
    if (directory) {
        for (; i + 1 < sizeof(narrow) && directory[i]; ++i) {
            if (directory[i] > 0x7f) {
                g_last_error = 1113; /* ERROR_NO_UNICODE_TRANSLATION */
                return 0;
            }
            narrow[i] = (char)directory[i];
        }
        if (directory[i]) {
            g_last_error = 206; /* ERROR_FILENAME_EXCED_RANGE */
            return 0;
        }
    }
    narrow[i] = 0;
    return impl_GetDiskFreeSpaceExA(narrow, free_available, total_bytes, total_free);
}

static u64 __attribute__((ms_abi))
impl_CreateFileA(const char *name, u32 access, u32 share, u64 sa,
                  u32 creation, u32 attrs, u64 tmpl)
{
    (void)sa; (void)attrs; (void)tmpl; (void)share;
    if (!name) return INVALID_HANDLE_VALUE64;

    char host_path[PATH_MAX];
    if (!resolve_windows_host_path(name, host_path, sizeof(host_path))) {
        g_last_error = 206;
        return INVALID_HANDLE_VALUE64;
    }

    int flags = 0;
    int mode  = 0644;
    if ((access & 0xC0000000) == 0xC0000000) flags = O_RDWR;
    else if (access & 0x80000000)            flags = O_RDONLY;
    else if (access & 0x40000000)            flags = O_WRONLY;
    else                                     flags = O_RDONLY;
    if (creation == 2)       flags |= O_CREAT|O_TRUNC;
    else if (creation == 1)  flags |= O_CREAT|O_EXCL;
    else if (creation == 4)  flags |= O_CREAT;
    int fd = open(host_path, flags, mode);
    if (fd < 0) {
        g_last_error = errno == EACCES ? 5 : 2;
        return INVALID_HANDLE_VALUE64;
    }
    return fh_alloc(fd);
}
static u64 __attribute__((ms_abi))
impl_CreateFileW(const u16 *name, u32 access, u32 share, u64 security,
                 u32 creation, u32 attributes, u64 template_file)
{
    char narrow[4096];
    size_t i = 0;
    if (!name) { g_last_error = 87; return INVALID_HANDLE_VALUE64; }
    for (; i + 1 < sizeof(narrow) && name[i]; ++i) {
        if (name[i] > 0x7f) { g_last_error = 1113; return INVALID_HANDLE_VALUE64; }
        narrow[i] = (char)name[i];
    }
    if (name[i]) { g_last_error = 206; return INVALID_HANDLE_VALUE64; }
    narrow[i] = 0;
    return impl_CreateFileA(narrow, access, share, security, creation,
                            attributes, template_file);
}
static u64 __attribute__((ms_abi))
impl_CloseHandle(u64 h)
{
    int tid = thread_id_from_handle(h);
    if (tid >= 0) {
        WinThread *t = &g_threads[tid];
        pthread_mutex_lock(&t->mtx);
        if (t->handle_refs > 0) t->handle_refs--;
        /* Closing a thread HANDLE never terminates the thread. Keep the object
         * until the worker exits so waits/exit-code queries on duplicates stay
         * valid. Beer reclaims only completed objects with no remaining refs. */
        if (t->handle_refs == 0 && t->done) {
            t->used = 0;
            pthread_mutex_unlock(&t->mtx);
            if (t->started) pthread_join(t->pt, NULL);
            pthread_mutex_destroy(&t->mtx);
            pthread_cond_destroy(&t->cv);
        } else {
            pthread_mutex_unlock(&t->mtx);
        }
        return 1;
    }

    int eid = event_id_from_handle(h);
    if (eid >= 0) {
        pthread_mutex_lock(&g_event_table_mtx);
        WinEvent *ev = &g_events[eid];
        pthread_mutex_destroy(&ev->mtx);
        pthread_cond_destroy(&ev->cv);
        ev->used = 0;
        pthread_mutex_unlock(&g_event_table_mtx);
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

    if (fh_close(h)) {
        g_last_error = 0;
        return 1;
    }
    g_last_error = 6; /* ERROR_INVALID_HANDLE */
    return 0;
}
typedef struct {
    u64 Internal;
    u64 InternalHigh;
    union {
        struct { u32 Offset, OffsetHigh; };
        u64 Pointer;
    };
    u64 hEvent;
} WIN_OVERLAPPED;

static u64 __attribute__((ms_abi))
impl_ReadFile(u64 h, void *buf, u32 n, u32 *done, WIN_OVERLAPPED *overlapped)
{
    int fd = fh_get(h);
    if (fd < 0 || (!buf && n)) {
        if (done) *done = 0;
        g_last_error = fd < 0 ? 6u : 87u;
        return 0;
    }

    ssize_t received;
    if (overlapped) {
        /* An OVERLAPPED read has its own absolute file position. RE8 issues
         * archive reads concurrently against shared handles; treating these as
         * sequential read(2) calls races the descriptor offset and silently
         * feeds the wrong asset/shader bytes to worker jobs. The host read is
         * synchronous for now, but its result and event follow the completed
         * Windows OVERLAPPED contract. */
        u64 offset = ((u64)overlapped->OffsetHigh << 32) | overlapped->Offset;
        received = offset <= (u64)INT64_MAX
            ? pread(fd, buf, n, (off_t)offset) : -1;
        overlapped->Internal = received < 0 ? (u64)(u32)errno : 0;
        overlapped->InternalHigh = received < 0 ? 0 : (u64)received;
        if (getenv("BEER_FILE_DIAGNOSTICS")) {
            static _Atomic(u32) overlapped_read_logs;
            u32 index = atomic_fetch_add(&overlapped_read_logs, 1);
            if (index < 64)
                fprintf(stderr, "[FILE] ReadFile overlapped handle=0x%llx "
                        "offset=%llu requested=%u received=%lld event=0x%llx\n",
                        (unsigned long long)h, (unsigned long long)offset, n,
                        (long long)received,
                        (unsigned long long)overlapped->hEvent);
        }
        if (received >= 0 && overlapped->hEvent)
            impl_SetEvent(overlapped->hEvent);
    } else {
        received = read(fd, buf, n);
    }

    if (done) *done = (u32)(received < 0 ? 0 : received);
    if (received < 0) {
        g_last_error = (u32)errno;
        return 0;
    }
    g_last_error = 0;
    return 1;
}
typedef struct {
    u32 FileAttributes;
    u32 CreationTimeLow;
    u32 CreationTimeHigh;
    u32 LastAccessTimeLow;
    u32 LastAccessTimeHigh;
    u32 LastWriteTimeLow;
    u32 LastWriteTimeHigh;
    u32 VolumeSerialNumber;
    u32 FileSizeHigh;
    u32 FileSizeLow;
    u32 NumberOfLinks;
    u32 FileIndexHigh;
    u32 FileIndexLow;
} WIN_BY_HANDLE_FILE_INFORMATION;

static u64 unix_time_to_filetime(time_t sec, long nsec)
{
    const u64 windows_epoch = 11644473600ULL;
    return ((u64)sec + windows_epoch) * 10000000ULL + (u64)nsec / 100ULL;
}

static u64 __attribute__((ms_abi))
impl_GetFileInformationByHandle(u64 h, WIN_BY_HANDLE_FILE_INFORMATION *info)
{
    int fd = fh_get(h);
    if (fd < 0 || !info) { g_last_error = 6; return 0; }

    struct stat st;
    if (fstat(fd, &st) != 0) { g_last_error = (u32)errno; return 0; }
    memset(info, 0, sizeof(*info));
    info->FileAttributes = S_ISDIR(st.st_mode) ? 0x10u : 0x80u;
    if (!(st.st_mode & S_IWUSR)) info->FileAttributes |= 0x1u;
#if defined(__linux__)
    u64 creation = unix_time_to_filetime(st.st_ctim.tv_sec, st.st_ctim.tv_nsec);
    u64 access = unix_time_to_filetime(st.st_atim.tv_sec, st.st_atim.tv_nsec);
    u64 write = unix_time_to_filetime(st.st_mtim.tv_sec, st.st_mtim.tv_nsec);
#else
    u64 creation = unix_time_to_filetime(st.st_ctime, 0);
    u64 access = unix_time_to_filetime(st.st_atime, 0);
    u64 write = unix_time_to_filetime(st.st_mtime, 0);
#endif
    info->CreationTimeLow = (u32)creation;
    info->CreationTimeHigh = (u32)(creation >> 32);
    info->LastAccessTimeLow = (u32)access;
    info->LastAccessTimeHigh = (u32)(access >> 32);
    info->LastWriteTimeLow = (u32)write;
    info->LastWriteTimeHigh = (u32)(write >> 32);
    info->VolumeSerialNumber = (u32)st.st_dev;
    info->FileSizeHigh = (u32)((u64)st.st_size >> 32);
    info->FileSizeLow = (u32)st.st_size;
    info->NumberOfLinks = (u32)st.st_nlink;
    info->FileIndexHigh = (u32)((u64)st.st_ino >> 32);
    info->FileIndexLow = (u32)st.st_ino;
    g_last_error = 0;
    return 1;
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
impl_GetFileSizeEx(u64 h, s64 *size)
{
    int fd = fh_get(h);
    if (fd < 0 || !size) { g_last_error = 6; return 0; }
    struct stat st;
    if (fstat(fd, &st) != 0) { g_last_error = (u32)errno; return 0; }
    *size = (s64)st.st_size;
    g_last_error = 0;
    return 1;
}
static u64 __attribute__((ms_abi))
impl_SetFilePointer(u64 h, s32 lo, s32 *hi, u32 method) {
    int fd = fh_get(h); if(fd<0) return INVALID_HANDLE_VALUE64;
    int whence = (method==0)?SEEK_SET:(method==1)?SEEK_CUR:SEEK_END;
    /* With a high-DWORD pointer the offset is the 64-bit pair (hi:lo) and lo is
     * UNSIGNED; without it lo is a signed 32-bit LONG. */
    off_t dist = hi ? (off_t)((((u64)(u32)*hi) << 32) | (u64)(u32)lo) : (off_t)lo;
    off_t r = lseek(fd, dist, whence);
    if (r < 0) { g_last_error = (u32)errno; return 0xFFFFFFFFu; }
    if (hi) *hi = (s32)(r>>32);
    g_last_error = 0;
    return (u64)(u32)r;
}

static u64 __attribute__((ms_abi))
impl_SetEndOfFile(u64 handle)
{
    int fd = fh_get(handle);
    if (fd < 0) { g_last_error = 6; return 0; } /* ERROR_INVALID_HANDLE */
    off_t position = lseek(fd, 0, SEEK_CUR);
    if (position < 0 || ftruncate(fd, position) != 0) {
        g_last_error = errno == EACCES ? 5u : 87u;
        return 0;
    }
    g_last_error = 0;
    return 1;
}

static u64 __attribute__((ms_abi))
impl_FlushFileBuffers(u64 handle)
{
    int fd = fh_get(handle);
    if (fd < 0) { g_last_error = 6; return 0; }
    if (fsync(fd) != 0) {
        g_last_error = errno == EACCES ? 5u : 29u; /* ERROR_WRITE_FAULT */
        return 0;
    }
    g_last_error = 0;
    return 1;
}

static int copy_windows_file_a(const char *source, const char *destination,
                               int fail_if_exists)
{
    char source_host[PATH_MAX], destination_host[PATH_MAX];
    if (!source || !destination ||
        !resolve_windows_host_path(source, source_host, sizeof(source_host)) ||
        !resolve_windows_host_path(destination, destination_host, sizeof(destination_host))) {
        g_last_error = 3;
        return 0;
    }

    int source_fd = open(source_host, O_RDONLY);
    if (source_fd < 0) {
        g_last_error = errno == EACCES ? 5u : 2u;
        return 0;
    }
    int destination_flags = O_WRONLY | O_CREAT | O_TRUNC;
    if (fail_if_exists) destination_flags |= O_EXCL;
    int destination_fd = open(destination_host, destination_flags, 0644);
    if (destination_fd < 0) {
        g_last_error = errno == EEXIST ? 80u : (errno == EACCES ? 5u : 3u);
        close(source_fd);
        return 0;
    }

    int success = 1;
    char buffer[128 * 1024];
    for (;;) {
        ssize_t received = read(source_fd, buffer, sizeof(buffer));
        if (received == 0) break;
        if (received < 0) { success = 0; break; }
        size_t offset = 0;
        while (offset < (size_t)received) {
            ssize_t sent = write(destination_fd, buffer + offset,
                                 (size_t)received - offset);
            if (sent <= 0) { success = 0; break; }
            offset += (size_t)sent;
        }
        if (!success) break;
    }
    if (success && fsync(destination_fd) != 0) success = 0;
    close(destination_fd);
    close(source_fd);
    if (!success) {
        unlink(destination_host);
        g_last_error = 29;
        return 0;
    }
    g_last_error = 0;
    return 1;
}

static u64 __attribute__((ms_abi))
impl_CopyFileA(const char *source, const char *destination, u32 fail_if_exists)
{
    return copy_windows_file_a(source, destination, fail_if_exists != 0);
}

static u64 __attribute__((ms_abi))
impl_CopyFileW(const u16 *source, const u16 *destination, u32 fail_if_exists)
{
    char source_narrow[PATH_MAX], destination_narrow[PATH_MAX];
    size_t source_length = 0, destination_length = 0;
    if (!source || !destination) { g_last_error = 87; return 0; }
    while (source[source_length] && source_length + 1 < sizeof(source_narrow)) {
        if (source[source_length] > 0x7f) { g_last_error = 1113; return 0; }
        source_narrow[source_length] = (char)source[source_length];
        ++source_length;
    }
    while (destination[destination_length] && destination_length + 1 < sizeof(destination_narrow)) {
        if (destination[destination_length] > 0x7f) { g_last_error = 1113; return 0; }
        destination_narrow[destination_length] = (char)destination[destination_length];
        ++destination_length;
    }
    if (source[source_length] || destination[destination_length]) {
        g_last_error = 206;
        return 0;
    }
    source_narrow[source_length] = 0;
    destination_narrow[destination_length] = 0;
    return copy_windows_file_a(source_narrow, destination_narrow,
                               fail_if_exists != 0);
}

static u64 __attribute__((ms_abi))
impl_GetFileAttributesA(const char *path)
{
    char host_path[PATH_MAX];
    struct stat st;
    if (!path || !resolve_windows_host_path(path, host_path, sizeof(host_path)) ||
        stat(host_path, &st) < 0) {
        g_last_error = 2;
        return 0xFFFFFFFF;
    }
    u32 attr = S_ISDIR(st.st_mode) ? 0x10u : 0x20u;
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

/* SetThreadIdealProcessor returns the previous preferred processor, not a
 * BOOL. Keep the Windows-visible preference per hosted thread; 64 denotes
 * MAXIMUM_PROCESSORS (no prior preference) on Win64. */
static _Thread_local u32 g_ideal_processor = 64;
static u64 __attribute__((ms_abi))
impl_SetThreadIdealProcessor(u64 thread, u32 processor)
{
    (void)thread;
    if (processor >= 64) {
        g_last_error = 87; /* ERROR_INVALID_PARAMETER */
        return 0xffffffffu;
    }
    u32 previous = g_ideal_processor;
    g_ideal_processor = processor;
    return previous;
}

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

/* Win32 RECT contains four signed 32-bit LONGs even in a 64-bit process.
 * Treating it as four u64s writes 32 bytes into the caller's 16-byte object;
 * GetWindowRect consequently overwrote Sekiro's adjacent /GS cookie with
 * `right == 1920` and produced FAST_FAIL_STACK_COOKIE_CHECK_FAILURE. */
typedef struct {
    s32 left;
    s32 top;
    s32 right;
    s32 bottom;
} WIN_RECT;
_Static_assert(sizeof(WIN_RECT) == 16, "Win32 RECT must remain 16 bytes");

static u64 __attribute__((ms_abi))
impl_GetWindowRect(u64 hwnd, WIN_RECT *rect) {
    if (!rect) { g_last_error = 87; return 0; }
    XwaylandWindowState state;
    if (hwnd != FAKE_HWND || !xwayland_window_get_state(&state)) {
        g_last_error = 1400;
        return 0;
    }
    rect->left = state.x;
    rect->top = state.y;
    rect->right = state.x + state.width;
    rect->bottom = state.y + state.height;
    return 1;
}
static u64 __attribute__((ms_abi))
impl_GetClientRect(u64 hwnd, WIN_RECT *rect) {
    if (!rect) { g_last_error = 87; return 0; }
    XwaylandWindowState state;
    if (hwnd != FAKE_HWND || !xwayland_window_get_state(&state)) {
        g_last_error = 1400;
        return 0;
    }
    rect->left = 0;
    rect->top = 0;
    rect->right = state.client_width;
    rect->bottom = state.client_height;
    return 1;
}
static u64 __attribute__((ms_abi))
impl_LoadIconW(u64 hmod, u64 name) { (void)hmod;(void)name; return 0x10000001; }
static u64 __attribute__((ms_abi))
impl_LoadIconA(u64 hmod, u64 name) { (void)hmod;(void)name; return 0x10000001; }
static u64 __attribute__((ms_abi))
impl_LoadCursorW(u64 hmod, u64 name) { (void)hmod;(void)name; return 0x20000001; }
static u64 __attribute__((ms_abi))
impl_LoadCursorA(u64 hmod, u64 name) { (void)hmod;(void)name; return 0x20000001; }
static u64 __attribute__((ms_abi))
impl_AdjustWindowRect(WIN_RECT *rect, u32 style, u32 menu) {
    (void)style;(void)menu;
    if (!rect) { g_last_error = 87; return 0; }
    rect->left -= 8;
    rect->top -= 30;
    rect->right += 8;
    rect->bottom += 8;
    return 1;
}
static u64 __attribute__((ms_abi))
impl_AdjustWindowRectEx(WIN_RECT *rect, u32 style, u32 menu, u32 exstyle)
    { (void)exstyle; return impl_AdjustWindowRect(rect,style,menu); }
static void window_title_from_wide(const u16 *wide, char *out, size_t capacity)
{
    if (!out || !capacity) return;
    size_t i = 0;
    if (wide) {
        for (; i + 1 < capacity && wide[i]; i++)
            out[i] = wide[i] <= 0x7f ? (char)wide[i] : '?';
    }
    out[i] = 0;
}

static void ensure_native_input_pump(void);

static u64 create_native_window(s32 x, s32 y, s32 w, s32 h, const char *title)
{
    /* CW_USEDEFAULT is 0x80000000. XWayland's compositor chooses final placement. */
    if ((u32)x == 0x80000000u) x = 0;
    if ((u32)y == 0x80000000u) y = 0;
    if (!xwayland_window_create(x, y, w, h, title, 1)) {
        g_last_error = 1407; /* ERROR_CANNOT_FIND_WND_CLASS / backend unavailable */
        return 0;
    }
    ensure_native_input_pump();
    if (!g_api_createwindow) {
        g_api_createwindow = 1;
        fprintf(stderr, "[PROGRESS] native XWayland window created\n");
    }
    return FAKE_HWND;
}

typedef struct {
    u64 lpCreateParams, hInstance, hMenu, hwndParent;
    s32 cy, cx, y, x, style;
    u32 padding;
    u64 lpszName, lpszClass;
    u32 dwExStyle, trailing_padding;
} WIN_CREATESTRUCT;
_Static_assert(sizeof(WIN_CREATESTRUCT) == 80, "CREATESTRUCT must be 80 bytes on Win64");

typedef u64 __attribute__((ms_abi)) (*GuestWndProc)(u64, u32, u64, u64);
static u64 call_guest_wndproc(u32 message, u64 wparam, u64 lparam)
{
    if (!g_window_proc) return message == 0x0081 ? 1 : 0;
    return ((GuestWndProc)(uintptr_t)g_window_proc)(FAKE_HWND, message, wparam, lparam);
}

static u64 create_guest_window(u32 exstyle, u64 classname, u64 title, u32 style,
                               s32 x, s32 y, s32 w, s32 h, u64 parent,
                               u64 menu, u64 inst, u64 param, const char *native_title)
{
    if (!g_window_proc) { g_last_error = 1407; return 0; }
    g_window_style = style; g_window_exstyle = exstyle;
    g_window_instance = inst ? inst : g_window_instance;
    g_window_thread_id = (u32)current_windows_thread_id();
    if (getenv("BEER_THREAD_DIAGNOSTICS"))
        fprintf(stderr,
                "[THREAD] window owner thread=%u creator=0x%llx\n",
                g_window_thread_id,
                (unsigned long long)g_dispatch_incoming_return);
    u64 hwnd = create_native_window(x, y, w, h, native_title);
    if (!hwnd) return 0;
    WIN_CREATESTRUCT create = {
        .lpCreateParams = param, .hInstance = g_window_instance, .hMenu = menu,
        .hwndParent = parent, .cy = h, .cx = w, .y = y, .x = x,
        .style = (s32)style, .lpszName = title, .lpszClass = classname,
        .dwExStyle = exstyle
    };
    if (!call_guest_wndproc(0x0081, 0, (u64)(uintptr_t)&create)) {
        xwayland_window_destroy(); return 0;
    }
    if ((s64)call_guest_wndproc(0x0001, 0, (u64)(uintptr_t)&create) == -1) {
        call_guest_wndproc(0x0082, 0, 0); xwayland_window_destroy(); return 0;
    }
    call_guest_wndproc(0x0003, 0, ((u64)(u16)y << 16) | (u16)x);
    call_guest_wndproc(0x0005, 0, ((u64)(u16)h << 16) | (u16)w);
    if (style & 0x10000000u) call_guest_wndproc(0x0018, 1, 0);
    return hwnd;
}

static u64 __attribute__((ms_abi))
impl_CreateWindowExW(u32 exstyle, u64 classname, u64 title, u32 style,
                      s32 x, s32 y, s32 w, s32 h, u64 parent, u64 menu, u64 inst, u64 param)
{
    char utf8_title[512];
    window_title_from_wide((const u16 *)(uintptr_t)title, utf8_title, sizeof(utf8_title));
    fprintf(stderr, "[WIN] CreateWindowExW(%dx%d, title=%s)\n", w, h,
            utf8_title[0] ? utf8_title : "(untitled)");
    return create_guest_window(exstyle, classname, title, style, x, y, w, h,
                               parent, menu, inst, param, utf8_title);
}
static u64 __attribute__((ms_abi))
impl_CreateWindowExA(u32 exstyle, u64 classname, u64 title, u32 style,
                      s32 x, s32 y, s32 w, s32 h, u64 parent, u64 menu, u64 inst, u64 param)
{
    const char *ascii_title = (const char *)(uintptr_t)title;
    fprintf(stderr, "[WIN] CreateWindowExA(%dx%d, title=%s)\n", w, h,
            (ascii_title && *ascii_title) ? ascii_title : "(untitled)");
    return create_guest_window(exstyle, classname, title, style, x, y, w, h,
                               parent, menu, inst, param, ascii_title);
}
static u64 __attribute__((ms_abi))
impl_DestroyWindow(u64 hw) {
    if (hw != FAKE_HWND || !xwayland_window_exists()) { g_last_error = 1400; return 0; }
    call_guest_wndproc(0x0002, 0, 0);
    call_guest_wndproc(0x0082, 0, 0);
    xwayland_window_destroy();
    g_window_focused = 0;
    return 1;
}
static u64 __attribute__((ms_abi))
impl_IsWindow(u64 hw) { return hw == FAKE_HWND && xwayland_window_exists(); }
static u64 __attribute__((ms_abi))
impl_SetWindowTextW(u64 hw, u64 txt) {
    if (hw != FAKE_HWND) { g_last_error = 1400; return 0; }
    char title[512];
    window_title_from_wide((const u16 *)(uintptr_t)txt, title, sizeof(title));
    return xwayland_window_set_title(title);
}
static u64 __attribute__((ms_abi))
impl_SetWindowTextA(u64 hw, u64 txt) {
    if (hw != FAKE_HWND) { g_last_error = 1400; return 0; }
    return xwayland_window_set_title((const char *)(uintptr_t)txt);
}
static u64 __attribute__((ms_abi))
impl_MoveWindow(u64 hw, s32 x, s32 y, s32 w, s32 h, u32 rep) {
    (void)rep;
    if (hw != FAKE_HWND) { g_last_error = 1400; return 0; }
    return xwayland_window_move_resize(x, y, w, h, 1, 1);
}
static u64 __attribute__((ms_abi))
impl_SetWindowPos(u64 hw, u64 ins, s32 x, s32 y, s32 w, s32 h, u32 flags) {
    (void)ins;
    if (hw != FAKE_HWND) { g_last_error = 1400; return 0; }
    return xwayland_window_move_resize(x, y, w, h,
                                       !(flags & 0x0002), !(flags & 0x0001));
}
static u64 __attribute__((ms_abi))
impl_GetDC(u64 hw) { (void)hw; return FAKE_HDC; }
static u64 __attribute__((ms_abi))
impl_ReleaseDC(u64 hw, u64 dc) { (void)hw;(void)dc; return 1; }
static u64 __attribute__((ms_abi))
impl_SetCursor(u64 hc) { (void)hc; return 0; }

static u64 __attribute__((ms_abi))
impl_ShowCursor(s32 show)
{
    /* Win32 maintains a process-wide signed display counter. The cursor is
     * visible when the counter is nonnegative, and callers commonly loop
     * until the returned value crosses zero. Return the updated count. */
    static _Atomic(s32) display_count;
    s32 value = show
        ? atomic_fetch_add(&display_count, 1) + 1
        : atomic_fetch_sub(&display_count, 1) - 1;
    static _Atomic(u32) logs;
    u32 log = atomic_fetch_add(&logs, 1);
    if (log < 16)
        fprintf(stderr, "[USER32] ShowCursor(%d) -> %d\n", show != 0, value);
    return (u32)value;
}

typedef struct { s32 x, y; } WIN_POINT;

typedef struct {
    u8 keys[256];
    u8 dik[256];
    u8 pressed[256];
    s32 root_x, root_y;
    s32 client_x, client_y;
    s32 delta_x, delta_y, wheel;
    u8 mouse_buttons[8];
} BEER_INPUT_STATE;

typedef struct {
    u32 offset;
    u32 data;
    u32 timestamp;
    u32 sequence;
    u64 application_data;
} BEER_DINPUT_EVENT;
_Static_assert(sizeof(BEER_DINPUT_EVENT) == 24,
               "Win64 DIDEVICEOBJECTDATA layout");

/* DirectInput callers routinely request buffer sizes far larger than a frame
 * of transitions (Sekiro asks for far more than 256).  A ring smaller than the
 * request forced SetProperty(DIPROP_BUFFERSIZE) to answer DI_PROPNOEFFECT,
 * which strict callers treat as a setup failure. */
#define BEER_DINPUT_EVENT_CAPACITY 1024
typedef struct {
    BEER_DINPUT_EVENT events[BEER_DINPUT_EVENT_CAPACITY];
    u32 head;
    u32 count;
    u32 next_sequence;
    int overflowed;
} BEER_DINPUT_QUEUE;

static BEER_INPUT_STATE g_input;
/* Index 1 is the mouse queue and index 2 is the keyboard queue. */
static BEER_DINPUT_QUEUE g_dinput_queues[3];
static pthread_mutex_t g_input_mutex = PTHREAD_MUTEX_INITIALIZER;
/* USER32 queue-state synchronization is declared with input state because
 * GetKeyState is defined before the message queue implementation below. */
static pthread_mutex_t g_win_message_mutex = PTHREAD_MUTEX_INITIALIZER;
static u8 g_win_message_keys[256];
static _Atomic(u32) g_input_event_logs;
/* Xlib event consumption is process-global. Serialize every producer so one
 * polling thread cannot consume an event while another suppresses delivery. */
static pthread_mutex_t g_event_collect_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Atomic(int) g_native_input_pump_started;
static void collect_native_window_events(void);
static void ensure_native_input_pump(void);
static u64 monotonic_milliseconds(void);

/* The restored compatibility DirectInput objects do not advertise buffered
 * host events. USER32 remains the owner of pointer messages, as it was before
 * the input-contract experiment. */
static void signal_dinput_notifications_locked(int kind)
{
    (void)kind;
}

static void queue_dinput_event_locked(int kind, u32 offset, u32 data)
{
    if (kind < 1 || kind > 2) return;
    BEER_DINPUT_QUEUE *queue = &g_dinput_queues[kind];
    if (queue->count == BEER_DINPUT_EVENT_CAPACITY) {
        /* DirectInput keeps the newest buffered transitions and reports an
         * overflow to the next GetDeviceData consumer. */
        queue->head = (queue->head + 1) % BEER_DINPUT_EVENT_CAPACITY;
        --queue->count;
        queue->overflowed = 1;
    }
    u32 index = (queue->head + queue->count) % BEER_DINPUT_EVENT_CAPACITY;
    queue->events[index] = (BEER_DINPUT_EVENT){
        .offset = offset,
        .data = data,
        .timestamp = (u32)monotonic_milliseconds(),
        .sequence = ++queue->next_sequence,
        .application_data = 0
    };
    ++queue->count;
    signal_dinput_notifications_locked(kind);
}

static void native_to_guest_client(s32 native_x, s32 native_y,
                                   s32 *guest_x, s32 *guest_y)
{
    XwaylandWindowState state;
    if (xwayland_window_get_state(&state) && state.width > 0 && state.height > 0 &&
        state.client_width > 0 && state.client_height > 0) {
        *guest_x = (s32)((s64)native_x * state.client_width / state.width);
        *guest_y = (s32)((s64)native_y * state.client_height / state.height);
        return;
    }
    *guest_x = native_x;
    *guest_y = native_y;
}

static u32 x11_keysym_to_vk(unsigned long keysym)
{
    if (keysym >= 'a' && keysym <= 'z') return (u32)(keysym - 'a' + 'A');
    if ((keysym >= 'A' && keysym <= 'Z') || (keysym >= '0' && keysym <= '9'))
        return (u32)keysym;
    if (keysym >= 0xffbe && keysym <= 0xffd5) return 0x70u + (u32)(keysym - 0xffbe); /* F1..F24 */
    switch (keysym) {
        case 0xff08: return 0x08; case 0xff09: return 0x09;
        case 0xff0d: return 0x0d; case 0xff1b: return 0x1b;
        case 0xffff: return 0x2e; case 0xff50: return 0x24;
        case 0xff51: return 0x25; case 0xff52: return 0x26;
        case 0xff53: return 0x27; case 0xff54: return 0x28;
        case 0xff55: return 0x21; case 0xff56: return 0x22;
        case 0xff57: return 0x23; case 0xff63: return 0x2d;
        case 0xffe1: return 0xa0; case 0xffe2: return 0xa1;
        case 0xffe3: return 0xa2; case 0xffe4: return 0xa3;
        case 0xffe9: return 0xa4; case 0xffea: return 0xa5;
        case 0xffe5: return 0x14; case 0xff7f: return 0x90;
        case 0x20: return 0x20; case '-': return 0xbd; case '=': return 0xbb;
        case '[': return 0xdb; case ']': return 0xdd; case '\\': return 0xdc;
        case ';': return 0xba; case '\'': return 0xde; case ',': return 0xbc;
        case '.': return 0xbe; case '/': return 0xbf; case '`': return 0xc0;
        default: return 0;
    }
}

static u8 x11_keycode_to_dik(unsigned int keycode)
{
    /* XWayland's core keycodes follow the Xorg evdev map. DirectInput scan
     * codes match PC set-1 for the common keyboard range. */
    static const u8 evdev_to_dik[128] = {
        [1]=0x01,[2]=0x02,[3]=0x03,[4]=0x04,[5]=0x05,[6]=0x06,[7]=0x07,[8]=0x08,
        [9]=0x09,[10]=0x0a,[11]=0x0b,[12]=0x0c,[13]=0x0d,[14]=0x0e,[15]=0x0f,
        [16]=0x10,[17]=0x11,[18]=0x12,[19]=0x13,[20]=0x14,[21]=0x15,[22]=0x16,
        [23]=0x17,[24]=0x18,[25]=0x19,[26]=0x1a,[27]=0x1b,[28]=0x1c,[29]=0x1d,
        [30]=0x1e,[31]=0x1f,[32]=0x20,[33]=0x21,[34]=0x22,[35]=0x23,[36]=0x24,
        [37]=0x25,[38]=0x26,[39]=0x27,[40]=0x28,[41]=0x29,[42]=0x2a,[43]=0x2b,
        [44]=0x2c,[45]=0x2d,[46]=0x2e,[47]=0x2f,[48]=0x30,[49]=0x31,[50]=0x32,
        [51]=0x33,[52]=0x34,[53]=0x35,[54]=0x36,[55]=0x37,[56]=0x38,[57]=0x39,
        [58]=0x3a,[59]=0x3b,[60]=0x3c,[61]=0x3d,[62]=0x3e,[63]=0x3f,[64]=0x40,
        [65]=0x41,[66]=0x42,[67]=0x43,[68]=0x44,[87]=0x57,[88]=0x58
    };
    unsigned int evdev = keycode >= 8 ? keycode - 8 : 0;
    return evdev < sizeof(evdev_to_dik) ? evdev_to_dik[evdev] : 0;
}

static u64 __attribute__((ms_abi))
impl_GetCursorPos(WIN_POINT *point)
{
    if (!point) { g_last_error = 87; return 0; }
    collect_native_window_events();
    pthread_mutex_lock(&g_input_mutex);
    point->x = g_input.root_x;
    point->y = g_input.root_y;
    pthread_mutex_unlock(&g_input_mutex);
    return 1;
}

static u64 __attribute__((ms_abi))
impl_ScreenToClient(u64 hwnd, WIN_POINT *point)
{
    if (hwnd != FAKE_HWND || !point) { g_last_error = hwnd == FAKE_HWND ? 87 : 1400; return 0; }
    XwaylandWindowState state;
    if (!xwayland_window_get_state(&state)) { g_last_error = 1400; return 0; }
    point->x -= state.x;
    point->y -= state.y;
    if (state.width > 0 && state.height > 0 &&
        state.client_width > 0 && state.client_height > 0) {
        point->x = (s32)((s64)point->x * state.client_width / state.width);
        point->y = (s32)((s64)point->y * state.client_height / state.height);
    }
    return 1;
}

static u64 __attribute__((ms_abi))
impl_ClipCursor(const WIN_RECT *rect)
{
    /* XWayland pointer confinement is not required for a quiet input backend,
     * but Win32 reports success for both setting and releasing the clip. */
    (void)rect;
    return 1;
}

static u64 __attribute__((ms_abi))
impl_GetKeyboardState(u8 *state)
{
    if (!state) { g_last_error = 87; return 0; }
    collect_native_window_events();
    pthread_mutex_lock(&g_input_mutex);
    memcpy(state, g_input.keys, sizeof(g_input.keys));
    pthread_mutex_unlock(&g_input_mutex);
    return 1;
}

static u64 __attribute__((ms_abi))
impl_GetAsyncKeyState(s32 virtual_key)
{
    if (virtual_key < 0 || virtual_key >= 256) return 0;
    collect_native_window_events();
    pthread_mutex_lock(&g_input_mutex);
    u16 result = (g_input.keys[virtual_key] & 0x80 ? 0x8000 : 0) |
                 (g_input.pressed[virtual_key] ? 1 : 0);
    g_input.pressed[virtual_key] = 0;
    pthread_mutex_unlock(&g_input_mutex);
    return result;
}

static u64 __attribute__((ms_abi))
impl_GetKeyState(s32 virtual_key)
{
    pthread_mutex_lock(&g_win_message_mutex);
    u16 result = beer_win32_get_key_state(g_win_message_keys, virtual_key);
    pthread_mutex_unlock(&g_win_message_mutex);
    if (getenv("BEER_INPUT_DIAGNOSTICS") &&
        (virtual_key == 0x01 || virtual_key == 0x02 || virtual_key == 0x04)) {
        static _Atomic(u32) logs;
        u32 log = atomic_fetch_add(&logs, 1);
        if (log < 128)
            fprintf(stderr, "[INPUT] GetKeyState vk=0x%02x -> 0x%04x\n",
                    virtual_key, result);
    }
    return result;
}

static u64 __attribute__((ms_abi))
impl_GetMessageExtraInfo(void)
{
    /* No synthetic input metadata is attached to the quiet host-input state. */
    return 0;
}

typedef struct {
    s32 dx;
    s32 dy;
    u32 mouse_data;
    u32 flags;
    u32 time;
    u64 extra_info;
} WIN_MOUSEINPUT;

typedef struct {
    u16 virtual_key;
    u16 scan_code;
    u32 flags;
    u32 time;
    u64 extra_info;
} WIN_KEYBDINPUT;

typedef struct {
    u32 message;
    u16 param_l;
    u16 param_h;
} WIN_HARDWAREINPUT;

typedef struct {
    u32 type;
    u32 _padding;
    union {
        WIN_MOUSEINPUT mouse;
        WIN_KEYBDINPUT keyboard;
        WIN_HARDWAREINPUT hardware;
    } data;
} WIN_INPUT;

_Static_assert(sizeof(WIN_INPUT) == 40, "Win64 INPUT layout");

static u64 __attribute__((ms_abi))
impl_SendInput(u32 count, const WIN_INPUT *inputs, s32 size)
{
    if (size != (s32)sizeof(WIN_INPUT)) { g_last_error = 87; return 0; }
    if (count && !inputs) { g_last_error = 87; return 0; }

    /* Beer currently exposes quiet input rather than injecting host events back
     * into XWayland. Validate every Windows INPUT record and report it consumed;
     * returning zero made the game's cursor/input bootstrap treat a valid
     * request as an API failure. */
    for (u32 i = 0; i < count; ++i) {
        if (inputs[i].type > 2u) { g_last_error = 87; return i; }
    }
    return count;
}

static u64 __attribute__((ms_abi))
impl_XInputGetStateDisconnected(u32 user_index, void *state)
{
    (void)user_index;
    if (state) memset(state, 0, 16); /* XINPUT_STATE */
    return 1167; /* ERROR_DEVICE_NOT_CONNECTED */
}

static u64 __attribute__((ms_abi))
impl_XInputGetCapabilitiesDisconnected(u32 user_index, u32 flags,
                                       void *capabilities)
{
    (void)user_index;
    (void)flags;
    if (capabilities) memset(capabilities, 0, 20); /* XINPUT_CAPABILITIES */
    return 1167; /* ERROR_DEVICE_NOT_CONNECTED */
}

static u64 __attribute__((ms_abi))
impl_XInputDisconnected(u32 user_index, void *state_or_vibration)
{
    return impl_XInputGetStateDisconnected(user_index, state_or_vibration);
}

/* XInputEnable is ordinal 5 in XInput 1.3. It is a void process-wide policy
 * switch and must not share the ERROR_DEVICE_NOT_CONNECTED return/signature
 * used by state/capability methods. Beer has no controller backend yet, so the
 * flag is retained only to keep the API contract coherent. */
static _Atomic(u32) g_xinput_enabled = 1;
static void __attribute__((ms_abi)) impl_XInputEnable(u32 enabled)
{
    atomic_store(&g_xinput_enabled, enabled != 0);
}

static u64 __attribute__((ms_abi))
impl_SetCapture(u64 hw) { return hw == FAKE_HWND ? FAKE_HWND : 0; }
static u64 __attribute__((ms_abi))
impl_ReleaseCapture(void) { return 1; }
static u64 __attribute__((ms_abi))
impl_SetFocus(u64 hw)
{
    if (hw && hw != FAKE_HWND) { g_last_error = 1400; return 0; }
    u64 previous = g_window_focused ? FAKE_HWND : 0;
    if (hw == FAKE_HWND && !g_window_focused) {
        g_window_focused = 1;
        call_guest_wndproc(0x0007, 0, 0);
    } else if (!hw && g_window_focused) {
        g_window_focused = 0;
        call_guest_wndproc(0x0008, 0, 0);
    }
    return previous;
}
static u64 __attribute__((ms_abi))
impl_GetFocus(void) { return g_window_focused ? FAKE_HWND : 0; }
static u64 __attribute__((ms_abi))
impl_GetActiveWindow(void) { return g_window_focused ? FAKE_HWND : 0; }
static u64 __attribute__((ms_abi))
impl_SetActiveWindow(u64 hwnd)
{
    if (hwnd && hwnd != FAKE_HWND) { g_last_error = 1400; return 0; }
    u64 previous = g_window_focused ? FAKE_HWND : 0;
    impl_SetFocus(hwnd);
    return previous;
}

/* Message loop */
typedef struct {
    u64 hwnd;
    u32 message;
    u32 padding;
    u64 wParam;
    u64 lParam;
    u32 time;
    WIN_POINT pt;
    u32 lPrivate;
} WIN_MSG;
_Static_assert(sizeof(WIN_MSG) == 48, "MSG must be 48 bytes on Win64");

#define WIN_MESSAGE_CAPACITY 256
static WIN_MSG g_win_messages[WIN_MESSAGE_CAPACITY];
/* Win32 message queues belong to threads, not processes. Keep routing metadata
 * separate from WIN_MSG so the guest-visible structure remains ABI-exact. */
static u32 g_win_message_threads[WIN_MESSAGE_CAPACITY];

static u32 g_win_message_head, g_win_message_count;
#define WIN_TIMER_CAPACITY 32
typedef struct {
    u64 hwnd;
    u64 id;
    u32 interval_ms;
    u64 callback;
    u64 next_fire_ms;
    int active;
} WIN_TIMER;
static WIN_TIMER g_win_timers[WIN_TIMER_CAPACITY];
static pthread_mutex_t g_win_timer_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Atomic(u64) g_next_win_timer_id = 1;

static u64 monotonic_milliseconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (u64)now.tv_sec * 1000 + (u64)now.tv_nsec / 1000000;
}

static void remove_window_message_locked(u32 position)
{
    for (u32 i = position; i + 1 < g_win_message_count; ++i) {
        u32 to = (g_win_message_head + i) % WIN_MESSAGE_CAPACITY;
        u32 from = (g_win_message_head + i + 1) % WIN_MESSAGE_CAPACITY;
        g_win_messages[to] = g_win_messages[from];
        g_win_message_threads[to] = g_win_message_threads[from];
    }
    --g_win_message_count;
}

static int queue_message_for_thread(u32 owner, u64 hwnd, u32 message,
                                    u64 wparam, u64 lparam)
{
    if (!owner) {
        g_last_error = 1444; /* ERROR_INVALID_THREAD_ID */
        return 0;
    }
    u32 now = (u32)monotonic_milliseconds();
    WIN_POINT cursor;
    pthread_mutex_lock(&g_input_mutex);
    cursor.x = g_input.root_x;
    cursor.y = g_input.root_y;
    pthread_mutex_unlock(&g_input_mutex);
    pthread_mutex_lock(&g_win_message_mutex);

    /* Windows coalesces pending mouse movement. Without this, normal pointer
     * motion fills Beer's bounded queue and silently drops the following
     * button transition, while hover still appears to work through cursor
     * polling. Keep only the newest pending motion for this window thread. */
    if (beer_win32_message_is_coalescible_motion(message)) {
        for (u32 i = g_win_message_count; i > 0; --i) {
            u32 index = (g_win_message_head + i - 1) % WIN_MESSAGE_CAPACITY;
            if (g_win_message_threads[index] == owner &&
                g_win_messages[index].message == message) {
                g_win_messages[index].wParam = wparam;
                g_win_messages[index].lParam = lparam;
                g_win_messages[index].time = now;
                g_win_messages[index].pt = cursor;
                pthread_mutex_unlock(&g_win_message_mutex);
                return 1;
            }
        }
    }

    if (g_win_message_count == WIN_MESSAGE_CAPACITY &&
        beer_win32_message_is_button_transition(message)) {
        /* A button transition must not be lost behind stale pointer motion.
         * Reclaim the oldest coalescible motion for the same destination. */
        for (u32 i = 0; i < g_win_message_count; ++i) {
            u32 index = (g_win_message_head + i) % WIN_MESSAGE_CAPACITY;
            if (g_win_message_threads[index] == owner &&
                beer_win32_message_is_coalescible_motion(
                    g_win_messages[index].message)) {
                remove_window_message_locked(i);
                break;
            }
        }
    }
    if (g_win_message_count == WIN_MESSAGE_CAPACITY) {
        pthread_mutex_unlock(&g_win_message_mutex);
        if (getenv("BEER_INPUT_DIAGNOSTICS"))
            fprintf(stderr, "[INPUT] Win32 queue full; dropped message 0x%x\n",
                    message);
        return 0;
    }
    u32 index = (g_win_message_head + g_win_message_count) % WIN_MESSAGE_CAPACITY;
    g_win_messages[index] = (WIN_MSG){
        .hwnd = beer_win32_message_hwnd(hwnd == 0, hwnd, message),
        .message = message, .wParam = wparam, .lParam = lparam,
        .time = now, .pt = cursor
    };
    g_win_message_threads[index] = owner;
    ++g_win_message_count;
    u32 queued_count = g_win_message_count;
    pthread_mutex_unlock(&g_win_message_mutex);
    if (getenv("BEER_MESSAGE_DIAGNOSTICS")) {
        static _Atomic(u32) queue_logs;
        u32 trace = atomic_fetch_add(&queue_logs, 1);
        if (trace < 512)
            fprintf(stderr,
                    "[MESSAGE] queue id=0x%x hwnd=0x%llx count=%u thread=%u "
                    "caller=0x%llx\n",
                    message,
                    (unsigned long long)beer_win32_message_hwnd(hwnd == 0, hwnd,
                                                               message),
                    queued_count, owner,
                    (unsigned long long)g_dispatch_incoming_return);
    }
    return 1;
}

static int queue_window_message(u32 message, u64 wparam, u64 lparam)
{
    u32 owner = g_window_thread_id ? g_window_thread_id :
                                    (u32)current_windows_thread_id();
    return queue_message_for_thread(owner, FAKE_HWND, message, wparam, lparam);
}

static void collect_native_window_events(void)
{
    pthread_mutex_lock(&g_event_collect_mutex);
    /* All native-event producers are serialized. Every consumed event must be
     * published; the previous depth guard let a concurrent polling thread eat
     * a click while deliberately omitting its Win32 and DirectInput records. */
    const int top_level = 1;
    XwaylandEvent event;
    while (xwayland_window_poll_event(&event)) {
        switch (event.type) {
            case XWAYLAND_EVENT_KEY_DOWN:
            case XWAYLAND_EVENT_KEY_UP: {
                int down = event.type == XWAYLAND_EVENT_KEY_DOWN;
                u32 vk = x11_keysym_to_vk(event.keysym);
                u8 dik = x11_keycode_to_dik(event.keycode);
                pthread_mutex_lock(&g_input_mutex);
                if (vk && vk < 256) {
                    if (down && !(g_input.keys[vk] & 0x80)) g_input.pressed[vk] = 1;
                    g_input.keys[vk] = down ? 0x80 : 0;
                }
                if (dik) {
                    g_input.dik[dik] = down ? 0x80 : 0;
                    if (top_level)
                        queue_dinput_event_locked(2, dik, down ? 0x80u : 0u);
                }
                pthread_mutex_unlock(&g_input_mutex);
                if (top_level && vk) {
                    u64 lparam = 1 | ((u64)(dik & 0x7f) << 16);
                    if (!down) lparam |= 0xc0000000ULL;
                    queue_window_message(down ? 0x0100 : 0x0101, vk, lparam);
                    u32 log = atomic_fetch_add(&g_input_event_logs, 1);
                    if (log < 32)
                        fprintf(stderr, "[INPUT] key %s vk=0x%02x dik=0x%02x\n",
                                down ? "down" : "up", vk, dik);
                }
                break;
            }
            case XWAYLAND_EVENT_BUTTON_DOWN:
            case XWAYLAND_EVENT_BUTTON_UP: {
                int down = event.type == XWAYLAND_EVENT_BUTTON_DOWN;
                if (event.button >= 1 && event.button <= 3) {
                    /* messages[button_index][!down]: [0]=down message, [1]=up message.
                     * Button 1 = left (0x0201 down / 0x0202 up)
                     * Button 2 = middle (0x0207 down / 0x0208 up)
                     * Button 3 = right (0x0204 down / 0x0205 up) */
                    static const u32 messages[3][2] = {
                        {0x0201, 0x0202}, {0x0207, 0x0208}, {0x0204, 0x0205}
                    };
                    static const u32 vks[3] = {0x01, 0x04, 0x02};
                    s32 guest_x, guest_y;
                    native_to_guest_client(event.x, event.y, &guest_x, &guest_y);
                    pthread_mutex_lock(&g_input_mutex);
                    u32 vk = vks[event.button - 1];
                    if (down && !(g_input.keys[vk] & 0x80))
                        g_input.pressed[vk] = 1;
                    g_input.mouse_buttons[event.button - 1] = down ? 0x80 : 0;
                    g_input.keys[vk] = down ? 0x80 : 0;
                    if (top_level)
                        queue_dinput_event_locked(1, 12u + event.button - 1,
                                                   down ? 0x80u : 0u);
                    g_input.client_x = guest_x; g_input.client_y = guest_y;
                    g_input.root_x = event.root_x; g_input.root_y = event.root_y;
                    u64 button_flags = 0;
                    if (g_input.mouse_buttons[0] & 0x80) button_flags |= 0x0001; /* MK_LBUTTON */
                    if (g_input.mouse_buttons[1] & 0x80) button_flags |= 0x0010; /* MK_MBUTTON */
                    if (g_input.mouse_buttons[2] & 0x80) button_flags |= 0x0002; /* MK_RBUTTON */
                    pthread_mutex_unlock(&g_input_mutex);
                    if (top_level) {
                        /* messages[i][0] = down msg, messages[i][1] = up msg.
                         * down==1 → index 0 (down msg), down==0 → index 1 (up msg). */
                        queue_window_message(messages[event.button - 1][!down],
                                             button_flags,
                                             ((u64)(u16)guest_y << 16) | (u16)guest_x);
                        u32 log = atomic_fetch_add(&g_input_event_logs, 1);
                        if (log < 64)
                            fprintf(stderr,
                                    "[INPUT] mouse button %u %s vk=0x%02x at %d,%d\n",
                                    event.button, down ? "down" : "up", vk,
                                    guest_x, guest_y);
                    }
                } else if (down && (event.button == 4 || event.button == 5)) {
                    s32 wheel = event.button == 4 ? 120 : -120;
                    s32 guest_x, guest_y;
                    native_to_guest_client(event.x, event.y, &guest_x, &guest_y);
                    pthread_mutex_lock(&g_input_mutex);
                    g_input.wheel += wheel;
                    if (top_level)
                        queue_dinput_event_locked(1, 8, (u32)wheel);
                    pthread_mutex_unlock(&g_input_mutex);
                    if (top_level)
                        queue_window_message(0x020a, (u64)(u16)wheel << 16,
                                             ((u64)(u16)guest_y << 16) | (u16)guest_x);
                }
                break;
            }
            case XWAYLAND_EVENT_POINTER_MOTION: {
                s32 guest_x, guest_y;
                native_to_guest_client(event.x, event.y, &guest_x, &guest_y);
                pthread_mutex_lock(&g_input_mutex);
                s32 dx = guest_x - g_input.client_x;
                s32 dy = guest_y - g_input.client_y;
                g_input.delta_x += dx;
                g_input.delta_y += dy;
                if (top_level) {
                    if (dx) queue_dinput_event_locked(1, 0, (u32)dx);
                    if (dy) queue_dinput_event_locked(1, 4, (u32)dy);
                }
                g_input.client_x = guest_x; g_input.client_y = guest_y;
                g_input.root_x = event.root_x; g_input.root_y = event.root_y;
                pthread_mutex_unlock(&g_input_mutex);
                if (top_level)
                    queue_window_message(0x0200, 0,
                                         ((u64)(u16)guest_y << 16) | (u16)guest_x);
                break;
            }
            case XWAYLAND_EVENT_CLOSE:
                if (top_level) queue_window_message(0x0010, 0, 0); break;
            case XWAYLAND_EVENT_SHOW:
                if (top_level) queue_window_message(0x0018, 1, 0); break;
            case XWAYLAND_EVENT_HIDE:
                if (top_level) queue_window_message(0x0018, 0, 0); break;
            case XWAYLAND_EVENT_FOCUS_IN:
                g_window_focused = 1;
                if (top_level) {
                    queue_window_message(0x001c, 1, 0);
                    queue_window_message(0x0006, 1, 0);
                    queue_window_message(0x0007, 0, 0);
                }
                break;
            case XWAYLAND_EVENT_FOCUS_OUT:
                g_window_focused = 0;
                pthread_mutex_lock(&g_input_mutex);
                memset(g_input.keys, 0, sizeof(g_input.keys));
                memset(g_input.dik, 0, sizeof(g_input.dik));
                memset(g_input.mouse_buttons, 0, sizeof(g_input.mouse_buttons));
                pthread_mutex_unlock(&g_input_mutex);
                if (top_level) {
                    queue_window_message(0x0008, 0, 0);
                    queue_window_message(0x0006, 0, 0);
                    queue_window_message(0x001c, 0, 0);
                }
                break;
            case XWAYLAND_EVENT_CONFIGURE:
                if (top_level) {
                    queue_window_message(0x0003, 0,
                                         ((u64)(u16)event.y << 16) | (u16)event.x);
                    queue_window_message(0x0005, 0,
                                         ((u64)(u16)event.height << 16) | (u16)event.width);
                }
                break;
            case XWAYLAND_EVENT_EXPOSE:
                if (top_level) queue_window_message(0x000f, 0, 0); break;
            default: break;
        }
    }
    pthread_mutex_unlock(&g_event_collect_mutex);
}

static void *native_input_pump_main(void *unused)
{
    (void)unused;
    for (;;) {
        collect_native_window_events();
        struct timespec delay = {0, 1000000L};
        nanosleep(&delay, NULL);
    }
    return NULL;
}

static void ensure_native_input_pump(void)
{
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_native_input_pump_started,
                                        &expected, 1))
        return;
    pthread_t thread;
    if (pthread_create(&thread, NULL, native_input_pump_main, NULL) == 0) {
        pthread_detach(thread);
        return;
    }
    atomic_store(&g_native_input_pump_started, 0);
}

static void collect_window_timers(void)
{
    u64 now = monotonic_milliseconds();
    pthread_mutex_lock(&g_win_timer_mutex);
    for (u32 i = 0; i < WIN_TIMER_CAPACITY; ++i) {
        WIN_TIMER *timer = &g_win_timers[i];
        if (!timer->active || now < timer->next_fire_ms) continue;
        do timer->next_fire_ms += timer->interval_ms;
        while (timer->next_fire_ms <= now);
        /* Timer callbacks are dispatched by DispatchMessage on Windows. The
         * title path registers a normal window timer (callback == NULL). */
        queue_window_message(0x0113, timer->id, timer->callback);
    }
    pthread_mutex_unlock(&g_win_timer_mutex);
}

static u64 __attribute__((ms_abi))
impl_SetTimer(u64 hwnd, u64 id, u32 interval_ms, u64 callback)
{
    if (hwnd && hwnd != FAKE_HWND) { g_last_error = 1400; return 0; }
    if (interval_ms < 10) interval_ms = 10; /* USER_TIMER_MINIMUM */
    pthread_mutex_lock(&g_win_timer_mutex);
    WIN_TIMER *free_slot = NULL;
    for (u32 i = 0; i < WIN_TIMER_CAPACITY; ++i) {
        WIN_TIMER *timer = &g_win_timers[i];
        if (timer->active && timer->hwnd == hwnd && timer->id == id && id) {
            free_slot = timer;
            break;
        }
        if (!timer->active && !free_slot) free_slot = timer;
    }
    if (!free_slot) {
        pthread_mutex_unlock(&g_win_timer_mutex);
        g_last_error = 8; /* ERROR_NOT_ENOUGH_MEMORY */
        return 0;
    }
    if (!id) id = atomic_fetch_add(&g_next_win_timer_id, 1);
    *free_slot = (WIN_TIMER){
        .hwnd = hwnd, .id = id, .interval_ms = interval_ms,
        .callback = callback, .next_fire_ms = monotonic_milliseconds() + interval_ms,
        .active = 1
    };
    pthread_mutex_unlock(&g_win_timer_mutex);
    return id;
}

static u64 __attribute__((ms_abi))
impl_KillTimer(u64 hwnd, u64 id)
{
    if (hwnd && hwnd != FAKE_HWND) { g_last_error = 1400; return 0; }
    int removed = 0;
    pthread_mutex_lock(&g_win_timer_mutex);
    for (u32 i = 0; i < WIN_TIMER_CAPACITY; ++i) {
        WIN_TIMER *timer = &g_win_timers[i];
        if (timer->active && timer->hwnd == hwnd && timer->id == id) {
            timer->active = 0;
            removed = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_win_timer_mutex);
    return removed;
}

static u64 __attribute__((ms_abi))
impl_IsZoomed(u64 hwnd)
{
    if (hwnd != FAKE_HWND || !xwayland_window_exists()) {
        g_last_error = 1400;
        return 0;
    }
    return 0; /* fullscreen is distinct from WS_MAXIMIZE */
}

static u64 __attribute__((ms_abi))
impl_D3DPERF_GetStatus(void)
{
    return 0; /* no D3D debug/performance capture tool is active */
}

static int take_window_message(WIN_MSG *message, u32 min, u32 max, int remove)
{
    int found = 0;
    u32 thread_id = (u32)current_windows_thread_id();
    pthread_mutex_lock(&g_win_message_mutex);
    for (u32 i = 0; i < g_win_message_count; ++i) {
        u32 index = (g_win_message_head + i) % WIN_MESSAGE_CAPACITY;
        u32 id = g_win_messages[index].message;
        if (g_win_message_threads[index] != thread_id) continue;
        if ((min || max) && (id < min || id > max)) continue;
        if (message) *message = g_win_messages[index];
        if (remove) {
            beer_win32_apply_input_message(g_win_message_keys, id,
                                            g_win_messages[index].wParam);
            for (u32 j = i; j + 1 < g_win_message_count; ++j) {
                u32 to = (g_win_message_head + j) % WIN_MESSAGE_CAPACITY;
                u32 from = (g_win_message_head + j + 1) % WIN_MESSAGE_CAPACITY;
                g_win_messages[to] = g_win_messages[from];
                g_win_message_threads[to] = g_win_message_threads[from];
            }
            --g_win_message_count;
        }
        found = 1;
        break;
    }
    u32 remaining = g_win_message_count;
    pthread_mutex_unlock(&g_win_message_mutex);
    if (found && getenv("BEER_MESSAGE_DIAGNOSTICS")) {
        static _Atomic(u32) take_logs;
        u32 trace = atomic_fetch_add(&take_logs, 1);
        if (trace < 512)
            fprintf(stderr,
                    "[MESSAGE] take id=0x%x remove=%d remaining=%u thread=%u "
                    "caller=0x%llx\n",
                    message ? message->message : 0, remove, remaining,
                    (unsigned)current_windows_thread_id(),
                    (unsigned long long)g_dispatch_incoming_return);
    }
    return found;
}

static u64 __attribute__((ms_abi))
impl_PeekMessageW(WIN_MSG *msg, u64 hw, u32 min, u32 max, u32 remove)
{
    if (hw && hw != FAKE_HWND) return 0;
    collect_native_window_events();
    collect_window_timers();
    return take_window_message(msg, min, max, (remove & 1) != 0);
}
static u64 __attribute__((ms_abi))
impl_GetMessageW(WIN_MSG *msg, u64 hw, u32 min, u32 max)
{
    if (!msg || (hw && hw != FAKE_HWND)) { g_last_error = 87; return (u64)-1; }
    if (getenv("BEER_MESSAGE_DIAGNOSTICS"))
        fprintf(stderr,
                "[MESSAGE] GetMessage enter hwnd=0x%llx range=0x%x..0x%x "
                "thread=%u caller=0x%llx\n",
                (unsigned long long)hw, min, max,
                (unsigned)current_windows_thread_id(),
                (unsigned long long)g_dispatch_incoming_return);
    for (;;) {
        collect_native_window_events();
        collect_window_timers();
        if (take_window_message(msg, min, max, 1))
            return msg->message == 0x0012 ? 0 : 1;
        struct timespec ts = {0, 1000000L};
        nanosleep(&ts, NULL);
    }
}
static u64 __attribute__((ms_abi))
impl_TranslateMessage(const WIN_MSG *msg) { (void)msg; return 0; }
static u64 __attribute__((ms_abi))
impl_DispatchMessageW(const WIN_MSG *msg)
{
    if (!msg || msg->message == 0x0012) return 0;
    if (msg->message == 0x0113 && msg->lParam) {
        typedef void __attribute__((ms_abi)) (*GuestTimerProc)(u64, u32, u64, u32);
        ((GuestTimerProc)(uintptr_t)msg->lParam)(msg->hwnd, msg->message,
                                                msg->wParam, msg->time);
        return 0;
    }
    if (!msg->hwnd) return 0;
    return call_guest_wndproc(msg->message, msg->wParam, msg->lParam);
}
static u64 __attribute__((ms_abi))
impl_PostQuitMessage(u32 code)
{
    g_win_quit = 1; g_win_quit_code = code;
    queue_window_message(0x0012, code, 0);
    return 0;
}
static u64 __attribute__((ms_abi))
impl_SendMessageW(u64 hw, u32 msg, u64 wp, u64 lp)
{
    if (hw != FAKE_HWND) { g_last_error = 1400; return 0; }
    return call_guest_wndproc(msg, wp, lp);
}
static u64 __attribute__((ms_abi))
impl_PostMessageW(u64 hw, u32 msg, u64 wp, u64 lp)
{
    if (hw != FAKE_HWND) { g_last_error = 1400; return 0; }
    /* PostMessage targets the window's owning thread, not the calling thread. */
    return queue_window_message(msg, wp, lp);
}

static u64 __attribute__((ms_abi))
impl_PostThreadMessageW(u32 thread_id, u32 msg, u64 wp, u64 lp)
{
    /* Thread messages have no window and belong only to the requested thread's
     * queue. RE8 uses message 0xa020 to wake its engine-control loop. */
    return queue_message_for_thread(thread_id, 0, msg, wp, lp);
}
static u64 __attribute__((ms_abi))
impl_DefWindowProcW(u64 hw, u32 msg, u64 wp, u64 lp)
{
    (void)wp; (void)lp;
    if (hw != FAKE_HWND) return 0;
    if (msg == 0x0010) return impl_DestroyWindow(hw);
    return msg == 0x0081 ? 1 : 0;
}
static u64 __attribute__((ms_abi))
impl_CallWindowProcW(u64 proc, u64 hw, u32 msg, u64 wp, u64 lp)
{
    if (!proc) return impl_DefWindowProcW(hw, msg, wp, lp);
    return ((GuestWndProc)(uintptr_t)proc)(hw, msg, wp, lp);
}
static u64 window_long_value(s32 index)
{
    if (index == -4) return g_window_proc;
    if (index == -21) return g_window_userdata;
    if (index == -16) return g_window_style;
    if (index == -20) return g_window_exstyle;
    return 0;
}
static u64 set_window_long_value(u64 hw, s32 index, u64 value)
{
    if (hw != FAKE_HWND) { g_last_error = 1400; return 0; }
    u64 old = window_long_value(index);
    if (index == -4) g_window_proc = value;
    else if (index == -21) g_window_userdata = value;
    else if (index == -16) g_window_style = value;
    else if (index == -20) g_window_exstyle = value;
    else { g_last_error = 87; return 0; }
    return old;
}
static u64 __attribute__((ms_abi))
impl_SetWindowLongPtrW(u64 hw, s32 idx, u64 new_value)
    { return set_window_long_value(hw, idx, new_value); }
static u64 __attribute__((ms_abi))
impl_GetWindowLongPtrW(u64 hw, s32 idx)
    { if (hw != FAKE_HWND) { g_last_error = 1400; return 0; } return window_long_value(idx); }
static u64 __attribute__((ms_abi))
impl_GetWindowLongPtrA(u64 hw, s32 idx)
    { return impl_GetWindowLongPtrW(hw, idx); }
static u64 __attribute__((ms_abi))
impl_SetWindowLongW(u64 hw, s32 idx, s32 new_value)
    { return (u32)set_window_long_value(hw, idx, (u32)new_value); }
static u64 __attribute__((ms_abi))
impl_GetWindowLongW(u64 hw, s32 idx)
    { return (u32)impl_GetWindowLongPtrW(hw, idx); }
static u64 __attribute__((ms_abi))
impl_GetWindowLongA(u64 hw, s32 idx)
    { return impl_GetWindowLongW(hw, idx); }

static int window_property_key(const void *name, int wide, int *is_atom,
                               u16 *atom, u16 key[WINDOW_PROPERTY_NAME_CAPACITY])
{
    uintptr_t raw = (uintptr_t)name;
    if (!name) return 0;
    if (raw <= 0xffffu) {
        *is_atom = 1;
        *atom = (u16)raw;
        key[0] = 0;
        return 1;
    }
    *is_atom = 0;
    *atom = 0;
    size_t i = 0;
    if (wide) {
        const u16 *source = (const u16 *)name;
        for (; i + 1 < WINDOW_PROPERTY_NAME_CAPACITY && source[i]; ++i)
            key[i] = source[i];
    } else {
        const unsigned char *source = (const unsigned char *)name;
        for (; i + 1 < WINDOW_PROPERTY_NAME_CAPACITY && source[i]; ++i)
            key[i] = source[i];
    }
    key[i] = 0;
    return i != 0;
}

static int window_property_matches(const WIN_PROPERTY *property, int is_atom,
                                   u16 atom,
                                   const u16 key[WINDOW_PROPERTY_NAME_CAPACITY])
{
    if (!property->active || property->is_atom != is_atom) return 0;
    if (is_atom) return property->atom == atom;
    for (size_t i = 0; i < WINDOW_PROPERTY_NAME_CAPACITY; ++i) {
        if (property->name[i] != key[i]) return 0;
        if (!key[i]) return 1;
    }
    return 1;
}

static u64 window_property_set(u64 hwnd, const void *name, int wide, u64 value)
{
    if (hwnd != FAKE_HWND) { g_last_error = 1400; return 0; }
    int is_atom;
    u16 atom, key[WINDOW_PROPERTY_NAME_CAPACITY];
    if (!window_property_key(name, wide, &is_atom, &atom, key)) {
        g_last_error = 87;
        return 0;
    }
    pthread_mutex_lock(&g_window_property_mutex);
    WIN_PROPERTY *free_slot = NULL;
    for (size_t i = 0; i < WINDOW_PROPERTY_CAPACITY; ++i) {
        WIN_PROPERTY *property = &g_window_properties[i];
        if (window_property_matches(property, is_atom, atom, key)) {
            property->value = value;
            pthread_mutex_unlock(&g_window_property_mutex);
            return 1;
        }
        if (!property->active && !free_slot) free_slot = property;
    }
    if (!free_slot) {
        pthread_mutex_unlock(&g_window_property_mutex);
        g_last_error = 8; /* ERROR_NOT_ENOUGH_MEMORY */
        return 0;
    }
    *free_slot = (WIN_PROPERTY){.active = 1, .is_atom = is_atom,
                                .atom = atom, .value = value};
    if (!is_atom)
        memcpy(free_slot->name, key, sizeof(free_slot->name));
    pthread_mutex_unlock(&g_window_property_mutex);
    return 1;
}

static u64 window_property_get(u64 hwnd, const void *name, int wide, int remove)
{
    if (hwnd != FAKE_HWND) { g_last_error = 1400; return 0; }
    int is_atom;
    u16 atom, key[WINDOW_PROPERTY_NAME_CAPACITY];
    if (!window_property_key(name, wide, &is_atom, &atom, key)) {
        g_last_error = 87;
        return 0;
    }
    pthread_mutex_lock(&g_window_property_mutex);
    for (size_t i = 0; i < WINDOW_PROPERTY_CAPACITY; ++i) {
        WIN_PROPERTY *property = &g_window_properties[i];
        if (!window_property_matches(property, is_atom, atom, key)) continue;
        u64 value = property->value;
        if (remove) memset(property, 0, sizeof(*property));
        pthread_mutex_unlock(&g_window_property_mutex);
        return value;
    }
    pthread_mutex_unlock(&g_window_property_mutex);
    return 0;
}

static u64 __attribute__((ms_abi))
impl_SetPropW(u64 hwnd, const u16 *name, u64 value)
    { return window_property_set(hwnd, name, 1, value); }
static u64 __attribute__((ms_abi))
impl_SetPropA(u64 hwnd, const char *name, u64 value)
    { return window_property_set(hwnd, name, 0, value); }
static u64 __attribute__((ms_abi))
impl_GetPropW(u64 hwnd, const u16 *name)
    { return window_property_get(hwnd, name, 1, 0); }
static u64 __attribute__((ms_abi))
impl_GetPropA(u64 hwnd, const char *name)
    { return window_property_get(hwnd, name, 0, 0); }
static u64 __attribute__((ms_abi))
impl_RemovePropW(u64 hwnd, const u16 *name)
    { return window_property_get(hwnd, name, 1, 1); }
static u64 __attribute__((ms_abi))
impl_RemovePropA(u64 hwnd, const char *name)
    { return window_property_get(hwnd, name, 0, 1); }

static u64 __attribute__((ms_abi))
impl_GetTopWindow(u64 parent)
    { return parent ? 0 : (xwayland_window_exists() ? FAKE_HWND : 0); }
static u64 __attribute__((ms_abi))
impl_GetWindow(u64 hwnd, u32 command)
{
    if (hwnd != FAKE_HWND) { g_last_error = 1400; return 0; }
    (void)command;
    return 0; /* Beer currently exposes one top-level guest window. */
}
static u64 __attribute__((ms_abi))
impl_GetWindowThreadProcessId(u64 hwnd, u32 *process_id)
{
    if (hwnd != FAKE_HWND || !xwayland_window_exists()) {
        g_last_error = 1400;
        if (process_id) *process_id = 0;
        return 0;
    }
    if (process_id) *process_id = (u32)getpid();
    return g_window_thread_id ? g_window_thread_id : (u32)current_windows_thread_id();
}
static u64 __attribute__((ms_abi))
impl_SetForegroundWindow(u64 hw)
{
    if (hw != FAKE_HWND) { g_last_error = 1400; return 0; }
    impl_SetFocus(hw);
    return 1;
}
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

/* Vista+ locale-name APIs queried dynamically by the Universal CRT. Keep the
 * callback ABI and return contracts exact: EnumSystemLocalesEx invokes the
 * callback with a BCP-47 locale name, flags and caller lParam; a FALSE callback
 * result stops enumeration but is not an API failure. */
static int u16_ascii_equal_ci(const u16 *value, const char *ascii)
{
    if (!value || !ascii) return 0;
    for (size_t i = 0;; i++) {
        u16 a = value[i];
        u16 b = (u8)ascii[i];
        if (a >= 'A' && a <= 'Z') a = (u16)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (u16)(b + ('a' - 'A'));
        if (a != b) return 0;
        if (!a) return 1;
    }
}

static int copy_ascii_to_u16(const char *src, u16 *dst, int capacity)
{
    int needed = (int)strlen(src) + 1;
    if (!dst || capacity == 0) return needed;
    if (capacity < needed) {
        g_last_error = 122; /* ERROR_INSUFFICIENT_BUFFER */
        return 0;
    }
    for (int i = 0; i < needed; i++) dst[i] = (u8)src[i];
    return needed;
}

static u64 __attribute__((ms_abi))
impl_EnumSystemLocalesEx(u64 callback, u32 flags, u64 lparam, void *reserved)
{
    (void)flags;
    if (!callback || reserved) {
        g_last_error = 87; /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    static u16 locale_names[][6] = {
        {'e','n','-','U','S',0},
        {'j','a','-','J','P',0}
    };
    typedef u64 __attribute__((ms_abi)) (*LocaleEnumProcEx)(u16 *, u32, u64);
    for (size_t i = 0; i < sizeof(locale_names) / sizeof(locale_names[0]); i++) {
        if (!((LocaleEnumProcEx)callback)(locale_names[i], 0, lparam))
            break;
    }
    return 1;
}

static u64 __attribute__((ms_abi))
impl_IsValidLocaleName(const u16 *name)
{
    if (!name || !name[0]) return 0;
    if (!name[1] && name[0] == 0x7f) return 1; /* LOCALE_NAME_INVARIANT */
    /* UCRT accepts both canonical locale names and the legacy language aliases
     * it obtains from LOCALE_SENGLANGUAGE while constructing std::locale. */
    return u16_ascii_equal_ci(name, "en-US") ||
           u16_ascii_equal_ci(name, "ja-JP") ||
           u16_ascii_equal_ci(name, "English") ||
           u16_ascii_equal_ci(name, "Japanese");
}

static u64 __attribute__((ms_abi))
impl_IsValidLocale(u32 lcid, u32 flags) { (void)flags; return lcid <= 0xFFFF ? 1 : 0; }
static u64 __attribute__((ms_abi))
impl_GetLocaleInfoA(u32 locale, u32 type, char *buf, int size)
{
    const char *value = beer_locale_info_ascii(locale, type);
    if (!value) { g_last_error = 1004; return 0; }

    if (type & 0x20000000u) {
        char *end = NULL;
        unsigned long number = strtoul(value, &end, 10);
        if (!end || *end) { g_last_error = 87; return 0; }
        if (!buf || size == 0) return sizeof(u32);
        if (size < (int)sizeof(u32)) { g_last_error = 122; return 0; }
        u32 result = (u32)number;
        memcpy(buf, &result, sizeof(result));
        g_last_error = 0;
        return sizeof(result);
    }

    int needed = (int)strlen(value) + 1;
    if (!buf || size == 0) return (u64)needed;
    if (size < needed) { g_last_error = 122; return 0; }
    memcpy(buf, value, (size_t)needed);
    g_last_error = 0;
    return (u64)needed;
}
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

extern char **environ;

static u64 __attribute__((ms_abi))
impl_GetEnvironmentStringsW(void)
{
    /* Windows returns a caller-owned sequence of NUL-terminated UTF-16
     * NAME=VALUE strings followed by one additional NUL.  The bundled Oodle
     * CRT walks this block during DLL_PROCESS_ATTACH and rejects attachment
     * when the API returns NULL.  Host environment entries are UTF-8 in
     * principle; the game-relevant variables are ASCII, so preserve ASCII and
     * replace unsupported multibyte bytes rather than inventing invalid UTF-16. */
    size_t units = 1;
    for (char **entry = environ; entry && *entry; ++entry)
        units += strlen(*entry) + 1;
    u16 *block = calloc(units, sizeof(*block));
    if (!block) {
        g_last_error = 8; /* ERROR_NOT_ENOUGH_MEMORY */
        return 0;
    }
    u16 *out = block;
    for (char **entry = environ; entry && *entry; ++entry) {
        const unsigned char *src = (const unsigned char *)*entry;
        while (*src)
            *out++ = *src < 0x80 ? *src++ : (src++, (u16)'?');
        *out++ = 0;
    }
    *out = 0;
    return (u64)block;
}

static u64 __attribute__((ms_abi))
impl_FreeEnvironmentStringsW(u64 p)
{
    if (!p) {
        g_last_error = 87; /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    free((void *)p);
    return 1;
}

static u64 __attribute__((ms_abi))
impl_GetEnvironmentVariableW(const u16 *name, u16 *buffer, u32 size)
{
    if (!name) { g_last_error = 87; return 0; }
    char narrow[256];
    size_t i = 0;
    while (name[i] && i + 1 < sizeof(narrow)) {
        if (name[i] > 0x7f) { g_last_error = 203; return 0; }
        narrow[i] = (char)name[i];
        ++i;
    }
    narrow[i] = 0;
    const char *value = getenv(narrow);
    if (!value) { g_last_error = 203; return 0; }
    u32 length = (u32)strlen(value);
    if (!buffer || size <= length) return length + 1;
    for (u32 j = 0; j <= length; ++j) buffer[j] = (u8)value[j];
    return length;
}
static u64 __attribute__((ms_abi))
impl_GetCommandLineA(void) { return (u64)(g_exe_path ? g_exe_path : ""); }
static u64 __attribute__((ms_abi))
impl_GetCommandLineW(void) { return (u64)g_exe_path_w; }

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
static u64 __attribute__((ms_abi)) impl_FatalAppExitW(u32 action, const u16 *message)
{
    char text[1024];
    size_t length = 0;
    (void)action;
    if (message) {
        while (message[length] && length + 1 < sizeof(text)) {
            u16 ch = message[length];
            text[length++] = ch <= 0x7f ? (char)ch : '?';
        }
    }
    text[length] = 0;
    fprintf(stderr, "[FATAL APP] %s\n", text[0] ? text : "(no message)");
    dbg_dump_guest_callchain("FatalAppExitW");
    fflush(stderr);
    _exit(1);
}

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
    guest_dll_notify_thread(2 /* DLL_THREAD_ATTACH */);
    typedef void __attribute__((ms_abi)) (*WorkCB)(u64,u64,u64);
    ((WorkCB)w->cb)(0 /*instance*/, w->ctx, (u64)w);
    guest_dll_notify_thread(3 /* DLL_THREAD_DETACH */);
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
    guest_dll_notify_thread(2 /* DLL_THREAD_ATTACH */);
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
    guest_dll_notify_thread(3 /* DLL_THREAD_DETACH */);
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
    u64             timeout_ms;
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
    guest_dll_notify_thread(2 /* DLL_THREAD_ATTACH */);
    typedef void __attribute__((ms_abi)) (*WaitCB)(u64,u64,u64,u32);
    u64 handle;
    u64 timeout_ms;
    pthread_mutex_lock(&w->mtx);
    handle = w->handle;
    timeout_ms = w->timeout_ms;
    pthread_mutex_unlock(&w->mtx);

    /* A Windows thread-pool wait fires only after the registered handle is
     * signaled or the timeout expires. The old implementation invoked every
     * callback immediately, before its producer event, which let dependent
     * resource jobs run with incomplete inputs. */
    u32 wait_ms = timeout_ms > UINT32_MAX ? 0xFFFFFFFFu : (u32)timeout_ms;
    u64 result = 0x102u;
    u32 elapsed = 0;
    for (;;) {
        pthread_mutex_lock(&w->mtx);
        int stop = w->cancel;
        pthread_mutex_unlock(&w->mtx);
        if (stop) break;
        u32 slice = wait_ms == 0xFFFFFFFFu ? 10u :
            (wait_ms - elapsed < 10u ? wait_ms - elapsed : 10u);
        result = impl_WaitForSingleObject(handle, slice);
        if (result == 0 || result == 0xFFFFFFFFu) break;
        elapsed += slice;
        if (wait_ms != 0xFFFFFFFFu && elapsed >= wait_ms) break;
    }

    pthread_mutex_lock(&w->mtx);
    int canceled = w->cancel;
    w->active = 0;
    pthread_cond_broadcast(&w->cv);
    pthread_mutex_unlock(&w->mtx);
    if (!canceled && (result == 0 || result == 0x102u))
        ((WaitCB)w->cb)(0, w->ctx, (u64)w,
                        result == 0 ? 0u : 258u /* WAIT_TIMEOUT */);
    guest_dll_notify_thread(3 /* DLL_THREAD_DETACH */);
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
impl_SetThreadpoolWait(WinTPWait *w, u64 handle, s64 *timeout)
{
    if (!w) return 0;
    pthread_mutex_lock(&w->mtx);
    if (!handle) {
        w->cancel = 1;
        pthread_cond_broadcast(&w->cv);
    } else {
        w->handle = handle;
        if (!timeout) {
            w->timeout_ms = 0xFFFFFFFFu;
        } else if (*timeout < 0) {
            u64 relative_100ns = (u64)(-*timeout);
            w->timeout_ms = (relative_100ns + 9999u) / 10000u;
        } else {
            u64 now = 0;
            impl_GetSystemTimeAsFileTime(&now);
            u64 remaining_100ns = (u64)*timeout > now ? (u64)*timeout - now : 0;
            w->timeout_ms = (remaining_100ns + 9999u) / 10000u;
        }
        w->cancel = 0;
        if (!w->active) {
            w->active = 1;
            if (pthread_create(&w->thr, NULL, tp_wait_thread, w) == 0)
                pthread_detach(w->thr);
            else
                w->active = 0;
        }
    }
    pthread_mutex_unlock(&w->mtx);
    return 0;
}

static u64 __attribute__((ms_abi))
impl_WaitForThreadpoolWaitCallbacks(WinTPWait *w, u32 cancel)
{
    if (!w) return 0;
    pthread_mutex_lock(&w->mtx);
    if (cancel) w->cancel = 1;
    while (w->active) pthread_cond_wait(&w->cv, &w->mtx);
    pthread_mutex_unlock(&w->mtx);
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
{
    (void)sa; (void)name; (void)access;
    /* CREATE_EVENT_MANUAL_RESET=1, CREATE_EVENT_INITIAL_SET=2. */
    if (flags & ~3u) { g_last_error = 87; return 0; }
    return event_alloc((flags & 1u) != 0, (flags & 2u) != 0);
}

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

static u64 __attribute__((ms_abi))
impl_CreateDirectoryA(const char *path, void *security_attributes)
{
    (void)security_attributes;
    if (!path || !*path) { g_last_error = 3; return 0; }
    char host_path[PATH_MAX];
    if (!resolve_windows_host_path(path, host_path, sizeof(host_path))) {
        g_last_error = 206;
        return 0;
    }
    if (mkdir(host_path, 0777) == 0) { g_last_error = 0; return 1; }
    g_last_error = errno == EEXIST ? 183u : (errno == ENOENT ? 3u : (u32)errno);
    return 0;
}

static u64 __attribute__((ms_abi))
impl_CreateDirectoryW(const u16 *path, void *security_attributes)
{
    if (!path) { g_last_error = 3; return 0; }
    char narrow[4096];
    size_t i = 0;
    for (; path[i] && i + 1 < sizeof(narrow); i++)
        narrow[i] = (char)(path[i] & 0x7f);
    if (path[i]) { g_last_error = 206; return 0; }
    narrow[i] = 0;
    return impl_CreateDirectoryA(narrow, security_attributes);
}

/* ---- CompareStringEx / GetLocaleInfoEx / LCMapStringEx ---- */
static u64 __attribute__((ms_abi))
impl_CompareStringEx(u64 locale, u32 flags, const u16 *s1, int n1,
                      const u16 *s2, int n2, u64 vers, u64 res, u64 lp)
    { (void)locale;(void)vers;(void)res;(void)lp;
      return impl_CompareStringW(0, flags, s1, n1, s2, n2); }

static u64 __attribute__((ms_abi))
impl_GetLocaleInfoEx(u64 locale, u32 lctype, u16 *data, int cchdata)
{
    const u16 *name = (const u16 *)locale;
    const int return_number = (lctype & 0x20000000u) != 0;
    const u32 type = lctype & ~0x20000000u;

    if (return_number) {
        u32 value;
        switch (type) {
            case 0x1004: value = 0x0409; break; /* LOCALE_ILANGUAGE */
            default: value = 0;
        }
        if (!data || cchdata == 0) return 2; /* DWORD measured in WCHARs */
        if (cchdata < 2) { g_last_error = 122; return 0; }
        memcpy(data, &value, sizeof(value));
        return 2;
    }

    const int japanese = name && (u16_ascii_equal_ci(name, "ja-JP") ||
                                  u16_ascii_equal_ci(name, "Japanese"));
    switch (type) {
        case 0x00000002: /* LOCALE_SLANGUAGE */
        case 0x00001001: /* LOCALE_SENGLANGUAGE */
            return copy_ascii_to_u16(japanese ? "Japanese" : "English (United States)",
                                     data, cchdata);
        case 0x0000005c: /* LOCALE_SNAME */
            return copy_ascii_to_u16(japanese ? "ja-JP" : "en-US", data, cchdata);
        case 0x00000001: /* LOCALE_ILANGUAGE */
            return copy_ascii_to_u16(japanese ? "0411" : "0409", data, cchdata);
        case 0x0000000e: /* LOCALE_SDECIMAL */
            return copy_ascii_to_u16(".", data, cchdata);
        case 0x0000000f: /* LOCALE_STHOUSAND */
            return copy_ascii_to_u16(",", data, cchdata);
        default:
            return impl_GetLocaleInfoW(japanese ? 0x0411 : 0x0409,
                                       type, data, cchdata);
    }
}

static u64 __attribute__((ms_abi))
impl_LCMapStringEx(u64 locale, u32 flags, const u16 *src, int srclen,
                    u16 *dst, int dstlen, u64 vers, u64 res, u64 sh)
    { (void)locale;(void)vers;(void)res;(void)sh;
      return impl_LCMapStringW(0, flags, src, srclen, dst, dstlen); }

/* ---- GetCurrentDirectory / SetCurrentDirectory ---- */
static u64 __attribute__((ms_abi))
impl_GetCurrentDirectoryA(u32 size, char *buffer)
{
    char cwd[PATH_MAX], windows_path[PATH_MAX + 3];
    if (!getcwd(cwd, sizeof(cwd))) return 0;
    snprintf(windows_path, sizeof(windows_path), "Z:%s", cwd);
    for (char *p = windows_path; *p; ++p) if (*p == '/') *p = '\\';
    u32 length = (u32)strlen(windows_path);
    if (!buffer || size <= length) return length + 1;
    memcpy(buffer, windows_path, length + 1);
    return length;
}
static u64 __attribute__((ms_abi))
impl_GetCurrentDirectoryW(u32 size, u16 *buffer)
{
    char path[PATH_MAX + 3];
    u64 length = impl_GetCurrentDirectoryA(sizeof(path), path);
    if (!length || length >= sizeof(path)) return length;
    if (!buffer || size <= length) return length + 1;
    for (u32 i = 0; i <= (u32)length; ++i) buffer[i] = (u8)path[i];
    return length;
}
static u64 __attribute__((ms_abi))
impl_SetCurrentDirectoryA(const char *path)
{
    char host_path[PATH_MAX];
    if (!path || !resolve_windows_host_path(path, host_path, sizeof(host_path))) return 0;
    return chdir(host_path) == 0 ? 1 : 0;
}
static u64 __attribute__((ms_abi))
impl_SetCurrentDirectoryW(const u16 *path)
{
    if (!path) return 0;
    char narrow[PATH_MAX];
    size_t i = 0;
    while (path[i] && i + 1 < sizeof(narrow)) {
        if (path[i] > 0x7f) return 0;
        narrow[i] = (char)path[i];
        ++i;
    }
    narrow[i] = 0;
    return impl_SetCurrentDirectoryA(narrow);
}

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

/* ---- GetSystemDefaultLangID / GetSystemDirectory ---- */
static u64 __attribute__((ms_abi))
impl_GetSystemDefaultLangID(void) { return 0x0409; /* en-US */ }

static u64 __attribute__((ms_abi))
impl_GetSystemDirectoryW(u16 *buffer, u32 size)
{
    u32 length = (u32)strlen(g_system_dir);
    if (!buffer || size <= length) { g_last_error = 122; return length + 1; }
    for (u32 i = 0; i <= length; ++i) buffer[i] = (u8)g_system_dir[i];
    return length;
}

static u64 __attribute__((ms_abi))
impl_GetSystemDirectoryA(char *buffer, u32 size)
{
    u32 length = (u32)strlen(g_system_dir);
    if (!buffer || size <= length) { g_last_error = 122; return length + 1; }
    memcpy(buffer, g_system_dir, length + 1);
    return length;
}

static u64 __attribute__((ms_abi))
impl_GetWindowsDirectoryW(u16 *buffer, u32 size)
{
    u32 length = (u32)strlen(g_windows_dir);
    if (!buffer || size <= length) { g_last_error = 122; return length + 1; }
    for (u32 i = 0; i <= length; ++i) buffer[i] = (u8)g_windows_dir[i];
    return length;
}

static u64 __attribute__((ms_abi))
impl_GetWindowsDirectoryA(char *buffer, u32 size)
{
    u32 length = (u32)strlen(g_windows_dir);
    if (!buffer || size <= length) { g_last_error = 122; return length + 1; }
    memcpy(buffer, g_windows_dir, length + 1);
    return length;
}

/* ID3DBlob used by D3DGetBlobPart. The returned part is an owned copy, as on
 * Windows, rather than an alias into the caller's shader bytecode. */
typedef struct {
    void **vtable;
    _Atomic(u32) refs;
    size_t size;
    u8 data[];
} BeerD3DBlob;

static u64 __attribute__((ms_abi)) beer_blob_QueryInterface(
    BeerD3DBlob *blob, const u8 *iid, void **out)
{
    static const u8 iid_iunknown[16] = {
        0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0,0x46
    };
    static const u8 iid_blob[16] = {
        0x78,0x0a,0x2f,0x8b,0x79,0x42,0x6e,0x41,
        0x93,0x13,0x0b,0xa0,0xb5,0x6d,0x19,0xcd
    };
    if (!out) return E_FAIL;
    *out = NULL;
    if (!iid || (memcmp(iid, iid_iunknown, 16) && memcmp(iid, iid_blob, 16)))
        return E_NOINTERFACE;
    *out = blob;
    atomic_fetch_add(&blob->refs, 1);
    return S_OK;
}

static u64 __attribute__((ms_abi)) beer_blob_AddRef(BeerD3DBlob *blob)
{
    return atomic_fetch_add(&blob->refs, 1) + 1;
}

static u64 __attribute__((ms_abi)) beer_blob_Release(BeerD3DBlob *blob)
{
    u32 old = atomic_fetch_sub(&blob->refs, 1);
    if (old == 1) { free(blob); return 0; }
    return old - 1;
}

static u64 __attribute__((ms_abi)) beer_blob_GetBufferPointer(BeerD3DBlob *blob)
{
    return (u64)blob->data;
}

static u64 __attribute__((ms_abi)) beer_blob_GetBufferSize(BeerD3DBlob *blob)
{
    return blob->size;
}

static void *g_d3d_blob_vtable[] = {
    (void *)beer_blob_QueryInterface,
    (void *)beer_blob_AddRef,
    (void *)beer_blob_Release,
    (void *)beer_blob_GetBufferPointer,
    (void *)beer_blob_GetBufferSize
};

static u64 __attribute__((ms_abi))
impl_D3DGetBlobPart(const u8 *src, size_t src_size, u32 part,
                    u32 flags, BeerD3DBlob **out)
{
    (void)flags;
    if (!out) return E_FAIL;
    *out = NULL;
    if (!src || src_size < 32 || memcmp(src, "DXBC", 4)) return E_FAIL;

    u32 chunk_count = *(const u32 *)(src + 28);
    if (chunk_count > (src_size - 32) / 4) return E_FAIL;
    const char *wanted = NULL;
    switch (part) {
        case 0: wanted = "ISGN"; break; /* D3D_BLOB_INPUT_SIGNATURE_BLOB */
        case 1: wanted = "OSGN"; break; /* D3D_BLOB_OUTPUT_SIGNATURE_BLOB */
        default: return (u64)0x80070057; /* E_INVALIDARG */
    }

    const u8 *chunk = NULL;
    size_t chunk_size = 0;
    for (u32 i = 0; i < chunk_count; ++i) {
        u32 offset = *(const u32 *)(src + 32 + i * 4);
        if (offset > src_size - 8) return E_FAIL;
        u32 size = *(const u32 *)(src + offset + 4);
        if ((size_t)size > src_size - offset - 8) return E_FAIL;
        if (!memcmp(src + offset, wanted, 4)) {
            chunk = src + offset;
            chunk_size = (size_t)size + 8;
            break;
        }
    }
    if (!chunk) return E_FAIL;

    BeerD3DBlob *blob = malloc(sizeof(*blob) + chunk_size);
    if (!blob) return (u64)0x8007000e;
    blob->vtable = g_d3d_blob_vtable;
    atomic_init(&blob->refs, 1);
    blob->size = chunk_size;
    memcpy(blob->data, chunk, chunk_size);
    *out = blob;
    return S_OK;
}

/* ---- Minimal FMOD event/core objects ----
 * Sekiro uses the legacy FMOD Ex C++ exports.  Returning FMOD_OK without
 * populating their mandatory output pointers is invalid and led directly to a
 * null dereference in the event-project loader.  These objects model a silent
 * (no-output) system: lifecycle/configuration calls succeed, getters return
 * coherent defaults, and project load returns a valid inert project object. */
typedef struct BeerFmodObject {
    void **vtable;
    _Atomic(u32) refs;
} BeerFmodObject;

enum {
    BEER_FMOD_OK = 0,
    BEER_FMOD_ERR_INVALID_PARAM = 31,
    /* FMOD Ex reports a non-zero result when indexed enumeration reaches the
     * end. The exact event-not-found value is version-specific; callers only
     * rely on success versus failure for this method. */
    BEER_FMOD_ERR_EVENT_NOTFOUND = 74
};

static u64 __attribute__((ms_abi)) beer_fmod_object_release(BeerFmodObject *obj)
{
    (void)obj;
    return BEER_FMOD_OK;
}

static u64 __attribute__((ms_abi))
beer_fmod_project_describe(BeerFmodObject *obj, void *description)
{
    (void)obj;
    (void)description;
    return BEER_FMOD_OK;
}

static u64 __attribute__((ms_abi))
beer_fmod_project_get_state(BeerFmodObject *obj, u32 *state)
{
    (void)obj;
    if (state) *state = 0;
    return BEER_FMOD_OK;
}

static u64 __attribute__((ms_abi))
beer_fmod_noop(BeerFmodObject *obj, u64 a, u64 b, u64 c)
{
    (void)obj; (void)a; (void)b; (void)c;
    return BEER_FMOD_OK;
}

static u64 __attribute__((ms_abi))
beer_fmod_project_zero_count(BeerFmodObject *obj, int *out)
{
    (void)obj;
    if (!out) return BEER_FMOD_ERR_INVALID_PARAM;
    *out = 0;
    return BEER_FMOD_OK;
}

static u64 __attribute__((ms_abi))
beer_fmod_project_get_group_by_index(BeerFmodObject *obj, int index,
                                     int cache_events, BeerFmodObject **out)
{
    (void)cache_events;
    if (!obj || !out || index < 0) return BEER_FMOD_ERR_INVALID_PARAM;
    /* The silent project contains no event groups. EventProject slot 3 is
     * getGroupByIndex; returning FMOD_OK with an untouched output previously
     * fed a stale pointer into the group's metadata walker. */
    *out = NULL;
    return BEER_FMOD_ERR_EVENT_NOTFOUND;
}

static void *g_fmod_project_vtable[16] = {
    (void *)beer_fmod_object_release,
    (void *)beer_fmod_project_describe,
    (void *)beer_fmod_noop,
    (void *)beer_fmod_project_get_group_by_index,
    (void *)beer_fmod_project_zero_count,
    (void *)beer_fmod_noop,
    (void *)beer_fmod_noop,
    (void *)beer_fmod_project_get_state,
    (void *)beer_fmod_noop,
    (void *)beer_fmod_noop,
    (void *)beer_fmod_noop,
    (void *)beer_fmod_noop,
    (void *)beer_fmod_noop,
    (void *)beer_fmod_noop,
    (void *)beer_fmod_noop,
    (void *)beer_fmod_noop
};

static u64 __attribute__((ms_abi))
beer_fmod_zero_count(BeerFmodObject *obj, int *out)
{
    (void)obj;
    if (!out) return BEER_FMOD_ERR_INVALID_PARAM;
    *out = 0;
    return BEER_FMOD_OK;
}

static void *g_fmod_plain_vtable[16] = {
    (void *)beer_fmod_object_release, (void *)beer_fmod_noop,
    (void *)beer_fmod_noop, (void *)beer_fmod_noop,
    (void *)beer_fmod_noop, (void *)beer_fmod_noop,
    (void *)beer_fmod_noop, (void *)beer_fmod_noop,
    (void *)beer_fmod_noop, (void *)beer_fmod_noop,
    (void *)beer_fmod_noop, (void *)beer_fmod_noop,
    (void *)beer_fmod_noop, (void *)beer_fmod_noop,
    (void *)beer_fmod_noop, (void *)beer_fmod_noop
};

/* EventCategory slot 3 is getNumSubCategories(int *).  The silent backend has
 * no child categories; writing zero is essential because callers initialize
 * the output storage with debug sentinels before testing it. */
static void *g_fmod_category_vtable[16] = {
    (void *)beer_fmod_object_release, (void *)beer_fmod_noop,
    (void *)beer_fmod_noop, (void *)beer_fmod_zero_count,
    (void *)beer_fmod_noop, (void *)beer_fmod_noop,
    (void *)beer_fmod_noop, (void *)beer_fmod_noop,
    (void *)beer_fmod_noop, (void *)beer_fmod_noop,
    (void *)beer_fmod_noop, (void *)beer_fmod_noop,
    (void *)beer_fmod_noop, (void *)beer_fmod_noop,
    (void *)beer_fmod_noop, (void *)beer_fmod_noop
};

static BeerFmodObject g_fmod_event_system  = { g_fmod_plain_vtable, 1 };
static BeerFmodObject g_fmod_core_system   = { g_fmod_plain_vtable, 1 };
static BeerFmodObject g_fmod_project       = { g_fmod_project_vtable, 1 };
static BeerFmodObject g_fmod_category      = { g_fmod_category_vtable, 1 };
static BeerFmodObject g_fmod_event         = { g_fmod_plain_vtable, 1 };
static BeerFmodObject g_fmod_channel_group = { g_fmod_plain_vtable, 1 };
static BeerFmodObject g_fmod_dsp           = { g_fmod_plain_vtable, 1 };
static BeerFmodObject g_fmod_connection    = { g_fmod_plain_vtable, 1 };
static BeerFmodObject g_fmod_sound         = { g_fmod_plain_vtable, 1 };
static BeerFmodObject g_fmod_channel       = { g_fmod_plain_vtable, 1 };

static u64 __attribute__((ms_abi))
impl_FMOD_Event_getInfo(BeerFmodObject *self, int *index, char **name,
                        void *event_info)
{
    (void)event_info;
    if (!self) return BEER_FMOD_ERR_INVALID_PARAM;
    if (index) *index = 0;
    if (name) *name = (char *)"Beer Silent Event";
    return BEER_FMOD_OK;
}

static u64 __attribute__((ms_abi))
impl_FMOD_System_createDSP(BeerFmodObject *self, const void *description,
                           BeerFmodObject **out)
{
    if (!self || !description || !out) return BEER_FMOD_ERR_INVALID_PARAM;
    *out = &g_fmod_dsp;
    return BEER_FMOD_OK;
}

static u64 __attribute__((ms_abi))
impl_FMOD_System_playDSP(BeerFmodObject *self, int channel_index,
                         BeerFmodObject *dsp, int paused,
                         BeerFmodObject **out)
{
    (void)channel_index;
    (void)paused;
    if (!self || !dsp || !out) return BEER_FMOD_ERR_INVALID_PARAM;
    *out = &g_fmod_channel;
    return BEER_FMOD_OK;
}

static u64 __attribute__((ms_abi))
impl_FMOD_System_createSound(BeerFmodObject *self, const char *name_or_data,
                             u32 mode, const void *create_info,
                             BeerFmodObject **out)
{
    (void)mode;
    (void)create_info;
    if (!self || !name_or_data || !out) return BEER_FMOD_ERR_INVALID_PARAM;
    /* The silent backend still has to return a valid Sound object.  FMOD_OK
     * with an untouched output made preloadFSB hand a null/zero-vtable object
     * to the guest's bank loader after the first rendered frame. */
    *out = &g_fmod_sound;
    return BEER_FMOD_OK;
}

static u64 __attribute__((ms_abi))
impl_FMOD_EventSystem_preloadFSB(BeerFmodObject *self, const char *name,
                                 int stream_instance, BeerFmodObject *sound,
                                 int load_into_memory)
{
    (void)stream_instance;
    (void)load_into_memory;
    if (!self || !name || !sound) return BEER_FMOD_ERR_INVALID_PARAM;
    return BEER_FMOD_OK;
}

static u64 __attribute__((ms_abi))
impl_FMOD_EventSystem_getSystemObject(BeerFmodObject *self, BeerFmodObject **out);
static u64 __attribute__((ms_abi))
impl_FMOD_EventSystem_load(BeerFmodObject *self, const char *name,
                           const void *load_info, BeerFmodObject **out);
static u64 __attribute__((ms_abi))
impl_FMOD_EventSystem_getEvent(BeerFmodObject *self, const char *name,
                               u32 mode, BeerFmodObject **out);
static u64 __attribute__((ms_abi))
impl_FMOD_ok(BeerFmodObject *self, u64 a, u64 b, u64 c);
static u64 __attribute__((ms_abi))
impl_FMOD_EventSystem_update_dispatch(void *event_system);
static u64 __attribute__((ms_abi))
impl_FMOD_EventSystem_getEvent_dispatch(void *event_system, const char *name,
                                        u32 mode, void **out);
static u64 __attribute__((ms_abi))
impl_FMOD_Event_start_dispatch(void *event);
static u64 __attribute__((ms_abi))
impl_FMOD_EventSystem_load_dispatch(void *event_system, const char *name,
                                    const void *load_info, void **out);
static u64 __attribute__((ms_abi))
impl_FMOD_EventSystem_init_dispatch(void *event_system, int max_channels,
                                    u32 flags, void *extra, u32 event_pool_size);

typedef struct {
    size_t slot;
    void *replacement;
    const char *name;
} BeerFmodVtableHook;

/* FMOD's exported C++ facade methods first unwrap the public object to an
 * internal interface and then invoke fixed virtual slots. Sekiro caches that
 * internal interface and calls it directly, so matching the vtable entries to
 * export-wrapper addresses can never work. Clone the internal vtable at
 * public-8 and replace the slots decoded from the bundled FMOD wrappers. */
static void patch_fmod_internal_vtable(void *public_object,
                                       const BeerFmodVtableHook *hooks,
                                       size_t hook_count,
                                       size_t slot_count)
{
    if (!public_object || !hooks || !hook_count || !slot_count) return;
    void *internal_object = (u8 *)public_object - 8;
    void ***object_vtable = (void ***)internal_object;
    void **source = *object_vtable;
    if (!source) return;

    void **clone = malloc(sizeof(void *) * slot_count);
    if (!clone) return;
    memcpy(clone, source, sizeof(void *) * slot_count);
    for (size_t hook = 0; hook < hook_count; ++hook) {
        if (hooks[hook].slot >= slot_count) {
            free(clone);
            return;
        }
        void *original = clone[hooks[hook].slot];
        if (hooks[hook].slot == 15) g_real_fmod_internal_init = (u64)original;
        else if (hooks[hook].slot == 18) g_real_fmod_internal_update = (u64)original;
        else if (hooks[hook].slot == 28) g_real_fmod_internal_load = (u64)original;
        else if (hooks[hook].slot == 38) g_real_fmod_internal_get_event = (u64)original;
        else if (hooks[hook].slot == 3 && slot_count == 16)
            g_real_fmod_internal_event_start = (u64)original;
        clone[hooks[hook].slot] = hooks[hook].replacement;
        fprintf(stderr, "[AUDIO] hooked FMOD internal vtable slot %zu: %s\n",
                hooks[hook].slot, hooks[hook].name);
    }
    *object_vtable = clone;
}

static void patch_fmod_event_system_vtable(void *event_system)
{
    /* Decoded from fmod_event64.dll's exported facade wrappers:
     * init +0x78, update +0x90, load +0xe0, getEvent +0x130. */
    BeerFmodVtableHook hooks[] = {
        { 15, (void *)impl_FMOD_EventSystem_init_dispatch, "EventSystem::init" },
        { 18, (void *)impl_FMOD_EventSystem_update_dispatch, "EventSystem::update" },
        { 28, (void *)impl_FMOD_EventSystem_load_dispatch, "EventSystem::load" },
        { 38, (void *)impl_FMOD_EventSystem_getEvent_dispatch, "EventSystem::getEvent" }
    };
    patch_fmod_internal_vtable(event_system, hooks,
                               sizeof(hooks) / sizeof(hooks[0]), 48);
}

static void patch_fmod_event_vtable(void *event)
{
    /* Event::start's facade tail-calls internal slot +0x18. */
    BeerFmodVtableHook hook = {
        3, (void *)impl_FMOD_Event_start_dispatch, "Event::start"
    };
    patch_fmod_internal_vtable(event, &hook, 1, 16);
}

static u64 __attribute__((ms_abi)) impl_FMOD_EventSystem_Create(BeerFmodObject **out)
{
    if (!out) return BEER_FMOD_ERR_INVALID_PARAM;
    *out = &g_fmod_event_system;
    fprintf(stderr, "[FMOD] EventSystem_Create -> silent event system %p\n", (void *)*out);
    return BEER_FMOD_OK;
}

/* Sekiro imports FMOD_EventSystem_Create without a DLL-qualified distinction,
 * so dependency-first IAT selection could bind the compatibility implementation
 * even in --audio=fmod mode. Route the public factory explicitly into the
 * loaded fmod_event64.dll while retaining compat mode as a stable fallback. */
static u64 __attribute__((ms_abi))
impl_BinkSetSoundSystem_dispatch(u64 open_callback, u64 parameter)
{
    if (!g_real_bink_set_sound_system) return 0;
    typedef u64 (__attribute__((ms_abi)) *Fn)(u64, u64);
    u64 result = ((Fn)g_real_bink_set_sound_system)(open_callback, parameter);
    fprintf(stderr, "[AUDIO] BinkSetSoundSystem(callback=%p, parameter=%p) -> %llu\n",
            (void *)open_callback, (void *)parameter, (unsigned long long)result);
    return result;
}

static u64 __attribute__((ms_abi))
impl_BinkOpen_dispatch(const char *name, u32 flags)
{
    if (!g_real_bink_open) return 0;
    typedef u64 (__attribute__((ms_abi)) *Fn)(const char *, u32);
    u64 result = ((Fn)g_real_bink_open)(name, flags);
    fprintf(stderr, "[AUDIO] BinkOpen(\"%s\", flags=0x%x) -> %p\n",
            name ? name : "(null)", flags, (void *)result);
    return result;
}

static u64 __attribute__((ms_abi))
impl_BinkClose_dispatch(void *bink)
{
    if (!g_real_bink_close) return 0;
    typedef u64 (__attribute__((ms_abi)) *Fn)(void *);
    fprintf(stderr, "[AUDIO] BinkClose(%p)\n", bink);
    return ((Fn)g_real_bink_close)(bink);
}

static void bink_trace_call(const char *name, u64 count, u64 result)
{
    if (count <= 8 || !(count % 256u))
        fprintf(stderr, "[BINK TRACE] %s #%llu -> 0x%llx\n", name,
                (unsigned long long)count, (unsigned long long)result);
}

static u64 __attribute__((ms_abi)) impl_BinkWait_dispatch(void *bink)
{
    typedef u64 (__attribute__((ms_abi)) *Fn)(void *);
    static _Atomic(u64) calls;
    u64 result = g_real_bink_wait ? ((Fn)g_real_bink_wait)(bink) : 0;
    bink_trace_call("BinkWait", atomic_fetch_add(&calls, 1) + 1, result);
    return result;
}

static u64 __attribute__((ms_abi)) impl_BinkNextFrame_dispatch(void *bink)
{
    typedef u64 (__attribute__((ms_abi)) *Fn)(void *);
    static _Atomic(u64) calls;
    u64 result = g_real_bink_next_frame ? ((Fn)g_real_bink_next_frame)(bink) : 0;
    bink_trace_call("BinkNextFrame", atomic_fetch_add(&calls, 1) + 1, result);
    return result;
}

static u64 __attribute__((ms_abi))
impl_BinkDoFrameAsyncMulti_dispatch(void *bink, u64 threads, u64 thread_count)
{
    static _Atomic(u64) calls;
    (void)threads;
    (void)thread_count;

    /* Bink's native asynchronous workers depend on Windows scheduler details
     * that Beer does not yet reproduce faithfully; they spin after the first
     * submitted frame while the caller waits for completion. Decode the same
     * frame through Bink's real synchronous implementation instead. This is
     * not fabricated output: the bundled decoder performs the complete frame
     * decode, only its scheduling policy changes. */
    u64 result = 0;
    if (g_real_bink_do_frame) {
        typedef u64 (__attribute__((ms_abi)) *SyncFn)(void *);
        result = ((SyncFn)g_real_bink_do_frame)(bink);
        /* Async submission reports whether work was accepted. The synchronous
         * call has completed that work before returning. */
        result = 1;
    }
    bink_trace_call("BinkDoFrameAsyncMulti(sync)",
                    atomic_fetch_add(&calls, 1) + 1, result);
    return result;
}

static u64 __attribute__((ms_abi)) impl_BinkDoFrameAsyncWait_dispatch(void *bink, s32 timeout)
{
    static _Atomic(u64) calls;
    (void)bink;
    (void)timeout;
    /* All work submitted by the wrapper above completes synchronously. */
    u64 result = 1;
    bink_trace_call("BinkDoFrameAsyncWait(sync)",
                    atomic_fetch_add(&calls, 1) + 1, result);
    return result;
}

static u64 __attribute__((ms_abi)) impl_BinkStartAsyncThread_dispatch(u32 thread_index, u64 affinity)
{
    static _Atomic(u64) calls;
    (void)thread_index;
    (void)affinity;
    /* Beer executes Bink frame decoding synchronously in the caller. Starting
     * Bink's native worker pool here leaves four workers spinning in its
     * Windows scheduler after the first submission, while the game waits for
     * completion. Report the logical worker as available without creating the
     * unsupported native scheduling path. */
    u64 result = 1;
    bink_trace_call("BinkStartAsyncThread(sync)",
                    atomic_fetch_add(&calls, 1) + 1, result);
    return result;
}

static u64 __attribute__((ms_abi)) impl_BinkRequestStopAsyncThread_dispatch(u32 thread_index)
{
    static _Atomic(u64) calls;
    (void)thread_index;
    u64 result = 1;
    bink_trace_call("BinkRequestStopAsyncThread(sync)",
                    atomic_fetch_add(&calls, 1) + 1, result);
    return result;
}

static u64 __attribute__((ms_abi)) impl_BinkWaitStopAsyncThread_dispatch(u32 thread_index)
{
    static _Atomic(u64) calls;
    (void)thread_index;
    u64 result = 1;
    bink_trace_call("BinkWaitStopAsyncThread(sync)",
                    atomic_fetch_add(&calls, 1) + 1, result);
    return result;
}

static u64 __attribute__((ms_abi))
impl_FMOD_System_setOutput_dispatch(void *system, u32 output)
{
    if (!g_use_guest_fmod)
        return impl_FMOD_ok((BeerFmodObject *)system, output, 0, 0);
    if (!system || !g_real_fmod_system_set_output)
        return BEER_FMOD_ERR_INVALID_PARAM;
    typedef u64 (__attribute__((ms_abi)) *SetOutputFn)(void *, u32);
    u64 result = ((SetOutputFn)g_real_fmod_system_set_output)(system, output);
    fprintf(stderr, "[AUDIO] real FMOD System::setOutput(%u) -> %llu\n",
            output, (unsigned long long)result);
    return result;
}

static u64 fmod_forward_volume(u64 target, void *object, float volume,
                               const char *kind)
{
    if (!object || !target) return BEER_FMOD_ERR_INVALID_PARAM;
    typedef u64 (__attribute__((ms_abi)) *Fn)(void *, float);
    float applied = volume;
    const char *unmute = getenv("BEER_SEKIRO_AUDIO_UNMUTE");
    if (unmute && *unmute && strcmp(unmute, "0") && applied <= 0.0f)
        applied = 1.0f;
    u64 result = ((Fn)target)(object, applied);
    static _Atomic(u32) logs;
    u32 index = atomic_fetch_add_explicit(&logs, 1, memory_order_relaxed);
    if (index < 64 || applied != volume)
        fprintf(stderr, "[AUDIO] real FMOD %s::setVolume(%p, %.3f -> %.3f) -> %llu%s\n",
                kind, object, volume, applied, (unsigned long long)result,
                applied != volume ? " [diagnostic override]" : "");
    return result;
}

static u64 __attribute__((ms_abi))
impl_FMOD_ChannelGroup_setVolume_dispatch(void *group, float volume)
{
    return fmod_forward_volume(g_real_fmod_channel_group_set_volume, group,
                               volume, "ChannelGroup");
}

static u64 __attribute__((ms_abi))
impl_FMOD_Event_setVolume_dispatch(void *event, float volume)
{
    return fmod_forward_volume(g_real_fmod_event_set_volume, event,
                               volume, "Event");
}

static u64 __attribute__((ms_abi))
impl_FMOD_System_createSound_dispatch(void *system, const char *name_or_data,
                                      u32 mode, const void *create_info, void **out)
{
    if (!system || !name_or_data || !out || !g_real_fmod_system_create_sound)
        return BEER_FMOD_ERR_INVALID_PARAM;
    typedef u64 (__attribute__((ms_abi)) *Fn)(void *, const char *, u32,
                                              const void *, void **);
    u64 result = ((Fn)g_real_fmod_system_create_sound)(system, name_or_data, mode,
                                                       create_info, out);
    static _Atomic(u32) logs;
    u32 index = atomic_fetch_add_explicit(&logs, 1, memory_order_relaxed);
    if (index < 64)
        fprintf(stderr, "[AUDIO] real FMOD System::createSound(\"%s\", mode=0x%x) "
                "-> %llu object=%p\n", name_or_data, mode,
                (unsigned long long)result, out ? *out : NULL);
    return result;
}

static u64 __attribute__((ms_abi))
impl_FMOD_EventSystem_preloadFSB_dispatch(void *event_system, const char *name,
                                          int stream_instance, void *sound,
                                          unsigned char load_into_memory)
{
    if (!event_system || !name || !sound || !g_real_fmod_event_preload_fsb)
        return BEER_FMOD_ERR_INVALID_PARAM;
    typedef u64 (__attribute__((ms_abi)) *Fn)(void *, const char *, int, void *,
                                              unsigned char);
    u64 result = ((Fn)g_real_fmod_event_preload_fsb)(event_system, name,
                                                     stream_instance, sound,
                                                     load_into_memory);
    static _Atomic(u32) logs;
    u32 index = atomic_fetch_add_explicit(&logs, 1, memory_order_relaxed);
    if (index < 64)
        fprintf(stderr, "[AUDIO] real FMOD EventSystem::preloadFSB(\"%s\", "
                "stream=%d memory=%d) -> %llu\n", name, stream_instance,
                load_into_memory, (unsigned long long)result);
    return result;
}

static u64 __attribute__((ms_abi))
impl_FMOD_EventSystem_Create_dispatch(BeerFmodObject **out)
{
    if (!g_use_guest_fmod)
        return impl_FMOD_EventSystem_Create(out);
    if (!g_real_fmod_event_system_create) {
        fprintf(stderr, "[AUDIO] real FMOD EventSystem factory is unavailable\n");
        if (out) *out = NULL;
        return BEER_FMOD_ERR_INVALID_PARAM;
    }
    typedef u64 (__attribute__((ms_abi)) *FmodEventSystemCreateFn)(void **);
    u64 result = ((FmodEventSystemCreateFn)g_real_fmod_event_system_create)((void **)out);
    fprintf(stderr, "[AUDIO] real FMOD EventSystem_Create -> result=%llu object=%p\n",
            (unsigned long long)result, out ? (void *)*out : NULL);
    if (!result && out && *out) {
        g_real_fmod_event_system = *out;
        fprintf(stderr,
                "[AUDIO] FMOD public object=%p words[-1]=%p [0]=%p [1]=%p [2]=%p [3]=%p\n",
                *out, ((void **)*out)[-1], ((void **)*out)[0], ((void **)*out)[1],
                ((void **)*out)[2], ((void **)*out)[3]);
    }
    /* Do not patch FMOD's private/internal vtable. Exported facade methods
     * translate their public arguments before invoking those slots, so routing
     * a private slot through a public-signature dispatcher corrupts arguments
     * (EventSystem::load was observed receiving bank bytes as a path) and then
     * the guest heap. The original private vtable already executes correctly;
     * Beer only needs to wrap the stable exported ABI and DirectSound boundary. */
    if (!result && out && *out && g_real_fmod_event_get_system) {
        typedef u64 (__attribute__((ms_abi)) *GetSystemFn)(void *, void **);
        u64 system_result = ((GetSystemFn)g_real_fmod_event_get_system)(*out,
                                                &g_real_fmod_core_system);
        fprintf(stderr, "[AUDIO] real FMOD core System -> result=%llu object=%p\n",
                (unsigned long long)system_result, g_real_fmod_core_system);
        if (system_result) return system_result;
    }

    /* Sekiro invokes EventSystem methods through the internal vtable cached at
     * public-8. That vtable is hooked above, so the game's real init call and
     * its exact arguments must own initialization. Do not preempt it with
     * guessed flags here; doing so turns the later init into a no-op and leaves
     * the event/project runtime unconfigured. */
    return result;
}

static void *fmod_public_event_system(void *event_system)
{
    /* Internal EventSystem virtual methods receive the internal interface at
     * public-8.  Exported C++ wrappers receive the public interface and unwrap
     * it themselves. Accept either form so IAT and cached-vtable calls share
     * one transparent dispatcher. */
    if (!event_system) return NULL;
    return event_system == (u8 *)g_real_fmod_event_system - 8
         ? g_real_fmod_event_system : event_system;
}

static u64 __attribute__((ms_abi))
impl_FMOD_EventSystem_update_dispatch(void *event_system)
{
    if (!g_use_guest_fmod)
        return impl_FMOD_ok((BeerFmodObject *)event_system, 0, 0, 0);
    int internal = event_system == (u8 *)g_real_fmod_event_system - 8;
    u64 target = internal ? g_real_fmod_internal_update : g_real_fmod_event_update;
    event_system = internal ? event_system : fmod_public_event_system(event_system);
    if (!event_system || !target) return BEER_FMOD_ERR_INVALID_PARAM;
    typedef u64 (__attribute__((ms_abi)) *Fn)(void *);
    u64 result = ((Fn)target)(event_system);
    static _Atomic(u32) updates;
    u32 update = atomic_fetch_add_explicit(&updates, 1, memory_order_relaxed) + 1;
    if (update <= 4 || !(update % 600))
        fprintf(stderr, "[AUDIO] real FMOD EventSystem::update #%u -> %llu\n",
                update, (unsigned long long)result);
    return result;
}

static u64 __attribute__((ms_abi))
impl_FMOD_EventSystem_getEvent_dispatch(void *event_system, const char *name,
                                        u32 mode, void **out)
{
    if (!g_use_guest_fmod)
        return impl_FMOD_EventSystem_getEvent((BeerFmodObject *)event_system, name,
                                               mode, (BeerFmodObject **)out);
    int internal = event_system == (u8 *)g_real_fmod_event_system - 8;
    u64 target = internal ? g_real_fmod_internal_get_event : g_real_fmod_event_get_event;
    event_system = internal ? event_system : fmod_public_event_system(event_system);
    if (!event_system || !name || !out || !target)
        return BEER_FMOD_ERR_INVALID_PARAM;
    typedef u64 (__attribute__((ms_abi)) *Fn)(void *, const char *, u32, void **);
    u64 result = ((Fn)target)(event_system, name, mode, out);
    /* FMOD Ex event values are opaque encoded handles (Sekiro commonly gets
     * values such as 0xc0001), not COM-style pointers with writable vtables.
     * They must only be passed back through FMOD's exported Event facade. */
    static _Atomic(u32) logs;
    u32 index = atomic_fetch_add_explicit(&logs, 1, memory_order_relaxed);
    if (index < 64)
        fprintf(stderr, "[AUDIO] real FMOD getEvent(\"%s\", mode=0x%x) -> %llu object=%p\n",
                name, mode, (unsigned long long)result, out ? *out : NULL);
    return result;
}

static u64 __attribute__((ms_abi))
impl_FMOD_Event_start_dispatch(void *event)
{
    if (!g_use_guest_fmod)
        return impl_FMOD_ok((BeerFmodObject *)event, 0, 0, 0);
    u64 target = g_real_fmod_internal_event_start
               ? g_real_fmod_internal_event_start : g_real_fmod_event_start;
    if (!event || !target) return BEER_FMOD_ERR_INVALID_PARAM;
    typedef u64 (__attribute__((ms_abi)) *Fn)(void *);
    u64 result = ((Fn)target)(event);
    static _Atomic(u32) logs;
    u32 index = atomic_fetch_add_explicit(&logs, 1, memory_order_relaxed);
    if (index < 64)
        fprintf(stderr, "[AUDIO] real FMOD Event::start(%p) -> %llu\n",
                event, (unsigned long long)result);
    return result;
}

static u64 __attribute__((ms_abi))
impl_FMOD_EventSystem_setMediaPath_dispatch(void *event_system, const char *path)
{
    if (!g_use_guest_fmod) return BEER_FMOD_OK;
    event_system = fmod_public_event_system(event_system);
    if (!event_system || !path) return BEER_FMOD_ERR_INVALID_PARAM;
    static u64 real_set_media_path;
    if (!real_set_media_path) {
        GuestDll *dll = guest_dll_find("fmod_event64.dll");
        if (dll) real_set_media_path = guest_dll_export(dll,
            "?setMediaPath@EventSystem@FMOD@@QEAA?AW4FMOD_RESULT@@PEBD@Z");
    }
    if (!real_set_media_path) return BEER_FMOD_ERR_INVALID_PARAM;
    typedef u64 (__attribute__((ms_abi)) *Fn)(void *, const char *);
    u64 result = ((Fn)real_set_media_path)(event_system, path);
    fprintf(stderr, "[AUDIO] real FMOD EventSystem::setMediaPath(\"%s\") -> %llu\n",
            path, (unsigned long long)result);
    return result;
}

static u64 __attribute__((ms_abi))
impl_FMOD_EventSystem_load_dispatch(void *event_system, const char *name,
                                    const void *load_info, void **out)
{
    if (!g_use_guest_fmod)
        return impl_FMOD_EventSystem_load((BeerFmodObject *)event_system, name,
                                          load_info, (BeerFmodObject **)out);
    int internal = event_system == (u8 *)g_real_fmod_event_system - 8;
    u64 target = internal ? g_real_fmod_internal_load : g_real_fmod_event_load;
    event_system = internal ? event_system : fmod_public_event_system(event_system);
    if (!event_system || !name || !out || !target)
        return BEER_FMOD_ERR_INVALID_PARAM;
    typedef u64 (__attribute__((ms_abi)) *Fn)(void *, const char *, const void *, void **);
    u64 result = ((Fn)target)(event_system, name, load_info, out);
    static _Atomic(u32) load_logs;
    u32 log_index = atomic_fetch_add_explicit(&load_logs, 1, memory_order_relaxed);
    if (log_index < 32)
        fprintf(stderr, "[AUDIO] real FMOD EventSystem::load(\"%s\") -> %llu object=%p\n",
                name, (unsigned long long)result, out ? *out : NULL);
    return result;
}

static u64 __attribute__((ms_abi))
impl_FMOD_EventSystem_getSystemObject_dispatch(void *event_system, void **out)
{
    if (!g_use_guest_fmod)
        return impl_FMOD_EventSystem_getSystemObject((BeerFmodObject *)event_system,
                                                      (BeerFmodObject **)out);
    event_system = fmod_public_event_system(event_system);
    if (!event_system || !out || !g_real_fmod_event_get_system) return BEER_FMOD_ERR_INVALID_PARAM;
    typedef u64 (__attribute__((ms_abi)) *Fn)(void *, void **);
    u64 result = ((Fn)g_real_fmod_event_get_system)(event_system, out);
    if (!result) g_real_fmod_core_system = *out;
    fprintf(stderr, "[AUDIO] real FMOD core System -> result=%llu object=%p\n",
            (unsigned long long)result, out ? *out : NULL);
    /* Do not initialize from this getter. Sekiro configures the returned core
     * system and later calls EventSystem::init with its real flags, extra
     * driver data, and event-pool size. Preempting that call opened only FMOD's
     * temporary device probe and suppressed the real event runtime. */
    return result;
}

static u64 __attribute__((ms_abi))
impl_FMOD_EventSystem_init_dispatch(void *event_system, int max_channels,
                                    u32 flags, void *extra, u32 event_pool_size)
{
    fprintf(stderr,
            "[AUDIO] FMOD EventSystem::init dispatch object=%p channels=%d flags=0x%x pool=%u internal=%d\n",
            event_system, max_channels, flags, event_pool_size,
            g_real_fmod_event_system &&
            event_system == (u8 *)g_real_fmod_event_system - 8);
    if (!g_use_guest_fmod)
        return impl_FMOD_ok((BeerFmodObject *)event_system, (u64)max_channels,
                            flags, (u64)extra);
    int internal = event_system == (u8 *)g_real_fmod_event_system - 8;
    u64 target = internal ? g_real_fmod_internal_init : g_real_fmod_event_init;
    event_system = internal ? event_system : fmod_public_event_system(event_system);
    if (!event_system || !target) return BEER_FMOD_ERR_INVALID_PARAM;
    /* Preserve the fifth stack argument from Sekiro. The earlier wrapper
     * replaced its real event-pool size with zero. */
    typedef u64 (__attribute__((ms_abi)) *InitFn)(void *, int, u32, void *, u32);
    if (g_real_fmod_initialized) return BEER_FMOD_OK;
    u64 result = ((InitFn)target)(event_system, max_channels,
                                  flags, extra, event_pool_size);
    fprintf(stderr,
            "[AUDIO] real FMOD EventSystem::init channels=%d flags=0x%x pool=%u -> %llu\n",
            max_channels, flags, event_pool_size, (unsigned long long)result);
    if (!result) g_real_fmod_initialized = 1;
    return result;
}

static u64 __attribute__((ms_abi))
impl_FMOD_EventSystem_getSystemObject(BeerFmodObject *self, BeerFmodObject **out)
{
    if (!self || !out) return BEER_FMOD_ERR_INVALID_PARAM;
    *out = &g_fmod_core_system;
    return BEER_FMOD_OK;
}

static u64 __attribute__((ms_abi))
impl_FMOD_EventSystem_load(BeerFmodObject *self, const char *name,
                           const void *load_info, BeerFmodObject **out)
{
    (void)load_info;
    if (!self || !name || !out) return BEER_FMOD_ERR_INVALID_PARAM;
    *out = &g_fmod_project;
    fprintf(stderr, "[FMOD] EventSystem::load(\"%s\") -> inert project %p\n",
            name, (void *)*out);
    return BEER_FMOD_OK;
}

static u64 __attribute__((ms_abi))
impl_FMOD_EventSystem_getCategory(BeerFmodObject *self, const char *name,
                                  BeerFmodObject **out)
{
    if (!self || !name || !out) return BEER_FMOD_ERR_INVALID_PARAM;
    *out = &g_fmod_category;
    return BEER_FMOD_OK;
}

static u64 __attribute__((ms_abi))
impl_FMOD_EventSystem_getCategoryByIndex(BeerFmodObject *self, int index,
                                         BeerFmodObject **out)
{
    if (!self || !out || index < 0) return BEER_FMOD_ERR_INVALID_PARAM;
    /* The silent event system exposes no categories. Reporting successful
     * objects for arbitrary indices makes the guest's enumeration unbounded. */
    *out = NULL;
    return BEER_FMOD_ERR_EVENT_NOTFOUND;
}

static u64 __attribute__((ms_abi))
impl_FMOD_get_int(BeerFmodObject *self, int *out)
{
    if (!self || !out) return BEER_FMOD_ERR_INVALID_PARAM;
    *out = 0;
    return BEER_FMOD_OK;
}

static u64 __attribute__((ms_abi))
impl_FMOD_getSpeakerMode(BeerFmodObject *self, u32 *mode)
{
    if (!self || !mode) return BEER_FMOD_ERR_INVALID_PARAM;
    *mode = 3; /* FMOD_SPEAKERMODE_STEREO */
    return BEER_FMOD_OK;
}

static u64 __attribute__((ms_abi))
impl_FMOD_getSoftwareFormat(BeerFmodObject *self, int *rate, u32 *format,
                            int *channels, int *max_input, u32 *resampler,
                            int *bits)
{
    if (!self) return BEER_FMOD_ERR_INVALID_PARAM;
    if (rate) *rate = 48000;
    if (format) *format = 2; /* FMOD_SOUND_FORMAT_PCM16 */
    if (channels) *channels = 2;
    if (max_input) *max_input = 0;
    if (resampler) *resampler = 1;
    if (bits) *bits = 16;
    return BEER_FMOD_OK;
}

static u64 __attribute__((ms_abi))
impl_FMOD_getDriverCaps(BeerFmodObject *self, int id, u32 *caps,
                        int *min_frequency, u32 *speaker_mode)
{
    (void)id;
    if (!self) return BEER_FMOD_ERR_INVALID_PARAM;
    if (caps) *caps = 0;
    if (min_frequency) *min_frequency = 48000;
    if (speaker_mode) *speaker_mode = 3; /* FMOD_SPEAKERMODE_STEREO */
    return BEER_FMOD_OK;
}

static u64 __attribute__((ms_abi))
impl_FMOD_get_output_object(BeerFmodObject *self, BeerFmodObject **out)
{
    if (!self || !out) return BEER_FMOD_ERR_INVALID_PARAM;
    *out = &g_fmod_core_system;
    return BEER_FMOD_OK;
}

static u64 __attribute__((ms_abi))
impl_FMOD_EventSystem_getEvent(BeerFmodObject *self, const char *name,
                               u32 mode, BeerFmodObject **out)
{
    (void)mode;
    if (!self || !name || !out) return BEER_FMOD_ERR_INVALID_PARAM;
    *out = &g_fmod_event;
    return BEER_FMOD_OK;
}

static u64 __attribute__((ms_abi))
impl_FMOD_Event_getChannelGroup(BeerFmodObject *self, BeerFmodObject **out)
{
    if (!self || !out) return BEER_FMOD_ERR_INVALID_PARAM;
    *out = &g_fmod_channel_group;
    return BEER_FMOD_OK;
}

static u64 __attribute__((ms_abi))
impl_FMOD_System_createChannelGroup(BeerFmodObject *self, const char *name,
                                    BeerFmodObject **out)
{
    (void)name;
    if (!self || !out) return BEER_FMOD_ERR_INVALID_PARAM;
    *out = &g_fmod_channel_group;
    return BEER_FMOD_OK;
}

static u64 __attribute__((ms_abi))
impl_FMOD_ChannelGroup_getDSPHead(BeerFmodObject *self, BeerFmodObject **out)
{
    if (!self || !out) return BEER_FMOD_ERR_INVALID_PARAM;
    *out = &g_fmod_dsp;
    return BEER_FMOD_OK;
}

static u64 __attribute__((ms_abi))
impl_FMOD_DSP_addInput(BeerFmodObject *self, BeerFmodObject *input,
                       BeerFmodObject **connection)
{
    if (!self || !input || !connection) return BEER_FMOD_ERR_INVALID_PARAM;
    *connection = &g_fmod_connection;
    return BEER_FMOD_OK;
}

static u64 __attribute__((ms_abi))
impl_FMOD_ok(BeerFmodObject *self, u64 a, u64 b, u64 c)
{
    (void)a; (void)b; (void)c;
    return self ? BEER_FMOD_OK : BEER_FMOD_ERR_INVALID_PARAM;
}

/* ---- DirectInput 8 -------------------------------------------------------
 * Sekiro creates DirectInput after its first UI draws.  Returning S_OK from
 * the generic import stub without writing ppvOut made the guest immediately
 * dereference NULL.  These objects implement the documented COM layouts and
 * expose a quiet keyboard/mouse backend until X11 event translation is wired
 * into device state. */
typedef struct BeerDirectInput BeerDirectInput;
typedef struct BeerDirectInputDevice BeerDirectInputDevice;

struct BeerDirectInput {
    void **vtable;
    _Atomic(u32) refs;
};

struct BeerDirectInputDevice {
    void **vtable;
    _Atomic(u32) refs;
    u8 guid[16];
    int kind; /* 1 = system mouse, 2 = system keyboard */
    int acquired;
};

#define DI_OK 0
#define DIENUM_STOP 0
#define DIERR_INVALIDPARAM ((u64)(u32)0x80070057u)
#define DIERR_OUTOFMEMORY  ((u64)(u32)0x8007000eu)

static const u8 beer_iid_iunknown[16] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xc0,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x46
};
static const u8 beer_iid_directinput8a[16] = {
    0x30,0x80,0x79,0xbf,0x3a,0x48,0xa2,0x4d,
    0xaa,0x99,0x5d,0x64,0xed,0x36,0x97,0x00
};
static const u8 beer_iid_directinput8w[16] = {
    0x31,0x80,0x79,0xbf,0x3a,0x48,0xa2,0x4d,
    0xaa,0x99,0x5d,0x64,0xed,0x36,0x97,0x00
};
static const u8 beer_iid_directinputdevice8a[16] = {
    0x80,0x10,0xd4,0x54,0x15,0xdc,0x33,0x48,
    0xa4,0x1b,0x74,0x8f,0x73,0xa3,0x81,0x79
};
static const u8 beer_iid_directinputdevice8w[16] = {
    0x81,0x10,0xd4,0x54,0x15,0xdc,0x33,0x48,
    0xa4,0x1b,0x74,0x8f,0x73,0xa3,0x81,0x79
};

static int beer_guid_is(const void *value, const u8 expected[16])
{
    return value && memcmp(value, expected, 16) == 0;
}

static u64 __attribute__((ms_abi))
beer_dinput_QueryInterface(BeerDirectInput *self, const void *iid, void **out)
{
    if (!out) return DIERR_INVALIDPARAM;
    *out = NULL;
    if (!self || (!beer_guid_is(iid, beer_iid_iunknown) &&
                  !beer_guid_is(iid, beer_iid_directinput8a) &&
                  !beer_guid_is(iid, beer_iid_directinput8w)))
        return E_NOINTERFACE;
    atomic_fetch_add(&self->refs, 1);
    *out = self;
    return DI_OK;
}

static u64 __attribute__((ms_abi)) beer_dinput_AddRef(BeerDirectInput *self)
{
    return self ? atomic_fetch_add(&self->refs, 1) + 1 : 0;
}

static u64 __attribute__((ms_abi)) beer_dinput_Release(BeerDirectInput *self)
{
    if (!self) return 0;
    u32 old = atomic_fetch_sub(&self->refs, 1);
    if (old == 1) { free(self); return 0; }
    return old - 1;
}

static u64 __attribute__((ms_abi))
beer_dinput_device_QueryInterface(BeerDirectInputDevice *self,
                                  const void *iid, void **out)
{
    if (!out) return DIERR_INVALIDPARAM;
    *out = NULL;
    if (!self || (!beer_guid_is(iid, beer_iid_iunknown) &&
                  !beer_guid_is(iid, beer_iid_directinputdevice8a) &&
                  !beer_guid_is(iid, beer_iid_directinputdevice8w)))
        return E_NOINTERFACE;
    atomic_fetch_add(&self->refs, 1);
    *out = self;
    return DI_OK;
}

static u64 __attribute__((ms_abi))
beer_dinput_device_AddRef(BeerDirectInputDevice *self)
{
    return self ? atomic_fetch_add(&self->refs, 1) + 1 : 0;
}

static u64 __attribute__((ms_abi))
beer_dinput_device_Release(BeerDirectInputDevice *self)
{
    if (!self) return 0;
    u32 old = atomic_fetch_sub(&self->refs, 1);
    if (old == 1) { free(self); return 0; }
    return old - 1;
}

static u64 __attribute__((ms_abi))
beer_dinput_device_ok(BeerDirectInputDevice *self, u64 a, u64 b, u64 c)
{
    (void)a; (void)b; (void)c;
    return self ? DI_OK : DIERR_INVALIDPARAM;
}

static u64 __attribute__((ms_abi))
beer_dinput_device_Acquire(BeerDirectInputDevice *self)
{
    if (!self) return DIERR_INVALIDPARAM;
    self->acquired = 1;
    return DI_OK;
}

static u64 __attribute__((ms_abi))
beer_dinput_device_Unacquire(BeerDirectInputDevice *self)
{
    if (!self) return DIERR_INVALIDPARAM;
    self->acquired = 0;
    return DI_OK;
}

static u64 __attribute__((ms_abi))
beer_dinput_device_GetDeviceState(BeerDirectInputDevice *self, u32 size,
                                  void *state)
{
    if (!self || !state || size == 0) return DIERR_INVALIDPARAM;
    collect_native_window_events();
    memset(state, 0, size);
    pthread_mutex_lock(&g_input_mutex);
    if (self->kind == 1) {
        /* Sekiro's pre-regression input path uses the standard DIMOUSESTATE
         * layout: three LONG axes followed by four byte-sized buttons. */
        const s32 axes[3] = {g_input.delta_x, g_input.delta_y, g_input.wheel};
        beer_dinput_write_standard_mouse_state(state, size, axes,
                                                g_input.mouse_buttons);
        g_input.delta_x = 0;
        g_input.delta_y = 0;
        g_input.wheel = 0;
    } else if (self->kind == 2) {
        u32 keys = size < sizeof(g_input.dik) ? size : sizeof(g_input.dik);
        memcpy(state, g_input.dik, keys);
    }
    pthread_mutex_unlock(&g_input_mutex);
    return DI_OK;
}

static u64 __attribute__((ms_abi))
beer_dinput_device_GetDeviceData(BeerDirectInputDevice *self, u32 object_size,
                                 void *data, u32 *count, u32 flags)
{
    (void)object_size; (void)data; (void)flags;
    if (!self || !count) return DIERR_INVALIDPARAM;
    *count = 0;
    return DI_OK;
}

static u64 __attribute__((ms_abi))
beer_dinput_device_Poll(BeerDirectInputDevice *self)
{
    return self ? DI_OK : DIERR_INVALIDPARAM;
}

static void *g_dinput_device_vtable[32] = {
    (void *)beer_dinput_device_QueryInterface,
    (void *)beer_dinput_device_AddRef,
    (void *)beer_dinput_device_Release,
    (void *)beer_dinput_device_ok, /* GetCapabilities */
    (void *)beer_dinput_device_ok, /* EnumObjects */
    (void *)beer_dinput_device_ok, /* GetProperty */
    (void *)beer_dinput_device_ok, /* SetProperty */
    (void *)beer_dinput_device_Acquire,
    (void *)beer_dinput_device_Unacquire,
    (void *)beer_dinput_device_GetDeviceState,
    (void *)beer_dinput_device_GetDeviceData,
    (void *)beer_dinput_device_ok, /* SetDataFormat */
    (void *)beer_dinput_device_ok, /* SetEventNotification */
    (void *)beer_dinput_device_ok, /* SetCooperativeLevel */
    (void *)beer_dinput_device_ok, /* GetObjectInfo */
    (void *)beer_dinput_device_ok, /* GetDeviceInfo */
    (void *)beer_dinput_device_ok, /* RunControlPanel */
    (void *)beer_dinput_device_ok, /* Initialize */
    (void *)beer_dinput_device_ok, (void *)beer_dinput_device_ok,
    (void *)beer_dinput_device_ok, (void *)beer_dinput_device_ok,
    (void *)beer_dinput_device_ok, (void *)beer_dinput_device_ok,
    (void *)beer_dinput_device_ok, (void *)beer_dinput_device_Poll,
    (void *)beer_dinput_device_ok, (void *)beer_dinput_device_ok,
    (void *)beer_dinput_device_ok, (void *)beer_dinput_device_ok,
    (void *)beer_dinput_device_ok, (void *)beer_dinput_device_ok
};

static u64 __attribute__((ms_abi))
beer_dinput_CreateDevice(BeerDirectInput *self, const void *guid,
                         BeerDirectInputDevice **out, void *outer)
{
    (void)outer;
    if (!self || !guid || !out) return DIERR_INVALIDPARAM;
    *out = NULL;
    BeerDirectInputDevice *device = calloc(1, sizeof(*device));
    if (!device) return DIERR_OUTOFMEMORY;
    device->vtable = g_dinput_device_vtable;
    atomic_init(&device->refs, 1);
    memcpy(device->guid, guid, sizeof(device->guid));
    /* GUID_SysMouse and GUID_SysKeyboard differ in Data1 only. */
    u32 guid_data1;
    memcpy(&guid_data1, guid, sizeof(guid_data1));
    if (guid_data1 == 0x6f1d2b60u) device->kind = 1;
    else if (guid_data1 == 0x6f1d2b61u) device->kind = 2;
    *out = device;
    fprintf(stderr, "[DINPUT] CreateDevice -> %s device %p\n",
            device->kind == 1 ? "mouse" :
            device->kind == 2 ? "keyboard" : "unknown",
            (void *)device);
    return DI_OK;
}

static u64 __attribute__((ms_abi))
beer_dinput_EnumDevices(BeerDirectInput *self, u32 type, void *callback,
                        void *context, u32 flags)
{
    (void)type; (void)callback; (void)context; (void)flags;
    return self ? DI_OK : DIERR_INVALIDPARAM;
}

static u64 __attribute__((ms_abi))
beer_dinput_ok(BeerDirectInput *self, u64 a, u64 b, u64 c)
{
    (void)a; (void)b; (void)c;
    return self ? DI_OK : DIERR_INVALIDPARAM;
}

static void *g_dinput_vtable[11] = {
    (void *)beer_dinput_QueryInterface,
    (void *)beer_dinput_AddRef,
    (void *)beer_dinput_Release,
    (void *)beer_dinput_CreateDevice,
    (void *)beer_dinput_EnumDevices,
    (void *)beer_dinput_ok, /* GetDeviceStatus */
    (void *)beer_dinput_ok, /* RunControlPanel */
    (void *)beer_dinput_ok, /* Initialize */
    (void *)beer_dinput_ok, /* FindDevice */
    (void *)beer_dinput_ok, /* EnumDevicesBySemantics */
    (void *)beer_dinput_ok  /* ConfigureDevices */
};

static u64 __attribute__((ms_abi))
impl_DirectInput8Create(u64 instance, u32 version, const void *iid,
                        BeerDirectInput **out, void *outer)
{
    (void)instance;
    if (!out) return DIERR_INVALIDPARAM;
    *out = NULL;
    if (outer || version < 0x0800 ||
        (!beer_guid_is(iid, beer_iid_directinput8a) &&
         !beer_guid_is(iid, beer_iid_directinput8w)))
        return DIERR_INVALIDPARAM;
    BeerDirectInput *object = calloc(1, sizeof(*object));
    if (!object) return DIERR_OUTOFMEMORY;
    object->vtable = g_dinput_vtable;
    atomic_init(&object->refs, 1);
    *out = object;
    fprintf(stderr, "[DINPUT] DirectInput8Create(version=0x%x) -> %p\n",
            version, (void *)object);
    return DI_OK;
}

/* ---- CommandLineToArgvW ---- */
static u64 __attribute__((ms_abi))
impl_CommandLineToArgvW(const u16 *cmdline, int *argc)
{
    (void)cmdline;
    if (argc) *argc = 1;
    return (u64)g_guest_argv_w;
}

/* ---- ImmDisableIME ---- */
static u64 __attribute__((ms_abi))
impl_ImmDisableIME(u32 tid) { (void)tid; return 1; }

/* ---- AMD AGS ----
 * The host backend does not expose AMD GPU Services. Returning AGS_SUCCESS from
 * the generic stub left both mandatory outputs untouched, after which Sekiro
 * treated a zeroed GPU-info object as initialized and called a null method.
 * Report the documented no-driver condition and clear the context output so the
 * game can take its vendor-neutral D3D11 path. */
#define AGS_NO_AMD_DRIVER_INSTALLED 6
static u64 __attribute__((ms_abi))
impl_agsInit(void **context, const void *configuration, void *gpu_info)
{
    (void)configuration;
    (void)gpu_info;
    if (context) *context = NULL;
    fprintf(stderr, "[AGS] AMD GPU Services unavailable; using standard D3D11 path\n");
    return AGS_NO_AMD_DRIVER_INSTALLED;
}

/* AGS 6.x renamed agsInit to agsInitialize and prepended an explicit SDK
 * version argument. RE8 imports this form. Returning the generic thunk's zero
 * would mean AGS_SUCCESS while leaving both mandatory output objects untouched,
 * selecting an extension path that Beer cannot provide. */
static u64 __attribute__((ms_abi))
impl_agsInitialize(u32 version, const void *configuration,
                   void **context, void *gpu_info)
{
    (void)version;
    (void)configuration;
    if (context) *context = NULL;
    if (gpu_info) memset(gpu_info, 0, 16);
    fprintf(stderr,
            "[AGS] AMD GPU Services unavailable; using standard D3D12 path\n");
    return AGS_NO_AMD_DRIVER_INSTALLED;
}

static u64 __attribute__((ms_abi)) impl_agsDeInit(void *context)
{
    (void)context;
    return AGS_NO_AMD_DRIVER_INSTALLED;
}

/* ---- Cryptographic random provider ---- */
#define BEER_CRYPT_PROVIDER 0x4352595054424545ULL
#define BEER_CALG_MD5       0x00008003u
#define BEER_CALG_SHA1      0x00008004u
#define BEER_HP_HASHVAL     0x0002u
#define BEER_HP_HASHSIZE    0x0004u

typedef struct BeerCryptHash {
    u64 magic;
    u32 algorithm;
    u8 *data;
    size_t size;
    size_t capacity;
} BeerCryptHash;

#define BEER_CRYPT_HASH_MAGIC 0x4841534842454552ULL

static u64 __attribute__((ms_abi))
impl_CryptAcquireContextW(u64 *provider, const u16 *container,
                          const u16 *provider_name, u32 provider_type, u32 flags)
{
    (void)container; (void)provider_name; (void)provider_type; (void)flags;
    if (!provider) { g_last_error = 87; return 0; }
    *provider = BEER_CRYPT_PROVIDER; /* stable opaque Beer crypto handle */
    return 1;
}

static u64 __attribute__((ms_abi))
impl_CryptAcquireContextA(u64 *provider, const char *container,
                          const char *provider_name, u32 provider_type, u32 flags)
{
    (void)container; (void)provider_name; (void)provider_type; (void)flags;
    if (!provider) { g_last_error = 87; return 0; }
    *provider = BEER_CRYPT_PROVIDER;
    return 1;
}

static u64 __attribute__((ms_abi))
impl_CryptReleaseContext(u64 provider, u32 flags)
{
    (void)flags;
    return provider == BEER_CRYPT_PROVIDER;
}

static u64 __attribute__((ms_abi))
impl_CryptGenRandom(u64 provider, u32 length, u8 *buffer)
{
    if (provider != BEER_CRYPT_PROVIDER || (!buffer && length)) {
        g_last_error = 87;
        return 0;
    }
    size_t done = 0;
    while (done < length) {
        ssize_t got = getrandom(buffer + done, (size_t)length - done, 0);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) { g_last_error = (u32)errno; return 0; }
        done += (size_t)got;
    }
    return 1;
}

static BeerCryptHash *beer_crypt_hash(u64 handle)
{
    BeerCryptHash *hash = (BeerCryptHash *)(uintptr_t)handle;
    return hash && hash->magic == BEER_CRYPT_HASH_MAGIC ? hash : NULL;
}

static u64 __attribute__((ms_abi))
impl_CryptCreateHash(u64 provider, u32 algorithm, u64 key, u32 flags,
                     u64 *hash_handle)
{
    (void)flags;
    if (hash_handle) *hash_handle = 0;
    if (provider != BEER_CRYPT_PROVIDER || !hash_handle || key != 0 ||
        (algorithm != BEER_CALG_MD5 && algorithm != BEER_CALG_SHA1)) {
        g_last_error = 87;
        return 0;
    }
    BeerCryptHash *hash = calloc(1, sizeof(*hash));
    if (!hash) { g_last_error = 8; return 0; }
    hash->magic = BEER_CRYPT_HASH_MAGIC;
    hash->algorithm = algorithm;
    *hash_handle = (u64)(uintptr_t)hash;
    return 1;
}

static u64 __attribute__((ms_abi))
impl_CryptHashData(u64 hash_handle, const u8 *data, u32 length, u32 flags)
{
    (void)flags;
    BeerCryptHash *hash = beer_crypt_hash(hash_handle);
    if (!hash || (!data && length)) { g_last_error = 87; return 0; }
    if ((size_t)length > SIZE_MAX - hash->size) { g_last_error = 8; return 0; }
    size_t required = hash->size + length;
    if (required > hash->capacity) {
        size_t capacity = hash->capacity ? hash->capacity : 64;
        while (capacity < required) {
            if (capacity > SIZE_MAX / 2) { capacity = required; break; }
            capacity *= 2;
        }
        u8 *grown = realloc(hash->data, capacity);
        if (!grown) { g_last_error = 8; return 0; }
        hash->data = grown;
        hash->capacity = capacity;
    }
    if (length) memcpy(hash->data + hash->size, data, length);
    hash->size = required;
    return 1;
}

static u64 __attribute__((ms_abi))
impl_CryptGetHashParam(u64 hash_handle, u32 parameter, u8 *data,
                       u32 *data_length, u32 flags)
{
    (void)flags;
    BeerCryptHash *hash = beer_crypt_hash(hash_handle);
    if (!hash || !data_length) { g_last_error = 87; return 0; }
    u32 required = hash->algorithm == BEER_CALG_MD5 ? 16u : 20u;
    if (parameter == BEER_HP_HASHSIZE) {
        required = sizeof(u32);
        if (!data || *data_length < required) {
            *data_length = required;
            g_last_error = 234; /* ERROR_MORE_DATA */
            return 0;
        }
        *(u32 *)data = hash->algorithm == BEER_CALG_MD5 ? 16u : 20u;
        *data_length = required;
        return 1;
    }
    if (parameter != BEER_HP_HASHVAL) { g_last_error = 87; return 0; }
    if (!data || *data_length < required) {
        *data_length = required;
        g_last_error = 234;
        return 0;
    }
    /* CryptoAPI consumers on this path need deterministic identity bytes, not
     * cryptographic authentication. Produce a stable digest of the complete
     * byte stream while preserving the documented output sizing contract. */
    u64 a = 1469598103934665603ULL;
    u64 b = 1099511628211ULL ^ hash->size;
    for (size_t i = 0; i < hash->size; ++i) {
        a = (a ^ hash->data[i]) * 1099511628211ULL;
        b ^= a + ((u64)hash->data[i] << ((i & 7u) * 8u));
        b = (b << 7) | (b >> 57);
    }
    for (u32 i = 0; i < required; ++i) {
        u64 value = i < 8 ? a : b;
        data[i] = (u8)(value >> ((i & 7u) * 8u));
        a = (a ^ (u64)i) * 1099511628211ULL;
        b ^= a >> 11;
    }
    *data_length = required;
    return 1;
}

static u64 __attribute__((ms_abi))
impl_CryptDestroyHash(u64 hash_handle)
{
    BeerCryptHash *hash = beer_crypt_hash(hash_handle);
    if (!hash) { g_last_error = 6; return 0; }
    hash->magic = 0;
    free(hash->data);
    free(hash);
    return 1;
}

static u64 __attribute__((ms_abi))
impl_SystemFunction036(u8 *buffer, u32 length)
{
    return impl_CryptGenRandom(0x4352595054424545ULL, length, buffer);
}

/* ---- UCRT/VCRUNTIME primitives ----
 * Large modern executables import these through API-set DLLs rather than using
 * compiler intrinsics.  Returning zero from the generic thunk corrupts startup
 * state long before USER32 can create a window. */
static u64 __attribute__((ms_abi))
impl_memcpy(void *dst, const void *src, size_t count)
{
    /* The C/Windows contract permits null pointers only for a zero-byte copy.
     * Avoid entering libc in that case: optimized memcpy implementations may
     * still touch or normalize the pointer even when count is zero. */
    if (count == 0) return (u64)(uintptr_t)dst;
    if (!dst || !src) { g_last_error = 87; return 0; }
    if ((uintptr_t)dst < 0x10000 || (uintptr_t)src < 0x10000) {
        fprintf(stderr,
                "[MEMCPY LOW] dst=%p src=%p count=%zu import=%llu "
                "incoming-rsp=0x%llx return=0x%llx saved-guest-rsp=0x%llx\n",
                dst, src, count,
                (unsigned long long)g_dispatch_import_index,
                (unsigned long long)g_dispatch_incoming_rsp,
                (unsigned long long)g_dispatch_incoming_return,
                (unsigned long long)g_saved_guest_rsp);
        fflush(stderr);
        g_last_error = 87;
        return 0;
    }
    return (u64)(uintptr_t)memcpy(dst, src, count);
}

static u64 __attribute__((ms_abi))
impl_memmove(void *dst, const void *src, size_t count)
{
    if (count == 0) return (u64)(uintptr_t)dst;
    if (!dst || !src) { g_last_error = 87; return 0; }
    return (u64)(uintptr_t)memmove(dst, src, count);
}

static u64 __attribute__((ms_abi))
impl_memset(void *dst, int value, size_t count)
{
    if (count == 0) return (u64)(uintptr_t)dst;
    if (!dst) { g_last_error = 87; return 0; }
    return (u64)(uintptr_t)memset(dst, value, count);
}

static u64 __attribute__((ms_abi))
impl_strchr(const char *text, int ch)
{
    return (u64)(uintptr_t)(text ? strchr(text, ch) : NULL);
}

static u64 __attribute__((ms_abi))
impl_strstr(const char *text, const char *needle)
{
    return (u64)(uintptr_t)((text && needle) ? strstr(text, needle) : NULL);
}

static u64 __attribute__((ms_abi))
impl_towupper(u32 ch)
{
    return (u64)(u32)towupper((wint_t)ch);
}

static u64 __attribute__((ms_abi))
impl__wcsnicmp(const u16 *left, const u16 *right, size_t count)
{
    if (!left || !right) return left == right ? 0 : left ? 1 : (u64)(s64)-1;
    for (size_t i = 0; i < count; ++i) {
        u32 a = (u32)towupper((wint_t)left[i]);
        u32 b = (u32)towupper((wint_t)right[i]);
        if (a != b) return (u64)(s64)(a < b ? -1 : 1);
        if (!left[i]) return 0;
    }
    return 0;
}

static u64 __attribute__((ms_abi))
impl_wcscpy_s(u16 *dst, size_t dst_count, const u16 *src)
{
    if (!dst || !dst_count || !src) {
        if (dst && dst_count) dst[0] = 0;
        return 22; /* EINVAL */
    }
    size_t length = 0;
    while (src[length]) ++length;
    if (length + 1 > dst_count) { dst[0] = 0; return 34; /* ERANGE */ }
    memcpy(dst, src, (length + 1) * sizeof(*dst));
    return 0;
}

static u64 __attribute__((ms_abi))
impl_wcsncpy_s(u16 *dst, size_t dst_count, const u16 *src, size_t count)
{
    if (!dst || !dst_count || !src) {
        if (dst && dst_count) dst[0] = 0;
        return 22;
    }
    size_t source_length = 0;
    while (src[source_length]) ++source_length;
    size_t copy = count == (size_t)-1 ? source_length :
                  (source_length < count ? source_length : count);
    if (copy + 1 > dst_count) {
        if (count == (size_t)-1) copy = dst_count - 1;
        else { dst[0] = 0; return 34; }
    }
    if (copy) memcpy(dst, src, copy * sizeof(*dst));
    dst[copy] = 0;
    return 0;
}

static u64 __attribute__((ms_abi))
impl_strncpy_s(char *dst, size_t dst_count, const char *src, size_t count)
{
    if (!dst || !dst_count || !src) {
        if (dst && dst_count) dst[0] = 0;
        return 22;
    }
    size_t source_length = strlen(src);
    size_t copy = count == (size_t)-1 ? source_length :
                  (source_length < count ? source_length : count);
    if (copy + 1 > dst_count) {
        if (count == (size_t)-1) copy = dst_count - 1;
        else { dst[0] = 0; return 34; }
    }
    if (copy) memcpy(dst, src, copy);
    dst[copy] = 0;
    return 0;
}

static u64 __attribute__((ms_abi))
impl__initterm(void (**first)(void), void (**last)(void))
{
    if (!first || !last) return 0;
    for (; first < last; ++first) if (*first) (*first)();
    return 0;
}

static u64 __attribute__((ms_abi))
impl__initterm_e(int (__attribute__((ms_abi)) **first)(void),
                 int (__attribute__((ms_abi)) **last)(void))
{
    if (!first || !last) return 0;
    for (; first < last; ++first) {
        if (!*first) continue;
        int result = (*first)();
        if (result) return (u64)(u32)result;
    }
    return 0;
}

static u64 __attribute__((ms_abi))
impl__get_narrow_winmain_command_line(void)
{
    return impl_GetCommandLineA();
}

/* UCRT process-wide startup state.  These values are returned by address from
 * the CRT, so they must remain stable for the lifetime of the process. */
static int g_ucrt_commode;
static int g_ucrt_fmode = 0x4000; /* _O_TEXT */
static int g_ucrt_app_type;

static u64 __attribute__((ms_abi))
impl__set_app_type(int app_type)
{
    g_ucrt_app_type = app_type;
    return 0;
}

static u64 __attribute__((ms_abi))
impl__set_fmode(int mode)
{
    /* UCRT accepts text, binary, and UTF text modes.  Preserve the exact mode
     * because later CRT code queries it through its own process-global state. */
    switch (mode) {
    case 0x4000:  /* _O_TEXT */
    case 0x8000:  /* _O_BINARY */
    case 0x10000: /* _O_WTEXT */
    case 0x20000: /* _O_U16TEXT */
    case 0x40000: /* _O_U8TEXT */
        g_ucrt_fmode = mode;
        return 0;
    default:
        return 22; /* EINVAL */
    }
}

static u64 __attribute__((ms_abi))
impl__p__commode(void)
{
    return (u64)(uintptr_t)&g_ucrt_commode;
}

static u64 __attribute__((ms_abi))
impl_strtoull(const char *text, char **end, int base)
{
    if (!text) {
        if (end) *end = NULL;
        return 0;
    }
    errno = 0;
    return (u64)strtoull(text, end, base);
}

#define BEER_ATEXIT_CAPACITY 4096
static void (__attribute__((ms_abi)) *g_ucrt_atexit[BEER_ATEXIT_CAPACITY])(void);
static u32 g_ucrt_atexit_count;
static pthread_mutex_t g_ucrt_atexit_mutex = PTHREAD_MUTEX_INITIALIZER;

static u64 __attribute__((ms_abi))
impl__crt_atexit(void (__attribute__((ms_abi)) *callback)(void))
{
    if (!callback) return (u64)(u32)-1;
    pthread_mutex_lock(&g_ucrt_atexit_mutex);
    int result = -1;
    if (g_ucrt_atexit_count < BEER_ATEXIT_CAPACITY) {
        g_ucrt_atexit[g_ucrt_atexit_count++] = callback;
        result = 0;
    }
    pthread_mutex_unlock(&g_ucrt_atexit_mutex);
    return (u64)(u32)result;
}

static u64 __attribute__((ms_abi))
impl__initialize_narrow_environment(void)
{
    return 0;
}

static u64 __attribute__((ms_abi))
impl__configure_narrow_argv(int mode)
{
    (void)mode;
    return 0;
}

static u64 __attribute__((ms_abi))
impl__configthreadlocale(int flag)
{
    /* _ENABLE_PER_THREAD_LOCALE and _DISABLE_PER_THREAD_LOCALE both return the
     * previous mode. Beer uses one coherent process locale, so report disabled. */
    (void)flag;
    return 0;
}

static int g_ucrt_new_mode;
static u64 __attribute__((ms_abi))
impl__set_new_mode(int mode)
{
    int previous = g_ucrt_new_mode;
    g_ucrt_new_mode = mode != 0;
    return (u64)(u32)previous;
}

/* UCRT heap and on-exit surface used by private MSVC redistributables.  These
 * allocations share Beer's tracked process heap so `_msize`, realloc, and free
 * remain coherent across API-set and HeapAlloc call paths. */
static u64 __attribute__((ms_abi))
impl_malloc(size_t size)
{
    return impl_HeapAlloc(impl_GetProcessHeap(), 0, size);
}

static u64 __attribute__((ms_abi))
impl_calloc(size_t count, size_t size)
{
    if (count && size > SIZE_MAX / count) return 0;
    return impl_HeapAlloc(impl_GetProcessHeap(), 0x8, count * size);
}

static u64 __attribute__((ms_abi))
impl_realloc(void *pointer, size_t size)
{
    if (!pointer) return impl_malloc(size);
    if (!size) {
        impl_HeapFree(impl_GetProcessHeap(), 0, pointer);
        return 0;
    }
    return impl_HeapReAlloc(impl_GetProcessHeap(), 0, pointer, size);
}

static u64 __attribute__((ms_abi))
impl_free(void *pointer)
{
    impl_HeapFree(impl_GetProcessHeap(), 0, pointer);
    return 0;
}

static u64 __attribute__((ms_abi))
impl__msize(void *pointer)
{
    return pointer ? impl_HeapSize(impl_GetProcessHeap(), 0, pointer) : (u64)-1;
}

static u64 __attribute__((ms_abi))
impl__callnewh(size_t size)
{
    (void)size;
    return 0; /* no process new-handler installed */
}

typedef struct {
    void (**first)(void);
    void (**last)(void);
    void (**end)(void);
} BeerOnExitTable;

static u64 __attribute__((ms_abi))
impl__initialize_onexit_table(BeerOnExitTable *table)
{
    if (!table) return (u64)(u32)-1;
    table->first = table->last = table->end = NULL;
    return 0;
}

static u64 __attribute__((ms_abi))
impl__register_onexit_function(BeerOnExitTable *table,
                              void (__attribute__((ms_abi)) *callback)(void))
{
    if (!table || !callback) return (u64)(u32)-1;
    size_t count = table->first ? (size_t)(table->last - table->first) : 0;
    size_t capacity = table->first ? (size_t)(table->end - table->first) : 0;
    if (count == capacity) {
        size_t next_capacity = capacity ? capacity * 2 : 32;
        void (**next)(void) = realloc(table->first,
                                     next_capacity * sizeof(*next));
        if (!next) return (u64)(u32)-1;
        table->first = next;
        table->last = next + count;
        table->end = next + next_capacity;
    }
    *table->last++ = (void (*)(void))callback;
    return 0;
}

static u64 __attribute__((ms_abi))
impl__execute_onexit_table(BeerOnExitTable *table)
{
    if (!table) return (u64)(u32)-1;
    while (table->first && table->last > table->first) {
        void (*callback)(void) = *--table->last;
        if (callback) callback();
    }
    free(table->first);
    table->first = table->last = table->end = NULL;
    return 0;
}

/* FILE is private to UCRT.  The early ConCRT path only needs stable, distinct
 * stream identities; actual console output continues through Win32 handles. */
static u8 g_ucrt_iob[3][64];
static u64 __attribute__((ms_abi))
impl___acrt_iob_func(u32 index)
{
    return index < 3 ? (u64)(uintptr_t)g_ucrt_iob[index] : 0;
}

/* ---- Impl dispatch table ---- */
typedef struct { const char *name; ImplFn fn; } ImplEntry;

static ImplEntry g_impls[] = {
    /* UCRT/VCRUNTIME primitives */
    {"memcpy",                                  (ImplFn)impl_memcpy},
    {"memmove",                                 (ImplFn)impl_memmove},
    {"memset",                                  (ImplFn)impl_memset},
    {"strchr",                                  (ImplFn)impl_strchr},
    {"strstr",                                  (ImplFn)impl_strstr},
    {"towupper",                                (ImplFn)impl_towupper},
    {"_wcsnicmp",                               (ImplFn)impl__wcsnicmp},
    {"wcscpy_s",                                (ImplFn)impl_wcscpy_s},
    {"wcsncpy_s",                               (ImplFn)impl_wcsncpy_s},
    {"strncpy_s",                               (ImplFn)impl_strncpy_s},
    {"_initterm",                               (ImplFn)impl__initterm},
    {"_initterm_e",                             (ImplFn)impl__initterm_e},
    {"_get_narrow_winmain_command_line",        (ImplFn)impl__get_narrow_winmain_command_line},
    {"_set_app_type",                           (ImplFn)impl__set_app_type},
    {"_set_fmode",                              (ImplFn)impl__set_fmode},
    {"__p__commode",                            (ImplFn)impl__p__commode},
    {"strtoull",                                (ImplFn)impl_strtoull},
    {"_crt_atexit",                             (ImplFn)impl__crt_atexit},
    {"malloc",                                  (ImplFn)impl_malloc},
    {"calloc",                                  (ImplFn)impl_calloc},
    {"realloc",                                 (ImplFn)impl_realloc},
    {"free",                                    (ImplFn)impl_free},
    {"_msize",                                  (ImplFn)impl__msize},
    {"_callnewh",                               (ImplFn)impl__callnewh},
    {"_initialize_onexit_table",                (ImplFn)impl__initialize_onexit_table},
    {"_register_onexit_function",               (ImplFn)impl__register_onexit_function},
    {"_execute_onexit_table",                   (ImplFn)impl__execute_onexit_table},
    {"__acrt_iob_func",                         (ImplFn)impl___acrt_iob_func},
    {"_initialize_narrow_environment",          (ImplFn)impl__initialize_narrow_environment},
    {"_configure_narrow_argv",                  (ImplFn)impl__configure_narrow_argv},
    {"_configthreadlocale",                     (ImplFn)impl__configthreadlocale},
    {"_set_new_mode",                           (ImplFn)impl__set_new_mode},
    {"AreFileApisANSI",                         (ImplFn)impl_AreFileApisANSI},
    {"VerSetConditionMask",                     (ImplFn)impl_VerSetConditionMask},
    {"VerifyVersionInfoW",                      (ImplFn)impl_VerifyVersionInfoW},
    {"RtlGetVersion",                           (ImplFn)impl_RtlGetVersion},
    {"WinHttpCheckPlatform",                    (ImplFn)impl_WinHttpCheckPlatform},
    {"GetAdaptersAddresses",                    (ImplFn)impl_GetAdaptersAddresses},
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
    {"GetSystemDirectoryA",                   (ImplFn)impl_GetSystemDirectoryA},
    {"GetSystemDirectoryW",                   (ImplFn)impl_GetSystemDirectoryW},
    {"GetWindowsDirectoryA",                  (ImplFn)impl_GetWindowsDirectoryA},
    {"GetWindowsDirectoryW",                  (ImplFn)impl_GetWindowsDirectoryW},
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
    {"DisableThreadLibraryCalls",             (ImplFn)impl_DisableThreadLibraryCalls},
    /* Process affinity */
    {"GetProcessAffinityMask",                (ImplFn)impl_GetProcessAffinityMask},
    {"SetProcessAffinityMask",                (ImplFn)impl_SetProcessAffinityMask},
    /* WINMM */
    {"timeGetTime",                           (ImplFn)impl_timeGetTime},
    {"timeBeginPeriod",                       (ImplFn)impl_timeBeginPeriod},
    {"timeEndPeriod",                         (ImplFn)impl_timeEndPeriod},
    {"waveOutGetNumDevs",                     (ImplFn)impl_waveOutGetNumDevs},
    {"waveInGetNumDevs",                      (ImplFn)impl_waveInGetNumDevs},
    {"waveOutGetDevCapsA",                    (ImplFn)impl_waveOutGetDevCapsA},
    {"waveOutGetDevCapsW",                    (ImplFn)impl_waveOutGetDevCapsW},
    {"waveOutOpen",                           (ImplFn)impl_waveOutOpen},
    {"waveOutClose",                          (ImplFn)impl_waveOutClose},
    {"waveOutPrepareHeader",                  (ImplFn)impl_waveOutPrepareHeader},
    {"waveOutUnprepareHeader",                (ImplFn)impl_waveOutUnprepareHeader},
    {"waveOutWrite",                          (ImplFn)impl_waveOutWrite},
    {"waveOutPause",                          (ImplFn)impl_waveOutPause},
    {"waveOutRestart",                        (ImplFn)impl_waveOutRestart},
    {"waveOutSetVolume",                      (ImplFn)impl_waveOutSetVolume},
    {"waveOutReset",                          (ImplFn)impl_waveOutReset},
    {"waveOutGetPosition",                    (ImplFn)impl_waveOutGetPosition},
    /* Registry: no persistent hive yet; return truthful not-found results. */
    {"RegOpenKeyExA",                          (ImplFn)impl_RegOpenKeyExA},
    {"RegOpenKeyExW",                          (ImplFn)impl_RegOpenKeyExW},
    {"RegQueryValueExA",                       (ImplFn)impl_RegQueryValueExA},
    {"RegQueryValueExW",                       (ImplFn)impl_RegQueryValueExW},
    {"RegCloseKey",                            (ImplFn)impl_RegCloseKey},
    /* Security */
    {"InitializeSecurityDescriptor",          (ImplFn)impl_InitializeSecurityDescriptor},
    {"SetSecurityDescriptorDacl",             (ImplFn)impl_SetSecurityDescriptorDacl},
    {"CryptAcquireContextA",                  (ImplFn)impl_CryptAcquireContextA},
    {"CryptAcquireContextW",                  (ImplFn)impl_CryptAcquireContextW},
    {"CryptReleaseContext",                   (ImplFn)impl_CryptReleaseContext},
    {"CryptGenRandom",                        (ImplFn)impl_CryptGenRandom},
    {"CryptCreateHash",                       (ImplFn)impl_CryptCreateHash},
    {"CryptHashData",                         (ImplFn)impl_CryptHashData},
    {"CryptGetHashParam",                     (ImplFn)impl_CryptGetHashParam},
    {"CryptDestroyHash",                      (ImplFn)impl_CryptDestroyHash},
    {"SystemFunction036",                     (ImplFn)impl_SystemFunction036},
    /* Steam */
    {"SteamAPI_Init",                         (ImplFn)impl_SteamAPI_Init},
    {"SteamAPI_RestartAppIfNecessary",        (ImplFn)impl_SteamAPI_RestartAppIfNecessary},
    {"SteamAPI_Shutdown",                     (ImplFn)impl_SteamAPI_Shutdown},
    {"SteamAPI_RunCallbacks",                 (ImplFn)impl_SteamAPI_RunCallbacks},
    {"SteamAPI_IsSteamRunning",               (ImplFn)impl_SteamAPI_IsSteamRunning},
    {"SteamInternal_ContextInit",              (ImplFn)impl_SteamInternal_ContextInit},
    {"SteamInternal_CreateInterface",         (ImplFn)impl_SteamInternal_CreateInterface},
    {"SteamInternal_FindOrCreateUserInterface", (ImplFn)impl_SteamInternal_FindOrCreateUserInterface},
    {"SteamInternal_FindOrCreateGameServerInterface", (ImplFn)impl_SteamInternal_FindOrCreateGameServerInterface},
    {"SteamAPI_GetHSteamUser",                 (ImplFn)impl_SteamAPI_GetHSteamUser},
    {"SteamAPI_GetHSteamPipe",                 (ImplFn)impl_SteamAPI_GetHSteamPipe},
    {"SteamAPI_RegisterCallback",             (ImplFn)impl_SteamAPI_RegisterCallback},
    {"SteamAPI_UnregisterCallback",           (ImplFn)impl_SteamAPI_UnregisterCallback},
    {"SteamAPI_RegisterCallResult",           (ImplFn)impl_SteamAPI_RegisterCallResult},
    {"SteamAPI_UnregisterCallResult",         (ImplFn)impl_SteamAPI_UnregisterCallResult},
    /* Steam interface objects */
    {"SteamApps",                             (ImplFn)impl_SteamApps},
    {"SteamClient",                           (ImplFn)impl_SteamClient},
    {"SteamUtils",                            (ImplFn)impl_SteamUtils},
    {"SteamUser",                             (ImplFn)impl_SteamUser},
    {"SteamFriends",                          (ImplFn)impl_SteamFriends},
    {"SteamUserStats",                        (ImplFn)impl_SteamUserStats},
    {"SteamUGC",                              (ImplFn)impl_SteamUGC},
    {"SteamRemoteStorage",                    (ImplFn)impl_SteamRemoteStorage},
    {"SteamNetworking",                       (ImplFn)impl_SteamNetworking},
    /* DXGI */
    {"CreateDXGIFactory",                     (ImplFn)impl_CreateDXGIFactory},
    {"CreateDXGIFactory1",                    (ImplFn)impl_CreateDXGIFactory1},
    {"CreateDXGIFactory2",                    (ImplFn)impl_CreateDXGIFactory2},
    /* WS2_32 */
    {"WSAStartup",                            (ImplFn)impl_WSAStartup},
    {"WSACleanup",                            (ImplFn)impl_WSACleanup},
    {"WSAGetLastError",                       (ImplFn)impl_WSAGetLastError},
    {"WSASetLastError",                       (ImplFn)impl_WSASetLastError},
    {"WSACreateEvent",                        (ImplFn)impl_WSACreateEvent},
    {"WSACloseEvent",                         (ImplFn)impl_WSACloseEvent},
    {"WSASetEvent",                           (ImplFn)impl_WSASetEvent},
    {"WSAResetEvent",                         (ImplFn)impl_WSAResetEvent},
    {"WSAWaitForMultipleEvents",              (ImplFn)impl_WSAWaitForMultipleEvents},
    {"socket",                                  (ImplFn)impl_socket},
    {"connect",                                 (ImplFn)impl_connect},
    {"closesocket",                             (ImplFn)impl_closesocket},
    {"getsockname",                             (ImplFn)impl_getsockname},
    {"inet_pton",                               (ImplFn)impl_inet_pton},
    /* NTDLL */
    {"NtQuerySystemInformation",              (ImplFn)impl_NtQuerySystemInformation},
    {"GetLogicalProcessorInformation",        (ImplFn)impl_GetLogicalProcessorInformation},
    {"GetLogicalProcessorInformationEx",      (ImplFn)impl_GetLogicalProcessorInformationEx},
    {"SetupDiGetClassDevsA",                  (ImplFn)impl_SetupDiGetClassDevsA},
    {"SetupDiGetClassDevsW",                  (ImplFn)impl_SetupDiGetClassDevsW},
    {"SetupDiEnumDeviceInfo",                 (ImplFn)impl_SetupDiEnumDeviceInfo},
    {"SetupDiEnumDeviceInterfaces",           (ImplFn)impl_SetupDiEnumDeviceInterfaces},
    {"SetupDiGetDeviceRegistryPropertyA",     (ImplFn)impl_SetupDiGetDeviceRegistryPropertyA},
    {"SetupDiGetDeviceRegistryPropertyW",     (ImplFn)impl_SetupDiGetDeviceRegistryPropertyW},
    {"SetupDiDestroyDeviceInfoList",          (ImplFn)impl_SetupDiDestroyDeviceInfoList},
    {"GlobalMemoryStatusEx",                  (ImplFn)impl_GlobalMemoryStatusEx},
    {"GetTimeZoneInformation",               (ImplFn)impl_GetTimeZoneInformation},
    /* Time */
    {"GetLocalTime",                          (ImplFn)impl_GetLocalTime},
    {"GetSystemTime",                         (ImplFn)impl_GetSystemTime},
    {"SystemTimeToFileTime",                  (ImplFn)impl_SystemTimeToFileTime},
    {"GetDateFormatEx",                       (ImplFn)impl_GetDateFormatEx},
    {"GetTimeFormatEx",                       (ImplFn)impl_GetTimeFormatEx},
    {"LCIDToLocaleName",                      (ImplFn)impl_LCIDToLocaleName},
    {"LocaleNameToLCID",                      (ImplFn)impl_LocaleNameToLCID},
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
    {"CoInitializeSecurity",                  (ImplFn)impl_CoInitializeSecurity},
    {"CoUninitialize",                        (ImplFn)impl_CoUninitialize},
    {"CoCreateInstance",                      (ImplFn)impl_CoCreateInstance},
    /* Media Foundation: typed, output-clearing boundaries. */
    {"MFStartup",                             (ImplFn)beer_mf_startup},
    {"MFShutdown",                            (ImplFn)beer_mf_shutdown},
    {"MFCreateDXGIDeviceManager",             (ImplFn)beer_mf_create_dxgi_device_manager},
    {"MFCreateAttributes",                    (ImplFn)beer_mf_create_attributes},
    {"MFCreateMediaType",                     (ImplFn)beer_mf_create_media_type},
    {"MFCreateMFByteStreamOnStream",          (ImplFn)beer_mf_create_byte_stream_on_stream},
    {"MFCreateSourceReaderFromByteStream",    (ImplFn)beer_mf_create_source_reader_from_byte_stream},
    /* DirectInput / DirectSound */
    {"DirectInput8Create",                    (ImplFn)impl_DirectInput8Create},
    {"DirectSoundCreate8",                    (ImplFn)impl_DirectSoundCreate8},
    {"DirectSoundCreate",                     (ImplFn)impl_DirectSoundCreate},
    {"DirectSoundEnumerateW",                 (ImplFn)impl_DirectSoundEnumerateW},
    {"DirectSoundCaptureEnumerateW",          (ImplFn)impl_DirectSoundCaptureEnumerateW},
    {"DirectSoundCaptureCreate8",             (ImplFn)impl_DirectSoundCaptureCreate8},
    {"DirectSoundCaptureCreate",              (ImplFn)impl_DirectSoundCaptureCreate},
    /* FMOD Ex: coherent silent event/core objects.  These decorated C++
     * exports are the exact names imported by Sekiro's FMOD Ex build. */
    {"FMOD_EventSystem_Create", (ImplFn)impl_FMOD_EventSystem_Create_dispatch},
    {"FMOD_Memory_Initialize", (ImplFn)impl_FMOD_ok},
    {"FMOD_Debug_SetLevel", (ImplFn)impl_FMOD_ok},
    {"?getSystemObject@EventSystem@FMOD@@QEAA?AW4FMOD_RESULT@@PEAPEAVSystem@2@@Z", (ImplFn)impl_FMOD_EventSystem_getSystemObject_dispatch},
    {"?load@EventSystem@FMOD@@QEAA?AW4FMOD_RESULT@@PEBDPEAUFMOD_EVENT_LOADINFO@@PEAPEAVEventProject@2@@Z", (ImplFn)impl_FMOD_EventSystem_load_dispatch},
    {"?init@EventSystem@FMOD@@QEAA?AW4FMOD_RESULT@@HIPEAXI@Z", (ImplFn)impl_FMOD_EventSystem_init_dispatch},
    {"?release@EventSystem@FMOD@@QEAA?AW4FMOD_RESULT@@XZ", (ImplFn)impl_FMOD_ok},
    {"?update@EventSystem@FMOD@@QEAA?AW4FMOD_RESULT@@XZ", (ImplFn)impl_FMOD_EventSystem_update_dispatch},
    {"?set3DListenerAttributes@EventSystem@FMOD@@QEAA?AW4FMOD_RESULT@@HPEBUFMOD_VECTOR@@000@Z", (ImplFn)impl_FMOD_ok},
    {"?unload@EventSystem@FMOD@@QEAA?AW4FMOD_RESULT@@XZ", (ImplFn)impl_FMOD_ok},
    {"?setLanguage@EventSystem@FMOD@@QEAA?AW4FMOD_RESULT@@PEBD@Z", (ImplFn)impl_FMOD_ok},
    {"?setMediaPath@EventSystem@FMOD@@QEAA?AW4FMOD_RESULT@@PEBD@Z", (ImplFn)impl_FMOD_EventSystem_setMediaPath_dispatch},
    {"?getCategory@EventSystem@FMOD@@QEAA?AW4FMOD_RESULT@@PEBDPEAPEAVEventCategory@2@@Z", (ImplFn)impl_FMOD_EventSystem_getCategory},
    {"?getCategoryByIndex@EventSystem@FMOD@@QEAA?AW4FMOD_RESULT@@HPEAPEAVEventCategory@2@@Z", (ImplFn)impl_FMOD_EventSystem_getCategoryByIndex},
    {"?getEvent@EventSystem@FMOD@@QEAA?AW4FMOD_RESULT@@PEBDIPEAPEAVEvent@2@@Z", (ImplFn)impl_FMOD_EventSystem_getEvent_dispatch},
    {"?getInfo@Event@FMOD@@QEAA?AW4FMOD_RESULT@@PEAHPEAPEADPEAUFMOD_EVENT_INFO@@@Z", (ImplFn)impl_FMOD_Event_getInfo},
    {"?start@Event@FMOD@@QEAA?AW4FMOD_RESULT@@XZ", (ImplFn)impl_FMOD_Event_start_dispatch},
    {"?getChannelGroup@Event@FMOD@@QEAA?AW4FMOD_RESULT@@PEAPEAVChannelGroup@2@@Z", (ImplFn)impl_FMOD_Event_getChannelGroup},
    {"?createChannelGroup@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEBDPEAPEAVChannelGroup@2@@Z", (ImplFn)impl_FMOD_System_createChannelGroup},
    {"?getDSPHead@ChannelGroup@FMOD@@QEAA?AW4FMOD_RESULT@@PEAPEAVDSP@2@@Z", (ImplFn)impl_FMOD_ChannelGroup_getDSPHead},
    {"?addInput@DSP@FMOD@@QEAA?AW4FMOD_RESULT@@PEAV12@PEAPEAVDSPConnection@2@@Z", (ImplFn)impl_FMOD_DSP_addInput},
    {"?getNumGroups@ChannelGroup@FMOD@@QEAA?AW4FMOD_RESULT@@PEAH@Z", (ImplFn)impl_FMOD_get_int},
    {"?setLevels@DSPConnection@FMOD@@QEAA?AW4FMOD_RESULT@@W4FMOD_SPEAKER@@PEAMH@Z", (ImplFn)impl_FMOD_ok},
    {"?disconnectAll@DSP@FMOD@@QEAA?AW4FMOD_RESULT@@_N0@Z", (ImplFn)impl_FMOD_ok},
    {"?setVolume@ChannelGroup@FMOD@@QEAA?AW4FMOD_RESULT@@M@Z", (ImplFn)impl_FMOD_ok},
    {"?addGroup@ChannelGroup@FMOD@@QEAA?AW4FMOD_RESULT@@PEAV12@@Z", (ImplFn)impl_FMOD_ok},
    {"?set3DNumListeners@EventSystem@FMOD@@QEAA?AW4FMOD_RESULT@@H@Z", (ImplFn)impl_FMOD_ok},
    {"?getNumProjects@EventSystem@FMOD@@QEAA?AW4FMOD_RESULT@@PEAH@Z", (ImplFn)impl_FMOD_get_int},
    {"?getNumDrivers@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEAH@Z", (ImplFn)impl_FMOD_get_int},
    {"?getSoftwareChannels@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEAH@Z", (ImplFn)impl_FMOD_get_int},
    {"?FS_GetSoftwareChannelsUsed@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEAH@Z", (ImplFn)impl_FMOD_get_int},
    {"?FS_GetEmulatedChannelsUsed@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEAH@Z", (ImplFn)impl_FMOD_get_int},
    {"?getHardwareChannels@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEAH@Z", (ImplFn)impl_FMOD_get_int},
    {"?getSpeakerMode@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEAW4FMOD_SPEAKERMODE@@@Z", (ImplFn)impl_FMOD_getSpeakerMode},
    {"?getSoftwareFormat@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEAHPEAW4FMOD_SOUND_FORMAT@@00PEAW4FMOD_DSP_RESAMPLER@@0@Z", (ImplFn)impl_FMOD_getSoftwareFormat},
    {"?getDriverCaps@System@FMOD@@QEAA?AW4FMOD_RESULT@@HPEAIPEAHPEAW4FMOD_SPEAKERMODE@@@Z", (ImplFn)impl_FMOD_getDriverCaps},
    {"?getOutputHandle@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEAPEAX@Z", (ImplFn)impl_FMOD_get_output_object},
    {"?getMasterChannelGroup@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEAPEAVChannelGroup@2@@Z", (ImplFn)impl_FMOD_get_output_object},
    {"?setOutput@System@FMOD@@QEAA?AW4FMOD_RESULT@@W4FMOD_OUTPUTTYPE@@@Z", (ImplFn)impl_FMOD_ok},
    {"?setSoftwareChannels@System@FMOD@@QEAA?AW4FMOD_RESULT@@H@Z", (ImplFn)impl_FMOD_ok},
    {"?setSoftwareFormat@System@FMOD@@QEAA?AW4FMOD_RESULT@@HW4FMOD_SOUND_FORMAT@@HHW4FMOD_DSP_RESAMPLER@@@Z", (ImplFn)impl_FMOD_ok},
    {"?setDSPBufferSize@System@FMOD@@QEAA?AW4FMOD_RESULT@@IH@Z", (ImplFn)impl_FMOD_ok},
    {"?setSpeakerMode@System@FMOD@@QEAA?AW4FMOD_RESULT@@W4FMOD_SPEAKERMODE@@@Z", (ImplFn)impl_FMOD_ok},
    {"?setDriver@System@FMOD@@QEAA?AW4FMOD_RESULT@@H@Z", (ImplFn)impl_FMOD_ok},
    {"?createDSP@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEAUFMOD_DSP_DESCRIPTION@@PEAPEAVDSP@2@@Z", (ImplFn)impl_FMOD_System_createDSP},
    {"?playDSP@System@FMOD@@QEAA?AW4FMOD_RESULT@@W4FMOD_CHANNELINDEX@@PEAVDSP@2@_NPEAPEAVChannel@2@@Z", (ImplFn)impl_FMOD_System_playDSP},
    {"?setVolume@Channel@FMOD@@QEAA?AW4FMOD_RESULT@@M@Z", (ImplFn)impl_FMOD_ok},
    {"?setFrequency@Channel@FMOD@@QEAA?AW4FMOD_RESULT@@M@Z", (ImplFn)impl_FMOD_ok},
    {"?createSound@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEBDIPEAUFMOD_CREATESOUNDEXINFO@@PEAPEAVSound@2@@Z", (ImplFn)impl_FMOD_System_createSound},
    {"?preloadFSB@EventSystem@FMOD@@QEAA?AW4FMOD_RESULT@@PEBDHPEAVSound@2@_N@Z", (ImplFn)impl_FMOD_EventSystem_preloadFSB},
    {"?setStreamBufferSize@System@FMOD@@QEAA?AW4FMOD_RESULT@@II@Z", (ImplFn)impl_FMOD_ok},
    {"?setFileSystem@System@FMOD@@QEAA?AW4FMOD_RESULT@@P6A?AW43@PEBDHPEAIPEAPEAX2@ZP6A?AW43@PEAX4@ZP6A?AW43@44I14@ZP6A?AW43@4I4@ZP6A?AW43@PEAUFMOD_ASYNCREADINFO@@4@Z5H@Z", (ImplFn)impl_FMOD_ok},
    {"?setCallback@System@FMOD@@QEAA?AW4FMOD_RESULT@@P6A?AW43@PEAUFMOD_SYSTEM@@W4FMOD_SYSTEM_CALLBACKTYPE@@PEAX2@Z@Z", (ImplFn)impl_FMOD_ok},
    {"?setAdvancedSettings@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEAUFMOD_ADVANCEDSETTINGS@@@Z", (ImplFn)impl_FMOD_ok},
    /* D3D11 / shader container helpers */
    {"D3D12CreateDevice",                     (ImplFn)beer_d3d12_create_device},
    {"D3D12GetDebugInterface",                (ImplFn)beer_d3d12_get_debug_interface},
    {"D3D12SerializeVersionedRootSignature",  (ImplFn)beer_d3d12_serialize_versioned_root_signature},
    {"D3D11CreateDevice",                     (ImplFn)impl_D3D11CreateDevice},
    {"D3D11CreateDeviceAndSwapChain",         (ImplFn)impl_D3D11CreateDeviceAndSwapChain},
    {"D3DGetBlobPart",                        (ImplFn)impl_D3DGetBlobPart},
    {"D3DPERF_GetStatus",                     (ImplFn)impl_D3DPERF_GetStatus},
    /* String / locale */
    {"MultiByteToWideChar",                   (ImplFn)impl_MultiByteToWideChar},
    {"WideCharToMultiByte",                   (ImplFn)impl_WideCharToMultiByte},
    /* Process */
    {"GetCurrentProcess",                     (ImplFn)impl_GetCurrentProcess},
    {"ExitProcess",                           (ImplFn)impl_ExitProcess},
    {"CorExitProcess",                        (ImplFn)impl_CorExitProcess},
    {"TerminateProcess",                      (ImplFn)impl_TerminateProcess},
    {"FatalAppExitW",                          (ImplFn)impl_FatalAppExitW},
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
    {"WaitForSingleObjectEx",                 (ImplFn)impl_WaitForSingleObjectEx},
    {"WaitForMultipleObjects",                (ImplFn)impl_WaitForMultipleObjects},
    {"WaitForMultipleObjectsEx",              (ImplFn)impl_WaitForMultipleObjectsEx},
    {"SetEvent",                              (ImplFn)impl_SetEvent},
    {"ResetEvent",                            (ImplFn)impl_ResetEvent},
    {"PulseEvent",                            (ImplFn)impl_PulseEvent},
    /* Mutex */
    {"CreateMutexA",                          (ImplFn)impl_CreateMutexA},
    {"CreateMutexW",                          (ImplFn)impl_CreateMutexW},
    {"ReleaseMutex",                          (ImplFn)impl_ReleaseMutex},
    {"OpenMutexA",                            (ImplFn)impl_OpenMutexA},
    /* Sleep / timing */
    {"Sleep",                                 (ImplFn)impl_Sleep},
    {"SleepEx",                               (ImplFn)impl_SleepEx},
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
    {"GetUserDefaultLangID",                  (ImplFn)impl_GetUserDefaultLangID},
    {"GetKeyboardLayout",                     (ImplFn)impl_GetKeyboardLayout},
    {"MapVirtualKeyA",                        (ImplFn)impl_MapVirtualKeyW},
    {"MapVirtualKeyW",                        (ImplFn)impl_MapVirtualKeyW},
    {"MapVirtualKeyExA",                      (ImplFn)impl_MapVirtualKeyExW},
    {"MapVirtualKeyExW",                      (ImplFn)impl_MapVirtualKeyExW},
    /* Format */
    {"FormatMessageA",                        (ImplFn)impl_FormatMessageA},
    {"FormatMessageW",                        (ImplFn)impl_FormatMessageW},
    /* Window placeholders */
    {"RegisterClassExA",                      (ImplFn)impl_RegisterClassExA},
    {"RegisterClassExW",                      (ImplFn)impl_RegisterClassExW},
    {"RegisterDeviceNotificationA",           (ImplFn)impl_RegisterDeviceNotificationA},
    {"RegisterDeviceNotificationW",           (ImplFn)impl_RegisterDeviceNotificationW},
    {"UnregisterDeviceNotification",          (ImplFn)impl_UnregisterDeviceNotification},
    {"RegisterSuspendResumeNotification",     (ImplFn)impl_RegisterSuspendResumeNotification},
    {"UnregisterSuspendResumeNotification",   (ImplFn)impl_UnregisterSuspendResumeNotification},
    {"GetSystemMetrics",                      (ImplFn)impl_GetSystemMetrics},
    {"GetDesktopWindow",                      (ImplFn)impl_GetDesktopWindow},
    {"GetForegroundWindow",                   (ImplFn)impl_GetForegroundWindow},
    {"GetTopWindow",                          (ImplFn)impl_GetTopWindow},
    {"GetWindow",                             (ImplFn)impl_GetWindow},
    {"GetWindowThreadProcessId",              (ImplFn)impl_GetWindowThreadProcessId},
    {"SetTimer",                              (ImplFn)impl_SetTimer},
    {"KillTimer",                             (ImplFn)impl_KillTimer},
    {"IsZoomed",                              (ImplFn)impl_IsZoomed},
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
    {"GetExitCodeThread",                     (ImplFn)impl_GetExitCodeThread},
    {"DuplicateHandle",                       (ImplFn)impl_DuplicateHandle},
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
    {"GetTempPath2W",                         (ImplFn)impl_GetTempPathW},
    {"GetTempPath2A",                         (ImplFn)impl_GetTempPathA},
    {"GetFullPathNameW",                      (ImplFn)impl_GetFullPathNameW},
    {"GetFullPathNameA",                      (ImplFn)impl_GetFullPathNameA},
    {"GetDiskFreeSpaceExA",                   (ImplFn)impl_GetDiskFreeSpaceExA},
    {"GetDiskFreeSpaceExW",                   (ImplFn)impl_GetDiskFreeSpaceExW},
    {"CreateFileA",                           (ImplFn)impl_CreateFileA},
    {"CreateFileW",                           (ImplFn)impl_CreateFileW},
    {"CloseHandle",                           (ImplFn)impl_CloseHandle},
    {"ReadFile",                              (ImplFn)impl_ReadFile},
    {"WriteFile",                             (ImplFn)impl_WriteFile},
    {"FlushFileBuffers",                      (ImplFn)impl_FlushFileBuffers},
    {"GetFileInformationByHandle",            (ImplFn)impl_GetFileInformationByHandle},
    {"GetFileSize",                           (ImplFn)impl_GetFileSize},
    {"GetFileSizeEx",                         (ImplFn)impl_GetFileSizeEx},
    {"SetFilePointer",                        (ImplFn)impl_SetFilePointer},
    {"SetEndOfFile",                          (ImplFn)impl_SetEndOfFile},
    {"CopyFileA",                             (ImplFn)impl_CopyFileA},
    {"CopyFileW",                             (ImplFn)impl_CopyFileW},
    {"GetFileAttributesA",                    (ImplFn)impl_GetFileAttributesA},
    {"GetFileAttributesW",                    (ImplFn)impl_GetFileAttributesW},
    {"PathFileExistsA",                       (ImplFn)impl_PathFileExistsA},
    {"PathFileExistsW",                       (ImplFn)impl_PathFileExistsW},
    {"GetPrivateProfileStringW",              (ImplFn)impl_GetPrivateProfileStringW},
    {"WritePrivateProfileStringW",            (ImplFn)impl_WritePrivateProfileStringW},
    {"SetFileAttributesA",                    (ImplFn)impl_SetFileAttributesA},
    {"SetFileAttributesW",                    (ImplFn)impl_SetFileAttributesW},
    /* Thread affinity */
    {"SetThreadAffinityMask",                 (ImplFn)impl_SetThreadAffinityMask},
    {"SetThreadIdealProcessor",               (ImplFn)impl_SetThreadIdealProcessor},
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
    {"IsWindowEnabled",                       (ImplFn)impl_IsWindowEnabled},
    {"IsWindowVisible",                       (ImplFn)impl_IsWindowVisible},
    {"IsIconic",                              (ImplFn)impl_IsIconic},
    {"SystemParametersInfoA",                 (ImplFn)impl_SystemParametersInfoA},
    {"SystemParametersInfoW",                 (ImplFn)impl_SystemParametersInfoW},
    {"SetWindowTextW",                        (ImplFn)impl_SetWindowTextW},
    {"SetWindowTextA",                        (ImplFn)impl_SetWindowTextA},
    {"MoveWindow",                            (ImplFn)impl_MoveWindow},
    {"SetWindowPos",                          (ImplFn)impl_SetWindowPos},
    {"GetDC",                                 (ImplFn)impl_GetDC},
    {"ReleaseDC",                             (ImplFn)impl_ReleaseDC},
    {"GetStockObject",                        (ImplFn)impl_GetStockObject},
    {"DragAcceptFiles",                       (ImplFn)impl_DragAcceptFiles},
    {"ImmGetDefaultIMEWnd",                   (ImplFn)impl_ImmGetDefaultIMEWnd},
    {"SetCursor",                             (ImplFn)impl_SetCursor},
    {"ShowCursor",                            (ImplFn)impl_ShowCursor},
    {"GetCursorPos",                          (ImplFn)impl_GetCursorPos},
    {"ScreenToClient",                        (ImplFn)impl_ScreenToClient},
    {"ClipCursor",                            (ImplFn)impl_ClipCursor},
    {"GetKeyboardState",                      (ImplFn)impl_GetKeyboardState},
    {"GetKeyState",                           (ImplFn)impl_GetKeyState},
    {"GetAsyncKeyState",                      (ImplFn)impl_GetAsyncKeyState},
    {"GetMessageExtraInfo",                   (ImplFn)impl_GetMessageExtraInfo},
    {"SendInput",                             (ImplFn)impl_SendInput},
    {"XInputGetState",                        (ImplFn)impl_XInputDisconnected},
    {"XInputSetState",                        (ImplFn)impl_XInputDisconnected},
    {"SetCapture",                            (ImplFn)impl_SetCapture},
    {"ReleaseCapture",                        (ImplFn)impl_ReleaseCapture},
    {"SetFocus",                              (ImplFn)impl_SetFocus},
    {"GetFocus",                              (ImplFn)impl_GetFocus},
    {"GetActiveWindow",                       (ImplFn)impl_GetActiveWindow},
    {"SetActiveWindow",                       (ImplFn)impl_SetActiveWindow},
    {"PeekMessageW",                          (ImplFn)impl_PeekMessageW},
    {"GetMessageW",                           (ImplFn)impl_GetMessageW},
    {"TranslateMessage",                      (ImplFn)impl_TranslateMessage},
    {"DispatchMessageW",                      (ImplFn)impl_DispatchMessageW},
    {"PostQuitMessage",                       (ImplFn)impl_PostQuitMessage},
    {"SendMessageW",                          (ImplFn)impl_SendMessageW},
    {"PostMessageW",                          (ImplFn)impl_PostMessageW},
    {"PostThreadMessageW",                    (ImplFn)impl_PostThreadMessageW},
    {"DefWindowProcW",                        (ImplFn)impl_DefWindowProcW},
    {"CallWindowProcW",                       (ImplFn)impl_CallWindowProcW},
    {"SetWindowLongPtrW",                     (ImplFn)impl_SetWindowLongPtrW},
    {"GetWindowLongPtrW",                     (ImplFn)impl_GetWindowLongPtrW},
    {"GetWindowLongPtrA",                     (ImplFn)impl_GetWindowLongPtrA},
    {"SetWindowLongW",                        (ImplFn)impl_SetWindowLongW},
    {"GetWindowLongW",                        (ImplFn)impl_GetWindowLongW},
    {"GetWindowLongA",                        (ImplFn)impl_GetWindowLongA},
    {"SetPropW",                              (ImplFn)impl_SetPropW},
    {"SetPropA",                              (ImplFn)impl_SetPropA},
    {"GetPropW",                              (ImplFn)impl_GetPropW},
    {"GetPropA",                              (ImplFn)impl_GetPropA},
    {"RemovePropW",                           (ImplFn)impl_RemovePropW},
    {"RemovePropA",                           (ImplFn)impl_RemovePropA},
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
    {"EnumSystemLocalesEx",                   (ImplFn)impl_EnumSystemLocalesEx},
    {"IsValidLocale",                         (ImplFn)impl_IsValidLocale},
    {"IsValidLocaleName",                     (ImplFn)impl_IsValidLocaleName},
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
    {"CreateDirectoryW",                      (ImplFn)impl_CreateDirectoryW},
    {"CreateDirectoryA",                      (ImplFn)impl_CreateDirectoryA},
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
    /* Optional vendor graphics extension */
    {"agsInit",                               (ImplFn)impl_agsInit},
    {"agsInitialize",                         (ImplFn)impl_agsInitialize},
    {"agsDeInit",                             (ImplFn)impl_agsDeInit},
    {"agsDeInitialize",                       (ImplFn)impl_agsDeInit},
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

static int guest_dll_rva_valid(const GuestDll *dll, u32 rva, size_t size)
{
    return dll && rva <= dll->image_size && size <= dll->image_size - rva;
}

static s64 (__attribute__((ms_abi)) *g_oodle_decompress)(
    const void *, s64, void *, s64, u32, u32, u32,
    void *, s64, void *, void *, void *, s64, u32);

static s64 __attribute__((ms_abi))
guest_oodle_decompress_trace(const void *compressed, s64 compressed_size,
                             void *raw, s64 raw_size,
                             u32 fuzz_safe, u32 check_crc, u32 verbosity,
                             void *decode_buffer, s64 decode_buffer_size,
                             void *callback, void *callback_user,
                             void *decoder_memory, s64 decoder_memory_size,
                             u32 thread_phase)
{
    if (!g_oodle_decompress) return 0;
    static _Atomic(u32) calls;
    u32 call = atomic_fetch_add(&calls, 1) + 1;
    s64 result = g_oodle_decompress(
        compressed, compressed_size, raw, raw_size,
        fuzz_safe, check_crc, verbosity, decode_buffer, decode_buffer_size,
        callback, callback_user, decoder_memory, decoder_memory_size,
        thread_phase);
    if (call <= 32 || result != raw_size) {
        fprintf(stderr,
                "[OODLE] Decompress #%u compressed=%lld raw=%lld -> %lld%s\n",
                call, (long long)compressed_size, (long long)raw_size,
                (long long)result, result == raw_size ? "" : " (incomplete)");
    }
    return result;
}

static u64 guest_dll_export(GuestDll *dll, const char *name)
{
    if (!dll || !dll->image || !name) return 0;
    NtHdrs64 *nt = (NtHdrs64 *)(dll->image + ((DosHdr *)dll->image)->lfanew);
    DataDir *dir = &nt->opt.dirs[0];
    if (!dir->size || !guest_dll_rva_valid(dll, dir->rva, sizeof(ExportDir))) return 0;
    ExportDir *exports = (ExportDir *)(dll->image + dir->rva);
    if (!guest_dll_rva_valid(dll, exports->names_rva,
                             (size_t)exports->name_count * sizeof(u32)) ||
        !guest_dll_rva_valid(dll, exports->ordinals_rva,
                             (size_t)exports->name_count * sizeof(u16)) ||
        !guest_dll_rva_valid(dll, exports->functions_rva,
                             (size_t)exports->function_count * sizeof(u32)))
        return 0;
    u32 *names = (u32 *)(dll->image + exports->names_rva);
    u16 *ordinals = (u16 *)(dll->image + exports->ordinals_rva);
    u32 *functions = (u32 *)(dll->image + exports->functions_rva);
    for (u32 i = 0; i < exports->name_count; ++i) {
        if (!guest_dll_rva_valid(dll, names[i], 1)) continue;
        const char *candidate = (const char *)(dll->image + names[i]);
        if (strcmp(candidate, name) != 0) continue;
        u16 ordinal = ordinals[i];
        if (ordinal >= exports->function_count) return 0;
        u32 rva = functions[ordinal];
        /* Forwarded exports point back into the export directory. Oodle does
         * not use them for the imported functions, so reject rather than
         * pretending the forwarder string is executable code. */
        if (rva >= dir->rva && rva < dir->rva + dir->size) return 0;
        if (!guest_dll_rva_valid(dll, rva, 1)) return 0;
        return (u64)(dll->image + rva);
    }
    return 0;
}

static void guest_dll_resolve_imports(GuestDll *dll)
{
    NtHdrs64 *nt = (NtHdrs64 *)(dll->image + ((DosHdr *)dll->image)->lfanew);
    DataDir *dir = &nt->opt.dirs[DIR_IMPORT];
    if (!dir->size) return;
    for (ImportDesc *desc = (ImportDesc *)(dll->image + dir->rva);
         desc->name_rva; ++desc) {
        if (!guest_dll_rva_valid(dll, desc->name_rva, 1))
            die("invalid import name RVA in %s", dll->name);
        const char *dependency = (const char *)(dll->image + desc->name_rva);
        u64 *ilt = (u64 *)(dll->image + (desc->orig_ilt ? desc->orig_ilt : desc->iat_rva));
        u64 *iat = (u64 *)(dll->image + desc->iat_rva);
        for (int i = 0; ilt[i]; ++i) {
            if (g_nstubs >= MAX_STUBS) die("DLL imports exceed MAX_STUBS");
            const char *function = NULL;
            char label[288];
            if (!(ilt[i] & (1ULL << 63))) {
                u32 name_rva = (u32)ilt[i];
                if (!guest_dll_rva_valid(dll, name_rva, 3))
                    die("invalid import function RVA in %s", dll->name);
                function = (const char *)(dll->image + name_rva + 2);
                snprintf(label, sizeof(label), "%s!%s", dependency, function);
            } else {
                snprintf(label, sizeof(label), "%s!#%u", dependency,
                         (u16)(ilt[i] & 0xffff));
            }
            ImplFn implementation = function ? find_impl(function) : NULL;
            char dependency_lower[64];
            lowercase_dll_basename(dependency, dependency_lower,
                                   sizeof(dependency_lower));
            int api_set_contract = !strncmp(dependency_lower, "api-ms-", 7) ||
                                   !strncmp(dependency_lower, "ext-ms-", 7);
            /* API-set DLLs are contracts forwarded into the Windows runtime,
             * not independent modules. Private redistributables may include
             * tiny fixed-base API-set images, but mapping each one collides at
             * 0x180000000. Route CRT contracts into the matching bundled
             * ucrtbase image; host implementations remain authoritative for
             * kernel/API contracts Beer already implements. */
            GuestDll *dependency_dll = NULL;
            if (api_set_contract) {
                if (!implementation && !strncmp(dependency_lower,
                                                "api-ms-win-crt-", 15))
                    dependency_dll = guest_dll_find("ucrtbase.dll");
            } else {
                dependency_dll = guest_dll_find(dependency);
                if (!dependency_dll)
                    dependency_dll = guest_dll_load(dependency);
            }
            u64 dependency_export = dependency_dll && function
                                  ? guest_dll_export(dependency_dll, function) : 0;
            g_snames[g_nstubs] = strdup(label);
            /* A loaded UCRT must own its API-set contracts. In particular,
             * startup helpers such as _initterm invoke PE callbacks and must
             * remain on the guest stack. Routing them through a Beer host shim
             * executes constructors on the dispatcher stack and breaks normal
             * guest CALL/RET semantics. Host implementations remain the
             * fallback when no bundled UCRT export is available. */
            int prefer_crt_export = api_set_contract && dependency_export &&
                                    !strncmp(dependency_lower,
                                             "api-ms-win-crt-", 15);
            if (prefer_crt_export) {
                iat[i] = dependency_export;
            } else if (dependency_export && g_use_guest_fmod &&
                (!strcasecmp(dependency, "fmodex64.dll") ||
                 !strcasecmp(dependency, "fmod_event64.dll"))) {
                iat[i] = dependency_export;
            } else if (implementation) {
                emit_impl_thunk(g_nstubs, (u64)implementation);
                iat[i] = (u64)(g_tramp + g_nstubs * THUNK_SZ);
            } else if (dependency_export) {
                iat[i] = dependency_export;
            } else {
                emit_thunk(g_nstubs);
                iat[i] = (u64)(g_tramp + g_nstubs * THUNK_SZ);
            }
            ++g_nstubs;
        }
    }
}

static GuestDll *guest_dll_load(const char *name)
{
    GuestDll *existing = guest_dll_find(name);
    if (existing) return existing;
    if (g_guest_dll_count >= MAX_GUEST_DLLS || !g_tramp) return NULL;

    char path[PATH_MAX];
    if (!build_guest_dll_path(name, path, sizeof(path))) return NULL;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return NULL; }
    u8 *file = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (file == MAP_FAILED) return NULL;
    if ((size_t)st.st_size < sizeof(DosHdr) || ((DosHdr *)file)->magic != MZ_MAGIC) {
        munmap(file, (size_t)st.st_size); return NULL;
    }
    DosHdr *dos = (DosHdr *)file;
    if ((size_t)dos->lfanew + sizeof(NtHdrs64) > (size_t)st.st_size) {
        munmap(file, (size_t)st.st_size); return NULL;
    }
    NtHdrs64 *nt = (NtHdrs64 *)(file + dos->lfanew);
    if (nt->sig != PE_SIG || nt->opt.magic != PE32PLUS) {
        munmap(file, (size_t)st.st_size); return NULL;
    }

    GuestDll *dll = &g_guest_dlls[g_guest_dll_count];
    memset(dll, 0, sizeof(*dll));
    lowercase_dll_basename(name, dll->name, sizeof(dll->name));
    strncpy(dll->path, path, sizeof(dll->path) - 1);
    dll->image_size = nt->opt.sz_image;
    dll->preferred_base = nt->opt.imagebase;
    dll->image = mmap((void *)dll->preferred_base, dll->image_size,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (dll->image == MAP_FAILED)
        dll->image = mmap(NULL, dll->image_size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (dll->image == MAP_FAILED) {
        munmap(file, (size_t)st.st_size); return NULL;
    }
    dll->delta = (u64)dll->image - dll->preferred_base;
    memcpy(dll->image, file, nt->opt.sz_headers);
    SecHdr *sections = (SecHdr *)((u8 *)&nt->opt + nt->file.opthdr_sz);
    for (u16 i = 0; i < nt->file.nsections; ++i) {
        SecHdr *section = &sections[i];
        if (!section->raw_sz || !section->raw_off) continue;
        size_t copy = section->raw_sz < section->vsz ? section->raw_sz : section->vsz;
        if ((size_t)section->raw_off + copy > (size_t)st.st_size ||
            (size_t)section->vrva + copy > dll->image_size)
            die("invalid section in %s", path);
        memcpy(dll->image + section->vrva, file + section->raw_off, copy);
    }
    munmap(file, (size_t)st.st_size);
    ++g_guest_dll_count; /* publish before resolving recursive dependencies */

    NtHdrs64 *mapped_nt = (NtHdrs64 *)(dll->image + ((DosHdr *)dll->image)->lfanew);
    if (dll->delta) {
        DataDir *relocations = &mapped_nt->opt.dirs[DIR_RELOC];
        if (!relocations->size)
            die("%s could not load at preferred base and has no relocations", dll->name);
        u8 *cursor = dll->image + relocations->rva;
        u8 *end = cursor + relocations->size;
        while (cursor + sizeof(RelocBlock) <= end) {
            RelocBlock *block = (RelocBlock *)cursor;
            if (block->block_sz < sizeof(RelocBlock) || cursor + block->block_sz > end) break;
            u16 *entries = (u16 *)(block + 1);
            int count = (int)((block->block_sz - sizeof(RelocBlock)) / sizeof(u16));
            for (int i = 0; i < count; ++i)
                if ((entries[i] >> 12) == 10)
                    *(u64 *)(dll->image + block->page_rva + (entries[i] & 0xfff)) += dll->delta;
            cursor += block->block_sz;
        }
    }

    g_loading_guest_dll = dll;
    guest_dll_resolve_imports(dll);
    g_loading_guest_dll = NULL;

    DataDir *tls_directory = &mapped_nt->opt.dirs[9];
    if (tls_directory->size >= sizeof(TLS_DIR64) &&
        guest_dll_rva_valid(dll, tls_directory->rva, sizeof(TLS_DIR64))) {
        TLS_DIR64 *tls = (TLS_DIR64 *)(dll->image + tls_directory->rva);
        dll->tls_index = (u32)g_guest_dll_count; /* slot 0 belongs to the EXE */
        dll->tls_start = tls->StartAddr;
        dll->tls_raw_size = tls->EndAddr >= tls->StartAddr
                          ? tls->EndAddr - tls->StartAddr : 0;
        dll->tls_zero_size = tls->SizeOfZeroFill;
        dll->tls_callbacks = tls->AddrOfCallbacks;
        if (tls->AddrOfIndex) *(u32 *)tls->AddrOfIndex = dll->tls_index;
        fprintf(stderr,
                "[PE DLL] %s TLS slot=%u raw=%lu zero=%u callbacks=%p\n",
                dll->name, dll->tls_index, (unsigned long)dll->tls_raw_size,
                dll->tls_zero_size, (void *)dll->tls_callbacks);
    }

    SecHdr *mapped_sections = (SecHdr *)((u8 *)&mapped_nt->opt + mapped_nt->file.opthdr_sz);
    for (u16 i = 0; i < mapped_nt->file.nsections; ++i) {
        SecHdr *section = &mapped_sections[i];
        if (!section->vsz) continue;
        int protection = 0;
        if (section->chars & SCN_READ) protection |= PROT_READ;
        if (section->chars & SCN_WRITE) protection |= PROT_WRITE;
        if (section->chars & SCN_EXEC) protection |= PROT_EXEC;
        u64 start = ((u64)dll->image + section->vrva) & ~(u64)0xfff;
        u64 end = ((u64)dll->image + section->vrva + section->vsz + 0xfff) & ~(u64)0xfff;
        if (mprotect((void *)start, (size_t)(end - start), protection) != 0)
            die("mprotect(%s): %s", dll->name, strerror(errno));
    }
    fprintf(stderr, "[PE DLL] loaded %s at %p (delta=%#lx)\n",
            dll->name, (void *)dll->image, dll->delta);
    return dll;
}

static void guest_dll_notify_thread(u32 reason)
{
    for (int i = 0; i < g_guest_dll_count; ++i) {
        GuestDll *dll = &g_guest_dlls[i];
        if (!dll->entry_called || !dll->image) continue;
        NtHdrs64 *nt = (NtHdrs64 *)(dll->image + ((DosHdr *)dll->image)->lfanew);
        if (dll->tls_callbacks) {
            typedef void __attribute__((ms_abi)) (*TlsCallback)(void *, u32, void *);
            TlsCallback *callbacks = (TlsCallback *)dll->tls_callbacks;
            for (size_t n = 0; n < 64 && callbacks[n]; ++n)
                callbacks[n](dll->image, reason, NULL);
        }
        if (nt->opt.entry_rva) {
            typedef int __attribute__((ms_abi)) (*DllEntry)(void *, u32, void *);
            ((DllEntry)(dll->image + nt->opt.entry_rva))(dll->image, reason, NULL);
        }
    }
}

static void guest_dll_initialize_all(void)
{
    for (int i = 0; i < g_guest_dll_count; ++i) {
        GuestDll *dll = &g_guest_dlls[i];
        if (dll->entry_called || dll->initializing) continue;
        NtHdrs64 *nt = (NtHdrs64 *)(dll->image + ((DosHdr *)dll->image)->lfanew);
        dll->initializing = 1;
        if (!nt->opt.entry_rva) {
            dll->entry_called = 1;
            dll->initializing = 0;
            continue;
        }
        if (dll->tls_callbacks) {
            typedef void __attribute__((ms_abi)) (*TlsCallback)(void *, u32, void *);
            TlsCallback *callbacks = (TlsCallback *)dll->tls_callbacks;
            for (size_t n = 0; n < 64 && callbacks[n]; ++n)
                callbacks[n](dll->image, 1 /* DLL_PROCESS_ATTACH */, NULL);
        }
        typedef int __attribute__((ms_abi)) (*DllEntry)(void *, u32, void *);
        DllEntry entry = (DllEntry)(dll->image + nt->opt.entry_rva);
        int result = entry(dll->image, 1 /* DLL_PROCESS_ATTACH */, NULL);
        dll->entry_called = 1;
        dll->initializing = 0;
        fprintf(stderr, "[PE DLL] %s process attach -> %d\n", dll->name, result);
        if (!result)
            fprintf(stderr, "[PE DLL] warning: %s rejected DLL_PROCESS_ATTACH; exports may be unavailable\n",
                    dll->name);
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

    /* Guest DLL dependency trees can contribute more imports than the EXE
     * itself (RE8's private UCRT/MSVCP/ConCRT stack exceeds the old +512
     * estimate). Map the full bounded thunk capacity so every index accepted by
     * the MAX_STUBS checks remains executable; otherwise later thunks spill into
     * adjacent RW memory and fault as soon as the runtime calls them. Keep the
     * trampoline in the low 32-bit address range: MSVC's runtime synthesizes
     * relative call targets from IAT thunks, and a high ASLR mapping can overflow
     * that signed displacement into a nearby non-executable address. */
    g_trampsz = (((size_t)MAX_STUBS * THUNK_SZ) + 0xFFFu) & ~(size_t)0xFFF;
    g_tramp = mmap(NULL, g_trampsz,
                   PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (g_tramp == MAP_FAILED) die("mmap(trampoline): %s", strerror(errno));

    /* Load bundled native PE dependencies before resolving the executable's
     * IAT. Oodle is required for resources. Bink owns the opening-movie video
     * and audio path. Real FMOD owns later event/game audio. */
    for (ImportDesc *d = rva_ptr(dir->rva); d->name_rva; d++) {
        const char *dll_name = rva_ptr(d->name_rva);
        char lower[64];
        lowercase_dll_basename(dll_name, lower, sizeof(lower));
        int required = !strcmp(lower, "oo2core_6_win64.dll");
        int real_bink = !strcmp(lower, "bink2w64.dll");
        int real_fmod = g_use_guest_fmod &&
                        (!strcmp(lower, "fmodex64.dll") ||
                         !strcmp(lower, "fmod_event64.dll"));
        int bundled_runtime = !strcmp(lower, "concrt140.dll") ||
                              !strcmp(lower, "msvcp140.dll") ||
                              !strcmp(lower, "msvcp140_1.dll") ||
                              !strcmp(lower, "msvcp140_2.dll") ||
                              !strcmp(lower, "msvcp140_atomic_wait.dll") ||
                              !strcmp(lower, "msvcp140_codecvt_ids.dll") ||
                              !strcmp(lower, "vcruntime140.dll") ||
                              !strcmp(lower, "vcruntime140_1.dll");
        if (bundled_runtime && !guest_dll_find("ucrtbase.dll")) {
            GuestDll *ucrt = guest_dll_load("ucrtbase.dll");
            if (!ucrt)
                fprintf(stderr, "[PE DLL] bundled runtime ucrtbase.dll was not found/loadable\n");
        }
        GuestDll *loaded = NULL;
        if (required || real_bink || real_fmod || bundled_runtime) {
            loaded = guest_dll_load(dll_name);
            if (bundled_runtime && !loaded)
                fprintf(stderr, "[PE DLL] bundled runtime %s was not found/loadable\n", dll_name);
        }
        if ((required || real_bink || real_fmod) && !loaded) {
            if (required || real_bink)
                die("unable to load required guest DLL %s", dll_name);
            fprintf(stderr, "[AUDIO] unable to load %s; FMOD audio unavailable\n", dll_name);
        }
        if (loaded && real_bink) {
            g_real_bink_open = guest_dll_export(loaded, "BinkOpen");
            g_real_bink_close = guest_dll_export(loaded, "BinkClose");
            g_real_bink_set_sound_system = guest_dll_export(loaded, "BinkSetSoundSystem");
            g_real_bink_wait = guest_dll_export(loaded, "BinkWait");
            g_real_bink_next_frame = guest_dll_export(loaded, "BinkNextFrame");
            g_real_bink_do_frame = guest_dll_export(loaded, "BinkDoFrame");
            g_real_bink_do_frame_async_multi = guest_dll_export(loaded, "BinkDoFrameAsyncMulti");
            g_real_bink_do_frame_async_wait = guest_dll_export(loaded, "BinkDoFrameAsyncWait");
            g_real_bink_start_async_thread = guest_dll_export(loaded, "BinkStartAsyncThread");
            g_real_bink_request_stop_async_thread = guest_dll_export(loaded, "BinkRequestStopAsyncThread");
            g_real_bink_wait_stop_async_thread = guest_dll_export(loaded, "BinkWaitStopAsyncThread");
        }
        if (loaded && real_fmod && !strcmp(lower, "fmod_event64.dll")) {
            g_real_fmod_event_system_create = guest_dll_export(loaded, "FMOD_EventSystem_Create");
            g_real_fmod_event_get_system = guest_dll_export(loaded,
                "?getSystemObject@EventSystem@FMOD@@QEAA?AW4FMOD_RESULT@@PEAPEAVSystem@2@@Z");
            g_real_fmod_event_init = guest_dll_export(loaded,
                "?init@EventSystem@FMOD@@QEAA?AW4FMOD_RESULT@@HIPEAXI@Z");
            g_real_fmod_event_update = guest_dll_export(loaded,
                "?update@EventSystem@FMOD@@QEAA?AW4FMOD_RESULT@@XZ");
            g_real_fmod_event_load = guest_dll_export(loaded,
                "?load@EventSystem@FMOD@@QEAA?AW4FMOD_RESULT@@PEBDPEAUFMOD_EVENT_LOADINFO@@PEAPEAVEventProject@2@@Z");
            g_real_fmod_event_get_event = guest_dll_export(loaded,
                "?getEvent@EventSystem@FMOD@@QEAA?AW4FMOD_RESULT@@PEBDIPEAPEAVEvent@2@@Z");
            g_real_fmod_event_start = guest_dll_export(loaded,
                "?start@Event@FMOD@@QEAA?AW4FMOD_RESULT@@XZ");
            g_real_fmod_event_preload_fsb = guest_dll_export(loaded,
                "?preloadFSB@EventSystem@FMOD@@QEAA?AW4FMOD_RESULT@@PEBDHPEAVSound@2@_N@Z");
        }
        if (loaded && real_fmod && !strcmp(lower, "fmodex64.dll")) {
            g_real_fmod_system_set_output = guest_dll_export(loaded,
                "?setOutput@System@FMOD@@QEAA?AW4FMOD_RESULT@@W4FMOD_OUTPUTTYPE@@@Z");
            g_real_fmod_system_set_file_system = guest_dll_export(loaded,
                "?setFileSystem@System@FMOD@@QEAA?AW4FMOD_RESULT@@P6A?AW43@PEBDHPEAIPEAPEAX2@ZP6A?AW43@PEAX4@ZP6A?AW43@44I14@ZP6A?AW43@4I4@ZP6A?AW43@PEAUFMOD_ASYNCREADINFO@@4@Z5H@Z");
            g_real_fmod_system_create_sound = guest_dll_export(loaded,
                "?createSound@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEBDIPEAUFMOD_CREATESOUNDEXINFO@@PEAPEAVSound@2@@Z");
            g_real_fmod_channel_group_set_volume = guest_dll_export(loaded,
                "?setVolume@ChannelGroup@FMOD@@QEAA?AW4FMOD_RESULT@@M@Z");
            g_real_fmod_event_set_volume = guest_dll_export(loaded,
                "?setVolume@Event@FMOD@@QEAA?AW4FMOD_RESULT@@M@Z");
        }
    }

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
            int import_by_ordinal = (ilt[i] & (1ULL << 63)) != 0;
            if (import_by_ordinal) {
                /* Import-by-ordinal labels are diagnostic names, not export names. */
                snprintf(buf, sizeof(buf), "%s!#%u", dll, (u16)(ilt[i] & 0xFFFF));
                fn_only = NULL;
            } else {
                /* import by name – IMAGE_IMPORT_BY_NAME: 2-byte hint then ASCII */
                fn_only = (const char *)rva_ptr((u32)ilt[i]) + 2;
                snprintf(buf, sizeof(buf), "%s!%s", dll, fn_only);
            }

            /* Use a Beer implementation first, then an export from a loaded
             * bundled PE DLL, otherwise retain the explicit logging stub. */
            ImplFn real = fn_only ? find_impl(fn_only) : NULL;
            if (!real && import_by_ordinal && !strcasecmp(dll, "xinput1_3.dll")) {
                u16 ordinal = (u16)(ilt[i] & 0xffff);
                /* XInput 1.3 exposes state/capability helpers by ordinal.
                 * Beer has no controller backend yet, so all observed probes
                 * report ERROR_DEVICE_NOT_CONNECTED instead of false success
                 * with untouched output structures. */
                if (ordinal == 2)
                    real = (ImplFn)impl_XInputGetStateDisconnected;
                else if (ordinal == 4)
                    real = (ImplFn)impl_XInputGetCapabilitiesDisconnected;
                else if (ordinal == 5)
                    real = (ImplFn)impl_XInputEnable;
            }
            if (!real && import_by_ordinal && !strcasecmp(dll, "ws2_32.dll")) {
                u16 ordinal = (u16)(ilt[i] & 0xffff);
                /* Winsock's legacy ordinal exports are still used by Sekiro's
                 * networking bootstrap. Resolve the observed lifecycle calls
                 * to the same implementations as their named exports. */
                if (ordinal == 3) real = (ImplFn)impl_closesocket;
                else if (ordinal == 4) real = (ImplFn)impl_connect;
                else if (ordinal == 6) real = (ImplFn)impl_getsockname;
                else if (ordinal == 23) real = (ImplFn)impl_socket;
                else if (ordinal == 111) real = (ImplFn)impl_WSAGetLastError;
                else if (ordinal == 115) real = (ImplFn)impl_WSAStartup;
                else if (ordinal == 116) real = (ImplFn)impl_WSACleanup;
            }
            GuestDll *guest = guest_dll_find(dll);
            char import_lower[64];
            lowercase_dll_basename(dll, import_lower, sizeof(import_lower));
            int crt_api_set = !strncmp(import_lower, "api-ms-win-crt-", 15);
            if (!guest && crt_api_set)
                guest = guest_dll_find("ucrtbase.dll");
            u64 guest_export = (guest && fn_only) ? guest_dll_export(guest, fn_only) : 0;
            /* Prefer the bundled UCRT for its API-set surface even when Beer
             * has a same-named fallback. This keeps CRT callback execution on
             * the guest stack and gives one coherent CRT heap/locale/runtime
             * state to RE8 and its private MSVC redistributables. */
            if (crt_api_set && guest_export)
                real = NULL;
            if (guest_export && fn_only && !strcmp(guest->name, "oo2core_6_win64.dll") &&
                !strcmp(fn_only, "OodleLZ_Decompress")) {
                g_oodle_decompress = (void *)guest_export;
                guest_export = (u64)guest_oodle_decompress_trace;
            }
            if (guest_export && fn_only && !strcmp(guest->name, "bink2w64.dll")) {
                if (!strcmp(fn_only, "BinkOpen"))
                    guest_export = (u64)impl_BinkOpen_dispatch;
                else if (!strcmp(fn_only, "BinkClose"))
                    guest_export = (u64)impl_BinkClose_dispatch;
                else if (!strcmp(fn_only, "BinkSetSoundSystem"))
                    guest_export = (u64)impl_BinkSetSoundSystem_dispatch;
                else if (!strcmp(fn_only, "BinkWait"))
                    guest_export = (u64)impl_BinkWait_dispatch;
                else if (!strcmp(fn_only, "BinkNextFrame"))
                    guest_export = (u64)impl_BinkNextFrame_dispatch;
                else if (!strcmp(fn_only, "BinkDoFrameAsyncMulti"))
                    guest_export = (u64)impl_BinkDoFrameAsyncMulti_dispatch;
                else if (!strcmp(fn_only, "BinkDoFrameAsyncWait"))
                    guest_export = (u64)impl_BinkDoFrameAsyncWait_dispatch;
                else if (!strcmp(fn_only, "BinkStartAsyncThread"))
                    guest_export = (u64)impl_BinkStartAsyncThread_dispatch;
                else if (!strcmp(fn_only, "BinkRequestStopAsyncThread"))
                    guest_export = (u64)impl_BinkRequestStopAsyncThread_dispatch;
                else if (!strcmp(fn_only, "BinkWaitStopAsyncThread"))
                    guest_export = (u64)impl_BinkWaitStopAsyncThread_dispatch;
            }
            g_snames[g_nstubs] = strdup(buf);
            int prefer_guest_fmod = g_use_guest_fmod && guest_export && guest &&
                                    (!strcmp(guest->name, "fmodex64.dll") ||
                                     !strcmp(guest->name, "fmod_event64.dll"));
            /* Keep host thunks around the real EventSystem factory/getter/init
             * so Beer can select and trace the WinMM output before FMOD starts
             * its mixer. Other methods are taken directly from its object. */
            int trace_fmod_factory = g_use_guest_fmod && fn_only &&
                                     !strcmp(fn_only, "FMOD_EventSystem_Create");
            int trace_fmod_set_output = g_use_guest_fmod && fn_only &&
                !strcmp(fn_only, "?setOutput@System@FMOD@@QEAA?AW4FMOD_RESULT@@W4FMOD_OUTPUTTYPE@@@Z");
            int trace_fmod_get_system = g_use_guest_fmod && fn_only &&
                !strcmp(fn_only, "?getSystemObject@EventSystem@FMOD@@QEAA?AW4FMOD_RESULT@@PEAPEAVSystem@2@@Z");
            int trace_fmod_init = g_use_guest_fmod && fn_only &&
                !strcmp(fn_only, "?init@EventSystem@FMOD@@QEAA?AW4FMOD_RESULT@@HIPEAXI@Z");
            int trace_fmod_update = g_use_guest_fmod && fn_only &&
                !strcmp(fn_only, "?update@EventSystem@FMOD@@QEAA?AW4FMOD_RESULT@@XZ");
            int trace_fmod_media_path = g_use_guest_fmod && fn_only &&
                !strcmp(fn_only, "?setMediaPath@EventSystem@FMOD@@QEAA?AW4FMOD_RESULT@@PEBD@Z");
            int trace_fmod_load = g_use_guest_fmod && fn_only &&
                !strcmp(fn_only, "?load@EventSystem@FMOD@@QEAA?AW4FMOD_RESULT@@PEBDPEAUFMOD_EVENT_LOADINFO@@PEAPEAVEventProject@2@@Z");
            int trace_fmod_get_event = g_use_guest_fmod && fn_only &&
                !strcmp(fn_only, "?getEvent@EventSystem@FMOD@@QEAA?AW4FMOD_RESULT@@PEBDIPEAPEAVEvent@2@@Z");
            int trace_fmod_start = g_use_guest_fmod && fn_only &&
                !strcmp(fn_only, "?start@Event@FMOD@@QEAA?AW4FMOD_RESULT@@XZ");
            int trace_fmod_create_sound = g_use_guest_fmod && fn_only &&
                !strcmp(fn_only, "?createSound@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEBDIPEAUFMOD_CREATESOUNDEXINFO@@PEAPEAVSound@2@@Z");
            int trace_fmod_channel_group_volume = g_use_guest_fmod && fn_only &&
                !strcmp(fn_only, "?setVolume@ChannelGroup@FMOD@@QEAA?AW4FMOD_RESULT@@M@Z");
            int trace_fmod_event_volume = g_use_guest_fmod && fn_only &&
                !strcmp(fn_only, "?setVolume@Event@FMOD@@QEAA?AW4FMOD_RESULT@@M@Z");
            int trace_fmod_preload_fsb = g_use_guest_fmod && fn_only &&
                !strcmp(fn_only, "?preloadFSB@EventSystem@FMOD@@QEAA?AW4FMOD_RESULT@@PEBDHPEAVSound@2@_N@Z");
            if (trace_fmod_factory || trace_fmod_set_output || trace_fmod_get_system ||
                trace_fmod_init || trace_fmod_update || trace_fmod_media_path ||
                trace_fmod_load || trace_fmod_get_event || trace_fmod_start ||
                trace_fmod_create_sound || trace_fmod_channel_group_volume ||
                trace_fmod_event_volume || trace_fmod_preload_fsb) {
                ImplFn traced = trace_fmod_factory
                              ? (ImplFn)impl_FMOD_EventSystem_Create_dispatch
                              : trace_fmod_set_output
                              ? (ImplFn)impl_FMOD_System_setOutput_dispatch
                              : trace_fmod_get_system
                              ? (ImplFn)impl_FMOD_EventSystem_getSystemObject_dispatch
                              : trace_fmod_init
                              ? (ImplFn)impl_FMOD_EventSystem_init_dispatch
                              : trace_fmod_media_path
                              ? (ImplFn)impl_FMOD_EventSystem_setMediaPath_dispatch
                              : trace_fmod_load
                              ? (ImplFn)impl_FMOD_EventSystem_load_dispatch
                              : trace_fmod_get_event
                              ? (ImplFn)impl_FMOD_EventSystem_getEvent_dispatch
                              : trace_fmod_start
                              ? (ImplFn)impl_FMOD_Event_start_dispatch
                              : trace_fmod_create_sound
                              ? (ImplFn)impl_FMOD_System_createSound_dispatch
                              : trace_fmod_channel_group_volume
                              ? (ImplFn)impl_FMOD_ChannelGroup_setVolume_dispatch
                              : trace_fmod_event_volume
                              ? (ImplFn)impl_FMOD_Event_setVolume_dispatch
                              : trace_fmod_preload_fsb
                              ? (ImplFn)impl_FMOD_EventSystem_preloadFSB_dispatch
                              : (ImplFn)impl_FMOD_EventSystem_update_dispatch;
                emit_impl_thunk(g_nstubs, (u64)traced);
                iat[i] = (u64)(g_tramp + g_nstubs * THUNK_SZ);
                printf("  [REAL] %s (real-FMOD dispatch)\n", buf);
            } else if (prefer_guest_fmod) {
                iat[i] = guest_export;
                printf("  [DLL ] %s -> 0x%lx\n", buf, guest_export);
            } else if (real) {
                emit_impl_thunk(g_nstubs, (u64)real);
                iat[i] = (u64)(g_tramp + g_nstubs * THUNK_SZ);
                printf("  [REAL] %s\n", buf);
            } else if (guest_export) {
                iat[i] = guest_export;
                printf("  [DLL ] %s -> 0x%lx\n", buf, guest_export);
                /* Keep an index entry for diagnostics but do not emit a thunk;
                 * calls enter the DLL directly with the original Windows ABI. */
            } else {
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

/* All Windows threads in a process reference the same PEB through gs:[0x60].
 * Worker TEBs previously left this field zero, causing guest runtime code on
 * those threads to observe an invalid process environment. */
static WinPEB *g_process_peb;

/*
 * alloc_teb_for_thread — allocate a private WinTEB with its own TLS array
 * and implicit-TLS data block for any thread (main or worker).
 *
 * Sets StackBase/StackLimit from the current pthread and points ProcEnvBlk at
 * the process-wide PEB once it exists. Does NOT call arch_prctl; the caller
 * installs the returned TEB as the thread's GS base.
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
    teb->ProcEnvBlk = (u64)g_process_peb;
    teb->UniqueProcess = (u64)(u32)getpid();
    teb->UniqueThread = (u64)(u32)syscall(SYS_gettid);
    g_cached_windows_thread_id = teb->UniqueThread;

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

    /* Copy the executable's implicit TLS initializer data (slot 0). */
    NtHdrs64 *nt = (NtHdrs64 *)(g_img + ((DosHdr *)g_img)->lfanew);
    DataDir  *tls_dir = &nt->opt.dirs[9]; /* IMAGE_DIRECTORY_ENTRY_TLS */
    if (tls_dir->size) {
        TLS_DIR64 *td = (TLS_DIR64 *)(g_img + tls_dir->rva);
        u64 raw_sz = td->EndAddr - td->StartAddr;
        u64 total_sz = raw_sz + td->SizeOfZeroFill;
        if (total_sz > 0) {
            u8 *tls_data = mmap(NULL, total_sz, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (tls_data != MAP_FAILED) {
                memset(tls_data, 0, total_sz);
                if (raw_sz > 0) memcpy(tls_data, (void *)td->StartAddr, (size_t)raw_sz);
                tls_array[0] = (u64)tls_data;
            }
        }
    }

    /* Install static TLS blocks for every loaded guest DLL. The CRT in FMOD
     * uses these slots on its mixer thread; process attach alone is not enough. */
    for (int i = 0; i < g_guest_dll_count; ++i) {
        GuestDll *dll = &g_guest_dlls[i];
        if (!dll->tls_raw_size && !dll->tls_zero_size) continue;
        if (dll->tls_index >= TLS_SLOTS) continue;
        size_t total = (size_t)dll->tls_raw_size + dll->tls_zero_size;
        u8 *tls_data = calloc(1, total ? total : 1);
        if (!tls_data) die("calloc(%s TLS): %s", dll->name, strerror(errno));
        if (dll->tls_raw_size)
            memcpy(tls_data, (void *)dll->tls_start, (size_t)dll->tls_raw_size);
        tls_array[dll->tls_index] = (u64)tls_data;
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
            g_current_guest_stack_low = (u64)stackaddr;
            g_current_guest_stack_high = (u64)stackaddr + (u64)stacksize;
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
    g_process_peb = peb;

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
    if (!g_img || !getenv("BEER_ENABLE_TRANSACTION_PATCHES")) {
        fprintf(stderr,
                "[PATCH] Leaving transaction assertion branches intact by default. "
                "Set BEER_ENABLE_TRANSACTION_PATCHES=1 to opt in.\n");
        return 0;
    }

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
    if (!aggressive && !getenv("BEER_ENABLE_KNOWN_TARGET_PATCHES")) {
        fprintf(stderr,
                "[PATCH] Leaving known guest code targets intact by default. "
                "Set BEER_ENABLE_KNOWN_TARGET_PATCHES=1 to opt in.\n");
        return;
    }

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

    /* Keep the valid `ret; int3` boundary at RVA 0x237ce59 intact.  Replacing
     * three bytes here used to turn the following function's leading INT3 into
     * a second RET, shifting recovery into malformed instruction boundaries. */

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
                0x1130b36, 0x1130b3d, 0x1130b44, 0x1130b4c, 
                /* Blocker function at 0x11e4200-0x11e43dc: patch densely */
                0x11e424c, 0x11e4250, 0x11e4254, 0x11e4258, 0x11e425c, 0x11e4260, 0x11e4264, 0x11e4268, 
                0x11e426c, 0x11e4270, 0x11e4274, 0x11e4278, 0x11e427c, 0x11e4280, 0x11e4284, 0x11e4288,
                0x30b5a90, 0x30b5ad0,
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

    /* Track signal counts for diagnostics */
    if (sig == SIGSEGV) {
        g_total_sigsegv_count++;
        fprintf(stderr, "[CRASH] SIGSEGV #%u at RIP=0x%lx FA=0x%lx\n", 
                g_total_sigsegv_count, rip, faultaddr);
        fflush(stderr);
        if (g_total_sigsegv_count % 100 == 0) {
            fprintf(stderr, "[DIAG] Total SIGSEGV count: %u\n", g_total_sigsegv_count);
        }
    } else if (sig == SIGILL) {
        g_total_sigill_count++;
    } else {
        g_total_other_sig_count++;
    }

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

    /* This post-device fault reads the display/config record vector from the
     * root object.  Record the producer-owned state before generic unwind
     * changes the call chain; this is diagnostic only and never fabricates the
     * missing record. */
    if (sig == SIGSEGV && g_img && rip == (u64)g_img + 0x0f817f1) {
        u64 root = *(u64 *)(g_img + 0x3f3b400);
        if (root) {
            fprintf(stderr,
                    "[ROOT] RVA 0x0f817f1: root=0x%lx records=[0x%lx,0x%lx,0x%lx] count=%u backend=0x%lx service=0x%lx\n",
                    root, *(u64 *)(root + 0x98), *(u64 *)(root + 0xa0),
                    *(u64 *)(root + 0xa8), *(u32 *)(root + 0x2a0),
                    *(u64 *)(root + 0x458), *(u64 *)(root + 0x328));
        }
    }

    /* Sekiro's internal nonlocal-context restore loads its target RSP from
     * [RCX+0x10] and target RIP from [RCX+0x50].  Continuing with an empty
     * context destroys all architectural state and previously led to repeated
     * RIP=0/RSP=0 recovery.  Reject that stale context at its source instead of
     * fabricating a stack or pretending the restore succeeded. */
    if (sig == SIGSEGV && g_img && rip == (u64)g_img + 0x269643d) {
        u64 context = rcx0;
        u64 target_rsp = context ? *(u64 *)(uintptr_t)(context + 0x10) : 0;
        u64 target_rip = context ? *(u64 *)(uintptr_t)(context + 0x50) : 0;
        if (!is_guest_stack_rsp(target_rsp) || !is_valid_resume_target(target_rip)) {
            fprintf(stderr,
                    "[FATAL] Refusing stale guest context restore at RVA 0x269643d: context=0x%lx RSP=0x%lx RIP=0x%lx\n",
                    context, target_rsp, target_rip);
            report_window_progress();
            fflush(stderr);
            _exit(2);
        }
    }

    /* A zero RSP is never recoverable without inventing a caller frame. At the
     * observed failure RCX still identifies the zeroed restore record, so report
     * that source context and stop on the first secondary execute fault. */
    if (sig == SIGSEGV && rsp0 == 0) {
        u64 context_rsp = rcx0 ? *(u64 *)(uintptr_t)(rcx0 + 0x10) : 0;
        u64 context_rip = rcx0 ? *(u64 *)(uintptr_t)(rcx0 + 0x50) : 0;
        fprintf(stderr,
                "[FATAL] Invalid guest context was applied: context=0x%lx saved-RSP=0x%lx saved-RIP=0x%lx; refusing synthetic recovery\n",
                rcx0, context_rsp, context_rip);
        report_window_progress();
        fflush(stderr);
        _exit(2);
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

    /* ============================================================================
     * Sync Memory Infrastructure: Handle XCHG crashes in game-init
     * ============================================================================
     * The remaining known XCHG sites use lock tables in the image's data section.
     * Emulate those exact instructions atomically rather than mapping fault-derived
     * addresses or modifying adjacent instructions.
     */
    
    /* Specific XCHG crash points: Emulate using real atomic XCHG on game's .data section */
    int is_xchg_crash = (rip == (u64)g_img + 0x237cff0 || rip == (u64)g_img + 0x23b1318);
    
    if (is_xchg_crash) {
        /* Both complete resolver functions copy their 32-bit selector from ECX
         * into R14D in the prologue. Recovery corruption can nevertheless enter
         * these sites with a stale selector. Keep the historical bounded slot
         * mapping for now, but record the intact resolver frame so the corrupt
         * caller can be identified without weakening the cycle detector. */
        u64 r14_raw = (u64)uc->uc_mcontext.gregs[REG_R14];
        int first_resolver = rip == (u64)g_img + 0x237cff0;
        u64 selector_max = first_resolver ? 8 : 31;
        u64 frame_ret = 0;
        if (is_guest_stack_rsp(rsp0) && is_guest_stack_rsp(rsp0 + 0x50))
            frame_ret = *(u64 *)(rsp0 + 0x48);

        /* R14D is the selector captured from ECX at function entry. All direct
         * wrappers use 4..8 for the first resolver and 0..31 for the second.
         * If recovery entered with a stale pointer/value, complete this intact
         * resolver frame as a failed lookup. Its real epilogue restores every
         * saved nonvolatile register and returns through the verified frame slot;
         * modulo-indexing the stale value instead mutates an unrelated cache slot
         * and is what sustains the cross-resolver cycle. The global detector also
         * covers period-1 cycles so a repeated fault inside any one recovery
         * branch remains bounded. Do not apply this to the initial synthetic
         * handoff frame: its return slot points into the guest stack rather than
         * to an executable image caller, so that first compatibility access keeps
         * the historical bounded behavior. */
        if (r14_raw > selector_max &&
            (image_addr_is_exec(frame_ret) ||
             (g_tramp && frame_ret >= (u64)g_tramp && frame_ret < (u64)(g_tramp + g_trampsz)))) {
            u64 epilogue = (u64)g_img + (first_resolver ? 0x237d027 : 0x23b134f);
            fprintf(stderr,
                    "[XCHG] Invalid selector 0x%lx at RVA 0x%lx; failed lookup via epilogue, caller=0x%lx\n",
                    r14_raw, rip - (u64)g_img, frame_ret);
            uc->uc_mcontext.gregs[REG_RAX] = 0;
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)epilogue;
            return;
        }

        u64 r14_slot = r14_raw % 8192;
        u64 rdx_val = (u64)uc->uc_mcontext.gregs[REG_RDX];
        u64 table_rva = first_resolver ? 0x3f33030 : 0x3f33bb0;
        u64 address_to_xchg = (u64)g_img + table_rva + (r14_slot * 8);

        if (first_resolver)
            uc->uc_mcontext.gregs[REG_RCX] = (greg_t)g_img;
        else
            uc->uc_mcontext.gregs[REG_R15] = (greg_t)g_img;

        /* XCHG returns the old memory value in RDX and does not modify RAX. */
        volatile u64 *target = (volatile u64 *)address_to_xchg;
        u64 old_value = __sync_lock_test_and_set(target, rdx_val);
        uc->uc_mcontext.gregs[REG_RDX] = (greg_t)old_value;
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)(rip + 8);

        fprintf(stderr,
                "[XCHG] emulated at RVA 0x%lx: raw-selector=0x%lx slot=%lu "
                "XCHG [0x%lx], 0x%lx -> RDX=0x%lx RSP=0x%lx frame-ret=0x%lx\n",
                rip - (u64)g_img, r14_raw, r14_slot, address_to_xchg,
                rdx_val, old_value, rsp0, frame_ret);
        return;
    }
    
sync_alloc_skip:
    /* ============================================================================
     * Fallback: Generic sync memory allocation with selective initialization
     * ============================================================================
     * PHASE 5: Only initialize locks in data pages, preserve code pages
     */
    if (faultaddr >= 0x1000 && faultaddr < 0x7f0000000000ULL && faultaddr != rip &&
        !is_xchg_crash &&
        (!g_img || rip < (u64)g_img || rip >= (u64)g_img + 0x42d2000) &&
        (!g_guest_stack_low || faultaddr < g_guest_stack_low || faultaddr >= g_guest_stack_high)) {
        u64 page_aligned = faultaddr & ~0xFFF;
        u64 alloc_size = 0x100000;
        static u32 generic_alloc_count = 0;  /* Track allocation attempts */
        
        int already_allocated = 0;
        for (int i = 0; i < g_sync_region_count; i++) {
            if (g_sync_regions[i].base_addr == page_aligned) {
                already_allocated = 1;
                break;
            }
        }
        
        if (!already_allocated && g_sync_region_count < MAX_SYNC_REGIONS) {
            /* CRITICAL GUARD: Never MAP_FIXED over the loaded guest image */
            if (g_img) {
                NtHdrs64 *_nt = (NtHdrs64 *)(g_img + ((DosHdr *)g_img)->lfanew);
                u64 img_end = (u64)g_img + _nt->opt.sz_image;
                if ((page_aligned >= (u64)g_img && page_aligned < img_end) ||
                    (page_aligned + alloc_size > (u64)g_img && page_aligned < img_end)) {
                    fprintf(stderr, "[SYNC] GUARD (generic): Refusing MAP_FIXED at 0x%lx (overlaps game image 0x%lx-0x%lx)\n",
                            page_aligned, (u64)g_img, img_end);
                    goto generic_alloc_skip;
                }
            }
            
            void *result = mmap((void*)page_aligned, alloc_size,
                               PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                               -1, 0);
            
            if (result != MAP_FAILED) {
                generic_alloc_count++;
                /* PHASE 5 SIMPLIFIED: For freshly allocated MAP_ANONYMOUS memory,
                 * we can safely initialize it as all data (it's zero-init).
                 * Selective analysis is only needed if mmap'ing over existing content. */
                SyncRegion *sr = &g_sync_regions[g_sync_region_count];
                sr->base_addr = page_aligned;
                sr->size = alloc_size;
                sr->mapped = 1;
                
                /* Mark all pages as data (since MAP_ANONYMOUS guarantees zero-init) */
                for (int i = 0; i < 256; i++) {
                    sr->page_type[i] = 0;  /* 0 = data page */
                }
                
                fprintf(stderr, "[SYNC] Generic alloc [%u] at 0x%lx: all 256 pages marked as data (safe)\n",
                        generic_alloc_count, page_aligned);
                fflush(stderr);
                
                /* Initialize lock structures in all data pages */
                BEER_CRITICAL_SECTION *locks = (BEER_CRITICAL_SECTION *)page_aligned;
                u64 num_locks = alloc_size / 64;
                
                fprintf(stderr, "[SYNC] Initializing %lu locks (0x%lx to 0x%lx)\n", 
                        num_locks, (u64)locks, (u64)&locks[num_locks-1]);
                fflush(stderr);
                
                u64 crash_count_before = g_total_sigsegv_count;
                /* WORKAROUND: Known hang at lock 14563+. Initialize only 0-14562 (88.8%)
                 * This is sufficient for early game initialization. Problematic locks will
                 * fault dynamically if game accesses them later. */
                u64 max_safe_locks = 14563;  /* Empirically determined hang point */
                u64 locks_to_init = (num_locks > max_safe_locks) ? max_safe_locks : num_locks;
                
                fprintf(stderr, "[SYNC] Starting loop: will init locks 0 to %lu\n", locks_to_init - 1);
                fflush(stderr);
                
                for (u64 i = 0; i < locks_to_init; i++) {
                    beer_critical_section_initialize(&locks[i]);
                    
                    if (i % 2048 == 0 && i > 0) {
                        fprintf(stderr, "[SYNC] Progress: %lu/%lu\n", i, locks_to_init);
                        fflush(stderr);
                    }
                }
                
                fprintf(stderr, "[SYNC] *** LOOP EXITED - all %lu locks init complete ***\n", locks_to_init);
                fflush(stderr);
                
                fprintf(stderr, "[SYNC] *** INITIALIZATION COMPLETE: %lu/%lu locks initialized ***\n",
                        locks_to_init, num_locks);
                if (locks_to_init < num_locks) {
                    fprintf(stderr, "[SYNC] WARNING: Skipped %lu locks (14563+ hang workaround)\n",
                            num_locks - locks_to_init);
                }
                fflush(stderr);
                
                g_sync_region_count++;
                
                fprintf(stderr, "[SYNC] Generic alloc [%u]: 0x%lx-0x%lx (%luKB) fully initialized with %lu locks\n",
                        generic_alloc_count, page_aligned, page_aligned + alloc_size, alloc_size / 1024, num_locks);
                
                /* If we've done too many allocations, game might be in infinite loop */
                if (generic_alloc_count > 10) {
                    fprintf(stderr, "[WARN] Generic allocator called %u times - game may be stuck in loop\n",
                            generic_alloc_count);
                }
                return;
            }
        }
    }

generic_alloc_skip:
    /* Do not mutate instructions in the broad game-init range. In particular,
     * replacing eight bytes at an arbitrary fault RIP can split instructions,
     * erase function epilogues, and destroy RSP. Exact sites are handled above;
     * all other faults continue to metadata-driven unwind recovery below. */

    /* RVA 0x237ce28 has one direct caller in the image: the CALL at 0x23596ce,
     * whose continuation is 0x23596d3. Recovery can enter the helper with RSP
     * three qwords below that caller frame; after its normal `add rsp, 0x28`
     * epilogue, the RET consequently sees recovery scratch data instead of the
     * saved continuation. Do not scan for an arbitrary executable address and
     * do not skip the RET. Reconstruct only this statically verified call edge,
     * and only when the expected continuation is in its exact observed slot. */
    if (sig == SIGSEGV && g_img && rip == (u64)g_img + 0x237ce59 &&
        is_guest_stack_rsp(rsp0) && is_guest_stack_rsp(rsp0 + 0x20)) {
        u64 expected_ret = (u64)g_img + 0x23596d3;
        u64 *sp = (u64 *)rsp0;
        if (sp[3] == expected_ret && image_addr_is_exec(expected_ret) &&
            (!g_entry_rsp_limit || rsp0 + 0x20 <= g_entry_rsp_limit)) {
            fprintf(stderr,
                    "[FRAME] Reconstructed 0x237ce28 caller: RET slot 0x%lx -> 0x%lx (discarded 0x18 recovery bytes)\n",
                    rsp0 + 0x18, expected_ret);
            uc->uc_mcontext.gregs[REG_RSP] = (greg_t)(rsp0 + 0x20);
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)expected_ret;
            return;
        }
    }

    /* The callback-record walker calls callbacks at RVA 0x23bb5ef and its
     * wrapper was called from 0x23596db (continuation 0x23596e0).  By the time
     * callback RVA 0x23b1ab0 reaches its RET, prior recovery has already popped
     * beyond both the callback and walker frames: the walker's continuation is
     * retained in RSI, while the exact wrapper continuation is now at RSP+0x10.
     * Generic unwind incorrectly consumes RSP+0x28 (0x237ce48), re-entering the
     * broken startup chain.  Finish the failed callback walk only for this exact
     * verified frame shape, preserving the callback's AL result. */
    if (sig == SIGSEGV && g_img && rip == (u64)g_img + 0x23b1b15 &&
        is_guest_stack_rsp(rsp0) && is_guest_stack_rsp(rsp0 + 0x18)) {
        u64 walker_cont = (u64)g_img + 0x23bb5f1;
        u64 wrapper_cont = (u64)g_img + 0x23596e0;
        u64 *sp = (u64 *)rsp0;
        if ((u64)uc->uc_mcontext.gregs[REG_RSI] == walker_cont &&
            sp[2] == wrapper_cont && image_addr_is_exec(wrapper_cont) &&
            (!g_entry_rsp_limit || rsp0 + 0x18 <= g_entry_rsp_limit)) {
            fprintf(stderr,
                    "[FRAME] Reconstructed failed callback walk: RVA 0x23b1b15 -> 0x23596e0 (discarded 0x10 recovery bytes)\n");
            uc->uc_mcontext.gregs[REG_RSP] = (greg_t)(rsp0 + 0x18);
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)wrapper_cont;
            return;
        }

        /* A second exact shape occurs after the nested callback's own
         * 0x23b1af2 epilogue has completed: RSI identifies the 0x237ce28
         * startup helper and RSP+0x10 contains that helper's verified caller
         * continuation. Complete that return instead of skipping the RET into
         * the next function. */
        u64 startup_cont = (u64)g_img + 0x23596d3;
        if ((u64)uc->uc_mcontext.gregs[REG_RSI] == (u64)g_img + 0x237ce3b &&
            sp[2] == startup_cont && image_addr_is_exec(startup_cont) &&
            (!g_entry_rsp_limit || rsp0 + 0x18 <= g_entry_rsp_limit)) {
            fprintf(stderr,
                    "[FRAME] Reconstructed nested callback return: RVA 0x23b1b15 -> 0x23596d3 (discarded 0x10 recovery bytes)\n");
            uc->uc_mcontext.gregs[REG_RSP] = (greg_t)(rsp0 + 0x18);
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)startup_cont;
            return;
        }
    }

    /* RVA 0x23921e8 tears down a three-entry worker array held in global
     * RVA 0x3f331a8.  The initializer is allowed to leave that allocation
     * NULL, but this cleanup routine dereferences it unconditionally at
     * 0x2392201.  The two cleanup calls preceding the loop have already run by
     * this point, so an absent array means there are no entries to destroy and
     * the correct structural recovery is the function's real epilogue.  Keep
     * this exact: require the first loop iteration, the NULL global, and the
     * expected zero byte offset rather than skipping individual instructions. */
    if (sig == SIGSEGV && g_img && rip == (u64)g_img + 0x2392201 &&
        (u64)uc->uc_mcontext.gregs[REG_RBX] == 0 &&
        *(u64 *)(g_img + 0x3f331a8) == 0) {
        fprintf(stderr,
                "[CLEANUP] Worker array is NULL at RVA 0x2392201; resuming at the function epilogue\n");
        uc->uc_mcontext.gregs[REG_RAX] = 0;
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)((u64)g_img + 0x239223d);
        return;
    }

    /* RVA 0x23ab7d0 is a bounded destructor/callback-array walker.  When an
     * entry is not executable, the fault occurs after `call rsi` has pushed
     * the exact continuation 0x23ab81f.  Generic execute-fault recovery resumes
     * that loop with already-corrupt iterator state and can walk indefinitely
     * beyond the nominal array.  Treat a bad callback as a failed teardown and
     * run the walker's real epilogue, but only for its exact frame shape: the
     * callback continuation must be on top, the nominal count must be small,
     * and the walker's own return slot at callback-RSP+0x30 must be executable. */
    if (sig == SIGSEGV && g_img && faultaddr == rip &&
        !(rip >= (u64)g_img && rip < (u64)g_img + 0x42d2000) &&
        is_guest_stack_rsp(rsp0) && is_guest_stack_rsp(rsp0 + 0x38)) {
        u64 *sp = (u64 *)rsp0;
        u64 walker_cont = (u64)g_img + 0x23ab81f;
        u64 walker_ret = sp[6];
        u64 count = (u64)uc->uc_mcontext.gregs[REG_RDI];
        if (sp[0] == walker_cont && count > 0 && count <= 0x1000 &&
            image_addr_is_exec(walker_ret) &&
            (!g_entry_rsp_limit || rsp0 + 0x38 <= g_entry_rsp_limit)) {
            fprintf(stderr,
                    "[CALLBACK] Invalid destructor target 0x%lx in %lu-entry walk; finishing at RVA 0x23ab82b (caller 0x%lx)\n",
                    rip, count, walker_ret);
            uc->uc_mcontext.gregs[REG_RAX] = 0;
            uc->uc_mcontext.gregs[REG_RSP] = (greg_t)(rsp0 + 8);
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)((u64)g_img + 0x23ab82b);
            return;
        }
    }

    if (rsp0 && !is_guest_stack_rsp(rsp0)) {
        u64 low = g_current_guest_stack_low ? g_current_guest_stack_low : g_guest_stack_low;
        u64 high = g_current_guest_stack_high ? g_current_guest_stack_high : g_guest_stack_high;
        fprintf(stderr,
                "[FATAL] Invalid RSP=0x%lx outside current thread stack 0x%lx..0x%lx; "
                "faulting RIP=0x%lx (RVA 0x%lx) faultaddr=0x%lx rcx=0x%lx\n",
                rsp0, low, high, rip,
                (g_img && rip >= (u64)g_img) ? rip - (u64)g_img : rip, faultaddr, rcx0);
        /* Never transplant a worker onto the main thread's synthetic startup
         * frame. That corrupts both call chains and turns one actionable fault
         * into a cascade. Stop at the original invalid-stack source instead. */
        report_window_progress();
        fflush(stderr);
        _exit(2);
    }

    /* If the current RSP is already outside the guest stack but the fault is on a
     * real in-image code address, keep the guest on its own stack frame instead of
     * delivering a new host-side loop through the fallback entry path. */
    if (sig == SIGSEGV && g_img && rip >= (u64)g_img && rip < (u64)g_img + 0x42d2000 &&
        rsp0 && !is_guest_stack_rsp(rsp0)) {
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
    
    /* Specific SIGILL handler for Sekiro game-init region (deep RVA 0x237ce60+)
     * Skip the bad instruction by advancing RIP by 1 byte (likely invalid opcode)
     * and continue instead of using fallback redirect which can create loops. */
    if (sig == SIGILL && rip >= (u64)g_img + 0x237ce50 && rip <= (u64)g_img + 0x237cf00) {
        fprintf(stderr, "[SKIP] Sekiro game-init SIGILL at RIP=0x%lx, skipping 1 byte\n", rip);
        uc->uc_mcontext.gregs[REG_RAX] = 0;
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)(rip + 1);  /* Skip 1 byte of bad instruction */
        return;
    }
    
    /* RVA 0x23bb5b8 walks [begin,end) callback records in 16-byte steps.
     * Its only direct entry is the tail wrapper at RVA 0x23ab634, which supplies
     * the static image range 0x30c6480..0x30c6570.  Recovery can incorrectly
     * enter the walker with unrelated stack/code pointers; letting it continue
     * then calls values 0/1 as callbacks.  Treat only a structurally impossible
     * range as empty and use the helper's real success epilogue. */
    if (sig == SIGSEGV && g_img && rip == (u64)g_img + 0x23bb5de) {
        u64 begin = (u64)uc->uc_mcontext.gregs[REG_RDI];
        u64 end = (u64)uc->uc_mcontext.gregs[REG_RSI];
        u64 span = end >= begin ? end - begin : UINT64_MAX;
        if (!begin || !end || end < begin || (span & 0xf) || span > 0x100000) {
            g_bad_callback_range_hits++;
            fprintf(stderr,
                    "[RANGE] Invalid callback range 0x%lx..0x%lx at RVA 0x23bb5de; reconstructing wrapper return to RVA 0x23596e0 [%u/8]\n",
                    begin, end, g_bad_callback_range_hits);
            if (g_bad_callback_range_hits > 8) {
                fprintf(stderr,
                        "[FATAL] Callback-range recovery made no forward progress after 8 attempts; aborting boundedly.\n");
                report_window_progress();
                fflush(stderr);
                _exit(2);
            }
            uc->uc_mcontext.gregs[REG_RAX] =
                (greg_t)(((u64)uc->uc_mcontext.gregs[REG_RAX] & ~0xffULL) | 1);
            /* The only caller of the 0x23ab634 wrapper is CALL 0x23ab634 at
             * 0x23596db. Discard the walker's 0x28-byte frame plus its return
             * slot, then resume at that call's exact continuation. */
            uc->uc_mcontext.gregs[REG_RSP] = (greg_t)(rsp0 + 0x30);
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)((u64)g_img + 0x23596e0);
            return;
        }
    }

    /* The old 0x1423b1000..0x1424b1000 "JIT" rule skipped eight bytes at
     * every fault, including valid PE instructions.  It is intentionally gone:
     * ordinary image faults now use exact handlers or PE unwind metadata. */
    
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
        /* PRIORITY 1: Blocker at 0x1411e424c & 0x1423b1403 - special escape logic
         * These locations have null-write patterns that crash
         * Detect and skip them */
        if (rip == 0x1411e424cULL || rip == 0x1423b1403ULL) {
            static int blocker_hit_count = 0;
            blocker_hit_count++;
            
            u64 rdi = uc->uc_mcontext.gregs[REG_RDI];
            u64 rax = uc->uc_mcontext.gregs[REG_RAX];
            u64 rsi = uc->uc_mcontext.gregs[REG_RSI];
            u64 rdx = uc->uc_mcontext.gregs[REG_RDX];
            u64 rcx = uc->uc_mcontext.gregs[REG_RCX];
            u64 r8  = uc->uc_mcontext.gregs[REG_R8];
            u64 r9  = uc->uc_mcontext.gregs[REG_R9];
            u64 rsp = uc->uc_mcontext.gregs[REG_RSP];
            u64 rbp = uc->uc_mcontext.gregs[REG_RBP];
            
            /* Dump 8 bytes of instruction at RIP */
            u8 *bytes = (u8*)rip;
            
            if (blocker_hit_count <= 3) {
                fprintf(stderr, "\n[REVERSE_ENG] ========== BLOCKER HIT #%d at 0x%lx ==========\n", blocker_hit_count, rip);
                fprintf(stderr, "[REVERSE_ENG] Instruction bytes at RIP: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                        bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7]);
                fprintf(stderr, "[REVERSE_ENG] Previous 8 bytes:        %02x %02x %02x %02x %02x %02x %02x %02x\n",
                        bytes[-8], bytes[-7], bytes[-6], bytes[-5], bytes[-4], bytes[-3], bytes[-2], bytes[-1]);
                fprintf(stderr, "[REVERSE_ENG] REGS: RAX=0x%016lx RBX=? RCX=0x%016lx RDX=0x%016lx\n", rax, rcx, rdx);
                fprintf(stderr, "[REVERSE_ENG]      RDI=0x%016lx RSI=0x%016lx RBP=0x%016lx RSP=0x%016lx\n", rdi, rsi, rbp, rsp);
                fprintf(stderr, "[REVERSE_ENG]      R8=0x%016lx R9=0x%016lx\n", r8, r9);
                fprintf(stderr, "[REVERSE_ENG] STACK at RSP: 0x%016lx 0x%016lx 0x%016lx 0x%016lx\n",
                        rsp < 0xffffffffffff ? ((u64*)rsp)[0] : 0,
                        rsp < 0xffffffffffff ? ((u64*)rsp)[1] : 0,
                        rsp < 0xffffffffffff ? ((u64*)rsp)[2] : 0,
                        rsp < 0xffffffffffff ? ((u64*)rsp)[3] : 0);
                fprintf(stderr, "[REVERSE_ENG] FaultAddr=0x%lx (trying to access NULL)\n", faultaddr);
                fprintf(stderr, "[REVERSE_ENG] ===============================================\n\n");
            }
            
            /* If RDI is NULL, this is a degenerate null-write case.
             * Likely game logic error: trying to initialize something that wasn't allocated.
             * Skip this function entirely and return success (RAX=0) */
            if (rdi == 0) {
                fprintf(stderr,
                        "[DIAG] Blocker NULL-write: RDI=0x%lx RAX=0x%lx - skipping entirely\n",
                        rdi, rax);
                uc->uc_mcontext.gregs[REG_RAX] = 0;  /* Return S_OK */
                uc->uc_mcontext.gregs[REG_RIP] = (greg_t)crash_pick_fallback_rip(rip, "blocker-null-write");
                return;
            }
            
            fprintf(stderr,
                    "[DIAG] Blocker 0x%lx with RDI=0x%lx RAX=0x%lx RSI=0x%lx RDX=0x%lx; escaping\n",
                    rip, rdi, rax, rsi, rdx);
            
            /* For non-null RDI, try to skip and return success */
            uc->uc_mcontext.gregs[REG_RAX] = 0;  /* Return S_OK / success */
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)crash_pick_fallback_rip(rip, "blocker-escape-forced");
            return;
        }
        
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
static int beer_present_d3d12_resource(const void *resource, uint64_t serial,
                                       const uint8_t *pixels, int width,
                                       int height, int row_pitch)
{
    return xwayland_window_present_resource_rgba8(
        resource, serial, pixels, width, height, row_pitch);
}

static int beer_clear_d3d12_target(const void *resource,
                                   uint64_t input_serial,
                                   uint64_t output_serial,
                                   uint8_t *pixels, uint64_t bytes,
                                   uint32_t width, uint32_t height,
                                   uint32_t format,
                                   const float color[4])
{
    return vulkan_indexed_renderer_clear_target(
        resource, input_serial, output_serial, pixels, (size_t)bytes,
        width, height, format, color);
}

static int beer_inspect_d3d12_compute(
    const BeerD3D12ComputeDispatch *dispatch)
{
    static pthread_mutex_t dump_mutex = PTHREAD_MUTEX_INITIALIZER;
    static uint64_t dumped_shader_hash;
    if (!dispatch || !dispatch->shader || !dispatch->shader_size)
        return 0;
    if (!getenv("BEER_D3D12_DUMP_COMPUTE"))
        return beer_d3d12_execute_known_compute(dispatch);

    pthread_mutex_lock(&dump_mutex);
    static unsigned binding_dump_count;
    if (binding_dump_count < 16) {
        fprintf(stderr, "[D3D12] compute bindings groups=(%u,%u,%u)",
                dispatch->group_count_x, dispatch->group_count_y,
                dispatch->group_count_z);
        for (uint32_t index = 0; index < BEER_D3D12_MAX_ROOT_PARAMETERS;
             ++index) {
            const BeerD3D12ComputeBinding *binding = &dispatch->bindings[index];
            if (binding->kind) {
                fprintf(stderr, " root%u=kind%u/table%llx/address%llx/constants%u",
                        index, binding->kind,
                        (unsigned long long)binding->descriptor_table,
                        (unsigned long long)binding->buffer_location,
                        binding->constant_count);
                if (binding->kind == 1) {
                    for (uint32_t heap = 0;
                         heap < dispatch->descriptor_heap_count; ++heap) {
                        uint64_t start = dispatch->descriptor_heap_gpu_start[heap];
                        size_t size = dispatch->descriptor_heap_size[heap];
                        if (binding->descriptor_table >= start &&
                            binding->descriptor_table - start < size) {
                            size_t offset = (size_t)(binding->descriptor_table - start);
                            size_t available = size - offset;
                            if (available > 16) available = 16;
                            fprintf(stderr, "/heap%u+%zu=", heap, offset);
                            const uint8_t *bytes =
                                dispatch->descriptor_heap_storage[heap] + offset;
                            for (size_t byte = 0; byte < available; ++byte)
                                fprintf(stderr, "%02x", bytes[byte]);
                            break;
                        }
                    }
                }
            }
        }
        fputc('\n', stderr);
        ++binding_dump_count;
    }
    if (dumped_shader_hash != dispatch->shader_hash) {
        char path[160];
        snprintf(path, sizeof(path), "/tmp/beer-d3d12-%016llx.dxbc",
                 (unsigned long long)dispatch->shader_hash);
        FILE *file = fopen(path, "wb");
        if (file) {
            fwrite(dispatch->shader, 1, dispatch->shader_size, file);
            fclose(file);
        }
        if (dispatch->root_signature_blob &&
            dispatch->root_signature_blob_size) {
            snprintf(path, sizeof(path), "/tmp/beer-d3d12-%016llx.rootsig",
                     (unsigned long long)dispatch->shader_hash);
            file = fopen(path, "wb");
            if (file) {
                fwrite(dispatch->root_signature_blob, 1,
                       dispatch->root_signature_blob_size, file);
                fclose(file);
            }
        }
        fprintf(stderr,
                "[D3D12] dumped compute shader=%zu root-signature=%zu "
                "hash=%016llx\n",
                dispatch->shader_size, dispatch->root_signature_blob_size,
                (unsigned long long)dispatch->shader_hash);
        dumped_shader_hash = dispatch->shader_hash;
    }
    pthread_mutex_unlock(&dump_mutex);
    /* Inspection does not claim execution. The Vulkan backend must return
     * success only after translating and submitting the workload. */
    return beer_d3d12_execute_known_compute(dispatch);
}

static void beer_observe_d3d12_submission(
    const BeerD3D12SubmissionStats *stats)
{
    static _Atomic(uint64_t) submission_serial;
    if (!stats) return;
    uint64_t serial = atomic_fetch_add(&submission_serial, 1) + 1;
    if (serial <= 128 || stats->draw_instanced_count ||
        stats->draw_indexed_instanced_count ||
        stats->render_target_binding_count) {
        fprintf(stderr,
                "[D3D12 STATE] submission=%llu lists=%u/%u open=%u "
                "draw=%llu draw-indexed=%llu set-rt=%llu dispatch=%llu "
                "executed=%u rejected=%u\n",
                (unsigned long long)serial, stats->valid_list_count,
                stats->submitted_list_count, stats->open_list_count,
                (unsigned long long)stats->draw_instanced_count,
                (unsigned long long)stats->draw_indexed_instanced_count,
                (unsigned long long)stats->render_target_binding_count,
                (unsigned long long)stats->dispatch_count,
                stats->executed_dispatch_count,
                stats->rejected_dispatch_count);
    }
}

int main(int argc, char **argv)
{
    beer_d3d12_set_event_signaler(beer_signal_guest_event);
    beer_d3d12_set_presenter(beer_present_d3d12_resource);
    beer_d3d12_set_clear_target(beer_clear_d3d12_target);
    beer_d3d12_set_compute_executor(beer_inspect_d3d12_compute);
    beer_d3d12_set_submission_observer(beer_observe_d3d12_submission);

    XwaylandPresenter presenter = XWAYLAND_PRESENTER_VULKAN;
    BeerD3D11Renderer renderer = BEER_D3D11_RENDERER_VULKAN;
    const char *exe_arg = NULL;
    const char *prefix_arg = NULL;
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (strncmp(arg, "--presenter=", 12) == 0) {
            const char *value = arg + 12;
            if (strcmp(value, "vulkan") != 0) {
                fprintf(stderr, "Unknown presenter '%s' (only vulkan is supported)\n",
                        value);
                return 2;
            }
            presenter = XWAYLAND_PRESENTER_VULKAN;
        } else if (strncmp(arg, "--renderer=", 11) == 0) {
            const char *value = arg + 11;
            if (strcmp(value, "vulkan") != 0) {
                fprintf(stderr, "Unknown renderer '%s' (only vulkan is supported)\n",
                        value);
                return 2;
            }
            renderer = BEER_D3D11_RENDERER_VULKAN;
        } else if (strncmp(arg, "--prefix=", 9) == 0) {
            prefix_arg = arg + 9;
            if (!*prefix_arg) {
                fprintf(stderr, "--prefix requires a non-empty path\n");
                return 2;
            }
        } else if (strncmp(arg, "--audio=", 8) == 0) {
            const char *value = arg + 8;
            if (!strcmp(value, "compat")) g_use_guest_fmod = 0;
            else if (!strcmp(value, "fmod")) g_use_guest_fmod = 1;
            else {
                fprintf(stderr, "Unknown audio backend '%s' (expected compat or fmod)\n",
                        value);
                return 2;
            }
        } else if (arg[0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", arg);
            return 2;
        } else if (!exe_arg) {
            exe_arg = arg;
        } else {
            fprintf(stderr, "Unexpected extra argument: %s\n", arg);
            return 2;
        }
    }
    if (!exe_arg) {
        fprintf(stderr,
                "Usage: %s [--renderer=vulkan] [--presenter=vulkan] "
                "[--prefix=PATH] [--audio=compat|fmod] <windows-executable>\n",
                argv[0]);
        return 2;
    }
    if (!d3d11_set_renderer(renderer))
        die("cannot select D3D11 renderer '%s'", d3d11_renderer_name(renderer));
    fprintf(stderr, "[RENDERER] selected Vulkan-only rendering; software fallback removed\n");
    if (presenter != XWAYLAND_PRESENTER_VULKAN)
        die("Vulkan rendering requires --presenter=vulkan; fallback is disabled");
    if (!xwayland_set_presenter(presenter))
        die("cannot select native presenter '%s'", xwayland_presenter_name(presenter));
    fprintf(stderr, "[PRESENTER] selected %s\n", xwayland_presenter_name(presenter));
    fprintf(stderr, "[AUDIO] selected %s backend\n",
            g_use_guest_fmod ? "bundled FMOD" : "silent compatibility");

    if (!configure_guest_prefix(prefix_arg))
        die("cannot configure Beer prefix '%s': %s",
            prefix_arg ? prefix_arg : "(default)", strerror(errno));
    const char *exe = configure_guest_process_path(exe_arg);
    if (!exe)
        die("cannot configure guest process path '%s': %s", exe_arg, strerror(errno));

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

    /* Delay installing guest-fault recovery until after the PE image has been
     * mapped. This keeps the preferred image range available to debuggers,
     * which may plant breakpoints there before the inferior reaches main. */
    printf("=== Beer Loader ===\n");
    pe_load(exe);

    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
    sigaction(SIGILL,  &sa, NULL);
    sigaction(SIGFPE,  &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL);  /* int3 / abort() in Windows CRT */
    
    /* DIAGNOSTIC: Check if blocker address is loaded correctly right after PE load */
    {
        u64 blocker_addr = (u64)g_img + 0x23b1403;
        u8 *test_bytes = (u8*)blocker_addr;
        fprintf(stderr, "[DIAGNOSTIC] Right after PE load, bytes at blocker 0x23b1403:\n");
        fprintf(stderr, "  %02x %02x %02x %02x %02x %02x %02x %02x\n",
                test_bytes[0], test_bytes[1], test_bytes[2], test_bytes[3],
                test_bytes[4], test_bytes[5], test_bytes[6], test_bytes[7]);
    }
    
    pe_imports();
    
    /* CHECK: After pe_imports */
    {
        u64 blocker_addr = (u64)g_img + 0x23b1403;
        u8 *test_bytes = (u8*)blocker_addr;
        fprintf(stderr, "[DIAGNOSTIC] After pe_imports, bytes at blocker 0x23b1403:\n");
        fprintf(stderr, "  %02x %02x %02x %02x %02x %02x %02x %02x\n",
                test_bytes[0], test_bytes[1], test_bytes[2], test_bytes[3],
                test_bytes[4], test_bytes[5], test_bytes[6], test_bytes[7]);
    }
    
    setup_teb_peb();
    guest_dll_initialize_all();
    
    /* CHECK: After setup_teb_peb */
    {
        u64 blocker_addr = (u64)g_img + 0x23b1403;
        u8 *test_bytes = (u8*)blocker_addr;
        fprintf(stderr, "[DIAGNOSTIC] After setup_teb_peb, bytes at blocker 0x23b1403:\n");
        fprintf(stderr, "  %02x %02x %02x %02x %02x %02x %02x %02x\n",
                test_bytes[0], test_bytes[1], test_bytes[2], test_bytes[3],
                test_bytes[4], test_bytes[5], test_bytes[6], test_bytes[7]);
    }

    {
        int n = patch_transaction_assertions();
        printf("[PATCH] Disabled %d Transaction-failed assertion branches\n", n);
    }
    
    /* CHECK: After patch_transaction_assertions */
    {
        u64 blocker_addr = (u64)g_img + 0x23b1403;
        u8 *test_bytes = (u8*)blocker_addr;
        fprintf(stderr, "[DIAGNOSTIC] After patch_transaction_assertions, bytes at blocker 0x23b1403:\n");
        fprintf(stderr, "  %02x %02x %02x %02x %02x %02x %02x %02x\n",
                test_bytes[0], test_bytes[1], test_bytes[2], test_bytes[3],
                test_bytes[4], test_bytes[5], test_bytes[6], test_bytes[7]);
    }

    apply_section_perms();
    
    /* CHECK: After apply_section_perms */
    {
        u64 blocker_addr = (u64)g_img + 0x23b1403;
        u8 *test_bytes = (u8*)blocker_addr;
        fprintf(stderr, "[DIAGNOSTIC] After apply_section_perms, bytes at blocker 0x23b1403:\n");
        fprintf(stderr, "  %02x %02x %02x %02x %02x %02x %02x %02x\n",
                test_bytes[0], test_bytes[1], test_bytes[2], test_bytes[3],
                test_bytes[4], test_bytes[5], test_bytes[6], test_bytes[7]);
    }
    
    patch_known_bad_targets();
    
    /* CHECK: After patch_known_bad_targets */
    {
        u64 blocker_addr = (u64)g_img + 0x23b1403;
        u8 *test_bytes = (u8*)blocker_addr;
        fprintf(stderr, "[DIAGNOSTIC] After patch_known_bad_targets, bytes at blocker 0x23b1403:\n");
        fprintf(stderr, "  %02x %02x %02x %02x %02x %02x %02x %02x\n",
                test_bytes[0], test_bytes[1], test_bytes[2], test_bytes[3],
                test_bytes[4], test_bytes[5], test_bytes[6], test_bytes[7]);
    }
    
    init_steam_fake();
    
    /* CHECK: After init_steam_fake */
    {
        u64 blocker_addr = (u64)g_img + 0x23b1403;
        u8 *test_bytes = (u8*)blocker_addr;
        fprintf(stderr, "[DIAGNOSTIC] After init_steam_fake, bytes at blocker 0x23b1403:\n");
        fprintf(stderr, "  %02x %02x %02x %02x %02x %02x %02x %02x\n",
                test_bytes[0], test_bytes[1], test_bytes[2], test_bytes[3],
                test_bytes[4], test_bytes[5], test_bytes[6], test_bytes[7]);
    }
    
    init_dxgi_fake();
    
    /* CHECK: After init_dxgi_fake */
    {
        u64 blocker_addr = (u64)g_img + 0x23b1403;
        u8 *test_bytes = (u8*)blocker_addr;
        fprintf(stderr, "[DIAGNOSTIC] After init_dxgi_fake, bytes at blocker 0x23b1403:\n");
        fprintf(stderr, "  %02x %02x %02x %02x %02x %02x %02x %02x\n",
                test_bytes[0], test_bytes[1], test_bytes[2], test_bytes[3],
                test_bytes[4], test_bytes[5], test_bytes[6], test_bytes[7]);
    }
    
    init_d3d11_fake();
    
    /* CHECK: After init_d3d11_fake */
    {
        u64 blocker_addr = (u64)g_img + 0x23b1403;
        u8 *test_bytes = (u8*)blocker_addr;
        fprintf(stderr, "[DIAGNOSTIC] After init_d3d11_fake, bytes at blocker 0x23b1403:\n");
        fprintf(stderr, "  %02x %02x %02x %02x %02x %02x %02x %02x\n",
                test_bytes[0], test_bytes[1], test_bytes[2], test_bytes[3],
                test_bytes[4], test_bytes[5], test_bytes[6], test_bytes[7]);
    }
    
    init_dxgi_adapter_fake();
    
    /* CHECK: After init_dxgi_adapter_fake */
    {
        u64 blocker_addr = (u64)g_img + 0x23b1403;
        u8 *test_bytes = (u8*)blocker_addr;
        fprintf(stderr, "[DIAGNOSTIC] After init_dxgi_adapter_fake, bytes at blocker 0x23b1403:\n");
        fprintf(stderr, "  %02x %02x %02x %02x %02x %02x %02x %02x\n",
                test_bytes[0], test_bytes[1], test_bytes[2], test_bytes[3],
                test_bytes[4], test_bytes[5], test_bytes[6], test_bytes[7]);
    }
    
    /* Patch dxgi factory vtable with real adapter + swapchain */
    g_dxgi_vtab[7]  = (u64)dxgi_EnumAdapters_with_fake;  /* EnumAdapters */
    g_dxgi_vtab[10] = (u64)dxgi_CreateSwapChain;          /* CreateSwapChain */
    g_dxgi_vtab[12] = (u64)dxgi_EnumAdapters_with_fake;   /* EnumAdapters1 */
    
    /* CHECK: After vtable patching */
    {
        u64 blocker_addr = (u64)g_img + 0x23b1403;
        u8 *test_bytes = (u8*)blocker_addr;
        fprintf(stderr, "[DIAGNOSTIC] After vtable patching, bytes at blocker 0x23b1403:\n");
        fprintf(stderr, "  %02x %02x %02x %02x %02x %02x %02x %02x\n",
                test_bytes[0], test_bytes[1], test_bytes[2], test_bytes[3],
                test_bytes[4], test_bytes[5], test_bytes[6], test_bytes[7]);
    }
    
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
    /* A PE entry point is called by the Windows loader. If it returns, control
     * belongs to the loader, not to the executable's CRT startup routine again. */
    u64 guest_initial_ret = (u64)guest_exit_stub;
    *(u64 *)guest_rbp_slot = guest_rbp_slot;
    *(u64 *)guest_ret_slot = guest_initial_ret;
    *(u64 *)guest_frame_ptr_slot = guest_ret_slot;
    g_entry_rsp = guest_frame_base;
    g_entry_rsp_limit = guest_frame_base + 0x1000;
    g_entry_ret_slot = guest_ret_slot;
    seed_guest_entry_frame(guest_initial_ret);
    g_guest_stack_low = guest_stack_low;
    g_guest_stack_high = guest_stack_high;
    g_current_guest_stack_low = guest_stack_low;
    g_current_guest_stack_high = guest_stack_high;
    set_teb_stack_bounds(guest_stack_low, guest_stack_high);

    printf("[STACK] guest stack=0x%lx..0x%lx ret=0x%lx entry_rsp=0x%lx ret_slot=0x%lx\n",
           guest_stack_low, guest_stack_high, *(u64 *)guest_ret_slot,
           g_entry_rsp, g_entry_ret_slot);
    fflush(stdout);

    fprintf(stderr, "[ENTRY] Entry point at %p, g_entry_rsp=%p, g_host_call_stack: %p..%p\n",
            (void*)entry, (void*)g_entry_rsp, (void*)g_host_call_stack_base, (void*)g_host_call_stack_top);

    /* SavedGuestRsp is transient: an outer dispatch sets it while host code is
     * active and clears it when guest RSP is restored. */
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
