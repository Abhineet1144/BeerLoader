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

/* NtRaiseHardError: report the error and its string parameters. A STATUS_SERVICE_NOTIFICATION is shown
 * as a message box (beer_message_dialog); without a display it is answered as a dismissed dialog. */
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
    /* STATUS_SERVICE_NOTIFICATION carries a MessageBox: text, caption and MB_* type. Show it and map
     * the IDxxx answer to the HARDERROR_RESPONSE values. */
    if (error_status == 0x50000018u && parameters && parameter_count >= 3 && (unicode_mask & 3) == 3) {
        char text[2048], caption[256];
        for (int which = 0; which < 2; ++which) {
            u8 unicode[16] = {0};
            struct iovec local = { unicode, 16 }, remote = { (void *)(uintptr_t)parameters[which], 16 };
            char *out = which ? caption : text;
            size_t size = which ? sizeof(caption) : sizeof(text);
            out[0] = 0;
            if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == 16) {
                u64 buffer;
                memcpy(&buffer, unicode + 8, 8);
                native_safe_narrow(buffer, out, size);
                u16 length;
                memcpy(&length, unicode, 2);
                if ((size_t)length / 2 < size) out[length / 2] = 0;
            }
        }
        int chosen = beer_message_box_dialog(caption, text, (unsigned)parameters[2]);
        if (chosen >= 0) {
            static const u32 response_for_id[12] = { 0, 6, 3, 2, 7, 4, 8, 5, 0, 0, 9, 10 };
            *response = chosen < 12 ? response_for_id[chosen] : 6;
            return NT_STATUS_SUCCESS;
        }
    }
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

/* shlwapi PathFindFileName{A,W}: return a pointer into the caller-owned path,
 * immediately after its last DOS separator. A trailing separator returns the
 * terminating NUL; a path without a separator returns the original pointer. */
#define PATH_FIND_FILE_NAME(name, type)                                                        \
static u64 __attribute__((ms_abi)) name(const type *path)                                      \
{                                                                                               \
    if (!path) return 0;                                                                         \
    const type *base = path;                                                                     \
    for (const type *cursor = path; *cursor; ++cursor)                                          \
        if (*cursor == '\\' || *cursor == '/') base = cursor + 1;                              \
    return (u64)(uintptr_t)base;                                                                 \
}
PATH_FIND_FILE_NAME(impl_PathFindFileNameW, u16)
PATH_FIND_FILE_NAME(impl_PathFindFileNameA, char)


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

/* ---- comctl32 TaskDialogIndirect (ordinal 345): shown with beer_message_dialog (answered with the
 * default button when no display is available). TASKDIALOGCONFIG is declared
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
    /* Buttons: custom TASKDIALOG_BUTTON { int id; PCWSTR text; } (packed, 12 bytes) from cButtons@0x3c
     * and pButtons@0x40, then the common buttons from dwCommonButtons@0x18. */
    int ids[8];
    const char *labels[8];
    static char custom_labels[8][64];
    int count = 0, cancel = -1;
    u32 custom_count, common, flags;
    u64 custom;
    s32 default_id;
    memcpy(&custom_count, config + 0x3c, 4);
    memcpy(&custom, config + 0x40, 8);
    memcpy(&common, config + 0x18, 4);
    memcpy(&flags, config + 0x14, 4);
    memcpy(&default_id, config + 0x48, 4);
    for (u32 i = 0; custom && i < custom_count && count < 8; ++i) {
        u8 entry[12];
        struct iovec local = { entry, 12 }, remote = { (void *)(uintptr_t)(custom + i * 12), 12 };
        if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != 12) break;
        u64 text_address;
        memcpy(&ids[count], entry, 4);
        memcpy(&text_address, entry + 4, 8);
        native_safe_narrow(text_address, custom_labels[count], sizeof(custom_labels[count]));
        labels[count] = custom_labels[count];
        ++count;
    }
    static const struct { u32 flag; int id; const char *label; } common_buttons[] = {
        { 0x01, 1, "OK" }, { 0x02, 6, "Yes" }, { 0x04, 7, "No" },
        { 0x10, 4, "Retry" }, { 0x08, 2, "Cancel" }, { 0x20, 8, "Close" },
    };
    for (size_t i = 0; i < sizeof(common_buttons) / sizeof(*common_buttons) && count < 8; ++i)
        if (common & common_buttons[i].flag) {
            if (common_buttons[i].id == 2) cancel = count;
            ids[count] = common_buttons[i].id;
            labels[count++] = common_buttons[i].label;
        }
    if (!count) { ids[count] = 1; labels[count++] = "OK"; }
    if (cancel < 0 && (flags & 0x8)) cancel = count - 1;   /* TDF_ALLOW_DIALOG_CANCELLATION */
    int default_index = 0;
    for (int i = 0; i < count; ++i) if (ids[i] == default_id) default_index = i;
    static char body[3200];
    snprintf(body, sizeof(body), "%s%s%s", instruction, instruction[0] && content[0] ? "\n\n" : "", content);
    int chosen = beer_message_dialog(title, body, ids, labels, count, default_index, cancel);
    if (button) *button = chosen >= 0 ? chosen : ids[default_index];
    if (radio_button) *radio_button = 0;
    if (verification_checked) *verification_checked = 0;
    return 0;                               /* S_OK */
}

