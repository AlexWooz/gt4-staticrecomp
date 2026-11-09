// TASK-090 CoreGS 3.0: the measured VU1 vertex transform, shared by the offline reference renderer (native/coregs3/
// coregs3_render.cpp) and, when it lands, the live renderer. The measurement and its evidence are
// docs/COREGS3-HOOKS.md §10.2; labels there follow docs/COREGS3-PLAN.md (H = High confidence).
//
// The convention:
//   * model positions are metres: the raw 16-bit model value divided by 4096, which is the scale build_mesh applies by
//     default (coregs3_shape.hpp). The division cancels the shape's `cmd.value` in the uploaded packet rows;
//   * the matrix the VU1 receives at qword 0xC1 is four COLUMNS, not rows: c0 = VU0 vf24, c1 = vf25, c2 = vf26 and
//     c3 = vf27, each four raw floats, used UNSCALED. The packet rows the game uploads are `vf * cmd.value`; mixing
//     that scale with the metre positions is the row reading that never validated on screen;
//   * clip = c0 * x + c1 * y + c2 * z + c3 * 1, componentwise, i.e. clip[i] = sum(j) column[j][i] * model[j];
//   * screen = ((0.5 + 0.5 * clip.x / clip.w) * width, (0.5 - 0.5 * clip.y / clip.w) * height). The captured night-race
//     frames are 640x448; the formula itself is resolution independent.
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

// The four columns of the 0xC1 matrix block, in order c0..c3 (= VU0 vf24..vf27). Each column is four raw floats, used
// unscaled. Built from the capture by `vertex_columns_from_vu0`; a hand-written matrix can be assembled directly.
struct VertexColumns {
  std::array<std::array<Float32, 4>, 4> column{};
};

// A model-space position in metres (the raw 16-bit model value / 4096).
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

// Builds the four columns from four captured raw columns (c0..c3, each four little-endian float bit patterns). No
// scaling is applied: the caller passes VU0 vf24..vf27 exactly as the capture recorded them.
VertexColumns vertex_columns_from_raw(const std::array<std::array<std::uint32_t, 4>, 4> &raw_columns) noexcept;

// The same for a CapturedDraw: `vu0_vf[0..3]` are vf24..vf27 (coregs3_scene.hpp / docs/COREGS3-HOOKS.md §10.2).
VertexColumns vertex_columns_from_vu0(const std::uint32_t (&vu0_vf)[8][4]) noexcept;

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
