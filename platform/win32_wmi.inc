/* Minimal WMI (wbemprox) for games that read hardware facts through COM: IWbemLocator -> IWbemServices ->
 * IEnumWbemClassObject -> IWbemClassObject.  Included from loader.c.
 *
 * Answered the way Wine's wbemprox does: Win32_Processor and Win32_VideoController carry real values, every other
 * class (notably the ROOT\Microsoft\Windows\Storage MSFT_* classes) is an empty result set, never a failure -- a
 * game that gets E_FAIL where Windows/Wine give "no rows" takes its error path (RDR2 reports "ACTIVATION REQUIRED").
 * Also here: the oleaut32 BSTR/VARIANT helpers WMI callers rely on (they used to be zero-returning stubs, so a
 * query string came back NULL). */

typedef struct { void **vtbl; _Atomic(u32) refs; int kind; int rows_left; char cls[48]; } BeerWmiObject;
enum { WMI_LOCATOR, WMI_SERVICES, WMI_ENUM, WMI_CLASS };

#define WMI_E_NOTIMPL   UINT64_C(0x80004001)
#define WMI_E_NOINTERFACE UINT64_C(0x80004002)
#define WMI_E_POINTER   UINT64_C(0x80004003)
#define WMI_S_FALSE     UINT64_C(1)
#define WMI_E_NOT_FOUND UINT64_C(0x80041002)

/* BSTRs come from the loader's oleaut32 implementation (defined later in loader.c). */
static u64 __attribute__((ms_abi)) impl_SysAllocStringLen(const u16 *src, u32 chars);
static u16 *beer_bstr_from_ascii(const char *s)
{
    u16 wide[256]; size_t n = strlen(s);
    if (n > 255) n = 255;
    for (size_t i = 0; i < n; ++i) wide[i] = (u8)s[i];
    return (u16 *)(uintptr_t)impl_SysAllocStringLen(wide, (u32)n);
}

static int beer_wmi_guid_is(const u8 *g, u32 d1, u16 d2, u16 d3, u8 a, u8 b, u8 c, u8 d, u8 e, u8 f, u8 g2, u8 h)
{
    const u8 want[16] = { (u8)d1, (u8)(d1 >> 8), (u8)(d1 >> 16), (u8)(d1 >> 24), (u8)d2, (u8)(d2 >> 8), (u8)d3, (u8)(d3 >> 8),
                          a, b, c, d, e, f, g2, h };
    return g && !memcmp(g, want, 16);
}
static int beer_wmi_is_iunknown(const u8 *g) { return beer_wmi_guid_is(g, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46) ; }

static BeerWmiObject *beer_wmi_new(int kind, void **vtbl, const char *cls, int rows)
{
    BeerWmiObject *o = calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->vtbl = vtbl; o->kind = kind; o->refs = 1; o->rows_left = rows;
    snprintf(o->cls, sizeof(o->cls), "%s", cls ? cls : "");
    return o;
}
static u64 __attribute__((ms_abi)) wmi_AddRef(BeerWmiObject *o) { return atomic_fetch_add(&o->refs, 1) + 1; }
static u64 __attribute__((ms_abi)) wmi_Release(BeerWmiObject *o)
{
    u32 left = atomic_fetch_sub(&o->refs, 1) - 1;
    if (!left) free(o);
    return left;
}
static u64 __attribute__((ms_abi)) wmi_NotImpl(void) { return WMI_E_NOTIMPL; }

static u64 __attribute__((ms_abi)) wmi_obj_QI(BeerWmiObject *o, const u8 *iid, void **out)
{
    if (!out) return WMI_E_POINTER;
    *out = NULL;
    const int match = beer_wmi_is_iunknown(iid) ||
        (o->kind == WMI_CLASS && beer_wmi_guid_is(iid, 0xdc12a681, 0x737f, 0x11cf, 0x88, 0x4d, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24)) ||
        (o->kind == WMI_ENUM && beer_wmi_guid_is(iid, 0x027947e1, 0xd731, 0x11ce, 0xa3, 0x57, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01));
    if (!match) return WMI_E_NOINTERFACE;
    wmi_AddRef(o); *out = o;
    return 0;
}

