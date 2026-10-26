"""Display page: final-image effects (brightness, colour, sharpen, colour-vision modes…) and screenshots.

Settings live in ~/.beer/display.json, are sent live to every running game (post_set) and at launch
(BEER_POST_JSON). They act on the finished picture, after the game's own tone mapping.
"""
import json
import os

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
gi.require_version("Gdk", "4.0")
from gi.repository import Adw, Gdk, Gio, GLib, Gtk  # noqa: E402

import storage  # noqa: E402

DISPLAY_FILE = storage.path("display.json")

# key, label, minimum, maximum, neutral, step, description
SLIDERS = [
    ("brightness", "Brightness", -0.5, 0.5, 0.0, 0.01, "Adds or removes light from the whole picture"),
    ("contrast", "Contrast", 0.3, 3.0, 1.0, 0.01, ""),
    ("gamma", "Gamma", 0.3, 3.0, 1.0, 0.01, "Higher lifts the shadows, lower deepens them"),
    ("saturation", "Saturation", 0.0, 3.0, 1.0, 0.01, "0 is black and white"),
    ("vibrance", "Vibrance", -1.0, 2.0, 0.0, 0.01, "Boosts dull colours more than vivid ones"),
    ("temperature", "Temperature", -1.0, 1.0, 0.0, 0.01, "Negative is cooler, positive is warmer"),
    ("sharpen", "Sharpen", 0.0, 3.0, 0.0, 0.01, ""),
    ("vignette", "Vignette", 0.0, 1.0, 0.0, 0.01, "Darkens the corners"),
    ("grain", "Film grain", 0.0, 1.0, 0.0, 0.01, ""),
    ("letterbox", "Letterbox bars", 0.0, 0.3, 0.0, 0.005, "Black bars at the top and bottom (fraction of the height)"),
]
COLOUR_MODES = ["Off", "Correct: protanopia (red-weak)", "Correct: deuteranopia (green-weak)", "Correct: tritanopia (blue-weak)",
                "Simulate: protanopia", "Simulate: deuteranopia", "Simulate: tritanopia"]
DEFAULT = {"effects": {k: n for k, _l, _a, _b, n, _s, _d in SLIDERS}, "colorblind": 0, "cb_strength": 1.0,
           "shot": {"keysym": 0xffc9, "shift": False, "ctrl": False, "alt": False, "label": "F12"}}


def load_config():
    try:
        with open(DISPLAY_FILE) as f:
            data = json.load(f)
    except (OSError, ValueError):
        data = {}
    cfg = json.loads(json.dumps(DEFAULT))
    cfg["effects"].update(data.get("effects", {}))
    for key in ("colorblind", "cb_strength"):
        if key in data:
            cfg[key] = data[key]
    cfg["shot"].update(data.get("shot", {}))
    return cfg


def save_config(cfg):
    os.makedirs(os.path.dirname(DISPLAY_FILE), exist_ok=True)
    tmp = DISPLAY_FILE + ".tmp"
    with open(tmp, "w") as f:
        json.dump(cfg, f, indent=2)
    os.replace(tmp, DISPLAY_FILE)


def post_message(cfg):
    message = {"cmd": "post_set", "colorblind": cfg["colorblind"], "cb_strength": cfg["cb_strength"]}
    message.update({k: round(v, 3) for k, v in cfg["effects"].items()})
    shot = cfg["shot"]
    message["shot_keysym"] = shot["keysym"]
    message["shot_mods"] = (1 if shot["shift"] else 0) | (4 if shot["ctrl"] else 0) | (8 if shot["alt"] else 0)
    return message


