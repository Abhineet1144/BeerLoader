/* NVAPI: nvapi64.dll exports a single function, nvapi_QueryInterface(id), that hands out the other entry points by
 * hashed id.  Games use it for driver and GPU identity checks ("driver 511.79 or newer"), VRAM size and vendor
 * extensions; every function they treat as optional can simply be missing, so only the identity queries are
 * answered and unknown ids are logged once and return NULL.  The values come from the Vulkan device (and
 * /proc/driver/nvidia/version for the driver), so BEER_SPOOF_GPU_* stays consistent with DXGI. */

#define NVAPI_OK 0
#define NVAPI_ERROR (-1)
#define NVAPI_NO_IMPLEMENTATION (-3)
#define NVAPI_API_NOT_INITIALIZED (-4)
#define NVAPI_INVALID_ARGUMENT (-5)
#define NVAPI_NVIDIA_DEVICE_NOT_FOUND (-6)
#define NVAPI_END_ENUMERATION (-7)
#define NVAPI_INVALID_HANDLE (-8)
#define NVAPI_GPU_HANDLE 0x4E564750u        /* "NVGP" */

static _Atomic(int) g_nvapi_initialized;

/* Driver version as NVAPI reports it: 595.91 -> 59591.  BEER_SPOOF_NVAPI_DRIVER (decimal) overrides. */
static u32 nvapi_driver_version(void)
{
    const char *forced = getenv("BEER_SPOOF_NVAPI_DRIVER");
    if (forced && *forced) return (u32)strtoul(forced, NULL, 10);
    FILE *f = fopen("/proc/driver/nvidia/version", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            const char *p = strstr(line, "Module");
            unsigned major = 0, minor = 0;
            if (p && sscanf(p + 6, " %u.%u", &major, &minor) == 2) { fclose(f); return major * 100 + minor; }
        }
        fclose(f);
    }
    return 53706;                            /* 537.06: a recent driver when the host's cannot be read */
}

static int nvapi_gpu(BeerHostGpuInfo *gpu)
{
    return beer_vulkan_host_gpu_info(gpu) && gpu->vendor_id == 0x10de;
}

static int __attribute__((ms_abi)) nvapi_Initialize(void)
{
    BeerHostGpuInfo gpu;
    if (!nvapi_gpu(&gpu)) return NVAPI_NVIDIA_DEVICE_NOT_FOUND;
    g_nvapi_initialized = 1;
    return NVAPI_OK;
}
static int __attribute__((ms_abi)) nvapi_Unload(void) { g_nvapi_initialized = 0; return NVAPI_OK; }

static int __attribute__((ms_abi)) nvapi_GetErrorMessage(int status, char *text)
{
    if (!text) return NVAPI_INVALID_ARGUMENT;
    snprintf(text, 64, status == NVAPI_OK ? "NVAPI_OK" : "NVAPI error %d", status);
    return NVAPI_OK;
}

static int __attribute__((ms_abi)) nvapi_GetInterfaceVersionString(char *text)
{
    if (!text) return NVAPI_INVALID_ARGUMENT;
    snprintf(text, 64, "NVAPI Open Source Interface (Beer)");
    return NVAPI_OK;
}

static int __attribute__((ms_abi)) nvapi_SYS_GetDriverAndBranchVersion(u32 *version, char *branch)
{
    if (!g_nvapi_initialized) return NVAPI_API_NOT_INITIALIZED;
    if (!version || !branch) return NVAPI_INVALID_ARGUMENT;
    *version = nvapi_driver_version();
    snprintf(branch, 64, "r%u_00", *version / 100);
    return NVAPI_OK;
}

static int __attribute__((ms_abi)) nvapi_EnumPhysicalGPUs(u64 *handles, u32 *count)
{
    if (!g_nvapi_initialized) return NVAPI_API_NOT_INITIALIZED;
    if (!handles || !count) return NVAPI_INVALID_ARGUMENT;
    handles[0] = NVAPI_GPU_HANDLE;
    *count = 1;
    return NVAPI_OK;
}

