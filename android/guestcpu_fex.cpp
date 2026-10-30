// FEXCore backend for Beer's guest-CPU layer (platform/guest_cpu.h): runs the guest's x86-64 code on an
// ARM64 host (Android/bionic) while the loader's own code runs natively.
//
// Mechanisms proven by android/spike/spike0-3:
//   - imports are `mov r10,rcx ; mov eax,<id> ; syscall ; ret` guest thunks; HandleSyscall() passes the guest's
//     argument registers to the loader (beer_guest_import_dispatch) and resumes at RCX (spike1)
//   - guest faults arrive as host signals inside FEX's JIT code; the guest context is rebuilt from the host
//     ucontext (FEX "SpillSRA"), handed to the loader's crash logic, then applied back (spike2)
//   - host -> guest calls use CTX->HandleCallback with the guest's registers/stack saved and restored; the callee
//     returns to a stub holding FEX's CALLBACKRET opcode (spike3)
//   - every guest thread needs a call/return stack (State.callret_sp), a descriptor table with a long-mode code
//     segment, and a 64-byte aligned TLS segment in the executable (bionic rule)
#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/ArchHelpers/Arm64.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/fextl/memory.h>

#include "Common/HostFeatures.h"

extern "C" {
#include "guest_cpu.h"
}

#include <cerrno>
#include <dlfcn.h>
#include <malloc.h>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

// bionic/arm64 requires the executable's PT_TLS segment to be 64-byte aligned; FEX's and the loader's
// thread-locals are only 8-byte aligned, so force the alignment with one over-aligned thread-local.
alignas(64) thread_local char g_beer_tls_alignment_pad[64] __attribute__((used));

extern "C" char __ehdr_start;   // linker-provided: start of the loader's own image
extern "C" char etext;          // end of its code

namespace X = FEXCore::X86State;   // FEX register indices; the global REG_* are the x86 ucontext indices
using CPUState = FEXCore::Core::CPUState;
using Frame = FEXCore::Core::CpuStateFrame;

// Android's allocator (scudo) tags the top byte of heap pointers on ARM64 (0xb4... with pointer tagging / TBI). The
// hardware ignores that byte, but guest x86-64 code treats the full 64 bits as the address: the pointer is non-canonical
// to it, and the loader's own pointer validation rejects it (seen as "InitializeCriticalSection* invalid ptr
// 0xb40000..."). Pointers the loader hands to the guest must be plain addresses, so turn heap tagging off for the process
// before anything is allocated. An APK also needs android:allowNativeHeapPointerTagging="false" in its manifest.
#ifndef M_HEAP_TAGGING_LEVEL
#define M_HEAP_TAGGING_LEVEL (-204)
#endif
__attribute__((constructor(101))) static void BeerDisableHeapTagging() {
  mallopt(M_HEAP_TAGGING_LEVEL, 0 /* M_HEAP_TAGGING_LEVEL_NONE */);
}

namespace {

void MsgHandler(LogMan::DebugLevels Level, const char* Message) {
  // Expected: logged once per distinct host function the guest calls (see ExecutableRange below).
  if (strstr(Message, "NoExec instruction in entry block")) return;
  if (Level <= LogMan::ERROR) fprintf(stderr, "[FEX %s] %s\n", LogMan::DebugLevelStr(Level), Message);
}
void AssertHandler(const char* Message) {
  fprintf(stderr, "[FEX ASSERT] %s\n", Message);
  fflush(nullptr);
}

constexpr unsigned kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE};
constexpr size_t kAltStackSize = 256 * 1024;

struct GuestThread {
  FEXCore::Core::InternalThreadState* ts = nullptr;
  uint64_t gs_base = 0;                // pending / current GS base (the TEB pointer)
  uint64_t callret_lo = 0, callret_hi = 0, callret_default = 0;
  Frame* import_frame = nullptr;       // frame of the import being handled (null outside imports)
  jmp_buf* resume_jb = nullptr;        // where beer_guest_resume() unwinds to
  beer_guest_ucontext_t caller_uc;     // storage for beer_guest_caller_regs()
  struct beer_fpstate fp;
  bool altstack_ready = false;
  // A guest fault being delivered: the host signal handler stashes the faulting guest state here and redirects the
  // guest to the fault stub, which "calls" the reserved import kFaultImportId (see HandleSyscall).
  bool fault_pending = false;
  bool fault_is_host_call = false;   // true: the guest called a host function address; false: a real guest fault
  // Top-level host -> guest calls (a thread start routine, a callback on a host helper thread): the call runs on a
  // guest stack of its own and ends with the reserved kThreadDoneImportId import, which unwinds back to the caller.
  uint64_t stack_lo = 0, stack_hi = 0;
  jmp_buf* top_jb = nullptr;
  uint64_t top_result = 0;
  CPUState fault_state;
  uint32_t fault_eflags = 0;
  int fault_sig = 0;
  siginfo_t fault_si;
};
thread_local GuestThread t_gt;

