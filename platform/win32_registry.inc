/* Minimal in-memory registry: a read-only set of keys/values that make the machine look like a
 * normal Windows install (Steam client location and "running" state). Keys are matched
 * case-insensitively; Wow6432Node is folded away. Values the guest writes are not persisted. */
typedef struct { const char *path; const char *name; u32 type; char text[160]; u32 dword; } BeerRegValue;

#define BEER_REG_MAX_VALUES 96
#define BEER_REG_MAX_HANDLES 64
static BeerRegValue g_reg_values[BEER_REG_MAX_VALUES];
static int g_reg_value_count;
static char g_reg_open_paths[BEER_REG_MAX_HANDLES][192];
static pthread_mutex_t g_reg_mutex = PTHREAD_MUTEX_INITIALIZER;

static void reg_add_sz(const char *path, const char *name, const char *text)
{
    if (g_reg_value_count >= BEER_REG_MAX_VALUES) return;
    BeerRegValue *v = &g_reg_values[g_reg_value_count++];
    v->path = path; v->name = name; v->type = 1;
    snprintf(v->text, sizeof(v->text), "%s", text);
}
static void reg_add_dword(const char *path, const char *name, u32 value)
{
    if (g_reg_value_count >= BEER_REG_MAX_VALUES) return;
    BeerRegValue *v = &g_reg_values[g_reg_value_count++];
    v->path = path; v->name = name; v->type = 4; v->dword = value;
}
/* HKLM\\HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0: the real CPU from /proc/cpuinfo unless the
 * BEER_SPOOF_CPU_NAME / _VENDOR / _MHZ variables say otherwise (API-level only; CPUID stays native). */
static void reg_seed_hardware(void)
{
    static char name[160], vendor[64], ident[96];
    u32 mhz = 3000;
    snprintf(name, sizeof(name), "%s", "Beer Virtual CPU");
    snprintf(vendor, sizeof(vendor), "%s", "GenuineIntel");
    u32 family = 6, model = 0, stepping = 0;
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char *colon = strchr(line, ':');
            if (!colon) continue;
            char *value = colon + 1;
            while (*value == ' ' || *value == '\t') ++value;
            value[strcspn(value, "\r\n")] = 0;
            if (!strncmp(line, "model name", 10)) snprintf(name, sizeof(name), "%s", value);
            else if (!strncmp(line, "vendor_id", 9)) snprintf(vendor, sizeof(vendor), "%s", value);
            else if (!strncmp(line, "cpu family", 10)) family = (u32)atoi(value);
            else if (!strncmp(line, "model\t", 6) || !strncmp(line, "model ", 6)) { if (strncmp(line, "model name", 10)) model = (u32)atoi(value); }
            else if (!strncmp(line, "stepping", 8)) stepping = (u32)atoi(value);
            else if (!strncmp(line, "cpu MHz", 7)) mhz = (u32)atoi(value);
            if (!strncmp(line, "power management", 16)) break;   /* first CPU block only */
        }
        fclose(f);
    }
    const char *spoof_name = getenv("BEER_SPOOF_CPU_NAME");
    const char *spoof_vendor = getenv("BEER_SPOOF_CPU_VENDOR");
    const char *spoof_mhz = getenv("BEER_SPOOF_CPU_MHZ");
    if (spoof_name && *spoof_name) snprintf(name, sizeof(name), "%s", spoof_name);
    if (spoof_vendor && *spoof_vendor) snprintf(vendor, sizeof(vendor), "%s", spoof_vendor);
    if (spoof_mhz && atoi(spoof_mhz) > 0) mhz = (u32)atoi(spoof_mhz);
    snprintf(ident, sizeof(ident), "%s Family %u Model %u Stepping %u",
             !strcmp(vendor, "AuthenticAMD") ? "AMD64" : "Intel64", family, model, stepping);
    const char *key = "hklm\\hardware\\description\\system\\centralprocessor\\0";
    reg_add_sz(key, "processornamestring", name);
    reg_add_sz(key, "vendoridentifier", vendor);
    reg_add_sz(key, "identifier", ident);
    reg_add_dword(key, "~mhz", mhz);
}
/* Version of a DLL in Beer's shared runtime directories (BEER_DLL_PATH, default
 * ~/.local/share/beer/runtime): the System32 of the redistributables. 0 when absent. */
