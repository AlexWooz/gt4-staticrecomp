// Copyright 2026 GT4Recomp contributors. Licensed under the MIT License (see LICENSE).
// Bench switch: GT4_THREAD_CPUS="gs=3,vu1=c,ee=c" keeps a named thread on the logical CPUs of a hexadecimal mask, to measure
// on a 2-core, 4-thread processor whether the place of the three busy threads matters (IN-488). Unset: Windows decides.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace gt4::thread_placement {

inline std::uint64_t mask_in(const char *list, const char *name) {
  const std::size_t length = std::strlen(name);
  for (const char *at = list; at && *at;) {
    if (std::strncmp(at, name, length) == 0 && at[length] == '=') return std::strtoull(at + length + 1, nullptr, 16);
    at = std::strchr(at, ',');
    if (at) ++at;
  }
  return 0;
}

inline void apply(const char *name) {
#ifdef _WIN32
  const std::uint64_t mask = mask_in(std::getenv("GT4_THREAD_CPUS"), name);
  if (mask == 0) return;
  const bool ok = SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(mask)) != 0;
  std::fprintf(stderr, "[threads] %s on CPUs %llx%s\n", name, static_cast<unsigned long long>(mask), ok ? "" : " (refused)");
#else
  (void)name;
#endif
}

}  // namespace gt4::thread_placement
