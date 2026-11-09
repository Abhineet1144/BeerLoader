/* Security-descriptor / token / anonymous-pipe / timer-queue APIs that protection and launcher code (the Razor1911 module among
 * them) calls to decide whether a folder is usable, to run helper programs and to schedule callbacks.  Included from loader.c.
 *
 * Beer has a single user with full access everywhere, so the security calls answer "allowed"; pipes are host pipes wrapped in
 * ordinary file handles; timer-queue timers are host threads that call the guest callback with a proper TEB. */

/* ---- tokens / security descriptors ---- */
static u64 __attribute__((ms_abi)) impl_DuplicateToken(u64 token, u32 level, u64 *out)
{
    (void)level;
    if (!out) { g_last_error = 87; return 0; }
    *out = token + 1;
    return 1;
}
static u64 __attribute__((ms_abi)) impl_DuplicateTokenEx(u64 token, u32 access, void *attributes, u32 level, u32 type, u64 *out)
{
    (void)access; (void)attributes; (void)level; (void)type;
    return impl_DuplicateToken(token, 0, out);
}
/* ConvertStringSecurityDescriptorToSecurityDescriptor(sddl, revision, &descriptor, &size): the SDDL parsed into a
 * self-relative SECURITY_DESCRIPTOR (platform/win32_sddl.c) in LocalAlloc memory the caller frees with LocalFree.
 * Beer does not enforce object security, but callers build named pipes, events and files from the result and read
 * the descriptor back, so it must be real. Revision must be SDDL_REVISION_1. */
static u64 sddl_convert(const void *sddl, int narrow, u32 revision, u64 *descriptor, u32 *size)
{
    if (descriptor) *descriptor = 0;
    if (!sddl || !descriptor) { g_last_error = 87; return 0; }
    if (revision != 1) { g_last_error = 1305; return 0; }                    /* ERROR_UNKNOWN_REVISION */
    size_t needed = 0;
    u32 rc = beer_sddl_to_descriptor(sddl, narrow, NULL, 0, &needed);
    if (rc) { g_last_error = rc; return 0; }
    u8 *out = (u8 *)(uintptr_t)impl_LocalAlloc(0x40, needed);               /* LPTR */
    if (!out) { g_last_error = 8; return 0; }
    rc = beer_sddl_to_descriptor(sddl, narrow, out, needed, &needed);
    if (rc) { impl_LocalFree(out); g_last_error = rc; return 0; }
    *descriptor = (u64)(uintptr_t)out;
    if (size) *size = (u32)needed;
    g_last_error = 0;
    return 1;
}
static u64 __attribute__((ms_abi))
impl_ConvertStringSecurityDescriptorToSecurityDescriptorW(const u16 *sddl, u32 revision, u64 *descriptor, u32 *size)
    { return sddl_convert(sddl, 0, revision, descriptor, size); }
static u64 __attribute__((ms_abi))
impl_ConvertStringSecurityDescriptorToSecurityDescriptorA(const char *sddl, u32 revision, u64 *descriptor, u32 *size)
    { return sddl_convert(sddl, 1, revision, descriptor, size); }

/* ---- EXPLICIT_ACCESS / TRUSTEE -> security descriptor ----
 * EXPLICIT_ACCESS (x64, 48 bytes): grfAccessPermissions 0, grfAccessMode 4, grfInheritance 8, TRUSTEE at 16.
 * TRUSTEE (x64, 32 bytes): pMultipleTrustee 0, MultipleTrusteeOperation 8, TrusteeForm 12, TrusteeType 16,
 * ptstrName 24 (a name string, or a SID pointer for TRUSTEE_IS_SID). The A and W forms differ only in the string
 * type. The current user is Beer's single account, S-1-5-21-1000-1000-1000-1001 (its HKCU registry key). */
