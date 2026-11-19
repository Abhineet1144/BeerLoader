/* IShellLink / IPersistFile for 32-bit programs (Inno Setup creates its [Icons] shortcuts through them).
 * The COM object lives in the 32-bit heap and its vtables point at this layer's thunks, so 32-bit code calls it like
 * any in-process COM server. Linux has no .lnk shell: Save() writes the link's properties as a small text file at the
 * requested path (so the file exists, can be removed again and tells what it would launch). */
#define W32_SL_LINK_METHODS    21
#define W32_SL_PERSIST_METHODS 9
typedef struct {
    u32 vtable_link, vtable_persist;      /* the two interface pointers: this and this+4 */
    u32 refs, wide;
    char path[512], arguments[512], directory[512], description[256], icon[512], saved_to[512];
    s32 icon_index, show_command; u32 hotkey;
} W32ShellLink;
static u32 g_w32_sl_vtable_link, g_w32_sl_vtable_persist;
static const u8 W32_IID_UNKNOWN[16] = {0,0,0,0,0,0,0,0,0xC0,0,0,0,0,0,0,0x46};
static int w32_guid_is(const u8 *g, u32 d1, u16 d2, u16 d3)
{
    u8 want[16] = {0,0,0,0,0,0,0,0,0xC0,0,0,0,0,0,0,0x46};
    memcpy(want, &d1, 4); memcpy(want + 4, &d2, 2); memcpy(want + 6, &d3, 2);
    return !memcmp(g, want, 16);
}
static void w32_sl_text(const W32ShellLink *l, const void *in, char *out, size_t size)
{
    if (l->wide) w32_narrow(in, out, size); else snprintf(out, size, "%s", (const char *)in);
}
static u32 w32_sl_put(const W32ShellLink *l, const char *text, u32 buffer, u32 chars)
{
    if (!buffer || !chars) return 0;
    const size_t n = strlen(text) < chars - 1 ? strlen(text) : chars - 1;
    for (size_t i = 0; i < n; ++i) { if (l->wide) ((u16 *)(uintptr_t)buffer)[i] = (unsigned char)text[i]; else ((char *)(uintptr_t)buffer)[i] = text[i]; }
    if (l->wide) ((u16 *)(uintptr_t)buffer)[n] = 0; else ((char *)(uintptr_t)buffer)[n] = 0;
    return 0;
}
/* One handler for both interfaces: `persist` selects the IPersistFile table, `index` the method. */
static u64 w32_shelllink_call(int persist, int index, const u32 *args)
{
    W32ShellLink *l = (W32ShellLink *)(uintptr_t)(args[0] - (persist ? 4 : 0));
    char text[512];
    switch (persist ? 100 + index : index) {
    case 0: case 100: {                                                          /* QueryInterface */
        const u8 *iid = W32_PTR(1);
        u32 *out = W32_PTR(2);
        if (!iid || !out) return 0x80004003u;                                    /* E_POINTER */
        if (w32_guid_is(iid, 0x00000000, 0, 0) || w32_guid_is(iid, 0x000214EE, 0, 0) || w32_guid_is(iid, 0x000214F9, 0, 0))
            *out = (u32)(uintptr_t)l;
        else if (w32_guid_is(iid, 0x0000010B, 0, 0))                             /* IPersistFile */
            *out = (u32)(uintptr_t)l + 4;
        else { *out = 0; return 0x80004002u; }                                   /* E_NOINTERFACE */
        ++l->refs;
        return 0;
    }
    case 1: case 101: return ++l->refs;
    case 2: case 102:
        if (--l->refs) return l->refs;
        w32_heap_free(l);
        return 0;
    case 3:  w32_sl_put(l, l->path, args[1], args[2]); return 0;                 /* GetPath */
    case 4:  case 5: return 1;                                                   /* Get/SetIDList: no PIDLs (S_FALSE) */
    case 6:  w32_sl_put(l, l->description, args[1], args[2]); return 0;
    case 7:  w32_sl_text(l, W32_PTR(1), l->description, sizeof(l->description)); return 0;
    case 8:  w32_sl_put(l, l->directory, args[1], args[2]); return 0;
    case 9:  w32_sl_text(l, W32_PTR(1), l->directory, sizeof(l->directory)); return 0;
    case 10: w32_sl_put(l, l->arguments, args[1], args[2]); return 0;
    case 11: w32_sl_text(l, W32_PTR(1), l->arguments, sizeof(l->arguments)); return 0;
    case 12: if (args[1]) *(u16 *)W32_PTR(1) = (u16)l->hotkey; return 0;
    case 13: l->hotkey = args[1] & 0xffff; return 0;
    case 14: if (args[1]) *(s32 *)W32_PTR(1) = l->show_command; return 0;
    case 15: l->show_command = (s32)args[1]; return 0;
    case 16: w32_sl_put(l, l->icon, args[1], args[2]); if (args[3]) *(s32 *)W32_PTR(3) = l->icon_index; return 0;
    case 17: w32_sl_text(l, W32_PTR(1), l->icon, sizeof(l->icon)); l->icon_index = (s32)args[2]; return 0;
    case 18: return 0;                                                           /* SetRelativePath */
    case 19: return 0;                                                           /* Resolve: the target is as given */
    case 20: w32_sl_text(l, W32_PTR(1), l->path, sizeof(l->path)); return 0;     /* SetPath */
    case 103: if (args[1]) memset(W32_PTR(1), 0, 16); return 0;                  /* GetClassID */
    case 104: return 1;                                                          /* IsDirty: S_FALSE */
    case 105: return 0;                                                          /* Load */
    case 106: {                                                                  /* Save(path, remember) */
        const u16 *wide_path = W32_PTR(1);
        char host[PATH_MAX];
        if (!wide_path && l->saved_to[0]) snprintf(text, sizeof(text), "%s", l->saved_to);
        else if (wide_path) { w32_narrow(wide_path, text, sizeof(text)); snprintf(l->saved_to, sizeof(l->saved_to), "%s", text); }
        else return 0x80004005u;
        if (!resolve_windows_host_path(text, host, sizeof(host))) return 0x80070003u;   /* path not found */
        FILE *f = fopen(host, "w");
        if (!f) return 0x80070005u;                                              /* access denied */
        fprintf(f, "[Beer shortcut]\nTarget=%s\nArguments=%s\nWorkingDirectory=%s\nDescription=%s\nIcon=%s,%d\nShowCmd=%d\n",
                l->path, l->arguments, l->directory, l->description, l->icon, l->icon_index, l->show_command);
        fclose(f);
        return 0;
    }
    case 107: return 0;                                                          /* SaveCompleted */
    case 108: if (args[1]) *(u32 *)W32_PTR(1) = 0; return 1;                     /* GetCurFile: none (S_FALSE) */
    default: return 0x80004001u;
    }
}
#define W32_SL_LINK(n)    static u64 w32_sl_link_##n(const u32 *a)    { return w32_shelllink_call(0, n, a); }
#define W32_SL_PERSIST(n) static u64 w32_sl_persist_##n(const u32 *a) { return w32_shelllink_call(1, n, a); }
W32_SL_LINK(0) W32_SL_LINK(1) W32_SL_LINK(2) W32_SL_LINK(3) W32_SL_LINK(4) W32_SL_LINK(5) W32_SL_LINK(6)
W32_SL_LINK(7) W32_SL_LINK(8) W32_SL_LINK(9) W32_SL_LINK(10) W32_SL_LINK(11) W32_SL_LINK(12) W32_SL_LINK(13)
W32_SL_LINK(14) W32_SL_LINK(15) W32_SL_LINK(16) W32_SL_LINK(17) W32_SL_LINK(18) W32_SL_LINK(19) W32_SL_LINK(20)
W32_SL_PERSIST(0) W32_SL_PERSIST(1) W32_SL_PERSIST(2) W32_SL_PERSIST(3) W32_SL_PERSIST(4) W32_SL_PERSIST(5)
W32_SL_PERSIST(6) W32_SL_PERSIST(7) W32_SL_PERSIST(8)
#define W32_SL_API_ENTRIES \
    {"IShellLink::QueryInterface", w32_sl_link_0, 12}, {"IShellLink::AddRef", w32_sl_link_1, 4}, \
    {"IShellLink::Release", w32_sl_link_2, 4}, {"IShellLink::GetPath", w32_sl_link_3, 20}, \
    {"IShellLink::GetIDList", w32_sl_link_4, 8}, {"IShellLink::SetIDList", w32_sl_link_5, 8}, \
    {"IShellLink::GetDescription", w32_sl_link_6, 12}, {"IShellLink::SetDescription", w32_sl_link_7, 8}, \
    {"IShellLink::GetWorkingDirectory", w32_sl_link_8, 12}, {"IShellLink::SetWorkingDirectory", w32_sl_link_9, 8}, \
    {"IShellLink::GetArguments", w32_sl_link_10, 12}, {"IShellLink::SetArguments", w32_sl_link_11, 8}, \
    {"IShellLink::GetHotkey", w32_sl_link_12, 8}, {"IShellLink::SetHotkey", w32_sl_link_13, 8}, \
    {"IShellLink::GetShowCmd", w32_sl_link_14, 8}, {"IShellLink::SetShowCmd", w32_sl_link_15, 8}, \
    {"IShellLink::GetIconLocation", w32_sl_link_16, 16}, {"IShellLink::SetIconLocation", w32_sl_link_17, 12}, \
    {"IShellLink::SetRelativePath", w32_sl_link_18, 12}, {"IShellLink::Resolve", w32_sl_link_19, 12}, \
    {"IShellLink::SetPath", w32_sl_link_20, 8}, \
    {"IPersistFile::QueryInterface", w32_sl_persist_0, 12}, {"IPersistFile::AddRef", w32_sl_persist_1, 4}, \
    {"IPersistFile::Release", w32_sl_persist_2, 4}, {"IPersistFile::GetClassID", w32_sl_persist_3, 8}, \
    {"IPersistFile::IsDirty", w32_sl_persist_4, 4}, {"IPersistFile::Load", w32_sl_persist_5, 12}, \
    {"IPersistFile::Save", w32_sl_persist_6, 12}, {"IPersistFile::SaveCompleted", w32_sl_persist_7, 8}, \
    {"IPersistFile::GetCurFile", w32_sl_persist_8, 8},
