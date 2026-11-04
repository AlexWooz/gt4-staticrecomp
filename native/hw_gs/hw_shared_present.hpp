// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
#pragma once
// TASK-174: the GS thread's rule before it draws a picture for the presentation thread, on a device with one queue in its graphics
// family (hw_gs_backend.cpp, present_on_shared_queue). The GS thread never waits for the presentation thread: when that thread is
// behind, the frame is not shown, and counted. Host only, so the fixture (hw_queue_lock_fixture.cpp) drives the same rule against a
// fake queue.
//
// Each frame slot owns a picture, a fence and two semaphores. A slot handed to the presentation thread is "held" until that thread's
// last queue call for it returned (the copy and present, or the release when no swapchain image came). After that its fence tells
// whether the copy of its picture ran on the GPU; that check is the backend's, done only on a slot that is not held, so the two
// threads never use one fence at the same time.

#include <cstddef>
#include <cstdint>

namespace gt4::hw {

// What the presentation thread holds, read under the backend's pacer mutex.
struct PresenterLoad {
  std::size_t queued = 0;        // pictures handed over and not taken yet
  bool busy = false;             // one picture taken, its queue calls not finished
  std::uint32_t slots_held = 0;  // one bit per frame slot: queued or busy
};

enum class HandOverVerdict {
  draw,              // draw the picture and hand it over
  presenter_behind,  // the presentation thread already holds enough pictures: this frame is not shown
  slot_held,         // the slot's last picture is still with the presentation thread: this frame is not shown
};

// `spaced`: the frame is shown at its place (TASK-082, frames left out); up to `spaced_queue_limit` pictures may wait for their place,
// as on the second queue (present_paced). Otherwise the picture goes at once, and at most `in_flight_limit` may be on their way
// (IN-516's two presentations).
constexpr HandOverVerdict hand_over_verdict(const PresenterLoad &load, std::uint32_t slot, bool spaced, std::size_t spaced_queue_limit,
                                            std::size_t in_flight_limit) {
  if ((load.slots_held >> slot) & 1u) return HandOverVerdict::slot_held;
  const std::size_t on_the_way = load.queued + (load.busy ? 1u : 0u);
  const bool behind = spaced ? load.queued >= spaced_queue_limit : on_the_way >= in_flight_limit;
  return behind ? HandOverVerdict::presenter_behind : HandOverVerdict::draw;
}

}  // namespace gt4::hw
