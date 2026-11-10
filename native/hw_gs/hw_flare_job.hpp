#pragma once
// TASK-215 S4 diagnostic: the jobs whose VIF packet copied the flare visibility table (16 bytes at table offset 0), and
// whether the GS worker is executing one now. Header-only inline state so every fixture that links the sink or the
// renderer resolves it without a new library.
//
// The writer is the MTVU journal (post_table_patches, EE thread); the reader is the GS worker (execute_mtvu_job). Both are
// in the same process; the ring keeps the last 256 markers.

#include <array>
#include <atomic>
#include <cstdint>

namespace gt4::hw {

inline std::array<std::atomic<std::uint64_t>, 256> g_flare_jobs{};
inline std::atomic<std::uint32_t> g_flare_jobs_posted{0};
inline std::atomic<bool> g_flare_job_active{false};

inline void note_flare_job(std::uint64_t job) {
  const std::uint32_t index = g_flare_jobs_posted.fetch_add(1, std::memory_order_release);
  g_flare_jobs[index % g_flare_jobs.size()].store(job, std::memory_order_release);
}

inline bool is_flare_job(std::uint64_t job) {
  const std::uint32_t posted = g_flare_jobs_posted.load(std::memory_order_acquire);
  const std::uint32_t kept = posted < g_flare_jobs.size() ? posted : static_cast<std::uint32_t>(g_flare_jobs.size());
  for (std::uint32_t i = 0; i < kept; ++i) {
    if (g_flare_jobs[(posted - 1u - i) % g_flare_jobs.size()].load(std::memory_order_acquire) == job) return true;
  }
  return false;
}

}  // namespace gt4::hw