static const char *const g_w32_sl_names[W32_SL_LINK_METHODS + W32_SL_PERSIST_METHODS] = {
    "IShellLink::QueryInterface", "IShellLink::AddRef", "IShellLink::Release", "IShellLink::GetPath",
    "IShellLink::GetIDList", "IShellLink::SetIDList", "IShellLink::GetDescription", "IShellLink::SetDescription",
    "IShellLink::GetWorkingDirectory", "IShellLink::SetWorkingDirectory", "IShellLink::GetArguments",
    "IShellLink::SetArguments", "IShellLink::GetHotkey", "IShellLink::SetHotkey", "IShellLink::GetShowCmd",
    "IShellLink::SetShowCmd", "IShellLink::GetIconLocation", "IShellLink::SetIconLocation",
    "IShellLink::SetRelativePath", "IShellLink::Resolve", "IShellLink::SetPath",
    "IPersistFile::QueryInterface", "IPersistFile::AddRef", "IPersistFile::Release", "IPersistFile::GetClassID",
    "IPersistFile::IsDirty", "IPersistFile::Load", "IPersistFile::Save", "IPersistFile::SaveCompleted",
    "IPersistFile::GetCurFile" };
static void w32_sl_vtables(void)
{
    if (g_w32_sl_vtable_link) return;
    u32 *link = w32_heap_alloc(W32_SL_LINK_METHODS * 4, 1), *persist = w32_heap_alloc(W32_SL_PERSIST_METHODS * 4, 1);
    for (int i = 0; i < W32_SL_LINK_METHODS; ++i) link[i] = w32_thunk_for("shell32.dll", g_w32_sl_names[i]);
    for (int i = 0; i < W32_SL_PERSIST_METHODS; ++i) persist[i] = w32_thunk_for("shell32.dll", g_w32_sl_names[W32_SL_LINK_METHODS + i]);
    g_w32_sl_vtable_persist = (u32)(uintptr_t)persist;
    g_w32_sl_vtable_link = (u32)(uintptr_t)link;
}
/* CoCreateInstance(clsid, outer, context, iid, ppv): only CLSID_ShellLink is served. */
static u64 w32_CoCreateInstance(const u32 *args)
{
    u32 *out = W32_PTR(4);
    if (!out) return 0x80004003u;
    *out = 0;
    const u8 *clsid = W32_PTR(0), *iid = W32_PTR(3);
    if (!clsid || !iid) return 0x80070057u;
    if (!w32_guid_is(clsid, 0x00021401, 0, 0)) return 0x80040154u;               /* REGDB_E_CLASSNOTREG */
    const int wide = w32_guid_is(iid, 0x000214F9, 0, 0);
    if (!wide && !w32_guid_is(iid, 0x000214EE, 0, 0) && !w32_guid_is(iid, 0x00000000, 0, 0) &&
        !w32_guid_is(iid, 0x0000010B, 0, 0)) return 0x80004002u;                 /* E_NOINTERFACE */
    w32_sl_vtables();
    W32ShellLink *l = w32_heap_alloc(sizeof(*l), 1);
    if (!l) return 0x8007000Eu;
    l->vtable_link = g_w32_sl_vtable_link; l->vtable_persist = g_w32_sl_vtable_persist;
    l->refs = 1; l->wide = (u32)wide; l->show_command = 1;
    *out = (u32)(uintptr_t)l + (w32_guid_is(iid, 0x0000010B, 0, 0) ? 4 : 0);
    return 0;
}
