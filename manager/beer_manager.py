#!/usr/bin/env python3
"""Beer manager: game library, launcher and live controls for the Beer loader.

Talks to running games over the loader's per-process unix socket
($XDG_RUNTIME_DIR/beer/<pid>.sock, one JSON object per line). Everything it saves lives in ~/.beer:
library.json, manager.json, overlay.json, display.json, logs/ (and the prefixes/ and trash/ folders).
"""
import glob
import json
import os
import re
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import time

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Adw, Gdk, Gio, GLib, Gtk  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import storage  # noqa: E402
from memory_window import MemoryWindow  # noqa: E402
from objects_window import ObjectsWindow  # noqa: E402
from overlay_page import OverlayPage, flat_message, load_config as load_overlay_config  # noqa: E402
from overlay_page import write_loader_file as write_overlay_loader_file  # noqa: E402
from display_page import DisplayPage, post_message, load_config as load_display_config  # noqa: E402
from display_page import write_loader_file as write_display_loader_file  # noqa: E402
from capture_page import CapturePage  # noqa: E402
from graphics_window import GraphicsWindow, apply_saved as apply_saved_tweaks  # noqa: E402
from frame_window import FrameWindow  # noqa: E402
from mods_page import ModRunner, ModsPage  # noqa: E402
from steam_page import DEFAULT_PROVIDER, SteamPage  # noqa: E402

APP_ID = "dev.beer.Manager"
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
storage.migrate()
LIBRARY_FILE = storage.path("library.json")
SETTINGS_FILE = storage.path("manager.json")
LOG_DIR = storage.LOG_DIR
SOCKET_DIR = os.path.join(os.environ.get("XDG_RUNTIME_DIR") or "/tmp/beer-%d" % os.getuid(), "beer") \
    if os.environ.get("XDG_RUNTIME_DIR") else "/tmp/beer-%d" % os.getuid()

AUDIO_BACKENDS = ["fmod", "compat"]
# BEER_STEAM_API_MODE choices: (setting, label, description). "auto" picks official for Steam installs and leaves
# the variable unset otherwise, so the loader runs an application-local steam_api64.dll (guest) or its facade.
STEAM_API_MODES = [
    ("auto", "Automatic", "Your Steam session for games installed from Steam; otherwise the game's own Steam DLL when it ships one, Beer's offline Steam if not"),
    ("compat", "Beer offline Steam", "Built-in offline Steam API: local user, local saves, no online services"),
    ("official", "Your Steam session", "Real identity from your signed-in account; ownership and tickets remain limited to owned games"),
    ("guest", "Game's own Steam DLL", "Runs the steam_api64.dll shipped with the game"),
]
PRESENTERS = ["Default", "auto", "vulkan", "x11"]
PRESENT_MODES = ["Default", "fifo", "mailbox", "immediate"]
# Every launch option the loader understands, as switches: (key, label, environment variable, value, description).
LAUNCH_FLAGS = [
    ("renderer_vulkan", "--renderer=vulkan", None, None, "Select the Vulkan renderer explicitly (the only one today)"),
    ("no_rt", "Disable ray tracing", "BEER_NO_RT", "1", "Never enable the Vulkan ray tracing extensions"),
    ("no_post", "Disable final-image passes", "BEER_NO_POST", "1", "No effects, overlay blending or screenshots; the overlay falls back to opaque boxes"),
    ("legacy_d3d11", "Use the legacy D3D11 path", "BEER_D3D11_LEGACY", "1", "Old D3D11 renderer instead of BD11 (the default)"),
    ("d3d12_sm51", "Limit D3D12 to shader model 5.1", "BEER_D3D12_SHADER_MODEL", "5.1", "Games that also ship DXBC shaders use them instead of SM 6.6 (DXIL): less memory, no bindless heap mirror. Games that need SM 6 refuse to start"),
    ("hide_steam_registry", "Hide the fake Steam registry keys", "BEER_FAKE_STEAM_REGISTRY", "0", "For games that behave differently when Steam looks installed"),
    ("known_patches", "Enable known guest-code patches", "BEER_ENABLE_KNOWN_TARGET_PATCHES", "1", ""),
    ("transaction_patches", "Enable transaction patches", "BEER_ENABLE_TRANSACTION_PATCHES", "1", ""),
    ("cpu_staging", "Force CPU staging in the presenter", "BEER_PRESENTER_CPU_STAGING", "1", "Slower, but a useful correctness check"),
    ("fps_log", "Print FPS to the log", "BEER_BD12_FPS", "1", "One line every 120 frames in the log"),
    ("allow_180_base", "Allow guest DLLs at 0x180000000", "BEER_ALLOW_180_BASE", "1", ""),
    ("mangohud", "Allow MangoHud", "MANGOHUD", "1", "Off by default: the MangoHud Vulkan layer hooks the swapchain and crashed RE8 (Beer has its own overlay)"),
    ("dxr_probe", "Log ray tracing requests", "BEER_DXR_PROBE", "1", "Log what the game asks of D3D12 ray tracing (diagnostic)"),
    # D3D12 (BD12)
    ("bd12_no_root_push", "Per-draw D3D12 binding (old path)", "BEER_BD12_ROOT_PUSH", "0",
     "Build descriptor sets per draw instead of the default root push + descriptor buffers. Much slower in SM 6 games (RE8: 148 -> 67 fps); for comparison when a game renders wrongly"),
    ("bd12_no_ssbo", "Texel-buffer descriptors (old path)", "BEER_BD12_SSBO", "0",
     "Raw/structured buffers as texel buffers instead of the default storage buffers (no UAV counters or 64-bit loads)"),
    ("bd12_multi_queue", "Separate Vulkan queues (experimental)", "BEER_BD12_MULTI_QUEUE", "1",
     "Compute and copy queues on their own Vulkan queues so they can overlap with graphics"),
    ("bd12_no_mesh", "Disable mesh shaders", "BEER_BD12_MESH", "0", "Hide D3D12 mesh/amplification shaders (on by default where the GPU has VK_EXT_mesh_shader)"),
    ("bd12_no_vrs", "Disable variable-rate shading", "BEER_BD12_VRS", "0", "Hide D3D12 VRS (on by default where the GPU has VK_KHR_fragment_shading_rate)"),
    ("no_dxr", "Hide DXR ray tracing", "BEER_DXR", "0", "Report no D3D12 ray tracing to the game (on by default where the GPU supports it); also hides DirectX 12 Ultimate"),
    ("bd12_release_pipelines", "Release pipelines after compiling", "BEER_BD12_RELEASE_PIPELINES", "1",
     "Always use warm-and-release (lower memory, more stutter). Normally automatic when memory gets tight"),
    ("bd12_keep_pipelines", "Never release pipelines", "BEER_BD12_RELEASE_PIPELINES", "0", "Keep every compiled pipeline resident even when memory is tight"),
    ("bd12_no_compile_at_create", "Compile pipelines on first use", "BEER_BD12_COMPILE_AT_CREATE", "0",
     "Instead of when the game creates them (less up-front work, more stutter in game)"),
    ("no_pipeline_library", "Disable D3D12 pipeline libraries", "BEER_D3D12_PIPELINE_LIBRARY", "0", "Report no pipeline library support to the game"),
    ("no_shader_disk_cache", "Disable the shader translation cache", "BEER_SHADER_DISK_CACHE", "0", "Translate every shader again on each launch"),
    ("no_pipeline_cache", "Ignore the saved pipeline cache", "BEER_NO_PIPELINE_CACHE", "1", "Start without the Vulkan pipeline cache file (it is still written)"),
    # Window, media and I/O
    ("hide_fullscreen_cursor", "Hide the cursor in fullscreen", "BEER_HIDE_FULLSCREEN_CURSOR", "1", "Always hide the host cursor while the game is fullscreen"),
    ("single_window", "Single native window", "BEER_SINGLE_WINDOW", "1", "Give child windows no surface of their own (older behaviour)"),
    ("dstorage_cpu", "DirectStorage: decompress on the CPU", "BEER_DSTORAGE_CPU", "1", "Sets DirectStorage's own override that disables GPU decompression"),
    ("mf_composite", "Videos: direct blit", "BEER_MF_COMPOSITE", "1", "Old direct blit of decoded video frames"),
]

# Launch options that take a value: (key, label, environment variable, description). Empty = not set.
LAUNCH_VALUES = [
    ("dll_builtin", "Use Beer's own DLLs (BEER_DLL_BUILTIN)", "BEER_DLL_BUILTIN",
     "Comma-separated DLL names Beer serves itself, ignoring copies in the game folder (e.g. winmm)"),
    ("dll_path", "Extra runtime DLL folders (BEER_DLL_PATH)", "BEER_DLL_PATH", "Colon-separated folders searched for redistributable DLLs"),
    ("pipeline_budget_mb", "Pipeline memory budget in MB", "BEER_BD12_PIPELINE_BUDGET_MB",
     "Resident size above which pipelines are released after compiling (default: 60% of RAM or the memory limit)"),
    ("dxgi_vram_mb", "Report at most this much VRAM (MB)", "BEER_DXGI_VRAM_MB", "Caps the dedicated video memory DXGI reports"),
    ("dxgi_shared_mb", "Report at most this much shared GPU memory (MB)", "BEER_DXGI_SHARED_MB", "Caps the shared system memory DXGI reports"),
    ("phys_mb", "Report at most this much RAM (MB)", "BEER_PHYS_MB", "Caps the physical memory the game is told about"),
    ("audio_latency_ms", "Audio latency margin (ms)", "BEER_AUDIO_LATENCY_MS", "Default 40"),
    ("nvapi_driver", "NVIDIA driver version to report", "BEER_SPOOF_NVAPI_DRIVER", "NVAPI driver version number (e.g. 56603 for 566.03)"),
    ("ngx_path", "DLSS (NGX) runtime folder", "BEER_NGX_PATH", "Folder with nvngx.dll / _nvngx.dll when it is not found automatically"),
    ("steam_id", "Steam ID for the offline Steam API", "BEER_STEAM_ID", "SteamID64 or account number reported by Beer's offline Steam"),
    ("steam_networking_library", "Steam local networking library", "BEER_STEAM_NETWORKING_LIBRARY",
     "Optional libGameNetworkingSockets.so for local listeners (including solo listen servers). "
     "Does not provide remote Steam signaling or relay authentication. Empty = not enabled by the manager."),
]

# DLSS overrides (platform/win32_ngx.inc, live settings dlss_* in platform/beer_control.c).
DLSS_PRESET_CHOICES = [
    ("Game default", None), ("DLSS default for its version", "0"), ("Preset J", "J"), ("Preset K", "K"),
    ("Preset L", "L"), ("Preset M", "M"), ("Preset F", "F"), ("Preset E", "E"),
    ("No upscaling (raw internal image, instant)", "none"),
]
DLSS_MARKS = [(33.3, "UP"), (50, "P"), (58, "B"), (66.7, "Q"), (77, "UQ"), (100, "DLAA")]


def dlss_settings(game):
    """A game's DLSS override as {enabled, ratio, render, preset}; older saved formats are converted."""
    dlss = dict(game.get("dlss") or {})
    if "res" in dlss:                                  # 2026-11-14 combo-box format
        res, custom = dlss.pop("res"), str(dlss.pop("custom", "")).strip().lower().replace(" ", "").rstrip("%")
        if res == "custom":
            dlss["enabled"] = bool(custom)
            if "x" in custom:
                dlss["render"] = custom
            elif custom:
                dlss["ratio"] = float(custom)
        elif res:
            dlss["enabled"], dlss["ratio"] = True, float(res)
    return {"enabled": bool(dlss.get("enabled")), "ratio": float(dlss.get("ratio") or 66.7),
            "render": str(dlss.get("render") or ""), "preset": dlss.get("preset")}


