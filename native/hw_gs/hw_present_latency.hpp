#pragma once
// IN-521: keeps the display queue short under vsync. At 60 frames on a 60 Hz display one frame goes in and one comes out per
// refresh, so every hiccup that adds a frame to the queue stays there: time from presentation to display 37 ms at the start of
// a lap and 53 ms later, 25 ms without vsync (a stream's virtual display). When that time stays high, one presentation is
// left out, which takes one frame out of the queue. It stops trying when that made the display repeat a frame, or did not
// help. The shown times come from VK_KHR_present_wait2; without it nothing here runs.
// Measured and dropped: walking the pacer's grid towards the flip as well reached 35 ms, with 18 frames repeated per lap.

#include <algorithm>
#include <cstdint>
#include <limits>
#include <mutex>

namespace gt4::hw {

class PresentLatencyControl {
public:
  static constexpr double kFrameMs = 1000.0 / 60.0;
  static constexpr int kWindow = 60;  // shown frames per decision
  static constexpr int kSettle = 30;  // shown frames after leaving one out, before judging it
  static constexpr double kStartTargetMs = 33.0;

  explicit PresentLatencyControl(double targetMs = kStartTargetMs) : targetMs_(targetMs) {}

  // Worker thread: one frame reached the display. `lagMs` from its presentation, `intervalMs` from the frame before it.
  void shown(double lagMs, double intervalMs) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (judging_) {
      repeated_ = repeated_ || intervalMs > kFrameMs * 1.5;
      if (++settled_ <= kSettle) return;
    }
    windowMin_ = std::min(windowMin_, lagMs);
    if (++windowCount_ < kWindow) return;
    const double lag = windowMin_;
    windowMin_ = std::numeric_limits<double>::infinity();
    windowCount_ = 0;
    lastLagMs_ = lag;
    if (judging_) {
      judging_ = false;
      // The queue is as short as this display takes, or a shorter one does not lower the time: stay here.
      if (repeated_ || lag > lagBefore_ - kFrameMs / 2.0) {
        targetMs_ = std::max(targetMs_, std::max(lag, lagBefore_) + 2.0);
      }
      return;
    }
    if (lag > targetMs_) {
      leaveOut_ = true;
      judging_ = true;
      repeated_ = false;
      settled_ = 0;
      lagBefore_ = lag;
    }
  }

  // GS thread, before each presentation: true once per decision.
  bool leave_out() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!leaveOut_) return false;
    leaveOut_ = false;
    ++leftOut_;
    return true;
  }

  std::uint64_t left_out() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return leftOut_;
  }
  double target_ms() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return targetMs_;
  }
  double last_lag_ms() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return lastLagMs_;
  }

private:
  std::mutex mutex_;
  double targetMs_;
  double windowMin_ = std::numeric_limits<double>::infinity();
  int windowCount_ = 0;
  bool leaveOut_ = false;
  bool judging_ = false;
  bool repeated_ = false;
  int settled_ = 0;
  double lagBefore_ = 0.0;
  double lastLagMs_ = 0.0;
  std::uint64_t leftOut_ = 0;
};

}  // namespace gt4::hw
