/* ---- Native process, thread, memory and hard-error services ----
 *
 * First-party implementations of the documented NT information contracts that
 * protected executables call through ntdll or directly through system calls
 * (see platform/win32_syscall.c). Only the information classes listed here are
 * answered. Every other class is logged once and fails with
 * STATUS_INVALID_INFO_CLASS, which is the native answer for an unsupported class.
 * Nothing here pretends that an unimplemented operation succeeded. */

#define NT_STATUS_SUCCESS               0x00000000u
#define NT_STATUS_PENDING               0x00000103u
#define NT_STATUS_INVALID_INFO_CLASS    0xC0000003u
#define NT_STATUS_INFO_LENGTH_MISMATCH  0xC0000004u
#define NT_STATUS_ACCESS_VIOLATION      0xC0000005u
#define NT_STATUS_INVALID_HANDLE        0xC0000008u
#define NT_STATUS_INVALID_PARAMETER     0xC000000Du
#define NT_STATUS_OBJECT_NAME_NOT_FOUND 0xC0000034u
#define NT_STATUS_PRIVILEGE_NOT_HELD    0xC0000061u
#define NT_STATUS_PORT_NOT_SET          0xC0000353u

static void native_info_unsupported(const char *service, u32 info_class)
{
    static _Atomic(u64) reported[64];
    u64 key = ((u64)(uintptr_t)service << 16) ^ info_class;
    for (int i = 0; i < 64; ++i) {
        u64 expected = 0;
        if (atomic_load(&reported[i]) == key) return;
        if (atomic_compare_exchange_strong(&reported[i], &expected, key)) break;
    }
    fprintf(stderr, "[NTINFO] %s class %u (0x%x) is not implemented -> STATUS_INVALID_INFO_CLASS\n",
            service, info_class, info_class);
}

static WinTEB *native_current_teb(void)
{
    u64 teb;
    __asm__ volatile("mov %%gs:0x30, %0" : "=r"(teb));
    return (WinTEB *)(uintptr_t)teb;
}

static int native_is_current_process(u64 process)
{
    return process == (u64)-1 || process == BEER_OWN_PROCESS_HANDLE;
}

/* Write a fixed-size information record with the native length checks. */
static u32 native_info_copy(void *buffer, u32 length, u32 *return_length, const void *data, u32 size)
{
    if (return_length) *return_length = size;
    if (length != size) return NT_STATUS_INFO_LENGTH_MISMATCH;
    if (!buffer) return NT_STATUS_ACCESS_VIOLATION;
    memcpy(buffer, data, size);
    return NT_STATUS_SUCCESS;
}

static _Atomic(u32) g_process_hard_error_mode = 1;