/* IClientSecurity (CoSetProxyBlanket asks for it): accept every blanket. */
static u64 __attribute__((ms_abi)) wmi_sec_QI(void *o, const u8 *iid, void **out) { (void)o; (void)iid; if (out) *out = NULL; return WMI_E_NOINTERFACE; }
static u64 __attribute__((ms_abi)) wmi_sec_Ref(void *o) { (void)o; return 1; }
static u64 __attribute__((ms_abi)) wmi_sec_Ok(void) { return 0; }
static void *g_wmi_security_vtbl[] = { (void *)wmi_sec_QI, (void *)wmi_sec_Ref, (void *)wmi_sec_Ref, (void *)wmi_sec_Ok, (void *)wmi_sec_Ok, (void *)wmi_sec_Ok, (void *)wmi_sec_Ok };
static void **g_wmi_security_object = (void **)&g_wmi_security_vtbl;

/* ---- IWbemClassObject ---- */
static const char *beer_wmi_cpu_name(void)
{
    static char name[128];
    if (name[0]) return name;
    FILE *f = fopen("/proc/cpuinfo", "r");
    char line[256];
    while (f && fgets(line, sizeof(line), f))
        if (!strncmp(line, "model name", 10) && strchr(line, ':')) {
            const char *v = strchr(line, ':') + 1;
            while (*v == ' ') ++v;
            snprintf(name, sizeof(name), "%s", v);
            name[strcspn(name, "\r\n")] = 0;
            break;
        }
    if (f) fclose(f);
    if (!name[0]) snprintf(name, sizeof(name), "Beer Virtual CPU");
    return name;
}
static int beer_wmi_cpu_mhz(void)
{
    FILE *f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", "r");
    long khz = 0;
    if (f) { if (fscanf(f, "%ld", &khz) != 1) khz = 0; fclose(f); }
    return khz > 0 ? (int)(khz / 1000) : 3000;
}
static u64 __attribute__((ms_abi)) wmi_class_Get(BeerWmiObject *o, const u16 *name, s32 flags, u8 *value, u32 *type, s32 *flavor)
{
    (void)flags;
    if (!name || !value) return WMI_E_POINTER;
    char key[64]; size_t n = 0;
    for (; name[n] && n < sizeof(key) - 1; ++n) key[n] = (char)name[n];
    key[n] = 0;
    memset(value, 0, 24);
    if (type) *type = 0;
    if (flavor) *flavor = 0;
    const int processor = !strcmp(o->cls, "Win32_Processor");
    if (processor && !strcmp(key, "NumberOfLogicalProcessors")) { *(u16 *)value = 3; *(s32 *)(value + 8) = (s32)sysconf(_SC_NPROCESSORS_ONLN); if (type) *type = 19; return 0; }
    if (processor && !strcmp(key, "NumberOfCores")) { *(u16 *)value = 3; *(s32 *)(value + 8) = (s32)sysconf(_SC_NPROCESSORS_ONLN); if (type) *type = 19; return 0; }
    if (processor && !strcmp(key, "MaxClockSpeed")) { *(u16 *)value = 3; *(s32 *)(value + 8) = beer_wmi_cpu_mhz(); if (type) *type = 19; return 0; }
    if ((processor && (!strcmp(key, "Name") || !strcmp(key, "Caption"))) || (!processor && !strcmp(key, "Name"))) {
        const char *text = processor ? beer_wmi_cpu_name() : "Beer Display Adapter";
        *(u16 *)value = 8; *(u16 **)(value + 8) = beer_bstr_from_ascii(text); if (type) *type = 8; return 0;
    }
    if (!processor && (!strcmp(key, "AdapterCompatibility"))) { *(u16 *)value = 8; *(u16 **)(value + 8) = beer_bstr_from_ascii("Beer"); if (type) *type = 8; return 0; }
    *(u16 *)value = 1;                                     /* VT_NULL: property exists but has no value */
    return 0;
}
static void *g_wmi_class_vtbl[] = {
    (void *)wmi_obj_QI, (void *)wmi_AddRef, (void *)wmi_Release, (void *)wmi_NotImpl, (void *)wmi_class_Get,
    /* 5..28: Put, Delete, GetNames, BeginEnumeration, Next, EndEnumeration, GetPropertyQualifierSet, Clone, GetObjectText, ... */
    (void *)wmi_NotImpl, (void *)wmi_NotImpl, (void *)wmi_NotImpl, (void *)wmi_NotImpl, (void *)wmi_NotImpl, (void *)wmi_NotImpl, (void *)wmi_NotImpl,
    (void *)wmi_NotImpl, (void *)wmi_NotImpl, (void *)wmi_NotImpl, (void *)wmi_NotImpl, (void *)wmi_NotImpl, (void *)wmi_NotImpl, (void *)wmi_NotImpl,
    (void *)wmi_NotImpl, (void *)wmi_NotImpl, (void *)wmi_NotImpl, (void *)wmi_NotImpl, (void *)wmi_NotImpl, (void *)wmi_NotImpl, (void *)wmi_NotImpl,
    (void *)wmi_NotImpl, (void *)wmi_NotImpl };