def parse_render_size(text):
    m = re.fullmatch(r"\s*(\d{2,5})\s*[xX×*]\s*(\d{2,5})\s*", text or "")
    return (int(m.group(1)), int(m.group(2))) if m else None


def dlss_environment(game):
    """BEER_DLSS_* variables for a game's saved DLSS override."""
    d, env = dlss_settings(game), {}
    if d["enabled"]:
        size = parse_render_size(d["render"])
        if size:
            env["BEER_DLSS_RENDER"] = "%dx%d" % size
        else:
            env["BEER_DLSS_RATIO"] = "%g" % d["ratio"]
    if d["preset"]:
        env["BEER_DLSS_PRESET"] = d["preset"]
    return env


DLSS_PRESET_HELP = (
    "<b>Which DLSS model to use</b>\n\n"
    "<b>Game default</b> — whatever the game asks for.\n"
    "<b>DLSS default for its version</b> — the model the game's DLSS DLL picks for the mode.\n\n"
    "<b>Transformer models</b> (DLSS 4 and later; sharper, more stable, less ghosting; a little slower)\n"
    "• <b>K</b> — the all-round best for Quality, Balanced and DLAA. Safe first choice.\n"
    "• <b>J</b> — the first transformer model; slightly sharper than K but shimmers and ghosts more.\n"
    "• <b>M</b> — DLSS 4.5 (DLL 310.5+); best at low internal resolutions (Performance, ~50%).\n"
    "• <b>L</b> — DLSS 4.5 (DLL 310.5+); tuned for Ultra Performance (~33%) and 4K output.\n\n"
    "<b>Older CNN models</b> (fastest, softer)\n"
    "• <b>F</b> — for Ultra Performance and DLAA in DLSS 3.\n"
    "• <b>E</b> — the last DLSS 3 default; use it if a transformer model costs too much on a slower GPU.\n\n"
    "<b>No upscaling</b> — shows the game's raw internal-resolution image stretched to the screen, for comparison. "
    "Switches instantly.\n\n"
    "A model the game's DLSS version does not have falls back to its default. The DLSS DLL version is shown in "
    "its file properties (nvngx_dlss.dll in the game folder).")


def help_button(markup):
    """A small "?" button that opens a popover with the given Pango markup."""
    label = Gtk.Label(use_markup=True, wrap=True, max_width_chars=60, xalign=0, margin_top=8, margin_bottom=8,
                      margin_start=8, margin_end=8)
    label.set_markup(markup)
    popover = Gtk.Popover(child=label)
    button = Gtk.MenuButton(icon_name="help-about-symbolic", popover=popover, valign=Gtk.Align.CENTER,
                            tooltip_text="What do these mean?")
    button.add_css_class("flat")
    return button


class DlssControls:
    """The DLSS rows (override switch, internal-resolution slider with mode marks, custom size, preset), used in the
    game settings and, live, on the running-game card. on_change(settings) runs after every user change."""

    def __init__(self, saved, on_change=None):
        self.on_change = on_change
        self._quiet = True
        self.enabled = Adw.SwitchRow(title="Override internal resolution",
                                     subtitle="The game renders at this size whatever DLSS mode it is set to")
        self.enabled.set_active(saved["enabled"])
        self.scale = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 10, 100, 0.5)
        self.scale.set_hexpand(True)
        self.scale.set_size_request(260, -1)
        self.scale.set_draw_value(False)
        self.scale.set_value(saved["ratio"])
        for value, label in DLSS_MARKS:
            self.scale.add_mark(value, Gtk.PositionType.BOTTOM, label)
        self.value_label = Gtk.Label(width_chars=6, xalign=1)
        self.value_label.add_css_class("numeric")
        self.slider_row = Adw.ActionRow(title="Internal resolution",
                                        subtitle="Below 33% is outside DLSS's official modes; most games cope")
        box = Gtk.Box(spacing=8, valign=Gtk.Align.CENTER, hexpand=True)
        box.append(self.scale)
        box.append(self.value_label)
        self.slider_row.add_suffix(box)
        self.custom = Adw.EntryRow(title="Custom render size, e.g. 1600x900 (overrides the slider)")
        self.custom.set_text(saved["render"])
        self.preset = Adw.ComboRow(title="DLSS model (render preset)",
                                   subtitle="J, K and later are the transformer models; a DLSS version without the "
                                            "preset uses its default",
                                   model=Gtk.StringList.new([label for label, _v in DLSS_PRESET_CHOICES]))
        values = [v for _l, v in DLSS_PRESET_CHOICES]
        self.preset.set_selected(values.index(saved["preset"]) if saved["preset"] in values else 0)
        self.preset.add_suffix(help_button(DLSS_PRESET_HELP))
        self.rows = [self.enabled, self.slider_row, self.custom, self.preset]
        self.scale.connect("value-changed", lambda _s: self._changed())
        self.enabled.connect("notify::active", lambda *_a: self._changed())
        self.custom.connect("changed", lambda _e: self._changed())
        self.preset.connect("notify::selected", lambda *_a: self._changed())
        self._quiet = False
        self._update_widgets()

    def _update_widgets(self):
        on = self.enabled.get_active()
        size = parse_render_size(self.custom.get_text())
        self.slider_row.set_sensitive(on and not size)
        self.custom.set_sensitive(on)
        self.value_label.set_label("%g%%" % round(self.scale.get_value(), 1))
        text = self.custom.get_text().strip()
        if text and not size:
            self.custom.add_css_class("error")
        else:
            self.custom.remove_css_class("error")

    def _changed(self):
        self._update_widgets()
        if not self._quiet and self.on_change:
            self.on_change(self.settings())

    def settings(self):
        return {"enabled": self.enabled.get_active(), "ratio": round(self.scale.get_value(), 1),
                "render": self.custom.get_text().strip(),
                "preset": DLSS_PRESET_CHOICES[self.preset.get_selected()][1]}


# Loader values that hold paths get a browse button: key -> (pick a folder, list separator or None).
PATH_VALUES = {"dll_path": (True, ":"), "ngx_path": (True, None), "steam_networking_library": (False, None)}

BEER_HOME = os.path.expanduser("~/.beer")
PREFIX_DIR = os.path.join(BEER_HOME, "prefixes")
TRASH_DIR = os.path.join(BEER_HOME, "trash")
# Default folder for games the Steam session provider downloads; more can be added in Loader settings.
STEAM_LIBRARY_DIR = os.path.join(BEER_HOME, "steam-library")
LEGACY_PREFIX_DIR = os.path.join(os.environ.get("XDG_DATA_HOME", os.path.expanduser("~/.local/share")), "beer", "prefixes")
PREFIX_SKELETON = [
    "drive_c/Windows/System32", "drive_c/Windows/SysWOW64", "drive_c/Windows/Temp", "drive_c/Program Files",
    "drive_c/ProgramData", "drive_c/Users/Player/AppData/Local", "drive_c/Users/Player/AppData/Roaming",
    "drive_c/Users/Player/Documents", "drive_c/Users/Player/Saved Games", "dosdevices",
]

# What the guest is told through Windows APIs. The real GPU still renders and CPUID stays native.
GPU_PRESETS = {
    "NVIDIA GeForce RTX 4090": ("10de", "2684", 24576),
    "NVIDIA GeForce RTX 4080": ("10de", "2704", 16384),
    "NVIDIA GeForce RTX 3080": ("10de", "2206", 10240),
    "NVIDIA GeForce RTX 2080 Ti": ("10de", "1e04", 11264),
    "NVIDIA GeForce GTX 1080 Ti": ("10de", "1b06", 11264),
    "AMD Radeon RX 7900 XTX": ("1002", "744c", 24576),
    "AMD Radeon RX 6800 XT": ("1002", "73bf", 16384),
    "Intel Arc A770": ("8086", "56a0", 16384),
}
CPU_PRESETS = {
    "Intel Core i9-13900K": ("GenuineIntel", 32, 3000),
    "Intel Core i7-12700K": ("GenuineIntel", 20, 3600),
    "Intel Core i5-10400": ("GenuineIntel", 12, 2900),
    "AMD Ryzen 9 7950X 16-Core Processor": ("AuthenticAMD", 32, 4500),
    "AMD Ryzen 7 5800X3D 8-Core Processor": ("AuthenticAMD", 16, 3400),
    "AMD Ryzen 5 5600X 6-Core Processor": ("AuthenticAMD", 12, 3700),
}


# ---------------------------------------------------------------- storage

def load_json(path, default):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


def save_json(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(data, f, indent=2)
    os.replace(tmp, path)


def find_vkd3d_library():
    patterns = [
        os.path.expanduser("~/.local/share/umu/*/*/files/lib/x86_64-linux-gnu/libvkd3d-shader.so.*"),
        os.path.expanduser("~/.local/share/umu/*/files/lib/x86_64-linux-gnu/libvkd3d-shader.so.*"),
        "/usr/lib/x86_64-linux-gnu/libvkd3d-shader.so.1",
    ]
    for pattern in patterns:
        hits = sorted(glob.glob(pattern))
        hits = [h for h in hits if not h.endswith(".so")]
        if hits:
            return hits[-1]
    return ""


# ---------------------------------------------------------------- prefixes

PREFIX_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,47}$")


def list_prefixes():
    """[{name, path, legacy}] from ~/.beer/prefixes plus not-yet-migrated ones from the old location."""
    found = {}
    for directory, legacy in ((LEGACY_PREFIX_DIR, True), (PREFIX_DIR, False)):
        try:
            names = sorted(os.listdir(directory))
        except OSError:
            continue
        for name in names:
            path = os.path.join(directory, name)
            if os.path.isdir(path):
                found[name] = {"name": name, "path": path, "legacy": legacy}
    result = sorted(found.values(), key=lambda p: (p["name"] != "default", p["name"].lower()))
    return result


def prefix_path(name):
    """Where a prefix lives: the new location, else the old one if that is where it still is."""
    name = name or "default"
    new = os.path.join(PREFIX_DIR, name)
    old = os.path.join(LEGACY_PREFIX_DIR, name)
    if not os.path.isdir(new) and os.path.isdir(old):
        return old
    return new


def create_prefix(name):
    if not PREFIX_NAME.match(name):
        raise ValueError("Use letters, digits, '.', '_' or '-' (max 48 characters)")
    path = os.path.join(PREFIX_DIR, name)
    if os.path.exists(path) or os.path.exists(os.path.join(LEGACY_PREFIX_DIR, name)):
        raise ValueError("A prefix named '%s' already exists" % name)
    for sub in PREFIX_SKELETON:
        os.makedirs(os.path.join(path, sub), exist_ok=True)
    os.symlink("../drive_c", os.path.join(path, "dosdevices", "c:"))
    return path


def clone_prefix(source, name):
    if not PREFIX_NAME.match(name):
        raise ValueError("Use letters, digits, '.', '_' or '-' (max 48 characters)")
    path = os.path.join(PREFIX_DIR, name)
    if os.path.exists(path):
        raise ValueError("A prefix named '%s' already exists" % name)
    os.makedirs(PREFIX_DIR, exist_ok=True)
    shutil.copytree(source, path, symlinks=True)
    return path


def steam_prefix(appid):
    """Official Steam installs get their own prefix, so their saves never mix with an imported copy's."""
    name = "steam-%d" % int(appid)
    if not os.path.isdir(prefix_path(name)):
        create_prefix(name)
    return name


def trash_prefix(path):
    """Never deletes: moves the prefix into ~/.beer/trash so saves can always be recovered."""
    os.makedirs(TRASH_DIR, exist_ok=True)
    target = os.path.join(TRASH_DIR, "%s-%s" % (os.path.basename(path), time.strftime("%Y%m%d-%H%M%S")))
    shutil.move(path, target)
    return target