#define BEER_USER_SID_TEXT "S-1-5-21-1000-1000-1000-1001"
static void sid_to_text(const u8 *sid, char *out, size_t cap)
{
    u64 authority = 0;
    for (int i = 0; i < 6; ++i) authority = (authority << 8) | sid[2 + i];
    int n = snprintf(out, cap, "S-%u-%llu", sid[0], (unsigned long long)authority);
    for (u8 i = 0; i < sid[1] && i < 15 && n > 0 && (size_t)n < cap; ++i) {
        u32 sub;
        memcpy(&sub, sid + 8 + 4u * i, 4);
        n += snprintf(out + n, cap - (size_t)n, "-%u", sub);
    }
}
/* The SDDL form of a trustee, or 0 (error code in *error). */
static int trustee_sddl(const u8 *trustee, int wide, char *out, size_t cap, u32 *error)
{
    u32 form;
    const void *name;
    memcpy(&form, trustee + 12, 4);
    memcpy(&name, trustee + 24, sizeof(name));
    if (!name) { *error = 87; return 0; }
    if (form == 0) {                                                     /* TRUSTEE_IS_SID */
        sid_to_text(name, out, cap);
        return 1;
    }
    if (form != 1) { *error = 87; return 0; }                            /* only TRUSTEE_IS_NAME besides SIDs */
    char text[128];
    size_t n = 0;
    for (; n + 1 < sizeof(text); ++n) {
        unsigned c = wide ? ((const u16 *)name)[n] : ((const unsigned char *)name)[n];
        if (!c) break;
        text[n] = (char)(c >= 'a' && c <= 'z' ? c - 32 : c);
    }
    text[n] = 0;
    static const struct { const char *name; const char *sddl; } known[] = {
        { "EVERYONE", "WD" }, { "CURRENT_USER", BEER_USER_SID_TEXT }, { "CREATOR OWNER", "CO" },
        { "CREATOR GROUP", "CG" }, { "SYSTEM", "SY" }, { "NT AUTHORITY\\SYSTEM", "SY" },
        { "AUTHENTICATED USERS", "AU" }, { "NT AUTHORITY\\AUTHENTICATED USERS", "AU" },
        { "ADMINISTRATORS", "BA" }, { "BUILTIN\\ADMINISTRATORS", "BA" }, { "USERS", "BU" }, { "BUILTIN\\USERS", "BU" },
        { "GUESTS", "BG" }, { "BUILTIN\\GUESTS", "BG" }, { "INTERACTIVE", "IU" }, { "NT AUTHORITY\\INTERACTIVE", "IU" },
        { "PLAYER", BEER_USER_SID_TEXT },
    };
    for (size_t i = 0; i < sizeof(known) / sizeof(*known); ++i)
        if (!strcmp(text, known[i].name)) { snprintf(out, cap, "%s", known[i].sddl); return 1; }
    *error = 1332;                                                       /* ERROR_NONE_MAPPED */
    return 0;
}
static void explicit_access_build(u8 *entry, const void *name, u32 permissions, u32 mode, u32 inheritance)
{
    if (!entry) return;
    memset(entry, 0, 48);
    memcpy(entry + 0, &permissions, 4);
    memcpy(entry + 4, &mode, 4);
    memcpy(entry + 8, &inheritance, 4);
    const u32 form = 1;                                                  /* TRUSTEE_IS_NAME, TRUSTEE_IS_UNKNOWN */
    memcpy(entry + 16 + 12, &form, 4);
    memcpy(entry + 16 + 24, &name, sizeof(name));
}
static u64 __attribute__((ms_abi))
impl_BuildExplicitAccessWithNameW(u8 *entry, const u16 *name, u32 permissions, u32 mode, u32 inheritance)
    { explicit_access_build(entry, name, permissions, mode, inheritance); return 0; }
static u64 __attribute__((ms_abi))
impl_BuildExplicitAccessWithNameA(u8 *entry, const char *name, u32 permissions, u32 mode, u32 inheritance)
    { explicit_access_build(entry, name, permissions, mode, inheritance); return 0; }

