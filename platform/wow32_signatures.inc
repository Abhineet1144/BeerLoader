/* Generated signature table (stdcall argument bytes) for imports a 32-bit program may call.
 * FWD entries use the shared native implementation; the others have no 32-bit-safe implementation yet and fail
 * with a defined error while keeping the guest stack balanced (an unknown import used to leave it corrupt). */
#define W32_SIG_FWD \
    W32_FWD("CompareFileTime", 8), \
    W32_FWD("CreateNamedPipeW", 32), \
    W32_FWD("GetComputerNameW", 8), \
    W32_FWD("GetCurrentThread", 0), \
    W32_FWD("GetDriveTypeW", 4), \
    W32_FWD("GetLogicalDrives", 0), \
    W32_FWD("GetPrivateProfileStringW", 24), \
    W32_FWD("GetShortPathNameW", 12), \
    W32_FWD("OpenProcess", 12), \
    W32_FWD("SetThreadLocale", 4), \
    W32_FWD("SwitchToThread", 0), \
    W32_FWD("WritePrivateProfileStringW", 16), \
    W32_FWD("Arc", 36), \
    W32_FWD("BitBlt", 36), \
    W32_FWD("Chord", 36), \
    W32_FWD("CreateBitmap", 20), \
    W32_FWD("CreateCompatibleBitmap", 12), \
    W32_FWD("CreateDCW", 16), \
    W32_FWD("CreateDIBitmap", 24), \
    W32_FWD("CreateICW", 16), \
    W32_FWD("CreatePenIndirect", 4), \
    W32_FWD("CreateRectRgn", 16), \
    W32_FWD("CreateSolidBrush", 4), \
    W32_FWD("Ellipse", 20), \
    W32_FWD("EndDoc", 4), \
    W32_FWD("EndPage", 4), \
    W32_FWD("ExcludeClipRect", 20), \
    W32_FWD("ExtFloodFill", 20), \
    W32_FWD("ExtTextOutW", 32), \
    W32_FWD("GetBitmapBits", 12), \
    W32_FWD("GetClipBox", 8), \
    W32_FWD("GetCurrentPositionEx", 8), \
    W32_FWD("GetDCOrgEx", 8), \
    W32_FWD("GetDIBits", 28), \
    W32_FWD("GetPaletteEntries", 16), \
    W32_FWD("GetPixel", 12), \
    W32_FWD("GetRgnBox", 8), \
    W32_FWD("GetSystemPaletteEntries", 16), \
    W32_FWD("GetTextExtentPointW", 16), \
    W32_FWD("GetTextMetricsW", 8), \
    W32_FWD("GetWindowOrgEx", 8), \
    W32_FWD("IntersectClipRect", 20), \
    W32_FWD("LineTo", 12), \
    W32_FWD("MaskBlt", 48), \
    W32_FWD("MoveToEx", 16), \
    W32_FWD("PatBlt", 24), \
    W32_FWD("Pie", 36), \
    W32_FWD("Polyline", 12), \
    W32_FWD("RealizePalette", 4), \
    W32_FWD("Rectangle", 20), \
    W32_FWD("RectVisible", 8), \
    W32_FWD("RestoreDC", 8), \
    W32_FWD("RoundRect", 28), \
    W32_FWD("SaveDC", 4), \
    W32_FWD("SelectClipRgn", 8), \
    W32_FWD("SelectPalette", 12), \
    W32_FWD("SetBkColor", 8), \
    W32_FWD("SetBkMode", 8), \
    W32_FWD("SetBrushOrgEx", 16), \
    W32_FWD("SetPixel", 16), \
    W32_FWD("SetROP2", 8), \
    W32_FWD("SetStretchBltMode", 8), \
    W32_FWD("SetTextAlign", 8), \
    W32_FWD("SetTextColor", 8), \
    W32_FWD("SetViewportOrgEx", 16), \
    W32_FWD("SetWindowOrgEx", 16), \
    W32_FWD("StartPage", 4), \
    W32_FWD("StretchBlt", 44), \
    W32_FWD("StretchDIBits", 52), \
    W32_FWD("AlphaBlend", 44), \
    W32_FWD("GetUserNameW", 8), \
    W32_FWD("RegEnumKeyExW", 32), \
    W32_FWD("RegEnumValueW", 32), \
    W32_FWD("RegFlushKey", 4), \
    W32_FWD("RegQueryInfoKeyW", 48), \
    W32_FWD("ActivateKeyboardLayout", 8), \
    W32_FWD("AdjustWindowRectEx", 16), \
    W32_FWD("AppendMenuW", 16), \
    W32_FWD("BringWindowToTop", 4), \
    W32_FWD("CallNextHookEx", 16), \
    W32_FWD("CheckMenuItem", 12), \
    W32_FWD("ClientToScreen", 8), \
    W32_FWD("CreateMenu", 0), \
    W32_FWD("CreatePopupMenu", 0), \
    W32_FWD("DefFrameProcW", 20), \
    W32_FWD("DefMDIChildProcW", 16), \
    W32_FWD("DestroyCursor", 4), \
    W32_FWD("DestroyIcon", 4), \
    W32_FWD("DestroyMenu", 4), \
    W32_FWD("DrawEdge", 16), \
    W32_FWD("DrawFocusRect", 8), \
    W32_FWD("DrawFrameControl", 16), \
    W32_FWD("DrawIconEx", 36), \
    W32_FWD("DrawMenuBar", 4), \
    W32_FWD("DrawTextW", 20), \
    W32_FWD("EnableMenuItem", 12), \
    W32_FWD("EnableScrollBar", 12), \
    W32_FWD("FillRect", 12), \
    W32_FWD("FindWindowW", 8), \
    W32_FWD("FrameRect", 12), \
    W32_FWD("GetCapture", 0), \
    W32_FWD("GetClassLongW", 8), \
    W32_FWD("GetClassNameW", 12), \
    W32_FWD("GetClientRect", 8), \
    W32_FWD("GetCursor", 0), \
    W32_FWD("GetCursorPos", 4), \
    W32_FWD("GetDCEx", 12), \
    W32_FWD("GetFocus", 0), \
    W32_FWD("GetForegroundWindow", 0), \
    W32_FWD("GetKeyboardState", 4), \
    W32_FWD("GetKeyNameTextW", 12), \
    W32_FWD("GetKeyState", 4), \
    W32_FWD("GetLastActivePopup", 4), \
    W32_FWD("GetMenu", 4), \
    W32_FWD("GetMenuItemCount", 4), \
    W32_FWD("GetMenuState", 12), \
    W32_FWD("GetMessagePos", 0), \
    W32_FWD("GetPropW", 8), \
    W32_FWD("GetScrollInfo", 12), \
    W32_FWD("GetSubMenu", 8), \
    W32_FWD("GetSysColor", 4), \
    W32_FWD("GetSysColorBrush", 4), \
    W32_FWD("GetTopWindow", 4), \
    W32_FWD("GetWindow", 8), \
    W32_FWD("GetWindowPlacement", 8), \
    W32_FWD("GetWindowTextW", 12), \
    W32_FWD("InflateRect", 12), \
    W32_FWD("InsertMenuW", 20), \
    W32_FWD("IntersectRect", 12), \
    W32_FWD("InvalidateRect", 12), \
    W32_FWD("IsIconic", 4), \
    W32_FWD("IsRectEmpty", 4), \
    W32_FWD("IsZoomed", 4), \
    W32_FWD("LoadBitmapW", 8), \
    W32_FWD("LoadKeyboardLayoutW", 8), \
    W32_FWD("MapVirtualKeyW", 8), \
    W32_FWD("OffsetRect", 12), \
    W32_FWD("PtInRect", 12), \
    W32_FWD("ReleaseCapture", 0), \
    W32_FWD("RemoveMenu", 12), \
    W32_FWD("RemovePropW", 8), \
    W32_FWD("ScreenToClient", 8), \
    W32_FWD("ScrollWindowEx", 32), \
    W32_FWD("SetCapture", 4), \
    W32_FWD("SetCursor", 4), \
    W32_FWD("SetFocus", 4), \
    W32_FWD("SetMenu", 8), \
    W32_FWD("SetParent", 8), \
    W32_FWD("SetPropW", 12), \
    W32_FWD("SetRect", 20), \
    W32_FWD("SetRectEmpty", 4), \
    W32_FWD("SetScrollInfo", 16), \
    W32_FWD("SetWindowPlacement", 8), \
    W32_FWD("ShowScrollBar", 12), \
    W32_FWD("TrackPopupMenu", 28), \
    W32_FWD("TranslateMDISysAccel", 8), \
    W32_FWD("UnhookWindowsHookEx", 4), \
    W32_FWD("UnregisterClassW", 8), \
    W32_FWD("WaitMessage", 0), \
    W32_FWD("WindowFromPoint", 8), \
    W32_FWD("ShellExecuteW", 24), \
    W32_FWD("ImageList_Add", 12), \
    W32_FWD("ImageList_BeginDrag", 16), \
    W32_FWD("ImageList_Copy", 20), \
    W32_FWD("ImageList_Create", 20), \
    W32_FWD("ImageList_Destroy", 4), \
    W32_FWD("ImageList_DragEnter", 12), \
    W32_FWD("ImageList_DragLeave", 4), \
    W32_FWD("ImageList_DragMove", 8), \
    W32_FWD("ImageList_DragShowNolock", 4), \
    W32_FWD("ImageList_DrawEx", 40), \
    W32_FWD("ImageList_EndDrag", 0), \
    W32_FWD("ImageList_GetIcon", 12), \
    W32_FWD("ImageList_GetImageCount", 4), \
    W32_FWD("ImageList_Remove", 8), \
    W32_FWD("ImageList_Replace", 16), \
    W32_FWD("ImageList_SetImageCount", 8), \
    W32_FWD("_TrackMouseEvent", 4), \
    W32_FWD("CoTaskMemFree", 4),
