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

## RE9 hitch attribution (2026-11-27; Windows parity at 120 fps, everything high / 1440p / PT / DLSS B / FG 2x)

Tools added (diagnostic only, no behaviour change): `[HITCH]` lines now end with `| this thread: cpu=..` (exact thread CPU
time and the BD12 sections run by the presenting thread itself), `[SLOWPSO]` lines for compiles over 100 ms (kind, sizes,
translate vs Vulkan ms), `BEER_BD12_HITCH_SYSCALL=1` (a thread samples `/proc/self/task/<tid>/syscall` every ~0.5 ms and the
hitch line lists the top states and the futex address), and `[LOCKS]` lines with Beer's device lock addresses. `BEER_BD12_HITCH`
now also totals critical-section / SRW / condvar / WaitOnAddress waits in `waited=` (before only handle waits and sleeps).
The profiler counters are no longer written when profiling is off. While the presenting thread waits on a guest critical section
the hitch line also shows `cs-owner(tid= rip= futex=)`: the holder's host tid, the guest return address of the Enter that took
the lock, and what the holder thread was doing meanwhile (sampled states, and the futex address it blocks on).

- **Cold vs warm:** the 0.7-3.3 s stalls (one thread compiling 1-35 big compute pipelines, 100-430 ms each, in `vkCreateComputePipelines`)
  only happen when the driver cache is cold; a warm run has no compile over 100 ms (0 `[SLOWPSO]`).
- **Warm-run hitches (frames over 20 ms) are not Beer CPU work.** The presenting thread uses 1-8 ms CPU in them; the rest is
  waiting. With the extended wait accounting only 5 of 691 hitch frames have more than 10 ms unexplained (before: 87 of ~340).
  Waits: game events `60000221`/`60000252` (frame/GPU pacing, ~2.5 s of longest-waits), a guest **critical section** at
  `0x37e24348` (not a condvar; callers RVA `0x1453c69b1`, `0x140378484`; ~50 frames, 6-14 ms each, up to 99 ms in single frames,
  while streaming 100+ resources/frame) and a condvar `7ffe841e11c8` (12 frames, ~22 ms each).
- **The holder of `0x37e24348` is another game render-job thread (one fixed tid per run, took the lock from `0x140378484`), and it
  is blocked in a `futex` (660 of 664 sampled ms) while holding it.** It records no guest wait of 0.5 ms or more (BEER_WAIT_LOG),
  so it is blocked on a host-side lock inside Beer (not a guest wait object): the same kind of lock stall the presenting thread
  then inherits through the game's critical section.
