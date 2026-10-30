// Beer guest-CPU spike 3: host -> guest calls (nested, re-entrant).
// The guest calls import #1 (syscall thunk). The host handler calls BACK into guest code with six Win64
// arguments (four in registers, two on the stack) via FEXCore's HandleCallback, gets the result, restores the
// guest's registers and returns to the guest. Done twice to check re-entry; guest registers must survive.
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

using namespace FEXCore::X86State;
using CPUState = FEXCore::Core::CPUState;

static void MsgHandler(LogMan::DebugLevels Level, const char* Message) {
  fprintf(stderr, "[FEX %s] %s\n", LogMan::DebugLevelStr(Level), Message);
}
static void AssertHandler(const char* Message) {
  fprintf(stderr, "[FEX ASSERT] %s\n", Message);
  fflush(nullptr);
}

static FEXCore::Context::Context* g_ctx;
static uint64_t g_guest_fn;      // guest function the host calls back
static uint64_t g_cb_ret;        // guest address of the callback-return sentinel (a hlt)
static int g_nested_calls;

// Call a guest function from the host, Win64 style, from inside an import handler.
static uint64_t GuestCall(FEXCore::Core::CpuStateFrame* Frame, uint64_t fn, const uint64_t* args, unsigned n) {
  auto* Thread = Frame->Thread;
  CPUState saved;
  memcpy(&saved, &Frame->State, sizeof saved);

  const unsigned stack_args = n > 4 ? n - 4 : 0;
  // HandleCallback pushes the return address and lowers RSP by 16, then runs `fn` at RIP. We reserve the
  // callee's 0x20-byte shadow space and the stack arguments below the current RSP first, keeping RSP == 8 (mod 16)
  // at function entry as the Win64 ABI requires.
  uint64_t rsp = saved.gregs[REG_RSP] - 0x30 - 8 * ((stack_args + 1) & ~1u);
  rsp = (rsp & ~0xFull) | 8;                       // callback entry rsp = rsp - 16, still == 8 (mod 16)
  uint64_t* stk = (uint64_t*)(rsp + 0x18);         // entry_rsp + 0x28 = first stack argument
  for (unsigned i = 0; i < stack_args; ++i) stk[i] = args[4 + i];

  auto& s = Frame->State;
  s.gregs[REG_RSP] = rsp;
  if (n > 0) s.gregs[REG_RCX] = args[0];
  if (n > 1) s.gregs[REG_RDX] = args[1];
  if (n > 2) s.gregs[REG_R8] = args[2];
  if (n > 3) s.gregs[REG_R9] = args[3];

  g_ctx->HandleCallback(Thread, fn);

  const uint64_t result = Frame->State.gregs[REG_RAX];
  memcpy(&Frame->State, &saved, sizeof saved);    // the guest resumes exactly where it was
  ++g_nested_calls;
  return result;
}

namespace {
class BeerSyscallHandler final : public FEXCore::HLE::SyscallHandler {
public:
  void HandleSyscall(FEXCore::Core::CpuStateFrame* Frame) override {
    auto& s = Frame->State;
    const uint64_t id = s.gregs[REG_RAX];
    const uint64_t resume = s.gregs[REG_RCX];
    uint64_t result = (uint64_t)-1;
    if (id == 1) {
      const uint64_t arg = s.gregs[REG_R10];
      const uint64_t a[6] = {arg, 2, 3, 4, 5, 6};
      result = GuestCall(Frame, g_guest_fn, a, 6) + 100;
    }
    Frame->State.gregs[REG_RAX] = result;
    Frame->State.rip = resume;
  }
  std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return std::nullopt;
  }
  FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return {0, UINT64_MAX, true};
  }
};

class BeerSignalDelegator final : public FEXCore::SignalDelegator {
public:
  // Where a guest function returns to when the host called it: a guest stub holding FEX's CALLBACKRET opcode (0F 3E).
  uintptr_t GetThunkCallbackRET() const override { return g_cb_ret; }
};
} // namespace

static void DiagFault(int sig, siginfo_t* si, void* uc_) {
  auto* uc = (ucontext_t*)uc_;
  fprintf(stderr, "[diag] signal %d si_addr=%p pc=%p\n", sig, si->si_addr, (void*)uc->uc_mcontext.pc);
  _exit(139);
}

struct Asm {
  std::vector<uint8_t> b;
  size_t pos() const { return b.size(); }
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
  g_ctx = CTX.get();
  BeerSyscallHandler Syscalls;
  BeerSignalDelegator Signals;
  CTX->SetSignalDelegator(&Signals);
  CTX->SetSyscallHandler(&Syscalls);