#define W32_SIG_FAIL \
    {"DeviceIoControl", w32_unsupported, 32}, \
    {"GetOverlappedResult", w32_unsupported, 16}, \
    {"SetNamedPipeHandleState", w32_unsupported, 16}, \
    {"TransactNamedPipe", w32_unsupported, 28}, \
    {"VirtualQueryEx", w32_VirtualQueryEx, 16}, \
    {"GetProfileStringW", w32_unsupported, 20}, \
    {"GlobalFindAtomW", w32_unsupported, 4}, \
    {"OpenMutexW", w32_unsupported, 12}, \
    {"WriteProfileStringW", w32_unsupported, 12}, \
    {"CreateBrushIndirect", w32_unsupported, 4}, \
    {"EnumFontFamiliesExW", w32_unsupported, 20}, \
    {"StartDocW", w32_unsupported, 8}, \
    {"AddFontResourceW", w32_unsupported, 4}, \
    {"CreateHalftonePalette", w32_unsupported, 4}, \
    {"EnumFontsW", w32_unsupported, 16}, \
    {"FrameRgn", w32_unsupported, 20}, \
    {"GetBrushOrgEx", w32_unsupported, 8}, \
    {"LineDDA", w32_unsupported, 24}, \
    {"RemoveFontResourceW", w32_unsupported, 4}, \
    {"SetAbortProc", w32_unsupported, 8}, \
    {"SetDIBColorTable", w32_unsupported, 16}, \
    {"UnrealizeObject", w32_unsupported, 4}, \
    {"OpenThreadToken", w32_unsupported, 16}, \
    {"GetTokenInformation", w32_unsupported, 20}, \
    {"RegDeleteKeyW", w32_unsupported, 8}, \
    {"RegDeleteValueW", w32_unsupported, 8}, \
    {"ClosePrinter", w32_unsupported, 4}, \
    {"DocumentPropertiesW", w32_unsupported, 24}, \
    {"EnumPrintersW", w32_unsupported, 28}, \
    {"OpenPrinterW", w32_unsupported, 12}, \
    {"GetDefaultPrinterW", w32_unsupported, 8}, \
    {"EnumChildWindows", w32_unsupported, 12}, \
    {"GetIconInfo", w32_unsupported, 8}, \
    {"GetMenuItemInfoW", w32_unsupported, 16}, \
    {"InsertMenuItemW", w32_unsupported, 16}, \
    {"SetMenuItemInfoW", w32_unsupported, 16}, \
    {"SetWindowsHookExW", w32_unsupported, 16}, \
    {"CharToOemBuffA", w32_unsupported, 12}, \
    {"ChildWindowFromPoint", w32_unsupported, 12}, \
    {"CopyIcon", w32_unsupported, 4}, \
    {"CreateAcceleratorTableW", w32_unsupported, 8}, \
    {"CreateIcon", w32_unsupported, 28}, \
    {"DrawIcon", w32_unsupported, 16}, \
    {"DrawTextExW", w32_unsupported, 24}, \
    {"FindWindowExW", w32_unsupported, 16}, \
    {"GetKeyboardLayoutNameW", w32_unsupported, 4}, \
    {"GetMenuItemID", w32_unsupported, 8}, \
    {"GetMenuStringW", w32_unsupported, 20}, \
    {"GetScrollPos", w32_unsupported, 8}, \
    {"GetScrollRange", w32_unsupported, 16}, \
    {"IsChild", w32_unsupported, 8}, \
    {"IsDialogMessageA", w32_unsupported, 8}, \
    {"IsDialogMessageW", w32_unsupported, 8}, \
    {"OemToCharBuffA", w32_unsupported, 12}, \
    {"ReplyMessage", w32_unsupported, 4}, \
    {"ScrollWindow", w32_unsupported, 20}, \
    {"SetScrollPos", w32_unsupported, 16}, \
    {"SetScrollRange", w32_unsupported, 20}, \
    {"ShowOwnedPopups", w32_unsupported, 8}, \
    {"WaitForInputIdle", w32_unsupported, 8}, \
    {"ShellExecuteExW", w32_unsupported, 4}, \
    {"SHGetFileInfoW", w32_unsupported, 20}, \
    {"SHGetPathFromIDListW", w32_unsupported, 8}, \
    {"ExtractIconW", w32_unsupported, 12}, \
    {"SHBrowseForFolderW", w32_unsupported, 4}, \
    {"SHChangeNotify", w32_unsupported, 16}, \
    {"GetOpenFileNameW", w32_unsupported, 4}, \
    {"GetSaveFileNameW", w32_unsupported, 4}, \
    {"ImageList_Draw", w32_unsupported, 24}, \
    {"ImageList_GetBkColor", w32_unsupported, 4}, \
    {"ImageList_GetDragImage", w32_unsupported, 8}, \
    {"ImageList_GetIconSize", w32_unsupported, 12}, \
    {"ImageList_GetImageInfo", w32_unsupported, 12}, \
    {"ImageList_LoadImageW", w32_unsupported, 28}, \
    {"ImageList_Read", w32_unsupported, 4}, \
    {"ImageList_ReplaceIcon", w32_unsupported, 12}, \
    {"ImageList_SetBkColor", w32_unsupported, 8}, \
    {"ImageList_SetIconSize", w32_unsupported, 12}, \
    {"ImageList_SetOverlayImage", w32_unsupported, 12}, \
    {"ImageList_Write", w32_unsupported, 8}, \
    {"FlatSB_GetScrollInfo", w32_unsupported, 12}, \
    {"FlatSB_GetScrollPos", w32_unsupported, 8}, \
    {"FlatSB_SetScrollInfo", w32_unsupported, 16}, \
    {"FlatSB_SetScrollPos", w32_unsupported, 16}, \
    {"FlatSB_SetScrollProp", w32_unsupported, 16}, \
    {"InitializeFlatSB", w32_unsupported, 4},
