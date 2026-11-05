// SPDX-License-Identifier: MIT
// TASK-180: the POSIX frame clock (Linux, Android). Ticks are CLOCK_MONOTONIC nanoseconds; waits sleep with
// clock_nanosleep on an absolute deadline until shortly before the target, then spin, as the Windows clock does.
#include "frame_clock.hpp"

#include <cerrno>
#include <ctime>

namespace {
// The final stretch is spun rather than slept: a timer wake-up can be late (same rule as frame_clock.cpp).
constexpr std::int64_t spin_nanoseconds = 500000;
constexpr std::int64_t nanoseconds_per_second = 1000000000;
}  // namespace

FrameClock::FrameClock() {
  frequency_ = static_cast<std::uint64_t>(nanoseconds_per_second);
  high_resolution_ = true;  // clock_nanosleep has nanosecond resolution; the kernel's timer slack decides the rest
}

FrameClock::~FrameClock() = default;

std::int64_t FrameClock::now() const {
  timespec value{};
  clock_gettime(CLOCK_MONOTONIC, &value);
  return static_cast<std::int64_t>(value.tv_sec) * nanoseconds_per_second + value.tv_nsec;
}

void FrameClock::sleep_ticks(std::int64_t ticks) {
  if (ticks <= 0) return;
  const std::int64_t deadline = now() + ticks;
  timespec until{};
  until.tv_sec = static_cast<time_t>(deadline / nanoseconds_per_second);
  until.tv_nsec = static_cast<long>(deadline % nanoseconds_per_second);
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, nullptr) == EINTR) {
  }
}

void FrameClock::wait_until(std::int64_t target) {
  for (;;) {
    const std::int64_t remaining = target - now();
    if (remaining <= 0) return;
    if (remaining > spin_nanoseconds) sleep_ticks(remaining - spin_nanoseconds);
    // Within the spin window: busy-wait for the exact tick.
  }
}