static u64 __attribute__((ms_abi))
impl_NtQueryInformationProcess(u64 process, u32 info_class, void *buffer, u32 length, u32 *return_length)
{
    if (!native_is_current_process(process)) return NT_STATUS_INVALID_HANDLE;
    WinTEB *teb = native_current_teb();
    switch (info_class) {
    case 0: {   /* ProcessBasicInformation */
        struct {
            u32 ExitStatus; u32 _pad0;
            u64 PebBaseAddress;
            u64 AffinityMask;
            s32 BasePriority; u32 _pad1;
            u64 UniqueProcessId;
            u64 InheritedFromUniqueProcessId;
        } info = { NT_STATUS_PENDING, 0, teb ? teb->ProcEnvBlk : 0,
                   win_processor_mask(), 8, 0, (u64)getpid(), (u64)getppid() };
        _Static_assert(sizeof(info) == 0x30, "PROCESS_BASIC_INFORMATION Win64 size");
        return native_info_copy(buffer, length, return_length, &info, sizeof(info));
    }
    case 7: {   /* ProcessDebugPort: no debug port is attached */
        u64 port = 0;
        return native_info_copy(buffer, length, return_length, &port, sizeof(port));
    }
    case 12: {  /* ProcessDefaultHardErrorMode, as last set (Windows starts processes with 1) */
        u32 mode = atomic_load(&g_process_hard_error_mode);
        return native_info_copy(buffer, length, return_length, &mode, sizeof(mode));
    }
    case 26: {  /* ProcessWow64Information: a native 64-bit process */
        u64 wow64 = 0;
        return native_info_copy(buffer, length, return_length, &wow64, sizeof(wow64));
    }
    case 29: {  /* ProcessBreakOnTermination: not a critical process */
        u32 critical = 0;
        return native_info_copy(buffer, length, return_length, &critical, sizeof(critical));
    }
    case 30: {  /* ProcessDebugObjectHandle: no debug object exists */
        if (return_length) *return_length = sizeof(u64);
        if (length != sizeof(u64)) return NT_STATUS_INFO_LENGTH_MISMATCH;
        if (!buffer) return NT_STATUS_ACCESS_VIOLATION;
        *(u64 *)buffer = 0;
        return NT_STATUS_PORT_NOT_SET;
    }
    case 31: {  /* ProcessDebugFlags: NoDebugInherit is set when no debugger is attached */
        u32 flags = 1;
        return native_info_copy(buffer, length, return_length, &flags, sizeof(flags));
    }
    case 36: {  /* ProcessCookie: a per-process random value, stable for the process lifetime */
        static u32 cookie;
        if (!cookie) {
            u32 value = 0;
            if (getrandom(&value, sizeof(value), 0) != (ssize_t)sizeof(value)) value = (u32)getpid() * 2654435761u;
            cookie = value ? value : 1;
        }
        return native_info_copy(buffer, length, return_length, &cookie, sizeof(cookie));
    }
    default:
        native_info_unsupported("NtQueryInformationProcess", info_class);
        if (return_length) *return_length = 0;
        return NT_STATUS_INVALID_INFO_CLASS;
    }
}

static u64 __attribute__((ms_abi))
impl_NtSetInformationProcess(u64 process, u32 info_class, const void *buffer, u32 length)
{
    if (!native_is_current_process(process)) return NT_STATUS_INVALID_HANDLE;
    switch (info_class) {
    case 12:    /* ProcessDefaultHardErrorMode: recorded; Beer has no system error UI to suppress */
        if (length != sizeof(u32)) return NT_STATUS_INFO_LENGTH_MISMATCH;
        if (!buffer) return NT_STATUS_ACCESS_VIOLATION;
        atomic_store(&g_process_hard_error_mode, *(const u32 *)buffer);
        return NT_STATUS_SUCCESS;
    case 29:    /* ProcessBreakOnTermination requires SeDebugPrivilege, which this process does not hold */
        if (length != sizeof(u32)) return NT_STATUS_INFO_LENGTH_MISMATCH;
        return NT_STATUS_PRIVILEGE_NOT_HELD;
    default:
        native_info_unsupported("NtSetInformationProcess", info_class);
        return NT_STATUS_INVALID_INFO_CLASS;
    }
}

/* ThreadHideFromDebugger is a write-once thread attribute that can be read back. */
static __thread int t_hidden_from_debugger;
static _Atomic(u8) g_thread_hidden_from_debugger[MAX_THREADS];

/* Resolve a thread handle to the current thread (1), another Beer thread (index in *other), or invalid (0). */
static int native_thread_handle(u64 thread, int *other)
{
    *other = -1;
    if (thread == (u64)-2) return 1;
    int id = thread_id_from_handle(thread);
    if (id < 0) return 0;
    if ((u64)g_threads[id].tid == current_windows_thread_id()) return 1;
    *other = id;
    return 2;
}