FEXCore::Context::Context* g_ctx = nullptr;
fextl::unique_ptr<FEXCore::Context::Context> g_ctx_owner;
beer_guest_fault_fn g_fault_fn = nullptr;
uint64_t g_cb_ret = 0;                 // guest address of the CALLBACKRET stub
uint64_t g_host_lo = 0, g_host_hi = 0;   // the loader's own code: not executable for the guest (see QueryGuestExecutableRange)
uint64_t g_fault_stub = 0;             // guest address of the fault-delivery stub (mov eax,kFaultImportId ; syscall)
constexpr uint32_t kFaultImportId = 0x7FFFFFF0u;
uint64_t g_thread_stub = 0;            // guest stub: call rax ; mov r10,rax ; mov eax,kThreadDoneImportId ; syscall
constexpr uint32_t kThreadDoneImportId = 0x7FFFFFF1u;
constexpr size_t kGuestStackSize = 8u << 20;   // virtual; pages are committed on use
struct sigaction g_prev_action[NSIG];
std::once_flag g_init_once;
bool g_init_ok = false;

class BeerSyscallHandler final : public FEXCore::HLE::SyscallHandler {
public:
  void HandleSyscall(Frame* F) override;
  std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return std::nullopt;
  }
  // Everything is executable for the guest except the loader's own code. A guest call/jmp into the loader's code (a
  // host-built COM vtable entry, a raw function pointer) then raises a guest fault that HostSignal() turns into a host
  // call (android/spike/spike4.cpp). The ranges returned must not span the host code or the decoder would translate it.
  FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t Address) override {
    if (Address < g_host_lo) return {0, g_host_lo, true};
    if (Address < g_host_hi) return {Address, 0, false};
    return {g_host_hi, UINT64_MAX - g_host_hi, true};
  }
};

class BeerSignalDelegator final : public FEXCore::SignalDelegator {
public:
  uintptr_t GetThunkCallbackRET() const override { return (uintptr_t)g_cb_ret; }
};

BeerSyscallHandler g_syscalls;
BeerSignalDelegator g_signals;

// ---- guest register <-> x86 ucontext layout -------------------------------------------------------------
constexpr struct { int reg; int x86; } kRegMap[] = {
  {X::REG_RAX, ::REG_RAX}, {X::REG_RCX, ::REG_RCX}, {X::REG_RDX, ::REG_RDX}, {X::REG_RBX, ::REG_RBX},
  {X::REG_RSP, ::REG_RSP}, {X::REG_RBP, ::REG_RBP}, {X::REG_RSI, ::REG_RSI}, {X::REG_RDI, ::REG_RDI},
  {X::REG_R8, ::REG_R8},   {X::REG_R9, ::REG_R9},   {X::REG_R10, ::REG_R10}, {X::REG_R11, ::REG_R11},
  {X::REG_R12, ::REG_R12}, {X::REG_R13, ::REG_R13}, {X::REG_R14, ::REG_R14}, {X::REG_R15, ::REG_R15},
};

void StateToUc(const CPUState& s, uint32_t eflags, GuestThread& gt, beer_guest_ucontext_t* uc) {
  memset(uc, 0, sizeof(*uc));
  for (auto m : kRegMap) uc->uc_mcontext.gregs[m.x86] = (long long)s.gregs[m.reg];
  uc->uc_mcontext.gregs[::REG_RIP] = (long long)s.rip;
  uc->uc_mcontext.gregs[::REG_EFL] = (long long)eflags;
  for (int i = 0; i < 16; ++i) memcpy(gt.fp._xmm[i].element, &s.xmm.sse.data[i][0], 16);
  uc->uc_mcontext.fpregs = &gt.fp;
}

void UcToState(const beer_guest_ucontext_t* uc, CPUState& s) {
  for (auto m : kRegMap) s.gregs[m.reg] = (uint64_t)uc->uc_mcontext.gregs[m.x86];
  s.rip = (uint64_t)uc->uc_mcontext.gregs[::REG_RIP];
}

