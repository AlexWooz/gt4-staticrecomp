#pragma once

#include <cstdint>

// Wall clock for the frame pacer.
//   Windows (frame_clock.cpp): QueryPerformanceCounter ticks, released by a high-resolution waitable timer (Windows 10
//   1803 and later) with a short spin for the final stretch. Falls back to Sleep under timeBeginPeriod(1) when the timer
//   cannot be created.
//   POSIX (frame_clock_posix.cpp, TASK-180): CLOCK_MONOTONIC nanoseconds, released by clock_nanosleep with the same
//   final spin.
class FrameClock {
public:
  FrameClock();
  ~FrameClock();
  FrameClock(const FrameClock &) = delete;
  FrameClock &operator=(const FrameClock &) = delete;

  std::uint64_t frequency() const { return frequency_; }
  std::int64_t now() const;
  // Returns once now() >= target. Sleeps until shortly before the target, then spins.
  void wait_until(std::int64_t target);
  bool high_resolution_timer() const { return high_resolution_; }

private:
  void sleep_ticks(std::int64_t ticks);
  std::uint64_t frequency_ = 0;
  void *timer_ = nullptr;  // Windows: the waitable timer HANDLE (null without one); unused elsewhere
  bool period_raised_ = false;
  bool high_resolution_ = false;
};
