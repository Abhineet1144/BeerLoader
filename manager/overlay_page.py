"""Overlay configuration page: hotkey, drag-and-drop layout preview, fields and colours.

The configuration lives in ~/.config/beer/overlay.json. It is sent to the loader as a flat JSON
object (see beer_overlay.c) at launch (BEER_OVERLAY_JSON) and live to every running game.
"""
import json
import os

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
gi.require_version("Gdk", "4.0")
from gi.repository import Adw, Gdk, GLib, Gtk  # noqa: E402

import storage  # noqa: E402

OVERLAY_FILE = storage.path("overlay.json")

FIELDS = [("fps", "FPS"), ("frametime", "Frame time"), ("cpu", "CPU usage"), ("ram", "Memory (RAM)"),
          ("threads", "Thread count"), ("speed", "Game speed"), ("uptime", "Uptime"),
          ("gpu", "GPU usage"), ("vram", "Video memory (VRAM)"), ("clock", "Clock (time of day)"), ("graph", "Frame time graph")]
SIZES = ["Small", "Medium", "Large"]
# Overlay font cells (matching platform/overlay_font.h) used to draw an accurate preview.
CELLS = [(8, 17), (12, 24), (17, 33)]
REF_W, REF_H = 1920, 1080

DEFAULT = {
    "hotkey": {"keysym": 0xffc7, "shift": False, "ctrl": False, "alt": False, "label": "F10"},
    "resources": {"enabled": True, "x": 0.01, "y": 0.01, "size": 1, "fields": ["fps", "frametime", "cpu", "ram", "threads"]},
    "pins": {"enabled": True, "x": 0.01, "y": 0.30, "size": 1},
    "colors": {"panel": "12161c", "text": "e8e8e8", "accent": "8fe3a0"},
    "opacity": 0.85,
}


def load_config():
    try:
        with open(OVERLAY_FILE) as f:
            data = json.load(f)
    except (OSError, ValueError):
        data = {}
    cfg = json.loads(json.dumps(DEFAULT))
    for key, value in data.items():
        if isinstance(value, dict) and key in cfg:
            cfg[key].update(value)
    if isinstance(data.get("opacity"), (int, float)):
        cfg["opacity"] = data["opacity"]
    return cfg


def save_config(cfg):
    os.makedirs(os.path.dirname(OVERLAY_FILE), exist_ok=True)
    tmp = OVERLAY_FILE + ".tmp"
    with open(tmp, "w") as f:
        json.dump(cfg, f, indent=2)
    os.replace(tmp, OVERLAY_FILE)


def flat_message(cfg, command="overlay_set"):
    mods = (1 if cfg["hotkey"]["shift"] else 0) | (4 if cfg["hotkey"]["ctrl"] else 0) | (8 if cfg["hotkey"]["alt"] else 0)
    r, p = cfg["resources"], cfg["pins"]
    return {
        "cmd": command, "hotkey_keysym": cfg["hotkey"]["keysym"], "hotkey_mods": mods,
        "opacity": round(cfg.get("opacity", 0.85), 3), "panel_color": cfg["colors"]["panel"], "text_color": cfg["colors"]["text"], "accent_color": cfg["colors"]["accent"],
        "fields": ",".join(r["fields"]),
        "res_enabled": r["enabled"], "res_x": round(r["x"], 4), "res_y": round(r["y"], 4), "res_size": r["size"],
        "pin_enabled": p["enabled"], "pin_x": round(p["x"], 4), "pin_y": round(p["y"], 4), "pin_size": p["size"],
    }


def box_pixels(cfg, which):
    """Approximate pixel size of a box at the 1080p reference, as the loader would draw it."""
    box = cfg[which]
    cw, ch = CELLS[max(0, min(2, box["size"]))]
    if which == "resources":
        lines = 1 + len([f for f in box["fields"] if f != "graph"]) + (1 if "graph" in box["fields"] else 0) * 3.4
        lines = int(round(lines))
        chars = 22
    else:
        lines, chars = 4, 27
    pad = ch // 3 + 2
    return chars * cw + pad * 2, lines * ch + pad * 2