// ---- thread setup -----------------------------------------------------------------------------------------
void EnsureAltStack(GuestThread& gt) {
  if (gt.altstack_ready) return;
  stack_t cur{};
  if (sigaltstack(nullptr, &cur) == 0 && !(cur.ss_flags & SS_DISABLE) && cur.ss_size >= 64 * 1024) {
    gt.altstack_ready = true;   // the loader already installed one on this thread
    return;
  }
  void* mem = mmap(nullptr, kAltStackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem != MAP_FAILED) {
    stack_t ss{};
    ss.ss_sp = mem;
    ss.ss_size = kAltStackSize;
    sigaltstack(&ss, nullptr);
  }
  gt.altstack_ready = true;
}

// Creates the FEX thread state for the calling host thread (descriptor table, call/return stack, GS base).
bool AttachThread(GuestThread& gt) {
  if (gt.ts) return true;
  gt.ts = g_ctx->CreateThread();
  if (!gt.ts) return false;
  auto& st = gt.ts->CurrentFrame->State;

  auto* gdt = new CPUState::gdt_segment[32]();
  auto& cs = gdt[CPUState::DEFAULT_USER_CS];
  CPUState::SetGDTBase(&cs, 0);
  CPUState::SetGDTLimit(&cs, 0xFFFFFu);
  cs.L = 1;   // long mode
  cs.D = 0;
  st.segment_arrays[CPUState::SEGMENT_ARRAY_INDEX_GDT] = &gdt[0];
  st.segment_arrays[CPUState::SEGMENT_ARRAY_INDEX_LDT] = &gdt[0];
  st.cs_idx = CPUState::DEFAULT_USER_CS << 3;
  st.gs_cached = gt.gs_base;
  st.FCW = 0x37F;

  const size_t sz = FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE, pg = 4096;
  void* base = mmap(nullptr, sz + 2 * pg, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (base == MAP_FAILED || mprotect((char*)base + pg, sz, PROT_READ | PROT_WRITE) != 0) return false;
  gt.ts->CallRetStackBase = (char*)base + pg;
  gt.callret_lo = (uint64_t)base;
  gt.callret_hi = (uint64_t)base + sz + 2 * pg;
  gt.callret_default = (uint64_t)gt.ts->CallRetStackBase + sz / 4;
  st.callret_sp = gt.callret_default;

  EnsureAltStack(gt);
  return true;
}

// ---- guest faults -------------------------------------------------------------------------------------------
void ChainOrDie(int sig, siginfo_t* si, void* uc) {
  const struct sigaction& prev = g_prev_action[sig];
  if (prev.sa_flags & SA_SIGINFO) {
    if (prev.sa_sigaction) { prev.sa_sigaction(sig, si, uc); return; }
  } else if (prev.sa_handler != SIG_DFL && prev.sa_handler != SIG_IGN && prev.sa_handler) {
    prev.sa_handler(sig);
    return;
  }
  signal(sig, SIG_DFL);   // re-raised on return: a genuine host crash
}

void HostSignal(int sig, siginfo_t* si, void* uc_) {
  auto* uc = (ucontext_t*)uc_;
  GuestThread& gt = t_gt;
  const uint64_t pc = uc->uc_mcontext.pc;
  const auto& cfg = g_signals.GetConfig();
  const bool in_jit = gt.ts && g_ctx->IsAddressInCodeBuffer(gt.ts, pc);
  // Faults raised by FEX itself (e.g. executing a non-executable address) come from its dispatcher stubs, where the
  // guest registers are already spilled to State.
  const bool in_dispatcher = gt.ts && pc >= cfg.DispatcherBegin && pc < cfg.DispatcherEnd;
  if (!in_jit && !in_dispatcher) {
    // Not a guest event: a genuine host crash (or a signal for the loader's own handlers). Say where it happened first.
    Dl_info di{};
    const int known = dladdr((void*)pc, &di);
    fprintf(stderr, "[GUESTCPU] host signal %d at pc=0x%llx (%s +0x%llx %s) si_addr=%p guest-thread=%s\n", sig, (unsigned long long)pc,
            known && di.dli_fname ? di.dli_fname : "?", known ? (unsigned long long)(pc - (uintptr_t)di.dli_fbase) : 0ull,
            known && di.dli_sname ? di.dli_sname : "", si->si_addr, gt.ts ? "attached" : "none");
    // The caller: x30 is the return address, and x29 chains the frames (offsets are relative to the loader image, so they
    // can be resolved with llvm-symbolizer on the same binary).
    const uint64_t base = (uint64_t)&__ehdr_start;
    fprintf(stderr, "[GUESTCPU]   lr=0x%llx (loader+0x%llx)", (unsigned long long)uc->uc_mcontext.regs[30],
            (unsigned long long)(uc->uc_mcontext.regs[30] - base));
    uint64_t* fp = (uint64_t*)uc->uc_mcontext.regs[29];
    for (int depth = 0; depth < 10 && fp && ((uintptr_t)fp & 7) == 0; ++depth) {
      const uint64_t ret = fp[1];
      if (ret >= base && ret < g_host_hi) fprintf(stderr, " <- +0x%llx", (unsigned long long)(ret - base));
      else fprintf(stderr, " <- 0x%llx", (unsigned long long)ret);
      uint64_t* next = (uint64_t*)fp[0];
      if (next <= fp) break;
      fp = next;
    }
    fprintf(stderr, "\n");
    ChainOrDie(sig, si, uc_);
    return;
  }

  // Call/return stack imbalance (longjmp, SEH unwinds, stack switches): put the pointer back and retry.
  const uint64_t fa = (uint64_t)si->si_addr;
  if (sig == SIGSEGV && fa >= gt.callret_lo && fa < gt.callret_hi) {
    uc->uc_mcontext.regs[25] = gt.callret_default;
    return;
  }
  // Misaligned atomics in JIT code are emulated by FEX itself.
  if (sig == SIGBUS && in_jit) {
    if (auto adv = FEXCore::ArchHelpers::Arm64::HandleUnalignedAccess(gt.ts, FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::HalfBarrier, pc,
                                                                       (uint64_t*)uc->uc_mcontext.regs)) {
      uc->uc_mcontext.pc = pc + *adv;
      return;
    }
  }

  Frame* F = gt.ts->CurrentFrame;
  auto& st = F->State;
  uint32_t eflags;
  if (in_jit) {
    // ---- Fault inside JIT code: rebuild the guest context from the host registers (FEX "SpillSRA", spike2).
    st.rip = g_ctx->RestoreRIPFromHostPC(gt.ts, pc);
    for (size_t i = 0; i < cfg.SRAGPRCount; i++) st.gregs[i] = uc->uc_mcontext.regs[cfg.SRAGPRMapping[i]];
    struct HostFpr { uint32_t magic, size, fpsr, fpcr; __uint128_t v[32]; };
    auto* fpr = reinterpret_cast<HostFpr*>(&uc->uc_mcontext.__reserved[0]);
    if (fpr->magic == 0x46508001)
      for (size_t i = 0; i < cfg.SRAFPRCount; i++) memcpy(&st.xmm.sse.data[i][0], &fpr->v[cfg.SRAFPRMapping[i]], 16);
    st.callret_sp = uc->uc_mcontext.regs[25];   // x25 is the live call/return stack pointer (REG_CALLRET_SP)
    eflags = g_ctx->ReconstructCompactedEFLAGS(gt.ts, true, (uint64_t*)uc->uc_mcontext.regs, uc->uc_mcontext.pstate);
    g_ctx->SetFlagsFromCompactedEFLAGS(gt.ts, eflags);
  } else {
    eflags = g_ctx->ReconstructCompactedEFLAGS(gt.ts, false, nullptr, 0);   // already spilled by FEX's stub
  }

  // A guest call/jmp into the loader's own code is a call to a host function (a COM vtable entry): perform it.
  gt.fault_is_host_call = st.rip >= g_host_lo && st.rip < g_host_hi;

  // The loader's fault logic may call back into guest code (vectored/SEH handlers), which cannot run from inside a
  // signal handler. Deliver the fault as a synthetic import instead: stash the faulting state, point the guest at the
  // fault stub (`mov eax,kFaultImportId ; syscall`), and let the dispatcher run it. HandleSyscall then runs the
  // loader's logic as an ordinary host frame.
  memcpy(&gt.fault_state, &st, sizeof(st));
  gt.fault_eflags = eflags;
  gt.fault_sig = sig;
  gt.fault_si = *si;
  gt.fault_pending = true;
  st.rip = g_fault_stub;

  // Resume through the dispatcher, which reloads the host registers from State and continues at State.rip.
  uc->uc_mcontext.pc = cfg.AbsoluteLoopTopAddressFillSRA;
  uc->uc_mcontext.regs[1] = 0;
  uc->uc_mcontext.regs[28] = (uint64_t)F;
}

void InstallSignals() {
  for (unsigned sig : kSignals) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = HostSignal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, &g_prev_action[sig]);
  }
}

