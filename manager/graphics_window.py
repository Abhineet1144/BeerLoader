"""Graphics window: find and tweak the numbers a game feeds its shaders (light intensity, fog, exposure…).

How it works, in short: every draw reads a few "constant buffers", small tables of numbers (sun colour, camera,
fog density, exposure…) that the game rewrites every frame. Beer can watch them, scan them the way the memory
window scans values, and overwrite single numbers right before the GPU reads them. Saved tweaks are stored per
game in ~/.beer/tweaks and re-applied at every launch (shader hashes do not change between runs).
"""
import json
import os
import re

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Adw, GLib, Gtk  # noqa: E402

import storage  # noqa: E402
from memory_window import MemClient  # noqa: E402

TWEAK_DIR = storage.path("tweaks")

RECIPES = [
    ("Flashlight or lamp strength",
     "1. Press “Start watching”, then “Take a snapshot”.\n"
     "2. Turn the flashlight ON in the game, then press “It went up”.\n"
     "3. Turn it OFF and press “It went down”.\n"
     "4. Repeat steps 2 and 3. Now and then stand still and press “Stayed the same”.\n"
     "5. When few results remain, open the Candidates tab and try them."),
    ("Game brightness or exposure",
     "1. Press “Start watching”, then “Take a snapshot”.\n"
     "2. Raise the game's own brightness (or exposure) setting, then press “It went up”.\n"
     "3. Lower it and press “It went down”. Repeat a couple of times.\n"
     "4. Leave it alone for a few seconds and press “Stayed the same”.\n"
     "5. Open the Candidates tab and try what is left."),
    ("Sun, time of day or sky colour",
     "1. Press “Start watching”, then “Take a snapshot”.\n"
     "2. Let the time of day move (or step through it), then press “It changed”.\n"
     "3. Use “Pause game time” in the manager so the clock stops, wait a few seconds and press “Stayed the same”.\n"
     "4. Unpause, wait, press “It changed”. Repeat until few results remain.\n"
     "5. Open the Candidates tab and try them."),
    ("Fog or mist density",
     "1. Press “Start watching”, then “Take a snapshot”.\n"
     "2. Walk from clear air into fog (or the other way round) and press “It went up” or “It went down”.\n"
     "3. Stand still and press “Stayed the same”. Repeat.\n"
     "4. Open the Candidates tab and try what is left."),
    ("Something else (general method)",
     "1. Press “Start watching”, then “Take a snapshot”.\n"
     "2. Change the thing you care about in the game and press “It changed” (or “went up”/“went down” if you know).\n"
     "3. Change nothing and press “Stayed the same”.\n"
     "4. Repeat until a few dozen results remain, then try them in the Candidates tab."),
]

STAGE_LABEL = {"vertex": "Vertex shader", "pixel": "Pixel shader", "compute": "Compute shader", "geometry": "Geometry shader",
               "hull": "Hull shader", "domain": "Domain shader"}


def tweak_file(exe):
    stem = re.sub(r"[^A-Za-z0-9._-]", "_", os.path.splitext(os.path.basename(exe))[0].lower()) or "game"
    return os.path.join(TWEAK_DIR, stem + ".json")


def load_tweaks(exe):
    try:
        with open(tweak_file(exe)) as f:
            return json.load(f).get("tweaks", [])
    except (OSError, ValueError):
        return []


def save_tweaks(exe, tweaks):
    os.makedirs(TWEAK_DIR, exist_ok=True)
    path = tweak_file(exe)
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump({"exe": exe, "tweaks": tweaks}, f, indent=2)
    os.replace(tmp, path)


def apply_tweak(client, tweak):
    """Sends one saved tweak to the loader; returns the loader's reply (with the tweak id)."""
    message = {"cmd": "gfx_tweak_set", "pso": tweak["pso"], "stage": tweak["stage"], "reg": tweak["reg"], "space": tweak["space"],
               "size": tweak["size"], "offset": tweak["offset"], "mode": tweak["mode"], "value": tweak["value"], "label": tweak["label"]}
    reply = client.request(message) if hasattr(client, "request") else client._request(message)
    if reply and reply.get("ok") and not tweak.get("enabled", True):
        (client.request if hasattr(client, "request") else client._request)({"cmd": "gfx_tweak_enable", "tid": reply["tid"], "on": False})
    return reply


