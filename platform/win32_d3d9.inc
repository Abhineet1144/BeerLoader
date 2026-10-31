/* Minimal Direct3D 9Ex runtime: the adapter-query half of IDirect3D9Ex.
 *
 * Several engines create a Direct3D 9Ex object next to their real renderer only to read the adapter identity,
 * display modes and format support, and refuse to start when Direct3DCreate9Ex fails.  Beer renders through
 * Vulkan, so this object describes the Vulkan device to those probes; CreateDevice(Ex) reports that no D3D9
 * device can be created.  Every unimplemented slot logs once and answers E_NOTIMPL. */

#define D3D9_OK 0u
#define D3D9ERR_NOTAVAILABLE 0x8876086Au
#define D3D9ERR_INVALIDCALL 0x8876086Cu
#define D3D9_FMT_X8R8G8B8 22u

typedef struct { u64 *vtable; u32 refs; } BeerD3D9Object;
static u64 g_d3d9_vtable[24];
static BeerD3D9Object g_d3d9_object = { g_d3d9_vtable, 1 };

static u64 __attribute__((ms_abi)) d3d9_QueryInterface(BeerD3D9Object *self, const u8 *riid, void **out)
{
    (void)riid;
    if (!out) return 0x80004003u;            /* E_POINTER */
    *out = self;                             /* IDirect3D9 and IDirect3D9Ex share this object */
    self->refs++;
    return D3D9_OK;
}
static u64 __attribute__((ms_abi)) d3d9_AddRef(BeerD3D9Object *self) { return ++self->refs; }
static u64 __attribute__((ms_abi)) d3d9_Release(BeerD3D9Object *self) { return self->refs > 1 ? --self->refs : 1; }
static u64 __attribute__((ms_abi)) d3d9_GetAdapterCount(BeerD3D9Object *self) { (void)self; return 1; }

static u64 __attribute__((ms_abi)) d3d9_GetAdapterIdentifier(BeerD3D9Object *self, u32 adapter, u32 flags, u8 *id)
{
    (void)self; (void)flags;
    if (adapter != 0 || !id) return D3D9ERR_INVALIDCALL;
    BeerHostGpuInfo gpu;
    const int have = beer_vulkan_host_gpu_info(&gpu);
    memset(id, 0, 1104);                                       /* sizeof(D3DADAPTER_IDENTIFIER9) */
    snprintf((char *)id + 0, 512, "beer_vulkan.dll");          /* Driver */
    snprintf((char *)id + 512, 512, "%s", have ? gpu.name : "Beer Virtual Adapter");   /* Description */
    snprintf((char *)id + 1024, 32, "\\\\.\\DISPLAY1");        /* DeviceName */
    *(u64 *)(id + 1056) = 0x000A000F00000001ULL;               /* DriverVersion */
    *(u32 *)(id + 1064) = have ? gpu.vendor_id : 0x1414;       /* VendorId */
    *(u32 *)(id + 1068) = have ? gpu.device_id : 1;            /* DeviceId */
    *(u32 *)(id + 1072) = 0;                                   /* SubSysId */
    *(u32 *)(id + 1076) = have ? gpu.revision : 0;             /* Revision */
    memcpy(id + 1080, &(u64){have ? gpu.luid : 1}, 8);         /* DeviceIdentifier (GUID) */
    *(u32 *)(id + 1096) = 0;                                   /* WHQLLevel */
    return D3D9_OK;
}

