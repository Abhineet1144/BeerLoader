/* Win32 exports that protected executables resolve by name but a plain run does not need.
 * Generated from a real RE4 start-up log; unimplemented ones return 0 (FALSE/NULL). */
static u64 __attribute__((ms_abi)) stub_GetConsoleMode(u64 h, u32 *mode){ (void)h; if(mode)*mode=0; return 0; }
static u64 __attribute__((ms_abi)) stub_ReadProcessMemory(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_RemoveDirectoryW(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SetEnvironmentVariableW(void){ return 1; }
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
static u64 __attribute__((ms_abi)) stub_FlushInstructionCache(void){ return 1; }
static u64 __attribute__((ms_abi)) stub_VirtualAllocEx(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_VirtualProtectEx(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_HeapDestroy(void){ return 1; }
static u64 __attribute__((ms_abi)) stub_OpenProcess(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetExitCodeProcess(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_CreateRemoteThread(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_WriteProcessMemory(void){ return 0; }
/* GetThreadContext: Beer cannot read another thread's registers, but it never sets hardware breakpoints, so the debug
 * registers are reported as clear (CONTEXT_DEBUG_REGISTERS: Dr0-Dr3, Dr6, Dr7 at +0x48..+0x77 of the x64 CONTEXT).
 * Failing the call left the caller's buffer as stack garbage, which copy protection (RDR2's Arxan TLS callback)
 * reads as a debugger's breakpoints and answers by sabotaging the game ("Social Club error"). */
static u64 __attribute__((ms_abi)) beer_GetThreadContext(u64 thread, u8 *context)
{
    (void)thread;
    if (!context) { g_last_error = 998; return 0; }                 /* ERROR_NOACCESS */
    const u32 flags = *(u32 *)(context + 0x30);
    if ((flags & 0x100010u) == 0x100010u) memset(context + 0x48, 0, 0x30);
    return 1;
}
static u64 __attribute__((ms_abi)) stub_GlobalDeleteAtom(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_DeviceIoControl(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_FindClose(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_MulDiv(s32 a, s32 b, s32 c){ if(!c) return (u64)(s64)-1; return (u64)(s64)(s32)(((int64_t)a*b + (c/2))/c); }
static u64 __attribute__((ms_abi)) stub_FileTimeToDosDateTime(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_VirtualLock(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_VirtualUnlock(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_IsBadReadPtr(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_IsBadWritePtr(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetThreadLocale(void){ return 0x409; }
static u64 __attribute__((ms_abi)) stub_SetThreadLocale(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SetThreadContext(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetProcessIdOfThread(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetThreadId(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_Module32First(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_Module32Next(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_DebugBreak(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SafeArrayCreate(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SafeArrayRedim(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SafeArrayGetUBound(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SafeArrayGetLBound(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SafeArrayAccessData(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SafeArrayUnaccessData(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SafeArrayGetElement(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SafeArrayPutElement(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SafeArrayPtrOfIndex(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_VariantChangeTypeEx(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_VariantCopy(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_CharUpperBuffW(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_CharLowerBuffW(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_UnregisterClassA(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetClassInfoA(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_RegisterClipboardFormatA(void){ return 0; }
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
static u64 __attribute__((ms_abi)) stub_GetClassInfoW(void){ return 0; }
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
static u64 __attribute__((ms_abi)) stub_GetCapture(void){ return 0; }
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
static u64 __attribute__((ms_abi)) stub_SetWindowRgn(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_InvalidateRgn(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_RedrawWindow(void){ return 0; }
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
static u64 __attribute__((ms_abi)) stub_ClientToScreen(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_MapWindowPoints(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_WindowFromPoint(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetSysColor(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetSysColorBrush(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SetSysColors(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_DrawFocusRect(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_FrameRect(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetParent(void){ return 0; }
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
static u64 __attribute__((ms_abi)) stub_CombineRgn(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_CreateBrushIndirect(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_CreateDIBitmap(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_CreateEllipticRgn(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_CreatePenIndirect(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_CreatePatternBrush(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_CreateRectRgn(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_CreateRoundRectRgn(void){ return 0; }
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
static u64 __attribute__((ms_abi)) stub_GetRgnBox(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetTextAlign(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetViewportExtEx(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetViewportOrgEx(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetWindowExtEx(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_GetWindowOrgEx(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_IntersectClipRect(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_LineTo(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_MaskBlt(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_OffsetRgn(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_Pie(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_PaintRgn(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_PtInRegion(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_RectInRegion(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_RectVisible(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_RealizePalette(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_RoundRect(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SelectClipRgn(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_ExtSelectClipRgn(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SelectPalette(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SetMapMode(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SetPolyFillMode(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_SetRectRgn(void){ return 0; }
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
static u64 __attribute__((ms_abi)) stub_NtOpenSection(void){ return 0xC0000022u; }
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
static u64 __attribute__((ms_abi)) stub_EnumProcesses(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_VariantClear(void){ return 0; }
static u64 __attribute__((ms_abi)) stub_VariantInit(void){ return 0; }
