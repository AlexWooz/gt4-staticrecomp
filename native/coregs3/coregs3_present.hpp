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

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

#include "coregs3_host_clock.hpp"
#include "coregs3_lighting.hpp"
#include "coregs3_presentation.hpp"
#include "coregs3_scene.hpp"
#include "coregs3_timeline.hpp"

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
// TASK-090 live world: one captured frame's world build, as the present layer measured it. The selection counts name
// every exclusion the shared rule returned; the texture counts name where each item's pixels came from (its own job's
// live record, the same material one or more frames older, or nothing) and every refusal. `blend_deferred`,
// `depth_defaulted` and `alpha_fail_defaulted` are the states this live world simplifies; `fog_items`/`fog_missing`
// count the emitted per-vertex fog and its identity fallback, and `filter_linear` the GS bilinear items.
struct WorldFrameCounters {
  std::uint32_t draws = 0;             // world draws that produced at least one range
  std::uint32_t items = 0;             // selected items
  std::uint64_t vertices = 0;          // world vertices uploaded
  std::uint64_t triangles = 0;         // triangles in the ranges
  std::uint32_t atlas_textures = 0;    // unique textures packed into the atlas
  std::uint32_t atlas_refused = 0;     // items whose texture did not fit the atlas
  std::uint32_t texture_live = 0;      // items bound to their own job's live record
  std::uint32_t texture_stale = 0;     // items bound to a record from an earlier frame (counted fallback)
  std::uint32_t texture_missing = 0;   // items with no live record for their address
  std::uint32_t texture_refused = 0;   // items whose decode or sampler state was refused
  std::uint32_t texture_untextured = 0;  // items skipped because no usable texture binding exists (the base stays)
  std::uint32_t texture_no_record = 0;   // ... no 40-byte texture record
  std::uint32_t texture_no_uv = 0;       // ... no model UV channel (the emitted UV pairing could not start)
  std::uint32_t texture_texgen = 0;      // ... the body/paint texgen family (no model UV path)
  std::uint32_t fog_items = 0;           // ranges whose PRIM.FGE applies the fog
  std::uint32_t fog_missing = 0;         // vertices with no emitted fog value (the 255 identity is used)
  std::uint32_t filter_linear = 0;       // items sampled with the GS bilinear
  std::uint32_t filter_defaulted = 0;    // linear items sampled nearest because GT4_COREGS3_WORLD_FILTER=nearest
  std::uint32_t mip_defaulted = 0;       // textures decoded at level 0 with TEX1.MXL > 0 (the table's fallback)
  std::uint32_t refused_draws = 0;     // world draws refused (mesh decode, matrix)
  std::uint32_t excluded_itop = 0;
  std::uint32_t excluded_shape = 0;
  std::uint32_t excluded_no_state = 0;
  std::uint32_t excluded_mirror = 0;
  std::uint32_t excluded_no_position = 0;
  std::uint32_t skipped_reflection = 0;  // the reflection pass (PassId 7) is not drawn in this first live world
  std::uint32_t sky_skipped = 0;         // sky items not drawn (the GS base keeps the sky and the HUD)
  std::uint32_t far_skipped = 0;         // far-underlay items not drawn (the base keeps its background; GT4_COREGS3_WORLD_BACKGROUND=1 draws both)
  std::uint32_t blend_deferred = 0;      // items whose PRIM enables blending (drawn opaque)
  std::uint32_t depth_defaulted = 0;     // items whose TEST is not ZTE + GEQUAL (drawn with the measured rule)
  std::uint32_t alpha_fail_defaulted = 0;  // items with AFAIL other than KEEP (discard on fail)
  // TASK-090 live post chain: items drawn with the captured multiplies/CT16 epilogue, of those the ones with the
  // faithful CT16 model, and the frames that asked for faithful without a noted dither environment.
  std::uint32_t post_items = 0;
  std::uint32_t post_faithful = 0;
  std::uint32_t post_env_missing = 0;
};
void note_world_frame(const WorldFrameCounters &counts) noexcept;
void note_world_tick(std::uint32_t draw_calls, std::uint64_t samples) noexcept;

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
  // The per-item lighting outcome of every captured draw the pass shaded, summed over passes. The families are
  // coregs3_lighting.hpp's LightProgram values; a draw can appear in more than one family (the capture mixes programs
  // inside a draw). `light_status` counts mesh vertices by their outcome (ok plus the fallback reasons).
  std::array<std::atomic<std::uint64_t>, kLightProgramCount> family_items{};
  std::array<std::atomic<std::uint64_t>, kLightProgramCount> family_items_lit{};
  std::array<std::atomic<std::uint64_t>, kLightProgramCount> family_items_fallback{};
  std::array<std::atomic<std::uint64_t>, kLightProgramCount> family_vertices_lit{};
  std::array<std::atomic<std::uint64_t>, kLightProgramCount> family_vertices_fallback{};
  std::array<std::atomic<std::uint64_t>, kLightProgramCount> family_draws_lit{};
  std::array<std::atomic<std::uint64_t>, kLightProgramCount> family_draws_fallback{};
  std::array<std::atomic<std::uint64_t>, kLightStatusCount> light_status{};
  std::atomic<std::uint64_t> items_uploaded{0};
  std::atomic<std::uint64_t> items_carried{0};
  std::atomic<std::uint64_t> items_uncertain{0};
  std::atomic<std::uint64_t> items_none{0};
  // TASK-090 live world, summed over the whole run: the frames built, the draw calls and occlusion samples of the
  // ticks, and the selection/texture/simplification counts of WorldFrameCounters.
  std::atomic<std::uint64_t> world_frames{0};
  std::atomic<std::uint64_t> world_ticks{0};
  std::atomic<std::uint64_t> world_draw_calls{0};
  std::atomic<std::uint64_t> world_samples{0};
  std::atomic<std::uint64_t> world_draws{0};
  std::atomic<std::uint64_t> world_items{0};
  std::atomic<std::uint64_t> world_vertices{0};
  std::atomic<std::uint64_t> world_triangles{0};
  std::atomic<std::uint64_t> world_atlas_textures{0};
  std::atomic<std::uint64_t> world_atlas_refused{0};
  std::atomic<std::uint64_t> world_texture_live{0};
  std::atomic<std::uint64_t> world_texture_stale{0};
  std::atomic<std::uint64_t> world_texture_missing{0};
  std::atomic<std::uint64_t> world_texture_refused{0};
  std::atomic<std::uint64_t> world_texture_untextured{0};
  std::atomic<std::uint64_t> world_texture_no_record{0};
  std::atomic<std::uint64_t> world_texture_no_uv{0};
  std::atomic<std::uint64_t> world_texture_texgen{0};
  std::atomic<std::uint64_t> world_fog_items{0};
  std::atomic<std::uint64_t> world_fog_missing{0};
  std::atomic<std::uint64_t> world_filter_linear{0};
  std::atomic<std::uint64_t> world_filter_defaulted{0};
  std::atomic<std::uint64_t> world_mip_defaulted{0};
  std::atomic<std::uint64_t> world_post_items{0};
  std::atomic<std::uint64_t> world_post_faithful{0};
  std::atomic<std::uint64_t> world_post_env_missing{0};
  std::atomic<std::uint64_t> world_refused_draws{0};
  std::atomic<std::uint64_t> world_excluded_itop{0};
  std::atomic<std::uint64_t> world_excluded_shape{0};
  std::atomic<std::uint64_t> world_excluded_no_state{0};
  std::atomic<std::uint64_t> world_excluded_mirror{0};
  std::atomic<std::uint64_t> world_excluded_no_position{0};
  std::atomic<std::uint64_t> world_skipped_reflection{0};
  std::atomic<std::uint64_t> world_sky_skipped{0};
  std::atomic<std::uint64_t> world_far_skipped{0};

  std::atomic<std::uint64_t> world_blend_deferred{0};
  std::atomic<std::uint64_t> world_depth_defaulted{0};
  std::atomic<std::uint64_t> world_alpha_fail_defaulted{0};
};

