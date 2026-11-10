// CTest native_coregs3_vertex: the measured VU1 vertex transform (native/coregs3/coregs3_vertex.{hpp,cpp}) on
// hand-authored data only. No game data is read.
//
//   - the three hand-checked pairs of docs/COREGS3-HOOKS.md §10.2 (draw 0 pairs 0-1, draw 3 pair 0), with the raw VU0
//     columns and the captured shape `value` transcribed from the gate's printed vf24..vf27/value: the corrected metre
//     transform must project within 0.3 px of the tapped screen position minus the inferred XYOFFSET;
//   - the upload rule: the linear columns c0..c2 are vf24..vf26 * cmd.value, the translation c3 = vf27 is never scaled,
//     and a `value` of exactly zero is the guest's unscaled branch. The metre transform adds 4096 units per metre for a
//     V3-16 channel and nothing for a V3-32 channel. These equalities are the controls that fail if the old unscaled
//     reading of vf24..vf27 comes back;
//   - the column convention itself: component i of the clip position is built from component i of every column;
//   - a synthetic off-screen vertex and a behind-camera vertex: the projection is either valid and unwrapped (still
//     thousands of pixels outside the viewport) or rejected, never the 12.4 wrapped coordinate;
//   - the diagnostics-only wrap helper reproduces the measured wrap;
//   - non-finite matrix or model input is rejected with an explicit status.
#include "coregs3_vertex.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

using namespace gt4::coregs3;

void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

std::uint32_t float_bits(float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof bits);
  return bits;
}

VertexColumns columns_from_floats(const float values[4][4]) {
  // values[0..3] are c0..c3 of a hand-written matrix, each four floats.
  std::array<std::array<std::uint32_t, 4>, 4> raw{};
  for (std::size_t column = 0; column < 4; ++column)
    for (std::size_t lane = 0; lane < 4; ++lane) raw[column][lane] = float_bits(values[column][lane]);
  return vertex_columns_from_raw(raw);
}

struct Vu0Block {
  // The capture's vf24..vf31 as the C array the vertex module takes; the 0xC1 block is the first four rows.
  std::uint32_t vf[8][4] = {};
};

Vu0Block vu0_from_floats(const float values[4][4]) {
  Vu0Block block;
  for (std::size_t row = 0; row < 4; ++row)
    for (std::size_t lane = 0; lane < 4; ++lane) block.vf[row][lane] = float_bits(values[row][lane]);
  return block;
}

// The measured raw columns of two night-race draws, transcribed from build/coregs3-r7/gate-fits.txt (docs/COREGS3-HOOKS.md
// §10.2). Order: c0 = vf24, c1 = vf25, c2 = vf26, c3 = vf27.
const float kDraw0Vf[4][4] = {
    {2.037678f, -0.09633104f, -0.108714f, -0.1086959f},
    {-1.607574f, -1.367137f, -1.542846f, -1.54259f},
    {-0.2296981f, -0.854579f, -0.9644135f, -0.9642529f},
    {18.51846f, 85.71249f, 137.8914f, 138.4234f},
};
const float kDraw3Vf[4][4] = {
    {2.037438f, -0.04543376f, -0.1123454f, -0.1123272f},
    {-0.03131399f, 3.449506f, -0.2452934f, -0.2452537f},
    {-0.2296948f, -0.8732733f, -0.9630855f, -0.9629296f},
    {18.51846f, 86.03542f, 137.8842f, 138.4009f},
};
// Both fitted draws carry the captured Cmd float `value 0.000232830644` (gate-fits.txt). The exact stored float is
// 1e6 / 2^32 = 0.00023283064365386963; this literal rounds to it.
constexpr float kBodyValue = 0.000232830644f;
// The other two value classes measured on the same VBlank 7398 frame (gate-fits.txt): 6.10370189e-05 and
// 0.000152592547, i.e. 4096 * value = 0.2500076 and 0.625019.
constexpr float kWheelValue = 6.10370189e-05f;

struct PairCheck {
  const char *name;
  const float (*vf)[4];  // raw vf24..vf27 (16 floats)
  float cmd_value;
  ModelPosition model;
  float tapped_x;  // tapped screen position minus the inferred XYOFFSET (-2368, +1824) px
  float tapped_y;
};

