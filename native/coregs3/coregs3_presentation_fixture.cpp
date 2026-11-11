// CTest native_coregs3_presentation: the live presentation timeline of CoreGS 3.0 (native/coregs3/coregs3_presentation.hpp),
// host only.
//   - the guest tick grid and the supported rates (20/30/45/60/120/144 Hz divide 3600 exactly);
//   - the presentation clock: one instant per 60 Hz frame at 60 Hz, two at 120 Hz, the 2/2/3/2/3 pattern at 144 Hz, one
//     per three frames at 20 Hz, the stall skip and the refusal to step backwards;
//   - the camera frame from the captured K4 registers;
//   - the rigid rules: decomposition, quaternion roundtrip and slerp, rigid object interpolation (translation lerp,
//     rotation slerp, scale lerp), the entrywise fallback for a rank-deficient matrix, exact endpoints;
//   - the camera pose interpolation (position lerp, orientation slerp, projection lerp);
//   - the frame builder: pairing and unmatched counts, alpha endpoints, the camera cut and the teleport drop, the pair
//     gap refusal, and that an object only in the older frame is counted and not drawn.
#include "coregs3_presentation.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {

using namespace gt4::coregs3;

void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

bool near(float a, float b, float tolerance = 1e-4f) { return std::fabs(a - b) <= tolerance; }