/* Appends one ACE "(type;flags;0xmask;;;sid)" for an entry; returns 0 on success or an error code. */
static u32 explicit_access_ace(const u8 *entry, int wide, int audit, int deny_pass, char *out, size_t cap, size_t *used)
{
    u32 permissions, mode, inheritance;
    memcpy(&permissions, entry, 4);
    memcpy(&mode, entry + 4, 4);
    memcpy(&inheritance, entry + 8, 4);
    const char *type;
    char flags[24] = "";
    if (audit) {
        if (deny_pass) return 0;
        if (mode != 5 && mode != 6) return 0;                            /* SET_AUDIT_SUCCESS / SET_AUDIT_FAILURE */
        type = "AU";
        strcat(flags, mode == 5 ? "SA" : "FA");
    } else if (mode == 3) {                                              /* DENY_ACCESS */
        if (!deny_pass) return 0;
        type = "D";
    } else if (mode == 1 || mode == 2) {                                 /* GRANT_ACCESS, SET_ACCESS */
        if (deny_pass) return 0;
        type = "A";
    } else {
        return 0;                                                        /* REVOKE_ACCESS, NOT_USED_ACCESS */
    }
    if (inheritance & 1) strcat(flags, "OI");
    if (inheritance & 2) strcat(flags, "CI");
    if (inheritance & 4) strcat(flags, "NP");
    if (inheritance & 8) strcat(flags, "IO");
    char sid[96];
    u32 error = 0;
    if (!trustee_sddl(entry + 16, wide, sid, sizeof(sid), &error)) return error;
    const int n = snprintf(out + *used, cap - *used, "(%s;%s;0x%x;;;%s)", type, flags, permissions, sid);
    if (n < 0 || (size_t)n >= cap - *used) return 87;
    *used += (size_t)n;
    return 0;
}
/* A security descriptor's owner, group, SACL or DACL (index 0..3), self-relative (offsets) or absolute (pointers). */
static const u8 *sd_component(const u8 *sd, int index)
{
    u16 control;
    memcpy(&control, sd + 2, 2);
    if (control & 0x8000) {                                             /* SE_SELF_RELATIVE */
        u32 offset;
        memcpy(&offset, sd + 4 + 4 * index, 4);
        return offset ? sd + offset : NULL;
    }
    const u8 *pointer;
    memcpy(&pointer, sd + 8 + 8 * index, sizeof(pointer));
    return pointer;
}
static int sid_equal(const u8 *a, const u8 *b) { return a[1] == b[1] && !memcmp(a, b, 8u + 4u * a[1]); }
/* Appends the ACEs of acl as SDDL. kind: 0 deny ACEs, 1 the rest. ACEs whose SID equals one in `drop` are skipped. */
static u32 acl_to_sddl(const u8 *acl, int kind, const u8 *const *drop, u32 drop_count, char *out, size_t cap,
                       size_t *used)
{
    if (!acl) return 0;
    u16 count;
    memcpy(&count, acl + 4, 2);
    const u8 *ace = acl + 8;
    for (u16 i = 0; i < count; ++i) {
        u16 size;
        u32 mask;
        memcpy(&size, ace + 2, 2);
        memcpy(&mask, ace + 4, 4);
        const u8 type = ace[0], flags = ace[1];
        const u8 *sid = ace + 8;
        const char *name = type == 0 ? "A" : type == 1 ? "D" : type == 2 ? "AU" : type == 3 ? "AL" :
                           type == 0x11 ? "ML" : NULL;
        if (!name || size < 8) return 87;                               /* object/callback ACEs not handled */
        int skip = (type == 1) != (kind == 0);
        for (u32 d = 0; d < drop_count && !skip; ++d) skip = sid_equal(sid, drop[d]);
        if (!skip) {
            char flag_text[24] = "", sid_text[96];
            static const struct { u8 bit; const char *code; } bits[] = {
                { 0x01, "OI" }, { 0x02, "CI" }, { 0x04, "NP" }, { 0x08, "IO" }, { 0x10, "ID" }, { 0x40, "SA" }, { 0x80, "FA" } };
            for (size_t b = 0; b < sizeof(bits) / sizeof(*bits); ++b) if (flags & bits[b].bit) strcat(flag_text, bits[b].code);
            sid_to_text(sid, sid_text, sizeof(sid_text));
            const int n = snprintf(out + *used, cap - *used, "(%s;%s;0x%x;;;%s)", name, flag_text, mask, sid_text);
            if (n < 0 || (size_t)n >= cap - *used) return 87;
            *used += (size_t)n;
        }
        ace += size;
    }
    return 0;
}

/* BuildSecurityDescriptor(owner, group, access count, access entries, audit count, audit entries, old descriptor,
 * &size, &descriptor) -> Win32 error code. Deny entries are placed before grants (canonical order). */
