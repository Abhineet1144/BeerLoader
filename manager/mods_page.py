"""Mods page: Python mods that run next to a game (docs/mod-api/).

A mod is distributed as a .bmod package (a zip; see mods/sdk/beer_mod/package.py). Installing one here unpacks it into
~/.beer/mods/<id>/ (mod.json manifest + entry script). Mods are enabled per game here; when a game with enabled mods
starts, the manager starts each mod as its own process with the game's control socket (BEER_SOCKET), its folder
(BEER_MOD_DIR) and the SDK plus the shared helper libraries (mods/libs) on PYTHONPATH, and stops it when the game exits.
"""
import json
import os
import signal
import subprocess
import sys
import time

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Adw, Gdk, Gio, GLib, Gtk  # noqa: E402

import storage  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODS_DIR = storage.path("mods")
SDK_DIR = os.path.join(REPO, "mods", "sdk")
LIBS_DIR = os.path.join(REPO, "mods", "libs")      # optional helper libraries (engine-specific)
DOCS = os.path.join(REPO, "docs", "mod-api", "README.md")
ENABLED_FILE = storage.path("mods-enabled.json")

sys.path.insert(0, SDK_DIR)
from beer_mod import package as bmod  # noqa: E402  (the same packaging code mod authors use)


def discover():
    """[(id, folder, manifest)] of every mod with a readable mod.json."""
    mods = []
    if not os.path.isdir(MODS_DIR):
        return mods
    for name in sorted(os.listdir(MODS_DIR)):
        if name.startswith("."):                  # .install-* / .old-* while a package is being installed
            continue
        folder = os.path.join(MODS_DIR, name)
        try:
            with open(os.path.join(folder, "mod.json")) as f:
                manifest = json.load(f)
        except (OSError, ValueError):
            continue
        manifest.setdefault("id", name)
        mods.append((manifest["id"], folder, manifest))
    return mods


def exe_key(exe):
    return os.path.basename(exe or "").lower()


def load_enabled():
    try:
        with open(ENABLED_FILE) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def save_enabled(enabled):
    tmp = ENABLED_FILE + ".tmp"
    with open(tmp, "w") as f:
        json.dump(enabled, f, indent=2)
    os.replace(tmp, ENABLED_FILE)


def mod_fits(manifest, exe):
    games = [g.lower() for g in manifest.get("games", ["*"])]
    return "*" in games or exe_key(exe) in games


def log_path(mod_id):
    return os.path.join(storage.LOG_DIR, "mod-%s.log" % mod_id)


