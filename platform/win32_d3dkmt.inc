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

/* KMTQAITYPE_QUERYREGISTRY. D3DDDI_QUERYREGISTRY_INFO: QueryType @0 (0 service key, 1 adapter key, 2 driver store
 * path, 3 driver image path), QueryFlags @4, ValueName[260] @8, ValueType @528, PhysicalAdapterIndex @532,
 * OutputValueSize @536, Status @540 (0 success, 1 buffer overflow, 2 fail), output @544. The adapter key's memory
 * values describe the GPU; any other value is reported as absent (Status FAIL), as for a key without it. Engines
 * read video memory from HardwareInformation.qwMemorySize; an empty "success" there read as 0 bytes. */
static u64 beer_kmt_query_registry(const u8 *query, u8 *data, u32 size, const BeerHostGpuInfo *gpu)
{
    (void)query;
    if (size < 552) return BEER_STATUS_INVALID_PARAMETER;
    const u32 type = *(const u32 *)data;
    const u16 *wide = (const u16 *)(data + 8);
    char name[261];
    size_t n = 0;
    for (; n < 260 && wide[n]; ++n) name[n] = wide[n] < 0x80 ? (char)wide[n] : '?';
    name[n] = 0;
    const u32 room = size - 544;
    u64 qword = 0;
    u32 value_type = 0, value_size = 0;
    u16 text[160];   /* REG_SZ values (UTF-16, NUL-terminated) */
    const void *value = &qword;
    if (type == 1 && gpu && !strcasecmp(name, "HardwareInformation.qwMemorySize")) {
        qword = gpu->dedicated_video_memory; value_type = 11; value_size = 8;            /* REG_QWORD */
    } else if (type == 1 && gpu && !strcasecmp(name, "HardwareInformation.MemorySize")) {
        qword = gpu->dedicated_video_memory > 0xffffffffull ? 0xffffffffull : gpu->dedicated_video_memory;
        value_type = 3; value_size = 4;                                                   /* REG_BINARY, 4 bytes */
    } else if (g_ngx_windows_path && !strcasecmp(name, "NGXCore\\NGXPath")) {
        /* The NVIDIA driver publishes its NGX (DLSS) core folder on the display driver's keys; current NGX loaders
         * (Streamline 2.x) ask for it through this query and give up with "adapter query failed" without it. */
        size_t i = 0;
        for (; g_ngx_windows_path[i] && i < 159; ++i) text[i] = (u16)(unsigned char)g_ngx_windows_path[i];
        text[i] = 0;
        value = text; value_type = 1; value_size = (u32)((i + 1) * 2);                 /* REG_SZ */
    }
    {
        static int logs;
        if (logs++ < 32)
            fprintf(stderr, "[D3DKMT] QueryRegistry type %u \"%s\" -> %s\n", type, name, value_size ? "value" : "absent");
    }
    *(u32 *)(data + 528) = value_type;
    if (!value_size) {
        *(u32 *)(data + 536) = 0;
        *(u32 *)(data + 540) = 2;                                                         /* FAIL */
        return 0;
    }
    *(u32 *)(data + 536) = value_size;
    if (room < value_size) { *(u32 *)(data + 540) = 1; return 0; }                        /* BUFFER_OVERFLOW */
    memcpy(data + 544, value, value_size);
    *(u32 *)(data + 540) = 0;
    return 0;
}

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
    if (type != 48) memset(data, 0, size);                /* QUERYREGISTRY carries its input in the buffer */
    {
        static u64 seen[2];
        if (type < 128 && !(seen[type / 64] & (1ull << (type % 64)))) {
            seen[type / 64] |= 1ull << (type % 64);
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
    case 48:                                              /* QUERYREGISTRY: D3DDDI_QUERYREGISTRY_INFO */
        return beer_kmt_query_registry(query, data, size, have ? &gpu : NULL);
    case 70:                                              /* WDDM_2_7_CAPS: hardware GPU scheduling */
        /* HwSchSupported | HwSchEnabled | HwSchEnabledByDefault on NVIDIA: the Linux driver schedules GPU work in
         * hardware, which is what Windows' HAGS switch selects. Streamline's DLSS frame generation requires it
         * ("not supported on current hardware" otherwise). Other vendors: not reported. */
        if (size < 4) return BEER_STATUS_INVALID_PARAMETER;
        *(u32 *)data = have && gpu.vendor_id == 0x10de ? 0x7u : 0u;
        return 0;
    case 30:                                              /* PHYSICALADAPTERCOUNT */
        if (size < 4) return BEER_STATUS_INVALID_PARAMETER;
        *(u32 *)data = 1;
        return 0;
    default:                                              /* capability blocks: nothing optional */
        return 0;
    }
}

