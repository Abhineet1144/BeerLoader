# FEXCore-on-Android spikes

Proof-of-concept programs for embedding FEXCore (x86-64 -> ARM64 JIT) in a native bionic process.
Built and run **on the phone in Termux** (clang 21, aarch64, Android 16). Not part of the loader build.

FEX base commit: `fex-base-commit.txt`. Local changes to FEX: `fex-bionic.patch` (2 small bionic shims).

## Reproduce (in Termux)

```sh
pkg install git cmake ninja clang python && pip install packaging
git clone --depth 1 https://github.com/FEX-Emu/FEX && cd FEX
git submodule update --init --depth 1 External/vixl External/fmt External/xxhash \
  External/unordered_dense External/range-v3 External/rpmalloc External/drm-headers \
  External/Vulkan-Headers Source/Common/cpp-optparse
git apply ../fex-bionic.patch
mkdir build && cd build
# CMAKE_SYSTEM_NAME=Linux bypasses FEX's "Unsupported system type Android" check; API 30 exposes
# aligned_alloc etc. in bionic's headers.
CC=clang CXX=clang++ cmake -G Ninja .. -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_FLAGS=--target=aarch64-linux-android30 -DCMAKE_CXX_FLAGS=--target=aarch64-linux-android30 \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DENABLE_LTO=OFF -DENABLE_FEX_ALLOCATOR=OFF \
  -DENABLE_JEMALLOC_GLIBC_ALLOC=OFF -DENABLE_CCACHE=OFF -DBUILD_FEXCONFIG=OFF -DBUILD_THUNKS=OFF \
  -DENABLE_OFFLINE_TELEMETRY=OFF -DENABLE_GDB_SYMBOLS=OFF
ninja -j4 FEXCore FEXCore_Base fmt JemallocDummy       # ~1 min
cd ../.. && sh build_spike.sh spike0 spike0.cpp && ./spike0
```

## Findings

- FEXCore itself compiles for bionic with two shims (`getdents64`, `valloc`); the rest is build flags.
- bionic/arm64 requires the executable's PT_TLS segment to be 64-byte aligned: add one
  `alignas(64) thread_local` variable (see spike0.cpp).
- A thread needs a descriptor table before it can run: the decoder reads the CS descriptor
  (`State.segment_arrays[GDT]`, `cs_idx`, a long-mode code segment). Copy of what FEX's ARM64EC module does.
  A 64-bit GS base (e.g. a Windows TEB) goes in `State.gs_cached`.
- `SignalDelegator` and `SyscallHandler` are tiny interfaces; the Linux emulation frontend is not needed.

## Spikes

- `spike0.cpp`: `mov eax,42 ; hlt` through FEXCore. **Passes** (rax=42).
- `spike1.cpp`: guest -> host calls through the `syscall` instruction, Windows-style
  (`mov r10,rcx ; mov eax,N ; syscall`): args in registers, result in RAX, host reads guest memory,
  TEB via `gs:[0x30]` (`State.gs_cached`). **Passes.**
- `spike2.cpp`: guest memory fault. Guest loads distinctive values into rbx/rsi/r12/r15/rax/xmm0/CF, then
  writes to address 0. A host SIGSEGV handler rebuilds the guest context from the host `ucontext`, checks it,
  advances guest RIP past the faulting instruction and resumes. **Passes** (3/3 runs).
- `spike3.cpp`: host -> guest calls (nested, re-entrant) from inside an import handler, with six Win64
  arguments (two on the stack) through `CTX->HandleCallback`; the guest's registers and stack pointer are
  restored afterwards. **Passes** (3/3 runs).
- `spike4.cpp`: universal interception of a guest call to a **raw host function address** (what a host-built COM
  vtable entry is). **Passes**: 1.3-1.5 us per call (100,000 calls, 3/3 runs). See below.
- `plat_probe.c`: Android platform assumptions (JIT memory, fixed mappings, address space, signals). All OK.

## What spike1 taught (matters for the import-thunk design)

- FEXCore models real `syscall` semantics: on entry `State.rip` is the `syscall` instruction itself,
  RCX = address of the next instruction, R11 = flags. The handler must resume the guest with
  `State.rip = RCX` (otherwise the instruction re-executes forever).
- RCX is clobbered, so a thunk must pass argument 1 in R10 like Windows syscall stubs:
  `mov r10,rcx ; mov eax,<import id> ; syscall ; ret`.
- Host import functions can no longer be `ms_abi` C functions called by an asm trampoline. A generic
  dispatcher has to read RCX(R10)/RDX/R8/R9 and stack arguments from `CpuStateFrame` and call a plain
  C function; the few imports with float/XMM arguments need their signature known to the dispatcher.

## What spike2 taught (the fault / exception layer)