const Counters &counters() noexcept;
void note_render(std::uint64_t vblank, std::uint32_t draws, std::uint32_t draws_refused, std::uint32_t draws_unlit,
                 std::uint64_t vertices, std::uint64_t vertices_unlit, std::uint64_t triangles,
                 const LightCounters &lighting) noexcept;
void note_pass_refused(std::uint32_t draws_refused) noexcept;

// --- the presentation timeline (docs/COREGS3-PLAN.md §5.5) ----------------------------------------------------------
// The live layer presents at a constant rate chosen by the owner (20/30/45/60/120/144 Hz, default 60) instead of once
// per guest VBlank: every guest frame is recorded in a `coregs3::FrameRing` with its guest timestamp and camera, and
// each presentation instant interpolates the two latest frames (coregs3_presentation.hpp). The guest itself is never
// retimed. `nearest` repeats the nearest captured frame, the baseline the interpolation is measured against.
struct TimelineConfig {
  std::uint32_t hz = 0;  // 0: not configured (the layer must refuse)
  PresentationMode mode = PresentationMode::interpolate;
  // Diagnostic for image evidence: draw only this due tick of each guest frame (0-based), leaving that tick's image in
  // the composed frame; UINT32_MAX draws every due tick, so the last one is what a present would show.
  std::uint32_t selected_tick = UINT32_MAX;
};

