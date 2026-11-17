/* DLSS overrides on NVIDIA's NGX core (_nvngx.dll / nvngx.dll, the driver's Windows-side DLL).
 *
 * Every DLSS game, direct NGX or through Streamline (sl.dlss), resolves the NGX core entry points with
 * GetProcAddress. Beer wraps a few of them to change two things the game cannot be told otherwise:
 *
 *   - The render (internal) resolution. Games ask DLSS which size to render at for a quality mode through the
 *     optimal-settings callback stored in the NGX parameter block ("DLSSOptimalSettingsCallback") and then render at
 *     that size. Beer replaces the callback with one that calls the real one and then reports its own size:
 *       BEER_DLSS_RATIO=<percent>   render at this share of the output on each axis (e.g. 50, 66.7, 77, 100 = DLAA)
 *       BEER_DLSS_RENDER=<W>x<H>    render at exactly this size
 *     The game's DLSS quality mode is left as the game chose it; only the size changes.
 *   - The DLSS model ("render preset"): BEER_DLSS_PRESET=<letter> (A.., e.g. J, K) or 0 for the DLL's default sets
 *     the preset hint of every quality mode when the feature is created.
 *
 * The NGX parameter block is a C++ object (NVSDK_NGX_Parameter) built by MSVC. MSVC lays overloaded virtual
 * functions out in reverse declaration order, giving these slots (checked against the driver's _nvngx.dll, whose
 * Get functions also convert between the stored and the requested type):
 *   Set: 0 void*, 1 ID3D12Resource*, 2 ID3D11Resource*, 3 int, 4 unsigned int, 5 double, 6 float, 7 unsigned long long
 *   Get: 8 void**, 9 ID3D12Resource**, 10 ID3D11Resource**, 11 int*, 12 unsigned int*, 13 double*, 14 float*, 15 ull*
 *   16 Reset.
 * This file is built without SSE, so only the integer and pointer slots are used. */

int bd12_debug_stretch(void *list_iface, void *source_iface, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                       void *target_iface);   /* graphics/d3d12v/bd12_com_list.c */

#define NGX_RESULT_SUCCESS 1u
#define NGX_FEATURE_SUPER_SAMPLING 1u

typedef void __attribute__((ms_abi)) (*NgxSetPtr)(u64 self, const char *name, u64 value);
typedef void __attribute__((ms_abi)) (*NgxSetU32)(u64 self, const char *name, u32 value);
typedef u32 __attribute__((ms_abi)) (*NgxGetPtr)(u64 self, const char *name, u64 *value);
typedef u32 __attribute__((ms_abi)) (*NgxGetU32)(u64 self, const char *name, u32 *value);
typedef u32 __attribute__((ms_abi)) (*NgxFn1)(u64);
typedef u32 __attribute__((ms_abi)) (*NgxFn4)(u64, u64, u64, u64);

enum { NGX_SET_PTR = 0, NGX_SET_U32 = 4, NGX_GET_PTR = 8, NGX_GET_D3D12_RESOURCE = 9, NGX_GET_U32 = 12 };

static u64 ngx_slot(u64 params, int slot)
{
    const u64 *vtable = *(u64 **)(uintptr_t)params;
    return vtable[slot];
}
static int ngx_get_u32(u64 params, const char *name, u32 *value)
{
    return ((NgxGetU32)(uintptr_t)ngx_slot(params, NGX_GET_U32))(params, name, value) == NGX_RESULT_SUCCESS;
}
static void ngx_set_u32(u64 params, const char *name, u32 value)
{
    ((NgxSetU32)(uintptr_t)ngx_slot(params, NGX_SET_U32))(params, name, value);
}
static int ngx_get_ptr(u64 params, const char *name, u64 *value)
{
    return ((NgxGetPtr)(uintptr_t)ngx_slot(params, NGX_GET_PTR))(params, name, value) == NGX_RESULT_SUCCESS;
}
static u64 ngx_get_resource(u64 params, const char *name)
{
    u64 value = 0;
    if (((NgxGetPtr)(uintptr_t)ngx_slot(params, NGX_GET_D3D12_RESOURCE))(params, name, &value) != NGX_RESULT_SUCCESS)
        return 0;
    return value;
}
static void ngx_set_ptr(u64 params, const char *name, u64 value)
{
    ((NgxSetPtr)(uintptr_t)ngx_slot(params, NGX_SET_PTR))(params, name, value);
}

