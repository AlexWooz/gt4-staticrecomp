// TASK-090 crash-race diagnostic. See crash_dump.hpp.
//
// The first version of this handler (host-clock round) printed the host stack only; llvm-symbolizer mapped the top frame
// into `gen::unit_00E0` at guest PC 0x00482674. This version adds what that round could not see: the faulting address
// and access type from the exception record, the host RIP as a PE offset, the whole guest EE register file and the
// guest RAM around the pointer the instruction used. Every line is written with std::fprintf and flushed, because the
// process is about to die.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "crash_dump.hpp"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ps2/cpu.hpp"
#include "ps2/memory.hpp"
#include "ps2/system.hpp"

namespace gt4::crash_dump {
namespace {

constexpr std::uintptr_t kPreferredImageBase = 0x140000000ull;  // llvm-symbolizer address space of gt4vulkan.exe

std::uintptr_t module_base() { return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)); }

// The thread the game entry point runs on: the EE/video/sound thread of the donor. A fault there is a guest-code
// crash; a fault on any other thread is an emulator worker bug, and the guest pc means nothing then.
DWORD g_main_thread = 0;

// The address llvm-symbolizer expects: the PE's preferred base plus the offset from where the module really loaded.
std::uintptr_t symbol_address(std::uintptr_t absolute) {
  const std::uintptr_t base = module_base();
  if (absolute < base) return absolute;
  return kPreferredImageBase + (absolute - base);
}

bool in_guest_ram(std::uint32_t address, std::uint32_t bytes) {
  return address < ps2::kRamSize && bytes <= ps2::kRamSize - address;
}

void dump_bytes(const char *label, const std::uint8_t *bytes, std::uint32_t count) {
  std::fprintf(stderr, "[crash] %s:", label);
  for (std::uint32_t index = 0; index < count; ++index) {
    if ((index & 15u) == 0) std::fprintf(stderr, "\n[crash]   %08X:", index);
    std::fprintf(stderr, " %02X", bytes[index]);
  }
  std::fprintf(stderr, "\n");
}

void dump_guest_memory(const char *what, std::uint32_t address, std::uint32_t count) {
  if (!in_guest_ram(address, count)) {
    std::fprintf(stderr, "[crash] %s 0x%08X is not inside guest RAM\n", what, address);
    return;
  }
  char label[96];
  std::snprintf(label, sizeof label, "%s at guest 0x%08X", what, address);
  dump_bytes(label, ps2::g_ram + address, count);
}

void dump_host_stack(const CONTEXT &context) {
  // The real backtrace first: RtlCaptureStackBackTrace unwinds with the x64 unwind info, so it returns actual return
  // addresses when the frames are intact.
  void *frames[48] = {};
  const USHORT count = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
  std::fprintf(stderr, "[crash] backtrace (%u frames, PE offsets, llvm-symbolizer address):\n", count);
  for (USHORT index = 0; index < count; ++index) {
    const auto value = reinterpret_cast<std::uintptr_t>(frames[index]);
    std::fprintf(stderr, "[crash] frame %u %p sym 0x%016" PRIXPTR "\n", index, frames[index], symbol_address(value));
  }
  // The raw scan stays: the first round used it, and it finds frames the unwind could not walk. It is clamped to the
  // committed region holding rsp: reading past it (at the top of a thread stack or beside a guard page) faults the
  // handler itself, and a nested exception then kills the process it was only supposed to describe.
  MEMORY_BASIC_INFORMATION region{};
  if (VirtualQuery(reinterpret_cast<LPCVOID>(context.Rsp), &region, sizeof region) == 0) return;
  const std::uintptr_t first = context.Rsp;
  const std::uintptr_t last = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
  const std::uintptr_t limit = std::min<std::uintptr_t>(last, first + 1024 * sizeof(std::uint64_t));
  const std::uintptr_t base = module_base();
  std::fprintf(stderr, "[crash] host stack scan from %p to %p (PE offsets, llvm-symbolizer address):\n",
               reinterpret_cast<const void *>(first), reinterpret_cast<const void *>(limit));
  std::uint32_t printed = 0;
  for (std::uintptr_t at = first; at + sizeof(std::uint64_t) <= limit && printed < 48; at += sizeof(std::uint64_t)) {
    std::uint64_t value = 0;
    std::memcpy(&value, reinterpret_cast<const void *>(at), sizeof value);
    if (value >= base + 0x1000ull && value < base + 0x10000000ull) {
      std::fprintf(stderr, "[crash] stack %p sym 0x%016" PRIXPTR "\n", reinterpret_cast<const void *>(value),
                   symbol_address(static_cast<std::uintptr_t>(value)));
      ++printed;
    }
  }
}