static void d3d9_fill_mode(u8 *mode, const DxgiHostMode *m)
{
    *(u32 *)(mode + 0) = m->width; *(u32 *)(mode + 4) = m->height;
    *(u32 *)(mode + 8) = m->refresh; *(u32 *)(mode + 12) = D3D9_FMT_X8R8G8B8;
}
static u64 __attribute__((ms_abi)) d3d9_GetAdapterModeCount(BeerD3D9Object *self, u32 adapter, u32 format)
{
    (void)self;
    DxgiHostMode modes[64];
    return adapter == 0 && format == D3D9_FMT_X8R8G8B8 ? dxgi_host_modes(modes, 64) : 0;
}
static u64 __attribute__((ms_abi)) d3d9_EnumAdapterModes(BeerD3D9Object *self, u32 adapter, u32 format, u32 index, u8 *mode)
{
    (void)self;
    DxgiHostMode modes[64];
    u32 total = dxgi_host_modes(modes, 64);
    if (adapter != 0 || format != D3D9_FMT_X8R8G8B8 || index >= total || !mode) return D3D9ERR_INVALIDCALL;
    d3d9_fill_mode(mode, &modes[index]);
    return D3D9_OK;
}
static u64 __attribute__((ms_abi)) d3d9_GetAdapterDisplayMode(BeerD3D9Object *self, u32 adapter, u8 *mode)
{
    (void)self;
    if (adapter != 0 || !mode) return D3D9ERR_INVALIDCALL;
    int w = 1920, h = 1080;
    xwayland_get_screen_size(&w, &h);
    DxgiHostMode native = { (u32)w, (u32)h, (u32)xwayland_get_screen_refresh() };
    d3d9_fill_mode(mode, &native);
    return D3D9_OK;
}
/* CheckDeviceType / CheckDeviceFormat / CheckDeviceMultiSampleType / CheckDepthStencilMatch / ConversionCheck:
 * the probes only ask whether common formats exist; they do. */
static u64 __attribute__((ms_abi)) d3d9_CheckOk(void) { return D3D9_OK; }
static u64 __attribute__((ms_abi)) d3d9_GetDeviceCaps(BeerD3D9Object *self, u32 adapter, u32 type, u8 *caps)
{
    (void)self; (void)type;
    if (adapter != 0 || !caps) return D3D9ERR_INVALIDCALL;
    memset(caps, 0, 304);                                      /* sizeof(D3DCAPS9) */
    *(u32 *)(caps + 0) = 1;                                    /* D3DDEVTYPE_HAL */
    return D3D9_OK;
}
/* IDirect3D9Ex mode enumeration: the filter picks the format; the answer is the same host mode list. */
static u64 __attribute__((ms_abi)) d3d9_GetAdapterModeCountEx(BeerD3D9Object *self, u32 adapter, const void *filter)
{
    (void)self; (void)filter;
    DxgiHostMode modes[64];
    return adapter == 0 ? dxgi_host_modes(modes, 64) : 0;
}
static void d3d9_fill_mode_ex(u8 *mode, const DxgiHostMode *m)
{
    *(u32 *)(mode + 0) = 24;                                   /* Size */
    *(u32 *)(mode + 4) = m->width; *(u32 *)(mode + 8) = m->height;
    *(u32 *)(mode + 12) = m->refresh; *(u32 *)(mode + 16) = D3D9_FMT_X8R8G8B8;
    *(u32 *)(mode + 20) = 0;                                   /* progressive */
}
static u64 __attribute__((ms_abi)) d3d9_EnumAdapterModesEx(BeerD3D9Object *self, u32 adapter, const void *filter, u32 index, u8 *mode)
{
    (void)self; (void)filter;
    DxgiHostMode modes[64];
    u32 total = dxgi_host_modes(modes, 64);
    if (adapter != 0 || index >= total || !mode) return D3D9ERR_INVALIDCALL;
    d3d9_fill_mode_ex(mode, &modes[index]);
    return D3D9_OK;
}
static u64 __attribute__((ms_abi)) d3d9_GetAdapterDisplayModeEx(BeerD3D9Object *self, u32 adapter, u8 *mode, u32 *rotation)
{
    (void)self;
    if (adapter != 0 || !mode) return D3D9ERR_INVALIDCALL;
    int w = 1920, h = 1080;
    xwayland_get_screen_size(&w, &h);
    DxgiHostMode native = { (u32)w, (u32)h, (u32)xwayland_get_screen_refresh() };
    d3d9_fill_mode_ex(mode, &native);
    if (rotation) *rotation = 1;                               /* D3DDISPLAYROTATION_IDENTITY */
    return D3D9_OK;
}
static u64 __attribute__((ms_abi)) d3d9_GetAdapterMonitor(BeerD3D9Object *self, u32 adapter) { (void)self; (void)adapter; return 1; }
static u64 __attribute__((ms_abi)) d3d9_CreateDevice(void) { return D3D9ERR_NOTAVAILABLE; }
static u64 __attribute__((ms_abi)) d3d9_GetAdapterLUID(BeerD3D9Object *self, u32 adapter, u64 *luid)
{
    (void)self;
    if (adapter != 0 || !luid) return D3D9ERR_INVALIDCALL;
    BeerHostGpuInfo gpu;
    *luid = beer_vulkan_host_gpu_info(&gpu) ? gpu.luid : 1;
    return D3D9_OK;
}
static u64 __attribute__((ms_abi)) d3d9_NotImplemented(void)
{
    static _Atomic(u32) logs;
    if (atomic_fetch_add(&logs, 1) < 8)
        fprintf(stderr, "[D3D9] unimplemented IDirect3D9Ex method called\n");
    return 0x80004001u;                                        /* E_NOTIMPL */
}

