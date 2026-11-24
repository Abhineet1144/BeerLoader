/* Win64 ALPC message attribute buffers. This is layout handling only, not
 * ALPC transport or handle transfer. Records are packed in descending bit order. */
typedef struct { u32 allocated, valid; } BeerAlpcAttributes;
_Static_assert(sizeof(BeerAlpcAttributes) == 8, "ALPC_MESSAGE_ATTRIBUTES layout");

static u64 __attribute__((ms_abi)) impl_AlpcGetHeaderSize(u32 flags)
{
    /* Security, view, context, handle, reserved, direct, work-on-behalf. */
    static const u8 sizes[] = {24, 32, 32, 24, 24, 8, 8};
    size_t size = sizeof(BeerAlpcAttributes);
    for (size_t i = 0; i < sizeof(sizes); ++i)
        if (flags & (0x80000000u >> i)) size += sizes[i];
    return size;
}

static u64 __attribute__((ms_abi)) impl_AlpcInitializeMessageAttribute(
    u32 flags, BeerAlpcAttributes *attributes, u64 capacity, u64 *required)
{
    if (!required) return 0xc000000du; /* STATUS_INVALID_PARAMETER */
    const u64 size = impl_AlpcGetHeaderSize(flags);
    *required = size;
    if (capacity < size) return 0xc0000023u; /* STATUS_BUFFER_TOO_SMALL */
    if (attributes) *attributes = (BeerAlpcAttributes){ .allocated = flags };
    return 0;
}

static u64 __attribute__((ms_abi)) impl_AlpcGetMessageAttribute(
    const BeerAlpcAttributes *attributes, u32 flag)
{
    if (!attributes || !(flag & 0xfe000000u) || (flag & (flag - 1)) ||
        !(attributes->allocated & flag)) return 0;
    /* An allocated slot can be retrieved before the caller marks it valid. */
    const u32 preceding = attributes->allocated & ~(flag | (flag - 1));
    return (u64)(uintptr_t)((const u8 *)attributes + impl_AlpcGetHeaderSize(preceding));
}