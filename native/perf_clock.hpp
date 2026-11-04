// TASK-134 P1d: the clock of the F7/F8 scopes. The scopes read it twice each, thousands of times per VBlank, on the EE and
// on the VU1 worker; through std::chrono::steady_clock that cost 11% of the worker's CPU in the George V Paris profile
// (docs/GAME-CODE-OPTIMIZATION-STUDY.md section 8). On a CPU with an invariant TSC this reads the TSC directly and scales
// it to microseconds with one multiplication. It only times reports: nothing in the game or the pacing reads it.
// GT4_PERF_CLOCK=steady selects the old clock (A/B), and so does a CPU without an invariant TSC.
#pragma once

#include <cstdint>

namespace gt4::perf {

// Microseconds in 0.64 fixed point per TSC tick; 0 when the TSC is not used. Set once, by the calibration during static
// initialisation of perf_clock.cpp, before any thread that reads it starts.
extern std::uint64_t g_tsc_us_per_tick;
std::uint64_t steady_wall_us() noexcept;

// The last value this thread read: a thread that moves between cores must not see the TSC step back, because Accounting
// treats a backward clock as a fault and stops counting.
inline thread_local std::uint64_t t_last_wall_us = 0;

inline std::uint64_t wall_us_from_ticks(std::uint64_t ticks, std::uint64_t us_per_tick) noexcept {
  return static_cast<std::uint64_t>((static_cast<unsigned __int128>(ticks) * us_per_tick) >> 64);
}

inline std::uint64_t scope_wall_us() noexcept {
  if (g_tsc_us_per_tick == 0) return steady_wall_us();
  const std::uint64_t now = wall_us_from_ticks(__builtin_ia32_rdtsc(), g_tsc_us_per_tick);
  if (now < t_last_wall_us) return t_last_wall_us;
  t_last_wall_us = now;
  return now;
}

// Fixed point scale from a measured pair of intervals; 0 when they cannot give one. Exposed for the fixture.
std::uint64_t tsc_scale_from(std::uint64_t ticks, std::uint64_t counter_ticks, std::uint64_t counter_hz) noexcept;

} // namespace gt4::perf
