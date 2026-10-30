// Beer guest-CPU spike 2: guest memory faults.
// The guest loads distinctive values into registers (incl. XMM0 and CF), then writes to address 0.
// A host SIGSEGV handler rebuilds the guest context from the host ucontext the way FEX's own Linux
// SignalDelegator does (SpillSRA), checks it against the known values, then resumes the guest after the
// faulting instruction (what a Windows vectored handler returning EXCEPTION_CONTINUE_EXECUTION would do).
#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/fextl/memory.h>

#include "Common/HostFeatures.h"

#include <cstdio>
#include <cstring>
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#include <vector>

alignas(64) thread_local char g_beer_tls_alignment_pad[64] __attribute__((used));

using FEXCore::X86State::REG_RSP;

static void MsgHandler(LogMan::DebugLevels Level, const char* Message) {
  fprintf(stderr, "[FEX %s] %s\n", LogMan::DebugLevelStr(Level), Message);
}
static void AssertHandler(const char* Message) {
  fprintf(stderr, "[FEX ASSERT] %s\n", Message);
  fflush(nullptr);
}

namespace {
class BeerSyscallHandler final : public FEXCore::HLE::SyscallHandler {
public:
  void HandleSyscall(FEXCore::Core::CpuStateFrame* Frame) override {
    Frame->State.rip = Frame->State.gregs[FEXCore::X86State::REG_RCX];
  }
  std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return std::nullopt;
  }
  FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return {0, UINT64_MAX, true};
  }
};
// FEXCore fills in the SignalDelegatorConfig (dispatcher addresses, static register mapping); we only expose it.
class BeerSignalDelegator final : public FEXCore::SignalDelegator {};
} // namespace

static FEXCore::Context::Context* g_ctx;
static FEXCore::Core::InternalThreadState* g_thread;
static BeerSignalDelegator* g_signals;

struct HostFprState { // aarch64 fpsimd_context in uc_mcontext.__reserved
  uint32_t magic, size, fpsr, fpcr;
  __uint128_t vregs[32];
};
static constexpr uint32_t kFpsimdMagic = 0x46508001;

// Captured guest context at the fault, for reporting after the run.
struct FaultReport {
  bool valid = false;
  bool in_jit = false;
  uint64_t rip = 0, fault_addr = 0, eflags = 0;
  uint64_t gregs[16] = {};
  uint64_t xmm0_lo = 0;
  int count = 0;
};
static FaultReport g_fault;
static uint64_t g_resume_skip = 0; // bytes to skip after the faulting instruction

static void OnFault(int sig, siginfo_t* si, void* uc_) {
  auto* uc = (ucontext_t*)uc_;
  auto* Thread = g_thread;
  const uint64_t pc = uc->uc_mcontext.pc;
  g_fault.count++;
  if (!g_ctx->IsAddressInCodeBuffer(Thread, pc) || g_fault.count > 1) {
    const char m[] = "[fault] not a guest fault in JIT code (or repeated); giving up\n";
    write(2, m, sizeof m - 1);
    _exit(139);
  }
  g_fault.in_jit = true;
  g_fault.fault_addr = (uint64_t)si->si_addr;

  // ---- SpillSRA: rebuild guest state from the host registers (mirrors FEX's LinuxSyscalls/SignalDelegator.cpp)
  const auto& Config = g_signals->GetConfig();
  auto& State = Thread->CurrentFrame->State;
  State.rip = g_ctx->RestoreRIPFromHostPC(Thread, pc);
  for (size_t i = 0; i < Config.SRAGPRCount; i++) State.gregs[i] = uc->uc_mcontext.regs[Config.SRAGPRMapping[i]];
  auto* fpr = reinterpret_cast<HostFprState*>(&uc->uc_mcontext.__reserved[0]);
  if (fpr->magic == kFpsimdMagic) {
    for (size_t i = 0; i < Config.SRAFPRCount; i++) memcpy(&State.xmm.sse.data[i][0], &fpr->vregs[Config.SRAFPRMapping[i]], 16);
  }
  const uint32_t eflags = g_ctx->ReconstructCompactedEFLAGS(Thread, true, (uint64_t*)uc->uc_mcontext.regs, uc->uc_mcontext.pstate);
  g_ctx->SetFlagsFromCompactedEFLAGS(Thread, eflags);

  g_fault.valid = true;
  g_fault.rip = State.rip;
  g_fault.eflags = eflags;
  memcpy(g_fault.gregs, State.gregs, sizeof(g_fault.gregs));
  memcpy(&g_fault.xmm0_lo, &State.xmm.sse.data[0][0], 8);

  // ---- Resume: like a vectored handler returning EXCEPTION_CONTINUE_EXECUTION after advancing Rip.
  State.rip += g_resume_skip;
  uc->uc_mcontext.pc = Config.AbsoluteLoopTopAddressFillSRA; // dispatcher entry that reloads SRA from State
  uc->uc_mcontext.regs[1] = 0;                               // FillSRA: not single-instruction
  uc->uc_mcontext.regs[28] = (uint64_t)Thread->CurrentFrame; // FEX's state register
}

