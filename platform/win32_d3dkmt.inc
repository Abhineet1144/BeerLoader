/* D3DKMT adapter queries (gdi32): games read the driver and WDDM version and the adapter type here before
 * creating a device. Linked through the import table, these used to reach the generic stub, which returned
 * STATUS_SUCCESS without writing the output: adapter handle 0, WDDM version 0, driver 0.0.0.0. RE Engine rejects
 * such an adapter as an unsupported graphics card. One adapter is described, the GPU Beer renders on. */

#define BEER_KMT_ADAPTER_HANDLE 0x40000400u

/* User-mode driver version as Windows packs it (four 16-bit fields). NVIDIA numbers its Windows drivers
 * 32.0.1x.yyyy from the release (595.91 -> 32.0.15.9591); other vendors get a recent generic version. */
static u64 beer_umd_driver_version(void)
{
    BeerHostGpuInfo gpu;
    if (beer_vulkan_host_gpu_info(&gpu) && gpu.vendor_id == 0x10de) {
        const u32 v = nvapi_driver_version();
        return ((u64)32 << 48) | ((u64)(10 + v / 10000) << 16) | (v % 10000);
    }
    return ((u64)32 << 48) | ((u64)15 << 16) | 7200u;
}

static u64 __attribute__((ms_abi)) impl_D3DKMTOpenAdapterFromLuid(u8 *open)
{
    if (!open) return BEER_STATUS_INVALID_PARAMETER;
    *(u32 *)(open + 8) = BEER_KMT_ADAPTER_HANDLE;          /* hAdapter, after the 8-byte LUID */
    return 0;
}

static u64 __attribute__((ms_abi)) impl_D3DKMTOpenAdapterFromHdc(u8 *open)
{
    if (!open) return BEER_STATUS_INVALID_PARAMETER;
    *(u32 *)(open + 8) = BEER_KMT_ADAPTER_HANDLE;          /* { HDC, hAdapter, LUID, VidPnSourceId } */
    BeerHostGpuInfo gpu;
    *(u64 *)(open + 12) = beer_vulkan_host_gpu_info(&gpu) ? gpu.luid : 1;
    *(u32 *)(open + 20) = 0;
    return 0;
}

static u64 __attribute__((ms_abi)) impl_D3DKMTCloseAdapter(const u32 *close) { (void)close; return 0; }

/* D3DKMT_QUERYADAPTERINFO { hAdapter, Type, pPrivateDriverData, PrivateDriverDataSize } */
static u64 __attribute__((ms_abi)) impl_D3DKMTQueryAdapterInfo(u8 *query)
{
    if (!query) return BEER_STATUS_INVALID_PARAMETER;
    const u32 type = *(u32 *)(query + 4);
    u8 *data = *(u8 **)(query + 8);
    const u32 size = *(u32 *)(query + 16);
    if (*(u32 *)query != BEER_KMT_ADAPTER_HANDLE) return 0xc0000008ull;   /* STATUS_INVALID_HANDLE */
    if (!data) return BEER_STATUS_INVALID_PARAMETER;
    BeerHostGpuInfo gpu;
    const int have = beer_vulkan_host_gpu_info(&gpu);
    memset(data, 0, size);
    {
        static u64 seen;
        if (type < 64 && !(seen & (1ull << type))) {
            seen |= 1ull << type;
            fprintf(stderr, "[D3DKMT] QueryAdapterInfo type %u (%u bytes)\n", type, size);
        }
    }
    switch (type) {
    case 3:                                               /* GETSEGMENTSIZE: dedicated VRAM, -, shared */
        if (size < 24) return BEER_STATUS_INVALID_PARAMETER;
        ((u64 *)data)[0] = have ? gpu.dedicated_video_memory : 0;
        ((u64 *)data)[2] = have ? gpu.shared_system_memory : 0;
        return 0;
    case 13:                                              /* DRIVERVERSION: WDDM 2.7 (Windows 10 2004+) */
        if (size < 4) return BEER_STATUS_INVALID_PARAMETER;
        *(u32 *)data = 2700;
        return 0;
    case 15:                                              /* ADAPTERTYPE: render + display, hardware */
        if (size < 4) return BEER_STATUS_INVALID_PARAMETER;
        *(u32 *)data = 0x3;
        return 0;
    case 18: case 49:                                     /* UMD / KMD driver version */
        if (size < 8) return BEER_STATUS_INVALID_PARAMETER;
        *(u64 *)data = beer_umd_driver_version();
        return 0;
    case 30:                                              /* PHYSICALADAPTERCOUNT */
        if (size < 4) return BEER_STATUS_INVALID_PARAMETER;
        *(u32 *)data = 1;
        return 0;
    default:                                              /* capability blocks: nothing optional */
        return 0;
    }
}
