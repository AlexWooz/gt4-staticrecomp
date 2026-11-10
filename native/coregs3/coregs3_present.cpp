// TASK-090 CoreGS 3.0: the live frame ring of the debug-only `GT4_COREGS3=present` mode. See coregs3_present.hpp.
#include "coregs3_present.hpp"

#include <mutex>
#include <utility>

namespace gt4::coregs3::present {
namespace {

struct Slot {
  FrameSnapshot frame;
  std::uint64_t sequence = 0;  // 0: free; otherwise the publish ordinal of the frame it holds
};

struct Ring {
  std::mutex mutex;
  Slot slots[kRingFrames];
  std::uint32_t cursor = 0;       // producer only, under `mutex`
  std::uint64_t published = 0;    // producer only, under `mutex`
  std::uint64_t claimed = 0;      // consumer only, under `mutex`
};

Ring g_ring;
std::atomic<bool> g_active{false};
Counters g_counters;

}  // namespace

void set_active(bool active) noexcept { g_active.store(active, std::memory_order_relaxed); }
bool active() noexcept { return g_active.load(std::memory_order_relaxed); }
const Counters &counters() noexcept { return g_counters; }

void publish(FrameSnapshot &frame) noexcept {
  // A frame the hooks refused (a capacity or read failure) never reaches this call: the ring always carries a complete
  // snapshot. try_lock keeps the guest thread from ever waiting for the renderer.
  std::unique_lock<std::mutex> lock(g_ring.mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    g_counters.dropped_publish.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  Slot &slot = g_ring.slots[g_ring.cursor];
  // Swap the whole snapshot: the slot's previous vectors (a claimed frame's buffers) come back into `frame`, which the
  // hooks reset and refill for the next VBlank without allocating again.
  std::swap(slot.frame, frame);
  slot.sequence = ++g_ring.published;
  g_ring.cursor = (g_ring.cursor + 1u) % kRingFrames;
  g_counters.published.fetch_add(1, std::memory_order_relaxed);
}

bool claim_latest(FrameSnapshot &frame) {
  const std::lock_guard<std::mutex> lock(g_ring.mutex);
  Slot *newest = nullptr;
  for (Slot &slot : g_ring.slots) {
    if (slot.sequence == 0 || slot.sequence <= g_ring.claimed) continue;
    if (newest == nullptr || slot.sequence > newest->sequence) newest = &slot;
  }
  if (newest == nullptr) {
    g_counters.empty_claims.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  g_ring.claimed = newest->sequence;
  newest->sequence = 0;  // free for the publisher, which brings the claimed buffers back
  std::swap(newest->frame, frame);
  g_counters.claims.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void note_render(std::uint64_t vblank, std::uint32_t draws, std::uint32_t draws_refused, std::uint32_t draws_unlit,
                 std::uint64_t vertices, std::uint64_t vertices_unlit, std::uint64_t triangles) noexcept {
  g_counters.passes.fetch_add(1, std::memory_order_relaxed);
  g_counters.draws.fetch_add(draws, std::memory_order_relaxed);
  g_counters.draws_refused.fetch_add(draws_refused, std::memory_order_relaxed);
  g_counters.draws_unlit.fetch_add(draws_unlit, std::memory_order_relaxed);
  g_counters.vertices.fetch_add(vertices, std::memory_order_relaxed);
  g_counters.vertices_unlit.fetch_add(vertices_unlit, std::memory_order_relaxed);
  g_counters.triangles.fetch_add(triangles, std::memory_order_relaxed);
  g_counters.last_vblank.store(vblank, std::memory_order_relaxed);
}

void note_pass_refused(std::uint32_t draws_refused) noexcept {
  g_counters.passes_refused.fetch_add(1, std::memory_order_relaxed);
  g_counters.draws_refused.fetch_add(draws_refused, std::memory_order_relaxed);
}

}  // namespace gt4::coregs3::present