static u64 __attribute__((ms_abi))
impl_NtQueryInformationThread(u64 thread, u32 info_class, void *buffer, u32 length, u32 *return_length)
{
    int other;
    int kind = native_thread_handle(thread, &other);
    if (!kind) return NT_STATUS_INVALID_HANDLE;
    switch (info_class) {
    case 0: {   /* ThreadBasicInformation */
        WinTEB *teb = kind == 1 ? native_current_teb() : NULL;
        struct {
            u32 ExitStatus; u32 _pad0;
            u64 TebBaseAddress;
            u64 UniqueProcess;
            u64 UniqueThread;
            u64 AffinityMask;
            s32 Priority;
            s32 BasePriority;
        } info = { NT_STATUS_PENDING, 0, (u64)(uintptr_t)teb, (u64)getpid(),
                   kind == 1 ? current_windows_thread_id() : (u64)g_threads[other].tid,
                   win_processor_mask(), 8, 8 };
        _Static_assert(sizeof(info) == 0x30, "THREAD_BASIC_INFORMATION Win64 size");
        if (kind == 2 && g_threads[other].done) info.ExitStatus = g_threads[other].exit_code;
        return native_info_copy(buffer, length, return_length, &info, sizeof(info));
    }
    case 9: {   /* ThreadQuerySetWin32StartAddress */
        const NtHdrs64 *nt = (const NtHdrs64 *)(g_img + ((const DosHdr *)g_img)->lfanew);
        u64 start = kind == 2 ? g_threads[other].fn : (u64)(uintptr_t)g_img + nt->opt.entry_rva;
        if (kind == 1) {
            for (int id = 1; id < g_thread_cnt; ++id)
                if (g_threads[id].used && (u64)g_threads[id].tid == current_windows_thread_id())
                    start = g_threads[id].fn;
        }
        return native_info_copy(buffer, length, return_length, &start, sizeof(start));
    }
    case 17: {  /* ThreadHideFromDebugger (BOOLEAN) */
        u8 hidden = kind == 1 ? (u8)t_hidden_from_debugger
                              : atomic_load(&g_thread_hidden_from_debugger[other]);
        return native_info_copy(buffer, length, return_length, &hidden, sizeof(hidden));
    }
    case 18: {  /* ThreadBreakOnTermination */
        u32 critical = 0;
        return native_info_copy(buffer, length, return_length, &critical, sizeof(critical));
    }
    case 20: {  /* ThreadIsTerminated */
        u32 terminated = kind == 2 ? (u32)g_threads[other].done : 0;
        return native_info_copy(buffer, length, return_length, &terminated, sizeof(terminated));
    }
    default:
        native_info_unsupported("NtQueryInformationThread", info_class);
        if (return_length) *return_length = 0;
        return NT_STATUS_INVALID_INFO_CLASS;
    }
}

static u64 __attribute__((ms_abi))
impl_NtSetInformationThreadNative(u64 thread, u32 info_class, const void *info, u32 size)
{
    if (info_class == 17) {     /* ThreadHideFromDebugger takes no buffer */
        int other;
        int kind = native_thread_handle(thread, &other);
        if (!kind) return NT_STATUS_INVALID_HANDLE;
        if (size != 0) return NT_STATUS_INFO_LENGTH_MISMATCH;
        if (kind == 1) t_hidden_from_debugger = 1;
        else atomic_store(&g_thread_hidden_from_debugger[other], 1);
        return NT_STATUS_SUCCESS;
    }
    return impl_NtSetInformationThread(thread, info_class, (void *)(uintptr_t)info, size);
}

static u64 __attribute__((ms_abi))
impl_NtQueryVirtualMemory(u64 process, const void *address, u32 info_class, void *buffer,
                          u64 length, u64 *return_length)
{
    if (!native_is_current_process(process)) return NT_STATUS_INVALID_HANDLE;
    if (info_class != 0) {      /* MemoryBasicInformation only */
        native_info_unsupported("NtQueryVirtualMemory", info_class);
        return NT_STATUS_INVALID_INFO_CLASS;
    }
    if (length < sizeof(BeerMemoryBasicInformation)) return NT_STATUS_INFO_LENGTH_MISMATCH;
    if (!buffer) return NT_STATUS_ACCESS_VIOLATION;
    if ((u64)(uintptr_t)address > 0x00007FFFFFFEFFFFull) return NT_STATUS_INVALID_PARAMETER;
    size_t written = beer_virtual_query(address, (BeerMemoryBasicInformation *)buffer,
                                        sizeof(BeerMemoryBasicInformation));
    if (!written) return NT_STATUS_INVALID_PARAMETER;
    if (return_length) *return_length = written;
    return NT_STATUS_SUCCESS;
}

