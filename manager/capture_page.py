"""Capture page: record running games to a video file or stream them live (RTMP: Twitch, YouTube, any server).

The loader does the work (platform/beer_capture.c): it scales the finished picture on the GPU (effects applied, overlay
left out), takes the game's own audio, and feeds both to an ffmpeg child process. This page picks the settings, sends
capture_start / capture_stop to a running game and shows each game's capture status.

Settings live in ~/.beer/capture.json (mode 0600: it holds the stream key).
"""
import json
import os
import shutil
import subprocess

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Adw, Gio, GLib, Gtk  # noqa: E402

import storage  # noqa: E402

CAPTURE_FILE = storage.path("capture.json")

RESOLUTIONS = [("Game's own resolution", 0), ("2160p", 2160), ("1440p", 1440), ("1080p", 1080), ("900p", 900),
               ("720p", 720), ("480p", 480)]
FRAME_RATES = [24, 30, 48, 60, 90, 120, 144]
ENCODERS = [("Automatic", "auto"), ("NVIDIA (NVENC)", "nvenc"), ("AMD / Intel (VA-API)", "vaapi"), ("CPU (software)", "cpu")]
CODECS = [("H.264 (plays everywhere)", "h264"), ("HEVC / H.265 (smaller files)", "hevc"), ("AV1 (smallest, newest players)", "av1")]
CONTAINERS = [("MP4", "mp4"), ("MKV", "mkv")]
SERVICES = [("Twitch", "rtmp://live.twitch.tv/app/"), ("YouTube", "rtmp://a.rtmp.youtube.com/live2/"),
            ("Custom server", "")]
ENCODER_NAMES = {
    ("nvenc", "h264"): "h264_nvenc", ("nvenc", "hevc"): "hevc_nvenc", ("nvenc", "av1"): "av1_nvenc",
    ("vaapi", "h264"): "h264_vaapi", ("vaapi", "hevc"): "hevc_vaapi", ("vaapi", "av1"): "av1_vaapi",
    ("cpu", "h264"): "libx264", ("cpu", "hevc"): "libx265", ("cpu", "av1"): "libsvtav1",
}

DEFAULT = {
    "height": 1080, "fps": 60, "encoder": "auto", "codec": "h264",
    "record_bitrate": 20000, "stream_bitrate": 6000,
    "container": "mp4", "folder": os.path.expanduser("~/Videos/Beer"),
    "audio": True, "game_time": False,
    "service": 0, "server": "", "key": "", "save_copy": False,
}


def load_config():
    cfg = dict(DEFAULT)
    try:
        with open(CAPTURE_FILE) as f:
            cfg.update(json.load(f))
    except (OSError, ValueError):
        pass
    return cfg


def save_config(cfg):
    os.makedirs(os.path.dirname(CAPTURE_FILE), exist_ok=True)
    tmp = CAPTURE_FILE + ".tmp"
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        json.dump(cfg, f, indent=2)
    os.replace(tmp, CAPTURE_FILE)


_ENCODERS_CACHE = None


def available_encoders(ffmpeg):
    """Names ffmpeg lists under -encoders (empty when ffmpeg is missing)."""
    global _ENCODERS_CACHE
    if _ENCODERS_CACHE is None:
        names = set()
        if ffmpeg:
            try:
                out = subprocess.run([ffmpeg, "-hide_banner", "-encoders"], capture_output=True, text=True, timeout=5).stdout
                for line in out.splitlines():
                    parts = line.split()
                    if len(parts) >= 2 and len(parts[0]) == 6 and parts[0][0] in "VAS":
                        names.add(parts[1])
            except (OSError, subprocess.SubprocessError):
                pass
        _ENCODERS_CACHE = names
    return _ENCODERS_CACHE


def resolve_encoder(family, codec, available):
    """(ffmpeg encoder name, None) or (None, reason)."""
    if family == "auto":
        nvidia = os.path.exists("/proc/driver/nvidia/version")
        render = os.path.exists("/dev/dri/renderD128")
        order = (["nvenc"] if nvidia else []) + (["vaapi"] if render and not nvidia else []) + ["cpu"]
        for f in order:
            name = ENCODER_NAMES[(f, codec)]
            if name in available:
                return name, None
        return None, "ffmpeg has no %s encoder" % codec.upper()
    name = ENCODER_NAMES[(family, codec)]
    if name not in available:
        return None, "this ffmpeg has no %s encoder" % name
    return name, None


def human_bytes(n):
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return ("%d %s" % (n, unit)) if unit == "B" else ("%.1f %s" % (n, unit))
        n /= 1024.0
    return ""


