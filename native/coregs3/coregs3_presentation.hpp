// TASK-090 CoreGS 3.0: the live presentation timeline (docs/COREGS3-PLAN.md §5.5).
//
// The guest is a fixed 60 Hz machine and stays bit-exact. Presentation is decoupled: the renderer runs on its own
// constant clock (20/30/45/60/120/144 Hz) and, for every presentation instant, takes the two latest captured guest
// frames and interpolates each object's transform between them (translation lerp, rotation slerp, scale lerp), plus
// the camera from its own pose. Interpolation trails the guest by about one frame, which is the accepted price of
// smooth motion; nothing here reads back into guest state.
//
// The rules are the measured ones of the prediction study (docs/journal/2026-11-10.md, "Prediction study: rigid
// predictor and pose camera"): entrywise matrix interpolation distorts a spinning wheel by up to 18 px at 1x because a
// wheel turns 0.8-1.3 rad per 60 Hz tick, while a rigid world-space interpolation (translation + quaternion + uniform
// scale) stays within 0.42 px at 120 Hz. A rank-deficient world matrix (the flat shell/reflection draws) falls back to
// the entrywise lerp of the captured matrices. The camera is interpolated from its pose (position and orientation),
// never entry by entry, and a camera cut or a teleport drops that instant to the newer captured frame instead of
// blending across the discontinuity.
//
// Clock unit: one tick = 1/3600 s and a guest VBlank = 60 ticks. Every supported rate divides 3600 exactly, so every
// presentation instant falls on an integer tick and alpha is exact:
//   20 Hz = 180 ticks, 30 = 120, 45 = 80, 60 = 60, 120 = 30, 144 = 25.
// `GuestTime::ticks` is this grid; the hooks do not yet capture the EE cycle counter, so the live path derives ticks
// from the VBlank counter, which is the clock every gate already uses. When the capture carries the finer counter the
// same interfaces keep working (the alpha formula only needs two distinct timestamps).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "coregs3_scene.hpp"
#include "coregs3_timeline.hpp"

namespace gt4::coregs3 {

inline constexpr std::uint64_t kPresentationTicksPerSecond = 3600;
inline constexpr std::uint64_t kPresentationTicksPerGuestFrame = 60;

// The guest presentation tick of one VBlank (vblank * 60), saturating instead of wrapping.
std::uint64_t guest_ticks(std::uint64_t vblank) noexcept;

// The requested presentation rate. 0 means "not one of the supported rates"; callers must report and refuse it, never
// default silently.
std::uint32_t parse_presentation_hz(const char *text) noexcept;
bool presentation_rate_supported(std::uint32_t hz) noexcept;
const char *presentation_rate_list() noexcept;  // "20, 30, 45, 60, 120, 144"
std::uint64_t presentation_step_ticks(std::uint32_t hz) noexcept;  // 0 when unsupported

enum class PresentationMode : std::uint8_t {
  interpolate = 0,  // sample the timeline between the two latest frames (the default)
  nearest = 1,      // repeat the nearest captured frame (the baseline the interpolation is measured against)
};
const char *presentation_mode_name(PresentationMode mode) noexcept;
bool parse_presentation_mode(const char *text, PresentationMode &mode) noexcept;

// The camera of one captured frame, from CameraSnapshot::vf_k4 at the K4 point: registers 0..3 are the four columns of
// the projection and 4..7 the four columns of the view (the same convention as the prediction study's camera_pose).
// P and V are row-major; clip = P * V * world.
struct CameraFrame {
  bool valid = false;
  float projection[4][4] = {};
  float view[4][4] = {};
};
CameraFrame camera_frame(const CameraSnapshot &camera) noexcept;

// A uniform grid of presentation instants in guest ticks. The origin is the timestamp of the first observed guest
// frame; every instant is origin + n * step. A new guest frame makes every grid instant up to its timestamp due, and
// the caller draws them in order (at 120 Hz that is two instants per 60 Hz frame: a mid-interval one and the exact
// frame one). A stall longer than kMaxDuePerFrame would queue stale images, so the clock skips to the last
// kMaxDuePerFrame instants of the gap and counts the loss.
class PresentationClock {
 public:
  static constexpr std::uint32_t kMaxDuePerFrame = 8;

  // rate_hz must satisfy presentation_rate_supported(); an unsupported rate constructs an inert clock (step 0).
  explicit PresentationClock(std::uint32_t rate_hz) noexcept;

  void reset() noexcept;
  std::uint32_t rate_hz() const noexcept { return rate_hz_; }
  std::uint64_t step_ticks() const noexcept { return step_; }
  std::uint64_t origin_ticks() const noexcept { return origin_; }
  std::uint64_t next_ticks() const noexcept { return next_; }
  bool started() const noexcept { return started_; }

  // The instants that became due with a guest frame at `newest_ticks`; the instant k is first + k * step. The first
  // observation makes the frame's own instant due (the grid origin).
  struct Due {
    std::uint32_t count = 0;
    std::uint64_t first = 0;
    std::uint64_t step = 0;
    std::uint64_t skipped = 0;
  };
  Due observe(std::uint64_t newest_ticks) noexcept;