#define W32_SIG_HR \
    {"GetErrorInfo", w32_unsupported_hr, 8}, \
    {"SafeArrayGetElement", w32_unsupported_hr, 12}, \
    {"SafeArrayPutElement", w32_unsupported_hr, 12}, \
    {"VariantCopyInd", w32_unsupported_hr, 8}, \
    {"LoadTypeLib", w32_unsupported_hr, 8}, \
    {"RegisterTypeLib", w32_unsupported_hr, 12}, \
    {"GetActiveObject", w32_unsupported_hr, 12}, \
    {"CLSIDFromProgID", w32_unsupported_hr, 8}, \
    {"CLSIDFromString", w32_unsupported_hr, 8}, \
        {"StringFromCLSID", w32_unsupported_hr, 8}, \
    {"SHGetMalloc", w32_unsupported_hr, 4}, \
    {"CoDisconnectObject", w32_unsupported_hr, 8}, \
    {"CoFreeUnusedLibraries", w32_unsupported_hr, 0},
#define W32_SIG_WNET \
    {"WNetCloseEnum", w32_unsupported_net, 4}, \
    {"WNetEnumResourceW", w32_unsupported_net, 16}, \
    {"WNetGetConnectionW", w32_unsupported_net, 12}, \
    {"WNetGetUniversalNameW", w32_unsupported_net, 16}, \
    {"WNetOpenEnumW", w32_unsupported_net, 20},
