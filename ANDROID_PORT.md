# Android port: handoff

Last updated 2026-10-30. Target device: iQOO 15 (Snapdragon 8 Elite Gen 5, Adreno 840).
Nothing here has been built or run as an APK yet.

## Direction (changed 2026-10-30)

**Native ARM64 host in an APK**, with only the game's x86-64 code emulated.

- Loader, Win32 layer, BD11/BD12, shader translators, audio/media are built for
  Android (NDK, bionic, aarch64) and run natively. A guest-CPU emulator (Box64 or
  FEX, embedded as a library) executes only guest code.
- Uses the phone's own Qualcomm Vulkan driver through the NDK. No glibc rootfs, no
  proot, no X server, no Turnip, no targetSdk-28 restriction (everything ships as
  `.so` inside the APK).
- **No exe-specific patches or bypasses are carried into the port.** The old ones
  are to be removed on the desktop once the PC is back (list below).
- The Box64-whole-loader-in-proot route (Turnip + Termux:X11) is **paused**; it only
  ever served as a stepping stone. Notes for resuming it are in the appendix.

No Android SDK/NDK and no main PC are available right now. Termux SSH is available
as an on-device bionic aarch64 compiler (`pkg install clang`) if compile checks are
needed before an NDK is set up.

## Verified so far (baseline from the Box64 experiments)

Unmodified x86-64 `loader` under Box64 0.4.5 on the phone (proot Ubuntu):

| Test | Result |
|---|---|
| `loader` with no args | starts, prints usage |
| `tools/hello.exe` (GetStdHandle/WriteFile/ExitProcess) | PE at 0x140000000, KUSER at 0x7ffe0000, TEB/GS, import trampolines, exit all work |
| `tools/crash.exe` (write to address 0) | SIGSEGV handler sees the same RIP/fault address as native x86 |
| `tools/veh.exe` (guest vectored handler skips a faulting write, then resumes) | passes natively on desktop; **not yet run under an emulator** |

This shows the loader's execution model survives emulation and that the existing
guest tests are usable as emulator-spike tests. Android reports a 39-bit address
space; fine for the 0x140000000 image base.

## Done in the repo (desktop behaviour unchanged)

All checked by compiling with the project flags; nothing was run on Android. The
desktop `loader` was not rebuilt/run against games (no PC, no game files).

- `platform/beer_arch.h`: `BEER_HOST_X86_64`, `BEER_GUEST_NATIVE`,
  `BEER_HAVE_GLIBC_DIAG`; `beer_guest_processor_features()` (one source for
  `IsProcessorFeaturePresent` and `KUSER_SHARED_DATA`; ARM gets a conservative SSE4.2
  baseline to tune to the emulator); `beer_mmap_low32_rwx()` (replaces `MAP_32BIT`).
- **BCrypt without OpenSSL, for Android only.** `platform/win32_bcrypt_portable.c` + `platform/beer_hash.{c,h}`
  implement MD5/SHA-1/SHA-2/HMAC with no external library (OpenSSL is not in the NDK). The **desktop build
  keeps the original OpenSSL `platform/win32_bcrypt.c`** (Makefile unchanged, still links `-lcrypto`), because the
  portable code is slower: 3-6x slower than OpenSSL at -O2 and 10-25x at the Makefile's -O0
  (SHA-256: ~20 MB/s at -O0, ~87 MB/s at -O2, ~554 MB/s OpenSSL on the dev PC). Both give identical output over
  834 lines of BCrypt API calls, and `platform/beer_hash_test.c` (75 hash + 40 HMAC cases, clean under
  ASan/UBSan) is in `make test-input`. The Android build should compile `beer_hash.c` with optimisation.
- **Bug fixed in passing:** `pseudo_handle()` only covered `0x81..0xF1`, so
  `BCryptHash(BCRYPT_SHA256_ALG_HANDLE, ...)` (pseudo handles `0x21..0x61`) dereferenced
  address 0x41 and crashed; those switch cases were unreachable. Range is now `0x11..0xF1` (fixed in both
  the desktop and the portable file; the only change to the desktop BCrypt file).
- glibc-only diagnostics compiled out under `BEER_HAVE_GLIBC_DIAG=0` (bionic has no
  `<execinfo.h>` before API 33; the sampler reads x86 registers): `BEER_BD12_SAMPLE` in
  `graphics/d3d12v/bd12_device.c`, the cache trace in `renderer/vulkan/vulkan_graphics.c`.
  Both branches compile cleanly.
