/* Minimal in-memory registry: a read-only set of keys/values that make the machine look like a
 * normal Windows install (Steam client location and "running" state). Keys are matched
 * case-insensitively; Wow6432Node is folded away. Values the guest writes are not persisted. */
typedef struct { const char *path; const char *name; u32 type; char text[160]; u32 dword; } BeerRegValue;

#define BEER_REG_MAX_VALUES 40
#define BEER_REG_MAX_HANDLES 64
static BeerRegValue g_reg_values[BEER_REG_MAX_VALUES];
static int g_reg_value_count;
static char g_reg_open_paths[BEER_REG_MAX_HANDLES][192];
static pthread_mutex_t g_reg_mutex = PTHREAD_MUTEX_INITIALIZER;

static void reg_add_sz(const char *path, const char *name, const char *text)
{
    BeerRegValue *v = &g_reg_values[g_reg_value_count++];
    v->path = path; v->name = name; v->type = 1;
    snprintf(v->text, sizeof(v->text), "%s", text);
}
static void reg_add_dword(const char *path, const char *name, u32 value)
{
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
static void reg_seed_locked(void)
{
    if (g_reg_value_count) return;
    reg_seed_hardware();
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
