#pragma once

#include "perf_accounting.hpp"
#include "perf_report_status.hpp"
#include <atomic>
#include <cstdio>
#include <functional>
#include <thread>

namespace gt4::perf {

struct GuestSample {
  std::uint64_t cycles = 0;
  std::uint64_t primitives = 0;
  std::uint64_t vu1_interpreted = 0;
};

// TASK-084 step 0 (slice260, docs/FRAME-COST-STUDY.md section 6): cheap per-interval counters that the wall-time
// scopes cannot show. Every writer is the EE thread (the probe's owner), so plain fields are enough; the probe resets
// them with its interval. Counters are always compiled in and cost one increment each.
struct FrameCounters {
  // GIF traffic at the render point, per path (1 = VU1 PATH1, 2 = VIF1 DIRECT, 3 = DMA ch2).
  std::uint64_t gif_bytes[4] = {};
  std::uint64_t gif_records[4] = {};
  // VIF1 posting: packets offered by the EE shadow, jobs posted to the worker, and DMA chain endings observed.
  std::uint64_t vif_packets = 0;
  std::uint64_t vif_jobs = 0;
  std::uint64_t vif_job_bytes = 0;
  std::uint64_t vif_chains = 0;
  // Kernel work: syscalls by number (the low 7 bits, so 0x80..0xFF fold onto 0x00..0x7F), their wall time, and
  // thread switches.
  std::uint64_t syscalls = 0;
  std::uint64_t syscall_us = 0;
  std::uint64_t syscall_count[128] = {};
  std::uint64_t thread_switches = 0;
};
FrameCounters &frame_counters() noexcept;
void reset_frame_counters() noexcept;

// Counts one syscall by number and its wall time (RAII: one call per handle_syscall).
class SyscallScope {
 public:
  explicit SyscallScope(std::uint32_t number) noexcept;
  ~SyscallScope();
  SyscallScope(const SyscallScope &) = delete;
  SyscallScope &operator=(const SyscallScope &) = delete;

