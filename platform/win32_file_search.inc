/* Directory enumeration and a few filesystem queries that Windows programs (installers, packers,
 * archive scanners, runtimes) rely on: FindFirstFile/FindNextFile/FindClose, GetTempFileName,
 * GetLogicalDriveStrings, GetFileAttributesEx, GetVolumeInformation, GetComputerName.
 * Included by loader.c after the path-mapping helpers. */
#include <dirent.h>
#include <fnmatch.h>

#define BEER_FIND_MAGIC 0x46494E4442454552ull   /* "FINDBEER" */
typedef struct {
    u64 magic;
    DIR *dir;
    char host_dir[PATH_MAX];
    char pattern[260];                /* UTF-8, as given by the caller (file part only) */
    int emitted_dots;
    int wide;
} BeerFindState;

static u64 beer_unix_to_filetime(time_t sec, long nsec)
{
    return ((u64)sec + 11644473600ull) * 10000000ull + (u64)(nsec / 100);
}

static size_t beer_utf8_to_utf16(const char *src, u16 *dst, size_t cap)
{
    size_t n = 0;
    while (*src && n + 2 < cap) {
        u32 c = (u8)*src++;
        if (c >= 0xf0 && src[0] && src[1] && src[2]) { c = ((c & 7) << 18) | (((u8)src[0] & 63) << 12) | (((u8)src[1] & 63) << 6) | ((u8)src[2] & 63); src += 3; }
        else if (c >= 0xe0 && src[0] && src[1]) { c = ((c & 15) << 12) | (((u8)src[0] & 63) << 6) | ((u8)src[1] & 63); src += 2; }
        else if (c >= 0xc0 && src[0]) { c = ((c & 31) << 6) | ((u8)src[0] & 63); src += 1; }
        if (c >= 0x10000) { c -= 0x10000; dst[n++] = (u16)(0xd800 + (c >> 10)); dst[n++] = (u16)(0xdc00 + (c & 0x3ff)); }
        else dst[n++] = (u16)c;
    }
    dst[n] = 0;
    return n;
}