static u64 __attribute__((ms_abi))
impl_NtDelayExecution(u32 alertable, const s64 *interval)
{
    (void)alertable;            /* Beer has no guest APC queue to deliver while waiting */
    if (!interval) return NT_STATUS_ACCESS_VIOLATION;
    s64 value = *interval;
    if (value < 0) {            /* relative, in 100 ns units */
        u64 ns = (u64)(-value) * 100u;
        struct timespec ts = { (time_t)(ns / 1000000000u), (long)(ns % 1000000000u) };
        while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
    } else if (value > 0) {     /* absolute system time (FILETIME) */
        u64 unix_100ns = (u64)value > EPOCH_DIFF ? (u64)value - EPOCH_DIFF : 0;
        struct timespec ts = { (time_t)(unix_100ns / 10000000u), (long)(unix_100ns % 10000000u) * 100 };
        while (clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
    } else {
        sched_yield();
    }
    return NT_STATUS_SUCCESS;
}

/* NtRaiseHardError: Beer has no system hard-error UI. Report the error and its string parameters
 * and answer as a dismissed dialog, like the MessageBox implementation. */
static u64 __attribute__((ms_abi))
impl_NtRaiseHardError(u32 error_status, u32 parameter_count, u32 unicode_mask,
                      const u64 *parameters, u32 response_option, u32 *response)
{
    touch_trace_disarm();
    fprintf(stderr, "[HARDERROR] status=0x%08x option=%u", error_status, response_option);
    for (u32 i = 0; parameters && i < parameter_count && i < 8; ++i) {
        if (unicode_mask & (1u << i)) {
            const struct { u16 length, maximum; u32 pad; const u16 *buffer; } *text =
                (const void *)(uintptr_t)parameters[i];
            char narrow[256];
            size_t n = 0;
            for (; text && text->buffer && n < text->length / 2u && n + 1 < sizeof(narrow); ++n)
                narrow[n] = text->buffer[n] < 0x80 ? (char)text->buffer[n] : '?';
            narrow[n] = 0;
            fprintf(stderr, " [%u]=\"%s\"", i, narrow);
        } else {
            fprintf(stderr, " [%u]=0x%llx", i, (unsigned long long)parameters[i]);
        }
    }
    fputc('\n', stderr);
    if (!response) return NT_STATUS_ACCESS_VIOLATION;
    switch (response_option) {
    case 0: *response = 4; break;   /* OptionAbortRetryIgnore -> ResponseIgnore */
    case 1: case 2: case 7: *response = 6; break;   /* Ok, OkCancel, OkNoWait -> ResponseOk */
    case 3: case 8: *response = 3; break;           /* RetryCancel, CancelTryContinue -> ResponseCancel */
    case 4: case 5: *response = 5; break;           /* YesNo, YesNoCancel -> ResponseNo */
    case 6: return NT_STATUS_PRIVILEGE_NOT_HELD;    /* OptionShutdownSystem */
    default: return NT_STATUS_INVALID_PARAMETER;
    }
    return NT_STATUS_SUCCESS;
}

/* NtOpenSection: the \KnownDlls object directory holds image sections for system DLLs. Beer's own
 * system modules are the only system DLLs in this process, so exactly those names are present. */
static u64 __attribute__((ms_abi))
impl_NtOpenSection(u64 *section_handle, u32 desired_access, const void *object_attributes)
{
    if (!section_handle || !object_attributes) return NT_STATUS_ACCESS_VIOLATION;
    *section_handle = 0;
    u16 path[512];
    if (!nt_object_path((const u8 *)object_attributes, path, sizeof(path) / sizeof(*path)))
        return NT_STATUS_INVALID_PARAMETER;
    static const char prefix[] = "\\KnownDlls\\";
    char name[128];
    size_t i = 0;
    for (; prefix[i]; ++i) {
        u16 ch = path[i];
        if (ch >= 'a' && ch <= 'z') ch = (u16)(ch - 32);
        char want = prefix[i] >= 'a' && prefix[i] <= 'z' ? (char)(prefix[i] - 32) : prefix[i];
        if (ch != (u8)want) goto not_found;
    }
    size_t n = 0;
    for (; path[i] && n + 1 < sizeof(name); ++i, ++n) {
        if (path[i] > 0x7f || path[i] == '\\') goto not_found;
        name[n] = (char)path[i];
    }
    name[n] = 0;
    {
        u64 file = synthetic_system_module_file(name);
        if (!file || file == INVALID_HANDLE_VALUE64) goto not_found;
        u64 status = impl_NtCreateSection(section_handle, desired_access, NULL, NULL,
                                          2 /* PAGE_READONLY */, 0x1000000 /* SEC_IMAGE */, file);
        beer_NtClose(file);
        return status;
    }
not_found:
    if (getenv("BEER_NT_SECTION_DIAGNOSTICS")) {
        char narrow[512];
        size_t k = 0;
        for (; path[k] && k + 1 < sizeof(narrow); ++k) narrow[k] = path[k] < 0x80 ? (char)path[k] : '?';
        narrow[k] = 0;
        fprintf(stderr, "[NTSECTION] open %s -> STATUS_OBJECT_NAME_NOT_FOUND\n", narrow);
    }
    return NT_STATUS_OBJECT_NAME_NOT_FOUND;
}

/* NtReadVirtualMemory / NtWriteVirtualMemory for the current process. The copy goes through
 * process_vm_readv/process_vm_writev, so an inaccessible range fails the way the native copy does
 * (STATUS_PARTIAL_COPY with the transferred byte count) and never faults the caller. */
#define NT_STATUS_PARTIAL_COPY 0x8000000Du

static u64 native_copy_virtual_memory(u64 process, u64 remote_address, u64 local_address, u64 size,
                                      u64 *transferred, int write)
{
    if (transferred) *transferred = 0;
    if (!native_is_current_process(process)) return NT_STATUS_INVALID_HANDLE;
    if (!size) return NT_STATUS_SUCCESS;
    if (remote_address + size < remote_address || remote_address + size > 0x00007FFFFFFF0000ull ||
        local_address + size < local_address || local_address + size > 0x00007FFFFFFF0000ull)
        return NT_STATUS_ACCESS_VIOLATION;
    struct iovec local = { (void *)(uintptr_t)local_address, size };
    struct iovec remote = { (void *)(uintptr_t)remote_address, size };
    ssize_t done = write ? process_vm_writev(getpid(), &local, 1, &remote, 1, 0)
                         : process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    if (done < 0) done = 0;
    if (transferred) *transferred = (u64)done;
    return (u64)done == size ? NT_STATUS_SUCCESS : NT_STATUS_PARTIAL_COPY;
}

static u64 __attribute__((ms_abi))
impl_NtReadVirtualMemory(u64 process, u64 base_address, void *buffer, u64 size, u64 *bytes_read)
{
    return native_copy_virtual_memory(process, base_address, (u64)(uintptr_t)buffer, size, bytes_read, 0);
}

static u64 __attribute__((ms_abi))
impl_NtWriteVirtualMemory(u64 process, u64 base_address, const void *buffer, u64 size, u64 *bytes_written)
{
    return native_copy_virtual_memory(process, base_address, (u64)(uintptr_t)buffer, size, bytes_written, 1);
}

/* ---- shlwapi PathRemoveFileSpec{A,W}: remove the trailing file name and its backslash, keeping a root
 * ("C:\", "\") intact. Returns TRUE when something was removed. ---- */
#define PATH_REMOVE_FILE_SPEC(name, type)                                                       \
static u64 __attribute__((ms_abi)) name(type *path)                                             \
{                                                                                               \
    if (!path || !path[0]) return 0;                                                            \
    size_t length = 0;                                                                          \
    while (path[length]) ++length;                                                              \
    size_t root = 0;                                                                            \
    if (length >= 2 && path[1] == ':') root = 2;                                                \
    if (length > root && path[root] == '\\') ++root;                                            \
    size_t slash = (size_t)-1;                                                                  \
    for (size_t i = root; i < length; ++i) if (path[i] == '\\') slash = i;                      \
    if (slash == (size_t)-1) {                                                                  \
        if (length == root) return 0;          /* already a root */                             \
        path[root] = 0;                        /* "C:\file" -> "C:\", "file" -> "" */           \
        return 1;                                                                               \
    }                                                                                           \
    path[slash] = 0;                                                                            \
    return 1;                                                                                   \
}
PATH_REMOVE_FILE_SPEC(impl_PathRemoveFileSpecW, u16)
PATH_REMOVE_FILE_SPEC(impl_PathRemoveFileSpecA, char)

/* Copy a guest UTF-16 string to ASCII without trusting the pointer (diagnostic text only). */
static void native_safe_narrow(u64 address, char *out, size_t size)
{
    size_t n = 0;
    while (address >= 0x10000 && n + 1 < size) {
        u16 ch = 0;
        struct iovec local = { &ch, 2 }, remote = { (void *)(uintptr_t)(address + n * 2), 2 };
        if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != 2 || !ch) break;
        out[n++] = ch < 0x80 ? (char)ch : '?';
    }
    out[n] = 0;
}