/* The live override (platform/beer_control.c: dlss_ratio / dlss_width / dlss_height / dlss_preset, seeded from the
 * BEER_DLSS_* variables, changed by the manager while the game runs). Read on every query, so a change applies the
 * next time the game asks DLSS for its render size (switching the game's DLSS mode or resolution) or creates DLSS. */
typedef struct { u32 ratio, render_w, render_h; int preset; } NgxConfig;   /* ratio in permille, 0 = off */
static NgxConfig ngx_config(void)
{
    int32_t ratio = 0, w = 0, h = 0, preset = -1;
    beer_control_dlss(&ratio, &w, &h, &preset);
    NgxConfig c = { (u32)(ratio > 0 ? ratio / 100 : 0), 0, 0, preset };
    if (w >= 64 && h >= 64) { c.render_w = (u32)w; c.render_h = (u32)h; }
    if (c.ratio && c.ratio < 100) c.ratio = 100;
    if (c.ratio > 1000) c.ratio = 1000;
    return c;
}
static void ngx_read_config(void)
{
    static int logged;
    if (logged) return;
    logged = 1;
    const NgxConfig c = ngx_config();
    fprintf(stderr, "[NGX] DLSS override at start: ratio=%u.%u%% render=%ux%u preset=%d (changeable live)\n",
            c.ratio / 10, c.ratio % 10, c.render_w, c.render_h, c.preset);
}
static int ngx_size_override(void) { const NgxConfig c = ngx_config(); return c.ratio || c.render_w; }

/* The render size for an output size; 0 when no size override is set. Very small shares are allowed (down to 10%);
 * DLSS itself may refuse a size below what its Ultra Performance mode supports (the create then fails, logged). */
static int ngx_render_size(u32 out_w, u32 out_h, u32 *w, u32 *h)
{
    const NgxConfig c = ngx_config();
    if ((!c.ratio && !c.render_w) || !out_w || !out_h) return 0;
    if (c.render_w) {
        *w = c.render_w < out_w ? c.render_w : out_w;
        *h = c.render_h < out_h ? c.render_h : out_h;
    } else {
        *w = (u32)(((u64)out_w * c.ratio + 500) / 1000);
        *h = (u32)(((u64)out_h * c.ratio + 500) / 1000);
    }
    *w = (*w + 1) & ~1u;                              /* even sizes, as DLSS modes report */
    *h = (*h + 1) & ~1u;
    if (*w < 32) *w = 32;
    if (*h < 32) *h = 32;
    return 1;
}


/* The smallest render size DLSS itself reports (Dynamic.Min.Render of any mode it offers, per output size), logged
 * when a request goes below it. Not enforced: DLSS works below it; the Onimusha 28.7% device loss is unexplained. */
static struct { u32 out_w, out_h, min_w, min_h; } g_ngx_min[4];

static void ngx_note_min(u32 out_w, u32 out_h, u32 min_w, u32 min_h)
{
    if (!min_w || !min_h) return;
    for (int i = 0; i < 4; ++i) {
        if (g_ngx_min[i].out_w == out_w && g_ngx_min[i].out_h == out_h) {
            if (min_w < g_ngx_min[i].min_w) g_ngx_min[i].min_w = min_w;
            if (min_h < g_ngx_min[i].min_h) g_ngx_min[i].min_h = min_h;
            return;
        }
        if (!g_ngx_min[i].out_w) { g_ngx_min[i] = (typeof(g_ngx_min[0])){ out_w, out_h, min_w, min_h }; return; }
    }
}
static int ngx_min_for(u32 out_w, u32 out_h, u32 *min_w, u32 *min_h)
{
    for (int i = 0; i < 4; ++i)
        if (g_ngx_min[i].out_w == out_w && g_ngx_min[i].out_h == out_h) {
            *min_w = g_ngx_min[i].min_w;
            *min_h = g_ngx_min[i].min_h;
            return 1;
        }
    return 0;
}

