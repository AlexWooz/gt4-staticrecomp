#include "perf_clock.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <intrin.h>
#define NOMINMAX
#include <windows.h>

namespace gt4::perf {

std::uint64_t steady_wall_us() noexcept {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::uint64_t tsc_scale_from(std::uint64_t ticks, std::uint64_t counter_ticks, std::uint64_t counter_hz) noexcept {
  if (ticks == 0 || counter_ticks == 0 || counter_hz == 0) return 0;
  // us per tick = (counter_ticks / counter_hz) * 1e6 / ticks, in 0.64 fixed point. A TSC below 1 MHz cannot be real and
  // would not fit the fraction.
  const long double us = static_cast<long double>(counter_ticks) * 1e6L / static_cast<long double>(counter_hz);
  const long double per_tick = us / static_cast<long double>(ticks);
  if (per_tick >= 1.0L) return 0;
  return static_cast<std::uint64_t>(per_tick * 18446744073709551616.0L);
}

namespace {

bool invariant_tsc() noexcept {
  int registers[4]{};
  __cpuid(registers, static_cast<int>(0x80000000u));
  if (static_cast<unsigned>(registers[0]) < 0x80000007u) return false;
  __cpuid(registers, static_cast<int>(0x80000007u));
  return (registers[3] & (1 << 8)) != 0;
}

// About 2 ms at process start, against the performance counter.
std::uint64_t calibrate() noexcept {
  char value[16]{};
  if (GetEnvironmentVariableA("GT4_PERF_CLOCK", value, sizeof value) > 0 && std::strcmp(value, "steady") == 0) return 0;
  if (!invariant_tsc()) return 0;
  LARGE_INTEGER hz{}, start{}, now{};
  if (!QueryPerformanceFrequency(&hz) || !QueryPerformanceCounter(&start)) return 0;
  const std::uint64_t first = __builtin_ia32_rdtsc();
  const LONGLONG wanted = hz.QuadPart / 500;
  do {
    QueryPerformanceCounter(&now);
  } while (now.QuadPart - start.QuadPart < wanted);
  const std::uint64_t last = __builtin_ia32_rdtsc();
  return tsc_scale_from(last - first, static_cast<std::uint64_t>(now.QuadPart - start.QuadPart),
                        static_cast<std::uint64_t>(hz.QuadPart));
}

} // namespace

std::uint64_t g_tsc_us_per_tick = calibrate();

} // namespace gt4::perf
