/* The local prefix has an empty service database. These handles grant no
 * access to host services, remote machines, or service installation. */
static pthread_mutex_t scm_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { u64 handle; u32 access; } scm_connections[128];
static u64 scm_next = 0x53434d0000000000ull;

static u64 scm_connect(int remote, int active_database, u32 access)
{
    if (remote) { g_last_error = 1722; return 0; } /* RPC_S_SERVER_UNAVAILABLE */
    if (!active_database) { g_last_error = 1065; return 0; } /* ERROR_DATABASE_DOES_NOT_EXIST */
    const u32 supported = 1u | 4u; /* CONNECT | ENUMERATE_SERVICE */
    if (access & ~supported) { g_last_error = 5; return 0; }
    pthread_mutex_lock(&scm_lock);
    u64 handle = 0;
    for (size_t i = 0; i < sizeof(scm_connections) / sizeof(*scm_connections); ++i) {
        if (scm_connections[i].handle) continue;
        scm_connections[i].handle = handle = ++scm_next;
        scm_connections[i].access = access | 1u;
        break;
    }
    pthread_mutex_unlock(&scm_lock);
    if (!handle) g_last_error = 8;
    return handle;
}

static int scm_local_a(const char *s)
{
    return !s || !*s || !strcmp(s, ".") || !strcmp(s, "\\\\.");
}

static int scm_wide_eq_ascii(const u16 *s, const char *text)
{
    while (*text) {
        const u16 ch = *s;
        if (ch > 0x7f || tolower((unsigned char)ch) != tolower((unsigned char)*text)) return 0;
        ++s; ++text;
    }
    return !*s && !*text;
}

static u64 __attribute__((ms_abi)) impl_OpenSCManagerA(const char *machine, const char *db, u32 access)
{
    return scm_connect(!scm_local_a(machine), !db || !strcasecmp(db, "ServicesActive"), access);
}

static u64 __attribute__((ms_abi)) impl_OpenSCManagerW(const u16 *machine, const u16 *db, u32 access)
{
    int local = !machine || !*machine || scm_wide_eq_ascii(machine, ".") || scm_wide_eq_ascii(machine, "\\\\.");
    return scm_connect(!local, !db || scm_wide_eq_ascii(db, "ServicesActive"), access);
}

static u32 scm_access(u64 handle, int close)
{
    if (!handle) return 0;
    pthread_mutex_lock(&scm_lock);
    u32 access = 0;
    for (size_t i = 0; i < sizeof(scm_connections) / sizeof(*scm_connections); ++i) {
        if (scm_connections[i].handle != handle) continue;
        access = scm_connections[i].access;
        if (close) scm_connections[i].handle = 0;
        break;
    }
    pthread_mutex_unlock(&scm_lock);
    return access;
}

static u64 __attribute__((ms_abi)) impl_CloseServiceHandle(u64 handle)
{
    if (scm_access(handle, 1)) return 1;
    g_last_error = 6;
    return 0;
}

static u64 scm_open_service(u64 handle, int has_name)
{
    g_last_error = !scm_access(handle, 0) ? 6 : !has_name ? 87 : 1060;
    return 0;
}

static u64 __attribute__((ms_abi)) impl_OpenServiceA(u64 handle, const char *name, u32 access)
{
    (void)access;
    return scm_open_service(handle, name && *name);
}

static u64 __attribute__((ms_abi)) impl_OpenServiceW(u64 handle, const u16 *name, u32 access)
{
    (void)access;
    return scm_open_service(handle, name && *name);
}

static u64 __attribute__((ms_abi)) impl_EnumServicesStatusW(u64 handle, u32 type, u32 state,
    void *buffer, u32 size, u32 *needed, u32 *returned, u32 *resume)
{
    (void)buffer; (void)size;
    const u32 access = scm_access(handle, 0);
    if (!access || !(access & 4)) { g_last_error = access ? 5 : 6; return 0; }
    /* Driver and Win32 service families, including the user-service flags. */
    if (!type || state < 1 || state > 3 || !needed || !returned) { g_last_error = 87; return 0; }
    *needed = *returned = 0;
    if (resume) *resume = 0;
    return 1;
}