static u32 build_security_descriptor(const u8 *owner, const u8 *group, u32 access_count, const u8 *access,
                                     u32 audit_count, const u8 *audit, const void *old_descriptor, u32 *size,
                                     u64 *descriptor, int wide)
{
    if (descriptor) *descriptor = 0;
    if (!size || !descriptor || (access_count && !access) || (audit_count && !audit)) return 87;
    /* Merging into old_descriptor: its owner/group stay unless replaced; its ACEs are kept except for trustees that
     * a new SET_ACCESS or REVOKE_ACCESS entry names. Canonical order: new denies, old denies, new grants, old grants. */
    const u8 *old_owner = old_descriptor ? sd_component(old_descriptor, 0) : NULL;
    const u8 *old_group = old_descriptor ? sd_component(old_descriptor, 1) : NULL;
    const u8 *old_sacl = old_descriptor ? sd_component(old_descriptor, 2) : NULL;
    const u8 *old_dacl = old_descriptor ? sd_component(old_descriptor, 3) : NULL;
    u16 old_control = 0;
    if (old_descriptor) memcpy(&old_control, (const u8 *)old_descriptor + 2, 2);
    char sddl[4096];
    size_t used = 0;
    u32 error = 0;
    char sid[96];
    if (owner) {
        if (!trustee_sddl(owner, wide, sid, sizeof(sid), &error)) return error;
        used += (size_t)snprintf(sddl + used, sizeof(sddl) - used, "O:%s", sid);
    } else if (old_owner) {
        sid_to_text(old_owner, sid, sizeof(sid));
        used += (size_t)snprintf(sddl + used, sizeof(sddl) - used, "O:%s", sid);
    }
    if (group) {
        if (!trustee_sddl(group, wide, sid, sizeof(sid), &error)) return error;
        used += (size_t)snprintf(sddl + used, sizeof(sddl) - used, "G:%s", sid);
    } else if (old_group) {
        sid_to_text(old_group, sid, sizeof(sid));
        used += (size_t)snprintf(sddl + used, sizeof(sddl) - used, "G:%s", sid);
    }
    /* Trustees whose old ACEs a SET_ACCESS/REVOKE_ACCESS replaces (only SID-form trustees can be compared). */
    const u8 *drop[64];
    u32 drop_count = 0;
    for (u32 i = 0; i < access_count && drop_count < 64; ++i) {
        u32 mode, form;
        const u8 *entry = access + 48u * i;
        memcpy(&mode, entry + 4, 4);
        memcpy(&form, entry + 16 + 12, 4);
        const u8 *trustee_sid;
        memcpy(&trustee_sid, entry + 16 + 24, sizeof(trustee_sid));
        if ((mode == 2 || mode == 4) && form == 0 && trustee_sid) drop[drop_count++] = trustee_sid;
    }
    const int old_dacl_present = old_descriptor && (old_control & 0x0004) && old_dacl;
    if (access_count || old_dacl_present) {
        used += (size_t)snprintf(sddl + used, sizeof(sddl) - used, "D:%s", (old_control & 0x1000) ? "P" : "");
        for (u32 i = 0; i < access_count; ++i)
            if ((error = explicit_access_ace(access + 48u * i, wide, 0, 1, sddl, sizeof(sddl), &used))) return error;
        if ((error = acl_to_sddl(old_dacl, 0, drop, drop_count, sddl, sizeof(sddl), &used))) return error;
        for (u32 i = 0; i < access_count; ++i)
            if ((error = explicit_access_ace(access + 48u * i, wide, 0, 0, sddl, sizeof(sddl), &used))) return error;
        if ((error = acl_to_sddl(old_dacl, 1, drop, drop_count, sddl, sizeof(sddl), &used))) return error;
    }
    if (audit_count || (old_descriptor && (old_control & 0x0010) && old_sacl)) {
        used += (size_t)snprintf(sddl + used, sizeof(sddl) - used, "S:");
        for (u32 i = 0; i < audit_count; ++i)
            if ((error = explicit_access_ace(audit + 48u * i, wide, 1, 0, sddl, sizeof(sddl), &used))) return error;
        if ((error = acl_to_sddl(old_sacl, 1, NULL, 0, sddl, sizeof(sddl), &used))) return error;
    }
    if (used >= sizeof(sddl)) return 87;
    size_t needed = 0;
    if ((error = beer_sddl_to_descriptor(sddl, 1, NULL, 0, &needed))) return error;
    u8 *out = (u8 *)(uintptr_t)impl_LocalAlloc(0x40, needed);
    if (!out) return 8;
    if ((error = beer_sddl_to_descriptor(sddl, 1, out, needed, &needed))) { impl_LocalFree(out); return error; }
    *descriptor = (u64)(uintptr_t)out;
    *size = (u32)needed;
    return 0;
}
static u64 __attribute__((ms_abi))
impl_BuildSecurityDescriptorW(const u8 *owner, const u8 *group, u32 access_count, const u8 *access, u32 audit_count,
                              const u8 *audit, const void *old_descriptor, u32 *size, u64 *descriptor)
    { return build_security_descriptor(owner, group, access_count, access, audit_count, audit, old_descriptor,
                                       size, descriptor, 1); }
