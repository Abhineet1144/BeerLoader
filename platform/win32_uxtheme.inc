/* uxtheme.dll as Windows behaves with visual styles off (the Classic theme).
 *
 * Beer draws no visual styles, so no theme is active and OpenThemeData never yields a handle. Programs
 * check IsAppThemed/IsThemeActive (or test the handle) and fall back to classic drawing, as they do on
 * such systems. Functions that take a theme handle report E_HANDLE; predicates report FALSE; buffered
 * painting reports failure. The GetThemeSys* functions with no theme answer from the system settings,
 * as Windows does. Included from loader.c before the implementation table. */

#define UXTHEME_E_HANDLE      0x80070006u   /* E_HANDLE: no theme handle can exist */
#define UXTHEME_E_NOTIMPL     0x80004001u
#define UXTHEME_E_FAIL        0x80004005u
#define UXTHEME_E_INVALIDARG  0x80070057u
#define UXTHEME_S_OK          0u

/* Predicates and handle-returning functions. */
static u64 __attribute__((ms_abi)) impl_ux_false(void) { return 0; }             /* FALSE / NULL HTHEME */
static u64 __attribute__((ms_abi)) impl_ux_open_theme(void) { g_last_error = 0x490; return 0; }   /* ERROR_NOT_FOUND */

/* Everything that needs a theme handle. */
static u64 __attribute__((ms_abi)) impl_ux_no_theme(void) { return UXTHEME_E_HANDLE; }

/* Window-level settings with no outputs: accepted, as Windows records them even without styles. */
static u64 __attribute__((ms_abi)) impl_ux_ok(void) { return UXTHEME_S_OK; }

static u64 __attribute__((ms_abi)) impl_ux_not_implemented(void) { return UXTHEME_E_NOTIMPL; }
static u64 __attribute__((ms_abi)) impl_ux_fail(void) { return UXTHEME_E_FAIL; }
static u64 __attribute__((ms_abi)) impl_ux_invalid(void) { return UXTHEME_E_INVALIDARG; }

/* EnableTheming(fEnable): turning themes off is satisfied; turning them on is not possible. */
static u64 __attribute__((ms_abi)) impl_EnableTheming(u32 enable) { return enable ? UXTHEME_E_FAIL : UXTHEME_S_OK; }

/* GetThemeAppProperties: no STAP_ALLOW_* flags; SetThemeAppProperties has no effect without styles. */
static u64 __attribute__((ms_abi)) impl_GetThemeAppProperties(void) { return 0; }
static u64 __attribute__((ms_abi)) impl_SetThemeAppProperties(u32 flags) { (void)flags; return 0; }

/* With no theme, these return the system values (GetSysColor, GetSysColorBrush, GetSystemMetrics). */
static u64 __attribute__((ms_abi)) impl_GetThemeSysColor(u64 theme, s32 index)
{
    (void)theme;
    ImplFn fn = find_impl("GetSysColor");
    return fn ? ((u64 (__attribute__((ms_abi)) *)(s32))fn)(index) : 0;
}
static u64 __attribute__((ms_abi)) impl_GetThemeSysColorBrush(u64 theme, s32 index)
{
    (void)theme;
    ImplFn fn = find_impl("GetSysColorBrush");
    return fn ? ((u64 (__attribute__((ms_abi)) *)(s32))fn)(index) : 0;
}
static u64 __attribute__((ms_abi)) impl_GetThemeSysSize(u64 theme, s32 index)
{
    (void)theme;
    ImplFn fn = find_impl("GetSystemMetrics");
    return fn ? ((u64 (__attribute__((ms_abi)) *)(s32))fn)(index) : 0;
}

