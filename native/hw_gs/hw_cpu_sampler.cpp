// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// TASK-027 CPU survey: statistical CPU profiler of the game process (see hw_cpu_sampler.hpp).

#include "hw_cpu_sampler.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gt4::hw::sampler {
namespace {

constexpr std::size_t kMaxDepth = 48;                 // frames kept per stack
constexpr std::size_t kStackCopyBytes = 64u << 10;    // top of the stack copied while the thread is suspended
constexpr std::size_t kPoolWords = 6u << 20;          // 48 MiB of samples, then later samples are dropped
constexpr std::uint64_t kIdleCycles = 100000;         // less than this since the last visit: the thread was idle
constexpr std::uint32_t kEnumerateEveryVisits = 250;  // new threads are picked up every 250 periods

using NtQueryInformationThreadFn = LONG(WINAPI *)(HANDLE, ULONG, PVOID, ULONG, PULONG);
using GetThreadDescriptionFn = HRESULT(WINAPI *)(HANDLE, PWSTR *);

struct NamedThread {
  DWORD id = 0;
  char name[32] = {};
};

std::mutex g_names_mutex;
NamedThread g_names[16];
std::size_t g_name_count = 0;

std::atomic<std::uint64_t> g_vblank{0};

struct Thread {
  DWORD id = 0;
  HANDLE handle = nullptr;
  std::string name;
  std::uint64_t start_address = 0;
  std::uint64_t stack_base = 0;
  std::uint64_t last_cycles = 0;
  bool primed = false;
  bool alive = true;
  std::uint64_t window_cycles = 0;
  std::uint64_t samples = 0;
  // Kernel + user CPU time (100 ns units) when the window opened and when it closed (or the thread was last seen).
  std::uint64_t cpu_open = 0, cpu_close = 0;
  bool cpu_opened = false;
};

std::uint64_t filetime_sum(const FILETIME &kernel, const FILETIME &user) {
  const auto ticks = [](const FILETIME &t) {
    return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
  };
  return ticks(kernel) + ticks(user);
}

std::uint64_t thread_cpu(HANDLE handle) {
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetThreadTimes(handle, &created, &exited, &kernel, &user)) return 0;
  return filetime_sum(kernel, user);
}

