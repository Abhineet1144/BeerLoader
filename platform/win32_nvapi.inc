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

/* ---- D3D12 CUDA interop (NGX / DLSS): graphics/d3d12v/bd12_cuda.c ---- */
#include "../graphics/d3d12v/bd12_cuda.h"

/* CUDA compute capability of the architecture GetArchInfo reports (consumer parts of each generation). */
static int nvapi_cuda_sm(u32 *major, u32 *minor)
{
    u32 info[4] = { 16u | (2u << 16) };
    if (nvapi_GPU_GetArchInfo(NVAPI_GPU_HANDLE, info) != NVAPI_OK) return 0;
    switch (info[1]) {
    case 0x1b0: *major = 12; *minor = 0; return 1;   /* Blackwell */
    case 0x190: *major = 8; *minor = 9; return 1;    /* Ada */
    case 0x170: *major = 8; *minor = 6; return 1;    /* Ampere */
    case 0x160: *major = 7; *minor = 5; return 1;    /* Turing */
    case 0x140: *major = 7; *minor = 0; return 1;    /* Volta */
    case 0x130: *major = 6; *minor = 1; return 1;    /* Pascal */
    default: *major = 5; *minor = 2; return 1;       /* Maxwell */
    }
}

static int __attribute__((ms_abi))
nvapi_D3D12_CreateCubinComputeShader(void *device, const void *cubin, u32 size, u32 bx, u32 by, u32 bz, u64 *shader)
{ return bd12_cuda_create_shader(device, cubin, size, bx, by, bz, 0, "", 0, shader); }
static int __attribute__((ms_abi))
nvapi_D3D12_CreateCubinComputeShaderWithName(void *device, const void *cubin, u32 size, u32 bx, u32 by, u32 bz,
                                             const char *name, u64 *shader)
{ return bd12_cuda_create_shader(device, cubin, size, bx, by, bz, 0, name, 0, shader); }
static int __attribute__((ms_abi))
nvapi_D3D12_CreateCubinComputeShaderEx(void *device, const void *cubin, u32 size, u32 bx, u32 by, u32 bz,
                                       u32 shared, const char *name, u64 *shader)
{ return bd12_cuda_create_shader(device, cubin, size, bx, by, bz, shared, name, 0, shader); }
/* NVAPI_D3D12_CREATE_CUBIN_SHADER_PARAMS: structSizeIn, structSizeOut, pDevice, pCubin, size, blockX/Y/Z,
 * dynSharedMemBytes, pShaderName, flags, hShader. */
typedef struct {
    u64 size_in, size_out;
    void *device;
    const void *cubin;
    u32 size, block_x, block_y, block_z, shared;
    u32 pad0;
    const char *name;
    u32 flags;
    u32 pad1;
    u64 shader;
} NvCubinShaderParams;
static int __attribute__((ms_abi)) nvapi_D3D12_CreateCubinComputeShaderExV2(NvCubinShaderParams *p)
{
    if (!p || p->size_in < sizeof(*p)) return NVAPI_INVALID_ARGUMENT;
    p->size_out = sizeof(*p);
    /* ExV2 modules use 64-bit texturing (their texture/surface handles come from the merged and independent
     * descriptor-object calls), as on the Windows driver and in vkd3d-proton. */
    return bd12_cuda_create_shader(p->device, p->cubin, p->size, p->block_x, p->block_y, p->block_z, p->shared,
                                   p->name, 1, &p->shader);
}
static int __attribute__((ms_abi)) nvapi_D3D12_DestroyCubinComputeShader(void *device, u64 shader)
{ return bd12_cuda_destroy_shader(device, shader); }
static int __attribute__((ms_abi))
nvapi_D3D12_LaunchCubinShader(void *list, u64 shader, u32 gx, u32 gy, u32 gz, const void *params, u32 size)
{ return bd12_cuda_launch(list, shader, gx, gy, gz, params, size); }
static int __attribute__((ms_abi)) nvapi_D3D12_GetCudaTextureObject(void *device, u64 srv, u64 sampler, u32 *handle)
{ return bd12_cuda_texture_object(device, srv, sampler, handle); }
static int __attribute__((ms_abi)) nvapi_D3D12_GetCudaSurfaceObject(void *device, u64 uav, u32 *handle)
{ return bd12_cuda_surface_object(device, uav, handle); }
/* NVAPI_D3D12_GET_CUDA_MERGED_TEXTURE_SAMPLER_OBJECT_PARAMS: sizes, pDevice, texDesc, smpDesc, textureHandle (u64). */
typedef struct { u64 size_in, size_out; void *device; u64 texture, sampler, handle; } NvMergedTextureParams;
static int __attribute__((ms_abi)) nvapi_D3D12_GetCudaMergedTextureSamplerObject(NvMergedTextureParams *p)
{
    if (!p || p->size_in < sizeof(*p)) return NVAPI_INVALID_ARGUMENT;
    p->size_out = sizeof(*p);
    if (bd12_cuda_texturing64()) return bd12_cuda_view_handle64(p->device, p->texture, p->sampler, 0, &p->handle);
    u32 handle = 0;
    const int status = bd12_cuda_texture_object(p->device, p->texture, p->sampler, &handle);
    p->handle = handle;
    return status;
}
/* NvAPI_D3D12_CaptureUAVInfo(device, NVAPI_UAV_INFO*): the next CreateUnorderedAccessView on this thread fills it
 * (bd12_cuda.c). V1 is version 1 (24 bytes), V2 adds outFlags (40 bytes, version 2). */
