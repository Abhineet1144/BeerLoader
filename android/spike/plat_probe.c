// Android platform probe for the Beer guest-CPU plan (run in Termux: bionic, aarch64).
#define _GNU_SOURCE
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

static void report(const char *name, int ok, const char *detail)
{
    printf("%-44s %s %s\n", name, ok ? "OK  " : "FAIL", detail ? detail : "");
}

static sigjmp_buf jb;
static volatile uintptr_t fault_addr, fault_pc;
static void on_segv(int sig, siginfo_t *si, void *uc)
{
    (void)sig;
    fault_addr = (uintptr_t)si->si_addr;
    fault_pc = ((ucontext_t *)uc)->uc_mcontext.pc;
    siglongjmp(jb, 1);
}

int main(void)
{
    char buf[128];
    snprintf(buf, sizeof buf, "pagesize=%ld", sysconf(_SC_PAGESIZE));
    report("page size", sysconf(_SC_PAGESIZE) == 4096, buf);

    // JIT-style executable memory: write ARM64 `mov w0,#42; ret`, flush, call.
    {
        void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        int ok = p != MAP_FAILED;
        snprintf(buf, sizeof buf, "errno=%d", ok ? 0 : errno);
        report("mmap RWX anonymous", ok, buf);
        if (ok) {
            const uint32_t code[] = {0x52800540u, 0xd65f03c0u};
            memcpy(p, code, sizeof code);
            __builtin___clear_cache((char *)p, (char *)p + sizeof code);
            int (*fn)(void) = (int (*)(void))p;
            snprintf(buf, sizeof buf, "returned %d", fn());
            report("execute JIT code in RWX page", fn() == 42, buf);
        }
    }
    // W^X flip: RW -> write -> RX -> execute.
    {
        void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        const uint32_t code[] = {0x528005a0u, 0xd65f03c0u};   // mov w0,#45; ret
        memcpy(p, code, sizeof code);
        int rc = mprotect(p, 4096, PROT_READ | PROT_EXEC);
        __builtin___clear_cache((char *)p, (char *)p + sizeof code);
        snprintf(buf, sizeof buf, "mprotect rc=%d errno=%d", rc, errno);
        report("mprotect RW -> RX then execute", rc == 0 && ((int (*)(void))p)() == 45, buf);
    }

    // Guest fixed addresses.
    {
        void *p = mmap((void *)0x140000000ull, 0x3000, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        snprintf(buf, sizeof buf, "got %p errno=%d", p, p == MAP_FAILED ? errno : 0);
        report("map PE image base 0x140000000 (RWX)", p == (void *)0x140000000ull, buf);
        p = mmap((void *)0x7ffe0000ull, 0x1000, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        snprintf(buf, sizeof buf, "got %p errno=%d", p, p == MAP_FAILED ? errno : 0);
        report("map KUSER_SHARED_DATA 0x7ffe0000", p == (void *)0x7ffe0000ull, buf);
        p = mmap((void *)0x10000ull, 0x1000, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        snprintf(buf, sizeof buf, "got %p errno=%d", p, p == MAP_FAILED ? errno : 0);
        report("map low address 0x10000 (Windows min)", p == (void *)0x10000ull, buf);
    }

    // Address-space extent: highest address that can be mapped, and big reservations.
    {
        uint64_t best = 0;
        for (int bit = 47; bit >= 30; --bit) {
            void *want = (void *)(1ull << bit);
            void *p = mmap(want, 0x10000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
            if (p == want) { best = 1ull << bit; munmap(p, 0x10000); break; }
            if (p != MAP_FAILED) munmap(p, 0x10000);
        }
        snprintf(buf, sizeof buf, "highest fixed 2^n mapping: 0x%llx", (unsigned long long)best);
        report("address space extent", best != 0, buf);
        static const uint64_t sizes[] = {1ull << 30, 16ull << 30, 64ull << 30, 128ull << 30};
        for (unsigned i = 0; i < 4; ++i) {
            void *p = mmap(NULL, sizes[i], PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
            snprintf(buf, sizeof buf, "%llu GiB reserve -> %p", (unsigned long long)(sizes[i] >> 30), p);
            report("PROT_NONE NORESERVE reservation", p != MAP_FAILED, buf);
            if (p != MAP_FAILED) munmap(p, sizes[i]);
        }
        // Everything below 4 GiB the loader uses for import thunks.
        void *low = NULL;
        for (uint64_t hint = 0x10000000ull; hint + 0x100000 <= 0x100000000ull; hint += 0x1000000ull) {
            void *p = mmap((void *)hint, 0x100000, PROT_READ | PROT_WRITE | PROT_EXEC,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (p == (void *)hint) { low = p; break; }
        }
        snprintf(buf, sizeof buf, "first free RWX range below 4 GiB: %p", low);
        report("low32 RWX region (import thunks)", low != NULL, buf);
    }

    // SIGSEGV delivery with full register access.
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = on_segv;
        sa.sa_flags = SA_SIGINFO | SA_NODEFER;
        sigaction(SIGSEGV, &sa, NULL);
        if (sigsetjmp(jb, 1) == 0) *(volatile int *)0 = 1;
        snprintf(buf, sizeof buf, "addr=%p pc=%p", (void *)fault_addr, (void *)fault_pc);
        report("SIGSEGV handler gets address and pc", fault_addr == 0 && fault_pc != 0, buf);
    }

    // memfd and getrandom (loader: NT sections, BCryptGenRandom).
    {
        int fd = (int)syscall(SYS_memfd_create, "probe", 1u);
        snprintf(buf, sizeof buf, "fd=%d errno=%d", fd, fd < 0 ? errno : 0);
        report("memfd_create", fd >= 0, buf);
        unsigned char r[8];
        long n = syscall(SYS_getrandom, r, sizeof r, 0);
        report("getrandom", n == 8, NULL);
    }
    return 0;
}
