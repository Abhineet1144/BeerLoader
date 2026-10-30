// Beer guest-CPU spike 4: can a guest `call` to a HOST function address be intercepted universally?
// The decoder is told (QueryGuestExecutableRange) that the host program's own code is not executable for the guest.
// A guest `call` to such an address then makes FEX raise a guest SIGSEGV (state already spilled), which our host
// signal handler turns into a host call: read Win64 arguments from the guest registers, call the host function
// natively, set RAX, pop the return address, resume. This would make every host-built COM vtable entry callable
// from the guest with no per-vtable thunks.
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
#include <time.h>
#include <vector>

alignas(64) thread_local char g_beer_tls_alignment_pad[64] __attribute__((used));

extern "C" char __ehdr_start;
extern "C" char etext;

namespace X = FEXCore::X86State;
using CPUState = FEXCore::Core::CPUState;

static void MsgHandler(LogMan::DebugLevels Level, const char* Message) {
  if (strstr(Message, "NoExec instruction in entry block")) return;   // expected: one per distinct host function
  fprintf(stderr, "[FEX %s] %s\n", LogMan::DebugLevelStr(Level), Message);
}
static void AssertHandler(const char* Message) {
  fprintf(stderr, "[FEX ASSERT] %s\n", Message);
  fflush(nullptr);
}

static FEXCore::Context::Context* g_ctx;
static FEXCore::Core::InternalThreadState* g_thread;
static uint64_t g_host_lo, g_host_hi;
static int g_host_calls, g_other_faults;
static constexpr unsigned kLoop = 100000;
static uint64_t g_t0;
static uint64_t NowNs() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }

// A host function the guest calls through a raw pointer, as it would a host-built COM vtable entry.
extern "C" __attribute__((noinline)) uint64_t host_add_three(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e) {
  return a + b + c + d + e;
}

namespace {
class BeerSyscallHandler final : public FEXCore::HLE::SyscallHandler {
public:
  void HandleSyscall(FEXCore::Core::CpuStateFrame* F) override {
    if (F->State.gregs[X::REG_RAX] == 0) {   // the guest's exit call: report and leave
      const uint64_t r12 = F->State.gregs[12];
      fprintf(stderr, "[spike] host calls intercepted: %d, other faults: %d, r12=%llu\n", g_host_calls, g_other_faults, (unsigned long long)r12);
      const uint64_t dt = NowNs() - g_t0;
      fprintf(stderr, "[spike] %u intercepted host calls in %.1f ms => %.2f us per call\n", (unsigned)g_host_calls, dt / 1e6, dt / 1e3 / (g_host_calls ? g_host_calls : 1));
      const bool ok = g_host_calls == (int)kLoop && r12 == 15;
      printf("%s\n", ok ? "SPIKE4 PASS: a guest call to a raw host function address was intercepted and executed natively"
                        : "SPIKE4 FAIL");
      fflush(nullptr);
      _exit(ok ? 0 : 2);
    }
    F->State.rip = F->State.gregs[X::REG_RCX];
  }
  std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return std::nullopt;
  }
  // Everything is executable for the guest except the host program's own code.
  FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t Address) override {
    if (Address < g_host_lo) return {0, g_host_lo, true};
    if (Address < g_host_hi) return {Address, 0, false};
    return {g_host_hi, UINT64_MAX - g_host_hi, true};
  }
};
class BeerSignalDelegator final : public FEXCore::SignalDelegator {};
BeerSyscallHandler g_syscalls;
BeerSignalDelegator g_signals;
} // namespace