static int __attribute__((ms_abi)) nvapi_D3D12_CaptureUAVInfo(void *device, u32 *info)
{
    if (!device || !info) return NVAPI_INVALID_ARGUMENT;
    if (info[0] != 1u && info[0] != (40u | (2u << 16))) return NVAPI_INCOMPATIBLE_STRUCT_VERSION;
    if (info[0] != 1u) { info[8] = info[9] = 0; }        /* outFlags */
    return bd12_cuda_capture_uav_info(device, info);
}
/* NVAPI_D3D12_GET_CUDA_INDEPENDENT_DESCRIPTOR_OBJECT_PARAMS: sizes, pDevice, type (0 surface, 1 texture,
 * 2 sampler), CPU descriptor handle, handle (u64). 64-bit handles; separate samplers have no Vulkan equivalent
 * (vkd3d-proton does not offer them either). */
typedef struct { u64 size_in, size_out; void *device; u32 type, pad; u64 desc, handle; } NvIndependentDescriptorParams;
static int __attribute__((ms_abi)) nvapi_D3D12_GetCudaIndependentDescriptorObject(NvIndependentDescriptorParams *p)
{
    if (!p || p->size_in < sizeof(*p)) return NVAPI_INVALID_ARGUMENT;
    p->size_out = sizeof(*p);
    p->handle = 0;
    if (p->type > 1) return NVAPI_NOT_SUPPORTED;
    return bd12_cuda_view_handle64(p->device, p->desc, 0, p->type == 0, &p->handle);
}
static int __attribute__((ms_abi)) nvapi_D3D12_IsFatbinPTXSupported(void *device, u8 *supported)
{
    (void)device;
    if (!supported) return NVAPI_INVALID_ARGUMENT;
    /* vkCreateCuModuleNVX takes fatbins with PTX (the driver JIT-compiles them), as dxvk-nvapi reports with both NVX
     * extensions. Answering "no" sent NGX (DLSS 310) to an architecture-specific SASS kernel set it does not ship
     * (InitCubins -> NVAPI_NOT_SUPPORTED: DLSS unavailable). */
    *supported = bd12_cuda_available() ? 1 : 0;
    return NVAPI_OK;
}
/* NV_D3D12_GRAPHICS_CAPS_V1: flags (exclusive scissor, VRS, fast UAV clear), majorSM, minorSM, majorCudaSM,
 * minorCudaSM (u16 each), reserved[5]. */
