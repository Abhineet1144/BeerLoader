/* Architecture-neutral executable manifest and primary-token initialization.
 * Expat is loaded at runtime, like other optional host services. No XML entities
 * or external resources are enabled. Initial host launches retain Beer's existing
 * compatibility policy: a requested admin/highest token is granted in the guest
 * model, not via a UAC consent broker and never as Linux root. CreateProcess uses
 * inherited primary-token state instead. */
typedef struct {
    int depth, matched[5], level, seen, invalid;
} BeerManifestXml;

static const char *beer_xml_local(const char *name)
{
    const char *sep = strrchr(name, '|');
    return sep ? sep + 1 : name;
}

static void beer_manifest_start(void *context, const char *name, const char **attrs)
{
    BeerManifestXml *state = context;
    static const char *const path[] = {
        "assembly", "trustInfo", "security", "requestedPrivileges", "requestedExecutionLevel"
    };
    int depth = state->depth++;
    if (depth >= 5) return;
    state->matched[depth] = (!depth || state->matched[depth - 1]) &&
                            !strcmp(beer_xml_local(name), path[depth]);
    if (depth != 4 || !state->matched[depth]) return;
    if (state->seen++) { state->invalid = 1; return; }
    for (size_t i = 0; attrs[i]; i += 2) {
        if (strcmp(attrs[i], "level")) continue;
        if (!strcmp(attrs[i + 1], "asInvoker")) state->level = 1;
        else if (!strcmp(attrs[i + 1], "highestAvailable")) state->level = 2;
        else if (!strcmp(attrs[i + 1], "requireAdministrator")) state->level = 3;
        else state->invalid = 1;
    }
    if (!state->level) state->invalid = 1;
}

static void beer_manifest_end(void *context, const char *name)
{
    (void)name;
    BeerManifestXml *state = context;
    --state->depth;
}

static void beer_manifest_doctype(void *context, const char *name, const char *system,
                                   const char *public_id, int internal)
{
    (void)name; (void)system; (void)public_id; (void)internal;
    ((BeerManifestXml *)context)->invalid = 1;
}

static int beer_manifest_xml_level(const u8 *data, size_t size)
{
    if (!size || size > (1u << 20)) return -1;
    void *lib = dlopen("libexpat.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return -1; /* Do not guess security from an unparsed manifest. */
    /* Public Expat narrow-character ABI; opaque XML_Parser keeps the optional
     * runtime dependency independent of development headers. */
    void *(*create)(const char *, char) = dlsym(lib, "XML_ParserCreateNS");
    void (*user)(void *, void *) = dlsym(lib, "XML_SetUserData");
    void (*elements)(void *, void (*)(void *, const char *, const char **),
                     void (*)(void *, const char *)) = dlsym(lib, "XML_SetElementHandler");
    void (*doctype)(void *, void (*)(void *, const char *, const char *, const char *, int)) =
        dlsym(lib, "XML_SetStartDoctypeDeclHandler");
    int (*parse)(void *, const char *, int, int) = dlsym(lib, "XML_Parse");
    void (*release)(void *) = dlsym(lib, "XML_ParserFree");
    int level = -1;
    if (create && user && elements && doctype && parse && release) {
        void *parser = create(NULL, '|');
        if (parser) {
            BeerManifestXml state = {0};
            user(parser, &state);
            elements(parser, beer_manifest_start, beer_manifest_end);
            doctype(parser, beer_manifest_doctype);
            if (parse(parser, (const char *)data, (int)size, 1) == 1 && !state.invalid)
                level = state.level;
            release(parser);
        }
    }
    dlclose(lib);
    return level;
}

/* Resource directories have the same format in PE32 and PE32+. Only their
 * location in the optional header differs. Raw files map RVA through sections. */
static int beer_security_image_level(const u8 *image, size_t size, int mapped)
{
    if (size < 64 || *(const u16 *)image != MZ_MAGIC) return -1;
    u32 pe; memcpy(&pe, image + 0x3c, 4);
    if (pe > size || size - pe < 24 || *(const u32 *)(image + pe) != PE_SIG) return -1;
    u16 optional, sections;
    memcpy(&sections, image + pe + 6, 2);
    memcpy(&optional, image + pe + 20, 2);
    const size_t opt = (size_t)pe + 24;
    if (optional > size - opt || optional < 2) return -1;
    u16 magic; memcpy(&magic, image + opt, 2);
    size_t dirs = magic == 0x10b ? 96 : magic == PE32PLUS ? 112 : 0;
    if (!dirs || optional < dirs) return -1;
    u32 count; memcpy(&count, image + opt + dirs - 4, 4);
    if (count <= DIR_RESOURCE) return 0;
    if (optional < dirs + 24) return -1;
    DataDir resource;
    memcpy(&resource, image + opt + dirs + 16, sizeof(resource));
    if (!resource.rva || !resource.size) return 0;
    size_t at = resource.rva;
    if (!mapped) {
        size_t table = opt + optional;
        if (sections > (size - table) / sizeof(SecHdr)) return -1;
        int found = 0;
        for (u16 i = 0; i < sections; ++i) {
            SecHdr section; memcpy(&section, image + table + i * sizeof(section), sizeof(section));
            if (resource.rva < section.vrva || resource.rva - section.vrva > section.raw_sz) continue;
            u32 delta = resource.rva - section.vrva;
            /* The directory may extend past the section's file data (the loader zero-fills the rest: Stellar
             * Blade's shipping executable); only what the file holds can be parsed. */
            if (resource.size > section.raw_sz - delta) resource.size = section.raw_sz - delta;
            if (!resource.size) return 0;
            at = (size_t)section.raw_off + delta;
            found = 1;
            break;
        }
        if (!found) return -1;
    }
    if (at > size || resource.size > size - at) return -1;
    const u8 *manifest;
    size_t length;
    if (!pe_resource_data_by_id(image + at, resource.size, resource.rva,
                                24 /* RT_MANIFEST */, 1, &manifest, &length)) return 0;
    return beer_manifest_xml_level(manifest, length);
}

static int beer_security_file_level(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || st.st_size < 64 || (u64)st.st_size > SIZE_MAX) { close(fd); return -1; }
    u8 *file = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (file == MAP_FAILED) return -1;
    int level = beer_security_image_level(file, (size_t)st.st_size, 0);
    munmap(file, (size_t)st.st_size);
    return level;
}

static int beer_security_init(const u8 *image, size_t size)
{
    int level = beer_security_image_level(image, size, 1);
    if (level < 0) { g_last_error = 13; return 0; }
    const char *inherited = getenv("_BEER_PRIMARY_TOKEN");
    int was_inherited = inherited != NULL;
    int elevated;
    if (inherited) {
        if (strcmp(inherited, "0") && strcmp(inherited, "1")) {
            unsetenv("_BEER_PRIMARY_TOKEN"); g_last_error = 13; return 0;
        }
        elevated = inherited[0] == '1';
        unsetenv("_BEER_PRIMARY_TOKEN");
        if (!elevated && level == 3) { g_last_error = 740; return 0; }
    } else elevated = level == 2 || level == 3;
    g_process_elevated = elevated;
    fprintf(stderr, "[TOKEN] guest primary token: %s (%s, manifest level %d; no host privilege change)\n",
            elevated ? "administrator" : "standard", was_inherited ? "inherited" : "initial-launch policy", level);
    return 1;
}