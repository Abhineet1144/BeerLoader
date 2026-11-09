#!/usr/bin/env python3
"""Regenerates platform/win32_module_exports.h: for every function in loader.c's g_impls table, the Windows system
DLLs that export it. The DLL ownership is read from a directory of Windows-API PE DLLs (default: Wine's x86_64 PE
DLLs, which carry the Windows export names). Only Beer-implemented names are written.

    python3 tools/gen_module_exports.py [dll-directory]
"""
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DLL_DIR = sys.argv[1] if len(sys.argv) > 1 else "/usr/lib/x86_64-linux-gnu/wine/x86_64-windows"
DLLS = """kernel32 kernelbase user32 advapi32 gdi32 shell32 shlwapi ole32 oleaut32 combase winmm imm32 version psapi
iphlpapi setupapi cfgmgr32 crypt32 bcrypt rpcrt4 comctl32 msimg32 ws2_32 wldap32 wtsapi32 userenv dbghelp secur32
powrprof dwmapi uxtheme hid xinput1_4 xinput1_3 dinput8 dsound winhttp wininet mswsock tdh dxva2 evr gdiplus mf
mfplat mfreadwrite comdlg32 wintrust normaliz dnsapi netapi32 avrt propsys shcore sechost win32u ntdll sspicli
bcryptprimitives""".split()


def export_ordinals(path):
    """name -> ordinal for a PE's named exports."""
    data = open(path, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    count = struct.unpack_from("<H", data, pe + 6)[0]
    optional = struct.unpack_from("<H", data, pe + 20)[0]
    sections = [struct.unpack_from("<8sIIII", data, pe + 24 + optional + i * 40) for i in range(count)]

    def offset(rva):
        for _name, vsize, va, rsize, raw in sections:
            if va <= rva < va + max(vsize, rsize):
                return rva - va + raw
        raise ValueError(rva)

    directory = offset(struct.unpack_from("<I", data, pe + 24 + 112)[0])
    base = struct.unpack_from("<I", data, directory + 16)[0]
    names = struct.unpack_from("<I", data, directory + 24)[0]
    name_table = offset(struct.unpack_from("<I", data, directory + 32)[0])
    ordinal_table = offset(struct.unpack_from("<I", data, directory + 36)[0])
    result = {}
    for index in range(names):
        start = offset(struct.unpack_from("<I", data, name_table + 4 * index)[0])
        name = data[start:data.index(b"\0", start)].decode()
        result[name] = base + struct.unpack_from("<H", data, ordinal_table + 2 * index)[0]
    return result


# DLLs whose programs import by ordinal (OLEAUT32's Automation and WS2_32's Winsock ordinals are a fixed Windows ABI).
ORDINAL_DLLS = ["oleaut32", "ws2_32", "d3d12", "mmdevapi", "xinput1_4", "xinput1_3"]   # d3d12: UE resolves D3D12CreateDevice as ordinal 101; UE delay-loads XInputGetState as XINPUT1_4 #2


def export_names(path):
    data = open(path, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    count = struct.unpack_from("<H", data, pe + 6)[0]
    optional = struct.unpack_from("<H", data, pe + 20)[0]
    sections = [struct.unpack_from("<8sIIII", data, pe + 24 + optional + i * 40) for i in range(count)]

    def offset(rva):
        for _name, vsize, va, rsize, raw in sections:
            if va <= rva < va + max(vsize, rsize):
                return rva - va + raw
        raise ValueError(rva)

    export_rva = struct.unpack_from("<I", data, pe + 24 + 112)[0]
    if not export_rva:
        return set()
    directory = offset(export_rva)
    names = struct.unpack_from("<I", data, directory + 24)[0]
    table = offset(struct.unpack_from("<I", data, directory + 32)[0])
    result = set()
    for index in range(names):
        start = offset(struct.unpack_from("<I", data, table + 4 * index)[0])
        result.add(data[start:data.index(b"\0", start)].decode())
    return result


def main():
    source = open(os.path.join(ROOT, "loader.c")).read()
    start = source.index("static ImplEntry g_impls[] = {")
    table = source[start:source.index("\n};", start)]
    # The table also #includes generated tables (platform/win32_missing_exports_table.inc).
    for include in re.findall(r'#include "([^"]+)"', table):
        path = os.path.join(ROOT, include)
        if os.path.exists(path):
            table += open(path).read()
    implemented = set(re.findall(r'\{\s*"([A-Za-z_][A-Za-z0-9_@?$]*)"\s*,', table))
    owners = {}
    for dll in DLLS:
        path = os.path.join(DLL_DIR, dll + ".dll")
        if os.path.exists(path):
            for name in export_names(path):
                owners.setdefault(name, []).append(dll + ".dll")
    # ntdll is included too: besides its Nt/Rtl/Ldr routines it exports names such as VerSetConditionMask.
    pairs = sorted((dll, name) for name in implemented for dll in owners.get(name, []))
    lines = [
        "/* Which Windows system DLL exports each function Beer implements (Windows public API surface).",
        " *",
        " * The synthetic and compatibility system modules publish these names in their PE export tables, in addition to",
        " * the names loaded images import. Protection code (Themida in Where Winds Meet) resolves the program's real imports",
        " * by walking export tables itself, so an implemented function missing from its DLL's table reads as \"entry point",
        " * not found\". Only names with a Beer implementation are listed; loader.c also checks find_impl() before",
        " * publishing. Regenerate with tools/gen_module_exports.py after adding implementations. */",
        "#ifndef BEER_WIN32_MODULE_EXPORTS_H",
        "#define BEER_WIN32_MODULE_EXPORTS_H",
        "",
        "static const char *const beer_module_exports[][2] = {",
    ]
    lines += ['    { "%s", "%s" },' % pair for pair in pairs]
    lines += ["};", "#define BEER_MODULE_EXPORT_COUNT (sizeof(beer_module_exports) / sizeof(*beer_module_exports))", ""]
    ordinals = []
    for dll in ORDINAL_DLLS:
        path = os.path.join(DLL_DIR, dll + ".dll")
        if os.path.exists(path):
            for name, ordinal in sorted(export_ordinals(path).items(), key=lambda item: item[1]):
                if name in implemented:
                    ordinals.append((dll + ".dll", name, ordinal))
    lines += ["/* The Windows ordinal of implemented exports of DLLs that programs import by ordinal. */",
              "static const struct { const char *module; const char *name; unsigned short ordinal; } beer_module_ordinals[] = {"]
    lines += ['    { "%s", "%s", %d },' % entry for entry in ordinals] or ['    { "", "", 0 },']
    lines += ["};", "#define BEER_MODULE_ORDINAL_COUNT (sizeof(beer_module_ordinals) / sizeof(*beer_module_ordinals))",
              "", "#endif", ""]
    with open(os.path.join(ROOT, "platform", "win32_module_exports.h"), "w") as out:
        out.write("\n".join(lines))
    print("%d exports" % len(pairs))


if __name__ == "__main__":
    main()
