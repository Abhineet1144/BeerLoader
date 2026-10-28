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
from overlay_page import OverlayPage, flat_message, load_config as load_overlay_config  # noqa: E402
from display_page import DisplayPage, post_message, load_config as load_display_config  # noqa: E402
from graphics_window import GraphicsWindow, apply_saved as apply_saved_tweaks  # noqa: E402

APP_ID = "dev.beer.Manager"
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
storage.migrate()
LIBRARY_FILE = storage.path("library.json")
SETTINGS_FILE = storage.path("manager.json")
LOG_DIR = storage.LOG_DIR
SOCKET_DIR = os.path.join(os.environ.get("XDG_RUNTIME_DIR") or "/tmp/beer-%d" % os.getuid(), "beer") \
    if os.environ.get("XDG_RUNTIME_DIR") else "/tmp/beer-%d" % os.getuid()

AUDIO_BACKENDS = ["fmod", "compat"]
PRESENTERS = ["Default", "auto", "vulkan", "x11"]
PRESENT_MODES = ["Default", "fifo", "mailbox", "immediate"]
# Every launch option the loader understands, as switches: (key, label, environment variable, value, description).
LAUNCH_FLAGS = [
    ("renderer_vulkan", "--renderer=vulkan", None, None, "Select the Vulkan renderer explicitly (the only one today)"),
    ("no_rt", "Disable ray tracing", "BEER_NO_RT", "1", "Never enable the Vulkan ray tracing extensions"),
    ("no_post", "Disable final-image passes", "BEER_NO_POST", "1", "No effects, overlay blending or screenshots; the overlay falls back to opaque boxes"),
    ("legacy_d3d11", "Use the legacy D3D11 path", "BEER_D3D11_LEGACY", "1", "Old D3D11 renderer instead of BD11 (the default)"),
    ("hide_steam_registry", "Hide the fake Steam registry keys", "BEER_FAKE_STEAM_REGISTRY", "0", "For games that behave differently when Steam looks installed"),
    ("exe_tls", "Run the exe's TLS callbacks", "BEER_EXECUTABLE_TLS", "1", "Needed by some protected executables"),
    ("known_patches", "Enable known guest-code patches", "BEER_ENABLE_KNOWN_TARGET_PATCHES", "1", ""),
    ("transaction_patches", "Enable transaction patches", "BEER_ENABLE_TRANSACTION_PATCHES", "1", ""),
    ("cpu_staging", "Force CPU staging in the presenter", "BEER_PRESENTER_CPU_STAGING", "1", "Slower, but a useful correctness check"),
    ("fps_log", "Print FPS to the log", "BEER_BD12_FPS", "1", "One line every 120 frames in the log"),
    ("allow_180_base", "Allow guest DLLs at 0x180000000", "BEER_ALLOW_180_BASE", "1", ""),
    ("mangohud", "Allow MangoHud", "MANGOHUD", "1", "Off by default: the MangoHud Vulkan layer hooks the swapchain and crashed RE8 (Beer has its own overlay)"),
    ("dxr_probe", "Advertise ray tracing (experimental)", "BEER_DXR_PROBE", "1", "DXR probe; the pipeline is not finished"),
]

BEER_HOME = os.path.expanduser("~/.beer")
PREFIX_DIR = os.path.join(BEER_HOME, "prefixes")
TRASH_DIR = os.path.join(BEER_HOME, "trash")
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


