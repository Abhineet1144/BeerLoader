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

#include <atomic>
#include <cerrno>
#include <pthread.h>
#include <dlfcn.h>
#include <link.h>
#include <malloc.h>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <map>
#include <set>
#include <vector>
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

// ---- Self-modifying code detection (SMCChecks=MTRACK, the default) ---------------------------------------------
// Modelled on FEX's Windows InvalidationTracker. A guest page FEX translates code from is write-protected if the
// guest may write it; the first write (guest or host code) faults, its translations are discarded and the page gets its
// protection back. Protection changes, frees and kernel writes (file reads) go through beer_guest_memory_will_change().
// Lock order: FEX's code invalidation mutex, then g_smc_lock.
bool g_smc = false;
std::mutex g_smc_lock;
std::set<uint64_t> g_smc_code_pages;           // pages FEX has translated code from
std::map<uint64_t, int> g_smc_trapped;         // write-protected pages -> their protection before
struct SmcMapRange { uint64_t lo, hi; int prot; };
std::vector<SmcMapRange> g_smc_maps;           // /proc/self/maps snapshot, refreshed after protection changes
bool g_smc_maps_dirty = true;
std::mutex g_threads_lock;
std::vector<FEXCore::Core::InternalThreadState*> g_threads;   // every guest thread state (FEX threads are never freed)
std::vector<pid_t> g_thread_tids;                              // their host thread ids (register dumps)
constexpr uint64_t kPage = 4096;

void SmcRefreshMaps() {
  g_smc_maps.clear();
  if (FILE* f = fopen("/proc/self/maps", "re")) {
    char line[512];
    while (fgets(line, sizeof line, f)) {
      unsigned long lo, hi;
      char perms[8];
      if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) != 3) continue;
      const int prot = (perms[0] == 'r' ? PROT_READ : 0) | (perms[1] == 'w' ? PROT_WRITE : 0) | (perms[2] == 'x' ? PROT_EXEC : 0);
      g_smc_maps.push_back({lo, hi, prot});
    }
    fclose(f);
  }
  g_smc_maps_dirty = false;
}

int SmcPageProt(uint64_t page) {   // -1 = not mapped
  size_t lo = 0, hi = g_smc_maps.size();
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (g_smc_maps[mid].hi <= page) lo = mid + 1;
    else hi = mid;
  }
  return lo < g_smc_maps.size() && g_smc_maps[lo].lo <= page ? g_smc_maps[lo].prot : -1;
}

void SmcInvalidateLocked(uint64_t start, uint64_t length) {   // code invalidation mutex held exclusively
  g_ctx->InvalidateCodeBuffersCodeRange(start, length);
  std::scoped_lock lk(g_threads_lock);
  for (auto* t : g_threads) g_ctx->InvalidateThreadCachedCodeRange(t, start, length);
}

void SmcMarkCodePage(FEXCore::Core::InternalThreadState*, uint64_t start, uint64_t length) {
  if (!g_smc) return;
  std::scoped_lock lk(g_smc_lock);
  for (uint64_t page = start & ~(kPage - 1); page < start + length; page += kPage) {
    if (page >= g_host_lo && page < g_host_hi) continue;
    g_smc_code_pages.insert(page);
    if (g_smc_trapped.count(page)) continue;
    if (g_smc_maps_dirty) SmcRefreshMaps();
    const int prot = SmcPageProt(page);   // host protection == guest protection (beer_virtual_protect uses mprotect)
    if (prot >= 0 && (prot & PROT_WRITE) && mprotect((void*)page, kPage, prot & ~PROT_WRITE) == 0)
      g_smc_trapped[page] = prot;   // if mprotect fails the page stays untracked (as with SMC detection off)
  }
}

// A write fault on a trapped page: discard its code and let the write through. Called from the signal handler.
bool SmcHandleWriteFault(uint64_t address) {
  if (!g_smc) return false;
  const uint64_t page = address & ~(kPage - 1);
  {   // cheap pre-check without the code mutex
    std::scoped_lock lk(g_smc_lock);
    if (!g_smc_trapped.count(page)) return false;
  }
  std::scoped_lock code(g_ctx->GetCodeInvalidationMutex());
  std::scoped_lock lk(g_smc_lock);
  auto it = g_smc_trapped.find(page);
  if (it != g_smc_trapped.end()) {   // another thread may have handled it meanwhile
    SmcInvalidateLocked(page, kPage);
    mprotect((void*)page, kPage, it->second);
    g_smc_trapped.erase(it);
    g_smc_code_pages.erase(page);
  }
  return true;
}

