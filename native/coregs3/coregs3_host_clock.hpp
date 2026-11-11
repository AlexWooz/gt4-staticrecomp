// TASK-090 CoreGS 3.0: the host presentation clock (docs/COREGS3-PLAN.md §5.5).
//
// The guest is a fixed 60 Hz machine and stays bit-exact; its frames are captured at VBlank. The presentation timeline
// (coregs3_presentation.hpp) samples that history on a uniform guest-tick grid, and every grid instant becomes due when a
// guest frame arrives. The window, however, must present at the display's own rate (120/144 Hz with GT4_COREGS3_PRESENT_HZ),
// not once per guest VBlank. This class turns the due instants of one VBlank into host presentation times:
//
//   - one tick = 1/3600 s, so the display period is an exact number of guest ticks at every supported rate;
//   - the due ticks are placed on a display grid one period apart. The first tick of a batch takes the next grid slot at
//     or after the time the batch became available; a slot that passed with no new tick is counted late and left empty,
//     which keeps the grid's phase instead of bunching the following ticks;
//   - a tick more than one guest frame behind the newest tick of its batch is stale: the guest already replaced that
//     image, so it is dropped and counted, never drawn late;
//   - when more ticks are due than the presenter can take (a stall, or a caller limit), the oldest are dropped and
//     counted; the newest tick is the image the timeline says is current;
//   - an instant is never scheduled twice and a show time never moves backwards.
//
// The grid re-anchors at the next free slot instead of assuming the guest runs at exactly 3600 ticks per wall second:
// a guest slower than 60 Hz (a heavy scene) then still presents each image as soon as a display slot exists, only less
// often, and the clock never drifts into calling fresh ticks stale. Host only (it compares a host steady clock); the
// fixture drives it with explicit nanosecond times.
#pragma once

#include <cstdint>

#include "coregs3_presentation.hpp"

namespace gt4::coregs3 {

class HostPresentationClock {
 public:
  // At most the presentation clock's stall bound.
  static constexpr std::uint32_t kMaxTicks = PresentationClock::kMaxDuePerFrame;
  // A tick further behind the newest tick of its batch than one guest frame is stale.
  static constexpr std::uint64_t kStaleTicks = kPresentationTicksPerGuestFrame;

  struct Tick {
    std::uint32_t index = 0;             // index within the PresentationClock::Due batch
    std::uint64_t instant_ticks = 0;     // the guest tick of the presentation instant
    std::int64_t show_ns = 0;            // the host steady-clock time to present it, in nanoseconds
  };

  struct Plan {
    std::uint32_t count = 0;
    Tick ticks[kMaxTicks];
    std::uint64_t stale = 0;       // this batch: more than one guest frame behind the newest due tick
    std::uint64_t skipped = 0;     // this batch: dropped because more were due than `capacity`
    std::uint64_t late = 0;        // this batch: a display slot passed with no new tick (the grid skipped one)
    std::uint64_t duplicates = 0;  // this batch: instants at or before the last one already planned
  };

  // An unsupported rate (not one of presentation_rate_supported) constructs an inert clock: no plan ticks.
  explicit HostPresentationClock(std::uint32_t rate_hz = 0) noexcept;
  void reset() noexcept;
  bool active() const noexcept { return rate_hz_ != 0; }
  std::uint32_t rate_hz() const noexcept { return rate_hz_; }
  std::uint64_t step_ticks() const noexcept { return step_ticks_; }
  std::int64_t period_ns() const noexcept;  // one display period, 0 when inert
  bool started() const noexcept { return have_last_show_; }

  // Schedules the instants of `due`, a PresentationClock observation taken at `now_ns` (the host steady clock). At most
  // `capacity` (clamped to kMaxTicks) instants are emitted, the newest kept; the rest are counted in `skipped`. A batch
  // that does not advance past the last planned instant only counts `duplicates`.
  Plan plan(const PresentationClock::Due &due, std::int64_t now_ns, std::uint32_t capacity = kMaxTicks) noexcept;

  // Session totals (the same numbers the plan returns, accumulated since reset()).
  std::uint64_t plans() const noexcept { return plans_; }
  std::uint64_t planned() const noexcept { return planned_; }
  std::uint64_t stale() const noexcept { return stale_; }
  std::uint64_t skipped() const noexcept { return skipped_; }
  std::uint64_t late() const noexcept { return late_; }
  std::uint64_t duplicates() const noexcept { return duplicates_; }

 private:
  std::uint32_t rate_hz_ = 0;
  std::uint64_t step_ticks_ = 0;
  bool have_last_instant_ = false;
  std::uint64_t last_instant_ticks_ = 0;  // the last instant of the last batch, planned or dropped
  bool have_last_show_ = false;
  std::int64_t last_show_ns_ = 0;
  std::uint64_t plans_ = 0;
  std::uint64_t planned_ = 0;
  std::uint64_t stale_ = 0;
  std::uint64_t skipped_ = 0;
  std::uint64_t late_ = 0;
  std::uint64_t duplicates_ = 0;
};

}  // namespace gt4::coregs3