/* InitCommonControlsEx: Beer's controls need no class registration. */
static u64 __attribute__((ms_abi)) impl_InitCommonControlsEx(const void *init) { return init ? 1 : 0; }

/* ---- WinRT strings (HSTRING), first-party. NULL is the empty string. A reference string
 * (WindowsCreateStringReference) lives in the caller's 24-byte HSTRING_HEADER and points at the
 * caller's NUL-terminated buffer; WindowsCreateString/Duplicate allocate a reference-counted copy. ---- */
typedef struct {
    u32 flags;                  /* 1 = reference string in a caller HSTRING_HEADER */
    u32 length;                 /* UTF-16 code units, excluding the terminator */
    const u16 *buffer;
    _Atomic(u32) references;    /* heap strings only */
} BeerHString;
_Static_assert(sizeof(BeerHString) <= 24, "HSTRING_HEADER is 24 bytes on x64");

#define BEER_E_INVALIDARG 0x80070057u
#define BEER_E_OUTOFMEMORY 0x8007000Eu

static u64 hstring_new(const u16 *source, u32 length, BeerHString **out)
{
    BeerHString *string = malloc(sizeof(*string) + ((size_t)length + 1) * 2);
    if (!string) return BEER_E_OUTOFMEMORY;
    u16 *text = (u16 *)(string + 1);
    if (length) memcpy(text, source, (size_t)length * 2);
    text[length] = 0;
    string->flags = 0;
    string->length = length;
    string->buffer = text;
    atomic_store(&string->references, 1);
    *out = string;
    return 0;
}

static u64 __attribute__((ms_abi)) impl_WindowsCreateString(const u16 *source, u32 length, BeerHString **out)
{
    if (!out || (!source && length)) return BEER_E_INVALIDARG;
    *out = NULL;
    return length ? hstring_new(source, length, out) : 0;
}

static u64 __attribute__((ms_abi))
impl_WindowsCreateStringReference(const u16 *source, u32 length, BeerHString *header, BeerHString **out)
{
    if (!out || !header || (!source && length)) return BEER_E_INVALIDARG;
    *out = NULL;
    if (!length) return 0;
    if (source[length] != 0) return BEER_E_INVALIDARG;   /* the source must be NUL-terminated at length */
    memset(header, 0, 24);
    header->flags = 1;
    header->length = length;
    header->buffer = source;
    *out = header;
    return 0;
}

static u64 __attribute__((ms_abi)) impl_WindowsDeleteString(BeerHString *string)
{
    if (string && !(string->flags & 1) && atomic_fetch_sub(&string->references, 1) == 1) free(string);
    return 0;
}