static u64 __attribute__((ms_abi))
impl_BuildSecurityDescriptorA(const u8 *owner, const u8 *group, u32 access_count, const u8 *access, u32 audit_count,
                              const u8 *audit, const void *old_descriptor, u32 *size, u64 *descriptor)
    { return build_security_descriptor(owner, group, access_count, access, audit_count, audit, old_descriptor,
                                       size, descriptor, 0); }

/* A self-relative SECURITY_DESCRIPTOR with no owner/group/DACL (everybody allowed). */
static u64 __attribute__((ms_abi)) impl_GetFileSecurityW(const u16 *path, u32 info, u8 *descriptor, u32 length, u32 *needed)
{
    (void)path; (void)info;
    if (needed) *needed = 20;
    if (!descriptor || length < 20) { g_last_error = 122; return 0; }       /* ERROR_INSUFFICIENT_BUFFER */
    memset(descriptor, 0, 20);
    descriptor[0] = 1;                                                       /* Revision */
    *(u16 *)(descriptor + 2) = 0x8000;                                       /* SE_SELF_RELATIVE */
    g_last_error = 0;
    return 1;
}
static u64 __attribute__((ms_abi)) impl_GetFileSecurityA(const char *path, u32 info, u8 *descriptor, u32 length, u32 *needed)
    { return impl_GetFileSecurityW(NULL, info, descriptor, length, needed); (void)path; }
static u64 __attribute__((ms_abi)) impl_AccessCheck(const void *descriptor, u64 token, u32 desired, const void *mapping,
                                                    void *privileges, u32 *privileges_length, u32 *granted, u32 *status)
{
    (void)descriptor; (void)token; (void)mapping; (void)privileges; (void)privileges_length;
    if (granted) *granted = desired;
    if (status) *status = 1;
    g_last_error = 0;
    return 1;
}
/* PRIVILEGE_SET begins with PrivilegeCount, Control, followed by LUID_AND_ATTRIBUTES.
 * Beer's single-user token has the privileges it exposes enabled. */
static u64 __attribute__((ms_abi))
impl_PrivilegeCheck(u64 token, const u32 *required_privileges, u32 *result)
{
    if (!token || !required_privileges || !result) {
        g_last_error = 87;
        return 0;
    }
    *result = 1;
    g_last_error = 0;
    return 1;
}
/* GENERIC_MAPPING { GenericRead, GenericWrite, GenericExecute, GenericAll } */
static u64 __attribute__((ms_abi)) impl_MapGenericMask(u32 *access, const u32 *mapping)
{
    if (!access || !mapping) return 0;
    u32 value = *access;
    if (value & 0x80000000u) value |= mapping[0];
    if (value & 0x40000000u) value |= mapping[1];
    if (value & 0x20000000u) value |= mapping[2];
    if (value & 0x10000000u) value |= mapping[3];
    *access = value & ~0xF0000000u;
    return 0;
}

/* ---- anonymous pipes ---- */
static u64 __attribute__((ms_abi)) impl_CreatePipe(u64 *read_handle, u64 *write_handle, const void *attributes, u32 size)
{
    note_inheritable_sa((u64)(uintptr_t)attributes); (void)size;
    int fds[2];
    if (!read_handle || !write_handle) { g_last_error = 87; return 0; }
    if (pipe2(fds, O_CLOEXEC) != 0) { g_last_error = 4; return 0; }
    u64 r = fh_alloc(fds[0], "<anonymous pipe>", "");
    u64 w = fh_alloc(fds[1], "<anonymous pipe>", "");
    if (r == INVALID_HANDLE_VALUE64 || w == INVALID_HANDLE_VALUE64) { close(fds[0]); close(fds[1]); return 0; }
    *read_handle = r; *write_handle = w;
    inherit_from_sa(r, (u64)(uintptr_t)attributes);
    inherit_from_sa(w, (u64)(uintptr_t)attributes);
    g_last_error = 0;
    return 1;
}
static u64 __attribute__((ms_abi)) impl_SetHandleInformation(u64 handle, u32 mask, u32 flags)
{
    if (mask & 1u) handle_set_inheritable(handle, (flags & 1u) != 0);   /* HANDLE_FLAG_INHERIT */
    return 1;
}