static u64 g_ngx_real_optimal;                        /* the DLSS DLL's optimal-settings callback */
static u64 g_ngx_optimal_thunk;                       /* Beer's replacement, callable from guest code */

/* Hook trace: entry/exit of every wrapped call while an override is active (first 400), so a stall shows where. */
static int ngx_trace(void)
{
    static _Atomic int lines;
    return ngx_size_override() && atomic_fetch_add(&lines, 1) < 400;
}

static u32 __attribute__((ms_abi)) ngx_optimal_settings(u64 params)
{
    const int trace = ngx_trace();
    if (trace) fprintf(stderr, "[NGX] optimal-settings query (tid %d) -> real callback\n", (int)syscall(SYS_gettid));
    const u32 result = g_ngx_real_optimal ? ((NgxFn1)(uintptr_t)g_ngx_real_optimal)(params) : 0xBAD00000u;
    if (trace) fprintf(stderr, "[NGX] optimal-settings real callback returned 0x%x\n", result);
    u32 out_w = 0, out_h = 0, quality = 0, w, h;
    if (result != NGX_RESULT_SUCCESS || !ngx_get_u32(params, "Width", &out_w) || !ngx_get_u32(params, "Height", &out_h))
        return result;
    ngx_get_u32(params, "PerfQualityValue", &quality);
    {
        u32 real_w = 0, real_h = 0, real_min_w = 0, real_min_h = 0;
        ngx_get_u32(params, "OutWidth", &real_w);
        ngx_get_u32(params, "OutHeight", &real_h);
        ngx_get_u32(params, "DLSS.Get.Dynamic.Min.Render.Width", &real_min_w);
        ngx_get_u32(params, "DLSS.Get.Dynamic.Min.Render.Height", &real_min_h);
        if (real_w && real_h) {                      /* a mode this DLSS offers: its minimum, else its own size */
            ngx_note_min(out_w, out_h, real_min_w ? real_min_w : real_w, real_min_h ? real_min_h : real_h);
        }
    }
    if (!ngx_render_size(out_w, out_h, &w, &h)) return result;
    {   /* Diagnostic only: DLSS runs below its reported minimum (tools commonly use 20-25%), so no floor. */
        u32 floor_w, floor_h;
        static u32 noted;
        if (ngx_min_for(out_w, out_h, &floor_w, &floor_h) && (w < floor_w || h < floor_h) && noted != w) {
            noted = w;
            fprintf(stderr, "[NGX] %ux%u is below the smallest size DLSS reports (%ux%u for %ux%u)\n",
                    w, h, floor_w, floor_h, out_w, out_h);
        }
    }
    u32 game_w = 0, game_h = 0, min_w = w, min_h = h;
    ngx_get_u32(params, "OutWidth", &game_w);
    ngx_get_u32(params, "OutHeight", &game_h);
    if (!game_w || !game_h) return result;            /* a mode this DLSS version does not offer stays unavailable */
    ngx_get_u32(params, "DLSS.Get.Dynamic.Min.Render.Width", &min_w);
    ngx_get_u32(params, "DLSS.Get.Dynamic.Min.Render.Height", &min_h);
    ngx_set_u32(params, "OutWidth", w);
    ngx_set_u32(params, "OutHeight", h);
    ngx_set_u32(params, "DLSS.Get.Dynamic.Max.Render.Width", w);
    ngx_set_u32(params, "DLSS.Get.Dynamic.Max.Render.Height", h);
    ngx_set_u32(params, "DLSS.Get.Dynamic.Min.Render.Width", min_w < w ? min_w : w);
    ngx_set_u32(params, "DLSS.Get.Dynamic.Min.Render.Height", min_h < h ? min_h : h);
    static u32 logged[8][4];
    static int nlogged;
    int seen = 0;
    for (int i = 0; i < nlogged; ++i)
        if (logged[i][0] == out_w && logged[i][1] == out_h && logged[i][2] == quality && logged[i][3] == w) seen = 1;
    if (!seen) {
        const int slot = nlogged < 8 ? nlogged++ : 7;
        logged[slot][0] = out_w; logged[slot][1] = out_h; logged[slot][2] = quality; logged[slot][3] = w;
        fprintf(stderr, "[NGX] optimal settings: output %ux%u mode %u: game size %ux%u -> %ux%u\n",
                out_w, out_h, quality, game_w, game_h, w, h);
    }
    return result;
}