// Runs the loader's crash/exception logic for the guest fault stashed by HostSignal(). On return the guest continues
// at whatever context the logic left (a vectored handler's CONTINUE_EXECUTION, an SEH unwind target, a skipped
// instruction...). beer_guest_resume() from inside also works: it unwinds to the setjmp in HandleSyscall.
void DeliverFault(Frame* F, GuestThread& gt) {
  gt.fault_pending = false;
  CPUState faulting;   // consumed now so a nested fault can reuse the slot (CPUState is not copy-constructible)
  memcpy(&faulting, &gt.fault_state, sizeof(faulting));
  const int sig = gt.fault_sig;
  siginfo_t si = gt.fault_si;

  Frame* prev_frame = gt.import_frame;
  jmp_buf* prev_jb = gt.resume_jb;
  jmp_buf jb;
  gt.import_frame = F;
  gt.resume_jb = &jb;
  if (_setjmp(jb) == 0) {
    // The stub's own register state is irrelevant; guest code called from the handler runs below the faulting RSP.
    memcpy(&F->State, &faulting, sizeof(faulting));
    if (gt.fault_is_host_call) {
      // The guest called the host function at faulting.rip with Win64 arguments: RCX/RDX/R8/R9 and the stack at RSP+0x28.
      const uint64_t rsp = faulting.gregs[X::REG_RSP];
      uint64_t a[16];
      a[0] = faulting.gregs[X::REG_RCX];
      a[1] = faulting.gregs[X::REG_RDX];
      a[2] = faulting.gregs[X::REG_R8];
      a[3] = faulting.gregs[X::REG_R9];
      const uint64_t* stack_args = reinterpret_cast<const uint64_t*>(rsp + 0x28);
      for (int i = 0; i < 12; ++i) a[4 + i] = stack_args[i];
      uint64_t x[4];
      for (int i = 0; i < 4; ++i) x[i] = faulting.xmm.sse.data[i][0];
      const uint64_t result = beer_guest_host_call(faulting.rip, a, x);
      memcpy(&F->State, &faulting, sizeof(faulting));   // nested guest calls restored it already; be explicit
      F->State.gregs[X::REG_RAX] = result;
      F->State.rip = *reinterpret_cast<const uint64_t*>(rsp);   // the return address the guest `call` pushed
      F->State.gregs[X::REG_RSP] = rsp + 8;
      F->State.callret_sp += 0x10;   // the emulated RET pops the call/return stack entry the CALL pushed
      gt.import_frame = prev_frame;
      gt.resume_jb = prev_jb;
      return;
    }
    beer_guest_ucontext_t guc;
    StateToUc(faulting, gt.fault_eflags, gt, &guc);
    if (g_fault_fn) {
      g_fault_fn(sig, &si, &guc);
    } else {
      fprintf(stderr, "[GUESTCPU] guest fault (signal %d) at guest rip=0x%llx with no fault handler\n", sig,
              (unsigned long long)faulting.rip);
      _exit(139);
    }
    memcpy(&F->State, &faulting, sizeof(faulting));   // nested guest calls restored it already; be explicit
    UcToState(&guc, F->State);
    g_ctx->SetFlagsFromCompactedEFLAGS(gt.ts, (uint32_t)guc.uc_mcontext.gregs[::REG_EFL]);
    if (guc.uc_mcontext.fpregs)
      for (int i = 0; i < 16; ++i) memcpy(&F->State.xmm.sse.data[i][0], guc.uc_mcontext.fpregs->_xmm[i].element, 16);
  }
  // else: beer_guest_resume() already loaded the new context into F->State
  gt.import_frame = prev_frame;
  gt.resume_jb = prev_jb;
}

