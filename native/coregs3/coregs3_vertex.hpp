// TASK-090 CoreGS 3.0: the measured VU1 vertex transform, shared by the offline reference renderer (native/coregs3/
// coregs3_render.cpp) and the live `present` layer. The measurement and its evidence are docs/COREGS3-HOOKS.md §10.2;
// labels there follow docs/COREGS3-PLAN.md (H = High confidence).
//
// The convention:
//   * the matrix the VU1 receives at qword 0xC1 is four COLUMNS, not rows: c0 = VU0 vf24, c1 = vf25, c2 = vf26 and
//     c3 = vf27. The guest's upload (func_0049E548) scales the three LINEAR columns c0..c2 by the shape's Cmd float
//     `value` (Cmd + 0x04); when `value` is exactly zero it stores them unscaled (the zero branch of the upload). The
//     translation column c3 is never scaled. The captured packet rows are exactly `vf24..vf26 * value, vf27`;
//   * model positions are metres: a V3-16 channel is a signed 16-bit value and build_mesh stores raw / 4096 (its
//     default position_scale, coregs3_shape.hpp), while a V3-32 channel is a float and build_mesh stores it as it
//     stands. The VU1 multiplies the raw integer for the first and the float for the second, so the metre transform
//     carries `(value or 1) * 4096` for a V3-16 vertex and `(value or 1)` for a V3-32 vertex: the two column sets
//     `vertex_columns_from_vu0_metres` builds. This is the only place a shape scale enters the vertex stage;
//   * clip = c0 * x + c1 * y + c2 * z + c3 * 1, componentwise, i.e. clip[i] = sum(j) column[j][i] * model[j];
//   * screen = ((0.5 + 0.5 * clip.x / clip.w) * width, (0.5 - 0.5 * clip.y / clip.w) * height). The captured night-race
//     frames are 640x448; the formula itself is resolution independent.
//
// History: the previous revision of this module used vf24..vf27 unscaled with metre positions. That is wrong by
// `(value or 1) * 4096` on the linear part; the correction and its measured effect are in docs/COREGS3-HOOKS.md
// §10.2 (the linear columns of the body/paint family are scaled by about 0.954, and the wheel/part families by 0.25
// and 0.625, so the old reading placed them by multiples of their true size).
//
// The 16-bit wrap trap: after the transform the VU1 adds the drawing offset and the GS stores each screen coordinate as
// a 16-bit 12.4 fixed-point value, so a coordinate more than 4096 px outside the drawing window wraps modulo 65536
// units (4096 px). The measured off-screen draws read about +4096 px away from the true projection for exactly that
// reason (docs/COREGS3-HOOKS.md §10.2). The wrap is arithmetic on an off-screen vertex, not a second transform. This
// module therefore never reconstructs it: it exposes the clip position, projects only when clip.w is positive, and
// lets the caller clip triangles in clip space. `diagnostics_gs_wrapped_12_4` below reproduces the wrap for analysis
// and is never used by the renderer.
#pragma once

#include <array>
#include <cstdint>
#include <limits>

