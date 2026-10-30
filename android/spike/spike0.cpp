// Beer guest-CPU spike 0: run raw x86-64 code through FEXCore on bionic (Android/Termux),
// with no Linux emulation frontend. Guest code: mov eax, 42 ; hlt   -> expect RAX == 42.
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
#include <dlfcn.h>
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

// bionic/arm64 requires the executable's PT_TLS segment to be 64-byte aligned; FEX's thread-locals are
// only 8-byte aligned, so force the segment alignment with one over-aligned thread-local.
alignas(64) thread_local char g_beer_tls_alignment_pad[64] __attribute__((used));

static void MsgHandler(LogMan::DebugLevels Level, const char* Message) {
  fprintf(stderr, "[FEX %s] %s\n", LogMan::DebugLevelStr(Level), Message);
}

static void AssertHandler(const char* Message) {
  fprintf(stderr, "[FEX ASSERT] %s\n", Message);
  fflush(nullptr);
}

static FEXCore::Context::Context* g_ctx;
static FEXCore::Core::InternalThreadState* g_thread;

// Diagnostic only: report where a fault happened and whether it was inside FEX's JIT code.
static void DiagFault(int sig, siginfo_t* si, void* uc_) {
  auto* uc = (ucontext_t*)uc_;
  const uintptr_t pc = uc->uc_mcontext.pc;
  Dl_info di {};
  const int ok = dladdr((void*)pc, &di);
  fprintf(stderr, "[diag] signal %d si_addr=%p pc=%p lr=%p sp=%p si_code=%d\n", sig, si->si_addr, (void*)pc,
          (void*)uc->uc_mcontext.regs[30], (void*)uc->uc_mcontext.sp, si->si_code);
  fprintf(stderr, "[diag] pc in: %s %s\n", ok && di.dli_fname ? di.dli_fname : "?", ok && di.dli_sname ? di.dli_sname : "(no symbol)");
  Dl_info dl {};
  if (dladdr((void*)uc->uc_mcontext.regs[30], &dl) && dl.dli_fbase)
    fprintf(stderr, "[diag] pc offset=0x%llx lr offset=0x%llx\n", (unsigned long long)(pc - (uintptr_t)di.dli_fbase),
            (unsigned long long)(uc->uc_mcontext.regs[30] - (uintptr_t)dl.dli_fbase));
  if (g_ctx && g_thread) {
    fprintf(stderr, "[diag] pc in FEX JIT code buffer: %d, guest rip=0x%llx\n", (int)g_ctx->IsAddressInCodeBuffer(g_thread, pc),
            (unsigned long long)g_thread->CurrentFrame->State.rip);
  }
  for (int i = 0; i < 28; i += 4) {
    fprintf(stderr, "[diag] x%-2d=%016llx x%-2d=%016llx x%-2d=%016llx x%-2d=%016llx\n", i, (unsigned long long)uc->uc_mcontext.regs[i],
            i + 1, (unsigned long long)uc->uc_mcontext.regs[i + 1], i + 2, (unsigned long long)uc->uc_mcontext.regs[i + 2], i + 3,
            (unsigned long long)uc->uc_mcontext.regs[i + 3]);
  }
  _exit(139);
}

namespace {
class BeerSyscallHandler final : public FEXCore::HLE::SyscallHandler {
public:
  void HandleSyscall(FEXCore::Core::CpuStateFrame* Frame) override {
    fprintf(stderr, "[spike] guest syscall rax=%llu\n", (unsigned long long)Frame->State.gregs[FEXCore::X86State::REG_RAX]);
  }
  std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return std::nullopt;
  }
  FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return {0, UINT64_MAX, true};
  }
};

class BeerSignalDelegator final : public FEXCore::SignalDelegator {};
} // namespace

int main() {
  LogMan::Throw::InstallHandler(AssertHandler);
  LogMan::Msg::InstallHandler(MsgHandler);

  FEXCore::Config::Initialize();
  FEXCore::Config::Load();
  FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
  FEXCore::Config::Set(FEXCore::Config::CONFIG_SMCCHECKS, "0");   // no write-fault based SMC tracking yet

  fprintf(stderr, "[spike] fetching host features\n");
  auto HostFeatures = FEX::FetchHostFeatures(true);
  fprintf(stderr, "[spike] creating context\n");
  auto CTX = FEXCore::Context::Context::CreateNewContext(HostFeatures);
  CTX->EnableExitOnHLT();

  BeerSyscallHandler Syscalls;
  BeerSignalDelegator Signals;
  CTX->SetSignalDelegator(&Signals);
  CTX->SetSyscallHandler(&Syscalls);

  fprintf(stderr, "[spike] InitCore\n");
  if (!CTX->InitCore()) {
    fprintf(stderr, "[spike] InitCore failed\n");
    return 1;
  }

  // Guest code at a PE-like address, and a stack.
  void* code = mmap((void*)0x140001000ull, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  void* stack = mmap(nullptr, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (code == MAP_FAILED || stack == MAP_FAILED) {
    perror("mmap");
    return 1;
  }
  static const unsigned char guest[] = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xF4}; // mov eax,42 ; hlt
  memcpy(code, guest, sizeof guest);

  fprintf(stderr, "[spike] CreateThread\n");
  auto* Thread = CTX->CreateThread();

  // Segment setup, modelled on FEX's ARM64EC module (Source/Windows/ARM64EC/Module.cpp): the decoder reads the
  // CS descriptor to pick 64-bit mode, so the thread needs a descriptor table with a long-mode code segment.
  using CPUState = FEXCore::Core::CPUState;
  auto* gdt = new CPUState::gdt_segment[32]();
  auto& cs = gdt[CPUState::DEFAULT_USER_CS];
  CPUState::SetGDTBase(&cs, 0);
  CPUState::SetGDTLimit(&cs, 0xFFFFFu);
  cs.L = 1; // long mode
  cs.D = 0;
  Thread->CurrentFrame->State.segment_arrays[CPUState::SEGMENT_ARRAY_INDEX_GDT] = &gdt[0];
  Thread->CurrentFrame->State.segment_arrays[CPUState::SEGMENT_ARRAY_INDEX_LDT] = &gdt[0];
  Thread->CurrentFrame->State.cs_idx = CPUState::DEFAULT_USER_CS << 3;
  Thread->CurrentFrame->State.gs_cached = 0; // would be the TEB address for a PE guest

  Thread->CurrentFrame->State.rip = 0x140001000ull;
  Thread->CurrentFrame->State.gregs[FEXCore::X86State::REG_RSP] = (uint64_t)stack + 0x10000 - 0x28;

  g_ctx = CTX.get();
  g_thread = Thread;
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = DiagFault;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, nullptr);
  sigaction(SIGBUS, &sa, nullptr);
  sigaction(SIGILL, &sa, nullptr);

  fprintf(stderr, "[spike] ExecuteThread\n");
  CTX->ExecuteThread(Thread);

  const uint64_t rax = Thread->CurrentFrame->State.gregs[FEXCore::X86State::REG_RAX];
  fprintf(stderr, "[spike] returned: rax=%llu rip=0x%llx\n", (unsigned long long)rax, (unsigned long long)Thread->CurrentFrame->State.rip);
  printf("%s\n", rax == 42 ? "SPIKE0 PASS: guest x86-64 code executed through FEXCore on bionic" : "SPIKE0 FAIL: wrong result");
  return rax == 42 ? 0 : 2;
}
