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
#define NVAPI_INCOMPATIBLE_STRUCT_VERSION (-9)
#define NVAPI_NOT_SUPPORTED (-104)
#define NVAPI_GPU_HANDLE 0x4E564750u        /* "NVGP" */
#define NVAPI_LOGICAL_GPU_HANDLE 0x4E56474Cu /* "NVGL" */

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

/* Beer exposes one active Windows display (\\.\\DISPLAY1), matching the
 * DXGI/USER32 topology.  NVAPI display ids are opaque nonzero values. */
static int __attribute__((ms_abi))
nvapi_DISP_GetDisplayIdByDisplayName(const char *display_name, u32 *display_id)
{
    if (!g_nvapi_initialized) return NVAPI_API_NOT_INITIALIZED;
    if (!display_name || !display_id) return NVAPI_INVALID_ARGUMENT;
    if (strcasecmp(display_name, "\\\\.\\DISPLAY1") != 0 &&
        strcasecmp(display_name, "\\\\DISPLAY1") != 0 &&
        strcasecmp(display_name, "DISPLAY1") != 0)
        return NVAPI_ERROR;
    *display_id = 1;
    return NVAPI_OK;
}

/* A single physical adapter also forms one logical GPU.  Streamline uses this
 * documented relationship to associate NVAPI capabilities with its DXGI
 * adapter; returning NO_IMPLEMENTATION discards otherwise valid GPU data. */
static int __attribute__((ms_abi))
nvapi_GetLogicalGPUFromPhysicalGPU(u64 physical, u64 *logical)
{
    if (!g_nvapi_initialized) return NVAPI_API_NOT_INITIALIZED;
    if (physical != NVAPI_GPU_HANDLE) return NVAPI_INVALID_HANDLE;
    if (!logical) return NVAPI_INVALID_ARGUMENT;
    *logical = NVAPI_LOGICAL_GPU_HANDLE;
    return NVAPI_OK;
}

static int __attribute__((ms_abi))
nvapi_GetPhysicalGPUsFromLogicalGPU(u64 logical, u64 *physical, u32 *count)
{
    if (!g_nvapi_initialized) return NVAPI_API_NOT_INITIALIZED;
    if (logical != NVAPI_LOGICAL_GPU_HANDLE) return NVAPI_INVALID_HANDLE;
    if (!physical || !count) return NVAPI_INVALID_ARGUMENT;
    physical[0] = NVAPI_GPU_HANDLE;
    *count = 1;
    return NVAPI_OK;
}

/* NV_GPU_LOGICAL_GPU_INFO is versioned and larger in newer NVAPI releases.
 * The stable prefix is: version, physicalGpuCount, physicalGpuHandles[64].
 * Fill only that documented prefix and preserve extension bytes owned by the
 * caller. */
static int __attribute__((ms_abi))
nvapi_GPU_GetLogicalGpuInfo(u64 logical, u8 *info)
{
    if (!g_nvapi_initialized) return NVAPI_API_NOT_INITIALIZED;
    if (logical != NVAPI_LOGICAL_GPU_HANDLE) return NVAPI_INVALID_HANDLE;
    if (!info) return NVAPI_INVALID_ARGUMENT;
    u32 version = *(u32 *)info;
    u32 size = version & 0xffffu;
    if (size < 16u + 64u * sizeof(u64))
        return NVAPI_INCOMPATIBLE_STRUCT_VERSION;
    *(u32 *)(info + 4) = 1;
    *(u64 *)(info + 8) = NVAPI_GPU_HANDLE;
    for (u32 index = 1; index < 64; ++index)
        *(u64 *)(info + 8 + index * sizeof(u64)) = 0;
    return NVAPI_OK;
}

/* NVIDIA's resource-creation extension is mostly a thin wrapper around
 * ID3D12Device::CreateCommittedResource.  Beer can truthfully support the
 * ordinary path because BD12 already implements that COM method.  NVIDIA-only
 * HTEX and CPU-visible-VRAM hints have no Vulkan equivalent in BD12 yet, so
 * report those flags as unsupported instead of silently changing semantics. */