/* ---- ShellExecuteExW: Beer cannot start another Windows program. Report the request and fail
 * the documented way (FALSE, hInstApp = SE_ERR_NOASSOC, no process). ---- */
static u64 __attribute__((ms_abi)) impl_ShellExecuteExW(u8 *info)
{
    if (!info || *(const u32 *)info < 0x70) { g_last_error = 87; return 0; }
    char verb[128] = "", file[512] = "", parameters[512] = "", directory[512] = "";
    native_safe_narrow(*(const u64 *)(info + 0x10), verb, sizeof(verb));
    native_safe_narrow(*(const u64 *)(info + 0x18), file, sizeof(file));
    native_safe_narrow(*(const u64 *)(info + 0x20), parameters, sizeof(parameters));
    native_safe_narrow(*(const u64 *)(info + 0x28), directory, sizeof(directory));
    fprintf(stderr, "[SHELL] ShellExecuteExW(mask=0x%x verb=\"%s\" file=\"%s\" params=\"%s\" dir=\"%s\") -> "
                    "not supported\n", *(const u32 *)(info + 4), verb, file, parameters, directory);
    beer_log_guest_stack("SHELL");
    *(u64 *)(info + 0x38) = 31;     /* hInstApp = SE_ERR_NOASSOC */
    *(u64 *)(info + 0x68) = 0;      /* hProcess */
    g_last_error = 1155;            /* ERROR_NO_ASSOCIATION */
    return 0;
}

