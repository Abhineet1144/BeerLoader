# Beer Loader — Active Development Context

RDR2 Path: /mnt/wwn-0x5000c500c67d4454-part1/Games/RDR2/Launcher.exe
Last consolidated: 2026-10-18

This file intentionally contains only active engineering state. Historical run
transcripts, disproven hypotheses, and stale experiments were removed.

> **2026-10-24: the old host-snapshot D3D12 path is gone.** BD12 (graphics/d3d12v) is the only D3D12
> implementation and no longer needs `BEER_D3D12_BD12=1`. Sections below that describe the "scalar bridge", host
> shadows, resident-buffer rollbacks (`BEER_VULKAN_RESIDENT_BUFFERS`, `BEER_D3D12_EAGER_BUFFER_PUBLICATION`, ...),
> compute epochs/batching and the compat device describe removed code and are kept only as history. A recoverable
> copy of the tree before removal is `git show refs/snapshots/pre-d3d12-compat-removal` (tracked files).

## Old D3D12 compatibility path removal (2026-10-24)

Removed: `graphics/d3d12/d3d12_compat.{c,h}` (8k lines: device/queue/list/fence/resource emulation), `d3d12_compute*`,
`d3d12_graphics*`, `d3d12_vulkan_compute*`, `d3d12_vulkan_graphics*`, `renderer/vulkan/vulkan_compute.{c,h}` (the
host-snapshot compute executor) and their tests/shaders, plus ~1,000 lines of loader glue (executors, draw audit,
d3d12 present/clear/forget hooks, submission observer, stats). Kept: `d3d12_shader_translator.c` (DXBC->SPIR-V, used
by BD12), the D3D11 path (`d3d11_impl.c`, `vulkan_graphics.c`, `vulkan_indexed_renderer.c`, `vulkan_renderer.c`,
which still serve the game's movie device) and everything under `graphics/d3d12v`. New `graphics/d3d12/d3d12_api.{c,h}`
holds what survived from the compat file: `D3D12CreateDevice` (BD12 only), `D3D12GetDebugInterface`,
`D3D12SerializeVersionedRootSignature` with its `ID3DBlob`, and the guest event signaler. `vulkan_graphics.c` lost its
dependency on the compute bridge (image mirrors are inert stubs; D3D11 never had them). `make test-d3d12` now runs only
the shader-translator test; `make test-renderer` only the target-mirror and Vulkan graphics tests. Result: loader
binary 4.19 MB -> 3.34 MB, RE8 title scene unchanged at ~120 fps with no device loss.

## Primary architecture program

Preserve Beer’s loader, Win32/input/windowing, audio/media, Steam/filesystem,
DXBC-to-SPIR-V translation, tests, diagnostics, and extension hooks. Replace the
D3D12 command/resource core with direct Vulkan command-list compilation:

```text
D3D12 command-list recording
    -> lightweight ordered command IR
ExecuteCommandLists
    -> one compiler context
    -> Vulkan barriers/copies/clears/dispatches/draws/indirect commands
    -> one/few command buffers and submissions
    -> one timeline completion value
```

DEFAULT resources should be device-backed and GPU-authoritative. UPLOAD and
READBACK heaps remain host-visible. Descriptors and constants must use
submission-lifetime upload storage. Present should consume the GPU image.
The scalar bridge remains only as a named compatibility/debug fallback and for
(unproven) watchdog-sensitive work; the watchdog premise was wrong, see Device loss root cause.

Host-wait policy is an enforcement guardrail, not the main workstream. A normal
`ExecuteCommandLists` path must not return to the host between operations.

## Fixed performance baselines

- Studio splash: 15–17 FPS (~59–67 ms/frame). Must not regress.
- Actual title: ~2.2–3.0 seconds/frame depending on phase.
- Long-term target: <=33.3 ms/frame (30 FPS), then Proton-class performance.
- Peak RSS must remain safely below the 6 GiB test scope.
- No optimization may reintroduce Vulkan device loss, workload rejection, input
  or visual regression.

## Current command/resource-core state

### Ordered command IR

Each executable command embeds a canonical ordered-stream node. Dispatch,
draw, buffer/resource/texture copy, color clear, and depth clear recording append
to one command-list stream. Legacy per-kind lists remain only as ownership
containers during migration. `ExecuteCommandLists` consumes the canonical stream
as the ordering source; it no longer reconstructs command order by selecting the
smallest sequence among six list heads.

### Vulkan timeline and D3D12 fences

- The shared Vulkan device owns a timeline semaphore.
- Compute and graphics signal monotonically increasing timeline values.
- D3D12 queue-local signals associate guest fence values with that logical
  queue’s submitted timeline point.
- `GetCompletedValue` and events follow actual Vulkan completion.
- Private-fence fallback remains for platforms without timeline support.
- Scalar paths still wait at old boundaries; removing those waits requires the
  complete command-list compiler and submission-lifetime resource ownership.

### Resident DEFAULT resources (R6, partial)

Implemented and enabled by default:

- Device-resident DEFAULT buffers, including writable buffers and exact CBV
  ranges.
- Resource-specific dirty-buffer publication; unrelated buffers remain GPU-side.
- GPU-authoritative images with safe, full-range committed-image CPU-shadow page
  reclamation.
- Explicit CPU/present consumers rematerialize one requested resource.
- Resource-bearing compute epochs can record once/submit once when eligible.

Rollbacks:

```text
BEER_VULKAN_RESIDENT_BUFFERS=0
BEER_D3D12_EAGER_BUFFER_PUBLICATION=1
BEER_D3D12_RETAIN_IMAGE_SHADOWS=1
```

Still pending:

- Remove mandatory DEFAULT-image CPU-shadow dependence from all copy/descriptor
  paths.
- Submission-lifetime descriptor/CBV upload ring.
- Unified compute/graphics resource state and mirror ownership.
- Direct GPU presentation.

### Existing batching

- Compatible commands in one `ExecuteIndirect` expansion use one Vulkan render
  pass, command buffer, submission, and wait.
- Compatible adjacent indirect records can merge into that batch.
- Resource-bearing compute epochs use one command buffer/submission when staging,
  descriptor, hazard, and watchdog constraints permit.
- (Stale: the watchdog premise was disproven) loop+barrier 3D storage-image workloads retained segmented scalar
  fallback.

## Display, adapter and config reporting (2026-10-24)

- Screen size no longer defaults to 1080p: `xwayland_get_screen_size` connects to X before any window exists
  (the DXGI factory and USER32 query it earlier) and `xwayland_get_screen_refresh` reads the XRandR rate.
- DXGI: `IDXGIOutput::GetDesc` uses the real desktop; `GetDisplayModeList` / `FindClosestMatchingMode` and
  `EnumDisplaySettingsW` share one list (`dxgi_host_modes`: common sizes up to the native size, at the native
  refresh and 60 Hz). `IDXGIAdapter::GetDesc` and the previously unimplemented `GetDesc1` come from the Vulkan
  device (`beer_vulkan_host_gpu_info`: name, vendor/device id, largest DEVICE_LOCAL heap as dedicated VRAM,
  shared memory, RT/mesh/VRS availability). The adapter used to be vendor 0x1414 with 1 GiB.
- `WritePrivateProfileStringW` used to append `[section]\nkey=value` on every write; the game's `config.ini`
  reached 593 KB / 32k lines. It now parses into unique sections/keys (last duplicate wins), updates in place,
  supports NULL value/key deletion and writes atomically, which compacts old files. A backup of the bloated file
  was kept in the session scratchpad; the `[Render/Display]` and `[Render/Adapter]` sections were removed once by
  hand so the game could regenerate them (it had not rewritten them yet).
- D3D12 `CheckFeatureSupport` (BD12 and the compat device) still answers the zero/baseline structures. RE8 asks
  for OPTIONS(0), ARCHITECTURE(1), SHADER_MODEL(7), OPTIONS1(8), ROOT_SIGNATURE(12), OPTIONS2(18), SHADER_CACHE(19),
  OPTIONS3(21), OPTIONS4(23), OPTIONS5(27: RaytracingTier), OPTIONS6(30: VRS tier). Tier values are NOT yet derived
  from Vulkan: BD12 has no DXR (no acceleration structures, state objects or DispatchRays), so the honest RT tier is
  NOT_SUPPORTED even though the RTX GPU has VK_KHR_ray_tracing_pipeline/ray_query. Advertising a tier requires
  implementing DXR first. With VRS tier 0 the game rewrites `VRSSetting=Off`.

## Planned: ray tracing (DXR) support

Status (honest): none. BD12 and the compat device answer OPTIONS5 `RaytracingTier = NOT_SUPPORTED` and VRS tier 0,
and no DXR API exists (`CreateStateObject`, `BuildRaytracingAccelerationStructure`, `DispatchRays`, raytracing
shader binding tables, DXIL libraries). The host GPU (RTX 4070 Ti SUPER) exposes VK_KHR_ray_tracing_pipeline,
VK_KHR_acceleration_structure and VK_KHR_ray_query (`beer_vulkan_host_gpu_info` reports them). RE8's menu rays
options (`RayTracing*` keys in config.ini) stay off until this exists.

Work items, in order (each needs its own test that runs without RE8, like `make test-d3d12v`):
1. Enable the Vulkan RT extensions/features and buffer-device-address use for acceleration structures in the shared
   context (only when `beer_vulkan_host_gpu_info().ray_tracing_pipeline`).
2. ID3D12Device5/ID3D12GraphicsCommandList4 interfaces: `GetRaytracingAccelerationStructurePrebuildInfo`,
   `BuildRaytracingAccelerationStructure` -> `vkCmdBuildAccelerationStructuresKHR` (BLAS/TLAS, GPU-VA addressed,
   scratch and result buffers from the BD12 VA table).
3. DXIL library support: RE8's RT shaders are DXIL (SM 6.3+), not DXBC. Needs vkd3d-shader's DXIL path or an
   equivalent translator to SPIR-V with ray tracing capabilities.
4. State objects and shader tables: `CreateStateObject`, `ID3D12StateObjectProperties::GetShaderIdentifier`,
   `DispatchRays` -> `vkCmdTraceRaysKHR`, shader record layout and local root signatures.
5. Only then report `RaytracingTier` 1.0/1.1 in OPTIONS5 (and ray query in OPTIONS12/OPTIONS9 where applicable),
   keeping the Vulkan-derived `BeerHostGpuInfo` as the single source for the answer.
VRS (OPTIONS6) and mesh shaders (OPTIONS7) follow the same rule: report a tier only when the path is implemented.

## Video (logo/movies) and cursor lock (2026-10-24)

Video: the MF source reader is NOT the problem. RE8's logo is a 1920x1080 WMV from the pak; the byte stream is
extracted, ffmpeg decodes NV12 and the game locks every frame (150 frames for the logo). The frames were never
shown because RE8 plays movies through a separate D3D11 device (the one handed to `MFCreateDXGIDeviceManager`, i.e.
Beer's mostly-stubbed `d3d11_impl.c`): no NV12/video-sized texture, upload, `WriteToSubresource` or Present ever
reaches BD12. The old composite hack (`beer_mf_composite_pending_frame`) only ran from the fake D3D11
`dxgi_Present`, which the D3D12 path never calls. Fix: `beer_present_bd12_image` now presents the cached RGBA
frame while MF frames are still being locked (cache older than 250 ms is ignored, so the game's own output returns
when playback stops; a run of at least 10 paced locks is required, because the game pre-rolls ~6 frames in a burst and then idles ~11 s before the logo really starts, which briefly flashed the first frame). Verified in logs only (432 composite presents for 150 frames, FPS returns to ~85 after
the logo); not eyeballed. Limits: letterboxing/scaling is whatever `xwayland_window_present_rgba8` does, audio
sync is unchanged, and any movie that the game composites into the 3D scene (not full-screen) would still be
wrong. A proper fix is a real D3D11 device (CreateTexture2D NV12, Map/UpdateSubresource, shaders, shared
textures into BD12) or feeding NV12 into a BD12 texture the game samples.

Cursor lock: `ClipCursor` was a no-op. It now maps to `xwayland_window_set_pointer_confine`: `XGrabPointer`
with `confine_to` the window (XWayland turns this into a confined/locked pointer and emulates lock for
`XWarpPointer` recentering when the cursor is hidden); re-applied on Map/FocusIn, released on FocusOut and
`ClipCursor(NULL)`. RE8 does not call `ClipCursor` before the title screen, so this is unverified in gameplay and
menus. `BEER_INPUT_DIAGNOSTICS=1` logs `ClipCursor`, `XGrabPointer`/`XUngrabPointer`.
`SetCapture`/`ReleaseCapture` and relative-motion (XInput2 raw) are still stubs.
Two cursors (2026-10-24): RE8 draws its own cursor and hides the host one via `WM_SETCURSOR` -> `SetCursor(NULL)`,
but Beer never sent `WM_SETCURSOR`. It is now posted before mouse messages (motion rate-limited to 40 ms, buttons
always); an unhandled one falls back to the arrow like `DefWindowProc`. The pointer is also confined whenever the
host cursor is hidden (not only under `ClipCursor`). Unverified in menus/gameplay: confirm with
`BEER_INPUT_DIAGNOSTICS=1` (`SetCursor(NULL)`, `ClipCursor`, `XGrabPointer` lines).

## Borderless size, 4x logo, relative mouse (2026-10-24)

General rules only (nothing keyed to an executable): behaviour follows Win32/DXGI semantics and the real desktop.
- Borderless stuck at 1080p: `xwayland_window_set_style(resizable=0)` pins WM min/max size hints to the creation size,
  so the compositor ignored the game's later `SetWindowPos(0,0,2560x1440)`. `move_resize` now re-pins the hints, and an
  undecorated window covering the whole desktop asks for `_NET_WM_STATE_FULLSCREEN` (undone when it stops covering it).
- The presenter swapchain is rebuilt whenever source OR output size changes. The staging/upload path only checked the
  output size, so a 1920x1080 movie frame after 2560x1440 game frames was read with the wrong row stride and showed
  as a 2x2 tiling of the logo.
- Host cursor: a fullscreen (desktop-covering) window never shows the host cursor (`BEER_HOST_CURSOR=1` restores it);
  otherwise it follows `ShowCursor`/`SetCursor`. The pointer is confined while the host cursor is hidden or under
  `ClipCursor`, and released on focus loss.
- Relative motion: XInput2 raw motion (`libXi`, root window, `XI_RawMotion`) feeds `g_input.delta_*`, DirectInput
  mouse events and WM_INPUT. XWayland fills it from the Wayland relative-pointer, so deltas continue at the screen
  edge (the camera used to stop when the absolute pointer hit the edge). Absolute position events are still used for
  `GetCursorPos`/WM_MOUSEMOVE. `BEER_NO_XI2=1` disables it. Unverified in gameplay.

## Memory explosion, exceptions and longjmp (2026-10-24)

Memory: after the title scene loaded the loader grew to 10+ GB RSS and took the terminal down (OOM). Cause (found with
an LD_PRELOAD malloc-site shim that attributes >=4 MB allocations): the NVIDIA driver allocates ~8.6 MB of host memory
per `vkCreateDescriptorSetLayout` (and more per pipeline) because Beer's translator numbers bindings
`stage*0x10000 + class*0x2000 + register` (up to ~172k) and the driver sizes tables by the highest binding index.
Hundreds of lazily created PSOs therefore pinned many GB. Fix: `bd12_densify_bindings` renumbers each pipeline's bindings
0..n-1 and rewrites the SPIR-V `OpDecorate Binding` copy; `BD12ShaderBinding` keeps `stage` (used for root-parameter
visibility) and `source_binding`. Result: RSS flat at ~2.3 GB and 117-120 fps on the same scene, and descriptor-set
allocation is cheaper. Diagnostics added: `BEER_BD12_PROF=1` also prints live heap counts/MiB by type and
`vm-live`/malloc arena sizes; `BEER_DXGI_VRAM_MB`, `BEER_DXGI_SHARED_MB`, `BEER_PHYS_MB` cap what the guest is told
(shared system memory is now capped to half of RAM like Windows). None of them was the cause.

Exceptions / longjmp: the game's second movie crashed Beer because `RtlUnwind` was a no-op stub: MSVC `longjmp` calls
it and then ran into garbage (null `jmp_buf` read, fault address 0x50). Implemented properly in loader.c:
`RtlUnwind`/`RtlUnwindEx` (second-pass dispatch: walk frames, call UHANDLERs with `EXCEPTION_UNWINDING`, mark
`TARGET_UNWIND`, restore the unwound context at `TargetIp` with `Rax=ReturnValue`), `RtlRestoreContext`, and a
`RaiseException` that builds a CONTEXT, runs vectored handlers then frame dispatch (a C++ catch handler unwinds and
resumes itself). Supporting fixes: guest API calls run on a separate host stack, so the trampolines rewrite the
captured Rip/Rsp from TEB.SavedGuestRsp; PE DLLs' `.pdata` are now registered so `RtlLookupFunctionEntry` can walk frames
inside vcruntime/msvcp/concrt; `beer_virtual_unwind` now returns only the handler class asked for and reports the
Windows establisher frame (frame register or RSP before unwinding, what `setjmp` records as `Frame`) instead of the
post-unwind RSP. Verified: the first `longjmp` the game does at startup now reaches its target frame and the run is
stable for 60 s; the second-movie path itself was not re-driven after the fix. `BEER_EXCEPTION_DIAGNOSTICS=1` prints
every frame walked. The crash handler also prints the faulting module/symbol and a backtrace.

## Input mapping (2026-10-24)

Added in loader.c: layout-independent keycode->VK table (keypad, PrtSc, Pause, Win/Menu keys), full evdev->DIK
table including extended keys (arrows, Delete, Home/End/PgUp/PgDn, Insert, RCtrl/RAlt, KP Enter, KP /), extended
flag (lParam bit 24, RI_KEY_E0) and 7-bit make codes for raw input, generic Shift/Ctrl/Alt VKs in messages and
GetAsyncKeyState, X11 buttons 8/9 -> XBUTTON1/2 (WM_XBUTTONDOWN/UP 0x020B/0x020C, VK 5/6, DirectInput buttons
3/4, RI_MOUSE_BUTTON_4/5, MK_XBUTTON flags), X11 buttons 6/7 -> WM_MOUSEHWHEEL. Right click already worked
(button 3). Not yet verified by hand in RE8; `BEER_INPUT_DIAGNOSTICS=1` logs key/button events.

## Current compatibility state

Working:

- Save write/read, including Steam async completion ABI; first-run questions skip.
- Window/client geometry, pointer alignment, input, cursor, and taskbar icon.
- Terms/UI/title path reaches the title reliably.
- Audio and trailer lifetime paths are stable.

## Confirmed fixes that must not regress

- Correct root-signature v1.1 descriptor range decoding.
- Ordered buffer/texture copies and deferred indirect replay.
- Graphics CBVs refreshed at execution time.
- Structured/raw view offsets and counts.
- Primitive restart/strip-cut handling.
- Anisotropic samplers, array/cube views, large texture uploads.
- Vertex-only depth pipelines and non-indexed/instanced input handling.
- Vulkan robustness and bounded first-loss capture.
- Compute image-cache true LRU (never unconditional slot-zero eviction).
- Sampler, framebuffer, descriptor-set, texel-view, image, and buffer caches.
- Lazy DEFAULT CPU mappings and bounded Vulkan residency.

## Unsafe/disproven approaches

Do not reintroduce without isolated proof:

- Executable-name, fixed-RVA, shader-hash, or asset-specific runtime behavior.
- Treating true MSAA as single-sample.
- Broad typed DSV/sample-state approximations that previously caused device loss.
- One-workgroup-per-fence global scheduling (unusable performance).
- Dedicated compute queue without explicit semaphore ownership/state tracking.
- Per-operation staging buffers or early snapshots before GPU producers execute.
- More eligibility micro-classes as a substitute for command-list compilation.

## Clean-slate core: BD12 (graphics/d3d12v/), vkd3d-style 1:1 mapping

Decision (2026-10-21): the host-authoritative snapshot/replay bridge is the
ceiling (~1.6 ms per op, ExecuteCommandLists = 83% of host time). Replace it
with a GPU-resident core that maps every D3D12 call to its Vulkan equivalent at
call time, as vkd3d(-proton) does. Old path stays in the build until the new
core matches it (snapshot ref: `refs/snapshots/pre-rewrite`).

Mapping: Heap=VkDeviceMemory, Resource=VkBuffer/VkImage bound in heaps (no host
shadow for DEFAULT), GPU VA=buffer device address + sorted VA table, command
list=VkCommandBuffer recorded at call time, ResourceBarrier=pipeline barrier
2 (state->stage/access), ExecuteCommandLists=one vkQueueSubmit2, Fence=timeline
semaphore, descriptors=GPU-visible heap resolved at record time, PSO=cache +
on-disk VkPipelineCache, present from the GPU image. Large 3D storage-image
dispatches are segmented by the watchdog path (see Device loss root cause: likely removable).
Fallback if M2 stalls: embed vkd3d-proton behind `BEER_D3D12_BACKEND`.

Shared Vulkan context now requests API 1.3 and enables BDA, sync2, dynamic
rendering, descriptor indexing, scalar layout (`beer_vulkan_modern()`); old
bridges are unaffected (`make test-renderer` passes).

| Milestone | Scope | Status |
| --- | --- | --- |
| M1 | device, heaps, buffers, VA table, timeline fences, queue, allocator/list, buffer barriers, VA-addressed compute dispatch | DONE: `make test-d3d12v` passes; upload->dispatch->readback in exactly 1 submit; 11.8 us per record+submit |
| M2 | descriptors (GPU-visible heap), root signatures, compute PSOs, shader translation hook | DONE (core): `make test-d3d12v` M2 passes; descriptor table + root CBV + root constants + null descriptor, 2 bound dispatches in 1 submit; 27.5 us per bound dispatch incl. GPU run. `VK_EXT_texel_buffer_alignment` is enabled when available and its per-uniform/storage alignment properties drive typed buffer views; the 4-byte-aligned R32 SRV regression passes on NVIDIA. Open: DXBC translator -> BD12ShaderBinding bridge, descriptor-set dedupe |
| M3 | images, state->layout barriers, copies, image/sampler descriptors | DONE (core): `make test-d3d12v` M3 passes; buffer->image, sampled+storage image, sampler cache, image->buffer, image->image, 1 submit. Layouts are derived from D3D12 states (no tracker, vkd3d-style); first use does UNDEFINED->creation-state layout. Open: clears (move to M4 with attachments), implicit COMMON promotion uses TRANSFER layouts, depth/stencil plane copies, MSAA resolve |
| M4 | graphics: dynamic rendering, draws, indirect, clears | DONE (core): `make test-d3d12v` M4 passes; pipeline from reflected bindings, vertex/index buffers, root constants as scratch CBV, depth test, CW-front culling, y-up via negative-height viewport, indexed/base-vertex/indirect draws, clears (idle and in-pass), 1 submit; 0.55 us per draw incl. descriptor resolve (old path ~1600 us/op). Moved to M5: GPU-image swapchain present (needs vulkan_presenter.c API), ExecuteIndirect command signatures. Open: stencil/MSAA resolve tests, blend/MRT tests, descriptor-set dedupe |
| M5 | COM shim + loader wiring behind a flag; RE8 splash then title, 240 s soaks under `MemoryMax=6G` | IN PROGRESS (2026-10-24): COM shim is the only D3D12 implementation; `BEER_BD12_FLIGHT_RECORDER=/path` keeps a bounded record of recent submissions. Startup OOM was fixed by compiling PSOs on first bind (optional `BEER_BD12_ASYNC_PSO=1`); RSS stays ~3.6 GiB under `MemoryMax=6G`. The RE8 device loss (Xid 109) is FIXED, see "Device loss root cause" below. Open: 240 s soak, matched FPS comparison, performance work. |
| M6 | delete the old path | DONE (2026-10-24), see "Old D3D12 compatibility path removal" |

### Device loss root cause (2026-10-24): FIXED

Symptom: RE8 on BD12 hit `watchdog list failed: VkResult=2`, then device lost, with a
matching NVIDIA `Xid 109 CTX SWITCH TIMEOUT` in the kernel log (about 20 s after launch, every run).

Root cause: `resolve_one` (bd12_bind.c) ignored `D3D12_SHADER_VISIBILITY` on root parameters. RE8's
root signature has separate `b1` root parameters for the vertex and pixel stages. The skinning vertex
shader (vs `bc4c5107c004f662`) got the pixel stage's `b1` = `{1.0f,...}`; it uses `b1[0].x` as an
unsigned loop bound, so it ran about 1.07 billion iterations per vertex. The GPU sat at 100% (~100 W)
for ~5 s until Xid 109. Fix: `visibility_matches()` filters root parameters by the stage encoded in the
binding number (`binding >> 16`: 0 compute, 1 vertex, 2 pixel, 5 geometry, 6 hull, 7 domain).
Verified live: 100 s, 120 s and 180 s runs with no device loss and no Xid; before, every run failed.

Other defects fixed on the way (valid, but not the cause of the Xid):
- `bd12_queue_submit` only submitted the last recorded segment of a split list; it now submits every segment.
- Segment command buffers are reused across re-recordings instead of leaking one per segment per frame.
- `ClearState` resets binding state only, not execution bookkeeping.
- `bd12_cmd_append_segment` marks all graphics state dirty: a new command buffer has nothing bound.
- Vulkan 1.2 `drawIndirectCount` is now enabled (needed by `vkCmdDraw*IndirectCount`).

Corrections to earlier notes (the old device-loss sections were removed; they were wrong):
- The 5 s in `result=2` / `VkResult=2` is Beer's own host timeline-wait timeout, not an NVIDIA limit and
  not a measure of how long a dispatch ran.
- Tiling, watchdog segmentation, per-dispatch isolation and screen-pass tiling chased a GPU time limit
  that was never the problem. They add many submits and host waits and are the prime suspects for the
  performance gap (see Performance).
- Shaders `af5e96c297ccd1ed`, `9aaf1cbbe609609a`, `0febfe1a270857df`, `03a26e9c4a34dd60` were never the
  cause. NV checkpoints "stuck before the next segment" only meant the hung work had not reached a
  marker; they report the last command reached. The shader-hash special cases
  (`RE8_SCREEN_*_SHADER_HASH`, fast-copy hash) also violate the "no shader-hash behavior" rule.
- Temporary env diagnostics still in the code, to delete with the watchdog machinery:
  `BEER_BD12_ISOLATE_DISPATCHES`, `SKIP_DISPATCH_HASH`, `SKIP_BINDS`, `CLAMP_LOOP`, `CLAMP_GROUPS`,
  `NULL_BIND`, `TRACE_SHADER`, `NO_WATCHDOG`, `NO_WAVE_WAIT`, `WAVE_LOG`, `DUMP_SHADER_INPUTS`,
  `BEER_VK_CHECKPOINTS`, `BEER_PRESENT_SKIP_ALL`.
- The Steam runtime moved: set `BEER_VKD3D_SHADER_LIBRARY` to
  `~/.local/share/umu/steamrt4/steamrt4_platform_4.0.20260714.251823/files/lib/x86_64-linux-gnu/libvkd3d-shader.so.1.18.0`
  (without it every shader fails translation).
- Method that found it: per-command `vkCmdSetCheckpointNV` markers to find the hung draw, then logging the
  CBV bound to that draw's PSO. Inspect the bound data before blaming the shader.

Known open bugs: `cl_execute_indirect` ignores the count buffer for dispatch signatures and runs
`max_count` indirect dispatches; static samplers ignore shader visibility.

Each milestone has a self-contained test that runs without RE8
(`make test-d3d12v`).

## Performance state (2026-10-24, BD12, RTX 4070 Ti SUPER, RE8 title scene)

Goal: vkd3d-proton level (engine cap ~120 fps). Measure with `BEER_BD12_FPS=1` (integer-math logger; the
loader is built without SSE so varargs doubles print as 0), `BEER_BD12_PROF=1`, `BEER_BD12_SAMPLE=1`.

| Step | Title-scene frame time |
| --- | --- |
| Watchdog segmentation (wrong premise) | ~264 ms (3.7 fps): ~1,500 tiny submits + host waits per frame |
| Watchdog machinery deleted | ~19-20 ms (~50 fps) |
| Presenter no longer waits for its blit | ~11.5 ms (~85-87 fps); user later measured ~80 fps |

What was removed: `submit_watchdog_list`, `bd12_queue_submit_watchdog*`, dispatch tiling/isolation/screen-hash
and shader-hash special cases, input-dump/capture code, `BEER_BD12_*` watchdog diagnostics. `bd12_cmd_append_segment`
no longer takes a watchdog flag. The M1/M2 tests were rewritten for the plain submit path (M1-M4 pass; the COM
test was NOT re-run after the removal and appeared to hang: check `bd12_com_test.c` first).

Presenter change (vulkan_presenter.c, direct-image path): no `WaitForFences` after the blit submit. The next
present waits for the previous blit (1 frame of CPU/GPU overlap); per-swapchain-image `render_finished_image[]`
semaphores replace the single `render_finished`; `vulkan_presenter_wait_source()` is called (weak symbol) by
`com_reap`/`com_defer` in bd12_com.c before any resource is destroyed, because the previous blit may still
read a guest image. If a use-after-free of a present source ever appears, look here.

Profilers (all opt-in, in bd12_device.c / BD12_PROF_* macros in bd12.h, printed every ~2 s from queue_execute):
- `BEER_BD12_PROF=1`: ms/calls for execute, submit, pso compile, descriptor resolve, fence wait, present.
- `BEER_BD12_SAMPLE=1`: 2 kHz SIGPROF RIP histogram (module+offset via dladdr; resolve with addr2line on
  `./loader` offsets) plus 14-frame stacks of the render thread (`g_bd12_render_tid`, set in queue_execute).
  `perf` is blocked (perf_event_paranoid=4) and `gdb -p` fails (ptrace_scope=1); this is the substitute.

Findings at ~11.5 ms/frame (before the stack sampler was usable):
- Render thread ~96% CPU, GPU only ~37% busy: CPU-bound on the render thread.
- BD12 host work is small: execute 7 ms and submit 10 ms per 2 s; descriptor resolve ~220 ms per 2 s
  (about 1,300 resolves/frame, ~1.3 ms/frame, about 11%); present ~40 ms per 2 s after the change.
- RIP histogram showed a hot spin loop in `libnvidia-glcore.so.610.57.04` (offsets 0xdd0a25-0xdd0a75) on the render
  thread and on ~7 other threads (driver threads, 17-30% CPU each). The render thread's share (~45% of its samples)
  needs a call stack to attribute (which Vulkan call spins). Next step: run with `BEER_BD12_SAMPLE=1` and read
  `[BD12 STACK]` blocks.
- Every `com_submit` waits on the previous retire serial (ALL_COMMANDS): all submissions are serialised on the
  GPU, including across D3D12 queues (no async compute overlap). Candidate to relax.
- `bd12_resolve_descriptor_set` allocates and writes a fresh set per draw/dispatch; caching by
  (root-arg/descriptor hash) is the obvious reduction.
- The default (non-BD12) path was never perf-compared; it stays as the compatibility fallback.

## Validation gate after every slice

Run:

```bash
make test-d3d12
make test-renderer
make -B loader
git diff --check
```

Then one short `MemoryMax=6G` smoke run. Stop immediately on device loss,
rejection, OOM, staging failure, input/visual regression, or slower matched
workload timing.

## Gameplay bottleneck / pop-in investigation (RE8)

Symptom: objects pop in/out and get worse after a 180° turn, which points to a CPU/streaming bottleneck rather than a draw-distance problem.

Findings and fixes:
- `bd12_buffer_view` used a linear scan per descriptor resolve and was the render thread's top hotspot. It now uses a per-buffer open-addressed hash (`view_hash` in `BD12Buffer`). Descriptor resolve cost fell from ~3.5 us to ~1.1 us.
- `heap-create` cost ~0.8 ms per call because every DEFAULT heap ran a command pool + fill + submit + fence zero-fill. Image-owned heaps now skip it (`bd12_heap_create_unzeroed`, used by `bd12_image.c`). Cost is now ~0.15–0.27 ms per call. Buffer heaps still zero-fill. Batching those into a shared command buffer is a remaining TODO.
- Real occlusion and timestamp queries are implemented (query pools, Begin/End/Resolve). Before this, they were no-ops and resolved to zeros. They have not been tested in gameplay yet.
- `IDXGIAdapter2/3` is accepted, including `QueryVideoMemoryInfo`, backed by heap byte counters. The game queried it ~6000 times per session and was refused before.
- Still unsupported: multi-argument `ExecuteIndirect` signatures (work is dropped), dispatch-signature count buffers, `SetPredication` and `DiscardResource` (no-ops). `[BD12 DROP] cumulative:` lines are printed every 5 s when anything is dropped.
- Diagnostics: `BEER_BD12_PROF=1` (per-section timings, heap counters), `BEER_BD12_SAMPLE=1` (RIP histogram), `BEER_COM_DIAGNOSTICS=1` (refused QueryInterface IIDs).

### RE8 mesh pop-in: status as of 2026-10-24 (UNRESOLVED)
Symptom: in gameplay, floor/furniture/characters vanish and reappear when the camera turns or moves slightly. GPU-driven culling (ExecuteIndirect + compute compaction) yields wrong draw args.
Fixed on the way (did not cure it): D3D12 min/max sampler reduction decoding in `sampler_from_desc`.
Ruled out (user re-tested each): barriers (`BEER_BD12_SYNC_ALL`), CPU/GPU timing (`BEER_BD12_SYNC_SUBMIT`), dropped commands (no `[BD12 DROP]`), heap zero-fill, queries, oversized texel-buffer views (`BEER_BD12_VIEW_LOG`: none >128 MiB, no VIEWFAIL), null/misaligned views, upload memory coherency (coherent), subgroup-barrier patch of workgroup-memory accesses (`BEER_SPV_SYNC=1|2`, experimental, off by default).
`BEER_BD12_INDIRECT_FORCE=1` (ignore GPU count) brings some meshes back but the floor still flickers => per-draw instanceCount/visibility is wrong, not only the count.
Pipeline per frame: 4c024a9c (object classify: frustum/camera-inside/sub-pixel -> 0/1/2), af5e96c2 + ad2f25c1 + 06481fce (arg compaction; 06481fce uses 32-wide groupshared atomics with no barriers), 427af29f (meshlet/cone bounds).
Next step: capture one gameplay frame's cull inputs/outputs (cb, bounds, flags, args) via copy-to-readback and replay the shader on CPU to see whether the GPU result or its inputs are wrong.
New diagnostics: BEER_BD12_SYNC_ALL, SYNC_SUBMIT, INDIRECT_LOG, INDIRECT_FORCE, TRACE(+_FILE), DUMP_SHADERS=<dir>, VIEW_LOG, SPV_SYNC.

### DXR (ray tracing) groundwork — 2026-10-24
- `third_party/dxil-spirv` (HansKristian-Work/dxil-spirv, MIT, cloned with submodules) builds with `make dxil-spirv`
  into `third_party/dxil-spirv/build/libdxil-spirv-c-shared.so` (C API `dxil_spirv_c.h`). Intended to be dlopen'd like
  libvkd3d-shader, only for DXIL (SM6+) shaders and DXR libraries; DXBC stays on vkd3d-shader.
- Still to do, in order: enable VK_KHR_acceleration_structure/ray_tracing_pipeline/ray_query + features in
  `vulkan_context.c`; ID3D12Device5/CommandList4 (prebuild info, BLAS/TLAS build); state objects + SBT + DispatchRays;
  only then report RaytracingTier in OPTIONS5. RE8 shaders seen so far are all DXBC (SM5), RT libs would be DXIL.
- DONE (stage 1): `vulkan_context.c` now enables VK_KHR_acceleration_structure + ray_tracing_pipeline + ray_query +
  deferred_host_operations and their features when all are supported (`beer_vulkan_ray_tracing_enabled()`, opt-out
  `BEER_NO_RT=1`). Verified the RTX 4070 Ti SUPER device still creates and the title loads. No D3D12 RT API exposed yet,
  OPTIONS5 still reports NOT_SUPPORTED.
- DONE (stage 2, acceleration structures): `graphics/d3d12v/bd12_raytracing.c` implements
  GetRaytracingAccelerationStructurePrebuildInfo (vkGetAccelerationStructureBuildSizesKHR), BuildRaytracingAccelerationStructure
  (BLAS triangles/AABBs, TLAS instances, update), CopyRaytracingAccelerationStructure (COMPACT runs as CLONE, reported
  compacted size == current size) and EmitRaytracingAccelerationStructurePostbuildInfo. AS objects are created lazily per
  (buffer, offset) and freed with the buffer. D3D12 instance descriptors have the same 64-byte layout as
  VkAccelerationStructureInstanceKHR and AS address == buffer VA (verified at runtime, logged as `[BD12 RT] ... matches VA`),
  so no translation pass is needed. COM: ID3D12Device3/4/5 and GraphicsCommandList1-4 IIDs are answered only when RT is
  available (vtables extended; unsupported slots are no-ops/E_NOTIMPL). Buffers get AS-storage/build-input/SBT usage when
  RT is on. Test: `make test-bd12-rt` builds a triangle BLAS+TLAS and traces two rays with a ray-query shader (PASS).
- STILL TO DO: DXIL shader path via dxil-spirv (inline ray query in SM6.5 shaders; DXR libraries), CreateStateObject +
  shader tables + DispatchRays (vkCmdTraceRaysKHR), CheckFeatureSupport OPTIONS5/OPTIONS12 tier reporting (keep
  NOT_SUPPORTED until DispatchRays works). Note the file bd12.h was accidentally truncated and rebuilt from a backup plus the
  logged edits on 2026-10-25; it compiles and runs at 120 fps, but diff it against intent if something looks missing.
- DXIL/DXR design findings (2026-10-25): dxil-spirv exposes the full vkd3d-proton API (`dxil_spirv_c.h`: remapper callbacks for
  SRV/UAV/CBV/sampler bindings, RDAT subobjects, physical-storage-buffer and bindless options). BD12's current binding model
  builds ONE static VkDescriptorSet per draw/dispatch from the bound root signature (texel buffers for raw/structured buffers),
  which cannot express DXR: local root signatures are per shader record, and hit shaders index descriptor tables/heaps
  dynamically. DXR therefore needs a bindless model: mirror D3D12 descriptor heaps into update-after-bind VkDescriptorSets
  (descriptor indexing is already enabled on the device), pass root arguments via push constants/VA, and let dxil-spirv's
  remappers target that layout. No DXC binary is available on this host to produce test DXIL (VS Code/IntelliJ ship Windows
  dxcompiler.dll only); real DXIL has to come from the game.
- DXR probe results (2026-10-25, `BEER_DXR_PROBE=1`, optional `BEER_DXR_PROBE_SM=<hex>`; reports OPTIONS5 tier 1.1, SM override opt-in):
  with RT off in config.ini RE8 ignores DXR. With RT ON (config `RayTracing=true`) RE8 at boot calls
  `CreateStateObject` thousands of times (type 3 RAYTRACING_PIPELINE, 11 subobjects: DXIL_LIBRARY 150-176 KB with 6 exports,
  4 HIT_GROUP, shader config, pipeline config, global+local root signatures) and also creates DXIL vertex shaders and a DXIL
  compute shader (`cd27d3a4...`) that vkd3d-shader cannot scan (-3). So DXR needs dxil-spirv for ALL stages (VS/PS/CS + RT libs),
  not just RT. `config.ini` RayTracing was reset to false after probing (the game rewrites it from the menu toggle).

### CANONICAL STATUS: RE8 mouse buttons ignored for gameplay actions (updated 2026-10-25) — ROOT CAUSE FOUND AND FIXED (needs user verification in gameplay)
**Symptom.** In RE8 the mouse buttons do nothing for gameplay actions (aim with RMB, fire with LMB, RMB-cancel / photo-frame put-back) and right-click-to-close does nothing in menus; left clicks on on-screen UI buttons work; keyboard (Esc included) works; mouse look works. Esc not closing some menus is intended game behaviour, not part of this bug. With the game's "invert mouse buttons" setting the primary click moves to the right button, which is consistent with a gate in front of the action layer rather than a button mapping problem.
**Root cause (isolated by instrumentation, user-verified).** RE8 keeps the `held/pressed/repeat` action predicates behind a *cursor-inside-active-viewport* check. In this build the input-mode getter returns `0x2101` (effective mode 1); for mode 1 the predicates first call the function at `0x1445fce90` (img offset `0x45fce90`) and only accept the (otherwise correct) button mask when it reports that the game's internal pointer lies inside the active viewport. Mode 7 skips the check. With Beer that call returns false, so every action predicate is rejected although the button bits are correct. UI buttons use a different path (cursor hit-testing), which is why they still work.
**Verified correct, so NOT the cause** (do not re-investigate): X11 -> WM_*BUTTON* messages, WM_INPUT RAWMOUSE `usButtonFlags` (RE8 uses the raw-input backend, class B vt `0x1472c7ec8`; the DirectInput mouse wrapper is never selected), DirectInput state/size/caps/EnumObjects/GUID path, button order (DirectInput numbering fixed earlier via `di_index[]`), device identity (`GetRawInputDeviceList/Info`), `GetSystemMetrics` (mouse present, 5 buttons), WM_INPUT wParam (RIM_INPUT while focused), per-device state `A+0x120/+0x128/+0x12c`, merged global state (RMB=2, LMB=1 and LMB+RMB=3 observed correct in gameplay), action mask resolution (aim fallback mask 2 stays 2, no LMB/RMB swap), the final held-bit comparison (match=1), GetAsyncKeyState/GetKeyState polling, input hooks (none installed), XInput (disconnected), foreground/active/focus answers, ClipCursor/SetCursorPos/ShowCursor/SetCapture (never called by the game), QPC, Steam remote-storage keybind persistence. Defensive fixes made on the way are real and stay (stable raw-input slots, DirectInput size check and caps, SM_* values, Steam async-read lifecycle) but none changed the symptom.
**FIX APPLIED (2026-10-25, VERIFIED BY THE USER, bypass and all RE8 mouse debug code removed).** The "cursor over the client area" flag the gate tests is byte `+0xaa` of the window object (`[[mgr+0x48]+0x10]` with `mgr` at `0x14c49f418`). RE8's window procedure sets it in the `WM_NCHITTEST` handler (`0x144265b0c`..`b19`: `DefWindowProcW(hwnd,msg,wp,lp) == 1` i.e. HTCLIENT) and clears it for the other branch; a second writer family (`0x144981b7a`/`cb5`) sets it to 1 when display mode (+0xfc) is 5, else to `(+0xd8 == 0)`. Beer never sent `WM_NCHITTEST` (Windows sends it, with the screen position in lParam, before WM_SETCURSOR and mouse messages) and its `DefWindowProcW` returned 0 for it, so the flag stayed 0 forever (measured: 43,019 gate calls, `aa=0` every time, the position test was never reached). Changes in `loader.c`: `post_wm_setcursor` now also queues `WM_NCHITTEST` (lParam = screen x/y) before `WM_SETCURSOR`; `impl_DefWindowProcW` answers `WM_NCHITTEST` (HTCLIENT inside the client area, HTCAPTION/HTBORDER for a decorated frame, HTNOWHERE outside the window). Measured after the change: `aa=1` on 25,855 of 25,855 calls and pointer positions inside the 2560x1440 viewport, so the gate returns true naturally. The user confirmed aim/fire/cancel work; the opt-in bypass and the RE8 mouse instrumentation were then removed. Lesson: when a game stores state from a message handler, check that Beer sends the message at all (NCHITTEST/SETCURSOR/MOUSEACTIVATE/ACTIVATE families).
**Workaround (REMOVED).** `BEER_RE8_CURSOR_GATE_BYPASS` patched `0x1445fce90` to `mov al,1; ret`; deleted after the real fix was verified.
**Proper fix (historical plan, superseded by the FIX APPLIED paragraph above).** Make Beer's cursor/viewport state satisfy RE8's check naturally. Next steps: (1) disassemble `0x1445fce90` (dump with BEER_CALLER_DUMP/BEER_MODULE_RANGE or gdb `x/60i` at `g_img+0x45fce90`) and identify exactly which inputs it compares: most likely the internal pointer position (fed by raw relative deltas and/or `GetCursorPos`/`ScreenToClient`) against a viewport rectangle (fed by `GetClientRect`, window/client size, DPI, or the swapchain/back-buffer size, possibly the pre-borderless size); (2) with `BEER_PEEK_OBJ`/gdb read both at the moment of an RMB press in gameplay and see which is out of range (e.g. pointer pinned at 0,0 or at a stale pre-resize coordinate while the viewport is 2560x1440, or a viewport still 0-sized); (3) fix the producing API (`GetCursorPos`, `GetClientRect`, `ClientToScreen`, relative-mode pointer integration, or window-size messages), re-test with the bypass OFF, then delete the bypass. Related existing facts: Beer uses a relative pointer (XI2 raw deltas) in gameplay and the window is borderless/resized, both plausible sources of the mismatch.
**Tooling note.** The RE8 mouse instrumentation (BEER_RE8_ACTION_TRACE, BEER_PEEK_OBJ/PEEK_ACTIVE, BEER_WATCH_*, BEER_CALLER_DUMP/MODULE_DUMP/MODULE_RANGE/PTR_SCAN, BEER_DI_NO_ENUM, RE8-specific hook signatures) was removed on 2026-10-25 once the cause was found; the generic tools (BEER_HOOK_LOG, BEER_PATCH_BYTES, BEER_API_TRACE, BEER_INPUT_DIAGNOSTICS, BEER_MESSAGE_DIAGNOSTICS) remain. Technique that found it: break at the failing predicate in gdb, read the byte it tests, then scan the executable image for writers of that field offset and disassemble them.
**Note on parallel work.** `platform/win32_input.h` and `platform/directinput_format.h` had uncommitted edits made outside the RE4 sessions (added RawInputDeviceList/Info types); not part of this entry.

### (history, superseded by the canonical section above) Mouse button input problems in RE8 menus (reported 2026-10-25)
Symptoms: only LEFT click on on-screen buttons works. Right click never closes menus (also reported earlier) and ordinary
left clicks elsewhere are intermittent. With the game's mouse buttons inverted in settings, RIGHT click then works for
buttons but LEFT does not register. Keyboard/gamepad-style "press" on buttons always works. The user got stuck in settings
because right-click-to-back does nothing.
Suspicions (not verified): (1) RE8 reads buttons through Raw Input (WM_INPUT RAWMOUSE usButtonFlags) and/or polled
GetAsyncKeyState/GetKeyState(VK_LBUTTON/VK_RBUTTON/VK_MBUTTON); only the WM_*BUTTON* message path is delivered correctly,
so any button the game reads via the other path is missing (the earlier side-button investigation showed raw+WM events were
delivered, but polled key state was never checked). (2) Edge-triggered polling: a fast down+up between two game polls is lost if
button state is only visible through events. (3) Button swapping: `SM_SWAPBUTTON`/`GetSystemMetrics`, `SwapMouseButton` and
the logical-vs-physical mapping after inversion look unhandled. (4) Clicks dropped while the pointer is re-captured/locked
when the cursor mode changes (menus <-> gameplay). Next step: run `BEER_INPUT_DIAGNOSTICS=1`, press each button in a menu and
compare delivered WM_INPUT flags, WM_*BUTTON* messages and GetAsyncKeyState results.

### RE8 pop-in investigation, findings after frame capture (2026-10-25) — STILL UNRESOLVED, safe to park
Tooling added (all opt-in, no effect on normal runs): `BEER_BD12_TRACE_FILE=<path>` (touch it to trace the next present, re-armable),
`BEER_BD12_CAPTURE_DIR=<dir> BEER_BD12_CAPTURE_CS=<hash-prefix,...>` (copies every bound buffer of the selected compute
shaders before/after each dispatch of the traced frame, 16 MiB clip, thread-safe; WARNING ~2 GB per frame on /tmp tmpfs),
`BEER_BD12_CAPTURE_TWICE=1` (re-run each dispatch to check determinism), `BEER_BD12_BIND_LOG=1` (every compute buffer binding
with GPU VA during the traced frame), `BEER_BD12_FORCE_INST=1|2` (draw ExecuteIndirect from an arg copy with instanceCount
forced to 1; 2 also ignores the GPU draw count), `BEER_SPV_FLOAT_CONTROLS=0` (disable SignedZeroInfNanPreserve, default on),
`BEER_SPV_SYNC=1|2` (subgroup barriers around workgroup memory; no effect). Python replays live in the session scratchpad only.
Verified by CPU replay of captured bad frames: classification shader 4c024a9c (667 objects, 0 mismatches), draw-arg shader
af5e96c2, merge/compaction shader 06481fce (90 groups: GPU count == CPU runs == non-zero records), 061bd7a2 camera-inside-box
flags (0 mismatches). ad2f25c1 is deterministic apart from its in-place list compaction. So the GPU culling/compaction chain computes
exactly what its inputs say on this GPU; the camera constants are self-consistent (3 side planes meet at cb0[7..9].w).
Experiments: FORCE_INST=1 (inst=1, GPU count kept): no change. FORCE_INST=2 (ignore GPU count too): characters such as Mia no
longer pop in/out, the black floor patch is unchanged. => character draws are dropped by the GPU count/instance data, the floor
is NOT dropped by that mechanism. The floor behaves like a view-dependent lighting/visibility input: moving the camera slightly up
turns it black, down restores it; distance also matters. Capture of 061bd7a2 shows nested room-volume boxes (e.g. 15.6x4.3x16.8 m,
a 15.7x0.47x16.6 m slab at y -500.3..-499.8) with camera y = -498.39 inside the large ones only; flag 65535 means camera inside.
Screenshots show the furniture lit and the wooden floor itself black in a polygonal patch, so the floor is possibly drawn but
unlit/with wrong data rather than culled. Not yet checked: which pass produces floor lighting (clustered light culling 0d8b3b7d,
bd066b41, GI/probe volumes), skinned-mesh bounds from 413631d7/47a89d6c (characters), lavapipe cannot be used (device lost at
first submit). Natural next step: capture 0d8b3b7d/bd066b41 (+ their light lists) in a bad vs good frame and replay.
Also done this session: DXIL (SM6) shader path (`graphics/d3d12/d3d12_dxil_translator.c`, dxil-spirv, bindless descriptor-heap mirror
`bd12_mirror.c`; enable with `BEER_DXR_PROBE=1 BEER_DXR_PROBE_SM=66`, title scene verified correct), acceleration structures,
see sections above.

### Pop-in investigation, latest narrowing (2026-10-25)
Video of the bug (frames at 2 fps) shows the black floor/wall regions have a STEPPED edge matching the 104x117 px light-grid tiles
(32x16 tiles over 3328x1871, 32 exponential depth slices, 3D uint grid + light-mask list). Light culling pass 0d8b3b7d was replayed on
the CPU for bad and good frames: 0 mismatches over 2112 sampled cells each (offset in the grid value is a BYTE offset). So the lighting
data is computed correctly for the lights the game submits.
Draw-list diff, bad frame vs good frame (BEER_BD12_CAPTURE_DRAWS=1 + trace trigger): the game's CPU does NOT issue three whole
ExecuteIndirect calls in the bad frame (PSOs bc397350 / bb12e281 / 1aa6c38d, single draws of 4944 / 600 / 576 indices, count word 65535 =
"camera inside box" flag from 061bd7a2, args at buffer de9b2808xx; PS 4eaa7c68 is a 3098-line deferred light-volume shader with a
shadow comparison sampler and Texture2DArray), and issues two extra direct draws of PSO d23c868f (VS a058c4b5, PS 79b3ee1f: a trivial
ray-direction/sky pass, no depth/cull/blend). Partially: PSOs 1130bacd and 04be3b67 draw fewer records in the bad frame.
=> the decision not to draw those light volumes is made by the game on the CPU, from data we supply (timing, queries, readbacks,
fences, DXGI/window properties, input) or from its own camera logic. GPU timestamps resolved per frame: one pair, delta ~23 us
(BEER_BD12_TS_LOG=1); not obviously wrong. Open candidates: CPU readbacks of GPU data (readback heap contents/fence timing),
frame-time based quality scaling, camera-inside tests on CPU. Diagnostics added: BEER_BD12_PSO_LOG (PSO state + VS/PS DXBC hashes),
BEER_BD12_CAPTURE_DRAWS, BEER_BD12_TS_LOG, [DRAW] lines in traced frames.

### RE4 Remake (re4.exe) bring-up — paused 2026-10-25
Game dir: /run/media/abhineet/56A4064AA4062CD5/Games/Resident Evil 4/re4.exe (RE Engine, steam_emu.ini/steam_api64.rne present).
Progress so far (all general loader fixes, nothing RE4-specific):
1. First crash: RIP=0 call through a NULL pointer. The exe resolves ~370 Win32 exports by name via GetProcAddress that the loader lacked
   (console, registry, services, user32/gdi32, ntdll Rtl*/Nt*). Added `platform/win32_missing_exports.inc` + `_table.inc` (included from loader.c):
   proper GetConsoleMode/GetConsoleOutputCP/GetConsoleCP/MulDiv/RtlInitUnicodeString/RtlInitString/etc., safe `return 0` stubs for the rest.
2. Next stop: the game calls ExitProcess(204) very early (after one VirtualAlloc of 0x3d0900 bytes RWX and RtlAddFunctionTable, before D3D12).
   Disassembly of the guest code around the exit (BEER_EXIT_DUMP=<dir> dumps code around the exit callers; BEER_VM_LOG=1 logs VirtualAlloc)
   shows a normal exit path reading a stored status 204, i.e. a deliberate failure decision inside the game, no further API calls before it.
3. Suspected cause: `IsProcessorFeaturePresent` always returned 0 (RE4R wants SSE4.2/AVX/AVX2). It now reports the host CPU's real features
   (winnt.h PF_* numbers). This change is built but NOT yet verified against RE4 (the verification run was interrupted).
Next steps when resumed: run re4.exe again; if still ExitProcess(204) check other CPU probes (GetSystemInfo, cpuid leaf use, XState / GetEnabledXStateFeatures,
IsProcessorFeaturePresent PF 40/41 callers), then DXGI/D3D12 bring-up as for RE8 (re4 imports d3d11.dll + dxgi.dll only, d3d12 is loaded dynamically from ./D3D12/D3D12Core.dll).
New diagnostics: loader.c prints stack/regs on the first low-RIP fault, BEER_EXIT_DUMP, BEER_VM_LOG.

### Pop-in investigation, mechanism found but cause still open (2026-10-25, late)
Good/bad frame pairs (camera moved ~1 mm) show: exactly one floor draw list flips, ExecuteIndirect args list of 43 meshlet draws (768 indices each,
PSOs 04be3b67 depth pass + 1130bacd G-buffer) with GPU count 0 (bad) vs 10 (good); a second light-volume draw (PSO 2a58644e/ac4081fb, 2226 indices,
count 0 vs 65535) flips too. All other lists are identical. For the floor list the compaction INPUT already has instanceCount (inst&1) = 0 for all 43
records in the bad frame and 1,0,1,0,... (10 visible) in the good frame, before the first classification dispatch, so the per-meshlet visibility is produced
upstream: not by 4c024a9c classification, 980a49dd reset (its table covers other offsets), compaction, or light culling (all CPU-replayed correct). It reaches the args buffer through
the many tiny copy dispatches 31b662b4 from CPU-prepared data, i.e. the game's CPU decides per-meshlet visibility.
The CPU reads GPU data back every frame (BEER_BD12_RB_LOG / BEER_BD12_RB_DUMP): a 16 KB buffer of camera-inside flags (d51baaf2 output, identical in good and bad frames)
and a 1 KB buffer written by tiny pixel shaders (store_raw u0[objectId*4] = 1 or 65535 from a flat-interpolated object id: occlusion-proxy visibility feedback);
the 1 KB buffer is ALL ZERO in both frames. Whether that is expected (the proxies it covers may be inactive in this scene) or a missing pixel-shader UAV write
on our side is the best remaining lead: check that fragments of proxy draws with UAV stores (PS 55633dc3, 7f35670b, b2af2f17; PSOs 0aa7ff08, bbfb4991 (26 draws/frame), 3e310aa0)
actually execute and write. Other candidates: depth-test-vs-UAV-write ordering for those pixel shaders (D3D writes UAVs for fragments that fail depth), and readback latency.
Tools added: BEER_BD12_RB_LOG, BEER_BD12_RB_DUMP, BEER_BD12_CAPTURE_DRAWS (see earlier notes).

### FIXED: RE8 meshes/floor/characters popping in and out (2026-10-25)
Cause: GPU occlusion culling by proxy rendering. Tiny pixel shaders (forceEarlyDepthStencil, no colour target, depth test GREATER_EQUAL without write, 4x MSAA depth)
draw per-object bounding boxes and store 1 into the draw-argument buffer's instanceCount where the proxy passes depth. Boxes hugging the floor/characters resolved
their coplanar depth comparison differently from D3D hardware, so whole batches were marked invisible for tiny camera moves. All culling/compaction/light shaders had been
CPU-replayed and were correct; the bug was only in the proxy depth test.
Fix: `bd12_graphics.c` makes pipelines of that shape (no render target, depth test without write, pixel shader with UAV bindings) always pass depth (conservative; only the
occlusion-culling optimisation is lost). Default on, `BEER_BD12_PROXY_ALWAYS=0` disables. User verified: "works perfectly".
Possible refinement later: find why the proxy depth compare differs (depth bias/precision/MSAA depth, VS position invariance) and restore real occlusion culling.
- Follow-up (2026-10-25): tried `BEER_BD12_PROXY_BIAS=<n>` (depth-bias the occlusion proxies toward the camera instead of always passing, n ULPs; opt-in, default off).
  The user reports the pop-in returns whenever the always-pass behaviour is off, so ties are not the whole story; the always-pass default stays. Root cause of the
  differing proxy depth comparison is still unknown (candidates: SV_Position invariance/FMA contraction between the proxy VS and the geometry VS, 4x MSAA depth with forced
  early depth + UAV stores, sample-position/rasterisation differences). Revisit later; current behaviour is correct but without proxy occlusion culling.

### (history, superseded) Mouse buttons in RE8 (2026-10-25) — button storage order fix
Fixed: button storage order. DirectInput numbers mouse buttons left, RIGHT, middle but the loader stored X11 order (left, middle, right), so DirectInput polled
state and buffered events swapped right and middle. Buttons are now stored by DirectInput index (`di_index[] = {0,2,1}` in loader.c) and the MK_* flags follow.
Verified with BEER_INPUT_DIAGNOSTICS=1 + BEER_MESSAGE_DIAGNOSTICS=1 for a right click: X11 button 3 -> WM_RBUTTONDOWN (0x204) taken by the game's message loop,
WM_RBUTTONUP (0x205), raw input flags 0x0004/0x0008 read through GetRawInputData, and DirectInput GetDeviceState reports `00 80 ...` (right slot) for the whole press (~15 polls).
All three channels deliver right click correctly, yet RE8 does nothing with it (menus do not close, photo frame cannot be put back); left click works on UI buttons only.
User observation: with the game's "invert mouse buttons" setting, right click acts as the primary click on buttons and left then does not act as cancel, i.e. the game's
secondary/cancel action never fires regardless of which physical button. Not yet explained. Ideas: game waits for another message (WM_CONTEXTMENU 0x7B / double-click / capture state),
reads raw `ulRawButtons` or another field of RAWMOUSE for held state, or gates secondary buttons on a state we do not provide (GetCapture/GetCursorInfo/foreground state).
Also note: in the rebind screen middle/X1/X2 did not register either, i.e. every non-left button.
Diagnostics: take-message log now always logs mouse button messages (0x201-0x20e), `buttons held` log unlimited for non-left buttons.

### Rare corruption after the first cutscene (user report 2026-10-25) — suspected fix applied, unverified
Symptom: after the opening cutscene at the road crash, lighting was badly wrong and the pause menu showed old "skip cutscene" text mixed in; a restart fixed everything.
Only happens when reaching that point from a fresh start (several minutes), not reproducible on demand. Looks like stale (uncleared) texture contents.
Suspect: an earlier perf change made committed IMAGE heaps skip the zero fill (`bd12_heap_create_unzeroed`). D3D12 zero-initializes committed resources and games rely on it;
device memory recycled after the cutscene's resources are freed can contain old pixels. Change: committed images are zero-filled again (`bd12_image.c`);
`BEER_BD12_NO_IMAGE_ZERO=1` restores the old behaviour. Watch for loading stutter (each heap zero fill is ~0.6 ms); batching the fills is the follow-up if needed.
If the corruption recurs: note exactly what was on screen, and re-run with `BEER_BD12_PROF=1` to see heap-create cost; also consider DiscardResource/ClearRenderTargetView semantics (DiscardResource is currently a no-op).

### FIXED: progressive flicker -> bland frames -> garbage UI after a few minutes of play (2026-10-25)
Cause: buffer-view cache leak. RE8 binds slices of ring buffers at a new offset every frame; BD12 cached a VkBufferView for every distinct (format, offset, range) and
never freed any. After ~520,000 views the driver returned VK_ERROR_OUT_OF_DEVICE_MEMORY (-2) for further views (log: `[BD12 VIEWS] created=521621 failed=... last=-2`), shaders
then read nothing, culling/lighting data went missing and the picture degraded (also seen as corrupted pause-menu text).
Fix (`bd12_descriptors.c`): (1) heap SRV/UAV buffer descriptors no longer create views when written; the view is looked up when a draw resolves the descriptor
(`d->format`, `d->source_buffer`, offset, range); (2) cached views carry `last_use` (device epoch, advanced once per presented frame in `dl_present`) and `pinned`;
when a buffer holds >= 4096 views, those unused for 16 frames are destroyed and the cache is compacted/rehashed; on a creation failure idle views (2 frames) are freed and
creation is retried; (3) ranges are floored to a whole number of texels (Vulkan requirement); (4) the bindless heap mirror uses pinned views. `BEER_BD12_VIEW_LOG=1` prints counts/failure codes.
User verified a long session stays stable and screenshots look correct.
Earlier "rare corruption after the cutscene" was most likely this same leak. Committed images are still zero-filled (kept: matches D3D12 semantics).
Other findings this session: a one-off `device lost` at startup (not reproduced in 4 follow-up runs); the machine's swap was full during the long test (gnome-shell ~4 GB), unrelated to the loader.

### (history, details kept; summary is in the canonical section) RE8 mouse gameplay actions — cursor/viewport gate isolated (2026-10-25)
Symptom: left/right click ignored for aim/fire/cancel in gameplay and menus (UI buttons clickable); keyboard (incl. Esc) works; mouse look works.
Verified correct (all via opt-in diagnostics, see below):
- X11 -> WM_*BUTTON*, WM_INPUT (RAWMOUSE usButtonFlags) and DirectInput state all carry the buttons.
- RE8 default mouse backend is RAW input (factory 0x14460e460: config [g+0x5ac]=5 "auto" -> [g+0x5a8]=2 = raw wrapper class B vt 0x1472c7ec8). DI mouse wrapper (class D, vt 0x1472c7ea8, update 0x14460f4d0) is never selected, same as real Windows would.
- Raw wrapper accumulates flags at [B+0x44], per frame f670 (0x14460f670) turns them into held bits at [B+0x60]; copied to mouse state [A+0x120] (A = mouse device, f460 0x14460f460 is its poll); edges at +0x128/+0x12c computed by 0x1445fc560; merged (0x1445fc6e0) A -> group -> global input state dst (+0x124 held, +0x128 pressed, +0x12c released, +0x130 repeat). In gameplay the global dst+0x124 shows 2 for RMB and 1 for LMB, so the game's final input state IS correct. The failing part is above that (action/binding layer) or game-state gating, not delivery.
- Lost clicks possible when down+up land in one frame (raw f44=0003) but holds are fine.
Fixes made (faithful to real DirectInput, did not change the symptom): GetDeviceState rejects size != data format size (game probes mouse/keyboard with 272-byte joystick buffers); GetCapabilities reports DIDC_ATTACHED and real device types (0x0112 mouse, 0x0413 keyboard); EnumObjects now really enumerates objects (callback was never invoked); GetProperty(DIPROP_GUIDANDPATH) fills GUID/path.
Ruled out: input hooks (game installs none), GetAsyncKeyState/GetKeyState polling, XInput pad (reports disconnected), ClipCursor/SetCursorPos/ShowCursor/SetCapture (game never calls them), foreground/active window answers, QPC.
Open ideas: action-layer binding/context gating (cancel via Esc also fails, so maybe not mouse-specific), game script state stuck (e.g. tutorial/lock), per-button hold timers (A+0x134/+0x138 use [g+0x350] frame delta which is 0 and unused, wall-clock path is used), ring-buffer of per-frame device snapshots at [rsi+0x3b0].
Diagnostics added (all env gated, guest code patching is DIAGNOSTIC ONLY): BEER_PEEK_OBJ=1 (hooks 0x14460f670/f4d0/f460/0x1445fc6ed, prints raw/mouse/merged state), BEER_WATCH_BUTTONS=1 (+BEER_WATCH_GLOBAL) page-protect watch logging RIPs, BEER_CALLER_DUMP / BEER_MODULE_DUMP / BEER_MODULE_RANGE / BEER_PTR_SCAN (dump guest code to files), BEER_DI_NO_ENUM=1, BEER_INPUT_DIAGNOSTICS=1 extra DI stub/caller logs.

### (history) Mouse actions: later checks (2026-10-25)
Also fixed: GetSystemMetrics now returns realistic values (SM_MOUSEPRESENT=1, SM_CMOUSEBUTTONS=5, wheel present, 1 monitor...); WM_INPUT wParam is RIM_INPUT(0) while focused (was always RIM_INPUTSINK(1) because the game registers RIDEV_INPUTSINK).
Measured: input update 120/s, edge calc 120/s, merge 240/s (1 poll/frame). Global input state shows RMB=2/LMB=1 correctly in gameplay. The action wrappers at 0x1415ae7xx-ae9xx consume that state through the VM boundary.

Action-trace follow-up, in chronological order:
1. A left click on a UI control reaches the managed `pressed` wrapper and resolves mask `1`, with callers at `0x14066f1e9` and `0x14189a6cb`. Repeated right clicks in a menu do not query an RMB-bound action, so right-click-to-close was not a reliable test of gameplay aim.
2. RE8's device edge computation receives RMB correctly (`previous=0, current=2, pressed=2`) and LMB-while-RMB-held correctly produces `current=3`. Raw Input delivery and edge generation are therefore correct.
3. Armed-gameplay tracing shows the shared resolver preserves aim fallback `2` as mask `2`; there is no LMB/RMB swap. The held predicate repeatedly reads global held value `2` for mask `2`, yielding `match=1`. Device identity/capabilities, saved bindings, action-mask resolution, and the final held-bit comparison are not the cause.
4. The rejecting precondition is RE8's cursor-inside-active-viewport gate at `0x1445fce90`. This build's mode getter returns `0x2101` (effective mode `1`), for which RE8 requires that viewport test before accepting the otherwise-successful action predicate. Earlier trace output incorrectly labelled the first byte of the mode object (`8`) as the effective mode.
5. `BEER_RE8_CURSOR_GATE_BYPASS=1` is a version/signature-guarded opt-in patch that makes only this viewport predicate return true. User verification with an equipped weapon confirmed that aim/fire work when it is enabled. This isolates the root cause to Beer's cursor-coordinate/active-viewport contract, not button delivery. The bypass is the currently verified workaround; the proper general fix is still to make the cursor/viewport state satisfy RE8 naturally, then remove the RE8-specific patch.

Instrumentation notes: `BEER_RE8_ACTION_TRACE=1` traces raw edges, merges, resolver masks, and held/pressed/repeat predicates. One experimental post-call predicate hook crashed because it relocated a relative conditional branch into a trampoline; those unsafe hooks were removed. Current entry probes are branch-safe. An earlier edge probe forwarded saved RAX while labelling it R8D; hook ID 8 now explicitly forwards saved R8. All action probes and the cursor-gate bypass are executable-signature guarded and off by default.

Defensive fix (not the RE8 root cause): raw-input handles were just `(ring index + 1)` and `store_raw_input` unconditionally overwrote that 256-entry ring. The independent 1 kHz native pump could therefore replace a packet referenced by an already-queued WM_INPUT before it was consumed. Raw packets now occupy stable slots until a successful RID_INPUT copy; queue failure releases its slot, and unbuttoned motion is dropped rather than overwriting an action edge when slots are exhausted. Unit coverage distinguishes coalescible raw motion, wheel packets, and button transitions. Later RE8 tracing proved held LMB/RMB bits reach the game's final global input state, so this correctness change did not resolve the action-layer failure.

Device identity follow-up: added coherent `GetRawInputDeviceList` and `GetRawInputDeviceInfoA/W` contracts for the same synthetic mouse and keyboard handles carried in `RAWINPUTHEADER`. Raw Input and DirectInput now use the same Beer-owned HID paths, and mouse capability reporting consistently exposes five buttons through Raw Input, DirectInput, and `SM_CMOUSEBUTTONS`. User verification showed this did not resolve RE8's action-layer rejection; device identity/capability probing is ruled out for this symptom.

Steam async-read lifecycle follow-up: `steam_utils_get_api_call_result` cleared every completed call record. For `RemoteStorageFileReadAsyncComplete_t` that is premature: the completion result only announces the ready range, and the game must subsequently call `FileReadAsyncComplete` to copy the bytes. The cleared record made that transfer fail. Read-call records now survive result retrieval and are consumed only by a successful `FileReadAsyncComplete`; write results retain the old one-phase lifecycle. This is a concrete path by which persisted action/keybind data could fail to load while raw input and UI hit-testing remain healthy; gameplay verification is pending.

### Steam ownership / DLC (2026-10-25) — FIXED
Cause of RE8 missing the third-person perspective, extra difficulty and many options: our built-in ISteamApps answered false to everything (BIsSubscribed, BIsDlcInstalled, GetDLCCount=0). No network involved. Now ISteamApps reports ownership from files next to the game: steam_emu.ini [Settings] AppId and [DLC] list (DLCUnlockall=1 unlocks all), steam_settings/DLC.txt, steam_settings/configs.app.ini [app::dlcs], steam_appid.txt. Implemented BIsSubscribed, BIsSubscribedApp, BIsDlcInstalled, GetEarliestPurchaseUnixTime, GetDLCCount, BGetDLCDataByIndex, BIsAppInstalled, GetAppInstallDir; unimplemented slots log under BEER_STEAM_DIAGNOSTICS=1. RE8 enumerates all 6 DLC ids from its steam_emu.ini. User confirmed the missing options/perspective/difficulty now appear.

### New RE8 reports (user, 2026-10-25, not yet investigated)
1. **Audio "blip"**: occasionally a loud short pop/blip in the audio during play. Not reproducible on demand yet. Suspects: PipeWire/Pulse backend buffer underrun or a stale/overrun ring buffer (BEER audio bridge for DirectSound/waveOut/FMOD output), a buffer submitted with wrong length, or a timing hiccup when the loader pauses (shader compile, loading). Next step: reproduce while running with audio diagnostics (`[AUDIO]` log lines, BEER_AUDIO_* env if present) and note what the game was doing at the time (loading, cutscene change, menu); check for xruns in `pw-top`/`journalctl --user -u pipewire` at the same moment.
2. **Missing texture on a haystack** (single object): in the snowy swamp area (screenshot `/home/abhineet/Pictures/Screenshots/Screenshot From 2026-10-25 20-47-18.png`, 117 fps) a hay-stack-like object renders as a flat dark blue-grey shape with ragged edges, with no material/texture (looks like unlit fallback or an unbound/black texture slot). Only one instance seen. Suspects: a texture that failed to stream/upload (zero-filled/never initialised image, view GC, mip streaming race), a descriptor resolving to a null/default view, or a shader permutation that failed to translate. Next step: when it reappears, capture the frame (`BEER_BD12_CAPTURE_RT=1` with `BEER_BD12_TRACE_FILE` touch) and check `[DROP]`/`BD12_DROP` counters, `BEER_BD12_VIEW_LOG=1` view failures, and whether the same object is fine after moving away and back (streaming) or permanently black (shader/descriptor).
Other: user notes some performance optimisation is still wanted (deferred).

### Sekiro / D3D11: plan for a BD12-style Vulkan mapping ("BD11"), started 2026-10-25
State today: Sekiro boots to the title screen (after the X11 fix below) on the OLD D3D11 path: `graphics/d3d11/d3d11_impl.c` (6.5k lines) plus `renderer/vulkan/vulkan_indexed_renderer.c` (a "typed shader / indexed draw" renderer that recognises specific shaders, not a general D3D11 implementation). That is the same dead end BD12 replaced for D3D12. Fix made now: `XSetInputFocus` was sent to a window that was only *requested* to map (X `BadMatch`, process killed). `platform/xwayland_backend.c` now has a `viewable` flag set only on MapNotify, and a non-fatal X error handler (logs `[XWAYLAND] ignored X error ...`).
Goal: a general D3D11 -> Vulkan layer reusing the BD12 core, like D3D11On12/DXVK in spirit but on our own foundation. Reuse from `graphics/d3d12v/`: device/queue/fence/timeline sync (`bd12_device.c`), VA/BDA buffers and heaps, images and format table (`bd12_image.c`, `bd12_formats.c`), descriptor machinery and per-draw resolution (`bd12_descriptors.c`, `bd12_bind.c`), DXBC -> SPIR-V through vkd3d-shader and DXIL via dxil-spirv (`bd12_translate.c`, `bd12_pso.c`), GENERAL image layouts (no per-resource layout tracking needed), dynamic rendering, buffer-view GC, capture/trace tooling (`bd12_capture.c`).
D3D11 specifics BD11 must add (this is the real work): (1) immediate context with implicit synchronisation: record into a BD12 command list and submit on Flush/Present/blocking Map/readback (fences give completion); (2) fixed binding model (VS/PS/GS/HS/DS/CS stages x 14 CBV, 128 SRV, 16 samplers, 8-64 UAV): one *universal root signature* per stage set and per-draw descriptor copies into a ring heap from the currently bound views; (3) state objects (blend / depth-stencil / rasterizer / input layout / sampler) -> pipeline cache keyed by shader pair + input layout + states + render-target/depth formats, built asynchronously where possible; (4) Map/Unmap semantics (DISCARD/NO_OVERWRITE renaming ring for dynamic buffers, staging for STAGING usage, UpdateSubresource, CopyResource/Region, ResolveSubresource, GenerateMips); (5) **deferred contexts and command lists** (Sekiro uses them: the trace shows `Unmap ... deferred=1`): record a compact command stream and replay it onto the immediate context at ExecuteCommandList (D3D11 semantics: state is reset after replay unless RestoreDeferredContextState); (6) views: SRV/RTV/DSV/UAV over buffers and textures (arrays, cube, mip ranges, typed/raw/structured buffers, append/consume counters), IA stream-out is probably unnecessary; (7) DXGI swap chain on the existing Vulkan presenter (flip model, GetBuffer, ResizeBuffers, fullscreen state contract); (8) queries (occlusion/timestamp/disjoint) and predication; (9) format/feature-level reporting (11_0 first, 11_1 optional), `CheckFeatureSupport`.
Milestones: M1 device, swap chain, buffers/textures/views, clears, one VS+PS draw through the universal root signature (prove translation + binding on the title screen); M2 state objects + pipeline cache + all update/copy paths; M3 deferred contexts / command lists; M4 compute + UAV + indirect draws; M5 queries/predication, mips, resolve, tessellation/geometry only if Sekiro needs them; M6 performance (descriptor ring reuse, draw batching, async PSO). Keep the old path selectable (`BEER_D3D11_LEGACY=1`) until BD11 reaches parity, then delete it and the typed-shader renderer. Test plan: Sekiro title screen -> menu -> in-game, with BEER_BD12_CAPTURE_RT-style pass dumps and the existing trace flags adapted for BD11.
First data to gather before writing code: log every D3D11 method Sekiro calls (count) and every shader stage/feature it creates (compute? geometry/hull/domain? UAVs? stream-out? deferred contexts: yes), so M1-M5 are scoped to the real API surface.

### Sekiro D3D11 API usage measured at the title screen (BEER_D3D11_CALL_STATS, 2026-10-25) — scopes BD11
New tool: `BEER_D3D11_CALL_STATS=<file>` wraps the device/context vtables of the current D3D11 implementation with counting stubs and rewrites `<file>` every 5 s as `table slot count` lines (slot -> name by the standard D3D11 vtable order; device 43 slots, context 115).
Result after ~75 s (boot to title screen): **everything goes through deferred contexts**: `CreateDeferredContext` 19, `FinishCommandList` / `ExecuteCommandList` ~35.7k each; state setting dominates the call counts (OMSetBlendState 33k, PSSetShaderResources 30.6k, RSSetViewports 27.8k, OMSetRenderTargets 27.7k, IASetIndexBuffer 27k, IASetVertexBuffers 24.4k, RSSetScissorRects 22k, OMSetDepthStencilState 21.4k, VSSetConstantBuffers 21.3k, PSSetSamplers 14.8k, RSSetState 13.6k, IASetInputLayout 12.7k); work: `DrawIndexed` 21.3k, `Draw` 3.1k, `Map`/`Unmap` 24.4k (dynamic data written into deferred contexts), `UpdateSubresource` 4.7k, `ClearRenderTargetView` 3.2k, `ClearDepthStencilView` 1.6k. Creation: 13.5k `CreatePixelShader` + 8.7k `CreateVertexShader` (the engine compiles shader variants constantly: a shader/PSO cache is mandatory), `CreateBuffer` 4.7k, `CreateInputLayout` 1.5k, `CreateTexture2D` 1.2k, `CreateShaderResourceView` 2.3k, rasterizer/blend/depth states 224/162/78 (create-and-dedupe), samplers 46. Queries are used heavily: `CreateQuery` 31k, `CreatePredicate` 1k, `Begin` 1.6k, `End` 6.3k, `GetData` 7.8k (occlusion/timestamp/disjoint: need a real query pool implementation, GetData polling must not stall). Also created: hull 73, domain 88, geometry 28 shaders (tessellation/geometry exist; the stage-setters are called 8.9k times, mostly with NULL), compute 100 + UAV 108 (compute is used). Not seen at title: Dispatch, instanced/indirect draws, stream-out, GenerateMips, ResolveSubresource, CopyResource/CopySubresourceRegion, 3D textures (5 only).
Consequences for BD11 design: (1) **deferred contexts are the main path, not an add-on**: recording must be cheap (compact command stream with state deltas), and ExecuteCommandList must replay efficiently on a worker-friendly path; (2) a *shadow state* tracker (per context, with dirty bits) is needed so that consecutive redundant Set* calls (very common here) cost nothing; (3) pipeline/PSO cache keyed by (VS,PS,[HS,DS,GS],input layout hash,blend,depth,raster,RT/DS formats,topology), with persistent cache on disk; (4) Map/Unmap on deferred contexts must write into per-list upload memory (D3D11_MAP_WRITE_DISCARD semantics) that is consumed at replay; (5) queries: occlusion via VkQueryPool (precise or boolean), timestamp + disjoint via timestamp pool, predicates; keep `GetData` non-blocking (return S_FALSE until the pool result is ready).

### BD11 architecture decisions after reading the existing code (2026-10-25)
- **Do not reuse the old `graphics/d3d11/d3d11_impl.c` object model.** It was shaped around the typed renderer: 4 vertex buffers, 8 constant buffers/SRVs/samplers per stage, a handful of command types, software-rasteriser leftovers. BD11 is a new module `graphics/d3d11v/` (bd11.h, bd11_com.c device/objects, bd11_ctx.c context + deferred recording, bd11_state.c shadow state + pipeline cache, bd11_query.c). The old DXGI fake in `loader.c` (`dxgi_CreateSwapChain`, `impl_D3D11CreateDevice`, `init_d3d11_fake`) is the integration point: BD11 supplies device/immediate context/swap chain; the legacy path stays behind `BEER_D3D11_LEGACY=1` until parity.
- **Backend = BD12's C API used like a D3D12 application would** (`bd12_device_create`, committed buffers/images, descriptor heaps, command lists/allocators, queue + fences, `bd12_graphics_pipeline_create`, `bd12_cmd_*`, query helpers, `bd12_cmd_set_render_targets`...). `beer_d3d12_translate_shader()` already turns DXBC (SM4/5 register model, no spaces) into SPIR-V with deterministic set-0 bindings and cached results; that is exactly the D3D11 register model, so no root signature is needed for translation; the D3D11 binding tables map onto one canonical BD12 root signature per pipeline stage set (tables for 14 CBV / 128 SRV / 16 sampler per stage, 8 UAV for PS/CS), with per-draw descriptors copied into a ring `BD12DescriptorHeap` from the currently bound views.
- **Synchronisation/layouts:** D3D11 has no resource barriers. Keep every image in GENERAL layout (valid for attachments with dynamic rendering) and insert a conservative global memory barrier at pass ends, dispatches and copies (`bd12_cmd_global_barrier`); optimise later with per-resource last-writer tracking.
- **Gaps in BD12 that BD11 needs (extend BD12, do not fork):** graphics pipelines only take VS+PS (`BD12GraphicsPipelineDesc`): add hull/domain/geometry stages + `VK_PRIMITIVE_TOPOLOGY_PATCH_LIST` (Sekiro creates 73 HS / 88 DS / 28 GS shaders; `tessellationShader`/`geometryShader` device features must be enabled); stream-out is likely unneeded; blend/depth/raster/stencil/sample-mask/logic-op already exist in the pipeline desc; extra vertex buffer slots up to 16 exist.
- **Presentation:** reuse `bd12_com_set_image_presenter(beer_present_bd12_image)` (`loader.c`) so BD11 presents a BD12 image through the same Vulkan presenter.
- **First M1 target:** device + immediate/deferred context objects with shadow state, buffers/textures/views, clear + one VS+PS indexed draw on the Sekiro title screen via deferred-context replay, with trace/capture hooks equivalent to BD12's (`BEER_BD11_TRACE`, capture reuses `bd12_capture_*`).

### BD11 milestone 1 reached: Sekiro title screen renders through D3D11-on-BD12 (2026-10-25)
Code: `graphics/d3d11v/` = `bd11.h` (types), `bd11_obj.c` (COM plumbing, IID rules, buffers/textures/views/shaders/states/samplers/queries, ID3D11Device vtable), `bd11_dev.c` (device, immediate command-list rotation over 3 allocators, retire timeline/submit, deferred destruction), `bd11_ctx.c` (ID3D11DeviceContext for immediate AND deferred contexts, shadow state, op recording/replay, PSO cache, descriptor resolution, draws/clears/copies/updates/Map), `bd11_swap.c` (IDXGISwapChain over one backbuffer texture + presenter, IDXGIDevice). BD12 changes: `BD12CmdList.resolve_hook` (+`resolve_user`) lets a client supply the set-0 descriptor set instead of a root signature; `bd12_allocate_descriptor_set()` exported; `bd12_com_get_image_presenter()`. Makefile: `graphics/d3d11v/%.o` rule (same ABI flags as BD12 + `-fno-strict-aliasing`) and the four new objects. Loader: `impl_D3D11CreateDevice`, `impl_D3D11CreateDeviceAndSwapChain` and `dxgi_CreateSwapChain` route to BD11 unless `BEER_D3D11_LEGACY=1`; `beer_dxgi_factory()/beer_dxgi_adapter()` exports for GetParent.
Design as built: every context method builds a `B11Op`; immediate contexts execute it (apply shadow state, then GPU work), deferred contexts apply it to their own shadow and record it (objects retained); `ExecuteCommandList` replays through the immediate path (default state before/after unless RestoreContextState). Draws: pipeline from DXBC via `bd12_pso_create_graphics(BD12PsoDesc)` (cache keyed by shader hashes + layout + blend/depth/raster hashes + topology class + RT/DS formats), render targets/viewports/scissors/VB/IB/topology/blend factor/stencil ref only when dirty, descriptor set built per draw in `b11_resolve_hook` straight from the shadow state (CBV/SRV/UAV/sampler per stage register; null bindings use VK_NULL_HANDLE or a zero constant buffer). Resource hazards: one tracked `BD12_STATE_*` per texture, transitions emitted on use (RTV=RENDER_TARGET, DSV write/read, SRV=PS|NON_PS resource, copy src/dst, present=COMMON); a resource bound for writing is unbound from SRV/UAV slots like D3D11 does. Dynamic buffers: host-visible ring of backings renamed on Map(WRITE_DISCARD), per-backing in-flight serial; deferred Map uses scratch memory and Unmap records a ranged `BUFFER_WRITE` op (discard: up to the last non-zero byte; no-overwrite: diff against the current backing). Textures: BD12 images, initial data and UpdateSubresource through upload buffers, staging textures as host-visible buffers.
Verified on the Sekiro title screen: boots, 60 fps (vsync-limited), ~18 draws/frame, 0 skipped, no validation errors; a captured frame (BEER_BD12_CAPTURE_RT=1 + BEER_BD12_TRACE_FILE touch works unchanged on BD11 command lists) shows the SEKIRO / SHADOWS DIE TWICE logo being drawn (partially revealed at the capture instant). New env vars: `BEER_BD11_STATS=1` (every 120 presents: draws/skipped/clears/updates/buffer writes/lists/ops), `BEER_BD11_TRACE`, `BEER_D3D11_LEGACY=1`.
Not implemented yet (logged once when hit): tessellation (HS/DS bound => draw skipped, `[BD11] tessellation shaders bound`), compute `Dispatch`, `CopyStructureCount`, `ClearUnorderedAccessView*`, `GenerateMips`, `ResolveSubresource`, indirect draws, `DrawAuto`/stream-out, queries (return "complete/visible" immediately), predication (ignored), `Map` of GPU-only textures, Get* state getters except `OMGetRenderTargets`, geometry shaders in the pipeline (stage ignored), MSAA paths, depth-stencil SRV reads of stencil, 1D/3D render targets untested. Performance: PSO creation is synchronous on first use (hitch), each Present waits for the GPU (`b11_flush(dev,1)`) because the presenter needs the image idle.
Next: look at the live game and the remaining captures (background layers), then M2 items (queries, GenerateMips, UAV clears, resolve, compute) as Sekiro reaches them; then tessellation/geometry stages in BD12 pipelines.

### Open issues list (as of 2026-10-25)
1. (done) Mouse actions fixed by sending WM_NCHITTEST/answering HTCLIENT; verified by the user, debug code removed.
2. Ambient/indirect lighting too dark (flashlight/dark scenes) — game CPU likely skipping light-volume draws.
3. Occlusion-proxy culling disabled by default (BEER_BD12_PROXY_ALWAYS=1) to avoid pop-in; real culling not restored.
4. Rare startup stall right after swapchain creation (seen ~2 times in dozens of launches) and rare device loss at startup.

### FIXED: stale/obstructing flashlight shadows (2026-10-25)
Symptom: shadows from earlier frames (loading-screen branches, a fence already passed) stayed in the flashlight beam, sometimes as one large square shadow blocking most light.
Cause: `cl_clear_dsv` (bd12_com_list.c) passed D3D12 clear flags (DEPTH=1, STENCIL=2) to `bd12_cmd_clear_dsv` as if they were Vulkan aspect bits (DEPTH=2, STENCIL=4). Depth-only clears (flags=1, used for the 2048x2048 shadow maps) masked to nothing, i.e. shadow maps were never cleared; depth+stencil clears (flags=3) cleared depth only, stencil-only clears cleared depth instead. Fix: convert flags to Vulkan aspects explicitly. User verified: "perfect now".
Found with the new render-target / depth capture tooling: BEER_BD12_CAPTURE_RT=1 (+ CAPTURE_DIR, TRACE_FILE touch; BEER_BD12_CAPTURE_RT_SCALE, BEER_BD12_CAPTURE_RT_DEPTH_ONLY, BEER_BD12_CAPTURE_3D, BEER_BD12_CAPTURE_SRV_DEPTH, BEER_BD12_COPY_LOG) dumps every render pass's colour/depth targets as PPM; [SHADOWPASS] and [CLEARDSV]/[COPYIMG] logs.
Note: stencil is now actually cleared when games ask for it; if any new stencil-related artefact appears, look here first.
Also established (not bugs): final-image darkness is intended exposure (auto-exposure E=1/m clamped 0.04..0.167); scene is lit-by-flashlight only.

## Roadmap: overlay, manager and live tools (planning notes, 2026-10-25)
Goals (from the user): maximum compatibility for their favourite single-player games, easier debugging, and above all an overlay plus accessibility / ease-of-use features: resource usage, a manager app that launches games and controls the loader, Cheat-Engine-style memory access, an upscaler override (DLSS) and live tweaks such as flashlight intensity. The user plays no online games, so memory tools are single-player only.

### Feasibility and expected cost (estimates, not measured)
| Feature | Feasibility | Cost |
|---|---|---|
| Overlay (FPS, graphs, menus) | Easy: we own the Vulkan present and the X11 input, so draw the UI right before present and swallow input while open. No injection. | ~0.2-0.5 ms/frame while visible, ~0 when hidden |
| Resource usage | Easy: GPU timestamps, per-pass capture, draw/dispatch counters and drop counters already exist; VRAM via VK_EXT_memory_budget; GPU load/temp/power via NVML (dlopen); CPU/RAM via /proc | < 1% polled at 1-2 Hz |
| Manager server | Easy: local unix socket (or localhost websocket), JSON messages, own thread, token if exposed to LAN | negligible (event driven) |
| Live settings | Mostly easy, BUT many options are read once through getenv; hot paths must read a cached settings struct instead (also remove repeated getenv in hot paths added during debugging) | 0 once done properly |
| Memory read/write/scan/freeze | Easy: guest memory is in our own address space. "What writes this address" is possible with the page-protect + single-step watch and code-patch hooks already prototyped (BEER_WATCH_*) | scans = transient CPU spike on a worker thread; watches are slow while armed (opt-in, short) |
| Flashlight / exposure / brightness | Two ways: (a) patch the value in game memory (needs a per-game address), (b) generic draw-time constant override keyed by (shader hash, constant buffer register, offset); all CBVs are already resolved at bind time. Final-image brightness/gamma can be applied in the overlay pass. Note: auto-exposure is E = 1/m clamped to 0.04..0.167 (see exposure findings) | very low |
| DLSS override | Hardest. Only for games that already ship an upscaler (FSR2/3, XeSS) because DLSS needs motion vectors + depth; replace the game's upscaler library with our own that feeds DLSS through NVIDIA's NGX Vulkan interface; we hold the VkImages so it is zero-copy. Frame generation is a separate, uncertain question. Licensing: do not ship NVIDIA libraries. libnvidia-ngx.so.1 exists on this machine (driver / Steam runtime overrides); still to verify: whether the DLSS model library (nvngx_dlss) is available natively or only as Windows nvngx_dlss.dll in some game folders | upscaler's own cost (~1-2 ms at high res), our glue negligible |

Rules: every feature off by default and zero cost on hot paths until enabled; invasive tools (memory watches, code hooks) are explicit short opt-in sessions.

### Manager design (two parts)
1. Manager app (separate process): game library + per-game profiles (exe, prefix, audio backend, upscaler/graphics options); builds the launch command (replaces the long command with the hard-coded vkd3d library path and discovers paths itself); validates the setup, launches `loader`, monitors it, shows an exit summary (crash or not, average FPS, log link); turns the BEER_* diagnostic env vars into checkboxes; "collect debug bundle" (zip logs + captures); editor for steam_emu.ini / DLC lists (relevant: ISteamApps now reads it); live tuning sliders (flashlight intensity, brightness, FPS cap); telemetry graphs. Local web-page mode would also allow tweaking from a phone on the LAN (needs an access token).
2. Control channel inside the loader: unix socket per running game (e.g. $XDG_RUNTIME_DIR/beer/<pid>.sock), small JSON messages, a settings registry (name, type, range, default, live-applicable flag) from which the manager generates its UI, so new loader features need no UI work. The overlay is a second client of the same channel.
Technology suggestion: loader stays C; manager in Python+Qt (fast iteration) or Rust+Tauri (polished, light).

### Suggested order
1. Settings registry + control socket in the loader (including the getenv cleanup).
2. Tiny command-line tool (list / launch / set) as a proof.
3. GUI manager on top.
4. Overlay as a second client; then draw-time constant overrides (flashlight/exposure), memory tools, DLSS bridge (weeks, per game).
Open decision: start step 1 after the remaining bugs, or begin the manager now and return to the mouse issue later.

### RE4 Remake bring-up, session 2 (2026-10-25, after a reboot; games drive now at /mnt/56A4064AA4062CD5)
Still ExitProcess(204) very early, before any D3D/DXGI use, after a single VirtualAlloc(0x3d0900 = 4,000,000 bytes, MEM_COMMIT|MEM_TOP_DOWN, RWX) and many HeapAlloc calls of 32 KB / 64 KB / 256 KB (the stub's own dlmalloc-like allocator with a free-chunk cache, 0xCB/0xCC are its fatal codes: 0xCB = chunk allocation failed, 0xCC = assertion in the chunk carving code or a flag check).
Done: VirtualAlloc with a NULL address now returns 64 KiB-aligned bases and reserves in 64 KiB granules like Windows (general fix in platform/win32_virtual_memory.c; did not change the outcome). Ruled out: HeapAlloc zero fill (BEER_HEAP_ZERO=1), 64 KB-aligned HeapAlloc (BEER_HEAP_ALIGN=1), heap guards (BEER_HEAP_GUARDS=1, no overflow crash), no OOM (all HeapAlloc succeed).
New diagnostics: BEER_EXIT_DUMP_SPAN=<bytes before return> (bigger code dump), BEER_EXIT_DUMP_ADDR=hexaddr,hexlen (dump guest memory at exit into <dir>/exit_mem.bin), BEER_EXIT_STACK=1 (list image return addresses on the guest stack at exit), BEER_PATCH_BYTES="addr:hex;addr:hex" (patch guest code before it runs, to step past a check), BEER_VM_LOG now also logs HeapAlloc (first 300).
Findings: exit handler = routine at 0x14e8730e0 (runs atexit handlers then ExitProcess(status stored at 0x14ebcf010)); fatal(code) helper at 0x14e86ccb0; two 0xCC call sites at 0x14e86d7a1 (flag check `chunk[0x10] & 1`) and 0x14e86dca0 (carve check `size - (bin<<5) > r12`, r12 = 1 MB when bin != 0). Patching either one with BEER_PATCH_BYTES (14e86dc9e:eb0c, 14e86d79f:eb0b) still ends in exit 204, so the real fatal call site is elsewhere or the exit is requested by another caller; the guest stack at exit shows image return addresses +0xe8731ef, +0xe86de72, +0xe86cb53, +0xe86f29b, +0xf5312d4 (the entry/CRT bootstrap). Next: find who calls the exit routine (breakpoint-style hook on 0x14e8730e0 or 0x14e86ccb0 recording the caller), then the condition that leads there; also consider that the stub may be probing the CPU / environment (cpuid features, XState, SYSTEM_INFO, GlobalMemoryStatusEx) before its allocator init.
RE4 session 2, continued findings: the protected stub (image 0x140000000..0x14fe68000, entry 0x14fe64a50) is a Free Pascal program: exit code 204 = FPC runtime error 204 "invalid pointer operation" raised by its own heap manager (0xCB = 203 heap overflow). It runs a self-contained init (HeapAlloc of 32K/64K/256K chunks, one VirtualAlloc(4,000,000 RWX), GetSystemMetrics-like display query which triggers "XWAYLAND connected") and makes no GetProcAddress/file calls before dying. The guest stack at exit shows the call chain RunError(0x14e86cc70) <- 0x14e86cca3 <- 0x14e869c55 (code at 0x14e869c12 allocates `GetMem([obj])` through 0x14e86d1e0 then FillChar(0x14e85da20)); the object's size field is likely an uninitialised/garbage value (huge GetMem -> heap manager error), i.e. some value we return (or fail to return) is consumed as a size. Also: the stub's code is unpacked at runtime, so patches/hooks applied at load time are overwritten; install them lazily.
New general diagnostics: BEER_HOOK_LOG="hexaddr:displaced_bytes;..." + BEER_HOOK_LAZY=1 (install on first HeapAlloc) adds a register-saving trampoline that logs rcx/rdx/r8/return address of a guest function entry (displaced bytes must be an instruction boundary >= 14 and rip-relative free). First attempt crashed at the hooked entry (SIGSEGV RIP=0x14e86cc70): the displaced instructions or the page protection need a closer look (the code page may be re-protected/re-unpacked by the stub). BEER_EXIT_STACK_PTRS=1 dumps what the pointer-looking stack words point to at exit.
Next ideas: (1) fix the hook so it can log the GetMem size and its caller, (2) trace which API produced the size (candidates: GetSystemMetrics / EnumDisplaySettings / GetSystemInfo / GlobalMemoryStatusEx / GetWindowRect style results) and compare with real Windows values, (3) alternatively run the stub with a Windows-like process environment (steam_emu.ini / Goldberg settings next to the exe).

### RE4 bring-up, session 2 — real progress (2026-10-25)
Root causes found and fixed (all general loader fixes, RE8 re-verified at 120 fps afterwards):
1. The stub's Free Pascal heap error 204 was caused by OLEAUT32 BSTR functions being stubs that returned 0: SysAllocString/SysAllocStringLen/SysAllocStringByteLen/SysReAllocString/SysReAllocStringLen/SysFreeString/SysStringLen/SysStringByteLen are now implemented (BSTR = 4-byte byte-length prefix + UTF-16 + NUL; header carries the malloc base).
2. GetProcAddress on emulated *system* DLLs (user32, ntdll, advapi32, gdi32, shell32, ole32, oleaut32, comctl32, uxtheme, dwmapi, msimg32, shlwapi, shfolder, version, setupapi, win32u, comdlg32, imm32, winmm, hid, crypt32, psapi) now returns a harmless failing stub for exports we do not implement (ntdll Zw*/Nt* and D3DKMT*: STATUS_NOT_IMPLEMENTED; ole32/oleaut32/dwmapi/SHCreate*: E_NOTIMPL; others 0) instead of NULL; kernel32 and game-owned modules still return NULL. Delphi/FPC runtimes probe whole groups of optional exports and call members without re-checking.
3. Anti-debug probes of the protection stub implemented as on an undebugged Windows machine: CheckRemoteDebuggerPresent (FALSE), GetErrorMode (0), Wow64Get/SetThreadContext (FALSE), NtSetInformationThread/ZwSetInformationThread (STATUS_SUCCESS), FatalAppExitA. VirtualAlloc(NULL) returns 64 KiB-aligned bases (earlier fix).
New diagnostics: BEER_API_TRACE=1 (+BEER_API_TRACE_FROM=N) logs every guest->host API call with name and first four arguments (names also decoded for GetModuleHandle/LoadLibrary/GetProcAddress); BEER_HOOK_LOG fixed (zero the jmp displacement); crash handler prints a hex dump of the caller code; BEER_SLOT_NAMES=<hex slots>; running the loader under `gdb -batch -ex run` WORKS (only attach is blocked) and gives register/stack dumps at a SIGSEGV that the loader's own handler misses.
Current state: the stub now runs its whole startup (heap, strings, SHGetFolderPath for ~20 CSIDLs, LoadLibrary of comctl32/gdi32/shell32/dwmapi/uxtheme..., IsAppThemed, anti-debug probes, timing loops) and then segfaults inside its own Move routine (guest 0x14e85d7d4) copying 0x40 bytes from source pointer 0xFFFFFFFFFFFFFFFF into a freshly allocated WideString: a UNICODE_STRING-like local (u16 Length at rbp-0x18 = 0x40, buffer pointer at rbp-0x10 = -1) filled by a call through 0x14e85c0e0 is bad. The loader's own SIGSEGV handler does not report this crash (process dies with 139, no [CRASH] print). Next: identify which API fills that struct (the call at 0x14e8e6506 is right after a SysFreeString in the trace) and what it should return; check why the loader's crash handler stays silent for it.

### RE4 bring-up, session 3 — protector finished, game code reached (2026-10-25)
RE4 (re4.exe) is protected by **Enigma Protector** (the "Virus warning" window is Enigma's integrity-failure dialog; registry key `HKCU\Software\Enigma Protector\...`). With the fixes below the stub unpacks the game, resolves the game's own imports, loads its DLLs and runs game code up to a Steam check. All fixes are general loader behaviour, not RE4 hacks.
General fixes (found one by one by tracing; each unblocked the next stage):
1. **VirtualAlloc commit inside a reservation** (`platform/win32_virtual_memory.c`): MEM_COMMIT on an address inside a region the loader already reserved used to fail (always tried a fresh mmap). Now it mprotects within the reservation; a pure re-reserve of an existing range still fails like Windows. This one bug made the stub's 64 KiB reserve/commit probing fail everywhere and corrupted its decoder state.
2. **Native NT APIs** (`platform/win32_file_search.inc`): real NtProtectVirtualMemory/ZwProtectVirtualMemory, QueryDosDeviceW (C: and Z: devices), RtlDosPathNameToNtPathName_U, NtCreateFile/NtOpenFile/NtReadFile/NtWriteFile/NtClose/NtQueryInformationFile/NtSetInformationFile (Zw and Nt names; over the existing file handles), **LdrGetProcedureAddress / LdrLoadDll / LdrGetDllHandle**. The protector resolves every import itself through these, bypassing kernel32.
3. **Import binding is not a feature probe**: LdrGetProcedureAddress binds a logged failing stub (`[IMPORT] ... not implemented -> failing stub`) for any missing export of an emulated system module, and for unknown ordinals (ws2_32, oleaut32, xinput ordinal tables included) so one missing function does not abort the whole load. The `[IMPORT]` lines are the work list of functions worth implementing (currently e.g. ProcessIdToSessionId, lstrcmpiA, CreateFile2, Module32FirstW/NextW, K32GetModuleInformation, OpenEventW, GetEnvironmentVariableA, K32GetProcessMemoryInfo, Numa*, GetLargePageMinimum, WinHttp*, getaddrinfo/freeaddrinfo, GetAdaptersInfo).
4. **Fake system modules are real PE images**: kernel32/user32/ntdll/advapi32 handles are now page-aligned mapped addresses (0xFEED0000, 0xFEF10000, 0xFEF50000, 0xFEF90000, 0x40000 each) with a valid MZ/PE header and an export directory listing every implemented function (jmp stubs into the normal thunks; built lazily the first time one of those modules is requested). Protectors that read e_lfanew / walk exports themselves no longer fault.
5. **kernel32 GetProcAddress**: names that exist in the real kernel32/kernelbase export table (generated list `platform/win32_kernel32_export_names.inc`, produced by `tools/gen_export_names.py` from a Wine kernel32/kernelbase) resolve to a failing stub instead of NULL when not implemented; names that do not exist in Windows still return NULL (keeps feature probes honest).
6. **msvcrt.dll** resolves into the bundled UCRT; vsprintf/_vsnprintf/vsnprintf are bridged to `__stdio_common_vsprintf` with small guest thunks.
7. **API-set contracts** (api-ms-*/ext-ms-*) requested at run time map to the UCRT guest image (crt contracts) or kernel32 instead of mapping the tiny fixed-base stub DLLs (which collided at 0x180000000).
8. **Shared runtime directory**: guest DLL lookup now also searches `BEER_DLL_PATH` (colon separated; default `~/.local/share/beer/runtime`). It was seeded with the UCRT / VC++ runtime DLLs (api-ms-*, ucrtbase, msvcp140*, vcruntime140*, concrt140, vccorlib140) copied from RE8's `Win64/` folder, because RE4 does not ship them. The Win64 lookup now requires the file to exist (it used to "succeed" on a non-existent path).
New trace features: `BEER_API_TRACE_COUNT=<n>` (window size, default 6000, with `_FROM`), decoded strings for LdrGetProcedureAddress/LdrLoadDll/ZwCreateFile/NtOpenFile object names and Reg*Key names; TerminateProcess now dumps the guest call chain. Never trace with a filter that matches Alloc/Heap (hundreds of MB).
**Where RE4 stops now**: after the stub finishes it memory-maps an *old genuine Valve steam_api* image (strings: SteamClient014, SteamUser017, SteamUtils007, STEAMAPPTICKET_INTERFACE_VERSION001, "Loaded local steamclient64.dll"), not our emulated Steam module. It looks for `HKCU\Software\Valve\Steam\ActiveProcess`, `HKLM\Software\Valve\Steam` and steamclient64.dll, finds no running Steam, calls ShellExecuteA (steam://run/<id>) and TerminateProcess(53). Options: (a) emulate steamclient64 (CreateInterface -> ISteamClient014 with ISteamUser017/ISteamUtils007/appticket) and the ActiveProcess registry values; (b) detect guest-mapped modules that export SteamAPI_* and redirect those exports to Beer's Steam emulation; (c) find how the RUNE crack normally avoids this (steam_api64.dll on disk is the 981 KB RUNE emulator, steam_api64.rne the 295 KB original; steam_emu.ini has AppId=2050650). Not decided/implemented yet.

### RE4 bring-up, session 4 — faking a running Steam client (2026-10-25)
Decision: on Windows RE4 starts without Steam, but the old Valve client code inside the unpacked exe still checks for a running Steam, so Beer now mimics a Steam install (also useful for later work). Implemented (all general):
- **Virtual registry** (`platform/win32_registry.inc`): read-only in-memory keys/values behind RegOpenKeyA/W, RegOpenKeyExA/W, RegQueryValueExA/W, RegCloseKey (handles 0x7100+, predefined roots incl. sign-extended HKCU, Wow6432Node folded away, REG_SZ and REG_DWORD, A/W conversion). Seeded: `HKCU\Software\Valve\Steam` (SteamPath, SteamExe, Language), `...\ActiveProcess` (pid = our pid, SteamClientDll, SteamClientDll64 -> c:\program files (x86)\steam\steamclient64.dll, Universe), `HKLM\SOFTWARE\Valve\Steam` InstallPath. `BEER_FAKE_STEAM_REGISTRY=0` disables the seeding.
- **steamclient64 surface**: exports CreateInterface (-> existing interface factory, logged as `[STEAMCLIENT]`), Steam_BGetCallback, Steam_FreeLastCallback, Steam_GetAPICallResult, Steam_ReleaseThreadLocalMemory, Steam_NotifyMissingInterface. `impl_SteamClient()` is now a real ISteamClient vtable (CreateSteamPipe, ConnectToGlobalUser, GetISteamUser/Friends/Utils/Apps/UserStats/RemoteStorage/GenericInterface...), each call logged.
- OpenProcess(own pid) returns a handle and GetExitCodeProcess reports STILL_ACTIVE (Steam's IsSteamRunning check); MessageBoxA/W log `[MSGBOX] "title": text` and return a dismissed-box answer; the fake DLL handle cache grew from 32 to 128 entries (it silently ran out, which made LoadLibrary("steamclient64.dll") fail).
Result: the Steam library now reports `Loaded 'c:\program files (x86)\steam\steamclient64.dll' OK` and calls CreateInterface("SteamClient017") then "SteamClient014", but no ISteamClient method is called afterwards and the wrapper shows `Steam Error: Application load error 3:0000065432` and calls TerminateProcess(51). This is the Steam DRM wrapper's error (needs something Steam normally provides, probably the app ownership ticket / IPC result); what exactly it checks after CreateInterface is not known yet. Next: find the check (disassemble from the MessageBoxA caller, img+0xe86e36f region is only a free() helper; use BEER_API_TRACE around the last GetProcAddress calls: it fetches CreateInterface three times and then fails).

### RE4 bring-up, session 5 — Steam client integrity check passed, ticket signature is the wall (2026-10-25)
New general pieces: (1) the genuine `steamclient64.dll` from the user's Steam install (`BEER_STEAM_CLIENT_DLL` overrides; default `~/.local/share/Steam/steamclient64.dll`) is mapped as a plain data image on LoadLibrary and symlinked into the prefix at `C:\Program Files (x86)\Steam\`; the module handle/path/headers are real, all its exports resolve to Beer's own Steam emulation (the DLL is never executed). The Valve client library verifies the loaded module against the file on disk (this was `Application load error 3`) and now passes. (2) GetModuleHandleExA/W FROM_ADDRESS, GetModuleFileNameA/W per module. (3) ISteamClient vtable, ISteamUtils007 (universe 1, AppID from steam_appid/steam_emu.ini, server time, country), ISteamAppTicket (STEAMAPPTICKET_INTERFACE_VERSION001, unsigned local ticket, logged). (4) Fake system module export-table entries are now what GetProcAddress/LdrGetProcedureAddress return (32-byte `mov rax,imm64; jmp rax` slots) so inline hooks that protectors write into API entry points take effect. (5) NtQueryAttributesFile/NtQueryFullAttributesFile, Zw aliases for section calls. (6) trace decodes ZwProtectVirtualMemory regions.
Findings: the Enigma stub hooks exactly 13 ntdll file functions (ZwClose/CreateFile/OpenFile/ReadFile/WriteFile/QueryInformationFile/SetInformationFile/QueryAttributesFile/QueryFullAttributesFile/CreateSection/MapViewOfSection/UnmapViewOfSection/ProtectVirtualMemory): this is its virtual file system. Its table holds steam_api64.dll, steam_api64.rne, steam_appid.txt, steam_emu.ini etc. Our kernel32-level file/LoadLibrary calls do not go through the ntdll entry points, so the virtual files are never served.
Current stop: with the client check passed, the wrapper validates the ownership ticket (ISteamUser::GetSteamID must match the ticket, universe byte, then an RSA signature check against Valve's public key in the image, `Application load error 6:0000065432`). A Valve-signed ticket cannot be produced and patching the key/check would be modifying guest code, which the loader deliberately never does. On Windows this must not be reached (the cracked setup normally serves its own steam DLL); how that happens is not understood. Candidate explanation: the image we run is the embedded genuine steam_api64 instead of the RUNE steam_api64.dll, because the virtual-file layer (hooked ntdll file functions) is bypassed. A faithful fix would layer our kernel32 CreateFile/ReadFile/GetFileAttributes/LoadLibrary over the (possibly hooked) ntdll entry points (guest calls from host code), large work; not started.

### INCIDENT (2026-10-25): guest wrote to the user's real `~/.local/share/Steam/steamclient64.dll` — fixed in the loader, file needs repair
What happened: the loader exposed the user's Steam `steamclient64.dll` to the guest through a symlink in the prefix, and the NT file layer (`nt_open_common`, `platform/win32_file_search.inc`) mapped the access mask `SYNCHRONIZE` (0x100000) to GENERIC_WRITE. A protector's hooked ntdll file functions open files with 0x80100080, so the file was opened write-capable and later modified (same size 26,547,864, new mtime 2026-10-25 20:46, CRC32 1189772473 instead of the 1644729272 recorded in `package/steam_client_steamrt64.installed`). Only that one file changed (checked: no other file modified today in the RE4/RE8 folders or Steam root).
Fixes: (1) access mapping now treats only READ_DATA/READ_EA/EXECUTE/READ_ATTRIBUTES as read and only WRITE_DATA/APPEND_DATA/WRITE_EA/WRITE_ATTRIBUTES as write; SYNCHRONIZE alone is read. (2) the file exposed to the guest is now a read-only *copy* inside the prefix (`drive_c/Program Files (x86)/Steam/steamclient64.dll`, mode 0444), never a symlink to user files. Rule going forward: never hand the guest a path that resolves outside the prefix with write capability; prefer copies for anything user-owned.
Repair (user action): let Steam re-verify/re-extract its client package (quit Steam, delete or rename `~/.local/share/Steam/steamclient64.dll`, start Steam; or delete `package/steam_client_steamrt64.installed`). Verify with CRC32 == 1644729272.
Side effect: while the file is damaged the RE4 Steam-wrapper integrity check fails with error 3 (it passed before the corruption).

## DEBUG TOOLS REFERENCE (consolidated 2026-10-25; all opt-in, off by default)
Standard launch (run in the background, verify with `pgrep -x loader`, stop with `pkill -x loader`; the games drive mount point changes between reboots: check `lsblk`):
`(BEER_VKD3D_SHADER_LIBRARY=<libvkd3d-shader.so path from the umu steamrt4 runtime> setsid nohup ./loader --audio=fmod "<game exe>" > <log> 2>&1 < /dev/null & disown)`
Scratchpad (tmpfs, wiped by reboot) holds logs/captures; never leave a file named dis.py there (it shadows Python's stdlib `dis`); run Python helpers with `python3 -I`.

### How to use the main workflows
- Frame capture of every render pass (colour + depth, PPM, downscaled): `BEER_BD12_CAPTURE_DIR=<dir> BEER_BD12_CAPTURE_RT=1 BEER_BD12_TRACE_FILE=<file>`, then `touch <file>` while the scene is on screen (traces and dumps the next presented frame; touch again to re-arm). Convert PPM to PNG with PIL and build a contact sheet to view. Extra switches below.
- Shader hunting: `BEER_BD12_PSO_LOG=1` ([PSO]/[PSOSHADERS] lines map pipeline hash -> VS/PS DXBC hash), `BEER_DUMP_SHADERS=<dir>` (writes <hash>.dxbc / .spv), disassemble with `spirv-dis` (found under the Android NDK shader-tools directory) and read; compute shaders of a traced frame are listed as `[TRACE] dispatch cs=<hash> groups=...`; capture a shader's buffers with BEER_BD12_CAPTURE_CS.
- Guest code reverse engineering: BEER_EXIT_DUMP* / BEER_HOOK_LOG / BEER_PATCH_BYTES / BEER_API_TRACE / BEER_PEEK_OBJ + BEER_WATCH_*, disassemble dumps with `objdump -D -b binary -mi386:x86-64 -M intel --adjust-vma=<base> file`.
- Crash location when the loader's own handler is silent: `gdb -batch -ex "handle SIGSEGV stop nopass" -ex run -ex "info registers" -ex "x/24gx \$rsp" -ex "x/8i \$rip-16" --args ./loader ...` (running under gdb works; only `gdb -p` attach and perf are blocked).

### Graphics / BD12 (D3D12 on Vulkan)
- BEER_BD12_FPS=1 frame rate line each second; BEER_BD12_PROF / BEER_BD12_SAMPLE / BEER_PROF_SAMPLE profiling; BEER_BD12_TS_LOG GPU timestamp log; BEER_BD12_RES_LOG, BEER_BD12_HEAP_LOG, BEER_BD12_VIEW_LOG (buffer view cache), BEER_BD12_CLEAR_LOG (UAV clears), BEER_BD12_BIND_LOG (bound buffer VAs per draw/dispatch in traced frames), BEER_BD12_INDIRECT_LOG, BEER_BD12_FLIGHT_RECORDER.
- Trace and capture: BEER_BD12_TRACE=1, BEER_BD12_TRACE_FILE=<file> (touch to trace next present), BEER_BD12_CAPTURE_DIR (output dir for all captures), BEER_BD12_CAPTURE_CS=<hash prefixes> (dump a compute shader's CBV/SRV before and UAV buffers/images after each dispatch), BEER_BD12_CAPTURE_DRAWS=1 (indirect draw counts/args), BEER_BD12_CAPTURE_TWICE, BEER_BD12_CAPTURE_3D=1 (every dispatch that writes a 3D image), BEER_BD12_CAPTURE_RT=1 (every render pass's colour and depth targets as PPM; BEER_BD12_CAPTURE_RT_SCALE=<n> downscale, default 4; BEER_BD12_CAPTURE_RT_DEPTH_ONLY=1 skip colour; BEER_BD12_CAPTURE_DEPTH_MAXPIX=<n> largest depth map to save), BEER_BD12_CAPTURE_SRV_DEPTH=1 (depth textures sampled by traced compute dispatches), BEER_BD12_COPY_LOG=1 ([COPYIMG] image copies and [CLEARDSV] depth clears in traced frames), [SHADOWPASS] lines list depth passes (clear flag, draw count). BEER_BD12_RB_LOG / BEER_BD12_RB_DUMP=all (GPU->CPU readbacks and their contents), BEER_BD12_DUMP_SPIRV, BEER_D3D11_CAPTURE_DIR, BEER_FRAME_DUMP_DIR / _INTERVAL / _LIMIT, BEER_FRAME_CAPTURE_PATH / _MIN_PRESENT.
- Behaviour switches used as experiments: BEER_BD12_PROXY_ALWAYS=0 (restore real occlusion-proxy depth test), BEER_BD12_PROXY_BIAS=<n>, BEER_BD12_FORCE_INST=1|2, BEER_BD12_INDIRECT_FORCE, BEER_BD12_SYNC_ALL, BEER_BD12_SYNC_SUBMIT, BEER_BD12_NO_IMAGE_ZERO, BEER_BD12_ASYNC_PSO, BEER_SPV_FLOAT_CONTROLS=0, BEER_SPV_SYNC, BEER_NO_RT=1 (disable Vulkan ray tracing), BEER_DXR_PROBE=1 + BEER_DXR_PROBE_SM=<hex>, BEER_DXIL_SPIRV_LIBRARY, BEER_DXIL_VERBOSE.
- Presenter / Vulkan: BEER_PRESENT_MODE, BEER_PRESENT_TIMING, BEER_PRESENT_SKIP_ALL / _SKIP_FLIP, BEER_PRESENTER_CPU_X11 / _CPU_STAGING, BEER_VULKAN_PERF(+_MIN_US), BEER_VULKAN_GPU_KEEPALIVE, BEER_VULKAN_DEVICE_LOSS_DIR, BEER_VK_CHECKPOINTS, BEER_VULKAN_DRAW_PROVENANCE, BEER_VULKAN_GUI_DRAW_TRACE, BEER_VULKAN_CACHE_TRACE, BEER_VULKAN_COMPOSITOR_SPV / _INDEXED_SPV, BEER_RENDER_DIAGNOSTICS, BEER_D3D11_DIAGNOSTICS, BEER_D3D12_DIAGNOSTICS, BEER_D3D12_STARTUP_TIMING, BEER_DXGI_VRAM_MB / _SHARED_MB, BEER_PHYS_MB.

### Input / window / messages
BEER_INPUT_DIAGNOSTICS=1 (X11 buttons/keys, raw input, DirectInput state, SetCapture, GetSystemMetrics first queries, DI stub calls), BEER_MESSAGE_DIAGNOSTICS=1 (message queue/take), BEER_RE8_ACTION_TRACE=1 (signature-guarded RE8 raw-edge/action resolver/predicate tracing), BEER_RE8_CURSOR_GATE_BYPASS=1 (signature-guarded RE8 diagnostic workaround; verified to restore aim/fire by accepting the cursor-in-active-viewport predicate), BEER_DI_NO_ENUM=1 (old DirectInput EnumObjects behaviour), BEER_NO_XI2=1, BEER_HOST_CURSOR, BEER_UI_CONTROL_TRACE, BEER_UI_STATE_TRACE, BEER_MAIN_THREAD_DIAGNOSTICS, BEER_EVENT_DIAGNOSTICS, BEER_SYNC_DIAGNOSTICS, BEER_WAIT_DIAGNOSTICS, BEER_SLEEP_DIAGNOSTICS, BEER_THREAD_DIAGNOSTICS.

### Guest-code introspection (diagnostic code patching; uses trampolines, never on by default)
- BEER_API_TRACE=1 (+BEER_API_TRACE_FROM=<n> skip the first n calls, 6000 calls logged): every guest->host API call with name and first four args, strings decoded for GetModuleHandle/LoadLibrary/GetProcAddress/GetFileVersion.
- BEER_HOOK_LOG="hexaddr:displaced_bytes;..." (+BEER_HOOK_LAZY=1 to install on the first HeapAlloc, needed when a packed stub unpacks its code at run time): logs rcx/rdx/r8 and the return address at a guest function entry; displaced bytes must be an instruction boundary >= 14 with no rip-relative instructions.
- BEER_PATCH_BYTES="hexaddr:hexbytes;..." patch guest code before it runs (also overwritten by self-unpacking stubs); BEER_SLOT_NAMES=<hex slots> names the import behind an IAT slot.
- BEER_EXIT_DUMP=<dir> on ExitProcess: dumps code around the callers (exit_code_N.bin); BEER_EXIT_DUMP_SPAN=<bytes before return> larger window; BEER_EXIT_DUMP_ADDR=hexaddr,hexlen dumps guest memory to exit_mem.bin; BEER_EXIT_STACK=1 lists image return addresses on the guest stack; BEER_EXIT_STACK_PTRS=1 dereferences pointer-looking stack words.
- BEER_VM_LOG=1 logs VirtualAlloc and HeapAlloc (first 300); BEER_HEAP_GUARDS=1 guard-page heap; BEER_HEAP_ZERO=1 / BEER_HEAP_ALIGN=1 experiments; BEER_ENABLE_KNOWN_TARGET_PATCHES / BEER_ENABLE_HARD_PATCHES / BEER_ENABLE_TRANSACTION_PATCHES opt in to old game-specific patches.

### Other subsystems
BEER_STEAM_DIAGNOSTICS=1 (ISteamApps calls), BEER_FILE_DIAGNOSTICS, BEER_SAVE_DIAGNOSTICS, BEER_NT_SECTION_DIAGNOSTICS, BEER_EXCEPTION_DIAGNOSTICS, BEER_COM_DIAGNOSTICS, BEER_DEBUG_CRASH, BEER_CRASH_RECOVERY_LIMIT, BEER_EXECUTABLE_TLS(+_TIMEOUT_MS), BEER_WASAPI_DIAGNOSTICS / _PACKET_DIAGNOSTICS, BEER_MF_DIAGNOSTICS / _MF_AUDIO_DUMP / _MF_PROBE_STREAM, BEER_SEKIRO_AUDIO_UNMUTE, BEER_PREFIX, BEER_ALIGN_OLD.
- BEER_BD11_STATS=1 (BD11 per-120-frame counters), BEER_BD11_TRACE, BEER_D3D11_LEGACY=1 (old D3D11 path)
- BEER_D3D11_CALL_STATS=<file> (counts D3D11 device/context vtable calls, rewritten every 5 s; see Sekiro section)
- BEER_API_TRACE_COUNT=<n> (trace window size), BEER_DLL_PATH=<dir[:dir]> (extra guest DLL search path, default ~/.local/share/beer/runtime), BEER_API_TRACE_FILTER / BEER_API_TRACE_NEARBY (see session 2). `tools/gen_export_names.py <C array name> <dll...>` regenerates export-name lists from PE files.
(The list above matches every BEER_* variable currently read by the sources; ones marked with a short note are the ones documented in detail elsewhere in this file.)

### BD11 status (2026-10-25, end of session)
BD11 is now OPT-IN (`BEER_D3D11_BD11=1`); the legacy D3D11 path is the default again. Findings: the 16-bit index format (DXGI 57 = R16_UINT, 32-bit is 42) was misread and is fixed; with it the Sekiro title composited mostly right but with ghosted/overlapping layers and vertical streaks; later builds rendered black and stuttered the whole PC (cause unknown, readbacks of RT0 were all zero). Splash screens (frames ~107-323) show only black fade polygons; they are probably Bink movies (needs compute Dispatch, not implemented in BD11). New debug env vars: BEER_BD11_FRAMELOG (per-frame draw counts), BEER_BD11_HOSTBUF (host-visible buffers so traces can read them), BEER_BD11_SYNC_DRAWS (flush+wait after each draw; hangs/slow), BEER_BD11_SPLIT_DRAWS, BEER_BD11_DUMP_SHADERS=<dir>, BEER_BD11_DUMP_PRESENT/DUMP_DIR and BEER_BD11_DUMP_DRAWS (readback dumps; unreliable, all-zero output), BEER_BD12_CAPTURE_DEBUG. BD12 RT capture produced no files on BD11 in this session (end_rendering never ran while the trace window was active).

### PRAGMATA first look (2026-10-25)
`/mnt/FE5007855007443D/Games/PRAGMATA/PRAGMATA.exe` (RE Engine, D3D12, 383 MB exe). The exe carries a VM-style protector (obfuscated junk-instruction code in `.reloc`-range sections, extra sections `.xtls .xcode .00cfg .arch .xtext .xpdata .udata .ecode`; looks Denuvo-like, not verified). Without `BEER_EXECUTABLE_TLS=1` the entry point crashes at once on a NULL VM-state pointer (RVAs 0xdfa42d9/0xdfa42e2/0xe897063, dead recovery cycle abort). With it, TLS callbacks 0 and 1 finish, callback[2] (RVA 0x15903560) spins at 100% CPU for >90 s (polls KUSER_SHARED_DATA around 0x7ffe02e8; looks like a timing/environment loop), then callback[3] and entry crash the same way. Not investigated further; a real fix means understanding the protector's wait condition. Logs: scratchpad prag1-3.log.

### Ghost of Tsushima Director's Cut first bring-up (2026-10-25)
`/mnt/56A4064AA4062CD5/Games/Ghost of Tsushima DC/GhostOfTsushima.exe` (clean PE sections, no heavy protector; D3D12, Streamline `sl.interposer.dll`, DirectStorage, EOS/Steam). General loader fixes made while getting it going (all in the generic API surface):
- `platform/win32_bcrypt.c`: real CNG (BCryptOpenAlgorithmProvider/GetProperty/CreateHash/HashData/FinishHash/DuplicateHash/DestroyHash/Hash/GenRandom, MD5/SHA1/SHA256/384/512 + HMAC) over libcrypto (`-lcrypto` added to LDFLAGS). Missing `BCryptGenRandom` made the game ExitProcess(1) at once.
- `platform/win32_version.c`: version.dll (GetFileVersionInfoSize/GetFileVersionInfo/VerQueryValue A+W) reading the RT_VERSION resource of PE files. NOTE: `platform/win32_missing_exports_table.inc` is searched BEFORE the main `g_impls` table (first match wins), so a generated stub there shadows a real implementation; I removed the stubs for the names implemented here (GetFileVersionInfo*, VerQueryValue*, inet_ntop, gethostname, GetVersionExA).
- Winsock `inet_ntop`, `gethostname` (ordinal 57), `SHCreateDirectoryExW` (real mkdir -p), `SetFilePointerEx` (was a stub returning FALSE), `GetVersionExW/A` (Windows 10 build 19045), `EnumDisplayDevicesW/A` (adapter + monitor from the host GPU info).
Where it stops now: reaches `D3D12CreateDevice`/BD12 (device ready) then the game's own checks fail: "No installed graphics card detected" (probably nvapi missing: `nvapi_QueryInterface` -> NULL, plus sl.interposer `D3D12SerializeRootSignature` stub returning NULL), "Shader Model 6.6 support not detected" (BD12 reports SM 0 unless `BEER_DXR_PROBE=1 BEER_DXR_PROBE_SM=66`), and with SM 6.6 "GPU is not supported" because `CheckFeatureSupport(D3D12_OPTIONS)` is all zeros (ResourceBindingTier etc.). Real support needs the DXIL (SM6.x) path for all stages plus a bindless descriptor model in BD12 (same prerequisite as RE8 ray tracing, see the DXR section above). Logs: scratchpad got*.log.

### Manager app + live control channel (2026-10-25)
- `manager/beer_manager.py` (Python, GTK4 + libadwaita, `manager/install.sh` adds the `beer-manager` command and an app-grid entry). Library in `~/.config/beer/library.json` (nothing preloaded, add via +), loader/vkd3d paths in `~/.config/beer/manager.json` (auto-detected), logs in `~/.local/state/beer/logs/` (last 30 kept). Launch builds the command (`--audio=...`, extra args/env, `BEER_VKD3D_SHADER_LIBRARY`, `BEER_TIME_SCALE`). Running page auto-discovers every loader socket (also games started from a terminal): fps, CPU, RSS, threads, uptime, game-speed slider + quick stops, pause, log viewer/copy, stop (SIGTERM to that pid only).
- Loader side: `platform/beer_control.c` = unix socket `$XDG_RUNTIME_DIR/beer/<pid>.sock` (dir 0700, socket 0600, `/tmp/beer-<uid>/` fallback), one JSON object per line: `hello`, `status`, `settings`, `set {name,value}`; settings registry (name/label/type/min/max/default/live) so the UI can be generated; floats are fixed point (milli) because the loader is built without SSE. Settings today: `time_scale` (0.05..10) and `paused`.
- Time multiplier: `platform/beer_time.c` (virtual monotonic/realtime clocks, continuous across changes, raw clock fast path until first used). Applied to QueryPerformanceCounter, GetTickCount/64, timeGetTime, GetSystemTimeAsFileTime/Precise and the KUSER_SHARED_DATA clock updater. NOT scaled: GetLocalTime/GetSystemTime, rdtsc, Sleep durations, audio pitch. `BEER_TIME_SCALE=<x>` sets the initial value.
- FPS in the manager counts frames through `beer_present_bd12_image` (BD12 games); the legacy D3D11 renderer path (Sekiro) does not report fps yet.
- Verified with RE8: socket present, status/settings/set all answer, 120 fps unaffected. Not yet verified in-game: that RE8 visibly slows down at 0.5x.

### Prefixes in ~/.beer, hardware spoofing (2026-10-25)
- New prefixes live in `~/.beer/prefixes/<name>`; `~/.beer/trash` receives removed ones (the manager never deletes a prefix). The loader's default prefix is `~/.beer/prefixes/default`, falling back to the old `~/.local/share/beer/prefixes/default` when only that exists, so current saves keep working until migrated (the manager's "Move to ~/.beer" does an atomic move). Manager: Prefixes page (create with the same skeleton the loader makes, clone = copytree, open folder, move legacy, trash) and a per-game prefix selector; launches pass `--prefix=<path>` explicitly.
- Spoofing (env vars, API-level only; CPUID and the real renderer are untouched): `BEER_SPOOF_GPU_NAME/_VENDOR/_DEVICE` (hex) `/_VRAM_MB` applied inside `beer_vulkan_host_gpu_info` (so DXGI adapter desc and EnumDisplayDevices agree, verified: GoT's "Current GPU" message shows the spoofed name); `BEER_SPOOF_CPU_NAME/_VENDOR/_THREADS/_MHZ` feed `HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0` (real values from /proc/cpuinfo by default now) and the processor count in GetSystemInfo/GetLogicalProcessorInformation. Registry seed no longer disappears with `BEER_FAKE_STEAM_REGISTRY=0` (only the Steam keys do). The CPU registry values are not yet verified against a game that reads them. Manager presets: 8 GPUs, 6 CPUs, plus custom entries.

### Frame cap + memory accessor (2026-10-26)
- Frame cap: control setting `fps_limit` (int, 0 = off, max 1000, `BEER_FPS_LIMIT=<n>` for the initial value), enforced by `beer_control_frame_limit_wait()` called per presented frame in `beer_present_bd12_image` (coarse nanosleep then a short spin; real monotonic clock, not the scaled game clock). Verified on RE8: cap 60 -> 59.9 fps. Covers BD12 (D3D12) games only; the legacy D3D11 renderer path is not capped. Manager: Off/30/60/90/120/144 buttons + spin on the running card, plus a starting cap per game.
- Memory accessor (`platform/beer_memory.c`, built with SSE through its own Makefile rule because of strtod/strtof): control commands `mem_regions, mem_read, mem_write, mem_scan, mem_next, mem_results, mem_reset, mem_values, mem_set, mem_freeze, mem_unfreeze, mem_frozen`. All access goes through `/proc/self/mem` (pread/pwrite), so unmapped addresses return an error instead of faulting the game, and writes also reach read-only pages. Scan session lives in the loader (<= 4M candidates, previous values kept for changed/unchanged/increased/decreased). First scan skips /dev, nvidia and memfd mappings; i32 writable scan of RE8 took ~5 s. Freeze = a thread rewriting the value every 8 ms (max 64). Types: i8 i16 i32 i64 f32 f64 str bytes. Exact float scans tolerate typed-text rounding.
- Manager: "Memory…" button on a running game opens `manager/memory_window.py` (Scan with First/Next scan, results with live values + Set + add to watch; Watch list with Set/Freeze; Hex view with write). Scans run off the UI thread on their own socket (120 s timeout). Watch addresses are not persisted (they change per run). Not done: pointer scans, unknown-initial-value scans, code disassembly.
- Known open: RE8 does not resume cleanly after "Pause game time" (user report, not investigated).

### Unknown-value scans and the in-game overlay (2026-10-26)
- Unknown initial value scan (`mem_scan` with `"unknown":true`, numeric types): snapshot of all scannable memory (cap 2.5 GB, `truncated` flag when regions were skipped) + one "still a candidate" bit per element; `mem_next` modes exact/changed/unchanged/increased/decreased filter it in place (updating the stored values) and it collapses into the normal candidate list as soon as <= 4M candidates remain. RE8 test: 631M candidates -> 292k after one "changed" pass (about 2 s per pass). Manager: "Unknown initial value" switch in the Scan tab.
- Overlay (`platform/beer_overlay.c`, `platform/overlay_font.h` generated by `tools/gen_overlay_font.py` from DejaVu Sans Mono at 14/20/28 px): two CPU-rendered boxes (Resources: FPS, frame time, CPU, RAM, threads, game speed, uptime; Pinned memory: live values of pinned addresses). `record_overlay()` in `renderer/vulkan/vulkan_presenter.c` copies them from a persistent host-visible buffer onto the swapchain image after the frame blit (both the direct-image/D3D12 path and the RGBA path). Boxes are OPAQUE (the presenter only has transfer operations, no blending pipeline; real transparency would need a compute/graphics pass on the swapchain image). Hotkey: handled in `collect_native_window_events` via `beer_overlay_hotkey_event` (keysym of the unshifted key + Shift/Ctrl/Alt mask, default F10), the event never reaches the game; held-key repeats do not re-toggle.
- Control commands: `overlay_get`, `overlay_set` (flat keys: visible, hotkey_keysym, hotkey_mods (1 shift, 4 ctrl, 8 alt), panel_color/text_color/accent_color (hex strings), fields ("fps,frametime,cpu,ram,threads,speed,uptime"), res_/pin_ enabled/x/y/size; x,y are fractions of the screen for the box's top-left corner), `overlay_toggle`, `overlay_pin {addr,type,label}`, `overlay_unpin`, `overlay_pins`. Pins live only in the loader process (max 16), so they must be set up again after a restart, as requested. `BEER_OVERLAY_JSON` carries the initial flat config; `BEER_OVERLAY_DUMP=<dir>` writes the rendered boxes as PPM (debug).
- Manager: Overlay page (`manager/overlay_page.py`): shortcut capture, drag-and-drop layout canvas (1920x1080 reference preview using the real font cell sizes), per-box enable/size, resource fields, colours; config in `~/.config/beer/overlay.json`, pushed live to every running game and passed at launch. Memory window: pin buttons on scan results and on watch-list rows (with an overlay label). Verified in code and via dumps (boxes render correctly: see BEER_OVERLAY_DUMP); on-screen presence in the game window still to be confirmed by the user.

### Scan thresholds and CPU percentage (2026-10-26)
- `mem_next` accepts an optional `threshold` (decimal) for the changed/increased/decreased modes: the new value must differ from the previous one by at most that amount (`|delta| <= threshold`; increased and decreased also keep their direction). Without it behaviour is unchanged. Works for list scans and unknown-value snapshots (`step_matches()` in beer_memory.c). Verified on a controlled word (DOS stub of RE8's exe, restored afterwards): 100->105 increased passes at threshold 5, fails at 4.9; decreased 100->95 fails at 4, passes at 5; changed 100->90 fails at 9, passes at 10; unchanged values never match changed. Manager: optional "Threshold" field in the Scan tab.
- CPU readings were process CPU time divided by wall time, i.e. percent of ONE core (a busy multi-threaded game reads 100+%). The status reply, the manager card and the overlay now show the share of the whole machine plus the cores' worth in brackets (`CPU 8.7% (1.7 cores)`); status gained `cpu_cores`.
- Scan change limits (2026-10-26): `mem_next` now takes `at_least` (smallest step) and `up_to` (largest step; `threshold` is kept as an alias), either or both, for changed/increased/decreased, list and snapshot scans. Verified on a controlled word: all 11 cases match the expected result (e.g. increased 100->105 passes at_least 5, fails at_least 6; at_least 3 + up_to 7 passes; at_least 6 + up_to 9 fails). Manager: two optional fields, "Change at least" and "Change up to".

### Memory: apply to all + manual removal (2026-10-26)
- `mem_set_all {value}` writes a value (in the scan's type) to every result of the list (refuses while the scan is still an unknown-value snapshot: narrow it down first); `mem_remove {addrs:[..]}` drops results from the list (<= 64 per call), so later Next scans ignore them. Manager: "Apply to all results" row with a confirmation showing the count, and a remove button on each result row (the manual removal updates the count). Verified: remove drops exactly the chosen address and the other results are kept; set_all errors without an active list scan.
- Test slip to remember: while testing I called `mem_set_all` on a session that still held 4 matches (3 planted words + 1 unknown address elsewhere in the RE8 test process) to probe the error path, so it really wrote there. Only a throwaway test process was affected (title/loading, no saves), and it was killed. Lesson: probe error paths with `mem_reset` first, and a controlled value must be verified to match only the planted addresses before any write-all.

### Snapshot coverage, value condition, window lifetime (2026-10-26)
- The unknown-value snapshot used to stop at 2.5 GB, which on RE8 was only part of the 6.7 GB of writable mappings (user suspicion confirmed: values were being missed, `truncated` was true). Snapshot copies now live in an unlinked temp file under `~/.cache/beer` (`BEER_SCAN_TMPDIR` overrides; needs free disk space + 2 GB, falls back to malloc) and the cap is 12 GB, so RE8's writable memory is covered completely (6,783 MB scanned, nothing truncated, ~5 s). Replies carry a `coverage` object (scanned/readonly/device/huge/limit MB) and the manager prints what was scanned and what was skipped and why: read-only memory (when "Writable memory only" is on), device/GPU mappings (/dev, nvidia, memfd; about 0.6-0.8 GB on RE8), regions over 24 GB, snapshot limit. Still not covered by design: unaligned values, memory allocated after the snapshot, str/bytes types in unknown scans.
- Value condition: `cond` (lt/le/gt/ge/eq) + `cond_value` on `mem_scan` and `mem_next` keeps only elements whose (new) value satisfies it, numeric types, also during the unknown snapshot (bits are only set for matching elements, so `>= 0` removes negatives immediately). RE8: 1.76 G candidates -> 1.84 G for `>= 0`, 169 M for `> 0`; chained "changed and < 1000" -> 1.8 M. Manager: "Only keep values that are…" row with the comparison and value.
- The Memory window now closes by itself when its game exits (pid check + socket existence every 0.7 s).

### Final-image pass, screenshots, manager storage in ~/.beer (2026-10-26)
- `renderer/vulkan/vulkan_post.c` (+ shaders `post_effects.comp`, `post_overlay.comp`, compiled by `tools/gen_spv_header.py` with glslc into `post_*_spv.h`): compute passes between the frame copy into the presenter's upload image (now created with STORAGE usage) and the swapchain blit. Pass 1 (effects): sharpen, white balance, brightness/contrast/gamma, saturation/vibrance, colour-vision correction (daltonize) or simulation (protan/deutan/tritan), vignette, film grain, letterbox, parameters as milli ints in push constants. Pass 2: blends the overlay boxes with real per-pixel alpha (opacity setting), scaled into source pixels if the window differs from the game. Screenshot: the finished image (before or after the overlay for "clean") is copied to a host buffer, collected after the presenter's next fence wait and PNG-encoded on a thread (zlib, `-lz`), saved to `~/Pictures/Beer/<game>-<time>.png` at the game's resolution. Nothing runs (and nothing is touched) when all effects are neutral, the overlay is hidden and no screenshot is pending; the old opaque overlay copy remains the fallback (also for direct-blit BGRA frames, which skip the post pass). `BEER_NO_POST=1` disables it.
- Control commands: `post_get`, `post_set` (brightness, contrast, gamma, saturation, vibrance, sharpen, vignette, grain, temperature, colorblind 0-6, cb_strength, letterbox, shot_keysym, shot_mods), `screenshot {clean}`; `BEER_POST_JSON` for the initial values; screenshot hotkey (default F12, never reaches the game). Overlay gained `opacity` (default 0.85) with per-pixel alpha in `beer_overlay.c`. Verified on RE8 via the screenshots themselves: grayscale+vignette+contrast applied, translucent overlay visible in the presented frame, clean shot without overlay, plain shot unchanged; this also confirmed the overlay really is on screen.
- Manager: new Display page (`manager/display_page.py`: sliders, colour-vision mode, screenshot shortcut + buttons, folder), overlay opacity slider, Memory window "Pinned" tab (rename/remove pins, live values), per-game "Loader options" (presenter, present mode, and a switch for every supported option: --renderer=vulkan, BEER_NO_RT, BEER_NO_POST, BEER_D3D11_BD11, BEER_FAKE_STEAM_REGISTRY=0, BEER_EXECUTABLE_TLS, BEER_ENABLE_KNOWN_TARGET_PATCHES, BEER_ENABLE_TRANSACTION_PATCHES, BEER_PRESENTER_CPU_STAGING, BEER_BD12_FPS, BEER_ALLOW_180_BASE, BEER_DXR_PROBE). All manager files now live in `~/.beer` (library.json, manager.json, overlay.json, display.json, logs/); old `~/.config/beer` files are copied once, never deleted. Live frame cap and game speed changes on the running card are saved into the game's library entry so they apply on the next launch (pause is not saved).

### Overlay metrics and the shader-constant finder (2026-10-26)
- Overlay fields added: GPU usage, VRAM (used/total plus "this game" on NVIDIA), clock (time of day), frame-time graph (bars of the last frames, green <= 16.7 ms / yellow <= 33.3 ms / red above, dim 60 fps guide line, scale >= 33 ms), via `platform/beer_gpu_stats.c` (NVML loaded at run time: util, memory, per-process memory with `nvmlDeviceGetGraphicsRunningProcesses_v3`; AMD fallback through sysfs, polled once a second by a thread). The overlay field list is now parsed as exact comma-separated names (substring matching confused "ram" and "vram"). Verified on RE8 through the box dump: GPU 50%, VRAM 4776/16376 MB, this game 3453 MB, clock, graph.
- Shader-constant engine `graphics/d3d12v/bd12_cbtweak.c`, hooked in `resolve_set_impl` (bd12_bind.c) for every CBV that has a CPU-visible `source_buffer` (root CBV or CBV table descriptor; root constants are not covered) and advanced per present from `bd12_trace_frame_advance`. Capture (`gfx_capture on`) keeps the first 4 KB of the first draw per (PSO shader hash, stage, slot, bound size) each frame (RE8: 573 distinct buffers, none GPU-only). The bound size is mostly 65536 because root CBVs only know the rest of the buffer, capped at the uniform limit. Commands: `gfx_capture, gfx_cbs, gfx_cb_read, gfx_scan, gfx_next {mode exact|changed|unchanged|increased|decreased, value, at_least, up_to}, gfx_results, gfx_scan_reset, gfx_tweak_set {id+offset+scope | explicit pso/stage/reg/space/size/offset, mode set|scale|add, value, label}, gfx_tweak_update/remove/enable, gfx_tweaks`. Tweaks overwrite a float in the CBV's memory when the draw is recorded (before the GPU reads it); scale/add keep the game's original value per address so they never compound on static buffers; the captured sample is taken before tweaks. `hits` = draws touched last frame. Tweaks need no capture, so saved ones work with the window closed. Limits: constants the game writes after the draw is recorded are not overridden, root constants and GPU-only buffers are invisible, per-object buffers look "changed" every frame (narrow with "stayed the same" rounds), only the first 1024 floats of a buffer are scanned. Verified on RE8: scan of 559k floats, filtering, 64 tweaks applied (256 hits) and removed, restore of saved tweaks.
- Manager: `Graphics…` button on the running card opens `manager/graphics_window.py` (separate from Memory): Start here (plain-language explanation, recipes for flashlight/brightness/sun/fog/general, Start watching -> snapshot -> "It changed / went up / went down / stayed the same" with coaching hints and optional size limits), Candidates (÷2, ×2, Zero tests with Undo, Save… with a name and scope), My tweaks (per-game list with enable switch, Multiply/Add/Set, slider/number, live hits, delete), Browse (every constant buffer and its live floats). Saved tweaks live in `~/.beer/tweaks/<exe>.json` and are re-applied on every launch (PSO hashes are stable), test tweaks are undone when the window closes, and watching stops when it closes. A smoke test drove the wizard against RE8 without errors.

### Pause no longer hangs RE8 (2026-10-26)
- Symptom: "Pause game time" slowed RE8 to ~6 fps (clock frozen, then 1/1000 speed) and after resuming the game hung for good (all 68 threads idle in waits/sleeps, no CPU). Reproduced on a private test copy: with a fully scaled clock 5 of 6 runs hung, nondeterministically (one variant that looked fixed was luck). Leaving individual clocks unscaled showed only QPC matters for pausing; the cause is code seeing a frozen or near-frozen QPC (zero frame deltas, spin loops waiting for the counter to move, NaN/inf poisoning of adaptive systems).
- Fix (`platform/beer_time.c`): pause = clocks frozen AND any thread that calls the performance counter or tick count is parked on a condition variable until resume, so no game code ever observes a frozen clock; time stays continuous across resume. KUSER_SHARED_DATA is not gated (its updater just stops advancing). The wall clock (GetSystemTimeAsFileTime) is left on the real clock. Verified: 5/5 pause+resume runs OK, frames while paused = 0 (a real freeze), 700+ frames in 3 s after resume. Speeds other than pause were never affected.
- Diagnostics added: `kill -USR1 <pid>` makes every thread print host call chain (module offsets; resolve with `addr2line -f -e loader <offset>`), the guest RVA of its last API call, and what it is blocked on (Sleep ms / WaitForSingleObject handle+timeout, thread-local `g_dbg_wait_*`). `BEER_TIME_RAW=qpc,tick,kuser,filetime` leaves individual clocks unscaled. Lesson: API tracing (`BEER_API_TRACE`) itself crashed the game with a stack-smashing error in this scenario; use the USR1 dump instead.
- Process lesson: test scripts must pick the control socket by the PID of the game they launched (never the first socket in the directory), because the user may have their own game running; verified from the user's log that none of my tests touched their session.

## DXR (ray tracing) in BD12 — session notes

Status: state objects, shader identifiers, SetPipelineState1, DispatchRays implemented; **DispatchRays itself is untested** (RE8 only dispatches rays in gameplay, not at title/menu).

- Enable: `BEER_DXR=1` (or `BEER_DXR_PROBE=1`) advertises OPTIONS5 tier 1.1; SM 6.6 via `BEER_DXR_PROBE_SM=66`. RE8 needs `RayTracing=true` in its config.ini — only change it through `scratchpad/rt_run.sh` (backs up and restores).
- `graphics/d3d12v/bd12_rtpipe.c`: blob interning (RE8 makes ~2374 state objects from 66 libraries), lazy build on first GetShaderIdentifier/dispatch, real Vulkan group handles (32 B) as identifiers, shared descriptor layout (set 0 from global root sig like compute + bindless mirror set 1), `bd12_cmd_dispatch_rays` -> `vkCmdTraceRaysKHR`.
- COM: `ComStateObject` (ID3D12StateObject + embedded ID3D12StateObjectProperties) in bd12_com.c.
- Translator: `beer_d3d12_dxil_translate_library` (all entry points, shared bindings); BD12Descriptor is 128 bytes (SBT descriptor shift 7); AS SRV descriptors (`BD12_DESC_SRV_AS`).
- RE8 facts: local root signatures are empty (SBT record = identifier only). Local root signatures with parameters are NOT supported yet (logged) — Ghost of Tsushima will likely need them.
- Debug: `BEER_DXR_DUMP=<dir>` dumps state objects + DXIL libs; `tools/dxr_probe.c` offline converter test (396/396 entry points OK).
- Result: RE8 with RT on creates 66 Vulkan RT pipelines with no failures.

## RE8 loading-screen GPU fault (open, parked 2026-10-26)

Symptom: RE8 freezes (0% CPU, frame counter stuck) or crashes at the loading screen before gameplay. Kernel log shows `NVRM: Xid 31` (MMU fault, GR0_PBDMA0/HUBCLIENT_ESC, VIRT_WRITE at 0x0), sometimes Xid 11/69 (invalid push buffer / class error); the process then shows "device lost" or just hangs.
- NOT caused by: DXR/RT work (reproduces with RT disabled), overlay, final-image pass (BEER_NO_POST), MangoHud, GPU spoof, --prefix, control-socket polling, CPU staging presenter, SYNC_SUBMIT. **The last commit (42a9289) freezes at the same place** (user test with a worktree build), so it predates today's work.
- It looked "manager only" because only a played session reaches the loading screen; idle launches sit at the title (but 2 of ~25 idle launches also hit Xid 69/11 around the intro->title transition). Launching from a terminal works for the user.
- Every captured failure: last BD12 submissions are a texture-streaming mip-chain upload (6x `copy-buffer-image` 1024..32) followed by fence-only submissions. Pattern points at corrupted/invalid command stream, not a shader.
- Hardening done meanwhile (harmless): deferred destruction of descriptor heaps, root signatures and command signatures (`bd12_com.c`); fence last-use tracking + wait before destroying a fence; `vulkan_post_forget_upload()` (cached view of a recreated upload image); SRW lock owner tracking and event details in the USR1 dump.
- Next step when resumed: run with `BEER_VK_CHECKPOINTS=1` (+ `BEER_BD12_FLIGHT_RECORDER=<dir>`), reproduce, `kill -USR1 <pid>` (or wait for "device lost") -> `[CKPT]` lines name the last draw/copy/dispatch the GPU reached (`graphics/d3d12v/bd12_ckpt.c`, marks come from `bd12_cmd_record_flight_op` and `record_dispatch`). Flight recorder files now also list each submission's fence waits/signals.
- Diagnostics added: USR1 dump prints SRWLOCK owner and event state; `kill -USR1` also dumps GPU checkpoints.

### DXR status for RE8 (parked 2026-10-26)
Works: state objects (66 permutations created at startup, pipelines built lazily), shader identifiers, SetPipelineState1/DispatchRays, BLAS/TLAS builds (774-instance TLAS, thousands of BLAS updates per run), AS descriptors, ray-query shaders accepted in ordinary compute/pixel shaders (`accel_ok=1`). Descriptor-lifetime crash fixed (`bd12_live.c`: live registry + per-object serial checked when resolving descriptors; stale descriptors act as null).
Not working: with `RayTracing=true` RE8 always dispatches ONE state object whose RG_Main is an empty function (lib 6fb64d023c9fc6e8, 3 descriptors) - 522240x1x1 and 2082240x1x1 each frame - so no RT GI/AO/shadow result is produced and the scene renders dark (RT reflections look fine). Tried without effect: RayTracingAOSetting=ON, GPU spoof (4090/24 GB), `BEER_DXR_FEATURES=1` (real OPTIONS/OPTIONS1/root-signature-1.1 values). The game picks the permutation itself; to continue, record the guest return address in `cl_rt_set_pso1` and read the game code that selects the state object.
Debug switches added: `BEER_DXR=1` (advertise tier 1.1), `BEER_DXR_PROBE=1` (logs), `BEER_DXR_LIST=1` (pipeline bindings), `BEER_DXR_FEATURES=1`, capture of RT dispatches via `BEER_BD12_CAPTURE_CS=d7a0`, `BEER_BD12_BIND_LOG=1`, `BEER_VK_CHECKPOINTS=1`.

## Performance pass (2026-10-26, RE8 gameplay, RT off)
Baseline: ~100 fps, GPU 55-70%, `resolve` (descriptor-set building) ~1.35 s per 2 s window (~700k calls, ~3400 per frame), loading hitches from heap creation (up to 387 ms/2 s window).
Changes:
- **Batched heap zero-fill** (`bd12_device.c`: `heap_zero_fill` / `bd12_zero_flush`): the per-heap command pool + fence + submit became one shared command buffer per batch on a device timeline semaphore; the batch is submitted at the top of `bd12_queue_submit` (and before a heap is destroyed). Heap create fell from ~387 ms to 2-27 ms per window: loading stutter essentially gone.
- **Lock-free live-object registry** (`bd12_live.c`, fixed 2M-slot table, writers locked, readers atomic) and a lock-free fast path in `bd12_image_ensure_initialized`: resolve 1.35 s -> 1.15 s per window.
- Profiler: new sections `alloc-set` and `update-sets` (driver cost of descriptor sets is only ~80 ms + ~140 ms per window; the rest of `resolve` is loader bookkeeping, ~1.6 us per call).
Result: ~132 fps, GPU 73-90%, no single hot thread (top thread ~60% CPU, 8 workers at 35-60%).
Ideas not done: cache candidate root parameters per shader binding (skip scanning all 13 root params per slot), reuse descriptor sets when bound state is unchanged, replace the per-submit `retire` timeline wait with a pipeline barrier, per-buffer view locks instead of device-wide `view_lock`.

## Sekiro on BD11 (`BEER_D3D11_BD11=1`)

- Black screen: `get_pso()` flushed the command list on every draw, so every draw was recorded without pipeline/targets and dropped. Flush removed.
- Garbled title (extra layers, duplicated letters): Sekiro uploads its UI mesh pool into DEFAULT vertex/index buffers with `UpdateSubresource(box)` on deferred contexts, passing a pointer to the START of the pool. The D3D11 runtime reads buffer data at `pSrcData + box.left` in that case, so `ctx_update` now offsets the source by `box[0]` for boxed buffer updates on deferred contexts (`BEER_BD11_NO_SRC_OFFSET=1` disables it). Title now matches Proton's.
- Debug env: `BEER_BD11_MAPLOG` (deferred map/unmap ranges, big-buffer creation/updates), `BEER_BD11_VBDUMP=<file>` (dump bound VB/IB mirror at the traced frame, needs `BEER_BD11_HOSTBUF=1`), trace now prints raw vertex dwords.
- Sekiro intro movie (Bink) froze: (1) `CreateMutex` returned one fake handle for every mutex and waits on it failed, so Bink threads spun; now real recursive owner-tracked mutexes (table of 65536; Sekiro creates >1000; `BEER_MUTEX_LOG`, `BEER_OLD_MUTEX`). (2) DirectSound `Unlock` blocked on the audio queue while Bink's audio thread held a Bink lock, starving the main thread; `beer_audio_submit_nowait` for DirectSound (ring size bounds the lead). Audio writes are also split into 20 ms pieces, prebuf=0, a watchdog flushes a stalled pa_simple_write (`BEER_AUDIO_DIAGNOSTICS` shows write age/played/queued).
- Window focus: SetFocus/SetForegroundWindow no longer steal focus after the user switched away; ShowWindow(SHOW) on a visible window no longer raises it.
- BD11 is the default D3D11 path (`BEER_D3D11_LEGACY=1` for the old one).
- Sekiro 3D world crash: the game's `longjmp` target frame lives on the host-call stack while the jumping code runs on the guest stack; `beer_rtl_unwind_helper` cannot walk across stacks, so RtlUnwindEx returned and the CRT looped into garbage (crash at RVA 0x269647e, rcx=0x56). Fallback: for STATUS_LONGJUMP (0x80000026) restore the registers straight from the jmp_buf (`[UNWIND] longjmp across stacks` in the log). Crash handler now prints raw stack words + guest return RVAs.
- BD11 compute: `Dispatch` now runs (`do_dispatch` in bd11_ctx.c uses `bd12_compute_pipeline_create_dxbc` + `bd12_cmd_dispatch_bound` through the BD11 resolve hook, global barriers around it). DispatchIndirect, UAV counters and ClearUAV are still missing; tessellation (HS/DS) draws are still skipped.
- Sekiro loading took 2000 ms/frame: `ctx_unmap` byte-scanned multi-MB streaming buffers (and `ctx_map` copied them) on every deferred `Map`, hundreds per frame. Deferred Map(DISCARD/NO_OVERWRITE/WRITE) now hands out a PROT_NONE scratch; first touch faults into `b11_lazy_fault` (hook in `on_crash`, filled from the live buffer or zeros) and Unmap only inspects dirty pages. Frame time 2000 ms -> ~18 ms.
- Persistent VkPipelineCache (`~/.cache/beer/pipelines-<vendor>-<device>-<driver>.bin`, `BEER_NO_PIPELINE_CACHE=1` disables); saved after 25 new pipelines (>=20 s apart) and at exit.
- Event/semaphore tables raised (131072/65536), semaphore slots reused: Sekiro panicked with "DLPlainConditionSignal: Failed Create Event" when the old tables filled.

## RDR2 bring-up (started 2026-10-26)

Entry point is `Launcher.exe` (Razor1911 launcher substitute, dynamic MSVC CRT), which is supposed to serve `\\.\pipe\RDR2Launcher_Pipe` and start `RDR2.exe` as a child. `RDR2.exe` itself is a statically linked, Arxan-style self-decrypting exe (file code is encrypted; disassemble from a live dump, e.g. `gdb -batch` with `handle SIGTRAP stop`, `dump binary memory`) plus `1911.dll` (crack, loads and attaches fine). Game uses `12on7\D3D12.DLL` (served by the fake D3D12, BD12 comes up fine on the RTX 2070 SUPER) and `vulkan-1.dll`/FSR/DLSS DLLs that are never touched so far.

Direct `loader RDR2.exe` findings, in the order the game hit them (all fixed generically unless marked):
- **`\\?\C:\...` paths** (`resolve_windows_host_path`): the Win32 file-namespace prefix was not stripped, so `CreateDirectoryW` of `\\?\C:\Users\Player\AppData\Local\Rockstar Games` failed and the game could not create `exit_file.dat`. `\\?\`, `\\.\` and `\??\` prefixes are now stripped for every caller of the resolver.
- **`GetPhysicallyInstalledSystemMemory`** was a zero stub; now returns installed RAM in KiB from `sysconf`.
- **`WSAStartup`** always returned wVersion 2.2. RDR2 asks for 2.0 and demands exactly 2.0 (`cmp al,2` / high byte == 0), otherwise it quits with "Game exited unexpectedly!" (rage fatal error file hash 0x82F6A12F line 0xFD; the same fatal-error dump goes to `%LOCALAPPDATA%\Rockstar Games\Red Dead Redemption 2\exit_file.dat`). wVersion is now the requested version capped at 2.2 (1.x capped at 1.1).
- The int3/SIGTRAP at RVA 0x25b520d seen in the first runs is just the instruction after the game's `exit(0)` call (the process was already dying), not a trap check.
- Next failure (open): `ERR_NO_LAUNCHER: Please run Red Dead Redemption 2 from the Rockstar Games Launcher`. The game looks for a launcher process (`WinVerifyTrust`, `CreateToolhelp32Snapshot`/`Process32First`), reads `HKLM\SOFTWARE\WOW6432Node\Rockstar Games\Launcher\InstallFolder` (`RegGetValueW`) and tries `ShellExecuteExW`. Those are zero stubs today, so RDR2 has to be started through `Launcher.exe`.

Diagnostics added: `BEER_DEBUG_CRASH=1` now also prints every stack word that points into the main image as a candidate return address (`stack+0x.. -> image RVA 0x..`), useful for packed games with no unwind data until decrypted.

Launcher.exe status: it imports the dynamic CRT (UCRT api-sets, `VCRUNTIME140`, `VCRUNTIME140_1`, `MSVCP140` iostreams, C++ exceptions), D3D12/DXGI/DComposition and winmm. Without the Microsoft runtime DLLs the loader stubs them to 0 and the process spins in `__C_specific_handler` forever (30M log lines in 30 s). The loader already supports bundled runtime DLLs through `BEER_DLL_PATH` (default `~/.local/share/beer/runtime`, see the RE4 notes above), but that directory does not exist on this machine. Wine's builtin `ucrtbase`/`msvcp140`/`vcruntime140_1` from `~/.wine` were tried and are NOT suitable: they import `__wine_dbg_header`/`__wine_dbg_output` from ntdll and then crash in `EnterCriticalSection` on statically initialised Wine critical sections. Genuine Microsoft copies are needed (RE8's `Win64/` folder has them), then the remaining work is multi-process support (`CreateProcess` of a second loader, named pipes, Toolhelp snapshots, registry values).

Build note: `third_party/` is gitignored; `third_party/dxil-spirv` must be cloned and `make dxil-spirv` run before `make` links (the `.d` files depend on `dxil_spirv_c.h`).

### RDR2 bring-up, session 2 (2026-10-26): launcher path, runtime DLLs, multi-process, graphics probes
Everything below is generic loader work; RDR2 is only the test case.

**Runtime DLLs.** `~/.local/share/beer/runtime/` now holds genuine Microsoft `ucrtbase.dll`, `msvcp140.dll`, `vcruntime140.dll`, `concrt140.dll`, `vccorlib140.dll` (extracted from the 2017 vc_redist) and `vcruntime140_1.dll` (from `~/.wine`, it has no Wine-specific imports). `api-ms-win-crt-*` imports of guest DLLs load `ucrtbase.dll` lazily (`g_loading_ucrtbase` guard). Guest DLL `DllMain` calls are now dependency ordered (1911.dll crashed in static init when its CRT had not initialised yet). `RtlPcToFileHeader` also searches guest DLLs (msvcp140's `__ExceptionPtr` copy faulted on a null image base). Any `oo2core_*_win64.dll` is now treated as the required real Oodle DLL (was hard-coded to one version).

**Multi-process model** (`platform/win32_process.inc`). A guest process is a host loader process (pid = host pid). `CreateProcessA/W` resolves the exe (MZ magic check), builds `loader [forwarded -- options] --cmdline=<full command line> <exe>` and `posix_spawn`s `/proc/self/exe`; `GetCommandLineA/W` and the PEB command line come from `--cmdline`. Process handles are `0xD1000000+`, snapshot handles `0xD2000000+`. Each loader writes `$XDG_RUNTIME_DIR/beer/<pid>.proc` (ppid, exe name, Windows path, image base) so `CreateToolhelp32Snapshot`/`Process32First/Next(W)`, `OpenProcess`, `GetProcessId`, `GetExitCodeProcess`, `K32EnumProcessModules(Ex)`, `GetModuleFileNameEx`, `GetModuleBaseName`, `QueryFullProcessImageName`, `GetProcessImageFileName` work across processes (stale files from crashed loaders are cleaned by the snapshot). `TerminateProcess` kills a child. Named pipes are `AF_UNIX` sockets in `$XDG_RUNTIME_DIR/beer/pipe-<name>.sock` (`CreateNamedPipe`, `ConnectNamedPipe` incl. overlapped, implicit accept on first read/write returning ERROR_PIPE_LISTENING until a client connects, `CreateFile("\\.\pipe\..")` client, `WaitNamedPipe`, `PeekNamedPipe`, `SetNamedPipeHandleState`, `DisconnectNamedPipe`). With this the Rockstar launcher check passes: RDR2.exe is started by `Launcher.exe`, finds its parent via Toolhelp, `RegGetValue(HKLM\SOFTWARE\Rockstar Games\Launcher, InstallFolder)` (seeded with the exe's folder; `RegGetValueA/W` added) and loads its rpf archives.

**Other fixes found on the way.** `SHGetFolderPath` CSIDL 0x05/0x28/0x23/0x24 (Documents, profile, ProgramData, Windows) were wrong or missing; `RegisterClassA/W` were zero stubs (only the Ex variants worked, so a plain-class window never got created); `QueryDisplayConfig`, `GetDisplayConfigBufferSizes`, `DisplayConfigGetDeviceInfo` (source name, target name, preferred mode, adapter name, base type, advanced colour = off, SDR white level) and `GetDpiForMonitor` were zero stubs that left their outputs unwritten, which made RDR2's monitor enumeration loop and then crash (`platform/win32_display_config.inc`); `Direct3DCreate9`/`Direct3DCreate9Ex` return a minimal adapter-query-only `IDirect3D9Ex` (`platform/win32_d3d9.inc`, `CreateDevice` reports D3DERR_NOTAVAILABLE); `GetPhysicallyInstalledSystemMemory`, `WSAStartup` (see above).

**NVAPI** (`platform/win32_nvapi.inc`). `nvapi_QueryInterface(id)` was missing, which made RDR2 stop with "Minimum Recommended Hardware Check Failure ... requires driver 511.79 or newer". It now serves Initialize, Unload, GetErrorMessage, GetInterfaceVersionString, SYS_GetDriverAndBranchVersion (driver from `/proc/driver/nvidia/version`, 595.91 -> 59591, `BEER_SPOOF_NVAPI_DRIVER` overrides), EnumPhysicalGPUs, GPU_GetFullName / GetPCIIdentifiers / frame buffer size / GPU type / system type / bus id, EnumNvidiaDisplayHandle. Initialize fails with NVAPI_NVIDIA_DEVICE_NOT_FOUND when the (possibly spoofed) vendor is not 0x10de. Unknown ids are logged once as `[NVAPI] interface 0x... is not implemented`. RDR2 asks for 0xAD298D3F, 0x33C7358C, 0x593E8644, 0x34EF9506, 0xC7026A87, 0xDCB616C3, 0xAE457190, 0x1E9D8A31 (not identified yet; all optional so far).

**Where RDR2 stands.** Launcher -> RDR2.exe -> rage init -> hardware check passes -> graphics: it probes both the D3D12 adapter (`12on7\D3D12.DLL`, BD12 device comes up, `CheckFeatureSupport` runs) and the Vulkan adapter (`vkCreateInstance` through the mapper below succeeds, `vkEnumeratePhysicalDeviceGroups`, `vkGetPhysicalDeviceProperties(2)` and memory properties are called). It then never calls `vkCreateDevice`/`vkCreateWin32SurfaceKHR`: with `kSettingAPI_Vulkan` the process crashes in its own code (null object, RVA 0x2c8363f and following, ends in `__fastfail`), with `kSettingAPI_DX12` it shows `ERR_GFX_D3D_INIT`. The cause is not found yet; the first thing to check is the result of its Vulkan physical-device selection and what the D3D12 adapter is missing (SM 6.6 / ResourceBindingTier, see the GoT notes above). `system.xml` lives in the prefix (`Documents/Rockstar Games/Red Dead Redemption 2/Settings/system.xml`, copied from the Steam compatdata); the DX12 variant is saved next to it as `system.xml.dx12.bak`.

### Vulkan-on-Vulkan mapper (2026-10-26)
Native Vulkan games get the same manager features as the D3D12 core. Files: `renderer/vulkan/vulkan_bridge.c` (the built-in `vulkan-1.dll`: Microsoft x64 -> System V ABI thunks, instance/device/layer/surface translation, see its header comment) and `renderer/vulkan/vulkan_map.c` (what Beer does to the calls).

Why not share the loader's Vulkan device like BD12 does: the shared device has one queue and a fixed extension list, a Vulkan guest chooses its own extensions, features, queues and threads. So the guest owns its `VkInstance`/`VkDevice` and the mapper intercepts:
- `vkQueuePresentKHR` (frame boundary): `beer_control_note_frame()` (fps counter, overlay frame graph), `beer_control_frame_limit_wait()` (live frame cap), then, when effects, the overlay or a screenshot are active, the same compute passes the D3D12 presenter uses (`vulkan_post_apply`, re-bound to the guest device with `vulkan_post_bind_device`) are recorded and submitted on the presenting queue (waits for the guest's semaphores, signals its own, the present waits on that). When nothing is active the recording is dropped and the present is untouched.
- `vkCreateSwapchainKHR`: adds `TRANSFER_SRC|DST` usage when the surface supports it (falls back to the exact create-info when the driver refuses). The format is never changed. RGBA8 UNORM/sRGB: raw `vkCmdCopyImage`; BGRA8 UNORM: blit (swizzles); BGRA8 sRGB: raw copy into a BGRA8 UNORM staging image, then blit (no sRGB decode/encode). Other formats (HDR, 10-bit, float) run without effects. A first attempt that re-created sRGB swapchains as UNORM + mutable format was dropped because a guest that clears or blits straight into the swapchain image would lose the sRGB encode.
- `vkGetPhysicalDeviceProperties(2/2KHR)` and `vkGetPhysicalDeviceMemoryProperties(2/2KHR)`: `BEER_SPOOF_GPU_NAME/_VENDOR/_DEVICE/_VRAM_MB` (shared helper `beer_vulkan_gpu_spoof`, DXGI uses the same one).
- `vkGetDeviceQueue(2)` / `vkDestroyDevice` / `vkDestroySwapchainKHR`: queue->family registry and cleanup.
Everything else is forwarded through the ABI bridge.

Verified with `tests/vulkan_guest/vkclear.exe` (freestanding Windows exe: window, instance, device, swapchain, clear + present; build with `tests/vulkan_guest/build.sh`, needs clang + lld-link + llvm-dlltool; run `./loader --cmdline='vkclear.exe [srgb] frames=N' tests/vulkan_guest/vkclear.exe`): channel order and values correct for BGRA UNORM and BGRA sRGB swapchains (sRGB clears come out sRGB-encoded), brightness and colour-blind effects change the output, `screenshot` (clean and with overlay) writes the PNG, the overlay box is drawn, `fps_limit` 60 gives 60.0 fps (uncapped ~180), the Khronos validation layer (`VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`) reports nothing from the mapper (only the test's own semaphore reuse).

Debugging: `BEER_VKBRIDGE_TRACE=1` logs every entry point the guest resolves, `=2` also logs every call (`[VKCALL] name`, floats variants are not traced); `BEER_NO_POST=1` disables the swapchain/effects hooks; `BEER_ALLOW_GUEST_VULKAN=1` uses a guest-provided `vulkan-1.dll` instead; `BEER_GUEST_VULKAN=0` hides Vulkan from guests. Not done: multi-swapchain presents (effects skipped), HDR swapchains, pause/time-scale (handled by the time layer, not the mapper), tweaks for guest pipelines (the constant-buffer tweak tool is D3D12 only), `VK_KHR_present_wait`/present-id pass through untouched, guest debug callbacks and layers are dropped, allocators are ignored.

Gotchas found:
- `make clean` removes the tracked `renderer/vulkan/shaders/*.spv` and `glslc` (a path into another machine's NDK) is missing here, so the next `make` fails until they are restored with `git restore -- renderer/vulkan/shaders/*.spv`. Something ran `make clean` three times during this session while the game was running (not this session's commands); do not run it here.
- The freestanding test exe must be built with `-O0`: with `-O1` clang kept a struct in a stack slot that it also used for a spilled xmm register in the loop (the guest saw its `VkCommandBufferBeginInfo` zeroed on the second frame). If a Windows guest ever shows this, check the guest's own stack slot reuse before suspecting the bridge.

### RDR2 bring-up, session 3 (2026-10-26): non-determinism, stack/ABI bugs, audio, window
RDR2 now gets through input, Vulkan device + swapchain creation, the first presents (mapper active) and audio init; the next failure is the game's window/display-mode selection (below). Findings, all generic:

- **Runs were non-deterministic because of ASLR.** The game took different graphics start-up paths depending on where the host placed heaps/stacks (about 70 % of runs never reached `vkCreateDevice`). `main()` now re-execs once with `personality(ADDR_NO_RANDOMIZE)` (set `BEER_ASLR=1` to keep randomisation). With it every run takes the same path; gdb sessions match normal runs.
- **`InitOnceExecuteOnce` was not thread safe** (a second thread returned while the callback was still running, the callback's result and context were ignored). Now blocks other callers until the callback finished, stores the context, and a failed callback leaves the once retryable.
- **`impl_XInputGetState/SetState` "disconnected" overran the guest stack.** One shared function memset 16 bytes into the caller's buffer, but `XInputSetState` passes a 4-byte `XINPUT_VIBRATION`; the memset clobbered a saved `rsi` in RDR2's frame (loop counter garbage, crashes at 0x14263cdda). Real XInput does not touch the buffer on failure; neither do we now.
- **Vulkan calls run on the thread's host call stack.** Guest worker threads can have small stacks and Vulkan drivers use a lot; every thunk (forwarded command or Beer's own override such as `vkQueuePresentKHR`) now enters through `beer_vk_common`, which switches to `g_host_call_stack_top`, copies the 12 stack-argument slots, calls, and returns on the guest stack. Thunks carry a `{host, entry, name}` record (`VkThunk`) instead of embedding the target.
- **DXGI/Vulkan device pairing.** RDR2 finds the Vulkan device whose `VkPhysicalDeviceIDProperties::deviceLUID` equals the DXGI adapter LUID (it compares the sign-extended 32-bit low part against the full 64-bit LUID). Linux drivers report no LUID, so the mapper reports the DXGI one (only for the host GPU, not for llvmpipe), and Beer's LUIDs now have HighPart 0 (`vendor<<16|device`, positive).
- **`CM_Get_Device_Interface_List*`** (cfgmgr32) are still zero stubs; the game only uses them to read VRAM through D3DKMT, which also fails, harmlessly.
- **WASAPI**: RDR2's audio mixer needs `IMMDevice::OpenPropertyStore` (friendly name), `IMMDevice::GetId`, and `IAudioClient::GetService(IAudioSessionControl)`. Added minimal `IPropertyStore` and `IAudioSessionControl` objects; unsupported interface requests are now logged (`[WASAPI] ... E_NOINTERFACE`).
- **DPI APIs**: `AdjustWindowRectExForDpi`, `GetDpiForSystem/Window`, `EnableNonClientDpiScaling`, `SetDialog*DpiChangeBehavior`, `SetProcessDpiAwareness(Context)`, `SetProcessDPIAware`, `GetThreadDpiAwarenessContext`, `AreDpiAwarenessContextsEqual` (96 dpi, identity scaling).
- **Zero-sized `CreateWindowEx`** (engines create hidden 0x0 probe windows) no longer fails; the native window gets at least 1 px.
- Diagnostics added: `BEER_CRASH_CODE`, `BEER_CRASH_RANGES=START:LEN,...` (hex dumps of guest memory at the first fault: pipe through `xxd -r -p` + `objdump -D -b binary -mi386:x86-64 --adjust-vma=`), `BEER_DUMP_RANGE`, `BEER_VKBRIDGE_TRACE=1|2|3` (resolves / every call with the guest return address / plus call-site code), `BEER_WINDOW_DIAGNOSTICS`, `[STUB]` lines show the caller address, `[NVAPI]` unknown interfaces. Debugging recipe that worked: gdb with `set follow-fork-mode child` + `follow-exec-mode same`, a Python breakpoint on `beer_vkbridge_trace_call` that creates the real probes on its second hit (hardware breakpoints cannot be set before the guest image is decrypted; there are only 4), watchpoints via `gdb.Breakpoint(expr, gdb.BP_WATCHPOINT, gdb.WP_WRITE)`.

**Open (next step).** The game computes its window from the display-mode record `adapter+0x198+output*0x110` (RECT at +0x40: left, top, right, bottom). Its display match function (0x1426d5f60, QueryDisplayConfig/EnumDisplayDevices based) returns false here, so the fallback fills the RECT with uninitialised data and `CreateWindowExW` gets garbage x/y/w/h (then `ERROR:FFFFFFFF`, hash 0xE1BD3165 line 0x30F). d5f60 must return true on Windows; find which comparison fails (candidates: source-name vs `EnumDisplayDevicesW` name, the `PCI\VEN_xxxx&DEV_xxxx` parse against the DXGI adapter desc at +0x100/+0x104, `DisplayConfigGetDeviceInfo` types the game asks for).

### RDR2 bring-up, session 4 (2026-10-26): the game renders
With the fixes below RDR2 (Vulkan API, `Launcher.exe`) creates a 2560x1440 window, a Vulkan device and swapchain, initialises audio and input, and renders ~190 frames through the Vulkan mapper (render passes, submits, presents, NvLL latency markers) before a crash inside its copy-protection code.

Fixed (all generic):
- **`EnumDisplaySettingsW` returned the DEVMODEA layout.** DEVMODEW has 64-byte name fields, so `dmBitsPerPel/PelsWidth/PelsHeight/DisplayFrequency` are at 168/172/176/184 (A: 104/108/112/120). The game found no valid display mode and fell back to uninitialised data. A and W now have separate layouts (`enum_display_settings`).
- **`SetRect`, `OffsetRect`, `InflateRect`, `IntersectRect` were no-op stubs** (the window size is computed with `SetRect` + `AdjustWindowRectEx`, so `CreateWindowEx` got garbage x/y/w/h). Implemented the whole user32 RECT family.
- `DISPLAYCONFIG_PATH_INFO.sourceInfo.modeInfoIdx` is now a plain index (0), not the packed `cloneGroupId=0xFFFF` form (the game indexes the mode array with it).
- `getaddrinfo`/`GetAddrInfoW`/`freeaddrinfo` (zero stubs reported success without writing `*result`); only NULL/localhost/dotted IPv4 resolve, the rest fail with WSAHOST_NOT_FOUND (offline by default).
- `SHGetSpecialFolderPathA/W` and CSIDL Program Files; `NtQueryInformationProcess` (basic info, debug port/object/flags look "not debugged"), `NtQueryVirtualMemory` (class 0 = VirtualQuery), `NtOpenProcess` (self), `NtQuerySystemInformation(SystemKernelDebuggerInformation)` no longer reports a kernel debugger, `GetNativeSystemInfo`, `Get/SetThreadGroupAffinity`, `SetDefaultDllDirectories`, `SetThreadStackGuarantee`, `Thread32First/Next` over `/proc/self/task`.
- API trace lines (`BEER_API_TRACE`) now carry the host thread id (`[API n tTID]`), the first SIGSEGV line prints its thread.

**Open: copy-protection (Arxan) crash after ~24 s / ~190 frames.** An obfuscated guard stub in the second `.text` (RVA 0x6033000+, e.g. 0x1470a411b `mov %rax,(%rcx)`) overwrites its caller's return address (slot at `rbp+8` of the calling frame) with `0x186d30a12`; the `ret` then jumps there (RIP=FA=0x186d30a12, thread != main). The value is identical in every run and with ASLR on or off, so it is a pure function of the image/environment, and it is exactly `0x146d30a12 + 0x40000000` (0x146d30a12 is a valid in-image address, so something contributes +0x40000000 too much; candidates: a Win32 constant or an API result that is 0x4000xxxx here, e.g. our fake export/trampoline addresses live at 0x40dc....). The routine is control-flow-flattened (`push rbp; lea rbp,[..]; xchg rbp,(rsp); ret` gadgets), so find the inputs dynamically: record with gdb (`record full` from a breakpoint just before the guard runs), or compare a run of the same exe under Wine. The stubs `UuidCreateSequential`, `GetSystemDefaultLocaleName`, `OpenProcessToken/AdjustTokenPrivileges`, `WinVerifyTrust`, `EventRegister`, `HeapSetInformation`, `SetHandleCount` are still zero-return stubs and are the first candidates (output buffers left uninitialised).

Handy: `tests/vulkan_guest` still passes after all of this; `make` can fail with "glslc: No such file" because something deletes the tracked `.spv` files (restore: `git restore -- renderer/vulkan/shaders/*.spv`).
