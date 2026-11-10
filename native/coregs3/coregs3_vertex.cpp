// TASK-090 CoreGS 3.0: the measured VU1 vertex transform. See coregs3_vertex.hpp for the convention and the 16-bit
// wrap trap. The numbers behind the convention are measured in docs/COREGS3-HOOKS.md §10.2.
#include "coregs3_vertex.hpp"

#include <cmath>
#include <cstring>
#include <limits>

namespace gt4::coregs3 {
namespace {

Float32 as_float(std::uint32_t bits) noexcept {
  Float32 value = 0.0f;
  std::memcpy(&value, &bits, sizeof value);
  return value;
}

bool finite_position(const ModelPosition &position) noexcept {
  return std::isfinite(position[0]) && std::isfinite(position[1]) && std::isfinite(position[2]);
}

}  // namespace

const char *projection_status_name(ProjectionStatus status) noexcept {
  switch (status) {
  case ProjectionStatus::ok: return "ok";
  case ProjectionStatus::not_finite: return "not_finite";
  case ProjectionStatus::behind_camera: return "behind_camera";
  case ProjectionStatus::degenerate: return "degenerate";
  default: return "unknown";
  }
}

const char *position_encoding_name(PositionEncoding encoding) noexcept {
  switch (encoding) {
  case PositionEncoding::integer_16: return "integer_16";
  case PositionEncoding::float_32: return "float_32";
  case PositionEncoding::integer_32: return "integer_32";
  default: return "unknown";
  }
}

VertexRows vertex_rows(const VertexColumns &columns) noexcept {
  VertexRows rows;
  for (std::size_t row = 0; row < 4; ++row)
    for (std::size_t lane = 0; lane < 4; ++lane) rows[row][lane] = columns.column[lane][row];
  return rows;
}

VertexColumns vertex_columns_from_raw(const std::array<std::array<std::uint32_t, 4>, 4> &raw_columns) noexcept {
  VertexColumns columns;
  for (std::size_t column = 0; column < 4; ++column)
    for (std::size_t lane = 0; lane < 4; ++lane) columns.column[column][lane] = as_float(raw_columns[column][lane]);
  return columns;
}

Float32 vu1_linear_upload_factor(Float32 cmd_value) noexcept {
  // func_0049E548 branches on `scale != 0.0f`: the scaled branch stores vf24..vf26 * scale, the zero branch stores them
  // unscaled. IEEE comparison makes -0.0 equal to zero, and NaN unequal (so a NaN value propagates into the columns and
  // the draw is refused). Confirmed in docs/COREGS3-HOOKS.md §10.2 and in the captured packet rows.
  return cmd_value != 0.0f ? cmd_value : 1.0f;
}

Float32 metre_linear_scale(Float32 cmd_value, PositionEncoding encoding) noexcept {
  // integer_16 carries the model's 4096-units-per-metre divisor; the raw-lane integer_32 family and the stored floats
  // need no unit conversion (the world renderer multiplies the raw lane by the upload columns directly).
  const Float32 units_per_metre =
      encoding == PositionEncoding::integer_16 ? kModelPositionUnitsPerMetre : 1.0f;
  return vu1_linear_upload_factor(cmd_value) * units_per_metre;
}

VertexColumns vertex_columns_from_vu0_raw(const std::uint32_t (&vu0_vf)[8][4]) noexcept {
  // The capture stores vf24..vf31; the 0xC1 block is the first four of them (vf24..vf27), in column order.
  std::array<std::array<std::uint32_t, 4>, 4> raw{};
  for (std::size_t column = 0; column < 4; ++column)
    for (std::size_t lane = 0; lane < 4; ++lane) raw[column][lane] = vu0_vf[column][lane];
  return vertex_columns_from_raw(raw);
}

VertexColumns vertex_columns_from_vu0_upload(const std::uint32_t (&vu0_vf)[8][4], Float32 cmd_value) noexcept {
  VertexColumns columns = vertex_columns_from_vu0_raw(vu0_vf);
  const Float32 factor = vu1_linear_upload_factor(cmd_value);
  // Only the three linear columns are scaled; c3 = vf27 is the translation and the upload never scales it.
  for (std::size_t column = 0; column < 3; ++column)
    for (Float32 &value : columns.column[column]) value *= factor;
  return columns;
}

VertexColumns vertex_columns_from_vu0_metres(const std::uint32_t (&vu0_vf)[8][4], Float32 cmd_value,
                                            PositionEncoding encoding) noexcept {
  VertexColumns columns = vertex_columns_from_vu0_raw(vu0_vf);
  const Float32 scale = metre_linear_scale(cmd_value, encoding);
  // Built from the raw block in one multiply, exactly like the upload: vf * (factor * units). c3 stays vf27.
  for (std::size_t column = 0; column < 3; ++column)
    for (Float32 &value : columns.column[column]) value *= scale;
  return columns;
}

bool vertex_columns_finite(const VertexColumns &columns) noexcept {
  for (const std::array<Float32, 4> &column : columns.column)
    for (const Float32 value : column)
      if (!std::isfinite(value)) return false;
  return true;
}

ClipPosition transform_model_position(const VertexColumns &columns, const ModelPosition &metres) noexcept {
  // clip = c0*x + c1*y + c2*z + c3*1, componentwise. The columns carry every scale of the vertex stage (the shape's
  // `value` and the position encoding's units, see vertex_columns_from_vu0_metres); this function adds none.
  ClipPosition clip;
  clip.x = columns.column[0][0] * metres[0] + columns.column[1][0] * metres[1] + columns.column[2][0] * metres[2] +
           columns.column[3][0];
  clip.y = columns.column[0][1] * metres[0] + columns.column[1][1] * metres[1] + columns.column[2][1] * metres[2] +
           columns.column[3][1];
  clip.z = columns.column[0][2] * metres[0] + columns.column[1][2] * metres[1] + columns.column[2][2] * metres[2] +
           columns.column[3][2];
  clip.w = columns.column[0][3] * metres[0] + columns.column[1][3] * metres[1] + columns.column[2][3] * metres[2] +
           columns.column[3][3];
  return clip;
}

ProjectionStatus classify_clip(const ClipPosition &clip) noexcept {
  if (!std::isfinite(clip.x) || !std::isfinite(clip.y) || !std::isfinite(clip.z) || !std::isfinite(clip.w))
    return ProjectionStatus::not_finite;
  // The plane is w >= kMinClipW, not w > 0: a clipped vertex lands exactly on kMinClipW, and the guard keeps the
  // perspective divide finite for a vertex that is almost at the eye. kMinClipW is positive, so this still means the
  // projection is only defined in front of the camera.
  if (!(clip.w >= kMinClipW)) return ProjectionStatus::behind_camera;
  return ProjectionStatus::ok;
}

ProjectionResult project_clip_position(const ClipPosition &clip, Float32 width, Float32 height,
                                       bool flip_y) noexcept {
  ProjectionResult result;
  result.clip = clip;
  const ProjectionStatus status = classify_clip(clip);
  if (status != ProjectionStatus::ok) {
    result.status = status;
    return result;
  }
  if (!(width > 0.0f) || !(height > 0.0f)) {
    result.status = ProjectionStatus::degenerate;
    return result;
  }
  const Float32 inverse_w = 1.0f / clip.w;
  const Float32 ndc_x = clip.x * inverse_w;
  const Float32 ndc_y = clip.y * inverse_w;
  // The measured screen map (docs/COREGS3-HOOKS.md §10.2): x grows right, y grows down.
  result.screen.x = (ndc_x * 0.5f + 0.5f) * width;
  result.screen.y = flip_y ? (0.5f - ndc_y * 0.5f) * height : (ndc_y * 0.5f + 0.5f) * height;
  if (!std::isfinite(result.screen.x) || !std::isfinite(result.screen.y)) {
    result.screen = ScreenPosition{};
    result.status = ProjectionStatus::not_finite;
    return result;
  }
  result.status = ProjectionStatus::ok;
  return result;
}

ProjectionResult project_vertex(const VertexColumns &columns, const ModelPosition &metres, Float32 width,
                                Float32 height, bool flip_y) noexcept {
  ProjectionResult result;
  // Reject non-finite input explicitly instead of relying on NaN propagation through the multiply-add; the caller gets
  // one named status for both the matrix and the model position.
  if (!finite_position(metres) || !vertex_columns_finite(columns)) {
    result.clip = transform_model_position(columns, metres);
    result.status = ProjectionStatus::not_finite;
    return result;
  }
  result.clip = transform_model_position(columns, metres);
  return project_clip_position(result.clip, width, height, flip_y);
}

GsWrapped12_4 diagnostics_gs_wrapped_12_4(Float32 screen_px, Float32 offset_px) noexcept {
  GsWrapped12_4 wrap;
  // The VU1 adds the drawing offset in 12.4 units before the GS stores the coordinate. Use double for the conversion
  // so the diagnostic itself does not add float rounding to the wrapped value it is trying to explain.
  const double units = static_cast<double>(screen_px) * 16.0 + static_cast<double>(offset_px) * 16.0;
  const long long rounded = std::llround(units);
  if (rounded > std::numeric_limits<std::int32_t>::max())
    wrap.fixed_12_4 = std::numeric_limits<std::int32_t>::max();
  else if (rounded < std::numeric_limits<std::int32_t>::min())
    wrap.fixed_12_4 = std::numeric_limits<std::int32_t>::min();
  else
    wrap.fixed_12_4 = static_cast<std::int32_t>(rounded);
  // The 16-bit store keeps the low 16 bits; the signed 12.4 reading then jumps by 4096 px when the sign bit flips.
  wrap.stored = static_cast<std::uint16_t>(static_cast<std::uint32_t>(wrap.fixed_12_4) & 0xFFFFu);
  const std::int32_t signed_value =
      wrap.stored >= 0x8000u ? static_cast<std::int32_t>(wrap.stored) - 65536 : static_cast<std::int32_t>(wrap.stored);
  wrap.signed_read_px = static_cast<Float32>(signed_value) / 16.0f;
  wrap.unsigned_read_px = static_cast<Float32>(wrap.stored) / 16.0f;
  return wrap;
}

}  // namespace gt4::coregs3