// ---- imports ------------------------------------------------------------------------------------------------
void BeerSyscallHandler::HandleSyscall(Frame* F) {
  GuestThread& gt = t_gt;
  auto& s = F->State;
  const uint64_t id = s.gregs[X::REG_RAX];
  const uint64_t rsp = s.gregs[X::REG_RSP];
  const uint64_t resume = s.gregs[X::REG_RCX];   // real `syscall` semantics: RCX = address after the instruction

  if ((uint32_t)id == kThreadDoneImportId && gt.top_jb) {
    gt.top_result = s.gregs[X::REG_R10];   // the guest function's RAX, moved by the stub
    F->InSyscallInfo = 0;
    longjmp(*gt.top_jb, 1);
  }
  if (gt.fault_pending && (uint32_t)id == kFaultImportId) {
    DeliverFault(F, gt);
    return;
  }

  uint64_t a[16];
  a[0] = s.gregs[X::REG_R10];   // argument 1 (the thunk moved it out of the RCX that `syscall` clobbers)
  a[1] = s.gregs[X::REG_RDX];
  a[2] = s.gregs[X::REG_R8];
  a[3] = s.gregs[X::REG_R9];
  const uint64_t* stack_args = reinterpret_cast<const uint64_t*>(rsp + 0x28);   // after return address + shadow space
  for (int i = 0; i < 12; ++i) a[4 + i] = stack_args[i];   // fixed window, over-reads like the native trampoline
  uint64_t x[4];
  for (int i = 0; i < 4; ++i) x[i] = s.xmm.sse.data[i][0];   // Win64 float arguments (XMM0-3 by position)

  Frame* prev_frame = gt.import_frame;
  jmp_buf* prev_jb = gt.resume_jb;
  jmp_buf jb;
  gt.import_frame = F;
  gt.resume_jb = &jb;
  if (_setjmp(jb) == 0) {
    const uint64_t result = beer_guest_import_dispatch(id, a, x, rsp);
    F->State.gregs[X::REG_RAX] = result;
    F->State.rip = resume;
  }
  // else: beer_guest_resume() loaded a new guest context into State and jumped here; State.rip is already set.
  gt.import_frame = prev_frame;
  gt.resume_jb = prev_jb;
}