static int __attribute__((ms_abi)) nvapi_GPU_GetFullName(u64 handle, char *name)
{
    BeerHostGpuInfo gpu;
    if (handle != NVAPI_GPU_HANDLE) return NVAPI_INVALID_HANDLE;
    if (!name) return NVAPI_INVALID_ARGUMENT;
    if (!nvapi_gpu(&gpu)) return NVAPI_NVIDIA_DEVICE_NOT_FOUND;
    snprintf(name, 64, "%s", gpu.name);
    return NVAPI_OK;
}

static int __attribute__((ms_abi)) nvapi_GPU_GetPCIIdentifiers(u64 handle, u32 *device, u32 *subsystem, u32 *revision, u32 *ext)
{
    BeerHostGpuInfo gpu;
    if (handle != NVAPI_GPU_HANDLE) return NVAPI_INVALID_HANDLE;
    if (!nvapi_gpu(&gpu)) return NVAPI_NVIDIA_DEVICE_NOT_FOUND;
    if (device) *device = (gpu.device_id << 16) | gpu.vendor_id;
    if (subsystem) *subsystem = 0;
    if (revision) *revision = gpu.revision;
    if (ext) *ext = gpu.device_id;
    return NVAPI_OK;
}

static int __attribute__((ms_abi)) nvapi_GPU_GetFrameBufferSize(u64 handle, u32 *kilobytes)
{
    BeerHostGpuInfo gpu;
    if (handle != NVAPI_GPU_HANDLE) return NVAPI_INVALID_HANDLE;
    if (!kilobytes) return NVAPI_INVALID_ARGUMENT;
    if (!nvapi_gpu(&gpu)) return NVAPI_NVIDIA_DEVICE_NOT_FOUND;
    *kilobytes = (u32)(gpu.dedicated_video_memory / 1024);
    return NVAPI_OK;
}

static int __attribute__((ms_abi)) nvapi_GPU_GetGPUType(u64 handle, u32 *type)
{
    if (handle != NVAPI_GPU_HANDLE) return NVAPI_INVALID_HANDLE;
    if (!type) return NVAPI_INVALID_ARGUMENT;
    *type = 2;                               /* NV_SYSTEM_TYPE_GPU_DISCRETE */
    return NVAPI_OK;
}

static int __attribute__((ms_abi)) nvapi_GPU_GetSystemType(u64 handle, u32 *type)
{
    if (handle != NVAPI_GPU_HANDLE) return NVAPI_INVALID_HANDLE;
    if (!type) return NVAPI_INVALID_ARGUMENT;
    *type = 2;                               /* NV_SYSTEM_TYPE_DESKTOP */
    return NVAPI_OK;
}

static int __attribute__((ms_abi)) nvapi_GPU_GetBusId(u64 handle, u32 *bus)
{
    if (handle != NVAPI_GPU_HANDLE) return NVAPI_INVALID_HANDLE;
    if (!bus) return NVAPI_INVALID_ARGUMENT;
    *bus = 1;
    return NVAPI_OK;
}

/* NvAPI_GPU_GetArchInfo: NV_GPU_ARCH_INFO { version, architecture, implementation, revision } (16 bytes, v1/v2).
 * Engines pick render paths, or refuse the GPU, by architecture; it follows from the PCI device id range of each
 * generation. */