// ---- Guest thunks for host functions reached through vtables -------------------------------------------------
// A guest call to a host address (a COM method of a Beer object) faults and is turned into a host call in DeliverFault:
// correct, but one signal per call. The first such call through a memory slot (`call [reg+disp]`, the vtable entry)
// rewrites the slot to a guest thunk `mov r10,rcx ; mov eax,kThunkIdBase|n ; syscall ; ret`, which HandleSyscall
// serves like an import. Vtables are shared per class, so one fault fixes the method for every object. The thunk area
// is not executable for the host: host code calling through a patched slot faults in HostSignal and continues at the
// real function. BEER_GUEST_THUNKS=0 keeps every call on the fault path.
constexpr uint32_t kThunkIdBase = 0x10000000u;
constexpr uint32_t kThunkSize = 16, kMaxThunks = 65536;
uint8_t* g_thunk_area = nullptr;               // kMaxThunks * kThunkSize bytes, PROT_READ|PROT_WRITE
std::atomic<uint64_t> g_thunk_fn[kMaxThunks];  // host function behind each thunk
std::atomic<uint32_t> g_thunk_count{0};
std::mutex g_thunk_lock;
std::map<uint64_t, uint32_t> g_thunk_of;       // host function -> thunk index
bool g_thunks_enabled = true;

bool IsThunkAddress(uint64_t a) {
  return g_thunk_area && a >= (uint64_t)g_thunk_area && a < (uint64_t)g_thunk_area + (uint64_t)kMaxThunks * kThunkSize;
}