// ---- context initialisation --------------------------------------------------------------------------------
void InitOnce() {
  LogMan::Throw::InstallHandler(AssertHandler);
  LogMan::Msg::InstallHandler(MsgHandler);
  FEXCore::Config::Initialize();
  FEXCore::Config::Load();
  FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
  FEXCore::Config::Set(FEXCore::Config::CONFIG_SMCCHECKS, "0");   // TODO: write-fault based SMC tracking

  g_host_lo = (uint64_t)&__ehdr_start;
  g_host_hi = (uint64_t)&etext;

  void* page = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (page == MAP_FAILED) return;
  static const unsigned char kCallbackRet[] = {0x0F, 0x3E};   // FEX CALLBACKRET: a host-called guest function returns here
  memcpy(page, kCallbackRet, sizeof kCallbackRet);
  g_cb_ret = (uint64_t)page;
  unsigned char* stub = (unsigned char*)page + 16;   // mov eax, kFaultImportId ; syscall ; hlt (never reached)
  stub[0] = 0xB8;
  memcpy(stub + 1, &kFaultImportId, 4);
  stub[5] = 0x0F; stub[6] = 0x05; stub[7] = 0xF4;
  g_fault_stub = (uint64_t)stub;
  unsigned char* tstub = (unsigned char*)page + 32;   // call rax ; mov r10, rax ; mov eax, kThreadDoneImportId ; syscall ; hlt
  tstub[0] = 0xFF; tstub[1] = 0xD0;
  tstub[2] = 0x49; tstub[3] = 0x89; tstub[4] = 0xC2;
  tstub[5] = 0xB8;
  memcpy(tstub + 6, &kThreadDoneImportId, 4);
  tstub[10] = 0x0F; tstub[11] = 0x05; tstub[12] = 0xF4;
  g_thread_stub = (uint64_t)tstub;

  auto host = FEX::FetchHostFeatures(true);
  g_ctx_owner = FEXCore::Context::Context::CreateNewContext(host);
  g_ctx = g_ctx_owner.get();
  g_ctx->SetSignalDelegator(&g_signals);
  g_ctx->SetSyscallHandler(&g_syscalls);
  if (!g_ctx->InitCore()) return;
  InstallSignals();
  g_init_ok = true;
}

bool EnsureInit() {
  std::call_once(g_init_once, InitOnce);
  return g_init_ok;
}

[[noreturn]] void Fatal(const char* what) {
  fprintf(stderr, "[GUESTCPU] fatal: %s\n", what);
  fflush(stderr);
  _exit(2);
}