/* ---- timer queue ---- */
typedef struct { u64 callback, parameter; u32 due, period, flags; _Atomic(int) cancelled; _Atomic(int) running; } BeerQueueTimer;
static void *beer_queue_timer_thread(void *arg)
{
    BeerQueueTimer *timer = arg;
    void *teb = alloc_teb_for_thread();
    syscall(SYS_arch_prctl, ARCH_SET_GS, (u64)teb);
    typedef void __attribute__((ms_abi)) (*TimerFn)(u64, u8);
    usleep(timer->due * 1000u);
    while (!atomic_load(&timer->cancelled)) {
        ((TimerFn)timer->callback)(timer->parameter, 1);
        if (!timer->period || (timer->flags & 0x8u /* WT_EXECUTEONLYONCE */)) break;
        usleep(timer->period * 1000u);
    }
    atomic_store(&timer->running, 0);
    return NULL;
}
static u64 __attribute__((ms_abi)) impl_CreateTimerQueue(void) { return 0x71710001ull; }
static u64 __attribute__((ms_abi)) impl_CreateTimerQueueTimer(u64 *out, u64 queue, u64 callback, u64 parameter, u32 due, u32 period, u32 flags)
{
    (void)queue;
    if (!out || !callback) { g_last_error = 87; return 0; }
    BeerQueueTimer *timer = calloc(1, sizeof(*timer));
    if (!timer) { g_last_error = 14; return 0; }
    timer->callback = callback; timer->parameter = parameter; timer->due = due; timer->period = period; timer->flags = flags;
    atomic_store(&timer->running, 1);
    pthread_t thread;
    if (pthread_create(&thread, NULL, beer_queue_timer_thread, timer) != 0) { free(timer); g_last_error = 8; return 0; }
    pthread_detach(thread);
    *out = (u64)(uintptr_t)timer;
    g_last_error = 0;
    return 1;
}
static u64 __attribute__((ms_abi)) impl_DeleteTimerQueueTimer(u64 queue, u64 timer_handle, u64 completion)
{
    (void)queue; (void)completion;
    BeerQueueTimer *timer = (BeerQueueTimer *)(uintptr_t)timer_handle;
    if (!timer) { g_last_error = 87; return 0; }
    atomic_store(&timer->cancelled, 1);        /* the (leaked) record stays valid for a thread still inside the callback */
    g_last_error = 0;
    return 1;
}
static u64 __attribute__((ms_abi)) impl_DeleteTimerQueueEx(u64 queue, u64 completion) { (void)queue; (void)completion; return 1; }

/* ---- Win32 file mappings on top of the NT section objects (platform/win32_nt_section.c) ----
 * Named page-file mappings are what two processes use to share memory (a launcher and its game, a crack module and its
 * host); they live in /dev/shm as "beer-map-<name>" so every Beer process that opens the same name sees the same bytes. */
static int beer_mapping_shm(const char *name, s64 size, int create, int *existed)
{
    char path[300] = "/beer-map-";
    size_t n = strlen(path);
    for (size_t i = 0; name[i] && n + 1 < sizeof(path); ++i) path[n++] = (name[i] == '/' || name[i] == '\\') ? '_' : name[i];
    path[n] = 0;
    int fd = shm_open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (existed) *existed = 0;
    if (fd < 0 && errno == EEXIST) { fd = shm_open(path, O_RDWR, 0600); if (existed) *existed = 1; }
    else if (fd < 0 && !create) return -1;
    if (fd < 0 && !create) return -1;
    if (fd >= 0 && create && size > 0) {
        struct stat st;
        if (fstat(fd, &st) == 0 && st.st_size < size && ftruncate(fd, size) != 0) { close(fd); return -1; }
    }
    return fd;
}
static u64 beer_create_file_mapping(u64 file, u32 protect, u32 size_high, u32 size_low, const char *name)
{
    s64 size = (s64)(((u64)size_high << 32) | size_low);
    int fd = -1, owned = 0, existed = 0;
    if (file && file != INVALID_HANDLE_VALUE64) {
        fd = fh_get(file);
        if (fd < 0) { g_last_error = 6; return 0; }
    } else if (name && *name) {
        if (size <= 0) { g_last_error = 87; return 0; }
        fd = beer_mapping_shm(name, size, 1, &existed);
        if (fd < 0) { g_last_error = 5; return 0; }
        owned = 1;
        if (existed) { struct stat st; if (fstat(fd, &st) == 0 && st.st_size > size) size = st.st_size; }
    } else if (size <= 0) { g_last_error = 87; return 0; }
    u64 handle = 0;
    u32 status = beer_nt_section_create(&handle, fd, size, protect & 0xFF, 0x8000000u /* SEC_COMMIT */);
    if (owned) close(fd);
    if (getenv("BEER_NT_SECTION_DIAGNOSTICS"))
        fprintf(stderr, "[MAPPING] CreateFileMapping name=%s file=0x%llx size=%lld protect=0x%x -> status=0x%x handle=0x%llx%s\n",
                name ? name : "(none)", (unsigned long long)file, (long long)size, protect, status, (unsigned long long)handle,
                existed ? " (existing)" : "");
    if (status) { g_last_error = status == BEER_STATUS_MAPPED_FILE_SIZE_ZERO ? 1006u : 8u; return 0; }
    g_last_error = existed ? 183u : 0u;                                    /* ERROR_ALREADY_EXISTS */
    return handle;
}
static void beer_mapping_name(const u16 *wide, char *out, size_t size)
{
    size_t i = 0;
    for (; wide && wide[i] && i + 1 < size; ++i) out[i] = (char)(wide[i] < 0x80 ? wide[i] : '_');
    out[i] = 0;
}
static u64 __attribute__((ms_abi)) impl_CreateFileMappingA(u64 file, void *attributes, u32 protect, u32 size_high, u32 size_low, const char *name)
    { note_inheritable_sa((u64)(uintptr_t)attributes); return beer_create_file_mapping(file, protect, size_high, size_low, name); }
