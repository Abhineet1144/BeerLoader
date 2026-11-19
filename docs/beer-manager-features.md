# Beer Manager — feature reference

The Beer manager (`manager/beer_manager.py`, GTK 4 + libadwaita, app id `dev.beer.Manager`) is the launcher and live
control panel for the Beer loader. This file lists what it offers, window by window. Source files are named per section
so the list can be checked against the code.

It talks to running games over the loader's per-process control socket
(`$XDG_RUNTIME_DIR/beer/<pid>.sock`, one JSON object per line; `/tmp/beer-<uid>` when there is no runtime dir) and to the
Steam session provider over `$XDG_RUNTIME_DIR/beer/steam.sock`.

## Where everything is stored (`manager/storage.py`)

| Path | Content |
|---|---|
| `~/.beer/library.json` | Game library (every per-game setting below) |
| `~/.beer/manager.json` | Loader settings (paths, Steam options, collapsed library sections) |
| `~/.beer/overlay.json`, `overlay-loader.json` | Overlay settings; the second is a flat copy the loader reads when started without the manager |
| `~/.beer/display.json` | Display (picture effects, colour vision, screenshot hotkey) |
| `~/.beer/mods/<id>/`, `mods-enabled.json` | Installed mods and the per-game enable list |
| `~/.beer/tweaks/` | Saved shader-constant tweaks per game |
| `~/.beer/objects.json` | Object type names chosen in the Objects window, per game |
| `~/.beer/frames/<game>-<time>/` | Frame-breakdown captures (deleted on the next capture / window close) |
| `~/.beer/logs/` | One log per launch (`<game>-<date>.log`); only the newest 30 are kept |
| `~/.beer/prefixes/`, `~/.beer/trash/` | Prefixes, and removed prefixes (never deleted) |
| `~/.beer/steam-library/` | Default Steam downloads folder |
| `~/.beer/cache/` | Shader caches (default; changeable) |
| `~/Pictures/Beer` | Screenshots |

Files from the old XDG locations (`~/.config/beer/*.json`) are copied (never moved) on first start. The shader cache from
`~/.cache/beer` is renamed into the new cache folder once, on first use.

## Main window

Title "Beer", 900×680. A header bar with a view switcher and two buttons:

- **Add a game** (`+`) — opens the Add game dialog.
- **Loader settings** (gear) — opens the Loader settings dialog.

Pages (view switcher): **Library, Steam, Running, Prefixes, Overlay, Display, Mods**. Toasts report events (started a
game, loader or exe not found, etc.). The manager polls running loaders continuously and refreshes the Running page and
the library's "Running" state.

## Library page

- Two collapsible sections, whose open/closed state is remembered: **Local Imports** (games added from an `.exe`) and
  **Steam Games** (installed by the Steam session provider). Each shows its count and a description.
- Empty state: "No games yet".
- Per game row: name and subtitle, and these buttons:
  - **Launch** / **Stop** (a game that is running shows a "Running" dot and **Show in Running**).
  - **Edit** and **Remove from library** (files on disk are untouched).

### What launching does (`launch`)

- Checks the loader and the game exe exist (toasts if not).
- Sets Steam identity: `SteamAppId`/`SteamGameId` for Steam games or when a Steam App ID is set; `BEER_WORKING_DIR` from
  the Steam launch option's working directory; `BEER_STEAM_API_MODE` from the Steam API choice (Automatic picks
  "Your Steam session" for Steam installs).
