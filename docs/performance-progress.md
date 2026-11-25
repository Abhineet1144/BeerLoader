# Performance & Benchmark Progress

Goal: make the loader as fast as the hardware allows (absolute numbers, not comparisons with Proton, whose performance
varies from day to day). Primary test game: Onimusha - Way of the Sword, same in-game spot every run.

## How to benchmark

```
tools/bench.sh -n <label> [-s SECONDS] [-e VAR=val ...] "<game.exe>"
```
- Interactive (tty): launch, walk to the spot, press Enter; the next SECONDS (default 30) are measured.
- Non-interactive: `BENCH_DELAY=<s>` seconds after launch (default 45) the measurement starts.
- Output: `~/.beer/bench/<label>.log` (full log), `<label>.window` (measured part), one summary appended to
  `~/.beer/bench/summary.txt`: average fps / frame ms, worst window, host-side section times, barrier and render-pass counts.
- Run-to-run noise at the Onimusha spot is about +/-5 fps; only larger differences are real.
- CPU profile: `BEER_PROF_SAMPLE=wait`, send SIGUSR2 once after loading (start) and once more (dump `/tmp/beer-prof.txt`).
  **Do not use `BEER_BD12_SAMPLE=1` on Onimusha**: it crashes the game at startup (guest SIGSEGV, deterministic, also
  with an -O0 core; open).

## Baseline (Onimusha, RTX 4070 Ti SUPER, i7-12700K, 15 GB RAM)

| Run | Build | avg fps | avg frame | worst frame | note |
|---|---|---|---|---|---|
| oni-O2 | -O2 core | 76.1 | 13.6 ms | 948 ms | includes pipeline compile at load |
| oni-O2b | -O2 core | 71.4 | 14.0 ms | 32 ms | steady |
| oni-mf | -O2 + MF single copy | 77.7 | 12.9 ms | 23 ms | steady |
| oni-base2 | same, 75 s after launch | 78.8 | 12.7 ms | 25 ms | steady (noise vs oni-mf: 1 fps) |
| (scratch) | -O0 core, O2 elsewhere | 78.1 | - | - | last 20 windows; **-O2 gain at this spot: none measurable** |

**-O0 vs -O2 at the Onimusha spot: 78.1 vs 77.7-78.8 fps, no measurable difference.** The game's own code dominates the CPU
(see findings); -O2 should matter more in API-heavy phases (loading, streaming). It stays the default regardless.

Steady-state profile (per frame at ~80 fps): ~640 barriers (58 full ALL_COMMANDS), 92 render passes, ~3300 view creates,
~7000 descriptor copies, 10 ExecuteCommandLists. GPU ~68% busy (not GPU-bound). CPU samples: ~70% guest code, ~20% libc,
~5% loader+BD12, ~4% driver. RAM ~12.7 of 15 GB, swap 3.7 GB in use.

## Fixes made

1. **Loader core built with -O2** (was -O0). See "What -O2 does" below. `Makefile`: `OPT ?= -O2 -fno-omit-frame-pointer
   -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks`. Rollback: `make clean && make OPT=-O0`.
   - loader.c: `__attribute__((used))` on statics referenced only from inline asm (`g_impl_targets`, `g_impl_real`,
     `beer_trace_log`, `beer_trace_ret`, `beer_rtl_unwind_helper`, `beer_raise_exception_helper`); -O2 discarded them.
   - Verified: smoke-cli, vkclear.exe, test-native-loader, test-native-sync, test-ki-dispatcher, Onimusha runs.
   - NOT verified: RE8, RE9, WWM, Stellar Blade, others. Watch for uninitialised variables, stack-layout assumptions in
     the exception-unwind code, timing-sensitive races.
2. **Media Foundation video frames copied twice** (media/media_foundation.c). Frames were read into `frame_data`, then
   malloc+memcpy'd into the sample (~12 MB extra per 4K frame). Now read directly into the sample's own buffer
   (`mf_sample_create_impl(..., adopt)`); audio path unchanged.
