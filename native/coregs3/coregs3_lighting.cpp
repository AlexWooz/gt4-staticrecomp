// TASK-090 CoreGS 3.0: the measured VU1 lighting formula. See coregs3_lighting.hpp for the units, constants, operation
// order, the role of M and the evidence.
#include "coregs3_lighting.hpp"

#include "coregs3_scene.hpp"
#include "coregs3_shape.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gt4::coregs3 {
namespace {

float qword_lane(const std::uint32_t (&qword)[4], std::uint32_t lane) {
  float value = 0.0f;
  std::memcpy(&value, &qword[lane], sizeof value);
  return value;
}

std::array<float, 4> qword_floats(const std::uint32_t (&qword)[4]) {
  return {qword_lane(qword, 0), qword_lane(qword, 1), qword_lane(qword, 2), qword_lane(qword, 3)};
}

// FTOI0 truncates toward zero. The result feeds an RGBA8 lane, so a value outside 0..255 saturates: the measured block
// cannot produce one (C3 = 1.0 and C4 = 0.8 bound the factor to 0.2..1 before the base is applied), but a caller that
// passes an unmeasured block gets a defined colour instead of a wrapped byte.
std::uint8_t pack_lane(float value) noexcept {
  const float truncated = std::trunc(value);
  if (!(truncated > 0.0f)) return 0;
  if (truncated >= 255.0f) return 255;
  return static_cast<std::uint8_t>(truncated);
}

}  // namespace

const char *light_block_status_name(LightBlockStatus status) noexcept {
  switch (status) {
  case LightBlockStatus::ok: return "ok";
  case LightBlockStatus::missing: return "missing";
  case LightBlockStatus::unknown_variant: return "unknown_variant";
  case LightBlockStatus::not_finite: return "not_finite";
  default: return "unknown";
  }
}

LightBlockStatus decode_light_block(const CapturedDraw &draw, LightBlock &block) noexcept {
  if (draw.light_block_qwords == 0) return LightBlockStatus::missing;
  if (draw.light_block_qwords != kBodyPaintLightBlockQwords ||
      draw.light_block_command != kBodyPaintLightBlockCommand)
    return LightBlockStatus::unknown_variant;
  LightBlock decoded;
  for (std::uint32_t row = 0; row < 3; ++row) decoded.matrix[row] = qword_floats(draw.light_block[row]);
  decoded.position = qword_floats(draw.light_block[3]);
  decoded.c3 = qword_floats(draw.light_block[4]);
  decoded.c4 = qword_floats(draw.light_block[5]);
  for (const std::array<float, 4> *qword : {&decoded.matrix[0], &decoded.matrix[1], &decoded.matrix[2],
                                            &decoded.position, &decoded.c3, &decoded.c4})
    for (const float value : *qword)
      if (!std::isfinite(value)) return LightBlockStatus::not_finite;
  block = decoded;
  return LightBlockStatus::ok;
}

bool shade_vertex(const LightBlock &block, const std::array<std::int8_t, 3> &normal,
                  const std::array<std::int32_t, 3> &raw_position, const std::array<std::uint8_t, 4> &base,
                  std::array<std::uint8_t, 4> &rgba) noexcept {
  // N = normal / 127: the model stores a unit normal scaled to the int8 byte range (the fragment's normal bank).
  const float nx = static_cast<float>(normal[0]) / 127.0f;
  const float ny = static_cast<float>(normal[1]) / 127.0f;
  const float nz = static_cast<float>(normal[2]) / 127.0f;
  // P is the raw signed integer after ITOF0; LP is in the same raw model unit space.
  const float dx = static_cast<float>(raw_position[0]) - block.position[0];
  const float dy = static_cast<float>(raw_position[1]) - block.position[1];
  const float dz = static_cast<float>(raw_position[2]) - block.position[2];
  const float length_squared = dx * dx + dy * dy + dz * dz;
  if (!(length_squared > 0.0f) || !std::isfinite(length_squared)) return false;
  // The fragment sums the three products in this order (lane 0 grows: x*y, +y*y, +z*z) with one multiply and one add
  // each; this build uses -ffp-contract=off, so the expression keeps the same two roundings.
  const float cosine = (dx * nx + dy * ny) + dz * nz;
  const float inverse_length = 1.0f / std::sqrt(length_squared);
  const float t = std::min(cosine, 0.0f) * inverse_length;
  std::array<std::uint8_t, 4> lit{};
  for (std::uint32_t lane = 0; lane < 4; ++lane) {
    const float factor = block.c3[lane] + block.c4[lane] * t;
    const float scaled = static_cast<float>(base[lane]) * factor;
    if (!std::isfinite(scaled)) return false;
    lit[lane] = pack_lane(scaled);
  }
  rgba = lit;
  return true;
}

bool shade_vertex(const LightBlock &block, const MeshVertex &vertex, std::array<std::uint8_t, 4> &rgba) noexcept {
  if (!vertex.has_normal || !vertex.has_raw_position) return false;
  return shade_vertex(block, vertex.normal, vertex.raw_position, vertex.color, rgba);
}

}  // namespace gt4::coregs3
