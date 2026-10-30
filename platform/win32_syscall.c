#define _GNU_SOURCE
#include "win32_syscall.h"

#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>

#ifndef PR_SET_SYSCALL_USER_DISPATCH
#define PR_SET_SYSCALL_USER_DISPATCH 59
#endif
#ifndef PR_SYS_DISPATCH_ON
#define PR_SYS_DISPATCH_ON 1
#endif
#ifndef SYS_USER_DISPATCH
#define SYS_USER_DISPATCH 2
#endif

__thread uint8_t beer_win32_syscall_selector;
__thread uint64_t beer_win32_syscall_resume_rip;

/* Re-enter guest code after a serviced call: block guest system calls again
 * and continue at the saved address. No register or flag is changed. */
extern void beer_win32_syscall_resume(void);
__asm__(
    ".text\n"
    ".global beer_win32_syscall_resume\n"
    "beer_win32_syscall_resume:\n"
    "    movb $1, %fs:beer_win32_syscall_selector@tpoff\n"
    "    jmp *%fs:beer_win32_syscall_resume_rip@tpoff\n"
);

typedef struct {
    char name[64];              /* canonical Nt<Name>; empty when unused */
    BeerWin32SyscallFn fn;
    int reported;
} BeerWin32Service;

static BeerWin32Service g_services[BEER_WIN32_SYSCALL_CAPACITY];
static unsigned g_next_extra = BEER_WIN32_SYSCALL_EXTRA_BASE;
static int g_installed;
static int g_trace = -1;

/* System-service numbers of Windows 10 build 19045 (x64) for the services Beer
 * may implement. Builds 19041 through 19045 share this table. */
static const struct { const char *name; uint16_t number; } g_windows_19045[] = {
    {"NtAccessCheck", 0x00}, {"NtWorkerFactoryWorkerReady", 0x01}, {"NtAcceptConnectPort", 0x02},
    {"NtMapUserPhysicalPagesScatter", 0x03}, {"NtWaitForSingleObject", 0x04}, {"NtCallbackReturn", 0x05},
    {"NtReadFile", 0x06}, {"NtDeviceIoControlFile", 0x07}, {"NtWriteFile", 0x08},
    {"NtRemoveIoCompletion", 0x09}, {"NtReleaseSemaphore", 0x0A}, {"NtReplyWaitReceivePort", 0x0B},
    {"NtReplyPort", 0x0C}, {"NtSetInformationThread", 0x0D}, {"NtSetEvent", 0x0E},
    {"NtClose", 0x0F}, {"NtQueryObject", 0x10}, {"NtQueryInformationFile", 0x11},
    {"NtOpenKey", 0x12}, {"NtEnumerateValueKey", 0x13}, {"NtFindAtom", 0x14},
    {"NtQueryDefaultLocale", 0x15}, {"NtQueryKey", 0x16}, {"NtQueryValueKey", 0x17},
    {"NtAllocateVirtualMemory", 0x18}, {"NtQueryInformationProcess", 0x19},
    {"NtWaitForMultipleObjects32", 0x1A}, {"NtWriteFileGather", 0x1B},
    {"NtSetInformationProcess", 0x1C}, {"NtCreateKey", 0x1D}, {"NtFreeVirtualMemory", 0x1E},
    {"NtImpersonateClientOfPort", 0x1F}, {"NtReleaseMutant", 0x20}, {"NtQueryInformationToken", 0x21},
    {"NtRequestWaitReplyPort", 0x22}, {"NtQueryVirtualMemory", 0x23}, {"NtOpenThreadToken", 0x24},
    {"NtQueryInformationThread", 0x25}, {"NtOpenProcess", 0x26}, {"NtSetInformationFile", 0x27},
    {"NtMapViewOfSection", 0x28}, {"NtAccessCheckAndAuditAlarm", 0x29}, {"NtUnmapViewOfSection", 0x2A},
    {"NtReplyWaitReceivePortEx", 0x2B}, {"NtTerminateProcess", 0x2C}, {"NtSetEventBoostPriority", 0x2D},
    {"NtReadFileScatter", 0x2E}, {"NtOpenThreadTokenEx", 0x2F}, {"NtOpenProcessTokenEx", 0x30},
    {"NtQueryPerformanceCounter", 0x31}, {"NtEnumerateKey", 0x32}, {"NtOpenFile", 0x33},
    {"NtDelayExecution", 0x34}, {"NtQueryDirectoryFile", 0x35}, {"NtQuerySystemInformation", 0x36},
    {"NtOpenSection", 0x37}, {"NtQueryTimer", 0x38}, {"NtFsControlFile", 0x39},
    {"NtWriteVirtualMemory", 0x3A}, {"NtCloseObjectAuditAlarm", 0x3B}, {"NtDuplicateObject", 0x3C},
    {"NtQueryAttributesFile", 0x3D}, {"NtClearEvent", 0x3E}, {"NtReadVirtualMemory", 0x3F},
    {"NtOpenEvent", 0x40}, {"NtAdjustPrivilegesToken", 0x41}, {"NtDuplicateToken", 0x42},
    {"NtContinue", 0x43}, {"NtQueryDefaultUILanguage", 0x44}, {"NtQueueApcThread", 0x45},
    {"NtYieldExecution", 0x46}, {"NtAddAtom", 0x47}, {"NtCreateEvent", 0x48},
    {"NtQueryVolumeInformationFile", 0x49}, {"NtCreateSection", 0x4A}, {"NtFlushBuffersFile", 0x4B},
    {"NtApphelpCacheControl", 0x4C}, {"NtCreateProcessEx", 0x4D}, {"NtCreateThread", 0x4E},
    {"NtIsProcessInJob", 0x4F}, {"NtProtectVirtualMemory", 0x50}, {"NtQuerySection", 0x51},
    {"NtResumeThread", 0x52}, {"NtTerminateThread", 0x53}, {"NtReadRequestData", 0x54},
    {"NtCreateFile", 0x55}, {"NtQueryEvent", 0x56}, {"NtWriteRequestData", 0x57},
    {"NtOpenDirectoryObject", 0x58}, {"NtAccessCheckByTypeAndAuditAlarm", 0x59},
    {"NtQuerySystemTime", 0x5A}, {"NtWaitForMultipleObjects", 0x5B}, {"NtSetInformationObject", 0x5C},
    {"NtCancelIoFile", 0x5D}, {"NtTraceEvent", 0x5E}, {"NtPowerInformation", 0x5F},
    {"NtSetValueKey", 0x60}, {"NtCancelTimer", 0x61}, {"NtSetTimer", 0x62},
};

