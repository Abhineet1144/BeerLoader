"""beer_mod: write mods for games running under Beer.

A mod is a small Python program the Beer Manager starts next to a game. It talks to the game through the game's
control socket (the same one the manager uses), so it can read and write game memory, follow pointer paths, find
objects, draw boxes on the in-game overlay, react to hotkeys and change the game speed. Mods run in their own process:
a mod that crashes never takes the game down.

Minimal mod::

    from beer_mod import Mod

    mod = Mod()

    @mod.every_frame
    def show(game):
        hp = game.read_f32(game.resolve(0x1234560, [0x10, 0x88], module="game.exe"))
        mod.box("hp").text = f"HP {hp:.0f}"

    mod.run()

See docs/mod-api/ in the Beer tree for the full reference.
"""
import glob
import json
import os
import socket
import struct
import sys
import threading
import time

__all__ = ["Mod", "Game", "Box", "GameGone", "ModError", "PROTOCOL_VERSION"]

PROTOCOL_VERSION = 1

_FORMATS = {  # type name -> (struct format, size)
    "i8": ("<b", 1), "u8": ("<B", 1), "i16": ("<h", 2), "u16": ("<H", 2), "i32": ("<i", 4), "u32": ("<I", 4),
    "i64": ("<q", 8), "u64": ("<Q", 8), "f32": ("<f", 4), "f64": ("<d", 8),
}


class ModError(Exception):
    """A command was refused by the game (the message says why)."""


class GameGone(Exception):
    """The game exited (or its control socket closed)."""


class _Connection:
    """One JSON-lines connection to a game's control socket (thread-safe)."""

    def __init__(self, path):
        self.path = path
        self.lock = threading.Lock()
        self.sock = None
        self.buffer = b""

    def request(self, message, timeout=30.0):
        with self.lock:
            for attempt in (0, 1):
                try:
                    if self.sock is None:
                        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                        s.settimeout(timeout)
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
                    if self.sock:
                        try:
                            self.sock.close()
                        except OSError:
                            pass
                    self.sock = None
                    if attempt or not os.path.exists(self.path):
                        raise GameGone(self.path)
        raise GameGone(self.path)


def _addr(value):
    """Addresses travel as hex strings."""
    return value if isinstance(value, str) else hex(int(value))


def _find_socket(pid=None):
    explicit = os.environ.get("BEER_SOCKET")
    if explicit:
        return explicit
    runtime = os.environ.get("XDG_RUNTIME_DIR")
    folder = os.path.join(runtime, "beer") if runtime else "/tmp/beer-%d" % os.getuid()
    if pid:
        return os.path.join(folder, "%d.sock" % pid)
    sockets = [p for p in glob.glob(os.path.join(folder, "*.sock")) if os.path.basename(p)[:-5].isdigit()]
    if not sockets:
        raise GameGone("no running Beer game (no control socket in %s)" % folder)
    return max(sockets, key=os.path.getmtime)   # the newest game


