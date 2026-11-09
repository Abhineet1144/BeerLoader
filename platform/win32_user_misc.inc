/* user32, gdi32, imm32, ole32, shell32, setupapi and kernel32 entry points that report what Beer's
 * environment actually provides. There are no menus, dialog templates, IMEs, OpenGL pixel formats,
 * tray icons, layered-window effects, display mode changes or HID devices; such calls fail with the
 * documented values. Included from loader.c before the implementation table. */

/* ---- kernel32 ---------------------------------------------------------------------------------- */

/* IsWow64Process: Beer runs 64-bit processes only, which are never WOW64. */
static u64 __attribute__((ms_abi)) impl_IsWow64Process(u64 process, u32 *wow64)
{
    (void)process;
    if (!wow64) { g_last_error = 87; return 0; }
    *wow64 = 0;
    return 1;
}

/* CopyFileEx: CopyFile with COPY_FILE_FAIL_IF_EXISTS (1). The progress routine is optional on
 * Windows; Beer copies in one step and does not call it. A set cancel flag cancels before copying. */
static u64 __attribute__((ms_abi))
impl_CopyFileExW(const u16 *source, const u16 *destination, u64 progress, u64 data, const s32 *cancel, u32 flags)
{
    (void)progress; (void)data;
    if (cancel && *cancel) { g_last_error = 1235; return 0; }   /* ERROR_REQUEST_ABORTED */
    return impl_CopyFileW(source, destination, (flags & 1u) != 0);
}

/* ---- gdi32 ------------------------------------------------------------------------------------- */

/* CreateDC: "DISPLAY" is the screen DC GetDC(NULL) returns; printers and other devices do not exist. */
static u64 __attribute__((ms_abi)) impl_CreateDCW(const u16 *driver, const u16 *device, const u16 *output, const void *mode)
{
    (void)output; (void)mode;
    const u16 *name = driver ? driver : device;
    static const char display[] = "display";
    int is_display = name != NULL;
    for (int i = 0; is_display && i < 8; i++) {
        u16 c = name[i];
        if (i == 7) { is_display = c == 0; break; }
        if (c >= 'A' && c <= 'Z') c = (u16)(c + 32);
        if (c != (u8)display[i]) is_display = 0;
    }
    if (!is_display) { g_last_error = 1801; return 0; }  /* ERROR_INVALID_PRINTER_NAME */
    return FAKE_HDC;
}

/* No OpenGL pixel formats: none can be described, set or reported. */
static u64 __attribute__((ms_abi)) impl_DescribePixelFormat(u64 dc, s32 index, u32 size, void *descriptor)
{
    (void)dc; (void)index; (void)size; (void)descriptor;
    g_last_error = 2000;                                 /* ERROR_INVALID_PIXEL_FORMAT */
    return 0;
}
static u64 __attribute__((ms_abi)) impl_GetPixelFormat(u64 dc)
{
    (void)dc;
    g_last_error = 2000;
    return 0;
}
static u64 __attribute__((ms_abi)) impl_SetPixelFormat(u64 dc, s32 index, const void *descriptor)
{
    (void)dc; (void)index; (void)descriptor;
    g_last_error = 2000;
    return 0;
}
/* No colour-management profile is associated with Beer's display. */
static u64 __attribute__((ms_abi)) impl_GetICMProfileW(u64 dc, u32 *size, u16 *name)
{
    (void)dc; (void)size; (void)name;
    g_last_error = 2302;                                 /* ERROR_PROFILE_NOT_FOUND */
    return 0;
}

/* ---- imm32: no input method editor, so no input contexts exist --------------------------------- */