def parse_color(hex_text):
    rgba = Gdk.RGBA()
    rgba.parse("#" + hex_text)
    return rgba


def hex_of(rgba):
    return "%02x%02x%02x" % (round(rgba.red * 255), round(rgba.green * 255), round(rgba.blue * 255))


class OverlayPage(Gtk.Box):
    def __init__(self, app):
        super().__init__(orientation=Gtk.Orientation.VERTICAL)
        self.app = app
        self.cfg = load_config()
        self.drag = None
        self.selected = "resources"
        self._push_id = None
        self._building = True

        clamp = Adw.Clamp(maximum_size=760, margin_top=18, margin_bottom=18, margin_start=12, margin_end=12)
        body = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=18)
        clamp.set_child(body)
        scroll = Gtk.ScrolledWindow(vexpand=True)
        scroll.set_child(clamp)
        self.append(scroll)

        # --- hotkey + live preview in the running game
        top = Adw.PreferencesGroup(title="Overlay",
                                   description="An in-game panel with resource usage and the memory values you pinned from the Memory window. "
                                               "Pins are per launch and have to be set up again after a restart.")
        self.hotkey_row = Adw.ActionRow(title="Toggle shortcut", subtitle=self.cfg["hotkey"]["label"])
        change = Gtk.Button(label="Change…", valign=Gtk.Align.CENTER)
        change.connect("clicked", lambda _b: self._capture_hotkey())
        self.hotkey_row.add_suffix(change)
        top.add(self.hotkey_row)
        show = Adw.ActionRow(title="Show / hide in running games", subtitle="Same as pressing the shortcut in the game")
        show_button = Gtk.Button(label="Toggle now", valign=Gtk.Align.CENTER)
        show_button.connect("clicked", lambda _b: self._broadcast({"cmd": "overlay_toggle"}))
        show.add_suffix(show_button)
        top.add(show)
        body.append(top)

        # --- layout editor
        layout = Adw.PreferencesGroup(title="Layout", description="Drag the boxes to where you want them on the screen.")
        self.area = Gtk.DrawingArea(content_width=640, content_height=360, halign=Gtk.Align.CENTER)
        self.area.set_draw_func(self._draw)
        drag = Gtk.GestureDrag()
        drag.connect("drag-begin", self._drag_begin)
        drag.connect("drag-update", self._drag_update)
        drag.connect("drag-end", self._drag_end)
        self.area.add_controller(drag)
        frame = Gtk.Frame(halign=Gtk.Align.CENTER)
        frame.set_child(self.area)
        layout.add(frame)
        body.append(layout)

        # --- per box options
        res = Adw.PreferencesGroup(title="Resource usage box")
        self.res_enabled = Adw.SwitchRow(title="Show this box")
        self.res_enabled.set_active(self.cfg["resources"]["enabled"])
        self.res_enabled.connect("notify::active", self._changed)
        self.res_size = Adw.ComboRow(title="Text size", model=Gtk.StringList.new(SIZES))
        self.res_size.set_selected(self.cfg["resources"]["size"])
        self.res_size.connect("notify::selected", self._changed)
        res.add(self.res_enabled)
        res.add(self.res_size)
        self.field_rows = {}
        for key, label in FIELDS:
            row = Adw.SwitchRow(title=label)
            row.set_active(key in self.cfg["resources"]["fields"])
            row.connect("notify::active", self._changed)
            res.add(row)
            self.field_rows[key] = row
        body.append(res)

        pins = Adw.PreferencesGroup(title="Pinned memory box",
                                    description="Pin values from a running game: Memory… → add to the watch list → Pin.")
        self.pin_enabled = Adw.SwitchRow(title="Show this box")
        self.pin_enabled.set_active(self.cfg["pins"]["enabled"])
        self.pin_enabled.connect("notify::active", self._changed)
        self.pin_size = Adw.ComboRow(title="Text size", model=Gtk.StringList.new(SIZES))
        self.pin_size.set_selected(self.cfg["pins"]["size"])
        self.pin_size.connect("notify::selected", self._changed)
        pins.add(self.pin_enabled)
        pins.add(self.pin_size)
        body.append(pins)

        colors = Adw.PreferencesGroup(title="Colours")
        opacity_row = Adw.ActionRow(title="Panel opacity", subtitle="Text and borders stay solid")
        self.opacity_scale = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 0.1, 1.0, 0.01)
        self.opacity_scale.set_size_request(260, -1)
        self.opacity_scale.set_valign(Gtk.Align.CENTER)
        self.opacity_scale.set_digits(2)
        self.opacity_scale.set_value(self.cfg.get("opacity", 0.85))
        self.opacity_scale.connect("value-changed", self._changed)
        opacity_row.add_suffix(self.opacity_scale)
        colors.add(opacity_row)
        self.color_buttons = {}
        for key, label in (("panel", "Panel background"), ("text", "Text"), ("accent", "Title and border")):
            row = Adw.ActionRow(title=label)
            button = Gtk.ColorDialogButton(dialog=Gtk.ColorDialog(), valign=Gtk.Align.CENTER)
            button.set_rgba(parse_color(self.cfg["colors"][key]))
            button.connect("notify::rgba", self._changed)
            row.add_suffix(button)
            colors.add(row)
            self.color_buttons[key] = button
        body.append(colors)
        self._building = False

    # ---------------------------------------------------------------- state
    def _changed(self, *_args):
        if self._building:
            return
        r, p = self.cfg["resources"], self.cfg["pins"]
        r["enabled"] = self.res_enabled.get_active()
        r["size"] = self.res_size.get_selected()
        r["fields"] = [key for key, _ in FIELDS if self.field_rows[key].get_active()]
        p["enabled"] = self.pin_enabled.get_active()
        p["size"] = self.pin_size.get_selected()
        for key, button in self.color_buttons.items():
            self.cfg["colors"][key] = hex_of(button.get_rgba())
        self.cfg["opacity"] = self.opacity_scale.get_value()
        self._commit()

    def _commit(self):
        save_config(self.cfg)
        self.area.queue_draw()
        if self._push_id:
            GLib.source_remove(self._push_id)
        self._push_id = GLib.timeout_add(150, self._push)

    def _push(self):
        self._push_id = None
        self._broadcast(flat_message(self.cfg))
        return False

    def _broadcast(self, message):
        for card in list(self.app.cards.values()):
            card.control.request(message)

    # ---------------------------------------------------------------- hotkey capture
    def _capture_hotkey(self):
        dialog = Adw.AlertDialog(heading="Press the new shortcut", body="Press the key (with Ctrl, Shift or Alt if you like).\nEscape cancels.")
        dialog.add_response("cancel", "Cancel")
        controller = Gtk.EventControllerKey()

        def pressed(_c, keyval, _code, state):
            if keyval == Gdk.KEY_Escape:
                dialog.close()
                return True
            if keyval in (Gdk.KEY_Shift_L, Gdk.KEY_Shift_R, Gdk.KEY_Control_L, Gdk.KEY_Control_R, Gdk.KEY_Alt_L,
                          Gdk.KEY_Alt_R, Gdk.KEY_Meta_L, Gdk.KEY_Meta_R, Gdk.KEY_Super_L, Gdk.KEY_Super_R, Gdk.KEY_ISO_Level3_Shift):
                return True
            lower = Gdk.keyval_to_lower(keyval)
            hk = self.cfg["hotkey"]
            hk.update(keysym=int(lower), shift=bool(state & Gdk.ModifierType.SHIFT_MASK),
                      ctrl=bool(state & Gdk.ModifierType.CONTROL_MASK), alt=bool(state & Gdk.ModifierType.ALT_MASK))
            hk["label"] = Gtk.accelerator_get_label(lower, state & (Gdk.ModifierType.SHIFT_MASK | Gdk.ModifierType.CONTROL_MASK | Gdk.ModifierType.ALT_MASK))
            self.hotkey_row.set_subtitle(hk["label"])
            self._commit()
            dialog.close()
            return True
        controller.connect("key-pressed", pressed)
        dialog.add_controller(controller)
        dialog.present(self.app.window)

    # ---------------------------------------------------------------- layout canvas
    def _scale(self):
        return self.area.get_width() / REF_W

    def _rect(self, which):
        scale = self._scale()
        w, h = box_pixels(self.cfg, which)
        box = self.cfg[which]
        x = min(max(box["x"] * REF_W, 0), REF_W - w) * scale
        y = min(max(box["y"] * REF_H, 0), REF_H - h) * scale
        return x, y, w * scale, h * scale

    def _draw(self, _area, cr, width, height):
        cr.set_source_rgb(0.08, 0.09, 0.11)
        cr.rectangle(0, 0, width, height)
        cr.fill()
        cr.set_source_rgba(1, 1, 1, 0.06)
        for i in range(1, 4):
            cr.rectangle(width * i / 4, 0, 1, height)
            cr.rectangle(0, height * i / 4, width, 1)
        cr.fill()
        cr.set_source_rgba(1, 1, 1, 0.35)
        cr.select_font_face("Sans")
        cr.set_font_size(12)
        cr.move_to(8, height - 8)
        cr.show_text("Game screen (preview at 1920×1080)")
        panel, text, accent = (parse_color(self.cfg["colors"][k]) for k in ("panel", "text", "accent"))
        for which, title in (("resources", "Resources"), ("pins", "Pinned memory")):
            if not self.cfg[which]["enabled"]:
                continue
            x, y, w, h = self._rect(which)
            cr.set_source_rgb(panel.red, panel.green, panel.blue)
            cr.rectangle(x, y, w, h)
            cr.fill()
            cr.set_source_rgb(accent.red, accent.green, accent.blue)
            cr.set_line_width(2 if which == self.selected else 1)
            cr.rectangle(x + 0.5, y + 0.5, w - 1, h - 1)
            cr.stroke()
            cr.set_source_rgb(accent.red, accent.green, accent.blue)
            cr.set_font_size(max(9, min(h / 4, 15)))
            cr.move_to(x + 6, y + 6 + max(9, min(h / 4, 15)))
            cr.show_text(title)
            cr.set_source_rgb(text.red, text.green, text.blue)
            sample = {"fps": "FPS 120.0", "frametime": "Frame time 8.3 ms", "cpu": "CPU 8.7% (1.7 cores)", "ram": "RAM 2.2 GB",
                      "threads": "Threads 70", "speed": "Speed 1.00x", "uptime": "Uptime 0:44", "gpu": "GPU 63%",
                      "vram": "VRAM 5120 / 16376 MB", "clock": "Time 21:04:33", "graph": "▂▃▂▂▃▂▂▃▂▂▅▂▂▃▂"}
            if which == "resources":
                shown = [sample[key] for key, _ in FIELDS if key in self.cfg["resources"]["fields"]]
            else:
                shown = ["label: value"]
            size = max(8, min(h / (len(shown) + 2), 13))
            cr.set_font_size(size)
            for i, line in enumerate(shown):
                cr.move_to(x + 6, y + 8 + max(9, min(h / 4, 15)) + (i + 1) * size * 1.2)
                cr.show_text(line)

    def _hit(self, px, py):
        for which in ("pins", "resources"):   # later-drawn first
            if not self.cfg[which]["enabled"]:
                continue
            x, y, w, h = self._rect(which)
            if x <= px <= x + w and y <= py <= y + h:
                return which
        return None

    def _drag_begin(self, _gesture, px, py):
        which = self._hit(px, py)
        self.drag = None
        if which:
            self.selected = which
            x, y, _w, _h = self._rect(which)
            self.drag = (which, px - x, py - y)
            self.area.queue_draw()

    def _drag_update(self, gesture, dx, dy):
        if not self.drag:
            return
        which, ox, oy = self.drag
        ok, sx, sy = gesture.get_start_point()
        scale = self._scale()
        w, h = box_pixels(self.cfg, which)
        nx = (sx + dx - ox) / scale
        ny = (sy + dy - oy) / scale
        box = self.cfg[which]
        box["x"] = min(max(nx, 0), REF_W - w) / REF_W
        box["y"] = min(max(ny, 0), REF_H - h) / REF_H
        self.area.queue_draw()

    def _drag_end(self, *_args):
        if self.drag:
            self.drag = None
            self._commit()