class Game:
    """The running game. Every method is a thin wrapper around one control command (docs/mod-api/protocol.md)."""

    def __init__(self, connection, owner):
        self._c = connection
        self.owner = owner

    # -- raw access -------------------------------------------------------------------------------------------
    def command(self, cmd, **args):
        """Sends one control command and returns its reply (a dict). Raises ModError when it fails."""
        reply = self._c.request(dict(cmd=cmd, **args))
        if not reply or not reply.get("ok", False):
            raise ModError("%s: %s" % (cmd, (reply or {}).get("error", "no reply")))
        return reply

    # -- game information -------------------------------------------------------------------------------------
    def info(self):
        """{'pid', 'exe', 'protocol'} of the game."""
        return self.command("hello")

    def status(self):
        """Live status: fps, frames (presented so far), cpu_percent, rss_mb, threads, time_scale, fps_limit, paused,
        max_frame_ms, hitches, memguard."""
        return self.command("status")

    def modules(self):
        """{name: (base, size)} of the executable and every loaded DLL."""
        return {m["name"].lower(): (int(m["base"], 16), m["size"]) for m in self.command("mem_modules")["modules"]}

    def module_base(self, name):
        """Base address of one module (case-insensitive), or None."""
        entry = self.modules().get(name.lower())
        return entry[0] if entry else None

    # -- memory -----------------------------------------------------------------------------------------------
    def read_bytes(self, addr, length):
        """Up to 65536 bytes at addr (8192 on loaders before 2026-11-16), or None when unreadable."""
        try:
            return bytes.fromhex(self.command("mem_read", addr=_addr(addr), len=int(length))["hex"])
        except ModError:
            return None

    def read(self, addr, type_name):
        """One value of type i8/u8/i16/u16/i32/u32/i64/u64/f32/f64, or None when unreadable."""
        fmt, size = _FORMATS[type_name]
        data = self.read_bytes(addr, size)
        return struct.unpack(fmt, data)[0] if data and len(data) == size else None

    def read_i32(self, addr): return self.read(addr, "i32")
    def read_u32(self, addr): return self.read(addr, "u32")
    def read_i64(self, addr): return self.read(addr, "i64")
    def read_u64(self, addr): return self.read(addr, "u64")
    def read_f32(self, addr): return self.read(addr, "f32")
    def read_f64(self, addr): return self.read(addr, "f64")
    def read_ptr(self, addr): return self.read(addr, "u64")

    def read_string(self, addr, max_length=256, wide=False):
        """A NUL-terminated string (UTF-8, or UTF-16 with wide=True)."""
        data = self.read_bytes(addr, max_length * (2 if wide else 1))
        if not data:
            return None
        text = data.decode("utf-16-le" if wide else "utf-8", "replace")
        return text.split("\0", 1)[0]

    def read_many(self, reads):
        """Many reads in one round trip: reads = [(addr, type_name), ...]; returns values (None where unreadable).
        Use this to sample many objects every frame."""
        values, batch = [], []
        for start in range(0, len(reads), 512):
            chunk = reads[start:start + 512]
            spec = ",".join("%s:%d" % (_addr(a), _FORMATS[t][1]) for a, t in chunk)
            batch = self.command("mem_read_batch", reads=spec)["data"]
            for (a, t), h in zip(chunk, batch):
                fmt, size = _FORMATS[t]
                values.append(struct.unpack(fmt, bytes.fromhex(h))[0] if h else None)
        return values

    def write_bytes(self, addr, data):
        """Writes up to 1024 bytes."""
        return self.command("mem_write", addr=_addr(addr), hex=bytes(data).hex())["written"]

    def write(self, addr, type_name, value):
        fmt, _ = _FORMATS[type_name]
        return self.write_bytes(addr, struct.pack(fmt, value))

    def write_i32(self, addr, value): return self.write(addr, "i32", value)
    def write_f32(self, addr, value): return self.write(addr, "f32", value)

    def freeze(self, addr, type_name, value):
        """Keeps rewriting value at addr (about 10 times a second) until unfreeze."""
        return self.command("mem_freeze", addr=_addr(addr), type=type_name.replace("u", "i"), value=value)

    def unfreeze(self, addr):
        return self.command("mem_unfreeze", addr=_addr(addr))

    def find_pattern(self, pattern, module=None, limit=16):
        """Addresses where a byte pattern occurs ("44 88 6B 14 C7 43 18 ?? ?? ?? ??"; ?? matches any byte), in one module
        (e.g. the executable) or in all readable memory. Finds code or data by its bytes, so a mod keeps working when
        a game update moves it. Read-only memory (code) is searched too."""
        args = {"pattern": pattern, "limit": limit}
        if module:
            args["module"] = module
        return [int(a, 16) for a in self.command("mem_aob", **args).get("addrs", [])]

    def resolve(self, base, offsets=(), module=None):
        """Follows a pointer path as in Cheat Engine: addr = base (+ module base); for each offset,
        addr = *(u64 *)addr + offset. Returns the final address, or None when a pointer on the way is null or
        unreadable (the object is not loaded yet)."""
        args = {"base": _addr(base), "offsets": ",".join(hex(o) for o in offsets)}
        if module:
            args["module"] = module
        try:
            return int(self.command("mem_resolve", **args)["addr"], 16)
        except ModError:
            return None

    def regions(self):
        """Mapped memory regions: [{'start', 'end', 'perms', 'path'}]."""
        return self.command("mem_regions")["regions"]

    # -- value scans (shared with the manager's Memory window) ----------------------------------------------
    def scan(self, type_name, value=None, writable=True, cond=None):
        """Starts a scan for value (or, with value=None, an unknown-value scan). Returns the match count.

        cond=("gt", 0) keeps only values that also satisfy the condition (lt, le, gt, ge, eq)."""
        args = {"type": type_name, "writable": writable}
        if value is None:
            args["unknown"] = True
        else:
            args["value"] = value
        if cond:
            args["cond"], args["cond_value"] = cond[0], cond[1]
        return self.command("mem_scan", **args).get("count", 0)

    def scan_next(self, mode, value=None, cond=None, up_to=None, at_least=None):
        """Narrows the last scan: mode = exact/changed/unchanged/increased/decreased.

        cond as in scan(); up_to / at_least bound the size of the change for changed/increased/decreased."""
        args = {"mode": mode}
        if value is not None:
            args["value"] = value
        if cond:
            args["cond"], args["cond_value"] = cond[0], cond[1]
        if up_to is not None:
            args["up_to"] = up_to
        if at_least is not None:
            args["at_least"] = at_least
        return self.command("mem_next", **args).get("count", 0)

    def find_values(self, values, limit=16, writable=True):
        """Every 8-aligned qword equal to one of `values` (up to 256), in one pass over memory.

        Returns {value: (count, [addresses...])}; count is exact, the address list is capped at `limit` (max 256).
        Unlike scan(), this does not touch the shared scan, so it is safe to call repeatedly (e.g. to find every object
        whose first qword is a known type pointer)."""
        values = [int(v) & 0xFFFFFFFFFFFFFFFF for v in values][:256]
        if not values:
            return {}
        reply = self.command("mem_find", values=",".join("0x%x" % v for v in values), limit=limit, writable=writable)
        return {int(r["value"], 16): (r["count"], [int(a, 16) for a in r["addrs"]]) for r in reply.get("results", [])}

    def scan_results(self, limit=200, offset=0):
        """[(addr, value)] of the current scan (up to `limit`, fetched in pages of 500)."""
        found = []
        while len(found) < limit:
            page = self.command("mem_results", offset=offset + len(found), limit=min(500, limit - len(found)))
            items = page.get("results", [])
            found += [(int(r["addr"], 16), r.get("value")) for r in items]
            if not items or len(found) >= page.get("count", 0):
                break
        return found

    def scan_reset(self):
        return self.command("mem_reset")

    # -- objects ----------------------------------------------------------------------------------------------
    def object_scan(self):
        """Finds objects by their transforms and groups them by type. Returns the groups:
        [{'id', 'name', 'count', 'kind', 'vtable', ...}]. Takes a second or two."""
        return self.command("obj_scan", timeout=120)["groups"]

    def objects(self, group, limit=300):
        """Objects of one group: [{'base', 'transform', 'v', ...}] ('v': the transform values)."""
        return self.command("obj_list", group=group, limit=limit)["items"]

    def find_player(self):
        """Best guesses for the player object (needs object_scan first)."""
        return self.command("obj_find_player")["items"]

    def near(self, x, y, z, radius=50.0, limit=100):
        """Objects within radius of a position (needs object_scan first)."""
        return self.command("obj_near", x=x, y=y, z=z, radius=radius, limit=limit)["items"]

    # -- game control -----------------------------------------------------------------------------------------
    def set_speed(self, scale):
        """Game speed: 1.0 normal, 0.5 half speed, 2.0 double (0.05 .. 10)."""
        return self.command("set", name="time_scale", value=float(scale))

    def set_paused(self, paused):
        """Freezes the game's clocks (rendering continues)."""
        return self.command("set", name="paused", value=bool(paused))

    def set_fps_cap(self, fps):
        """Frame cap (0 = none)."""
        return self.command("set", name="fps_limit", value=int(fps))

    def press(self, vk, down=True, dik=0):
        """Delivers one key transition (Windows virtual-key code) to the game window."""
        return self.command("key", vk=int(vk), dik=int(dik), down=1 if down else 0)

    def tap(self, vk, hold=0.05):
        self.press(vk, True)
        time.sleep(hold)
        self.press(vk, False)

    def screenshot(self):
        """Saves a screenshot; returns its path."""
        return self.command("screenshot").get("path")