/* ---- IEnumWbemClassObject ---- */
static u64 __attribute__((ms_abi)) wmi_enum_Next(BeerWmiObject *o, s32 timeout, u32 count, void **objects, u32 *returned)
{
    (void)timeout;
    u32 produced = 0;
    while (objects && produced < count && o->rows_left > 0) {
        BeerWmiObject *row = beer_wmi_new(WMI_CLASS, g_wmi_class_vtbl, o->cls, 0);
        if (!row) break;
        objects[produced++] = row;
        o->rows_left--;
    }
    if (returned) *returned = produced;
    return produced == count ? 0 : WMI_S_FALSE;
}
static u64 __attribute__((ms_abi)) wmi_enum_Reset(BeerWmiObject *o) { (void)o; return 0; }
static void *g_wmi_enum_vtbl[] = { (void *)wmi_obj_QI, (void *)wmi_AddRef, (void *)wmi_Release, (void *)wmi_enum_Reset, (void *)wmi_enum_Next,
                                   (void *)wmi_NotImpl, (void *)wmi_NotImpl, (void *)wmi_NotImpl };

/* ---- IWbemServices ---- */
static void beer_wmi_class_from_query(const u16 *query, char *cls, size_t size)
{
    char text[256]; size_t n = 0;
    for (; query && query[n] && n < sizeof(text) - 1; ++n) text[n] = (char)query[n];
    text[n] = 0;
    cls[0] = 0;
    const char *from = strcasestr(text, " from ");
    if (!from) return;
    from += 6;
    while (*from == ' ') ++from;
    size_t k = 0;
    while (from[k] && from[k] != ' ' && k + 1 < size) { cls[k] = from[k]; ++k; }
    cls[k] = 0;
}
static int beer_wmi_class_has_rows(const char *cls) { return !strcasecmp(cls, "Win32_Processor") || !strcasecmp(cls, "Win32_VideoController"); }
static u64 beer_wmi_make_enum(const char *cls, void **out)
{
    if (!out) return WMI_E_POINTER;
    BeerWmiObject *e = beer_wmi_new(WMI_ENUM, g_wmi_enum_vtbl, cls, beer_wmi_class_has_rows(cls) ? 1 : 0);
    if (!e) return UINT64_C(0x8007000e);
    *out = e;
    return 0;
}
static u64 __attribute__((ms_abi)) wmi_services_QI(BeerWmiObject *o, const u8 *iid, void **out)
{
    if (!out) return WMI_E_POINTER;
    if (beer_wmi_guid_is(iid, 0x0000013d, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46)) { *out = &g_wmi_security_object; return 0; }   /* IClientSecurity */
    *out = NULL;
    if (beer_wmi_is_iunknown(iid) || beer_wmi_guid_is(iid, 0x9556dc99, 0x828c, 0x11cf, 0xa3, 0x7e, 0x00, 0xaa, 0x00, 0x32, 0x40, 0xc7)) {
        wmi_AddRef(o); *out = o; return 0;
    }
    return WMI_E_NOINTERFACE;
}
static u64 __attribute__((ms_abi)) wmi_services_CreateInstanceEnum(BeerWmiObject *o, const u16 *cls, s32 flags, void *ctx, void **out)
{
    (void)o; (void)flags; (void)ctx;
    char name[48]; size_t n = 0;
    for (; cls && cls[n] && n < sizeof(name) - 1; ++n) name[n] = (char)cls[n];
    name[n] = 0;
    return beer_wmi_make_enum(name, out);
}
static u64 __attribute__((ms_abi)) wmi_services_ExecQuery(BeerWmiObject *o, const u16 *lang, const u16 *query, s32 flags, void *ctx, void **out)
{
    (void)o; (void)lang; (void)flags; (void)ctx;
    char cls[48];
    beer_wmi_class_from_query(query, cls, sizeof(cls));
    return beer_wmi_make_enum(cls, out);
}
static void *g_wmi_services_vtbl[] = {
    (void *)wmi_services_QI, (void *)wmi_AddRef, (void *)wmi_Release,
    (void *)wmi_NotImpl, (void *)wmi_NotImpl, (void *)wmi_NotImpl, (void *)wmi_NotImpl, (void *)wmi_NotImpl, (void *)wmi_NotImpl,
    (void *)wmi_NotImpl, (void *)wmi_NotImpl, (void *)wmi_NotImpl, (void *)wmi_NotImpl, (void *)wmi_NotImpl, (void *)wmi_NotImpl,
    (void *)wmi_NotImpl, (void *)wmi_NotImpl, (void *)wmi_NotImpl, (void *)wmi_services_CreateInstanceEnum, (void *)wmi_NotImpl,
    (void *)wmi_services_ExecQuery, (void *)wmi_NotImpl, (void *)wmi_NotImpl, (void *)wmi_NotImpl, (void *)wmi_NotImpl,
    (void *)wmi_NotImpl, (void *)wmi_NotImpl };