static u64 __attribute__((ms_abi)) impl_ImmGetContext(u64 window) { (void)window; return 0; }
static u64 __attribute__((ms_abi)) impl_ImmReleaseContext(u64 window, u64 context) { (void)window; (void)context; return 1; }
static u64 __attribute__((ms_abi)) impl_ImmAssociateContext(u64 window, u64 context) { (void)window; (void)context; return 0; }
static u64 __attribute__((ms_abi)) impl_imm_no_context_false(void) { return 0; }       /* BOOL / DWORD 0 */
static u64 __attribute__((ms_abi)) impl_ImmGetCompositionStringW(void)
{
    return (u64)(s64)-2;                                 /* IMM_ERROR_GENERAL: no input context */
}

/* ---- ole32 drag and drop ----------------------------------------------------------------------- */

/* A drop target can be registered for a window; Beer has no drag sources, so it never receives a
 * drop. RegisterDragDrop takes a reference on the target, RevokeDragDrop releases it. */
#define MAX_DROP_TARGETS 32
static struct { u64 window, target; } g_drop_targets[MAX_DROP_TARGETS];
static pthread_mutex_t g_drop_target_lock = PTHREAD_MUTEX_INITIALIZER;
typedef u32 __attribute__((ms_abi)) (*ComRefFn)(u64 self);
static void com_add_ref(u64 object) { ((ComRefFn)(uintptr_t)((u64 *)(uintptr_t)*(u64 *)(uintptr_t)object)[1])(object); }
static void com_release(u64 object) { ((ComRefFn)(uintptr_t)((u64 *)(uintptr_t)*(u64 *)(uintptr_t)object)[2])(object); }

static u64 __attribute__((ms_abi)) impl_RegisterDragDrop(u64 window, u64 target)
{
    if (!window) return 0x80040102u;                     /* DRAGDROP_E_INVALIDHWND */
    if (!target) return 0x80070057u;                     /* E_INVALIDARG */
    pthread_mutex_lock(&g_drop_target_lock);
    int slot = -1;
    for (int i = 0; i < MAX_DROP_TARGETS; i++) {
        if (g_drop_targets[i].window == window) { pthread_mutex_unlock(&g_drop_target_lock); return 0x80040101u; }  /* already registered */
        if (slot < 0 && !g_drop_targets[i].window) slot = i;
    }
    if (slot < 0) { pthread_mutex_unlock(&g_drop_target_lock); return 0x8007000Eu; }   /* E_OUTOFMEMORY */
    g_drop_targets[slot].window = window;
    g_drop_targets[slot].target = target;
    pthread_mutex_unlock(&g_drop_target_lock);
    com_add_ref(target);
    return 0;
}
static u64 __attribute__((ms_abi)) impl_RevokeDragDrop(u64 window)
{
    u64 target = 0;
    pthread_mutex_lock(&g_drop_target_lock);
    for (int i = 0; i < MAX_DROP_TARGETS && !target; i++)
        if (g_drop_targets[i].window == window && window) {
            target = g_drop_targets[i].target;
            g_drop_targets[i].window = g_drop_targets[i].target = 0;
        }
    pthread_mutex_unlock(&g_drop_target_lock);
    if (!target) return 0x80040100u;                     /* DRAGDROP_E_NOTREGISTERED */
    com_release(target);
    return 0;
}

/* ReleaseStgMedium: frees the medium's storage by its TYMED, or releases pUnkForRelease instead.
 * STGMEDIUM is { DWORD tymed; union { HGLOBAL; LPOLESTR file; IStream*; IStorage*; HBITMAP; ... }; IUnknown *pUnkForRelease }. */
