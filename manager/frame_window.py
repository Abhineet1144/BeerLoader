"""Frame breakdown: how the game builds one frame, step by step.

Beer records the next presented frame of a D3D12 game and saves what every render pass drew (its colour targets and
its depth buffer) as small pictures. This window shows them in order, so you can watch the frame come together:
shadow maps, the G-buffer (albedo, normals, material), lighting, transparent effects, post-processing and the UI.
Captures live in ~/.beer/frames/<game>-<time>/ (PPM pictures, one per target per pass) only while they are shown:
a new capture deletes the previous one, and closing the window (or opening it again) deletes the game's captures.
"""
import os
import re
import shutil
import time

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
gi.require_version("GdkPixbuf", "2.0")
from gi.repository import Adw, Gdk, GdkPixbuf, Gio, GLib, Gtk  # noqa: E402

import storage  # noqa: E402
from memory_window import MemClient  # noqa: E402

FRAME_DIR = storage.path("frames")
WAIT_SECONDS = 20

# rt012_a0_1280x720_vk97_h<pso>.ppm (colour target 0 of pass 12) and rt012_dp005_l0_2560x1440_vk126_h<pso>.ppm (its depth).
NAME_RE = re.compile(r"^rt(\d+)_(?:a(\d+)|dp\d+_l(\d+))_(\d+)x(\d+)_vk(\d+)_h([0-9a-f]+)\.ppm$")

VK_FORMAT_NAMES = {
    37: "RGBA8", 43: "RGBA8 sRGB", 44: "BGRA8", 50: "BGRA8 sRGB", 64: "RGB10A2", 97: "RGBA16F", 109: "RGBA32F",
    122: "B10G11R11F", 76: "R16F", 83: "RG16F", 100: "R32F", 103: "RG32F", 9: "R8", 16: "RG8",
    124: "Depth16", 125: "Depth24", 126: "Depth32F", 129: "Depth24 Stencil8", 130: "Depth32F Stencil8",
}


def game_stem(exe):
    return re.sub(r"[^A-Za-z0-9._-]", "_", os.path.splitext(os.path.basename(exe))[0].lower()) or "game"


def read_steps(folder):
    """Pictures of a capture folder in drawing order: [(pass, kind, index, w, h, fmt, path)]."""
    steps = []
    try:
        names = os.listdir(folder)
    except OSError:
        return steps
    for name in names:
        m = NAME_RE.match(name)
        if not m:
            continue
        depth = m.group(2) is None
        steps.append({"pass": int(m.group(1)), "depth": depth, "index": int(m.group(3) if depth else m.group(2)),
                      "w": int(m.group(4)), "h": int(m.group(5)), "fmt": int(m.group(6)),
                      "path": os.path.join(folder, name)})
    # Within a pass: depth first (it is filled while drawing), then colour targets in order.
    steps.sort(key=lambda s: (s["pass"], 0 if s["depth"] else 1, s["index"]))
    return steps


def describe(step):
    fmt = VK_FORMAT_NAMES.get(step["fmt"], "format %d" % step["fmt"])
    what = ("Depth" + (" layer %d" % step["index"] if step["index"] else "")) if step["depth"] else \
        ("Colour target %d" % step["index"])
    return what, "%d×%d · %s" % (step["w"], step["h"], fmt)


def guess_role(step, frame_w, frame_h):
    """A friendly hint about what a target probably is (heuristic; engines differ)."""
    w, h, fmt = step["w"], step["h"], step["fmt"]
    if step["depth"]:
        if w == h or (frame_w and w != frame_w and h != frame_h):
            return "shadow map?"
        return "scene depth"
    if frame_w and (w, h) == (frame_w, frame_h):
        if fmt in (37, 43, 44, 50, 64) and step["index"] == 0:
            return "final image / UI?"
        return "full-resolution pass"
    if frame_w and w < frame_w // 3:
        return "small buffer (blur, bloom, exposure?)"
    if frame_w and w < frame_w:
        return "lower-resolution pass (upscaler input?)"
    return ""


