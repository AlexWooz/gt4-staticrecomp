// TASK-090 CoreGS 3.0: the presentation timeline (owner requirement, 2026-11-05: variable frame rate from the start).
//
// The guest stays a 60 Hz machine: physics, logic, timing and every EE-visible read-back are bit-exact and are never
// retimed. What changes is presentation. The capture records each guest frame with its guest timestamp and a stable
// identity per drawn object; the native renderer runs at its own rate (60, 120, 144 Hz or VRR) and, for a presentation
// instant, interpolates each object's transform between the last two captured guest frames. That costs about one guest
// frame of latency (the standard price of interpolation); extrapolation can be added later behind a switch.
//
// This header is the contract the live renderer will use. It is pure data and pure functions so the fixture can pin it
// without a GPU or a guest, and it is deliberately independent of the capture file format: a `TimelineFrame` can be built
// from a `FrameSnapshot` (offline) or published by the EE hooks (live).
//
// Fallback is part of the contract, not an afterthought: an object present in only one of the two frames is drawn with
// that frame's transform (no interpolation), an object that appears in neither is not drawn, and a frame pair whose
// timestamps are not ordered or too far apart is refused as a whole, which tells the caller to present the newer frame
// un-interpolated.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "coregs3_render.hpp"

namespace gt4::coregs3 {

// The guest's clock for one captured frame. `vblank` is the donor's VBlank counter (60 Hz, the unit the bench and every
// gate use); `ticks` is the EE cycle counter at that VBlank, which is the finer clock the renderer needs to place a
// presentation instant between two frames without assuming that the guest is exactly 1/60 s per frame.
struct GuestTime {
  std::uint64_t vblank = 0;
  std::uint64_t ticks = 0;
  bool operator==(const GuestTime &) const = default;
};

// Stable identity of one drawn object across frames: the shape's `Cmd` address, the address of every item packet and the
// material record address, hashed. Those are the model's own data as the game keeps it in guest RAM, so the same object
// yields the same key on the next frame even though its matrix changed (measured: the car shapes keep their addresses
// across frames; the matrix is what moves). `instance` separates the (rare) case of the same key drawn twice in a frame,
// in draw order.
struct ObjectKey {
  std::uint64_t id = 0;
  std::uint32_t instance = 0;
  bool operator==(const ObjectKey &) const = default;
};

ObjectKey object_key(const CapturedDraw &draw) noexcept;

// One object of one frame: its identity and the composed model matrix (the `0xC1` block: rows 0-2 scaled by the shape's
// `value`, row 3 as uploaded). Row-major, 16 floats.
struct ObjectTransform {
  ObjectKey key;
  float matrix[16] = {};
  std::uint32_t sequence = 0;  // the draw's ordinal in the frame, for the fallback and for diagnostics
};

struct TimelineFrame {
  GuestTime time;
  std::uint32_t index = 0;  // capture-local frame ordinal
  std::vector<ObjectTransform> objects;
};

// Builds one timeline frame from a capture frame. Draws whose matrix is degenerate are left out (the renderer would
// refuse them anyway) and counted by the caller through `objects.size()`.
TimelineFrame timeline_frame(const FrameSnapshot &frame, GuestTime time);

// A short ring of the last N frames, newest last. The live renderer keeps it behind a lock; offline tools just fill it.
class FrameRing {
 public:
  explicit FrameRing(std::size_t capacity = 8);

  void push(const TimelineFrame &frame);
  void clear() noexcept;
  std::size_t size() const noexcept { return frames_.size(); }
  std::size_t capacity() const noexcept { return capacity_; }
  // 0 is the newest frame, 1 the one before it. Throws std::out_of_range.
  const TimelineFrame &at(std::size_t age) const;

  struct Pair {
    const ObjectTransform *older = nullptr;
    const ObjectTransform *newer = nullptr;
  };
  // Pairs the objects of two frames by (id, instance), in the older frame's order. Objects present in only one frame are
  // counted, not paired: the renderer draws them with their own frame's transform.
  void pair(std::size_t newer_age, std::size_t older_age, std::vector<Pair> &pairs, std::uint32_t &only_newer,
            std::uint32_t &only_older) const;

 private:
  std::size_t capacity_;
  std::vector<TimelineFrame> frames_;  // oldest first
};

// Linear interpolation of two 4x4 matrices entry by entry. Rotation matrices interpolated this way are not orthonormal;
// for a car body over one 60 Hz step the error is far below a pixel, and the deferred plan replaces this with a
// quaternion/dual-quaternion path when motion vectors arrive (phase 6).
void lerp_matrix(const float *older, const float *newer, float alpha, float *out) noexcept;

// alpha in [0,1] for a presentation instant `present_ticks` between two guest frames. Returns false when the pair cannot
// be used (ticks not increasing, present instant outside the pair): the caller then presents the newer frame as is.
bool interpolation_alpha(const GuestTime &older, const GuestTime &newer, std::uint64_t present_ticks,
                         float &alpha) noexcept;

}  // namespace gt4::coregs3