/* A parameter block from the NGX core: route its optimal-settings callback through Beer. */
static void ngx_hook_params(u64 params)
{
    if (!params) return;
    u64 callback = 0;
    if (!ngx_get_ptr(params, "DLSSOptimalSettingsCallback", &callback) || !callback) return;
    if (!g_ngx_optimal_thunk)
        g_ngx_optimal_thunk = beer_guest_com_entry((u64)(uintptr_t)ngx_optimal_settings, "ngx-optimal", 0);
    if (callback == g_ngx_optimal_thunk) return;
    if (g_ngx_real_optimal && g_ngx_real_optimal != callback)
        fprintf(stderr, "[NGX] optimal-settings callback changed %llx -> %llx\n",
                (unsigned long long)g_ngx_real_optimal, (unsigned long long)callback);
    g_ngx_real_optimal = callback;
    ngx_set_ptr(params, "DLSSOptimalSettingsCallback", g_ngx_optimal_thunk);
}

static const char *const g_ngx_param_exports[] = {
    "NVSDK_NGX_D3D12_GetCapabilityParameters", "NVSDK_NGX_D3D12_GetParameters", "NVSDK_NGX_D3D12_AllocateParameters",
    "NVSDK_NGX_VULKAN_GetCapabilityParameters", "NVSDK_NGX_VULKAN_GetParameters", "NVSDK_NGX_VULKAN_AllocateParameters",
    "NVSDK_NGX_D3D11_GetCapabilityParameters", "NVSDK_NGX_D3D11_GetParameters", "NVSDK_NGX_D3D11_AllocateParameters",
};
#define NGX_PARAM_EXPORTS (sizeof(g_ngx_param_exports) / sizeof(*g_ngx_param_exports))
static const char *const g_ngx_create_exports[] = {
    "NVSDK_NGX_D3D12_CreateFeature", "NVSDK_NGX_D3D11_CreateFeature",
};
#define NGX_CREATE_EXPORTS (sizeof(g_ngx_create_exports) / sizeof(*g_ngx_create_exports))
static u64 g_ngx_real_param[NGX_PARAM_EXPORTS], g_ngx_real_create[NGX_CREATE_EXPORTS];

/* NVSDK_NGX_<api>_GetCapabilityParameters / GetParameters / AllocateParameters(NVSDK_NGX_Parameter **out). */
#define NGX_PARAM_WRAPPER(i) \
    static u32 __attribute__((ms_abi)) ngx_param_##i(u64 out) \
    { \
        const int trace = ngx_trace(); \
        if (trace) fprintf(stderr, "[NGX] %s (tid %d)\n", g_ngx_param_exports[i], (int)syscall(SYS_gettid)); \
        const u32 result = ((NgxFn1)(uintptr_t)g_ngx_real_param[i])(out); \
        if (trace) fprintf(stderr, "[NGX] %s -> 0x%x\n", g_ngx_param_exports[i], result); \
        if (result == NGX_RESULT_SUCCESS && out) ngx_hook_params(*(u64 *)(uintptr_t)out); \
        return result; \
    }
NGX_PARAM_WRAPPER(0) NGX_PARAM_WRAPPER(1) NGX_PARAM_WRAPPER(2) NGX_PARAM_WRAPPER(3) NGX_PARAM_WRAPPER(4)
NGX_PARAM_WRAPPER(5) NGX_PARAM_WRAPPER(6) NGX_PARAM_WRAPPER(7) NGX_PARAM_WRAPPER(8)
static u32 (__attribute__((ms_abi)) *const g_ngx_param_wrappers[NGX_PARAM_EXPORTS])(u64) = {
    ngx_param_0, ngx_param_1, ngx_param_2, ngx_param_3, ngx_param_4, ngx_param_5, ngx_param_6, ngx_param_7, ngx_param_8,
};