static u64 __attribute__((ms_abi)) impl_ReleaseStgMedium(u8 *medium)
{
    if (!medium) return 0;
    u32 tymed;
    u64 handle, unknown;
    memcpy(&tymed, medium, 4);
    memcpy(&handle, medium + 8, 8);
    memcpy(&unknown, medium + 16, 8);
    if (unknown) {
        com_release(unknown);
    } else if (handle) {
        if (tymed == 1) {                                /* TYMED_HGLOBAL */
            ImplFn fn = find_impl("GlobalFree");
            if (fn) ((u64 (__attribute__((ms_abi)) *)(u64))fn)(handle);
        } else if (tymed == 2) {                         /* TYMED_FILE: the name is CoTaskMem */
            free((void *)(uintptr_t)handle);
        } else if (tymed == 4 || tymed == 8) {           /* TYMED_ISTREAM / TYMED_ISTORAGE */
            com_release(handle);
        } else if (tymed == 16 || tymed == 32 || tymed == 64) {   /* GDI, MFPICT, ENHMF */
            ImplFn fn = find_impl("DeleteObject");
            if (fn) ((u64 (__attribute__((ms_abi)) *)(u64))fn)(handle);
        }
    }
    memset(medium, 0, 24);
    return 0;
}

/* ---- setupapi / shell32 ------------------------------------------------------------------------ */

/* SetupDi enumeration in Beer yields no devices, so no device information element is valid. */
static u64 __attribute__((ms_abi)) impl_SetupDiGetDeviceInstanceIdA(u64 set, void *info, char *id, u32 size, u32 *required)
{
    (void)set; (void)info; (void)size;
    if (id && size) id[0] = 0;
    if (required) *required = 0;
    g_last_error = 0xE000020Bu;                          /* ERROR_NO_SUCH_DEVINST */
    return 0;
}
/* There is no notification area to hold an icon. */
static u64 __attribute__((ms_abi)) impl_Shell_NotifyIconW(u32 message, void *data)
{
    (void)message; (void)data;
    g_last_error = 50;
    return 0;
}

/* ---- user32 ------------------------------------------------------------------------------------ */

/* All of Beer's windows share one input state, which is what attaching input queues achieves. */
static u64 __attribute__((ms_abi)) impl_AttachThreadInput(u32 from, u32 to, u32 attach)
{
    (void)attach;
    if (!from || !to || from == to) { g_last_error = 87; return 0; }
    return 1;
}

/* ChangeDisplaySettingsEx: the display mode is fixed. Restoring the registry mode (NULL mode)
 * changes nothing and succeeds; any other request fails. */
static u64 __attribute__((ms_abi))
impl_ChangeDisplaySettingsExW(const u16 *device, const void *mode, u64 window, u32 flags, void *param)
{
    (void)device; (void)window; (void)param;
    if (!mode && !(flags & ~0x40000000u)) return 0;      /* DISP_CHANGE_SUCCESSFUL (CDS_RESET allowed) */
    return (u64)(s64)-1;                                 /* DISP_CHANGE_FAILED */
}

/* Menus: none exist, so every menu handle is invalid. */
static u64 __attribute__((ms_abi)) impl_CheckMenuItem(u64 menu, u32 item, u32 flags)
{
    (void)menu; (void)item; (void)flags;
    return (u64)(u32)-1;                                 /* the item does not exist */
}
static u64 __attribute__((ms_abi)) impl_menu_invalid(void)
{
    g_last_error = 1401;                                 /* ERROR_INVALID_MENU_HANDLE */
    return 0;
}

/* Icons from resource bits are not supported (CreateIconIndirect is not either). */
static u64 __attribute__((ms_abi)) impl_CreateIconFromResource(const u8 *bits, u32 size, u32 icon, u32 version)
{
    (void)bits; (void)size; (void)icon; (void)version;
    g_last_error = 50;
    return 0;
}

/* CreateIconFromResourceEx(bits, size, icon, version, cx, cy, flags): the same as CreateIconFromResource with a
 * size and flags. Not supported either. */
static u64 __attribute__((ms_abi)) impl_CreateIconFromResourceEx(const u8 *bits, u32 size, u32 icon, u32 version,
                                                                  s32 cx, s32 cy, u32 flags)
{
    (void)cx; (void)cy; (void)flags;
    return impl_CreateIconFromResource(bits, size, icon, version);
}