int beer_win32_syscall_is_service_name(const char *name)
{
    if (!name) return 0;
    if (strncmp(name, "Nt", 2) && strncmp(name, "Zw", 2)) return 0;
    if (!strncmp(name, "Ntdll", 5)) return 0;               /* NtdllDefWindowProc_A and similar */
    if (!strcmp(name + 2, "GetTickCount") || !strcmp(name + 2, "CurrentTeb")) return 0;
    return name[2] >= 'A' && name[2] <= 'Z';
}

int beer_win32_syscall_register(const char *name, BeerWin32SyscallFn fn)
{
    if (!beer_win32_syscall_is_service_name(name) || strlen(name) >= sizeof(g_services[0].name))
        return -1;
    char canonical[64];
    snprintf(canonical, sizeof(canonical), "Nt%s", name + 2);
    int number = -1;
    for (size_t i = 0; i < sizeof(g_windows_19045) / sizeof(*g_windows_19045); ++i)
        if (!strcmp(g_windows_19045[i].name, canonical)) number = g_windows_19045[i].number;
    if (number < 0) {
        for (unsigned i = BEER_WIN32_SYSCALL_EXTRA_BASE; i < g_next_extra; ++i)
            if (!strcmp(g_services[i].name, canonical)) number = (int)i;
    }
    if (number < 0) {
        if (g_next_extra >= BEER_WIN32_SYSCALL_CAPACITY) return -1;
        number = (int)g_next_extra++;
    }
    BeerWin32Service *service = &g_services[number];
    if (!service->name[0]) memcpy(service->name, canonical, sizeof(canonical));
    if (!service->fn) service->fn = fn;
    return number;
}

size_t beer_win32_syscall_write_stub(uint8_t *out, size_t capacity, uint32_t number)
{
    static const uint8_t stub[BEER_WIN32_SYSCALL_STUB_SIZE] = {
        0x4C, 0x8B, 0xD1,                               /* mov r10, rcx */
        0xB8, 0, 0, 0, 0,                               /* mov eax, number */
        0xF6, 0x04, 0x25, 0x08, 0x03, 0xFE, 0x7F, 0x01, /* test byte ptr [7FFE0308h], 1 */
        0x75, 0x03,                                     /* jne int2e */
        0x0F, 0x05,                                     /* syscall */
        0xC3,                                           /* ret */
        0xCD, 0x2E,                                     /* int 2Eh */
        0xC3,                                           /* ret */
    };
    if (!out || capacity < sizeof(stub)) return 0;
    memcpy(out, stub, sizeof(stub));
    memcpy(out + 4, &number, sizeof(number));
    return sizeof(stub);
}