void check_hand_checked_pairs() {
  const PairCheck checks[] = {
      {"draw 0 pair 0", kDraw0Vf, kBodyValue, {-0.79858f, 0.01440f, 1.64233f}, 358.6250f, 86.0000f},
      {"draw 0 pair 1", kDraw0Vf, kBodyValue, {-0.73511f, 0.01440f, 1.78735f}, 358.8750f, 86.0625f},
      {"draw 3 pair 0", kDraw3Vf, kBodyValue, {-0.74268f, 0.83325f, -0.78296f}, 359.5000f, 79.8750f},
  };
  const float width = 640.0f, height = 448.0f;
  for (const PairCheck &check : checks) {
    const Vu0Block vu0 = vu0_from_floats(check.vf);
    // The hand-authored model positions are metres (raw 16-bit / 4096), the measured V3-16 encoding.
    const VertexColumns columns =
        vertex_columns_from_vu0_metres(vu0.vf, check.cmd_value, PositionEncoding::integer_16);
    require(vertex_columns_finite(columns), "the measured columns are finite");
    const ProjectionResult result = project_vertex(columns, check.model, width, height, true);
    require(result.status == ProjectionStatus::ok, check.name);
    require(result.clip.w > 0.0f, "the hand-checked vertex is in front of the camera");
    require(std::fabs(result.screen.x - check.tapped_x) < 0.3f, check.name);
    require(std::fabs(result.screen.y - check.tapped_y) < 0.3f, check.name);
  }
}