/* Before DLSS super sampling is created: the preset hints, and the quality mode that matches an overridden size
 * (the game passes the render size it got from the optimal-settings callback as Width/Height). */
static void ngx_before_create(u64 feature, u64 params)
{
    if (feature != NGX_FEATURE_SUPER_SAMPLING || !params) return;
    ngx_read_config();
    ngx_hook_params(params);
    const NgxConfig cfg = ngx_config();
    if (cfg.preset >= 0) {
        static const char *const keys[] = {
            "DLSS.Hint.Render.Preset.DLAA", "DLSS.Hint.Render.Preset.Quality", "DLSS.Hint.Render.Preset.Balanced",
            "DLSS.Hint.Render.Preset.Performance", "DLSS.Hint.Render.Preset.UltraPerformance",
            "DLSS.Hint.Render.Preset.UltraQuality",
        };
        for (size_t i = 0; i < sizeof(keys) / sizeof(*keys); ++i) ngx_set_u32(params, keys[i], (u32)cfg.preset);
    }
    u32 in_w = 0, in_h = 0, out_w = 0, out_h = 0, quality = 0;
    ngx_get_u32(params, "Width", &in_w);
    ngx_get_u32(params, "Height", &in_h);
    ngx_get_u32(params, "OutWidth", &out_w);
    ngx_get_u32(params, "OutHeight", &out_h);
    ngx_get_u32(params, "PerfQualityValue", &quality);
    /* The game's quality mode is kept (as DLSSTweaks does): switching it to the mode "matching" the size put DLSS in
     * DLAA with a smaller input (80.5%: Xid 32) or in Ultra Performance below its range (28.7%: Xid 13). */
    fprintf(stderr, "[NGX] DLSS create: render %ux%u -> output %ux%u, mode %u, preset %d\n", in_w, in_h, out_w, out_h,
            quality, cfg.preset);
}

/* DLSS super-sampling handles (NVSDK_NGX_Handle*) created through the D3D12 core, for the "no upscaling" bypass. */
static u64 g_ngx_sr_handles[16];
static void ngx_note_handle(u64 handle_out)
{
    const u64 handle = handle_out ? *(u64 *)(uintptr_t)handle_out : 0;
    if (!handle) return;
    for (int i = 0; i < 16; ++i) if (g_ngx_sr_handles[i] == handle) return;
    for (int i = 0; i < 16; ++i) if (!g_ngx_sr_handles[i]) { g_ngx_sr_handles[i] = handle; return; }
    g_ngx_sr_handles[0] = handle;
}
static int ngx_is_sr_handle(u64 handle)
{
    for (int i = 0; handle && i < 16; ++i) if (g_ngx_sr_handles[i] == handle) return 1;
    return 0;
}

#define NGX_CREATE_WRAPPER(i) \
    static u32 __attribute__((ms_abi)) ngx_create_##i(u64 cmd, u64 feature, u64 params, u64 handle) \
    { \
        ngx_before_create(feature, params); \
        if (feature == NGX_FEATURE_SUPER_SAMPLING) fprintf(stderr, "[NGX] CreateFeature calling (tid %d)\n", (int)syscall(SYS_gettid)); \
        const u32 result = ((NgxFn4)(uintptr_t)g_ngx_real_create[i])(cmd, feature, params, handle); \
        if (feature == NGX_FEATURE_SUPER_SAMPLING) fprintf(stderr, "[NGX] CreateFeature returned 0x%x\n", result); \
        if (feature == NGX_FEATURE_SUPER_SAMPLING && result != NGX_RESULT_SUCCESS) \
            fprintf(stderr, "[NGX] DLSS create failed: 0x%x\n", result); \
        if (i == 0 && feature == NGX_FEATURE_SUPER_SAMPLING && result == NGX_RESULT_SUCCESS) ngx_note_handle(handle); \
        return result; \
    }