 private:
  std::uint64_t start_us_ = 0;
  std::uint32_t number_ = 0;
};
void count_thread_switch() noexcept;
// One GIF record at the render point (EE thread): bytes per path for the split.
void count_gif(int path, std::uint64_t bytes) noexcept;

enum class Request { start, report, toggle };

// The optional CPU clock returns UINT64_MAX when unavailable.
struct Clocks {
  std::uint64_t (*wall_us)();
  std::uint64_t (*thread_cpu_us)();
};

class Probe {
public:
  Probe(Clocks clocks, bool enabled, std::FILE *output = stderr, bool detailed = true,
        bool split_vu = false);
  static Probe &shared();
  Accounting::Token enter(Category category) noexcept;
  void leave(Accounting::Token token) noexcept;
  void request(Request request, bool repeated = false) noexcept;
  void frame_boundary(GuestSample guest) noexcept;
  void bytes(bool read, std::uint64_t bytes) noexcept;
  void presentation(std::uint64_t rgba_bytes) noexcept;
  void dispatch(std::uint64_t workgroups) noexcept;
  void audio(std::uint64_t produced, std::uint64_t completed,
             std::uint64_t empty_refills = 0) noexcept;
  void finish() noexcept;
  // TASK-011 step 3: the [mtvu] report section (after the legacy lines) and the interval reset that goes with it.
  // Installed by the worker runtime on the EE thread; with no hooks the report is unchanged.
  using MtvuReport = void (*)(std::FILE *, const IntervalTime &, std::uint64_t);
  using MtvuReset = void (*)();
  static void set_mtvu_hooks(MtvuReport report, MtvuReset reset) noexcept;
  // TASK-035: the [gs-ipc] section (GS stream counters), printed after the legacy lines; same signatures.
  static void set_gs_ipc_hooks(MtvuReport report, MtvuReset reset) noexcept;
  // Extra report lines (for example pacing diagnostics) printed after each
  // interval report. An empty function removes the section.
  void set_report_section(std::function<void(std::FILE *)> section);
  // TASK-175: where F7 ("Recording") and F8 ("Saved") are announced for the performance HUD; null: nowhere. Set before the first
  // frame boundary (Probe::shared() points it at ReportStatus::shared()).
  void set_report_status(ReportStatus *status) noexcept { status_ = status; }
  bool enabled() const noexcept { return accounting_.enabled(); }
  bool valid() const noexcept;
  std::uint64_t frames() const noexcept { return frames_; }
  const IntervalTime &interval() const noexcept { return accounting_.interval(); }
  // TASK-041: measured EE-thread CPU ms/frame of the report being printed (negative when Unknown); the report hooks
  // ([mtvu], [gs-ipc]) read it while print() runs.
  double report_cpu_ms_per_frame() const noexcept { return report_cpu_ms_; }
  // TASK-062: exclusive time of a category since the probe started, across every interval reset (F7, F8, periodic),
  // so a live reader (the GS stream OSD statistics) can take differences without disturbing the F8 intervals. Only
  // the owner thread may call it; 0 for an invalid category.
  std::uint64_t cumulative_exclusive_us(Category category) const noexcept;

private:
  ReportStatus *status_ = nullptr;
  bool report_written_ = false;  // the last print() wrote a full report (not "disabled", INVALID or "no complete interval")
  bool owner() noexcept;
  std::uint64_t timestamp() noexcept;
  void reset(std::uint64_t now, GuestSample guest) noexcept;
  void print(const char *reason) noexcept;
  void add(std::uint64_t &value, std::uint64_t amount) noexcept;
  Clocks clocks_;
  std::FILE *output_;
  const bool detailed_;
  const bool split_vu_;
  const std::thread::id owner_thread_;
  std::atomic<bool> wrong_thread_{false};
  Accounting accounting_;
  bool counter_fault_ = false, guest_reset_ = false, cpu_valid_ = false;
  bool first_frame_ = true, manual_ = false;
  bool pending_start_ = false, pending_report_ = false, pending_toggle_ = false;
  std::function<void(std::FILE *)> report_section_;
  std::uint64_t last_us_ = 0, last_frame_us_ = 0, cpu_start_ = 0;
  std::uint64_t cpu_at_boundary_ = UINT64_MAX;
  double report_cpu_ms_ = -1.0;
  std::uint64_t frames_ = 0, guest_cycles_ = 0, primitives_ = 0, vu_calls_ = 0;
  // TASK-062: exclusive time of every category in the intervals before the current one (added at each reset).
  std::array<std::uint64_t, static_cast<std::size_t>(Category::count)> carried_exclusive_us_{};
  GuestSample last_guest_{};
  std::uint64_t read_bytes_ = 0, write_bytes_ = 0, rgba_bytes_ = 0;
  std::uint64_t presentations_ = 0, dispatches_ = 0, workgroups_ = 0;
  std::uint64_t audio_produced_ = 0, audio_completed_ = 0, empty_refills_ = 0;
  static constexpr std::size_t capacity = 4096;
  std::array<std::uint64_t, capacity> periods_{};
  std::size_t period_count_ = 0, next_period_ = 0;
};

#ifdef GT4_PERF_FRAME_ONLY
class Scope {
public:
  explicit Scope(Category) noexcept {}
};
#else
// TASK-011 step 3 (slice171): a thread that bound a worker accounting (bind_worker) records its scopes there,
// not in the shared probe. Only that thread touches the accounting; other threads never see it.
struct WorkerBinding {
  Accounting *accounting = nullptr;
  std::uint64_t (*wall_us)() = nullptr;
};
inline thread_local WorkerBinding t_worker_binding{};
void bind_worker(Accounting *accounting, std::uint64_t (*wall_us)()) noexcept;

class Scope {
public:
  // TASK-134: with the performance report off (a player's normal session) a scope reads no clock and keeps no stack: on the
  // VU1 worker and in the VIF path that is thousands of scopes per VBlank.
  explicit Scope(Category category) noexcept : worker_(t_worker_binding.accounting) {
    if (worker_) {
      if (worker_->enabled()) token_ = worker_->enter(category, t_worker_binding.wall_us());
    } else {
      Probe &probe = Probe::shared();
      if (probe.enabled()) {
        probe_ = &probe;
        token_ = probe.enter(category);
      }
    }
  }
  ~Scope() {
    if (!token_) return;
    if (worker_) worker_->leave(token_, t_worker_binding.wall_us());
    else probe_->leave(token_);
  }
  Scope(const Scope &) = delete;
  Scope &operator=(const Scope &) = delete;
private:
  Accounting *worker_ = nullptr;
  Probe *probe_ = nullptr;
  Accounting::Token token_ = 0;
};
#endif
} // namespace gt4::perf

#if defined(GT4_PERF_PROBE) && !defined(GT4_PERF_FRAME_ONLY)
#define GT4_PERF_SCOPE(category) gt4::perf::Scope gt4_perf_scope(category)
#else
#define GT4_PERF_SCOPE(category) ((void)0)
#endif
