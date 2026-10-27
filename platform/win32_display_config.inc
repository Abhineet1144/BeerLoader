/* Win32 display configuration (QueryDisplayConfig family): one active path (\\.\DISPLAY1 on the host GPU, one
 * monitor at the host's native mode).  Engines that enumerate monitors through the CCD API read these structures
 * back with fixed layouts and retry on empty answers, so every field they look at is filled. */

#define CCD_SUCCESS 0u
#define CCD_INVALID_PARAMETER 87u
#define CCD_INSUFFICIENT_BUFFER 122u
#define CCD_OUTPUT_DISPLAYPORT_EXTERNAL 10u

static u64 ccd_adapter_luid(void)
{
    BeerHostGpuInfo gpu;
    return beer_vulkan_host_gpu_info(&gpu) ? gpu.luid : 1;
}

static void ccd_native_mode(u32 *width, u32 *height, u32 *hz)
{
    int w = 1920, h = 1080;
    xwayland_get_screen_size(&w, &h);
    *width = (u32)w;
    *height = (u32)h;
    *hz = (u32)xwayland_get_screen_refresh();
    if (*hz == 0) *hz = 60;
}

static void ccd_text(u8 *dest, size_t chars, const char *text)
{
    u16 *wide = (u16 *)dest;
    size_t i = 0;
    for (; text[i] && i + 1 < chars; ++i) wide[i] = (u8)text[i];
    wide[i] = 0;
}

/* DISPLAYCONFIG_VIDEO_SIGNAL_INFO (48 bytes) */
static void ccd_signal_info(u8 *p, u32 width, u32 height, u32 hz)
{
    const u32 total_w = width + width / 8, total_h = height + height / 24;
    memset(p, 0, 48);
    *(u64 *)(p + 0) = (u64)total_w * total_h * hz;                          /* pixelRate */
    *(u32 *)(p + 8) = total_h * hz; *(u32 *)(p + 12) = 1;                   /* hSyncFreq */
    *(u32 *)(p + 16) = hz * 1000; *(u32 *)(p + 20) = 1000;                  /* vSyncFreq */
    *(u32 *)(p + 24) = width; *(u32 *)(p + 28) = height;                    /* activeSize */
    *(u32 *)(p + 32) = total_w; *(u32 *)(p + 36) = total_h;                 /* totalSize */
    *(u32 *)(p + 40) = 255;                                                 /* videoStandard: other */
    *(u32 *)(p + 44) = 1;                                                   /* progressive */
}

static u64 __attribute__((ms_abi)) impl_GetDisplayConfigBufferSizes(u32 flags, u32 *paths, u32 *modes)
{
    (void)flags;
    if (!paths || !modes) return CCD_INVALID_PARAMETER;
    *paths = 1;
    *modes = 2;
    return CCD_SUCCESS;
}

static u64 __attribute__((ms_abi)) impl_QueryDisplayConfig(u32 flags, u32 *path_count, u8 *paths, u32 *mode_count, u8 *modes,
                                                           u32 *topology)
{
    (void)flags;
    if (!path_count || !paths || !mode_count || !modes) return CCD_INVALID_PARAMETER;
    if (*path_count < 1 || *mode_count < 2) return CCD_INSUFFICIENT_BUFFER;
    u32 w, h, hz;
    ccd_native_mode(&w, &h, &hz);
    const u64 luid = ccd_adapter_luid();

    memset(paths, 0, 72);                                                  /* DISPLAYCONFIG_PATH_INFO */
    memcpy(paths + 0, &luid, 8);                                           /* sourceInfo.adapterId */
    *(u32 *)(paths + 12) = 0;                                              /* modeInfoIdx: plain index of the source mode (no virtual-mode packing) */
    *(u32 *)(paths + 16) = 1;                                              /* SOURCE_IN_USE */
    memcpy(paths + 20, &luid, 8);                                          /* targetInfo.adapterId */
    *(u32 *)(paths + 32) = 1;                                              /* modeInfoIdx */
    *(u32 *)(paths + 36) = CCD_OUTPUT_DISPLAYPORT_EXTERNAL;
    *(u32 *)(paths + 40) = 1;                                              /* rotation: identity */
    *(u32 *)(paths + 44) = 1;                                              /* scaling: identity */
    *(u32 *)(paths + 48) = hz * 1000; *(u32 *)(paths + 52) = 1000;         /* refreshRate */
    *(u32 *)(paths + 56) = 1;                                              /* progressive */
    *(u32 *)(paths + 60) = 1;                                              /* targetAvailable */
    *(u32 *)(paths + 64) = 1;                                              /* TARGET_IN_USE */
    *(u32 *)(paths + 68) = 1;                                              /* PATH_ACTIVE */

    memset(modes, 0, 128);                                                 /* two DISPLAYCONFIG_MODE_INFO */
    *(u32 *)(modes + 0) = 1;                                               /* source mode */
    memcpy(modes + 8, &luid, 8);
    *(u32 *)(modes + 16) = w; *(u32 *)(modes + 20) = h;
    *(u32 *)(modes + 24) = 4;                                              /* 32 bits per pixel */
    u8 *target = modes + 64;
    *(u32 *)(target + 0) = 2;                                              /* target mode */
    memcpy(target + 8, &luid, 8);
    ccd_signal_info(target + 16, w, h, hz);

    *path_count = 1;
    *mode_count = 2;
    if (topology) *topology = 1;                                           /* internal */
    return CCD_SUCCESS;
}