/* ---- Host or guest caller ----
 * Host code is the loader binary, shared objects and the vDSO. Everything else
 * that executes (PE images, guest allocations) is guest code. Ranges are cached
 * from /proc/self/maps, which is reread when an address is in no known range. */
typedef struct { uintptr_t start, end; uint8_t host; } BeerMapRange;
static BeerMapRange g_ranges[16384];
static _Atomic int g_range_count;
static atomic_flag g_ranges_lock = ATOMIC_FLAG_INIT;
static char g_exe_path[512];

static int mapping_is_host(const char *path)
{
    if (!path[0]) return 0;
    if (!strcmp(path, "[vdso]") || !strcmp(path, "[vsyscall]")) return 1;
    if (path[0] != '/') return 0;
    if (g_exe_path[0] && !strncmp(path, g_exe_path, strlen(g_exe_path))) return 1;
    for (const char *so = strstr(path, ".so"); so; so = strstr(so + 3, ".so"))
        if (so[3] == 0 || so[3] == '.' || so[3] == ' ') return 1;
    return 0;
}

static void ranges_refresh(void)
{
    while (atomic_flag_test_and_set_explicit(&g_ranges_lock, memory_order_acquire)) {}
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    int count = 0;
    static char buffer[1 << 16];
    static char line[1024];
    size_t line_length = 0;
    ssize_t got;
    while (fd >= 0 && (got = read(fd, buffer, sizeof(buffer))) > 0) {
        for (ssize_t i = 0; i < got; ++i) {
            char ch = buffer[i];
            if (ch != '\n') {
                if (line_length + 1 < sizeof(line)) line[line_length++] = ch;
                continue;
            }
            line[line_length] = 0;
            line_length = 0;
            unsigned long long start, end;
            char perms[8];
            int path_offset = 0;
            if (sscanf(line, "%llx-%llx %7s %*s %*s %*s %n", &start, &end, perms, &path_offset) < 3)
                continue;
            if (perms[2] != 'x' || count >= (int)(sizeof(g_ranges) / sizeof(*g_ranges))) continue;
            const char *path = path_offset ? line + path_offset : "";
            g_ranges[count].start = (uintptr_t)start;
            g_ranges[count].end = (uintptr_t)end;
            g_ranges[count].host = (uint8_t)mapping_is_host(path);
            ++count;
        }
    }
    if (fd >= 0) close(fd);
    atomic_store(&g_range_count, count);
    atomic_flag_clear_explicit(&g_ranges_lock, memory_order_release);
}

static int ranges_find(uintptr_t address)
{
    int count = atomic_load(&g_range_count);
    for (int i = 0; i < count; ++i)
        if (address >= g_ranges[i].start && address < g_ranges[i].end) return g_ranges[i].host;
    return -1;
}

static int address_is_host(uintptr_t address)
{
    int host = ranges_find(address);
    if (host < 0) {
        ranges_refresh();
        host = ranges_find(address);
    }
    return host == 1;
}

/* ---- Service dispatch ---- */
static void trace_object_name(const char *service, greg_t *regs, char *line, size_t size)
{
    uint64_t attributes = 0;
    if (!strcmp(service, "NtQueryAttributesFile") || !strcmp(service, "NtQueryFullAttributesFile"))
        attributes = (uint64_t)regs[REG_R10];
    else if (!strcmp(service, "NtCreateFile") || !strcmp(service, "NtOpenFile") ||
             !strcmp(service, "NtOpenSection"))
        attributes = (uint64_t)regs[REG_R8];
    if (!attributes) return;
    uint64_t object[3], unicode[2];
    uint16_t text[260];
    struct iovec local = { object, sizeof(object) };
    struct iovec remote = { (void *)(uintptr_t)attributes, sizeof(object) };
    if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != (ssize_t)sizeof(object)) return;
    local = (struct iovec){ unicode, sizeof(unicode) };
    remote = (struct iovec){ (void *)(uintptr_t)object[2], sizeof(unicode) };
    if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != (ssize_t)sizeof(unicode)) return;
    size_t chars = (unicode[0] & 0xffff) / 2;
    if (chars > 259) chars = 259;
    local = (struct iovec){ text, chars * 2 };
    remote = (struct iovec){ (void *)(uintptr_t)unicode[1], chars * 2 };
    if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != (ssize_t)(chars * 2)) return;
    char narrow[264];
    for (size_t i = 0; i < chars; ++i) narrow[i] = text[i] < 0x80 ? (char)text[i] : '?';
    narrow[chars] = 0;
    int length = snprintf(line, size, "[SYSCALL]   object \"%s\"\n", narrow);
    if (length > 0) (void)!write(2, line, (size_t)length);
}