namespace gt4::coregs3 {

// The VU1 and the GS work on IEEE-754 binary32. Keep the width explicit so a capture, a fixture and the GPU vertex
// shader (which will use 32-bit floats) agree.
using Float32 = float;
static_assert(sizeof(Float32) == 4, "the vertex transform requires 32-bit floats");
static_assert(std::numeric_limits<Float32>::is_iec559, "the vertex transform requires IEEE-754 floats");

// The near plane, in clip-space w units. A vertex at or behind it is never projected (the perspective divide by a
// near-zero w is what turns a valid off-screen vertex into noise). The renderer clips triangles against w >= this
// value before projecting, so the clip position stays the only source of screen coordinates.
inline constexpr Float32 kMinClipW = 1e-6f;

// The model position unit of a V3-16 channel: its signed 16-bit values carry 4096 units per metre, the divisor build_mesh
// applies by default (coregs3_shape.hpp).
inline constexpr Float32 kModelPositionUnitsPerMetre = 4096.0f;

// Which measured model position channel a mesh vertex came from. The two reach the VU1 in different units, so the same
// uploaded matrix needs a different linear scale for each (see metre_linear_scale).
enum class PositionEncoding : std::uint8_t {
  integer_16 = 0,  // a V3-16 channel: signed 16-bit model units, stored by build_mesh as raw / 4096 metres
  float_32 = 1,    // a V3-32 channel: the stored float, which build_mesh uses as it stands
};
const char *position_encoding_name(PositionEncoding encoding) noexcept;

// The four columns of the 0xC1 matrix block, in order c0..c3 (= VU0 vf24..vf27), each four floats. Which scale the
// columns carry depends on the constructor used: `vertex_columns_from_vu0_raw` leaves them exactly as VU0 holds them,
// `vertex_columns_from_vu0_upload` applies the shape's `value`, and `vertex_columns_from_vu0_metres` is the set a
// renderer uses with metre model positions (it applies `value` and the position encoding's unit conversion).
struct VertexColumns {
  std::array<std::array<Float32, 4>, 4> column{};
};

// Row-major form of a column set: rows[r][c] = columns.column[c][r]. This is the form of the uploaded 0xC1 matrix
// (rows 0-2 hold the shape-scaled linear columns, row 3 the translation) and the form the flat-shading normal rotation
// and the presentation timeline use.
using VertexRows = std::array<std::array<Float32, 4>, 4>;
VertexRows vertex_rows(const VertexColumns &columns) noexcept;

// A model-space position in metres (a V3-16 raw value / 4096, or a V3-32 float as stored).
using ModelPosition = std::array<Float32, 3>;

// clip = (cx, cy, cz, cw), componentwise c0*x + c1*y + c2*z + c3*1.
struct ClipPosition {
  Float32 x = 0.0f;
  Float32 y = 0.0f;
  Float32 z = 0.0f;
  Float32 w = 0.0f;
};

struct ScreenPosition {
  Float32 x = 0.0f;
  Float32 y = 0.0f;
};

// The outcome of a projection. `screen` is defined only when `status == ok`; every other status means the caller must
// reject or clip the vertex, never substitute another coordinate (in particular never the 12.4 wrapped one).
enum class ProjectionStatus : std::uint8_t {
  ok = 0,
  not_finite,     // a column, a model position or the resulting clip/screen value was NaN or infinite
  behind_camera,  // clip.w < kMinClipW (at or behind the eye plane)
  degenerate,     // the viewport has no area (width or height <= 0)
};
const char *projection_status_name(ProjectionStatus status) noexcept;

struct ProjectionResult {
  ProjectionStatus status = ProjectionStatus::not_finite;
  ClipPosition clip;
  ScreenPosition screen;  // valid only when status == ok
};

// Builds the four columns from four raw columns (c0..c3, each four little-endian float bit patterns). No scaling is
// applied: the caller passes its matrix exactly as it wants it multiplied.
VertexColumns vertex_columns_from_raw(const std::array<std::array<std::uint32_t, 4>, 4> &raw_columns) noexcept;

// The factor the guest upload applies to the linear columns (func_0049E548): the Cmd float `value`, or 1 when it is
// exactly zero (the zero branch stores vf24..vf26 unscaled). Confirmed in the decompiled upload function and in the
// captured packet rows (docs/COREGS3-HOOKS.md §10.2). NaN propagates, so the draw is refused instead of guessed.
Float32 vu1_linear_upload_factor(Float32 cmd_value) noexcept;

// The linear scale from the RAW VU0 columns to the transform for METRE positions of one encoding:
//   integer_16 -> vu1_linear_upload_factor(cmd_value) * kModelPositionUnitsPerMetre
//   float_32   -> vu1_linear_upload_factor(cmd_value)
// The translation column is never scaled. This is the single place where the shape scale and the position encoding
// meet; no other module applies either.
Float32 metre_linear_scale(Float32 cmd_value, PositionEncoding encoding) noexcept;

// The raw VU0 vf24..vf27 block as the capture stores it (`vu0_vf[0..3]`), with NO upload scale: c0..c2 are the linear
// columns before the shape's `value`, c3 is the translation column. This is not a draw transform; it exists for
// diagnostics, fixtures and the raw-vs-packet comparison. A renderer uses the constructors below.
VertexColumns vertex_columns_from_vu0_raw(const std::uint32_t (&vu0_vf)[8][4]) noexcept;

// The 0xC1 matrix exactly as the game uploads it: c0..c2 = vf24..vf26 * vu1_linear_upload_factor(cmd_value), c3 = vf27.
VertexColumns vertex_columns_from_vu0_upload(const std::uint32_t (&vu0_vf)[8][4], Float32 cmd_value) noexcept;

// The columns to use with metre model positions of the given encoding:
//   c0..c2 = vf24..vf26 * metre_linear_scale(cmd_value, encoding),  c3 = vf27.
// A non-finite scale makes the linear columns non-finite, so the draw is refused rather than drawn with a guessed scale.
VertexColumns vertex_columns_from_vu0_metres(const std::uint32_t (&vu0_vf)[8][4], Float32 cmd_value,
                                            PositionEncoding encoding) noexcept;

// True when every one of the sixteen column values is finite. A draw whose columns fail this is refused, not drawn.
bool vertex_columns_finite(const VertexColumns &columns) noexcept;

// The measured transform: clip[i] = column[0][i]*x + column[1][i]*y + column[2][i]*z + column[3][i]. The result is
// computed even when it is not finite or lies behind the eye plane, so the caller can classify or clip it.
ClipPosition transform_model_position(const VertexColumns &columns, const ModelPosition &metres) noexcept;

// Classifies a clip position without projecting it. ok requires finite components and w >= kMinClipW.
ProjectionStatus classify_clip(const ClipPosition &clip) noexcept;

// Projects an already classified clip position. `flip_y` true is the measured GS convention (screen y grows
// downward); false is the mathematical y-up variant kept for diagnostics. On failure `screen` stays zero and must not
// be used.
ProjectionResult project_clip_position(const ClipPosition &clip, Float32 width, Float32 height,
                                       bool flip_y = true) noexcept;

// transform_model_position followed by project_clip_position. This is the reference vertex stage: `screen` is
// defined only for ProjectionStatus::ok.
ProjectionResult project_vertex(const VertexColumns &columns, const ModelPosition &metres, Float32 width, Float32 height,
                                bool flip_y = true) noexcept;

// DIAGNOSTICS ONLY. The VU1 adds the drawing offset to the screen coordinate and stores it as a signed 16-bit 12.4
// fixed-point value; an off-screen vertex wraps modulo 65536 units = 4096 px. This helper reconstructs what the GS
// would carry so a tool can explain the measured off-screen rows. The renderer never calls it.
struct GsWrapped12_4 {
  std::int32_t fixed_12_4 = 0;    // round(screen_px * 16 + offset_px * 16), before the 16-bit store
  std::uint16_t stored = 0;       // the low 16 bits the GS would carry
  Float32 signed_read_px = 0.0f;  // stored read as a signed 12.4 value (what the tap prints), /16
  Float32 unsigned_read_px = 0.0f;  // stored read as an unsigned 12.4 value, /16
};
GsWrapped12_4 diagnostics_gs_wrapped_12_4(Float32 screen_px, Float32 offset_px) noexcept;

}  // namespace gt4::coregs3