static int __attribute__((ms_abi))
nvapi_D3D12_CreateCommittedResource(void *device, const void *heap_properties,
                                    u32 heap_flags, const void *resource_desc,
                                    u32 initial_state, const void *clear_value,
                                    const u32 *nv_params, const u8 *riid,
                                    void **resource, u8 *supported)
{
    if (getenv("BEER_NVAPI_DIAGNOSTICS"))
        fprintf(stderr,
                "[NVAPI] D3D12_CreateCommittedResource device=%p heap=%p flags=0x%x "
                "desc=%p state=0x%x clear=%p params=%p riid=%p out=%p supported=%p\n",
                device, heap_properties, heap_flags, resource_desc, initial_state,
                clear_value, nv_params, riid, resource, supported);
    if (resource) *resource = NULL;
    if (supported) *supported = 0;
    if (!g_nvapi_initialized) return NVAPI_API_NOT_INITIALIZED;
    if (!device || !heap_properties || !resource_desc || !nv_params || !riid ||
        (!resource && !supported))
        return NVAPI_INVALID_ARGUMENT;
    const u32 version = nv_params[0];
    if ((version & 0xffffu) < 8u || (version >> 16) != 1u)
        return NVAPI_INCOMPATIBLE_STRUCT_VERSION;
    /* CPUVISIBLE_VIDMEM is only an allocation-placement hint.  BD12 already
     * honors the upload/readback heap properties supplied by the caller, so
     * accepting this bit preserves ordinary resource semantics.  HTEX changes
     * texture sampling/mip behavior and remains unsupported. */
    if (nv_params[1] & ~2u)
        return NVAPI_NOT_SUPPORTED;
    /* NVIDIA documents ppvResource == NULL as a capability query.  Do not
     * create and immediately leak a D3D12 resource in that mode. */
    if (!resource) {
        *supported = 1;
        return NVAPI_OK;
    }
    /* ID3D12Device::CreateCommittedResource is vtable slot 27. */
    void **vtable = *(void ***)device;
    if (!vtable || !vtable[27]) return NVAPI_INVALID_HANDLE;
    typedef u32 (__attribute__((ms_abi)) *CreateFn)(
        void *, const void *, u32, const void *, u32, const void *,
        const u8 *, void **);
    void *created = NULL;
    u32 hr = ((CreateFn)vtable[27])(device, heap_properties, heap_flags,
                                   resource_desc, initial_state, clear_value,
                                   riid, resource ? &created : NULL);
    if (hr != 0) return NVAPI_ERROR;
    if (resource) *resource = created;
    if (supported) *supported = 1;
    return NVAPI_OK;
}

static int g_nvapi_test_create_calls;

static u32 __attribute__((ms_abi))
nvapi_test_CreateCommittedResource(void *device, const void *heap_properties,
                                   u32 heap_flags, const void *resource_desc,
                                   u32 initial_state, const void *clear_value,
                                   const u8 *riid, void **resource)
{
    (void)device;
    (void)clear_value;
    if (!heap_properties || !resource_desc || !riid || heap_flags != 3u ||
        initial_state != 7u)
        return 0x80070057u;
    ++g_nvapi_test_create_calls;
    if (resource) *resource = (void *)(uintptr_t)0x12345678u;
    return 0;
}

