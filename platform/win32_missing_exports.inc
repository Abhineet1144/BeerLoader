/* Win32 exports that protected executables resolve by name but a plain run does not need.
 * Generated from a real RE4 start-up log; unimplemented ones return 0 (FALSE/NULL). */
static u64 __attribute__((ms_abi)) stub_GetConsoleMode(u64 h, u32 *mode){ (void)h; if(mode)*mode=0; return 0; }
static u64 __attribute__((ms_abi)) stub_RemoveDirectoryW(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetConsoleOutputCP(void){ return 437; }
static u64 __attribute__((ms_abi)) stub_GetThreadPriority(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetConsoleCP(void){ return 437; }
static u64 __attribute__((ms_abi)) stub_EnumResourceTypesA(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_EnumResourceNamesA(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_EnumResourceLanguagesA(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GlobalAddAtomA(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_WritePrivateProfileStringA(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetVolumeInformationA(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetComputerNameA(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetDateFormatA(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_EnumCalendarInfoA(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetLogicalDriveStringsW(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetTempFileNameW(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_QueryDosDeviceW(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetFileAttributesExW(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetVolumeInformationW(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetComputerNameW(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_FindFirstFileW(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_FindNextFileW(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetDateFormatW(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_FindFirstFileExW(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GlobalReAlloc(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GlobalSize(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GlobalLock(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GlobalUnlock(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_HeapDestroy(void){ return 1; }
static u64 __attribute__((ms_abi)) stub_OpenProcess(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetExitCodeProcess(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_CreateRemoteThread(void){ return 0; }
/* GetThreadContext: Beer cannot read another thread's registers, but it never sets hardware breakpoints, so the debug
 * registers are reported as clear (CONTEXT_DEBUG_REGISTERS: Dr0-Dr3, Dr6, Dr7 at +0x48..+0x77 of the x64 CONTEXT).
 * Failing the call left the caller's buffer as stack garbage, which copy protection (RDR2's Arxan TLS callback)
 * reads as a debugger's breakpoints and answers by sabotaging the game ("Social Club error"). */
static u64 __attribute__((ms_abi)) beer_GetThreadContext(u64 thread, u8 *context)
{
    if (!context) { g_last_error = 998; return 0; }                 /* ERROR_NOACCESS */
    if (thread_unstarted_context(thread, context, 0) > 0) return 1;
    const u32 flags = *(u32 *)(context + 0x30);
    if ((flags & 0x100010u) == 0x100010u) memset(context + 0x48, 0, 0x30);
    return 1;
}
static u64 __attribute__((ms_abi)) stub_GlobalDeleteAtom(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_DeviceIoControl(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_FindClose(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_MulDiv(s32 a, s32 b, s32 c){ if(!c) return (u64)(s64)-1; return (u64)(s64)(s32)(((int64_t)a*b + (c/2))/c); }
static u64 __attribute__((ms_abi)) stub_FileTimeToDosDateTime(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_IsBadReadPtr(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_IsBadWritePtr(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetThreadLocale(void){ return 0x409; }
static u64 __attribute__((ms_abi)) stub_SetThreadLocale(void){ return 0; }
/* SetThreadContext: only a thread suspended at creation can be changed (it starts with the new registers);
 * Beer cannot rewrite a running thread's registers. */
static u64 __attribute__((ms_abi)) stub_SetThreadContext(u64 thread, u8 *context)
{
    if (context && thread_unstarted_context(thread, context, 1) > 0) return 1;
    g_last_error = 50;   /* ERROR_NOT_SUPPORTED */
    return 0;
}
static u64 __attribute__((ms_abi)) stub_GetProcessIdOfThread(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetThreadId(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_Module32First(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_Module32Next(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_DebugBreak(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SafeArrayRedim(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SafeArrayPtrOfIndex(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_CharUpperBuffW(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_CharLowerBuffW(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_UnregisterClassA(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetClipboardFormatNameA(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_CharToOemA(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_CharUpperA(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_CharUpperBuffA(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_CharLowerA(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_CharLowerBuffA(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetMenuItemInfoA(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_EnumPropsA(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetWindowTextA(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SetWindowLongA(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetClassLongA(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SetClassLongPtrA(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_FindWindowA(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetClassNameA(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_UnregisterClassW(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_InsertMenuItemW(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetMenuItemInfoW(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SetMenuItemInfoW(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_DrawStateW(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetWindowTextW(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetWindowTextLengthW(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_MessageBoxW(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_FindWindowW(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetClassNameW(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_DefFrameProcW(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_DefMDIChildProcW(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetDoubleClickTime(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_IsMenu(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_ShowWindowAsync(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetWindowPlacement(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SetWindowPlacement(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_BeginDeferWindowPos(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_DeferWindowPos(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_EndDeferWindowPos(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_OpenClipboard(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_CloseClipboard(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SetClipboardData(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetClipboardData(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_CountClipboardFormats(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_EnumClipboardFormats(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_EmptyClipboard(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_IsClipboardFormatAvailable(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_EnableWindow(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetMenu(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SetMenu(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_DrawMenuBar(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetSystemMenu(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_CreateMenu(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_CreatePopupMenu(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_DestroyMenu(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_EnableMenuItem(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetSubMenu(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetMenuItemCount(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_RemoveMenu(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_DeleteMenu(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetMenuItemRect(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_WindowFromDC(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetDCEx(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_ScrollWindowEx(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_ShowScrollBar(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_EnableScrollBar(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_MessageBeep(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_CreateCaret(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_DestroyCaret(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_HideCaret(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_ShowCaret(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SetCaretPos(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetCaretPos(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetSysColor(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetSysColorBrush(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SetSysColors(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_DrawFocusRect(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_FrameRect(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SetParent(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_EnumWindows(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_EnumThreadWindows(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetLastActivePopup(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_DestroyCursor(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_DestroyIcon(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_CopyImage(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_CreateIconIndirect(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetIconInfo(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SetScrollInfo(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetScrollInfo(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_TranslateMDISysAccel(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_DrawEdge(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_DrawFrameControl(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_TrackPopupMenuEx(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_ChildWindowFromPointEx(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_DrawIconEx(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_FlashWindowEx(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_RegCreateKeyExA(void){ return 5; }
static u64 __attribute__((ms_abi)) stub_RegDeleteValueA(void){ return 2; }
static u64 __attribute__((ms_abi)) stub_RegOpenKeyA(void){ return 2; }
static u64 __attribute__((ms_abi)) stub_RegSetValueExA(void){ return 5; }
static u64 __attribute__((ms_abi)) stub_OpenSCManagerA(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_RegSetValueExW(void){ return 5; }
static u64 __attribute__((ms_abi)) stub_RegCreateKeyExW(void){ return 5; }
static u64 __attribute__((ms_abi)) stub_EnumServicesStatusW(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_RegFlushKey(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_CloseServiceHandle(void){ return 1; }
static u64 __attribute__((ms_abi)) stub_CreateFontIndirectA(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_EnumFontFamiliesA(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetCharABCWidthsA(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetTextMetricsA(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetObjectA(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_ExtTextOutA(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_EnumFontFamiliesExW(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetCharABCWidthsW(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetTextExtentExPointW(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetRandomRgn(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_Arc(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_Chord(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_CreateBrushIndirect(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_CreateDIBitmap(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_CreatePenIndirect(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_CreatePatternBrush(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_Ellipse(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_EqualRgn(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_ExcludeClipRect(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_ExtCreateRegion(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_ExtFloodFill(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_FillRgn(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetROP2(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetBitmapBits(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetClipBox(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetClipRgn(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetCurrentObject(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetDIBits(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetMapMode(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetRegionData(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetTextAlign(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetViewportExtEx(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetViewportOrgEx(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetWindowExtEx(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetWindowOrgEx(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_IntersectClipRect(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_LineTo(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_MaskBlt(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_Pie(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_PaintRgn(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_RectInRegion(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_RectVisible(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_RealizePalette(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_RoundRect(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SelectClipRgn(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_ExtSelectClipRgn(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SelectPalette(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SetMapMode(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SetPolyFillMode(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SetROP2(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SetTextCharacterExtra(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SetArcDirection(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_ExtCreatePen(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_MoveToEx(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_CreatePolygonRgn(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_DPtoLP(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_LPtoDP(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_Polygon(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_Polyline(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_PolyBezier(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SetViewportExtEx(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SetViewportOrgEx(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SetWindowExtEx(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SetWindowOrgEx(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_OffsetViewportOrgEx(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SetBrushOrgEx(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetDCOrgEx(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_DragQueryFileA(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_DragQueryFileW(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_ExtractAssociatedIconW(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_DragFinish(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_OleUninitialize(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_CLSIDFromProgID(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetErrorInfo(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_LdrGetProcedureAddress(void){ return 0xC0000139u; }
static u64 __attribute__((ms_abi)) stub_ZwProtectVirtualMemory(void){ return 0xC0000022u; }
/* RtlFormatCurrentUserKeyPath(PUNICODE_STRING out): "\\Registry\\User\\<SID>" in a heap buffer the
 * caller releases with RtlFreeUnicodeString. Callers (Pascal/Delphi runtimes) copy the buffer. */
static u64 __attribute__((ms_abi)) stub_RtlFormatCurrentUserKeyPath(u16 *out)
{
    static const char path[] = "\\Registry\\User\\S-1-5-21-1000-1000-1000-1001";
    if (!out) return 0xC000000Du;                                   /* STATUS_INVALID_PARAMETER */
    size_t n = sizeof(path) - 1;
    u16 *buffer = (u16 *)(uintptr_t)impl_HeapAlloc(1, 0, (n + 1) * 2);
    if (!buffer) return 0xC0000017u;                                /* STATUS_NO_MEMORY */
    for (size_t i = 0; i <= n; ++i) buffer[i] = (u8)path[i];
    out[0] = (u16)(n * 2);
    out[1] = (u16)(n * 2 + 2);
    *(u16 **)(void *)((u8 *)out + 8) = buffer;
    return 0;                                                       /* STATUS_SUCCESS */
}
static u64 __attribute__((ms_abi)) stub_RtlDosPathNameToNtPathName_U(void){ return 0; }

/* SMBIOS ("RSMB") for GetSystemFirmwareTable: BIOS, system and baseboard structures built from /sys/class/dmi/id,
 * which unprivileged users can read (the raw DMI table needs root). Same approach as Wine. */
static size_t beer_smbios_string(char *out, size_t room, const char *field, const char *fallback)
{
    char path[96], value[128] = "";
    snprintf(path, sizeof(path), "/sys/class/dmi/id/%s", field);
    FILE *f = fopen(path, "r");
    if (f) { if (!fgets(value, sizeof(value), f)) value[0] = 0; fclose(f); }
    value[strcspn(value, "\n")] = 0;
    if (!value[0]) snprintf(value, sizeof(value), "%s", fallback);
    size_t n = strlen(value) + 1;
    if (n > room) return 0;
    memcpy(out, value, n);
    return n;
}
static size_t beer_build_smbios(u8 *t, size_t room)
{
    size_t n = 0;
    #define SMB_STR(field, fb) do { size_t k = beer_smbios_string((char *)t + n, room - n, field, fb); if (!k) return 0; n += k; } while (0)
    if (room < 512) return 0;
    /* Type 0: BIOS information */
    u8 *h = t + n; memset(h, 0, 0x18); h[0] = 0; h[1] = 0x18; h[4] = 1; h[5] = 2; h[8] = 3; h[9] = 0xFF;
    h[10] = 0x08; h[0x14] = 0x03; h[0x15] = 0x0D; n += 0x18;
    SMB_STR("bios_vendor", "American Megatrends Inc."); SMB_STR("bios_version", "1.0"); SMB_STR("bios_date", "01/01/2020"); t[n++] = 0;
    /* Type 1: system information (UUID zero: product_uuid is root-only) */
    h = t + n; memset(h, 0, 0x1B); h[0] = 1; h[1] = 0x1B; h[2] = 1; h[4] = 1; h[5] = 2; h[6] = 3; h[7] = 4;
    h[0x18] = 6; h[0x19] = 5; h[0x1A] = 6; n += 0x1B;
    SMB_STR("sys_vendor", "System manufacturer"); SMB_STR("product_name", "System Product Name");
    SMB_STR("product_version", "1.0"); SMB_STR("product_serial", "Default string");
    SMB_STR("product_sku", "Default string"); SMB_STR("product_family", "Default string"); t[n++] = 0;
    /* Type 2: baseboard */
    h = t + n; memset(h, 0, 0x0F); h[0] = 2; h[1] = 0x0F; h[2] = 2; h[4] = 1; h[5] = 2; h[6] = 3; h[7] = 4; h[8] = 5;
    h[9] = 0x09; h[0x0D] = 0x0A; n += 0x0F;
    SMB_STR("board_vendor", "Default string"); SMB_STR("board_name", "Default string"); SMB_STR("board_version", "1.0");
    SMB_STR("board_serial", "Default string"); SMB_STR("board_asset_tag", "Default string"); t[n++] = 0;
    /* Type 127: end of table */
    if (room - n < 6) return 0;
    h = t + n; h[0] = 127; h[1] = 4; h[2] = 3; h[3] = 0; h[4] = 0; h[5] = 0; n += 6;
    #undef SMB_STR
    return n;
}
static u64 __attribute__((ms_abi)) beer_GetSystemFirmwareTable(u32 provider, u32 id, u8 *buffer, u32 size)
{
    (void)id;
    if (provider != 0x52534D42u) { g_last_error = 1; return 0; }   /* only 'RSMB'; ERROR_INVALID_FUNCTION */
    u8 table[2048];
    size_t length = beer_build_smbios(table, sizeof(table));
    u32 needed = (u32)(8 + length);                                 /* RawSMBIOSData header */
    if (!buffer || size < needed) { g_last_error = 122; return needed; }   /* ERROR_INSUFFICIENT_BUFFER */
    buffer[0] = 0; buffer[1] = 2; buffer[2] = 8; buffer[3] = 0;     /* SMBIOS 2.8 */
    u32 len32 = (u32)length;
    memcpy(buffer + 4, &len32, 4);
    memcpy(buffer + 8, table, length);
    g_last_error = 0;
    return needed;
}
static u64 __attribute__((ms_abi)) beer_EnumSystemFirmwareTables(u32 provider, u8 *buffer, u32 size)
{
    if (provider != 0x52534D42u) { g_last_error = 1; return 0; }
    if (buffer && size >= 4) memset(buffer, 0, 4);                  /* one table, id 0 */
    return 4;
}
static u64 __attribute__((ms_abi)) stub_RtlInitString(u16 *d, const char *s){ if(!d) return 0; size_t n=0; if(s) while(s[n]) ++n; d[0]=(u16)n; d[1]=(u16)(s?n+1:0); *(const char **)(void*)((u8*)d+8)=s; return 0; }
static u64 __attribute__((ms_abi)) stub_PathMatchSpecW(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_InitCommonControls(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_ImageList_Create(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_ImageList_Destroy(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_ImageList_GetImageCount(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_ImageList_SetImageCount(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_ImageList_Add(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_ImageList_Replace(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_ImageList_AddMasked(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_ImageList_DrawEx(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_ImageList_DrawIndirect(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_ImageList_Remove(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_ImageList_Copy(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_ImageList_BeginDrag(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_ImageList_EndDrag(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_ImageList_DragEnter(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_ImageList_DragLeave(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_ImageList_DragMove(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_ImageList_DragShowNolock(void){ return 0; }
static u64 __attribute__((ms_abi)) stub__TrackMouseEvent(void){ return 0; }

/* Many entry points below report a fixed result and ignore some parameters by design. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
/* ---- Entry points Where Winds Meet's protection resolves by name (found with BEER_EXTRA_EXPORTS) ---- */
/* Touch input: Beer delivers no WM_TOUCH, so registration succeeds and there is never touch input to read. */
static u64 __attribute__((ms_abi)) beer_RegisterTouchWindow(u64 window, u32 flags)
    { (void)flags; if (!window) { g_last_error = 1400; return 0; } return 1; }      /* ERROR_INVALID_WINDOW_HANDLE */
static u64 __attribute__((ms_abi)) beer_UnregisterTouchWindow(u64 window)
    { if (!window) { g_last_error = 1400; return 0; } return 1; }
static u64 __attribute__((ms_abi)) beer_GetTouchInputInfo(u64 input, u32 count, void *inputs, s32 size)
    { (void)input; (void)count; (void)inputs; (void)size; g_last_error = 6; return 0; }   /* ERROR_INVALID_HANDLE */
static u64 __attribute__((ms_abi)) beer_CloseTouchInputHandle(u64 input)
    { (void)input; g_last_error = 6; return 0; }
/* No taskbar to flash: the window was not active-highlighted before (FALSE). */
static u64 __attribute__((ms_abi)) beer_FlashWindow(u64 window, u32 invert) { (void)window; (void)invert; return 0; }
/* Window-class fields are not stored separately from the window; there is no previous value to return. */
static u64 __attribute__((ms_abi)) beer_SetClassLongPtrW(u64 window, s32 index, u64 value)
    { (void)window; (void)index; (void)value; return 0; }
/* Beer shows the host pointer for every cursor handle, as LoadCursorW does. */
static u64 __attribute__((ms_abi)) beer_LoadCursorFromFileW(const u16 *path)
    { if (!path) { g_last_error = 2; return 0; } return impl_LoadCursorW(0, 32512 /* IDC_ARROW */); }
/* WM_MOUSELEAVE/HOVER tracking is accepted; Beer's input path does not generate those messages. */
static u64 __attribute__((ms_abi)) beer_TrackMouseEvent(u32 *event)
    { if (!event || event[0] < 24) { g_last_error = 87; return 0; } return 1; }       /* cbSize of TRACKMOUSEEVENT */
/* Layered-window alpha/colour keys have no effect on Beer's presented surface. */
static u64 __attribute__((ms_abi)) beer_SetLayeredWindowAttributes(u64 window, u32 key, u8 alpha, u32 flags)
    { (void)key; (void)alpha; (void)flags; if (!window) { g_last_error = 1400; return 0; } return 1; }

/* Winsock: Beer passes sockets through to the host (see impl_getsockname). */
static u64 __attribute__((ms_abi)) beer_getpeername(u64 socket_handle, void *address, s32 *address_length)
{
    if (!winsock_ready()) return (u32)-1;
    if (!address || !address_length || *address_length < 0) { beer_wsa_set_error(10014); return (u32)-1; }
    socklen_t length = (socklen_t)*address_length;
    if (getpeername((int)socket_handle, (struct sockaddr *)address, &length) != 0) {
        beer_wsa_set_error(winsock_error_from_errno(errno));
        return (u32)-1;
    }
    *address_length = (s32)length;
    beer_wsa_set_error(0);
    return 0;
}
static u64 __attribute__((ms_abi)) beer_inet_addr(const char *text)
{
    if (!text) return 0xFFFFFFFFu;                                                 /* INADDR_NONE */
    return (u64)(u32)inet_addr(text);
}
static u64 __attribute__((ms_abi)) beer_inet_ntoa(u32 address)
{
    static __thread char text[16];                                                 /* per thread, as Winsock's */
    const u8 *b = (const u8 *)&address;
    snprintf(text, sizeof(text), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return (u64)(uintptr_t)text;
}
/* getnameinfo: Winsock's NI_* flags equal glibc's; only the AF_INET6 number differs (23 vs 10). */
static u64 __attribute__((ms_abi)) beer_getnameinfo(const u8 *address, s32 address_length, char *host, u32 host_length,
                                                    char *service, u32 service_length, s32 flags)
{
    if (!address || address_length < 2) return 10014;                              /* WSAEFAULT */
    struct sockaddr_storage copy;
    memset(&copy, 0, sizeof(copy));
    memcpy(&copy, address, (size_t)address_length < sizeof(copy) ? (size_t)address_length : sizeof(copy));
    u16 family = *(const u16 *)address;
    if (family == 23) copy.ss_family = AF_INET6;
    else if (family != 2) return 10047;                                            /* WSAEAFNOSUPPORT */
    int result = getnameinfo((struct sockaddr *)&copy,
                             copy.ss_family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in),
                             host, host_length, service, service_length, flags);
    switch (result) {
    case 0: return 0;
    case EAI_AGAIN: return 11002;                                                  /* WSATRY_AGAIN */
    case EAI_NONAME: return 11001;                                                 /* WSAHOST_NOT_FOUND */
    case EAI_FAMILY: return 10047;
    case EAI_MEMORY: return 8;                                                     /* WSA_NOT_ENOUGH_MEMORY */
    case EAI_OVERFLOW: return 10014;
    default: return 11003;                                                         /* WSANO_RECOVERY */
    }
}
/* WSASendTo, sent synchronously. An OVERLAPPED request completes at once: its status and byte count are stored and
 * its event set, and the call returns 0 as Windows does for an immediately completed overlapped send. Completion
 * routines (APC delivery) are not supported. */
static u64 __attribute__((ms_abi)) beer_WSASendTo(u64 socket_handle, const u8 *buffers, u32 buffer_count, u32 *sent_bytes,
                                                  u32 flags, const void *destination, s32 destination_length,
                                                  u64 *overlapped, u64 completion)
{
    if (!winsock_ready()) return (u32)-1;
    if ((!buffers && buffer_count) || buffer_count > 1024) { beer_wsa_set_error(10014); return (u32)-1; }
    if (completion) { beer_wsa_set_error(10045); return (u32)-1; }                 /* WSAEOPNOTSUPP */
    if (overlapped && buffer_count <= 16) {                                        /* completes asynchronously if needed */
        WsaAsyncOp request;
        memset(&request, 0, sizeof(request));
        request.kind = WSA_ASYNC_SEND;
        request.socket = socket_handle;
        request.flags = (int)flags;
        request.overlapped = overlapped;
        request.vector_count = (int)buffer_count;
        for (u32 i = 0; i < buffer_count; ++i) {
            request.vectors[i].iov_len = *(const u32 *)(buffers + i * 16);
            request.vectors[i].iov_base = *(void *const *)(buffers + i * 16 + 8);
        }
        if (destination && destination_length > 0 && (size_t)destination_length <= sizeof(request.destination)) {
            memcpy(&request.destination, destination, (size_t)destination_length);
            request.destination_length = (socklen_t)destination_length;
        }
        u64 transferred = 0;
        u64 result = beer_wsa_overlapped(&request, &transferred);
        if ((u32)result == 0 && sent_bytes) *sent_bytes = (u32)transferred;
        return result;
    }
    struct iovec vectors[1024];
    for (u32 i = 0; i < buffer_count; ++i) {                                       /* WSABUF { ULONG len; CHAR *buf; } */
        vectors[i].iov_len = *(const u32 *)(buffers + i * 16);
        vectors[i].iov_base = *(void *const *)(buffers + i * 16 + 8);
    }
    struct msghdr message;
    memset(&message, 0, sizeof(message));
    message.msg_name = (void *)destination;
    message.msg_namelen = destination ? (socklen_t)destination_length : 0;
    message.msg_iov = vectors;
    message.msg_iovlen = buffer_count;
    ssize_t sent = sendmsg((int)socket_handle, &message, (int)flags | MSG_NOSIGNAL);
    if (sent < 0) {
        beer_wsa_set_error(winsock_error_from_errno(errno));
        return (u32)-1;
    }
    if (sent_bytes) *sent_bytes = (u32)sent;
    if (overlapped) {                                                              /* Internal, InternalHigh, .., hEvent */
        overlapped[0] = 0;
        overlapped[1] = (u64)sent;
        if (overlapped[3]) impl_SetEvent(overlapped[3]);
    }
    beer_wsa_set_error(0);
    return 0;
}

/* ---- Second set resolved by Where Winds Meet's protection ---- */
static void beer_unsupported_once(const char *what)
{
    static char reported[32][48];
    for (int i = 0; i < 32; ++i) {
        if (!strcmp(reported[i], what)) return;
        if (!reported[i][0]) { snprintf(reported[i], sizeof(reported[i]), "%s", what); break; }
    }
    fprintf(stderr, "[UNSUPPORTED] %s is not supported by Beer; the call fails\n", what);
}

/* Winsock. WSABUF arrays are { ULONG len; CHAR *buf; } (16 bytes on x64). */
static int beer_wsabuf_vectors(const u8 *buffers, u32 count, struct iovec *vectors)
{
    if ((!buffers && count) || count > 1024) return 0;
    for (u32 i = 0; i < count; ++i) {
        vectors[i].iov_len = *(const u32 *)(buffers + i * 16);
        vectors[i].iov_base = *(void *const *)(buffers + i * 16 + 8);
    }
    return 1;
}
static void beer_wsa_complete(u64 *overlapped, u64 bytes)
{
    if (!overlapped) return;
    overlapped[0] = 0;                              /* Internal: STATUS_SUCCESS */
    overlapped[1] = bytes;                          /* InternalHigh: bytes transferred */
    if (overlapped[3]) impl_SetEvent(overlapped[3]);
}
static u64 __attribute__((ms_abi)) beer_WSASend(u64 socket_handle, const u8 *buffers, u32 count, u32 *sent,
                                                u32 flags, u64 *overlapped, u64 completion)
{
    return beer_WSASendTo(socket_handle, buffers, count, sent, flags, NULL, 0, overlapped, completion);
}
/* Receives are performed at once. An overlapped receive with no data waiting cannot be completed later
 * (Beer has no asynchronous socket completion), so it fails with WSAEOPNOTSUPP instead of WSA_IO_PENDING. */
static u64 beer_wsa_receive(u64 socket_handle, const u8 *buffers, u32 count, u32 *received, u32 *flags,
                            void *from, s32 *from_length, u64 *overlapped, u64 completion)
{
    if (!winsock_ready()) return (u32)-1;
    struct iovec vectors[1024];
    if (!beer_wsabuf_vectors(buffers, count, vectors)) { beer_wsa_set_error(10014); return (u32)-1; }
    if (completion) { beer_unsupported_once("WSARecv completion routines"); beer_wsa_set_error(10045); return (u32)-1; }
    if (overlapped && count <= 16) {                                               /* completes asynchronously if needed */
        WsaAsyncOp request;
        memset(&request, 0, sizeof(request));
        request.kind = WSA_ASYNC_RECV;
        request.socket = socket_handle;
        request.flags = flags ? (int)*flags : 0;
        request.overlapped = overlapped;
        request.flags_out = flags;
        request.address = from;
        request.address_length = from_length;
        request.vector_count = (int)count;
        memcpy(request.vectors, vectors, sizeof(struct iovec) * count);
        u64 transferred = 0;
        u64 result = beer_wsa_overlapped(&request, &transferred);
        if ((u32)result == 0 && received) *received = (u32)transferred;
        return result;
    }
    struct msghdr message;
    memset(&message, 0, sizeof(message));
    message.msg_name = from;
    message.msg_namelen = from && from_length ? (socklen_t)*from_length : 0;
    message.msg_iov = vectors;
    message.msg_iovlen = count;
    int host_flags = flags ? (int)(*flags & 0x3) : 0;          /* MSG_OOB, MSG_PEEK have the same values */
    if (overlapped) host_flags |= MSG_DONTWAIT;
    ssize_t got = recvmsg((int)socket_handle, &message, host_flags);
    if (got < 0) {
        if (overlapped && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            beer_unsupported_once("overlapped socket receive without waiting data");
            beer_wsa_set_error(10045);
        } else {
            beer_wsa_set_error(winsock_error_from_errno(errno));
        }
        return (u32)-1;
    }
    if (!(host_flags & 2)) beer_net_trace_rx(socket_handle, vectors, (int)count, (u64)got, "WSARecv");
    if (received) *received = (u32)got;
    if (flags) *flags = (message.msg_flags & MSG_TRUNC) ? 0x2 /* MSG_PARTIAL */ : 0;
    if (from && from_length) *from_length = (s32)message.msg_namelen;
    beer_wsa_complete(overlapped, (u64)got);
    beer_wsa_set_error(0);
    return 0;
}
static u64 __attribute__((ms_abi)) beer_WSARecv(u64 socket_handle, const u8 *buffers, u32 count, u32 *received,
                                                u32 *flags, u64 *overlapped, u64 completion)
    { return beer_wsa_receive(socket_handle, buffers, count, received, flags, NULL, NULL, overlapped, completion); }
static u64 __attribute__((ms_abi)) beer_WSARecvFrom(u64 socket_handle, const u8 *buffers, u32 count, u32 *received,
                                                    u32 *flags, void *from, s32 *from_length, u64 *overlapped,
                                                    u64 completion)
    { return beer_wsa_receive(socket_handle, buffers, count, received, flags, from, from_length, overlapped, completion); }
static u64 __attribute__((ms_abi)) beer_WSASocketW(s32 family, s32 type, s32 protocol, const u8 *info, u32 group, u32 flags)
{
    (void)group; (void)flags;                       /* WSA_FLAG_OVERLAPPED: Beer sockets serve both kinds of call */
    if (info && family == -1) {                     /* FROM_PROTOCOL_INFO: WSAPROTOCOL_INFOW iAddressFamily.. */
        family = *(const s32 *)(info + 0x4c); type = *(const s32 *)(info + 0x58); protocol = *(const s32 *)(info + 0x5c);
    }
    return impl_socket(family, type, protocol);
}
static u64 __attribute__((ms_abi)) beer_WSADuplicateSocketW(u64 socket_handle, u32 process, void *info)
    { (void)socket_handle; (void)process; (void)info; beer_unsupported_once("WSADuplicateSocketW"); beer_wsa_set_error(10045); return (u32)-1; }
static u64 __attribute__((ms_abi)) beer_WSASendDisconnect(u64 socket_handle, const void *data)
{
    (void)data;
    if (!winsock_ready()) return (u32)-1;
    if (shutdown((int)socket_handle, SHUT_WR) != 0) { beer_wsa_set_error(winsock_error_from_errno(errno)); return (u32)-1; }
    beer_wsa_set_error(0);
    return 0;
}
/* "a.b.c.d[:port]" for AF_INET (2); "addr" or "[addr]:port" for AF_INET6 (23). */
static u64 __attribute__((ms_abi)) beer_WSAStringToAddressW(const u16 *text, s32 family, const void *info, u8 *address,
                                                            s32 *length)
{
    (void)info;
    if (!winsock_ready()) return (u32)-1;
    if (!text || !length) { beer_wsa_set_error(10014); return (u32)-1; }
    char narrow[128];
    size_t n = 0;
    while (text[n] && n + 1 < sizeof(narrow)) { narrow[n] = (char)(text[n] < 0x80 ? text[n] : '?'); ++n; }
    narrow[n] = 0;
    const s32 needed = family == 23 ? 28 : 16;
    if (family != 2 && family != 23) { beer_wsa_set_error(10047); return (u32)-1; }
    if (!address || *length < needed) { *length = needed; beer_wsa_set_error(10014); return (u32)-1; }
    char host[128];
    unsigned port = 0;
    const char *port_text = NULL;
    if (family == 23 && narrow[0] == '[') {
        const char *close = strchr(narrow, ']');
        if (!close) { beer_wsa_set_error(10022); return (u32)-1; }
        snprintf(host, sizeof(host), "%.*s", (int)(close - narrow - 1), narrow + 1);
        if (close[1] == ':') port_text = close + 2;
    } else {
        snprintf(host, sizeof(host), "%s", narrow);
        char *colon = family == 2 ? strchr(host, ':') : NULL;
        if (colon) { *colon = 0; port_text = colon + 1; }
    }
    if (port_text) {
        char *end = NULL;
        unsigned long value = strtoul(port_text, &end, 10);
        if (!*port_text || *end || value > 65535) { beer_wsa_set_error(10022); return (u32)-1; }
        port = (unsigned)value;
    }
    memset(address, 0, (size_t)needed);
    *(u16 *)address = (u16)family;
    *(u16 *)(address + 2) = htons((u16)port);
    if (inet_pton(family == 23 ? AF_INET6 : AF_INET, host, address + (family == 23 ? 8 : 4)) != 1) {
        beer_wsa_set_error(10022);
        return (u32)-1;
    }
    *length = needed;
    beer_wsa_set_error(0);
    return 0;
}
static u64 __attribute__((ms_abi)) beer_WSAAddressToStringW(const u8 *address, u32 address_length, const void *info,
                                                            u16 *text, u32 *text_length)
{
    (void)info;
    if (!winsock_ready()) return (u32)-1;
    if (!address || !text_length || address_length < 16) { beer_wsa_set_error(10014); return (u32)-1; }
    const u16 family = *(const u16 *)address;
    const unsigned port = ntohs(*(const u16 *)(address + 2));
    char host[64], formatted[96];
    if (family == 2) {
        inet_ntop(AF_INET, address + 4, host, sizeof(host));
        if (port) snprintf(formatted, sizeof(formatted), "%s:%u", host, port);
        else snprintf(formatted, sizeof(formatted), "%s", host);
    } else if (family == 23 && address_length >= 28) {
        inet_ntop(AF_INET6, address + 8, host, sizeof(host));
        if (port) snprintf(formatted, sizeof(formatted), "[%s]:%u", host, port);
        else snprintf(formatted, sizeof(formatted), "%s", host);
    } else {
        beer_wsa_set_error(10047);
        return (u32)-1;
    }
    const u32 needed = (u32)strlen(formatted) + 1;
    if (!text || *text_length < needed) { *text_length = needed; beer_wsa_set_error(10014); return (u32)-1; }
    for (u32 i = 0; i < needed; ++i) text[i] = (u8)formatted[i];
    *text_length = needed;
    beer_wsa_set_error(0);
    return 0;
}
/* MSWSOCK: asynchronous accept needs completion delivery that Beer does not have. */
static u64 __attribute__((ms_abi)) beer_AcceptEx(u64 listen_socket, u64 accept_socket, void *buffer, u32 receive_length,
                                                 u32 local_length, u32 remote_length, u32 *received, void *overlapped)
{
    (void)listen_socket; (void)accept_socket; (void)buffer; (void)receive_length; (void)local_length;
    (void)remote_length; (void)received; (void)overlapped;
    beer_unsupported_once("AcceptEx");
    beer_wsa_set_error(10045);
    return 0;
}
static u64 __attribute__((ms_abi)) beer_GetAcceptExSockaddrs(void *buffer, u32 receive_length, u32 local_length,
                                                             u32 remote_length, u64 *local, s32 *local_size,
                                                             u64 *remote, s32 *remote_size)
{
    (void)buffer; (void)receive_length; (void)local_length; (void)remote_length;
    if (local) *local = 0;
    if (local_size) *local_size = 0;
    if (remote) *remote = 0;
    if (remote_size) *remote_size = 0;
    return 0;
}

/* Kernel32 */
static u64 __attribute__((ms_abi)) beer_CreateFiber(u64 stack_size, u64 start, u64 parameter)
    { (void)stack_size; (void)start; (void)parameter; beer_unsupported_once("CreateFiber"); g_last_error = 50; return 0; }
static u64 __attribute__((ms_abi)) beer_ConvertThreadToFiber(u64 parameter)
    { (void)parameter; beer_unsupported_once("ConvertThreadToFiber"); g_last_error = 50; return 0; }
static u64 __attribute__((ms_abi)) beer_GetNumaNodeProcessorMask(u8 node, u64 *mask)
{
    if (!mask) { g_last_error = 87; return 0; }
    if (node != 0) { g_last_error = 87; return 0; }                 /* one NUMA node */
    long processors = sysconf(_SC_NPROCESSORS_ONLN);
    *mask = processors >= 64 ? ~0ull : ((1ull << (processors > 0 ? processors : 1)) - 1);
    return 1;
}
static u64 __attribute__((ms_abi)) beer_GetLargePageMinimum(void) { return 0x200000; }
/* Job objects: the handle is accepted by CloseHandle and the job calls; Beer applies no job limits. */
static u64 __attribute__((ms_abi)) beer_CreateJobObjectW(u64 attributes, const u16 *name)
    { (void)name; return impl_CreateEventW(attributes, 1, 0, 0); }
static u64 __attribute__((ms_abi)) beer_AssignProcessToJobObject(u64 job, u64 process)
    { (void)process; if (!job) { g_last_error = 6; return 0; } return 1; }
static u64 __attribute__((ms_abi)) beer_SetInformationJobObject(u64 job, u32 info_class, void *info, u32 length)
    { (void)info_class; (void)info; (void)length; if (!job) { g_last_error = 6; return 0; } return 1; }
static u64 __attribute__((ms_abi)) beer_ReOpenFile(u64 file, u32 access, u32 share, u32 flags)
{
    (void)share; (void)flags;
    u64 duplicate = 0;
    if (!impl_DuplicateHandle((u64)-1, file, (u64)-1, &duplicate, access, 0, access ? 0 : 2 /* SAME_ACCESS */))
        return (u64)-1;                                               /* INVALID_HANDLE_VALUE */
    return duplicate;
}
/* Beer's file system has no 8.3 short names: both conversions return the existing path unchanged. */
static u64 beer_path_same_form(const u16 *source, u16 *destination, u32 capacity)
{
    if (!source) { g_last_error = 87; return 0; }
    if (impl_GetFileAttributesW(source) == 0xFFFFFFFFu) { g_last_error = 2; return 0; }
    u32 length = 0;
    while (source[length]) ++length;
    if (!destination || capacity <= length) return length + 1;
    if (destination != source) memmove(destination, source, (length + 1) * sizeof(u16));
    return length;
}
static u64 __attribute__((ms_abi)) beer_GetLongPathNameW(const u16 *source, u16 *destination, u32 capacity)
    { return beer_path_same_form(source, destination, capacity); }
static u64 __attribute__((ms_abi)) beer_GetShortPathNameW(const u16 *source, u16 *destination, u32 capacity)
    { return beer_path_same_form(source, destination, capacity); }
static u64 __attribute__((ms_abi)) beer_ReadDirectoryChangesW(u64 directory, void *buffer, u32 length, u32 subtree,
                                                              u32 filter, u32 *returned, void *overlapped, u64 completion)
{
    (void)directory; (void)buffer; (void)length; (void)subtree; (void)filter; (void)returned; (void)overlapped; (void)completion;
    beer_unsupported_once("ReadDirectoryChangesW");
    g_last_error = 50;                                                /* ERROR_NOT_SUPPORTED */
    return 0;
}
static u64 __attribute__((ms_abi)) beer_SetPriorityClass(u64 process, u32 priority)
    { (void)process; (void)priority; return 1; }
static u64 __attribute__((ms_abi)) beer_GetPriorityClass(u64 process)
    { (void)process; return 0x20; }                                   /* NORMAL_PRIORITY_CLASS */
static u64 __attribute__((ms_abi)) beer_SetThreadPriorityBoost(u64 thread, u32 disable)
    { (void)thread; (void)disable; return 1; }
/* IO_COUNTERS from /proc/self/io: read/write/other operation counts and byte counts. */
static u64 __attribute__((ms_abi)) beer_GetProcessIoCounters(u64 process, u64 *counters)
{
    (void)process;
    if (!counters) { g_last_error = 87; return 0; }
    memset(counters, 0, 6 * sizeof(u64));
    FILE *io = fopen("/proc/self/io", "r");
    char line[128];
    unsigned long long value;
    while (io && fgets(line, sizeof(line), io)) {
        if (sscanf(line, "syscr: %llu", &value) == 1) counters[0] = value;
        else if (sscanf(line, "syscw: %llu", &value) == 1) counters[1] = value;
        else if (sscanf(line, "rchar: %llu", &value) == 1) counters[3] = value;
        else if (sscanf(line, "wchar: %llu", &value) == 1) counters[4] = value;
    }
    if (io) fclose(io);
    return 1;
}
/* Console calls in a process without a console fail with ERROR_INVALID_HANDLE, as in a Windows GUI process. */
#define BEER_NO_CONSOLE(name, ...) \
    static u64 __attribute__((ms_abi)) beer_##name(__VA_ARGS__) { g_last_error = 6; return 0; }
BEER_NO_CONSOLE(GetConsoleTitleW, u16 *title, u32 size)
BEER_NO_CONSOLE(FillConsoleOutputCharacterW, u64 output, u16 character, u32 length, u32 coord, u32 *written)
BEER_NO_CONSOLE(FillConsoleOutputAttribute, u64 output, u16 attribute, u32 length, u32 coord, u32 *written)
BEER_NO_CONSOLE(GetConsoleCursorInfo, u64 output, void *info)
BEER_NO_CONSOLE(SetConsoleCursorInfo, u64 output, const void *info)
BEER_NO_CONSOLE(SetConsoleCursorPosition, u64 output, u32 coord)
BEER_NO_CONSOLE(WriteConsoleInputW, u64 input, const void *records, u32 length, u32 *written)
BEER_NO_CONSOLE(SetConsoleScreenBufferSize, u64 output, u32 size)
BEER_NO_CONSOLE(SetConsoleWindowInfo, u64 output, u32 absolute, const void *window)
#undef BEER_NO_CONSOLE
/* SYSTEMTIME <-> seconds, for the time-zone conversion below. */
static int beer_systemtime_to_unix(const u16 *st, s64 *seconds)
{
    struct tm tm = {0};
    tm.tm_year = st[0] - 1900; tm.tm_mon = st[1] - 1; tm.tm_mday = st[3];
    tm.tm_hour = st[4]; tm.tm_min = st[5]; tm.tm_sec = st[6];
    if (st[1] < 1 || st[1] > 12 || st[3] < 1 || st[3] > 31) return 0;
    *seconds = (s64)timegm(&tm);
    return 1;
}
static void beer_unix_to_systemtime(s64 seconds, u16 milliseconds, u16 *st)
{
    time_t t = (time_t)seconds;
    struct tm tm;
    gmtime_r(&t, &tm);
    st[0] = (u16)(tm.tm_year + 1900); st[1] = (u16)(tm.tm_mon + 1); st[2] = (u16)tm.tm_wday; st[3] = (u16)tm.tm_mday;
    st[4] = (u16)tm.tm_hour; st[5] = (u16)tm.tm_min; st[6] = (u16)tm.tm_sec; st[7] = milliseconds;
}
/* UTC = local + Bias (+ StandardBias). Daylight rules are not evaluated, as in Beer's reverse conversion. */
static u64 __attribute__((ms_abi)) beer_TzSpecificLocalTimeToSystemTime(const u8 *zone, const u16 *local, u16 *utc)
{
    if (!local || !utc) { g_last_error = 87; return 0; }
    WIN_TIME_ZONE_INFORMATION current;
    if (!zone) { impl_GetTimeZoneInformation(&current); zone = (const u8 *)&current; }
    s64 seconds;
    if (!beer_systemtime_to_unix(local, &seconds)) { g_last_error = 87; return 0; }
    seconds += 60 * (s64)(*(const s32 *)zone + *(const s32 *)(zone + 0x54));
    beer_unix_to_systemtime(seconds, local[7], utc);
    return 1;
}
static u64 __attribute__((ms_abi)) beer_GetFinalPathNameByHandleA(u64 file, char *path, u32 capacity, u32 flags)
{
    char guest[1024], host[1024];
    if (!fh_paths(file, guest, sizeof(guest), host, sizeof(host))) { g_last_error = 6; return 0; }
    char formatted[1100];
    if (flags & 0x4) snprintf(formatted, sizeof(formatted), "%s", guest + (guest[1] == ':' ? 2 : 0));  /* VOLUME_NAME_NONE */
    else snprintf(formatted, sizeof(formatted), "\\\\?\\%s", guest);
    for (char *c = formatted; *c; ++c) if (*c == '/') *c = '\\';
    u32 length = (u32)strlen(formatted);
    if (!path || capacity <= length) return length + 1;
    memcpy(path, formatted, length + 1);
    return length;
}
static u64 __attribute__((ms_abi)) beer_CreateHardLinkA(const char *link, const char *target, void *attributes)
    { (void)link; (void)target; (void)attributes; beer_unsupported_once("CreateHardLinkA"); g_last_error = 50; return 0; }
static u64 __attribute__((ms_abi)) beer_CreateSymbolicLinkA(const char *link, const char *target, u32 flags)
{
    if (!link || !target) { g_last_error = 87; return 0; }
    u16 wide_link[1024], wide_target[1024];
    size_t i = 0;
    for (; link[i] && i + 1 < 1024; ++i) wide_link[i] = (u8)link[i];
    wide_link[i] = 0;
    for (i = 0; target[i] && i + 1 < 1024; ++i) wide_target[i] = (u8)target[i];
    wide_target[i] = 0;
    return impl_CreateSymbolicLinkW((u64)(uintptr_t)wide_link, (u64)(uintptr_t)wide_target, flags);
}
static u64 __attribute__((ms_abi)) beer_GetNamedPipeHandleStateW(u64 pipe, u32 *state, u32 *instances, u32 *max_count,
                                                                 u32 *timeout, u16 *user, u32 user_size)
{
    (void)pipe; (void)state; (void)instances; (void)max_count; (void)timeout; (void)user; (void)user_size;
    g_last_error = 6;
    return 0;
}
static u64 __attribute__((ms_abi)) beer_CancelSynchronousIo(u64 thread)
    { (void)thread; g_last_error = 1168; return 0; }                  /* ERROR_NOT_FOUND: no I/O to cancel */
static int beer_apc_queue(u64 thread, u64 routine, u64 argument1, u64 argument2, u64 argument3, int native);
static u64 __attribute__((ms_abi)) beer_QueueUserAPC(u64 function, u64 thread, u64 data)
{
    if (!beer_apc_queue(thread, function, data, 0, 0, 0)) { g_last_error = 6; return 0; }   /* ERROR_INVALID_HANDLE */
    return 1;
}
static u64 __attribute__((ms_abi)) beer_NtQueueApcThread(u64 thread, u64 routine, u64 argument1, u64 argument2, u64 argument3)
    { return beer_apc_queue(thread, routine, argument1, argument2, argument3, 1) ? 0 : 0xC0000008u; }   /* STATUS_INVALID_HANDLE */
static u64 __attribute__((ms_abi)) beer_NtTestAlert(void) { beer_apc_run(); return 0; }
static u64 __attribute__((ms_abi)) beer_InitOnceInitialize(u64 *once) { if (once) *once = 0; return 0; }
static u64 __attribute__((ms_abi)) beer_GetFileAttributesExA(const char *name, u32 level, void *data)
{
    if (!name) { g_last_error = 87; return 0; }
    u16 wide[1024];
    size_t i = 0;
    for (; name[i] && i + 1 < 1024; ++i) wide[i] = (u8)name[i];
    wide[i] = 0;
    return beer_GetFileAttributesExW(wide, level, data);
}
/* The Ex forms on another process are not supported; on this process they are the plain calls. */
static u64 __attribute__((ms_abi)) beer_VirtualQueryEx(u64 process, void *address, BeerMemoryBasicInformation *info, u64 size)
    { if (process != (u64)-1) { g_last_error = 5; return 0; } return impl_VirtualQuery(address, info, size); }
static u64 __attribute__((ms_abi)) beer_VirtualFreeEx(u64 process, void *address, u64 size, u32 type)
    { if (process != (u64)-1) { g_last_error = 5; return 0; } return impl_VirtualFree(address, size, type); }
/* One geographic location (the United States, GEOID 244), matching Beer's en-US locale. */
static u64 __attribute__((ms_abi)) beer_GetUserGeoID(u32 geo_class) { (void)geo_class; return 244; }
static u64 __attribute__((ms_abi)) beer_GetGeoInfoW(u32 location, u32 type, u16 *data, s32 capacity, u16 language)
{
    (void)language;
    const char *value = NULL;
    if (location == 244) {
        switch (type) {
        case 1: value = "244"; break;                  /* GEO_NATION */
        case 4: value = "US"; break;                   /* GEO_ISO2 */
        case 5: value = "USA"; break;                  /* GEO_ISO3 */
        case 6: value = "1"; break;                    /* GEO_RFC1766... GEO_LCID handled as en-US */
        case 8: value = "United States"; break;        /* GEO_FRIENDLYNAME */
        case 9: value = "United States"; break;        /* GEO_OFFICIALNAME */
        default: break;
        }
    }
    if (!value) { g_last_error = 87; return 0; }
    s32 needed = (s32)strlen(value) + 1;
    if (!capacity) return (u64)needed;
    if (!data || capacity < needed) { g_last_error = 122; return 0; }  /* ERROR_INSUFFICIENT_BUFFER */
    for (s32 i = 0; i < needed; ++i) data[i] = (u8)value[i];
    return (u64)needed;
}
/* Global atoms: integer atoms ("#n") are returned as is; string atoms get 0xC000.. values per distinct name. */
static u64 __attribute__((ms_abi)) beer_GlobalAddAtomW(const u16 *name)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static u16 names[256][64];
    static int count;
    if (!name) { g_last_error = 87; return 0; }
    if ((uintptr_t)name < 0x10000) return (u64)(uintptr_t)name;      /* MAKEINTATOM */
    if (name[0] == '#') {
        unsigned value = 0;
        for (const u16 *c = name + 1; *c >= '0' && *c <= '9'; ++c) value = value * 10 + (*c - '0');
        return value && value < 0xC000 ? value : 0;
    }
    pthread_mutex_lock(&lock);
    u64 atom = 0;
    for (int i = 0; i < count && !atom; ++i) {
        int same = 1;
        for (int k = 0; k < 64 && same; ++k) {
            u16 a = names[i][k], b = name[k];
            if (a >= 'a' && a <= 'z') a -= 32;
            if (b >= 'a' && b <= 'z') b -= 32;
            if (a != b) same = 0;
            if (!a || !b) break;
        }
        if (same) atom = 0xC000u + (u64)i;
    }
    if (!atom && count < 256) {
        for (int k = 0; k < 63 && name[k]; ++k) names[count][k] = name[k];
        atom = 0xC000u + (u64)count++;
    }
    pthread_mutex_unlock(&lock);
    if (!atom) g_last_error = 8;
    return atom;
}

/* IMM32: no input method is installed. Contexts exist so callers can associate and release them, but there is
 * never a composition, conversion mode or open IME. */
static u64 __attribute__((ms_abi)) beer_ImmGetDescriptionW(u64 layout, u16 *text, u32 length)
    { (void)layout; if (text && length) text[0] = 0; return 0; }
static u64 __attribute__((ms_abi)) beer_ImmCreateContext(void)
    { static _Atomic u64 next = 0x10; return 0x494D4300ull + atomic_fetch_add(&next, 1); }   /* opaque HIMC */
static u64 __attribute__((ms_abi)) beer_ImmGetContext(u64 window) { (void)window; return 0; }
static u64 __attribute__((ms_abi)) beer_ImmReleaseContext(u64 window, u64 context) { (void)window; (void)context; return 1; }
static u64 __attribute__((ms_abi)) beer_ImmAssociateContext(u64 window, u64 context) { (void)window; (void)context; return 0; }
static u64 __attribute__((ms_abi)) beer_ImmGetCompositionStringW(u64 context, u32 index, void *buffer, u32 length)
    { (void)context; (void)index; (void)buffer; (void)length; return 0; }
static u64 __attribute__((ms_abi)) beer_ImmGetConversionStatus(u64 context, u32 *conversion, u32 *sentence)
{
    if (!context) return 0;
    if (conversion) *conversion = 0;
    if (sentence) *sentence = 0;
    return 1;
}
static u64 __attribute__((ms_abi)) beer_ImmSetCompositionWindow(u64 context, const void *form) { (void)form; return context != 0; }
static u64 __attribute__((ms_abi)) beer_ImmNotifyIME(u64 context, u32 action, u32 index, u32 value)
    { (void)context; (void)action; (void)index; (void)value; return 0; }
static u64 __attribute__((ms_abi)) beer_ImmSetConversionStatus(u64 context, u32 conversion, u32 sentence)
    { (void)conversion; (void)sentence; return context != 0; }
static u64 __attribute__((ms_abi)) beer_ImmGetOpenStatus(u64 context) { (void)context; return 0; }
static u64 __attribute__((ms_abi)) beer_ImmSetOpenStatus(u64 context, u32 open) { (void)open; return context != 0; }

/* SHLWAPI */
static u64 __attribute__((ms_abi)) beer_PathStripPathW(u16 *path)
{
    if (!path) return 0;
    u16 *name = path;
    for (u16 *c = path; *c; ++c)
        if ((*c == '\\' || *c == '/' || *c == ':') && c[1]) name = c + 1;
    if (name != path) {
        size_t length = 0;
        while (name[length]) ++length;
        memmove(path, name, (length + 1) * sizeof(u16));
    }
    return 0;
}
static u64 __attribute__((ms_abi)) beer_PathRemoveExtensionW(u16 *path)
{
    if (!path) return 0;
    u16 *dot = NULL;
    for (u16 *c = path; *c; ++c) {
        if (*c == '\\' || *c == '/' || *c == ' ') dot = NULL;
        else if (*c == '.') dot = c;
    }
    if (dot) *dot = 0;
    return 0;
}
/* Beer's registry view is read-only: there is no key to delete. */
static u64 __attribute__((ms_abi)) beer_SHDeleteKeyW(u64 key, const u16 *subkey) { (void)key; (void)subkey; return 2; }

/* ---- Third set resolved by Where Winds Meet's protection ---- */
/* WTSAPI32: there are no other sessions, so no session change is ever posted. */
static u64 __attribute__((ms_abi)) beer_WTSRegisterSessionNotification(u64 window, u32 flags)
    { (void)flags; if (!window) { g_last_error = 1400; return 0; } return 1; }

/* WINMM joystick API: the driver supports 16 joysticks and none is attached. */
#define BEER_JOYERR_PARMS     165u
#define BEER_JOYERR_UNPLUGGED 167u
static u64 __attribute__((ms_abi)) beer_joyGetNumDevs(void) { return 16; }
static u64 __attribute__((ms_abi)) beer_joyGetPos(u32 id, void *info)
    { (void)info; return id < 16 ? BEER_JOYERR_UNPLUGGED : BEER_JOYERR_PARMS; }
static u64 __attribute__((ms_abi)) beer_joyGetDevCapsW(u64 id, void *caps, u32 size)
    { (void)caps; (void)size; return id < 16 ? BEER_JOYERR_UNPLUGGED : BEER_JOYERR_PARMS; }
static u64 __attribute__((ms_abi)) beer_joyGetThreshold(u32 id, u32 *threshold)
    { if (threshold) *threshold = 0; return id < 16 ? BEER_JOYERR_UNPLUGGED : BEER_JOYERR_PARMS; }
static u64 __attribute__((ms_abi)) beer_joySetThreshold(u32 id, u32 threshold)
    { (void)threshold; return id < 16 ? BEER_JOYERR_UNPLUGGED : BEER_JOYERR_PARMS; }
static u64 __attribute__((ms_abi)) beer_joySetCapture(u64 window, u32 id, u32 period, u32 changed)
    { (void)window; (void)period; (void)changed; return id < 16 ? BEER_JOYERR_UNPLUGGED : BEER_JOYERR_PARMS; }
static u64 __attribute__((ms_abi)) beer_joyReleaseCapture(u32 id) { return id < 16 ? 0 : BEER_JOYERR_PARMS; }
/* timeGetDevCaps: 1 ms .. 1,000,000 ms periods, as Windows reports. */
static u64 __attribute__((ms_abi)) beer_timeGetDevCaps(u32 *caps, u32 size)
{
    if (!caps || size < 8) return 97;                                 /* TIMERR_STRUCT */
    caps[0] = 1; caps[1] = 1000000;
    return 0;
}
/* WINMM wave input: there is no capture device (waveInGetNumDevs is 0). */
static u64 __attribute__((ms_abi)) beer_waveInOpen(u64 *handle, u32 device, const void *format, u64 callback, u64 instance, u32 flags)
    { (void)device; (void)format; (void)callback; (void)instance; (void)flags; if (handle) *handle = 0; return 2; }  /* MMSYSERR_BADDEVICEID */
#define BEER_WAVEIN_NO_HANDLE(name, ...) \
    static u64 __attribute__((ms_abi)) beer_##name(__VA_ARGS__) { return 5; }                                    /* MMSYSERR_INVALHANDLE */
BEER_WAVEIN_NO_HANDLE(waveInClose, u64 handle)
BEER_WAVEIN_NO_HANDLE(waveInPrepareHeader, u64 handle, void *header, u32 size)
BEER_WAVEIN_NO_HANDLE(waveInUnprepareHeader, u64 handle, void *header, u32 size)
BEER_WAVEIN_NO_HANDLE(waveInAddBuffer, u64 handle, void *header, u32 size)
BEER_WAVEIN_NO_HANDLE(waveInStart, u64 handle)
BEER_WAVEIN_NO_HANDLE(waveInStop, u64 handle)
BEER_WAVEIN_NO_HANDLE(waveInReset, u64 handle)
#undef BEER_WAVEIN_NO_HANDLE

/* WINMM multimedia file I/O (RIFF) over a file handle or a caller memory buffer. */
typedef struct { u64 file; u8 *memory; s64 memory_size; s64 position; } BeerMmio;
#define BEER_FOURCC(a, b, c, d) ((u32)(u8)(a) | ((u32)(u8)(b) << 8) | ((u32)(u8)(c) << 16) | ((u32)(u8)(d) << 24))
static s64 beer_mmio_read(BeerMmio *m, void *buffer, s64 length)
{
    if (length <= 0) return 0;
    if (m->memory) {
        s64 available = m->memory_size - m->position;
        if (length > available) length = available > 0 ? available : 0;
        memcpy(buffer, m->memory + m->position, (size_t)length);
        m->position += length;
        return length;
    }
    u32 done = 0;
    if (!impl_ReadFile(m->file, buffer, (u32)length, &done, NULL)) return -1;
    m->position += done;
    return done;
}
static s64 beer_mmio_write(BeerMmio *m, const void *buffer, s64 length)
{
    if (length <= 0) return 0;
    if (m->memory) {
        s64 available = m->memory_size - m->position;
        if (length > available) length = available > 0 ? available : 0;
        memcpy(m->memory + m->position, buffer, (size_t)length);
        m->position += length;
        return length;
    }
    u32 done = 0;
    if (!impl_WriteFile(m->file, buffer, (u32)length, &done, 0)) return -1;
    m->position += done;
    return done;
}
static int beer_mmio_seek(BeerMmio *m, s64 position)
{
    if (position < 0) return 0;
    if (!m->memory && !impl_SetFilePointerEx(m->file, position, NULL, 0)) return 0;
    m->position = position;
    return 1;
}
/* MMIOINFO (x64): dwFlags +0, fccIOProc +4, pIOProc +8, wErrorRet +16, cchBuffer +32, pchBuffer +40 */
static u64 __attribute__((ms_abi)) beer_mmioOpenW(const u16 *name, u8 *info, u32 flags)
{
    BeerMmio *m = calloc(1, sizeof(*m));
    if (!m) return 0;
    if (info && *(const u32 *)(info + 4) == BEER_FOURCC('M', 'E', 'M', ' ')) {
        m->memory = *(u8 **)(info + 40);
        m->memory_size = *(const s32 *)(info + 32);
        if (!m->memory) { free(m); *(u32 *)(info + 16) = 257; return 0; }           /* MMIOERR_OUTOFMEMORY-ish */
        return (u64)(uintptr_t)m;
    }
    if (!name) { free(m); if (info) *(u32 *)(info + 16) = 256; return 0; }         /* MMIOERR_FILENOTFOUND */
    const u32 mode = flags & 3;                                      /* MMIO_READ 0, MMIO_WRITE 1, MMIO_READWRITE 2 */
    const u32 access = mode == 0 ? 0x80000000u : mode == 1 ? 0x40000000u : 0xC0000000u;
    const u32 disposition = (flags & 0x1000) ? 2 /* CREATE_ALWAYS */ : 3 /* OPEN_EXISTING */;
    u64 file = impl_CreateFileW(name, access, 1 /* FILE_SHARE_READ */, 0, disposition, 0x80, 0);
    if (file == (u64)-1) { free(m); if (info) *(u32 *)(info + 16) = 256; return 0; }
    if (flags & 0x4000) { impl_CloseHandle(file); free(m); return 1; }             /* MMIO_EXIST: just test */
    m->file = file;
    return (u64)(uintptr_t)m;
}
static u64 __attribute__((ms_abi)) beer_mmioClose(u64 handle, u32 flags)
{
    (void)flags;
    BeerMmio *m = (BeerMmio *)(uintptr_t)handle;
    if (!m) return 5;
    if (!m->memory) impl_CloseHandle(m->file);
    free(m);
    return 0;
}
static u64 __attribute__((ms_abi)) beer_mmioRead(u64 handle, void *buffer, s32 length)
{
    BeerMmio *m = (BeerMmio *)(uintptr_t)handle;
    if (!m) return (u64)(s64)-1;
    return (u64)beer_mmio_read(m, buffer, length);
}
static u64 __attribute__((ms_abi)) beer_mmioWrite(u64 handle, const void *buffer, s32 length)
{
    BeerMmio *m = (BeerMmio *)(uintptr_t)handle;
    if (!m) return (u64)(s64)-1;
    return (u64)beer_mmio_write(m, buffer, length);
}
/* MMCKINFO: ckid +0, cksize +4, fccType +8, dwDataOffset +12, dwFlags +16 */
#define BEER_MMIO_FINDCHUNK 0x10u
#define BEER_MMIO_FINDRIFF  0x20u
#define BEER_MMIO_FINDLIST  0x40u
#define BEER_MMIO_DIRTY     0x10000u
static u64 __attribute__((ms_abi)) beer_mmioDescend(u64 handle, u32 *chunk, const u32 *parent, u32 flags)
{
    BeerMmio *m = (BeerMmio *)(uintptr_t)handle;
    if (!m || !chunk) return 5;
    const u32 riff = BEER_FOURCC('R', 'I', 'F', 'F'), list = BEER_FOURCC('L', 'I', 'S', 'T');
    const u32 want_id = chunk[0], want_type = chunk[2];
    const s64 end = parent ? (s64)parent[3] + parent[1] : INT64_MAX;
    for (;;) {
        if (m->position + 8 > end) return 265;                                     /* MMIOERR_CHUNKNOTFOUND */
        u32 header[2];
        if (beer_mmio_read(m, header, 8) != 8) return 265;
        u32 type = 0;
        int container = header[0] == riff || header[0] == list;
        if (container && beer_mmio_read(m, &type, 4) != 4) return 265;
        int match = 1;
        if (flags & BEER_MMIO_FINDCHUNK) match = header[0] == want_id;
        else if (flags & BEER_MMIO_FINDRIFF) match = header[0] == riff && type == want_type;
        else if (flags & BEER_MMIO_FINDLIST) match = header[0] == list && type == want_type;
        if (match) {
            chunk[0] = header[0]; chunk[1] = header[1]; chunk[2] = container ? type : 0;
            chunk[3] = (u32)(m->position - (container ? 4 : 0));
            chunk[4] = 0;
            return 0;
        }
        s64 data_start = m->position - (container ? 4 : 0);
        if (!beer_mmio_seek(m, data_start + header[1] + (header[1] & 1))) return 265;
    }
}
static u64 __attribute__((ms_abi)) beer_mmioAscend(u64 handle, u32 *chunk, u32 flags)
{
    (void)flags;
    BeerMmio *m = (BeerMmio *)(uintptr_t)handle;
    if (!m || !chunk) return 5;
    if (chunk[4] & BEER_MMIO_DIRTY) {                       /* created chunk: write its final size */
        u32 size = (u32)(m->position - chunk[3]);
        if (size & 1) { u8 pad = 0; beer_mmio_write(m, &pad, 1); }
        s64 resume = m->position;
        if (size != chunk[1]) {
            chunk[1] = size;
            if (!beer_mmio_seek(m, (s64)chunk[3] - 4) || beer_mmio_write(m, &size, 4) != 4) return 259; /* MMIOERR_CANNOTWRITE */
        }
        chunk[4] &= ~BEER_MMIO_DIRTY;
        return beer_mmio_seek(m, resume) ? 0 : 263;                                   /* MMIOERR_CANNOTSEEK */
    }
    return beer_mmio_seek(m, (s64)chunk[3] + chunk[1] + (chunk[1] & 1)) ? 0 : 263;
}
static u64 __attribute__((ms_abi)) beer_mmioCreateChunk(u64 handle, u32 *chunk, u32 flags)
{
    BeerMmio *m = (BeerMmio *)(uintptr_t)handle;
    if (!m || !chunk) return 5;
    if (flags & 0x20) chunk[0] = BEER_FOURCC('R', 'I', 'F', 'F');                    /* MMIO_CREATERIFF */
    else if (flags & 0x40) chunk[0] = BEER_FOURCC('L', 'I', 'S', 'T');               /* MMIO_CREATELIST */
    u32 header[2] = { chunk[0], chunk[1] };
    if (beer_mmio_write(m, header, 8) != 8) return 259;
    chunk[3] = (u32)m->position;
    if (flags & 0x60) {
        if (beer_mmio_write(m, &chunk[2], 4) != 4) return 259;
    }
    chunk[4] = BEER_MMIO_DIRTY;
    return 0;
}

/* D3D11On12 and the DXVA2 device manager are not provided. */
static u64 __attribute__((ms_abi)) beer_D3D11On12CreateDevice(void)
{
    static int logged;
    if (!logged++) fprintf(stderr, "[D3D11] D3D11On12CreateDevice -> E_NOTIMPL (11-on-12 is not implemented)\n");
    return 0x80004001u;                                                             /* E_NOTIMPL */
}
static u64 __attribute__((ms_abi)) beer_DXVA2CreateDirect3DDeviceManager9(u32 *token, u64 *manager)
    { if (token) *token = 0; if (manager) *manager = 0; return 0x80004001u; }

/* SETUPAPI: the display adapter's interface path (beer_setupapi_interface_detail in loader.c). */
u64 beer_setupapi_interface_detail(u64 set, const void *interface_data, void *detail, u32 detail_size,
                                   u32 *required, void *device_info, int wide);
static u64 __attribute__((ms_abi)) beer_SetupDiGetDeviceInterfaceDetailW(u64 set, void *interface_data, void *detail,
                                                                         u32 detail_size, u32 *required, void *device_info)
    { return beer_setupapi_interface_detail(set, interface_data, detail, detail_size, required, device_info, 1); }
static u64 __attribute__((ms_abi)) beer_SetupDiGetDeviceInterfaceDetailA(u64 set, void *interface_data, void *detail,
                                                                         u32 detail_size, u32 *required, void *device_info)
    { return beer_setupapi_interface_detail(set, interface_data, detail, detail_size, required, device_info, 0); }
static u64 __attribute__((ms_abi)) beer_CM_Get_Parent(u32 *parent, u32 device, u32 flags)
    { (void)device; (void)flags; if (parent) *parent = 0; return 0x0D; }              /* CR_NO_SUCH_DEVNODE */

/* TDH / event tracing: Beer has no ETW sessions to consume. */
static u64 __attribute__((ms_abi)) beer_TdhGetProperty(void) { return 50; }                            /* ERROR_NOT_SUPPORTED */
static u64 __attribute__((ms_abi)) beer_TdhGetPropertySize(void) { return 50; }
static u64 __attribute__((ms_abi)) beer_TdhGetEventInformation(void) { return 50; }
static u64 __attribute__((ms_abi)) beer_OpenTraceW(void *logfile)
    { (void)logfile; g_last_error = 5; return ~0ull; }                               /* INVALID_PROCESSTRACE_HANDLE */
static u64 __attribute__((ms_abi)) beer_ControlTraceW(u64 trace, const u16 *name, void *properties, u32 control)
    { (void)trace; (void)name; (void)properties; (void)control; return 5; }          /* ERROR_ACCESS_DENIED: not admin */
static u64 __attribute__((ms_abi)) beer_CloseTrace(u64 trace) { (void)trace; return 6; }               /* ERROR_INVALID_HANDLE */

/* ADVAPI32 security. The process is an administrator only when elevated (beer_process_elevated). */
static u64 __attribute__((ms_abi)) beer_AllocateAndInitializeSid(const u8 *authority, u8 count, u32 s0, u32 s1, u32 s2,
                                                                 u32 s3, u32 s4, u32 s5, u32 s6, u32 s7, u64 *sid)
{
    if (!authority || !sid || count == 0 || count > 8) { g_last_error = 87; return 0; }
    u8 *out = (u8 *)(uintptr_t)impl_LocalAlloc(0x40, 8u + 4u * count);
    if (!out) { g_last_error = 8; return 0; }
    out[0] = 1; out[1] = count;                                       /* Revision, SubAuthorityCount */
    memcpy(out + 2, authority, 6);
    const u32 sub[8] = { s0, s1, s2, s3, s4, s5, s6, s7 };
    memcpy(out + 8, sub, 4u * count);
    *sid = (u64)(uintptr_t)out;
    return 1;
}
static u64 __attribute__((ms_abi)) beer_FreeSid(void *sid) { if (sid) impl_LocalFree(sid); return 0; }
/* Windows starts an executable whose manifest requests requireAdministrator (or highestAvailable, for an
 * administrator account) elevated; anything else runs with a standard user token. */
static int beer_process_elevated(void)
{
    static int elevated = -1;
    if (elevated >= 0) return elevated;
    elevated = 0;
    if (g_img && g_img_size) {
        static const char *const levels[] = { "level=\"requireAdministrator\"", "level=\"highestAvailable\"",
                                              "level='requireAdministrator'", "level='highestAvailable'" };
        for (size_t i = 0; i < sizeof(levels) / sizeof(*levels) && !elevated; ++i)
            elevated = memmem(g_img, g_img_size, levels[i], strlen(levels[i])) != NULL;
    }
    if (elevated) fprintf(stderr, "[TOKEN] manifest requests elevation: the process token is an elevated administrator\n");
    return elevated;
}

/* Members: Everyone (S-1-1-0), Users (S-1-5-32-545), Authenticated Users (S-1-5-11), Interactive (S-1-5-4), and
 * Administrators (S-1-5-32-544) when the process runs elevated. */
static u64 __attribute__((ms_abi)) beer_CheckTokenMembership(u64 token, const u8 *sid, u32 *is_member)
{
    (void)token;
    if (!sid || !is_member) { g_last_error = 87; return 0; }
    const u8 count = sid[1];
    const u8 *authority = sid + 2;
    const u32 *sub = (const u32 *)(sid + 8);
    int everyone = count == 1 && authority[5] == 1 && sub[0] == 0;
    int nt = authority[5] == 5;
    int users = nt && count == 2 && sub[0] == 32 && sub[1] == 545;
    int authenticated = nt && count == 1 && (sub[0] == 11 || sub[0] == 4);
    int administrators = nt && count == 2 && sub[0] == 32 && sub[1] == 544 && beer_process_elevated();
    *is_member = everyone || users || authenticated || administrators;
    return 1;
}
/* Security descriptors: objects carry no DACL (all access), as Beer does not enforce object security. */
static u64 __attribute__((ms_abi)) beer_SetEntriesInAclW(u32 count, const void *entries, const void *old_acl, u64 *new_acl)
{
    (void)count; (void)entries; (void)old_acl;
    if (!new_acl) return 87;
    u8 *acl = (u8 *)(uintptr_t)impl_LocalAlloc(0x40, 8);
    if (!acl) return 8;
    acl[0] = 2; *(u16 *)(acl + 2) = 8;                                /* ACL_REVISION, AclSize, no ACEs */
    *new_acl = (u64)(uintptr_t)acl;
    return 0;
}
static u64 __attribute__((ms_abi)) beer_GetSecurityInfo(u64 handle, u32 type, u32 info, u64 *owner, u64 *group,
                                                        u64 *dacl, u64 *sacl, u64 *descriptor)
{
    (void)handle; (void)type; (void)info;
    if (owner) *owner = 0;
    if (group) *group = 0;
    if (dacl) *dacl = 0;
    if (sacl) *sacl = 0;
    if (descriptor) {
        u8 *sd = (u8 *)(uintptr_t)impl_LocalAlloc(0x40, 20);           /* self-relative, DACL present and NULL */
        if (!sd) return 8;
        sd[0] = 1; *(u16 *)(sd + 2) = 0x8004;                         /* SE_SELF_RELATIVE | SE_DACL_PRESENT */
        *descriptor = (u64)(uintptr_t)sd;
    }
    return 0;
}
static u64 __attribute__((ms_abi)) beer_SetSecurityInfo(u64 handle, u32 type, u32 info, void *owner, void *group,
                                                        void *dacl, void *sacl)
    { (void)handle; (void)type; (void)info; (void)owner; (void)group; (void)dacl; (void)sacl; return 0; }
static u64 __attribute__((ms_abi)) beer_AllocateLocallyUniqueId(u64 *luid)
{
    static _Atomic u64 next = 0x10000;
    if (!luid) { g_last_error = 998; return 0; }
    *luid = atomic_fetch_add(&next, 1);
    return 1;
}

static u64 __attribute__((ms_abi)) impl_CoTaskMemAlloc(u64 size);
/* ---- Shell item ID lists for special folders ----
 * A PIDL here is one SHITEMID { USHORT cb = 10; 'B','E','E','R'; INT csidl } and the terminating cb = 0.
 * SHGetPathFromIDList resolves it with SHGetFolderPath, so the paths are those Beer already reports. */
static u64 __attribute__((ms_abi)) beer_SHGetFolderLocation(u64 window, s32 csidl, u64 token, u32 reserved, u64 *pidl)
{
    (void)window; (void)token; (void)reserved;
    if (!pidl) return 0x80070057u;                                    /* E_INVALIDARG */
    *pidl = 0;
    u16 probe[260];
    if ((u32)impl_SHGetFolderPathW(0, csidl & 0xff, 0, 0, probe) != 0) return 0x80070002u;   /* no such folder */
    u8 *list = (u8 *)(uintptr_t)impl_CoTaskMemAlloc(12);
    if (!list) return 0x8007000Eu;                                    /* E_OUTOFMEMORY */
    *(u16 *)list = 10;
    memcpy(list + 2, "BEER", 4);
    *(s32 *)(list + 6) = csidl & 0xff;
    *(u16 *)(list + 10) = 0;
    *pidl = (u64)(uintptr_t)list;
    return 0;
}
static u64 __attribute__((ms_abi)) beer_SHGetSpecialFolderLocation(u64 window, s32 csidl, u64 *pidl)
    { return beer_SHGetFolderLocation(window, csidl, 0, 0, pidl); }
static int beer_pidl_csidl(const u8 *pidl, s32 *csidl)
{
    if (!pidl || *(const u16 *)pidl != 10 || memcmp(pidl + 2, "BEER", 4)) return 0;
    *csidl = *(const s32 *)(pidl + 6);
    return 1;
}
static u64 __attribute__((ms_abi)) beer_SHGetPathFromIDListW(const u8 *pidl, u16 *path)
{
    s32 csidl;
    if (!path) return 0;
    path[0] = 0;
    if (!beer_pidl_csidl(pidl, &csidl)) return 0;
    return (u32)impl_SHGetFolderPathW(0, csidl, 0, 0, path) == 0;
}
static u64 __attribute__((ms_abi)) beer_SHGetPathFromIDListA(const u8 *pidl, char *path)
{
    s32 csidl;
    if (!path) return 0;
    path[0] = 0;
    if (!beer_pidl_csidl(pidl, &csidl)) return 0;
    return (u32)impl_SHGetFolderPathA(0, csidl, 0, 0, path) == 0;
}
static u64 __attribute__((ms_abi)) beer_ILFree(void *pidl) { if (pidl) impl_CoTaskMemFree(pidl); return 0; }

/* CFGMGR32 device interface lists: Beer enumerates no device interfaces, so every list is empty (a lone
 * terminating NUL, length 1) and interface properties report CR_NO_SUCH_DEVICE_INTERFACE. */
static u64 __attribute__((ms_abi)) beer_CM_Get_Device_Interface_List_SizeW(u32 *length, const void *guid, const u16 *device, u32 flags)
    { (void)guid; (void)device; (void)flags; if (!length) return 0x1F; *length = 1; return 0; }   /* CR_INVALID_POINTER */
static u64 __attribute__((ms_abi)) beer_CM_Get_Device_Interface_List_SizeA(u32 *length, const void *guid, const char *device, u32 flags)
    { (void)guid; (void)device; (void)flags; if (!length) return 0x1F; *length = 1; return 0; }
static u64 __attribute__((ms_abi)) beer_CM_Get_Device_Interface_ListW(const void *guid, const u16 *device, u16 *buffer, u32 length, u32 flags)
    { (void)guid; (void)device; (void)flags; if (!buffer) return 0x1F; if (!length) return 0x1A; buffer[0] = 0; return 0; }  /* CR_BUFFER_SMALL */
static u64 __attribute__((ms_abi)) beer_CM_Get_Device_Interface_ListA(const void *guid, const char *device, char *buffer, u32 length, u32 flags)
    { (void)guid; (void)device; (void)flags; if (!buffer) return 0x1F; if (!length) return 0x1A; buffer[0] = 0; return 0; }
static u64 __attribute__((ms_abi)) beer_CM_Get_Device_Interface_PropertyW(const u16 *interface, const void *key, u32 *type, u8 *buffer,
                                                                          u32 *size, u32 flags)
    { (void)interface; (void)key; (void)buffer; (void)flags; if (type) *type = 0; if (size) *size = 0; return 0x37; }

/* ---- Process-memory calls on the current process (replacing the earlier return-0 placeholders) ----
 * Other processes' memory is not reachable from Beer: those calls fail with ERROR_ACCESS_DENIED. */
static int beer_is_current_process(u64 process)
{
    return process == (u64)-1 || process == (u64)(u32)getpid();
}
static u64 __attribute__((ms_abi)) beer_VirtualAllocEx(u64 process, void *address, u64 size, u32 type, u32 protect)
{
    if (!beer_is_current_process(process)) { g_last_error = 5; return 0; }
    return impl_VirtualAlloc(address, size, type, protect);
}
static u64 __attribute__((ms_abi)) beer_VirtualProtectEx(u64 process, void *address, u64 size, u32 protect, u32 *old)
{
    if (!beer_is_current_process(process)) { g_last_error = 5; return 0; }
    return impl_VirtualProtect(address, size, protect, old);
}
static u64 beer_process_memory_copy(u64 process, void *remote, void *local, u64 size, u64 *done, int write)
{
    if (done) *done = 0;
    if (!beer_is_current_process(process)) { g_last_error = 5; return 0; }
    if (!size) return 1;
    struct iovec local_vector = { local, (size_t)size }, remote_vector = { remote, (size_t)size };
    ssize_t copied = write ? process_vm_writev(getpid(), &local_vector, 1, &remote_vector, 1, 0)
                           : process_vm_readv(getpid(), &local_vector, 1, &remote_vector, 1, 0);
    if (copied < 0) copied = 0;
    if (done) *done = (u64)copied;
    if ((u64)copied != size) { g_last_error = 299; return 0; }        /* ERROR_PARTIAL_COPY */
    return 1;
}
static u64 __attribute__((ms_abi)) beer_ReadProcessMemory(u64 process, void *base, void *buffer, u64 size, u64 *read)
    { return beer_process_memory_copy(process, base, buffer, size, read, 0); }
static u64 __attribute__((ms_abi)) beer_WriteProcessMemory(u64 process, void *base, const void *buffer, u64 size, u64 *written)
    { return beer_process_memory_copy(process, base, (void *)buffer, size, written, 1); }
/* Pages are not paged out under Beer's control; locking is accepted. */
static u64 __attribute__((ms_abi)) beer_VirtualLock(void *address, u64 size) { (void)address; (void)size; return 1; }
static u64 __attribute__((ms_abi)) beer_VirtualUnlock(void *address, u64 size) { (void)address; (void)size; return 1; }

/* ---- msvcrt.dll internals with no UCRT export (legacy CRT imports bound to the bundled UCRT) ---- */
static pthread_mutex_t g_msvcrt_locks[64];
static pthread_once_t g_msvcrt_locks_once = PTHREAD_ONCE_INIT;
static void beer_msvcrt_locks_init(void)
{
    pthread_mutexattr_t attributes;
    pthread_mutexattr_init(&attributes);
    pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
    for (int i = 0; i < 64; ++i) pthread_mutex_init(&g_msvcrt_locks[i], &attributes);
    pthread_mutexattr_destroy(&attributes);
}
/* _lock/_unlock: the CRT's numbered internal locks (recursive, as msvcrt's critical sections are). */
static u64 __attribute__((ms_abi)) beer_msvcrt_lock(s32 number)
{
    pthread_once(&g_msvcrt_locks_once, beer_msvcrt_locks_init);
    if (number >= 0 && number < 64) pthread_mutex_lock(&g_msvcrt_locks[number]);
    return 0;
}
static u64 __attribute__((ms_abi)) beer_msvcrt_unlock(s32 number)
{
    pthread_once(&g_msvcrt_locks_once, beer_msvcrt_locks_init);
    if (number >= 0 && number < 64) pthread_mutex_unlock(&g_msvcrt_locks[number]);
    return 0;
}
/* _onexit/__dllonexit register a function to run at exit (or DLL unload); Beer does not run them. The documented
 * success result is the function pointer itself. */
static u64 __attribute__((ms_abi)) beer_msvcrt_onexit(u64 function) { return function; }
static u64 __attribute__((ms_abi)) beer_msvcrt_dllonexit(u64 function, u64 *begin, u64 *end)
    { (void)begin; (void)end; return function; }
static u64 __attribute__((ms_abi)) beer_msvcrt_amsg_exit(s32 code)
{
    fprintf(stderr, "[MSVCRT] _amsg_exit(%d): runtime error, exiting\n", code);
    _exit(255);
}
/* _XcptFilter: the CRT's top-level filter; without a signal() handler the exception keeps propagating. */
static u64 __attribute__((ms_abi)) beer_msvcrt_XcptFilter(u32 code, void *pointers)
    { (void)code; (void)pointers; return 0; }                        /* EXCEPTION_CONTINUE_SEARCH */
/* type_info::~type_info: the name cache is not used by Beer's UCRT bridge; nothing to release. */
static u64 __attribute__((ms_abi)) beer_type_info_destructor(void *self) { (void)self; return 0; }

/* ---- OLEAUT32 SafeArrays (replacing the earlier return-0 placeholders) ----
 * x64 SAFEARRAY: cDims u16 +0, fFeatures u16 +2, cbElements u32 +4, cLocks u32 +8, pvData +16, then cDims
 * SAFEARRAYBOUND { cElements u32, lLbound s32 } stored rightmost dimension first. With FADF_HAVEVARTYPE the VARTYPE is
 * the u32 just before the descriptor. Element copies handle plain data and BSTRs; VARIANT and interface elements are
 * reported as DISP_E_BADVARTYPE by Get/PutElement (their data stays reachable through AccessData). */
#define BEER_FADF_BSTR 0x0100u
#define BEER_FADF_HAVEVARTYPE 0x0080u
static u32 beer_safearray_element_size(u16 vt)
{
    switch (vt) {
    case 16: case 17: return 1;                                   /* VT_I1, VT_UI1 */
    case 2: case 18: case 11: return 2;                           /* VT_I2, VT_UI2, VT_BOOL */
    case 3: case 19: case 4: case 10: case 22: case 23: return 4; /* VT_I4, VT_UI4, VT_R4, VT_ERROR, VT_INT, VT_UINT */
    case 20: case 21: case 5: case 6: case 7: return 8;           /* VT_I8, VT_UI8, VT_R8, VT_CY, VT_DATE */
    case 8: case 9: case 13: return 8;                            /* VT_BSTR, VT_DISPATCH, VT_UNKNOWN */
    case 12: return 24;                                           /* VT_VARIANT */
    default: return 0;
    }
}
static u8 *beer_safearray_new(u16 vt, u32 dims, const u32 *bounds_leftmost_first)
{
    u32 element = beer_safearray_element_size(vt);
    if (!element || !dims || dims > 64) return NULL;
    u64 total = 1;
    for (u32 d = 0; d < dims; ++d) total *= bounds_leftmost_first[2 * d];
    u8 *block = calloc(1, 16 + 24 + 8u * dims);
    if (!block) return NULL;
    u8 *sa = block + 16;
    *(u32 *)(sa - 4) = vt;
    *(u16 *)sa = (u16)dims;
    *(u16 *)(sa + 2) = (u16)(BEER_FADF_HAVEVARTYPE | (vt == 8 ? BEER_FADF_BSTR : 0));
    *(u32 *)(sa + 4) = element;
    for (u32 d = 0; d < dims; ++d) {                               /* stored rightmost first */
        u32 *bound = (u32 *)(sa + 24 + 8u * (dims - 1 - d));
        bound[0] = bounds_leftmost_first[2 * d];
        bound[1] = bounds_leftmost_first[2 * d + 1];
    }
    void *data = calloc(total ? total : 1, element);
    if (!data) { free(block); return NULL; }
    *(void **)(sa + 16) = data;
    return sa;
}
static u64 __attribute__((ms_abi)) beer_SafeArrayCreate(u16 vt, u32 dims, const u32 *bounds)
    { return bounds ? (u64)(uintptr_t)beer_safearray_new(vt, dims, bounds) : 0; }
static u64 __attribute__((ms_abi)) beer_SafeArrayCreateVector(u16 vt, s32 lower, u32 count)
    { u32 bound[2] = { count, (u32)lower }; return (u64)(uintptr_t)beer_safearray_new(vt, 1, bound); }
static u64 __attribute__((ms_abi)) beer_SafeArrayDestroy(u8 *sa)
{
    if (!sa) return 0;
    if (*(u32 *)(sa + 8)) return 0x8002000Du;                      /* DISP_E_ARRAYISLOCKED */
    u16 dims = *(u16 *)sa;
    void *data = *(void **)(sa + 16);
    if ((*(u16 *)(sa + 2) & BEER_FADF_BSTR) && data) {
        u64 total = 1;
        for (u16 d = 0; d < dims; ++d) total *= *(u32 *)(sa + 24 + 8u * d);
        for (u64 i = 0; i < total; ++i) if (((void **)data)[i]) impl_SysFreeString(((void **)data)[i]);
    }
    free(data);
    free(sa - 16);
    return 0;
}
static u64 __attribute__((ms_abi)) beer_SafeArrayGetUBound(const u8 *sa, u32 dim, s32 *upper)
{
    if (!sa || !upper) return 0x80070057u;                         /* E_INVALIDARG */
    u16 dims = *(const u16 *)sa;
    if (dim < 1 || dim > dims) return 0x8002000Bu;                 /* DISP_E_BADINDEX */
    const u32 *bound = (const u32 *)(sa + 24 + 8u * (dims - dim));
    *upper = (s32)bound[1] + (s32)bound[0] - 1;
    return 0;
}
static u64 __attribute__((ms_abi)) beer_SafeArrayGetLBound(const u8 *sa, u32 dim, s32 *lower)
{
    if (!sa || !lower) return 0x80070057u;
    u16 dims = *(const u16 *)sa;
    if (dim < 1 || dim > dims) return 0x8002000Bu;
    *lower = (s32)((const u32 *)(sa + 24 + 8u * (dims - dim)))[1];
    return 0;
}
static u64 __attribute__((ms_abi)) beer_SafeArrayAccessData(u8 *sa, void **data)
{
    if (!sa || !data) return 0x80070057u;
    __atomic_add_fetch((u32 *)(sa + 8), 1, __ATOMIC_SEQ_CST);
    *data = *(void **)(sa + 16);
    return 0;
}
static u64 __attribute__((ms_abi)) beer_SafeArrayUnaccessData(u8 *sa)
{
    if (!sa) return 0x80070057u;
    if (!*(u32 *)(sa + 8)) return 0x8000FFFFu;                     /* E_UNEXPECTED */
    __atomic_sub_fetch((u32 *)(sa + 8), 1, __ATOMIC_SEQ_CST);
    return 0;
}
/* The address of the element at the given indices (leftmost first); 0 when out of bounds. */
static u8 *beer_safearray_element(u8 *sa, const s32 *indices)
{
    u16 dims = *(u16 *)sa;
    u64 offset = 0, stride = 1;
    for (u16 d = 0; d < dims; ++d) {                               /* rgsabound[0] is the rightmost dimension */
        const u32 *bound = (const u32 *)(sa + 24 + 8u * d);
        s64 index = (s64)indices[dims - 1 - d] - (s32)bound[1];
        if (index < 0 || index >= (s64)bound[0]) return NULL;
        offset += (u64)index * stride;
        stride *= bound[0];
    }
    return (u8 *)*(void **)(sa + 16) + offset * *(u32 *)(sa + 4);
}
static u64 __attribute__((ms_abi)) beer_SafeArrayGetElement(u8 *sa, const s32 *indices, void *value)
{
    if (!sa || !indices || !value) return 0x80070057u;
    u8 *element = beer_safearray_element(sa, indices);
    if (!element) return 0x8002000Bu;
    u16 vt = (u16)*(u32 *)(sa - 4);
    if (vt == 12 || vt == 9 || vt == 13) return 0x80020008u;       /* DISP_E_BADVARTYPE */
    if (vt == 8) {
        const u16 *text = *(const u16 **)element;
        *(u64 *)value = text ? impl_SysAllocStringLen(text, ((const u32 *)text)[-1] / 2) : 0;
        return 0;
    }
    memcpy(value, element, *(u32 *)(sa + 4));
    return 0;
}
static u64 __attribute__((ms_abi)) beer_SafeArrayPutElement(u8 *sa, const s32 *indices, void *value)
{
    if (!sa || !indices) return 0x80070057u;
    u8 *element = beer_safearray_element(sa, indices);
    if (!element) return 0x8002000Bu;
    u16 vt = (u16)*(u32 *)(sa - 4);
    if (vt == 12 || vt == 9 || vt == 13) return 0x80020008u;
    if (vt == 8) {
        if (*(void **)element) impl_SysFreeString(*(void **)element);
        const u16 *text = value;
        *(u64 *)element = text ? impl_SysAllocStringLen(text, ((const u32 *)text)[-1] / 2) : 0;
        return 0;
    }
    if (!value) return 0x80070057u;
    memcpy(element, value, *(u32 *)(sa + 4));
    return 0;
}

/* ---- GDI+: start-up and memory are real; Beer has no GDI+ renderer, so drawing, fonts and images report
 * NotImplemented (status 6) with NULL outputs. ---- */
#define BEER_GDIP_NOT_IMPLEMENTED 6u
static u64 __attribute__((ms_abi)) beer_GdiplusStartup(u64 *token, const u32 *input, u64 *output)
{
    if (!token || !input) return 2;                                /* InvalidParameter */
    if (input[0] != 1 && input[0] != 2 && input[0] != 3) return 4; /* UnsupportedGdiplusVersion... */
    *token = 0x47445000u;
    if (output) { output[0] = 0; output[1] = 0; }                  /* no background-thread hook functions */
    return 0;
}
static u64 __attribute__((ms_abi)) beer_GdiplusShutdown(u64 token) { (void)token; return 0; }
static u64 __attribute__((ms_abi)) beer_GdipAlloc(u64 size) { return (u64)(uintptr_t)malloc(size ? size : 1); }
static u64 __attribute__((ms_abi)) beer_GdipFree(void *pointer) { free(pointer); return 0; }
/* Each NotImplemented call clears its output pointer (the argument given by index, 0 = none). */
#define BEER_GDIP_NI(name, out_index, ...) \
    static u64 __attribute__((ms_abi)) beer_##name(__VA_ARGS__) { BEER_GDIP_CLEAR_##out_index; return BEER_GDIP_NOT_IMPLEMENTED; }
#define BEER_GDIP_CLEAR_0 do {} while (0)
#define BEER_GDIP_CLEAR_2 do { if (b) *(u64 *)(uintptr_t)b = 0; } while (0)
#define BEER_GDIP_CLEAR_3 do { if (c) *(u64 *)(uintptr_t)c = 0; } while (0)
#define BEER_GDIP_CLEAR_4 do { if (d) *(u64 *)(uintptr_t)d = 0; } while (0)
#define BEER_GDIP_CLEAR_5 do { if (e) *(u64 *)(uintptr_t)e = 0; } while (0)
#define BEER_GDIP_CLEAR_6 do { if (f) *(u64 *)(uintptr_t)f = 0; } while (0)
BEER_GDIP_NI(GdipCreateFont, 5, u64 a, u64 b, u64 c, u64 d, u64 e)                  /* (family, size, style, unit, font**) */
BEER_GDIP_NI(GdipCreateFontFamilyFromName, 3, u64 a, u64 b, u64 c)                  /* (name, collection, family**) */
static u64 __attribute__((ms_abi)) beer_GdipGetGenericFontFamilySansSerif(u64 *family) { if (family) *family = 0; return BEER_GDIP_NOT_IMPLEMENTED; }
/* Deleting objects that were never created: GDI+ answers InvalidParameter for NULL. */
static u64 __attribute__((ms_abi)) beer_GdipDeleteObject(void *object) { return object ? 0 : 2; }

/* ---- Media Foundation pipelines (sessions, topologies, renderers): not provided by Beer ---- */
#define BEER_MF_NOTIMPL(name, ...) \
    static u64 __attribute__((ms_abi)) beer_##name(__VA_ARGS__) { return 0x80004001u; }
static u64 __attribute__((ms_abi)) beer_MFCreateTopology(u64 *topology) { if (topology) *topology = 0; return 0x80004001u; }
static u64 __attribute__((ms_abi)) beer_MFCreateTopologyNode(u32 type, u64 *node) { (void)type; if (node) *node = 0; return 0x80004001u; }
static u64 __attribute__((ms_abi)) beer_MFCreateMediaSession(void *config, u64 *session) { (void)config; if (session) *session = 0; return 0x80004001u; }
static u64 __attribute__((ms_abi)) beer_MFCreateSourceResolver(u64 *resolver) { if (resolver) *resolver = 0; return 0x80004001u; }
static u64 __attribute__((ms_abi)) beer_MFCreateAudioRendererActivate(u64 *activate) { if (activate) *activate = 0; return 0x80004001u; }
static u64 __attribute__((ms_abi)) beer_MFCreateVideoRendererActivate(u64 window, u64 *activate) { (void)window; if (activate) *activate = 0; return 0x80004001u; }
static u64 __attribute__((ms_abi)) beer_MFCreateAsyncResult(void *object, void *callback, void *state, u64 *result)
    { (void)object; (void)callback; (void)state; if (result) *result = 0; return 0x80004001u; }
static u64 __attribute__((ms_abi)) beer_MFCreateVideoSampleFromSurface(void *surface, u64 *sample) { (void)surface; if (sample) *sample = 0; return 0x80004001u; }
static u64 __attribute__((ms_abi)) beer_MFGetService(void *object, const void *service, const void *iid, u64 *out)
    { (void)object; (void)service; (void)iid; if (out) *out = 0; return 0xC00D36BAu; }       /* MF_E_UNSUPPORTED_SERVICE */
/* 100-ns frame duration for a rational frame rate (numerator/denominator). */
static u64 __attribute__((ms_abi)) beer_MFFrameRateToAverageTimePerFrame(u32 numerator, u32 denominator, u64 *duration)
{
    if (!duration) return 0x80004003u;
    *duration = numerator ? (u64)(((unsigned __int128)10000000u * denominator + numerator / 2) / numerator) : 0;
    return 0;
}

/* ---- HID: Beer enumerates no HID devices, so there is never a valid device handle or preparsed data ---- */
static u64 __attribute__((ms_abi)) beer_HidD_GetHidGuid(u8 *guid)
{
    static const u8 hid[16] = { 0xb2,0x55,0x1e,0x4d, 0x6f,0xf1, 0xcf,0x11, 0x88,0xcb,0x00,0x11,0x11,0x00,0x00,0x30 };
    if (guid) memcpy(guid, hid, 16);                               /* GUID_DEVINTERFACE_HID 4D1E55B2-F16F-11CF-88CB-001111000030 */
    return 0;
}
#define BEER_HID_FALSE(name, ...) \
    static u64 __attribute__((ms_abi)) beer_##name(__VA_ARGS__) { g_last_error = 6; return 0; }
BEER_HID_FALSE(HidD_GetAttributes, u64 device, void *attributes)
BEER_HID_FALSE(HidD_GetPreparsedData, u64 device, u64 *data)
BEER_HID_FALSE(HidD_FreePreparsedData, u64 data)
BEER_HID_FALSE(HidD_GetFeature, u64 device, void *buffer, u32 length)
BEER_HID_FALSE(HidD_SetFeature, u64 device, void *buffer, u32 length)
BEER_HID_FALSE(HidD_GetManufacturerString, u64 device, void *buffer, u32 length)
BEER_HID_FALSE(HidD_GetProductString, u64 device, void *buffer, u32 length)
BEER_HID_FALSE(HidD_GetSerialNumberString, u64 device, void *buffer, u32 length)
BEER_HID_FALSE(HidD_GetIndexedString, u64 device, u32 index, void *buffer, u32 length)
static u64 __attribute__((ms_abi)) beer_HidP_GetValueCaps(u32 type, void *caps, u16 *length, u64 data)
    { (void)type; (void)caps; (void)data; if (length) *length = 0; return 0xC0110001u; }   /* HIDP_STATUS_INVALID_PREPARSED_DATA */

/* ---- Remaining entry points ---- */
static u64 __attribute__((ms_abi)) beer_StartTraceW(u64 *trace, const u16 *name, void *properties)
    { (void)name; (void)properties; if (trace) *trace = 0; return 5; }    /* ERROR_ACCESS_DENIED: not admin for ETW */
/* The open-file dialog is not available: behave as if the user cancelled (CommDlgExtendedError() is 0). */
static u64 __attribute__((ms_abi)) beer_GetOpenFileNameW(void *dialog) { (void)dialog; return 0; }
static u64 __attribute__((ms_abi)) beer_EnumFontFamiliesW(u64 dc, const u16 *family, u64 callback, u64 parameter)
    { (void)dc; (void)family; (void)callback; (void)parameter; return 1; }   /* no fonts enumerated */
static u64 __attribute__((ms_abi)) beer_GetIpForwardTable(void *table, u32 *size, u32 order)
    { (void)table; (void)order; if (size) *size = 0; return 50; }          /* ERROR_NOT_SUPPORTED */
static u64 __attribute__((ms_abi)) beer_CreateStreamOnHGlobal(u64 global, u32 release, u64 *stream)
    { (void)global; (void)release; if (stream) *stream = 0; return 0x80004001u; }
static u64 __attribute__((ms_abi)) beer_GetUserProfileDirectoryW(u64 token, u16 *path, u32 *size)
{
    (void)token;
    u16 profile[260];
    if (!size) { g_last_error = 87; return 0; }
    if ((u32)impl_SHGetFolderPathW(0, 0x28 /* CSIDL_PROFILE */, 0, 0, profile) != 0) { g_last_error = 2; return 0; }
    u32 length = 0;
    while (profile[length]) ++length;
    if (!path || *size < length + 1) { *size = length + 1; g_last_error = 122; return 0; }
    memcpy(path, profile, (length + 1) * sizeof(u16));
    *size = length + 1;
    return 1;
}
/* SetDllDirectory(path): sets the one extra DLL search directory (searched after the application directory);
 * NULL or "" clears it. Fails with ERROR_PATH_NOT_FOUND / ERROR_DIRECTORY like Windows when the path is no
 * directory. */
static u64 beer_set_dll_directory_utf8(const char *path)
{
    char host[PATH_MAX];
    host[0] = 0;
    if (path && *path) {
        struct stat status;
        if (!resolve_windows_host_path(path, host, sizeof(host)) || stat(host, &status) != 0) {
            g_last_error = 3;                                   /* ERROR_PATH_NOT_FOUND */
            return 0;
        }
        if (!S_ISDIR(status.st_mode)) { g_last_error = 267; return 0; }   /* ERROR_DIRECTORY */
    }
    pthread_mutex_lock(&g_dll_directory_mutex);
    snprintf(g_set_dll_directory, sizeof(g_set_dll_directory), "%s", host);
    pthread_mutex_unlock(&g_dll_directory_mutex);
    g_last_error = 0;
    return 1;
}
static u64 __attribute__((ms_abi)) beer_SetDllDirectoryW(const u16 *path)
{
    if (!path) return beer_set_dll_directory_utf8(NULL);
    char narrow[PATH_MAX];
    size_t length = 0;
    while (path[length] && length + 1 < sizeof(narrow)) {
        narrow[length] = path[length] < 0x80 ? (char)path[length] : '?';
        ++length;
    }
    if (path[length]) { g_last_error = 206; return 0; }         /* ERROR_FILENAME_EXCED_RANGE */
    narrow[length] = 0;
    return beer_set_dll_directory_utf8(narrow);
}
static u64 __attribute__((ms_abi)) beer_SetDllDirectoryA(const char *path) { return beer_set_dll_directory_utf8(path); }

/* ---- System time in NT form (100-ns intervals since 1601-01-01 UTC) ---- */
#define BEER_EPOCH_1970 116444736000000000ull                  /* 1970-01-01 as NT time */
#define BEER_EPOCH_1980 119600064000000000ull                  /* 1980-01-01 as NT time */
static u64 __attribute__((ms_abi)) beer_NtQuerySystemTime(u64 *time)
{
    if (!time) return 0xC0000005u;                             /* STATUS_ACCESS_VIOLATION */
    impl_GetSystemTimePreciseAsFileTime(time);
    return 0;
}
/* BOOLEAN RtlTimeToSecondsSince1970/1980(const LARGE_INTEGER *time, ULONG *seconds): FALSE outside the ULONG range. */
static u64 __attribute__((ms_abi)) beer_RtlTimeToSecondsSince1970(const u64 *time, u32 *seconds)
{
    if (!time || !seconds || *time < BEER_EPOCH_1970) return 0;
    u64 value = (*time - BEER_EPOCH_1970) / 10000000u;
    if (value > 0xffffffffu) return 0;
    *seconds = (u32)value;
    return 1;
}
static u64 __attribute__((ms_abi)) beer_RtlTimeToSecondsSince1980(const u64 *time, u32 *seconds)
{
    if (!time || !seconds || *time < BEER_EPOCH_1980) return 0;
    u64 value = (*time - BEER_EPOCH_1980) / 10000000u;
    if (value > 0xffffffffu) return 0;
    *seconds = (u32)value;
    return 1;
}
static u64 __attribute__((ms_abi)) beer_RtlSecondsSince1970ToTime(u32 seconds, u64 *time)
    { if (time) *time = BEER_EPOCH_1970 + (u64)seconds * 10000000u; return 0; }
static u64 __attribute__((ms_abi)) beer_RtlSecondsSince1980ToTime(u32 seconds, u64 *time)
    { if (time) *time = BEER_EPOCH_1980 + (u64)seconds * 10000000u; return 0; }

/* int NormalizeString(NORM_FORM form, LPCWSTR src, int src_length, LPWSTR dst, int dst_length) and
 * BOOL IsNormalizedString(form, src, length), exported by kernel32 and normaliz.dll. Forms: C=1, D=2, KC=5, KD=6.
 * Text below U+0300 is unchanged by every form. Other text goes through the host's GLib g_utf8_normalize when it is
 * present; without GLib (Android), the text comes back unchanged, which is right for text that is already composed. */
typedef char *(*BeerGUtf8Normalize)(const char *, long, int);
static BeerGUtf8Normalize beer_glib_normalize(void (**g_free_out)(void *))
{
    static BeerGUtf8Normalize normalize;
    static void (*g_free_fn)(void *);
    static int tried;
    if (!tried) {
        tried = 1;
        void *glib = dlopen("libglib-2.0.so.0", RTLD_NOW | RTLD_LOCAL);
        if (glib) {
            normalize = (BeerGUtf8Normalize)dlsym(glib, "g_utf8_normalize");
            g_free_fn = (void (*)(void *))dlsym(glib, "g_free");
            if (!g_free_fn) normalize = NULL;
        }
    }
    *g_free_out = g_free_fn;
    return normalize;
}
/* Returns the normalized UTF-16 text (malloc'd, *out_length units, no terminator added), or NULL with *error set. */
static u16 *beer_normalize_utf16(u32 form, const u16 *src, int length, int *out_length, u32 *error)
{
    int glib_mode = form == 1 ? 1 : form == 2 ? 0 : form == 5 ? 3 : form == 6 ? 2 : -1;
    if (glib_mode < 0) { *error = 87; return NULL; }
    int simple = 1;
    for (int i = 0; i < length; ++i) if (src[i] >= 0x300) { simple = 0; break; }
    void (*glib_free)(void *) = NULL;
    BeerGUtf8Normalize normalize = simple ? NULL : beer_glib_normalize(&glib_free);
    if (!normalize) {
        u16 *copy = malloc(((size_t)length + 1) * 2);
        if (!copy) { *error = 8; return NULL; }
        memcpy(copy, src, (size_t)length * 2);
        *out_length = length;
        return copy;
    }
    char *utf8 = malloc((size_t)length * 3 + 1);
    if (!utf8) { *error = 8; return NULL; }
    size_t n = 0;
    for (int i = 0; i < length; ++i) {
        u32 c = src[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < length && src[i + 1] >= 0xdc00 && src[i + 1] < 0xe000)
            c = 0x10000 + ((c - 0xd800) << 10) + (src[++i] - 0xdc00);
        else if (c >= 0xd800 && c < 0xe000) { free(utf8); *error = 1113; return NULL; }   /* ERROR_NO_UNICODE_TRANSLATION */
        if (c < 0x80) utf8[n++] = (char)c;
        else if (c < 0x800) { utf8[n++] = (char)(0xc0 | c >> 6); utf8[n++] = (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { utf8[n++] = (char)(0xe0 | c >> 12); utf8[n++] = (char)(0x80 | ((c >> 6) & 63)); utf8[n++] = (char)(0x80 | (c & 63)); }
        else { utf8[n++] = (char)(0xf0 | c >> 18); utf8[n++] = (char)(0x80 | ((c >> 12) & 63)); utf8[n++] = (char)(0x80 | ((c >> 6) & 63)); utf8[n++] = (char)(0x80 | (c & 63)); }
    }
    char *normalized = normalize(utf8, (long)n, glib_mode);
    free(utf8);
    if (!normalized) { *error = 1113; return NULL; }
    size_t bytes = strlen(normalized);
    u16 *out = malloc((bytes + 1) * 2 * 2);
    if (!out) { glib_free(normalized); *error = 8; return NULL; }
    int units = 0;
    for (size_t i = 0; i < bytes;) {
        unsigned char b = (unsigned char)normalized[i];
        u32 c; int extra;
        if (b < 0x80) { c = b; extra = 0; } else if (b < 0xe0) { c = b & 31; extra = 1; }
        else if (b < 0xf0) { c = b & 15; extra = 2; } else { c = b & 7; extra = 3; }
        ++i;
        for (int k = 0; k < extra && i < bytes; ++k, ++i) c = c << 6 | ((unsigned char)normalized[i] & 63);
        if (c >= 0x10000) { c -= 0x10000; out[units++] = (u16)(0xd800 + (c >> 10)); out[units++] = (u16)(0xdc00 + (c & 0x3ff)); }
        else out[units++] = (u16)c;
    }
    glib_free(normalized);
    *out_length = units;
    return out;
}
static u64 __attribute__((ms_abi)) beer_NormalizeString(u32 form, const u16 *src, s32 src_length, u16 *dst, s32 dst_length)
{
    if (!src || dst_length < 0 || (dst_length > 0 && !dst) || src_length < -1 || src_length == 0) {
        g_last_error = 87; return 0;
    }
    int length = src_length;
    if (length == -1) { length = 0; while (src[length]) ++length; ++length; }   /* the terminator is counted */
    u32 error = 0;
    int out_length = 0;
    u16 *out = beer_normalize_utf16(form, src, length, &out_length, &error);
    if (!out) { g_last_error = error; return 0; }
    if (dst_length == 0) {                       /* size query: an estimate, as Windows gives */
        free(out);
        g_last_error = 0;
        return (u64)(u32)(out_length > length ? out_length : length);
    }
    if (out_length > dst_length) {
        free(out);
        g_last_error = 122;                        /* ERROR_INSUFFICIENT_BUFFER */
        return (u64)(u32)-out_length;              /* Windows returns <= 0 here */
    }
    memcpy(dst, out, (size_t)out_length * 2);
    free(out);
    g_last_error = 0;
    return (u64)(u32)out_length;
}
static u64 __attribute__((ms_abi)) beer_IsNormalizedString(u32 form, const u16 *src, s32 length)
{
    if (!src || length < -1) { g_last_error = 87; return 0; }
    if (length == -1) { length = 0; while (src[length]) ++length; }
    u32 error = 0;
    int out_length = 0;
    u16 *out = beer_normalize_utf16(form, src, length, &out_length, &error);
    if (!out) { g_last_error = error; return 0; }
    int same = out_length == length && !memcmp(out, src, (size_t)length * 2);
    free(out);
    g_last_error = 0;
    return (u64)same;
}

/* ANSI forms of the drive queries, and process/driver enumeration (psapi K32*): this process only, no drivers. */
static u64 __attribute__((ms_abi)) beer_GetLogicalDriveStringsA(u32 size, char *buffer)
{
    u16 wide[16];
    const u32 length = (u32)beer_GetLogicalDriveStringsW(16, wide);   /* characters without the final NUL */
    if (!buffer || size < length + 1) return length + 1;
    for (u32 i = 0; i <= length; ++i) buffer[i] = (char)wide[i];
    return length;
}
static u64 __attribute__((ms_abi)) beer_QueryDosDeviceA(const char *name, char *target, u32 size)
{
    u16 wide_name[260], wide_target[1024];
    size_t n = 0;
    for (; name && name[n] && n + 1 < 260; ++n) wide_name[n] = (u8)name[n];
    wide_name[n] = 0;
    u32 capacity = size < 1024 ? size : 1024;
    u32 length = (u32)beer_QueryDosDeviceW(name ? wide_name : NULL, wide_target, capacity);
    if (!length) return 0;
    for (u32 i = 0; i < length && target; ++i) target[i] = (char)wide_target[i];
    return length;
}
static u64 __attribute__((ms_abi)) beer_K32EnumProcesses(u32 *ids, u32 size, u32 *needed)
{
    if (!needed) { g_last_error = 998; return 0; }
    if (ids && size >= 4) { ids[0] = (u32)impl_GetCurrentProcessId(); *needed = 4; }
    else *needed = 0;
    return 1;
}
static u64 __attribute__((ms_abi)) beer_K32EnumDeviceDrivers(u64 *bases, u32 size, u32 *needed)
    { (void)bases; (void)size; if (needed) *needed = 0; return 1; }
static u64 __attribute__((ms_abi)) beer_K32GetDeviceDriverFileName(u64 base, void *name, u32 size)
    { (void)base; (void)name; (void)size; g_last_error = 87; return 0; }
static u64 __attribute__((ms_abi)) beer_SetProcessPreferredUILanguages(u32 flags, const u16 *languages, u32 *count)
    { (void)flags; (void)languages; if (count) *count = 0; return 1; }
/* FlsGetValue2 (Windows 11): FlsGetValue without touching the last error. */
static u64 __attribute__((ms_abi)) beer_FlsGetValue2(u32 index)
{
    const u32 saved = g_last_error;
    const u64 value = impl_FlsGetValue(index);
    g_last_error = saved;
    return value;
}

/* CoGetMalloc(MEMCTX_TASK, IMalloc **): the process task allocator, the same heap as CoTaskMemAlloc/Free.
 * IMalloc: QueryInterface, AddRef, Release, Alloc, Realloc, Free, GetSize, DidAlloc, HeapMinimize. One static
 * object; its reference count is irrelevant (it lives for the whole process, as on Windows). dxil.dll fetched it
 * and called through it after the old stub returned S_OK without one. */
static u64 __attribute__((ms_abi)) beer_imalloc_query(void *self, const u8 *iid, void **out)
{
    static const u8 iid_unknown[16] = { 0,0,0,0, 0,0, 0,0, 0xc0,0,0,0,0,0,0,0x46 };
    static const u8 iid_imalloc[16] = { 0x02,0,0,0, 0,0, 0,0, 0xc0,0,0,0,0,0,0,0x46 };   /* 00000002-0000-0000-C000-000000000046 */
    if (!out) return 0x80004003u;                                                        /* E_POINTER */
    if (iid && (!memcmp(iid, iid_unknown, 16) || !memcmp(iid, iid_imalloc, 16))) { *out = self; return 0; }
    *out = NULL;
    return 0x80004002u;                                                                  /* E_NOINTERFACE */
}
static u64 __attribute__((ms_abi)) beer_imalloc_addref(void *self) { (void)self; return 1; }
static u64 __attribute__((ms_abi)) beer_imalloc_alloc(void *self, u64 size) { (void)self; return (u64)(uintptr_t)malloc(size ? (size_t)size : 1); }
static u64 __attribute__((ms_abi)) beer_imalloc_realloc(void *self, void *block, u64 size)
{
    (void)self;
    if (block && !size) { free(block); return 0; }
    return (u64)(uintptr_t)realloc(block, size ? (size_t)size : 1);
}
static u64 __attribute__((ms_abi)) beer_imalloc_free(void *self, void *block) { (void)self; free(block); return 0; }
static u64 __attribute__((ms_abi)) beer_imalloc_get_size(void *self, void *block)
    { (void)self; return block ? (u64)malloc_usable_size(block) : (u64)-1; }
static u64 __attribute__((ms_abi)) beer_imalloc_did_alloc(void *self, void *block) { (void)self; return block ? (u64)-1 : 0; }   /* -1: cannot tell */
static u64 __attribute__((ms_abi)) beer_imalloc_minimize(void *self) { (void)self; return 0; }
static u64 __attribute__((ms_abi)) beer_CoGetMalloc(u32 context, void **out)
{
    static u64 vtable[9];
    static void *object[1];
    static int ready;
    if (!out) return 0x80004003u;
    if (context != 1) { *out = NULL; return 0x80070057u; }                               /* only MEMCTX_TASK */
    if (!ready) {
        const u64 methods[9] = {
            (u64)(uintptr_t)beer_imalloc_query, (u64)(uintptr_t)beer_imalloc_addref, (u64)(uintptr_t)beer_imalloc_addref,
            (u64)(uintptr_t)beer_imalloc_alloc, (u64)(uintptr_t)beer_imalloc_realloc, (u64)(uintptr_t)beer_imalloc_free,
            (u64)(uintptr_t)beer_imalloc_get_size, (u64)(uintptr_t)beer_imalloc_did_alloc, (u64)(uintptr_t)beer_imalloc_minimize,
        };
        for (size_t i = 0; i < 9; ++i) vtable[i] = (i == 1 || i == 2) ? methods[i] : beer_guest_com_entry(methods[i], "IMalloc", i);
        object[0] = vtable;
        ready = 1;
    }
    *out = object;
    return 0;
}
/* SHLWAPI PathIsRelative: FALSE for a rooted path ("\\x", "/x", "\\\\server") or one with a drive ("C:..."), TRUE
 * otherwise (also for an empty string). ccprotocal.dll (WWM's FFmpeg "cc:" protocol) prefixes its own directory to
 * relative handler names; the old stub answered "absolute", so it loaded videores.dll from the wrong folder and every
 * in-game video stayed black. PathIsDirectory: the attributes of an existing directory, else FALSE. */
static int beer_path_is_relative_chars(u32 c0, u32 c1)
{
    if (c0 == '\\' || c0 == '/') return 0;
    if (c0 && c1 == ':') return 0;
    return 1;
}
static u64 __attribute__((ms_abi)) beer_PathIsRelativeA(const char *path)
    { return !path ? 1 : (u64)beer_path_is_relative_chars((u8)path[0], path[0] ? (u8)path[1] : 0); }
static u64 __attribute__((ms_abi)) beer_PathIsRelativeW(const u16 *path)
    { return !path ? 1 : (u64)beer_path_is_relative_chars(path[0], path[0] ? path[1] : 0); }
static u64 __attribute__((ms_abi)) beer_PathIsDirectoryA(const char *path)
{
    const u32 attributes = path ? (u32)impl_GetFileAttributesA(path) : 0xffffffffu;
    return attributes != 0xffffffffu && (attributes & 0x10) ? 0x10 : 0;   /* FILE_ATTRIBUTE_DIRECTORY */
}
static u64 __attribute__((ms_abi)) beer_PathIsDirectoryW(const u16 *path)
{
    const u32 attributes = path ? (u32)impl_GetFileAttributesW(path) : 0xffffffffu;
    return attributes != 0xffffffffu && (attributes & 0x10) ? 0x10 : 0;
}

/* USER32 CharNext/CharPrev, as Wine implements them: one UTF-16 unit (no surrogate or combining handling), never
 * past the terminator or before the start. The ANSI forms step one byte: Beer's ANSI code page is single-byte.
 * NetEase's login protocol module walks its XML with CharNextW; the old stubs returned NULL and it crashed. */
static u64 __attribute__((ms_abi)) beer_CharNextW(const u16 *x)
    { return (u64)(uintptr_t)(x && *x ? x + 1 : x); }
static u64 __attribute__((ms_abi)) beer_CharPrevW(const u16 *start, const u16 *x)
    { return (u64)(uintptr_t)(x > start ? x - 1 : start); }
static u64 __attribute__((ms_abi)) beer_CharNextA(const char *x)
    { return (u64)(uintptr_t)(x && *x ? x + 1 : x); }
static u64 __attribute__((ms_abi)) beer_CharNextExA(u16 codepage, const char *x, u32 flags)
    { (void)codepage; (void)flags; return (u64)(uintptr_t)(x && *x ? x + 1 : x); }
static u64 __attribute__((ms_abi)) beer_CharPrevA(const char *start, const char *x)
    { return (u64)(uintptr_t)(x > start ? x - 1 : start); }
static u64 __attribute__((ms_abi)) beer_CharPrevExA(u16 codepage, const char *start, const char *x, u32 flags)
    { (void)codepage; (void)flags; return (u64)(uintptr_t)(x > start ? x - 1 : start); }

/* bcryptprimitives!ProcessPrng: the system RNG Rust's standard library uses on Windows (it aborts the process when the
 * import is missing; WWM's Discord SDK did). Always succeeds, as on Windows. */
static u64 __attribute__((ms_abi)) beer_ProcessPrng(u8 *data, u64 length)
{
    for (u64 done = 0; data && done < length; ) {
        const ssize_t got = getrandom(data + done, (size_t)(length - done), 0);
        if (got > 0) done += (u64)got;
        else if (errno != EINTR) break;
    }
    return 1;
}
/* ws2_32!GetHostNameW: gethostname as UTF-16 (the host name is ASCII). */
static u64 __attribute__((ms_abi)) beer_GetHostNameW(u16 *name, s32 length)
{
    if (!name || length <= 0) { beer_wsa_set_error(10014); return (u32)-1; }   /* WSAEFAULT */
    char host[256];
    if (gethostname(host, sizeof(host)) != 0) snprintf(host, sizeof(host), "BEER-PC");
    host[sizeof(host) - 1] = 0;
    const size_t n = strlen(host);
    if ((size_t)length <= n) { beer_wsa_set_error(10014); return (u32)-1; }
    for (size_t i = 0; i <= n; ++i) name[i] = (u8)host[i];
    return 0;
}
/* SetWinEventHook: accessibility events (foreground, focus, ...) are not generated, so the hook is valid and silent. */
static u64 __attribute__((ms_abi)) beer_SetWinEventHook(u32 event_min, u32 event_max, u64 module, u64 proc, u32 pid,
                                                        u32 tid, u32 flags)
{
    (void)event_min; (void)event_max; (void)module; (void)pid; (void)tid; (void)flags;
    static u64 next = 0xE7000000u;
    if (!proc) { g_last_error = 87; return 0; }
    return __atomic_add_fetch(&next, 1, __ATOMIC_RELAXED);
}
static u64 __attribute__((ms_abi)) beer_UnhookWinEvent(u64 hook) { return hook ? 1 : 0; }
/* powrprof!PowerRegisterSuspendResumeNotification: registration succeeds; Beer's process never sees a suspend. */
static u64 __attribute__((ms_abi)) beer_PowerRegisterSuspendResumeNotification(u32 flags, u64 recipient, u64 *handle)
{
    (void)flags; (void)recipient;
    static u64 next = 0xE8000000u;
    if (!handle) return 87;
    *handle = __atomic_add_fetch(&next, 1, __ATOMIC_RELAXED);
    return 0;                                                          /* ERROR_SUCCESS */
}
static u64 __attribute__((ms_abi)) beer_PowerUnregisterSuspendResumeNotification(u64 handle) { return handle ? 0 : 6; }

/* SHGetMalloc: the shell's allocator is the COM task allocator (PIDLs come from CoTaskMemAlloc). */
static u64 __attribute__((ms_abi)) beer_SHGetMalloc(void **out) { return beer_CoGetMalloc(1, out); }

#pragma GCC diagnostic pop