/* ---- String-table resources (RT_STRING = 6). String id N lives in block N/16 + 1 as entry N%16; a
 * block is 16 consecutive { u16 length; u16 text[length]; } records. ---- */
static const u16 *native_string_resource(u64 module, u32 id, u32 *length)
{
    u64 resource = beer_resource_find(module ? module : (u64)(uintptr_t)g_img, (u64)(id / 16 + 1), 6, 0);
    u32 size = 0;
    const u8 *data = resource ? beer_resource_data(module ? module : (u64)(uintptr_t)g_img, resource, &size) : NULL;
    const u8 *end = data ? data + size : NULL;
    for (u32 entry = 0; data && data + 2 <= end; ++entry) {
        u16 count = *(const u16 *)data;
        if (data + 2 + (size_t)count * 2 > end) return NULL;
        if (entry == id % 16) { *length = count; return (const u16 *)(data + 2); }
        data += 2 + (size_t)count * 2;
    }
    return NULL;
}

static u64 __attribute__((ms_abi)) impl_LoadStringW(u64 module, u32 id, u16 *buffer, s32 capacity)
{
    if (!buffer) { g_last_error = 87; return 0; }
    u32 length = 0;
    const u16 *text = native_string_resource(module, id, &length);
    if (!text || !length) {
        if (capacity > 0) buffer[0] = 0;
        g_last_error = 1814;                    /* ERROR_RESOURCE_NAME_NOT_FOUND */
        return 0;
    }
    if (capacity == 0) {                        /* read-only pointer to the resource text */
        *(const u16 **)buffer = text;
        return length;
    }
    u32 copy = length < (u32)capacity - 1 ? length : (u32)capacity - 1;
    memcpy(buffer, text, copy * 2);
    buffer[copy] = 0;
    return copy;
}