/* Dialog templates are not supported; no dialog windows or their controls exist. */
static u64 __attribute__((ms_abi)) impl_DialogBoxIndirectParamW(u64 instance, const void *dialog_template, u64 parent,
                                                                 u64 dialog_proc, u64 init_param)
{
    (void)instance; (void)dialog_template; (void)parent; (void)dialog_proc; (void)init_param;
    g_last_error = 50;
    return (u64)(s64)-1;
}
static u64 __attribute__((ms_abi)) impl_EndDialog(u64 dialog, s64 result)
{
    (void)dialog; (void)result;
    g_last_error = 1400;                                 /* ERROR_INVALID_WINDOW_HANDLE */
    return 0;
}
static u64 __attribute__((ms_abi)) impl_GetDlgItem(u64 dialog, s32 id)
{
    (void)dialog; (void)id;
    g_last_error = 1421;                                 /* ERROR_CONTROL_ID_NOT_FOUND */
    return 0;
}

/* The process has no clipboard access (OpenClipboard fails), for which Windows returns 0. */
static u64 __attribute__((ms_abi)) impl_GetClipboardSequenceNumber(void) { return 0; }

/* Buffered raw input reads: Beer delivers raw input through WM_INPUT only, so the buffer is empty. */
static u64 __attribute__((ms_abi)) impl_GetRawInputBuffer(void *data, u32 *size, u32 header_size)
{
    (void)data;
    if (!size || header_size != 24) { g_last_error = 87; return (u64)(u32)-1; }   /* sizeof(RAWINPUTHEADER) */
    if (!data) *size = 0;
    return 0;
}

/* Layered-window transparency is not composited by Beer's presenter. */
static u64 __attribute__((ms_abi)) impl_SetLayeredWindowAttributes(u64 window, u32 key, u8 alpha, u32 flags)
{
    (void)window; (void)key; (void)alpha; (void)flags;
    g_last_error = 50;
    return 0;
}

/* TrackMouseEvent: Beer does not generate WM_MOUSELEAVE / WM_MOUSEHOVER, so tracking is not set up. */
static u64 __attribute__((ms_abi)) impl_TrackMouseEvent(void *track)
{
    (void)track;
    g_last_error = 50;
    return 0;
}

/* Registered window messages and clipboard formats share one table of names (0xC000-0xFFFF), as
 * both come from the global atom table; names compare case-insensitively. */
#define MAX_REGISTERED_NAMES 512
static char g_registered_names[MAX_REGISTERED_NAMES][128];
static u32 g_registered_name_count;
static pthread_mutex_t g_registered_name_lock = PTHREAD_MUTEX_INITIALIZER;
static u64 register_global_name(const void *name, int wide)
{
    if (!name) { g_last_error = 87; return 0; }
    char key[128];
    size_t i = 0;
    for (; i + 1 < sizeof(key); i++) {
        u32 c = wide ? ((const u16 *)name)[i] : ((const u8 *)name)[i];
        if (!c) break;
        key[i] = c < 0x80 ? (char)tolower((int)c) : '?';
    }
    key[i] = 0;
    if (!i) { g_last_error = 87; return 0; }
    pthread_mutex_lock(&g_registered_name_lock);
    u64 id = 0;
    for (u32 k = 0; k < g_registered_name_count && !id; k++)
        if (!strcmp(g_registered_names[k], key)) id = 0xC000u + k;
    if (!id && g_registered_name_count < MAX_REGISTERED_NAMES) {
        snprintf(g_registered_names[g_registered_name_count], sizeof(g_registered_names[0]), "%s", key);
        id = 0xC000u + g_registered_name_count++;
    }
    pthread_mutex_unlock(&g_registered_name_lock);
    if (!id) g_last_error = 8;
    return id;
}
static u64 __attribute__((ms_abi)) impl_RegisterWindowMessageA(const char *name) { return register_global_name(name, 0); }
static u64 __attribute__((ms_abi)) impl_RegisterWindowMessageW(const u16 *name) { return register_global_name(name, 1); }
static u64 __attribute__((ms_abi)) impl_RegisterClipboardFormatA(const char *name) { return register_global_name(name, 0); }
static u64 __attribute__((ms_abi)) impl_RegisterClipboardFormatW(const u16 *name) { return register_global_name(name, 1); }

