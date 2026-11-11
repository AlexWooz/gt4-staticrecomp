// TASK-090 CoreGS 3.0: the host presentation clock. See coregs3_host_clock.hpp for the contract.
#include "coregs3_host_clock.hpp"

#include <algorithm>
#include <limits>

namespace gt4::coregs3 {
namespace {

// Integer nanoseconds cannot represent 1/120 s exactly (8,333,333.3...), so a slot decision treats a difference under
// this as on time. The periods are 7-50 ms; one microsecond is far below a display period and far above the rounding.
constexpr std::int64_t kSlotToleranceNs = 1000;

// Guest ticks to nanoseconds on the 3600-tick second, saturating instead of wrapping. The clock only converts display
// periods and the stale bound (tens of ticks), but a corrupted step must not become a negative time.
std::int64_t ticks_to_ns(std::uint64_t ticks) noexcept {
  constexpr std::uint64_t limit =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) / kPresentationTicksPerSecond;
  if (ticks > limit) return std::numeric_limits<std::int64_t>::max();
  return static_cast<std::int64_t>(ticks * 1000000000ull / kPresentationTicksPerSecond);
}

std::uint64_t saturating_add(std::uint64_t left, std::uint64_t right) noexcept {
  const std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
  return left > max - right ? max : left + right;
}

std::uint64_t saturating_multiply(std::uint64_t left, std::uint64_t right) noexcept {
  if (left == 0 || right == 0) return 0;
  const std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
  return left > max / right ? max : left * right;
}

}  // namespace

HostPresentationClock::HostPresentationClock(std::uint32_t rate_hz) noexcept
    : rate_hz_(presentation_rate_supported(rate_hz) ? rate_hz : 0),
      step_ticks_(presentation_rate_supported(rate_hz) ? presentation_step_ticks(rate_hz) : 0) {}

void HostPresentationClock::reset() noexcept {
  have_last_instant_ = false;
  last_instant_ticks_ = 0;
  have_last_show_ = false;
  last_show_ns_ = 0;
  plans_ = 0;
  planned_ = 0;
  stale_ = 0;
  skipped_ = 0;
  late_ = 0;
  duplicates_ = 0;
}

std::int64_t HostPresentationClock::period_ns() const noexcept {
  return active() ? ticks_to_ns(step_ticks_) : 0;
}

HostPresentationClock::Plan HostPresentationClock::plan(const PresentationClock::Due &due, std::int64_t now_ns,
                                                        std::uint32_t capacity) noexcept {
  Plan out;
  if (!active() || due.count == 0 || due.step == 0) return out;
  const std::uint32_t batch_count = std::min<std::uint32_t>(due.count, kMaxTicks);
  const std::uint64_t newest = saturating_add(
      due.first, saturating_multiply(static_cast<std::uint64_t>(batch_count) - 1u, due.step));
  const std::uint32_t keep_capacity = std::min<std::uint32_t>(capacity, kMaxTicks);

  // The instants already planned (a replayed or overlapping batch) are never shown twice; a tick more than one guest
  // frame older than the batch's newest tick is stale (the guest has already replaced it).
  Tick candidates[kMaxTicks];
  std::uint32_t candidate_count = 0;
  for (std::uint32_t index = 0; index < batch_count; ++index) {
    const std::uint64_t instant = saturating_add(due.first, static_cast<std::uint64_t>(index) * due.step);
    if (have_last_instant_ && instant <= last_instant_ticks_) {
      out.duplicates += 1;
      continue;
    }
    if (newest - instant > kStaleTicks) {
      out.stale += 1;
      continue;
    }
    if (candidate_count >= kMaxTicks) {
      out.skipped += 1;
      continue;
    }
    candidates[candidate_count].index = index;
    candidates[candidate_count].instant_ticks = instant;
    candidate_count += 1;
  }
  // The presenter can only take so many pictures: keep the newest instants, whose images the timeline says are current.
  const std::uint32_t dropped_for_capacity = candidate_count > keep_capacity ? candidate_count - keep_capacity : 0;
  out.skipped += dropped_for_capacity;

  const std::int64_t period = period_ns();
  for (std::uint32_t candidate = dropped_for_capacity; candidate < candidate_count; ++candidate) {
    std::int64_t show_ns = now_ns;
    if (have_last_show_) {
      // The next slot on the display grid: one period after the last show, or the next one at/after now when the grid
      // already slipped (a slot passed with no new tick, counted late).
      const std::int64_t behind = now_ns - last_show_ns_;
      std::int64_t slots = 1;  // a tick never shares a display period with the one before it
      if (behind > period + kSlotToleranceNs) {
        slots = behind / period;
        if (behind % period > kSlotToleranceNs) slots += 1;
        out.late += 1;  // a display slot went by with no new tick: leave it empty, keep the grid's phase
      }
      show_ns = last_show_ns_ + slots * period;
    }
    Tick scheduled = candidates[candidate];
    scheduled.show_ns = show_ns;
    out.ticks[out.count++] = scheduled;
    last_show_ns_ = show_ns;
    have_last_show_ = true;
  }

  if (!have_last_instant_ || newest > last_instant_ticks_) {
    last_instant_ticks_ = newest;
    have_last_instant_ = true;
  }
  plans_ += 1;
  planned_ += out.count;
  stale_ += out.stale;
  skipped_ += out.skipped;
  late_ += out.late;
  duplicates_ += out.duplicates;
  return out;
}

}  // namespace gt4::coregs3
