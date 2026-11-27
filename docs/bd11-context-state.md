# BD11 owned pipeline state and internal mutable banks

## Scope and interface boundary

This is an internal state-ownership foundation, not a COM implementation of
ID3DDeviceContextState, CreateDeviceContextState or SwapDeviceContextState.
No Device1/2/3 or Context1/2/3 interface is added, advertised or resized. The
base context table remains 115 entries; ExecuteCommandList is slot 58 and
FinishCommandList is slot 114. No public SaveContextState method exists or is
introduced. The existing 77db970f-6276-48ba-ba28-070143b4392c entry is
IDXGIDevice1, not ID3D11Device1; device QI routes it to the separate DXGI table.
The actual ID3D11Device1 IID is a04bfb29-08ef-43d6-a49c-a9bdbdcbe686.
CPU regressions verify refusal of all Device1/2/3 and Context1/2/3 IIDs.

The foundation covers exactly the current B11PipelineState representation:
all six stages' shaders, CB pointers and numeric first/count ranges, 128 SRVs
and their borrowed resource aliases/high-water marks, samplers, UAVs and
initial-count shadow values; IA layout, topology, VB bindings/strides/offsets,
IB/format/offset; RTVs/DSV, blend factors/sample mask, depth/stencil reference,
rasterizer, viewport/scissor arrays/counts, and predication binding/value.
It does not add missing stream-output state, shader class-instance bindings,
unimplemented getters, lower-feature enforcement, extended setters, or backend
support for numeric CB ranges. UAV initial-count values are copied as shadow
data, not used to rewind resource counter contents. This is not full D3D11
pipeline coverage or D3D10/11 interface emulation.

## Ownership core

The helpers in graphics/d3d11v/bd11_context_state.[ch] have explicit contracts:

- `init` initializes fresh storage to BD11 defaults without releasing it.
- `capture` copies stable state into fresh storage and retains every owned
  binding, including duplicated pointers in different slots.
- `copy` replaces initialized owned storage, retaining the source first to
  keep aliases alive; self-copy is a no-op.
- `dispose` detaches all fields and releases every owned binding. A second
  dispose is harmless. Ownership walks cover all slots, not only srv_end or
  rtv_count. srv_res aliases are never independently retained/released.
- `reset` disposes and initializes defaults: NULL bindings, zero numeric data,
  blend factors of one and sample mask UINT32_MAX.

Raw owned-state helpers do not lock; capture/copy of live context state must
hold its context protection guard. None allocates memory. Full-state transitions
invalidate all BD11 dirty bits, bound VB/IB VA caches, generation comparison and
rendering-valid shadow. The next backend draw/dispatch re-evaluates its state;
immutable PSO/resource caches are not deleted. No GPU completion or drawing
correctness claim follows from the CPU tests.

## Existing command-list correction

ExecuteCommandList(TRUE) now captures an owned snapshot, clears to defaults,
replays, restores the snapshot and invalidates all binding shadows. FALSE also
starts from defaults, and ends at defaults. The same recursive guard surrounds
capture, replay and restore, so protected concurrent setters cannot interleave
or have their preceding changes overwritten by a pre-lock snapshot. Deferred
contexts, non-command-list objects and foreign-device command lists are rejected
without mutation. FinishCommandList continues its existing TRUE/FALSE behavior;
this increment does not redesign recording after FinishCommandList(TRUE),
mapped-resource handling or query recording.

## Internal mutable bank API

`b11_context_state_create` creates a host-only bank initialized to defaults. It
accepts only the device's nonzero exact granted feature level, not a lower/higher
level or emulated-interface GUID. Banks retain their device and owned bindings.
Their atomic reference count does not make their mutable saved state free-threaded.

`b11_context_state_swap` is immediate-context-only and requires the same device
and exact level. It uses the existing recursive protection guard. NULL incoming
bank has no effect, including leaving previous output untouched. Same-active
bank is a no-op; optional previous output receives an owned reference. On a
real switch the outgoing live state is saved into its bank, and the incoming
bank's last saved state becomes live. There is no external save method. The
live context remains authoritative until switch-out; an active bank's saved
copy is intentionally stale while setters run.

The original default bank is allocated lazily on the first switch, capturing
the application's then-current state. Both default and active pointers own
references; previous outputs are independently owned. All fallible allocation
happens before mutation, and failure leaves live state, caches, bank ownership
and previous output unchanged. A context releases both owned bank references
on destruction. Callers must serialize bank use with its immediate context;
concurrent activation across different contexts is not supported.

Swap touches only pipeline state and invalidation shadows, not map/discard
tracking, resource contents, query issued/ended/results, submission serials or
draw counts. Command lists do not retain banks and do not update inactive banks;
their live-state effects are saved only when that active bank switches out.

Contract inspiration: [Microsoft Learn SwapDeviceContextState semantics](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11devicecontext1-swapdevicecontextstate).
The internal bank deliberately omits that API's feature/interface emulation and
does not justify exposing its COM interface.

## CPU validation

`make test-d3d11-context-state` builds the complete linked BD11 backend but never
constructs a Vulkan device or launches a guest. It exercises actual ms_abi base
setters, slot 58 execution, slot 114 finish and slot 89 owned RTV/DSV getters:

- All represented owned slots, duplicate aliases and all numeric fields;
  exact reference counts, high SRVs, repeated/self-copy/dispose, last-owner
  destruction and release with intentionally shortened scan bounds.
- First replayed setter observes defaults, and the final setter observes
  preceding real replay bindings across all stages. TRUE/FALSE restoration,
  empty-list defaults, dirty/generation/rendering/VB/IB invalidation.
- Initial default bank, save-on-switch, last-state restore, NULL/same/foreign/
  deferred cases, inactive-bank lifetimes, active-bank command-list effects,
  exact feature-level rejection and allocation-failure atomicity.
- Real Begin query/predication plus unchanged query/map/serial state during
  swap. Condition-variable handshakes prove protected execution and swapping
  wait for recursive device-lock ownership, preserving a concurrent setter's
  preceding change. No sleeps, GPU operations or fabricated return values.

The replay observer wraps the external retain of the first and last recorded
setter, not replay itself: GNU linker wrapping cannot intercept calls defined
and called in the same translation unit. Other existing test targets are
test-d3d11-multithread, test-d3d11-creation and test-input. Header-layout changes
require a forced build to avoid stale objects.

### Verified results (2026-11-28)

- Context-state CPU test: PASS with all linked sources at O2, O0 and
  O1 + AddressSanitizer/UndefinedBehaviorSanitizer (leak detection enabled,
  UBSan halt-on-error). Sanitizer build uses `--param=asan-globals=0` to permit
  garbage-collection linking of unused backend COM globals.
- Existing multithread CPU test: PASS at O2, O0 and the same sanitizer settings.
- `make -B -j8`: full forced loader build PASS.
- Existing `test-d3d11-creation` and full `test-input`: PASS after that build.
- Editor diagnostics for the changed C/header files and whitespace diff check:
  clean. Existing unrelated compiler warnings remain.
- No guest/game/launcher launches or commits. No loader source or object-QI
  source edits in this increment, and no Direct2D/rendering success claim.