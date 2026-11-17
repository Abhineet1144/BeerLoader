# Control-socket protocol (version 1)

Every game running under Beer listens on a Unix socket:
`$XDG_RUNTIME_DIR/beer/<pid>.sock`, or `/tmp/beer-<uid>/<pid>.sock` when `XDG_RUNTIME_DIR` is not set. The socket is
mode 0600, so only your user can connect. The Python SDK wraps all of this; use the protocol directly for tools in
other languages.

- One JSON object per line in each direction: send `{"cmd": "...", ...}\n`, read one reply line.
- Every reply has `"ok": true` or `"ok": false, "error": "..."`.
- Several clients may connect at once (the manager, mods). Each connection gets its own thread.
- **Addresses** are strings in hex (`"0x7ff6a1b20000"`).
- **Numbers** may be JSON numbers or strings. **Booleans** are `true`/`false` (`1`/`0` also work).
- Replies are at most 256 KiB.

Version 1 is stable: commands and fields are only ever added, never changed or removed. `hello` reports the version.

## General

| Command | Arguments | Reply |
|---|---|---|
| `hello` | — | `protocol`, `pid`, `exe` |
| `status` | — | `pid`, `uptime_s`, `frames`, `fps`, `cpu_percent`, `cpu_cores`, `rss_mb`, `threads`, `time_scale`, `fps_limit`, `paused`, `max_frame_ms`, `hitches` (frames over 50 ms since the last `status`), `memguard` `{active, reason, avail_mb, total_mb, cg_used_mb, cg_limit_mb}` |
| `settings` | — | `settings`: live settings `[{name, label, description, type, min, max, default, value}]` |
| `set` | `name`, `value` | `setting`. Names: `time_scale`, `fps_limit`, `paused`, `dlss_ratio`, `dlss_width`, `dlss_height`, `dlss_preset`, `dlss_bypass`, `memguard`, `memguard_free_mb`, `memguard_margin_mb`, `memguard_auto_resume` |
| `key` | `vk` (Windows virtual-key code), `dik` (DirectInput scan code, optional), `down` (1/0), `extended` | — |
| `memguard_resume` | — | Resumes a game paused by the low-memory guard |
| `trim` | — | `rss_before_mb`, `rss_after_mb`, `ms` (returns free memory inside the allocator to the system) |

## Memory

Types: `i8 i16 i32 i64 f32 f64 str bytes`. Unknown-value scans and value searches skip memory the game reserved but never touched. Unsigned reads are done by the client: read the signed type and
reinterpret the bits.

| Command | Arguments | Reply |
|---|---|---|
| `mem_read` | `addr`, `len` (1–65536) | `addr`, `hex` |
| `mem_read_batch` | `reads`: `"addr:len,addr:len,..."` (each `len` 1–256, at most 512 reads) | `data`: `["hex" or null, ...]`, in the order given |
| `mem_write` | `addr`, `hex` (up to 1024 bytes) | `written` |
| `mem_resolve` | `base`, `offsets` (`"0x10,0x8"`), `module` (optional) | `addr` (final), `steps`; an error names the pointer that was null or unreadable |
| `mem_modules` | — | `modules`: `[{name, base, size}]`, the executable first |
| `mem_regions` | — | `regions`: `[{start, end, perms, path}]` |
| `mem_scan` | `type`, `value` (or `unknown: true`), `writable`, optional `cond` (`lt le gt ge eq`) + `cond_value` | `count`, `results` |
| `mem_next` | `mode` (`exact` `changed` `unchanged` `increased` `decreased`), `value`, optional `cond` + `cond_value`, `up_to` / `at_least` (bounds on the size of the change) | `count`, `results` |
| `mem_results` | `offset`, `limit` | `count`, `results`: `[{addr, value}]` |
| `mem_aob` | `pattern` (`"44 88 6B ?? C7"`, `??` = any byte), `module` (optional), `limit` (default 16, max 4096) | `count`, `addrs`: array-of-bytes search, read-only memory (code) included |
| `mem_find` | `values` (`"0x..,0x.."`, up to 256), `limit` (addresses per value, default 16, max 256), `writable` | `results`: `[{value, count, addrs}]` in the order given. One pass, exact counts, does not touch the scan |
| `mem_values` | `type`, `addrs` (array) | `values` |
| `mem_set` | `addr`, `type`, `value` | — |
| `mem_set_all` | `value` | `written`, `failed` (every current scan result) |
| `mem_remove` | `addrs` (array) | `count` (drops results from the scan) |
| `mem_reset` | — | — |
| `mem_freeze` / `mem_unfreeze` | `addr`, `type`, `value` / `addr` | — (rewrites the value about 10 times a second) |
| `mem_frozen` | — | `frozen`: `[{addr, type, value}]` |