// The guest stack for the calling thread, with a guard page below it. Allocated on first use.
bool EnsureGuestStack(GuestThread& gt) {
  if (gt.stack_lo) return true;
  void* stk = mmap(nullptr, kGuestStackSize + 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (stk == MAP_FAILED || mprotect((char*)stk + 4096, kGuestStackSize, PROT_READ | PROT_WRITE) != 0) return false;
  gt.stack_lo = (uint64_t)stk + 4096;
  gt.stack_hi = gt.stack_lo + kGuestStackSize;
  return true;
}

// A host -> guest call with no guest code active on this thread (thread start, callbacks on host helper threads).
uint64_t TopLevelCall(GuestThread& gt, uint64_t fn, const uint64_t* args, unsigned nargs, unsigned float_mask) {
  if (!EnsureInit()) Fatal("FEXCore failed to initialise");
  if (!AttachThread(gt)) Fatal("cannot create the guest thread");
  if (!EnsureGuestStack(gt)) Fatal("cannot allocate a guest stack");
  Frame* F = gt.ts->CurrentFrame;
  auto& s = F->State;

  // Win64 call site: RSP is 16-byte aligned at the `call rax`; the callee finds its stack arguments at entry_rsp + 0x28.
  const unsigned stack_args = nargs > 4 ? nargs - 4 : 0;
  const uint64_t rsp = (gt.stack_hi - 0x100 - 8 * stack_args) & ~0xFull;
  uint64_t* stk = reinterpret_cast<uint64_t*>(rsp + 0x20);
  for (unsigned i = 0; i < stack_args; ++i) stk[i] = args[4 + i];

  for (auto& g : s.gregs) g = 0;
  static const int kGp[4] = {X::REG_RCX, X::REG_RDX, X::REG_R8, X::REG_R9};
  for (unsigned i = 0; i < nargs && i < 4; ++i) {
    if (float_mask & (1u << i)) { s.xmm.sse.data[i][0] = args[i]; s.xmm.sse.data[i][1] = 0; }
    else s.gregs[kGp[i]] = args[i];
  }
  s.gregs[X::REG_RAX] = fn;
  s.gregs[X::REG_RSP] = rsp;
  s.rip = g_thread_stub;
  s.gs_cached = gt.gs_base;
  s.callret_sp = gt.callret_default;

  Frame* prev_frame = gt.import_frame;
  jmp_buf* prev_resume = gt.resume_jb;
  jmp_buf* prev_top = gt.top_jb;
  jmp_buf jb;
  gt.import_frame = nullptr;
  gt.resume_jb = nullptr;
  gt.top_jb = &jb;
  if (_setjmp(jb) == 0) g_ctx->ExecuteThread(gt.ts);   // returns here only through the thread-done import
  gt.top_jb = prev_top;
  gt.resume_jb = prev_resume;
  gt.import_frame = prev_frame;
  return gt.top_result;
}

} // namespace

// ============================== the guest_cpu.h contract ==============================================================

