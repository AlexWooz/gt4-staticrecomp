// TASK-090 CoreGS 3.0: the live frame ring of the debug-only `GT4_COREGS3=present` mode (docs/COREGS3-HOOKS.md §10.3).
//
// The hooks close one FrameSnapshot per guest VBlank in finish_frame() (native/coregs3/coregs3_hooks.cpp). In `present`
// mode that snapshot is published here with an O(1) vector swap; the live renderer (native/hw_gs/coregs3/) claims the
// newest complete frame the same way. The guest thread publishes and never waits for the renderer: the publish takes the
// ring lock with try_lock and counts a drop instead of blocking. Both sides swap whole std::vectors, so the buffers of a
// claimed frame travel back into the ring and are reused by the next capture without a per-frame allocation.
//
// No guest memory, no capture file and no CoreGS 2.0 target is involved: the ring holds the snapshot the observe-only
// hooks already build. This module is plain C++ so it can be linked into the game (gt4_coregs3) and into the renderer.
#pragma once

#include <atomic>
#include <cstdint>

#include "coregs3_scene.hpp"

namespace gt4::coregs3::present {

// A small ring, as the plan's §5.5 asks (default 8): the renderer claims the newest frame and the publisher keeps going.
inline constexpr std::uint32_t kRingFrames = 8;

// Set by the hooks' configure_from_environment() when GT4_COREGS3=present. False in the ref build (no hooks linked), so
// the renderer stays inert there and `off|shadow` are unchanged.
void set_active(bool active) noexcept;
bool active() noexcept;

// Producer (EE thread, finish_frame): moves `frame` into the ring with O(1) vector swaps. Never waits.
void publish(FrameSnapshot &frame) noexcept;

// Consumer (GS worker, after ComposeFrameGpu): swaps the newest complete frame into `frame`, or returns false when no
// frame was ever published (or none since the last claim). The caller's previous buffers go into the ring for reuse.
// Not noexcept: the ring lock may in principle fail to lock (std::system_error); the producer's try_lock never waits.
bool claim_latest(FrameSnapshot &frame);

// Counters. The producer updates published/dropped, the consumer the render figures, and the report reads them from
// another thread; every field is atomic with relaxed ordering, which is enough for diagnostics.
struct Counters {
  std::atomic<std::uint64_t> published{0};       // frames moved into the ring
  std::atomic<std::uint64_t> dropped_publish{0}; // publish found the ring busy (the frame was not published)
  std::atomic<std::uint64_t> claims{0};          // claims that returned a frame
  std::atomic<std::uint64_t> empty_claims{0};    // claims with no new frame
  std::atomic<std::uint64_t> passes{0};          // overlay passes submitted
  std::atomic<std::uint64_t> passes_refused{0};  // frames refused before a pass (no draws, capacity, init failure)
  std::atomic<std::uint64_t> draws{0};           // draw calls issued (one per captured draw)
  std::atomic<std::uint64_t> draws_refused{0};   // captured draws skipped (no matrix, mesh decode, capacity)
  // Draws still drawn but left on the base colour because the measured VU1 light block (or the normals/colours the
  // formula needs) was absent; and the uploaded vertices that kept that base colour for the same reason. Every draw
  // call is counted once in `draws`, so a lit draw is `draws - draws_unlit`.
  std::atomic<std::uint64_t> draws_unlit{0};
  std::atomic<std::uint64_t> vertices_unlit{0};
  std::atomic<std::uint64_t> vertices{0};        // vertices uploaded to the GPU for the drawn draws
  std::atomic<std::uint64_t> triangles{0};       // triangles in those draw calls
  std::atomic<std::uint64_t> last_vblank{0};     // guest VBlank of the last frame a pass ran for
};

const Counters &counters() noexcept;
void note_render(std::uint64_t vblank, std::uint32_t draws, std::uint32_t draws_refused, std::uint32_t draws_unlit,
                 std::uint64_t vertices, std::uint64_t vertices_unlit, std::uint64_t triangles) noexcept;
void note_pass_refused(std::uint32_t draws_refused) noexcept;

}  // namespace gt4::coregs3::present
