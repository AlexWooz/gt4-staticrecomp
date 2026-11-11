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

#include "coregs3_tap_file.hpp"
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
// The synthesized tap snapshot for one frame: bounded like the tap file (the world build refuses over it and counts).
inline constexpr std::uint64_t kMaxSnapshotBytes = 64u * 1024u * 1024u;

// One PATH1 GIF record the job emitted, kept so the world build can synthesize the `.gt4tap` view of the job: the
// per-item TEX0/TEX1/CLAMP state, the emitted UVs and the per-vertex fog. The bytes are the same records the GS worker
// buffers to execute the job.
struct GifRecord {
  std::uint64_t sequence = 0;  // global emission order (the offline tap's file order)
  std::uint64_t job = 0;       // the VU1 job that emitted it
  std::uint64_t vblank = 0;
  std::vector<std::uint8_t> bytes;
};

// One job's texture/palette windows and emitted records, copied before the job executed. Immutable once published.
struct JobTexture {
  std::uint64_t job = 0;
  std::uint64_t vblank = 0;  // the GS worker's VBlank counter when the job executed
  std::vector<vram::Window> windows;
  std::vector<std::uint8_t> bytes;  // sum(windows.block_count) * 256, windows concatenated in order
  std::vector<GifRecord> gif_records;  // the job's PATH1 emissions, in emission order
  std::vector<std::uint32_t> addresses;  // the packet addresses the job was posted for, in post order
};

// Reads `GT4_COREGS3_LIVE_WORLD` (unset or `1` enables, `0` disables) and marks the store configured. Called by the
// hooks only in `GT4_COREGS3=present`, so a shadow run never starts the per-job capture.
void configure_from_environment() noexcept;
bool configured() noexcept;
bool active() noexcept;  // the store is on in the present path (the car pass needs it too)
// The world draw switch: GT4_COREGS3_LIVE_WORLD (default 1). Independent of the store, so the car pass keeps its live
// texture records in the car-only A/B (GT4_COREGS3_LIVE_WORLD=0).
bool world_enabled() noexcept;
void set_active(bool active) noexcept;  // tests and unusual build orders (sets the store and the world switch)

// The sink the MTVU runtime forwards posted item packets to (see coregs3_tap.hpp's live source sink).
void note_job_source(std::uint64_t job, std::uint32_t guest_address) noexcept;

// Copies the job's windows from the GS local-memory shadow (`slm`, the 4 MiB image the worker owns) and links them to
// the addresses the job was posted for. Runs on the GS worker before the job executes. A job with no windows is still
// published when it has emitted records and posted addresses, so its fog/state reach the world build.
void note_job_textures(std::uint64_t job, std::uint64_t vblank, const vram::Window *windows,
                       std::uint32_t window_count, const std::uint8_t *slm) noexcept;

// Records the job's emitted PATH1 GIF records (the bytes the GS worker is about to execute), for the live tap snapshot.
// `data` holds the records' payloads concatenated; `offsets`/`sizes` name each record. Runs on the GS worker before
// `note_job_textures` for the same job.
void note_job_stream(std::uint64_t job, std::uint64_t vblank, const std::uint8_t *data, const std::uint32_t *offsets,
                     const std::uint32_t *sizes, std::uint32_t count) noexcept;

// The live SLM base: the GS local memory copied at the frame-end flush over `first_block..first_block+block_count`,
// the same role the `.gt4vram` vblank record plays offline (the job records take precedence; a page no job window
// covers falls back to this image, never to a zero fill). Called on the GS worker/EE thread at the VBlank flush.
void note_vram_base(std::uint64_t vblank, std::uint32_t first_block, std::uint32_t block_count,
                    const std::uint8_t *slm) noexcept;
// Copies the newest base image out; false when none was captured yet.
bool copy_vram_base(std::vector<std::uint8_t> &bytes, std::uint32_t &first_block, std::uint32_t &block_count,
                    std::uint64_t &vblank) noexcept;

// Synthesizes the `.gt4tap` view of the newest job records for one captured frame: one source record per requested
// packet address (stamped `frame_vblank`) and the jobs' emitted GIF records. `build_frame_texture_table` consumes it
// with `use_tap = true`, so the live world gets the emitted per-item state, UVs and per-vertex fog without a tap file.
// Only the addresses the frame's world items name are included, so the snapshot stays small at 60 Hz. Returns false
// when no address has a record or the snapshot exceeds `kMaxSnapshotBytes` (counted).
bool build_tap_snapshot(std::uint64_t frame_vblank, const std::vector<std::uint32_t> &addresses,
                        std::vector<std::uint8_t> &data, std::vector<TapFileRecord> &records) noexcept;

// The newest record published for `guest_address`, or null when the address has none. The shared pointer keeps the
// record alive even when it is pruned from the store while the renderer still uses it.
std::shared_ptr<const JobTexture> find(std::uint32_t guest_address) noexcept;

// The GS dither/frame state at the VBlank flush, for the captured post chain of the live present: DIMX, DTHE, COLCLAMP
// and the native display size the CT16 dither cell is defined over. The faithful CT16 model needs all four; the modern
// path (GT4_COREGS3_DITHER=0) ignores them. Noted by the backend flush from the renderer's register file and display
// layout (the stream-owning thread), read by the present layer's frame build.
struct FrameEnv {
  std::uint64_t dimx = 0;
  std::uint32_t dthe = 0;
  std::uint32_t colclamp = 1;
  std::uint32_t native_width = 0;
  std::uint32_t native_height = 0;
  bool valid = false;
};
void note_frame_env(const FrameEnv &env) noexcept;
FrameEnv frame_env() noexcept;

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
  std::atomic<std::uint64_t> stream_records{0};    // emitted GIF records stored for the tap snapshot
  std::atomic<std::uint64_t> snapshots{0};         // tap snapshots built
  std::atomic<std::uint64_t> snapshot_refused{0};  // snapshots refused by the byte bound
  std::atomic<std::uint64_t> snapshot_jobs{0};     // distinct jobs in the last snapshot
  std::atomic<std::uint64_t> snapshot_vertices{0}; // GIF records in the last snapshot
};
const Counters &counters() noexcept;
void reset() noexcept;  // tests: clears the store and the counters

}  // namespace gt4::coregs3::live_world
