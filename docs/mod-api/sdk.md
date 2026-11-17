# beer_mod SDK reference

`beer_mod` lives in `<Beer>/mods/sdk/beer_mod/`. The manager puts it, and the optional helper libraries in `<Beer>/mods/libs/`, on `PYTHONPATH` for every mod it starts.

```python
from beer_mod import Mod, Game, Box, GameGone, ModError
```

- **Mod**: your mod. It owns the connection, settings, saved data, overlay boxes, hotkeys and the main loop.
- **Game**: the running game (`mod.game`). Every method wraps one control command (see [protocol.md](protocol.md)).
- **Box**: one overlay text box, owned by the mod.
- **GameGone**: raised when the game exits; `Mod.run()` handles it and ends the mod.
- **ModError**: raised when the game refuses a command; the message says why.

## Packaging

`beer_mod.package` builds and installs `.bmod` packages; see [Packages](README.md#packages-bmod).

## Mod

```python
mod = Mod(name=None, pid=None)
```

Connects to the game: the one the manager started the mod for (`BEER_SOCKET`), or with `pid`, that game, or else the
newest running Beer game. `name` defaults to the manifest `id`.

| Member | Purpose |
|---|---|
| `mod.game` | The `Game` |
| `mod.name`, `mod.folder`, `mod.manifest` | Identity, the installed mod folder, parsed `mod.json` |
| `mod.settings` | Dict of setting values: manifest defaults, overridden by the manager's settings panel |
| `mod.load_data(default=None)` | What `save_data` stored last time (`data.json`), or `default` |
| `mod.save_data(obj)` | Stores any JSON value in `data.json` (learned offsets, personal bests, ...) |
| `mod.log(*parts)` | Writes a timestamped line to the mod's log |
| `mod.box(id, x=0.01, y=0.6, size=1, text_color=None, panel_color=None)` | Gets the overlay box `id`, creating it on first use |
| `@mod.every_frame` | Decorator: `fn(game)` once per presented frame (at most `max_rate` times a second) |
| `@mod.every(seconds)` | Decorator: `fn(game)` every `seconds` |
| `@mod.hotkey(key, mods="", swallow=False)` | Decorator: `fn(game)` when `key` is pressed in the game window |
| `@mod.on_settings_changed` | Decorator: `fn(game, settings)` after the user changes the mod's settings in the manager while it runs (`mod.settings` is already updated) |
| `@mod.on_exit` | Decorator: `fn(game)` once when the mod ends (Stop in the manager, Ctrl+C). Undo changes here, e.g. restore a patched value. Not called when the game itself has exited. |
| `mod.run(max_rate=60)` | Main loop until the game exits or `mod.stop()` is called |
| `mod.stop()` | Ends `run()` after the current iteration |

Callbacks that raise are logged and the loop goes on. When `run()` ends, the mod's boxes and hotkeys are removed.

**Hotkey names:**
- keys `F1`–`F12`, `a`–`z`, `0`–`9`, plus `Insert`, `Delete`, `Home`, `End`, `PageUp`, `PageDown`, `Pause`, `ScrollLock`, `Tab`, `Space`, and the numpad keys `KP_Add`, `KP_Subtract`, `KP_Multiply`, `KP_Divide`;
- an X keysym number for anything else;
- modifiers: `ctrl`, `shift`, `alt`, combined with `+` (for example `"ctrl+shift"`).

## Box

| Member | Purpose |
|---|---|
| `box.text = "..."` | Sets the text. `\n` starts a new line, `[a]...[/a]` uses the accent colour, `[d]...[/d]` dims. An empty string hides the box. The overlay font is ASCII; other characters show as `?`. |
| `box.move(x, y)` | Top-left corner as a fraction of the screen (0..1) |
| `box.style(size=None, text_color=None, panel_color=None)` | Font size `0`/`1`/`2`; colours as `"rrggbb"` |
| `box.remove()` | Removes the box |

Updates are only sent when something changed, so setting the same text every frame costs nothing. Mod boxes are shown
whether or not the user has toggled Beer's own overlay. Up to 9 boxes exist at a time across all mods.

## Game

### Information

| Method | Returns |
|---|---|
| `info()` | `{'pid', 'exe', 'protocol'}` |
| `status()` | `{'fps', 'frames', 'cpu_percent', 'rss_mb', 'threads', 'time_scale', 'fps_limit', 'paused', 'max_frame_ms', 'hitches', 'memguard'}` |
| `modules()` | `{name: (base, size)}` for the executable and every loaded DLL (names lower-case) |
| `module_base(name)` | Base address of one module, or `None` |

### Memory

Types: `i8 u8 i16 u16 i32 u32 i64 u64 f32 f64`. Unreadable memory gives `None`, never an exception.

| Method | Purpose |
|---|---|
| `read(addr, type)` | One value |
| `read_i32 / read_u32 / read_i64 / read_u64 / read_f32 / read_f64 / read_ptr(addr)` | Shorthands |
| `read_bytes(addr, length)` | Up to 65536 bytes |
| `read_string(addr, max_length=256, wide=False)` | NUL-terminated UTF-8 (or UTF-16 with `wide=True`) |
| `read_many([(addr, type), ...])` | Many values in one round trip (use this per frame) |
| `write(addr, type, value)`, `write_i32`, `write_f32`, `write_bytes(addr, data)` | Writes (up to 1024 bytes) |
| `freeze(addr, type, value)` / `unfreeze(addr)` | Keeps a value rewritten about 10 times a second |
| `find_pattern(pattern, module=None, limit=16)` | Addresses of a byte pattern (`"C7 43 18 ?? ?? ?? ??"`, `??` = any byte) in one module or all readable memory, code included. Find code or data by its bytes so the mod survives game updates. |
| `resolve(base, offsets=(), module=None)` | Pointer path → final address, or `None` while a pointer on the way is null (object not loaded) |
| `regions()` | Mapped memory: `[{'start', 'end', 'perms', 'path'}]` |

`resolve` follows Cheat Engine's convention:
1. Start at `addr = base`, plus the module's base address if `module` is given.
2. For each offset: `addr = read_u64(addr) + offset`.

So `resolve(0x10, [0x20, 0x8], module="game.exe")` means `[[game.exe+0x10]+0x20]+0x8`.

### Scanning

Scans share their state with the manager's Memory window: a scan from a mod replaces the window's scan, and the other
way round.

| Method | Purpose |
|---|---|
| `scan(type, value=None, writable=True, cond=None)` | New scan for `value`, or unknown-value scan with `None`; returns the count. `cond=("gt", 0)` also requires the value to pass a test (`lt le gt ge eq`) |
| `scan_next(mode, value=None, cond=None, up_to=None, at_least=None)` | Narrow: `exact`, `changed`, `unchanged`, `increased`, `decreased`. `up_to`/`at_least` bound the size of the change |
| `scan_results(limit=200, offset=0)` | `[(addr, value)]`, up to `limit` (fetched from the game in pages of 500) |
| `scan_reset()` | Clears the scan |
| `find_values(values, limit=16, writable=True)` | Every 8-aligned qword equal to one of up to 256 `values`, in one pass: `{value: (count, [addrs])}`. Does **not** touch the shared scan, so it is safe to repeat (for example, find every object whose first qword is a known type pointer) |

### Objects

| Method | Purpose |
|---|---|
| `object_scan()` | Finds objects by their transforms and groups them by type; returns the groups. Takes a second or two. |
| `objects(group, limit=300)` | Objects of one group: `[{'base', 'transform', 'v', ...}]` |
| `find_player()` | Candidates for the player object |
| `near(x, y, z, radius=50, limit=100)` | Objects around a position |

### Game control

| Method | Purpose |
|---|---|
| `set_speed(scale)` | Game speed: 1.0 normal, 0.05–10 |
| `set_paused(bool)` | Freezes the game clocks (rendering continues) |
| `set_fps_cap(fps)` | Frame cap, 0 = none |
| `press(vk, down=True)`, `tap(vk)` | Key input to the game window (Windows virtual-key codes) |
| `screenshot()` | Saves a screenshot, returns its path |
| `command(cmd, **args)` | Any control command directly (see protocol.md) |

## Patterns

**RE Engine games**: import the optional [reengine](reengine.md) helper library (mods/libs) to find classes, fields and live objects by name instead of addresses.

**Find every object of a type** (objects of one class share their first qword: a vtable or type pointer):

```python
count, objects = game.find_values([type_ptr], limit=256)[type_ptr]
```


**Wait for an object to load** (`resolve` gives `None` until it exists):

```python
@mod.every(0.25)
def update(game):
    hp = game.resolve(0x0A1B2C0, [0x60, 0x18, 0x2C8], module="game.exe")
    box.text = "" if hp is None else f"HP {game.read_i32(hp)}"
```

**Sample many objects per frame**:

```python
values = game.read_many([(base + 0x2C8, "i32") for base in enemies])
```

**Remember something between runs**:

```python
data = mod.load_data({})
data["best"] = 123.4
mod.save_data(data)
```