static u64 __attribute__((ms_abi)) impl_WindowsDuplicateString(BeerHString *string, BeerHString **out)
{
    if (!out) return BEER_E_INVALIDARG;
    *out = NULL;
    if (!string) return 0;
    if (string->flags & 1) return hstring_new(string->buffer, string->length, out);
    atomic_fetch_add(&string->references, 1);
    *out = string;
    return 0;
}

static u64 __attribute__((ms_abi)) impl_WindowsGetStringRawBuffer(const BeerHString *string, u32 *length)
{
    static const u16 empty[1] = { 0 };
    if (length) *length = string ? string->length : 0;
    return (u64)(uintptr_t)(string ? string->buffer : empty);
}

static u64 __attribute__((ms_abi)) impl_WindowsGetStringLen(const BeerHString *string)
{
    return string ? string->length : 0;
}

/* ---- Windows.Gaming.UI.GameBar (IGameBarStatics), first-party. Beer has no Game Bar overlay, so it is
 * never visible and never redirects input; its change events can be registered but never fire, exactly
 * what a never-opened Game Bar reports. The statics object is a process-lifetime singleton. ---- */
typedef struct { const void *const *vtable; } BeerWinRtObject;

static const u8 g_iid_iunknown[16]     = { 0,0,0,0, 0,0, 0,0, 0xC0,0,0,0,0,0,0,0x46 };
static const u8 g_iid_iinspectable[16] = { 0xE0,0xE2,0x86,0xAF, 0x2D,0xB1, 0x6A,0x4C, 0x9C,0x5A,0xD7,0xAA,0x65,0x10,0x1E,0x90 };
static const u8 g_iid_iagile[16]       = { 0x94,0x2B,0xEA,0x94, 0xCC,0xE9, 0xE0,0x49, 0xC0,0xFF,0xEE,0x64,0xCA,0x8F,0x5B,0x90 };
static const u8 g_iid_game_bar_statics[16] = { 0x92,0xA2,0xB9,0x1D, 0x78,0xCC, 0x73,0x41, 0xBE,0x45,0xB6,0x1E,0x67,0x28,0x3E,0xA7 };
static _Atomic(s64) g_winrt_event_token = 1;

static u64 __attribute__((ms_abi)) game_bar_query_interface(BeerWinRtObject *self, const u8 *iid, void **out)
{
    if (!out) return 0x80004003u;               /* E_POINTER */
    *out = NULL;
    if (!iid) return 0x80070057u;
    if (memcmp(iid, g_iid_iunknown, 16) && memcmp(iid, g_iid_iinspectable, 16) &&
        memcmp(iid, g_iid_iagile, 16) && memcmp(iid, g_iid_game_bar_statics, 16))
        return 0x80004002u;                     /* E_NOINTERFACE */
    *out = self;
    return 0;
}
static u64 __attribute__((ms_abi)) game_bar_add_ref(BeerWinRtObject *self) { (void)self; return 2; }
static u64 __attribute__((ms_abi)) game_bar_release(BeerWinRtObject *self) { (void)self; return 1; }
static u64 __attribute__((ms_abi)) game_bar_get_iids(BeerWinRtObject *self, u32 *count, void **iids)
{
    (void)self;
    if (!count || !iids) return 0x80004003u;
    *count = 0;                                 /* no additional interfaces beyond IGameBarStatics' own */
    *iids = NULL;
    return 0;
}
static u64 __attribute__((ms_abi)) game_bar_get_runtime_class_name(BeerWinRtObject *self, BeerHString **name)
{
    (void)self;
    static const u16 class_name[] = { 'W','i','n','d','o','w','s','.','G','a','m','i','n','g','.',
                                      'U','I','.','G','a','m','e','B','a','r' };
    if (!name) return 0x80004003u;
    return impl_WindowsCreateString(class_name, (u32)(sizeof(class_name) / 2), name);
}
static u64 __attribute__((ms_abi)) game_bar_get_trust_level(BeerWinRtObject *self, u32 *level)
{
    (void)self;
    if (!level) return 0x80004003u;
    *level = 0;                                 /* BaseTrust */
    return 0;
}
static u64 __attribute__((ms_abi)) game_bar_add_event(BeerWinRtObject *self, void *handler, s64 *token)
{
    (void)self;
    if (!handler || !token) return 0x80004003u;
    *token = atomic_fetch_add(&g_winrt_event_token, 1);
    return 0;
}
static u64 __attribute__((ms_abi)) game_bar_remove_event(BeerWinRtObject *self, s64 token)
{
    (void)self; (void)token;
    return 0;
}
static u64 __attribute__((ms_abi)) game_bar_get_false(BeerWinRtObject *self, u8 *value)
{
    (void)self;
    if (!value) return 0x80004003u;
    *value = 0;
    return 0;
}
/* IUnknown, IInspectable, then IGameBarStatics: add_/remove_VisibilityChanged,
 * add_/remove_IsInputRedirectedChanged, get_Visible, get_IsInputRedirected. */
