// TASK-090 CoreGS 3.0: the measured body/paint reflection. See coregs3_reflection.hpp for the formula and the subset.
#include "coregs3_reflection.hpp"

#include <cmath>
#include <cstring>

#include "coregs3_lighting.hpp"

namespace gt4::coregs3 {
namespace {

// The VU1 prologue's normal conversion (coregs3_lighting.cpp): the V3-8 byte times the microcode's 1/127 constant.
constexpr float kInverse127 = 1.0f / 127.0f;

float qword_lane(const std::uint32_t (&qword)[4], std::uint32_t lane) {
  float value = 0.0f;
  std::memcpy(&value, &qword[lane], sizeof value);
  return value;
}

std::uint32_t bits(std::uint64_t value, unsigned low, unsigned high) {
  const std::uint64_t mask = high - low == 63 ? ~0ull : ((1ull << (high - low + 1)) - 1ull);
  return static_cast<std::uint32_t>((value >> low) & mask);
}

}  // namespace

const char *reflection_status_name(ReflectionStatus status) noexcept {
  switch (status) {
    case ReflectionStatus::ok: return "ok";
    case ReflectionStatus::refused_block: return "refused_block";
    case ReflectionStatus::refused_position: return "refused_position";
    case ReflectionStatus::refused_normal: return "refused_normal";
    case ReflectionStatus::refused_non_finite: return "refused_non_finite";
    case ReflectionStatus::refused_degenerate: return "refused_degenerate";
  }
  return "unknown";
}

ReflectionStatus reflection_st(const CapturedItem &item, const MeshVertex &vertex, ReflectionCoordinate &out) noexcept {
  if (item.light_block_qwords < kBodyLightBlockQwords || item.light_block_command != kBodyLightBlockCommand)
    return ReflectionStatus::refused_block;
  if (!vertex.has_raw_position) return ReflectionStatus::refused_position;
  if (!vertex.has_normal) return ReflectionStatus::refused_normal;

  // The six qwords the fragment reads at TOPS+0xC9: q0..q2 are the texgen columns C9..CB, q3 is the light position LP.
  const std::array<float, 4> c9 = {qword_lane(item.light_block[0], 0), qword_lane(item.light_block[0], 1),
                                   qword_lane(item.light_block[0], 2), qword_lane(item.light_block[0], 3)};
  const std::array<float, 4> ca = {qword_lane(item.light_block[1], 0), qword_lane(item.light_block[1], 1),
                                   qword_lane(item.light_block[1], 2), qword_lane(item.light_block[1], 3)};
  const std::array<float, 4> cb = {qword_lane(item.light_block[2], 0), qword_lane(item.light_block[2], 1),
                                   qword_lane(item.light_block[2], 2), qword_lane(item.light_block[2], 3)};
  const std::array<float, 4> lp = {qword_lane(item.light_block[3], 0), qword_lane(item.light_block[3], 1),
                                   qword_lane(item.light_block[3], 2), qword_lane(item.light_block[3], 3)};

  // The exact float32 order of the study (validate_formula.py): every product and sum is written out so the operations
  // are the fragment's, not a reassociation of them.
  const float px = static_cast<float>(vertex.raw_position[0]);
  const float py = static_cast<float>(vertex.raw_position[1]);
  const float pz = static_cast<float>(vertex.raw_position[2]);
  const float nx = static_cast<float>(vertex.normal[0]) * kInverse127;
  const float ny = static_cast<float>(vertex.normal[1]) * kInverse127;
  const float nz = static_cast<float>(vertex.normal[2]) * kInverse127;
  if (!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(pz) || !std::isfinite(nx) || !std::isfinite(ny) ||
      !std::isfinite(nz))
    return ReflectionStatus::refused_non_finite;

  const float dx = px - lp[0];
  const float dy = py - lp[1];
  const float dz = pz - lp[2];
  const float c = (dx * nx + dy * ny) + dz * nz;
  // R = D - N*c - N*c, the fragment's two msubs.
  const float rx = (dx - nx * c) - nx * c;
  const float ry = (dy - ny * c) - ny * c;
  const float rz = (dz - nz * c) - nz * c;
  // V = (C9*R.x + CA*R.y) + CB*R.z.
  const float vx = (c9[0] * rx + ca[0] * ry) + cb[0] * rz;
  const float vy = (c9[1] * rx + ca[1] * ry) + cb[1] * rz;
  const float vz = (c9[2] * rx + ca[2] * ry) + cb[2] * rz;
  const float length = std::sqrt((vx * vx + vy * vy) + vz * vz);
  if (!std::isfinite(length)) return ReflectionStatus::refused_non_finite;

  // W = V/|V| + (0,0,1); q = 0.5/|W|. The study's formula divides componentwise; a zero length is not a coordinate.
  const float wx = vx / length;
  const float wy = vy / length;
  const float wz = vz / length + 1.0f;
  const float length_w = std::sqrt((wx * wx + wy * wy) + wz * wz);
  if (!std::isfinite(length_w)) return ReflectionStatus::refused_non_finite;
  if (!(length_w > 0.0f)) return ReflectionStatus::refused_degenerate;

  const float q = 0.5f / length_w;
  const float s = 0.5f + wx * q;
  const float t = 0.5f - wy * q;
  if (!std::isfinite(s) || !std::isfinite(t)) return ReflectionStatus::refused_non_finite;
  out.s = s;
  out.t = t;
  return ReflectionStatus::ok;
}

std::array<std::uint8_t, 4> apply_vertex_fog(const std::array<std::uint8_t, 4> &colour, std::uint32_t fog,
                                             std::uint32_t fog_colour) noexcept {
  const std::int32_t f = static_cast<std::int32_t>(fog > 255u ? 255u : fog);
  const std::int32_t fr = static_cast<std::int32_t>(fog_colour & 0xFFu);
  const std::int32_t fg = static_cast<std::int32_t>((fog_colour >> 8) & 0xFFu);
  const std::int32_t fb = static_cast<std::int32_t>((fog_colour >> 16) & 0xFFu);
  const auto mix = [f](std::int32_t c, std::int32_t fc) {
    const std::int32_t value = (f * c + (255 - f) * fc) >> 8;
    return static_cast<std::uint8_t>(value < 0 ? 0 : value > 255 ? 255 : value);
  };
  return {mix(colour[0], fr), mix(colour[1], fg), mix(colour[2], fb), colour[3]};
}

const char *reflection_binding_status_name(ReflectionBindingStatus status) noexcept {
  switch (status) {
    case ReflectionBindingStatus::ok: return "ok";
    case ReflectionBindingStatus::no_texture: return "no_texture";
    case ReflectionBindingStatus::refused_psm: return "refused_psm";
    case ReflectionBindingStatus::refused_tfx: return "refused_tfx";
    case ReflectionBindingStatus::refused_filter: return "refused_filter";
    case ReflectionBindingStatus::refused_mip: return "refused_mip";
    case ReflectionBindingStatus::refused_alpha_fail: return "refused_alpha_fail";
    case ReflectionBindingStatus::refused_blend: return "refused_blend";
    case ReflectionBindingStatus::refused_depth: return "refused_depth";
  }
  return "unknown";
}

ReflectionBindingStatus check_reflection_binding(const TextureBinding &binding) noexcept {
  if (binding.texture == nullptr) return ReflectionBindingStatus::no_texture;
  const SampledTexture &texture = *binding.texture;
  if (texture.psm != 0x14u && texture.psm != 0x13u && texture.psm != 0x00u)
    return ReflectionBindingStatus::refused_psm;
  if (texture.tfx != 0u) return ReflectionBindingStatus::refused_tfx;
  if (texture.mxl != 0u) return ReflectionBindingStatus::refused_mip;
  if (texture.mmag != texture.mmin || texture.mmag > 1u) return ReflectionBindingStatus::refused_filter;
  const SampleState &state = binding.state;
  if (bits(state.test, 0, 0) != 0u && bits(state.test, 12, 13) != 0u)
    return ReflectionBindingStatus::refused_alpha_fail;
  // The measured reflection pass is ABE on with the additive equation (0,2,1,1). Anything else is not reproduced.
  if (!prim_blend_enabled(state.prim) || blend_family(state.alpha) != BlendFamily::additive)
    return ReflectionBindingStatus::refused_blend;
  // The measured pass tests GEQUAL (2) against the reversed Z. Another compare is a refusal, not a guess.
  if (bits(state.test, 17, 18) != 2u) return ReflectionBindingStatus::refused_depth;
  return ReflectionBindingStatus::ok;
}

bool is_body_paint_item(const CapturedItem &item) noexcept {
  return light_program_of_item(item) == LightProgram::body_paint;
}

}  // namespace gt4::coregs3