class Box:
    """A text box on the in-game overlay, owned by one mod. Changing a property updates the game at once."""

    def __init__(self, game, box_id, x, y, size, text_color, panel_color):
        self._game = game
        self.id = "%s:%s" % (game.owner, box_id)
        self._state = {"x": x, "y": y, "size": size, "text_color": text_color or "", "panel_color": panel_color or ""}
        self._text = ""
        self._sent = None

    def _push(self):
        payload = dict(self._state, text=self._text)
        if payload == self._sent:          # nothing changed: no round trip
            return
        self._game.command("overlay_box_set", id=self.id, owner=self._game.owner,
                           x=int(payload["x"] * 1000), y=int(payload["y"] * 1000), size=payload["size"],
                           text_color=payload["text_color"], panel_color=payload["panel_color"], text=payload["text"])
        self._sent = payload

    @property
    def text(self):
        return self._text

    @text.setter
    def text(self, value):
        """Text of the box: \\n for new lines, [a]...[/a] for the accent colour, [d] for dimmed text.
        An empty text hides the box."""
        self._text = str(value)
        self._push()

    def move(self, x, y):
        """Top-left corner as a fraction of the screen (0..1)."""
        self._state.update(x=x, y=y)
        self._push()

    def style(self, size=None, text_color=None, panel_color=None):
        """size 0 small, 1 medium, 2 large; colours as 'rrggbb'."""
        if size is not None:
            self._state["size"] = size
        if text_color is not None:
            self._state["text_color"] = text_color
        if panel_color is not None:
            self._state["panel_color"] = panel_color
        self._push()

    def remove(self):
        self._game.command("overlay_box_remove", id=self.id)
        self._sent = None