3. **tools/bench.sh** added (see above).
4. **Media Foundation: decoders start on first read** (`decoder_lazy`, media/media_foundation.c). At load Onimusha opens ~5 clips
   (5-17 MB, up to 4K) at once but plays one at a time; each open used to spawn a video+audio ffmpeg pair that burst-decoded
   and then idled holding ~650 MB. Measured 25 s into the load: 4 ffmpeg processes, 2.2 GB RSS, 2.5-7.5 cores before; 0
   processes during the first ~24 s after. Same number of video samples delivered (13 in 40 s). Cost: up to ~130 ms before a
   clip's first frame (ffmpeg start-up). `BEER_MF_LAZY_DECODER=0` restores starting at open. **Needs a visual check** (intro,
   title and loading videos start and look right).
5. **Media Foundation: no restart of an unread decoder.** A rewind (`SetCurrentPosition`) of a reader from which nothing was
   consumed keeps the running pair (same stream); a spare pair for looping is only started for a clip that was played.
6. **Stale clip files cleaned up.** Extracted clips (`/tmp/beer-mf-decode-*`, /tmp is tmpfs = RAM) leaked when a run was killed:
   19 files / 309 MB found. Names now carry the owner pid (`beer-mf-decode-<pid>-XXXXXX`); at first use any file whose owner
   is gone is removed. Older-format leftovers are not touched (I deleted the ones on this machine by hand).
7. **Opt-in GPU video decode: `BEER_MF_HWDEC=1`** (ffmpeg `-hwaccel cuda`, NVDEC). Output is bit-identical to the software
   decoder (300 frames of a 4K H.264 clip, per-frame MD5, 0 differences) at ~4.4x less CPU (1.1 vs 4.9 CPU-s per 300 frames),
   and ffmpeg falls back to software for codecs NVDEC cannot decode. First frame is ~0.6 s later (CUDA context), hence opt-in.
   In the game during load: ffmpeg CPU 176-180% vs 250-750%. Not default until tried across games.

## Findings (2026-11-24/25, Onimusha steady state)

- **Pipeline compile is a loading cost only**, not a steady-state cost. Earlier benchmark windows that started at 45-60 s
  overlapped the load phase. A full run: ~17.7k graphics `CreateGraphicsPipelineState` (2.2k unique shader sets), all 3-5 s
  per 2 s window for the first ~40 s, ~1.5 ms each from the driver cache, then 0. Start measurements >= 75 s after launch.
- **No thread is the loader's fault.** Per-thread CPU (/proc/<pid>/task/*/stat) at the spot: one game thread ~100% that never
  blocks (the render/main thread, zero waits in the wait log), ten game worker threads at 50-77%, the rest idle. Samples on
  the busiest threads: ~84% guest code, <2% loader. The GPU is ~68% busy, so the frame is limited by the game's render thread
  CPU time, not by GPU or locks. Whole-process sample split: ~11% attributable to loader functions (biggest: multi-object
  wait 1.9%, LeaveCriticalSection 1.2%, copy descriptors 0.6%, cs_backend 0.7%).
- **Already optimised before this work:** BEER_* `getenv` is cached per call site (platform/beer_env.h), so hot-path getenv
  calls are not a cost.
- **Transparent huge pages tried and dropped:** `madvise(MADV_HUGEPAGE)` on guest allocations >= 8 MiB (THP is in madvise
  mode here and was never requested): 79.2 / 76.5 fps vs 78.8 / 77.7 without, i.e. noise; only ~1.4 GB became huge pages.
  Not kept.
- **Critical section spinning not changed:** `EnterCriticalSection` goes straight to a futex sleep after one `sem_trywait`
  (Windows spins `SpinCount` first). No evidence of contention cost in the render thread's profile, so left alone.
- How to find the busiest thread: snapshot `utime+stime` per task twice, take the delta; combine with
  `BEER_WAIT_LOG=<trigger>` (waits >= `BEER_WAIT_LOG_US` for `BEER_WAIT_LOG_SECONDS`, output `<trigger>.out`).