static u64 __attribute__((ms_abi)) impl_DisplayConfigGetDeviceInfo(u8 *packet)
{
    if (!packet) return CCD_INVALID_PARAMETER;
    const u32 type = *(u32 *)packet, size = *(u32 *)(packet + 4);
    u32 w, h, hz;
    ccd_native_mode(&w, &h, &hz);
    switch (type) {
    case 1:                                                                /* GET_SOURCE_NAME */
        if (size < 84) return CCD_INVALID_PARAMETER;
        ccd_text(packet + 20, 32, "\\\\.\\DISPLAY1");
        return CCD_SUCCESS;
    case 2:                                                                /* GET_TARGET_NAME */
        if (size < 420) return CCD_INVALID_PARAMETER;
        memset(packet + 20, 0, 400);
        *(u32 *)(packet + 24) = CCD_OUTPUT_DISPLAYPORT_EXTERNAL;
        *(u16 *)(packet + 28) = 0xBEE0;                                    /* EDID manufacturer */
        *(u16 *)(packet + 30) = 0x0001;                                    /* EDID product code */
        ccd_text(packet + 36, 64, "Generic PnP Monitor");
        ccd_text(packet + 164, 128, "\\\\?\\DISPLAY#BEER0000#4&0&UID0#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}");
        return CCD_SUCCESS;
    case 3:                                                                /* GET_TARGET_PREFERRED_MODE */
        if (size < 76) return CCD_INVALID_PARAMETER;
        *(u32 *)(packet + 20) = w; *(u32 *)(packet + 24) = h;
        ccd_signal_info(packet + 28, w, h, hz);
        return CCD_SUCCESS;
    case 4: {                                                              /* GET_ADAPTER_NAME */
        if (size < 276) return CCD_INVALID_PARAMETER;
        BeerHostGpuInfo gpu;
        const int have = beer_vulkan_host_gpu_info(&gpu);
        char path[200];
        snprintf(path, sizeof(path), "\\\\?\\PCI#VEN_%04X&DEV_%04X&SUBSYS_00000000&REV_%02X#4&0&0&0008#{5b45201d-f2f2-4f3b-85bb-30ff1f953599}",
                 have ? gpu.vendor_id : 0x1414u, have ? gpu.device_id : 1u, have ? gpu.revision : 0u);
        memset(packet + 20, 0, 256);
        ccd_text(packet + 20, 128, path);
        return CCD_SUCCESS;
    }
    case 6:                                                                /* GET_TARGET_BASE_TYPE */
        if (size < 24) return CCD_INVALID_PARAMETER;
        *(u32 *)(packet + 20) = CCD_OUTPUT_DISPLAYPORT_EXTERNAL;
        return CCD_SUCCESS;
    case 9:                                                                /* GET_ADVANCED_COLOR_INFO: SDR only */
        if (size < 32) return CCD_INVALID_PARAMETER;
        memset(packet + 20, 0, 12);
        *(u32 *)(packet + 24) = 1;                                         /* DISPLAYCONFIG_COLOR_ENCODING_RGB */
        *(u32 *)(packet + 28) = 8;
        return CCD_SUCCESS;
    case 11:                                                               /* GET_SDR_WHITE_LEVEL: 80 nits */
        if (size < 24) return CCD_INVALID_PARAMETER;
        *(u32 *)(packet + 20) = 1000;
        return CCD_SUCCESS;
    default:
        return CCD_INVALID_PARAMETER;
    }
}

static u64 __attribute__((ms_abi)) impl_DisplayConfigSetDeviceInfo(u8 *packet)
{
    (void)packet;
    return 50;                                                             /* ERROR_NOT_SUPPORTED */
}

/* shcore GetDpiForMonitor: the desktop runs at 96 dpi. */
static u64 __attribute__((ms_abi)) impl_GetDpiForMonitor(u64 monitor, u32 type, u32 *x, u32 *y)
{
    (void)monitor; (void)type;
    if (!x || !y) return 0x80070057u;
    *x = *y = 96;
    return 0;
}
