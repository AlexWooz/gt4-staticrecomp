// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
//
// TASK-175: what the performance report is doing, for the HUD to show. The report is the F7/F8 interval of perf_probe.*: F7 starts an
// interval ("Recording"), F8 writes the report ("Saved", shown for kSavedShownMs, then "Idle"). The probe writes this on the EE thread;
// the HUD reads it from the thread that presents. One atomic phase and one atomic time, no lock, nothing allocated: the render path
// only loads two integers. Header only, so the renderer library does not link the probe.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace gt4::perf {

enum class ReportPhase : std::uint8_t {
  idle = 0,
  recording = 1,  // an F7 interval is running
  saved = 2,      // F8 wrote the report, less than kSavedShownMs ago (older reads as idle)
};

class ReportStatus {
public:
  static constexpr std::uint64_t kSavedShownMs = 3000;

  // `enabled`: the launcher's Performance report setting (GT4_PERF=1). With it off the HUD shows no report line.
  explicit ReportStatus(bool enabled = false) : enabled_(enabled) {}

  // The process-wide status: enabled when GT4_PERF=1 at the first use.
  static ReportStatus &shared() {
    static ReportStatus status([] {
      const char *text = std::getenv("GT4_PERF");
      return text != nullptr && std::strcmp(text, "1") == 0;
    }());
    return status;
  }

  // A steady clock in milliseconds: the unit of every time here.
  static std::uint64_t now_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
  }

  bool enabled() const noexcept { return enabled_; }

  // The writer's side (the probe, on the EE thread).
  void interval_started() noexcept { phase_.store(ReportPhase::recording, std::memory_order_release); }
  void interval_ended() noexcept { phase_.store(ReportPhase::idle, std::memory_order_release); }
  void report_saved(std::uint64_t now) noexcept {
    saved_at_ms_.store(now, std::memory_order_relaxed);
    phase_.store(ReportPhase::saved, std::memory_order_release);
  }

  // The reader's side (any thread): Saved decays to Idle by itself kSavedShownMs after the report.
  ReportPhase phase(std::uint64_t now) const noexcept {
    const ReportPhase phase = phase_.load(std::memory_order_acquire);
    if (phase != ReportPhase::saved) return phase;
    const std::uint64_t saved_at = saved_at_ms_.load(std::memory_order_relaxed);
    return now >= saved_at && now - saved_at >= kSavedShownMs ? ReportPhase::idle : ReportPhase::saved;
  }

private:
  const bool enabled_;
  std::atomic<ReportPhase> phase_{ReportPhase::idle};
  std::atomic<std::uint64_t> saved_at_ms_{0};
};

}  // namespace gt4::perf