static int __attribute__((ms_abi)) nvapi_GPU_GetArchInfo(u64 handle, u32 *info)
{
    if (handle != NVAPI_GPU_HANDLE) return NVAPI_INVALID_HANDLE;
    if (!info) return NVAPI_INVALID_ARGUMENT;
    if ((info[0] & 0xffff) != 16 || (info[0] >> 16) < 1 || (info[0] >> 16) > 2) return -9;   /* INCOMPATIBLE_STRUCT_VERSION */
    BeerHostGpuInfo gpu;
    if (!nvapi_gpu(&gpu)) return NVAPI_NVIDIA_DEVICE_NOT_FOUND;
    const u32 id = gpu.device_id;
    u32 arch, impl = 0;
    if (id >= 0x2900) { arch = 0x1b0; impl = id >= 0x2f00 ? 7 : id >= 0x2d00 ? 6 : id >= 0x2c00 ? 5 : 2; }  /* Blackwell */
    else if (id >= 0x2600) { arch = 0x190; impl = id >= 0x28a0 ? 7 : id >= 0x2800 ? 6 : id >= 0x2780 ? 4 : id >= 0x2700 ? 3 : 2; }  /* Ada */
    else if (id >= 0x2200) { arch = 0x170; impl = id >= 0x25a0 ? 7 : id >= 0x2500 ? 6 : id >= 0x2400 ? 4 : id >= 0x2200 && id < 0x2300 ? 2 : 0; }  /* Ampere */
    else if (id >= 0x1e00) { arch = 0x160; impl = id >= 0x2180 ? 8 : id >= 0x1f80 ? 7 : id >= 0x1f00 ? 6 : id >= 0x1e80 ? 4 : 2; }  /* Turing */
    else if (id >= 0x1db0 && id < 0x1dc0) { arch = 0x140; }                                                           /* Volta */
    else if (id >= 0x15f0) { arch = 0x130; impl = id >= 0x1d00 ? 8 : id >= 0x1c80 ? 7 : id >= 0x1c00 ? 6 : id >= 0x1b80 ? 4 : 2; }  /* Pascal */
    else { arch = 0x120; }                                                                                             /* Maxwell */
    info[1] = arch;
    info[2] = impl;
    info[3] = gpu.revision ? gpu.revision : 0xa1;
    return NVAPI_OK;
}

static int __attribute__((ms_abi)) nvapi_EnumNvidiaDisplayHandle(u32 index, u64 *handle)
{
    if (!g_nvapi_initialized) return NVAPI_API_NOT_INITIALIZED;
    if (!handle) return NVAPI_INVALID_ARGUMENT;
    if (index != 0) return NVAPI_END_ENUMERATION;
    *handle = 0x4E564431u;                   /* "NVD1" */
    return NVAPI_OK;
}

static void *__attribute__((ms_abi)) impl_nvapi_QueryInterface(u32 id)
{
    static const struct { u32 id; void *fn; } table[] = {
        { 0x0150E828u, nvapi_Initialize },
        { 0xD22BDD7Eu, nvapi_Unload },
        { 0x6C2D048Cu, nvapi_GetErrorMessage },
        { 0x01053FA5u, nvapi_GetInterfaceVersionString },
        { 0x2926AAADu, nvapi_SYS_GetDriverAndBranchVersion },
        { 0xE5AC921Fu, nvapi_EnumPhysicalGPUs },
        { 0xCEEE8E9Fu, nvapi_GPU_GetFullName },
        { 0x2DDFB66Eu, nvapi_GPU_GetPCIIdentifiers },
        { 0x46FBEB03u, nvapi_GPU_GetFrameBufferSize },      /* GetPhysicalFrameBufferSize */
        { 0x5A04B644u, nvapi_GPU_GetFrameBufferSize },      /* GetVirtualFrameBufferSize */
        { 0xC33BAEB1u, nvapi_GPU_GetGPUType },
        { 0xBAAABFCCu, nvapi_GPU_GetSystemType },
        { 0x1BE0B8E5u, nvapi_GPU_GetBusId },
        { 0x9ABDD40Du, nvapi_EnumNvidiaDisplayHandle },
        { 0xD8265D24u, nvapi_GPU_GetArchInfo },
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(*table); ++i)
        if (table[i].id == id) return table[i].fn;
    static _Atomic(u32) logged;
    if (atomic_fetch_add(&logged, 1) < 64)
        fprintf(stderr, "[NVAPI] interface 0x%08X is not implemented (called from %p)\n", id, __builtin_return_address(0));
    return NULL;
}