`mem_resolve` follows Cheat Engine's convention:
1. Start at `addr = base`, plus the base address of `module` when it is given.
2. For each offset: `addr = *(u64 *)addr + offset`.

## Objects

| Command | Arguments | Reply |
|---|---|---|
| `obj_scan` | — | `groups` `[{id, name, count, kind, vtable, ...}]`, `transforms`, `typed`, `ms`, `scanned_mb` |
| `obj_list` | `group`, `limit` | `items`: `[{index, base, transform, v}]` |
| `obj_find_player` | — | `items` ranked by `score` |
| `obj_near` | `x`, `y`, `z`, `radius`, `limit` | `items` with `distance`, `total` |
| `obj_moved` | `min`, `max` | `items` that moved by that much since the last call |
| `obj_copies` / `obj_shift` | position copies / move them all | (used by the manager's Objects window) |

## Overlay

| Command | Arguments | Reply |
|---|---|---|
| `overlay_box_set` | `id`, `owner`, `text`, `x`, `y` (permille of the screen, top-left), `size` (0–2), `text_color`, `panel_color` (`"rrggbb"`) | `slot`. Creates or updates a mod box. In `text`, `\n` starts a new line, `[a]..[/a]` is the accent colour, `[d]..[/d]` is dim. An empty `text` hides the box. The font is ASCII; any other character is drawn as `?`. Up to 9 boxes in total. |
| `overlay_box_remove` | `id` | `removed` |
| `overlay_box_clear` | `owner` (`""` = all) | `removed` |
| `overlay_boxes` | — | `max`, `boxes`: `[{id, owner, x, y}]` |
| `overlay_get` / `overlay_set` / `overlay_toggle` | Beer's own overlay configuration | configuration |
| `overlay_pin` / `overlay_unpin` / `overlay_pins` | `addr`, `type`, `label` | Values shown in Beer's pinned-values box |

Mod boxes are drawn whether or not the user has toggled Beer's own overlay.

## Hotkeys

| Command | Arguments | Reply |
|---|---|---|
| `hotkey_add` | `id`, `key`, `mods` (`"ctrl+shift"`...), `swallow` (true: the game does not see the key), `owner` | `keysym` |
| `hotkey_events` | `owner` (optional) | `events`: `[{id, presses}]` since the last poll; the counts then reset |
| `hotkey_remove` | `id` | `removed` |
| `hotkey_clear` | `owner` (`""` = all) | `removed` |

Key names:
- `F1`–`F12`, `a`–`z`, `0`–`9`, `Insert`, `Delete`, `Home`, `End`, `PageUp`, `PageDown`, `Pause`, `ScrollLock`, `Tab`, `Space`;
- the numpad keys `KP_Add`, `KP_Subtract`, `KP_Multiply`, `KP_Divide`;
- otherwise an X keysym number.

Presses are counted until polled, so none are missed.

## Graphics

| Command | Arguments | Reply |
|---|---|---|
| `screenshot` | — | `path` |
| `post_get` / `post_set` | Post effects (brightness, contrast, gamma, saturation, sharpen, colour-blind modes, ...) | configuration |
| `gfx_frame_capture` | `dir` | Saves every render pass of the next frame as PPM files, then writes `dir/done` |
| `gfx_frame_capture_status` | — | `state`: `idle`, `waiting` or `done` |
| `gfx_capture`, `gfx_cbs`, `gfx_cb_read`, `gfx_scan`, `gfx_next`, `gfx_results`, `gfx_tweak_*` | | Shader-constant watching and tweaks (the manager's Graphics window) |

## Example session

```
> {"cmd":"hello"}
< {"ok":true,"protocol":1,"pid":41233,"exe":"/games/RE8/re8.exe"}
> {"cmd":"mem_resolve","base":"0x0A1B2C0","module":"re8.exe","offsets":"0x60,0x18,0x2c8"}
< {"ok":true,"steps":["0x14a1b2c0","0x7ff0c4a0","0x7ff1d200","0x7ff1d4c8"],"addr":"0x7ff1d4c8"}
> {"cmd":"mem_read_batch","reads":"0x7ff1d4c8:4,0x7ff1d4cc:4"}
< {"ok":true,"data":["58020000","58020000"]}
> {"cmd":"overlay_box_set","id":"hp:enemy","owner":"hp","text":"[a]Enemy[/a] 600 / 600","x":10,"y":600}
< {"ok":true,"slot":0}
```