static const void *const g_game_bar_statics_vtable[] = {
    (const void *)game_bar_query_interface, (const void *)game_bar_add_ref, (const void *)game_bar_release,
    (const void *)game_bar_get_iids, (const void *)game_bar_get_runtime_class_name,
    (const void *)game_bar_get_trust_level,
    (const void *)game_bar_add_event, (const void *)game_bar_remove_event,
    (const void *)game_bar_add_event, (const void *)game_bar_remove_event,
    (const void *)game_bar_get_false, (const void *)game_bar_get_false,
};
static BeerWinRtObject g_game_bar_statics = { g_game_bar_statics_vtable };

/* ---- WinRT activation. Beer provides only the runtime classes implemented here; any other class
 * answers REGDB_E_CLASSNOTREG with a NULL factory (the native result for an unregistered class), and
 * is logged so missing classes are visible. ---- */
static u64 __attribute__((ms_abi)) impl_RoGetActivationFactory(const BeerHString *class_id, const u8 *iid, void **factory)
{
    char name[256];
    u32 length = 0;
    const u16 *text = (const u16 *)(uintptr_t)impl_WindowsGetStringRawBuffer(class_id, &length);
    size_t n = 0;
    for (; n < length && n + 1 < sizeof(name); ++n) name[n] = text[n] < 0x80 ? (char)text[n] : '?';
    name[n] = 0;
    if (!factory) return 0x80004003u;           /* E_POINTER */
    *factory = NULL;
    if (!strcmp(name, "Windows.Gaming.UI.GameBar"))
        return game_bar_query_interface(&g_game_bar_statics, iid, factory);
    fprintf(stderr, "[WINRT] RoGetActivationFactory(\"%s\") -> REGDB_E_CLASSNOTREG (class not provided)\n", name);
    return 0x80040154u;                         /* REGDB_E_CLASSNOTREG */
}

/* ---- ReadConsoleA/W: Beer attaches no console to the process, so every handle is a non-console
 * handle and the read fails as on Windows (FALSE, ERROR_INVALID_HANDLE, nothing read). ---- */
static u64 __attribute__((ms_abi))
impl_ReadConsoleW(u64 console, void *buffer, u32 count, u32 *read, const void *control)
{
    (void)console; (void)buffer; (void)count; (void)control;
    if (read) *read = 0;
    g_last_error = 6;                           /* ERROR_INVALID_HANDLE */
    return 0;
}
static u64 __attribute__((ms_abi))
impl_ReadConsoleA(u64 console, void *buffer, u32 count, u32 *read, const void *control)
{
    return impl_ReadConsoleW(console, buffer, count, read, control);
}

/* SwitchToThread: yield the processor to another ready thread. Linux does not report whether another
 * thread ran, so the call answers TRUE (a switch was requested) only when the host yield succeeded. */