static u64 __attribute__((ms_abi)) impl_CreateFileMappingW(u64 file, void *attributes, u32 protect, u32 size_high, u32 size_low, const u16 *name)
{
    note_inheritable_sa((u64)(uintptr_t)attributes);
    char narrow[260];
    beer_mapping_name(name, narrow, sizeof(narrow));
    return beer_create_file_mapping(file, protect, size_high, size_low, name ? narrow : NULL);
}
static u64 beer_open_file_mapping(const char *name)
{
    int fd = name ? beer_mapping_shm(name, 0, 0, NULL) : -1;
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size <= 0) {
        if (fd >= 0) close(fd);
        if (getenv("BEER_NT_SECTION_DIAGNOSTICS")) fprintf(stderr, "[MAPPING] OpenFileMapping name=%s -> not found\n", name ? name : "(none)");
        g_last_error = 2;
        return 0;
    }
    u64 handle = 0;
    u32 status = beer_nt_section_create(&handle, fd, st.st_size, 0x04, 0x8000000u);
    close(fd);
    if (getenv("BEER_NT_SECTION_DIAGNOSTICS")) fprintf(stderr, "[MAPPING] OpenFileMapping name=%s -> handle=0x%llx\n", name, (unsigned long long)handle);
    if (status) { g_last_error = 8; return 0; }
    g_last_error = 0;
    return handle;
}
static u64 __attribute__((ms_abi)) impl_OpenFileMappingA(u32 access, u32 inherit, const char *name)
    { (void)access; (void)inherit; return beer_open_file_mapping(name); }
static u64 __attribute__((ms_abi)) impl_OpenFileMappingW(u32 access, u32 inherit, const u16 *name)
{
    (void)access; (void)inherit;
    char narrow[260];
    beer_mapping_name(name, narrow, sizeof(narrow));
    return beer_open_file_mapping(name ? narrow : NULL);
}
static u64 __attribute__((ms_abi)) impl_MapViewOfFileEx(u64 mapping, u32 access, u32 offset_high, u32 offset_low, u64 bytes, void *base)
{
    u32 protection = (access & 0x20u) ? 0x40u : ((access & 0x2u) || (access & 0xF001Fu) == 0xF001Fu) ? 0x04u
                   : (access & 0x1u) ? 0x08u /* FILE_MAP_COPY */ : 0x02u;
    s64 offset = (s64)(((u64)offset_high << 32) | offset_low);
    size_t size = (size_t)bytes;
    void *address = base;
    u32 status = beer_nt_section_map(mapping, &address, 0, &offset, &size, 1, 0, protection);
    if (getenv("BEER_NT_SECTION_DIAGNOSTICS"))
        fprintf(stderr, "[MAPPING] MapViewOfFile handle=0x%llx access=0x%x offset=%lld size=%llu -> status=0x%x base=%p\n",
                (unsigned long long)mapping, access, (long long)offset, (unsigned long long)bytes, status, address);
    if (status) { g_last_error = status == BEER_STATUS_INVALID_HANDLE ? 6u : 8u; return 0; }
    g_last_error = 0;
    return (u64)(uintptr_t)address;
}
static u64 __attribute__((ms_abi)) impl_MapViewOfFile(u64 mapping, u32 access, u32 offset_high, u32 offset_low, u64 bytes)
    { return impl_MapViewOfFileEx(mapping, access, offset_high, offset_low, bytes, NULL); }
