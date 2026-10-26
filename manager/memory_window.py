"""Cheat-Engine-style memory window for a running Beer game (scan, watch list, hex view).

All access goes through the loader's control socket (mem_* commands), which reads and writes through
/proc/self/mem, so a bad address returns an error instead of crashing the game.
"""
import json
import os
import socket
import threading

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Adw, GLib, Gtk  # noqa: E402

TYPES = [
    ("4 Bytes (integer)", "i32"), ("2 Bytes (integer)", "i16"), ("1 Byte (integer)", "i8"), ("8 Bytes (integer)", "i64"),
    ("Float", "f32"), ("Double", "f64"), ("Text (ASCII)", "str"), ("Array of bytes (hex)", "bytes"),
]
CONDITIONS = [("Anything", ""), ("Less than", "lt"), ("Less than or equal to", "le"), ("Greater than", "gt"),
              ("Greater than or equal to", "ge"), ("Equal to", "eq")]
NEXT_MODES = [("Exact value", "exact"), ("Changed value", "changed"), ("Unchanged value", "unchanged"),
              ("Increased value", "increased"), ("Decreased value", "decreased")]


class MemClient:
    """Own socket with a long timeout (scans take seconds), requests run off the UI thread."""

    def __init__(self, path):
        self.path = path
        self.lock = threading.Lock()
        self.sock = None
        self.buffer = b""

    def _request(self, message):
        with self.lock:
            for attempt in (0, 1):
                try:
                    if self.sock is None:
                        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                        s.settimeout(120)
                        s.connect(self.path)
                        self.sock, self.buffer = s, b""
                    self.sock.sendall((json.dumps(message) + "\n").encode())
                    while b"\n" not in self.buffer:
                        chunk = self.sock.recv(1 << 20)
                        if not chunk:
                            raise OSError("closed")
                        self.buffer += chunk
                    line, self.buffer = self.buffer.split(b"\n", 1)
                    return json.loads(line)
                except (OSError, ValueError):
                    try:
                        if self.sock:
                            self.sock.close()
                    except OSError:
                        pass
                    self.sock = None
                    if attempt:
                        return {"ok": False, "error": "game not responding"}

    def ask(self, message, callback):
        def work():
            reply = self._request(message)
            GLib.idle_add(callback, reply)
        threading.Thread(target=work, daemon=True).start()


def mono(text):
    label = Gtk.Label(label=text, xalign=0, selectable=True)
    label.add_css_class("monospace")
    return label