Recipe (mirrors `Source/Tools/LinuxEmulation/LinuxSyscalls/SignalDelegator.cpp`), all in a plain
`sigaction(SIGSEGV)` handler:
1. `CTX->IsAddressInCodeBuffer(Thread, pc)` tells a guest fault (inside FEX's JIT) from a host fault.
2. Rebuild guest state ("SpillSRA"): `State.rip = CTX->RestoreRIPFromHostPC(Thread, pc)` (exact guest RIP of the
   faulting instruction); `State.gregs[i] = uc.regs[Config.SRAGPRMapping[i]]` for `i < Config.SRAGPRCount`;
   XMM from the fpsimd record in `uc_mcontext.__reserved` via `Config.SRAFPRMapping`;
   EFLAGS via `CTX->ReconstructCompactedEFLAGS(...)` then `SetFlagsFromCompactedEFLAGS`.
   `Config` is `SignalDelegator::GetConfig()`, filled in by FEXCore at `InitCore`.
3. Resume (after e.g. a Windows handler changed the context): set `State.*`, then in the host `ucontext`
   set `pc = Config.AbsoluteLoopTopAddressFillSRA`, `x1 = 0`, `x28 = (uint64_t)Thread->CurrentFrame`.
   The dispatcher reloads the host registers from `State` and continues at `State.rip`.

Not covered yet: upper halves of YMM, x87/MXCSR, self-modifying-code tracking (`SMCChecks` is off; its
default write-fault tracking needs the same handler), multiple threads, nested faults, SA_ONSTACK.

## What spike3 taught (host -> guest calls and per-thread setup)

- **Every guest thread needs a call/return stack.** The JIT predicts `ret` targets with a stack reached through
  `State.callret_sp`. Allocate a guarded region and start the pointer a quarter of the way in
  (`CALLRET_STACK_SIZE`, see FEX `Source/Windows/Common/CallRetStack.h`); set `Thread->CallRetStackBase` too.
  Without it the first guest `call` writes to address -16. (Spikes 0-2 never executed a `call`, so they passed.)
  An imbalance (longjmp, SEH unwind, switching stacks) shows up as a fault inside that region: reset host
  register x25 to the default position in the signal handler and retry.
- **Host -> guest call recipe** (`GuestCall` in spike3.cpp): save the whole `CPUState`; lower guest RSP by
  `0x30 + 8*stack_args` and align so that RSP == 8 (mod 16) *after* FEX's own 16-byte adjustment; write stack
  arguments at `entry_rsp + 0x28`; set RCX/RDX/R8/R9; `CTX->HandleCallback(Thread, fn)`; read RAX; restore `CPUState`.
  `HandleCallback` pushes the return address itself but reserves no Win64 shadow space.
- **The callee returns to a CALLBACKRET stub:** `SignalDelegator::GetThunkCallbackRET()` must return the guest address
  of the two bytes `0F 3E` (FEX's private CALLBACKRET opcode). `EnableExitOnHLT` is *not* usable for this: `hlt` leaves the
  outermost dispatcher, not the nested callback.

## What spike4 taught (host-built COM vtables, ~48 arrays / ~900 slots in the loader)

The guest calls COM methods through vtables whose entries are plain host function addresses. Rather than rewriting
every vtable with `syscall` thunks, make the host program's own code range **non-executable for the guest**:

1. `SyscallHandler::QueryGuestExecutableRange(thread, addr)` returns `{addr, 0, false}` for addresses inside the host
   executable's text (`__ehdr_start` .. `etext`) and exact ranges around it for everything else (a range spanning the host
   text would make the decoder translate ARM code as x86).
2. A guest `call`/`jmp` to such an address makes FEX raise a guest SIGSEGV through its own stub: all guest registers are
   already spilled to `State`, `State.rip` is the host function, the return address is at `[RSP]`, and the host signal
   arrives with the pc in the *dispatcher* (`SignalDelegatorConfig::DispatcherBegin/End`), not the JIT code buffer.
3. The handler performs the call (Win64 args: RCX/RDX/R8/R9 and the stack at `RSP+0x28`), sets RAX, `rip = [RSP]`,
   `RSP += 8` and **`State.callret_sp += 0x10`**: the guest `call` pushed an entry on FEX's call/return stack that the emulated
   `ret` must pop, otherwise it leaks 16 bytes per call and eventually faults on the guard page.
4. Do **not** call `EnableExitOnHLT()`: in that mode FEX's guest-SIGSEGV stub exits cleanly instead of faulting.
5. FEX logs `NoExec instruction in entry block: <addr>` once per distinct target; filter it.

Cost is one host signal per call (~1.4 us). Calls that run real work (D3D12 command recording) should eventually get
`syscall` thunks like imports; this makes everything work first. The call must not run inside the signal handler: deliver it
like a guest fault (stash the state, redirect to the stub that executes `syscall` with a reserved id).