static u64 __attribute__((ms_abi)) impl_SwitchToThread(void)
{
    return sched_yield() == 0 ? 1 : 0;
}

/* PROPVARIANT (24 bytes on Win64): vt at +0, value at +8. Strings returned by property stores are
 * CoTaskMem (host heap) allocations; BSTRs belong to the OleAut32 allocator. */
static u64 __attribute__((ms_abi)) impl_PropVariantClear(u8 *value)
{
    if (!value) return 0;
    u16 vt = *(u16 *)value;
    void *data = *(void **)(value + 8);
    if ((vt == 30 || vt == 31) && data) free(data);                       /* VT_LPSTR / VT_LPWSTR */
    else if (vt == 8 && data) impl_SysFreeString(data);                   /* VT_BSTR */
    else if ((vt == 9 || vt == 13) && data)                               /* VT_DISPATCH / VT_UNKNOWN */
        ((u64 (__attribute__((ms_abi)) *)(void *))(*(void ***)data)[2])(data);
    else if (vt != 0 && vt != 1 && vt != 2 && vt != 3 && vt != 4 && vt != 5 && vt != 10 &&
             vt != 11 && vt != 16 && vt != 17 && vt != 18 && vt != 19 && vt != 20 && vt != 21 &&
             vt != 22 && vt != 23 && vt != 64)
        return 0x80070057;                                                /* E_INVALIDARG: unsupported vt */
    memset(value, 0, 24);
    return 0;
}

/* PropVariantToStringAlloc: string and integer variants convert to a new CoTaskMem string;
 * VT_EMPTY yields an empty string, other types report TYPE_E_TYPEMISMATCH. */
static u64 __attribute__((ms_abi)) impl_PropVariantToStringAlloc(const u8 *value, u16 **out)
{
    if (!out) return 0x80004003;                                          /* E_POINTER */
    *out = NULL;
    if (!value) return 0x80070057;
    u16 vt = *(const u16 *)value;
    const u8 *data = value + 8;
    char number[32];
    const char *narrow = NULL;
    const u16 *wide = NULL;
    switch (vt) {
    case 0: case 1: narrow = ""; break;                                   /* VT_EMPTY / VT_NULL */
    case 31: case 8: wide = *(const u16 *const *)data; if (!wide) narrow = ""; break;
    case 30: narrow = *(const char *const *)data; if (!narrow) narrow = ""; break;
    case 2:  snprintf(number, sizeof(number), "%d", *(const int16_t *)data); narrow = number; break;
    case 18: snprintf(number, sizeof(number), "%u", *(const uint16_t *)data); narrow = number; break;
    case 3: case 22: snprintf(number, sizeof(number), "%d", *(const int32_t *)data); narrow = number; break;
    case 19: case 23: snprintf(number, sizeof(number), "%u", *(const uint32_t *)data); narrow = number; break;
    case 20: snprintf(number, sizeof(number), "%lld", (long long)*(const int64_t *)data); narrow = number; break;
    case 21: snprintf(number, sizeof(number), "%llu", (unsigned long long)*(const uint64_t *)data); narrow = number; break;
    case 16: snprintf(number, sizeof(number), "%d", *(const int8_t *)data); narrow = number; break;
    case 17: snprintf(number, sizeof(number), "%u", *data); narrow = number; break;
    case 11: narrow = *(const int16_t *)data ? "1" : "0"; break;          /* VT_BOOL */
    default: return 0x80028CA0;                                           /* TYPE_E_TYPEMISMATCH */
    }
    size_t length = 0;
    if (wide) while (wide[length]) length++;
    else length = strlen(narrow);
    u16 *copy = malloc((length + 1) * sizeof(u16));
    if (!copy) return 0x8007000E;                                         /* E_OUTOFMEMORY */
    for (size_t i = 0; i < length; i++) copy[i] = wide ? wide[i] : (u8)narrow[i];
    copy[length] = 0;
    *out = copy;
    return 0;
}
