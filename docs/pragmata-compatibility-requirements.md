# PRAGMATA compatibility requirements

Status: evidence baseline, not an executable-specific patch specification
Target inspected: `PRAGMATA.exe` from the supplied game directory
Compatibility implementation: Beer-owned code only

## 1. Scope and dependency policy

This document describes the Windows contracts Beer must provide for the target to initialize and run. It does **not** authorize RVA patches, instruction skipping, fabricated game state, or copying behavior/data from another compatibility runtime.

Allowed inputs:

- The target executable and files distributed with the game.
- Host operating-system services and device drivers exposed through stable host APIs.
- Beer's own source and generated assets.
- The explicitly approved shader translator.

Wine may be run separately as an external behavioral oracle to observe reached
API calls and documented return/output behavior. This does not permit copying
its source, DLLs, export inventories, internal data, or implementation into Beer.

Prohibited compatibility dependencies:

- Wine, Proton, or files generated/extracted from either.
- Host or copied Windows DLLs.
- Export lists copied from another runtime.
- Per-machine addresses, timing constants, CPU answers, or hardware identities.
- Target-specific RVA branches, forced callback returns, null-read emulation, or synthetic success responses.

Game-distributed PE DLLs are guest payload, not compatibility implementations. Beer may load them when they are part of the installation, but the loader must not silently substitute unrelated third-party DLLs. Optional vendor SDKs must be allowed to fail through their documented API contracts so the game can select a supported path.

## 2. Observable target contract

The values below are parsed directly from the supplied PE image and are evidence, not values to hardcode in Beer.

| Property | Observed value |
|---|---:|
| Architecture | PE32+, x86-64 |
| Subsystem | Windows GUI |
| Preferred image base | `0x140000000` |
| Image size | `0x17924000` |
| Entry RVA | `0x0feba380` |
| Sections | 19 |
| Import descriptors | 60 |
| Static imports | 728 |
| Exception directory | RVA `0x176b8000`, size `0x26b5b4` |
| TLS directory | RVA `0x09505590`, size `0x28` |
| Load-config directory | RVA `0x09505440`, size `0x140` |
| TLS raw initialization | 5,616 bytes plus zero fill |
| TLS callbacks | 4, in image-declared order |

The image has no base-relocation directory. Beer must therefore either map it at its preferred base or reject the launch cleanly. Relocating it without relocation records is invalid.

The load configuration contains image-owned control-flow check/dispatch pointers. Beer must preserve the mapped load-config data and executable protections. It must not replace those pointers with host-runtime addresses.

## 3. Required startup sequence

A conforming launch must perform these stages in this order:

1. Reserve and map the complete PE image at its preferred base.
2. Copy headers and section data, zero virtual tails, and apply final section protections.
3. Resolve static imports by both module and symbol/ordinal.
4. Load required game-distributed PE dependencies and recursively resolve their imports.
5. Initialize process-wide loader state: PEB, process parameters, environment, loader data, heap identity, version and processor fields.
6. Initialize the initial TEB, NT_TIB stack bounds, client ID, PEB pointer, TLS vector, last-error state, and FLS/TLS bookkeeping.
7. Allocate and copy the executable's static TLS block; write its assigned TLS index.
8. Process the load-config fields Beer supports, preserving unsupported fields rather than rewriting them.
9. Call dependency TLS callbacks and process-attach entry points in loader order.
10. Call all executable TLS callbacks in declared order with `DLL_PROCESS_ATTACH`.
11. Enter the executable only after every mandatory callback returns normally.

Executable TLS is mandatory Windows behavior, not an opt-in compatibility switch. A timeout is a diagnostic watchdog only. Continuing to the next callback or entry point after a timeout/fault creates invalid process state and must not be considered a successful launch.

## 4. Native module model

### 4.1 PEB loader database

Every module Beer reports as successfully loaded must have one persistent, internally consistent loader entry in all applicable PEB loader lists. Each entry must provide:

- Correct module-specific base address and image size.
- Correct full and base names as counted UTF-16 strings.
- Entry point where one exists.
- Stable list links and stable storage lifetime.
- Load/init ordering that matches actual initialization.

The database must update on dynamic load/unload. A static startup snapshot is insufficient.

### 4.2 Beer-owned system-module images

Code that walks `PEB->Ldr` and parses PE exports must see valid Beer-owned compatibility images. Requirements:

- Separate module identities and per-module export directories.
- Export names and ordinals generated only from Beer-owned implementation declarations.
- Export RVAs that remain inside the owning synthetic image.
- No identical union export table cloned into every system module.
- Correct failure semantics for unresolved exports; absence must remain absence unless Windows guarantees the export.
- API-set names must resolve through Beer's own module-routing table without external schema files.