void check_upload_rule_and_units() {
  const Vu0Block vu0 = vu0_from_floats(kDraw0Vf);
  const VertexColumns raw = vertex_columns_from_vu0_raw(vu0.vf);
  require(vu1_linear_upload_factor(kBodyValue) == kBodyValue, "a nonzero value is the upload factor");
  require(vu1_linear_upload_factor(0.0f) == 1.0f, "a zero value takes the guest's unscaled branch");
  require(vu1_linear_upload_factor(-0.0f) == 1.0f, "-0.0 compares equal to zero in the guest branch too");

  // The 0xC1 upload (func_0049E548): vf24..vf26 * value, and vf27 untouched. These equalities are exact, because the
  // module and this fixture perform the same single multiply.
  const VertexColumns upload = vertex_columns_from_vu0_upload(vu0.vf, kBodyValue);
  for (std::size_t column = 0; column < 3; ++column)
    for (std::size_t lane = 0; lane < 4; ++lane)
      require(upload.column[column][lane] == raw.column[column][lane] * kBodyValue,
              "the upload scales vf24..vf26 by cmd.value");
  for (std::size_t lane = 0; lane < 4; ++lane)
    require(upload.column[3][lane] == raw.column[3][lane], "the upload never scales the translation vf27");

  // The metre transforms: 4096 units per metre for a V3-16 channel, the stored float for a V3-32 channel.
  const float integer_scale = metre_linear_scale(kBodyValue, PositionEncoding::integer_16);
  require(integer_scale == kBodyValue * 4096.0f, "the V3-16 metre scale is cmd.value * 4096");
  require(metre_linear_scale(kBodyValue, PositionEncoding::float_32) == kBodyValue,
          "the V3-32 metre scale is cmd.value");
  const VertexColumns integral = vertex_columns_from_vu0_metres(vu0.vf, kBodyValue, PositionEncoding::integer_16);
  const VertexColumns floating = vertex_columns_from_vu0_metres(vu0.vf, kBodyValue, PositionEncoding::float_32);
  for (std::size_t column = 0; column < 3; ++column)
    for (std::size_t lane = 0; lane < 4; ++lane) {
      require(integral.column[column][lane] == raw.column[column][lane] * integer_scale,
              "the V3-16 metre transform is vf * (value * 4096)");
      require(floating.column[column][lane] == raw.column[column][lane] * kBodyValue,
              "the V3-32 metre transform is vf * value");
    }
  for (std::size_t lane = 0; lane < 4; ++lane)
    require(integral.column[3][lane] == raw.column[3][lane] &&
                floating.column[3][lane] == raw.column[3][lane],
            "both metre transforms keep the translation unscaled");

  // The old unscaled reading used vf24..vf27 with metre positions, which is neither set. This is the control that
  // fails if that reading returns: its projection of a measured vertex differs by a visible amount.
  require(integral.column[0][0] != raw.column[0][0] && floating.column[0][0] != raw.column[0][0],
          "the measured transform is not the raw vf block");
  const ModelPosition hand = {-0.79858f, 0.01440f, 1.64233f};
  const ProjectionResult corrected = project_vertex(integral, hand, 640.0f, 448.0f, true);
  const ProjectionResult old = project_vertex(raw, hand, 640.0f, 448.0f, true);
  require(corrected.status == ProjectionStatus::ok && old.status == ProjectionStatus::ok,
          "both readings project the measured vertex");
  require(std::fabs(corrected.screen.x - old.screen.x) > 0.1f || std::fabs(corrected.screen.y - old.screen.y) > 0.1f,
          "the corrected transform is not the unscaled reading");

  // The wheel/part value class measured on the same frame: 4096 * value = 0.2500076, so the old reading overshot the
  // linear part by four times.
  const float wheel_scale = metre_linear_scale(kWheelValue, PositionEncoding::integer_16);
  require(wheel_scale == kWheelValue * 4096.0f && std::fabs(wheel_scale - 0.2500076f) < 1e-6f,
          "the wheel-class metre scale is about 0.25");
  const VertexColumns wheel = vertex_columns_from_vu0_metres(vu0.vf, kWheelValue, PositionEncoding::integer_16);
  require(wheel.column[0][0] == raw.column[0][0] * wheel_scale,
          "the wheel-class transform follows the same rule");
  require(std::fabs(wheel.column[0][0] / raw.column[0][0] - 0.25f) < 1e-5f,
          "the wheel-class reading is a quarter of the old unscaled one");

  // The guest's zero branch: the rows are the raw block, and the V3-16 metre transform still converts its units.
  const VertexColumns zero_upload = vertex_columns_from_vu0_upload(vu0.vf, 0.0f);
  for (std::size_t column = 0; column < 3; ++column)
    for (std::size_t lane = 0; lane < 4; ++lane)
      require(zero_upload.column[column][lane] == raw.column[column][lane],
              "value zero uploads vf24..vf26 unscaled");
  const VertexColumns zero_metres = vertex_columns_from_vu0_metres(vu0.vf, 0.0f, PositionEncoding::integer_16);
  require(zero_metres.column[0][0] == raw.column[0][0] * 4096.0f,
          "the zero-value V3-16 metre transform is vf * 4096");

  // A non-finite value is never silently replaced by an unscaled matrix: the outcome is non-finite and refused.
  const float nan = std::numeric_limits<float>::quiet_NaN();
  require(!vertex_columns_finite(vertex_columns_from_vu0_metres(vu0.vf, nan, PositionEncoding::integer_16)),
          "a NaN value makes the columns non-finite");
}

