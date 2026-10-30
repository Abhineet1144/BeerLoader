"""Objects window: finds game objects by their transform, the player by walking it, and edits what is around it.

The loader does the work (obj_* commands, platform/beer_objects.h); this window shows and edits the results.
Type names chosen here are remembered per game in ~/.beer/objects.json.
"""
import json
import os
import struct

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Adw, GLib, Gtk  # noqa: E402

import storage  # noqa: E402
import transforms  # noqa: E402
from memory_window import MemClient  # noqa: E402

LABELS_FILE = storage.path("objects.json")
FIELD_BYTES = 512


def load_labels():
    try:
        with open(LABELS_FILE, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def save_labels(data):
    try:
        with open(LABELS_FILE, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2)
    except OSError:
        pass


def group_key(item):
    """Stable across runs of the same game: type name, transform offset, pointer offset and layout."""
    return "%s|%s|%s|%s" % (item.get("name"), item.get("offset", ""), item.get("via", ""), item.get("kind"))


def fmt_vec(v, digits=2):
    return ", ".join("%.*f" % (digits, x) for x in v)


class ObjectsWindow(Adw.Window):
    def __init__(self, app, path, name, pid=None, exe=""):
        super().__init__(title="Objects — %s" % name, default_width=1180, default_height=760)
        self.set_transient_for(app.window)
        self.client = MemClient(path)
        self.pid = pid
        self.game = os.path.basename(exe) or name
        self.labels = load_labels()
        self.groups = {}           # id -> group dict from the last scan
        self.player = None         # item dict of the player
        self.selected = None       # item dict being edited
        self.frozen = []           # addresses frozen for the selected object
        self.field_rows = []
        self.alive = True
        self.busy = False
        self.connect("close-request", self._on_close)

        view = Adw.ToolbarView()
        header = Adw.HeaderBar()
        self.scan_button = Gtk.Button(label="Scan")
        self.scan_button.add_css_class("suggested-action")
        self.scan_button.set_tooltip_text("Find every object with a position in the game's memory (10–30 s)")
        self.scan_button.connect("clicked", self._scan)
        self.player_button = Gtk.Button(label="Find player")
        self.player_button.set_tooltip_text("Walks your character forward and back for 3 s to find it. "
                                            "The game window must have focus and the character must be able to move.")
        self.player_button.connect("clicked", self._find_player)
        header.pack_start(self.scan_button)
        header.pack_start(self.player_button)
        self.mode = Gtk.DropDown.new_from_strings(["Near player", "All types"])
        self.mode.connect("notify::selected", lambda *_a: self._show_list())
        self.radius = Gtk.SpinButton.new_with_range(1, 5000, 5)
        self.radius.set_value(50)
        self.radius.set_tooltip_text("Radius around the player, in game units (metres in most games)")
        self.radius.connect("value-changed", lambda *_a: self._show_list())
        header.pack_end(self.radius)
        header.pack_end(Gtk.Label(label="Radius"))
        header.pack_end(self.mode)
        view.add_top_bar(header)

        self.toasts = Adw.ToastOverlay()
        body = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        self.status = Gtk.Label(xalign=0, margin_start=12, margin_end=12, margin_top=6, margin_bottom=6, wrap=True)
        self.status.add_css_class("dim-label")
        self.status.set_text("Scan to find objects, then Find player to list what is around your character.")
        body.append(self.status)
        paned = Gtk.Paned(orientation=Gtk.Orientation.HORIZONTAL, vexpand=True, position=470)
        left = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        self.back_button = Gtk.Button(label="← All types", visible=False, halign=Gtk.Align.START, margin_start=6)
        self.back_button.add_css_class("flat")
        self.back_button.connect("clicked", lambda _b: self._show_list())
        left.append(self.back_button)
        self.list = Gtk.ListBox(selection_mode=Gtk.SelectionMode.SINGLE)
        self.list.add_css_class("navigation-sidebar")
        self.list.connect("row-activated", self._row_activated)
        scroll = Gtk.ScrolledWindow(vexpand=True)
        scroll.set_child(self.list)
        left.append(scroll)
        paned.set_start_child(left)
        paned.set_end_child(self._editor())
        body.append(paned)
        self.toasts.set_child(body)
        view.set_content(self.toasts)
        self.set_content(view)
        GLib.timeout_add(1000, self._refresh)
        GLib.timeout_add(700, self._watch_game)

    # ------------------------------------------------------------ lifecycle
    def _watch_game(self):
        if not self.alive:
            return False
        if not os.path.exists(self.client.path):
            self.alive = False
            self.close()
            return False
        return True

    def _on_close(self, *_args):
        self.alive = False
        for addr in self.frozen:
            self.client.ask({"cmd": "mem_unfreeze", "addr": hex(addr)}, lambda _r: False)
        return False

    def toast(self, text):
        self.toasts.add_toast(Adw.Toast.new(text))

    def _busy(self, busy, text=None):
        self.busy = busy
        self.scan_button.set_sensitive(not busy)
        self.player_button.set_sensitive(not busy)
        if text:
            self.status.set_text(text)

    # ------------------------------------------------------------ naming
    def _label(self, item):
        return self.labels.get(self.game, {}).get("types", {}).get(group_key(item))

    def _title(self, item):
        label = self._label(item)
        return label if label else item.get("name", "?")

    # ------------------------------------------------------------ scan / player
    def _scan(self, _b):
        self._busy(True, "Scanning the game's memory for objects…")
        self.client.ask({"cmd": "obj_scan"}, self._scan_done)

    def _scan_done(self, reply):
        self._busy(False)
        if not reply.get("ok"):
            self.status.set_text(reply.get("error", "scan failed"))
            return False
        self.groups = {g["id"]: g for g in reply["groups"]}
        self.player = None
        self.status.set_text("%d objects in %d types (%d MB scanned in %.1f s). Press Find player next."
                             % (reply["typed"], reply["group_count"], reply["scanned_mb"], reply["ms"] / 1000.0))
        self.mode.set_selected(1)
        self._show_list()
        return False

    def _find_player(self, _b):
        if not self.groups:
            self.toast("Scan first")
            return
        self._busy(True, "Walking your character forward and back… keep the game window focused.")
        self.client.ask({"cmd": "obj_find_player"}, self._player_done)

    def _player_done(self, reply):
        self._busy(False)
        items = reply.get("items") if reply.get("ok") else None
        if not items:
            self.status.set_text(reply.get("error") or "Nothing moved forward and back. Give the game window focus, "
                                 "make sure the character can walk (not in a menu or cutscene) and try again.")
            return False
        self.player = items[0]
        self.candidates = items
        self.status.set_text("Player: %s at %s. %d other candidates moved with it (camera, mount); pick one from "
                             "the list if the guess is wrong." % (self._title(self.player), fmt_vec(transforms.position(
                                 self.player["kind"], self.player["v"])), len(items) - 1))
        self.mode.set_selected(0)
        self._show_list()
        self._select(self.player)
        return False

    # ------------------------------------------------------------ list
    def _clear_list(self):
        while (row := self.list.get_row_at_index(0)) is not None:
            self.list.remove(row)

    def _show_list(self):
        self.back_button.set_visible(False)
        if self.mode.get_selected() == 0:
            if not self.player:
                self._clear_list()
                self._placeholder("Find player first, or switch to All types.")
                return
            pos = transforms.position(self.player["kind"], self.player["v"])
            self.client.ask({"cmd": "obj_near", "x": pos[0], "y": pos[1], "z": pos[2],
                             "radius": self.radius.get_value(), "limit": 300}, self._near_done)
        else:
            self._clear_list()
            groups = sorted(self.groups.values(), key=lambda g: (g["via"] < 0, -g["count"]))
            if not groups:
                self._placeholder("Press Scan.")
            for g in groups[:400]:
                row = Adw.ActionRow(title=GLib.markup_escape_text(self._title(g)), activatable=True,
                                    subtitle="%d × %s%s" % (g["count"], g["kind"],
                                                            "" if g["via"] < 0 else "  · via pointer at +0x%x" % g["via"]))
                row.group = g
                self.list.append(row)

    def _placeholder(self, text):
        row = Adw.ActionRow(title=text, activatable=False)
        self.list.append(row)

    def _near_done(self, reply):
        self._clear_list()
        if not reply.get("ok"):
            self._placeholder(reply.get("error", "failed"))
            return False
        player_base = self.player["base"] if self.player else None
        others = [c for c in getattr(self, "candidates", [])[1:]]
        for item in reply["items"]:
            tag = "Player · " if item["base"] == player_base else (
                "moved with player · " if any(c["base"] == item["base"] for c in others) else "")
            row = Adw.ActionRow(title=GLib.markup_escape_text(tag + self._title(item)), activatable=True,
                                subtitle="%.1f away · %s" % (item["distance"], fmt_vec(transforms.position(item["kind"], item["v"]), 1)))
            row.item = item
            self.list.append(row)
        if reply.get("total", 0) > len(reply["items"]):
            self._placeholder("%d more within the radius; lower it to see the nearest only." % (reply["total"] - len(reply["items"])))
        return False

    def _row_activated(self, _list, row):
        if hasattr(row, "item"):
            self._select(row.item)
        elif hasattr(row, "group"):
            self._open_group(row.group)

    def _open_group(self, group):
        self.client.ask({"cmd": "obj_list", "group": group["id"], "limit": 300}, self._group_done)

    def _group_done(self, reply):
        if not reply.get("ok"):
            self.toast(reply.get("error", "failed"))
            return False
        self._clear_list()
        self.back_button.set_visible(True)
        g = reply["group"]
        for it in reply["items"]:
            item = dict(it, group=g["id"], name=g["name"], kind=g["kind"], via=g["via"], offset=g["offset"])
            row = Adw.ActionRow(title="%s #%d" % (GLib.markup_escape_text(self._title(g)), it["index"]), activatable=True,
                                subtitle="%s · %s" % (it["base"], fmt_vec(transforms.position(g["kind"], it["v"]), 1)))
            row.item = item
            self.list.append(row)
        return False

    # ------------------------------------------------------------ editor
    def _spin(self, low, high, digits):
        spin = Gtk.SpinButton.new_with_range(low, high, 10 ** -digits)
        spin.set_digits(digits)
        spin.set_hexpand(True)
        spin.connect("value-changed", self._edited)
        return spin

    def _editor(self):
        scroll = Gtk.ScrolledWindow(vexpand=True, hexpand=True)
        page = Adw.PreferencesPage()
        self.info = Adw.PreferencesGroup(title="No object selected", description="Pick an object from the list.")
        page.add(self.info)

        self.transform_group = Adw.PreferencesGroup(title="Transform")
        self.pos = [self._spin(-1e7, 1e7, 3) for _ in range(3)]
        self.rot = [self._spin(-360, 360, 2) for _ in range(3)]
        self.scale = [self._spin(0.001, 1000, 3) for _ in range(3)]
        for title, spins, labels in (("Position", self.pos, "XYZ"), ("Rotation (°)", self.rot, ("Yaw", "Pitch", "Roll")),
                                     ("Scale", self.scale, "XYZ")):
            row = Adw.ActionRow(title=title)
            box = Gtk.Box(spacing=6, valign=Gtk.Align.CENTER)
            for label, spin in zip(labels, spins):
                box.append(Gtk.Label(label=label))
                box.append(spin)
            row.add_suffix(box)
            self.transform_group.add(row)
        actions = Adw.ActionRow(title="Apply")
        buttons = Gtk.Box(spacing=6, valign=Gtk.Align.CENTER)
        self.apply_button = Gtk.Button(label="Apply")
        self.apply_button.add_css_class("suggested-action")
        self.apply_button.connect("clicked", self._apply)
        self.freeze_button = Gtk.ToggleButton(label="Freeze position")
        self.freeze_button.connect("toggled", self._freeze)
        self.bring_button = Gtk.Button(label="Bring to player")
        self.bring_button.connect("clicked", self._bring)
        self.goto_button = Gtk.Button(label="Move player here")
        self.goto_button.connect("clicked", self._goto)
        for b in (self.apply_button, self.freeze_button, self.bring_button, self.goto_button):
            buttons.append(b)
        actions.add_suffix(buttons)
        self.transform_group.add(actions)
        name_row = Adw.ActionRow(title="Type name", subtitle="Shown instead of the vtable name; remembered for this game")
        self.name_entry = Gtk.Entry(placeholder_text="e.g. Character, Horse, Wagon", valign=Gtk.Align.CENTER)
        self.name_entry.connect("activate", self._rename)
        name_button = Gtk.Button(label="Save", valign=Gtk.Align.CENTER)
        name_button.connect("clicked", self._rename)
        name_row.add_suffix(self.name_entry)
        name_row.add_suffix(name_button)
        self.transform_group.add(name_row)
        self.transform_group.set_sensitive(False)
        page.add(self.transform_group)

        self.fields_group = Adw.PreferencesGroup(title="Fields",
                                                 description="The object's memory as 4-byte values. Type a number to "
                                                             "write it: 1.5 writes a float, 42 an integer, 0x… hex.")
        page.add(self.fields_group)
        scroll.set_child(page)
        self.editing = False
        return scroll

    def _select(self, item):
        self.selected = item
        self._unfreeze_all()
        self.freeze_button.set_active(False)
        self.info.set_title(GLib.markup_escape_text(self._title(item)))
        self.info.set_description("object %s · transform %s (%s)%s" % (
            item["base"], item["transform"], item["kind"],
            "" if item.get("via", -1) < 0 else " · held through the pointer at +0x%x" % item["via"]))
        self.name_entry.set_text(self._label(item) or "")
        self.transform_group.set_sensitive(True)
        is_player = self.player is not None and item["base"] == self.player["base"]
        self.bring_button.set_sensitive(self.player is not None and not is_player)
        self.goto_button.set_sensitive(self.player is not None and not is_player)
        self._load_transform(item["v"])
        self._build_fields()

    def _load_transform(self, v):
        if v is None:
            return
        pos, rot, scale = transforms.decode(self.selected["kind"], v)
        self.editing = True
        for spin, value in zip(self.pos + self.rot + self.scale, list(pos) + list(rot) + list(scale)):
            spin.set_value(value)
        self.editing = False
        self.dirty = False

    def _edited(self, _spin):
        if not self.editing:
            self.dirty = True

    def _values(self):
        return ([s.get_value() for s in self.pos], [s.get_value() for s in self.rot], [s.get_value() for s in self.scale])

    def _write_transform(self, item, pos, rot, scale, then=None):
        def write(reply):
            if not reply.get("ok"):
                self.toast(reply.get("error", "read failed"))
                return False
            kind = item["kind"]
            raw = bytes.fromhex(reply["hex"])
            v = list(struct.unpack("<12d" if kind == "qts64" else ("<12f" if kind == "qts" else "<16f"), raw))
            data = transforms.encode(kind, v, pos, rot, scale)
            self.client.ask({"cmd": "mem_write", "addr": item["transform"], "hex": data.hex()},
                            lambda r: (self.toast("Written" if r.get("ok") else r.get("error", "write failed")),
                                       then and then()) and False)
            return False
        self.client.ask({"cmd": "mem_read", "addr": item["transform"], "len": transforms.KIND_BYTES[item["kind"]]}, write)

    def _apply(self, _b):
        if self.selected:
            pos, rot, scale = self._values()
            self._write_transform(self.selected, pos, rot, scale)
            self.dirty = False

    def _bring(self, _b):
        """Puts the selected object two units in front of the player."""
        if not (self.selected and self.player):
            return
        ppos, prot, _ = transforms.decode(self.player["kind"], self.player["v"])
        forward = transforms._axes_from_euler(*prot)[1]
        target = [ppos[i] + 2.0 * forward[i] for i in range(3)]
        _, rot, scale = self._values()
        self._write_transform(self.selected, target, rot, scale)

    def _goto(self, _b):
        if not (self.selected and self.player):
            return
        pos, _, _ = self._values()
        _, prot, pscale = transforms.decode(self.player["kind"], self.player["v"])
        self._write_transform(self.player, [pos[0], pos[1], pos[2] + 1.0], prot, pscale)

    def _freeze(self, button):
        if not self.selected:
            return
        if not button.get_active():
            self._unfreeze_all()
            return
        addrs, fmt = transforms.position_addresses(self.selected["kind"], int(self.selected["transform"], 16))
        pos, _, _ = self._values()
        for addr, value in zip(addrs, pos):
            self.client.ask({"cmd": "mem_freeze", "addr": hex(addr), "type": fmt, "value": repr(float(value))},
                            lambda r: (not r.get("ok") and self.toast(r.get("error", "freeze failed"))) and False)
            self.frozen.append(addr)

    def _unfreeze_all(self):
        for addr in self.frozen:
            self.client.ask({"cmd": "mem_unfreeze", "addr": hex(addr)}, lambda _r: False)
        self.frozen = []

    def _rename(self, *_args):
        if not self.selected:
            return
        types = self.labels.setdefault(self.game, {}).setdefault("types", {})
        text = self.name_entry.get_text().strip()
        if text:
            types[group_key(self.selected)] = text
        else:
            types.pop(group_key(self.selected), None)
        save_labels(self.labels)
        self.info.set_title(GLib.markup_escape_text(self._title(self.selected)))
        self.toast("Saved")
        self._show_list()

    # ------------------------------------------------------------ fields
    def _build_fields(self):
        for row in self.field_rows:
            self.fields_group.remove(row)
        self.field_rows = []
        for offset in range(0, FIELD_BYTES, 4):
            row = Adw.ActionRow(title="+0x%03x" % offset)
            row.add_css_class("property")
            entry = Gtk.Entry(width_chars=12, valign=Gtk.Align.CENTER)
            entry.connect("activate", self._write_field, offset)
            row.add_suffix(entry)
            row.entry = entry
            self.fields_group.add(row)
            self.field_rows.append(row)
        self._read_fields()

    def _read_fields(self):
        if self.selected:
            self.client.ask({"cmd": "mem_read", "addr": self.selected["base"], "len": FIELD_BYTES}, self._fields_done)

    def _fields_done(self, reply):
        if not reply.get("ok") or not self.field_rows:
            return False
        data = bytes.fromhex(reply["hex"])
        for i, row in enumerate(self.field_rows):
            chunk = data[i * 4:i * 4 + 4]
            if len(chunk) < 4:
                break
            f = struct.unpack("<f", chunk)[0]
            n = struct.unpack("<i", chunk)[0]
            parts = ["i32 %d" % n]
            if f == f and 1e-4 < abs(f) < 1e7:
                parts.insert(0, "f32 %.4g" % f)
            if i % 2 == 0 and i * 4 + 8 <= len(data):
                p = struct.unpack("<Q", data[i * 4:i * 4 + 8])[0]
                if 0x10000 < p < 0x800000000000:
                    parts.append("ptr 0x%x" % p)
            row.set_subtitle("  ·  ".join(parts))
        return False

    def _write_field(self, entry, offset):
        text = entry.get_text().strip()
        if not text or not self.selected:
            return
        try:
            if text.lower().startswith("0x"):
                data = struct.pack("<I", int(text, 16) & 0xFFFFFFFF)
            elif any(c in text for c in ".eE"):
                data = struct.pack("<f", float(text))
            else:
                data = struct.pack("<i", int(text))
        except (ValueError, struct.error):
            self.toast("Not a number")
            return
        addr = int(self.selected["base"], 16) + offset
        self.client.ask({"cmd": "mem_write", "addr": hex(addr), "hex": data.hex()},
                        lambda r: (self.toast("Written" if r.get("ok") else r.get("error", "failed")), entry.set_text(""),
                                   self._read_fields()) and False)

    # ------------------------------------------------------------ live refresh
    def _refresh(self):
        if not self.alive:
            return False
        if self.busy:
            return True
        if self.player:
            self.client.ask({"cmd": "mem_read", "addr": self.player["transform"],
                             "len": transforms.KIND_BYTES[self.player["kind"]]}, self._player_live)
        if self.selected:
            focus = self.get_focus()
            if not getattr(self, "dirty", False) and not isinstance(focus, Gtk.Text):
                self.client.ask({"cmd": "mem_read", "addr": self.selected["transform"],
                                 "len": transforms.KIND_BYTES[self.selected["kind"]]}, self._selected_live)
            if not isinstance(focus, Gtk.Text):
                self._read_fields()
        return True

    @staticmethod
    def _unpack(kind, reply):
        if not reply.get("ok"):
            return None
        raw = bytes.fromhex(reply["hex"])
        return list(struct.unpack("<12d" if kind == "qts64" else ("<12f" if kind == "qts" else "<16f"), raw))

    def _player_live(self, reply):
        v = self._unpack(self.player["kind"], reply) if self.player else None
        if v:
            self.player["v"] = v
        return False

    def _selected_live(self, reply):
        if self.selected and not getattr(self, "dirty", False):
            v = self._unpack(self.selected["kind"], reply)
            if v:
                self.selected["v"] = v
                self._load_transform(v)
        return False