class DisplayPage(Gtk.Box):
    def __init__(self, app):
        super().__init__(orientation=Gtk.Orientation.VERTICAL)
        self.app = app
        self.cfg = load_config()
        self._building = True
        self._push_id = None

        clamp = Adw.Clamp(maximum_size=760, margin_top=18, margin_bottom=18, margin_start=12, margin_end=12)
        body = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=18)
        clamp.set_child(body)
        scroll = Gtk.ScrolledWindow(vexpand=True)
        scroll.set_child(clamp)
        self.append(scroll)

        shots = Adw.PreferencesGroup(title="Screenshots",
                                     description="Saved as PNG at the game's own resolution in ~/Pictures/Beer. “Clean” leaves the overlay out.")
        self.shot_row = Adw.ActionRow(title="Screenshot shortcut", subtitle=self.cfg["shot"]["label"])
        change = Gtk.Button(label="Change…", valign=Gtk.Align.CENTER)
        change.connect("clicked", lambda _b: self._capture_hotkey())
        self.shot_row.add_suffix(change)
        shots.add(self.shot_row)
        now = Adw.ActionRow(title="Take one now", subtitle="From every running game")
        for label, clean in (("Screenshot", False), ("Clean screenshot", True)):
            b = Gtk.Button(label=label, valign=Gtk.Align.CENTER)
            b.connect("clicked", lambda _b, c=clean: self._screenshot(c))
            now.add_suffix(b)
        folder = Gtk.Button(icon_name="folder-open-symbolic", valign=Gtk.Align.CENTER, tooltip_text="Open the screenshot folder")
        folder.add_css_class("flat")
        folder.connect("clicked", lambda _b: self._open_folder())
        now.add_suffix(folder)
        shots.add(now)
        body.append(shots)

        effects = Adw.PreferencesGroup(title="Picture",
                                       description="Applies to every game, live. These work on the finished image, after the game's own tone mapping.")
        reset = Gtk.Button(label="Reset all", valign=Gtk.Align.CENTER)
        reset.connect("clicked", lambda _b: self._reset_all())
        effects.set_header_suffix(reset)
        self.scales = {}
        for key, label, lo, hi, neutral, step, description in SLIDERS:
            row = Adw.ActionRow(title=label, subtitle=description)
            scale = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, lo, hi, step)
            scale.set_size_request(280, -1)
            scale.set_valign(Gtk.Align.CENTER)
            scale.set_draw_value(True)
            scale.set_digits(2)
            scale.set_value(self.cfg["effects"][key])
            scale.add_mark(neutral, Gtk.PositionType.BOTTOM, None)
            scale.connect("value-changed", self._changed, key)
            row.add_suffix(scale)
            effects.add(row)
            self.scales[key] = scale
        body.append(effects)

        vision = Adw.PreferencesGroup(title="Colour vision", description="Correct colours for colour-blind players, or simulate how others see the game.")
        self.mode = Adw.ComboRow(title="Mode", model=Gtk.StringList.new(COLOUR_MODES))
        self.mode.set_selected(self.cfg["colorblind"])
        self.mode.connect("notify::selected", self._mode_changed)
        self.strength = Adw.ActionRow(title="Strength")
        self.strength_scale = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 0.0, 1.0, 0.01)
        self.strength_scale.set_size_request(280, -1)
        self.strength_scale.set_valign(Gtk.Align.CENTER)
        self.strength_scale.set_digits(2)
        self.strength_scale.set_value(self.cfg["cb_strength"])
        self.strength_scale.connect("value-changed", self._strength_changed)
        self.strength.add_suffix(self.strength_scale)
        vision.add(self.mode)
        vision.add(self.strength)
        body.append(vision)
        self._building = False

    # ------------------------------------------------------------------ state
    def _changed(self, scale, key):
        if self._building:
            return
        self.cfg["effects"][key] = scale.get_value()
        self._commit()

    def _mode_changed(self, row, _param):
        if self._building:
            return
        self.cfg["colorblind"] = row.get_selected()
        self._commit()

    def _strength_changed(self, scale):
        if self._building:
            return
        self.cfg["cb_strength"] = scale.get_value()
        self._commit()

    def _reset_all(self):
        self._building = True
        for key, _l, _a, _b, neutral, _s, _d in SLIDERS:
            self.scales[key].set_value(neutral)
            self.cfg["effects"][key] = neutral
        self.mode.set_selected(0)
        self.cfg["colorblind"] = 0
        self.strength_scale.set_value(1.0)
        self.cfg["cb_strength"] = 1.0
        self._building = False
        self._commit()

    def _commit(self):
        save_config(self.cfg)
        if self._push_id:
            GLib.source_remove(self._push_id)
        self._push_id = GLib.timeout_add(80, self._push)

    def _push(self):
        self._push_id = None
        self._broadcast(post_message(self.cfg))
        return False

    def _broadcast(self, message):
        replies = [card.control.request(message) for card in list(self.app.cards.values())]
        return replies

    # ------------------------------------------------------------------ screenshots
    def _screenshot(self, clean):
        replies = self._broadcast({"cmd": "screenshot", "clean": clean})
        paths = [r["path"] for r in replies if r and r.get("ok")]
        self.app.toast("Saved %s" % os.path.basename(paths[0]) if paths else "No game is running")

    def _open_folder(self):
        folder = os.path.expanduser("~/Pictures/Beer")
        os.makedirs(folder, exist_ok=True)
        Gio.AppInfo.launch_default_for_uri("file://" + folder, None)

    def _capture_hotkey(self):
        dialog = Adw.AlertDialog(heading="Press the screenshot shortcut", body="Press the key (with Ctrl, Shift or Alt if you like).\nEscape cancels.")
        dialog.add_response("cancel", "Cancel")
        controller = Gtk.EventControllerKey()

        def pressed(_c, keyval, _code, state):
            if keyval == Gdk.KEY_Escape:
                dialog.close()
                return True
            if keyval in (Gdk.KEY_Shift_L, Gdk.KEY_Shift_R, Gdk.KEY_Control_L, Gdk.KEY_Control_R, Gdk.KEY_Alt_L, Gdk.KEY_Alt_R,
                          Gdk.KEY_Meta_L, Gdk.KEY_Meta_R, Gdk.KEY_Super_L, Gdk.KEY_Super_R, Gdk.KEY_ISO_Level3_Shift):
                return True
            lower = Gdk.keyval_to_lower(keyval)
            mods = state & (Gdk.ModifierType.SHIFT_MASK | Gdk.ModifierType.CONTROL_MASK | Gdk.ModifierType.ALT_MASK)
            self.cfg["shot"].update(keysym=int(lower), shift=bool(mods & Gdk.ModifierType.SHIFT_MASK),
                                    ctrl=bool(mods & Gdk.ModifierType.CONTROL_MASK), alt=bool(mods & Gdk.ModifierType.ALT_MASK),
                                    label=Gtk.accelerator_get_label(lower, mods))
            self.shot_row.set_subtitle(self.cfg["shot"]["label"])
            self._commit()
            dialog.close()
            return True
        controller.connect("key-pressed", pressed)
        dialog.add_controller(controller)
        dialog.present(self.app.window)