static int reg_shared_runtime_version(const char *dll, u32 version[4])
{
    const char *search = getenv("BEER_DLL_PATH");
    char fallback[PATH_MAX], list[PATH_MAX * 2], path[PATH_MAX + 64];
    if (!search || !*search) {
        const char *home = getenv("HOME");
        if (!home || snprintf(fallback, sizeof(fallback), "%s/.local/share/beer/runtime", home) >= (int)sizeof(fallback))
            return 0;
        search = fallback;
    }
    snprintf(list, sizeof(list), "%s", search);
    for (char *save = NULL, *dir = strtok_r(list, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        snprintf(path, sizeof(path), "%s/%s", dir, dll);
        uint8_t *data = NULL;
        uint32_t size = 0;
        if (access(path, R_OK) || !beer_pe_version_resource(path, &data, &size)) continue;
        static const u16 root[] = { '\\', 0 };
        const void *value = NULL;
        uint32_t length = 0;
        int ok = beer_version_query(data, size, root, &value, &length) && length >= 16;
        if (ok) {
            const u8 *fixed = value;                       /* VS_FIXEDFILEINFO */
            u32 signature, ms, ls;
            memcpy(&signature, fixed, 4);
            memcpy(&ms, fixed + 8, 4);
            memcpy(&ls, fixed + 12, 4);
            ok = signature == 0xFEEF04BDu;
            version[0] = ms >> 16; version[1] = ms & 0xffff; version[2] = ls >> 16; version[3] = ls & 0xffff;
        }
        free(data);
        if (ok) return 1;
    }
    return 0;
}

/* The VC++ 2015-2022 redistributable records itself under VC\Runtimes\x64 when it installs its
 * DLLs into System32. Prerequisite checks (Unreal's bootstrap launcher, installers) read this key.
 * It is published only when the shared runtime directories really hold those DLLs, with the
 * version read from vcruntime140.dll itself. */
static void reg_seed_vc_runtime(void)
{
    static const char *const required[] = { "vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll" };
    u32 version[4] = { 0 }, other[4];
    for (size_t i = 0; i < sizeof(required) / sizeof(*required); i++)
        if (!reg_shared_runtime_version(required[i], i ? other : version)) return;
    if (version[0] != 14) return;
    static const char key[] = "hklm\\software\\microsoft\\visualstudio\\14.0\\vc\\runtimes\\x64";
    static char text[48];
    snprintf(text, sizeof(text), "v%u.%u.%u.%02u", version[0], version[1], version[2], version[3]);
    reg_add_dword(key, "installed", 1);
    reg_add_sz(key, "version", text);
    reg_add_dword(key, "major", version[0]);
    reg_add_dword(key, "minor", version[1]);
    reg_add_dword(key, "bld", version[2]);
    reg_add_dword(key, "rbld", version[3]);
}

/* User-mode display driver version Beer reports, product.version.sub.build: IDXGIAdapter::CheckInterfaceSupport
 * and the adapter's driver key below carry the same value. */
#define BEER_UMD_VERSION_PRODUCT 32u
#define BEER_UMD_VERSION_MAJOR   0u
#define BEER_UMD_VERSION_MINOR   15u
#define BEER_UMD_VERSION_BUILD   7200u

/* The display adapter's driver key, seeded on first access below Control (it needs the host GPU): the
 * display-class instance and the Video key that EnumDisplayDevices' DeviceKey names hold the same driver
 * values, as on Windows: DriverDesc is the DXGI adapter description, ProviderName the vendor, MatchingDeviceId
 * the PCI id, DriverVersion the reported UMD version. */
static void reg_seed_display_driver_locked(void)
{
    static int seeded;
    if (seeded) return;
    seeded = 1;
    static char desc[160], matching[64], version[32];
    BeerHostGpuInfo gpu;
    const int have = beer_vulkan_host_gpu_info(&gpu);
    const u32 vendor = have ? gpu.vendor_id : 0x1414u;
    snprintf(desc, sizeof(desc), "%s", have ? gpu.name : "Beer Virtual D3D11 Adapter");
    snprintf(matching, sizeof(matching), "pci\\ven_%04x&dev_%04x", vendor, have ? gpu.device_id : 1u);
    snprintf(version, sizeof(version), "%u.%u.%u.%u", BEER_UMD_VERSION_PRODUCT, BEER_UMD_VERSION_MAJOR,
             BEER_UMD_VERSION_MINOR, BEER_UMD_VERSION_BUILD);
    const char *provider = vendor == 0x8086u ? "Intel Corporation" : vendor == 0x10deu ? "NVIDIA"
                         : vendor == 0x1002u ? "Advanced Micro Devices, Inc." : "Beer";
    static const char *const keys[] = {
        "hklm\\system\\currentcontrolset\\control\\class\\{4d36e968-e325-11ce-bfc1-08002be10318}\\0000",
        "hklm\\system\\currentcontrolset\\control\\video\\{beer0001-0000-0000-0000-000000000000}\\0000",
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(*keys); ++i) {
        reg_add_sz(keys[i], "driverdesc", desc);
        reg_add_sz(keys[i], "providername", provider);
        reg_add_sz(keys[i], "driverversion", version);
        reg_add_sz(keys[i], "matchingdeviceid", matching);
    }
}

static void reg_seed_locked(void)
{
    if (g_reg_value_count) return;
    reg_seed_hardware();
    reg_seed_vc_runtime();
    {   /* Launchers look for their sibling launcher through its install folder: point it at the game directory. */
        char folder[160];
        snprintf(folder, sizeof(folder), "%s", g_exe_path ? g_exe_path : "");
        char *slash = strrchr(folder, '\\');
        if (slash) *slash = 0;
        if (folder[0]) reg_add_sz("hklm\\software\\rockstar games\\launcher", "installfolder", folder);
    }
    const char *steam_off = getenv("BEER_FAKE_STEAM_REGISTRY");
    if (steam_off && !strcmp(steam_off, "0")) return;

    char steam_root[4096], steam_exe[4096], steam_client[4096], steam_client64[4096];
    if (!beer_steam_registry_root(g_steam_api_guest_mode, g_exe_path,
                                  steam_root, sizeof(steam_root)))
        snprintf(steam_root, sizeof(steam_root), "C:\\Program Files (x86)\\Steam");
    snprintf(steam_exe, sizeof(steam_exe), "%s\\steam.exe", steam_root);
    snprintf(steam_client, sizeof(steam_client), "%s\\steamclient.dll", steam_root);
    snprintf(steam_client64, sizeof(steam_client64), "%s\\steamclient64.dll", steam_root);

    reg_add_sz("hkcu\\software\\valve\\steam", "steampath", steam_root);
    reg_add_sz("hkcu\\software\\valve\\steam", "steamexe", steam_exe);
    reg_add_sz("hkcu\\software\\valve\\steam", "language", "english");
    reg_add_dword("hkcu\\software\\valve\\steam\\activeprocess", "pid", (u32)getpid());
    reg_add_sz("hkcu\\software\\valve\\steam\\activeprocess", "steamclientdll", steam_client);
    reg_add_sz("hkcu\\software\\valve\\steam\\activeprocess", "steamclientdll64", steam_client64);
    reg_add_sz("hkcu\\software\\valve\\steam\\activeprocess", "universe", "Public");
    reg_add_sz("hklm\\software\\valve\\steam", "installpath", steam_root);
}

/* Normalise "a\\b/C" -> "a\\b\\c" appended to dst, dropping wow6432node segments. */
static void reg_append_path(char *dst, size_t cap, const char *src)
{
    size_t len = strlen(dst);
    char segment[96]; size_t s = 0;
    for (const char *c = src;; ++c) {
        if (*c == '\\' || *c == '/' || !*c) {
            segment[s] = 0;
            if (s && strcmp(segment, "wow6432node") && len + s + 2 < cap) {
                if (len) dst[len++] = '\\';
                memcpy(dst + len, segment, s); len += s; dst[len] = 0;
            }
            s = 0;
            if (!*c) break;
        } else if (s + 1 < sizeof(segment)) {
            segment[s++] = (*c >= 'A' && *c <= 'Z') ? (char)(*c + 32) : *c;
        }
    }
}

static int reg_key_path(u64 key, char *out, size_t cap)
{
    u32 low = (u32)key;
    out[0] = 0;
    if (low == 0x80000000u) { snprintf(out, cap, "hkcr"); return 1; }
    if (low == 0x80000001u) { snprintf(out, cap, "hkcu"); return 1; }
    if (low == 0x80000002u) { snprintf(out, cap, "hklm"); return 1; }
    if (low >= 0x7100u && low < 0x7100u + BEER_REG_MAX_HANDLES && g_reg_open_paths[low - 0x7100u][0]) {
        snprintf(out, cap, "%s", g_reg_open_paths[low - 0x7100u]);
        return 1;
    }
    return 0;
}

static int reg_key_exists_locked(const char *path)
{
    if (!strcmp(path, "hkcu") || !strcmp(path, "hklm") || !strcmp(path, "hkcr")) return 1;
    size_t n = strlen(path);
    for (int i = 0; i < g_reg_value_count; ++i)
        if (!strncmp(g_reg_values[i].path, path, n) &&
            (g_reg_values[i].path[n] == 0 || g_reg_values[i].path[n] == '\\')) return 1;
    return 0;
}

static u64 reg_open(u64 key, const char *subkey, u64 *result)
{
    if (result) *result = 0;
    pthread_mutex_lock(&g_reg_mutex);
    reg_seed_locked();
    char path[192];
    u64 status = 6;                                       /* ERROR_INVALID_HANDLE */
    if (reg_key_path(key, path, sizeof(path))) {
        if (subkey) reg_append_path(path, sizeof(path), subkey);
        if (!strncmp(path, "hklm\\system\\currentcontrolset\\control", 37)) reg_seed_display_driver_locked();
        status = 2;                                       /* ERROR_FILE_NOT_FOUND */
        if (g_reg_value_count > 0 && reg_key_exists_locked(path) && result) {
            for (int i = 0; i < BEER_REG_MAX_HANDLES; ++i)
                if (!g_reg_open_paths[i][0]) {
                    snprintf(g_reg_open_paths[i], sizeof(g_reg_open_paths[i]), "%s", path);
                    *result = 0x7100u + (u64)i;
                    status = 0;
                    break;
                }
        } else if (g_reg_value_count > 0 && reg_key_exists_locked(path)) {
            status = 0;
        }
    }
    pthread_mutex_unlock(&g_reg_mutex);
    return status;
}

static u64 reg_query(u64 key, const char *name_ascii, u32 *type, void *data, u32 *size, int wide)
{
    pthread_mutex_lock(&g_reg_mutex);
    reg_seed_locked();
    char path[192], lname[96];
    u64 status = 2;
    size_t i = 0;
    for (; name_ascii && name_ascii[i] && i + 1 < sizeof(lname); ++i)
        lname[i] = (name_ascii[i] >= 'A' && name_ascii[i] <= 'Z') ? (char)(name_ascii[i] + 32) : name_ascii[i];
    lname[i] = 0;
    if (!reg_key_path(key, path, sizeof(path))) status = 6;
    else for (int k = 0; k < g_reg_value_count; ++k) {
        BeerRegValue *v = &g_reg_values[k];
        if (strcmp(v->path, path) || strcmp(v->name, lname)) continue;
        u32 need = v->type == 4 ? 4 : (u32)(strlen(v->text) + 1) * (wide ? 2u : 1u);
        if (type) *type = v->type;
        if (size) {
            u32 have = *size;
            *size = need;
            if (data) {
                if (have < need) { status = 234; break; }   /* ERROR_MORE_DATA */
                if (v->type == 4) memcpy(data, &v->dword, 4);
                else if (wide) for (u32 c = 0; c < need / 2; ++c) ((u16 *)data)[c] = (u8)v->text[c];
                else memcpy(data, v->text, need);
            }
        }
        status = 0;
        break;
    }
    pthread_mutex_unlock(&g_reg_mutex);
    return status;
}

static u64 reg_close(u64 key)
{
    u32 low = (u32)key;
    if (low >= 0x80000000u && low <= 0x80000005u) return 0;
    pthread_mutex_lock(&g_reg_mutex);
    u64 status = 6;
    if (low >= 0x7100u && low < 0x7100u + BEER_REG_MAX_HANDLES && g_reg_open_paths[low - 0x7100u][0]) {
        g_reg_open_paths[low - 0x7100u][0] = 0;
        status = 0;
    }
    pthread_mutex_unlock(&g_reg_mutex);
    return status;
}

static void reg_narrow(const u16 *w, char *out, size_t cap)
{
    size_t i = 0;
    for (; w && w[i] && i + 1 < cap; ++i) out[i] = (char)(w[i] > 0x7f ? '?' : w[i]);
    out[i] = 0;
}

/* RegGetValueA/W: open the optional subkey, query the value, enforce the RRF_RT_* type filter. */
static u64 reg_get_value(u64 key, const char *subkey, const char *value, u32 flags, u32 *type, void *data,
                         u32 *size, int wide)
{
    u64 opened = 0, use = key, status;
    if (subkey && *subkey) {
        status = reg_open(key, subkey, &opened);
        if (status) return status;
        use = opened;
    }
    u32 found = 0;
    status = reg_query(use, value ? value : "", &found, data, size, wide);
    if (opened) reg_close(opened);
    if (status == 0) {
        if (type) *type = found;
        if ((flags & 0xFFFFu) != 0 && !(found == 1 && (flags & 0x2u)) && !(found == 4 && (flags & 0x10u)) &&
            (flags & 0xFFFFu) != 0xFFFFu)
            return 1630;                                          /* ERROR_UNSUPPORTED_TYPE */
    }
    return status;
}

/* Immediate subkeys of path, derived from the stored value paths, in first-seen order. Caller holds
 * g_reg_mutex. Returns the count; names[i] point into the value paths (length in lengths[i]). */
static int reg_subkeys_locked(const char *path, const char **names, size_t *lengths, int capacity)
{
    size_t n = strlen(path);
    int count = 0;
    for (int i = 0; i < g_reg_value_count && count < capacity; ++i) {
        const char *v = g_reg_values[i].path;
        if (strncmp(v, path, n) || v[n] != '\\') continue;
        const char *child = v + n + 1;
        size_t len = strcspn(child, "\\");
        int seen = 0;
        for (int k = 0; k < count && !seen; ++k) seen = lengths[k] == len && !strncmp(names[k], child, len);
        if (!seen) { names[count] = child; lengths[count++] = len; }
    }
    return count;
}

/* RegQueryInfoKey: subkey and value counts and the largest name/data sizes (W: in characters). */
static u64 reg_query_info(u64 key, u32 *class_chars, u32 *subkeys, u32 *max_subkey, u32 *max_class,
                          u32 *values, u32 *max_value_name, u32 *max_value_data, u32 *security, u64 *last_write,
                          int wide)
{
    pthread_mutex_lock(&g_reg_mutex);
    reg_seed_locked();
    char path[192];
    if (!reg_key_path(key, path, sizeof(path)) || !reg_key_exists_locked(path)) {
        pthread_mutex_unlock(&g_reg_mutex);
        return 6;                                         /* ERROR_INVALID_HANDLE */
    }
    const char *names[64];
    size_t lengths[64];
    int count = reg_subkeys_locked(path, names, lengths, 64);
    u32 longest_key = 0, value_count = 0, longest_name = 0, longest_data = 0;
    for (int k = 0; k < count; ++k) if (lengths[k] > longest_key) longest_key = (u32)lengths[k];
    for (int i = 0; i < g_reg_value_count; ++i) {
        const BeerRegValue *v = &g_reg_values[i];
        if (strcmp(v->path, path)) continue;
        value_count++;
        if (strlen(v->name) > longest_name) longest_name = (u32)strlen(v->name);
        u32 data = v->type == 4 ? 4u : (u32)(strlen(v->text) + 1) * (wide ? 2u : 1u);
        if (data > longest_data) longest_data = data;
    }
    pthread_mutex_unlock(&g_reg_mutex);
    if (class_chars) *class_chars = 0;
    if (subkeys) *subkeys = (u32)count;
    if (max_subkey) *max_subkey = longest_key;
    if (max_class) *max_class = 0;
    if (values) *values = value_count;
    if (max_value_name) *max_value_name = longest_name;
    if (max_value_data) *max_value_data = longest_data;
    if (security) *security = 0;
    if (last_write) *last_write = 0;
    return 0;
}

static u64 __attribute__((ms_abi))
impl_RegQueryInfoKeyW(u64 key, u16 *class_name, u32 *class_chars, u32 *reserved, u32 *subkeys, u32 *max_subkey,
                      u32 *max_class, u32 *values, u32 *max_value_name, u32 *max_value_data, u32 *security,
                      u64 *last_write)
{
    (void)reserved;
    if (class_name && class_chars && *class_chars) class_name[0] = 0;
    return reg_query_info(key, class_chars, subkeys, max_subkey, max_class, values, max_value_name,
                          max_value_data, security, last_write, 1);
}
static u64 __attribute__((ms_abi))
impl_RegQueryInfoKeyA(u64 key, char *class_name, u32 *class_chars, u32 *reserved, u32 *subkeys, u32 *max_subkey,
                      u32 *max_class, u32 *values, u32 *max_value_name, u32 *max_value_data, u32 *security,
                      u64 *last_write)
{
    (void)reserved;
    if (class_name && class_chars && *class_chars) class_name[0] = 0;
    return reg_query_info(key, class_chars, subkeys, max_subkey, max_class, values, max_value_name,
                          max_value_data, security, last_write, 0);
}

/* RegEnumKeyEx: the index-th subkey name (names are stored lower-cased). */
static u64 reg_enum_key(u64 key, u32 index, void *name, u32 *name_chars, u64 *last_write, int wide)
{
    if (!name || !name_chars) return 87;                  /* ERROR_INVALID_PARAMETER */
    pthread_mutex_lock(&g_reg_mutex);
    reg_seed_locked();
    char path[192];
    if (!reg_key_path(key, path, sizeof(path)) || !reg_key_exists_locked(path)) {
        pthread_mutex_unlock(&g_reg_mutex);
        return 6;
    }
    const char *names[64];
    size_t lengths[64];
    int count = reg_subkeys_locked(path, names, lengths, 64);
    if (index >= (u32)count) { pthread_mutex_unlock(&g_reg_mutex); return 259; }   /* ERROR_NO_MORE_ITEMS */
    size_t len = lengths[index];
    if (len + 1 > *name_chars) { pthread_mutex_unlock(&g_reg_mutex); return 234; }  /* ERROR_MORE_DATA */
    for (size_t c = 0; c < len; ++c) {
        if (wide) ((u16 *)name)[c] = (u8)names[index][c];
        else ((char *)name)[c] = names[index][c];
    }
    if (wide) ((u16 *)name)[len] = 0; else ((char *)name)[len] = 0;
    pthread_mutex_unlock(&g_reg_mutex);
    *name_chars = (u32)len;
    if (last_write) *last_write = 0;
    return 0;
}
static u64 __attribute__((ms_abi))
impl_RegEnumKeyExW(u64 key, u32 index, u16 *name, u32 *name_chars, u32 *reserved, u16 *class_name,
                   u32 *class_chars, u64 *last_write)
{
    (void)reserved;
    if (class_name && class_chars && *class_chars) { class_name[0] = 0; *class_chars = 0; }
    return reg_enum_key(key, index, name, name_chars, last_write, 1);
}
static u64 __attribute__((ms_abi))
impl_RegEnumKeyExA(u64 key, u32 index, char *name, u32 *name_chars, u32 *reserved, char *class_name,
                   u32 *class_chars, u64 *last_write)
{
    (void)reserved;
    if (class_name && class_chars && *class_chars) { class_name[0] = 0; *class_chars = 0; }
    return reg_enum_key(key, index, name, name_chars, last_write, 0);
}
