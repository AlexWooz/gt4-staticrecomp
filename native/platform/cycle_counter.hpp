// SPDX-License-Identifier: MIT
// TASK-180: the CPU's free-running cycle counter for short timings (profiles, transport counters), header-only so the
// renderer libraries need no link dependency. x86-64: RDTSC (what __rdtsc and __builtin_ia32_rdtsc compile to, so the
// Windows build is unchanged); arm64: the virtual counter CNTVCT_EL0. Its rate is not known here: callers calibrate
// it against a wall clock, as they already do for the TSC.
#pragma once

#include <cstdint>

namespace gt4::platform {

inline std::uint64_t cycle_counter() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
  return __builtin_ia32_rdtsc();
#elif defined(__aarch64__)
  std::uint64_t value;
  __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(value));
  return value;
#else
#error "gt4::platform::cycle_counter: no cycle counter for this CPU"
#endif
}

// True when cycle_counter() runs at a constant rate across cores and power states, so it can stand in for a wall clock
// once calibrated (x86-64: the invariant TSC bit is checked by the caller; arm64: the generic timer always is).
inline constexpr bool cycle_counter_is_generic_timer() noexcept {
#if defined(__aarch64__)
  return true;
#else
  return false;
#endif
}

}  // namespace gt4::platform