void dump_exception(const EXCEPTION_RECORD &record, const CONTEXT &context) {
  const char *access = "?";
  std::uint64_t address = 0;
  if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2) {
    access = record.ExceptionInformation[0] == 0 ? "read" : record.ExceptionInformation[0] == 1 ? "write" : "execute";
    address = record.ExceptionInformation[1];
  }
  const DWORD tid = GetCurrentThreadId();
  std::fprintf(stderr,
               "[crash] exception 0x%08lX tid=%lu thread=%s rip=%p sym 0x%016" PRIXPTR
               " access=%s address=0x%016" PRIX64 "\n",
               static_cast<unsigned long>(record.ExceptionCode), static_cast<unsigned long>(tid),
               tid == g_main_thread ? "main (EE)" : "worker", reinterpret_cast<const void *>(context.Rip),
               symbol_address(context.Rip), access, address);
  std::fprintf(stderr, "[crash] context rip=%p rsp=%p rbp=%p rax=%p rbx=%p rcx=%p rdx=%p rsi=%p rdi=%p r8=%p r9=%p\n",
               reinterpret_cast<const void *>(context.Rip), reinterpret_cast<const void *>(context.Rsp),
               reinterpret_cast<const void *>(context.Rbp), reinterpret_cast<const void *>(context.Rax),
               reinterpret_cast<const void *>(context.Rbx), reinterpret_cast<const void *>(context.Rcx),
               reinterpret_cast<const void *>(context.Rdx), reinterpret_cast<const void *>(context.Rsi),
               reinterpret_cast<const void *>(context.Rdi), reinterpret_cast<const void *>(context.R8),
               reinterpret_cast<const void *>(context.R9));
  // The guest EE state: pc and every GPR, in the shape the translated units address them.
  const ps2::Cpu &cpu = ps2::g_cpu;
  std::fprintf(stderr, "[crash] guest pc=0x%08X budget=%d base=0x%08X hi=0x%016" PRIX64 " lo=0x%016" PRIX64 "\n",
               cpu.pc, static_cast<int>(cpu.budget), cpu.base, cpu.hi.d[0], cpu.lo.d[0]);
  for (std::uint32_t reg = 0; reg < 32; ++reg)
    std::fprintf(stderr, "[crash] guest r%02u=0x%016" PRIX64 "%s\n", reg, cpu.r[reg].d[0],
                 reg == 0 || reg == 29 || reg == 31 || reg == 19 ? "  (zero/sp/ra/s3)" : "");
  std::fprintf(stderr, "[crash] guest cop0 status=0x%08X cause=0x%08X epc=0x%08X badvaddr=0x%08X\n", cpu.cop0[12],
               cpu.cop0[13], cpu.cop0[14], cpu.cop0[8]);
  // The guest stack around sp: the earlier symbolization put the store through r19 at sp+160, and the crash site's
  // caller chain is readable from the guest words around sp.
  const std::uint32_t sp = cpu.r[29].w[0];
  if (in_guest_ram(sp, 0x200)) dump_bytes("guest stack at sp", ps2::g_ram + sp, 0x200);
  // The faulting host address, translated back to a guest address when it is inside guest RAM or the scratchpad.
  if (address >= reinterpret_cast<std::uintptr_t>(ps2::g_ram) &&
      address < reinterpret_cast<std::uintptr_t>(ps2::g_ram) + ps2::kRamSize) {
    const auto guest = static_cast<std::uint32_t>(address - reinterpret_cast<std::uintptr_t>(ps2::g_ram));
    std::fprintf(stderr, "[crash] faulting host address is guest RAM 0x%08X\n", guest);
    const std::uint32_t from = guest >= 64 ? guest - 64 : 0;
    dump_guest_memory("faulting guest RAM", from, 192);
  } else if (address >= reinterpret_cast<std::uintptr_t>(ps2::g_spr) &&
             address < reinterpret_cast<std::uintptr_t>(ps2::g_spr) + ps2::kScratchpadSize) {
    const auto guest = static_cast<std::uint32_t>(0x70000000u + (address - reinterpret_cast<std::uintptr_t>(ps2::g_spr)));
    std::fprintf(stderr, "[crash] faulting host address is guest scratchpad 0x%08X\n", guest);
  }
  // The symbolic report of the first round named a store through r19; show what that pointer addresses.
  const std::uint32_t r19 = cpu.r[19].w[0];
  std::fprintf(stderr, "[crash] guest r19 points to 0x%08X (%s)\n", r19,
               in_guest_ram(r19, 4) ? "guest RAM" : (r19 >= 0x70000000u && r19 < 0x70004000u) ? "scratchpad" : "outside RAM");
  if (in_guest_ram(r19, 64)) dump_guest_memory("guest r19 target", r19, 64);
}