def _install_stop_handlers(mod):
    """SIGTERM (the manager's Stop) and SIGINT raise KeyboardInterrupt, and the mod's boxes and hotkeys are removed at
    exit, so a mod stopped at any point (even while it is still starting) leaves nothing behind in the game."""
    import atexit
    import signal

    def interrupt(signum, frame):
        raise KeyboardInterrupt
    for sig in (signal.SIGTERM, signal.SIGINT):
        try:
            signal.signal(sig, interrupt)
        except (ValueError, OSError):        # not the main thread
            pass
    atexit.register(lambda: mod._cleanup() if getattr(mod, "game", None) else None)


class Mod:
    """A mod: connection, settings, persistent data, overlay boxes, hotkeys and the main loop."""

    def __init__(self, name=None, pid=None):
        _install_stop_handlers(self)
        self.folder = os.environ.get("BEER_MOD_DIR") or os.path.dirname(os.path.abspath(sys.argv[0]))
        manifest = {}
        try:
            with open(os.path.join(self.folder, "mod.json")) as f:
                manifest = json.load(f)
        except (OSError, ValueError):
            pass
        self.manifest = manifest
        self.name = name or manifest.get("id") or os.path.basename(self.folder)
        self.game = Game(_Connection(_find_socket(pid)), self.name)
        self.settings = self._load_settings()
        self._boxes = {}
        self._frame_callbacks = []
        self._timers = []              # [interval, next_due, callback]
        self._hotkeys = {}
        self._stop = False
        self._exit_callbacks = []
        self._settings_callbacks = []
        self._settings_stamp = self._settings_mtime()
        self._cleaned = False
        self._data_path = os.path.join(self.folder, "data.json")

    # -- settings and data ------------------------------------------------------------------------------------
    def _load_settings(self):
        """Defaults from mod.json "settings", overridden by settings.json (written by the manager's mod panel)."""
        values = {s["key"]: s.get("default") for s in self.manifest.get("settings", []) if "key" in s}
        try:
            with open(os.path.join(self.folder, "settings.json")) as f:
                values.update(json.load(f))
        except (OSError, ValueError):
            pass
        return values

    def _settings_mtime(self):
        try:
            return os.stat(os.path.join(self.folder, "settings.json")).st_mtime_ns
        except OSError:
            return None

    def on_settings_changed(self, fn):
        """Decorator: fn(game, settings) after the user changes the mod's settings in the manager while it runs
        (mod.settings is already updated)."""
        self._settings_callbacks.append(fn)
        return fn

    def on_exit(self, fn):
        """Decorator: fn(game) once when the mod ends (stopped from the manager, game exiting, Ctrl+C). Use it to undo
        changes, e.g. restore a value the mod patched. When the game itself has exited, fn is not called."""
        self._exit_callbacks.append(fn)
        return fn

    def load_data(self, default=None):
        """Data the mod saved earlier (data.json in its folder): offsets it learned, records, ..."""
        try:
            with open(self._data_path) as f:
                return json.load(f)
        except (OSError, ValueError):
            return default

    def save_data(self, data):
        tmp = self._data_path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(data, f, indent=2)
        os.replace(tmp, self._data_path)

    def log(self, *parts):
        """A line in the mod's log (shown on the manager's Mods page)."""
        print(time.strftime("%H:%M:%S"), *parts, flush=True)

    # -- overlay ----------------------------------------------------------------------------------------------
    def box(self, box_id="main", x=0.01, y=0.6, size=1, text_color=None, panel_color=None):
        """The overlay box with this id (created on first use). Up to 9 boxes across all mods."""
        if box_id not in self._boxes:
            self._boxes[box_id] = Box(self.game, box_id, x, y, size, text_color, panel_color)
        return self._boxes[box_id]

    # -- events -----------------------------------------------------------------------------------------------
    def every_frame(self, callback):
        """Decorator: callback(game) once per presented frame (at most 60 times a second)."""
        self._frame_callbacks.append(callback)
        return callback

    def every(self, seconds):
        """Decorator: callback(game) every `seconds`."""
        def wrap(callback):
            self._timers.append([float(seconds), 0.0, callback])
            return callback
        return wrap

    def hotkey(self, key, mods="", swallow=False):
        """Decorator: callback(game) when key (F1-F12, a-z, 0-9, Insert, Home, ...) is pressed in the game window.
        mods: "ctrl", "shift", "alt" (combine with +). swallow=True hides the key from the game."""
        def wrap(callback):
            key_id = "%s:%s%s" % (self.name, mods + "+" if mods else "", key)
            self.game.command("hotkey_add", id=key_id, key=key, mods=mods, swallow=swallow, owner=self.name)
            self._hotkeys[key_id] = callback
            return callback
        return wrap

    def stop(self):
        self._stop = True

    def run(self, max_rate=60.0):
        """The main loop: frame callbacks, timers and hotkeys until the game exits or stop() is called.
        Exceptions in callbacks are logged and do not end the mod."""
        last_frames = None
        period = 1.0 / max_rate
        try:
            while not self._stop:
                started = time.monotonic()
                if self._hotkeys:
                    for event in self.game.command("hotkey_events", owner=self.name)["events"]:
                        callback = self._hotkeys.get(event["id"])
                        for _ in range(event.get("presses", 1)):
                            self._call(callback)
                if self._frame_callbacks:
                    frames = self.game.status()["frames"]
                    if frames != last_frames:
                        last_frames = frames
                        for callback in self._frame_callbacks:
                            self._call(callback)
                now = time.monotonic()
                if self._settings_callbacks and now >= getattr(self, "_settings_due", 0):
                    self._settings_due = now + 1.0
                    stamp = self._settings_mtime()
                    if stamp != self._settings_stamp:
                        self._settings_stamp = stamp
                        self.settings = self._load_settings()
                        for callback in self._settings_callbacks:
                            try:
                                callback(self.game, self.settings)
                            except GameGone:
                                raise
                            except Exception as error:
                                self.log("error in %s: %s" % (getattr(callback, "__name__", "callback"), error))
                for timer in self._timers:
                    if now >= timer[1]:
                        timer[1] = now + timer[0]
                        self._call(timer[2])
                time.sleep(max(0.0, period - (time.monotonic() - started)))
        except GameGone:
            self.log("game exited")
        except KeyboardInterrupt:
            pass
        finally:
            self._cleanup()

    def _call(self, callback):
        if not callback:
            return
        try:
            callback(self.game)
        except GameGone:
            raise
        except Exception as error:   # a bug in one callback must not end the mod
            import traceback
            self.log("error in %s: %s" % (getattr(callback, "__name__", "callback"), error))
            traceback.print_exc()

    def _cleanup(self):
        if self._cleaned:
            return
        self._cleaned = True
        for callback in self._exit_callbacks:
            try:
                callback(self.game)
            except GameGone:
                break
            except Exception as error:
                self.log("error in %s: %s" % (getattr(callback, "__name__", "on_exit"), error))
        for command in ("overlay_box_clear", "hotkey_clear"):
            try:
                self.game.command(command, owner=self.name)
            except Exception:            # game gone or socket closed: nothing left to clean
                pass
