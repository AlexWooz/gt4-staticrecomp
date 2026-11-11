// TASK-090 CoreGS 3.0: the live frame ring of the debug-only `GT4_COREGS3=present` mode. See coregs3_present.hpp.
#define _CRT_SECURE_NO_WARNINGS
#include "coregs3_present.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
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

// The presentation timeline's consumer state. One writer: the GS thread that claims frames and draws the layer; the
// producer only touches `g_ring`. The F8 report reads the atomics in g_timeline_counters from another thread.
struct TimelineState {
  TimelineConfig config;
  bool configured = false;
  FrameRing ring{8};
  std::vector<CameraFrame> cameras;  // aligned with `ring`, oldest first
  PresentationClock clock{60};
  PresentationClock::Due batch;
  InterpolationLimits limits;
  HostPresentationClock host_clock{60};
  HostPresentationClock::Plan host_plan;
  // TASK-090 crash-race diagnostic: the per-instant counter pass (removed by commit 6fc9f75b), restored behind
  // GT4_COREGS3_DRY_PASS=1 so the crash it exposes can be reproduced and measured.
  std::vector<DrawTransform> dry_transforms;
};

// TASK-090 crash-race diagnostic: the removed per-VBlank counter pass, opt-in. It runs the interpolation of every due
// presentation instant on the GS worker (no drawing) and is what reproduces the exit-139 fault at VBlank ~6783.
bool dry_pass_enabled() noexcept {
  static const bool enabled = [] {
    const char *value = std::getenv("GT4_COREGS3_DRY_PASS");
    return value != nullptr && *value != '\0' && std::strcmp(value, "0") != 0;
  }();
  return enabled;
}

std::int64_t steady_now_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
TimelineState &timeline_state() {
  static TimelineState state;
  return state;
}
TimelineCounters g_timeline_counters;
std::atomic<std::uint64_t> g_interval_last{0};  // last presentation instant, for the frame-time histogram
std::atomic<bool> g_have_interval_last{false};

void add_interval(std::uint64_t interval_ticks) noexcept {
  TimelineCounters &c = g_timeline_counters;
  c.interval_count.fetch_add(1, std::memory_order_relaxed);
  c.interval_sum.fetch_add(interval_ticks, std::memory_order_relaxed);
  const auto min = c.interval_min.load(std::memory_order_relaxed);
  if (min == 0 || interval_ticks < min) c.interval_min.store(interval_ticks, std::memory_order_relaxed);
  const auto max = c.interval_max.load(std::memory_order_relaxed);
  if (interval_ticks > max) c.interval_max.store(interval_ticks, std::memory_order_relaxed);
  const std::size_t bucket = std::min<std::size_t>(interval_ticks / 8u, c.interval_hist.size() - 1);
  c.interval_hist[bucket].fetch_add(1, std::memory_order_relaxed);
}

void add_counts(const InterpolationCounters &counts) noexcept {
  TimelineCounters &c = g_timeline_counters;
  const auto add = [](std::atomic<std::uint64_t> &field, std::uint64_t value) {
    if (value != 0) field.fetch_add(value, std::memory_order_relaxed);
  };
  add(c.pairs, counts.pairs);
  add(c.pairs_refused, counts.pairs_refused);
  add(c.paired, counts.paired);
  add(c.only_newer, counts.only_newer);
  add(c.only_older, counts.only_older);
  add(c.interpolated, counts.interpolated);
  add(c.entrywise, counts.entrywise);
  add(c.nearest, counts.nearest);
  add(c.predicted, counts.predicted);
  add(c.dropped_discontinuity, counts.dropped_discontinuity);
}

}  // namespace

void set_active(bool active) noexcept { g_active.store(active, std::memory_order_relaxed); }
bool active() noexcept { return g_active.load(std::memory_order_relaxed); }
const Counters &counters() noexcept { return g_counters; }
const TimelineCounters &timeline_counters() noexcept { return g_timeline_counters; }

