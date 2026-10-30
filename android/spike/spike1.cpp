// Beer guest-CPU spike 1: guest -> host calls through the x86 `syscall` instruction, Windows-style
// (mov r10,rcx ; mov eax,N ; syscall), with arguments in registers, a result in RAX, host access to
// guest memory, and a Windows-style TEB reachable through gs:[0x30].
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
#include <vector>

// bionic/arm64 needs a 64-byte aligned PT_TLS segment (see spike0).
alignas(64) thread_local char g_beer_tls_alignment_pad[64] __attribute__((used));

using FEXCore::X86State::REG_RAX;
using FEXCore::X86State::REG_RDX;
using FEXCore::X86State::REG_R10;
using FEXCore::X86State::REG_RSP;

static void MsgHandler(LogMan::DebugLevels Level, const char* Message) {
  fprintf(stderr, "[FEX %s] %s\n", LogMan::DebugLevelStr(Level), Message);
}
static void AssertHandler(const char* Message) {
  fprintf(stderr, "[FEX ASSERT] %s\n", Message);
  fflush(nullptr);
}

static FEXCore::Core::InternalThreadState* g_thread;
static uint64_t g_rcx_after_syscall, g_r11_after_syscall, g_rip_at_syscall;

namespace {
// The host side of an import: id in RAX, Win64-syscall-style args in R10, RDX, R8, R9.
class BeerSyscallHandler final : public FEXCore::HLE::SyscallHandler {
public:
  void HandleSyscall(FEXCore::Core::CpuStateFrame* Frame) override {
    auto& s = Frame->State;
    const uint64_t id = s.gregs[REG_RAX];
    g_rip_at_syscall = s.rip;
    // FEXCore models real `syscall` semantics: State.rip is the syscall instruction itself, RCX the address of the
    // next instruction, R11 the flags. The handler resumes the guest by setting rip = rcx.
    const uint64_t resume = s.gregs[FEXCore::X86State::REG_RCX];
    switch (id) {
    case 1: // add(a, b)
      s.gregs[REG_RAX] = s.gregs[REG_R10] + s.gregs[REG_RDX];
      break;
    case 2: { // print(ptr, len): the host reads guest memory directly (guest address == host address)
      const ssize_t n = write(1, (const void*)s.gregs[REG_R10], (size_t)s.gregs[REG_RDX]);
      s.gregs[REG_RAX] = (uint64_t)n;
      break;
    }
    default:
      fprintf(stderr, "[spike] unknown import %llu\n", (unsigned long long)id);
      s.gregs[REG_RAX] = (uint64_t)-1;
    }
    s.rip = resume;
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

static void DiagFault(int sig, siginfo_t* si, void* uc_) {
  auto* uc = (ucontext_t*)uc_;
  fprintf(stderr, "[diag] signal %d si_addr=%p pc=%p\n", sig, si->si_addr, (void*)uc->uc_mcontext.pc);
  _exit(139);
}

struct Asm {
  std::vector<uint8_t> b;
  void emit(std::initializer_list<uint8_t> v) { b.insert(b.end(), v.begin(), v.end()); }
  void imm32(uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
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

  constexpr uint64_t kBase = 0x140001000ull;
  void* code = mmap((void*)kBase, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  void* stack = mmap(nullptr, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  // A fake TEB: NtTib.Self at +0x30 points to itself, like Windows.
  uint64_t* teb = (uint64_t*)mmap(nullptr, 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (code == MAP_FAILED || stack == MAP_FAILED || teb == MAP_FAILED) { perror("mmap"); return 1; }
  teb[0x30 / 8] = (uint64_t)teb;

  static const char kMsg[] = "hello from guest x86-64 code via FEXCore (import #2)\n";
  Asm a;
  a.emit({0x65, 0x48, 0x8B, 0x1C, 0x25, 0x30, 0x00, 0x00, 0x00}); // mov rbx, gs:[0x30]
  a.emit({0x41, 0xBA}); a.imm32(5);                                // mov r10d, 5
  a.emit({0xBA}); a.imm32(7);                                      // mov edx, 7
  a.emit({0xB8}); a.imm32(1);                                      // mov eax, 1
  a.emit({0x0F, 0x05});                                            // syscall      -> add(5,7)
  a.emit({0x49, 0x89, 0xC4});                                      // mov r12, rax
  a.emit({0x49, 0x89, 0xCD});                                      // mov r13, rcx  (is RCX clobbered by syscall?)
  a.emit({0x4C, 0x8D, 0x15});                                      // lea r10, [rip+msg]
  const size_t lea_fixup = a.b.size();
  a.imm32(0);
  a.emit({0xBA}); a.imm32(sizeof(kMsg) - 1);                       // mov edx, len
  a.emit({0xB8}); a.imm32(2);                                      // mov eax, 2
  a.emit({0x0F, 0x05});                                            // syscall      -> print(msg,len)
  a.emit({0xF4});                                                  // hlt
  const size_t msg_off = a.b.size();
  a.b.insert(a.b.end(), kMsg, kMsg + sizeof(kMsg) - 1);
  const int32_t rel = (int32_t)(msg_off - (lea_fixup + 4));
  memcpy(&a.b[lea_fixup], &rel, 4);
  memcpy(code, a.b.data(), a.b.size());

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
  Thread->CurrentFrame->State.gs_cached = (uint64_t)teb;   // GS base = TEB
  Thread->CurrentFrame->State.rip = kBase;
  Thread->CurrentFrame->State.gregs[REG_RSP] = (uint64_t)stack + 0x10000 - 0x28;

  g_thread = Thread;
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = DiagFault;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, nullptr);
  sigaction(SIGBUS, &sa, nullptr);

  CTX->ExecuteThread(Thread);

  auto& s = Thread->CurrentFrame->State;
  const uint64_t rbx = s.gregs[FEXCore::X86State::REG_RBX];
  const uint64_t r12 = s.gregs[12];
  const uint64_t r13 = s.gregs[13];
  fprintf(stderr, "[spike] rbx(gs:[0x30])=0x%llx expected teb=0x%llx\n", (unsigned long long)rbx, (unsigned long long)teb);
  fprintf(stderr, "[spike] add(5,7) result in r12=%llu\n", (unsigned long long)r12);
  fprintf(stderr, "[spike] rcx after syscall (r13)=0x%llx  (rip at syscall was 0x%llx) -> %s\n", (unsigned long long)r13,
          (unsigned long long)g_rip_at_syscall, r13 == 0 ? "rcx preserved/zero" : "rcx clobbered by syscall");
  const bool ok = rbx == (uint64_t)teb && r12 == 12;
  printf("%s\n", ok ? "SPIKE1 PASS: guest->host calls via syscall, args/results in registers, TEB via gs" : "SPIKE1 FAIL");
  return ok ? 0 : 2;
}