/* ToUnicode(vk, scan, key state, buffer, size, flags): the character a key produces with the US
 * layout. Returns the number of characters written, 0 when the key produces none. */
static u64 __attribute__((ms_abi)) impl_ToUnicode(u32 vk, u32 scan, const u8 *state, u16 *buffer, s32 size, u32 flags)
{
    (void)scan; (void)flags;
    if (!buffer || size <= 0) return 0;
    int shift = state && (state[0x10] & 0x80), caps = state && (state[0x14] & 1);
    int ctrl = state && (state[0x11] & 0x80), alt = state && (state[0x12] & 0x80);
    u16 c = 0;
    if (vk >= 'A' && vk <= 'Z') {
        if (ctrl && !alt) c = (u16)(vk - 'A' + 1);       /* Ctrl+letter: control character */
        else if (!ctrl && !alt) c = (u16)((shift ^ caps) ? vk : vk + 32);
    } else if (!ctrl && !alt) {
        static const char shifted_digits[] = ")!@#$%^&*(";
        static const struct { u8 vk; char plain, shifted; } oem[] = {
            { 0xBA, ';', ':' }, { 0xBB, '=', '+' }, { 0xBC, ',', '<' }, { 0xBD, '-', '_' }, { 0xBE, '.', '>' },
            { 0xBF, '/', '?' }, { 0xC0, '`', '~' }, { 0xDB, '[', '{' }, { 0xDC, '\\', '|' }, { 0xDD, ']', '}' },
            { 0xDE, '\'', '"' },
        };
        if (vk >= '0' && vk <= '9') c = (u16)(shift ? shifted_digits[vk - '0'] : vk);
        else if (vk >= 0x60 && vk <= 0x69) c = (u16)('0' + vk - 0x60);           /* VK_NUMPAD0..9 */
        else if (vk == 0x6A) c = '*';
        else if (vk == 0x6B) c = '+';
        else if (vk == 0x6D) c = '-';
        else if (vk == 0x6E) c = '.';
        else if (vk == 0x6F) c = '/';
        else if (vk == 0x20) c = ' ';
        else if (vk == 0x0D) c = '\r';
        else if (vk == 0x09) c = '\t';
        else if (vk == 0x08) c = '\b';
        else if (vk == 0x1B) c = 0x1B;
        else for (size_t i = 0; i < sizeof(oem) / sizeof(*oem) && !c; i++)
            if (oem[i].vk == vk) c = (u16)(shift ? oem[i].shifted : oem[i].plain);
    }
    if (!c) return 0;
    buffer[0] = c;
    if (size > 1) buffer[1] = 0;
    return 1;
}