static void d3d9_init_vtable(void)
{
    if (g_d3d9_vtable[0]) return;
    for (size_t i = 0; i < sizeof(g_d3d9_vtable) / sizeof(*g_d3d9_vtable); ++i)
        g_d3d9_vtable[i] = (u64)d3d9_NotImplemented;
    g_d3d9_vtable[0] = (u64)d3d9_QueryInterface;
    g_d3d9_vtable[1] = (u64)d3d9_AddRef;
    g_d3d9_vtable[2] = (u64)d3d9_Release;
    g_d3d9_vtable[4] = (u64)d3d9_GetAdapterCount;
    g_d3d9_vtable[5] = (u64)d3d9_GetAdapterIdentifier;
    g_d3d9_vtable[6] = (u64)d3d9_GetAdapterModeCount;
    g_d3d9_vtable[7] = (u64)d3d9_EnumAdapterModes;
    g_d3d9_vtable[8] = (u64)d3d9_GetAdapterDisplayMode;
    g_d3d9_vtable[9] = (u64)d3d9_CheckOk;                      /* CheckDeviceType */
    g_d3d9_vtable[10] = (u64)d3d9_CheckOk;                     /* CheckDeviceFormat */
    g_d3d9_vtable[11] = (u64)d3d9_CheckOk;                     /* CheckDeviceMultiSampleType */
    g_d3d9_vtable[12] = (u64)d3d9_CheckOk;                     /* CheckDepthStencilMatch */
    g_d3d9_vtable[13] = (u64)d3d9_CheckOk;                     /* CheckDeviceFormatConversion */
    g_d3d9_vtable[14] = (u64)d3d9_GetDeviceCaps;
    g_d3d9_vtable[15] = (u64)d3d9_GetAdapterMonitor;
    g_d3d9_vtable[16] = (u64)d3d9_CreateDevice;
    g_d3d9_vtable[17] = (u64)d3d9_GetAdapterModeCountEx;
    g_d3d9_vtable[18] = (u64)d3d9_EnumAdapterModesEx;
    g_d3d9_vtable[19] = (u64)d3d9_GetAdapterDisplayModeEx;
    g_d3d9_vtable[20] = (u64)d3d9_CreateDevice;                /* CreateDeviceEx */
    g_d3d9_vtable[21] = (u64)d3d9_GetAdapterLUID;
}

static u64 __attribute__((ms_abi)) impl_Direct3DCreate9(u32 sdk_version)
{
    (void)sdk_version;
    d3d9_init_vtable();
    return (u64)(uintptr_t)&g_d3d9_object;
}
static u64 __attribute__((ms_abi)) impl_Direct3DCreate9Ex(u32 sdk_version, u64 *out)
{
    (void)sdk_version;
    if (!out) return D3D9ERR_INVALIDCALL;
    d3d9_init_vtable();
    *out = (u64)(uintptr_t)&g_d3d9_object;
    return D3D9_OK;
}
