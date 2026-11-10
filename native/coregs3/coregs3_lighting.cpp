// TASK-090 CoreGS 3.0: the measured VU1 lighting programs. See coregs3_lighting.hpp for the formulas, their inputs and
// the evidence. The body/paint formula is the round 12 module, unchanged; the per-item programs are round 14's measured
// formulas, decoded from the version 4 per-item light state.
#include "coregs3_lighting.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace gt4::coregs3 {
namespace {

// The VIF prologue at 0x1878 converts the V3-8 normal with ITOF0 and multiplies by this float (the microcode's r.i at
// 0x18A8). For sixteen of the 256 int8 values the multiply differs from a division by 127 in the last bit; the multiply
// is the fragment's exact operation and is used for every V3-8 normal.
constexpr float kInverse127 = 1.0f / 127.0f;

float qword_lane(const std::uint32_t (&qword)[4], std::uint32_t lane) {
  float value = 0.0f;
  std::memcpy(&value, &qword[lane], sizeof value);
  return value;
}

std::array<float, 4> qword_floats(const std::uint32_t (&qword)[4]) {
  return {qword_lane(qword, 0), qword_lane(qword, 1), qword_lane(qword, 2), qword_lane(qword, 3)};
}

// FTOI0 truncates toward zero. The result feeds an RGBA8 lane, so a value outside 0..255 saturates: the measured blocks
// cannot produce one (their constants bound the result), but a caller that passes an unmeasured block gets a defined
// colour instead of a wrapped byte.
std::uint8_t pack_lane(float value) noexcept {
  const float truncated = std::trunc(value);
  if (!(truncated > 0.0f)) return 0;
  if (truncated >= 255.0f) return 255;
  return static_cast<std::uint8_t>(truncated);
}

// The donor runtime's VU RSQRT: Q = s / sqrt(|t|) computed in double, then truncated to a 23-bit mantissa (fp::rsqrt /
// fp::from_double, private/references/ps2-web-recomp/runtime/include/ps2/fpu.hpp). The VU1 program calls it with s = 1,
// so this is 1/sqrt(value). It differs from 1.0f/sqrtf in the last bit, which matters for the eighth power below.
float vu_rsqrt(float value) noexcept {
  // The donor flushes a denormal input to zero (fp::flush), which makes RSQRT invalid; the smallest positive normal is
  // the last usable input.
  if (!(value >= std::numeric_limits<float>::min()) || !std::isfinite(value))
    return std::numeric_limits<float>::infinity();
  const double quotient = 1.0 / std::sqrt(static_cast<double>(value));
  std::uint64_t bits = 0;
  std::memcpy(&bits, &quotient, sizeof bits);
  bits &= ~static_cast<std::uint64_t>(0x1FFFFFFFu);  // keep the top 23 mantissa bits (fp::from_double's >> 29)
  double truncated = 0.0;
  std::memcpy(&truncated, &bits, sizeof truncated);
  return static_cast<float>(truncated);
}

// The normal the nine-qword families read at TOPS+0x80: the V3-8 bytes after the 0x1878 prologue's ITOF0 * (1/127).
std::array<float, 3> nine_normal(const MeshVertex &vertex) noexcept {
  return {static_cast<float>(vertex.normal80[0]) * kInverse127,
          static_cast<float>(vertex.normal80[1]) * kInverse127,
          static_cast<float>(vertex.normal80[2]) * kInverse127};
}

// L_j = max(dot((q0[j], q1[j], q2[j]), N), 0), summed in the fragment's order: (q0*nx + q1*ny) + q2*nz.
std::array<float, 4> nine_directions(const ItemLightState &state, const std::array<float, 3> &normal) noexcept {
  std::array<float, 4> directions{};
  for (std::uint32_t lane = 0; lane < 4; ++lane) {
    const float dot = (state.qword[0][lane] * normal[0] + state.qword[1][lane] * normal[1]) +
                      state.qword[2][lane] * normal[2];
    directions[lane] = std::max(dot, 0.0f);
  }
  return directions;
}

// q3 + q4*L0 + q5*L1 + q6*L2^power + q7*L3^power, clamped to [0, q8], per lane. power is 1, 8 or 16; the powers are the
// repeated squarings the fragment performs on the clamped direction before the coefficient sum.
bool nine_sum(const ItemLightState &state, const std::array<float, 4> &directions, std::uint32_t power,
              std::array<float, 4> &result) noexcept {
  std::array<float, 4> weighted{};
  for (std::uint32_t lane = 0; lane < 4; ++lane) {
    float power2 = directions[2];
    float power3 = directions[3];
    for (std::uint32_t squaring = 1; squaring < power; squaring *= 2) {
      power2 = power2 * power2;
      power3 = power3 * power3;
    }
    float sum = state.qword[3][lane];
    sum = sum + state.qword[4][lane] * directions[0];
    sum = sum + state.qword[5][lane] * directions[1];
    sum = sum + state.qword[6][lane] * power2;
    sum = sum + state.qword[7][lane] * power3;
    if (!std::isfinite(sum)) return false;
    sum = std::max(sum, 0.0f);
    sum = std::min(sum, state.qword[8][lane]);
    weighted[lane] = sum;
  }
  result = weighted;
  return true;
}

// Body/paint: the round 12 formula, fed from the item's block qwords.
LightStatus shade_body(const ItemLightState &state, const MeshVertex &vertex,
                       std::array<std::uint8_t, 4> &rgba) noexcept {
  if (!vertex.has_normal || !vertex.has_raw_position || !vertex.has_color) return LightStatus::missing_channel;
  LightBlock block;
  for (std::uint32_t row = 0; row < 3; ++row)
    for (std::uint32_t lane = 0; lane < 4; ++lane) block.matrix[row][lane] = state.qword[row][lane];
  block.position = state.qword[3];
  block.c3 = state.qword[4];
  block.c4 = state.qword[5];
  return shade_vertex(block, vertex, rgba) ? LightStatus::ok : LightStatus::invalid_math;
}

// nine_qword_first / pow8 / pow16 / direct.
LightStatus shade_nine(const ItemLightState &state, const MeshVertex &vertex, bool direct, std::uint32_t power,
                       std::array<std::uint8_t, 4> &rgba) noexcept {
  std::array<float, 3> normal{};
  if (direct) {
    if (!vertex.has_normal80_float) return LightStatus::missing_channel;
    normal = vertex.normal80_float;
  } else {
    if (!vertex.has_normal80) return LightStatus::missing_channel;
    normal = nine_normal(vertex);
  }
  for (const float value : normal)
    if (!std::isfinite(value)) return LightStatus::invalid_math;
  const std::array<float, 4> directions = nine_directions(state, normal);
  std::array<float, 4> sum{};
  if (!nine_sum(state, directions, power, sum)) return LightStatus::invalid_math;
  for (std::uint32_t lane = 0; lane < 4; ++lane) rgba[lane] = pack_lane(sum[lane]);
  return LightStatus::ok;
}

// three_qword: out = clamp(bias + gain * colour, 0, clamp) per lane; lane 3 is the colour's alpha.
LightStatus shade_three(const ItemLightState &state, const MeshVertex &vertex,
                        std::array<std::uint8_t, 4> &rgba) noexcept {
  if (!vertex.has_color) return LightStatus::missing_channel;
  std::array<std::uint8_t, 4> lit{};
  for (std::uint32_t lane = 0; lane < 4; ++lane) {
    const float colour = static_cast<float>(vertex.color[lane]);
    float sum = state.qword[2][lane] + state.qword[0][lane] * colour;
    if (!std::isfinite(sum)) return LightStatus::invalid_math;
    sum = std::max(sum, 0.0f);
    sum = std::min(sum, state.qword[1][lane]);
    lit[lane] = pack_lane(sum);
  }
  rgba = lit;
  return LightStatus::ok;
}

// six_qword_no_colour: the Phong-style specular, with the signed t = R*q, the weighted sum, one max after the sum, and
// the eighth power of the summed x component. Alpha is the fragment's constant 128.
LightStatus shade_six(const ItemLightState &state, const MeshVertex &vertex,
                      std::array<std::uint8_t, 4> &rgba) noexcept {
  if (!vertex.has_normal80 || !vertex.has_raw_position) return LightStatus::missing_channel;
  const std::array<float, 3> normal = nine_normal(vertex);
  const float position[3] = {static_cast<float>(vertex.raw_position[0]), static_cast<float>(vertex.raw_position[1]),
                             static_cast<float>(vertex.raw_position[2])};
  float delta[3];
  for (std::uint32_t axis = 0; axis < 3; ++axis) delta[axis] = position[axis] - state.qword[3][axis];
  const float length_squared = (delta[0] * delta[0] + delta[1] * delta[1]) + delta[2] * delta[2];
  if (!(length_squared > 0.0f) || !std::isfinite(length_squared)) return LightStatus::invalid_math;
  // n1 = D * N + D * N = 2 * fl(D*N), exactly as the fragment's MADD pair; R subtracts N * n1[i] for i = 0, 1, 2.
  float doubled[3];
  for (std::uint32_t axis = 0; axis < 3; ++axis) doubled[axis] = delta[axis] * normal[axis] + delta[axis] * normal[axis];
  float reflected[3];
  for (std::uint32_t axis = 0; axis < 3; ++axis) {
    float value = delta[axis];
    value = value - normal[axis] * doubled[0];
    value = value - normal[axis] * doubled[1];
    value = value - normal[axis] * doubled[2];
    reflected[axis] = value;
  }
  const float inverse_length = vu_rsqrt(length_squared);
  if (!std::isfinite(inverse_length)) return LightStatus::invalid_math;
  float t[3];
  for (std::uint32_t axis = 0; axis < 3; ++axis) t[axis] = reflected[axis] * inverse_length;
  float summed[3];
  for (std::uint32_t lane = 0; lane < 3; ++lane) {
    float value = state.qword[0][lane] * t[0];
    value = value + state.qword[1][lane] * t[1];
    value = value + state.qword[2][lane] * t[2];
    if (!std::isfinite(value)) return LightStatus::invalid_math;
    summed[lane] = std::max(value, 0.0f);
  }
  float scalar = summed[0];
  scalar = scalar * scalar;  // L^2
  scalar = scalar * scalar;  // L^4
  scalar = scalar * scalar;  // L^8
  std::array<std::uint8_t, 4> lit{};
  for (std::uint32_t lane = 0; lane < 3; ++lane) {
    const float value = state.qword[4][lane] * scalar;
    if (!std::isfinite(value)) return LightStatus::invalid_math;
    lit[lane] = pack_lane(value);
  }
  lit[3] = 128;
  rgba = lit;
  return LightStatus::ok;
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
  // N = normal * (1/127): the model stores a unit normal scaled to the int8 byte range, and the 0x1908 prologue
  // multiplies the ITOF0 value by the microcode's 1/127 constant. For the int8 range this is the same float as a
  // division except for sixteen byte values, where the multiply is the fragment's exact operation.
  const float nx = static_cast<float>(normal[0]) * kInverse127;
  const float ny = static_cast<float>(normal[1]) * kInverse127;
  const float nz = static_cast<float>(normal[2]) * kInverse127;
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

const char *light_program_name(LightProgram program) noexcept {
  switch (program) {
  case LightProgram::none: return "none";
  case LightProgram::body_paint: return "body_paint";
  case LightProgram::nine_qword_first: return "nine_qword_first";
  case LightProgram::nine_qword_pow8: return "nine_qword_pow8";
  case LightProgram::nine_qword_pow16: return "nine_qword_pow16";
  case LightProgram::nine_qword_direct: return "nine_qword_direct";
  case LightProgram::six_qword_no_colour: return "six_qword_no_colour";
  case LightProgram::three_qword_gain: return "three_qword_gain";
  case LightProgram::passthrough: return "passthrough";
  default: return "unknown";
  }
}

std::uint32_t item_dispatch(const CapturedItem &item) noexcept {
  if (item.run_valid == 0) return 0;
  return (item.run_itop & 0x3FFu) * 8u;
}

LightProgram light_program_from_dispatch(std::uint32_t dispatch) noexcept {
  switch (dispatch) {
  case 0x1058u: return LightProgram::body_paint;
  case 0x0B08u: return LightProgram::nine_qword_first;
  case 0x0250u: return LightProgram::nine_qword_pow8;
  case 0x03D8u: return LightProgram::nine_qword_pow16;
  case 0x0418u: return LightProgram::nine_qword_direct;
  case 0x0C68u: return LightProgram::six_qword_no_colour;
  case 0x14D8u: return LightProgram::three_qword_gain;
  case 0x0080u: return LightProgram::passthrough;
  default: return LightProgram::none;
  }
}

LightProgram light_program_of_item(const CapturedItem &item) noexcept {
  return light_program_from_dispatch(item_dispatch(item));
}

const char *light_status_name(LightStatus status) noexcept {
  switch (status) {
  case LightStatus::ok: return "ok";
  case LightStatus::no_program: return "no_program";
  case LightStatus::no_block: return "no_block";
  case LightStatus::uncertain_block: return "uncertain_block";
  case LightStatus::block_variant: return "block_variant";
  case LightStatus::block_not_finite: return "block_not_finite";
  case LightStatus::missing_channel: return "missing_channel";
  case LightStatus::invalid_math: return "invalid_math";
  default: return "unknown";
  }
}

LightStatus decode_item_light(const CapturedItem &item, ItemLightState &state) noexcept {
  state = ItemLightState{};
  if (item.run_valid == 0) return LightStatus::no_program;
  const LightProgram program = light_program_from_dispatch(item_dispatch(item));
  state.program = program;
  if (program == LightProgram::none) return LightStatus::no_program;
  // The passthrough program reads no block: it emits the V4-8 colour unchanged, so its light_source is irrelevant.
  if (program == LightProgram::passthrough) return LightStatus::ok;
  if (item.light_source == kLightItemNone) return LightStatus::no_block;
  if (item.light_source == kLightItemCarriedUncertain) return LightStatus::uncertain_block;
  if (item.light_source != kLightItemUploaded && item.light_source != kLightItemCarried) return LightStatus::no_block;
  std::uint32_t expected_qwords = 0;
  std::uint32_t expected_command = 0;
  switch (program) {
  case LightProgram::body_paint:
  case LightProgram::six_qword_no_colour:
    expected_qwords = kBodyPaintLightBlockQwords;
    expected_command = kBodyPaintLightBlockCommand;
    break;
  case LightProgram::three_qword_gain:
    expected_qwords = kThreeQwordLightBlockQwords;
    expected_command = kThreeQwordLightBlockCommand;
    break;
  default:
    expected_qwords = kNineQwordLightBlockQwords;
    expected_command = kNineQwordLightBlockCommand;
    break;
  }
  if (item.light_block_qwords != expected_qwords) return LightStatus::block_variant;
  // An uploaded block carries its own tag word, which must be the measured form. A carried block has no tag of its own:
  // the capture keeps the tracked upload's command when it knows it and writes zero otherwise, so the qword count is
  // the variant and a known command must still match.
  if (item.light_source == kLightItemUploaded) {
    if (item.light_block_command != expected_command) return LightStatus::block_variant;
  } else if (item.light_block_command != 0 && item.light_block_command != expected_command) {
    return LightStatus::block_variant;
  }
  for (std::uint32_t qword = 0; qword < expected_qwords; ++qword)
    for (std::uint32_t lane = 0; lane < 4; ++lane) {
      const float value = qword_lane(item.light_block[qword], lane);
      if (!std::isfinite(value)) return LightStatus::block_not_finite;
      state.qword[qword][lane] = value;
    }
  return LightStatus::ok;
}

LightStatus shade_item_vertex(const ItemLightState &state, const MeshVertex &vertex,
                              std::array<std::uint8_t, 4> &rgba) noexcept {
  switch (state.program) {
  case LightProgram::body_paint: return shade_body(state, vertex, rgba);
  case LightProgram::nine_qword_first: return shade_nine(state, vertex, false, 1u, rgba);
  case LightProgram::nine_qword_pow8: return shade_nine(state, vertex, false, 8u, rgba);
  case LightProgram::nine_qword_pow16: return shade_nine(state, vertex, false, 16u, rgba);
  case LightProgram::nine_qword_direct: return shade_nine(state, vertex, true, 16u, rgba);
  case LightProgram::six_qword_no_colour: return shade_six(state, vertex, rgba);
  case LightProgram::three_qword_gain: return shade_three(state, vertex, rgba);
  case LightProgram::passthrough:
    if (!vertex.has_color) return LightStatus::missing_channel;
    rgba = vertex.color;
    return LightStatus::ok;
  default: return LightStatus::no_program;
  }
}

void LightCounters::add(const LightCounters &other) noexcept {
  for (std::uint32_t index = 0; index < kLightProgramCount; ++index) {
    family[index].items += other.family[index].items;
    family[index].items_lit += other.family[index].items_lit;
    family[index].items_fallback += other.family[index].items_fallback;
    family[index].vertices_lit += other.family[index].vertices_lit;
    family[index].vertices_fallback += other.family[index].vertices_fallback;
    family[index].draws_lit += other.family[index].draws_lit;
    family[index].draws_fallback += other.family[index].draws_fallback;
  }
  for (std::uint32_t index = 0; index < kLightStatusCount; ++index) status[index] += other.status[index];
  draws_lit += other.draws_lit;
  draws_fallback += other.draws_fallback;
  vertices_lit += other.vertices_lit;
  vertices_fallback += other.vertices_fallback;
  items_uploaded += other.items_uploaded;
  items_carried += other.items_carried;
  items_uncertain += other.items_uncertain;
  items_none += other.items_none;
}

void shade_mesh_items(const CapturedDraw &draw, Mesh &mesh, LightCounters &counters,
                      std::vector<ShadedVertex> *shades) {
  const std::uint32_t vertex_count = static_cast<std::uint32_t>(mesh.vertices.size());
  if (shades != nullptr) shades->assign(vertex_count, ShadedVertex{});
  std::vector<ItemLightState> states(draw.item_count < kMaxItemsPerDraw ? draw.item_count : kMaxItemsPerDraw);
  std::vector<LightStatus> prepared(states.size(), LightStatus::no_program);
  for (std::uint32_t item = 0; item < states.size(); ++item) prepared[item] = decode_item_light(draw.items[item], states[item]);

  struct ItemTally {
    std::uint64_t vertices = 0;
    std::uint64_t lit = 0;
    std::uint64_t fallback = 0;
    LightProgram program = LightProgram::none;
    LightStatus prepared = LightStatus::no_program;
    bool used = false;
  };
  std::vector<ItemTally> tally(states.size());
  std::array<bool, kLightProgramCount> draw_lit{};
  std::array<bool, kLightProgramCount> draw_fallback{};
  bool any_lit = false;

  for (std::uint32_t index = 0; index < vertex_count; ++index) {
    MeshVertex &vertex = mesh.vertices[index];
    const std::uint32_t item = vertex.item;
    ItemTally *record = item < tally.size() ? &tally[item] : nullptr;
    LightProgram program = LightProgram::none;
    LightStatus status = LightStatus::no_program;
    if (record != nullptr) {
      record->used = true;
      record->program = states[item].program;
      record->vertices += 1;
      program = states[item].program;
      status = prepared[item];
    }
    std::array<std::uint8_t, 4> rgba{};
    if (status == LightStatus::ok) status = shade_item_vertex(states[item], vertex, rgba);
    if (status == LightStatus::ok) {
      vertex.color = rgba;
      counters.vertices_lit += 1;
      counters.family[static_cast<std::uint32_t>(program)].vertices_lit += 1;
      if (record != nullptr) record->lit += 1;
      draw_lit[static_cast<std::uint32_t>(program)] = true;
      any_lit = true;
    } else {
      counters.vertices_fallback += 1;
      counters.family[static_cast<std::uint32_t>(program)].vertices_fallback += 1;
      if (record != nullptr) record->fallback += 1;
      draw_fallback[static_cast<std::uint32_t>(program)] = true;
    }
    counters.status[static_cast<std::uint32_t>(status)] += 1;
    if (shades != nullptr) (*shades)[index] = ShadedVertex{program, status};
  }

  for (const ItemTally &record : tally) {
    if (!record.used) continue;
    LightFamilyCounts &family = counters.family[static_cast<std::uint32_t>(record.program)];
    family.items += 1;
    if (record.lit != 0 && record.fallback == 0) family.items_lit += 1;
    if (record.fallback != 0) family.items_fallback += 1;
  }
  for (std::uint32_t program = 0; program < kLightProgramCount; ++program) {
    if (draw_lit[program]) counters.family[program].draws_lit += 1;
    if (draw_fallback[program]) counters.family[program].draws_fallback += 1;
  }
  if (any_lit)
    counters.draws_lit += 1;
  else
    counters.draws_fallback += 1;
  // The item light_source tally (one per item represented in the mesh).
  for (std::uint32_t item = 0; item < tally.size(); ++item) {
    if (!tally[item].used) continue;
    switch (draw.items[item].light_source) {
    case kLightItemUploaded: counters.items_uploaded += 1; break;
    case kLightItemCarried: counters.items_carried += 1; break;
    case kLightItemCarriedUncertain: counters.items_uncertain += 1; break;
    default: counters.items_none += 1; break;
    }
  }
}

}  // namespace gt4::coregs3