struct Asm {
  std::vector<uint8_t> b;
  void emit(std::initializer_list<uint8_t> v) { b.insert(b.end(), v.begin(), v.end()); }
  void imm64(uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
};

int main() {
  LogMan::Throw::InstallHandler(AssertHandler);
  LogMan::Msg::InstallHandler(MsgHandler);
  FEXCore::Config::Initialize();
  FEXCore::Config::Load();
  FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
  FEXCore::Config::Set(FEXCore::Config::CONFIG_SMCCHECKS, "0");

  auto HostFeatures = FEX::FetchHostFeatures(true);
  auto CTX = FEXCore::Context::Context::CreateNewContext(HostFeatures);
  CTX->EnableExitOnHLT();
  BeerSyscallHandler Syscalls;
  BeerSignalDelegator Signals;
  CTX->SetSignalDelegator(&Signals);
  CTX->SetSyscallHandler(&Syscalls);
  if (!CTX->InitCore()) { fprintf(stderr, "InitCore failed\n"); return 1; }

  const auto& cfg = Signals.GetConfig();
  fprintf(stderr, "[spike] SRA gpr count=%u fpr count=%u loop-top-fillSRA=0x%llx\n", cfg.SRAGPRCount, cfg.SRAFPRCount,
          (unsigned long long)cfg.AbsoluteLoopTopAddressFillSRA);

  constexpr uint64_t kBase = 0x140001000ull;
  void* code = mmap((void*)kBase, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  void* stack = mmap(nullptr, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (code == MAP_FAILED || stack == MAP_FAILED) { perror("mmap"); return 1; }

  Asm a;
  a.emit({0x48, 0xBB}); a.imm64(0x1111111111111111ull); // mov rbx, imm64
  a.emit({0x48, 0xBE}); a.imm64(0x2222222222222222ull); // mov rsi, imm64
  a.emit({0x49, 0xBC}); a.imm64(0x3333333333333333ull); // mov r12, imm64
  a.emit({0x49, 0xBF}); a.imm64(0x4444444444444444ull); // mov r15, imm64
  a.emit({0x48, 0xB8}); a.imm64(0x5555555555555555ull); // mov rax, imm64
  a.emit({0x66, 0x48, 0x0F, 0x6E, 0xC0});               // movq xmm0, rax
  a.emit({0x31, 0xFF});                                 // xor edi, edi   (null pointer)
  a.emit({0xF9});                                       // stc            (CF = 1)
  const uint64_t fault_off = a.b.size();
  a.emit({0xC7, 0x07, 0x01, 0x00, 0x00, 0x00});         // mov dword [rdi], 1   <- access violation
  const uint64_t after_off = a.b.size();
  a.emit({0x49, 0xBE}); a.imm64(0xFEEDFEEDFEEDFEEDull); // mov r14, imm64  (only reached after resume)
  a.emit({0xF4});                                       // hlt
  memcpy(code, a.b.data(), a.b.size());
  g_resume_skip = after_off - fault_off;

  using CPUState = FEXCore::Core::CPUState;
  auto* Thread = CTX->CreateThread();
  auto* gdt = new CPUState::gdt_segment[32]();
  auto& cs = gdt[CPUState::DEFAULT_USER_CS];
  CPUState::SetGDTBase(&cs, 0);
  CPUState::SetGDTLimit(&cs, 0xFFFFFu);
  cs.L = 1;
  cs.D = 0;
  Thread->CurrentFrame->State.segment_arrays[CPUState::SEGMENT_ARRAY_INDEX_GDT] = &gdt[0];
  Thread->CurrentFrame->State.segment_arrays[CPUState::SEGMENT_ARRAY_INDEX_LDT] = &gdt[0];
  Thread->CurrentFrame->State.cs_idx = CPUState::DEFAULT_USER_CS << 3;
  Thread->CurrentFrame->State.rip = kBase;
  Thread->CurrentFrame->State.gregs[REG_RSP] = (uint64_t)stack + 0x10000 - 0x28;

  g_ctx = CTX.get();
  g_thread = Thread;
  g_signals = &Signals;
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = OnFault;
  sa.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigaction(SIGSEGV, &sa, nullptr);
  sigaction(SIGBUS, &sa, nullptr);

  CTX->ExecuteThread(Thread);

  auto& s = Thread->CurrentFrame->State;
  const uint64_t expect_rip = kBase + fault_off;
  int failures = 0;
  auto check = [&](const char* what, uint64_t got, uint64_t want) {
    const bool ok = got == want;
    if (!ok) ++failures;
    fprintf(stderr, "  %-28s got=0x%016llx want=0x%016llx %s\n", what, (unsigned long long)got, (unsigned long long)want, ok ? "ok" : "MISMATCH");
  };
  fprintf(stderr, "[spike] fault seen: %d, in JIT: %d, fault address=0x%llx\n", g_fault.count, (int)g_fault.in_jit,
          (unsigned long long)g_fault.fault_addr);
  fprintf(stderr, "[spike] guest context rebuilt at the fault:\n");
  check("faulting guest RIP", g_fault.rip, expect_rip);
  check("rbx", g_fault.gregs[FEXCore::X86State::REG_RBX], 0x1111111111111111ull);
  check("rsi", g_fault.gregs[FEXCore::X86State::REG_RSI], 0x2222222222222222ull);
  check("r12", g_fault.gregs[12], 0x3333333333333333ull);
  check("r15", g_fault.gregs[15], 0x4444444444444444ull);
  check("rax", g_fault.gregs[FEXCore::X86State::REG_RAX], 0x5555555555555555ull);
  check("rdi (the null pointer)", g_fault.gregs[FEXCore::X86State::REG_RDI], 0);
  check("xmm0 low 64 bits", g_fault.xmm0_lo, 0x5555555555555555ull);
  fprintf(stderr, "  %-28s 0x%llx (CF bit %s)\n", "EFLAGS", (unsigned long long)g_fault.eflags, (g_fault.eflags & 1) ? "set, ok" : "CLEAR, MISMATCH");
  if (!(g_fault.eflags & 1)) ++failures;
  fprintf(stderr, "[spike] after resume past the faulting instruction:\n");
  check("r14 (set after the fault)", s.gregs[14], 0xFEEDFEEDFEEDFEEDull);
  check("rbx still intact", s.gregs[FEXCore::X86State::REG_RBX], 0x1111111111111111ull);
  check("final RIP (at the hlt)", s.rip, kBase + a.b.size() - 1); // exit-on-HLT leaves RIP on the hlt itself

  const bool ok = failures == 0 && g_fault.count == 1;
  printf("%s\n", ok ? "SPIKE2 PASS: guest fault caught, full context rebuilt, guest resumed" : "SPIKE2 FAIL");
  return ok ? 0 : 2;
}