NGX_CREATE_WRAPPER(0) NGX_CREATE_WRAPPER(1)
static u32 (__attribute__((ms_abi)) *const g_ngx_create_wrappers[NGX_CREATE_EXPORTS])(u64, u64, u64, u64) = {
    ngx_create_0, ngx_create_1,
};

/* NVSDK_NGX_D3D12_EvaluateFeature(command list, handle, parameters, progress callback). With "no upscaling" on
 * (dlss_bypass), DLSS super sampling is replaced by a linear stretch of the colour input (its render subrect) over
 * the output: the game's internal-resolution image as it reaches DLSS, for comparison. */
static u64 g_ngx_real_evaluate;
static u32 __attribute__((ms_abi)) ngx_evaluate(u64 cmd, u64 handle, u64 params, u64 progress)
{
    if (beer_control_dlss_bypass() && params && ngx_is_sr_handle(handle)) {
        const u64 color = ngx_get_resource(params, "Color"), output = ngx_get_resource(params, "Output");
        u32 x = 0, y = 0, w = 0, h = 0;
        ngx_get_u32(params, "DLSS.Input.Color.Subrect.Base.X", &x);
        ngx_get_u32(params, "DLSS.Input.Color.Subrect.Base.Y", &y);
        ngx_get_u32(params, "DLSS.Render.Subrect.Dimensions.Width", &w);
        ngx_get_u32(params, "DLSS.Render.Subrect.Dimensions.Height", &h);
        static int logged;
        if (color && output && bd12_debug_stretch((void *)(uintptr_t)cmd, (void *)(uintptr_t)color, x, y, w, h,
                                                  (void *)(uintptr_t)output)) {
            if (!logged++) fprintf(stderr, "[NGX] DLSS bypassed: input %ux%u at %u,%u stretched to the output\n", w, h, x, y);
            return NGX_RESULT_SUCCESS;
        }
        if (logged != -1) { logged = -1; fprintf(stderr, "[NGX] DLSS bypass not possible here (color=%llx output=%llx)\n",
                                                   (unsigned long long)color, (unsigned long long)output); }
    }
    return ((NgxFn4)(uintptr_t)g_ngx_real_evaluate)(cmd, handle, params, progress);
}

/* GetProcAddress on a guest DLL: NGX core entry points get Beer's wrapper when an override is set. */
static u64 ngx_wrap_export(const GuestDll *dll, const char *name, u64 address)
{
    if (!address || strncmp(name, "NVSDK_NGX_", 10) ||
        (strcasecmp(dll->name, "_nvngx.dll") && strcasecmp(dll->name, "nvngx.dll")))
        return address;
    ngx_read_config();                                /* always wrapped: the override can be switched on live */
    for (size_t i = 0; i < NGX_PARAM_EXPORTS; ++i)
        if (!strcmp(name, g_ngx_param_exports[i])) {
            if (g_ngx_real_param[i] && g_ngx_real_param[i] != address) return address;   /* second core: leave it */
            g_ngx_real_param[i] = address;
            return beer_guest_com_entry((u64)(uintptr_t)g_ngx_param_wrappers[i], name, i);
        }
    if (!strcmp(name, "NVSDK_NGX_D3D12_EvaluateFeature")) {
        if (g_ngx_real_evaluate && g_ngx_real_evaluate != address) return address;
        g_ngx_real_evaluate = address;
        return beer_guest_com_entry((u64)(uintptr_t)ngx_evaluate, name, 0);
    }
    for (size_t i = 0; i < NGX_CREATE_EXPORTS; ++i)
        if (!strcmp(name, g_ngx_create_exports[i])) {
            if (g_ngx_real_create[i] && g_ngx_real_create[i] != address) return address;
            g_ngx_real_create[i] = address;
            return beer_guest_com_entry((u64)(uintptr_t)g_ngx_create_wrappers[i], name, i);
        }
    return address;
}