// The copied stack is walked with the unwind tables after the thread was resumed. No C++ objects live here, so the
// structured exception handler can catch a fault on a stack that the unwind data does not describe.
std::size_t walk_copy(CONTEXT &context, std::uint64_t copy_low, std::uint64_t copy_high, std::uint64_t *frames) {
  std::size_t depth = 0;
  __try {
    frames[depth++] = context.Rip;
    while (depth < kMaxDepth) {
      if (context.Rsp < copy_low || context.Rsp + 8 > copy_high) break;
      DWORD64 image_base = 0;
      PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
      if (function) {
        PVOID handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data, &establisher,
                         nullptr);
      } else {  // a leaf function without unwind data: the return address is on top of the stack
        context.Rip = *reinterpret_cast<const DWORD64 *>(context.Rsp);
        context.Rsp += 8;
      }
      if (context.Rip == 0) break;
      frames[depth++] = context.Rip;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return depth;
}

class Sampler {
 public:
  bool start(const char *path) {
    path_ = path;
    if (const char *period = std::getenv("GT4_CPU_SAMPLER_PERIOD_US")) {
      const long value = std::strtol(period, nullptr, 10);
      if (value >= 100) period_us_ = static_cast<std::uint32_t>(value);
    }
    if (const char *from = std::getenv("GT4_CPU_SAMPLER_FROM")) from_ = std::strtoull(from, nullptr, 10);
    if (const char *to = std::getenv("GT4_CPU_SAMPLER_TO")) to_ = std::strtoull(to, nullptr, 10);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    query_thread_ = ntdll ? reinterpret_cast<NtQueryInformationThreadFn>(
                                reinterpret_cast<void *>(GetProcAddress(ntdll, "NtQueryInformationThread")))
                          : nullptr;
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    describe_ = kernel ? reinterpret_cast<GetThreadDescriptionFn>(
                             reinterpret_cast<void *>(GetProcAddress(kernel, "GetThreadDescription")))
                       : nullptr;
    pool_.reserve(kPoolWords);
    copy_.resize(kStackCopyBytes / 8);
    thread_ = std::thread([this] { run(); });
    return true;
  }

  void stop_and_write(std::FILE *log) {
    stop_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
    write(log);
  }

 private:
  void run() {
    sampler_id_ = GetCurrentThreadId();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    std::uint32_t visits = 0;
    while (!stop_.load(std::memory_order_acquire)) {
      if (timer) {
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>(period_us_) * 10;
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        WaitForSingleObject(timer, INFINITE);
      } else {
        Sleep(1);
      }
      if (visits++ % kEnumerateEveryVisits == 0) enumerate();
      const std::uint64_t vblank = g_vblank.load(std::memory_order_relaxed);
      const bool in_window = vblank >= from_ && vblank < to_;
      if (in_window && !window_open_) open_window();
      if (!in_window && window_open_) close_window();
      for (Thread &t : threads_) visit(t, in_window);
      ++periods_;
    }
    if (window_open_) close_window();
    if (timer) CloseHandle(timer);
  }

  void open_window() {
    window_open_ = true;
    window_opened_ = true;
    QueryPerformanceCounter(&window_start_);
    window_vblank_start_ = g_vblank.load(std::memory_order_relaxed);
    for (Thread &t : threads_) {
      if (!t.alive) continue;
      t.cpu_open = thread_cpu(t.handle);
      t.cpu_opened = true;
    }
  }

  void close_window() {
    window_open_ = false;
    QueryPerformanceCounter(&window_end_);
    window_vblank_end_ = g_vblank.load(std::memory_order_relaxed);
    for (Thread &t : threads_) {
      if (t.alive) t.cpu_close = thread_cpu(t.handle);
    }
  }

  // Picks up threads created since the last pass (handles stay open, so thread ids are never reused under us).
  void enumerate() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    const DWORD process = GetCurrentProcessId();
    THREADENTRY32 entry{};
    entry.dwSize = sizeof entry;
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
      if (entry.th32OwnerProcessID != process || entry.th32ThreadID == sampler_id_) continue;
      const DWORD id = entry.th32ThreadID;
      if (std::any_of(threads_.begin(), threads_.end(), [id](const Thread &t) { return t.id == id && t.alive; })) {
        continue;
      }
      Thread t;
      t.id = id;
      t.handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
      if (!t.handle) continue;
      describe(t);
      if (window_open_) {
        t.cpu_open = thread_cpu(t.handle);
        t.cpu_opened = true;
      }
      threads_.push_back(std::move(t));
    }
    CloseHandle(snapshot);
  }

  void describe(Thread &t) {
    if (query_thread_) {
      struct BasicInformation {
        LONG exit_status;
        PVOID teb;
        struct {
          HANDLE process, thread;
        } client;
        ULONG_PTR affinity;
        LONG priority, base_priority;
      } basic{};
      if (query_thread_(t.handle, 0 /* ThreadBasicInformation */, &basic, sizeof basic, nullptr) == 0 && basic.teb) {
        t.stack_base = reinterpret_cast<std::uint64_t>(static_cast<const NT_TIB *>(basic.teb)->StackBase);
      }
      PVOID start = nullptr;
      if (query_thread_(t.handle, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof start, nullptr) == 0) {
        t.start_address = reinterpret_cast<std::uint64_t>(start);
      }
    }
    {
      std::lock_guard<std::mutex> lock(g_names_mutex);
      for (std::size_t i = 0; i < g_name_count; ++i) {
        if (g_names[i].id == t.id) t.name = g_names[i].name;
      }
    }
    if (t.name.empty() && describe_) {
      PWSTR text = nullptr;
      if (SUCCEEDED(describe_(t.handle, &text)) && text) {
        char narrow[64] = {};
        WideCharToMultiByte(CP_UTF8, 0, text, -1, narrow, sizeof narrow - 1, nullptr, nullptr);
        t.name = narrow;
        LocalFree(text);
      }
    }
    for (char &c : t.name) {
      if (c == ' ' || c == '\t') c = '_';
    }
    if (t.name.empty()) t.name = "-";
  }

  void visit(Thread &t, bool in_window) {
    if (!t.alive) return;
    ULONG64 cycles = 0;
    DWORD exit_code = 0;
    if (!QueryThreadCycleTime(t.handle, &cycles) || !GetExitCodeThread(t.handle, &exit_code) ||
        exit_code != STILL_ACTIVE) {
      t.alive = false;
      if (window_open_) t.cpu_close = thread_cpu(t.handle);
      return;
    }
    const std::uint64_t used = cycles - t.last_cycles;
    t.last_cycles = cycles;
    if (!t.primed) {
      t.primed = true;
      return;
    }
    if (!in_window) return;
    t.window_cycles += used;
    if (used < kIdleCycles) return;
    if (t.stack_base == 0) return;

    // Suspended only for the register read and the stack copy: no allocation, no lock.
    LARGE_INTEGER before{}, after{};
    QueryPerformanceCounter(&before);
    if (SuspendThread(t.handle) == static_cast<DWORD>(-1)) return;
    CONTEXT context{};
    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    std::size_t copied = 0;
    std::uint64_t rsp = 0;
    if (GetThreadContext(t.handle, &context)) {
      rsp = context.Rsp;
      if (rsp < t.stack_base && t.stack_base - rsp < (64ull << 20)) {
        copied = static_cast<std::size_t>(std::min<std::uint64_t>(kStackCopyBytes, t.stack_base - rsp)) & ~7ull;
        std::memcpy(copy_.data(), reinterpret_cast<const void *>(rsp), copied);
      }
    }
    ResumeThread(t.handle);
    QueryPerformanceCounter(&after);
    suspended_ticks_ += static_cast<std::uint64_t>(after.QuadPart - before.QuadPart);
    ++suspensions_;
    if (copied == 0) return;

    // Values that point into the copied range (saved frame pointers, the registers) are moved into the copy.
    const std::uint64_t low = rsp, high = rsp + copied;
    const std::uint64_t copy_low = reinterpret_cast<std::uint64_t>(copy_.data());
    const auto rebase = [&](std::uint64_t value) { return value >= low && value < high ? value - low + copy_low : value; };
    for (std::size_t i = 0; i < copied / 8; ++i) copy_[i] = rebase(copy_[i]);
    context.Rsp = copy_low;
    context.Rbp = rebase(context.Rbp);
    context.Rbx = rebase(context.Rbx);
    context.Rsi = rebase(context.Rsi);
    context.Rdi = rebase(context.Rdi);
    context.R12 = rebase(context.R12);
    context.R13 = rebase(context.R13);
    context.R14 = rebase(context.R14);
    context.R15 = rebase(context.R15);

    std::uint64_t frames[kMaxDepth];
    const std::size_t depth = walk_copy(context, copy_low, copy_low + copied, frames);
    if (pool_.size() + 2 + depth > pool_.capacity()) {
      ++dropped_;
      return;
    }
    pool_.push_back((static_cast<std::uint64_t>(t.id) << 32) | depth);
    pool_.push_back(used);
    pool_.insert(pool_.end(), frames, frames + depth);
    ++t.samples;
    ++samples_;
  }

  void write(std::FILE *log) {
    std::FILE *file = std::fopen(path_.c_str(), "w");
    if (!file) {
      std::fprintf(log, "[cpu-sampler] cannot write %s\n", path_.c_str());
      return;
    }
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    const double window_us =
        window_opened_ ? static_cast<double>(window_end_.QuadPart - window_start_.QuadPart) * 1e6 / frequency.QuadPart
                       : 0.0;
    const double suspended_us = static_cast<double>(suspended_ticks_) * 1e6 / frequency.QuadPart;
    std::fprintf(file,
                 "gt4-cpu-sampler 1 period_us=%u from=%llu to=%llu window_vblanks=%llu window_us=%.0f periods=%llu "
                 "samples=%llu dropped=%llu suspensions=%llu suspended_us=%.0f\n",
                 period_us_, static_cast<unsigned long long>(from_), static_cast<unsigned long long>(to_),
                 static_cast<unsigned long long>(window_vblank_end_ - window_vblank_start_), window_us,
                 static_cast<unsigned long long>(periods_), static_cast<unsigned long long>(samples_),
                 static_cast<unsigned long long>(dropped_), static_cast<unsigned long long>(suspensions_),
                 suspended_us);
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
      MODULEENTRY32W module{};
      module.dwSize = sizeof module;
      for (BOOL more = Module32FirstW(snapshot, &module); more; more = Module32NextW(snapshot, &module)) {
        char path[MAX_PATH * 3] = {};
        WideCharToMultiByte(CP_UTF8, 0, module.szExePath, -1, path, sizeof path - 1, nullptr, nullptr);
        std::fprintf(file, "module 0x%llx 0x%lx %s\n",
                     static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(module.modBaseAddr)),
                     static_cast<unsigned long>(module.modBaseSize), path);
      }
      CloseHandle(snapshot);
    }
    for (Thread &t : threads_) {
      {  // a thread may have been named after it was first seen
        std::lock_guard<std::mutex> lock(g_names_mutex);
        for (std::size_t i = 0; i < g_name_count; ++i) {
          if (g_names[i].id == t.id) t.name = g_names[i].name;
        }
      }
      const double cpu_ms = t.cpu_opened && t.cpu_close >= t.cpu_open ? (t.cpu_close - t.cpu_open) / 10000.0 : 0.0;
      std::fprintf(file, "thread %lu 0x%llx %llu %llu %.3f %s\n", static_cast<unsigned long>(t.id),
                   static_cast<unsigned long long>(t.start_address), static_cast<unsigned long long>(t.window_cycles),
                   static_cast<unsigned long long>(t.samples), cpu_ms, t.name.c_str());
    }
    // Identical stacks are written once with their count and cycles.
    std::map<std::vector<std::uint64_t>, std::pair<std::uint64_t, std::uint64_t>> stacks;
    for (std::size_t i = 0; i + 2 <= pool_.size();) {
      const std::uint64_t header = pool_[i];
      const std::size_t depth = static_cast<std::size_t>(header & 0xFFFFFFFFu);
      std::vector<std::uint64_t> key;
      key.reserve(depth + 1);
      key.push_back(header >> 32);
      key.insert(key.end(), pool_.begin() + i + 2, pool_.begin() + i + 2 + depth);
      auto &slot = stacks[key];
      slot.first += 1;
      slot.second += pool_[i + 1];
      i += 2 + depth;
    }
    for (const auto &[key, value] : stacks) {
      std::fprintf(file, "s %llu %llu %llu", static_cast<unsigned long long>(key[0]),
                   static_cast<unsigned long long>(value.first), static_cast<unsigned long long>(value.second));
      for (std::size_t j = 1; j < key.size(); ++j) std::fprintf(file, " %llx", static_cast<unsigned long long>(key[j]));
      std::fputc('\n', file);
    }
    std::fclose(file);
    std::fprintf(log,
                 "[cpu-sampler] %llu samples (%zu stacks) of %zu threads over %llu VBlanks to %s; dropped %llu; "
                 "suspended %.1f us per visit\n",
                 static_cast<unsigned long long>(samples_), stacks.size(), threads_.size(),
                 static_cast<unsigned long long>(window_vblank_end_ - window_vblank_start_), path_.c_str(),
                 static_cast<unsigned long long>(dropped_), suspensions_ ? suspended_us / suspensions_ : 0.0);
  }

  std::string path_;
  std::uint32_t period_us_ = 1000;
  std::uint64_t from_ = 0, to_ = ~0ull;
  NtQueryInformationThreadFn query_thread_ = nullptr;
  GetThreadDescriptionFn describe_ = nullptr;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  DWORD sampler_id_ = 0;
  std::vector<Thread> threads_;
  std::vector<std::uint64_t> pool_;
  std::vector<std::uint64_t> copy_;
  std::uint64_t samples_ = 0, dropped_ = 0, periods_ = 0, suspensions_ = 0, suspended_ticks_ = 0;
  bool window_open_ = false, window_opened_ = false;
  LARGE_INTEGER window_start_{}, window_end_{};
  std::uint64_t window_vblank_start_ = 0, window_vblank_end_ = 0;
};

Sampler *g_sampler = nullptr;
std::once_flag g_started;
std::once_flag g_finished;

}  // namespace

void name_current_thread(const char *name) {
  std::lock_guard<std::mutex> lock(g_names_mutex);
  if (g_name_count == std::size(g_names)) return;
  NamedThread &slot = g_names[g_name_count++];
  slot.id = GetCurrentThreadId();
  std::strncpy(slot.name, name, sizeof slot.name - 1);
}

bool start_from_env() {
  std::call_once(g_started, [] {
    const char *path = std::getenv("GT4_CPU_SAMPLER");
    if (!path || !*path) return;
    g_sampler = new Sampler;  // never destroyed: finish() joins its thread
    g_sampler->start(path);
  });
  return g_sampler != nullptr;
}

void note_vblank(std::uint64_t vblank) { g_vblank.store(vblank, std::memory_order_relaxed); }

void finish(std::FILE *log) {
  std::call_once(g_finished, [log] {
    if (g_sampler) g_sampler->stop_and_write(log);
  });
}

}  // namespace gt4::hw::sampler