The startup trace proves that an early callback opens `ntdll.dll` by Windows path and dynamically requests `win32u.dll`. This is a generic loader/module-discovery requirement. It does not justify importing either file from another runtime.

### 4.3 Import correctness

The current trace reports 1,266 generated thunks, of which 556 return zero. Import resolution alone is therefore not an acceptance signal. Each reached function needs its documented return convention, output initialization, error code, handle lifetime, and side effects.

Imports must be keyed by `(module, name-or-ordinal)`, not only by function name. The same symbol name may have different ownership or forwarding behavior in different modules.

## 5. Initial API surface

The static import inventory contains 60 module descriptors. They form implementation groups rather than a demand to execute arbitrary guest DLL code.

### Core process/runtime

- `KERNEL32`, `KERNELBASE`, `NTDLL`
- MSVC/UCRT API-set modules and game-distributed runtime DLLs
- `ADVAPI32`, `VERSION`, `OLE32`, `OLEAUT32`, `COMCTL32`, `SHLWAPI`, `SHELL32`

Required behavior includes files, mappings/sections, virtual memory, heap, environment, locale, synchronization, threads, waits, timing, process/module enumeration, registry, COM initialization, exceptions and x64 unwind.

### Window, input and device discovery

- `USER32`, `GDI32`, `IMM32`
- `HID`, `SETUPAPI`, `DINPUT8`, `XINPUT1_4`

Required behavior includes display topology, window/message lifetime, raw input, keyboard translation, cursor/capture, controller state, HID enumeration and stable device identity.

### Graphics and storage

- `DXGI`, `D3D11`, dynamically discovered D3D12 interfaces
- `DSTORAGE`
- Optional AMD, Intel XeSS, and frame-generation SDK modules distributed with the game

The baseline path is D3D12. Optional vendor paths must report unsupported capabilities honestly and permit fallback. DirectStorage requires a Beer-owned factory/queue/file/request contract or a documented synchronous fallback with equivalent completion/error semantics.

### Media and audio

- `MFPlat`, `MFReadWrite`
- Game-distributed audio/plugin modules

Media Foundation byte streams, source readers, DXGI device manager interaction, and frame lifetime must work. Audio must provide timing and callback behavior, not merely successful creation.

### Network/platform

- `WS2_32`, `WINHTTP`, `WININET`, `IPHLPAPI`
- The game-distributed platform API module

Offline operation may return documented network failures. It must still initialize output parameters, asynchronous completion state, last-error/WSA error, and callback lifetimes correctly.

The complete reproducible import inventory is obtained with:

```sh
python3 tools/pe_requirements.py /path/to/PRAGMATA.exe
```

## 6. Directly accessed process state

Early initialization executes substantial code without imported API calls, so API tracing cannot identify all dependencies. Beer must provide ABI-correct memory state for direct reads.

### TEB/NT_TIB

At minimum:

- Exception list / NT_TIB prefix.
- Live `StackBase` and `StackLimit` matching the active guest stack.
- TEB self pointer and process/thread client IDs.
- Process environment block pointer.
- TLS vector and static TLS slots for every loaded PE module.
- Last-error and last-status values at their Windows ABI offsets.
- Thread-local loader/FLS fields when used.

Beer-private dispatcher data must stay in host TLS or other host-owned memory, never in Windows-owned TEB offsets.

### PEB and process parameters

At minimum:

- Image base, loader database, process parameters and process heap.
- Command line, image path, current directory, environment and DLL search path.
- OS version/build fields derived from Beer's declared compatibility version.
- Processor count and process flags consistent with system-information APIs.
- Loader lock and initialization flags where observable.

### Shared user data and CPU state

- Live monotonic/system time fields at the documented shared-data offsets.
- Tick multiplier, interrupt time, system time, time-zone bias and version/product data must be mutually consistent.
- `CPUID`, `XGETBV`, MXCSR, x87 control state and processor-feature APIs must describe the host capabilities Beer actually permits guest code to use.
- System-basic-information values must be host-derived and mutually consistent: page size, allocation granularity, address bounds, physical pages, processor count and affinity.

No value may be copied from a reference machine or selected by executable name.

## 7. TLS callback-2 blocker

Native traces establish the following sequence when executable TLS is enabled:

1. Callback 0 returns.
2. Callback 1 returns after discovering Beer's `ntdll` and requesting `win32u`.
3. Callback 2 allocates/uses working state and remains in a CPU/TEB/shared-data computation path for at least 30 seconds.
4. It does not make further imported API calls during the observed loop.
5. Forcibly aborting it leaves callback 3 and the entry point with null state, producing the later three-site fault cycle.

Therefore the null-state faults are downstream symptoms. They are **not** candidates for emulation or patching.

The unresolved requirement is one or more directly read ABI fields or CPU semantics consumed by callback 2. It must be localized with Beer-native instrumentation:

- Periodic signal-based register/RIP sampling without changing guest registers.
- Static decode of only the sampled target-image basic blocks.
- Watchpoints or page-protection logging on Beer-owned TEB, PEB, TLS and shared-data pages.
- Before/after snapshots of those structures.
- Progress detection based on changed guest memory, not elapsed time alone.
- A standalone conformance test for every field/semantic fixed.

A valid fix must be stated as a general Windows ABI invariant and pass independently of this executable.

## 8. Implementation work packages

### P0 — Correct startup semantics

- Make executable TLS part of normal process initialization.
- Stop launch on mandatory TLS failure; do not continue with partial state.
- Remove null-read/write recovery as a compatibility mechanism.
- Initialize load-config-supported state before TLS.

### P0 — Correct process structures

- Replace sparse ad-hoc TEB/PEB layouts with offset-asserted ABI structures for every field Beer exposes.
- Add tests for initial and worker-thread stack bounds, client IDs, PEB pointers, TLS blocks, last error/status and process parameters.
- Keep system-information, shared-data, version and processor answers consistent.

### P0 — Correct module namespace

- Maintain a dynamic PEB loader database.
- Build per-module synthetic PE images and per-module exports from Beer-owned declarations.
- Route imports by module plus name/ordinal.
- Implement native `Ldr*` behavior needed by custom resolvers without external loader files.

### P1 — Replace reached zero stubs

- Add a trace mode that reports first call, caller module/RVA, arguments, result and modified output bytes for unresolved functions.
- Implement functions in first-reached order.
- Return documented failures for unsupported optional features.
- Add a focused test before marking each function implemented.

### P1 — Graphics/media/storage path

- Complete the D3D12/DXGI interfaces the engine reaches.
- Provide DirectStorage completion semantics or a correct fallback.
- Preserve Media Foundation and audio timing/lifetime behavior.
- Keep unsupported ray tracing, mesh shading, VRS and vendor extensions disabled until implemented.

### P2 — Runtime independence audit

- Ensure no compatibility path opens foreign Windows/runtime DLLs outside the game installation and Beer prefix.
- Ensure generated source contains no foreign-runtime export lists.
- Audit build/link/runtime dependencies against the dependency policy.
- Keep target-specific facts in diagnostics/docs, never in loader control flow.

## 9. Acceptance criteria

### Startup gate

- Preferred-base mapping succeeds.
- Every required static import resolves to a real implementation or documented, safe feature failure.
- All four executable TLS callbacks return naturally and exactly once, in order.
- No callback timeout, signal recovery, null-call substitution, or RVA redirect occurs.
- Entry point is reached with intact guest stack, TEB, PEB and TLS state.

### Engine gate

- The game creates its window, graphics device and swap chain.
- It reaches first visible frame with no zero-stub call whose result is consumed as success.
- Input, audio, media and storage completion paths remain live.
- Optional vendor features can be disabled without aborting startup.

### Independence gate

- No Wine/Proton path or binary is opened.
- No copied Windows system DLL is required.
- No target RVA appears in Beer source or behavior switches.
- Hardware/system answers are derived at runtime.
- Only approved translator code is used for shader translation.

### Regression gate

- Loader build and CLI smoke tests pass.
- Process-state, shared-data, virtual-memory, section, exception, unwind and TLS tests pass.
- Existing compatible applications retain their startup behavior.
- A target launch produces a bounded diagnostic report rather than a recovery loop.

## 10. Evidence versus unknowns

Proven:

- The PE metadata/import/TLS requirements above.
- PEB loader visibility is consumed before entry.
- `ntdll` and `win32u` are dynamically discovered by early initialization.
- Callback 2 must complete before callback 3/entry can use its state.
- Skipping or timing out callback 2 is invalid.

Not yet proven:

- The exact TEB/PEB/shared-data field that prevents callback 2 completion.
- Which of the 556 zero stubs are reached after valid TLS startup.
- The first required D3D12 feature level/interface after engine initialization.
- Whether DirectStorage is mandatory or can use the engine's fallback.

Unknowns must remain explicit. They are resolved by instrumentation and conformance tests, not guessed constants.