bool timeline_config_from_environment(TimelineConfig &config, std::string &error) noexcept {
  config = TimelineConfig{};
  const char *hz_text = std::getenv("GT4_COREGS3_PRESENT_HZ");
  if (hz_text == nullptr || *hz_text == '\0') {
    config.hz = 60;
  } else {
    config.hz = parse_presentation_hz(hz_text);
    if (config.hz == 0) {
      error = std::string("GT4_COREGS3_PRESENT_HZ=") + hz_text + " is not a supported presentation rate (" +
              presentation_rate_list() + ")";
      return false;
    }
  }
  const char *mode_text = std::getenv("GT4_COREGS3_PRESENT_MODE");
  if (!parse_presentation_mode(mode_text, config.mode)) {
    error = std::string("GT4_COREGS3_PRESENT_MODE=") + mode_text + " is not interpolate or nearest";
    return false;
  }
  config.selected_tick = UINT32_MAX;
  if (const char *tick_text = std::getenv("GT4_COREGS3_PRESENT_TICK"); tick_text != nullptr && *tick_text != '\0') {
    char *end = nullptr;
    const long value = std::strtol(tick_text, &end, 10);
    if (end == nullptr || *end != '\0' || value < 0 || value > 64) {
      error = std::string("GT4_COREGS3_PRESENT_TICK=") + tick_text +
              " is not a tick index between 0 and 64 (unset draws every due tick)";
      return false;
    }
    config.selected_tick = static_cast<std::uint32_t>(value);
  }
  return true;
}

void set_timeline_config(const TimelineConfig &config) noexcept {
  TimelineState &state = timeline_state();
  state.config = config;
  state.configured = config.hz != 0;
  state.clock = PresentationClock(config.hz);
  state.ring.clear();
  state.cameras.clear();
  state.batch = PresentationClock::Due{};
  state.host_clock = HostPresentationClock(config.hz);
  state.host_plan = HostPresentationClock::Plan{};
}

const TimelineConfig &timeline_config() noexcept { return timeline_state().config; }
bool timeline_configured() noexcept { return timeline_state().configured; }

void note_guest_frame(const FrameSnapshot &frame) {
  TimelineState &state = timeline_state();
  if (!state.configured) return;
  const GuestTime time{frame.vblank, guest_ticks(frame.vblank)};
  TimelineFrame timeline = timeline_frame(frame, time);
  // Diagnostic (GT4_COREGS3_PRESENT_TRACE=<first vblank>): what the claimed frame carries. A windowed run and a
  // headless run of the same capture must claim the same kind of frame; this prints the first few after the vblank.
  if (const char *trace = std::getenv("GT4_COREGS3_PRESENT_TRACE")) {
    static std::uint64_t traced = 0;
    const std::uint64_t first = std::strtoull(trace, nullptr, 10);
    if (frame.vblank >= first && traced < 30) {
      std::fprintf(stderr, "[coregs3] claim vblank=%llu draws=%u objects=%zu packets=%zu camera=%u\n",
                   static_cast<unsigned long long>(frame.vblank), frame.draw_count, timeline.objects.size(),
                   frame.packets.size(), frame.camera.valid);
      traced += 1;
    }
  }
  state.ring.push(timeline);
  while (state.cameras.size() >= state.ring.capacity()) state.cameras.erase(state.cameras.begin());
  state.cameras.push_back(camera_frame(frame.camera));
  state.batch = state.clock.observe(time.ticks);
  g_timeline_counters.frames.fetch_add(1, std::memory_order_relaxed);
  g_timeline_counters.instants.fetch_add(state.batch.count, std::memory_order_relaxed);
  if (state.batch.skipped != 0) g_timeline_counters.skipped.fetch_add(state.batch.skipped, std::memory_order_relaxed);
  // The frame-time histogram: consecutive presentation instants, in guest ticks. The first instant of a batch is one
  // step after the previous batch's last instant unless the clock had to skip (then the gap shows in this histogram and
  // in `skipped`).
  if (state.batch.count != 0) {
    if (g_have_interval_last.load(std::memory_order_relaxed)) {
      const std::uint64_t previous = g_interval_last.load(std::memory_order_relaxed);
      if (state.batch.first > previous) add_interval(state.batch.first - previous);
    } else {
      g_have_interval_last.store(true, std::memory_order_relaxed);
    }
    if (state.batch.count > 1)
      for (std::uint32_t index = 1; index < state.batch.count; ++index) add_interval(state.batch.step);
    g_interval_last.store(state.batch.first + (state.batch.count - 1) * state.batch.step, std::memory_order_relaxed);
  }
  // TASK-090 crash-race diagnostic: the per-instant counter pass of commit 6fc9f75b, opt-in. See dry_pass_enabled.
  if (dry_pass_enabled()) {
    for (std::uint32_t index = 0; index < state.batch.count; ++index) {
      const std::uint64_t instant = state.batch.first + static_cast<std::uint64_t>(index) * state.batch.step;
      interpolated_transforms(instant, state.dry_transforms, true);
    }
  }
  // TASK-090 host clock: the host presentation times of this batch on the display grid (coregs3_host_clock.hpp; a
  // window presenting at the requested rate uses them, a headless run only reports the counters).
  state.host_plan = HostPresentationClock::Plan{};
  if (state.config.hz > 60) {
    state.host_plan = state.host_clock.plan(state.batch, steady_now_ns(), HostPresentationClock::kMaxTicks);
    TimelineCounters &c = g_timeline_counters;
    const auto add = [](std::atomic<std::uint64_t> &field, std::uint64_t value) {
      if (value != 0) field.fetch_add(value, std::memory_order_relaxed);
    };
    add(c.host_plans, 1);
    add(c.host_ticks, state.host_plan.count);
    add(c.host_late, state.host_plan.late);
    add(c.host_stale, state.host_plan.stale);
    add(c.host_skipped, state.host_plan.skipped);
    add(c.host_duplicates, state.host_plan.duplicates);
  }
}