// Reads GT4_COREGS3_PRESENT_HZ (default 60), GT4_COREGS3_PRESENT_MODE (interpolate|nearest, default interpolate) and
// the diagnostic GT4_COREGS3_PRESENT_TICK. An unusable value is refused with `error`, never defaulted.
bool timeline_config_from_environment(TimelineConfig &config, std::string &error) noexcept;
void set_timeline_config(const TimelineConfig &config) noexcept;
const TimelineConfig &timeline_config() noexcept;
bool timeline_configured() noexcept;

// The timeline's own counters; every field is atomic so the F8 report thread can read them. `interpolated` counts
// objects drawn with a blended matrix, `entrywise` those blended entry by entry (the rank-deficient fallback), and
// `nearest` those drawn with a captured matrix. `predicted` stays 0 until the prediction switch lands.
struct TimelineCounters {
  std::atomic<std::uint64_t> frames{0};        // guest frames recorded in the timeline
  std::atomic<std::uint64_t> instants{0};      // presentation instants produced by the clock
  std::atomic<std::uint64_t> skipped{0};       // instants lost to a stall (the clock jumped to the newest grid point)
  std::atomic<std::uint64_t> pairs{0};         // instants whose frame pair and camera could be used
  std::atomic<std::uint64_t> pairs_refused{0}; // unordered/too far apart, camera invalid, or a camera cut
  std::atomic<std::uint64_t> paired{0};        // objects present in both frames
  std::atomic<std::uint64_t> only_newer{0};
  std::atomic<std::uint64_t> only_older{0};
  std::atomic<std::uint64_t> interpolated{0};  // objects drawn with the rigid interpolation
  std::atomic<std::uint64_t> entrywise{0};     // objects drawn with the entrywise fallback
  std::atomic<std::uint64_t> nearest{0};       // objects drawn with a captured matrix
  std::atomic<std::uint64_t> predicted{0};     // reserved; prediction is not implemented here
  std::atomic<std::uint64_t> dropped_discontinuity{0};  // objects dropped to the newest frame by a cut/teleport
  std::atomic<std::uint64_t> draw_ticks{0};    // presentation ticks the layer actually drew
  // Frame-time histogram of the presentation clock: the interval between consecutive instants, in guest ticks (8-tick
  // buckets, the last bucket is 248+) and the exact arithmetic the report prints.
  std::atomic<std::uint64_t> interval_count{0};
  std::atomic<std::uint64_t> interval_sum{0};
  std::atomic<std::uint64_t> interval_min{0};
  std::atomic<std::uint64_t> interval_max{0};
  std::array<std::atomic<std::uint64_t>, 32> interval_hist{};
  // TASK-090 host clock (coregs3_host_clock.hpp): the due instants it scheduled on the display grid and the ones it
  // dropped, accumulated by note_guest_frame. `host_ticks` counts the instants handed to the window presenter.
  std::atomic<std::uint64_t> host_plans{0};
  std::atomic<std::uint64_t> host_ticks{0};
  std::atomic<std::uint64_t> host_late{0};
  std::atomic<std::uint64_t> host_stale{0};
  std::atomic<std::uint64_t> host_skipped{0};
  std::atomic<std::uint64_t> host_duplicates{0};
};
const TimelineCounters &timeline_counters() noexcept;
void note_draw_tick() noexcept;  // the layer drew one presentation tick

// Records one claimed guest frame in the timeline and advances the presentation clock. Called once per guest VBlank
// by the live layer, whether or not the frame is drawn (a headless run still samples the timeline every frame).
// Not noexcept: it allocates the frame's object list.
void note_guest_frame(const FrameSnapshot &frame);

// The instants the latest guest frame made due (a fixed grid in guest ticks; see PresentationClock).
PresentationClock::Due due_ticks() noexcept;

// TASK-090 host clock: the presentation times of the latest due batch, scheduled for a window that presents at the
// requested rate (coregs3_host_clock.hpp). Empty unless the rate is above the guest's 60 Hz; the backend uses it only
// when it can present more than one picture per guest frame. `host_clock_enabled` is the timeline's decision (a
// configured rate above 60 Hz), not whether a window is attached.
bool host_clock_enabled() noexcept;
const HostPresentationClock::Plan &host_clock_plan() noexcept;
// A VBlank with no new captured frame: the previous batch must not be drawn or presented again.
void clear_host_clock_plan() noexcept;

// The transforms of the newest frame's objects at one presentation instant, ready for the present layer. Objects of
// the newest frame only: the newest frame's draw list is the image's reference (an object that disappeared from it is
// counted `only_older` and not drawn). `count` adds the outcome to `timeline_counters()`; the live layer passes true,
// so the counters describe the ticks it presents (in a headless evidence run, the dumped frames). Not noexcept:
// allocates.
void interpolated_transforms(std::uint64_t instant_ticks, std::vector<DrawTransform> &out, bool count);

}  // namespace gt4::coregs3::present