- Passes the vkd3d-shader library (`BEER_VKD3D_SHADER_LIBRARY`) and the shader cache folder (`BEER_CACHE_DIR`).
- Applies the starting game speed (`BEER_TIME_SCALE`) and starting frame cap (`BEER_FPS_LIMIT`).
- Turns MangoHud off unless allowed (it crashed the loader's swapchain; Beer has its own overlay).
- Applies spoofed GPU/CPU, DLSS override, memory guard, presenter, present mode, every loader switch and value, the
  user's environment string, then extra loader arguments, the exe and game arguments.
- Sends the current overlay and display settings to the game at launch (`BEER_OVERLAY_JSON`, `BEER_POST_JSON`).
- With a memory cap, runs the game in a transient `systemd-run --user --scope` (same PID) with `MemoryMax` = the cap,
  `MemoryHigh` = 94 % of it and `MemorySwapMax` = the allowed swap. Without `systemd-run` it starts uncapped and says so.
- Writes the output to a per-launch log, prunes old logs (keeps 30) and switches to the Running page.
- A launcher exe that starts the real game as a child loader (RDR2) and a helper loader (Ghost of Tsushima's crash
  handler) are folded into one card: the one that presents frames is the game.

### Add / Edit game dialog (`GameDialog`)

General
- **Name**, **Executable (.exe)** (with a file picker).
- **Audio backend**: `fmod` or `compat`.
- **Starting frame cap** (0 = unlimited) and **Starting game speed**.
- **Prefix** (separate saves, settings and files per prefix).

Steam
- **Steam API** mode: Automatic / Beer offline Steam / Your Steam session / Game's own Steam DLL, with a description of
  the chosen mode.
- **Steam App ID** (its label changes with the mode: from the install, the game your account must own, or only needed
  without `steam_appid.txt`).

Hardware shown to the game (what Windows APIs report; the real GPU still renders and CPUID stays native)
- **GPU** preset: RTX 4090, 4080, 3080, 2080 Ti, GTX 1080 Ti, RX 7900 XTX, RX 6800 XT, Arc A770, or custom with GPU
  name, vendor ID, device ID and video memory (MB).
- **CPU** preset: i9-13900K, i7-12700K, i5-10400, Ryzen 9 7950X, 7 5800X3D, 5 5600X, or custom with CPU name, vendor
  (GenuineIntel / AuthenticAMD), logical processors and clock (MHz).

DLSS (same controls as the Running page card, see below)
- Override internal resolution switch, internal resolution slider (with UP/P/B/Q/UQ/DLAA marks), custom render size
  (`WxH`), DLSS model/preset choice.

Memory
- **Limit the game's memory**: memory cap (GB) and swap allowed beyond the cap (GB).
- **Low-memory guard**: pause when free RAM drops below N MB, pause within N MB of a memory cap, and resume
  automatically when memory recovers.

Loader options
- **Presenter**: Default / auto / vulkan / x11.
- **Present mode**: Default / fifo (vsync) / mailbox (low latency) / immediate (tearing).
- **Switches** (one per launch option the loader understands, each with a description and its environment variable):
  - General: explicit `--renderer=vulkan`; disable ray tracing; disable final-image passes; legacy D3D11 path; limit
    D3D12 to shader model 5.1; hide fake Steam registry keys; known guest-code patches; transaction patches; CPU staging
    in the presenter; print FPS to the log; allow guest DLLs at 0x180000000; allow MangoHud; log ray tracing requests.
  - D3D12 (BD12): per-draw binding (old path); texel-buffer descriptors (old path); separate Vulkan queues
    (experimental); disable mesh shaders; disable variable-rate shading; hide DXR; release pipelines after compiling;
    never release pipelines; compile pipelines on first use; disable pipeline libraries; disable the shader
    translation cache; ignore the saved pipeline cache.
  - Window, media, I/O: hide the cursor in fullscreen; single native window; DirectStorage CPU decompression; videos
    as direct blit.
- **Values** (empty = default): Beer's own DLLs (`BEER_DLL_BUILTIN`); extra runtime DLL folders (`BEER_DLL_PATH`);
  pipeline memory budget (MB); max VRAM reported (MB); max shared GPU memory reported (MB); max RAM reported (MB);
  audio latency margin (ms); NVIDIA driver version to report; DLSS (NGX) runtime folder; Steam ID for the offline Steam
  API. Path-type values get a folder picker.

Advanced
- **Extra loader arguments**, **Game arguments** (passed to the exe), **Environment** (`KEY=value KEY2=value`).

Buttons: Cancel, Save.

## Steam page (`steam_page.py`)

Signs into a real Steam account through the `beer-steam` session provider (started on demand; the session lives in the
system keyring; passwords and Steam Guard codes are passed straight to the provider and never stored or logged).

States: provider not running (with the reason), connecting, signed out, Steam Guard, signed in.

- **Sign in**: **QR code** (approve in the Steam mobile app, with an on-screen QR and cancel) or **Account name +
  Password**.
- **Steam Guard**: type a code (submit), or wait for approval in the app; cancel the login.
- **Signed in**: profile (persona, **SteamID**, **Session storage**), **Sign out** (confirmed).
- **Library** of owned apps with count, a refresh spinner and **Show all** (also tools, DLC and other software).
  Sections **Installed / Downloading / Library** with counts, and a **search** box.
- Per app: **Install** (dialog: destination folder, free space, version/branch picker), **Pause/Resume**,
  **Cancel**, **Retry**, **Dismiss**, **Update** when one is available, an "Installed" mark, and a **More** menu:
  - Open folder, Verify files, Change version… (switch branch; downloads only the difference), Move to another
    folder…, Uninstall… (files added to the folder, such as saves and mods, are kept).
- **Versions/branches**: lists the app's branches and offers **Beta password** unlock for password-protected ones.
- Download progress, speed and ETA per job; installs are added to the library under Steam Games and get their own
  prefix (`steam-<appid>`), separate from an imported copy.

## Running page / game card (`RunningCard`)

One card per running game (empty state: "Nothing running"). Everything is live over the control socket.

- **Status**: fps, CPU % (and cores), memory, threads, uptime; "not responding" when the game stops answering.
- **Low memory — game paused** row with **Continue** (the memory guard).
- **Game speed**: log slider 0.05×–10× with marks and a readout; **Quick speeds** 0.25× / 0.5× / 1× / 2× / 4×.
- **Frame cap**: Off / 30 / 60 / 90 / 120 / 144 buttons and a custom value (0–1000).
- **Volume** slider (per game, shows the number of audio streams) and **Mute**.
- **Pause game time** (freezes the clocks the game sees; rendering keeps running).
- **DLSS** expander (live; applies when the game next sets up DLSS): override internal resolution switch, internal
  resolution slider (33–100 %, with UP/P/B/Q/UQ/DLAA marks), custom `WxH`, model/preset
  (Game default, DLSS default for its version, J, K, L, M, F, E, or "No upscaling"), and a **?** help popover that
  explains each model.
- **Session** buttons: **Log** (viewer), **Graphics…**, **Frame…**, **Memory…**, **Objects…**, **Stop**.
- Speed, cap, volume and DLSS choices are remembered per game in the library.
- Overlay and display settings are pushed to the game when its card appears (including games started from a terminal),
  and saved shader tweaks are re-applied.
- Mods enabled for the game are started with the card and stopped when the game ends.

### Log viewer
Shows the tail (last 200 KB) of the game's log, refreshes live, **Copy** copies it.

## Prefixes page

A prefix is a private fake `C:` drive (saves, settings, installed files).

- **New prefix** (name: letters, digits, `.`, `_`, `-`, max 48; creates the standard folder layout and `dosdevices`).
- Per prefix: **Open folder**, **Clone**, **Move to ~/.beer** (for prefixes still in the old
  `~/.local/share/beer/prefixes`, marked "(old location)"), **Remove** (moves it to `~/.beer/trash`, never deletes).

## Loader settings dialog

- **Loader binary**, **vkd3d-shader library**, **Shader cache folder** (each with a file/folder picker; defaults are
  detected, the cache folder moves the old cache when first used).
- **Steam download limit** (MB/s, 0 = none), **Steam game language**, **Steam session provider** path.
- **Steam downloads** folders: add and remove folders (the last one cannot be removed; files stay untouched).

## Overlay page (`overlay_page.py`)

In-game overlay settings, applied live to every running game and at launch.

- **Toggle shortcut** (default F10; **Change…** captures a key combo) and **Toggle now** for running games.
- **Show when a game starts** (also for games started from a terminal).
- **Layout**: style *Panels (one value per line)* or *Single row (compact)*; a drag-and-drop preview (scaled to
  1920×1080) to place the boxes.
- **Resource usage box**: show/hide, text size (Small/Medium/Large) and fields: FPS, frame time, CPU usage, memory,
  thread count, game speed, uptime, GPU usage, VRAM, clock, frame-time graph.
- **Pinned memory box**: show/hide and size; shows values pinned from the Memory window, only while something is pinned.
- **Colours**: panel opacity (text and borders stay solid), panel background, text, title and border colour.

## Display page (`display_page.py`)

Final-image effects applied after the game's own tone mapping, live and at launch.

- **Screenshots**: shortcut (default F12, **Change…**), **Screenshot** and **Clean screenshot** (without the overlay)
  from every running game, **Open the screenshot folder**. PNG at the game's resolution in `~/Pictures/Beer`.
- **Picture** sliders (each with a neutral mark, **Reset all**): brightness, contrast, gamma, saturation, vibrance,
  temperature, sharpen, vignette, film grain, letterbox bars.
- **Colour vision**: Off; correct protanopia / deuteranopia / tritanopia; simulate protanopia / deuteranopia /
  tritanopia; strength slider.

## Mods page (`mods_page.py`)

Python mods that run next to a game as their own process (docs in `docs/mod-api/`).

- **Install mod…** (a `.bmod` package; also drag-and-drop onto the page), **Docs**, **Open mods folder**, **Reload**.
- Per installed mod: name and version, **Show the log**, **Remove**, a status row ("Running for <game>" / "Not running"),
  one switch per matching library game ("Run with this game"; "No matching game in the library" with the games the mod
  was made for), and a **Settings** expander built from the mod's manifest (switch, int/float, choice, text), applied the
  next time the mod starts.
- A mod is given the game's control socket (`BEER_SOCKET`), its folder (`BEER_MOD_DIR`) and the SDK plus shared helper
  libraries on `PYTHONPATH`; it is stopped when the game exits.

## Graphics window (`graphics_window.py`, from the card's Graphics…)

Finds and tweaks numbers the game feeds its shaders (light intensity, fog, exposure…). Tabs:

- **Start here**: explanation, a **Recipe** picker (Flashlight or lamp strength; Game brightness or exposure; Sun, time of
  day or sky colour; Fog or mist density; Something else) with step-by-step text, then **Start watching → Take a
  snapshot → It went up / It went down / Stayed the same**, or **Keep equal** to a value you can see; optional minimum and
  maximum change; live status/hint; **Start over**.
- **Candidates**: the remaining constants; try ×0.5, ×2 or zero, **Undo**, **Save…** with a name.
- **My tweaks**: saved tweaks per game (Multiply / Add / Set, value, enable switch, hit count, delete). Re-applied at every
  launch (stored in `~/.beer/tweaks`).
- **Browse**: every constant buffer the last frame used, refresh the list, **Show numbers** live.

## Memory window (`memory_window.py`, from Memory…)

Cheat-Engine-style tool, reading/writing through the loader (`/proc/self/mem`, so bad addresses return an error instead of
crashing the game). Tabs:

- **Scan**: value type (4/2/1/8-byte integer, float, double, text, byte array), value, *Unknown initial value*,
  *Writable memory only*, a keep-only condition (less/greater/equal…), **First scan**, **Next scan** (exact, changed,
  unchanged, increased, decreased, with optional minimum/maximum change), **Reset**, **Set all**, coverage status. Results
  rows: set a value, add to watch list, pin to overlay, remove.
- **Watch list**: add by address; per row set value, overlay label, **Pin**, **Freeze** (rewrites every few ms), remove.
- **Pinned**: everything shown in the overlay's Pinned box; rename or remove (pins last until the game closes).
- **Hex view**: read an address with a length, write bytes (e.g. `90 90 90`).

## Objects window (`objects_window.py`, from Objects…)

Finds game objects by their transform, finds the player by walking it, and edits what is around it.

- **Scan**, **Find player**, **Radius** (1–5000 game units) for "near the player".
- Lists by type (group), with **← All types**; per object: distance and position.
- Details: **Transform** (position/rotation/scale), **Apply**, **Freeze position**, **Bring to player**, **Move player
  here**, **Type name** (remembered per game), and **Fields** (the object's memory as 4-byte values, editable).

## Frame breakdown window (`frame_window.py`, from Frame…)

Records the next presented frame of a D3D12 game and shows how it was built.

- Capture modes: **Render passes**, **Passes + compute**, **Every draw and dispatch** (a reduced picture after each step,
  capped at about 1,500).
- **Capture frame**, **Open the capture folder**, **Depth** and **Compute** toggles.
- Grid of all steps with format, size and the pass/dispatch it came from; a step viewer with previous / next (← →),
  play (space) and back-to-grid.
- Captures are deleted when a new one is taken or the window closes.