def migrate_prefix(path):
    name = os.path.basename(path)
    target = os.path.join(PREFIX_DIR, name)
    if os.path.exists(target):
        raise ValueError("~/.beer/prefixes/%s already exists" % name)
    os.makedirs(PREFIX_DIR, exist_ok=True)
    shutil.move(path, target)
    return target


def add_path_picker(row, title, folder=False, patterns=None, filter_name=None, append_sep=None, on_pick=None):
    """Adds a browse button to an entry row: the same file dialog as the game .exe picker. folder=True picks a folder.
    append_sep appends the pick to a separator-joined list instead of replacing the text. on_pick(path) replaces the
    default action."""
    button = Gtk.Button(icon_name="folder-open-symbolic" if folder else "document-open-symbolic",
                        valign=Gtk.Align.CENTER, tooltip_text=title)
    button.add_css_class("flat")

    def picked(path):
        if on_pick:
            on_pick(path)
        elif append_sep and row.get_text().strip():
            parts = [p for p in row.get_text().split(append_sep) if p]
            if path not in parts:
                row.set_text(append_sep.join(parts + [path]))
        else:
            row.set_text(path)

    def done(dialog, result):
        try:
            f = dialog.select_folder_finish(result) if folder else dialog.open_finish(result)
        except GLib.Error:
            return
        if f and f.get_path():
            picked(f.get_path())

    def clicked(_b):
        chooser = Gtk.FileDialog(title=title)
        current = row.get_text().strip()
        if append_sep and current:
            current = current.split(append_sep)[-1]
        start = current if folder else os.path.dirname(current)
        if start and os.path.isdir(start):
            chooser.set_initial_folder(Gio.File.new_for_path(start))
        if patterns:
            file_filter = Gtk.FileFilter()
            file_filter.set_name(filter_name or title)
            for pattern in patterns:
                file_filter.add_pattern(pattern)
            filters = Gio.ListStore.new(Gtk.FileFilter)
            filters.append(file_filter)
            chooser.set_filters(filters)
        root = row.get_root()
        parent = root if isinstance(root, Gtk.Window) else None
        if folder:
            chooser.select_folder(parent, None, done)
        else:
            chooser.open(parent, None, done)
    button.connect("clicked", clicked)
    row.add_suffix(button)
    return button


class Library:
    def __init__(self):
        self.games = load_json(LIBRARY_FILE, {"games": []})["games"]
        self.settings = load_json(SETTINGS_FILE, {})
        self.settings.setdefault("loader", os.path.join(REPO, "loader"))
        self.settings.setdefault("vkd3d_library", find_vkd3d_library())
        self.settings.setdefault("steam_libraries", [STEAM_LIBRARY_DIR])
        self.settings.setdefault("shader_cache_dir", storage.CACHE_DIR)

    def save(self):
        save_json(LIBRARY_FILE, {"games": self.games})
        save_json(SETTINGS_FILE, self.settings)


# ---------------------------------------------------------------- control channel

class Control:
    """One persistent connection to a running loader."""

    def __init__(self, path):
        self.path = path
        self.sock = None
        self.buffer = b""
        self.info = None

    def _connect(self):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(0.6)
        s.connect(self.path)
        self.sock = s
        self.buffer = b""

    def request(self, message):
        for attempt in (0, 1):
            try:
                if self.sock is None:
                    self._connect()
                self.sock.sendall((json.dumps(message) + "\n").encode())
                while b"\n" not in self.buffer:
                    chunk = self.sock.recv(65536)
                    if not chunk:
                        raise OSError("closed")
                    self.buffer += chunk
                line, self.buffer = self.buffer.split(b"\n", 1)
                return json.loads(line)
            except (OSError, ValueError):
                self.close()
                if attempt:
                    return None
        return None

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
        self.sock = None


def spoof_environment(game):
    env = {}
    gpu = game.get("gpu", {})
    choice = gpu.get("preset", "Real hardware")
    if choice in GPU_PRESETS:
        vendor, device, vram = GPU_PRESETS[choice]
        env.update(BEER_SPOOF_GPU_NAME=choice, BEER_SPOOF_GPU_VENDOR=vendor, BEER_SPOOF_GPU_DEVICE=device,
                   BEER_SPOOF_GPU_VRAM_MB=str(vram))
    elif choice == "Custom…":
        if gpu.get("name"):
            env["BEER_SPOOF_GPU_NAME"] = gpu["name"]
        if gpu.get("vendor"):
            env["BEER_SPOOF_GPU_VENDOR"] = gpu["vendor"]
        if gpu.get("device"):
            env["BEER_SPOOF_GPU_DEVICE"] = gpu["device"]
        env["BEER_SPOOF_GPU_VRAM_MB"] = str(gpu.get("vram", 8192))
    cpu = game.get("cpu", {})
    choice = cpu.get("preset", "Real hardware")
    if choice in CPU_PRESETS:
        vendor, threads, mhz = CPU_PRESETS[choice]
        env.update(BEER_SPOOF_CPU_NAME=choice, BEER_SPOOF_CPU_VENDOR=vendor, BEER_SPOOF_CPU_THREADS=str(threads),
                   BEER_SPOOF_CPU_MHZ=str(mhz))
    elif choice == "Custom…":
        if cpu.get("name"):
            env["BEER_SPOOF_CPU_NAME"] = cpu["name"]
        env["BEER_SPOOF_CPU_VENDOR"] = cpu.get("vendor", "GenuineIntel")
        env["BEER_SPOOF_CPU_THREADS"] = str(cpu.get("threads", 16))
        env["BEER_SPOOF_CPU_MHZ"] = str(cpu.get("mhz", 3600))
    return env


# ---------------------------------------------------------------- per-game sound (PipeWire / PulseAudio)

def audio_streams(pid):
    """Playback streams the process opened, as pactl reports them ([] when none or pactl is missing)."""
    try:
        out = subprocess.run(["pactl", "-f", "json", "list", "sink-inputs"], capture_output=True, text=True,
                             timeout=2).stdout
        streams = json.loads(out or "[]")
    except (OSError, ValueError, subprocess.SubprocessError):
        return []
    return [s for s in streams if str(s.get("properties", {}).get("application.process.id")) == str(pid)]


def stream_volume(stream):
    values = [c.get("value_percent", "100%") for c in (stream.get("volume") or {}).values()]
    try:
        return int(values[0].rstrip("%")) if values else 100
    except ValueError:
        return 100