extern "C" {

int beer_guest_get_gs_base(uint64_t* base) {
  *base = t_gt.gs_base;
  return 0;
}

int beer_guest_set_gs_base(uint64_t base) {
  t_gt.gs_base = base;
  if (t_gt.ts) t_gt.ts->CurrentFrame->State.gs_cached = base;
  return 0;
}

int beer_guest_thread_stack(uint64_t* low, uint64_t* high) {
  if (!EnsureGuestStack(t_gt)) return -1;
  *low = t_gt.stack_lo;
  *high = t_gt.stack_hi;
  return 0;
}

void beer_guest_set_fault_handler(beer_guest_fault_fn fn) {
  g_fault_fn = fn;
}

void beer_guest_enter(uintptr_t stack_top, BeerGuestTlsRunner tls, uintptr_t entry) {
  if (!EnsureInit()) Fatal("FEXCore failed to initialise");
  GuestThread& gt = t_gt;
  if (!AttachThread(gt)) Fatal("cannot create the guest thread");
  // The TLS runner is host code; it calls each guest TLS callback through beer_guest_call (a top-level call on this
  // thread's guest stack), as the native entry runs them on the initial thread before the image's entry point.
  if (tls) tls();

  auto& st = gt.ts->CurrentFrame->State;
  st.rip = entry;
  st.gregs[X::REG_RSP] = stack_top;
  st.gregs[X::REG_RAX] = st.gregs[X::REG_RCX] = st.gregs[X::REG_RDX] = st.gregs[X::REG_R8] = st.gregs[X::REG_R9] = 0;   // as the native entry asm does
  st.gs_cached = gt.gs_base;

  g_ctx->ExecuteThread(gt.ts);   // returns only if the guest executes `hlt` (EnableExitOnHLT is not set): never in practice
  Fatal("the guest's entry point returned");
}

uint64_t beer_guest_call(uint64_t fn, const uint64_t* args, unsigned nargs) {
  return beer_guest_call_f(fn, args, nargs, 0);
}

uint64_t beer_guest_call_f(uint64_t fn, const uint64_t* args, unsigned nargs, unsigned float_mask) {
  GuestThread& gt = t_gt;
  Frame* F = gt.import_frame;
  if (!F) return TopLevelCall(gt, fn, args, nargs, float_mask);

  CPUState saved;
  memcpy(&saved, &F->State, sizeof saved);

  // HandleCallback pushes the return address and lowers RSP by 16 but reserves no Win64 shadow space, so
  // reserve it (and the stack arguments) first, keeping RSP == 8 (mod 16) at function entry (spike3).
  const unsigned stack_args = nargs > 4 ? nargs - 4 : 0;
  uint64_t rsp = saved.gregs[X::REG_RSP] - 0x30 - 8 * ((stack_args + 1) & ~1u);
  rsp = (rsp & ~0xFull) | 8;
  auto* stk = reinterpret_cast<uint64_t*>(rsp + 0x18);   // entry_rsp + 0x28
  for (unsigned i = 0; i < stack_args; ++i) stk[i] = args[4 + i];

  auto& s = F->State;
  s.gregs[X::REG_RSP] = rsp;
  static const int kGp[4] = {X::REG_RCX, X::REG_RDX, X::REG_R8, X::REG_R9};
  for (unsigned i = 0; i < nargs && i < 4; ++i) {
    if (float_mask & (1u << i)) {            // Win64: a float/double argument travels in XMM<position>
      s.xmm.sse.data[i][0] = args[i];
      s.xmm.sse.data[i][1] = 0;
    } else {
      s.gregs[kGp[i]] = args[i];
    }
  }

  jmp_buf* prev_jb = gt.resume_jb;
  g_ctx->HandleCallback(gt.ts, fn);
  gt.resume_jb = prev_jb;
  gt.import_frame = F;

  const uint64_t result = F->State.gregs[X::REG_RAX];
  memcpy(&F->State, &saved, sizeof saved);   // the guest resumes exactly where it was
  return result;
}

uint64_t beer_guest_call_in_context(const beer_guest_ucontext_t* regs, uint64_t fn, const uint64_t* args, unsigned nargs) {
  GuestThread& gt = t_gt;
  Frame* F = gt.import_frame;
  if (!F) Fatal("beer_guest_call_in_context outside an import");
  if (nargs > 4) Fatal("beer_guest_call_in_context: stack arguments are not supported");

  CPUState saved;
  memcpy(&saved, &F->State, sizeof saved);

  auto& s = F->State;
  const auto& g = regs->uc_mcontext.gregs;
  static const int kNonvolatile[][2] = {
    {X::REG_RBX, ::REG_RBX}, {X::REG_RBP, ::REG_RBP}, {X::REG_RSI, ::REG_RSI}, {X::REG_RDI, ::REG_RDI},
    {X::REG_R12, ::REG_R12}, {X::REG_R13, ::REG_R13}, {X::REG_R14, ::REG_R14}, {X::REG_R15, ::REG_R15},
  };
  for (const auto& r : kNonvolatile) s.gregs[r[0]] = (uint64_t)g[r[1]];
  // Same frame as the native beer_call_consolidate_asm: align the unwound RSP down to 16, reserve 0x50, then `call`
  // (entry RSP = aligned - 0x58). HandleCallback's entry RSP is RSP - 0x10 (see beer_guest_call_f).
  s.gregs[X::REG_RSP] = (((uint64_t)g[::REG_RSP]) & ~0xFull) - 0x48;
  static const int kGp[4] = {X::REG_RCX, X::REG_RDX, X::REG_R8, X::REG_R9};
  for (unsigned i = 0; i < nargs; ++i) s.gregs[kGp[i]] = args[i];

  jmp_buf* prev_jb = gt.resume_jb;
  g_ctx->HandleCallback(gt.ts, fn);
  gt.resume_jb = prev_jb;
  gt.import_frame = F;

  const uint64_t result = F->State.gregs[X::REG_RAX];
  memcpy(&F->State, &saved, sizeof saved);
  return result;
}

void beer_guest_caller_regs(beer_guest_ucontext_t* out) {
  GuestThread& gt = t_gt;
  Frame* F = gt.import_frame;
  if (!F) Fatal("beer_guest_caller_regs outside an import");
  const auto& s = F->State;
  const uint64_t rsp = s.gregs[X::REG_RSP];
  StateToUc(s, (uint32_t)s.gregs[X::REG_R11], gt, out);   // `syscall` left the flags in R11
  out->uc_mcontext.gregs[::REG_RIP] = (long long)*(const uint64_t*)rsp;   // return address
  out->uc_mcontext.gregs[::REG_RSP] = (long long)(rsp + 8);               // after the caller's call returns
  out->uc_mcontext.gregs[::REG_RCX] = (long long)s.gregs[X::REG_R10];        // the original argument 1
  out->uc_mcontext.gregs[::REG_R11] = 0;
}

void beer_guest_resume(const beer_guest_ucontext_t* regs) {
  GuestThread& gt = t_gt;
  Frame* F = gt.import_frame;
  if (!F || !gt.resume_jb) Fatal("beer_guest_resume outside an import");
  auto& st = F->State;
  UcToState(regs, st);
  g_ctx->SetFlagsFromCompactedEFLAGS(gt.ts, (uint32_t)regs->uc_mcontext.gregs[::REG_EFL]);
  if (regs->uc_mcontext.fpregs)
    for (int i = 6; i <= 15; ++i) memcpy(&st.xmm.sse.data[i][0], regs->uc_mcontext.fpregs->_xmm[i].element, 16);   // nonvolatile bank
  longjmp(*gt.resume_jb, 1);
}

} // extern "C"