class Library:
    def __init__(self):
        self.games = load_json(LIBRARY_FILE, {"games": []})["games"]
        self.settings = load_json(SETTINGS_FILE, {})
        self.settings.setdefault("loader", os.path.join(REPO, "loader"))
        self.settings.setdefault("vkd3d_library", find_vkd3d_library())

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

        self.pause = Adw.SwitchRow(title="Pause game time", subtitle="Freezes the clocks the game sees; rendering keeps running")
        self.pause.connect("notify::active", self._on_pause)
        self.add(self.pause)

        actions = Adw.ActionRow(title="Session")
        log_button = Gtk.Button(label="Log", valign=Gtk.Align.CENTER)
        log_button.connect("clicked", lambda _b: app.show_log(self.name, self.log_path))
        log_button.set_sensitive(bool(log_path))
        graphics = Gtk.Button(label="Graphics…", valign=Gtk.Align.CENTER, tooltip_text="Find and tweak shader constants such as light intensity")
        graphics.connect("clicked", lambda _b: GraphicsWindow(app, self.path, self.name, self.pid, self.exe).present())
        actions.add_suffix(graphics)
        memory = Gtk.Button(label="Memory…", valign=Gtk.Align.CENTER)
        memory.connect("clicked", lambda _b: MemoryWindow(app, self.path, self.name, self.pid).present())
        actions.add_suffix(memory)
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

    def _on_pause(self, row, _param):
        if not self._updating:
            self.control.request({"cmd": "set", "name": "paused", "value": bool(row.get_active())})

    def refresh(self):
        """Returns False when the game is gone."""
        if not pid_alive(self.pid):
            return False
        status = self.control.request({"cmd": "status"})
        if not status or not status.get("ok"):
            self.stats.set_subtitle("not responding")
            return True
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
        browse = Gtk.Button(icon_name="document-open-symbolic", valign=Gtk.Align.CENTER)
        browse.add_css_class("flat")
        browse.connect("clicked", lambda _b: app.pick_exe(self._picked))
        self.exe.add_suffix(browse)
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
        adv = Adw.PreferencesGroup(title="Advanced")
        self.args = Adw.EntryRow(title="Extra loader arguments")
        self.args.set_text(game.get("args", ""))
        self.env = Adw.EntryRow(title="Environment (KEY=value KEY2=value)")
        self.env.set_text(game.get("env", ""))
        adv.add(self.args)
        adv.add(self.env)
        page.add(adv)
        view.set_content(page)
        self.set_child(view)

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
            "env": self.env.get_text().strip(),
            "prefix": self.prefix_names[self.prefix.get_selected()],
            "presenter": PRESENTERS[self.presenter.get_selected()],
            "present_mode": PRESENT_MODES[self.present_mode.get_selected()],
            "flags": {key: row.get_active() for key, row in self.flag_rows.items()},
            "gpu": {"preset": self.gpu_choices[self.gpu.get_selected()], "name": self.gpu_name.get_text().strip(),
                    "vendor": self.gpu_vendor.get_text().strip(), "device": self.gpu_device.get_text().strip(),
                    "vram": int(self.gpu_vram.get_value())},
            "cpu": {"preset": self.cpu_choices[self.cpu.get_selected()], "name": self.cpu_name.get_text().strip(),
                    "vendor": ["GenuineIntel", "AuthenticAMD"][self.cpu_vendor.get_selected()],
                    "threads": int(self.cpu_threads.get_value()), "mhz": int(self.cpu_mhz.get_value())},
        })
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

    # -- window
    def do_activate(self):
        if getattr(self, "window", None):
            self.window.present()
            return
        w = self.window = Adw.ApplicationWindow(application=self, title="Beer", default_width=760, default_height=640)
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
        self.library_list = Gtk.ListBox(selection_mode=Gtk.SelectionMode.NONE)
        self.library_list.add_css_class("boxed-list")
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
        for index, game in enumerate(games):
            row = Adw.ActionRow(title=GLib.markup_escape_text(game["name"]), subtitle=GLib.markup_escape_text(game["exe"]))
            row.set_subtitle_lines(1)
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
            row.add_suffix(edit)
            row.add_suffix(remove)
            row.add_suffix(play)
            self.library_list.append(row)

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

    def pick_exe(self, callback):
        chooser = Gtk.FileDialog(title="Choose the game's .exe")
        exe_filter = Gtk.FileFilter()
        exe_filter.set_name("Windows executables")
        exe_filter.add_pattern("*.exe")
        exe_filter.add_pattern("*.EXE")
        filters = Gio.ListStore.new(Gtk.FileFilter)
        filters.append(exe_filter)
        chooser.set_filters(filters)

        def done(dialog, result):
            try:
                f = dialog.open_finish(result)
            except GLib.Error:
                return
            if f:
                callback(f.get_path())
        chooser.open(self.window, None, done)

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
        group.add(loader)
        group.add(vkd3d)
        page.add(group)
        view.set_content(page)
        dialog.set_child(view)

        def closed(_d):
            self.lib.settings["loader"] = loader.get_text().strip() or os.path.join(REPO, "loader")
            self.lib.settings["vkd3d_library"] = vkd3d.get_text().strip()
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
        if self.lib.settings.get("vkd3d_library"):
            env["BEER_VKD3D_SHADER_LIBRARY"] = self.lib.settings["vkd3d_library"]
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
        env["BEER_OVERLAY_JSON"] = json.dumps(flat_message(load_overlay_config()))
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
        if game.get("presenter", "Default") != "Default":
            cmd.append("--presenter=%s" % game["presenter"])
        if game.get("present_mode", "Default") != "Default":
            env["BEER_PRESENT_MODE"] = game["present_mode"]
        cmd += shlex.split(game.get("args", "")) + [game["exe"]]
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
        seen = set()
        for pid, path in sockets.items():
            if pid in launcher_of.values():
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
                self.cards[pid] = card
                self.running_box.append(card)
        for pid in list(self.cards):
            card = self.cards[pid]
            if pid not in seen or not card.refresh():
                self.running_box.remove(card)
                card.control.close()
                del self.cards[pid]
        self.running_empty.set_visible(not self.cards)
        return True


def main():
    return Manager().run(sys.argv)


if __name__ == "__main__":
    sys.exit(main())
