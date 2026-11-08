/* BEER_EXTRA_EXPORTS="dll!Name,dll!Name,..." (diagnostic, off by default): publishes the listed functions in the
 * named system module's export table as stubs that return 0 and log their first call as "[EXTRA] dll!Name called".
 * Protection code that resolves a program's imports by walking export tables stops at the first name it cannot
 * find; this finds the whole set in one session, before real implementations are written. Never a default. */
#define BEER_EXTRA_EXPORT_MAX 1024
static struct { char dll[32]; char name[96]; } g_extra_exports[BEER_EXTRA_EXPORT_MAX];
static int g_extra_export_count = -1;

static void beer_extra_export_called(int index)
{
    static _Atomic(u8) reported[BEER_EXTRA_EXPORT_MAX];
    if (index < g_extra_export_count && !atomic_exchange(&reported[index], 1))
        fprintf(stderr, "[EXTRA] %s!%s called (stub returns 0)\n", g_extra_exports[index].dll, g_extra_exports[index].name);
}

#define BEER_EXTRA_STUB(n) \
    static u64 __attribute__((ms_abi)) beer_extra_stub_##n(void) { beer_extra_export_called(n); return 0; }
#define BEER_EXTRA_STUB16(n) \
    BEER_EXTRA_STUB(n##0) BEER_EXTRA_STUB(n##1) BEER_EXTRA_STUB(n##2) BEER_EXTRA_STUB(n##3) \
    BEER_EXTRA_STUB(n##4) BEER_EXTRA_STUB(n##5) BEER_EXTRA_STUB(n##6) BEER_EXTRA_STUB(n##7) \
    BEER_EXTRA_STUB(n##8) BEER_EXTRA_STUB(n##9) BEER_EXTRA_STUB(n##a) BEER_EXTRA_STUB(n##b) \
    BEER_EXTRA_STUB(n##c) BEER_EXTRA_STUB(n##d) BEER_EXTRA_STUB(n##e) BEER_EXTRA_STUB(n##f)
#define BEER_EXTRA_STUB_FN16(n) \
    (ImplFn)beer_extra_stub_##n##0, (ImplFn)beer_extra_stub_##n##1, (ImplFn)beer_extra_stub_##n##2, \
    (ImplFn)beer_extra_stub_##n##3, (ImplFn)beer_extra_stub_##n##4, (ImplFn)beer_extra_stub_##n##5, \
    (ImplFn)beer_extra_stub_##n##6, (ImplFn)beer_extra_stub_##n##7, (ImplFn)beer_extra_stub_##n##8, \
    (ImplFn)beer_extra_stub_##n##9, (ImplFn)beer_extra_stub_##n##a, (ImplFn)beer_extra_stub_##n##b, \
    (ImplFn)beer_extra_stub_##n##c, (ImplFn)beer_extra_stub_##n##d, (ImplFn)beer_extra_stub_##n##e, \
    (ImplFn)beer_extra_stub_##n##f
BEER_EXTRA_STUB16(0x00) BEER_EXTRA_STUB16(0x01) BEER_EXTRA_STUB16(0x02) BEER_EXTRA_STUB16(0x03) BEER_EXTRA_STUB16(0x04) BEER_EXTRA_STUB16(0x05) BEER_EXTRA_STUB16(0x06) BEER_EXTRA_STUB16(0x07) BEER_EXTRA_STUB16(0x08) BEER_EXTRA_STUB16(0x09) BEER_EXTRA_STUB16(0x0a) BEER_EXTRA_STUB16(0x0b) BEER_EXTRA_STUB16(0x0c) BEER_EXTRA_STUB16(0x0d) BEER_EXTRA_STUB16(0x0e) BEER_EXTRA_STUB16(0x0f) BEER_EXTRA_STUB16(0x10) BEER_EXTRA_STUB16(0x11) BEER_EXTRA_STUB16(0x12) BEER_EXTRA_STUB16(0x13) BEER_EXTRA_STUB16(0x14) BEER_EXTRA_STUB16(0x15) BEER_EXTRA_STUB16(0x16) BEER_EXTRA_STUB16(0x17) BEER_EXTRA_STUB16(0x18) BEER_EXTRA_STUB16(0x19) BEER_EXTRA_STUB16(0x1a) BEER_EXTRA_STUB16(0x1b) BEER_EXTRA_STUB16(0x1c) BEER_EXTRA_STUB16(0x1d) BEER_EXTRA_STUB16(0x1e) BEER_EXTRA_STUB16(0x1f) BEER_EXTRA_STUB16(0x20) BEER_EXTRA_STUB16(0x21) BEER_EXTRA_STUB16(0x22) BEER_EXTRA_STUB16(0x23) BEER_EXTRA_STUB16(0x24) BEER_EXTRA_STUB16(0x25) BEER_EXTRA_STUB16(0x26) BEER_EXTRA_STUB16(0x27) BEER_EXTRA_STUB16(0x28) BEER_EXTRA_STUB16(0x29) BEER_EXTRA_STUB16(0x2a) BEER_EXTRA_STUB16(0x2b) BEER_EXTRA_STUB16(0x2c) BEER_EXTRA_STUB16(0x2d) BEER_EXTRA_STUB16(0x2e) BEER_EXTRA_STUB16(0x2f) BEER_EXTRA_STUB16(0x30) BEER_EXTRA_STUB16(0x31) BEER_EXTRA_STUB16(0x32) BEER_EXTRA_STUB16(0x33) BEER_EXTRA_STUB16(0x34) BEER_EXTRA_STUB16(0x35) BEER_EXTRA_STUB16(0x36) BEER_EXTRA_STUB16(0x37) BEER_EXTRA_STUB16(0x38) BEER_EXTRA_STUB16(0x39) BEER_EXTRA_STUB16(0x3a) BEER_EXTRA_STUB16(0x3b) BEER_EXTRA_STUB16(0x3c) BEER_EXTRA_STUB16(0x3d) BEER_EXTRA_STUB16(0x3e) BEER_EXTRA_STUB16(0x3f)
static const ImplFn g_extra_export_stubs[BEER_EXTRA_EXPORT_MAX] = {
    BEER_EXTRA_STUB_FN16(0x00),
    BEER_EXTRA_STUB_FN16(0x01),
    BEER_EXTRA_STUB_FN16(0x02),
    BEER_EXTRA_STUB_FN16(0x03),
    BEER_EXTRA_STUB_FN16(0x04),
    BEER_EXTRA_STUB_FN16(0x05),
    BEER_EXTRA_STUB_FN16(0x06),
    BEER_EXTRA_STUB_FN16(0x07),
    BEER_EXTRA_STUB_FN16(0x08),
    BEER_EXTRA_STUB_FN16(0x09),
    BEER_EXTRA_STUB_FN16(0x0a),
    BEER_EXTRA_STUB_FN16(0x0b),
    BEER_EXTRA_STUB_FN16(0x0c),
    BEER_EXTRA_STUB_FN16(0x0d),
    BEER_EXTRA_STUB_FN16(0x0e),
    BEER_EXTRA_STUB_FN16(0x0f),
    BEER_EXTRA_STUB_FN16(0x10),
    BEER_EXTRA_STUB_FN16(0x11),
    BEER_EXTRA_STUB_FN16(0x12),
    BEER_EXTRA_STUB_FN16(0x13),
    BEER_EXTRA_STUB_FN16(0x14),
    BEER_EXTRA_STUB_FN16(0x15),
    BEER_EXTRA_STUB_FN16(0x16),
    BEER_EXTRA_STUB_FN16(0x17),
    BEER_EXTRA_STUB_FN16(0x18),
    BEER_EXTRA_STUB_FN16(0x19),
    BEER_EXTRA_STUB_FN16(0x1a),
    BEER_EXTRA_STUB_FN16(0x1b),
    BEER_EXTRA_STUB_FN16(0x1c),
    BEER_EXTRA_STUB_FN16(0x1d),
    BEER_EXTRA_STUB_FN16(0x1e),
    BEER_EXTRA_STUB_FN16(0x1f),
    BEER_EXTRA_STUB_FN16(0x20),
    BEER_EXTRA_STUB_FN16(0x21),
    BEER_EXTRA_STUB_FN16(0x22),
    BEER_EXTRA_STUB_FN16(0x23),
    BEER_EXTRA_STUB_FN16(0x24),
    BEER_EXTRA_STUB_FN16(0x25),
    BEER_EXTRA_STUB_FN16(0x26),
    BEER_EXTRA_STUB_FN16(0x27),
    BEER_EXTRA_STUB_FN16(0x28),
    BEER_EXTRA_STUB_FN16(0x29),
    BEER_EXTRA_STUB_FN16(0x2a),
    BEER_EXTRA_STUB_FN16(0x2b),
    BEER_EXTRA_STUB_FN16(0x2c),
    BEER_EXTRA_STUB_FN16(0x2d),
    BEER_EXTRA_STUB_FN16(0x2e),
    BEER_EXTRA_STUB_FN16(0x2f),
    BEER_EXTRA_STUB_FN16(0x30),
    BEER_EXTRA_STUB_FN16(0x31),
    BEER_EXTRA_STUB_FN16(0x32),
    BEER_EXTRA_STUB_FN16(0x33),
    BEER_EXTRA_STUB_FN16(0x34),
    BEER_EXTRA_STUB_FN16(0x35),
    BEER_EXTRA_STUB_FN16(0x36),
    BEER_EXTRA_STUB_FN16(0x37),
    BEER_EXTRA_STUB_FN16(0x38),
    BEER_EXTRA_STUB_FN16(0x39),
    BEER_EXTRA_STUB_FN16(0x3a),
    BEER_EXTRA_STUB_FN16(0x3b),
    BEER_EXTRA_STUB_FN16(0x3c),
    BEER_EXTRA_STUB_FN16(0x3d),
    BEER_EXTRA_STUB_FN16(0x3e),
    BEER_EXTRA_STUB_FN16(0x3f),
};

static void beer_extra_exports_load(void)
{
    if (g_extra_export_count >= 0) return;
    g_extra_export_count = 0;
    const char *list = getenv("BEER_EXTRA_EXPORTS");
    while (list && *list && g_extra_export_count < BEER_EXTRA_EXPORT_MAX) {
        const char *end = strchr(list, ',');
        size_t length = end ? (size_t)(end - list) : strlen(list);
        const char *bang = memchr(list, '!', length);
        if (bang && (size_t)(bang - list) < sizeof(g_extra_exports[0].dll) &&
            length - (size_t)(bang - list) - 1 < sizeof(g_extra_exports[0].name)) {
            int index = g_extra_export_count++;
            snprintf(g_extra_exports[index].dll, sizeof(g_extra_exports[index].dll), "%.*s", (int)(bang - list), list);
            for (char *c = g_extra_exports[index].dll; *c; ++c) *c = (char)tolower((unsigned char)*c);
            snprintf(g_extra_exports[index].name, sizeof(g_extra_exports[index].name), "%.*s",
                     (int)(length - (size_t)(bang - list) - 1), bang + 1);
        }
        list = end ? end + 1 : NULL;
    }
    if (g_extra_export_count) fprintf(stderr, "[EXTRA] %d diagnostic stub export(s)\n", g_extra_export_count);
}

static ImplFn beer_extra_export_impl(const char *name)
{
    beer_extra_exports_load();
    for (int i = 0; i < g_extra_export_count; ++i)
        if (!strcmp(g_extra_exports[i].name, name)) return g_extra_export_stubs[i];
    return NULL;
}