def set_streams(streams, volume=None, mute=None):
    for stream in streams:
        index = str(stream["index"])
        if volume is not None:
            subprocess.run(["pactl", "set-sink-input-volume", index, "%d%%" % volume], timeout=2,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if mute is not None:
            subprocess.run(["pactl", "set-sink-input-mute", index, "1" if mute else "0"], timeout=2,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def pid_alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def parent_pid(pid):
    try:
        with open("/proc/%d/stat" % pid) as f:
            return int(f.read().rsplit(")", 1)[1].split()[1])
    except (OSError, ValueError, IndexError):
        return 0


# ---------------------------------------------------------------- running game card

SCALE_MIN, SCALE_MAX = 0.05, 10.0


def scale_to_pos(scale):
    import math
    return math.log(max(scale, SCALE_MIN) / SCALE_MIN) / math.log(SCALE_MAX / SCALE_MIN)


def pos_to_scale(pos):
    return SCALE_MIN * (SCALE_MAX / SCALE_MIN) ** pos


def fmt_scale(scale):
    return ("%.2f" % scale).rstrip("0").rstrip(".") + "×"


class RunningCard(Adw.PreferencesGroup):
    def __init__(self, app, pid, path, name, log_path, exe=""):
        super().__init__()
        self.app, self.pid, self.name, self.log_path, self.exe = app, pid, name, log_path, exe
        self.control = Control(path)
        self.path = path
        self.set_title(GLib.markup_escape_text(name))
        self.set_description("PID %d" % pid)
        self._pending = None
        self._updating = False

        self.stats = Adw.ActionRow(title="Status", subtitle="connecting…")
        self.stats.add_css_class("property")
        self.add(self.stats)
        # Low-memory guard (platform/beer_control.c): shown while the loader holds the game paused.
        self.memguard_row = Adw.ActionRow(title="Low memory — game paused", visible=False)
        self.memguard_row.add_css_class("error")
        resume = Gtk.Button(label="Continue", valign=Gtk.Align.CENTER, tooltip_text="Resume the game now")
        resume.add_css_class("suggested-action")
        resume.connect("clicked", lambda _b: self.control.request({"cmd": "memguard_resume"}))
        self.memguard_row.add_suffix(resume)
        self.add(self.memguard_row)
        self.memguard_notified = False

        speed = Adw.ActionRow(title="Game speed")
        self.speed_label = Gtk.Label(label="1×", width_chars=6, xalign=1)
        self.speed_label.add_css_class("numeric")
        self.scale = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 0, 1, 0.001)
        self.scale.set_hexpand(True)
        self.scale.set_size_request(260, -1)
        self.scale.set_draw_value(False)
        self.scale.set_value(scale_to_pos(1.0))
        self.scale.connect("value-changed", self._on_slider)
        for value in (0.25, 0.5, 1.0, 2.0, 4.0):
            self.scale.add_mark(scale_to_pos(value), Gtk.PositionType.BOTTOM, fmt_scale(value))
        speed.add_suffix(self.scale)
        speed.add_suffix(self.speed_label)
        self.add(speed)

        quick = Adw.ActionRow(title="Quick speeds")
        box = Gtk.Box(spacing=6, valign=Gtk.Align.CENTER)
        for value in (0.25, 0.5, 1.0, 2.0, 4.0):
            b = Gtk.Button(label=fmt_scale(value))
            b.add_css_class("flat")
            b.connect("clicked", lambda _b, v=value: self.set_speed(v))
            box.append(b)
        quick.add_suffix(box)
        self.add(quick)

        cap = Adw.ActionRow(title="Frame cap", subtitle="Limits presented frames per second (D3D12 games); Off = unlimited")
        cap_box = Gtk.Box(spacing=6, valign=Gtk.Align.CENTER)
        self.cap_buttons = {}
        for value in (0, 30, 60, 90, 120, 144):
            b = Gtk.ToggleButton(label="Off" if value == 0 else str(value))
            b.add_css_class("flat")
            b.connect("clicked", lambda _b, v=value: self.set_cap(v))
            cap_box.append(b)
            self.cap_buttons[value] = b
        self.cap_spin = Gtk.SpinButton.new_with_range(0, 1000, 1)
        self.cap_spin.set_valign(Gtk.Align.CENTER)
        self.cap_spin.connect("value-changed", lambda s: self.set_cap(int(s.get_value())))
        cap_box.append(self.cap_spin)
        cap.add_suffix(cap_box)
        self.add(cap)

        # Sound: the game's own playback streams in the desktop mixer.
        self.sound = Adw.ActionRow(title="Volume", subtitle="Waiting for the game to play sound")
        self.volume = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 0, 150, 1)
        self.volume.set_hexpand(True)
        self.volume.set_size_request(260, -1)
        self.volume.set_draw_value(False)
        self.volume.add_mark(100, Gtk.PositionType.BOTTOM, None)
        self.volume.connect("value-changed", self._on_volume)
        self.volume_label = Gtk.Label(label="100%", width_chars=5, xalign=1)
        self.volume_label.add_css_class("numeric")
        self.mute = Gtk.ToggleButton(icon_name="audio-volume-high-symbolic", valign=Gtk.Align.CENTER, tooltip_text="Mute")
        self.mute.add_css_class("flat")
        self.mute.connect("toggled", self._on_mute)
        self.sound.add_suffix(self.volume)
        self.sound.add_suffix(self.volume_label)
        self.sound.add_suffix(self.mute)
        self.add(self.sound)
        game = next((g for g in app.lib.games if g.get("exe") == exe), {})
        # Only a volume the user chose for this game is reapplied; otherwise the mixer's own level stays.
        self.saved_volume = int(game["volume"]) if "volume" in game else None
        self.known_streams = set()
        self.sound_tick = 0
        self._setting_sound = False
        self._set_sound_widgets(self.saved_volume if self.saved_volume is not None else 100, False)
        self.volume.set_sensitive(False)
        self.mute.set_sensitive(False)

        self.pause = Adw.SwitchRow(title="Pause game time", subtitle="Freezes the clocks the game sees; rendering keeps running")
        self.pause.connect("notify::active", self._on_pause)
        self.add(self.pause)

        # DLSS, live: the game picks the new size up the next time it asks DLSS (changing its DLSS mode or resolution).
        self.dlss_pending = None
        dlss_row = Adw.ExpanderRow(title="DLSS",
                                   subtitle="Live. Applies when the game next sets up DLSS: switch its DLSS quality "
                                            "mode (or resolution) once in the game's menu")
        self.dlss = DlssControls(dlss_settings(game), self._on_dlss)
        for row in self.dlss.rows:
            dlss_row.add_row(row)
        self.add(dlss_row)

        actions = Adw.ActionRow(title="Session")
        log_button = Gtk.Button(label="Log", valign=Gtk.Align.CENTER)
        log_button.connect("clicked", lambda _b: app.show_log(self.name, self.log_path))
        log_button.set_sensitive(bool(log_path))
        graphics = Gtk.Button(label="Graphics…", valign=Gtk.Align.CENTER, tooltip_text="Find and tweak shader constants such as light intensity")
        graphics.connect("clicked", lambda _b: GraphicsWindow(app, self.path, self.name, self.pid, self.exe).present())
        actions.add_suffix(graphics)
        frame = Gtk.Button(label="Frame…", valign=Gtk.Align.CENTER,
                           tooltip_text="Capture one frame and watch how the game draws it, pass by pass")
        frame.connect("clicked", lambda _b: FrameWindow(app, self.path, self.name, self.pid, self.exe).present())
        actions.add_suffix(frame)
        memory = Gtk.Button(label="Memory…", valign=Gtk.Align.CENTER)
        memory.connect("clicked", lambda _b: MemoryWindow(app, self.path, self.name, self.pid).present())
        actions.add_suffix(memory)
        objects = Gtk.Button(label="Objects…", valign=Gtk.Align.CENTER,
                             tooltip_text="Find the player and the objects around it; edit position, rotation, scale and fields")
        objects.connect("clicked", lambda _b: ObjectsWindow(app, self.path, self.name, self.pid, self.exe).present())
        actions.add_suffix(objects)
        stop = Gtk.Button(label="Stop", valign=Gtk.Align.CENTER)
        stop.add_css_class("destructive-action")
        stop.connect("clicked", lambda _b: app.stop_game(self.pid))
        actions.add_suffix(log_button)
        actions.add_suffix(stop)
        self.add(actions)

    def remember(self, key, value):
        """Live changes become the game's starting values, so they survive a relaunch."""
        self.app.remember(self.exe, key, value)

    def set_speed(self, value):
        self.scale.set_value(scale_to_pos(value))

    def _on_slider(self, scale):
        value = pos_to_scale(scale.get_value())
        # Snap near the common stops so 1x is exactly 1x.
        for stop in (0.25, 0.5, 1.0, 2.0, 4.0):
            if abs(value - stop) / stop < 0.03:
                value = stop
        self.speed_label.set_label(fmt_scale(value))
        if self._updating:
            return
        if self._pending:
            GLib.source_remove(self._pending)
        self._pending = GLib.timeout_add(80, self._send_speed, value)

    def _send_speed(self, value):
        self._pending = None
        self.control.request({"cmd": "set", "name": "time_scale", "value": round(value, 3)})
        self.remember("time_scale", round(value, 3))
        return False

    def set_cap(self, value):
        if self._updating:
            return
        self._updating = True
        try:
            for v, b in self.cap_buttons.items():
                b.set_active(v == value)
            self.cap_spin.set_value(value)
        finally:
            self._updating = False
        self.control.request({"cmd": "set", "name": "fps_limit", "value": value})
        self.remember("fps_limit", int(value))

    def _on_dlss(self, settings):
        if self.dlss_pending:
            GLib.source_remove(self.dlss_pending)
        self.dlss_pending = GLib.timeout_add(150, self._send_dlss, settings)

    def _send_dlss(self, settings):
        self.dlss_pending = None
        size = parse_render_size(settings["render"]) if settings["enabled"] else None
        ratio = settings["ratio"] if settings["enabled"] and not size else 0
        preset = settings["preset"]
        bypass = preset == "none"
        preset_value = -1 if not preset or bypass else (int(preset) if preset.isdigit() else ord(preset.upper()) - 64)
        for name, value in (("dlss_ratio", ratio), ("dlss_width", size[0] if size else 0),
                            ("dlss_height", size[1] if size else 0), ("dlss_preset", preset_value),
                            ("dlss_bypass", bypass)):
            self.control.request({"cmd": "set", "name": name, "value": value})
        self.remember("dlss", settings)
        return False

    def _on_pause(self, row, _param):
        if not self._updating:
            self.control.request({"cmd": "set", "name": "paused", "value": bool(row.get_active())})

    def _set_sound_widgets(self, volume, muted):
        self._setting_sound = True
        try:
            self.volume.set_value(volume)
            self.volume_label.set_label("%d%%" % volume)
            self.mute.set_active(muted)
            self.mute.set_icon_name("audio-volume-muted-symbolic" if muted else "audio-volume-high-symbolic")
            self.mute.set_tooltip_text("Unmute" if muted else "Mute")
        finally:
            self._setting_sound = False

    def _on_volume(self, scale):
        volume = int(scale.get_value())
        self.volume_label.set_label("%d%%" % volume)
        if self._setting_sound:
            return
        self.saved_volume = volume
        set_streams(audio_streams(self.pid), volume=volume)
        self.app.remember(self.exe, "volume", volume)

    def _on_mute(self, button):
        muted = button.get_active()
        button.set_icon_name("audio-volume-muted-symbolic" if muted else "audio-volume-high-symbolic")
        button.set_tooltip_text("Unmute" if muted else "Mute")
        if not self._setting_sound:
            set_streams(audio_streams(self.pid), mute=muted)

    def _refresh_sound(self):
        streams = audio_streams(self.pid)
        new = [st for st in streams if st["index"] not in self.known_streams]
        if new:
            # Streams opened later (FMOD opens several) get the volume and mute chosen for this game.
            mute = self.mute.get_active() if self.known_streams else None
            if self.saved_volume is not None or mute is not None:
                set_streams(new, volume=self.saved_volume, mute=mute)
                streams = audio_streams(self.pid)
            self.known_streams.update(st["index"] for st in new)
        has_sound = bool(streams)
        self.volume.set_sensitive(has_sound)
        self.mute.set_sensitive(has_sound)
        if not has_sound:
            self.sound.set_subtitle("Waiting for the game to play sound")
            return
        self.sound.set_subtitle("%d stream%s" % (len(streams), "" if len(streams) == 1 else "s"))
        if not self.volume.has_focus():
            self._set_sound_widgets(stream_volume(streams[0]), bool(streams[0].get("mute")))

    def refresh(self):
        """Returns False when the game is gone."""
        if not pid_alive(self.pid):
            return False
        self.sound_tick += 1
        if self.sound_tick % 3 == 1:
            self._refresh_sound()
        status = self.control.request({"cmd": "status"})
        if not status or not status.get("ok"):
            self.stats.set_subtitle("not responding")
            return True
        guard = status.get("memguard") or {}
        if guard.get("active"):
            text = "%s RAM free: %d of %d MB" % (guard.get("reason", ""), guard.get("avail_mb", 0), guard.get("total_mb", 0))
            if guard.get("cg_limit_mb"):
                text += " · game %d of its %d MB cap" % (guard.get("cg_used_mb", 0), guard["cg_limit_mb"])
            self.memguard_row.set_subtitle(text + ". Close other apps, then Continue.")
            self.memguard_row.set_visible(True)
            if not self.memguard_notified:
                self.memguard_notified = True
                note = Gio.Notification.new("%s paused: low memory" % self.name)
                note.set_body(text)
                note.set_priority(Gio.NotificationPriority.URGENT)
                self.app.send_notification("memguard-%d" % self.pid, note)
        elif self.memguard_row.get_visible():
            self.memguard_row.set_visible(False)
            self.memguard_notified = False
            self.app.withdraw_notification("memguard-%d" % self.pid)
        up = status["uptime_s"]
        self.stats.set_subtitle("%.0f fps · CPU %.1f%% (%.1f cores) · %d MB · %d threads · up %d:%02d" % (
            status["fps"], status["cpu_percent"], status.get("cpu_cores", 0), status["rss_mb"], status["threads"], up // 60, up % 60))
        self._updating = True
        try:
            if self._pending is None:
                self.scale.set_value(scale_to_pos(status["time_scale"]))
                self.speed_label.set_label(fmt_scale(status["time_scale"]))
            self.pause.set_active(bool(status["paused"]))
            cap = status.get("fps_limit", 0)
            for v, b in self.cap_buttons.items():
                b.set_active(v == cap)
            if not self.cap_spin.has_focus():
                self.cap_spin.set_value(cap)
        finally:
            self._updating = False
        return True


# ---------------------------------------------------------------- dialogs

class GameDialog(Adw.Dialog):
    def __init__(self, app, game, on_save):
        super().__init__()
        self.set_title("Edit game" if game.get("name") else "Add game")
        self.set_content_width(520)
        self.game = dict(game)
        self.on_save = on_save
        view = Adw.ToolbarView()
        header = Adw.HeaderBar()
        header.set_show_end_title_buttons(False)
        cancel = Gtk.Button(label="Cancel")
        cancel.connect("clicked", lambda _b: self.close())
        save = Gtk.Button(label="Save")
        save.add_css_class("suggested-action")
        save.connect("clicked", self._save)
        header.pack_start(cancel)
        header.pack_end(save)
        view.add_top_bar(header)

        page = Adw.PreferencesPage()
        group = Adw.PreferencesGroup()
        self.name = Adw.EntryRow(title="Name")
        self.name.set_text(game.get("name", ""))
        self.exe = Adw.EntryRow(title="Executable (.exe)")
        self.exe.set_text(game.get("exe", ""))
        add_path_picker(self.exe, "Choose the game's .exe", patterns=["*.exe", "*.EXE"],
                        filter_name="Windows executables", on_pick=self._picked)
        self.audio = Adw.ComboRow(title="Audio backend")
        self.audio.set_model(Gtk.StringList.new(AUDIO_BACKENDS))
        self.audio.set_selected(AUDIO_BACKENDS.index(game.get("audio", "fmod")) if game.get("audio", "fmod") in AUDIO_BACKENDS else 0)
        self.cap = Adw.SpinRow.new_with_range(0, 1000, 1)
        self.cap.set_title("Starting frame cap")
        self.cap.set_subtitle("0 = unlimited")
        self.cap.set_value(float(game.get("fps_limit", 0)))
        self.speed = Adw.SpinRow.new_with_range(0.05, 10.0, 0.05)
        self.speed.set_title("Starting game speed")
        self.speed.set_digits(2)
        self.speed.set_value(float(game.get("time_scale", 1.0)))
        self.prefix_names = [p["name"] for p in list_prefixes()] or ["default"]
        if "default" not in self.prefix_names:
            self.prefix_names.insert(0, "default")
        current = game.get("prefix", "default")
        if current not in self.prefix_names:
            self.prefix_names.append(current)
        self.prefix = Adw.ComboRow(title="Prefix", subtitle="Separate saves, settings and files per prefix")
        self.prefix.set_model(Gtk.StringList.new(self.prefix_names))
        self.prefix.set_selected(self.prefix_names.index(current))
        for row in (self.name, self.exe, self.prefix, self.audio, self.speed, self.cap):
            group.add(row)
        page.add(group)

        steam = Adw.PreferencesGroup(title="Steam")
        self.steam_mode = Adw.ComboRow(title="Steam API")
        self.steam_mode.set_model(Gtk.StringList.new([label for _key, label, _d in STEAM_API_MODES]))
        keys = [key for key, _l, _d in STEAM_API_MODES]
        current_mode = game.get("steam_mode", "auto")
        self.steam_mode.set_selected(keys.index(current_mode) if current_mode in keys else 0)
        self.steam_appid = Adw.EntryRow(title="Steam App ID")
        self.steam_appid.set_input_purpose(Gtk.InputPurpose.DIGITS)
        self.steam_appid.set_text(str(game.get("appid", "")) if game.get("appid") else str(game.get("steam_appid", "")))
        self.steam_appid.set_sensitive(game.get("source") != "steam")
        self.steam_mode.connect("notify::selected", lambda *_a: self._sync_steam_rows())
        steam.add(self.steam_mode)
        steam.add(self.steam_appid)
        page.add(steam)
        self._sync_steam_rows()

        hw = Adw.PreferencesGroup(title="Hardware shown to the game",
                                  description="Changes what the game is told through Windows APIs (DXGI adapter, display devices, registry CPU "
                                              "name, processor count). The real GPU still renders and CPUID is not changed, so this is "
                                              "experimental and can break vendor-specific paths.")
        self.gpu_choices = ["Real hardware"] + list(GPU_PRESETS) + ["Custom…"]
        self.gpu = Adw.ComboRow(title="GPU")
        self.gpu.set_model(Gtk.StringList.new(self.gpu_choices))
        gpu_game = game.get("gpu", {})
        self.gpu_name = Adw.EntryRow(title="GPU name")
        self.gpu_vendor = Adw.EntryRow(title="Vendor ID (hex, e.g. 10de)")
        self.gpu_device = Adw.EntryRow(title="Device ID (hex)")
        self.gpu_vram = Adw.SpinRow.new_with_range(512, 65536, 512)
        self.gpu_vram.set_title("Video memory (MB)")
        self.gpu_vram.set_value(float(gpu_game.get("vram", 8192)))
        self.gpu_name.set_text(gpu_game.get("name", ""))
        self.gpu_vendor.set_text(gpu_game.get("vendor", ""))
        self.gpu_device.set_text(gpu_game.get("device", ""))
        preset = gpu_game.get("preset", "Real hardware")
        self.gpu.set_selected(self.gpu_choices.index(preset) if preset in self.gpu_choices else 0)
        self.gpu.connect("notify::selected", self._sync_hw_rows)
        self.cpu_choices = ["Real hardware"] + list(CPU_PRESETS) + ["Custom…"]
        self.cpu = Adw.ComboRow(title="CPU")
        self.cpu.set_model(Gtk.StringList.new(self.cpu_choices))
        cpu_game = game.get("cpu", {})
        self.cpu_name = Adw.EntryRow(title="CPU name")
        self.cpu_name.set_text(cpu_game.get("name", ""))
        self.cpu_vendor = Adw.ComboRow(title="CPU vendor")
        self.cpu_vendor.set_model(Gtk.StringList.new(["GenuineIntel", "AuthenticAMD"]))
        self.cpu_vendor.set_selected(1 if cpu_game.get("vendor") == "AuthenticAMD" else 0)
        self.cpu_threads = Adw.SpinRow.new_with_range(1, 64, 1)
        self.cpu_threads.set_title("Logical processors")
        self.cpu_threads.set_value(float(cpu_game.get("threads", 16)))
        self.cpu_mhz = Adw.SpinRow.new_with_range(800, 7000, 100)
        self.cpu_mhz.set_title("Clock (MHz)")
        self.cpu_mhz.set_value(float(cpu_game.get("mhz", 3600)))
        cpu_preset = cpu_game.get("preset", "Real hardware")
        self.cpu.set_selected(self.cpu_choices.index(cpu_preset) if cpu_preset in self.cpu_choices else 0)
        self.cpu.connect("notify::selected", self._sync_hw_rows)
        for row in (self.gpu, self.gpu_name, self.gpu_vendor, self.gpu_device, self.gpu_vram,
                    self.cpu, self.cpu_name, self.cpu_vendor, self.cpu_threads, self.cpu_mhz):
            hw.add(row)
        page.add(hw)
        self._sync_hw_rows()

        dlss_group = Adw.PreferencesGroup(
            title="DLSS",
            description="For games with DLSS (NVIDIA). The game renders at the internal resolution you pick and DLSS "
                        "upscales to the game's output resolution, whatever quality mode is chosen in the game. "
                        "Starting values; while the game runs they can be changed live on its card.")
        self.dlss = DlssControls(dlss_settings(game))
        for row in self.dlss.rows:
            dlss_group.add(row)
        page.add(dlss_group)
        guard_group = Adw.PreferencesGroup(
            title="Memory",
            description="A memory cap keeps the game from using up the PC's RAM (it runs in its own systemd scope). "
                        "The low-memory guard pauses the game with a warning before memory runs out, so you can close "
                        "other apps and continue instead of the game or the PC being killed.")
        saved_cap = game.get("memory_cap") or {}
        self.cap_on = Adw.SwitchRow(title="Limit the game's memory")
        self.cap_on.set_active(bool(saved_cap.get("enabled")))
        self.cap_gb = Adw.SpinRow.new_with_range(1, 512, 0.5)
        self.cap_gb.set_digits(1)
        self.cap_gb.set_title("Memory cap (GB)")
        self.cap_gb.set_subtitle("RAM the game may use; it is slowed a little before the cap and stopped at it")
        self.cap_gb.set_value(float(saved_cap.get("gb", 9)))
        self.cap_swap = Adw.SpinRow.new_with_range(0, 64, 0.5)
        self.cap_swap.set_digits(1)
        self.cap_swap.set_title("Swap allowed beyond the cap (GB)")
        self.cap_swap.set_value(float(saved_cap.get("swap_gb", 1)))
        self.cap_gb.set_sensitive(self.cap_on.get_active())
        self.cap_swap.set_sensitive(self.cap_on.get_active())
        self.cap_on.connect("notify::active", lambda row, _p: (self.cap_gb.set_sensitive(row.get_active()),
                                                              self.cap_swap.set_sensitive(row.get_active())))
        for row in (self.cap_on, self.cap_gb, self.cap_swap):
            guard_group.add(row)
        saved_guard = game.get("memguard") or {}
        self.guard_on = Adw.SwitchRow(title="Low-memory guard")
        self.guard_on.set_active(saved_guard.get("enabled", True))
        self.guard_free = Adw.SpinRow.new_with_range(0, 65536, 128)
        self.guard_free.set_title("Pause when free RAM drops below (MB)")
        self.guard_free.set_subtitle("0 = never; system memory available to apps")
        self.guard_free.set_value(float(saved_guard.get("free_mb", 1024)))
        self.guard_margin = Adw.SpinRow.new_with_range(0, 65536, 50)
        self.guard_margin.set_title("Pause this close to a memory cap (MB)")
        self.guard_margin.set_subtitle("Only when the game runs under a memory limit; 0 = never")
        self.guard_margin.set_value(float(saved_guard.get("margin_mb", 300)))
        self.guard_auto = Adw.SwitchRow(title="Resume automatically when memory recovers")
        self.guard_auto.set_active(bool(saved_guard.get("auto_resume", False)))
        for row in (self.guard_on, self.guard_free, self.guard_margin, self.guard_auto):
            guard_group.add(row)
        page.add(guard_group)
        options = Adw.PreferencesGroup(title="Loader options", description="Every launch option the loader supports. Leave them off unless a game needs one.")
        self.presenter = Adw.ComboRow(title="Presenter", subtitle="--presenter=…", model=Gtk.StringList.new(PRESENTERS))
        presenter = game.get("presenter", "Default")
        self.presenter.set_selected(PRESENTERS.index(presenter) if presenter in PRESENTERS else 0)
        self.present_mode = Adw.ComboRow(title="Present mode", subtitle="fifo = vsync, mailbox = low latency, immediate = tearing",
                                         model=Gtk.StringList.new(PRESENT_MODES))
        mode = game.get("present_mode", "Default")
        self.present_mode.set_selected(PRESENT_MODES.index(mode) if mode in PRESENT_MODES else 0)
        options.add(self.presenter)
        options.add(self.present_mode)
        self.flag_rows = {}
        saved_flags = game.get("flags", {})
        for key, label, _env, _value, description in LAUNCH_FLAGS:
            row = Adw.SwitchRow(title=label, subtitle=description)
            row.set_active(bool(saved_flags.get(key, False)))
            options.add(row)
            self.flag_rows[key] = row
        page.add(options)
        values = Adw.PreferencesGroup(title="Loader values", description="Leave empty to use the default.")
        self.value_rows = {}
        saved_values = game.get("values", {})
        for key, label, _env, description in LAUNCH_VALUES:
            row = Adw.EntryRow(title=label)
            row.set_tooltip_text(description)
            row.set_text(str(saved_values.get(key, "")))
            if key in PATH_VALUES:
                folder, sep = PATH_VALUES[key]
                add_path_picker(row, label, folder=folder, append_sep=sep)
            values.add(row)
            self.value_rows[key] = row
        page.add(values)
        adv = Adw.PreferencesGroup(title="Advanced")
        self.args = Adw.EntryRow(title="Extra loader arguments")
        self.args.set_text(game.get("args", ""))
        self.game_args = Adw.EntryRow(title="Game arguments (passed to the exe)")
        self.game_args.set_text(game.get("game_args", ""))
        self.env = Adw.EntryRow(title="Environment (KEY=value KEY2=value)")
        self.env.set_text(game.get("env", ""))
        adv.add(self.game_args)
        adv.add(self.args)
        adv.add(self.env)
        page.add(adv)
        view.set_content(page)
        self.set_child(view)

    def _sync_steam_rows(self):
        _key, _label, description = STEAM_API_MODES[self.steam_mode.get_selected()]
        self.steam_mode.set_subtitle(description)
        key = STEAM_API_MODES[self.steam_mode.get_selected()][0]
        if self.game.get("source") == "steam":
            self.steam_appid.set_title("Steam App ID (from the install)")
        elif key == "official":
            self.steam_appid.set_title("Steam App ID (the game your account must own)")
        else:
            self.steam_appid.set_title("Steam App ID (only needed without steam_appid.txt)")

    def _sync_hw_rows(self, *_args):
        gpu_custom = self.gpu_choices[self.gpu.get_selected()] == "Custom…"
        cpu_custom = self.cpu_choices[self.cpu.get_selected()] == "Custom…"
        for row in (self.gpu_name, self.gpu_vendor, self.gpu_device, self.gpu_vram):
            row.set_visible(gpu_custom)
        for row in (self.cpu_name, self.cpu_vendor, self.cpu_threads, self.cpu_mhz):
            row.set_visible(cpu_custom)

    def _picked(self, path):
        self.exe.set_text(path)
        if not self.name.get_text():
            base = os.path.basename(os.path.dirname(path)) or os.path.splitext(os.path.basename(path))[0]
            self.name.set_text(base)

    def _save(self, _b):
        exe = self.exe.get_text().strip()
        if not exe:
            return
        self.game.update({
            "name": self.name.get_text().strip() or os.path.splitext(os.path.basename(exe))[0],
            "exe": exe,
            "audio": AUDIO_BACKENDS[self.audio.get_selected()],
            "time_scale": round(self.speed.get_value(), 3),
            "fps_limit": int(self.cap.get_value()),
            "args": self.args.get_text().strip(),
            "game_args": self.game_args.get_text().strip(),
            "env": self.env.get_text().strip(),
            "prefix": self.prefix_names[self.prefix.get_selected()],
            "presenter": PRESENTERS[self.presenter.get_selected()],
            "present_mode": PRESENT_MODES[self.present_mode.get_selected()],
            "flags": {key: row.get_active() for key, row in self.flag_rows.items()},
            "values": {key: row.get_text().strip() for key, row in self.value_rows.items() if row.get_text().strip()},
            "dlss": self.dlss.settings(),
            "memory_cap": {"enabled": self.cap_on.get_active(), "gb": round(self.cap_gb.get_value(), 1),
                           "swap_gb": round(self.cap_swap.get_value(), 1)},
            "memguard": {"enabled": self.guard_on.get_active(), "free_mb": int(self.guard_free.get_value()),
                         "margin_mb": int(self.guard_margin.get_value()), "auto_resume": self.guard_auto.get_active()},
            "steam_mode": STEAM_API_MODES[self.steam_mode.get_selected()][0],
            "gpu": {"preset": self.gpu_choices[self.gpu.get_selected()], "name": self.gpu_name.get_text().strip(),
                    "vendor": self.gpu_vendor.get_text().strip(), "device": self.gpu_device.get_text().strip(),
                    "vram": int(self.gpu_vram.get_value())},
            "cpu": {"preset": self.cpu_choices[self.cpu.get_selected()], "name": self.cpu_name.get_text().strip(),
                    "vendor": ["GenuineIntel", "AuthenticAMD"][self.cpu_vendor.get_selected()],
                    "threads": int(self.cpu_threads.get_value()), "mhz": int(self.cpu_mhz.get_value())},
        })
        if self.game.get("source") != "steam":
            appid = self.steam_appid.get_text().strip()
            if appid.isdigit() and int(appid) > 0:
                self.game["steam_appid"] = int(appid)
            else:
                self.game.pop("steam_appid", None)
        self.on_save(self.game)
        self.close()


class LogDialog(Adw.Dialog):
    def __init__(self, name, path):
        super().__init__()
        self.set_title("Log: %s" % name)
        self.set_content_width(900)
        self.set_content_height(600)
        self.path = path
        view = Adw.ToolbarView()
        header = Adw.HeaderBar()
        copy = Gtk.Button(label="Copy")
        copy.connect("clicked", self._copy)
        header.pack_start(copy)
        view.add_top_bar(header)
        self.text = Gtk.TextView(editable=False, monospace=True, cursor_visible=False)
        self.text.set_left_margin(8)
        scroller = Gtk.ScrolledWindow()
        scroller.set_child(self.text)
        view.set_content(scroller)
        self.set_child(view)
        self.scroller = scroller
        self._refresh()
        self.timer = GLib.timeout_add_seconds(1, self._refresh)
        self.connect("closed", lambda _d: GLib.source_remove(self.timer))

    def _tail(self, limit=200 * 1024):
        try:
            with open(self.path, "rb") as f:
                f.seek(0, os.SEEK_END)
                size = f.tell()
                f.seek(max(0, size - limit))
                return f.read().decode("utf-8", "replace")
        except OSError:
            return "(no log)"

    def _refresh(self):
        buffer = self.text.get_buffer()
        adjustment = self.scroller.get_vadjustment()
        at_end = adjustment.get_value() >= adjustment.get_upper() - adjustment.get_page_size() - 40
        buffer.set_text(self._tail())
        if at_end:
            GLib.idle_add(lambda: adjustment.set_value(adjustment.get_upper()))
        return True

    def _copy(self, _b):
        self.get_clipboard().set(self._tail())


# ---------------------------------------------------------------- main window

class Manager(Adw.Application):
    def __init__(self):
        super().__init__(application_id=APP_ID, flags=Gio.ApplicationFlags.DEFAULT_FLAGS)
        self.lib = Library()
        self.children = {}          # pid -> (Popen, log path, name)
        self.cards = {}             # pid -> RunningCard
        self.mod_runner = ModRunner()

    # -- window
    def do_activate(self):
        if getattr(self, "window", None):
            self.window.present()
            return
        w = self.window = Adw.ApplicationWindow(application=self, title="Beer", default_width=900, default_height=680)
        self.toasts = Adw.ToastOverlay()
        view = Adw.ToolbarView()
        header = Adw.HeaderBar()
        self.stack = Adw.ViewStack()
        switcher = Adw.ViewSwitcher(stack=self.stack, policy=Adw.ViewSwitcherPolicy.WIDE)
        header.set_title_widget(switcher)
        add = Gtk.Button(icon_name="list-add-symbolic", tooltip_text="Add a game")
        add.connect("clicked", lambda _b: self.add_game())
        header.pack_start(add)
        menu = Gtk.Button(icon_name="emblem-system-symbolic", tooltip_text="Loader settings")
        menu.connect("clicked", lambda _b: self.show_settings())
        header.pack_end(menu)
        view.add_top_bar(header)

        # Library page
        self.library_list = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=18)
        self.library_empty = Adw.StatusPage(icon_name="applications-games-symbolic", title="No games yet",
                                            description="Use the + button to add a game's .exe")
        lib_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        lib_box.append(self.library_empty)
        lib_clamp = Adw.Clamp(maximum_size=720, margin_top=18, margin_bottom=18, margin_start=12, margin_end=12)
        lib_clamp.set_child(self.library_list)
        lib_box.append(lib_clamp)
        lib_scroll = Gtk.ScrolledWindow(vexpand=True)
        lib_scroll.set_child(lib_box)
        self.stack.add_titled_with_icon(lib_scroll, "library", "Library", "view-grid-symbolic")
        self.steam_page = SteamPage(self)
        self.stack.add_titled_with_icon(self.steam_page, "steam", "Steam", "avatar-default-symbolic")

        # Running page
        self.running_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=18)
        self.running_empty = Adw.StatusPage(icon_name="media-playback-start-symbolic", title="Nothing running",
                                            description="Games started from here — or from a terminal — show up automatically")
        run_outer = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        run_outer.append(self.running_empty)
        run_clamp = Adw.Clamp(maximum_size=720, margin_top=18, margin_bottom=18, margin_start=12, margin_end=12)
        run_clamp.set_child(self.running_box)
        run_outer.append(run_clamp)
        run_scroll = Gtk.ScrolledWindow(vexpand=True)
        run_scroll.set_child(run_outer)
        self.stack.add_titled_with_icon(run_scroll, "running", "Running", "utilities-system-monitor-symbolic")

        # Prefixes page
        self.prefix_list = Gtk.ListBox(selection_mode=Gtk.SelectionMode.NONE)
        self.prefix_list.add_css_class("boxed-list")
        new_row = Adw.ActionRow(title="Prefixes", subtitle="A prefix is a private fake C: drive: saves, settings and installed files. "
                                                            "New ones live in ~/.beer/prefixes.")
        new_button = Gtk.Button(label="New prefix", valign=Gtk.Align.CENTER)
        new_button.add_css_class("suggested-action")
        new_button.connect("clicked", lambda _b: self.new_prefix())
        new_row.add_suffix(new_button)
        head = Gtk.ListBox(selection_mode=Gtk.SelectionMode.NONE)
        head.add_css_class("boxed-list")
        head.append(new_row)
        pbox = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=18)
        pbox.append(head)
        pbox.append(self.prefix_list)
        pclamp = Adw.Clamp(maximum_size=720, margin_top=18, margin_bottom=18, margin_start=12, margin_end=12)
        pclamp.set_child(pbox)
        pscroll = Gtk.ScrolledWindow(vexpand=True)
        pscroll.set_child(pclamp)
        self.stack.add_titled_with_icon(pscroll, "prefixes", "Prefixes", "folder-symbolic")

        self.overlay_page = OverlayPage(self)
        self.stack.add_titled_with_icon(self.overlay_page, "overlay", "Overlay", "view-reveal-symbolic")
        self.display_page = DisplayPage(self)
        self.stack.add_titled_with_icon(self.display_page, "display", "Display", "video-display-symbolic")
        self.capture_page = CapturePage(self)
        self.stack.add_titled_with_icon(self.capture_page, "capture", "Capture", "media-record-symbolic")
        self.mods_page = ModsPage(self)
        self.stack.add_titled_with_icon(self.mods_page, "mods", "Mods", "application-x-addon-symbolic")

        view.set_content(self.stack)
        self.toasts.set_child(view)
        w.set_content(self.toasts)
        self.rebuild_library()
        self.rebuild_prefixes()
        GLib.timeout_add_seconds(1, self.tick)
        self.tick()
        w.present()

    def toast(self, text):
        self.toasts.add_toast(Adw.Toast.new(text))

    # -- library
    def rebuild_library(self):
        child = self.library_list.get_first_child()
        while child:
            nxt = child.get_next_sibling()
            self.library_list.remove(child)
            child = nxt
        games = self.lib.games
        self.library_empty.set_visible(not games)
        if hasattr(self, "prefix_list"):
            self.rebuild_prefixes()
        self.library_list.set_visible(bool(games))
        # Games installed by the Steam session provider carry source "steam"; everything else was imported by hand.
        sections = [("Local Imports", "Games added from an .exe on disk", lambda g: g.get("source") != "steam"),
                    ("Steam Games", "Downloaded from your Steam account", lambda g: g.get("source") == "steam")]
        for title, description, belongs in sections:
            members = [(index, game) for index, game in enumerate(games) if belongs(game)]
            if not members:
                continue
            rows = Gtk.ListBox(selection_mode=Gtk.SelectionMode.NONE, margin_top=6)
            rows.add_css_class("boxed-list")
            for index, game in members:
                rows.append(self._library_row(index, game))
            label = Gtk.Label(use_markup=True, xalign=0,
                              label="<b>%s</b>  <span alpha=\"60%%\">%d · %s</span>" % (title, len(members), description))
            collapsed = self.lib.settings.setdefault("collapsed_sections", [])
            section = Gtk.Expander(label_widget=label, expanded=title not in collapsed, child=rows)
            section.connect("notify::expanded", self._section_toggled, title)
            self.library_list.append(section)

    def _section_toggled(self, section, _param, title):
        collapsed = self.lib.settings.setdefault("collapsed_sections", [])
        if section.get_expanded() and title in collapsed:
            collapsed.remove(title)
        elif not section.get_expanded() and title not in collapsed:
            collapsed.append(title)
        self.lib.save()

    def _library_row(self, index, game):
        subtitle = game["exe"]
        if game.get("source") == "steam":
            subtitle = "App %s · %s" % (game.get("appid", "?"), game["exe"])
        row = Adw.ActionRow(title=GLib.markup_escape_text(game["name"]), subtitle=GLib.markup_escape_text(subtitle))
        row.set_subtitle_lines(1)
        pid = self.running_pid(game)
        if pid:
            badge = Gtk.Box(spacing=6, valign=Gtk.Align.CENTER)
            dot = Gtk.Image(icon_name="media-record-symbolic", pixel_size=10)
            dot.add_css_class("success")
            badge.append(dot)
            label = Gtk.Label(label="Running")
            label.add_css_class("success")
            label.add_css_class("caption-heading")
            badge.append(label)
            show = Gtk.Button(icon_name="utilities-system-monitor-symbolic", valign=Gtk.Align.CENTER,
                              tooltip_text="Show in Running")
            show.add_css_class("flat")
            show.connect("clicked", lambda _b: self.stack.set_visible_child_name("running"))
            row.add_suffix(badge)
            row.add_suffix(show)
            play = Gtk.Button(icon_name="media-playback-stop-symbolic", valign=Gtk.Align.CENTER, tooltip_text="Stop")
            play.add_css_class("destructive-action")
            play.add_css_class("circular")
            play.connect("clicked", lambda _b, p=pid: self.stop_game(p))
        else:
            play = Gtk.Button(icon_name="media-playback-start-symbolic", valign=Gtk.Align.CENTER, tooltip_text="Launch")
            play.add_css_class("suggested-action")
            play.add_css_class("circular")
            play.connect("clicked", lambda _b, g=game: self.launch(g))
        edit = Gtk.Button(icon_name="document-edit-symbolic", valign=Gtk.Align.CENTER, tooltip_text="Edit")
        edit.add_css_class("flat")
        edit.connect("clicked", lambda _b, i=index: self.edit_game(i))
        remove = Gtk.Button(icon_name="user-trash-symbolic", valign=Gtk.Align.CENTER, tooltip_text="Remove from library (files are untouched)")
        remove.add_css_class("flat")
        remove.connect("clicked", lambda _b, i=index: self.remove_game(i))
        remove.set_sensitive(not pid)
        row.add_suffix(edit)
        row.add_suffix(remove)
        row.add_suffix(play)
        return row

    def sync_steam_installs(self, records):
        """Keeps the Library's Steam Games in step with what the provider has installed."""
        changed = False
        installed = {r["appid"]: r for r in records if r.get("state") == "installed" and r.get("executable")}
        for appid, record in installed.items():
            install_dir = os.path.join(record["folder"], "common", record["installdir"])
            exe = os.path.join(install_dir, record["executable"])
            workdir = os.path.join(install_dir, record["workingdir"]) if record.get("workingdir") else ""
            entry = next((g for g in self.lib.games if g.get("source") == "steam" and g.get("appid") == appid), None)
            if entry is None:
                try:
                    prefix = steam_prefix(appid)
                except (OSError, ValueError):
                    prefix = "default"
                entry = {"name": record.get("name") or "App %d" % appid, "exe": exe, "args": "",
                         "game_args": record.get("arguments", ""),
                         "env": "", "prefix": prefix, "audio": "fmod", "time_scale": 1.0, "fps_limit": 0,
                         "presenter": "Default", "present_mode": "Default", "flags": {},
                         "source": "steam", "appid": appid, "steam_folder": record["folder"], "steam_workdir": workdir}
                self.lib.games.append(entry)
                changed = True
            elif "game_args" not in entry:
                # Older entries kept the launch option's arguments with the loader's own options.
                arguments = record.get("arguments", "")
                if arguments and entry.get("args", "") == arguments:
                    entry["args"] = ""
                entry["game_args"] = arguments
                changed = True
            if entry.get("exe") != exe or entry.get("steam_workdir", "") != workdir:
                entry["exe"] = exe
                entry["steam_folder"] = record["folder"]
                entry["steam_workdir"] = workdir
                changed = True
        for entry in list(self.lib.games):
            # Drop entries whose install is gone, but not ones on a drive that is simply not mounted.
            if entry.get("source") == "steam" and entry.get("appid") not in installed and \
                    os.path.isdir(os.path.join(entry.get("steam_folder", ""), "beer-steam")):
                self.lib.games.remove(entry)
                changed = True
        if changed:
            self.lib.save()
            self.rebuild_library()

    def remember(self, exe, key, value):
        for game in self.lib.games:
            if game.get("exe") == exe and game.get(key) != value:
                game[key] = value
                self.lib.save()
                return

    # -- prefixes
    def rebuild_prefixes(self):
        child = self.prefix_list.get_first_child()
        while child:
            nxt = child.get_next_sibling()
            self.prefix_list.remove(child)
            child = nxt
        used = {}
        for game in self.lib.games:
            used.setdefault(game.get("prefix", "default"), []).append(game["name"])
        for prefix in list_prefixes():
            games = used.get(prefix["name"], [])
            subtitle = prefix["path"]
            if games:
                subtitle += "\nUsed by: " + ", ".join(games)
            row = Adw.ActionRow(title=GLib.markup_escape_text(prefix["name"] + ("  (old location)" if prefix["legacy"] else "")),
                                subtitle=GLib.markup_escape_text(subtitle))
            row.set_subtitle_lines(3)
            open_button = Gtk.Button(icon_name="folder-open-symbolic", valign=Gtk.Align.CENTER, tooltip_text="Open folder")
            open_button.add_css_class("flat")
            open_button.connect("clicked", lambda _b, p=prefix["path"]: Gio.AppInfo.launch_default_for_uri("file://" + p, None))
            clone = Gtk.Button(icon_name="edit-copy-symbolic", valign=Gtk.Align.CENTER, tooltip_text="Clone")
            clone.add_css_class("flat")
            clone.connect("clicked", lambda _b, p=prefix: self.clone_prefix(p))
            row.add_suffix(open_button)
            row.add_suffix(clone)
            if prefix["legacy"]:
                move = Gtk.Button(label="Move to ~/.beer", valign=Gtk.Align.CENTER)
                move.connect("clicked", lambda _b, p=prefix: self.migrate_prefix(p))
                row.add_suffix(move)
            remove = Gtk.Button(icon_name="user-trash-symbolic", valign=Gtk.Align.CENTER, tooltip_text="Move to ~/.beer/trash")
            remove.add_css_class("flat")
            remove.connect("clicked", lambda _b, p=prefix: self.remove_prefix(p))
            row.add_suffix(remove)
            self.prefix_list.append(row)

    def _ask_name(self, heading, body, initial, on_accept):
        dialog = Adw.AlertDialog(heading=heading, body=body)
        entry = Gtk.Entry(text=initial, activates_default=True)
        dialog.set_extra_child(entry)
        dialog.add_response("cancel", "Cancel")
        dialog.add_response("ok", "Create")
        dialog.set_response_appearance("ok", Adw.ResponseAppearance.SUGGESTED)
        dialog.set_default_response("ok")

        def done(_d, response):
            if response == "ok":
                try:
                    on_accept(entry.get_text().strip())
                except (ValueError, OSError) as error:
                    self.toast(str(error))
                self.rebuild_prefixes()
        dialog.connect("response", done)
        dialog.present(self.window)

    def new_prefix(self):
        self._ask_name("New prefix", "Name for the new prefix", "", create_prefix)

    def clone_prefix(self, prefix):
        self._ask_name("Clone %s" % prefix["name"], "Copies saves, settings and files into a new prefix.",
                       prefix["name"] + "-copy", lambda name: clone_prefix(prefix["path"], name))

    def migrate_prefix(self, prefix):
        try:
            migrate_prefix(prefix["path"])
            self.toast("Moved %s to ~/.beer/prefixes" % prefix["name"])
        except (ValueError, OSError) as error:
            self.toast(str(error))
        self.rebuild_prefixes()

    def remove_prefix(self, prefix):
        dialog = Adw.AlertDialog(heading="Remove prefix %s?" % prefix["name"],
                                 body="It is moved to ~/.beer/trash, not deleted, so saves can be recovered.")
        dialog.add_response("cancel", "Cancel")
        dialog.add_response("remove", "Move to trash")
        dialog.set_response_appearance("remove", Adw.ResponseAppearance.DESTRUCTIVE)

        def done(_d, response):
            if response == "remove":
                try:
                    trash_prefix(prefix["path"])
                except OSError as error:
                    self.toast(str(error))
                self.rebuild_prefixes()
        dialog.connect("response", done)
        dialog.present(self.window)

    def add_game(self):
        GameDialog(self, {}, self._added).present(self.window)

    def _added(self, game):
        self.lib.games.append(game)
        self.lib.save()
        self.rebuild_library()

    def edit_game(self, index):
        def saved(game):
            self.lib.games[index] = game
            self.lib.save()
            self.rebuild_library()
        GameDialog(self, self.lib.games[index], saved).present(self.window)

    def remove_game(self, index):
        game = self.lib.games[index]
        dialog = Adw.AlertDialog(heading="Remove %s?" % game["name"],
                                 body="Only the library entry is removed. Game files are never touched.")
        dialog.add_response("cancel", "Cancel")
        dialog.add_response("remove", "Remove")
        dialog.set_response_appearance("remove", Adw.ResponseAppearance.DESTRUCTIVE)

        def done(_d, response):
            if response == "remove":
                del self.lib.games[index]
                self.lib.save()
                self.rebuild_library()
        dialog.connect("response", done)
        dialog.present(self.window)

    def show_settings(self):
        dialog = Adw.Dialog(title="Loader settings", content_width=560)
        view = Adw.ToolbarView()
        view.add_top_bar(Adw.HeaderBar())
        page = Adw.PreferencesPage()
        group = Adw.PreferencesGroup(description="Paths the manager uses to start games. Defaults are detected automatically.")
        loader = Adw.EntryRow(title="Loader binary")
        loader.set_text(self.lib.settings["loader"])
        vkd3d = Adw.EntryRow(title="vkd3d-shader library")
        vkd3d.set_text(self.lib.settings["vkd3d_library"])
        cache = Adw.EntryRow(title="Shader cache folder")
        cache.set_text(self.lib.settings.get("shader_cache_dir") or storage.CACHE_DIR)
        bandwidth = Adw.SpinRow.new_with_range(0, 1000, 1)
        bandwidth.set_title("Steam download limit")
        bandwidth.set_subtitle("MB/s, 0 for no limit")
        bandwidth.set_value(float(self.lib.settings.get("steam_bandwidth_mbps") or 0))
        languages = ["english", "french", "german", "italian", "spanish", "latam", "portuguese", "brazilian", "russian",
                     "polish", "japanese", "koreana", "schinese", "tchinese", "turkish", "ukrainian", "thai"]
        language = Adw.ComboRow(title="Steam game language", model=Gtk.StringList.new(languages))
        current = self.lib.settings.get("steam_language", "english")
        language.set_selected(languages.index(current) if current in languages else 0)
        provider = Adw.EntryRow(title="Steam session provider")
        provider.set_text(self.lib.settings.get("steam_provider") or DEFAULT_PROVIDER)
        add_path_picker(loader, "Choose the loader binary")
        add_path_picker(vkd3d, "Choose the vkd3d-shader library", patterns=["*.so", "*.so.*"],
                        filter_name="Shared libraries")
        add_path_picker(cache, "Choose the shader cache folder", folder=True)
        add_path_picker(provider, "Choose the Steam session provider")
        group.add(loader)
        group.add(vkd3d)
        group.add(cache)
        group.add(provider)
        page.add(group)

        steam_group = Adw.PreferencesGroup(title="Steam downloads",
                                           description="Folders games from your Steam account are installed into. "
                                                       "Removing a folder from this list never deletes the games in it.")
        add_folder = Gtk.Button(icon_name="list-add-symbolic", valign=Gtk.Align.CENTER, tooltip_text="Add a folder")
        add_folder.add_css_class("flat")
        steam_group.set_header_suffix(add_folder)
        page.add(steam_group)
        steam_options = Adw.PreferencesGroup()
        steam_options.add(language)
        steam_options.add(bandwidth)
        page.add(steam_options)
        folders = list(self.lib.settings.get("steam_libraries") or [STEAM_LIBRARY_DIR])
        folder_rows = []

        def show_folders():
            for row in folder_rows:
                steam_group.remove(row)
            folder_rows.clear()
            for folder in folders:
                row = Adw.ActionRow(title=GLib.markup_escape_text(folder))
                remove = Gtk.Button(icon_name="list-remove-symbolic", valign=Gtk.Align.CENTER,
                                    tooltip_text="Remove from the list (files are untouched)")
                remove.add_css_class("flat")
                remove.set_sensitive(len(folders) > 1)
                remove.connect("clicked", lambda _b, f=folder: (folders.remove(f), show_folders()))
                row.add_suffix(remove)
                steam_group.add(row)
                folder_rows.append(row)

        def folder_picked(chooser, result):
            try:
                picked = chooser.select_folder_finish(result)
            except GLib.Error:
                return
            if picked and picked.get_path() not in folders:
                folders.append(picked.get_path())
                show_folders()
        add_folder.connect("clicked", lambda _b: Gtk.FileDialog(title="Choose a Steam downloads folder")
                           .select_folder(self.window, None, folder_picked))
        show_folders()
        view.set_content(page)
        dialog.set_child(view)

        def closed(_d):
            self.lib.settings["loader"] = loader.get_text().strip() or os.path.join(REPO, "loader")
            self.lib.settings["vkd3d_library"] = vkd3d.get_text().strip()
            self.lib.settings["shader_cache_dir"] = os.path.expanduser(cache.get_text().strip()) or storage.CACHE_DIR
            self.lib.settings["steam_libraries"] = folders
            self.lib.settings["steam_provider"] = provider.get_text().strip() or DEFAULT_PROVIDER
            self.lib.settings["steam_bandwidth_mbps"] = bandwidth.get_value()
            self.lib.settings["steam_language"] = languages[language.get_selected()]
            self.steam_page.send_bandwidth()
            self.steam_page._refresh_installs()
            self.lib.save()
        dialog.connect("closed", closed)
        dialog.present(self.window)

    # -- launching
    def launch(self, game):
        loader = self.lib.settings["loader"]
        if not os.path.exists(loader):
            self.toast("Loader not found: %s" % loader)
            return
        if not os.path.exists(game["exe"]):
            self.toast("Game exe not found — is the drive mounted?")
            return
        env = dict(os.environ)
        appid = game.get("appid") if game.get("source") == "steam" else game.get("steam_appid")
        if appid:
            # As Steam does for the games it launches.
            env["SteamAppId"] = env["SteamGameId"] = str(appid)
        if game.get("steam_workdir"):
            env["BEER_WORKING_DIR"] = game["steam_workdir"]     # the launch option's working directory
        mode = game.get("steam_mode", "auto")
        if mode == "auto":
            mode = "official" if game.get("source") == "steam" else None
        if mode:
            env["BEER_STEAM_API_MODE"] = mode
        if self.lib.settings.get("vkd3d_library"):
            env["BEER_VKD3D_SHADER_LIBRARY"] = self.lib.settings["vkd3d_library"]
        # Where shader caches live (pipeline caches and translated shaders), chosen here rather than by the loader.
        cache_dir = self.lib.settings.get("shader_cache_dir") or storage.CACHE_DIR
        storage.migrate_cache(cache_dir)
        env["BEER_CACHE_DIR"] = cache_dir
        scale = float(game.get("time_scale", 1.0))
        if abs(scale - 1.0) > 1e-6:
            env["BEER_TIME_SCALE"] = "%g" % scale
        if int(game.get("fps_limit", 0)) > 0:
            env["BEER_FPS_LIMIT"] = str(int(game["fps_limit"]))
        # Third-party Vulkan overlays hook the loader's swapchain; MangoHud crashed it. Beer has its own overlay.
        if not game.get("flags", {}).get("mangohud"):
            env["MANGOHUD"] = "0"
            env["DISABLE_MANGOHUD"] = "1"
        for item in shlex.split(game.get("env", "")):
            if "=" in item:
                key, value = item.split("=", 1)
                env[key] = value
        env.update(spoof_environment(game))
        env["BEER_OVERLAY_JSON"] = json.dumps(flat_message(load_overlay_config(), starting=True))
        env["BEER_POST_JSON"] = json.dumps(post_message(load_display_config()))
        cmd = [loader, "--audio=%s" % game.get("audio", "fmod"), "--prefix=%s" % prefix_path(game.get("prefix", "default"))]
        flags = game.get("flags", {})
        for key, _label, variable, value, _description in LAUNCH_FLAGS:
            if not flags.get(key):
                continue
            if variable:
                env[variable] = value
            elif key == "renderer_vulkan":
                cmd.append("--renderer=vulkan")
        values = game.get("values", {})
        for key, _label, variable, _description in LAUNCH_VALUES:
            if str(values.get(key, "")).strip():
                value = str(values[key]).strip()
                # dlopen does not expand '~'; a saved library path must also work outside a terminal.
                env[variable] = os.path.expanduser(value) if key == "steam_networking_library" else value
        env.update(dlss_environment(game))
        guard = game.get("memguard") or {}
        if guard:
            env["BEER_MEMGUARD"] = "1" if guard.get("enabled", True) else "0"
            env["BEER_MEMGUARD_FREE_MB"] = str(int(guard.get("free_mb", 1024)))
            env["BEER_MEMGUARD_MARGIN_MB"] = str(int(guard.get("margin_mb", 300)))
            env["BEER_MEMGUARD_AUTO_RESUME"] = "1" if guard.get("auto_resume") else "0"
        if game.get("presenter", "Default") != "Default":
            cmd.append("--presenter=%s" % game["presenter"])
        if game.get("present_mode", "Default") != "Default":
            env["BEER_PRESENT_MODE"] = game["present_mode"]
        # Loader options come before the exe; everything after it is the game's own command line.
        cmd += shlex.split(game.get("args", "")) + [game["exe"]] + shlex.split(game.get("game_args", ""))
        # Memory cap: the game runs in its own transient systemd scope (same PID, so tracking and the control socket
        # are unchanged). MemoryHigh slightly under the cap slows the game down before the hard stop.
        cap = game.get("memory_cap") or {}
        if cap.get("enabled"):
            if shutil.which("systemd-run"):
                cap_mb = int(float(cap.get("gb", 9)) * 1024)
                swap_mb = int(float(cap.get("swap_gb", 1)) * 1024)
                cmd = ["systemd-run", "--user", "--scope", "--quiet", "--collect",
                       "-p", "MemoryMax=%dM" % cap_mb, "-p", "MemoryHigh=%dM" % int(cap_mb * 0.94),
                       "-p", "MemorySwapMax=%dM" % swap_mb, "--"] + cmd
            else:
                self.toast("systemd-run not found: starting without the memory cap")
        os.makedirs(LOG_DIR, exist_ok=True)
        safe = "".join(c if c.isalnum() else "_" for c in game["name"])
        log_path = os.path.join(LOG_DIR, "%s-%s.log" % (safe, time.strftime("%Y%m%d-%H%M%S")))
        log = open(log_path, "wb")
        try:
            proc = subprocess.Popen(cmd, cwd=REPO, env=env, stdin=subprocess.DEVNULL, stdout=log,
                                    stderr=subprocess.STDOUT, start_new_session=True)
        except OSError as error:
            self.toast("Could not start: %s" % error)
            return
        finally:
            log.close()
        self.children[proc.pid] = (proc, log_path, game["name"])
        self.toast("Started %s" % game["name"])
        self.stack.set_visible_child_name("running")
        self.prune_logs()

    def prune_logs(self, keep=30):
        logs = sorted(glob.glob(os.path.join(LOG_DIR, "*.log")), key=os.path.getmtime)
        for old in logs[:-keep]:
            try:
                os.remove(old)
            except OSError:
                pass

    def stop_game(self, pid):
        # Launchers start the real game as a child loader (RDR2: Launcher.exe -> RDR2.exe), which outlives its parent.
        # Games started here run in their own session, so stop the whole process group.
        try:
            group = os.getpgid(pid)
            if group != os.getpgid(0):
                os.killpg(group, signal.SIGTERM)
                return
            os.kill(pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        except PermissionError:
            self.toast("Not allowed to stop PID %d" % pid)

    def show_log(self, name, path):
        LogDialog(name, path).present(self.window)

    # -- live view
    def tick(self):
        for pid, (proc, _log, _name) in list(self.children.items()):
            if proc.poll() is not None:
                del self.children[pid]
        sockets = {}
        for path in glob.glob(os.path.join(SOCKET_DIR, "*.sock")):
            try:
                pid = int(os.path.basename(path)[:-5])
            except ValueError:
                continue
            if not pid_alive(pid):
                try:
                    os.unlink(path)
                except OSError:
                    pass
                continue
            sockets[pid] = path
        # A launcher exe (RDR2's Launcher.exe) runs the real game as a child loader and then only waits for it:
        # show the game, under the launcher's library name and log, instead of two cards.
        launcher_of = {pid: parent_pid(pid) for pid in sockets}
        launcher_of = {pid: parent for pid, parent in launcher_of.items() if parent in sockets}
        # A child loader is not always the game: Ghost of Tsushima starts its crash handler (crs-handler) as one.
        # Whichever of the pair presents frames is the game; the other is the launcher or a helper.
        helpers = set()
        for child, parent in list(launcher_of.items()):
            child_frames, parent_frames = self.frames_of(sockets[child]), self.frames_of(sockets[parent])
            if parent_frames > 0 and child_frames == 0:
                helpers.add(child)
                del launcher_of[child]
        seen = set()
        for pid, path in sockets.items():
            if pid in launcher_of.values() or pid in helpers:
                continue
            seen.add(pid)
            if pid not in self.cards:
                control = Control(path)
                hello = control.request({"cmd": "hello"})
                control.close()
                if not hello or not hello.get("ok"):
                    continue
                exe = hello.get("exe", "")
                launcher = launcher_of.get(pid)
                started = self.children.get(pid) or self.children.get(launcher) or (None, "", "")
                name = next((g["name"] for g in self.lib.games if g["exe"] == exe), started[2]
                            or os.path.splitext(os.path.basename(exe))[0] or "game")
                log = started[1]
                card = RunningCard(self, pid, path, name, log, exe)
                apply_saved_tweaks(card.control, exe)
                # Games started from a terminal get the saved overlay and display settings too.
                card.control.request(flat_message(load_overlay_config(), starting=pid not in self.children))
                card.control.request(post_message(load_display_config()))
                self.cards[pid] = card
                self.running_box.append(card)
                self.mod_runner.start_for_game(pid, path, exe, name)
        for pid in list(self.cards):
            card = self.cards[pid]
            if pid not in seen or not card.refresh():
                self.running_box.remove(card)
                card.control.close()
                del self.cards[pid]
                # A card can drop for a moment while the game is busy (a failed refresh) and come back on the next
                # poll; its mods keep running unless the game process itself is gone.
                if not os.path.exists("/proc/%d" % pid):
                    self.mod_runner.stop_for_game(pid)
        self.running_empty.set_visible(not self.cards)
        running = {(card.exe, card.name): pid for pid, card in self.cards.items()}
        if running != getattr(self, "running_games", None):
            self.running_games = running
            self.rebuild_library()
        return True

    def frames_of(self, path):
        """Frames a running loader has presented (0 when it does not answer)."""
        control = Control(path)
        try:
            status = control.request({"cmd": "status"}) or {}
        finally:
            control.close()
        try:
            return int(status.get("frames", 0))
        except (TypeError, ValueError):
            return 0

    def running_pid(self, game):
        """PID of the running copy of a library game (matched by exe, or by name for launcher games)."""
        for (exe, name), pid in getattr(self, "running_games", {}).items():
            if exe == game.get("exe") or name == game.get("name"):
                return pid
        return None


def write_loader_settings():
    """The loader reads these flat copies when it was started without the manager."""
    try:
        write_overlay_loader_file(load_overlay_config())
        write_display_loader_file(load_display_config())
    except OSError:
        pass


def main():
    write_loader_settings()
    return Manager().run(sys.argv)


if __name__ == "__main__":
    sys.exit(main())