static int nvapi_logical_gpu_self_test(void)
{
    u64 logical = 0, physical[64] = {0};
    u32 count = 0;
    g_nvapi_initialized = 1;
    if (nvapi_GetLogicalGPUFromPhysicalGPU(NVAPI_GPU_HANDLE, &logical) != NVAPI_OK ||
        logical != NVAPI_LOGICAL_GPU_HANDLE)
        return 0;
    if (nvapi_GetPhysicalGPUsFromLogicalGPU(logical, physical, &count) != NVAPI_OK ||
        count != 1 || physical[0] != NVAPI_GPU_HANDLE)
        return 0;
    u8 info[16 + 64 * sizeof(u64)];
    memset(info, 0xa5, sizeof(info));
    *(u32 *)info = (u32)sizeof(info) | (1u << 16);
    if (nvapi_GPU_GetLogicalGpuInfo(logical, info) != NVAPI_OK ||
        *(u32 *)(info + 4) != 1 || *(u64 *)(info + 8) != NVAPI_GPU_HANDLE ||
        *(u64 *)(info + 16) != 0)
        return 0;
    if (nvapi_GetLogicalGPUFromPhysicalGPU(0, &logical) != NVAPI_INVALID_HANDLE ||
        nvapi_GetLogicalGPUFromPhysicalGPU(NVAPI_GPU_HANDLE, NULL) != NVAPI_INVALID_ARGUMENT)
        return 0;
    u32 display_id = 0;
    if (nvapi_DISP_GetDisplayIdByDisplayName("\\\\.\\DISPLAY1", &display_id) != NVAPI_OK ||
        display_id != 1 ||
        nvapi_DISP_GetDisplayIdByDisplayName("\\\\.\\DISPLAY9", &display_id) != NVAPI_ERROR)
        return 0;
    u32 params[2] = { 8u | (1u << 16), 1u };
    u8 supported = 1;
    if (nvapi_D3D12_CreateCommittedResource((void *)1, (void *)1, 0,
            (void *)1, 0, NULL, params, (const u8 *)1, NULL, &supported) != NVAPI_NOT_SUPPORTED ||
        supported != 0)
        return 0;

    /* CPU-visible video memory is a placement hint and must retain ordinary
     * committed-resource capability when the supplied heap is otherwise valid. */
    params[1] = 2u;
    supported = 0;
    if (nvapi_D3D12_CreateCommittedResource((void *)1, (void *)1, 0,
            (void *)1, 0, NULL, params, (const u8 *)1, NULL, &supported) != NVAPI_OK ||
        supported != 1)
        return 0;

    void *vtable[28] = {0};
    vtable[27] = nvapi_test_CreateCommittedResource;
    void **device = vtable;
    params[1] = 0;
    supported = 0;
    g_nvapi_test_create_calls = 0;
    if (nvapi_D3D12_CreateCommittedResource(&device, (void *)1, 3,
            (void *)1, 7, NULL, params, (const u8 *)1, NULL, &supported) != NVAPI_OK ||
        supported != 1 || g_nvapi_test_create_calls != 0)
        return 0;

    void *resource = NULL;
    supported = 0;
    if (nvapi_D3D12_CreateCommittedResource(&device, (void *)1, 3,
            (void *)1, 7, NULL, params, (const u8 *)1, &resource, &supported) != NVAPI_OK ||
        supported != 1 || resource != (void *)(uintptr_t)0x12345678u ||
        g_nvapi_test_create_calls != 1)
        return 0;
    return 1;
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
        { 0xAE457190u, nvapi_DISP_GetDisplayIdByDisplayName },
        { 0xADD604D1u, nvapi_GetLogicalGPUFromPhysicalGPU },
        { 0xAEA3FA32u, nvapi_GetPhysicalGPUsFromLogicalGPU },
        { 0x842B066Eu, nvapi_GPU_GetLogicalGpuInfo },
        { 0x027E98AEu, nvapi_D3D12_CreateCommittedResource },
        { 0xD8265D24u, nvapi_GPU_GetArchInfo },
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(*table); ++i)
        if (table[i].id == id) return table[i].fn;
    static _Atomic(u32) logged;
    if (atomic_fetch_add(&logged, 1) < 64)
        fprintf(stderr, "[NVAPI] interface 0x%08X is not implemented (called from %p)\n", id, __builtin_return_address(0));
    return NULL;
}
