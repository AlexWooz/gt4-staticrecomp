#include "frame_clock.hpp"

#include <cstdio>
#include <stdexcept>

#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h>
#include <timeapi.h>
// clang-format on

#pragma comment(lib, "winmm.lib")

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace {
// The final stretch is spun rather than slept: a timer wake-up can be late.
constexpr double spin_seconds = 0.0005;
}

FrameClock::FrameClock() {
  LARGE_INTEGER frequency{};
  if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
    throw std::runtime_error("QueryPerformanceFrequency unavailable");
  frequency_ = static_cast<std::uint64_t>(frequency.QuadPart);
  timer_ = CreateWaitableTimerExW(nullptr, nullptr,
                                  CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                  TIMER_ALL_ACCESS);
  if (!timer_) {
    // Older Windows: a 1 ms system timer period makes Sleep usable for coarse waits.
    if (timeBeginPeriod(1) != TIMERR_NOERROR)
      throw std::runtime_error("No high-resolution timer and timeBeginPeriod failed");
    period_raised_ = true;
    std::fprintf(stderr, "[pacing] high-resolution waitable timer unavailable; using 1 ms Sleep\n");
  }
  high_resolution_ = timer_ != nullptr;
}

FrameClock::~FrameClock() {
  if (timer_)
    CloseHandle(timer_);
  if (period_raised_)
    timeEndPeriod(1);
}

std::int64_t FrameClock::now() const {
  LARGE_INTEGER counter{};
  QueryPerformanceCounter(&counter);
  return counter.QuadPart;
}

void FrameClock::sleep_ticks(std::int64_t ticks) {
  if (timer_) {
    // Relative due time in 100 ns units; negative means relative.
    const double seconds = static_cast<double>(ticks) / static_cast<double>(frequency_);
    LARGE_INTEGER due{};
    due.QuadPart = -static_cast<LONGLONG>(seconds * 10000000.0);
    if (due.QuadPart >= 0)
      return;
    if (!SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE))
      throw std::runtime_error("SetWaitableTimer failed");
    WaitForSingleObject(timer_, INFINITE);
    return;
  }
  const double milliseconds = static_cast<double>(ticks) * 1000.0 / static_cast<double>(frequency_);
  const auto whole = static_cast<DWORD>(milliseconds);
  if (whole)
    Sleep(whole);
}

void FrameClock::wait_until(std::int64_t target) {
  const auto spin = static_cast<std::int64_t>(spin_seconds * static_cast<double>(frequency_));
  for (;;) {
    const std::int64_t remaining = target - now();
    if (remaining <= 0)
      return;
    if (remaining > spin)
      sleep_ticks(remaining - spin);
    // Within the spin window: busy-wait for the exact tick.
  }
}
