# Beer mod API

Mods are small Python programs that run next to a game under Beer. A mod can:

- read and write the game's memory, follow pointer paths, scan for values and find objects by type;
- draw its own text boxes on the in-game overlay (health bars, timers, stats, notes);
- react to hotkeys pressed in the game window;
- change the game speed, pause the game clocks, set a frame cap, send key presses, take screenshots.

Mods are **not** tied to one use: an enemy-health display, a speedrun timer, a stat tracker, a practice tool or a
"freeze this value" cheat are all just mods.

Every mod runs in **its own process** and talks to the game through the game's control socket, so a mod that crashes
or hangs never takes the game down. The manager starts a game's enabled mods when the game starts and stops them when
it exits.

| Document | What it covers |
|---|---|
| [Quick start](#quick-start) (below) | Your first mod in five minutes |
| [sdk.md](sdk.md) | The Python SDK (`beer_mod`): every class and method |
| [reengine.md](reengine.md) | Optional helper library for RE Engine games: classes, fields and live objects by name (`mods/libs/reengine.py`) |
| [protocol.md](protocol.md) | The control-socket protocol under the SDK, for tools in other languages |

## Quick start

1. Make a folder for the mod anywhere, for example `~/my-mods/hello/`.
2. Add `mod.json`:

   ```json
   {
     "id": "hello",
     "name": "Hello overlay",
     "version": "1.0",
     "description": "Shows the frame rate in a box.",
     "games": ["*"],
     "entry": "main.py"
   }
   ```

3. Add `main.py`:

   ```python
   from beer_mod import Mod

   mod = Mod()
   box = mod.box("fps", x=0.80, y=0.02)

   @mod.every(0.5)
   def update(game):
       status = game.status()
       box.text = f"[a]FPS[/a] {status['fps']:.0f}"

   mod.run()
   ```

4. Build the package: `PYTHONPATH=<Beer>/mods/sdk python3 -m beer_mod.package pack ~/my-mods/hello`. This writes
   `hello-1.0.bmod`.
5. On the manager's **Mods** page, click **Install mod…** (or drop the `.bmod` file onto the page). Then enable *Hello
   overlay* for a game and start the game.

While developing, you can run a mod straight from its folder next to a running game, without packaging it:
`PYTHONPATH=<Beer>/mods/sdk:<Beer>/mods/libs python3 main.py`. It connects to the newest running game;
`BEER_SOCKET=<path>` picks one.

## Packages (.bmod)

A `.bmod` file is a zip archive with the mod's `mod.json` at its root. A single top-level folder that contains
`mod.json` is accepted too. Mods are shared as `.bmod` files; they do not live in Beer's source tree.

```
PYTHONPATH=<Beer>/mods/sdk python3 -m beer_mod.package pack <mod folder> [-o out.bmod]   # build <id>-<version>.bmod
PYTHONPATH=<Beer>/mods/sdk python3 -m beer_mod.package check <file.bmod>                 # validate, list the files
PYTHONPATH=<Beer>/mods/sdk python3 -m beer_mod.package install <file.bmod>               # install without the manager
```

- `pack` leaves out `settings.json`, `data.json`, `__pycache__`, `.pyc` files, hidden files and links.
- Installing unpacks into `~/.beer/mods/<id>/`. A broken package never leaves a half-installed mod: it is unpacked
  into a temporary folder first.
- Installing over an existing copy replaces the mod's files but keeps the user's `settings.json` and `data.json`. A
  running copy is restarted with the new version.
- Packages are refused when a path is absolute or contains `..`, when they contain links, when they have more than
  2000 files or unpack to more than 256 MB, and when `mod.json` is missing or invalid (`id`, `name` and `version`
  are required; `id` is lower-case letters, digits, `.`, `_`, `-`).
- **Remove** (the bin icon on the Mods page) deletes the installed mod with its settings and saved data.

Shared helper libraries are different: they ship with Beer in `<Beer>/mods/libs/` (for example
[reengine](reengine.md)) and are on every mod's `PYTHONPATH`, so packages import them instead of bundling copies.

## The installed mod folder

```
~/.beer/mods/<id>/
  mod.json        manifest (required)
  main.py         entry point (any name; see "entry")
  settings.json   values from the manager's settings panel (written by the manager)
  data.json       data the mod saves with mod.save_data() (learned offsets, records, ...)
```

The manager writes the mod's output (anything it prints, plus errors) to `~/.beer/logs/mod-<id>.log` and shows it on
the Mods page.

### mod.json

| Key | Meaning |
|---|---|
| `id` | Unique short name (required): lower-case letters, digits, `.`, `_`, `-`. The install folder name; used for overlay boxes, hotkeys and logs |
| `name`, `version` | Required; shown on the Mods page |
| `description`, `author` | Shown on the Mods page |
| `games` | Executable names the mod is meant for (`["re8.exe"]`), or `["*"]` for any game |
| `entry` | The Python file to run (default `main.py`) |
| `settings` | Settings the manager shows as a panel: `[{"key", "label", "type": "bool"/"int"/"float"/"text"/"choice", "default", "min", "max", "choices"}]` |

Mods listed in `games` are offered for those games only; whether a mod actually runs is chosen per game on the Mods
page.

## How memory addresses are found

Addresses differ per game and often per game version. Four ways to get to a value, most robust first:

- **Engine reflection**: some engines keep their own class and field names in memory. For Capcom's RE Engine
  (Resident Evil 2/3/4/7/8/9, Monster Hunter, Pragmata, Onimusha...), the optional `reengine` helper library (in
  `mods/libs`) finds classes, fields and every live object **by name**, so a mod needs no addresses at all and survives game updates. See
  [reengine.md](reengine.md); the RE8 enemy-health mod uses it.
- **Pointer paths**: a chain of offsets from a module, as Cheat Engine and LiveSplit autosplitters use.
  `game.resolve(0x1A2B3C0, [0x58, 0x10, 0x2C], module="game.exe")` survives restarts (not always updates).
- **Scanning**: the manager's Memory window (or `game.scan` / `game.scan_next`) narrows a value down while it changes
  in the game ("health went down" -> scan `decreased`).
- **Objects**: objects of one class share their first qword (a vtable or type pointer). Once one field of one object
  is known, `game.find_values([header])` finds every other object of that class, and the field is at the same offset.

## Limits and good manners

- A mod sees exactly what the game process holds: values are per game and per version.
- There are no save states (whole-game snapshots); pause and slow motion are the practice tools.
- Writing memory or injecting input is what anti-cheat systems look for. Use those features in offline games only.
- Keep per-frame work small: batch reads with `game.read_many` instead of hundreds of single reads.