static u64 __attribute__((ms_abi)) impl_UnmapViewOfFile(void *base)
    { return beer_nt_section_unmap(base) == 0 ? 1 : (g_last_error = 487, 0); }

/* ---- Event Log (advapi32) ----
 * Beer has no Windows event log. Event sources and logs are handles that accept writes and read as empty: ReportEvent
 * succeeds and prints the message strings, as a log sink. Programs register a source at start-up and report errors
 * through it; Forza Horizon 6 resolves RegisterEventSourceW from advapi32's export table and stops with "entry point
 * not found" when it is absent. Handles use 0x38000000.. (kernel-object values stay below 0x80000000). */
#define BEER_EVENTLOG_HANDLE_BASE 0x38000000u
static _Atomic(u32) g_eventlog_next = 1;
static int eventlog_handle(u64 h) { return h > BEER_EVENTLOG_HANDLE_BASE && h < BEER_EVENTLOG_HANDLE_BASE + 0x01000000u; }
static u64 eventlog_new(void) { return BEER_EVENTLOG_HANDLE_BASE + (atomic_fetch_add(&g_eventlog_next, 1) & 0x00ffffffu); }
static u64 __attribute__((ms_abi)) impl_RegisterEventSourceW(const u16 *server, const u16 *source)
    { (void)server; (void)source; g_last_error = 0; return eventlog_new(); }
static u64 __attribute__((ms_abi)) impl_RegisterEventSourceA(const char *server, const char *source)
    { (void)server; (void)source; g_last_error = 0; return eventlog_new(); }
static u64 __attribute__((ms_abi)) impl_OpenEventLogW(const u16 *server, const u16 *source)
    { (void)server; (void)source; g_last_error = 0; return eventlog_new(); }
static u64 __attribute__((ms_abi)) impl_OpenEventLogA(const char *server, const char *source)
    { (void)server; (void)source; g_last_error = 0; return eventlog_new(); }
static u64 __attribute__((ms_abi)) impl_DeregisterEventSource(u64 h)
    { if (!eventlog_handle(h)) { g_last_error = 6; return 0; } return 1; }
static u64 __attribute__((ms_abi)) impl_CloseEventLog(u64 h)
    { if (!eventlog_handle(h)) { g_last_error = 6; return 0; } return 1; }
static u64 __attribute__((ms_abi)) impl_GetNumberOfEventLogRecords(u64 h, u32 *count)
    { if (!eventlog_handle(h) || !count) { g_last_error = 6; return 0; } *count = 0; return 1; }
/* ReportEvent(handle, type, category, event id, user SID, string count, data size, strings, raw data) */
static u64 eventlog_report(u64 h, u16 type, u32 event_id, u16 count, const void *const *strings, int wide)
{
    if (!eventlog_handle(h)) { g_last_error = 6; return 0; }
    fprintf(stderr, "[EVENTLOG] type=0x%x id=0x%x", type, event_id);
    for (u16 i = 0; strings && i < count && i < 8; ++i) {
        char text[256];
        size_t n = 0;
        if (wide) { const u16 *w = strings[i]; for (; w && w[n] && n + 1 < sizeof(text); ++n) text[n] = w[n] < 0x80 ? (char)w[n] : '?'; }
        else { const char *a = strings[i]; for (; a && a[n] && n + 1 < sizeof(text); ++n) text[n] = a[n]; }
        text[n] = 0;
        fprintf(stderr, " \"%s\"", text);
    }
    fprintf(stderr, "\n");
    g_last_error = 0;
    return 1;
}
static u64 __attribute__((ms_abi)) impl_ReportEventW(u64 h, u16 type, u16 category, u32 event_id, void *sid, u16 count,
                                                      u32 data_size, const u16 **strings, void *data)
    { (void)category; (void)sid; (void)data_size; (void)data; return eventlog_report(h, type, event_id, count, (const void *const *)strings, 1); }
static u64 __attribute__((ms_abi)) impl_ReportEventA(u64 h, u16 type, u16 category, u32 event_id, void *sid, u16 count,
                                                      u32 data_size, const char **strings, void *data)
    { (void)category; (void)sid; (void)data_size; (void)data; return eventlog_report(h, type, event_id, count, (const void *const *)strings, 0); }