def texture_for(path):
    try:
        return Gdk.Texture.new_for_pixbuf(GdkPixbuf.Pixbuf.new_from_file(path))
    except GLib.Error:
        return None


class FrameWindow(Adw.Window):
    def __init__(self, app, path, name, pid, exe):
        super().__init__(title="Frame breakdown — %s" % name, default_width=1100, default_height=760)
        self.set_transient_for(app.window)
        self.app, self.name, self.pid, self.exe = app, name, pid, exe
        self.client = MemClient(path)
        self.steps = []
        self.current = 0
        self.playing = None
        self.folder = None
        self.wait_started = 0.0

        view = Adw.ToolbarView()
        header = Adw.HeaderBar()
        self.capture_button = Gtk.Button(label="Capture frame")
        self.capture_button.add_css_class("suggested-action")
        self.capture_button.set_tooltip_text("Record the next frame the game draws, pass by pass")
        self.capture_button.connect("clicked", self._capture)
        header.pack_start(self.capture_button)
        self.open_button = Gtk.Button(icon_name="folder-open-symbolic", tooltip_text="Open the capture folder")
        self.open_button.connect("clicked", self._open_folder)
        self.open_button.set_sensitive(False)
        header.pack_end(self.open_button)
        self.depth_toggle = Gtk.ToggleButton(label="Depth", active=True, tooltip_text="Show depth buffers and shadow maps")
        self.depth_toggle.connect("toggled", lambda _b: self._show_steps())
        header.pack_end(self.depth_toggle)
        view.add_top_bar(header)

        self.stack = Gtk.Stack(transition_type=Gtk.StackTransitionType.CROSSFADE)
        self.status = Adw.StatusPage(icon_name="camera-photo-symbolic", title="See how a frame is drawn",
                                     description="Press “Capture frame” while something interesting is on screen. "
                                                 "Beer records every render pass of the next frame: shadow maps, "
                                                 "geometry buffers, lighting, effects and the final image.\n\n"
                                                 "Works with D3D12 games. The game hitches for a moment while "
                                                 "the frame is recorded.")
        self.stack.add_named(self.status, "status")

        # Grid of every step.
        self.flow = Gtk.FlowBox(valign=Gtk.Align.START, max_children_per_line=6, min_children_per_line=2,
                                selection_mode=Gtk.SelectionMode.NONE, homogeneous=True,
                                row_spacing=12, column_spacing=12, margin_top=12, margin_bottom=12,
                                margin_start=12, margin_end=12)
        self.flow.connect("child-activated", lambda _f, child: self._open_step(child.get_index()))
        grid_scroll = Gtk.ScrolledWindow(hscrollbar_policy=Gtk.PolicyType.NEVER, vexpand=True)
        grid_scroll.set_child(self.flow)
        self.summary = Gtk.Label(xalign=0, margin_start=14, margin_top=8, wrap=True)
        self.summary.add_css_class("dim-label")
        grid_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        grid_box.append(self.summary)
        grid_box.append(grid_scroll)
        self.stack.add_named(grid_box, "grid")

        # One step, large, with stepping and playback.
        step_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8, margin_top=8, margin_bottom=12,
                           margin_start=12, margin_end=12)
        self.picture = Gtk.Picture(content_fit=Gtk.ContentFit.CONTAIN, vexpand=True, hexpand=True)
        self.step_title = Gtk.Label(xalign=0)
        self.step_title.add_css_class("title-3")
        self.step_info = Gtk.Label(xalign=0)
        self.step_info.add_css_class("dim-label")
        controls = Gtk.Box(spacing=6, halign=Gtk.Align.CENTER)
        back_to_grid = Gtk.Button(icon_name="view-grid-symbolic", tooltip_text="All steps")
        back_to_grid.connect("clicked", lambda _b: self._stop_play() or self.stack.set_visible_child_name("grid"))
        prev_button = Gtk.Button(icon_name="go-previous-symbolic", tooltip_text="Previous step (←)")
        prev_button.connect("clicked", lambda _b: self._step(-1))
        self.play_button = Gtk.Button(icon_name="media-playback-start-symbolic", tooltip_text="Play the frame (space)")
        self.play_button.connect("clicked", lambda _b: self._toggle_play())
        next_button = Gtk.Button(icon_name="go-next-symbolic", tooltip_text="Next step (→)")
        next_button.connect("clicked", lambda _b: self._step(1))
        self.scrub = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 0, 1, 1)
        self.scrub.set_hexpand(True)
        self.scrub.set_draw_value(False)
        self.scrub.connect("value-changed", self._on_scrub)
        for widget in (back_to_grid, prev_button, self.play_button, next_button):
            controls.append(widget)
        step_box.append(self.step_title)
        step_box.append(self.step_info)
        step_box.append(self.picture)
        step_box.append(self.scrub)
        step_box.append(controls)
        self.stack.add_named(step_box, "step")

        self.toasts = Adw.ToastOverlay()
        self.toasts.set_child(self.stack)
        view.set_content(self.toasts)
        self.set_content(view)

        keys = Gtk.EventControllerKey()
        keys.connect("key-pressed", self._on_key)
        self.add_controller(keys)
        self.connect("close-request", self._on_close)
        self._clear()                 # leftovers from an earlier window (e.g. the manager was killed)

    # ---- capture ----
    def _clear(self, keep=None):
        """Deletes this game's capture folders except keep."""
        stem = game_stem(self.exe)
        try:
            folders = [d for d in os.listdir(FRAME_DIR) if d.startswith(stem + "-")]
        except OSError:
            return
        for d in folders:
            full = os.path.join(FRAME_DIR, d)
            if full != keep:
                shutil.rmtree(full, ignore_errors=True)

    def _on_close(self, _window):
        self._stop_play()
        self.picture.set_paintable(None)
        self._clear()
        return False

    def _capture(self, _button):
        folder = os.path.join(FRAME_DIR, "%s-%s" % (game_stem(self.exe), time.strftime("%Y%m%d-%H%M%S")))
        try:
            os.makedirs(folder, exist_ok=True)
        except OSError as e:
            self.toasts.add_toast(Adw.Toast(title="Cannot create %s: %s" % (folder, e.strerror)))
            return
        self.capture_button.set_sensitive(False)
        self.capture_button.set_label("Recording…")
        self.folder = folder
        self.client.ask({"cmd": "gfx_frame_capture", "dir": folder}, self._capture_started)

    def _capture_started(self, reply):
        if not reply or not reply.get("ok"):
            self._capture_finished()
            shutil.rmtree(self.folder, ignore_errors=True)
            self.toasts.add_toast(Adw.Toast(title="Capture failed: %s" % ((reply or {}).get("error") or "no reply")))
            return False
        self.wait_started = time.monotonic()
        GLib.timeout_add(300, self._poll)
        return False

    def _poll(self):
        if os.path.exists(os.path.join(self.folder, "done")):
            self._capture_finished()
            self._clear(keep=self.folder)
            self._load(self.folder)
            return False
        if time.monotonic() - self.wait_started > WAIT_SECONDS:
            self._capture_finished()
            shutil.rmtree(self.folder, ignore_errors=True)
            self.toasts.add_toast(Adw.Toast(title="No frame was recorded. Is the game drawing with D3D12, "
                                                  "and not paused in a menu that stops rendering?", timeout=6))
            return False
        return True

    def _capture_finished(self):
        self.capture_button.set_sensitive(True)
        self.capture_button.set_label("Capture frame")

    # ---- showing ----
    def _load(self, folder):
        self.folder = folder
        self.open_button.set_sensitive(True)
        self.all_steps = read_steps(folder)
        if not self.all_steps:
            self.status.set_title("Nothing was drawn in render passes")
            self.status.set_description("The captured frame had no render pass Beer could save (for example a frame "
                                        "drawn only with compute shaders, or targets in formats it cannot show yet). "
                                        "Try again in another scene.")
            self.stack.set_visible_child_name("status")
            return
        self._show_steps()

    def _show_steps(self):
        if not getattr(self, "all_steps", None):
            return
        show_depth = self.depth_toggle.get_active()
        self.steps = [s for s in self.all_steps if show_depth or not s["depth"]]
        colour = [s for s in self.all_steps if not s["depth"]]
        biggest = max(colour, key=lambda s: s["w"] * s["h"]) if colour else None
        self.frame_w, self.frame_h = (biggest["w"], biggest["h"]) if biggest else (0, 0)
        passes = len({s["pass"] for s in self.all_steps})
        self.summary.set_label("%d render passes, %d pictures · %s · click a picture to step through the frame" % (
            passes, len(self.all_steps), os.path.basename(self.folder)))
        while (child := self.flow.get_first_child()) is not None:
            self.flow.remove(child)
        for number, step in enumerate(self.steps, 1):
            self.flow.append(self._card(number, step))
        self.scrub.set_range(0, max(1, len(self.steps) - 1))
        self.stack.set_visible_child_name("grid")

    def _card(self, number, step):
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)
        picture = Gtk.Picture(content_fit=Gtk.ContentFit.CONTAIN, can_shrink=True, height_request=120)
        texture = texture_for(step["path"])
        if texture:
            picture.set_paintable(texture)
        frame = Gtk.Frame(child=picture)
        what, info = describe(step)
        title = Gtk.Label(label="%d. Pass %d · %s" % (number, step["pass"], what), xalign=0,
                          ellipsize=3)  # Pango.EllipsizeMode.END
        title.add_css_class("heading")
        role = guess_role(step, self.frame_w, self.frame_h)
        sub = Gtk.Label(label=info + (" · " + role if role else ""), xalign=0, ellipsize=3)
        sub.add_css_class("caption")
        sub.add_css_class("dim-label")
        box.append(frame)
        box.append(title)
        box.append(sub)
        return box

    def _open_step(self, index):
        self.current = max(0, min(index, len(self.steps) - 1))
        self.stack.set_visible_child_name("step")
        self._show_current()

    def _show_current(self):
        if not self.steps:
            return
        step = self.steps[self.current]
        texture = texture_for(step["path"])
        self.picture.set_paintable(texture)
        what, info = describe(step)
        role = guess_role(step, self.frame_w, self.frame_h)
        self.step_title.set_label("Step %d of %d · Pass %d · %s" % (self.current + 1, len(self.steps), step["pass"], what))
        self.step_info.set_label(info + (" · " + role if role else "") + " · " + os.path.basename(step["path"]))
        if int(self.scrub.get_value()) != self.current:
            self.scrub.set_value(self.current)

    def _step(self, delta):
        if self.steps:
            self.current = (self.current + delta) % len(self.steps)
            self._show_current()

    def _on_scrub(self, scale):
        index = int(round(scale.get_value()))
        if index != self.current and self.steps:
            self.current = index
            self._show_current()

    def _toggle_play(self):
        if self.playing:
            self._stop_play()
            return
        if self.stack.get_visible_child_name() != "step":
            self._open_step(0)
        self.play_button.set_icon_name("media-playback-pause-symbolic")
        self.playing = GLib.timeout_add(450, self._play_tick)

    def _play_tick(self):
        if self.current >= len(self.steps) - 1:
            self._stop_play()
            return False
        self._step(1)
        return True

    def _stop_play(self):
        if self.playing:
            GLib.source_remove(self.playing)
            self.playing = None
        self.play_button.set_icon_name("media-playback-start-symbolic")
        return None

    def _on_key(self, _controller, keyval, _code, _state):
        if self.stack.get_visible_child_name() != "step":
            return False
        if keyval in (Gdk.KEY_Right, Gdk.KEY_Down):
            self._step(1)
        elif keyval in (Gdk.KEY_Left, Gdk.KEY_Up):
            self._step(-1)
        elif keyval == Gdk.KEY_space:
            self._toggle_play()
        elif keyval == Gdk.KEY_Escape:
            self._stop_play()
            self.stack.set_visible_child_name("grid")
        else:
            return False
        return True

    def _open_folder(self, _button):
        if self.folder:
            Gio.AppInfo.launch_default_for_uri("file://" + self.folder, None)