static void OnFault(int sig, siginfo_t* si, void* uc_) {
  auto* uc = (ucontext_t*)uc_;
  const uint64_t pc = uc->uc_mcontext.pc;
  const auto& cfg = g_signals.GetConfig();
  auto* F = g_thread->CurrentFrame;
  auto& st = F->State;
  const bool in_dispatcher = pc >= cfg.DispatcherBegin && pc < cfg.DispatcherEnd;
  const bool in_jit = g_ctx->IsAddressInCodeBuffer(g_thread, pc);
  if (g_host_calls == 0) {
  fprintf(stderr, "[fault] sig=%d si_addr=%p pc=0x%llx in_dispatcher=%d in_jit=%d guest rip=0x%llx\n", sig, si->si_addr,
          (unsigned long long)pc, (int)in_dispatcher, (int)in_jit, (unsigned long long)st.rip);
  fprintf(stderr, "[fault] SynchronousFaultData: ErrorRegister=0x%llx Signal=%d TrapNumber=%d si_code=%d\n",
          (unsigned long long)F->SynchronousFaultData.FaultToTopAndGeneratedException, (int)F->SynchronousFaultData.Signal,
          (int)F->SynchronousFaultData.TrapNo, (int)F->SynchronousFaultData.si_code);
  }

  if (!(in_dispatcher || in_jit)) { _exit(139); }

  if (st.rip >= g_host_lo && st.rip < g_host_hi) {
    // A guest call to a host function. Win64: args in RCX, RDX, R8, R9, then the stack above the return address
    // and 0x20 bytes of shadow space.
    const uint64_t rsp = st.gregs[X::REG_RSP];
    const uint64_t* stk = (const uint64_t*)(rsp + 0x28);
    typedef uint64_t (*Fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    const uint64_t r = ((Fn)st.rip)(st.gregs[X::REG_RCX], st.gregs[X::REG_RDX], st.gregs[X::REG_R8], st.gregs[X::REG_R9], stk[0]);
    st.gregs[X::REG_RAX] = r;
    st.rip = *(const uint64_t*)rsp;       // return address pushed by the guest `call`
    st.gregs[X::REG_RSP] = rsp + 8;
    // The guest CALL pushed an entry on FEX's call/return stack; the RET we are emulating would have popped it.
    st.callret_sp += 0x10;
    ++g_host_calls;
    uc->uc_mcontext.pc = cfg.AbsoluteLoopTopAddressFillSRA;
    uc->uc_mcontext.regs[1] = 0;
    uc->uc_mcontext.regs[28] = (uint64_t)F;
    return;
  }
  ++g_other_faults;
  _exit(139);
}

struct Asm {
  std::vector<uint8_t> b;
  void emit(std::initializer_list<uint8_t> v) { b.insert(b.end(), v.begin(), v.end()); }
  void imm32(uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
  void imm64(uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
};

int main() {
  g_host_lo = (uint64_t)&__ehdr_start;
  g_host_hi = (uint64_t)&etext;
  fprintf(stderr, "[spike] host code range 0x%llx..0x%llx, host_add_three=%p\n", (unsigned long long)g_host_lo,
          (unsigned long long)g_host_hi, (void*)&host_add_three);

  LogMan::Throw::InstallHandler(AssertHandler);
  LogMan::Msg::InstallHandler(MsgHandler);
  FEXCore::Config::Initialize();
  FEXCore::Config::Load();
  FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
  FEXCore::Config::Set(FEXCore::Config::CONFIG_SMCCHECKS, "0");

  auto HostFeatures = FEX::FetchHostFeatures(true);
  auto CTX = FEXCore::Context::Context::CreateNewContext(HostFeatures);
  // NOTE: no EnableExitOnHLT: in that mode FEX's guest-SIGSEGV stub exits cleanly instead of raising the fault.
  g_ctx = CTX.get();
  CTX->SetSignalDelegator(&g_signals);
  CTX->SetSyscallHandler(&g_syscalls);
  if (!CTX->InitCore()) { fprintf(stderr, "InitCore failed\n"); return 1; }

  constexpr uint64_t kBase = 0x140001000ull;
  void* code = mmap((void*)kBase, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  void* stack = mmap(nullptr, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (code == MAP_FAILED || stack == MAP_FAILED) { perror("mmap"); return 1; }

  Asm a;
  a.emit({0xBB}); a.imm32(kLoop);                                // mov ebx, kLoop
  const size_t loop = a.b.size();
  a.emit({0x48, 0xB8}); a.imm64((uint64_t)&host_add_three);     // mov rax, host function
  a.emit({0xB9}); a.imm32(1);                                    // mov ecx, 1
  a.emit({0xBA}); a.imm32(2);                                    // mov edx, 2
  a.emit({0x41, 0xB8}); a.imm32(3);                              // mov r8d, 3
  a.emit({0x41, 0xB9}); a.imm32(4);                              // mov r9d, 4
  a.emit({0x48, 0x83, 0xEC, 0x38});                              // sub rsp, 0x38
  a.emit({0x48, 0xC7, 0x44, 0x24, 0x20, 0x05, 0x00, 0x00, 0x00});// mov qword [rsp+0x20], 5
  a.emit({0xFF, 0xD0});                                          // call rax  -> host_add_three(1,2,3,4,5) = 15
  a.emit({0x48, 0x83, 0xC4, 0x38});                              // add rsp, 0x38
  a.emit({0xFF, 0xCB});                                          // dec ebx
  a.emit({0x0F, 0x85}); a.imm32((uint32_t)(int32_t)(loop - (a.b.size() + 4)));   // jnz loop
  a.emit({0x49, 0x89, 0xC4});                                    // mov r12, rax
  a.emit({0xB8}); a.imm32(0);                                    // mov eax, 0
  a.emit({0x0F, 0x05});                                          // syscall  (exit: handled by the host)
  memcpy(code, a.b.data(), a.b.size());

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
  {
    const size_t sz = FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE, pg = 4096;
    void* base = mmap(nullptr, sz + 2 * pg, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    mprotect((char*)base + pg, sz, PROT_READ | PROT_WRITE);
    Thread->CallRetStackBase = (char*)base + pg;
    Thread->CurrentFrame->State.callret_sp = (uint64_t)Thread->CallRetStackBase + sz / 4;
  }
  Thread->CurrentFrame->State.rip = kBase;
  Thread->CurrentFrame->State.gregs[X::REG_RSP] = ((uint64_t)stack + 0x10000 - 0x40) & ~0xFull;
  g_thread = Thread;

  stack_t ss{};
  ss.ss_sp = mmap(nullptr, 256 * 1024, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ss.ss_size = 256 * 1024;
  sigaltstack(&ss, nullptr);
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = OnFault;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigaction(SIGSEGV, &sa, nullptr);
  sigaction(SIGBUS, &sa, nullptr);
  sigaction(SIGILL, &sa, nullptr);
  sigaction(SIGTRAP, &sa, nullptr);

  g_t0 = NowNs();
  CTX->ExecuteThread(Thread);

  auto& s = Thread->CurrentFrame->State;
  fprintf(stderr, "[spike] host calls intercepted: %d, other faults: %d, r12=%llu\n", g_host_calls, g_other_faults,
          (unsigned long long)s.gregs[12]);
  const bool ok = g_host_calls == 1 && s.gregs[12] == 15;
  printf("%s\n", ok ? "SPIKE4 PASS: a guest call to a raw host function address was intercepted and executed natively"
                    : "SPIKE4 FAIL");
  return ok ? 0 : 2;
}
