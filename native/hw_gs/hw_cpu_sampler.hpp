// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
//
// TASK-027 CPU survey: a statistical CPU profiler of the whole game process (diagnostic, off by default).
//
// GT4_CPU_SAMPLER=<file> starts a sampler thread that, every GT4_CPU_SAMPLER_PERIOD_US microseconds (default 1000),
// visits every other thread of the process that used CPU since its last visit: it suspends the thread, copies its
// registers and the top of its stack, resumes it, and only then walks the copied stack with the x64 unwind tables
// (no lock is taken while a thread is suspended). Samples are kept only between the VBlanks GT4_CPU_SAMPLER_FROM and
// GT4_CPU_SAMPLER_TO (default: the whole run). At the end of the session the raw samples (absolute return addresses
// per stack), the module table and the thread table go to <file>; scripts/cpu_sampler_report.py turns them into
// per-thread function profiles with llvm-symbolizer (the build needs line tables: -gline-tables-only and /DEBUG).
#pragma once

#include <cstdint>
#include <cstdio>

namespace gt4::hw::sampler {

// Tags the calling thread in the report ("ee", "gs-worker"). Cheap; works whether or not the sampler runs.
void name_current_thread(const char *name);

// Starts the sampler thread when GT4_CPU_SAMPLER is set (once; later calls do nothing). Returns true when it runs.
bool start_from_env();

// The guest VBlank counter, for the sampling window.
void note_vblank(std::uint64_t vblank);

// Stops the sampler and writes the raw report (once). The summary line goes to `log`.
void finish(std::FILE *log);

}  // namespace gt4::hw::sampler