class MemoryWindow(Adw.Window):
    def __init__(self, app, path, name, pid=None):
        super().__init__(title="Memory — %s" % name, default_width=820, default_height=700)
        self.set_transient_for(app.window)
        self.client = MemClient(path)
        self.pid = pid
        self.type_code = "i32"
        self.watch = []          # dicts: addr, type, label
        self.watch_rows = {}
        self.busy = False
        self.alive = True
        self.connect("close-request", self._on_close)

        view = Adw.ToolbarView()
        header = Adw.HeaderBar()
        self.stack = Adw.ViewStack()
        header.set_title_widget(Adw.ViewSwitcher(stack=self.stack, policy=Adw.ViewSwitcherPolicy.WIDE))
        view.add_top_bar(header)
        self.toasts = Adw.ToastOverlay()
        self.stack.add_titled(self._scan_page(), "scan", "Scan")
        self.stack.add_titled(self._watch_page(), "watch", "Watch list")
        self.stack.add_titled(self._pinned_page(), "pinned", "Pinned")
        self.stack.add_titled(self._hex_page(), "hex", "Hex view")
        self.toasts.set_child(self.stack)
        view.set_content(self.toasts)
        self.set_content(view)
        GLib.timeout_add(1500, self._refresh)
        GLib.timeout_add(700, self._watch_game)

    def _watch_game(self):
        """The window belongs to one game session: close it as soon as that game is gone."""
        if not self.alive:
            return False
        gone = False
        if self.pid:
            try:
                os.kill(self.pid, 0)
            except ProcessLookupError:
                gone = True
            except PermissionError:
                pass
        if not gone and not os.path.exists(self.client.path):
            gone = True
        if gone:
            self.alive = False
            self.close()
            return False
        return True

    def _on_close(self, *_args):
        self.alive = False
        return False

    def toast(self, text):
        self.toasts.add_toast(Adw.Toast.new(text))

    # ------------------------------------------------------------ scan page
    def _scan_page(self):
        page = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12, margin_top=12, margin_bottom=12, margin_start=12, margin_end=12)
        group = Adw.PreferencesGroup(description="Find a value (health, ammo, money…) by scanning, change it in the game, then use Next scan to narrow it down.")
        self.type_row = Adw.ComboRow(title="Value type")
        self.type_row.set_model(Gtk.StringList.new([t[0] for t in TYPES]))
        self.value_row = Adw.EntryRow(title="Value")
        self.unknown_row = Adw.SwitchRow(title="Unknown initial value",
                                         subtitle="Snapshot all memory now, then filter with Changed / Unchanged / Increased / Decreased (numeric types)")
        self.unknown_row.connect("notify::active", lambda r, _p: self.value_row.set_sensitive(not r.get_active()))
        self.writable_row = Adw.SwitchRow(title="Writable memory only", subtitle="Much faster; code and read-only data are skipped")
        self.writable_row.set_active(True)
        self.cond_row = Adw.ComboRow(title="Only keep values that are", subtitle="Applies to every scan (numeric types) — e.g. “Greater or equal 0” hides negative numbers")
        self.cond_row.set_model(Gtk.StringList.new([c[0] for c in CONDITIONS]))
        self.cond_value_row = Adw.EntryRow(title="…than / to this value")
        self.cond_value_row.set_visible(False)
        self.cond_row.connect("notify::selected", lambda r, _p: self.cond_value_row.set_visible(r.get_selected() > 0))
        self.mode_row = Adw.ComboRow(title="Next scan")
        self.mode_row.set_model(Gtk.StringList.new([m[0] for m in NEXT_MODES]))
        self.atleast_row = Adw.EntryRow(title="Change at least (optional) — Changed / Increased / Decreased only")
        self.upto_row = Adw.EntryRow(title="Change up to (optional) — Changed / Increased / Decreased only")
        for row in (self.type_row, self.value_row, self.unknown_row, self.writable_row, self.cond_row, self.cond_value_row, self.mode_row, self.atleast_row, self.upto_row):
            group.add(row)
        page.append(group)

        buttons = Gtk.Box(spacing=8, halign=Gtk.Align.END)
        self.first_button = Gtk.Button(label="First scan")
        self.first_button.add_css_class("suggested-action")
        self.first_button.connect("clicked", self._first_scan)
        self.next_button = Gtk.Button(label="Next scan", sensitive=False)
        self.next_button.connect("clicked", self._next_scan)
        self.reset_button = Gtk.Button(label="Reset", sensitive=False)
        self.reset_button.connect("clicked", self._reset)
        for b in (self.first_button, self.next_button, self.reset_button):
            buttons.append(b)
        page.append(buttons)

        apply_group = Adw.PreferencesGroup()
        self.apply_row = Adw.EntryRow(title="Apply to all results — write this value to every address in the list")
        self.apply_button = Gtk.Button(label="Set all", valign=Gtk.Align.CENTER, sensitive=False)
        self.apply_button.add_css_class("destructive-action")
        self.apply_button.connect("clicked", self._apply_all)
        self.apply_row.add_suffix(self.apply_button)
        apply_group.add(self.apply_row)
        page.append(apply_group)
        self.last_count = 0

        self.status = Gtk.Label(label="No scan yet", xalign=0)
        self.status.add_css_class("dim-label")
        page.append(self.status)
        self.coverage = Gtk.Label(label="", xalign=0, wrap=True)
        self.coverage.add_css_class("dim-label")
        self.coverage.add_css_class("caption")
        page.append(self.coverage)
        self.results = Gtk.ListBox(selection_mode=Gtk.SelectionMode.NONE)
        self.results.add_css_class("boxed-list")
        scroll = Gtk.ScrolledWindow(vexpand=True)
        scroll.set_child(self.results)
        page.append(scroll)
        return page

    def _add_condition(self, message):
        """Adds the optional value condition; returns False (after a toast) when it is not a number."""
        op = CONDITIONS[self.cond_row.get_selected()][1]
        if not op:
            return True
        text = self.cond_value_row.get_text().strip()
        try:
            float(text)
        except ValueError:
            self.toast("Enter a number for the value condition")
            return False
        message["cond"] = op
        message["cond_value"] = text
        return True

    def _selected_type(self):
        return TYPES[self.type_row.get_selected()][1]

    def _set_busy(self, busy, text=None):
        self.busy = busy
        self.first_button.set_sensitive(not busy)
        if text:
            self.status.set_label(text)

    def _first_scan(self, _b):
        value = self.value_row.get_text().strip()
        unknown = bool(self.unknown_row.get_active())
        if not value and not unknown:
            self.toast("Enter a value to search for, or turn on Unknown initial value")
            return
        self.type_code = self._selected_type()
        if unknown and self.type_code in ("str", "bytes"):
            self.toast("Unknown-value scans need a numeric type")
            return
        self._set_busy(True, "Taking a snapshot of memory…" if unknown else "Scanning memory…")
        message = {"cmd": "mem_scan", "type": self.type_code, "value": value, "writable": bool(self.writable_row.get_active())}
        if unknown:
            message["unknown"] = True
        if not self._add_condition(message):
            self._set_busy(False)
            return
        self.client.ask(message, self._scan_done)

    def _next_scan(self, _b):
        mode = NEXT_MODES[self.mode_row.get_selected()][1]
        value = self.value_row.get_text().strip()
        self._set_busy(True, "Filtering…")
        message = {"cmd": "mem_next", "mode": mode, "value": value}
        if mode in ("changed", "increased", "decreased"):
            for key, row in (("at_least", self.atleast_row), ("up_to", self.upto_row)):
                text = row.get_text().strip()
                if not text:
                    continue
                try:
                    if float(text) < 0:
                        raise ValueError
                except ValueError:
                    self._set_busy(False)
                    self.toast("The change limits must be numbers of 0 or more")
                    return
                message[key] = text
        if not self._add_condition(message):
            self._set_busy(False)
            return
        self.client.ask(message, self._scan_done)

    def _reset(self, _b):
        self.client.ask({"cmd": "mem_reset"}, lambda _r: None)
        self._show_results({"results": [], "count": 0})
        self.next_button.set_sensitive(False)
        self.reset_button.set_sensitive(False)
        self.apply_button.set_sensitive(False)
        self.last_count = 0
        self.status.set_label("No scan yet")
        self.coverage.set_label("")

    def _scan_done(self, reply):
        self._set_busy(False)
        if not reply.get("ok"):
            self.status.set_label(reply.get("error", "scan failed"))
            return False
        self._show_results(reply)
        cov = reply.get("coverage")
        if cov:
            parts = ["Scanned %.1f GB" % (cov["scanned_mb"] / 1024)]
            skipped = []
            if cov["readonly_mb"]:
                skipped.append("%d MB read-only (turn off “Writable memory only” to include it)" % cov["readonly_mb"])
            if cov["device_mb"]:
                skipped.append("%d MB device / GPU mappings" % cov["device_mb"])
            if cov["huge_mb"]:
                skipped.append("%d MB in regions over 24 GB" % cov["huge_mb"])
            if cov["limit_mb"]:
                skipped.append("%d MB over the snapshot limit or disk space" % cov["limit_mb"])
            self.coverage.set_label(parts[0] + (" · skipped: " + ", ".join(skipped) if skipped else " · nothing skipped"))
        self.next_button.set_sensitive(True)
        self.reset_button.set_sensitive(True)
        count = reply["count"]
        shown = len(reply["results"])
        self.last_count = count
        self.apply_button.set_sensitive(count > 0 and not reply.get("snapshot"))
        if reply.get("snapshot"):
            self.status.set_label("%s candidates in the snapshot%s — change the value in the game, then pick Changed/Increased/… and press Next scan" % (
                format(count, ","), " (memory limit reached, some regions skipped)" if reply.get("truncated") else ""))
            return False
        self.status.set_label("%d result%s%s" % (count, "" if count == 1 else "s",
                                                 " (showing %d — narrow it down with Next scan)" % shown if count > shown else ""))
        return False

    def _apply_all(self, _b):
        value = self.apply_row.get_text().strip()
        if not value:
            self.toast("Enter the value to write")
            return
        dialog = Adw.AlertDialog(heading="Write %s to %s address%s?" % (value, format(self.last_count, ","), "" if self.last_count == 1 else "es"),
                                 body="This changes the game's memory right now. Writing to the wrong addresses can crash the game.")
        dialog.add_response("cancel", "Cancel")
        dialog.add_response("write", "Write to all")
        dialog.set_response_appearance("write", Adw.ResponseAppearance.DESTRUCTIVE)

        def done(_d, response):
            if response == "write":
                self.client.ask({"cmd": "mem_set_all", "value": value}, self._apply_done)
        dialog.connect("response", done)
        dialog.present(self)

    def _apply_done(self, reply):
        if reply.get("ok"):
            msg = "Wrote %d address%s" % (reply["written"], "" if reply["written"] == 1 else "es")
            if reply.get("failed"):
                msg += " (%d no longer writable)" % reply["failed"]
            self.toast(msg)
        else:
            self.toast(reply.get("error", "write failed"))
        return False

    def _remove_result(self, addr, row):
        def done(reply):
            if reply.get("ok"):
                self.results.remove(row)
                self.result_rows.pop(addr, None)
                self.last_count = reply["count"]
                self.status.set_label("%d result%s" % (reply["count"], "" if reply["count"] == 1 else "s"))
            else:
                self.toast(reply.get("error", "remove failed"))
            return False
        self.client.ask({"cmd": "mem_remove", "addrs": [addr]}, done)

    def _show_results(self, reply):
        child = self.results.get_first_child()
        while child:
            nxt = child.get_next_sibling()
            self.results.remove(child)
            child = nxt
        self.result_rows = {}
        for item in reply["results"][:200]:
            self.results.append(self._result_row(item))

    def _result_row(self, item):
        addr = item["addr"]
        row = Adw.ActionRow(title=addr, subtitle=item["value"])
        row.add_css_class("monospace")
        entry = Gtk.Entry(width_chars=12, valign=Gtk.Align.CENTER, placeholder_text="new value")
        set_button = Gtk.Button(label="Set", valign=Gtk.Align.CENTER)
        set_button.connect("clicked", lambda _b: self._set_value(addr, self.type_code, entry.get_text().strip()))
        watch = Gtk.Button(icon_name="list-add-symbolic", valign=Gtk.Align.CENTER, tooltip_text="Add to watch list")
        watch.add_css_class("flat")
        watch.connect("clicked", lambda _b: self.add_watch(addr, self.type_code))
        pin = Gtk.Button(icon_name="view-pin-symbolic", valign=Gtk.Align.CENTER, tooltip_text="Pin to the in-game overlay")
        pin.add_css_class("flat")
        pin.connect("clicked", lambda _b: self._pin(addr, self.type_code, addr))
        remove = Gtk.Button(icon_name="list-remove-symbolic", valign=Gtk.Align.CENTER, tooltip_text="Remove from this list")
        remove.add_css_class("flat")
        remove.connect("clicked", lambda _b: self._remove_result(addr, row))
        for w in (entry, set_button, watch, pin, remove):
            row.add_suffix(w)
        self.result_rows[addr] = row
        return row

    def _set_value(self, addr, type_code, value):
        if not value:
            return
        self.client.ask({"cmd": "mem_set", "addr": addr, "type": type_code, "value": value},
                        lambda r: self.toast("Written" if r.get("ok") else r.get("error", "failed")))

    # ------------------------------------------------------------ watch page
    def _watch_page(self):
        page = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12, margin_top=12, margin_bottom=12, margin_start=12, margin_end=12)
        add = Gtk.Box(spacing=8)
        self.add_addr = Gtk.Entry(placeholder_text="Address, e.g. 0x7f12a0c4", hexpand=True)
        self.add_type = Gtk.DropDown.new_from_strings([t[0] for t in TYPES])
        add_button = Gtk.Button(label="Add address")
        add_button.connect("clicked", self._add_manual)
        add.append(self.add_addr)
        add.append(self.add_type)
        add.append(add_button)
        page.append(add)
        note = Gtk.Label(label="Freeze rewrites the value every few milliseconds, like Cheat Engine's lock. Addresses change between runs.",
                         xalign=0, wrap=True)
        note.add_css_class("dim-label")
        page.append(note)
        self.watch_list = Gtk.ListBox(selection_mode=Gtk.SelectionMode.NONE)
        self.watch_list.add_css_class("boxed-list")
        scroll = Gtk.ScrolledWindow(vexpand=True)
        scroll.set_child(self.watch_list)
        page.append(scroll)
        return page

    def _add_manual(self, _b):
        text = self.add_addr.get_text().strip()
        try:
            int(text, 0)
        except ValueError:
            self.toast("Not a valid address")
            return
        self.add_watch(text, TYPES[self.add_type.get_selected()][1])

    def add_watch(self, addr, type_code):
        if any(w["addr"] == addr and w["type"] == type_code for w in self.watch):
            return
        entry = {"addr": addr, "type": type_code}
        self.watch.append(entry)
        row = Adw.ActionRow(title=addr, subtitle="…")
        row.add_css_class("monospace")
        label = Gtk.Label(label=type_code, valign=Gtk.Align.CENTER)
        label.add_css_class("dim-label")
        value = Gtk.Entry(width_chars=12, valign=Gtk.Align.CENTER, placeholder_text="new value")
        set_button = Gtk.Button(label="Set", valign=Gtk.Align.CENTER)
        set_button.connect("clicked", lambda _b: self._set_value(addr, type_code, value.get_text().strip()))
        label_entry = Gtk.Entry(width_chars=10, valign=Gtk.Align.CENTER, placeholder_text="overlay label")
        pin = Gtk.ToggleButton(icon_name="view-pin-symbolic", valign=Gtk.Align.CENTER, tooltip_text="Pin to the in-game overlay")
        pin.connect("toggled", lambda b: self._toggle_pin(b, addr, type_code, label_entry))
        freeze = Gtk.ToggleButton(label="Freeze", valign=Gtk.Align.CENTER)
        freeze.connect("toggled", lambda b: self._toggle_freeze(b, addr, type_code, value))
        remove = Gtk.Button(icon_name="user-trash-symbolic", valign=Gtk.Align.CENTER, tooltip_text="Remove")
        remove.add_css_class("flat")
        remove.connect("clicked", lambda _b: self._remove_watch(entry, row, freeze, pin))
        for w in (label, label_entry, pin, value, set_button, freeze, remove):
            row.add_suffix(w)
        self.watch_list.append(row)
        self.watch_rows[(addr, type_code)] = row
        self.toast("Added %s" % addr)

    def _pin(self, addr, type_code, label):
        self.client.ask({"cmd": "overlay_pin", "addr": addr, "type": type_code, "label": label or addr},
                        lambda r: self.toast("Pinned to the overlay" if r.get("ok") else r.get("error", "pin failed")))

    def _toggle_pin(self, button, addr, type_code, label_entry):
        if button.get_active():
            if type_code in ("str", "bytes"):
                button.set_active(False)
                self.toast("Only numeric values can be pinned")
                return
            self._pin(addr, type_code, label_entry.get_text().strip())
        else:
            self.client.ask({"cmd": "overlay_unpin", "addr": addr}, lambda r: None)

    def _toggle_freeze(self, button, addr, type_code, entry):
        if button.get_active():
            value = entry.get_text().strip()
            if not value:
                self.client.ask({"cmd": "mem_values", "type": type_code, "addrs": [addr]},
                                lambda r, a=addr, t=type_code: self._freeze_current(r, a, t, button))
                return
            self.client.ask({"cmd": "mem_freeze", "addr": addr, "type": type_code, "value": value}, lambda r: self._freeze_reply(r, button))
        else:
            self.client.ask({"cmd": "mem_unfreeze", "addr": addr}, lambda r: None)

    def _freeze_current(self, reply, addr, type_code, button):
        values = reply.get("values") or []
        if not values or values[0]["value"] == "??":
            button.set_active(False)
            self.toast("Cannot read that address")
            return False
        self.client.ask({"cmd": "mem_freeze", "addr": addr, "type": type_code, "value": values[0]["value"]}, lambda r: self._freeze_reply(r, button))
        return False

    def _freeze_reply(self, reply, button):
        if not reply.get("ok"):
            button.set_active(False)
            self.toast(reply.get("error", "freeze failed"))
        return False

    def _remove_watch(self, entry, row, freeze, pin):
        if freeze.get_active():
            freeze.set_active(False)
        if pin.get_active():
            pin.set_active(False)
        self.watch.remove(entry)
        self.watch_rows.pop((entry["addr"], entry["type"]), None)
        self.watch_list.remove(row)

    # ------------------------------------------------------------ pinned page
    def _pinned_page(self):
        page = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12, margin_top=12, margin_bottom=12, margin_start=12, margin_end=12)
        note = Gtk.Label(label="Everything shown in the overlay's Pinned memory box. Rename a pin or remove it here; pins last until the game closes.",
                         xalign=0, wrap=True)
        note.add_css_class("dim-label")
        page.append(note)
        self.pinned_list = Gtk.ListBox(selection_mode=Gtk.SelectionMode.NONE)
        self.pinned_list.add_css_class("boxed-list")
        self.pinned_empty = Adw.StatusPage(icon_name="view-pin-symbolic", title="Nothing pinned",
                                           description="Use the pin button on a scan result or a watch-list row")
        scroll = Gtk.ScrolledWindow(vexpand=True)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        box.append(self.pinned_empty)
        box.append(self.pinned_list)
        scroll.set_child(box)
        page.append(scroll)
        self.pinned_rows = {}      # addr -> row
        self.pinned_shown = ()
        self.stack.connect("notify::visible-child-name", lambda *_a: self._refresh_pinned())
        return page

    def _refresh_pinned(self):
        if self.stack.get_visible_child_name() != "pinned":
            return
        self.client.ask({"cmd": "overlay_pins"}, self._pinned_reply)

    def _pinned_reply(self, reply):
        if not reply.get("ok"):
            return False
        pins = reply["pins"]
        signature = tuple((p["addr"], p["type"], p["label"]) for p in pins)
        if signature != self.pinned_shown:
            self.pinned_shown = signature
            child = self.pinned_list.get_first_child()
            while child:
                nxt = child.get_next_sibling()
                self.pinned_list.remove(child)
                child = nxt
            self.pinned_rows = {}
            for pin in pins:
                self.pinned_list.append(self._pin_row(pin))
        self.pinned_empty.set_visible(not pins)
        self.pinned_list.set_visible(bool(pins))
        for type_code in {p["type"] for p in pins}:
            addrs = [p["addr"] for p in pins if p["type"] == type_code]
            self.client.ask({"cmd": "mem_values", "type": type_code, "addrs": addrs}, self._pinned_values)
        return False

    def _pinned_values(self, reply):
        if reply.get("ok"):
            for item in reply["values"]:
                row = self.pinned_rows.get(item["addr"])
                if row:
                    row.set_subtitle("%s  ·  %s" % (item["addr"], item["value"]))
        return False

    def _pin_row(self, pin):
        addr, type_code = pin["addr"], pin["type"]
        row = Adw.ActionRow(title=GLib.markup_escape_text(pin["label"]), subtitle=addr)
        entry = Gtk.Entry(text=pin["label"], width_chars=16, valign=Gtk.Align.CENTER, placeholder_text="name")
        rename = Gtk.Button(label="Rename", valign=Gtk.Align.CENTER)

        def do_rename(_w=None):
            label = entry.get_text().strip()
            if label:
                self.client.ask({"cmd": "overlay_pin", "addr": addr, "type": type_code, "label": label},
                                lambda r: (self._refresh_pinned(), False)[1])
        rename.connect("clicked", do_rename)
        entry.connect("activate", do_rename)
        remove = Gtk.Button(icon_name="user-trash-symbolic", valign=Gtk.Align.CENTER, tooltip_text="Remove from the overlay")
        remove.add_css_class("flat")
        remove.connect("clicked", lambda _b: self.client.ask({"cmd": "overlay_unpin", "addr": addr},
                                                             lambda r: (self._refresh_pinned(), False)[1]))
        for w in (entry, rename, remove):
            row.add_suffix(w)
        self.pinned_rows[addr] = row
        return row

    # ------------------------------------------------------------ hex page
    def _hex_page(self):
        page = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12, margin_top=12, margin_bottom=12, margin_start=12, margin_end=12)
        top = Gtk.Box(spacing=8)
        self.hex_addr = Gtk.Entry(placeholder_text="Address, e.g. 0x140000000", hexpand=True)
        self.hex_addr.connect("activate", self._hex_read)
        self.hex_len = Gtk.DropDown.new_from_strings(["64 bytes", "256 bytes", "1 KB", "4 KB"])
        self.hex_len.set_selected(1)
        read = Gtk.Button(label="Read")
        read.connect("clicked", self._hex_read)
        top.append(self.hex_addr)
        top.append(self.hex_len)
        top.append(read)
        page.append(top)
        self.hex_text = Gtk.TextView(editable=False, monospace=True, cursor_visible=False)
        scroll = Gtk.ScrolledWindow(vexpand=True)
        scroll.set_child(self.hex_text)
        page.append(scroll)
        write = Gtk.Box(spacing=8)
        self.hex_write = Gtk.Entry(placeholder_text="Bytes to write at the address, e.g. 90 90 90", hexpand=True)
        write_button = Gtk.Button(label="Write")
        write_button.add_css_class("destructive-action")
        write_button.connect("clicked", self._hex_write)
        write.append(self.hex_write)
        write.append(write_button)
        page.append(write)
        self.hex_current = None
        return page

    def _hex_length(self):
        return [64, 256, 1024, 4096][self.hex_len.get_selected()]

    def _hex_read(self, *_args):
        text = self.hex_addr.get_text().strip()
        try:
            addr = int(text, 0)
        except ValueError:
            self.toast("Not a valid address")
            return
        self.hex_current = addr
        self.client.ask({"cmd": "mem_read", "addr": hex(addr), "len": self._hex_length()}, self._hex_show)

    def _hex_show(self, reply):
        buffer = self.hex_text.get_buffer()
        if not reply.get("ok"):
            buffer.set_text(reply.get("error", "read failed"))
            return False
        data = bytes.fromhex(reply["hex"])
        base = int(reply["addr"], 16)
        lines = []
        for offset in range(0, len(data), 16):
            chunk = data[offset:offset + 16]
            hexpart = " ".join("%02x" % b for b in chunk).ljust(47)
            ascii_part = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
            lines.append("%016x  %s  %s" % (base + offset, hexpart, ascii_part))
        buffer.set_text("\n".join(lines))
        return False

    def _hex_write(self, _b):
        if self.hex_current is None:
            self.toast("Read an address first")
            return
        data = self.hex_write.get_text().strip()
        if not data:
            return
        self.client.ask({"cmd": "mem_write", "addr": hex(self.hex_current), "hex": data.replace(" ", "")},
                        lambda r: (self.toast("Written" if r.get("ok") else r.get("error", "failed")), self._hex_read()) and False)

    # ------------------------------------------------------------ periodic refresh
    def _refresh(self):
        if not self.alive:
            return False
        page = self.stack.get_visible_child_name()
        if page == "scan" and getattr(self, "result_rows", None) and not self.busy:
            self.client.ask({"cmd": "mem_results", "offset": 0, "limit": 200}, self._refresh_results)
        elif page == "watch" and self.watch:
            for type_code in {w["type"] for w in self.watch}:
                addrs = [w["addr"] for w in self.watch if w["type"] == type_code]
                self.client.ask({"cmd": "mem_values", "type": type_code, "addrs": addrs},
                                lambda r, t=type_code: self._refresh_watch(r, t))
        elif page == "pinned":
            self._refresh_pinned()
        elif page == "hex" and self.hex_current is not None and not self.hex_write.has_focus():
            self.client.ask({"cmd": "mem_read", "addr": hex(self.hex_current), "len": self._hex_length()}, self._hex_show)
        return True

    def _refresh_results(self, reply):
        if reply.get("ok"):
            for item in reply["results"]:
                row = self.result_rows.get(item["addr"])
                if row:
                    row.set_subtitle(item["value"])
        return False

    def _refresh_watch(self, reply, type_code):
        if reply.get("ok"):
            for item in reply["values"]:
                row = self.watch_rows.get((item["addr"], type_code))
                if row is None:   # the loader prints addresses normalised; match by numeric value
                    for (a, t), r in self.watch_rows.items():
                        if t == type_code and a.lower().startswith("0x") and int(a, 16) == int(item["addr"], 16):
                            row = r
                            break
                if row:
                    row.set_subtitle(item["value"])
        return False