8. **Case-insensitive directory cache** (`dircache_lookup`, loader.c, used by `resolve_windows_host_path`). A name that did not
   exist with exactly the requested spelling cost an opendir + scan of the whole directory every time; RE Engine probes
   thousands of missing loose files under `natives/STM/`. Listings are now cached per directory and trusted only while the
   directory's mtime/inode/device are unchanged and the listing was read >= 150 ms after that mtime (coarse FS timestamps);
   a directory that changed during the read, is too fresh, is huge (>120k entries) or cannot be read falls back to the old
   scan. Once a path component is known not to exist, the rest are copied through without file-system calls. Opt-out
   `BEER_DIRCACHE=0`. strace -c over the first 45 s of the Onimusha load: getdents64 38,154 -> 270, openat 90,931 ->
   15,966, newfstatat 204,991 -> 163,882 (failing ones ~100k -> 44k). Isolated test (hits, misses, same-tick change,
   deletion, case-colliding names, fresh directory) passes. **Measured CPU and load time did not change** (process CPU at
   40 s: 170 s vs 168 s) - the benefit is mostly for slow or cold file systems (FUSE NTFS, HDD), where each of those
   calls is expensive.

9. **Dedicated heaps are freed on a background thread** (`beer-heapfree`, bd12_device.c `bd12_heap_destroy`). `vkFreeMemory`
   (~1 ms) ran inline on the thread submitting the frame (`com_reap`); a turn in Onimusha frees dozens of streaming heaps in one
   frame. With RT on, 8 frames had `heap-destroy` bursts (mean 61 frees, 54 ms) and averaged 112 ms; after, 1 such frame
   (39 ms). The game-visible usage counters (`g_bd12_heap_live/bytes`, which feed the DXGI budget) are released immediately; an
   allocation that fails with OUT_OF_DEVICE_MEMORY while frees are queued drains the queue and retries before it demotes to
   system memory; the device drains the queue on destroy. `BEER_BD12_SYNC_HEAP_FREE=1` restores inline frees. Smoke test
   through the load (up to 607 destroys per 2 s window, 0 ms inline), no crash, no "VRAM full" fallbacks.
10. **New profiler counter `pso-vk`** (time inside `vkCreateGraphicsPipelines`, in the `[BD12 PROF]` line and `[HITCH]` lines).

## GPU / stutter investigation (Onimusha, 2026-11-25)

- **The GPU is not the limit; it is starved.** In the heavy scene (RT off 23-40 fps, RT on 22-43 fps) the GPU sat at P0, 2700 MHz
  (max 3120), no throttle reason, but only 60-74% utilised and ~100-130 W (the card draws 250 W+ when saturated).
- **Slow frames are mostly sustained 40-45 ms frames, not spikes** (RT off: 2085 of 2342 slow frames had no pipeline compile).
  The render thread's own CPU time in them is only ~10-20 ms and it waits ~3 ms in guest waits, i.e. the rest is blocked
  somewhere not yet attributed (suspect GPU/driver/Streamline pacing). Next: per-pass GPU timing (`BEER_BD12_GPU_PROF`, which
  caused device loss on Onimusha on 2026-11-14; untested since).
- **Hitch causes found:** (a) heap-free bursts -> fixed by item 9; (b) **pipeline-creation bursts**: the game creates 90-146
  pipelines inside one frame in gameplay (RT on) and the render thread waits (up to 143 ms) on the game event
  `0x60000221` (caller RVA `0x145051326`); (c) loading-phase frames of 100-300 ms.
- **Pipeline creation costs 3.5 ms each and 95% of that is the NVIDIA driver** (`vkCreateGraphicsPipelines`, 17k calls = 61 s
  of CPU per load). Shader translation is negligible. Beer's default for NVIDIA is "in-memory VkPipelineCache, repeat builds from
  the driver's shader disk cache" (`use_pipeline_cache_blob`); the driver cache (`~/.cache/nvidia/GLCache`, 744 MB here) serves
  hits at ~3.5 ms.
- **With Beer's persistent pipeline cache (`BEER_BD12_PIPELINE_CACHE=1`) a warm run costs 0.33 ms per `vkCreateGraphicsPipelines`
  (10x) and total pipeline time during load drops 64 s -> 12 s**; the Onimusha blob is 207 MB
  (`~/.cache/beer/pipelines-10de-2705-988e4100-OnimushaWotS.exe.bin`, or under `BEER_CACHE_DIR`). Not made the default: an earlier
  decision avoided blobs after RE9's reached 1.1 GB and a save froze the game (now a background saver thread). Options: per-game
  env, or default-on with a size cap. **Gameplay effect not yet measured** (needs a warmed cache for the stuttery spot).