/* ---- comctl32 TaskDialogIndirect (ordinal 345): Beer has no dialog UI. Report the dialog's text and
 * answer like a dismissed dialog, as the MessageBox implementation does. TASKDIALOGCONFIG is declared
 * with 1-byte packing: hInstance@0x0c pszWindowTitle@0x1c pszMainInstruction@0x2c pszContent@0x34.
 * MAKEINTRESOURCE strings are looked up in hInstance's string table. ---- */
static void task_dialog_text(const u8 *config, size_t offset, char *out, size_t size)
{
    u64 value;
    memcpy(&value, config + offset, sizeof(value));
    if (!value) { out[0] = 0; return; }
    if (value < 0x10000) {                      /* MAKEINTRESOURCE string id in hInstance (config+0x10) */
        u32 length = 0;
        u64 instance;
        memcpy(&instance, config + 0x0c, sizeof(instance));
        const u16 *text = native_string_resource(instance, (u32)value, &length);
        size_t n = 0;
        for (; text && n < length && n + 1 < size; ++n) out[n] = text[n] < 0x80 ? (char)text[n] : '?';
        out[n] = 0;
        if (!text) snprintf(out, size, "#%llu", (unsigned long long)value);
        return;
    }
    native_safe_narrow(value, out, size);
}

static u64 __attribute__((ms_abi))
impl_TaskDialogIndirect(const u8 *config, s32 *button, s32 *radio_button, u32 *verification_checked)
{
    if (!config || *(const u32 *)config < 0x60) return 0x80070057u;   /* E_INVALIDARG */
    char title[256], instruction[1024], content[2048];
    task_dialog_text(config, 0x1c, title, sizeof(title));
    task_dialog_text(config, 0x2c, instruction, sizeof(instruction));
    task_dialog_text(config, 0x34, content, sizeof(content));
    touch_trace_disarm();
    fprintf(stderr, "[TASKDIALOG] \"%s\": %s%s%s\n", title, instruction, content[0] ? " | " : "", content);
    if (getenv("BEER_TASKDIALOG_RAW"))
        for (int i = 0; i < 12; ++i)
            fprintf(stderr, "[TASKDIALOG]   +0x%02x = 0x%llx\n", i * 8, (unsigned long long)*(const u64 *)(config + i * 8));
    beer_log_guest_stack("TASKDIALOG");
    if (button) *button = 1;                /* IDOK */
    if (radio_button) *radio_button = 0;
    if (verification_checked) *verification_checked = 0;
    return 0;                               /* S_OK */
}

/* InitCommonControlsEx: Beer's controls need no class registration. */
static u64 __attribute__((ms_abi)) impl_InitCommonControlsEx(const void *init) { return init ? 1 : 0; }