/* D3DKMT_ADAPTERINFO (20 bytes): hAdapter, LUID, NumOfSources, bPrecisePresentRegionsPreferred. */
static void beer_kmt_adapter_info(u8 *info)
{
    BeerHostGpuInfo gpu;
    const u64 luid = beer_vulkan_host_gpu_info(&gpu) ? gpu.luid : 1;
    memset(info, 0, 20);
    *(u32 *)info = BEER_KMT_ADAPTER_HANDLE;
    *(u32 *)(info + 4) = (u32)luid;
    *(u32 *)(info + 8) = (u32)(luid >> 32);
    *(u32 *)(info + 12) = 1;
}

/* D3DKMT_ENUMADAPTERS2 { ULONG NumAdapters; D3DKMT_ADAPTERINFO *pAdapters; }: a NULL array asks for the count. */
static u64 __attribute__((ms_abi)) impl_D3DKMTEnumAdapters2(u8 *desc)
{
    if (!desc) return BEER_STATUS_INVALID_PARAMETER;
    u8 *adapters = *(u8 **)(desc + 8);
    const u32 room = *(u32 *)desc;
    *(u32 *)desc = 1;
    if (!adapters) return 0;
    if (room < 1) return 0xc0000023ull;                   /* STATUS_BUFFER_TOO_SMALL */
    beer_kmt_adapter_info(adapters);
    return 0;
}

/* D3DKMT_OPENADAPTERFROMDEVICENAME { const WCHAR *pDeviceName; hAdapter; LUID } */
static u64 __attribute__((ms_abi)) impl_D3DKMTOpenAdapterFromDeviceName(u8 *open)
{
    if (!open || !*(void **)open) return BEER_STATUS_INVALID_PARAMETER;
    BeerHostGpuInfo gpu;
    *(u32 *)(open + 8) = BEER_KMT_ADAPTER_HANDLE;
    *(u64 *)(open + 12) = beer_vulkan_host_gpu_info(&gpu) ? gpu.luid : 1;
    return 0;
}

/* D3DKMT_QUERYSTATISTICS (808 bytes): Type @0, AdapterLuid @4, hProcess @16, QueryResult @24 (776 bytes), then the
 * query input (SegmentId / NodeId / VidPnSourceId) @800. Two memory segments, as on a discrete GPU: 0 is video
 * memory, 1 the system-memory aperture. Commitments are BD12's live heaps. */
extern uint64_t g_bd12_heap_bytes[4];
static u64 __attribute__((ms_abi)) impl_D3DKMTQueryStatistics(u8 *query)
{
    if (!query) return BEER_STATUS_INVALID_PARAMETER;
    const u32 type = *(u32 *)query;
    u8 *result = query + 24;
    memset(result, 0, 776);
    BeerHostGpuInfo gpu;
    const int have = beer_vulkan_host_gpu_info(&gpu);
    const u64 vram = have ? gpu.dedicated_video_memory : 0;
    const u64 shared = have ? gpu.shared_system_memory : 0;
    const u64 vram_used = __atomic_load_n(&g_bd12_heap_bytes[1], __ATOMIC_RELAXED);
    const u64 shared_used = __atomic_load_n(&g_bd12_heap_bytes[2], __ATOMIC_RELAXED) +
                            __atomic_load_n(&g_bd12_heap_bytes[3], __ATOMIC_RELAXED);
    {
        static u32 seen;
        if (type < 32 && !(seen & (1u << type))) {
            seen |= 1u << type;
            fprintf(stderr, "[D3DKMT] QueryStatistics type %u\n", type);
        }
    }
    switch (type) {
    case 0:                                               /* ADAPTER: NbSegments, NodeCount, VidPnSourceCount */
        *(u32 *)result = 2;
        *(u32 *)(result + 4) = 1;
        *(u32 *)(result + 8) = 1;
        return 0;
    case 3: {                                             /* SEGMENT: CommitLimit, BytesCommitted, BytesResident, .. Aperture @40 */
        const u32 segment = *(u32 *)(query + 800);
        if (segment > 1) return BEER_STATUS_INVALID_PARAMETER;
        *(u64 *)result = segment ? shared : vram;
        *(u64 *)(result + 8) = *(u64 *)(result + 16) = segment ? shared_used : vram_used;
        *(u32 *)(result + 40) = segment;
        return 0;
    }
    case 4: {                                             /* PROCESS_SEGMENT: BytesCommitted, MaximumWorkingSet, ... */
        const u32 segment = *(u32 *)(query + 800);
        if (segment > 1) return BEER_STATUS_INVALID_PARAMETER;
        *(u64 *)result = segment ? shared_used : vram_used;
        *(u64 *)(result + 8) = segment ? shared : vram;
        return 0;
    }
    default:                                              /* process, node and source counters: none recorded */
        return 0;
    }
}