void check_column_convention() {
  // c0 = (2,0,0,0) contributes only to clip.x; c1 = (0,3,0,0) only to clip.y; c2 = (0,0,4,0) only to clip.z;
  // c3 = (5,6,7,8) contributes to every component. A row reading of the same sixteen values would compute a different
  // clip position, so this separates the two conventions.
  const float columns[4][4] = {
      {2.0f, 0.0f, 0.0f, 0.0f},
      {0.0f, 3.0f, 0.0f, 0.0f},
      {0.0f, 0.0f, 4.0f, 0.0f},
      {5.0f, 6.0f, 7.0f, 8.0f},
  };
  const VertexColumns matrix = columns_from_floats(columns);
  const ClipPosition clip = transform_model_position(matrix, {1.0f, 2.0f, 3.0f});
  require(clip.x == 2.0f * 1.0f + 5.0f, "clip.x is c0[0]*x + c3[0]");
  require(clip.y == 3.0f * 2.0f + 6.0f, "clip.y is c1[1]*y + c3[1]");
  require(clip.z == 4.0f * 3.0f + 7.0f, "clip.z is c2[2]*z + c3[2]");
  require(clip.w == 8.0f, "clip.w is c3[3] here");

  // The raw VU0 constructor reads vf24..vf27 in the same order as the raw columns.
  const Vu0Block vu0 = vu0_from_floats(columns);
  const VertexColumns from_vu0 = vertex_columns_from_vu0_raw(vu0.vf);
  for (std::size_t column = 0; column < 4; ++column)
    for (std::size_t lane = 0; lane < 4; ++lane)
      require(from_vu0.column[column][lane] == matrix.column[column][lane], "vf24..vf27 are columns c0..c3");

  // The row-major form stores component r of every column in row r.
  const VertexRows rows = vertex_rows(matrix);
  for (std::size_t row = 0; row < 4; ++row)
    for (std::size_t lane = 0; lane < 4; ++lane)
      require(rows[row][lane] == matrix.column[lane][row], "row r holds component r of every column");

  // A simple y-flip check on an identity transform.
  const float identity[4][4] = {
      {1.0f, 0.0f, 0.0f, 0.0f},
      {0.0f, 1.0f, 0.0f, 0.0f},
      {0.0f, 0.0f, 1.0f, 0.0f},
      {0.0f, 0.0f, 0.0f, 1.0f},
  };
  const VertexColumns identity_columns = columns_from_floats(identity);
  const ProjectionResult down = project_vertex(identity_columns, {0.0f, 1.0f, 1.0f}, 64.0f, 48.0f, true);
  const ProjectionResult up = project_vertex(identity_columns, {0.0f, 1.0f, 1.0f}, 64.0f, 48.0f, false);
  require(down.status == ProjectionStatus::ok && up.status == ProjectionStatus::ok, "the identity projects");
  require(down.screen.x == 32.0f && up.screen.x == 32.0f, "the horizontal centre is shared");
  require(down.screen.y == 0.0f && up.screen.y == 48.0f, "flip_y inverts the y axis");
}

void check_behind_camera_and_off_screen() {
  // c2 = (0,0,0,1): clip.w is the model z, so z > 0 is in front and z <= 0 is at or behind the eye plane.
  const float w_is_z[4][4] = {
      {1.0f, 0.0f, 0.0f, 0.0f},
      {0.0f, 1.0f, 0.0f, 0.0f},
      {0.0f, 0.0f, 0.0f, 1.0f},
      {0.0f, 0.0f, 0.0f, 0.0f},
  };
  const VertexColumns columns = columns_from_floats(w_is_z);
  const float width = 640.0f, height = 448.0f;

  for (const float z : {-1.0f, 0.0f}) {
    const ProjectionResult behind = project_vertex(columns, {0.0f, 0.0f, z}, width, height, true);
    require(behind.status == ProjectionStatus::behind_camera, "a vertex at or behind the eye plane is rejected");
    require(behind.clip.w == z, "the rejected vertex keeps its clip position for a caller that clips");
  }
  const ProjectionResult front = project_vertex(columns, {0.0f, 0.0f, 2.0f}, width, height, true);
  require(front.status == ProjectionStatus::ok && front.clip.w == 2.0f, "a vertex in front projects");

  // An off-screen vertex: ndc_y = -17.669 puts the true screen y at about 4182 px, far below the 448-px viewport. The
  // 16-bit 12.4 store would wrap it back to about 86 px; the module must return the true coordinate, never the wrap.
  const ProjectionResult off = project_vertex(columns, {0.0f, -17.669f, 1.0f}, width, height, true);
  require(off.status == ProjectionStatus::ok, "an off-screen vertex in front of the camera still projects");
  require(off.screen.y > 4096.0f, "the off-screen coordinate is not wrapped back into the viewport");
  require(std::fabs(off.screen.y - 4181.856f) < 0.01f, "the off-screen coordinate is the true projection");

  // The diagnostics helper explains the measured wrap: the GS would carry 30558 in 12.4, which reads as 1909.875 px,
  // i.e. the true 4181.856 minus one 4096-px wrap, and subtracting OFY = 1824 px leaves the on-screen-looking 85.875.
  const GsWrapped12_4 wrap = diagnostics_gs_wrapped_12_4(off.screen.y, 1824.0f);
  require(wrap.stored == 30558u, "the 12.4 store wraps modulo 65536 units");
  require(wrap.signed_read_px == 1909.875f, "the signed read-back is the wrapped screen coordinate");
  require(std::fabs((wrap.signed_read_px - 1824.0f) - (off.screen.y - 4096.0f)) < 0.1f,
          "the wrapped read is one 4096-px step away from the true projection");
  require(wrap.stored < 0x8000u && wrap.signed_read_px == wrap.unsigned_read_px,
          "below the sign bit the signed and unsigned readings agree");

  // The x-axis case that the capture printed: screen x = 358.625 px with OFX = -2368 px stores 33386 (negative when
  // read signed). An off-screen x one wrap away stores exactly the same 16 bits, which is why the captured stream
  // cannot be used to place an off-screen vertex.
  const GsWrapped12_4 on_screen = diagnostics_gs_wrapped_12_4(358.625f, -2368.0f);
  const GsWrapped12_4 wrapped = diagnostics_gs_wrapped_12_4(358.625f + 4096.0f, -2368.0f);
  require(on_screen.stored == 33386u && on_screen.signed_read_px == -2009.375f,
          "the measured draw-0 x reads -2009.375 px signed");
  require(on_screen.signed_read_px < 0.0f && on_screen.unsigned_read_px > 0.0f,
          "above the sign bit the two readings differ");
  require(wrapped.stored == on_screen.stored && wrapped.signed_read_px == on_screen.signed_read_px,
          "one 4096-px wrap changes the stored coordinate by exactly one 16-bit period");
}

