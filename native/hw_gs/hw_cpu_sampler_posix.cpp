// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// TASK-180: the TASK-027 CPU survey sampler outside Windows. hw_cpu_sampler.cpp samples thread instruction pointers with
// SuspendThread and GetThreadContext, which have no portable form; here GT4_CPU_SAMPLER is reported as unavailable and
// every other call does nothing. Profiles on Android use simpleperf instead.
#include "hw_cpu_sampler.hpp"

#include <cstdlib>

namespace gt4::hw::sampler {

void name_current_thread(const char *name) { (void)name; }

bool start_from_env() {
  if (const char *wanted = std::getenv("GT4_CPU_SAMPLER"); wanted != nullptr && *wanted != '\0')
    std::fprintf(stderr, "[sampler] GT4_CPU_SAMPLER is Windows-only; use simpleperf on Android\n");
  return false;
}

void note_vblank(std::uint64_t vblank) { (void)vblank; }

void finish(std::FILE *log) { (void)log; }

}  // namespace gt4::hw::sampler