uint64_t ThunkFor(uint64_t fn) {   // 0 if no thunk can be made
  std::scoped_lock lk(g_thunk_lock);
  if (!g_thunk_area) {
    void* area = mmap(nullptr, (size_t)kMaxThunks * kThunkSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (area == MAP_FAILED) return 0;
    g_thunk_area = (uint8_t*)area;
  }
  auto it = g_thunk_of.find(fn);
  if (it != g_thunk_of.end()) return (uint64_t)g_thunk_area + (uint64_t)it->second * kThunkSize;
  const uint32_t n = g_thunk_count.load();
  if (n >= kMaxThunks) return 0;
  g_thunk_fn[n].store(fn);
  uint8_t* t = g_thunk_area + (size_t)n * kThunkSize;
  const uint32_t id = kThunkIdBase | n;
  const uint8_t code[] = {0x49, 0x89, 0xCA,                                           // mov r10, rcx
                          0xB8, (uint8_t)id, (uint8_t)(id >> 8), (uint8_t)(id >> 16), (uint8_t)(id >> 24),   // mov eax, id
                          0x0F, 0x05, 0xC3};                                          // syscall ; ret
  memcpy(t, code, sizeof code);   // a fresh slot: no translated code to invalidate
  g_thunk_of[fn] = n;
  g_thunk_count.store(n + 1);
  return (uint64_t)t;
}

// The memory slot read by the guest `call qword ptr [...]` (FF /2) that ends at `ret`, or 0. `regs` are the guest
// registers at the call (RSP still includes the pushed return address).
uint64_t CallSlot(const CPUState& st, uint64_t ret) {
  for (uint32_t len = 2; len <= 8; ++len) {
    const uint8_t* p = (const uint8_t*)(ret - len);
    uint32_t i = 0, rex = 0;
    if ((p[i] & 0xF0) == 0x40) rex = p[i++];
    if (p[i++] != 0xFF) continue;
    const uint8_t modrm = p[i++];
    const uint32_t mod = modrm >> 6, reg = (modrm >> 3) & 7, rm = modrm & 7;
    if (reg != 2 || mod == 3) continue;
    auto gpr = [&](uint32_t n) -> uint64_t {
      return n == 4 ? st.gregs[X::REG_RSP] + 8 : st.gregs[n];   // RSP as it was before the call pushed ret
    };
    uint64_t ea = 0;
    if (rm == 4) {   // SIB
      const uint8_t sib = p[i++];
      const uint32_t scale = sib >> 6, index = ((sib >> 3) & 7) | ((rex & 2) << 2), base = (sib & 7) | ((rex & 1) << 3);
      if (index != 4) ea += gpr(index) << scale;
      if ((sib & 7) == 5 && mod == 0) { int32_t d; memcpy(&d, p + i, 4); i += 4; ea += (int64_t)d; }
      else ea += gpr(base);
    } else if (rm == 5 && mod == 0) {   // RIP-relative
      int32_t d; memcpy(&d, p + i, 4); i += 4;
      ea = ret + (int64_t)d;
    } else {
      ea = gpr(rm | ((rex & 1) << 3));
    }
    if (mod == 1) { ea += (int64_t)(int8_t)p[i]; i += 1; }
    else if (mod == 2) { int32_t d; memcpy(&d, p + i, 4); ea += (int64_t)d; i += 4; }
    if (i == len) return ea;
  }
  return 0;
}

// Points every slot holding `fn` in the loader's .data and .data.rel.ro (where the static COM vtables live) at its
// thunk. Used once per function when the faulting call went through a register (`call rax`), which names no slot.
// Only those two sections: .got holds the same addresses for the loader's own calls to exported functions, and .bss
// holds the thunk and statistics tables.
std::set<uint64_t> g_scanned_fns;
std::vector<std::pair<uint64_t, uint64_t>> g_vtable_sections;   // [start, end) in memory

void FindVtableSections() {
  Dl_info self{};
  if (!dladdr((void*)&FindVtableSections, &self) || !self.dli_fname) return;
  FILE* f = fopen(self.dli_fname, "rb");
  if (!f) return;
  ElfW(Ehdr) eh{};
  std::vector<ElfW(Shdr)> sh;
  std::vector<char> names;
  if (fread(&eh, sizeof eh, 1, f) == 1 && eh.e_shentsize == sizeof(ElfW(Shdr)) && eh.e_shstrndx < eh.e_shnum) {
    sh.resize(eh.e_shnum);
    if (fseek(f, (long)eh.e_shoff, SEEK_SET) == 0 && fread(sh.data(), sizeof(ElfW(Shdr)), sh.size(), f) == sh.size()) {
      names.resize(sh[eh.e_shstrndx].sh_size + 1);
      if (fseek(f, (long)sh[eh.e_shstrndx].sh_offset, SEEK_SET) != 0 ||
          fread(names.data(), 1, names.size() - 1, f) != names.size() - 1)
        names.clear();
    }
  }
  fclose(f);
  for (const auto& s : sh) {
    if (names.empty() || s.sh_name >= names.size() - 1) continue;
    const char* name = names.data() + s.sh_name;
    if (strcmp(name, ".data") && strcmp(name, ".data.rel.ro")) continue;
    const uint64_t lo = (uint64_t)self.dli_fbase + s.sh_addr;
    g_vtable_sections.push_back({lo, lo + s.sh_size});
  }
}

void PatchAllSlots(uint64_t fn) {
  {
    std::scoped_lock lk(g_thunk_lock);
    if (!g_scanned_fns.insert(fn).second) return;   // already scanned
    static bool found = false;
    if (!found) { FindVtableSections(); found = true; }
  }
  const uint64_t thunk = ThunkFor(fn);
  if (!thunk) return;
  for (const auto& [lo, hi] : g_vtable_sections) {
    for (uint64_t a = (lo + 7) & ~7ull; a + 8 <= hi; a += 8) {
      if (*(const uint64_t*)a != fn) continue;
      if (mprotect((void*)(a & ~(kPage - 1)), kPage, PROT_READ | PROT_WRITE) != 0) continue;
      uint64_t expected = fn;
      __atomic_compare_exchange_n((uint64_t*)a, &expected, thunk, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    }
  }
}

// After a faulting call from the guest to host function `fn`: point the slot it came from at a thunk.
void PatchCallSlot(const CPUState& st, uint64_t fn) {
  if (!g_thunks_enabled) return;
  const uint64_t ret = *(const uint64_t*)st.gregs[X::REG_RSP];
  const uint64_t slot = CallSlot(st, ret);
  // Only vtables inside the loader's own image are patched (static COM vtables, some in read-only .data.rel.ro):
  // their pages can safely be made writable. A slot anywhere else keeps the fault path. Checked before reading the
  // slot: a `call reg` can mis-decode as a memory form with a meaningless address.
  Dl_info where{};
  if (!slot || (slot & 7) || slot < g_host_hi || !dladdr((void*)slot, &where) || where.dli_fbase != (void*)&__ehdr_start ||
      *(const uint64_t*)slot != fn) {
    PatchAllSlots(fn);   // a register call (or a slot outside the loader): find the vtable entries by value
    return;
  }
  const uint64_t thunk = ThunkFor(fn);
  if (!thunk) return;
  const uint64_t page = slot & ~(kPage - 1);
  if (mprotect((void*)page, kPage, PROT_READ | PROT_WRITE) != 0) return;
  uint64_t expected = fn;
  __atomic_compare_exchange_n((uint64_t*)slot, &expected, thunk, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}

// BEER_GUESTCPU_STATS=1: every 5 s, guest->host calls per second by path (syscall imports vs faulting calls into host
// code such as COM vtables) and the busiest host functions of the faulting path (libbeer.so offsets for llvm-symbolizer).
bool g_stats = false;
std::atomic<uint64_t> g_stat_imports{0}, g_stat_host_calls{0};
constexpr uint32_t kStatSlots = 1024;
std::atomic<uint64_t> g_stat_fn[kStatSlots], g_stat_count[kStatSlots];
void StatHostCall(uint64_t fn) {
  g_stat_host_calls.fetch_add(1, std::memory_order_relaxed);
  for (uint32_t i = 0, slot = (uint32_t)((fn >> 4) * 0x9E3779B1u) % kStatSlots; i < kStatSlots; ++i, slot = (slot + 1) % kStatSlots) {
    uint64_t seen = g_stat_fn[slot].load(std::memory_order_relaxed);
    if (!seen && g_stat_fn[slot].compare_exchange_strong(seen, fn)) seen = fn;
    if (seen == fn) { g_stat_count[slot].fetch_add(1, std::memory_order_relaxed); return; }
  }
}
void* StatReporter(void*) {
  Dl_info self{};
  dladdr((void*)&StatHostCall, &self);
  const uint64_t base = (uint64_t)self.dli_fbase;
  uint64_t last_imports = 0, last_calls = 0;
  for (;;) {
    sleep(5);
    const uint64_t imports = g_stat_imports.load(), calls = g_stat_host_calls.load();
    fprintf(stderr, "[GUESTCPU STATS] imports/s=%llu host-calls/s=%llu (faulting path)\n",
            (unsigned long long)(imports - last_imports) / 5, (unsigned long long)(calls - last_calls) / 5);
    last_imports = imports;
    last_calls = calls;
    for (int top = 0; top < 12; ++top) {   // busiest functions of the last interval; counters reset as reported
      uint32_t best = kStatSlots;
      uint64_t best_count = 0;
      for (uint32_t i = 0; i < kStatSlots; ++i) {
        const uint64_t c = g_stat_count[i].load(std::memory_order_relaxed);
        if (c > best_count) { best_count = c; best = i; }
      }
      if (best == kStatSlots) break;
      const uint64_t fn = g_stat_fn[best].load();
      g_stat_count[best].fetch_sub(best_count, std::memory_order_relaxed);
      Dl_info info{};
      dladdr((void*)fn, &info);
      fprintf(stderr, "[GUESTCPU STATS]   %8llu/s  +0x%llx %s\n", (unsigned long long)best_count / 5,
              (unsigned long long)(fn - base), info.dli_sname ? info.dli_sname : "");
    }
  }
  return nullptr;
}

void SmcMarkCodePage(FEXCore::Core::InternalThreadState* Thread, uint64_t Start, uint64_t Length);

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
  // SMCChecks=MTRACK: FEX reports each guest page it starts translating code from (called with the code
  // invalidation mutex held shared).
  void MarkGuestExecutableRange(FEXCore::Core::InternalThreadState* Thread, uint64_t Start, uint64_t Length) override {
    SmcMarkCodePage(Thread, Start, Length);
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
  {
    std::scoped_lock lk(g_threads_lock);
    g_threads.push_back(gt.ts);
    g_thread_tids.push_back((pid_t)syscall(SYS_gettid));
  }
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
  // Host code called a vtable slot that now holds a guest thunk: run the real host function instead.
  if (IsThunkAddress(((ucontext_t*)uc_)->uc_mcontext.pc)) {   // SIGSEGV or SIGILL, depending on the kernel
    auto* huc = (ucontext_t*)uc_;
    huc->uc_mcontext.pc = g_thunk_fn[(huc->uc_mcontext.pc - (uint64_t)g_thunk_area) / kThunkSize].load();
    return;
  }
  // A write to a page write-protected for SMC detection, by guest or host code: discard its code, retry the write.
  if (sig == SIGSEGV && si->si_code == SEGV_ACCERR && SmcHandleWriteFault((uint64_t)si->si_addr)) return;
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

// Thread dump (BEER_THREAD_DUMP): print the interrupted thread's guest registers when it is inside JIT code.
const int kRegsSignal = SIGRTMIN + 10;
void GuestRegsSignal(int, siginfo_t*, void* uc_) {
  auto* uc = (ucontext_t*)uc_;
  GuestThread& gt = t_gt;
  const uint64_t pc = uc->uc_mcontext.pc;
  char line[512];
  int n;
  if (!gt.ts || !g_ctx->IsAddressInCodeBuffer(gt.ts, pc)) {
    n = snprintf(line, sizeof line, "[GUEST-REGS] tid=%d not in guest code (host pc=0x%llx)\n", (int)syscall(SYS_gettid),
                 (unsigned long long)pc);
  } else {
    const auto& cfg = g_signals.GetConfig();
    uint64_t r[16] = {};
    for (size_t i = 0; i < cfg.SRAGPRCount && i < 16; i++) r[i] = uc->uc_mcontext.regs[cfg.SRAGPRMapping[i]];
    n = snprintf(line, sizeof line,
                 "[GUEST-REGS] tid=%d rip=0x%llx rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx rbp=%llx rsp=%llx "
                 "r8=%llx r9=%llx r10=%llx r11=%llx r12=%llx r13=%llx r14=%llx r15=%llx\n",
                 (int)syscall(SYS_gettid), (unsigned long long)g_ctx->RestoreRIPFromHostPC(gt.ts, pc),
                 (unsigned long long)r[X::REG_RAX], (unsigned long long)r[X::REG_RBX], (unsigned long long)r[X::REG_RCX],
                 (unsigned long long)r[X::REG_RDX], (unsigned long long)r[X::REG_RSI], (unsigned long long)r[X::REG_RDI],
                 (unsigned long long)r[X::REG_RBP], (unsigned long long)r[X::REG_RSP], (unsigned long long)r[X::REG_R8],
                 (unsigned long long)r[X::REG_R9], (unsigned long long)r[X::REG_R10], (unsigned long long)r[X::REG_R11],
                 (unsigned long long)r[X::REG_R12], (unsigned long long)r[X::REG_R13], (unsigned long long)r[X::REG_R14],
                 (unsigned long long)r[X::REG_R15]);
  }
  if (n > 0) (void)!write(2, line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
}

void InstallSignals() {
  {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = GuestRegsSignal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(kRegsSignal, &sa, nullptr);
  }
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
      if (__builtin_expect(g_stats, 0)) StatHostCall(faulting.rip);
      PatchCallSlot(faulting, faulting.rip);
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

  if (__builtin_expect(g_stats, 0)) g_stat_imports.fetch_add(1, std::memory_order_relaxed);
  Frame* prev_frame = gt.import_frame;
  jmp_buf* prev_jb = gt.resume_jb;
  jmp_buf jb;
  gt.import_frame = F;
  gt.resume_jb = &jb;
  if (_setjmp(jb) == 0) {
    const uint32_t thunk = (uint32_t)id - kThunkIdBase;
    const uint64_t result = thunk < kMaxThunks && (uint32_t)id >= kThunkIdBase
        ? beer_guest_host_call(g_thunk_fn[thunk].load(std::memory_order_relaxed), a, x)
        : beer_guest_import_dispatch(id, a, x, rsp);
    F->State.gregs[X::REG_RAX] = result;
    F->State.rip = resume;
  }
  // else: beer_guest_resume() loaded a new guest context into State and jumped here; State.rip is already set.
  gt.import_frame = prev_frame;
  gt.resume_jb = prev_jb;
}

// ---- context initialisation --------------------------------------------------------------------------------
void InitOnce() {
  g_thunks_enabled = !getenv("BEER_GUEST_THUNKS") || atoi(getenv("BEER_GUEST_THUNKS")) != 0;
  if (getenv("BEER_GUESTCPU_STATS")) {
    g_stats = true;
    pthread_t reporter;
    if (pthread_create(&reporter, nullptr, StatReporter, nullptr) == 0) pthread_detach(reporter);
  }
  LogMan::Throw::InstallHandler(AssertHandler);
  LogMan::Msg::InstallHandler(MsgHandler);
  FEXCore::Config::Initialize();
  FEXCore::Config::Load();
  FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
  // Self-modifying code: write-fault tracking (SmcMarkCodePage / SmcHandleWriteFault). BEER_FEX_SMCCHECKS=0 turns it off.
  FEXCore::Config::Set(FEXCore::Config::CONFIG_SMCCHECKS, "1");
  // BEER_FEX_<OPTION>=<value> sets any FEXCore option (e.g. BEER_FEX_TSOENABLED=0, BEER_FEX_MAXINST=500) for
  // measuring emulation trade-offs; launch.txt passes it as env:BEER_FEX_...=...
  {
    static const struct { const char* env; FEXCore::Config::ConfigOption option; } kOptions[] = {
#define OPT_BASE(type, group, enum, json, default) {"BEER_FEX_" #enum, FEXCore::Config::CONFIG_##enum},
#include <FEXCore/Config/ConfigValues.inl>
    };
    for (const auto& o : kOptions)
      if (const char* value = getenv(o.env)) {
        FEXCore::Config::Set(o.option, value);
        fprintf(stderr, "[GUESTCPU] FEX option %s = %s\n", o.env + 9, value);
      }
  }
  g_smc = !getenv("BEER_FEX_SMCCHECKS") || atoi(getenv("BEER_FEX_SMCCHECKS")) == FEXCore::Config::CONFIG_SMC_MTRACK;

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

void beer_guest_dump_registers(void) {   // async-signal-safe enough for a diagnostic: no allocation, only tgkill
  const pid_t pid = getpid();
  for (size_t i = 0; i < g_thread_tids.size(); ++i) syscall(SYS_tgkill, pid, g_thread_tids[i], kRegsSignal);
}

void beer_guest_memory_will_change(const void* address, size_t size) {
  if (!g_smc || !g_ctx || !size) return;
  const uint64_t lo = (uint64_t)address & ~(kPage - 1);
  const uint64_t hi = ((uint64_t)address + size + kPage - 1) & ~(kPage - 1);
  {   // nothing translated from this range (file reads into data buffers, most protection changes): only note it
    std::scoped_lock lk(g_smc_lock);
    g_smc_maps_dirty = true;
    auto it = g_smc_code_pages.lower_bound(lo);
    if (it == g_smc_code_pages.end() || *it >= hi) return;
  }
  std::scoped_lock code(g_ctx->GetCodeInvalidationMutex());
  std::scoped_lock lk(g_smc_lock);
  SmcInvalidateLocked(lo, hi - lo);
  for (auto it = g_smc_code_pages.lower_bound(lo); it != g_smc_code_pages.end() && *it < hi;) {
    auto trapped = g_smc_trapped.find(*it);
    if (trapped != g_smc_trapped.end()) {   // give the caller (and the kernel) the protection the guest set
      mprotect((void*)trapped->first, kPage, trapped->second);
      g_smc_trapped.erase(trapped);
    }
    it = g_smc_code_pages.erase(it);
  }
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