def apply_saved(client, exe):
    """Called when a game starts: re-creates the saved tweaks inside the loader."""
    for tweak in load_tweaks(exe):
        if tweak.get("enabled", True):
            apply_tweak(client, tweak)


def short(h):
    return h[:8]


def describe(item):
    return "%s · %s · slot b%d · byte %d" % (STAGE_LABEL.get(item["stage"], item["stage"]), short(item["pso"]), item["reg"], item["offset"])


class GraphicsWindow(Adw.Window):
    def __init__(self, app, path, name, pid, exe):
        super().__init__(title="Graphics — %s" % name, default_width=860, default_height=720)
        self.set_transient_for(app.window)
        self.app, self.name, self.pid, self.exe = app, name, pid, exe
        self.client = MemClient(path)
        self.alive = True
        self.watching = False
        self.temp = {}               # (id, offset) -> tid of a test tweak
        self.result_rows = {}
        self.saved = load_tweaks(exe)
        self.saved_tids = {}         # index -> tid in the loader
        self.connect("close-request", self._on_close)

        view = Adw.ToolbarView()
        header = Adw.HeaderBar()
        self.stack = Adw.ViewStack()
        header.set_title_widget(Adw.ViewSwitcher(stack=self.stack, policy=Adw.ViewSwitcherPolicy.WIDE))
        view.add_top_bar(header)
        self.toasts = Adw.ToastOverlay()
        self.stack.add_titled(self._start_page(), "start", "Start here")
        self.stack.add_titled(self._candidates_page(), "candidates", "Candidates")
        self.stack.add_titled(self._tweaks_page(), "tweaks", "My tweaks")
        self.stack.add_titled(self._browse_page(), "browse", "Browse")
        self.toasts.set_child(self.stack)
        view.set_content(self.toasts)
        self.set_content(view)
        GLib.timeout_add(1500, self._refresh)
        GLib.timeout_add(700, self._watch_game)
        self._rebuild_tweaks()

    # ---------------------------------------------------------------- lifetime
    def _on_close(self, *_args):
        self.alive = False
        # Stop watching (it costs a little per draw) and undo unsaved test tweaks; saved tweaks stay applied.
        for tid in list(self.temp.values()):
            self.client.ask({"cmd": "gfx_tweak_remove", "tid": tid}, lambda _r: False)
        self.client.ask({"cmd": "gfx_capture", "on": False}, lambda _r: False)
        return False

    def _watch_game(self):
        if not self.alive:
            return False
        try:
            os.kill(self.pid, 0)
        except ProcessLookupError:
            self.alive = False
            self.close()
            return False
        except PermissionError:
            pass
        return True

    def toast(self, text):
        self.toasts.add_toast(Adw.Toast.new(text))

    # ---------------------------------------------------------------- start page
    def _start_page(self):
        page = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=14, margin_top=14, margin_bottom=14, margin_start=14, margin_end=14)
        scroll = Gtk.ScrolledWindow(vexpand=True)
        scroll.set_child(page)

        intro = Adw.PreferencesGroup(title="What is this?")
        text = Gtk.Label(wrap=True, xalign=0, selectable=False,
                         label="Every time the game draws something, it hands the graphics card a small table of numbers: how bright the sun is, "
                               "how thick the fog is, the exposure, the camera… The game rewrites those tables every frame.\n\n"
                               "This window lets you find one of those numbers the same way you find a health value in the Memory window: "
                               "snapshot them all, change something in the game, and keep only the numbers that reacted. "
                               "Then you can turn a number up or down and watch the picture change, and save it as a tweak that is applied "
                               "every time you launch this game.")
        intro.add(text)
        page.append(intro)

        recipe_group = Adw.PreferencesGroup(title="What do you want to change?")
        self.recipe = Adw.ComboRow(title="Recipe", subtitle="Pick one and follow the steps below",
                                   model=Gtk.StringList.new([r[0] for r in RECIPES]))
        self.recipe.connect("notify::selected", self._recipe_changed)
        recipe_group.add(self.recipe)
        self.recipe_text = Gtk.Label(wrap=True, xalign=0, margin_top=8, label=RECIPES[0][1])
        recipe_group.add(self.recipe_text)
        page.append(recipe_group)

        steps = Adw.PreferencesGroup(title="Find it")
        row = Adw.ActionRow(title="1. Start watching", subtitle="Beer starts recording the numbers each frame (slightly slower while this window is open)")
        self.watch_button = Gtk.Button(label="Start watching", valign=Gtk.Align.CENTER)
        self.watch_button.add_css_class("suggested-action")
        self.watch_button.connect("clicked", self._start_watching)
        row.add_suffix(self.watch_button)
        steps.add(row)
        row = Adw.ActionRow(title="2. Take a snapshot", subtitle="Remember every number as it is right now")
        self.snapshot_button = Gtk.Button(label="Take a snapshot", valign=Gtk.Align.CENTER, sensitive=False)
        self.snapshot_button.connect("clicked", self._snapshot)
        row.add_suffix(self.snapshot_button)
        steps.add(row)
        row = Adw.ActionRow(title="3. Then, after doing something in the game…", subtitle="Keep only the numbers that did this since the last press")
        box = Gtk.Box(spacing=6, valign=Gtk.Align.CENTER)
        self.filter_buttons = []
        for label, mode in (("It changed", "changed"), ("It went up", "increased"), ("It went down", "decreased"), ("Stayed the same", "unchanged")):
            b = Gtk.Button(label=label, sensitive=False)
            b.connect("clicked", lambda _b, m=mode: self._filter(m))
            box.append(b)
            self.filter_buttons.append(b)
        row.add_suffix(box)
        steps.add(row)
        row = Adw.EntryRow(title="…or keep only numbers equal to (optional, if you can see the number)")
        self.exact_row = row
        exact = Gtk.Button(label="Keep equal", valign=Gtk.Align.CENTER, sensitive=False)
        exact.connect("clicked", lambda _b: self._filter("exact"))
        row.add_suffix(exact)
        self.filter_buttons.append(exact)
        steps.add(row)
        self.limits = Adw.ExpanderRow(title="Only count changes of a certain size (optional)")
        self.at_least = Adw.EntryRow(title="Change at least")
        self.up_to = Adw.EntryRow(title="Change up to")
        self.limits.add_row(self.at_least)
        self.limits.add_row(self.up_to)
        steps.add(self.limits)
        page.append(steps)

        self.status = Gtk.Label(label="Not watching yet.", xalign=0, wrap=True)
        self.status.add_css_class("title-4")
        page.append(self.status)
        self.hint = Gtk.Label(label="", xalign=0, wrap=True)
        self.hint.add_css_class("dim-label")
        page.append(self.hint)
        reset = Gtk.Button(label="Start over", halign=Gtk.Align.START, sensitive=False)
        reset.connect("clicked", self._reset)
        self.reset_button = reset
        page.append(reset)
        return scroll

    def _recipe_changed(self, row, _param):
        self.recipe_text.set_label(RECIPES[row.get_selected()][1])

    def _start_watching(self, _b):
        def done(reply):
            if reply.get("ok"):
                self.watching = True
                self.watch_button.set_sensitive(False)
                self.snapshot_button.set_sensitive(True)
                self.status.set_label("Watching. Now press “Take a snapshot”.")
            else:
                self.toast(reply.get("error", "could not start watching"))
            return False
        self.client.ask({"cmd": "gfx_capture", "on": True}, done)

    def _snapshot(self, _b):
        self.status.set_label("Taking a snapshot…")
        self.client.ask({"cmd": "gfx_scan"}, self._scan_done)

    def _filter(self, mode):
        message = {"cmd": "gfx_next", "mode": mode}
        if mode == "exact":
            message["value"] = self.exact_row.get_text().strip()
        for key, row in (("at_least", self.at_least), ("up_to", self.up_to)):
            if mode in ("changed", "increased", "decreased") and row.get_text().strip():
                message[key] = row.get_text().strip()
        self.status.set_label("Filtering…")
        self.client.ask(message, self._scan_done)

    def _reset(self, _b):
        self.client.ask({"cmd": "gfx_scan_reset"}, lambda _r: False)
        self._show_candidates({"results": [], "count": 0})
        for b in self.filter_buttons:
            b.set_sensitive(False)
        self.snapshot_button.set_sensitive(self.watching)
        self.reset_button.set_sensitive(False)
        self.status.set_label("Watching. Press “Take a snapshot” to begin again." if self.watching else "Not watching yet.")
        self.hint.set_label("")

    def _scan_done(self, reply):
        if not reply.get("ok"):
            self.status.set_label(reply.get("error", "failed"))
            return False
        count = reply["count"]
        for b in self.filter_buttons:
            b.set_sensitive(count > 0)
        self.snapshot_button.set_sensitive(False)
        self.reset_button.set_sensitive(True)
        self.status.set_label("%s numbers still in the running." % format(count, ","))
        if count == 0:
            self.hint.set_label("Nothing is left. Press “Start over” and try again, a little slower: make one clear change in the game, then press the matching button.")
        elif count > 50000:
            self.hint.set_label("Lots left. Make a big, obvious change in the game (or stand completely still), then press the button that matches what you did.")
        elif count > 1000:
            self.hint.set_label("Getting there. Do one more round: change it, press the matching button; then change it back and press again.")
        elif count > 60:
            self.hint.set_label("Almost. One or two more rounds, including “Stayed the same” while you do nothing.")
        else:
            self.hint.set_label("Few enough! Open the Candidates tab and try them one by one.")
        self._show_candidates(reply)
        return False

    # ---------------------------------------------------------------- candidates page
    def _candidates_page(self):
        page = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10, margin_top=14, margin_bottom=14, margin_start=14, margin_end=14)
        note = Gtk.Label(wrap=True, xalign=0,
                         label="Try a candidate: ×0.5, ×2 or zero it and look at the game. If the picture changes the way you wanted, press Save and give it a name; "
                               "if not, press Undo and try the next one. Tests are undone when you close this window; saved tweaks stay.")
        note.add_css_class("dim-label")
        page.append(note)
        self.cand_status = Gtk.Label(label="No candidates yet. Use the Start here tab.", xalign=0)
        page.append(self.cand_status)
        self.cand_list = Gtk.ListBox(selection_mode=Gtk.SelectionMode.NONE)
        self.cand_list.add_css_class("boxed-list")
        scroll = Gtk.ScrolledWindow(vexpand=True)
        scroll.set_child(self.cand_list)
        page.append(scroll)
        return page

    def _show_candidates(self, reply):
        child = self.cand_list.get_first_child()
        while child:
            nxt = child.get_next_sibling()
            self.cand_list.remove(child)
            child = nxt
        self.result_rows = {}
        results = reply.get("results", [])
        self.cand_status.set_label("%d candidate%s%s" % (reply.get("count", 0), "" if reply.get("count", 0) == 1 else "s",
                                                       " (showing the first %d)" % len(results) if reply.get("count", 0) > len(results) else ""))
        for item in results[:150]:
            self.cand_list.append(self._candidate_row(item))
        return False

    def _candidate_row(self, item):
        key = (item["id"], item["offset"])
        row = Adw.ActionRow(title=GLib.markup_escape_text(describe(item)),
                            subtitle="value %s · used by %d draw%s" % (item["value"], item["draws"], "" if item["draws"] == 1 else "s"))
        for label, mode, value in (("÷2", "scale", 0.5), ("×2", "scale", 2.0), ("Zero", "set", 0.0)):
            b = Gtk.Button(label=label, valign=Gtk.Align.CENTER)
            b.connect("clicked", lambda _b, i=item, m=mode, v=value: self._try(i, m, v))
            row.add_suffix(b)
        undo = Gtk.Button(label="Undo", valign=Gtk.Align.CENTER)
        undo.connect("clicked", lambda _b, k=key: self._undo(k))
        save = Gtk.Button(label="Save…", valign=Gtk.Align.CENTER)
        save.add_css_class("suggested-action")
        save.connect("clicked", lambda _b, i=item: self._save_dialog(i))
        row.add_suffix(undo)
        row.add_suffix(save)
        self.result_rows[key] = row
        return row

    def _try(self, item, mode, value):
        key = (item["id"], item["offset"])
        if key in self.temp:
            self.client.ask({"cmd": "gfx_tweak_remove", "tid": self.temp.pop(key)}, lambda _r: False)

        def done(reply):
            if reply.get("ok"):
                self.temp[key] = reply["tid"]
            else:
                self.toast(reply.get("error", "failed"))
            return False
        self.client.ask({"cmd": "gfx_tweak_set", "id": item["id"], "offset": item["offset"], "mode": mode, "value": value,
                         "scope": "shader", "label": "test"}, done)

    def _undo(self, key):
        tid = self.temp.pop(key, None)
        if tid:
            self.client.ask({"cmd": "gfx_tweak_remove", "tid": tid}, lambda _r: False)

    def _save_dialog(self, item):
        dialog = Adw.AlertDialog(heading="Save this tweak", body="Name it so you can recognise it later. It is applied every time you launch %s." % self.name)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        entry = Gtk.Entry(placeholder_text="e.g. Flashlight strength", activates_default=True)
        scope = Gtk.DropDown.new_from_strings(["Only this shader", "Every shader using this slot (wider effect)"])
        box.append(entry)
        box.append(scope)
        dialog.set_extra_child(box)
        dialog.add_response("cancel", "Cancel")
        dialog.add_response("save", "Save")
        dialog.set_response_appearance("save", Adw.ResponseAppearance.SUGGESTED)
        dialog.set_default_response("save")

        def done(_d, response):
            if response != "save":
                return
            label = entry.get_text().strip() or describe(item)
            self._save_tweak(item, label, "slot" if scope.get_selected() == 1 else "shader")
        dialog.connect("response", done)
        dialog.present(self)

    def _save_tweak(self, item, label, scope):
        key = (item["id"], item["offset"])
        if key in self.temp:
            self.client.ask({"cmd": "gfx_tweak_remove", "tid": self.temp.pop(key)}, lambda _r: False)
        tweak = {"label": label, "pso": "0" if scope == "slot" else item["pso"], "stage": item["stage"], "reg": item["reg"],
                 "space": 0, "size": item["size"], "offset": item["offset"], "mode": "scale", "value": 1.0, "enabled": True,
                 "original": item["value"]}
        self.saved.append(tweak)
        save_tweaks(self.exe, self.saved)
        self._rebuild_tweaks()
        self.stack.set_visible_child_name("tweaks")
        self.toast("Saved. Move its slider in “My tweaks”.")

    # ---------------------------------------------------------------- saved tweaks page
    def _tweaks_page(self):
        page = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10, margin_top=14, margin_bottom=14, margin_start=14, margin_end=14)
        note = Gtk.Label(wrap=True, xalign=0,
                         label="Your saved tweaks for this game. “Multiply” scales the game's own number, “Add” shifts it, “Set” forces a fixed number. "
                               "“Hits” is how many draws the tweak touched last frame: 0 means nothing uses it right now (for example the light is off).")
        note.add_css_class("dim-label")
        page.append(note)
        self.tweak_list = Gtk.ListBox(selection_mode=Gtk.SelectionMode.NONE)
        self.tweak_list.add_css_class("boxed-list")
        self.tweak_empty = Adw.StatusPage(icon_name="preferences-system-symbolic", title="No saved tweaks",
                                          description="Find one with the Start here tab and press Save on a candidate")
        scroll = Gtk.ScrolledWindow(vexpand=True)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        box.append(self.tweak_empty)
        box.append(self.tweak_list)
        scroll.set_child(box)
        page.append(scroll)
        self.hit_labels = {}
        return page

    def _rebuild_tweaks(self):
        child = self.tweak_list.get_first_child()
        while child:
            nxt = child.get_next_sibling()
            self.tweak_list.remove(child)
            child = nxt
        self.hit_labels = {}
        self.tweak_empty.set_visible(not self.saved)
        self.tweak_list.set_visible(bool(self.saved))
        for index, tweak in enumerate(self.saved):
            self.tweak_list.append(self._tweak_row(index, tweak))
        # make sure the loader has them all (adds the new one; identical ones are replaced in place)
        for index, tweak in enumerate(self.saved):
            if tweak.get("enabled", True) and index not in self.saved_tids:
                self.client.ask(dict({"cmd": "gfx_tweak_set"}, **self._tweak_message(tweak)), lambda r, i=index: self._tid_reply(r, i))

    def _tweak_message(self, tweak):
        return {"pso": tweak["pso"], "stage": tweak["stage"], "reg": tweak["reg"], "space": tweak["space"], "size": tweak["size"],
                "offset": tweak["offset"], "mode": tweak["mode"], "value": tweak["value"], "label": tweak["label"]}

    def _tid_reply(self, reply, index):
        if reply.get("ok"):
            self.saved_tids[index] = reply["tid"]
        return False

    def _tweak_row(self, index, tweak):
        row = Adw.ActionRow(title=GLib.markup_escape_text(tweak["label"]),
                            subtitle=GLib.markup_escape_text("%s · slot b%d · byte %d%s" % (STAGE_LABEL.get(tweak["stage"], tweak["stage"]), tweak["reg"], tweak["offset"],
                                                                                        " · any shader" if tweak["pso"] in ("0", 0) else " · shader " + short(str(tweak["pso"])))))
        enable = Gtk.Switch(valign=Gtk.Align.CENTER, active=tweak.get("enabled", True))
        enable.connect("notify::active", lambda s, _p: self._enable(index, s.get_active()))
        mode = Gtk.DropDown.new_from_strings(["Multiply", "Add", "Set"])
        mode.set_valign(Gtk.Align.CENTER)
        mode.set_selected({"scale": 0, "add": 1, "set": 2}.get(tweak["mode"], 0))
        spin = Gtk.SpinButton.new_with_range(-1000000, 1000000, 0.05)
        spin.set_digits(3)
        spin.set_valign(Gtk.Align.CENTER)
        spin.set_value(tweak["value"])
        slider = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 0.0, 4.0, 0.01)
        slider.set_size_request(160, -1)
        slider.set_valign(Gtk.Align.CENTER)
        slider.set_draw_value(False)
        slider.set_value(min(max(tweak["value"], 0.0), 4.0))
        slider.set_visible(tweak["mode"] == "scale")

        def push():
            self.saved[index]["value"] = spin.get_value()
            save_tweaks(self.exe, self.saved)
            tid = self.saved_tids.get(index)
            if tid:
                self.client.ask({"cmd": "gfx_tweak_update", "tid": tid, "value": spin.get_value()}, lambda _r: False)
            else:
                self.client.ask(dict({"cmd": "gfx_tweak_set"}, **self._tweak_message(self.saved[index])), lambda r, i=index: self._tid_reply(r, i))

        def on_slider(s):
            spin.set_value(round(s.get_value(), 3))
        slider.connect("value-changed", on_slider)
        spin.connect("value-changed", lambda s: (slider.set_value(min(max(s.get_value(), 0.0), 4.0)), push()))

        def on_mode(drop, _p):
            names = ["scale", "add", "set"]
            self.saved[index]["mode"] = names[drop.get_selected()]
            slider.set_visible(drop.get_selected() == 0)
            if drop.get_selected() == 0 and not (0.0 <= spin.get_value() <= 4.0):
                spin.set_value(1.0)
            self._resend(index)
        mode.connect("notify::selected", on_mode)
        hits = Gtk.Label(label="hits –", valign=Gtk.Align.CENTER)
        hits.add_css_class("dim-label")
        self.hit_labels[index] = hits
        remove = Gtk.Button(icon_name="user-trash-symbolic", valign=Gtk.Align.CENTER, tooltip_text="Delete this tweak")
        remove.add_css_class("flat")
        remove.connect("clicked", lambda _b: self._delete(index))
        for w in (hits, mode, slider, spin, enable, remove):
            row.add_suffix(w)
        return row

    def _resend(self, index):
        save_tweaks(self.exe, self.saved)
        tid = self.saved_tids.pop(index, None)
        if tid:
            self.client.ask({"cmd": "gfx_tweak_remove", "tid": tid}, lambda _r: False)
        if self.saved[index].get("enabled", True):
            self.client.ask(dict({"cmd": "gfx_tweak_set"}, **self._tweak_message(self.saved[index])), lambda r, i=index: self._tid_reply(r, i))

    def _enable(self, index, on):
        self.saved[index]["enabled"] = on
        save_tweaks(self.exe, self.saved)
        tid = self.saved_tids.get(index)
        if tid:
            self.client.ask({"cmd": "gfx_tweak_enable", "tid": tid, "on": on}, lambda _r: False)
        elif on:
            self._resend(index)

    def _delete(self, index):
        tid = self.saved_tids.pop(index, None)
        if tid:
            self.client.ask({"cmd": "gfx_tweak_remove", "tid": tid}, lambda _r: False)
        del self.saved[index]
        self.saved_tids = {(i if i < index else i - 1): t for i, t in self.saved_tids.items()}
        save_tweaks(self.exe, self.saved)
        self._rebuild_tweaks()

    # ---------------------------------------------------------------- browse page
    def _browse_page(self):
        page = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10, margin_top=14, margin_bottom=14, margin_start=14, margin_end=14)
        note = Gtk.Label(wrap=True, xalign=0,
                         label="For the curious: every constant buffer the last frame used. Pick one to see its numbers live. "
                               "Needs “Start watching” on the first tab.")
        note.add_css_class("dim-label")
        page.append(note)
        top = Gtk.Box(spacing=8)
        refresh = Gtk.Button(label="Refresh the list")
        refresh.connect("clicked", lambda _b: self._load_cbs())
        top.append(refresh)
        self.cb_status = Gtk.Label(label="", xalign=0, hexpand=True)
        top.append(self.cb_status)
        page.append(top)
        self.cb_list = Gtk.ListBox(selection_mode=Gtk.SelectionMode.NONE)
        self.cb_list.add_css_class("boxed-list")
        list_scroll = Gtk.ScrolledWindow(vexpand=True)
        list_scroll.set_child(self.cb_list)
        page.append(list_scroll)
        self.cb_view = Gtk.TextView(editable=False, monospace=True, cursor_visible=False)
        view_scroll = Gtk.ScrolledWindow(vexpand=True)
        view_scroll.set_child(self.cb_view)
        page.append(view_scroll)
        self.cb_open = None
        return page

    def _load_cbs(self):
        def done(reply):
            if not reply.get("ok"):
                self.cb_status.set_label(reply.get("error", "failed"))
                return False
            cbs = sorted(reply["cbs"], key=lambda c: -c["draws"])
            self.cb_status.set_label("%d constant buffers (%d GPU-only, not editable)" % (reply["keys"], reply["gpu_only"]))
            child = self.cb_list.get_first_child()
            while child:
                nxt = child.get_next_sibling()
                self.cb_list.remove(child)
                child = nxt
            for cb in cbs[:200]:
                row = Adw.ActionRow(title="%s · %s · slot b%d" % (STAGE_LABEL.get(cb["stage"], cb["stage"]), short(cb["pso"]), cb["reg"]),
                                    subtitle="%d draws · bound range %d bytes" % (cb["draws"], cb["size"]))
                b = Gtk.Button(label="Show numbers", valign=Gtk.Align.CENTER)
                b.connect("clicked", lambda _b, c=cb: self._open_cb(c))
                row.add_suffix(b)
                self.cb_list.append(row)
            return False
        self.client.ask({"cmd": "gfx_cbs"}, done)

    def _open_cb(self, cb):
        self.cb_open = cb
        self._show_cb()

    def _show_cb(self):
        if not self.cb_open:
            return
        def done(reply):
            buffer = self.cb_view.get_buffer()
            if not reply.get("ok"):
                buffer.set_text(reply.get("error", "failed"))
                return False
            floats = reply["floats"]
            lines = ["%s · %s · slot b%d   (each row is one float4 register, 16 bytes)" % (STAGE_LABEL.get(self.cb_open["stage"], ""),
                                                                                        short(self.cb_open["pso"]), self.cb_open["reg"])]
            for i in range(0, min(len(floats), 256), 4):
                lines.append("+%4d  %s" % (i * 4, "  ".join("%12s" % v for v in floats[i:i + 4])))
            buffer.set_text("\n".join(lines))
            return False
        self.client.ask({"cmd": "gfx_cb_read", "id": self.cb_open["id"]}, done)

    # ---------------------------------------------------------------- periodic refresh
    def _refresh(self):
        if not self.alive:
            return False
        page = self.stack.get_visible_child_name()
        if page == "candidates" and self.result_rows:
            self.client.ask({"cmd": "gfx_results"}, self._refresh_candidates)
        elif page == "tweaks" and self.saved:
            self.client.ask({"cmd": "gfx_tweaks"}, self._refresh_hits)
        elif page == "browse" and self.cb_open:
            self._show_cb()
        return True

    def _refresh_candidates(self, reply):
        if reply.get("ok"):
            for item in reply["results"]:
                row = self.result_rows.get((item["id"], item["offset"]))
                if row:
                    row.set_subtitle("value %s · used by %d draw%s" % (item["value"], item["draws"], "" if item["draws"] == 1 else "s"))
        return False

    def _refresh_hits(self, reply):
        if reply.get("ok"):
            by_tid = {t["tid"]: t for t in reply["tweaks"]}
            for index, label in self.hit_labels.items():
                t = by_tid.get(self.saved_tids.get(index))
                label.set_label("hits %d" % t["hits"] if t else "hits –")
        return False