- `renderer/vulkan/vulkan_context.c`: falls back to `dlopen("libvulkan.so")` (Android's name).
- Tools: `tools/pack_for_box64.sh`, `tools/make_test_pe.py` (`--crash`), `tools/hello.exe`,
  `tools/crash.exe`, `tools/veh.exe` (`--veh`), `tools/vk_probe(.c)`, plus `*.gz.b64` text forms.

Not verifiable here (no PC, no game files): any run of RE8/Sekiro/PRAGMATA. A full desktop link needs the `third_party/dxil-spirv` checkout
(missing on this machine). A build of everything else links with only the three
`beer_d3d12_dxil_*` symbols unresolved, and no object references OpenSSL.

## Roadmap

1. **Android build scaffolding (next).** CMake + Gradle project (NDK, minSdk 28 for
   `getrandom`/`memfd_create`), compiling the portable sources into a shared library;
   NativeActivity or GameActivity shell. Needs an NDK, or compile checks over Termux SSH.
2. **Guest-CPU spike (the real risk).** See "Emulator research" below. Run `hello.exe`,
   `crash.exe`, `veh.exe` through an embedded emulator inside a native ARM64 loader.
3. **`GuestCPU` interface** (enter guest, call guest function, import dispatch,
   capture/restore context, per-thread TEB/GS) with a native-x86 backend wrapping
   today's code so desktop is unaffected, then the emulator backend.
4. **Exception layer** rewritten against the emulator's context (see audit).
5. **Android backends** (below), then Vulkan feature check on Adreno 840, then performance.

## Android build inventory

Portable already (compile as-is, modulo the items above): `platform/` (except
`xwayland_backend.c`), `graphics/d3d11`, `d3d11v`, `d3d12`, `d3d12v`, `renderer/vulkan`,
`audio/wasapi_compat.c`, `media/`.

To write:
- **Window/input backend** with the same API as `platform/xwayland_backend.h`
  (events, present, cursor, fullscreen...) over `ANativeWindow` + `AInputQueue`/touch/gamepad.
- **Vulkan surface** in `renderer/vulkan/vulkan_presenter.c`: `VK_KHR_android_surface`
  instead of `VK_KHR_xlib_surface` (also requested in `vulkan_context.c`).
- **Audio** in `audio/audio_backend.c`: it dlopens `libpulse-simple.so.0`; needs AAudio.
- **Storage**: game files via SAF or a user-picked folder; prefix/save directories under app storage.

To build for Android and ship in the APK:
- `libdxil-spirv-c-shared.so` (from `third_party/dxil-spirv`, C++, dlopen'd by
  `graphics/d3d12/d3d12_dxil_translator.c`).
- `libvkd3d-shader` (dlopen'd by `graphics/d3d12/d3d12_shader_translator.c`; override
  with `BEER_VKD3D_SHADER_LIBRARY`).
- zlib and libvulkan come from the NDK.

Still glibc-specific in `loader.c`: `<execinfo.h>` (`backtrace` at ~L20708, ~L23021),
`<ucontext.h>` x86 register use, `arch_prctl`. These are the guest-CPU layer's job.

## x86-specific code in `loader.c` (the guest-CPU layer)

Line numbers are approximate (pre-edit; recent edits shifted some by a few lines).

| Area | Where | Needs |
|---|---|---|
| Import trampoline (guest -> host, stack switch) | `beer_dispatch_trampoline` ~L1161 | guest-CPU backend |
| Entry into the PE | `asm volatile` near the end of the main flow | guest-CPU backend |
| Host/guest stack bookkeeping in `%fs:` TLS | ~L13511, L18177-18182, L20325 | guest-CPU backend |
| `RtlCaptureContext` / `RtlUnwind*` / `RtlRestoreContext` / `RaiseException` | ~L3296, L20215-20302 | guest-CPU backend |
| TEB/PEB via GS | `arch_prctl` call sites | guest-CPU backend |
| Fault/SEH/VEH recovery from `ucontext` | `on_crash_impl` ~L20641-22976 | rewrite against emulator context |

## Exe-specific code to remove (when the PC is back)

None of this is carried into the Android build. Plan: add a counter to each path,
run RE8/Sekiro/PRAGMATA on the desktop, delete what never fires, then re-test.

- Opt-in code patches: `loader.c` ~L19290-19530, gated by `BEER_ENABLE_KNOWN_TARGET_PATCHES` /
  `BEER_ENABLE_HARD_PATCHES` (NOPs/stubs at fixed RVAs such as `0x3dd7570`, `0x2359f48`,
  `0x235a82c`, `0x1a6652e`). Off by default.
- ~40 always-on RVA crash recoveries in `on_crash_impl` (fault RIP at fixed RVA -> jump/skip),
  including a Sekiro-labelled SIGILL handler (~L21356-21380). Gated by fault address, not
  by which exe is loaded.
- `BEER_SEKIRO_AUDIO_UNMUTE` (~L14836) and the FMOD Ex stub block written for Sekiro
  (~L14400-17100; this is API emulation, review rather than delete).
- RE8 frame-clock XMM9 workaround (~L1197).
- Comments naming Sekiro/RE8 elsewhere (~50 in `loader.c`) mostly document generic
  Win32 behaviour and can stay.

## Spike results (2026-10-30, on the phone via Termux SSH)

Code and reproduction steps: `android/spike/` (README.md, `fex-bionic.patch`).

- **Platform probe** (executable memory, fixed mappings at 0x140000000 / 0x7ffe0000 / 0x10000,
  128 GiB PROT_NONE reservations, RWX below 4 GiB, SIGSEGV with fault address and PC, memfd_create,
  getrandom): all work in Termux's app sandbox on Android 16.
- **FEXCore builds on bionic.** FEX's CMake rejects Android by name only; with `CMAKE_SYSTEM_NAME=Linux`
  and an API-30 target, 58 objects compile with 0 errors. Two bionic gaps needed shims (`getdents64`, `valloc`).
- **spike0 passes:** `mov eax,42 ; hlt` runs through FEXCore in a native ARM64 bionic process with
  our own ~100-line embedder (no Linux emulation frontend). Needed: a 64-byte-aligned TLS segment
  (bionic rule) and a thread descriptor table with a long-mode code segment (copied from FEX's ARM64EC module).
- **spike1 passes:** guest->host calls through the `syscall` instruction (Windows-style
  `mov r10,rcx ; mov eax,N ; syscall`), arguments and results in registers, host reading guest memory,
  TEB through `gs:[0x30]`. Handler must set `State.rip = RCX` to resume (FEXCore models real syscall semantics).
- **spike2 passes:** a guest null write faults inside FEX's JIT; a plain SIGSEGV handler rebuilds the full
  guest context (exact faulting RIP, GPRs, XMM0, EFLAGS) from the host `ucontext`, then resumes the guest after
  the faulting instruction. This is the core of the Windows exception layer (fault -> CONTEXT -> VEH/SEH ->
  resume). Recipe in `android/spike/README.md`.
- **spike3 passes:** nested host->guest calls from inside an import handler (six Win64 args, two on the stack),
  guest state preserved. Also found: each guest thread needs a FEX call/return stack (`State.callret_sp`), and
  host->guest callbacks return through FEX's `0F 3E` CALLBACKRET stub (not `hlt`). Details in `android/spike/README.md`.
- **Not yet shown:** YMM upper halves/x87/MXCSR in the context, self-modifying-code tracking, threads,
  nested faults, performance, running inside an APK (SELinux domain differs slightly from Termux).

## GuestCPU layer: progress (2026-10-30)

`platform/guest_cpu.h` is the seam between the loader and whatever runs the guest's x86-64 code.
On a native x86-64 host it maps to exactly what `loader.c` did before (same syscalls, same types, same asm);
on any other host it provides an x86-shaped context type (`beer_guest_ucontext_t`, glibc `REG_*` indices) and
declares the functions an emulator backend must implement:

| Backend function | Replaces |
|---|---|
| `beer_guest_get_gs_base` / `beer_guest_set_gs_base` | `arch_prctl(ARCH_GET_GS/SET_GS)` (13 call sites, now converted) |
| `beer_guest_call(fn, args, n)` | host -> guest calls (window procs, timers, thread start...), `call_guest_nested` |
| `beer_guest_enter(entry, stack, tls_runner, run_tls)` | the inline-asm jump into the exe's entry point |
| `beer_guest_caller_regs(uc)` | the naked `RtlCaptureContext` / `RtlUnwind*` / `RaiseException` register capture |
| `beer_guest_resume(regs)` | `beer_restore_context_asm` (RtlRestoreContext, SEH unwind) |

The crash/exception code (`on_crash_impl`, VEH/SEH dispatch, `beer_context_to/from_ucontext`) is unchanged: it now
takes a `beer_guest_ucontext_t`, which natively IS the host `ucontext_t`. Under FEX the backend fills one from the
emulator's state (recipe: `android/spike/README.md`, spike2), runs the same logic, and copies it back.

**Measured with `android/check_aarch64.sh` (clang, aarch64, Android API 30, on the phone):** all 55 sources compile with
0 errors (was 852, 832 of them in `loader.c`). `android/build_arm.sh` then links every object; the only undefined symbols
were the 11 the backend had to provide.

## First end-to-end run (2026-10-30): native ARM64 loader + FEXCore, on the phone

`android/guestcpu_fex.cpp` implements the `guest_cpu.h` contract on FEXCore. Built with `android/build_beer_arm.sh`
(needs the FEXCore build from `android/spike/README.md`), it produces `obj/beer_arm`: the whole Beer loader as an
ARM64 bionic executable, with only the guest's x86-64 code emulated.

| Test (in Termux, no GPU needed) | Result |
|---|---|
| `hello.exe` (GetStdHandle/WriteFile/ExitProcess) | prints `hello from a guest exe`, exit 0; PE mapping, imports, TEB/PEB, D3D11 init all native |
| `crash.exe` (write to address 0) | `SIGSEGV #1 at RIP=0x140001035 FA=0x0` and the same FATAL message and exit code as the x86 build |
| `veh.exe` (guest vectored handler skips the faulting write) | `before fault` / `recovered after fault`, exit 0: fault -> rebuilt context -> loader logic -> guest handler (nested host->guest call) -> resume |
| `com.exe` (imports `D3D11CreateDevice`, calls `AddRef`/`Release` through the device's vtable) | `ExitProcess(770)` = 0x302, identical to the native x86 run; the guest called **host-built COM methods** and the loader created a real BD11/BD12 Vulkan device on the phone (Termux's software `llvmpipe` driver; an APK would use the Adreno driver) |

How the pieces work (all in `guestcpu_fex.cpp`, details in `android/spike/README.md`):
- Imports are `mov r10,rcx ; mov eax,<id> ; syscall ; ret` thunks (`emit_syscall_thunk` in `loader.c`). `HandleSyscall` passes the
  argument registers and 12 stack qwords to `beer_guest_import_dispatch`, which calls the same `g_impl_targets[]` table
  the native trampoline used, then resumes at RCX.
- A guest fault is a host signal inside FEX's JIT. The handler rebuilds the guest context, stashes it, and redirects the guest to a
  stub that "calls" a reserved import; the loader's existing crash/VEH/SEH code then runs as an ordinary host frame
  (where nested guest calls are legal) and the resulting context is applied back.
- Host -> guest calls (`BEER_GUEST_CALL`, `beer_guest_call`) save the whole guest state, build a Win64 frame, run the callee with
  `HandleCallback`, and restore the state. `RtlRestoreContext` / SEH unwinds use `beer_guest_resume` (a `longjmp` to the import's frame).
- `BEER_GUEST_CALL(fn, args...)` is a plain direct call on x86 and goes through the backend otherwise. It is applied to the four
  exception-layer call sites (vectored handlers, function-table callbacks, SEH and unwind language handlers). Float/double
  arguments are rejected at compile time.

Desktop safety of everything above: loader output and exit codes for the three exes match the prebuilt loader, warnings unchanged
(705), identical assembly where checked, platform unit tests unchanged (`win32_unwind_test` was already failing on this toolchain).

**Guest calls into host-built COM objects (vtables) need no thunks.** The loader's own code is declared non-executable for the
guest (`QueryGuestExecutableRange`); a guest `call`/`jmp` to a host function address (any vtable entry, ~48 arrays / ~900 slots) makes FEX
raise a guest fault, which the backend turns into a host call (Win64 args from the guest registers, result in RAX, return address
popped, FEX's call/return stack entry popped). Measured at ~1.4 us per call (`android/spike/spike4.cpp`), so everything works first and
hot vtables can later get faster `syscall` thunks.

**Host -> guest calls:** `BEER_GUEST_CALL` is applied to all 48 call sites in `loader.c` that go through typed `ms_abi` pointers
(window procedures, hooks, timers, thread-pool callbacks, DLL entry/TLS callbacks, enumeration callbacks, FMOD/Bink forwarding)
plus the four in the exception layer. The macro now rejects float/double arguments with a compile error at the call site
(a first version silently converted them; fixed). The one float call site (FMOD `setVolume`) uses `beer_guest_call_f`, which
puts the float in its XMM register. x86 assembly for `loader.c` is identical before and after these conversions.

## Real games on the phone (2026-10-30, Termux, native ARM64 loader + FEXCore)

RE8 (`Download/Games/Resident Evil - Village8/re8.exe`, the game this loader is best tuned for) boots through DLL initialisation,
creates its D3D12 device and DirectInput devices, starts Media Foundation, and runs its render loop for the whole test window
(170 s, killed by the timeout) with **0 guest crashes, 0 host signals, 0 device-lost events**, on the **Adreno 840** through
Android's own Vulkan driver. Run it with `BEER_VULKAN_LIBRARY=/system/lib64/libvulkan.so` (see below).
What stops it rendering is only the missing shader translator: `libvkd3d-shader` / `dxil-spirv` are not built for Android, so
pipelines "failed to compile" and draws are dropped.
(Sekiro also runs 150 s without crashing, but it barely launches even on x86, so it is not a useful signal.)

What it took, beyond the earlier spikes (all in `android/guestcpu_fex.cpp` unless noted):
- **Heap pointer tagging.** bionic's allocator returns `malloc` pointers with a tag in the top byte (`0xb4...`). The ARM hardware
  ignores it, but guest x86-64 code sees a non-canonical address (and the loader rejects it: "InitializeCriticalSection* invalid ptr").
  `mallopt(M_HEAP_TAGGING_LEVEL, 0)` runs from a constructor. An APK also needs `android:allowNativeHeapPointerTagging="false"`.
- **Stack bounds.** `__chkstk` probes pages downward from `TEB.StackLimit`. Worker threads got the *pthread's* stack bounds while running
  on the backend's guest stack, so the probe walked unmapped memory. `beer_guest_thread_stack()` now supplies each thread's guest stack
  and `alloc_teb_for_thread` publishes it.
- **Guest threads / top-level calls.** `beer_guest_call` with no guest code active runs on a per-thread 8 MiB guest stack through a
  small guest stub (`call rax` then a reserved "thread done" import that unwinds back). `tools/thread.exe` passes.
- **Calls into guest code.** `BEER_GUEST_CALL` now takes up to 16 arguments (Oodle decompress passes 14). Sites found by hand were
  not enough (a global pointer to a guest DLL function, `(*first)()` loops in `_initterm`); `android/find_indirect_calls.py` reads a
  clang AST dump and lists *every* call through a function pointer. After the fixes, the only indirect calls left in `loader.c` and
  the other sources are host-to-host (Vulkan/X11 entry points, hooks the loader registers with itself).
- **Floats on the guest -> host side.** `BEER_HOST_FLOAT_FN(fn, mask)` registers host functions that take floats (6 known). The loader's
  `beer_guest_host_call` moves the floats into d0.. and compacts the integer arguments; checked on ARM64 with a standalone test.
- **Vulkan on Android.** `VK_KHR_xlib_surface` does not exist there, so instance extensions are now enumerated and only the available ones
  are requested (silent and identical on desktop). `BEER_VULKAN_LIBRARY` selects a specific Vulkan loader; unset, behaviour is unchanged.
  In Termux the default `libvulkan.so.1` is Mesa's software `lavapipe`, which crashes under RE8's workload; the system loader uses Adreno.
- Diagnostics: a host signal outside guest code now prints the pc, the caller and a frame-pointer chain (resolve with
  `llvm-symbolizer -e obj/beer_arm`).

- **Audio without a sound server.** The audio backend only knew PulseAudio; on Android `dlopen` failed, WASAPI initialisation failed
  and RE8 retried forever. `audio/audio_backend.c` now falls back to a **silent timed sink** (PCM is accepted and discarded at the
  device's real-time rate, so the game's audio threads behave normally). `BEER_AUDIO_NULL=1` forces it. Desktop with Pulse is unchanged.
  An AAudio backend replaces it later.
- **Display from Termux.** With Termux:X11 running (`termux-x11 :0`), `DISPLAY=:0 BEER_VULKAN_LIBRARY=/system/lib64/libvulkan.so
  beer_arm --presenter=x11 re8.exe` creates a real X window ("Resident Evil Village", 1920x1080) and the game enters its render loop.
  Frames cannot be meaningful until shaders translate (every draw is dropped: ~25k draws and ~45k dispatches per second were dropped
  in the 90 s run). Process time was ~1m28 user + ~1m52 system over 90 s wall across threads: the high system time is the universal
  host-call path (one signal per guest call into the loader), the first performance item to attack with `syscall` thunks.

**Remaining, in rough order:**
0. **How frames reach a screen from Termux:** they do not yet. BD11/BD12 render on the GPU, but Android's Vulkan has no X11 surface and
   a Termux process has no `ANativeWindow`. For testing, the loader's CPU presenter (`--presenter=x11`: read the image back, `XPutImage`
   through `libX11.so.6`) works with the Termux:X11 app (APK in `Download/`): install `x11-repo` + `libx11` + `termux-x11-nightly`,
   run `termux-x11 :0`, then `DISPLAY=:0 beer_arm --presenter=x11 ...`. In the APK the presenter becomes `VK_KHR_android_surface`.
1. **Shader translators for Android** (not started: the build was blocked, see below). Findings from reading the sources:
   `libvkd3d-shader` is Wine's `vkd3d` (https://gitlab.winehq.org/wine/vkd3d, LGPL-2.1, autotools). Its `configure.ac` has no mandatory
   third-party libraries (ncurses/OpenGL/SPIRV-Tools/XCB are optional; use `--disable-tests --without-ncurses --without-opengl
   --without-xcb --without-spirv-tools`); it needs autoconf/automake/libtool/flex/bison to generate its parsers. The loader calls only
   `vkd3d_shader_compile/scan` and the free functions. LGPL-2.1: ship it as a separate `.so` and provide its source.
   `dxil-spirv` (https://github.com/HansKristian-Work/dxil-spirv, MIT, CMake, needs submodules) is the DXIL (SM6) path; RE8 needs both.
   Original item: build `libvkd3d-shader` (DXBC) and `libdxil-spirv-c-shared` (DXIL, `third_party/dxil-spirv`) with
   the NDK or in Termux, put them where `d3d12_shader_translator.c` / `d3d12_dxil_translator.c` look (or set `BEER_VKD3D_SHADER_LIBRARY`).
   Nothing renders without them.
2. **Display, input, audio, storage:** the presenter needs `VK_KHR_android_surface` and a window (an `ANativeWindow`); input events from
   Android; an AAudio audio backend (the current one dlopens PulseAudio); game files via a user-picked folder; the loader's control
   socket and other `/tmp` paths need an app-private temp directory.
3. **The APK shell** (targetSdk, manifest with tagging off, `jniLibs`), then performance work (the universal host-call path is ~1.4 us per
   call; hot vtables can get `syscall` thunks, and FEX options such as TSO and SMC handling are at conservative defaults).
4. Not yet covered: self-modifying code (`SMCChecks` is off), YMM upper halves in contexts, float *return* values from host functions,
   `BEER_EXECUTABLE_TLS` at entry, `<execinfo.h>`/`backtrace` for an API-30 NDK build.

## Shader translators and temp paths (2026-10-30)

- `libvkd3d-shader` (DXBC) built on the phone, no root needed; load with
  `BEER_VKD3D_SHADER_LIBRARY=$HOME/beer-spike/lib/libvkd3d-shader.so.1`. Recipe:
  `pkg install autoconf automake libtool flex bison pkg-config make spirv-headers`,
  `./autogen.sh`, a local `JSON.pm` shim (re-exports core `JSON::PP`, `PERL5LIB`),
  configure in `build/` with `--disable-tests --disable-demos --without-ncurses
  --without-opengl --without-xcb --without-spirv-tools --disable-static`, generate
  `include/vkd3d_d3dcommon.h` / `vkd3d_d3dx9shader.h` (no widl on Android: small
  python stand-in for enums/structs), `make include/private/spirv_grammar.h
  include/private/vkd3d_version.h`, drop `libs/vkd3d-common/blob.lo` from the
  generated Makefile, `make libvkd3d-shader.la`. With it loaded RE8's BD12
  "pipeline failed / draw dropped" messages stop. LGPL-2.1: ship as a separate .so.
- `dxil-spirv` (SM6/DXIL) built on the phone (`~/beer-spike/dxil-spirv`, commit a7676a1, `git clone --recursive`,
  `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release`, `ninja -C build dxil-spirv-c-shared`; no patches needed).
  `android/build_arm.sh` builds the real `d3d12_dxil_translator.c` when that checkout exists (else the stub). Load with
  `BEER_DXIL_SPIRV_LIBRARY=$HOME/beer-spike/dxil-spirv/build/libdxil-spirv-c-shared.so`. In 100 s of RE8 (logo stage) no DXIL
  shader was requested yet, so the translator itself is untested on a real game shader.
- Hard-coded `/tmp` in MF, control socket and NT section fallback now goes through
  `platform/beer_paths.h` `beer_tmp_dir()` (`BEER_TMPDIR`, Android `TMPDIR`, else
  `/tmp`; desktop unchanged). This fixed RE8 spinning on `ReadSample -> EOS`
  (ffmpeg decode needs a writable temp dir). Debug-only `/tmp` dumps left as is.
- Audio: silent timed sink when libpulse-simple is missing or `BEER_AUDIO_NULL=1`;
  AAudio backend still to write.
- Desktop regression after these edits: 5 test exes give identical output and exit
  codes (only the host backtrace path differs).

## Debugging an emulated guest (no gdb needed)

- `BEER_THREAD_DUMP=1` then `kill -USR2 <pid>`: one line per guest thread with its last import, whether it is still inside
  it (`IN`) or has returned (`after`), the guest return address and a running call count. Dump twice and diff the counts
  to see which threads spin and which are blocked.
- `BEER_API_TRACE=1` (+ `BEER_API_TRACE_FILTER=name,name`) now works under emulation too (logs from the import dispatcher).
- Other existing switches that work unchanged: `BEER_BD12_FPS`, `BEER_BD12_PROF`, `BEER_BD12_PSO_LOG`, `BEER_BD12_TRACE=<frame>`
  (per-frame draw/dispatch list), `BEER_FILE_DIAGNOSTICS`, `BEER_MF_DIAGNOSTICS`, `BEER_WAIT_DIAGNOSTICS`, `BEER_EVENT_DIAGNOSTICS`.
- Screen grab on the phone: `pkg install imagemagick`, then `DISPLAY=:0 import -window root shot.png`.
- **Display (fixed):** `--presenter=x11` turns the Vulkan swapchain presenter off, so D3D12 frames were never shown (only
  the movie path, which is why just the logo ever appeared). `loader.c` now reads the back buffer back
  (`vulkan_readback_image_rgba8` in `vulkan_presenter.c`, no swapchain needed) and presents it via
  `xwayland_window_present_rgba8` when `xwayland_window_cpu_presenter()`. RE8's "Health Warning" splash now shows.
  Cost: a full-frame readback + hash per present (about 10-15 fps at 1080p on the phone); the real fix is the
  ANativeWindow presenter. Also new: `BEER_BD12_TRACE=<frame>` now logs draws (`[TRACE] draw rtv= dsv= viewport=`),
  `BEER_BD12_CAPTURE_RT=1 BEER_BD12_CAPTURE_DIR=<dir>` dumps render targets as PPM, `BEER_BD12_FORCE_PS_MAGENTA=1`
  proves rasterization.
- Seen on the screenshot: the window is placed at the game's requested 544,0 / 1936x1118 on a 3024x949 Termux:X11 screen,
  so the picture is offset and clipped; window placement on Android is still to do.
- RE8 status (before the display fix): window created, logo movie decodes (ffmpeg, 150 frames) and shows, then the game runs
  ~70 fps of D3D12 frames that contain only compute dispatches and no draws (black screen); ~100k waits/s on two job
  threads; file reads stop early (64 overlapped reads total). Not yet known whether this is slow loading under emulation
  or a stall. `dxil-spirv` was never asked for a shader in this phase.

## The APK (2026-10-30)

First working APK: RE8 reaches its title-screen background, full screen, no X11.
- Build host: this PC. SDK/NDK in `../android-sdk` (NDK 29, build-tools 35), sources of FEX and dxil-spirv in
  `../android-deps` (FEX pinned to the commit in `android/spike/fex-base-commit.txt` + `fex-bionic.patch`, built into
  `FEX/build-ndk`; `dxil-spirv/build-ndk` with `-DCMAKE_SHARED_LINKER_FLAGS=-llog`). NDK 27 is too old for FEX (`std::atomic_ref`).
- `android/apk/build_native.sh` -> `out/lib/arm64-v8a/libbeer.so`; `android/apk/build_apk.sh` -> signed `out/beer.apk`
  (debug key `../android-deps/beer-debug.jks`). Bundles `libvkd3d-shader.so` (prebuilt on the phone, in `android/apk/prebuilt/`)
  and `libdxil-spirv-c-shared.so`. `NativeActivity`, no Java: `AndroidManifest.xml` (allowNativeHeapPointerTagging=false, all-files access).
- `android/android_backend.c` implements `xwayland_window_*` on the ANativeWindow: touch -> left click, hardware keys -> X keycodes,
  Vulkan swapchain presenter (blit from the D3D12 back buffer). `android/android_support.c`: `backtrace` for API < 33.
- Game selection: `Download/Beer/launch.txt` (line 1 game dir, line 2 exe, more lines = loader args). The app asks for all-files access.
- Debugging: wireless adb (`adb pair`/`adb connect`), `adb logcat -s BeerLog Beer`; the loader's stderr goes to logcat tag `BeerLog`.
- Bugs found on the way: the guest stack guard page used `MAP_FIXED` and overwrote Android-runtime memory (now `MAP_FIXED_NOREPLACE`);
  HOME/prefix must be the app's internal dir; `SUBOPTIMAL` from present is not a reason to recreate the swapchain.
- Still missing in the APK: movie decoding (the loader shells out to `ffmpeg`; use AMediaExtractor/AMediaCodec), real touch/gamepad controls,
  audio (AAudio), a game picker UI.

## Resuming on another machine (state at the pause, 2026-10-30)

**Nothing is committed.** All work is uncommitted on branch `apk` (new files under `android/`, `platform/`, `tools/`, plus edits to
`loader.c` and others). Bring it with a commit (`git add -A; git commit` on `apk`, then push or `git bundle`), or copy the working tree.
Do not copy `beer-box64-pack*`, `android/apk/out/`, `.tmp_*`. Keep `android/apk/prebuilt/libvkd3d-shader.so` (8.7 MB, built on the phone,
LGPL-2.1: ship with source).

**Rebuild the toolchain (about 2 GB, none of it is in the repo)** next to the project folder:
1. Android command-line tools: `https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip` into
   `../android-sdk/cmdline-tools/latest`, then
   `sdkmanager --sdk_root=../android-sdk "platform-tools" "platforms;android-35" "build-tools;35.0.0" "ndk;29.0.14206865" "cmake;3.22.1"`
   (JDK 17+ needed; NDK 27 is too old for FEX).
2. `../android-deps/FEX`: clone FEX-Emu/FEX, checkout the commit in `android/spike/fex-base-commit.txt`, init the submodules listed in
   `android/spike/README.md`, `git apply android/spike/fex-bionic.patch`, configure into `build-ndk` with the NDK clang
   (`CC/CXX=<ndk>/toolchains/llvm/prebuilt/linux-x86_64/bin/clang[++]`, flags in the README) and `ninja FEXCore FEXCore_Base fmt JemallocDummy`.
3. `../android-deps/dxil-spirv`: clone HansKristian-Work/dxil-spirv at `a7676a1f`, `--recursive`, configure with the NDK toolchain file
   (`-DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 -DANDROID_STL=c++_static -DCMAKE_SHARED_LINKER_FLAGS=-llog`), build `dxil-spirv-c-shared`.
4. `android/apk/build_native.sh` then `android/apk/build_apk.sh` -> `android/apk/out/beer.apk` (debug-signed; the keystore is created
   on first use). Install with `adb install -r`. Phone: Settings > Developer options > Wireless debugging, `adb pair` then `adb connect`.
5. Put `Download/Beer/launch.txt` on the phone: line 1 game directory, line 2 exe, then `env:NAME=value` lines for loader debug options.

**Where the work stopped (RE8, in the APK):** runs, shows the title background full screen at 60 fps, but
- the scene geometry (scarecrow, trees), logo and menu are missing: ~824 draws + 639 ExecuteIndirect per frame are recorded and none dropped, yet the
  depth buffers stay zero. Suspect the GPU-driven culling/ExecuteIndirect counts or mesh data. The desktop already has a culling fix
  (see `beer-loader-progress.md`, "GPU occlusion culling by proxy rendering", `BEER_BD12_PROXY_ALWAYS`; the user's other PC may have a newer
  one that is not in this tree: bring it over first and retest before debugging further);
- one frame in three lacks the UI layer (flicker); it disappears with `BEER_BD12_SYNC_ALL=1 BEER_BD12_SYNC_SUBMIT=1`, so look for a missing sync;
- movies loop on EOS (no `ffmpeg` in an app: use AMediaExtractor/AMediaCodec), touch is only a left click, no gamepad, no audio, no game picker.

**Cleanup when finished:** remove the `claude-session-beerloader` line from the phone's Termux `~/.ssh/authorized_keys`, and turn off
Wireless debugging on the phone.

## Emulator research (2026-10-30, from reading the sources; not run)

Neither project ships a drop-in "emulator as a library" for our case; what follows is
what would have to be extracted or written. Claims marked (unverified) were not checked
against a running build.

**FEXCore** (MIT; `FEX-Emu/FEX`) - recommended candidate.
- A real C++ library with a public interface: `FEXCore::Context::Context`
  (`FEXCore/include/FEXCore/Core/Context.h`). FEX's own Windows ARM64EC/WOW64 modules
  (`Source/Windows/`) already embed it outside the Linux frontend, so a non-Linux host is a precedent.
- Per-thread `InternalThreadState` via `CreateThread`; full register state in
  `CpuStateFrame` (`gregs`, `rip`, `gs_cached`/`fs_cached`, flags, XMM); run with `ExecuteThread`;
  `EnableExitOnHLT()` leaves the JIT cleanly (described as a test aid).
- Host-call hook candidate: guest `mov eax,N; syscall; ret` as the import thunk, dispatched in
  `SyscallHandler::HandleSyscall(CpuStateFrame*)` with the full frame (unverified outside Linux emulation).
- We would own the SIGSEGV handler (`SignalDelegator`) and rebuild a Win64 CONTEXT using
  `RestoreRIPFromHostPC`, `ReconstructCompactedEFLAGS`, `ReconstructXMMRegisters`.
- Guest address == host address. `64BitAllocator` reserves a 48-bit space (switch
  `ENABLE_FEX_ALLOCATOR`); 39-bit handling not traced.
- **No Android/bionic support found at all**; AArch64 JIT only, no interpreter; needs recent clang/libc++.

**Box64** (MIT; `ptitSeb/box64`) - fallback.
- Has `ANDROID`/`TERMUX`/`WINLATOR_GLIBC` build options and the only **interpreter**
  (`Run()`), useful for debugging and for devices that block JIT. Builds a standalone
  process runner, not a `.so`; process-wide globals (`box64_context`) and its own signal
  machinery fight a host program's handlers.
- Hooks: a "bridge" (`CC 'S' 'C' <wrapper> <fn>` via `AddBridge`) maps well onto import thunks
  (unverified without `box64_context` initialisation). Its WowBox64 DLL is 32-bit x86 and Wine-bound.

**Risks:** (1) FEXCore on bionic: untested toolchain/ABI (`preserve_all`), JIT memory under
Android SELinux; (2) faithful fault context (flags, XMM) and self-modifying-code detection vs our
own mmap/mprotect use; (3) 39-bit address space vs allocator/JIT placement and our fixed regions
(0x140000000, 0x7ffe0000).

**Smallest proof of concept:** a native aarch64 host program that maps a tiny PE, builds a TEB
with GS base, creates a FEXCore context/thread, runs `hello.exe` with imports as `syscall` thunks,
then `crash.exe` and `veh.exe`. Needs an aarch64 build environment (NDK or Termux `clang`).

## Risks / unknowns

- Embedding an x86-64 emulator as a library in our process is unproven; Box64/FEX are
  built around running Linux programs. This is the first thing to spike.
- Adreno 840 Vulkan features vs what BD12 needs (descriptor indexing, timeline semaphores...): unchecked.
- Performance unknown; the desktop is well optimised, guest code will still be emulated.
- 16 KB-page devices would complicate guest `VirtualProtect` (guest pages are 4 KB); this phone reports 4096.
- Without desktop testing, everything above is compile-checked only.

## Needs a PC

- Rebuild `loader` and re-run RE8/Sekiro/PRAGMATA after the changes above.
- Count which RVA recoveries still fire; remove the dead ones.
- Build with an NDK, build `dxil-spirv` and `vkd3d-shader` for Android.

## Appendix: paused Box64 + Turnip route

Kept so it can be resumed. Everything runs in a proot Ubuntu in Termux.

1. `tools/pack_for_box64.sh` on the PC -> `beer-box64-pack.tar.gz` (loader, libs, test exes,
   `vk_probe`); copy to the phone. (The prebuilt `loader` there still links libcrypto.)
2. Termux:X11: `pkg install x11-repo termux-x11-nightly`, `termux-x11 :0 &`, open the app.
3. Turnip with kgsl, built inside Ubuntu. Ubuntu's meson (1.3.2) is too old; use a venv
   (`python3 -m venv ~/mesonenv && ~/mesonenv/bin/pip install meson mako pyyaml packaging`).
   Option name for kgsl is `-Dfreedreno-kmds=kgsl` (check `grep -n -A6 freedreno-kmds meson.options`);
   X11 dev packages incl. `libx11-xcb-dev` are needed. Gen 8 (Adreno 840) Turnip is in
   upstream Mesa 26.0+ but the Android/kgsl path may need the `whitebelyash/mesa-tu8` fork.
   Install to `/opt/turnip`; the ICD JSON is in `/opt/turnip/share/vulkan/icd.d/`.
4. `vulkaninfo --summary`, then `box64 ./vk_probe` with `DISPLAY=:0`,
   `VK_ICD_FILENAMES=<icd json>`, `MESA_VK_WSI_DEBUG=sw`.
   If `vulkaninfo` works but `vk_probe` does not, Box64's Vulkan forwarding is the problem.