void check_rejections() {
  const float identity[4][4] = {
      {1.0f, 0.0f, 0.0f, 0.0f},
      {0.0f, 1.0f, 0.0f, 0.0f},
      {0.0f, 0.0f, 1.0f, 0.0f},
      {0.0f, 0.0f, 0.0f, 1.0f},
  };
  VertexColumns columns = columns_from_floats(identity);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();

  // Non-finite model positions are rejected with an explicit status, not projected.
  const ProjectionResult nan_position = project_vertex(columns, {nan, 0.0f, 1.0f}, 640.0f, 448.0f);
  require(nan_position.status == ProjectionStatus::not_finite, "a NaN model position is rejected");
  const ProjectionResult inf_position = project_vertex(columns, {inf, 0.0f, 1.0f}, 640.0f, 448.0f);
  require(inf_position.status == ProjectionStatus::not_finite, "an infinite model position is rejected");

  // Non-finite matrix columns are rejected both by the predicate the renderer uses and by the projection.
  columns.column[3][2] = nan;
  require(!vertex_columns_finite(columns), "a NaN column is not finite");
  require(project_vertex(columns, {0.0f, 0.0f, 1.0f}, 640.0f, 448.0f).status == ProjectionStatus::not_finite,
          "a NaN column is rejected");

  // A clip position with no usable w, and a viewport with no area.
  ClipPosition clip;
  clip.x = 1.0f;
  clip.y = 1.0f;
  clip.w = 0.0f;
  require(project_clip_position(clip, 640.0f, 448.0f).status == ProjectionStatus::behind_camera,
          "w = 0 is behind the eye plane");
  clip.w = 1.0f;
  require(project_clip_position(clip, 0.0f, 448.0f).status == ProjectionStatus::degenerate,
          "a zero-width viewport is degenerate");
  clip.w = nan;
  require(project_clip_position(clip, 640.0f, 448.0f).status == ProjectionStatus::not_finite,
          "a NaN clip position is rejected");
  require(std::string(projection_status_name(ProjectionStatus::behind_camera)) == "behind_camera",
          "the status has a name");
}

}  // namespace

int main() {
  try {
    check_hand_checked_pairs();
    check_upload_rule_and_units();
    check_column_convention();
    check_behind_camera_and_off_screen();
    check_rejections();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "coregs3 vertex fixture: %s\n", error.what());
    return 1;
  }
  std::printf("coregs3 vertex fixture: ok\n");
  return 0;
}