#define UXTHEME_ENTRIES \
    {"OpenThemeData",                    (ImplFn)impl_ux_open_theme}, \
    {"OpenThemeDataEx",                  (ImplFn)impl_ux_open_theme}, \
    {"OpenThemeDataForDpi",              (ImplFn)impl_ux_open_theme}, \
    {"GetWindowTheme",                   (ImplFn)impl_ux_false}, \
    {"IsThemeActive",                    (ImplFn)impl_ux_false}, \
    {"IsAppThemed",                      (ImplFn)impl_ux_false}, \
    {"IsCompositionActive",              (ImplFn)impl_ux_false}, \
    {"IsThemeDialogTextureEnabled",      (ImplFn)impl_ux_false}, \
    {"IsThemePartDefined",               (ImplFn)impl_ux_false}, \
    {"IsThemeBackgroundPartiallyTransparent", (ImplFn)impl_ux_false}, \
    {"CloseThemeData",                   (ImplFn)impl_ux_no_theme}, \
    {"DrawThemeBackground",              (ImplFn)impl_ux_no_theme}, \
    {"DrawThemeBackgroundEx",            (ImplFn)impl_ux_no_theme}, \
    {"DrawThemeText",                    (ImplFn)impl_ux_no_theme}, \
    {"DrawThemeTextEx",                  (ImplFn)impl_ux_no_theme}, \
    {"DrawThemeEdge",                    (ImplFn)impl_ux_no_theme}, \
    {"DrawThemeIcon",                    (ImplFn)impl_ux_no_theme}, \
    {"GetThemeBackgroundContentRect",    (ImplFn)impl_ux_no_theme}, \
    {"GetThemeBackgroundExtent",         (ImplFn)impl_ux_no_theme}, \
    {"GetThemeBackgroundRegion",         (ImplFn)impl_ux_no_theme}, \
    {"GetThemePartSize",                 (ImplFn)impl_ux_no_theme}, \
    {"GetThemeTextExtent",               (ImplFn)impl_ux_no_theme}, \
    {"GetThemeTextMetrics",              (ImplFn)impl_ux_no_theme}, \
    {"HitTestThemeBackground",           (ImplFn)impl_ux_no_theme}, \
    {"GetThemeColor",                    (ImplFn)impl_ux_no_theme}, \
    {"GetThemeMetric",                   (ImplFn)impl_ux_no_theme}, \
    {"GetThemeString",                   (ImplFn)impl_ux_no_theme}, \
    {"GetThemeBool",                     (ImplFn)impl_ux_no_theme}, \
    {"GetThemeInt",                      (ImplFn)impl_ux_no_theme}, \
    {"GetThemeEnumValue",                (ImplFn)impl_ux_no_theme}, \
    {"GetThemePosition",                 (ImplFn)impl_ux_no_theme}, \
    {"GetThemeFont",                     (ImplFn)impl_ux_no_theme}, \
    {"GetThemeRect",                     (ImplFn)impl_ux_no_theme}, \
    {"GetThemeMargins",                  (ImplFn)impl_ux_no_theme}, \
    {"GetThemeIntList",                  (ImplFn)impl_ux_no_theme}, \
    {"GetThemePropertyOrigin",           (ImplFn)impl_ux_no_theme}, \
    {"GetThemeFilename",                 (ImplFn)impl_ux_no_theme}, \
    {"GetThemeBitmap",                   (ImplFn)impl_ux_no_theme}, \
    {"GetThemeStream",                   (ImplFn)impl_ux_no_theme}, \
    {"GetThemeTransitionDuration",       (ImplFn)impl_ux_no_theme}, \
    {"GetThemeSysBool",                  (ImplFn)impl_ux_false}, \
    {"GetThemeSysFont",                  (ImplFn)impl_ux_not_implemented}, \
    {"GetThemeSysString",                (ImplFn)impl_ux_not_implemented}, \
    {"GetThemeSysInt",                   (ImplFn)impl_ux_not_implemented}, \
    {"GetThemeSysColor",                 (ImplFn)impl_GetThemeSysColor}, \
    {"GetThemeSysColorBrush",            (ImplFn)impl_GetThemeSysColorBrush}, \
    {"GetThemeSysSize",                  (ImplFn)impl_GetThemeSysSize}, \
    {"GetCurrentThemeName",              (ImplFn)impl_ux_fail}, \
    {"GetThemeDocumentationProperty",    (ImplFn)impl_ux_fail}, \
    {"DrawThemeParentBackground",        (ImplFn)impl_ux_not_implemented}, \
    {"DrawThemeParentBackgroundEx",      (ImplFn)impl_ux_not_implemented}, \
    {"SetWindowTheme",                   (ImplFn)impl_ux_ok}, \
    {"SetWindowThemeAttribute",          (ImplFn)impl_ux_ok}, \
    {"EnableThemeDialogTexture",         (ImplFn)impl_ux_ok}, \
    {"EnableTheming",                    (ImplFn)impl_EnableTheming}, \
    {"GetThemeAppProperties",            (ImplFn)impl_GetThemeAppProperties}, \
    {"SetThemeAppProperties",            (ImplFn)impl_SetThemeAppProperties}, \
    {"BufferedPaintInit",                (ImplFn)impl_ux_ok}, \
    {"BufferedPaintUnInit",              (ImplFn)impl_ux_ok}, \
    {"BeginBufferedPaint",               (ImplFn)impl_ux_false}, \
    {"BeginBufferedAnimation",           (ImplFn)impl_ux_false}, \
    {"EndBufferedPaint",                 (ImplFn)impl_ux_invalid}, \
    {"EndBufferedAnimation",             (ImplFn)impl_ux_invalid}, \
    {"GetBufferedPaintTargetRect",       (ImplFn)impl_ux_invalid}, \
    {"GetBufferedPaintTargetDC",         (ImplFn)impl_ux_false}, \
    {"GetBufferedPaintDC",               (ImplFn)impl_ux_false}, \
    {"GetBufferedPaintBits",             (ImplFn)impl_ux_invalid}, \
    {"BufferedPaintClear",               (ImplFn)impl_ux_invalid}, \
    {"BufferedPaintSetAlpha",            (ImplFn)impl_ux_invalid}, \
    {"BufferedPaintStopAllAnimations",   (ImplFn)impl_ux_ok}, \
    {"BufferedPaintRenderAnimation",     (ImplFn)impl_ux_false}, \
    {"BeginPanningFeedback",             (ImplFn)impl_ux_false}, \
    {"UpdatePanningFeedback",            (ImplFn)impl_ux_false}, \
    {"EndPanningFeedback",               (ImplFn)impl_ux_false},