PresentationClock::Due due_ticks() noexcept { return timeline_state().batch; }

bool host_clock_enabled() noexcept {
  const TimelineState &state = timeline_state();
  return state.configured && state.config.hz > 60;
}

const HostPresentationClock::Plan &host_clock_plan() noexcept { return timeline_state().host_plan; }

void clear_host_clock_plan() noexcept { timeline_state().host_plan = HostPresentationClock::Plan{}; }

void note_draw_tick() noexcept { g_timeline_counters.draw_ticks.fetch_add(1, std::memory_order_relaxed); }

void interpolated_transforms(std::uint64_t instant_ticks, std::vector<DrawTransform> &out, bool count) {
  TimelineState &state = timeline_state();
  InterpolationCounters counts;
  out.clear();
  if (!state.configured || state.ring.size() == 0) return;
  const TimelineFrame &newer = state.ring.at(0);
  const CameraFrame &newer_camera = state.cameras.back();
  counts.instants = 1;
  if (state.ring.size() == 1) {
    // No pair yet: present the newest frame exactly.
    out.reserve(newer.objects.size());
    for (const ObjectTransform &object : newer.objects) {
      DrawTransform draw;
      draw.key = object.key;
      draw.source = TransformSource::captured_newer;
      std::memcpy(draw.matrix, object.matrix, sizeof draw.matrix);
      out.push_back(draw);
    }
    counts.nearest += newer.objects.size();
    if (count) add_counts(counts);
    return;
  }
  const TimelineFrame &older = state.ring.at(1);
  const CameraFrame &older_camera = state.cameras[state.cameras.size() - 2];
  float alpha = 1.0f;
  if (state.config.mode == PresentationMode::nearest) {
    if (instant_ticks <= older.time.ticks) {
      alpha = 0.0f;
    } else if (instant_ticks >= newer.time.ticks) {
      alpha = 1.0f;
    } else {
      const std::uint64_t to_older = instant_ticks - older.time.ticks;
      const std::uint64_t to_newer = newer.time.ticks - instant_ticks;
      alpha = to_older < to_newer ? 0.0f : 1.0f;  // a tie repeats the newer captured frame
    }
  } else if (!interpolation_alpha(older.time, newer.time, instant_ticks, alpha)) {
    alpha = 1.0f;  // an unordered pair: never blend, present the newest frame as captured
  }
  interpolate_frame(older, older_camera, newer, newer_camera, alpha, state.limits, out, counts);
  if (count) add_counts(counts);
}

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
                 std::uint64_t vertices, std::uint64_t vertices_unlit, std::uint64_t triangles,
                 const LightCounters &lighting) noexcept {
  g_counters.passes.fetch_add(1, std::memory_order_relaxed);
  g_counters.draws.fetch_add(draws, std::memory_order_relaxed);
  g_counters.draws_refused.fetch_add(draws_refused, std::memory_order_relaxed);
  g_counters.draws_unlit.fetch_add(draws_unlit, std::memory_order_relaxed);
  g_counters.vertices.fetch_add(vertices, std::memory_order_relaxed);
  g_counters.vertices_unlit.fetch_add(vertices_unlit, std::memory_order_relaxed);
  g_counters.triangles.fetch_add(triangles, std::memory_order_relaxed);
  g_counters.last_vblank.store(vblank, std::memory_order_relaxed);
  for (std::uint32_t program = 0; program < kLightProgramCount; ++program) {
    const LightFamilyCounts &family = lighting.family[program];
    g_counters.family_items[program].fetch_add(family.items, std::memory_order_relaxed);
    g_counters.family_items_lit[program].fetch_add(family.items_lit, std::memory_order_relaxed);
    g_counters.family_items_fallback[program].fetch_add(family.items_fallback, std::memory_order_relaxed);
    g_counters.family_vertices_lit[program].fetch_add(family.vertices_lit, std::memory_order_relaxed);
    g_counters.family_vertices_fallback[program].fetch_add(family.vertices_fallback, std::memory_order_relaxed);
    g_counters.family_draws_lit[program].fetch_add(family.draws_lit, std::memory_order_relaxed);
    g_counters.family_draws_fallback[program].fetch_add(family.draws_fallback, std::memory_order_relaxed);
  }
  for (std::uint32_t status = 0; status < kLightStatusCount; ++status)
    g_counters.light_status[status].fetch_add(lighting.status[status], std::memory_order_relaxed);
  g_counters.items_uploaded.fetch_add(lighting.items_uploaded, std::memory_order_relaxed);
  g_counters.items_carried.fetch_add(lighting.items_carried, std::memory_order_relaxed);
  g_counters.items_uncertain.fetch_add(lighting.items_uncertain, std::memory_order_relaxed);
  g_counters.items_none.fetch_add(lighting.items_none, std::memory_order_relaxed);
}