void identity(float m[16]) {
  std::memset(m, 0, 16 * sizeof(float));
  m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void rotation_z(float radians, float scale, float m[16]) {
  identity(m);
  const float c = std::cos(radians) * scale;
  const float s = std::sin(radians) * scale;
  m[0] = c;
  m[1] = -s;
  m[4] = s;
  m[5] = c;
  m[10] = scale;
}

void translation(float x, float y, float z, float m[16]) {
  identity(m);
  m[3] = x;
  m[7] = y;
  m[11] = z;
}

ObjectKey key_of(std::uint64_t id, std::uint32_t instance = 0) {
  ObjectKey key;
  key.id = id;
  key.instance = instance;
  return key;
}

TimelineFrame make_timeline(std::uint64_t vblank, std::vector<ObjectTransform> objects) {
  TimelineFrame frame;
  frame.time = GuestTime{vblank, guest_ticks(vblank)};
  frame.index = static_cast<std::uint32_t>(vblank);
  frame.objects = std::move(objects);
  return frame;
}

CameraFrame make_camera(const float view[16], const float projection[16]) {
  CameraFrame camera;
  camera.valid = true;
  std::memcpy(&camera.view[0][0], view, sizeof camera.view);
  std::memcpy(&camera.projection[0][0], projection, sizeof camera.projection);
  return camera;
}

void check_guest_time_and_rates() {
  require(guest_ticks(0) == 0, "vblank 0 is tick 0");
  require(guest_ticks(1) == 60, "one guest frame is 60 ticks");
  require(guest_ticks(100) == 6000, "100 guest frames are 6000 ticks");
  require(guest_ticks(~0ull) == ~0ull, "the tick conversion saturates instead of wrapping");

  require(parse_presentation_hz("20") == 20 && parse_presentation_hz("30") == 30, "20 and 30 Hz parse");
  require(parse_presentation_hz("45") == 45 && parse_presentation_hz("60") == 60, "45 and 60 Hz parse");
  require(parse_presentation_hz("120") == 120 && parse_presentation_hz("144") == 144, "120 and 144 Hz parse");
  require(parse_presentation_hz("50") == 0, "an unsupported rate is refused");
  require(parse_presentation_hz("60x") == 0 && parse_presentation_hz("") == 0, "junk is refused");
  require(presentation_step_ticks(20) == 180 && presentation_step_ticks(30) == 120,
          "20 Hz is 180 ticks, 30 Hz 120");
  require(presentation_step_ticks(45) == 80 && presentation_step_ticks(60) == 60, "45 Hz is 80 ticks, 60 Hz 60");
  require(presentation_step_ticks(120) == 30 && presentation_step_ticks(144) == 25, "120 Hz is 30 ticks, 144 Hz 25");
  require(presentation_step_ticks(50) == 0, "an unsupported rate has no step");

  PresentationMode mode = PresentationMode::interpolate;
  require(parse_presentation_mode(nullptr, mode) && mode == PresentationMode::interpolate, "the default mode");
  require(parse_presentation_mode("nearest", mode) && mode == PresentationMode::nearest, "nearest parses");
  require(parse_presentation_mode("interpolate", mode) && mode == PresentationMode::interpolate,
          "interpolate parses");
  require(!parse_presentation_mode("predict", mode), "an unknown mode is refused");
  require(std::strcmp(presentation_mode_name(PresentationMode::nearest), "nearest") == 0, "the mode name");
}

void check_clock() {
  {
    PresentationClock clock(60);
    PresentationClock::Due due = clock.observe(0);
    require(due.count == 1 && due.first == 0 && due.step == 60, "60 Hz emits the first frame's instant");
    due = clock.observe(60);
    require(due.count == 1 && due.first == 60 && due.skipped == 0, "60 Hz emits one instant per frame");
    due = clock.observe(120);
    require(due.count == 1 && due.first == 120, "60 Hz stays on the frame grid");
  }
  {
    PresentationClock clock(120);
    PresentationClock::Due due = clock.observe(0);
    require(due.count == 1 && due.first == 0, "120 Hz starts at the first frame");
    due = clock.observe(60);
    require(due.count == 2 && due.first == 30 && due.step == 30, "120 Hz emits a midpoint and the frame at 60");
    due = clock.observe(120);
    require(due.count == 2 && due.first == 90, "120 Hz keeps the 30-tick grid");
  }
  {
    PresentationClock clock(144);
    require(clock.observe(0).count == 1, "144 Hz starts at 0");
    PresentationClock::Due due = clock.observe(60);
    require(due.count == 2 && due.first == 25, "144 Hz emits 25 and 50 in the first interval");
    due = clock.observe(120);
    require(due.count == 2 && due.first == 75, "144 Hz emits 75 and 100 in the second");
    due = clock.observe(180);
    require(due.count == 3 && due.first == 125 && due.first + 2 * due.step == 175,
            "144 Hz emits three instants when the grid fits three");
    due = clock.observe(240);
    require(due.count == 2 && due.first == 200, "240 is not on the 25-tick grid: only 200 and 225 are due");
    require(clock.next_ticks() == 250, "the grid continues past the newest frame");
  }
  {
    PresentationClock clock(20);
    require(clock.observe(0).count == 1, "20 Hz starts at 0");
    require(clock.observe(60).count == 0, "20 Hz emits nothing at tick 60");
    require(clock.observe(120).count == 0, "20 Hz emits nothing at tick 120");
    PresentationClock::Due due = clock.observe(180);
    require(due.count == 1 && due.first == 180, "20 Hz emits one instant every three frames");
  }
  {
    PresentationClock clock(120);
    require(clock.observe(0).count == 1, "a stall test starts at 0");
    const PresentationClock::Due due = clock.observe(6000);
    require(due.count == PresentationClock::kMaxDuePerFrame, "a stall keeps at most kMaxDuePerFrame instants");
    require(due.skipped == 200 - PresentationClock::kMaxDuePerFrame, "the stall's lost instants are counted");
    require(due.first == 30 + due.skipped * 30, "the kept instants stay on the grid, nearest the newest frame");
    require(due.first + (due.count - 1) * due.step == 6000, "the last kept instant is the newest frame");
  }
  {
    PresentationClock clock(120);
    require(clock.observe(60).count == 1, "an out-of-order test starts at 60");
    const PresentationClock::Due due = clock.observe(30);
    require(due.count == 0 && clock.next_ticks() == 90, "the clock never steps backwards");
  }
}

void check_camera_frame() {
  CameraSnapshot snapshot;
  snapshot.valid = 2;  // K4 seen
  for (int column = 0; column < 4; ++column)
    for (int row = 0; row < 4; ++row) {
      const std::uint32_t identity_word = row == column ? 0x3F800000u : 0u;  // 1.0f / 0.0f
      snapshot.vf_k4[column][row] = identity_word;
      snapshot.vf_k4[4 + column][row] = identity_word;
    }
  snapshot.vf_k4[4 + 3][0] = 0xC0000000u;  // view column 3, row 0 = -2.0f
  const CameraFrame camera = camera_frame(snapshot);
  require(camera.valid, "the K4 camera is usable");
  require(camera.projection[0][0] == 1.0f && camera.view[1][1] == 1.0f, "projection and view are column-major captures");
  require(camera.view[0][3] == -2.0f, "the view's translation column comes through");
  snapshot.valid = 0;
  require(!camera_frame(snapshot).valid, "a frame without K4 has no camera");
}

void check_rigid_decomposition() {
  float m[16];
  rotation_z(0.7f, 2.0f, m);
  m[3] = 1.0f;
  m[7] = 2.0f;
  m[11] = 3.0f;
  float scale = 0.0f, rotation[9] = {}, offset[3] = {};
  require(decompose_rigid(m, scale, rotation, offset), "a uniform scale times a rotation decomposes");
  require(near(scale, 2.0f), "the scale is recovered");
  require(near(offset[0], 1.0f) && near(offset[1], 2.0f) && near(offset[2], 3.0f), "the translation is recovered");
  require(near(rotation[0], std::cos(0.7f)) && near(rotation[1], -std::sin(0.7f)), "the rotation is recovered");
  require(!rigid_rank_deficient(m), "a scaled rotation is not rank deficient");

  float flat[16];
  identity(flat);
  flat[2] = flat[6] = flat[10] = 0.0f;  // the third linear column collapses: the flat shell/reflection draws
  require(rigid_rank_deficient(flat), "a rank-deficient linear part is detected");

  float shear[16];
  identity(shear);
  shear[1] = 0.4f;
  require(rigid_rank_deficient(shear), "a shear is not uniform-scale times rotation");

  float not_affine[16];
  identity(not_affine);
  not_affine[12] = 0.5f;
  require(!decompose_rigid(not_affine, scale, rotation, offset), "a projective bottom row is refused");
}

void check_quaternions() {
  float rotation[9];
  const float half = static_cast<float>(3.14159265358979323846) / 4.0f;
  const float q[4] = {std::cos(half), 0.0f, 0.0f, std::sin(half)};
  quat_to_mat(q, rotation);
  float back[4];
  mat_to_quat(rotation, back);
  require(near(back[0], q[0]) || near(back[0], -q[0]), "the quaternion roundtrip keeps the rotation");
  require(near(std::fabs(back[3]), std::fabs(q[3])), "the roundtrip keeps the z component");

  float a[4] = {1.0f, 0.0f, 0.0f, 0.0f};
  float b[4] = {std::cos(half), 0.0f, 0.0f, std::sin(half)};
  float mid[4];
  quat_slerp(a, b, 0.5f, mid);
  float mid_rotation[9];
  quat_to_mat(mid, mid_rotation);
  // q is a 90 degree rotation, so the midpoint is 45 degrees: matrix[0] = cos(45) = cos(half).
  require(near(mid_rotation[0], std::cos(half), 1e-5f), "the slerp midpoint is half the angle");
  require(near(mid_rotation[1], -std::sin(half), 1e-5f), "the slerp midpoint keeps the axis");

  // A rotation of 350 degrees must take the short arc (10 degrees), not blend the long way round.
  const float long_half = 3.05432619f;  // 175 degrees, i.e. a 350 degree rotation about z
  float long_q[4] = {std::cos(long_half), 0.0f, 0.0f, std::sin(long_half)};
  float short_mid[4];
  quat_slerp(a, long_q, 0.5f, short_mid);
  require(short_mid[0] > 0.999f, "slerp takes the shortest arc (the 5 degree midpoint of the 10 degree arc)");
}

void check_rigid_object_interpolation() {
  float identity_matrix[16], moved[16], out[16];
  identity(identity_matrix);
  translation(2.0f, 0.0f, 0.0f, moved);
  const CameraFrame camera = make_camera(identity_matrix, identity_matrix);
  require(interpolate_object_rigid(camera, camera, identity_matrix, moved, 0.5f, out),
          "a translation interpolates rigidly");
  require(near(out[3], 1.0f) && near(out[7], 0.0f), "the midpoint translation is half");
  require(near(out[0], 1.0f) && near(out[5], 1.0f), "the rotation stays identity");

  float rotated[16];
  rotation_z(static_cast<float>(3.14159265358979323846) / 2.0f, 1.0f, rotated);
  require(interpolate_object_rigid(camera, camera, identity_matrix, rotated, 0.5f, out),
          "a rotation interpolates rigidly");
  require(near(out[0], std::cos(3.14159265358979323846 / 4.0)) && near(out[1], -std::sin(3.14159265358979323846 / 4.0)),
          "the midpoint rotation is 45 degrees");
  require(near(out[3], 0.0f), "a pure rotation keeps the origin");

  float flat[16];
  identity(flat);
  flat[2] = flat[6] = flat[10] = 0.0f;
  require(!interpolate_object_rigid(camera, camera, identity_matrix, flat, 0.5f, out),
          "a rank-deficient object asks for the entrywise fallback");
}

void check_camera_interpolation() {
  float identity_matrix[16];
  identity(identity_matrix);
  float view_origin[16], view_moved[16];
  translation(0.0f, 0.0f, 0.0f, view_origin);
  translation(-2.0f, 0.0f, 0.0f, view_moved);  // the camera moves +2 m in world x, so V's translation is -2
  const CameraFrame older = make_camera(view_origin, identity_matrix);
  const CameraFrame newer = make_camera(view_moved, identity_matrix);
  float pv[16];
  require(interpolated_camera_pv(older, newer, 0.5f, pv), "a camera pair interpolates");
  require(near(pv[3], -1.0f), "the camera position is interpolated, so V translation is -1 at the midpoint");

  CameraFrame invalid = older;
  invalid.valid = false;
  require(!interpolated_camera_pv(invalid, newer, 0.5f, pv), "a missing camera refuses the pair");

  InterpolationLimits limits;
  require(camera_pair_continuous(older, newer, limits), "a 2 m camera move is not a cut");
  float view_far[16];
  translation(-10.0f, 0.0f, 0.0f, view_far);
  const CameraFrame far = make_camera(view_far, identity_matrix);
  require(!camera_pair_continuous(older, far, limits), "a 10 m camera jump is a cut");
}

void check_frame_interpolation() {
  float identity_matrix[16], moved[16];
  identity(identity_matrix);
  translation(2.0f, 0.0f, 0.0f, moved);
  float flat[16];
  identity(flat);
  flat[2] = flat[6] = flat[10] = 0.0f;

  ObjectTransform rigid_older, rigid_newer, shell_older, shell_newer, gone;
  rigid_older.key = key_of(0x11);
  std::memcpy(rigid_older.matrix, identity_matrix, sizeof rigid_older.matrix);
  rigid_newer.key = key_of(0x11);
  std::memcpy(rigid_newer.matrix, moved, sizeof rigid_newer.matrix);
  shell_older.key = key_of(0x22);
  std::memcpy(shell_older.matrix, identity_matrix, sizeof shell_older.matrix);
  shell_newer.key = key_of(0x22);
  std::memcpy(shell_newer.matrix, flat, sizeof shell_newer.matrix);
  gone.key = key_of(0x33);
  std::memcpy(gone.matrix, identity_matrix, sizeof gone.matrix);

  const TimelineFrame older_frame = make_timeline(100, {rigid_older, shell_older, gone});
  const TimelineFrame newer_frame = make_timeline(101, {rigid_newer, shell_newer});
  const CameraFrame camera = make_camera(identity_matrix, identity_matrix);
  InterpolationLimits limits;
  std::vector<DrawTransform> out;
  InterpolationCounters counters;

  interpolate_frame(older_frame, camera, newer_frame, camera, 0.5f, limits, out, counters);
  require(out.size() == 2, "one transform per object of the newest frame");
  require(counters.instants == 1 && counters.pairs == 1 && counters.pairs_refused == 0, "the pair is usable");
  require(counters.paired == 2 && counters.only_older == 1 && counters.only_newer == 0,
          "pairing counts the vanished object as only-older");
  require(counters.interpolated == 1 && counters.entrywise == 1 && counters.nearest == 0,
          "the rigid object interpolates and the flat one falls back");
  require(out[0].key.id == 0x11 && out[0].source == TransformSource::interpolated, "the rigid object is blended");
  require(near(out[0].matrix[3], 1.0f), "the rigid object is at the midpoint");
  require(out[1].key.id == 0x22 && out[1].source == TransformSource::interpolated, "the flat object is blended");
  require(near(out[1].matrix[10], 0.5f), "the flat object uses the entrywise midpoint");

  // Endpoints: alpha 1 is the newest captured matrix, bit for bit (the 60 Hz present path).
  out.clear();
  counters = InterpolationCounters{};
  interpolate_frame(older_frame, camera, newer_frame, camera, 1.0f, limits, out, counters);
  require(out[0].source == TransformSource::captured_newer && out[1].source == TransformSource::captured_newer,
          "alpha 1 draws the newest captured matrices");
  require(std::memcmp(out[0].matrix, moved, sizeof moved) == 0, "alpha 1 is the captured matrix bit for bit");
  require(counters.nearest == 2 && counters.interpolated == 0, "alpha 1 counts captured draws, not blends");

  // A camera cut drops the whole instant to the newest frame.
  float view_far[16];
  translation(-10.0f, 0.0f, 0.0f, view_far);
  const CameraFrame cut_camera = make_camera(view_far, identity_matrix);
  out.clear();
  counters = InterpolationCounters{};
  interpolate_frame(older_frame, camera, newer_frame, cut_camera, 0.5f, limits, out, counters);
  require(counters.pairs_refused == 1 && counters.dropped_discontinuity == 2, "a camera cut drops both objects");
  require(out[0].source == TransformSource::captured_newer && out[1].source == TransformSource::captured_newer,
          "a cut presents the newest captured matrices");

  // An object that jumps 20 m in one frame is a teleport: it alone is dropped to the newest matrix.
  float teleported[16];
  translation(20.0f, 0.0f, 0.0f, teleported);
  ObjectTransform jump = rigid_newer;
  std::memcpy(jump.matrix, teleported, sizeof jump.matrix);
  const TimelineFrame teleport_frame = make_timeline(101, {jump, shell_newer});
  out.clear();
  counters = InterpolationCounters{};
  interpolate_frame(older_frame, camera, teleport_frame, camera, 0.5f, limits, out, counters);
  require(counters.dropped_discontinuity == 1, "the teleport alone is dropped");
  require(out[0].source == TransformSource::captured_newer && near(out[0].matrix[3], 20.0f),
          "the teleported object is drawn at its newest position");

  // A pair spanning more than two guest frames is refused as a whole.
  const TimelineFrame far_frame = make_timeline(103, {rigid_newer, shell_newer});
  out.clear();
  counters = InterpolationCounters{};
  interpolate_frame(older_frame, camera, far_frame, camera, 0.5f, limits, out, counters);
  require(counters.pairs_refused == 1 && counters.dropped_discontinuity == 0,
          "a missed-frame gap refuses the pair without calling it a discontinuity");
  require(out[0].source == TransformSource::captured_newer, "a refused pair draws the newest matrices");
}

}  // namespace

int main() {
  try {
    check_guest_time_and_rates();
    check_clock();
    check_camera_frame();
    check_rigid_decomposition();
    check_quaternions();
    check_rigid_object_interpolation();
    check_camera_interpolation();
    check_frame_interpolation();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "coregs3 presentation fixture: %s\n", error.what());
    return 1;
  }
  std::printf("coregs3 presentation fixture: ok\n");
  return 0;
}