/* ---- IWbemLocator ---- */
static u64 __attribute__((ms_abi)) wmi_locator_ConnectServer(BeerWmiObject *o, const u16 *network, const u16 *user, const u16 *password,
                                                            const u16 *locale, s32 flags, const u16 *authority, void *ctx, void **out)
{
    (void)o; (void)network; (void)user; (void)password; (void)locale; (void)flags; (void)authority; (void)ctx;
    if (!out) return WMI_E_POINTER;
    BeerWmiObject *services = beer_wmi_new(WMI_SERVICES, g_wmi_services_vtbl, "", 0);
    if (!services) return UINT64_C(0x8007000e);
    *out = services;
    return 0;
}
static u64 __attribute__((ms_abi)) wmi_locator_QI(BeerWmiObject *o, const u8 *iid, void **out)
{
    if (!out) return WMI_E_POINTER;
    *out = NULL;
    if (beer_wmi_is_iunknown(iid) || beer_wmi_guid_is(iid, 0xdc12a687, 0x737f, 0x11cf, 0x88, 0x4d, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24)) {
        wmi_AddRef(o); *out = o; return 0;
    }
    return WMI_E_NOINTERFACE;
}
static void *g_wmi_locator_vtbl[] = { (void *)wmi_locator_QI, (void *)wmi_AddRef, (void *)wmi_Release, (void *)wmi_locator_ConnectServer };

/* CoCreateInstance hook: CLSID_WbemLocator {4590f811-1d3a-11d0-891f-00aa004b2e24}. Returns 0 when handled. */
static int beer_wmi_create_instance(const u8 *clsid, const u8 *iid, void **out)
{
    if (!beer_wmi_guid_is(clsid, 0x4590f811, 0x1d3a, 0x11d0, 0x89, 0x1f, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24)) return 0;
    BeerWmiObject *locator = beer_wmi_new(WMI_LOCATOR, g_wmi_locator_vtbl, "", 0);
    if (!locator) return 0;
    if (wmi_locator_QI(locator, iid, out) != 0) { free(locator); return 0; }
    wmi_Release(locator);                                  /* QI took its own reference */
    return 1;
}

static u64 __attribute__((ms_abi)) impl_CoSetProxyBlanket(void *proxy, u32 a, u32 b, void *c, u32 d, u32 e, void *f, u32 g)
{
    (void)proxy; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g;
    return 0;
}

/* ---- Windows Firewall policy objects (HNetCfg.FwMgr -> LocalPolicy -> CurrentProfile -> AuthorizedApplications -> Item).
 * Games ask whether their executable is an authorised application; Beer reports an enabled exception for everything
 * (there is no Windows firewall here to block anything). All are IDispatch-derived: methods start at vtable slot 7. ---- */