static size_t beer_utf16_to_utf8(const u16 *src, char *dst, size_t cap)
{
    size_t n = 0;
    while (*src && n + 4 < cap) {
        u32 c = *src++;
        if (c >= 0xd800 && c < 0xdc00 && *src >= 0xdc00 && *src < 0xe000) { c = 0x10000 + ((c - 0xd800) << 10) + (*src++ - 0xdc00); }
        if (c < 0x80) dst[n++] = (char)c;
        else if (c < 0x800) { dst[n++] = (char)(0xc0 | (c >> 6)); dst[n++] = (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { dst[n++] = (char)(0xe0 | (c >> 12)); dst[n++] = (char)(0x80 | ((c >> 6) & 63)); dst[n++] = (char)(0x80 | (c & 63)); }
        else { dst[n++] = (char)(0xf0 | (c >> 18)); dst[n++] = (char)(0x80 | ((c >> 12) & 63)); dst[n++] = (char)(0x80 | ((c >> 6) & 63)); dst[n++] = (char)(0x80 | (c & 63)); }
    }
    dst[n] = 0;
    return n;
}

static int beer_find_fill(BeerFindState *st, const char *name, void *out)
{
    char full[PATH_MAX + 300];
    snprintf(full, sizeof(full), "%s/%s", st->host_dir, name);
    struct stat sb;
    if (stat(full, &sb) != 0) memset(&sb, 0, sizeof(sb));
    u32 attr = S_ISDIR(sb.st_mode) ? 0x10u : 0x80u;
    if (!(sb.st_mode & 0222)) attr |= 0x1u;
    u8 *o = out;
    memset(o, 0, st->wide ? 592 : 320);
    memcpy(o, &attr, 4);
    u64 created = beer_unix_to_filetime(sb.st_ctim.tv_sec, sb.st_ctim.tv_nsec);
    u64 accessed = beer_unix_to_filetime(sb.st_atim.tv_sec, sb.st_atim.tv_nsec);
    u64 written = beer_unix_to_filetime(sb.st_mtim.tv_sec, sb.st_mtim.tv_nsec);
    memcpy(o + 4, &created, 8); memcpy(o + 12, &accessed, 8); memcpy(o + 20, &written, 8);
    u32 size_high = S_ISDIR(sb.st_mode) ? 0 : (u32)((u64)sb.st_size >> 32);
    u32 size_low = S_ISDIR(sb.st_mode) ? 0 : (u32)((u64)sb.st_size & 0xffffffffu);
    memcpy(o + 28, &size_high, 4); memcpy(o + 32, &size_low, 4);
    if (st->wide) beer_utf8_to_utf16(name, (u16 *)(o + 44), 260);
    else { strncpy((char *)(o + 44), name, 259); }
    return 1;
}

static int beer_find_next_entry(BeerFindState *st, void *out)
{
    for (;;) {
        const char *name;
        struct dirent *de = NULL;
        if (st->emitted_dots < 2) {
            name = st->emitted_dots == 0 ? "." : "..";
            st->emitted_dots++;
        } else {
            de = readdir(st->dir);
            if (!de) return 0;
            name = de->d_name;
            if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        }
        const char *pat = st->pattern;
        int all = !*pat || !strcmp(pat, "*") || !strcmp(pat, "*.*");
        if (!all && fnmatch(pat, name, FNM_CASEFOLD) != 0) continue;
        return beer_find_fill(st, name, out);
    }
}

static u64 beer_find_first(const char *win_path, void *out, int wide)
{
    char host[PATH_MAX];
    char dir_part[PATH_MAX];
    const char *slash = strrchr(win_path, '\\');
    const char *fwd = strrchr(win_path, '/');
    if (fwd && (!slash || fwd > slash)) slash = fwd;
    size_t dir_len = slash ? (size_t)(slash - win_path) + 1 : 0;
    if (dir_len >= sizeof(dir_part)) { g_last_error = 206; return INVALID_HANDLE_VALUE64; }
    if (dir_len) { memcpy(dir_part, win_path, dir_len); dir_part[dir_len] = 0; }
    else snprintf(dir_part, sizeof(dir_part), ".\\");
    if (!resolve_windows_host_path(dir_part, host, sizeof(host))) { g_last_error = 3; return INVALID_HANDLE_VALUE64; }
    DIR *dir = opendir(host);
    if (!dir) { g_last_error = errno == ENOTDIR || errno == ENOENT ? 3 : 5; return INVALID_HANDLE_VALUE64; }
    BeerFindState *st = calloc(1, sizeof(*st));
    if (!st) { closedir(dir); g_last_error = 8; return INVALID_HANDLE_VALUE64; }
    st->magic = BEER_FIND_MAGIC; st->dir = dir; st->wide = wide;
    snprintf(st->host_dir, sizeof(st->host_dir), "%s", host);
    size_t hl = strlen(st->host_dir);
    while (hl > 1 && st->host_dir[hl - 1] == '/') st->host_dir[--hl] = 0;
    snprintf(st->pattern, sizeof(st->pattern), "%s", slash ? slash + 1 : win_path);
    if (!beer_find_next_entry(st, out)) {
        closedir(dir); free(st);
        g_last_error = 2;                                  /* ERROR_FILE_NOT_FOUND */
        return INVALID_HANDLE_VALUE64;
    }
    g_last_error = 0;
    return (u64)(uintptr_t)st;
}

static u64 __attribute__((ms_abi)) beer_FindFirstFileW(const u16 *name, void *data)
{
    char narrow[PATH_MAX];
    if (!name || !data) { g_last_error = 87; return INVALID_HANDLE_VALUE64; }
    beer_utf16_to_utf8(name, narrow, sizeof(narrow));
    return beer_find_first(narrow, data, 1);
}
static u64 __attribute__((ms_abi)) beer_FindFirstFileA(const char *name, void *data)
{
    if (!name || !data) { g_last_error = 87; return INVALID_HANDLE_VALUE64; }
    return beer_find_first(name, data, 0);
}
static u64 __attribute__((ms_abi)) beer_FindFirstFileExW(const u16 *name, u32 level, void *data, u32 op, void *filter, u32 flags)
{
    (void)level; (void)op; (void)filter; (void)flags;
    return beer_FindFirstFileW(name, data);
}
static u64 __attribute__((ms_abi)) beer_FindFirstFileExA(const char *name, u32 level, void *data, u32 op, void *filter, u32 flags)
{
    (void)level; (void)op; (void)filter; (void)flags;
    return beer_FindFirstFileA(name, data);
}
static u64 __attribute__((ms_abi)) beer_FindNextFile(u64 handle, void *data)
{
    BeerFindState *st = (BeerFindState *)(uintptr_t)handle;
    if (!st || handle == INVALID_HANDLE_VALUE64 || st->magic != BEER_FIND_MAGIC || !data) { g_last_error = 6; return 0; }
    if (!beer_find_next_entry(st, data)) { g_last_error = 18; return 0; }   /* ERROR_NO_MORE_FILES */
    return 1;
}
static u64 __attribute__((ms_abi)) beer_FindClose(u64 handle)
{
    BeerFindState *st = (BeerFindState *)(uintptr_t)handle;
    if (!st || handle == INVALID_HANDLE_VALUE64 || st->magic != BEER_FIND_MAGIC) { g_last_error = 6; return 0; }
    st->magic = 0;
    closedir(st->dir);
    free(st);
    return 1;
}

/* GetTempFileName: "<path>\<up to 3 prefix chars><hex unique>.tmp"; uUnique == 0 picks a unique name and creates the file. */
static u64 beer_get_temp_file_name(const char *dir, const char *prefix, u32 unique, char *out_win)
{
    char path[PATH_MAX];
    size_t n = strlen(dir);
    if (n + 40 >= sizeof(path)) { g_last_error = 111; return 0; }
    memcpy(path, dir, n + 1);
    if (n && path[n - 1] != '\\' && path[n - 1] != '/') { path[n++] = '\\'; path[n] = 0; }
    char pre[4] = {0};
    for (int i = 0; i < 3 && prefix && prefix[i]; ++i) pre[i] = prefix[i];
    u32 value = unique;
    for (int attempt = 0; attempt < 65536; ++attempt) {
        if (!unique) value = (u32)((getpid() * 2654435761u) ^ (u32)(attempt * 40503u) ^ (u32)time(NULL)) & 0xffff;
        if (!value) value = 1;
        snprintf(out_win, PATH_MAX, "%s%s%x.tmp", path, pre, value);
        if (unique) return value;
        char host[PATH_MAX];
        if (!resolve_windows_host_path(out_win, host, sizeof(host))) { g_last_error = 3; return 0; }
        int fd = open(host, O_CREAT | O_EXCL | O_WRONLY, 0644);
        if (fd >= 0) { close(fd); return value; }
        if (errno != EEXIST) { g_last_error = errno == ENOENT ? 3 : 5; return 0; }
    }
    g_last_error = 80;
    return 0;
}
static u64 __attribute__((ms_abi)) beer_GetTempFileNameW(const u16 *dir, const u16 *prefix, u32 unique, u16 *out)
{
    char d[PATH_MAX], p[16], o[PATH_MAX];
    if (!dir || !out) { g_last_error = 87; return 0; }
    beer_utf16_to_utf8(dir, d, sizeof(d));
    beer_utf16_to_utf8(prefix ? prefix : (const u16 *)L"", p, sizeof(p));
    u64 r = beer_get_temp_file_name(d, p, unique, o);
    if (r) beer_utf8_to_utf16(o, out, 260);
    return r;
}
static u64 __attribute__((ms_abi)) beer_GetTempFileNameA(const char *dir, const char *prefix, u32 unique, char *out)
{
    char o[PATH_MAX];
    if (!dir || !out) { g_last_error = 87; return 0; }
    u64 r = beer_get_temp_file_name(dir, prefix ? prefix : "", unique, o);
    if (r) { strncpy(out, o, 259); out[259] = 0; }
    return r;
}

static u64 __attribute__((ms_abi)) beer_GetLogicalDriveStringsW(u32 size, u16 *buffer)
{
    static const char drives[] = "C:\\\0Z:\\\0";
    const u32 needed = 8 + 1;                   /* two "X:\" + NUL each + final NUL */
    if (!buffer || size < needed) return needed;
    for (u32 i = 0; i < 9; ++i) buffer[i] = (u8)(i < 8 ? drives[i] : 0);
    return 8;
}
static u64 __attribute__((ms_abi)) beer_GetLogicalDrives(void) { return (1u << 2) | (1u << 25); }   /* C: and Z: */

static u64 __attribute__((ms_abi)) beer_GetFileAttributesExW(const u16 *name, u32 level, void *data)
{
    if (!name || !data || level != 0) { g_last_error = 87; return 0; }
    char narrow[PATH_MAX], host[PATH_MAX];
    beer_utf16_to_utf8(name, narrow, sizeof(narrow));
    {   /* BEER_CALLER_MATCH=<substring>: show which guest code asks about matching paths. */
        const char *match = getenv("BEER_CALLER_MATCH");
        if (match && *match && strstr(narrow, match)) {
            fprintf(stderr, "[CALLER] GetFileAttributesExW(%s) t%ld\n", narrow, (long)syscall(SYS_gettid));
            dbg_dump_guest_callchain("GetFileAttributesExW");
        }
    }
    if (!resolve_windows_host_path(narrow, host, sizeof(host))) { g_last_error = 3; return 0; }
    struct stat sb;
    if (stat(host, &sb) != 0) { g_last_error = errno == ENOENT ? 2 : 3; return 0; }
    u8 *o = data;
    u32 attr = S_ISDIR(sb.st_mode) ? 0x10u : 0x80u;
    if (!(sb.st_mode & 0222)) attr |= 1u;
    memset(o, 0, 36);
    memcpy(o, &attr, 4);
    u64 c = beer_unix_to_filetime(sb.st_ctim.tv_sec, sb.st_ctim.tv_nsec);
    u64 a = beer_unix_to_filetime(sb.st_atim.tv_sec, sb.st_atim.tv_nsec);
    u64 w = beer_unix_to_filetime(sb.st_mtim.tv_sec, sb.st_mtim.tv_nsec);
    memcpy(o + 4, &c, 8); memcpy(o + 12, &a, 8); memcpy(o + 20, &w, 8);
    u32 hi = S_ISDIR(sb.st_mode) ? 0 : (u32)((u64)sb.st_size >> 32);
    u32 lo = S_ISDIR(sb.st_mode) ? 0 : (u32)((u64)sb.st_size);
    memcpy(o + 28, &hi, 4); memcpy(o + 32, &lo, 4);
    return 1;
}

static u64 __attribute__((ms_abi)) beer_GetVolumeInformationW(const u16 *root, u16 *volume_name, u32 volume_len,
        u32 *serial, u32 *max_component, u32 *flags, u16 *fs_name, u32 fs_len)
{
    (void)root;
    if (volume_name && volume_len) { static const char n[] = "Windows"; for (u32 i = 0; i < volume_len; ++i) { volume_name[i] = i < sizeof(n) ? (u8)n[i] : 0; if (i >= sizeof(n) - 1) break; } }
    if (serial) *serial = 0x1e57ca11u;
    if (max_component) *max_component = 255;
    if (flags) *flags = 0x00000007u | 0x00000008u | 0x00010000u;   /* case-sensitive search, case-preserved, unicode, persistent ACLs placeholder */
    if (fs_name && fs_len) { static const char n[] = "NTFS"; for (u32 i = 0; i < fs_len; ++i) { fs_name[i] = i < sizeof(n) ? (u8)n[i] : 0; if (i >= sizeof(n) - 1) break; } }
    return 1;
}

static u64 __attribute__((ms_abi)) beer_GetComputerNameW(u16 *buffer, u32 *size)
{
    static const char name[] = "BEER-PC";
    const u32 len = sizeof(name) - 1;
    if (!size) { g_last_error = 87; return 0; }
    if (!buffer || *size <= len) { *size = len + 1; g_last_error = 111; return 0; }
    for (u32 i = 0; i <= len; ++i) buffer[i] = (u8)name[i];
    *size = len;
    return 1;
}

/* ---- Native protection / DOS device / NT path helpers used by protection stubs and runtimes ---- */
static u64 __attribute__((ms_abi)) beer_NtProtectVirtualMemory(u64 process, void **base, u64 *size, u32 protect, u32 *old)
{
    (void)process;
    if (!base || !size || !old) return 0xC000000Du;                       /* STATUS_INVALID_PARAMETER */
    uintptr_t page = (uintptr_t)sysconf(_SC_PAGESIZE);
    uintptr_t start = (uintptr_t)*base & ~(page - 1);
    size_t length = (((uintptr_t)*base - start) + (size_t)*size + page - 1) & ~(page - 1);
    uint32_t previous = 0;
    if (!beer_virtual_protect((void *)start, length, protect, &previous))
        return 0xC0000005u;                                               /* STATUS_ACCESS_VIOLATION (not committed) */
    *base = (void *)start;
    *size = length;
    *old = previous;
    return 0;
}

static u64 __attribute__((ms_abi)) beer_QueryDosDeviceW(const u16 *name, u16 *target, u32 size)
{
    static const char c_dev[] = "\\Device\\HarddiskVolume1";
    static const char z_dev[] = "\\Device\\HarddiskVolume2";
    const char *dev = NULL;
    if (name && name[0] && name[1] == ':' && !name[2]) {
        u16 letter = name[0] | 0x20;
        if (letter == 'c') dev = c_dev;
        else if (letter == 'z') dev = z_dev;
    } else if (!name) {
        /* Enumerate: the DOS device names we expose. */
        static const char list[] = "C:\0Z:\0";
        u32 n = sizeof(list);                                              /* includes the NUL after Z: */
        if (!target || size < n + 1) { g_last_error = 122; return 0; }
        for (u32 i = 0; i < n; ++i) target[i] = (u8)list[i];
        target[n] = 0;
        return n + 1;
    }
    if (!dev) { g_last_error = 2; return 0; }                              /* ERROR_FILE_NOT_FOUND */
    u32 len = (u32)strlen(dev);
    if (!target || size < len + 2) { g_last_error = 122; return 0; }       /* ERROR_INSUFFICIENT_BUFFER */
    for (u32 i = 0; i <= len; ++i) target[i] = (u8)dev[i];
    target[len + 1] = 0;
    return len + 2;
}

static u64 __attribute__((ms_abi)) beer_RtlDosPathNameToNtPathName_U(const u16 *dos, u16 *nt, u16 **file_part, void *relative)
{
    (void)relative;
    if (!dos || !nt) return 0;
    size_t n = 0;
    while (dos[n]) ++n;
    int skip = 0;
    const char *prefix = "\\??\\";
    size_t plen = 4;
    if (n >= 4 && dos[0] == '\\' && dos[1] == '\\' && (dos[2] == '?' || dos[2] == '.') && dos[3] == '\\') { skip = 4; }
    size_t total = plen + (n - skip);
    u16 *buffer = (u16 *)(uintptr_t)impl_HeapAlloc(1, 0, (total + 1) * 2);
    if (!buffer) return 0;
    for (size_t i = 0; i < plen; ++i) buffer[i] = (u8)prefix[i];
    for (size_t i = 0; i < n - skip; ++i) buffer[plen + i] = dos[skip + i];
    buffer[total] = 0;
    nt[0] = (u16)(total * 2);
    nt[1] = (u16)(total * 2 + 2);
    *(u16 **)(void *)((u8 *)nt + 8) = buffer;
    if (file_part) {
        u16 *p = buffer + total;
        while (p > buffer && p[-1] != '\\') --p;
        *file_part = p;
    }
    return 1;
}

/* ---- Native NT file API (runtimes that bypass kernel32 resolve Zw and Nt File functions) ---- */
typedef struct { u64 status; u64 information; } BeerIoStatus;

static void nt_set_iosb(void *iosb, u64 status, u64 information)
{
    if (!iosb) return;
    ((BeerIoStatus *)iosb)->status = status;
    ((BeerIoStatus *)iosb)->information = information;
}

static u32 nt_status_from_last_error(void)
{
    switch (g_last_error) {
    case 2: case 3: return 0xC0000034u;     /* OBJECT_NAME_NOT_FOUND */
    case 5: return 0xC0000022u;             /* ACCESS_DENIED */
    case 80: case 183: return 0xC0000035u;  /* OBJECT_NAME_COLLISION */
    case 32: return 0xC0000043u;            /* SHARING_VIOLATION */
    default: return 0xC0000001u;            /* UNSUCCESSFUL */
    }
}

static u64 nt_open_common(u64 *out, u32 access, const u8 *attributes, void *iosb, u32 share, u32 disposition)
{
    if (!out || !attributes) return 0xC000000Du;
    const u8 *name = *(const u8 *const *)(attributes + 16);
    if (!name) return 0xC0000033u;           /* OBJECT_NAME_INVALID */
    u16 bytes = *(const u16 *)name;
    const u16 *chars = *(const u16 *const *)(name + 8);
    u32 count = bytes / 2, skip = 0;
    if (count >= 4 && chars[0] == '\\' && chars[1] == '?' && chars[2] == '?' && chars[3] == '\\') skip = 4;
    else if (count >= 4 && chars[0] == '\\' && chars[1] == '\\' && chars[2] == '?' && chars[3] == '\\') skip = 4;
    u16 path[1024];
    if (count - skip >= 1023) return 0xC0000106u;   /* NAME_TOO_LONG */
    for (u32 i = skip; i < count; ++i) path[i - skip] = chars[i];
    path[count - skip] = 0;
    static const u32 creation[6] = { 2, 3, 1, 4, 5, 2 };   /* FILE_SUPERSEDE..FILE_OVERWRITE_IF */
    if (disposition > 5) return 0xC000000Du;
    u32 win_access = access;
    /* Only genuine read rights imply a readable handle and only genuine write rights a writable one.
     * SYNCHRONIZE (0x100000) alone must not request write access: a protector's virtual file layer
     * opens every file with it, and a write-capable handle on a user's file is a data-loss hazard. */
    if (win_access & 0x000000a9u) win_access |= 0x80000000u;  /* READ_DATA, READ_EA, EXECUTE, READ_ATTRIBUTES */
    if (!(win_access & 0xC0000000u) && !(win_access & 0x00000116u)) win_access |= 0x80000000u;
    if (win_access & 0x00000116u) win_access |= 0x40000000u;  /* WRITE_DATA, APPEND_DATA, WRITE_EA, WRITE_ATTRIBUTES */
    u64 handle = impl_CreateFileW(path, win_access, share, 0, creation[disposition], 0x80, 0);
    if (handle == INVALID_HANDLE_VALUE64) {
        u32 status = nt_status_from_last_error();
        nt_set_iosb(iosb, status, 0);
        return status;
    }
    *out = handle;
    nt_set_iosb(iosb, 0, disposition == 1 ? 1 : 2);        /* FILE_OPENED / FILE_CREATED */
    return 0;
}

static u64 __attribute__((ms_abi)) beer_NtCreateFile(u64 *out, u32 access, const u8 *attributes, void *iosb,
                                                     void *alloc, u32 file_attributes, u32 share, u32 disposition)
{
    (void)alloc; (void)file_attributes;
    return nt_open_common(out, access, attributes, iosb, share, disposition);
}

static u64 __attribute__((ms_abi)) beer_NtOpenFile(u64 *out, u32 access, const u8 *attributes, void *iosb, u32 share)
{
    return nt_open_common(out, access, attributes, iosb, share, 1);
}

static u64 __attribute__((ms_abi)) beer_NtReadFile(u64 h, u64 event, void *apc, void *apc_ctx, void *iosb,
                                                   void *buffer, u32 length, const s64 *offset)
{
    (void)event; (void)apc; (void)apc_ctx;
    int fd = fh_get(h);
    if (fd < 0) return 0xC0000008u;                         /* INVALID_HANDLE */
    if (offset && *offset >= 0) {
        s32 high = (s32)(*offset >> 32);
        impl_SetFilePointer(h, (s32)(u32)*offset, &high, 0);
    }
    u32 done = 0;
    if (!impl_ReadFile(h, buffer, length, &done, NULL)) {
        u32 status = nt_status_from_last_error();
        nt_set_iosb(iosb, status, 0);
        return status;
    }
    if (!done && length) { nt_set_iosb(iosb, 0xC0000011u, 0); return 0xC0000011u; }   /* END_OF_FILE */
    nt_set_iosb(iosb, 0, done);
    return 0;
}

static u64 __attribute__((ms_abi)) beer_NtWriteFile(u64 h, u64 event, void *apc, void *apc_ctx, void *iosb,
                                                    const void *buffer, u32 length, const s64 *offset)
{
    (void)event; (void)apc; (void)apc_ctx;
    int fd = fh_get(h);
    if (fd < 0) return 0xC0000008u;
    if (offset && *offset >= 0) {
        s32 high = (s32)(*offset >> 32);
        impl_SetFilePointer(h, (s32)(u32)*offset, &high, 0);
    }
    u32 done = 0;
    if (!impl_WriteFile(h, buffer, length, &done, 0)) {
        u32 status = nt_status_from_last_error();
        nt_set_iosb(iosb, status, 0);
        return status;
    }
    nt_set_iosb(iosb, 0, done);
    return 0;
}

static u64 __attribute__((ms_abi)) beer_NtClose(u64 h)
{
    if (fh_get(h) < 0) return impl_CloseHandle(h) ? 0 : 0xC0000008u;
    return impl_CloseHandle(h) ? 0 : 0xC0000008u;
}

/* Loader lock: protection code takes it (often after locating it by walking ntdll's export table) before reading
 * PEB->Ldr module lists. One process-wide recursive lock; the cookie only has to round-trip. */
static pthread_mutex_t g_loader_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static u64 __attribute__((ms_abi)) beer_LdrLockLoaderLock(u32 flags, u32 *disposition, u64 *cookie)
{
    if (flags & 2) {                                    /* LDR_LOCK_LOADER_LOCK_FLAG_TRY_ONLY */
        int acquired = pthread_mutex_trylock(&g_loader_lock) == 0;
        if (disposition) *disposition = acquired ? 1 : 2;   /* LOCK_ACQUIRED / LOCK_NOT_ACQUIRED */
        if (cookie) *cookie = acquired ? 0x1D11ull : 0;
        return 0;
    }
    pthread_mutex_lock(&g_loader_lock);
    if (disposition) *disposition = 1;
    if (cookie) *cookie = 0x1D11ull;
    return 0;
}
static u64 __attribute__((ms_abi)) beer_LdrUnlockLoaderLock(u32 flags, u64 cookie)
{
    (void)flags;
    if (!cookie) return 0;
    return pthread_mutex_unlock(&g_loader_lock) == 0 ? 0 : 0xC000000Du;   /* STATUS_INVALID_PARAMETER */
}

static u64 __attribute__((ms_abi)) beer_NtQueryInformationFile(u64 h, void *iosb, u8 *info, u32 length, u32 klass)
{
    int fd = fh_get(h);
    if (fd < 0) return 0xC0000008u;
    struct stat st;
    if (fstat(fd, &st) != 0) return 0xC0000001u;
    if (klass == 5 && length >= 24) {                       /* FileStandardInformation */
        memset(info, 0, 24);
        *(s64 *)(info + 0) = (s64)((st.st_size + 4095) & ~(off_t)4095);
        *(s64 *)(info + 8) = (s64)st.st_size;
        *(u32 *)(info + 16) = 1;
        info[21] = S_ISDIR(st.st_mode) ? 1 : 0;
        nt_set_iosb(iosb, 0, 24);
        return 0;
    }
    if (klass == 14 && length >= 8) {                       /* FilePositionInformation */
        *(s64 *)info = (s64)lseek(fd, 0, SEEK_CUR);
        nt_set_iosb(iosb, 0, 8);
        return 0;
    }
    return 0xC0000003u;                                     /* INVALID_INFO_CLASS */
}

static u64 __attribute__((ms_abi)) beer_NtSetInformationFile(u64 h, void *iosb, const u8 *info, u32 length, u32 klass)
{
    int fd = fh_get(h);
    if (fd < 0) return 0xC0000008u;
    if (klass == 14 && length >= 8) {                       /* FilePositionInformation */
        if (lseek(fd, (off_t) * (const s64 *)info, SEEK_SET) < 0) return 0xC0000001u;
        nt_set_iosb(iosb, 0, 0);
        return 0;
    }
    if (klass == 20 && length >= 8) {                       /* FileEndOfFileInformation */
        if (ftruncate(fd, (off_t) * (const s64 *)info) != 0) return 0xC0000001u;
        nt_set_iosb(iosb, 0, 0);
        return 0;
    }
    if (klass == 19) { nt_set_iosb(iosb, 0, 0); return 0; } /* FileAllocationInformation */
    return 0xC0000003u;
}

static u64 beer_bind_failing_stub(const char *name)
{
    if (g_nstubs >= MAX_STUBS) return 0;
    g_snames[g_nstubs] = strdup(name);
    emit_impl_thunk(g_nstubs, (u64)win_export_stub_zero);
    u64 address = (u64)(g_tramp + g_nstubs * THUNK_SZ);
    g_nstubs++;
    return address;
}

/* ---- Native loader entry points (custom import resolvers call these instead of kernel32) ---- */
static u64 __attribute__((ms_abi)) beer_LdrGetProcedureAddress(u64 module, const u8 *name, u32 ordinal, u64 *out)
{
    if (!out) return 0xC000000Du;
    if (name) {
        u16 length = *(const u16 *)name;
        const char *text = *(const char *const *)(name + 8);
        char copy[256];
        if (!text || length >= sizeof(copy)) return 0xC000007Au;
        memcpy(copy, text, length);
        copy[length] = 0;
        u64 address = impl_GetProcAddress(module, copy);
        if (!address && module != HMOD_EXE && !guest_dll_from_handle(module)) {
            /* Import binding is not a feature probe: a missing import from an emulated system
             * module is fatal for the caller. Bind a logged failing stub so the whole missing
             * set is visible at once. */
            address = beer_bind_failing_stub(copy);
            if (address) fprintf(stderr, "[IMPORT] 0x%lx!%s not implemented -> failing stub\n",
                                 (unsigned long)module, copy);
        }
        if (!address) return 0xC000007Au;                    /* STATUS_PROCEDURE_NOT_FOUND */
        *out = address;
        return 0;
    }
    /* Import by ordinal: only modules whose public ordinals are well known. */
    const char *module_name = NULL;
    for (int i = 0; i < g_fake_dll_count; ++i)
        if (g_fake_dll_cache[i].handle == module) { module_name = g_fake_dll_cache[i].name; break; }
    if (module_name && !strncmp(module_name, "xinput", 6)) {
        static const char *const xinput_ordinals[] = {
            NULL, "XInputGetState", "XInputSetState", "XInputGetCapabilities", "XInputEnable",
            "XInputGetDSoundAudioDeviceGuids", "XInputGetBatteryInformation", "XInputGetKeystroke",
            "XInputGetAudioDeviceIds" };
        const char *resolved = NULL;
        if (ordinal < sizeof(xinput_ordinals) / sizeof(*xinput_ordinals)) resolved = xinput_ordinals[ordinal];
        else if (ordinal == 100) resolved = "XInputGetStateEx";
        if (resolved) {
            u64 address = impl_GetProcAddress(module, resolved);
            if (!address) address = beer_bind_failing_stub(resolved);
            if (address) { *out = address; return 0; }
        }
    }
    if (module_name && !strncmp(module_name, "oleaut32", 8)) {
        const char *resolved = NULL;
        switch (ordinal) {
            case 2: resolved = "SysAllocString"; break;
            case 3: resolved = "SysReAllocString"; break;
            case 4: resolved = "SysAllocStringLen"; break;
            case 5: resolved = "SysReAllocStringLen"; break;
            case 6: resolved = "SysFreeString"; break;
            case 7: resolved = "SysStringLen"; break;
            case 8: resolved = "VariantInit"; break;
            case 9: resolved = "VariantClear"; break;
            case 10: resolved = "VariantCopy"; break;
            case 149: resolved = "SysStringByteLen"; break;
            case 150: resolved = "SysAllocStringByteLen"; break;
        }
        if (resolved) {
            u64 address = impl_GetProcAddress(module, resolved);
            if (!address) address = beer_bind_failing_stub(resolved);
            if (address) { *out = address; return 0; }
        }
    }
    if (module_name && !strncmp(module_name, "ws2_32", 6)) {
        static const char *const winsock_ordinals[] = {
            NULL, "accept", "bind", "closesocket", "connect", "getpeername", "getsockname", "getsockopt",
            "htonl", "htons", "ioctlsocket", "inet_addr", "inet_ntoa", "listen", "ntohl", "ntohs",
            "recv", "recvfrom", "select", "send", "sendto", "setsockopt", "shutdown", "socket" };
        const char *resolved = NULL;
        if (ordinal < sizeof(winsock_ordinals) / sizeof(*winsock_ordinals)) resolved = winsock_ordinals[ordinal];
        else switch (ordinal) {
            case 51: resolved = "gethostbyaddr"; break;
            case 52: resolved = "gethostbyname"; break;
            case 57: resolved = "gethostname"; break;
            case 111: resolved = "WSAGetLastError"; break;
            case 112: resolved = "WSASetLastError"; break;
            case 115: resolved = "WSAStartup"; break;
            case 116: resolved = "WSACleanup"; break;
        }
        if (resolved) {
            u64 address = impl_GetProcAddress(module, resolved);
            if (!address) address = beer_bind_failing_stub(resolved);
            if (address) { *out = address; return 0; }
        }
    }
    /* Unknown ordinal: an import must not be fatal for the binder; bind a failing stub and log it. */
    char label[48];
    snprintf(label, sizeof(label), "%s!#%u", module_name ? module_name : "?", ordinal);
    u64 address = beer_bind_failing_stub(label);
    fprintf(stderr, "[IMPORT] %s ordinal %u not implemented -> failing stub\n", module_name ? module_name : "?", ordinal);
    if (address) { *out = address; return 0; }
    return 0xC0000138u;                                     /* STATUS_ORDINAL_NOT_FOUND */
}

static u64 nt_dll_name_to_handle(const u8 *unicode_name, u64 *out, int load)
{
    if (!unicode_name || !out) return 0xC000000Du;
    u16 bytes = *(const u16 *)unicode_name;
    const u16 *chars = *(const u16 *const *)(unicode_name + 8);
    char narrow[260];
    u32 count = bytes / 2;
    if (!chars || count >= sizeof(narrow)) return 0xC0000135u;
    for (u32 i = 0; i < count; ++i) narrow[i] = (char)chars[i];
    narrow[count] = 0;
    const char *base = narrow;
    for (const char *c = narrow; *c; ++c) if (*c == '\\' || *c == '/') base = c + 1;
    u64 handle = load ? impl_LoadLibraryA(base) : impl_GetModuleHandleA(base);
    if (!handle) return 0xC0000135u;                        /* STATUS_DLL_NOT_FOUND */
    *out = handle;
    return 0;
}

static u64 __attribute__((ms_abi)) beer_LdrLoadDll(const void *path, u32 *flags, const u8 *name, u64 *out)
{
    (void)path; (void)flags;
    return nt_dll_name_to_handle(name, out, 1);
}

static u64 __attribute__((ms_abi)) beer_LdrGetDllHandle(const void *path, u32 *flags, const u8 *name, u64 *out)
{
    (void)path; (void)flags;
    return nt_dll_name_to_handle(name, out, 0);
}

/* ---- Process handles: only the current process can be opened (it is the only one we know). ---- */
#define BEER_OWN_PROCESS_HANDLE 0x7FF0ull
static u64 __attribute__((ms_abi)) beer_OpenProcess(u32 access, u32 inherit, u32 pid)
{
    (void)access; (void)inherit;
    if (pid == (u32)impl_GetCurrentProcessId()) return BEER_OWN_PROCESS_HANDLE;
    if (pid && kill((pid_t)pid, 0) == 0) {                  /* another live process (guest children included) */
        u64 handle = beer_proc_alloc((pid_t)pid, 0);
        if (handle) return handle;
        return 0;
    }
    g_last_error = 87;                                      /* ERROR_INVALID_PARAMETER: no such process */
    return 0;
}
static u64 __attribute__((ms_abi)) beer_GetExitCodeProcess(u64 process, u32 *code)
{
    BeerProc *p = beer_proc_from_handle(process);
    if (p) {
        if (code) *code = beer_proc_exited(p) ? p->exit_code : 259u;   /* STILL_ACTIVE */
        return 1;
    }
    if (process != BEER_OWN_PROCESS_HANDLE && process != (u64)-1) { g_last_error = 6; return 0; }
    if (code) *code = 259;                                  /* STILL_ACTIVE */
    return 1;
}

/* ---- Message boxes: nothing to show headlessly; record the text and answer like a dismissed box. ---- */
static u64 beer_message_box_result(u32 type)
{
    switch (type & 0xF) {
    case 4: case 3: return 7;                               /* Yes/No(/Cancel): answer No */
    case 5: return 2;                                       /* Retry/Cancel */
    default: return 1;                                      /* OK / OK-Cancel */
    }
}
static u64 __attribute__((ms_abi)) beer_MessageBoxA(u64 owner, const char *text, const char *title, u32 type)
{
    (void)owner;
    fprintf(stderr, "[MSGBOX] \"%s\": %s\n", title ? title : "", text ? text : "");
    return beer_message_box_result(type);
}
static u64 __attribute__((ms_abi)) beer_MessageBoxW(u64 owner, const u16 *text, const u16 *title, u32 type)
{
    (void)owner;
    char t[512], c[1024];
    reg_narrow(title, t, sizeof(t)); reg_narrow(text, c, sizeof(c));
    fprintf(stderr, "[MSGBOX] \"%s\": %s\n", t, c);
    return beer_message_box_result(type);
}

/* ---- NtQueryAttributesFile / NtQueryFullAttributesFile (existence probes of NT-level VFS layers) ---- */
static int nt_object_path(const u8 *attributes, u16 *path, size_t capacity)
{
    if (!attributes) return 0;
    const u8 *name = *(const u8 *const *)(attributes + 16);
    if (!name) return 0;
    u16 bytes = *(const u16 *)name;
    const u16 *chars = *(const u16 *const *)(name + 8);
    u32 count = bytes / 2, skip = 0;
    if (!chars || count + 1 > capacity) return 0;
    if (count >= 4 && chars[0] == '\\' && (chars[1] == '?' || chars[1] == '\\') && chars[2] == '?' && chars[3] == '\\') skip = 4;
    for (u32 i = skip; i < count; ++i) path[i - skip] = chars[i];
    path[count - skip] = 0;
    return 1;
}

static u64 nt_query_attributes(const u8 *attributes, void *out, int full)
{
    u16 path[1024];
    if (!out || !nt_object_path(attributes, path, sizeof(path) / sizeof(*path))) return 0xC000000Du;
    u8 data[36];
    if (!beer_GetFileAttributesExW(path, 0, data)) return 0xC0000034u;      /* OBJECT_NAME_NOT_FOUND */
    u32 attr; memcpy(&attr, data, 4);
    u64 creation, access, write, size = 0;
    u32 high, low;
    memcpy(&creation, data + 4, 8); memcpy(&access, data + 12, 8); memcpy(&write, data + 20, 8);
    memcpy(&high, data + 28, 4); memcpy(&low, data + 32, 4);
    size = ((u64)high << 32) | low;
    u8 *o = out;
    memset(o, 0, full ? 56 : 40);
    memcpy(o + 0, &creation, 8); memcpy(o + 8, &access, 8); memcpy(o + 16, &write, 8); memcpy(o + 24, &write, 8);
    if (full) {
        u64 allocation = (size + 4095) & ~(u64)4095;
        memcpy(o + 32, &allocation, 8); memcpy(o + 40, &size, 8); memcpy(o + 48, &attr, 4);
    } else {
        memcpy(o + 32, &attr, 4);
    }
    return 0;
}
static u64 __attribute__((ms_abi)) beer_NtQueryAttributesFile(const u8 *attributes, void *basic_info)
{ return nt_query_attributes(attributes, basic_info, 0); }
static u64 __attribute__((ms_abi)) beer_NtQueryFullAttributesFile(const u8 *attributes, void *network_info)
{ return nt_query_attributes(attributes, network_info, 1); }
