#ifndef BEER_ARCH_H
#define BEER_ARCH_H

/* Host/guest ISA seams.
 *
 * The guest is always x86-64 PE code. On an x86-64 host it runs natively; on
 * any other host (Android/ARM64) it needs a guest CPU emulator, and everything
 * that reads host CPU state directly has to go through a helper in here instead
 * of using x86 builtins or Linux flags. */

#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>

#if defined(__x86_64__)
#define BEER_HOST_X86_64 1
#else
#define BEER_HOST_X86_64 0
#endif

/* Nonzero when guest instructions execute directly on the host CPU. */
#define BEER_GUEST_NATIVE BEER_HOST_X86_64

/* Diagnostics that need glibc's <execinfo.h> backtrace() or x86 ucontext
 * registers. bionic (Android) has neither before API 33. Override with
 * -DBEER_HAVE_GLIBC_DIAG=0 to build the stub branches. */
#ifndef BEER_HAVE_GLIBC_DIAG
#if defined(__GLIBC__) && BEER_HOST_X86_64
#define BEER_HAVE_GLIBC_DIAG 1
#else
#define BEER_HAVE_GLIBC_DIAG 0
#endif
#endif

/* backtrace() is missing from bionic before API 33; android/android_support.c provides one on _Unwind_Backtrace. */
#if defined(__ANDROID__) && __ANDROID_API__ < 33
int backtrace(void **buffer, int size);
#endif

/* Number of winnt.h PF_* feature slots mirrored into KUSER_SHARED_DATA. */
#define BEER_PF_COUNT 64

/* Fills out[PF_*] with the processor features the guest should observe, using
 * winnt.h numbering. A single source keeps IsProcessorFeaturePresent and the
 * KUSER_SHARED_DATA ProcessorFeatures[] array in agreement.
 *
 * Native x86-64: the host CPU's real features. Emulated: a conservative SSE4.2
 * baseline; raise it only to what the chosen emulator actually implements. */
static inline void beer_guest_processor_features(uint8_t out[BEER_PF_COUNT])
{
    for (int i = 0; i < BEER_PF_COUNT; i++) out[i] = 0;

    out[2]  = 1;                                    /* CMPXCHG8B */
    out[8]  = 1;                                    /* RDTSC */
    out[9]  = 1;                                    /* PAE */
    out[12] = 1;                                    /* NX */
    out[14] = 1;                                    /* CMPXCHG16B */
    out[32] = 1;                                    /* RDTSCP */

#if BEER_HOST_X86_64
    __builtin_cpu_init();
    /* __builtin_cpu_supports needs string literals, so each feature is spelled out. */
    out[3]  = __builtin_cpu_supports("mmx") != 0;
    out[6]  = __builtin_cpu_supports("sse") != 0;
    out[10] = __builtin_cpu_supports("sse2") != 0;
    out[13] = __builtin_cpu_supports("sse3") != 0;
    out[17] = __builtin_cpu_supports("xsave") != 0;
    out[22] = 1;                                    /* RDWRFSGSBASE */
    out[28] = __builtin_cpu_supports("rdrnd") != 0;
    out[36] = __builtin_cpu_supports("ssse3") != 0;
    out[37] = __builtin_cpu_supports("sse4.1") != 0;
    out[38] = __builtin_cpu_supports("sse4.2") != 0;
    out[39] = __builtin_cpu_supports("avx") != 0;
    out[40] = __builtin_cpu_supports("avx2") != 0;
    out[41] = __builtin_cpu_supports("avx512f") != 0;
#else
    out[3]  = 1;                                    /* MMX */
    out[6]  = 1;                                    /* SSE */
    out[10] = 1;                                    /* SSE2 */
    out[13] = 1;                                    /* SSE3 */
    out[36] = 1;                                    /* SSSE3 */
    out[37] = 1;                                    /* SSE4.1 */
    out[38] = 1;                                    /* SSE4.2 */
#endif
}

/* Anonymous RWX mapping below 4 GiB. MSVC runtimes synthesize rel32 call
 * targets from IAT thunks, so the thunk area must stay in the low 32-bit range.
 * MAP_32BIT exists only on x86-64 Linux; elsewhere probe for a free low range
 * and accept it only if the kernel placed the mapping exactly where asked. */
static inline void *beer_mmap_low32_rwx(size_t size)
{
    const int prot = PROT_READ | PROT_WRITE | PROT_EXEC;
#if BEER_HOST_X86_64 && defined(MAP_32BIT)
    return mmap(NULL, size, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
#else
    const uint64_t limit = UINT64_C(0x100000000);
    const uint64_t step = UINT64_C(0x1000000);
    if (size == 0 || size > limit - step) return MAP_FAILED;
    for (uint64_t hint = UINT64_C(0x10000000); hint + size <= limit; hint += step) {
        void *p = mmap((void *)(uintptr_t)hint, size, prot,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) return MAP_FAILED;
        if ((uint64_t)(uintptr_t)p == hint) return p;
        munmap(p, size);
    }
    return MAP_FAILED;
#endif
}

#endif