typedef struct { void **vtbl; _Atomic(u32) refs; } BeerFwObject;
static u64 __attribute__((ms_abi)) fw_QI(BeerFwObject *o, const u8 *iid, void **out)
{
    (void)iid;
    if (!out) return WMI_E_POINTER;
    o->refs++; *out = o;                       /* every Beer firewall object answers every interface request with itself */
    return 0;
}
static u64 __attribute__((ms_abi)) fw_AddRef(BeerFwObject *o) { return ++o->refs; }
static u64 __attribute__((ms_abi)) fw_Release(BeerFwObject *o) { u32 left = --o->refs; if (!left) free(o); return left; }
static BeerFwObject *fw_new(void **vtbl) { BeerFwObject *o = calloc(1, sizeof(*o)); if (o) { o->vtbl = vtbl; o->refs = 1; } return o; }
static u64 __attribute__((ms_abi)) fw_get_true(BeerFwObject *o, int16_t *value) { (void)o; if (!value) return WMI_E_POINTER; *value = -1; return 0; }
static u64 __attribute__((ms_abi)) fw_get_zero(BeerFwObject *o, s32 *value) { (void)o; if (!value) return WMI_E_POINTER; *value = 0; return 0; }
static u64 __attribute__((ms_abi)) fw_ok(void) { return 0; }

static void *g_fw_app_vtbl[32];
static void *g_fw_apps_vtbl[16];
static void *g_fw_profile_vtbl[32];
static void *g_fw_policy_vtbl[16];
static void *g_fw_manager_vtbl[16];
static void fw_fill(void **vtbl, size_t count) { for (size_t i = 0; i < count; ++i) vtbl[i] = (void *)wmi_NotImpl; vtbl[0] = (void *)fw_QI; vtbl[1] = (void *)fw_AddRef; vtbl[2] = (void *)fw_Release; }

static u64 __attribute__((ms_abi)) fw_child(BeerFwObject *o, void **table, void **out) { (void)o; if (!out) return WMI_E_POINTER; *out = fw_new(table); return *out ? 0 : UINT64_C(0x8007000e); }
static u64 __attribute__((ms_abi)) fw_manager_get_LocalPolicy(BeerFwObject *o, void **out) { return fw_child(o, g_fw_policy_vtbl, out); }
static u64 __attribute__((ms_abi)) fw_policy_get_CurrentProfile(BeerFwObject *o, void **out) { return fw_child(o, g_fw_profile_vtbl, out); }
static u64 __attribute__((ms_abi)) fw_profile_get_AuthorizedApplications(BeerFwObject *o, void **out) { return fw_child(o, g_fw_apps_vtbl, out); }
static u64 __attribute__((ms_abi)) fw_apps_Item(BeerFwObject *o, const u16 *path, void **out) { (void)path; return fw_child(o, g_fw_app_vtbl, out); }

static int beer_fw_create_instance(const u8 *clsid, const u8 *iid, void **out)
{
    (void)iid;
    if (!out || !beer_wmi_guid_is(clsid, 0x304ce942, 0x6e39, 0x40d8, 0x94, 0x3a, 0xb9, 0x13, 0xc4, 0x0c, 0x9c, 0xd4)) return 0;
    static int ready;
    if (!ready) {
        fw_fill(g_fw_app_vtbl, 32); fw_fill(g_fw_apps_vtbl, 16); fw_fill(g_fw_profile_vtbl, 32); fw_fill(g_fw_policy_vtbl, 16); fw_fill(g_fw_manager_vtbl, 16);
        g_fw_manager_vtbl[7] = (void *)fw_manager_get_LocalPolicy;
        g_fw_policy_vtbl[7] = (void *)fw_policy_get_CurrentProfile;
        g_fw_profile_vtbl[8] = (void *)fw_get_true;                      /* get_FirewallEnabled */
        g_fw_profile_vtbl[20] = (void *)fw_profile_get_AuthorizedApplications;
        g_fw_apps_vtbl[7] = (void *)fw_get_zero;                          /* get_Count */
        g_fw_apps_vtbl[8] = (void *)fw_ok;                                /* Add */
        g_fw_apps_vtbl[10] = (void *)fw_apps_Item;
        g_fw_app_vtbl[17] = (void *)fw_get_true;                          /* get_Enabled */
        g_fw_app_vtbl[18] = (void *)fw_ok;                                /* put_Enabled */
        ready = 1;
    }
    BeerFwObject *manager = fw_new(g_fw_manager_vtbl);
    if (!manager) return 0;
    *out = manager;
    return 1;
}