#define USER_MISC_ENTRIES \
    {"IsWow64Process",              (ImplFn)impl_IsWow64Process}, \
    {"CopyFileExW",                 (ImplFn)impl_CopyFileExW}, \
    {"EnumResourceNamesW",          (ImplFn)beer_EnumResourceNamesW}, \
    {"CreateDCW",                   (ImplFn)impl_CreateDCW}, \
    {"DescribePixelFormat",         (ImplFn)impl_DescribePixelFormat}, \
    {"GetPixelFormat",              (ImplFn)impl_GetPixelFormat}, \
    {"SetPixelFormat",              (ImplFn)impl_SetPixelFormat}, \
    {"GetICMProfileW",              (ImplFn)impl_GetICMProfileW}, \
    {"ImmGetContext",               (ImplFn)impl_ImmGetContext}, \
    {"ImmReleaseContext",           (ImplFn)impl_ImmReleaseContext}, \
    {"ImmAssociateContext",         (ImplFn)impl_ImmAssociateContext}, \
    {"ImmGetCandidateListW",        (ImplFn)impl_imm_no_context_false}, \
    {"ImmGetCompositionFontW",      (ImplFn)impl_imm_no_context_false}, \
    {"ImmGetCompositionStringW",    (ImplFn)impl_ImmGetCompositionStringW}, \
    {"ImmGetIMEFileNameA",          (ImplFn)impl_imm_no_context_false}, \
    {"ImmSetCandidateWindow",       (ImplFn)impl_imm_no_context_false}, \
    {"ImmSetCompositionStringW",    (ImplFn)impl_imm_no_context_false}, \
    {"ImmSetCompositionWindow",     (ImplFn)impl_imm_no_context_false}, \
    {"RegisterDragDrop",            (ImplFn)impl_RegisterDragDrop}, \
    {"RevokeDragDrop",              (ImplFn)impl_RevokeDragDrop}, \
    {"ReleaseStgMedium",            (ImplFn)impl_ReleaseStgMedium}, \
    {"SetupDiGetDeviceInstanceIdA", (ImplFn)impl_SetupDiGetDeviceInstanceIdA}, \
    {"Shell_NotifyIconW",           (ImplFn)impl_Shell_NotifyIconW}, \
    {"AttachThreadInput",           (ImplFn)impl_AttachThreadInput}, \
    {"ChangeDisplaySettingsExW",    (ImplFn)impl_ChangeDisplaySettingsExW}, \
    {"CheckMenuItem",               (ImplFn)impl_CheckMenuItem}, \
    {"InsertMenuW",                 (ImplFn)impl_menu_invalid}, \
    {"TrackPopupMenu",              (ImplFn)impl_menu_invalid}, \
    {"CreateIconFromResource",      (ImplFn)impl_CreateIconFromResource}, \
    {"CreateIconFromResourceEx",    (ImplFn)impl_CreateIconFromResourceEx}, \
    {"DialogBoxIndirectParamW",     (ImplFn)impl_DialogBoxIndirectParamW}, \
    {"EndDialog",                   (ImplFn)impl_EndDialog}, \
    {"GetDlgItem",                  (ImplFn)impl_GetDlgItem}, \
    {"GetClipboardSequenceNumber",  (ImplFn)impl_GetClipboardSequenceNumber}, \
    {"GetRawInputBuffer",           (ImplFn)impl_GetRawInputBuffer}, \
    {"SetLayeredWindowAttributes",  (ImplFn)impl_SetLayeredWindowAttributes}, \
    {"TrackMouseEvent",             (ImplFn)impl_TrackMouseEvent}, \
    {"RegisterWindowMessageA",      (ImplFn)impl_RegisterWindowMessageA}, \
    {"RegisterWindowMessageW",      (ImplFn)impl_RegisterWindowMessageW}, \
    {"RegisterClipboardFormatA",    (ImplFn)impl_RegisterClipboardFormatA}, \
    {"RegisterClipboardFormatW",    (ImplFn)impl_RegisterClipboardFormatW}, \
    {"ToUnicode",                   (ImplFn)impl_ToUnicode}, \
    {"GetClassInfoExW",             (ImplFn)impl_GetClassInfoExW}, \
    {"GetClassInfoExA",             (ImplFn)impl_GetClassInfoExA}, \
    {"GetClassInfoW",               (ImplFn)impl_GetClassInfoW}, \
    {"GetClassInfoA",               (ImplFn)impl_GetClassInfoA}, \
    {"GetClipCursor",               (ImplFn)impl_GetClipCursor}, \
    {"GetQueueStatus",              (ImplFn)impl_GetQueueStatus}, \
    {"GetMessageTime",              (ImplFn)impl_GetMessageTime}, \
    {"GetMessagePos",               (ImplFn)impl_GetMessagePos},
