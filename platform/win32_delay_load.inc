/* Win64 RVA-based PE delay imports. Included after LoadLibrary/GetProcAddress.
 * Bound-import shortcuts and unload restoration are not needed for resolution;
 * failure callbacks and descriptor formats other than RVA are not supported. */
typedef struct {
    u32 attributes, name, module, iat, names, bound, unload, timestamp;
} BeerDelayDescriptor;
_Static_assert(sizeof(BeerDelayDescriptor) == 32, "PE delay descriptor size");

static void *delay_image_range(u64 base, size_t size, u64 rva, size_t length, int write)
{
    if (!rva || rva > size || length > size - rva || base > UINT64_MAX - size) return NULL;
    const u64 end = base + rva + length;
    for (u64 p = base + rva; p < end;) {
        BeerMemoryBasicInformation info;
        if (!impl_VirtualQuery((void *)(uintptr_t)p, &info, sizeof(info)) || info.state != 0x1000 ||
            (info.protect & 0x100)) return NULL;
        const u32 prot = info.protect & 0xff;
        if (write ? (prot != 4 && prot != 8 && prot != 0x40 && prot != 0x80) :
                    (prot != 2 && prot != 4 && prot != 8 && prot != 0x20 && prot != 0x40 && prot != 0x80))
            return NULL;
        const u64 region = (u64)(uintptr_t)info.base_address;
        if (region > p || info.region_size > UINT64_MAX - region || region + info.region_size <= p) return NULL;
        p = region + info.region_size;
    }
    return (void *)(uintptr_t)(base + rva);
}

static const char *delay_image_string(u64 base, size_t size, u64 rva)
{
    if (!rva || rva >= size || base > UINT64_MAX - size) return NULL;
    size_t limit = size - rva;
    if (limit > 4096) limit = 4096;
    const char *s = (const char *)(uintptr_t)(base + rva);
    return !beer_is_bad_string_ptr_a(s, limit) && strnlen(s, limit) < limit && *s ? s : NULL;
}

static u64 __attribute__((ms_abi)) impl_ResolveDelayLoadedAPI(
    u64 base, u64 descriptor, u64 dll_hook, u64 system_hook, u64 thunk, u32 flags)
{
    (void)dll_hook;
    (void)system_hook;
    GuestDll *dll = base ? guest_dll_from_handle(base) : NULL;
    const size_t size = dll ? dll->image_size : base && base == (u64)g_img ? g_img_size : 0;
    if (!size || descriptor < base || thunk < base || flags) goto invalid;
    const BeerDelayDescriptor *mapped = delay_image_range(base, size, descriptor - base,
                                                          sizeof(BeerDelayDescriptor), 0);
    if (!mapped) goto invalid;
    BeerDelayDescriptor d;
    memcpy(&d, mapped, sizeof(d));
    const u64 slot = thunk - base;
    if (d.attributes != 1 || !d.iat || !d.names || slot < d.iat ||
        ((slot | d.iat | d.names | d.module) & 7)) goto invalid;
    u64 *iat = delay_image_range(base, size, slot, sizeof(u64), 1);
    u64 *module = delay_image_range(base, size, d.module, sizeof(u64), 1);
    const u64 index = (slot - d.iat) / 8;
    if (!iat || !module || iat == module || d.names >= size || index >= (size - d.names) / 8)
        goto invalid;
    const u64 *imports = delay_image_range(base, size, d.names, (index + 1) * sizeof(u64), 0);
    const char *library = delay_image_string(base, size, d.name);
    if (!imports || !library) goto invalid;
    /* The name-table terminator also bounds the associated IAT. */
    for (u64 i = 0; i <= index; ++i) if (!imports[i]) goto invalid;
    const u64 import = imports[index];
    const char *symbol;
    if (import >> 63) {
        if ((import & 0x7fffffffffff0000ULL) || !(u16)import) goto invalid;
        symbol = (const char *)(uintptr_t)(u16)import;
    } else if (import <= UINT32_MAX && import &&
               (symbol = delay_image_string(base, size, import + 2))) {
        /* Import-by-name begins with a two-byte hint. */
    } else goto invalid;
    u64 handle = __atomic_load_n(module, __ATOMIC_ACQUIRE);
    if (!handle) {
        handle = impl_LoadLibraryA(library);
        if (!handle) return 0;
        u64 expected = 0;
        if (!__atomic_compare_exchange_n(module, &expected, handle, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            impl_FreeLibrary(handle);
            handle = expected;
        }
    }
    const u64 target = impl_GetProcAddress(handle, symbol);
    if (target) __atomic_store_n(iat, target, __ATOMIC_RELEASE);
    return target;

invalid:
    g_last_error = 87;
    return 0;
}