- Housekeeping: the root disk was 93% full (7 GB free) when the cache blob was written; `make test-d3d12v` does not link
  (its source list lacks bd12_live/bd12_canary/dxil caps; pre-existing).
- **Locks (futex traffic, critical-section spinning) will be done in a fresh working tree with no other changes**, as requested.

## Load time (Onimusha, warm page cache, NVMe)

- **Not disk I/O.** The game is on NVMe; ~360 MB actually came from disk in the first 66 s. `rchar` (all read() bytes incl.
  pipes) reached 24 GB: the decoded-video pipes (12 MB per 4K NV12 frame), not file streaming.
- **CPU-bound:** ~5.7 cores average over the first 66 s (user 153 s + sys 58 s: 28% kernel time). Sample split during load:
  libc/kernel 42%, guest code 41%, NVIDIA driver 10%, loader 6%.
- **ffmpeg was the biggest avoidable consumer** (see fixes 4-7). Per-reader decoder CPU and RAM came from clips the game
  opened but did not play.
- **Syscall counts, 45 s of load (strace -c):** futex 3.86M calls (619k EAGAIN), newfstatat 205k (100k failing), openat 91k
  (60k failing), getdents64 38k, readlink 36k (35k failing, `realpath()` of the same four parent directories ~4000 times),
  wait4 4.1k (ECHILD), getpid 26k. The failing opens/stats are the engine probing loose files under `natives/STM/...`.
- **Loader function costs during load:** `resolve_windows_host_path` 3.9% of samples (every non-exact path scans the directory),
  multi-object wait 3.1% + notify 2.3%, `Sleep` 1.0%, `bd12_pipelib_find` 0.6%, `fnv` 0.6%, `beer_virtual_free` 0.6%.
- **Load duration varies by +/-10 s run to run on its own.** BD12 `pso=` activity shows bursts (~8 s each) separated by idle
  gaps; across five runs of the same build (eager and lazy decoders alike) the second burst started at 24-54 s. Compare
  load changes over several runs, not one.
- **What is left in the load is the game's own work:** per-thread, ten worker threads at 55-77% of a core in guest code.
  Loader-side: ~3.6M futex calls in 45 s (wait/notify, critical sections) and ~1 core of kernel time. Candidates: spin
  briefly before sleeping in the multi-object wait and critical sections; cache `build_guest_dll_path` (4,000 `realpath` calls
  = 36k `readlink`s in 45 s, ~1% of CPU).
- strace -c needs a quiet machine and wall-clock `-w`; under strace everything is slower, so use counts, not times.

## Open items / ideas (in priority order)

1. ~~Pipeline compile in steady state~~ (resolved: loading only; see findings). Loading itself takes ~40 s of compile at 1.5 ms per
   pipeline across ~17.7k creates: look at why 17.7k creates for 2.2k unique shader sets (distinct states, or repeated?).
2. ~58 full ALL_COMMANDS barriers and ~640 barriers per frame: reduce with per-resource tracking (correctness-sensitive).
3. Memory pressure: swap in use at 12.7 GB; review heap-guard size table (2^21 buckets), caches, ffmpeg buffering.
4. Sparse tile-mapping binds drain the lane once per mapping (see progress file, 2026-11-23).
5. GPU frame ~30% heavier than vkd3d-proton in an earlier comparison; root push costs ~10% GPU at the title screen.
6. `BEER_BD12_SAMPLE` startup crash on Onimusha.
7. `make test-media` does not link (pre-existing: missing vulkan_presenter symbols).

## What -O2 does

`-O0` compiles every C statement literally: variables live in memory, nothing is inlined, no loop or register
optimisation. `-O2` turns on GCC's standard optimiser: values stay in registers, small functions are inlined, dead code
and repeated computations are removed, loops are tightened and instructions are scheduled. Typically 2-5x faster for
branchy code like the API shims, with no change in intended behaviour. The extra flags keep it conservative:
`-fno-strict-aliasing` (do not assume pointers of different types never overlap), `-fwrapv` (signed overflow wraps),
`-fno-delete-null-pointer-checks` (keep null checks), `-fno-omit-frame-pointer` (the unwinder and crash dumper walk frames).
The risk is code that only worked by accident at -O0 (uninitialised variables, undefined behaviour), which -O2 can expose.
