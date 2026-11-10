// TASK-090 CoreGS 3.0: the snapshot channel between the EE hooks and the GS stream. See coregs3_vram.hpp.
//
// One atomic function pointer and two counters: the note_* entry points run on the EE guest path (once per K7 marker and
// once per captured draw) and must stay one load and one indirect call, with no allocation, no lock and no file I/O.
// The sink the GS backend installs does the stream-order work on the GS side.
#include "coregs3_vram.hpp"

#include <atomic>

namespace gt4::coregs3::vram {
namespace {

std::atomic<SnapshotSink> g_sink{nullptr};
std::atomic<std::uint64_t> g_requests{0};
std::atomic<std::uint64_t> g_delivered{0};
std::atomic<std::uint64_t> g_pass_mask{0};
std::atomic<bool> g_pass_filter{false};
std::atomic<bool> g_draw_marks{true};

void deliver(const SnapshotRequest &request) noexcept {
  const SnapshotSink sink = g_sink.load(std::memory_order_acquire);
  if (sink == nullptr) return;
  g_requests.fetch_add(1, std::memory_order_relaxed);
  sink(request);
  g_delivered.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

void set_snapshot_sink(SnapshotSink sink) noexcept { g_sink.store(sink, std::memory_order_release); }

bool snapshot_sink_installed() noexcept { return g_sink.load(std::memory_order_acquire) != nullptr; }

void set_pass_mark_mask(std::uint64_t pass_mask, bool filter) noexcept {
  g_pass_mask.store(pass_mask, std::memory_order_relaxed);
  g_pass_filter.store(filter, std::memory_order_release);
}

void set_draw_marks_enabled(bool enabled) noexcept { g_draw_marks.store(enabled, std::memory_order_release); }

bool draw_marks_enabled() noexcept { return g_draw_marks.load(std::memory_order_acquire); }

void note_pass_begin(std::uint32_t pass, std::uint64_t vblank) noexcept {
  if (g_pass_filter.load(std::memory_order_acquire) && (pass >= 64u || (g_pass_mask.load(std::memory_order_relaxed) & (1ull << pass)) == 0))
    return;
  SnapshotRequest request;
  request.kind = kRecordPassBegin;
  request.pass = pass;
  request.vblank = vblank;
  deliver(request);
}

void note_draw_textures(std::uint32_t sequence, std::uint64_t vblank, const Window *windows,
                        std::uint32_t window_count) noexcept {
  if (!g_draw_marks.load(std::memory_order_acquire)) return;
  if (windows == nullptr || window_count == 0) return;
  SnapshotRequest request;
  request.kind = kRecordDrawTexture;
  request.label = sequence;
  request.vblank = vblank;
  request.window_count = window_count > kMaxCaptureWindows ? kMaxCaptureWindows : window_count;
  for (std::uint32_t index = 0; index < request.window_count; ++index) request.windows[index] = windows[index];
  deliver(request);
}

SnapshotCounters snapshot_counters() noexcept {
  SnapshotCounters snapshot;
  snapshot.requests = g_requests.load(std::memory_order_relaxed);
  snapshot.delivered = g_delivered.load(std::memory_order_relaxed);
  return snapshot;
}

void reset_snapshot_counters() noexcept {
  g_requests.store(0, std::memory_order_relaxed);
  g_delivered.store(0, std::memory_order_relaxed);
}

}  // namespace gt4::coregs3::vram