  constexpr uint64_t kBase = 0x140001000ull;
  void* code = mmap((void*)kBase, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  void* stack = mmap(nullptr, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (code == MAP_FAILED || stack == MAP_FAILED) { perror("mmap"); return 1; }

  // Layout: main, import thunk, guest callee, callback-return sentinel.
  Asm a;
  a.emit({0xB9}); a.imm32(41);                       // mov ecx, 41
  a.emit({0xE8}); const size_t call1 = a.pos(); a.imm32(0);  // call thunk
  a.emit({0x49, 0x89, 0xC4});                        // mov r12, rax
  a.emit({0xB9}); a.imm32(7);                        // mov ecx, 7
  a.emit({0xE8}); const size_t call2 = a.pos(); a.imm32(0);  // call thunk
  a.emit({0x49, 0x89, 0xC5});                        // mov r13, rax
  a.emit({0xF4});                                    // hlt
  const size_t thunk = a.pos();
  a.emit({0x49, 0x89, 0xCA});                        // mov r10, rcx
  a.emit({0xB8}); a.imm32(1);                        // mov eax, 1
  a.emit({0x0F, 0x05});                              // syscall
  a.emit({0xC3});                                    // ret
  const size_t callee = a.pos();                     // rax = a+b+c+d+e+f (e, f on the stack)
  a.emit({0x48, 0x8D, 0x04, 0x11});                  // lea rax, [rcx+rdx]
  a.emit({0x4C, 0x01, 0xC0});                        // add rax, r8
  a.emit({0x4C, 0x01, 0xC8});                        // add rax, r9
  a.emit({0x48, 0x03, 0x44, 0x24, 0x28});            // add rax, [rsp+0x28]
  a.emit({0x48, 0x03, 0x44, 0x24, 0x30});            // add rax, [rsp+0x30]
  a.emit({0xC3});                                    // ret
  const size_t sentinel = a.pos();
  a.emit({0x0F, 0x3E});                              // FEX CALLBACKRET: a guest function called from the host returns here
  int32_t rel = (int32_t)(thunk - (call1 + 4)); memcpy(&a.b[call1], &rel, 4);
  rel = (int32_t)(thunk - (call2 + 4));         memcpy(&a.b[call2], &rel, 4);
  memcpy(code, a.b.data(), a.b.size());
  g_guest_fn = kBase + callee;
  g_cb_ret = kBase + sentinel;

  if (!CTX->InitCore()) { fprintf(stderr, "InitCore failed\n"); return 1; }

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
  // The JIT keeps a call/return stack per thread (guest CALL pushes, RET pops) through State.callret_sp. The embedder
  // must provide it: a guarded region with the pointer started a quarter of the way in (as FEX's Windows port does).
  {
    const size_t sz = FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE, pg = 4096;
    void* base = mmap(nullptr, sz + 2 * pg, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    mprotect((char*)base + pg, sz, PROT_READ | PROT_WRITE);
    Thread->CallRetStackBase = (char*)base + pg;
    Thread->CurrentFrame->State.callret_sp = (uint64_t)Thread->CallRetStackBase + sz / 4;
  }
  Thread->CurrentFrame->State.rip = kBase;
  Thread->CurrentFrame->State.gregs[REG_RSP] = ((uint64_t)stack + 0x10000 - 0x40) & ~0xFull;   // 16-aligned, like at a call site
  Thread->CurrentFrame->State.gregs[REG_RBX] = 0xB0B0B0B0B0B0B0B0ull;                         // must survive the nested calls
  Thread->CurrentFrame->State.gregs[REG_R14] = 0x1414141414141414ull;

  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = DiagFault;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, nullptr);
  sigaction(SIGBUS, &sa, nullptr);

  CTX->ExecuteThread(Thread);

  auto& s = Thread->CurrentFrame->State;
  int failures = 0;
  auto check = [&](const char* what, uint64_t got, uint64_t want) {
    const bool ok = got == want;
    if (!ok) ++failures;
    fprintf(stderr, "  %-34s got=%-20llu want=%-20llu %s\n", what, (unsigned long long)got, (unsigned long long)want, ok ? "ok" : "MISMATCH");
  };
  fprintf(stderr, "[spike] nested host->guest calls made: %d\n", g_nested_calls);
  check("import #1 -> 41+2+3+4+5+6 + 100", s.gregs[12], 41 + 2 + 3 + 4 + 5 + 6 + 100);
  check("import #2 ->  7+2+3+4+5+6 + 100", s.gregs[13], 7 + 2 + 3 + 4 + 5 + 6 + 100);
  check("rbx survived (hex compare)", s.gregs[REG_RBX] == 0xB0B0B0B0B0B0B0B0ull, 1);
  check("r14 survived (hex compare)", s.gregs[14] == 0x1414141414141414ull, 1);
  check("outer guest rsp restored", s.gregs[REG_RSP], ((uint64_t)stack + 0x10000 - 0x40) & ~0xFull);
  check("final rip is the hlt in main", s.rip, kBase + thunk - 1);
  printf("%s\n", failures == 0 && g_nested_calls == 2 ? "SPIKE3 PASS: nested host->guest calls with stack args, state preserved"
                                                       : "SPIKE3 FAIL");
  return failures == 0 ? 0 : 2;
}