class ModRunner:
    """Starts and stops mod processes for running games."""

    def __init__(self):
        self.procs = {}          # (game pid, mod id) -> Popen

    def start_for_game(self, pid, socket_path, exe, name):
        enabled = load_enabled().get(exe_key(exe), [])
        for mod_id, folder, manifest in discover():
            if mod_id not in enabled or (pid, mod_id) in self.procs:
                continue
            entry = os.path.join(folder, manifest.get("entry", "main.py"))
            if not os.path.isfile(entry):
                continue
            os.makedirs(storage.LOG_DIR, exist_ok=True)
            log = open(log_path(mod_id), "ab")
            log.write(("\n=== %s: started for %s (pid %d) ===\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), name, pid)).encode())
            log.flush()
            env = dict(os.environ, BEER_SOCKET=socket_path, BEER_MOD_DIR=folder, BEER_GAME_EXE=exe,
                       BEER_GAME_PID=str(pid), PYTHONUNBUFFERED="1",
                       PYTHONPATH=SDK_DIR + os.pathsep + LIBS_DIR + (os.pathsep + os.environ["PYTHONPATH"] if os.environ.get("PYTHONPATH") else ""))
            try:
                self.procs[(pid, mod_id)] = subprocess.Popen([sys.executable, entry], cwd=folder, env=env,
                                                            stdin=subprocess.DEVNULL, stdout=log,
                                                            stderr=subprocess.STDOUT, start_new_session=True)
            except OSError as error:
                log.write(("could not start: %s\n" % error).encode())
            finally:
                log.close()

    def stop_for_game(self, pid):
        for key in [k for k in self.procs if k[0] == pid]:
            self._stop(self.procs.pop(key))

    def stop_mod(self, mod_id):
        for key in [k for k in self.procs if k[1] == mod_id]:
            self._stop(self.procs.pop(key))

    @staticmethod
    def _stop(proc):
        """Stops a mod and waits for it, so its exit cleanup (removing its overlay boxes and hotkeys) is finished before
        a new copy of the same mod can start (an update would otherwise lose the new copy's boxes)."""
        if proc.poll() is not None:
            return
        try:
            os.killpg(proc.pid, signal.SIGTERM)
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                pass
        except (ProcessLookupError, PermissionError):
            pass

    def running(self, mod_id):
        """Game pids this mod is running for."""
        return [pid for (pid, m), proc in self.procs.items() if m == mod_id and proc.poll() is None]

    def reap(self):
        for key, proc in list(self.procs.items()):
            if proc.poll() is not None:
                del self.procs[key]


class ModsPage(Gtk.ScrolledWindow):
    def __init__(self, app):
        super().__init__(hscrollbar_policy=Gtk.PolicyType.NEVER, vexpand=True)
        self.app = app
        self.page = Adw.PreferencesPage()
        drop = Gtk.DropTarget.new(Gio.File, Gdk.DragAction.COPY)      # drop a .bmod onto the page to install it
        drop.connect("drop", self._dropped)
        self.add_controller(drop)
        self.set_child(self.page)
        self.groups = []
        self.status_rows = {}
        self.rebuild()
        GLib.timeout_add(1500, self._refresh_status)

    def _games(self):
        """(exe, name) of the library games, plus running games not in the library."""
        games = [(g["exe"], g["name"]) for g in self.app.lib.games if g.get("exe")]
        known = {exe_key(e) for e, _ in games}
        for card in getattr(self.app, "cards", {}).values():
            if exe_key(card.exe) not in known:
                games.append((card.exe, card.name))
                known.add(exe_key(card.exe))
        return games

    def rebuild(self):
        for group in self.groups:
            self.page.remove(group)
        self.groups = []
        self.status_rows = {}
        intro = Adw.PreferencesGroup(
            title="Mods",
            description="Python mods that run next to a game: overlays (enemy health, timers, stats), memory tools, "
                        "hotkeys. Each mod runs in its own process and starts and stops with the game it is enabled "
                        "for. Install a mod from its .bmod file (or drop the file here); installed mods live in "
                        "~/.beer/mods.")
        # The buttons go in a row below the description (not in the header, where they squeeze the text).
        buttons = Gtk.Box(spacing=6, halign=Gtk.Align.START, margin_bottom=6)
        for label, action in (("Install mod…", lambda b: self._pick_package(b)),
                              ("Docs", lambda _b: Gio.AppInfo.launch_default_for_uri("file://" + DOCS, None)),
                              ("Open mods folder", lambda _b: Gio.AppInfo.launch_default_for_uri("file://" + MODS_DIR, None)),
                              ("Reload", lambda _b: self.rebuild())):
            b = Gtk.Button(label=label)
            if label.startswith("Install"):
                b.add_css_class("suggested-action")          # the main action of the page
            b.connect("clicked", action)
            buttons.append(b)
        intro.add(buttons)
        self.page.add(intro)
        self.groups.append(intro)
        mods = discover()
        if not mods:
            intro.add(Adw.ActionRow(title="No mods installed",
                                    subtitle="Install a .bmod file with \u201cInstall mod\u2026\u201d, or see Docs to write one."))
        games = self._games()
        enabled = load_enabled()
        for mod_id, folder, manifest in mods:
            group = Adw.PreferencesGroup(
                title=GLib.markup_escape_text("%s %s" % (manifest.get("name", mod_id), manifest.get("version", ""))),
                description=GLib.markup_escape_text(manifest.get("description", "")))
            log_button = Gtk.Button(icon_name="text-x-generic-symbolic", tooltip_text="Show the log", valign=Gtk.Align.CENTER)
            log_button.add_css_class("flat")
            log_button.connect("clicked", lambda _b, m=mod_id: self._show_log(m))
            remove_button = Gtk.Button(icon_name="user-trash-symbolic", tooltip_text="Remove this mod", valign=Gtk.Align.CENTER)
            remove_button.add_css_class("flat")
            remove_button.connect("clicked", lambda _b, m=mod_id, n=manifest.get("name", mod_id): self._remove(m, n))
            header = Gtk.Box(spacing=2)
            header.append(log_button)
            header.append(remove_button)
            group.set_header_suffix(header)
            status = Adw.ActionRow(title="Not running")
            status.add_css_class("dim-label")
            group.add(status)
            self.status_rows[mod_id] = status
            fitting = [(exe, name) for exe, name in games if mod_fits(manifest, exe)]
            if not fitting:
                group.add(Adw.ActionRow(title="No matching game in the library",
                                        subtitle="Made for: " + ", ".join(manifest.get("games", ["*"]))))
            for exe, name in fitting:
                row = Adw.SwitchRow(title=GLib.markup_escape_text(name), subtitle="Run with this game")
                row.set_active(mod_id in enabled.get(exe_key(exe), []))
                row.connect("notify::active", lambda r, _p, m=mod_id, e=exe: self._toggle(m, e, r.get_active()))
                group.add(row)
            settings = manifest.get("settings", [])
            if settings:
                group.add(self._settings_panel(mod_id, folder, settings))
            self.page.add(group)
            self.groups.append(group)
        self._refresh_status()

    # ---- packages ---------------------------------------------------------------------------------------------
    def _dropped(self, _target, value, _x, _y):
        path = value.get_path() if value else None
        if not path or not path.endswith(bmod.EXTENSION):
            return False
        self.install_file(path)
        return True

    def _pick_package(self, button):
        chooser = Gtk.FileDialog(title="Install a mod")
        file_filter = Gtk.FileFilter()
        file_filter.set_name("Beer mods (*.bmod)")
        file_filter.add_pattern("*" + bmod.EXTENSION)
        filters = Gio.ListStore.new(Gtk.FileFilter)
        filters.append(file_filter)
        chooser.set_filters(filters)

        def done(dialog, result):
            try:
                f = dialog.open_finish(result)
            except GLib.Error:
                return
            if f and f.get_path():
                self.install_file(f.get_path())
        root = button.get_root()
        chooser.open(root if isinstance(root, Gtk.Window) else None, None, done)

    def install_file(self, path):
        """Installs a .bmod; a mod that is running is restarted with the new version."""
        runner = self.app.mod_runner
        try:
            manifest, _members, _ = bmod.inspect(path)
            was_running = runner.running(manifest["id"])
            runner.stop_mod(manifest["id"])
            manifest, previous = bmod.install(path, MODS_DIR)
        except (bmod.PackageError, OSError) as error:
            self._message("Could not install %s" % os.path.basename(path), str(error))
            return
        name = "%s %s" % (manifest.get("name", manifest["id"]), manifest.get("version", ""))
        if previous:
            self.app.toast("Updated %s (was %s)" % (name, previous.get("version", "?")))
        else:
            self.app.toast("Installed %s. Enable it for a game below." % name)
        if was_running:
            for card in list(getattr(self.app, "cards", {}).values()):
                runner.start_for_game(card.pid, card.path, card.exe, card.name)
        self.rebuild()

    def _remove(self, mod_id, name):
        dialog = Adw.AlertDialog(heading="Remove %s?" % name,
                                 body="The mod, its settings and the data it saved are deleted. Its .bmod file, if "
                                      "you still have it, is not touched.")
        dialog.add_response("cancel", "Cancel")
        dialog.add_response("remove", "Remove")
        dialog.set_response_appearance("remove", Adw.ResponseAppearance.DESTRUCTIVE)

        def done(_d, response):
            if response != "remove":
                return
            self.app.mod_runner.stop_mod(mod_id)
            try:
                bmod.uninstall(mod_id, MODS_DIR)
            except (bmod.PackageError, OSError) as error:
                self._message("Could not remove %s" % name, str(error))
                return
            enabled = load_enabled()
            for exe in list(enabled):
                enabled[exe] = [m for m in enabled[exe] if m != mod_id]
            save_enabled(enabled)
            self.app.toast("Removed %s" % name)
            self.rebuild()
        dialog.connect("response", done)
        dialog.present(self.get_root())

    def _message(self, heading, body):
        dialog = Adw.AlertDialog(heading=heading, body=body)
        dialog.add_response("ok", "OK")
        dialog.present(self.get_root())

    def _toggle(self, mod_id, exe, on):
        enabled = load_enabled()
        mods = set(enabled.get(exe_key(exe), []))
        (mods.add if on else mods.discard)(mod_id)
        enabled[exe_key(exe)] = sorted(mods)
        save_enabled(enabled)
        runner = self.app.mod_runner
        for pid, card in getattr(self.app, "cards", {}).items():   # apply to a game that is already running
            if exe_key(card.exe) != exe_key(exe):
                continue
            if on:
                runner.start_for_game(pid, card.path, card.exe, card.name)
            else:
                for key in [k for k in runner.procs if k == (pid, mod_id)]:
                    runner._stop(runner.procs.pop(key))
        self._refresh_status()

    def _settings_panel(self, mod_id, folder, settings):
        path = os.path.join(folder, "settings.json")
        try:
            with open(path) as f:
                values = json.load(f)
        except (OSError, ValueError):
            values = {}
        expander = Adw.ExpanderRow(title="Settings", subtitle="Applied the next time the mod starts")

        def store(key, value):
            values[key] = value
            with open(path + ".tmp", "w") as f:
                json.dump(values, f, indent=2)
            os.replace(path + ".tmp", path)

        for s in settings:
            key, label, kind = s.get("key"), s.get("label", s.get("key")), s.get("type", "text")
            current = values.get(key, s.get("default"))
            if kind == "bool":
                row = Adw.SwitchRow(title=label)
                row.set_active(bool(current))
                row.connect("notify::active", lambda r, _p, k=key: store(k, r.get_active()))
            elif kind in ("int", "float"):
                row = Adw.SpinRow.new_with_range(float(s.get("min", 0)), float(s.get("max", 1000000)),
                                                 1 if kind == "int" else 0.1)
                row.set_title(label)
                if kind == "float":
                    row.set_digits(2)
                row.set_value(float(current or 0))
                row.connect("notify::value", lambda r, _p, k=key, t=kind: store(k, int(r.get_value()) if t == "int"
                                                                                  else round(r.get_value(), 3)))
            elif kind == "choice":
                choices = [str(c) for c in s.get("choices", [])]
                row = Adw.ComboRow(title=label, model=Gtk.StringList.new(choices))
                if str(current) in choices:
                    row.set_selected(choices.index(str(current)))
                row.connect("notify::selected", lambda r, _p, k=key, c=choices: store(k, c[r.get_selected()]))
            else:
                row = Adw.EntryRow(title=label)
                row.set_text(str(current or ""))
                row.connect("changed", lambda r, k=key: store(k, r.get_text()))
            if s.get("description"):
                row.set_tooltip_text(s["description"])
            expander.add_row(row)
        return expander

    def _refresh_status(self):
        runner = self.app.mod_runner
        runner.reap()
        names = {pid: card.name for pid, card in getattr(self.app, "cards", {}).items()}
        for mod_id, row in self.status_rows.items():
            pids = runner.running(mod_id)
            row.set_title("Running for " + ", ".join(names.get(p, str(p)) for p in pids) if pids else "Not running")
        return True

    def _show_log(self, mod_id):
        try:
            with open(log_path(mod_id), errors="replace") as f:
                text = "".join(f.readlines()[-400:])
        except OSError:
            text = "(no log yet)"
        dialog = Adw.Dialog(title="Log: %s" % mod_id, content_width=860, content_height=560)
        view = Adw.ToolbarView()
        view.add_top_bar(Adw.HeaderBar())
        buffer = Gtk.TextBuffer()
        buffer.set_text(text)
        text_view = Gtk.TextView(buffer=buffer, editable=False, monospace=True, wrap_mode=Gtk.WrapMode.WORD_CHAR)
        scroll = Gtk.ScrolledWindow(child=text_view, vexpand=True)
        view.set_content(scroll)
        dialog.set_child(view)
        dialog.present(self.app.window)
        GLib.idle_add(lambda: scroll.get_vadjustment().set_value(scroll.get_vadjustment().get_upper()) or False)