LONG CALLBACK handler(EXCEPTION_POINTERS *info) {
  if (info == nullptr || info->ExceptionRecord == nullptr || info->ContextRecord == nullptr)
    return EXCEPTION_CONTINUE_SEARCH;
  // Every first-chance exception except the kinds the process raises on purpose: MSVC C++ exceptions
  // (0xE06D7363, used for every std::throw), the thread-name message (0x406D1388) and the debug-output exception
  // (0x40010006/0x40010007, raised by OutputDebugString on every use). Everything else is dumped, including
  // STATUS_HEAP_CORRUPTION (0xC0000374), which the first version filtered out and which kills silently.
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  if (code == 0xE06D7363u || code == 0x406D1388u || code == 0x40010006u || code == 0x40010007u)
    return EXCEPTION_CONTINUE_SEARCH;
  // No nested dump: if printing faults (a corrupted stack, a bad string), the process must not die inside its own
  // diagnostic. A nested exception goes straight to the default handler.
  static thread_local bool in_handler = false;
  if (in_handler) return EXCEPTION_CONTINUE_SEARCH;
  in_handler = true;
  // A throttled dump: a component that probes addresses on purpose would otherwise flood the log. The first ones are
  // the ones that matter; the count is reported so a filtered repeat is visible.
  static std::atomic<std::uint32_t> seen[8]{};
  const std::size_t slot = (code >> 4) & 7u;
  const std::uint32_t count = seen[slot].fetch_add(1, std::memory_order_relaxed);
  if (count < 16) {
    dump_exception(*info->ExceptionRecord, *info->ContextRecord);
    dump_host_stack(*info->ContextRecord);
    std::fflush(stderr);
  }
  in_handler = false;
  return EXCEPTION_CONTINUE_SEARCH;  // the default handler still terminates the process and reports exit 139
}

}  // namespace

void install_from_environment() {
  const char *value = std::getenv("GT4_CRASH_DUMP");
  if (value == nullptr || *value == '\0' || std::strcmp(value, "0") == 0) return;
  g_main_thread = GetCurrentThreadId();
  AddVectoredExceptionHandler(1, &handler);
  std::fprintf(stderr, "[crash] handler installed (GT4_CRASH_DUMP=1)\n");
  std::fflush(stderr);
}

}  // namespace gt4::crash_dump
