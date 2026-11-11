// TASK-090 CoreGS 3.0 live world: the pixels of the captured world passes, held in memory for the live renderer.
//
// The `.gt4vram` sidecar solves the same problem offline: the world passes sample texture pages that the frame uploads
// *during* the course pass, so the frame-end snapshot and every pass-begin snapshot hold the wrong content (page 0x36D0
// is an advert atlas there, while the road draws sample road). The measured fix is the per-VU1-job snapshot taken on the
// GS worker right before the job executes (docs/COREGS3-HOOKS.md §24): every command posted before the job, its texture
// uploads included, has run and nothing of the job has. This module is that snapshot, kept in memory instead of written
// to a file, so `GT4_COREGS3=present` can bind the world textures live.
//
// Two channels join the item to its pixels:
//   * the EE posts every shape item's VIF packet as a VU1 job (`mtvu_runtime.cpp`, the same site that feeds the
//     `.gt4tap` source records). `note_job_source` stores `job -> {packet guest address}` in a bounded pending table.
//   * the GS worker executes the job and calls `note_job_textures` with the texture/palette windows its TEX0 writes bind
//     (coregs3_texwin.hpp), copied from the CPU shadow of GS local memory before the job runs. Every address the job was
//     posted for receives a shared, immutable record, so an item looks its pixels up by its own packet address.
//
// A record is kept for `kRetentionVblanks` guest frames after the worker's VBlank counter reached it. The live layer
// draws the newest captured frame and, when that frame's job has not executed yet (the worker lags the EE by about one
// VBlank), the previous record of the same address is still found: the same material uploaded into the same pages one
// frame earlier. That fallback is measured and counted by the caller (`vblank` is in the record), never guessed.
//
// Threading: `note_job_source` runs on the EE thread at job post, `note_job_textures` on the GS worker, `find` on the
// renderer's thread. One mutex serialises them; the per-frame traffic is a few thousand small entries, and the copies
// happen only for captured world jobs (the branch is off unless the live world is active).
//
// This header is dependency-free (coregs3_vram.hpp only) so it links into the game library, the GS worker and the
// renderer. It never reads or writes guest memory.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "coregs3_vram.hpp"

namespace gt4::coregs3::live_world {

// How many guest frames a job record stays available. The live layer draws the newest captured frame; its own job
// records may be one or two VBlanks behind on this machine, so a small window covers the lag and still bounds memory.
inline constexpr std::uint64_t kRetentionVblanks = 8;
// Bounds. A record over these is refused and counted, never truncated silently.
inline constexpr std::uint32_t kMaxRecords = 4096u;
inline constexpr std::uint64_t kMaxBytes = 96u * 1024u * 1024u;
// Pending sources whose job never executed (a dropped job): the oldest are forgotten past this bound.
inline constexpr std::uint32_t kMaxPendingJobs = 16384u;

// One job's texture/palette windows, copied before the job executed. Immutable once published.
struct JobTexture {
  std::uint64_t job = 0;
  std::uint64_t vblank = 0;  // the GS worker's VBlank counter when the job executed
  std::vector<vram::Window> windows;
  std::vector<std::uint8_t> bytes;  // sum(windows.block_count) * 256, windows concatenated in order
};

// Reads `GT4_COREGS3_LIVE_WORLD` (unset or `1` enables, `0` disables) and marks the store configured. Called by the
// hooks only in `GT4_COREGS3=present`, so a shadow run never starts the per-job capture.
void configure_from_environment() noexcept;
bool configured() noexcept;
bool active() noexcept;
void set_active(bool active) noexcept;  // tests and unusual build orders

// The sink the MTVU runtime forwards posted item packets to (see coregs3_tap.hpp's live source sink).
void note_job_source(std::uint64_t job, std::uint32_t guest_address) noexcept;

// Copies the job's windows from the GS local-memory shadow (`slm`, the 4 MiB image the worker owns) and links them to
// the addresses the job was posted for. Runs on the GS worker before the job executes.
void note_job_textures(std::uint64_t job, std::uint64_t vblank, const vram::Window *windows,
                       std::uint32_t window_count, const std::uint8_t *slm) noexcept;

// The newest record published for `guest_address`, or null when the address has none. The shared pointer keeps the
// record alive even when it is pruned from the store while the renderer still uses it.
std::shared_ptr<const JobTexture> find(std::uint32_t guest_address) noexcept;

struct Counters {
  std::atomic<std::uint64_t> sources = 0;         // pending source entries recorded
  std::atomic<std::uint64_t> sources_dropped{0}; // pending entries forgotten at the bound (their job never executed)
  std::atomic<std::uint64_t> job_records{0};     // job records published
  std::atomic<std::uint64_t> jobs_unlinked{0};   // a job executed with no pending source address: dropped, counted
  std::atomic<std::uint64_t> jobs_refused{0};    // no windows / malformed windows / over the bounds
  std::atomic<std::uint64_t> records_pruned{0};  // records dropped by the retention window
  std::atomic<std::uint64_t> bytes_held{0};      // bytes of the records currently held (approximate, atomically updated)
  std::atomic<std::uint64_t> lookups{0};
  std::atomic<std::uint64_t> lookup_missing{0};
};
const Counters &counters() noexcept;
void reset() noexcept;  // tests: clears the store and the counters

}  // namespace gt4::coregs3::live_world