static void beer_win32_syscall_signal(int sig, siginfo_t *info, void *context)
{
    (void)sig;
    beer_win32_syscall_selector = 0;        /* host code below may make system calls */
    ucontext_t *uc = (ucontext_t *)context;
    greg_t *regs = uc->uc_mcontext.gregs;
    uint64_t rip = (uint64_t)regs[REG_RIP];

    /* Host code reached without the import trampoline (a COM method, a signal
     * handler) while guest calls were blocked: allow and run the call again. */
    if (address_is_host((uintptr_t)rip)) {
        regs[REG_RAX] = (greg_t)(unsigned)info->si_syscall;
        regs[REG_RIP] = (greg_t)(rip - 2);
        return;
    }

    uint64_t status = BEER_STATUS_INVALID_SYSTEM_SERVICE;
    unsigned number = (unsigned)info->si_syscall;
    BeerWin32Service *service =
        number < BEER_WIN32_SYSCALL_CAPACITY && g_services[number].name[0] ? &g_services[number] : NULL;

    /* Stack arguments five onwards follow the return address and the 32-byte
     * home area. Read them without trusting the guest RSP. */
    uint64_t stack[8] = {0};
    struct iovec local = { stack, sizeof(stack) };
    struct iovec remote = { (void *)(uintptr_t)(regs[REG_RSP] + 0x28), sizeof(stack) };
    if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != (ssize_t)sizeof(stack))
        memset(stack, 0, sizeof(stack));

    if (service && service->fn) {
        status = (uint32_t)service->fn((uint64_t)regs[REG_R10], (uint64_t)regs[REG_RDX],
                                       (uint64_t)regs[REG_R8], (uint64_t)regs[REG_R9],
                                       stack[0], stack[1], stack[2], stack[3],
                                       stack[4], stack[5], stack[6], stack[7]);
    } else if (service) {
        status = BEER_STATUS_NOT_IMPLEMENTED;
    }

    if (g_trace < 0) g_trace = getenv("BEER_API_TRACE") != NULL;
    if (g_trace || !service || !service->fn) {
        static _Atomic uint8_t invalid_reported[BEER_WIN32_SYSCALL_CAPACITY];
        int first = service ? !service->reported
                            : number >= BEER_WIN32_SYSCALL_CAPACITY || !atomic_exchange(&invalid_reported[number], 1);
        if (g_trace || first) {
            char line[256];
            int length = snprintf(line, sizeof(line),
                                  "[SYSCALL] 0x%x %s(%llx, %llx, %llx, %llx) rip=%llx -> 0x%llx%s\n",
                                  number, service ? service->name : "<no Beer service>",
                                  (unsigned long long)regs[REG_R10], (unsigned long long)regs[REG_RDX],
                                  (unsigned long long)regs[REG_R8], (unsigned long long)regs[REG_R9],
                                  (unsigned long long)rip, (unsigned long long)status,
                                  service && !service->fn ? " (not implemented)" : "");
            if (length > 0) (void)!write(2, line, (size_t)length);
            if (service) {
                service->reported = 1;
                trace_object_name(service->name, regs, line, sizeof(line));
            }
        }
    }
    regs[REG_RAX] = (greg_t)status;
    beer_win32_syscall_resume_rip = rip;
    regs[REG_RIP] = (greg_t)(uintptr_t)beer_win32_syscall_resume;
}

void beer_win32_syscall_thread_init(void)
{
    if (!g_installed) return;
    beer_win32_syscall_selector = 0;
    if (prctl(PR_SET_SYSCALL_USER_DISPATCH, PR_SYS_DISPATCH_ON, 0, 0,
              &beer_win32_syscall_selector) != 0) {
        static int reported;
        if (!reported++) perror("[SYSCALL] PR_SET_SYSCALL_USER_DISPATCH");
    }
}

int beer_win32_syscall_install(void)
{
    ssize_t length = readlink("/proc/self/exe", g_exe_path, sizeof(g_exe_path) - 1);
    g_exe_path[length > 0 ? length : 0] = 0;

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = beer_win32_syscall_signal;
    /* A Beer implementation can run guest code (for example a callback), and
     * that code may make another system call, so SIGSYS stays deliverable
     * while the handler runs. */
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    if (sigaction(SIGSYS, &action, NULL) != 0) return 0;

    beer_win32_syscall_selector = 0;
    if (prctl(PR_SET_SYSCALL_USER_DISPATCH, PR_SYS_DISPATCH_ON, 0, 0,
              &beer_win32_syscall_selector) != 0)
        return 0;
    g_installed = 1;
    ranges_refresh();
    return 1;
}