 private:
  std::uint32_t rate_hz_ = 0;
  std::uint64_t step_ = 0;
  std::uint64_t origin_ = 0;
  std::uint64_t next_ = 0;
  bool started_ = false;
};

// Per-instant decisions the transform builder needs. The defaults are the measured ones; the fixture pins them.
struct InterpolationLimits {
  // A pair spanning more than two guest frames means frames were missed; it is refused as a whole.
  std::uint64_t max_pair_ticks = 2 * kPresentationTicksPerGuestFrame;
  // A world translation step larger than this is a teleport/reset (a car at 300 km/h moves ~1.4 m per tick).
  float max_object_translation_step_m = 8.0f;
  // A camera change larger than these is a cut: interpolating across it would sweep the view through the scene.
  float max_camera_translation_step_m = 5.0f;
  float max_camera_rotation_step_rad = 0.5f;
};

// The object-level outcome of one instant. `interpolated` and `entrywise` are objects drawn with a blended matrix;
// `nearest` counts objects drawn with a captured matrix (unmatched, an endpoint alpha, a refused pair or a dropped
// discontinuity). `predicted` stays 0: prediction is a later switch, never the default (plan §5.5).
struct InterpolationCounters {
  std::uint64_t instants = 0;
  std::uint64_t pairs = 0;               // instants whose frame pair and camera could be used
  std::uint64_t pairs_refused = 0;       // unordered, too far apart, camera invalid, or a camera cut
  std::uint64_t paired = 0;              // objects present in both frames
  std::uint64_t only_newer = 0;          // objects present only in the newest frame (drawn with its matrix)
  std::uint64_t only_older = 0;          // objects present only in the older frame (not drawn; the newest frame rules)
  std::uint64_t interpolated = 0;        // objects interpolated with the rigid path
  std::uint64_t entrywise = 0;           // objects interpolated entry by entry (rank-deficient world matrix)
  std::uint64_t nearest = 0;             // objects drawn with a captured matrix
  std::uint64_t predicted = 0;           // reserved for the prediction switch; always 0 here
  std::uint64_t dropped_discontinuity = 0;  // objects (or a whole pair) dropped to the newest captured matrix
  void add(const InterpolationCounters &other) noexcept;
};

// One object of the newest frame and the matrix to draw it with.
enum class TransformSource : std::uint8_t {
  captured_newer = 0,  // the newest frame's own captured upload matrix, drawn exactly as the capture does
  captured_older = 1,  // the older frame's captured upload matrix (alpha 0 / nearest-older)
  interpolated = 2,    // a built matrix (rigid or entrywise)
};
struct DrawTransform {
  ObjectKey key;
  float matrix[16] = {};
  TransformSource source = TransformSource::captured_newer;
};

// Builds one matrix per object of `newer` at `alpha`, pairing with `older` by ObjectKey (id and instance, the
// timeline's stable identity). `out` follows `newer.objects` order and always holds exactly one entry per object:
// objects only in `newer` keep its matrix, objects only in `older` are counted and not drawn (the newest frame's draw
// list is the image's reference), and objects in both follow the rigid rules with the entrywise fallback.
//
// `alpha` is the presentation instant's position between the two frames: <= 0 selects `older` exactly, >= 1 selects
// `newer` exactly (so a 60 Hz presentation, whose instants coincide with the frames, is byte-identical to the capture).
void interpolate_frame(const TimelineFrame &older, const CameraFrame &older_camera, const TimelineFrame &newer,
                       const CameraFrame &newer_camera, float alpha, const InterpolationLimits &limits,
                       std::vector<DrawTransform> &out, InterpolationCounters &counters);

// --- the rigid rules, exposed for the fixture and for diagnostics ---------------------------------------------------
// Decomposes an affine matrix into a uniform scale, a rotation (row-major 3x3) and a translation. False when the
// bottom row is not (0,0,0,1) or the scale is not positive/finite.
bool decompose_rigid(const float matrix[16], float &scale, float rotation[9], float translation[3]) noexcept;
// True when the matrix's linear part is not uniform-scale-times-rotation (the flat shell/reflection draws): the
// relative Gram residual exceeds 1%, or |det| / scale^3 is below 0.05.
bool rigid_rank_deficient(const float matrix[16]) noexcept;
void mat_to_quat(const float rotation[9], float quat[4]) noexcept;  // [w, x, y, z]
void quat_to_mat(const float quat[4], float rotation[9]) noexcept;
void quat_slerp(const float a[4], const float b[4], float alpha, float out[4]) noexcept;  // shortest arc
// Interpolates two captured model->clip matrices through their world poses and the interpolated camera. Returns false
// when either world pose is not rigid (the entrywise fallback) or a camera is unusable.
bool interpolate_object_rigid(const CameraFrame &older_camera, const CameraFrame &newer_camera,
                              const float older_matrix[16], const float newer_matrix[16], float alpha,
                              float out_matrix[16]) noexcept;
// The interpolated projection*view: P entrywise, V rebuilt from the camera pose (position lerp, rotation slerp).
bool interpolated_camera_pv(const CameraFrame &older, const CameraFrame &newer, float alpha,
                            float projection_view[16]) noexcept;
// The camera discontinuity rule: false when the two cameras differ by more than the cut limits above.
bool camera_pair_continuous(const CameraFrame &older, const CameraFrame &newer,
                            const InterpolationLimits &limits) noexcept;

}  // namespace gt4::coregs3