void note_world_frame(const WorldFrameCounters &counts) noexcept {
  Counters &c = g_counters;
  const auto add = [](std::atomic<std::uint64_t> &field, std::uint64_t value) {
    if (value != 0) field.fetch_add(value, std::memory_order_relaxed);
  };
  c.world_frames.fetch_add(1, std::memory_order_relaxed);
  add(c.world_draws, counts.draws);
  add(c.world_items, counts.items);
  add(c.world_vertices, counts.vertices);
  add(c.world_triangles, counts.triangles);
  add(c.world_atlas_textures, counts.atlas_textures);
  add(c.world_atlas_refused, counts.atlas_refused);
  add(c.world_texture_live, counts.texture_live);
  add(c.world_texture_stale, counts.texture_stale);
  add(c.world_texture_missing, counts.texture_missing);
  add(c.world_texture_refused, counts.texture_refused);
  add(c.world_texture_untextured, counts.texture_untextured);
  add(c.world_filter_defaulted, counts.filter_defaulted);
  add(c.world_mip_defaulted, counts.mip_defaulted);
  add(c.world_refused_draws, counts.refused_draws);
  add(c.world_excluded_itop, counts.excluded_itop);
  add(c.world_excluded_shape, counts.excluded_shape);
  add(c.world_excluded_no_state, counts.excluded_no_state);
  add(c.world_excluded_mirror, counts.excluded_mirror);
  add(c.world_excluded_no_position, counts.excluded_no_position);
  add(c.world_skipped_reflection, counts.skipped_reflection);
  add(c.world_sky_skipped, counts.sky_skipped);
  add(c.world_far_skipped, counts.far_skipped);
  add(c.world_fog_deferred, counts.fog_deferred);
  add(c.world_blend_deferred, counts.blend_deferred);
  add(c.world_depth_defaulted, counts.depth_defaulted);
  add(c.world_alpha_fail_defaulted, counts.alpha_fail_defaulted);
}

void note_world_tick(std::uint32_t draw_calls, std::uint64_t samples) noexcept {
  g_counters.world_ticks.fetch_add(1, std::memory_order_relaxed);
  g_counters.world_draw_calls.fetch_add(draw_calls, std::memory_order_relaxed);
  g_counters.world_samples.fetch_add(samples, std::memory_order_relaxed);
}

void note_pass_refused(std::uint32_t draws_refused) noexcept {
  g_counters.passes_refused.fetch_add(1, std::memory_order_relaxed);
  g_counters.draws_refused.fetch_add(draws_refused, std::memory_order_relaxed);
}

}  // namespace gt4::coregs3::present