def clock(seconds):
    seconds = int(seconds)
    return "%d:%02d:%02d" % (seconds // 3600, seconds // 60 % 60, seconds % 60) if seconds >= 3600 else "%d:%02d" % (seconds // 60, seconds % 60)


class CapturePage(Gtk.Box):
    def __init__(self, app):
        super().__init__(orientation=Gtk.Orientation.VERTICAL)
        self.app = app
        self.cfg = load_config()
        self.ffmpeg = shutil.which("ffmpeg")
        self.rows = {}          # pid -> (row, record button, stream button, stop button)
        self.last_state = {}    # pid -> state seen on the previous poll
        self._building = True

        clamp = Adw.Clamp(maximum_size=760, margin_top=18, margin_bottom=18, margin_start=12, margin_end=12)
        body = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=18)
        clamp.set_child(body)
        scroll = Gtk.ScrolledWindow(vexpand=True)
        scroll.set_child(clamp)
        self.append(scroll)

        # ---- running games
        self.games = Adw.PreferencesGroup(title="Running games",
                                          description="Record or stream a game with the settings below. The picture is taken "
                                                      "before Beer's overlay, with the Display page's effects.")
        folder = Gtk.Button(icon_name="folder-open-symbolic", valign=Gtk.Align.CENTER, tooltip_text="Open the video folder")
        folder.add_css_class("flat")
        folder.connect("clicked", lambda _b: self._open_folder())
        self.games.set_header_suffix(folder)
        self.empty = Adw.ActionRow(title="Nothing running", subtitle="Start a game from the Library")
        self.games.add(self.empty)
        if not self.ffmpeg:
            self.games.add(Adw.ActionRow(title="ffmpeg is not installed",
                                         subtitle="Recording and streaming use ffmpeg; install it with your package manager."))
        body.append(self.games)

        # ---- video
        video = Adw.PreferencesGroup(title="Video")
        self.res = Adw.ComboRow(title="Resolution", subtitle="Scaled on the GPU; never above the game's own",
                                model=Gtk.StringList.new([r[0] for r in RESOLUTIONS]))
        self.res.set_selected(next((i for i, r in enumerate(RESOLUTIONS) if r[1] == self.cfg["height"]), 0))
        self.res.connect("notify::selected", self._changed)
        video.add(self.res)
        self.fps = Adw.ComboRow(title="Frame rate", subtitle="Frames per second in the video",
                                model=Gtk.StringList.new(["%d fps" % f for f in FRAME_RATES]))
        self.fps.set_selected(FRAME_RATES.index(self.cfg["fps"]) if self.cfg["fps"] in FRAME_RATES else 3)
        self.fps.connect("notify::selected", self._changed)
        video.add(self.fps)
        self.encoder = Adw.ComboRow(title="Encoder", model=Gtk.StringList.new([e[0] for e in ENCODERS]))
        self.encoder.set_selected(next((i for i, e in enumerate(ENCODERS) if e[1] == self.cfg["encoder"]), 0))
        self.encoder.connect("notify::selected", self._changed)
        video.add(self.encoder)
        body.append(video)

        # ---- recording
        record = Adw.PreferencesGroup(title="Recording")
        self.rec_bitrate = self._bitrate_row("Bitrate", "kbit/s. 20 000 suits 1080p60; double it for 1440p, ×4 for 4K",
                                             self.cfg["record_bitrate"], "record_bitrate")
        record.add(self.rec_bitrate)
        self.codec = Adw.ComboRow(title="Codec", model=Gtk.StringList.new([c[0] for c in CODECS]))
        self.codec.set_selected(next((i for i, c in enumerate(CODECS) if c[1] == self.cfg["codec"]), 0))
        self.codec.connect("notify::selected", self._changed)
        record.add(self.codec)
        self.container = Adw.ComboRow(title="File type", subtitle="MP4 and MKV both survive a crash mid-recording",
                                      model=Gtk.StringList.new([c[0] for c in CONTAINERS]))
        self.container.set_selected(next((i for i, c in enumerate(CONTAINERS) if c[1] == self.cfg["container"]), 0))
        self.container.connect("notify::selected", self._changed)
        record.add(self.container)
        self.folder = Adw.EntryRow(title="Folder")
        self.folder.set_text(self.cfg["folder"])
        self.folder.connect("changed", self._changed)
        pick = Gtk.Button(icon_name="folder-open-symbolic", valign=Gtk.Align.CENTER, tooltip_text="Choose the folder")
        pick.add_css_class("flat")
        pick.connect("clicked", lambda _b: self._pick_folder())
        self.folder.add_suffix(pick)
        record.add(self.folder)
        self.game_time = Adw.SwitchRow(title="Record in normal time when the game is slowed",
                                       subtitle="Frames follow the game's clock: at 0.25× game speed, a minute of play "
                                                "becomes 15 seconds of normal-speed video (smooth even when the game "
                                                "cannot run full speed). No sound: the game's audio does not slow down.")
        self.game_time.set_active(self.cfg["game_time"])
        self.game_time.connect("notify::active", self._changed)
        record.add(self.game_time)
        body.append(record)

        # ---- streaming
        stream = Adw.PreferencesGroup(title="Streaming", description="Live to Twitch, YouTube or any RTMP server. Streams use H.264.")
        self.str_bitrate = self._bitrate_row("Bitrate", "kbit/s. Twitch allows up to 6 000; YouTube 1080p60 takes up to 12 000",
                                             self.cfg["stream_bitrate"], "stream_bitrate")
        stream.add(self.str_bitrate)
        self.service = Adw.ComboRow(title="Service", model=Gtk.StringList.new([s[0] for s in SERVICES]))
        self.service.set_selected(min(int(self.cfg["service"]), len(SERVICES) - 1))
        self.service.connect("notify::selected", self._changed)
        stream.add(self.service)
        self.server = Adw.EntryRow(title="Server address (rtmp://…)")
        self.server.set_text(self.cfg["server"])
        self.server.connect("changed", self._changed)
        stream.add(self.server)
        self.key = Adw.PasswordEntryRow(title="Stream key")
        self.key.set_text(self.cfg["key"])
        self.key.connect("changed", self._changed)
        stream.add(self.key)
        self.save_copy = Adw.SwitchRow(title="Also save a copy", subtitle="Writes the same video to the recording folder")
        self.save_copy.set_active(self.cfg["save_copy"])
        self.save_copy.connect("notify::active", self._changed)
        stream.add(self.save_copy)
        body.append(stream)

        # ---- sound
        sound = Adw.PreferencesGroup(title="Sound")
        self.audio = Adw.SwitchRow(title="Game sound", subtitle="Only the game's own audio (no other apps, no microphone)")
        self.audio.set_active(self.cfg["audio"])
        self.audio.connect("notify::active", self._changed)
        sound.add(self.audio)
        body.append(sound)

        self._building = False
        self._sync_sensitivity()
        GLib.timeout_add(1000, self._poll)

    # ------------------------------------------------------------------ settings
    def _bitrate_row(self, title, subtitle, value, key):
        row = Adw.SpinRow(title=title, subtitle=subtitle,
                          adjustment=Gtk.Adjustment(value=value, lower=500, upper=200000, step_increment=500, page_increment=5000))
        row.connect("notify::value", lambda r, _p: self._set(key, int(r.get_value())))
        return row

    def _set(self, key, value):
        if self._building:
            return
        self.cfg[key] = value
        save_config(self.cfg)

    def _changed(self, *_args):
        if self._building:
            return
        self.cfg.update(
            height=RESOLUTIONS[self.res.get_selected()][1], fps=FRAME_RATES[self.fps.get_selected()],
            encoder=ENCODERS[self.encoder.get_selected()][1], codec=CODECS[self.codec.get_selected()][1],
            container=CONTAINERS[self.container.get_selected()][1], folder=self.folder.get_text().strip(),
            game_time=self.game_time.get_active(), audio=self.audio.get_active(),
            service=self.service.get_selected(), server=self.server.get_text().strip(), key=self.key.get_text().strip(),
            save_copy=self.save_copy.get_active())
        save_config(self.cfg)
        self._sync_sensitivity()

    def _sync_sensitivity(self):
        custom = SERVICES[self.service.get_selected()][1] == ""
        self.server.set_visible(custom)
        self.audio.set_sensitive(not self.game_time.get_active())
        self.audio.set_subtitle("Off while recording in normal time (the game's audio does not slow down)"
                                if self.game_time.get_active() else "Only the game's own audio (no other apps, no microphone)")
        available = available_encoders(self.ffmpeg)
        name, problem = resolve_encoder(self.cfg["encoder"], self.cfg["codec"], available)
        self.encoder.set_subtitle(("Uses %s" % name) if name else (problem or ""))

    def _pick_folder(self):
        dialog = Gtk.FileDialog(title="Video folder")
        if os.path.isdir(self.folder.get_text()):
            dialog.set_initial_folder(Gio.File.new_for_path(self.folder.get_text()))

        def done(d, result):
            try:
                f = d.select_folder_finish(result)
            except GLib.Error:
                return
            if f and f.get_path():
                self.folder.set_text(f.get_path())
        dialog.select_folder(self.app.window, None, done)

    def _open_folder(self):
        folder = self.cfg["folder"] or os.path.expanduser("~/Videos/Beer")
        os.makedirs(folder, exist_ok=True)
        Gio.AppInfo.launch_default_for_uri("file://" + folder, None)

    # ------------------------------------------------------------------ start / stop
    def _start_message(self, mode):
        cfg = self.cfg
        streaming = mode == "stream"
        codec = "h264" if streaming else cfg["codec"]
        name, problem = resolve_encoder(cfg["encoder"], codec, available_encoders(self.ffmpeg))
        if not self.ffmpeg:
            return None, "ffmpeg is not installed"
        if not name:
            return None, problem
        message = {"cmd": "capture_start", "mode": mode, "encoder": name, "fps": cfg["fps"], "height": cfg["height"],
                   "bitrate": cfg["stream_bitrate"] if streaming else cfg["record_bitrate"],
                   "audio": bool(cfg["audio"]), "game_time": bool(cfg["game_time"]) and not streaming,
                   "container": cfg["container"], "folder": cfg["folder"], "ffmpeg": self.ffmpeg}
        if streaming:
            server = SERVICES[cfg["service"]][1] or cfg["server"]
            if not server.startswith(("rtmp://", "rtmps://")):
                return None, "Set the server address (rtmp://…) in Streaming"
            if not cfg["key"]:
                return None, "Paste your stream key in Streaming"
            message["url"] = server.rstrip("/") + "/" + cfg["key"]
            message["save_copy"] = bool(cfg["save_copy"])
        return message, None

    def _start(self, card, mode):
        message, problem = self._start_message(mode)
        if not message:
            self.app.toast(problem)
            return
        reply = card.control.request(message)
        if not reply or not reply.get("ok"):
            self.app.toast("Could not start: %s" % ((reply or {}).get("error") or "the game did not answer"))
            return
        self.app.toast("%s %s" % ("Streaming" if mode == "stream" else "Recording", card.name))
        self._poll()

    def _stop(self, card):
        card.control.request({"cmd": "capture_stop"})
        self._poll()

    # ------------------------------------------------------------------ status
    def _row_for(self, pid, card):
        if pid in self.rows:
            return self.rows[pid]
        row = Adw.ActionRow(title=GLib.markup_escape_text(card.name))
        rec = Gtk.Button(label="Record", valign=Gtk.Align.CENTER)
        rec.add_css_class("suggested-action")
        rec.connect("clicked", lambda _b: self._start(card, "record"))
        live = Gtk.Button(label="Stream", valign=Gtk.Align.CENTER)
        live.connect("clicked", lambda _b: self._start(card, "stream"))
        stop = Gtk.Button(label="Stop", valign=Gtk.Align.CENTER)
        stop.add_css_class("destructive-action")
        stop.connect("clicked", lambda _b: self._stop(card))
        for b in (rec, live, stop):
            row.add_suffix(b)
        self.games.add(row)
        self.rows[pid] = (row, rec, live, stop)
        return self.rows[pid]

    def _poll(self):
        cards = dict(self.app.cards)
        for pid in list(self.rows):
            if pid not in cards:
                self.games.remove(self.rows.pop(pid)[0])
                self.last_state.pop(pid, None)
        self.empty.set_visible(not cards)
        for pid, card in cards.items():
            row, rec, live, stop = self._row_for(pid, card)
            status = card.control.request({"cmd": "capture_status"}) or {}
            state = status.get("state", "idle")
            busy = state in ("running", "finishing")
            rec.set_visible(not busy)
            live.set_visible(not busy)
            stop.set_visible(state == "running")
            row.set_subtitle(self._describe(status))
            previous = self.last_state.get(pid)
            if previous in ("running", "finishing") and state == "idle":
                if status.get("error"):
                    self.app.toast("%s: %s" % (card.name, status["error"]))
                elif status.get("path"):
                    self.app.toast("Saved %s" % os.path.basename(status["path"]))
            self.last_state[pid] = state
        return True

    @staticmethod
    def _describe(status):
        if not status.get("ok"):
            return "Not responding"
        state = status.get("state")
        what = "Streaming" if status.get("mode") == "stream" else "Recording"
        size = "%dx%d %d fps" % (status.get("width", 0), status.get("height", 0), status.get("fps", 0))
        if state == "running":
            parts = ["● %s %s" % (what, clock(float(status.get("seconds", 0))))]
            if status.get("width"):
                parts.append(size)
            if status.get("bytes"):
                parts.append(human_bytes(status["bytes"]))
            if status.get("game_time"):
                parts.append("game time")
            late = int(status.get("late", 0))
            if late > 30:
                parts.append("encoder behind (%d late)" % late)
            return " · ".join(parts)
        if state == "finishing":
            return "Finishing the %s…" % ("stream" if status.get("mode") == "stream" else "file")
        if status.get("error"):
            return "Stopped: %s" % status["error"]
        if status.get("frames"):
            return "Last: %s, %s%s" % (clock(float(status.get("seconds", 0))), size,
                                       (" · " + os.path.basename(status["path"])) if status.get("path") else "")
        return "Ready"
