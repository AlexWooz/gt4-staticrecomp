// TASK-090 CoreGS 3.0 live world: the in-memory per-job texture store on hand-authored jobs and windows. It pins the
// join the live renderer depends on: an item's packet address -> the VU1 job it was posted as -> the texture bytes the
// job samples, with the staleness, replacement, refusal and retention rules, and every refusal counted.
// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
#include "coregs3_live_world.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char *what) {
  if (!condition) {
    std::fprintf(stderr, "live_world fixture: FAIL %s\n", what);
    ++failures;
  }
}

// A 4 MiB shadow whose byte at offset B is (B * 7 + 3) & 0xFF, so a copied window is verifiable byte for byte.
std::vector<std::uint8_t> make_slm() {
  std::vector<std::uint8_t> slm(4u * 1024u * 1024u);
  for (std::size_t index = 0; index < slm.size(); ++index)
    slm[index] = static_cast<std::uint8_t>((index * 7u + 3u) & 0xFFu);
  return slm;
}

std::vector<std::uint8_t> window_bytes(const std::vector<std::uint8_t> &slm, const gt4::coregs3::vram::Window *windows,
                                       std::uint32_t count) {
  std::vector<std::uint8_t> bytes;
  for (std::uint32_t index = 0; index < count; ++index) {
    const std::size_t first = static_cast<std::size_t>(windows[index].first_block) * 256u;
    const std::size_t size = static_cast<std::size_t>(windows[index].block_count) * 256u;
    bytes.insert(bytes.end(), slm.begin() + static_cast<std::ptrdiff_t>(first),
                 slm.begin() + static_cast<std::ptrdiff_t>(first + size));
  }
  return bytes;
}

void reset_store(bool active) {
  gt4::coregs3::live_world::reset();
  gt4::coregs3::live_world::set_active(active);
}

}  // namespace

int main() {
  using namespace gt4::coregs3::live_world;
  const std::vector<std::uint8_t> slm = make_slm();

  // The default environment (no GT4_COREGS3_LIVE_WORLD) enables the store.
  configure_from_environment();
  check(configured(), "configured after configure_from_environment");
  check(active(), "active by default");

  // ---- The join: two addresses posted as one job share its record.
  reset_store(true);
  note_job_source(100, 0x1000);
  note_job_source(100, 0x1004);
  note_job_source(100, 0x1000);  // a repeated source does not duplicate the address
  gt4::coregs3::vram::Window windows[2] = {{0x0100u, 2u}, {0x0200u, 1u}};
  note_job_textures(100, 10, windows, 2, slm.data());
  const std::shared_ptr<const JobTexture> first = find(0x1000);
  check(first != nullptr, "address 0x1000 has a record");
  const std::shared_ptr<const JobTexture> second = find(0x1004);
  check(second != nullptr, "address 0x1004 has a record");
  check(first == second, "both addresses share one immutable record");
  check(first != nullptr && first->job == 100 && first->vblank == 10, "record names its job and VBlank");
  check(first != nullptr && first->windows.size() == 2, "record keeps both windows");
  const std::vector<std::uint8_t> expected = window_bytes(slm, windows, 2);
  check(first != nullptr && first->bytes == expected, "record bytes are the windows' SLM content");

  // ---- A missing address is counted, never guessed.
  check(find(0x2000) == nullptr, "an unposted address has no record");
  check(counters().lookup_missing.load() >= 1, "the miss is counted");

  // ---- A second job for another address; the first record stays.
  note_job_source(101, 0x2000);
  gt4::coregs3::vram::Window window2[1] = {{0x0300u, 1u}};
  note_job_textures(101, 11, window2, 1, slm.data());
  check(find(0x2000) != nullptr, "the second job's address has a record");
  check(find(0x1000) != nullptr, "the first job's address keeps its record");

  // ---- Replacement: a new job for the same address wins, and the previously returned record stays alive.
  note_job_source(102, 0x1000);
  gt4::coregs3::vram::Window window3[1] = {{0x0400u, 1u}};
  note_job_textures(102, 12, window3, 1, slm.data());
  const std::shared_ptr<const JobTexture> replaced = find(0x1000);
  check(replaced != nullptr && replaced->job == 102 && replaced->vblank == 12, "the newest job replaces the address");
  check(first != nullptr && first->job == 100, "the previously returned record is still alive and unchanged");

  // ---- Refusals: no windows, a malformed window, more windows than the capture carries, and an unlinked job.
  reset_store(true);
  note_job_source(200, 0x5000);
  note_job_textures(200, 20, nullptr, 0, slm.data());
  check(counters().jobs_refused.load() == 1, "a job without windows is refused");
  gt4::coregs3::vram::Window malformed[1] = {{gt4::coregs3::vram::kBlocksTotal, 1u}};
  note_job_textures(200, 20, malformed, 1, slm.data());
  check(counters().jobs_refused.load() == 2, "a window outside the 4 MiB image is refused");
  gt4::coregs3::vram::Window too_many[gt4::coregs3::vram::kMaxCaptureWindows + 1] = {};
  note_job_textures(200, 20, too_many, gt4::coregs3::vram::kMaxCaptureWindows + 1, slm.data());
  check(counters().jobs_refused.load() == 3, "a job over the window bound is refused");
  check(find(0x5000) == nullptr, "a refused job publishes no record");
  note_job_textures(201, 21, window2, 1, slm.data());
  check(counters().jobs_unlinked.load() == 1, "a job with no posted source is counted and dropped");

  // ---- Inactive: both entry points are inert.
  reset_store(false);
  note_job_source(300, 0x6000);
  note_job_textures(300, 30, window2, 1, slm.data());
  check(find(0x6000) == nullptr, "an inactive store publishes nothing");

  // ---- Retention: a record older than the window is pruned; a newer one stays.
  reset_store(true);
  note_job_source(400, 0x7000);
  note_job_textures(400, 100, window2, 1, slm.data());
  note_job_source(401, 0x7004);
  const std::uint64_t newer = 100 + kRetentionVblanks + 2;
  note_job_textures(401, newer, window2, 1, slm.data());
  check(find(0x7000) == nullptr, "a record past the retention window is pruned from the address table");
  check(find(0x7004) != nullptr, "a record inside the window stays");
  check(counters().records_pruned.load() >= 1, "the prune is counted");

  // ---- The pending-source bound forgets the oldest unexecuted job instead of growing without limit.
  reset_store(true);
  for (std::uint32_t index = 0; index < kMaxPendingJobs + 8u; ++index)
    note_job_source(1000u + index, 0x8000u + index * 4u);
  check(counters().sources_dropped.load() != 0, "pending sources are bounded and the loss is counted");
  note_job_textures(1000, 40, window2, 1, slm.data());
  check(counters().jobs_unlinked.load() == 1, "a forgotten source makes its job unlinked");

  if (failures == 0) {
    std::printf("coregs3_live_world: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "coregs3_live_world: %d check(s) failed\n", failures);
  return 1;
}
