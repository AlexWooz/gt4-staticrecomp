// CTest native_coregs3_host_clock: the host presentation clock of CoreGS 3.0 (native/coregs3/coregs3_host_clock.hpp),
// host only.
//   - every supported rate (20/30/45/60/120/144 Hz) has its exact display period in guest ticks and nanoseconds;
//   - a 120 Hz timeline of PresentationClock batches places every instant on a display grid one period apart (the first
//     slot at startup is missed and counted late, then the cadence is exact);
//   - a 144 Hz timeline over 30 VBlanks keeps every interval an integer number of display periods, with no tick lost,
//     no instant twice and no show time moving backwards;
//   - a tick more than one guest frame behind its batch's newest tick is stale and never shown; a batch larger than the
//     caller's capacity keeps the newest instants and counts the rest; a late batch leaves the missed slot empty, keeps
//     the grid phase and is counted;
//   - a replayed batch schedules nothing twice, and reset() forgets the schedule and the counters.
#include "coregs3_host_clock.hpp"

#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace gt4::coregs3;

namespace {

void check(bool condition, const char *what) {
  if (condition) return;
  std::fprintf(stderr, "coregs3_host_clock: %s\n", what);
  std::exit(1);
}

std::int64_t ns_of(std::uint64_t ticks) {
  return static_cast<std::int64_t>(ticks * 1000000000ull / kPresentationTicksPerSecond);
}

PresentationClock::Due due(std::uint32_t count, std::uint64_t first, std::uint64_t step) {
  PresentationClock::Due value;
  value.count = count;
  value.first = first;
  value.step = step;
  return value;
}

void check_rates() {
  const std::uint32_t rates[] = {20, 30, 45, 60, 120, 144};
  const std::uint64_t steps[] = {180, 120, 80, 60, 30, 25};
  for (std::size_t index = 0; index < sizeof rates / sizeof rates[0]; ++index) {
    HostPresentationClock clock(rates[index]);
    check(clock.active(), "a supported rate is active");
    check(clock.rate_hz() == rates[index], "the rate is kept");
    check(clock.step_ticks() == steps[index], "the display period in guest ticks");
    check(clock.period_ns() == ns_of(steps[index]), "the display period in nanoseconds");
  }
  check(HostPresentationClock::kStaleTicks == 60, "the stale bound is one guest frame");
  HostPresentationClock unsupported(50);
  check(!unsupported.active(), "an unsupported rate is inert");
  const HostPresentationClock::Plan plan = unsupported.plan(due(1, 0, 30), 1000);
  check(plan.count == 0, "an inert clock schedules nothing");
}

void check_120hz_cadence() {
  HostPresentationClock clock(120);
  PresentationClock timeline(120);
  const std::int64_t origin = 1000000000;
  std::vector<std::int64_t> shows;
  std::uint64_t planned = 0;
  for (std::uint64_t vblank = 0; vblank <= 30; ++vblank) {
    const PresentationClock::Due batch = timeline.observe(vblank * 60);
    const HostPresentationClock::Plan plan = clock.plan(batch, origin + ns_of(vblank * 60));
    check(plan.stale == 0 && plan.skipped == 0 && plan.duplicates == 0, "120 Hz: a paced guest loses nothing");
    for (std::uint32_t index = 0; index < plan.count; ++index) {
      check(plan.ticks[index].index == index, "120 Hz: the tick index follows the batch");
      shows.push_back(plan.ticks[index].show_ns);
    }
    check(plan.count == batch.count, "120 Hz: every due instant is scheduled");
    planned += plan.count;
  }
  check(planned == 61, "120 Hz: one first tick plus 30 frames of two");
  check(shows.size() == planned, "every planned tick is reported");
  const std::int64_t period = clock.period_ns();
  check(shows[0] == origin, "120 Hz: the first image is shown as soon as it exists");
  check(shows[1] - shows[0] == 2 * period, "120 Hz: the startup slot with no tick is left empty");
  for (std::size_t index = 2; index < shows.size(); ++index)
    check(shows[index] - shows[index - 1] == period, "120 Hz: one display period between shows");
  check(clock.late() == 1, "120 Hz: exactly the startup slot is late");
  check(clock.plans() == 31, "one plan per VBlank");
}

void check_144hz_cadence() {
  HostPresentationClock clock(144);
  PresentationClock timeline(144);
  const std::int64_t origin = 2000000000;
  std::vector<std::int64_t> shows;
  std::uint64_t planned = 0;
  for (std::uint64_t vblank = 0; vblank <= 30; ++vblank) {
    const PresentationClock::Due batch = timeline.observe(vblank * 60);
    const HostPresentationClock::Plan plan = clock.plan(batch, origin + ns_of(vblank * 60));
    check(plan.stale == 0 && plan.skipped == 0 && plan.duplicates == 0, "144 Hz: a paced guest loses no tick");
    for (std::uint32_t index = 0; index < plan.count; ++index) shows.push_back(plan.ticks[index].show_ns);
    check(plan.count == batch.count, "144 Hz: every due instant is scheduled");
    planned += plan.count;
  }
  check(planned == 73, "144 Hz: 73 presentation instants over 30 guest frames");
  const std::int64_t period = clock.period_ns();
  for (std::size_t index = 1; index < shows.size(); ++index) {
    const std::int64_t interval = shows[index] - shows[index - 1];
    check(interval > 0, "144 Hz: no show time moves backwards");
    check(interval % period == 0, "144 Hz: every interval is a whole number of display periods");
    check(interval / period >= 1 && interval / period <= 3, "144 Hz: a probe slot can be left empty, never more");
  }
  check(clock.late() != 0, "144 Hz: the VBlank quantization leaves some display slots empty");
  check(clock.plans() == 31, "one plan per VBlank");
}

void check_stale_capacity_and_late() {
  {  // Instants 0, 30, 60, 90 due at once: 0 is 90 ticks (1.5 guest frames) behind the newest and is stale.
    HostPresentationClock clock(120);
    const HostPresentationClock::Plan plan = clock.plan(due(4, 0, 30), 0);
    check(plan.count == 3 && plan.stale == 1, "only the instants within one guest frame of the newest are shown");
    check(plan.ticks[0].instant_ticks == 30 && plan.ticks[2].instant_ticks == 90, "the newest instants are kept");
  }
  {  // Capacity applies after the stale filter: the newest two of 60, 90, 120 are kept.
    HostPresentationClock clock(120);
    const HostPresentationClock::Plan plan = clock.plan(due(3, 60, 30), 0, 2);
    check(plan.count == 2 && plan.skipped == 1 && plan.stale == 0, "capacity keeps the newest two instants");
    check(plan.ticks[0].instant_ticks == 90 && plan.ticks[1].instant_ticks == 120, "the old instant was dropped");
  }
  {  // The next batch arrives after its display slot: the slot is left empty, the grid keeps its phase.
    HostPresentationClock clock(120);
    const std::int64_t period = clock.period_ns();
    clock.plan(due(1, 0, 30), 0);
    const std::int64_t now = ns_of(110);
    const HostPresentationClock::Plan plan = clock.plan(due(1, 60, 30), now);
    check(plan.count == 1 && plan.late == 1, "a batch after its slot is counted late");
    check(plan.ticks[0].show_ns == 4 * period, "the missed slot is left empty and the grid phase is kept");
    check(plan.ticks[0].show_ns > now, "the late image waits for the next display slot");
  }
  {  // A replayed batch schedules nothing twice.
    HostPresentationClock clock(120);
    const PresentationClock::Due batch = due(2, 0, 30);
    clock.plan(batch, 0);
    const HostPresentationClock::Plan replay = clock.plan(batch, ns_of(60));
    check(replay.count == 0 && replay.duplicates == 2, "a replayed batch schedules nothing twice");
    check(clock.duplicates() == 2, "the duplicate count is kept");
  }
}

void check_reset() {
  HostPresentationClock clock(120);
  clock.plan(due(1, 0, 30), 0);
  clock.plan(due(1, 30, 30), ns_of(60));
  check(clock.started() && clock.planned() != 0, "the clock ran");
  clock.reset();
  check(!clock.started() && clock.planned() == 0 && clock.plans() == 0 && clock.stale() == 0 &&
            clock.skipped() == 0 && clock.late() == 0 && clock.duplicates() == 0,
        "reset forgets the schedule and the counters");
  const HostPresentationClock::Plan plan = clock.plan(due(1, 300, 30), 7000000);
  check(plan.count == 1 && plan.ticks[0].show_ns == 7000000, "the first plan after a reset anchors a new grid");
}

}  // namespace

int main() {
  check_rates();
  check_120hz_cadence();
  check_144hz_cadence();
  check_stale_capacity_and_late();
  check_reset();
  std::printf("coregs3 host clock fixture: ok\n");
  return 0;
}