static int __attribute__((ms_abi)) nvapi_D3D12_GetGraphicsCapabilities(void *device, u32 version, u8 *caps)
{
    (void)device;
    if (!caps) return NVAPI_INVALID_ARGUMENT;
    if (version != (32u | (1u << 16))) return NVAPI_INCOMPATIBLE_STRUCT_VERSION;
    u32 major = 0, minor = 0;
    if (!nvapi_cuda_sm(&major, &minor)) return NVAPI_NVIDIA_DEVICE_NOT_FOUND;
    memset(caps, 0, 32);
    /* majorSMVersion/minorSMVersion: the GPU's SM architecture. The CUDA SM pair stays 0, as dxvk-nvapi reports it:
     * with it set, NGX (DLSS 310) looked for a kernel set it does not ship (InitCubins -> NVAPI_NOT_SUPPORTED) and
     * DLSS failed to initialise. UAV clears need no padding on Vulkan (fast clear); VRS follows the device. */
    const u16 sm[4] = { (u16)major, (u16)minor, 0, 0 };
    memcpy(caps + 4, sm, sizeof(sm));
    const BeerVulkanModern *m = beer_vulkan_modern();
    *(u32 *)caps = 4u | (m && m->vrs_tier ? 2u : 0u);   /* bFastUAVClearSupported, bVariablePixelRateShadingSupported */
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
    /* NV_LOGICAL_GPU_DATA_V1 (x64): version@0, pOSAdapterId@8 (caller buffer that receives the adapter LUID),
     * physicalGpuCount@16, physicalGpuHandles[64]@24, reserved[8]. NGX matches the D3D12 adapter to the NVAPI GPU
     * through that LUID, so it is the one DXGI and ID3D12Device::GetAdapterLuid report. */
    if (!g_nvapi_initialized) return NVAPI_API_NOT_INITIALIZED;
    if (logical != NVAPI_LOGICAL_GPU_HANDLE) return NVAPI_INVALID_HANDLE;
    if (!info) return NVAPI_INVALID_ARGUMENT;
    u32 version = *(u32 *)info;
    u32 size = version & 0xffffu;
    if (size < 24u + 64u * sizeof(u64) || (version >> 16) != 1)
        return NVAPI_INCOMPATIBLE_STRUCT_VERSION;
    u8 *luid = (u8 *)(uintptr_t)*(u64 *)(info + 8);
    if (luid) {
        BeerHostGpuInfo gpu;
        const u64 value = nvapi_gpu(&gpu) ? gpu.luid : 1;
        memcpy(luid, &value, 8);
    }
    *(u32 *)(info + 16) = 1;
    *(u64 *)(info + 24) = NVAPI_GPU_HANDLE;
    for (u32 index = 1; index < 64; ++index)
        *(u64 *)(info + 24 + index * sizeof(u64)) = 0;
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
    u8 info[24 + 64 * sizeof(u64) + 32];
    u64 luid = 0xa5a5a5a5a5a5a5a5ull;
    memset(info, 0xa5, sizeof(info));
    *(u32 *)info = (u32)sizeof(info) | (1u << 16);
    *(u64 *)(info + 8) = (u64)(uintptr_t)&luid;
    if (nvapi_GPU_GetLogicalGpuInfo(logical, info) != NVAPI_OK ||
        luid == 0xa5a5a5a5a5a5a5a5ull || *(u64 *)(info + 8) != (u64)(uintptr_t)&luid ||
        *(u32 *)(info + 16) != 1 || *(u64 *)(info + 24) != NVAPI_GPU_HANDLE ||
        *(u64 *)(info + 32) != 0)
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

/* Driver settings (DRS). The session holds no profiles: no application profile and no setting is found, which is
 * what a driver without a profile for the game reports. */
#define NVAPI_SETTING_NOT_FOUND ((u32)-160)
#define NVAPI_PROFILE_NOT_FOUND ((u32)-163)
#define NVAPI_EXECUTABLE_NOT_FOUND ((u32)-166)
#define NVAPI_DRS_SESSION 0x4E564453u        /* "NVDS" */
#define NVAPI_DRS_BASE_PROFILE 0x4E564450u   /* "NVDP" */

static u32 __attribute__((ms_abi)) nvapi_DRS_CreateSession(u64 *session)
{
    if (!session) return NVAPI_INVALID_ARGUMENT;
    *session = NVAPI_DRS_SESSION;
    return NVAPI_OK;
}
static u32 __attribute__((ms_abi)) nvapi_DRS_Session(u64 session)
{ return session == NVAPI_DRS_SESSION ? (u32)NVAPI_OK : (u32)NVAPI_INVALID_HANDLE; }   /* Destroy, Load, Save */
static u32 __attribute__((ms_abi)) nvapi_DRS_GetBaseProfile(u64 session, u64 *profile)
{
    if (session != NVAPI_DRS_SESSION) return NVAPI_INVALID_HANDLE;
    if (!profile) return NVAPI_INVALID_ARGUMENT;
    *profile = NVAPI_DRS_BASE_PROFILE;   /* also the current global profile */
    return NVAPI_OK;
}
static u32 __attribute__((ms_abi)) nvapi_DRS_FindApplicationByName(u64 session, const u16 *name, u64 *profile,
                                                                   void *application)
{
    if (session != NVAPI_DRS_SESSION) return NVAPI_INVALID_HANDLE;
    if (!name || !profile || !application) return NVAPI_INVALID_ARGUMENT;
    return NVAPI_EXECUTABLE_NOT_FOUND;
}
static u32 __attribute__((ms_abi)) nvapi_DRS_FindProfileByName(u64 session, const u16 *name, u64 *profile)
{
    if (session != NVAPI_DRS_SESSION) return NVAPI_INVALID_HANDLE;
    if (!name || !profile) return NVAPI_INVALID_ARGUMENT;
    return NVAPI_PROFILE_NOT_FOUND;
}
static u32 __attribute__((ms_abi)) nvapi_DRS_GetSetting(u64 session, u64 profile, u32 id, void *setting)
{
    (void)id;
    if (session != NVAPI_DRS_SESSION || !profile) return NVAPI_INVALID_HANDLE;
    if (!setting) return NVAPI_INVALID_ARGUMENT;
    return NVAPI_SETTING_NOT_FOUND;
}

/* NVIDIA Reflex: VK_NV_low_latency2 on the presenter's swapchain (renderer/vulkan/vulkan_presenter.c), as dxvk-nvapi
 * implements it under Proton. Offered only when the device has VK_NV_low_latency2 + VK_KHR_present_id; otherwise
 * NO_IMPLEMENTATION, so a game shows no Reflex option. Structs (NVAPI R555+): sizes from their version words. */
static int nvapi_reflex_ready(const void *device)
{
    if (!g_nvapi_initialized) return NVAPI_API_NOT_INITIALIZED;
    if (!device) return NVAPI_INVALID_ARGUMENT;
    /* The device capability decides: games and Streamline query Reflex before the first swapchain exists. */
    const BeerVulkanModern *m = beer_vulkan_modern();
    return m && m->low_latency ? NVAPI_OK : NVAPI_NO_IMPLEMENTATION;
}
/* BEER_REFLEX_LOG=1: every Reflex entry point, its parameters and its result (first 200 calls, then every 1000th). */
static int nvapi_reflex_log(const char *name, u64 a, u64 b, int result)
{
    static int log = -1;
    static unsigned n;
    if (log < 0) log = getenv("BEER_REFLEX_LOG") != NULL;
    if (log) {
        const unsigned i = __atomic_fetch_add(&n, 1, __ATOMIC_RELAXED);
        if (i < 200 || i % 1000 == 0)
            fprintf(stderr, "[REFLEX] #%u tid=%ld %s(%llx, %llx) -> %d\n", i, (long)syscall(SYS_gettid), name, (unsigned long long)a, (unsigned long long)b, result);
    }
    return result;
}
static int nvapi_struct_v1(const void *p, u32 min_size)
{
    const u32 v = p ? *(const u32 *)p : 0;
    return (v >> 16) == 1 && (v & 0xffffu) >= min_size;
}
/* NV_LATENCY_MARKER_TYPE -> VkLatencyMarkerNV: equal up to TRIGGER_FLASH; PC_LATENCY_PING (8) has no Vulkan marker;
 * the out-of-band markers (9..12) are 8..11 there. -1: drop. */
static int nvapi_reflex_marker(u32 type)
{
    if (type <= 7) return (int)type;
    if (type >= 9 && type <= 12) return (int)type - 1;
    return -1;
}
/* NV_SET_SLEEP_MODE_PARAMS: version@0, bLowLatencyMode@4, bLowLatencyBoost@5, minimumIntervalUs@8. */
static int nvapi_D3D_SetSleepMode_impl(void *device, const u8 *p)
{
    const int ready = nvapi_reflex_ready(device);
    if (ready != NVAPI_OK) return ready;
    if (!nvapi_struct_v1(p, 12)) return NVAPI_INCOMPATIBLE_STRUCT_VERSION;
    return beer_vulkan_low_latency_set_mode(p[4] != 0, p[5] != 0, *(const u32 *)(p + 8)) ? NVAPI_OK : NVAPI_ERROR;
}
/* NV_GET_SLEEP_STATUS_PARAMS: version@0, bLowLatencyMode@4, bFsVrr@5, bCplVsyncOn@6, sleepIntervalUs@8, ... */
static int nvapi_D3D_GetSleepStatus_impl(void *device, u8 *p)
{
    const int ready = nvapi_reflex_ready(device);
    if (ready != NVAPI_OK) return ready;
    if (!nvapi_struct_v1(p, 8)) return NVAPI_INCOMPATIBLE_STRUCT_VERSION;
    const u32 size = *(const u32 *)p & 0xffffu;
    memset(p + 4, 0, size - 4);
    int enable = 0;
    u32 interval = 0;
    beer_vulkan_low_latency_get_mode(&enable, NULL, &interval);
    p[4] = (u8)enable;
    if (size >= 12) *(u32 *)(p + 8) = interval;
    return NVAPI_OK;
}
static int nvapi_D3D_Sleep_impl(void *device)
{
    const int ready = nvapi_reflex_ready(device);
    if (ready != NVAPI_OK) return ready;
    return beer_vulkan_low_latency_sleep() ? NVAPI_OK : NVAPI_ERROR;
}
/* NV_LATENCY_MARKER_PARAMS: version@0, frameID@8, markerType@16. */
static int nvapi_D3D_SetLatencyMarker_impl(void *device, const u8 *p)
{
    const int ready = nvapi_reflex_ready(device);
    if (ready != NVAPI_OK) return ready;
    if (!nvapi_struct_v1(p, 20)) return NVAPI_INCOMPATIBLE_STRUCT_VERSION;
    const int marker = nvapi_reflex_marker(*(const u32 *)(p + 16));
    if (marker < 0) return NVAPI_OK;                       /* PC_LATENCY_PING: nothing to forward */
    beer_vulkan_low_latency_marker(*(const u64 *)(p + 8), marker);
    return NVAPI_OK;
}
/* NV_ASYNC_FRAME_MARKER_PARAMS (D3D12 queue): version@0, frameID@8, markerType@16, presentFrameID@24. */
static int nvapi_D3D12_SetAsyncFrameMarker_impl(void *queue, const u8 *p)
{
    const int ready = nvapi_reflex_ready(queue);
    if (ready != NVAPI_OK) return ready;
    if (!nvapi_struct_v1(p, 20)) return NVAPI_INCOMPATIBLE_STRUCT_VERSION;
    const int marker = nvapi_reflex_marker(*(const u32 *)(p + 16));
    if (marker < 0) return NVAPI_OK;
    beer_vulkan_low_latency_marker(*(const u64 *)(p + 8), marker);
    return NVAPI_OK;
}
/* One D3D12 queue type is "out of band" (frame generation's present work). Beer runs every D3D12 queue on one Vulkan
 * queue, so flagging that queue would mislabel the game's own rendering: accepted, nothing to change. */
static int nvapi_D3D12_NotifyOutOfBandCommandQueue_impl(void *queue, u32 type)
{
    const int ready = nvapi_reflex_ready(queue);
    if (ready != NVAPI_OK) return ready;
    return type <= 4 ? NVAPI_OK : NVAPI_INVALID_ARGUMENT;
}
/* NV_LATENCY_RESULT_PARAMS: version@0, frameReport[64]@8 (240 bytes each: 14 u64 timestamps in microseconds,
 * gpuActiveRenderTimeUs@112, gpuFrameTimeUs@116, ...), oldest first. */
static int nvapi_D3D_GetLatency_impl(void *device, u8 *p)
{
    const int ready = nvapi_reflex_ready(device);
    if (ready != NVAPI_OK) return ready;
    if (!nvapi_struct_v1(p, 8 + 64 * 240)) return NVAPI_INCOMPATIBLE_STRUCT_VERSION;
    memset(p + 8, 0, 64 * 240);
    static VkLatencyTimingsFrameReportNV reports[64];
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    const u32 count = beer_vulkan_low_latency_timings(reports, 64);
    const u32 first = count > 64 ? count - 64 : 0;
    u64 previous_gpu_end = 0;
    for (u32 i = first, slot = 0; i < count && slot < 64; ++i, ++slot) {
        const VkLatencyTimingsFrameReportNV *r = &reports[i];
        if (!r->presentID) continue;                       /* no frame behind this entry yet */
        u8 *f = p + 8 + (size_t)slot * 240;
        const u64 v[14] = { r->presentID, r->inputSampleTimeUs, r->simStartTimeUs, r->simEndTimeUs,
                            r->renderSubmitStartTimeUs, r->renderSubmitEndTimeUs, r->presentStartTimeUs,
                            r->presentEndTimeUs, r->driverStartTimeUs, r->driverEndTimeUs,
                            r->osRenderQueueStartTimeUs, r->osRenderQueueEndTimeUs, r->gpuRenderStartTimeUs,
                            r->gpuRenderEndTimeUs };
        memcpy(f, v, sizeof(v));
        const u32 active = r->gpuRenderEndTimeUs > r->gpuRenderStartTimeUs
                               ? (u32)(r->gpuRenderEndTimeUs - r->gpuRenderStartTimeUs) : 0;
        const u32 frame = previous_gpu_end && r->gpuRenderEndTimeUs > previous_gpu_end
                              ? (u32)(r->gpuRenderEndTimeUs - previous_gpu_end) : 0;
        memcpy(f + 112, &active, 4);
        memcpy(f + 116, &frame, 4);
        previous_gpu_end = r->gpuRenderEndTimeUs;
    }
    pthread_mutex_unlock(&lock);
    return NVAPI_OK;
}

static int __attribute__((ms_abi)) nvapi_D3D_SetSleepMode(void *device, const u8 *p)
{
    return nvapi_reflex_log("D3D_SetSleepMode", p ? *(const u32 *)(p + 4) : 0, p ? *(const u32 *)(p + 8) : 0, nvapi_D3D_SetSleepMode_impl(device, p));
}
static int __attribute__((ms_abi)) nvapi_D3D_GetSleepStatus(void *device, u8 *p)
{
    return nvapi_reflex_log("D3D_GetSleepStatus", (u64)(uintptr_t)device, 0, nvapi_D3D_GetSleepStatus_impl(device, p));
}
static int __attribute__((ms_abi)) nvapi_D3D_Sleep(void *device)
{
    return nvapi_reflex_log("D3D_Sleep", (u64)(uintptr_t)device, 0, nvapi_D3D_Sleep_impl(device));
}
static int __attribute__((ms_abi)) nvapi_D3D_SetLatencyMarker(void *device, const u8 *p)
{
    return nvapi_reflex_log("D3D_SetLatencyMarker", p ? *(const u64 *)(p + 8) : 0, p ? *(const u32 *)(p + 16) : 0, nvapi_D3D_SetLatencyMarker_impl(device, p));
}
static int __attribute__((ms_abi)) nvapi_D3D12_SetAsyncFrameMarker(void *queue, const u8 *p)
{
    return nvapi_reflex_log("D3D12_SetAsyncFrameMarker", p ? *(const u64 *)(p + 8) : 0, p ? *(const u32 *)(p + 16) : 0, nvapi_D3D12_SetAsyncFrameMarker_impl(queue, p));
}
static int __attribute__((ms_abi)) nvapi_D3D12_NotifyOutOfBandCommandQueue(void *queue, u32 type)
{
    return nvapi_reflex_log("D3D12_NotifyOutOfBandCommandQueue", (u64)(uintptr_t)queue, type, nvapi_D3D12_NotifyOutOfBandCommandQueue_impl(queue, type));
}
static int __attribute__((ms_abi)) nvapi_D3D_GetLatency(void *device, u8 *p)
{
    return nvapi_reflex_log("D3D_GetLatency", (u64)(uintptr_t)device, 0, nvapi_D3D_GetLatency_impl(device, p));
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
        /* D3D12 CUDA interop, only with the NVX Vulkan extensions (NGX checks for these before using DLSS). */
        { 0x2A2C79E8u, nvapi_D3D12_CreateCubinComputeShader },
        { 0x1DC7261Fu, nvapi_D3D12_CreateCubinComputeShaderWithName },
        { 0x3151211Bu, nvapi_D3D12_CreateCubinComputeShaderEx },
        { 0x299F5FDCu, nvapi_D3D12_CreateCubinComputeShaderExV2 },
        { 0x7FB785BAu, nvapi_D3D12_DestroyCubinComputeShader },
        { 0x5C52BB86u, nvapi_D3D12_LaunchCubinShader },
        { 0x80403FC9u, nvapi_D3D12_GetCudaTextureObject },
        { 0x48F5B2EEu, nvapi_D3D12_GetCudaSurfaceObject },
        { 0x329FE6E0u, nvapi_D3D12_GetCudaMergedTextureSamplerObject },
        { 0x0DDAC234u, nvapi_D3D12_GetCudaIndependentDescriptorObject },
        { 0x6E5EA9DBu, nvapi_D3D12_CaptureUAVInfo },
        { 0xAC1CA9E0u, nvapi_D3D_SetSleepMode },
        { 0xAEF96CA1u, nvapi_D3D_GetSleepStatus },
        { 0x852CD1D2u, nvapi_D3D_Sleep },
        { 0xD9984C05u, nvapi_D3D_SetLatencyMarker },
        { 0x1A587F9Cu, nvapi_D3D_GetLatency },
        { 0x13C98F73u, nvapi_D3D12_SetAsyncFrameMarker },
        { 0x03D6E8CBu, nvapi_D3D12_NotifyOutOfBandCommandQueue },
        { 0x70C07832u, nvapi_D3D12_IsFatbinPTXSupported },
        { 0x01E87354u, nvapi_D3D12_GetGraphicsCapabilities },
        { 0x0694D52Eu, nvapi_DRS_CreateSession },
        { 0xDAD9CFF8u, nvapi_DRS_Session },                 /* DestroySession */
        { 0x375DBD6Bu, nvapi_DRS_Session },                 /* LoadSettings */
        { 0xFCBC7E14u, nvapi_DRS_Session },                 /* SaveSettings */
        { 0xDA8466A0u, nvapi_DRS_GetBaseProfile },
        { 0x617BFF9Fu, nvapi_DRS_GetBaseProfile },          /* GetCurrentGlobalProfile */
        { 0xEEE566B2u, nvapi_DRS_FindApplicationByName },
        { 0x7E4A9A0Bu, nvapi_DRS_FindProfileByName },
        { 0x73BF8338u, nvapi_DRS_GetSetting },
    };
    /* The CUDA entries exist only when the device can run CUDA kernels. */
    const int cuda = bd12_cuda_available();
    for (size_t i = 0; i < sizeof(table) / sizeof(*table); ++i)
        if (table[i].id == id) {
            const int cuda_entry = table[i].fn == (void *)nvapi_D3D12_CreateCubinComputeShader ||
                table[i].fn == (void *)nvapi_D3D12_CreateCubinComputeShaderWithName ||
                table[i].fn == (void *)nvapi_D3D12_CreateCubinComputeShaderEx ||
                table[i].fn == (void *)nvapi_D3D12_CreateCubinComputeShaderExV2 ||
                table[i].fn == (void *)nvapi_D3D12_DestroyCubinComputeShader ||
                table[i].fn == (void *)nvapi_D3D12_LaunchCubinShader ||
                table[i].fn == (void *)nvapi_D3D12_GetCudaTextureObject ||
                table[i].fn == (void *)nvapi_D3D12_GetCudaSurfaceObject ||
                table[i].fn == (void *)nvapi_D3D12_GetCudaMergedTextureSamplerObject ||
                table[i].fn == (void *)nvapi_D3D12_GetCudaIndependentDescriptorObject ||
                table[i].fn == (void *)nvapi_D3D12_CaptureUAVInfo ||
                table[i].fn == (void *)nvapi_D3D12_IsFatbinPTXSupported;
            if (!cuda && cuda_entry) break;
            if (getenv("BEER_NVAPI_LOG")) fprintf(stderr, "[NVAPI] QueryInterface(0x%08X)\n", id);
            /* The game calls these pointers directly. This file is built without SSE, so its ms_abi functions do not
             * keep the nonvolatile XMM6-XMM15 across the SSE code they reach (presenter, Vulkan driver): the Reflex
             * sleep cleared RE9's frame-limiter divisor in XMM6 and the game spun in its limiter forever. Hand out
             * dispatch-trampoline entries, which save XMM6-XMM15 like an import call. */
            return (void *)(uintptr_t)beer_guest_com_entry((u64)(uintptr_t)table[i].fn, "nvapi", id);
        }
    static _Atomic(u32) logged;
    if (atomic_fetch_add(&logged, 1) < 64)
        fprintf(stderr, "[NVAPI] interface 0x%08X is not implemented (called from %p)\n", id, __builtin_return_address(0));
    return NULL;
}