- **Holder's futex (re9-hitch12, 38 frames with `cs-owner`):** always one holder thread, always entered from `0x140378484`
  (once from `0x7ffff5108e32`, a loader/host return address), blocked in `futex` on `0x55555b10c798`/`0x55555b10c79c` (the two
  halves of one glibc `sem_t`; in the loader's brk heap). Waits it caused: ~450 ms in 28 frames of 6-16 ms. **It is not any Beer
  BD12 lock**: the `[LOCKS]` addresses (submit_barrier/view/zero, heapdb, device submit/defer/grave/wait) are in a different
  region (`0x5555662...`), and BD12 heap/resource creation times in those frames are ~0. The same address shows up as the
  presenting thread's own futex in other frames, so it is a shared semaphore. Likely another guest synchronisation object
  (nested critical-section backend `CsBackend.lock`, event or semaphore) whose wait is not recorded: this thread has no timed
  wait of 0.5 ms or more in the BEER_WAIT_LOG window, so some blocking path in the loader does not go through `wlog_add`
  (candidates: `sem_wait` paths in critical-section slow path variants, the SRW/WaitOnAddress/keyed-event code, or host helpers).
- **Not found:** the producer that would unblock the holder. Dead ends so far: BD12 locks (excluded by address), PSO compiles
  (0 `[SLOWPSO]` in warm runs), CPU load of the presenting thread (1-8 ms), wait-log correlation (idle worker-pool waits span
  everything, so overlap matching alone is noise; match on the holder's tid).
- **Resolved (2026-11-27, runs re9-hitch13..16): the holder is not blocked on a Beer lock.** Its futex `0x55555b10c798/79c`
  lies in the loader's static `g_events` table (event #594 = handle `0x60000252`; `0x55555b10ad94` is #545 = `0x60000221`):
  the two addresses are the two group slots of that event's `pthread_cond_t`. Hitch lines now print the handle
  (`futex=0x..(handle 60000252)`, via `beer_futex_handle`). So the chain is: holder takes the game lock at `0x140378484`,
  waits for GPU fence event `0x60000252`, presenting thread waits for the lock. The earlier "no wait recorded" was wrong.
- **Fence wait split (`BEER_FENCE_LATENCY=5`, now also prints when the game called Signal, how long Beer held it, the event,
  and `[FENCE HOLD]` lines for every queue Wait that held its queue >= 5 ms, with the Signal's queue type and host tid):**
  | event | waiter | Beer held the Signal | GPU time after submit |
  |---|---|---|---|
  | `0x60000252` | lock holder | 0.0 ms (never) | 8.5 ms median |
  | `0x60000253` | presenting thread | 0.0 ms median | 11.2 ms |
  | `0x600002ce` | presenting thread | 22.7 ms median | 2.6 ms |
  `0x2ce`'s Signal is held behind a cross-queue Wait (direct waits on compute 4.5k times, compute on direct 3.2k, direct on
  copy 2.9k, direct on a CPU Signal 1.4k per ~2 min). Beer releases held work 0.1 ms after the awaited Signal is submitted
  (later "releases" are Waits queued behind another Wait); the awaited Signals come 10-30 ms late because their submitting
  threads (direct submitter, two compute submitters; not the lock holder) sit in `Sleep(1)` loops and waits on `0x60000221`
  (GPU fences) first. The whole chain ends in GPU fence completion.
- **Beer-vs-Windows difference found: every D3D12 queue runs on ONE Vulkan queue by default.** RE9 creates 3 direct, 2 compute
  (one at priority 10000 = HIGH) and 1 copy queue; the log shows all `-> Vulkan queue 0`. Separate queues exist only behind
  `BEER_BD12_MULTI_QUEUE=1`. On Windows the compute/copy queues overlap graphics; here their work (and every fence after it)
  waits behind all graphics work queued before it, which fits the GPU at 60-74% busy while CPU threads wait on fences. The
  older note "RE9 uses no async compute" (beer-loader-progress.md, 2026-11-12) no longer holds at these settings: compute
  queues signal thousands of times. Multi-queue faulted on RE9 then (Xid 31/109, believed to be the start-up race).
- **Next:** measure `BEER_BD12_MULTI_QUEUE=1` at this spot (risk: GPU fault/hang as before); if it helps, fix the multi-queue
  race instead of keeping one queue. Logs: `/mnt/hdd2/beer-trace/re9-hitch13..16-20261127.log`.
- **Multi-queue attempt (re9-mq17, 2026-11-27):** hung during start-up at frame ~778 (title/loading, before gameplay).
  No new Xid in the kernel log. SIGUSR1 dump, twice 4 s apart, unchanged: lane 0 submitted=retired=8680, lane 1 8674/8674,
  **lane 2 (copy) submitted 8683, retired 8681**, no queue holding work on the CPU. So two copy-lane submissions never
  finish on the GPU, i.e. a GPU-side stall on the copy queue, not a CPU deadlock. Fits the 2026-11-12 multi-queue faults.
- **Reproduced (re9-mq18, `BEER_BD12_FLIGHT_RECORDER`, auto-dump script):** start-up again, lane 2 submitted 19397 /
  retired 19396, lanes 0/1 fully retired, nothing held on the CPU. Flight files: `/mnt/hdd2/beer-trace/mq18/hang-copy/`.
  **The stuck copy-lane submission 19397 has NO waits and no command buffers**: it only signals game fence
  `0x55556708a9c0` = 1441 (it signalled 1440 at 19389) plus the lane-2 retire value. 19396 (signal `..a9f0`=1441) completed.
  So the "waits on a value nobody signals" guess was wrong. A signal-only submit on an idle queue not completing points at
  (a) the timeline value not being above what the semaphore already has (another path signalling `..a9c0` without
  `bd12_fence_claim`, e.g. a host/CPU signal or a shared/imported semaphore), or (b) the driver's copy-queue channel being
  stuck from earlier work. Next: log every signal of that fence (queue, lane, value, CPU or GPU) and
  `vkGetSemaphoreCounterValue` of it at the hang; also try the copy lane on queue 0 with compute on 1 to split (a)/(b).
- **Correction: 19397 is not wait-free on the GPU.** `bd12_queue_submit` adds a hidden wait on the heap zero-fill timeline
  (`device->zero_sem`, value `zero_submitted`) to every submission on a non-main queue; the flight recorder does not
  list it. The SIGUSR1 dump now prints `[STACKS] bd12 heap zero fill: recorded= submitted= completed=`: if completed <
  submitted at the hang, the copy lane is stuck on that wait (zero fills go to queue 0 via `bd12_zero_flush`; note that
  function sets `zero_submitted` even when its `QueueSubmit2` fails). Values advance correctly (`zero_recorded =
  zero_submitted + 1`). Rerun: scratchpad `mq18.sh` (multi-queue + flight recorder + auto dump/kill on 16 s without submits).
- **Hang narrowed (re9-mq19..23):** zero fill ruled out (recorded = submitted = completed). Fence timelines of every pending
  submission are below their signal values, so no out-of-order signal. GPU checkpoints on all queues (`bd12_ckpt_dump` now
  queries every queue): every queue reached BOTTOM_OF_PIPE of its last command, i.e. **idle, no spinning shader**, yet
  lane 0 stops at a submission whose only recorded wait is already satisfied and lane 2 at a copy submission with no
  recorded waits. So there is an unrecorded dependency or a driver-side stall. **Compute separate + copy on queue 0
  (`BEER_BD12_MQ_LANES=1,0`, new diagnostic override) does not hang** (two runs, reached gameplay): the hang needs the
  separate copy queue. Remaining suspects: presenter submissions on queue 0 waiting on binary `image_available`, sparse
  binds on the copy lane (`com_bind_sparse`, not in the flight log), driver behaviour of the copy queue.
- **Perf with compute on its own queue (re9-mq24 vs re9-hitch16, same diagnostics, same area):** fence waits improve a lot
  (`0x600002ce`: held 21.6 -> 0.0 ms, wait 24.5 -> 11.0 ms; `0x60000253` wait 20.2 -> 6.8 ms; `0x60000252` unchanged
  ~8 ms), but **fps and hitches do not** (gameplay windows mean 98 -> 108, median 77 -> 72; frames > 50 ms 27 -> 24,
  > 30 ms 58 -> 75; user: "similar to before"). So the single-queue fence latency is real but is not what limits fps or
  causes the remaining hitches. Next lead: what the presenting thread does in the >30 ms frames of re9-mq24.
- **Multi-queue "hang" is a GPU fault (2026-11-28, re9-mqA..D):** the freeze is device loss, not a stall. An empty kick
  submission at the freeze returns `VK_ERROR_DEVICE_LOST` (`BEER_BD12_HANG_KICK=1`), and the kernel log has **Xid 32**
  (corrupted push buffer) in every run, preceded in C and D by **Xid 69** (graphics class error, class `c997`, method
  offset `0x1b0c`, data `0x90100369`, error code `0xc`), always on channel `0x14`. Beer only notices in the fence waiter
  ("device lost during BD12 fence waiter"), so the game just stops.
  - **Copy alone on its own queue faults** (`BEER_BD12_MQ_LANES=0,2`); compute alone does not (runs 23/24). So the
    trigger is copy-queue work on a second VkQueue of the graphics family.
  - **Ruled out:** sparse binds (`BEER_BD12_SPARSE_LANE=0`, new, still faults), zero-fill wait, out-of-order timeline
    signals, a spinning shader (checkpoints), in-flight command list frees (new `[BD12 MQ]` check: 0 hits), queue use
    without its lock (all non-BD12 submits go to queue 0; lane submits are locked).
  - The first submission that never starts (run 22, 32006) is a plain mip-chain upload: six copy-buffer-to-image ops,
    512x256 down to 16x8, 1 byte/texel (BC-compressed). The same uploads run fine on queue 0.
  - Leads: (1) image layout/state assumptions recorded on the copy list while the direct queue uses the same image
    concurrently; (2) block-compressed / tiled (reserved) destination images on a second channel; (3) vkd3d-proton puts
    D3D12 copy queues on a transfer-only queue family, not a second graphics queue. Validation layers
    (`vulkan-validationlayers`, not installed) would check (1).
  - Practical state: compute-only multi-queue works (`BEER_BD12_MULTI_QUEUE=1 BEER_BD12_MQ_LANES=1,0`), gives no fps gain.
  - **ROOT CAUSE (fixed): MangoHud.** `MANGOHUD=1` is set in this desktop session, so its implicit Vulkan layer runs.
    Inside `vkQueuePresentKHR` it records and submits its overlay drawing itself, on a graphics queue it picks (here
    queue 2), without Beer's queue locks. With the copy lane on queue 2 that raced BD12's copy submissions on the same
    VkQueue (externally synchronized) -> corrupted push buffer (Xid 32/69) -> device lost. Compute-only worked because
    queue 2 was then unused. Evidence: validation `UNASSIGNED-Threading-MultipleThreads-Write` on the non-zero queue and
    swapchain-image barriers on it; Beer's own per-queue tracer (`BEER_VK_QUEUE_TRACE=1`) saw no Beer-vs-Beer overlap;
    `DISABLE_MANGOHUD=1` makes the copy split run clean (186 fps windows, no Xid).
  - **Fix:** `beer_vulkan_queue_lock_present()` (vulkan_context.c) holds every queue lock across present when more than
    one queue exists and an overlay library is loaded (MangoHud, OBS vkcapture, Steam overlay; detected with
    `dlopen(RTLD_NOLOAD)`, logged as `[VULKAN] overlay layer ... loaded`). `BEER_PRESENT_LOCK_ALL=1/0` forces it. Not
    done always because a FIFO present can block for vblank and would stall the other queues. Verified: full multi-queue
    (`BEER_BD12_MULTI_QUEUE=1`, compute on 1, copy on 2) with MangoHud on runs without faults (re9-mqG).
  - The Vulkan validation layer (1.4.341) crashes inside itself on RE9 after a short while; usable only for the first
    seconds.
- **Multi-queue test matrix after the fix (2026-11-28, all with `BEER_BD12_MULTI_QUEUE=1`, compute on 1, copy on 2):**
  | game | launch | result |
  |---|---|---|
  | RE9 (same area as run 16, HITCH+FENCE_LATENCY) | manual, MangoHud on | no fault; median gameplay window 76.9 -> 95.5 fps, mean 98 -> 109; fence waits 0x253 20.2 -> 7.5 ms, 0x2ce 24.5 -> 7.9 ms (0 held); frames > 50 ms 27 -> 24 (one run each) |
  | Onimusha (bench.sh at the spot, warm cache) | manual, MangoHud on | no fault; 62.9 -> 63.8 fps (noise), worst window 54.4 -> 59.8, worst frame 34 -> 31 ms |
  | RE8 (~20 min, area transitions) | manual, MangoHud on | no fault, 597 windows |
  | Ghost of Tsushima | manager-equivalent (`manager_launch.py`), MangoHud off | no fault, 100 windows, median 130 fps |
  Note: the manager disables MangoHud by default (`MANGOHUD=0 DISABLE_MANGOHUD=1`), so manager launches never hit the
  overlay race; manual shell runs inherit `MANGOHUD=1`. Onimusha's 63 fps here (manual launch, not manager env) is below
  the 78-79 of 2026-11-25; not investigated.
- **RE9 120 fps goal, where it stands (2026-11-28, manager-equivalent launches, multi-queue default):**
  - Frame generation works: with DLSS-G active there are exactly 2 presents per rendered frame (NGX feature 11
    evaluations track ray reconstruction 1:1; `[NGX] EvaluateFeature` now prints every 600). The ~75 fps shown =
    ~38 rendered + 38 generated. Windows 120 with FG 2x = ~60 rendered.
  - **GPU-bound now:** 98% utilisation, 221-223 W, 2685 MHz for the whole session (single-queue era: 60-74%, 100-130 W).
    So the gap is GPU time per rendered frame: ~26 ms here vs ~16.7 ms implied on Windows (~1.6x).
  - **Barriers ruled out:** 99.3% of full barriers (~7,500 per 2 s window, ~84 per rendered frame) are D3D12 aliasing
    barriers (`cl_barrier`, found with new `BEER_BD12_FULL_SITES=1`). None could be skipped by vkd3d-proton's rule (all
    involve placed or NULL resources). Dropping them entirely (`BEER_BD12_ALIAS_BARRIER=none`, unsafe diagnostic):
    full barriers 7,500 -> 11 per window, fps unchanged (75). Default stays the full drain; `=narrow` = vkd3d-proton's
    version, no measured gain.
  - **Also ruled out by measurement:** BLAS/AS builds (`BEER_BD12_AS_FREEZE=1`, diagnostic: every BLAS built once and
    never again -> 81.7 fps, same as normal; the GPU profiler's per-op timestamps had inflated them to ~25%), VRAM spill
    (13.8 of 16 GB used, under budget, no heaps in system memory), frame generation cost (FG off: 45.9 rendered fps =
    21.8 ms; FG on ~38-41 rendered = ~25 ms, so FG + its CUDA path cost ~3-4 ms per rendered frame, normal for DLSS-G).
  - Translation is already the fast configuration (SSBO raw buffers, scalar layout, NV raw access chains, new
    structurizer); root push + descriptor buffers is required for RE9/RE8 RT (alternatives broken or CPU-bound). The top
    PT compute shaders (`7bbcc3d56fa7bc6c`, `4a76ec92b1acd9b4`, inline ray queries, 24k/17k SPIR-V lines) show no obvious
    Beer-added overhead (scalar function variables, a few small arrays, one memory-model qualifier).
  - **Resolved against a Windows reference (same card, YouTube benchmark: 1440p, DLSS Quality, PT, max, FG off: 60+
    indoors, 55+ outdoors):** Beer at the same settings, FG off: **56-59 indoors** (DLSS Balanced: 70). Indoors is within
    a few percent of Windows. Outdoors (the streaming area used all along): ~45 under Beer; no Windows number exists for
    that exact area, and PT cost rises sharply outdoors, so it is not a confirmed gap. The earlier "~1.6x" came from
    comparing against a 120-with-FG figure from a different scene. Note: the manager's RE9 settings were DLSS Balanced
    (render 1485x835), not Quality.
  - Still to confirm: FG 2x indoors should show ~110-120; the video's own outdoor spot for an outdoor comparison.
- **RE9 out-of-memory crash: per-thread block leak (fixed 2026-11-28).** RAM grew ~0.5 GB/min until the OOM killer
  ended the game (8.1-8.4 GB anon, 350 MB page tables, ~47k mappings; Proton reference ~10 GB total system RAM).
  Found with `BEER_MEM_PROF=1` (new: [MEM] line every 10 s: mappings, VirtualAlloc reservations, thread blocks, RSS
  split, Beer malloc, guest heap, D3D12 heaps, NVIDIA mappings) and an LD_PRELOAD mmap tracer
  (`/mnt/hdd2/beer-tools/mmaptrace/`): VirtualAlloc reservations stayed flat (~1,500) while 1-4 MB anonymous
  mappings created by the loader itself piled up. Cause: `alloc_teb_for_thread` maps a host call stack (1 MB), a
  signal stack (256 KB), the TEB, the implicit-TLS array and the EXE TLS data for every thread and never released
  them; `QueueUserWorkItem` starts a host thread per work item, and RE9 queues ~200 per second. Fix: per-thread blocks
  return to a pool at thread exit (pthread key destructor; DLL TLS blocks freed, TLS array removed from
  `g_tls_arrays`) and are reused after a 2 s quarantine. Result: mappings ~3,900 flat, page tables 34 MB, RSS ~5.9 GB
  flat, swap 0, 7,538 blocks reused 77k times in 6 min. `BEER_THREAD_BLOCK_POOL=0` restores the old behaviour.
  Follow-up: a real worker pool for QueueUserWorkItem (200 thread creations/s cost CPU and DLL_THREAD_ATTACH calls).
- **Launch note:** games started from a terminal share its systemd scope; an OOM kill then stops the terminal too
  (it took a Claude session down). `manager_launch.py` now runs the game in its own scope (`MEMCAP_GB`, default 11).
- **RE9 random GPU hang (open):** caught by the new GPU watchdog (default on; `BEER_GPU_WATCHDOG=0` off): the compute
  lane stopped with 49 submissions pending, direct lane waiting on it, Xid 109 CTX SWITCH TIMEOUT. Same shape as the
  2026-11-12 note (compute shader looping on bounds read from a buffer): likely a missing cross-queue dependency.
  Next: catch it with `BEER_VK_CHECKPOINTS=1` to name the shader.
- **RE9 path-tracing grain on hair, rain and smoke (2026-11-28): most likely the NVIDIA 610 driver, not Beer.**
  Symptom: with PT (+ Ray Reconstruction) grain on hair strands, rain and smoke, visible even in a still frame, at DLSS
  Quality and DLAA; RT High (no RR) clean; Windows clean. Ruled out by direct test: RR disocclusion mask
  (`BEER_NGX_NO_DISOCCLUSION=1`), frozen noise (frame-to-frame high-frequency correlation 0.09; hair 0.31 vs buildings
  0.27 = real detail), jitter/frame counter (normal Halton sequence), dropped passes (0 `[BD12 DROP]`, no failed PSOs),
  RR model (game ships DLSS 310.3; forced preset E via new `BEER_NGX_RR_PRESET=<n>`: same), RR created once with
  intact history, BLAS geometry/instance flags (identical bits, passed through), CUDA input views
  (`BEER_CUDA_DIAGNOSTICS=1`: handles, formats, depth-only aspect, layouts all correct), IEEE float controls
  (`BEER_SPV_FLOAT_CONTROLS=1`: same), aliasing discard (new `BEER_BD12_ALIAS_DISCARD=0`, vkd3d-proton behaviour:
  same, no new issues). RE9 passes RR no particle mask / transparency layer (game choice, same on all platforms).
  Timeline: game files unchanged since 2026-04-09; NVIDIA 595.84 (open) -> 610.57.04 on 2026-09-24; Proton looked
  right in July and now shows the same grain. Both Beer and Proton run RR's kernels through the driver's VK_NVX CUDA
  interop, so a 610 regression there fits. Not confirmed (driver downgrade not tested at the user's request; Proton
  also updated since July). Test if wanted: nvidia-driver-595-open 595.84, same spot.
  Tools: `BEER_NGX_DUMP=<trigger>` + `BEER_BD12_CAPTURE_DIR` (depth now skipped unless `BEER_NGX_DUMP_DEPTH=1`: its
  capture caused an Xid 31 copy-engine fault), `BEER_NGX_DIAGNOSTICS=1`.
- **Slow frames are heavy GPU frames (re9-mq24):** in frames > 30 ms the presenting thread uses ~6 ms CPU and waits ~31 ms,
  mostly on `0x60000252`/`0x60000221`; the fence it waits for reached the GPU 2.6 ms after the wait and the GPU then took
  23.5 ms (median; `0x252` GPU time median 9.8, p90 26 ms). Not a Beer lock, hold or CPU stall.
- **Per-frame GPU profile (re9-gpuprof27, `BEER_BD12_GPU_PROF=1 BEER_BD12_GPU_PROF_FRAME=20`):** the per-frame breakdown
  used to cap at 48 distinct operations and lump the rest as `other ffffffffffffff` (21-43 ms of each slow frame in
  re9-gpuprof26); now 512 tags, 8 printed, lump shown as `rest`. Window shares: AS builds 13.1%, dispatch 12.9%, CUDA
  (DLSS/RR) 11.6%, indirect dispatch 6.2%, draws < 1.5%. In slow frames: dispatch `bcc3d56fa7bc6c` ~5.8 ms, indirect
  dispatch `76ec92b1acd9b4` ~3.7 ms, and bursts of 40-80+ single-geometry BLAS full builds (`as-build 200001`, 3-11 ms).
  Caveat: the profiler's per-operation ALL_COMMANDS timestamps serialise the GPU, so small overlapping builds look more
  expensive than they are and absolute frame times are inflated (median "GPU frame" 22 ms here). Builds already run
  without Beer drains (`BEER_BD12_AS_BARRIERS`); one `vkCmdBuildAccelerationStructuresKHR` per build. Candidates: batch
  consecutive builds into one call (vkd3d-proton does), check whether `bcc3d56fa7bc6c` is slow because of translation.
- Process note: run 25 crashed because run 24 had not exited yet (two games, VRAM budget 1653 MiB). Check for a running
  `./loader` before each launch.
- **Status:** no behaviour change yet; all added tracing is gated by `BEER_FENCE_LATENCY` / `BEER_BD12_HITCH_SYSCALL`.

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
